#include "packetflow.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include "log.hpp"
#include "packets.hpp"
#include "plugins.hpp"
#include "scan.hpp"

namespace vanacore::packetflow {
namespace {

// FFXiMain.dll's check before it sends a buffer: mov eax,[esi+417Ch] (the wait); push eax; push 0; call check;
// add esp,0Ch; test al,al; je (not yet). The call is 9 bytes in: it's sent to send_check() in place of the game's check.
// The same bytes PacketFlow looks for.
constexpr char kSendCheck[] = "8B 86 7C 41 00 00 50 6A 00 E8 ?? ?? ?? ?? 83 C4 0C 84 C0 74";
constexpr std::size_t kSendCheckCall = 9;

// The wait the game is given once the server has answered (PacketFlow's)
constexpr std::int32_t kShortWait = 250;

// The outgoing zone-out packet: after it, until the game sends another, the wait is always the short one (the game's own
// grows to seconds while it connects to the new zone)
constexpr std::uint16_t kZoneOut = 0x00D;
constexpr std::size_t kHeader = 0x1C;

// bool __cdecl (uint32_t, int32_t wait, uint32_t): true when the game may send now. The caller pushes three arguments
// (add esp,0Ch after the call); the third is passed on as it came.
using SendCheck = bool(__cdecl*)(std::uint32_t, std::int32_t, std::uint32_t);

// Only touched on the game's thread, but for the flags (set by the packet hooks).
std::wstring g_ini;
bool g_enabled = true;
bool g_logged_once = false;
bool g_logged = false;
FILETIME g_ini_time{};
ULONGLONG g_next_check = 0;
bool g_searched = false;
bool g_holding = false; // PacketFlow held off

std::uintptr_t g_call = 0;           // the game's call to its check
std::uint8_t g_game[5] = {};         // its bytes
bool g_patched = false;
SendCheck g_check = nullptr;         // the game's check

std::atomic<bool> g_awaiting{true};  // a buffer sent and nothing from the server since
std::atomic<bool> g_leaving{false};  // the last packet sent was the zone-out (0x00D)

bool __cdecl send_check(std::uint32_t first, std::int32_t wait, std::uint32_t third)
{
    const bool shorten = !g_awaiting.load(std::memory_order_relaxed) || g_leaving.load(std::memory_order_relaxed);
    const bool send = g_check(first, shorten ? kShortWait : wait, third);
    if (send)
        g_awaiting.store(true, std::memory_order_relaxed);
    return send;
}

bool write_code(std::uintptr_t at, const void* bytes, std::size_t size)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    return true;
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t offset = 0;
    std::memcpy(&offset, reinterpret_cast<const void*>(call + 1), sizeof offset);
    return call + 5 + offset;
}

void find()
{
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    if (module == nullptr)
        return;
    g_searched = true;
    const std::uintptr_t at = scan::find(module, kSendCheck);
    if (at == 0) {
        log::write(L"Packet flow: couldn't find the game's check before it sends; it keeps its own timing ([client] packetflow)");
        return;
    }
    g_call = at + kSendCheckCall;
    std::memcpy(g_game, reinterpret_cast<const void*>(g_call), sizeof g_game);
    g_check = reinterpret_cast<SendCheck>(call_target(g_call));
    log::write(L"Packet flow: the game's check before it sends found (+0x%X)",
               static_cast<unsigned>(g_call - reinterpret_cast<std::uintptr_t>(module)));
}

void apply()
{
    // Nothing from the server is seen without packets.cpp's hooks: the wait would never be cut but after a zone-out
    const bool want = g_enabled && packets::ready();
    if (g_call == 0 || want == g_patched)
        return;
    if (want) {
        std::uint8_t bytes[5] = {0xE8};
        const auto offset = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&send_check) - (g_call + 5));
        std::memcpy(bytes + 1, &offset, sizeof offset);
        g_awaiting = true; // as the game starts: its own wait until the server answers
        g_leaving = false;
        g_patched = write_code(g_call, bytes, sizeof bytes);
    } else {
        g_patched = !write_code(g_call, g_game, sizeof g_game);
    }
    if (g_patched != want)
        log::write(L"Packet flow: couldn't change the game's code; it stays %ls", g_patched ? L"on" : L"off");
}

// PacketFlow patches the same call: held off while ours is on (unloaded before ours goes in, so its own clean-up doesn't
// take ours out), let load again once ours is out
void hold_plugin(bool on)
{
    if (on == g_holding)
        return;
    g_holding = on;
    if (on)
        plugins::hold("packetflow", "VanaCore does what it did ([client] packetflow); it loads again when that's off");
    else
        plugins::release("packetflow");
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

void read_settings()
{
    g_enabled = GetPrivateProfileIntW(L"client", L"packetflow", 1, g_ini.c_str()) != 0;
    if (g_logged_once && g_enabled == g_logged)
        return;
    g_logged_once = true;
    g_logged = g_enabled;
    log::write(L"Packet flow: %ls ([client] packetflow)", g_enabled ? L"on" : L"off");
}

} // namespace

void install(const wchar_t* ini)
{
    g_ini = ini != nullptr ? ini : L"";
    if (g_ini.empty())
        return;
    g_ini_time = ini_time();
    read_settings();
    hold_plugin(g_enabled); // before boot.txt's /load packetflow
}

void on_frame()
{
    if (g_ini.empty())
        return;
    const ULONGLONG tick = GetTickCount64();
    if (tick >= g_next_check) {
        g_next_check = tick + 500;
        const FILETIME time = ini_time();
        if (CompareFileTime(&time, &g_ini_time) != 0) {
            g_ini_time = time;
            read_settings();
        }
    }
    if (!g_searched)
        find();
    if (!g_searched)
        return;
    // PacketFlow out before ours goes in; ours out before it may load again
    if (g_enabled)
        hold_plugin(true);
    apply();
    if (!g_enabled && !g_patched)
        hold_plugin(false);
}

void on_received()
{
    g_awaiting.store(false, std::memory_order_relaxed);
}

void on_sent(const std::uint8_t* buffer, std::size_t length)
{
    // Each packet: a 4-byte header first, its id in the low 9 bits of the first word, its size in 4-byte units in the high 7
    bool any = false;
    std::uint16_t last = 0;
    for (std::size_t at = kHeader; buffer != nullptr && at + 4 <= length;) {
        std::uint16_t head = 0;
        std::memcpy(&head, buffer + at, sizeof head);
        const std::size_t size = static_cast<std::size_t>(head >> 9) * 4;
        if (size < 4 || at + size > length)
            break;
        any = true;
        last = head & 0x1FF;
        at += size;
    }
    if (any)
        g_leaving.store(last == kZoneOut, std::memory_order_relaxed);
}

} // namespace vanacore::packetflow
