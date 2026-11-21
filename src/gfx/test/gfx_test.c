/* The graphics back end without the game (R3.2): renders into an offscreen target through gfx.h,
 * reads the pixels back and checks them - clears, D3D's pixel-center rules for XYZRHW vertices,
 * texturing and the texture formats, fixed-function lighting, fog, alpha test, blending, the
 * vs.1.1 / ps.1.1 translation - then builds a sweep of fixed-function keys (every texture op,
 * argument modifier, fog mode, light type, texture coordinate source) so a generator change that
 * emits bad MSL fails here rather than in the game.
 *
 * usage: gfx_test            (exit status 0 when every check passes)
 *        gfx_test --window   the same on a device made for an SDL window, then two seconds of
 *                            frames presented to it (the CAMetalLayer path) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef VANACORE
/* VanaCore.GfxTest: offscreen only; setenv as the CRT spells it */
#define setenv(k, v, o) _putenv_s(k, v)
#else
#include <SDL3/SDL.h>
#endif

#include "gfx.h"

#ifdef VANACORE
#include "gfx_backend.h" /* gfx12_lose_device, gfx_selected_d3d11 */
int grass_test(void); /* grass_test.c */
void grass_device_lost(void); /* grass.c */
void floors_device_lost(void); /* floors.c */
int vanacore_gfx_dred_notes_test(void (*out)(const char* line), int after); /* gfx_d3d12.c */
#endif

#define W 32
#define H 32

static int g_fails;

#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            g_fails++;                                                                                                \
        }                                                                                                             \
    } while (0)

static GfxTex *g_rt, *g_ds;
static uint32_t g_px[W * H];

static void identity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void readback(void)
{
    gfx_tex_read(g_rt, 0, 0, g_px, W * 4);
}

/* A8R8G8B8 as the D3D value */
static uint32_t px(int x, int y) { return g_px[y * W + x]; }

static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 32; s += 8)
    {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < -tol || d > tol)
            return 0;
    }
    return 1;
}

static const uint32_t VP[6] = { 0, 0, W, H, 0, 0x3F800000u };

/* a draw with the device defaults the front end would produce */
static void defaults(GfxDraw* d)
{
    memset(d, 0, sizeof *d);
    identity(d->u.wvp), identity(d->u.wv), identity(d->u.wvit);
    for (int i = 0; i < 8; ++i)
        identity(d->u.texm[i]);
    d->u.vp[0] = 0, d->u.vp[1] = 0, d->u.vp[2] = W, d->u.vp[3] = H;
    d->u.tfactor[0] = d->u.tfactor[1] = d->u.tfactor[2] = d->u.tfactor[3] = 1;
    d->pipe.write_mask = 0xF;
    d->cull = 1;
    d->depth.zfunc = 4;
    memcpy(d->vp, VP, sizeof VP);
    /* stage 0: MODULATE(TEXTURE, CURRENT), SELECTARG1(TEXTURE) alpha - D3D's defaults */
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){ 4, 2, 1, 1, 2, 2, 1, 1, 1, 0, 0, 2 };
}

/* XYZRHW | DIFFUSE | TEX1 (FVF 0x144), the game's UI vertex */
typedef struct
{
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} UiVert;

static void layout_ui(GfxDraw* d)
{
    d->vs.rhw = 1;
    d->vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT4, 0 };
    d->vs.el[5] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d->vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d->vs.ntex = 1;
    d->u.stride[0] = sizeof(UiVert);
    d->u.offset[0] = 0, d->u.offset[5] = 16, d->u.offset[7] = 20;
}

static void quad(UiVert* v, float x0, float y0, float x1, float y1, uint32_t c, float z)
{
    v[0] = (UiVert){ x0, y0, z, 1, c, 0, 0 };
    v[1] = (UiVert){ x1, y0, z, 1, c, 1, 0 };
    v[2] = (UiVert){ x0, y1, z, 1, c, 0, 1 };
    v[3] = (UiVert){ x1, y1, z, 1, c, 1, 1 };
}

static void draw_ui(GfxDraw* d, UiVert* v)
{
    d->data[0] = v, d->size[0] = 4 * sizeof(UiVert);
    d->prim = GFX_TRIANGLESTRIP, d->count = 2;
    gfx_draw(d);
}

static void test_clear(void)
{
    gfx_clear(0, NULL, 3, 0xFFFF0000u, 1.0f, 0, VP);
    readback();
    CHECK(px(0, 0) == 0xFFFF0000u && px(W - 1, H - 1) == 0xFFFF0000u, "full clear: %08x", px(0, 0));
    int32_t r[4] = { 4, 4, 8, 8 };
    gfx_clear(1, r, 1, 0xFF00FF00u, 1.0f, 0, VP);
    readback();
    CHECK(px(4, 4) == 0xFF00FF00u && px(7, 7) == 0xFF00FF00u, "rect clear inside: %08x %08x", px(4, 4), px(7, 7));
    CHECK(px(3, 4) == 0xFFFF0000u && px(8, 8) == 0xFFFF0000u, "rect clear outside: %08x %08x", px(3, 4), px(8, 8));
}

static void test_rhw_edges(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    UiVert v[4];
    quad(v, 8, 8, 24, 24, 0xFF0000FFu, 0.5f);
    draw_ui(&d, v);
    readback();
    /* D3D: pixel centers on integers, so [8, 24) covers pixels 8..23 */
    CHECK(px(8, 8) == 0xFF0000FFu && px(23, 23) == 0xFF0000FFu, "quad inside: %08x %08x", px(8, 8), px(23, 23));
    CHECK(px(7, 8) == 0xFF000000u && px(24, 23) == 0xFF000000u && px(8, 7) == 0xFF000000u && px(8, 24) == 0xFF000000u,
        "quad edges: %08x %08x %08x %08x", px(7, 8), px(24, 23), px(8, 7), px(8, 24));
}

static void test_texture(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    /* 2x2: red green / blue white, point sampled over a 16x16 quad */
    uint32_t tex[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 2, 2, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, tex, 8);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0].tex = 1;
    d.tex[0] = t;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0 };
    UiVert v[4];
    quad(v, 0, 0, 16, 16, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(2, 2) == 0xFFFF0000u && px(12, 2) == 0xFF00FF00u && px(2, 12) == 0xFF0000FFu && px(12, 12) == 0xFFFFFFFFu,
        "texture: %08x %08x %08x %08x", px(2, 2), px(12, 2), px(2, 12), px(12, 12));

    /* modulated by the vertex color */
    quad(v, 16, 0, 32, 16, 0xFF808080u, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(28, 12), 0xFF808080u, 1), "modulate: %08x", px(28, 12));
    gfx_tex_destroy(t);

    /* R5G6B5 and DXT1 */
    uint16_t t565[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    t = gfx_tex_create(GFX_TEX_2D, 23, 2, 2, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, t565, 4);
    d.tex[0] = t;
    quad(v, 0, 16, 16, 32, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(2, 18) == 0xFFFF0000u && px(12, 18) == 0xFF00FF00u && px(2, 28) == 0xFF0000FFu, "R5G6B5: %08x %08x %08x", px(2, 18),
        px(12, 18), px(2, 28));
    gfx_tex_destroy(t);

    /* one DXT1 block, every texel color0 = pure green (0x07E0) */
    uint8_t dxt[8] = { 0xE0, 0x07, 0xE0, 0x07, 0, 0, 0, 0 };
    t = gfx_tex_create(GFX_TEX_2D, 0x31545844u /* DXT1 */, 4, 4, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, dxt, 8);
    d.tex[0] = t;
    quad(v, 16, 16, 32, 32, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(24, 24) == 0xFF00FF00u, "DXT1: %08x", px(24, 24));
    gfx_tex_destroy(t);
}

static void test_alpha(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 }; /* diffuse only */
    /* alpha test GREATER 0x80: a 0x40-alpha quad is discarded */
    d.fs.alpha_func = 5;
    d.u.params[1] = 128;
    UiVert v[4];
    quad(v, 0, 0, 16, 16, 0x40FFFFFFu, 0.5f);
    draw_ui(&d, v);
    /* blending SRCALPHA / INVSRCALPHA at 50% over black */
    d.fs.alpha_func = 8;
    d.pipe.blend = 1, d.pipe.src = 5, d.pipe.dst = 6, d.pipe.op = 1;
    quad(v, 16, 0, 32, 16, 0x80FFFFFFu, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(px(8, 8) == 0xFF000000u, "alpha test: %08x", px(8, 8));
    CHECK(near(px(24, 8) & 0xFFFFFF, 0x808080u, 2), "blend: %08x", px(24, 8));
}

static void test_depth(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    UiVert v[4];
    quad(v, 0, 0, 32, 32, 0xFFFF0000u, 0.25f);
    draw_ui(&d, v);
    quad(v, 0, 0, 16, 16, 0xFF00FF00u, 0.75f); /* behind: hidden */
    draw_ui(&d, v);
    quad(v, 16, 16, 32, 32, 0xFF0000FFu, 0.1f); /* in front */
    draw_ui(&d, v);
    readback();
    CHECK(px(8, 8) == 0xFFFF0000u && px(24, 24) == 0xFF0000FFu, "depth: %08x %08x", px(8, 8), px(24, 24));
}

/* XYZ | DIFFUSE (FVF 0x042) in camera space, through FFXI's projection (near 0.1, no far plane to speak of) */
static void persp_quad(float z, float x0, float x1, uint32_t c, int zwrite, int zbias)
{
    struct
    {
        float x, y, z;
        uint32_t c;
    } v[6] = {
        { x0, -z, z, c }, { x1, -z, z, c }, { x1, z, z, c }, /* corners from x0 at the top: split top left to bottom right */
        { x0, -z, z, c }, { x1, z, z, c }, { x0, z, z, c },
    };
    GfxDraw d;
    defaults(&d);
    memset(d.u.wvp, 0, 64);
    d.u.wvp[0] = d.u.wvp[5] = 1, d.u.wvp[10] = d.u.wvp[11] = -1, d.u.wvp[14] = -0.1f;
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[5] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d.u.stride[0] = 16, d.u.offset[5] = 12;
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    d.depth.zenable = 1, d.depth.zwrite = (uint8_t)zwrite, d.depth.zfunc = 4;
    d.zbias = zbias;
    float pull = (float)zbias * 0.00125f; /* d3d8.c's */
    memcpy(&d.u.pad0[2], &pull, 4);
    d.data[0] = v, d.size[0] = sizeof v;
    d.prim = GFX_TRIANGLELIST, d.count = 2;
    gfx_draw(&d);
}

/* D3DRS_ZBIAS: a decal on a wall (the wall's plane, split the other way) shows over it, near and at 30 yalms,
 * but not over a banner hung 3 cm in front of the wall. A bias in depth alone large enough for the near wall
 * took the decal a third of a yalm forward at 30, over the banner (Bastok Markets' towers). */
static void test_decal_bias(void)
{
    static const float dist[2] = { 1.0f, 30.0f };
    for (int i = 0; i < 2; ++i)
    {
        float z = -dist[i], s = dist[i];
        gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
        persp_quad(z, -s, s, 0xFFFF0000u, 1, 0);           /* the wall */
        persp_quad(z + 0.03f, -s, 0, 0xFF00FF00u, 1, 0);   /* the banner, the left half */
        persp_quad(z, s, -s, 0xFF0000FFu, 0, 8);           /* the decal over both, mirrored: the other diagonal */
        readback();
        CHECK(px(24, 8) == 0xFF0000FFu && px(24, 24) == 0xFF0000FFu, "decal at %g: on the wall %08x %08x", dist[i], px(24, 8),
            px(24, 24));
        CHECK(px(8, 8) == 0xFF00FF00u && px(8, 24) == 0xFF00FF00u, "decal at %g: through the banner %08x %08x", dist[i],
            px(8, 8), px(8, 24));
    }
}

/* XYZ | NORMAL (FVF 0x012) through the transform and one directional light */
static void test_lighting(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    float vert[4][6] = { { -1, 1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 }, { -1, -1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 } };
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 24, d.u.offset[3] = 12;
    d.vs.lighting = 1, d.vs.nlights = 1, d.vs.light_type[0] = 3, d.vs.normalize = 1;
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    float half[4] = { 0.5f, 0.5f, 0.5f, 1 };
    memcpy(d.u.mat_d, half, 16);
    /* light straight at the surface (toward the light: -z), white; ambient 0.25 on a 1.0 material */
    d.u.light[0].diffuse[0] = d.u.light[0].diffuse[1] = d.u.light[0].diffuse[2] = 1;
    d.u.light[0].dir[2] = -1;
    d.u.ambient[0] = d.u.ambient[1] = d.u.ambient[2] = 0.25f;
    d.u.mat_a[0] = d.u.mat_a[1] = d.u.mat_a[2] = 1;
    d.data[0] = vert, d.size[0] = sizeof vert;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
    readback();
    /* 0.25 ambient + 0.5 diffuse = 0.75 */
    CHECK(near(px(16, 16), 0xFFBFBFBFu, 2), "lighting: %08x", px(16, 16));
}

/* A point light just before the middle of a large quad: lit per pixel (light 2) its pool shows in
 * the middle; lit per vertex (light 0, and light 1: there the sun is per pixel, the game's torches
 * per vertex), the corners (almost edge-on to it) are all there is, and the middle stays dark. */
static void test_pixel_lighting(void)
{
    static const float modes[3] = { 2, 0, 1 };
    for (int pass = 0; pass < 3; ++pass)
    {
        gfx_fx_set("light", modes[pass]);
        gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
        GfxDraw d;
        defaults(&d);
        float vert[4][6] = { { -1, 1, 0.5f, 0, 0, -1 }, { 1, 1, 0.5f, 0, 0, -1 }, { -1, -1, 0.5f, 0, 0, -1 }, { 1, -1, 0.5f, 0, 0, -1 } };
        d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d.u.stride[0] = 24, d.u.offset[3] = 12;
        d.vs.lighting = 1, d.vs.nlights = 1, d.vs.light_type[0] = 1, d.vs.normalize = 1;
        d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
        d.u.mat_d[0] = d.u.mat_d[1] = d.u.mat_d[2] = d.u.mat_d[3] = 1;
        d.u.light[0].diffuse[0] = d.u.light[0].diffuse[1] = d.u.light[0].diffuse[2] = 1;
        d.u.light[0].pos[2] = 0.4f, d.u.light[0].pos[3] = 10.0f;
        d.u.light[0].att[0] = 1;
        d.data[0] = vert, d.size[0] = sizeof vert;
        d.prim = GFX_TRIANGLESTRIP, d.count = 2;
        gfx_draw(&d);
        readback();
        uint32_t c = px(W / 2, H / 2) & 255;
        if (pass == 0)
            CHECK(c >= 230, "per-pixel lighting: middle %u (want the light's pool)", c);
        else
            CHECK(c <= 40, "per-vertex point light (light %g): middle %u (want dark, from the corners)", modes[pass], c);
    }
    gfx_fx_set("light", 1.0f);
}

static void test_fog(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    /* linear table fog from 0 to 2 on w = 1 (rhw 1): half fogged toward white */
    d.fs.fog = 3;
    d.u.params[2] = 0, d.u.params[3] = 2;
    d.u.fogcolor[0] = d.u.fogcolor[1] = d.u.fogcolor[2] = 1;
    UiVert v[4];
    quad(v, 0, 0, 32, 32, 0xFF000000u, 0.5f);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(16, 16), 0xFF808080u, 2), "fog: %08x", px(16, 16));
}

