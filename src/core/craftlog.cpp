#include "craftlog.hpp"

#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::craftlog {
namespace {

constexpr std::size_t kHeader = 0x1C; // a buffer's header; its packets follow
constexpr std::uint16_t kZoneIn = 0x00A, kUseItem = 0x037, kInventoryModify = 0x01E, kInventoryAssign = 0x01F,
                        kInventoryItem = 0x020, kSynthAnswer = 0x06F, kSynthResult = 0x070, kSynthStart = 0x096;
constexpr int kMostLines = 300, kMostUses = 40, kMostZones = 30, kMostAnswers = 12;
constexpr ULONGLONG kWindow = 5000; // ms after a synthesis or a crystal used that what comes back is logged

// The game's "no item can be used now" test (FUN_10097b60 in the Ghidra export of the 2025-12-26 FFXiMain.dll): the Items
// menu's Use/Discard window greys Use out for every item, crystals and clusters too, while it is true, and a greyed Use
// takes no pick. True when the character's data (the pointer in the first global) has a non-zero word at the offset in
// its second instruction, or the mode byte (the second global) holds the value compared with:
//   mov eax,[player]; test eax,eax; jz; cmp word ptr [eax+flag],0; jz; mov al,1; ret; cmp byte ptr [mode],17h; setz al; ret
constexpr char kUseGate[] = "A1 ?? ?? ?? ?? 85 C0 74 0D 66 83 B8 ?? ?? ?? ?? 00 74 03 B0 01 C3 80 3D ?? ?? ?? ?? ?? 0F 94 C0 C3";
constexpr std::ptrdiff_t kGatePlayerAt = 1, kGateFlagAt = 12, kGateModeAt = 24, kGateModeValueAt = 28;
constexpr ULONGLONG kGateWatch = 15 * 60 * 1000; // ms the test is watched after a crystal, a cluster or a synthesis
constexpr ULONGLONG kGateEvery = 250;            // ms between looks at it
constexpr int kMostGateLines = 40;

// The item ids of the eight crystals (4096-4103) and the eight clusters (4104-4111)
constexpr const char* kNames[16] = {"Fire Crystal",    "Ice Crystal",   "Wind Crystal",   "Earth Crystal",
                                    "Lightning Crystal", "Water Crystal", "Light Crystal",  "Dark Crystal",
                                    "Fire Cluster",    "Ice Cluster",   "Wind Cluster",   "Earth Cluster",
                                    "Lightning Cluster", "Water Cluster", "Light Cluster",  "Dark Cluster"};

struct Slot {
    std::uint16_t item = 0;
    std::uint32_t qty = 0;
    bool known = false; // the server has said what's here
};

std::mutex g_lock;
Slot g_slots[16][256]; // [bag][slot], as the server last described them
int g_lines = 0, g_uses = 0, g_zones = 0;
bool g_first = true;
ULONGLONG g_window_until = 0, g_window_from = 0;
bool g_seen[512] = {};
int g_answers = 0;

struct Gate {
    const std::uint8_t* const* player = nullptr; // the global holding the character's data
    std::uint32_t flag_at = 0;                   // the word in it that disables Use
    const std::uint8_t* mode = nullptr;          // the mode byte's global
    std::uint8_t mode_value = 0;                 // what in it disables Use
};
Gate g_gate;
bool g_gate_searched = false, g_gate_found = false;
bool g_gate_known = false, g_gate_on = false; // a look at it was taken since the watch began, and what it said
ULONGLONG g_gate_until = 0, g_gate_next = 0;
int g_gate_lines = 0;

std::uint16_t u16(const std::uint8_t* p)
{
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::uint32_t u32(const std::uint8_t* p)
{
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

bool crystalish(std::uint16_t id)
{
    return id >= 4096 && id <= 4111;
}

std::string format(const char* fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    return text;
}

std::string item_text(std::uint16_t id)
{
    if (id == 0)
        return "nothing";
    if (crystalish(id))
        return format("%s (%u)", kNames[id - 4096], id);
    return format("item %u", id);
}

std::string slot_text(const Slot& s)
{
    return s.known ? item_text(s.item) + format(" x%u", s.qty) : std::string("not described by the server");
}

// A line, within the session's cap
void say(const std::string& text)
{
    if (g_lines >= kMostLines) {
        if (g_lines == kMostLines) {
            ++g_lines;
            log::write(L"Craft: %d lines logged this session; the rest are left out", kMostLines);
        }
        return;
    }
    ++g_lines;
    log::write(L"Craft: %hs", text.c_str());
}

// The item-use test's globals found in FFXiMain.dll (once, when something first asks)
bool find_gate()
{
    if (g_gate_searched)
        return g_gate_found;
    g_gate_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kUseGate) : 0;
    if (at == 0) {
        say("the game's item-use test wasn't found in FFXiMain.dll: it isn't watched");
        return false;
    }
    const auto* code = reinterpret_cast<const std::uint8_t*>(at);
    g_gate.player = reinterpret_cast<const std::uint8_t* const*>(static_cast<std::uintptr_t>(u32(code + kGatePlayerAt)));
    g_gate.flag_at = u32(code + kGateFlagAt);
    g_gate.mode = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(u32(code + kGateModeAt)));
    g_gate.mode_value = code[kGateModeValueAt];
    g_gate_found = true;
    return true;
}

struct GateLook {
    bool ok = false;
    std::uint16_t word = 0;
    std::uint8_t mode = 0;
};

// The test's two inputs, read the way the game does (the character's data may not be there yet)
GateLook look_at_gate()
{
    GateLook look;
    __try {
        const std::uint8_t* player = *g_gate.player;
        if (player != nullptr)
            std::memcpy(&look.word, player + g_gate.flag_at, sizeof look.word);
        look.mode = *g_gate.mode;
        look.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return look;
}

// A crystal, a cluster or a synthesis: the item-use test is watched for the next while (and looked at at once)
void arm_gate(ULONGLONG now)
{
    if (now >= g_gate_until)
        g_gate_known = false; // a watch begins: the first look says how things stand
    g_gate_until = now + kGateWatch;
    g_gate_next = 0;
}

// While watched, the test looked at every kGateEvery ms and each change of it logged: if "Use" stays greyed out in the
// Items menu after a cluster until the next zone, this says when it went on and what turned it off
void watch_gate(ULONGLONG now)
{
    if (now >= g_gate_until || now < g_gate_next || g_gate_lines >= kMostGateLines)
        return;
    g_gate_next = now + kGateEvery;
    if (!find_gate())
        return;
    const GateLook look = look_at_gate();
    if (!look.ok)
        return;
    const bool on = look.word != 0 || look.mode == g_gate.mode_value;
    if (g_gate_known && on == g_gate_on)
        return;
    ++g_gate_lines;
    say(format("the game's item-use test is %s (%s): the word at player+0x%X is 0x%04X, the mode byte %u (it greys out Use in the "
               "Items menu at %u)",
               on ? "ON" : "off", g_gate_known ? "changed" : "first look", g_gate.flag_at, look.word, look.mode, g_gate.mode_value));
    g_gate_known = true;
    g_gate_on = on;
}

void open_window(ULONGLONG now)
{
    g_window_from = now;
    g_window_until = now + kWindow;
    std::memset(g_seen, 0, sizeof g_seen);
    g_answers = 0;
    arm_gate(now);
}

std::string hex(const std::uint8_t* p, std::size_t n)
{
    std::string s;
    for (std::size_t i = 0; i < n; ++i)
        s += format(i == 0 ? "%02X" : " %02X", p[i]);
    return s;
}

// What the server last said slot `slot` of bag `bag` holds, for a line about an id sent
std::string check(unsigned bag, unsigned slot, std::uint16_t sent)
{
    if (bag >= 16 || slot >= 256)
        return " [slot out of range]";
    const Slot& s = g_slots[bag][slot];
    if (!s.known)
        return " [! the server never described that slot]";
    if (s.item != sent)
        return " [! the server has " + slot_text(s) + " there]";
    return " [" + slot_text(s) + "]";
}

// A change to a slot, logged when a crystal or cluster is in it, before or after
void change(unsigned bag, unsigned slot, bool item_known, std::uint16_t item, std::uint32_t qty, const char* by)
{
    if (bag >= 16 || slot >= 256)
        return;
    Slot& s = g_slots[bag][slot];
    const Slot was = s;
    if (item_known) {
        s.item = item;
        s.known = true;
    }
    s.qty = qty;
    const bool wanted = (was.known && crystalish(was.item)) || (s.known && crystalish(s.item));
    if (!wanted)
        return;
    say(format("bag %u slot %u (%s): %s%s", bag, slot, by, slot_text(s).c_str(),
               was.known ? (" (was " + slot_text(was) + ")").c_str() : " (the server hadn't described it)"));
}

bool deny(std::uint16_t id)
{
    // what the server sends all the time: positions, the entities, our own status, actions, the party
    switch (id) {
    case 0x00D: case 0x00E: case 0x037: case 0x067: case 0x063: case 0x0DD: case 0x0DF: case 0x028: case 0x01A: case 0x008:
    case 0x0E0: case 0x076: case 0x061: case 0x0C8: case 0x05E: case 0x0F4: case 0x0F5: case 0x01B:
        return true;
    default:
        return false;
    }
}

void received(std::uint16_t id, const std::uint8_t* p, std::size_t length, ULONGLONG now)
{
    switch (id) {
    case kZoneIn:
        if (g_zones < kMostZones) {
            ++g_zones;
            say("zone-in packet (0x00A)");
        }
        return;
    case kInventoryModify:
        if (length >= 0x0A)
            change(p[8], p[9], false, 0, u32(p + 4), "0x01E quantity");
        return;
    case kInventoryAssign:
        if (length >= 0x0C)
            change(p[0xA], p[0xB], true, u16(p + 8), u32(p + 4), "0x01F item");
        return;
    case kInventoryItem:
        if (length >= 0x10)
            change(p[0xE], p[0xF], true, u16(p + 0xC), u32(p + 4), "0x020 item");
        return;
    case kSynthAnswer:
    case kSynthResult: {
        const std::size_t n = length < 0x20 ? length : 0x20;
        say(format("%s: result byte %u; bytes %s", id == kSynthAnswer ? "synthesis answer (0x06F)" : "synthesis result (0x070)",
                   length > 4 ? p[4] : 0u, hex(p, n).c_str()));
        open_window(now);
        return;
    }
    default:
        break;
    }
    if (now < g_window_until && id < 512 && !g_seen[id] && !deny(id) && g_answers < kMostAnswers) {
        g_seen[id] = true;
        ++g_answers;
        say(format("the server sent 0x%03X (%u bytes) %llu ms after the crystal or synthesis", id, static_cast<unsigned>(length),
                   now - g_window_from));
    }
}

void sent(std::uint16_t id, const std::uint8_t* p, std::size_t length, ULONGLONG now)
{
    if (id == kSynthStart && length >= 0x22) {
        const std::uint16_t crystal = u16(p + 6);
        const unsigned count = p[9];
        std::string text = format("synthesis sent: crystal %s from slot %u%s; %u ingredients", item_text(crystal).c_str(), p[8],
                                  check(0, p[8], crystal).c_str(), count);
        for (unsigned i = 0; i < count && i < 8; ++i)
            text += format("; %s from slot %u", item_text(u16(p + 0x0A + 2 * i)).c_str(), p[0x1A + i]) +
                    check(0, p[0x1A + i], u16(p + 0x0A + 2 * i));
        say(text);
        open_window(now);
    } else if (id == kUseItem && length >= 0x11) {
        const unsigned slot = p[0x0E], bag = p[0x10] < 16 ? p[0x10] : 0;
        const Slot& s = g_slots[bag][slot];
        const bool crystal = s.known && crystalish(s.item);
        if (!crystal && g_uses >= kMostUses)
            return;
        if (!crystal)
            ++g_uses;
        say(format("item used: slot %u of bag %u (byte 0x10 = %u): %s", slot, bag, p[0x10], slot_text(s).c_str()));
        if (crystal)
            open_window(now);
    }
}

template <typename Handler> void walk(const std::uint8_t* buffer, std::size_t size, Handler handler)
{
    const ULONGLONG now = GetTickCount64();
    for (std::size_t at = kHeader; at + 4 <= size;) {
        const std::uint8_t* packet = buffer + at;
        const std::uint16_t head = u16(packet);
        const std::size_t length = static_cast<std::size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size)
            break;
        handler(static_cast<std::uint16_t>(head & 0x1FF), packet, length, now);
        at += length;
    }
}

void first_line()
{
    if (!g_first)
        return;
    g_first = false;
    say("logging crystals, clusters and synthesis (the server's answers, the stacks as it changes them, the synthesis the "
        "game sends and what the server holds in those slots)");
}

} // namespace

