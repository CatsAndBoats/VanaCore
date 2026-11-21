/* The occlusion test on Direct3D 12 ([graphics] occl; gfx.h GfxOccl, core/occlusion.cpp; included by gfx_d3d12.c after gfx_sky).
 *
 * Why: a zone without view lists (The Sanctuary of Zi'Tah, 10,752 pieces) is drawn from its quadtree, every piece in view within the
 * draw distance: at 8x some 1,600 pieces and 6,000 draws a frame from the outpost, most of them cedars standing behind the
 * cedars in front (2026-11-12: 31-64% of the pieces in view behind the zone's collision alone, by the way the camera faced; the
 * canopies hide more). The game draws them all: the CPU's work per draw is the frame's cost.
 *
 * Once a frame, after the world is drawn (gfx_scene_done, before the effects), each piece's box (the core's, gfx_occl_boxes) is
 * tested against the scene's depth: a chain of the farthest depth over ever larger squares (occl_zmax, from the depth itself), and a
 * texel a piece (occl_test: its box's nearest point against the farthest depth over the squares its box covers on the screen, at the
 * level where that's 2x2 texels or fewer). Copied into a readback buffer; once the GPU is past it (occl_collect, the next scene on),
 * into g_gfx_occl.result, which the core reads to leave those pieces undrawn. A piece left undrawn writes no depth, so it hides
 * nothing: what's tested against is only what was drawn, and a piece is never wrongly hidden by one that wasn't. One that comes
 * into sight is drawn again when the test that saw it comes back (a frame or two later): the core keeps that to pieces far off. */

#define OCCL_LEVELS 11 /* the depth's chain: down from 2048 x 2048 to 2 x 2 */
#define OCCL_RB 4      /* readbacks in flight */
#define OCCL_RB_PITCH 256 /* a row of the result: GFX_OCCL_COLS bytes, at the copy's pitch */

