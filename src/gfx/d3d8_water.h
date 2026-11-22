/* VanaCore's water: the zone's water found among the game's draws (water_tile_shape by a tile's shape, water_model by the game's
 * water models), the draws made VanaCore's water (water_draw, water_shade: gfx_hlsl_water.h shades them) or left out (water_layer),
 * and the water pass (water_keep, water_pass): the water drawn after the world over copies of the scene's colour and depth. d3d8.c
 * includes this just before its character shadows, after g_dev, obj, State, GfxDraw, g_scene, is_character and gfx_now_ns, whose
 * names it uses; g_water_tiles, g_water_shape_tiles and g_water_kept_n are declared with d3d8.c's other per-frame counts (reset at
 * Present). It is no file of its own. */
#pragma once

/* The zone's water. Each tile of its "water"/"water2" meshes (a flat 40x40 yalm strip, no texture, vertex-lit) is drawn as an
 * opaque solid, and the game draws its own unlit water over it: layers a few cm above and below the tile's plane, the first of
 * them writing depth. Which of the two took a pixel was the depth buffer's rounding (D24, near plane 0.1: about 5 cm at 300
 * yalms), so the sea changed from blue to the black, shadowed solid as the camera moved.
 *   g_water_mode 1: the tile is VanaCore's water (gfx_hlsl.c emit_water_fs; the tile's world position and the time ride in
 *     texture matrix 7), and the game's layers drawn after a tile this frame are left out - they were the water's look.
 *   g_water_mode 0: the game's water, its tile pushed back g_water_tile_back along the view ray in depth alone (the decals'
 *     pull, the other way) so its layers are always in front of it.
 * Both are plain globals, so they can be changed while the game runs.
 * What is the water (g_water_set.detect, [graphics] water.detect): 0 the tiles by their shape (water_tile_shape) and the layers
 * by theirs (water_layer), as above; 1 the game's water models (water_model: the vertex buffers made while the game loaded a
 * model under a water directory of the zone's DAT); 2 either. See water_draw for what each mode does with a water model's draw. */
static int g_water_mode = 0;
/* the zone is one VanaCore's water is never drawn in, the game's own left as with water=0 (water_set_zone: data\water_zones.json,
 * d3d8_set_water_zones_off): Alzadaal Undersea Ruins' (72) glowing floors were drawn as water, which they don't want (2026-11-22) */
static int g_water_zone_off;
static float g_water_tile_back = 0.3f;
/* this frame's VanaCore water, in the world: x and z from and to, then the height (water_shade; the first WATER_RECTS of
 * g_water_tiles) */
#define WATER_RECTS 256
#define WATER_KEEP_COPY 262144u /* the most bytes of a dynamic draw's data copied to keep it (a basin is 5 KB) */

/* the indices a primitive count takes (D3DPRIMITIVETYPE) */
static uint32_t water_prim_indices(uint32_t prim, uint32_t count)
{
    switch (prim)
    {
    case 1: return count;           /* points */
    case 2: return count * 2;       /* lines */
    case 3: return count + 1;       /* a line strip */
    case 4: return count * 3;       /* triangles */
    default: return count + 2;      /* a strip or a fan */
    }
}

/* ... and of them, kept for the water pass (g_water_kept). A sea is many 40-yalm tiles (Lower Jeuno's harbour from the terrace:
 * well over 64 in view); at 64 the rest went out in their place, opaque (water 1: no bed through them), dark rectangles among
 * the pass's tiles that came and went as the camera's distance changed which tiles were drawn first */
#define WATER_KEPT 256
static float g_water_rects[WATER_RECTS][5];

/* mode 1's look ([graphics] water*, vanacore_gfx_set_water): they ride to the pixel function in texture matrix 7 (bytes 4-15) and
 * matrix 6 (bytes 0-3 the picked colour, 11-15 the pass's; its bytes 4-10 are water_pass_draw's) */
static WaterSettings g_water_set = { 0, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f, 0.0f, 1.0f, 0, 0, 2, 0, 1, 4.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 0, 1 };
static float g_water_turn[2] = { 1.0f, 0.0f }; /* cos and sin of the waves' direction */
static double g_water_clock;      /* the waves' time, in seconds, wrapped at 600 (emit_water_fs): it runs at g_water_set.speed */
static uint64_t g_water_last_ns;  /* when it was last moved on */

/* Waterfalls' feet: a water model's draw that isn't level and stands tall (WATER_FOOT_TALL yalms or more top to bottom, no
 * more than WATER_FOOT_WIDE across: a waterfall, not a ripple or a splash) ends somewhere, and where that is at VanaCore's
 * water's height the water churns and foams there (gfx_hlsl_water.h emit_water_surface, emit_water_depth: matrix 5's rows 1-3
 * carry up to three feet a draw, x, z, radius and strength: bytes 4-7, 8-11, 12-15, columns 1-3 to the shader, whose matrices
 * are column_major). The feet seen in a frame are the next frame's water's, a frame
 * behind, as a pool may be drawn before its fall */
#define WATER_FEET 32
#define WATER_FOOT_TALL 4.0f
#define WATER_FOOT_WIDE 40.0f
static float g_water_feet[2][WATER_FEET][4]; /* x, z, radius, the foot's height: [0] being gathered, [1] in use */
static uint32_t g_water_nfeet[2];

static void water_foot_note(const float lo[3], const float hi[3])
{
    const float dx = hi[0] - lo[0], dz = hi[2] - lo[2], dy = hi[1] - lo[1];
    if (g_water_set.falls <= 0.0f || dy < WATER_FOOT_TALL || dx > WATER_FOOT_WIDE || dz > WATER_FOOT_WIDE || g_water_nfeet[0] >= WATER_FEET)
        return;
    float* f = g_water_feet[0][g_water_nfeet[0]];
    f[0] = (lo[0] + hi[0]) * 0.5f, f[1] = (lo[2] + hi[2]) * 0.5f;
    f[2] = fminf(fmaxf(fmaxf(dx, dz) * 0.5f + 1.5f, 2.0f), 12.0f);
    f[3] = hi[1]; /* its bottom: the world's y grows downward */
    ++g_water_nfeet[0];
}

/* the frame's end (d3d8.c, with the frame's other water counts): the feet gathered are the next frame's */
static void water_feet_turn(void)
{
    memcpy(g_water_feet[1], g_water_feet[0], sizeof g_water_feet[0]);
    g_water_nfeet[1] = g_water_nfeet[0], g_water_nfeet[0] = 0;
}

/* the feet at this water's height (3 yalms either way) over its extent (its radius to spare), the first three, into matrix
 * 5's rows 1-3 (x, z, radius, strength); a radius of 0 ends the list */
static void water_feet_set(float m[16], const float lo[3], const float hi[3])
{
    const float y = (lo[1] + hi[1]) * 0.5f;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_water_nfeet[1] && n < 3; ++i)
    {
        const float* f = g_water_feet[1][i];
        if (fabsf(f[3] - y) > 3.0f || f[0] < lo[0] - f[2] || f[0] > hi[0] + f[2] || f[1] < lo[2] - f[2] || f[1] > hi[2] + f[2])
            continue;
        m[4 + n * 4] = f[0], m[5 + n * 4] = f[1], m[6 + n * 4] = f[2], m[7 + n * 4] = g_water_set.falls;
        ++n;
    }
    for (; n < 3; ++n)
        m[6 + n * 4] = 0.0f;
}

/* the ground's water (water_ground_pass: one draw a chunk, its water at every height, the chunks' extents in the bake): the
 * first three feet, whatever their height; the shader's radius does the rest */
static void water_feet_any(float m[16])
{
    uint32_t n = 0;
    for (; n < 3 && n < g_water_nfeet[1]; ++n)
    {
        const float* f = g_water_feet[1][n];
        m[4 + n * 4] = f[0], m[5 + n * 4] = f[1], m[6 + n * 4] = f[2], m[7 + n * 4] = g_water_set.falls;
    }
    for (; n < 3; ++n)
        m[6 + n * 4] = 0.0f;
}

static int g_water_as_is; /* the next draw kept is drawn again as the game draws it (water_sheet: the game's shimmer; water_keep) */
static uint32_t g_bake_nv;  /* the ground's water's vertices this zone (below, with the ground's water) */
static int water_ground_under(const float lo[3], const float hi[3]); /* below */

static void sea_note(const float lo[3], const float hi[3], int own); /* below: a tile's level, for the sea drawn on to the horizon */
static int sea_own(const float lo[3], const float hi[3]);            /* below: a tile of the sea, dropped for VanaCore's plane */

void d3d8_set_water(const WaterSettings* s)
{
    g_water_set = *s;
    g_water_mode = s->enabled && !g_water_zone_off ? 1 : 0;
    g_water_turn[0] = cosf(s->direction * 0.01745329252f), g_water_turn[1] = sinf(s->direction * 0.01745329252f);
}

/* The extent of the position stream's first nverts vertices, as the CPU has them, in the world (each through the draw's world
 * matrix: a turned or scaled model's as it stands; Rabao's pond is placed with its axes turned, and the box of its own box's
 * corners had a rise it hasn't); 0 when they can't be read */
/* the inverse of an affine matrix (a rotation, a scale and a translation, as D3D lays them out: the translation in row 3) */
static int water_affine_inv(float r[16], const float* m)
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-12f)
        return 0;
    const float k = 1.0f / det;
    r[0] = A * k, r[1] = (c * h - b * i) * k, r[2] = (b * f - c * e) * k, r[3] = 0.0f;
    r[4] = B * k, r[5] = (a * i - c * g) * k, r[6] = (c * d - a * f) * k, r[7] = 0.0f;
    r[8] = C * k, r[9] = (b * g - a * h) * k, r[10] = (a * e - b * d) * k, r[11] = 0.0f;
    r[12] = -(m[12] * r[0] + m[13] * r[4] + m[14] * r[8]);
    r[13] = -(m[12] * r[1] + m[13] * r[5] + m[14] * r[9]);
    r[14] = -(m[12] * r[2] + m[13] * r[6] + m[14] * r[10]);
    r[15] = 1.0f;
    return 1;
}

/* The draw's world matrix as the world has it (into W) and its view, returned. The game draws some of its water models
 * (Rabao's pond sheets, Jugner Forest's rivers) with the view folded into the world matrix and the view matrix left the
 * identity: seen that way the sheets stood on end and off to one side, and none was level. The world is then the draw's world
 * through the inverse of the scene's view (the view the zone's own chunks were drawn with this frame), and the view the scene's.
 * Any other draw's are its own. g_water_folded: 1 for such a draw, -1 for one before the scene's view is known this frame (the
 * world can't be told: not judged), 0 for any other. */
static int g_water_folded;
static const float* water_frames(float W[16])
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float* w = g_dev.cur.xf[24];
    const float* v = g_dev.cur.xf[2];
    memcpy(W, w, 64);
    g_water_folded = 0;
    int folded = 1;
    for (int i = 0; i < 16 && folded; ++i)
        folded = fabsf(v[i] - I[i]) < 1e-4f;
    if (!folded)
        return v;
    float inv[16];
    if (!g_scene.cam || g_scene.s.view[15] == 0.0f || !water_affine_inv(inv, g_scene.s.view))
    {
        g_water_folded = -1;
        return v;
    }
    mat_mul(W, w, inv);
    g_water_folded = 1;
    return g_scene.s.view;
}

