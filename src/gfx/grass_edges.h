/* grass_edges.h: included by grass.c (its statics: g, gf, the floors' squares, g_build), just before build_step. Not a header of
 * its own: it's grass.c, kept apart for its length. */

/* --- shorter near the grass's edge (grass.edge.fade, grass.edge.height) ----------------------------------------------------------
 * Josh 2026-11-22, La Theine Plateau (zone 102, Debug view 26): the grass stopped at its full height in a line along the hills' feet.
 * The rules that end it (too steep, not grass in its texture, no floor to stand on) each cut it off where they say, and a hill drawn as
 * a mesh of its own is no part of the slope the grass's own mesh smooths across (smooth_corners). With grass.edge.fade (yalms, 0 to
 * 10, 0 off) a tuft that near where the grass ends grows shorter, to grass.edge.height of itself at the edge; and climbing, it
 * shortens from half grass.slope to that at grass.slope. Where the grass ends, by:
 *  - the zone's floors (its collision, the whole zone in the world, whatever mesh draws it): walking out over them, a floor steeper
 *    than grass.slope, none within a climb of the last (a drop, a wall, a cliff's top far over it), or a hard floor (an object's,
 *    stone, metal, wood) after soft ground (path, grass, sand, snow, shallow water). Found at the points of a lattice laid on the
 *    world (edge_space apart: a quarter of the fade, 0.75 at the least), each once (kept by its place), and a tuft takes the four
 *    round it, blended: walked from every tuft, a fade of 10 yalms cost seconds of filling in;
 *  - its texture: ground that isn't grass in the triangle's picture carried past its edges (path_near's way);
 *  - the brink of a drop in its own mesh (find_edges, brink: room). */
#define EDGE_CACHE 16384 /* the lattice's points kept, by place (a power of two) */
#define EDGE_RAYS 8
#define EDGE_BAND 4.0f /* the lattice's points kept apart by height, in bands this tall: a floor over another is another point */

static const float g_ray[EDGE_RAYS][2] = { { 1.0f, 0.0f }, { 0.7071068f, 0.7071068f }, { 0.0f, 1.0f }, { -0.7071068f, 0.7071068f },
    { -1.0f, 0.0f }, { -0.7071068f, -0.7071068f }, { 0.0f, -1.0f }, { 0.7071068f, -0.7071068f } };

static struct
{
    uint64_t key[EDGE_CACHE]; /* a lattice point: its place and height band */
    float d[EDGE_CACHE];      /* how far from it the floors end (edge_floors), to the fade */
    uint32_t tag[EDGE_CACHE]; /* what it was found with (edge_tag): another's is no answer */
    const FTile* last;        /* the floors' square looked in last */
} g_edge;

/* the floor at (x, z) nearest height y (the world, y down), within win of it: 1, its height *fy, how upright *up (1 flat) and its
 * ground *terrain; 0 none that near, though there are floors over or under it there (a drop, a cliff's top); -1 none there at all
 * (ground out of reach) */
