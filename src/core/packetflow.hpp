#pragma once

#include <cstddef>
#include <cstdint>

// [client] packetflow: the game sends to the server sooner. Before it sends a buffer, FFXiMain.dll asks whether long enough
// has passed since the last one, giving the wait; it waits longer still while zoning. With packetflow on, once the server
// has answered the last buffer sent (or after the game's zone-out packet, 0x00D) the wait is cut to 250, so the two
// sides take turns as fast as the latency allows: zoning and busy instances fill in sooner. While no answer has come, the
// game's own wait stands. What the PacketFlow plugin did (Thorny, idea by Byrth; its source isn't published, so this is
// rebuilt from what its DLL patches), found and patched by VanaCore itself; PacketFlow is held off while this is on. On
// unless vanacore.ini turns it off, and follows it while the game runs.
namespace vanacore::packetflow {

// `ini` (vanacore.ini): read now, and again whenever it changes.
void install(const wchar_t* ini);

// Once a frame on the game's thread (Present): the game's code patched in or out as the setting says (once FFXiMain.dll
// is unpacked and packets.cpp's hooks are in).
void on_frame();

// From packets.cpp's hooks: a buffer came from the server (before it's decrypted, whether or not it decrypts), and the
// buffer the game sends, `length` bytes, its 28-byte header first.
void on_received();
void on_sent(const std::uint8_t* buffer, std::size_t length);

} // namespace vanacore::packetflow
