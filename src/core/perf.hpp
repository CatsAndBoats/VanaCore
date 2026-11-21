#pragma once

#include <string>
#include <vector>

// /vana graph: a window of the game's performance over the last minute, a point every 500 ms (the average of five
// samples, one every 100 ms), with the metric to show flipped between a list: FPS, frame time, the worst frame in each
// 500 ms, the game's CPU use and memory, its address space used and the largest block of it left free. The FPS line is
// red where it's under 30. (Where each frame went and DirectX 12's GPU time, draws and uploads were here too, behind a
// Detailed timing switch that cost FPS: /vana stats has them.) Drawn with ImGui, so it moves and resizes like an
// addon's window, and Save CSV writes what it holds to logs\graph-<date>-<time>.csv.
//
//   /vana graph            opens it, or closes it
//   /vana graph on|off
//   /vana graph save      the CSV, without the window's button
//
// The history is kept all the time (a clock read a frame, and the process's CPU time and memory read every 100 ms), so
// it's there when the window opens. Nothing here depends on [logging].
namespace vanacore::perf {

// From VanaCoreInstall: VanaCore's folder (the CSV goes in its logs folder).
void install(const wchar_t* install_dir);

// Each frame, from the Present hook, on the render thread: counts the frame; every 100 ms closes a sample, and every fifth sample a point.
void on_frame();

// The game's address space, which is what a 32-bit game runs out of (4 GB on 64-bit Windows, with the large-address
// flag): what's in use (reserved or committed: its heaps, DLLs and mapped files), all of it, and the largest block left
// free, which is the biggest allocation that can still succeed. MB. The largest free block comes from a walk of the
// address space every 2 s, on a thread of its own from install (0 until its first walk).
struct AddressSpace {
    double used_mb = 0.0;
    double total_mb = 0.0;
    double largest_free_mb = 0.0;
};
AddressSpace address_space();

// Each frame, between gui::begin_frame and gui::end_frame: the window, while it's open.
void draw();

// "graph ..." (its words after /vana): true, it's handled.
bool command(const std::vector<std::string>& parts);

} // namespace vanacore::perf
