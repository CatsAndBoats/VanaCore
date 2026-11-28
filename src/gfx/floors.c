/* The zone's floors as a layer the reflections read (floors.h).
 *
 * The zone's collision (VanaCore reads it from the zone's DAT, zonefloor.cpp, and hands it over as it does for the grass's
 * floors) has a kind of ground on each triangle, the one the game's footsteps sound for: path, grass, sand, snow, stone,
 * metal, wood, shallow or deep water, or "object" (props: a crate's top, a cart's). The collision lies on the drawn ground to
 * the hundredth, so drawn with the scene's camera into a target of its own, with a depth of its own, each pixel of the layer
 * says what ground is under that pixel of the scene wherever the two depths agree, and the reflections keep to the kinds
 * asked for (refl_floors: stone and metal). Where no floor lies at the pixel's depth (a crate's top over the ground, the water
 * over its bed, ground out of the player's reach) there's none.
 *
 * The triangles are laid in the world once a zone (each placement's, as the DAT has each mesh placed many times) and kept in
 * a static buffer: one draw a frame, the fixed function with the vertex colour alone, the kind in red.
 *
 * Grass takes no part: grass ground is a kind the reflections never keep to, and grass the game lays over stone as cut-outs is
 * left to the reflections' colour test (fx_ssr). Those cut-outs were once drawn again into this layer, a copy of each kept as the
 * scene drew it, which cost the game's thread too much in grassy zones.
 *
 * /vana debug's collision view ([graphics] collision.show) is here too: the zone's whole collision, its walls with the floors
 * (vanacore_gfx_set_collision), laid in the world the same way and drawn over the scene see-through: walls red, floors by their
 * kind of ground. */
#include "floors.h"
#include "rtbvh.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define FLOORS_MOST (4u << 20) /* the most triangles a zone's floors are laid out to (past it the layer is left out) */

typedef struct FVert
{
    float x, y, z;
    uint32_t col; /* D3DCOLOR: the ground's kind + 1, times 16, in red (red 0: no floor, the cleared target) */
} FVert;

static struct
{
    int zone; /* the zone the floors are for, or are being read for (0: none) */
    int none; /* ... and it has none to lay: its collision unread or not whole, or too many triangles (the reflections take any ground) */
    GfxBuf* vb;
    uint32_t ntris, bytes;
    GfxTex* id;    /* the layer: the ground's kind in red */
    GfxTex* depth; /* ... the floors' depth, from the scene's camera */
    uint32_t w, h;
} g;

/* The collision view: the triangles laid in the world, each its colour and shade, drawn while it's held */
#define COLL_MOST (2u << 20) /* the most triangles laid out for it (past it the view shows nothing) */
#define COLL_WALL 0x10u      /* a wall's mark on its terrain (zonefloor::kWall) */
/* floors' colours (0xRRGGBB) by their kind of ground, as Render visual 18's (gfx_d3d12.c KIND_COL, debug.lua KINDS): object, path,
 * grass, sand, snow, stone, metal, wood, shallow water, deep water, any other; the walls' */
static const uint32_t COLL_RGB[11] = { 0xB58CE6, 0xC7B58C, 0x6EC759, 0xE6D16E, 0xF2F5FF, 0xE8945C,
                                       0xE8638C, 0xA86B3D, 0x59C7E6, 0x3D6EDB, 0xFF00FF };
#define COLL_WALL_RGB 0xE8302Cu
#define COLL_FLOOR_A 0x99u /* how much of the colour lies over the picture: floors 60% */
#define COLL_WALL_A 0x80u  /* ... walls 50% */
#define COLL_NEAR 0.996f   /* the camera's space scaled by it for the draw (collision_draw) */

static struct
{
    int zone; /* the zone it's for (0: none held) */
    GfxBuf* vb;
    uint32_t ntris, bytes;
} coll;

static int coll_shown; /* floors_show_collision: drawn while [graphics] collision.show is on (the rain's shelter has the collision read too) */

