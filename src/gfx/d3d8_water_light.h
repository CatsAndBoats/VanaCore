/* d3d8_water_light.h: an include unit of d3d8_water.h (in its scope: GfxLight, GfxDraw, g_water_set, water_extent). The light the
 * game's layers made VanaCore's water are shaded by: the sun, the moon and the ambient (water_lights, water_sun, water_shade) */


/* The sun and the moon (the directional lights) and the ambient as the last lit draw of the world had them, for a layer of the
 * game's made VanaCore's water (water_shade): the game draws its layers unlit, and the water takes its light from the draw's.
 * Held for the zone (d3d8_set_water_floors lets them go), their directions in the world (g_water_sun_w) and turned into each
 * water draw's view: the lit draws come and go with what the camera sees (Caedarva Mire has its trees in every frame; where
 * none but the characters' are in view there are none), and held for 30 frames in the lit draw's view the water lost its sun
 * and glint once the camera turned from them, and the sun turned with the camera while it was held. The water's sun itself is
 * the shadows' now (water_sun): from these, its ambient, and its light until the sun is first picked */
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

/* The water's sun, on a draw the game drew unlit (water_shade): the shadows' (d3d8_sun_world), with the last lit draw's ambient
 * (water_lights); its lights before the first pick. FFXI lights each object with directional lights of its own (d3d8_sun.c): where
 * the walls are unlit (Lower Jeuno) the last lit draw was a prop by the camera, and its light, so the glint, came and went as the
 * camera moved (a player's video, 17:53, 2026-11-22) */
static void water_sun(GfxDraw* d, const float* V)
{
    float sun[3], col[4];
    const int pick = d3d8_sun_world(sun, col);
    if (!pick && !g_water_nsun)
        return;
    const uint32_t n = pick ? 1 : g_water_nsun;
    for (uint32_t i = 0; i < n; ++i)
    {
        d->u.light[i] = g_water_sun[i], d->vs.light_type[i] = 3;
        const float* w = pick ? sun : g_water_sun_w[i];
        for (int j = 0; j < 3; ++j) /* the world's direction into this draw's view (V) */
            d->u.light[i].dir[j] = w[0] * V[j] + w[1] * V[4 + j] + w[2] * V[8 + j];
        water_unit3(d->u.light[i].dir);
    }
    if (pick) /* the picked light's colour; the borrowed lights' ambient all on it, as the water had it from them */
        memcpy(d->u.light[0].diffuse, col, sizeof col), memcpy(d->u.light[0].specular, col, sizeof col);
    for (uint32_t i = 1; pick && i < g_water_nsun; ++i)
        for (int c = 0; c < 4; ++c)
            d->u.light[0].ambient[c] += g_water_sun[i].ambient[c];
    d->vs.nlights = (uint8_t)n;
    if (g_water_nsun)
        memcpy(d->u.ambient, g_water_ambient, sizeof g_water_ambient);
}

/* VanaCore's water on the draw (mode 1): gfx_hlsl_water.h in place of its stages, opaque and writing depth (a tile is
 * already; a layer of the game's is made so, and lit as the world is: water_lights); its world position and the time in
 * texture matrix 7, the settings after them, the picked colour and the pass's settings in matrix 6, the draw's world matrix in
 * matrix 3 and the view in matrix 4 (the functions take the world position, the up and the normal through them: a turned or
 * scaled model's water lies as a tile's). Its place in the world (lo, hi: its extent there) is kept for the frame
 * (g_water_rects). Drawn in its place, or kept for the pass (water_keep). */
static void water_shade(GfxDraw* d, const float lo[3], const float hi[3], const float* W, const float* V, int rect)
{
    if (!d->vs.lighting)
        water_sun(d, V);
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
    memset(d->u.texm[2], 0, 64); /* matrix 2: the mirror's and the rain's, set on the pass's draws (water_mirror_bind, rain_water_bind); none here */
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
