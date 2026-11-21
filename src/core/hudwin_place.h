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
constexpr std::ptrdiff_t kWinLayer = 0x60;
constexpr std::uint8_t kTopLayer = 5;
struct Raised {
    char* window;
    std::uint8_t was; // its layer before
    bool seen;        // in the manager's list this frame
};
constexpr int kRaisedAtMost = 8;
Raised g_raised[kRaisedAtMost] = {};
int g_raised_count = 0;

Raised* raised(const char* window)
{
    for (int i = 0; i < g_raised_count; ++i)
        if (g_raised[i].window == window)
            return &g_raised[i];
    return nullptr;
}

// Window `window`: raised while it's one of the table's kOnTop with its move on, set back once that's off
void layer(char* window)
{
    const Window* table = table_window(window);
    const bool top = table != nullptr && (table->can & kOnTop) != 0 && table->move;
    std::uint8_t& now = at<std::uint8_t>(window, kWinLayer);
    Raised* was = raised(window);
    if (was != nullptr)
        was->seen = true;
    if (top && now < kTopLayer) {
        if (was == nullptr && g_raised_count < kRaisedAtMost)
            was = &(g_raised[g_raised_count++] = {window, now, true});
        if (was != nullptr) {
            was->was = now;
            now = kTopLayer;
        }
    } else if (!top && was != nullptr && now == kTopLayer) {
        now = was->was;
        was->window = nullptr; // (dropped below)
    }
}

} // namespace

void raise_moved()
{
    if (g_manager == nullptr) // (as skip's found() left it, from the first window drawn)
        return;
    for (int i = 0; i < g_raised_count; ++i)
        g_raised[i].seen = false;
    __try {
        int count = 0;
        for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 256; node = at<char*>(node, 0), ++count)
            if (char* window = at<char*>(node, 0x10); window != nullptr && at<char*>(window, kWindowLayout) != nullptr)
                layer(window);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    int kept = 0; // (one gone from the list is forgotten: its address may be another window's next)
    for (int i = 0; i < g_raised_count; ++i)
        if (g_raised[i].window != nullptr && g_raised[i].seen)
            g_raised[kept++] = g_raised[i];
    g_raised_count = kept;
}
