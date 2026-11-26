#pragma once

#include <functional>
#include <string>
#include <vector>

// Screenshots of the game as PNG files in <VanaCore>\screenshots\<date>\, named like
// Vekien_2026.09.25_213012.png. Print Screen takes one (a key bind, binds.cpp), as
// does //screenshot.
namespace vanacore::screenshot {

// Screenshots go under `install_dir`. [screenshots] overlays= in `ini` says whether Print Screen's
// include VanaCore's own drawing (the label, the FPS counter, addons' text): 0 (the default) leaves
// it out.
void install(const wchar_t* install_dir, const wchar_t* ini);

// Takes one on the next frame: `overlays` -1 as [screenshots] overlays= says, 0 without VanaCore's
// drawing, 1 with it.
void request(int overlays = -1);

// Opens the screenshots folder in Explorer (/screenshot open).
void open_folder();

// The next frame's picture for code instead of a file (the MCP server's screenshot tool): `done` is called
// on the game's thread with 24-bit BGR rows, top row first, `stride` bytes apart, or false and why. `ui` false: the 3D scene
// alone, as the frame shows it (effects, upscaler and colour grade in it) before the game draws its interface - no chat log,
// menus, windows or names over heads, and none of VanaCore's drawing whatever `overlays` says (renderer=d3d12 or d3d11; a
// frame or two later than the screen's, and false when no scene is drawn within 3 seconds).
using Captured = std::function<void(bool ok, unsigned width, unsigned height, unsigned stride, std::vector<unsigned char> bgr,
                                    std::string error)>;
void capture(bool overlays, Captured done, bool ui = true);

// vanacore.ini changed: [screenshots] overlays again.
void reload(const wchar_t* ini);

// From Present: before VanaCore draws (shots without its drawing), and after (shots with it).
void before_overlay(void* device);
void after_overlay(void* device);

// Once per frame on the game thread: says in the chat log where finished screenshots went.
void on_frame();

} // namespace vanacore::screenshot
