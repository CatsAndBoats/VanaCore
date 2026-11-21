/* The grass (grass.c) without the game: a flat piece of ground, half of its texture green and half brown, drawn the way the
 * game draws a zone's ground (a static mesh, noted to the grass), then the scene finished; tufts must stand on the green half
 * alone, and nowhere when the zone isn't one the grass works in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "grass.h"
#include "gfx.h"

#define TW 320
#define TH 240
#define BACKGROUND 0xFF000080u

typedef struct
{
    float x, y, z;
    uint32_t col;
    float u, v;
} GroundVert;

static int g_fails;

#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            printf("FAIL grass_test.c:%d: ", __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            g_fails++;                                                                                                \
        }                                                                                                             \
    } while (0)

static void look_at(float* m, const float eye[3], const float at[3], const float up[3])
{
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float l = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (int i = 0; i < 3; ++i)
        z[i] /= l;
    float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
    l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; ++i)
        x[i] /= l;
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    memset(m, 0, 64);
    for (int i = 0; i < 3; ++i)
        m[i * 4] = x[i], m[i * 4 + 1] = y[i], m[i * 4 + 2] = z[i];
    m[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    m[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    m[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    m[15] = 1.0f;
}

static void perspective(float* m, float fovy, float aspect, float n, float f)
{
    const float ys = 1.0f / tanf(fovy * 0.5f), xs = ys / aspect;
    memset(m, 0, 64);
    m[0] = xs, m[5] = ys, m[10] = f / (f - n), m[11] = 1.0f, m[14] = -n * f / (f - n);
}

static void mul(float* o, const float* a, const float* b)
{
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, 64);
}

/* the screen x a world point lands on */
static float screen_x(const float* vp, float x, float y, float z)
{
    const float cx = x * vp[0] + y * vp[4] + z * vp[8] + vp[12], cw = x * vp[3] + y * vp[7] + z * vp[11] + vp[15];
    return (cx / cw * 0.5f + 0.5f) * TW;
}

/* ... and the screen y (down) */
static float screen_y(const float* vp, float x, float y, float z)
{
    const float cy = x * vp[1] + y * vp[5] + z * vp[9] + vp[13], cw = x * vp[3] + y * vp[7] + z * vp[11] + vp[15];
    return (0.5f - cy / cw * 0.5f) * TH;
}

/* GRASS_TEST_PERF=1: a field as big as La Theine's round the player (16 tiles of one 50 x 50 mesh, 12 x 12 quads each), with a
 * player's heaviest settings (radius 60, density 12, limit 100000), timed: the CPU a frame takes noting the ground and making and
 * drawing the grass, its first frame apart (the kept tufts are made then), and the pixels it covers (the same field either way) */
static void grass_perf(GfxTex* rt, GfxTex* ds, const uint32_t* tex, uint64_t hash, const uint32_t vpd[6])
{
    enum { Q = 12, TILES = 4 };
    static GroundVert pv[Q * Q * 6];
    int nv = 0;
    for (int cz = 0; cz < Q; ++cz)
        for (int cx = 0; cx < Q; ++cx)
        {
            const float step = 50.0f / Q, x0 = step * (float)cx, x1 = x0 + step, z0 = step * (float)cz, z1 = z0 + step;
            const float y00 = 0.4f * sinf(x0 * 0.3f) * cosf(z0 * 0.2f), y10 = 0.4f * sinf(x1 * 0.3f) * cosf(z0 * 0.2f);
            const float y01 = 0.4f * sinf(x0 * 0.3f) * cosf(z1 * 0.2f), y11 = 0.4f * sinf(x1 * 0.3f) * cosf(z1 * 0.2f);
            const GroundVert q[4] = { { x0, y00, z0, 0xFF808080u, 0.1f, 0.1f }, { x1, y10, z0, 0xFF808080u, 0.4f, 0.1f },
                { x0, y01, z1, 0xFF808080u, 0.1f, 0.4f }, { x1, y11, z1, 0xFF808080u, 0.4f, 0.4f } };
            const int tri[6] = { 0, 1, 2, 1, 3, 2 };
            for (int i = 0; i < 6; ++i)
                pv[nv++] = q[tri[i]];
        }
    GfxScene s;
    memset(&s, 0, sizeof s);
    const float eye[3] = { 0, -14.0f, -30.0f }, at[3] = { 0, 0, 20.0f }, up[3] = { 0, -1, 0 };
    look_at(s.view, eye, at, up);
    perspective(s.proj, 1.0471975f, (float)TW / (float)TH, 0.1f, 1000.0f);
    memcpy(s.vp, vpd, 6 * sizeof(uint32_t));
    s.focus[3] = 1.0f;
    float vp[16];
    mul(vp, s.view, s.proj);
    GfxDraw tile[TILES * TILES];
    for (int t = 0; t < TILES * TILES; ++t)
    {
        GfxDraw* d = &tile[t];
        memset(d, 0, sizeof *d);
        d->vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
        d->vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
        d->vs.el[GFX_R_TEXCOORD0] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
        d->u.offset[GFX_R_POSITION] = 0, d->u.offset[GFX_R_DIFFUSE] = 12, d->u.offset[GFX_R_TEXCOORD0] = 16;
        d->u.stride[0] = (int32_t)sizeof(GroundVert);
        d->cpu[0] = pv, d->cpu_size[0] = sizeof pv;
        float T[16];
        memset(T, 0, sizeof T);
        T[0] = T[5] = T[10] = T[15] = 1.0f;
        T[12] = -100.0f + 50.0f * (float)(t % TILES), T[14] = -100.0f + 50.0f * (float)(t / TILES);
        mul(d->u.wvp, T, vp);
    }
    GrassSrc src = { hash, 9, 0, (uint32_t)nv, NULL, 0, 0, GFX_TRIANGLELIST, (uint32_t)nv / 3 };
    GrassSettings gs = { 1, "", 60.0f, 12.0f, 0.55f, 0.8f, 0.3f, 1.0f, 0.1f, 1.0f, 0, 0, 100000, 55.0f };
    grass_configure(&gs);
    grass_set_zone(100);
    grass_texture(hash, 21, 64, 64, tex, 64 * 4);
    enum { FRAMES = 120 };
    double ms[FRAMES];
    for (int f = 0; f < FRAMES; ++f)
    {
        const uint64_t t0 = gfx_now_ns();
        gfx_set_targets(rt, 0, 0, ds);
        gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
        for (int t = 0; t < TILES * TILES; ++t)
            grass_note(&tile[t], &src);
        grass_scene(rt, ds, &s);
        ms[f] = (double)(gfx_now_ns() - t0) * 1e-6;
        if (f + 1 < FRAMES)
            gfx_present(NULL);
    }
    static uint32_t px[TW * TH];
    gfx_tex_read(rt, 0, 0, px, TW * 4);
    long covered = 0;
    for (int i = 0; i < TW * TH; ++i)
        covered += px[i] != BACKGROUND;
    if (getenv("GRASS_TEST_BMP"))
    {
        char path[512];
        snprintf(path, sizeof path, "%s.perf", getenv("GRASS_TEST_BMP"));
        FILE* f = fopen(path, "wb");
        if (f)
        {
            const uint32_t wh[2] = { TW, TH };
            fwrite(wh, 4, 2, f), fwrite(px, 4, TW * TH, f), fclose(f);
        }
    }
    gfx_present(NULL);
    double sum = 0.0, worst = 0.0;
    for (int f = 10; f < FRAMES; ++f)
        sum += ms[f], worst = ms[f] > worst ? ms[f] : worst;
    printf("  grass perf (radius 60, density 12, limit 100000): first frames %.2f %.2f %.2f %.2f %.2f ms, then %.2f ms a frame (worst %.2f), "
           "%ld pixels\n", ms[0], ms[1], ms[2], ms[3], ms[4], sum / (FRAMES - 10), worst, covered);
    gs.enabled = 0;
    grass_configure(&gs);
}

/* the ground drawn once more and the grass made: the pixels its tufts cover */
static uint32_t g_px[TW * TH]; /* grass_pixels' picture */

