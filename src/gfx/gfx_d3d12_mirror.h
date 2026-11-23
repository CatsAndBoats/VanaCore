/* The world mirrored in the water, in Direct3D 12 (gfx.h gfx_scene_mirror; gfx_d3d11_mirror.h has Direct3D 11's): the frame's
 * casters - each opaque draw of the world, as kept for the sun's maps - drawn again with their own colour from the camera
 * mirrored about the water's level, into a smaller target the water's pixel function reads (gfx_hlsl_water.h emit_water_mirror).
 * Each goes through its own functions with the mirror's key (GfxVsKey.shadow GFX_VS_MIRROR): the clip-space position it makes
 * is taken through one matrix (ROOT_SUN, as the sun's maps take it) - the camera's inverse, the mirror about the level, the
 * camera again - whose depth is the height over the level, so what lies under the water is cut off by the near plane and no draw
 * needs a clip plane of its own. The light and the fog on each are the draw's own: what's mirrored is lit as it stands. Not the
 * caster pass's pieces (out of the camera's view, kept for the maps alone, without their textures).
 * gfx_d3d12.c includes this after gfx_scene_snapshot. It is no file of its own. */
#pragma once

#define MIRROR_KEEP 4  /* frames after the last call the casters still keep their textures for it */
#define MIRROR_Z 0.25f /* the mirror's depth: this share of the height over the level, over the view's w; under 1 within any view
                        * narrower than 75 degrees off its axis (the height is less than the distance) */
#define MIRROR_SLACK 60.0f /* a caster stands where its first vertex is: this much farther than the reach it may still reach in */

static struct
{
    GfxTex *color, *depth;
    uint64_t asked;                 /* the frame after the one gfx_scene_mirror was last called in (0 never) */
    uint32_t drawn, building, dead; /* the last pass's casters: drawn, left out while their pipelines build, gone */
} g_mir;

/* 1 while the water asks for the mirror: every caster keeps its textures' and samplers' slots then (an alpha-tested one's are
 * kept anyway), and they're kept with the sun's shadows off too (draw_encode, caster_wanted) */
static int mirror_wanted(void) { return g_mir.asked && g_serial < g_mir.asked + MIRROR_KEEP; }

/* the mirror's key's blend (build_pipeline): the colour as drawn and the alpha 0, where the mirror is cleared to alpha 1, the
 * sky's place */
static void mirror_blend(D3D12_RENDER_TARGET_BLEND_DESC* c)
{
    c->BlendEnable = TRUE;
    c->SrcBlend = D3D12_BLEND_ONE, c->DestBlend = D3D12_BLEND_ZERO, c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    c->SrcBlendAlpha = c->DestBlendAlpha = D3D12_BLEND_ZERO;
}

static void mirror_release(void)
{
    gfx_tex_destroy(g_mir.color), gfx_tex_destroy(g_mir.depth);
    memset(&g_mir, 0, sizeof g_mir);
}

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
    return g_mir.color && g_mir.depth && g_mir.color->srv >= 0;
}

/* the pipeline a caster is drawn into the mirror with: its own functions with the mirror's key, lit per vertex, without what
 * the scene's passes add to a draw (normal maps, the rain's wet look, the pool, the height fog); the faces it culled by one
 * winding culled by the other (the mirror turns each over) */
