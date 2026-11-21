#include "menuhover.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gameui.hpp"
#include "log.hpp"
#include "menulog.hpp"
#include "scan.hpp"

namespace vanacore::menuhover {
namespace {

// The hover in the game's mouse frame (FUN_10124EA0, 0x101258CE in the 2025-12-26 FFXiMain.dll), where a menu is up and the
// cursor shown (the mouse object's +0x4D is 2, +0x4E set; esi the mouse, ebp the current window, edi the row of the frame
// before, which the frame set to -1 before this):
//   mov ecx,ebp; call IsOnButton(window, &mouse+4, &flag)     the row under the pointer (a short in ax; the rest junk)
//   test ax,ax; mov [esi+52h],ax; jle; cmp ax,di; je          kept as the row of this frame; none, or the same: done
//   push 0; push eax; mov ecx,ebp; call SetCursol(window, row, 0)
constexpr char kHover[] = "8B CD E8 ?? ?? ?? ?? 66 85 C0 66 89 46 52 7E 0F 66 3B C7 74 0A 6A 00 50 8B CD E8 ?? ?? ?? ??";
constexpr std::ptrdiff_t kPickCall = 2, kSelectCall = 26;

// Both are thiscalls with two stack arguments, the callee's ret 8: __fastcall with the same two (edx unused)
using Pick = int(__fastcall*)(void* window, void* edx, const std::int32_t* point, char* flag);
using Select = void(__fastcall*)(void* window, void* edx, int row, int flag);

// The Items list's per-frame mouse block (and the storage list's, the Equipment list's: 0x101F105B, 0x101F1080, 0x101F10A5 in
// the 2025-12-26 FFXiMain.dll): for the sort box, the storage window... each
//   lea edx,[esp+0Ch]; push edx; call IsOnWindow(window, &point); test al,al; je +0Ah; mov eax,[edi]; push esi; push key;
//   mov ecx,edi; call [eax+18h]             the list handed its key (8 the sort box, 4 the storage...) when the pointer is over it
constexpr char kWindowHover[] = "8D 54 24 0C 52 E8 ?? ?? ?? ?? 84 C0 74 0A 8B 07 56 6A ?? 8B CF FF 50 18";
constexpr std::ptrdiff_t kWindowCall = 5;
constexpr unsigned kMostWindowSites = 8;
constexpr unsigned long long kFocusSettleMs = 500; // after the game moves the keys, a window under the pointer doesn't take them

// The macro bar's two edit rows' per-frame mouse block (Macros > Edit: "menu    mcr1edit" / "mcr2edit", the long bar's
// "mcr1edlo" / "mcr2edlo"; 0x101A3407 and 0x101A3AEA in the 2025-12-26 FFXiMain.dll, read live 2026-11-14), after the test of
// the other row: the row's handler handed its key 8, which gives the Button Edit box (Copy, Paste, Delete: "menu    mcrbedit",
// whose handler is [0x1062DB18]) the keys, whenever the pointer is inside the box's frame:
//   lea edx,[esp+0Ch]; push edx; call IsOnWindow(box, &point); test al,al; je +0Eh; mov eax,[esi]; movsx cx,bl; push ecx;
//   push 8; mov ecx,esi; call [eax+18h]
// Moving the cursor with the keys puts the game's pointer on the key the cursor is on, so with the box moved over the bar
// (macroeditoptions.move) the key under it gave the box the keys, and took them back every frame after Esc had given them to
// the row (reported 2026-11-14: "I spam escape and I can't get out of this window"). With menu.hovermove the box takes the
// keys by its + key (the game's own) or a click on it, never by the pointer being over it.
constexpr char kBoxHover[] = "8D 54 24 0C 52 E8 ?? ?? ?? ?? 84 C0 74 0E 8B 06 66 0F BE CB 51 6A 08 8B CE FF 50 18";
constexpr unsigned kBoxSites = 2;

// A thiscall with one stack argument, the callee's ret 4: the result in al
using OnWindow = char(__fastcall*)(void* window, void* edx, const std::int32_t* point);
OnWindow g_box_on_window = nullptr; // the game's IsOnWindow, as the edit rows call it
bool g_box_logged = false;          // the first time the box was kept from the keys was written to the log

std::atomic<bool> g_on{true};   // [client] menu.hovermove
std::atomic<bool> g_no_mouse{false}; // [client] menu.nomouse: the mouse picks no menu row and gives no window the keys
bool g_no_mouse_logged = false; // the first pick dropped for it was written to the log
bool g_searched = false;        // FFXiMain.dll was looked through
bool g_hooked = false;
Pick g_pick = nullptr;          // the game's IsOnButton (past any other module's detour of it: it is called by its start)
Select g_select = nullptr;      // the game's SetCursol
const std::int32_t* g_point = nullptr; // the mouse's place (x, y) in the game's mouse object, +4: what the game hands IsOnButton
std::int32_t g_x = 0, g_y = 0;  // where the pointer was last taken up
bool g_known = false;           // g_x and g_y are a place
bool g_moved = true;            // the pointer moved since the last time it was taken up, as of this frame's test
bool g_ran = false;             // the test ran since the last frame (on_frame)
bool g_logged = false;          // the first dropped pick was written to the log
OnWindow g_on_window = nullptr; // the game's IsOnWindow (past any other module's detour of it: it is called by its start)
bool g_window_logged = false;   // the first dropped window pick was written to the log
bool g_settle_logged = false;   // the first window kept from the keys just after they moved was written to the log

// A mouse button down (+0x58 held, +0x60 pressed this frame: bits 1 left, 2 right, 4 middle): a click is on the row under
// the pointer, which the pick must have found whether or not the pointer moved
bool button_down()
{
    if (g_point == nullptr)
        return false;
    const auto* mouse = reinterpret_cast<const std::uint8_t*>(g_point) - 4;
    std::uint32_t held = 0, pressed = 0;
    std::memcpy(&held, mouse + 0x58, sizeof held);
    std::memcpy(&pressed, mouse + 0x60, sizeof pressed);
    return ((held | pressed) & 7u) != 0;
}

// The first pick or window the mouse was kept from ([client] menu.nomouse), once, in the log
void note_no_mouse()
{
    if (g_no_mouse_logged)
        return;
    g_no_mouse_logged = true;
    log::write(L"Menus: the mouse was kept from picking a menu row or giving a window the keys ([client] menu.nomouse)");
}

// In place of the game's call of IsOnButton: the pointer's place noted, then the game's own
int __fastcall pick_hook(void* window, void* edx, const std::int32_t* point, char* flag)
{
    g_point = point;
    const std::int32_t x = point[0], y = point[1];
    g_moved = !g_known || x != g_x || y != g_y;
    g_x = x;
    g_y = y;
    g_known = true;
    g_ran = true;
    if (gameui::veiled(window))
        return -1; // a window an addon hides by its layout's name (vana.gameui.hide): no row of it under the pointer, the addon's own drawing is
    return g_pick(window, edx, point, flag);
}

// In place of the game's call of SetCursol: made only when the pointer moved, or a button is down
void __fastcall select_hook(void* window, void* edx, int row, int flag)
{
    if (g_no_mouse.load(std::memory_order_relaxed)) {
        note_no_mouse();
        return;
    }
    if (g_on.load(std::memory_order_relaxed) && !g_moved && !button_down()) {
        if (!g_logged) {
            g_logged = true;
            log::write(L"Menus: the mouse resting on a row didn't pick it (row %d): the cursor stays where the keys put it "
                       L"([client] menu.hovermove)",
                       static_cast<int>(static_cast<short>(row)));
        }
        return;
    }
    g_select(window, edx, row, flag);
}

// In place of the game's call of IsOnWindow in the lists' mouse block: a window the pointer is over is one it rests over only
// until it moves (or a button is down), as for a row
char __fastcall on_window_hook(void* window, void* edx, const std::int32_t* point)
{
    const char over = g_on_window(window, edx, point);
    if (over != 0 && gameui::veiled(window))
        return 0; // hidden by an addon (vana.gameui.hide): not under the pointer
    if (over != 0 && g_no_mouse.load(std::memory_order_relaxed)) {
        note_no_mouse();
        return 0;
    }
    // The game made another window the current one a moment ago (a list just opened): the pointer it put on the list's cursor row
    // (a jump counts as the pointer moving) can lie inside the sort box's frame where the game first made it, over the list's top
    // rows, until the box is moved (sortmenu.move) a frame or two later: row 1 after a restart, the cursor kept thereafter. Not
    // the user's: the box doesn't take the keys for that moment, and does when the pointer is over it after
    if (over != 0 && g_on.load(std::memory_order_relaxed)) {
        const unsigned long long changed = menulog::focus_changed_at();
        if (changed != 0 && GetTickCount64() - changed < kFocusSettleMs) {
            if (!g_settle_logged) {
                g_settle_logged = true;
                log::write(L"Menus: a window under the pointer %llu ms after the keys moved didn't take them (the pointer at %d, %d): "
                           L"the game's own jump of the pointer to a list's row isn't the mouse picking it ([client] menu.hovermove)",
                           GetTickCount64() - changed, static_cast<int>(point[0]), static_cast<int>(point[1]));
            }
            return 0;
        }
    }
    if (over != 0 && g_on.load(std::memory_order_relaxed) && g_known && !g_moved && !button_down()) {
        if (!g_window_logged) {
            g_window_logged = true;
            log::write(L"Menus: the mouse resting over a list's sort box (or storage window) didn't give it the keys (the pointer at %d, %d): "
                       L"it takes them when the pointer moves onto it ([client] menu.hovermove)",
                       static_cast<int>(point[0]), static_cast<int>(point[1]));
        }
        return 0;
    }
    return over;
}

// In place of the game's call of IsOnWindow in the macro edit rows' test of the Button Edit box: over it only for a click
char __fastcall box_hook(void* window, void* edx, const std::int32_t* point)
{
    const char over = g_box_on_window(window, edx, point);
    if (over == 0)
        return 0;
    if (g_no_mouse.load(std::memory_order_relaxed)) {
        note_no_mouse();
        return 0;
    }
    if (g_on.load(std::memory_order_relaxed) && !button_down()) {
        if (!g_box_logged) {
            g_box_logged = true;
            log::write(L"Menus: the pointer over the macro bar's Button Edit box didn't give it the keys (the pointer at %d, %d): it "
                       L"takes them by its + key or a click ([client] menu.hovermove)",
                       static_cast<int>(point[0]), static_cast<int>(point[1]));
        }
        return 0;
    }
    return over;
}

std::uintptr_t call_target(std::uintptr_t at)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), sizeof rel);
    return at + 5 + static_cast<std::uintptr_t>(rel);
}