static int water_extent(const GfxDraw* d, uint32_t nverts, const float* W, float lo[3], float hi[3])
{
    const State* s = &g_dev.cur;
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    /* up to 65536 vertices: Selbina's sea is two water-model sheets of 5213 each, and at 4096 they had no extent, were never
     * taken for level water and stayed the game's, over VanaCore's sea plane (their depth over it): a second sea by the shore */
    if (nverts < 3 || nverts > 65536 || !e->used || e->stream >= GFX_NSTREAMS || !d->cpu[e->stream] || !s->stride[e->stream])
        return 0;
    const uint8_t* p = (const uint8_t*)d->cpu[e->stream] + d->u.offset[GFX_R_POSITION];
    uint32_t stride = s->stride[e->stream];
    if (d->cpu_size[e->stream] < (uint32_t)(nverts - 1) * stride + 12u)
        return 0;
    for (int a = 0; a < 3; ++a)
        lo[a] = 1e30f, hi[a] = -1e30f;
    for (uint32_t i = 0; i < nverts; ++i)
    {
        float v[3];
        memcpy(v, p + (size_t)i * stride, 12);
        for (int a = 0; a < 3; ++a)
        {
            const float w = v[0] * W[a] + v[1] * W[4 + a] + v[2] * W[8 + a] + W[12 + a];
            lo[a] = w < lo[a] ? w : lo[a], hi[a] = w > hi[a] ? w : hi[a];
        }
    }
    return 1;
}

/* A water tile by its shape (see above): flat and wide, untextured, vertex-lit, opaque, writing depth. lo and hi: its extent. */
static int water_tile_shape(const GfxDraw* d, uint32_t prim, uint32_t nverts, float lo[3], float hi[3])
{
    const State* s = &g_dev.cur;
    if (prim != 5 /* D3DPT_TRIANGLESTRIP */ || s->vs != 0x152 /* XYZ | NORMAL | DIFFUSE | TEX1 */ || d->vs.rhw || d->vs.prog ||
        d->fs.prog || !d->vs.lighting || d->pipe.blend || !d->depth.zenable || !d->depth.zwrite || obj(s->tex[0]))
        return 0;
    /* flat, and at least 16 yalms each way: not a ledge or a plank */
    float W[16];
    water_frames(W);
    return water_extent(d, nverts, W, lo, hi) && hi[1] - lo[1] <= 0.001f && hi[0] - lo[0] >= 16.0f && hi[2] - lo[2] >= 16.0f;
}

/* A draw of the game's water models (Obj.water: its position stream's buffer was made while the game loaded one). 0 for any
 * other draw; 1 for a level surface of it - flat (rising no more than 1 in 100 across it, or 0.2 yalm for a small one) and at
 * least 4 yalms each way: the sheets of the sea, a pond or a river - and 2 for the rest: a waterfall, a river running downhill, a
 * ripple or a splash, one drawn with shaders or whose vertices can't be read. lo and hi: its extent in the world (for 1). */
static int water_model(const GfxDraw* d, uint32_t nverts, uint32_t up_data, float lo[3], float hi[3])
{
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    if (up_data || d->vs.rhw || !e->used || e->stream >= GFX_NSTREAMS)
        return 0;
    const Obj* vb = obj(g_dev.cur.stream[e->stream]);
    if (!vb || vb->kind != O_VB || !vb->water)
        return 0;
    const int shaders = d->vs.prog || d->fs.prog;
    float W[16];
    water_frames(W);
    if (g_water_folded < 0) /* the world can't be told this frame: the game's, this once */
        return 2;
    const int extent = water_extent(d, nverts, W, lo, hi);
    if (!extent)
        lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0.0f;
    const float dx = hi[0] - lo[0], dz = hi[2] - lo[2];
    const int level = extent && !shaders && hi[1] - lo[1] <= fmaxf(0.2f, 0.01f * fmaxf(dx, dz)) && dx >= 4.0f && dz >= 4.0f;
    if (g_water_set.log) /* once a buffer and vertex count: what it is, and why it is or isn't level */
    {
        static uint32_t said[64][2];
        static uint32_t nsaid;
        uint32_t i = 0;
        while (i < nsaid && (said[i][0] != vb->mem || said[i][1] != nverts))
            ++i;
        if (i == nsaid && nsaid < 64)
        {
            said[nsaid][0] = vb->mem, said[nsaid][1] = nverts, ++nsaid;
            rt_log("[recomp] water: model draw %s: %u vertices, %.1f x %.1f yalms, heights %.2f..%.2f, x %.0f..%.0f z %.0f..%.0f; "
                   "world matrix rows %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f, at %.1f %.1f %.1f; blend %d %u/%u, z %d/%d, "
                   "shaders %d/%d, position %d stream %u type %d cpu %d size %u stride %u, view folded into the world %d\n",
                level ? "level" : shaders ? "with shaders" : !extent ? "no extent" : "not level", nverts, dx, dz, lo[1], hi[1], lo[0], hi[0],
                lo[2], hi[2], W[0], W[1], W[2], W[4], W[5], W[6], W[8], W[9], W[10], W[12], W[13], W[14], d->pipe.blend, d->pipe.src,
                d->pipe.dst, d->depth.zenable, d->depth.zwrite, d->vs.prog != 0, d->fs.prog != 0, e->used, e->stream, e->type,
                e->stream < GFX_NSTREAMS && d->cpu[e->stream] != NULL, e->stream < GFX_NSTREAMS ? d->cpu_size[e->stream] : 0,
                e->stream < GFX_NSTREAMS ? g_dev.cur.stride[e->stream] : 0, g_water_folded);
        }
    }
    return level ? 1 : 2;
}

/* Over VanaCore's water drawn this frame: the draw's extent (lo, hi: in the world) lies inside one of g_water_rects (half a
 * yalm to spare), within a yalm of its height. Inside, not touching: a river's next stretch, or a pool a step below, overlapping
 * one already made water at an edge, is water of its own, and left out it was a hole */
static int water_covered(const float lo[3], const float hi[3])
{
    const float x0 = lo[0], z0 = lo[2], x1 = hi[0], z1 = hi[2];
    const float y = (lo[1] + hi[1]) * 0.5f;
    const uint32_t n = g_water_tiles < WATER_RECTS ? g_water_tiles : WATER_RECTS;
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* r = g_water_rects[i];
        if (x0 >= r[0] - 0.5f && x1 <= r[2] + 0.5f && z0 >= r[1] - 0.5f && z1 <= r[3] + 0.5f && fabsf(y - r[4]) <= 1.0f)
            return 1;
    }
    return 0;
}

/* The debug view ([graphics.effects] debug=11, g_water_set.view): the water found drawn in a flat colour (rgb), unfogged, a
 * blended draw half over what's behind it */
static void water_view(GfxDraw* d, uint32_t rgb)
{
    if (d->fs.prog)
        return;
    GfxStage* st = &d->fs.st[0];
    memset(st, 0, sizeof *st);
    st->cop = st->aop = 2; /* D3DTOP_SELECTARG1 */
    st->ca1 = st->aa1 = 3; /* D3DTA_TFACTOR */
    d->fs.nstages = 1;
    d->fs.fog = 0, d->fs.alpha_func = 0, d->fs.specular_add = 0, d->fs.nmap = 0, d->fs.water = 0;
    d->u.tfactor[0] = (float)((rgb >> 16) & 255) / 255.0f, d->u.tfactor[1] = (float)((rgb >> 8) & 255) / 255.0f;
    d->u.tfactor[2] = (float)(rgb & 255) / 255.0f, d->u.tfactor[3] = 0.5f;
    if (d->pipe.blend)
        d->pipe.src = 5, d->pipe.dst = 6, d->pipe.op = 1; /* SRCALPHA, INVSRCALPHA, ADD */
}

/* The sun and the moon (the directional lights) and the ambient as the last lit draw of the world had them, for a layer of the
 * game's made VanaCore's water (water_shade): the game draws its layers unlit, and the water takes its light from the draw's.
 * Held for the zone (d3d8_set_water_floors lets them go), their directions in the world (g_water_sun_w) and turned into each
 * water draw's view: the lit draws come and go with what the camera sees (Caedarva Mire has its trees in every frame; where
 * none but the characters' are in view there are none), and held for 30 frames in the lit draw's view the water lost its sun
 * and glint once the camera turned from them, and the sun turned with the camera while it was held */
static GfxLight g_water_sun[2];
static float g_water_sun_w[2][3];
static float g_water_ambient[4];
static uint32_t g_water_nsun;

static void water_unit3(float* v)
{
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
        v[0] /= l, v[1] /= l, v[2] /= l;
}

/* from the world's own lit draws only: fogged, writing depth, from the game's buffers, not a character (their lights are the
 * actors' own) nor the menus' 3D preview. A light's direction is in the draw's view (draw_packet: through the view matrix):
 * back into the world through the view's rotation */
static void water_lights(const GfxDraw* d, uint32_t up_data)
{
    if (up_data || !d->fs.fog || !d->depth.zwrite || is_character(d, up_data))
        return;
    const float* V = g_dev.cur.xf[2];
    uint32_t n = 0;
    for (int i = 0; i < d->vs.nlights && n < 2; ++i)
        if (d->vs.light_type[i] == 3)
        {
            const float* v = d->u.light[i].dir;
            float* w = g_water_sun_w[n];
            for (int j = 0; j < 3; ++j)
                w[j] = v[0] * V[j * 4] + v[1] * V[j * 4 + 1] + v[2] * V[j * 4 + 2];
            water_unit3(w);
            g_water_sun[n++] = d->u.light[i];
        }
    if (n)
        g_water_nsun = n, memcpy(g_water_ambient, d->u.ambient, sizeof g_water_ambient);
}

/* VanaCore's water on the draw (mode 1): gfx_hlsl_water.h in place of its stages, opaque and writing depth (a tile is
 * already; a layer of the game's is made so, and lit as the world is: water_lights); its world position and the time in
 * texture matrix 7, the settings after them, the picked colour and the pass's settings in matrix 6, the draw's world matrix in
 * matrix 3 and the view in matrix 4 (the functions take the world position, the up and the normal through them: a turned or
 * scaled model's water lies as a tile's). Its place in the world (lo, hi: its extent there) is kept for the frame
 * (g_water_rects). Drawn in its place, or kept for the pass (water_keep). */
static void water_shade(GfxDraw* d, const float lo[3], const float hi[3], const float* W, const float* V, int rect)
{
    if (!d->vs.lighting && g_water_nsun)
    {
        for (uint32_t i = 0; i < g_water_nsun; ++i)
        {
            d->u.light[i] = g_water_sun[i], d->vs.light_type[i] = 3;
            const float* w = g_water_sun_w[i];
            for (int j = 0; j < 3; ++j) /* the world's direction into this draw's view (V) */
                d->u.light[i].dir[j] = w[0] * V[j] + w[1] * V[4 + j] + w[2] * V[8 + j];
            water_unit3(d->u.light[i].dir);
        }
        d->vs.nlights = (uint8_t)g_water_nsun;
        memcpy(d->u.ambient, g_water_ambient, sizeof g_water_ambient);
    }
    if (!g_water_tiles) /* the frame's first moves the waves' time on, for all of them */
    {
        const uint64_t now = gfx_now_ns();
        double dt = g_water_last_ns ? (double)(now - g_water_last_ns) * 1e-9 : 0.0;
        if (dt > 0.25) /* a pause or a zone change: carry on from where it was */
            dt = 0.0;
        g_water_clock = fmod(g_water_clock + dt * (double)g_water_set.speed, 600.0);
        g_water_last_ns = now;
    }
    float* m = d->u.texm[7];
    d->fs.water = 1;
    memcpy(d->u.texm[3], W, 64), memcpy(d->u.texm[4], V, 64);
    d->pipe.blend = 0, d->depth.zwrite = 1;
    m[0] = W[12], m[1] = W[13], m[2] = W[14], m[3] = (float)g_water_clock;
    m[4] = g_water_set.waves, m[5] = g_water_set.size, m[6] = g_water_set.reflection, m[7] = g_water_set.glint;
    m[8] = g_water_set.brightness, m[9] = g_water_set.colour, m[10] = g_water_set.shallowness, m[11] = g_water_set.turbulence;
    m[12] = g_water_turn[0], m[13] = g_water_turn[1], m[14] = g_water_set.glint_size < 0.2f ? 0.2f : g_water_set.glint_size;
    m[15] = g_water_set.ripple_size < 0.25f ? 0.25f : g_water_set.ripple_size;
    /* a pool (a town's fountain basin, under 40 yalms each way) lies calmer than a sea or a river: the swell at a third, the
     * sun's glint and the fine ripples at half, so a flat basin isn't covered in the ripples' white squiggles */
    const int pool = hi[0] - lo[0] < 40.0f && hi[2] - lo[2] < 40.0f;
    if (pool)
        m[4] *= 0.35f, m[7] *= 0.5f;
    float* tn = d->u.texm[6]; /* the picked colour, when there is one: matrix 6's bytes 0-3 (r, g, b, 1) */
    tn[0] = (float)((g_water_set.tint >> 16) & 255) / 255.0f, tn[1] = (float)((g_water_set.tint >> 8) & 255) / 255.0f;
    tn[2] = (float)(g_water_set.tint & 255) / 255.0f, tn[3] = g_water_set.has_tint ? 1.0f : 0.0f;
    tn[11] = g_water_set.clarity < 0.5f ? 0.5f : g_water_set.clarity, tn[12] = g_water_set.refraction, tn[13] = g_water_set.shore;
    tn[14] = g_water_set.foam, tn[15] = pool ? g_water_set.ripples * 0.5f : g_water_set.ripples;
    d->u.texm[5][0] = (float)g_water_set.probe; /* matrix 5's byte 0: the probe (water.probe), 0 otherwise */
    d->u.texm[5][2] = g_water_set.caustics, d->u.texm[5][3] = g_water_set.relief; /* bytes 2 and 3 */
    water_feet_set(d->u.texm[5], lo, hi);
    if (rect && g_water_tiles < WATER_RECTS)
    {
        float* r = g_water_rects[g_water_tiles];
        r[0] = lo[0], r[1] = lo[2], r[2] = hi[0], r[3] = hi[2];
        r[4] = (lo[1] + hi[1]) * 0.5f;
    }
    if (rect)
        ++g_water_tiles;
}

