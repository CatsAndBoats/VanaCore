#pragma once

#include <windows.h>

// The game's interface area: the units its windows are placed in (FFXiMain's left, top, right and bottom of it), and
// where a point in it lands in VanaCore's own drawing. For VanaCore's text drawn over a game window (items.count,
// network.icon=dot).
namespace vanacore::uispace {

// Finds the area in `game` (FFXiMain.dll, unpacked) the first time; true once it has been found.
bool find(HMODULE game);

// (x, y) in the interface's units as the window's pixels (`width` x `height`), squeezed toward the middle as the game's
// drawing is for [ui] aspect, and `scale` the pixels a unit (up and down). False if the area isn't known (or is empty).
bool to_pixels(float x, float y, unsigned width, unsigned height, float& px, float& py, float& scale);

// The same with no squeeze: for what the game draws over the world rather than in the interface (the arrow over the
// target, which the renderer leaves where the game put it under [ui] aspect).
bool to_pixels_plain(float x, float y, unsigned width, unsigned height, float& px, float& py, float& scale);

// The other way: (px, py) in the window's pixels (`width` x `height`, where addons draw) as the interface's units, the
// squeeze undone. For a game window placed by something an addon draws (the auto-translate list over fancychat's bar).
bool to_units(float px, float py, unsigned width, unsigned height, float& x, float& y);

// The interface area's left, top, right and bottom; false while it isn't known.
bool area(int& left, int& top, int& right, int& bottom);

} // namespace vanacore::uispace
