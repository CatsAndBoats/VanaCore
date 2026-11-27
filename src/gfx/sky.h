/* VanaCore's sky (sky.c, [graphics] sky): drawn in place of the game's dome. True Sky's first phase (research/true-sky.md): the
 * dome's own colours drawn again by VanaCore, so the atmosphere, clouds and rain can be built on it. Direct3D 12 (gfx_sky). */
#pragma once

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [graphics] sky: enabled; log 1: a line in the log as the sky takes the dome, or lets it be (sky.log) */
typedef struct SkySettings
{
    int enabled;
    int log;
} SkySettings;

void sky_configure(const SkySettings* settings);
/* A weather's sky borrowed from another zone's files (d3d8_weather_look.h): the dome's colours laid over the game's by `k` (0 none
 * to 1 all), from 8 slices' colours (0 to 1) at their heights (0 the horizon, 1 the zenith). While k is over 0 the dome is taken
 * and drawn by VanaCore as [graphics] sky does, whatever that's set to (Direct3D 12; Direct3D 11 keeps the game's dome) */
void sky_set_look(const float slice[8][3], const float elev[8], float k);

/* A 3D draw into the frame's world (d3d8.c draw_packet), before it's drawn: number its place among the world's draws this frame (1
 * the first), round_camera 1 when its view and world matrices move nothing (the camera at their origin). The game's dome is the
 * world's first draws, triangle strips of colour round the camera from the horizon up (phase 0's captures): each one's vertices are
 * taken into the sky's rings and the draw is skipped (1). The first draw after them that isn't one gets 2: draw the sky (sky_flush)
 * just before it, into its target. 0: nothing to do. */
int sky_scene_draw(const GfxDraw* d, uint32_t prim, uint32_t nverts, uint32_t number, int round_camera);
/* The sky drawn into the target bound now (the world's), from the dome's rings taken this frame */
void sky_flush(void);

#ifdef __cplusplus
}
#endif
