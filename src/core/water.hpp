#pragma once

#include <string>

// The game's water models, found as the game loads them (from XI on Anything, rubymatrix/xi-on-anything, host/host64.c): a
// map model under one of a zone DAT's water directories (mizu, umi1, sea, taki, kawa ...) is water, rivers and waterfalls
// too. The vertex buffers made while one loads are flagged in VanaCore's Direct3D 8 (vanacore_gfx_water_loading), for its
// water ([graphics] water, water.detect) and its debug view ([graphics.effects] debug=11). Each zone's count is logged
// ("Water: zone ...").
namespace vanacore::water {

// Each frame with VanaCore's Direct3D 8 (graphics::on_frame): the first time, hooks the game's map model loading; then, once
// the loading has gone quiet for two seconds, logs how many of the models and vertex buffers were water (once a zone, and
// again in it only for more water).
void on_frame();

// [graphics] water.log=1: every map model the game loads logged with the directories above it, water or not (to find the
// names of more water directories).
void set_log(bool on);

// Whether anything wants the water models known ([graphics] water.detect, the debug view, water.log): the game's code is
// patched only then (from the first frame after), so a player who uses none of them runs the game's own loading.
void set_wanted(bool on);

// The zones VanaCore's water is never drawn in, the game's own left there (vanacore_gfx_set_water_zones_off), from
// addons\vanacore\vana\data\water_zones.json under `dir` (VanaCore.exe's folder), VanaCore's own, not a setting:
// { "zones": { "72": { "name": "Alzadaal Undersea Ruins", "water": false }, ... } }. Read with the water's settings, again
// only once the file has changed; without it, water in every zone.
void read_zones(const std::wstring& dir);

} // namespace vanacore::water
