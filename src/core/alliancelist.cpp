#include "alliancelist.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::alliancelist {
namespace {

// The party list's frame (YkWndParty's update in the PS2 client; VA 0x1020250A in FFXiMain.dll) keeps the alliance lists
// ("menu    raid1   " and "menu    raid2   ": parties 2 and 3) up only while nothing is in their way: no window in the
// menu manager's area 0x81 (IsAreaWindow, VA 0x1015DA40: any window whose area word, ctrl+0x38, has a bit of 0x81 and
// which isn't on its way out, state ctrl+0x10 == 0xE), the Equipment window not drawn, neither macro palette drawn (the
// bar that comes up on Ctrl or Alt: FsMPaletteOne and FsMPaletteTwo, "menu    mcr1pall" / "mcr2pall"), no event (a
// cutscene). Else it destroys them, and makes them again once the way is clear. On the PC the palettes are area 0x81
// windows themselves, so the first check takes the lists down the frame the bar comes up, before the two checks that
// name them. VanaCore gives the first check a call of its own that hides the palettes' area bits while the game's
// IsAreaWindow walks the list, and makes the two palette checks' jumps nops; the other checks stay as they were.
//
// push 0; push 0; push 81h; mov ecx,<the menu manager>; call IsAreaWindow; test al,al; jne; then three of
// mov ecx,[the window's pointer]; call IsDrawWindow; test al,al; jne (Equipment, palette one, palette two); push 1; call IsEvent
constexpr char kChecks[] = "6A 00 6A 00 68 81 00 00 00 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? "
                           "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? "
                           "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? "
                           "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? "
                           "6A 01 E8";
constexpr std::ptrdiff_t kAreaCall = 14;              // call IsAreaWindow (E8 rel32)
constexpr std::ptrdiff_t kPalettes[2] = {40, 55};     // the mov ecx,[..] operands: where the palettes' pointers are
constexpr std::ptrdiff_t kJumps[2] = {51, 66};        // the jne after palette one's IsDrawWindow, and palette two's
constexpr std::ptrdiff_t kCtrl = 0x8;                 // a window object: its CTkMenuCtrlData (0 while it isn't up)
constexpr std::ptrdiff_t kArea = 0x38;                // a ctrl: its area word
constexpr std::uint8_t kNops[2] = {0x90, 0x90};

using IsAreaWindow = bool(__thiscall*)(void* manager, int area, void* ctrl, int flag);

bool g_on = true;        // [client] alliance.macrobar
bool g_applied = false;  // the call and the two jumps are VanaCore's now
bool g_searched = false; // FFXiMain.dll was looked through (g_call != 0: found)
std::uintptr_t g_call = 0;
IsAreaWindow g_is_area_window = nullptr; // the game's
std::uintptr_t g_jumps[2] = {};
std::uint8_t g_original[2][2] = {}; // the jumps' own bytes, to put back
std::uintptr_t g_palettes[2] = {};  // where the game keeps each palette object's pointer

std::uintptr_t palette_ctrl(std::size_t i)
{
    const std::uintptr_t object = *reinterpret_cast<const std::uintptr_t*>(g_palettes[i]);
    return object != 0 ? *reinterpret_cast<const std::uintptr_t*>(object + kCtrl) : 0;
}

// The game's IsAreaWindow (__thiscall on the menu manager, through __fastcall: the same stack, edx unused), with the
// palettes' area bits off while it walks the windows, so the macro bar isn't in the alliance lists' way
bool __fastcall area_window(void* manager, void*, int area, void* ctrl, int flag)
{
    std::uintptr_t ctrls[2] = {};
    std::uint16_t areas[2] = {};
    for (std::size_t i = 0; i < 2; ++i) {
        ctrls[i] = palette_ctrl(i);
        if (ctrls[i] == 0)
            continue;
        areas[i] = *reinterpret_cast<std::uint16_t*>(ctrls[i] + kArea);
        *reinterpret_cast<std::uint16_t*>(ctrls[i] + kArea) = 0;
    }
    const bool found = g_is_area_window(manager, area, ctrl, flag);
    for (std::size_t i = 0; i < 2; ++i)
        if (ctrls[i] != 0)
            *reinterpret_cast<std::uint16_t*>(ctrls[i] + kArea) = areas[i];
    return found;
}

// The call and the two jumps, the first time they're wanted once FFXiMain.dll is there
bool found()
{
    if (g_searched)
        return g_call != 0;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const std::uintptr_t at = scan::find(game, kChecks);
    if (at == 0) {
        log::write(L"Alliance lists: the party list's macro bar check wasn't found in FFXiMain.dll: [client] alliance.macrobar "
                   L"does nothing");
        return false;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + kAreaCall + 1), 4);
    const std::uintptr_t is_area_window = at + kAreaCall + 5 + rel;
    for (std::size_t i = 0; i < 2; ++i) {
        std::memcpy(&g_palettes[i], reinterpret_cast<const void*>(at + kPalettes[i]), 4);
        g_jumps[i] = at + kJumps[i];
        std::memcpy(g_original[i], reinterpret_cast<const void*>(g_jumps[i]), 2);
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    const bool sane = is_area_window > base && is_area_window < end && g_palettes[0] > base && g_palettes[0] < end &&
                      g_palettes[1] > base && g_palettes[1] < end && g_original[0][0] == 0x75 && g_original[1][0] == 0x75;
    if (!sane) {
        log::write(L"Alliance lists: the party list's macro bar check at +0x%X isn't as expected: [client] alliance.macrobar does "
                   L"nothing",
                   static_cast<unsigned>(at - base));
        return false;
    }
    g_call = at + kAreaCall;
    g_is_area_window = reinterpret_cast<IsAreaWindow>(is_area_window);
    log::write(L"Alliance lists: the party list's macro bar check found at +0x%X (IsAreaWindow +0x%X)",
               static_cast<unsigned>(at - base), static_cast<unsigned>(is_area_window - base));
    return true;
}

// `size` bytes of code (2: one store, so the game's thread, which runs this code, never sees one byte of two)
void write(std::uintptr_t at, const void* bytes, std::size_t size)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Alliance lists: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(at),
                   GetLastError());
        return;
    }
    if (size == 2) {
        std::uint16_t word = 0;
        std::memcpy(&word, bytes, 2);
        *reinterpret_cast<volatile std::uint16_t*>(at) = word;
    } else {
        std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    }
    VirtualProtect(reinterpret_cast<void*>(at), size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(at), size);
}

