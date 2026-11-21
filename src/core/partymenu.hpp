#pragma once

// [client] partyfinder.findmember (off by default): the main menu's Party > Find Member opens the Party Finder addon
// (ashita/partyfinder) when it's loaded and running, in place of the game's search for characters seeking a party.
// Without the addon, or with the setting off, Find Member is the game's.
namespace vanacore::partymenu {

// `ini` (the session's vanacore.ini): [client] partyfinder.findmember, 1 or 0 (the default). Read now, and again by
// reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Each frame, from Present, on the game's thread: the Party menu's handler hooked, once FFXiMain.dll is there.
void on_frame();

} // namespace vanacore::partymenu
