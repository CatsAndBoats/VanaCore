#pragma once

// [client] singlemenu: the main menu as one list. The game's main menu has two pages, "menu    menuwind" (Status ...
// Map) and "menu    socialme" (Missions ... Log Out), flipped with Left / Right or the menu key; with this on, page 2's
// twelve items sit under page 1's in one column and there's no flip. None of the game's code is changed (configmenu.cpp's
// way): menuwind's layout is rebuilt with socialme's items after its own and registered in place of the game's, its frame
// a sprite of VanaCore's own with a well for each of the 24, and menuwind's handler gets a copy of its table of functions
// whose input runs a page-2 item through socialme's own input (as the game does, sounds and checks included) and drops the
// page flip. socialme's layout and handler are left as they are, for when the setting goes off.
namespace vanacore::singlemenu {

// `ini` (the session's vanacore.ini): [client] singlemenu, 0 (the default) for the game's two pages, 1 for one list.
// Read now, and again by reload whenever vanacore.ini changes; on_frame applies it.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Each frame, from Present, on the game's thread: twice a second, the one list put in once the game's menus are loaded
// (and again after it loads them again), or the game's put back when the setting is off. Either waits while the main
// menu is open.
void on_frame();

} // namespace vanacore::singlemenu