/* a draw the pass can draw again at the scene's end: every stream it reads in a static buffer of the game's, its indices too (the
 * memory a dynamic draw brings is the game's to write over by then), and room to keep it */
static int water_keepable(const GfxDraw* d)
{
    if (!g_water_set.pass || g_water_kept_n >= WATER_KEPT)
        return 0;
    uint32_t bytes = 0; /* a dynamic draw's data is copied when kept (water_keep), up to WATER_KEEP_COPY */
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (d->data[s])
            bytes += d->size[s];
    if (d->indices && !d->ibuf)
        bytes += water_prim_indices(d->prim, d->count) * (d->index_size ? d->index_size : 2);
    return bytes <= WATER_KEEP_COPY;
}

/* --- the game's water in the zone's own meshes --------------------------------------------------------------------- */
/* Most of the game's water isn't a tile or a water model: Jugner Forest's ponds, West Ronfaure's streams, Rabao's oasis are
 * sheets in the zone's meshes - flat, blended, fogged, textured with the water's own scrolling picture, from the game's static
 * buffers - drawn over their beds. They're told from the rest of the zone's sheets (a window, a wet floor, a glow) by the zone's
 * collision: the game's footsteps know deep water (a floor at the surface the player walks on) and shallow water (a floor on
 * the bed, under it), so a sheet with the footsteps' water under it, at the surface or within a few yalms below, is water
 * (grass_floor_near; the water asks for the zone's floors with water.detect=2, graphics.cpp g_water_floors). Looked at only
 * once the cheap tests pass; its extent is read then. One drawn with the zone's own shaders is made a fixed-function draw
 * (its position in the usual register), lit as the world is (water_lights), as a layer of the game's is. With water.log, each
 * sheet looked at is written to the log once (by its buffer and vertex count): what it is and why it was or wasn't taken. */
#define SHEET_LOGGED 256
static uint64_t g_sheet_logged[SHEET_LOGGED];
static uint32_t g_sheet_nlogged;

static void sheet_log(const GfxDraw* d, uint32_t nverts, const float lo[3], const float hi[3], const int t[3], const float dy[3], int took)
{
    if (t[0] < 0 && t[1] < 0 && t[2] < 0) /* near no floor at all: a decal on a hill, not worth a line */
        return;
    Obj* tex = obj(g_dev.cur.tex[0]);
    const Obj* vb = obj(g_dev.cur.stream[d->vs.el[GFX_R_POSITION].stream]);
    const uint64_t key = ((uint64_t)nverts << 40) ^ (vb ? vb->mem : 0u) ^ ((uint64_t)(unsigned)took << 63);
    for (uint32_t i = 0; i < g_sheet_nlogged; ++i)
        if (g_sheet_logged[i] == key)
            return;
    if (g_sheet_nlogged >= SHEET_LOGGED)
        return;
    g_sheet_logged[g_sheet_nlogged++] = key;
    const float* w = g_dev.cur.xf[24];
    rt_log("[recomp] water: sheet %s: %u vertices, %.1f x %.1f yalms at height %.1f (rise %.2f), x %.0f..%.0f z %.0f..%.0f, texture %016llx, "
           "blend %u/%u, shaders %d/%d, lit %d, fog %d, depth %d/%d; floors under it %d (%.2f) %d (%.2f) %d (%.2f); view folded %d, "
           "the draw's world at %.1f %.1f %.1f, the scene's view rows %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f at %.1f %.1f %.1f\n",
        took ? "taken" : "left", nverts, hi[0] - lo[0], hi[2] - lo[2], (lo[1] + hi[1]) * 0.5f, hi[1] - lo[1], lo[0], hi[0], lo[2], hi[2],
        (unsigned long long)(tex ? texture_hash(tex) : 0), d->pipe.src, d->pipe.dst, d->vs.prog != 0, d->fs.prog != 0, d->vs.lighting,
        d->fs.fog || d->vs.fog_vertex, d->depth.zenable, d->depth.zwrite, t[0], dy[0], t[1], dy[1], t[2], dy[2], g_water_folded, w[12], w[13],
        w[14], g_scene.s.view[0], g_scene.s.view[1], g_scene.s.view[2], g_scene.s.view[4], g_scene.s.view[5], g_scene.s.view[6], g_scene.s.view[8],
        g_scene.s.view[9], g_scene.s.view[10], g_scene.s.view[12], g_scene.s.view[13], g_scene.s.view[14]);
}

/* whether a draw's vertices (by W) lie in one plane: the plane through its first triangle that isn't a sliver, every vertex
 * within 0.3 yalm plus 4% of its size of it, and lying level (its plane's normal within about 35 degrees of up). A stream's
 * shimmer is a level sheet; a light's cone down a wall (Lower Jeuno's window) is not one plane, and a lamp's beam (Mhaura's,
 * one slanted quad from the lamp to the ground) is not level. An affine W keeps a plane a plane, so the test holds even where
 * W's box comes out wrong-shaped (water_sheet's unlit twin) */