static void test_shaders(void)
{
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, VP);
    /* vs.1.1: m4x4 oPos, v0, c0 / mov oD0, c4 / mov oT0, v7 */
    static const uint32_t vs[] = {
        0xFFFE0101u,
        20, 0x80000000u | (4u << 28) | (0xFu << 16), 0x80000000u | (1u << 28) | (0xE4u << 16), 0x80000000u | (2u << 28) | (0xE4u << 16),
        1, 0x80000000u | (5u << 28) | (0xFu << 16), 0x80000000u | (2u << 28) | (0xE4u << 16) | 4,
        1, 0x80000000u | (6u << 28) | (0xFu << 16), 0x80000000u | (1u << 28) | (0xE4u << 16) | 7,
        0x0000FFFFu,
    };
    /* ps.1.1: tex t0 / mul r0, t0, v0 */
    static const uint32_t ps[] = {
        0xFFFF0101u,
        66, 0x80000000u | (3u << 28) | (0xFu << 16),
        5, 0x80000000u | (0xFu << 16), 0x80000000u | (3u << 28) | (0xE4u << 16), 0x80000000u | (1u << 28) | (0xE4u << 16),
        0x0000FFFFu,
    };
    GfxDraw d;
    defaults(&d);
    d.vs.prog = 1, d.fs.prog = 1, d.vs_tokens = vs, d.ps_tokens = ps;
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d.vs.ntex = 8;
    d.u.stride[0] = 20, d.u.offset[7] = 12;
    identity(&d.u.vsc[0][0]); /* c0..c3 */
    d.u.vsc[4][0] = 1, d.u.vsc[4][1] = 0.5f, d.u.vsc[4][2] = 0, d.u.vsc[4][3] = 1;
    uint32_t white = 0xFFFFFFFFu;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
    gfx_tex_upload(t, 0, 0, &white, 4);
    d.tex[0] = t;
    d.fs.st[0].tex = 1;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0 };
    float vert[4][5] = { { -1, 1, 0.5f, 0, 0 }, { 1, 1, 0.5f, 1, 0 }, { -1, -1, 0.5f, 0, 1 }, { 1, -1, 0.5f, 1, 1 } };
    d.data[0] = vert, d.size[0] = sizeof vert;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
    readback();
    CHECK(near(px(16, 16), 0xFFFF8000u, 2), "vs/ps 1.1: %08x", px(16, 16));
    gfx_tex_destroy(t);
}

/* every texture op and argument form, fog mode, light type and coordinate source: all must build */
static void test_sweep(void)
{
    uint32_t before = gfx_failures();
    GfxDraw d;
    UiVert v[4];
    quad(v, 0, 0, 1, 1, 0xFFFFFFFFu, 0.5f);
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, 21, 1, 1, 1, GFX_USE_SAMPLE);
    GfxTex* cube = gfx_tex_create(GFX_TEX_CUBE, 21, 1, 1, 1, GFX_USE_SAMPLE);
    for (int op = 1; op <= 26; ++op)
        for (int form = 0; form < 3; ++form)
        {
            defaults(&d);
            layout_ui(&d);
            int mod = form == 1 ? 0x10 : form == 2 ? 0x20 : 0;
            d.fs.nstages = 2;
            d.fs.st[0] = (GfxStage){ (uint8_t)op, (uint8_t)(2 | mod), (uint8_t)(0 | mod), 3, (uint8_t)(op > 1 ? op : 2), 2, 0, 4, 1, 1, 0, 2 };
            d.fs.st[1] = (GfxStage){ (uint8_t)op, 1, (uint8_t)(5 | mod), 4, 1, 1, 1, 1, 5, 2, 0, 3 };
            d.tex[0] = t, d.tex[1] = cube;
            d.fs.specular_add = (uint8_t)(op & 1);
            d.fs.fog = (uint8_t)(op % 5);
            d.fs.alpha_func = (uint8_t)(op % 9);
            d.vs.ntex = 2;
            draw_ui(&d, v);
        }
    for (int lt = 1; lt <= 3; ++lt)
        for (int gen = 0; gen < 4; ++gen)
        {
            defaults(&d);
            float vert[3][8] = { { 0 } };
            d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
            d.vs.el[3] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
            d.vs.el[7] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
            d.u.stride[0] = 32, d.u.offset[3] = 12, d.u.offset[7] = 24;
            d.vs.lighting = 1, d.vs.specular = 1, d.vs.localviewer = (uint8_t)(gen & 1);
            d.vs.nlights = 3, d.vs.light_type[0] = (uint8_t)lt, d.vs.light_type[1] = 3, d.vs.light_type[2] = 1;
            d.vs.src_diffuse = (uint8_t)(gen % 3);
            d.vs.fog_vertex = (uint8_t)gen, d.vs.range_fog = (uint8_t)(lt & 1);
            d.vs.ntex = 2, d.vs.tci[0] = (uint8_t)(gen << 4), d.vs.ttf[0] = (uint8_t)(gen ? 3 : 2), d.vs.tci[1] = 0, d.vs.ttf[1] = 0x82;
            d.fs.nstages = 2;
            d.fs.st[1] = (GfxStage){ 4, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1, 3 };
            d.tex[0] = d.tex[1] = t;
            d.fs.st[0].tex = 1;
            d.fs.fog = (uint8_t)(gen ? 4 : 0);
            d.vs.flat = d.fs.flat = (uint8_t)(lt == 2);
            d.data[0] = vert, d.size[0] = sizeof vert;
            d.prim = GFX_TRIANGLEFAN, d.count = 1;
            gfx_draw(&d);
        }
    gfx_finish();
    gfx_tex_destroy(t);
    gfx_tex_destroy(cube);
    CHECK(gfx_failures() == before, "%u keys failed to build", gfx_failures() - before);
}

/* frames to a window: a back buffer the size of the window, cleared and drawn, presented */
#ifndef VANACORE
static void run_window(SDL_Window* win)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    GfxTex* bb = gfx_tex_create(GFX_TEX_2D, 22, (uint32_t)ww, (uint32_t)wh, 1, GFX_USE_RT);
    uint32_t vp[6] = { 0, 0, (uint32_t)ww, (uint32_t)wh, 0, 0x3F800000u };
    gfx_set_targets(bb, 0, 0, NULL);
    Uint64 end = SDL_GetTicks() + 2000;
    for (int frame = 0; SDL_GetTicks() < end; ++frame)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            ;
        gfx_clear(0, NULL, 1, 0xFF203040u, 1.0f, 0, vp);
        GfxDraw d;
        defaults(&d);
        layout_ui(&d);
        d.u.vp[2] = (float)ww, d.u.vp[3] = (float)wh;
        memcpy(d.vp, vp, sizeof vp);
        d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
        float x = (float)(frame % 120) / 120.0f * (float)(ww - 64);
        UiVert v[4];
        quad(v, x, (float)wh / 2 - 32, x + 64, (float)wh / 2 + 32, 0xFFFFA000u, 0.5f);
        draw_ui(&d, v);
        gfx_present(bb);
    }
    gfx_tex_destroy(bb);
}
#endif

/* --- the scene effects (gfx_scene_done, fx on) ---------------------------------------------------------
 * A 128x128 scene through a 90-degree perspective camera, left- or right-handed (FFXI's is
 * right-handed: the scene is at negative z), made of flat-colored quads; each test turns on the one
 * effect it checks. */
enum { SS = 128 };
static GfxTex *g_srt, *g_sds;
static float g_sproj[16], g_sz;
static uint32_t g_spx[SS * SS];
static const uint32_t SVP[6] = { 0, 0, SS, SS, 0, 0x3F800000u };

static void fx_only(const char* on, float v)
{
    static const char* const all[] = { "ao", "fog", "bloom", "rays", "grade", "sun", "outline", "bands",
        "smooth", "dof" };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; ++i)
        gfx_fx_set(all[i], 0.0f);
    /* the sun's shadows as these tests were written for (the defaults before the player's own became them):
     * casters nearer than 1 ignored, soft edges, reach 40, sharp to 15, faces turned away unshaded, the zone smoothed by 0.6 */
    gfx_fx_set("sun_min", 1.0f);
    gfx_fx_set("sun_face", 0.0f);
    gfx_fx_set("sun_soft_zone", 0.6f);
    gfx_fx_set("sun_soft", 0.03f);
    gfx_fx_set("sun_distance", 40.0f);
    gfx_fx_set("sun_near", 15.0f);
    gfx_fx_set("sun_direct", 0.3f);
    /* the maps drawn whole every frame: the still layers (sun_far_hold, sun_near_hold) keep a frame's casters for a few frames, and
     * these tests change them from one frame to the next. And the occlusion's reach as it was before it started at 2.0 */
    gfx_fx_set("sun_far_hold", 0.0f);
    gfx_fx_set("sun_near_hold", 0.0f);
    gfx_fx_set("radius", 1.0f);
    gfx_fx_set("sun_mid", 0.0f); /* no middle map (its default; test_scene_sun_mid turns it on) */
    if (on)
        gfx_fx_set(on, v);
}

static void scene_begin(int rh, uint32_t clear)
{
    if (!g_srt)
        g_srt = gfx_tex_create(GFX_TEX_2D, 21, SS, SS, 1, GFX_USE_RT), g_sds = gfx_tex_create(GFX_TEX_2D, 75, SS, SS, 1, GFX_USE_DEPTH);
    gfx_set_targets(g_srt, 0, 0, g_sds);
    gfx_clear(0, NULL, 3, clear, 1.0f, 0, SVP);
    /* PerspectiveFovLH / RH: 90 degrees, square, near 0.5, far 100 */
    float zn = 0.5f, zf = 100.0f;
    g_sz = rh ? -1.0f : 1.0f;
    memset(g_sproj, 0, sizeof g_sproj);
    g_sproj[0] = g_sproj[5] = 1.0f, g_sproj[10] = g_sz * zf / (zf - zn), g_sproj[11] = g_sz, g_sproj[14] = -zn * zf / (zf - zn);
}

/* scene_quad's objects stand here (GfxDraw.anchor) while anchor[3] is 1: placed objects, as a tree is */
static float g_quad_anchor[4];

/* a quad (strip order) in view space, z given as a distance in front of the camera, of one color */
static void scene_quad(float q[4][3], uint32_t color)
{
    GfxDraw d;
    defaults(&d);
    memcpy(d.u.wvp, g_sproj, 64);
    d.u.vp[2] = d.u.vp[3] = SS;
    memcpy(d.vp, SVP, sizeof SVP);
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 12;
    d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 }; /* SELECTARG1(TFACTOR) */
    d.u.tfactor[0] = ((color >> 16) & 255) / 255.0f, d.u.tfactor[1] = ((color >> 8) & 255) / 255.0f;
    d.u.tfactor[2] = (color & 255) / 255.0f, d.u.tfactor[3] = 1.0f;
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    d.caster = 1;
    float v[4][3];
    for (int i = 0; i < 4; ++i)
        v[i][0] = q[i][0], v[i][1] = q[i][1], v[i][2] = q[i][2] * g_sz;
    d.data[0] = v, d.size[0] = sizeof v;
    d.cpu[0] = v, d.cpu_size[0] = sizeof v;
    memcpy(d.anchor, g_quad_anchor, sizeof d.anchor);
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
}

/* the scene done, with a directional light toward sun (view space, left-handed; NULL for none) and
 * a fog color; its pixels read back */
static void scene_end(const float* sun, uint32_t fog)
{
    GfxScene sc;
    memset(&sc, 0, sizeof sc);
    memcpy(sc.proj, g_sproj, 64);
    identity(sc.view);
    memcpy(sc.vp, SVP, sizeof SVP);
    if (sun)
    {
        float l = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        sc.sun_dir[0] = sun[0] / l, sc.sun_dir[1] = sun[1] / l, sc.sun_dir[2] = sun[2] / l * g_sz, sc.sun_dir[3] = 1;
        sc.sun_color[0] = sc.sun_color[1] = sc.sun_color[2] = 1;
    }
    sc.fog[2] = 1.0f; /* the game fogged its world */
    sc.fogcolor[0] = ((fog >> 16) & 255) / 255.0f, sc.fogcolor[1] = ((fog >> 8) & 255) / 255.0f, sc.fogcolor[2] = (fog & 255) / 255.0f;
    gfx_scene_done(g_srt, &sc);
    gfx_tex_read(g_srt, 0, 0, g_spx, SS * 4);
}

static uint32_t spx(int x, int y, int shift) { return (g_spx[y * SS + x] >> shift) & 255; }

/* a wall 6 ahead and a floor 3 below meeting it (at row 96) */
static void wall_and_floor(uint32_t color)
{
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -3, 6 }, { 8, -3, 6 } };
    float floor[4][3] = { { -8, -3, 6 }, { 8, -3, 6 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    scene_quad(wall, color);
    scene_quad(floor, color);
}

/* Occlusion darkens the wall where it meets the floor and leaves the open wall as it was. */
static void test_scene_ao(int rh)
{
    /* each quality (32, 24, 16, 10 and 6 taps a pixel), one frame of it alone */
    for (int q = 4; q >= 0; --q)
    {
        fx_only("ao", 0.8f);
        gfx_fx_set("ao_quality", (float)q);
        gfx_fx_set("temporal", 0.0f);
        scene_begin(rh, 0xFF000000u);
        wall_and_floor(0xFF909090u); /* grey: near white is light, which the occlusion leaves (fx_comp) */
        scene_end(NULL, 0);
        uint32_t open = spx(64, 30, 0), corner = spx(64, 94, 0);
        CHECK(open >= 138, "occlusion (%s, quality %d): open wall %u (want about 144)", rh ? "RH" : "LH", q, open);
        CHECK(corner + 10 <= open, "occlusion (%s, quality %d): wall at the floor %u, open %u (want darker)", rh ? "RH" : "LH", q,
            corner, open);
        gfx_present(NULL);
    }
    gfx_fx_set("ao_quality", 0.0f);
    gfx_fx_set("temporal", 0.85f);
}

/* A block floating 0.7 before a wall (inside the occlusion's reach, but far nearer the camera than
 * the wall) leaves no halo on it: it hides the wall, it does not shade it. */
static void test_scene_ao_halo(void)
{
    fx_only("ao", 1.0f);
    scene_begin(1, 0xFF000000u);
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    float block[4][3] = { { -0.5f, 0.5f, 5.3f }, { 0.5f, 0.5f, 5.3f }, { -0.5f, -0.5f, 5.3f }, { 0.5f, -0.5f, 5.3f } };
    scene_quad(wall, 0xFF909090u);
    scene_quad(block, 0xFF909090u);
    scene_end(NULL, 0);
    /* the block spans 64 +- 6 pixels; the wall 3 pixels past its edge */
    uint32_t beside = spx(73, 64, 0), below = spx(64, 73, 0);
    CHECK(beside >= 138 && below >= 138, "occlusion: wall around a floating block %u %u (want no halo)", beside, below);
}

/* Height fog: the far wall takes more of the fog's color than the floor just ahead; with height
 * falloff, the floor (below the camera) more than the wall's top at the same distance. */
static void test_scene_fog(void)
{
    fx_only("fog", 0.08f);
    gfx_fx_set("fog_falloff", 0.0f), gfx_fx_set("fog_max", 1.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFFFFFFFFu);
    scene_end(NULL, 0xFFFF0000u);
    uint32_t far_g = spx(64, 40, 8), near_g = spx(64, 126, 8), far_r = spx(64, 40, 16);
    CHECK(far_r >= 250 && far_g <= 200, "fog: far wall %u/%u (want red)", far_r, far_g);
    CHECK(near_g >= far_g + 25, "fog: near floor green %u, far wall %u (want less fog near)", near_g, far_g);
    /* falloff: more fog below the camera than above it, at the wall */
    gfx_fx_set("fog_falloff", 0.5f), gfx_fx_set("fog_height", 0.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFFFFFFFFu);
    scene_end(NULL, 0xFFFF0000u);
    uint32_t high = spx(64, 20, 8), low = spx(64, 90, 8);
    CHECK(low + 20 < high, "fog: below the camera %u, above %u (want more fog below)", low, high);
    /* from one frame to the next the fog's color eases: red, then blue is still mostly red */
    gfx_fx_set("fog_falloff", 0.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFF000000u);
    scene_end(NULL, 0xFFFF0000u);
    gfx_present(NULL);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFF000000u);
    scene_end(NULL, 0xFF0000FFu);
    uint32_t r = spx(64, 40, 16), b = spx(64, 40, 0);
    CHECK(r > 2 * b, "fog: a new color the next frame %u red, %u blue (want it eased: mostly red)", r, b);
    gfx_fx_set("fog_falloff", 0.08f), gfx_fx_set("fog_height", 2.0f), gfx_fx_set("fog_max", 0.5f);
}

/* Zone lines: a box between the camera and a black wall tints the wall red through it, strongest low in the
 * box; nothing beside it, nor through a box behind the wall, nor with none set; and a box given deep along its own z
 * and turned a quarter (as West Ronfaure's come) the same as the first. */
