/* VanaCore's sky on Direct3D 12 (gfx.h gfx_sky; included by gfx_d3d12.c after gfx_glow): drawn in place of the game's dome,
 * whose draws come first in the frame's world and are skipped (sky.c). One full-screen pass into the target bound now (the
 * scene's), over the dome's viewport, writing no depth: each pixel's ray through the dome's own world-view-projection, its
 * elevation and bearing, the dome's colour there from its rings. The world drawn after it covers it as it covered the dome.
 *
 * True Sky's first phase (research/true-sky.md): the game's own sky drawn again, so the rest can be built on it. Its source is its
 * own, apart from the scene effects' (FX_HLSL_1/2, at the 64 KB literal limit), built with them (fx_build: g_fx.truesky) on the
 * effects' vertex shader (fx_vs: a triangle over the viewport, uv 0..1 across it), one pipeline per target format (fx_by_fmt). */

static const char SKY_HLSL[] =
    "cbuffer SU : register(b0) {\n"
    "  float4 inv[4];  // clip space to the dome's own: its world-view-projection inverted, by rows (v * M)\n"
    "  float4 info;    // DLSS's sub-pixel step (clip space), taken back out; the rings; spare\n"
    "  float4 ring[16]; // x: each ring's height, the sine of its elevation, from the lowest up\n"
    "  float4 col[256]; // ring r's colour at bearing b (16 round it, the first toward +z, then toward +x): col[r * 16 + b]\n"
    "};\n"
    "struct FO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 bearing_col(uint r, float b) {\n"
    "  uint b0 = (uint)b % 16, b1 = (b0 + 1) % 16;\n"
    "  return lerp(col[r * 16 + b0], col[r * 16 + b1], frac(b));\n"
    "}\n"
    "float4 sky_ps(FO i) : SV_Target {\n"
    "  float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2) - info.xy;\n"
    "  float4 h = ndc.x * inv[0] + ndc.y * inv[1] + 0.5 * inv[2] + inv[3];\n"
    "  float3 d = normalize(h.xyz / h.w); // the camera is the dome's origin: a point on the ray is its direction\n"
    "  float s = -d.y;                    // the game's y points down\n"
    "  uint n = (uint)info.z;\n"
    "  if (n < 2 || s < ring[0].x || s > ring[n - 1].x) discard; // where the dome drew nothing\n"
    "  float b = frac(atan2(d.x, d.z) / 6.28318531 + 1.0) * 16.0;\n"
    "  uint r = 0;\n"
    "  [loop] for (uint k = 1; k + 1 < n; ++k) if (s >= ring[k].x) r = k;\n"
    "  float t = saturate((s - ring[r].x) / max(ring[r + 1].x - ring[r].x, 1e-6));\n"
    "  return lerp(bearing_col(r, b), bearing_col(r + 1, b), t);\n"
    "}\n";

static const char* sky_hlsl(void) { return SKY_HLSL; }

/* SKY_HLSL's constants */
typedef struct SkyU
{
    float inv[4][4];
    float info[4];
    float rings[GFX_SKY_RINGS][4];
    float col[GFX_SKY_RINGS * GFX_SKY_BEARINGS][4];
} SkyU;

int gfx_sky(const GfxSky* s)
{
    if (!g_dev || !fx_ready() || !g_fx.truesky)
        return 0;
    if (!s)
        return 1;
    BATCH_SETTLE(); /* (the zone's draws kept go first: the sky is drawn before them) */
    if (!g_rt || g_rt->type != GFX_TEX_2D || g_rt_face || g_rt_level || s->nrings < 2 || s->nrings > GFX_SKY_RINGS)
        return 0;
    const int fmt = fx_by_fmt(g_rt->dxfmt);
    if (fmt < 0 || !g_fx.by_fmt[fmt].sky)
        return 0;
    static SkyU u;
    memset(&u, 0, sizeof u);
    memcpy(u.inv, s->inv, sizeof u.inv);
    u.info[0] = s->jitter[0], u.info[1] = s->jitter[1], u.info[2] = (float)s->nrings;
    for (uint32_t r = 0; r < s->nrings; ++r)
    {
        u.rings[r][0] = s->ring[r];
        memcpy(u.col[r * GFX_SKY_BEARINGS], s->color[r], sizeof s->color[r]);
    }
    const Alloc a = ring(sizeof u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return 0;
    memcpy(a.cpu, &u, sizeof u);
    const int part_was = gpu_part_cur();
    gpu_part(GP_TRUESKY);
    uint32_t tw, th;
    level_size(g_rt, 0, &tw, &th);
    tex_state(g_rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_VIEWPORT v = viewport_for(s->vp);
    const float vp[4] = { v.TopLeftX, v.TopLeftY, v.Width, v.Height };
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list(), ROOT_U, a.gpu); /* (fx_draw sets none: u is NULL) */
    fx_draw(target_view(g_rt, 0, 0), tw, th, g_fx.by_fmt[fmt].sky, vp, NULL, NULL, 0, 0, 0);
    gpu_part(part_was);
    return 1;
}
