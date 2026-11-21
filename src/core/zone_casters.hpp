#pragma once

// The zone's own meshes for the sun's shadows ([graphics.effects] sun_zone_mesh): read from the zone's model DAT on a thread
// (zonemesh::read) and handed to the graphics (vanacore_gfx_set_zone_mesh, gfx/zonemesh.c), which give them to the sun's maps
// in place of casters.cpp's pass.
namespace vanacore::zone_casters {

// Each frame, on the game's thread: the zone the player is in (0 none) and whether the sun's shadows are on. A new zone's
// meshes are read on a thread and handed over once read; not wanted, they're let go.
void update(int zone, bool wanted);

// The graphics device lost and made again: what was handed over went with it. Read and handed over again at the next update.
void forget();

} // namespace vanacore::zone_casters
