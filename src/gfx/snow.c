/* VanaCore's snow trails (snow.h): footprints pressed into a zone's snowfields.
 *
 * Nothing is dug: the ground is the game's static geometry. What's pressed in is shading, laid over the snow the game drew.
 *
 * Where: the zone's collision (VanaCore reads it from the zone's DAT, as for the grass's and the reflections' floors) has a
 * kind of ground on each triangle, the one the game's footsteps sound for, and "snow" is one of them (terrain 4). The collision
 * lies on the drawn ground to the hundredth, so the snow triangles, laid in the world once a zone (cut to triangles of at most
 * SN_EDGE yalms a side and sorted into SN_BUCKET squares), are drawn again over the scene with its camera: the picture under
 * them is scaled by what the trail says is there (2 x the shading x the picture: the shading's grey is no change, darker
 * shades it and lighter lights it). Depth-tested, so a character standing on the snow isn't painted over.
 *
 * What: a texture of that shading wrapped on a torus SN_SPAN yalms wide that follows the player (the vertices carry x and z over
 * the span), with its mips so far prints fade. A footprint is baked into it once, when it's pressed in: its dent and the lip of snow
 * pushed up round it as a small patch of heights (softened by a blur), shaded for the light there was then - the walls turned to
 * or from the sun, the shadow of the rim, the cavity's occlusion, a little blue in the shade - and laid into the texture by
 * multiplying. The light is taken once, when the scene first gives one, and kept: a print never changes after, and nothing is
 * shaded again for the sun moving. Only the fade (the shading eased toward none, a tile at a time) touches a print again.
 *
 * Who: the entities near the player (vanacore_gfx_set_characters) are followed from frame to frame by where they are. Each
 * presses a foot in every stride, left and right, where the collision has snow under it at their height. A foot is the size and
 * place of the character's own: the lowest vertices of its model, as the game skinned them this frame (snow_character_draw),
 * say where each foot is and how big. With sinking on, those draws are lowered into the snow. */
#include "snow.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define FMT_A8R8G8B8 21u

enum
{
    SN_N = 2048,                    /* texels a side of the trail's torus */
    SN_LEVELS = 5,                  /* its mips: 2048 down to 128 */
    SN_TILE = 64,                   /* texels a side of a tile, what is cleared and faded at a time */
    SN_TILES = SN_N / SN_TILE,      /* tiles a side */
    SN_HALF = SN_TILES / 2,
    SN_MASK = SN_N - 1,
    SN_MARGIN = 24,                 /* texels round a print's heights that its shading reads: the blur's reach (8), the shading's (8) and its taps' (8) */
    SN_PATCH = 128,                 /* texels a side of the most a print's patch is */
    SN_TRACKS = 256,
    SN_PTS = 160,                   /* the lowest vertices kept of a character, for its feet */
    SN_FLAT = 128,                  /* a patch texel with nothing pressed into it */
};

#define SN_CELL 0.03125f                 /* yalms to a texel */
#define SN_SPAN (SN_N * SN_CELL)         /* yalms the torus spans: 64 */
#define SN_TILE_YALM (SN_TILE * SN_CELL) /* 2 */
#define SN_BUCKET 8.0f                   /* yalms a side of the squares the snow's triangles are sorted into and drawn by */
#define SN_EDGE 4.0f                     /* the longest side a snow triangle keeps (so a bucket's triangles reach 4 yalms past it) */
#define SN_LIFT 0.04f                    /* yalms the overlay stands over the ground, against the depth buffer's rounding */
#define SN_MOST_TRIS 500000u             /* a zone with more snow triangles than this has none of it done */
#define SN_STRIDE 0.62f                  /* yalms between one foot's print and the other's, for a player's feet */
#define SN_GROUND 2.5f                   /* how far (yalms) over or under the snow an entity may be and press into it */
#define SN_BAND 0.10f                    /* how far up from its lowest point a model's vertices count as its feet */
#define SN_FOOT 0.30f                    /* a player's foot's length, the fallback and what a measured one is scaled against */

typedef struct SVert
{
    float x, y, z; /* where it is in the world as the game draws it, lifted */
    float u, v;    /* x and z over the torus's span: the trail's texture, wrapped */
} SVert;

typedef struct SBucket
{
    int32_t bx, bz;    /* the square: its corner over SN_BUCKET */
    uint32_t first, n; /* the triangles of it, from g.v[3 * first] */
} SBucket;

typedef struct STile
{
    uint8_t used;   /* something is pressed into it */
    float owed;     /* seconds since it was last faded */
    uint64_t last_ns;
} STile;

typedef struct STrack
{
    float x, z;     /* where the entity was last frame */
    float sx, sz;   /* where its last print went */
    float scale;    /* its feet against a player's */
    float sink;     /* how far it's lowered into the snow (yalms) */
    float gx, gz;   /* where the snow under it was last looked for */
    int gnd, age;   /* snow ground under it; frames since the look */
    int side;       /* the foot to press next: 1 or -1 */
} STrack;

typedef struct SLight
{
    float lx, lz, up; /* the way to the light in the world as the game draws it: across the ground, and how far up (unit length) */
    float hx, hz;     /* across the ground, as a unit vector */
    float tan_e;      /* the light's height over the ground, as a slope */
} SLight;

typedef struct SFoot
{
    float x, z;   /* its middle */
    float fx, fz; /* toward its toes */
    float len, wid;
} SFoot;

typedef struct SPts
{
    float ymax;      /* the lowest point (down is +y) of the entity's model this frame */
    uint16_t n;
    float p[SN_PTS][3]; /* its vertices within SN_BAND of that */
} SPts;

static struct
{
    SnowSettings set;
    int zone;
    SVert* v;           /* the snow triangles, three vertices each, in bucket order */
    uint32_t ntris;
    SBucket* bk;
    uint32_t nbk;
    GfxBuf* vb;
    uint32_t bytes;
    GfxTex* tex; /* the shading: SN_N square, SN_LEVELS mips; grey (2 x it is 1) where nothing is pressed */
    int tex_failed;
    uint32_t* lvl[SN_LEVELS];  /* the shading as the CPU has it, each mip */
    STile tile[SN_TILES][SN_TILES];
    int have_c, cx, cz; /* the tile the window is about (the player's), once it has one: tiles cx-16 to cx+15 are the torus's */
    unsigned nused;
    SLight light;
    int light_set;      /* the scene's light has been taken: kept from then on */
    int blur_r;         /* the softness as a blur's radius in texels (0 to 4), twice over */
    float chars[256][4];
    unsigned nchars;
    float sink_of[256]; /* how far each of them is lowered, by their place in chars */
    uint8_t moving[256]; /* ... and whether it moved last frame (its feet are looked at while it does) */
    STrack trk[SN_TRACKS];
    unsigned ntrk;
    uint64_t last_ns;
} g;