void floors_show_collision(int on)
{
    coll_shown = on;
}

static void collision_free(void)
{
    gfx_scene_rt_geometry(NULL, 0, 0, 0, 0, NULL, 0); /* (the ray-traced reflections' tree was built from it) */
    gfx_buf_destroy(coll.vb);
    coll.vb = NULL, coll.ntris = coll.bytes = 0;
}

/* point p of a mesh through placement matrix m (12 floats: where x, y and z go, then the origin) */
static void place(const float* m, const float* p, float o[3])
{
    for (int j = 0; j < 3; ++j)
        o[j] = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
}

/* what the floors hold checked (as grass_set_floors checks it): the triangles their placements lay, 0 when anything is out of
 * place (and the zone has none) */
static uint64_t floors_placed(const GrassFloors* f)
{
    int ok = f->npairs && f->nmesh && f->ntris && f->tris && f->terrain && f->mesh_first && f->matrices && f->pairs &&
        f->mesh_first[f->nmesh] <= f->ntris;
    for (uint32_t m = 0; ok && m < f->nmesh; ++m)
        ok = f->mesh_first[m] <= f->mesh_first[m + 1];
    uint64_t total = 0;
    for (uint32_t k = 0; ok && k < f->npairs; ++k)
    {
        ok = f->pairs[2 * k] < f->nmatrices && f->pairs[2 * k + 1] < f->nmesh;
        if (ok)
            total += f->mesh_first[f->pairs[2 * k + 1] + 1] - f->mesh_first[f->pairs[2 * k + 1]];
    }
    return ok ? total : 0;
}

/* triangle t laid in the world by placement matrix m, and its normal (by the placed triangle, not of unit length) */
static void tri_placed(const GrassFloors* f, const float* m, uint32_t t, float w[3][3], float nrm[3])
{
    for (int c = 0; c < 3; ++c)
        place(m, &f->tris[t * 9 + c * 3], w[c]);
    const float ax = w[1][0] - w[0][0], ay = w[1][1] - w[0][1], az = w[1][2] - w[0][2];
    const float bx = w[2][0] - w[0][0], by = w[2][1] - w[0][1], bz = w[2][2] - w[0][2];
    nrm[0] = ay * bz - az * by, nrm[1] = az * bx - ax * bz, nrm[2] = ax * by - ay * bx;
}

/* a placed triangle's three corners (w: 9 floats), each the colour col */
static void tri_verts(FVert* v, const float* w, uint32_t col)
{
    for (int c = 0; c < 3; ++c)
        v[c].x = w[c * 3], v[c].y = w[c * 3 + 1], v[c].z = w[c * 3 + 2], v[c].col = col;
}

static void mat_mul(float* o, const float* a, const float* b)
{
    float t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
    memcpy(o, t, 64);
}

static void targets_free(void)
{
    gfx_scene_floors(NULL, NULL, 0);
    gfx_tex_destroy(g.id), gfx_tex_destroy(g.depth);
    g.id = g.depth = NULL, g.w = g.h = 0;
}

/* Only the floors one can walk on reflect ([graphics.effects] refl_walkable, on by default): a wall's or a roof's top is floor in
 * the collision too, and took the wet floor's mirror where no one ever stands (Aht Urhgan Whitegate, 2026-11-28). The floors are
 * joined into areas once a zone: a grid of WALK_CELL squares, each with the heights the floors over its middle lie at, and two of
 * them, in one square or two side by side, are one area when they're within WALK_STEP of each other: a stair's ramp, a kerb, a
 * doorway's sill join; a wall's top stands a wall's height over the ground and doesn't. The area under the player's feet is
 * walkable (layer_scene, each frame), and stays so: the rest is drawn into the layer as no floor (red 0), which doesn't reflect */
#define WALK_CELL 0.5f
#define WALK_STEP 0.6f
#define WALK_MOST (12u << 20) /* the most squares' heights a zone's floors take (past it every floor reflects) */