static int sheet_planar(const GfxDraw* d, uint32_t nverts, const float* W)
{
    const State* s = &g_dev.cur;
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    if (nverts < 3 || nverts > 4096 || !e->used || e->stream >= GFX_NSTREAMS || !d->cpu[e->stream] || !s->stride[e->stream])
        return 0;
    const uint8_t* p = (const uint8_t*)d->cpu[e->stream] + d->u.offset[GFX_R_POSITION];
    const uint32_t stride = s->stride[e->stream];
    if (d->cpu_size[e->stream] < (uint32_t)(nverts - 1) * stride + 12u)
        return 0;
    float q[3][3], n[3] = { 0, 0, 0 }, lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    float nl = 0.0f;
    uint32_t got = 0;
    for (uint32_t i = 0; i < nverts; ++i)
    {
        float v[3], w[3];
        memcpy(v, p + (size_t)i * stride, 12);
        for (int a = 0; a < 3; ++a)
        {
            w[a] = v[0] * W[a] + v[1] * W[4 + a] + v[2] * W[8 + a] + W[12 + a];
            lo[a] = fminf(lo[a], w[a]), hi[a] = fmaxf(hi[a], w[a]);
        }
        if (nl > 0.0f)
            continue;
        memcpy(q[got < 3 ? got : 2], w, 12);
        got = got < 3 ? got + 1 : 3;
        if (got == 3)
        {
            const float a[3] = { q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2] };
            const float b[3] = { q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2] };
            n[0] = a[1] * b[2] - a[2] * b[1], n[1] = a[2] * b[0] - a[0] * b[2], n[2] = a[0] * b[1] - a[1] * b[0];
            const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            const float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (l > 0.05f * la * lb && l > 1e-6f)
                nl = l, n[0] /= l, n[1] /= l, n[2] /= l;
            else
                got = 2; /* a sliver (or a repeated vertex): its last corner replaced by the next */
        }
    }
    if (!(nl > 0.0f) || fabsf(n[1]) < 0.82f)
        return 0;
    const float size = fmaxf(fmaxf(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
    const float tol = 0.3f + 0.04f * size;
    for (uint32_t i = 0; i < nverts; ++i)
    {
        float v[3], w[3];
        memcpy(v, p + (size_t)i * stride, 12);
        for (int a = 0; a < 3; ++a)
            w[a] = v[0] * W[a] + v[1] * W[4 + a] + v[2] * W[8 + a] + W[12 + a];
        if (fabsf((w[0] - q[0][0]) * n[0] + (w[1] - q[0][1]) * n[1] + (w[2] - q[0][2]) * n[2]) > tol)
            return 0;
    }
    return 1;
}

#include "d3d8_water_memo.h" /* the footsteps' water under a sheet's box, kept by the box (sheet_floors) */

/* A sheet of the game's made VanaCore's water, from water_draw for a draw that's neither a tile nor a water model's: 0 not one,
 * 1 one to leave out (over VanaCore's water drawn this frame already: a second layer of the same water), 2 made water and to keep
 * for the pass (water_keepable), else drawn in its place */
static int water_sheet(GfxDraw* d, uint32_t prim, uint32_t nverts, uint32_t up_data)
{
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    if (up_data || d->vs.rhw || !d->pipe.blend || !d->depth.zenable || (d->pipe.src == 2 && d->pipe.dst == 2) ||
        !(d->fs.fog || d->vs.fog_vertex) || (prim != GFX_TRIANGLELIST && prim != GFX_TRIANGLESTRIP) || !e->used ||
        e->type != GFX_FLOAT3 || e->stream >= GFX_NSTREAMS)
        return 0;
    const Obj* vb = obj(g_dev.cur.stream[e->stream]);
    if (!vb || (vb->usage & USAGE_DYNAMIC))
        return 0;
    float lo[3], hi[3], W[16];
    const float* V = water_frames(W);
    /* a draw with the view folded into its world matrix is a stream's own layer (West Ronfaure's: a lit sheet the ground's water
     * is made from, and an unlit twin whose unfolded box comes out wrong-shaped): not a sheet for this test. Taken here, the lit
     * one was VanaCore's water in its place, opaque, with no bed (dark navy by day), in the frames its sample points fell on
     * water floors, and the ground's water in the others */
    if (g_water_folded)
    {
        /* the unlit one, blended: the shimmer the game lays over the stream, which gives the game's stream its brightness.
         * Drawn in the world it would lie under VanaCore's water; kept, it's drawn again as the game draws it after the
         * ground's water (water_pass), so the stream has VanaCore's water under the game's shimmer */
        /* ... and only a sheet: flat in some plane. Any unlit blended draw with the view folded in passed once, and Lower Jeuno's
         * window light (a cone of light down a wall) was kept and drawn again after the water, bright over the wall */
        if (g_water_folded > 0 && !d->vs.lighting && g_bake_nv && water_keepable(d) && sheet_planar(d, nverts, W)) /* blended: every sheet here is */
        {
            g_water_as_is = 1; /* kept at the world's keep point (draw_packet), lit by then as the world is */
            return 2;
        }
        return 0;
    }
    if (!water_extent(d, nverts, W, lo, hi))
        return 0;
    const float dx = hi[0] - lo[0], dz = hi[2] - lo[2], rise = hi[1] - lo[1];
    if (dx < 3.0f || dz < 3.0f || rise > fmaxf(3.0f, 0.1f * fmaxf(dx, dz)))
        return 0;
    const int flat = rise <= fmaxf(0.5f, 0.03f * fmaxf(dx, dz));
    int t[3];
    float dy[3];
    const int took = flat && sheet_floors(lo, hi, t, dy) >= 2;
    if (g_water_set.log)
        sheet_log(d, nverts, lo, hi, t, dy, took);
    /* a small blended sheet lying on the ground's water, writing no depth (the skirts the game blends a stream into its banks
     * with: 8 by 22 yalms at the water's height, a bank's floors under them) is left out: drawn over VanaCore's water it was a
     * flat lighter patch by day and a dark slab by night, with a hard edge at the bank; the water's own shore does what it did.
     * Never the stream's own sheets (40 yalms and more), which the ground's water is made from */
    /* (a textured layer lying on the ground's water, Jugner Forest's floating leaves, was kept here to draw again over VanaCore's
     * water as the game draws it, as the shimmer is; added or blended over the brighter water it came out as white shapes with
     * the water's rim, so it draws in its place as before, blue-tinted under the water) */
    if (!took && !d->depth.zwrite && dx <= 35.0f && dz <= 35.0f && rise <= 3.0f && water_ground_under(lo, hi))
        return 1;
    if (!took)
        return 0;
    if (water_covered(lo, hi))
        return 1;
    if (d->vs.prog || d->fs.prog) /* the zone's shaders: the water's functions in their place, the position where they read it */
    {
        d->vs.prog = 0, d->fs.prog = 0, d->vs_tokens = NULL, d->ps_tokens = NULL;
        d->vs.ntex = 0, d->vs.lighting = 0, d->vs.fog_vertex = 0;
        d->fs.fog = 3; /* D3DFOG_LINEAR: the world's fog, as the game's water has it */
    }
    water_shade(d, lo, hi, W, V, 1);
    return water_keepable(d) ? 2 : 0;
}

static void water_ground(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, uint32_t up_data); /* below */
static int water_ground_under(const float lo[3], const float hi[3]);                                            /* below */

/* The water's own draws, after decal_pull (a tile pushed back is pushed back); model is water_model's for the draw, lo and hi
 * its extent. Returns 1 for a draw to leave out.
 *   The debug view colours them and does nothing else: a water model's level surface that mode 1 would make VanaCore's water
 *     magenta, the rest of a water model's draws orange, a tile found by its shape alone green.
 *   A tile - detect 0 or 2: by its shape; 1 or 2: a water model's level surface drawn solid (no blending, writing depth: the
 *     tile's part) - mode 1 makes VanaCore's water (turned or scaled too: the functions take its matrices); mode 0 pushes it back.
 *   A water model's level surface drawn blended (detect 1 or 2), mode 1: left out over VanaCore's water drawn this frame (a
 *     tile's, or a layer's under it), else made VanaCore's water itself, solid and writing depth - a pond or a river the game
 *     draws as layers alone (Rabao's) gets its water too, and the layers stacked over it are left out. Mode 0: the game's.
 *   The rest of a water model's draws - a waterfall, a river running downhill, a ripple or a splash - are drawn as
 *     the game draws them in either mode, never left out: VanaCore's water is a level surface (waves across the world's x and
 *     z, the sky mirrored about the up axis), which on a waterfall would be a sky reflection sliding sideways, and nothing would
 *     stand in for one left out.
 *   Returns 2 for a draw made VanaCore's water that the pass can draw again at the scene's end (water_keepable): draw_packet
 *     keeps it (water_keep) once the world's light is on it, in place of drawing it. */
static uint32_t water_floors_zone(void); /* below: the zone whose floors are held (0 none) */
/* water_draw's debug view: a water model's level surface magenta, its other draws orange, a tile found by its shape green, and the
 * ground's water cyan (water_ground_pass) */
static void water_draw_view(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, uint32_t up_data, int model, int shape)
{
    if (model || shape)
        water_view(d, model == 1 ? 0xFF00FFu : model ? 0xFF8000u : 0x00FF00u);
    else if (g_water_mode == 1 && g_water_set.detect == 2)
        water_ground(d, prim, count, nverts, up_data);
}

/* water_draw's draw that is neither a tile nor a water model's surface: a sheet of the zone's own meshes (water_sheet), else the
 * ground's baked water (water_ground), with detect 2; what water_draw returns */
static int water_draw_other(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, uint32_t up_data, int model)
{
    if (g_water_mode != 1 || g_water_set.detect != 2 || model)
        return 0;
    const int sheet = water_sheet(d, prim, nverts, up_data);
    if (!sheet)
        water_ground(d, prim, count, nverts, up_data);
    return sheet;
}

/* An additive level surface of the game's (source alpha, destination one) that stays the game's own: one with the zone's
 * collision's water under it (the body of water_draw's rule, split out) */
static int water_additive_game(const float lo[3], const float hi[3])
{
    /* ... when the zone's collision has water under it (Rabao's: its footsteps splash, 1221 water triangles). A basin the
     * collision knows nothing of (Bastok Markets' fountain, laid the same way, its cobbles about a yalm down, no wading in it)
     * is VanaCore's water: the pass shows the bed through it, foams the rim and froths under the jets. The collision's water
     * floor is looked for under three points along the surface's box */
    float under = 0.0f;
    int n = 0;
    for (int i = 0; i < 3; ++i)
    {
        const float k = 0.25f + 0.25f * (float)i;
        const float p[3] = { lo[0] + (hi[0] - lo[0]) * k, (lo[1] + hi[1]) * 0.5f, lo[2] + (hi[2] - lo[2]) * k };
        float dy;
        if (grass_floor_near(p, 6.0f, &dy) >= 0 && dy > 0.0f)
            under += dy, ++n;
    }
    if (g_water_set.log) /* once a surface a zone (its box's corner), the first 64 */
    {
        static uint32_t said_n, said_zone;
        static float said_x[64], said_z[64];
        if (said_zone != water_floors_zone())
            said_zone = water_floors_zone(), said_n = 0;
        uint32_t j = 0;
        while (j < said_n && (said_x[j] != lo[0] || said_z[j] != lo[2]))
            ++j;
        if (j == said_n && said_n < 64)
            said_x[said_n] = lo[0], said_z[said_n] = lo[2], ++said_n, rt_log("[recomp] water: additive level surface x %.0f..%.0f z %.0f..%.0f at %.2f: the floor under it at %d of 3 points, %.2f under on average (none: VanaCore's water)%.0s\n",
                lo[0], hi[0], lo[2], hi[2], (lo[1] + hi[1]) * 0.5f, n, n ? under / (float)n : 0.0f, 0.0f);
    }
    return n > 0;
}

static int water_draw(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, uint32_t up_data, int model, const float lo[3], const float hi[3])
{
    float tlo[3], thi[3], W[16];
    const int view = g_water_set.view, detect = g_water_set.detect;
    if (g_water_mode == 1 && detect && d->vs.lighting && !d->vs.rhw)
        water_lights(d, up_data);
    /* by its shape too in mode 0 whatever detect says: a tile that isn't a water model's is still pushed back */
    const int shape = (view || detect != 1 || g_water_mode == 0) && water_tile_shape(d, prim, nverts, tlo, thi);
    const float* V = water_frames(W);
    if (view)
    {
        water_draw_view(d, prim, count, nverts, up_data, model, shape);
        return 0;
    }
    if (model == 2 && g_water_mode == 1) /* a waterfall's foot, for the water it lands in */
        water_foot_note(lo, hi);
    const int surface = model == 1 && detect != 0 && d->depth.zenable;
    if (!shape && !surface) /* neither: a sheet of the zone's own meshes, or the ground's baked water, with detect 2 */
        return water_draw_other(d, prim, count, nverts, up_data, model);
    if (shape || (!d->pipe.blend && d->depth.zwrite)) /* a tile */
    {
        /* a water model's solid surface is a tile only as a tile is: untextured, not alpha-tested. Foam, lily pads, a flat
         * rock in a waterfall's model are the game's */
        if (!shape && (obj(g_dev.cur.tex[0]) || g_dev.cur.rs[15] /* D3DRS_ALPHATESTENABLE */))
            return 0;
        if (g_water_mode == 0)
        {
            float pull = -g_water_tile_back;
            if (pull < 0.0f)
                memcpy(&d->u.pad0[2], &pull, 4);
        }
        else
        {
            water_shade(d, shape ? tlo : lo, shape ? thi : hi, W, V, 1);
            sea_note(shape ? tlo : lo, shape ? thi : hi, 1); /* the game's own water: a tile, or a water model's solid sheet */
            if (shape)
                ++g_water_shape_tiles;
            if (sea_own(shape ? tlo : lo, shape ? thi : hi))
                return 1; /* the sea's: VanaCore's plane is drawn in its place */
            return water_keepable(d) ? 2 : 0;
        }
        return 0;
    }
    if (g_water_mode != 1)
        return 0;
    /* an additive layer (source alpha, destination one) is light laid over the bed, not a surface: Rabao's pond, whose bed is
     * a hand under it and whose collision floors bake to no ground water. Made VanaCore's water it came out as its sunlit sand
     * bed with a sheen (the pass's bed-through look), where the game's layer reads plainly as a pale blue pond: the game's own */
    if (d->pipe.src == 5 && d->pipe.dst == 2 && water_additive_game(lo, hi))
        return 0;
    if (water_covered(lo, hi))
        return 1;
    /* a layer made VanaCore's water itself, solid: a pond's or a river's, 16 yalms each way as a tile is - not a fountain's
     * basin or a shallow stream, whose bed would go under it */
    if (hi[0] - lo[0] < 16.0f || hi[2] - lo[2] < 16.0f)
        return 0;
    water_shade(d, lo, hi, W, V, 1);
    sea_note(lo, hi, 1);
    if (sea_own(lo, hi))
        return 1;
    const int keep = water_keepable(d);
    if (g_water_set.log && !keep)
    {
        static uint32_t said;
        if (said < 8)
            ++said, rt_log("[recomp] water: a water model's surface x %.0f..%.0f z %.0f..%.0f made VanaCore's water in its place, not keepable: pass %d, kept %u, data %u bytes, indices %s\n",
                lo[0], hi[0], lo[2], hi[2], g_water_set.pass, g_water_kept_n, d->data[0] ? d->size[0] : 0, d->indices ? (d->ibuf ? "in a static buffer" : "the game's memory") : "none");
    }
    return keep ? 2 : 0;
}

/* One of the game's own layers over the water (mode 1, after a tile drawn this frame): unlit, blended, fogged, a static
 * flat mesh - the sheets the game lays over its tiles, in colour and as the shimmer - not the sprites (dynamic, or with a second
 * stage), the sky (unfogged) or lit decals. Dropped: VanaCore's water is the look. By their shape (detect 0 or 2); a water
 * model's level surface with detect 2 is water_draw's. */
static int water_layer(const GfxDraw* d, uint32_t prim, uint32_t nverts, uint32_t up_data)
{
    const State* s = &g_dev.cur;
    if (g_water_mode != 1 || !g_water_shape_tiles || g_water_set.view || g_water_set.detect == 1 ||
        (d->water == 1 && g_water_set.detect && d->depth.zenable) || up_data || s->vs != 0x152 || d->vs.rhw || d->vs.prog ||
        d->fs.prog || d->vs.lighting || !d->pipe.blend || d->fs.fog != 3 || s->tss[1][1] != 1 /* D3DTOP_DISABLE: no second stage */ ||
        (prim != 5 && prim != 4))
        return 0;
    const Obj* vb = obj(s->stream[d->vs.el[GFX_R_POSITION].stream]);
    if (!vb || (vb->usage & USAGE_DYNAMIC))
        return 0;
    float lo[3], hi[3], W[16];
    water_frames(W);
    if (!water_extent(d, nverts, W, lo, hi) || hi[1] - lo[1] > 0.2f || (hi[0] - lo[0] < 1.0f && hi[2] - lo[2] < 1.0f))
        return 0;
    return 1;
}


/* --- the water pass: VanaCore's water drawn after the world ------------------------------------------------------------- */
/* With [graphics] water.pass (g_water_set.pass, the default), a draw made VanaCore's water doesn't go out in its place in the
 * world: it's kept (water_keep: a copy of the draw as it would have gone out, lit as the world is, its vertices in the game's
 * static buffers, which stay through the frame) and drawn once the world and the grass are (water_pass, from scene_finish), with
 * copies of the scene's colour and depth as they are then (gfx_scene_snapshot) in its texture slots 6 and 7. The pixel function
 * (gfx_hlsl_water.h, GfxFsKey.water 2) reads the depth under each of its pixels for how deep the water is there - the bed shows
 * through in the shallows, the water's own colour takes over with depth, a soft edge and a line of foam where the depth reaches
 * nothing at the shore - and the scene's colour, bent by the waves, for what shows through. The water writes its own depth, so
 * what the scene effects do by depth after it (the fog, the sun's shadows, the god rays) takes the surface as a surface. Without
 * the copies (no depth of the target's size to copy) the kept draws go out as they were, opaque (water 1).
 *   Kept from the scene's own draws alone, into the scene's target: a late one (after the effects) goes out in its place as
 * before. A frame's kept draws are let go of at its Present (g_water_kept_n). */
typedef struct WaterKept
{
    GfxDraw d;
    GfxTex* rt; /* the target it was drawn into: the pass draws into the scene's alone */
    int as_is;  /* drawn again as the game draws it, after the ground's water: the shimmer the game lays over a stream */
    /* a dynamic draw's vertices and indices, copied: the memory the game handed over is its own to write over before the
     * scene ends (Bastok Markets' fountain basin is drawn so, "cpu 1" in the water log); the copies are this slot's and grow */
    void* copy[GFX_NSTREAMS];
    uint32_t copy_cap[GFX_NSTREAMS];
    void* icopy;
    uint32_t icopy_cap;
} WaterKept;
/* room for n bytes in a kept slot's copy (realloc'd, kept across frames) */
static void* water_copy_room(void** buf, uint32_t* cap, uint32_t n)
{
    if (*cap < n)
    {
        void* v = realloc(*buf, n);
        if (!v)
            return NULL;
        *buf = v, *cap = n;
    }
    return *buf;
}
static WaterKept g_water_kept[WATER_KEPT];
static struct
{
    GfxTex *c, *d; /* the pass's copies of the scene's colour and depth (gfx_scene_snapshot), for a water draw after the scene is done */
    uint32_t w, h, frame;
} g_water_late;
static void water_pass_draw(GfxDraw* d, GfxTex* colour_copy, GfxTex* depth_copy, uint32_t w, uint32_t h, const GfxScene* s);

/* A draw kept for the pass reads the game's textures and buffers as they were when it was kept; the game may release them
 * before the scene ends (at a zone change, a settings reload: the kept draw's vertex buffer or shimmer texture gone), and
 * drawn then it handed Direct3D 11 freed views (DXMT's "pure virtual function called" trap on the pass's first draw, Yuhtunga
 * Jungle, three times). So d3d8.c's object release drops every kept draw that reads the object (count 0: gfx_draw skips it) */
static void water_kept_drop(const GfxTex* t, const GfxBuf* b)
{
    if (!t && !b)
        return;
    for (uint32_t i = 0; i < g_water_kept_n; ++i)
    {
        GfxDraw* d = &g_water_kept[i].d;
        int reads = 0;
        for (int j = 0; j < 8 && !reads; ++j)
            reads = t && d->tex[j] == t;
        for (int s = 0; s < GFX_NSTREAMS && !reads; ++s)
            reads = b && d->buf[s] == b;
        if (reads || (b && d->ibuf == b))
            d->count = 0;
    }
}

/* A kept draw's static streams and indices copied too (its vertices as the CPU has them now, d->cpu): the game writes some of its
 * "static" buffers again within a frame (d3d8.c Buffer_Lock marks them for upload, and the upload goes in command order), so a
 * kept draw read from the buffer at the scene's end drew whatever the game had put there since, through the tile's matrices:
 * thin stray triangles of water on the ground (Lower Jeuno's terrace, by the statue). Too large to copy: read from the buffer */
static void water_keep_static(WaterKept* k, const GfxDraw* d, uint32_t prim, uint32_t count)
{
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        if (!d->buf[s] || !d->cpu[s] || !d->cpu_size[s] || d->cpu_size[s] > WATER_KEEP_COPY)
            continue;
        void* v = water_copy_room(&k->copy[s], &k->copy_cap[s], d->cpu_size[s]);
        if (!v)
            continue;
        memcpy(v, d->cpu[s], d->cpu_size[s]);
        k->d.data[s] = v, k->d.size[s] = d->cpu_size[s];
        k->d.buf[s] = NULL, k->d.buf_off[s] = 0;
    }
    if (d->indices && d->ibuf)
    {
        const uint32_t n = water_prim_indices(prim, count) * (d->index_size ? d->index_size : 2);
        void* v = n <= WATER_KEEP_COPY ? water_copy_room(&k->icopy, &k->icopy_cap, n) : NULL;
        if (v)
            memcpy(v, d->indices, n), k->d.indices = v, k->d.ibuf = NULL, k->d.ibuf_off = 0;
    }
}

