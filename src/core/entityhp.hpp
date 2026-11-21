#pragma once

#include <cstdint>

// The health of the party's members, and the maximum HP and MP the game never says for any but the local player.
//
// The game's own copy has a maximum for the player only (the server's 0x061). A party or alliance member's HP, MP and TP come
// with their percents (0x0DD); any other entity has a percent and no number. Addons that size a heal by a member's maximum
// kept their own: note the current value when the percent reads full (Ashita addons, "Max HP of an entity"). Here it is done
// once for all of them, from the game's own party data every frame (entityhp_track.h has how), and given to addons as
// vana.game.entity's and party()'s hp_max (lua_entity.cpp) and to plugins in the plugin API (pluginapi.cpp).
// Game thread only.
namespace vanacore::entityhp {

// Where a maximum comes from
enum class Source : std::uint8_t {
    None = 0,     // VanaCore has no number for it (an entity outside the party: only its percent is known)
    Self = 1,     // the game's own: the local player's
    Full = 2,     // the value seen while the percent read 100 (to within a percent), kept until a sample disagrees
    Estimate = 3, // worked out from a value and its percent: `low` to `high` is what the samples so far allow
    Server = 4,   // the server said it, when asked (maxhp_ask.hpp): for any entity it lets the client ask about, a monster too
};
// "none", "self", "full", "estimate" or "server"
const char* source_name(Source source);

struct Max {
    Source source = Source::None;
    std::uint32_t value = 0; // the maximum (0 for none)
    std::uint32_t low = 0;   // it is not below this ...
    std::uint32_t high = 0;  // ... nor above this (4294967295 for no upper limit yet). Both equal `value` for Self
};

// A party or alliance member's health, as the game has it, with its maximums.
struct Vitals {
    std::uint32_t id = 0;
    unsigned party_slot = 0; // 0-17: party 1 (0 the local player), then the alliance's other two parties
    std::uint32_t hp = 0, mp = 0, tp = 0;
    std::uint8_t hpp = 0, mpp = 0;
    Max hp_max;
    Max mp_max;
};

// Once per frame, before the addons' and plugins' frames: the party's samples taken.
void on_frame();

// The member with this server id (the local player included), as of the last frame. False for anyone else.
bool vitals(std::uint32_t id, Vitals& out);

} // namespace vanacore::entityhp
