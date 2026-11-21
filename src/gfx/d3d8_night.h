/* The world's light through the Vana'diel night ([graphics.effects] night), brighter (night_bright, indoor_bright: d3d8_brighten)
 * and its least ambient (ambient_force): night_light, on each lit draw of the world's scene.
 * d3d8.c includes this just after character_light, after g_gfx_night, d3d8_night, obj, g_dev, g_scene and surface_gpu, whose
 * names it uses. It is no file of its own. */
#pragma once

/* [graphics.effects] night (g_gfx_night): the world's light through the Vana'diel night, outdoors (d3d8_night: 0 by day, 1 at
 * night). The game lights its night mostly by the ambient - Batallia Downs at 02:00: the moon 0.18 and the ambient 0.24, against
 * the day's sun 1.06 and ambient 0.38 - so a zone at night looked flat and lit. The ambient (the draw's and each light's) is
 * taken down to 1 - 0.75 night and the sun's and moon's light (the directional lights) to 1 - 0.35 night: the slopes the moon
 * lights stand out from those it doesn't. The lamps' and torches' own lights (point and spot) are left, their pools as bright,
 * and what the game draws unlit (flames, windows, effects, the sky, the moon) is as it was. A lit draw into the world's scene
 * only (not a menu's 3D preview), or VanaCore's water with the world's lights (water_shade). After scene_note: the effects'
 * ambient (the shadows' share of the light, sun_direct) stays the game's. The moonlight's colour is laid last (night_moon).
 * The other way, brighter (d3d8_sun.c d3d8_brighten): night_bright the moon's light raised as strong as a sun's (its General
 * lighting, night_bright_ambient, the ambient), day_bright the sun's by day, indoor_bright the ambient and the zone's fixed light
 * indoors; the lamps' lights left. With night on as well, both apply. ambient_force (g_gfx_ambient_force, 0 to 1) last: the draw's ambient raised, each
 * channel by the same amount, until its brightest is at least that (its tint kept), by day and night, indoors and out -
 * Windower's game_forceambientlight, Ashita's /ambient, which put a fixed ambient on everything the game lights, so dark places
 * show; at 1 the vertex colours are lit full */
static void night_light(GfxDraw* d)
{
    const float n = g_gfx_night * d3d8_night(), least = g_gfx_ambient_force;
    float kd, ka;
    d3d8_brighten(&kd, &ka);
    if (!(n > 0.0f || kd != 1.0f || ka != 1.0f || least > 0.0f) || d->vs.rhw || !(d->vs.lighting || d->vs.nlights))
        return;
    uint32_t face, level;
    const Obj* rt = obj(g_dev.rt);
    if (!rt || !g_scene.rt || surface_gpu(rt, &face, &level) != g_scene.rt)
        return;
    ka *= 1.0f - 0.75f * n, kd *= 1.0f - 0.35f * n;
    for (int c = 0; c < 3; ++c)
        d->u.ambient[c] *= ka;
    for (int i = 0; i < d->vs.nlights; ++i)
    {
        const float k = d->vs.light_type[i] == 3 ? kd : 1.0f;
        for (int c = 0; c < 3; ++c)
            d->u.light[i].ambient[c] *= ka, d->u.light[i].diffuse[c] *= k, d->u.light[i].specular[c] *= k;
    }
    const float top = fmaxf(d->u.ambient[0], fmaxf(d->u.ambient[1], d->u.ambient[2]));
    if (top < least)
        for (int c = 0; c < 3; ++c)
            d->u.ambient[c] += least - top;
}
