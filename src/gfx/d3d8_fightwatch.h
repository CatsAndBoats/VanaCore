/* d3d8.c include unit: the fight watch (vanacore_gfx_fight_watch, core fightwatch.cpp). While it's on, each frame's lighting
 * as the draws were given it: every distinct light the lit 3D draws used (as lit, after night_light and character_light),
 * the brightest a draw's light could add up to, the material's emissive and diffuse at their highest, and the world scene's
 * ambient, fog and sun. For the reports of the whole world washing white or pink in a fight against an Aern in Al'Taieu
 * (2026-11-22 Trence, 2026-11-26 Vitali): whether a light, the fog or the ambient the game set went out of range, or none
 * did and it's VanaCore's effects or the upscaler. Also the draw log for one frame on request (vanacore_gfx_drawlog_once),
 * whatever [graphics] drawlog says. Included after g_scene; fw_cap_once is declared before cap_present. */
#pragma once

#ifdef VANACORE
#include "vanacore_gfx.h"

static struct
{
    int on;
    VanaGfxFight now, last; /* this frame's, as it goes; the last whole frame's */
    int have;               /* last holds a frame */
    uint32_t frames;        /* Presents since it was turned on */
    char once[1024];        /* the draw log's path for the next frame alone (""), and whether the open g_cap is it */
    int once_open;
} g_fw;

static float fw_max3(const float* c)
{
    return fmaxf(c[0], fmaxf(c[1], c[2]));
}

/* A lit 3D draw, as it goes to the back end (after night_light and character_light) */
static void fw_draw(const GfxDraw* d)
{
    if (!g_fw.on || !d->vs.lighting || d->vs.rhw)
        return;
    VanaGfxFight* f = &g_fw.now;
    f->lit_draws++;
    float sum[3] = { d->u.ambient[0], d->u.ambient[1], d->u.ambient[2] };
    for (int i = 0; i < d->vs.nlights; ++i)
    {
        const GfxLight* l = &d->u.light[i];
        for (int c = 0; c < 3; ++c)
            sum[c] += l->ambient[c] + l->diffuse[c];
        const uint8_t type = d->vs.light_type[i];
        const float near_ = sqrtf(l->pos[0] * l->pos[0] + l->pos[1] * l->pos[1] + l->pos[2] * l->pos[2]);
        uint32_t k = 0;
        for (; k < f->nlights; ++k)
        {
            const VanaGfxFightLight* s = &f->light[k];
            if (s->type == type && fabsf(s->diffuse[0] - l->diffuse[0]) < 0.01f && fabsf(s->diffuse[1] - l->diffuse[1]) < 0.01f &&
                fabsf(s->diffuse[2] - l->diffuse[2]) < 0.01f && fabsf(s->range - l->pos[3]) < 0.01f &&
                fabsf(s->att[0] - l->att[0]) < 1e-4f && fabsf(s->att[1] - l->att[1]) < 1e-4f && fabsf(s->att[2] - l->att[2]) < 1e-4f)
                break;
        }
        if (k == f->nlights)
        {
            if (f->nlights == VANA_GFX_FIGHT_LIGHTS)
            {
                f->lights_dropped++;
                continue;
            }
            VanaGfxFightLight* n = &f->light[f->nlights++];
            n->type = type;
            memcpy(n->diffuse, l->diffuse, 12);
            memcpy(n->ambient, l->ambient, 12);
            n->range = l->pos[3];
            memcpy(n->att, l->att, 12);
            n->nearest = near_;
        }
        VanaGfxFightLight* s = &f->light[k];
        s->draws++;
        s->char_draws += d->is_char != 0;
        if (type != 3 && near_ < s->nearest)
            s->nearest = near_;
    }
    /* (NaN stays out of fmaxf, so a NaN light is counted apart) */
    const float top = fw_max3(sum);
    if (!(top == top) || !isfinite(top))
        f->nonfinite++;
    else if (top > f->lit_max)
        f->lit_max = top, f->lit_max_char = d->is_char != 0;
    f->emissive_max = fmaxf(f->emissive_max, fw_max3(d->u.mat_e));
    f->diffuse_max = fmaxf(f->diffuse_max, fw_max3(d->u.mat_d));
}

/* The world's scene, as scene_finish hands it to the effects */
static void fw_scene(const GfxScene* s)
{
    if (!g_fw.on)
        return;
    VanaGfxFight* f = &g_fw.now;
    memcpy(f->ambient, s->ambient, 12);
    memcpy(f->fog_color, s->fogcolor, 12);
    f->fog_start = s->fog[0], f->fog_end = s->fog[1], f->fog_on = s->fog[2] != 0.0f;
    memcpy(f->sun_color, s->sun_color, 12);
    f->sun_on = s->sun_dir[3] != 0.0f;
    f->scene = 1;
}

/* From cap_present, each Present: the frame's record kept; then the draw log for one frame, when asked. 1 when it handled
 * g_cap (cap_present leaves it alone). */
static int fw_cap_once(void)
{
    if (g_fw.on)
    {
        ++g_fw.frames;
        if (g_fw.now.scene)
        {
            g_fw.now.frame = g_fw.frames;
            g_fw.last = g_fw.now, g_fw.have = 1;
        }
        memset(&g_fw.now, 0, sizeof g_fw.now);
    }
    if (g_cap && g_fw.once_open)
    {
        fclose(g_cap), g_cap = NULL;
        g_fw.once_open = 0;
        return 1;
    }
    if (!g_fw.once[0] || g_cap)
        return 0;
    g_cap = fopen(g_fw.once, "w");
    g_fw.once[0] = 0;
    g_cap_n = 0;
    if (!g_cap)
        return 1;
    g_fw.once_open = 1;
    uint32_t w, h;
    d3d8_screen_size(&w, &h);
    fprintf(g_cap, "frame %u screen %ux%u backbuffer %ux%u image %08x-%08x (the fight watch's)\n", g_cap_frame, w, h,
        g_dev.pp[0], g_dev.pp[1], rt_image_lo, rt_image_hi);
    return 1;
}

void vanacore_gfx_fight_watch(int on)
{
    g_fw.on = on != 0;
    g_fw.have = 0, g_fw.frames = 0;
    memset(&g_fw.now, 0, sizeof g_fw.now);
}

int vanacore_gfx_fight_frame(VanaGfxFight* out)
{
    if (!g_fw.on || !g_fw.have)
        return 0;
    *out = g_fw.last;
    return 1;
}

void vanacore_gfx_drawlog_once(const char* path)
{
    if (path && *path)
        snprintf(g_fw.once, sizeof g_fw.once, "%s", path);
}
#else
static void fw_draw(const GfxDraw* d) { (void)d; }
static void fw_scene(const GfxScene* s) { (void)s; }
#endif
