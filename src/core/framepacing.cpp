#include "framepacing.hpp"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "framerate.hpp"
#include "log.hpp"
#include "plugins.hpp"
#include "scan.hpp"

namespace vanacore::framepacing {
namespace {

// FFXiMain.dll's tick (GameManager::CheckTick in XIClient's names): the 60ths of a second the frame stands for, at least 1.
// At 60 FPS it's 1 whatever the frame took.
//   mov ecx,[game]; fld [ecx+28h]; fcomp [1]; fnstsw ax; test ah,5; jp +7; fld [1]; ret; fld [ecx+28h]; ret
constexpr char kTick[] = "8B 0D ?? ?? ?? ?? D9 41 28 D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 07 D9 05 ?? ?? ?? ?? C3 D9 41 28 C3";
constexpr std::size_t kTickSize = 33;

// The tick is the game's last four frames averaged and cut to a whole number of 60ths (at least the divisor, at most 20):
// 1 from 60 FPS down to 30, 2 down to 20, so between them everything it times runs slow. Three of its callers get the
// frame's real time instead, and they have to go together, since the game lines them up against each other:
// Motion: AnimationTrack::UpdateFrame's step through a motion (call tick; fmul [esp+10h]; fadd [esi+24h]; fstp [esi+24h])
// and AnimationTrack::UpdateBlendState's blend and fade between motions (push ecx; push esi; mov esi,ecx; call tick;
// fstp [esp+4]; xor eax,eax; mov ecx,1.0; mov al,[esi+8]).
constexpr char kMotionFrame[] = "E8 ?? ?? ?? ?? D8 4C 24 10 D8 46 24 D9 5E 24";
constexpr char kMotionBlend[] = "51 56 8B F1 E8 ?? ?? ?? ?? D9 5C 24 04 33 C0 B9 00 00 80 3F 8A 46 08";
constexpr std::ptrdiff_t kMotionBlendCall = 4;
// The step: the render manager's frame timing ends by keeping the tick as the frame's step (call tick; fstp [esi+0EB0h];
// pop edi; pop esi; add esp,8; ret), which the schedulers count by: a weapon skill, spell or ability is a scheduler
// playing its motions, hits and effects each at its frame, scaled to the length the server gave it; the effects, the
// screen fades and the camera's moves count by it too. With the motions at the clock and this at the tick, each motion
// of a weapon skill ended early and the character stood still until the scheduler's next one, at 30 to 40 FPS.
constexpr char kStep[] = "E8 ?? ?? ?? ?? D9 9E B0 0E 00 00 5F 5E 83 C4 08 C3";

// The interface: its root window's update, called once a frame (sub esp,24h; push ebx; push ebp; push esi; push edi;
// mov ebp,ecx; call update; mov ax,[ebp+56h]; xor edi,edi; cmp ax,di; jle +5; dec eax; mov [ebp+56h],ax), and the chat's
// in the main interface pass (mov ecx,[chat]; cmp ecx,ebx; je +5; call update; mov eax,[...]; cmp [eax+8],bl): each run
// again for each 60th a long frame stood for. The root's pattern starts past its first 5 bytes, which chat.cpp's detour
// of the same function (input.hide) has replaced by then.
constexpr char kUiRoot[] = "56 57 8B E9 E8 ?? ?? ?? ?? 66 8B 45 56 33 FF 66 3B C7 7E 05 48 66 89 45 56";
constexpr std::ptrdiff_t kUiRootCall = 4;
constexpr char kChatRoot[] = "8B 0D ?? ?? ?? ?? 3B CB 74 05 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 38 58 08";
constexpr std::ptrdiff_t kChatRootCall = 10;
// The two chat log windows (mov eax,[log1]; mov byte [eax+59h],1; mov ecx,[log2]; mov byte [ecx+59h],1; ret 8, as FancyChat
// finds them): each counts frames down (+3Ch) before its new lines scroll in, sped up to the clock.
constexpr char kChatWindows[] = "A1 ?? ?? ?? ?? C6 40 59 01 8B 0D ?? ?? ?? ?? C6 41 59 01 C2 08 00";
constexpr std::ptrdiff_t kChatWindowAt[2] = {1, 11};
constexpr std::ptrdiff_t kChatCountdown = 0x3C;
constexpr std::uint32_t kChatCountdownMax = 120;
constexpr int kMostExtra = 4;  // updates run again in a frame at most
constexpr int kMostTick = 20;  // 60ths a frame stands for at most: the game's own most

// The camera (FUN_1001ec70, YmCameraManager::Follow): sub esp,1E0h; mov al,[...]; push esi; push edi; mov edi,ecx; ...
// Its three loops over the frame's 60ths (call tick; call ftol) run twice a frame at 60 FPS: once, the wall collision
// shows every other one of the two steps it settles in, and the camera flickers against a wall.
constexpr char kCamera[] = "81 EC E0 01 00 00 A0 ?? ?? ?? ?? 56 57 8B F9 84 C0 89 7C 24 10";
constexpr std::ptrdiff_t kCameraLoops[3] = {0xBD8, 0x101C, 0x12E8};
// ... and its push out to the side, away from a wall (3 less the distance to the side, times 0.125 a step: fmul [0.125]
// twice), goes the whole way at once (times 1), so it doesn't creep back and forth
constexpr std::ptrdiff_t kCameraSideways[2] = {0xFD8, 0xFE8};

// Walk or run: SkeletalMeshActor::GetMoveVectorRatio (FUN_100c8680) picks an actor's motion by its speed, a share of a run
// (1/3 a walk; over 0.5 the run motion, played at that share). Your own character's comes from the keys, but on auto-run
// (and for everyone else) it's the distance moved this frame over the tick's worth of a run. Your character moves by the
// frame's 60ths uncut ([game+2Ch]), so below 60 FPS a walk on auto-run counted as more than a walk: at 31 FPS, 0.64, the
// run motion at walk speed. With [client] autorun.walk, its two calls to the tick (call tick; fmul [esp+8]) get the uncut
// 60ths for your character on auto-run; the others still move by the tick. The function: sub esp,8; push esi; mov esi,ecx; mov [esp+4],0;
// mov eax,[esi]; call [eax+304h] (it's your character); test al,al; jz; mov al,[autorun]; test al,al; jnz
constexpr char kRatio[] = "83 EC 08 56 8B F1 C7 44 24 04 00 00 00 00 8B 06 FF 90 04 03 00 00 84 C0 0F 84 ?? ?? ?? ?? A0 ?? ?? ?? ?? 84 C0 0F 85";
constexpr std::ptrdiff_t kRatioAutorun = 0x1F;
// call [edx+1A8h] (its run speed); fmul [1/60]; fstp [esp+8]; call tick; fmul [esp+8]; jmp; and riding: [eax+1A8h], [1/37.5]
constexpr char kRatioTicks[] = "FF 92 A8 01 00 00 D8 0D ?? ?? ?? ?? D9 5C 24 08 E8 ?? ?? ?? ?? D8 4C 24 08 EB ?? 8B 06 8B CE FF 90 "
                               "A8 01 00 00 D8 0D ?? ?? ?? ?? D9 5C 24 08 E8 ?? ?? ?? ?? D8 4C 24 08";
constexpr std::ptrdiff_t kRatioTicksAt = 0x188; // from the function's start
constexpr std::ptrdiff_t kRatioTickCalls[2] = {0x10, 0x2F};
constexpr std::ptrdiff_t kIsControlled = 0x304; // the actor's vtable: it's your character
constexpr std::ptrdiff_t kUncut = 0x2C;         // the game: the last four frames' 60ths averaged, not cut

constexpr double kLongest = 250.0; // ms: a longer frame (a load, a breakpoint) isn't timed

using TickFn = float(__cdecl*)();
using UpdateFn = void(__thiscall*)(void*);
using IsFn = bool(__thiscall*)(void*);

// A piece of the game's code replaced by ours, and the game's bytes to put back.
struct Patch {
    std::uintptr_t at = 0;
    std::size_t size = 0;
    std::uint8_t game[10] = {};
    bool on = false;
};

struct Settings {
    bool motion = true; // framepacing
    bool ui = true;     // framepacing.ui
    bool camera = true; // framepacing.camera
    bool walk = false;  // autorun.walk
};

// Only touched on the game's thread.
std::wstring g_ini;
Settings g_settings;
Settings g_logged;
bool g_logged_once = false;
FILETIME g_ini_time{};
ULONGLONG g_next_check = 0;
bool g_searched = false;
bool g_holding = false; // FrameFix held off

TickFn g_tick = nullptr;
Patch g_motion_frame, g_motion_blend, g_step;
UpdateFn g_ui_update = nullptr, g_chat_update = nullptr;
Patch g_ui_root, g_chat_root;
std::uintptr_t g_chat_window[2] = {}; // where the game keeps each chat window's address
Patch g_camera_loops[3], g_camera_sideways[2];
float g_one = 1.0f; // read by the camera's push to the side in place of its 0.125
Patch g_ratio_ticks[2];
const std::uint8_t* g_autorun = nullptr; // the game's auto-run flag
std::uintptr_t g_game_at = 0;            // where the game keeps its game manager (the tick's mov ecx,[game])

// The frame: measured at each Present, used through the next
LARGE_INTEGER g_frequency{}, g_last{};
unsigned g_frame = 0;
bool g_sixty = false;         // the game runs at 60 FPS (divisor 1): the only rate the tick is fixed at
float g_scalar = 1.0f;        // the last frame's 60ths, 1 to 4: the interface's
float g_motion = 1.0f;        // motion's and the step's 60ths: the last four frames' average (1 within 4%, as the game's
                              // own smoothing), 1 to 20
float g_history[4] = {1.0f, 1.0f, 1.0f, 1.0f};
unsigned g_history_at = 0;
bool g_history_primed = false;
double g_carry = 0.0;         // the interface's 60ths not run yet
int g_extra = 0;              // the interface's updates to run again this frame
double g_chat_carry[2] = {};
unsigned g_chat_frame = ~0u;
bool g_ui_inside = false, g_chat_inside = false;

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

bool put(Patch& patch, const void* bytes)
{
    if (patch.on || patch.at == 0)
        return patch.on;
    std::memcpy(patch.game, reinterpret_cast<const void*>(patch.at), patch.size);
    patch.on = write_code(patch.at, bytes, patch.size);
    return patch.on;
}

bool take_out(Patch& patch)
{
    if (!patch.on)
        return true;
    patch.on = !write_code(patch.at, patch.game, patch.size);
    return !patch.on;
}

// A call (E8) at `at` to `to`, then NOPs to `size`
bool put_call(Patch& patch, const void* to)
{
    std::uint8_t bytes[10] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    const auto offset = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(to) - (patch.at + 5));
    std::memcpy(bytes + 1, &offset, sizeof offset);
    return put(patch, bytes);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t offset = 0;
    std::memcpy(&offset, reinterpret_cast<const void*>(call + 1), sizeof offset);
    return call + 5 + offset;
}

bool read(void* out, std::uintptr_t from, std::size_t size)
{
    SIZE_T got = 0;
    return from != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(from), out, size, &got) &&
           got == size;
}

