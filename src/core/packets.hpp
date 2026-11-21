#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// FFXI's packets. The game decrypts each buffer the server sends into a 28-byte header followed
// by packets (a 4-byte header each: id in the low 9 bits of the first word, size in 4-byte units
// in the high 7, then the server's sequence number), and builds each buffer it sends the same way
// before encrypting it. VanaCore sits between: plugins see each packet, can change or block it,
// and can add packets of their own. Game thread only, except inject().
namespace vanacore::packets {

// `ini` (vanacore.ini): [client] packets.keepactions read now, and again whenever it changes. An action from the server
// (0x028) that an addon blocks still goes to the game, without its chat lines, so its animation plays: an addon that blocks
// one by mistake (SimpleLog's resend check) took the cast, the weapon skill or the spell's effect away. A buffer the
// server sends again keeps the addons' blocks (its actions came the first time), and so does a plugin's block. On unless
// the settings file turns it off.
void configure(const wchar_t* ini);

// Once per frame, from Present: the first time, hooks the game's packet decryption and
// encryption in FFXiMain.dll (its code is decrypted by then). The setting is read again when its file changes.
void on_frame();

// The buffer being handled, received (`incoming`) or sent: each one is numbered, one more than the last. Addons' packet
// events have it as e.vana_chunk, the same for every packet of a buffer and those added to it (compat\ashita.lua's
// SimpleLog fix tells buffers apart by it: a buffer sent again has the same bytes). Game thread.
std::uint32_t buffer_number(bool incoming);

// Adds a packet to the next buffer the game receives (`incoming`) or sends. `data` is the whole
// packet, its 4-byte header first (VanaCore sets its id, size and sequence), `size` bytes long.
// Any thread. `from`: the Ashita plugin that adds it, if one does. Ashita calls a plugin's packet handler for
// its own packet inside its Add...Packet call, where plugins like LegacyAC (which re-add the cast packet
// after changing gear) have a guard that ignores it; VanaCore adds the packet to the next buffer, so the
// plugin is left out when the packet goes through the plugins, or its own packet would start its swap again.
void inject(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::string_view from = {});

// True once the hooks are in.
bool ready();

// Whether the character is between zones: the server's zone-out packet (0x00B) came and the player isn't back in the world
// (`in_world` false) yet. Ashita's login status reads 1 ("loading") then, not 0 (the title screen): addons such as FancyChat
// treat 0 as a logout and reload themselves. Any thread.
bool zoning(bool in_world);

// How long ago (ms) the server's last packet came; 0 before the first. A connection that is lost keeps growing it (the game
// itself gives up on the server after 90 s). Any thread.
unsigned long long server_silence();

// The zone the player is in, given the one the game's party list has for them (`listed`, Ashita's GetMemberZone(0)): the
// zone the server's zone-in packet (0x00A) named, until the list has it. The list only catches up once the new zone has
// loaded, seconds after the packet, so addons that wait a moment after 0x00A to read the zone (zonename) read the last one.
std::uint16_t player_zone(std::uint16_t listed);

// Whether the player is in their own Mog House, as the server's last zone-in packet (0x00A) said (its byte 0x80: 1 there, 2
// elsewhere); false before one came. Any thread.
bool in_mog_house();

// Whether a home point's teleport is under way: the destination picked (0x05B, option 2) and the zone not yet left (0x00B),
// for 15 s at most. Any thread.
bool warping();

// The last packet with this id the game received (`incoming`) or sent, as it got or sent it, its header first; empty
// if none yet. Any thread.
// `at`, if given, is when it came (seconds; later packets have later times).
std::string last(bool incoming, std::uint16_t id, double* at = nullptr);

} // namespace vanacore::packets