static const char OCCL_HLSL[] =
    "cbuffer OU : register(b0) {\n"
    "  float4 m[4]; // the world to clip space: the scene's view times its projection, by rows (v * M)\n"
    "  float4 vp;   // the scene's viewport in the depth: x, y, width, height (pixels)\n"
    "  float4 zr;   // that viewport's depth range (min, max); the margin round a box on the screen (pixels); the pieces\n"
    "  float4 lv;   // x: the chain's levels\n"
    "  float4 zm;   // occl_zmax: where its source's region starts (x, y) and its last texel there (z, w)\n"
    "};\n"
    "cbuffer FB : register(b1) { uint4 fti[2]; uint4 fsi[2]; };\n"
    "Texture2D<float4> tx2[] : register(t0, space1);\n"
    "struct FO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "uint slot(uint k) { return k < 8 ? fti[(k >> 2) & 1][k & 3] : fsi[1][k & 3]; }\n"
    /* a level's texel p is the farthest of its source's 3x3 from 2p (overlapping its neighbours': over all it stands for, and
     * then some), so a pixel x is under level L's texel x >> (L + 1) */
    "float4 occl_zmax(FO i) : SV_Target {\n"
    "  int2 p = int2(i.pos.xy) * 2, o = int2(zm.xy), last = int2(zm.zw);\n"
    "  float z = 0.0;\n"
    "  [unroll] for (int y = 0; y < 3; ++y)\n"
    "    [unroll] for (int x = 0; x < 3; ++x)\n"
    "      z = max(z, tx2[slot(0)].Load(int3(o + min(p + int2(x, y), last), 0)).r);\n"
    "  return float4(z, 0.0, 0.0, 0.0);\n"
    "}\n"
    /* a texel a piece (input 0 the boxes, two texels each; inputs 1 on the chain): 0 off the screen (the game's own test leaves it
     * out), 0.5 seen, 1 hidden - its box's nearest point farther than the farthest depth everywhere it covers on the screen */
    "float4 occl_test(FO i) : SV_Target {\n"
    "  uint2 q = uint2(i.pos.xy);\n"
    "  uint k = q.y * 128 + q.x;\n"
    "  if (k >= (uint)zr.w) return 0.0;\n"
    "  float4 lo = tx2[slot(0)].Load(int3(q.x * 2, q.y, 0)), hi = tx2[slot(0)].Load(int3(q.x * 2 + 1, q.y, 0));\n"
    "  if (lo.w == 0.0) return 0.0;\n"
    "  float2 a = 1e30, b = -1e30;\n"
    "  float zn = 1e30;\n"
    "  [unroll] for (uint c = 0; c < 8; ++c) {\n"
    "    float4 h = ((c & 1) ? hi.x : lo.x) * m[0] + ((c & 2) ? hi.y : lo.y) * m[1] + ((c & 4) ? hi.z : lo.z) * m[2] + m[3];\n"
    "    if (h.w < 0.1) return 0.5; // through the camera's plane: seen\n"
    "    float3 n = h.xyz / h.w;\n"
    "    a = min(a, n.xy); b = max(b, n.xy); zn = min(zn, n.z);\n"
    "  }\n"
    "  if (b.x < -1.0 || a.x > 1.0 || b.y < -1.0 || a.y > 1.0) return 0.0;\n"
    /* partly off the screen: seen. Its part past the edge wasn't tested, and the result lands a frame or two late: turning the
     * camera (which moves far pieces on the screen as much as near ones; occl.near only covers moving) brought that part into
     * view still left out, the sky showing through (2026-11-14: Lower Jeuno's tunnel through its arch; Navukgo's chamber) */
    "  if (a.x < -1.0 || b.x > 1.0 || a.y < -1.0 || b.y > 1.0) return 0.5;\n"
    "  if (zn <= 0.0) return 0.5;\n"
    "  float2 p0 = max(float2(a.x * 0.5 + 0.5, 0.5 - b.y * 0.5) * vp.zw - zr.z, 0.0);\n"
    "  float2 p1 = min(float2(b.x * 0.5 + 0.5, 0.5 - a.y * 0.5) * vp.zw + zr.z, vp.zw - 1.0);\n"
    "  float span = max(max(p1.x - p0.x, p1.y - p0.y), 1.0);\n"
    "  uint L = (uint)clamp(ceil(log2(span)) - 1.0, 0.0, lv.x - 1.0);\n"
    "  float t = exp2((float)L + 1.0);\n"
    "  uint tw, th;\n"
    "  tx2[NonUniformResourceIndex(slot(1 + L))].GetDimensions(tw, th);\n"
    "  int2 e = min(int2(floor(p1 / t)), int2(tw, th) - 1);\n"
    "  int2 s = min(int2(floor(p0 / t)), e);\n"
    "  e = min(e, s + 3);\n"
    "  float z = 0.0;\n"
    "  [loop] for (int y = s.y; y <= e.y; ++y)\n"
    "    [loop] for (int x = s.x; x <= e.x; ++x)\n"
    "      z = max(z, tx2[NonUniformResourceIndex(slot(1 + L))].Load(int3(x, y, 0)).r);\n"
    "  return zr.x + zn * (zr.y - zr.x) > z ? 1.0 : 0.5;\n"
    "}\n";

/* OCCL_HLSL's constants */
typedef struct OcclU
{
    float m[4][4];
    float vp[4];
    float zr[4];
    float lv[4];
    float zm[4];
} OcclU;

static struct
{
    ID3DBlob *zmax_ps, *test_ps;
    ID3D12PipelineState *zmax, *test;
    FxTex hz[OCCL_LEVELS]; /* the depth's chain (occl_zmax) */
    FxTex boxes;           /* the pieces' boxes, two texels each, GFX_OCCL_COLS pieces a row */
    FxTex out;             /* a texel a piece (occl_test) */
    uint32_t boxes_gen;    /* the set in boxes (g_gfx_occl.gen) */
    ID3D12Resource* rb[OCCL_RB];
    uint8_t* rb_cpu[OCCL_RB];
    uint64_t rb_serial[OCCL_RB]; /* the submission its copy is in (0: none waiting) */
    uint32_t rb_gen[OCCL_RB];
    uint64_t tested_serial; /* the frame tested last: once a frame, its first scene */
} g_occl;

