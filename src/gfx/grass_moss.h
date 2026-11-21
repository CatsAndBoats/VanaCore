/* The moss (grass.moss): low, rounded cushions on the ground the zone's collision calls shallow water (GRASS_WET), where no tuft grows
 * (grass.nowater): Dragon's Aery's meadow under its waterfalls, a green grass texture with no water drawn over it. No picture of moss
 * ships with the game in a form the tufts can use, so it's painted here, in grey, like the drawn blades: keep_moss colours it the
 * ground's (tuft_colour), lifted by its mean shade (g.moss_avg) with grass.match.
 *
 * Its picture (paint_moss) is four MOSS_CELL squares side by side: two cushions seen from the side (a few domes run together, their
 * tops bristling with the moss's shoots, darker toward the ground) and two seen from above (a rounded clump of small domes, the
 * hollows between them darker). A cushion is three quads: two crossed upright ones of a side view and one lying over them, at a
 * little under half their height, of a top view: from the game's low camera the side shows, from above the top, and neither sways.
 * moss_spot puts three on a tuft's place.
 *
 * grass.c includes this after grass_keep.h (g, kept_room, kvert, lean, pack_color, rnd01, hash32, vnoise, clampf, minf, maxf,
 * gfx_tex_create and the MOSS kind are its names). It is no file of its own. */
#pragma once

#define MOSS_CELL 128
#define MOSS_W (MOSS_CELL * 4)
#define MOSS_H MOSS_CELL
#define MOSS_REF (100.0f / 255.0f) /* kept_draw's alpha test, as the blades' */

/* fine noise for the moss's pictures: vnoise moved to its own corner of the lattice by seed */
static float moss_noise(float x, float y, uint32_t seed)
{
    return vnoise(x + (float)(seed % 997u) * 13.0f, y + (float)(seed % 991u) * 17.0f);
}

/* side view v (0 or 1) at (X across, Y up; 0 to 1 of the cell): its coverage of the point (1 in, 0 out) and its shade there */
static int moss_side(int v, float X, float Y, float* shade, float* hue)
{
    const uint32_t seed = 4049u * (uint32_t)(v + 1);
    float top = 0.0f;
    const int n = 7 + 2 * v;
    for (int i = 0; i < n; ++i) /* the domes run together: the highest of them over X, lower toward the ends */
    {
        const uint32_t r = seed + (uint32_t)i * 97u;
        const float rad = 0.11f + 0.13f * rnd01(r + 1u), c = 0.03f + rad + (0.94f - 2.0f * rad) * rnd01(r + 2u);
        const float h = (0.55f + 0.4f * rnd01(r + 3u)) * sqrtf(maxf(0.0f, 1.0f - 3.2f * (c - 0.5f) * (c - 0.5f)));
        const float t = (X - c) / rad;
        if (t > -1.0f && t < 1.0f)
            top = maxf(top, h * sqrtf(1.0f - t * t));
    }
    /* ... within the cushion's own dome across the cell, so its ends meet the ground under the top's rim, not out past it */
    top = minf(top, 1.05f * sqrtf(maxf(0.0f, 1.0f - (2.0f * X - 1.0f) * (2.0f * X - 1.0f))));
    if (top <= 0.0f)
        return 0;
    /* the shoots on top: a ragged edge of tiny tips, none standing alone where the cushion meets the ground */
    const float tips = clampf(top / 0.25f, 0.0f, 1.0f);
    const float edge = top + tips * (0.045f * (moss_noise(X * 46.0f, 0.5f, seed) - 0.55f) + 0.03f * (moss_noise(X * 120.0f, 3.5f, seed + 7u) - 0.5f));
    if (Y >= edge)
        return 0;
    const float depth = clampf(Y / maxf(top, 0.05f), 0.0f, 1.0f); /* 0 at the ground, 1 at its top */
    const float leaf = moss_noise(X * 40.0f, Y * 40.0f, seed + 11u), fine = moss_noise(X * 95.0f, Y * 95.0f, seed + 13u);
    *shade = (0.6f + 0.4f * powf(depth, 0.8f)) * (0.78f + 0.34f * leaf) * (0.9f + 0.2f * fine) * (edge - Y < 0.05f ? 1.08f : 1.0f);
    *hue = 0.8f * (depth - 0.55f) + 0.6f * (leaf - 0.5f);
    return 1;
}

