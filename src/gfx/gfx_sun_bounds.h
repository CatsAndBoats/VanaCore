/* A caster's bounds for the sun's maps: the box round its vertices, in the world. A caster stood where its first vertex is
 * (caster_at) - one corner of it - and beyond_map culled it by that alone: a zone piece a hundred units long whose corner was
 * past the line went whole while the rest of it shaded the plaza, and as the map moved with the camera its shadow came and
 * went (Port Jeuno, 2026-11-01). Its box can't do that; and as its shadow falls along the light (the map's z), where the map's
 * x and y don't change, a box whose x and y miss the map shades nothing in it: the reach past the map's sides is the
 * filter's alone (SBOX_REACH), not a guess at the zone's biggest piece (SUN_REACH).
 *
 * gfx_d3d12.c and gfx_d3d11.c include this; it is no file of its own:
 *   - as a caster is recorded (where gfx_draw_clip0 runs): sbox_model, its box in model space (scanned once for each vertex
 *     range, then looked up), and sbox_basis, this frame's map from model space to the camera's clip space;
 *   - in sun_cache_update, once the frame's camera is known: sbox_world, the box in the world, kept by the entry beside where
 *     it stands (sbox_rel_set), so a copy found again a little way off (cache_moved, drift) takes it along (sbox_rel_at);
 *   - in beyond_map and cache_unseen: sbox_beyond and sbox_beyond_xy in place of the one point;
 *   - sbox_clear with the cache's own (the camera jumped: a new zone). */
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_hlsl.h" /* gfx_draw_clip_pos */

/* How far past a map's sides a box can still reach into it, in world units: the filter's widest reach round a point and the
 * point's offset off its surface, with room to spare */
#define SBOX_REACH 4.0f

/* more vertex ranges than this known: all let go (a long stay in one zone, its buffers made again and again) */
#define SBOX_MOST (1u << 18)

/* --- the box in model space, once for each vertex range ------------------------------------------------------------ */
typedef struct SboxModel
{
    const void* p; /* the position stream's vertices as the CPU has them (GfxDraw.cpu); NULL: a free slot */
    uint32_t bytes, stride, off, version;
    float lo[3], hi[3]; /* lo[0] > hi[0]: none of its vertices could be read */
} SboxModel;

static SboxModel* g_sbox;
static uint32_t g_sbox_cap, g_sbox_n;
static uint32_t g_sbox_scans; /* vertex ranges scanned since the start (each once until sbox_clear) */

static void sbox_clear(void)
{
    free(g_sbox);
    g_sbox = NULL, g_sbox_cap = g_sbox_n = 0;
}

static uint32_t sbox_hash(const SboxModel* k)
{
    const uint64_t p = (uint64_t)(uintptr_t)k->p;
    const uint32_t w[5] = { (uint32_t)p ^ (uint32_t)(p >> 32), k->bytes, k->stride, k->off, k->version };
    uint32_t h = 2166136261u;
    for (int i = 0; i < 5; ++i)
        h = (h ^ w[i]) * 16777619u;
    return h ^ (h >> 15);
}

static SboxModel* sbox_slot(SboxModel* t, uint32_t cap, const SboxModel* k)
{
    for (uint32_t i = sbox_hash(k) & (cap - 1);; i = (i + 1) & (cap - 1))
    {
        SboxModel* e = &t[i];
        if (!e->p || (e->p == k->p && e->bytes == k->bytes && e->stride == k->stride && e->off == k->off && e->version == k->version))
            return e;
    }
}

static int sbox_grow(void)
{
    const uint32_t cap = g_sbox_cap ? g_sbox_cap * 2 : 4096;
    SboxModel* t = (SboxModel*)calloc(cap, sizeof *t);
    if (!t)
        return 0;
    for (uint32_t i = 0; i < g_sbox_cap; ++i)
        if (g_sbox[i].p)
            *sbox_slot(t, cap, &g_sbox[i]) = g_sbox[i];
    free(g_sbox);
    g_sbox = t, g_sbox_cap = cap;
    return 1;
}

/* the draw's position stream (float positions alone): its vertices, their stride and the position's offset in one; 0 none */
static int sbox_stream(const GfxDraw* d, SboxModel* k)
{
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    if (d->vs.rhw || !e->used || e->stream >= GFX_NSTREAMS || (e->type != GFX_FLOAT3 && e->type != GFX_FLOAT4))
        return 0;
    if (d->u.stride[e->stream] <= 0 || d->u.offset[GFX_R_POSITION] < 0)
        return 0;
    memset(k, 0, sizeof *k);
    k->p = d->cpu[e->stream], k->bytes = d->cpu_size[e->stream];
    k->stride = (uint32_t)d->u.stride[e->stream], k->off = (uint32_t)d->u.offset[GFX_R_POSITION];
    return k->p && k->off <= k->bytes && k->bytes - k->off >= 12u;
}

