#pragma once

#include "nameplates.hpp"

struct lua_State;

// Pictures an addon puts in the game's 3D space instead of on the screen: a vana.ui box (text, rectangle or image) placed
// at a character's nameplate (vana.world.attach) or at a point in the world (vana.world.place). Both are drawn by
// VanaCore as screen-space quads in the pass that draws the names over heads, at a depth the world's own depth tests,
// so walls hide them and the effects, DLSS and frame generation treat them as they treat a name (docs/addons/18-widgets.md).
// A placed box isn't drawn at its x and y any more (ui::Box::placed). Only used from the game's thread.
namespace vanacore::widgets {

// vana.world: a new table of the calls, left on the stack
void push(lua_State* L);

// Whether any widget is placed, and whether any is in the world: nameplates.cpp hooks the names' drawing for them. Boxes an
// addon has destroyed are let go here.
bool any();
bool any_world();

// nameplates.cpp, around the game's call drawing a character's name: whether a widget is at the name of target `index`, and
// (after it is drawn, or laid out and left undrawn when the settings or an addon hide it) the widgets drawn at its place
bool attached(unsigned index);
void draw_at_name(unsigned index, const nameplates::Slot& slot);

// Where the name over target `index` (at x, y, z in the world) is on the screen, in the interface's pixels (vana.ui.screen's):
// its place across and the letters' top and bottom as drawn lately. False while the name wasn't drawn lately. For
// vana.world.name_at and vana.game.entity's name_x, name_top and name_bottom.
bool name_on_screen(unsigned index, float x, float y, float z, float& across, float& top, float& bottom);

// nameplates.cpp, before the game's walk of the characters that draws their names (the names go over them): the widgets at
// points in the world
void draw_world();

// overlay.cpp, with the interface (the boxes of vana.ui): the widgets in the world that are drawn over it
void draw_over_ui(void* device);

// overlay.cpp, at each Present: the next names' pass draws the widgets in the world again. The game walks its characters'
// names twice in a frame now and then; drawn at both, a widget see-through in the world was drawn twice over itself
// (a beam of light flashed brighter, or doubled, for that frame), so only the frame's first walk draws them.
void on_present();

// graphics.cpp, as [graphics] widgets.log is read (/vana > Client > Developer > World widgets): the widget watch on or off,
// which logs a frame that draws the widgets in the world unlike the frame before (walks, boxes, sizes, the camera)
void set_watch(bool on);

} // namespace vanacore::widgets
