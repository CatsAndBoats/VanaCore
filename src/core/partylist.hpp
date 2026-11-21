#pragma once

#include <cstdint>

// The party list's and the alliance lists' names and numbers, as the game draws them (so the game's letters and True Font's
// alike): [client] party.names.upper / alliance.names.upper print the members' names in capitals (the party list's pet's
// too); party.hp.color, .mp.color, .tp.color (alliance.* the same) colour the HP, MP and TP numbers (#RRGGBB; unset the
// game's). HP's colour is its colour at 75% HP and over: under it the game's yellow, orange and red still show. TP's is its
// colour from 1000; under 1000 a quarter darker, as the game's grey is to its white. Their sizes are True Font's
// (party.font.numbers.size: chatfont.cpp).
namespace vanacore::partylist {

// `ini` (vanacore.ini): the keys read now, and again by reload whenever it changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: the game's row code found the first time it's wanted (once FFXiMain.dll is there),
// and patched as the settings say.
void on_frame();

// The party list's TP number's colour for `tp` (party.tp.color, its colour under 1000 a quarter darker), or `game` while
// that isn't set: tpbar.cpp's TP standing in a member's MP.
std::uint32_t tp_colour(int tp, std::uint32_t game);

} // namespace vanacore::partylist
