#pragma once

// [client] tips.extended (on by default): an item's details show everything on one page. The game splits a long item's
// details into up to three pages of 12 rows, and an item's augments always go on a page of their own (the details' second
// page, behind the arrow in its corner), so a piece of augmented gear showed its augments only when the page was turned.
// With this on, the pages are put together into one, in order: the name and type, the description and stats, the augments,
// the job list and the notes under it; the arrow and the page turn go. Past 12 rows the box is made taller by the game's
// own resize, a row (16 of the interface's pixels) at a time. Off, the game's pages as they were. (The same resize sizes a
// True Font tooltip's box to the pixel: set_lines' `extra`.)
namespace vanacore::itemtips {

// `ini` (the settings file): [client] tips.extended read now, and again by reload whenever it changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: the details' page building and page turn hooked the first time it's on (once
// FFXiMain.dll is unpacked), and their 12-row limits (the rows drawn, the rows counted) widened while it's on.
void on_frame();

// The item details' SetLines (CTkItemInfo, 0x1013BCC0: __thiscall, its count of rows; the game holds it to 3 to 12), as
// chatfont.cpp's hook of it gives it on: `lines` rows, past 12 the game's 12-row box made that much taller, `extra` of the
// interface's pixels more (less below 0: True Font's tooltips' paddings, line height, parts) and as wide as `wide` says
// (given the box as the game laid it, left, top, right, bottom; 0 as it is: True Font's tooltips' larger parts), by the
// game's resize, to the pixel. `again`: only sized again (its rows measured since), never laid out. Where the resize wasn't
// found, `extra` as whole rows.
using SetLines = void(__fastcall*)(void* self, void* edx, int count);
using Wide = int (*)(int left, int top, int right, int bottom);
struct Size {
    int lines = 0;
    int extra = 0;
    Wide wide = nullptr;
    bool again = false;
};
void set_lines(void* self, const Size& size, SetLines original);

// [client] tips.empty.hide (on by default): the window `ctrl` (chat.cpp asks of each before its draw) is an empty box, so
// it's left out (closed, it still gets its calc, its delete: hudwin::left_out). `details` (an item's details window): it
// shows no item, the cursor on an empty slot (its details' item, at +1Ch, none: their draw draws nothing in the box). Else
// the comment box with nothing in it (the Check window's on View Wares for a player with no bazaar message). False when
// it's off.
bool empty(const void* ctrl, bool details);

// How much wider than the game laid it the details' box `self` was made (set_lines' `wide`), the interface's pixels: 0 none
int widened(const void* self);

// For hudwin.cpp's hook of the game's SetLocate: how far window `window` was moved down, under a details box made taller
// than the game's (the auction house's gil and price windows under a bid's details, still where VanaCore put them), else 0.
// Done once a frame (on_frame) and as a box is made taller: the open windows the game laid out at the game's bottom of the
// box, across it, go down to just under it, and back up as it shrinks
int lowered(const void* window);

} // namespace vanacore::itemtips
