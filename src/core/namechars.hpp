#pragma once

// [client] macronames.extended (/vana Game Options > Macro Bar > Enable extended macro book names): a macro book's and
// an equip set's name (Macros > a book or a set > Change Name) takes spaces and symbols, not only letters and digits,
// and their lists give a name the row's whole width.
//
// Why the game refuses them (read from the client and tried live, 2026-11-12): the two name boxes are the only text
// inputs made in mode 4 (0x101A4E6B the books', 0x101A5897 the equip sets'), and as one becomes the box typed into
// (0x101C5120) mode 4 asks the keyboard for character set 3, which lets a typed character through only if it's a letter
// or a digit (0x101B54B0: isalnum). Nothing past the box needs that: the name is copied as it is, 15 bytes, into the
// book's 16 of mcr.ttl / mcr_2.ttl (0x101A10E0), which is saved and loaded whole. The reason is the lists' font: the
// macro menus print names with the "menu    keytop" sheet's letters, which has only 0-9, a-z and A-Z and passes over
// anything else without leaving its room (0x101B7EC0), as they do a macro's own name, which may hold anything. With the
// menus' True Font on, chatfont.cpp prints the names as text and every character shows; with it off a space or a symbol
// is kept in the name but not drawn in the lists ("My Book!" reads "MyBook").
//
// On, mode 4 asks for character set 4 in its place, the game's own "any typed character from a space up" (single
// bytes; what its mode 5 and 6 boxes use), so the gamepad's on-screen keyboard offers its symbols too. A box already
// open keeps the set it opened with.
//
// The lists' room: a row prints its name until a letter would pass 75 pixels (0x101A4FDA the books', 0x101A55D4 the equip
// sets'), in a window 112 wide with the name 8 in. In the game's own letters 15 of them are 105 to 127 wide, so it
// never showed a long name whole ("BlackMageNukes1" stops after "BlackMageN"). On, the room is 96, the row's width.
//
// Not longer than 15 characters: a name has 16 bytes in the macro controller and in the file (20 names of 16 bytes
// each), and the game copies one into 16-byte buffers of its own (the name box as it opens, a book's Copy).
//
// Three bytes of the game's code, put back when the setting goes off.
namespace vanacore::namechars {

// `ini` (the session's vanacore.ini): [client] macronames.extended, 0 (the default, the game's) or 1. Read by the first
// on_frame, and again by reload whenever vanacore.ini changes; on_frame applies it.
void reload(const wchar_t* ini);

// Each frame, on the game's thread: the game's code changed once FFXiMain.dll is there, and again when the setting changes.
void on_frame(const wchar_t* ini);

} // namespace vanacore::namechars
