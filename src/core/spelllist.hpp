#pragma once

// [client] spells.autosort: the Magic lists (the main menu's Magic and the action menu's) put in A-Z order each time one
// opens. The game keeps its order of the character's spells (the one its Magic > Rearrange changes, saved with the
// character's settings) and lists each category (White Magic, Black Magic...) in it; this sorts that order by the
// spells' names as the game shows them, just after the game adds newly learned spells to it.
namespace vanacore::spelllist {

// `ini` (vanacore.ini): [client] spells.autosort read now, and again by reload whenever vanacore.ini changes. A change
// applies to the next list opened.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's making of the Magic list the first time spells.autosort is on
// (once FFXiMain.dll is unpacked).
void on_frame();

} // namespace vanacore::spelllist
