/* The sun's shadow cache: the zone's casters kept while they're out of the camera's view, with copies (snaps) of the ones
 * drawn from buffers the game rewrites, the drift they're moved along, their stats (fx_stats), how they're let go, and the
 * caster pass's reach (gfx_seek_reach). gfx_d3d12.c includes this just before SunCascade, after Caster, CasterSet, LibKey,
 * KeyMap, g_fxs and g_sst, whose names it uses; sun_draw and sun_map (after it) draw from it. It is no file of its own. */
#pragma once

/* The zone's casters, kept after they leave the view: the game draws only what the camera sees, but
 * a low sun throws the shadows of what is behind and beside the camera into it. Each caster drawn
 * from buffers the game keeps is kept with a copy of its uniforms and that frame's camera (clip
 * space back to the world), and drawn into the map from there while it is out of view - for good
 * while the player stays in the zone (cache_expired lets go of only what stands a thousand units off, unseen a minute), or
 * until the player jumps (a new zone). A buffer or texture it draws from that goes or changes (sun_forget) ends it. */
typedef struct CacheKey
{
    D3D12_GPU_VIRTUAL_ADDRESS vb[GFX_NSTREAMS], ib;
    uint32_t n, vstart;
    LibKey lib;
} CacheKey;

typedef struct Cached
{
    Caster c;                         /* as last drawn; its own uniforms are u */
    /* (what's read of every entry each frame, c.dead with it: together, after c) */
    float at[3];                      /* where it stands (caster_at), when has_at */
    uint8_t has_at;
    uint8_t sought;                   /* the caster pass has drawn it (Caster.sought): a piece of the zone round the player, never
                                       * culled for standing past the maps' reach (cache_beyond), left out of the characters' map */
    uint8_t still;                    /* a plant's first pose, never taken again: drawn in place of the plant each frame (plant_still) */
    uint64_t seen, ring_serial;
    uint64_t seen_ns;                 /* when it was last seen (cache_expired) */
    uint64_t fade_ns;                 /* when it was let go of (cache_let_go): fading since; 0 not */
    float cam[3];                     /* ... and where it was */
    GfxU* u;                          /* ... c.need bytes of them */
    float clip_world[16];             /* that frame's camera: clip space back to the world */
    uint32_t next;                    /* the next entry of its key in its cell (cache_link), index + 1 */
    uint64_t kh;                      /* its CacheKey's hash */
    D3D12_GPU_VIRTUAL_ADDRESS ring_u; /* u in this frame's ring, once drawn this frame */
    /* drawn again as this caster of the set of generation pend_gen (0: none), not yet copied into c, u, clip_world and
     * cam: that's done once it's out of view (cache_take), not every frame it's seen */
    uint32_t pend, pend_gen;
} Cached;

static Cached* g_cache;
static uint32_t g_ncache, g_cache_cap;
static KeyMap g_cache_map; /* CellKey (a key's hash and a cell) -> index + 1 (cache_link) */
/* The caster pass's draws, each kept by its piece (the game's own: Caster.piece) and its place among the piece's draws - PieceKey ->
 * index + 1 (piece_see) - not by mesh and place: one entry each, made again with the cells (sun_cache_trim). Matched up by mesh and
 * place within a unit, a swaying tree was kept again and again: The Sanctuary of Zi'Tah's far map took 27,000-42,000 of the
 * cache's entries for some 1,700 of the pass's draws, the frame 20 ms with the player standing still (2026-11-03) */
typedef struct PieceKey
{
    uint32_t piece, draw;
} PieceKey;
static KeyMap g_piece_map;
/* frames a piece's draw is kept after the pass last drew it: some 15 of its rounds (casters.cpp kRoundWalks); past that the piece
 * is out of reach, or another zone's */
#define PIECE_KEEP 120
static float g_cache_anchor[3]; /* where the player stands (the camera, without one): what cache_expired and cache_see measure from,
                                 * and whose jump clears the cache (a new zone), as the camera orbits the player and its eye moves
                                 * tens of units in a turn; set once a frame has run (g_cache_anchor_set) */
static int g_cache_anchor_set;

/* What's seen casting is kept for good while the player stays in the zone: the zone stands still, so a piece seen blocking the
 * light blocks it still, whatever the camera sees now. Let go of before - 10 s after it was last seen and past the maps' reach,
 * or in plain view and not drawn (another LOD of it, or its cell's view list without it), or 3 s after the caster pass last drew
 * it - a piece's shadow went and came back as the camera or the player moved: 50 to 130 a frame with the camera over Port Jeuno's
 * archway (2026-11-02 16:32), the plaza's shadows shimmering, and a wall's railings with its LOD. Only one standing SUN_DROP_FAR
 * past the player, unseen for SUN_KEEP_NS, goes (cache_expired): it can cast into no map from there, and the pass finds it again
 * as the player nears, so the cache holds the neighbourhood and not the whole of a zone walked across. A buffer or texture that
 * goes (sun_forget), or a piece that moved (cache_moved), still ends an entry at once. */
#define SUN_KEEP_NS 60000000000ull
#define SUN_DROP_FAR 1000.0f

/* sun_gate: parts of the sun's shadows turned off, to find which one changes them as the camera moves with the sun held still
 * (sun_free; Port Jeuno's walls, 2026-11-02). Under 256 the shader's (u.aoc2.z: fx_ao, sun_map, sun_look), the rest the maps'
 * and the cache's */
enum
{
    SG_SMOOTH = 1, SG_TURNED = 2, SG_FLANK = 4, SG_FACE = 8, SG_EDGE = 16, SG_CONTACT = 32, SG_SIMPLE = 64, SG_BUMP = 128,
    SG_NEAR = 256, SG_CACHE = 512, SG_CULL = 1024, SG_HOLD = 2048, SG_SEEN = 4096
};
static uint32_t sun_gate(void) { return g_fxs.sun_gate >= 1.0f && g_fxs.sun_gate < 65536.0f ? (uint32_t)g_fxs.sun_gate : 0u; }
/* the cache off: [graphics.effects] sun_cache 0 (saved), or the gate's SG_CACHE (not) - nothing kept but the plants' still poses
 * (plant_still). The caster pass runs all the same (gfx_seek_reach): it draws every piece within reach every frame, and while it
 * does the cache holds nothing else it would draw (sun_pass_only) */
static int sun_cache_off(void) { return g_fxs.sun_cache == 0.0f || (sun_gate() & SG_CACHE) != 0; }
/* How far past a map's sides a caster can still reach into it (beyond_map; and the cache's reach, cache_see and cache_expired).
 * A caster stands where its FIRST vertex is (clip0), not where its body is: a zone piece longer than this with its first vertex
 * past the line is culled whole while the rest of it could reach the map (a flaw, not seen to matter yet: 400 here made no
 * difference to Port Jeuno's flicker, 2026-11-01; gfx_sun_bounds.h has a box cull to wire in) */
#define SUN_REACH 96.0f
static uint64_t g_fade_now; /* this frame's time (sun_cache_update) */

/* The caster pass's reach (gfx.h): what the cache keeps round the player (cache_expired), the far map and SUN_REACH past it; and
 * the light's stretch, how far a shadow reaches along the ground for each unit of its caster's height (the shadows' light's cosine
 * over its sine, from gfx_sun_light's height: 1 at 45 degrees up; 3 at most, the dusk clamp's 17 degrees), which the pass takes a
 * tall piece from farther by (the walls far behind the camera onto Port Jeuno's plaza, 2026-11-02). The cache on or off: the pass
 * reaches every piece every frame (casters.cpp kRoundWalks 1), and with the cache needed for it, /vana's two switches for the one
 * thing read on and did nothing (2026-11-02 23:05) */
static float g_seek_stretch;
static void seek_stretch_note(float height)
{
    gfx_watch_value("sun: shadows' light height (after the dusk clamp)", height);
    g_seek_stretch = height > 0.3f ? fminf(sqrtf(1.0f - height * height) / height, 3.0f) : 3.0f;
}
/* The zone's shadows from the caster pass alone (sun_draw, caster_left_out): while it drew this frame, the game's own draws of the
 * zone (fixed casters it drew for its camera) and the cache are left out of the sun's maps, which hold what the pass drew - every
 * piece within reach of the player, one version of each, every frame - and this frame's characters. Mixed with the game's own,
 * the maps changed as its camera turned: the game draws a piece at the detail its camera's distance picks, or a far copy of it,
 * and the cache kept every version it saw, the one entry of a mesh taking another placement's uniforms; the far map, looked at
 * as it was drawn (debug 27), had parts of a building nearer or farther from one quarter-second to the next with the sun and the
 * player still (Port Jeuno, 2026-11-02 22:25). Not while the pass draws nothing - a moving zone's slot, a ship's - where the
 * game's own draws and the cache are all there is */
