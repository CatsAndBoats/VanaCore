/* d3d8_water_memo.h: an include unit of d3d8_water.h (in its scope: grass_floor_near, grass_floor_water). What the zone's
 * floors say under a sheet's box, kept (sheet_floors, for water_sheet) */

/* the footsteps' water at a point whose nearest floor within reach is terrain t, dy below it: deep (9) at the surface within a
 * yalm and a half, or shallow (8) on the bed under it */
static int sheet_floor(int t, float dy)
{
    return (t == 9 && fabsf(dy) <= 1.5f) || (t == 8 && dy >= -0.5f);
}

/* What the zone's floors said under a sheet's box, kept by the box (its bits: a static sheet's comes out the same each frame)
 * until a zone's floors are handed over again. Asked afresh each frame, every blended fogged sheet in view cost six floor
 * lookups, and past 2x draw distance the squares they fell in were more than the 64 the grass keeps laid: each lookup laid a
 * square again (Tahrongi at 3x, 2026-11-19: 57% of the game's thread, 39 FPS against 60 with water.detect=1) */
#define SHEET_MEMO 2048 /* a power of two; cleared when three quarters full */
typedef struct SheetMemo
{
    uint32_t box[6], used;
    int8_t t[3];
    uint8_t water;
    float dy[3];
} SheetMemo;
static SheetMemo g_sheet_memo[SHEET_MEMO];
static uint32_t g_sheet_memo_n, g_sheet_memo_set;

/* the footsteps' water under a sheet's box: how many of its middle and the quarter points of its longer side have it (t, dy:
 * what was under each, for water.log) */
static int sheet_floors(const float lo[3], const float hi[3], int t[3], float dy[3])
{
    unsigned set = 0;
    const int wet = grass_floor_water(&set, lo, hi);
    t[0] = t[1] = t[2] = -1, dy[0] = dy[1] = dy[2] = 0.0f;
    if (wet <= 0) /* none in the squares it lies over (or the zone), or the floors not known yet */
        return 0;
    if (set != g_sheet_memo_set || g_sheet_memo_n >= SHEET_MEMO / 4 * 3)
        memset(g_sheet_memo, 0, sizeof g_sheet_memo), g_sheet_memo_n = 0, g_sheet_memo_set = set;
    uint32_t box[6], h = 2166136261u;
    memcpy(box, lo, 12), memcpy(box + 3, hi, 12);
    for (int i = 0; i < 6; ++i)
        h = (h ^ box[i]) * 16777619u;
    SheetMemo* m = &g_sheet_memo[h & (SHEET_MEMO - 1)];
    while (m->used && memcmp(m->box, box, sizeof box))
        m = m == &g_sheet_memo[SHEET_MEMO - 1] ? g_sheet_memo : m + 1;
    if (!m->used)
    {
        const float dx = hi[0] - lo[0], dz = hi[2] - lo[2], y = (lo[1] + hi[1]) * 0.5f;
        float p[3][3] = { { (lo[0] + hi[0]) * 0.5f, y, (lo[2] + hi[2]) * 0.5f }, { 0 }, { 0 } };
        for (int i = 1; i < 3; ++i)
        {
            const float k = i == 1 ? 0.25f : 0.75f;
            p[i][0] = dx >= dz ? lo[0] + dx * k : p[0][0], p[i][1] = y, p[i][2] = dx >= dz ? p[0][2] : lo[2] + dz * k;
        }
        memcpy(m->box, box, sizeof box), m->used = 1, ++g_sheet_memo_n;
        for (int i = 0; i < 3; ++i)
        {
            const int ti = grass_floor_near(p[i], 4.0f, &m->dy[i]);
            m->t[i] = (int8_t)ti, m->water += (uint8_t)sheet_floor(ti, m->dy[i]);
        }
    }
    for (int i = 0; i < 3; ++i)
        t[i] = m->t[i], dy[i] = m->dy[i];
    return m->water;
}