// ---- What the game's code calls in place of its own ---------------------------------------------------------------

// Motion and the step: the frame's real time at 60 FPS, the game's tick otherwise
float __cdecl motion_tick()
{
    return g_sixty ? g_motion : g_tick();
}

// The camera's loops: twice at 60 FPS, the game's count otherwise (as call tick; call ftol gave it)
int __cdecl camera_steps()
{
    return g_sixty ? 2 : static_cast<int>(g_tick());
}

// Walk or run's tick: the frame's 60ths uncut, as it moved by, for your character on auto-run; the tick otherwise
float __cdecl ratio_tick_for(void* actor)
{
    if (actor != nullptr && *g_autorun != 0) {
        const auto is_controlled = *reinterpret_cast<IsFn*>(*reinterpret_cast<std::uintptr_t*>(actor) + kIsControlled);
        const auto game = *reinterpret_cast<const std::uintptr_t*>(g_game_at);
        if (game != 0 && is_controlled(actor)) {
            const float uncut = *reinterpret_cast<const float*>(game + kUncut);
            if (uncut > 0.0f)
                return uncut;
        }
    }
    return g_tick();
}

// In place of the call to the tick: the actor is in esi
__declspec(naked) void ratio_tick()
{
    __asm {
        push esi
        call ratio_tick_for
        add esp, 4
        ret
    }
}

