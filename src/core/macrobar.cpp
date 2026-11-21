#include "macrobar.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "addons.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "menulog.hpp"
#include "outline.hpp"
#include "scan.hpp"
#include "window.hpp"

namespace vanacore::macrobar {
namespace {

// The macro controller's key handling (FsMacroController::onKeyDown in the PS2 client; VA 0x101A15E0 in FFXiMain.dll),
// once a frame. For each of the controller's two palette keys (0x89 and 0x8A of group 0x3E, asked of GetKey 0x10123670;
// the triggers, through the game's own handlers for the two keys): a tap (trigger 1) toggles the key's palette
// ("menu    mcr1pall" / "mcr2pall"), and a hold (trigger 4) opens it once the game's second counter (0x101035E0) has
// moved on since the hold began (the first held frame records the counter at +0x14, which the frame with neither key
// held clears), with "pad open" (+0x18) on. The keyboard's Ctrl and Alt (keys 0x76 and 0x77, trigger 4: "book" keys
// below) have paths of their own under them: the same wait (their held frames counted at +0x10, cleared the same way),
// then the palette opened with "pad open" off, so the branch that runs while neither is held closes it when the key is
// let go. The palette up is at +0x0C (0 none, 1 the first key's, 2 the second's); the palette's own window puts it back
// to 0 as it goes on Esc or a macro chosen in it.
//
// [client] macrobar.nodelay=1 in the game's own mode takes out the two waits alone (the keyboard's Ctrl and Alt: their
// paths only hold, nothing there taps), so a held Ctrl or Alt shows the palette on the second frame while the triggers
// stay as the game has them (a tap toggles, a hold waits). Hold and toggle take the waits out as part of what they do.
//
// [client] macrobar.mode=hold does what the macrofix addon (atom0s) does, in six patches: the two tap branches are
// skipped (their je made a jmp), the two hold branches are sent into the book keys' paths (their mov ecx,esi; call
// _canUseMacro, 7 bytes, made jmp <the book path's same instruction>; int3; int3), and the two waits in the book paths
// are removed (the jb after cmp eax,ebx made nops). A held key then opens its palette on the second frame of the hold
// and the palette goes on the first frame without it; a tap does nothing on its own.
//
// macrobar.mode=toggle: a press of the key shows its palette, which stays up until the key is pressed again (or Esc).
// The tap branches are skipped as above; the hold branches and the book paths ask press() in place of _canUseMacro,
// which toggles the palette on the first held frame (the game's +0x14, or the book paths' +0x10, is 0 then; press()
// puts 1 there, and the game's own no-key frame clears it, so the next press is a first frame again) and says yes, so
// the game goes on: the hold path to its wait, whose jb is made a jmp, and a book path past itself (its "this palette
// is up already" je made a jmp) to the macro keys. Neither opens a palette of its own.

// add esp,10h; test al,al; je; mov ecx,esi; call _canUseMacro; test al,al; je; mov al,[esi+0Ch]; mov ecx,<manager>;
// cmp al,bl; je; push <the other palette's name>; mov [esi+18h],bl; call _destroy; push 0; push ebx; push <this palette's
// name>; mov ecx,<manager>; call _create; mov [esi+0Ch],bl: the first key's tap, and the manager, the names and the
// window calls press() needs
constexpr char kTapOne[] = "83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 8A 46 0C B9 ?? ?? ?? ?? 3A C3 74 ?? 68 ?? ?? ?? ?? "
                           "88 5E 18 E8 ?? ?? ?? ?? 6A 00 53 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 88 5E 0C";
constexpr std::ptrdiff_t kTapManager = 22, kTapOtherName = 31, kTapDestroy = 38, kTapName = 47, kTapCreate = 56;
// ... mov al,[esi+0Ch] -> cmp byte ptr [esi+0Ch],2: the second key's tap
constexpr char kTapTwo[] = "83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 80 7E 0C 02";
constexpr std::ptrdiff_t kTapJump = 5; // the je
constexpr std::ptrdiff_t kTapCall = 9; // the call _canUseMacro
// add esp,10h; test al,al; je; mov ecx,esi; call _canUseMacro; test al,al; je; mov al,[esi+0Ch]; test al,al; jne; mov
// eax,[esi+14h]; test eax,eax; jne; call _seconds; mov [esi+14h],eax; jmp; call _seconds; sub eax,[esi+14h]; cmp eax,ebx;
// jb; mov [esi+18h],bl: the first key's hold (first match) and the second's (second)
constexpr char kHold[] = "83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 8A 46 0C 84 C0 75 ?? 8B 46 14 85 C0 75 ?? E8 ?? ?? ?? ?? "
                         "89 46 14 EB ?? E8 ?? ?? ?? ?? 2B 46 14 3B C3 72 ?? 88 5E 18";
constexpr std::ptrdiff_t kHoldCall = 7;  // mov ecx,esi; call _canUseMacro
constexpr std::ptrdiff_t kHoldWait = 52; // the jb (the second counter hasn't moved on)
// push -1; push 4; push 76h / 77h; push 3Eh; call GetKey; add esp,10h; test al,al; je; mov ecx,esi; call _canUseMacro
// ... test al,al; je <end>; cmp byte ptr [esi+0Ch],bl / cmp byte ptr [esi+0Ch],2; je <the macro keys>
constexpr char kBookOne[] = "6A FF 6A 04 6A 76 6A 3E E8 ?? ?? ?? ?? 83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? "
                            "38 5E 0C 0F 84";
constexpr char kBookTwo[] = "6A FF 6A 04 6A 77 6A 3E E8 ?? ?? ?? ?? 83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? "
                            "80 7E 0C 02 0F 84";
constexpr std::ptrdiff_t kBookCall = 20;                          // its mov ecx,esi; call _canUseMacro: where the hold branches are sent
constexpr std::ptrdiff_t kBookStateOne = 38, kBookStateTwo = 39; // the je past the path when its palette is up already
// sub eax,[esi+10h]; cmp eax,ebx; jb (near, then short); push <the palette's name>; mov ecx,<manager>
constexpr char kWaitLong[] = "2B 46 10 3B C3 0F 82 ?? ?? ?? ?? 68 ?? ?? ?? ?? B9";
constexpr char kWaitShort[] = "2B 46 10 3B C3 72 ?? 68 ?? ?? ?? ?? B9";
constexpr std::ptrdiff_t kWaitJump = 5; // the jb

enum Mode { kModeGame = 0, kModeHold = 1, kModeToggle = 2 };
constexpr unsigned kWithNoDelay = 1u << 3; // a site's modes bit: patched in any mode while macrobar.nodelay is on

struct Site {
    std::uintptr_t at = 0;
    std::size_t size = 0;
    std::uint8_t original[7] = {};
    std::uint8_t patched[7] = {};
    unsigned modes = 0; // bit 1 << Mode: the modes this site is patched in (kWithNoDelay: and with macrobar.nodelay on)
};

int g_mode = kModeGame;      // [client] macrobar.mode
int g_applied = kModeGame;   // the mode the sites hold now
bool g_nodelay = false;      // [client] macrobar.nodelay
bool g_nodelay_applied = false;
bool g_searched = false; // FFXiMain.dll was looked through (g_count != 0: found)
Site g_sites[14];
std::size_t g_count = 0;

// _canUseMacro (thiscall, no arguments: fastcall's ecx, edx unused), and the menu manager's _destroy(name) and
// _create(name, 1, 0) (thiscall on the manager)
using CanUse = bool(__fastcall*)(void*, void*);
using Destroy = void(__fastcall*)(void*, void*, const char*);
using Create = void(__fastcall*)(void*, void*, const char*, int, int);
CanUse g_can_use = nullptr;
Destroy g_destroy = nullptr;
Create g_create = nullptr;
void* g_manager = nullptr;
const char* g_names[2] = {nullptr, nullptr}; // the palettes' window names, keys 1 and 2
// mode=toggle, Alt+Tab: the game going to the background takes the bar down (Alt opened it as it went), and a key still
// held as the game comes back in front (the Alt of an Alt+Tab back to it) isn't a press for half a second
void* g_controller = nullptr;   // the macro controller, from the last press
bool g_background = false;      // the game was in the background last frame
std::uint32_t g_front_at = 0;   // GetTickCount (never 0) when the game last came back in front

// In place of _canUseMacro in the hold branch of the key of palette N (every frame the key is held), mode=toggle: on the
// first frame of the press, the palette toggled (the other closed if it was up). Yes to the game, whose hold path then
// only waits (never opens: its jb is a jmp).
template <std::uint8_t N, std::size_t Held>
bool __fastcall press(void* self, void* edx)
{
    if (!g_can_use(self, edx))
        return false;
    auto* controller = static_cast<std::uint8_t*>(self);
    g_controller = controller;
    std::uint32_t held = 0;
    std::memcpy(&held, controller + Held, sizeof held);
    if (held != 0)
        return true;
    held = 1;
    std::memcpy(controller + Held, &held, sizeof held);
    if (g_front_at != 0 && GetTickCount() - g_front_at < 500) // held from an Alt+Tab back to the game: not a press
        return true;
    if (controller[0x0C] == N) {
        controller[0x0C] = 0;
        controller[0x18] = 0;
        g_destroy(g_manager, nullptr, g_names[N - 1]);
    } else {
        controller[0x18] = 1;
        g_destroy(g_manager, nullptr, g_names[2 - N]);
        g_create(g_manager, nullptr, g_names[N - 1], 1, 0);
        controller[0x0C] = N;
    }
    return true;
}

// A site: `at`, its `size` bytes as the game has them, and `patched` to put there in the `modes`
bool site(Site& s, std::uintptr_t at, std::size_t size, const std::uint8_t* patched, const std::uint8_t* expected,
          std::size_t expected_size, unsigned modes, const wchar_t* what, std::uintptr_t base)
{
    s.at = at;
    s.size = size;
    s.modes = modes;
    std::memcpy(s.original, reinterpret_cast<const void*>(at), size);
    std::memcpy(s.patched, patched, size);
    if (std::memcmp(s.original, expected, expected_size) != 0) {
        log::write(L"Macro bar: the game's %ls at +0x%X isn't as expected (%02X %02X; the macrofix addon may have changed it already): "
                   L"[client] macrobar.mode does nothing",
                   what, static_cast<unsigned>(at - base), s.original[0], s.original[1]);
        return false;
    }
    return true;
}

// A call's 5 bytes to `target` from `at`
void call_to(std::uint8_t* bytes, std::uintptr_t at, std::uintptr_t target)
{
    const auto rel = static_cast<std::int32_t>(target - (at + 5));
    bytes[0] = 0xE8;
    std::memcpy(bytes + 1, &rel, 4);
}

// Where the call at `at` goes
std::uintptr_t call_target(std::uintptr_t at)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), 4);
    return at + 5 + static_cast<std::uintptr_t>(rel);
}

