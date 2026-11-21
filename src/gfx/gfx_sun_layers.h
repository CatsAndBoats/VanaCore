/* The sun's still layers drawn a slice a frame ([graphics.effects] sun_roll): gfx_d3d12.c includes this just before sun_map, after
 * sun_draw, g_still, g_still_mid, g_still_near, still_stale and near_stale, whose names it uses. It is no file of its own. */
#pragma once

#define SUN_SLICES 4 /* the frames a layer is drawn over */

/* what sun_map's layers work from this frame: the scene, its inverse projection and view, the light, the sharp map's reach, and the
 * player's distance from the camera (both for sun_layer_note) */
typedef struct
{
    const GfxScene* s;
    const float *invP, *invV, *L;
    float reach, to;
} SunCtx;

/* A layer being drawn into its back map a slice a frame, then swapped in. The cascade it's drawn through (fitted when its first
 * slice was), the frames of its first and latest slice, the slices drawn so far (0: none under way), their casters, the pipelines
 * left out, why it was begun, its map's size (the sharp map's), and what it was drawn for: the sun, the far map's reach and the
 * settings that change it */
typedef struct
{
    SunCascade k;
    uint64_t began, last;
    uint32_t step, drawn, skipped, why, size;
    uint32_t live, cached, nou; /* of drawn: this frame's casters, the cache's; left out for no room in the ring for its uniforms */
    float L[3], dfar, casters, soft, plants;
} SunRoll;
static SunRoll g_roll_far, g_roll_near, g_roll_mid;

/* A wide map's still layer, made as the far map's is (wide_layer): what it's kept in, its slices' progress, the map it's drawn
 * into and the back map a sliced one is drawn into and swapped with, and which it is (0 the far map's, 2 the middle one's: the
 * flicker watch's layer_count_note) */
typedef struct
{
    SunStill* st;
    SunRoll* r;
    SunMap *layer, *back;
    int which;
} SunWide;
static const SunWide WIDE_FAR = { &g_still, &g_roll_far, &g_fx.smaps, &g_fx.smapsb, 0 };
static const SunWide WIDE_MID = { &g_still_mid, &g_roll_mid, &g_fx.smapms, &g_fx.smapmsb, 2 };

/* the frame's caster counters (g_fx.tr_*) before a layer's draw, and the draw's share of them added to the layer's after */
static void roll_tally_begin(uint32_t b[4])
{
    b[0] = g_fx.tr_live, b[1] = g_fx.tr_cached, b[2] = g_fx.tr_skipped, b[3] = g_fx.tr_nou;
}
static void roll_tally_end(SunRoll* r, const uint32_t b[4])
{
    r->live += g_fx.tr_live - b[0], r->cached += g_fx.tr_cached - b[1];
    r->skipped += g_fx.tr_skipped - b[2], r->nou += g_fx.tr_nou - b[3];
}

/* The reasons the layer can't go on serving the map while a new one is drawn: it isn't what the map was made from the frame
 * before, other settings, the sun turned. The rest - its age, the camera's move, a radius - leave it good for the few frames the
 * new one takes. */
#define WHY_HARD (WHY_GAP | WHY_SETTINGS | WHY_SUN)

/* a layer's next slice into back, through the cascade k; 1 when it was the last */
static int roll_slice(SunMap* back, const SunCtx* x, const SunCascade* k, SunRoll* r)
{
    uint32_t b[4];
    roll_tally_begin(b);
    g_sun_slice.n = SUN_SLICES, g_sun_slice.phase = r->step, g_sun_slice.keep = r->step > 0;
    r->drawn += sun_draw(back, x->s, x->invP, x->invV, k, 1, 0, SUN_STILL);
    g_sun_slice.n = 0, g_sun_slice.keep = 0; /* (keep left on, every map drawn after went uncleared) */
    roll_tally_end(r, b);
    r->last = g_serial;
    return ++r->step >= SUN_SLICES;
}

