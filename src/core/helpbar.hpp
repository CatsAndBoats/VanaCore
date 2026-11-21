#pragma once

// [client] helpbar.*: the help bar across the top of the screen while the main menu is open (the long bar with the
// description of the command under the cursor, the small title box at its left with the page's name, and the "Help"
// caption at its right end), each a toggle:
//   helpbar.hide=1        the help line gone, frame, text and caption (default 0); the title box's own is helptitle.hide
//                        (hudwin.cpp, with helptitle.move and .background)
//   helpbar.background=0  the bar's frame gone; its text and caption stay
//   helpbar.title=0       the title box gone (its frame and the page's name): the key that hid it before helptitle.hide,
//                        still read
//   helpbar.text=0        the description gone
//   helpbar.label=0       the "Help" caption gone
// and network.panel=0 (netstat.cpp) the Network window, frame and numbers. All 1 by default: the game's own look.
namespace vanacore::helpbar {

// `ini` (vanacore.ini): [client] helpbar.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: patches the game's help bar drawing the first time a toggle is off (once
// FFXiMain.dll is there).
void on_frame();

// From chat.cpp's hook of the game's window draw, before the window is drawn: true if `ctrl` is one of these windows and
// its drawing is to be left out.
bool skip(void* ctrl);

// After the game has drawn `ctrl` (chat.cpp's raise of the game's windows over an addon's): it's the title box or the help
// line with its frame faded out by the game, which is how they go while no menu is up (after 180 idle frames, FUN_1015e730;
// the title box's is switched off at once, FUN_10136af0). They stay in the menu manager at their full size all the while
// and draw nothing, so there's nothing to lay over an addon's window, the HUD Manager's panel or the box it outlines. Not the
// Network window: its numbers are drawn with its frame gone.
bool idle(const void* ctrl);

} // namespace vanacore::helpbar
