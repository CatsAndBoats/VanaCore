#pragma once

#include <cstddef>
#include <cstdint>

// Other players, their pets and their trusts left out, for slow PCs in busy places ([graphics] hide.players, hide.pets,
// hide.trusts). The server's updates about them (0x00D, 0x00E) are taken out of each buffer before the game and the
// plugins see it, so the game never makes them: nothing of theirs is loaded, animated or drawn. You, your party and
// alliance (and their pets and trusts) and anyone who asks you to trade are always kept. What's taken out is kept here
// (the last update of each kind since each came into view), so one who joins the party, or the setting turned off,
// brings them back at once; turning it on takes out those already there (an update saying each is gone).
namespace vanacore::others {

// What's left out: any of these.
enum : unsigned { kPlayers = 1, kPets = 2, kTrusts = 4 };

// Any thread.
void set(unsigned hide);

// Each buffer the game receives, `size` bytes, its 28-byte header first: on the game's thread, after trusts::on_received
// and before the plugins see its packets. Takes out the packets about those left out and returns the new size. The
// updates that take others out of the game or bring them back are added to the buffer (packets::inject), about `room`
// bytes of them at most: the space left in it.
std::size_t on_received(std::uint8_t* buffer, std::size_t size, std::size_t room);

} // namespace vanacore::others
