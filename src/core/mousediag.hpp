#pragma once

#include <windows.h>

// [logging] mouse=1: a trace of how the game finds the mouse, for a cursor that's off from what the
// game points at (the title screen, character select). Logs the game's (FFXiMain.dll's and
// polcore.dll's) calls for the cursor and the window's geometry - GetCursorPos, ScreenToClient,
// ClientToScreen, GetClientRect, GetWindowRect, GetSystemMetrics, MapWindowPoints, GetMessagePos -
// with what each returned, and the mouse messages the game's window gets (as Windows sent them and
// as the game gets them), a few of each and then one a second. Read when the game starts.
namespace vanacore::mousediag {

// From VanaCoreInstall.
void install(const wchar_t* ini);

// A mouse button message arriving at the window procedure, before anything takes it (window.cpp).
void on_entry(UINT message, LPARAM lparam);

// A press or release VanaCore kept from the game: `by` says what took it (ImGui, a plugin, an addon, or the
// low-level mouse hook for a VanaCore window).
void on_taken(UINT message, const char* by, int x, int y);

// A mouse message reaching the game's window procedure: `sent` as Windows sent it, `given` as the
// game gets it (window.cpp).
void on_message(UINT message, LPARAM sent, LPARAM given);

} // namespace vanacore::mousediag