// The 32-bit immediate at `at`
std::uintptr_t imm32(std::uintptr_t at)
{
    std::uintptr_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(at), 4);
    return value;
}

// The sites, the first time they're wanted once FFXiMain.dll is there
bool found()
{
    if (g_searched)
        return g_count != 0;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t image = nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t tap_one = scan::find(game, kTapOne);
    const std::uintptr_t tap_two = scan::find(game, kTapTwo);
    const std::uintptr_t hold_one = scan::find(base, image, kHold, 0);
    const std::uintptr_t hold_two = scan::find(base, image, kHold, 1);
    const std::uintptr_t book_one = scan::find(game, kBookOne);
    const std::uintptr_t book_two = scan::find(game, kBookTwo);
    const std::uintptr_t wait_long = scan::find(game, kWaitLong);
    const std::uintptr_t wait_short = scan::find(game, kWaitShort);
    if (tap_one == 0 || tap_two == 0 || hold_one == 0 || hold_two == 0 || book_one == 0 || book_two == 0 || wait_long == 0 ||
        wait_short == 0) {
        log::write(L"Macro bar: the game's macro key handling wasn't found in FFXiMain.dll (%d%d%d%d%d%d%d%d; the macrofix addon "
                   L"may have changed it already): [client] macrobar.mode does nothing",
                   tap_one != 0, tap_two != 0, hold_one != 0, hold_two != 0, book_one != 0, book_two != 0, wait_long != 0,
                   wait_short != 0);
        return false;
    }
    // The hold branches' jumps: to the book paths' own mov ecx,esi; call _canUseMacro (the same 7 bytes they replace)
    std::uint8_t jump_one[7] = {0xE9, 0, 0, 0, 0, 0xCC, 0xCC}, jump_two[7] = {0xE9, 0, 0, 0, 0, 0xCC, 0xCC};
    const auto rel_one = static_cast<std::int32_t>((book_one + kBookCall) - (hold_one + kHoldCall + 5));
    const auto rel_two = static_cast<std::int32_t>((book_two + kBookCall) - (hold_two + kHoldCall + 5));
    std::memcpy(jump_one + 1, &rel_one, 4);
    std::memcpy(jump_two + 1, &rel_two, 4);
    // The hold branches' calls made press()'s
    std::uint8_t press_one[5], press_two[5];
    call_to(press_one, hold_one + kHoldCall + 2, reinterpret_cast<std::uintptr_t>(&press<1, 0x14>));
    call_to(press_two, hold_two + kHoldCall + 2, reinterpret_cast<std::uintptr_t>(&press<2, 0x14>));
    // The book paths' calls made press()'s too (their held frames counted at +0x10), and their "this palette is up
    // already" je made a jmp (0F 84 made 90 E9: the same four bytes of distance), so they never open one of their own
    std::uint8_t book_press_one[5], book_press_two[5];
    call_to(book_press_one, book_one + kBookCall + 2, reinterpret_cast<std::uintptr_t>(&press<1, 0x10>));
    call_to(book_press_two, book_two + kBookCall + 2, reinterpret_cast<std::uintptr_t>(&press<2, 0x10>));
    constexpr std::uint8_t kJmp[1] = {0xEB}, kJe[1] = {0x74}, kCall[3] = {0x8B, 0xCE, 0xE8}, kJbLong[2] = {0x0F, 0x82},
                           kJbShort[1] = {0x72}, kE8[1] = {0xE8};
    constexpr std::uint8_t kNops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    constexpr unsigned kInHold = 1u << kModeHold, kInToggle = 1u << kModeToggle, kInBoth = kInHold | kInToggle;
    constexpr std::uint8_t kJeNear[2] = {0x0F, 0x84}, kJmpNear[2] = {0x90, 0xE9};
    Site sites[14];
    const bool ok = site(sites[0], tap_one + kTapJump, 1, kJmp, kJe, 1, kInBoth, L"first tap branch", base) &&
                    site(sites[1], tap_two + kTapJump, 1, kJmp, kJe, 1, kInBoth, L"second tap branch", base) &&
                    site(sites[2], hold_one + kHoldCall, 7, jump_one, kCall, 3, kInHold, L"first hold branch", base) &&
                    site(sites[3], hold_two + kHoldCall, 7, jump_two, kCall, 3, kInHold, L"second hold branch", base) &&
                    site(sites[4], wait_long + kWaitJump, 6, kNops, kJbLong, 2, kInBoth | kWithNoDelay, L"first book key wait", base) &&
                    site(sites[5], wait_short + kWaitJump, 2, kNops, kJbShort, 1, kInBoth | kWithNoDelay, L"second book key wait", base) &&
                    site(sites[6], hold_one + kHoldCall + 2, 5, press_one, kE8, 1, kInToggle, L"first hold call", base) &&
                    site(sites[7], hold_two + kHoldCall + 2, 5, press_two, kE8, 1, kInToggle, L"second hold call", base) &&
                    site(sites[8], hold_one + kHoldWait, 1, kJmp, kJbShort, 1, kInToggle, L"first hold wait", base) &&
                    site(sites[9], hold_two + kHoldWait, 1, kJmp, kJbShort, 1, kInToggle, L"second hold wait", base) &&
                    site(sites[10], book_one + kBookCall + 2, 5, book_press_one, kE8, 1, kInToggle, L"first book key call", base) &&
                    site(sites[11], book_two + kBookCall + 2, 5, book_press_two, kE8, 1, kInToggle, L"second book key call", base) &&
                    site(sites[12], book_one + kBookStateOne, 2, kJmpNear, kJeNear, 2, kInToggle, L"first book key palette test", base) &&
                    site(sites[13], book_two + kBookStateTwo, 2, kJmpNear, kJeNear, 2, kInToggle, L"second book key palette test", base);
    if (!ok)
        return false;
    g_can_use = reinterpret_cast<CanUse>(call_target(tap_one + kTapCall));
    g_manager = reinterpret_cast<void*>(imm32(tap_one + kTapManager));
    g_names[0] = reinterpret_cast<const char*>(imm32(tap_one + kTapName));
    g_names[1] = reinterpret_cast<const char*>(imm32(tap_one + kTapOtherName));
    g_destroy = reinterpret_cast<Destroy>(call_target(tap_one + kTapDestroy));
    g_create = reinterpret_cast<Create>(call_target(tap_one + kTapCreate));
    if (g_can_use != reinterpret_cast<CanUse>(call_target(tap_two + kTapCall)) ||
        g_manager != reinterpret_cast<void*>(imm32(tap_one + kTapCreate - 4)) || g_manager == nullptr) {
        log::write(L"Macro bar: the game's tap branches aren't as expected: [client] macrobar.mode does nothing");
        return false;
    }
    for (std::size_t i = 0; i < 14; ++i)
        g_sites[i] = sites[i];
    g_count = 14;
    log::write(L"Macro bar: the game's macro key handling found at +0x%X", static_cast<unsigned>(tap_one - base));
    return true;
}

