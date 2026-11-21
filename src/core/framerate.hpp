#pragma once

namespace vanacore::framerate {

// Reads [client] fps.max, the frame rate the game renders at (1-60; missing is 60, 0 is divisor 0,
// no cap). FFXI caps itself at 60 / divisor, its own setting (2 by default). VanaCore picks
// the divisor whose cap is the lowest at or above fps.max, and limits the rest of the way itself:
// 60 is divisor 1; 45 is divisor 1 plus VanaCore's limit of 45; 30 is the game's default. The game's own wait for its
// cap (Sleep(1) on a millisecond clock) is skipped and VanaCore's timer holds the rate instead; the divisor stays, as
// the least 60ths the game counts a frame as. At divisor 0
// neither the game nor VanaCore caps it (vsync, fps.background and frame generation's 240 still do), and
// above 60 FPS the game runs fast: its tick counts each frame as at least a 60th of a second.
// [client] fps.background (1-60; 0, the default, is off): the most frames per second while the game is in the background
// (window::in_background), held by VanaCore's own limit.
// With frame generation, at most 240 frames are shown a second; the game's own frame rate is held to it over the frames
// shown for each of the game's (graphics::shown_per_frame), by VanaCore's limit: 60 at 4x.
// [client] fps.cutscene (1-60, or 0 for no cap; unset is off): the frame rate while the player is in an event, a
// cutscene (the local player's status 4 as the server sent it or the client has it, polled ten times a second), in
// place of fps.max. The game's animations
// run fast above 60 FPS, so 0 plays a cutscene faster.
// [graphics] vsync.cutscene (0 or 1; unset is vsync's): vsync while the player is in that event, on Direct3D 12 and 11
// (each Present takes it). 0 with fps.cutscene=0: cutscenes unlocked, the rest of the game held to the screen.
// [client] timer.resolution=1 asks Windows for a 1 ms timer for the game's process (a Sleep or
// timeGetTime otherwise moves in 15.6 ms steps).
void install(const wchar_t* ini);

// vanacore.ini changed (on the game's render thread): fps.max, fps.cutscene, fps.background and vsync.cutscene again,
// from the next frame.
void reload(const wchar_t* ini);

// The game's frame rate divisor now (1: 60 FPS; 0: no cap), once VanaCore has its setting; else the game's own 2.
int divisor();

// Called on the game's render thread just before each Present: takes over the game's divisor
// setting on the first frame (its code is decrypted by then) and waits for VanaCore's limit.
void before_present();

// After before_present: when VanaCore's limit held this frame, its place in the limit's cadence (a QueryPerformanceCounter
// time), the same for a frame that came late and was let straight through to catch up; 0 when nothing held it. The FPS
// counter times frames by it, so a late frame and the one after it don't make a second read 61 or 62.
long long frame_slot();

// [graphics] vsync `on` as it stands now: [graphics] vsync.cutscene's in place of it while the player is in an event (for
// what graphics.cpp gives Direct3D 12 and 11).
bool vsync(bool on);

} // namespace vanacore::framerate
