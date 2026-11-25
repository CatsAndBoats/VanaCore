/* gfx_d3d12_floors.h: an include unit of gfx_d3d12.c (in its scope: g_fx, g_fxs, g_dev, g_serial, fx_draw). The glow laid again
 * over the grass (gfx_glow_over_grass), the zone's floors for the reflections (gfx_scene_floors) and the rain's wet-ground mask
 * they read too (gfx_scene_rain_mask, rain_reflects) */
#pragma once

/* gfx.h: VanaCore's glow round avatars and trusts, laid partway through the world as the game lays its own (gfx_glow), again over the
 * grass drawn over it once the world was done: the frame's glows (glow_kept) where the grass covers the scene, its depth not the depth
 * before it (gfx_scene_world_depth). The tufts in front of Carbuncle ate its glow (2026-11-04) */
void gfx_glow_over_grass(GfxTex* color, GfxTex* depth)
{
    BATCH_SETTLE();
    if (!g_dev || !color || !depth || g_fx.glow_kept_serial != g_serial || g_fx.wdepth_glow != depth || g_fx.wdepth_serial != g_serial ||
        !g_fx.wdepth || g_fx.wdepth->srv < 0 || depth->srv < 0 || color->type != GFX_TEX_2D || !fx_ready())
        return;
    const int fmt = fx_by_fmt(color->dxfmt);
    if (fmt < 0 || !g_fx.by_fmt[fmt].glowg)
        return;
    const int part_was = gpu_part_cur();
    gpu_part(GP_GLOW);
    tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(g_fx.wdepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    static FxU none; /* (it reads no uniforms) */
    const int32_t in[3] = { g_fx.glow_kept.srv, depth->srv, g_fx.wdepth->srv };
    const float vp[4] = { 0, 0, (float)color->w, (float)color->h };
    fx_draw(target_view(color, 0, 0), color->w, color->h, g_fx.by_fmt[fmt].glowg, vp, &none, in, 3, 0, 0);
    gpu_part(part_was);
}

/* gfx.h: the zone's floors drawn this frame as a layer (floors.c), for the reflections to keep to kinds of ground (refl_floors) */
void gfx_scene_floors(GfxTex* id, GfxTex* depth, int state)
{
    g_fx.floors_id = id, g_fx.floors_depth = depth, g_fx.floors_state = state;
    g_fx.floors_serial = g_serial;
}

void gfx_scene_rain_mask(GfxTex* mask, float wet, float reflect, float puddles)
{
    g_fx.rain_mask = mask, g_fx.rain_wet = mask ? wet : 0.0f, g_fx.rain_reflect = reflect, g_fx.rain_puddles = puddles;
    g_fx.rain_serial = g_serial;
}

int gfx_scene_rain_mask_wanted(void)
{
    return g_dev && g_fxs.fx != 0.0f && g_fxs.refl > 0.0f;
}

/* the rain's wet ground reflecting this frame (or the last: the floors are asked for before the rain pass runs): only with the
 * reflections on, whose wet patches it is (fx_ssr); off, the wet ground reflects nothing */
static int rain_reflects(void)
{
    return g_fxs.refl > 0.0f && g_fx.rain_mask && g_fx.rain_wet > 0.0f && g_fx.rain_reflect > 0.0f && g_serial - g_fx.rain_serial <= 1;
}

int gfx_scene_floors_wanted(void)
{
    if (!g_dev || g_fxs.fx == 0.0f || !(g_fxs.refl > 0.0f))
        return 0;
    /* bit 1: the floors' layer (refl_floors); bit 2: the zone's collision for the ray-traced reflections (refl_rt); bit 4: the zone's meshes for them (refl_rt 2: zonemesh.c) */
    return (g_fxs.refl_floors >= 1.0f ? 1 : 0) | (g_fxs.refl_rt > 0.0f ? 2 : 0) | (g_fxs.refl_rt >= 2.0f ? 4 : 0);
}
