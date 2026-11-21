/* What casts the sun's shadow beyond the scene's own draws: the world's pieces the game draws after the effects ran (late:
 * for the next frame's maps, gfx_d3d12.c sun_cache_late) and the pieces out of the camera's view VanaCore's caster pass has
 * it draw (seek_note: for the maps alone, never the scene). d3d8.c includes this just before scene_note, after g_scene, its
 * SceneTrace, obj, surface_gpu, describe_draw and cap_draw, whose names it uses. It is no file of its own. */
#pragma once

/* A draw after the effects ran (scene_note's late ones) that is the world's own: fogged, into the world's target, through its
 * camera - not the sky's layers, not a menu's model. A caster still, for the next frame's maps (gfx_d3d12.c sun_cache_late). */
static int late_world(const GfxDraw* d, const GfxTex* c, const State* s, const GfxScene* sc)
{
    /* (not by fog: the game draws its late pieces unfogged - 300 a frame of them in Port Jeuno's bursts, 2026-11-01 23:02, and
     * a build that asked for fog took none; the sky's layers are told apart by their camera, a view with no translation) */
    return g_scene.cam && c == g_scene.rt && !d->vs.rhw && (d->vs.prog || !memcmp(s->xf[2], sc->view, 64));
}

/* a late draw of the zone's own meshes: from buffers the game keeps, not a fan (gfx_d3d12.c caster_fixed). The game's late
 * pass draws them with the depth write off (280 a frame of them in Port Jeuno's bursts, 2026-11-01 23:30), which is its pass's
 * state, not the mesh's: a caster all the same */
static int late_mesh(const GfxDraw* d) { return d->prim != GFX_TRIANGLEFAN && (!d->indices || d->ibuf); }

/* A world draw after the effects ran this frame, which the composite's player light missed: in its own pixel function the pool
 * is laid over its colour as the composite laid it over the draws before (GfxFsKey.pool, gfx_hlsl.c emit_pool; the pool in its
 * texture matrix 5). The game sorts its models round the camera by distance - a pond's water layers among a waterfall's spray -
 * and the effects run at the first spray (camera_effect), so as the camera turned the layers went from before the effects to
 * after them and back: by day the pool's darkening on the water came and went (Dragon's Aery, 2026-11-06). Its own: the zone's
 * pieces through the scene's camera (late_world) and the models round the camera from buffers the game keeps, fogged (the
 * water, a waterfall; not the effects, made each frame, which always come after them, nor the sky's layers, unfogged).
 * Fixed-function, with no stage past 5 (matrix 5 is free), blended as the composite's multiply carries over: over or onto */
static void late_pool(GfxDraw* d, const GfxTex* c, const State* s, const GfxScene* sc)
{
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    const int kept = e->used && e->stream < GFX_NSTREAMS && d->buf[e->stream];
    const int blend_ok = !d->pipe.blend || ((d->pipe.src == 2 || d->pipe.src == 5) && (d->pipe.dst == 2 || d->pipe.dst == 6) &&
                                               d->pipe.op == 1); /* ONE or SRCALPHA onto ONE or INVSRCALPHA, ADD */
    if (!g_scene.world_done || c != g_scene.rt || d->vs.rhw || d->vs.prog || d->fs.prog || d->overlay || d->screen_fx ||
        d->vs.ntex > 5 || !blend_ok || !(late_world(d, c, s, sc) || (kept && (d->fs.fog || d->vs.fog_vertex))))
        return;
    if (gfx_player_pool(d->u.texm[5]))
        d->fs.pool = 1;
    /* ... and the height fog, which the composite laid over the draws before (gfx_height_fog: matrix 4, so no stage past 4).
     * The game's water layers come after the effects: unfogged, the sea stood out dark and sharp against the fogged shore, the
     * fog the composite had laid on the seabed under it showing through its blended layer (2026-11-15) */
    if (d->vs.ntex <= 4 && gfx_height_fog(d->u.texm[4]))
        d->fs.hfog = !d->pipe.blend || (d->pipe.src == 5 && d->pipe.dst == 6) ? 1 : d->pipe.dst == 2 ? 2 : 3;
}

/* the late draws counted for the scenes line: taken as casters, or by what kept each out, in late_world's order (the late tail
 * was 500 draws a frame in Port Jeuno, and the first builds took none of them); one of the kept-out every 30 s described to
 * the flicker watch (one a draw used up its line budget: 2787 lines held back, and the other lines never got out) */