static SPts g_pts[256];

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* (the library's fmaxf, fminf, lroundf and hypotf check for NaNs and the like: slow in a loop of texels) */
static float fmx(float a, float b) { return a > b ? a : b; }

static float fmn(float a, float b) { return a < b ? a : b; }

static int roundi(float v) { return (int)(v < 0.0f ? v - 0.5f : v + 0.5f); }

static float hyp(float a, float b) { return sqrtf(a * a + b * b); }

static float smooth(float a, float b, float x)
{
    const float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static int ifloor(float v) { return (int)floorf(v); }

static float sq(float x) { return x * x; }

/* ---- the snow's triangles, laid out once a zone ---- */

typedef struct TriList
{
    float* t; /* 9 floats a triangle */
    size_t n, cap;
    int over;
} TriList;

static void tl_add(TriList* l, const float t[9])
{
    if (l->n >= SN_MOST_TRIS)
    {
        l->over = 1;
        return;
    }
    if (l->n == l->cap)
    {
        const size_t cap = l->cap ? l->cap * 2 : 4096;
        float* p = (float*)realloc(l->t, cap * 9 * sizeof(float));
        if (!p)
        {
            l->over = 1;
            return;
        }
        l->t = p, l->cap = cap;
    }
    memcpy(&l->t[l->n++ * 9], t, 9 * sizeof(float));
}

/* a triangle cut in two across its longest side until none is over SN_EDGE */
static void tl_split(TriList* out, const float t0[9])
{
    float stack[48][9];
    int sp = 0;
    memcpy(stack[sp++], t0, sizeof stack[0]);
    while (sp > 0 && !out->over)
    {
        float t[9];
        memcpy(t, stack[--sp], sizeof t);
        int le = 0;
        float lm = 0.0f;
        for (int e = 0; e < 3; ++e)
        {
            const float* a = &t[e * 3];
            const float* b = &t[(e + 1) % 3 * 3];
            const float d2 = sq(a[0] - b[0]) + sq(a[1] - b[1]) + sq(a[2] - b[2]);
            if (d2 > lm)
                lm = d2, le = e;
        }
        if (lm <= SN_EDGE * SN_EDGE || sp > 44)
        {
            tl_add(out, t);
            continue;
        }
        const float* a = &t[le * 3];
        const float* b = &t[(le + 1) % 3 * 3];
        const float* c = &t[(le + 2) % 3 * 3];
        const float m[3] = { 0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2]) };
        memcpy(&stack[sp][0], a, 12), memcpy(&stack[sp][3], m, 12), memcpy(&stack[sp][6], c, 12);
        ++sp;
        memcpy(&stack[sp][0], m, 12), memcpy(&stack[sp][3], b, 12), memcpy(&stack[sp][6], c, 12);
        ++sp;
    }
}

/* point p of a mesh through placement matrix m (12 floats: where x, y and z go, then the origin) */
static void place(const float* m, const float* p, float o[3])
{
    for (int j = 0; j < 3; ++j)
        o[j] = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
}

/* a triangle of a placed mesh in the world; 0 if it's steep (over 60 degrees from level, the collision's sides that aren't walls) */
static int world_tri(const float* m, const float* tri, float w[9])
{
    float p[3][3];
    for (int c = 0; c < 3; ++c)
        place(m, &tri[c * 3], p[c]);
    const float ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1], az = p[1][2] - p[0][2];
    const float bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1], bz = p[2][2] - p[0][2];
    const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    if (4.0f * ny * ny < nx * nx + ny * ny + nz * nz)
        return 0;
    for (int c = 0; c < 3; ++c)
        w[c * 3] = p[c][0], w[c * 3 + 1] = p[c][1] - SN_LIFT, w[c * 3 + 2] = p[c][2]; /* (up is -y) */
    return 1;
}

static int floors_ok(const GrassFloors* f)
{
    int ok = f->npairs && f->nmesh && f->ntris && f->tris && f->terrain && f->mesh_first && f->matrices && f->pairs &&
        f->mesh_first[f->nmesh] <= f->ntris;
    for (uint32_t m = 0; ok && m < f->nmesh; ++m)
        ok = f->mesh_first[m] <= f->mesh_first[m + 1];
    for (uint32_t k = 0; ok && k < f->npairs; ++k)
        ok = f->pairs[2 * k] < f->nmatrices && f->pairs[2 * k + 1] < f->nmesh;
    return ok;
}

static void collect(const GrassFloors* f, TriList* out)
{
    const int sand = g.set.sand;
    for (uint32_t k = 0; k < f->npairs && !out->over; ++k)
    {
        const uint32_t mesh = f->pairs[2 * k + 1];
        const float* m = &f->matrices[f->pairs[2 * k] * 12];
        for (uint32_t t = f->mesh_first[mesh]; t < f->mesh_first[mesh + 1]; ++t)
        {
            float w[9];
            if ((f->terrain[t] == 4 /* snow */ || (sand && f->terrain[t] == 3 /* sand */)) && world_tri(m, &f->tris[t * 9], w))
                tl_split(out, w);
        }
    }
}

typedef struct SKey
{
    int32_t bx, bz;
    uint32_t i;
} SKey;

static int key_cmp(const void* a, const void* b)
{
    const SKey* p = (const SKey*)a;
    const SKey* q = (const SKey*)b;
    if (p->bz != q->bz)
        return p->bz < q->bz ? -1 : 1;
    return p->bx < q->bx ? -1 : p->bx > q->bx ? 1 : 0;
}

/* the triangles in bucket order (by where each one's middle is) with their texture coordinates, and the buckets' table */
static int pack(const TriList* l)
{
    SKey* keys = (SKey*)malloc(l->n * sizeof(SKey));
    g.v = (SVert*)malloc(l->n * 3 * sizeof(SVert));
    g.bk = (SBucket*)malloc(l->n * sizeof(SBucket));
    if (!keys || !g.v || !g.bk)
    {
        free(keys);
        return 0;
    }
    for (size_t i = 0; i < l->n; ++i)
    {
        const float* t = &l->t[i * 9];
        keys[i].bx = ifloor((t[0] + t[3] + t[6]) / 3.0f / SN_BUCKET);
        keys[i].bz = ifloor((t[2] + t[5] + t[8]) / 3.0f / SN_BUCKET);
        keys[i].i = (uint32_t)i;
    }
    qsort(keys, l->n, sizeof(SKey), key_cmp);
    g.nbk = 0;
    for (size_t i = 0; i < l->n; ++i)
    {
        const float* t = &l->t[keys[i].i * 9];
        for (int c = 0; c < 3; ++c)
        {
            SVert* v = &g.v[i * 3 + c];
            v->x = t[c * 3], v->y = t[c * 3 + 1], v->z = t[c * 3 + 2];
            v->u = v->x / SN_SPAN, v->v = v->z / SN_SPAN;
        }
        if (!g.nbk || g.bk[g.nbk - 1].bx != keys[i].bx || g.bk[g.nbk - 1].bz != keys[i].bz)
            g.bk[g.nbk++] = (SBucket){ keys[i].bx, keys[i].bz, (uint32_t)i, 0 };
        ++g.bk[g.nbk - 1].n;
    }
    g.ntris = (uint32_t)l->n;
    free(keys);
    return 1;
}

