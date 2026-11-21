#pragma once

struct lua_State;

// The native half of vana.game.request_max_hp (docs/addons/04-game-data.md, "Maximum HP and MP"), put into the table on the top
// of the stack (vana.game's, lua_api.cpp's open_game). The Lua half (lua/vana.lua) wraps these in the callback call:
//   native_request_max_hp(ids)    -> a token, or nil and why. `ids`: a server id, or a list of up to 8
//   native_max_hp_answers(token)  -> nil while the request waits; then, once, a list of { id, hp, hp_max, status } in the order
//                                    asked (status: "ok", "not_found", "disabled", "too_fast", "hidden" or "no_answer"; maxhp_ask.hpp)
namespace vanacore::lua_maxhp {

void add(lua_State* L);

} // namespace vanacore::lua_maxhp
