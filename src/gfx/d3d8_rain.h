/* VanaCore's rain on the characters ([graphics] rain*, d3d8_set_rain): while the server's weather is rain or squall (0x057's 6
 * and 7, d3d8_set_weather), the players, NPCs and monsters in the world's scene are drawn wet - their lit pixel function lays a
 * wet look over the finished colour (gfx_hlsl_rain.h emit_wet, GfxFsKey.wet): darker and glossier, the sky in them, the drops
 * seen landing on what faces up as rings that spread and fade, runnels down their sides. The wetness rises over some 12 s as the
 * rain comes (the game fades its own rain in over about 20 s) and dries off over rain.dry seconds after it stops. The game's own
 * rain (dynamic streaks built round the camera each frame, additive) is left as it is.
 * d3d8.c includes this after character_light and night_light, before the draw is handed on; it uses g_scene, g_dev, gfx_now_ns and
 * the draw's keys. It is no file of its own. */
#pragma once

static RainSettings g_rain_set = { 1, 1.0f, 1.0f, 45.0f, 0, 1, 1.0f, 1.0f, 1, 1, 0, 0.5f, 1.0f, 1, 1, 0, 16.0f, 1.0f, 1.0f, 150.0f, 0, 1.0f };
/* the wet ground's mask for the floor reflections (rain_mask_draw): the scene's size, red the wet ground, green the puddles,
 * blue how open to the sky */
static GfxTex* g_rain_mask;
static uint32_t g_rain_mask_w, g_rain_mask_h;
static int g_rain_zone;             /* the zone the player is in (rain_set_zone from d3d8_set_grass_zone) */
static uint32_t g_rain_zone_weathers; /* d3d8_set_zone_weathers: the weathers its model DAT has (rain and squall only with
                                       * rain in them), bit n 0x057's n, bit 31 known; 0 not known */
static float g_rain_fall;           /* how much rain falls now, 0 to the weather's heaviness, eased over 6 s */
static unsigned g_rain_weather;     /* d3d8_set_weather: 0x057's id */
static int g_rain_weather_known;    /* ... once a packet has given one */
static float g_rain_wet;            /* how wet the characters are now, 0 dry to 1 */
static double g_rain_clock;         /* the drops' time, seconds, wrapped at 600 */
static uint64_t g_rain_last_ns;     /* when the wetness last moved on */
static uint32_t g_rain_frame;       /* ... in which frame (g_scene.frame): once a frame */
static float g_rain_dt;             /* the last frame's length, seconds (the spatter's step) */
static int g_rain_preview;          /* weather.preview is on (d3d8_set_weather_fx): it rains as previewed wherever */

void d3d8_set_rain(const RainSettings* settings)
{
    g_rain_set = *settings;
    if (g_rain_set.dry < 1.0f)
        g_rain_set.dry = 1.0f;
    g_rain_set.range = g_rain_set.range >= 8.0f ? (g_rain_set.range <= 500.0f ? g_rain_set.range : 500.0f) : 48.0f;
    g_rain_set.reach = g_rain_set.reach >= 52.0f ? (g_rain_set.reach <= 500.0f ? g_rain_set.reach : 500.0f) : 150.0f;
}

void d3d8_set_weather(unsigned weather, int known)
{
    g_rain_weather = weather, g_rain_weather_known = known;
}

static void rain_set_zone(int zone) { g_rain_zone = zone; }

void d3d8_set_zone_weathers(uint32_t mask) { g_rain_zone_weathers = mask; }

/* A zone whose model DAT has no rain or squall weather (6, 7) that carries the rain itself never rains, whatever the server's
 * weather: the game has no rain of its own there. Phomiuna Aqueducts (indoors) is given rain by the server, its DAT has none,
 * and VanaCore's rain fell in it (a player, 2026-11-21); Alzadaal Undersea Ruins' squall, as most indoor zones' rain, is a sky
 * and light with no rain in it (2026-11-22: core/graphics_rain.h track_rain_weathers clears those bits). 0: the DAT not read,
 * so rain as given. */
static int rain_zone_dry(void)
{
    return !g_rain_preview && g_rain_zone_weathers != 0 && (g_rain_zone_weathers & (1u << 6 | 1u << 7)) == 0;
}

/* Thunder weathers (14, 15) where rain would be wrong, left dry (rain.thunder): a zone whose DAT has no rain (rain_zone_dry) -
 * Oldton and Newton Movalpolis' mines (it rained there, a player, 2026-11-20), Qufim Island, Delkfutt's Tower, the Stellar
 * Fulcrum... And by name, for when the DAT isn't read or has rain all the same: Qufim Island, Behemoth's Dominion, Ru'Aun
 * Gardens (above the clouds), Stellar Fulcrum, the Cloister of Storms, the Hall of the Gods (rain in its DAT), Delkfutt's
 * Tower, Dynamis-Qufim */
static int rain_thunder_dry(int zone)
{
    if (rain_zone_dry())
        return 1;
    static const int dry[] = { 126, 127, 130, 179, 202, 251, 157, 158, 184, 41 };
    for (size_t i = 0; i < sizeof dry / sizeof dry[0]; ++i)
        if (dry[i] == zone)
            return 1;
    return 0;
}

/* how heavily it rains by the weather the server gave: rain 1, squall 1.6, thunder 0.6 and thunderstorms 1.4 (a look alone,
 * rain.thunder; not where rain_thunder_dry leaves it dry), 0 none and in a zone with no rain (rain_zone_dry); rain.force as rain */
static float rain_heaviness(void)
{
    if (g_rain_set.force)
        return 1.0f;
    if (!g_rain_weather_known)
        return 0.0f;
    switch (g_rain_weather)
    {
    case 6: return rain_zone_dry() ? 0.0f : 1.0f;
    case 7: return rain_zone_dry() ? 0.0f : 1.6f;
    case 14: return g_rain_set.thunder && !rain_thunder_dry(g_rain_zone) ? 0.6f : 0.0f;
    case 15: return g_rain_set.thunder && !rain_thunder_dry(g_rain_zone) ? 1.4f : 0.0f;
    default: return 0.0f;
    }
}

/* is it raining: the weather the server gave (rain 6, squall 7, thunder with rain.thunder), or rain.force */
static int rain_falling(void)
{
    return rain_heaviness() > 0.0f;
}

/* the wetness and the clock moved on, once a frame: wetting over 12 s, drying over rain.dry s */
static void rain_tick(void)
{
    if (g_rain_frame == g_scene.frame)
        return;
    g_rain_frame = g_scene.frame;
    const uint64_t now = gfx_now_ns();
    double dt = g_rain_last_ns ? (double)(now - g_rain_last_ns) * 1e-9 : 0.0;
    g_rain_last_ns = now;
    if (dt > 0.25 || g_rain_set.freeze) /* a pause or a zone change: carry on from where it was; rain.freeze: the rain held still -
                                         * the drops, the droplets, the rings, the clock the shaders' streaks, ripples, runnels and
                                         * lens beads run by, the wetness - and going on from there, no burst, once it's off */
        dt = 0.0;
    g_rain_dt = (float)dt;
    g_rain_clock = fmod(g_rain_clock + dt, 600.0);
    const int falling = rain_falling();
    const float rate = falling ? 1.0f / 12.0f : -1.0f / g_rain_set.dry;
    g_rain_wet += (float)dt * rate;
    if (g_rain_wet > 1.0f)
        g_rain_wet = 1.0f;
    if (g_rain_wet < 0.0f)
        g_rain_wet = 0.0f;
    /* the rain itself comes and goes over 6 s (the game fades its own over about 20) */
    const float want = rain_heaviness();
    const float step = (float)dt / 6.0f;
    g_rain_fall += g_rain_fall < want ? fminf(step, want - g_rain_fall) : -fminf(step, g_rain_fall - want);
}

/* The game's own rain (6 dynamic draws round the camera, additive, texture 53942d980fab7902: research/true-sky.md) left out
 * while VanaCore's rain draws (rain.game 0), so two rains don't fight; the snow and the sand storms are other textures */
#define RAIN_GAME_TEX 0x53942d980fab7902ull
static int rain_game_skip(const GfxDraw* d, uint32_t tex0)
{
    if (g_rain_set.game || !g_rain_set.rain || g_rain_fall <= 0.001f)
        return 0;
    if (d->vs.rhw || !d->pipe.blend || d->pipe.dst != 2 || d->pipe.op != 1) /* additive, in the world */
        return 0;
    Obj* t0 = obj(tex0);
    return t0 && texture_hash(t0) == RAIN_GAME_TEX;
}

/* --- the shelter: the zone's collision cast down ---------------------------------------------------------------------- */
/* Where the rain can't fall: under a roof, a bridge, a cave's ceiling. The zone's whole collision (d3d8_set_cover, the same
 * hand-over as the collision view's) is laid in the world once and binned over x and z; a map of COVER_N cells a side, COVER_CELL
 * yalms each, follows the player: each cell holds the height (the world's y, down) of the lowest roof over the ground there - the
 * lowest surface that has a floor at least COVER_ROOF yalms under it - or a sky's worth of height with none. The pass reads it
 * (stage 5) where each streak and splash is: under the roof's height, the rain is kept out. A hill beside the player has one
 * surface at its cell, no floor beneath, and takes the rain; a bridge shelters the water under it and takes the rain on top.
 * The map is laid again, a few rows a frame, when the player has moved COVER_MOVE yalms from its centre. The near-vertical
 * triangles (walls) never shelter and aren't kept. */