static const SBucket* bucket_find(int bx, int bz)
{
    uint32_t lo = 0, hi = g.nbk;
    while (lo < hi)
    {
        const uint32_t mid = (lo + hi) / 2;
        const SBucket* b = &g.bk[mid];
        if (b->bz == bz && b->bx == bx)
            return b;
        if (b->bz < bz || (b->bz == bz && b->bx < bx))
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* 1 where the snow ground is under (x, z) within SN_GROUND of height ey */
static int grounded(float x, float z, float ey)
{
    const int bx = ifloor(x / SN_BUCKET), bz = ifloor(z / SN_BUCKET);
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const SBucket* b = bucket_find(bx + dx, bz + dz);
            for (uint32_t t = 0; b && t < b->n; ++t)
            {
                const SVert* v = &g.v[(size_t)(b->first + t) * 3];
                const float d = (v[1].z - v[2].z) * (v[0].x - v[2].x) + (v[2].x - v[1].x) * (v[0].z - v[2].z);
                if (fabsf(d) < 1e-9f)
                    continue;
                const float l0 = ((v[1].z - v[2].z) * (x - v[2].x) + (v[2].x - v[1].x) * (z - v[2].z)) / d;
                const float l1 = ((v[2].z - v[0].z) * (x - v[2].x) + (v[0].x - v[2].x) * (z - v[2].z)) / d;
                const float l2 = 1.0f - l0 - l1;
                if (l0 < -0.01f || l1 < -0.01f || l2 < -0.01f)
                    continue;
                if (fabsf(l0 * v[0].y + l1 * v[1].y + l2 * v[2].y - ey) <= SN_GROUND)
                    return 1;
            }
        }
    return 0;
}

/* ---- the trail ---- */

static void trail_free(void)
{
    gfx_tex_destroy(g.tex);
    g.tex = NULL;
    free(g.lvl[0]);
    for (int l = 0; l < SN_LEVELS; ++l)
        g.lvl[l] = NULL;
    memset(g.tile, 0, sizeof g.tile);
    g.have_c = 0, g.nused = 0, g.ntrk = 0, g.light_set = 0;
}

static void zone_free(void)
{
    trail_free();
    gfx_buf_destroy(g.vb);
    g.vb = NULL, g.bytes = 0;
    free(g.v), free(g.bk);
    g.v = NULL, g.bk = NULL, g.ntris = g.nbk = 0;
    g.tex_failed = 0;
}

static void pts_reset(void)
{
    for (int i = 0; i < 256; ++i)
        g_pts[i].ymax = -1e30f, g_pts[i].n = 0;
}

static int trail_make(void)
{
    if (g.tex)
        return 1;
    if (g.tex_failed)
        return 0;
    size_t total = 0;
    for (int l = 0; l < SN_LEVELS; ++l)
        total += (size_t)(SN_N >> l) * (SN_N >> l);
    g.lvl[0] = (uint32_t*)malloc(total * 4);
    if (g.lvl[0])
    {
        memset(g.lvl[0], 0x80, total * 4); /* grey: 2 x 0.5 x the picture */
        for (int l = 1; l < SN_LEVELS; ++l)
            g.lvl[l] = g.lvl[l - 1] + (size_t)(SN_N >> (l - 1)) * (SN_N >> (l - 1));
        g.tex = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, SN_N, SN_N, SN_LEVELS, GFX_USE_SAMPLE);
    }
    if (!g.tex)
    {
        trail_free();
        g.tex_failed = 1;
        return 0;
    }
    for (int l = 0; l < SN_LEVELS; ++l)
        gfx_tex_upload(g.tex, 0, (uint32_t)l, g.lvl[l], (uint32_t)(SN_N >> l) * 4);
    pts_reset();
    return 1;
}

/* the world tile a torus slot holds, with tile `centre` the window's */
static int slot_world(int slot, int centre)
{
    const int lo = centre - SN_HALF;
    return lo + ((slot - lo) & (SN_TILES - 1));
}

static int tile_valid(int wx, int wz)
{
    return wx >= g.cx - SN_HALF && wx < g.cx + SN_HALF && wz >= g.cz - SN_HALF && wz < g.cz + SN_HALF;
}

/* 1 if (x, z) is in the window, `margin` tiles in from its edge */
static int in_window(float x, float z, int margin)
{
    const int wx = ifloor(x / SN_TILE_YALM), wz = ifloor(z / SN_TILE_YALM);
    return wx >= g.cx - SN_HALF + margin && wx < g.cx + SN_HALF - margin && wz >= g.cz - SN_HALF + margin &&
        wz < g.cz + SN_HALF - margin;
}

/* the mips of a rect of the texture (texels x..x+w by y..y+h of its level 0, within the torus: no wrapping) made from the level before */
static void mips_rect(int x, int y, int w, int h)
{
    for (int l = 1; l < SN_LEVELS; ++l)
    {
        const int lw = SN_N >> l, pw = SN_N >> (l - 1);
        const int a = x >> l, b = (x + w + (1 << l) - 1) >> l, c = y >> l, d = (y + h + (1 << l) - 1) >> l;
        for (int j = c; j < d; ++j)
            for (int i = a; i < b; ++i)
            {
                const uint32_t* s = &g.lvl[l - 1][(size_t)j * 2 * pw + (size_t)i * 2];
                const uint32_t px[4] = { s[0], s[1], s[pw], s[pw + 1] };
                uint32_t out = 0;
                for (int sh = 0; sh < 32; sh += 8)
                    out |= (((px[0] >> sh & 255) + (px[1] >> sh & 255) + (px[2] >> sh & 255) + (px[3] >> sh & 255) + 2) / 4) << sh;
                g.lvl[l][(size_t)j * lw + i] = out;
            }
    }
}

static void upload_rect(int x, int y, int w, int h)
{
    for (int l = 0; l < SN_LEVELS; ++l)
    {
        const int lw = SN_N >> l;
        const int a = x >> l, b = (x + w + (1 << l) - 1) >> l, c = y >> l, d = (y + h + (1 << l) - 1) >> l;
        if (b <= a || d <= c)
            continue;
        gfx_tex_upload_rect(g.tex, 0, (uint32_t)l, (uint32_t)a, (uint32_t)c, (uint32_t)(b - a), (uint32_t)(d - c),
            &g.lvl[l][(size_t)c * lw + a], (uint32_t)lw * 4);
    }
}

