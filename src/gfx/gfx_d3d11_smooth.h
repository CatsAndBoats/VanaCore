/* The scene's own smooth normals for the sun's shading in Direct3D 11 (sun_smooth, always on in the game; gfx_d3d12.c's vn_replay):
 * vn_replay, and its detail target, which the effects read at t20 (fx_source: smooth_at; fx_unbind lets it go). gfx_d3d11.c
 * includes this after sun_draw. It is no file of its own. */
#pragma once

/* the detail target: each pixel's interpolated vertex normal in view space and its view z (gfx_hlsl.c emit_fs_tail, nmap 6),
 * the scene's size */
static FxTex g_nd;
/* ... and the zone's curved surfaces at each pixel (zn_draw's second target; t21): how far it lies under the surface its normals
 * describe, the bulge's height there, how curved */
static FxTex g_nh;

/* a caster drawn again with its smooth normal (gfx_d3d12.c's vn_caster): a lit fixed-function draw's (most of a zone - Qufim's
 * rock and snow - and the characters: the game lights them by it) and a zone shader's (a vertex shader with a float3 normal
 * input: its pieces' baked colour, no light). Not one already gone, nor one drawn pretransformed */
static int vn_caster(const Caster* c)
{
    const GfxVsKey* vk = &c->lib.vs;
    if (c->dead || vk->rhw || !vk->el[GFX_R_NORMAL].used)
        return 0;
    return vk->prog ? vk->el[GFX_R_NORMAL].type == GFX_FLOAT3 : g_fxs.light != 0.0f && vk->lighting && !vk->flat;
}

/* the projection's 1/P00, 1/P11 and P23 into the sun's buffer (b2: sun_f), which the smooth-normal pass takes its normals into
 * view space by (gfx_hlsl.c emit_fs_tail); sun_m isn't read by it */
static void vn_constants(const GfxScene* s)
{
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_fx.sun_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        const float f[4] = { 1.0f / s->proj[0], 1.0f / s->proj[5], s->proj[11] < 0.0f ? -1.0f : 1.0f, 0.0f };
        memset(ms.pData, 0, 64);
        memcpy((uint8_t*)ms.pData + 64, f, 16);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_fx.sun_cb, 0);
    }
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 2, 1, &g_fx.sun_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 2, 1, &g_fx.sun_cb);
}

/* the functions a caster's smooth normal is drawn with (gfx_d3d12.c's pipeline key for it): its own, with the normal passed on
 * (a lit fixed-function draw's per pixel, a vertex shader's clip-space direction: cpos 2), its colour left out but for an alpha
 * test; NULL while they build */
static Shaders* vn_shaders(const Caster* cs, int at)
{
    LibKey lk = cs->lib;
    GfxVsKey* vk = &lk.vs;
    vk->shadow = 0, vk->motion = 0;
    vk->pixel = vk->prog ? 0 : 1;
    vk->cpos = (uint8_t)(vk->prog ? 2 : 0);
    if (!at)
        memset(&lk.fs, 0, sizeof lk.fs);
    lk.fs.nmap = 6;
    return shaders_for(&lk, cs->vs, at ? cs->ps : NULL);
}

/* one caster again, its uniforms and streams as sun_draw binds them; 0 when it can't be (its ring's chunk written again) */
static int vn_draw(Caster* cs, int at, const Shaders* sh)
{
    int rv = 0;
    for (int st = 0; st < GFX_NSTREAMS; ++st)
        rv |= cs->ring[st];
    if ((rv && g_vring.c[cs->vchunk].gen != cs->vgen) || (cs->iring && g_iring.c[cs->ichunk].gen != cs->igen))
        return 0;
    if (cs->ugen != g_cring.gen)
    {
        if (!uniforms_put((const GfxU*)(g_caster_u + cs->cpu_u), cs->need, NULL, NULL, &cs->uoff))
            return 0;
        cs->ugen = g_cring.gen;
    }
    if (at)
    {
        if (memcmp(cs->srv, g_bound.tex, sizeof cs->srv))
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 16, cs->srv), memcpy(g_bound.tex, cs->srv, sizeof cs->srv);
        if (memcmp(cs->samp, g_bound.samp, sizeof cs->samp))
            ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 8, cs->samp), memcpy(g_bound.samp, cs->samp, sizeof cs->samp);
    }
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

/* The shadow maps go by the polygons: a low-polygon rock under a low sun was shaded from the very edge between a face lit and
 * one turned away, a hard line, where the game's own smooth normals light it on past that edge. Once the scene is done, this
 * frame's casters (each opaque draw, as kept for the sun's maps) are drawn again into the detail target against its finished
 * depth (no write: only what shows), each with its interpolated normal in view space, from what the casters keep - their
 * uniforms, streams and indices, an alpha test's textures - as sun_draw draws them. Drawn in the scene's own viewport, within
 * its target (not the one bound now: a background wider than the window would have cut it). Then bound at t20 for the effects
 * (smooth_at, u.sunuv.w): fx_ao's sun_map, for the angle its faces turned away are shaded by (sun_face) and, with
 * sun_game_shading on, the shadows weighed against the game's own light; the player's light's facing in fx_comp. Only when one
 * of them reads it (`wanted`): with sun_face 0, sun_game_shading 0 and no light round the player, its draws would go for
 * nothing. 1 when it drew any */
