/* d3d8_water_fill.h: an include unit of d3d8_water.h (in its scope: WaterBake, the pool g_bake_v, BAKE_*, g_sea, grass_floor_near).
 * The holes in a stream's sheet made water (bake_fill, from bake_build), and a pond flooded from the game's additive layer over
 * it, or from a sheet of its surface (bake_flood) */

/* The holes in a stream's sheet: the game leaves shelves of the bank under the level without water (its murky stream hid them);
 * through VanaCore's clear water they showed as dark slabs with the sheet's straight edges (West Ronfaure). Cells a yalm across
 * out to FILL_REACH from the sheet are made water at its level where the footsteps' floor is the deep water's surface or a tenth
 * or more under the level, appended to b's water. Not a sea's sheets, nor ones too big to grid */
#define FILL_REACH 8
#define FLOOD_REACH 96 /* cells a pond is flooded out to from its layer, at most */
#define FILL_POND_DOWN 16.0f /* how far from a pond's level its water's floors are looked for (fill_wet): no farther than its
                              * deepest bed needs, so a pool isn't flooded on over water lying a fall's height under its lip */
static const float FILL_PT[5][2] = { { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } }; /* a cell's middle, corners */
#define FILL_MOST 262144u /* cells, at most */
/* a cell: 0 open, 1 the sheet's, 2+k filled in pass k, and these */
#define FILL_SEED 252  /* under the layer a pond is flooded from, not looked at yet */
#define FILL_NOT 253   /* not the pond's water (the flood looked): open to the shelves' passes */
#define FILL_TAKEN 254 /* another layer's flood's water already */
#define FILL_DRY 255   /* looked at and dry */
typedef struct FillGrid
{
    uint8_t* m;
    float* dp;       /* each made cell's depth */
    float x0, z0, y; /* its first cell's corner in the world, and the level */
    uint32_t nx, nz;
    float tag; /* added to its cells' depths: BAKE_FILL_TAG for a sheet's holes (bake_fill), 0 for a flooded pond's */
    int pond;  /* a pond's surface flooded (bake_flood): its sheet's cells water whatever the collision has under them, the pond's
                * out from them where its water floors are at the level or under it, every cell BAKE_DEEP deep */
} FillGrid;

/* The ponds' cells flooded this zone (bake_flood), so that four layers round one pond flood it once: x, z (a cell's corner, whole
 * yalms) and the level in half yalms; open addressing, forgotten at the zone's floors' hand-over. A pond of Caedarva Mire's is
 * thousands of cells */
#define FLOOD_SET 131072u
typedef struct FloodCell
{
    int16_t x, z, y2, used;
} FloodCell;
static FloodCell g_flood[FLOOD_SET];
static uint32_t g_flood_n;
/* Where a flood stopped in open water, to be carried on from (flood_pend, flood_pump): a cell's middle, the level, and the
 * flood's kind (bake_flood's pond and own) */
