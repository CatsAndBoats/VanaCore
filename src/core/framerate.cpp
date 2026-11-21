#include "framerate.hpp"

#include <windows.h>

#include <timeapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>

#include "game.hpp"
#include "graphics.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "window.hpp"
#include "../gfx/vanacore_gfx.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#pragma comment(lib, "winmm.lib") // timeBeginPeriod

namespace vanacore::framerate {
namespace {

// FFXiMain.dll's "FPS limit" setter (its debug text reads "FPS 制限[%d]"): void __cdecl(int).
// It stores the divisor in a global (the last value set) and in a field of the game's config
// object, which the frame loop caps at 60 / divisor. The game calls it with 2 (30 FPS) in normal
// play and other values in some states (4, 15 FPS).
//   +0  mov eax,[esp+4]  +4  mov ecx,[last]  ...  +20  mov ecx,[config]  ...  +42  mov [ecx+field],eax
constexpr char kSetDivisor[] = "8B 44 24 04 8B 0D ?? ?? ?? ?? 81 EC 00 01 00 00 3B C1 74 ?? 8B 0D ?? ?? ?? ?? "
                               "50 8D 54 24 04 68 ?? ?? ?? ?? A3 ?? ?? ?? ?? 52 89 41 ??";
constexpr int kLastAt = 6;
constexpr int kConfigAt = 22;
constexpr int kFieldAt = 44;
constexpr int kGameDivisor = 2; // the game's normal 30 FPS
// The game's wait for its cap, in its frame timing (GameManager's, before the tick is worked out): while the frame's 60ths
// (timeGetTime, whole ms) are under the divisor, Sleep(1) and look again; then a second loop of Sleep(0) for any left.
// Uneven by a millisecond or more, and by 15.6 ms steps without timer.resolution. VanaCore jumps from the first check
// past both (je +0F becomes jmp to the fstp st(0) both end at) and holds the rate with its own timer instead. The divisor
// stays the game's: the tick is still at least the divisor, so at 30 FPS each frame still counts as 2 60ths.
//   fcompp; fnstsw ax; and eax,100h; je +0Fh; push 1; mov byte [esp+1Bh],1; fstp st(0); call ebp; pause; jmp back;
//   cmp byte [esp+17h],1; je (the fstp st(0))
constexpr char kWait[] = "DE D9 DF E0 25 00 01 00 00 74 0F 6A 01 C6 44 24 1B 01 DD D8 FF D5 F3 90 EB AC 80 7C 24 17 01 74 57";
constexpr int kWaitJump = 9;     // the je that waits when the frame came early
constexpr int kWaitSecond = 31;  // the second loop's je, whose target is where both waits end
constexpr int kGameRate = 60;   // frame rate at divisor 1

int g_divisor = kGameDivisor; // ours for the game's normal state; 0 is no cap; the game's own 2 is its 30
double g_limit = 0;           // VanaCore's own frame rate limit; 0 when the divisor lands on fps.max
// [graphics] vsync. Before the game's own wait is off (kWait), its cap (60 / divisor) only holds while Present waits for
// the display: on VanaCore's Direct3D 12 or 9 without vsync it draws as fast as the graphics card goes (all of it, so the
// rest of the PC lags), so VanaCore holds it to the divisor's rate itself.
bool g_vsync = false;

// Only touched on the game's render thread.
int* g_last = nullptr;     // the game's global
void** g_config = nullptr; // where the game keeps its config object's address
BYTE g_field = 0;          // the divisor's offset in it
int g_requested = kGameDivisor;
bool g_patched = false;
bool g_failed = false;
bool g_wait_off = false; // the game's own wait skipped (kWait): VanaCore's limit holds every rate
std::set<int> g_logged;

int wanted()
{
    if (g_requested != kGameDivisor)
        return g_requested;
    return g_divisor;
}

void apply(int divisor)
{
    *g_last = divisor;
    if (auto* config = static_cast<BYTE*>(*g_config))
        *reinterpret_cast<int*>(config + g_field) = divisor;
}

// Stands in for the game's setter.
void __cdecl set_divisor(int requested)
{
    g_requested = requested;
    if (requested != kGameDivisor && g_logged.insert(requested).second)
        log::write(L"FPS: the game set its frame rate divisor to %d (%d FPS) for now; allowed", requested,
                   requested > 0 ? kGameRate / requested : 0);
    apply(wanted());
}

void patch()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t setter = game != nullptr ? scan::find(game, kSetDivisor) : 0;
    if (setter == 0) {
        g_failed = true;
        log::write(L"FPS: couldn't find the game's frame rate setting; it stays at its own 30 FPS at most");
        return;
    }
    g_last = *reinterpret_cast<int**>(setter + kLastAt);
    g_config = *reinterpret_cast<void***>(setter + kConfigAt);
    g_field = *reinterpret_cast<const BYTE*>(setter + kFieldAt);
    g_requested = *g_last;
    if (!imports::jump(reinterpret_cast<void*>(setter), reinterpret_cast<void*>(&set_divisor))) {
        g_failed = true;
        log::write(L"FPS: couldn't take over the game's frame rate setting; it stays at its own 30 FPS at most");
        return;
    }
    g_patched = true;
    log::write(L"FPS: took over the game's frame rate setting (FFXiMain.dll+0x%X), divisor %d",
               static_cast<unsigned>(setter - reinterpret_cast<std::uintptr_t>(game)), wanted());
    apply(wanted());
    // The game's own wait out: VanaCore's limit (before_present) holds the rate the divisor asks for
    const std::uintptr_t wait = scan::find(game, kWait);
    if (wait == 0) {
        log::write(L"FPS: couldn't find the game's frame rate wait; the game holds its own cap");
        return;
    }
    const std::uintptr_t end = wait + kWaitSecond + 2 + *reinterpret_cast<const std::int8_t*>(wait + kWaitSecond + 1);
    const std::intptr_t to = static_cast<std::intptr_t>(end) - static_cast<std::intptr_t>(wait + kWaitJump + 2);
    const BYTE jump[2] = {0xEB, static_cast<BYTE>(to)};
    DWORD old = 0;
    if (to < -128 || to > 127 || !VirtualProtect(reinterpret_cast<void*>(wait + kWaitJump), 2, PAGE_EXECUTE_READWRITE, &old)) {
        log::write(L"FPS: couldn't change the game's frame rate wait; the game holds its own cap");
        return;
    }
    std::memcpy(reinterpret_cast<void*>(wait + kWaitJump), jump, sizeof jump);
    VirtualProtect(reinterpret_cast<void*>(wait + kWaitJump), 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(wait + kWaitJump), 2);
    g_wait_off = true;
    log::write(L"FPS: the game's own frame rate wait is off (FFXiMain.dll+0x%X): VanaCore holds the rate",
               static_cast<unsigned>(wait - reinterpret_cast<std::uintptr_t>(game)));
}

// ---- VanaCore's own limit ----------------------------------------------------------------

LARGE_INTEGER g_frequency{};
LONGLONG g_next = 0; // when the next frame may be presented
LONGLONG g_slot = 0; // the frame just held: its place in the cadence (late or not); 0, not held (frame_slot)
HANDLE g_timer = nullptr;

// Sleeps most of the way on a high-resolution timer, then spins the last half millisecond.
void wait_until(LONGLONG deadline)
{
    const LONGLONG spin = g_frequency.QuadPart / 2000;
    for (LARGE_INTEGER now; QueryPerformanceCounter(&now), now.QuadPart < deadline;) {
        const LONGLONG remaining = deadline - now.QuadPart;
        if (g_timer != nullptr && remaining > spin) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((remaining - spin) * 10'000'000.0 / g_frequency.QuadPart);
            if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(g_timer, INFINITE);
        } else {
            YieldProcessor();
        }
    }
}

