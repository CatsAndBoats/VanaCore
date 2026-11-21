#pragma once

// [client] framepacing, framepacing.ui, framepacing.camera and autorun.walk: at 60 FPS the game counts each frame as a whole number of
// 60ths of a second (1 from 60 FPS down to 30), so when the frame rate drops what it times by frames runs slow. These
// keep it to the clock: the characters' motion together with the actions' timing (the schedulers weapon skills, spells
// and effects play by), the interface's and the chat's updates, and the camera's steps against a wall. What the FrameFix
// plugin did (rockerudon, MIT: https://github.com/rockerudon/FrameFix), found and patched by VanaCore itself, and the
// actions besides; FrameFix is held off while any is on. All three are on unless vanacore.ini turns them off, and
// follow it while the game runs.

namespace vanacore::framepacing {

// `ini` (vanacore.ini): read now, and again whenever it changes.
void install(const wchar_t* ini);

// Once a frame on the game's thread (Present): the frame's time, and the game's code patched in or out as the settings say
// (once FFXiMain.dll is there).
void on_frame();

} // namespace vanacore::framepacing