void write(std::uintptr_t at, const std::uint8_t* bytes, std::size_t size)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Macro bar: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(at), GetLastError());
        return;
    }
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(at), size);
}

// The sites as the setting wants them (on the game's thread, outside the key handling): the game's own bytes back, then
// the mode's
void apply()
{
    if ((g_mode == g_applied && g_nodelay == g_nodelay_applied) || !found())
        return;
    for (std::size_t i = 0; i < g_count; ++i)
        write(g_sites[i].at, g_sites[i].original, g_sites[i].size);
    for (std::size_t i = 0; i < g_count; ++i)
        if ((g_sites[i].modes & (1u << g_mode)) != 0 || (g_nodelay && (g_sites[i].modes & kWithNoDelay) != 0))
            write(g_sites[i].at, g_sites[i].patched, g_sites[i].size);
    g_applied = g_mode;
    g_nodelay_applied = g_nodelay;
    if (g_mode == kModeToggle)
        log::write(L"Macro bar: a press shows it, until the next ([client] macrobar.mode=toggle)");
    else if (g_mode == kModeHold)
        log::write(L"Macro bar: up while the key is held, at once ([client] macrobar.mode=hold)");
    else if (g_nodelay)
        log::write(L"Macro bar: Ctrl and Alt show it at once, the triggers as the game has them ([client] macrobar.nodelay=1)");
    else
        log::write(L"Macro bar: the game's own keys again ([client] macrobar.mode=game)");
}

