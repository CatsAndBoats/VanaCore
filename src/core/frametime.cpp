#include "frametime.hpp"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "d3d8.hpp"
#include "imports.hpp"
#include "log.hpp"

namespace vanacore::frametime {
namespace {

bool g_on = false;      // timing: [logging] frametimes, or a recording (/vana debug)
bool g_logging = false; // [logging] frametimes=1: a line every kLogSeconds
constexpr double kLogSeconds = 10.0;
bool g_sleeps_hooked = false;
void* g_device = nullptr; // the game's, for timing its methods when a recording turns timing on
void (*g_sink)(const Parts&) = nullptr;  // /vana debug's recording
constexpr int kWatchers = 4;
void (*g_watchers[kWatchers])(const Parts&) = {}; // /vana stats, /vana trace's timed trace
int g_profile_users = 0;                 // who has Direct3D 12's frame profile on
double g_ms_per_tick = 0.0; // QueryPerformanceCounter ticks to milliseconds
std::atomic<DWORD> g_render_thread{0};

LONGLONG now()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

// Sleep calls on the render thread, by the module that made them. Only the render thread writes.
struct Sleeper {
    HMODULE module = nullptr;
    unsigned calls = 0;
    double asked = 0.0; // ms
    double slept = 0.0; // ms
};
constexpr int kSleepers = 8;
Sleeper g_sleepers[kSleepers];
std::atomic<unsigned> g_other_calls{0}; // on other threads (network, sound...): counted only

decltype(&::Sleep) g_sleep = &::Sleep;
decltype(&::SleepEx) g_sleep_ex = &::SleepEx;

double g_frame_sleeps = 0.0; // this frame's, ms

void record(void* caller, DWORD asked, LONGLONG start)
{
    if (!g_on)
        return;
    if (GetCurrentThreadId() != g_render_thread.load(std::memory_order_relaxed)) {
        g_other_calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(caller), &module);
    for (Sleeper& sleeper : g_sleepers) {
        if (sleeper.module == module || sleeper.module == nullptr) {
            sleeper.module = module;
            ++sleeper.calls;
            sleeper.asked += asked;
            const double slept = static_cast<double>(now() - start) * g_ms_per_tick;
            sleeper.slept += slept;
            g_frame_sleeps += slept;
            return;
        }
    }
}

void WINAPI sleep(DWORD ms)
{
    const LONGLONG start = now();
    g_sleep(ms);
    record(_ReturnAddress(), ms, start);
}

DWORD WINAPI sleep_ex(DWORD ms, BOOL alertable)
{
    const LONGLONG start = now();
    const DWORD result = g_sleep_ex(ms, alertable);
    record(_ReturnAddress(), ms, start);
    return result;
}

void on_module(HMODULE module)
{
    if (imports::is_windows_module(module))
        return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&on_module), &self);
    if (module == self)
        return;
    for (const char* dll : {"KERNEL32.dll", "api-ms-win-core-synch-l1-2-0.dll", "api-ms-win-core-synch-l1-1-0.dll"}) {
        imports::hook(module, dll, "Sleep", &sleep, g_sleep);
        imports::hook(module, dll, "SleepEx", &sleep_ex, g_sleep_ex);
    }
}