void limit(double rate)
{
    const auto interval = static_cast<LONGLONG>(g_frequency.QuadPart / rate);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Keep an even cadence; after a stall of more than a frame, start again from now instead of
    // rushing frames out to catch up.
    if (g_next == 0 || now.QuadPart - g_next > interval)
        g_next = now.QuadPart;
    else
        wait_until(g_next);
    g_slot = g_next;
    g_next += interval;
}

int g_asked = -1; // fps.max as last read; -1 before the first read
// [client] fps.background: the frame rate while the game is in the background (window::in_background), 1-60, held by
// VanaCore's own limit; 0 (the default) is fps.max's, in front or not.
int g_background = 0;
bool g_was_background = false;
// With frame generation, the most frames shown a second (the game's and the ones made between them): the game's own frame
// rate is held to it over the frames shown for each of the game's (60 at 4x; 2x and 3x never hold it under 60).
constexpr int kFramegenMax = 240;
int g_fps_applied = kGameRate; // what fps.max gave frame generation's log (vanacore_gfx_set_fps_max)
int g_held_shown = 1;          // the frames shown for each of the game's when the hold was last worked out; 0, work it out
double g_held = 0;             // the game's frame rate held to for it; 0 when it isn't

// [client] fps.cutscene: the frame rate while the player is in an event (a cutscene), 1-60, or 0 for no cap; -1 (unset) is
// off, fps.max's. The event is the player's status 4 (Ashita's Status::Event), polled ten times a second: the status the
// server last sent for the player (StatusServer, what Ashita's FastCS reads), or the client's own (Status). The server's is
// 4 through every event, an NPC's chat and menu too; the client's follows it only in some (a Home Point's or Survival
// Guide's warp), so it alone missed most (Lunexi, JullRole, 12 November).
constexpr int kEventStatus = 4;
constexpr ULONGLONG kPollMs = 100;
int g_max = kGameRate;        // [client] fps.max as last read
int g_cutscene = -1;          // [client] fps.cutscene as last read
bool g_in_cutscene = false;   // the player's status was Event at the last poll (looked for while either cutscene key is set)
ULONGLONG g_next_poll = 0;
// [graphics] vsync.cutscene: vsync while the player is in an event, 0 or 1; -1 (unset) is vsync's. Off with fps.cutscene=0
// plays a cutscene as fast as the PC draws while vsync holds the rest of the game to the screen. Direct3D 12 and 11 take
// it at the next Present (vanacore_gfx_set_vsync); Direct3D 9 only takes vsync at a Reset, so it keeps [graphics] vsync.
int g_vsync_cutscene = -1;