static void sbox_scan(SboxModel* s)
{
    const uint32_t n = (s->bytes - s->off - 12u) / s->stride + 1u;
    const uint8_t* v = (const uint8_t*)s->p + s->off;
    for (int a = 0; a < 3; ++a)
        s->lo[a] = INFINITY, s->hi[a] = -INFINITY;
    for (uint32_t i = 0; i < n; ++i)
    {
        float q[3];
        memcpy(q, v + (size_t)i * s->stride, sizeof q);
        if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]))
            continue; /* (junk: a vertex the indices never reach) */
        for (int a = 0; a < 3; ++a)
            s->lo[a] = fminf(s->lo[a], q[a]), s->hi[a] = fmaxf(s->hi[a], q[a]);
    }
    g_sbox_scans++;
}

/* The box round the draw's vertices in model space: those its position stream holds for it (GfxDraw.cpu: the draw's own
 * range, its indices' lowest to highest). version tells a buffer's contents apart when the game writes it again (0 if
 * nothing does). 0 when it can't be had: blended or tweened positions (not where one matrix puts them), or none readable. */
static int sbox_model(const GfxDraw* d, uint32_t version, float lo[3], float hi[3])
{
    SboxModel k;
    if (!sbox_stream(d, &k))
        return 0;
    if (!d->vs.prog && (d->vs.el[GFX_R_BLENDWEIGHT].used || d->vs.el[GFX_R_POSITION2].used))
        return 0;
    k.version = version;
    if (g_sbox_n >= SBOX_MOST)
        sbox_clear();
    if ((g_sbox_n + 1) * 2 > g_sbox_cap && !sbox_grow())
        return 0;
    SboxModel* s = sbox_slot(g_sbox, g_sbox_cap, &k);
    if (!s->p)
        *s = k, sbox_scan(s), g_sbox_n++;
    if (!(s->lo[0] <= s->hi[0]))
        return 0;
    memcpy(lo, s->lo, sizeof s->lo), memcpy(hi, s->hi, sizeof s->hi);
    return 1;
}

/* --- the box in the world ------------------------------------------------------------------------------------------ */
/* This frame's map from the draw's model space to the camera's clip space, row vectors as gfx_draw_clip0's (clip =
 * (x, y, z, 1) * m): the fixed function's world-view-projection; a vertex shader's found by running it on the CPU over three
 * points beside its first vertex, clip0 (gfx_draw_clip0's) the fourth - a transform as long as the shader places the mesh by
 * a matrix (the zone's trees, by its constants), as grass.c takes a zone piece's. The points are the box's size apart (lo,
 * hi: sbox_model's): a clip-space depth far out holds a few hundredths of a unit, and points 16 units apart multiplied that
 * by the box's size over 16 (0.4 units off, 400 out). 0 when it can't be had. */
static int sbox_basis(const GfxDraw* d, const float clip0[4], const float lo[3], const float hi[3], float m[16])
{
    if (d->vs.rhw)
        return 0;
    if (!d->vs.prog)
    {
        memcpy(m, d->u.wvp, 16 * sizeof(float));
        return 1;
    }
    SboxModel k;
    if (!d->vs_tokens || !sbox_stream(d, &k))
        return 0;
    float p0[3], c[3][4], step[3];
    memcpy(p0, (const uint8_t*)k.p + k.off, sizeof p0);
    for (int i = 0; i < 3; ++i)
    {
        float q[3] = { p0[0], p0[1], p0[2] };
        step[i] = fmaxf(hi[i] - lo[i], 1.0f);
        q[i] += step[i];
        if (!gfx_draw_clip_pos(d, q, c[i]))
            return 0;
    }
    for (int j = 0; j < 4; ++j)
    {
        for (int i = 0; i < 3; ++i)
            m[i * 4 + j] = (c[i][j] - clip0[j]) / step[i];
        m[12 + j] = clip0[j] - p0[0] * m[j] - p0[1] * m[4 + j] - p0[2] * m[8 + j];
    }
    for (int i = 0; i < 16; ++i)
        if (!isfinite(m[i]))
            return 0;
    return 1;
}