static int sun_pass_only(void) { return g_fxs.sun_seek != 0.0f && g_sought_serial && g_sought_serial + 2 >= g_serial; }

/* [graphics.effects] sun_plants_still: a plant (draw_plant: a tree, a bush) casts the pose it was first seen in, for good - the
 * cache's entry of it is never taken again, and is drawn in its place each frame (sun_draw), the cache on or not. The game sways
 * Kazham's palms every frame: as drawn, every frond's shadow moved each frame and the jungle floor shimmered under the canopy
 * (2026-11-02 22:45, the cache and the caster pass off). Kept, the pose's uniforms are that frame's camera's with it
 * (Cached.clip_world; the caster pass's in world space, by the map's own matrix), so the camera moving moves nothing either. */
static int plant_still(const Caster* c) { return g_fxs.sun_plants_still != 0.0f && c->plant; }
static int cache_still(const Cached* ce) { return g_fxs.sun_plants_still != 0.0f && ce->still; }

/* entry ce is drawn into the maps in place of this frame's caster of it (sun_draw's live_left_out): the caster pass's, kept by piece,
 * while the pass runs; a plant's still pose */
static int cache_instead(const Cached* ce)
{
    return !ce->c.dead && (ce->c.piece ? sun_pass_only() : cache_still(ce));
}

/* entry ce of those sun_cache_update listed is drawn into a map (sun_draw): the caster pass's while it runs, and nothing else of the
 * cache's then; with the cache off (still_only), the plants' still poses alone */
static int cache_drawn(const Cached* ce, int still_only)
{
    if (ce->c.dead)
        return 0;
    if (ce->c.piece)
        return 1;
    if (sun_pass_only())
        return 0;
    return !still_only || cache_still(ce);
}

float gfx_seek_reach(float* stretch)
{
    if (stretch)
        *stretch = g_seek_stretch;
    if (!g_dev || g_fxs.fx == 0.0f || g_fxs.sun <= 0.0f || g_fxs.sun_seek == 0.0f || g_fxs.sun_casters == 1.0f)
        return 0.0f;
#ifdef VANACORE
    if (zonemesh_resident())
        return 0.0f; /* the zone's own meshes cast in its place (zonemesh.c) */
#endif
    return fmaxf(g_fxs.sun_distance, 4.0f) + SUN_REACH;
}

/* the far map's footprint as last drawn (sun_map): its x and y rows, and how far past its sides a caster can still reach it
 * (beyond_map's edge); g_far_set 0 before the first */
static float g_far_xy[8], g_far_edge;
static int g_far_set;

/* entry ce's shadow can reach no map: it stands past the far map's sides by SUN_REACH or more (as beyond_map) */
static int cache_unseen(const Cached* ce)
{
    if (!g_far_set || !ce->has_at)
        return 0;
    const float* a = ce->at;
    const float x = a[0] * g_far_xy[0] + a[1] * g_far_xy[1] + a[2] * g_far_xy[2] + g_far_xy[3];
    const float y = a[0] * g_far_xy[4] + a[1] * g_far_xy[5] + a[2] * g_far_xy[6] + g_far_xy[7];
    return fabsf(x) > g_far_edge || fabsf(y) > g_far_edge;
}

/* no longer kept: unseen for SUN_KEEP_NS and standing SUN_DROP_FAR past the player (one with no place known is kept) */
static int cache_expired(const Cached* ce)
{
    if (ce->c.piece) /* (the caster pass's: kept while the pass draws it, PIECE_KEEP) */
        return 0;
    if (!ce->has_at || !g_cache_anchor_set || ce->seen_ns >= g_fade_now || g_fade_now - ce->seen_ns < SUN_KEEP_NS)
        return 0;
    const float dx = ce->at[0] - g_cache_anchor[0], dy = ce->at[1] - g_cache_anchor[1], dz = ce->at[2] - g_cache_anchor[2];
    return dx * dx + dy * dy + dz * dz > SUN_DROP_FAR * SUN_DROP_FAR;
}

/* [graphics.effects] sun_fade: what the cache lets go of (no longer kept: cache_expired) goes on casting for sun_fade seconds,
 * fainter and fainter (a share of its texels in the maps: GfxVsKey.shadow 2), rather than its shadow going all at once. What
 * moved (cache_moved) or drew from a buffer or texture that went (sun_forget) still goes at once. Drawn by the game again, it's
 * whole again. One whose shadow can reach no map (cache_unseen) goes at once: there's nothing of it to fade. 15 seconds by
 * default before, Kazham drew some 2300 entries fading a frame, each through the fading pipeline's discard (2026-10-28): 1
 * second now. */

static void cache_let_go(Cached* ce)
{
    if (!ce->fade_ns)
        ce->fade_ns = g_fade_now ? g_fade_now : 1;
}

/* how much of entry ce still casts: 1 whole, under 1 fading (eased in and out), 0 gone */
static float cache_share(const Cached* ce)
{
    if (!ce->fade_ns)
        return 1.0f;
    const double len = fmin(fmax((double)g_fxs.sun_fade, 0.0), 120.0) * 1e9;
    if (!(len > 0.0) || (double)(g_fade_now - ce->fade_ns) >= len)
        return 0.0f;
    const float t = (float)((double)(g_fade_now - ce->fade_ns) / len);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* let go of and faded: dropped (sun_cache_trim, snap_trim) */
static int cache_done(const Cached* ce)
{
    return ce->fade_ns && cache_share(ce) <= 0.0f;
}

/* --- the shadow cache's copies ------------------------------------------------------------------------------
 * An object placed by a world matrix (GfxDraw.anchor: a tree, a prop) that the cache above can't draw
 * again from the game's buffers - they're rewritten: a dynamic buffer, or a static one written again
 * (FFXI rewrites its trees as they sway) - is kept as a copy: its vertices and indices as last drawn, in
 * chunks of the back end's own, with its uniforms and that frame's camera, and drawn into the map from
 * there while it's out of view, as the cache's are. Characters (their vertices in the world already, no
 * anchor) aren't kept: they move. One whose place is in plain view near the camera but that the game
 * didn't draw is gone - it moved, or another LOD of it is drawn - and is dropped. */
#define SNAP_MAX 4096
#define SNAP_CHUNK (4u << 20)
#define SNAP_CHUNKS 12 /* 48 MB at most */

typedef struct SnapKey
{
    LibKey lib;
    GfxTex* tex[8];
    uint32_t n, vstart, count, prim, index_size, size[GFX_NSTREAMS];
    /* the vertices the copy starts at (the first index in it, as vofs takes it there) and their strides: a draw of
     * another range of the same buffer, the same size, is another copy - the copy's indices taken through another
     * range's vofs read outside it (past its chunk: the GPU faults) */
    int32_t vofs, stride[GFX_NSTREAMS];
    int32_t at[3]; /* where it stands, in quarter units */
} SnapKey;

typedef struct Snap
{
    Cached k;      /* drawn as the cache's are; its Caster draws from its chunk */
    SnapKey key;
    float at[3];   /* where it stands */
    int chunk;     /* the chunk holding its copies; -1 none */
} Snap;

typedef struct SnapChunk
{
    GfxBuf* buf;
    uint32_t used, live; /* bytes handed out; the copies in it still kept */
} SnapChunk;

static Snap* g_snaps;
static uint32_t g_nsnaps, g_snaps_cap;
static KeyMap g_snap_map; /* SnapKey -> index + 1 */
static SnapChunk g_chunks[SNAP_CHUNKS];
static int g_chunk_at = -1; /* the chunk new copies go into */

static void chunk_free(int i)
{
    gfx_buf_destroy(g_chunks[i].buf);
    memset(&g_chunks[i], 0, sizeof g_chunks[i]);
    if (g_chunk_at == i)
        g_chunk_at = -1;
}

/* a chunk with room for `bytes` more (a new one when the one filling up is full); -1 when there's none */
static int snap_chunk(uint32_t bytes)
{
    if (bytes > SNAP_CHUNK)
        return -1;
    if (g_chunk_at >= 0 && g_chunks[g_chunk_at].live == 0)
        g_chunks[g_chunk_at].used = 0; /* every copy in it went: from the start again */
    if (g_chunk_at >= 0 && g_chunks[g_chunk_at].used + bytes <= SNAP_CHUNK)
        return g_chunk_at;
    for (int i = 0; i < SNAP_CHUNKS; ++i)
        if (!g_chunks[i].buf)
        {
            g_chunks[i].buf = gfx_buf_create(SNAP_CHUNK);
            if (!g_chunks[i].buf)
                return -1;
            /* the full one is let go of once its last copy is (snap_release) */
            if (g_chunk_at >= 0 && g_chunks[g_chunk_at].live == 0)
                chunk_free(g_chunk_at);
            g_chunk_at = i;
            return i;
        }
    return -1;
}

static int snap_room(uint32_t bytes)
{
    if (g_chunk_at >= 0 && (g_chunks[g_chunk_at].live == 0 || g_chunks[g_chunk_at].used + bytes <= SNAP_CHUNK))
        return 1;
    for (int i = 0; i < SNAP_CHUNKS; ++i)
        if (!g_chunks[i].buf)
            return 1;
    return 0;
}

static void snap_release(Snap* sp)
{
    free(sp->k.u);
    sp->k.u = NULL;
    if (sp->chunk >= 0 && --g_chunks[sp->chunk].live == 0 && sp->chunk != g_chunk_at)
        chunk_free(sp->chunk);
    sp->chunk = -1;
}

static int snap_newest_first(const void* a, const void* b)
{
    const uint64_t x = ((const Snap*)a)->k.seen, y = ((const Snap*)b)->k.seen;
    return x > y ? -1 : x < y ? 1 : 0;
}

/* drops what went, and what's no longer kept (cache_expired; all of it when all is true); with
 * `room`, the older half too; the map made again */
static void snap_trim(int all, int room)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nsnaps; ++i)
    {
        Snap* sp = &g_snaps[i];
        if (cache_expired(&sp->k))
            cache_let_go(&sp->k);
        if (all || sp->k.c.dead || sp->chunk < 0 || cache_done(&sp->k))
            snap_release(sp);
        else
            g_snaps[n++] = *sp;
    }
    g_nsnaps = n;
    if (room && g_nsnaps > 1)
    {
        qsort(g_snaps, g_nsnaps, sizeof(Snap), snap_newest_first);
        const uint32_t keep = g_nsnaps / 2;
        for (uint32_t i = keep; i < g_nsnaps; ++i)
            snap_release(&g_snaps[i]);
        g_nsnaps = keep;
    }
    kmap_clear(&g_snap_map, g_nsnaps, sizeof(SnapKey));
    for (uint32_t i = 0; i < g_nsnaps; ++i)
        kmap_set(&g_snap_map, &g_snaps[i].key, sizeof(SnapKey), i + 1);
}