static int edge_floor(float x, float z, float y, float win, float* fy, float* up, int* terrain)
{
    const int32_t tx = (int32_t)floorf(x / FLOOR_TILE), tz = (int32_t)floorf(z / FLOOR_TILE);
    const FTile* t = g_edge.last;
    if (!t || !t->used || t->tx != tx || t->tz != tz)
        t = g_edge.last = floor_tile(tx, tz);
    if (!t->list)
        return -1;
    const float per = FLOOR_CELLS / FLOOR_TILE;
    const int cx = (int)clampf(floorf((x - (float)tx * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int cz = (int)clampf(floorf((z - (float)tz * FLOOR_TILE) * per), 0.0f, FLOOR_CELLS - 1.0f);
    const int c = cz * FLOOR_CELLS + cx;
    const FTri* best = NULL;
    int any = 0;
    for (uint32_t k = t->cell[c]; k < t->cell[c + 1]; ++k)
    {
        const FTri* o = &t->tri[t->list[k]];
        const float qx = x - o->ax, qz = z - o->az;
        const float u = (qx * o->e2z - o->e2x * qz) * o->inv, w = (o->e1x * qz - qx * o->e1z) * o->inv;
        if (u < -1e-4f || w < -1e-4f || u + w > 1.0001f)
            continue;
        const float at = o->y0 + u * o->dy1 + w * o->dy2;
        any = 1;
        if (fabsf(at - y) <= win)
            win = fabsf(at - y), best = o, *fy = at;
    }
    if (!best)
        return any ? 0 : -1;
    const float nx = best->dy1 * best->e2z - best->e1z * best->dy2, ny = best->e1z * best->e2x - best->e1x * best->e2z;
    const float nz = best->e1x * best->dy2 - best->dy1 * best->e2x, len = sqrtf(nx * nx + ny * ny + nz * nz);
    *up = len > 1e-12f ? fabsf(ny) / len : 0.0f;
    *terrain = best->terrain;
    return 1;
}

/* a floor no grass ends on softly: an object's (a rock, a crate), stone, metal, wood */
static int edge_hard(int terrain) { return terrain == 0 || (terrain >= 5 && terrain <= 7); }

/* grass.slope's rise over a run of 1 */
static float edge_rise(void) { return sqrtf(maxf(1.0f - g.slope_cos * g.slope_cos, 0.0f)) / g.slope_cos; }

/* how far from p (the world) the grass ends by the zone's floors: its own floor the one nearest p's height within win (none: 0,
 * the grass ended before it; nothing known there at all: reach), then walked out from along EDGE_RAYS ways, n steps to reach, each
 * step's floor within a climb of the last's (grass.slope's over the step); reach when it doesn't end */
static float edge_floors(const float p[3], float win, int n, float reach)
{
    float fy, up;
    int soil;
    const int own = edge_floor(p[0], p[2], p[1], win, &fy, &up, &soil);
    if (own <= 0)
        return own < 0 ? reach : 0.0f;
    if (up < g.slope_cos)
        return 0.0f;
    const int soft = (soil >= 1 && soil <= 4) || soil == GRASS_WET;
    const float step = reach / (float)n, climb = step * edge_rise() + 0.3f;
    float best = reach;
    for (int k = 0; k < EDGE_RAYS; ++k)
    {
        float y = fy;
        for (int i = 1; i <= n && ((float)i - 0.5f) * step < best; ++i)
        {
            const float r = (float)i * step;
            float ny, nu;
            int nt;
            const int at = edge_floor(p[0] + g_ray[k][0] * r, p[2] + g_ray[k][1] * r, y, climb, &ny, &nu, &nt);
            if (at == 0 || (at == 1 && (nu < g.slope_cos || (soft && edge_hard(nt)))))
            {
                best = r - 0.5f * step;
                break;
            }
            y = at == 1 ? ny : y; /* none there at all: on at the same height */
        }
    }
    return best;
}

/* what the lattice's points are found with: the zone's floors (gf.set counts their hand-overs), the fade and grass.slope */
static uint32_t edge_tag(void)
{
    return hash32(gf.set * 73856093u ^ (uint32_t)(g.edge_fade * 64.0f) * 19349663u ^ (uint32_t)(g.slope_cos * 65536.0f) * 83492791u) | 1u;
}

/* the lattice's spacing: a quarter of the fade, 0.75 at the least */
static float edge_space(void) { return maxf(0.25f * g.edge_fade, 0.75f); }

/* how far from lattice point (ix, iz) the grass ends by the floors, from the floor nearest height y there (a tuft's beside it: it
 * may stand a spacing and a half off, on a slope), found once and kept by its place and y's band */
static float edge_node(int32_t ix, int32_t iz, float y, uint32_t tag)
{
    const float sp = edge_space(), fade = g.edge_fade;
    const uint64_t key = hash64(((uint64_t)(uint32_t)ix << 32 | (uint32_t)iz) ^ (uint64_t)(uint32_t)(int32_t)floorf(y / EDGE_BAND) * 0x9E3779B97F4A7C15ull);
    const uint32_t h = (uint32_t)(key ^ key >> 32) & (EDGE_CACHE - 1);
    if (g_edge.key[h] != key || g_edge.tag[h] != tag)
    {
        const float p[3] = { (float)ix * sp, y, (float)iz * sp };
        const int n = (int)clampf(ceilf(fade / 0.75f), 4.0f, 8.0f); /* the walk's steps: 0.75 to 1.25 apart, never under 4 */
        g_edge.key[h] = key, g_edge.tag[h] = tag;
        g_edge.d[h] = edge_floors(p, 1.0f + 1.5f * sp * edge_rise(), n, fade);
    }
    return g_edge.d[h];
}

/* how far from p the grass ends by the zone's floors: the lattice's four points round it, blended; the fade when they aren't known */
static float edge_lattice(const float p[3])
{
    if (gf.zone != g.zone || !gf.npairs)
        return g.edge_fade;
    const uint32_t tag = edge_tag();
    const float sp = edge_space(), fx = p[0] / sp, fz = p[2] / sp;
    const int32_t ix = (int32_t)floorf(fx), iz = (int32_t)floorf(fz);
    const float tx = fx - (float)ix, tz = fz - (float)iz;
    const float a = edge_node(ix, iz, p[1], tag), b = edge_node(ix + 1, iz, p[1], tag);
    const float c = edge_node(ix, iz + 1, p[1], tag), d = edge_node(ix + 1, iz + 1, p[1], tag);
    const float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
    return top + (bottom - top) * tz;
}

/* how far from p, up to within, the ground stops being grass in triangle t's picture (texture tx) carried past its edges
 * (path_near's way): EDGE_RAYS ways on 3 to 5 rings out to within (2 yalms apart at the most); within when it doesn't */
static float tex_edge(const GWTri* t, const GTex* tx, const float p[3], float within)
{
    const float x0 = t->p[0][0], z0 = t->p[0][2];
    const float ax = t->p[1][0] - x0, az = t->p[1][2] - z0, bx = t->p[2][0] - x0, bz = t->p[2][2] - z0;
    const float den = ax * bz - bx * az;
    if (!tx->map || !(within > 0.05f) || !(fabsf(den) > 1e-9f))
        return within;
    const float inv = 1.0f / den;
    const int rings = (int)clampf(ceilf(within * 0.5f), 3.0f, 5.0f);
    for (int ring = 1; ring <= rings; ++ring)
    {
        const float r = within * (float)ring / (float)rings;
        for (int k = 0; k < EDGE_RAYS; ++k)
        {
            const float qx = p[0] + r * g_ray[k][0] - x0, qz = p[2] + r * g_ray[k][1] - z0;
            const float u = (qx * bz - bx * qz) * inv, w = (ax * qz - qx * az) * inv;
            if (u < -4.0f || w < -4.0f || u + w > 5.0f) /* far off a small triangle: its picture says nothing of the ground there */
                continue;
            const float c0 = 1.0f - u - w;
            uint8_t rgb[3];
            if (tex_color(tx, c0 * t->uv[0][0] + u * t->uv[1][0] + w * t->uv[2][0], c0 * t->uv[0][1] + u * t->uv[1][1] + w * t->uv[2][1], rgb) &&
                !ground_ok(rgb, g_build.dry))
                return r - 0.5f * within / (float)rings;
        }
    }
    return within;
}

/* the share of its height (and width) a tuft at p grows to, b its place in triangle t of texture tx: with grass.edge.fade, by how
 * near the grass ends (the zone's floors, its texture, room: the brink of a drop's way) and its slope toward grass.slope, the
 * lesser, grass.edge.height at the least; without, the slope's last degrees' (flat: build_step's, 0 at grass.slope, 1 below the
 * fade) */
static float edge_grow(const GWTri* t, const GTex* tx, const float b[3], const float p[3], float flat, float room)
{
    if (!(g.edge_fade > 0.0f))
        return 0.45f + 0.55f * flat;
    const float fade = g.edge_fade;
    /* (grass.mega: the floors' edges alone, its texture's speckles none: on a grass floor every spot of it grows) */
    float d = g.mega ? minf(room, edge_lattice(p)) : tex_edge(t, tx, p, minf(room, edge_lattice(p)));
    d += (vnoise(p[0] * 0.7f, p[2] * 0.7f) - 0.5f) * 0.4f * fade; /* ragged: the shortening follows no line */
    float e = clampf(d / fade, 0.0f, 1.0f);
    e = e * e * (3.0f - 2.0f * e);
    /* the slope: full height to half grass.slope, then shorter, to the least at it */
    const float up = clampf((b[0] * t->vup[0] + b[1] * t->vup[1] + b[2] * t->vup[2]) * (1.0f / 254.0f), 0.0f, 1.0f);
    const float most = acosf(g.slope_cos);
    float s = clampf((acosf(up) - 0.5f * most) / (0.5f * most), 0.0f, 1.0f);
    s = s * s * (3.0f - 2.0f * s);
    return g.edge_low + (1.0f - g.edge_low) * minf(e, 1.0f - s);
}
