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
static void rt_bind(ID3D12GraphicsCommandList* l, const float* view)
{
    const uint32_t nslot = g_rtx.tex ? g_rtx.ntex : 0;
    Alloc a = ring(80 + 4 * nslot, 16);
    if (!a.cpu)
        return;
    memset(a.cpu, 0, 80 + 4 * nslot);
    float inv[16];
    const int k = g_fxs.refl_rt >= 2.0f && g_rtx.bvh[1] ? 1 : 0;
    const int on = g_fxs.refl_rt > 0.0f && g_rtx.bvh[k] && mat_inverse(inv, view);
    if (on)
    {
        memcpy(a.cpu, inv, 64);
        ((uint32_t*)a.cpu)[16] = 1u + (uint32_t)k, ((uint32_t*)a.cpu)[17] = g_rtx.node_bytes[k];
        if (k)
        {
            /* the meshes' textures: their heap slots, read by index (word 20 on), each in the state a pixel shader reads it in */
            ((uint32_t*)a.cpu)[18] = g_rtx.attr_at;
            for (uint32_t i = 0; i < nslot; ++i)
            {
                GfxTex* t = g_rtx.tex[i];
                if (t && t->srv >= 0)
                    tex_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), ((uint32_t*)a.cpu)[20 + i] = (uint32_t)t->srv;
            }
        }
        /* (read by a pixel shader: its state beside the vertex reads' the buffers have) */
        buf_state(g_rtx.bvh[k], BUF_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_RT, on ? g_rtx.bvh[k]->gpu : a.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_RTV, a.gpu);
}
