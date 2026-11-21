#pragma once

// [client] menu.hovermove (on unless 0): the mouse picks a menu row only while it moves. The game's mouse frame
// (FUN_10124EA0) tests the pointer against the current menu's rows every frame it is shown, and picks the row under it
// (SetCursol) whenever that differs from the row it found the frame before. The frame before is forgotten (-1) each frame
// the test is skipped, as one with the menu opening is, and a picked row is nudged, which moves its edges under a pointer
// resting near one: so a pointer left on a menu pulls the cursor back to its row after a key or the pad has moved it
// (the "tick" of a menu that won't move: moving the mouse off it frees it), and a menu opens on the row under the pointer
// in place of the one the game remembers (the main menu at Items, not Status). With this on, a pick the pointer didn't
// cause is dropped: the pointer must have moved since the last frame the test ran (or since the last frame at all, when
// the test didn't run), or a mouse button be down (a click must find the row under it). The same goes for a window the
// pointer rests over: the Items list (and the storage and Equipment lists) hand the keys to their sort box, the storage
// window... when the pointer is over it, every frame (the list's key 8 / 4), so a box that comes up under a resting pointer
// took the keys from the list the moment it opened (reported 2026-10-28: the inventory opened on its sort box; moving
// onto it by hand is the game's own way to get there, and still works). The game's own jump of the pointer to a list's cursor
// row, as the list opens, can put it inside the box's frame before the box is moved, and counts as the pointer moving: a
// window under the pointer doesn't take the keys for half a second after the game last moved them. The macro bar's Button
// Edit box (Macros > Edit: Copy, Paste, Delete) takes them by its + key or a click on it only, never by the pointer being
// over it: the pointer follows the cursor's key, and a box moved over the bar took the keys from the key under it, and back
// after every Esc (reported 2026-11-14). Nothing else of the game's mouse changes.
namespace vanacore::menuhover {

// [client] menu.nomouse (off unless 1): the mouse picks no menu row and gives no window the keys by being over it, whether
// it moves or not (for a game played with the keys or a pad, where a pointer left anywhere takes the cursor or the keys). A
// click still acts on the row under it; the game's own mouse code is left alone otherwise.
//
// `ini` (the session's vanacore.ini): [client] menu.hovermove, menu.nomouse. Read now, and again by reload whenever
// vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Each frame, from Present, on the game's thread: the game's two calls hooked the first time they're wanted (once
// FFXiMain.dll is there), and the pointer's place taken up when the menu test didn't run, so a pointer that moved while no
// menu was up isn't a move to the next one.
void on_frame();

// The pointer's place (the game's mouse object's +4, +8: the units of the windows' frames) as last handed to the game's
// test, and whether it moved since the frame before; false until a menu has been up
bool pointer(int& x, int& y, bool& moved);

} // namespace vanacore::menuhover
