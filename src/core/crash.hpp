#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace vanacore::crash {

// The code of a C++ exception thrown by Visual C++ code, as a structured exception.
inline constexpr DWORD kCppException = 0xE06D7363;

// A C++ exception's std::exception::what() into `out` (at most `size` bytes). False if `record`
// isn't one, or isn't a std::exception. Call it from an exception filter: the thrown object lives
// on the stack that the handler unwinds.
bool cpp_exception_message(const EXCEPTION_RECORD& record, char* out, std::size_t size);

// The return addresses on a faulting thread's stack, nearest first, at most `max` of them into `out`: the values from the
// fault's ESP up (2 KB of it) that point just past a call in executable memory (a DLL's code, or code made as the game
// runs: LuaJIT's machine code, hooks). For the reports of a fault the game survives (an addon's, a plugin's): the fault's
// own address is often a C library routine (strlen) and only its caller says who passed it the bad pointer. Call it from
// an exception filter, before the stack unwinds.
int callers(const CONTEXT& context, std::uintptr_t* out, int max);

// Handles the game's unhandled exceptions, then hands them to whatever handler the game set
// (handlers the game sets later go behind VanaCore's). With [logging] crashdumps (`dumps`):
//   0  one CRASH line in the log: what, where, which thread
//   1  plus registers, the call stack, code addresses found on the stack and the loaded DLLs
//      (path, base, size, build time), and logs\crash-<time>-<pid>.dmp: threads, stacks, the
//      DLLs' code (FFXiMain.dll's as decrypted in memory) and data, and the memory they point at
//   2  as 1, but the dump holds all of the game's memory (hundreds of MB)
void install(const wchar_t* install_dir, int dumps);

// The game was told to quit (/shutdown). Its teardown faults in FFXiMain's heap every time, well after
// the game looks closed: for the next two minutes (the countdown and the teardown; a cancelled
// shutdown is past it by then) a fault gets one line in the log, and no report or dump.
void shutting_down();

} // namespace vanacore::crash
