/* Where hudwin's windows go and over what: beside (a box kept on the screen by the window it opened from), moved (a
 * window its X and Y place, which another module's moving leaves alone), own (one of the table's), place (another
 * module's SetLocate, which locate passes through) and raise_moved (a moved window on its own layer, kOnTop). hudwin.cpp includes this inside its namespace, after its helpers (area, table_window, g_manager, at), whose
 * names it uses. It is no file of its own. */
#pragma once

// Window `window` put at (x, y) beside another open window and partly off the interface's left or right edge: x on that
// window's other side instead, as far into it, kept in the area; else x as it is. A list's options box is put this way
// (the equip sets' Name, Equipment, ... Back, "menu    mcresmn", by 0x101A5A40; a macro book's, "menu    mcrselop", by
// 0x101A5190: its right edge 17 into the list at the chosen row, x = the row's left less its width, once as it opens).
// With the Macro menu moved to the screen's left edge, the equip set options came up at -80, all but their last letters
// off the screen (reported 2026-11-06). The other window is the open one holding the box's inner edge at its top
int beside(char* window, int x, int y)
{
    const int width = at<short>(window, kWinFrame + 4) - at<short>(window, kWinFrame);
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    if (width <= 0 || g_manager == nullptr || !area(area_left, area_top, area_w, area_h))
        return x;
    const bool off_left = x < area_left, off_right = x + width > area_left + area_w;
    if (off_left == off_right) // (on the screen, or wider than it)
        return x;
    const int edge = off_left ? x + width - 1 : x;
    int count = 0;
    for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
        char* other = at<char*>(node, 0x10);
        if (other == nullptr || other == window || at<std::uint32_t>(other, kWinState) == kWinClosed)
            continue;
        const int left = at<short>(other, kWinFrame), top = at<short>(other, kWinFrame + 2);
        const int right = at<short>(other, kWinFrame + 4), bottom = at<short>(other, kWinFrame + 6);
        if (edge < left || edge >= right || y < top || y >= bottom || (off_left ? x >= left : x + width <= right))
            continue;
        return std::clamp(left + right - (x + width), area_left, std::max(area_left, area_left + area_w - width));
    }
    return x;
}

