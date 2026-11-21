/* VanaCore's sky ([graphics] sky): drawn in place of the game's dome. True Sky's first phase (research/true-sky.md).
 *
 * What the game draws (phase 0's 21 captures, 12 zones): its dome is the first draws of the frame's world, 7 to 11 triangle
 * strips round the camera (view and world translation 0) from the horizon up, about 52,000 units out. Each strip is a band
 * between two rings of vertices, 10 steps round; the colour is the vertices' alone (stage 0 selects the diffuse colour: the
 * texture bound, shared with hundreds of other draws, isn't read), opaque, depth tested but not written, unfogged. The hour and
 * the weather change only those colours. Every zone checked draws it, Palborough Mines under its roof too.
 *
 * Here: each of those strips is recognised by how it's drawn, never by its texture (is_dome); its vertices go into rings by
 * height (the sine of their elevation; the game's y points down) and each ring's colours by bearing; and the draw is skipped. At
 * the first draw after them (the world's own), the sky is drawn from the rings into the world's target before it (gfx_sky): one
 * pass over the viewport, nothing below the lowest ring or over the highest, as the dome drew nothing there. The world covers it
 * as it covered the dome. A frame the back end can't draw it (Direct3D 11, its shader still building) keeps the game's dome. */
#include "sky.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "runtime.h"

#define SKY_TAU 6.28318531f
#define SKY_SAMPLES 48     /* a ring's vertices kept, by bearing (the dome's have 10) */
#define SKY_RING_GAP 0.002f /* vertices this close in height (the sine of their elevation) are one ring's */
#define SKY_FAR 1000.0f    /* the dome's vertices are farther than this (about 52,000): not an effect's round the camera */

typedef struct SkyRing
{
    float s;                    /* its height: the sine of its elevation */
    uint32_t n;                 /* its vertices kept */
    float az[SKY_SAMPLES];      /* each one's bearing, 0 to 2 pi from +z toward +x */
    float rgba[SKY_SAMPLES][4]; /* ... and colour */
} SkyRing;

static struct
{
    SkySettings set;
    uint32_t taken; /* the dome's strips taken (and skipped) this frame */
    int over;       /* this frame's dome is over: a draw that wasn't one came, or the sky couldn't be drawn */
    SkyRing ring[GFX_SKY_RINGS];
    uint32_t nring;
    GfxSky sky;
    /* sky.log: what was said last, so it's said again only as it changes: 1 drawn (with said_strips, said_rings), 2 the game's
     * dome left (the back end can't), 3 the sky not drawn after the dome was taken */
    int said;
    uint32_t said_strips, said_rings;
} g;

void sky_configure(const SkySettings* in)
{
    g.set = *in;
    if (!g.set.enabled)
        g.taken = 0, g.over = 1, g.said = 0;
}

/* a stream's vertex i (of the draw's, from its first) as the CPU has it: elem's bytes, size of them, or NULL */
static const uint8_t* vertex_elem(const GfxDraw* d, int reg, uint32_t i, uint32_t nverts, uint32_t size)
{
    const GfxElem* e = &d->vs.el[reg];
    if (!e->used || e->stream >= GFX_NSTREAMS || !d->cpu[e->stream] || i >= nverts)
        return NULL;
    const uint32_t stride = (uint32_t)d->u.stride[e->stream], at = (uint32_t)d->u.offset[reg];
    if ((uint64_t)(nverts - 1) * stride + at + size > d->cpu_size[e->stream])
        return NULL;
    return (const uint8_t*)d->cpu[e->stream] + (size_t)i * stride + at;
}

