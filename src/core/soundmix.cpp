#include "soundmix.hpp"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::soundmix {
namespace {

// The menus' sound call (FUN_10036d10, __thiscall, ret 8: the pan, the volume): sub esp,10h; movsx eax,word ptr [esp+14h];
// mov edx,[esp+18h]; mov [esp+14h],eax; fild dword ptr [esp+14h]; lea eax,[esp]; push edx; push eax; ...; call <the
// request>; add esp,10h; ret 8
constexpr char kUiPlay[] = "83 EC 10 0F BF 44 24 14 8B 54 24 18 89 44 24 14 DB 44 24 14 8D 44 24 00 52 50 C7 44 24 10 00 00 00 00 "
                           "D9 5C 24 08 C7 44 24 0C 00 00 00 00 C7 44 24 14 00 00 00 00 E8 ?? ?? ?? ?? 83 C4 10 C2 08 00";
constexpr std::ptrdiff_t kUiRequestCall = 0x36;
constexpr std::size_t kUiPrologue = 8; // sub esp,10h; movsx eax,word ptr [esp+14h]
// The request (FUN_10036d60, __thiscall, ret 8: its parameters, the volume): mov al,[<sound on>]; push esi; test al,al;
// mov esi,ecx
constexpr std::uint8_t kRequestStart[] = {0xA0, 0x00, 0x00, 0x00, 0x00, 0x56, 0x84, 0xC0, 0x8B, 0xF1};
constexpr std::size_t kRequestPrologue = 5; // mov al,[<sound on>]
// A playing sound's volume (FUN_10038280, __cdecl: the sound, the volume, the time): mov edx,[esp+4]; test edx,edx; jl
constexpr std::uint8_t kVolumeStart[] = {0x8B, 0x54, 0x24, 0x04, 0x85, 0xD2, 0x7C};
constexpr std::size_t kVolumePrologue = 6; // mov edx,[esp+4]; test edx,edx
// Each menu sound (fourteen, 0x60 apart): its sound looked up once, then played at its volume, the pan centred (40h):
// push ecx; mov eax,[<its sound>]; push esi; test eax,eax; jnz; ...; fld [<its volume>]; fmul [127]; call ftol; push eax;
// push 40h; mov ecx,esi; call <the menus' sound call>
constexpr char kMenuSound[] = "51 A1 ?? ?? ?? ?? 56 85 C0 75 2B A1 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8D 4C 24 08 6A ?? 8B 80 08 00 04 00 51 "
                              "8B 00 8B C8 E8 ?? ?? ?? ?? 8B 00 85 C0 A3 ?? ?? ?? ?? 74 21 8B 30 85 F6 74 1B D9 05 ?? ?? ?? ?? "
                              "D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 6A ?? 8B CE E8";
constexpr std::ptrdiff_t kMenuSoundCall = 0x52;
constexpr int kMenuSounds = 14;
// The world's two requests: one whose sound is kept at +1E0h of its owner (mov ecx,[ecx]; test ecx,ecx; jz; push eax; push
// edx; call <the request>; mov [esi+1E0h],eax; mov dword ptr [esi+1E4h],<3.0>), and the event's (lea ecx,[esp+4]; push eax;
// push ecx; mov ecx,esi; call <the request>; pop esi; add esp,10h; ret 18h); then the first's volume set (mov edx,
// [esi+1E0h]; push 0; push eax; push edx; call <a playing sound's volume>; add esp,0Ch; xor al,al; pop esi; add esp,20h; ret)
constexpr char kWorldKept[] = "8B 09 85 C9 74 0D 50 52 E8 ?? ?? ?? ?? 89 86 E0 01 00 00 C7 86 E4 01 00 00";
constexpr std::ptrdiff_t kWorldKeptCall = 8;
constexpr char kWorldEvent[] = "8D 4C 24 04 50 51 8B CE E8 ?? ?? ?? ?? 5E 83 C4 10 C2 18 00";
constexpr std::ptrdiff_t kWorldEventCall = 8;
constexpr char kWorldVolume[] = "8B 96 E0 01 00 00 6A 00 50 52 E8 ?? ?? ?? ?? 83 C4 0C 32 C0 5E 83 C4 20 C3";
constexpr std::ptrdiff_t kWorldVolumeCall = 10;

using UiPlay = int(__thiscall*)(void* self, int pan, int volume);
using Request = int(__thiscall*)(void* self, void* params, int volume);
using SetVolume = int(__cdecl*)(int sound, int volume, int time);

std::atomic<int> g_world{100};
std::atomic<int> g_ui{100};
bool g_tried = false;
UiPlay g_ui_play = nullptr;
Request g_request = nullptr;
SetVolume g_set_volume = nullptr;
std::uintptr_t g_menu_returns[kMenuSounds] = {}; // where the menu sounds' calls return to
std::uintptr_t g_kept_return = 0, g_event_return = 0, g_volume_return = 0;

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call_to(std::uintptr_t call, std::uintptr_t target)
{
    return *reinterpret_cast<const std::uint8_t*>(call) == 0xE8 && call_target(call) == target;
}

bool starts(std::uintptr_t at, const std::uint8_t* bytes, std::size_t size, std::size_t skip_from = 0, std::size_t skip = 0)
{
    const auto* code = reinterpret_cast<const std::uint8_t*>(at);
    for (std::size_t i = 0; i < size; ++i)
        if ((i < skip_from || i >= skip_from + skip) && code[i] != bytes[i])
            return false;
    return true;
}

int scaled(int volume, int share)
{
    if (share >= 100)
        return volume;
    const int v = std::clamp(volume, 0, 127);
    return (v * std::max(share, 0) + 50) / 100;
}

bool menu_sound(std::uintptr_t from)
{
    return std::find(std::begin(g_menu_returns), std::end(g_menu_returns), from) != std::end(g_menu_returns);
}

int __fastcall ui_hook(void* self, void*, int pan, int volume)
{
    const int share = g_ui.load(std::memory_order_relaxed);
    if (share < 100 && menu_sound(reinterpret_cast<std::uintptr_t>(_ReturnAddress())))
        volume = scaled(volume, share);
    return g_ui_play(self, pan, volume);
}

int __fastcall request_hook(void* self, void*, void* params, int volume)
{
    const int share = g_world.load(std::memory_order_relaxed);
    const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (share < 100 && (from == g_kept_return || from == g_event_return))
        volume = scaled(volume, share);
    return g_request(self, params, volume);
}

int __cdecl volume_hook(int sound, int volume, int time)
{
    const int share = g_world.load(std::memory_order_relaxed);
    if (share < 100 && sound > 0 && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_volume_return)
        volume = scaled(volume, share);
    return g_set_volume(sound, volume, time);
}

// The fourteen menu sounds, each a call of `ui`: where each call returns to. False unless there are exactly fourteen.
bool find_menu_sounds(HMODULE game, std::uintptr_t ui)
{
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    int found = 0;
    for (std::uintptr_t at = scan::find(game, kMenuSound); at != 0 && found <= kMenuSounds;
         at = scan::find(at + 1, base + size - (at + 1), kMenuSound)) {
        if (!is_call_to(at + kMenuSoundCall, ui))
            return false;
        if (found < kMenuSounds)
            g_menu_returns[found] = at + kMenuSoundCall + 5;
        ++found;
    }
    return found == kMenuSounds;
}

const wchar_t* find_all(HMODULE game, std::uintptr_t& ui, std::uintptr_t& request, std::uintptr_t& volume)
{
    ui = scan::find(game, kUiPlay);
    if (ui == 0)
        return L"the menus' sound call";
    request = call_target(ui + kUiRequestCall);
    if (!starts(request, kRequestStart, sizeof kRequestStart, 1, 4))
        return L"the sounds' request";
    const std::uintptr_t kept = scan::find(game, kWorldKept);
    const std::uintptr_t event = scan::find(game, kWorldEvent);
    if (kept == 0 || event == 0 || !is_call_to(kept + kWorldKeptCall, request) || !is_call_to(event + kWorldEventCall, request))
        return L"the world's sound requests";
    const std::uintptr_t set = scan::find(game, kWorldVolume);
    volume = set != 0 ? call_target(set + kWorldVolumeCall) : 0;
    if (set == 0 || !starts(volume, kVolumeStart, sizeof kVolumeStart))
        return L"the world's sound volume";
    if (!find_menu_sounds(game, ui))
        return L"the fourteen menu sounds";
    g_kept_return = kept + kWorldKeptCall + 5;
    g_event_return = event + kWorldEventCall + 5;
    g_volume_return = set + kWorldVolumeCall + 5;
    return nullptr;
}

void install()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr) {
        g_tried = false; // at a later call
        return;
    }
    std::uintptr_t ui = 0, request = 0, volume = 0;
    if (const wchar_t* missing = find_all(game, ui, request, volume)) {
        log::write(L"Sound: %ls isn't as VanaCore knows it in FFXiMain.dll: World and Interface Sounds Volume do nothing", missing);
        return;
    }
    // The menus' calls go through the request too: hooked first, so the request is never seen half done
    const bool hooked = imports::detour(reinterpret_cast<SetVolume>(volume), kVolumePrologue, &volume_hook, g_set_volume) &&
                        imports::detour(reinterpret_cast<Request>(request), kRequestPrologue, reinterpret_cast<Request>(&request_hook),
                                        g_request) &&
                        imports::detour(reinterpret_cast<UiPlay>(ui), kUiPrologue, reinterpret_cast<UiPlay>(&ui_hook), g_ui_play);
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    if (!hooked) {
        log::write(L"Sound: the game's sound calls couldn't be hooked: World and Interface Sounds Volume do nothing");
        return;
    }
    log::write(L"Sound: World and Interface Sounds Volume hooked (menus' sounds +0x%X, request +0x%X, volume +0x%X)",
               static_cast<unsigned>(ui - base), static_cast<unsigned>(request - base), static_cast<unsigned>(volume - base));
}

} // namespace

void set(int world, int ui)
{
    g_world.store(std::clamp(world, 0, 100), std::memory_order_relaxed);
    g_ui.store(std::clamp(ui, 0, 100), std::memory_order_relaxed);
    if (!g_tried && (world < 100 || ui < 100))
        install();
}

} // namespace vanacore::soundmix
