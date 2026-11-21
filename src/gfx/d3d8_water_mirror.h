/* d3d8_water_mirror.h: an include unit of d3d8_water.h (in its scope: g_water_set, g_water_rects, g_water_tiles, g_bake,
 * g_bake_wet, g_scene, water_affine_inv). The world mirrored in VanaCore's water ([graphics] water.mirror).
 *
 * The water's pixel function mirrors the sky alone: a colour by the reflected ray's height, so a lake under a wooded shore was
 * a sheet of the sky's colour. With water.mirror the back end draws the frame's opaque world again from the camera mirrored
 * about one level of water (gfx_scene_mirror: the casters it keeps for the sun's maps, in colour, what stands above the level
 * alone) into a smaller target, and the pass's water at that level reads it at each pixel's own place on the screen, pushed
 * about by the waves' slopes, in place of the sky's colour wherever the world was drawn in it: the shore, the trees, walls
 * and cliffs upside down in the water, broken up by the swell. One mirror a frame, about the level of the water nearest the
 * camera (a lake's, a pond's, the sea's); water at another level keeps the sky (the pixel function fades the mirror out with
 * the surface's height from the level). Nothing with the camera under the level, in the debug view, or without the pass. */
#pragma once

#define MIRROR_SAME 0.3f  /* yalms: water within this of the mirror's level is at it */
#define MIRROR_STAY 0.75f /* another level takes the mirror over when it's nearer than this share of the held one's distance, */
#define MIRROR_STEP 3.0f  /* ... and by this many yalms: two ponds as near as each other don't trade it frame by frame */

static struct
{
    GfxTex* tex;    /* the mirror (the back end's), NULL none */
    uint32_t frame; /* the scene's frame it was made in */
    float level;    /* the level it mirrors about (the world's y) */
    int held;       /* a level is held (the frame before's: water_mirror_level) */
} g_wmirror;

/* a piece of water at height y over x0..x1, z0..z1, for the level's choice: its distance from the camera across the level (0
 * over it) against the nearest so far (best: the distance, then the height) and against the nearest at the held level (held) */
static void mirror_candidate(const float cam[3], float x0, float z0, float x1, float z1, float y, float best[2], float held[2])
{
    if (!(y > cam[1] + 0.05f)) /* the camera is under it (the world's y points down) */
        return;
    const float dx = cam[0] < x0 ? x0 - cam[0] : cam[0] > x1 ? cam[0] - x1 : 0.0f;
    const float dz = cam[2] < z0 ? z0 - cam[2] : cam[2] > z1 ? cam[2] - z1 : 0.0f;
    const float d = sqrtf(dx * dx + dz * dz);
    if (d < best[0])
        best[0] = d, best[1] = y;
    if (g_wmirror.held && fabsf(y - g_wmirror.level) <= MIRROR_SAME && d < held[0])
        held[0] = d, held[1] = y;
}

/* the height of a chunk's water where it lies nearest the camera: of some sixteen of its vertices spread through it (x, y, z and
 * the bed's depth each, in the pool), the nearest one's. A pond's are all at its level; a stream's chunk runs downhill, and its
 * box's middle is no height its water has by the camera */
static float mirror_chunk_level(const WaterBake* b, const float cam[3])
{
    float y = (b->lo[1] + b->hi[1]) * 0.5f, least = 1e30f;
    if (!g_bake_v || b->at + b->n > g_bake_nv)
        return y;
    const float* v = &g_bake_v[(size_t)b->at * BAKE_STRIDE];
    const uint32_t step = b->n > 16 ? b->n / 16 : 1;
    for (uint32_t i = 0; i < b->n; i += step)
    {
        const float dx = v[i * BAKE_STRIDE] - cam[0], dz = v[i * BAKE_STRIDE + 2] - cam[2];
        if (dx * dx + dz * dz < least)
            least = dx * dx + dz * dz, y = v[i * BAKE_STRIDE + 1];
    }
    return y;
}