// Windows' timer resolution now, in ms (what a Sleep rounds up to).
double timer_resolution()
{
    using Query = LONG(NTAPI*)(PULONG, PULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
    ULONG coarsest = 0, finest = 0, current = 0;
    if (query == nullptr || query(&coarsest, &finest, &current) != 0)
        return 0.0;
    return current / 10000.0;
}

std::wstring module_name(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return L"?";
    const wchar_t* name = wcsrchr(path, L'\\');
    return name != nullptr ? name + 1 : path;
}

// ---- Direct3D calls ----------------------------------------------------------------------------
//
// Each timed method's vtable slot points at a stub (push its index; jmp timed_common). The common
// code swaps the caller's return address for timed_leave and runs the method; timed_leave takes the
// time and goes back to the real return address. It works for any method without knowing its
// arguments (they're __stdcall: the method pops them). Only the game's calls on the render thread
// are timed (not VanaCore's own, inside its Present hook).

struct Timed {
    const char* name = nullptr;
    void* original = nullptr;
    unsigned calls = 0;
    double ms = 0.0;
};
constexpr int kMaxTimed = 128;
Timed g_timed[kMaxTimed];
int g_timed_count = 0;
BYTE* g_stubs = nullptr;
bool g_in_present = false; // inside VanaCore's Present hook: VanaCore's own calls
double g_direct3d = 0.0;   // this second's time in Direct3D (outermost calls), ms
double g_frame_direct3d = 0.0; // this frame's
double g_frame_slowest = 0.0;  // ... its slowest outermost call
int g_frame_slowest_index = -1;

struct Pending {
    void* return_address;
    int index;
    LONGLONG start;
};
// Calls in progress on this thread (a method calling another nests), cheap in Debug too.
constexpr int kMaxDepth = 16;
thread_local Pending t_pending[kMaxDepth];
thread_local int t_depth = 0;

extern "C" void __stdcall timed_leave();

extern "C" void* __stdcall timed_enter(int index, void** return_slot)
{
    Timed& timed = g_timed[index];
    if (!g_on || g_in_present || t_depth == kMaxDepth || GetCurrentThreadId() != g_render_thread.load(std::memory_order_relaxed))
        return timed.original;
    t_pending[t_depth++] = {*return_slot, index, now()};
    *return_slot = reinterpret_cast<void*>(&timed_leave);
    return timed.original;
}

extern "C" void* __stdcall timed_exit()
{
    const Pending pending = t_pending[--t_depth];
    const double ms = static_cast<double>(now() - pending.start) * g_ms_per_tick;
    Timed& timed = g_timed[pending.index];
    ++timed.calls;
    timed.ms += ms;
    if (t_depth == 0) {
        g_direct3d += ms;
        g_frame_direct3d += ms;
        if (ms > g_frame_slowest)
            g_frame_slowest = ms, g_frame_slowest_index = pending.index;
    }
    return pending.return_address;
}

__declspec(naked) void timed_common()
{
    __asm {
        pop eax            ; the stub's index; [esp] is now the caller's return address
        mov ecx, esp
        push ecx
        push eax
        call timed_enter   ; the method, the return address swapped for timed_leave
        jmp eax
    }
}

extern "C" __declspec(naked) void __stdcall timed_leave()
{
    __asm {
        push eax           ; the method's result
        push edx
        call timed_exit    ; the real return address
        mov ecx, eax
        pop edx
        pop eax
        jmp ecx
    }
}

// Times `vtable[slot]` as `name`. Skipped if it's timed already (a class shared by objects).
void time_slot(void** vtable, int slot, const char* name)
{
    if (g_timed_count >= kMaxTimed || g_stubs == nullptr)
        return;
    void* current = vtable[slot];
    const auto* at = static_cast<const BYTE*>(current);
    if (at >= g_stubs && at < g_stubs + kMaxTimed * 16)
        return;
    const int index = g_timed_count++;
    g_timed[index].name = name;
    g_timed[index].original = current;
    BYTE* stub = g_stubs + index * 16;
    stub[0] = 0x68; // push index
    *reinterpret_cast<INT32*>(stub + 1) = index;
    stub[5] = 0xE9; // jmp timed_common
    *reinterpret_cast<INT32*>(stub + 6) = static_cast<INT32>(reinterpret_cast<BYTE*>(&timed_common) - (stub + 10));
    DWORD protect = 0;
    if (VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &protect)) {
        vtable[slot] = stub;
        VirtualProtect(&vtable[slot], sizeof(void*), protect, &protect);
    }
}

