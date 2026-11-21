#pragma once

// The game's own save of the files it holds in memory for the character (USER\<id>\: mcr.sys, the macro book and set in
// use; mcr.ttl and mcr_2.ttl, the books' names; and the rest registered the same way). The game writes them only as it
// leaves the world: its teardown (/logout, /shutdown, its own exit; FFXiMain VA 0x10014970 and 0x100141F0) is all that
// calls the save (0x101A9F30). A game ended any other way comes back on the macro book and set of the last clean logout.
// Reported (Arestia, 2026-11-12): since the window's X could end the game at once, the macro bar came back on another
// page after logging in.
namespace vanacore::gamesave {

// Runs the game's save now, before VanaCore ends the process itself (shutdown::end_now, /terminate). On the game's
// thread only: elsewhere nothing is run. False if it wasn't run (another thread, not found in FFXiMain.dll) or it
// faulted; vanacore.log says which.
bool run();

} // namespace vanacore::gamesave
