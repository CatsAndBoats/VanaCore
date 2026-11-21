/* The drawn blades' pictures: type 1's one tuft of broad blades (make_tuft1_texture) and type 2's four tufts of fine curved
 * ones (make_tuft_texture, BLADE_*), and the shade keep_blades gives each row. grass.c includes this just before the flower's
 * picture, after g, g_level and TUFT_TEX, whose names it uses. It is no file of its own. */
#pragma once

/* the shade keep_blades gives row y of h of a drawn blades' picture: its quads' colour runs from 0.75 of it at the root (the bottom
 * row) to all of it at the tip. With it, each picture's mean shade where the alpha test keeps it (g.blade_avg): grass.match lifts
 * the tufts by it, to the ground's colour. The mean is a plain one, every row alike: the game's camera looks over a field at a
 * low angle, where the roots in the gaps between tufts are seen as much as the tips (weighted toward the tips, as it was for a
 * view from above, the field came out at three quarters of the ground's brightness and its roots at half: 2026-11-01). */
static float blade_quad_shade(int y, int h)
{
    return 1.0f - 0.25f * ((float)y + 0.5f) / (float)h;
}

/* The drawn blades of type 1 (asset 0), the first: one tuft of eight broad, straight blades fanned across the picture, each a
 * triangle from its base to a leaning tip, lighter toward the top. In grey: keep_blades colours it the ground's. */
static void make_tuft1_texture(void)
{
    uint32_t* px = g_level[0];
    for (int i = 0; i < TUFT_TEX * TUFT_TEX; ++i)
        px[i] = 0x00C8C8C8u; /* transparent, in the blades' own grey so the small mips don't darken */
    const int blades = 8;
    for (int b = 0; b < blades; ++b)
    {
        const float bx = 5.0f + (float)b * (TUFT_TEX - 10.0f) / (float)(blades - 1) + (rnd01(b * 17 + 1) - 0.5f) * 5.0f;
        const float bh = 36.0f + rnd01(b * 17 + 2) * 26.0f, lean = (rnd01(b * 17 + 3) - 0.5f) * 26.0f;
        const float bw = 5.0f + rnd01(b * 17 + 4) * 3.5f, tone = 0.82f + 0.18f * rnd01(b * 17 + 5);
        const float ax = bx - bw * 0.5f, bxr = bx + bw * 0.5f, ay = (float)TUFT_TEX, cx = bx + lean, cy = (float)TUFT_TEX - bh;
        for (int y = 0; y < TUFT_TEX; ++y)
            for (int x = 0; x < TUFT_TEX; ++x)
            {
                int in = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                    {
                        const float X = (float)x + ((float)sx + 0.5f) * 0.25f, Y = (float)y + ((float)sy + 0.5f) * 0.25f;
                        const float d0 = (bxr - ax) * (Y - ay) - (ay - ay) * (X - ax);
                        const float d1 = (cx - bxr) * (Y - ay) - (cy - ay) * (X - bxr);
                        const float d2 = (ax - cx) * (Y - cy) - (ay - cy) * (X - cx);
                        in += (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
                    }
                if (!in)
                    continue;
                const float grad = 0.78f + 0.22f * (1.0f - (float)y / (float)TUFT_TEX);
                const uint32_t v = (uint32_t)(255.0f * tone * grad), a = (uint32_t)(in * 255 / 16);
                const uint32_t old = px[y * TUFT_TEX + x] >> 24;
                const uint32_t na = old + a > 255 ? 255 : old + a;
                px[y * TUFT_TEX + x] = (na << 24) | (v << 16) | (v << 8) | v;
            }
    }
    {
        double sum = 0.0, n = 0.0;
        for (int y = 0; y < TUFT_TEX; ++y)
            for (int x = 0; x < TUFT_TEX; ++x)
                if ((px[y * TUFT_TEX + x] >> 24) > 100u) /* kept_draw's alpha test */
                {
                    sum += (float)(px[y * TUFT_TEX + x] & 255u) / 255.0f * blade_quad_shade(y, TUFT_TEX), n += 1.0;
                }
        g.blade_avg[0] = n > 0.0 ? (float)(sum / n) : 0.0f;
    }
    g.tuft[0] = upload_mipped();
}

/* The drawn blades of type 2 (asset 1), the finer: BLADE_VARIANTS tufts side by side, each BLADE_TEX square, keep_blades picking
 * one a tuft and flipping each of its quads at random. A tuft is a clump of thin blades, each a curve from its root that leans out
 * and bends over toward its tip, tapering to a point: darker at the root, lighter along its middle, a touch yellower or bluer than
 * the next, the darker ones behind. Painted with smooth edges (coverage from the distance to the blade's edge, not samples), in
 * grey: keep_blades colours it the ground's. */
#define BLADE_TEX 256
#define BLADE_VARIANTS 4
#define BLADE_REF (100.0f / 255.0f) /* kept_draw's alpha test for the blades */

/* one blade, premultiplied into acc (w wide, rows of float rgba): a quadratic curve P0 (the root) - P1 - P2 (the tip), w0 wide at
 * the root */
static void paint_blade(float* acc, int w, int h, const float P[3][2], float w0, float tone, float hue)
{
    enum { N = 20 };
    float pt[N + 1][2], hw[N + 1];
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (int i = 0; i <= N; ++i)
    {
        const float t = (float)i / N, a = (1 - t) * (1 - t), b = 2 * t * (1 - t), c = t * t;
        for (int k = 0; k < 2; ++k)
        {
            pt[i][k] = a * P[0][k] + b * P[1][k] + c * P[2][k];
            lo[k] = minf(lo[k], pt[i][k]), hi[k] = maxf(hi[k], pt[i][k]);
        }
        hw[i] = 0.5f * w0 * powf(1.0f - t, 0.8f);
    }
    const int x0 = (int)maxf(0.0f, lo[0] - w0 - 2), x1 = (int)minf((float)w - 1, hi[0] + w0 + 2);
    const int y0 = (int)maxf(0.0f, lo[1] - w0 - 2), y1 = (int)minf((float)h - 1, hi[1] + w0 + 2);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const float X = (float)x + 0.5f, Y = (float)y + 0.5f;
            float best = 1e30f, bt = 0.0f, bd = 0.0f, bh = 1.0f;
            for (int i = 0; i < N; ++i)
            {
                const float ex = pt[i + 1][0] - pt[i][0], ey = pt[i + 1][1] - pt[i][1], ll = ex * ex + ey * ey;
                const float u = ll > 1e-9f ? clampf(((X - pt[i][0]) * ex + (Y - pt[i][1]) * ey) / ll, 0.0f, 1.0f) : 0.0f;
                const float dx = X - pt[i][0] - u * ex, dy = Y - pt[i][1] - u * ey, d = sqrtf(dx * dx + dy * dy);
                const float half = hw[i] + u * (hw[i + 1] - hw[i]);
                if (d - half < best)
                    best = d - half, bt = ((float)i + u) / N, bd = d, bh = half;
            }
            const float cov = clampf(0.5f - best, 0.0f, 1.0f);
            if (cov <= 0.0f)
                continue;
            /* the root's shade up to the tip's, lighter along the middle than at the edges */
            const float s = bh > 1e-3f ? minf(bd / bh, 1.0f) : 1.0f;
            const float v = tone * (0.78f + 0.22f * powf(bt, 0.6f)) * (1.06f - 0.16f * s * s);
            const float rgb[3] = { minf(1.0f, v * (1.0f + 0.06f * hue)), minf(1.0f, v), minf(1.0f, v * (1.0f - 0.1f * hue)) };
            float* o = &acc[((size_t)y * w + x) * 4];
            for (int k = 0; k < 3; ++k)
                o[k] = rgb[k] * cov + o[k] * (1.0f - cov);
            o[3] = cov + o[3] * (1.0f - cov);
        }
}