#define FLOOD_PEND 32
#define FLOOD_PEND_APART 48.0f /* yalms between two of them at a level: the floods from each reach well past that */
#define FLOOD_PUMP_EVERY 4     /* frames between two floods carried on: a lake fills in over a second, without a hitch */
#define FLOOD_ROOM 24000u      /* cells g_flood must still have room for (a flood's most) before one is carried on */
typedef struct FloodPend
{
    float x, z, y;
    uint8_t pond, own;
} FloodPend;
static FloodPend g_flood_pend[FLOOD_PEND];
static uint32_t g_flood_pend_n;
static void flood_reset(void)
{
    memset(g_flood, 0, sizeof g_flood), g_flood_n = 0, g_flood_pend_n = 0;
}
static FloodCell* flood_at(int x, int z, int y2, int add)
{
    uint32_t h = ((uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u) & (FLOOD_SET - 1);
    for (uint32_t k = 0; k < FLOOD_SET && g_flood[h].used; ++k, h = (h + 1) & (FLOOD_SET - 1))
        if (g_flood[h].x == x && g_flood[h].z == z && abs(g_flood[h].y2 - y2) <= 2)
            return &g_flood[h];
    if (!add || g_flood_n >= FLOOD_SET / 4 * 3)
        return NULL;
    FloodCell* c = &g_flood[h];
    c->x = (int16_t)x, c->z = (int16_t)z, c->y2 = (int16_t)y2, c->used = 1, ++g_flood_n;
    return c;
}

/* a sheet's box (lo, hi) lying mostly in a pond flooded at its level: three of five points across it (its middle and four
 * between it and its corners) */
static int bake_flooded(const float lo[3], const float hi[3])
{
    static const float at[5][2] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
    int n = 0;
    for (int k = 0; k < 5; ++k)
        n += flood_at((int)floorf(lo[0] + (hi[0] - lo[0]) * at[k][0]), (int)floorf(lo[2] + (hi[2] - lo[2]) * at[k][1]),
                 (int)lrintf(lo[1] + hi[1]), 0) != NULL;
    return n >= 3;
}

/* the grid round b's water out to reach cells, at level y (0: too big) */
static int fill_grid(const WaterBake* b, float y, uint32_t reach, FillGrid* g)
{
    g->x0 = floorf(b->lo[0]) - (float)reach, g->z0 = floorf(b->lo[2]) - (float)reach, g->y = y, g->tag = 0.0f, g->pond = 0;
    g->nx = (uint32_t)(b->hi[0] - g->x0) + reach + 2, g->nz = (uint32_t)(b->hi[2] - g->z0) + reach + 2;
    if ((uint64_t)g->nx * g->nz > FILL_MOST)
        return 0;
    g->m = (uint8_t*)calloc((size_t)g->nx * g->nz, 1);
    g->dp = (float*)calloc((size_t)g->nx * g->nz, sizeof(float));
    if (g->m && g->dp)
        return 1;
    free(g->m), free(g->dp);
    return 0;
}

/* the points of each cell (FILL_PT's bits) b's triangles cover */
static void fill_cover(const WaterBake* b, FillGrid* g)
{
    for (uint32_t i = 0; i + 3 <= b->n; i += 3)
    {
        const float* a = &g_bake_v[(size_t)(b->at + i) * BAKE_STRIDE];
        const float* c1 = a + BAKE_STRIDE;
        const float* c2 = c1 + BAKE_STRIDE;
        const int ix0 = (int)floorf(fminf(a[0], fminf(c1[0], c2[0])) - g->x0), ix1 = (int)ceilf(fmaxf(a[0], fmaxf(c1[0], c2[0])) - g->x0);
        const int iz0 = (int)floorf(fminf(a[2], fminf(c1[2], c2[2])) - g->z0), iz1 = (int)ceilf(fmaxf(a[2], fmaxf(c1[2], c2[2])) - g->z0);
        for (int iz = iz0 < 0 ? 0 : iz0; iz <= iz1 && iz < (int)g->nz; ++iz)
            for (int ix = ix0 < 0 ? 0 : ix0; ix <= ix1 && ix < (int)g->nx; ++ix)
                for (int k = 0; k < 5; ++k)
                {
                    const float x = g->x0 + (float)ix + FILL_PT[k][0], z = g->z0 + (float)iz + FILL_PT[k][1];
                    const float d0 = (c1[0] - a[0]) * (z - a[2]) - (c1[2] - a[2]) * (x - a[0]);
                    const float d1 = (c2[0] - c1[0]) * (z - c1[2]) - (c2[2] - c1[2]) * (x - c1[0]);
                    const float d2 = (a[0] - c2[0]) * (z - c2[2]) - (a[2] - c2[2]) * (x - c2[0]);
                    if ((d0 >= 0.0f && d1 >= 0.0f && d2 >= 0.0f) || (d0 <= 0.0f && d1 <= 0.0f && d2 <= 0.0f))
                        g->m[(size_t)iz * g->nx + (size_t)ix] |= (uint8_t)(1u << k);
                }
    }
}

/* the cell's water at the level: its depth (the deepest of its wet points', at least BAKE_SHALLOW), or 0 for none. Water where any
 * of its corners or its middle is (the parts over a bank higher than the level are hidden by it). A pond's surface's: its water
 * floors at the level or under it, BAKE_DEEP (one depth: the collision's, cell by cell, made squares of lighter and darker water
 * over the painted pond). Strict, the pond the collision has: its deep water's floor at the level or under it, its shallow
 * water's bed under it. Else the deep water's surface at the level, or any floor a tenth or more under it (the bank's shelves) */
static float fill_wet(const FillGrid* g, uint32_t ix, uint32_t iz, int strict)
{
    int wet = 0;
    float deep = 0.0f;
    for (int k = 0; k < 5; ++k)
    {
        float p[3] = { g->x0 + (float)ix + FILL_PT[k][0], g->y, g->z0 + (float)iz + FILL_PT[k][1] }, d;
        /* (a pond's floors looked for deeper: Lake Mechieume's bed lies 13 yalms down in places, past the 12 the rest ask
         * within, and its deepest water was square holes in the lake) */
        const int tk = grass_floor_near(p, g->pond ? FILL_POND_DOWN : 12.0f, &d);
        const int w = g->pond  ? (tk == 8 || tk == 9) && d >= -BAKE_LEVEL
                      : strict ? (tk == 9 && d >= -1.0f) || (tk == 8 && d >= 0.1f)
                               : (tk == 9 && fabsf(d) <= 1.0f) || (tk >= 0 && tk != 9 && d >= 0.1f);
        if (w)
            ++wet, deep = fmaxf(deep, tk == 9 ? fmaxf(d, 1.0f) : d);
    }
    return !wet ? 0.0f : g->pond ? BAKE_DEEP : fmaxf(deep, BAKE_SHALLOW);
}

/* cells ix to ix + len - 1 of row iz made water at the level, two triangles appended to b's (0: the pool is full) */
static int fill_emit(WaterBake* b, const FillGrid* g, uint32_t ix, uint32_t iz, uint32_t len, float dp)
{
    if (g_bake_nv + 6 > g_bake_cap)
    {
        const uint32_t cap = g_bake_cap ? g_bake_cap * 2 : 3072;
        float* v = cap * BAKE_STRIDE <= BAKE_MOST ? (float*)realloc(g_bake_v, (size_t)cap * BAKE_STRIDE * sizeof(float)) : NULL;
        if (!v)
            return 0;
        g_bake_v = v, g_bake_cap = cap;
    }
    const float q[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    for (int k = 0; k < 6; ++k)
    {
        float* w = &g_bake_v[(size_t)g_bake_nv * BAKE_STRIDE];
        w[0] = g->x0 + (float)ix + q[k][0] * (float)len, w[1] = g->y - BAKE_LIFT, w[2] = g->z0 + (float)iz + q[k][1], w[3] = dp + g->tag;
        ++g_bake_nv, ++b->n;
    }
    return 1;
}

/* The made cells (2 up to FILL_SEED) into b's water: a run of them along a row as deep as its first (within 5 cm) one quad, so a
 * pond's thousands of cells are a few hundred quads. sign -1: the depths negated (the scene's depth under them is the painted
 * pond's own, not its bed's: gfx_hlsl_water.h takes the vertex's). 0: the pool is full */
static int fill_runs(WaterBake* b, const FillGrid* g, float sign)
{
    for (uint32_t iz = 0; iz < g->nz; ++iz)
        for (uint32_t ix = 0; ix < g->nx;)
        {
            const size_t i = (size_t)iz * g->nx + ix;
            if (g->m[i] < 2 || g->m[i] >= FILL_SEED)
            {
                ++ix;
                continue;
            }
            uint32_t len = 1;
            while (ix + len < g->nx && g->m[i + len] >= 2 && g->m[i + len] < FILL_SEED && fabsf(g->dp[i + len] - g->dp[i]) < 0.05f)
                ++len;
            if (!fill_emit(b, g, ix, iz, len, sign * g->dp[i]))
                return 0;
            ix += len;
        }
    return 1;
}

/* One pass over the grid: an open cell next to water (the sheet, a cell filled in an earlier pass, another flood's) - or,
 * strict, a seed - wet by the rule is made water (2 + pass), its depth kept for fill_runs; a pond's surface's seed is water
 * whatever the collision has under it (the sheet is the pond). Strict, one that isn't is FILL_NOT (the shelves' passes look
 * again); else FILL_DRY. How many were made */
static uint32_t fill_pass(FillGrid* g, int pass, int strict)
{
    uint32_t made = 0;
    for (uint32_t iz = 1; iz + 1 < g->nz; ++iz)
        for (uint32_t ix = 1; ix + 1 < g->nx; ++ix)
        {
            const size_t i = (size_t)iz * g->nx + ix;
            uint8_t* c = &g->m[i];
            if (*c != 0 && *c != FILL_SEED && (strict || *c != FILL_NOT))
                continue;
            const uint8_t n4[4] = { c[-1], c[1], c[-(ptrdiff_t)g->nx], c[g->nx] };
            int next = strict && *c == FILL_SEED;
            for (int k = 0; k < 4; ++k)
                next |= (n4[k] >= 1 && n4[k] < 2 + pass) || n4[k] == FILL_TAKEN;
            if (!next)
                continue;
            const float dp = g->pond && *c == FILL_SEED ? BAKE_DEEP : fill_wet(g, ix, iz, strict);
            if (dp <= 0.0f)
            {
                *c = strict ? FILL_NOT : FILL_DRY;
                continue;
            }
            g->dp[i] = dp, *c = (uint8_t)(2 + pass), ++made;
        }
    return made;
}

static void bake_fill(WaterBake* b, float y)
{
    FillGrid g;
    if (!b->n || (g_sea.n >= SEA_LEAST && sea_zone() && fabsf(y - g_sea.y) <= 1.5f) || !fill_grid(b, y, FILL_REACH, &g))
        return;
    /* its cells fade out where the ground drawn under them lies at the level (gfx_hlsl_water.h): the collision is no picture of
     * the ground there, and over Caedarva Mire's pond, painted at its level, they were squares of foam (the depth under them 0) */
    g.tag = BAKE_FILL_TAG;
    /* the cells the sheet covers whole (corners and middle; one covered in part is filled under its edge: no seam) */
    fill_cover(b, &g);
    for (size_t i = 0; i < (size_t)g.nx * g.nz; ++i) /* covered whole: the sheet's (1); in part or not at all: open (0) */
        g.m[i] = g.m[i] == 31 ? 1 : 0;
    uint32_t filled = 0;
    for (int pass = 0; pass < FILL_REACH; ++pass)
        filled += fill_pass(&g, pass, 0);
    fill_runs(b, &g, 1.0f);
    free(g.m), free(g.dp);
    if (g_water_set.log && filled)
        rt_log("[recomp] water: the sheet at %.1f (x %.0f..%.0f z %.0f..%.0f): %u cells of its holes and banks' shelves under its "
               "level made water\n", y, b->lo[0], b->hi[0], b->lo[2], b->hi[2], filled);
}

/* b's box made its cells' (the flood's water), and the cells noted as flooded (flood_at) */
static void flood_note(WaterBake* b, const FillGrid* g)
{
    float lo[3] = { 1e30f, g->y, 1e30f }, hi[3] = { -1e30f, g->y, -1e30f };
    for (uint32_t iz = 0; iz < g->nz; ++iz)
        for (uint32_t ix = 0; ix < g->nx; ++ix)
        {
            const uint8_t c = g->m[(size_t)iz * g->nx + ix];
            if (c < 2 || c >= FILL_SEED)
                continue;
            const float x = g->x0 + (float)ix, z = g->z0 + (float)iz;
            lo[0] = fminf(lo[0], x), hi[0] = fmaxf(hi[0], x + 1.0f), lo[2] = fminf(lo[2], z), hi[2] = fmaxf(hi[2], z + 1.0f);
            flood_at((int)x, (int)z, (int)lrintf(g->y * 2.0f), 1);
        }
    if (b->n)
        memcpy(b->lo, lo, sizeof lo), memcpy(b->hi, hi, sizeof hi);
}

/* A flood that stopped in open water leaves where to carry on from. bake_flood goes FLOOD_REACH cells out from its sheet, a
 * pass a cell, four ways: on a lake wider than that (Jugner Forest's Lake Mechieume, whose one sheet lies at the river's mouth)
 * it stopped across the open lake along a staircase of yalm cells, a diamond's edge, VanaCore's water on one side and the
 * game's on the other, and nothing took it on. The cells of its last pass (when it stopped with cells still being made: cut)
 * and the ones at its grid's rim are such places: kept, FLOOD_PEND_APART apart, for flood_pump */
static void flood_pend(const FillGrid* g, int cut, int pond, int own)
{
    uint8_t last = 0;
    for (size_t i = 0; cut && i < (size_t)g->nx * g->nz; ++i)
        if (g->m[i] >= 2 && g->m[i] < FILL_SEED && g->m[i] > last)
            last = g->m[i];
    for (uint32_t iz = 1; iz + 1 < g->nz; ++iz)
        for (uint32_t ix = 1; ix + 1 < g->nx; ++ix)
        {
            const uint8_t c = g->m[(size_t)iz * g->nx + ix];
            const int rim = ix <= 1 || iz <= 1 || ix + 2 >= g->nx || iz + 2 >= g->nz;
            if (c < 2 || c >= FILL_SEED || !(rim || (cut && c == last)))
                continue;
            const float x = g->x0 + (float)ix + 0.5f, z = g->z0 + (float)iz + 0.5f;
            int near = 0;
            for (uint32_t k = 0; k < g_flood_pend_n && !near; ++k)
                near = fabsf(g_flood_pend[k].y - g->y) <= 1.0f && fabsf(g_flood_pend[k].x - x) < FLOOD_PEND_APART &&
                       fabsf(g_flood_pend[k].z - z) < FLOOD_PEND_APART;
            if (!near && g_flood_pend_n < FLOOD_PEND)
                g_flood_pend[g_flood_pend_n++] = (FloodPend){ x, z, g->y, (uint8_t)(pond != 0), (uint8_t)(own != 0) };
        }
}

/* A pond flooded from the game's additive layer over it (b: the layer's triangles, at level y). East Sarutabaruta's pond is
 * painted on its bed, its water only that and a lit additive mesh laid four times along its banks: no sheet to make VanaCore's
 * water from. The cells under the layer and out from them up to FLOOD_REACH where the collision has the pond's water at the
 * layer's level (strict: the deep water's surface, the shallow water's bed under it) are made water, then the banks' shelves
 * round them as bake_fill makes them; the layer's own triangles are let go (light over the water, not its shape). Cells another
 * layer's flood made are left: round one pond, the first layer floods it all, the other three nothing.
 *   pond: b is a sheet of a pond's surface (on the deep water's own floor). Caedarva Mire's pond is painted at its level, its
 * sheets tiles among the ground's own: the sheets alone were squares of VanaCore's water in the game's murk. Flooded so from its
 * sheet, one depth throughout (fill_wet), negated when own (the sheet writes depth: the scene's depth under the water is the
 * painted pond's, not its bed's) */
static void bake_flood(WaterBake* b, float y, int pond, int own)
{
    FillGrid g;
    /* (not at a sea's level: VanaCore's sea plane is its water) */
    const int ok = !(g_sea.n >= SEA_LEAST && sea_zone() && fabsf(y - g_sea.y) <= 1.5f) && fill_grid(b, y, FLOOD_REACH + FILL_REACH, &g);
    if (ok)
        fill_cover(b, &g), g.pond = pond;
    g_bake_nv = b->at, b->n = 0; /* the layer's triangles (the last in the pool) let go; the flood's cells take their place */
    if (!ok)
        return;
    const int y2 = (int)lrintf(y * 2.0f);
    for (uint32_t iz = 0; iz < g.nz; ++iz)
        for (uint32_t ix = 0; ix < g.nx; ++ix)
        {
            uint8_t* c = &g.m[(size_t)iz * g.nx + ix];
            *c = flood_at((int)(g.x0 + (float)ix), (int)(g.z0 + (float)iz), y2, 0) ? FILL_TAKEN : *c ? FILL_SEED : 0;
        }
    uint32_t cells = 0, shelf = 0, n = 0;
    int pass = 0;
    for (; pass < FLOOD_REACH && (n = fill_pass(&g, pass, 1)) != 0; ++pass)
        cells += n;
    const int cut = pass == FLOOD_REACH && n != 0; /* stopped by the reach with cells still being made: in open water */
    for (int k = 0; k < FILL_REACH; ++k, ++pass)
        shelf += fill_pass(&g, pass, 0);
    fill_runs(b, &g, own ? -1.0f : 1.0f);
    flood_note(b, &g);
    flood_pend(&g, cut, pond, own);
    free(g.m), free(g.dp);
    if (g_water_set.log)
        rt_log("[recomp] water: the %s at %.2f (drawn at %.1f %.1f %.1f) flooded %u cells of its pond and %u of its banks' shelves "
               "(%u quads), x %.0f..%.0f z %.0f..%.0f\n", pond ? "pond's sheet" : "additive layer", y, b->t[0], b->t[1], b->t[2], cells,
            shelf, b->n / 6, b->lo[0], b->hi[0], b->lo[2], b->hi[2]);
}

/* After a pond's flood (b, at level y): the other sheets' water lying mostly in it at its level, taken before the flood reached
 * them, let go; the flood's is the pond's (two layers of water at one height drew each other's patches) */
static void bake_pond_over(const WaterBake* b, float y)
{
    for (uint32_t i = 0; i < g_bake_wet_n; ++i)
    {
        WaterBake* o = &g_bake[g_bake_wet[i]];
        if (o != b && o->n && !o->seed && fabsf((o->lo[1] + o->hi[1]) * 0.5f - y) < 0.1f && bake_flooded(o->lo, o->hi))
            o->n = 0;
    }
}


/* Once a frame, from water_ground_pass before the pool goes to the card: one flood that stopped in open water carried on
 * (flood_pend), every FLOOD_PUMP_EVERY frames. An entry of its own takes its cells (no mesh's: never found by bake_find), a
 * cell's box where the flood stopped; bake_flood lays its grid round that, takes the cells flooded before as water and goes
 * on from them another FLOOD_REACH out, by the same rule - and leaves its own places to carry on from, until the lake's banks
 * end it. Not past what g_flood has room to remember (a cell not remembered would be flooded again, water over water) */
static void flood_pump(void)
{
    static uint32_t frames;
    if (!g_flood_pend_n || (++frames % FLOOD_PUMP_EVERY) != 0)
        return;
    if (g_bake_n >= BAKE_ENTRIES || g_flood_n + FLOOD_ROOM > FLOOD_SET / 4 * 3)
    {
        g_flood_pend_n = 0;
        return;
    }
    const FloodPend p = g_flood_pend[--g_flood_pend_n];
    WaterBake* b = &g_bake[g_bake_n];
    memset(b, 0, sizeof *b);
    b->at = g_bake_nv, b->seed = 2, b->gen = g_sea.gen, b->seen = g_scene.frame;
    b->lo[0] = p.x - 0.5f, b->lo[1] = p.y, b->lo[2] = p.z - 0.5f, b->hi[0] = p.x + 0.5f, b->hi[1] = p.y, b->hi[2] = p.z + 0.5f;
    bake_flood(b, p.y, p.pond, p.own);
    if (!b->n) /* nothing more there (another flood got to it first): the entry isn't taken */
        return;
    ++g_bake_n;
    if (p.pond)
        bake_pond_over(b, p.y);
    bake_wet(b);
    g_bake_dirty = 1;
}
