#pragma once

#include <string>
#include <vector>

// Overlays for the game's files, as XIPivot does them: [pivot] enabled=1, rootfolder= (default
// pivot\ in VanaCore's folder), and the overlay folders in it in [pivot.order] (0=ffxi-hd,
// 1=catseyexi: the lowest number is looked in first). Whatever the game reads under the FFXI folder
// (FFXiMain.dll's), it gets from the first overlay that has the same path instead: any file, so the
// ROM* folders' DATs, the root VTABLE.DAT and FTABLE.DAT, and the sound folders. Reads only: a file
// opened for writing is always the FFXI folder's own. The overlays are listed at start, and again
// while the game runs whenever [pivot.order] changes or files come and go under the root folder: the
// game's next file loads (a zone's DATs at the next zone) come from the new list. catseyexi, the
// server's own overlay, is always first and always in use when the root folder has it.
namespace vanacore::pivot {

// Reads [pivot] and [pivot.order] from `ini` (relative folders are from `base_dir`, VanaCore.exe's),
// lists the overlays' files, hooks the file functions of every non-Windows DLL and of the C runtimes
// (ucrtbase, msvcrt: fopen and std::ifstream go through them), and starts watching `ini` and the
// root folder. [pivot] enabled and rootfolder are read here only. Call before plugins::install, so
// that the Windower host's own file redirect runs on top of it.
void install(const wchar_t* ini, const wchar_t* base_dir);

// The overlay file that stands in for `path`, or `path` itself when there's none (or pivot is off).
// Any thread.
std::wstring resolve(const std::wstring& path);

// [pivot] rootfolder, absolute, no trailing backslash: where the overlay folders are. Empty when pivot
// is off or the folder isn't there. Set by install(), before plugins are loaded.
const std::wstring& overlay_root();

// The DAT files under the FFXI folder opened to read through the hooked file functions lately (the game's own loads: a
// zone's, its sub-areas', the models around; the last 1024 to 2048), relative to the FFXI folder in lower case
// ("rom2\22\4.dat"), the most recent first. Empty when pivot is off. Any thread.
std::vector<std::wstring> opened_dats();

} // namespace vanacore::pivot
