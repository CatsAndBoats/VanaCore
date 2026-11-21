#pragma once

namespace vanacore::shutdown {

// The game is closing: every addon's unload event (the last loaded first), then every plugin unloaded, as Ashita and
// Windower do when the game closes - so what they save as they go is saved. Once, at the first of: the game's window
// closing, FFXI's GameStart returning, ExitProcess. `why` says which, in the log. On the game's thread.
void run(const wchar_t* why);

// The same from the crash handler, as FFXI's teardown faults (it does on every /shutdown): on a thread of its own,
// waited for at most `wait_ms`, so a teardown that left a lock held can't keep the game from ending.
void run_bounded(const wchar_t* why, unsigned wait_ms);

// The game ended at once by VanaCore, without the game's own teardown (its window closed where /shutdown can't work): the
// game's own files saved as its logout saves them (gamesave: the macro book and set in use), run_bounded, then the
// process ends with exit code 0. On the game's thread.
[[noreturn]] void end_now(const wchar_t* why, unsigned wait_ms);

// ExitProcess, in every module but Windows' own, goes through run first. Call from VanaCoreInstall.
void install();

} // namespace vanacore::shutdown
