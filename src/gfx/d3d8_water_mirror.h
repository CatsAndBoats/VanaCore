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

/* the camera's distance across the ground from x0..x1, z0..z1 (0 over it) */
static float mirror_rect_dist(const float cam[3], float x0, float z0, float x1, float z1)
{
    const float dx = cam[0] < x0 ? x0 - cam[0] : cam[0] > x1 ? cam[0] - x1 : 0.0f;
    const float dz = cam[2] < z0 ? z0 - cam[2] : cam[2] > z1 ? cam[2] - z1 : 0.0f;
    return sqrtf(dx * dx + dz * dz);
}

/* A piece of water for the level's choice: how far it lies from the camera across the ground (0 under it), its height, and
 * how much of it there is (its area over the ground) */
typedef struct MirrorWater { float d, y, area; } MirrorWater;

/* of two pieces of water at different heights, a is the one to mirror: the nearer by MIRROR_STEP or more, else, of two about
 * as near, the one there is more of. Lake Mechieume has thirteen chunks of specks afloat by its banks (a flat mesh of small
 * quads each, taken whole as water) 0.65 over the lake's flooded surface: from a bank the specks are as near as the lake, and
 * the mirror made about them stood a hand's width over the water one sees */
static int mirror_rather(const MirrorWater* a, const MirrorWater* b)
{
    if (a->d + MIRROR_STEP < b->d)
        return 1;
    if (b->d + MIRROR_STEP < a->d)
        return 0;
    return fabsf(a->y - b->y) <= MIRROR_SAME ? a->d < b->d : a->area > b->area;
}

/* a piece of water against the one to mirror so far (best) and against the nearest at the held level (held); d 1e30: none yet */
static void mirror_candidate(const float cam[3], const MirrorWater* w, MirrorWater* best, MirrorWater* held)
{
    if (!(w->y > cam[1] + 0.05f)) /* the camera is under it (the world's y points down) */
        return;
    if (best->d >= 1e30f || mirror_rather(w, best))
        *best = *w;
    if (g_wmirror.held && fabsf(w->y - g_wmirror.level) <= MIRROR_SAME && w->d < held->d)
        *held = *w;
}

/* A chunk's water against the view: 0 when its box's eight corners all lie past one side of what the camera sees (behind it,
 * or off one edge of the screen). The chunks the game drew this frame or the one before were the ones asked first, and that
 * was wrong twice over by Lake Mechieume: a flood carried on (flood_pump) is no mesh the game draws, so the lake's water had
 * no level of its own, and the mirror came and went as the camera turned the lake's one sheet, at the river's mouth, in and
 * out of the game's view */
static int mirror_in_view(const GfxScene* s, const float lo[3], const float hi[3])
{
    int out[5] = { 0, 0, 0, 0, 0 };
    for (int k = 0; k < 8; ++k)
    {
        const float p[3] = { k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2] };
        float v[4], c[4];
        for (int j = 0; j < 4; ++j)
            v[j] = p[0] * s->view[j] + p[1] * s->view[4 + j] + p[2] * s->view[8 + j] + s->view[12 + j];
        for (int j = 0; j < 4; ++j)
            c[j] = v[0] * s->proj[j] + v[1] * s->proj[4 + j] + v[2] * s->proj[8 + j] + v[3] * s->proj[12 + j];
        out[0] += c[3] <= 0.0f, out[1] += c[0] < -c[3], out[2] += c[0] > c[3], out[3] += c[1] < -c[3], out[4] += c[1] > c[3];
    }
    return out[0] < 8 && out[1] < 8 && out[2] < 8 && out[3] < 8 && out[4] < 8;
}

/* A chunk's water where it lies nearest the camera across the ground: how far (0 with the camera over it), its height there
 * and its area. By its triangles (x, y, z and the bed's depth a vertex, in the pool), each as its box over the ground: a
 * flood's are the halves of level quads, a row of cells each, so theirs is exact. The chunk's own box says too little: a
 * flood's has the banks in it, a stream's the ground it winds through. A pond's water is all at its level; a stream's runs
 * downhill, and the height is the nearest triangle's. The parts of it over the camera aren't asked. d 1e30: none */