bool moved(const void* ctrl)
{
    if (ctrl == nullptr)
        return false;
    __try {
        const Window* table = table_window(static_cast<char*>(const_cast<void*>(ctrl)));
        return table != nullptr && (table->can & kMove) != 0 && table->move;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool own(const void* ctrl)
{
    if (ctrl == nullptr)
        return false;
    __try {
        return table_window(static_cast<char*>(const_cast<void*>(ctrl))) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

namespace {
const void* g_placing = nullptr; // the window place() is putting: locate leaves it where it's asked
} // namespace

void place(void* ctrl, int x, int y)
{
    if (ctrl == nullptr || !found())
        return;
    g_placing = ctrl;
    g_set_locate(ctrl, x, y);
    g_placing = nullptr;
}

namespace {

// The menu manager draws its windows in nine passes (FUN_1015E3F0's loop at 0x1015E41A), pass n drawing the windows whose
// +0x60 is n, each pass in the list's order. A window's +0x60 is its menu table entry's (+9 of the 0x2C entries at VA
// 0x10370288, set as it's made at 0x1015D708): the menus 2 to 4 (Status, Equipment, an item's description, the Items list
// all 2), the keypads and the macro editor 6 and 7, the Yes/No boxes 8. The menu code (0x10110000 - 0x10170000) reads a
// window's +0x60 nowhere else but the list's insert (0x1015C840: a window of 5 or more is kept at the list's head, a new
// one goes after those), read 2026-11-06. 5 is no window's.
// An item's description moved (itemdetails.move) onto the Equipment window, its stats or the Items list was drawn under
// them, being 2 and before them in the list: its frame cut and its True Font letters dropped where they covered it (reported
// 2026-11-06: "When it's moved it should be on its own layer"). On 5 it's drawn over every menu, under those boxes.
// The layer is the window's for the draw only (draw_all_hook), its own the rest of the frame. The list's insert keeps a
// window of 5 or more ahead of every other, and each frame the manager gives the keys to the first window of the list that
// can take them (0x1015E9B7: SetCurMCD(FUN_1015D510())): a window left on 5 that takes keys held them for good. The macro
// bar's Button Edit box (Copy, Paste, Delete: macroeditoptions.move) did: Esc handed the keys to the row (0x101A4860), the
// row went into the list behind the box, and the next frame the box had them again, so the menu couldn't be left
// (reported 2026-11-24: "this menu freezes and I can't exit it at all", a relog to get out).
constexpr std::ptrdiff_t kWinLayer = 0x60;
constexpr std::uint8_t kTopLayer = 5;
// The manager's draw of its windows (FUN_1015E3D0, the nine passes: thiscall, no arguments):
//   push ecx; push ebx; push esi; push edi; mov edi,ecx; call ...; mov ecx,[...]; test ecx,ecx; je +5; call ...;
//   mov ecx,[...]; call ...; mov eax,[edi+54h]; test eax,eax; je +1Ch; mov esi,[eax+88h]
constexpr char kDrawAll[] = "51 53 56 57 8B F9 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                            "8B 47 54 85 C0 74 1C 8B B0 88 00 00 00";
constexpr std::size_t kDrawAllPrologue = 6;
using DrawAll = void(__fastcall*)(char* manager, void* edx);
DrawAll g_draw_all = nullptr;
bool g_draw_all_searched = false;
struct Raised {
    char* window;
    std::uint8_t was; // its layer before
};
constexpr int kRaisedAtMost = 8;

// The table's kOnTop windows with their move on put on the top layer, into `out`: how many
int raise(Raised (&out)[kRaisedAtMost])
{
    int raised = 0;
    __try {
        int count = 0;
        for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 256 && raised < kRaisedAtMost;
             node = at<char*>(node, 0), ++count) {
            char* window = at<char*>(node, 0x10);
            if (window == nullptr || at<char*>(window, kWindowLayout) == nullptr)
                continue;
            const Window* table = table_window(window);
            std::uint8_t& now = at<std::uint8_t>(window, kWinLayer);
            if (table == nullptr || (table->can & kOnTop) == 0 || !table->move || now >= kTopLayer)
                continue;
            out[raised++] = {window, now};
            now = kTopLayer;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return raised;
}

// The `count` windows of `raised` set back on their own layers: those still in the manager's list (one gone from it may
// be freed)
void lower(const Raised* raised, int count)
{
    __try {
        int walked = 0;
        for (char* node = at<char*>(g_manager, 0); node != nullptr && walked < 256; node = at<char*>(node, 0), ++walked) {
            char* window = at<char*>(node, 0x10);
            for (int i = 0; window != nullptr && i < count; ++i)
                if (raised[i].window == window && at<std::uint8_t>(window, kWinLayer) == kTopLayer)
                    at<std::uint8_t>(window, kWinLayer) = raised[i].was;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// In place of the manager's draw: the moved kOnTop windows on the top layer while it draws
void __fastcall draw_all_hook(char* manager, void* edx)
{
    Raised raised[kRaisedAtMost];
    const int count = g_manager != nullptr ? raise(raised) : 0;
    g_draw_all(manager, edx);
    if (count > 0)
        lower(raised, count);
}

} // namespace

void raise_moved()
{
    if (g_draw_all_searched || g_manager == nullptr) // (as skip's found() left it, from the first window drawn)
        return;
    g_draw_all_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t draw = game != nullptr ? scan::find(game, kDrawAll) : 0;
    if (draw == 0 || !imports::detour(reinterpret_cast<DrawAll>(draw), kDrawAllPrologue, &draw_all_hook, g_draw_all)) {
        g_draw_all = nullptr;
        log::write(L"HUD windows: the game's draw of its windows wasn't found in FFXiMain.dll: a moved item description or macro "
                   L"Button Edit box isn't drawn over the other menus ([client] itemdetails.move, macroeditoptions.move)");
        return;
    }
    log::write(L"HUD windows: the game's draw of its windows hooked at +0x%X: a moved item description or macro Button Edit box "
               L"is drawn over the other menus, and takes the keys no sooner for it",
               static_cast<unsigned>(draw - reinterpret_cast<std::uintptr_t>(game)));
}
