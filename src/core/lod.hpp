#pragma once

// [graphics] lod=high|range|game: how the game picks a zone object's mesh by its distance (graphics.cpp reads the setting, lod.cpp
// patches the game's picks).
namespace vanacore::lod {

// lod=high: every object's high-detail mesh at any distance; lod=range (range > 0): each keeps its detail `range` times farther;
// neither: the game's own choice. Takes effect at the next apply().
void set(bool high, float range);

// Each frame, on the game's thread: the game's sites found the first time they're wanted (once FFXiMain.dll is there) and
// changed when the setting changed.
void apply();

} // namespace vanacore::lod
