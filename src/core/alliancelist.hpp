#pragma once

// [client] alliance.macrobar: the alliance lists (the member lists of parties 2 and 3) stay on screen while the macro
// bar is up (Ctrl or Alt held). The game takes them down the moment the bar comes up and puts them back when it goes,
// from the days when the bar covered them; the Equipment window, a cutscene and the game's own menus still take them
// down as they did.
namespace vanacore::alliancelist {

// `ini` (vanacore.ini): [client] alliance.macrobar read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// [client] party.hide, alliance1.hide, alliance2.hide: true for a window of the menu manager (a ctrl) that is the party
// list (party.hide) or alliance list 1 or 2 (the member lists of parties 2 and 3) with its key on, whose draw is then
// left out. Called for every window drawn, on the game's thread. The party list is drawn all the same while its owner
// shows a gauge of its own in place of the members (fishing: the hooked fish's HP, shown nowhere else).
// [client] party.background, alliance1.background, alliance2.background (on unless 0) are seen to here too: off, that
// list is drawn without its frame and title, its rows (names, bars, numbers) as they are.
bool skip(const void* ctrl);

// True for the party list's window while its owner shows that gauge: an addon's hide of it (vana.gameui.hide
// ('party_list')) must leave it drawn too.
bool gauge(const void* ctrl);

// Once a frame on the game's thread: patches the game's check the first time it's wanted (once FFXiMain.dll is there).
void on_frame();

} // namespace vanacore::alliancelist