static void fx_tex_drop(FxTex* t); /* (gfx_d3d12.c, with the device's other targets) */

/* the shaders and pipelines (fx_build, on its thread, before the effects are ready) */
static void occl_build(void)
{
    g_occl.zmax_ps = compile(OCCL_HLSL, "occl_zmax", "ps_5_1");
    g_occl.test_ps = compile(OCCL_HLSL, "occl_test", "ps_5_1");
    if (g_occl.zmax_ps && g_occl.test_ps)
    {
        g_occl.zmax = fx_make_pso(g_occl.zmax_ps, DXGI_FORMAT_R32_FLOAT);
        g_occl.test = fx_make_pso(g_occl.test_ps, DXGI_FORMAT_R8_UNORM);
    }
    if (!g_occl.zmax || !g_occl.test)
        fprintf(stderr, "[recomp] gfx: the occlusion test didn't compile; every piece is drawn ([graphics] occl)\n");
}

/* its targets and readbacks let go (the device's end, or its loss) */
static void occl_drop(void)
{
    for (int i = 0; i < OCCL_LEVELS; ++i)
        fx_tex_drop(&g_occl.hz[i]);
    fx_tex_drop(&g_occl.boxes);
    fx_tex_drop(&g_occl.out);
    for (int i = 0; i < OCCL_RB; ++i)
    {
        if (g_occl.rb[i])
            ID3D12Resource_Release(g_occl.rb[i]);
        g_occl.rb[i] = NULL, g_occl.rb_cpu[i] = NULL, g_occl.rb_serial[i] = 0, g_occl.rb_gen[i] = 0;
    }
    g_occl.boxes_gen = 0, g_occl.tested_serial = 0;
}

/* the newest test the GPU is done with into g_gfx_occl.result (the older ones done let go) */
static void occl_collect(void)
{
    GfxOccl* o = &g_gfx_occl;
    const uint64_t done = completed();
    int best = -1;
    for (int i = 0; i < OCCL_RB; ++i)
    {
        if (!g_occl.rb_serial[i] || g_occl.rb_serial[i] > done)
            continue;
        if (best >= 0 && g_occl.rb_serial[best] > g_occl.rb_serial[i])
        {
            g_occl.rb_serial[i] = 0;
            continue;
        }
        if (best >= 0)
            g_occl.rb_serial[best] = 0;
        best = i;
    }
    if (best < 0)
        return;
    g_occl.rb_serial[best] = 0;
    const uint8_t* src = g_occl.rb_cpu[best];
    if (!src || g_occl.rb_gen[best] != o->gen || !o->result || !o->count)
        return;
    uint32_t tested = 0, hidden = 0;
    for (uint32_t k = 0; k < o->count; ++k)
    {
        const uint8_t v = src[(size_t)(k / GFX_OCCL_COLS) * OCCL_RB_PITCH + k % GFX_OCCL_COLS];
        o->result[k] = v;
        tested += v != 0;
        hidden += v > 192;
    }
    o->result_gen = o->gen, o->result_seq++, o->tested = tested, o->hidden = hidden;
}