// The call at `at` (E8 rel32) made a call of `target`
bool call_to(std::uintptr_t at, const void* target)
{
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(target) - (at + 5));
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 5, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at + 1), &rel, sizeof rel);
    VirtualProtect(reinterpret_cast<void*>(at), 5, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(at), 5);
    return true;
}

// The calls of IsOnWindow in the lists' mouse block found and hooked (the same one function at each)
void hook_windows(HMODULE game)
{
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    std::uintptr_t sites[kMostWindowSites] = {};
    unsigned count = 0;
    for (unsigned i = 0; i < kMostWindowSites; ++i) {
        const std::uintptr_t found = scan::find(base, size, kWindowHover, i);
        if (found == 0)
            break;
        sites[count++] = found + kWindowCall;
    }
    if (count == 0) {
        log::write(L"Menus: the lists' mouse test of their sort box wasn't found in FFXiMain.dll: a resting mouse over it can still take the keys");
        return;
    }
    const std::uintptr_t target = call_target(sites[0]);
    for (unsigned i = 0; i < count; ++i)
        if (*reinterpret_cast<const std::uint8_t*>(sites[i]) != 0xE8 || call_target(sites[i]) != target ||
            target < base || target >= base + size) {
            log::write(L"Menus: the lists' mouse test of their sort box isn't as expected: a resting mouse over it can still take the keys");
            return;
        }
    g_on_window = reinterpret_cast<OnWindow>(target);
    for (unsigned i = 0; i < count; ++i)
        if (!call_to(sites[i], reinterpret_cast<const void*>(&on_window_hook))) {
            log::write(L"Menus: the game's code couldn't be written (error %lu): a resting mouse over a list's sort box can still take the keys",
                       GetLastError());
            return;
        }
    log::write(L"Menus: the lists' mouse test of their sort box and storage window hooked at %u places: a mouse at rest doesn't give a "
               L"window the keys ([client] menu.hovermove)",
               count);
}