#define COVER_N 128
#define COVER_CELL 0.75f
#define COVER_ROOF 2.0f
#define COVER_EPS 0.4f /* a point is sheltered this far under its roof or more (the ground at the roof's own height is open) */
#define COVER_LID 35.0f /* a roof further above its floor than this is the zone's invisible lid, no shelter */
#define COVER_MOVE 12.0f
#define COVER_BIN 4.0f /* (8 before the zone's own meshes joined the collision: their bins hold many more) */
#define COVER_ROWS 6 /* rows laid a frame (12 before the zone's own meshes made each cell's cast dearer) */
#define COVER_MOST 400000u /* triangles kept, at most */
#define COVER_SKY 1e9f
#define COVER_FMT 21u /* D3DFMT_A8R8G8B8: the roof's height about the map's reference, 8.8 fixed point in R (whole) and G (fraction), 0xFFFF the sky */
#define COVER_RANGE 128.0f
static struct
{
    int zone;
    uint32_t ntri;     /* both sources' triangles (cover_tri) */
    float lo[2], hi[2]; /* their box over x and z */
    uint32_t bx, bz;    /* bins */
    uint32_t* first;    /* bin b's triangles are at[first[b]..first[b+1]) */
    uint32_t* at;
    float map[COVER_N * COVER_N]; /* the roof's height a cell, COVER_SKY with none: the one in use */
    float next[COVER_N * COVER_N]; /* ... the one being laid, round (nx, nz, ny) */
    uint32_t px[COVER_N * COVER_N]; /* ... as the texture holds it */
    int gen;            /* counted up at each build: the far grid's roofs (d3d8_rain_ground.h) are cast again */
    float ox, oz, oy;   /* the map's corner (its cell 0, 0) in the world, and its reference height (the camera's as it was laid) */
    float nx, nz, ny;   /* ... the next map's, while it's laid */
    int ready;          /* a whole map is in the texture: the old one stays in use while the next is laid */
    int row;            /* the next row to lay (COVER_N: laid) */
    int dirty;          /* the texture wants the map */
    GfxTex* tex;
} g_cover;

/* the shelter's surfaces in the world, 9 floats a triangle, the near-level ones alone: [0] the zone's collision (d3d8_set_cover),
 * [1] the zone's own meshes as the game draws them (d3d8_set_cover_mesh, core/zone_casters.cpp) - an archway, a covered passage,
 * an awning or a roof nobody walks on is in the drawn meshes and not in the collision, and the rain fell through it */
static struct
{
    float* tri;
    uint32_t n;
    int zone;
} g_cover_src[2];

static const float* cover_tri(uint32_t i)
{
    return i < g_cover_src[0].n ? &g_cover_src[0].tri[(size_t)i * 9] : &g_cover_src[1].tri[(size_t)(i - g_cover_src[0].n) * 9];
}

static void cover_free(void)
{
    free(g_cover.first), free(g_cover.at);
    g_cover.first = NULL, g_cover.at = NULL, g_cover.ntri = 0, g_cover.zone = 0, g_cover.row = COVER_N;
}

/* a placed triangle w kept: not too steep and not degenerate. The collision's to 72 degrees (a wall never shelters); the drawn
 * meshes' (drawn) to 84: a cave's ceiling is rock in steep facets, and at 72 Norg's was full of holes the rain fell through,
 * its drops gone mid-air as they drifted under the facets kept beside them */
static int cover_keep(const float* w, int drawn)
{
    const float ax = w[3] - w[0], ay = w[4] - w[1], az = w[5] - w[2], bx = w[6] - w[0], by = w[7] - w[1], bz = w[8] - w[2];
    const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    const float l2 = nx * nx + ny * ny + nz * nz;
    return l2 > 1e-12f && ny * ny >= (drawn ? 0.011f : 0.09f) * l2;
}

static uint32_t cover_bin(float x, float z)
{
    int i = (int)((x - g_cover.lo[0]) / COVER_BIN), j = (int)((z - g_cover.lo[1]) / COVER_BIN);
    i = i < 0 ? 0 : i >= (int)g_cover.bx ? (int)g_cover.bx - 1 : i;
    j = j < 0 ? 0 : j >= (int)g_cover.bz ? (int)g_cover.bz - 1 : j;
    return (uint32_t)j * g_cover.bx + (uint32_t)i;
}

/* both sources' triangles binned over x and z: each goes into every bin its box touches (counted, then placed); the map is laid
 * again round the player at the next frame, the last one kept in use meanwhile (none at all once both sources are empty) */
static void cover_build(void)
{
    const int was = g_cover.zone;
    cover_free();
    const uint32_t n = g_cover_src[0].n + g_cover_src[1].n;
    if (!n)
    {
        g_cover.ready = 0;
        return;
    }
    g_cover.lo[0] = g_cover.lo[1] = 1e30f, g_cover.hi[0] = g_cover.hi[1] = -1e30f;
    for (uint32_t t = 0; t < n; ++t)
    {
        const float* w = cover_tri(t);
        for (int c = 0; c < 3; ++c)
        {
            g_cover.lo[0] = fminf(g_cover.lo[0], w[c * 3]), g_cover.hi[0] = fmaxf(g_cover.hi[0], w[c * 3]);
            g_cover.lo[1] = fminf(g_cover.lo[1], w[c * 3 + 2]), g_cover.hi[1] = fmaxf(g_cover.hi[1], w[c * 3 + 2]);
        }
    }
    g_cover.bx = (uint32_t)((g_cover.hi[0] - g_cover.lo[0]) / COVER_BIN) + 1, g_cover.bz = (uint32_t)((g_cover.hi[1] - g_cover.lo[1]) / COVER_BIN) + 1;
    const uint32_t nb = g_cover.bx * g_cover.bz;
    g_cover.first = (uint32_t*)calloc((size_t)nb + 1, sizeof(uint32_t));
    uint32_t* count = (uint32_t*)calloc(nb, sizeof(uint32_t));
    if (!g_cover.first || !count)
    {
        free(count), cover_free();
        return;
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        for (uint32_t t = 0; t < n; ++t)
        {
            const float* w = cover_tri(t);
            const uint32_t b0 = cover_bin(fminf(w[0], fminf(w[3], w[6])), fminf(w[2], fminf(w[5], w[8])));
            const uint32_t b1 = cover_bin(fmaxf(w[0], fmaxf(w[3], w[6])), fmaxf(w[2], fmaxf(w[5], w[8])));
            for (uint32_t j = b0 / g_cover.bx; j <= b1 / g_cover.bx; ++j)
                for (uint32_t i = b0 % g_cover.bx; i <= b1 % g_cover.bx; ++i)
                {
                    const uint32_t bin = j * g_cover.bx + i;
                    if (pass == 0)
                        ++count[bin];
                    else
                        g_cover.at[g_cover.first[bin] + count[bin]++] = t;
                }
        }
        if (pass == 0)
        {
            uint32_t sum = 0;
            for (uint32_t bin = 0; bin < nb; ++bin)
                g_cover.first[bin] = sum, sum += count[bin], count[bin] = 0;
            g_cover.first[nb] = sum;
            g_cover.at = (uint32_t*)malloc((size_t)(sum ? sum : 1) * sizeof(uint32_t));
            if (!g_cover.at)
            {
                free(count), cover_free();
                return;
            }
        }
    }
    free(count);
    g_cover.ntri = n;
    g_cover.zone = g_cover_src[0].zone ? g_cover_src[0].zone : g_cover_src[1].zone;
    if (g_cover.zone != was)
        g_cover.ready = 0; /* another zone's map: none until this one's is laid */
    g_cover.ox = g_cover.oz = 1e30f; /* laid at the next frame, round the player */
    ++g_cover.gen;
}

static void cover_src_clear(int k)
{
    free(g_cover_src[k].tri);
    g_cover_src[k].tri = NULL, g_cover_src[k].n = 0, g_cover_src[k].zone = 0;
}

/* the collision's near-level triangles placed in the world (d3d8_set_cover) */
void d3d8_set_cover(const GrassFloors* f)
{
    cover_src_clear(0);
    if (g_cover_src[1].zone && (!f || f->zone != g_cover_src[1].zone))
        cover_src_clear(1); /* the meshes of a zone left */
    if (!f || !f->zone || !f->npairs || !f->nmesh || !f->ntris || !f->tris || !f->mesh_first || !f->matrices || !f->pairs)
    {
        cover_build();
        return;
    }
    uint64_t total = 0;
    for (uint32_t k = 0; k < f->npairs; ++k)
    {
        if (f->pairs[2 * k] >= f->nmatrices || f->pairs[2 * k + 1] >= f->nmesh)
        {
            cover_build();
            return;
        }
        total += f->mesh_first[f->pairs[2 * k + 1] + 1] - f->mesh_first[f->pairs[2 * k + 1]];
    }
    if (!total || total > 4u * COVER_MOST || !(g_cover_src[0].tri = (float*)malloc((size_t)total * 9 * sizeof(float))))
    {
        cover_build();
        return;
    }
    uint32_t n = 0;
    for (uint32_t k = 0; k < f->npairs && n < COVER_MOST; ++k)
    {
        const uint32_t mesh = f->pairs[2 * k + 1];
        const float* m = &f->matrices[f->pairs[2 * k] * 12];
        for (uint32_t t = f->mesh_first[mesh]; t < f->mesh_first[mesh + 1] && n < COVER_MOST; ++t)
        {
            float* w = &g_cover_src[0].tri[n * 9];
            for (int c = 0; c < 3; ++c)
            {
                const float* p = &f->tris[t * 9 + c * 3];
                for (int j = 0; j < 3; ++j)
                    w[c * 3 + j] = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
            }
            n += (uint32_t)cover_keep(w, 0);
        }
    }
    g_cover_src[0].n = n, g_cover_src[0].zone = f->zone;
    cover_build();
}

