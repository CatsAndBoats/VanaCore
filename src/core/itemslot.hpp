#pragma once

#include <cstdint>

// [client] items.slot: the grey bevelled square the game draws behind an item's icon (the item details window, the
// equipment slots, trade, bazaar and the other windows built from the menu resources' "menu    itemslot" picture), left
// as the game draws it (game), recoloured (tint: items.slot.color, .brightness, .contrast), filled with one colour
// (solid: items.slot.color) or not drawn (hide; the trade and delivery windows keep a white outline, so their empty slots
// can be found); items.slot.opacity fades it. The picture is one 32x32 DXT1 texture all
// those windows share, recoloured in place; the fade is each draw's own.
// [client] items.equip=1: the Equipment window's squares drawn instead with VanaCore's own picture, a background
// (items.equip.background, .background.opacity) inside a border (items.equip.border, .border.opacity, .border.size),
// whatever items.slot does to the rest. items.equip.wardrobe=0 leaves out the wardrobe number on its slots. The Check
// window's (/check) and the Equipment Set window's (an equip set's Equipment) squares go by items.equip too, and their
// slots' names aren't drawn under an item's icon.
namespace vanacore::itemslot {

// `ini` (vanacore.ini): [client] items.slot* and items.equip* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's drawing of a menu picture the first time items.slot isn't game
// or items.equip is on (once FFXiMain.dll is unpacked).
void on_frame();

// The game's draw of a window's control, bracketing it (chat.cpp): the squares drawn inside it know their window. With
// items.slot=hide the trade and delivery windows draw a white outline of each square, so empty slots can be found.
const void* enter_window(const void* ctrl);
void leave_window(const void* outer);

// [client] buttons.color and buttons.selected.color (empty, the default: the game's; the main menu's windows their
// mainmenu.buttons.* keys, buttons.cpp's bar_colour): every menu's button bars, and the selected one's, drawn in the colour
// picked (as they'd look on the frame), framed or not, solid or not.
// From hudwin.cpp's frameless draw of a window (<id>.background=0) with [client] <id>.solid=1 (off by default), on
// before it and off after: each bar of its buttons (a "menu    button*" picture, which the game adds onto what's under
// it, the frame) is drawn solid instead, blended over the scene from a copy of its picture, so the buttons are as plain
// to see without the frame as with it. The menu picture drawing is hooked (on_frame) after the first time it's on.
void frameless(bool on);

// An item's icon drawn at `x`, `y` (itemrarity.cpp's hook of the game's icon draw, which goes in while wants_icons says
// so): with items.equip on, the Check and Equipment Set windows' slot names under the icons aren't drawn the next frame
void icon_drawn(int x, int y);
bool wants_icons();

// The game's own drawing of one menu picture's quad (past the hook; nothing before it's hooked), for buttons.cpp's boxes
void draw_game(void* quad, int x, int y, float sx, float sy, std::uint32_t colour, int flags);

} // namespace vanacore::itemslot
