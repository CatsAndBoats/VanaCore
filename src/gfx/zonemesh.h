/* VanaCore's own copy of the zone's meshes for the sun's shadows (zonemesh.c, [graphics.effects] sun_zone_mesh): read from the
 * zone's model DAT by VanaCore (core/zonemesh.cpp), laid in the world once, kept on the GPU while the player is in the zone, and
 * given to the sun's maps as the caster pass's pieces are - in place of that pass, which has the game draw its pieces and keeps
 * what it drew. */
#pragma once

#include "gfx.h"
#include "vanacore_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* [graphics.effects] sun_zone_mesh: 1 (the default) the zone's shadows from its own meshes while they're held, 0 from the caster
 * pass as before (gfx_fx_set, gfx_dispatch.c) */
extern float g_gfx_zone_mesh;

/* The zone the player is in, each frame: another zone's meshes are let go. */
void zonemesh_set_zone(int zone);
/* A zone's meshes (core/zone_casters.cpp): copied to the GPU. zone 0, or none: let go. */
void zonemesh_set(const VanaZoneMesh* m);
/* The graphics device lost: the buffers and textures went with it; the zone's are handed over again. */
void zonemesh_device_lost(void);
/* 1 while the zone's own meshes are what the sun's maps take for it (held, sun_zone_mesh on): the caster pass is off then
 * (gfx_sun_cache.h gfx_seek_reach) */
int zonemesh_resident(void);
/* The held meshes' curved surfaces (rock and ground: the solid triangles curved at all three corners), for the back end's own
 * pass over them (gfx_d3d11_smooth.h zn_draw): their positions (x, y, z a vertex, triangle lists, in the world), beside them
 * each vertex's normal and curve (VanaZoneMesh.nrm: 4 floats), and how many vertices. 0 when none are held, or the zone has
 * none (a town) */
int zonemesh_normals(GfxBuf** pos, GfxBuf** nrm, uint32_t* n);
/* Each world scene, before the effects: a share of the meshes' parts handed to the sun's maps (as the caster pass's pieces,
 * each again every few frames, so the cache keeps them all) */
void zonemesh_scene(const GfxScene* s);

#ifdef __cplusplus
}
#endif
