#pragma once

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

} // namespace vanacore::water
