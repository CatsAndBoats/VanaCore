/* gfx_d3d12_rt.h: an include unit of gfx_d3d12.c (in its scope: g_dev, g_fxs, list, ring, buf_state, mat_inverse, ROOT_RT).
 * Ray-traced reflections (refl_rt): where the screen-space reflections find nothing (fx_ssr: the ray left the picture or went
 * behind everything), the ray goes on through a bounding-volume tree (rtbvh.c), walked in the pixel shader (bvh_trace). The game is a
 * 32-bit process, where Windows shows the card no DXR (tier 0), so no hardware rays: the tree is a plain buffer, read as a root
 * view (t0, space3), and a small one a frame (t1) with the view's inverse that takes the ray into the world. Two trees can be
 * held: the zone's collision (refl_rt 1, floors.c), and the zone's own meshes (refl_rt 2, zonemesh.c), which also take the rays
 * the screen could answer, so what is reflected no longer follows the camera. */
#pragma once

static struct
{
    GfxBuf* bvh[2];         /* the trees: [0] the collision's, [1] the zone's meshes' (rtbvh.h), or NULL */
    uint32_t node_bytes[2]; /* where each one's triangles start */
    uint32_t attr_at;       /* the meshes' tree: where each triangle's own words start (uv, colours, texture: zonemesh.c), 0 none */
    GfxTex* const* tex;     /* its textures, by the index the triangles hold (owned by zonemesh.c) */
    uint32_t ntex;
} g_rtx;

static int g_rt_sky; /* the sky's colour was worked out this frame, for the reflections (the composite's pass: fx_skycol) */

/* gfx.h: a tree (scene 0 the collision's, 1 the zone's meshes'); NULL or 0: let the last go */
void gfx_scene_rt_geometry(const void* data, uint32_t bytes, uint32_t node_bytes, int scene, uint32_t attr_at, GfxTex* const* tex, uint32_t ntex)
{
    const int k = scene ? 1 : 0;
    if (g_rtx.bvh[k])
        gfx_buf_destroy(g_rtx.bvh[k]), g_rtx.bvh[k] = NULL;
    g_rtx.node_bytes[k] = 0;
    if (k)
        g_rtx.attr_at = 0, g_rtx.tex = NULL, g_rtx.ntex = 0;
    if (!data || !bytes || !g_dev)
        return;
    GfxBuf* b = gfx_buf_create(bytes);
    if (!b)
        return;
    gfx_buf_upload(b, data, bytes);
    g_rtx.bvh[k] = b, g_rtx.node_bytes[k] = node_bytes;
    if (k)
        g_rtx.attr_at = attr_at, g_rtx.tex = tex, g_rtx.ntex = ntex > 1024 ? 1024 : ntex;
    fprintf(stderr, "[recomp] gfx: ray tracing: the zone's %s tree, %u KB (triangles from byte %u)\n", k ? "mesh" : "collision", bytes >> 10,
        node_bytes);
}

/* before fx_ssr: its two root views, the tree (or, with none, the frame's words again, which it never reads) and the frame's
 * words: the view's inverse (16 floats), then 1 (the collision's tree) or 2 (the meshes') and where the triangles start when a
 * tree is bound, else 0. Both are always set while the reflections run, since the pipeline reads them under a branch */