/* the zone's own meshes (their solid parts, in the world, 3 floats a vertex, a triangle each three: zonemesh.c's) as a second
 * source of roofs (d3d8_set_cover_mesh); zone 0: none */
void d3d8_set_cover_mesh(int zone, const float* pos, uint32_t npos)
{
    cover_src_clear(1);
    const uint32_t nt = pos && zone ? npos / 3 : 0;
    if (nt && (g_cover_src[1].tri = (float*)malloc((size_t)(nt < COVER_MOST ? nt : COVER_MOST) * 9 * sizeof(float))) != NULL)
    {
        uint32_t n = 0;
        for (uint32_t t = 0; t < nt && n < COVER_MOST; ++t)
        {
            float* w = &g_cover_src[1].tri[n * 9];
            memcpy(w, &pos[(size_t)t * 9], 9 * sizeof(float));
            n += (uint32_t)cover_keep(w, 1);
        }
        g_cover_src[1].n = n, g_cover_src[1].zone = zone;
    }
    cover_build();
}

/* the roof's height over (x, z): the surfaces cast through there, from the top down; the topmost one that isn't the zone's lid
 * and has anything at all COVER_ROOF or more under it (a roof, a bridge, a hall's floor over a passage, an awning). A point is
 * sheltered when it's more than COVER_EPS under it, so the top of a terrace over a tunnel still takes the rain while the
 * tunnel doesn't, and it doesn't hang on where the camera is (a camera above an arcade's roof left the arcade open). A lone
 * surface over the reference height `ref` is a roof whose floor the collision lacks (a pillar with no top face).
 *   Only the collision's surfaces can be the lid (COVER_LID): one the game draws (source 1, the zone's own meshes) is seen, so
 * it shelters however far over the floor it is. Lower Jeuno's halls under the streets are deeper than the lid's 35 yalms, and
 * the rain fell in them as if the street over them were the zone's lid */
static float cover_roof(float x, float z, float ref)
{
    float ys[32];
    unsigned char seen[32];
    int ny = 0;
    const uint32_t b = cover_bin(x, z);
    for (uint32_t i = g_cover.first[b]; i < g_cover.first[b + 1]; ++i)
    {
        const float* w = cover_tri(g_cover.at[i]);
        const float d0 = (w[3] - w[0]) * (z - w[2]) - (w[5] - w[2]) * (x - w[0]);
        const float d1 = (w[6] - w[3]) * (z - w[5]) - (w[8] - w[5]) * (x - w[3]);
        const float d2 = (w[0] - w[6]) * (z - w[8]) - (w[2] - w[8]) * (x - w[6]);
        if ((d0 < 0.0f || d1 < 0.0f || d2 < 0.0f) && (d0 > 0.0f || d1 > 0.0f || d2 > 0.0f))
            continue;
        const float area = d0 + d1 + d2;
        if (fabsf(area) < 1e-9f)
            continue;
        const unsigned char s = g_cover.at[i] >= g_cover_src[0].n; /* the zone's own meshes: drawn, never the lid */
        const float y = (w[1] * d1 + w[4] * d2 + w[7] * d0) / area; /* the height by the barycentric weights */
        /* the 32 highest kept: the first 32 found were the collision's (listed first), and in a cave town stacked with floors
         * Norg's ceiling, a drawn mesh's, was never reached - it rained inside */
        int at = ny < 32 ? ny++ : -1;
        if (at < 0)
        {
            at = 0;
            for (int k = 1; k < 32; ++k)
                at = ys[k] > ys[at] ? k : at;
            if (!(y < ys[at]))
                continue;
        }
        ys[at] = y, seen[at] = s;
    }
    if (ny == 0)
        return COVER_SKY;
    if (ny == 1)
        return ys[0] < ref - 1.0f && (seen[0] || ref - ys[0] <= COVER_LID) ? ys[0] : COVER_SKY;
    for (int i = 1; i < ny; ++i) /* highest in space first: the smallest y */
    {
        const float y = ys[i];
        const unsigned char s = seen[i];
        int j = i;
        while (j > 0 && ys[j - 1] > y)
            ys[j] = ys[j - 1], seen[j] = seen[j - 1], --j;
        ys[j] = y, seen[j] = s;
    }
    for (int i = 0; i + 1 < ny; ++i)
    {
        if (!seen[i] && ys[i + 1] - ys[i] > COVER_LID)
            continue; /* the zone's lid (or a sky far over everything): not a roof */
        for (int k = i + 1; k < ny; ++k)
            if (ys[k] - ys[i] >= COVER_ROOF)
                return !seen[i] && ys[k] - ys[i] > COVER_LID ? COVER_SKY : ys[i];
        return COVER_SKY; /* the ground and whatever lies flat on it: open */
    }
    return COVER_SKY;
}

#include "d3d8_rain_ground.h" /* the far drops' ground and roofs */

/* the roof over (x, z) from the map in use, at once (the drops' and the characters' shelter, every frame); off the map, the
 * cast itself when `exact`, else the far grid's (none where it isn't laid yet) */
static float cover_at(float x, float z, float ref, int exact)
{
    if (!g_cover.ntri || !g_rain_set.shelter)
        return COVER_SKY;
    const int i = (int)floorf((x - g_cover.ox) / COVER_CELL), j = (int)floorf((z - g_cover.oz) / COVER_CELL);
    if (g_cover.ready && i >= 0 && j >= 0 && i < COVER_N && j < COVER_N)
        return g_cover.map[j * COVER_N + i];
    return exact ? cover_roof(x, z, ref) : rain_ground_roof(x, z);
}

/* the laid map (next) into the one in use, each roof kept only where the four cells round it are under a roof too: a cell's
 * roof is its middle's, so a roof's edge reached out up to a cell past its wall (dry steps along a building's front, dry
 * squares round a plinth); drawn in by a cell, it never does, and things under some 1.5 yalms across (a statue's arm, a
 * lamp's cap) shelter nothing. The shader's soft read (emit_rain_fs) fades over the cells left */
static void cover_erode(void)
{
    for (int j = 0; j < COVER_N; ++j)
        for (int i = 0; i < COVER_N; ++i)
        {
            const float* n = g_cover.next;
            const int c = j * COVER_N + i;
            const int all = n[c] < COVER_SKY && (i == 0 || n[c - 1] < COVER_SKY) && (i == COVER_N - 1 || n[c + 1] < COVER_SKY) &&
                            (j == 0 || n[c - COVER_N] < COVER_SKY) && (j == COVER_N - 1 || n[c + COVER_N] < COVER_SKY);
            g_cover.map[c] = all ? n[c] : COVER_SKY;
        }
}

/* the map laid round (x, z) a few rows a frame; the texture made and filled once it's whole */
static void cover_update(float x, float y, float z)
{
    if (!g_cover.ntri || !g_rain_set.shelter)
        return;
    /* the next map is laid round the camera into `next` while the last whole one stays in use, so the shelter never lapses
     * (laying straight into the one in use left the rain open everywhere for the frames it took, every few steps) */
    const float cx = g_cover.ox + COVER_N * COVER_CELL * 0.5f, cz = g_cover.oz + COVER_N * COVER_CELL * 0.5f;
    if (g_cover.row >= COVER_N && (!g_cover.ready || fabsf(x - cx) > COVER_MOVE || fabsf(z - cz) > COVER_MOVE || fabsf(y - g_cover.oy) > COVER_ROOF))
    {
        g_cover.nx = x - COVER_N * COVER_CELL * 0.5f, g_cover.nz = z - COVER_N * COVER_CELL * 0.5f, g_cover.ny = y;
        g_cover.row = 0;
    }
    if (g_cover.row < COVER_N)
    {
        for (int r = 0; r < COVER_ROWS && g_cover.row < COVER_N; ++r, ++g_cover.row)
            for (int i = 0; i < COVER_N; ++i)
                g_cover.next[g_cover.row * COVER_N + i] = cover_roof(g_cover.nx + ((float)i + 0.5f) * COVER_CELL, g_cover.nz + ((float)g_cover.row + 0.5f) * COVER_CELL, g_cover.ny);
        if (g_cover.row >= COVER_N)
        {
            cover_erode();
            g_cover.ox = g_cover.nx, g_cover.oz = g_cover.nz, g_cover.oy = g_cover.ny;
            g_cover.dirty = 1;
        }
    }
    if (g_cover.dirty)
    {
        for (int i = 0; i < COVER_N * COVER_N; ++i) /* the roof about the reference, in 1/256 yalms, 0xFFFF the sky (or anything that far below) */
        {
            const float r = g_cover.map[i] >= COVER_SKY ? COVER_RANGE : g_cover.map[i] - g_cover.oy;
            const float q = (r + COVER_RANGE) * 256.0f;
            const uint32_t v = q <= 0.0f ? 0u : q >= 65535.0f ? 65535u : (uint32_t)q;
            g_cover.px[i] = 0xFF000000u | ((v >> 8) << 16) | ((v & 255u) << 8);
        }
        if (!g_cover.tex)
            g_cover.tex = gfx_tex_create(GFX_TEX_2D, COVER_FMT, COVER_N, COVER_N, 1, GFX_USE_SAMPLE);
        if (g_cover.tex)
            gfx_tex_upload(g_cover.tex, 0, 0, g_cover.px, COVER_N * 4), g_cover.ready = 1;
        g_cover.dirty = 0;
    }
}

