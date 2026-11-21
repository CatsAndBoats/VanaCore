#pragma once

namespace vanacore::session {

// [client] attached, from VanaCore.exe. Call from VanaCoreInstall.
void install(bool attached);

// Tells VanaCore.exe the game is up (its Direct3D device exists), so with attached=0 it exits;
// the game then lets go of the console it shared with VanaCore, so that window closes (or a
// terminal gets its prompt back) and closing it can't take the game down. Only the first call
// does anything.
void game_started();

} // namespace vanacore::session
