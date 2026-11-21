#pragma once

#include <windows.h>

#include <mmreg.h>
#include <mmsystem.h>

#include <dsound.h>

// DirectSound that follows Windows' default output device. The game makes one DirectSound device on the default output
// when it starts and keeps it; when the output changes (headphones to speakers, a headset that unplugs or whose driver
// resets) the device it holds goes silent for good, until the game is started again. This stands between the game and
// DirectSound: the game is handed wrappers for its device and sound buffers, and when Windows' default output changes
// (or the output in use is removed, added or its state changes) a new device is made on the new default output and every
// buffer is made again on it, with its contents, volume, pan, pitch, play position and playing state, so the game never
// knows. The buffers' contents are kept as the game writes them (a copy at each Unlock), since a buffer on a dead device
// can't be read back. Only the default output is followed: a device the game names stays unwrapped.
//
// This file and dsound_follow.cpp need only the Windows SDK (the test in tools\soundtest builds them alone).
namespace vanacore::dsfollow {

using CreateFunction = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);

// Gets the text of each log line (UTF-16, without a line end).
using LogFunction = void (*)(const wchar_t* text);

// `create` is the real DirectSoundCreate8 (what the game's import held before it was hooked). Once, before the game makes
// its device.
void set_functions(CreateFunction create, LogFunction log);

// In place of DirectSoundCreate8. Starts the device watcher on the first call; if that can't be (no Windows audio device
// API), the real device is handed back unwrapped.
HRESULT WINAPI create(LPCGUID device, LPDIRECTSOUND8* out, LPUNKNOWN outer);

// Moves every wrapped device to the default output now, as a device change does (for tests). False if it couldn't.
bool rebuild_now();

// How many times devices have been moved, and how many buffers the last move carried (for tests and the log).
int rebuilds();
int last_moved();

} // namespace vanacore::dsfollow
