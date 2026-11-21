/* VanaCore's floors layer (floors.c): the zone's collision floors drawn each frame as a layer of their own, the kind of
 * ground (as the game's footsteps know it) under each pixel of the scene, for the reflections to keep to stone and metal
 * ([graphics.effects] refl_floors). */
#pragma once

#include "gfx.h"
#include "grass.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The zone the player is in, each frame (0 while there's none): a zone's floors are let go as it changes, and the reflections
 * wait for the new zone's. */
void floors_set_zone(int zone);
/* The graphics device lost and made again (d3d8_device_lost): the layer's targets and the floors' buffer went with it. The
 * targets are made again at the next scene; the zone's floors wait to be handed over again by whoever gave them (VanaCore's
 * grass_floors::forget), as for a zone not seen yet. */
void floors_device_lost(void);
/* The zone's floors, the same hand-over as the grass's (grass_set_floors, from VanaCore's grass_floors.cpp): copied, laid in
 * the world once (each placement's triangles) and kept on the GPU. zone 0, or none: let go. */
void floors_set(const GrassFloors* f);
/* /vana debug's collision view ([graphics] collision.show): the zone's whole collision (vanacore_gfx_set_collision, its walls
 * marked 0x10 on their terrain), laid in the world once and kept on the GPU, drawn over the scene each frame by floors_scene
 * while held. zone 0, or none: let go. */
void floors_set_collision(const GrassFloors* f);
/* 1 while the collision view is wanted ([graphics] collision.show): the collision is also handed over for the rain's shelter, and
 * isn't drawn then */
void floors_show_collision(int on);
/* The frame's 3D scene is finished in color, drawn with depth (before grass_scene and gfx_scene_done): what the back end wants
 * of the layer (gfx_scene_floors_wanted) is drawn with the scene's camera into a target and depths of its own, the viewport's
 * size, and handed to it (gfx_scene_floors); the scene's targets are set back after. Then the collision view, while held, into
 * color itself, against depth. */
void floors_scene(GfxTex* color, GfxTex* depth, const GfxScene* s);

#ifdef __cplusplus
}
#endif
