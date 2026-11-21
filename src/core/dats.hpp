#pragma once

#include <cstdint>
#include <string>
#include <vector>

// FFXI's data files (DATs), found by file id the way the game finds them: each ROM folder's
// VTABLE says whether it has the file, its FTABLE where (ROM<n>\<folder>\<file>.DAT). Through
// pivot's overlays, as the game gets them (tables and DATs). Any thread.
namespace vanacore::dats {

// The FFXI folder the game runs from (FFXiMain.dll's), with a trailing backslash; empty before
// FFXiMain.dll is loaded.
std::wstring ffxi_folder();

// DAT `file_id`'s full path (an overlay's when one has it), or empty if no table has it.
std::wstring path(std::uint32_t file_id);
// The same under the FFXI folder ("ROM2\12\107.DAT"), or empty.
std::wstring rom_path(std::uint32_t file_id);

// Reads DAT `file_id`. False if it isn't there.
bool read(std::uint32_t file_id, std::vector<std::uint8_t>& out);

// Reads the file at `path` (up to 256 MB) whole. False if it can't.
bool read_file(const std::wstring& path, std::vector<std::uint8_t>& out);

} // namespace vanacore::dats
