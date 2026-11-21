#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

// Which FFXiMain.dll is loaded, and what its own code says about the structures VanaCore reaches into. CatsEyeXI's client
// and retail's are built from the same source but not the same build: most of the code is byte for byte the same, and what
// moves is where fields sit inside the game's big objects (an item's data +0x800, the game's own object +8, +0x70 and +0x78
// at different places) and which register the compiler picked. VanaCore finds everything by pattern, so a pattern must leave
// open anything that moves (a displacement, a limit, a register), and read what it needs back out of the code it found
// (scan::operand) rather than assume the number. tools\sigaudit.py checks every pattern in the source against two builds'
// code, offline, and says which would miss and which functions changed under the ones that match.
namespace vanacore::sigs {

struct Build {
    std::uint32_t timestamp = 0;  // the PE header's TimeDateStamp
    std::uint32_t image_size = 0; // its SizeOfImage
    const char* name = nullptr;   // a build tools\sigaudit.py has checked, else nullptr
};

// `game`'s build, from its header
Build build(HMODULE game);

// scan::find() tells this about each scan of a module: the first one of FFXiMain.dll logs its build, and a pattern that
// found nothing is logged once, so a client VanaCore hasn't met says in its log which of its features lost their place.
void note_scan(HMODULE module, const char* pattern, std::uintptr_t found);

// How many different patterns have found nothing so far
unsigned missed();

// An item's data (CTkItemDataMenber, the one the item icon drawing is called on): how far its fields are from where
// CatsEyeXI's build has them (+0xCB8 the item's id, +0xCC8 its name, +0xCE8 its icon's bitmap, +0xD0C its icon...), read
// from the icon drawing's own code: 0 for CatsEyeXI's, 0x800 for retail's. 0, with a log line, if the code isn't found.
std::ptrdiff_t item_data_shift();

} // namespace vanacore::sigs
