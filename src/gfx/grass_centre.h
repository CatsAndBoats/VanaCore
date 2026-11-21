/* The grass round the free camera (grass.freecam): grass_set_centre and centre_ground, the ground under the camera's eye
 * that grass_scene puts in the scene's focus. grass.c includes this just after floor_ready, after g, gf, FLOOR_TILE,
 * FLOOR_CELLS, FLOOR_NEAR, floor_tile and SAY, whose names it uses. It is no file of its own. */
#pragma once

/* --- round the free camera (grass_set_centre) ----------------------------------------------------------------
 * With grass.freecam the tufts grow round the free camera, not the player: round the ground under its eye, so the vertex function's
 * height band (the ground near the centre's height, and 0.3 of the distance) holds the ground it looks down on, not the air it
 * flies in. The ground under it is the nearest floor of the zone's collision there (floor_under), else the nearest of the grass's
 * own ground drawn lately (inst_under), else the last found in this zone, else the eye's own height. */
static float g_centre[4]; /* the eye (the world, y down); w = 1 while it's given */
static int g_centre_zone; /* the zone the last ground under it was found in (0: none yet) */
static float g_centre_y;  /* ... and its height */

void grass_set_centre(float x, float y, float z, int known)
{
    if ((g_centre[3] != 0.0f) != (known != 0))
        SAY("[recomp] grass: %s\n", known ? "growing round the free camera (grass.freecam)" : "growing round the player again");
    g_centre[0] = x, g_centre[1] = y, g_centre[2] = z, g_centre[3] = known ? 1.0f : 0.0f;
}

/* the height of the nearest floor under p (the world, y down; FLOOR_NEAR over it at most) in *y: 0 for none there, or the zone's
 * floors not known */
static int floor_under(const float p[3], float* y)
{
    if (gf.zone != g.zone || !gf.npairs)
        return 0;
    const int32_t tx = (int32_t)floorf(p[0] / FLOOR_TILE), tz = (int32_t)floorf(p[2] / FLOOR_TILE);
    const FTile* t = floor_tile(tx, tz);
    if (!t->list)
        return 0;
    const float per = FLOOR_CELLS / FLOOR_TILE;
    const int cx = (int)clampf(floorf((p[0] - (float)tx * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int cz = (int)clampf(floorf((p[2] - (float)tz * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int c = cz * FLOOR_CELLS + cx;
    float best = 1e30f;
    for (uint32_t k = t->cell[c]; k < t->cell[c + 1]; ++k)
    {
        const FTri* o = &t->tri[t->list[k]];
        const float qx = p[0] - o->ax, qz = p[2] - o->az;
        const float u = (qx * o->e2z - o->e2x * qz) * o->inv, w = (o->e1x * qz - qx * o->e1z) * o->inv;
        if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
            continue;
        const float d = o->y0 + u * o->dy1 + w * o->dy2 - p[1]; /* above 0: the floor is below p */
        if (d > -FLOOR_NEAR && d < best)
            best = d;
    }
    if (best >= 1e29f)
        return 0;
    *y = p[1] + best;
    return 1;
}

/* the height of the nearest of the grass's ground drawn lately (the pieces kept_draw draws from) under p, FLOOR_NEAR over it at most,
 * in *y: 0 for none */
static int inst_under(const float p[3], float* y)
{
    float best = 1e30f;
    for (uint32_t i = 0; i < g.ninst; ++i)
    {
        const GInst* in = &g.inst[i];
        const float dx = in->ctr[0] - p[0], dz = in->ctr[2] - p[2];
        if (g.scene - in->seen > 12 || !in->n || in->moving || dx * dx + dz * dz > in->rad * in->rad)
            continue;
        for (uint32_t k = 0; k < in->n; ++k)
        {
            const GWTri* tri = &g.wtris[in->first + k];
            const float ax = tri->p[1][0] - tri->p[0][0], az = tri->p[1][2] - tri->p[0][2];
            const float bx = tri->p[2][0] - tri->p[0][0], bz = tri->p[2][2] - tri->p[0][2];
            const float den = ax * bz - bx * az;
            if (!(fabsf(den) > 1e-9f))
                continue;
            const float qx = p[0] - tri->p[0][0], qz = p[2] - tri->p[0][2];
            const float u = (qx * bz - bx * qz) / den, w = (ax * qz - qx * az) / den;
            if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
                continue;
            const float d = tri->p[0][1] + u * (tri->p[1][1] - tri->p[0][1]) + w * (tri->p[2][1] - tri->p[0][1]) - p[1];
            if (d > -FLOOR_NEAR && d < best)
                best = d;
        }
    }
    if (best >= 1e29f)
        return 0;
    *y = p[1] + best;
    return 1;
}

/* the place the tufts grow round, for the scene's focus: the ground under the free camera's eye (w 1) */
static void centre_ground(float out[4])
{
    float y;
    if (floor_under(g_centre, &y) || inst_under(g_centre, &y))
        g_centre_zone = g.zone, g_centre_y = y;
    else
        y = g_centre_zone && g_centre_zone == g.zone ? g_centre_y : g_centre[1];
    out[0] = g_centre[0], out[1] = y, out[2] = g_centre[2], out[3] = 1.0f;
}
