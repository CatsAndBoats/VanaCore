/* The world mirrored in the water, in Direct3D 11 (gfx.h gfx_scene_mirror; gfx_d3d12_mirror.h has Direct3D 12's): the frame's
 * casters - each opaque draw of the world, as kept for the sun's maps - drawn again with their own colour from the camera
 * mirrored about the water's level, into a smaller target the water's pixel function reads (gfx_hlsl_water.h emit_water_mirror).
 * Each goes through its own functions with the mirror's key (GfxVsKey.shadow GFX_VS_MIRROR): the clip-space position it makes
 * is taken through one matrix (b2, as the sun's maps take it) - the camera's inverse, the mirror about the level, the camera
 * again - whose depth is the height over the level, so what lies under the water is cut off by the near plane and no draw
 * needs a clip plane of its own. The light and the fog on each are the draw's own: what's mirrored is lit as it stands.
 * gfx_d3d11.c includes this after gfx_d3d11_smooth.h. It is no file of its own. */
#pragma once

#define MIRROR_KEEP 4  /* frames after the last call the casters still keep their textures for it */
#define MIRROR_Z 0.25f /* the mirror's depth: this share of the height over the level, over the view's w; under 1 within any view
                        * narrower than 75 degrees off its axis (the height is less than the distance) */
#define MIRROR_SLACK 60.0f /* a caster stands where its first vertex is: this much farther than the reach it may still reach in */

static struct
{
    GfxTex *color, *depth;
    ID3D11BlendState* blend; /* the colour as drawn and the alpha 0: the target is cleared to alpha 1, the sky's place */
    uint64_t asked;          /* the frame after the one gfx_scene_mirror was last called in (0 never) */
    uint32_t drawn, building, dead; /* the last pass's casters: drawn, left out while their functions build, gone */
} g_mir;

/* 1 while the water asks for the mirror: every caster keeps its textures and samplers then (an alpha-tested one's are kept
 * anyway), and they're kept with the sun's shadows off too */
static int mirror_wanted(void) { return g_mir.asked && g_serial < g_mir.asked + MIRROR_KEEP; }

/* the matrix the casters' clip-space positions go through into the mirror (row vectors, as the sun's): to the world (clip_world,
 * handed back for the casters' places), mirrored about y = level, through the scene's view and projection; then the depth made
 * the height over the level (the world's up is -y). 0 when the camera has no inverse */
static int mirror_matrix(float M[16], float clip_world[16], float cam[3], const GfxScene* s, float level)
{
    float invP[16], invV[16], RV[16], Mw[16];
    float R[16] = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    if (!mat_inverse(invP, s->proj) || !mat_inverse(invV, s->view))
        return 0;
    mat_mul(clip_world, invP, invV);
    R[13] = 2.0f * level;
    mat_mul(RV, R, s->view);
    mat_mul(Mw, RV, s->proj);
    Mw[2] = 0.0f, Mw[6] = -MIRROR_Z, Mw[10] = 0.0f, Mw[14] = MIRROR_Z * level;
    mat_mul(M, clip_world, Mw);
    memcpy(cam, invV + 12, 12);
    return 1;
}

static int mirror_targets(uint32_t w, uint32_t h, uint32_t depth_fmt)
{
    if (!g_mir.color || g_mir.color->w != w || g_mir.color->h != h)
    {
        gfx_tex_destroy(g_mir.color);
        g_mir.color = gfx_tex_create(GFX_TEX_2D, 21 /* D3DFMT_A8R8G8B8 */, w, h, 1, GFX_USE_RT);
    }
    if (!g_mir.depth || g_mir.depth->w != w || g_mir.depth->h != h || g_mir.depth->fmt != depth_fmt)
    {
        gfx_tex_destroy(g_mir.depth);
        g_mir.depth = gfx_tex_create(GFX_TEX_2D, depth_fmt, w, h, 1, GFX_USE_DEPTH);
    }
    if (!g_mir.blend)
    {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE, bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bd, &g_mir.blend)))
            g_mir.blend = NULL;
    }
    return g_mir.color && g_mir.depth && g_mir.color->srv && g_mir.blend;
}

/* one caster into the mirror: its own functions with the mirror's key, its uniforms, textures and streams as it was drawn
 * (vn_draw's way); 1 drawn, 0 not (its functions still building, counted; or its bytes gone) */