void on_received(const std::uint8_t* buffer, std::size_t size)
{
    if (buffer == nullptr || size < kHeader + 4)
        return;
    const std::lock_guard lock(g_lock);
    first_line();
    walk(buffer, size, received);
    watch_gate(GetTickCount64());
}

void on_sent(const std::uint8_t* buffer, std::size_t length)
{
    if (buffer == nullptr || length < kHeader + 4)
        return;
    const std::lock_guard lock(g_lock);
    first_line();
    walk(buffer, length, sent);
}

void on_text(int mode, std::string_view message)
{
    const bool server_lines = mode == 121 || mode == 122 || mode == 123;
    const bool system_lines = mode == 8 || mode == 90 || mode == 91 || mode == 127 || mode == 129 || mode == 142 || mode == 144;
    if (!server_lines && !system_lines)
        return;
    std::string text;
    for (const char c : message.substr(0, 240)) {
        const auto b = static_cast<unsigned char>(c);
        if (b == '\n' || b < 0x20)
            continue; // the game's colour codes and line ends
        text += b < 0x7F ? c : '?';
    }
    if (text.empty())
        return;
    std::string lower = text;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool keyword = lower.find("synth") != std::string::npos || lower.find("crystal") != std::string::npos ||
                         lower.find("cluster") != std::string::npos || lower.find("ingredient") != std::string::npos;
    const std::lock_guard lock(g_lock);
    if (!server_lines && !keyword && GetTickCount64() >= g_window_until)
        return;
    say(format("text, mode %d: %s", mode, text.c_str()));
}

} // namespace vanacore::craftlog