/* Keeps the draw for the pass (kind: what water_draw gave; 2 is a draw to keep), from draw_packet once the world's light is on
 * it. 0 for any other kind, or when it can't be kept (the scene is done: a late draw; the target is a level or face of a
 * texture; no scene), and the draw goes out in its place */
static int water_keep(GfxDraw* d, uint32_t prim, uint32_t count, int kind)
{
    uint32_t f = 0, l = 0;
    const Obj* rt = kind == 2 ? obj(g_dev.rt) : NULL;
    GfxTex* c = rt ? surface_gpu(rt, &f, &l) : NULL;
    /* a water draw after the scene is done (Bastok Markets' fountain basin: the game draws it after the world and the effects,
     * when the pass has run): drawn in its place, now, with the pass's look over the pass's copies of the scene (this frame's;
     * else a copy taken now) - the bed through it, the rim's foam, the jets' froth - not as the opaque sheet. Not a draw to keep as
     * the game draws it (g_water_as_is: never made water), which goes out as the game's after the pass already */
    if (c && !f && !l && kind == 2 && (g_scene.done || g_scene.world_done) && g_water_set.pass && !g_water_as_is)
    {
        GfxTex *cc = NULL, *dc = NULL;
        uint32_t w = 0, h = 0;
        /* the pass's copies of this frame's scene, or the frame before's (the draw comes after the frame counter moved on): a
         * copy taken now would be of a depth buffer cleared for the next frame, and under the basin the bed read as the far
         * plane, the water opaque navy */
        const int stashed = g_water_late.c && (g_water_late.frame == g_scene.frame || g_water_late.frame + 1 == g_scene.frame);
        if (stashed)
            cc = g_water_late.c, dc = g_water_late.d, w = g_water_late.w, h = g_water_late.h;
        else if (gfx_scene_snapshot(c, &cc, &dc, &w, &h))
            g_water_late.c = cc, g_water_late.d = dc, g_water_late.w = w, g_water_late.h = h, g_water_late.frame = g_scene.frame;
        if (g_water_set.log)
        {
            static uint32_t said;
            if (said < 8)
                ++said, rt_log("[recomp] water: a late water draw drawn with the pass's look over %s (the pass's frame %u, this frame %u)\n",
                    stashed ? "the pass's copies" : cc ? "a copy taken now" : "nothing: in its place", g_water_late.frame, g_scene.frame);
        }
        if (cc)
        {
            water_pass_draw(d, cc, dc, w, h, &g_scene.s);
            g_water_as_is = 0;
            return 0;
        }
    }
    if (!c || f || l || g_scene.done || g_scene.world_done || !g_scene.draws || g_water_kept_n >= WATER_KEPT)
    {
        if (g_water_set.log && kind == 2)
        {
            static uint32_t said;
            if (said < 8)
                ++said, rt_log("[recomp] water: a draw to keep not kept: target %s, level %u face %u, scene done %d, world done %d, scene draws %u, kept %u of %d\n",
                    c ? "the scene's" : "not a scene target", f, l, g_scene.done, g_scene.world_done, g_scene.draws, g_water_kept_n, WATER_KEPT);
        }
        g_water_as_is = 0;
        return 0;
    }
    WaterKept* k = &g_water_kept[g_water_kept_n++];
    k->d = *d;
    k->d.prim = prim, k->d.count = count;
    k->rt = c;
    k->as_is = g_water_as_is, g_water_as_is = 0;
    /* a dynamic draw's vertices and indices copied into the slot's own memory (water_keepable sized them) */
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (d->data[s] && d->size[s])
        {
            void* v = water_copy_room(&k->copy[s], &k->copy_cap[s], d->size[s]);
            if (!v)
            {
                --g_water_kept_n;
                return 0;
            }
            memcpy(v, d->data[s], d->size[s]);
            k->d.data[s] = v;
        }
    if (d->indices && !d->ibuf)
    {
        const uint32_t n = water_prim_indices(prim, count) * (d->index_size ? d->index_size : 2);
        void* v = water_copy_room(&k->icopy, &k->icopy_cap, n);
        if (!v)
        {
            --g_water_kept_n;
            return 0;
        }
        memcpy(v, d->indices, n);
        k->d.indices = v;
    }
    water_keep_static(k, d, prim, count);
    return 1;
}

/* A kept draw made the pass's (GfxFsKey.water 2): the scene's colour copy in slot 6 (clamped, filtered), its depth copy in slot
 * 7 (clamped, as it is), and in texture matrix 6's bytes 4-10 what reads the depth back into the world: the projection's z scale
 * and offset and its hand (proj[10], proj[14], proj[11]), the viewport's depth range (min, max - min), and the target's size in
 * pixels (the pixel's place over it is the uv) */
static void water_pass_draw(GfxDraw* d, GfxTex* colour_copy, GfxTex* depth_copy, uint32_t w, uint32_t h, const GfxScene* s)
{
    d->fs.water = 2;
    memset(d->tex, 0, sizeof d->tex);
    d->tex[6] = colour_copy, d->tex[7] = depth_copy;
    memset(d->fs.st, 0, sizeof d->fs.st);
    d->fs.st[6].tex = d->fs.st[7].tex = 1;
    d->fs.st[6].ncoord = d->fs.st[7].ncoord = 2;
    d->fs.nstages = 8;
    memset(d->samp, 0, sizeof d->samp);
    d->samp[6] = (GfxSampler){ 3, 3, 3, 2, 2, 0, 1, 0, 0, 0, { 0, 0 }, 0 }; /* CLAMP, LINEAR, no mips */
    d->samp[7] = (GfxSampler){ 3, 3, 3, 1, 1, 0, 1, 0, 0, 0, { 0, 0 }, 0 }; /* CLAMP, POINT */
    float minz, maxz;
    memcpy(&minz, &d->vp[4], 4), memcpy(&maxz, &d->vp[5], 4);
    if (!(maxz > minz))
        minz = 0.0f, maxz = 1.0f;
    float* tn = d->u.texm[6];
    tn[4] = s->proj[10], tn[5] = s->proj[14], tn[6] = s->proj[11] < 0.0f ? -1.0f : 1.0f, tn[7] = minz;
    tn[8] = maxz - minz, tn[9] = (float)w, tn[10] = (float)h;
}

