#pragma once

// [client] party.tpbar: in the party list, a member on a job with no spells (a Warrior, Ranger, Samurai with a sub job
// that has none either) gets a TP bar, blue, where the MP bar would be, filling to 3000 TP. The numbers stay as they
// were. A member whose MP isn't 0 keeps the MP bar.
// [client] party.tp: every member gets a TP bar of its own, the MP bar's look in blue, under the name and left of the
// MP bar, filling to 3000 TP and brighter from 1000, with the game's TP number at its end whatever Config says. On,
// party.tpbar does nothing (the MP bar stays the MP's).
namespace vanacore::tpbar {

// `ini` (vanacore.ini): [client] party.tpbar and party.tp read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: patches the game's MP bar call the first time it's wanted (once FFXiMain.dll is there).
void on_frame();

} // namespace vanacore::tpbar