static Snap* snap_find(const SnapKey* k)
{
    const uint32_t at = kmap_get(&g_snap_map, k, sizeof *k);
    return at ? &g_snaps[at - 1] : NULL;
}

/* a new copy's place, cleared, kept by k (no chunk yet); NULL when there is no room for another */
static Snap* snap_add(const SnapKey* k)
{
    if (g_nsnaps == g_snaps_cap)
    {
        const uint32_t cap = g_snaps_cap ? g_snaps_cap * 2 : 256;
        Snap* more = (Snap*)realloc(g_snaps, cap * sizeof(Snap));
        if (!more)
            return NULL;
        g_snaps = more, g_snaps_cap = cap;
    }
    Snap* sp = &g_snaps[g_nsnaps++];
    memset(sp, 0, sizeof *sp);
    sp->key = *k, sp->chunk = -1;
    kmap_set(&g_snap_map, k, sizeof *k, g_nsnaps);
    return sp;
}

/* A caster just drawn: kept as a copy if it's one the cache can't draw again (above), or brought up to
 * date (its uniforms: this frame's camera) if it's kept already. */
/* the world's shift since the frame before (the land the boats sail past: drift_update, below the cache) */
static float g_drift[3];
static int g_drift_on;
static double g_drift_sum[3];
static uint32_t g_drift_n;

/* The shadows' casters over the last few seconds, for the log (fx_stats): what the frames' casters were,
 * and what the cache and its copies kept and drew */
static struct
{
    uint64_t frames, casters, fixed, placed, placed_fixed, made, refused_cpu, refused_room, cache_drawn, copies_drawn, stale, gone,
        culled;
    uint64_t in_view; /* of gone: let go of as in plain view and not drawn - none now, what's seen casting is kept for good */
    uint64_t moved; /* entries moved along with the world's drift (the land the boats sail past) rather than taken again */
    uint64_t own; /* casters from buffers the game rewrites, read as drawn from the ring (caster_bytes) */
    uint64_t sought; /* casters the caster pass drew, out of the camera's view (caster_only) */
    uint64_t fading; /* kept casters drawn fading (cache_let_go) */
    uint64_t late;   /* the world's pieces the game drew after the effects, taken into the cache for the next frame (sun_cache_late) */
    uint64_t still_frames, still_drawn; /* frames the far map was made from its still layer (sun_far_hold), and the layer drawn */
    uint64_t near_frames, near_drawn;   /* ... the sharp map from its own (sun_near_hold) */
    uint64_t why_far[7], why_near[7];   /* the layers drawn again, by why (WHY_AGE on, one slot a bit) */
    uint64_t radius_far, radius_near;   /* ... of them through a sphere of another radius than the layer's before (sun_layer_note) */
    uint32_t range_set;                 /* the sharp map's radius, the player's distance and the reach, the least and most (sun_map) */
    float near_lo, near_hi, to_lo, to_hi, reach_lo, reach_hi;
    uint64_t since;
} g_sst;

/* the reasons a still layer was drawn again (WHY_AGE on, a bit each), as text: "age, moved" */
static const char* const WHY_NAME[7] = { "age", "gap", "settings", "sun", "radius", "moved", "shrunk" };

static void sun_why_text(uint32_t why, char* out, size_t cap)
{
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < 7 && at + 1 < cap; ++i)
        if (why & (1u << i))
        {
            const int w = snprintf(out + at, cap - at, "%s%s", at ? ", " : "", WHY_NAME[i]);
            at = w < 0 || (size_t)w >= cap - at ? cap - 1 : at + (size_t)w;
        }
}

/* ... and counted over a while (g_sst.why_far, why_near): "age 12, moved 3" */
static void sun_why_counts(const uint64_t* n, char* out, size_t cap)
{
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < 7 && at + 1 < cap; ++i)
        if (n[i])
        {
            const int w = snprintf(out + at, cap - at, "%s%s %llu", at ? ", " : "", WHY_NAME[i], (unsigned long long)n[i]);
            at = w < 0 || (size_t)w >= cap - at ? cap - 1 : at + (size_t)w;
        }
    if (!at)
        snprintf(out, cap, "none");
}

static void fx_stats(void)
{
    if (!gfx_profiling) /* a line every 5 s: only with [graphics] profile or a /vana debug recording */
        return;
    static uint32_t epoch;
    if (epoch != gfx_profile_epoch) /* the profile just came on: the casters counted while it was off aren't these frames' */
        epoch = gfx_profile_epoch, memset(&g_sst, 0, sizeof g_sst);
    g_sst.frames++;
    const uint64_t now = gfx_now_ns();
    if (!g_sst.since)
        g_sst.since = now;
    if (now - g_sst.since < 5000000000ull)
        return;
    const double f = (double)g_sst.frames;
    fprintf(stderr, "[recomp] gfx: shadows, a frame over the last 5 s: %.0f casters (%.0f the cache can keep; %.0f placed objects, "
                    "%.0f of them the cache can keep; %.0f from buffers the game rewrites, read as drawn; %.0f the caster pass's, "
                    "round you); the cache holds %u (%u the pass's draws, by piece), %.0f "
                    "drawn into the maps; %.0f left out past the maps' sides; copies held %u, %.0f drawn; "
                    "%llu copies made, %llu refused (%llu without the vertices, %llu for room), %llu dropped as seen gone; the cache %llu "
                    "let go of (%llu in plain view, not drawn); %.0f drawn fading; the far map from its still layer %llu frames, the layer drawn %llu times; the sharp "
                    "map from its own %llu frames, drawn %llu times; %llu moved along (drift %.2f %.2f %.2f a frame); %.0f drawn after "
                    "the effects, taken for the next frame\n",
        g_sst.casters / f, g_sst.fixed / f, g_sst.placed / f, g_sst.placed_fixed / f, g_sst.own / f, g_sst.sought / f, g_ncache,
        g_piece_map.n, g_sst.cache_drawn / f,
        g_sst.culled / f, g_nsnaps, g_sst.copies_drawn / f, (unsigned long long)g_sst.made,
        (unsigned long long)(g_sst.refused_cpu + g_sst.refused_room), (unsigned long long)g_sst.refused_cpu,
        (unsigned long long)g_sst.refused_room, (unsigned long long)g_sst.stale, (unsigned long long)g_sst.gone,
        (unsigned long long)g_sst.in_view, g_sst.fading / f,
        (unsigned long long)g_sst.still_frames, (unsigned long long)g_sst.still_drawn, (unsigned long long)g_sst.near_frames,
        (unsigned long long)g_sst.near_drawn, (unsigned long long)g_sst.moved,
        g_drift[0], g_drift[1], g_drift[2], g_sst.late / f);
    /* the maps' cascades (the flicker's usual cause: a map cut through a sphere of another size puts every edge in it on a new
     * grid): the sharp map's radius and what it follows, and why the layers were drawn again */
    if (g_sst.range_set)
    {
        char why_f[96], why_n[96]; /* (not far and near: windows.h defines those away) */
        sun_why_counts(g_sst.why_far, why_f, sizeof why_f), sun_why_counts(g_sst.why_near, why_n, sizeof why_n);
        fprintf(stderr, "[recomp] gfx: shadows, the maps over the last 5 s: the sharp map's radius %.0f..%.0f units (reach %.1f..%.1f, the "
                        "player %.1f..%.1f from the camera); layers drawn again: far %llu (%s), sharp %llu (%s); through another radius "
                        "than the layer before's: far %llu, sharp %llu\n",
            g_sst.near_lo, g_sst.near_hi, g_sst.reach_lo, g_sst.reach_hi, g_sst.to_lo, g_sst.to_hi, (unsigned long long)g_sst.still_drawn,
            why_f, (unsigned long long)g_sst.near_drawn, why_n, (unsigned long long)g_sst.radius_far,
            (unsigned long long)g_sst.radius_near);
    }
    memset(&g_sst, 0, sizeof g_sst);
    g_sst.since = now;
}

