/* d3d8_water_level.h: an include unit of d3d8_water.h (in its scope: g_water_set, g_scene). The zone's floors (g_wfloor) and the
 * sea (g_sea, SEA_*); the sea's level: which zones have a sea (sea_zone), its level (sea_level, sea_note) and the game's sea
 * sheets VanaCore's sea plane takes the place of (sea_own) */
#pragma once

static struct
{
    uint32_t n;         /* the collision's water triangles (0: none, or the zone's floors not known) */
    float lo[3], hi[3]; /* their box in the world */
    float zlo[3], zhi[3]; /* the zone's floors' box in the world, every terrain: the sea's tiles run past it (water_horizon_pass) */
    int zone;             /* whose floors (0: none) */
    float rim_y;          /* the lowest point of the water floors at the zone's rim (within SEA_RIM of the box's edge): the shore's
                           * level where the sea is painted on the sky's dome and has no tiles (Valkurm Dunes); rim_n corners */
    uint32_t rim_n;
} g_wfloor;
static uint32_t water_floors_zone(void) { return g_wfloor.zone; }

/* The sea, for water_horizon_pass: the lowest level of the game's water tiles that reach the zone's rim (sea_note) */
static struct
{
    float y;      /* its height (the world's y points down: the largest; a chunk's lowest point) */
    uint32_t n;   /* tile draws, or chunks, at it */
    uint32_t gen; /* changed as the level is set or first reaches SEA_LEAST: chunks without water are looked at again (water_ground) */
    int said;     /* the log has said */
    uint32_t full; /* the frame (g_scene.frame) VanaCore's sea plane was last drawn (water_horizon_pass): the game's tiles are dropped
                    * only while it is (sea_own), never with no sea at all */
    float box[4];  /* the x and z box (x0, z0, x1, z1) of the draw that set the level */
    float under;   /* the level of the sea's under-sheet (has_under: one), that same draw again lower down (sea_note) */
    int has_under;
} g_sea;
#define SEA_PLANES 2.0f /* the game's level water up to this far over the sea's level is the sea's too: Lower Jeuno lays its harbour in
                       * two or three sheets a little apart, which showed as steps and seams in the water */
#define SEA_RIM 60.0f   /* a tile within this of the zone's floors' box edge, or past it, is the sea's; a marsh's tiles lie well inside */
#define SEA_LEAST 4     /* tile draws, or chunks, at a level before the sea is drawn on */
#define SEA_STACK 8.0f  /* the sheet that set the sea's level drawn again up to this far over it is the sea's surface (sea_note) */
#define SEA_UNDER 0.15f /* the horizon's sheet this far under the tiles' level: where the tiles are, they win the depth test */
#define SEA_OVER 0.1f   /* ... and this far over the shore's level where the sea has no tiles: it lies over the wet sand's end, which it fades in across */
#define SEA_EDGE 120.0f /* the sheet fades in from nothing over this much from its inner edge (its first ring's depth 0 to SEA_DEPTH) */
#define SEA_DEPTH 30.0f /* the bed's depth the sheet's pixels take: the deep water's colour */
#define SEA_IN 100.0f   /* the sheet starts this far inside the floors' box: under the last tiles, chunks or shore, with no gap to them */
#define SEA_RIM_LEAST 3000u /* water corners at the rim before the shore's level counts as a sea's (Valkurm's has 10,500; a stream's end near a zone's edge a few hundred) */

/* A tile of the game's drawn as VanaCore's water (water_draw): the sea's level for water_horizon_pass. The lowest level (the
 * world's y points down) of the tiles that reach the zone's rim, within SEA_RIM of its floors' box or past it: the sea's tiles
 * run on past the ground the footsteps know, a marsh's lie well inside. Counted each draw: SEA_LEAST at a level before it's drawn on */
/* A zone with a sea: its collision's water floors reach the zone's rim (the sea bed runs on to the edge of the ground the footsteps
 * know; a pond's, a stream's lie well inside, and they're no sea however near the rim their water is) */
