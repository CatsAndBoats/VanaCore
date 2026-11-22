/* gfx_d3d12.c's copies of the scene for VanaCore's passes: its depth before the grass (gfx_scene_world_depth: the occlusion,
 * shadows and lines without it, and the grass's own lines) and its colour and depth for the water pass (gfx_scene_snapshot). An
 * include unit of gfx_d3d12.c, moved out for its length as gfx_d3d12_cmask.h was: it uses that file's statics and is no file of
 * its own. */
#pragma once

/* gfx.h: the scene's depth before the grass, copied whole (Direct3D copies depth no other way) for its occlusion, contact shadows,
 * sun's shadows on the tufts (fx_ao: else shaded as ground by their blades' angle, grey), lines without the grass and reflections */
void gfx_scene_world_depth(GfxTex* depth)
{
    BATCH_SETTLE();
    g_fx.wdepth_of = g_fx.wdepth_glow = NULL;
    int lines = 0;
#ifdef VANACORE
    lines = (g_fxs.outline > 0.0f && g_fxs.outline_nograss != 0.0f) || (int)g_fxs.debug == 26; /* ... and Render visual 26's grass */
#endif
    /* ... and VanaCore's glow laid again over the grass (gfx_glow_over_grass), which alone leaves the others as they were */
    const int others = (g_fxs.fx != 0.0f && (g_fxs.ao > 0.0f || g_fxs.refl > 0.0f || g_fxs.sun > 0.0f || lines)) || mask_wanted();
    if (!g_dev || !(others || g_fx.glow_kept_serial == g_serial) || !depth || depth->use != GFX_USE_DEPTH || depth->type != GFX_TEX_2D ||
        depth->srv < 0)
        return;
    if (!g_fx.wdepth || g_fx.wdepth->w != depth->w || g_fx.wdepth->h != depth->h || g_fx.wdepth->dxfmt != depth->dxfmt)
    {
        gfx_tex_destroy(g_fx.wdepth);
        g_fx.wdepth = gfx_tex_create(GFX_TEX_2D, depth->fmt, depth->w, depth->h, 1, GFX_USE_DEPTH);
    }
    if (!g_fx.wdepth || g_fx.wdepth->srv < 0)
        return;
    gfx_copy(depth, 0, 0, 0, 0, depth->w, depth->h, g_fx.wdepth, 0, 0, 0, 0);
    g_fx.wdepth_of = others ? depth : NULL, g_fx.wdepth_glow = depth, g_fx.wdepth_serial = g_serial;
}

/* gfx.h: the scene's colour and depth copied for VanaCore's water pass (d3d8_water.h water_pass). The depth is the one the target
 * was last drawn with (depth_seen: the scratch one of its size when the game's is larger), so the copy is what its world was
 * drawn against; the colour copy is a plain sampled texture of the target's format (gfx_copy wants the formats the same) */
static GfxTex *g_snap_color, *g_snap_depth;
static uint32_t g_snap_serial; /* the frame (g_serial) the copies were last taken in: fx_ao's water mask (GfxScene.water_noshadow) */
int gfx_scene_snapshot(GfxTex* color, GfxTex** colour_copy, GfxTex** depth_copy, uint32_t* w, uint32_t* h)
{
    *colour_copy = *depth_copy = NULL, *w = *h = 0;
    if (!g_dev || !color || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return 0;
    GfxTex* depth = color->depth_seen;
    if (!depth || depth->use != GFX_USE_DEPTH || depth->type != GFX_TEX_2D || depth->w != color->w || depth->h != color->h || depth->srv < 0)
        return 0;
    if (!g_snap_color || g_snap_color->w != color->w || g_snap_color->h != color->h || g_snap_color->dxfmt != color->dxfmt)
    {
        gfx_tex_destroy(g_snap_color);
        g_snap_color = gfx_tex_create(GFX_TEX_2D, color->fmt, color->w, color->h, 1, GFX_USE_SAMPLE);
    }
    if (!g_snap_depth || g_snap_depth->w != depth->w || g_snap_depth->h != depth->h || g_snap_depth->dxfmt != depth->dxfmt)
    {
        gfx_tex_destroy(g_snap_depth);
        g_snap_depth = gfx_tex_create(GFX_TEX_2D, depth->fmt, depth->w, depth->h, 1, GFX_USE_DEPTH);
    }
    if (!g_snap_color || !g_snap_depth || g_snap_color->srv < 0 || g_snap_depth->srv < 0)
        return 0;
    gfx_copy(color, 0, 0, 0, 0, color->w, color->h, g_snap_color, 0, 0, 0, 0);
    gfx_copy(depth, 0, 0, 0, 0, depth->w, depth->h, g_snap_depth, 0, 0, 0, 0);
    *colour_copy = g_snap_color, *depth_copy = g_snap_depth, *w = color->w, *h = color->h;
    g_snap_serial = (uint32_t)g_serial;
    return 1;
}