static void rain_particles_update(float dt, const float cam[3], float wind);
static void rain_particles_draw(const GfxScene* s, const float* inv, const float drop[3], const GfxDraw* pass);
static float saturate_f(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* --- the pass: the streaks, the splashes, the wet ground -------------------------------------------------------------- */
/* After the water pass (d3d8.c scene end): one draw over the whole viewport (pretransformed, no depth), the scene's colour and
 * depth copies in stages 6 and 7 (the water pass's this frame, else taken now) and the shelter map in stage 5; the pixel function
 * (gfx_hlsl_rain.h emit_rain_fs) puts the scene's colour back out with the rain over it. Texture matrix 3 carries the view's
 * inverse (the world from a pixel's view-space position), 4 the view, 7 the rain's own values. */
/* the wet ground's mask (GfxFsKey.rain 3: the pass's pixel function, its wet ground, puddles and shelter written out, not laid
 * over), drawn into a target at the scene's size for Direct3D 12's floor reflections (gfx_scene_rain_mask): the wet ground
 * mirrors the scene there, its puddles near mirrors. Not drawn where nothing reads it (Direct3D 11, the effects off,
 * rain.reflect 0, the wet ground off) */
static void rain_mask_draw(GfxDraw* d, GfxTex* color, GfxTex* depth, uint32_t w, uint32_t h)
{
    const float k = g_rain_set.ground && g_rain_set.reflect > 0.0f ? g_rain_wet : 0.0f;
    if (!(k > 0.0f) || !gfx_scene_rain_mask_wanted())
    {
        gfx_scene_rain_mask(NULL, 0.0f, 0.0f, 0.0f);
        return;
    }
    if (g_rain_mask && (g_rain_mask_w != w || g_rain_mask_h != h))
        gfx_tex_destroy(g_rain_mask), g_rain_mask = NULL;
    if (!g_rain_mask)
        g_rain_mask = gfx_tex_create(GFX_TEX_2D, 21 /* D3DFMT_A8R8G8B8 */, w, h, 1, GFX_USE_RT), g_rain_mask_w = w, g_rain_mask_h = h;
    if (!g_rain_mask)
    {
        gfx_scene_rain_mask(NULL, 0.0f, 0.0f, 0.0f);
        return;
    }
    gfx_set_targets(g_rain_mask, 0, 0, NULL);
    const uint8_t was_mask = d->pipe.write_mask, was_blend = d->pipe.blend;
    d->fs.rain = 3, d->pipe.write_mask = 0xF, d->pipe.blend = 0;
    gfx_draw(d);
    d->fs.rain = 1, d->pipe.write_mask = was_mask, d->pipe.blend = was_blend;
    gfx_set_targets(color, 0, 0, depth);
    const float wk = k * k * (3.0f - 2.0f * k); /* eased as the shader's wet ground is */
    gfx_scene_rain_mask(g_rain_mask, wk, g_rain_set.reflect, g_rain_set.puddles);
}

static void rain_mask_lost(void)
{
    gfx_tex_destroy(g_rain_mask), g_rain_mask = NULL;
}

/* The drops landing on VanaCore's water (rain.water), from the water's own draws in its pass (d3d8_water.h, after
 * water_pass_draw and water_mirror_bind): the rain's pass lays its rings by the scene's depth from before the water (the
 * copies), and the drops land on the zone's collision - over water both are the bed, so nothing of the rain showed on a lake.
 * The water's pixel function makes the rings itself (gfx_hlsl_water_rain.h emit_water_rain; the draw's key says so by a
 * texture in stage 2), from texture matrix 2's bytes 5-14 (0-4 are the mirror's): how strong (the rain's heaviness times
 * rain.splash and rain.water), the clock, how many (the heaviness times rain.density), the splash reach, whether stage 2 is
 * the shelter map (else the colour copy stands in: a stage needs a texture), the map's reference height, its corner and 1
 * over its width. The map is the pass's from the frame before (cover_update), which moves a few rows a frame */
static void rain_water_bind(GfxDraw* d)
{
    if (!g_rain_set.rain || !(g_rain_set.water > 0.0f) || !(g_rain_set.splash > 0.0f) || !d->tex[6])
        return;
    rain_tick();
    if (g_rain_fall <= 0.001f)
        return;
    const int map = g_cover.tex && g_cover.ready && g_rain_set.shelter;
    d->tex[2] = map ? g_cover.tex : d->tex[6];
    d->fs.st[2].tex = 1, d->fs.st[2].ncoord = 2;
    d->samp[2] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0, 0, { 0, 0 }, 0 }; /* CLAMP, POINT */
    float* m = d->u.texm[2];
    m[5] = saturate_f(g_rain_fall) * fminf(g_rain_set.splash, 3.0f) * g_rain_set.water;
    m[6] = (float)g_rain_clock, m[7] = g_rain_fall * g_rain_set.density, m[8] = fminf(g_rain_set.range, g_rain_set.reach);
    m[9] = map ? 1.0f : 0.0f, m[11] = g_cover.oy;
    m[12] = g_cover.ox, m[13] = g_cover.oz, m[14] = 1.0f / (COVER_N * COVER_CELL);
}

