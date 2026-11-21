#pragma once

#include <windows.h>

// [ui] hide.key (F11 unless vanacore.ini says otherwise): hides the game's interface and shows it again, as the game's
// own hide mode does (CTkMenuMng's, in the PS2 client's names): its windows, the chat log and the bars go, the world
// and the names over heads stay. VanaCore's own drawing goes with it (the label, the FPS counter, addons' text and
// windows, Ashita's fonts). As in the game, it works in the world only, and Escape, a cutscene or leaving the world
// bring the interface back.
namespace vanacore::hideui {

// `ini` (vanacore.ini): [ui] hide.key, a key's name as /bind takes it (f11, scrolllock, numpad0, ...), or off; [ui]
// hide.camera, 1 (the default) to let the mouse turn the camera while the interface is hidden, which the game doesn't;
// [camera] mouse.hidecursor, 1 to hide the cursor while the right button turns the camera. Read now, and again by
// reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Each frame, on the game's thread: hide.camera's patch to the game's mouse frame put in or taken out as it says, and
// the cursor hidden or shown again for mouse.hidecursor.
void on_frame();

// A key message from the game's window (WM_KEYDOWN, WM_SYSKEYDOWN, WM_KEYUP, WM_SYSKEYUP): true if it's hide.key, which
// the game then doesn't see; its press (not a repeat) hides the interface or shows it again.
bool on_key(UINT message, WPARAM wparam, LPARAM lparam);

// True while the game's interface is hidden (by hide.key or by the game itself): VanaCore draws nothing of its own
// meanwhile. Game thread.
bool hidden();

// The interface hidden or shown as the key does (hiding works in the world only, as with the key; showing anywhere): true if
// it's hidden afterwards. False too if the game's hide mode wasn't found. Game thread.
bool set_hidden(bool hide);

// True while one of the game's menus is up (the main menu, a shop): a window has the cursor, and the Help and Network
// windows show their frames. False with none: the Network window's contents are then drawn without a frame
// (CTkMenuMng::HaveActiveWindow). True too if the manager wasn't found. Game thread.
bool window_up();

// True while the mouse is over the game's current menu (the main menu, a list), or drags or scrolls in it: the wheel is that
// menu's, not the camera's. False with no menu up, or the mouse object not found. Game thread.
bool mouse_over_window();

// The mouse buttons the game holds down, by its own mouse object (bit 1 left, 2 right, 4 middle); 0 with none, or with the
// object not found. Game thread.
unsigned game_mouse_buttons();

// The game's mouse object forgets every button (held, repeating, pressed): a click it's mid-way through is dropped. Game
// thread.
void clear_game_mouse_buttons();

} // namespace vanacore::hideui