/* the boxes into their texture, for a new set (a zone): through the ring, as the game's textures */
static int occl_upload(const GfxOccl* o, uint32_t rows)
{
    if (!fx_tex(&g_occl.boxes, DXGI_FORMAT_R32G32B32A32_FLOAT, GFX_OCCL_COLS * 2, rows) ||
        !fx_tex(&g_occl.out, DXGI_FORMAT_R8_UNORM, GFX_OCCL_COLS, rows))
        return 0;
    const uint32_t pitch = GFX_OCCL_COLS * 2 * 16; /* (4096: the copy's pitch alignment is 256) */
    const Alloc a = ring((size_t)pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return 0;
    memset(a.cpu, 0, (size_t)pitch * rows);
    for (uint32_t k = 0; k < o->count; ++k)
    {
        const float* b = &o->boxes[k * 6];
        if (!(b[0] <= b[3]))
            continue; /* none: w 0 */
        const float d[8] = { b[0], b[1], b[2], 1.0f, b[3], b[4], b[5], 0.0f };
        memcpy(a.cpu + (size_t)k * sizeof d, d, sizeof d); /* (a row is GFX_OCCL_COLS pieces of 8 floats: k * 32 bytes in) */
    }
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    to.pResource = g_occl.boxes.res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, to.SubresourceIndex = 0;
    from.pResource = a.res, from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = a.off;
    from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    from.PlacedFootprint.Footprint.Width = GFX_OCCL_COLS * 2;
    from.PlacedFootprint.Footprint.Height = rows;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = pitch;
    fx_state(&g_occl.boxes, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, 0, 0, 0, &from, NULL);
    fx_state(&g_occl.boxes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return 1;
}

/* a full-target pass of the test's own (its constants in ROOT_U, as gfx_sky's) */
static void occl_pass(FxTex* to, ID3D12PipelineState* p, const OcclU* u, const int32_t* in, int n)
{
    const Alloc a = ring(sizeof *u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    memcpy(a.cpu, u, sizeof *u);
    fx_state(to, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list(), ROOT_U, a.gpu);
    const float vp[4] = { 0.0f, 0.0f, (float)to->w, (float)to->h };
    fx_draw(heap_cpu(&g_rtv, to->rtv), to->w, to->h, p, vp, NULL, in, n, 0, 0);
    fx_state(to, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

/* the result's copy for the CPU, into a readback not in flight; 0 when all are (the GPU behind: this frame isn't tested) */
static int occl_read(const GfxOccl* o, uint32_t rows)
{
    int slot = -1;
    for (int i = 0; i < OCCL_RB && slot < 0; ++i)
        if (!g_occl.rb_serial[i])
            slot = i;
    if (slot < 0)
        return 0;
    if (!g_occl.rb[slot])
    {
        g_occl.rb[slot] = make_buffer((uint64_t)OCCL_RB_PITCH * (GFX_OCCL_MAX / GFX_OCCL_COLS), D3D12_HEAP_TYPE_READBACK,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
        if (!g_occl.rb[slot])
            return 0;
        ID3D12Resource_Map(g_occl.rb[slot], 0, NULL, (void**)&g_occl.rb_cpu[slot]); /* readback memory stays mapped */
    }
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    from.pResource = g_occl.out.res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, from.SubresourceIndex = 0;
    to.pResource = g_occl.rb[slot], to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    to.PlacedFootprint.Footprint.Width = GFX_OCCL_COLS;
    to.PlacedFootprint.Footprint.Height = rows;
    to.PlacedFootprint.Footprint.Depth = 1;
    to.PlacedFootprint.Footprint.RowPitch = OCCL_RB_PITCH;
    fx_state(&g_occl.out, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, 0, 0, 0, &from, NULL);
    fx_state(&g_occl.out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_occl.rb_serial[slot] = pending(), g_occl.rb_gen[slot] = o->gen;
    return 1;
}

/* From gfx_scene_done, the world just drawn: what came back taken up, then this frame's test (its first scene, the world's) */
static void occl_scene(GfxTex* color, const GfxScene* s)
{
    const GfxOccl* o = &g_gfx_occl;
    occl_collect();
    if (!o->on || !o->count || !o->boxes || g_occl.tested_serial == g_serial || !color || color->type != GFX_TEX_2D ||
        color->use != GFX_USE_RT)
        return;
    GfxTex* depth = color->depth_world ? color->depth_world : color->depth_seen;
    if (!depth || depth->srv < 0 || depth->w != color->w || depth->h != color->h || s->proj[11] == 0.0f)
        return;
    uint32_t vx = s->vp[0], vy = s->vp[1], vw = s->vp[2], vh = s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > depth->w || vy + vh > depth->h)
        vx = vy = 0, vw = depth->w, vh = depth->h;
    if (vw < 256 || vh < 128 || !fx_ready() || !g_occl.zmax || !g_occl.test)
        return; /* (a small scene isn't the world: a portrait) */
    g_occl.tested_serial = g_serial;
    const uint32_t rows = (o->count + GFX_OCCL_COLS - 1) / GFX_OCCL_COLS;
    if (g_occl.boxes_gen != o->gen)
    {
        if (!occl_upload(o, rows))
            return;
        g_occl.boxes_gen = o->gen;
        fprintf(stderr, "[recomp] gfx: occlusion test: %u pieces each frame against the scene's depth (%ux%u) ([graphics] occl)\n",
                o->count, vw, vh);
    }
    /* the chain: half the viewport, then halves of that, to 1 x 1 or OCCL_LEVELS of them */
    uint32_t lw = vw, lh = vh;
    int levels = 0;
    while (levels < OCCL_LEVELS && (lw > 1 || lh > 1))
    {
        lw = (lw + 1) / 2, lh = (lh + 1) / 2;
        if (!fx_tex(&g_occl.hz[levels], DXGI_FORMAT_R32_FLOAT, lw, lh))
            return;
        levels++;
    }
    float minz, maxz;
    memcpy(&minz, &s->vp[4], 4);
    memcpy(&maxz, &s->vp[5], 4);
    if (!(maxz > minz))
        minz = 0.0f, maxz = 1.0f;
    OcclU u;
    memset(&u, 0, sizeof u);
    for (int r = 0; r < 4; ++r) /* view times projection, row vectors */
        for (int c = 0; c < 4; ++c)
            u.m[r][c] = s->view[r * 4 + 0] * s->proj[0 * 4 + c] + s->view[r * 4 + 1] * s->proj[1 * 4 + c] +
                        s->view[r * 4 + 2] * s->proj[2 * 4 + c] + s->view[r * 4 + 3] * s->proj[3 * 4 + c];
    u.vp[0] = (float)vx, u.vp[1] = (float)vy, u.vp[2] = (float)vw, u.vp[3] = (float)vh;
    u.zr[0] = minz, u.zr[1] = maxz, u.zr[2] = fmaxf(o->margin * (float)vw / 1000.0f, 1.0f), u.zr[3] = (float)o->count;
    u.lv[0] = (float)levels;
    const int part_was = gpu_part_cur();
    gpu_part(GP_OCCL);
    tex_state(depth, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    int32_t in[1 + OCCL_LEVELS];
    in[0] = depth->srv;
    u.zm[0] = (float)vx, u.zm[1] = (float)vy, u.zm[2] = (float)(vw - 1), u.zm[3] = (float)(vh - 1);
    occl_pass(&g_occl.hz[0], g_occl.zmax, &u, in, 1);
    for (int lv = 1; lv < levels; ++lv)
    {
        in[0] = g_occl.hz[lv - 1].srv;
        u.zm[0] = u.zm[1] = 0.0f, u.zm[2] = (float)(g_occl.hz[lv - 1].w - 1), u.zm[3] = (float)(g_occl.hz[lv - 1].h - 1);
        occl_pass(&g_occl.hz[lv], g_occl.zmax, &u, in, 1);
    }
    in[0] = g_occl.boxes.srv;
    for (int lv = 0; lv < levels; ++lv)
        in[1 + lv] = g_occl.hz[lv].srv;
    occl_pass(&g_occl.out, g_occl.test, &u, in, 1 + levels);
    occl_read(o, rows);
    gpu_part(part_was);
}