// The macro edit rows' two calls of IsOnWindow for the Button Edit box found and hooked (both of the same function, in the image)
void hook_box(HMODULE game)
{
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    std::uintptr_t sites[kBoxSites] = {};
    for (unsigned i = 0; i < kBoxSites; ++i) {
        const std::uintptr_t found = scan::find(base, size, kBoxHover, i);
        if (found == 0 || scan::find(base, size, kBoxHover, kBoxSites) != 0) {
            log::write(L"Menus: the macro bar's edit rows' mouse test of the Button Edit box wasn't found once for each row in "
                       L"FFXiMain.dll: the pointer over the box still gives it the keys");
            return;
        }
        sites[i] = found + kWindowCall;
    }
    const std::uintptr_t target = call_target(sites[0]);
    for (const std::uintptr_t site : sites)
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8 || call_target(site) != target || target < base || target >= base + size) {
            log::write(L"Menus: the macro bar's edit rows' mouse test of the Button Edit box isn't as expected: the pointer over the box "
                       L"still gives it the keys");
            return;
        }
    g_box_on_window = reinterpret_cast<OnWindow>(target);
    for (const std::uintptr_t site : sites)
        if (!call_to(site, reinterpret_cast<const void*>(&box_hook))) {
            log::write(L"Menus: the game's code couldn't be written (error %lu): the pointer over the macro bar's Button Edit box "
                       L"can still give it the keys",
                       GetLastError());
            return;
        }
    log::write(L"Menus: the macro bar's edit rows' mouse test of the Button Edit box hooked: it takes the keys by its + key or a click "
               L"([client] menu.hovermove)");
}

