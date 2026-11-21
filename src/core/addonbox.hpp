#pragma once

#include <string>

// The HUD Manager's Addon View (vana\hud.lua, vana.layout.box): a yellow box over each addon's and plugin's ImGui window and
// over each one's text and shapes (Ashita's fonts and primitives: layout.hpp's "Text and shapes##objects/<owner>"), as
// Addons > Layout knows them, each moved with Shift + drag as the HUD Manager's blue boxes move the game's windows. What a
// box is moved to goes through layout::set, so it's kept in config\layout.json as the Layout page's X and Y are.
//
// A window's box is drawn in that window's own draw list (its last child's, so it's over what the window shows), so a
// window in front of it covers it as it covers the window, and VanaCore's own windows (the HUD Manager) stay over them all.
// A box of text and shapes, which have no window, is drawn under ImGui's windows. While the mouse is over a box, the addon's
// window under it doesn't get the mouse (its buttons don't take a click meant for the box), nor does the game.
// Game thread.
namespace vanacore::addonbox {

// What box saw: `released` the frame a move is let go (a drag, or the arrow keys), with where the box went (x, y) and where
// it was before (from_x, from_y), in ImGui's pixels as vana.layout.windows() has them (the HUD Manager's undo)
struct Result {
    bool drawn = false; // the window (or text and shapes) is on the screen: it has a box
    bool released = false;
    float x = 0, y = 0, from_x = 0, from_y = 0;
};

// In ImGui's frame (vana.layout.box, each frame Addon View is on): window `name`'s box, `label` above its top left corner
// (empty: none) in the font ImGui has now. Shift + drag moves it, its top left corner to the grid `snap` pixels apart out
// from the screen's centre (0: anywhere); a click without Shift is box_clicked's. `picked` (the HUD Manager's addon): drawn
// green with a thick edge, and the arrow keys move it a pixel (Shift: a grid square while it snaps, else 10).
Result box(const std::string& name, const std::string& label, int snap, bool picked);

// The name of the box left-clicked (with or without Shift) since the last call, or empty (vana.layout.box_clicked). Taken:
// the next call is empty.
std::string clicked();

// From the game's keyboard reads (input.cpp): true while picked boxes are up (drawn in the last quarter second), when an
// arrow key (alone or with Shift) is box's and is kept from the game.
bool takes_arrow();

// Right after ImGui::NewFrame (gui.cpp), the mouse at (x, y) in ImGui's pixels as it is on the screen (not in a scaled
// window's own): over a box drawn last frame, the addon's window under it is no longer ImGui's hovered window.
void after_new_frame(float x, float y);

// Each frame, from gui::end_frame after layout::end_frame (the windows' scale applied): the boxes drawn.
void end_frame();

} // namespace vanacore::addonbox
