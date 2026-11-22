/* The rings: the share of the tufts kept by their distance from the player (set_rings, ring_share, ring_level) and the most
 * tufts at once that follows from them (kept_area, tuft_limit, kept_density). grass.c includes this just before
 * grass_configure, after g, GrassSettings, clampf, minf and maxf, whose names it uses. It is no file of its own. */
#pragma once

/* the rings: in order outward, each keeping no more than the one inside it (a square is made with the share its nearest point keeps,
 * enough for all of it only when the share falls outward); each step a ramp a tenth of the radius wide, narrower where the rings are
 * closer than that */
static void set_rings(const GrassSettings* in)
{
    static const float start0[3] = { 0.25f, 0.50f, 0.75f }, amount0[3] = { 0.80f, 0.50f, 0.30f };
    float start[3], keep[4] = { 1.0f };
    for (int i = 0; i < 3; ++i)
    {
        start[i] = clampf(in->rings_set ? in->ring[i] : start0[i], i ? start[i - 1] : 0.0f, 1.0f);
        keep[i + 1] = clampf(in->rings_set ? in->ring_amount[i] : amount0[i], 0.0f, keep[i]);
    }
    for (int i = 0; i < 3; ++i)
    {
        const float before = i ? start[i] - start[i - 1] : start[i], after = (i < 2 ? start[i + 1] : 1.0f) - start[i];
        const float half = minf(0.05f, 0.5f * minf(before, after));
        g.ring_from[i] = start[i] - half, g.ring_to[i] = start[i] + maxf(half, 1e-4f);
        g.ring_step[i] = keep[i] - keep[i + 1];
        g.ring_keep[i + 1] = keep[i + 1];
        g.ring_at[i] = start[i];
    }
    g.ring_keep[0] = 1.0f;
    g.show_rings = in->show_rings;
}

/* The rings (grass_configure, from grass.ring1-3 and .amount): the share of the tufts kept by their distance over the radius - all of
 * them near the player, then less ring by ring (80%, 50% and 30% unless set) - each step a short ramp, so the range can reach farther
 * for the same tufts; past KEEP_EDGE of the radius the last ring thins to none at it (and the tufts shrink from 0.8 of it). A tuft's
 * keep draw is its own, so the 30% kept far off are among the 50% and the 80% nearer in: walking toward a field adds tufts, it never
 * swaps them. */
#define KEEP_EDGE 0.85f

/* the share kept dd from the player (the vertex function's, on the CPU) */
static float ring_share(float dd)
{
    const float R = g.radius, at = dd / R;
    float f = 1.0f;
    for (int i = 0; i < 3; ++i)
        f -= g.ring_step[i] * clampf((at - g.ring_from[i]) / (g.ring_to[i] - g.ring_from[i]), 0.0f, 1.0f);
    return f * clampf((R - dd) / ((1.0f - KEEP_EDGE) * R), 0.0f, 1.0f);
}

/* the share a square is made with, whose nearest point is dd from the player: the least of the rings' that is enough there (a square
 * nearer than that holds tufts it can't show yet, never too few) */
static float ring_level(float dd)
{
    const float f = ring_share(dd);
    for (int i = 3; i > 0; --i)
        if (f <= g.ring_keep[i])
            return g.ring_keep[i];
    return 1.0f;
}

/* the share of the limit the rings' ground is given (the squares hold more than they show: their rings' shares rounded up, the edge's
 * thinning) */
#define KEPT_SHOWN 0.7f

/* the ground the rings keep, in square yalms: a disc of the radius weighed by the share each ring keeps */
static float kept_area(void)
{
    const float R = g.radius;
    float area = 0.0f;
    for (int i = 0; i < 64; ++i)
    {
        const float r = (i + 0.5f) * R / 64.0f;
        area += ring_share(r) * 6.2831853f * r * (R / 64.0f);
    }
    return maxf(area, 1.0f);
}

/* the most tufts at once: as many as the density over the rings' ground needs, so the density is what's drawn at any range;
 * `asked` (grass.tufts, in tufts; 0 none) a ceiling on it, and a million the most */
static uint32_t tuft_limit(unsigned asked)
{
    const float need = g.density * kept_area() / KEPT_SHOWN;
    const uint32_t most = asked != 0 && asked < 1000000 ? asked : 1000000;
    const uint32_t limit = need >= (float)most ? most : (uint32_t)need + 1;
    return limit < 1000 ? 1000 : limit;
}

/* tufts to a square yalm: grass.density, but not more than the limit over the ground the rings keep: past the ceiling (grass.tufts,
 * a million) a wide range thins them evenly rather than leaving the far side bare */
static float kept_density(void)
{
    return minf(g.density, (float)g.limit * KEPT_SHOWN / kept_area());
}