static void rain_pass(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    /* the characters' wet look: the reflections on them, by the occlusion (gfx_d3d12.c fx_ssr), as wet as they are */
    if (g_rain_set.wet)
        rain_tick();
    g_scene.s.rain_sheen = g_rain_set.wet ? g_rain_wet * g_rain_set.sheen : 0.0f;
    if (!g_rain_set.rain || !color || !g_scene.cam)
    {
        gfx_scene_rain_mask(NULL, 0.0f, 0.0f, 0.0f);
        return;
    }
    rain_tick();
    /* on while it rains and after, while the ground is still wet: the puddles and the wet ground come in and dry off with
     * the wetness (12 s in, rain.dry out) - cut when the rain itself had eased off, they vanished at once, and coming back
     * they jumped to the wetness the characters still had */
    if (g_rain_fall <= 0.001f && g_rain_wet <= 0.0f)
        return;
    float inv[16];
    if (!water_affine_inv(inv, s->view))
        return;
    cover_update(inv[12], inv[13], inv[14]);
    GfxTex *cc = NULL, *dc = NULL;
    uint32_t w = 0, h = 0;
    if (g_water_late.c && g_water_late.frame == g_scene.frame)
        cc = g_water_late.c, dc = g_water_late.d, w = g_water_late.w, h = g_water_late.h;
    else if (gfx_scene_snapshot(color, &cc, &dc, &w, &h))
        g_water_late.c = cc, g_water_late.d = dc, g_water_late.w = w, g_water_late.h = h, g_water_late.frame = g_scene.frame;
    if (!cc)
        return;
    gfx_set_targets(color, 0, 0, depth);
    static GfxDraw d;
    memset(&d, 0, sizeof d);
    const float x0 = (float)s->vp[0], y0 = (float)s->vp[1], x1 = x0 + (float)s->vp[2], y1 = y0 + (float)s->vp[3];
    static float v[6][4];
    const float q[6][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y0 }, { x1, y1 }, { x0, y1 } };
    for (int i = 0; i < 6; ++i)
        v[i][0] = q[i][0], v[i][1] = q[i][1], v[i][2] = 0.5f, v[i][3] = 1.0f;
    d.vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT4, 0 };
    d.vs.rhw = 1, d.vs.centered = 1;
    d.u.stride[0] = 16;
    d.data[0] = v, d.size[0] = sizeof v;
    d.prim = GFX_TRIANGLELIST, d.count = 2;
    for (int i = 0; i < 4; ++i)
        d.u.vp[i] = (float)s->vp[i], d.u.mat_d[i] = d.u.tfactor[i] = 1.0f;
    memcpy(d.vp, s->vp, sizeof d.vp);
    memcpy(d.u.fogcolor, s->fogcolor, sizeof d.u.fogcolor);
    memcpy(d.u.ambient, s->ambient, sizeof d.u.ambient);
    d.u.params2[2] = s->proj[0] != 0.0f ? 1.0f / s->proj[0] : 0.0f, d.u.params2[3] = s->proj[5] != 0.0f ? 1.0f / s->proj[5] : 0.0f;
    d.cull = 1, d.fill = 3;
    water_pass_draw(&d, cc, dc, w, h, s); /* stages 6 and 7, the projection's and the viewport's values in matrix 6 */
    d.fs.water = 0, d.fs.rain = 1;
    /* laid over the target as it is, not as the copies have it: with the water pass on, the copies are the scene before the
     * water (d3d8_water.h water_pass takes them, this reuses them), and putting the copy back out with the rain over it wiped
     * the water the pass had drawn since. The pixel function gives the rain's change as out = target x a + rgb (emit_rain_fs's
     * end): ONE, SRCALPHA, ADD; the target's alpha left alone */
    d.pipe.write_mask = 0x7;
    d.pipe.blend = 1, d.pipe.src = 2, d.pipe.dst = 5, d.pipe.op = 1;
    d.tex[5] = g_cover.tex && g_cover.ready && g_rain_set.shelter ? g_cover.tex : NULL;
    d.fs.st[5].tex = d.tex[5] ? 1 : 0, d.fs.st[5].ncoord = 2;
    d.samp[5] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0, 0, { 0, 0 }, 0 }; /* CLAMP, POINT */
    memcpy(d.u.texm[3], inv, 64), memcpy(d.u.texm[4], s->view, 64);
    float* m = d.u.texm[7];
    m[0] = (float)g_rain_clock, m[1] = g_rain_fall, m[2] = g_rain_set.density, m[3] = g_rain_set.splash;
    m[4] = inv[12], m[5] = inv[13], m[6] = inv[14];
    m[7] = g_cover.ox, m[8] = g_cover.oz, m[9] = 1.0f / (COVER_N * COVER_CELL), m[10] = g_rain_wet;
    m[11] = d.tex[5] ? 1.0f : 0.0f;
    m[12] = s->fog[2] != 0.0f ? s->fog[1] : 0.0f; /* the fog's end: the far streaks fade into it */
    m[14] = g_cover.oy; /* the shelter map's reference height */
    m[15] = g_rain_set.ground ? 1.0f : 0.0f; /* the wet ground and the puddles (rain.ground) */
    /* the lens's droplets (texture matrix 6's byte 2, which the pass's copies leave alone): rain.lens while rain.lens.on, none
     * while the camera itself is under a roof */
    float lens = g_rain_set.lens_on ? g_rain_set.lens : 0.0f;
    static float lens_now = 0.0f;
    if (lens > 0.0f)
    {
        const float roof = cover_at(inv[12], inv[14], inv[13], 1);
        if (roof < COVER_SKY && inv[13] > roof + COVER_EPS)
            lens = 0.0f;
    }
    lens_now += (lens - lens_now) * fminf(g_rain_dt * 1.5f, 1.0f); /* eases over about a second: the beads fade under a roof, come back out of it */
    d.u.texm[6][2] = lens_now;
    uint32_t dw, dh; /* the window's shape (byte 3): the scene's target is stretched over it, and the beads are round there */
    drawn_size(&dw, &dh);
    d.u.texm[6][3] = dh ? (float)dw / (float)dh : 1.0f;
    /* the diagnostics (emit_rain_fs): the debug views' Rain (5) and Rain rings (6), rain.force's */
    m[13] = g_rain_set.view ? 4.0f + (float)g_rain_set.view : g_rain_set.force >= 2 ? (float)(g_rain_set.force - 1) : 0.0f;
    d.u.texm[6][12] = g_rain_set.reach; /* the rain's reach: the screen's layers lie past it */
    d.u.texm[6][13] = fminf(g_rain_set.range, g_rain_set.reach); /* ... and its splashes' (the Rain rings view) */
    gfx_draw(&d);
    rain_mask_draw(&d, color, depth, w, h);
    /* the droplets off the characters, after the rain is laid: the pass's dt (rain_tick's), the wind as the shader leans the
     * streaks, the drops' grey as the shader lights them */
    {
        const float t = (float)g_rain_clock;
        const float lean = 0.05f + 0.15f * saturate_f(g_rain_fall - 0.9f);
        const float wind = lean * (0.6f + 0.4f * sinf(t * 0.09f)) * (sinf(t * 0.013f + 0.4f) < 0.0f ? -1.0f : 1.0f);
        const float cam[3] = { inv[12], inv[13], inv[14] };
        float drop[3];
        for (int i = 0; i < 3; ++i)
            drop[i] = fmaxf(s->fogcolor[i] * 0.6f + 0.25f, 0.42f) * 0.9f;
        rain_particles_update(g_rain_dt, cam, wind);
        rain_particles_draw(s, inv, drop, &d);
    }
}

/* --- the drops, the spatter and the rings: particles ------------------------------------------------------------------ */
/* Real things in the world, not a flicker: near the camera (DROP_REACH) the rain is a pool of drops, each falling at 12 yalms
 * a second with the wind's lean from well above the camera to the ground under it (the zone's floors: grass_floor_near) or the
 * roof over it (the shelter map's cover_roof), drawn as a thin grey streak along its motion. Where one lands it throws a
 * splash: four to six droplets up and outward a few centimetres that fall back within 0.2 s (the same pool as the spatter the
 * characters throw off their heads, shoulders and arms), and a thin ring that spreads on the ground and fades. All blended,
 * never brighter than the sky, tested against the scene's depth, drawn by the pass (rain_particles). rain.splash scales the
 * splashes, rain.density the drops. */
/* The near drops, the thickest, out to 16 yalms (less with a shorter splash reach): their number goes with the area,
 * DROP_BASE at 16 yalms */
#define DROP_BASE 2048.0f
#define DROP_N 8192
#define DROP_REACH (fminf(g_rain_set.range, 16.0f))
/* Past the near drops, rings as the grass's: the further out, the fewer drops. The middle ring, from the near reach out to
 * mid_reach() (48 yalms): drops spread evenly in the distance, so they thin as 1/r, landing on the floor under them. The far
 * ring, from there out to FAR_REACH (rain.far, Rain Range, 52 to 500 yalms, 150 as made): thinning as 1/r squared, longer and
 * fainter, falling to the ground the far grid has there (d3d8_rain_ground.h); past it only the screen's layers. Each 100
 * yalms out a fifth fewer (rain_band: all to 100, 80% to 200, 60%, 40%, then 20%). The drops splash out to SPLASH_REACH
 * (rain.range, Splash Range, 8 to 500, 48 as made), by the same bands, fading over its last stretch; past some 40 yalms only
 * the ring on the ground (its droplets are under a pixel), grown with the distance to stay a pixel or so */
#define MID_N 6144
#define FAR_N 12288
#define FAR_BASE 6144.0f /* the far ring's drops from 48 to 150 yalms; more as it reaches further, by the distance's log */
#define FAR_REACH (g_rain_set.reach)
#define SPLASH_REACH (fminf(g_rain_set.range, FAR_REACH))
static float mid_reach(void) { return fminf(48.0f, FAR_REACH - 8.0f); }
static float rain_band(float r) { return fmaxf(0.2f, 1.0f - 0.2f * floorf(r / 100.0f)); }
/* a drop landing r out: its splash, 0 none to 1, by the bands and fading over the splash reach's last 15% */
static float splash_at(float r)
{
    const float s = SPLASH_REACH;
    return r < s ? rain_band(r) * saturate_f((s - r) / fmaxf(0.15f * s, 2.0f)) : 0.0f;
}
#define SPAT_N 8192
#define SPAT_KEEP 256 /* the pool's last droplets kept for the characters' spatter: the ground's splashes never take them */
#define SPAT_REACH 28.0f /* characters within this of the camera spatter */
#define RING_N 4096
#define RING_SEG 14
typedef struct Drop
{
    float p[3], v[3], land; /* land: the height it lands at (the world's y, down) */
    float k;                /* its splash, 0 none to 1 (the near drops'): smaller further out */
} Drop;
typedef struct Spat
{
    float p[3], v[3], life, span, size;
    int ground; /* a drop's splash on the ground (drop_land), drawn only where the ground is seen (rain_particles_draw) */
} Spat;
typedef struct Ring
{
    float p[3], life, span, a; /* a: how strong (rain.splash) */
    float s;                   /* its size: 1, more far off (drop_land) */
} Ring;
static Drop g_drop[DROP_N];
static Drop g_mid[MID_N];
static Drop g_far[FAR_N];
static uint32_t g_mid_n, g_far_n;
static Spat g_spat[SPAT_N];
static Ring g_ring[RING_N];
static uint32_t g_drop_n, g_spat_n, g_ring_n, g_spat_seed = 0x9E3779B9u;
/* the camera is indoors - under a roof (the shelter's cast where it stands): no drop splashes. Those it saw were the ones
 * landing on the roof over it, shown through a thin one by the splashes' half-yalm depth test (a tunnel's specks, 2026-11-19) */
static int g_rain_indoor;
static float spat_rand(void)
{
    g_spat_seed = g_spat_seed * 1664525u + 1013904223u;
    return (float)(g_spat_seed >> 8) * (1.0f / 16777216.0f);
}

/* a droplet thrown from p: out `out` and up `rise` yalms a second in a random direction, with the wind */
static void spat_throw(const float p[3], float out, float rise, float wind)
{
    if (g_spat_n >= SPAT_N - SPAT_KEEP)
        return;
    Spat* d = &g_spat[g_spat_n++];
    const float dir = spat_rand() * 6.2831853f;
    memcpy(d->p, p, sizeof d->p);
    d->v[0] = cosf(dir) * out + wind * 2.0f, d->v[1] = -rise, d->v[2] = sinf(dir) * out;
    d->life = 0.0f, d->span = 0.2f + 0.15f * spat_rand(), d->size = 0.016f + 0.012f * spat_rand(), d->ground = 1;
}