void __fastcall ui_update(void* self, void*)
{
    g_ui_update(self);
    if (g_ui_inside)
        return;
    g_ui_inside = true;
    for (int i = 0; i < g_extra; ++i)
        g_ui_update(self);
    g_ui_inside = false;
}

// A chat window's countdown to scrolling its new lines in, down by the 60ths the frame stood for beyond its one
void chat_scroll()
{
    for (int i = 0; i < 2; ++i) {
        std::uintptr_t window = 0;
        std::uint32_t countdown = 0;
        if (!g_sixty || !(g_scalar > 1.0f) || !read(&window, g_chat_window[i], sizeof window) || window == 0 ||
            !read(&countdown, window + kChatCountdown, sizeof countdown) || countdown == 0 || countdown > kChatCountdownMax) {
            g_chat_carry[i] = 0.0;
            continue;
        }
        g_chat_carry[i] += g_scalar - 1.0;
        auto extra = static_cast<std::uint32_t>(std::floor(g_chat_carry[i] + 0.000001));
        if (extra == 0)
            continue;
        g_chat_carry[i] -= extra;
        if (g_chat_carry[i] < 0.0 || g_chat_carry[i] > kMostExtra)
            g_chat_carry[i] = 0.0;
        *reinterpret_cast<std::uint32_t*>(window + kChatCountdown) = countdown - (extra < countdown ? extra : countdown);
    }
}