/* The level the frame's mirror is made about: that of the water nearest the camera across the ground - this frame's tiles
 * and sheets (g_water_rects) and the ground's water drawn this frame or the one before (g_bake's chunks: a pond's, a stream's,
 * each at its water's height by the camera: mirror_chunk_level) - else the sea's (sea_y, with has_sea). The level held from
 * the frame before stays while its water is still about as near. 0 when there is none under the camera */
static int water_mirror_level(const float cam[3], int has_sea, float sea_y, float* level)
{
    float best[2] = { 1e30f, 0.0f }, held[2] = { 1e30f, 0.0f };
    const uint32_t n = g_water_tiles < WATER_RECTS ? g_water_tiles : WATER_RECTS;
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* r = g_water_rects[i];
        mirror_candidate(cam, r[0], r[1], r[2], r[3], r[4], best, held);
    }
    for (uint32_t i = 0; i < g_bake_wet_n; ++i)
    {
        const WaterBake* b = &g_bake[g_bake_wet[i]];
        if (b->n && b->seen + 1 >= g_scene.frame)
            mirror_candidate(cam, b->lo[0], b->lo[2], b->hi[0], b->hi[2], mirror_chunk_level(b, cam), best, held);
    }
    if (best[0] >= 1e30f)
    {
        if (!has_sea || !(sea_y > cam[1] + 0.05f))
        {
            g_wmirror.held = 0;
            return 0;
        }
        best[1] = sea_y;
    }
    else if (held[0] < 1e30f && !(best[0] < held[0] * MIRROR_STAY && best[0] + MIRROR_STEP < held[0]))
        best[1] = held[1]; /* (its height now: a stream's level goes on down with the camera) */
    g_wmirror.level = *level = best[1];
    g_wmirror.held = 1;
    return 1;
}

/* The frame's mirror, from water_pass before the water is drawn (the back end binds targets of its own: the pass binds the
 * scene's again after) */
static void water_mirror(const GfxTex* color, const GfxScene* s, int has_sea, float sea_y)
{
    float inv[16], level;
    g_wmirror.tex = NULL, g_wmirror_lost = 0;
    if (!(g_water_set.mirror > 0.0f) || !g_water_set.pass || g_water_set.view || !water_affine_inv(inv, s->view))
        return;
    if (!water_mirror_level(&inv[12], has_sea, sea_y, &level))
        return;
    g_wmirror.tex = gfx_scene_mirror(color, s, level, g_water_set.mirror_scale, g_water_set.mirror_reach);
    g_wmirror.frame = g_scene.frame;
    if (g_water_set.log)
    {
        static uint64_t said_ns;
        const uint64_t now = gfx_now_ns();
        if (now - said_ns > 2000000000ull) /* every two seconds */
            said_ns = now, rt_log("[recomp] water: the mirror about the level %.2f (the camera at %.2f): %s\n", level, inv[13],
                              g_wmirror.tex ? "drawn" : "none (no casters kept yet, or none drawn)");
    }
}

/* The mirror on one of the pass's draws (water_pass_draw): stage 3 (clamped, filtered), and texture matrix 2's bytes 0-4 its
 * strength (water.mirror), its level, how far the waves push it about (water.mirror.waves) and the projection's x and y
 * scales (the pixel function finds the sky a level surface mirrors on the screen by them). This frame's, or the frame
 * before's for a water draw that comes after the frame counter moved on (water_keep's late ones) */
static void water_mirror_bind(GfxDraw* d)
{
    if (!g_wmirror.tex || g_wmirror_lost || g_wmirror.frame + 1 < g_scene.frame)
        return;
    d->tex[3] = g_wmirror.tex;
    d->fs.st[3].tex = 1, d->fs.st[3].ncoord = 2;
    d->samp[3] = (GfxSampler){ 3, 3, 3, 2, 2, 0, 1, 0, 0, 0, { 0, 0 }, 0 }; /* CLAMP, LINEAR, no mips */
    float* m = d->u.texm[2];
    memset(m, 0, 64);
    m[0] = g_water_set.mirror, m[1] = g_wmirror.level, m[2] = g_water_set.mirror_waves;
    m[3] = g_scene.s.proj[0], m[4] = g_scene.s.proj[5]; /* (the sky's place on the screen by its direction) */
}
