/* The sun the shadows fall from (d3d8_sun.c): picked from the directional lights the world's lit draws use. */
#pragma once
#include <stdint.h>

#include "gfx.h"

/* A lit draw's vote for the directional light that points from w (world, unit) toward the light, with its diffuse colour; `draw` is
 * the draw's number in the scene. A light's first vote counts for more (weight 1; its others 0.5). */
void d3d8_sun_vote(const float w[3], const float color[4], float weight, uint32_t draw);
/* Once a frame (`frame`), before the world's effects: the votes counted, the sun picked, into the scene (its camera's view): its
 * direction, colour and first draw, the GfxScene.indoors, warping and vana_minute. Returns how many lights were voted for this frame. */
uint32_t d3d8_sun_pick(GfxScene* sc, uint32_t frame);
/* The light the last d3d8_sun_pick put into the scene (the sun, or the moon by night): its world direction toward it (unit) and its
 * diffuse colour, held while the pick has none. 0 before the first pick (VanaCore's water: d3d8_water.h water_shade) */
int d3d8_sun_world(float dir[3], float color[4]);
/* How far into the Vana'diel night it is, outdoors, 0 by day to 1 ([graphics.effects] night: d3d8.c night_light), as the last
 * d3d8_sun_pick eased it */
float d3d8_night(void);
/* Indoors, 0 to 1 ([graphics.effects] indoor_bright: d3d8_night.h night_light): a Mog House, a zone lit by a fixed light alone (its
 * DAT, or its sun not moving with the clock for 10 seconds), as the last d3d8_sun_pick eased it */
float d3d8_inside(void);
/* The world's lit draws' light raised by night_bright, night_bright_ambient (through the night), day_bright (through the sun's
 * hours) and indoor_bright (indoors): kd the directional lights' factor, ka the ambient's, 1 each for none (d3d8_night.h
 * night_light; GfxScene.brighten for sun_direct) */
void d3d8_brighten(float* kd, float* ka);
