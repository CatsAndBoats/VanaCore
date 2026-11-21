#pragma once

#include <string>
#include <vector>

// Rings of light on the ground under things in the world, an addon's marks (vana.world.marks), each a circle or a square,
// with a column of light rising from it when it has a height (a waymark: where to stand): kept per addon and handed
// together to the graphics layer (vanacore_gfx_set_ground_marks), which lays them onto the ground the scene's depth shows,
// so a slope carries one and what stands in front of it (a character, the monster it's under) hides it. Drawn by the scene
// effects on Direct3D 12 and 11; on the game's own Direct3D 8 nothing is drawn. Only used from the game's thread.
namespace vanacore::groundmarks {

struct Mark {
    float x = 0, y = 0, z = 0;  // its centre on the ground, as the game has positions (z the height)
    float radius = 1;           // world units
    float r = 1, g = 1, b = 1;  // its colour, 0 to 1
    float strength = 1;         // how strongly it's drawn (0: not at all)
    float shape = 0;            // 0 a circle, 1 a square of the same half-width
    float height = 0;           // the column of light rising from it, world units (0: none)
    float glyph = 0;            // the sign over the column: 0 none, 1 to 8 for A, B, C, D, 1, 2, 3, 4
    float glyph_size = 0;       // half the sign's height, world units
    float glyph_height = 0;     // how high over the ring the sign's centre floats, world units (0: just over the column's top)
};

// The whole set of `owner`'s marks (an addon's name), replacing what it had; empty takes them all off. The first 16 of every
// addon's together are what's drawn.
void set(const std::string& owner, const std::vector<Mark>& marks);
// `owner`'s marks gone (the addon unloaded)
void release(const std::string& owner);

} // namespace vanacore::groundmarks
