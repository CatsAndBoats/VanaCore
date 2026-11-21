#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// Plugins' own drawing moved and scaled as Addons > Layout (and the HUD Manager's Addon View) has their "Text and shapes":
// what a plugin draws on the device itself (Ashita's Minimap in its Direct3D callbacks, Windower's FFXIDB minimap in its
// PreRender), not only the fonts and primitives VanaCore draws for it. Around each plugin's drawing callbacks the renderer
// is told whose draws come (graphics::set_mover): it notes where that plugin draws on the screen, and moves its screen-space
// draws by its owner's move. Ashita's fonts.cpp puts each plugin's box in with its fonts' and primitives' (one "Text and
// shapes" box an owner, layout::objects) and hands the moves back here; the plugin's mouse callbacks get the mouse back in
// its own place (unmoved), so its clicks, drags and wheel still find what it drew. Game thread, but unmoved (any).
namespace vanacore::plugin_move {

// Around a plugin's drawing callbacks (Ashita's Direct3DBeginScene, EndScene and Present; Windower's frame, PreRender and
// PostRender): `owner` its name, as Ashita's fonts.cpp keys a plugin's objects (its display name); empty after.
void drawing_as(const std::string& owner);

// Around a font or primitive VanaCore draws for a plugin by hand amid its drawing (Render): already moved, left as it is.
void pause();
void resume();

// Where each plugin drew on the screen in the last frame, as it put it (unmoved): x0, y0, x1, y1 in the game's pixels
struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
std::vector<std::pair<std::string, Box>> boxes();

// What Addons > Layout gave each owner's objects this frame (fonts.cpp, from layout::objects): drawn scaled from (ox, oy),
// the top-left corner of all of it, then moved by (dx, dy). An owner not in it is drawn where it puts itself.
struct Move {
    float ox = 0, oy = 0, dx = 0, dy = 0, scale = 1;
};
void set_moves(std::map<std::string, Move> moves);

// A point (the game's pixels) on what `owner` drew, back where it put it: its mouse callbacks see their own place
void unmoved(const std::string& owner, int& x, int& y);

} // namespace vanacore::plugin_move
