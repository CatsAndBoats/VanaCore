#pragma once

struct lua_State;

// The native half of vana.ashita: what compat/ashita.lua builds Ashita's API for addons from (lua_api::open puts it there).
namespace vanacore::lua_ashita {

// A new table of its calls, left on the stack: object, string, item, item_icon, win32, parse_auto_translate, gui_visible,
// execute, console and regex (search, match, replace, split); with interface and libs, an Ashita addon's
// [addons] ashita.interface and ashita.libs (lua_api::Context), for compat/ashita.lua.
void push(lua_State* L, double ashita_interface, double ashita_libs);

} // namespace vanacore::lua_ashita