typedef struct WEntry
{
    uint32_t key; /* the square: x and z (WALK_CELL, offset 32768) in 16 bits each */
    float y;      /* the floor's height there (the world's y, down) */
    uint32_t tri;
} WEntry;

static struct
{
    FVert* v;          /* the layer's vertices as uploaded */
    uint32_t* kind;    /* each triangle's own colour (its kind of ground) */
    uint32_t* area;    /* each triangle's area (a triangle's number) */
    uint8_t* walk;     /* by area: walkable */
    WEntry* e;         /* the grid, by square then height */
    uint32_t ne, ntri; /* (0 entries: the rule isn't available for the zone, every floor reflects) */
    int shown;         /* the colours in the buffer now: 1 the walkable rule's, 0 every floor's */
    uint32_t nwalk;    /* areas found walkable */
} wk;

static void walk_free(void)
{
    free(wk.v), free(wk.kind), free(wk.area), free(wk.walk), free(wk.e);
    memset(&wk, 0, sizeof wk);
}

static void floors_free(void)
{
    gfx_buf_destroy(g.vb);
    g.vb = NULL, g.ntris = g.bytes = 0, g.none = 0;
    walk_free();
}

static uint32_t walk_root(uint32_t* p, uint32_t a)
{
    while (p[a] != a)
        p[a] = p[p[a]], a = p[a];
    return a;
}

static void walk_join(uint32_t* p, uint32_t a, uint32_t b)
{
    a = walk_root(p, a), b = walk_root(p, b);
    if (a != b)
        p[a < b ? b : a] = a < b ? a : b;
}

static int walk_cmp(const void* a, const void* b)
{
    const WEntry *x = (const WEntry*)a, *y = (const WEntry*)b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : x->y < y->y ? -1 : x->y > y->y ? 1 : 0;
}

static uint32_t walk_key(float x, float z)
{
    const int32_t ix = (int32_t)floorf(x / WALK_CELL) + 32768, iz = (int32_t)floorf(z / WALK_CELL) + 32768;
    return (uint32_t)(ix & 0xFFFF) << 16 | (uint32_t)(iz & 0xFFFF);
}