void __fastcall chat_update(void* self, void*)
{
    g_chat_update(self);
    if (g_chat_inside)
        return;
    if (g_chat_frame != g_frame) {
        g_chat_frame = g_frame;
        if (g_chat_window[0] != 0)
            chat_scroll();
    }
    g_chat_inside = true;
    for (int i = 0; i < g_extra; ++i)
        g_chat_update(self);
    g_chat_inside = false;
}

// ---- Finding the game's code ----------------------------------------------------------------------------------------

bool is_tick(std::uintptr_t at)
{
    return at != 0 && scan::find(at, kTickSize, kTick) == at;
}

void find()
{
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    if (module == nullptr)
        return;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    // motion and the step (and the tick, for the rest): all three or none, since the game lines them up against each other
    const std::uintptr_t frame = scan::find(module, kMotionFrame);
    const std::uintptr_t blend = scan::find(module, kMotionBlend);
    const std::uintptr_t step = scan::find(module, kStep);
    if (frame != 0 && is_tick(call_target(frame)) && blend != 0 && call_target(blend + kMotionBlendCall) == call_target(frame) &&
        step != 0 && call_target(step) == call_target(frame)) {
        g_tick = reinterpret_cast<TickFn>(call_target(frame));
        g_motion_frame = {frame, 5};
        g_motion_blend = {blend + kMotionBlendCall, 5};
        g_step = {step, 5};
    } else if (frame != 0 && is_tick(call_target(frame))) {
        g_tick = reinterpret_cast<TickFn>(call_target(frame)); // for the camera
        log::write(L"Frame pacing: couldn't find the game's %ls; the characters' motion and the actions' timing keep the "
                   L"game's tick ([client] framepacing)",
                   step == 0 ? L"frame step (what weapon skills, spells and effects count by)" : L"motion blending");
    } else {
        log::write(L"Frame pacing: couldn't find the game's motion timing; it keeps its own ([client] framepacing)");
    }
    // the interface and the chat
    const std::uintptr_t ui = scan::find(module, kUiRoot);
    const std::uintptr_t chat = scan::find(module, kChatRoot);
    const std::uintptr_t windows = scan::find(module, kChatWindows);
    if (ui != 0 && chat != 0) {
        g_ui_update = reinterpret_cast<UpdateFn>(call_target(ui + kUiRootCall));
        g_chat_update = reinterpret_cast<UpdateFn>(call_target(chat + kChatRootCall));
        g_ui_root = {ui + kUiRootCall, 5};
        g_chat_root = {chat + kChatRootCall, 5};
        for (int i = 0; i < 2 && windows != 0; ++i)
            std::memcpy(&g_chat_window[i], reinterpret_cast<const void*>(windows + kChatWindowAt[i]), sizeof g_chat_window[i]);
    } else {
        log::write(L"Frame pacing: couldn't find the interface's update; it keeps the game's timing ([client] framepacing.ui)");
    }
    // the camera: each loop a call to the tick and one to ftol, each push to the side fmul [0.125]
    const std::uintptr_t camera = scan::find(module, kCamera);
    const auto* code = reinterpret_cast<const std::uint8_t*>(camera);
    bool ok = camera != 0 && g_tick != nullptr;
    for (const std::ptrdiff_t loop : kCameraLoops)
        ok = ok && code[loop] == 0xE8 && code[loop + 5] == 0xE8 &&
             call_target(camera + loop) == reinterpret_cast<std::uintptr_t>(g_tick);
    for (const std::ptrdiff_t push : kCameraSideways)
        ok = ok && code[push - 2] == 0xD8 && code[push - 1] == 0x0D;
    if (ok) {
        for (int i = 0; i < 3; ++i)
            g_camera_loops[i] = {camera + kCameraLoops[i], 10};
        for (int i = 0; i < 2; ++i)
            g_camera_sideways[i] = {camera + kCameraSideways[i], 4};
    } else {
        log::write(L"Frame pacing: couldn't find the camera's steps; it keeps the game's ([client] framepacing.camera)");
    }
    // walk or run: both calls to the tick, in the function that starts as kRatio says
    const std::uintptr_t ratio = scan::find(module, kRatio);
    const std::uintptr_t ticks = scan::find(module, kRatioTicks);
    ok = ratio != 0 && ticks == ratio + kRatioTicksAt;
    for (const std::ptrdiff_t call : kRatioTickCalls)
        ok = ok && is_tick(call_target(ticks + call));
    if (ok) {
        const std::uintptr_t tick = call_target(ticks + kRatioTickCalls[0]);
        if (g_tick == nullptr)
            g_tick = reinterpret_cast<TickFn>(tick);
        std::memcpy(&g_game_at, reinterpret_cast<const void*>(tick + 2), sizeof g_game_at);
        g_autorun = *reinterpret_cast<const std::uint8_t* const*>(ratio + kRatioAutorun);
        for (int i = 0; i < 2; ++i)
            g_ratio_ticks[i] = {ticks + kRatioTickCalls[i], 5};
    } else {
        log::write(L"Frame pacing: couldn't find how the game picks walk or run; a walk on auto-run can look like a run "
                   L"below 60 FPS ([client] autorun.walk does nothing)");
    }
    log::write(L"Frame pacing: the game's timing found (motion +0x%X, step +0x%X, interface +0x%X, chat +0x%X, camera +0x%X, "
               L"walk or run +0x%X)",
               static_cast<unsigned>(frame ? frame - base : 0), static_cast<unsigned>(step ? step - base : 0),
               static_cast<unsigned>(ui ? ui - base : 0), static_cast<unsigned>(chat ? chat - base : 0),
               static_cast<unsigned>(camera ? camera - base : 0), static_cast<unsigned>(ok ? ratio - base : 0));
}

