/* d3d8_water_bake.h: an include unit of d3d8_water.h (in its scope: WaterBake, g_bake, g_wfloor, BAKE_*, obj, g_dev). Which of
 * the world's draws water_ground looks at as ground (bake_ground_draw, bake_left), the entry one has had (bake_find), one of two
 * sheets over each other let go (bake_one_sheet) and a sheet lying on the ground's water (water_ground_under) */

/* water.log: why a draw of the world's wasn't looked at as ground (the first BAKE_SAID a zone, chunk-sized draws only) */
static void bake_left(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, uint32_t up_data, const char* why)
{
    if (!g_water_set.log || g_bake_said[2] >= BAKE_SAID || nverts < 64 || up_data || d->vs.rhw || !d->vs.lighting || !d->depth.zenable)
        return;
    ++g_bake_said[2];
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    rt_log("[recomp] water: ground draw left (%s): prim %u, %u of %u vertices, lit %d, z %d/%d, fog %d/%d, blend %d, position %d stream %u "
           "type %d cpu %d size %u, indices %d (%u bytes)\n",
        why, prim, count, nverts, d->vs.lighting, d->depth.zenable, d->depth.zwrite, d->fs.fog, d->vs.fog_vertex, d->pipe.blend, e->used,
        e->stream, e->type, e->stream < GFX_NSTREAMS && d->cpu[e->stream] != NULL, e->stream < GFX_NSTREAMS ? d->cpu_size[e->stream] : 0,
        d->indices != NULL, d->index_size);
}

/* water_ground's draw: 1 a chunk of the zone's ground to look at (lit, fogged, depth-tested, from a static buffer the CPU has),
 * 2 an additive layer (destination one) of the same, 0 neither (with water.log, why). An additive layer is light laid over a
 * surface, not the surface: East Sarutabaruta's pond is painted on its bed, with one lit additive mesh laid four times along its
 * banks; baked as ground, one of them was VanaCore's water in the layer's shape, a bright strip with foam and jagged ends over
 * the dark pond. Its pond is flooded from it instead (bake_flood) */
static int bake_ground_draw(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, uint32_t up_data)
{
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    if (up_data || d->vs.rhw || !d->vs.lighting || !d->depth.zenable || !(d->fs.fog || d->vs.fog_vertex) ||
        (prim != GFX_TRIANGLELIST && prim != GFX_TRIANGLESTRIP) || !e->used || e->type != GFX_FLOAT3 || e->stream >= GFX_NSTREAMS ||
        !d->cpu[e->stream] || count > 8192 || (d->indices && d->index_size != 2 && d->index_size != 4))
    {
        bake_left(d, prim, count, nverts, up_data, "not a lit, fogged, depth-tested chunk");
        return 0;
    }
    const Obj* vb = obj(g_dev.cur.stream[e->stream]);
    if (!vb || (vb->usage & USAGE_DYNAMIC))
    {
        bake_left(d, prim, count, nverts, up_data, "a dynamic buffer");
        return 0;
    }
    return d->pipe.blend && d->pipe.dst == 2 ? 2 : 1;
}

/* The entry of the draw's mesh (its vertices, indices, count and kind) drawn at t (its world matrix's translation, within half a
 * yalm), or NULL for a place it hasn't had. One mesh at several places (East Sarutabaruta's pond layer, four times; the zone's
 * repeated chunks) is an entry each: keyed by the mesh alone, the first place's water stood for all. Past BAKE_PLACES of one
 * mesh, its first entry: a model the game moves (a door, a boat) isn't looked at afresh wherever it goes */
#define BAKE_PLACES 8
/* The entries by their mesh (each the entry's index + 1, 0 empty; open addressing): every lit draw of the world asks, and the
 * list gone through whole was 1300 draws a frame by hundreds of entries */
#define BAKE_HASH 32768u /* a power of two, four times BAKE_ENTRIES */
static uint16_t g_bake_ix[BAKE_HASH];
static uint32_t bake_hash(uintptr_t key, uint32_t count, uint32_t prim)
{
    const uint32_t h = ((uint32_t)key * 2654435761u) ^ (count * 40503u) ^ prim;
    return (h ^ (h >> 15)) & (BAKE_HASH - 1);
}

/* the zone's floors handed over (d3d8_set_water_floors, which lets the entries go): the index and the ponds' cells forgotten */
static void bake_forget(void)
{
    memset(g_bake_ix, 0, sizeof g_bake_ix);
    flood_reset();
}

/* entry i (about to be made, for that mesh) into the index */
static void bake_index(uint32_t i, uintptr_t key, uint32_t count, uint32_t prim)
{
    uint32_t h = bake_hash(key, count, prim);
    for (uint32_t k = 0; k < BAKE_HASH && g_bake_ix[h]; ++k)
        h = (h + 1) & (BAKE_HASH - 1);
    if (!g_bake_ix[h])
        g_bake_ix[h] = (uint16_t)(i + 1);
}

static WaterBake* bake_find(uintptr_t key, uint32_t count, uint32_t prim, const float t[3])
{
    WaterBake* first = NULL;
    uint32_t places = 0;
    for (uint32_t h = bake_hash(key, count, prim), k = 0; k < BAKE_HASH && g_bake_ix[h]; ++k, h = (h + 1) & (BAKE_HASH - 1))
    {
        WaterBake* b = &g_bake[g_bake_ix[h] - 1];
        if (b->key != key || b->count != count || b->prim != prim)
            continue;
        if (fabsf(b->t[0] - t[0]) <= 0.5f && fabsf(b->t[1] - t[1]) <= 0.5f && fabsf(b->t[2] - t[2]) <= 0.5f)
            return b;
        first = first ? first : b, ++places;
    }
    return places >= BAKE_PLACES || (first && first->moved) ? first : NULL;
}

