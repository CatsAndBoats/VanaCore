#include "packetwatch.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "game.hpp"
#include "log.hpp"

namespace vanacore::packetwatch {
namespace {

constexpr std::size_t kHeader = 0x1C; // the buffer's header: its counter first
constexpr std::uint16_t kZoneIn = 0x00A, kZoneOut = 0x00B, kAction = 0x028;
// 0x00A: the player's id (0x04)
constexpr std::size_t kId = 0x04;
// 0x028, bit-packed from its 5th byte (see others.cpp): the actor (bit 40, 32 bits), the category (82, 4) and its
// parameter (86, 16: the weapon skill's or spell's id in a finished one)
constexpr std::size_t kActor = 40, kCategory = 82, kParameter = 86, kParameterEnd = 102;
constexpr std::uint32_t kMelee = 1;

constexpr ULONGLONG kReport = 30000;      // ms between the lines of counts
constexpr ULONGLONG kBlockedEvery = 10000; // ms: the blocked packets logged one by one, up to kBlockedLines in it
constexpr unsigned kBlockedLines = 20;
// A counter that jumps by more than this isn't a loss: a new connection (the game makes one for each zone)
constexpr std::uint16_t kJump = 256;

std::wstring g_ini;
FILETIME g_ini_time{};
ULONGLONG g_next_check = 0;
std::atomic<bool> g_on{false};

// What came since the last line of counts
struct Counts {
    unsigned buffers = 0;
    unsigned gaps = 0;      // times the counter skipped
    unsigned missing = 0;   // buffers it skipped over
    unsigned repeats = 0;   // buffers that came again or out of order
    unsigned actions = 0;   // 0x028 packets
    unsigned yours = 0;     // ... with you as the actor
    unsigned lost = 0;      // action packets that came and didn't reach the game
    unsigned blocked = 0;   // packets an addon or plugin blocked
};
Counts g_counts;
ULONGLONG g_counts_at = 0;

bool g_have = false;      // g_prev is the last buffer's counter
bool g_resync = false;    // the next buffer starts the counter over (a zone-out or zone-in came)
std::uint16_t g_prev = 0;
ULONGLONG g_prev_at = 0;
std::uint16_t g_this = 0;     // the buffer being handled
unsigned g_raw_actions = 0;   // its 0x028 packets as they came
std::uint32_t g_me = 0;       // the player's id (0x00A)
ULONGLONG g_blocked_from = 0;
unsigned g_blocked_lines = 0;
unsigned g_blocked_quiet = 0; // blocked since the lines were used up

std::uint16_t word(const std::uint8_t* at)
{
    std::uint16_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::uint32_t dword(const std::uint8_t* at)
{
    std::uint32_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::uint32_t bits(const std::uint8_t* packet, std::size_t at, unsigned count)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= static_cast<std::uint32_t>((packet[(at + i) / 8] >> ((at + i) % 8)) & 1) << i;
    return value;
}

const char* category_name(std::uint32_t category)
{
    switch (category) {
    case 1: return "melee";
    case 2: return "ranged attack finished";
    case 3: return "weapon skill finished";
    case 4: return "spell finished";
    case 5: return "item finished";
    case 6: return "job ability";
    case 7: return "weapon skill started";
    case 8: return "spell started";
    case 9: return "item started";
    case 11: return "monster ability finished";
    case 12: return "ranged attack started";
    case 13: return "pet ability finished";
    case 14: return "dance";
    case 15: return "run ability";
    default: return "other";
    }
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

void read_settings(bool first)
{
    const bool on = GetPrivateProfileIntW(L"client", L"packets.watch", 0, g_ini.c_str()) != 0;
    if (on == g_on.load())
        return;
    g_on = on;
    g_have = g_resync = false;
    g_counts = {};
    g_counts_at = GetTickCount64();
    g_blocked_from = 0;
    g_blocked_lines = g_blocked_quiet = 0;
    if (on || !first)
        log::write(L"Packet watch: %ls ([client] packets.watch)", on ? L"on" : L"off");
}

void report(ULONGLONG now)
{
    const Counts c = g_counts;
    g_counts = {};
    g_counts_at = now;
    if (c.buffers == 0) {
        log::write(L"Packet watch: no buffer came from the server in the last %llu s", kReport / 1000);
        return;
    }
    log::write(L"Packet watch, last %llu s: %u buffers from the server (the counter skipped %u times, %u buffers not seen; %u came "
               L"again), %u action packets (%u yours, %u not delivered to the game), %u packets blocked by addons or plugins",
               kReport / 1000, c.buffers, c.gaps, c.missing, c.repeats, c.actions, c.yours, c.lost, c.blocked);
}

} // namespace

void install(const wchar_t* ini)
{
    g_ini = ini != nullptr ? ini : L"";
    if (g_ini.empty())
        return;
    g_ini_time = ini_time();
    read_settings(true);
}

bool on()
{
    return g_on.load(std::memory_order_relaxed);
}

void on_frame()
{
    if (g_ini.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    if (now >= g_next_check) {
        g_next_check = now + 500;
        const FILETIME time = ini_time();
        if (CompareFileTime(&time, &g_ini_time) != 0) {
            g_ini_time = time;
            read_settings(false);
        }
    }
    if (g_on.load() && now - g_counts_at >= kReport)
        report(now);
}

void on_received(const std::uint8_t* buffer, std::size_t size)
{
    if (!g_on.load() || buffer == nullptr || size < kHeader)
        return;
    const ULONGLONG now = GetTickCount64();
    const std::uint16_t counter = word(buffer);
    g_this = counter;
    g_raw_actions = 0;
    ++g_counts.buffers;

    // The counter: one more than the last buffer's, unless a zone came between
    const bool resync = g_resync;
    g_resync = false;
    bool moved = true; // g_prev follows it
    if (g_have && !resync) {
        const std::uint16_t step = static_cast<std::uint16_t>(counter - g_prev);
        const double since = static_cast<double>(now - g_prev_at) / 1000.0;
        if (step == 0 || step >= 0x8000) {
            ++g_counts.repeats;
            moved = false;
            log::write(L"Packet watch: buffer %u came again (the last was %u), %.2f s after it: the server sent it again, or it "
                       L"came out of order",
                       counter, g_prev, since);
        } else if (step > kJump) {
            log::write(L"Packet watch: the counter went from %u to %u: taken as a new connection, not a loss", g_prev, counter);
        } else if (step > 1) {
            ++g_counts.gaps;
            g_counts.missing += step - 1u;
            log::write(L"Packet watch: buffer %u came after %u: %u not seen (%u to %u), %.2f s since the last", counter, g_prev,
                       step - 1u, static_cast<std::uint16_t>(g_prev + 1), static_cast<std::uint16_t>(counter - 1), since);
        }
    }
    if (moved) {
        g_prev = counter;
        g_prev_at = now;
    }
    g_have = true;

    // The packets: yours, and the zone changes that start the counter over
    bool zoned = false;
    for (std::size_t at = kHeader; at + 4 <= size;) {
        const std::uint8_t* packet = buffer + at;
        const std::uint16_t head = word(packet);
        const std::size_t length = static_cast<std::size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size)
            break;
        const std::uint16_t id = head & 0x1FF;
        if (id == kZoneIn || id == kZoneOut) {
            zoned = true;
            if (id == kZoneIn && length >= kId + 4)
                g_me = dword(packet + kId);
        } else if (id == kAction) {
            ++g_raw_actions;
            ++g_counts.actions;
            if (length * 8 >= kParameterEnd) {
                if (g_me == 0)
                    g_me = game::local_player_id();
                if (g_me != 0 && bits(packet, kActor, 32) == g_me) {
                    ++g_counts.yours;
                    const std::uint32_t category = bits(packet, kCategory, 4);
                    if (category != kMelee)
                        log::write(L"Packet watch: your action came in buffer %u: %hs (category %u, parameter %u)", counter,
                                   category_name(category), category, bits(packet, kParameter, 16));
                }
            }
        }
        at += length;
    }
    if (zoned)
        g_resync = true;
}

void on_delivered(const std::uint8_t* buffer, std::size_t size)
{
    if (!g_on.load() || buffer == nullptr || size < kHeader || g_raw_actions == 0)
        return;
    unsigned delivered = 0;
    for (std::size_t at = kHeader; at + 4 <= size;) {
        const std::uint16_t head = word(buffer + at);
        const std::size_t length = static_cast<std::size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size)
            break;
        if ((head & 0x1FF) == kAction)
            ++delivered;
        at += length;
    }
    if (delivered < g_raw_actions) {
        g_counts.lost += g_raw_actions - delivered;
        log::write(L"Packet watch: buffer %u had %u action packets (0x028) and %u reached the game: the rest were blocked or "
                   L"dropped inside VanaCore",
                   g_this, g_raw_actions, delivered);
    }
    g_raw_actions = 0;
}

void blocked(std::string_view by, std::uint16_t id)
{
    if (!g_on.load(std::memory_order_relaxed))
        return;
    ++g_counts.blocked;
    const ULONGLONG now = GetTickCount64();
    if (now - g_blocked_from >= kBlockedEvery) {
        if (g_blocked_quiet != 0)
            log::write(L"Packet watch: %u more packets blocked in that time", g_blocked_quiet);
        g_blocked_from = now;
        g_blocked_lines = g_blocked_quiet = 0;
    }
    if (g_blocked_lines >= kBlockedLines) {
        ++g_blocked_quiet;
        return;
    }
    ++g_blocked_lines;
    const std::string name(by);
    log::write(L"Packet watch: %hs blocked the received packet 0x%03X", name.empty() ? "something" : name.c_str(), id);
}

} // namespace vanacore::packetwatch
