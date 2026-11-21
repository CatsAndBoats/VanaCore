#pragma once

#include <vanacore/boot_info.hpp>

// VanaCore's stand-in for a bootloader (xiloader 2.1.2), after VanaCore.exe's own login: the game
// process's side. Everything else a bootloader offers (menus, account creation, password changes,
// one-time passwords, the hairpin fix) is left out on purpose: the CatsEyeXI launcher does those.
namespace vanacore::loader {

// With `boot.login` from VanaCore.exe: hooks the game's winsock (ffxi00.pol.com is the server, and
// the lobby's packets carry the session hash), keeps the lobby's data link to the server, stands
// in for PlayOnline's profile server, sets PlayOnline (polcore) up and starts FFXI in this
// process. Returns once the game has closed, with the process's exit code: 0, or
// kExitGameStartFailed if PlayOnline or FFXI couldn't be started (the reason is in the log and on
// the console).
int run_game(const BootInfo& boot);

} // namespace vanacore::loader