/* one droplet off character c (x, height, z, r): a spot on its top - the head's crown, a shoulder's or an upper arm's outer
 * edge, just outside the body so it isn't hidden in it - thrown outward along that side and up. A fine spray: specks of a few
 * millimetres flung a hand's width at most (bigger and faster, it read as a water balloon bursting on every hit); rain.spatter
 * (k) scales their size and how far they fly */
static void spat_spawn(const float* c, float wind)
{
    const float k = g_rain_set.spatter;
    const float h = c[3] > 0.0f && c[3] < 3.0f ? 1.2f + 0.5f * c[3] : 1.7f; /* the head's height over the feet, by the hitbox */
    const float part = spat_rand();
    const float ang = spat_rand() * 6.2831853f;
    const float rx = cosf(ang), rz = sinf(ang);
    float rad, up;
    if (part < 0.3f)
        rad = 0.1f + 0.08f * spat_rand(), up = h + 0.03f; /* the head's crown, just over it */
    else if (part < 0.75f)
        rad = 0.3f + 0.06f * spat_rand(), up = h * 0.86f; /* the shoulders' outer edge */
    else
        rad = 0.34f + 0.06f * spat_rand(), up = h * 0.72f; /* the upper arms' */
    if (g_spat_n + 5 > SPAT_N)
        return;
    const float p[3] = { c[0] + rx * rad, c[1] - up, c[2] + rz * rad };
    Spat* hit = &g_spat[g_spat_n++]; /* the hit: a bright dot where the drop breaks, gone in a few hundredths of a second */
    memcpy(hit->p, p, sizeof hit->p);
    hit->v[0] = 0.0f, hit->v[1] = -0.1f * k, hit->v[2] = 0.0f;
    hit->life = 0.0f, hit->span = 0.06f, hit->size = 0.006f * k, hit->ground = 0;
    for (int i = 0; i < 4; ++i) /* and its droplets, thrown out along that side and up, falling in an arc */
    {
        Spat* d = &g_spat[g_spat_n++];
        const float a2 = ang + (spat_rand() - 0.5f) * 1.6f, out = (0.25f + 0.35f * spat_rand()) * k, rise = (0.4f + 0.4f * spat_rand()) * k;
        memcpy(d->p, p, sizeof d->p);
        d->v[0] = cosf(a2) * out + wind * 0.5f, d->v[1] = -rise, d->v[2] = sinf(a2) * out;
        d->life = 0.0f, d->span = 0.15f + 0.1f * spat_rand(), d->size = (0.0035f + 0.0025f * spat_rand()) * k, d->ground = 0;
    }
}

/* a drop landed at p, dist from the camera: its splash - a crown of droplets and a ring on the ground. rain.splash (k) scales
 * it: how many droplets (2 to 3 times k), how high and far they fly, the ring's strength. It only switched the splash on and off
 * before, a steady 6 to 8 droplets that filled the pool (512, some 1,750 drops landing a second) whatever it was, so 0.5 and 3
 * looked the same. Past 40 yalms the ring alone, grown with the distance, and never the pool's last 1024 (the near ones') */
static void drop_land(const float p[3], float wind, float kr, float dist)
{
    const float k = g_rain_set.splash * kr;
    const float s = 0.6f + 0.4f * fminf(k, 3.0f);
    const int far = dist > 40.0f;
    float want = far ? 0.0f : k * (2.0f + 1.0f * spat_rand());
    for (; want > 0.0f; want -= 1.0f)
        if (want >= 1.0f || spat_rand() < want)
            spat_throw(p, (0.3f + 0.7f * spat_rand()) * s, (0.8f + 0.9f * spat_rand()) * s, wind);
    if (g_ring_n < (far ? RING_N - 1024u : RING_N))
    {
        Ring* r = &g_ring[g_ring_n++];
        memcpy(r->p, p, sizeof r->p);
        r->life = 0.0f, r->span = 0.22f + 0.1f * spat_rand(), r->a = fminf(0.6f * k, 1.0f), r->s = far ? dist / 40.0f : 1.0f;
    }
}

/* a drop spawned over the camera: a random spot within DROP_REACH, 6 to 10 yalms above the camera, falling to the floor under
 * it or the roof over that */
static void drop_spawn(const float cam[3], float wind)
{
    if (g_drop_n >= DROP_N)
        return;
    const float ang = spat_rand() * 6.2831853f, rad = DROP_REACH * sqrtf(spat_rand());
    float p[3] = { cam[0] + cosf(ang) * rad, cam[1] - 6.0f - 4.0f * spat_rand(), cam[2] + sinf(ang) * rad };
    float dy = 0.0f;
    float land = p[1] + 14.0f;
    if (grass_floor_near(p, 40.0f, &dy) >= 0 && dy > 0.5f)
        land = p[1] + dy;
    const float roof = cover_at(p[0], p[2], cam[1], 1);
    if (roof < COVER_SKY && p[1] > roof - COVER_EPS)
        return; /* born under a roof (a cave's ceiling lower than the drops' height): no drop there */
    if (roof < COVER_SKY && roof > p[1] && roof < land)
        land = roof;
    Drop* d = &g_drop[g_drop_n++];
    memcpy(d->p, p, sizeof d->p);
    d->v[0] = wind * 2.5f, d->v[1] = 11.0f + 3.0f * spat_rand(), d->v[2] = 0.0f;
    d->land = land, d->k = splash_at(rad);
}

/* a drop of the middle ring: anywhere from the near reach out to mid_reach(), even in the distance (so thinning as 1/r), 6 to
 * 14 yalms above the camera, falling to the floor under it with its splash (splash_at); none with no floor found under it (it
 * falls on through the ground, hidden by it) */
static void mid_spawn(const float cam[3], float wind)
{
    if (g_mid_n >= MID_N)
        return;
    const float r0 = DROP_REACH, r1 = mid_reach();
    const float ang = spat_rand() * 6.2831853f, rad = r0 + (r1 - r0) * spat_rand();
    const float p[3] = { cam[0] + cosf(ang) * rad, cam[1] - 6.0f - 8.0f * spat_rand(), cam[2] + sinf(ang) * rad };
    float dy = 0.0f, land = p[1] + 24.0f, k = 0.0f;
    if (grass_floor_near(p, 40.0f, &dy) >= 0 && dy > 0.5f)
        land = p[1] + dy, k = splash_at(rad);
    const float roof = cover_at(p[0], p[2], cam[1], 0);
    if (roof < COVER_SKY && p[1] > roof - COVER_EPS)
        return;
    if (roof < COVER_SKY && roof > p[1] && roof < land)
        land = roof;
    Drop* d = &g_mid[g_mid_n++];
    memcpy(d->p, p, sizeof d->p);
    d->v[0] = wind * 2.5f, d->v[1] = 11.0f + 3.0f * spat_rand(), d->v[2] = 0.0f;
    d->land = land, d->k = k;
}

/* a far drop spawned in the ring between mid_reach() and FAR_REACH, even in the distance's log (so thinning as 1/r squared), a
 * fifth fewer each 100 yalms out (rain_band); 6 to 20 yalms over the ground the far grid has there, landing on it with its
 * splash (splash_at); none where the grid knows no ground yet, none under a roof (cover_at: the map, else the grid's) */
static void far_spawn(const float cam[3], float wind)
{
    if (g_far_n >= FAR_N)
        return;
    const float r0 = mid_reach(), r1 = FAR_REACH;
    const float ang = spat_rand() * 6.2831853f, rad = r0 * powf(r1 / r0, spat_rand());
    if (spat_rand() > rain_band(rad))
        return;
    float gy = 0.0f;
    const float x = cam[0] + cosf(ang) * rad, z = cam[2] + sinf(ang) * rad;
    const int ground = rain_ground(x, z, &gy);
    if (!ground)
        return; /* not laid yet, or no floor there: nothing known of a roof over it either (Norg's far rain) */
    const float p[3] = { x, gy - 6.0f - 14.0f * spat_rand(), z };
    const float roof = cover_at(p[0], p[2], cam[1], 0);
    if (roof < COVER_SKY && p[1] > roof - COVER_EPS)
        return;
    float land = gy;
    if (roof < COVER_SKY && roof > p[1] && roof < land)
        land = roof;
    Drop* d = &g_far[g_far_n++];
    memcpy(d->p, p, sizeof d->p);
    d->v[0] = wind * 2.5f, d->v[1] = 11.0f + 3.0f * spat_rand(), d->v[2] = 0.0f;
    d->land = land, d->k = splash_at(rad);
}

/* a pool of drops moved on by dt: each falls, and goes when it lands (splashing, when `splash`), leaves the reach, or has drifted
 * in under a roof (the map at where it is now, not only where it was born) */
static void drops_move(Drop* pool, uint32_t* count, float dt, const float cam[3], float reach, float wind, int splash)
{
    for (uint32_t i = 0; i < *count;)
    {
        Drop* d = &pool[i];
        for (int a = 0; a < 3; ++a)
            d->p[a] += d->v[a] * dt;
        const float dx = d->p[0] - cam[0], dz = d->p[2] - cam[2];
        const float roof = cover_at(d->p[0], d->p[2], cam[1], 0);
        const int landed = d->p[1] >= d->land;
        if (landed || dx * dx + dz * dz > reach * reach || (roof < COVER_SKY && d->p[1] > roof + COVER_EPS))
        {
            if (landed && splash && !g_rain_indoor && d->k > 0.0f && g_rain_set.splash > 0.0f)
            {
                const float at[3] = { d->p[0], d->land, d->p[2] };
                drop_land(at, wind, d->k, sqrtf(dx * dx + dz * dz));
            }
            pool[i] = pool[--*count];
            continue;
        }
        ++i;
    }
}

