/* VanaCore's glow round avatars and trusts in Direct3D 11 (gfx_d3d12.c's): gfx_glow, and gfx_glow_over_grass, which lays the frame's
 * glows again over VanaCore's grass. gfx_d3d11.c includes this after fx_draw, fx_pass and fx_tex, before gfx_scene_world_depth.
 * It is no file of its own. */
#pragma once

/* VanaCore's glow just blurred (glow[1], w x h; its uniforms u) into the frame's layer of them too, premultiplied (glow2.w) over
 * what's there (cleared at the frame's first), for the grass drawn over them once the world is done: the glow is laid partway
 * through it (gfx_glow_over_grass) */
static void glow_keep(uint32_t w, uint32_t h, FxU* u)
{
    if (!g_fx.ps[FX_GLOWG])
        return;
    if (g_fx.glow_kept.w != w || g_fx.glow_kept.h != h)
        g_fx.glow_kept_serial = 0;
    if (!fx_tex(&g_fx.glow_kept, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h))
        return;
    const float none[4] = { 0, 0, 0, 0 }, vp[4] = { 0, 0, (float)w, (float)h };
    if (g_fx.glow_kept_serial != g_serial)
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_fx.glow_kept.rtv, none);
    u->glow2[3] = 1.0f, g_fx_blend = 2;
    fx_draw(g_fx.glow_kept.rtv, g_fx.ps[FX_GLOWC], vp, u, &g_fx.glow[1].srv, 1, 0, 0);
    u->glow2[3] = 0.0f, g_fx_blend = 0;
    g_fx.glow_kept_serial = g_serial;
}

/* VanaCore's glow: gfx_d3d12.c's, the blur's chain made by GenerateMips */
void gfx_glow(GfxTex* src, const float tint[4])
{
    if (!g_dev || !src || !g_rt || src->type != GFX_TEX_2D || !src->srv_mips || src->mips < 3 || !fx_ready() ||
        !g_fx.ps[FX_GLOWB] || !g_fx.ps[FX_GLOWC])
    {
        static int said; /* why the glow is missing, once */
        if (src && src->type == GFX_TEX_2D && (!src->srv_mips || src->mips < 3) && !said)
            said = 1, fprintf(stderr, "[recomp] gfx: the trusts' and avatars' glow (glow=custom) isn't drawn: the picture it's "
                "made from (%ux%u) has no mip chain\n", src->w, src->h);
        return;
    }
    if (!src->scene) /* its chain, from the picture as it is now */
    {
        ID3D11DeviceContext_GenerateMips(g_ctx, src->srv_mips);
        src->scene = g_serial;
    }
    /* glow.size widens the blur's step; past the quality's limit it's halved a level down the chain instead, and higher
     * qualities blur again (gfx_d3d12.c's) */
    static const float STEP_LIMIT[4] = { 2.5f, 1.5f, 1.0f, 0.75f };
    const int q = (int)fminf(fmaxf(g_fxs.glow_quality + 0.5f, 0.0f), 3.0f);
    float step = 1.5f * fminf(fmaxf(g_fxs.glow_size, 0.1f), 4.0f);
    uint32_t lv = 2;
    while (step > STEP_LIMIT[q] && lv < 6 && lv + 1 < (uint32_t)src->mips)
        lv++, step *= 0.5f;
    const uint32_t w = (src->w >> lv) ? (src->w >> lv) : 1, h = (src->h >> lv) ? (src->h >> lv) : 1;
    const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!fx_tex(&g_fx.glow[0], RGBA, w, h) || !fx_tex(&g_fx.glow[1], RGBA, w, h))
        return;
    FxU u;
    memset(&u, 0, sizeof u);
    memcpy(u.glow, tint, sizeof u.glow);
    u.glow[3] *= fmaxf(g_fxs.glow_strength, 0.0f);
    u.glow2[0] = (float)lv, u.glow2[1] = step, u.glow2[2] = 1.0f;
    fx_pass(&g_fx.glow[0], FX_GLOWB, &u, &src->srv_mips, 1, 1, 0);
    u.glow2[0] = 0.0f, u.glow2[2] = 0.0f;
    fx_pass(&g_fx.glow[1], FX_GLOWB, &u, &g_fx.glow[0].srv, 1, 0, 1);
    for (int again = 1; again < (q == 0 ? 1 : q); ++again)
    {
        fx_pass(&g_fx.glow[0], FX_GLOWB, &u, &g_fx.glow[1].srv, 1, 1, 0);
        fx_pass(&g_fx.glow[1], FX_GLOWB, &u, &g_fx.glow[0].srv, 1, 0, 1);
    }
    glow_keep(w, h, &u);
    uint32_t tw, th;
    level_size(g_rt, g_rt_level, &tw, &th);
    float vp[4] = { 0, 0, (float)tw, (float)th };
    g_fx_blend = 1;
    fx_draw((ID3D11RenderTargetView*)target_view(g_rt, g_rt_face, g_rt_level), g_fx.ps[FX_GLOWC], vp, &u, &g_fx.glow[1].srv, 1, 0, 0);
    g_fx_blend = 0;
}

/* gfx.h: the glow again over the grass drawn over it (gfx_d3d12.c's): the frame's glows (glow_kept) where the grass covers the scene,
 * its depth not the depth before it (gfx_scene_world_depth) */
void gfx_glow_over_grass(GfxTex* color, GfxTex* depth)
{
    if (!g_dev || !color || !depth || g_fx.glow_kept_serial != g_serial || g_fx.wdepth_glow != depth || g_fx.wdepth_serial != g_serial ||
        !g_fx.wdepth || !g_fx.wdepth->srv || !depth->srv || !g_fx.glow_kept.srv || color->type != GFX_TEX_2D || !fx_ready() ||
        !g_fx.ps[FX_GLOWG])
        return;
    ID3D11ShaderResourceView* in[3] = { g_fx.glow_kept.srv, depth->srv, g_fx.wdepth->srv };
    const float vp[4] = { 0, 0, (float)color->w, (float)color->h };
    g_fx_blend = 2;
    fx_draw((ID3D11RenderTargetView*)target_view(color, 0, 0), g_fx.ps[FX_GLOWG], vp, NULL, in, 3, 0, 0);
    g_fx_blend = 0;
}
