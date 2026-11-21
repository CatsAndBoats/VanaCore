#pragma once

// [input] fishing.dpad (default 1): a hooked fish is fought with the controller's D-pad too. The game's fish fight takes
// its left and right from the movement controls only (the movement stick, numpad 4 and 6, two key binds), and the D-pad
// is the menus' left and right in the game's button map, so it did nothing there without remapping the pad. With this
// on, when the movement controls give no direction, the fight takes the menus' left and right: the D-pad, or whatever
// the player's own button map has there, DirectInput and XInput alike. A press counts once, as the stick's does.
namespace vanacore::fishing {

// `ini` (vanacore.ini): [input] fishing.dpad read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: the fight's read patched the first time it's wanted (once FFXiMain.dll is
// unpacked).
void on_frame();

} // namespace vanacore::fishing