// ---- Settings and patches -------------------------------------------------------------------------------------------

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

void read_settings()
{
    g_settings.motion = GetPrivateProfileIntW(L"client", L"framepacing", 1, g_ini.c_str()) != 0;
    g_settings.ui = GetPrivateProfileIntW(L"client", L"framepacing.ui", 1, g_ini.c_str()) != 0;
    g_settings.camera = GetPrivateProfileIntW(L"client", L"framepacing.camera", 1, g_ini.c_str()) != 0;
    g_settings.walk = GetPrivateProfileIntW(L"client", L"autorun.walk", 0, g_ini.c_str()) != 0;
    if (g_logged_once && g_settings.motion == g_logged.motion && g_settings.ui == g_logged.ui &&
        g_settings.camera == g_logged.camera && g_settings.walk == g_logged.walk)
        return;
    g_logged_once = true;
    g_logged = g_settings;
    log::write(L"Frame pacing: at 60 FPS, kept to the clock: motion %ls, interface %ls, camera %ls ([client] framepacing, "
               L"framepacing.ui, framepacing.camera); a walk on auto-run kept a walk below 60 FPS: %ls ([client] autorun.walk)",
               g_settings.motion ? L"on" : L"off", g_settings.ui ? L"on" : L"off", g_settings.camera ? L"on" : L"off",
               g_settings.walk ? L"on" : L"off");
}

bool any()
{
    return g_settings.motion || g_settings.ui || g_settings.camera;
}

// FrameFix patches the same code: held off while any of ours is on (unloaded before ours goes in, so its own clean-up
// doesn't take ours out), let load again once all of ours is out
void hold_framefix(bool on)
{
    if (on == g_holding)
        return;
    g_holding = on;
    if (on)
        plugins::hold("framefix", "VanaCore's frame pacing does what it did ([client] framepacing, framepacing.ui, "
                                  "framepacing.camera); it loads again when they're all off");
    else
        plugins::release("framefix");
}