static int snap_note(const GfxDraw* d, const Caster* rec)
{
    if (!rec)
        return 0;
    g_sst.casters++, g_sst.fixed += rec->fixed, g_sst.own += rec->own, g_sst.sought += rec->sought;
    if (d->anchor[3] != 0.0f)
        g_sst.placed++, g_sst.placed_fixed += rec->fixed;
    if (rec->fixed || rec->dead || d->anchor[3] == 0.0f || d->prim == GFX_TRIANGLEFAN || g_fxs.sun_casters == 1.0f)
        return 0;
    SnapKey k;
    memset(&k, 0, sizeof k);
    k.lib = rec->lib;
    memcpy(k.tex, rec->tex, sizeof k.tex);
    k.n = rec->n, k.vstart = d->vertex_start, k.count = d->count, k.prim = d->prim;
    k.vofs = d->u.vofs;
    memcpy(k.stride, d->u.stride, sizeof k.stride);
    k.index_size = d->indices ? d->index_size : 0;
    uint32_t bytes = 0;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        if (!d->buf[s] && !(d->data[s] && d->size[s]))
            continue;
        if (!d->cpu[s] || !d->cpu_size[s])
        {
            g_sst.refused_cpu++;
            return 0;
        }
        k.size[s] = d->cpu_size[s];
        bytes += (k.size[s] + 15) & ~15u;
    }
    bytes += (rec->n * k.index_size + 15) & ~15u;
    for (int j = 0; j < 3; ++j)
        k.at[j] = (int32_t)floorf(d->anchor[j] * 4.0f + 0.5f);
    Snap* sp = snap_find(&k);
    if (!sp || sp->k.c.dead || sp->chunk < 0)
    {
        /* a copy made (again) */
        if (sp || g_nsnaps >= SNAP_MAX || !snap_room(bytes))
        {
            snap_trim(0, g_nsnaps >= SNAP_MAX || !snap_room(bytes));
            sp = snap_find(&k);
        }
        const int ch = g_nsnaps < SNAP_MAX ? snap_chunk(bytes) : -1;
        if (ch < 0)
        {
            g_sst.refused_room++;
            return 0;
        }
        if (sp)
            snap_release(sp);
        else if (!(sp = snap_add(&k)))
        {
            g_sst.refused_room++;
            return 0;
        }
        g_sst.made++;
        SnapChunk* c = &g_chunks[ch];
        Caster* cs = &sp->k.c;
        *cs = *rec;
        cs->fixed = 1, cs->dead = 0, cs->cpu_u = ~0u, cs->own = 0;
        for (int j = 0; j < 8; ++j) /* the textures it samples: their going ends it (gfx_tex_destroy, sun_forget) */
            if (cs->tex[j])
                cs->tex[j]->sun_cached = 1;
        const D3D12_GPU_VIRTUAL_ADDRESS dummy = ID3D12Resource_GetGPUVirtualAddress(g_dummy);
        for (int s = 0; s <= GFX_NSTREAMS; ++s)
            cs->buf[s] = NULL; /* its copies are its own: no buffer of the game's ends them */
        for (int s = 0; s < GFX_NSTREAMS; ++s)
        {
            cs->vb[s] = dummy;
            if (!k.size[s])
                continue;
            buf_upload_at(c->buf, c->used, d->cpu[s], k.size[s]);
            cs->vb[s] = c->buf->gpu + c->used;
            c->used += (k.size[s] + 15) & ~15u;
        }
        memset(&cs->ibv, 0, sizeof cs->ibv);
        if (k.index_size)
        {
            const uint32_t ib = rec->n * k.index_size;
            buf_upload_at(c->buf, c->used, d->indices, ib);
            cs->ibv.BufferLocation = c->buf->gpu + c->used, cs->ibv.SizeInBytes = ib;
            cs->ibv.Format = k.index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            c->used += (ib + 15) & ~15u;
        }
        c->live++;
        sp->chunk = ch;
        sp->k.u = (GfxU*)malloc(sizeof(GfxU));
    }
    sp->k.seen = g_serial, sp->k.seen_ns = gfx_now_ns(), sp->k.fade_ns = 0; /* its camera is taken when the scene is done (sun_cache_update) */
    sp->k.sought |= rec->sought; /* (sticks: the pass's copy stands for the game's own sway of it too, snap_drawn) */
    memcpy(sp->at, d->anchor, 12);
    sp->k.c.need = rec->need;
    sp->k.c.world = rec->world; /* (placed as the uniforms it takes now are: the pass's world matrix, or the game's through its camera) */
    sun_u_copy(sp->k.u, &d->u, &sp->k.c);
    return sp->k.sought;
}

/* copy sp is drawn into a map this frame (sun_draw). With the cache off or the caster pass running (still_only), the pass's alone,
 * while it runs, each as first copied, in place of its draws (live_left_out): a tree the game sways by rewriting it, which the pass
 * draws every few frames, not every frame (casters.cpp kRoundWalks) - one not drawn again for PIECE_KEEP frames is gone. Else
 * those the game didn't draw this frame, kept for good as the cache's entries are, but what's let go of (cache_expired) */
static int snap_drawn(Snap* sp, int still_only)
{
    Cached* ce = &sp->k;
    if (sp->chunk < 0)
        return 0;
    if (still_only)
    {
        if (!ce->sought || !sun_pass_only())
            return 0;
        if (g_serial - ce->seen > PIECE_KEEP)
            ce->c.dead = 1;
        return !ce->c.dead;
    }
    if (!ce->c.dead && ce->seen != g_serial && (ce->fade_ns || cache_expired(ce)) && cache_unseen(ce))
        ce->c.dead = 1; /* let go of out of the maps' reach: nothing to fade */
    else if (!ce->c.dead && ce->seen != g_serial && cache_expired(ce))
        cache_let_go(ce); /* fading (sun_fade) */
    return !(ce->c.dead || ce->seen == g_serial || cache_done(ce));
}

/* A LibKey's hash: a frame's thousands of casters share a few dozen, so the last ones' are kept (by a few of their words,
 * checked whole) rather than ~250 bytes hashed for each */
static uint64_t lib_hash(const LibKey* lib)
{
    static struct
    {
        LibKey k;
        uint64_t h;
    } memo[64];
    _Static_assert(sizeof(LibKey) % 8 == 0, "lib_hash reads a LibKey in pairs of words");
    const uint32_t* w = (const uint32_t*)lib;
    uint32_t q = 0;
    for (size_t j = 0; j < sizeof(LibKey) / 4; j += 2)
        q = (q ^ w[j]) * 0x01000193u;
    q ^= q >> 15;
    uint32_t at = q & 63;
    if (memo[at].h && !memcmp(&memo[at].k, lib, sizeof *lib))
        return memo[at].h;
    memo[at].k = *lib, memo[at].h = fnv(lib, sizeof *lib);
    return memo[at].h;
}

/* the hash of caster c's CacheKey (its buffers, range and LibKey: what cache_same compares) */
static uint64_t cache_hash(const Caster* c)
{
    struct
    {
        D3D12_GPU_VIRTUAL_ADDRESS vb[GFX_NSTREAMS], ib;
        uint32_t n, vstart;
    } k;
    _Static_assert(sizeof k == 8 * (GFX_NSTREAMS + 2), "cache_hash's key has no padding");
    memcpy(k.vb, c->vb, sizeof k.vb);
    k.ib = c->ibv.BufferLocation, k.n = c->n, k.vstart = c->vstart;
    return fnv(&k, sizeof k) ^ lib_hash(&c->lib);
}