/* The mesh (key, count, prim) drawn at a new place t (its box lo, hi) within half its size across (4 yalms at most) and a yalm up
 * or down of one it's had: a model the game moves (its copies the zone lays don't stand on each other). Selbina's ferry was looked
 * at afresh at each place it sailed through, its deck's water left behind at each, eight sheets half a yalm apart that stayed
 * after it had gone, in squares, and fought each other's depth. Its entries' water let go and marked moved (1): bake_find gives
 * the first of them from then on, never looked at again */
#define BAKE_MOVED 4.0f
static int bake_moving(uintptr_t key, uint32_t count, uint32_t prim, const float t[3], const float lo[3], const float hi[3])
{
    const float rx = fminf((hi[0] - lo[0]) * 0.5f, BAKE_MOVED), rz = fminf((hi[2] - lo[2]) * 0.5f, BAKE_MOVED);
    int moved = 0;
    for (int pass = 0; pass < 1 + moved; ++pass)
        for (uint32_t h = bake_hash(key, count, prim), k = 0; k < BAKE_HASH && g_bake_ix[h]; ++k, h = (h + 1) & (BAKE_HASH - 1))
        {
            WaterBake* b = &g_bake[g_bake_ix[h] - 1];
            if (b->key != key || b->count != count || b->prim != prim)
                continue;
            if (pass) /* (n 0: the pass and water_ground_under pass it by; its triangles stay in the pool, unseen) */
                b->moved = 1, b->n = 0;
            else if (fabsf(b->t[0] - t[0]) < rx && fabsf(b->t[2] - t[2]) < rz && fabsf(b->t[1] - t[1]) < 1.0f)
                moved = 1;
        }
    return moved;
}

/* a box (lo, hi: in the world) reaching the collision's water: within its box in x and z, and not under the beds or more than 12
 * yalms over them */
static int bake_reaches(const float lo[3], const float hi[3])
{
    return hi[0] >= g_wfloor.lo[0] && lo[0] <= g_wfloor.hi[0] && hi[2] >= g_wfloor.lo[2] && lo[2] <= g_wfloor.hi[2] &&
           hi[1] >= g_wfloor.lo[1] - 12.0f && lo[1] <= g_wfloor.hi[1] + 1.0f;
}

/* The game lays a stream as two flat sheets half a yalm apart, the lower broader than the upper: in the pass both were opaque
 * water and the lower showed past the upper's edge at the banks with depths of its own, a lighter patch with a hard edge.
 * Of two sheets lying over each other within a yalm and a half, the lower is let go (its triangles stay in the pool, unseen) */
static void bake_one_sheet(WaterBake* b)
{
    const float ay = (b->lo[1] + b->hi[1]) * 0.5f;
    for (uint32_t i = 0; i < g_bake_n; ++i)
    {
        WaterBake* o = &g_bake[i];
        if (o == b || !o->n)
            continue;
        const float oy = (o->lo[1] + o->hi[1]) * 0.5f;
        /* (at one height they're one surface in two meshes, not over each other: Caedarva Mire's pond lays its sheets' tiles in two
         * pictures, a checkerboard, and one of the two was let go) */
        if (fabsf(oy - ay) > 1.5f || fabsf(oy - ay) < 0.1f)
            continue;
        /* over each other: the smaller's box mostly within the larger's */
        const float ix = fminf(b->hi[0], o->hi[0]) - fmaxf(b->lo[0], o->lo[0]), iz = fminf(b->hi[2], o->hi[2]) - fmaxf(b->lo[2], o->lo[2]);
        if (ix <= 0.0f || iz <= 0.0f)
            continue;
        const float ab = (b->hi[0] - b->lo[0]) * (b->hi[2] - b->lo[2]), ao = (o->hi[0] - o->lo[0]) * (o->hi[2] - o->lo[2]);
        if (ix * iz < 0.6f * fminf(ab, ao))
            continue;
        if (ay > oy) /* b is the lower (y down) */
        {
            b->n = 0;
            return;
        }
        o->n = 0;
    }
}

/* lo, hi (a sheet's box) lying on a chunk of the ground's water this zone: its middle within the chunk's box across the ground,
 * and the sheet from a yalm and a half under the chunk's water to a yalm over it */
static int water_ground_under(const float lo[3], const float hi[3])
{
    const float mx = (lo[0] + hi[0]) * 0.5f, mz = (lo[2] + hi[2]) * 0.5f;
    for (uint32_t i = 0; i < g_bake_wet_n; ++i)
    {
        const WaterBake* b = &g_bake[g_bake_wet[i]];
        if (!b->n || mx < b->lo[0] || mx > b->hi[0] || mz < b->lo[2] || mz > b->hi[2])
            continue;
        const float y = (b->lo[1] + b->hi[1]) * 0.5f; /* the water's height (a sheet taken whole is flat) */
        if (hi[1] <= y + 1.5f && lo[1] >= y - 1.0f)
            return 1;
    }
    return 0;
}