/* the pools moved on by dt (the world's y points down): the drops fall and land, the droplets fall and die, the rings fade;
 * new drops and the characters' spatter spawn by the rain's heaviness and rain.splash */
static void rain_particles_update(float dt, const float cam[3], float wind)
{
    const float over = cover_at(cam[0], cam[2], cam[1], 1);
    g_rain_indoor = over < COVER_SKY && cam[1] > over + COVER_EPS;
    drops_move(g_drop, &g_drop_n, dt, cam, DROP_REACH + 6.0f, wind, 1);
    drops_move(g_mid, &g_mid_n, dt, cam, mid_reach() + 6.0f, wind, 1);
    drops_move(g_far, &g_far_n, dt, cam, FAR_REACH + 6.0f, wind, 1);
    rain_ground_update(cam, FAR_REACH);
    for (uint32_t i = 0; i < g_spat_n;)
    {
        Spat* d = &g_spat[i];
        d->life += dt;
        if (d->life >= d->span)
        {
            g_spat[i] = g_spat[--g_spat_n];
            continue;
        }
        d->v[1] += 9.0f * dt;
        for (int a = 0; a < 3; ++a)
            d->p[a] += d->v[a] * dt;
        ++i;
    }
    for (uint32_t i = 0; i < g_ring_n;)
    {
        g_ring[i].life += dt;
        if (g_ring[i].life >= g_ring[i].span)
            g_ring[i] = g_ring[--g_ring_n];
        else
            ++i;
    }
    /* the drops: enough in the air that the pool's share by the density is kept, each in the air about 0.7 s */
    const float area = (DROP_REACH / 16.0f) * (DROP_REACH / 16.0f);
    const float want_n = fminf(DROP_BASE * area * saturate_f(g_rain_fall * g_rain_set.density * 0.6f), (float)DROP_N);
    float spawn = (want_n / 0.7f) * dt;
    while (spawn > 0.0f && (float)g_drop_n < want_n)
    {
        if (spat_rand() < spawn)
            drop_spawn(cam, wind);
        spawn -= 1.0f;
    }
    const float want_mid = (float)MID_N * saturate_f(g_rain_fall * g_rain_set.density * 0.6f); /* each in the air about 1 s */
    for (float f = want_mid * dt; f > 0.0f && (float)g_mid_n < want_mid; f -= 1.0f)
        if (spat_rand() < f)
            mid_spawn(cam, wind);
    /* each in the air about a second; as many to a stretch of the distance's log as from 48 to 150 yalms (FAR_BASE) */
    const float far_k = logf(FAR_REACH / mid_reach()) / logf(150.0f / 48.0f);
    const float want_far = fminf(FAR_BASE * far_k, (float)FAR_N) * saturate_f(g_rain_fall * g_rain_set.density * 0.6f);
    for (float f = (want_far / 1.2f) * dt; f > 0.0f && (float)g_far_n < want_far; f -= 1.0f)
        if (spat_rand() < f)
            far_spawn(cam, wind);
    const float rate = g_rain_set.spatter > 0.0f ? 11.0f * g_rain_fall * g_rain_set.splash : 0.0f; /* hits a second a character, four droplets each */
    for (unsigned i = 0; i < g_nchars; ++i)
    {
        const float* c = g_chars[i];
        if (c[3] < 0.0f)
            continue; /* a fixture (a character whose hitbox isn't known is 0) */
        const float dx = c[0] - cam[0], dz = c[2] - cam[2];
        if (dx * dx + dz * dz > SPAT_REACH * SPAT_REACH)
            continue;
        const float roof = cover_at(c[0], c[2], cam[1], 1);
        if (roof < COVER_SKY && c[1] > roof + COVER_EPS)
            continue; /* under a roof: no drops break on it */
        float want = rate * dt;
        while (want > 0.0f)
        {
            if (spat_rand() < want)
                spat_spawn(c, wind);
            want -= 1.0f;
        }
    }
}

/* the vertices: a fan of `seg` triangles round a centre (opaque) with a clear rim stretched `along` and `across`; v gets
 * seg * 3 vertices of x, y, z, colour */
static void fan_emit(float* v, const float c[3], const float along[3], const float across[3], int seg, uint32_t cc, uint32_t cr)
{
    for (int k = 0; k < seg; ++k)
    {
        const float a0 = (float)k * 6.2831853f / (float)seg, a1 = (float)(k + 1) * 6.2831853f / (float)seg;
        float* o = &v[k * 12];
        for (int a = 0; a < 3; ++a)
        {
            o[a] = c[a];
            o[4 + a] = c[a] + cosf(a0) * along[a] + sinf(a0) * across[a];
            o[8 + a] = c[a] + cosf(a1) * along[a] + sinf(a1) * across[a];
        }
        memcpy(&o[3], &cc, 4), memcpy(&o[7], &cr, 4), memcpy(&o[11], &cr, 4);
    }
}

/* a drop as a thin quad along its motion, facing the camera: the head end opaque (ch), the tail clear (ct); v gets 6 vertices */
static int quad_emit(float* v, const float p[3], const float vel[3], float w, float len, const float eye[3], uint32_t ch, uint32_t ct)
{
    const float to[3] = { p[0] - eye[0], p[1] - eye[1], p[2] - eye[2] };
    const float dist = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (dist < 0.3f)
        return 0;
    const float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    const float along[3] = { vel[0] / fmaxf(sp, 1e-3f), vel[1] / fmaxf(sp, 1e-3f), vel[2] / fmaxf(sp, 1e-3f) };
    const float across[3] = { to[1] * along[2] - to[2] * along[1], to[2] * along[0] - to[0] * along[2], to[0] * along[1] - to[1] * along[0] };
    const float al = sqrtf(across[0] * across[0] + across[1] * across[1] + across[2] * across[2]);
    if (al < 1e-4f)
        return 0;
    const float ww = w * (1.0f + dist * 0.03f) / al;
    float h[3], t[3], a[3];
    for (int i = 0; i < 3; ++i)
        h[i] = p[i] + along[i] * len * 0.5f, t[i] = p[i] - along[i] * len * 0.5f, a[i] = across[i] * ww;
    const float* c[6] = { h, h, t, h, t, t };
    const float sgn[6] = { -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
    const uint32_t col[6] = { ch, ch, ct, ch, ct, ct };
    for (int k = 0; k < 6; ++k)
    {
        float* o = &v[k * 4];
        for (int i = 0; i < 3; ++i)
            o[i] = c[k][i] + sgn[k] * a[i];
        memcpy(&o[3], &col[k], 4);
    }
    return 1;
}

/* a moving particle's fan: facing the camera, stretched along its motion; 0 when it's out of reach */
static int streak_emit(float* v, const float p[3], const float vel[3], float size, float stretch, int seg, const float eye[3], uint32_t cc, uint32_t cr)
{
    float to[3] = { p[0] - eye[0], p[1] - eye[1], p[2] - eye[2] };
    const float dist = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (dist < 0.3f)
        return 0;
    to[0] /= dist, to[1] /= dist, to[2] /= dist;
    const float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    float along[3] = { vel[0] / fmaxf(sp, 1e-3f), vel[1] / fmaxf(sp, 1e-3f), vel[2] / fmaxf(sp, 1e-3f) };
    float across[3] = { to[1] * along[2] - to[2] * along[1], to[2] * along[0] - to[0] * along[2], to[0] * along[1] - to[1] * along[0] };
    const float al = sqrtf(across[0] * across[0] + across[1] * across[1] + across[2] * across[2]);
    if (al < 1e-4f)
        return 0;
    const float w = size * (1.0f + dist * 0.03f); /* never under a pixel far off */
    for (int a = 0; a < 3; ++a)
        along[a] *= stretch, across[a] *= w / al;
    fan_emit(v, p, along, across, seg, cc, cr);
    return 1;
}

static uint32_t rain_colour(const float drop[3], float alpha)
{
    const uint32_t ca = (uint32_t)(255.0f * saturate_f(alpha));
    if (g_rain_set.view == 1) /* /vana debug's Rain: red over the picture in grey (emit_rain_fs) */
        return ((uint32_t)(255.0f * saturate_f(alpha * 1.5f)) << 24) | 0xFF0000u;
    if (g_rain_set.force == 4) /* the diagnostics: red, plain */
        return (ca << 24) | 0xFF2020u;
    return (ca << 24) | ((uint32_t)(drop[0] * 255.0f) << 16) | ((uint32_t)(drop[1] * 255.0f) << 8) | (uint32_t)(drop[2] * 255.0f);
}

/* one blended draw of the particles' vertices v[0..n): the pass's depth copy and its projection values (pass: stage 7, matrix 6)
 * when `soft` - the splashes and the rings (fs.rain 2, gfx_hlsl_rain.h emit_rain_soft) shown only within half a yalm in front
 * of what the scene shows there: the drops land on the zone's collision, which isn't always what's drawn (the harbour's,
 * a tower's ledges, the zone's invisible floors), and their splashes hung in the air over the sea and on walls */
static void particles_draw(const GfxScene* s, const float* v, uint32_t n, const GfxDraw* soft)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    static GfxDraw d;
    memset(&d, 0, sizeof d);
    d.vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d.u.offset[GFX_R_DIFFUSE] = 12;
    d.u.stride[0] = 16;
    d.data[0] = (void*)v, d.size[0] = n * 16;
    d.prim = GFX_TRIANGLELIST, d.count = n / 3;
    d.fs.nstages = 1;
    d.fs.st[0] = (GfxStage){ 2 /* SELECTARG1 */, 0 /* DIFFUSE */, 0, 1, 2, 0, 0, 1, 1, 0, 0, 2 };
    if (soft && soft->tex[7])
    {
        d.fs.rain = 2;
        d.tex[7] = soft->tex[7], d.samp[7] = soft->samp[7], d.fs.st[7] = soft->fs.st[7];
        memcpy(d.u.texm[6], soft->u.texm[6], 64);
    }
    if (s->fog[2] != 0.0f)
        d.fs.fog = 3, d.u.params[2] = s->fog[0], d.u.params[3] = s->fog[1];
    memcpy(d.u.fogcolor, s->fogcolor, sizeof d.u.fogcolor);
    for (int i = 0; i < 4; ++i)
        d.u.vp[i] = (float)s->vp[i], d.u.mat_d[i] = d.u.tfactor[i] = 1.0f;
    memcpy(d.vp, s->vp, sizeof d.vp);
    mat_mul(d.u.wvp, s->view, s->proj);
    memcpy(d.u.wv, s->view, 64);
    memcpy(d.u.wvit, I, 64);
    d.pipe.write_mask = 0xF;
    d.pipe.blend = 1, d.pipe.src = 5, d.pipe.dst = 6, d.pipe.op = 1; /* SRCALPHA, INVSRCALPHA, ADD */
    d.depth.zenable = 1, d.depth.zwrite = 0, d.depth.zfunc = 4;
    d.cull = 1, d.fill = 3;
    gfx_draw(&d);
}