/* A layer's drawing starts: what it's drawn through and for (L: the sun; fit: the far map's reach, or the sharp map's size) */
static void roll_begin(SunRoll* r, const SunCascade* k, const float* L, float fit, uint32_t why)
{
    memset(r, 0, sizeof *r);
    r->k = *k, r->began = r->last = g_serial, r->why = why, r->dfar = fit, r->size = (uint32_t)fit;
    memcpy(r->L, L, sizeof r->L);
    r->casters = g_fxs.sun_casters, r->soft = g_fxs.sun_soft, r->plants = g_fxs.sun_plants;
}

/* What a layer does this frame: 0 nothing, 1 drawn whole in this one (as it was before sun_roll, and when it can't be served by
 * the old one meanwhile), 2 the next slice of a new one. ready: its back map is there. */
static int roll_plan(SunRoll* r, int stale, uint32_t why, int ready)
{
    if (r->step && r->last + 1 != g_serial)
        r->step = 0; /* a frame went by without it: the slices since missed what came and went */
    if (!stale && !r->step)
        return 0;
    if (!ready || (why & WHY_HARD))
        return r->step = 0, 1;
    return 2;
}

/* The flicker watch: a layer made with LAYER_STEP fewer or more casters than the one before it (the far map's, the sharp map's
 * and the middle one's apart: which 0, 1, 2) is said, with the cascade's centre - the zone stands still and the cache keeps what
 * it saw, so a step is a piece that came or went: a shadow popping as the camera moves (Port Jeuno, 2026-11-02, the camera
 * turned round the character) */
#define LAYER_STEP 6
static void layer_count_note(int which, const SunRoll* r)
{
    static const char* const NAME[3] = { "far", "sharp", "middle" };
    static uint32_t last[3];
    const uint32_t was = last[which];
    last[which] = r->drawn;
    if (was && (r->drawn + LAYER_STEP <= was || r->drawn >= was + LAYER_STEP))
        gfx_watch("the %s layer was made with %u casters (%u this frame's, %u the cache's; %u left out for their pipelines, %u for "
                  "no room for their uniforms), the one before with %u (centred %.0f %.0f %.0f, why %u, %u frames)",
            NAME[which], r->drawn, r->live, r->cached, r->skipped, r->nou, was, r->k.c[0], r->k.c[1], r->k.c[2], r->why,
            (unsigned)(g_serial - r->began));
}

/* The flicker watch's holes (gfx_sun_slice.h slice_mark): the cache's live entries standing within the far layer's map that none of
 * its slices took - each a shadow gone for the frames the layer serves - said with where the first few stand and when they were
 * last seen; the marks cleared for the next layer */
static void layer_holes_note(const SunRoll* r)
{
    uint32_t holes = 0, listed = 0;
    char where[200];
    int n = 0;
    for (uint32_t i = 0; i < g_ncache && i < g_slice_mark_n; ++i)
    {
        const Cached* ce = &g_cache[i];
        if (ce->c.dead || !ce->has_at || beyond_map(&r->k, ce->at))
            continue;
        listed++;
        if (g_slice_mark[i])
            continue;
        holes++;
        if (n < 150)
            n += snprintf(where + n, sizeof where - (size_t)n, " (%.0f %.0f %.0f, seen %u frames ago)", ce->at[0], ce->at[1], ce->at[2],
                (unsigned)(g_serial - ce->seen));
    }
    if (holes)
        gfx_watch("the far layer was made without %u of the cache's %u entries in its reach:%s", holes, listed, where);
    if (g_slice_mark_n)
        memset(g_slice_mark, 0, g_slice_mark_n);
}

/* a wide layer made: the sun's, the settings' and the cascade it was drawn for are its SunStill's; swap, the back map it was drawn
 * into taking the place of the one the map is made from. The far map's alone goes to fx_stats and the holes watch */
static void wide_commit(const SunWide* w, const SunCtx* x, SunRoll* r, int swap)
{
    SunStill* st = w->st;
    const float was = st->k.across;
    layer_count_note(w->which, r);
    if (w->which == 0)
        layer_holes_note(r);
    if (swap)
    {
        const SunMap done = *w->back;
        *w->back = *w->layer, *w->layer = done;
    }
    st->drawn = r->drawn, st->k = r->k, st->serial = r->skipped ? 0 : r->began, st->dfar = r->dfar;
    st->casters = r->casters, st->soft = r->soft, st->plants = r->plants;
    memcpy(st->L, r->L, sizeof st->L);
    if (w->which == 0)
    {
        g_sst.still_drawn++;
        sun_layer_note(0, was, r->k.across, SUN_MAP, r->why, x->reach, x->to); /* (skipped: casters left out while their pipelines build, drawn again next frame) */
    }
    r->step = 0;
}