/* top view v (0 or 1) at (X, Y, 0 to 1 of the cell): coverage and shade, as moss_side */
static int moss_top(int v, float X, float Y, float* shade, float* hue)
{
    const uint32_t seed = 6247u * (uint32_t)(v + 1);
    const float dx = X - 0.5f, dy = Y - 0.5f, r = sqrtf(dx * dx + dy * dy);
    const float ca = r > 1e-5f ? dx / r : 1.0f, sa = r > 1e-5f ? dy / r : 0.0f;
    const float a = atan2f(dy, dx), p1 = 6.2831853f * rnd01(seed + 1u), p2 = 6.2831853f * rnd01(seed + 2u);
    /* its outline: round, lobed, the shoots' tips ragged round it (noise round the circle, so it meets itself) */
    const float R = 0.4f * (1.0f + 0.1f * sinf(3.0f * a + p1) + 0.07f * sinf(5.0f * a + p2)) + 0.035f * (moss_noise(ca * 7.0f + 20.0f, sa * 7.0f + 20.0f, seed) - 0.5f) +
        0.02f * (moss_noise(ca * 19.0f + 40.0f, sa * 19.0f + 40.0f, seed + 3u) - 0.5f);
    if (r >= R)
        return 0;
    /* its height: the clump's dome and the small domes on it, the highest; the hollows between them lower */
    float z = 0.5f * sqrtf(maxf(0.0f, 1.0f - (r / R) * (r / R)));
    for (int i = 0; i < 12; ++i)
    {
        const uint32_t q = seed + 31u + (uint32_t)i * 53u;
        const float br = 0.08f + 0.08f * rnd01(q), ang = 6.2831853f * rnd01(q + 1u), off = 0.29f * sqrtf(rnd01(q + 2u));
        const float bx = dx - cosf(ang) * off, by = dy - sinf(ang) * off, bd = sqrtf(bx * bx + by * by) / br;
        if (bd < 1.0f)
            z = maxf(z, (0.7f + 0.3f * (1.0f - off / 0.29f)) * sqrtf(1.0f - bd * bd));
    }
    const float leaf = moss_noise(X * 44.0f, Y * 44.0f, seed + 11u), fine = moss_noise(X * 100.0f, Y * 100.0f, seed + 13u);
    *shade = (0.45f + 0.55f * z) * (0.78f + 0.34f * leaf) * (0.9f + 0.2f * fine) * (R - r < 0.025f ? 0.92f : 1.0f);
    *hue = 0.8f * (z - 0.6f) + 0.6f * (leaf - 0.5f);
    return 1;
}

/* the moss's picture, MOSS_W x MOSS_H premultiplied rgba (lv), painted 4 x 4 samples a texel; its mean shade where the alpha test
 * keeps it, the side views' by the shade their quads give each row (blade_quad_shade), in *avg */
