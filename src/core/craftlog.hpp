#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Crystals, clusters and synthesis in vanacore.log, always on, for reports like "after I used a cluster, no crystal works"
// (lines start "Craft:", at most 300 a session). Logged:
//  - the server's answers to a synthesis (0x06F, 0x070) and to a crystal being used, as the packets that came back within
//    5 s of it (their ids and sizes: no answer is itself the finding);
//  - every crystal and cluster stack as the server changes it (0x01E, 0x01F, 0x020): bag, slot, what and how many;
//  - the synthesis the game sends (0x096): the crystal and ingredients with their slots, each checked against what the
//    server last said that slot holds (a "!" marks a slot that holds something else, or that the server never described);
//  - an item used (0x037) with the slot's item (every crystal or cluster, 40 others a session);
//  - the server's lines about it (chat modes 121-123, and the system modes while one of the above is going on, or with
//    synth/crystal/cluster in them), without any player's chat; and zone-ins (30 a session);
//  - the game's own "no item can be used now" test, which greys Use out in the Items menu (for every item) while true:
//    off until a crystal, a cluster or a synthesis is seen, then looked at every 250 ms for 15 minutes after the last
//    one, each change logged ("the game's item-use test is ON ...", 40 lines a session).
// Nothing a player types or says is logged.
namespace vanacore::craftlog {

// From packets.cpp: a buffer from the server as decrypted, and the buffer the game sends. Both sizes include the 28-byte
// header.
void on_received(const std::uint8_t* buffer, std::size_t size);
void on_sent(const std::uint8_t* buffer, std::size_t length);

// From addons.cpp: a line coming into the chat log, in chat mode `mode`.
void on_text(int mode, std::string_view message);

} // namespace vanacore::craftlog