static long grass_pixels(GfxTex* rt, GfxTex* ds, const GfxDraw* d, const GrassSrc* src, const GfxScene* s, const uint32_t vpd[6])
{
    gfx_set_targets(rt, 0, 0, ds);
    gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
    grass_note(d, src);
    grass_scene(rt, ds, s);
    gfx_tex_read(rt, 0, 0, g_px, TW * 4);
    long n = 0;
    for (int i = 0; i < TW * TH; ++i)
        n += g_px[i] != BACKGROUND;
    return n;
}

/* A floor over the ground covers it: none grows there (a building's floor laid over the ground the tufts came up through), with the
 * ground's own floor under the tufts or none; one 3 over it (a bridge's) is out of the tallest tuft's reach and they grow under it.
 * A stone floor only 0.15 over it is a building's too (Windurst Waters' rooms: within FLOOR_OVER it was taken for the ground),
 * alone or over the ground's own floor (the room's case: only the nearest floor, the ground's, was asked); a grass one that near
 * is the ground's own collision lying a little over it and grows, but not over the ground's own floor (two floors stacked).
 * grass.cover.show draws the covered ones, red. Mesh 0 is a grass floor under all the ground (x and z -8 to 8), mesh 1 the same
 * of stone; matrix 0 lays it at the ground, 1 0.6 over it, 2 3 over it, 3 0.15 over it (y is down) */
static void cover_test(GfxTex* rt, GfxTex* ds, const GfxDraw* d, const GrassSrc* src, const GfxScene* s, const uint32_t vpd[6],
    GrassSettings* gs)
{
    static const float ftris[4 * 9] = { -8, 0, -8, 8, 0, -8, -8, 0, 8, 8, 0, -8, 8, 0, 8, -8, 0, 8, -8, 0, -8, 8, 0, -8, -8, 0, 8, 8, 0,
        -8, 8, 0, 8, -8, 0, 8 };
    static const unsigned fmesh[3] = { 0, 2, 4 };
    static const unsigned char terrain[4] = { 2, 2, 5, 5 }; /* grass, stone */
    static const float mat[4][12] = { { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.6f, 0 },
        { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -3.0f, 0 }, { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, -0.15f, 0 } };
    static const unsigned pairs[12] = { 0, 0, 1, 0, 2, 0, 3, 1, 0, 0, 3, 0 };
    GrassFloors f = { 100, ftris, terrain, 4, fmesh, 2, &mat[0][0], 4, pairs, 1 };
    long n[9], red = 0;
    gs->walkable = 1;
    grass_configure(gs);
    grass_set_zone(100);
    grass_set_floors(&f); /* the ground's own floor */
    n[0] = grass_pixels(rt, ds, d, src, s, vpd);
    f.npairs = 2; /* and one 0.6 over it */
    grass_set_floors(&f);
    n[1] = grass_pixels(rt, ds, d, src, s, vpd);
    f.pairs = &pairs[2], f.npairs = 1; /* the one over it alone: no floor of the ground's own under it */
    grass_set_floors(&f);
    n[2] = grass_pixels(rt, ds, d, src, s, vpd);
    f.pairs = &pairs[4]; /* one 3 over it alone */
    grass_set_floors(&f);
    n[3] = grass_pixels(rt, ds, d, src, s, vpd);
    f.pairs = &pairs[6]; /* a stone floor 0.15 over it alone */
    grass_set_floors(&f);
    n[5] = grass_pixels(rt, ds, d, src, s, vpd);
    f.npairs = 2; /* ... over the ground's own */
    grass_set_floors(&f);
    n[7] = grass_pixels(rt, ds, d, src, s, vpd);
    f.pairs = &pairs[10], f.npairs = 1; /* a grass one alone */
    grass_set_floors(&f);
    n[6] = grass_pixels(rt, ds, d, src, s, vpd);
    f.pairs = &pairs[8], f.npairs = 2; /* ... over the ground's own */
    grass_set_floors(&f);
    n[8] = grass_pixels(rt, ds, d, src, s, vpd);
    f.pairs = &pairs[2], f.npairs = 1, gs->show_cover = 1;
    grass_configure(gs);
    grass_set_floors(&f);
    n[4] = grass_pixels(rt, ds, d, src, s, vpd);
    for (int i = 0; i < TW * TH; ++i)
    {
        const uint32_t r = (g_px[i] >> 16) & 255, gr = (g_px[i] >> 8) & 255, b = g_px[i] & 255;
        red += g_px[i] != BACKGROUND && r > 2 * gr && r > 2 * b;
    }
    printf("  grass: on its floor %ld pixels; a floor 0.6 over it too %ld, over it alone %ld, 3 over it %ld; 0.15 over it: stone %ld, "
           "stone over its own %ld, grass %ld, grass over its own %ld; covered shown %ld, %ld red\n", n[0], n[1], n[2], n[3], n[5], n[7],
        n[6], n[8], n[4], red);
    CHECK(n[0] > 400, "on its own floor: %ld pixels", n[0]);
    CHECK(n[1] == 0, "a floor 0.6 over the ground's own: %ld pixels", n[1]);
    CHECK(n[2] == 0, "a floor 0.6 over the ground, none under it: %ld pixels", n[2]);
    CHECK(n[3] * 20 >= n[0] * 19, "a floor 3 over the ground: %ld pixels against %ld", n[3], n[0]);
    CHECK(n[5] == 0, "a stone floor 0.15 over the ground: %ld pixels", n[5]);
    CHECK(n[7] == 0, "a stone floor 0.15 over the ground's own: %ld pixels", n[7]);
    CHECK(n[6] * 20 >= n[0] * 19, "a grass floor 0.15 over the ground: %ld pixels against %ld", n[6], n[0]);
    CHECK(n[8] == 0, "a grass floor 0.15 over the ground's own: %ld pixels", n[8]);
    CHECK(n[4] * 20 >= n[0] * 19 && red * 2 > n[4], "the covered tufts shown: %ld pixels against %ld, %ld red", n[4], n[0], red);
    gs->show_cover = 0, gs->walkable = 0;
    grass_configure(gs);
    grass_set_floors(NULL);
}

/* grass.moss: on the shallow water grass.nowater leaves bare (Dragon's Aery's meadow), cushions of moss in the tufts' place; none on
 * grass ground (its tufts as they were), none without grass.nowater (the tufts grow there). The floors lie under the green half
 * (x -8 to 0): mesh 0 its near quarter (z -8 to 0), mesh 1 its far one, each of terrain t[0] and t[1] (2 grass, 8 shallow water) */
