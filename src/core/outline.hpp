#pragma once

#include <string>
#include <vector>

// A game window's place shown as a box over the game, for /vana's eye on a position row (vana.layout.outline): drawn on
// ImGui's foreground where the window is (or would be, while it isn't open), and moved with Shift + drag.
namespace vanacore::outline {

// A place in the interface's units (the game's window frames: left, top, right, bottom)
struct Box {
    int left = 0, top = 0, right = 0, bottom = 0;
};

// What draw saw of a drag: while the box is held, `left` and `top` are where the mouse has taken its corner to (each
// frame, `held`); `released` the frame it's let go
struct Drag {
    bool held = false, released = false;
    int left = 0, top = 0;
};

// In ImGui's frame: `box` drawn (`cells` squares across it, as the status icons', 0 for none), Shift + drag on it taken
// from the game. `id` tells the windows apart (one is dragged at a time). Shift + right click picks a box (or no longer);
// a picked box dragged takes every other picked one with it, as far as it goes (it snaps, they don't), each's `drag` as
// if it were held, and released the frame after at most. An arrow key moves the picked boxes a unit, Shift + an arrow
// key a big step (a grid square while they snap, else 10 units), each's `drag` held there, and released when the keys
// are let go. A click
// without Shift in the game, or Escape, picks none. False if the box can't be put on the screen.
bool draw(const char* id, const Box& box, int cells, Drag& drag);

// The boxes picked: `id` alone (empty: none), as Shift + right click on it would leave them (vana.layout.select: /vana's
// HUD Manager's list of elements); and the ids picked now (vana.layout.selected).
void select(const std::string& id);
std::vector<std::string> selected();

// From the game's keyboard reads and messages (input.cpp): true while boxes are picked and up (drawn in the last quarter
// second), when an arrow key (alone or with Shift) is draw's and is kept from the game.
bool takes_arrow();

// True while boxes are up (drawn in the last quarter second): chat.cpp lays no game window over the ImGui windows then, as one
// the size of its box (an open HP gauge, the Spell info box) hid it, drawn at Present before the raised copies go back.
bool showing();

// The grid a held box snaps to, `pixels` apart out from the screen's centre, as grid draws it (0: none): a box's left
// edge (`right`: its right edge) and its top go to the nearest lines: its top-left corner, or top-right. Set before draw (vana.layout.outline's
// second and fourth arguments: /vana's HUD Manager's Anchor).
void snap(int pixels, bool right = false);

// A name written in the next drawn box's top left corner (/vana's HUD Manager's Show element names), or empty for none.
// Set before draw (vana.layout.outline's third argument); draw clears it.
void label(const char* text);

// The next drawn box goes with box `to` (`name` in its tooltip, e.g. "the Interaction Menu"): a sub-menu with no place of
// its own, moved by its parent's. Drawn with a chain link in its top-right corner, and amber while `to` is dragged. Set
// before draw; draw clears it.
void docked(const char* to, const char* name);

// The id of the box left-clicked without Shift since the last call (the smallest, of boxes over each other), or empty:
// /vana's HUD Manager shows its window's section (vana.layout.clicked). Taken: the next call is empty.
std::string clicked();

// In ImGui's frame: lines `pixels` apart over the whole screen, out from its centre, behind ImGui's windows (/vana's HUD
// Manager grid, vana.layout.grid). `color` is an ImU32 (0xAABBGGRR); `centre` the two 3-pixel lines through the centre's;
// `fill` (0: none) the screen under the lines, the same.
void grid(int pixels, unsigned color, unsigned centre, unsigned fill = 0);

} // namespace vanacore::outline