// ---- Where the bar is (macrobar.move) ----------------------------------------------------------------------------------
// Either palette is a window of the menu manager, its layout named "menu    mcr1pall" / "mcr2pall", or "mcr1long" /
// "mcr2long" (the manager's name resolver swaps them by a setting): 16 characters at the layout's +0x46, a window's +4
// being its layout. The layout is the window's own copy, made at each open from the game's data with the place it opens at
// (16, 48: the top left), so the window is put where it's wanted once it's up, as targetwin.cpp puts the Target window.
// Found on its own, not by the key handling's patterns, which the macrofix addon changes.
//
// Put there once a frame (at Present) alone, a palette opened in the game's frame was drawn once at the top left before
// it moved: each press of a spammed key showed it there first. So the manager's open of a menu by name (_create above)
// is detoured while macrobar.move is on, and a palette is put where it goes as it opens, before the game draws it, as
// the game itself places the item details window (SetLocate) as it opens. Every palette in the list is put, not only the
// first: a palette that's going can still be there beside the one coming.
constexpr char kPalettes[4][16 + 1] = {"menu    mcr1pall", "menu    mcr2pall", "menu    mcr1long", "menu    mcr2long"};
// The first tap branch (kTapOne) with its je left open (macrobar.mode and the macrofix addon make it a jmp): its call of
// the manager's open, and the manager it's called on
constexpr char kOpenAt[] = "83 C4 10 84 C0 ?? ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 8A 46 0C B9 ?? ?? ?? ?? 3A C3 74 ?? 68 ?? ?? ?? ?? "
                           "88 5E 18 E8 ?? ?? ?? ?? 6A 00 53 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 88 5E 0C";
// The manager's global (as targetwin.cpp): push ebx; push ebp; push esi; mov ebp,ecx; push edi; mov ecx,<the manager>; call
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8";
constexpr std::ptrdiff_t kManagerAt = 7;
// SetLocate (FUN_10118d10: thiscall window, x, y), matched past its first 8 bytes (itemlist.cpp's detour prologue)
constexpr char kSetLocate[] = "53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54";
constexpr std::ptrdiff_t kSetLocatePrologue = 8;
// The manager: its windows' list at +0 (a node: +0 the next, +0x10 its window, +0x14 set to skip), the interface's area
// (left, top, right, bottom, 16 bits each) at +0x7C. A window: +0x3A its frame, +0x42 its place as opened, +0x70 to be deleted.
constexpr std::ptrdiff_t kAreaLeft = 0x7C, kWinLayout = 4, kLayoutName = 0x46, kWinFrame = 0x3A, kWinOpened = 0x42,
                         kWinDeleting = 0x70;

using SetLocate = void(__thiscall*)(void* window, int x, int y);
// The manager's open (Create's: thiscall on the manager, name, 1, 0), what it gives back kept for the caller
using Open = std::uintptr_t(__fastcall*)(void* manager, void* edx, const char* name, int a, int b);

bool g_move = false;   // [client] macrobar.move
double g_x = 0.8;      // macrobar.x: the bar's left edge, percent of the interface's width
double g_y = 4;        // macrobar.y: its top edge, percent of its height

bool g_place_searched = false;
char* g_place_manager = nullptr;
SetLocate g_set_locate = nullptr;
bool g_placed = false;              // a palette was put somewhere (put back where it opened when macrobar.move goes off)
int g_seen_w = 862, g_seen_h = 42;  // the bar's size as last seen, for its box while it isn't up
bool g_open_tried = false;          // the manager's open was looked for and detoured (g_open: done)
Open g_open = nullptr;              // the game's open, past the detour
bool g_open_logged = false;         // whether a palette was there to put as it opened, logged once

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

bool place_found()
{
    if (g_place_searched)
        return g_set_locate != nullptr;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_place_searched = true;
    const std::uintptr_t help = scan::find(game, kHelp);
    const std::uintptr_t match = scan::find(game, kSetLocate);
    if (help != 0 && match != 0) {
        const auto* start = reinterpret_cast<const std::uint8_t*>(match - kSetLocatePrologue);
        if (start[0] == 0x83 || start[0] == 0xE9) {
            g_place_manager = reinterpret_cast<char*>(imm32(help + kManagerAt));
            g_set_locate = reinterpret_cast<SetLocate>(match - kSetLocatePrologue);
        }
    }
    if (g_set_locate == nullptr)
        log::write(L"Macro bar: the game's window placing wasn't found in FFXiMain.dll: [client] macrobar.move does nothing");
    return g_set_locate != nullptr;
}

// Whether `name` (16 characters) is a palette's
bool is_palette(const char* name)
{
    for (const auto& palette : kPalettes)
        if (std::memcmp(name, palette, 16) == 0)
            return true;
    return false;
}

// The palettes that are up in the manager's list, the first `most` of them into `found`: how many
int find_palettes(char** found, int most)
{
    int count = 0, seen = 0;
    for (char* node = at<char*>(g_place_manager, 0); node != nullptr && seen < 512 && count < most; node = at<char*>(node, 0), ++seen) {
        char* window = at<char*>(node, 0x10);
        if (window == nullptr || at<std::uint8_t>(node, 0x14) != 0 || at<std::uint8_t>(window, kWinDeleting) != 0)
            continue;
        const char* layout = at<char*>(window, kWinLayout);
        if (layout != nullptr && is_palette(layout + kLayoutName))
            found[count++] = window;
    }
    return count;
}

// The palette that's up, or null
char* find_palette()
{
    char* window = nullptr;
    find_palettes(&window, 1);
    return window;
}