static void mirror_pipe_key(const Caster* cs, PipeKey* pk)
{
    memset(pk, 0, sizeof *pk);
    pk->lib = cs->lib;
    pk->lib.vs.shadow = GFX_VS_MIRROR, pk->lib.vs.pixel = 0, pk->lib.vs.motion = 0, pk->lib.vs.cpos = 0, pk->lib.vs.inst = 0;
    pk->lib.fs.nmap = 0, pk->lib.fs.wet = 0, pk->lib.fs.pool = 0, pk->lib.fs.hfog = 0;
    pk->color = (uint32_t)g_mir.color->dxfmt, pk->dsv = (uint32_t)g_mir.depth->dxfmt;
    pk->pipe.write_mask = 15;
    pk->depth.zenable = 1, pk->depth.zwrite = 1, pk->depth.zfunc = 4; /* D3DCMP_LESSEQUAL */
    pk->cull = (uint8_t)(cs->cull == 2 ? 3 : cs->cull == 3 ? 2 : 1), pk->fill = 3;
    pk->topo = (uint8_t)(cs->topo == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
        : cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINELIST || cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
        : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
}

/* the casters into the mirror's targets, already bound: this frame's own, within the reach (far2 its square, 0 none) of cam */
static void mirror_casters(ID3D12GraphicsCommandList* l, const float* clip_world, const float cam[3], float far2)
{
    D3D12_GPU_VIRTUAL_ADDRESS bound_vb[GFX_NSTREAMS];
    memset(bound_vb, 0, sizeof bound_vb);
    D3D12_INDEX_BUFFER_VIEW bound_ib;
    memset(&bound_ib, 0, sizeof bound_ib);
    /* the key the caster before had, and its pipeline: the zone's draws in a row mostly share one */
    LibKey last;
    uint8_t last_topo = 0, last_cull = 0;
    ID3D12PipelineState* last_ps = NULL;
    memset(&last, 0, sizeof last);
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* cs = &g_casters[i];
        if (cs->dead || cs->sought || !cs->textured || cs->lib.vs.rhw)
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
        ID3D12PipelineState* ps = last_ps && last_topo == cs->topo && last_cull == cs->cull && !memcmp(&last, &cs->lib, sizeof last) ? last_ps : NULL;
        if (!ps)
        {
            PipeKey pk;
            mirror_pipe_key(cs, &pk);
            ps = pipeline_for(&pk, cs->vs, cs->ps);
            if (!ps)
            {
                g_mir.building += (uint32_t)g_pipe_waiting;
                continue; /* building: next frame */
            }
        }
        last = cs->lib, last_topo = cs->topo, last_cull = cs->cull, last_ps = ps;
        for (int t = 0; t < 8; ++t)
            if (cs->tex[t])
                tex_state(cs->tex[t], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, cs->bind, 0);
        set_pso(ps);
        set_topology((D3D12_PRIMITIVE_TOPOLOGY)cs->topo);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, cs->ub);
        for (int j = 0; j <= GFX_NSTREAMS; ++j)
            if (cs->buf[j])
                buf_state(cs->buf[j], BUF_READ);
        for (int st = 0; st < GFX_NSTREAMS; ++st)
            if (cs->vb[st] != bound_vb[st])
                ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + st, cs->vb[st]), bound_vb[st] = cs->vb[st];
        DrawNote* dn = dred_draw(l, "the water's mirror (the scene's caster again)", cs->n);
        if (dn)
            dn->start = cs->ibv.BufferLocation ? 0 : cs->vstart, dn->flags = cs->ibv.BufferLocation ? DN_INDEXED : 0;
        if (cs->ibv.BufferLocation)
        {
            if (memcmp(&cs->ibv, &bound_ib, sizeof bound_ib))
                ID3D12GraphicsCommandList_IASetIndexBuffer(l, &cs->ibv), bound_ib = cs->ibv;
            ID3D12GraphicsCommandList_DrawIndexedInstanced(l, cs->n, 1, 0, 0, 0);
        }
        else
            ID3D12GraphicsCommandList_DrawInstanced(l, cs->n, 1, cs->vstart, 0);
        g_mir.drawn++;
    }
}

GfxTex* gfx_scene_mirror(const GfxTex* color, const GfxScene* s, float level, float scale, float reach)
{
    const int kept = mirror_wanted(); /* the casters drawn so far this frame have their textures */
    g_mir.asked = g_serial + 1;
    if (!g_dev || !kept || !color || !s || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return NULL;
    BATCH_SETTLE(); /* the zone's draws kept: their casters with them */
    if (!g_ncasters)
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
    if (!mirror_targets(w, h, sd && sd->use == GFX_USE_DEPTH ? sd->fmt : F_D24S8))
        return NULL;
    ID3D12GraphicsCommandList* l = list();
    tex_state(g_mir.color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    tex_state(g_mir.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_view(g_mir.color, 0, 0), dsv = target_view(g_mir.depth, 0, 0);
    static const float clear[4] = { 0, 0, 0, 1 }; /* alpha 1: the sky's place, where nothing is drawn */
    ID3D12GraphicsCommandList_ClearRenderTargetView(l, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(l, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT v = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    static const float nofade[4] = { 1, 0, 0, 0 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 16, M, 0);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 4, nofade, 16);
    g_targets_bound = 0;
    drawn_forget();
    g_mir.drawn = g_mir.building = g_mir.dead = 0;
    mirror_casters(l, clip_world, cam, reach > 0.0f ? (reach + MIRROR_SLACK) * (reach + MIRROR_SLACK) : 0.0f);
    g_targets_bound = 0; /* the game's next draw binds its own again */
    drawn_forget();
    return g_mir.drawn ? g_mir.color : NULL;
}