static int mirror_draw(Caster* cs)
{
    int rv = 0;
    for (int st = 0; st < GFX_NSTREAMS; ++st)
        rv |= cs->ring[st];
    if ((rv && g_vring.c[cs->vchunk].gen != cs->vgen) || (cs->iring && g_iring.c[cs->ichunk].gen != cs->igen))
        return 0;
    LibKey lk = cs->lib;
    lk.vs.shadow = GFX_VS_MIRROR, lk.vs.pixel = 0, lk.vs.motion = 0, lk.vs.cpos = 0;
    lk.fs.nmap = 0, lk.fs.wet = 0, lk.fs.pool = 0, lk.fs.hfog = 0;
    const Shaders* sh = shaders_for(&lk, cs->vs, cs->ps);
    if (!sh)
    {
        g_mir.building += (uint32_t)g_pair_waiting;
        return 0;
    }
    if (cs->ugen != g_cring.gen)
    {
        if (!uniforms_put((const GfxU*)(g_caster_u + cs->cpu_u), cs->need, NULL, NULL, &cs->uoff))
            return 0;
        cs->ugen = g_cring.gen;
    }
    /* the mirror turns each face over: what the draw culled by one winding is culled by the other */
    RasterKey rk = { (uint8_t)(cs->cull == 2 ? 3 : cs->cull == 3 ? 2 : 1), 3, 0, 0, 0 };
    ID3D11RasterizerState* raster = raster_state(&rk);
    if (!raster)
        return 0;
    if (raster != g_bound.raster)
        ID3D11DeviceContext_RSSetState(g_ctx, raster), g_bound.raster = raster;
    if (memcmp(cs->srv, g_bound.tex, sizeof cs->srv))
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 16, cs->srv), memcpy(g_bound.tex, cs->srv, sizeof cs->srv);
    if (memcmp(cs->samp, g_bound.samp, sizeof cs->samp))
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 8, cs->samp), memcpy(g_bound.samp, cs->samp, sizeof cs->samp);
    if (sh->vs != g_bound.vs)
        ID3D11DeviceContext_VSSetShader(g_ctx, sh->vs, NULL, 0), g_bound.vs = sh->vs;
    if (sh->ps != g_bound.ps)
        ID3D11DeviceContext_PSSetShader(g_ctx, sh->ps, NULL, 0), g_bound.ps = sh->ps;
    set_topology((D3D11_PRIMITIVE_TOPOLOGY)cs->topo);
    uniforms_bind(cs->uoff);
    ID3D11ShaderResourceView* sv[GFX_NSTREAMS];
    for (int st = 0; st < GFX_NSTREAMS; ++st)
        sv[st] = cs->buf[st] ? cs->buf[st]->srv : cs->ring[st] ? g_vring.c[cs->vchunk].srv : NULL;
    if (memcmp(sv, g_bound.streams, sizeof sv))
        ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, GFX_NSTREAMS, sv), memcpy(g_bound.streams, sv, sizeof sv);
    if (cs->ifmt != DXGI_FORMAT_UNKNOWN)
    {
        ID3D11Buffer* ib = cs->iring ? g_iring.c[cs->ichunk].buf : cs->buf[GFX_NSTREAMS] ? cs->buf[GFX_NSTREAMS]->res : NULL;
        if (!ib)
            return 0;
        set_index_buffer(ib, cs->ifmt, cs->ioff);
        ID3D11DeviceContext_DrawIndexed(g_ctx, cs->n, 0, 0);
    }
    else
        ID3D11DeviceContext_Draw(g_ctx, cs->n, cs->vstart);
    return 1;
}

GfxTex* gfx_scene_mirror(const GfxTex* color, const GfxScene* s, float level, float scale, float reach)
{
    const int kept = mirror_wanted(); /* the casters drawn so far this frame have their textures */
    g_mir.asked = g_serial + 1;
    if (!g_dev || !kept || !g_ncasters || !color || !s || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return NULL;
    float M[16], clip_world[16], cam[3];
    if (!mirror_matrix(M, clip_world, cam, s, level) || !(level - cam[1] > 0.05f)) /* (not from under the surface) */
        return NULL;
    float vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vw > color->w || vh > color->h)
        vw = (float)color->w, vh = (float)color->h;
    scale = scale < 0.125f ? 0.125f : scale > 1.0f ? 1.0f : scale;
    const uint32_t w = (uint32_t)(vw * scale + 0.5f) < 64 ? 64 : (uint32_t)(vw * scale + 0.5f);
    const uint32_t h = (uint32_t)(vh * scale + 0.5f) < 64 ? 64 : (uint32_t)(vh * scale + 0.5f);
    const GfxTex* sd = color->depth_seen;
    if (!mirror_targets(w, h, sd && sd->use == GFX_USE_DEPTH ? sd->fmt : 75 /* D3DFMT_D24S8 */))
        return NULL;
    ID3D11RenderTargetView* rtv = (ID3D11RenderTargetView*)target_view(g_mir.color, 0, 0);
    ID3D11DepthStencilView* dsv = (ID3D11DepthStencilView*)target_view(g_mir.depth, 0, 0);
    if (!rtv || !dsv)
        return NULL;
    /* the mirror may still be bound for reading (the frame before's water) */
    ID3D11ShaderResourceView* none[16] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 16, none);
    static const float clear[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, rtv, clear);
    ID3D11DeviceContext_ClearDepthStencilView(g_ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, dsv);
    D3D11_VIEWPORT v = { 0, 0, (float)w, (float)h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
    GfxDepthKey dk;
    memset(&dk, 0, sizeof dk);
    dk.zenable = 1, dk.zwrite = 1, dk.zfunc = 4; /* D3DCMP_LESSEQUAL */
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_mir.blend, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth_state(&dk), 0);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_fx.sun_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        memcpy(ms.pData, M, 64);
        memset((uint8_t*)ms.pData + 64, 0, 16);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_fx.sun_cb, 0);
    }
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 2, 1, &g_fx.sun_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 2, 1, &g_fx.sun_cb);
    g_targets_bound = 0;
    bound_forget();
    g_mir.drawn = g_mir.building = g_mir.dead = 0;
    const float far2 = reach > 0.0f ? (reach + MIRROR_SLACK) * (reach + MIRROR_SLACK) : 0.0f;
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* cs = &g_casters[i];
        if (cs->dead || !cs->textured || cs->lib.vs.rhw)
        {
            g_mir.dead++;
            continue;
        }
        float at[3];
        if (far2 > 0.0f && caster_at(cs, clip_world, at))
        {
            const float dx = at[0] - cam[0], dy = at[1] - cam[1], dz = at[2] - cam[2];
            if (dx * dx + dy * dy + dz * dz > far2)
                continue;
        }
        g_mir.drawn += (uint32_t)mirror_draw(cs);
    }
    ID3D11RenderTargetView* no_rt = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &no_rt, NULL);
    g_targets_bound = 0; /* the game's next draw binds its own again */
    bound_forget();
    return g_mir.drawn ? g_mir.color : NULL;
}