static int sea_zone(void)
{
    return g_wfloor.zone && g_wfloor.n && g_wfloor.zlo[0] <= g_wfloor.zhi[0] &&
        (g_wfloor.lo[0] < g_wfloor.zlo[0] + SEA_RIM || g_wfloor.hi[0] > g_wfloor.zhi[0] - SEA_RIM ||
         g_wfloor.lo[2] < g_wfloor.zlo[2] + SEA_RIM || g_wfloor.hi[2] > g_wfloor.zhi[2] - SEA_RIM);
}

/* The zone's dry floors (the collision's, not water) by height: how much of them lies under a level. A river's or a lake's tiles
 * can reach the zone's rim as the sea's do (La Theine Plateau's river runs off its edge), and taken for the sea the plane went
 * over the whole zone at their level, everything lower under water. The sea is the lowest thing in its zone: a level with more
 * than SEA_DRY_MOST of the dry floors more than SEA_DRY_UNDER under it is no sea's */
#define SEA_DRY_BINS 1024
#define SEA_DRY_UNDER 2.0f
#define SEA_DRY_MOST 0.08f
static struct
{
    float y0;                  /* the first bin's height (the zone's top: the world's y points down) */
    float step;                /* yalms a bin */
    float area[SEA_DRY_BINS];  /* the dry floors' area by their middles' height */
    float total;               /* all of it (0: not known; every level is let be the sea's) */
    float level;               /* the last level looked at, and its answer */
    int level_n, level_ok;
} g_dry;

static void sea_dry(const GrassFloors* f)
{
    memset(g_dry.area, 0, sizeof g_dry.area);
    g_dry.total = 0.0f, g_dry.level_n = 0;
    g_dry.y0 = g_wfloor.zlo[1];
    const float span = g_wfloor.zhi[1] - g_wfloor.zlo[1];
    g_dry.step = span > SEA_DRY_BINS ? span / SEA_DRY_BINS : 1.0f;
    if (!(span > 0.0f))
        return;
    for (uint32_t k = 0; k < f->npairs; ++k)
    {
        const uint32_t mi = f->pairs[2 * k], mesh = f->pairs[2 * k + 1];
        if (mi >= f->nmatrices || mesh >= f->nmesh)
            continue;
        const float* m = &f->matrices[mi * 12];
        for (uint32_t t = f->mesh_first[mesh]; t < f->mesh_first[mesh + 1] && t < f->ntris; ++t)
        {
            if (f->terrain[t] == 8 || f->terrain[t] == 9)
                continue;
            float o[3][3];
            for (int c = 0; c < 3; ++c)
            {
                const float* p = &f->tris[t * 9 + c * 3];
                for (int j = 0; j < 3; ++j)
                    o[c][j] = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
            }
            /* its area as seen from above (a wall's is none) */
            const float ar = 0.5f * fabsf((o[1][0] - o[0][0]) * (o[2][2] - o[0][2]) - (o[2][0] - o[0][0]) * (o[1][2] - o[0][2]));
            int b = (int)(((o[0][1] + o[1][1] + o[2][1]) / 3.0f - g_dry.y0) / g_dry.step);
            b = b < 0 ? 0 : b >= SEA_DRY_BINS ? SEA_DRY_BINS - 1 : b;
            g_dry.area[b] += ar, g_dry.total += ar;
        }
    }
}

/* 1: y can be the sea's (not much of the dry floors under it, or they aren't known) */
static int sea_dry_ok(float y)
{
    if (!(g_dry.total > 0.0f))
        return 1;
    if (g_dry.level_n && g_dry.level == y)
        return g_dry.level_ok;
    int b = (int)ceilf((y + SEA_DRY_UNDER - g_dry.y0) / g_dry.step);
    float under = 0.0f;
    for (b = b < 0 ? 0 : b; b < SEA_DRY_BINS; ++b)
        under += g_dry.area[b];
    g_dry.level = y, g_dry.level_n = 1, g_dry.level_ok = under <= g_dry.total * SEA_DRY_MOST;
    if (g_water_set.log && !g_dry.level_ok)
        rt_log("[recomp] water: the level %.1f is no sea's: %.0f%% of the zone's dry floors lie under it\n", y, 100.0f * under / g_dry.total);
    return g_dry.level_ok;
}