/* a droplet's dot, stretched along its motion: 18 vertices at v, or none */
static uint32_t spat_emit(float* v, const Spat* d, const float drop[3], float big, const float eye[3])
{
    const float fade = 1.0f - d->life / d->span;
    const float sp = sqrtf(d->v[0] * d->v[0] + d->v[1] * d->v[1] + d->v[2] * d->v[2]);
    const float lit[3] = { drop[0] * 0.35f + 0.65f, drop[1] * 0.35f + 0.65f, drop[2] * 0.35f + 0.65f }; /* catches the sky */
    const uint32_t cc = rain_colour(lit, fade), cr = rain_colour(lit, 0.0f);
    return 18 * (uint32_t)streak_emit(v, d->p, d->v, d->size * big, d->size * big * (1.2f + sp * 0.45f), 6, eye, cc, cr);
}

/* the particles drawn in two blended draws: the drops as thin streaks (a fan of four), the droplets as soft dots stretched along
 * their motion (six), the rings as flat hexagonal rings on the ground spreading and fading */
#define PART_FLOATS ((DROP_N + MID_N + FAR_N) * 6 * 4 + SPAT_N * 18 * 4 + RING_N * RING_SEG * 6 * 4)
static void rain_particles_draw(const GfxScene* s, const float* inv, const float drop[3], const GfxDraw* pass)
{
    static float v[PART_FLOATS];
    const float eye[3] = { inv[12], inv[13], inv[14] };
    const float big = g_rain_set.force == 4 ? 4.0f : 1.0f;
    uint32_t n = 0; /* vertices */
    for (uint32_t i = 0; i < g_drop_n && n + 6 <= PART_FLOATS / 4; ++i)
    {
        const Drop* d = &g_drop[i];
        const float dx = d->p[0] - eye[0], dy = d->p[1] - eye[1], dz = d->p[2] - eye[2];
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        const float nearf = saturate_f((dist - 0.5f) / 1.5f); /* a drop at the camera fades out, never a bar across the screen */
        const uint32_t cc = rain_colour(drop, 0.85f * nearf), cr = rain_colour(drop, 0.0f);
        n += 6 * (uint32_t)quad_emit(&v[n * 4], d->p, d->v, 0.007f * big, fminf(0.5f, 0.15f * dist), eye, cc, cr);
    }
    for (uint32_t i = 0; i < g_mid_n && n + 6 <= PART_FLOATS / 4; ++i) /* the middle ring's: as the near ones, a little fainter */
    {
        const Drop* d = &g_mid[i];
        const float dx = d->p[0] - eye[0], dy = d->p[1] - eye[1], dz = d->p[2] - eye[2];
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        const uint32_t cc = rain_colour(drop, 0.7f), cr = rain_colour(drop, 0.0f);
        n += 6 * (uint32_t)quad_emit(&v[n * 4], d->p, d->v, 0.008f * big, fminf(0.9f, 0.04f * dist), eye, cc, cr);
    }
    const float fade0 = FAR_REACH * 0.85f, fade1 = FAR_REACH * 0.15f;
    for (uint32_t i = 0; i < g_far_n && n + 6 <= PART_FLOATS / 4; ++i) /* the far ones: longer, about a pixel wide, fading out at the reach */
    {
        const Drop* d = &g_far[i];
        const float dx = d->p[0] - eye[0], dy = d->p[1] - eye[1], dz = d->p[2] - eye[2];
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        const float vs = sqrtf(d->v[0] * d->v[0] + d->v[1] * d->v[1] + d->v[2] * d->v[2]) * fmaxf(dist, 1e-3f);
        const float cx = dy * d->v[2] - dz * d->v[1], cy = dz * d->v[0] - dx * d->v[2], cz = dx * d->v[1] - dy * d->v[0];
        const float side = sqrtf(cx * cx + cy * cy + cz * cz) / fmaxf(vs, 1e-3f); /* 1 seen across its fall, 0 end on: a dot, speckle */
        const float a = 0.6f * (1.0f - saturate_f((dist - fade0) / fade1)) * saturate_f(side * 2.0f - 0.2f);
        const uint32_t cc = rain_colour(drop, a), cr = rain_colour(drop, 0.0f);
        /* about a pixel wide however far: quad_emit grows the width with the distance itself (some 0.8 to 1.3 pixels from 50 to
         * 500 yalms at 1500 high). Grown here as well, by dist / 50, it was 3 to 5 pixels wide out past 300 yalms: chunky */
        n += 6 * (uint32_t)quad_emit(&v[n * 4], d->p, d->v, 0.02f * big, fminf(3.0f, 0.04f * dist), eye, cc, cr);
    }
    for (uint32_t i = 0; i < g_spat_n && n + 18 <= PART_FLOATS / 4; ++i) /* the characters' spatter */
        if (!g_spat[i].ground)
            n += spat_emit(&v[n * 4], &g_spat[i], drop, big, eye);
    const uint32_t n1 = n; /* the drops and the characters' spatter before, the ground's splashes and rings after */
    for (uint32_t i = 0; i < g_spat_n && n + 18 <= PART_FLOATS / 4; ++i)
        if (g_spat[i].ground)
            n += spat_emit(&v[n * 4], &g_spat[i], drop, big, eye);
    for (uint32_t i = 0; i < g_ring_n && n + RING_SEG * 6 <= PART_FLOATS / 4; ++i)
    {
        const Ring* r = &g_ring[i];
        const float t = r->life / r->span, rad = (0.03f + 0.12f * t) * big * r->s;
        const uint32_t cc = rain_colour(drop, r->a * (1.0f - t)), cr = rain_colour(drop, 0.0f);
        float* o = &v[n * 4];
        for (int k = 0; k < RING_SEG; ++k) /* a band between the rim and 0.75 of it: two triangles a side, the rim's edge opaque */
        {
            const float a0 = (float)k * 6.2831853f / (float)RING_SEG, a1 = (float)(k + 1) * 6.2831853f / (float)RING_SEG;
            const float q[4][3] = { { r->p[0] + cosf(a0) * rad, r->p[1] - 0.01f, r->p[2] + sinf(a0) * rad }, { r->p[0] + cosf(a1) * rad, r->p[1] - 0.01f, r->p[2] + sinf(a1) * rad },
                                    { r->p[0] + cosf(a0) * rad * 0.75f, r->p[1] - 0.01f, r->p[2] + sinf(a0) * rad * 0.75f }, { r->p[0] + cosf(a1) * rad * 0.75f, r->p[1] - 0.01f, r->p[2] + sinf(a1) * rad * 0.75f } };
            const int tri[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
            for (int t2 = 0; t2 < 2; ++t2)
                for (int c = 0; c < 3; ++c)
                {
                    float* w = &o[(k * 6 + t2 * 3 + c) * 4];
                    memcpy(w, q[tri[t2][c]], 12);
                    const uint32_t col = tri[t2][c] < 2 ? cc : cr; /* the rim's edge opaque, the inner clear */
                    memcpy(&w[3], &col, 4);
                }
        }
        n += RING_SEG * 6;
    }
    if (n1)
        particles_draw(s, v, n1, NULL);
    if (n > n1)
        particles_draw(s, &v[n1 * 4], n - n1, pass);
}
