#pragma once

#include <string>

namespace vanacore::registry {

// The PlayOnline keys whose values VanaCore can serve (see classify() in registry.cpp).
enum class Key {
    Settings,      // HKLM\SOFTWARE\PlayOnline[US|EU]\SquareEnix\FinalFantasyXI: [ffxi], [ffxi.registry]
    InstallFolder, // HKLM\SOFTWARE\PlayOnline[US|EU]\InstallFolder: install paths ([client] pol=, ffxi=)
    PolSettings,   // HKLM\SOFTWARE\PlayOnline[US|EU]\SquareEnix\PlayOnlineViewer\Settings: [language]
};

// Serves `text` as REG_SZ for value `name` of `key` in place of the registry's. Call before install().
void serve(Key key, const char* name, const std::wstring& text);

// The keys a vanacore.ini from before has where they no longer are, moved to where they are now or taken out
// (window mode -> [ffxi] mode, window size). Writes ini: it's the file itself, before settings::install makes the game's
// copy of it, not the copy.
void migrate(const wchar_t* ini);

// Reads the game's settings from `ini`: [ffxi] by name (resolution.game=1920x1080 is 0001 and 0002,
// mipmap=6 is 0000, ...) and [ffxi.registry] by value name (0001=1920, padmode000=...), [ffxi]'s
// winning where both set one ([ffxi.registry]'s with [ffxi] registry.override=1), then [ffxi]'s
// defaults (a new account's in the launcher) for what neither sets. Also [language] playonline (the
// game's text language: 1 Japanese, 2 English, 0 the game's own). Hands those values, and any added
// with serve(), to every DLL in the process that reads them: each DLL's own registry imports are
// hooked (the game's, plugins', VanaCore's; not Windows'), and advapi32's, so a call reaches VanaCore
// however the caller found the function. Checked at install (the log says). First moves the keys a
// vanacore.ini from before has where they no longer are ([window] mode to [ffxi], [window] resolution
// to [graphics]; size is taken out, the window being resolution.game's size). A PlayOnline install's
// keys are always stood in for by keys made under HKCU\Software\VanaCore\Registry, whether or not the
// registry has them, serving these values, what the game writes, then a normal install's; the
// registry's own are never read or changed. [ffxi] -1 (no default) or [ffxi.registry] -1 or empty
// keeps the stand-in's own value. [ffxi.registry]'s whole numbers are served as REG_DWORD, anything
// else as REG_SZ. Other PlayOnline keys the game opens for writing, which fails without admin rights,
// are opened read-only instead. Also logs registry calls on PlayOnline keys (capped).
void install(const wchar_t* ini);

// FFXI's window mode as configured (0 full screen, 1 windowed, 2 borderless windowed, 3 borderless
// full screen: [ffxi] mode, else 0034 as the launcher set it), once the game has read it; -1
// before. The game itself knows 0 and 1: it's given 1 for 2 and 3, and window.cpp makes the window
// borderless.
int window_mode();

// The game's setting `name` (its value name: "0001", "padmode000") as vanacore.ini hands it to the
// game, [ffxi]'s, [ffxi.registry]'s or [ffxi]'s default, as text (a REG_DWORD as a signed number;
// "0034" is [ffxi] mode's 0-3 when that's set); empty when it's left to the registry. Fixed once
// install() has run.
std::wstring setting(const char* name);

// A resolution as [ffxi]'s resolution keys take it: <width>x<height> (1920x1080); auto, the
// resolution Windows has the monitor at (the one `window` is on, an HWND; the main one for nullptr);
// or auto-<percent> (auto-75: three quarters of that each way, 1-100). False for anything else.
bool parse_resolution(const std::wstring& text, void* window, long& width, long& height);

// [ffxi] resolution.ui's "fit the game window": window (100) or window-<percent> (window-75, 1-100), the menus that share
// of the game's resolution, which in the windowed modes is the window's client area (its title bar and borders left
// out); 0 for anything else. At the start the share of [ffxi] resolution.game's size; while the game runs uiscale.cpp
// works it out again as the window and [ui] aspect change.
int window_share(const std::wstring& text);

// [graphics] background as written: launcher (or empty) is 0 x 0, the launcher's [ffxi] resolution.background;
// <width>x<height> (4096x4096) or one side (4096, square), each 512 to 8192. False, and 0 x 0, for anything else.
bool parse_background(const std::wstring& text, long& width, long& height);

// [ffxi.registry] sets value `name` (0034, 0001) over [ffxi]'s key for it (registry.override=1), so
// that key, read again while the game runs ([ffxi] mode, resolution.game), is ignored.
bool registry_overrides(const char* name);

} // namespace vanacore::registry
