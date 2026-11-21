/* The far rain's ground (d3d8_rain.h far_spawn): the height of the top of the zone's floors every 4 yalms out to the rain's
 * reach, kept in 32-yalm squares (grass_floor_tops) round the camera, a few laid a frame, the nearest missing first, so the
 * far drops fall over the hills and roofs they're seen against and land on them. Its own squares, not the grass's 64 laid
 * round the player: asked at 500 yalms those were laid and thrown away each drop. With each the shelter's roof there
 * (cover_roof), for cover_at past its map's 48 yalms: the far rain fell through Norg's ceiling. Included in d3d8_rain.h. */
#define RG_SIDE 36 /* squares a side kept: 17 each way round the camera's, the 500 yalms at 32 a square */
#define RG_LAY 3   /* squares laid a frame, at most */
typedef struct RgTile
{
    int tx, tz, ok;
    float top[GRASS_FLOOR_CELLS * GRASS_FLOOR_CELLS];
    float roof[GRASS_FLOOR_CELLS * GRASS_FLOOR_CELLS]; /* the roof over the ground (COVER_SKY: open; the ground is the roof's top) */
} RgTile;
static RgTile g_rg[RG_SIDE * RG_SIDE];
static int g_rg_zone = -1, g_rg_gen = -1;

static RgTile* rg_slot(int tx, int tz)
{
    return &g_rg[(((tz % RG_SIDE) + RG_SIDE) % RG_SIDE) * RG_SIDE + ((tx % RG_SIDE) + RG_SIDE) % RG_SIDE];
}

/* the ground's height (the world's y, down) at x, z into *y; 0 with none known there (not laid yet, no floor) */
static int rain_ground(float x, float z, float* y)
{
    const int tx = (int)floorf(x / GRASS_FLOOR_TILE), tz = (int)floorf(z / GRASS_FLOOR_TILE);
    const RgTile* t = rg_slot(tx, tz);
    if (!t->ok || t->tx != tx || t->tz != tz)
        return 0;
    const float per = (float)GRASS_FLOOR_CELLS / GRASS_FLOOR_TILE;
    const int cx = (int)fminf(fmaxf(floorf((x - (float)tx * GRASS_FLOOR_TILE) * per), 0.0f), GRASS_FLOOR_CELLS - 1.0f);
    const int cz = (int)fminf(fmaxf(floorf((z - (float)tz * GRASS_FLOOR_TILE) * per), 0.0f), GRASS_FLOOR_CELLS - 1.0f);
    const float v = t->top[cz * GRASS_FLOOR_CELLS + cx];
    if (!(v < 1e29f))
        return 0;
    *y = v;
    return 1;
}

/* the shelter's roof over x, z as the grid has it: COVER_SKY with none, or the square not laid */
static float rain_ground_roof(float x, float z)
{
    const int tx = (int)floorf(x / GRASS_FLOOR_TILE), tz = (int)floorf(z / GRASS_FLOOR_TILE);
    const RgTile* t = rg_slot(tx, tz);
    if (!t->ok || t->tx != tx || t->tz != tz)
        return COVER_SKY;
    const float per = (float)GRASS_FLOOR_CELLS / GRASS_FLOOR_TILE;
    const int cx = (int)fminf(fmaxf(floorf((x - (float)tx * GRASS_FLOOR_TILE) * per), 0.0f), GRASS_FLOOR_CELLS - 1.0f);
    const int cz = (int)fminf(fmaxf(floorf((z - (float)tz * GRASS_FLOOR_TILE) * per), 0.0f), GRASS_FLOOR_CELLS - 1.0f);
    return t->roof[cz * GRASS_FLOOR_CELLS + cx];
}

/* square t's roofs: the shelter's cast at each cell's middle, kept where the ground lies under it (a roof that is itself the
 * top floor is ground the rain lands on) */
static void rg_roofs(RgTile* t, float ref)
{
    const float cell = GRASS_FLOOR_TILE / GRASS_FLOOR_CELLS;
    for (int c = 0; c < GRASS_FLOOR_CELLS * GRASS_FLOOR_CELLS; ++c)
    {
        t->roof[c] = COVER_SKY;
        if (!g_cover.ntri)
            continue;
        const float x = (float)t->tx * GRASS_FLOOR_TILE + ((float)(c % GRASS_FLOOR_CELLS) + 0.5f) * cell;
        const float z = (float)t->tz * GRASS_FLOOR_TILE + ((float)(c / GRASS_FLOOR_CELLS) + 0.5f) * cell;
        const float top = t->top[c];
        const float roof = cover_roof(x, z, top < 1e29f ? top : ref);
        if (roof < COVER_SKY && (!(top < 1e29f) || top > roof + COVER_EPS))
            t->roof[c] = roof;
    }
}

/* the squares out to reach round the camera: RG_LAY laid a frame, ring by ring out from the camera's; all forgotten on a new
 * zone (or the floors not known) */
static void rain_ground_update(const float cam[3], float reach)
{
    float probe[GRASS_FLOOR_CELLS * GRASS_FLOOR_CELLS];
    const int zone = grass_floor_tops(0x7FFFFF, 0x7FFFFF, probe) ? g_rain_zone : 0; /* a square off the map: only whether they're known */
    if (zone != g_rg_zone || g_cover.gen != g_rg_gen) /* a new zone, or its shelter laid again (its meshes come in later) */
    {
        for (int i = 0; i < RG_SIDE * RG_SIDE; ++i)
            g_rg[i].ok = 0;
        g_rg_zone = zone, g_rg_gen = g_cover.gen;
    }
    if (!zone)
        return;
    const int ctx = (int)floorf(cam[0] / GRASS_FLOOR_TILE), ctz = (int)floorf(cam[2] / GRASS_FLOOR_TILE);
    const int most = (int)fminf(ceilf(reach / GRASS_FLOOR_TILE) + 1.0f, RG_SIDE / 2 - 1);
    int laid = 0;
    for (int r = 0; r <= most && laid < RG_LAY; ++r)
        for (int dz = -r; dz <= r && laid < RG_LAY; ++dz)
            for (int dx = -r; dx <= r && laid < RG_LAY; dx += (dz == -r || dz == r) ? 1 : 2 * r)
            {
                RgTile* t = rg_slot(ctx + dx, ctz + dz);
                if (t->ok && t->tx == ctx + dx && t->tz == ctz + dz)
                    continue;
                t->tx = ctx + dx, t->tz = ctz + dz;
                t->ok = grass_floor_tops(t->tx, t->tz, t->top);
                if (t->ok)
                    rg_roofs(t, cam[1]);
                ++laid;
            }
}
