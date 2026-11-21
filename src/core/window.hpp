#pragma once

#include <windows.h>

#include <string>
#include <vector>

// The game window's keys that VanaCore handles itself: Ctrl+V pastes into the chat line, Ctrl+C copies it.
namespace vanacore::window {

// [window] dpiaware (default 1): tells Windows the game process draws at the display's own pixels,
// per monitor, so on a scaled display (125%, 150%) Windows doesn't draw the game smaller and stretch
// it (blurred): a window is its size in real pixels. From VanaCoreInstall, before the game makes a
// window; the process's setting can't change after.
void dpi_aware(const wchar_t* ini);

// The display's scale where the game window is (1.5 at 150%) when the game draws at its own pixels
// ([window] dpiaware), else 1: what Windows gave a Windower addon's text for free when it stretched the
// game's window (a 10 pt text stays the size it was drawn at on a scaled display).
float dpi_scale();

// Makes the game's frame and title bar sizes read as its window's own: zero while it has none, the measured ones while it
// has them (FFXiMain.dll scales the mouse by them: with VanaCore's borderless window the menus' hit areas, the title
// screen's buttons and the character list, came out low by up to 33 px, and a few px off with its windowed one). From
// VanaCoreInstall, before the game makes its window.
void hook_metrics();

// Takes over `window`'s messages (subclassing it) and starts the thread of the low-level mouse hook over VanaCore's
// windows. Call on the game's thread (the device's), once it has its window.
void install(HWND window);

// The game window, once install() has it (nullptr before).
HWND handle();

// The game window has been the foreground window (it has been in front since install()).
bool was_in_front();

// The game is in the background: the window in front is another program's (or none, mid Alt+Tab). One of the game's
// own process's windows in front (an addon's file dialog, a message box) is the game's. False before install(). The
// one test for what the game does in the background: controllers ([input], the game's "works while inactive" switch),
// [client] fps.background, Lua's system.focused. Any thread.
bool in_background();

// Brings the game window of process `pid` (another game on this PC) in front, from this game while it's in front (Windows
// gives the foreground away only from the process that has it). True if it's in front now. The game's thread.
bool front_process(unsigned long pid);

// The window's settings in `ini`: [ffxi] mode (FFXI's four: full screen, windowed, borderless
// windowed, borderless full screen; unset, the game's own window mode setting, 0034) and
// resolution.game (its client size in the windowed modes), and [window] resizable (whether its edges
// can be dragged), mouse.scale (whether mouse positions are scaled to the game's resolution when
// the window isn't that size) and keepawake (default 1: the screen saver, the display's sleep and the
// lock that follows them are held off while the game runs) and close.shutdown (default 1: closing the window
// with a character in the game runs /shutdown; closing it again within a minute closes the game; with the server silent
// for 20 s, a second close with no character in the game, or three closes in a row, the game is ended at once). Applied now and again whenever it's called (vanacore.ini changed); on
// the game's thread. `exclusive`: the game's device is full screen on Windows' own Direct3D 8, which
// owns the window: the window is left alone. Full screen otherwise is borderless over the monitor.
void apply(const wchar_t* ini, bool exclusive);

// A window mode by FFXI's number (0 full screen, 1 windowed, 2 borderless windowed, 3 borderless full
// screen): "0".."3", or fullscreen, windowed, borderless (windowed), borderless fullscreen (any case,
// spaces or dashes). -1 for anything else.
int parse_mode(const wchar_t* text);

// Where the window is ([window] position, as Ashita's move addon moves it, kept):
//   position=x,y     its top-left there, in the screen's pixels (left of or above the main monitor is
//                    negative); full screen covers the monitor that point is on
//   position=center  the middle of its monitor
//   position=last    where it was left, kept in [window] last as it's moved and when the game closes
//   unset            where the game puts it
// position.<character>, with any of those, is where that character's window goes when it logs in
// (several characters on one PC, each window in its own place). Read when the game starts and at
// each login; one changed while the game runs (the /vana window's Position) moves the window at
// once. [window] topmost=1 keeps it over other windows. In apply(), as the others.

// Each half second from the Present hook (the game's thread): a character logging in that has a
// position of its own goes there; the window on another monitor takes [ffxi] resolution.game and
// resolution.ui with it (auto worked out for that monitor, a size that was the old monitor's own
// becomes the new one's).
void poll();

// The window to monitor `number` (1 the leftmost, as monitors() has them): the middle of it, or
// covering it in full screen; kept with position=last. False when it can't move (Windows' own
// exclusive full screen) or there's no such monitor. The /vana window's Monitor, /vana window monitor.
bool to_monitor(int number);

// vana.window.block_mouse(on, ms): while on (the Import and Export windows are open), no mouse button reaches the game: not
// from its window's messages, not from the low-level hook's presses. On is a lease of `ms` (the window asks again every
// frame it's drawn). Off: still blocked for half a second and until every button is up, then the game is told every button
// is up. Thread safe.
void block_mouse(bool on, unsigned lease_ms = 400);
bool mouse_blocked();

// A press the game already has (a drag the map takes over): the game's mouse forgets its buttons, sees none held in its moves
// and gets no release. Game thread.
void release_game_buttons();

// "/vana window <what> ..." (its words from "window" on): pos (move) <x> <y>, center, monitor <n>,
// size <w> <h>, front, top [on|off], frame, max, min, restore, save [char], forget [char], info,
// help. True: handled ("/vana window" alone is the /vana window's Client tab).
bool command(const std::vector<std::string>& parts);

// A monitor, as Windows lays them out: left to right, then top to bottom (1 is the leftmost)
struct Monitor {
    RECT area{};    // the whole monitor
    RECT work{};    // less the taskbar
    bool main = false;
};
std::vector<Monitor> monitors();

// The game window's place, for vana.window.info. False before install().
struct Place {
    RECT frame{};          // the window, on the screen
    int client_width = 0, client_height = 0;
    int monitor = 0;       // 1-based, in monitors()' order
    bool topmost = false, minimized = false, maximized = false, covering = false;
    std::string character; // the logged in character position.<character> is for ("" before login)
};
bool place(Place& out);

// The client size the window was made at in a windowed mode ([ffxi] resolution.game, /vana window size; the game's
// resolution while it's as the game made it), whatever Windows has since left of it: a window with a title bar is kept
// no taller than the screen, so one the screen's height has a shorter client area, and a monitor scaled otherwise gives
// it another frame. The interface is fitted to this shape, not the client area's. False while the window covers its
// monitor, can have its edges dragged, or is minimized: its client area is its size then.
bool made_size(unsigned& width, unsigned& height);

// The window's client area while it's a size of its own: [window] resizable, its edges dragged to a size kept for the next
// start ([window] lastsize, the /vana window's Resolution "(resized)"), in a windowed mode. False while it isn't, covers
// its monitor or is minimized. [ffxi] resolution.ui unset ("Same as Resolution") lays the menus out at this size.
bool resized_client(unsigned& width, unsigned& height);

} // namespace vanacore::window
