#pragma once

// [client] buffs.*: the player's status icons (the row of buff icons under the help bar at the screen's top left):
//   buffs.move=1   placed by buffs.x (its left edge) and buffs.y (the bottom of its first row of icons), percents of the
//                  interface; the defaults are the game's own place. Off, it goes back where the game opened it
//   buffs.scale=N  the icons' size, a percent of the game's (25 to 300, default 100): each icon and its timer drawn N percent
//                  as big, the grid scaled from the window's corner (with buffs.move on, the bottom of the first row stays
//                  at buffs.y), the square that picks an icon drawn the same, and a click or the pointer taken to
//                  the icon it's on
//   buffs.columns=N  how many icons go in a row before the next row starts (1 to 36, default 9, the game's own): the
//                  window's buttons laid out N to a row, so the game draws each icon and its timer there, the square
//                  that picks one goes with it and the mouse picks it there (32 puts every status on one line)
namespace vanacore::buffwin {

// `ini` (vanacore.ini): [client] buffs.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread, after Present: puts the window where it's wanted (a window the game made again, at a
// zone change, is found again).
void on_frame();

// The game's draw of a window's control, bracketing it (chat.cpp): the square drawn inside it for the status icons
// are scaled only in the status icons' window.
const void* enter_window(const void* ctrl);
void leave_window(const void* outer);

// The mouse's `point` (x, y, the interface's units) for the status icons' window `window`, taken back by the scale to where
// the game laid the icons out (it asks which button a point is on, 0x101188b0): true with `mapped` set, false when the
// window isn't the status icons' or the scale is 1 (the point stands).
bool map_point(const void* window, const int* point, int mapped[2]);

// In ImGui's frame (vana.layout.outline('buffs'), /vana's eye on the Status Icons row): a box on ImGui's foreground where
// the first row of icons goes, a square for each icon in a row (buffs.columns), so the place can be set with no statuses on. False if it can't be worked out.
// Shift + drag moves it, the icons with it (buffs.move on, buffs.x and buffs.y as it goes, not written): the frame it's let
// go, moved_x and moved_y are set to where it was left, for the caller to save.
bool outline(double* moved_x = nullptr, double* moved_y = nullptr);

} // namespace vanacore::buffwin
