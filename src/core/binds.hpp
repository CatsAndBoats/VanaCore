#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

// Key binds: Ashita's /bind and Windower's bind (//bind), a key with modifiers running a command. A bound key runs its
// command and the game doesn't see it; while the chat line is open, binds wait (keyboard.blockbindsduringinput, on as
// in Ashita) unless made for it (Windower's $).
namespace vanacore::binds {

// Modifier bits, the numpad's Enter as a key (Enter, extended), and the mouse wheel's turns and tilts as keys (no
// virtual-key code is one)
inline constexpr unsigned kCtrl = 1, kAlt = 2, kShift = 4, kWin = 8, kApps = 16;
inline constexpr unsigned kNumpadEnter = 0x100 | VK_RETURN;
inline constexpr unsigned kWheelUp = 0x201, kWheelDown = 0x202, kWheelLeft = 0x203, kWheelRight = 0x204;

// [input] keyboard.blockbindsduringinput (1 by default) and keyboard.silentbinds (0: /bind and /unbind say what they
// did in the chat log).
void install(const wchar_t* ini);

// keyboard.blockbindsduringinput and keyboard.silentbinds read again (vanacore.ini changed: /vana's switch), for the
// next key and the next /bind
void reload(const wchar_t* ini);

// True while binds wait: keyboard.blockbindsduringinput on and the chat line open. Ashita and Windower addons' and
// plugins' key presses (the window messages) wait then too (addons.cpp, plugins.cpp): XIUI's hotbar keys (1-0 with no
// modifier) are binds to the player, and fired as digits were typed.
bool typing_blocks();

// [input] keyboard.blockbindsduringinput: whether key binds are withheld while the chat line is open. Ashita exposes
// this to plugins and addons as IKeyboard::Get/SetBlockBindsDuringInput; a Set is runtime only (the /vana window and
// the ini are the lasting setting, and a file change through reload() wins).
bool block_during_input();
void set_block_during_input(bool block);

// /bind, //bind, /unbind, //unbind, /unbindall: true if `line` is one of them (done).
//   /bind [up|down] <key> <command>   the key: modifiers (^ Ctrl, ! Alt, @ Win, # Apps, + or ~ Shift; % only while
//                                     the chat line is closed, $ only while it's open), then its name (f1, a, 1,
//                                     numpad1, up, insert, ...; a mouse button: lbutton, rbutton, mbutton, xbutton1
//                                     or mouseback, xbutton2 or mouseforward, mouse1-5; the wheel: wheelup, wheeldown,
//                                     wheelleft, wheelright). The command as typed: /ma "Cure" <t>, /addon load x,
//                                     or Windower's console form (gs c toggle, input /ma ...).
//   /bind                             lists the binds
//   /unbind <key> | all               takes binds off
bool command(std::string_view line);

// A key message from the game's window (WM_KEYDOWN, WM_SYSKEYDOWN, WM_KEYUP, WM_SYSKEYUP): true if a bind took it (its
// command run on a press, or on a release for an up bind; the key kept from the game either way).
//   on_queued_key  as the game's loop takes it from the queue (PeekMessageA), before the loop acts on it: the loop opens
//                  the chat line on a / press before the message is dispatched, so a bind on / is answered here
//   on_key         as the window procedure gets it: a message on_queued_key let through isn't asked about again
bool on_queued_key(UINT message, WPARAM wparam, LPARAM lparam);
bool on_key(UINT message, WPARAM wparam, LPARAM lparam);

// A mouse button's message from the game's window (a press, release or double click, as the window procedure gets it,
// after VanaCore's windows, plugins and addons passed on it): true if a bind took it (its command run as on_key's). The
// window procedure keeps it from the game, and a press from the game's DirectInput mouse too.
bool on_mouse(UINT message, WPARAM wparam);

// The game's DirectInput keyboard (input.cpp), a read of it showing `dik` (a DirectInput key code) pressed with
// `modifiers` held: true if a bind takes the key now (one on it, for its press or its release, with the chat line as it
// is) or a capture does, so the read keeps it from the game. The game reads its keyboard once a frame, and a read between
// the key going down and its window message reaching on_queued_key saw the key: hidden from the message alone, it was
// hidden too late then (Alt+P bound to /follow opened the party chat now and then). Runs no command: the message does.
bool takes_key(unsigned dik, unsigned modifiers);

// A key message's key as binds name it, from its virtual key, scan code and extended bit (a WM_KEYDOWN's wparam and
// lparam, a low-level hook's vkCode, scanCode and flags): the numpad's Enter is kNumpadEnter, and the numpad's other keys
// are numpad keys whatever Num Lock says. With Num Lock off, or Shift held with it on, Windows gives them the keys Insert,
// End, Down... are (Clear for 5), told from those only by the extended bit. Ashita's binds go by DirectInput's codes, where
// the two never meet; VanaCore's took the virtual key, so /bind insert (Ashita's stock /bind insert /ashita, which the
// launcher copies in) took the numpad's 0 from the game: its target cycling, Num0 with Num Lock off and Shift+Num0 with
// it on, worked only now and then (reported 2026-11-06).
unsigned message_key(unsigned vk, unsigned scan, bool extended);

// A key pressed with a Windows key held (Win+F1, @f1), from the low-level keyboard hook's own thread: Windows takes it
// for itself (Win+F1 is its help, Win+G the Game Bar) before the game's window sees it. True if a bind takes it (its
// command queued for the game's thread, which runs it on kRunPending): the hook keeps the key from Windows and the game,
// and anything unbound goes on to Windows as ever. `key` as on_key's, `modifiers` the bits above (kWin included), `repeat`
// a held key's later presses.
bool take_win_key(unsigned key, bool down, bool repeat, unsigned modifiers);

// A turn or tilt of the mouse wheel (WM_MOUSEWHEEL, WM_MOUSEHWHEEL) over the game in front and not over VanaCore's
// windows, from the low-level mouse hook's thread (`data` its mouseData): true if a bind takes it (its command queued for
// the game's thread, as take_win_key's), so the hook keeps it from the game. Taken there, not from the window's message,
// which can come after the game read the turn through its DirectInput mouse (and zoomed the camera). A bind runs once a
// notch, as a press and its release (a bind on either runs); a high-resolution wheel's smaller turns add up to a notch.
bool take_wheel(UINT message, DWORD data);

// The message take_win_key and take_wheel post to the game's window; its handler (the game's thread) calls run_pending().
inline constexpr UINT kRunPending = WM_APP + 0x3A8;
void run_pending();

// A key's name as /bind takes it, without modifiers (f11, a, numpad1, scrolllock, ...): its virtual-key code
// (kNumpadEnter for numpadenter), 0 if it isn't one VanaCore knows or is a mouse button.
unsigned key_code(std::string_view name);

// Ashita plugins' binds (IKeyboard::Bind and the rest): `key` a DirectInput key code (DIK_*), its modifiers as the bits
// above, on its release if `up`.
void add(unsigned dik, unsigned modifiers, bool up, const std::string& command);
bool remove(unsigned dik, unsigned modifiers, bool up);
void clear();
bool bound(unsigned dik, unsigned modifiers, bool up);

// The keybinds window's view of the binds (vana.binds in Lua), in the order they were made.
struct Entry {
    std::string key;     // the key's name without modifiers, as /bind takes it (F6, a, numpad1, up)
    unsigned vk = 0;     // its virtual-key code (kNumpadEnter for the numpad's Enter)
    unsigned modifiers = 0;
    bool up = false;     // on the key's release
    char when = 'a';     // 'a' always, 'c' only while the chat line is closed (%), 'o' only while it's open ($)
    std::string spec;    // modifiers, then % or $, then the key, as /bind takes it (^f6)
    std::string command;
    double fired_ago = -1; // seconds since it last ran, -1 if it hasn't
};
std::vector<Entry> entries();

// The binds there are now, kept as the binds boot.txt and the addons made: scripts\keybinds.txt is about to replay (a later
// call, or one after the window's own, changes nothing). The window's cue for which keys a removal has to /unbind in the file.
void note_base();

// The binds note_base kept. False, `out` empty, before it ran.
bool base_entries(std::vector<Entry>& out);

// /bind without the chat log: `spec` as /bind takes the key (^!f1, $return). False, with why in `error`, for a key
// VanaCore doesn't know or a command that's empty.
bool set(const std::string& spec, bool up, const std::string& command, std::string& error);

// /unbind without the chat log: by a bind's Entry, or by `spec` (when's part of it). True if one was bound.
bool unset(unsigned vk, unsigned modifiers, bool up, char when);
bool unset(const std::string& spec, bool up);

// Key capture, for a "press a key" button: between capture_begin and capture_end the next key pressed (not a modifier
// alone) goes to the capture and to no one else. Escape alone cancels. `modifiers` follows the keys held until it's done.
// A mouse button or the wheel counts too (capture_mouse): left, right and the wheel's turns only with a modifier held.
struct CaptureState {
    bool active = false;
    bool done = false;
    bool cancelled = false;
    unsigned key = 0; // a virtual-key code once done (key_name_of turns it into a name)
    unsigned modifiers = 0;
};
void capture_begin();
void capture_end();
CaptureState capture_state();

// A mouse button pressed, or the wheel turned or tilted, over the game, from the low-level mouse hook's thread (`data` its
// mouseData): true if it's the capture's answer, so the hook keeps it (and a button's release) from everyone, VanaCore's
// windows included. A plain left or right click, or a plain turn of the wheel, isn't: it stays a click (on the capture box
// again, or Clear) or scrolls.
bool capture_mouse(UINT message, DWORD data);

// A virtual-key code's name as /bind takes it
std::string key_name_of(unsigned key);

// The lines of a saved binds file (scripts\keybinds.txt: /bind and /unbind, # for comments) run without chat output; how many
// ran. `//keybinds apply` is this on the file, queued once boot.txt has run.
int apply_file(const std::string& text);

// Every key /bind takes, once each (letters, digits, F1-F24, numpad digits, then the named keys), for a key picker
std::vector<std::string> key_list();

} // namespace vanacore::binds