/* The sea's level for the sheet drawn on to the horizon: the tiles' or chunks' at the rim once SEA_LEAST are seen (sea_note),
 * else the shore's, where the zone has water floors at its rim and no sea of its own (Valkurm Dunes: the sea is painted on the
 * sky's dome, the beach's wet sand is shallow water to the footsteps). 0: none */
static int sea_level(float* y)
{
    if (g_sea.n >= SEA_LEAST)
        return *y = g_sea.y, sea_dry_ok(g_sea.y);
    if (sea_zone() && g_wfloor.rim_n >= SEA_RIM_LEAST)
        return *y = g_wfloor.rim_y, sea_dry_ok(g_wfloor.rim_y);
    return 0;
}

/* own: the game's own water (a tile found by its shape, a water model's level sheet), which is water wherever it lies: only the
 * floors' box is needed to tell it reaches the rim. The zone's collision needn't have water floors for it (Lower Jeuno's harbour:
 * no one wades in it, so its collision has none at the rim, and with sea_zone() asked of every tile the sea's level was never set
 * and water.horizon drew nothing). 0: a chunk of the ground's water (water.detect=2), a sea's only in a zone with a sea (a
 * stream's end near the rim is none) */
/* A ferry's zone (core/graphics.cpp ship_zone's: Ship bound for Selbina / Mhaura, their pirate versions, the open sea routes): its
 * collision isn't read and the whole zone moves past the boat, so there's no floors' box to tell the sea by. Its sea is the game's
 * level sheets round the boat, 274 and 658 yalms across (2026-11-21), and nothing past them: the water model's level sheets
 * SEA_SHIP_LEAST across or more are the sea's there, and VanaCore's sea plane goes on to the horizon from them */
#define SEA_SHIP_LEAST 200.0f
static int g_water_ship;
/* The zones VanaCore's water is never drawn in (d3d8_set_water_zones_off: data\water_zones.json's "water": false), the game's own
 * left as with water=0 (g_water_zone_off); the zone the player is in, -1 before the first */
#define WATER_OFF_MOST 1024
static uint16_t g_water_off[WATER_OFF_MOST];
static unsigned g_water_off_n;
static int g_water_zone_now = -1;
static void water_set_zone(int zone)
{
    g_water_zone_now = zone;
    g_water_ship = zone == 46 || zone == 47 || zone == 220 || zone == 221 || zone == 227 || zone == 228;
    g_water_zone_off = 0;
    for (unsigned i = 0; i < g_water_off_n && !g_water_zone_off; ++i)
        g_water_zone_off = g_water_off[i] == zone;
    g_water_mode = g_water_set.enabled && !g_water_zone_off ? 1 : 0;
}

