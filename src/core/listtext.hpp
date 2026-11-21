#pragma once

#include <cstdint>

// The item lists' names as VanaCore's own text: each name in the Items, Satchel, Mog storage and bank lists one whole text in
// True Font (the items' area: items.font), drawn in VanaCore's interface pass where the game prints it, as the macro bar's names
// are (macrotext.cpp).
// Before, the game's letters were kept as it drew them and True Font's laid in their place a word at a time: whatever the game
// drew over one after it (the cursor's box, Use / Drop opened over the list) gave that letter back to the game's font at the game's
// place, so under the cursor a name was in two fonts and a long one's last letter stood apart from it (reported 2026-11-13:
// "Hydro Baghnakh  s"; asked for: "make inventory true text boxes and not just texture replacements, since we should know the
// full proper text being passed in").
//   The name is taken at the game's coloured text draw (0x10129A40, itemrarity.cpp's hook: x, y, the text, its colour) from a list's
//   cell (the cell drawer's code), in a list window (told apart by its layout's name), with the colour the game or items.equipped
//   and items.rarity gave it; the game prints nothing for it.
//   The text is drawn over the game's picture, so a window the game drew after the list (Use / Drop, a Yes / No box, the quantity
//   box) is over it: the text is cut where such a window's rectangle starts, and where the list's own ends.
// A name past ASCII, a list window whose rectangle can't be read, or the items' True Font off: the game's own letters.
//
// The Equipment window's slot names the same way (reported 2026-11-13, "are we doing texture replacements? we should just be hiding
// the vanilla textures and writing out as pure text box our own, with custom padding from the top left of the square"): the game's
// letters left out (chatfont.cpp: vanacore_gfx_hide_letters, as equip.font=hide does), and each of its 16 squares' name ("Main",
// "Sub", ... "Ear 1", "Ring 2": always the same words) one whole text in the Equipment Slots' True Font (equip.font), its top left
// kPadLeft, kPadTop in from the square's. The squares are read off the picture hook as the game draws them (itemslot.cpp), in rows
// of four; a square with an item's icon over it (the item-icon hook, itemrarity.cpp) has no name, as the game's was under the icon.
namespace vanacore::listtext {

// The items' True Font is on: itemrarity.cpp puts its text hook in for it
bool wanted();

// The Equipment slots' True Font is on: the slot names are VanaCore's own text (itemslot.cpp's picture hook is wanted for it)
bool equip_wanted();

// Around the game's drawing of a window (chat.cpp's window draw), its layout (a window's +4; null none): every window drawn is
// noted with its rectangle, in the order they're drawn (what's drawn later is over what's earlier), an item list's ("menu
// inventor", "mnstorag", "itmstora", "bank") has the names printed in it taken, and the Equipment window ("menu    equip   ")
// has its squares and icons noted
struct Window {
    Window(const void* window, const char* layout);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

private:
    int was_;
    int was_equip_;
    int was_cur_;
    int index_;
    const void* window_;
};

// The Equipment window is being drawn and its slot names are VanaCore's own
bool in_equip();

// The game draws one of the Equipment window's slot squares with its top left at x, y (the interface's units), or an item's icon
// there (a square's own place within a few units): noted for the frame's names
void equip_square(int x, int y);
void equip_icon(int x, int y);

// A string the game is about to print from an item list's cell, its top-left at x, y (the interface's units), in `colour` (the
// game's, 80h a channel full): kept to be drawn whole (true: the game prints nothing). False outside an item list, with the items'
// True Font off, or for a name past ASCII: the game's own print.
bool keep(int x, int y, const char* text, std::uint32_t colour);

// One line of the Equipment window's stats (equipstats.cpp's: its label and its number in the stats box, the Check window's box
// too) as VanaCore's own whole text in `area`'s True Font (VANA_FONT_*), white, its top at y and its left at x, or with `right`
// ending at x (the interface's units): kept to be drawn in the interface pass, so what the game draws over it after (the
// selection cursor's hand, a highlight) leaves it whole (reported 2026-11-14: the numbers under the cursor went back to the game's
// letters). The text is cut where a window drawn after the one it was drawn in starts. False (the caller prints it as before)
// outside a window noted, with the area's True Font off or its letters left out, or for text past ASCII.
bool keep_stat(int area, int x, int y, const char* text, bool right);

// [client] equip.stats is on (equipstats.cpp's reload): every window drawn is noted, as keep_stat's text is kept in the one it's
// drawn in
void set_stats(bool on);

// In VanaCore's interface pass (`width` x `height` the drawn pixels): the names the game printed this frame.
void draw(void* device, unsigned width, unsigned height);

} // namespace vanacore::listtext