/* --- the game's water baked into its ground ------------------------------------------------------------------------ */
/* The game's footsteps know water by the zone's collision: deep water (terrain 9) and shallow (8) are floors on the BED, under
 * the surface (Rabao's oasis bed lies 5 to 9 yalms under its sheet; the player wades). Where a zone bakes the water's look into
 * its lit ground (Jugner Forest's ponds, West Ronfaure's streams: opaque 40-yalm chunks with the water's texture, which no
 * draw-by-draw test tells from their banks) the surface is the chunk's own triangles that hover over such a floor. With
 * water.detect=2 the collision's water triangles are kept as they're handed over (d3d8_set_water_floors, with the grass's: their
 * box, for a cheap test), and each lit static chunk of the zone whose box reaches the water's is looked at once (water_ground):
 * its triangles whose three corners stand over a water floor (the nearest floor within 12 yalms is water, 10 cm or more under
 * the corner: the bed's own triangles, which the floor lies on, aren't) are kept in the world, lifted 3 cm (no z-fight with the chunk they come from), each corner with the floor's depth
 * under it (the bed's: the scene's depth under the water is the chunk's own, so the water's pixel function takes its depth from
 * the vertex instead, GfxFsKey.water 3; at least BAKE_SHALLOW, so the water is never nothing), and drawn as VanaCore's water at
 * the pass's end (water_ground_pass), each chunk's while the chunk is drawn. A chunk with none is remembered too, so the test
 * runs once. The pool and the chunks go at a zone change (the hand-over) and the buffer at a device loss (water_device_lost). */
#define BAKE_ENTRIES 8192 /* a place of a mesh each (bake_find); every lit chunk of the zone takes one. 2048 ran out in Caedarva
                           * Mire's forest, and the chunks seen after were never looked at: its pond water in some places only */
#define BAKE_MOST (4 * 300000) /* floats kept a zone, at most */
#define BAKE_LIFT 0.03f
#define BAKE_SHALLOW 0.4f
#define BAKE_DEEP 1.0f   /* the depth of the deep water's surface at the chunk's own level (no bed in the collision under it) */
#define BAKE_LEAST 64    /* triangles: a chunk with fewer is a decal (a leaf, a post's top) */
#define BAKE_UNDER 0.1f /* a corner is over water with the water floor at least this far under it: the bed's own triangles (the
                         * floor lies on them: Rabao's oasis bed, a stream's) aren't the surface */
#define BAKE_LEVEL 0.25f /* ... or with the deep water's own floor (terrain 9: the surface, where the player swims or stands) under
                          * or over it by less than this: the chunk is the deep water's surface. Caedarva Mire's pond is that over
                          * most of it: taken only where its floor dipped 10 cm, the rest stayed the game's murk round squares of
                          * VanaCore's water */
#define BAKE_ABOVE 3.0f /* a sheet mostly this far or more over the deep water's own floor (the surface) stands over the water, not in
                         * it: Selbina's ferry's lower deck, 8 yalms over the sea, was VanaCore's water and its fill cells on the pier */
#define BAKE_STRIDE 4 /* x, y, z, the bed's depth: negated for a chunk that writes depth (the scene's depth under its water is its
                       * own: the pixel function takes the vertex's), plus BAKE_FILL_TAG for a hole's cell (bake_fill) */
#define BAKE_FILL_TAG 1000.0f
typedef struct WaterBake
{
    uintptr_t key;     /* the draw's first vertex on the CPU, and its indices */
    uint32_t count, prim;
    uint32_t at, n;    /* its water's vertices in the pool (n 0: none) */
    uint32_t seen;     /* the frame it was last drawn in (g_scene.frame); drawn by the pass this frame or the next */
    float lo[3], hi[3]; /* its box in the world (with water) */
    uint32_t gen;      /* the sea's generation it was baked at (g_sea.gen): one without water is looked at again once the sea's level is known */
    float t[3];        /* where it was drawn: its world matrix's translation (bake_find: one mesh at several places, an entry each) */
    int seed;          /* 1 an additive layer, 2 a pond's surface: its pond flooded from it (bake_flood), its own triangles not the water */
    int wet;           /* in g_bake_wet */
    int moved;         /* its mesh is a model the game moves: no water (bake_moving) */
} WaterBake;
static WaterBake g_bake[BAKE_ENTRIES];
static uint32_t g_bake_n, g_bake_nv, g_bake_cap, g_bake_buf_n;
static uint16_t g_bake_wet[BAKE_ENTRIES]; /* the entries that have had water, for the loops each frame (most entries have none) */
static uint32_t g_bake_wet_n;
static void bake_wet(WaterBake* b)
{
    if (b->n && !b->wet)
        b->wet = 1, g_bake_wet[g_bake_wet_n++] = (uint16_t)(b - g_bake);
}
static float* g_bake_v;
static int g_bake_dirty;
static GfxBuf* g_bake_buf;
#define BAKE_SAID 48 /* what water.log says of the chunks looked at: this many of each kind of line a zone, the first ones */
static uint32_t g_bake_said[3]; /* chunks with water, chunks without, draws left */
static uint32_t g_bake_calls, g_bake_found, g_bake_hits, g_bake_folded; /* this frame, for the pass's log line: water_ground called,
                                                                        * an entry found, one with water found, a folded draw */
static struct
{
    uint32_t n;         /* the collision's water triangles (0: none, or the zone's floors not known) */
    float lo[3], hi[3]; /* their box in the world */
    float zlo[3], zhi[3]; /* the zone's floors' box in the world, every terrain: the sea's tiles run past it (water_horizon_pass) */
    int zone;             /* whose floors (0: none) */
    float rim_y;          /* the lowest point of the water floors at the zone's rim (within SEA_RIM of the box's edge): the shore's
                           * level where the sea is painted on the sky's dome and has no tiles (Valkurm Dunes); rim_n corners */
    uint32_t rim_n;
} g_wfloor;
static uint32_t water_floors_zone(void) { return g_wfloor.zone; }

/* The sea, for water_horizon_pass: the lowest level of the game's water tiles that reach the zone's rim (sea_note) */
static struct
{
    float y;      /* its height (the world's y points down: the largest; a chunk's lowest point) */
    uint32_t n;   /* tile draws, or chunks, at it */
    uint32_t gen; /* changed as the level is set or first reaches SEA_LEAST: chunks without water are looked at again (water_ground) */
    int said;     /* the log has said */
    uint32_t full; /* the frame (g_scene.frame) VanaCore's sea plane was last drawn (water_horizon_pass): the game's tiles are dropped
                    * only while it is (sea_own), never with no sea at all */
} g_sea;
#define SEA_PLANES 2.0f /* the game's level water up to this far over the sea's level is the sea's too: Lower Jeuno lays its harbour in
                       * two or three sheets a little apart, which showed as steps and seams in the water */
#define SEA_RIM 60.0f   /* a tile within this of the zone's floors' box edge, or past it, is the sea's; a marsh's tiles lie well inside */
#define SEA_LEAST 4     /* tile draws, or chunks, at a level before the sea is drawn on */
#define SEA_UNDER 0.15f /* the horizon's sheet this far under the tiles' level: where the tiles are, they win the depth test */
#define SEA_OVER 0.1f   /* ... and this far over the shore's level where the sea has no tiles: it lies over the wet sand's end, which it fades in across */
#define SEA_EDGE 120.0f /* the sheet fades in from nothing over this much from its inner edge (its first ring's depth 0 to SEA_DEPTH) */
#define SEA_DEPTH 30.0f /* the bed's depth the sheet's pixels take: the deep water's colour */
#define SEA_IN 100.0f   /* the sheet starts this far inside the floors' box: under the last tiles, chunks or shore, with no gap to them */
#define SEA_RIM_LEAST 3000u /* water corners at the rim before the shore's level counts as a sea's (Valkurm's has 10,500; a stream's end near a zone's edge a few hundred) */

#include "d3d8_water_level.h" /* the sea's level (sea_zone, sea_level, sea_note, sea_own, sea_dry) */
static void bake_forget(void); /* d3d8_water_bake.h: the entries' index and the ponds' flooded cells forgotten */
void d3d8_set_water_floors(const GrassFloors* f)
{
    g_wfloor.n = 0, g_wfloor.zone = 0, g_wfloor.rim_n = 0, g_bake_n = 0, g_bake_wet_n = 0, g_bake_nv = 0, g_bake_dirty = 1;
    bake_forget();
    g_water_nsun = 0; /* the last zone's sun (water_lights): the new one's lit draws give its own */
    g_sea.n = 0, g_sea.said = 0, g_dry.total = 0.0f, g_dry.level_n = 0;
    memset(g_bake_said, 0, sizeof g_bake_said);
    if (!f || !f->zone || !f->npairs || !f->tris || !f->terrain || !f->mesh_first || !f->matrices || !f->pairs)
        return;
    for (int a = 0; a < 3; ++a)
        g_wfloor.lo[a] = g_wfloor.zlo[a] = 1e30f, g_wfloor.hi[a] = g_wfloor.zhi[a] = -1e30f;
    g_wfloor.zone = f->zone;
    for (uint32_t k = 0; k < f->npairs; ++k)
    {
        const uint32_t mi = f->pairs[2 * k], mesh = f->pairs[2 * k + 1];
        if (mi >= f->nmatrices || mesh >= f->nmesh)
            continue;
        const float* m = &f->matrices[mi * 12];
        for (uint32_t t = f->mesh_first[mesh]; t < f->mesh_first[mesh + 1] && t < f->ntris; ++t)
        {
            const int water = f->terrain[t] == 8 || f->terrain[t] == 9;
            for (int c = 0; c < 3; ++c)
            {
                const float* p = &f->tris[t * 9 + c * 3];
                for (int j = 0; j < 3; ++j)
                {
                    const float o = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
                    g_wfloor.zlo[j] = o < g_wfloor.zlo[j] ? o : g_wfloor.zlo[j], g_wfloor.zhi[j] = o > g_wfloor.zhi[j] ? o : g_wfloor.zhi[j];
                    if (water)
                        g_wfloor.lo[j] = o < g_wfloor.lo[j] ? o : g_wfloor.lo[j], g_wfloor.hi[j] = o > g_wfloor.hi[j] ? o : g_wfloor.hi[j];
                }
            }
            g_wfloor.n += (uint32_t)water;
        }
    }
    /* the water floors at the zone's rim: their lowest point is the shore's level for the sea drawn on where the zone has no sea of
     * its own (water_horizon_pass); the zone's box is known now, so a second pass over the water triangles alone */
    g_wfloor.rim_y = -1e30f;
    for (uint32_t k = 0; g_wfloor.n && k < f->npairs; ++k)
    {
        const uint32_t mi = f->pairs[2 * k], mesh = f->pairs[2 * k + 1];
        if (mi >= f->nmatrices || mesh >= f->nmesh)
            continue;
        const float* m = &f->matrices[mi * 12];
        for (uint32_t t = f->mesh_first[mesh]; t < f->mesh_first[mesh + 1] && t < f->ntris; ++t)
        {
            if (f->terrain[t] != 8 && f->terrain[t] != 9)
                continue;
            for (int c = 0; c < 3; ++c)
            {
                const float* p = &f->tris[t * 9 + c * 3];
                float o[3];
                for (int j = 0; j < 3; ++j)
                    o[j] = p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j];
                if (o[0] < g_wfloor.zlo[0] + SEA_RIM || o[0] > g_wfloor.zhi[0] - SEA_RIM || o[2] < g_wfloor.zlo[2] + SEA_RIM ||
                    o[2] > g_wfloor.zhi[2] - SEA_RIM)
                    g_wfloor.rim_y = o[1] > g_wfloor.rim_y ? o[1] : g_wfloor.rim_y, ++g_wfloor.rim_n;
            }
        }
    }
    sea_dry(f);
    if (g_water_set.log)
        rt_log("[recomp] water: zone %d's collision has %u water triangles, x %.0f..%.0f z %.0f..%.0f heights %.1f..%.1f; its floors' box "
               "x %.0f..%.0f z %.0f..%.0f; %u water corners at its rim, the lowest at height %.1f\n", f->zone, g_wfloor.n, g_wfloor.lo[0],
            g_wfloor.hi[0], g_wfloor.lo[2], g_wfloor.hi[2], g_wfloor.lo[1], g_wfloor.hi[1], g_wfloor.zlo[0], g_wfloor.zhi[0], g_wfloor.zlo[2],
            g_wfloor.zhi[2], g_wfloor.rim_n, g_wfloor.rim_n ? g_wfloor.rim_y : 0.0f);
}