static void late_count(const GfxDraw* d, const GfxTex* c, const State* s, const GfxScene* sc)
{
    static uint64_t told_ns;
    if (d->caster)
    {
        g_scene.st_late_cast++;
        return;
    }
    if (c != g_scene.rt)
        g_scene.st_late_out[0]++;
    else if (d->vs.rhw)
        g_scene.st_late_out[1]++;
    else if (!d->vs.prog && memcmp(s->xf[2], sc->view, 64) != 0)
        g_scene.st_late_out[2]++;
    else if (d->pipe.blend && !(d->fs.alpha_func && d->fs.alpha_func != 8))
        g_scene.st_late_out[3]++;
    else if (!d->depth.zwrite)
        g_scene.st_late_out[4]++;
    else
        g_scene.st_late_out[5]++;
    if (c == g_scene.rt && !d->vs.rhw && (gfx_profiling || gfx_watching) && gfx_now_ns() - told_ns > 30000000000ull)
    {
        told_ns = gfx_now_ns();
        char what[512];
        describe_draw(d, what, sizeof what);
        gfx_watch("a late draw of the world's target left out of the casters (fog %d, depth write %d, blend %d, shader %d, the scene's "
                  "camera %d, kept buffers %d): %s", d->fs.fog || d->vs.fog_vertex, d->depth.zwrite, d->pipe.blend, d->vs.prog != 0,
            d->vs.prog || !memcmp(s->xf[2], sc->view, 64), late_mesh(d), what);
    }
}

/* What casts the sun's shadow (the back end's shadow map), scene_note's: the scene's opaque, depth-writing draws - alpha-tested
 * ones too (leaves, fences) - of the world, which is fogged or drawn by the zone's shaders; not the sky, the sun and its flare
 * (unfogged, with views of their own), not one of the game's unlit water layers made VanaCore's water (water_layer_made). A late
 * draw (after the effects) is one when it's the world's own (late_world), the depth write on or not for the zone's own meshes
 * (late_mesh). Sets d->caster and, for a caster placed by a world matrix (trees, props), where it stands: the shadow cache keeps a
 * copy of one it can't draw again from the game's buffers. Characters' vertices are in the world already. */
static void scene_caster(GfxDraw* d, const GfxTex* c, const State* s, const GfxScene* sc, int late, int water_layer_made)
{
    const int opaque = (d->depth.zwrite || (late && late_mesh(d))) && (!d->pipe.blend || (d->fs.alpha_func && d->fs.alpha_func != 8)) &&
        !water_layer_made;
    d->caster = opaque && (late ? late_world(d, c, s, sc) : (d->vs.prog || d->fs.fog || d->vs.fog_vertex) && !g_scene.world_done);
    if (!d->caster)
        return;
    const float* W = s->xf[24];
    if (W[12] != 0.0f || W[13] != 0.0f || W[14] != 0.0f)
        d->anchor[0] = W[12], d->anchor[1] = W[13], d->anchor[2] = W[14], d->anchor[3] = 1.0f;
}

/* the world's pieces among the 3D after the effects cast from the next frame on (scene_note's late casters): said when their
 * number jumps, as it did for 25 s at a time in Port Jeuno (2026-11-01) with the game's own offscreen 3D laid over the world */
static void late_watch(const struct SceneTrace* t, const struct SceneTrace* p)
{
    if (t->late > p->late + 100 || (p->late > 100 && t->late + 100 < p->late))
        gfx_watch("the game drew %u of its 3D after the effects (the frame before %u): the world's pieces among them cast "
                  "from the next frame on", t->late, p->late);
}

/* A draw of VanaCore's caster pass (d3d8_set_seek: a piece of the zone round the player, whatever the camera sees, drawn by the
 * game's own renderer for casters.cpp): kept for the sun's maps alone when it's a caster as scene_note takes one (the world's,
 * opaque, fogged, into the world's target through its camera), never drawn into the scene. The game draws only what its camera
 * sees, and a building out of view cast nothing until it had been in view once (the shadow cache kept it then): moving the camera
 * up 3.5 units over Port Jeuno brought a whole building's shadow onto the plaza, 2026-11-02, whatever the shadow settings. */
/* the caster pass's draw is one of the world's own into the world's target (c) through the scene's camera, depth-tested, and a
 * caster as scene_caster takes one: opaque (an alpha test too), fogged or the zone's shader's, no water layer */
static int seek_world(const GfxDraw* d, const GfxTex* c, const State* s)
{
    if (c != g_scene.rt || !g_scene.cam || d->vs.rhw || !d->depth.zenable || !obj(g_dev.ds) ||
        (!d->vs.prog && memcmp(s->xf[2], g_scene.s.view, 64) != 0))
        return 0;
    const int opaque = d->depth.zwrite && (!d->pipe.blend || (d->fs.alpha_func && d->fs.alpha_func != 8)) &&
        !(d->fs.water && !d->vs.lighting);
    return opaque && (d->vs.prog || d->fs.fog || d->vs.fog_vertex);
}

/* why seek_world turned a draw of the pass down (the flicker watch, seek_piece_done): 1 not the world's target, 2 no scene camera
 * yet, 4 screen-space or not depth-tested, 8 through another camera, 16 not a caster (blended, no depth write, unfogged, water) */
