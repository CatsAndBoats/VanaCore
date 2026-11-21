#pragma once

#include <string>

// VanaCore's own reads of the session copy of its settings (settings.hpp) from memory. Windows' GetPrivateProfileString
// opens and parses the whole file for every key it's asked for (some 0.1 ms in a 40 KB copy): the thousand-odd keys every
// module reads again while a /vana slider is dragged (overlay.cpp check_settings, each 100 ms) made 150-200 ms frames, and
// the /vana window reads its rows' keys each frame (2026-11-07: "the whole game lags like crazy" moving a nameplate
// slider). GetPrivateProfileStringW, GetPrivateProfileIntW and GetPrivateProfileSectionW of the copy, through
// VanaCore.Core.dll's own imports, are answered from the copy read whole once: each of its sections by Windows' own
// GetPrivateProfileSectionW, a value trimmed and its quotes taken off and a number read by RtlUnicodeStringToInteger as
// Windows reads them (checked against Windows on every key of a copy in game). Read whole again after each write of it
// (ini.cpp's, which tells it; a WritePrivateProfileStringW of it), and when its time or size on the disk is seen to change
// (looked at twice a second at most). Every other file, the section and key lists (a null section or key) and the
// addons' own calls (LuaJIT's FFI) go to Windows as before.
namespace vanacore::inicache {

// From VanaCoreInstall, once the copy is made: `copy` its path (empty: there's none, the game reads the file itself, which
// the launcher may change: nothing is cached).
void install(const std::wstring& copy);

// The copy changed: read whole again at the next read.
void changed();

} // namespace vanacore::inicache
