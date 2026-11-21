#pragma once

#include <string>
#include <vector>

// The game's windows laid over everything drawn after the game (addons' fonts, sprites and ImGui windows, VanaCore's own):
// the auto-translate list (phraselist.cpp) and the windows over fancychat's chat and over the ImGui windows of addons that
// ask (vana.ui.game_on_top; chat.cpp). Each frame a window is wanted on top, its pixels as the game drew them are copied at the game's scene end on
// the back buffer, before plugins and addons draw there (graphics::raise_take, or a copy of the back buffer's with Windows'
// Direct3D 8 or 9), and laid back at Present after VanaCore's own drawing. The mouse over them is the game's: ImGui's
// windows under them don't take it (gui::set_covered). Everything here runs on the game's thread.
namespace vanacore::raise {

// The most windows a frame (the renderer's D3D8_RAISE_MOST): past that, a window is joined to the last one's place.
constexpr int kMost = 8;

// vana.ui.game_on_top: the addon `owner` (gameui::key's, "vanacore/hello"; layout::owner_of's for its ImGui windows) asks for
// the game's windows to go over its ImGui windows where they meet (on), or no longer does. An addon that unloads is dropped
// (addons.cpp). Game thread.
void game_on_top(const std::string& owner, bool on);

// The addons asking now, for chat.cpp's look at where their windows are each frame. Game thread.
const std::vector<std::string>& game_on_top_owners();

// A window the game has drawn this frame, to go on top: its rect (left, top, right, bottom) in the interface's units. One
// inside another already added isn't added again.
void add(const short rect[4]);

// True when a window was added this frame and isn't copied yet.
bool pending();

// At the game's scene end on the back buffer, before plugins and addons draw there: the windows added since the last take,
// as the game drew them, copied.
void take(void* device);

// After VanaCore's own drawing at Present: the copies laid back over the back buffer and VanaCore's interface, and their
// places given to the mouse for the frames until the next put.
void put(void* device);

// The windows the last put() laid back (their rects in the interface's units) into `rects`, `most` at most: how many. For
// VanaCore's text over them, drawn again on top (overlay.cpp, text::replay).
int laid(short (*rects)[4], int most);

// The copies' render targets let go (Windows' Direct3D 8 or 9 keeps them between frames, in the device's default pool): before
// the device's Reset, which fails while they're there.
void release();

} // namespace vanacore::raise
