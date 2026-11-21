#pragma once

#include "occlusion.hpp"

// [graphics] drawdistance, drawdistance.mobs and fog: how far the game draws the zone and the
// characters in it, and where its fog lies (what the VanaView and drawdistance plugins did for Ashita).

namespace vanacore::distance {

enum class Fog {
    Distance, // the game's fog, pushed out with a farther draw distance as the game does it (the default)
    Game,     // the fog where the game's own draw distance puts it, while ours draws farther
    Custom,   // fog.start to fog.end, everywhere
};

struct Settings {
    float scenery = 0.0f; // drawdistance: the zone's draw distance multiplier; 0 the game's own
    float mobs = 0.0f;    // drawdistance.mobs: the characters' (players, NPCs, monsters); 0 the game's own
    bool target_drawn = false; // drawdistance.target=drawn: characters targeted as far as they're drawn, not the game's own
    bool full = false;    // drawdistance.full: with scenery above 1, the zone drawn past the game's view lists too
    bool fill = false;    // drawdistance.fill: with scenery above 1, the lists and the big pieces they leave out
    bool large_off = true; // drawdistance.large_off: Al'Taieu (no view lists; distance.cpp kLargeZones) drawn at the game's
                           // own scenery distance, its fog left as drawdistance puts it
    Fog fog = Fog::Distance;
    float fog_start = 0.0f; // fog=custom: where the fog starts and where it's whole
    float fog_end = 0.0f;
    occlusion::Settings occl; // occl, occl.near, occl.frames, occl.margin: pieces hidden behind what's drawn left out
};

// On the game's thread, each frame: finds the game's draw distance and fog the first time they're
// wanted (once FFXiMain.dll is there), and keeps them as `settings` says.
void apply(const Settings& settings);

} // namespace vanacore::distance