/* The box in the world: its eight corners through m (sbox_basis) and that frame's camera back (clip_world: clip space to the
 * world, as caster_at's). 0 when a corner can't be placed. */
static int sbox_world(const float lo[3], const float hi[3], const float m[16], const float* clip_world, float wlo[3], float whi[3])
{
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i * 4 + j] = m[i * 4] * clip_world[j] + m[i * 4 + 1] * clip_world[4 + j] + m[i * 4 + 2] * clip_world[8 + j] +
                m[i * 4 + 3] * clip_world[12 + j];
    for (int a = 0; a < 3; ++a)
        wlo[a] = INFINITY, whi[a] = -INFINITY;
    for (int c = 0; c < 8; ++c)
    {
        const float p[3] = { (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2] };
        float h[4];
        for (int j = 0; j < 4; ++j)
            h[j] = p[0] * t[j] + p[1] * t[4 + j] + p[2] * t[8 + j] + t[12 + j];
        if (!(fabsf(h[3]) > 1e-20f))
            return 0;
        for (int a = 0; a < 3; ++a)
            wlo[a] = fminf(wlo[a], h[a] / h[3]), whi[a] = fmaxf(whi[a], h[a] / h[3]);
    }
    for (int a = 0; a < 3; ++a)
        if (!isfinite(wlo[a]) || !isfinite(whi[a]))
            return 0;
    return 1;
}

/* A world box kept beside where its caster stands (caster_at's point): a copy found again a little way off takes it along */
typedef struct SboxRel
{
    float lo[3], hi[3];
    uint8_t ok;
} SboxRel;

static void sbox_rel_set(SboxRel* r, const float wlo[3], const float whi[3], const float at[3])
{
    for (int a = 0; a < 3; ++a)
        r->lo[a] = wlo[a] - at[a], r->hi[a] = whi[a] - at[a];
    r->ok = 1;
}

static int sbox_rel_at(const SboxRel* r, const float at[3], float lo[3], float hi[3])
{
    if (!r->ok)
        return 0;
    for (int a = 0; a < 3; ++a)
        lo[a] = at[a] + r->lo[a], hi[a] = at[a] + r->hi[a];
    return 1;
}

/* --- can it reach a map ------------------------------------------------------------------------------------------- */
/* how far the box's nearest point is from p, squared (0 inside): sun_cache_update's reach from the camera, by the box */
static float sbox_dist2(const float lo[3], const float hi[3], const float p[3])
{
    float s = 0.0f;
    for (int a = 0; a < 3; ++a)
    {
        const float o = p[a] < lo[a] ? lo[a] - p[a] : p[a] > hi[a] ? p[a] - hi[a] : 0.0f;
        s += o * o;
    }
    return s;
}

/* along one of a map's axes (v = p . row[0..2] + row[3]): the box's lowest and highest */
static void sbox_span(const float row[4], const float lo[3], const float hi[3], float* vlo, float* vhi)
{
    float c = row[3], r = 0.0f;
    for (int a = 0; a < 3; ++a)
        c += 0.5f * (lo[a] + hi[a]) * row[a], r += 0.5f * (hi[a] - lo[a]) * fabsf(row[a]);
    *vlo = c - r, *vhi = c + r;
}

/* by a map's x and y rows alone (x's four, then y's: as cache_unseen's g_far_xy), edge its sides (1 + reach * 2 / across) */
static int sbox_beyond_xy(const float xy[8], float edge, const float lo[3], const float hi[3])
{
    float a, b;
    sbox_span(xy, lo, hi, &a, &b);
    if (a > edge || b < -edge)
        return 1;
    sbox_span(xy + 4, lo, hi, &a, &b);
    return a > edge || b < -edge;
}

/* True when the box can't reach the cascade's map (S: the world to the map, as SunCascade.S; across: its width in units):
 * past its sides by more than reach, or all of it farther from the sun than the map's far end (z past 1: behind everything
 * the map holds). Nearer the sun than its near end is left to the depth clip (with it off, a caster there is pancaked and
 * still shades). */
static int sbox_beyond(const float S[16], float across, const float lo[3], const float hi[3], float reach)
{
    const float xy[8] = { S[0], S[4], S[8], S[12], S[1], S[5], S[9], S[13] }, z[4] = { S[2], S[6], S[10], S[14] };
    if (sbox_beyond_xy(xy, 1.0f + reach * 2.0f / across, lo, hi))
        return 1;
    float zlo, zhi;
    sbox_span(z, lo, hi, &zlo, &zhi);
    return zlo > 1.0f;
}