static void moss_test(GfxTex* rt, GfxTex* ds, const GfxDraw* d, const GrassSrc* src, const GfxScene* s, const uint32_t vpd[6],
    GrassSettings* gs)
{
    static const float ftris[4 * 9] = { -8, 0, -8, 0, 0, -8, -8, 0, 0, 0, 0, -8, 0, 0, 0, -8, 0, 0, -8, 0, 0, 0, 0, 0, -8, 0, 8, 0, 0, 0, 0,
        0, 8, -8, 0, 8 };
    static const unsigned fmesh[3] = { 0, 2, 4 }, pairs[4] = { 0, 0, 1, 0 };
    static const float mat[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
    static const unsigned char t[3][4] = { { 2, 2, 2, 2 }, { 2, 2, 8, 8 }, { 8, 8, 8, 8 } };
    GrassFloors f = { 100, ftris, t[0], 4, fmesh, 2, mat, 1, pairs, 2 };
    /* n: grass alone, moss off and on; grass near and water far, moss off and on; water alone, moss off and on, moss on and nowater off */
    static const int case_t[7] = { 0, 0, 1, 1, 2, 2, 2 }, case_moss[7] = { 0, 1, 0, 1, 0, 1, 1 }, case_nowater[7] = { 1, 1, 1, 1, 1, 1, 0 };
    long n[7], rgb[3] = { 0, 0, 0 };
    grass_set_zone(100);
    for (int c = 0; c < 7; ++c)
    {
        gs->walkable = 1, gs->nowater = case_nowater[c], gs->moss = case_moss[c];
        grass_configure(gs);
        f.terrain = t[case_t[c]];
        grass_set_floors(&f);
        n[c] = grass_pixels(rt, ds, d, src, s, vpd);
        for (int i = 0; i < TW * TH && c == 5; ++i) /* the moss alone: its colour */
            if (g_px[i] != BACKGROUND)
                rgb[0] += (g_px[i] >> 16) & 255, rgb[1] += (g_px[i] >> 8) & 255, rgb[2] += g_px[i] & 255;
    }
    printf("  grass: moss: on grass %ld pixels (moss on %ld); grass near, water far %ld (moss on %ld); water %ld (moss on %ld, colour %ld %ld "
           "%ld; nowater off %ld)\n", n[0], n[1], n[2], n[3], n[4], n[5], rgb[0], rgb[1], rgb[2], n[6]);
    CHECK(n[0] > 400 && n[1] == n[0], "moss on grass ground: %ld pixels, %ld without it", n[1], n[0]);
    CHECK(n[2] > 0 && n[2] < n[0] && n[4] == 0, "no grass on shallow water: %ld pixels half on it, %ld all on it", n[2], n[4]);
    CHECK(n[3] > n[2] && n[5] > 50, "no moss on shallow water: %ld pixels half on it (%ld bare), %ld all on it", n[3], n[2], n[5]);
    CHECK(rgb[1] > rgb[0] && rgb[1] > rgb[2], "moss colour %ld %ld %ld is not green", rgb[0], rgb[1], rgb[2]);
    CHECK(n[6] == n[0], "moss with grass.nowater off: %ld pixels, the grass alone %ld", n[6], n[0]);
    gs->walkable = 0, gs->nowater = 0, gs->moss = 0;
    grass_configure(gs);
    grass_set_floors(NULL);
}

/* grass.dirt, a zone's dirt level, scales every grass test's bar by 2 to the (dirt - 50) / 50. An olive green (100, 110, 80: green
 * 10% over red and 38% over blue) is grass at 50 and dirt at 100 (it would need 44% over blue); a dull yellow-olive (110, 106, 88:
 * hue 49, saturation 0.20, under dry grass's 0.25) is dirt at 50 and dry grass at 1 (the bar about 0.13) */
static void dirt_test(GfxTex* rt, GfxTex* ds, const GfxDraw* d, const GrassSrc* src, const GfxScene* s, const uint32_t vpd[6],
    GrassSettings* gs)
{
    static uint32_t olive[64 * 64], dull[64 * 64];
    for (int i = 0; i < 64 * 64; ++i)
        olive[i] = 0xFF646E50u, dull[i] = 0xFF6E6A58u;
    GrassSrc so = *src, sd = *src;
    so.tex_hash = 0xD1D7000000000A01ull, sd.tex_hash = 0xD1D7000000000A02ull;
    long n[5];
    gs->dry = 1;
    grass_configure(gs);
    grass_set_zone(100);
    grass_texture(so.tex_hash, 21, 64, 64, olive, 64 * 4);
    grass_texture(sd.tex_hash, 21, 64, 64, dull, 64 * 4);
    n[0] = grass_pixels(rt, ds, d, &so, s, vpd);
    n[1] = grass_pixels(rt, ds, d, &sd, s, vpd);
    gs->dirt = 100.0f;
    grass_configure(gs);
    n[2] = grass_pixels(rt, ds, d, &so, s, vpd);
    gs->dirt = 1.0f;
    grass_configure(gs);
    n[3] = grass_pixels(rt, ds, d, &so, s, vpd);
    n[4] = grass_pixels(rt, ds, d, &sd, s, vpd);
    printf("  grass: dirt level 50: olive green %ld pixels, dull olive %ld; 100: olive green %ld; 1: olive green %ld, dull olive %ld\n", n[0],
        n[1], n[2], n[3], n[4]);
    CHECK(n[0] > 400, "olive green at dirt level 50: %ld pixels", n[0]);
    CHECK(n[1] == 0, "dull olive at dirt level 50: %ld pixels", n[1]);
    CHECK(n[2] == 0, "olive green at dirt level 100: %ld pixels", n[2]);
    CHECK(n[3] * 20 >= n[0] * 19, "olive green at dirt level 1: %ld pixels against %ld", n[3], n[0]);
    CHECK(n[4] > 400, "dull olive at dirt level 1: %ld pixels", n[4]);
    gs->dirt = 0.0f, gs->dry = 0;
    grass_configure(gs);
}

/* the halves of grass_test's picture: the screen x between the ground's green half (x -4) and its brown (x 4), and which is left */
static float ground_mid(const GfxScene* s, int* green_is_left)
{
    float vp[16];
    mul(vp, s->view, s->proj);
    const float sx_green = screen_x(vp, -4, 0, 0), sx_brown = screen_x(vp, 4, 0, 0);
    *green_is_left = sx_green < sx_brown;
    return 0.5f * (sx_green + sx_brown);
}

/* grass.shade: a forest floor's texture, green on one half and in shadow on the other (20, 20, 16: too dark to read as any
 * colour), grows on the green alone with it off and on both halves with it on. Bhaflau Thickets' floors are green on a third
 * of their texels and dark or olive on the rest, and grew on the third */
static void shade_test(GfxTex* rt, GfxTex* ds, const GfxDraw* d, const GrassSrc* src, const GfxScene* s, const uint32_t vpd[6],
    GrassSettings* gs)
{
    int green_is_left;
    const float mid = ground_mid(s, &green_is_left);
    static uint32_t floor[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            floor[y * 64 + x] = x < 32 ? 0xFF3C8C32u : 0xFF141410u;
    const uint64_t hf = 0x5AADE00000000001ull;
    grass_texture(hf, 21, 64, 64, floor, 64 * 4);
    GrassSrc sf = *src;
    sf.tex_hash = hf;
    long on[2][2];
    double lum[2][2]; /* the tuft pixels' mean brightness (the brightest channel) on each half: a tuft in shade isn't black */
    for (int k = 0; k < 2; ++k)
    {
        gs->shade = k;
        grass_configure(gs);
        grass_pixels(rt, ds, d, &sf, s, vpd);
        on[k][0] = on[k][1] = 0;
        lum[k][0] = lum[k][1] = 0;
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
                if (g_px[y * TW + x] != BACKGROUND)
                {
                    const uint32_t p = g_px[y * TW + x];
                    const int half = ((float)x < mid) == green_is_left ? 0 : 1;
                    const uint32_t r = (p >> 16) & 255, gg = (p >> 8) & 255, b = p & 255;
                    ++on[k][half];
                    lum[k][half] += r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
                }
        for (int h = 0; h < 2; ++h)
            lum[k][h] = on[k][h] ? lum[k][h] / (double)on[k][h] : 0.0;
        printf("  grass: shade %s, green beside shadow: %ld tuft pixels on the green half (brightness %.0f), %ld on the dark (%.0f)\n",
               k ? "on" : "off", on[k][0], lum[k][0], on[k][1], lum[k][1]);
    }
    CHECK(on[0][0] > 400 && on[0][1] * 20 < on[0][0], "shade off, green beside shadow: %ld pixels on the green, %ld on the dark", on[0][0], on[0][1]);
    CHECK(on[1][0] > 400 && on[1][1] > 400, "shade on, green beside shadow: %ld pixels on the green, %ld on the dark", on[1][0], on[1][1]);
    CHECK(lum[1][1] >= 0.3 * lum[1][0], "shade on, the tufts in shadow aren't black: brightness %.0f on the dark half to %.0f on the green",
          lum[1][1], lum[1][0]);
    gs->shade = 0;
    grass_configure(gs);
}

/* grass.freecam: the player far off (600, 600: no ground in reach) and the free camera 9 over the middle of the ground (y is
 * down): the tufts grow round the ground under it (found from the grass's own ground) as they do round a player standing there;
 * round the camera's own height they'd be past the height band near it. With the zone's floors known, the floor under it is that
 * ground: one 30 below the drawn ground (a pit's) takes the band with it, and next to nothing grows. Let go, round the player again */
static void freecam_test(GfxTex* rt, GfxTex* ds, const GfxDraw* d, const GrassSrc* src, const GfxScene* s, const uint32_t vpd[6])
{
    static const float ftris[2 * 9] = { -8, 0, -8, 8, 0, -8, -8, 0, 8, 8, 0, -8, 8, 0, 8, -8, 0, 8 };
    static const unsigned fmesh[2] = { 0, 2 };
    static const float fmat[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 30, 0 };
    static const unsigned fpairs[2] = { 0, 0 };
    static const unsigned char terrain[2] = { 2, 2 };
    const GrassFloors f = { 100, ftris, terrain, 2, fmesh, 1, fmat, 1, fpairs, 1 };
    GfxScene away = *s;
    away.focus[0] = 600.0f, away.focus[2] = 600.0f;
    long n[5];
    n[0] = grass_pixels(rt, ds, d, src, s, vpd);
    n[1] = grass_pixels(rt, ds, d, src, &away, vpd);
    grass_set_centre(0.0f, -9.0f, 0.0f, 1);
    n[2] = grass_pixels(rt, ds, d, src, &away, vpd);
    grass_set_floors(&f);
    n[3] = grass_pixels(rt, ds, d, src, &away, vpd);
    grass_set_floors(NULL);
    grass_set_centre(0.0f, 0.0f, 0.0f, 0);
    n[4] = grass_pixels(rt, ds, d, src, &away, vpd);
    printf("  grass: free camera: round the player %ld pixels, the player far off %ld, round the camera %ld, over a floor 30 down %ld, "
           "let go %ld\n", n[0], n[1], n[2], n[3], n[4]);
    CHECK(n[0] > 400, "round the player: %ld pixels", n[0]);
    CHECK(n[1] == 0, "the player far off: %ld pixels", n[1]);
    CHECK(n[2] * 20 >= n[0] * 19, "round the free camera: %ld pixels against %ld round the player", n[2], n[0]);
    CHECK(n[3] * 10 < n[2], "the floor under the camera wasn't the ground it grew round: %ld pixels against %ld", n[3], n[2]);
    CHECK(n[4] == 0, "the free camera let go, the player far off: %ld pixels", n[4]);
}

/* A hillside seen from the side: a strip of the green ground (x -8 to 8, z -1 to 1) rising 25 degrees along x (y = -k x, y down),
 * the camera far off down z, the ground itself not drawn. Each end of a tuft's quads stands on the slope, so no tuft reaches more
 * than its 0.04 sink below it. Standing level, a quad 0.8 wide sank its uphill end 0.19 into the ground (its blades cut off along a
 * line, in game) and hung its downhill end as far over it (Batallia Downs' hills) */
static void slope_test(GfxTex* rt, GfxTex* ds, const GfxDraw* d, const GrassSrc* src, const uint32_t vpd[6])
{
    const float k = 0.4663f; /* tan 25 degrees */
    static GroundVert sv[8 * 6];
    int nv = 0;
    for (int cx = 0; cx < 8; ++cx)
    {
        const float x0 = -8.0f + 2.0f * (float)cx, x1 = x0 + 2.0f;
        const GroundVert q[4] = { { x0, -k * x0, -1, 0xFF808080u, 0.25f, 0.25f }, { x1, -k * x1, -1, 0xFF808080u, 0.25f, 0.25f },
            { x0, -k * x0, 1, 0xFF808080u, 0.25f, 0.75f }, { x1, -k * x1, 1, 0xFF808080u, 0.25f, 0.75f } };
        const int tri[6] = { 0, 1, 2, 1, 3, 2 };
        for (int i = 0; i < 6; ++i)
            sv[nv++] = q[tri[i]];
    }
    GfxScene s;
    memset(&s, 0, sizeof s);
    const float eye[3] = { 0, 0, -200 }, at[3] = { 0, 0, 0 }, up[3] = { 0, -1, 0 };
    look_at(s.view, eye, at, up);
    perspective(s.proj, 0.064f, (float)TW / (float)TH, 1.0f, 1000.0f);
    memcpy(s.vp, vpd, sizeof s.vp);
    s.focus[3] = 1;
    float vp[16];
    mul(vp, s.view, s.proj);
    GfxDraw dh = *d;
    memcpy(dh.u.wvp, vp, 64);
    dh.cpu[0] = sv, dh.cpu_size[0] = sizeof sv;
    GrassSrc sh = *src;
    sh.vb_id = 8, sh.nverts = (uint32_t)nv, sh.count = (uint32_t)nv / 3;
    const long n = grass_pixels(rt, ds, &dh, &sh, &s, vpd);
    if (getenv("GRASS_TEST_BMP"))
    {
        char path[512];
        snprintf(path, sizeof path, "%s.slope", getenv("GRASS_TEST_BMP"));
        FILE* f = fopen(path, "wb");
        if (f)
        {
            const uint32_t wh[2] = { TW, TH };
            fwrite(wh, 4, 2, f), fwrite(g_px, 4, TW * TH, f), fclose(f);
        }
    }
    /* the slope's line on the screen, and how many pixels a unit is: a tuft's pixel more than 0.1 under the line is in the ground */
    const float ax = screen_x(vp, -8, 8 * k, 0), ay = screen_y(vp, -8, 8 * k, 0), bx = screen_x(vp, 8, -8 * k, 0), by = screen_y(vp, 8, -8 * k, 0);
    const float unit = screen_y(vp, 0, 1, 0) - screen_y(vp, 0, 0, 0);
    long under = 0;
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x)
            if (g_px[y * TW + x] != BACKGROUND)
            {
                const float line = ay + (by - ay) * (((float)x + 0.5f) - ax) / (bx - ax);
                under += ((float)y + 0.5f) - line > 0.1f * unit;
            }
    printf("  grass: on a 25 degree slope, %ld tuft pixels, %ld of them under the ground\n", n, under);
    CHECK(n > 400, "on a slope: %ld pixels", n);
    CHECK(under * 100 < n, "on a slope, tufts reach into the ground: %ld of %ld pixels", under, n);
}

int grass_test(void)
{
    g_fails = 0;
    GfxTex* rt = gfx_tex_create(GFX_TEX_2D, 21, TW, TH, 1, GFX_USE_RT);
    GfxTex* ds = gfx_tex_create(GFX_TEX_2D, 75, TW, TH, 1, GFX_USE_DEPTH);
    CHECK(rt && ds, "targets");
    if (!rt || !ds)
        return 1;

    /* the ground texture: green on the left half, brown on the right */
    static uint32_t tex[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            tex[y * 64 + x] = x < 32 ? 0xFF3C8C32u : 0xFF8C643Cu;
    const uint64_t hash = 0x1234567890abcdefull;

    /* the ground: 16 x 16, a 4 x 4 grid of quads, y = 0, u running with x */
    static GroundVert verts[4 * 4 * 6];
    int nv = 0;
    for (int cz = 0; cz < 4; ++cz)
        for (int cx = 0; cx < 4; ++cx)
        {
            const float x0 = -8.0f + 4.0f * (float)cx, x1 = x0 + 4.0f, z0 = -8.0f + 4.0f * (float)cz, z1 = z0 + 4.0f;
            const GroundVert q[4] = { { x0, 0, z0, 0xFF808080u, (x0 + 8) / 16, (z0 + 8) / 16 },
                { x1, 0, z0, 0xFF808080u, (x1 + 8) / 16, (z0 + 8) / 16 }, { x0, 0, z1, 0xFF808080u, (x0 + 8) / 16, (z1 + 8) / 16 },
                { x1, 0, z1, 0xFF808080u, (x1 + 8) / 16, (z1 + 8) / 16 } };
            const int tri[6] = { 0, 1, 2, 1, 3, 2 };
            for (int i = 0; i < 6; ++i)
                verts[nv++] = q[tri[i]];
        }

    GfxScene s;
    memset(&s, 0, sizeof s);
    /* GRASS_TEST_CLOSE=1: the camera in among the tufts, for a look at one's edges (the checks below are for the far view) */
    const int close_up = getenv("GRASS_TEST_CLOSE") != NULL;
    const float eye[3] = { 0, close_up ? -0.5f : -9.0f, close_up ? -3.0f : -12.0f }, at[3] = { 0, 0, close_up ? -2.0f : 0.0f },
                up[3] = { 0, -1, 0 };
    look_at(s.view, eye, at, up);
    perspective(s.proj, 1.0471975f, (float)TW / (float)TH, 0.1f, 1000.0f);
    const uint32_t vpd[6] = { 0, 0, TW, TH, 0, 0x3F800000u };
    memcpy(s.vp, vpd, sizeof vpd);
    if (getenv("GRASS_TEST_PERF"))
        grass_perf(rt, ds, tex, hash, vpd);
    s.focus[0] = 0, s.focus[1] = 0, s.focus[2] = 0, s.focus[3] = 1;
    float vp[16];
    mul(vp, s.view, s.proj);

    GfxDraw d;
    memset(&d, 0, sizeof d);
    memcpy(d.u.wvp, vp, 64);
    d.vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d.vs.el[GFX_R_TEXCOORD0] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d.u.offset[GFX_R_POSITION] = 0, d.u.offset[GFX_R_DIFFUSE] = 12, d.u.offset[GFX_R_TEXCOORD0] = 16;
    d.u.stride[0] = (int32_t)sizeof(GroundVert);
    d.cpu[0] = verts, d.cpu_size[0] = sizeof verts;
    GrassSrc src = { hash, 7, 0, (uint32_t)nv, NULL, 0, 0, GFX_TRIANGLELIST, (uint32_t)nv / 3 };

    GrassSettings gs = { 1, "", 15.0f, 6.0f, 0.6f, 0.8f, 0.3f, 1.0f, 0.0f, 1.0f, 0, 0, 24000, 40.0f };
    gs.build_ms = -1.0f; /* every square the frame it's wanted: the checks read the pixels of the first frame */
    grass_configure(&gs);
    grass_set_zone(100);
    grass_texture(hash, 21, 64, 64, tex, 64 * 4);

    /* GRASS_TEST_PNG=1: a cut-out texture (black where transparent) logged as a plant sheet, which writes it in full to
     * logs\grass\<hash>.png: 32 x 32, a green diagonal band with alpha 255 on black with alpha 0 */
    if (getenv("GRASS_TEST_PNG"))
    {
        static uint32_t cut[32 * 32];
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                cut[y * 32 + x] = (x + y) % 16 < 8 ? 0xFF30A040u : 0x00000000u;
        gs.log = 1;
        grass_configure(&gs);
        grass_texture(0xFEEDFACE12345678ull, 21, 32, 32, cut, 32 * 4);
        gs.log = 0;
        grass_configure(&gs);
    }

    /* which side of the screen the green half of the ground is on */
    const float sx_green = screen_x(vp, -4, 0, 0), sx_brown = screen_x(vp, 4, 0, 0), mid = 0.5f * (sx_green + sx_brown);
    const int green_is_left = sx_green < sx_brown;

    static uint32_t px[TW * TH];
    for (int pass = 0; pass < 2; ++pass)
    {
        grass_set_zone(pass ? 0 : 100);
        gfx_set_targets(rt, 0, 0, ds);
        gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
        grass_note(&d, &src);
        grass_scene(rt, ds, &s);
        gfx_tex_read(rt, 0, 0, px, TW * 4);
        long on_green = 0, on_brown = 0, red = 0, green = 0, blue = 0;
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
            {
                const uint32_t c = px[y * TW + x];
                if (c == BACKGROUND)
                    continue;
                const int left = (float)x < mid;
                if (left == green_is_left)
                    ++on_green;
                else
                    ++on_brown;
                red += (c >> 16) & 255, green += (c >> 8) & 255, blue += c & 255;
            }
        if (!pass && getenv("GRASS_TEST_BMP")) /* the picture, for a look: raw BGRA after a 32-bit width/height header */
        {
            FILE* f = fopen(getenv("GRASS_TEST_BMP"), "wb");
            if (f)
            {
                const uint32_t wh[2] = { TW, TH };
                fwrite(wh, 4, 2, f), fwrite(px, 4, TW * TH, f), fclose(f);
            }
        }
        if (!pass)
        {
            printf("  grass: %ld tuft pixels on the green half, %ld on the brown\n", on_green, on_brown);
            CHECK(on_green > 400, "tufts on the green ground: %ld pixels", on_green);
            CHECK(on_brown * 20 < on_green, "tufts on the brown ground: %ld pixels against %ld", on_brown, on_green);
            CHECK(green > red && green > blue, "tuft colour %ld %ld %ld is not green", red, green, blue);
        }
        else
            CHECK(on_green + on_brown == 0, "grass outside its zone: %ld pixels", on_green + on_brown);
    }
    /* the game rewrites the ground's buffer under the same draw (a vertex nudged): the mesh is read again and the grass stays; it
     * used to be left out for good, the grass gone until it was switched off and on */
    grass_set_zone(100);
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass)
            verts[0].u += 0.001f;
        gfx_set_targets(rt, 0, 0, ds);
        gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
        grass_note(&d, &src);
        grass_scene(rt, ds, &s);
        gfx_tex_read(rt, 0, 0, px, TW * 4);
        long n = 0;
        for (int i = 0; i < TW * TH; ++i)
            n += px[i] != BACKGROUND;
        if (pass)
            printf("  grass: after its buffer changed, %ld pixels\n", n);
        CHECK(n > 400, "grass %s the ground's buffer changed: %ld pixels", pass ? "after" : "before", n);
        if (pass)
            verts[0].u -= 0.001f;
    }
    /* grass.walkable: tufts only where the zone's collision (grass_set_floors) has a floor of walkable ground at their place. Mesh
     * 0 is a floor under all the ground (x and z -8 to 8, y 0), mesh 1 its x 0 to 8 half (the brown); matrix 1 mirrors x, which
     * lays mesh 1 under the green half */
    {
        static const float ftris[4 * 9] = { -8, 0, -8, 8, 0, -8, -8, 0, 8, 8, 0, -8, 8, 0, 8, -8, 0, 8, 0, 0, -8, 8, 0, -8, 0, 0, 8, 8, 0, -8,
            8, 0, 8, 0, 0, 8 };
        static const unsigned fmesh[3] = { 0, 2, 4 };
        unsigned char terrain[4];
        float mat[2][12] = { { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 }, { -1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 } };
        unsigned pairs[2] = { 0, 0 };
        GrassFloors f = { 101, ftris, terrain, 4, fmesh, 2, &mat[0][0], 2, pairs, 1 };
        long n[10];
        gs.walkable = 0;
        grass_configure(&gs);
        grass_set_zone(100);
        n[0] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        gs.walkable = 1;
        grass_configure(&gs);
        memset(terrain, 2, sizeof terrain); /* grass */
        grass_set_floors(&f); /* another zone's: the tufts wait for this one's */
        n[1] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        f.zone = 100; /* this zone's: grass ground under all of it */
        grass_set_floors(&f);
        n[2] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        memset(terrain, 7, sizeof terrain); /* wood: a cart's, a crate's top */
        grass_set_floors(&f);
        n[3] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        memset(terrain, 2, sizeof terrain), mat[0][10] = 2.0f; /* grass ground 2 below it (y is down): a tree's boughs, a roof */
        grass_set_floors(&f);
        n[4] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        mat[0][10] = 0.0f, pairs[0] = pairs[1] = 1, memset(terrain, 7, sizeof terrain); /* wood under the green half, mirrored there */
        grass_set_floors(&f);
        n[5] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        pairs[0] = pairs[1] = 0, mat[0][9] = 1000.0f, memset(terrain, 2, sizeof terrain); /* no floor near: ground out of reach */
        grass_set_floors(&f);
        n[6] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        mat[0][9] = 0.0f, memset(terrain, 8, sizeof terrain); /* shallow water under all of it: Dragon's Aery's meadow (GRASS_WET) */
        gs.nowater = 0; /* grass.nowater off: it grows there as on grass */
        grass_configure(&gs);
        grass_set_floors(&f);
        n[9] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        gs.nowater = 1; /* on (the default): none */
        grass_configure(&gs);
        n[7] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        gs.walkable = 0; /* ... none on it with walkable off too */
        grass_configure(&gs);
        n[8] = grass_pixels(rt, ds, &d, &src, &s, vpd);
        gs.nowater = 0;
        printf("  grass: walkable off %ld pixels; on: waiting %ld, on grass %ld, on wood %ld, above grass %ld, on mirrored wood %ld, no floor %ld, "
               "in shallow water %ld (walkable off %ld; nowater off %ld)\n", n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9]);
        CHECK(n[0] > 400, "walkable off: %ld pixels", n[0]);
        CHECK(n[1] == 0, "the tufts didn't wait for the zone's floors: %ld pixels", n[1]);
        CHECK(n[2] * 20 >= n[0] * 19, "on a grass floor: %ld pixels against %ld", n[2], n[0]);
        CHECK(n[3] == 0, "on a wood floor: %ld pixels", n[3]);
        CHECK(n[4] == 0, "above a grass floor: %ld pixels", n[4]);
        CHECK(n[5] == 0, "on a mirrored wood floor: %ld pixels", n[5]);
        CHECK(n[6] * 20 >= n[0] * 19, "with no floor near: %ld pixels against %ld", n[6], n[0]);
        CHECK(n[7] == 0 && n[8] == 0, "in shallow water: %ld pixels (walkable off %ld)", n[7], n[8]);
        CHECK(n[9] * 20 >= n[0] * 19, "in shallow water with grass.nowater off: %ld pixels against %ld", n[9], n[0]);
        gs.walkable = 0;
        grass_configure(&gs);
        grass_set_floors(NULL);
    }
    freecam_test(rt, ds, &d, &src, &s, vpd);
    cover_test(rt, ds, &d, &src, &s, vpd, &gs), dirt_test(rt, ds, &d, &src, &s, vpd, &gs), moss_test(rt, ds, &d, &src, &s, vpd, &gs);
    /* dry grass ground (East Sarutabaruta's colours) on a floor the game's footsteps call stone throughout grows nothing, as the
     * Nyzul portal's gold disc in Alzadaal (dry grass by its colours alone) grew a meadow; on grass it grows; half grass half stone,
     * the stone half alone is bare (spot by spot, not a square at a time); grass.overgrown grows it on the stone as well. The floor
     * is mesh 0 placed once under all the ground, its triangle 0 the half where x + z < 0 */
    {
        static uint32_t dryt[64 * 64];
        for (int i = 0; i < 64 * 64; ++i)
            dryt[i] = 0xFF786F49u;
        const uint64_t hd = 0xD47A0000000000F0ull;
        static const float ftris[2 * 9] = { -8, 0, -8, 8, 0, -8, -8, 0, 8, 8, 0, -8, 8, 0, 8, -8, 0, 8 };
        static const unsigned fmesh[2] = { 0, 2 };
        static const float fmat[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
        static const unsigned fpairs[2] = { 0, 0 };
        unsigned char terrain[2];
        const GrassFloors f = { 100, ftris, terrain, 2, fmesh, 1, fmat, 1, fpairs, 1 };
        GrassSrc sd = src;
        sd.tex_hash = hd;
        long n[5];
        gs.dry = 1;
        grass_configure(&gs);
        grass_set_zone(100);
        grass_texture(hd, 21, 64, 64, dryt, 64 * 4);
        n[0] = grass_pixels(rt, ds, &d, &sd, &s, vpd); /* no floors known: as before */
        gs.walkable = 1;
        grass_configure(&gs);
        terrain[0] = terrain[1] = 5; /* stone */
        grass_set_floors(&f);
        n[1] = grass_pixels(rt, ds, &d, &sd, &s, vpd);
        terrain[0] = terrain[1] = 2; /* grass */
        grass_set_floors(&f);
        n[2] = grass_pixels(rt, ds, &d, &sd, &s, vpd);
        terrain[0] = 2, terrain[1] = 5; /* grass, then stone */
        grass_set_floors(&f);
        n[3] = grass_pixels(rt, ds, &d, &sd, &s, vpd);
        terrain[0] = terrain[1] = 5;
        grass_set_floors(&f);
        gs.overgrown = 1;
        grass_configure(&gs);
        n[4] = grass_pixels(rt, ds, &d, &sd, &s, vpd);
        printf("  grass: dry grass ground %ld pixels; on stone %ld, on grass %ld, half grass half stone %ld, on stone overgrown %ld\n", n[0],
            n[1], n[2], n[3], n[4]);
        CHECK(n[0] > 400, "dry grass ground: %ld pixels", n[0]);
        CHECK(n[1] == 0, "dry grass ground on stone: %ld pixels", n[1]);
        CHECK(n[2] * 20 >= n[0] * 19, "dry grass ground on grass: %ld pixels against %ld", n[2], n[0]);
        /* the stone half bare: a fifth to four fifths of the picture's tufts, as the halves lie nearer or farther */
        CHECK(n[3] * 5 > n[0] && n[3] * 5 < n[0] * 4, "dry grass ground half on grass: %ld pixels against %ld", n[3], n[0]);
        CHECK(n[4] * 20 >= n[0] * 19, "dry grass ground on stone, overgrown: %ld pixels against %ld", n[4], n[0]);
        gs.overgrown = 0, gs.walkable = 0, gs.dry = 0;
        grass_configure(&gs);
        grass_set_floors(NULL);
    }
    /* flowers on: blooms that aren't green appear */
    gs.flowers = 1.0f;
    grass_configure(&gs);
    grass_set_zone(100);
    gfx_set_targets(rt, 0, 0, ds);
    gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
    grass_note(&d, &src);
    grass_scene(rt, ds, &s);
    gfx_tex_read(rt, 0, 0, px, TW * 4);
    if (getenv("GRASS_TEST_BMP"))
    {
        char path[512];
        snprintf(path, sizeof path, "%s.flowers", getenv("GRASS_TEST_BMP"));
        FILE* f = fopen(path, "wb");
        if (f)
        {
            const uint32_t wh[2] = { TW, TH };
            fwrite(wh, 4, 2, f), fwrite(px, 4, TW * TH, f), fclose(f);
        }
    }
    {
        long tuft = 0, bloom = 0;
        for (int i = 0; i < TW * TH; ++i)
        {
            const uint32_t c = px[i];
            if (c == BACKGROUND)
                continue;
            ++tuft;
            bloom += ((c >> 16) & 255) > ((c >> 8) & 255) + 12 || ((c & 255) > ((c >> 8) & 255) + 12);
        }
        printf("  grass: with flowers on, %ld tuft pixels, %ld of them not green\n", tuft, bloom);
        CHECK(tuft > 400, "grass with flowers on: %ld pixels", tuft);
        CHECK(bloom > 15, "no flowers: %ld non-green pixels", bloom);
    }
    /* a limit of 5 degrees on flat ground (here, with its margin, it is nearly the limit): almost bare */
    gs.flowers = 0.0f, gs.slope = 5.0f;
    grass_configure(&gs);
    grass_set_zone(100);
    gfx_set_targets(rt, 0, 0, ds);
    gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
    grass_note(&d, &src);
    grass_scene(rt, ds, &s);
    gfx_tex_read(rt, 0, 0, px, TW * 4);
    {
        long bare = 0;
        for (int i = 0; i < TW * TH; ++i)
            bare += px[i] != BACKGROUND;
        printf("  grass: with a 5 degree limit, %ld pixels\n", bare);
        CHECK(bare < 2500, "steep limit left %ld pixels", bare);
    }
    gs.slope = 40.0f;
    /* the rings set: all three keeping 100% grows at least as much as the default rings; ring 1 from the player keeping none, nothing */
    {
        long counts[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; ++k)
        {
            gs.rings_set = k > 0;
            for (int i = 0; i < 3; ++i)
                gs.ring[i] = k == 2 ? 0.0f : 0.25f * (float)(i + 1), gs.ring_amount[i] = k == 2 ? 0.0f : 1.0f;
            grass_configure(&gs);
            grass_set_zone(100);
            gfx_set_targets(rt, 0, 0, ds);
            gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
            grass_note(&d, &src);
            grass_scene(rt, ds, &s);
            gfx_tex_read(rt, 0, 0, px, TW * 4);
            for (int i = 0; i < TW * TH; ++i)
                counts[k] += px[i] != BACKGROUND;
        }
        printf("  grass: rings default %ld pixels, all keeping 100%% %ld, keeping none %ld\n", counts[0], counts[1], counts[2]);
        CHECK(counts[1] >= counts[0] && counts[0] > 400, "rings keeping all: %ld pixels against %ld with the default rings", counts[1], counts[0]);
        CHECK(counts[2] == 0, "rings keeping none: %ld pixels", counts[2]);
        gs.rings_set = 0;
    }
    gs.flowers = 1.0f;
    /* flowers only: no tufts, so far fewer pixels than with them, but some */
    gs.flowers_only = 1;
    grass_configure(&gs);
    grass_set_zone(100);
    gfx_set_targets(rt, 0, 0, ds);
    gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
    grass_note(&d, &src);
    grass_scene(rt, ds, &s);
    gfx_tex_read(rt, 0, 0, px, TW * 4);
    if (getenv("GRASS_TEST_BMP"))
    {
        char path[512];
        snprintf(path, sizeof path, "%s.flowers_only", getenv("GRASS_TEST_BMP"));
        FILE* f = fopen(path, "wb");
        if (f)
        {
            const uint32_t wh[2] = { TW, TH };
            fwrite(wh, 4, 2, f), fwrite(px, 4, TW * TH, f), fclose(f);
        }
    }
    {
        long only = 0;
        for (int i = 0; i < TW * TH; ++i)
            only += px[i] != BACKGROUND;
        printf("  grass: flowers only, %ld pixels\n", only);
        CHECK(only > 20 && only < 6000, "flowers only: %ld pixels", only);
    }
    /* the catalogue: each type of the drawn blades (assets 0 and 1) grows alone, in the ground's green */
    gs.flowers = 0.0f, gs.flowers_only = 0;
    grass_configure(&gs);
    grass_set_zone(100);
    for (int type = 0; type < 2; ++type)
    {
        grass_asset_weight(0, type ? 0.0f : 1.0f);
        grass_asset_weight(1, type ? 1.0f : 0.0f);
        gfx_set_targets(rt, 0, 0, ds);
        gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
        grass_note(&d, &src);
        grass_scene(rt, ds, &s);
        gfx_tex_read(rt, 0, 0, px, TW * 4);
        long n = 0, greeny = 0;
        for (int i = 0; i < TW * TH; ++i)
        {
            if (px[i] == BACKGROUND)
                continue;
            ++n;
            greeny += ((px[i] >> 8) & 255) > ((px[i] >> 16) & 255) + 10;
        }
        printf("  grass: drawn blades type %d alone, %ld pixels (%ld green)\n", type + 1, n, greeny);
        CHECK(n > 400, "the drawn blades of type %d: %ld pixels", type + 1, n);
        CHECK(greeny * 5 > n * 4, "the drawn blades of type %d aren't green: %ld of %ld", type + 1, greeny, n);
    }
    /* grass.match: at 1 the drawn blades (type 2, still alone) are the ground's colour, lit as the game lights the ground (a lit draw:
     * the ambient 0.5 and a light from straight above at 0.75, on the vertex colour 0x80 doubled): their mean green is the texel's
     * (140) times 1.25. At 0 they're their own shade, darker. */
    {
        const GfxDraw unlit = d;
        d.vs.lighting = 1, d.vs.src_diffuse = 1, d.vs.src_ambient = 1, d.vs.nlights = 1, d.vs.light_type[0] = 3;
        memcpy(d.u.wv, s.view, 64);
        for (int i = 0; i < 3; ++i)
            d.u.ambient[i] = 0.5f, d.u.light[0].diffuse[i] = 0.75f, d.u.light[0].dir[i] = -s.view[4 + i];
        double green[2] = { 0, 0 };
        for (int pass = 0; pass < 2; ++pass)
        {
            gs.match = pass ? 1.0f : 0.0f;
            grass_configure(&gs);
            gfx_set_targets(rt, 0, 0, ds);
            gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
            grass_note(&d, &src);
            grass_scene(rt, ds, &s);
            gfx_tex_read(rt, 0, 0, px, TW * 4);
            long n = 0;
            for (int i = 0; i < TW * TH; ++i)
                if (px[i] != BACKGROUND)
                    green[pass] += (px[i] >> 8) & 255, ++n;
            green[pass] = n ? green[pass] / n : 0.0;
        }
        printf("  grass: drawn blades' mean green %.0f with grass.match 0, %.0f with 1 (the lit ground's %.0f)\n", green[0], green[1], 140 * 1.25);
        CHECK(fabs(green[1] - 140 * 1.25) < 140 * 1.25 * 0.15, "grass.match 1: the blades' mean green %.0f, the lit ground's %.0f", green[1], 140 * 1.25);
        CHECK(green[0] < green[1] * 0.8, "grass.match 0 (%.0f) isn't darker than 1 (%.0f)", green[0], green[1]);
        gs.match = 0.0f;
        grass_configure(&gs);
        d = unlit;
    }
    /* then an asset (a solid orange picture with one sprite) is the only one grown; the tufts are its colour, not the ground's green.
     * Then the mix: the blades (type 2) at 1 and the picture at 3 grow both, the picture's tufts about three times as many */
    {
        static uint32_t orange[64 * 64];
        for (int i = 0; i < 64 * 64; ++i)
            orange[i] = 0xFFFF8000u;
        const float whole[4] = { 0, 0, 64, 64 };
        grass_asset(2, orange, 64, 64, whole, 1, 1.0f);
        for (int mix = 0; mix < 2; ++mix)
        {
            grass_asset_weight(1, mix ? 1.0f : 0.0f);
            grass_asset_weight(2, mix ? 3.0f : 1.0f);
            gfx_set_targets(rt, 0, 0, ds);
            gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
            grass_note(&d, &src);
            grass_scene(rt, ds, &s);
            gfx_tex_read(rt, 0, 0, px, TW * 4);
            long n = 0, orangey = 0, greeny = 0;
            for (int i = 0; i < TW * TH; ++i)
            {
                if (px[i] == BACKGROUND)
                    continue;
                ++n;
                const int r = (px[i] >> 16) & 255, gr = (px[i] >> 8) & 255, b = px[i] & 255;
                orangey += r > gr + 20 && r > b + 40;
                greeny += gr > r + 10;
            }
            printf("  grass: catalogue %s, %ld pixels (%ld orange, %ld green)\n", mix ? "mix 1:3" : "picture alone", n, orangey, greeny);
            CHECK(n > 400, "the catalogue's picture: %ld pixels", n);
            if (!mix)
                CHECK(orangey * 10 > n * 9 && greeny < n / 50, "the picture alone isn't all orange: %ld of %ld orange, %ld green", orangey, n, greeny);
            else
                CHECK(orangey > greeny && greeny > n / 20, "the mix: %ld orange and %ld green of %ld", orangey, greeny, n);
        }
        grass_asset(2, NULL, 0, 0, NULL, 0, 1.0f);
        grass_asset_weight(1, 1.0f);
        grass_asset_weight(2, 0.0f);
    }

    /* one mesh laid twice (two tiles of ground, the second 16 along x), then the first moving a unit each frame: only the moving one
     * stops growing; the other tile of the same mesh keeps its tufts (one false "moves" used to take the grass off every tile) */
    {
        GfxDraw d2 = d;
        float T[16];
        memset(T, 0, sizeof T);
        T[0] = T[5] = T[10] = T[15] = 1.0f;
        T[12] = 16.0f;
        mul(d2.u.wvp, T, vp);
        long counts[4] = { 0, 0, 0, 0 };
        for (int f = 0; f < 4; ++f)
        {
            GfxDraw d1 = d;
            float M1[16];
            memset(M1, 0, sizeof M1);
            M1[0] = M1[5] = M1[10] = M1[15] = 1.0f;
            M1[12] = f >= 2 ? (float)f - 1.0f : 0.0f; /* frames 2 and 3: the first tile a unit, then two, along x */
            mul(d1.u.wvp, M1, vp);
            gfx_set_targets(rt, 0, 0, ds);
            gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
            grass_note(&d1, &src);
            grass_note(&d2, &src);
            grass_scene(rt, ds, &s);
            gfx_tex_read(rt, 0, 0, px, TW * 4);
            for (int i = 0; i < TW * TH; ++i)
                counts[f] += px[i] != BACKGROUND;
        }
        printf("  grass: two tiles of one mesh %ld %ld pixels, the first moving %ld %ld\n", counts[0], counts[1], counts[2], counts[3]);
        CHECK(counts[1] > 400 && counts[2] > 400, "two tiles: %ld, then %ld with the first moving", counts[1], counts[2]);
        CHECK(counts[3] < counts[1] && counts[3] > 200, "the moving tile kept growing or took the other with it: %ld against %ld", counts[3],
            counts[1]);
    }

    /* the camera walking (a tenth of a unit a frame, turning) past a tile far from the zone's origin (600, 600: Rolanberry's far
     * side is farther): the tufts stay every frame. The tile's place was found through the projection's inverse, a yalm off out
     * there and different each frame the camera moved, so walking took the tile for a mover and its grass went */
    {
        float T[16];
        memset(T, 0, sizeof T);
        T[0] = T[5] = T[10] = T[15] = 1.0f;
        T[12] = 600.0f, T[14] = 600.0f;
        GfxScene sf = s;
        sf.focus[0] = 600.0f, sf.focus[2] = 600.0f;
        const float up2[3] = { 0, -1, 0 };
        long first = 0, least = -1;
        for (int f = 0; f < 8; ++f)
        {
            const float yaw = 0.005f * (float)f;
            const float eye2[3] = { 600.0f + 0.1f * (float)f, -9.0f, 588.0f };
            const float at2[3] = { 600.0f + 12.0f * sinf(yaw), 0.0f, 588.0f + 12.0f * cosf(yaw) };
            look_at(sf.view, eye2, at2, up2);
            float vpf[16];
            mul(vpf, sf.view, sf.proj);
            GfxDraw df = d;
            mul(df.u.wvp, T, vpf);
            gfx_set_targets(rt, 0, 0, ds);
            gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
            grass_note(&df, &src);
            grass_scene(rt, ds, &sf);
            gfx_tex_read(rt, 0, 0, px, TW * 4);
            long n = 0;
            for (int i = 0; i < TW * TH; ++i)
                n += px[i] != BACKGROUND;
            if (!f)
                first = n;
            least = least < 0 || n < least ? n : least;
        }
        printf("  grass: walking past a tile 600 from the zone's origin, %ld pixels, at the fewest %ld\n", first, least);
        CHECK(first > 400 && least * 2 > first, "walking far from the origin: %ld pixels, then as few as %ld", first, least);
    }

    /* grass.dry on: a path's olive-grey dirt (92, 88, 70: dry grass's hue, greyer) grows nothing, beside green in one texture or in a
     * texture of its own, while East Sarutabaruta's dry grass (120, 111, 73) still grows. Dry grass's yellow-olive took in the dirt
     * of paths through green fields as well, and tufts in the dirt's colour stood all over them */
    {
        static uint32_t mixed[64 * 64], dirt[64 * 64], dry[64 * 64];
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                mixed[y * 64 + x] = x < 32 ? 0xFF3C8C32u : 0xFF5C5846u;
                dirt[y * 64 + x] = 0xFF5C5846u;
                dry[y * 64 + x] = 0xFF786F49u;
            }
        const uint32_t* texs[3] = { mixed, dirt, dry };
        static const char* names[3] = { "green beside a path's dirt", "a path's dirt alone", "dry grass alone" };
        gs.dry = 1;
        grass_configure(&gs);
        long on[3][2];
        for (int k = 0; k < 3; ++k)
        {
            const uint64_t hk = 0xD47A000000000001ull + (uint64_t)k;
            grass_texture(hk, 21, 64, 64, texs[k], 64 * 4);
            GrassSrc sk = src;
            sk.tex_hash = hk;
            gfx_set_targets(rt, 0, 0, ds);
            gfx_clear(0, NULL, 3, BACKGROUND, 1.0f, 0, vpd);
            grass_note(&d, &sk);
            grass_scene(rt, ds, &s);
            gfx_tex_read(rt, 0, 0, px, TW * 4);
            on[k][0] = on[k][1] = 0;
            for (int y = 0; y < TH; ++y)
                for (int x = 0; x < TW; ++x)
                    if (px[y * TW + x] != BACKGROUND)
                        ++on[k][((float)x < mid) == green_is_left ? 0 : 1];
            printf("  grass: dry grass on, %s: %ld tuft pixels on the left half of the texture, %ld on the right\n", names[k], on[k][0], on[k][1]);
        }
        CHECK(on[0][0] > 400 && on[0][1] * 20 < on[0][0], "dry grass on, green beside a path's dirt: %ld pixels on the green, %ld on the dirt",
            on[0][0], on[0][1]);
        CHECK(on[1][0] + on[1][1] == 0, "dry grass on, a path's dirt alone: %ld pixels", on[1][0] + on[1][1]);
        CHECK(on[2][0] > 400 && on[2][1] > 400, "dry grass on, dry grass alone: %ld and %ld pixels", on[2][0], on[2][1]);
        gs.dry = 0;
        grass_configure(&gs);
    }

    shade_test(rt, ds, &d, &src, &s, vpd, &gs);
    slope_test(rt, ds, &d, &src, vpd);

    /* capture: a cut-out sheet (two sprites on transparent black) drawn near the player is kept, with its two sprites found; a
     * solid one (a wall's texture drawn alpha-tested, gear with black in it) is not */
    {
        static uint32_t sheet[64 * 64], solid[64 * 64];
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
            {
                sheet[y * 64 + x] = (x >= 4 && x < 24 && y >= 8 && y < 60) || (x >= 40 && x < 60 && y >= 20 && y < 60) ? 0xFF40A030u : 0x00000000u;
                solid[y * 64 + x] = x < 8 ? 0xFF000000u : 0xFF806040u;
            }
        const uint64_t sheet_hash = 0x5EE7CAFE00112233ull, solid_hash = 0x5011D00000112233ull;
        grass_capture_start();
        CHECK(grass_capturing() && grass_capture_count() == -1, "capture: not running");
        for (int f = 0; f < 50; ++f)
        {
            gfx_set_targets(rt, 0, 0, ds);
            grass_note(&d, &src);
            grass_capture_note(&d, sheet_hash, 21, 64, 64, sheet, 64 * 4);
            grass_capture_note(&d, solid_hash, 21, 64, 64, solid, 64 * 4);
            grass_scene(rt, ds, &s);
        }
        const int count = grass_capture_count();
        uint64_t hv = 0;
        unsigned w = 0, h = 0, n = 0;
        const uint32_t* cpx = NULL;
        float rects[32 * 4];
        const int got = count > 0 && grass_capture_result(0, &hv, &w, &h, &cpx, rects, &n);
        printf("  grass: capture kept %d sheet(s), the first %ux%u with %u sprite(s)\n", count, w, h, n);
        CHECK(count == 1 && got && hv == sheet_hash && w == 64 && h == 64 && cpx && cpx[10 * 64 + 10] == 0xFF40A030u, "capture: %d sheets", count);
        CHECK(n == 2, "capture: %u sprites found (want 2)", n);
        if (n >= 1)
            CHECK(rects[0] <= 4 && rects[2] >= 24 && rects[1] <= 8 && rects[3] >= 60, "capture: the larger sprite's box %g %g %g %g", rects[0], rects[1], rects[2],
                rects[3]);
        grass_capture_clear();
        CHECK(grass_capture_count() == 0, "capture: not cleared");
    }
    grass_set_zone(100);
    gs.enabled = 0;
    grass_configure(&gs);
    gfx_tex_destroy(rt);
    gfx_tex_destroy(ds);
    printf(g_fails ? "grass_test: %d failed\n" : "grass_test: ok\n", g_fails);
    return g_fails != 0;
}