/* the share of acc's texels (n of them, premultiplied rgba) the alpha test keeps with their alpha times k */
static float blade_cover(const float* acc, size_t n, float k)
{
    size_t in = 0;
    for (size_t i = 0; i < n; ++i)
        in += acc[i * 4 + 3] * k >= BLADE_REF;
    return n ? (float)in / (float)n : 0.0f;
}

static void make_tuft_texture(void)
{
    const int W = BLADE_TEX * BLADE_VARIANTS, H = BLADE_TEX;
    float* lv = (float*)calloc((size_t)W * H * 4, sizeof(float)); /* a level, premultiplied */
    float* nx = (float*)calloc((size_t)W * H, sizeof(float));     /* the next, a quarter of it */
    uint32_t* out = (uint32_t*)malloc((size_t)W * H * 4);
    if (!lv || !nx || !out)
    {
        free(lv), free(nx), free(out);
        return;
    }
    const float S = (float)BLADE_TEX, margin = S * 0.06f;
    for (int v = 0; v < BLADE_VARIANTS; ++v)
    {
        const uint32_t seed = 7919u * (uint32_t)(v + 1);
        const int n = 17 + (int)(rnd01(seed) * 7.0f);
        /* the blades in order of tone, the darker first: they stand behind */
        float tone[24];
        for (int b = 0; b < n; ++b)
            tone[b] = 0.7f + 0.3f * rnd01(seed + (uint32_t)b * 31u + 1u);
        for (int a = 1; a < n; ++a)
            for (int b = a; b > 0 && tone[b] < tone[b - 1]; --b)
            {
                const float t = tone[b];
                tone[b] = tone[b - 1], tone[b - 1] = t;
            }
        for (int b = 0; b < n; ++b)
        {
            const uint32_t r = seed + (uint32_t)b * 131u;
            const float ox = (float)v * S, cx = ox + 0.5f * S;
            const float bx = cx + (rnd01(r + 2u) - 0.5f) * S * 0.5f;
            const float bh = S * (0.42f + 0.5f * rnd01(r + 3u));
            /* leaning out from the clump's middle, more the farther out the root, and bending over: the tip lower than the height */
            const float lean = ((bx - cx) / (0.2f * S) * 0.3f + (rnd01(r + 4u) - 0.5f) * 0.6f) * bh * 0.55f;
            float P[3][2] = { { bx, S + 2.0f }, { bx + lean * 0.2f, S - bh * 0.62f }, { bx + lean, S - bh + fabsf(lean) * 0.35f } };
            for (int k = 1; k < 3; ++k)
                P[k][0] = clampf(P[k][0], ox + margin, ox + S - margin);
            paint_blade(lv, W, H, P, S * (0.032f + 0.024f * rnd01(r + 5u)), tone[b], 2.0f * rnd01(r + 6u) - 1.0f);
        }
    }
    {
        double sum = 0.0, n = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float* p = &lv[((size_t)y * W + x) * 4];
                if (p[3] >= BLADE_REF)
                {
                    sum += (p[0] + p[1] + p[2]) / (3.0f * p[3]) * blade_quad_shade(y, H), n += 1.0;
                }
            }
        g.blade_avg[1] = n > 0.0 ? (float)(sum / n) : 0.0f;
    }
    /* the levels: each the last's quarter, premultiplied (no dark or light fringes), its alpha raised so the alpha test keeps as much
     * of it as of the first (thin blades would thin out and vanish far off) and the empty texels in the blades' mean colour */
    const float want = blade_cover(lv, (size_t)W * H, 1.0f);
    int levels = 1;
    for (int w = W, h = H; w > 1 || h > 1; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        ++levels;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, (uint32_t)W, (uint32_t)H, (uint32_t)levels, GFX_USE_SAMPLE);
    for (int l = 0, w = W, h = H; t && l < levels; ++l)
    {
        float k = 1.0f;
        if (l > 0 && want > 0.0f)
        {
            float a = 1.0f, b = 16.0f;
            for (int it = 0; it < 16; ++it)
            {
                const float m = 0.5f * (a + b);
                if (blade_cover(lv, (size_t)w * h, m) < want)
                    a = m;
                else
                    b = m;
            }
            k = b;
        }
        double mean[3] = { 0, 0, 0 }, wsum = 0;
        for (size_t i = 0; i < (size_t)w * h; ++i)
            mean[0] += lv[i * 4], mean[1] += lv[i * 4 + 1], mean[2] += lv[i * 4 + 2], wsum += lv[i * 4 + 3];
        for (int c = 0; c < 3; ++c)
            mean[c] = wsum > 0 ? mean[c] / wsum : 0.8;
        for (size_t i = 0; i < (size_t)w * h; ++i)
        {
            const float* p = &lv[i * 4];
            const float a = minf(1.0f, p[3] * k);
            float c[3];
            for (int j = 0; j < 3; ++j)
                c[j] = p[3] > 1e-4f ? p[j] / p[3] : (float)mean[j];
            out[i] = (uint32_t)(a * 255.0f + 0.5f) << 24 | (uint32_t)(clampf(c[0], 0, 1) * 255.0f + 0.5f) << 16 |
                (uint32_t)(clampf(c[1], 0, 1) * 255.0f + 0.5f) << 8 | (uint32_t)(clampf(c[2], 0, 1) * 255.0f + 0.5f);
        }
        gfx_tex_upload(t, 0, (uint32_t)l, out, (uint32_t)w * 4u);
        if (l + 1 == levels)
            break;
        const int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
                for (int j = 0; j < 4; ++j)
                {
                    const int xa = 2 * x < w ? 2 * x : w - 1, xb = 2 * x + 1 < w ? 2 * x + 1 : w - 1;
                    const int ya = 2 * y < h ? 2 * y : h - 1, yb = 2 * y + 1 < h ? 2 * y + 1 : h - 1;
                    nx[((size_t)y * dw + x) * 4 + j] = 0.25f * (lv[((size_t)ya * w + xa) * 4 + j] + lv[((size_t)ya * w + xb) * 4 + j] +
                                                           lv[((size_t)yb * w + xa) * 4 + j] + lv[((size_t)yb * w + xb) * 4 + j]);
                }
        memcpy(lv, nx, (size_t)dw * dh * 4 * sizeof(float));
        w = dw, h = dh;
    }
    free(lv), free(nx), free(out);
    g.tuft[1] = t;
}