// The interface's area; false while it isn't known
bool area(int& left, int& top, int& width, int& height)
{
    left = at<short>(g_place_manager, kAreaLeft);
    top = at<short>(g_place_manager, kAreaLeft + 2);
    width = at<short>(g_place_manager, kAreaLeft + 4) - left;
    height = at<short>(g_place_manager, kAreaLeft + 6) - top;
    return width > 0 && height > 0;
}

// Where macrobar.x and .y put a bar `wide` x `high`, kept on the screen
void wanted(int wide, int high, int& x, int& y)
{
    int left = 0, top = 0, width = 0, height = 0;
    area(left, top, width, height);
    x = std::clamp(left + static_cast<int>(std::lround(width * g_x / 100.0)), left, std::max(left, left + width - wide));
    y = std::clamp(top + static_cast<int>(std::lround(height * g_y / 100.0)), top, std::max(top, top + height - high));
}

// A palette put where macrobar.x and .y say, or (switched off while it's up) back where it opened; false while its
// size or the interface's isn't known
bool place(char* window)
{
    const int left = at<short>(window, kWinFrame), top = at<short>(window, kWinFrame + 2);
    const int wide = at<short>(window, kWinFrame + 4) - left, high = at<short>(window, kWinFrame + 6) - top;
    if (wide <= 0 || high <= 0)
        return false;
    g_seen_w = wide, g_seen_h = high;
    int x = at<short>(window, kWinOpened), y = at<short>(window, kWinOpened + 2);
    if (g_move) {
        int area_left, area_top, width, height;
        if (!area(area_left, area_top, width, height))
            return false;
        wanted(wide, high, x, y);
    }
    if (left != x || top != y)
        g_set_locate(window, x, y); // its parts and handler go with it
    return true;
}

// Every palette that's up put where it goes: how many were
int place()
{
    char* windows[4] = {};
    const int count = find_palettes(windows, 4);
    int put = 0;
    for (int i = 0; i < count; ++i)
        put += place(windows[i]) ? 1 : 0;
    g_placed = g_move;
    return put;
}

