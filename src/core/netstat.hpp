#pragma once

// [client] network.*: the Network window at the top right while the main menu is open, which the game fills with
// flashing arrows, its connection quality ("100%") and S over R (bytes a second sent and received). network.icon: arrows
// (the game's), dot (one dot coloured by the quality, drawn by VanaCore) or none. network.layout: game (the game's
// places) or line (one line in the middle of the window, network.text with {s}, {r} and {p} filled in).
// network.percent=0, .sent=0 and .received=0 leave the quality, S and R out; network.simple=1 shows Connected, Failing
// (the server quiet 2 s) or Disconnected (R 0) instead; network.x, .y move the numbers. network.hide=1 draws nothing
// while no menu is up (hideui::window_up): the game keeps the contents on screen once the window's frame has gone.
namespace vanacore::netstat {

// `ini` (vanacore.ini): [client] network.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's drawing of the window the first time a network.* key differs
// from the game's look (once FFXiMain.dll is unpacked).
void on_frame();

// network.panel=0: the window isn't shown at all, its frame (helpbar.cpp leaves it out of the window draw) or its contents.
bool panel_hidden();
// network.background=0: the window's frame and its "Network" caption are left out, its contents stay. And how many times the
// game has drawn the contents (the hook counts them), for helpbar.cpp to tell whether they're drawn inside the frame's draw.
bool background_hidden();
unsigned contents_draws();

// In VanaCore's own interface (between text::begin and text::end, in the window's `width` x `height` pixels): the dot,
// if the window was drawn this frame with network.icon=dot.
void draw(void* device, unsigned width, unsigned height);

} // namespace vanacore::netstat