// The call and the jumps as the setting wants them (VanaCore's: the lists stay; the game's: the bar takes them down)
void apply()
{
    if (g_on == g_applied || !found())
        return;
    const std::uintptr_t target = g_on ? reinterpret_cast<std::uintptr_t>(&area_window) : reinterpret_cast<std::uintptr_t>(g_is_area_window);
    const auto rel = static_cast<std::int32_t>(target - (g_call + 5));
    write(g_call + 1, &rel, 4);
    for (std::size_t i = 0; i < 2; ++i)
        write(g_jumps[i], g_on ? kNops : g_original[i], 2);
    g_applied = g_on;
    if (g_on)
        log::write(L"Alliance lists: kept on screen while the macro bar is up ([client] alliance.macrobar=1)");
    else
        log::write(L"Alliance lists: taken down while the macro bar is up, as the game does ([client] alliance.macrobar=0)");
}

// [client] party.hide, alliance1.hide, alliance2.hide: the party list is a window of the menu manager like the help
// bar's, with a layout named in the game's data ("menu    partywin", 16 characters at the layout's +0x46, a window's +4
// being its layout); the alliance lists are "menu    raid1   " and "menu    raid2   ". Each left out of the window
// draw, rows and all, by its own key.
constexpr char kHideLayouts[3][16 + 1] = {"menu    partywin", "menu    raid1   ", "menu    raid2   "};
constexpr const wchar_t* kHideKeys[3] = {L"party.hide", L"alliance1.hide", L"alliance2.hide"};
constexpr const wchar_t* kHideNames[3] = {L"Party list", L"Alliance list 1", L"Alliance list 2"};
constexpr std::ptrdiff_t kWindowLayout = 4;
constexpr std::ptrdiff_t kLayoutName = 0x46;
// A window's owner (+0xC; the party list's is CTkParty, the global at 0x1062F708). Its draw (FUN_102028F0) starts with a
// mode of its own, +0x40 nonzero: in place of the members it draws one entry with two gauges (+0x41 and +0x42 their
// shown values, YkDrawHPBar2 and the bar beside it), a name (+0x48) and two numbers (+0x44, +0x46), and returns. That's
// the fishing gauge: the hooked fish's HP, shown nowhere else, so hiding the party list hid it (reported 2026-10-21).
// The list is drawn while the mode is on, whatever hides it.
constexpr std::ptrdiff_t kWindowOwner = 0xC, kOwnerGauge = 0x40;