void d3d8_set_water_zones_off(const uint16_t* zones, unsigned n)
{
    g_water_off_n = 0;
    for (unsigned i = 0; zones && i < n && g_water_off_n < WATER_OFF_MOST; ++i)
        g_water_off[g_water_off_n++] = zones[i];
    if (g_water_zone_now >= 0) /* the zone the player is in, again with the new list */
        water_set_zone(g_water_zone_now);
}
static void sea_note(const float lo[3], const float hi[3], int own)
{
    if (g_water_ship)
    {
        if (!own || (hi[0] - lo[0] < SEA_SHIP_LEAST && hi[2] - lo[2] < SEA_SHIP_LEAST))
            return;
    }
    else if (own ? !g_wfloor.zone || !(g_wfloor.zlo[0] <= g_wfloor.zhi[0]) : !sea_zone())
        return;
    else if (lo[0] > g_wfloor.zlo[0] + SEA_RIM && hi[0] < g_wfloor.zhi[0] - SEA_RIM && lo[2] > g_wfloor.zlo[2] + SEA_RIM &&
             hi[2] < g_wfloor.zhi[2] - SEA_RIM)
        return;
    const float y = hi[1]; /* the lowest point: a tile's one height; a sea chunk's wave troughs, which the horizon's sheet lies under */
    const float box[4] = { lo[0], lo[2], hi[0], hi[2] };
    /* The lowest level at the rim is the sea's. But the game may draw its sea's sheet twice, one over the other: Port Jeuno's is
     * one water model drawn at 15.0 and again at 19.9, its under-sheet, through a world matrix that scales its heights 200 times.
     * The sea was taken at 19.9, its sheets at 15 were no tiles of it (sea_own) and were made VanaCore's water too, a second sea 5
     * yalms over VanaCore's: where their mesh reached, clear and pale over nothing (the void under the docks read as a bed a hand
     * down), and VanaCore's deep teal beside it, a dark slab with straight edges by the Kazham airship's berth (a player's report,
     * 2026-11-23). So the sheet that set the level (its x and z box) drawn again more than SEA_PLANES (sheets nearer than that are
     * one sea: Lower Jeuno's harbour) and up to SEA_STACK over it is the sea's surface, and the level that was its under-sheet:
     * dropped with the sea's tiles (sea_own), never the level again. Another water higher up at the rim (a river's end) is another
     * box and leaves the level as it was */
    const int same = g_sea.n && fabsf(box[0] - g_sea.box[0]) < 2.0f && fabsf(box[1] - g_sea.box[1]) < 2.0f &&
                     fabsf(box[2] - g_sea.box[2]) < 2.0f && fabsf(box[3] - g_sea.box[3]) < 2.0f;
    if (g_sea.has_under && fabsf(y - g_sea.under) <= 1.0f)
        return; /* the sea's under-sheet */
    if (same && y < g_sea.y - SEA_PLANES && y >= g_sea.y - SEA_STACK)
    {
        if (g_water_set.log)
            rt_log("[recomp] water: the sea's surface at %.2f, over its under-sheet at %.2f (one sheet drawn twice)\n", y, g_sea.y);
        g_sea.under = g_sea.y, g_sea.has_under = 1;
        g_sea.y = y, g_sea.n = g_sea.n > SEA_LEAST ? g_sea.n : 1, g_sea.said = 0, ++g_sea.gen;
        return;
    }
    if (!g_sea.n || y > g_sea.y + 1.0f)
    {
        g_sea.y = y, g_sea.n = 1, g_sea.said = 0, ++g_sea.gen, g_sea.has_under = 0;
        memcpy(g_sea.box, box, sizeof box);
    }
    else if (fabsf(y - g_sea.y) <= 1.0f && g_sea.n < 1000000u)
    {
        if (++g_sea.n == SEA_LEAST)
            ++g_sea.gen;
    }
}

/* water.sea: a level surface of the game's own water (lo, hi its box) that is the sea's - at the sea's level (sea_level's tiles),
 * up to SEA_PLANES over it, or its under-sheet's (sea_note) - while VanaCore's sea plane is drawn (the last frame or this one):
 * dropped, the plane is the sea */
static int sea_own(const float lo[3], const float hi[3])
{
    if (!g_water_set.sea || !g_water_set.pass || g_sea.n < SEA_LEAST || g_scene.frame > g_sea.full + 1)
        return 0;
    (void)lo;
    /* (the world's y points down: hi[1] the lowest point) */
    return (hi[1] <= g_sea.y + 0.5f && hi[1] >= g_sea.y - SEA_PLANES) || (g_sea.has_under && fabsf(hi[1] - g_sea.under) <= 0.5f);
}
