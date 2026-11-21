#pragma once

#include <cstdint>
#include <string>

// The game's menus in vanacore.log while Record Stats Report runs (/vana > Stats), for reports of stuck menus. Each line
// starts "Menus:". Logged: each game window as it opens, goes or changes (its state, its cursor and count of buttons, the
// button pieces it really has, the cursor its layout keeps for its next opening, its owner); the window the keys go to;
// the arrow keys' steps in a menu and who answered them (the game, or VanaCore's extra rows); the menu keys and pad
// buttons, and who kept any from the game (an addon by name, a plugin, a key bind, an ImGui window); and VanaCore's own
// hold on a selection menu ([client] menu.rows). A value that can't be right is marked "!" (a cursor on no button, an
// owner whose window is another, a window gone while still open). At most 4000 lines a recording. Nothing is read while
// no recording runs. One line is logged always: an Options box (the Items list's "itmsort2", the Magic list's
// "mgcsortw"...) made the window the keys go to, with the code that asked and the window changes before it.
namespace vanacore::menulog {

// A recording runs: the logging is on
bool on();

// Each frame, from Present, on the game's thread: the menu manager's windows looked at, and what changed logged
void on_frame();

// A line of a module's own about a menu (prefixed "Menus: "), while a recording runs
void note(const char* text);

// When (GetTickCount64) the game last made another window the one the keys go to (its SetCurMCD, hooked for good); 0 before
// the first. menuhover.cpp keeps a window under the pointer from taking the keys for a moment after.
unsigned long long focus_changed_at();

// The menu manager's windows in one line, for another module's log line: the window the keys go to, then each window that
// isn't closed (its layout's name, address and state). Read on the game's thread, whether or not a recording runs.
std::string windows_text();

// The game's step of a window's cursor for a direction key (FUN_1011AE20, chat.cpp's jump_tag_hook): in the window whose
// layout is `data`, from `cursor` with `key` (the game's direction, 0-3) to `result`, answered by `by`
void key_step(const void* data, int cursor, int key, int result, const char* by);

// A keyboard event (DirectInput: the key's scan code, pressed or let go) and who kept it from the game (null: it reached
// the game). A press that reached the game is logged for the keys menus use (arrows, Enter, Escape, Tab, minus, the
// numpad, Page Up/Down, Home/End, Insert/Delete, the function keys); any key kept from the game is, by its code
void key(std::uint32_t code, bool down, const char* kept_by);

// The game's held keys (DirectInput's 256-byte state) before and after the addons' key_state: a menu key held that an
// addon let go is logged as it starts and as it ends
void key_state(const std::uint8_t* before, const std::uint8_t* after);

// A pad button's change (XInput's button 0-15, or a DirectInput offset: the D-pad 0x20 up, the buttons 0x30 up), as it
// was before plugins and addons saw it, and who kept it from the game (null: it reached the game). Sticks and triggers
// aren't logged
void pad(bool xinput, std::uint32_t button, std::int32_t value, bool injected, const char* kept_by);

} // namespace vanacore::menulog
