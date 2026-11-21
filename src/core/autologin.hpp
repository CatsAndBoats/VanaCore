#pragma once

// [client] autologin: the title screen goes by on its own. The terms are accepted, Select Character is picked, then the
// character in [client] autologin.character (default 1, the first in the list) and Yes to playing it, each a moment
// after its window opens, as the game's own key handlers would for a press of Enter. Once per game start: a logout back
// to the title screen is left alone, as is a game that's been in the world. Holding Shift at the title screen stops it
// for that start.
namespace vanacore::autologin {

// `ini` (vanacore.ini): [client] autologin and autologin.character read now, and again by reload whenever it changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: finds the lobby's windows the first time it's wanted (once FFXiMain.dll is
// unpacked), then answers whichever one is open.
void on_frame();

} // namespace vanacore::autologin
