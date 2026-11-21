#pragma once

#include <cstddef>
#include <cstdint>

struct lua_State;

// A zone change as it loads, for the loadscreen addon (vana.game.loading). From the server's zone-out (0x00B):
//   server  until its zone-in (0x00A) names the zone
//   zone    the game reads the zone's data files on its render thread; over once it has read none for 0.5 s
//   people  the zone is drawn; the game's other threads read its people's models; over after 2 s with none
// The data files each zone needed are kept in cache\zoneload.txt, so the next load of it has a total to count toward.
//
// [logging] zoneload=1 also writes each zone change's timeline to logs\zoneload-<stamp>.log, in ms from the zone-out
// with the thread: the packets (the first of each kind), every file opened (and its size), and the frames shown (each
// gap of 40 ms or more, and a count each second), until 15 s after the zone-in.
namespace vanacore::zoneload {

// From VanaCoreInstall, after pivot's: the settings, the cache, and the game's CreateFile hooked to see what it opens.
void install(const wchar_t* ini, const wchar_t* install_dir);

// Each packet as it goes by (packets.cpp's remember), any thread.
void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length);

// The Present hook, on the render thread, before the device's Present.
void on_present();

// vana.game.loading, into the table on the top of the stack (lua_api.cpp's open_game).
void add_lua(lua_State* L);

} // namespace vanacore::zoneload
