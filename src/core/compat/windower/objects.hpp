#pragma once

// What host.cpp and objects.cpp share: the objects Windower plugins are given, and the plugin
// being called (whose objects are freed when it unloads).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "abi.hpp"

namespace vanacore::windower {

// The plugin manager every plugin gets in Load.
abi::PluginManager* manager();

// The plugin being called on the game's thread (as its file spells it), empty otherwise: text and
// primitives made now are its own.
std::string_view calling();
// VanaCore's folder, with a trailing backslash.
const std::string& install_path();

void set_device(void* device);
// Frees text and primitives plugin `owner` made (it's unloading).
void release_objects(std::string_view owner);
// Draws every visible text and primitive, in the order they were made.
void draw_objects(void* device);
// Keeps the packet for GetLastIncoming / GetLastOutgoing.
void record_packet(bool incoming, std::uint16_t id, const std::uint8_t* data, std::uint32_t size);

// Windower's game state, kept from the server's packets (state.cpp): the pointer table
// PacketStreamHandler::GetStreamState returns, and each buffer the game receives (decrypted).
const void* const* state_pointers();
void state_on_received(const std::uint8_t* buffer, std::size_t size);
// From the same state: the server's clock (Unix seconds, the last buffer's header; 0 before one) and the zone's
// weather (0x00A's, then 0x057's: Windower's weather id). Windower addons' get_info reads them (vana.game.world).
void state_world(std::uint32_t& server_time, std::uint32_t& weather);

// Text for VanaCore's log and chat: a plugin's own name in front.
void say(std::string_view source, std::string_view text);
// A Windower console line for the chat log: "\cs(r,g,b)" (a colour) and "\cr" (back to normal)
// become FFXI's own colour codes.
std::string console_text(std::string_view text);

// Windower 3 plugins' host (legacy.cpp), and freeing the text objects plugin `owner` made there.
abi::legacy::Host* legacy_host();
void legacy_release(std::string_view owner);

} // namespace vanacore::windower