static void paint_moss(float* lv, float* avg)
{
    double sum = 0.0, n = 0.0;
    for (int y = 0; y < MOSS_H; ++y)
        for (int x = 0; x < MOSS_W; ++x)
        {
            const int cell = x / MOSS_CELL;
            float acc[3] = { 0, 0, 0 }, in = 0.0f;
            for (int s = 0; s < 16; ++s)
            {
                const float X = ((float)(x % MOSS_CELL) + ((float)(s % 4) + 0.5f) * 0.25f) / MOSS_CELL;
                const float Y = ((float)y + ((float)(s / 4) + 0.5f) * 0.25f) / MOSS_H;
                float shade = 0.0f, hue = 0.0f;
                if (cell < 2 ? !moss_side(cell, X, 1.0f - Y, &shade, &hue) : !moss_top(cell - 2, X, Y, &shade, &hue))
                    continue;
                shade = minf(shade, 1.0f), hue = clampf(hue, -1.0f, 1.0f);
                acc[0] += minf(1.0f, shade * (1.0f + 0.08f * hue)), acc[1] += shade, acc[2] += shade * (1.0f - 0.14f * hue), in += 1.0f;
            }
            float* o = &lv[((size_t)y * MOSS_W + x) * 4];
            for (int k = 0; k < 3; ++k)
                o[k] = acc[k] / 16.0f;
            o[3] = in / 16.0f;
            if (o[3] >= MOSS_REF)
            {
                sum += (o[0] + o[1] + o[2]) / (3.0f * o[3]) * (cell < 2 ? blade_quad_shade(y, MOSS_H) : 1.0f), n += 1.0;
            }
        }
    *avg = n > 0.0 ? (float)(sum / n) : 0.0f;
}

/* the moss's picture as a texture (g.moss_tex), with its mips: each the last's quarter, premultiplied, the empty texels in the moss's mean
 * colour (no dark fringes far off) */
static void make_moss_texture(void)
{
    float* lv = (float*)calloc((size_t)MOSS_W * MOSS_H * 4, sizeof(float));
    uint32_t* out = (uint32_t*)malloc((size_t)MOSS_W * MOSS_H * 4);
    if (!lv || !out)
    {
        free(lv), free(out);
        return;
    }
    paint_moss(lv, &g.moss_avg);
    int levels = 1;
    for (int w = MOSS_W, h = MOSS_H; w > 1 || h > 1; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        ++levels;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, MOSS_W, MOSS_H, (uint32_t)levels, GFX_USE_SAMPLE);
    for (int l = 0, w = MOSS_W, h = MOSS_H; t && l < levels; ++l)
    {
        double mean[3] = { 0, 0, 0 }, wsum = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i)
            mean[0] += lv[i * 4], mean[1] += lv[i * 4 + 1], mean[2] += lv[i * 4 + 2], wsum += lv[i * 4 + 3];
        for (size_t i = 0; i < (size_t)w * h; ++i)
        {
            const float* p = &lv[i * 4];
            uint32_t c = (uint32_t)(clampf(p[3], 0.0f, 1.0f) * 255.0f + 0.5f) << 24;
            for (int j = 0; j < 3; ++j)
                c |= (uint32_t)(clampf(p[3] > 1e-4f ? p[j] / p[3] : (float)(wsum > 0 ? mean[j] / wsum : 0.6), 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * j);
            out[i] = c;
        }
        gfx_tex_upload(t, 0, (uint32_t)l, out, (uint32_t)w * 4u);
        const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        for (int y = 0; y < dh; ++y) /* in place: each texel of the next level reads texels at or after its own */
            for (int x = 0; x < dw; ++x)
                for (int j = 0; j < 4; ++j)
                {
                    const int xa = 2 * x < w ? 2 * x : w - 1, xb = 2 * x + 1 < w ? 2 * x + 1 : w - 1;
                    const int ya = 2 * y < h ? 2 * y : h - 1, yb = 2 * y + 1 < h ? 2 * y + 1 : h - 1;
                    lv[((size_t)y * dw + x) * 4 + j] = 0.25f * (lv[((size_t)ya * w + xa) * 4 + j] + lv[((size_t)ya * w + xb) * 4 + j] +
                                                           lv[((size_t)yb * w + xa) * 4 + j] + lv[((size_t)yb * w + xb) * 4 + j]);
                }
        w = dw, h = dh;
    }
    free(lv), free(out);
    g.moss_tex = t;
}

/* one cushion of moss on p: two crossed upright quads of a side view, `size` wide and h tall, and a top view lying over them at 0.45
 * of h, 0.9 of size across (the side view's dome is that wide there: its ends meet the ground under the top's rim); col its colour (halved: drawn MODULATE2X, as the blades), pick its views and flips */
