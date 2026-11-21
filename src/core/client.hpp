#pragma once

#include <string>

namespace vanacore::client {

// Reads [client] retail from `ini` (the game's copy of the settings file), or takes `forced` (VanaCore.exe's --retail). Call
// first, before anything that asks retail().
void install(const wchar_t* ini, bool forced);

// [client] retail=1 or --retail: the game is PlayOnline as installed (retail or Steam) that VanaCore.exe started, not a private
// server's through VanaCore's own login or a bootloader. Everything such a server's setup stands in for is then left to the
// machine: PlayOnline and FFXI keep their real registry keys and folders (no stand-in keys, no [client] pol=/ffxi=
// redirect, no defaults for settings [ffxi] doesn't name), and the server's own DAT overlay (pivot's catseyexi) isn't used.
bool retail();

// The server's host as VanaCore.exe was given it (--server, else [server] server=), in lower case; empty without one
// (PlayOnline as installed). Set once as the core installs, before anything asks server().
void set_server(const wchar_t* host);
const std::string& server();

} // namespace vanacore::client
