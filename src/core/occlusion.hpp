#pragma once

#include <cstddef>
#include <cstdint>

// [graphics] occl: the zone's pieces hidden behind what's drawn left undrawn. Each frame the renderer tests every piece's box
// against the depth of the scene just drawn (Direct3D 12, gfx_d3d12_occl.h); the core leaves out a piece far enough off that came
// back hidden in a few tests in a row. For a zone the game draws from its quadtree (The Sanctuary of Zi'Tah: no view lists), at a
// draw distance over the game's own: twin_hidden (distance.cpp) asks it of each piece.

namespace vanacore::occlusion {

struct Settings {
    int mode = 0;          // occl: 0 off; 1 pieces tested hidden left undrawn; 2 tested and logged only (nothing left out)
    float nearest = 80.0f; // occl.near: a piece's box nearer the camera than this (world units) is always drawn
    int frames = 2;        // occl.frames: tests in a row a piece must come back hidden in before it's left out
    float margin = 10.0f;  // occl.margin: round each box on the screen, thousandths of the scene's width (the camera's moves)
};

// On the game's thread, each frame (distance::apply): the renderer told; a line in the log every 10 s while it's on
void apply(const Settings& settings);

// From twin_hidden for a piece of the zone's first slot: `i` its index among `count` pieces at `chunks`. True leaves it undrawn.
bool hidden(std::uintptr_t renderer, std::uintptr_t chunks, int count, std::size_t i);

} // namespace vanacore::occlusion
