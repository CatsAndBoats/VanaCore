#pragma once

// [client] buttons.spacing (with buttons.style=flat; the main menu's windows mainmenu.buttons.spacing and .style, the Action
// Menu's action.buttons.*, each the global one's until set: buttons.cpp's spacing_for): the game's menus' buttons moved apart, this many of the layout's units
// between each row of them (0: the game's, touching), the window made taller by as much. The menus that are up take a
// change at their next draw.
namespace vanacore::buttonspace {

// From hudwin.cpp's hook of the game's making of a window from its layout (SetResourceData): `window` made (again), its
// parts as its layout has them.
void made(const void* window);

// From chat.cpp's hook of the game's window draw, first (before hudwin places the window, so it goes by the spaced size):
// the window spaced as the settings say, if it isn't.
void drawing(void* window);

} // namespace vanacore::buttonspace