void apply()
{
    bool ok = true;
    if (g_motion_frame.at != 0) {
        if (g_settings.motion)
            ok &= put_call(g_motion_frame, reinterpret_cast<const void*>(&motion_tick)) &&
                  put_call(g_motion_blend, reinterpret_cast<const void*>(&motion_tick)) &&
                  put_call(g_step, reinterpret_cast<const void*>(&motion_tick));
        else
            ok &= take_out(g_motion_frame) && take_out(g_motion_blend) && take_out(g_step);
    }
    if (g_ui_update != nullptr) {
        if (g_settings.ui)
            ok &= put_call(g_ui_root, reinterpret_cast<const void*>(&ui_update)) &&
                  put_call(g_chat_root, reinterpret_cast<const void*>(&chat_update));
        else
            ok &= take_out(g_ui_root) && take_out(g_chat_root);
    }
    if (g_camera_loops[0].at != 0) {
        const auto one = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_one));
        for (Patch& loop : g_camera_loops)
            ok &= g_settings.camera ? put_call(loop, reinterpret_cast<const void*>(&camera_steps)) : take_out(loop);
        for (Patch& push : g_camera_sideways)
            ok &= g_settings.camera ? put(push, &one) : take_out(push);
    }
    if (g_ratio_ticks[0].at != 0) {
        for (Patch& call : g_ratio_ticks)
            ok &= g_settings.walk ? put_call(call, reinterpret_cast<const void*>(&ratio_tick)) : take_out(call);
    }
    if (!ok)
        log::write(L"Frame pacing: couldn't change some of the game's code; part of it may stay as it was");
}

// The frame just presented: its 60ths, for motion, the interface and the chat through the next
void time_frame(double ms)
{
    ++g_frame;
    g_sixty = framerate::divisor() == 1;
    const double sixtieths = ms * 60.0 / 1000.0;
    g_scalar = static_cast<float>(sixtieths < 1.0 ? 1.0 : sixtieths > kMostExtra ? kMostExtra : sixtieths);
    // motion and the step: the last four frames averaged (as the game smooths its own tick), 1 when within 4% of it
    const auto sample = static_cast<float>(sixtieths < 1.0 ? 1.0 : sixtieths > kMostTick ? kMostTick : sixtieths);
    if (!g_history_primed) {
        for (float& s : g_history)
            s = sample;
        g_history_primed = true;
    }
    g_history[g_history_at] = sample;
    g_history_at = (g_history_at + 1) % 4;
    const float average = (g_history[0] + g_history[1] + g_history[2] + g_history[3]) / 4.0f;
    g_motion = std::fabs(average - 1.0f) <= 0.04f ? 1.0f : average;
    // the interface: a run again for each whole 60th a frame stood for beyond the one it gets anyway. What a frame
    // stands for beyond a 60th is carried; a frame short of one owes its shortfall (the interface ran ahead by that much),
    // at most a 60th; once the carry reaches a whole 60th the update runs again. Until 2026-10-24 the frame's whole 60ths
    // were carried and only a whole number past 1 taken, so a frame just short of a 60th (0.99) banked all of itself and
    // the next just past one (1.01) made two: at a frame limiter's jitter about 60 FPS the interface ran twice every
    // other frame, jerking along with the frame rate steady (a player's "random stutter" in the macro editor).
    if (!g_sixty) {
        g_carry = 0.0, g_extra = 0;
        return;
    }
    g_carry += sixtieths - 1.0;
    if (g_carry < -1.0)
        g_carry = -1.0;
    int extra = 0;
    if (g_carry >= 1.0) {
        extra = static_cast<int>(std::floor(g_carry + 0.000001));
        g_carry -= extra;
    }
    g_extra = extra < kMostExtra ? extra : kMostExtra;
}

} // namespace

void install(const wchar_t* ini)
{
    QueryPerformanceFrequency(&g_frequency);
    g_ini = ini != nullptr ? ini : L"";
    if (g_ini.empty())
        return;
    g_ini_time = ini_time();
    read_settings();
    hold_framefix(any()); // before boot.txt's /plugin load framefix
}

void on_frame()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_last.QuadPart != 0 && g_frequency.QuadPart != 0) {
        const double ms = static_cast<double>(now.QuadPart - g_last.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
        if (ms > 0.001 && ms <= kLongest)
            time_frame(ms);
    }
    g_last = now;
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
    // FrameFix out before ours goes in; ours out before it may load again
    if (any())
        hold_framefix(true);
    apply();
    if (!any())
        hold_framefix(false);
}

} // namespace vanacore::framepacing