// In place of the manager's open of a menu by name, while macrobar.move is on: a palette put where it goes as soon as
// it's made, before the game draws it
std::uintptr_t __fastcall open_hook(void* manager, void* edx, const char* name, int a, int b)
{
    const std::uintptr_t result = g_open(manager, edx, name, a, b);
    if (g_move && manager == g_place_manager && name != nullptr) {
        __try {
            if (is_palette(name)) {
                const int put = place();
                if (!g_open_logged) { // (else it's put a frame later, at Present)
                    g_open_logged = true;
                    log::write(put != 0 ? L"Macro bar: put where it goes as it opened"
                                        : L"Macro bar: not ready to put as it opened: put a frame later");
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return result;
}

// The manager's open detoured, the first frame macrobar.move is on (on the game's thread, outside the open)
void hook_open()
{
    g_open_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t branch = game != nullptr ? scan::find(game, kOpenAt) : 0;
    if (branch == 0 || *reinterpret_cast<const std::uint8_t*>(branch + kTapCreate) != 0xE8 ||
        reinterpret_cast<char*>(imm32(branch + kTapCreate - 4)) != g_place_manager) {
        log::write(L"Macro bar: the game's opening of a menu wasn't found in FFXiMain.dll: a moved bar opens at the top left for a "
                   L"frame");
        return;
    }
    const auto open = reinterpret_cast<Open>(call_target(branch + kTapCreate));
    if (!imports::detour_auto(open, &open_hook, g_open)) {
        log::write(L"Macro bar: the game's opening of a menu couldn't be hooked: a moved bar opens at the top left for a frame");
        return;
    }
    log::write(L"Macro bar: the game's opening of a menu hooked at +0x%X: a moved bar opens where it goes",
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(open) - reinterpret_cast<std::uintptr_t>(game)));
}

// ---- Why the bar isn't coming up (always logged, a few lines a session: "Macro bar: ...") ------------------------------------
// A report: "after a while, anywhere, Ctrl no longer shows the macro bar until I zone". Every show of the bar goes through the
// game's _canUseMacro (VA 0x101A1B40), which says no while any of about ten conditions holds (the game isn't in its playing
// state, the interface is hidden, an event runs, a game flag is set, a menu window other than the allowed ones is up), so one
// left set blocks the bar until the zone clears it. Nothing said which. So: with Ctrl or Alt held 2 s and no palette up, and
// with the game saying no for 20 s whatever is held, the condition that said no and the values it read are logged with the
// menu manager's windows. The controller's own state going stale (a palette it thinks is up with none there; the held-key
// state set with no key down) and its key handling not running at all are logged too. At most kMostLines a session.
//
// Found by pattern: _canUseMacro's first 0x60 bytes (the game's data it reads and the functions it asks are read from its
// code, so the log names what it really reads), and the controller's key handler (FsMacroController::onKeyDown, VA
// 0x101A15E0), detoured to know the controller and that it runs (its first instructions aren't the ones the macrofix addon
// and macrobar.mode change).
constexpr char kCanUse[] =
    "66 A1 ?? ?? ?? ?? 56 33 F6 66 85 C0 74 0A 0F BF C0 8B 34 85 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 83 39 60 0F 85 ?? ?? ?? ?? B9 "
    "?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8A 48 59 84 C9 0F 85 ?? ?? ?? ?? 8B 88 C4 00 00 00 85 C9 "
    "0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 85 C0 74 0B";
constexpr char kKeyHandler[] = "53 56 8B F1 8B 4E 04 E8 ?? ?? ?? ?? BB 01 00 00 00 6A FF 53 68 89 00 00 00 6A 3E E8";
constexpr int kMostLines = 60;

using Ask = bool(__fastcall*)(void* manager, void* edx);                    // FUN_1015CA10, FUN_1015DAE0
using AskWindow = bool(__fastcall*)(void* manager, void* edx, void* ctrl);  // FUN_1015DAB0
using AskEvent = bool(__cdecl*)(int);                                        // FUN_10086E80
using KeyHandler = void(__fastcall*)(void* controller, void* edx);

// What _canUseMacro reads, from its code: the mode and its handlers' table, the game's state, the manager and what it's asked,
// the game's two objects (the flags at +0x59, +0xC4 and +0x68; the busy word at +0x1C), and the four windows it lets through
struct Gates {
    std::uintptr_t mode = 0, table = 0, state = 0, object = 0, busy = 0, allowed[4] = {};
    void* manager = nullptr;
    Ask hidden = nullptr, has_window = nullptr;
    AskEvent in_event = nullptr;
    AskWindow is_window = nullptr;
};

Gates g_gates;
bool g_gates_searched = false;
bool g_watch_failed = false;
bool g_watch_tried = false;
KeyHandler g_key_handler = nullptr;
void* g_seen = nullptr;        // the macro controller, as the key handler last had it
ULONGLONG g_ran_at = 0;        // when the key handler last ran (GetTickCount64)
ULONGLONG g_checked_at = 0;
int g_lines = 0;

struct Episode {
    ULONGLONG since = 0;
    bool logged = false;
};
Episode g_blocked, g_stale_palette, g_stale_hold;
char g_blocked_why[160] = {};
ULONGLONG g_key_down[2] = {};
bool g_key_logged[2] = {};
bool g_gap_logged = false;
bool g_first_run = true;

void say(const char* fmt, ...)
{
    if (g_lines >= kMostLines) {
        if (g_lines == kMostLines) {
            ++g_lines;
            log::write(L"Macro bar: %d lines logged this session; the rest are left out", kMostLines);
        }
        return;
    }
    ++g_lines;
    char text[1536];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    log::write(L"Macro bar: %hs", text);
}

bool gates_found()
{
    if (g_gates_searched)
        return g_gates.manager != nullptr;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_gates_searched = true;
    const std::uintptr_t f = scan::find(game, kCanUse);
    // the opcodes of what the pattern doesn't cover: the call asking for an event, the one for a window, the four windows
    const auto op = [f](std::ptrdiff_t at) { return *reinterpret_cast<const std::uint8_t*>(f + at); };
    if (f == 0 || op(0x6D) != 0xE8 || op(0x9A) != 0xE8 || op(0xB0) != 0xA1 || op(0xBE) != 0xE8 || op(0xC7) != 0x8B ||
        op(0xDF) != 0x8B || op(0xF7) != 0xA1 || op(0x2C) != 0xE8) {
        log::write(L"Macro bar: the game's _canUseMacro wasn't found in FFXiMain.dll: the log can't say what blocks the bar");
        return false;
    }
    Gates g;
    g.mode = imm32(f + 2);
    g.table = imm32(f + 0x14);
    g.state = imm32(f + 0x1A);
    g.manager = reinterpret_cast<void*>(imm32(f + 0x28));
    g.hidden = reinterpret_cast<Ask>(call_target(f + 0x2C));
    g.object = imm32(f + 0x3A);
    g.busy = imm32(f + 0x58);
    g.in_event = reinterpret_cast<AskEvent>(call_target(f + 0x6D));
    g.has_window = reinterpret_cast<Ask>(call_target(f + 0x9A));
    g.allowed[0] = imm32(f + 0xB1);
    g.allowed[1] = imm32(f + 0xC9);
    g.allowed[2] = imm32(f + 0xE1);
    g.allowed[3] = imm32(f + 0xF8);
    g.is_window = reinterpret_cast<AskWindow>(call_target(f + 0xBE));
    g_gates = g;
    return true;
}

// Whether the game says no to the macro keys now, and why in words with the values it read (empty: it says yes). The
// conditions in the order the game asks them
bool blocked_by(char* why, std::size_t size)
{
    why[0] = '\0';
    __try {
        const Gates& g = g_gates;
        const short mode = *reinterpret_cast<const short*>(g.mode);
        const char* handler = mode != 0 ? *reinterpret_cast<const char* const*>(g.table + static_cast<std::ptrdiff_t>(mode) * 4) : nullptr;
        const std::uint32_t state = **reinterpret_cast<const std::uint32_t* const*>(g.state);
        if (state != 0x60) {
            std::snprintf(why, size, "the game's state is 0x%X, not 0x60 (playing)", state);
            return true;
        }
        if (g.hidden(g.manager, nullptr)) {
            std::snprintf(why, size, "the interface is hidden or an event is up (the manager's FUN_1015CA10)");
            return true;
        }
        const char* object = *reinterpret_cast<const char* const*>(g.object);
        if (object[0x59] != 0) {
            std::snprintf(why, size, "a game flag is set (object +0x59 = %d)", object[0x59]);
            return true;
        }
        if (*reinterpret_cast<const std::uint32_t*>(object + 0xC4) != 0) {
            std::snprintf(why, size, "a game flag is set (object +0xC4 = 0x%X)", *reinterpret_cast<const std::uint32_t*>(object + 0xC4));
            return true;
        }
        const char* busy = *reinterpret_cast<const char* const*>(g.busy);
        if (busy != nullptr && *reinterpret_cast<const short*>(busy + 0x1C) != 0) {
            std::snprintf(why, size, "a game object is busy (+0x1C = %d)", *reinterpret_cast<const short*>(busy + 0x1C));
            return true;
        }
        if (g.in_event(0)) {
            std::snprintf(why, size, "an event is running (FUN_10086E80)");
            return true;
        }
        if (handler != nullptr) {
            const int busy_mode = *reinterpret_cast<const int*>(handler + 0x170);
            if (busy_mode == 2 || busy_mode == 3) {
                std::snprintf(why, size, "the mode's handler says busy (+0x170 = %d)", busy_mode);
                return true;
            }
        }
        const bool menu = g.has_window(g.manager, nullptr);
        const short flag = *reinterpret_cast<const short*>(object + 0x68);
        if (!menu && flag == 0)
            return false;
        for (const std::uintptr_t at : g.allowed) {
            const char* owner = *reinterpret_cast<const char* const*>(at);
            if (g.is_window(g.manager, nullptr, *reinterpret_cast<void* const*>(owner + 8)))
                return false;
        }
        std::snprintf(why, size, "a menu window is up that the macro keys don't allow (menu up %d, object +0x68 = %d)", menu ? 1 : 0, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(why, size, "(the game's conditions couldn't be read)");
        return true;
    }
}

struct Controller {
    std::uint8_t palette = 0, book = 0, pad_open = 0;
    std::uint32_t t10 = 0, t14 = 0;
};

bool read_controller(Controller& c)
{
    if (g_seen == nullptr)
        return false;
    __try {
        const auto* p = static_cast<const std::uint8_t*>(g_seen);
        c.palette = p[0x0C];
        c.book = p[0x0D];
        c.pad_open = p[0x18];
        std::memcpy(&c.t10, p + 0x10, 4);
        std::memcpy(&c.t14, p + 0x14, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string controller_text(const Controller& c, bool have)
{
    if (!have)
        return "controller unreadable";
    char text[160];
    std::snprintf(text, sizeof text, "controller: palette %d, book key %d, pad open %d, hold counters 0x%X/0x%X", c.palette, c.book,
                  c.pad_open, c.t10, c.t14);
    return text;
}

const char* mode_name()
{
    return g_applied == kModeToggle ? "toggle" : g_applied == kModeHold ? "hold" : g_nodelay_applied ? "game, nodelay=1" : "game";
}

// How many palette windows are up; false while that can't be known
bool palettes_up(int& count)
{
    count = 0;
    if (!place_found() || g_place_manager == nullptr)
        return false;
    __try {
        char* windows[4] = {};
        count = find_palettes(windows, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The controller's key handler detoured: it's the controller, and that it runs, that's wanted
void __fastcall key_handler_hook(void* controller, void* edx)
{
    g_seen = controller;
    g_ran_at = GetTickCount64();
    g_key_handler(controller, edx);
}

void start_watch()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_watch_tried = true;
    const std::uintptr_t handler = scan::find(game, kKeyHandler);
    if (!gates_found() || handler == 0 ||
        !imports::detour_auto(reinterpret_cast<KeyHandler>(handler), &key_handler_hook, g_key_handler)) {
        g_watch_failed = true;
        log::write(L"Macro bar: the game's macro controller wasn't hooked (%ls): what blocks the bar isn't logged",
                   handler == 0 ? L"key handler not found" : g_gates.manager == nullptr ? L"_canUseMacro not found" : L"detour failed");
        return;
    }
    log::write(L"Macro bar: the game's macro controller key handling hooked at +0x%X: what blocks the bar is logged",
               static_cast<unsigned>(handler - reinterpret_cast<std::uintptr_t>(game)));
}

// Every 250 ms, on the game's thread
void watch()
{
    if (g_watch_failed || g_key_handler == nullptr)
        return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_checked_at < 250)
        return;
    g_checked_at = now;
    if (g_seen == nullptr)
        return;
    const bool front = !window::in_background();
    const bool running = now - g_ran_at < 1000;
    if (!running) {
        if (front && now - g_ran_at >= 20000 && !g_gap_logged) {
            g_gap_logged = true;
            say("the controller's key handling hasn't run for %llu s (the game isn't playing, or a caller stopped asking it)",
                (now - g_ran_at) / 1000);
        }
        return;
    }
    if (g_gap_logged) {
        g_gap_logged = false;
        say("the controller's key handling runs again");
    }
    Controller c;
    const bool have = read_controller(c);
    int palettes = 0;
    const bool know = palettes_up(palettes);
    char why[160];
    const bool blocked = blocked_by(why, sizeof why);
    if (g_first_run) {
        g_first_run = false;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        const std::uintptr_t branch = game != nullptr ? scan::find(game, kOpenAt) : 0;
        const std::uint8_t tap = branch != 0 ? *reinterpret_cast<const std::uint8_t*>(branch + kTapJump) : 0;
        say("the key handling runs (controller @%08X, macrobar.mode=%s); its tap branch is %s; macros are %s",
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_seen)), mode_name(),
            tap == 0x74 ? "the game's own" : tap == 0xEB ? "changed (the macrofix addon, or macrobar.mode)" : "not found",
            blocked ? why : "allowed");
    }
    const bool down[2] = {front && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, front && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0};
    const bool any_down = down[0] || down[1];

    // The game saying no: for 20 s, whatever is held
    if (blocked) {
        if (g_blocked.since == 0 || std::strcmp(why, g_blocked_why) != 0) {
            if (g_blocked.logged)
                say("macros are blocked by something else now: %s", why);
            g_blocked = {now, false};
            std::snprintf(g_blocked_why, sizeof g_blocked_why, "%s", why);
        }
        if (!g_blocked.logged && now - g_blocked.since >= 20000) {
            g_blocked.logged = true;
            say("macros have been blocked for %llu s: %s; %s; %s", (now - g_blocked.since) / 1000, why,
                controller_text(c, have).c_str(), menulog::windows_text().c_str());
        }
    } else {
        if (g_blocked.logged)
            say("macros are allowed again after %llu s", (now - g_blocked.since) / 1000);
        g_blocked = {};
        g_blocked_why[0] = '\0';
    }

    // Ctrl or Alt held 2 s with no palette up
    static const char* const kKey[2] = {"Ctrl", "Alt"};
    for (int i = 0; i < 2; ++i) {
        if (!down[i]) {
            g_key_down[i] = 0;
            g_key_logged[i] = false;
            continue;
        }
        if (g_key_down[i] == 0)
            g_key_down[i] = now;
        if (g_key_logged[i] || !know || palettes != 0 || now - g_key_down[i] < 2000)
            continue;
        g_key_logged[i] = true;
        say("%s held for %llu ms and no palette is up: %s%s; %s; macrobar.mode=%s; %s", kKey[i], now - g_key_down[i],
            blocked ? "the game says no: " : "the game says yes (the key isn't reaching the controller, or its state is stale)",
            blocked ? why : "", controller_text(c, have).c_str(), mode_name(), menulog::windows_text().c_str());
    }

    // The controller's state going stale
    if (have && know && c.palette != 0 && palettes == 0) {
        if (g_stale_palette.since == 0)
            g_stale_palette.since = now;
        if (!g_stale_palette.logged && now - g_stale_palette.since >= 1500) {
            g_stale_palette.logged = true;
            say("the controller says palette %d is up but no palette window is, so the key finds it up already and opens none; %s; %s",
                c.palette, controller_text(c, have).c_str(), menulog::windows_text().c_str());
        }
    } else {
        if (g_stale_palette.logged)
            say("the controller's palette matches the windows again (palette %d, %d windows)", c.palette, palettes);
        g_stale_palette = {};
    }
    if (have && front && !any_down && (c.book != 0 || c.t10 != 0 || c.t14 != 0)) {
        if (g_stale_hold.since == 0)
            g_stale_hold.since = now;
        if (!g_stale_hold.logged && now - g_stale_hold.since >= 2500) {
            g_stale_hold.logged = true;
            say("no Ctrl or Alt is down but the controller's held-key state is set (a held pad button would do this too); %s",
                controller_text(c, have).c_str());
        }
    } else {
        if (g_stale_hold.logged)
            say("the controller's held-key state is clear again");
        g_stale_hold = {};
    }
}

// mode=toggle: the controller's word that palette N is up (+0x0C) kept to the palette windows there are. A palette gone another
// way than a press of its key (the game's own closing of it) left the controller saying it was up, so the next press only put
// down what wasn't there and the bar took two presses to come back (reported 2026-11-06: Alt, then a click out of the game's
// window, took the bar down with Alt still "down"). Once the controller has said up for kResyncFrames frames with no palette
// window in the manager's list, it's put down as a press would (+0x0C, +0x18), so the next press shows the bar. (A palette
// macrobar.hide leaves out of the draw is still in the list.)
constexpr int kResyncFrames = 10;
int g_missing = 0;

void resync()
{
    auto* controller = static_cast<std::uint8_t*>(g_controller);
    if (controller == nullptr || !place_found() || g_place_manager == nullptr)
        return;
    __try {
        const std::uint8_t up = controller[0x0C];
        if (up == 0 || up > 2 || find_palette() != nullptr) {
            g_missing = 0;
            return;
        }
        if (++g_missing < kResyncFrames)
            return;
        g_missing = 0;
        controller[0x0C] = 0;
        controller[0x18] = 0;
        say("toggle: the controller said palette %d was up with no palette window there: put down, so the next press shows it", up);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_missing = 0;
    }
}

} // namespace

void reload(const wchar_t* ini)
{
    wchar_t mode[32] = {};
    GetPrivateProfileStringW(L"client", L"macrobar.mode", L"", mode, 32, ini);
    if (_wcsicmp(mode, L"toggle") == 0)
        g_mode = kModeToggle;
    else if (_wcsicmp(mode, L"hold") == 0)
        g_mode = kModeHold;
    else if (mode[0] == L'\0' && GetPrivateProfileIntW(L"client", L"macrobar.instant", 0, ini) != 0) // before 0.7.14
        g_mode = kModeHold;
    else
        g_mode = kModeGame;
    g_nodelay = GetPrivateProfileIntW(L"client", L"macrobar.nodelay", 0, ini) != 0;
    apply();
    const auto number = [ini](const wchar_t* key, double fallback) {
        wchar_t text[32] = {};
        GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
        wchar_t* end = nullptr;
        const double value = std::wcstod(text, &end);
        return end != text ? std::clamp(value, 0.0, 100.0) : fallback;
    };
    g_move = GetPrivateProfileIntW(L"client", L"macrobar.move", 0, ini) != 0;
    g_x = number(L"macrobar.x", 0.8);
    g_y = number(L"macrobar.y", 4);
}

void install(const wchar_t* ini)
{
    // The macrofix addon (atom0s) searches the game's code for the bytes this file changes and changes them itself, so with
    // macrobar.mode or macrobar.nodelay on it finds nothing ("Failed to locate a required pointer"), and with both off it
    // would change them under this file, which then leaves them alone. Hold and nodelay are what it does: it's held off.
    addons::hold("macrofix", "VanaCore has it built in and the two can't both change the game's macro keys. /vana > HUD > "
                             "Macro Bar > Show: Hold is macrofix's way, Toggle shows the bar on a press and hides it on the next; "
                             "No delay speeds Ctrl and Alt alone");
    reload(ini);
}

void on_frame()
{
    if (g_mode != g_applied || g_nodelay != g_nodelay_applied)
        apply();
    if (!g_watch_tried && !g_watch_failed)
        start_watch();
    watch();
    if ((g_move || g_placed) && place_found() && g_place_manager != nullptr) {
        if (g_move && !g_open_tried)
            hook_open();
        __try {
            place();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_placed = false;
        }
    }
    if (g_applied != kModeToggle)
        return;
    resync();
    const bool background = window::in_background();
    if (background == g_background)
        return;
    g_background = background;
    if (!background) {
        g_front_at = GetTickCount() | 1;
        return;
    }
    // Gone to the background (Alt+Tab): the bar down, if up
    auto* controller = static_cast<std::uint8_t*>(g_controller);
    if (controller == nullptr || controller[0x0C] == 0 || controller[0x0C] > 2)
        return;
    const std::uint8_t up = controller[0x0C];
    controller[0x0C] = 0;
    controller[0x18] = 0;
    g_destroy(g_manager, nullptr, g_names[up - 1]);
}

bool outline(double* moved_x, double* moved_y)
{
    if (!place_found() || g_place_manager == nullptr)
        return false;
    int area_left = 0, area_top = 0, width = 0, height = 0;
    outline::Box box;
    __try {
        if (!area(area_left, area_top, width, height))
            return false;
        char* window = find_palette();
        if (window != nullptr) {
            box = {at<short>(window, kWinFrame), at<short>(window, kWinFrame + 2), at<short>(window, kWinFrame + 4),
                   at<short>(window, kWinFrame + 6)};
        }
        if (window == nullptr || box.right <= box.left || box.bottom <= box.top) {
            // where macrobar.x and .y put it, at the size it last had (or the game's: off, its top left)
            if (g_move) {
                wanted(g_seen_w, g_seen_h, box.left, box.top);
            } else {
                box.left = area_left + 16;
                box.top = area_top + 48;
            }
            box.right = box.left + g_seen_w;
            box.bottom = box.top + g_seen_h;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    outline::Drag drag;
    if (!outline::draw("macrobar", box, 10, drag))
        return false;
    // dragged: the bar goes where the box is taken to (macrobar.move on) at once
    if (drag.held) {
        g_move = true;
        g_x = std::clamp(std::round((drag.left - area_left) * 10000.0 / width) / 100.0, 0.0, 100.0);
        g_y = std::clamp(std::round((drag.top - area_top) * 10000.0 / height) / 100.0, 0.0, 100.0);
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = g_x;
        *moved_y = g_y;
    }
    return true;
}

} // namespace vanacore::macrobar
