#pragma once

#include <cstdint>

struct lua_State;

// The native half of vana.game's entity functions (docs/addons/04-game-data.md, "Entities"), put into the table on the top of
// the stack (vana.game's, lua_api.cpp's open_game):
//   entity(index)     -> the entity at that target index with all the game's record of it, and the health of it where the
//                        party has it (hp, mp, tp, hp_max, ...: entityhp.hpp), or nil
//   entity_by_id(id)  -> the same for the entity with that server id (every slot looked at: ask when it's needed), or nil
//   vitals(id)        -> { id, party_slot, hp, hpp, mp, mpp, tp, hp_max, hp_max_low, hp_max_high, hp_max_source, mp_max, ... }
//                        for a member of the party or alliance (also while they're in another zone, with no entity), or nil
namespace vanacore::lua_entity {

void add(lua_State* L);

// For a table on the top of the stack that is a party member's (vana.game.party's members): its hp_max, hp_max_low,
// hp_max_high, hp_max_source and the same for mp. `id` is their server id; a source of "none" and no numbers for anyone
// the party list doesn't have.
void set_max(lua_State* L, std::uint32_t id);

} // namespace vanacore::lua_entity