static void test_scene_zonelines(void)
{
    uint32_t px[4][3];
    for (int k = 0; k < 4; ++k)
    {
        fx_only(NULL, 0.0f);
        scene_begin(1, 0xFF000000u);
        float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
        scene_quad(wall, 0xFF000000u);
        GfxScene sc;
        memset(&sc, 0, sizeof sc);
        memcpy(sc.proj, g_sproj, 64);
        identity(sc.view);
        memcpy(sc.vp, SVP, sizeof SVP);
        /* 4 wide, 4 tall and 1 deep, 3 ahead (2.5 to 3.5), its middle 1 below the camera; the second 8 ahead,
         * past the wall */
        const float box[7] = { 0.0f, -1.0f, (k == 1 ? 8.0f : 3.0f) * g_sz, 0.0f, 4.0f, 4.0f, 1.0f };
        const float turned[7] = { 0.0f, -1.0f, 3.0f * g_sz, 1.5707963f, 1.0f, 4.0f, 4.0f };
        if (k < 2)
            memcpy(sc.zonelines[0], box, sizeof box), sc.nzonelines = 1;
        if (k == 3)
            memcpy(sc.zonelines[0], turned, sizeof turned), sc.nzonelines = 1;
        sc.zoneline_color[0] = 1.0f, sc.zoneline_color[3] = 1.0f;
        gfx_scene_done(g_srt, &sc);
        gfx_tex_read(g_srt, 0, 0, g_spx, SS * 4);
        /* low in the box (its near face at row 106, 1.67 below), at its top edge (row 64, 1 above its middle),
         * beside it */
        px[k][0] = spx(64, 104, 16), px[k][1] = spx(64, 64, 16), px[k][2] = spx(124, 104, 16);
    }
    CHECK(px[0][0] >= 60, "zone lines: low in the box %u red (want the wall tinted)", px[0][0]);
    CHECK(px[0][1] < px[0][0], "zone lines: higher in the box %u red, low %u (want it fading up)", px[0][1], px[0][0]);
    CHECK(px[0][2] <= 8, "zone lines: beside the box %u red (want the black wall)", px[0][2]);
    CHECK(px[1][0] <= 8, "zone lines: a box behind the wall %u red (want none)", px[1][0]);
    CHECK(px[2][0] <= 8, "zone lines: none set %u red (want none)", px[2][0]);
    CHECK(px[3][0] + 12 >= px[0][0] && px[3][0] <= px[0][0] + 12 && px[3][2] <= 8,
        "zone lines: deep along its z, turned a quarter: low %u, beside %u (want as the first: %u, black)", px[3][0], px[3][2],
        px[0][0]);
}

static float g_zl_fade; /* zoneline_scene's lines' ends fade over this */
static float g_zl_ground; /* ... and the ground is this far above the line's middle (0: at it) */

/* the black floor 3 below (from 0.6 to 6 ahead) and wall 6 ahead, with one zone line of `style`, red */
static void zoneline_scene(const float box[7], uint32_t style, int glow)
{
    fx_only(NULL, 0.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFF000000u);
    GfxScene sc;
    memset(&sc, 0, sizeof sc);
    memcpy(sc.proj, g_sproj, 64);
    identity(sc.view);
    memcpy(sc.vp, SVP, sizeof SVP);
    memcpy(sc.zonelines[0], box, sizeof(float) * 7), sc.nzonelines = 1;
    sc.zonelines[0][7] = g_zl_ground;
    sc.zoneline_color[0] = 1.0f, sc.zoneline_color[3] = 1.0f;
    sc.zoneline_style = style, sc.zoneline_glow = (uint32_t)glow, sc.zoneline_height = 1.0f, sc.zoneline_size = 1.0f,
    sc.zoneline_fade = g_zl_fade;
    gfx_scene_done(g_srt, &sc);
    gfx_tex_read(g_srt, 0, 0, g_spx, SS * 4);
}

/* Zone lines' styles: a line floating across the box a unit above its middle (4 ahead, 2 below the camera: row
 * 96), none on the floor under it nor past its ends; that line dotted, lit and dark along it; sparkles on the
 * box's middle within its width, none beside it; the curtain's glow past its sides. */
static void test_scene_zoneline_styles(void)
{
    const float on_floor[7] = { 0.0f, -3.0f, 4.0f * g_sz, 0.0f, 4.0f, 4.0f, 1.0f };
    zoneline_scene(on_floor, 1, 0);
    uint32_t line = spx(64, 96, 16), off = spx(64, 112, 16), side = spx(120, 96, 16);
    CHECK(line >= 150, "zone lines (line): on the line %u red (want it lit)", line);
    CHECK(off <= 8 && side <= 8, "zone lines (line): the floor under it %u, past its end %u red (want black)", off, side);
    /* its ends faded over 1.5: 0.2 from its end (column 93) dimmer than its middle */
    g_zl_fade = 1.5f;
    zoneline_scene(on_floor, 1, 0);
    uint32_t mid = spx(64, 96, 16), end = spx(93, 96, 16);
    g_zl_fade = 0.0f;
    CHECK(mid >= 150 && end + 60 <= mid, "zone lines (fade): the line's middle %u red, near its end %u (want it faded)", mid, end);
    zoneline_scene(on_floor, 2, 0);
    uint32_t lit = 0, dark = 255;
    for (int x = 40; x < 88; ++x)
    {
        uint32_t v = spx(x, 96, 16);
        lit = v > lit ? v : lit, dark = v < dark ? v : dark;
    }
    CHECK(lit >= 150 && dark <= 40, "zone lines (dots): along the line from %u to %u red (want dots and gaps)", dark, lit);

    /* dots on the floor: a box 4 ahead whose middle is 3 below the floor (3 below the camera) and 10 tall, so the floor
     * is within it (Bastok Markets'): a floating line a unit above its middle is under the floor, hidden; the floor's
     * dots lie on it at 4 ahead (row 112), lit and dark along it */
    const float low[7] = { 0.0f, -6.0f, 4.0f * g_sz, 0.0f, 4.0f, 10.0f, 1.0f };
    zoneline_scene(low, 2, 0);
    uint32_t hidden = 0;
    for (int x = 40; x < 88; ++x)
        for (int y = 108; y < 116; ++y)
        {
            uint32_t v = spx(x, y, 16);
            hidden = v > hidden ? v : hidden;
        }
    zoneline_scene(low, 4, 0);
    uint32_t floor_lit = 0, floor_dark = 255;
    for (int x = 40; x < 88; ++x)
    {
        uint32_t v = 0;
        for (int y = 110; y < 115; ++y)
            v = spx(x, y, 16) > v ? spx(x, y, 16) : v;
        floor_lit = v > floor_lit ? v : floor_lit, floor_dark = v < floor_dark ? v : floor_dark;
    }
    /* the ground given (3 above the box's middle: the floor): the line (and the dots) a unit over it, row 96, as a box
     * whose middle is at the floor; none of it where the ground isn't given (the line under the floor, hidden) */
    g_zl_ground = 3.0f;
    zoneline_scene(low, 1, 0);
    uint32_t grounded = spx(64, 96, 16);
    zoneline_scene(low, 2, 0);
    uint32_t grounded_dots = 0;
    for (int x = 40; x < 88; ++x)
        grounded_dots = spx(x, 96, 16) > grounded_dots ? spx(x, 96, 16) : grounded_dots;
    g_zl_ground = 0.0f;
    CHECK(grounded >= 150 && grounded_dots >= 150, "zone lines (ground): a unit over the ground the line %u, a dot %u red (want them lit)",
        grounded, grounded_dots);
    CHECK(hidden <= 8, "zone lines (floor): the floating line under the floor %u red (want it hidden)", hidden);
    CHECK(floor_lit >= 150 && floor_dark <= 60, "zone lines (floor): dots on the floor from %u to %u red (want dots and gaps)",
        floor_dark, floor_lit);
    /* ... but none on a wall within the box (it isn't facing up): a box at the wall, 6 ahead; the rows above the
     * floor's meeting it (row 96) */
    const float at_wall[7] = { 0.0f, -1.0f, 6.0f * g_sz, 0.0f, 4.0f, 10.0f, 1.0f };
    zoneline_scene(at_wall, 4, 0);
    uint32_t on_wall = 0;
    for (int y = 30; y < 90; ++y)
        for (int x = 40; x < 88; ++x)
            on_wall = spx(x, y, 16) > on_wall ? spx(x, y, 16) : on_wall;
    CHECK(on_wall <= 8, "zone lines (floor): the wall inside the box %u red (want none: it isn't a floor)", on_wall);

    /* the box 4 ahead, its middle at the camera's height, 6 wide and 6 tall: x and y within 48 pixels of the
     * middle (rows and columns 16 to 112); the sparkles' sum over it, and over the columns past it */
    const float ahead[7] = { 0.0f, 0.0f, 4.0f * g_sz, 0.0f, 6.0f, 6.0f, 1.0f };
    zoneline_scene(ahead, 3, 1);
    uint32_t in = 0, out = 0;
    for (int y = 16; y < 112; ++y)
    {
        for (int x = 24; x < 104; ++x)
            in += spx(x, y, 16);
        for (int x = 0; x < 6; ++x)
            out += spx(x, y, 16) + spx(127 - x, y, 16);
    }
    CHECK(in >= 2000, "zone lines (stars): sparkles in the box sum %u red (want some)", in);
    CHECK(out <= 200, "zone lines (stars): past its sides %u red (want next to none)", out);

    /* the curtain 3 ahead, 4 wide: with its glow, the wall just past its side (column 120: 0.65 out at its middle) */
    const float curtain[7] = { 0.0f, 0.0f, 3.0f * g_sz, 0.0f, 4.0f, 4.0f, 1.0f };
    zoneline_scene(curtain, 0, 0);
    uint32_t bare = spx(120, 60, 16);
    zoneline_scene(curtain, 0, 1);
    uint32_t glowed = spx(120, 60, 16);
    CHECK(bare <= 8 && glowed >= bare + 30, "zone lines (glow): past the curtain's side %u red, with its glow %u", bare, glowed);
}

#ifdef VANACORE
/* kernel32's, without windows.h (whose near and far macros these tests' names would meet) */
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);

/* DLSS, through VanaCore.Dlss.exe from this build (build\<config>\dlss): the scene drawn at 128x128 and shown at
 * 192x192, its left half red and its right half blue; once DLSS has had some frames, a draw reading the scene's
 * target reads DLSS's output - at 192 across, red on the left and blue on the right. Skipped (said so) where the
 * helper isn't built, or DLSS isn't on this machine (Direct3D 11: never). */
static void test_scene_dlss(void)
{
#ifdef _DEBUG
    const wchar_t* helper = L"build\\Debug\\dlss\\VanaCore.Dlss.exe";
#else
    const wchar_t* helper = L"build\\Release\\dlss\\VanaCore.Dlss.exe";
#endif
    FILE* f = _wfopen(helper, L"rb");
    if (!f)
    {
        fprintf(stderr, "  dlss: skipped (%ls isn't built)\n", helper);
        return;
    }
    fclose(f);
    fx_only(NULL, 0.0f);
    gfx_dlss_set(2, helper, L"build\\dlss_test.log");
    GfxTex* shown = gfx_tex_create(GFX_TEX_2D, 21, 192, 192, 1, GFX_USE_RT);
    static uint32_t px[192 * 192];
    static const uint32_t svp[6] = { 0, 0, 192, 192, 0, 0x3F800000u };
    char msg[300] = "";
    int state = 0, after = 0;
    for (int frame = 0; frame < 2000 && after < 30; ++frame)
    {
        scene_begin(1, 0xFF000000u);
        float left[4][3] = { { -8, 8, 6 }, { 0, 8, 6 }, { -8, -8, 6 }, { 0, -8, 6 } };
        float right[4][3] = { { 0, 8, 6 }, { 8, 8, 6 }, { 0, -8, 6 }, { 8, -8, 6 } };
        scene_quad(left, 0xFFFF0000u);
        scene_quad(right, 0xFF0000FFu);
        GfxScene sc;
        memset(&sc, 0, sizeof sc);
        memcpy(sc.proj, g_sproj, 64);
        identity(sc.view);
        memcpy(sc.vp, SVP, sizeof SVP);
        sc.present_w = sc.present_h = 192;
        gfx_scene_done(g_srt, &sc);
        /* drawn into the scene after its effects, as the game draws spells and names: in DLSS's picture too */
        static const int32_t late[4] = { 56, 56, 72, 72 };
        gfx_set_targets(g_srt, 0, 0, g_sds);
        gfx_clear(1, late, 1, 0xFF00FF00u, 1.0f, 0, SVP);
        /* the interface drawn into it after that (a name): over DLSS's output, at its size - a white box at 100..120
         * across, 20..40 down of the scene's 128 (150..180, 30..60 of the 192 shown) */
        {
            GfxDraw o;
            defaults(&o);
            layout_ui(&o);
            memcpy(o.vp, SVP, sizeof SVP);
            o.u.vp[2] = o.u.vp[3] = (float)SS;
            o.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 }; /* diffuse only */
            o.overlay = 1;
            UiVert ov[4];
            quad(ov, 100, 20, 120, 40, 0xFFFFFFFFu, 0.5f);
            draw_ui(&o, ov);
        }
        /* more of the world after that (the game's glares and spells): in DLSS's picture, the interface still over
         * its output - a yellow square up and to the left, 26..61 across and 9..44 down of the 192 shown */
        {
            float late3d[4][3] = { { -4, 5, 5.5f }, { -2, 5, 5.5f }, { -4, 3, 5.5f }, { -2, 3, 5.5f } };
            scene_quad(late3d, 0xFFFFFF00u);
        }
        /* the scene's target onto `shown`, as the game lays its scene onto the back buffer */
        gfx_set_targets(shown, 0, 0, NULL);
        gfx_clear(0, NULL, 1, 0xFF00FF00u, 1.0f, 0, svp);
        GfxDraw d;
        defaults(&d);
        layout_ui(&d);
        memcpy(d.vp, svp, sizeof svp);
        d.u.vp[2] = d.u.vp[3] = 192;
        d.tex[0] = g_srt;
        d.fs.st[0] = (GfxStage){ 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2 }; /* SELECTARG1(TEXTURE), a 2D texture */
        UiVert v[4];
        quad(v, 0, 0, 192, 192, 0xFFFFFFFFu, 0.5f);
        draw_ui(&d, v);
        gfx_tex_read(shown, 0, 0, px, 192 * 4);
        gfx_present(NULL);
        state = gfx_dlss_state(msg, sizeof msg);
        if (state >= 3 || state == 0)
            break;
        if (state == 2)
            after++;
        else
            Sleep(5);
    }
    gfx_dlss_set(-1, NULL, NULL);
    gfx_tex_destroy(shown);
    if (state != 2)
    {
        fprintf(stderr, "  dlss: skipped (state %d: %s)\n", state, msg);
        return;
    }
    uint32_t l = px[96 * 192 + 40], r = px[96 * 192 + 150], mid = px[96 * 192 + 96];
    fprintf(stderr, "  dlss: %s; left %08x, right %08x, middle %08x\n", msg, l, r, mid);
    CHECK(((mid >> 8) & 255) >= 200 && ((mid >> 16) & 255) <= 60, "dlss: the square drawn after the effects %08x (want green)", mid);
    uint32_t name = px[45 * 192 + 165], beside = px[45 * 192 + 140];
    CHECK((name & 0xFFFFFF) == 0xFFFFFF, "dlss: the interface over DLSS's output %08x (want white)", name);
    CHECK(((beside >> 16) & 255) <= 60 && (beside & 255) >= 200, "dlss: beside it %08x (want the blue scene)", beside);
    CHECK(((l >> 16) & 255) >= 200 && (l & 255) <= 40, "dlss: the left %08x (want red)", l);
    uint32_t late3d = px[26 * 192 + 44];
    CHECK(((late3d >> 16) & 255) >= 200 && ((late3d >> 8) & 255) >= 200 && (late3d & 255) <= 60,
        "dlss: the world drawn after the interface %08x (want yellow)", late3d);
    CHECK((r & 255) >= 200 && ((r >> 16) & 255) <= 40, "dlss: the right %08x (want blue)", r);
    CHECK(!strcmp(msg, "Game 128x128 -> DLSS 192x192"), "dlss: %s (want its sizes: DLSS running)", msg);
}

/* DLSS 5 before DLSS (NVIDIA's nvngx_dlssnr.dll beside the helper: skipped, said so, where it isn't, or NGX doesn't
 * give it): the DLSS test's scene with dlss5 on, then DLSS made again at another quality while DLSS 5 runs - as
 * changing DLSS's size in the game does - which DLSS 5 has to come through still running. Its network redraws the
 * picture, so the colours are near the scene's, not them: red on the left, blue on the right. */
