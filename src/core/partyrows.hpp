#pragma once

// [client] party.spacing, alliance.spacing: room between the party list's rows (the alliance lists'), in the interface's
// units (0-40, 0 the game's), each list that much taller for each row past the first with its bottom where it was: it
// grows up, as the party list does when members join. The alliance lists, while their Position is off, go up by as much as
// the lists under them grew (the game stands them on a full party list), so they stay clear of it; one the player has put
// somewhere (Position on, or dragged in the HUD Manager, which turns it on) stays where it's put. hudwin.cpp calls space()
// for those three windows as it places them, before each is drawn.
namespace vanacore::partyrows {

using Resize = void(__thiscall*)(void* window, int x, int y, int w, int h, char a, char b, char c);

void reload(const wchar_t* ini);

// `id` hudwin's ("party", "alliance1", "alliance2"; any other does nothing), `window` the window, `resize` the game's
// ResizeWindow, `moved` its <id>.move (the player's Position: no going up for the lists under it)
void space(const char* id, char* window, Resize resize, bool moved);

} // namespace vanacore::partyrows
