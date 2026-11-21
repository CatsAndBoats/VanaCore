#pragma once

struct lua_State;

// The native half of vana.gameui.window, windows and press (gamewin.hpp; lua/vana.lua wraps press), put into the table on
// the top of the stack (vana.gameui's, lua_api.cpp's open_gameui):
//   window(name) -> { name, x, y, w, h, scale, cursor, count, focused, closing, hidden, drawn, rows, lines, pictures } | nil
//   windows() -> { 'menuwind', ... }
//   native_press(name, row, event) -> true | false, why
//   native_watch(names)                  the windows the addon watches (lua/vana.lua's watch and unwatch give the union)
// Places (x, y, w, h) are the screen's pixels addons draw in (vana.ui.screen), `scale` those pixels to one of the game's
// interface units.
namespace vanacore::lua_gamewin {

void add(lua_State* L);

} // namespace vanacore::lua_gamewin