/* a rect of world texels (it may cross the torus's seam: made and sent in the pieces either side) made into mips and sent to the GPU */
static void refresh_rect(int i0, int j0, int w, int h)
{
    while (w > 0)
    {
        const int x = i0 & SN_MASK, cw = SN_N - x < w ? SN_N - x : w;
        int jj = j0, hh = h;
        while (hh > 0)
        {
            const int y = jj & SN_MASK, ch = SN_N - y < hh ? SN_N - y : hh;
            mips_rect(x, y, cw, ch);
            upload_rect(x, y, cw, ch);
            jj += ch, hh -= ch;
        }
        i0 += cw, w -= cw;
    }
}

/* a tile made neutral again (the torus brought it round to the window's far side) */
static void tile_clear(int sx, int sz)
{
    STile* t = &g.tile[sz][sx];
    if (!t->used)
        return;
    for (int l = 0; l < SN_LEVELS; ++l)
    {
        const int s = SN_TILE >> l, w = SN_N >> l;
        for (int y = 0; y < s; ++y)
            memset(&g.lvl[l][(size_t)(sz * s + y) * w + (size_t)sx * s], 0x80, (size_t)s * 4);
        gfx_tex_upload_rect(g.tex, 0, (uint32_t)l, (uint32_t)(sx * s), (uint32_t)(sz * s), (uint32_t)s, (uint32_t)s,
            &g.lvl[l][(size_t)sz * s * w + (size_t)sx * s], (uint32_t)w * 4);
    }
    t->used = 0, t->owed = 0.0f;
}

/* the window about the player's tile: tiles it leaves are cleared where the torus brings them round to its far side */
static void window_move(const float* focus)
{
    const int ncx = ifloor(focus[0] / SN_TILE_YALM), ncz = ifloor(focus[2] / SN_TILE_YALM);
    if (!g.have_c || abs(ncx - g.cx) >= SN_TILES || abs(ncz - g.cz) >= SN_TILES)
    {
        for (int sz = 0; sz < SN_TILES; ++sz)
            for (int sx = 0; sx < SN_TILES; ++sx)
                tile_clear(sx, sz);
        g.cx = ncx, g.cz = ncz, g.have_c = 1;
        return;
    }
    while (g.cx != ncx)
    {
        const int step = ncx > g.cx ? 1 : -1;
        const int col = step > 0 ? g.cx + SN_HALF : g.cx - SN_HALF - 1; /* the column the step brings in */
        for (int sz = 0; sz < SN_TILES; ++sz)
            tile_clear(col & (SN_TILES - 1), sz);
        g.cx += step;
    }
    while (g.cz != ncz)
    {
        const int step = ncz > g.cz ? 1 : -1;
        const int row = step > 0 ? g.cz + SN_HALF : g.cz - SN_HALF - 1;
        for (int sx = 0; sx < SN_TILES; ++sx)
            tile_clear(sx, row & (SN_TILES - 1));
        g.cz += step;
    }
}

/* ---- the light, taken once ---- */

/* the way to the sun (the moon's, as the game lights the scene) in the world, from the scene's camera-space one. Until the scene gives
 * one the light is a fixed one from the upper left; once it has, that one is kept for the zone: the prints are shaded by it as they're
 * pressed in and never again */
static void light_update(const GfxScene* s)
{
    if (g.light_set)
        return;
    float w[3] = { 0.55f, -0.6f, 0.58f }; /* no light in the scene: from the upper left, in the world as the game draws it (up is -y) */
    const int has = s->sun_dir[3] != 0.0f;
    if (has)
        for (int j = 0; j < 3; ++j)
            w[j] = s->sun_dir[0] * s->view[j * 4] + s->sun_dir[1] * s->view[j * 4 + 1] + s->sun_dir[2] * s->view[j * 4 + 2];
    const float len = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (!(len > 1e-4f))
        return;
    const float up = clampf(-w[1] / len, 0.2f, 1.0f); /* a low or setting sun is held at 0.2: the shadows stay short and steady */
    const float hl = sqrtf(w[0] * w[0] + w[2] * w[2]);
    const float hs = sqrtf(1.0f - up * up);
    SLight n = { 0.0f, 0.0f, up, 0.0f, 0.0f, 20.0f };
    if (hl > 1e-3f && hs > 0.05f)
    {
        n.hx = w[0] / hl, n.hz = w[2] / hl;
        n.lx = n.hx * hs, n.lz = n.hz * hs;
        n.tan_e = up / hs;
    }
    g.light = n;
    if (has)
    {
        g.light_set = 1;
        rt_log("[recomp] snow: the light toward the sun, kept for the prints: across the ground %.2f %.2f, up %.2f\n", n.lx, n.lz, n.up);
    }
}

/* ---- a print's patch: heights, blurred, shaded ---- */

/* a box blur of radius r along each of n lines of a w x h patch (step: texels to the next along a line; line: to the next line), edges held */
static void blur_axis(uint8_t* a, int n, int len, int r, int step, int line)
{
    int out[SN_PATCH];
    for (int ln = 0; ln < n; ++ln)
    {
        uint8_t* s = a + ln * line;
        int acc = 0;
        for (int k = -r; k <= r; ++k)
            acc += s[clampi(k, 0, len - 1) * step];
        for (int i = 0; i < len; ++i)
        {
            out[i] = acc / (2 * r + 1);
            acc += s[clampi(i + r + 1, 0, len - 1) * step] - s[clampi(i - r, 0, len - 1) * step];
        }
        for (int i = 0; i < len; ++i)
            s[i * step] = (uint8_t)out[i];
    }
}

/* m: 1 where any texel within 8 of it in the w x h patch isn't flat; the shading of the rest is neutral and costs nothing */
static void near_mask(const uint8_t* b, uint8_t* m, int w, int h)
{
    static int pre[SN_PATCH + 1];
    static uint8_t t[SN_PATCH * SN_PATCH];
    for (int py = 0; py < h; ++py)
    {
        pre[0] = 0;
        for (int px = 0; px < w; ++px)
            pre[px + 1] = pre[px] + (b[py * w + px] != SN_FLAT);
        for (int px = 0; px < w; ++px)
            t[py * w + px] = (uint8_t)(pre[clampi(px + 9, 0, w)] - pre[clampi(px - 8, 0, w)] > 0);
    }
    for (int px = 0; px < w; ++px)
    {
        pre[0] = 0;
        for (int py = 0; py < h; ++py)
            pre[py + 1] = pre[py] + t[py * w + px];
        for (int py = 0; py < h; ++py)
            m[py * w + px] = (uint8_t)(pre[clampi(py + 9, 0, h)] - pre[clampi(py - 8, 0, h)] > 0);
    }
}