#include "d3d8_water_ocean.h" /* the open sea's FFT waves (ocean_update), for VanaCore's sea plane */

/* the device lost (d3d8_device_lost): the pool's buffer went with it, made again at the next pass */
static void water_device_lost(void)
{
    gfx_buf_destroy(g_bake_buf), g_bake_buf = NULL, g_bake_dirty = 1;
    ocean_lost();
}

/* the CPU row of index slot i of the draw (its own row for a draw without indices) */
static int32_t bake_row(const GfxDraw* d, uint32_t i)
{
    if (!d->indices)
        return (int32_t)i;
    uint32_t v = 0;
    memcpy(&v, (const uint8_t*)d->indices + (size_t)i * d->index_size, d->index_size == 4 ? 4 : 2);
    return (int32_t)v + d->u.vofs;
}

/* a corner of the chunk in the world (through its world matrix W): 1 standing over a water floor (BAKE_UNDER or more under it),
 * or on the deep water's own (within BAKE_LEVEL); 2 over another floor, or a shallow water floor at or over it; 0 with no floor
 * within reach, or the row out of the draw. t: the floor's terrain, dy how far under */
static int bake_corner(const GfxDraw* d, int32_t row, uint32_t nverts, const float* W, float o[3], int* t, float* dy)
{
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    *t = -1, *dy = 0.0f;
    if (row < 0 || (uint32_t)row >= nverts)
        return 0;
    float v[3];
    memcpy(v, (const uint8_t*)d->cpu[e->stream] + (size_t)row * g_dev.cur.stride[e->stream] + d->u.offset[GFX_R_POSITION], 12);
    for (int j = 0; j < 3; ++j)
        o[j] = v[0] * W[j] + v[1] * W[4 + j] + v[2] * W[8 + j] + W[12 + j];
    *t = grass_floor_near(o, 12.0f, dy);
    return *t < 0 ? 0 : ((*t == 8 || *t == 9) && *dy >= BAKE_UNDER) || (*t == 9 && fabsf(*dy) < BAKE_LEVEL) ? 1 : 2;
}
#include "d3d8_water_fill.h" /* the holes in a stream's sheet made water (bake_fill) */
/* A flat chunk (its highest point hi_y) taken whole by bake_build: a quarter of its corners over water floors - or, at the sea's
 * level in a zone with a sea, over no floor within reach (the sea's bed lies deeper than the collision is looked for, 12 yalms,
 * so the sea's chunks past the shallows have no floor under them at all) - and not most of those BAKE_ABOVE over the deep
 * water's own floor (over the water, not its surface: a boat's deck) */
static int bake_whole(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, const float* W, float hi_y)
{
    uint32_t tally[3] = { 0, 0, 0 }, above = 0; /* corners with no floor, over water, over another floor; BAKE_ABOVE over the deep water's own */
    for (uint32_t k = 0; k < count; ++k)
        for (int c = 0; c < 3; ++c)
        {
            float o[3], dy;
            int t;
            const int r = bake_corner(d, bake_row(d, prim == GFX_TRIANGLELIST ? 3 * k + (uint32_t)c : k + (uint32_t)c), nverts, W, o, &t, &dy);
            ++tally[r], above += r == 1 && t == 9 && dy >= BAKE_ABOVE;
        }
    const int at_sea = g_sea.n >= SEA_LEAST && sea_zone() && fabsf(hi_y - g_sea.y) <= 1.5f; /* (the tiles set g_sea in any zone) */
    return 4 * (tally[1] + (at_sea ? tally[0] : 0)) >= tally[0] + tally[1] + tally[2] && 2 * above <= tally[1];
}

/* The chunk's water triangles into the pool: its entry's at and n. A flat chunk (a sheet: the stream's surface, a pond's) with
 * a quarter of its corners over water floors is taken whole, each corner as deep as the floor under it (the deep channel's
 * water floor, the shallows' stone bed; where a bank's floor lies over it the ground hides it): the game's footsteps call a
 * stream's shallows stone, and taking the sheet's triangles one by one left those as the game's, a hard seam across the stream.
 * Any other chunk gives nothing: its level triangles over a water floor were taken once, and a bank's ledge a yalm or two over
 * the stream came out as a flat patch of water floating at the bank's height. With water.log, the corners' tally */
static void bake_build(WaterBake* b, const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, const float lo[3], const float hi[3])
{
    float W[16];
    water_frames(W);
    const float dx = hi[0] - lo[0], dz = hi[2] - lo[2];
    /* flat: rising no more than 3 in 100 across it (Valkurm's sea chunks rise a yalm in 40, their waves) */
    const int flat = hi[1] - lo[1] <= fmaxf(0.3f, 0.03f * fmaxf(dx, dz)) && dx >= 8.0f && dz >= 8.0f;
    uint32_t tally[3] = { 0, 0, 0 }, level = 0; /* corners with no floor, over water, over another floor; on the deep water's own */
    int other_t = -1;
    float other_dy = 0.0f, other_at[3] = { 0, 0, 0 }, first[3] = { 0, 0, 0 }, dmin = 1e30f, dmax = -1e30f, dsum = 0.0f;
    b->at = g_bake_nv, b->n = 0;
    /* a flat chunk: its corners' tally first, then all or nothing */
    const int whole = flat && bake_whole(d, prim, count, nverts, W, hi[1]);
    /* not a sheet: nothing of it (a bank's level ledge over the stream was taken as water, floating at its own height); one lying in
     * a pond flooded at its level: nothing of its own either, the flood's water is the pond's */
    if (!whole || bake_flooded(lo, hi))
        goto said;
    for (uint32_t k = 0; k < count; ++k)
    {
        float o[3][3], depth[3];
        for (int c = 0; c < 3; ++c)
        {
            int t;
            float dy;
            const int r = bake_corner(d, bake_row(d, prim == GFX_TRIANGLELIST ? 3 * k + (uint32_t)c : k + (uint32_t)c), nverts, W, o[c], &t, &dy);
            ++tally[r];
            if (r == 2 && other_t < 0)
                other_t = t, other_dy = dy, memcpy(other_at, o[c], sizeof other_at);
            if (r == 1)
                dmin = dy < dmin ? dy : dmin, dmax = dy > dmax ? dy : dmax, dsum += dy;
            if (!k && !c)
                memcpy(first, o[c], sizeof first);
            const int surface = r == 1 && t == 9 && dy < BAKE_UNDER; /* on the deep water's own floor */
            level += (uint32_t)surface;
            depth[c] = surface ? BAKE_DEEP : r == 1 && dy > BAKE_SHALLOW ? dy : r ? BAKE_SHALLOW : SEA_DEPTH; /* no floor: the deep sea */
        }
        if (g_bake_nv + 3 > g_bake_cap)
        {
            const uint32_t cap = g_bake_cap ? g_bake_cap * 2 : 3072;
            float* v = cap * BAKE_STRIDE <= BAKE_MOST ? (float*)realloc(g_bake_v, (size_t)cap * BAKE_STRIDE * sizeof(float)) : NULL;
            if (!v)
                break;
            g_bake_v = v, g_bake_cap = cap;
        }
        for (int c = 0; c < 3; ++c)
        {
            float* w = &g_bake_v[(size_t)g_bake_nv * BAKE_STRIDE];
            w[0] = o[c][0], w[1] = o[c][1] - BAKE_LIFT, w[2] = o[c][2], w[3] = d->depth.zwrite ? -depth[c] : depth[c];
            ++g_bake_nv, ++b->n;
        }
    }
    /* an additive layer's: its pond flooded from it, its own triangles let go (bake_flood). A pond's surface's (mostly on the deep
     * water's own floor): the pond flooded from it the same way (seed 2), and other sheets' water in it let go */
    const int pond = !b->seed && 2 * level > tally[1];
    if (b->n && (b->seed || pond))
    {
        if (pond)
            sea_note(lo, hi, 0);
        bake_flood(b, (lo[1] + hi[1]) * 0.5f, pond, pond && d->depth.zwrite);
        if (pond)
            b->seed = 2, bake_pond_over(b, (lo[1] + hi[1]) * 0.5f);
    }
    else if (b->n) /* its holes made water (bake_fill); a sheet at the zone's rim: the sea's level (water_horizon_pass) */
        bake_fill(b, (lo[1] + hi[1]) * 0.5f), sea_note(lo, hi, 0);
    g_bake_dirty |= b->n != 0;
said:;
    const int kind = b->n ? 0 : 1;
    if (g_water_set.log && g_bake_said[kind] < BAKE_SAID)
    {
        ++g_bake_said[kind];
        rt_log("[recomp] water: ground chunk with %u water triangles of %u (%u vertices)%s, x %.0f..%.0f z %.0f..%.0f heights %.1f..%.1f, blend %d, "
               "z %d/%d, lit %d; corners over water %u (%u on the deep water's own; the floor %.2f to %.2f under, %.2f on average), over "
               "other floors %u (the first terrain %d, "
               "%.2f under %.1f %.1f %.1f), no floor %u; first corner %.1f %.1f %.1f\n",
            b->n / 3, count, nverts, whole ? ", a flat sheet taken whole" : flat ? ", a flat sheet not over water" : "", lo[0], hi[0], lo[2], hi[2],
            lo[1], hi[1], d->pipe.blend, d->depth.zenable, d->depth.zwrite, d->vs.lighting, tally[1], level, tally[1] ? dmin : 0.0f, tally[1] ? dmax : 0.0f,
            tally[1] ? dsum / (float)tally[1] : 0.0f, tally[2], other_t, other_dy, other_at[0], other_at[1], other_at[2], tally[0], first[0],
            first[1], first[2]);
    }
}

#include "d3d8_water_bake.h" /* the ground's draws looked at and found again (bake_find), bake_one_sheet, water_ground_under */

/* From water_draw, for a draw that's neither a tile, a water model's nor a sheet: a lit static chunk of the zone's ground
 * reaching the collision's water, looked at once at each place it's drawn (the cache by its vertices, indices and place), and
 * its water triangles marked for this frame's pass; an additive layer's, its pond's (bake_flood). Never changes the draw. */
static void water_ground(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t nverts, uint32_t up_data)
{
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    ++g_bake_calls;
    const int kind = g_wfloor.n ? bake_ground_draw(d, prim, count, nverts, up_data) : 0;
    if (!kind)
        return;
    float W[16];
    water_frames(W);
    if (g_water_folded < 0) /* the world can't be told this frame: looked at another time */
        return;
    g_bake_folded += g_water_folded != 0;
    const uintptr_t key = (uintptr_t)d->cpu[e->stream] ^ ((uintptr_t)d->indices << 1);
    WaterBake* b = bake_find(key, count, prim, &W[12]);
    if (b)
        ++g_bake_found, g_bake_hits += b->n != 0;
    if (!b)
    {
        float lo[3], hi[3];
        if (count < BAKE_LEAST)
            return;
        if (g_bake_n >= BAKE_ENTRIES)
        {
            bake_left(d, prim, count, nverts, up_data, "no entry left");
            return;
        }
        if (!water_extent(d, nverts, W, lo, hi))
        {
            bake_left(d, prim, count, nverts, up_data, "no extent");
            return;
        }
        if (kind == 1 && bake_moving(key, count, prim, &W[12], lo, hi)) /* (an additive layer's copies round a pond may lie close) */
            return;
        bake_index(g_bake_n, key, count, prim);
        b = &g_bake[g_bake_n++];
        b->key = key, b->count = count, b->prim = prim, b->at = 0, b->n = 0, b->seen = 0, b->gen = g_sea.gen, b->seed = kind == 2;
        b->wet = 0, b->moved = 0;
        memcpy(b->lo, lo, sizeof b->lo), memcpy(b->hi, hi, sizeof b->hi), memcpy(b->t, &W[12], sizeof b->t);
        if (bake_reaches(lo, hi))
            bake_build(b, d, prim, count, nverts, lo, hi);
        if (b->n)
            bake_one_sheet(b), bake_wet(b);
    }
    else if (!b->n && !b->seed && !b->moved && b->gen != g_sea.gen && g_sea.n >= SEA_LEAST)
    {
        /* the sea's level known since it was looked at: a flat chunk at that level past the collision's reach is the sea's now */
        b->gen = g_sea.gen;
        if (bake_reaches(b->lo, b->hi))
            bake_build(b, d, prim, count, nverts, b->lo, b->hi);
        if (b->n)
            bake_one_sheet(b), bake_wet(b);
    }
    if (b->n)
        b->seen = g_scene.frame;
}