static uint32_t seek_why(const GfxDraw* d, const GfxTex* c, const State* s)
{
    if (c != g_scene.rt)
        return 1;
    if (!g_scene.cam)
        return 2;
    if (d->vs.rhw || !d->depth.zenable || !obj(g_dev.ds))
        return 4;
    return !d->vs.prog && memcmp(s->xf[2], g_scene.s.view, 64) != 0 ? 8 : 16;
}

/* The flicker watch's look at the caster pass piece by piece (d3d8_set_seek): a piece it had taken casters from that gives none
 * when drawn again - the game's renderer drew nothing of it, or seek_world turned every draw down (why) - is said, and so is its
 * return. The cache keeps a piece's draws PIECE_KEEP frames after the pass last took them (gfx_sun_cache.h), so a piece the pass
 * keeps missing casts and stops: Ru'Lude Gardens (2026-11-04), 38 of the cache's draws gone 376 frames in every 432 */
#define SEEK_SEEN 8192 /* a power of two, over a zone's pieces (5,163 in the most seen) */
typedef struct
{
    uint32_t piece, frame; /* the piece; the frame it last gave casters */
    uint16_t took;         /* how many, then */
    uint8_t missing;       /* it gave none since */
} SeekSeen;
static SeekSeen g_seek_seen[SEEK_SEEN];
static uint32_t g_seek_took, g_seek_why; /* of the piece being drawn: the casters seek_note took, why it turned the rest down */

/* a few lines a second, the rest counted */
static int seek_say(uint32_t now)
{
    static uint32_t window, said, held;
    if (now - window >= 240)
    {
        if (held)
            gfx_watch("... and %u more of the caster pass's pieces came or went", held);
        window = now, said = held = 0;
    }
    if (said < 12)
        return ++said;
    held++;
    return 0;
}

static void seek_piece_done(uint32_t piece, uint32_t draws)
{
    if (!gfx_profiling && !gfx_watching)
        return;
    const uint32_t h = (piece >> 2) * 2654435761u;
    SeekSeen* e = NULL;
    for (uint32_t k = 0; k < 32 && !e; ++k)
    {
        SeekSeen* t = &g_seek_seen[(h + k) & (SEEK_SEEN - 1)];
        if (t->piece == piece || !t->piece)
            e = t;
    }
    if (!e)
        return;
    const uint32_t now = g_scene.frame;
    if (e->piece && g_seek_took && e->missing && seek_say(now))
        gfx_watch("piece %08X gives the shadows %u casters again (of %u draws), %u frames after it last did", piece, g_seek_took,
            draws, now - e->frame);
    else if (e->piece && !g_seek_took && !e->missing && e->took && seek_say(now))
        gfx_watch("piece %08X gave the shadows no caster: %u draws (why %u; it gave %u, %u frames before)", piece, draws, g_seek_why,
            e->took, now - e->frame);
    e->missing = !g_seek_took && (e->missing || e->took);
    e->piece = piece;
    if (g_seek_took)
        e->took = (uint16_t)g_seek_took, e->frame = now;
}

static void seek_note(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t first, uint32_t nverts, uint32_t up_data, uint32_t up_stride)
{
    uint32_t face, level;
    const Obj* rt = obj(g_dev.rt);
    const GfxTex* c = rt ? surface_gpu(rt, &face, &level) : NULL;
    const State* s = &g_dev.cur;
    if (!c || face || level || !seek_world(d, c, s))
    {
        g_seek_why |= !c || face || level ? 1 : seek_why(d, c, s);
        return;
    }
    g_seek_took++;
    d->caster = 1, d->sought = 1;
    const float* W = s->xf[24];
    /* A fixed-function piece goes into the sun's maps from the world (sought 2: its transform is its world matrix alone), not
     * through the camera and back: so placed, the far map changed as the camera turned, parts of a building nearer or farther
     * from one quarter-second to the next with the sun and the player still (debug view 27, Port Jeuno, 2026-11-02 22:31), and
     * held still while the camera did. A vertex shader's place comes from its constants: through the camera as before */
    if (!d->vs.prog)
    {
        memcpy(d->u.wvp, W, 64);
        d->sought = 2;
    }
    float jit[2]; /* moved by DLSS's sub-pixel step as the scene's own casters are (scene_note) */
    if (d->sought == 1 && (c == g_scene.world || !g_scene.world) && gfx_dlss_jitter(jit))
        memcpy(&d->u.pad0[0], &jit[0], 4), memcpy(&d->u.pad0[1], &jit[1], 4);
    if (W[12] != 0.0f || W[13] != 0.0f || W[14] != 0.0f)
        d->anchor[0] = W[12], d->anchor[1] = W[13], d->anchor[2] = W[14], d->anchor[3] = 1.0f;
    d->prim = prim, d->count = count;
    if (g_cap)
        cap_draw(d, prim, count, first, nverts, up_data, up_stride);
    gfx_draw(d);
}