static void test_scene_dlss5(void)
{
#ifdef _DEBUG
    const wchar_t* helper = L"build\\Debug\\dlss\\VanaCore.Dlss.exe";
    const wchar_t* dll = L"build\\Debug\\dlss\\nvngx_dlssnr.dll";
#else
    const wchar_t* helper = L"build\\Release\\dlss\\VanaCore.Dlss.exe";
    const wchar_t* dll = L"build\\Release\\dlss\\nvngx_dlssnr.dll";
#endif
    FILE* f = _wfopen(dll, L"rb");
    if (!f)
    {
        fprintf(stderr, "  dlss5: skipped (%ls isn't there)\n", dll);
        return;
    }
    fclose(f);
    fx_only(NULL, 0.0f);
    /* DLSS 5 on its own, as this test was written for: its own mask, the effects as lights and the glow after it off */
    gfx_fx_set("dlss5_mask", 0.0f);
    gfx_fx_set("dlss5_fx_light", 0.0f);
    gfx_fx_set("dlss5_elight_post", 0.0f);
    gfx_fx_set("dlss5", 1.0f);
    gfx_dlss_set(2, helper, L"build\\dlss_test.log");
    GfxTex* shown = gfx_tex_create(GFX_TEX_2D, 21, 192, 192, 1, GFX_USE_RT);
    static uint32_t px[192 * 192];
    static const uint32_t svp[6] = { 0, 0, 192, 192, 0, 0x3F800000u };
    char msg[300] = "", first[300] = "";
    int state = 0, after = 0, remade = 0;
    for (int frame = 0; frame < 4000 && after < 30; ++frame)
    {
        scene_begin(1, 0xFF000000u);
        float left[4][3] = { { -8, 8, 6 }, { 0, 8, 6 }, { -8, -8, 6 }, { 0, -8, 6 } };
        float right[4][3] = { { 0, 8, 6 }, { 8, 8, 6 }, { 0, -8, 6 }, { 8, -8, 6 } };
        scene_quad(left, 0xFFFF0000u);
        scene_quad(right, 0xFF0000FFu);
        GfxScene sc;
        memset(&sc, 0, sizeof sc);
        memcpy(sc.proj, g_sproj, 64);
        identity(sc.view);
        memcpy(sc.vp, SVP, sizeof SVP);
        sc.present_w = sc.present_h = 192;
        gfx_scene_done(g_srt, &sc);
        /* the scene's target onto `shown`, as the game lays its scene onto the back buffer */
        gfx_set_targets(shown, 0, 0, NULL);
        gfx_clear(0, NULL, 1, 0xFF00FF00u, 1.0f, 0, svp);
        GfxDraw d;
        defaults(&d);
        layout_ui(&d);
        memcpy(d.vp, svp, sizeof svp);
        d.u.vp[2] = d.u.vp[3] = 192;
        d.tex[0] = g_srt;
        d.fs.st[0] = (GfxStage){ 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2 }; /* SELECTARG1(TEXTURE), a 2D texture */
        UiVert v[4];
        quad(v, 0, 0, 192, 192, 0xFFFFFFFFu, 0.5f);
        draw_ui(&d, v);
        gfx_tex_read(shown, 0, 0, px, 192 * 4);
        gfx_present(NULL);
        state = gfx_dlss_state(msg, sizeof msg);
        if (state >= 3 || state == 0)
            break;
        if (state == 2)
            after++;
        else
            Sleep(5);
        if (after == 30 && !remade)
        {
            /* DLSS made again, DLSS 5 running */
            remade = 1, after = 0;
            memcpy(first, msg, sizeof first);
            gfx_dlss_set(1, helper, L"build\\dlss_test.log");
        }
    }
    gfx_dlss_set(-1, NULL, NULL);
    gfx_fx_set("dlss5", 0.0f);
    gfx_tex_destroy(shown);
    if (!remade || !strstr(first, "-> DLSS 5 "))
    {
        fprintf(stderr, "  dlss5: skipped (state %d: %s)\n", state, remade ? first : msg);
        return;
    }
    CHECK(state == 2, "dlss5: DLSS stopped when it was made again with DLSS 5 running (state %d: %s)", state, msg);
    if (state != 2)
        return;
    uint32_t l = px[96 * 192 + 40], r = px[96 * 192 + 150];
    fprintf(stderr, "  dlss5: %s; left %08x, right %08x\n", msg, l, r);
    CHECK(!strcmp(msg, "Game 128x128 -> DLSS 5 128x128 -> DLSS 192x192"), "dlss5: %s (want DLSS 5 running still, DLSS made again)", msg);
    CHECK(((l >> 16) & 255) >= 150 && (l & 255) <= 90, "dlss5: the left %08x (want red)", l);
    CHECK((r & 255) >= 150 && ((r >> 16) & 255) <= 90, "dlss5: the right %08x (want blue)", r);
}

/* DLSS frame generation (VanaCore.GfxTest.exe framegen): frame generation shows its frames on a window's swap chain, so
 * this runs on its own with one (window, from main.c; pump its messages). The DLSS test's scene, its square moving
 * across, laid onto a 192x192 frame presented through 2x, then 4x (2x where the card makes fewer): each on (its state
 * 2) for 120 frames without failing, the helper's log saying it made frame generation and no frame failed; then off,
 * presented as usual. Skipped (said so) where the helper isn't built or frame generation isn't on this machine. */
static void framegen_frame(GfxTex* shown, int frame)
{
    static const uint32_t svp[6] = { 0, 0, 192, 192, 0, 0x3F800000u };
    scene_begin(1, 0xFF000000u);
    float left[4][3] = { { -8, 8, 6 }, { 0, 8, 6 }, { -8, -8, 6 }, { 0, -8, 6 } };
    float right[4][3] = { { 0, 8, 6 }, { 8, 8, 6 }, { 0, -8, 6 }, { 8, -8, 6 } };
    scene_quad(left, 0xFFFF0000u);
    scene_quad(right, 0xFF0000FFu);
    float x = (float)(frame % 60) / 60.0f * 8.0f - 4.0f;
    float square[4][3] = { { x - 1, 1, 5 }, { x + 1, 1, 5 }, { x - 1, -1, 5 }, { x + 1, -1, 5 } };
    scene_quad(square, 0xFF00FF00u);
    GfxScene sc;
    memset(&sc, 0, sizeof sc);
    memcpy(sc.proj, g_sproj, 64);
    identity(sc.view);
    memcpy(sc.vp, SVP, sizeof SVP);
    sc.present_w = sc.present_h = 192;
    gfx_scene_done(g_srt, &sc);
    /* the scene onto the frame, then the game's interface over it (a white box) */
    gfx_set_targets(shown, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xFF000000u, 1.0f, 0, svp);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    memcpy(d.vp, svp, sizeof svp);
    d.u.vp[2] = d.u.vp[3] = 192;
    d.tex[0] = g_srt;
    d.fs.st[0] = (GfxStage){ 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2 }; /* SELECTARG1(TEXTURE), a 2D texture */
    UiVert v[4];
    quad(v, 0, 0, 192, 192, 0xFFFFFFFFu, 0.5f);
    draw_ui(&d, v);
    GfxDraw o;
    defaults(&o);
    layout_ui(&o);
    memcpy(o.vp, svp, sizeof svp);
    o.u.vp[2] = o.u.vp[3] = 192;
    o.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 }; /* diffuse only */
    UiVert ov[4];
    quad(ov, 8, 8, 40, 24, 0xFFFFFFFFu, 0.5f);
    draw_ui(&o, ov);
}

/* the helper's log from `from` on, into text (NUL-terminated) */
static void read_log_from(const wchar_t* path, long from, char* text, size_t size)
{
    text[0] = 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f)
        return;
    fseek(f, from, SEEK_SET);
    size_t n = fread(text, 1, size - 1, f);
    text[n] = 0;
    fclose(f);
}

int gfx_test_framegen_main(void* window, void (*pump)(void))
{
    gfx_set_sync_pipelines(1);
    setenv("FFXI_FX", "1", 1);
    setenv("FFXI_FX_FILE", "/nonexistent/fx.txt", 1);
    if (!gfx_init(window, 0))
    {
        printf("no graphics back end\n");
        return 1;
    }
#ifdef _DEBUG
    const wchar_t* helper = L"build\\Debug\\dlss\\VanaCore.Dlss.exe";
#else
    const wchar_t* helper = L"build\\Release\\dlss\\VanaCore.Dlss.exe";
#endif
    const wchar_t* log = L"build\\dlss_test.log";
    FILE* f = _wfopen(helper, L"rb");
    if (!f)
    {
        printf("framegen: skipped (%ls isn't built)\n", helper);
        return 0;
    }
    fclose(f);
    long log_from = 0;
    if ((f = _wfopen(log, L"rb")) != NULL)
    {
        fseek(f, 0, SEEK_END);
        log_from = ftell(f);
        fclose(f);
    }
    fx_only(NULL, 0.0f);
    gfx_dlss_set(2, helper, log);
    GfxTex* shown = gfx_tex_create(GFX_TEX_2D, 21, 192, 192, 1, GFX_USE_RT);
    char msg[300] = "";
    int frame = 0, skipped = 0;
    for (int count = 1; count <= 3 && !skipped; count += 2)
    {
        gfx_dlss_framegen(count);
        int state = 0, on = 0;
        for (int tries = 0; tries < 3000 && on < 120; ++tries, ++frame)
        {
            if (pump)
                pump();
            framegen_frame(shown, frame);
            gfx_present(shown);
            state = gfx_dlss_framegen_state(msg, sizeof msg);
            if (state >= 3 || gfx_dlss_state(NULL, 0) >= 3)
                break;
            if (state == 2)
                on++;
            else
                Sleep(5);
        }
        printf("  framegen (asked %dx): state %d: %s\n", count + 1, state, msg);
        if (state == 3 || gfx_dlss_state(NULL, 0) >= 3)
        {
            printf("framegen: skipped (not on this machine)\n");
            skipped = 1;
            break;
        }
        CHECK(state == 2 && on >= 120, "framegen %dx: state %d after %d frames on: %s (want on)", count + 1, state, on, msg);
    }
    /* off: presented as before */
    gfx_dlss_framegen(0);
    for (int k = 0; k < 30; ++k, ++frame)
    {
        if (pump)
            pump();
        framegen_frame(shown, frame);
        gfx_present(shown);
    }
    CHECK(gfx_dlss_framegen_state(NULL, 0) == 0, "framegen: still on after it was turned off");
    gfx_dlss_set(-1, NULL, NULL);
    gfx_present(NULL);
    gfx_finish();
    gfx_tex_destroy(shown);
    if (!skipped)
    {
        static char text[1 << 16];
        Sleep(200); /* the helper's last lines written */
        read_log_from(log, log_from, text, sizeof text);
        CHECK(strstr(text, "frame generation at") != NULL, "framegen: the helper's log doesn't say it made frame generation");
        CHECK(strstr(text, "frame generation failed") == NULL && strstr(text, "frame generation couldn't") == NULL,
            "framegen: the helper's log says frame generation failed");
    }
    CHECK(gfx_failures() == 0, "framegen: %u failures", gfx_failures());
    printf(g_fails ? "framegen: %d failed\n" : "framegen: ok\n", g_fails);
    return g_fails != 0;
}
#endif

/* Bloom: a bright square glows onto the dark wall around it. */
static void test_scene_bloom(void)
{
    fx_only("bloom", 1.0f);
    gfx_fx_set("threshold", 0.5f);
    scene_begin(1, 0xFF000000u);
    float lamp[4][3] = { { -0.5f, 0.5f, 5 }, { 0.5f, 0.5f, 5 }, { -0.5f, -0.5f, 5 }, { 0.5f, -0.5f, 5 } };
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    scene_quad(lamp, 0xFFFFFFFFu);
    scene_quad(wall, 0xFF101010u);
    scene_end(NULL, 0);
    /* the lamp spans 64 +- 6.4 pixels; 4 pixels past its edge */
    uint32_t glow = spx(75, 64, 0), far = spx(120, 120, 0);
    CHECK(glow >= 28, "bloom: next to the lamp %u (want a glow over 16)", glow);
    CHECK(far <= 24, "bloom: far corner %u (want the wall's 16)", far);
    gfx_fx_set("threshold", 0.85f);
}

/* God rays: with the sun behind the top of a wall (the sky above it bright), light spills down over
 * the wall below the skyline; with the sun behind the camera, none. */
