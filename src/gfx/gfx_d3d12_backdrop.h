/* gfx_d3d12_backdrop.h: frosted glass behind VanaCore's own windows (gfx_present_backdrops). Included by gfx_d3d12.c after
 * fx_pass, which it draws with.
 *
 * At Present the frame (with VanaCore's windows below the lowest backdrop over it, when there are: gfx_present_layer_below,
 * drawn onto the window by bd_below, so the blur shows them too) is shrunk to a half and a quarter and blurred (the bloom's
 * passes: fx_down, then fx_gauss across and down, twice), and, when a rectangle wants more than that, shrunk again to an eighth and blurred the same (bd_blur) before
 * present_draw draws it onto the window; after the frame, under VanaCore's interface layer, each asked-for rectangle is drawn
 * from that blur with its corners rounded (bd_quads, backdrop_fs in gfx_hlsl_util). A rectangle's strength (0 to 1) picks where
 * it sits among the three levels: 0 the half (barely blurred), 0.5 the quarter, 1 the eighth (about twice the quarter's blur),
 * blended between. The blur is never in the back buffer itself: frame generation copies that, and would interpolate it.
 *
 * Frame generation draws each frame it shows through present_draw, so the blur is made again for each (small passes). */

#define BD_MAX 8 /* rectangles at a time */

static struct Backdrops
{
    float rect[BD_MAX][6]; /* x, y, width, height, corner radius (the layer's, the window's, pixels), strength 0..1 */
    int n;                 /* asked for this frame */
    int blurred;           /* the blur is made for the frame being drawn */
    int deep;              /* a rectangle wants more than the quarter's blur: the eighth is made too */
    FxTex t[5];            /* a half, a quarter, the quarter's other half of the gaussian's ping-pong, an eighth, its other half */
    GfxTex* below;         /* VanaCore's windows below the lowest backdrop (gfx_present_layer_below), for this Present */
    ID3D12PipelineState* pso;
    ID3D12PipelineState* comp; /* the frame with `below` over it, shrunk to the half (backdrop_comp_fs), into a float target */
} g_bd;

void gfx_present_layer_below(GfxTex* layer) { g_bd.below = layer; }

static FxU g_bd_zero; /* the gaussian and the shrink read no constants, but their root view is set */

/* the rectangles for this Present: copies of the valid ones. 1 when backdrops are drawn (the pipeline is there) */
int gfx_present_backdrops(const float* rects, int n)
{
    g_bd.n = 0, g_bd.deep = 0;
    if (!g_bd.pso)
        return 0;
    for (int i = 0; rects && i < n && i < BD_MAX; ++i)
    {
        const float* r = rects + i * 6;
        if (!(r[2] >= 2.0f && r[3] >= 2.0f))
            continue;
        float* d = g_bd.rect[g_bd.n++];
        memcpy(d, r, 6 * sizeof(float));
        const float half = (r[2] < r[3] ? r[2] : r[3]) * 0.5f;
        d[4] = r[4] < 0.0f ? 0.0f : (r[4] > half ? half : r[4]);
        d[5] = r[5] < 0.0f ? 0.0f : (r[5] > 1.0f ? 1.0f : r[5]); /* (NaN stays out: not > 0.5 either) */
        if (d[5] > 0.5f)
            g_bd.deep = 1;
    }
    return 1;
}

/* this Present's frames are done: the rectangles are asked for again next frame */
static void bd_done(void) { g_bd.n = 0, g_bd.blurred = 0, g_bd.deep = 0, g_bd.below = NULL; }

/* the windows below the lowest backdrop, over the frame already drawn onto the window (before bd_quads: the blur of the frame
 * with them is drawn over where the backdrops are) */
