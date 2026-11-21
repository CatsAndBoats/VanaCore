#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace vanacore::scan {

// Patterns are hex bytes, two digits each, spaces optional, "??" (or "?") matching any byte:
// "8B 44 24 04 8B 0D ?? ?? ?? ??" or "8B4424048B0D????????". A pattern can list alternatives with "|": the same code as
// two builds compile it ("66 8B 04 ?? ?? ?? ?? ?? 66 85 C0 | 66 8B 04 ?? ?? ?? ?? ?? 8D 34"). The first alternative that
// matches anywhere is the match, so list them in the order the builds are met. Leave "??" over what a build moves (a
// displacement, a limit, a register's encoding), and read it back with operand().

// Finds `pattern` in `module`'s executable sections and returns the address of its first match,
// or 0. A pattern that finds nothing is logged once (sigs::note_scan), with the module's build the first time.
std::uintptr_t find(HMODULE module, const char* pattern);

// The 32 bits at `site + at`: a displacement, immediate or address in the code a pattern found, which the pattern leaves
// as "??" so it matches a build where the game has moved it. 0 if `site` is 0.
std::uint32_t operand(std::uintptr_t site, std::ptrdiff_t at);

// Finds the `count`th match (0 for the first) of `pattern` in the readable memory from `begin` for
// `size` bytes, and returns its address, or 0. Unreadable pages are skipped.
std::uintptr_t find(std::uintptr_t begin, std::size_t size, const char* pattern, unsigned count = 0);

} // namespace vanacore::scan
