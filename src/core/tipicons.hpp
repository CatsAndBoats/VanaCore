#pragma once

// [client] tips.icon.x, .y, .size and tips.badges.x, .y, .size: an item's details' picture (the item's icon, with a stack's
// count over it) and its badges (Rare, Ex and the rest, by the name) moved and made larger or smaller where the game draws
// them, whatever draws the letters (the game's or True Font's): the details' draw's own calls of them redirected. The badges
// also move right as far as the box was made wider (itemtips: True Font's larger parts).
//
// [client] tips.type: the type row under the name as the game writes it (game, the default: "[Body]All Races", "(Sword)All
// Races"), simple ("Body - All Races") or the type alone (type: "Body"), from the same draw's calls of the game's getters of
// those texts; its prints told to True Font, which knows the row by them (vanacore_gfx_tip_type).
namespace vanacore::tipicons {

// `ini` (the settings file): the settings read now, and again by reload whenever it changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: the details' draw found and its calls redirected, the first time FFXiMain.dll is
// unpacked.
void on_frame();

} // namespace vanacore::tipicons
