/* d3d8_water_sea.h: an include unit of d3d8_water.h (in its scope: g_sea, g_wfloor, SEA_*, sea_level, water_affine_inv,
 * water_ground_setup, gfx_draw). The sea drawn on to the horizon (water_horizon_pass, from water_pass) */
#pragma once

#define SEA_FAR 50000.0f /* water.horizon's reach: the sheet's far edge under a pixel from the horizon (15 yalms up, 2160 high) */
/* The whole sea's mesh: levels of even square cells round the camera (a geometry clipmap), SEA_RING cells each way, the first
 * SEA_CELL across, each level's cells twice its finer one's, with that one's square cut out of it; each level held to the
 * world on its own cells' grid (twice its cells' size: so the finer level's square starts on one of its lines), so the
 * triangles don't slide as the camera moves. Each cell is four triangles to its middle, the coarser level's cells along the
 * finer square split at their sides' middles to meet that level's corners (no T joins). One quad 100,000 yalms across, clipped
 * to the view, gave its pixels near the camera a surface place and depth a few centimetres off that moved as the camera did:
 * in Bastok Markets' canal the shore's teal fade and foam rim came and went along the whole bank with every step, and the
 * plane's edge crept up the cliff's foot (2026-11-26); then a 24 x 24 grid in cells by the cube of the step from the camera,
 * a few cells 30 to 1,000 yalms across in view, in long slivers to one point under the camera */
#define SEA_CELL 2.0f /* the finest cells, round the camera (yalms) */
#define SEA_RING 16   /* each level's cells each way (a multiple of 4) */
#define SEA_LEVELS 16 /* the most levels: the last SEA_CELL * SEA_RING / 2 * 2^15 out, past SEA_FAR */
#define SEA_VERTS ((SEA_RING * SEA_RING * 4 + SEA_RING * 2) * 3 * SEA_LEVELS)

/* a cell's triangles to its middle (x, y, z, the bed's depth) from v[k], its place held in lo..hi; split: its sides cut at their
 * middles (1 its z0 side, 2 its x1, 4 its z1, 8 its x0). The next k */
static int sea_cell(float (*v)[4], int k, float x0, float z0, float s, int split, float y, const float lo[2], const float hi[2])
{
    const float p[5][2] = { { x0, z0 }, { x0 + s, z0 }, { x0 + s, z0 + s }, { x0, z0 + s }, { x0, z0 } };
    const float m[2] = { x0 + s * 0.5f, z0 + s * 0.5f };
    for (int e = 0; e < 4; ++e)
    {
        float r[3][2];
        int n = 0;
        r[n][0] = p[e][0], r[n][1] = p[e][1], ++n;
        if (split >> e & 1)
            r[n][0] = (p[e][0] + p[e + 1][0]) * 0.5f, r[n][1] = (p[e][1] + p[e + 1][1]) * 0.5f, ++n;
        r[n][0] = p[e + 1][0], r[n][1] = p[e + 1][1], ++n;
        for (int t = 0; t + 1 < n; ++t)
        {
            const float* c[3] = { m, r[t], r[t + 1] };
            for (int q = 0; q < 3; ++q, ++k)
                v[k][0] = fminf(fmaxf(c[q][0], lo[0]), hi[0]), v[k][1] = y, v[k][2] = fminf(fmaxf(c[q][1], lo[1]), hi[1]),
                v[k][3] = SEA_DEPTH;
        }
    }
    return k;
}