static int vn_replay(const GfxTex* color, GfxTex* depth, const GfxScene* s, int wanted)
{
    if (!wanted || g_fxs.sun <= 0.0f || g_fxs.sun_smooth < 0.5f || !g_ncasters || !depth || depth->use != GFX_USE_DEPTH ||
        s->proj[0] == 0.0f || s->proj[5] == 0.0f || s->proj[11] == 0.0f)
        return 0;
    ID3D11DepthStencilView* dsv = (ID3D11DepthStencilView*)target_view(depth, 0, 0);
    if (!dsv || !fx_tex(&g_nd, DXGI_FORMAT_R16G16B16A16_FLOAT, color->w, color->h))
        return 0;
    ID3D11ShaderResourceView* none[10] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 10, none); /* the depth may still be bound for reading */
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 20, 1, none);
    const float zero[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_nd.rtv, zero);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_nd.rtv, dsv);
    float zmin, zmax;
    memcpy(&zmin, &s->vp[4], 4);
    memcpy(&zmax, &s->vp[5], 4);
    float x = fminf((float)s->vp[0], (float)color->w), y = fminf((float)s->vp[1], (float)color->h);
    D3D11_VIEWPORT v = { x, y, fminf((float)s->vp[2], (float)color->w - x), fminf((float)s->vp[3], (float)color->h - y), zmin, zmax };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
    GfxDepthKey dk;
    memset(&dk, 0, sizeof dk);
    dk.zenable = 1, dk.zfunc = 4; /* D3DCMP_LESSEQUAL, no write: the scene's own depth decides */
    RasterKey rk = { 1, 3, 0, 0, 0 };
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend_none, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth_state(&dk), 0);
    ID3D11DeviceContext_RSSetState(g_ctx, raster_state(&rk));
    vn_constants(s);
    g_targets_bound = 0;
    bound_forget();
    uint32_t drawn = 0;
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* cs = &g_casters[i];
        if (!vn_caster(cs))
            continue;
        const int at = alpha_tested(&cs->lib.fs);
        const Shaders* sh = vn_shaders(cs, at); /* (shaders_for: the zone's draws in a row mostly share one, compared first) */
        if (!sh)
            continue; /* building: next frame */
        drawn += (uint32_t)vn_draw(cs, at, sh);
    }
    g_targets_bound = 0; /* the game's next draw binds its own again */
    bound_forget();
    if (!drawn)
        return 0;
    ID3D11RenderTargetView* no_rt = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &no_rt, NULL);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 20, 1, &g_nd.srv);
    return 1;
}

/* --- the zone's own curved surfaces (zn_draw, [graphics.effects] sun_curved) -------------------------------------------------
 * The shadow maps go by the polygons, and a polygon turned from the sun is in its own shadow all of it at once: round a curved
 * low-polygon shape (the Crag of Dem's dome, a hillside) the shadow's own edge was a staircase of whole polygons, and under a
 * high sun the dome's lower half a wall of dark squares, the plainest thing in the picture. Told by the pixel it can't be mended:
 * a wall a few degrees past edge-on reads as a curved shape's edge, and a building's averaged normals lean off its walls (tried
 * 2026-11-11, four ways). VanaCore reads the zone's own meshes for the maps (zonemesh.c), and each vertex's normal with them now,
 * and from the faces and edges round a vertex how far the surface there is curved and smoothly shaded and how high it bulges
 * (core/zonemesh.cpp curve_of: rock and ground alone, 0 all over a building's mesh). Once the scene is done the curved triangles
 * are drawn against its finished depth (no write: only what shows, pulled a little toward the camera, their positions being
 * worked out another way than the game's own draws'), into a target of their own (g_nh, t21): how far the pixel lies under the
 * smooth surface its three corners' normals describe (Phong tessellation's point for it), the bulge's height there, and the
 * curve. The detail target keeps vn_replay's normals (the game's own, with their class); where it drew none (sun_smooth 0) the
 * mesh's are written there too. sun_map then asks the shadow maps from the pixel's place on that smooth surface, not from the
 * polygon cut across under it, and shades the surface's own edge by its normal's angle to the light, a ramp; every other pixel
 * is as it was. One draw, the zone's curved vertices (none in a town). The pass's text is gfx_d3d12.c's (gfx_zn_hlsl), which has
 * Direct3D 12's zn_draw; this is Direct3D 11's. */
#include "zonemesh.h"

static struct
{
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11Buffer* cb;
    ID3D11RasterizerState* rs[2]; /* 24-bit depth, 16-bit */
    ID3D11BlendState* keep;       /* the second target alone written: the first keeps vn_replay's normals */
    int failed;
} g_zn;

