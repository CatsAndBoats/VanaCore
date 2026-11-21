#pragma once

// [client] macrooptions.equipfirst: an equip set's options box (Macros > Edit Equip. Set, a set chosen: "menu    mcresmn",
// Edit, Change Name, Copy, Paste, Delete, Use Current, Equip, Style Lock) with Equip at the top and the cursor starting on
// it. None of the game's code is changed: the box's layout, as the game loaded it, has its buttons' places and their up and
// down links laid in that order (each button keeps its id, so it does what it did), and as a box opens its cursor is put
// on Equip (SetCursol). Off, the game's order is put back.
namespace vanacore::equipfirst {

// `ini` (the session's vanacore.ini): [client] macrooptions.equipfirst, 0 (the default) for the game's order, 1 for Equip
// first. Read now, and again by reload whenever vanacore.ini changes; on_frame applies it.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Each frame, from Present, on the game's thread: a box just opened has its cursor put on Equip; twice a second, the
// layout laid in the order the setting wants while no box is open (and again after the game loads its menus again).
void on_frame();

} // namespace vanacore::equipfirst
