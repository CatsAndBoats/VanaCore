/* The blended ground (grass.blended): layer_scale, layer_log, layer_new, layer_seen and layer_keeps, how much of a blended layer
 * laid over other ground shows, so its tufts are as many. grass.c includes this just after ground_light_note, after g, GMesh,
 * GWTri and rnd01, whose names it uses. It is no file of its own. */
#pragma once

/* A blended ground draw (grass.blended) is a layer laid over other ground, shown as much as its alpha says: the scale stage 0's alpha
 * op puts on the vertices' alpha (1, 2 or 4), so its tufts are as many as it shows of itself; 0 when its alpha isn't the vertices'
 * (a pixel shader's, the texture's alone): it grows all over, as opaque ground does */
static uint8_t layer_scale(const GfxDraw* d)
{
    if (d->fs.prog || !d->fs.nstages)
        return 0;
    const GfxStage* st = &d->fs.st[0];
    const int a1 = (st->aa1 & 0x3F) <= 1, a2 = (st->aa2 & 0x3F) <= 1; /* D3DTA_DIFFUSE, or CURRENT (stage 0's is the diffuse) */
    switch (st->aop)
    {
    case 2: /* SELECTARG1 */
        return a1 ? 1 : 0;
    case 3: /* SELECTARG2 */
        return a2 ? 1 : 0;
    case 4: /* MODULATE */
        return a1 || a2 ? 1 : 0;
    case 5: /* MODULATE2X */
        return a1 || a2 ? 2 : 0;
    case 6: /* MODULATE4X */
        return a1 || a2 ? 4 : 0;
    }
    return 0;
}

/* with grass.log, a blended ground mesh as first read: how the game draws it and its vertices' alpha, which says how much of it shows */
static void layer_log(const GfxDraw* d, const GMesh* m)
{
    uint32_t lo = 255, hi = 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < m->n; ++i)
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t a = g.tris[m->first + i].c[k] >> 24;
            lo = a < lo ? a : lo, hi = a > hi ? a : hi, sum += a;
        }
    const GfxStage* st = &d->fs.st[0];
    rt_log("[recomp] grass: a blended ground mesh, %u triangles (texture %016llx): blend %u, %u, depth written %u, alpha test %u; "
           "stage 0 alpha op %u (%u, %u)%s; its vertices' alpha %u to %u, %.0f on average; its tufts %s\n",
        m->n, (unsigned long long)g.tex[m->tex].hash, d->pipe.src, d->pipe.dst, d->depth.zwrite, d->fs.alpha_func, st->aop, st->aa1,
        st->aa2, d->fs.prog ? ", a pixel shader" : "", lo, hi, (double)sum / (3.0 * m->n),
        m->layer == 4 ? "by its alpha, times 4" : m->layer == 2 ? "by its alpha, times 2" : m->layer ? "by its alpha" : "all over it");
}

/* a ground mesh as first read (note_one, after capture_mesh): drawn blended or not, and with grass.log how a blended one is drawn */
static void layer_new(const GfxDraw* d, GMesh* m)
{
    m->blended = d->pipe.blend != 0, m->layer = m->blended ? layer_scale(d) : 0;
    if (g.log && m->blended && m->n)
        layer_log(d, m);
}

/* a ground mesh drawn again (note_one): drawn opaque too (a blended pass over its own opaque one), it's ground whole */
static void layer_seen(const GfxDraw* d, GMesh* m)
{
    if (!d->pipe.blend)
        m->blended = 0, m->layer = 0;
}

/* a tuft at b (its spot's share of each corner) on triangle tri of a blended layer whose vertices' alpha is scaled by layer
 * (GMesh.layer; 0 not a layer: every one kept): kept as much as its spot's alpha shows of the layer, none where it fades to the
 * ground under it (build_step) */
static int layer_keeps(const GWTri* tri, const float b[3], float layer, uint32_t sd)
{
    if (layer <= 0.0f)
        return 1;
    const float shown = (b[0] * (float)(tri->c[0] >> 24) + b[1] * (float)(tri->c[1] >> 24) + b[2] * (float)(tri->c[2] >> 24)) * layer;
    return rnd01(sd + 8u) * 255.0f < shown;
}