/* The zone's copies of one mesh - FFXI draws a zone's trees of a kind from one buffer, each placed by its
 * vertex shader's constants - share a key: they're told apart by where each stands (its first vertex, run
 * through its function on the CPU: gfx_draw_clip0, taken back to the world through that frame's camera).
 * The map holds them by cell (cache_link). */
static int caster_at(const Caster* c, const float* clip_world, float at[3])
{
    if (!c->has_clip0)
        return 0;
    if (c->world && fabsf(c->clip0[3]) > 1e-20f) /* the caster pass's in world space: clip0 is where it stands already */
    {
        for (int j = 0; j < 3; ++j)
            at[j] = c->clip0[j] / c->clip0[3];
        return isfinite(at[0]) && isfinite(at[1]) && isfinite(at[2]);
    }
    float w[4];
    for (int j = 0; j < 4; ++j)
        w[j] = c->clip0[0] * clip_world[j] + c->clip0[1] * clip_world[4 + j] + c->clip0[2] * clip_world[8 + j] + c->clip0[3] * clip_world[12 + j];
    if (!(fabsf(w[3]) > 1e-20f))
        return 0;
    for (int j = 0; j < 3; ++j)
        at[j] = w[j] / w[3];
    return isfinite(at[0]) && isfinite(at[1]) && isfinite(at[2]);
}

/* The map's cells: a key's entries standing within CACHE_CELL of each other share one, chained (Cached.next) from
 * the entry the map holds for it. A copy drawn again stands within a unit of where it stood (cache_find), so the
 * cells that unit round it reaches - 8 at most - are all that is looked in. One with no place known has a cell of
 * its own. (A key's copies were one chain, walked for each caster drawn: thousands of copies of a zone's rock or
 * tree made each frame's cost grow as their square - Abyssea - Konschtat's frames 10 ms longer, 2026-10-09.) */
#define CACHE_CELL 2.0f

typedef struct CellKey
{
    uint32_t key[2]; /* the CacheKey's hash */
    int32_t c[3];    /* the cell; INT32_MIN in all three: no place known */
} CellKey;

static int32_t cell_coord(float v)
{
    const float c = floorf(v / CACHE_CELL);
    return c < -1e9f ? -1000000000 : c > 1e9f ? 1000000000 : (int32_t)c;
}

static void cell_key(CellKey* ck, uint64_t kh, const float* pos)
{
    ck->key[0] = (uint32_t)kh, ck->key[1] = (uint32_t)(kh >> 32);
    for (int j = 0; j < 3; ++j)
        ck->c[j] = pos ? cell_coord(pos[j]) : INT32_MIN;
}

/* ce is a copy of c's mesh (their CacheKeys match; the hashes did) */
static int cache_same(const Cached* ce, const Caster* c)
{
    return ce->c.n == c->n && ce->c.vstart == c->vstart && ce->c.ibv.BufferLocation == c->ibv.BufferLocation &&
        !memcmp(ce->c.vb, c->vb, sizeof c->vb) && !memcmp(&ce->c.lib, &c->lib, sizeof c->lib);
}

/* the entry for caster c (its key's hash kh) standing at pos (NULL: not known): the nearest within a unit not yet
 * brought up to date this frame, else the one at the very place that was (the same drawn again) */