// Whether Present waits for the display now: vsync.cutscene in an event when it is set, else `vsync`
bool vsync_now(bool vsync)
{
    return g_in_cutscene && g_vsync_cutscene >= 0 ? g_vsync_cutscene != 0 : vsync;
}

// vsync as it is now to VanaCore's Direct3D 12 or 11, after an event starts or ends or the settings change (graphics.cpp's
// apply_live gives it through framerate::vsync too, whichever of the two reads the settings file first)
void apply_vsync()
{
    if (graphics::active())
        vanacore_gfx_set_vsync(vsync_now(g_vsync) ? 1 : 0);
}

// fps.max=0 waits for a character in the world: 60 FPS until then, on the title screen, the character select,
// "Downloading data" and the black screen between zones. The game seems to count its wait for the map server in frames:
// unlocked (hundreds of frames a second on VanaCore's renderer, which has no vsync) it gave up with FFXI-3001 3 or 4
// seconds after its first packet, where at 60 it waited 20 s and more (Haruuc's log, 6 November). Under Ashita the fps
// addon's /fps comes from the player's script (Haruuc's: /fps 1, 60) and the game keeps its own wait, so the login never
// ran unlocked. Polled with the cutscene, ten times a second.
bool g_out_of_world = true; // no character in the world at the last poll (game::local_player_name empty); the title at start
bool g_holding = false;     // the rate applied is that hold's 60
bool g_hold_said[2] = {};   // the hold's start and end, said in the log the first time each

// The rate in force: fps.cutscene during an event when it is set, else fps.max
int wanted_max()
{
    return g_in_cutscene && g_cutscene >= 0 ? g_cutscene : g_max;
}

// The player's status as the server sent it and as the client has it; false without a player
bool player_status(std::uint32_t& server, std::uint32_t& client)
{
    game::PartyMember members[18];
    game::Alliance alliance;
    game::Entity entity;
    if (!game::party(members, alliance) || !members[0].active || members[0].target_index == 0 ||
        !game::entity(members[0].target_index, entity))
        return false;
    server = entity.status_server, client = entity.status;
    return true;
}