/* One fixed-function draw in the world for the ground's water and the sea's horizon sheet (the identity rides in texture
 * matrix 3): position and the bed's depth (TEXCOORD0, read by the water's pixel function), fogged as the scene is, lit as the
 * world's last lit draw (water_shade borrows water_lights' sun, else the scene's ambient alone), its depth written, over the
 * scene's copies when the pass has them (else the debug view's cyan, or opaque). No rect (the game's layers are its own water's) */
static void water_ground_setup(GfxDraw* d, const GfxScene* s, int copies, GfxTex* colour_copy, GfxTex* depth_copy, uint32_t w, uint32_t h)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    memset(d, 0, sizeof *d);
    d->vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d->vs.el[GFX_R_TEXCOORD0] = (GfxElem){ 1, 0, GFX_FLOAT1, 0 }; /* the bed's depth, read by the water's pixel function */
    d->u.offset[GFX_R_TEXCOORD0] = 12;
    d->vs.ntex = 1;
    d->u.stride[0] = BAKE_STRIDE * 4;
    d->fs.nstages = 1;
    d->fs.st[0] = (GfxStage){ 2 /* SELECTARG1 */, 0 /* DIFFUSE */, 0, 1, 2, 0, 0, 1, 1, 0, 0, 2 };
    if (s->fog[2] != 0.0f)
        d->fs.fog = 3, d->u.params[2] = s->fog[0], d->u.params[3] = s->fog[1];
    memcpy(d->u.fogcolor, s->fogcolor, sizeof d->u.fogcolor);
    memcpy(d->u.ambient, s->ambient, sizeof d->u.ambient);
    for (int i = 0; i < 4; ++i)
        d->u.mat_d[i] = d->u.mat_a[i] = d->u.tfactor[i] = 1.0f;
    for (int i = 0; i < 4; ++i)
        d->u.vp[i] = (float)s->vp[i];
    memcpy(d->vp, s->vp, sizeof d->vp);
    float jit[2];
    if (gfx_dlss_jitter(jit))
        memcpy(&d->u.pad0[0], &jit[0], 4), memcpy(&d->u.pad0[1], &jit[1], 4);
    mat_mul(d->u.wvp, s->view, s->proj);
    memcpy(d->u.wv, s->view, 64);
    memcpy(d->u.wvit, I, 64);
    d->pipe.write_mask = 0xF;
    d->depth.zenable = 1, d->depth.zwrite = 1, d->depth.zfunc = 4; /* LESSEQUAL */
    /* blended by its alpha: the shader fades the ground's water out where its bed reaches the surface, so a chunk's water
     * triangle ending on the bank (West Ronfaure's: a pale triangle of foam) has no edge of its own */
    d->pipe.blend = 1, d->pipe.src = 5, d->pipe.dst = 6, d->pipe.op = 1;
    d->cull = 1; /* NONE: the chunks' winding is theirs */
    d->fill = 3;
    d->prim = GFX_TRIANGLELIST;
    water_shade(d, g_wfloor.lo, g_wfloor.hi, I, s->view, 0);
    if (g_water_set.view) /* the debug view: cyan */
        water_view(d, 0x00FFFFu);
    else if (copies)
        water_pass_draw(d, colour_copy, depth_copy, w, h, s), d->fs.water = 3;
}

/* The ground's water drawn as VanaCore's water, at the pass's end: the chunks drawn this frame, each its triangles from the
 * pool (one fixed-function draw in the world each: the identity rides in texture matrix 3), fogged as the scene is, lit as the
 * world's last lit draw (water_shade borrows water_lights' sun, else the scene's ambient alone), its depth written, over the
 * scene's copies when the pass has them. No rect (the game's layers over it are its own water's). */
static void water_ground_pass(const GfxScene* s, int copies, GfxTex* colour_copy, GfxTex* depth_copy, uint32_t w, uint32_t h)
{
    if (g_bake_dirty && g_bake_nv)
    {
        gfx_buf_destroy(g_bake_buf);
        g_bake_buf = gfx_buf_create(g_bake_nv * BAKE_STRIDE * 4);
        if (g_bake_buf)
            gfx_buf_upload(g_bake_buf, g_bake_v, g_bake_nv * BAKE_STRIDE * 4);
        g_bake_buf_n = g_bake_nv, g_bake_dirty = 0;
    }
    if (!g_bake_buf || !g_bake_buf_n)
        return;
    static GfxDraw d;
    water_ground_setup(&d, s, copies, colour_copy, depth_copy, w, h);
    water_feet_any(d.u.texm[5]); /* the waterfalls' feet (Yuhtunga Jungle's pool under its falls is the ground's water) */
    d.u.texm[5][2] = g_water_set.caustics, d.u.texm[5][3] = g_water_set.relief;
    uint32_t drawn = 0, tris = 0, stale = 0;
    const WaterBake* last_stale = NULL;
    for (uint32_t i = 0; i < g_bake_wet_n; ++i)
    {
        const WaterBake* b = &g_bake[g_bake_wet[i]];
        if (!b->n)
            continue;
        /* every chunk with water, whether or not the game drew it this frame: they don't move, there are a few a zone, and the
         * game draws West Ronfaure's stream sheet after the effects once the zone is in (seen by the ground path at zone-in
         * alone, so the pass that drew only what was seen drew nothing there); what's out of view the card drops */
        if (b->seen + 1 < g_scene.frame)
            ++stale, last_stale = b;
        if (b->at + b->n > g_bake_buf_n)
            continue;
        d.buf[0] = g_bake_buf, d.buf_off[0] = b->at * BAKE_STRIDE * 4, d.size[0] = b->n * BAKE_STRIDE * 4;
        d.count = b->n / 3;
        gfx_draw(&d);
        ++drawn, tris += d.count;
    }
    static uint64_t said_ns;
    const uint64_t now = gfx_now_ns();
    if (g_water_set.log && now - said_ns > 2000000000ull) /* every two seconds: what the pass drew */
    {
        said_ns = now;
        /* the water's light as the shader sees it: the view's up, the sun's direction (view space), their dot (the sun's share),
         * the sun's diffuse and the ambient */
        const float* V = s->view;
        const float upv[3] = { -V[4], -V[5], -V[6] }; /* the world's up (0, -1, 0) through the view's rotation */
        const float* sd = d.vs.nlights ? d.u.light[0].dir : NULL;
        const float dot = sd ? upv[0] * sd[0] + upv[1] * sd[1] + upv[2] * sd[2] : 0.0f;
        rt_log("[recomp] water: ground pass drew %u chunks (%u triangles), %u of them not seen drawn this frame, of %u; copies %d, view %d, "
               "frame %u; lights %d, up %.2f %.2f %.2f, sun %.2f %.2f %.2f (up.sun %.2f), diffuse %.2f %.2f %.2f, ambient %.2f %.2f %.2f, "
               "fog %.0f..%.0f; since the last line water_ground was called %u times, found an entry %u times (%u with water), %u folded; "
               "a stale chunk seen at frame %u, x %.0f..%.0f z %.0f..%.0f, %u triangles\n",
            drawn, tris, stale, g_bake_n, copies, g_water_set.view, g_scene.frame, d.vs.nlights, upv[0], upv[1], upv[2], sd ? sd[0] : 0.0f,
            sd ? sd[1] : 0.0f, sd ? sd[2] : 0.0f, dot, sd ? d.u.light[0].diffuse[0] : 0.0f, sd ? d.u.light[0].diffuse[1] : 0.0f,
            sd ? d.u.light[0].diffuse[2] : 0.0f, d.u.ambient[0], d.u.ambient[1], d.u.ambient[2], d.u.params[2], d.u.params[3], g_bake_calls,
            g_bake_found, g_bake_hits, g_bake_folded, last_stale ? last_stale->seen : 0, last_stale ? last_stale->lo[0] : 0.0f,
            last_stale ? last_stale->hi[0] : 0.0f, last_stale ? last_stale->lo[2] : 0.0f, last_stale ? last_stale->hi[2] : 0.0f,
            last_stale ? last_stale->n / 3 : 0);
        g_bake_calls = g_bake_found = g_bake_hits = g_bake_folded = 0;
    }
}

#include "d3d8_water_sea.h" /* the sea drawn on to the horizon (water_horizon_pass) */

/* The pass: the frame's kept draws into the scene's target (color, with its depth), each over the scene as it is - the game's
 * world, VanaCore's grass - from scene_finish once the world is done, before the scene effects. From the ones the pass can't
 * draw over the scene (gfx_scene_snapshot gives no copies), the look they'd have had in their place. Then the ground's baked
 * water (water_ground_pass). */
static void water_pass(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    const int floors = g_bake_nv && g_water_mode == 1 && g_water_set.detect == 2;
    float sea_y;
    const int horizon = g_water_mode == 1 && (g_water_set.horizon || (g_water_set.sea && g_sea.n >= SEA_LEAST)) && sea_level(&sea_y);
    g_scene.s.water_noshadow = 0;
    if ((!g_water_kept_n && !floors && !horizon) || !color)
        return;
    GfxTex *colour_copy = NULL, *depth_copy = NULL;
    uint32_t w = 0, h = 0;
    const int copies = gfx_scene_snapshot(color, &colour_copy, &depth_copy, &w, &h);
    /* the sun's shadows off the water (water.shadows 0): the depth copy is the scene before the water, which the effects tell it by */
    g_scene.s.water_noshadow = copies && !g_water_set.shadows ? 1u : 0u;
    if (copies) /* kept for a water draw that comes after the scene is done (water_keep) */
        g_water_late.c = colour_copy, g_water_late.d = depth_copy, g_water_late.w = w, g_water_late.h = h, g_water_late.frame = g_scene.frame;
    gfx_set_targets(color, 0, 0, depth);
    for (uint32_t i = 0; i < g_water_kept_n; ++i)
    {
        WaterKept* k = &g_water_kept[i];
        if (k->rt != color || k->as_is)
            continue;
        GfxDraw* d = &k->d;
        d->caster = 0, d->sought = 0, d->piece = 0, d->piece_draw = 0;
        if (copies)
            water_pass_draw(d, colour_copy, depth_copy, w, h, s);
        gfx_draw(d);
    }
    if (floors)
        water_ground_pass(s, copies, colour_copy, depth_copy, w, h);
    if (horizon) /* after the tiles and the ground's water, which it lies under */
        water_horizon_pass(s, copies, colour_copy, depth_copy, w, h);
    for (uint32_t i = 0; i < g_water_kept_n; ++i) /* the game's shimmer over the ground's water, as the game draws it */
    {
        WaterKept* k = &g_water_kept[i];
        if (k->rt != color || !k->as_is)
            continue;
        k->d.caster = 0, k->d.sought = 0, k->d.piece = 0, k->d.piece_draw = 0;
        gfx_draw(&k->d);
    }
    g_water_kept_n = 0;
}
