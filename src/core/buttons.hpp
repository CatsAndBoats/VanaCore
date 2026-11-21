#pragma once

#include <cstdint>

// [client] buttons.*: the game's menu buttons (the main menu's, the Action Menu's, every list of "menu    button*" bars).
// buttons.style=flat draws each button as a box of its own instead of the game's glossy bar: filled with buttons.color
// (buttons.selected.color for the one the cursor is on) at buttons.opacity, inside a border of buttons.border.size
// (buttons.border, buttons.selected.border), reaching buttons.padding.x and .y past the button's own size (less than it
// for less: gaps between the buttons). buttons.text and buttons.selected.text colour the labels (empty: the game's white
// and gold), whatever the style. buttons.selected.nudge=0 keeps the selected button where it's laid out (the game lifts
// it a pixel up and left, and presses it down and right while it flashes); buttons.selected.flash=0 leaves out the flash
// when one is picked. buttons.spacing (flat) moves the rows of buttons apart (buttonspace.cpp).
// buttons.* are /vana Interface > Buttons' Global tab. The same keys for the main menu's windows and the menus it opens in its
// place (True Font's mainmenu.font windows: "menuwind", "socialme", Config, Search, ...) are mainmenu.buttons.* (its Main
// Menu tab), and for the Action Menu and its Magic and Abilities lists ("playermo", "mp_pmode", "magselec", "abiselec",
// "mp_abisc") action.buttons.* (its Action Menu tab). Those two tabs' Enable, mainmenu.buttons and action.buttons: 1 their
// own keys alone (Global's changes don't reach them), 0 Global's alone; never set, each key buttons.*'s until it's set.
namespace vanacore::buttons {

// `ini`: [client] buttons.*, mainmenu.buttons.* and action.buttons.* read now, and again by reload whenever the settings change.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: the game's shape drawing hooked the first time the style is flat, a label has a
// colour or True Font draws the labels (a button's shape drawn is told to the graphics layer: its letters the buttons'
// font, the rest the titles'), and the nudge and flash written into the game's drawing of a window as the settings say.
void on_frame();

// Whether the menu picture drawing (itemslot.cpp's hook) is wanted for the buttons.
bool active();

// The spacing `window`'s rows of buttons are to have (its style's .spacing while flat, else 0): buttonspace.cpp's.
int spacing_for(const void* window);

// The game style's bar colour picked for the window being drawn (.color, or `selected` .selected.color): false while it's
// unset (the game's); and whether any style has one (itemslot.cpp's hook, which draws them, wanted).
bool bar_colour(bool selected, std::uint32_t& rgb);
bool bars_coloured();

// Around a game window's draw (itemslot.cpp's enter_window and leave_window): its buttons in its style (the main menu's, the
// Action Menu's or the global one), the nudge and flash written for it; with the flat style its buttons' labels are kept while it's drawn
// and drawn at leave_window, over all its boxes (a padded box goes over the next button's place).
void enter_window(const void* window);
void leave_window();

// Whether the game's button place (FUN_10118500) nudges the cursor's button now, as the nudge is written for the window
// being drawn: chat.cpp's selection menu rows past the game's third, placed as it would place them.
bool nudging();

// From itemslot.cpp's hook of the game's drawing of one menu picture, for each quad: true if it was drawn here (a
// flat button's bar: the box drawn in its place, with the first), else false for the game to draw it, `colour` the one
// to draw it with (a button's label recoloured), at `x`, `y` (a flat button's label moved by buttons.text.x and .y).
bool on_quad(const char* quad, int& x, int& y, float sx, float sy, std::uint32_t& colour, int flags);

} // namespace vanacore::buttons
