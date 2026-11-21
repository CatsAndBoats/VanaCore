#pragma once

#include <cstdint>

// Zone lines shown ([graphics] zonelines): the boxes the zone's model DAT marks as lines to other zones (its
// RID section's 'z' entries with a destination), handed to the effects (Direct3D 12 and 11), which draw each
// as a glowing curtain. Read again at each zone change. But for the curtain, each is drawn on the face of its
// box the player is on, where the game zones them, not its middle.
namespace vanacore::zonelines {

struct Settings {
    bool on = true;
    std::uint32_t colour = 0x60C0FF; // 0xRRGGBB
    float strength = 0.6f;           // 0 to 1
    unsigned style = 0;              // 0 fill (a curtain), 1 line (floating across), 2 dots (that line dotted), 3 stars, 4 floor (dots on the ground inside the box)
    bool glow = true;                // a halo round them
    float height = 0.5f;             // the line's and dots' height above each zone line's middle
    float size = 1.0f;               // lines', dots' and stars' size, times their own
    float fade = 1.0f;               // how far their ends fade out, toward the walls or the zone line's ends
};

// Each frame, on the game's thread, while VanaCore's renderer draws the game (Direct3D 12 or 11).
void apply(const Settings& settings);

} // namespace vanacore::zonelines
