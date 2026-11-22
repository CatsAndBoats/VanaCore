/* The tufts lit by the slope of the ground under them, as the game lights that ground (with grass.match). The game's lights are
 * noted from its lit ground draws (slope_note, from ground_light_note: the ambient, and its directional lights' - the sun's, the
 * moon's - directions in the camera's space and colours), and each frame the tufts' vertex function is given them in the world's
 * (slope_light: c18-c22), where a tuft takes the light on its ground's normal (PVert.keep's blue and alpha, from GWTri.lnx / lnz)
 * against flat ground's, its colour times (1 - match) + match times that. A tuft on flat ground is lit as before; one on a slope
 * turned to a low sun brighter, turned from it darker, through the day as the ground under it is: the grass on La Theine's cliff
 * feet stayed flat ground's colour at dawn and dusk while the slope under it went bright or dark (2026-11-22). grass.c includes this
 * before ground_light_note, after g, GTri, GWTri, GfxDraw, GfxScene, maxf and clampf, whose names it uses. It is no file of its own. */
#pragma once

static struct
{
    float amb[3];    /* the ambient: the draw's and each light's own */
    float dir[2][3]; /* the directional lights' directions, camera space, as the game's lighting takes them (n . dir: ground_light_note) */
    float dif[2][3]; /* ... and their diffuse colours */
    int n;           /* how many of them (0 to 2); -1 none noted yet in this game */
} g_slope = { { 0 }, { { 0 } }, { { 0 } }, -1 };

/* a world triangle's normal as the game lights it, looking up (y is down), into its lnx / lnz (0 to 255 for -1 to 1): its corners'
 * normals' (src->n, by the instance's W) when it has them, else its face's (fx, fy, fz) */
static void slope_normal(GWTri* w, const GTri* src, const float W[16], float fx, float fy, float fz)
{
    float n[3] = { fx, fy, fz };
    if (src->hasn)
    {
        const float wx = src->n[0] * W[0] + src->n[1] * W[4] + src->n[2] * W[8];
        const float wy = src->n[0] * W[1] + src->n[1] * W[5] + src->n[2] * W[9];
        const float wz = src->n[0] * W[2] + src->n[1] * W[6] + src->n[2] * W[10];
        const float wl = sqrtf(wx * wx + wy * wy + wz * wz);
        if (wl > 1e-6f)
            n[0] = wx / wl, n[1] = wy / wl, n[2] = wz / wl;
    }
    const float up = n[1] > 0.0f ? -1.0f : 1.0f;
    w->lnx = (uint8_t)(clampf(up * n[0], -1.0f, 1.0f) * 127.5f + 128.0f);
    w->lnz = (uint8_t)(clampf(up * n[2], -1.0f, 1.0f) * 127.5f + 128.0f);
}

/* a lit ground draw's lights (ground_light_note's, once its light is a number): the scene's latest kept */
static void slope_note(const GfxDraw* d)
{
    float amb[3] = { d->u.ambient[0], d->u.ambient[1], d->u.ambient[2] };
    int n = 0;
    for (int i = 0; i < d->vs.nlights && i < GFX_NLIGHTS; ++i)
    {
        if (d->vs.light_type[i] != 3)
            continue;
        const GfxLight* li = &d->u.light[i];
        for (int k = 0; k < 3; ++k)
            amb[k] += li->ambient[k];
        /* a NaN direction lights nothing on the GPU (max(0, n . l)), as ground_light_note takes it: none here either */
        if (n < 2 && isfinite(li->dir[0]) && isfinite(li->dir[1]) && isfinite(li->dir[2]))
        {
            memcpy(g_slope.dir[n], li->dir, sizeof g_slope.dir[n]), memcpy(g_slope.dif[n], li->diffuse, sizeof g_slope.dif[n]);
            ++n;
        }
    }
    for (int k = n; k < 2; ++k)
        memset(g_slope.dir[k], 0, sizeof g_slope.dir[k]), memset(g_slope.dif[k], 0, sizeof g_slope.dif[k]);
    memcpy(g_slope.amb, amb, sizeof amb), g_slope.n = n;
}

/* a kind's slope light into c (its vertex constants; c18-c22): the lights' directions in the world (by s's view: a world axis is
 * a row of it), and with the kind's match m (kind_light's) and flat ground's light F by them (its normal up, -y), c20 = 1 - m + m A
 * / F, c21 = m D1 / F, c22 = m D2 / F; with no match (a flower, grass.match 0, the ground's light not known or no light noted) c20
 * 1 and the rest 0: the tufts lit by kind_light's c17 alone */
static void slope_light(int flower, const GfxScene* s, float (*c)[4])
{
    const float m = g.match > 0.0f && g.ground_known && !flower && g_slope.n >= 0 ? g.match : 0.0f;
    float dw[2][3], flat[3];
    for (int l = 0; l < 2; ++l)
        for (int k = 0; k < 3; ++k)
        {
            const float* row = s->view + 4 * k;
            const float len = sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
            dw[l][k] = len > 1e-12f ? (row[0] * g_slope.dir[l][0] + row[1] * g_slope.dir[l][1] + row[2] * g_slope.dir[l][2]) / len : 0.0f;
        }
    for (int k = 0; k < 3; ++k)
        flat[k] = g_slope.amb[k] + g_slope.dif[0][k] * maxf(-dw[0][1], 0.0f) + g_slope.dif[1][k] * maxf(-dw[1][1], 0.0f);
    for (int k = 0; k < 3; ++k)
    {
        const float f = m > 0.0f && isfinite(flat[k]) && flat[k] > 0.02f ? m / flat[k] : 0.0f;
        c[18][k] = dw[0][k], c[19][k] = dw[1][k];
        c[20][k] = f > 0.0f ? 1.0f - m + f * g_slope.amb[k] : 1.0f;
        c[21][k] = f * g_slope.dif[0][k], c[22][k] = f * g_slope.dif[1][k];
    }
    c[18][3] = c[19][3] = c[20][3] = c[21][3] = c[22][3] = 0.0f;
}