static void bd_below(void)
{
    GfxTex* layer = g_bd.below;
    if (!layer || layer->srv < 0 || !g_present_layer_pso)
        return;
    ID3D12GraphicsCommandList* l = list();
    uint32_t bind[16] = { 0 };
    bind[0] = (uint32_t)layer->srv, bind[8] = g_present_samp;
    tex_state(layer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu_part(GP_LAYER);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
    set_pso(g_present_layer_pso);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dred_draw(l, "VanaCore's interface below the glass onto the window", 3);
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
}

/* the frame (heap slot src, readable) shrunk and blurred, for a window of w x h; before present_draw binds its target */
static void bd_blur(int32_t src, uint32_t w, uint32_t h)
{
    g_bd.blurred = 0;
    if (g_bd.n == 0 || !g_bd.pso || !g_present_layer || !fx_ready() || !g_fx.pso[FX_DOWN] || !g_fx.pso[FX_GAUSS])
        return;
    const DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; /* (the effects' pipelines draw into this) */
    if (!fx_tex(&g_bd.t[0], fmt, (w + 1) / 2, (h + 1) / 2) || !fx_tex(&g_bd.t[1], fmt, (w + 3) / 4, (h + 3) / 4) ||
        !fx_tex(&g_bd.t[2], fmt, (w + 3) / 4, (h + 3) / 4))
        return;
    if (g_bd.deep && (!fx_tex(&g_bd.t[3], fmt, (w + 7) / 8, (h + 7) / 8) || !fx_tex(&g_bd.t[4], fmt, (w + 7) / 8, (h + 7) / 8)))
        return;
    if (g_bd.below && g_bd.below->srv >= 0 && g_bd.comp)
    {
        /* the half-size picture is the frame with the windows below over it, not the frame alone */
        tex_state(g_bd.below, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const int32_t in[2] = { src, g_bd.below->srv };
        const float vp[4] = { 0, 0, (float)g_bd.t[0].w, (float)g_bd.t[0].h };
        fx_state(&g_bd.t[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
        fx_draw(heap_cpu(&g_rtv, g_bd.t[0].rtv), g_bd.t[0].w, g_bd.t[0].h, g_bd.comp, vp, &g_bd_zero, in, 2, 0, 0);
        fx_state(&g_bd.t[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    else
        fx_pass(&g_bd.t[0], FX_DOWN, &g_bd_zero, &src, 1, 0, 0);
    fx_pass(&g_bd.t[1], FX_DOWN, &g_bd_zero, &g_bd.t[0].srv, 1, 0, 0);
    for (int round = 0; round < 2; ++round)
    {
        fx_pass(&g_bd.t[2], FX_GAUSS, &g_bd_zero, &g_bd.t[1].srv, 1, 2, 0);
        fx_pass(&g_bd.t[1], FX_GAUSS, &g_bd_zero, &g_bd.t[2].srv, 1, 0, 2);
    }
    if (g_bd.deep)
    {
        fx_pass(&g_bd.t[3], FX_DOWN, &g_bd_zero, &g_bd.t[1].srv, 1, 0, 0);
        for (int round = 0; round < 2; ++round)
        {
            fx_pass(&g_bd.t[4], FX_GAUSS, &g_bd_zero, &g_bd.t[3].srv, 1, 2, 0);
            fx_pass(&g_bd.t[3], FX_GAUSS, &g_bd_zero, &g_bd.t[4].srv, 1, 0, 2);
        }
    }
    g_bd.blurred = 1;
}

/* each rectangle from the blur, onto the target present_draw has bound (w x h), over the frame and under the layer */
static void bd_quads(uint32_t w, uint32_t h)
{
    if (!g_bd.blurred || g_bd.n == 0 || !g_bd.pso)
        return;
    ID3D12GraphicsCommandList* l = list();
    set_pso(g_bd.pso);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int k = 0; k < g_bd.n; ++k)
    {
        const float* r = g_bd.rect[k];
        D3D12_RECT sc = { (LONG)r[0] - 1, (LONG)r[1] - 1, (LONG)(r[0] + r[2]) + 2, (LONG)(r[1] + r[3]) + 2 };
        sc.left = sc.left < 0 ? 0 : sc.left, sc.top = sc.top < 0 ? 0 : sc.top;
        sc.right = sc.right > (LONG)w ? (LONG)w : sc.right, sc.bottom = sc.bottom > (LONG)h ? (LONG)h : sc.bottom;
        if (sc.right <= sc.left || sc.bottom <= sc.top)
            continue;
        /* the two levels the strength lies between (the half and the quarter, or the quarter and the eighth), and how far */
        const float s = r[5];
        const int lo = s <= 0.5f ? 0 : 1, hi = s <= 0.5f ? 1 : 3;
        const float mix = s <= 0.5f ? s * 2.0f : (s - 0.5f) * 2.0f;
        uint32_t bind[16] = { 0 };
        bind[0] = (uint32_t)g_bd.t[lo].srv, bind[6] = (uint32_t)g_bd.t[hi].srv, bind[8] = g_present_samp;
        memcpy(&bind[1], r, 5 * sizeof(float)), memcpy(&bind[7], &mix, sizeof(float));
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        dred_draw(l, "a backdrop (the frame blurred behind a window)", 3);
        ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
    }
    D3D12_RECT full = { 0, 0, (LONG)w, (LONG)h };
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &full);
}
