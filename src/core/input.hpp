#pragma once

#include <string>
#include <vector>

namespace vanacore::input {

// Reads [input] from `ini` and hooks the game's input handling to match:
//   gamepad.disableenumeration=1  controllers aren't looked for at all (fixes the stutter of the
//                                 game's periodic device search; no controller works while on)
//   keyboard.windowskeyenabled=1  the Windows keys work normally instead of being swallowed (the default)
//   keyboard.hookthread=1         the game's low-level keyboard hook runs on a thread of its own (the default; 0 under
//                                 Wine); 0 keeps it on the game's thread, as the game sets it
//   mouse.unhook=1 (default)      the game can't move the mouse cursor (menus jumping it around)
// Controllers in the background follow the game's own "works while inactive" switch (gamepad.mode's fifth), held to it
// by window::in_background(); Ashita's gamepad.allowbackground isn't used.
void install(const wchar_t* ini);

// true when a mouse button goes down on the game window's frame (its title bar, edges), false when
// Windows is done moving or sizing it: meanwhile the game's DirectInput mouse reads nothing, and
// what it buffered is dropped, so dragging the window doesn't turn the camera.
void frame_press(bool down);

// true when a modal window over the game (an addon's file dialog, a message box) disables the game's window, false when
// it's enabled again: meanwhile the game's DirectInput mouse and keyboard read nothing; after, what they buffered is
// dropped, and the buttons and keys still held (the double click or Enter that closed it) stay hidden until let go.
void modal(bool open);
// A modal window is over the game, or closed less than half a second ago, or a button held as it closed is still down: the
// game gets no mouse buttons at all, from DirectInput or its window's messages (a double click on a file in an import's
// dialog clicked the game, or left it holding a click).
bool modal_quiet();

// Hides the key with `scan_code` (a DirectInput key code) from the game's DirectInput keyboard until
// it's released: a key a bind took, the V of a Ctrl+V that VanaCore pasted with, so the game's own V
// (first-person view) doesn't act on it too. For the window's message: the keyboard's reads hide such
// a key on their own as well, as soon as one shows it pressed (whichever comes first).
void hide_key(unsigned scan_code);

// A plugin or addon took a mouse event from the window's messages (FFXIDB's minimap): the game reads
// the mouse through DirectInput too, so there it goes without it as well. A press: no buttons until
// they're all up; a wheel turn: no wheel for its next reads.
void mouse_taken(bool press, bool wheel);

// Windows says a device came or went (WM_DEVICECHANGE): the game's next controller search runs for real, not from the
// result kept from the last one.
void device_changed();

// A DirectInput controller Windows lists: its instance GUID (what the game saves as padguid000), name, DirectInput
// device type, whether the game made a device for it this session, whether the game reads it now (DirectInput, the
// last 2 seconds), and whether it's an XInput controller (with XInput on, the game reads it that way instead).
struct Controller {
    std::string guid;
    std::string name;
    std::string product;
    unsigned type = 0;
    bool opened = false;
    bool reading = false;
    bool xinput = false;
};

// The controllers Windows lists (DirectInput), the XInput slots with one connected, and the slots the game read a
// connected pad from in the last 2 seconds. A copy of the last search, which runs again when asked (`refresh`) or after
// a device came or went: cheap to call every frame.
struct Gamepads {
    std::vector<Controller> controllers;
    bool xinput[4] = {};
    bool xinput_reading[4] = {};
};
Gamepads gamepads(bool refresh);

// The game's own gamepad switches as they are now, not as the settings file has them (a controller's profile or a pick
// in /vana changes them while it runs): the gamepad on, XInput on (padmode000's first and sixth). `known` is false for a
// client whose pad code isn't the one this was written for.
struct GameGamepad {
    bool known = false;
    bool on = false;
    bool xinput = false;
};
GameGamepad game_gamepad();

// Makes the game use the controller with this instance GUID ("{...}", all zeros: Auto-detect) now, without a restart:
// the game's own change-the-pad path, which closes the pad it has and opens that one on its next frame. False, with
// `why`, when it isn't a GUID or this client's pad code isn't the one it was written for.
bool select_gamepad(const std::string& guid, std::string& why);

// A pad's whole configuration, as the game's gamepad config tool saves it (vanacore.ini's [ffxi] gamepad.mode, .buttons,
// .buttons_ex, .guid, as text; an empty one is left as the game has it).
struct GamepadSettings {
    std::string mode;
    std::string buttons;
    std::string buttons_ex;
    std::string guid;
};

// Applies them to the running game through its own setters: the pad closes and opens again on the game's next frame
// where the mode or GUID changed. False, with `why`, when this client's pad code isn't the one this was written for
// (then nothing has changed).
bool apply_gamepad(const GamepadSettings& settings, std::string& why);

// [input] gamepad.disable, live: while on, the game's controller search finds nothing, XInput reports no controllers,
// and a controller the game already has reads as let go (plugins' and addons' controller events too)
void disable_gamepads(bool off);
// true while no controller works (gamepad.disable, or gamepad.disableenumeration from the start)
bool gamepads_disabled();

} // namespace vanacore::input
