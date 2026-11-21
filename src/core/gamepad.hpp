#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Controllers' events, as Ashita has them: each change of a controller's button, trigger, stick or hat as the game reads
// it goes to plugins' controller callbacks (IController, IXInput) and addons' 'dinput_button' / 'xinput_button' events,
// which may change or block it; the game reads what they leave. A change is the control's own (an edge): one a handler
// blocks stays unseen until the control changes again, and a press a plugin queues (QueueButtonData) holds until then.
// Stick movement inside the dead zone raises nothing unless asked (SetTrackDeadZone), though the game still reads it;
// triggers have none (as in Ashita). Nothing is changed while no plugin or addon listens.
//
// XInput's controls, by Ashita's numbers: 0-15 the bits of wButtons (0-3 the D-pad up, down, left, right; 4 Start, 5
// Back, 6 and 7 the sticks pressed, 8 LB, 9 RB, 12 A, 13 B, 14 X, 15 Y), 1 down and 0 up; 16 and 17 the triggers
// (0-255); 18-21 the sticks (left X, left Y, right X, right Y). DirectInput's: their offsets in DIJOYSTATE (DIJOFS_X ...
// DIJOFS_BUTTON(n)), buttons 0x80 down.
//
// [camera] view.reset: the game's Change View button (its first-person view), found in its button map, is hidden from the
// game after the handlers while camera.cpp turns the camera round behind the character for it.
namespace vanacore::gamepad {

// A DirectInput controller's state read (DIJOYSTATE, 80 bytes, or DIJOYSTATE2, 272): changed in place
void on_dinput_state(void* device, DWORD size, void* state);
// Its buffered reads (DIDEVICEOBJECTDATA entries): each through the events, changed in place; blocked ones dropped
void on_dinput_data(void* device, DWORD object_size, void* data, DWORD* count);
// XInputGetState's result for controller `user` (XINPUT_STATE, 16 bytes): changed in place
void on_xinput_state(DWORD user, void* state);

// Plugins' QueueButtonData: a control set as if pressed (or moved), at the next read
void queue_dinput(DWORD offset, DWORD value);
void queue_xinput(std::uint8_t button, std::int16_t value);

bool track_dead_zone(bool xinput);
void set_track_dead_zone(bool xinput, bool track);

// The game's pad settings as it's given them, in the form of [ffxi] gamepad.mode and gamepad.buttons (input.cpp: vanacore.ini's
// at start, apply_gamepad's while it runs; empty keeps what it had, a normal install's at first): where its Change View is.
void game_settings(const std::string& mode, const std::string& buttons);

// ---- ImGui -----------------------------------------------------------------------------------------------------------
// Each read of XInput controller 0, and of a DirectInput controller, is also ImGui's (gui::gamepad_key): its gamepad keys,
// so an addon window can be driven with the pad. Confirm and Cancel are the game's own (padsin000's seventh and fifth
// slots): ImGui's activate and cancel. While an addon has the pad (vana.gamepad.nav, gui::wants_gamepad), the game reads
// it at rest: XInput's state zeroed after the handlers, a DirectInput controller's buttons and hats cleared (its sticks
// still move the character: their range is the game's to know).

// The game's Confirm, Cancel, main menu and chat window / options buttons: XInput's control numbers (12 A, 13 B, 14 X,
// 15 Y, 0-3 the D-pad, 4 Start, 5 Back, 8 LB, 9 RB, 16 LT, 17 RT, 6 and 7 the sticks pressed) or DirectInput's button
// numbers; -1 one the game has none for
struct Bindings {
    bool xinput = true;
    int confirm = -1, cancel = -1, menu = -1, options = -1;
};
Bindings bindings();
// Seconds since a read had something pressed or moved; a day when there was never one
double idle_seconds();
// A read within the last two seconds
bool present();

} // namespace vanacore::gamepad