/* what the picture under a texel is scaled by, as 2 x the colour (0.5 of it is no change): the walls turned to the light lit and
 * from it shaded, the cavity darkened and a raised lip a little lit, the rim's shadow over the floor, a bluish shade. p is a patch W
 * wide, i the texel, tap the offsets of the rim's shadow's taps toward the light, ds the depth of a height unit in yalms */
static uint32_t shade_texel(const uint8_t* p, int i, int W, const int* tap, float ds)
{
    const SLight* L = &g.light;
    const int sx = (p[i - W + 1] + 2 * p[i + 1] + p[i + W + 1]) - (p[i - W - 1] + 2 * p[i - 1] + p[i + W - 1]);
    const int sz = (p[i + W - 1] + 2 * p[i + W] + p[i + W + 1]) - (p[i - W - 1] + 2 * p[i - W] + p[i - W + 1]);
    const float gx = (float)sx * ds / (8.0f * SN_CELL), gz = (float)sz * ds / (8.0f * SN_CELL); /* the depth's slope, a Sobel */
    const float ndl = (gx * L->lx + L->up + gz * L->lz) / sqrtf(gx * gx + 1.0f + gz * gz);
    const float lit = ndl / fmx(L->up, 0.2f); /* 1 on flat ground */
    const int ring = p[i - 4] + p[i + 4] + p[i - 4 * W] + p[i + 4 * W] + p[i - 3 - 3 * W] + p[i + 3 - 3 * W] + p[i - 3 + 3 * W] +
        p[i + 3 + 3 * W];
    const float cav = clampf(((float)(p[i] - SN_FLAT) - (float)(ring - 8 * SN_FLAT) / 8.0f) * ds / 0.06f, -1.0f, 1.0f);
    const float here = (float)(p[i] - SN_FLAT) * ds;
    const float ease = 0.04f + 0.08f * g.set.soft; /* how far over the ray to the light the rim must stand to shade fully */
    float shadow = 0.0f;
    for (int k = 0; k < 4; ++k)
    {
        const float rise = (here - (float)(p[i + tap[k]] - SN_FLAT) * ds) - L->tan_e * (float)(2 * k + 2) * SN_CELL; /* the rim over the ray to the light */
        const float s = clampf(rise / ease, 0.0f, 1.0f);
        shadow = s > shadow ? s : shadow;
    }
    float f = 1.0f + 0.85f * (lit - 1.0f);
    f *= 1.0f - 0.32f * fmx(cav, 0.0f) + 0.10f * fmx(-cav, 0.0f);
    f *= 1.0f - 0.40f * shadow;
    f = clampf(1.0f + g.set.strength * (f - 1.0f), 0.0f, 2.0f);
    const float dk = fmx(0.0f, 1.0f - f); /* the shade's blue: less red and a little less green */
    const uint32_t r = (uint32_t)(clampf(0.5f * f * (1.0f - 0.10f * dk), 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint32_t gr = (uint32_t)(clampf(0.5f * f * (1.0f - 0.05f * dk), 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint32_t b = (uint32_t)(clampf(0.5f * f, 0.0f, 1.0f) * 255.0f + 0.5f);
    return 0xFF000000u | r << 16 | gr << 8 | b;
}

/* a sole's dent and the lip pushed up beyond it, at (a, b) along and across its length and width: wider at the toe than the heel,
 * narrower at the arch, a rounded end, a wall a few centimetres wide, a little more pressure at the heel and the ball */
static void foot_profile(float a, float b, float len, float wid, float* dent, float* lip)
{
    *dent = *lip = 0.0f;
    const float u = a / (0.5f * len), hw = 0.5f * wid;
    if (fabsf(u) > 1.6f)
        return;
    const float k = u / 0.32f;
    const float wf = (0.88f + 0.12f * clampf(u, -1.0f, 1.0f)) * (1.0f - 0.16f * expf(-k * k));
    const float v = b / (hw * wf);
    const float m = sqrtf(powf(fabsf(u), 2.4f) + v * v); /* 1 on the outline */
    const float out = (m - 1.0f) * hw;                    /* yalms past it, below 0 inside */
    const float k_soft = 1.0f + 2.0f * g.set.soft; /* the softness widens the wall and the lip */
    if (out < 0.0f)
    {
        const float press = 0.88f + 0.12f * (expf(-sq((u - 0.45f) / 0.3f)) + expf(-sq((u + 0.62f) / 0.28f)));
        *dent = smooth(0.0f, clampf(0.05f * k_soft, 0.02f, 0.7f * hw), -out) * press;
    }
    else if (out < 0.4f)
        *lip = 0.30f * expf(-sq(out / (0.05f * k_soft)));
}

/* a patch's texel multiplied into the texture's: 2 x 2 x the shading of both, held above a floor so prints on prints don't go black */
static uint32_t mul_shade(uint32_t old, uint32_t add)
{
    uint32_t out = old & 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8)
    {
        const int v = (int)((old >> sh & 255) * (add >> sh & 255) + 64) / 128;
        out |= (uint32_t)clampi(v, 56, 255) << sh;
    }
    return out;
}

/* a foot pressed in, baked: its heights as a patch (dent, lip, blur), shaded by the light kept, multiplied into the texture, the part
 * it changed made into mips and sent to the GPU; the tiles it reached are marked in use */
static void bake_foot(const SFoot* f, float amp)
{
    static uint8_t P[SN_PATCH * SN_PATCH], M[SN_PATCH * SN_PATCH];
    const int half = (int)((0.5f * fmx(f->len, f->wid) + 0.4f) / SN_CELL) + 1; /* texels from its middle to the lip's reach */
    const int pad = half + SN_MARGIN, w = 2 * pad + 1;
    if (w > SN_PATCH)
        return;
    const int i0 = ifloor(f->x / SN_CELL) - pad, j0 = ifloor(f->z / SN_CELL) - pad;
    memset(P, SN_FLAT, (size_t)w * w);
    for (int y = pad - half; y <= pad + half; ++y)
        for (int x = pad - half; x <= pad + half; ++x)
        {
            const float rx = ((float)(i0 + x) + 0.5f) * SN_CELL - f->x, rz = ((float)(j0 + y) + 0.5f) * SN_CELL - f->z;
            float dent, lip;
            foot_profile(rx * f->fx + rz * f->fz, -rx * f->fz + rz * f->fx, f->len, f->wid, &dent, &lip);
            if (dent > 0.004f)
                P[y * w + x] = (uint8_t)(SN_FLAT + (int)(dent * amp * 127.0f + 0.5f));
            else if (lip > 0.004f)
                P[y * w + x] = (uint8_t)(SN_FLAT - (int)(lip * amp * 127.0f + 0.5f));
        }
    for (int k = 0; k < 2 && g.blur_r > 0; ++k)
        blur_axis(P, w, w, g.blur_r, 1, w), blur_axis(P, w, w, g.blur_r, w, 1);
    near_mask(P, M, w, w);
    int tap[4];
    for (int k = 0; k < 4; ++k)
        tap[k] = roundi(g.light.hz * (float)(2 * k + 2)) * w + roundi(g.light.hx * (float)(2 * k + 2));
    const float ds = g.set.depth / 127.0f;
    const int lo = pad - half - 16, hi = pad + half + 16; /* (where the blur and the shading reach: the margin past is only read) */
    for (int y = lo; y <= hi; ++y)
        for (int x = lo; x <= hi; ++x)
        {
            if (!M[y * w + x])
                continue;
            const uint32_t v = shade_texel(P, y * w + x, w, tap, ds);
            uint32_t* dst = &g.lvl[0][(size_t)((j0 + y) & SN_MASK) * SN_N + (size_t)((i0 + x) & SN_MASK)];
            *dst = mul_shade(*dst, v);
        }
    refresh_rect(i0 + lo, j0 + lo, hi - lo + 1, hi - lo + 1);
    for (int tz = (j0 + lo) >> 6; tz <= (j0 + hi) >> 6; ++tz) /* (64 texels a tile) */
        for (int tx = (i0 + lo) >> 6; tx <= (i0 + hi) >> 6; ++tx)
        {
            STile* t = &g.tile[tz & (SN_TILES - 1)][tx & (SN_TILES - 1)];
            if (!t->used)
                t->used = 1, t->last_ns = gfx_now_ns(), t->owed = 0.0f;
        }
}

/* ---- fading ---- */

/* a used tile's shading eased toward none by share a of what's left, its mips made and the tile sent again; it's in use no more once nothing's left */
static void tile_fade(int sx, int sz, float a)
{
    int left = 0;
    for (int y = 0; y < SN_TILE; ++y)
    {
        uint8_t* row = (uint8_t*)&g.lvl[0][(size_t)(sz * SN_TILE + y) * SN_N + (size_t)sx * SN_TILE];
        for (int x = 0; x < SN_TILE; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const int d = (int)row[x * 4 + c] - 128, d2 = (int)((float)d * (1.0f - a)); /* (the truncation sees to it that it ends) */
                row[x * 4 + c] = (uint8_t)(128 + d2);
                left |= d2 != 0;
            }
    }
    refresh_rect(sx * SN_TILE, sz * SN_TILE, SN_TILE, SN_TILE);
    if (!left)
        g.tile[sz][sx].used = 0;
}

/* each used tile is faded a little at a time: an exponential, 5% of its time constant (fade / 3) between, two tiles a frame at most */
static void fade_step(uint64_t now)
{
    const float fade = g.set.fade;
    int budget = 2;
    g.nused = 0;
    for (int sz = 0; sz < SN_TILES; ++sz)
        for (int sx = 0; sx < SN_TILES; ++sx)
        {
            STile* t = &g.tile[sz][sx];
            if (!t->used)
                continue;
            if (fade > 0.0f && budget > 0)
            {
                const float since = (float)((double)(now - t->last_ns) * 1e-9);
                if (since >= fmx(1.0f, fade / 60.0f))
                {
                    tile_fade(sx, sz, 1.0f - expf(-3.0f * since / fade));
                    t->last_ns = now, --budget;
                }
            }
            g.nused += t->used;
        }
}

/* ---- the characters' feet, from their models ---- */

static int nearest_entity(float x, float z, float within)
{
    int best = -1;
    float bd = within * within;
    for (unsigned i = 0; i < g.nchars; ++i)
    {
        const float* c = g.chars[i];
        const float dx = c[0] - x, dz = c[2] - z, d2 = dx * dx + dz * dz;
        if (c[3] >= 0.0f && d2 < bd && in_window(c[0], c[2], 0))
            bd = d2, best = (int)i;
    }
    return best;
}

/* the points kept lower than the new lowest point's band are let go */
static void pts_trim(SPts* P)
{
    unsigned k = 0;
    for (unsigned i = 0; i < P->n; ++i)
        if (P->p[i][1] >= P->ymax - SN_BAND)
        {
            if (k != i)
                memcpy(P->p[k], P->p[i], sizeof P->p[0]);
            ++k;
        }
    P->n = (uint16_t)k;
}

/* the vertices of a draw of this entity's model that lie in the band over its lowest: its feet, as the game skinned them */
static void gather_feet(unsigned ent, const uint8_t* cpu, uint32_t bytes, uint32_t stride, uint32_t at)
{
    SPts* P = &g_pts[ent];
    const float* c = g.chars[ent];
    uint32_t n = (bytes - at - 12) / stride + 1;
    if (n > 6000)
        n = 6000;
    for (uint32_t v = 0; v < n; ++v)
    {
        float p[3];
        memcpy(p, cpu + (size_t)v * stride + at, 12);
        if (!(fabsf(p[0]) < 1e6f && fabsf(p[1]) < 1e6f && fabsf(p[2]) < 1e6f))
            continue;
        const float dx = p[0] - c[0], dz = p[2] - c[2];
        if (dx * dx + dz * dz > 0.81f || p[1] < P->ymax - SN_BAND || p[1] > c[1] + 0.5f)
            continue;
        if (p[1] > P->ymax)
            P->ymax = p[1], pts_trim(P);
        if (P->n < SN_PTS)
            memcpy(P->p[P->n++], p, 12);
    }
}

/* the foot of side (1 or -1: on the left or right of the way it's going, (fx, fz)) as its model has it this frame: where it is and how
 * big. 0 if the model gave too little (a foot off the ground, a model that isn't there) */
static int measure_foot(unsigned ent, const float* c, float fx, float fz, int side, SFoot* o)
{
    const SPts* P = &g_pts[ent];
    if (P->n < 6 || fabsf(P->ymax - c[1]) > 0.6f)
        return 0;
    float a0 = 1e9f, a1 = -1e9f, b0 = 1e9f, b1 = -1e9f;
    int n = 0;
    for (unsigned k = 0; k < P->n; ++k)
    {
        const float rx = P->p[k][0] - c[0], rz = P->p[k][2] - c[2];
        const float a = rx * fx + rz * fz, b = -rx * fz + rz * fx;
        if (b * (float)side <= 0.0f)
            continue;
        a0 = fmn(a0, a), a1 = fmx(a1, a), b0 = fmn(b0, b), b1 = fmx(b1, b);
        ++n;
    }
    if (n < 4 || a1 - a0 < 0.08f)
        return 0;
    const float ac = 0.5f * (a0 + a1), bc = 0.5f * (b0 + b1);
    o->x = c[0] + fx * ac - fz * bc, o->z = c[2] + fz * ac + fx * bc;
    o->fx = fx, o->fz = fz;
    o->len = clampf(a1 - a0, 0.12f, 0.7f), o->wid = clampf(b1 - b0, 0.06f, 0.32f);
    return 1;
}

void snow_character_draw(GfxDraw* d)
{
    if (!g.set.enabled || !g.ntris || !g.have_c || !g.nchars)
        return;
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    if (!e->used || e->stream >= GFX_NSTREAMS || (e->type != GFX_FLOAT3 && e->type != GFX_FLOAT4))
        return;
    const uint8_t* cpu = (const uint8_t*)d->cpu[e->stream];
    const uint32_t bytes = d->cpu_size[e->stream], stride = (uint32_t)d->u.stride[e->stream], at = (uint32_t)d->u.offset[GFX_R_POSITION];
    if (!cpu || !stride || bytes < at + 12)
        return;
    float p0[3];
    memcpy(p0, cpu + at, 12);
    const int ent = nearest_entity(p0[0], p0[2], 2.2f);
    if (ent < 0)
        return;
    if (g.moving[ent])
        gather_feet((unsigned)ent, cpu, bytes, stride, at); /* (a foot is only wanted while it walks) */
    const float s = g.sink_of[ent]; /* lowered into the snow: the model's matrix moved down, the ground's depth hides what's under it */
    if (s >= 0.002f)
        for (int k = 0; k < 4; ++k)
            d->u.wvp[12 + k] += s * d->u.wvp[4 + k], d->u.wv[12 + k] += s * d->u.wv[4 + k];
}

/* ---- the entities ---- */

/* how big an entity's feet are against a player's, from its hitbox, until its model says */
static float foot_scale(float hitbox) { return hitbox > 0.25f ? clampf(hitbox / 0.5f, 0.8f, 3.0f) : 1.0f; }

static STrack track_new(const float* c)
{
    const STrack t = { c[0], c[2], c[0], c[2], foot_scale(c[3]), 0.0f, -1e9f, -1e9f, 0, 1000, 1 };
    return t;
}

/* whether there's snow under the entity: looked for when it's moved a little or some frames have gone */
static void track_ground(STrack* t, const float* c)
{
    const float dx = c[0] - t->gx, dz = c[2] - t->gz;
    if (t->age < 20 && dx * dx + dz * dz < 0.16f)
    {
        ++t->age;
        return;
    }
    t->gnd = grounded(c[0], c[2], c[1]), t->gx = c[0], t->gz = c[2], t->age = 0;
}

/* the sink eases to where it's going (the snow's depth under the entity, or none), so stepping on and off doesn't jump */
static void track_sink(STrack* t, float dt)
{
    const float target = g.set.sink > 0.0f && t->gnd ? g.set.sink * t->scale : 0.0f;
    t->sink += (target - t->sink) * clampf(dt * 6.0f, 0.0f, 1.0f);
    if (fabsf(target - t->sink) < 0.002f)
        t->sink = target;
}

static void foot_place(STrack* t, const float* c, unsigned ent, float fx, float fz)
{
    SFoot f = { t->sx - fz * (float)t->side * 0.14f * t->scale, t->sz + fx * (float)t->side * 0.14f * t->scale, fx, fz,
        SN_FOOT * t->scale, 0.4f * SN_FOOT * t->scale };
    SFoot m;
    if (measure_foot(ent, c, fx, fz, t->side, &m))
    {
        f = m;
        t->scale = clampf(t->scale + (m.len / SN_FOOT - t->scale) * 0.15f, 0.4f, 3.0f);
    }
    bake_foot(&f, 0.9f);
}

/* an entity's walk since last frame: a foot pressed in every stride along the way */
static void track_walk(STrack* t, const float* c, unsigned ent)
{
    const float stride = SN_STRIDE * t->scale;
    if (hyp(c[0] - t->sx, c[2] - t->sz) > 12.0f)
        t->sx = c[0], t->sz = c[2]; /* (it jumped: a warp, a new zone) */
    float dx = c[0] - t->sx, dz = c[2] - t->sz, dist = hyp(dx, dz);
    for (int n = 0; dist >= stride && dist <= 12.0f && n < 12; ++n)
    {
        const float fx = dx / dist, fz = dz / dist;
        t->sx += fx * stride, t->sz += fz * stride;
        if (in_window(t->sx, t->sz, 1) && grounded(t->sx, t->sz, c[1]))
            foot_place(t, c, ent, fx, fz);
        t->side = -t->side;
        dx = c[0] - t->sx, dz = c[2] - t->sz, dist = hyp(dx, dz);
    }
    t->x = c[0], t->z = c[2];
}

/* the entity from last frame nearest to c (within a yard and a half) that hasn't been taken, or -1 */
static int track_match(const float* c, const uint8_t* taken)
{
    int best = -1;
    float bd = 1.5f * 1.5f;
    for (unsigned j = 0; j < g.ntrk; ++j)
    {
        const float dx = g.trk[j].x - c[0], dz = g.trk[j].z - c[2], d2 = dx * dx + dz * dz;
        if (!taken[j] && d2 < bd)
            bd = d2, best = (int)j;
    }
    return best;
}

static void entities_step(uint64_t now)
{
    static STrack next[SN_TRACKS];
    uint8_t taken[SN_TRACKS] = { 0 };
    const float dt = g.last_ns ? clampf((float)((double)(now - g.last_ns) * 1e-9), 0.0f, 0.2f) : 0.0f;
    g.last_ns = now;
    unsigned nn = 0;
    memset(g.sink_of, 0, sizeof g.sink_of);
    memset(g.moving, 0, sizeof g.moving);
    for (unsigned i = 0; i < g.nchars && nn < SN_TRACKS; ++i)
    {
        const float* c = g.chars[i];
        if (c[3] < 0.0f)
            continue; /* a fixture */
        const int j = track_match(c, taken);
        STrack t = track_new(c);
        if (j >= 0)
            taken[j] = 1, t = g.trk[j];
        g.moving[i] = hyp(c[0] - t.x, c[2] - t.z) > 0.005f;
        track_ground(&t, c);
        track_sink(&t, dt);
        g.sink_of[i] = t.sink;
        track_walk(&t, c, i);
        next[nn++] = t;
    }
    memcpy(g.trk, next, nn * sizeof(STrack));
    g.ntrk = nn;
    pts_reset(); /* (this frame's models are spent: the next frame's draws bring new) */
}

/* ---- drawing ---- */

static void mat_mul(float* o, const float* a, const float* b)
{
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, 64);
}

/* 1 if a tile of the trail that's used lies within reach of the bucket's triangles */
static int bucket_has_trail(int bx, int bz)
{
    const int x0 = ifloor((bx * SN_BUCKET - SN_EDGE - 1.0f) / SN_TILE_YALM), x1 = ifloor(((bx + 1) * SN_BUCKET + SN_EDGE + 1.0f) / SN_TILE_YALM);
    const int z0 = ifloor((bz * SN_BUCKET - SN_EDGE - 1.0f) / SN_TILE_YALM), z1 = ifloor(((bz + 1) * SN_BUCKET + SN_EDGE + 1.0f) / SN_TILE_YALM);
    for (int wz = z0; wz <= z1; ++wz)
        for (int wx = x0; wx <= x1; ++wx)
            if (tile_valid(wx, wz) && g.tile[wz & (SN_TILES - 1)][wx & (SN_TILES - 1)].used)
                return 1;
    return 0;
}

/* the fixed function: the trail's shading over the scene as 2 x it x the picture (SRC x DEST + DEST x SRC), depth-tested against the
 * scene's depth and writing none; the scene's viewport and camera, and DLSS's jitter */
static void draw_state(GfxDraw* d, const GfxScene* s)
{
    memset(d, 0, sizeof *d);
    d->vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d->vs.el[GFX_R_TEXCOORD0] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
    d->u.offset[GFX_R_POSITION] = (int32_t)offsetof(SVert, x), d->u.offset[GFX_R_TEXCOORD0] = (int32_t)offsetof(SVert, u);
    d->u.stride[0] = (int32_t)sizeof(SVert);
    d->vs.ntex = 1;
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){ 2 /* SELECTARG1 */, 2 /* TEXTURE */, 0, 1, 2, 2, 0, 1, 1, 1, 0, 2 };
    for (int i = 0; i < 4; ++i)
        d->u.mat_d[i] = d->u.mat_a[i] = d->u.ambient[i] = d->u.tfactor[i] = 1.0f;
    float jit[2];
    if (gfx_dlss_jitter(jit))
        memcpy(&d->u.pad0[0], &jit[0], 4), memcpy(&d->u.pad0[1], &jit[1], 4);
    float vpm[16];
    mat_mul(vpm, s->view, s->proj);
    memcpy(d->u.wvp, vpm, 64);
    memcpy(d->u.wv, s->view, 64);
    for (int i = 0; i < 4; ++i)
        d->u.vp[i] = (float)s->vp[i];
    memcpy(d->vp, s->vp, sizeof d->vp);
    d->pipe.blend = 1, d->pipe.src = 9 /* DESTCOLOR */, d->pipe.dst = 3 /* SRCCOLOR */, d->pipe.op = 1 /* ADD */;
    d->pipe.write_mask = 0x7; /* red, green, blue */
    d->depth.zenable = 1, d->depth.zwrite = 0, d->depth.zfunc = 4; /* LESSEQUAL */
    d->zbias = 2;
    d->cull = 1; /* NONE */
    d->fill = 3; /* SOLID */
    d->prim = GFX_TRIANGLELIST;
    d->tex[0] = g.tex;
    d->samp[0] = (GfxSampler){ 1, 1, 1, 2, 3, 2, 8, 0, 0, 0, { 0, 0 }, 0 }; /* wrapped, anisotropic, mipped */
    d->buf[0] = g.vb, d->size[0] = g.bytes;
}

static void overlay_draw(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    static GfxDraw d;
    draw_state(&d, s);
    gfx_set_targets(color, 0, 0, depth);
    /* the buckets whose triangles lie wholly in the tiles the torus holds, 4 yalms in from its edge */
    const float xlo = (float)(g.cx - SN_HALF) * SN_TILE_YALM + SN_EDGE, xhi = (float)(g.cx + SN_HALF) * SN_TILE_YALM - SN_EDGE;
    const float zlo = (float)(g.cz - SN_HALF) * SN_TILE_YALM + SN_EDGE, zhi = (float)(g.cz + SN_HALF) * SN_TILE_YALM - SN_EDGE;
    const int bx0 = (int)ceilf(xlo / SN_BUCKET), bx1 = ifloor(xhi / SN_BUCKET) - 1;
    const int bz0 = (int)ceilf(zlo / SN_BUCKET), bz1 = ifloor(zhi / SN_BUCKET) - 1;
    for (int bz = bz0; bz <= bz1; ++bz)
        for (int bx = bx0; bx <= bx1; ++bx)
        {
            const SBucket* b = bucket_find(bx, bz);
            if (!b || !bucket_has_trail(bx, bz))
                continue;
            d.buf_off[0] = b->first * 3u * (uint32_t)sizeof(SVert), d.count = b->n;
            gfx_draw(&d);
        }
}

/* ---- the interface ---- */

void snow_configure(const SnowSettings* in)
{
    SnowSettings n = *in;
    n.depth = clampf(n.depth, 0.02f, 0.5f), n.strength = clampf(n.strength, 0.0f, 2.0f), n.fade = clampf(n.fade, 0.0f, 86400.0f);
    n.sink = clampf(n.sink, 0.0f, 1.0f), n.soft = clampf(n.soft, 0.0f, 1.0f);
    g.set = n; /* (prints already pressed in keep the look they were pressed with) */
    g.blur_r = (int)(n.soft * 4.0f + 0.5f); /* the blur, twice over: a tent of up to 8 texels each way */
}

void snow_set_zone(int zone)
{
    if (zone == g.zone)
        return;
    zone_free();
    g.zone = zone;
}

void snow_device_lost(void)
{
    zone_free();
    g.zone = 0;
}

void snow_set_floors(const GrassFloors* f)
{
    zone_free();
    g.zone = f ? f->zone : 0;
    if (!f || !f->zone || !floors_ok(f))
        return;
    TriList l = { 0 };
    collect(f, &l);
    if (l.over)
        rt_log("[recomp] snow: zone %d's snow left out of the trails: more than %u triangles\n", f->zone, SN_MOST_TRIS);
    else if (l.n && pack(&l))
    {
        g.bytes = g.ntris * 3u * (uint32_t)sizeof(SVert);
        g.vb = gfx_buf_create(g.bytes);
        if (g.vb)
            gfx_buf_upload(g.vb, g.v, g.bytes);
        rt_log("[recomp] snow: zone %d has %u triangles of snow ground (%u buckets, %u KB) for the trails\n", f->zone, g.ntris, g.nbk,
            g.bytes >> 10);
        if (!g.vb)
            zone_free(), g.zone = f->zone;
    }
    else if (l.n)
        zone_free(), g.zone = f->zone;
    free(l.t);
}

void snow_set_characters(const float* xyzr, unsigned count)
{
    g.nchars = count < 256 ? count : 256;
    if (g.nchars)
        memcpy(g.chars, xyzr, sizeof(float) * 4 * g.nchars);
}

void snow_scene(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    if (!g.set.enabled)
    {
        if (g.tex)
            trail_free();
        return;
    }
    if (!g.ntris || !g.vb || !color || !depth || s->focus[3] == 0.0f || !trail_make())
        return;
    window_move(s->focus);
    light_update(s);
    const uint64_t now = gfx_now_ns();
    entities_step(now);
    fade_step(now);
    if (g.nused)
        overlay_draw(color, depth, s);
}