// `max` (fps.max, or fps.cutscene in an event): the divisor and VanaCore's own limit for it. 60, the game's own; 0,
// divisor 0: the game's cap taken off.
void apply_max(int max)
{
    const wchar_t* key = g_in_cutscene && g_cutscene >= 0 ? L"fps.cutscene" : L"fps.max";
    if (max < 0)
        max = 0;
    const bool held = max == 0 && g_out_of_world;
    if (held)
        max = kGameRate;
    const bool released = !held && g_holding;
    g_holding = held;
    if (max == g_asked)
        return;
    // The hold coming and going (at login and at each zone) is said once each, the first time, in its own words
    if (held || released) {
        bool& said = g_hold_said[held ? 0 : 1];
        if (!said)
            log::write(held ? L"FPS: %ls=0 waits for a character in the world: 60 FPS until one is (the title screen, the "
                              L"character select, \"Downloading data\", between zones), as the game counts its wait for "
                              L"the map server in frames"
                            : L"FPS: a character is in the world: %ls=0 unlocks the frame rate (divisor 0) until the next "
                              L"zone change or logout",
                      key);
        said = true;
    }
    const bool quiet = held || released;
    g_asked = max;
    if (max > kGameRate) {
        log::write(L"FPS: %ls=%d is above the game's 60 FPS limit; using 60", key, max);
        max = kGameRate;
    }
    g_next = 0;
    g_held_shown = 0; // worked out again for the new rate (before_present)
    if (max == 0) {
        g_divisor = 0, g_limit = 0;
        g_fps_applied = 0;
        vanacore_gfx_set_fps_max(g_fps_applied);
        if (!quiet)
            log::write(L"FPS: %ls=0: the game's frame rate unlocked (divisor 0); above 60 FPS the game runs fast", key);
        return;
    }
    g_divisor = kGameRate / max;
    g_limit = kGameRate / g_divisor == max ? 0 : max;
    g_fps_applied = g_limit > 0 ? max : kGameRate / g_divisor;
    vanacore_gfx_set_fps_max(g_fps_applied);
    if (g_limit > 0 && g_timer == nullptr) {
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (g_timer == nullptr) // before Windows 10 1803
            g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    if (!quiet)
        log::write(L"FPS: %ls=%d: the game capped at %d FPS (divisor %d)%ls", key, max, kGameRate / g_divisor, g_divisor,
                   g_limit > 0 ? L", then limited by VanaCore" : L"");
}

// [graphics] vsync.cutscene: said in the log when it changes
void read_vsync_cutscene(const wchar_t* ini)
{
    const int vsync = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"graphics", L"vsync.cutscene", -1, ini)), -1, 1);
    if (vsync == g_vsync_cutscene)
        return;
    g_vsync_cutscene = vsync;
    if (g_vsync_cutscene >= 0)
        log::write(L"FPS: vsync.cutscene=%d: vsync %ls while the player is in an event (status %d)%ls", g_vsync_cutscene,
                   g_vsync_cutscene != 0 ? L"on" : L"off", kEventStatus,
                   graphics::d3d9_active() ? L"; not with renderer=d3d9, whose vsync changes only at a Reset" : L"");
    else
        log::write(L"FPS: vsync.cutscene is off: vsync's in a cutscene too");
}

// [client] fps.max, fps.cutscene and fps.background, and [graphics] vsync and vsync.cutscene
void read_max(const wchar_t* ini)
{
    g_vsync = GetPrivateProfileIntW(L"graphics", L"vsync", 0, ini) != 0;
    read_vsync_cutscene(ini);
    const int background =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", L"fps.background", 0, ini)), 0, kGameRate);
    if (background != g_background) {
        g_background = background;
        if (g_background > 0)
            log::write(L"FPS: fps.background=%d: at most %d FPS while the game is in the background", g_background, g_background);
        else
            log::write(L"FPS: fps.background is off: fps.max in the background too");
    }
    g_max = std::max(0, static_cast<int>(GetPrivateProfileIntW(L"client", L"fps.max", kGameRate, ini)));
    const int cutscene = std::max(-1, static_cast<int>(GetPrivateProfileIntW(L"client", L"fps.cutscene", -1, ini)));
    if (cutscene != g_cutscene) {
        g_cutscene = cutscene;
        if (g_cutscene >= 0)
            log::write(L"FPS: fps.cutscene=%d: %ls while the player is in an event (status %d)", g_cutscene,
                       g_cutscene == 0 ? L"the frame rate unlocked" : L"at most that many FPS", kEventStatus);
        else
            log::write(L"FPS: fps.cutscene is off: fps.max in a cutscene too");
    }
    if (g_cutscene < 0 && g_vsync_cutscene < 0)
        g_in_cutscene = false;
    apply_max(wanted_max());
    apply_vsync();
}

