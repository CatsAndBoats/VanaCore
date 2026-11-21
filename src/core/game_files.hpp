#pragma once

namespace vanacore::game_files {

// Reads [client] pol= (PlayOnline folder) and ffxi= (FFXI folder) from `ini`; relative paths are
// from `base_dir` (VanaCore.exe's folder). PlayOnline and FFXI then load their DLLs from those
// folders instead of wherever Windows has their COM classes registered, and read them as their
// install paths (InstallFolder). Call before registry::install.
void install(const wchar_t* ini, const wchar_t* base_dir);

} // namespace vanacore::game_files