static void rt_bind(ID3D12GraphicsCommandList* l, const GfxScene* s, int sky_ok)
{
    const uint32_t nslot = g_rtx.tex ? g_rtx.ntex : 0;
    Alloc a = ring(96 + 4 * nslot, 16);
    if (!a.cpu)
        return;
    memset(a.cpu, 0, 96 + 4 * nslot);
    float inv[16];
    const int k = g_fxs.refl_rt >= 2.0f && g_rtx.bvh[1] ? 1 : 0;
    const int on = g_fxs.refl_rt > 0.0f && g_rtx.bvh[k] && mat_inverse(inv, s->view);
    if (on)
    {
        memcpy(a.cpu, inv, 64);
        /* this frame's world up in view space (words 20 to 22; as the effects' u.up, but not eased over frames: that one lags a
         * camera that pitches, and a reflection taken along it swung after it) */
        {
            const float sign = inv[5] < 0.0f ? -1.0f : 1.0f;
            const float up[3] = { inv[1] * sign, inv[5] * sign, inv[9] * sign };
            const float len = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            float unit[3];
            for (int j = 0; j < 3; ++j)
                unit[j] = len > 1e-6f ? up[j] / len : 0.0f;
            memcpy(a.cpu + 80, unit, 12);
        }
        /* the scene's ambient light (word 23: 10 bits a channel, 0 to 2), for the light on what a ray finds as the game has it */
        {
            uint32_t w = 0;
            for (int j = 0; j < 3; ++j)
                w |= (uint32_t)(fminf(fmaxf(s->ambient[j], 0.0f), 2.0f) * 511.0f + 0.5f) << (10 * j);
            ((uint32_t*)a.cpu)[23] = w;
        }
        ((uint32_t*)a.cpu)[16] = 1u + (uint32_t)k, ((uint32_t*)a.cpu)[17] = g_rtx.node_bytes[k], ((uint32_t*)a.cpu)[19] = sky_ok ? 1u : 0u;
        if (k)
        {
            /* the meshes' textures: their heap slots, read by index (word 24 on), each in the state a pixel shader reads it in */
            ((uint32_t*)a.cpu)[18] = g_rtx.attr_at;
            for (uint32_t i = 0; i < nslot; ++i)
            {
                GfxTex* t = g_rtx.tex[i];
                if (t && t->srv >= 0)
                    tex_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), ((uint32_t*)a.cpu)[24 + i] = (uint32_t)t->srv;
            }
        }
        /* (read by a pixel shader: its state beside the vertex reads' the buffers have) */
        buf_state(g_rtx.bvh[k], BUF_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_RT, on ? g_rtx.bvh[k]->gpu : a.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_RTV, a.gpu);
}

/* The sky's colour, one texel from this frame's scene (and the last frame's, to ease from), for the height fog (fog_color=sky: the
 * composite's eighth input) and the ray-traced reflections' sky (fx_ssr's input 11, rt_sky_input) */
static void sky_colour_pass(FxU* u, int32_t depth_srv, int32_t* comp_in)
{
    const int fogsky = g_fxs.fog_mode == 1.0f && g_fxs.fog > 0.0f;
    const int rtsky = g_fxs.refl_rt >= 2.0f && g_fxs.refl > 0.0f;
    g_rt_sky = 0;
    if (!(fogsky || rtsky) || !g_fx.skycol_pso || !fx_tex(&g_fx.sky[0], DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1) || !fx_tex(&g_fx.sky[1], DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1))
        return;
    FxTex* last_sky = &g_fx.sky[g_fx.sky_i];
    FxTex* sky = &g_fx.sky[g_fx.sky_i ^ 1];
    g_fx.sky_i ^= 1;
    fx_state(last_sky, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fx_state(sky, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float vp1[4] = { 0, 0, 1, 1 };
    const int32_t sky_in[3] = { g_fx.src.srv, depth_srv, last_sky->srv };
    gpu_part(GP_SKY);
    fx_draw(heap_cpu(&g_rtv, sky->rtv), 1, 1, g_fx.skycol_pso, vp1, u, sky_in, 3, 0, 0);
    gpu_part(GP_REST);
    fx_state(sky, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (fogsky)
        comp_in[7] = sky->srv, u->grade2[3] = 1.0f;
    g_rt_sky = rtsky;
}

/* fx_ssr's input 11: the sky's colour, where the ray-traced reflections want it and sky_colour_pass made it this frame (1); its
 * input 10 the same when the characters' mask isn't there (a placeholder: an input past the last given must be one) */
static int rt_sky_input(int32_t* ssr_in, int* ssr_n)
{
    if (!g_rt_sky || g_fxs.refl_rt < 2.0f)
        return 0;
    FxTex* sky = &g_fx.sky[g_fx.sky_i];
    fx_state(sky, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (ssr_in[10] < 0)
        ssr_in[10] = sky->srv;
    ssr_in[11] = sky->srv, *ssr_n = 12;
    return 1;
}

/* The reflections' layer's size: with the ray tracing on, 25 to 100% of the scene's each way (refl_rt_res; half by default: a ray in
 * each pixel through the zone's tree was 8 ms a frame at 3440x1440, 2026-11-26, and what they show is soft). The composite reads it
 * by the nearest depth of its texels (lay_at), against the depth level nearest its size: that level's index */
static int rt_layer_size(uint32_t aw, uint32_t ah, uint32_t* w, uint32_t* h)
{
    const float res = g_fxs.refl_rt >= 2.0f ? fminf(fmaxf(g_fxs.refl_rt_res, 0.25f), 1.0f) : 1.0f;
    *w = (uint32_t)ceilf((float)aw * res), *h = (uint32_t)ceilf((float)ah * res);
    return res < 0.4f ? 2 : res < 0.6f ? 1 : 0;
}