// Ten times a second at most, on the render thread: a character coming into or leaving the world (fps.max=0's hold), and
// the player going into or out of an event, swap the rate
void poll_world()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_poll)
        return;
    g_next_poll = now + kPollMs;
    const bool out_of_world = game::local_player_name().empty();
    std::uint32_t server = 0, client = 0;
    const bool in_event = (g_cutscene >= 0 || g_vsync_cutscene >= 0) && !out_of_world && player_status(server, client) &&
                          (server == kEventStatus || client == kEventStatus);
    if (out_of_world == g_out_of_world && in_event == g_in_cutscene)
        return;
    g_out_of_world = out_of_world;
    if (in_event != g_in_cutscene) {
        g_in_cutscene = in_event;
        apply_vsync();
        log::write(L"FPS: the player %ls an event (cutscene; status %u from the server, %u in the client)%ls",
                   in_event ? L"is in" : L"left", server, client,
                   g_vsync_cutscene >= 0 && graphics::active() ? (vsync_now(g_vsync) ? L"; vsync on" : L"; vsync off") : L"");
    }
    apply_max(wanted_max());
}

} // namespace

void install(const wchar_t* ini)
{
    QueryPerformanceFrequency(&g_frequency);
    // [client] timer.resolution: the Windows timer resolution the game's process asks for, in ms.
    // Without it a Sleep (and timeGetTime) moves in 15.6 ms steps, whatever other programs ask for.
    const UINT resolution = GetPrivateProfileIntW(L"client", L"timer.resolution", 0, ini);
    if (resolution > 0) {
        const MMRESULT result = timeBeginPeriod(resolution);
        log::write(L"FPS: timer.resolution=%u: %ls", resolution,
                   result == TIMERR_NOERROR ? L"the game's timer runs in that many ms" : L"Windows refused it (TIMERR_NOCANDO)");
    }
    read_max(ini);
}

void reload(const wchar_t* ini)
{
    read_max(ini);
}

int divisor()
{
    if (g_patched)
        if (const auto* config = static_cast<const BYTE*>(*g_config))
            return *reinterpret_cast<const int*>(config + g_field);
    return kGameDivisor;
}

long long frame_slot()
{
    return g_slot;
}

bool vsync(bool on)
{
    return vsync_now(on);
}

void before_present()
{
    g_slot = 0;
    if (!g_patched && !g_failed)
        patch();
    poll_world();
    // The game may also have set the field before VanaCore took over, or its config object may
    // only just exist: keep it on what's wanted.
    if (g_patched) {
        auto* config = static_cast<BYTE*>(*g_config);
        if (config != nullptr && *reinterpret_cast<int*>(config + g_field) != wanted())
            apply(wanted());
    }
    // VanaCore's own limit (45); and the rate the divisor asks for (none for divisor 0): always with the game's own wait
    // off, else without vsync on Direct3D 12 or 9
    double rate = g_limit;
    const int divisor = g_patched ? wanted() : kGameDivisor;
    const double capped = divisor > 0 ? static_cast<double>(kGameRate) / divisor : 0; // 0: no cap
    const bool vsync = graphics::active() ? vsync_now(g_vsync) : g_vsync;
    if (capped > 0 && (g_wait_off || (!vsync && (graphics::active() || graphics::d3d9_active()))))
        rate = rate > 0 && rate < capped ? rate : capped;
    // With frame generation, the game held to 240 shown over the frames shown for each of the game's
    const int shown = graphics::shown_per_frame();
    if (shown != g_held_shown) {
        g_held_shown = shown;
        const double game = rate > 0 && (capped <= 0 || rate < capped) ? rate : capped; // 0: no cap
        const double most = static_cast<double>(kFramegenMax) / shown;
        const double held = shown > 1 && (game <= 0 || most < game) ? most : 0;
        if (held != g_held) {
            g_next = 0;
            if (held > 0)
                log::write(L"FPS: frame generation shows %d frames for each of the game's: the game held to %.1f FPS "
                           L"(at most %d shown)", shown, held, kFramegenMax);
            else if (g_held > 0)
                log::write(L"FPS: the game is no longer held for frame generation (%d frame(s) shown for each of its own)", shown);
        }
        g_held = held;
        vanacore_gfx_set_fps_max(held > 0 ? static_cast<int>(held) : g_fps_applied);
    }
    if (g_held > 0 && (rate <= 0 || g_held < rate))
        rate = g_held;
    // fps.background; the cadence starts again as the game goes to the background or comes back
    const bool background = g_background > 0 && window::in_background();
    if (background != g_was_background) {
        g_was_background = background;
        g_next = 0;
    }
    if (background && (rate <= 0 || g_background < rate))
        rate = g_background;
    if (rate > 0) {
        if (g_timer == nullptr) {
            g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (g_timer == nullptr) // before Windows 10 1803
                g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        limit(rate);
    }
}

} // namespace vanacore::framerate