// The two calls found and hooked, the first time they're wanted once FFXiMain.dll is there
void hook()
{
    if (g_searched)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_searched = true;
    const std::uintptr_t found = scan::find(game, kHover);
    if (found == 0) {
        log::write(L"Menus: the game's mouse pick of a menu row wasn't found in FFXiMain.dll: [client] menu.hovermove does nothing");
        return;
    }
    const std::uintptr_t pick_at = found + kPickCall, select_at = found + kSelectCall;
    if (*reinterpret_cast<const std::uint8_t*>(pick_at) != 0xE8 || *reinterpret_cast<const std::uint8_t*>(select_at) != 0xE8) {
        log::write(L"Menus: the game's mouse pick of a menu row isn't as expected: [client] menu.hovermove does nothing");
        return;
    }
    g_pick = reinterpret_cast<Pick>(call_target(pick_at));
    g_select = reinterpret_cast<Select>(call_target(select_at));
    // Both targets in the game's own image, as IsOnButton and SetCursol are, not a jump of another program's
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    const auto inside = [&](const void* p) { return reinterpret_cast<std::uintptr_t>(p) >= base && reinterpret_cast<std::uintptr_t>(p) < end; };
    if (!inside(reinterpret_cast<const void*>(g_pick)) || !inside(reinterpret_cast<const void*>(g_select))) {
        log::write(L"Menus: the game's mouse pick of a menu row calls somewhere unexpected: [client] menu.hovermove does nothing");
        g_pick = nullptr;
        g_select = nullptr;
        return;
    }
    if (!call_to(pick_at, reinterpret_cast<const void*>(&pick_hook)) || !call_to(select_at, reinterpret_cast<const void*>(&select_hook))) {
        log::write(L"Menus: the game's code couldn't be written (error %lu): [client] menu.hovermove does nothing", GetLastError());
        // either call that was changed would run a hook with its partner missing: the first one back as it was
        std::uint8_t* const site = reinterpret_cast<std::uint8_t*>(pick_at);
        DWORD protect = 0;
        const std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(g_pick) - (pick_at + 5));
        if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protect)) {
            std::memcpy(site + 1, &rel, sizeof rel);
            VirtualProtect(site, 5, protect, &protect);
            FlushInstructionCache(GetCurrentProcess(), site, 5);
        }
        g_pick = nullptr;
        g_select = nullptr;
        return;
    }
    g_hooked = true;
    log::write(L"Menus: the game's mouse pick of a menu row hooked at +0x%X: a mouse at rest doesn't pick a row ([client] menu.hovermove)",
               static_cast<unsigned>(found - base));
    hook_windows(game);
    hook_box(game);
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"menu.hovermove", 1, ini) != 0;
    g_no_mouse = GetPrivateProfileIntW(L"client", L"menu.nomouse", 0, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

bool pointer(int& x, int& y, bool& moved)
{
    if (!g_known)
        return false;
    x = g_x;
    y = g_y;
    moved = g_moved;
    return true;
}

void on_frame()
{
    if ((g_on.load(std::memory_order_relaxed) || g_no_mouse.load(std::memory_order_relaxed) || gameui::veiling()) && !g_searched)
        hook();
    if (!g_hooked)
        return;
    // The test didn't run this frame (no menu up, the menu opening, the interface hidden): wherever the pointer is now is
    // where it rests, so a menu that comes up under it isn't one the pointer moved onto
    if (!g_ran && g_point != nullptr) {
        g_x = g_point[0];
        g_y = g_point[1];
        g_known = true;
    }
    g_ran = false;
}

} // namespace vanacore::menuhover