static void keep_moss(const float p[3], float yaw, float size, float h, const float col[3], uint32_t keep, uint32_t pick)
{
    KVert* v = kept_room(MOSS, 12);
    if (!v)
        return;
    const float root[3] = { p[0], p[1] + 0.02f, p[2] }, cw = 1.0f / 4.0f;
    const uint32_t bottom = pack_color(col[0] * 0.375f, col[1] * 0.375f, col[2] * 0.375f), top = pack_color(col[0] * 0.5f, col[1] * 0.5f, col[2] * 0.5f);
    const float ca = cosf(yaw) * size * 0.5f, sa = sinf(yaw) * size * 0.5f;
    for (int q = 0; q < 2; ++q)
    {
        const float dx = q ? -sa : ca, dz = q ? ca : sa, dy = lean(dx, dz); /* the second quad the first turned a quarter */
        const float ua = (float)((pick >> q) & 1u) * cw;
        const int flip = (pick >> (4 + q)) & 1u;
        const float u0 = flip ? ua + cw : ua, u1 = flip ? ua : ua + cw;
        v[0] = kvert(p[0] - dx, p[1] + 0.02f - dy, p[2] - dz, bottom, u0, 1, root, keep);
        v[1] = kvert(p[0] + dx, p[1] + 0.02f + dy, p[2] + dz, bottom, u1, 1, root, keep);
        v[2] = kvert(p[0] - dx, p[1] - h - dy, p[2] - dz, top, u0, 0, root, keep);
        v[3] = kvert(p[0] + dx, p[1] - h + dy, p[2] + dz, top, u1, 0, root, keep);
        v += 4;
    }
    /* the top, turned from the uprights, its corners following the ground's slope */
    const float ut = (2.0f + (float)((pick >> 2) & 1u)) * cw, k = 0.9f, cb = cosf(yaw + 0.4f) * size * 0.5f * k, sb = sinf(yaw + 0.4f) * size * 0.5f * k;
    const float cx[4] = { -cb + sb, cb + sb, -cb - sb, cb - sb }, cz[4] = { -sb - cb, sb - cb, -sb + cb, sb + cb };
    const float tu[4] = { ut, ut + cw, ut, ut + cw }, tv[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; ++i)
        v[i] = kvert(p[0] + cx[i], p[1] - 0.45f * h + lean(cx[i], cz[i]), p[2] + cz[i], top, tu[i], tv[i], root, keep);
}

/* a tuft's place on shallow water (build_step, floor_grows' GROW_MOSS): its cushion of moss and two smaller ones beside it, each
 * 0.3 to 0.55 across and 0.04 to 0.13 tall (times grow, the slope's share), in col (the tuft's) lifted by lift (g.moss_avg's, with
 * grass.match) and a touch lighter or darker each. The cushions made */
static int moss_spot(const float p[3], const float col[3], float lift, uint32_t keep, uint32_t sd, float grow)
{
    for (int i = 0; i < 3; ++i)
    {
        const uint32_t s = hash32(sd + 40u + (uint32_t)i * 7u);
        const float k = i ? 0.75f : 1.0f, a = 6.2831853f * rnd01(s), r = i ? 0.18f + 0.14f * rnd01(s + 1u) : 0.0f;
        const float dx = cosf(a) * r, dz = sinf(a) * r, q[3] = { p[0] + dx, p[1] + lean(dx, dz), p[2] + dz };
        const float tone = lift * (0.9f + 0.2f * rnd01(s + 2u)), c[3] = { col[0] * tone, col[1] * tone, col[2] * tone };
        keep_moss(q, rnd01(s + 3u) * 3.14159265f, (0.3f + 0.25f * rnd01(s + 4u)) * k * grow, (0.04f + 0.09f * rnd01(s + 5u)) * k * grow, c, keep,
            hash32(s + 6u));
    }
    return 3;
}