/* the whole sea's clipmap round cam (x, z) out over lo..hi in v: its vertex count */
static int sea_clipmap(float (*v)[4], const float cam[2], float y, const float lo[2], const float hi[2])
{
    int k = 0, hole[2] = { 0, 0 };
    float fine[2] = { 0.0f, 0.0f }; /* the finer level's first corner */
    for (int l = 0; l < SEA_LEVELS; ++l)
    {
        const float s = SEA_CELL * (float)(1 << l);
        float o[2];
        for (int a = 0; a < 2; ++a)
        {
            o[a] = floorf(cam[a] / (2.0f * s)) * (2.0f * s) - (float)(SEA_RING / 2) * s;
            hole[a] = l ? (int)lroundf((fine[a] - o[a]) / s) : 0;
        }
#define SEA_HOLE(i, j) (l && (i) >= hole[0] && (i) < hole[0] + SEA_RING / 2 && (j) >= hole[1] && (j) < hole[1] + SEA_RING / 2)
        for (int j = 0; j < SEA_RING; ++j)
            for (int i = 0; i < SEA_RING; ++i)
            {
                const float x0 = o[0] + (float)i * s, z0 = o[1] + (float)j * s;
                if (SEA_HOLE(i, j) || x0 >= hi[0] || z0 >= hi[1] || x0 + s <= lo[0] || z0 + s <= lo[1])
                    continue;
                const int split = (SEA_HOLE(i, j - 1) ? 1 : 0) | (SEA_HOLE(i + 1, j) ? 2 : 0) | (SEA_HOLE(i, j + 1) ? 4 : 0) |
                                  (SEA_HOLE(i - 1, j) ? 8 : 0);
                k = sea_cell(v, k, x0, z0, s, split, y, lo, hi);
            }
#undef SEA_HOLE
        fine[0] = o[0], fine[1] = o[1];
        const float span = (float)SEA_RING * s;
        if (o[0] <= lo[0] && o[1] <= lo[1] && o[0] + span >= hi[0] && o[1] + span >= hi[1])
            break;
    }
    return k;
}

/* The sea drawn on to the horizon (water.horizon): the game's sea stops at the edge of the zone's meshes, a hard line under a far
 * draw distance. A flat sheet at the sea's level (g_sea: the lowest tiles that reach the zone's rim), SEA_UNDER lower, from the
 * zone's floors' box pulled in SEA_IN (so it lies under the last tiles, which win the depth test) out past the fog, in four quads
 * round the box, drawn as the ground's water is with the bed SEA_DEPTH deep (the deep water's colour) and fogged as the scene,
 * so it goes into the fog as the land does. The zone's own ground, above the sea, hides it where the two meet; inside the box
 * nothing is drawn, so ground that lies below the sea's level there is never covered. Not with the camera under the sea */
