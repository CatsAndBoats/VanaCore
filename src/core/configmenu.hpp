#pragma once

// [client] configmenu: VanaCore in the game's own Config menu. The main menu's Config list gets an item after its
// thirteen, "VanaCore", drawn and run by the game's menu code as the others are (its window, cursor, sounds and help
// line); choosing it opens the /vana window, as typing /vana does. None of the game's code is changed: the list's layout
// is rebuilt with the item and registered in place of the game's, the label is a sprite of VanaCore's own made of the
// game's button and menu font, and the list's handler gets a copy of its table of functions whose drawing and input come
// here first. The technique is XI on Anything's Config > Advanced (rubymatrix/xi-on-anything, host/modern.c), on the same
// FFXiMain build. Built so more of VanaCore's own items can join the one there is.
namespace vanacore::configmenu {

// `ini` (the session's vanacore.ini): [client] configmenu, 1 (the default) for the item, 0 for the game's list as it is.
// Read now, and again by reload whenever vanacore.ini changes; on_frame applies it.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Each frame, from Present, on the game's thread: twice a second, the item put into the game's Config list once its menus
// are loaded (and again after it loads them again), or taken out when the setting is off. Either waits while the list is
// open.
void on_frame();

} // namespace vanacore::configmenu
