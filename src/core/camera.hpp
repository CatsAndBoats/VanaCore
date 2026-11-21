#pragma once

#include <windows.h>

#include <string>

// [camera] zoom=distance: the mouse wheel and the game's zoom keys move the camera nearer and farther,
// where the game's own zoom narrows and widens the field of view. [camera] fov sets the field of view.
// [camera] offset.x, offset.y move the camera aside, your character that far from the screen's middle; [camera] lock takes
// the camera along with your character at once. [camera] view.reset: the controller's Change View button turns the camera
// round behind your character instead of switching to first person. [camera] zoom.speed, distance.remember, mouse.edgeturn (the
// cursor at the window's edge turning the camera), mouse.noleftdrag (a left-button drag not moving the camera), behind (the camera swinging round behind your character as it moves) and
// freelook (the camera turning without turning your character as it walks). /freecam detaches the camera from your character
// altogether: it flies where you send it while your character stands still.

namespace vanacore::camera {

// `ini` (vanacore.ini): [camera] is read from it, and again whenever it changes. Every game's zoom starts
// at [camera] distance (6); the zoom itself isn't saved.
void install(const wchar_t* ini);

// Once a frame on the game's thread: finds the game's camera the first time zoom=distance wants it (once
// FFXiMain.dll is there) and puts ours in or the game's back as the setting says; the same for the offset and the lock.
// Those, walls, behind and view.reset only while a character is in the world: the title screen, the character select
// and a zone's loading have the game's own camera.
void on_frame();

// A [camera] setting (fov, turn, turn.vertical, battle.distance, battle.range, distance, distance.max, zoom.speed, behind.speed,
// behind.delay, offset.x, offset.y) as the /vana
// window's slider is dragged:
// applied at once, without vanacore.ini, which the window writes when the slider is let go. Game thread.
void preview(const std::string& key, float value);

// view.reset: the controller's Change View button (the game's first-person view; gamepad.cpp finds it in the game's button
// map) as the game would read it from `source` (0-3 an XInput controller, 4 DirectInput's). True: hide it from the game, as
// the camera turns round behind your character for it. A press that began in first person goes through, so Change View
// still comes back out of it; a press is hidden or not until it's let go. Any thread.
bool view_button(int source, bool down);

// /camtrace: the last 8 seconds of what the camera's walls and lock saw and did, and the next 4, to vanacore.log (it also does by
// itself when the walls' pull jumps, the wall ray hits beside the point the camera looks at, or lock moves the camera far in a
// frame, at most every 10 seconds). Game thread.
void dump_trace();

// /freelook: the camera turns without turning your character. On, the character walks (and turns on the spot) the way the
// keys say from the camera as it was when free look came on (again after a zone), not from where it's turned now. `want`: 1
// on, 0 off, -1 the other way, written to [camera] freelook (the /vana window's Free look). Returns 1 on, 0 off, -1 not found
// in the game (it stays off). Game thread.
int free_look(int want);

// /freecam: the camera detached from the character, flown with W A S D (E or Space up, Q or C down, Shift fast, the wheel the
// speed) and turned with the mouse (the right button held) or the arrow keys. The character stands still: those keys, the numpad's
// arrows, the right button, the mouse's movement while it's held and the wheel don't reach the game. Off again, the game's camera
// is where it was. Off by itself with a zone. `want`: 1 on, 0 off, -1 the other way. Returns 1 on, 0 off, -1 the game's camera
// view wasn't found (it stays off), -2 no character in the world. Game thread.
int free_cam(int want);

// /freecam float: the free camera glides to a stop and turns with some lag, so it feels floaty ([camera] freecam.float, written
// to the settings file; freecam.lag is the seconds of lag, the /vana window's slider). `want`: 1 on, 0 off, -1 the other way.
// Returns 1 on, 0 off. Game thread.
int free_cam_float(int want);

// The free camera is on. Any thread.
bool fly_on();

// The free camera as an addon sees it (vana.camera.state()): `on` and `have` (on, and its pose is taken from the game's view: the
// pose can be read and set), `hold` (an addon sets the pose, see fly_hold()), the float setting, its lag in seconds, the speed.
struct FlyState {
    bool on = false, have = false, hold = false, floaty = false;
    float lag = 0.0f, speed = 0.0f;
};
FlyState fly_state();

// The free camera's pose, for the window and the sequencer: out = x, y, z (the eye, the game's world) and yaw, tilt (radians). Yaw is
// the way it looks about the world's up, from the world's x axis toward its z; tilt is up from level (never past 89 degrees). False
// when it isn't on or the pose isn't taken yet. Game thread.
bool fly_pose(float* out);

// The same five numbers set as the pose; the camera's speed and what it was gliding on are dropped. False when it isn't on, the pose
// isn't taken yet, or a number is not finite. Game thread.
bool fly_set_pose(const float* in);

// While on, the keys, the mouse and the wheel move nothing (the camera's own turn and glide stop): a sequencer plays the camera
// through fly_set_pose() without them pulling it aside. Off with the free camera. Game thread.
void fly_hold(bool on);

// The free camera's speed in units a second (the wheel's, before Shift): set when `want` is above 0 (kept within its limits),
// returned either way. Game thread.
float fly_speed(float want);

// While it's on, from the game's DirectInput keyboard read (input.cpp): true if the key (a DirectInput key code, `modifiers` as
// binds count them) is one the free camera flies with, so the game's keyboard reads it up. Not with Ctrl or Alt, or while the
// chat line or an ImGui text box has the keyboard.
bool takes_key(unsigned dik, unsigned modifiers);

// ... and one of those that's a letter (W A S D, Q E C), asked as its press comes out of the game's message queue (input.cpp):
// with the game's keyboard set to Full a letter opens the chat line and types itself there, from the message, whatever the
// keyboard read says. Not Space (it opens the chat line in either layout, as it did), nor the arrows and the numpad, which type
// nothing. The chat line opened another way (/, Space, Enter), the letters are the game's again (takes_key).
bool takes_letter(unsigned dik, unsigned modifiers);

// While it's on, from the game window's procedure (the game reads its mouse from the window's messages; it makes no DirectInput
// mouse): a mouse message that's the free camera's, so the game doesn't see it. The wheel (the camera's speed), the right button
// (a press and its release, if the press was the camera's) and the mouse's movement while it's held, which turns the camera: the
// cursor is hidden and held where the button went down meanwhile, and shows there again when it's let go. Game thread.
bool fly_mouse(UINT message, WPARAM wparam);

// The right button is held for the free camera's turn: the window's procedure gives fly_mouse() the mouse before VanaCore's own
// windows then (one under the cursor, where it's held, took its movement, and the camera jumped by all of it
// once the cursor came out from under it)
bool fly_dragging();

// The game's collision ray from `from` to `to` (the game's draw coordinates): 1 where nothing of the zone is between them, 0 where
// something is, -1 where that isn't known (the ray or the zone's collision isn't there). Game thread.
int sight(const float* from, const float* to);

// The same ray, for where it meets the zone: 1 and `hit` (8 floats, the point first in the game's draw coordinates) where it does,
// 0 where it meets nothing between `from` and `to`, -1 where that isn't known. Game thread.
int cast(const float* from, const float* to, float* hit);

} // namespace vanacore::camera