static void test_scene_rays(void)
{
    float wall[4][3] = { { -8, 0, 6 }, { 8, 0, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    const float sun[3] = { 0, 0.1f, 1 }, behind[3] = { 0, 0.1f, -1 };
    uint32_t lit[2];
    for (int k = 0; k < 2; ++k)
    {
        fx_only("rays", 1.0f);
        scene_begin(1, 0xFFFFFFFFu); /* white sky */
        scene_quad(wall, 0xFF202020u);
        scene_end(k ? behind : sun, 0);
        lit[k] = spx(64, 72, 0); /* 8 rows below the skyline */
    }
    CHECK(lit[0] >= 60, "god rays: wall below the sun %u (want lit over its 32)", lit[0]);
    CHECK(lit[1] <= 40, "god rays: sun behind the camera %u (want the wall's 32)", lit[1]);
}

/* The sun's shadow map: a post 10 ahead, 3 high, the sun beyond it and up at about 27 degrees - its
 * shadow reaches 6 along the floor toward the camera; beside it
 * the floor is lit, and with the sun behind the camera the floor before it is lit too. */
static void test_scene_sun_map(void)
{
    const float beyond[3] = { 0, 0.5f, 1 }, behind[3] = { 0, 0.5f, -1 };
    for (int k = 0; k < 2; ++k)
    {
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(k ? behind : beyond, 0);
        /* the floor 7 ahead: row 91; before the post (x 0) and beside it (x 4) */
        uint32_t before = spx(64, 91, 0), beside = spx(100, 91, 0);
        CHECK(beside >= 245, "sun map (%d): open floor %u (want lit)", k, beside);
        if (k == 0)
            CHECK(before <= 160, "sun map: floor in the post's shadow %u (want shaded)", before);
        else
            CHECK(before >= 245, "sun map: sun behind the camera, floor before the post %u (want lit)", before);
    }
}

/* A patch of ground turned a little from the sun (about 6 degrees past its edge-on), inside the post's shadow:
 * shaded like the ground around it. FFXI lights its low-polygon ground with smooth normals, so the game
 * leaves such a facet lit; the map's shadow used to be taken off it, and shadows on open ground were full of
 * lit triangles. */
static void test_scene_sun_turned(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    fx_only("sun", 1.0f);
    gfx_fx_set("temporal", 0.0f);
    scene_begin(1, 0xFF000000u);
    float patch[4][3] = { { -6, -2.05f, 8.5f }, { 6, -2.05f, 8.5f }, { -6, -3.95f, 5.5f }, { 6, -3.95f, 5.5f } };
    float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
    scene_quad(patch, 0xFFFFFFFFu);
    scene_quad(post, 0xFFFFFFFFu);
    scene_end(beyond, 0);
    uint32_t in = spx(64, 91, 0), beside = spx(100, 91, 0);
    CHECK(beside >= 245, "sun map, turned ground: beside the post's shadow %u (want lit)", beside);
    CHECK(in <= 160, "sun map, turned ground: in the post's shadow %u (want shaded, not a lit facet)", in);
    gfx_present(NULL);
    gfx_fx_set("temporal", 0.85f);
}

#ifdef VANACORE
extern int gfx_hlsl_d3d11;

/* A ridge of two faces drawn as FFXI draws its zone, its normals smooth across the shared edge, under a low sun from the camera's
 * side (10 degrees up, not held: sun_dusk 0) that lights the near face (28 degrees onto it) and leaves the far one turned 8
 * degrees away: lit 0, a zone shader's (a vertex shader, the normal an input (v3), its own white and no light); lit 1, a
 * fixed-function draw lit by the sun (Qufim's rock and snow: ambient 0.3, the sun's 0.7). The near face's rows 104 and 120, the
 * far one's 99 (a unit past the edge) and 95 (four), into out */
static void ridge_scene(int lit, float smooth, uint32_t out[4])
{
    /* vs.1.1: m4x4 oPos, v0, c0 / mov oD0, c4 */
    static const uint32_t vs[] = {
        0xFFFE0101u,
        20, 0x80000000u | (4u << 28) | (0xFu << 16), 0x80000000u | (1u << 28) | (0xE4u << 16), 0x80000000u | (2u << 28) | (0xE4u << 16),
        1, 0x80000000u | (5u << 28) | (0xFu << 16), 0x80000000u | (2u << 28) | (0xE4u << 16) | 4,
        0x0000FFFFu,
    };
    fx_only("sun", 1.0f);
    gfx_fx_set("sun_min", 0.0f), gfx_fx_set("sun_dusk", 0.0f), gfx_fx_set("temporal", 0.0f), gfx_fx_set("sun_smooth", smooth);
    const float e = 10.0f * 3.14159265f / 180.0f, f = 18.0f * 3.14159265f / 180.0f, sf = sinf(f), cf = cosf(f);
    const float sun[3] = { 0, sinf(e), -cosf(e) };
    /* x, y, distance ahead; the normal (distance-wise): the near face's foot, the edge, the far face's foot */
    const float rows[3][6] = { { 0, -3.5f - 3 * sf, 6 - 3 * cf, 0, cf, -sf }, { 0, -3.5f, 6, 0, 1, 0 }, { 0, -3.5f - 6 * sf, 6 + 6 * cf, 0, cf, sf } };
    for (int f = 0; f < 4; ++f) /* the sun taken (test_scene_sun_hold), and the smooth normals' pass given the projection */
    {
        gfx_present(NULL);
        scene_begin(1, 0xFF000000u);
        scene_end(sun, 0);
    }
    {
        gfx_present(NULL);
        scene_begin(1, 0xFF000000u);
        GfxDraw d;
        defaults(&d);
        d.vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d.vs.el[GFX_R_NORMAL] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d.u.stride[0] = 24, d.u.offset[GFX_R_NORMAL] = 12;
        if (lit)
        {
            memcpy(d.u.wvp, g_sproj, 64); /* (the world and view the identity: the positions are in view space) */
            d.vs.lighting = 1, d.vs.nlights = 1, d.vs.light_type[0] = 3, d.vs.normalize = 1;
            for (int i = 0; i < 4; ++i)
                d.u.mat_d[i] = d.u.mat_a[i] = 1.0f, d.u.ambient[i] = 0.3f, d.u.light[0].diffuse[i] = 0.7f;
            const float l = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
            d.u.light[0].dir[0] = sun[0] / l, d.u.light[0].dir[1] = sun[1] / l, d.u.light[0].dir[2] = sun[2] / l * g_sz;
        }
        else
        {
            d.vs.prog = 1, d.vs_tokens = vs;
            for (int c = 0; c < 4; ++c) /* c0..c3: the projection's columns (the positions are in view space) */
                for (int r = 0; r < 4; ++r)
                    d.u.vsc[c][r] = g_sproj[r * 4 + c];
            d.u.vsc[4][0] = d.u.vsc[4][1] = d.u.vsc[4][2] = d.u.vsc[4][3] = 1.0f;
        }
        d.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 }; /* SELECTARG1(DIFFUSE) */
        d.u.vp[2] = d.u.vp[3] = SS;
        memcpy(d.vp, SVP, sizeof SVP);
        d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
        d.caster = 1;
        float v[6][6];
        for (int i = 0; i < 6; ++i)
        {
            const float* rw = rows[i >> 1];
            v[i][0] = (i & 1) ? 6.0f : -6.0f, v[i][1] = rw[1], v[i][2] = rw[2] * g_sz;
            v[i][3] = rw[3], v[i][4] = rw[4], v[i][5] = rw[5] * g_sz;
        }
        d.data[0] = v, d.size[0] = sizeof v;
        d.cpu[0] = v, d.cpu_size[0] = sizeof v;
        d.prim = GFX_TRIANGLESTRIP, d.count = 4;
        gfx_draw(&d);
        scene_end(sun, 0);
        out[0] = spx(64, 104, 0), out[1] = spx(64, 120, 0), out[2] = spx(64, 99, 0), out[3] = spx(64, 95, 0);
    }
    const float beyond[3] = { 0, 0.5f, 1 };
    for (int f = 0; f < 4; ++f) /* the sun beyond again, for the tests after */
    {
        gfx_present(NULL);
        scene_begin(1, 0xFF000000u);
        scene_end(beyond, 0);
    }
    gfx_present(NULL);
    gfx_fx_set("sun_dusk", 1.0f), gfx_fx_set("temporal", 0.85f), gfx_fx_set("sun_smooth", 1.0f);
}

/* The sun on the zone by its own smooth normals (sun_smooth, gfx_d3d12.c vn_replay; Direct3D 12's). By the maps alone the far face
 * is in the near one's shadow from the very edge on: a hard step there (sun_min 0, the players' default). By the smooth normals
 * the edge is lit on past it, as smooth shading would: a zone shader's far face (no light of the game's) darkens away from the
 * edge as it turns from the sun; a lit draw's is left the game's own shade, its light going as the normal turns */
static void test_scene_sun_smooth(void)
{
    if (gfx_hlsl_d3d11)
        return;
    uint32_t off[4], on[4];
    ridge_scene(0, 0.0f, off);
    ridge_scene(0, 1.0f, on);
    printf("  sun, smooth normals (zone shader): near face %u %u, far face %u %u; by the maps alone %u %u, %u %u\n", on[0], on[1],
        on[2], on[3], off[0], off[1], off[2], off[3]);
    CHECK(off[0] >= off[2] + 100, "sun by the maps alone: the ridge's near face %u, the far one by the edge %u (want a hard step)",
        off[0], off[2]);
    CHECK(on[2] >= off[2] + 60, "sun by the smooth normals: the far face by the edge %u, %u by the maps alone (want lit on past the edge)",
        on[2], off[2]);
    CHECK(on[1] >= 230, "sun by the smooth normals: the near face %u (want lit)", on[1]);
    CHECK(on[3] <= 100, "sun by the smooth normals: the far face four units past the edge %u (want shaded)", on[3]);
    ridge_scene(1, 0.0f, off);
    ridge_scene(1, 1.0f, on);
    printf("  sun, smooth normals (lit draw): near face %u %u, far face %u %u; by the maps alone %u %u, %u %u\n", on[0], on[1],
        on[2], on[3], off[0], off[1], off[2], off[3]);
    CHECK(off[0] >= off[2] + 50, "sun by the maps alone, a lit draw: the ridge's near face %u, the far one by the edge %u (want a hard step)",
        off[0], off[2]);
    CHECK(on[2] + 30 >= on[0], "sun by the smooth normals, a lit draw: the far face by the edge %u, the near face %u (want the game's own step)",
        on[2], on[0]);
    CHECK(on[3] >= off[3] + 30, "sun by the smooth normals, a lit draw: the far face four units on %u, %u by the maps alone (want the "
        "game's own shade, not darker)", on[3], off[3]);
}
#endif

/* Dusk: the sun beyond the post but 11 degrees under the horizon (FFXI's light as it sets). With
 * sun_dusk the shadow falls from about 17 degrees up on the same bearing - long, reaching the floor 3
 * before the post - and without it there's none: the sun has set. */
static void test_scene_sun_dusk(void)
{
    const float low[3] = { 0, -0.2f, 1 };
    for (int dusk = 1; dusk >= 0; --dusk)
    {
        fx_only("sun", 1.0f);
        gfx_fx_set("sun_dusk", (float)dusk);
        gfx_fx_set("temporal", 0.0f);
        scene_begin(1, 0xFF000000u);
        float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(low, 0);
        uint32_t before = spx(64, 91, 0), beside = spx(100, 91, 0);
        CHECK(beside >= 245, "sun at dusk (sun_dusk %d): open floor %u (want lit)", dusk, beside);
        if (dusk)
            CHECK(before <= 160, "sun at dusk: floor in the post's long shadow %u (want shaded)", before);
        else
            CHECK(before >= 245, "sun at dusk, sun_dusk 0: floor before the post %u (want lit: the sun has set)", before);
    }
    gfx_fx_set("sun_dusk", 1.0f);
    gfx_fx_set("temporal", 0.85f);
}

/* The sun from frame to frame (the flicker watch's first find): the post's map shadow as before, frame
 * after frame. A lit draw with a light of its own (the sun behind the camera) in one frame doesn't turn
 * the shadow - nor does a frame with no lit draw at all, which keeps the sun it had; the same light four
 * frames running does turn it (the floor before the post lit). */
static void test_scene_sun_hold(void)
{
    const float beyond[3] = { 0, 0.5f, 1 }, behind[3] = { 0, 0.5f, -1 };
    /* each frame's sun: 5 beyond (taken), one behind, none, then behind 4 frames running */
    const float* suns[11] = { beyond, beyond, beyond, beyond, beyond, behind, NULL, behind, behind, behind, behind };
    fx_only("sun", 1.0f);
    gfx_fx_set("temporal", 0.0f); /* each frame's shadow alone */
    for (int f = 0; f < 11; ++f)
    {
        gfx_present(NULL);
        scene_begin(1, 0xFF000000u);
        float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(suns[f], 0);
        uint32_t before = spx(64, 91, 0);
        if (f >= 4 && f < 10)
            CHECK(before <= 160, "sun held (frame %d, %s): floor in the post's shadow %u (want shaded)", f,
                f == 5 ? "one frame's other light" : f == 6 ? "no lit draw" : "another light, not yet four frames", before);
        else if (f == 10)
            CHECK(before >= 245, "sun held: another light four frames running, floor before the post %u (want lit: taken)", before);
    }
    /* the sun beyond again, taken, for the tests after (their frames follow one another too) */
    for (int f = 0; f < 4; ++f)
    {
        gfx_present(NULL);
        scene_begin(1, 0xFF000000u);
        scene_end(beyond, 0);
    }
    gfx_present(NULL);
    gfx_fx_set("temporal", 0.85f);
}

/* The zone out of view still casts: a post drawn from a buffer the game keeps shades the floor in
 * the frame it is drawn and in the next one, where the game (out of view) no longer draws it. */
static void test_scene_sun_cache(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    float post[4][3] = { { -1, 0, -10 }, { 1, 0, -10 }, { -1, -3, -10 }, { 1, -3, -10 } }; /* right-handed: z ahead is negative */
    GfxBuf* b = gfx_buf_create(sizeof post);
    gfx_buf_upload(b, post, sizeof post);
    gfx_fx_set("sun_casters", 0.0f); /* the zone casts too */
    for (int frame = 0; frame < 2; ++frame)
    {
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        scene_quad(floor, 0xFFFFFFFFu);
        if (frame == 0)
        {
            GfxDraw d;
            defaults(&d);
            memcpy(d.u.wvp, g_sproj, 64);
            d.u.vp[2] = d.u.vp[3] = SS;
            memcpy(d.vp, SVP, sizeof SVP);
            d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
            d.u.stride[0] = 12;
            d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 };
            d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
            d.caster = 1;
            d.buf[0] = b, d.size[0] = sizeof post;
            d.prim = GFX_TRIANGLESTRIP, d.count = 2;
            gfx_draw(&d);
        }
        scene_end(beyond, 0);
        uint32_t before = spx(64, 91, 0), beside = spx(100, 91, 0);
        CHECK(beside >= 245, "sun cache (frame %d): open floor %u (want lit)", frame, beside);
        CHECK(before <= 160, "sun cache (frame %d): floor in the post's shadow %u (want shaded%s)", frame, before,
            frame ? ", from the post kept after the game stopped drawing it" : "");
        gfx_present(NULL);
    }
    /* characters alone cast (sun_casters 1, the default): the zone's post, drawn again, does not */
    gfx_fx_set("sun_casters", 1.0f);
    gfx_fx_set("temporal", 0.0f); /* not the frame before's shadow */
    fx_only("sun", 1.0f);
    scene_begin(1, 0xFF000000u);
    {
        float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        scene_quad(floor, 0xFFFFFFFFu);
        GfxDraw d;
        defaults(&d);
        memcpy(d.u.wvp, g_sproj, 64);
        d.u.vp[2] = d.u.vp[3] = SS;
        memcpy(d.vp, SVP, sizeof SVP);
        d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d.u.stride[0] = 12;
        d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 };
        d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
        d.caster = 1;
        d.buf[0] = b, d.size[0] = sizeof post;
        d.prim = GFX_TRIANGLESTRIP, d.count = 2;
        gfx_draw(&d);
    }
    scene_end(beyond, 0);
    CHECK(spx(64, 91, 0) >= 245, "sun casters 1: the floor before the zone's post %u (want lit: the zone does not cast)", spx(64, 91, 0));
    gfx_present(NULL);
    /* the environment alone (sun_casters 2): the zone's post, kept out of view, casts; a character (drawn
     * from the frame's own vertices) beside it doesn't */
    gfx_fx_set("sun_casters", 2.0f);
    fx_only("sun", 1.0f);
    scene_begin(1, 0xFF000000u);
    {
        float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
        float pole[4][3] = { { 3, 0, 10 }, { 5, 0, 10 }, { 3, -3, 10 }, { 5, -3, 10 } };
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(pole, 0xFFFFFFFFu);
    }
    scene_end(beyond, 0);
    CHECK(spx(64, 91, 0) <= 160, "sun casters 2: the floor before the zone's post %u (want shaded: the zone casts)", spx(64, 91, 0));
    CHECK(spx(100, 91, 0) >= 245, "sun casters 2: the floor before a character %u (want lit: characters don't cast)", spx(100, 91, 0));
    gfx_present(NULL);
    gfx_fx_set("sun_casters", 1.0f); /* as the tests after this had it */
    gfx_fx_set("temporal", 0.85f);
    gfx_buf_destroy(b);
}

/* a white quad (as scene_quad's) from static buffer b, uploaded with it first: one buffer the game rewrites for each
 * object it draws from it (d3d8.c uploads it at the draw after the game wrote it) */
static void scene_rewritten(GfxBuf* b, const float (*q)[3])
{
    float v[4][3];
    for (int i = 0; i < 4; ++i)
        v[i][0] = q[i][0], v[i][1] = q[i][1], v[i][2] = q[i][2] * g_sz;
    gfx_buf_upload(b, v, sizeof v);
    GfxDraw d;
    defaults(&d);
    memcpy(d.u.wvp, g_sproj, 64);
    d.u.vp[2] = d.u.vp[3] = SS;
    memcpy(d.vp, SVP, sizeof SVP);
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 12;
    d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 }; /* SELECTARG1(TFACTOR) */
    d.u.tfactor[0] = d.u.tfactor[1] = d.u.tfactor[2] = d.u.tfactor[3] = 1.0f;
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    d.caster = 1;
    d.buf[0] = b, d.size[0] = sizeof v;
    d.cpu[0] = v, d.cpu_size[0] = sizeof v;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
}

/* Two posts drawn from one buffer the game rewrites for each (FFXI sways each tree of a kind through one buffer,
 * uploading it again before the next is drawn): both cast. The sun's maps are drawn once the scene is done, and each
 * upload ended the casters drawn from the contents before it, so only the last post cast; each now reads its vertices
 * as they were when it was drawn. The first frame the buffer isn't known to be rewritten yet: A casts from the second.
 * (Before the copies tests: theirs, kept out of view, cast where these posts do.) */
static void test_scene_sun_rewritten(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    const float a[4][3] = { { -5, 0, 10 }, { -3, 0, 10 }, { -5, -3, 10 }, { -3, -3, 10 } };
    const float bq[4][3] = { { 3, 0, 10 }, { 5, 0, 10 }, { 3, -3, 10 }, { 5, -3, 10 } };
    GfxBuf* b = gfx_buf_create(sizeof a);
    gfx_fx_set("sun_casters", 0.0f);
    gfx_fx_set("temporal", 0.0f); /* not the frame before's shadow */
    for (int frame = 0; frame < 3; ++frame)
    {
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        scene_quad(floor, 0xFFFFFFFFu);
        scene_rewritten(b, a);
        scene_rewritten(b, bq);
        scene_end(beyond, 0);
        uint32_t a_shadow = spx(28, 91, 0), b_shadow = spx(100, 91, 0);
        if (frame)
            CHECK(a_shadow <= 160, "sun, one buffer rewritten for two posts (frame %d): the floor in A's shadow %u (want shaded: "
                "A's vertices as drawn, not B's)", frame, a_shadow);
        CHECK(b_shadow <= 160, "sun, one buffer rewritten for two posts (frame %d): the floor in B's shadow %u (want shaded)", frame,
            b_shadow);
        gfx_present(NULL);
    }
    gfx_fx_set("temporal", 0.85f);
    gfx_fx_set("sun_casters", 1.0f); /* as the tests after this had it */
    gfx_buf_destroy(b);
}

