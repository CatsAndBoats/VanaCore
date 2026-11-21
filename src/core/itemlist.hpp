#pragma once

// [client] items.rows: how many rows the Items list (the main menu's Items, and the action menu's: what can be used,
// kept above the action menu's own window, and the auction house's Sell), the Mog House's lists (View House: a
// storage's list with the inventory's beside it) and a storage's list viewed from outside the house show at once, 10
// (the game's) to 50. [client] items.autosort: those lists put in the sorted order (the Items window's Sort option's) each
// time one opens (the inventory, the Mog Safe, the wardrobes...), as private servers send a bag back in slot order at
// every login. [client] items.sort=az: that sort (and the Sort option's) puts items in A-Z order by name. Their windows
// are made taller by the rows and the item details window under them is moved down as far. The other lists that use the same windows
// (trading, the bazaar, a storage opened from an NPC's menu...) keep the game's 10. [client] auction.rows: the same for
// the auction house's list of a category's items, 10 to 50. [client] shop.rows: the same for an NPC's shop (what it
// sells and what of yours it buys), its gil window moved down under it with the item details window. [client] equip.rows:
// the same for the Equipment menu's list of what fits a slot (the Items window, opened with the equipment's controller),
// 10 to 50, apart from items.rows. [client] shop.total:
// once a shop purchase's quantity is picked, what it will cost in the chat ("Purchase: 12 Toolbag (Ino) for 360,000 Gil? (6,114,942 G)").
// [client] lists.rows: how many rows the lists over the chat (Magic, Abilities, Key Items...) show at once, 12 (the
// game's) to 40, as many as fit; their windows grow upward from the chat. [client] currencies.rows: the same for the
// Currencies lists (Status > Currencies, Currencies 2), 8 (the game's) to 30, growing downward.
namespace vanacore::itemlist {

// `ini` (vanacore.ini): [client] items.rows, auction.rows, shop.rows, equip.rows and items.autosort read now, and again by reload whenever vanacore.ini
// changes. A change applies to the next list opened.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's setting up of its list windows the first time items.rows or
// auction.rows, shop.rows or equip.rows is over 10 or items.autosort is on (once FFXiMain.dll is unpacked).
void on_frame();

// The game's GetJumpTag (CTkMenuData's: where a key takes the cursor from a button, from the menu resource), as chat.cpp
// hooks it
using JumpTag = short(__fastcall*)(void* data, void* edx, int cursor, int key);

// For chat.cpp's hook of it: where the key `key` takes the cursor from row `cursor` of a list VanaCore gave rows past the
// game's ten (its window's menu data `data`), with `game` the game's own routine: true with `result` set, false for the
// game's own answer.
bool jump_tag(void* data, int cursor, int key, JumpTag game, short& result);

// The list window up now (the Items list's, a storage's, the auction house's or a shop's; the first found), or null:
// hudwin puts an item description by the list it opens under
char* open_list();

} // namespace vanacore::itemlist