constexpr const char* kDeviceMethods[] = {
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem", "ResourceManagerDiscardBytes",
    "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters", "SetCursorProperties", "SetCursorPosition",
    "ShowCursor", "CreateAdditionalSwapChain", "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetGammaRamp",
    "GetGammaRamp", "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer",
    "CreateRenderTarget", "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "UpdateTexture", "GetFrontBuffer",
    "SetRenderTarget", "GetRenderTarget", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform",
    "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "BeginStateBlock",
    "EndStateBlock", "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "SetClipStatus",
    "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice", "GetInfo",
    "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette", "DrawPrimitive",
    "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexShader",
    "SetVertexShader", "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant", "GetVertexShaderConstant",
    "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource", "SetIndices", "GetIndices",
    "CreatePixelShader", "SetPixelShader", "GetPixelShader", "DeletePixelShader", "SetPixelShaderConstant",
    "GetPixelShaderConstant", "GetPixelShaderFunction", "DrawRectPatch", "DrawTriPatch", "DeletePatch"};

// This second's frames, on the render thread.
LONGLONG g_window_start = 0;
LONGLONG g_frame_start = 0; // the last Present hook's start
LONGLONG g_mark = 0;
unsigned g_frames = 0;
double g_frame_min = 0, g_frame_max = 0, g_frame_sum = 0;
double g_vanacore = 0, g_limit = 0, g_present = 0;
double g_frame_vanacore = 0, g_frame_limit = 0, g_frame_present = 0; // the frame's

void report(double seconds)
{
    const double frames = static_cast<double>(g_frames);
    const double average = g_frame_sum / frames;
    double sleeping = 0.0;
    std::wstring sleeps;
    for (Sleeper& sleeper : g_sleepers) {
        if (sleeper.module == nullptr)
            break;
        sleeping += sleeper.slept;
        wchar_t part[200];
        swprintf_s(part, L"%ls%ls %.0f calls asking %.1f ms, slept %.1f ms", sleeps.empty() ? L"" : L"; ", module_name(sleeper.module).c_str(),
                   sleeper.calls / seconds, sleeper.asked / seconds, sleeper.slept / seconds);
        sleeps += part;
        sleeper = {};
    }
    const double direct3d = g_direct3d / frames;
    const double game = average - (g_vanacore + g_limit + g_present) / frames - sleeping / frames - direct3d;
    // The Direct3D methods that took the most, per frame.
    std::vector<Timed*> costly;
    for (int i = 0; i < g_timed_count; ++i)
        if (g_timed[i].calls > 0)
            costly.push_back(&g_timed[i]);
    std::sort(costly.begin(), costly.end(), [](const Timed* a, const Timed* b) { return a->ms > b->ms; });
    std::wstring methods;
    for (size_t i = 0; i < costly.size() && i < 8; ++i) {
        wchar_t part[120];
        swprintf_s(part, L"%ls%hs %.2f ms x%.0f", methods.empty() ? L"" : L", ", costly[i]->name, costly[i]->ms / frames,
                   costly[i]->calls / frames);
        methods += part;
    }
    for (int i = 0; i < g_timed_count; ++i) {
        g_timed[i].calls = 0;
        g_timed[i].ms = 0.0;
    }
    g_direct3d = 0.0;
    log::write(L"Frames: %.0f fps; frame %.1f ms (%.1f-%.1f); in it: game %.1f, its Direct3D calls %.1f, its sleeps %.1f, "
               L"VanaCore %.1f, FPS limit %.1f, Present %.1f ms; Direct3D per frame: %ls; sleeps on the render thread, per second: %ls; on other "
               L"threads: %.0f; system-wide timer %.2f ms",
               frames / seconds, average, g_frame_min, g_frame_max, game, direct3d, sleeping / frames, g_vanacore / frames,
               g_limit / frames, g_present / frames, methods.empty() ? L"not timed" : methods.c_str(),
               sleeps.empty() ? L"none" : sleeps.c_str(), g_other_calls.exchange(0, std::memory_order_relaxed) / seconds, timer_resolution());
    g_frames = 0;
    g_frame_min = g_frame_max = g_frame_sum = 0;
    g_vanacore = g_limit = g_present = 0;
}

} // namespace

void time_device(void* device);