static Cached* cache_find(const Caster* c, uint64_t kh, const float* pos)
{
    CellKey ck;
    cell_key(&ck, kh, pos);
    if (!pos)
    {
        for (uint32_t i = kmap_get(&g_cache_map, &ck, sizeof ck); i; i = g_cache[i - 1].next)
            if (!g_cache[i - 1].has_at && cache_same(&g_cache[i - 1], c))
                return &g_cache[i - 1];
        return NULL;
    }
    int side[3]; /* the neighbouring cell a unit round pos reaches into, along each axis */
    for (int j = 0; j < 3; ++j)
        side[j] = pos[j] / CACHE_CELL - floorf(pos[j] / CACHE_CELL) < 0.5f ? -1 : 1;
    Cached *best = NULL, *again = NULL;
    float bd = 1.0f;
    for (int n = 0; n < 8; ++n)
    {
        CellKey k = ck;
        for (int j = 0; j < 3; ++j)
            if (n & (1 << j))
                k.c[j] += side[j];
        for (uint32_t i = kmap_get(&g_cache_map, &k, sizeof k); i; i = g_cache[i - 1].next)
        {
            Cached* ce = &g_cache[i - 1];
            if (!ce->has_at || !cache_same(ce, c))
                continue;
            const float dx = ce->at[0] - pos[0], dy = ce->at[1] - pos[1], dz = ce->at[2] - pos[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (ce->seen != g_serial)
            {
                if (d < bd)
                    bd = d, best = ce;
            }
            else if (d < 1e-4f && !again)
                again = ce;
        }
        if (n == 0 && best && bd < 1e-4f)
            return best; /* drawn again where it stood: nothing nearer */
    }
    return best ? best : again;
}

/* entry i into its cell (the one it stands in as it is now: one that moves stays in the cell it was put in until
 * sun_cache_trim, a unit's move still in reach of cache_find) */
static void cache_link(uint32_t i)
{
    CellKey ck;
    cell_key(&ck, g_cache[i].kh, g_cache[i].has_at ? g_cache[i].at : NULL);
    const uint32_t head = kmap_get(&g_cache_map, &ck, sizeof ck);
    if (!head)
    {
        /* (into the cell's slot when it's there, emptied by cache_unlink: a second slot of its key behind that one
         * was never found) */
        g_cache[i].next = 0;
        kmap_set(&g_cache_map, &ck, sizeof ck, i + 1);
    }
    else
        g_cache[i].next = g_cache[head - 1].next, g_cache[head - 1].next = i + 1;
}

/* --- what moves as one: the land the boats sail past ---------------------------------------------------------
 * The world's shift since the frame before (the ship's way, in units a frame), the mean of the pieces found moved
 * this frame (cache_moved; cache_find at where a piece stood the frame before). A caster not found where it stands
 * is looked for at where it would have stood a frame ago, and its entry moved along (cache_relink) rather than the
 * mesh copied again: 1,300 pieces of coast taken again every frame piled up 7 million cells and ran the process out
 * of memory in a minute (2026-10-21). Nothing moving, no piece is found moved and nothing is looked for twice. */
static void drift_note(const float* from, const float* to)
{
    for (int j = 0; j < 3; ++j)
        g_drift_sum[j] += (double)to[j] - from[j];
    g_drift_n++;
}

/* the frame's drift from what it found moved; without enough found, the last stays (the ship stopped: nothing is
 * looked for twice while every piece is found where it stands) */
static void drift_update(void)
{
    if (g_drift_n >= 4)
    {
        for (int j = 0; j < 3; ++j)
            g_drift[j] = (float)(g_drift_sum[j] / g_drift_n);
        g_drift_on = g_drift[0] * g_drift[0] + g_drift[1] * g_drift[1] + g_drift[2] * g_drift[2] > 1e-4f;
    }
    g_drift_n = 0;
    memset(g_drift_sum, 0, sizeof g_drift_sum);
}

/* the cell ck's chain to start at entry head (index + 1; 0: none) */
static void cache_cell_set(const CellKey* ck, uint32_t head)
{
    KeySlot* e = kmap_find(&g_cache_map, ck, sizeof *ck, fnv(ck, sizeof *ck));
    if (e)
        e->v = head;
    else if (head)
        kmap_set(&g_cache_map, ck, sizeof *ck, head);
}

/* entry i out of the chain of the cell it was linked into, standing at old_at then (NULL: no place known) */
static void cache_unlink(uint32_t i, const float* old_at)
{
    CellKey ck;
    cell_key(&ck, g_cache[i].kh, old_at);
    uint32_t h = kmap_get(&g_cache_map, &ck, sizeof ck);
    if (h == i + 1)
    {
        cache_cell_set(&ck, g_cache[i].next);
        return;
    }
    for (; h; h = g_cache[h - 1].next)
        if (g_cache[h - 1].next == i + 1)
        {
            g_cache[h - 1].next = g_cache[i].next;
            return;
        }
}

/* entry i (at and has_at as they are now) into the cell it stands in, if that's not the one it was linked into
 * (standing at old_at, when had_at) */
static void cache_relink(uint32_t i, int had_at, const float* old_at)
{
    const Cached* ce = &g_cache[i];
    int same = had_at == ce->has_at;
    for (int j = 0; same && had_at && j < 3; ++j)
        same = cell_coord(old_at[j]) == cell_coord(ce->at[j]);
    if (same)
        return;
    cache_unlink(i, had_at ? old_at : NULL);
    cache_link(i);
}

/* The uniforms of the entries sun_cache_trim drops, kept for the entries made after it rather than freed: thousands
 * in its one frame (as the map's keys, KeyMap). What's more than the cache could soon want goes a few a frame. */
static GfxU** g_u_spare;
static uint32_t g_nu_spare, g_u_spare_cap;

static GfxU* cache_u_new(void)
{
    return g_nu_spare ? g_u_spare[--g_nu_spare] : (GfxU*)malloc(sizeof(GfxU));
}

static void cache_u_spare(GfxU* u)
{
    if (!u)
        return;
    if (g_nu_spare == g_u_spare_cap)
    {
        GfxU** more = (GfxU**)realloc(g_u_spare, (g_u_spare_cap ? g_u_spare_cap * 2 : 1024) * sizeof(GfxU*));
        if (!more)
        {
            free(u);
            return;
        }
        g_u_spare = more, g_u_spare_cap = g_u_spare_cap ? g_u_spare_cap * 2 : 1024;
    }
    g_u_spare[g_nu_spare++] = u;
}

static void cache_u_spare_trim(void)
{
    for (int k = 0; k < 16 && g_nu_spare > 256 + g_ncache / 4; ++k)
        free(g_u_spare[--g_nu_spare]);
}

/* a new entry at the cache's end, cleared, with its uniforms; NULL when there is no room for another (the draw isn't kept) */
static Cached* cache_add(void)
{
    if (g_ncache == g_cache_cap)
    {
        const uint32_t cap = g_cache_cap ? g_cache_cap * 2 : 1024;
        Cached* more = (Cached*)realloc(g_cache, cap * sizeof(Cached));
        if (!more)
            return NULL;
        g_cache = more, g_cache_cap = cap;
    }
    Cached* ce = &g_cache[g_ncache++];
    memset(ce, 0, sizeof *ce);
    ce->u = cache_u_new();
    return ce;
}

static uint32_t g_forgot[2];   /* the cache's live entries sun_forget let go of: the caster pass's, the rest (forget_note) */
static uint32_t g_forgot_id[3]; /* ... the first pieces among them */

/* a buffer or texture gone, or given new contents: this frame's casters and the cache's that draw
 * from it are drawn no more (the cache takes such a caster again when it is next drawn as it is) */
static void sun_forget(GfxBuf* b, GfxTex* t)
{
    g_forgot[0] = g_forgot[1] = 0;
    for (int pass = 0; pass < 4; ++pass)
    {
        /* (the set before this one's too, while entries of the cache wait on it: cache_take copies them as they are) */
        uint32_t n = pass == 3 ? (g_was.npend ? g_was.n : 0) : pass == 2 ? g_nsnaps : pass ? g_ncache : g_ncasters;
        for (uint32_t i = 0; i < n; ++i)
        {
            Caster* c = pass == 3 ? &g_was.c[i] : pass == 2 ? &g_snaps[i].k.c : pass ? &g_cache[i].c : &g_casters[i];
            int hit = 0;
            for (int j = 0; b && j <= GFX_NSTREAMS; ++j)
                hit |= c->buf[j] == b;
            for (int j = 0; t && j < 8; ++j)
                hit |= c->tex[j] == t;
            if (hit && pass == 1 && !c->dead)
            {
                if (c->piece && g_forgot[0] < 3)
                    g_forgot_id[g_forgot[0]] = c->piece;
                g_forgot[c->piece ? 0 : 1]++;
            }
            if (hit)
                c->dead = 1;
        }
    }
    if (b)
        b->sun_seen = 0, b->sun_cached = 0;
    if (t)
        t->sun_seen = 0, t->sun_cached = 0;
}

/* The flicker watch: a buffer the cache's casters draw from written again (gfx_buf_upload, before it counts the write: its
 * uploads and upload_frame are the last one's) or freed (gfx_buf_destroy), or a texture they sample freed, and what sun_forget
 * let go of for it, with the first pieces - the caster pass takes those pieces again when it next draws them. Every 4 s at most,
 * with how many were held back */
static void forget_note(const GfxBuf* b, const GfxTex* t, const char* what)
{
    static uint64_t told;
    static uint32_t held;
    const uint32_t pieces = g_forgot[0], other = g_forgot[1];
    g_forgot[0] = g_forgot[1] = 0;
    if ((!gfx_profiling && !gfx_watching) || !(pieces + other))
        return;
    if (told && g_serial - told < 480)
    {
        held++;
        return;
    }
    told = g_serial;
    char ids[40] = "";
    for (uint32_t k = 0; k < pieces && k < 3; ++k)
        snprintf(ids + strlen(ids), sizeof ids - strlen(ids), " %08X", g_forgot_id[k]);
    gfx_watch("a %s the shadows' cache draws from was %s (%u bytes, write %u, %llu frames after the one before): %u of the caster "
              "pass's draws (pieces%s) and %u other kept casters let go of (%u such lines held back before it)",
        b ? "buffer" : "texture", what, b ? b->size : 0u, b ? b->uploads + 1 : 0u,
        (unsigned long long)(b && b->uploads ? g_serial - b->upload_frame : 0), pieces, pieces ? ids : " none", other, held);
    held = 0;
}

/* The flicker watch: the caster pass's draws the cache couldn't take (cache_takes), by why - incomplete (the ring full), drawn
 * from memory of the draw's own or a buffer rewritten between draws (not fixed), no uniforms kept - every 4 s at most */
static void piece_refused_note(const Caster* c)
{
    static uint64_t window;
    static uint32_t why[3];
    why[c->dead ? 0 : !c->fixed ? 1 : 2]++;
    if ((!gfx_profiling && !gfx_watching) || g_serial - window < 480)
        return;
    gfx_watch("the shadows' cache couldn't take %u of the caster pass's draws in the last 4 s: %u incomplete, %u not from "
              "buffers the game keeps, %u without their uniforms", why[0] + why[1] + why[2], why[0], why[1], why[2]);
    window = g_serial, why[0] = why[1] = why[2] = 0;
}

/* entry ce as drawn as caster c (its uniforms in u, at c->cpu_u), through that scene's camera */
static void cache_take(Cached* ce, const Caster* c, const uint8_t* u, const float* clip_world, const float* cam)
{
    ce->c = *c;
    sun_u_copy(ce->u, u + c->cpu_u, c);
    memcpy(ce->clip_world, clip_world, 64);
    memcpy(ce->cam, cam, 12);
}

/* the entries still waiting on set s (not drawn again since) copied from it */
static void cache_pend_flush(CasterSet* s)
{
    const Caster* cs = s == &g_set ? g_casters : s->c;
    const uint8_t* u = s == &g_set ? g_caster_u : s->u;
    for (uint32_t k = 0; k < s->npend; ++k)
    {
        Cached* ce = &g_cache[s->pend[k]];
        if (ce->pend_gen == s->gen)
            cache_take(ce, &cs[ce->pend], u, s->clip_world, s->cam), ce->pend_gen = 0;
    }
    s->npend = 0;
}

/* The cache's entries sun_draw may draw this frame (not gone, not drawn by the game this frame, still kept: sun_cache_update),
 * in order, with where each stands: the thousands left out past a map's sides cost a look at these alone, not at the entries */
typedef struct CacheVis
{
    float at[3];
    uint32_t i; /* the entry; CACHE_VIS_AT: at is known (Cached.has_at) */
} CacheVis;
#define CACHE_VIS_AT 0x80000000u
static CacheVis* g_cache_vis;
static uint32_t g_ncache_vis, g_cache_vis_cap;

/* drops what's no longer kept (cache_expired), or went (all of it when all is true), and
 * rebuilds the map */
static void sun_cache_trim(int all)
{
    if (all) /* the entries waiting on a set of casters are dropped below; the rest are moved: brought up to date first */
        g_was.npend = g_set.npend = 0;
    else
        cache_pend_flush(&g_was), cache_pend_flush(&g_set);
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        if (cache_expired(&g_cache[i]))
            cache_let_go(&g_cache[i]);
        if (all || g_cache[i].c.dead || cache_done(&g_cache[i]))
            cache_u_spare(g_cache[i].u);
        else
            g_cache[n++] = g_cache[i];
    }
    g_ncache = n;
    g_ncache_vis = 0; /* (made again by sun_cache_update) */
    kmap_clear(&g_cache_map, n, sizeof(CellKey));
    kmap_clear(&g_piece_map, g_piece_map.n, sizeof(PieceKey));
    for (uint32_t i = 0; i < n; ++i)
    {
        const Caster* c = &g_cache[i].c;
        const PieceKey k = { c->piece, c->piece_draw };
        if (c->piece) /* (the pass's by piece alone, never matched by mesh and place: in no cell) */
            kmap_set(&g_piece_map, &k, sizeof k, i + 1);
        else
            cache_link(i);
    }
}

/* How far a piece of the zone may have moved since the frame before and still be the same one: the land the boats
 * sail past (Ship bound for Selbina/Mhaura) slides by a little each frame, out of reach of cache_find once it leaves
 * the cell it was put in, and was taken as new each frame while the old stayed kept (the camera never 40 units from
 * where it saw it, the keep's rule then) - 180,000 entries in a minute, the sun maps 90 ms a frame. */
