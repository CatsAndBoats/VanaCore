#include "vanaclock.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::vanaclock {
namespace {

// XiDateTime (the PS2 client's names; the PC's code from VA 0x1018A600): the game's Vana'diel date and time, in sixtieths of a
// Vana'diel second (5,184,000 to a day), at VA 0x10627E7C, and its game timer's flag at VA 0x10375B74. While the flag is set,
// SysMove (VA 0x1018AD70) works the time out of the server's time every frame. A cutscene clears it (DisableGameTimer, VA
// 0x1018AFB0), sets an hour (SetCurrentDateTime, VA 0x1018ACA0) and sets it again at its end (EnableGameTimer, VA 0x1018AF90);
// while it's clear the time stands still. The free camera's Time does the same.
//
// EnableGameTimer: mov byte [flag],1; call SysMove; mov ecx,[zone]; test ecx,ecx; je; jmp UpdateCombineWeather; ret; 4 nops;
// then DisableGameTimer: mov byte [flag],0; ret
constexpr char kTimer[] = "C6 05 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E9 ?? ?? ?? ?? C3 90 90 90 90 "
                          "C6 05 ?? ?? ?? ?? 00 C3";
// SetCurrentDateTime(int): mov eax,[esp+4]; mov ecx,<the time>; push eax; call SetTickFrame; ret; then the next function's start
constexpr char kSet[] = "8B 44 24 04 B9 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? C3 8D 4C 24 04 E8";
constexpr std::int32_t kTicksDay = 5184000;
constexpr double kTicksMinute = 3600.0;

bool g_searched = false;
const volatile std::uint8_t* g_timer = nullptr;
const volatile std::int32_t* g_time = nullptr;

} // namespace

double server_minute(std::uint32_t server)
{
    // The PC's clock to the millisecond, put on the server's: in whole seconds, each second moved the clock 0.42 Vana'diel minutes at
    // once, and everything that goes by it (the shadows and the light round 06:00 and 18:00) stepped once a second (seen
    // 2026-11-13). The server's seconds come only in its packets' headers, so they set how far the PC's clock is off it, taken again
    // only when that's out by two seconds or more (a second either way is the two clocks' rounding, not a change); a server time
    // more than a day off the PC's isn't taken (as before).
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const double pc =
        static_cast<double>(((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000ULL) * 1e-7;
    static double offset = 0.0;
    const double off = static_cast<double>(server) - std::floor(pc);
    const bool known = server != 0 && std::fabs(off) < 86400.0;
    if (known && std::fabs(off - offset) >= 2.0)
        offset = off;
    // LandSandBoat's CVanaTime: 25 Vana'diel minutes to an Earth minute from Unix 1009810800
    double minute = std::fmod((pc + (known ? offset : 0.0) - 1009810800.0) * 25.0 / 60.0, 1440.0);
    if (minute < 0.0)
        minute += 1440.0;
    return minute;
}

bool held_minute(double& minute)
{
    if (!g_searched) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return false;
        g_searched = true;
        const std::uintptr_t timer = scan::find(game, kTimer);
        const std::uintptr_t set = scan::find(game, kSet);
        if (timer == 0 || set == 0) {
            log::write(L"Vana'diel clock: the game's own wasn't found in FFXiMain.dll: VanaCore's sun keeps the server's hour in "
                       L"cutscenes and under the free camera's Time");
            return false;
        }
        g_timer = reinterpret_cast<const volatile std::uint8_t*>(static_cast<std::uintptr_t>(scan::operand(timer, 2)));
        g_time = reinterpret_cast<const volatile std::int32_t*>(static_cast<std::uintptr_t>(scan::operand(set, 5)));
    }
    if (g_timer == nullptr || g_time == nullptr || *g_timer != 0)
        return false;
    const std::int32_t ticks = ((*g_time % kTicksDay) + kTicksDay) % kTicksDay;
    // (never 0, which the sun's code takes for "not known yet": midnight held is a hundredth of a minute past)
    const double held = static_cast<double>(ticks) / kTicksMinute;
    minute = held > 0.01 ? held : 0.01;
    return true;
}

} // namespace vanacore::vanaclock