static MirrorWater mirror_chunk_near(const WaterBake* b, const float cam[3])
{
    MirrorWater w = { 1e30f, (b->lo[1] + b->hi[1]) * 0.5f, 0.0f };
    if (!g_bake_v || b->at + b->n > g_bake_nv)
        return w;
    const float* v = &g_bake_v[(size_t)b->at * BAKE_STRIDE];
    for (uint32_t i = 0; i + 2 < b->n; i += 3, v += 3 * BAKE_STRIDE)
    {
        const float* q = v + BAKE_STRIDE;
        const float* r = q + BAKE_STRIDE;
        const float ty = (v[1] + q[1] + r[1]) * (1.0f / 3.0f);
        w.area += 0.5f * fabsf((q[0] - v[0]) * (r[2] - v[2]) - (r[0] - v[0]) * (q[2] - v[2]));
        if (!(ty > cam[1] + 0.05f))
            continue;
        const float d = mirror_rect_dist(cam, fminf(fminf(v[0], q[0]), r[0]), fminf(fminf(v[2], q[2]), r[2]),
                                         fmaxf(fmaxf(v[0], q[0]), r[0]), fmaxf(fmaxf(v[2], q[2]), r[2]));
        if (d < w.d)
            w.d = d, w.y = ty;
    }
    return w;
}

/* The level the frame's mirror is made about: that of the water nearest the camera across the ground - this frame's tiles
 * and sheets (g_water_rects) and the ground's water in the view (g_bake's chunks, as water_ground_pass draws them, the game's
 * draw or not: mirror_in_view; a pond's, a stream's, each where its water lies nearest the camera: mirror_chunk_near) - else
 * the sea's (sea_y, with has_sea). The level held from the frame before stays while its water is still about as near, unless
 * there is four times as much of the other and it is as near within MIRROR_STEP (a lake flooded in the frames since: the
 * specks by its bank were all there was). 0 when there is none under the camera */
static int water_mirror_level(const GfxScene* s, const float cam[3], int has_sea, float sea_y, float* level)
{
    MirrorWater best = { 1e30f, 0.0f, 0.0f }, held = { 1e30f, 0.0f, 0.0f };
    const uint32_t n = g_water_tiles < WATER_RECTS ? g_water_tiles : WATER_RECTS;
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* r = g_water_rects[i];
        const MirrorWater w = { mirror_rect_dist(cam, r[0], r[1], r[2], r[3]), r[4], (r[2] - r[0]) * (r[3] - r[1]) };
        mirror_candidate(cam, &w, &best, &held);
    }
    for (uint32_t i = 0; i < g_bake_wet_n; ++i)
    {
        const WaterBake* b = &g_bake[g_bake_wet[i]];
        if (!b->n || !mirror_in_view(s, b->lo, b->hi))
            continue;
        const MirrorWater w = mirror_chunk_near(b, cam);
        if (w.d < 1e30f)
            mirror_candidate(cam, &w, &best, &held);
    }
    if (best.d >= 1e30f)
    {
        if (!has_sea || !(sea_y > cam[1] + 0.05f))
        {
            g_wmirror.held = 0;
            return 0;
        }
        best.y = sea_y;
    }
    else if (held.d < 1e30f && !(best.d < held.d * MIRROR_STAY && best.d + MIRROR_STEP < held.d) &&
             !(best.area > held.area * 4.0f && best.d <= held.d + MIRROR_STEP))
        best.y = held.y; /* (its height now: a stream's level goes on down with the camera) */
    g_wmirror.level = *level = best.y;
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
    if (!water_mirror_level(s, &inv[12], has_sea, sea_y, &level))
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

/* The mirror on one of the water's draws (after water_pass_draw): stage 3 (clamped, filtered), and texture matrix 2's bytes 0-4 its
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