/* A placed object drawn from the frame's own vertices (a tree the game rewrites as it sways), which the
 * cache can't draw again from the game's buffers, is kept as a copy: its shadow stays when the game stops
 * drawing it out of view. One whose place is in plain view, but that the game didn't draw, went: its
 * shadow goes. */
static void test_scene_sun_snapshot(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
    float pole[4][3] = { { 3, 0, 10 }, { 5, 0, 10 }, { 3, -3, 10 }, { 5, -3, 10 } };
    gfx_fx_set("sun_casters", 0.0f);
    gfx_fx_set("temporal", 0.0f); /* not the frame before's shadow */
    for (int frame = 0; frame < 4; ++frame)
    {
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        scene_quad(floor, 0xFFFFFFFFu);
        if (frame == 0)
        {
            /* the post stands behind the camera, as far as the cache knows: out of view */
            const float behind[4] = { 0, 0, 20, 1 };
            memcpy(g_quad_anchor, behind, sizeof behind);
            scene_quad(post, 0xFFFFFFFFu);
        }
        if (frame == 2)
        {
            /* the pole stands where it's drawn: in plain view */
            const float there[4] = { 4, -3, -10, 1 };
            memcpy(g_quad_anchor, there, sizeof there);
            scene_quad(pole, 0xFFFFFFFFu);
        }
        memset(g_quad_anchor, 0, sizeof g_quad_anchor);
        scene_end(beyond, 0);
        uint32_t post_shadow = spx(64, 91, 0), pole_shadow = spx(100, 91, 0);
        CHECK(post_shadow <= 160, "sun copies (frame %d): the floor in the post's shadow %u (want shaded%s)", frame, post_shadow,
            frame ? ", from the copy kept after the game stopped drawing it" : "");
        if (frame == 2)
            CHECK(pole_shadow <= 160, "sun copies: the floor in the pole's shadow %u (want shaded)", pole_shadow);
        if (frame == 3)
            CHECK(pole_shadow >= 245, "sun copies: the floor where the pole's shadow was %u (want lit: in plain view, not drawn, it went)",
                pole_shadow);
        gfx_present(NULL);
    }
    gfx_fx_set("temporal", 0.85f);
    gfx_fx_set("sun_casters", 1.0f); /* as the tests after this had it */
}

/* one part of a mesh: vertices lo .. lo + 3 of v, by their indices (as the game draws one part of a buffer that
 * holds several, d3d8's draw_packet: the part's vertices, and vofs taking its first index to the first of them) */
static void scene_part(const float (*v)[3], const uint16_t* idx, uint32_t lo)
{
    GfxDraw d;
    defaults(&d);
    memcpy(d.u.wvp, g_sproj, 64);
    d.u.vp[2] = d.u.vp[3] = SS;
    memcpy(d.vp, SVP, sizeof SVP);
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 12;
    d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 }; /* SELECTARG1(TFACTOR) */
    d.u.tfactor[0] = d.u.tfactor[1] = d.u.tfactor[2] = d.u.tfactor[3] = 1.0f;
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    d.caster = 1;
    float w[4][3];
    for (int i = 0; i < 4; ++i)
        w[i][0] = v[lo + i][0], w[i][1] = v[lo + i][1], w[i][2] = v[lo + i][2] * g_sz;
    d.data[0] = w, d.size[0] = sizeof w;
    d.cpu[0] = w, d.cpu_size[0] = sizeof w;
    d.indices = idx, d.index_size = 2;
    d.u.vofs = -(int32_t)lo;
    memcpy(d.anchor, g_quad_anchor, sizeof d.anchor);
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
}

/* Two parts of one object, the same size, from one buffer, where it stands: each keeps a copy of its own. The
 * second's draw found the first's copy and took its uniforms - its vofs with the first's indices - so the copy
 * read outside its vertices (a negative vertex: gigabytes past its chunk, where the GPU faults - an AMD card's
 * driver timeout) and the second part's shadow went with the first's. */
static void test_scene_sun_snapshot_parts(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    const float parts[8][3] = { { -5, 0, 10 }, { -3, 0, 10 }, { -5, -3, 10 }, { -3, -3, 10 },  /* A: vertices 0-3 */
        { 3, 0, 10 }, { 5, 0, 10 }, { 3, -3, 10 }, { 5, -3, 10 } };                         /* B: vertices 4-7 */
    const uint16_t ia[4] = { 0, 1, 2, 3 }, ib[4] = { 4, 5, 6, 7 };
    gfx_fx_set("sun_casters", 0.0f);
    gfx_fx_set("temporal", 0.0f);
    for (int frame = 0; frame < 3; ++frame)
    {
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        scene_quad(floor, 0xFFFFFFFFu);
        if (frame == 0)
        {
            /* behind the camera, as far as the cache knows: kept while out of view */
            const float behind[4] = { 0, 0, 20, 1 };
            memcpy(g_quad_anchor, behind, sizeof behind);
            scene_part(parts, ia, 0);
            scene_part(parts, ib, 4);
        }
        memset(g_quad_anchor, 0, sizeof g_quad_anchor);
        scene_end(beyond, 0);
        uint32_t a_shadow = spx(28, 91, 0), b_shadow = spx(100, 91, 0);
        CHECK(a_shadow <= 160, "sun copies of two parts (frame %d): the floor in A's shadow %u (want shaded)", frame, a_shadow);
        CHECK(b_shadow <= 160, "sun copies of two parts (frame %d): the floor in B's shadow %u (want shaded%s)", frame, b_shadow,
            frame ? ", from its own copy: A's taken through B's vofs is neither" : "");
        gfx_present(NULL);
    }
    /* a read outside the copy past its chunk takes the device down: nothing more can be made on it */
    GfxBuf* probe = gfx_buf_create(16);
    CHECK(probe != NULL, "sun copies of two parts: the graphics device is gone (a copy read outside its vertices)");
    gfx_buf_destroy(probe);
    gfx_fx_set("temporal", 0.85f);
    gfx_fx_set("sun_casters", 1.0f); /* as the tests after this had it */
}

/* one card of a mesh the zone draws several times from one buffer, each copy placed by its own matrix (FFXI's
 * trees of a kind): at x, 18 ahead */
static void scene_instance(GfxBuf* b, const float (*card)[3], uint32_t size, float x)
{
    GfxDraw d;
    defaults(&d);
    const float t[3] = { x, 0.0f, -18.0f };
    memcpy(d.u.wvp, g_sproj, 64);
    for (int j = 0; j < 4; ++j)
        d.u.wvp[12 + j] += t[0] * g_sproj[j] + t[1] * g_sproj[4 + j] + t[2] * g_sproj[8 + j];
    d.u.vp[2] = d.u.vp[3] = SS;
    memcpy(d.vp, SVP, sizeof SVP);
    d.vs.el[0] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.stride[0] = 12;
    d.fs.st[0] = (GfxStage){ 2, 3, 1, 1, 2, 3, 1, 1, 1, 0, 0, 2 };
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    d.caster = 1;
    d.buf[0] = b, d.size[0] = size;
    d.cpu[0] = card, d.cpu_size[0] = size;
    d.prim = GFX_TRIANGLESTRIP, d.count = 2;
    gfx_draw(&d);
}

/* The zone's copies of one mesh are kept apart: three cards from one buffer, the sun low to the right. In
 * the first frame all three are drawn - one (A) off the view's right edge, its shadow falling into view;
 * in the next only B is: A is out of view, and still casts, where the cache used to keep only the copy
 * drawn last; C, in plain view but not drawn, went, and its shadow with it. */
static void test_scene_sun_instances(void)
{
    const float side[3] = { 1, 0.25f, 0 }, beyond[3] = { 0, 0.5f, 1 };
    const float card[4][3] = { { 0, 0, -2 }, { 0, 0, 2 }, { 0, -3, -2 }, { 0, -3, 2 } }; /* facing the sun, 3 high */
    float floor[4][3] = { { -40, -3, 40 }, { 40, -3, 40 }, { -40, -3, 0.6f }, { 40, -3, 0.6f } };
    GfxBuf* b = gfx_buf_create(sizeof card);
    gfx_buf_upload(b, card, sizeof card);
    gfx_fx_set("sun_casters", 0.0f);
    gfx_fx_set("temporal", 0.0f);
    gfx_fx_set("sun_fade", 0.0f); /* what the game stops drawing in plain view goes at once, not fading over a second */
    fx_only("sun", 1.0f);
    for (int f = 0; f < 4; ++f) /* the sun to the right, taken (test_scene_sun_hold) */
    {
        gfx_present(NULL);
        scene_begin(1, 0xFF000000u);
        scene_end(side, 0);
    }
    for (int frame = 0; frame < 2; ++frame)
    {
        gfx_present(NULL);
        fx_only("sun", 1.0f);
        scene_begin(1, 0xFF000000u);
        scene_quad(floor, 0xFFFFFFFFu);
        if (frame == 0)
            scene_instance(b, card, sizeof card, 21.0f); /* A: off the right edge; its shadow reaches 9 */
        scene_instance(b, card, sizeof card, -6.0f);     /* B */
        if (frame == 0)
            scene_instance(b, card, sizeof card, 3.0f);  /* C */
        scene_end(side, 0);
        /* the floor 18 ahead (row 74): in A's shadow (x 13), C's (x -2) and B's (x -12), and open (x 7; the sun
         * this low lights it less than full) */
        uint32_t a = spx(110, 74, 0), c = spx(57, 74, 0), bs = spx(22, 74, 0), open = spx(90, 74, 0);
        CHECK(open >= 150, "sun cache copies (frame %d): open floor %u (want lit)", frame, open);
        CHECK(a <= open / 2, "sun cache copies (frame %d): the floor in A's shadow %u, open %u (want shaded%s)", frame, a, open,
            frame ? ": A is out of view, kept apart from B, drawn from the same buffer" : "");
        CHECK(bs <= open / 2, "sun cache copies (frame %d): the floor in B's shadow %u, open %u (want shaded)", frame, bs, open);
        if (frame == 0)
            CHECK(c <= open / 2, "sun cache copies: the floor in C's shadow %u, open %u (want shaded)", c, open);
        else
            CHECK(c + 10 >= open, "sun cache copies: the floor where C's shadow was %u, open %u (want lit: in plain view, not drawn, it went)",
                c, open);
    }
    for (int f = 0; f < 4; ++f) /* the sun beyond again, for the tests after */
    {
        gfx_present(NULL);
        scene_begin(1, 0xFF000000u);
        scene_end(beyond, 0);
    }
    gfx_present(NULL);
    gfx_fx_set("temporal", 0.85f);
    gfx_fx_set("sun_fade", 1.0f);
    gfx_fx_set("sun_casters", 1.0f);
    gfx_buf_destroy(b);
}

/* Over time: a post stands three frames, then steps aside (a character running) - its shadow is whole at
 * once where it now falls, and none is left where it was: the frames before, blended in at the same world
 * point, are held to what this frame has there. The same with the temporal pass off. */
static void test_scene_temporal(void);

/* VanaCore's glow (gfx_glow): a character's picture - a 1024 target, transparent but for a solid white square in
 * its middle - glowed onto the scene (128x128) in an orange tint: lit over the square, a fading orange just
 * outside it, nothing farther out, and the orange red's, not blue's */
static void test_scene_glow(void)
{
    fx_only(NULL, 0);
    GfxTex* pic = gfx_tex_create(GFX_TEX_2D, 21, 1024, 1024, 1, GFX_USE_RT);
    static const uint32_t pvp[6] = { 0, 0, 1024, 1024, 0, 0x3F800000u };
    gfx_set_targets(pic, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0x00000000u, 1.0f, 0, pvp);
    const int32_t square[4] = { 448, 448, 576, 576 };
    gfx_clear(1, square, 1, 0xFFFFFFFFu, 1.0f, 0, pvp);
    gfx_set_targets(g_srt, 0, 0, g_sds);
    gfx_clear(0, NULL, 3, 0xFF000000u, 1.0f, 0, SVP);
    const float tint[4] = { 1.0f, 0.5f, 0.0f, 1.5f };
    gfx_glow(pic, tint);
    gfx_tex_read(g_srt, 0, 0, g_spx, SS * 4);
    /* the square spans 56..72 at this size */
    uint32_t in = spx(64, 64, 16), near_r = spx(73, 64, 16), near_b = spx(73, 64, 0), far = spx(90, 64, 16);
    CHECK(in >= 200, "glow: over the square %u (want lit)", in);
    CHECK(near_r >= 15 && near_r < in && near_b <= 5, "glow: just outside it red %u, blue %u (want a fading orange)", near_r, near_b);
    CHECK(far <= 5, "glow: far from it %u (want none)", far);
    /* ... then VanaCore's grass drawn over it (a green patch over the square's lower part, nearer than the scene's depth, after the
     * depth before it was kept) and the glow laid again over it (gfx_glow_over_grass): lit again over the square and orange beside
     * it on the grass, the grass far from it green, and what the grass doesn't cover glowed once, as before */
    gfx_scene_world_depth(g_sds);
    const int32_t grass[4] = { 40, 66, 100, 80 };
    gfx_clear(1, grass, 3, 0xFF00C000u, 0.5f, 0, SVP);
    gfx_glow_over_grass(g_srt, g_sds);
    gfx_tex_read(g_srt, 0, 0, g_spx, SS * 4);
    const uint32_t g_in = spx(64, 69, 16), g_near = spx(73, 69, 16), g_far = spx(95, 76, 16), g_far_g = spx(95, 76, 8);
    CHECK(g_in >= 150, "glow over the grass: over the square %u (want lit again, not the grass's green)", g_in);
    CHECK(g_near >= 10, "glow over the grass: just outside the square %u (want its orange over the grass)", g_near);
    CHECK(g_far <= 5 && g_far_g >= 150, "glow over the grass: far from it red %u, green %u (want the grass alone)", g_far, g_far_g);
    const uint32_t still = spx(73, 64, 16);
    CHECK(still + 2 >= near_r && still <= near_r + 2, "glow over the grass: beside the square off the grass %u (want %u, as before)", still,
        near_r);
    gfx_present(NULL);
    gfx_tex_destroy(pic);
}

static void test_scene_temporal(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    for (int pass = 0; pass < 2; ++pass)
    {
        uint32_t was = 0, now = 0;
        for (int frame = 0; frame < 4; ++frame)
        {
            fx_only("sun", 1.0f);
            gfx_fx_set("temporal", pass ? 0.0f : 0.85f);
            scene_begin(1, 0xFF000000u);
            float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
            float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
            if (frame == 3) /* a step to the right */
                for (int v = 0; v < 4; ++v)
                    post[v][0] += 4.0f;
            scene_quad(floor, 0xFFFFFFFFu);
            scene_quad(post, 0xFFFFFFFFu);
            scene_end(beyond, 0);
            was = spx(64, 91, 0), now = spx(100, 91, 0);
            gfx_present(NULL);
        }
        CHECK(now <= 160, "temporal %s: the floor in the moved post's shadow %u (want shaded at once, not faint)", pass ? "off" : "on", now);
        CHECK(was >= 245, "temporal %s: the floor where its shadow was %u (want lit: no trail)", pass ? "off" : "on", was);
    }
    gfx_fx_set("temporal", 0.85f);
}

/* Resolution: a pole 0.2 wide casts a thin shadow, dark in its middle and gone a few pixels beside
 * it (the near cascade's centimetre texels, no blur over it). */