static void water_horizon_pass(const GfxScene* s, int copies, GfxTex* colour_copy, GfxTex* depth_copy, uint32_t w, uint32_t h)
{
    float inv[16], level;
    if (!sea_level(&level) || !water_affine_inv(inv, s->view))
        return;
    const float cam[3] = { inv[12], inv[13], inv[14] };
    if (cam[1] > level) /* under the sea: the world's y points down */
        return;
    const int tiles = g_sea.n >= SEA_LEAST;
    /* water.sea with the game's sea tiles seen: one plane at their level over the whole zone and on to the horizon, the tiles
     * dropped (sea_own); its depth read from the scene where the bed is near (the shore's fade and foam), deep past it. In the
     * debug view too, in its cyan (water_ground_setup): left out, the view's sea stopped at the game's last tile (Norg's
     * harbour mouth) */
    const int full = tiles && g_water_set.sea && g_water_set.pass && copies;
    if (!full && !g_water_set.horizon)
        return; /* water.sea alone, with no copies to draw it by: the game's tiles stay (sea_own), nothing past them */
    const float y = level + (full ? 0.0f : tiles ? SEA_UNDER : -SEA_OVER);
    const float fog_end = s->fog[2] != 0.0f ? s->fog[1] : 0.0f;
    float reach = fog_end * 2.5f + 400.0f; /* well past the fog's end: the sheet has faded to nothing before its far edge */
    reach = reach < 2000.0f ? 2000.0f : reach > 20000.0f ? 20000.0f : reach;
    /* water.horizon: past the game's far plane too, which its draw distance sets (a ferry's Boat rides at 1x cut the sea about 330
     * yalms out, the game's painted sea past it), out to SEA_FAR; not cut there, its depth held at the far end (GfxDepthKey.noclip:
     * LESSEQUAL against the cleared depth). With the camera over a yalm above it: nothing of it nearer than the near plane */
    const int noclip = g_water_set.horizon && level - cam[1] > 1.0f;
    if (noclip)
        reach = SEA_FAR;
    float ilo[2] = { g_wfloor.zlo[0] + SEA_IN, g_wfloor.zlo[2] + SEA_IN }, ihi[2] = { g_wfloor.zhi[0] - SEA_IN, g_wfloor.zhi[2] - SEA_IN };
    if (g_water_ship) /* a ferry has no floors' box (sea_note): the sea round the camera (left as it was, the last zone's box) */
        ilo[0] = ihi[0] = cam[0], ilo[1] = ihi[1] = cam[2];
    for (int a = 0; a < 2; ++a)
        if (ilo[a] >= ihi[a])
            ilo[a] = ihi[a] = (ilo[a] + ihi[a]) * 0.5f;
    const float olo[2] = { fminf(cam[0] - reach, ilo[0] - 1.0f), fminf(cam[2] - reach, ilo[1] - 1.0f) };
    const float ohi[2] = { fmaxf(cam[0] + reach, ihi[0] + 1.0f), fmaxf(cam[2] + reach, ihi[1] + 1.0f) };
    /* two rings of four quads round the inner box: the first SEA_EDGE wide, its depth 0 at the inner edge (the water's shore fade:
     * nothing, then foam, then water) and SEA_DEPTH at the outer; the second out to the outer box, deep throughout. Each ring: two
     * quads the ring's full width, two between them (x0, z0, x1, z1, the depth at the inner edge, at the outer) */
    const float mlo[2] = { fmaxf(ilo[0] - SEA_EDGE, olo[0] + 1.0f), fmaxf(ilo[1] - SEA_EDGE, olo[1] + 1.0f) };
    const float mhi[2] = { fminf(ihi[0] + SEA_EDGE, ohi[0] - 1.0f), fminf(ihi[1] + SEA_EDGE, ohi[1] - 1.0f) };
    const float q[8][6] = { { mlo[0], mlo[1], mhi[0], ilo[1], 0.0f, SEA_DEPTH },  { mlo[0], ihi[1], mhi[0], mhi[1], 0.0f, SEA_DEPTH },
                            { mlo[0], ilo[1], ilo[0], ihi[1], 0.0f, SEA_DEPTH },  { ihi[0], ilo[1], mhi[0], ihi[1], 0.0f, SEA_DEPTH },
                            { olo[0], olo[1], ohi[0], mlo[1], SEA_DEPTH, SEA_DEPTH }, { olo[0], mhi[1], ohi[0], ohi[1], SEA_DEPTH, SEA_DEPTH },
                            { olo[0], mlo[1], mlo[0], mhi[1], SEA_DEPTH, SEA_DEPTH }, { mhi[0], mlo[1], ohi[0], mhi[1], SEA_DEPTH, SEA_DEPTH } };
    static float v[SEA_VERTS][4]; /* x, y, z, the bed's depth */
    int nv = 48;
    if (full) /* the camera to olo / ohi in the clipmap's cells, deep throughout */
    {
        const float c2[2] = { cam[0], cam[2] };
        nv = sea_clipmap(v, c2, y, olo, ohi);
    }
    for (int i = 0; !full && i < 8; ++i)
    {
        /* the quad's inner side: the one facing the box (its z1 for the north quads, z0 south, x1 west, x0 east) */
        const int side = i & 3; /* 0 north (z0 outer), 1 south (z1 outer), 2 west (x0 outer), 3 east (x1 outer) */
        const float c[6][2] = { { q[i][0], q[i][1] }, { q[i][2], q[i][1] }, { q[i][2], q[i][3] },
                                { q[i][0], q[i][1] }, { q[i][2], q[i][3] }, { q[i][0], q[i][3] } };
        for (int k = 0; k < 6; ++k)
        {
            const int outer = side == 0 ? c[k][1] == q[i][1] : side == 1 ? c[k][1] == q[i][3] : side == 2 ? c[k][0] == q[i][0] : c[k][0] == q[i][2];
            v[i * 6 + k][0] = c[k][0], v[i * 6 + k][1] = y, v[i * 6 + k][2] = c[k][1], v[i * 6 + k][3] = outer ? q[i][5] : q[i][4];
        }
    }
    static GfxDraw d;
    water_ground_setup(&d, s, copies, colour_copy, depth_copy, w, h);
    d.depth.noclip = (uint8_t)noclip;
    d.data[0] = v, d.size[0] = (uint32_t)nv * sizeof v[0];
    d.count = (uint32_t)nv / 3;
    /* The sheet is shaded as the sea's tiles it carries on from are (the water's own settings: its colour, brightness and reflection
     * as theirs; gfx_hlsl_water.h takes open water's body partly from the game's painted sea behind it and lets that through as the
     * fog would take it). Its own colour (green, a third brighter) and half reflection made it a second, lighter sea, with the
     * tiles a darker rectangle in it (Lower Jeuno's harbour, 2026-11-15). No fine ripples: thousands of yalms out they're far under a
     * pixel, and Direct3D 12's upscaler made moire of them (dark and light streaks through the sun's path on the far sea) */
    d.u.texm[6][15] = 0.0f;
    if (!full)
        d.u.texm[7][4] *= 0.5f; /* the waves at half (as before): its swells out to the horizon were bands */
    /* the horizon's sheet (gfx_hlsl_water.h: matrix 5's byte 1): its inner ring fades in, and no least share of the sky */
    d.u.texm[5][1] = full ? 2.0f : 1.0f; /* 2: the whole sea, faded at the shore by the scene's depth under it, not the rings' */
    /* 3: a ferry's whole sea, deep throughout and no shore foam: its one shore is the boat's hull, whose waterline foam was a
     * strip of noisy bubbles seen past the rail from the deck, a white outline round the rail and the rope (2026-11-21) */
    if (full && g_water_ship)
        d.u.texm[5][1] = 3.0f;
    /* the whole sea's waves are the FFT ocean's (d3d8_water_ocean.h; GfxFsKey.water 4): its two cascades in stages 4 and 5,
     * wrapping, filtered through their mips (anisotropic: the sea is seen at grazing angles) */
    if (full && d.fs.water == 3 && ocean_update())
    {
        d.fs.water = 4;
        for (int c = 0; c < 2; ++c)
        {
            d.tex[4 + c] = g_ocean.tex[c];
            d.fs.st[4 + c].tex = 1, d.fs.st[4 + c].ncoord = 2;
            d.samp[4 + c] = (GfxSampler){ 1, 1, 1, 2, 3, 2, 8, 0, 0, 0, { 0, 0 }, 0 }; /* WRAP, LINEAR, ANISOTROPIC x8, mips */
        }
    }
    d.pipe.blend = 1, d.pipe.src = 5, d.pipe.dst = 6, d.pipe.op = 1; /* SRCALPHA, INVSRCALPHA, ADD */
    water_tris_draw(&d);
    if (full)
        g_sea.full = g_scene.frame;
    if (g_water_set.log && !g_sea.said)
    {
        g_sea.said = 1;
        rt_log("[recomp] water: the sea at height %.1f (%s) drawn on to the horizon, %.0f out, from the floors' box x %.0f..%.0f z %.0f..%.0f\n",
            level, g_sea.n >= SEA_LEAST ? "the tiles' or chunks' level at the zone's rim" : "the shore's level: the water floors at the rim", reach,
            ilo[0], ihi[0], ilo[1], ihi[1]);
    }
}