/* the first entry of square key (or where it would be) */
static uint32_t walk_find(uint32_t key)
{
    uint32_t lo = 0, hi = wk.ne;
    while (lo < hi)
    {
        const uint32_t mid = (lo + hi) / 2;
        if (wk.e[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* triangle t's heights over the middles of the squares it covers (seen from above), or over its centre where it covers none;
 * how many entries it took (written while e is given and there's room) */
static uint32_t walk_tri(const FVert* v, uint32_t t, WEntry* e, uint32_t room)
{
    const FVert *a = &v[t * 3], *b = a + 1, *c = a + 2;
    const float e1x = b->x - a->x, e1z = b->z - a->z, e2x = c->x - a->x, e2z = c->z - a->z;
    const float det = e1x * e2z - e2x * e1z;
    uint32_t n = 0;
    if (fabsf(det) > 1e-8f)
    {
        const float x0 = fminf(a->x, fminf(b->x, c->x)), x1 = fmaxf(a->x, fmaxf(b->x, c->x));
        const float z0 = fminf(a->z, fminf(b->z, c->z)), z1 = fmaxf(a->z, fmaxf(b->z, c->z));
        for (float cz = (floorf(z0 / WALK_CELL) + 0.5f) * WALK_CELL; cz <= z1; cz += WALK_CELL)
            for (float cx = (floorf(x0 / WALK_CELL) + 0.5f) * WALK_CELL; cx <= x1; cx += WALK_CELL)
            {
                const float qx = cx - a->x, qz = cz - a->z;
                const float u = (qx * e2z - e2x * qz) / det, w = (e1x * qz - qx * e1z) / det;
                if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
                    continue;
                if (e && n < room)
                    e[n] = (WEntry){ walk_key(cx, cz), a->y + u * (b->y - a->y) + w * (c->y - a->y), t };
                n++;
            }
    }
    if (!n)
    {
        if (e && room)
            e[0] = (WEntry){ walk_key((a->x + b->x + c->x) / 3.0f, (a->z + b->z + c->z) / 3.0f), (a->y + b->y + c->y) / 3.0f, t };
        n = 1;
    }
    return n;
}

/* the entries of squares at i (one square's) and at j (the square beside it): those within WALK_STEP of each other joined */
static void walk_join_squares(uint32_t* p, uint32_t i, uint32_t i1, uint32_t j, uint32_t j1)
{
    uint32_t lo = j;
    for (; i < i1; ++i)
    {
        while (lo < j1 && wk.e[lo].y < wk.e[i].y - WALK_STEP)
            lo++;
        for (uint32_t k = lo; k < j1 && wk.e[k].y <= wk.e[i].y + WALK_STEP; ++k)
            walk_join(p, wk.e[i].tri, wk.e[k].tri);
    }
}

/* The floors' areas (wk), from the n vertices laid in v (which wk keeps): 0 left without them (too many, no memory) */
static int walk_areas(FVert* v, uint32_t n)
{
    const uint32_t nt = n / 3;
    uint64_t total = 0;
    for (uint32_t t = 0; t < nt && total <= WALK_MOST; ++t)
        total += walk_tri(v, t, NULL, 0);
    wk.v = v, wk.ntri = nt;
    if (total > WALK_MOST)
        return 0;
    wk.e = (WEntry*)malloc((size_t)total * sizeof(WEntry));
    wk.area = (uint32_t*)malloc((size_t)nt * sizeof(uint32_t));
    wk.kind = (uint32_t*)malloc((size_t)nt * sizeof(uint32_t));
    wk.walk = (uint8_t*)calloc(nt ? nt : 1, 1);
    if (!wk.e || !wk.area || !wk.kind || !wk.walk)
        return 0;
    for (uint32_t t = 0; t < nt; ++t)
    {
        wk.area[t] = t, wk.kind[t] = v[t * 3].col;
        wk.ne += walk_tri(v, t, wk.e + wk.ne, (uint32_t)total - wk.ne);
    }
    qsort(wk.e, wk.ne, sizeof(WEntry), walk_cmp);
    for (uint32_t i = 0; i < wk.ne;)
    {
        uint32_t i1 = i;
        while (i1 < wk.ne && wk.e[i1].key == wk.e[i].key)
            ++i1;
        walk_join_squares(wk.area, i, i1, i, i1); /* (in the square itself) */
        const uint32_t key = wk.e[i].key, side[2] = { key + 0x10000u, key + 1u }; /* (x + 1, z + 1) */
        for (int s = 0; s < 2; ++s)
        {
            const uint32_t j = walk_find(side[s]);
            uint32_t j1 = j;
            while (j1 < wk.ne && wk.e[j1].key == side[s])
                ++j1;
            walk_join_squares(wk.area, i, i1, j, j1);
        }
        i = i1;
    }
    uint32_t areas = 0;
    for (uint32_t t = 0; t < nt; ++t)
        areas += (wk.area[t] = walk_root(wk.area, t)) == t;
    rt_log("[recomp] floors: %u triangles of floor joined into %u areas one can walk between (%u squares' heights)\n", nt, areas, wk.ne);
    return 1;
}

/* the layer's colours as the rule says (on: the walkable areas' floors their kind, the rest none; off: every floor its kind) */
static void walk_upload(int on)
{
    for (uint32_t t = 0; t < wk.ntri; ++t)
    {
        const uint32_t col = !on || wk.walk[wk.area[t]] ? wk.kind[t] : 0xFF000000u;
        wk.v[t * 3].col = wk.v[t * 3 + 1].col = wk.v[t * 3 + 2].col = col;
    }
    gfx_buf_upload(g.vb, wk.v, g.bytes);
    wk.shown = on;
}

/* Each frame before the layer is drawn: the area under the player's feet (s->focus, within 1.5 of their height) made walkable,
 * and the layer's colours brought up to the rule */
static void walk_scene(const GfxScene* s)
{
    if (!wk.ne || !g.vb)
        return;
    const int on = g_gfx_refl_walkable != 0;
    int changed = on != wk.shown;
    if (on && s->focus[3] != 0.0f)
    {
        const uint32_t key = walk_key(s->focus[0], s->focus[2]);
        uint32_t best = UINT32_MAX;
        float bd = 1.5f;
        for (uint32_t i = walk_find(key); i < wk.ne && wk.e[i].key == key; ++i)
            if (fabsf(wk.e[i].y - s->focus[1]) < bd)
                bd = fabsf(wk.e[i].y - s->focus[1]), best = wk.area[wk.e[i].tri];
        if (best != UINT32_MAX && !wk.walk[best])
            wk.walk[best] = 1, wk.nwalk++, changed = 1;
    }
    if (changed)
        walk_upload(on);
}

void floors_set_zone(int zone)
{
    if (coll.zone && zone != coll.zone)
        collision_free(), coll.zone = 0; /* the last zone's collision isn't drawn in this one; its own is handed over once read */
    if (zone == g.zone)
        return;
    floors_free();
    g.zone = zone;
}

void floors_device_lost(void)
{
    targets_free();
    floors_free();
    g.zone = 0; /* the zone's floors are handed over again, as for one not seen yet */
    collision_free();
    coll.zone = 0; /* ... and its collision */
}

void floors_set(const GrassFloors* f)
{
    floors_free();
    g.zone = f ? f->zone : 0;
    if (!f || !f->zone)
        return;
    g.none = 1; /* until they're laid */
    const uint64_t total = floors_placed(f); /* (0: anything out of place, and the zone has none) */
    if (!total)
        return;
    if (total > FLOORS_MOST)
    {
        rt_log("[recomp] floors: zone %d's floors left out of the reflections' layer: %llu triangles placed, more than %u\n", f->zone,
            (unsigned long long)total, FLOORS_MOST);
        return;
    }
    FVert* v = (FVert*)malloc((size_t)total * 3 * sizeof(FVert));
    if (!v)
        return;
    uint32_t n = 0;
    double area[15] = { 0 }; /* the floors' area by kind, seen from above */
    double stone_level = 0.0; /* ... and the stone's that lies level (within 14 degrees) */
    for (uint32_t k = 0; k < f->npairs; ++k)
    {
        const uint32_t mesh = f->pairs[2 * k + 1];
        const float* m = &f->matrices[f->pairs[2 * k] * 12];
        for (uint32_t t = f->mesh_first[mesh]; t < f->mesh_first[mesh + 1]; ++t)
        {
            const uint32_t kind = f->terrain[t] > 14 ? 14u : f->terrain[t];
            const uint32_t col = 0xFF000000u | ((kind + 1u) * 16u) << 16;
            float w[3][3], nv[3];
            tri_placed(f, m, t, w, nv);
            /* not the steep ones (over 60 degrees from level, by the placed triangle, either way up): the collision's sides that
             * aren't walls, the invisible sheet along a staircase's open side, a post's faces, stand over the ground beside them
             * in the layer, which then lies nearer than the picture there, and that ground took no reflection, a pale gap down
             * the stairs' side. The reflections only take ground that faces up */
            const float nx = nv[0], ny = nv[1], nz = nv[2];
            if (4.0f * ny * ny < nx * nx + ny * ny + nz * nz)
                continue;
            area[kind] += 0.5 * fabs((double)ny);
            if (kind == 5 && ny * ny >= 0.9409f * (nx * nx + ny * ny + nz * nz)) /* stone (5), cos 14 degrees squared */
                stone_level += 0.5 * fabs((double)ny);
            tri_verts(&v[n], &w[0][0], col);
            n += 3;
        }
    }
    /* A zone whose land is largely snow is under snow, and its stone is the rock showing through it (Qufim Island's collision
     * is half snow and half stone, Beaucedine Glacier's and Xarcabard's four fifths snow, drawn as snow and rock alike), not a
     * street that's wet: as stone it took the wet floor's mirror in patches across the snowfield, grey sheets with straight
     * edges where the collision changes kind. There its stone is laid as snow. No town has any snow (none of the zones
     * looked at that aren't snowfields has a triangle of it), so their stone keeps its reflections */
    double land = 0.0;
    for (int k = 0; k < 15; ++k)
        land += k == 8 || k == 9 ? 0.0 : area[k]; /* (not the water) */
    const int snowy = land > 0.0 && area[4] >= 0.1 * land;
    /* Rock is tagged stone too: a field's bare hills (South Gustaberg's collision is two fifths stone, laid in patches with
     * straight edges among dirt it looks the same as) took the wet floor's mirror, shiny grey sheets on dirt and rock. Paved
     * stone lies level: of 16 towns and 2 dungeons, 60 to 96 percent of the stone's area lies within 14 degrees of level; of 11
     * fields (Qufim's snow aside) and 3 mines and caves, 6 to 44. A zone whose stone is under half level has its stone laid
     * as path (1), dirt, which the reflections never keep to */
    const int rough = !snowy && area[5] > 0.0 && stone_level < 0.5 * area[5];
    if (snowy || rough)
        for (uint32_t i = 0; i < n; ++i)
            if (v[i].col == (0xFF000000u | (6u * 16u) << 16))             /* stone (5) */
                v[i].col = 0xFF000000u | ((snowy ? 5u : 2u) * 16u) << 16; /* ... as snow (4) or path (1) */
    g.bytes = n * (uint32_t)sizeof(FVert);
    g.vb = gfx_buf_create(g.bytes);
    if (g.vb)
    {
        gfx_buf_upload(g.vb, v, g.bytes);
        g.ntris = n / 3, g.none = 0;
        rt_log("[recomp] floors: zone %d's floors laid out for the reflections' layer: %u triangles (%u KB)%s\n", f->zone, g.ntris,
            g.bytes >> 10,
            snowy ? ", its stone as snow (the land is largely snow)"
                  : rough ? ", its stone as path (under half of it lies level: rock, not paving)" : "");
        /* ... and joined into the areas one can walk between (refl_walkable: walk_scene); kept, v with them */
        if (!walk_areas(v, n))
            walk_free(); /* (v with it: every floor reflects) */
        return;
    }
    g.bytes = 0;
    free(v);
}

void floors_set_collision(const GrassFloors* f)
{
    collision_free();
    coll.zone = f ? f->zone : 0;
    if (!f || !f->zone)
        return;
    const uint64_t total = floors_placed(f); /* (checked as floors_set checks the floors) */
    if (!total)
        return;
    if (total > COLL_MOST)
    {
        rt_log("[recomp] floors: zone %d's collision not shown: %llu triangles placed, more than %u\n", f->zone,
            (unsigned long long)total, COLL_MOST);
        return;
    }
    FVert* v = (FVert*)malloc((size_t)total * 3 * sizeof(FVert));
    if (!v)
        return;
    /* each triangle shaded by how squarely it faces a light from above and to one side (either way up: a tenth of the floors
     * face down as stored), so walls facing different ways, and a slope against the level ground, read apart */
    static const float L[3] = { 0.36f, -0.80f, 0.48f }; /* (y counts down) */
    uint32_t n = 0, walls = 0;
    for (uint32_t k = 0; k < f->npairs; ++k)
    {
        const uint32_t mesh = f->pairs[2 * k + 1];
        const float* m = &f->matrices[f->pairs[2 * k] * 12];
        for (uint32_t t = f->mesh_first[mesh]; t < f->mesh_first[mesh + 1]; ++t)
        {
            const int wall = (f->terrain[t] & COLL_WALL) != 0;
            const uint32_t kind = f->terrain[t] & 0x0Fu;
            const uint32_t rgb = wall ? COLL_WALL_RGB : COLL_RGB[kind > 10 ? 10 : kind];
            float w[3][3], nv[3];
            tri_placed(f, m, t, w, nv);
            const float nx = nv[0], ny = nv[1], nz = nv[2];
            const float len = sqrtf(nx * nx + ny * ny + nz * nz);
            const float shade = 0.55f + 0.45f * (len > 0.0f ? fabsf(nx * L[0] + ny * L[1] + nz * L[2]) / len : 0.0f);
            uint32_t col = (wall ? COLL_WALL_A : COLL_FLOOR_A) << 24;
            for (int s = 0; s < 24; s += 8)
                col |= (uint32_t)((float)(rgb >> s & 0xFFu) * shade + 0.5f) << s;
            tri_verts(&v[n], &w[0][0], col);
            n += 3;
            walls += (uint32_t)wall;
        }
    }
    coll.bytes = n * (uint32_t)sizeof(FVert);
    coll.vb = gfx_buf_create(coll.bytes);
    if (coll.vb)
    {
        gfx_buf_upload(coll.vb, v, coll.bytes);
        coll.ntris = n / 3;
        rt_log("[recomp] floors: zone %d's collision laid out for the collision view: %u triangles, %u of them walls (%u KB)\n",
            f->zone, coll.ntris, walls, coll.bytes >> 10);
    }
    else
        coll.bytes = 0;
    /* the ray-traced reflections (refl_rt): a tree of these triangles, kept by the renderer; the colour word is left out by the stride */
    if (n && (gfx_scene_floors_wanted() & 2))
    {
        uint32_t bytes = 0, node_bytes = 0;
        uint8_t* tree = rtbvh_build((const float*)v, (uint32_t)(sizeof(FVert) / sizeof(float)), n / 3, &bytes, &node_bytes);
        const int built = tree != NULL;
        if (built)
            gfx_scene_rt_geometry(tree, bytes, node_bytes, 0, 0, NULL, 0), free(tree);
        rt_log("[recomp] floors: zone %d's collision as a tree for the ray-traced reflections: %u triangles, %u KB%s\n", f->zone, n / 3,
            bytes >> 10, built ? "" : " (not built)");
    }
    free(v);
}

/* a draw of FVerts as triangles (floors_draw, collision_draw): the fixed function, the vertex colour as it is, both sides; the
 * scene's viewport and DLSS's jitter, the camera (view) */
static void fvert_draw(GfxDraw* d, const GfxScene* s, const float* view)
{
    memset(d, 0, sizeof *d);
    d->vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d->vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d->u.offset[GFX_R_POSITION] = (int32_t)offsetof(FVert, x), d->u.offset[GFX_R_DIFFUSE] = (int32_t)offsetof(FVert, col);
    d->u.stride[0] = (int32_t)sizeof(FVert);
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){ 2 /* SELECTARG1 */, 0 /* DIFFUSE */, 0, 1, 2, 0, 0, 1, 1, 0, 0, 2 };
    for (int i = 0; i < 4; ++i)
        d->u.mat_d[i] = d->u.mat_a[i] = d->u.ambient[i] = d->u.tfactor[i] = 1.0f;
    float jit[2];
    if (gfx_dlss_jitter(jit))
        memcpy(&d->u.pad0[0], &jit[0], 4), memcpy(&d->u.pad0[1], &jit[1], 4);
    float vpm[16];
    mat_mul(vpm, view, s->proj);
    memcpy(d->u.wvp, vpm, 64);
    memcpy(d->u.wv, view, 64);
    for (int i = 0; i < 4; ++i)
        d->u.vp[i] = (float)s->vp[i];
    memcpy(d->vp, s->vp, sizeof d->vp);
    d->cull = 1; /* NONE: a tenth of the floors face down as stored */
    d->fill = 3; /* SOLID */
    d->prim = GFX_TRIANGLELIST;
}

/* the floors: into red, with the scene's camera, depth-tested among themselves, no blending, no fog */
static void floors_draw(const GfxScene* s)
{
    static GfxDraw d;
    fvert_draw(&d, s, s->view);
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4; /* LESSEQUAL */
    d.pipe.write_mask = 0x1; /* red */
    d.count = g.ntris;
    d.buf[0] = g.vb, d.buf_off[0] = 0, d.size[0] = g.bytes;
    gfx_draw(&d);
}

/* the collision view: over the scene's picture by each vertex's alpha, against the scene's depth (not writing it), both sides */
static void collision_draw(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    /* the camera's space scaled by COLL_NEAR about the eye: each point lands on the same pixel, a little nearer (0.4% of its
     * distance), so collision lying on the drawn ground (to the hundredth) is over it, not in and out of it in stripes */
    float view[16];
    memcpy(view, s->view, 64);
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 3; ++k)
            view[r * 4 + k] *= COLL_NEAR;
    static GfxDraw d;
    fvert_draw(&d, s, view);
    /* into the scene's fog as the world goes: the collision runs on past where the game stops drawing, and there it stood
     * against the sky in bright slivers */
    if (s->fog[2] != 0.0f)
        d.fs.fog = 3 /* LINEAR */, d.u.params[2] = s->fog[0], d.u.params[3] = s->fog[1];
    memcpy(d.u.fogcolor, s->fogcolor, sizeof d.u.fogcolor);
    d.depth.zenable = 1, d.depth.zwrite = 0, d.depth.zfunc = 4; /* LESSEQUAL */
    d.pipe.blend = 1, d.pipe.src = 5 /* SRCALPHA */, d.pipe.dst = 6 /* INVSRCALPHA */, d.pipe.op = 1 /* ADD */;
    d.pipe.write_mask = 0x7; /* the colour, the picture's alpha left as it is */
    d.count = coll.ntris;
    d.buf[0] = coll.vb, d.buf_off[0] = 0, d.size[0] = coll.bytes;
    gfx_set_targets(color, 0, 0, depth);
    gfx_draw(&d);
}

/* the reflections' layer of the floors (floors_scene's first part) */
static void layer_scene(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    const int layer = (gfx_scene_floors_wanted() & 1) && g.vb && g.ntris;
    /* why the floors aren't drawn: 2 the zone has none (any ground reflects), 0 not read yet (the reflections wait) */
    const int state = layer ? 1 : g.zone && g.none ? 2 : 0;
    if (!layer)
    {
        if (g.id || g.depth)
            targets_free();
        gfx_scene_floors(NULL, NULL, state);
        return;
    }
    const uint32_t w = s->vp[0] + s->vp[2], h = s->vp[1] + s->vp[3];
    if (!w || !h || w > 16384 || h > 16384)
    {
        gfx_scene_floors(NULL, NULL, 0);
        return;
    }
    if (!g.id || !g.depth || g.w != w || g.h != h)
    {
        targets_free();
        g.id = gfx_tex_create(GFX_TEX_2D, 21 /* D3DFMT_A8R8G8B8 */, w, h, 1, GFX_USE_RT);
        g.depth = gfx_tex_create(GFX_TEX_2D, 75 /* D3DFMT_D24S8 */, w, h, 1, GFX_USE_DEPTH);
        g.w = w, g.h = h;
        if (!g.id || !g.depth)
        {
            targets_free();
            gfx_scene_floors(NULL, NULL, 0);
            return;
        }
    }
    walk_scene(s); /* (the walkable areas' colours: refl_walkable) */
    const uint32_t whole[6] = { 0, 0, w, h, 0, 0x3F800000u /* 1.0f */ };
    gfx_set_targets(g.id, 0, 0, g.depth);
    gfx_clear(0, NULL, 3 /* D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER */, 0, 1.0f, 0, whole);
    floors_draw(s);
    gfx_set_targets(color, 0, 0, depth);
    gfx_scene_floors(g.id, g.depth, state);
}

void floors_scene(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    layer_scene(color, depth, s);
    if (coll_shown && coll.vb && coll.ntris && color && depth)
        collision_draw(color, depth, s);
}