/* A wide map's still layer (the far map's, the middle one's) brought up to date (still_stale says when): drawn whole, or -
 * sun_roll - a slice of the next one while the one the map is made from goes on serving, then swapped in. k: this frame's
 * cascade; reach: the map's (sun_distance, sun_mid) */
static void wide_layer(const SunWide* w, const SunCtx* x, const SunCascade* k, float reach, uint32_t hold)
{
    SunRoll* r = w->r;
    const int stale = still_stale(w->st, k, x->L, reach, hold), roll = g_fxs.sun_roll != 0.0f;
    const uint32_t why = stale ? g_stale_why : 0;
    if (!roll && w->back->res)
        sun_release(w->back); /* its 64 MB back while it's off */
    const int plan = roll_plan(r, stale, why, roll && sun_target(w->back, SUN_MAP));
    if (plan == 1)
    {
        roll_begin(r, k, x->L, reach, why);
        uint32_t b[4];
        roll_tally_begin(b);
        r->drawn = sun_draw(w->layer, x->s, x->invP, x->invV, &r->k, 1, 0, SUN_STILL);
        roll_tally_end(r, b);
        wide_commit(w, x, r, 0);
    }
    else if (plan == 2)
    {
        if (!r->step)
            roll_begin(r, k, x->L, reach, why);
        if (roll_slice(w->back, x, &r->k, r))
            wide_commit(w, x, r, 1);
    }
}

/* the sharp layer made, as wide_commit */
static void near_commit(const SunCtx* x, SunRoll* r, int swap)
{
    const float was = g_still_near.k.across;
    layer_count_note(1, r);
    if (swap)
    {
        const SunMap done = g_fx.smapnsb;
        g_fx.smapnsb = g_fx.smapns, g_fx.smapns = done;
    }
    g_still_near.k = r->k, g_still_near.serial = r->skipped ? 0 : r->began, g_still_near.size = r->size;
    g_still_near.casters = r->casters, g_still_near.soft = r->soft, g_still_near.plants = r->plants;
    memcpy(g_still_near.L, r->L, sizeof g_still_near.L);
    g_sst.near_drawn++;
    sun_layer_note(1, was, r->k.across, r->size, r->why, x->reach, x->to);
    r->step = 0;
}

/* The sharp map's still layer brought up to date (near_stale says when), through a cascade fitted with a margin (SUN_NEAR_MARGIN) */
static void near_layer(const SunCtx* x, const SunCascade* cnear, uint32_t size, uint32_t hold)
{
    SunRoll* r = &g_roll_near;
    const int stale = near_stale(cnear, x->L, size, hold), roll = g_fxs.sun_roll != 0.0f;
    const uint32_t why = stale ? g_stale_why : 0;
    if (!roll && g_fx.smapnsb.res)
        sun_release(&g_fx.smapnsb);
    const int plan = roll_plan(r, stale, why, roll && sun_target(&g_fx.smapnsb, size));
    if (plan == 0)
        return;
    if (plan == 1 || !r->step)
    {
        SunCascade wide;
        sun_fit(x->s, x->invV, x->L, 0.5f, x->reach, size, SUN_NEAR_MARGIN, &wide);
        roll_begin(r, &wide, x->L, (float)size, why);
    }
    if (plan == 1)
    {
        uint32_t b[4];
        roll_tally_begin(b);
        r->drawn = sun_draw(&g_fx.smapns, x->s, x->invP, x->invV, &r->k, 1, 0, SUN_STILL);
        roll_tally_end(r, b);
        near_commit(x, r, 0);
    }
    else if (roll_slice(&g_fx.smapnsb, x, &r->k, r))
        near_commit(x, r, 1);
}