#define SUN_MOVED_NEAR 6.0f

/* (SUN_REACH, how far past a map's sides a caster can still reach into it: with the cache's keep rules, above) */

/* ce (seen a frame or two ago, not this one) moved: a copy of its mesh new to the cache this frame (entries from
 * first on) stands within SUN_MOVED_NEAR of where it was */
static const Cached* cache_moved(const Cached* ce, uint32_t first)
{
    for (uint32_t j = first; j < g_ncache; ++j)
    {
        const Cached* nw = &g_cache[j];
        if (nw->c.piece || nw->kh != ce->kh || !nw->has_at || !cache_same(ce, &nw->c))
            continue;
        const float dx = nw->at[0] - ce->at[0], dy = nw->at[1] - ce->at[1], dz = nw->at[2] - ce->at[2];
        if (dx * dx + dy * dy + dz * dz < SUN_MOVED_NEAR * SUN_MOVED_NEAR)
            return nw;
    }
    return NULL;
}

/* this frame's fixed casters into the cache (new ones added, seen ones brought up to date), and what the
 * cache holds that stands in plain view but the game didn't draw dropped: it moved, or went (another LOD
 * of it is drawn, or the game draws it no more) */
/* caster i of this frame's seen by the cache (sun_cache_update's each; sun_cache_late's for the late ones): found by its key
 * and where it stands, or made; its entry brought up to date and this frame's caster given it */
/* a caster the cache may keep: drawn from buffers the game keeps, live, its uniforms kept; not the caster pass's in world space
 * (drawn every frame, and its uniforms aren't the camera's, which the cache keeps) but for a plant's, whose still pose is kept
 * (plant_still) and drawn by the map's own matrix (kept_matrix): the pass draws Kazham's palms placed by their world matrices, and
 * they swayed as drawn while it ran, none of their still poses used (2026-11-02 23:02) */
static int cache_takes(const Caster* c) { return c->fixed && !c->dead && c->cpu_u != ~0u && (!c->world || plant_still(c) || c->piece); }

/* the buffers and textures caster c draws from: the cache's now (sun_forget's) */
static void cache_holds(const Caster* c)
{
    for (int j = 0; j <= GFX_NSTREAMS; ++j)
        if (c->buf[j])
            c->buf[j]->sun_cached = 1;
    for (int j = 0; j < 8; ++j)
        if (c->tex[j])
            c->tex[j]->sun_cached = 1;
}

/* The caster pass's draw i, kept by its piece (PieceKey): its entry made, or brought up to date as drawn now - but a plant's still
 * pose (plant_still), kept as first drawn unless what it drew from went, or the piece draws another mesh there now (cache_same) */
static void piece_see(uint32_t i, const float* clip_world, const float* cam)
{
    const Caster* c = &g_casters[i];
    if (!cache_takes(c))
    {
        piece_refused_note(c);
        return;
    }
    const PieceKey k = { c->piece, c->piece_draw };
    const uint32_t found = kmap_get(&g_piece_map, &k, sizeof k);
    Cached* ce = found ? &g_cache[found - 1] : NULL;
    const int still = plant_still(c);
    if (!ce)
    {
        if (!(ce = cache_add()))
            return;
        kmap_set(&g_piece_map, &k, sizeof k, g_ncache);
        cache_take(ce, c, g_caster_u, clip_world, cam);
    }
    else if (!still || !ce->still || ce->c.dead || !cache_same(ce, c))
        cache_take(ce, c, g_caster_u, clip_world, cam), ce->pend_gen = 0;
    ce->kh = cache_hash(c);
    ce->has_at = (uint8_t)caster_at(&ce->c, ce->clip_world, ce->at);
    ce->seen = g_serial, ce->seen_ns = g_fade_now, ce->fade_ns = 0, ce->sought = 1, ce->still = (uint8_t)still;
    cache_holds(c);
    g_casters[i].entry = (uint32_t)(ce - g_cache) + 1;
}

/* caster c, a plant's (still: plant_still) or not, isn't the cache's: not one it takes; the cache off, but for a plant's still
 * pose; or the game's own draw of the zone while the caster pass draws it (sun_pass_only: the pass's, by piece, are what's drawn) */
static int cache_skips(const Caster* c, int still)
{
    return !cache_takes(c) || (!still && sun_cache_off()) || (c->fixed && !c->sought && sun_pass_only());
}

/* a caster first seen standing further from the player (the camera, without one) than the far map (sun_distance) and SUN_REACH: it
 * can reach no map until the player nears it, and the game draws it then - not kept (the coast beyond a ship: half of what the game
 * draws there is out of the maps' reach, 2026-10-21). Never the caster pass's: it takes a tall piece from farther, as far as its
 * shadow reaches */
static int cache_beyond(const Caster* c, const float* pos, const float* cam)
{
    if (c->sought)
        return 0;
    const float* from = g_cache_anchor_set ? g_cache_anchor : cam;
    const float dx = pos[0] - from[0], dy = pos[1] - from[1], dz = pos[2] - from[2];
    const float reach = fmaxf(g_fxs.sun_distance, 4.0f) + SUN_REACH;
    return dx * dx + dy * dy + dz * dz > reach * reach;
}

/* entry ce drawn again as this frame's caster i (cache_see): a plant's still pose kept as it is (plant_still: taken once, and again
 * only when what it drew from came back); the rest taken as drawn this frame - live again if something it drew from had gone, and
 * the same address now holds what the game draws - once it's out of view (cache_pend_flush); its key (cache_same) is i's already */
static void cache_seen_again(Cached* ce, uint32_t i, int still, const float* clip_world, const float* cam)
{
    if (still && ce->still && !ce->c.dead)
        return;
    if (still)
    {
        cache_take(ce, &g_casters[i], g_caster_u, clip_world, cam), ce->pend_gen = 0; /* still from this pose on */
        return;
    }
    if (g_set.npend == g_set.pend_cap)
    {
        g_set.pend_cap = g_set.pend_cap ? g_set.pend_cap * 2 : 1024;
        g_set.pend = (uint32_t*)realloc(g_set.pend, g_set.pend_cap * sizeof(uint32_t));
    }
    g_set.pend[g_set.npend++] = (uint32_t)(ce - g_cache);
    ce->pend = i, ce->pend_gen = g_set.gen;
}

static void cache_see(uint32_t i, const float* clip_world, const float* cam)
{
    const Caster* c = &g_casters[i];
    if (c->piece)
    {
        piece_see(i, clip_world, cam);
        return;
    }
    const int still = plant_still(c);
    if (cache_skips(c, still))
        return;
    const uint64_t kh = cache_hash(c);
    float pos[3];
    const int has_at = caster_at(c, clip_world, pos);
    Cached* ce = cache_find(c, kh, has_at ? pos : NULL);
    if (!ce && has_at && g_drift_on)
    {
        /* not where it stands: where it stood a frame ago, if the world is drifting (the land the boats sail past) */
        const float was[3] = {pos[0] - g_drift[0], pos[1] - g_drift[1], pos[2] - g_drift[2]};
        ce = cache_find(c, kh, was);
        if (ce && ce->seen == g_serial)
            ce = NULL; /* (cache_find's again: drawn this frame already, at its very place) */
        if (ce)
            drift_note(ce->at, pos), g_sst.moved++;
    }
    if (!ce && has_at && cache_beyond(c, pos, cam))
    {
        g_sst.culled++;
        return;
    }
    if (!ce)
    {
        if (!(ce = cache_add()))
            return;
        ce->kh = kh, ce->has_at = (uint8_t)has_at;
        if (has_at)
            memcpy(ce->at, pos, 12);
        cache_link(g_ncache - 1);
        cache_take(ce, c, g_caster_u, clip_world, cam); /* (a new one's key with it: cache_same) */
    }
    else
        cache_seen_again(ce, i, still, clip_world, cam);
    cache_holds(c);
    ce->seen = g_serial, ce->seen_ns = g_fade_now, ce->fade_ns = 0, ce->sought |= c->sought; /* (sought sticks: cache_beyond) */
    ce->still = (uint8_t)still;
    float old_at[3];
    const int had_at = ce->has_at;
    memcpy(old_at, ce->at, 12);
    ce->has_at = (uint8_t)has_at;
    if (has_at)
        memcpy(ce->at, pos, 12);
    cache_relink((uint32_t)(ce - g_cache), had_at, old_at); /* (moved along: into the cell it stands in now) */
    g_casters[i].entry = (uint32_t)(ce - g_cache) + 1; /* (its shadow pipeline shared with the entry: sun_draw) */
}

