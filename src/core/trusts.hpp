#pragma once

#include <cstddef>
#include <cstdint>

// The effect trusts arrive with. The server (LandSandBoat) marks every update about a trust with the flag
// that has the game play a summoned creature's arrival (0x00E's PetNewFlag), not only the one sent as it's
// summoned: walking up to someone with trusts out, each plays it at once. VanaCore takes the flag out of the
// updates about a trust it didn't see summoned ([graphics] trusts.spawn). Never from a trust in the party or alliance:
// the same flag is what lets the game release one (the party menu's Release, /refa).
namespace vanacore::trusts {

// cast: the effect for trusts seen being summoned only; always: for every trust that comes into view (as
// the server sends it); never: for none.
enum class Spawn { Cast, Always, Never };

// Any thread.
void set(Spawn spawn);

// Each buffer the game receives, `size` bytes, its 28-byte header first: on the game's thread, before
// the plugins see its packets. Changed in place.
void on_received(std::uint8_t* buffer, std::size_t size);

// True while the entity at `index` (its target index) is a trust, as the server's updates about it say.
// The game's thread.
bool is_trust(std::uint16_t index);

} // namespace vanacore::trusts
