#pragma once
// VanaCoreApp.dll kept up to date from GitHub, at the splash. Only the DLL: a new VanaCoreApp.exe is a download of its own
// (the player's), and VanaCore itself is the DLL's to update. Nothing here ever stops the start: whatever fails, the DLL
// stays as it was and the app starts with it, with a note to say so.

#include <functional>
#include <string>

namespace vcapp {

struct UpdateUi {
    std::function<void(const std::wstring& text, float progress)> status; // the splash's line and bar
    std::function<bool()> cancelled;                                       // the splash closed
};

// The newest published App release (app-v<version>) on GitHub against this folder's VanaCoreApp.dll: a newer one, for
// this exe's API, downloaded, checked (its SHA-256 as app.json gives it, signed as this exe is) and put in its place,
// the one it replaces kept as VanaCoreApp.dll.old. What to tell the player: "" when it's up to date or updated.
std::wstring update_dll(const std::wstring& dir, const UpdateUi& ui);

// VanaCoreApp.dll.old back in VanaCoreApp.dll's place (a new DLL that wouldn't load): true when there was one
bool restore_dll(const std::wstring& dir);

} // namespace vcapp
