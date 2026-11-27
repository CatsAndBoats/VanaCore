/* gfx_d3d12.c's characters' mask: the stencil bit their draws write (CHAR_STENCIL), turned into an R8 mask for the effects
 * (cmask_usable, cmask_pass). An include unit of gfx_d3d12.c, moved out for its length as gfx_d3d12_sky.h was: it uses that
 * file's statics and is no file of its own. */
#pragma once

/* the rain's reflection on the characters (GfxScene.rain_sheen, the frame before's: the characters' draws, which write the bit,
 * come before this frame's is known), fx_ssr's u.rays.w */
static float g_fx_rain;

#ifdef VANACORE
/* Whether fx_ssr takes the characters' mask as its input 10: while the rain's reflection is on them (rain: u.rays[3]), and with the
 * ray-traced reflections (refl_rt 2), whose tree has no one in it: a reflection's hit on a character is the screen's at any height
 * (gfx_d3d12_rt_hlsl.h hchr; by its height above the floor alone, everyone's reflection was cut off at the knees) */
static int cmask_in_ssr(float rain)
{
    return rain > 0.0f || (g_fxs.refl > 0.0f && g_fxs.refl_rt >= 2.0f);
}

/* Whether the characters are told from the world by the stencil (CHAR_STENCIL, written by their draws): something asks for it
 * (the rain's wet sheen on them, the occlusion's share on characters, ao_chars, its smoothing on them, ao_soft_chars, an aura's glow, the bounce light,
 * which is left off them, the sun's shadows, whose casters within half a unit a character leaves out (fx_ao g_minc), or Render
 * visual 19), the depth has a stencil the game doesn't use, and the mask's pipeline is there.
 * Else the share is on the characters' depth drawn again (sun_map_view), as it was, and the smoothing is left off (the
 * bounce light falls on characters then, as before). */
static int cmask_usable(const GfxTex* ds)
{
    return ds && ds->has_stencil && !g_game_stencil &&
           ((g_fxs.ao > 0.0f && (g_fxs.ao_chars != 1.0f || g_fxs.ao_soft_chars > 0.0f)) || g_fxs.gi > 0.0f || g_aura_on ||
            g_fxs.sun > 0.0f || (int)g_fxs.debug == 19 || g_fx_rain > 0.0f || cmask_in_ssr(0.0f)) &&
           InterlockedCompareExchange(&g_fx.state, 2, 2) == 2 && g_fx.pso[FX_CMASK];
}

/* The characters' mask: into `to` (R8, the scene's size) white where the scene's stencil has the characters' bit, which their
 * draws wrote (pipeline, d3d8.c is_character). The depth-stencil is bound read-only and kept readable as a texture for the passes
 * after. Cleared first, so a frame with no character leaves none. */
static void cmask_pass(GfxTex* depth, FxTex* to)
{
    static int32_t dsv_slot = -1;
    static ID3D12Device* dsv_dev;
    if (dsv_slot < 0 || dsv_dev != g_dev)
        dsv_slot = heap_alloc(&g_dsv), dsv_dev = g_dev;
    if (dsv_slot < 0)
        return;
    D3D12_DEPTH_STENCIL_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.Format = depth->dxfmt;
    v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    v.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = heap_cpu(&g_dsv, dsv_slot);
    ID3D12Device_CreateDepthStencilView(g_dev, depth->res, &v, dsv);
    fx_state(to, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float none[4] = { 0, 0, 0, 0 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list(), heap_cpu(&g_rtv, to->rtv), none, 0, NULL);
    tex_state(depth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    float vp[4] = { 0, 0, (float)to->w, (float)to->h };
    g_fx_dsv = &dsv, g_fx_sref = CHAR_STENCIL;
    fx_draw(heap_cpu(&g_rtv, to->rtv), to->w, to->h, g_fx.pso[FX_CMASK], vp, NULL, NULL, 0, 0, 0);
    g_fx_dsv = NULL;
    fx_state(to, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
#endif
