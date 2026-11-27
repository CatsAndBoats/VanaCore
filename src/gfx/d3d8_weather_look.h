/* A weather's look borrowed from another zone's files (d3d8_set_weather_look, from core/weatherlook.cpp): for a weather the
 * zone the player is in has no environment of its own for (a Weather preview, or a server's weather its files lack), the game
 * shows another weather's sky and fog, or a clear sky. Here the donor's is laid over it as the game would fade into it, over
 * some 2.5 s: the fog's colour the donor's, its start and end the zone's own times the donor's change from its clear weather;
 * the ambient light and the sun's or moon's times the same kind of change (the terrain's for the world, the model light's for
 * the characters); the dome's colours the donor's slices (sky.c sky_set_look, which takes the dome while they show).
 * d3d8.c includes this after d3d8_weather.h: look_fog before scene_note (the scene's fog colour is the overridden one), look_light
 * once the draw is known to be a character's. It is no file of its own. */
#pragma once

static WeatherLook g_look;    /* the last look given while one was wanted (kept as it fades out) */
static int g_look_on;         /* wanted this frame */
static float g_look_k;        /* how far it has faded in, 0 to 1 */
static uint64_t g_look_ns;
static uint32_t g_look_frame = ~0u;

void d3d8_set_weather_look(const WeatherLook* look)
{
    if (look->active)
        g_look = *look;
    g_look_on = look->active;
}

/* once a frame: the fade moved on, the dome told */
static void look_tick(void)
{
    if (g_look_frame == g_scene.frame)
        return;
    g_look_frame = g_scene.frame;
    const uint64_t now = gfx_now_ns();
    float dt = g_look_ns ? (float)((double)(now - g_look_ns) * 1e-9) : 0.0f;
    g_look_ns = now;
    if (dt > 0.25f)
        dt = 0.25f;
    g_look_k += (g_look_on ? dt : -dt) / 2.5f;
    g_look_k = g_look_k < 0.0f ? 0.0f : g_look_k > 1.0f ? 1.0f : g_look_k;
    sky_set_look(g_look.slice, g_look.elev, g_look.sky ? g_look_k : 0.0f);
}

static void look_fog(GfxDraw* d)
{
    look_tick();
    if (!(g_look_k > 0.0f) || d->vs.rhw)
        return;
    const float k = g_look_k, r = 1.0f + (g_look.fog_ratio - 1.0f) * k;
    for (int i = 0; i < 3; ++i)
        d->u.fogcolor[i] += (g_look.fog[i] - d->u.fogcolor[i]) * k;
    d->u.params[2] *= r, d->u.params[3] *= r;
}

static void look_light(GfxDraw* d)
{
    if (!(g_look_k > 0.0f) || d->vs.rhw)
        return;
    const float k = g_look_k;
    const float* amb = d->is_char ? g_look.amb_m : g_look.amb_t;
    const float* sun = d->is_char ? g_look.sun_m : g_look.sun_t;
    for (int i = 0; i < 3; ++i)
        d->u.ambient[i] *= 1.0f + (amb[i] - 1.0f) * k;
    for (int n = 0; n < d->vs.nlights; ++n)
        if (d->vs.light_type[n] == 3) /* the sun or the moon: directional */
            for (int i = 0; i < 3; ++i)
                d->u.light[n].diffuse[i] *= 1.0f + (sun[i] - 1.0f) * k;
}