static int zn_ready(void)
{
    if (g_zn.failed)
        return 0;
    if (g_zn.vs)
        return 1;
    /* (gfx_d3d12.c's text, both back ends': the constants here at b4, past the draws' own) */
    static const char REG[] = "#define ZN_REG b4\n";
    const char* body = gfx_zn_hlsl();
    char* src = (char*)malloc(sizeof REG + strlen(body));
    if (src)
        memcpy(src, REG, sizeof REG - 1), strcpy(src + sizeof REG - 1, body);
    ID3DBlob* v = src ? compile(src, "zn_vs", "vs_5_0") : NULL;
    ID3DBlob* p = v ? compile(src, "zn_ps", "ps_5_0") : NULL;
    free(src);
    int ok = v && p &&
             SUCCEEDED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &g_zn.vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &g_zn.ps));
    if (v)
        ID3D10Blob_Release(v);
    if (p)
        ID3D10Blob_Release(p);
    D3D11_BUFFER_DESC bd = { 144, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_zn.cb));
    for (int i = 0; ok && i < 2; ++i)
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
        /* toward the camera: the same surface as the game drew, by a few of the depth's steps and a slope's worth */
        rd.DepthBias = i ? -1 : -48, rd.SlopeScaledDepthBias = -1.0f;
        ok = SUCCEEDED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_zn.rs[i]));
    }
    if (ok)
    {
        D3D11_BLEND_DESC bl;
        memset(&bl, 0, sizeof bl);
        bl.IndependentBlendEnable = TRUE;
        bl.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = SUCCEEDED(ID3D11Device_CreateBlendState(g_dev, &bl, &g_zn.keep));
    }
    if (!ok)
    {
        g_zn.failed = 1;
        fprintf(stderr, "[recomp] gfx: the zone's curved surfaces' pass couldn't be made; the sun's shadows go by the polygons there\n");
    }
    return ok;
}

/* 1 when the zone's curved surfaces were drawn into the detail target (cleared first unless vn_replay drew into it: `kept`), and
 * it is bound at t20 */
static int zn_draw(const GfxTex* color, GfxTex* depth, const GfxScene* s, int kept)
{
    GfxBuf *pos, *nrm;
    uint32_t n;
    if (!depth || depth->use != GFX_USE_DEPTH || s->proj[11] == 0.0f || !zonemesh_resident() || !zonemesh_normals(&pos, &nrm, &n) ||
        !pos->srv || !nrm->srv || !zn_ready())
        return 0;
    ID3D11DepthStencilView* dsv = (ID3D11DepthStencilView*)target_view(depth, 0, 0);
    if (!dsv || !fx_tex(&g_nd, DXGI_FORMAT_R16G16B16A16_FLOAT, color->w, color->h) ||
        !fx_tex(&g_nh, DXGI_FORMAT_R16G16B16A16_FLOAT, color->w, color->h))
        return 0;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_zn.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        return 0;
    const float vp[4] = { (float)(s->vp[2] ? s->vp[2] : color->w), (float)(s->vp[3] ? s->vp[3] : color->h), 0.0f, 0.0f };
    memcpy(ms.pData, s->view, 64);
    memcpy((uint8_t*)ms.pData + 64, s->proj, 64);
    memcpy((uint8_t*)ms.pData + 128, vp, 16);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_zn.cb, 0);
    ID3D11ShaderResourceView* none[10] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 10, none); /* the depth may still be bound for reading */
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 20, 2, none);
    const float zero[4] = { 0, 0, 0, 0 };
    if (!kept)
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_nd.rtv, zero);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_nh.rtv, zero);
    ID3D11RenderTargetView* rt[2] = { g_nd.rtv, g_nh.rtv };
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 2, rt, dsv);
    float zmin, zmax;
    memcpy(&zmin, &s->vp[4], 4);
    memcpy(&zmax, &s->vp[5], 4);
    if (zmax <= zmin)
        zmin = 0.0f, zmax = 1.0f;
    const float x = fminf((float)s->vp[0], (float)color->w), y = fminf((float)s->vp[1], (float)color->h);
    D3D11_VIEWPORT v = { x, y, fminf(vp[0], (float)color->w - x), fminf(vp[1], (float)color->h - y), zmin, zmax };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
    GfxDepthKey dk;
    memset(&dk, 0, sizeof dk);
    dk.zenable = 1, dk.zfunc = 4; /* D3DCMP_LESSEQUAL, no write */
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, kept ? g_zn.keep : g_blend_none, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth_state(&dk), 0);
    ID3D11DeviceContext_RSSetState(g_ctx, g_zn.rs[depth->fmt == F_D16 ? 1 : 0]);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_zn.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_zn.ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 4, 1, &g_zn.cb);
    ID3D11ShaderResourceView* in[2] = { pos->srv, nrm->srv };
    ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, 2, in);
    ID3D11DeviceContext_Draw(g_ctx, n, 0);
    ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, 2, none);
    ID3D11RenderTargetView* no_rt[2] = { NULL, NULL };
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 2, no_rt, NULL);
    ID3D11ShaderResourceView* out[2] = { g_nd.srv, g_nh.srv };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 20, 2, out);
    g_targets_bound = 0; /* the game's next draw binds its own again */
    bound_forget();
    return 1;
}
