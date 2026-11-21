#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// [client] packets.watch: a record in vanacore.log of what the server's packets did on their way in, for "my weapon skill
// showed nothing" (no animation, no chat line, only the TP gone): where did the packet go?
//  - Each buffer from the server starts with a counter that goes up by one a buffer. One that skips (3 after 1) means a buffer
//    the server sent never reached the game: lost on the way (or never sent). One that repeats or goes back is the server
//    sending it again. The counter starts over in a new zone, so those aren't counted.
//  - Each of your own actions (0x028 with you as the actor, but your melee) is logged as it arrives: if the weapon skill that
//    showed nothing has a line, the packet reached VanaCore and the loss is after it; if it has none, it never came.
//  - Action packets that came in a buffer and weren't in the one the game got, and any packet an addon or plugin blocked
//    (and which) are logged.
//  - Every 30 seconds, one line of counts.
// Off unless vanacore.ini turns it on (/vana > Client > Game > Packet watch); follows it while the game runs. Game thread.
namespace vanacore::packetwatch {

// `ini` (vanacore.ini): read now, and again whenever it changes.
void install(const wchar_t* ini);

// Once a frame on the game's thread: the setting re-read when its file changes, and the 30-second line.
void on_frame();

// Whether it's on.
bool on();

// From packets.cpp's receive hook: a buffer from the server as decrypted, before anything of VanaCore's touches it, and the
// same buffer as the game is given. Both sizes include the 28-byte header.
void on_received(const std::uint8_t* buffer, std::size_t size);
void on_delivered(const std::uint8_t* buffer, std::size_t size);

// A received packet was blocked: by an addon (its name) or by a plugin ("a plugin"). Any thread that runs packet handlers.
void blocked(std::string_view by, std::uint16_t id);

} // namespace vanacore::packetwatch