/* A strip of the game's dome (the header says how one is drawn) */
static int is_dome(const GfxDraw* d, uint32_t prim, uint32_t nverts, int round_camera)
{
    if (!round_camera || prim != GFX_TRIANGLESTRIP || nverts < 4 || d->vs.rhw || d->vs.prog || d->fs.prog || d->vs.lighting)
        return 0;
    if (d->pipe.blend || !d->depth.zenable || d->depth.zwrite || d->fs.fog || d->vs.fog_vertex)
        return 0;
    const GfxElem *p = &d->vs.el[GFX_R_POSITION], *c = &d->vs.el[GFX_R_DIFFUSE];
    if (p->type != GFX_FLOAT3 || c->type != GFX_D3DCOLOR || d->vs.el[GFX_R_NORMAL].used || d->vs.el[GFX_R_TEXCOORD0].used)
        return 0;
    /* its colour and alpha the vertices' alone: SELECTARG1 (2) of DIFFUSE (0) */
    const GfxStage* st = &d->fs.st[0];
    if (st->cop != 2 || st->ca1 != 0 || st->aop != 2 || st->aa1 != 0)
        return 0;
    const uint8_t* v0 = vertex_elem(d, GFX_R_POSITION, 0, nverts, 12);
    if (!v0 || !vertex_elem(d, GFX_R_DIFFUSE, 0, nverts, 4))
        return 0;
    float q[3];
    memcpy(q, v0, sizeof q);
    return q[0] * q[0] + q[1] * q[1] + q[2] * q[2] > SKY_FAR * SKY_FAR;
}

/* the same bearing: within a hundredth of a radian, round the circle */
static int same_bearing(float a, float b)
{
    const float d = fabsf(a - b);
    return d < 0.01f || d > SKY_TAU - 0.01f;
}

/* a vertex of the dome into the ring at its height (a new one, or the nearest when there are GFX_SKY_RINGS already) */
static void ring_add(float s, float az, const float rgba[4])
{
    uint32_t r = 0;
    while (r < g.nring && fabsf(g.ring[r].s - s) > SKY_RING_GAP)
        ++r;
    if (r == g.nring && g.nring == GFX_SKY_RINGS)
    {
        float best = 2.0f;
        for (uint32_t k = 0; k < g.nring; ++k)
            if (fabsf(g.ring[k].s - s) < best)
                best = fabsf(g.ring[k].s - s), r = k;
    }
    else if (r == g.nring)
        g.ring[r].s = s, g.ring[r].n = 0, g.nring++;
    SkyRing* R = &g.ring[r];
    for (uint32_t k = 0; k < R->n; ++k)
        if (same_bearing(R->az[k], az)) /* the same vertex again: the band under it and the band over it share it */
            return;
    if (R->n == SKY_SAMPLES)
        return;
    R->az[R->n] = az;
    memcpy(R->rgba[R->n], rgba, sizeof R->rgba[0]);
    R->n++;
}

static void take_strip(const GfxDraw* d, uint32_t nverts)
{
    for (uint32_t i = 0; i < nverts; ++i)
    {
        const uint8_t *pv = vertex_elem(d, GFX_R_POSITION, i, nverts, 12), *cv = vertex_elem(d, GFX_R_DIFFUSE, i, nverts, 4);
        if (!pv || !cv)
            return;
        float p[3];
        uint32_t c;
        memcpy(p, pv, sizeof p);
        memcpy(&c, cv, sizeof c);
        const float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (!(len > 0.0f))
            continue;
        float az = atan2f(p[0], p[2]);
        if (az < 0.0f)
            az += SKY_TAU;
        const float rgba[4] = { (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f,
            (float)(c >> 24) / 255.0f };
        ring_add(-p[1] / len, az, rgba);
    }
}

/* m inverted (4x4), into out; 0 when it has none */
static int invert4(const float* m, float* out)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c)
            a[r][c] = c < 4 ? (double)m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (fabs(a[r][c]) > fabs(a[pivot][c]))
                pivot = r;
        if (fabs(a[pivot][c]) < 1e-12)
            return 0;
        if (pivot != c)
            for (int k = 0; k < 8; ++k)
            {
                const double t = a[c][k];
                a[c][k] = a[pivot][k], a[pivot][k] = t;
            }
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k)
            a[c][k] *= inv;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0)
            {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k)
                    a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = (float)a[r][c + 4];
    return 1;
}

/* the first strip: the dome's camera (its world-view-projection, inverted), viewport and DLSS's step */
static int first_strip(const GfxDraw* d)
{
    if (!invert4(d->u.wvp, g.sky.inv))
        return 0;
    memcpy(g.sky.vp, d->vp, sizeof g.sky.vp);
    float jit[2] = { 0.0f, 0.0f };
    if (!gfx_dlss_jitter(jit))
        jit[0] = jit[1] = 0.0f;
    g.sky.jitter[0] = jit[0], g.sky.jitter[1] = jit[1];
    g.nring = 0;
    return 1;
}

