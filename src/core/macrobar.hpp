#pragma once

// [client] macrobar.mode: how the macro bar's keys (Alt and Ctrl, the controller's triggers) show the bar. `game`: the
// game's own (a tap toggles it; a hold shows it once the game's second counter moves on, anywhere from at once to a
// second after the key goes down). `toggle`: a press shows it, and it stays up until the key is pressed again or Esc.
// `hold`: it shows while the key is held, at once, and goes when the key is let go (what the macrofix addon does).
// `toggle` and `hold` have no wait; [client] macrobar.nodelay=1 takes the wait out of the game's own mode for the
// keyboard's Ctrl and Alt alone (the triggers stay the game's: a tap toggles, a hold waits).
//
// The macrofix addon changes the same bytes as these, so install() holds it off (addons::hold): /addon load macrofix, and
// boot.txt's, say it isn't loaded and what to set instead.
//
// Always logged, a few lines a session ("Macro bar: ..."): what the game's _canUseMacro says no with (a menu window left up,
// an event, a game flag) when Ctrl or Alt has been held 2 s and no bar is up, or macros have been blocked for 20 s; the
// controller's own state gone stale; its key handling not running. For "Ctrl shows no bar until I zone" reports.
//
// [client] macrobar.move=1: the bar (either palette) put with its left edge macrobar.x percent across the interface and
// its top edge macrobar.y percent down, in place of the game's top left.
namespace vanacore::macrobar {

// `ini` (vanacore.ini): [client] macrobar.mode read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: patches the game's key handling the first time it's wanted (once FFXiMain.dll is
// there), and puts the bar where macrobar.move says while it's up (the first frame it's on, the game's opening of a menu
// is hooked too, so the bar is put there as it opens, not a frame later).
void on_frame();

// In ImGui's frame (vana.layout.outline('macrobar')): the bar's place drawn as a box (outline.hpp), where macrobar.x and
// macrobar.y put it while it isn't up. Shift + drag moves it (macrobar.move on, macrobar.x and .y as it goes, not
// written): the frame it's let go, moved_x and moved_y are where it was left, for the caller to save.
bool outline(double* moved_x = nullptr, double* moved_y = nullptr);

} // namespace vanacore::macrobar