static void test_scene_sun_sharp(void)
{
    const float beyond[3] = { 0, 0.5f, 1 };
    fx_only("sun", 1.0f);
    gfx_fx_set("temporal", 0.0f);
    scene_begin(1, 0xFF000000u);
    float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    float pole[4][3] = { { -0.1f, 0, 10 }, { 0.1f, 0, 10 }, { -0.1f, -3, 10 }, { 0.1f, -3, 10 } };
    scene_quad(floor, 0xFFFFFFFFu);
    scene_quad(pole, 0xFFFFFFFFu);
    scene_end(beyond, 0);
    uint32_t mid = spx(64, 91, 0), side = spx(68, 91, 0);
    CHECK(mid <= 140, "sharp shadows: under a 0.2-wide pole %u (want dark)", mid);
    CHECK(side >= 235, "sharp shadows: 4 pixels beside it %u (want lit)", side);
    /* a post 2 wide: its shadow's edge across the floor, the pixels part lit - softened (sun_soft 0.03, then
     * the edges' default), then hard (sun_soft 0, the near map at 8192) */
    int widths[2];
    for (int hard = 0; hard < 2; ++hard)
    {
        fx_only("sun", 1.0f);
        gfx_fx_set("sun_soft", hard ? 0.0f : 0.03f);
        gfx_fx_set("sun_soft_zone", 0.0f); /* the zone's own smoothing (0.6) blurs a hard edge across the floor */
        gfx_fx_set("sun_rings", 0.0f);     /* and the rings' fewer taps past the player, all of them here */
        gfx_fx_set("sun_soft_chars", 0.0f); /* the quads are casters, so characters to the shadows: their blur (0.05) takes the floor too */
        gfx_fx_set("sun_detail", hard ? 8192.0f : 4096.0f);
        scene_begin(1, 0xFF000000u);
        float post[4][3] = { { -1, 0, 10 }, { 1, 0, 10 }, { -1, -3, 10 }, { 1, -3, 10 } };
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(post, 0xFFFFFFFFu);
        scene_end(beyond, 0);
        CHECK(spx(64, 91, 0) <= 60, "sun shadows (%s): under the post %u (want dark)", hard ? "hard" : "soft", spx(64, 91, 0));
        CHECK(spx(100, 91, 0) >= 245, "sun shadows (%s): beside the post %u (want lit)", hard ? "hard" : "soft", spx(100, 91, 0));
        widths[hard] = 0;
        for (int x = 64; x < SS; ++x)
            widths[hard] += spx(x, 91, 0) > 25 && spx(x, 91, 0) < 230;
        gfx_present(NULL);
    }
    CHECK(widths[1] <= 2 && widths[1] < widths[0], "sun shadows: the edge %d pixels wide hard, %d soft (want at most 2, and narrower)",
        widths[1], widths[0]);
    gfx_fx_set("sun_soft", 0.03f);
    gfx_fx_set("sun_soft_zone", 0.6f);
    gfx_fx_set("sun_rings", 1.0f);
    gfx_fx_set("sun_soft_chars", 0.0f);
    gfx_fx_set("sun_detail", 4096.0f);
    gfx_fx_set("temporal", 0.85f);
}

/* The middle map (sun_mid, gfx_sun_mid.h): with the sharp map off and the far map spread over 150 units (texels of 7 cm and
 * more), a pole 0.1 wide 10 units off casts a dark, thin shadow with the middle map out to 40 (texels of some 2 cm), and no
 * lighter one than without it. Hard edges and none of the blurs, so only the maps' texels decide. Direct3D 12's alone */