/* a ring's colour at each of the sky's bearings: between the two vertices either side of it, round the circle */
static void ring_bearings(const SkyRing* R, float out[GFX_SKY_BEARINGS][4])
{
    uint32_t o[SKY_SAMPLES];
    for (uint32_t i = 0; i < R->n; ++i) /* by bearing */
    {
        uint32_t k = i;
        while (k > 0 && R->az[o[k - 1]] > R->az[i])
            o[k] = o[k - 1], --k;
        o[k] = i;
    }
    for (uint32_t b = 0; b < GFX_SKY_BEARINGS; ++b)
    {
        if (R->n == 1)
        {
            memcpy(out[b], R->rgba[0], sizeof out[b]);
            continue;
        }
        const float at = (float)b * SKY_TAU / (float)GFX_SKY_BEARINGS;
        uint32_t hi = 0;
        while (hi < R->n && R->az[o[hi]] < at)
            ++hi;
        const uint32_t i0 = o[(hi + R->n - 1) % R->n], i1 = o[hi % R->n];
        float span = R->az[i1] - R->az[i0], off = at - R->az[i0];
        if (span <= 0.0f)
            span += SKY_TAU;
        if (off < 0.0f)
            off += SKY_TAU;
        const float t = span > 1e-6f ? fminf(off / span, 1.0f) : 0.0f;
        for (int c = 0; c < 4; ++c)
            out[b][c] = R->rgba[i0][c] + (R->rgba[i1][c] - R->rgba[i0][c]) * t;
    }
}

/* the rings, lowest first, into the sky */
static void sky_build(void)
{
    for (uint32_t i = 1; i < g.nring; ++i)
        for (uint32_t k = i; k > 0 && g.ring[k - 1].s > g.ring[k].s; --k)
        {
            const SkyRing t = g.ring[k];
            g.ring[k] = g.ring[k - 1], g.ring[k - 1] = t;
        }
    g.sky.nrings = g.nring;
    for (uint32_t r = 0; r < g.nring; ++r)
    {
        g.sky.ring[r] = g.ring[r].s;
        ring_bearings(&g.ring[r], g.sky.color[r]);
    }
}

/* sky.log: a line as what the sky does changes */
static void say(int what)
{
    if (!g.set.log || (what == g.said && (what != 1 || (g.said_strips == g.taken && g.said_rings == g.nring))))
        return;
    g.said = what, g.said_strips = g.taken, g.said_rings = g.nring;
    if (what == 1)
        rt_log("[recomp] sky: VanaCore's sky drawn in place of the game's dome (%u strips, %u rings, %.0f to %.0f degrees up)\n",
            g.taken, g.nring, asinf(g.sky.ring[0]) * 57.2958f, asinf(g.sky.ring[g.nring - 1]) * 57.2958f);
    else if (what == 2)
        rt_log("[recomp] sky: the game's dome is drawn: VanaCore's sky can't be (Direct3D 11, or its shader still building)\n");
    else
        rt_log("[recomp] sky: the dome's strips were taken but VanaCore's sky wasn't drawn (%u rings)\n", g.nring);
}

int sky_scene_draw(const GfxDraw* d, uint32_t prim, uint32_t nverts, uint32_t number, int round_camera)
{
    if (number == 1)
        g.taken = 0, g.over = !g.set.enabled;
    if (g.over || number != g.taken + 1)
        return 0;
    if (!is_dome(d, prim, nverts, round_camera))
    {
        g.over = 1;
        return g.taken ? 2 : 0;
    }
    if (!g.taken && (!gfx_sky(NULL) || !first_strip(d)))
    {
        g.over = 1;
        say(2);
        return 0;
    }
    take_strip(d, nverts);
    g.taken++;
    return 1;
}

void sky_flush(void)
{
    if (!g.taken)
        return;
    int drawn = 0;
    if (g.nring >= 2)
    {
        sky_build();
        drawn = gfx_sky(&g.sky);
    }
    say(drawn ? 1 : 3);
    g.taken = 0;
}
