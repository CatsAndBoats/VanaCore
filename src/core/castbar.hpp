#pragma once

// [client] castbar.*: the cast bar (the bar that fills while a spell is cast or an item is used)
//   castbar.hide=1   left out of the game's window draw, as party.hide does for the party list. Casting is unchanged;
//                    only the bar isn't drawn.
//   castbar.background=0   drawn without its frame: the gauge and the percent stay. Default: 1
//   castbar.move=1   placed by castbar.x (its middle) and castbar.y (its top edge), percents of the interface; the
//                    defaults are the game's own place (the top left). Off, the game places it
namespace vanacore::castbar {

// `ini` (vanacore.ini): [client] castbar.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// From chat.cpp's hook of the game's window draw, before the window is drawn: true if `ctrl` (a window of the menu
// manager) is the cast bar and its drawing is to be left out. The cast bar is put where castbar.move says here, before
// it's drawn, so the frame a cast opens it in shows it there too. Called for every window drawn, on the game's thread.
bool skip(const void* ctrl);

// In ImGui's frame (vana.layout.outline('castbar'), /vana's eye on the Cast Bar row): a box on ImGui's foreground where
// the bar goes, so the place can be set with no cast on. False if it can't be worked out. Shift + drag moves it
// (castbar.move on, castbar.x and castbar.y as it goes, not written): the frame it's let go, moved_x and moved_y are set
// to where it was left, for the caller to save.
bool outline(double* moved_x = nullptr, double* moved_y = nullptr);

} // namespace vanacore::castbar
