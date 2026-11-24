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

// The game's menu manager and sprite sheets and its calls that put a layout or a sheet on their lists and take one off, as
// found for the Config list: for gamesettings.cpp, which changes another of the game's menus the same way. nullptr before
// FFXiMain.dll is loaded, or when they aren't as VanaCore knows them (logged once). The game's thread.
struct Menus {
    using Payload = void(__thiscall*)(void* list, const void** payload); // a pointer to a pointer to a payload (or a name)
    char* manager;                                                        // its windows at +0, its layouts at +0x1C
    char* sheets;
    Payload layout_add;    // parses a layout's payload and puts it on the manager's list
    Payload layout_remove; // takes the first layout of a name off and frees it
    Payload sheet_add;
    Payload sheet_remove;
    char*(__thiscall* window_part)(void* window, int item); // an item's part (+0x4A has a help line, +0x4C its text)
    void(__thiscall* window_help)(void* window);           // the help line shown again for the item the cursor is on
};
const Menus* menus();

} // namespace vanacore::configmenu