bool g_hide[3] = {};        // [client] party.hide, alliance1.hide, alliance2.hide
bool g_hide_logged[3] = {};
bool g_gauge_logged = false;

// [client] party.background, alliance1.background, alliance2.background (on unless 0): off, the list's frame and title
// aren't drawn, its rows (names, HP and MP bars, TP) are. The game's window draw (FUN_101173D0) draws the layout's pieces
// (the frame and the title among them) only while the window's +0x69 is set, then its owner's draw (the rows) whatever
// it is (seen live 2026-10-23). The flag is cleared before each draw (a list made again, for a zone or a new party, has
// it set) and set again on the window VanaCore cleared it on when the key goes back on.
constexpr const wchar_t* kFrameKeys[3] = {L"party.background", L"alliance1.background", L"alliance2.background"};
constexpr std::ptrdiff_t kWindowPieces = 0x69;
bool g_frame[3] = {true, true, true};
const void* g_bare[3] = {}; // the window whose flag VanaCore cleared
bool g_frame_logged[3] = {};

// Nothing to do for any window: no list hidden, every frame drawn and none left bare
bool idle()
{
    for (int i = 0; i < 3; ++i)
        if (g_hide[i] || !g_frame[i] || g_bare[i] != nullptr)
            return false;
    return true;
}

// List `i`'s frame and title drawn or not, as its key says
void frame(int i, const void* ctrl)
{
    std::uint8_t& pieces = *reinterpret_cast<std::uint8_t*>(static_cast<char*>(const_cast<void*>(ctrl)) + kWindowPieces);
    if (g_frame[i]) {
        if (g_bare[i] == ctrl)
            pieces = 1;
        g_bare[i] = nullptr; // (or that window is gone: there's one of each list)
        return;
    }
    if (pieces == 0)
        return;
    pieces = 0;
    g_bare[i] = ctrl;
    if (!g_frame_logged[i]) {
        g_frame_logged[i] = true;
        log::write(L"%ls: drawn without its frame and title ([client] %ls=0)", kHideNames[i], kFrameKeys[i]);
    }
}

// The party list's window drawing its owner's own gauge (the fishing gauge): drawn whatever hides it
bool gauge_window(const char* window)
{
    const char* owner = *reinterpret_cast<const char* const*>(window + kWindowOwner);
    return owner != nullptr && *reinterpret_cast<const std::uint8_t*>(owner + kOwnerGauge) != 0;
}

} // namespace

bool gauge(const void* ctrl)
{
    if (ctrl == nullptr)
        return false;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWindowLayout);
        return layout != nullptr && std::memcmp(layout + kLayoutName, kHideLayouts[0], 16) == 0 &&
               gauge_window(static_cast<const char*>(ctrl));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"alliance.macrobar", 1, ini) != 0;
    for (int i = 0; i < 3; ++i) {
        g_hide[i] = GetPrivateProfileIntW(L"client", kHideKeys[i], 0, ini) != 0;
        g_frame[i] = GetPrivateProfileIntW(L"client", kFrameKeys[i], 1, ini) != 0;
    }
    apply();
}

bool skip(const void* ctrl)
{
    if (idle() || ctrl == nullptr)
        return false;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWindowLayout);
        if (layout == nullptr)
            return false;
        for (int i = 0; i < 3; ++i) {
            if (std::memcmp(layout + kLayoutName, kHideLayouts[i], 16) == 0) {
                frame(i, ctrl);
                if (!g_hide[i])
                    return false;
                if (i == 0 && gauge_window(static_cast<const char*>(ctrl))) { // the fishing gauge: drawn
                    if (!g_gauge_logged) {
                        g_gauge_logged = true;
                        log::write(L"Party list: drawn for its own gauge (the hooked fish's HP) while [client] party.hide=1");
                    }
                    return false;
                }
                if (!g_hide_logged[i]) {
                    g_hide_logged[i] = true;
                    log::write(L"%ls: left out of the window draw ([client] %ls=1)", kHideNames[i], kHideKeys[i]);
                }
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_on != g_applied)
        apply();
}

} // namespace vanacore::alliancelist