void install(const wchar_t* ini)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_ms_per_tick = 1000.0 / static_cast<double>(frequency.QuadPart);
    g_logging = ini != nullptr && ini[0] != L'\0' && GetPrivateProfileIntW(L"logging", L"frametimes", 0, ini) != 0;
    g_on = g_logging;
    if (!g_on)
        return;
    imports::for_each_module(&on_module);
    g_sleeps_hooked = true;
    log::write(L"Frames: [logging] frametimes=1: a line every 10 seconds (system-wide timer now %.2f ms; this process's own "
               L"steps show as a sleep's ms slept against ms asked)", timer_resolution());
}

void on_device(void* device)
{
    if (device == nullptr)
        return;
    g_device = device;
    if (g_on)
        time_device(device);
}

static bool watching()
{
    for (auto* watcher : g_watchers)
        if (watcher != nullptr)
            return true;
    return false;
}

// Timing on while anything wants the parts of the frames (a recording, the graph) or [logging] frametimes does
static void update_timing()
{
    if (g_sink != nullptr || watching()) {
        if (!g_sleeps_hooked) {
            imports::for_each_module(&on_module);
            g_sleeps_hooked = true;
        }
        if (g_timed_count == 0 && g_device != nullptr)
            time_device(g_device);
        g_on = true;
    } else {
        g_on = g_logging;
    }
}

void record(void (*sink)(const Parts& parts))
{
    g_sink = sink;
    update_timing();
}

void watch(void (*sink)(const Parts& parts))
{
    for (auto*& watcher : g_watchers)
        if (watcher == sink)
            return;
    for (auto*& watcher : g_watchers)
        if (watcher == nullptr) {
            watcher = sink;
            break;
        }
    update_timing();
}

void unwatch(void (*sink)(const Parts& parts))
{
    for (auto*& watcher : g_watchers)
        if (watcher == sink)
            watcher = nullptr;
    update_timing();
}

int take_methods(Method* out, int max)
{
    int n = 0;
    for (int i = 0; i < g_timed_count; ++i) {
        if (g_timed[i].calls == 0)
            continue;
        if (n < max)
            out[n++] = {g_timed[i].name, g_timed[i].ms, g_timed[i].calls};
        g_timed[i].calls = 0;
        g_timed[i].ms = 0.0;
    }
    return n;
}

void profile(bool on)
{
    if (on) {
        if (g_profile_users++ == 0)
            vanacore_gfx_record(1);
    } else if (g_profile_users > 0 && --g_profile_users == 0) {
        vanacore_gfx_record(0);
    }
}

