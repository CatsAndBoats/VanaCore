/* Windows that go under another of their element's (kUnder): the macro bar's Alt row under its Ctrl row while it's edited.
 * hudwin.cpp includes this inside its namespace, just before place, after its helpers' declarations (open_window) and the
 * table (g_windows), whose names it uses. It is no file of its own. */
#pragma once

bool named(char* window, const char* name);

// the first of each pair goes under the second, at its left and its bottom: the macro bar's Alt row under its Ctrl row while
// Edit Macro edits it ("mcr2edit" under "mcr1edit", the long bar's "mcr2edlo" under "mcr1edlo"; macroedit). The game opens
// the Alt row where the Ctrl row is laid out and puts it 42 lower once, as it opens (its handler's open, 0x101A3630: its own
// rect + 0x2A, read 2026-11-11), so the Ctrl row moved alone left it behind. Each is in its element's layouts (its hide,
// frame and Menu Content); place puts it under the other, not at the element's X and Y, and the element's box is both rows
constexpr char kUnder[][2][16 + 1] = {{"menu    mcr2edit", "menu    mcr1edit"}, {"menu    mcr2edlo", "menu    mcr1edlo"}};

// `window` put under `above`, at its left and its bottom, if it isn't there. Put as asked (hudwin::place): taken for the
// game's, a place in the top-left 512 x 448 would get Menu Content's shift as well, every frame
void put_under(char* window, char* above)
{
    const int x = at<short>(above, kWinFrame), y = at<short>(above, kWinFrame + 6);
    if (at<short>(window, kWinFrame) != x || at<short>(window, kWinFrame + 2) != y)
        vanacore::hudwin::place(window, x, y);
}

// From place, before a window of the table's is drawn: if it's one that goes under another of its element's, put there
// (when that one's up), moved or not, and true (place leaves it at that: the other's box takes it in, below_of)
bool went_under(char* window)
{
    for (const auto& pair : kUnder) {
        if (!named(window, pair[0]))
            continue;
        char* above = open_window(pair[1]);
        if (above != nullptr && g_set_locate != nullptr)
            put_under(window, above);
        return true;
    }
    return false;
}

// The window that's up and goes under `window` (the macro bar's Alt row under its Ctrl row), or null; `height` its height
char* below_of(char* window, int& height)
{
    height = 0;
    for (const auto& pair : kUnder) {
        if (!named(window, pair[1]))
            continue;
        char* below = open_window(pair[0]);
        if (below != nullptr)
            height = std::max(0, at<short>(below, kWinFrame + 6) - at<short>(below, kWinFrame + 2));
        return below;
    }
    return nullptr;
}