/* The world's pieces the game drew after the effects ran (d3d8.c scene_note's late casters), at the frame's end: too late for
 * this frame's maps, seen by the cache now, through the scene's camera, so they cast from the next frame on as what's out of
 * view does - and go on being drawn from it while the game keeps drawing them late (sun_cache_update's plain-view let-go
 * waits a frame for them). Before, they cast nothing: the game drew up to a third of a zone after the effects some frames
 * (Port Jeuno, 2026-11-01: 500 of 1900 draws for 25 s at a time, its own offscreen 3D laid over the world partway through
 * it), and a building's whole shadow came and went with the game's draw order as the camera moved. */
/* The flicker watch: the caster pass's draws that came after the effects ran, which sun_late 0 leaves out of the cache like the
 * game's own late draws - a piece the pass draws then isn't kept that round (said once every 4 s at most, with how many) */
static void late_pass_note(void)
{
    static uint64_t told;
    uint32_t n = 0, world = 0;
    for (uint32_t i = g_casters_scene; i < g_ncasters; ++i)
        n += g_casters[i].piece != 0, world += g_casters[i].piece != 0 && g_casters[i].world;
    if (n && (gfx_profiling || gfx_watching) && (!told || g_serial - told >= 480))
        told = g_serial, gfx_watch("%u of the caster pass's draws came after the effects this frame (%u placed in the world, %u "
                                   "through the camera): %s", n, world, n - world,
                             g_fxs.sun_late != 0.0f ? "taken for the next frame" : "not kept (sun_late 0)");
}

static void sun_cache_late(void)
{
    if (g_set.updated && g_ncasters > g_casters_scene)
        late_pass_note();
    if (!g_set.updated || g_ncasters <= g_casters_scene || g_fxs.sun_late == 0.0f) /* (sun_late 0: they cast nothing) */
        return;
    g_fade_now = gfx_now_ns();
    for (uint32_t i = g_casters_scene; i < g_ncasters; ++i)
        cache_see(i, g_set.clip_world, g_set.cam);
    g_sst.late += g_ncasters - g_casters_scene;
    g_casters_scene = g_ncasters;
}

/* The flicker watch: a draw of the caster pass's let go of, not drawn again for PIECE_KEEP frames - its shadow gone until the pass
 * takes it again (Ru'Lude Gardens, 2026-11-04: 38 of them every 432 frames, back 256 frames later). The first few every 2 s said
 * with the piece (d3d8_casters.h seek_piece_done names it too), its draw and where it stands; the rest counted */
static void piece_gone_note(const Cached* ce)
{
    static uint64_t window;
    static uint32_t said, held;
    if (!gfx_profiling && !gfx_watching)
        return;
    if (g_serial - window >= 240)
    {
        if (held)
            gfx_watch("... and %u more of the caster pass's draws let go of", held);
        window = g_serial, said = held = 0;
    }
    if (said++ >= 8)
    {
        held++;
        return;
    }
    gfx_watch("the caster pass's draw %u of piece %08X (at %.0f %.0f %.0f) let go of: not drawn again for %llu frames",
        ce->c.piece_draw, ce->c.piece, ce->at[0], ce->at[1], ce->at[2], (unsigned long long)(g_serial - ce->seen));
}

/* entry ce is one sun_draw may draw this frame (sun_cache_update's g_cache_vis); one that went is marked so: moved (a copy of its
 * mesh new this frame, from first_new on, stands near), let go of past the maps' reach, faded, or (off: the cache off) not a plant's
 * still pose. One the game drew this frame is drawn as it is, but for a plant's still pose, drawn in its place (sun_draw) */
static int cache_listed(Cached* ce, uint32_t first_new, int off)
{
    if (ce->c.dead)
        return 0;
    if (ce->c.piece) /* the caster pass's: drawn while the pass runs, kept while it draws the piece */
    {
        if (g_serial - ce->seen > PIECE_KEEP)
            ce->c.dead = 1, piece_gone_note(ce);
        return !ce->c.dead && sun_pass_only();
    }
    if (off && !cache_still(ce))
    {
        ce->c.dead = 1;
        return 0;
    }
    const int seen = ce->seen == g_serial;
    if (seen && !cache_still(ce))
        return 0;
    const Cached* nw;
    if (!seen && ce->has_at && ce->seen + 2 >= g_serial && first_new < g_ncache && (nw = cache_moved(ce, first_new)) != NULL)
    {
        drift_note(ce->at, nw->at);
        ce->c.dead = 1, g_sst.gone++;
        return 0;
    }
    if ((ce->fade_ns || cache_expired(ce)) && cache_unseen(ce))
    {
        g_sst.gone += ce->fade_ns == 0;
        ce->c.dead = 1; /* let go of, its shadow out of the maps' reach: nothing to fade */
        return 0;
    }
    /* (kept for good otherwise: a piece seen blocking the light blocks it still, whatever the camera sees now) */
    if (!ce->fade_ns && cache_expired(ce))
        cache_let_go(ce), g_sst.gone++;
    if (cache_done(ce))
    {
        ce->c.dead = 1;
        return 0;
    }
    return 1;
}

static void sun_cache_update(const GfxScene* s, const float* clip_world, const float* cam)
{
    g_fade_now = gfx_now_ns();
    /* [graphics.effects] sun_cache 0: nothing kept, and what was, let go of; this frame's casters alone cast - but for the plants'
     * still poses (plant_still) and the caster pass's draws (kept by piece: the pass draws each every few frames), kept all the same */
    const int off = sun_cache_off();
    if (off && g_fxs.sun_plants_still == 0.0f && g_fxs.sun_seek == 0.0f)
    {
        if (g_ncache || g_nsnaps)
            sun_cache_trim(1), snap_trim(1, 0);
        g_ncache_vis = 0, g_set.updated = 0;
        return;
    }
    if (off && g_nsnaps && !sun_pass_only()) /* (the pass's copies kept while it runs: snap_drawn) */
        snap_trim(1, 0);
    /* the player's jump (the camera's, without one): a new zone - not the camera's, which the free camera flies anywhere */
    const float* ref = s->focus[3] != 0.0f ? s->focus : cam;
    float dx = ref[0] - g_cache_anchor[0], dy = ref[1] - g_cache_anchor[1], dz = ref[2] - g_cache_anchor[2];
    if (g_cache_anchor_set && dx * dx + dy * dy + dz * dz > 50.0f * 50.0f)
    {
        if (gfx_profiling && g_ncache)
            fprintf(stderr, "[recomp] gfx: shadows: the player jumped %.0f units (frame %llu): cache cleared\n",
                sqrtf(dx * dx + dy * dy + dz * dz), (unsigned long long)g_serial);
        sun_cache_trim(1);
        snap_trim(1, 0);
        g_drift_on = 0;
        g_fx.fogc_set = 0;
        if (!g_sync_pipelines)
            g_fx.shadows_in = 0.0f; /* back in over a second as the zone is seen */
    }
    memcpy(g_cache_anchor, ref, 12); /* the player, for cache_expired and cache_see */
    g_cache_anchor_set = 1;
    if (!(g_serial & 255))
        sun_cache_trim(0), snap_trim(0, 0);
    cache_u_spare_trim();
    const uint32_t first_new = g_ncache; /* the entries this frame adds start here (cache_moved) */
    /* the copies drawn this frame: their uniforms were this frame's camera */
    for (uint32_t i = 0; i < g_nsnaps; ++i)
        if (g_snaps[i].k.seen == g_serial)
            memcpy(g_snaps[i].k.clip_world, clip_world, 64), memcpy(g_snaps[i].k.cam, cam, 12);
    if (g_set.updated) /* these casters were matched once already (a scene before this one, no casters_clear between): taken */
        cache_pend_flush(&g_set);
    memcpy(g_set.clip_world, clip_world, 64), memcpy(g_set.cam, cam, 12), g_set.updated = 1;
    for (uint32_t i = 0; i < g_ncasters; ++i)
        cache_see(i, clip_world, cam);
    g_casters_scene = g_ncasters; /* the ones after: drawn after the effects, seen at the frame's end (sun_cache_late) */
    cache_pend_flush(&g_was); /* drawn in the scene before, not in this one: taken as they were then */
    /* in plain view but not drawn: gone (fading, sun_fade; at once if it moved); the rest that sun_draw may draw, and those
     * fading, listed (g_cache_vis) */
    if (g_cache_vis_cap < g_ncache)
    {
        g_cache_vis_cap = g_ncache + g_ncache / 2 + 1024;
        free(g_cache_vis);
        g_cache_vis = (CacheVis*)malloc(g_cache_vis_cap * sizeof(CacheVis));
    }
    g_ncache_vis = 0;
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        Cached* ce = &g_cache[i];
        if (!cache_listed(ce, first_new, off))
            continue;
        CacheVis* v = &g_cache_vis[g_ncache_vis++];
        memcpy(v->at, ce->at, 12);
        v->i = i | (ce->has_at ? CACHE_VIS_AT : 0);
    }
    drift_update();
}