static void test_scene_sun_mid(void)
{
    if (gfx_selected_d3d11())
        return;
    const float beyond[3] = { 0, 0.5f, 1 };
    float floor[4][3] = { { -8, -3, 30 }, { 8, -3, 30 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    float pole[4][3] = { { -0.05f, 0, 10 }, { 0.05f, 0, 10 }, { -0.05f, -3, 10 }, { 0.05f, -3, 10 } };
    uint32_t under[2], side[2];
    for (int mid = 0; mid < 2; ++mid)
    {
        fx_only("sun", 1.0f);
        gfx_fx_set("temporal", 0.0f);
        gfx_fx_set("sun_soft", 0.0f);
        gfx_fx_set("sun_soft_zone", 0.0f);
        gfx_fx_set("sun_soft_chars", 0.0f);
        gfx_fx_set("sun_rings", 0.0f);
        gfx_fx_set("sun_sharp", 0.0f);
        gfx_fx_set("sun_distance", 150.0f);
        gfx_fx_set("sun_mid", mid ? 40.0f : 0.0f);
        scene_begin(1, 0xFF000000u);
        scene_quad(floor, 0xFFFFFFFFu);
        scene_quad(pole, 0xFFFFFFFFu);
        scene_end(beyond, 0);
        under[mid] = spx(64, 91, 0), side[mid] = spx(68, 91, 0);
        gfx_present(NULL);
    }
    CHECK(under[1] <= 120, "middle map: under a 0.1-wide pole %u with it, %u without (want dark with it)", under[1], under[0]);
    CHECK(under[1] <= under[0], "middle map: under the pole %u with it, %u without (want no lighter)", under[1], under[0]);
    CHECK(side[1] >= 235, "middle map: 4 pixels beside the pole %u with it (want lit)", side[1]);
    gfx_fx_set("sun_mid", 0.0f);
    gfx_fx_set("sun_sharp", 1.0f);
    gfx_fx_set("sun_soft", 0.03f);
    gfx_fx_set("sun_soft_zone", 0.6f);
    gfx_fx_set("sun_rings", 1.0f);
    gfx_fx_set("temporal", 0.85f);
}

#ifdef VANACORE
/* The cel look's ink lines: a block before a wall gets a dark line along its own edge, none inside it
 * and none on the wall just past it; the crease where a wall meets the floor gets one, the open wall
 * and floor none. */
static void test_scene_outline(void)
{
    fx_only("outline", 1.0f);
    scene_begin(1, 0xFF000000u);
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    float block[4][3] = { { -1, 1, 4 }, { 1, 1, 4 }, { -1, -1, 4 }, { 1, -1, 4 } };
    scene_quad(wall, 0xFFFFFFFFu);
    scene_quad(block, 0xFFFFFFFFu);
    scene_end(NULL, 0);
    /* the block spans 48..80 across (64 +- 64 / 4) */
    uint32_t edge = 255;
    for (int x = 76; x <= 80; ++x)
        edge = spx(x, 64, 0) < edge ? spx(x, 64, 0) : edge;
    uint32_t inside = spx(64, 64, 0), past = spx(82, 64, 0);
    CHECK(edge <= 80, "outline: the block's edge %u (want a dark line)", edge);
    CHECK(inside >= 245 && past >= 245, "outline: inside the block %u, the wall past it %u (want no line)", inside, past);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFFFFFFFFu);
    scene_end(NULL, 0);
    uint32_t crease = 255; /* the wall meets the floor at row 96 */
    for (int y = 93; y <= 99; ++y)
        crease = spx(64, y, 0) < crease ? spx(64, y, 0) : crease;
    uint32_t open = spx(64, 40, 0), floor = spx(64, 118, 0);
    CHECK(crease <= 100, "outline: the crease %u (want a dark line)", crease);
    CHECK(open >= 245 && floor >= 245, "outline: the open wall %u, the floor %u (want no line)", open, floor);
    /* debug 6: the lines alone, white elsewhere */
    gfx_fx_set("debug", 6.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFF202020u);
    scene_end(NULL, 0);
    gfx_fx_set("debug", 0.0f);
    CHECK(spx(64, 40, 0) >= 245, "outline debug: the open wall %u (want white)", spx(64, 40, 0));
}

/* The ink lines' own width on characters (outline_width_chars), with the sun's shadows off: a character (drawn as the game
 * draws them, by indices from its own memory, so not a fixed caster) and a block of the zone side by side before a wall.
 * The character's line is as wide as its own setting and the block's as the world's (outline_width): with the shadows
 * off the characters' depth (sun_map_view) went undrawn, and the character took the world's width. */
static void test_scene_outline_chars(void)
{
    fx_only("outline", 1.0f);
    /* at 128 lines the widths are scaled by 128 / 1080: the world's 1 pixel, the characters' 4 */
    gfx_fx_set("outline_width", 1.0f);
    gfx_fx_set("outline_width_chars", 4.0f * 1080.0f / 128.0f);
    scene_begin(1, 0xFF000000u);
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    float block[4][3] = { { -3, 1, 4 }, { -1, 1, 4 }, { -3, -1, 4 }, { -1, -1, 4 } };
    static const float chr[4][3] = { { 1, 1, 4 }, { 3, 1, 4 }, { 1, -1, 4 }, { 3, -1, 4 } };
    static const uint16_t idx[4] = { 0, 1, 2, 3 };
    scene_quad(wall, 0xFFFFFFFFu);
    scene_quad(block, 0xFFFFFFFFu);
    scene_part(chr, idx, 0);
    scene_end(NULL, 0);
    /* the block covers 16..47 across and the character 80..111 (64 + 64 * x / 4): the dark run in from each one's right edge */
    int bw = 0, cw = 0;
    while (bw < 8 && spx(47 - bw, 64, 0) < 128)
        ++bw;
    while (cw < 8 && spx(111 - cw, 64, 0) < 128)
        ++cw;
    CHECK(bw >= 1 && bw <= 2, "outline chars: the block's line %d pixels wide (want the world's, 1)", bw);
    CHECK(cw >= 3 && cw <= 5, "outline chars: the character's line %d pixels wide with the shadows off (want its own, 4)", cw);
    /* ... and the widths the other way round (the world's 4, the characters' 1), a character standing on the floor before the
     * wall: the floor's fold against its feet is no line of the floor's (it drew a band the world's width round them), the
     * character's own line stays its width */
    gfx_fx_set("outline_width", 4.0f * 1080.0f / 128.0f);
    gfx_fx_set("outline_width_chars", 1.0f);
    scene_begin(1, 0xFF000000u);
    wall_and_floor(0xFFFFFFFFu);
    static const float standing[4][3] = { { -1, -1, 4 }, { 1, -1, 4 }, { -1, -3, 4 }, { 1, -3, 4 } };
    scene_part(standing, idx, 0);
    scene_end(NULL, 0);
    /* it covers 48..79 across and rows 80..111 (its feet on the floor at row 112) */
    uint32_t edge = spx(79, 96, 0), in = spx(78, 96, 0), beside = 255, before = 255;
    for (int x = 81; x <= 83; ++x) /* the floor beside the feet: a world pixel with the character 4 to its left */
        beside = spx(x, 111, 0) < beside ? spx(x, 111, 0) : beside;
    for (int y = 114; y <= 115; ++y) /* ... and in front of them, the character 4 above */
        before = spx(64, y, 0) < before ? spx(64, y, 0) : before;
    CHECK(edge <= 80 && in >= 245, "outline chars: the standing character's edge %u, 1 pixel in %u (want its own 1-pixel line)", edge, in);
    CHECK(beside >= 245 && before >= 245, "outline chars: the floor beside the feet %u, before them %u (want no band)", beside, before);
    gfx_fx_set("outline_width", 1.0f);
    gfx_fx_set("outline_width_chars", 0.0f);
}

/* The cel look's bands: two grays in the same one of four bands come out the same (its middle,
 * 0.375), and a color keeps its hue. */
static void test_scene_bands(void)
{
    fx_only("bands", 4.0f);
    scene_begin(1, 0xFF000000u);
    float left[4][3] = { { -8, 8, 6 }, { 0, 8, 6 }, { -8, -8, 6 }, { 0, -8, 6 } };
    float right[4][3] = { { 0, 8, 6 }, { 8, 8, 6 }, { 0, -8, 6 }, { 8, -8, 6 } };
    scene_quad(left, 0xFF4D4D4Du);  /* 0.30 */
    scene_quad(right, 0xFFC04830u); /* orange, luma about 0.36 */
    scene_end(NULL, 0);
    uint32_t a = spx(32, 64, 0), r = spx(96, 64, 16), g = spx(96, 64, 8), b = spx(96, 64, 0);
    CHECK(a >= 92 && a <= 100, "bands: gray 0.30 is %u (want its band's middle, 96)", a);
    CHECK(r > g && g > b, "bands: orange %u %u %u (want its hue kept)", r, g, b);
    scene_begin(1, 0xFF000000u);
    scene_quad(left, 0xFF737373u); /* 0.45: the same band as 0.30 */
    scene_end(NULL, 0);
    uint32_t c = spx(32, 64, 0);
    CHECK(c >= 92 && c <= 100, "bands: gray 0.45 is %u (want the same band's middle, 96)", c);
}

/* The cel look's smoothing keeps a hard edge hard: pixels two from it keep their side's color. */
static void test_scene_smooth(void)
{
    fx_only("smooth", 1.0f);
    scene_begin(1, 0xFF000000u);
    float left[4][3] = { { -8, 8, 6 }, { 0, 8, 6 }, { -8, -8, 6 }, { 0, -8, 6 } };
    float right[4][3] = { { 0, 8, 6 }, { 8, 8, 6 }, { 0, -8, 6 }, { 8, -8, 6 } };
    scene_quad(left, 0xFFFFFFFFu);
    scene_quad(right, 0xFF000000u);
    scene_end(NULL, 0);
    uint32_t l = spx(62, 64, 0), r = spx(66, 64, 0);
    CHECK(l >= 245 && r <= 10, "smooth: two pixels either side of an edge %u %u (want 255 and 0)", l, r);
    /* ... and flattens detail: a dark stripe one pixel wide (column 64) on a white wall mostly goes */
    scene_begin(1, 0xFF000000u);
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    float stripe[4][3] = { { 0.01f, 8, 6 }, { 0.09f, 8, 6 }, { 0.01f, -8, 6 }, { 0.09f, -8, 6 } };
    scene_quad(wall, 0xFFFFFFFFu);
    scene_quad(stripe, 0xFF000000u);
    scene_end(NULL, 0);
    uint32_t line = spx(64, 64, 0);
    CHECK(line >= 120, "smooth: a one-pixel dark line %u (want it mostly gone)", line);
}

/* Depth of field: the focus lands on the block in the middle (the player's place), which stays
 * sharp, and doesn't smear over the far wall beside it; the far wall's own black-and-white edge is
 * blurred. */
static void test_scene_dof(void)
{
    fx_only("dof", 60.0f); /* about 7 pixels at the widest in this 128-line scene */
    float white[4][3] = { { -60, 60, 40 }, { 0, 60, 40 }, { -60, -60, 40 }, { 0, -60, 40 } };
    float black[4][3] = { { 0, 60, 40 }, { 60, 60, 40 }, { 0, -60, 40 }, { 60, -60, 40 } };
    float block[4][3] = { { -1, 1, 4 }, { 1, 1, 4 }, { -1, -1, 4 }, { 1, -1, 4 } };
    for (int pass = 0; pass < 2; ++pass)
    {
        gfx_fx_set("debug", pass ? 7.0f : 0.0f);
        scene_begin(1, 0xFF000000u);
        scene_quad(white, 0xFFFFFFFFu);
        scene_quad(black, 0xFF000000u);
        scene_quad(block, 0xFF808080u);
        scene_end(NULL, 0);
        if (pass)
        {
            /* the blur amount: none on the block, all of it on the far wall */
            CHECK(spx(64, 64, 0) <= 10 && spx(20, 20, 0) >= 245, "dof debug: block %u, far wall %u (want 0 and 255)",
                spx(64, 64, 0), spx(20, 20, 0));
            break;
        }
        uint32_t wl = spx(61, 20, 0), wr = spx(67, 20, 0), in = spx(78, 64, 0), out = spx(82, 64, 0);
        CHECK(wl < 235 && wr > 20, "dof: the far wall's edge %u | %u (want it blurred)", wl, wr);
        CHECK(in >= 120 && in <= 136, "dof: the block's edge %u (want its gray, sharp)", in);
        CHECK(out <= 20, "dof: the far wall beside the block %u (want black, no gray smeared on)", out);
    }
    /* the sky (cleared depth) round the block: fully blurred, or none with dof_sky */
    gfx_fx_set("debug", 7.0f);
    for (int sky = 0; sky < 2; ++sky)
    {
        gfx_fx_set("dof_sky", (float)sky);
        scene_begin(1, 0xFF000000u);
        scene_quad(block, 0xFF808080u);
        scene_end(NULL, 0);
        uint32_t s = spx(20, 20, 0);
        CHECK(sky ? s <= 10 : s >= 245, "dof debug: the sky %u with dof_sky=%d (want %d)", s, sky, sky ? 0 : 255);
    }
    gfx_fx_set("dof_sky", 0.0f);
    gfx_fx_set("debug", 0.0f);
}

/* FXAA: a white wall's top edge sloping about 1 in 10 over black. Drawn, every pixel is white or
 * black; with FXAA the pixels along the edge take grays, and the rest stays as it was. It runs with
 * the scene effects off too. */
static void test_scene_fxaa(void)
{
    static const char* const WHAT[3] = { "off", "with the effects", "with the effects off" };
    float wall[4][3] = { { -8, 1.0f, 6 }, { 8, -0.6f, 6 }, { -8, -8, 6 }, { 8, -8, 6 } };
    for (int pass = 0; pass < 3; ++pass)
    {
        fx_only(NULL, 0.0f);
        gfx_fx_set("fx", pass == 2 ? 0.0f : 1.0f);
        gfx_fx_set("aa", pass ? 1.0f : 0.0f);
        scene_begin(1, 0xFF000000u);
        scene_quad(wall, 0xFFFFFFFFu);
        scene_end(NULL, 0);
        int grays = 0;
        for (int y = 40; y < 90; ++y)
            for (int x = 8; x < 120; ++x)
                grays += spx(x, y, 8) > 40 && spx(x, y, 8) < 215;
        uint32_t above = spx(64, 20, 8), below = spx(64, 110, 8);
        CHECK(above <= 5 && below >= 250, "fxaa %s: away from the edge %u and %u (want 0 and 255)", WHAT[pass], above, below);
        if (pass == 0)
            CHECK(grays == 0, "fxaa off: %d gray pixels along the edge (want none)", grays);
        else
            CHECK(grays >= 40, "fxaa %s: %d gray pixels along the edge (want it smoothed)", WHAT[pass], grays);
    }
    gfx_fx_set("aa", 0.0f);
    gfx_fx_set("fx", 1.0f);
}
#endif

/* The scene filter: a 1024x1024 scene of 8-pixel stripes drawn at 32x32 onto a large target (512x512, as the world onto
 * the back buffer) is gray, where one bilinear sample per pixel (every pixel lands on a white row) is white - the shimmer.
 * Onto a small target (this test's 32x32, as the game's 256x256 ones) it is that one sample, filter or not: the game never
 * filtered those, and rebuilding the scene's mips for each was a crowd's whole frame (from XI on Anything,
 * rubymatrix/xi-on-anything, tests/gfx_test.c). */
static void test_scene_filter(void)
{
    enum { B = 1024, L = 512 };
    static uint32_t big[L * L];
    GfxTex* large = gfx_tex_create(GFX_TEX_2D, 21, L, L, 1, GFX_USE_RT);
    const uint32_t lvp[6] = { 0, 0, L, L, 0, 0x3F800000u };
    GfxTex* rt = gfx_tex_create(GFX_TEX_2D, 21, B, B, 1, GFX_USE_RT);
    const uint32_t bvp[6] = { 0, 0, B, B, 0, 0x3F800000u };
    gfx_set_targets(rt, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xFF000000u, 1.0f, 0, bvp);
    static UiVert rows[B / 16][6];
    for (int i = 0; i < B / 16; ++i)
    {
        UiVert q[4];
        quad(q, 0, (float)(16 * i), B, (float)(16 * i + 8), 0xFFFFFFFFu, 0.5f);
        rows[i][0] = q[0], rows[i][1] = q[1], rows[i][2] = q[2], rows[i][3] = q[1], rows[i][4] = q[3], rows[i][5] = q[2];
    }
    GfxDraw stripes, d;
    defaults(&stripes);
    layout_ui(&stripes);
    stripes.u.vp[2] = stripes.u.vp[3] = B;
    memcpy(stripes.vp, bvp, sizeof bvp);
    stripes.fs.st[0] = (GfxStage){ 2, 0, 1, 1, 2, 0, 1, 1, 1, 0, 0, 2 };
    stripes.data[0] = rows, stripes.size[0] = sizeof rows;
    stripes.prim = GFX_TRIANGLELIST, stripes.count = 2 * (B / 16);
    gfx_draw(&stripes);
    GfxScene sc;
    memset(&sc, 0, sizeof sc); /* no camera: the filter alone */
    /* pass 0: filter on, onto the large target; 1: filter off, onto it; 2: filter on, onto the small target, the scene
     * drawn to again first (its mips behind, as the world between the game's small targets) */
    for (int pass = 0; pass < 3; ++pass)
    {
        gfx_fx_set("filter", pass == 1 ? 0.0f : 1.0f);
        if (pass == 2)
            gfx_draw(&stripes);
        gfx_scene_done(rt, &sc);
        if (pass < 2)
        {
            gfx_set_targets(large, 0, 0, NULL);
            gfx_clear(0, NULL, 1, 0xFF000000u, 1.0f, 0, lvp);
        }
        else
            gfx_set_targets(g_rt, 0, 0, g_ds);
        defaults(&d);
        layout_ui(&d);
        if (pass < 2)
        {
            d.u.vp[2] = d.u.vp[3] = L;
            memcpy(d.vp, lvp, sizeof lvp);
        }
        d.tex[0] = rt;
        d.samp[0] = (GfxSampler){ 3, 3, 3, 2, 2, 0, 0, 0, 0 }; /* CLAMP, LINEAR, no mips: the game's */
        d.fs.st[0] = (GfxStage){ 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2 };
        UiVert v[4];
        quad(v, 0, 0, W, H, 0xFFFFFFFFu, 0);
        for (int i = 0; i < 4; ++i) /* each screen pixel's center on a texel's center (row 32y + 16) */
            v[i].u += 0.5f / B, v[i].v += 0.5f / B;
        draw_ui(&d, v);
        uint32_t c;
        if (pass < 2)
        {
            gfx_tex_read(large, 0, 0, big, L * 4);
            c = big[16 * L + 16] & 255;
        }
        else
        {
            readback();
            c = px(16, 16) & 255;
        }
        if (pass == 0)
            CHECK(c >= 100 && c <= 156, "scene filter: stripes at 1/32 size %u (want gray)", c);
        else if (pass == 1)
            CHECK(c >= 250, "scene filter off: %u (want the white row one sample finds)", c);
        else
            CHECK(c >= 250, "scene filter onto a small target: %u (want the one sample, as the game's)", c);
        gfx_set_targets(rt, 0, 0, NULL);
    }
    gfx_fx_set("filter", 1.0f);
    gfx_set_targets(g_rt, 0, 0, g_ds);
    gfx_tex_destroy(rt);
    gfx_tex_destroy(large);
}

#ifdef VANACORE
/* The GPU crash data names the draw the card stopped at: made-up breadcrumbs of two lists (DXGI's own between them),
 * the first stopped at its first draw, matched to the notes kept as they were recorded. */
static char g_dred_text[4096];

static void dred_line(const char* line)
{
    strncat(g_dred_text, line, sizeof g_dred_text - strlen(g_dred_text) - 2);
    strcat(g_dred_text, "\n");
}

/* ... and still when lists were sent after the card stopped (before Present found it): they count as sent but DRED
 * never has them, as on a 2026-10-17 RTX 5080 report ("13 marks, 7 notes - they don't pair") */
/* The device lost (ID3D12Device5::RemoveDevice, as a driver timeout) and made again: gfx_shutdown lets go of everything the
 * back end made - Direct3D 12 makes one device per card in a process, so a child of it still held keeps the lost one, and
 * gfx_shutdown says so - gfx_init brings a new one up, and textures draw on it as before. The scene effects' and the sun's
 * targets, the shadow cache and its copies, the grass's and the crash data's notes are all up from the tests before. */
static void test_device_lost(void)
{
    if (!gfx12_lose_device())
    {
        printf("device lost: skipped (no ID3D12Device5)\n");
        return;
    }
    CHECK(gfx_lost(), "device lost: not found lost");
    gfx_tex_destroy(g_rt), gfx_tex_destroy(g_ds), g_rt = g_ds = NULL;
    const uint32_t failures = gfx_failures();
    /* what d3d8_device_lost lets go of before the game's gfx_shutdown: the grass's shared quad indices, the floors' buffers */
    grass_device_lost();
    floors_device_lost();
    CHECK(gfx_shutdown(), "device lost: the device is still held after the teardown (a child of it leaked)");
    CHECK(gfx_init(NULL, 1), "device lost: no new device");
    CHECK(!gfx_lost(), "device lost: still lost after gfx_init");
    g_rt = gfx_tex_create(GFX_TEX_2D, 21, W, H, 1, GFX_USE_RT);
    g_ds = gfx_tex_create(GFX_TEX_2D, 75, W, H, 1, GFX_USE_DEPTH);
    CHECK(g_rt && g_ds, "device lost: no targets on the new device");
    gfx_set_targets(g_rt, 0, 0, g_ds);
    const int fails = g_fails;
    test_texture();
    CHECK(g_fails == fails, "device lost: the texture test fails on the new device");
    CHECK(gfx_failures() == failures, "device lost: %u shaders or pipelines failed on the new device", gfx_failures() - failures);
}

static void test_dred_notes(void)
{
    static const int k_after[] = { 0, 1, 2, 3 };
    for (int a = 0; a < (int)(sizeof k_after / sizeof k_after[0]); ++a)
    {
        const char* t = g_dred_text;
        g_dred_text[0] = 0;
        if (!vanacore_gfx_dred_notes_test(dred_line, k_after[a]))
            return; /* GPU crash data on (FFXI_D3D12_DRED): its notes are the real ones */
        CHECK((strstr(t, "sent after the card stopped") != NULL) == (k_after[a] > 0),
            "dred notes: the lists sent after (%d) not said right\n%s", k_after[a], t);
        CHECK(strstr(t, "GPU:   in pass: the game's scene") != NULL, "dred notes: no pass named\n%s", t);
        CHECK(strstr(t, "the draw it stopped at: the game's: 1234 vertices (a triangle strip, vertices from 7") != NULL,
            "dred notes: the stopped draw not named\n%s", t);
        CHECK(strstr(t, "texture 256x256 format 21, onto 2048x2048 format 21") != NULL, "dred notes: no sizes\n%s", t);
        CHECK(strstr(t, "then the game's: 6 vertices") != NULL, "dred notes: the draw after not named\n%s", t);
        CHECK(strstr(t, "don't pair") == NULL && strstr(t, "not kept") == NULL, "dred notes: the lists not matched\n%s", t);
    }
}
#endif

/* A large render target the game samples itself (FFXI's character shadow, projected around the
 * player) with a mip filter reads its one level, not the empty chain the scene filter may add. */
static void test_large_target_mips(void)
{
    enum { B = 1024 };
    GfxTex* rt = gfx_tex_create(GFX_TEX_2D, 21, B, B, 1, GFX_USE_RT);
    const uint32_t bvp[6] = { 0, 0, B, B, 0, 0x3F800000u };
    gfx_set_targets(rt, 0, 0, NULL);
    gfx_clear(0, NULL, 1, 0xFFFF0000u, 1.0f, 0, bvp);
    gfx_set_targets(g_rt, 0, 0, g_ds);
    GfxDraw d;
    defaults(&d);
    layout_ui(&d);
    d.tex[0] = rt;
    d.samp[0] = (GfxSampler){ 3, 3, 3, 2, 2, 2, 0, 0, 0 }; /* LINEAR, mips LINEAR */
    d.fs.st[0] = (GfxStage){ 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2 };
    UiVert v[4];
    quad(v, 0, 0, W, H, 0xFFFFFFFFu, 0);
    draw_ui(&d, v);
    readback();
    CHECK(near(px(16, 16), 0xFFFF0000u, 2), "large target through a mip filter: %08x (want its red)", px(16, 16));
    gfx_tex_destroy(rt);
}

#ifdef VANACORE
/* Reflections stay off grass: a white wall over a floor; a grey floor shows the wall in it, a green one doesn't (the floor pixel's
 * own colour says). The floor's red at one pixel, with reflections as asked. */
static uint32_t refl_floor_red(uint32_t floor_color, float refl)
{
    fx_only(NULL, 0.0f);
    gfx_fx_set("refl", refl), gfx_fx_set("refl_wet", 1.0f), gfx_fx_set("refl_rough", 0.0f), gfx_fx_set("refl_dist", 60.0f);
    gfx_fx_set("refl_sun", 0.0f), gfx_fx_set("temporal", 0.0f), gfx_fx_set("refl_floors", 0.0f); /* no floors layer here */
    scene_begin(0, 0xFF000000u);
    float wall[4][3] = { { -8, 8, 6 }, { 8, 8, 6 }, { -8, -3, 6 }, { 8, -3, 6 } };
    float floor[4][3] = { { -8, -3, 6 }, { 8, -3, 6 }, { -8, -3, 0.6f }, { 8, -3, 0.6f } };
    scene_quad(wall, 0xFFFFFFFFu);
    scene_quad(floor, floor_color);
    scene_end(NULL, 0);
    const uint32_t r = spx(64, 112, 16);
    gfx_present(NULL);
    return r;
}

static void test_scene_refl_grass(void)
{
    const uint32_t grey = 0xFF606060u, green = 0xFF406828u;
    const uint32_t grey_off = refl_floor_red(grey, 0.0f), grey_on = refl_floor_red(grey, 2.0f);
    const uint32_t green_off = refl_floor_red(green, 0.0f), green_on = refl_floor_red(green, 2.0f);
    gfx_fx_set("refl", 0.0f), gfx_fx_set("temporal", 0.85f);
    printf("  reflections off grass: grey floor %u -> %u, green floor %u -> %u\n", grey_off, grey_on, green_off, green_on);
    CHECK(grey_on > grey_off + 15, "reflections: the grey floor %u with them, %u without (want it showing the wall)", grey_on, grey_off);
    CHECK(green_on <= green_off + 4, "reflections: the green floor %u with them, %u without (want none)", green_on, green_off);
}
#endif

static void test_scene_effects(void)
{
    test_large_target_mips();
    test_scene_ao(0);
    test_scene_ao(1);
    test_scene_ao_halo();
    test_scene_fog();
    test_scene_zonelines();
    test_scene_zoneline_styles();
    test_scene_bloom();
    test_scene_rays();
    test_scene_sun_map();
    test_scene_sun_turned();
#ifdef VANACORE
    test_scene_sun_smooth();
#endif
    test_scene_sun_dusk();
    test_scene_sun_hold();
    test_scene_sun_cache();
    test_scene_sun_rewritten();
    test_scene_sun_snapshot();
    test_scene_sun_snapshot_parts();
    test_scene_sun_instances();
    test_scene_glow();
#ifdef VANACORE
    test_scene_dlss();
    test_scene_dlss5();
#endif
    test_scene_temporal();
    test_scene_sun_sharp();
    test_scene_sun_mid();
#ifdef VANACORE
    test_scene_refl_grass();
    test_scene_outline();
    test_scene_outline_chars();
    test_scene_bands();
    test_scene_smooth();
    test_scene_dof();
    test_scene_fxaa();
#endif
    CHECK(gfx_failures() == 0, "scene effects: %u failures", gfx_failures());
    gfx_set_targets(g_rt, 0, 0, g_ds);
    test_scene_filter();
    gfx_tex_destroy(g_srt);
    gfx_tex_destroy(g_sds);
}

#ifdef VANACORE
int gfx_test_main(void)
{
    void* win = NULL;
#else
int main(int argc, char** argv)
{
    SDL_Window* win = NULL;
    if (argc > 1 && !strcmp(argv[1], "--window"))
    {
        if (!SDL_Init(SDL_INIT_VIDEO) || !(win = SDL_CreateWindow("gfx_test", 640, 360, 0)))
        {
            printf("no window: %s\n", SDL_GetError());
            return 1;
        }
    }
#endif
    gfx_set_sync_pipelines(1);
    setenv("FFXI_FX", "1", 1); /* the scene effects run only where a test asks (test_scene_effects) */
    setenv("FFXI_FX_FILE", "/nonexistent/fx.txt", 1); /* not the player's settings */
    if (!gfx_init(win, 1))
    {
        printf("no graphics back end\n");
        return 1;
    }
    g_rt = gfx_tex_create(GFX_TEX_2D, 21, W, H, 1, GFX_USE_RT);
    g_ds = gfx_tex_create(GFX_TEX_2D, 75, W, H, 1, GFX_USE_DEPTH);
    gfx_set_targets(g_rt, 0, 0, g_ds);
    test_clear();
    test_rhw_edges();
    test_texture();
    test_alpha();
    test_depth();
    test_decal_bias();
    test_lighting();
    test_pixel_lighting();
    test_fog();
    test_shaders();
    test_sweep();
    test_scene_effects();
#ifdef VANACORE
    g_fails += grass_test();
    test_dred_notes();
    test_device_lost();
#endif
    gfx_present(NULL);
#ifdef VANACORE
    /* the targets on the device the tests after this (d3d8_test's device lost) lose in their turn: none of this test's held */
    gfx_tex_destroy(g_rt), gfx_tex_destroy(g_ds), g_rt = g_ds = NULL;
#endif
#ifndef VANACORE
    if (win)
    {
        run_window(win);
        CHECK(gfx_failures() == 0, "window frames: %u failures", gfx_failures());
        SDL_DestroyWindow(win);
    }
#endif
    printf(g_fails ? "gfx_test: %d failed\n" : "gfx_test: ok\n", g_fails);
    return g_fails != 0;
}