void time_device(void* device)
{
    if (g_stubs == nullptr)
        g_stubs = static_cast<BYTE*>(VirtualAlloc(nullptr, kMaxTimed * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    auto** vtable = *static_cast<void***>(device);
    for (int slot = 3; slot < static_cast<int>(std::size(kDeviceMethods)); ++slot)
        if (slot != d3d8::slot::Present) // VanaCore's own hook, timed as Present
            time_slot(vtable, slot, kDeviceMethods[slot]);
    // Buffers' and textures' locks, where waiting for the graphics card would show: their classes'
    // vtables, from throwaway ones (one of each kind the game makes).
    using d3d8::call;
    namespace slot = d3d8::slot;
    void* object = nullptr;
    // CreateVertexBuffer(length, usage, fvf, pool, out) and CreateIndexBuffer(length, usage, format, pool, out): managed,
    // and dynamic in the default pool.
    for (const auto [usage, pool] : {std::pair<DWORD, DWORD>{0, 1}, std::pair<DWORD, DWORD>{0x208, 0}}) {
        object = nullptr;
        if (SUCCEEDED(call(device, 23, 64u, usage, DWORD{0x42}, pool, &object)) && object != nullptr) {
            time_slot(*static_cast<void***>(object), 11, usage != 0 ? "VertexBuffer::Lock (dynamic)" : "VertexBuffer::Lock");
            time_slot(*static_cast<void***>(object), 12, "VertexBuffer::Unlock");
            call<ULONG>(object, slot::Release);
        }
        object = nullptr;
        if (SUCCEEDED(call(device, 24, 64u, usage, DWORD{101}, pool, &object)) && object != nullptr) {
            time_slot(*static_cast<void***>(object), 11, usage != 0 ? "IndexBuffer::Lock (dynamic)" : "IndexBuffer::Lock");
            time_slot(*static_cast<void***>(object), 12, "IndexBuffer::Unlock");
            call<ULONG>(object, slot::Release);
        }
    }
    object = nullptr;
    if (SUCCEEDED(call(device, slot::CreateTexture, 4u, 4u, 1u, DWORD{0}, d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged, &object)) &&
        object != nullptr) {
        time_slot(*static_cast<void***>(object), slot::TextureLockRect, "Texture::LockRect");
        time_slot(*static_cast<void***>(object), slot::TextureUnlockRect, "Texture::UnlockRect");
        call<ULONG>(object, slot::Release);
    }
    log::write(L"Frames: timing %d Direct3D methods of the game's", g_timed_count);
}

void frame_start()
{
    if (!g_on)
        return;
    g_in_present = true;
    const LONGLONG start = now();
    if (g_render_thread.load(std::memory_order_relaxed) == 0)
        g_render_thread = GetCurrentThreadId();
    if (g_frame_start != 0 && (g_sink != nullptr || watching())) {
        Parts parts;
        parts.frame = static_cast<double>(start - g_frame_start) * g_ms_per_tick;
        parts.direct3d = g_frame_direct3d, parts.sleeps = g_frame_sleeps;
        parts.vanacore = g_frame_vanacore, parts.limit = g_frame_limit, parts.present = g_frame_present;
        parts.slowest = g_frame_slowest;
        parts.slowest_name = g_frame_slowest_index >= 0 ? g_timed[g_frame_slowest_index].name : "";
        if (g_sink != nullptr)
            g_sink(parts);
        for (auto* watcher : g_watchers)
            if (watcher != nullptr)
                watcher(parts);
    }
    g_frame_direct3d = g_frame_sleeps = g_frame_vanacore = g_frame_limit = g_frame_present = g_frame_slowest = 0.0;
    g_frame_slowest_index = -1;
    if (!g_logging) {
        g_frame_start = start;
        g_mark = start;
        return;
    }
    if (g_frame_start != 0) {
        const double frame = static_cast<double>(start - g_frame_start) * g_ms_per_tick;
        g_frame_min = g_frames == 0 ? frame : std::min(g_frame_min, frame);
        g_frame_max = std::max(g_frame_max, frame);
        g_frame_sum += frame;
        ++g_frames;
    } else {
        g_window_start = start;
    }
    g_frame_start = start;
    g_mark = start;
    const double seconds = static_cast<double>(start - g_window_start) * g_ms_per_tick / 1000.0;
    if (seconds >= kLogSeconds && g_frames > 0) {
        report(seconds);
        g_window_start = start;
    }
}

void limit_start()
{
    if (!g_on || g_mark == 0)
        return;
    const LONGLONG at = now();
    const double ms = static_cast<double>(at - g_mark) * g_ms_per_tick;
    g_vanacore += ms, g_frame_vanacore += ms;
    g_mark = at;
}

void limit_end()
{
    if (!g_on || g_mark == 0)
        return;
    const LONGLONG at = now();
    const double ms = static_cast<double>(at - g_mark) * g_ms_per_tick;
    g_limit += ms, g_frame_limit += ms;
    g_mark = at;
}

void present_start()
{
    if (!g_on || g_mark == 0)
        return;
    const LONGLONG at = now();
    const double ms = static_cast<double>(at - g_mark) * g_ms_per_tick;
    g_vanacore += ms, g_frame_vanacore += ms;
    g_mark = at;
}

void present_end()
{
    if (!g_on)
        return;
    g_in_present = false;
    if (g_mark == 0)
        return;
    const double ms = static_cast<double>(now() - g_mark) * g_ms_per_tick;
    g_present += ms, g_frame_present += ms;
    g_mark = 0;
}

} // namespace vanacore::frametime
