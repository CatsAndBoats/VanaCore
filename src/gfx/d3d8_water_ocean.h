/* d3d8_water_ocean.h: an include unit of d3d8_water.h (in its scope: g_water_set, g_water_turn, g_water_clock, gfx_tex_*). The
 * open sea's waves for VanaCore's sea plane (water.sea; d3d8_water_sea.h water_horizon_pass), as film and games make an ocean:
 * Tessendorf's FFT ocean ("Simulating Ocean Water", 2001; the approach of Crest and of Garrett Gunnell's water, both MIT). A
 * wind-driven spectrum of thousands of waves (Phillips', spread about the wind's direction) is moved on in time by deep water's
 * dispersion and summed by inverse FFTs into the surface's slopes, its choppy crests and where the crests fold (the Jacobian of
 * the choppy displacement under 1: whitecaps, which linger and fade). Two cascades, 64 x 64 each, tiling OCEAN_L0 and OCEAN_L1
 * yalms (their ratio no whole number, so the two never line up): the swell, and the chop over it, each wavelength in one of
 * them only. Worked out on the CPU each frame (6 FFTs of 64 x 64, well under a millisecond) and sent up as two A8R8G8B8 textures
 * with their mips (the far sea reads the mips: no moire): R and G the slopes in x and z, B the foam, A the height.
 *   Each wave's frequency is a whole number of turns in the water's 600 s clock (g_water_clock), so the clock's wrap is seamless.
 * The shader's side: gfx_hlsl_water.h (GfxFsKey.water 4), stages 4 and 5. */
#pragma once

#define OCEAN_N 64
#define OCEAN_LOG 6
#define OCEAN_L0 223.0f /* yalms the swell's tile covers */
#define OCEAN_L1 31.7f  /* the chop's */
#define OCEAN_SPLIT 10.0f /* the wavelength between them, yalms: longer in the swell, shorter in the chop */
#define OCEAN_SMAX 1.25f  /* the slope the texture's 8 bits reach (gfx_hlsl_water.h decodes by it) */
#define OCEAN_MIPS 7      /* 64 to 1 */
#define OCEAN_TEXELS (64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1)

typedef struct
{
    float re, im;
} OcC;

static struct
{
    GfxTex* tex[2];
    OcC h0[2][OCEAN_N * OCEAN_N];  /* h0(k) */
    OcC h0m[2][OCEAN_N * OCEAN_N]; /* conj(h0(-k)) */
    float w[2][OCEAN_N * OCEAN_N]; /* deep water's frequency, a whole number of turns in 600 s */
    float foam[2][OCEAN_N * OCEAN_N];
    float hscale;                  /* the height's spread, for A */
    OcC f1[OCEAN_N * OCEAN_N], f2[OCEAN_N * OCEAN_N], f3[OCEAN_N * OCEAN_N];
    uint8_t px[OCEAN_TEXELS * 4];
    float made_waves, made_dir, made_turb, made_size; /* the settings the spectrum was made for */
    float L[2];                    /* the cascades' tiles, yalms: OCEAN_L0 and OCEAN_L1 times the square root of water.size */
    double clock;                  /* the ocean's own time, wrapped at 600 s */
    uint64_t last_ns;
    int made, failed;
    uint32_t frame;
    OcC tw[OCEAN_N / 2];
    int rev[OCEAN_N];
} g_ocean;

static uint32_t ocean_rng = 0x9E3779B9u;
static float ocean_uniform(void)
{
    ocean_rng ^= ocean_rng << 13, ocean_rng ^= ocean_rng >> 17, ocean_rng ^= ocean_rng << 5;
    return ((float)(ocean_rng >> 8) + 0.5f) * (1.0f / 16777216.0f);
}
static void ocean_gauss(float* a, float* b)
{
    const float u1 = ocean_uniform(), u2 = ocean_uniform();
    const float r = sqrtf(-2.0f * logf(u1));
    *a = r * cosf(6.2831853f * u2), *b = r * sinf(6.2831853f * u2);
}

/* the wave number of index n of N (FFT order: 0, 1 .. N/2 - 1, -N/2 .. -1) */
static float ocean_k(int n, float L)
{
    return 6.2831853f * (float)(n < OCEAN_N / 2 ? n : n - OCEAN_N) / L;
}

/* Phillips' spectrum, spread about the wind (wx, wz): the swell's direction with a little of every other, so the sea is
 * confused, not combed into parallel lines; per mode (dk squared), no amplitude yet */
static float ocean_phillips(float kx, float kz, float wx, float wz, float Lw)
{
    const float k2 = kx * kx + kz * kz;
    if (k2 < 1e-8f)
        return 0.0f;
    const float k = sqrtf(k2);
    const float c = (kx * wx + kz * wz) / k;
    float dirn = 0.05f + 0.95f * c * c;
    if (c < 0.0f)
        dirn *= 0.2f; /* waves running against the wind, few */
    const float small = 0.12f; /* yalms: the shortest damped away */
    return expf(-1.0f / (k2 * Lw * Lw)) / (k2 * k2) * dirn * expf(-k2 * small * small);
}

/* the spectrum's random amplitudes for the settings: made again when they change */
static void ocean_make(void)
{
    const float wx = g_water_turn[0], wz = g_water_turn[1];
    /* the settings were made for the old sine waves (which took them high): gentler here. The wind 6 to 9 yalms a second by
     * water.turbulence (0 to 4) */
    const float V = 6.0f + 0.75f * fminf(fmaxf(g_water_set.turbulence, 0.0f), 4.0f);
    const float zs = sqrtf(fmaxf(g_water_set.size, 0.25f));
    g_ocean.L[0] = OCEAN_L0 * zs, g_ocean.L[1] = OCEAN_L1 * zs;
    const float Lw = V * V / 9.81f * zs; /* the size setting stretches the waves themselves, not just their tiles */
    const float kc = 6.2831853f / (OCEAN_SPLIT * zs);
    const float w0 = 6.2831853f / 600.0f;
    double var = 0.0;
    ocean_rng = 0x9E3779B9u;
    for (int c = 0; c < 2; ++c)
    {
        const float L = g_ocean.L[c], dk = 6.2831853f / L;
        for (int m = 0; m < OCEAN_N; ++m)
            for (int n = 0; n < OCEAN_N; ++n)
            {
                const int i = m * OCEAN_N + n;
                const float kx = ocean_k(n, L), kz = ocean_k(m, L), k = sqrtf(kx * kx + kz * kz);
                const int mine = c ? k >= kc : k < kc;
                float a, b;
                ocean_gauss(&a, &b);
                const float P = mine ? ocean_phillips(kx, kz, wx, wz, Lw) * dk * dk : 0.0f;
                const float s = sqrtf(P * 0.5f);
                g_ocean.h0[c][i] = (OcC){ a * s, b * s };
                g_ocean.w[c][i] = floorf(sqrtf(9.81f * k) / w0) * w0;
                var += (double)(P * k * k); /* the slopes' variance (x and z together), at amplitude 1 */
            }
        for (int m = 0; m < OCEAN_N; ++m)
            for (int n = 0; n < OCEAN_N; ++n)
            {
                const int i = m * OCEAN_N + n, j = ((OCEAN_N - m) & (OCEAN_N - 1)) * OCEAN_N + ((OCEAN_N - n) & (OCEAN_N - 1));
                g_ocean.h0m[c][i] = (OcC){ g_ocean.h0[c][j].re, -g_ocean.h0[c][j].im };
            }
    }
    /* scaled to the slope wanted: water.waves 0 a calm sea (RMS slope 0.02), 1 a light one (0.042), 3 a choppy one (0.086). At
     * 0.27 (waves 3, first cut) the sky's and the body's colours broke the sea into camouflage patches */
    const float waves = fminf(fmaxf(g_water_set.waves, 0.0f), 4.0f);
    const float want = 0.025f + 0.03f * waves; /* (bigger, 2026-11-16: 0.02 + 0.022 x waves read as a calm harbour) */
    const float sc = var > 0.0 ? want / sqrtf((float)var * 2.0f) : 0.0f; /* (h0 and h0(-k) both carry it) */
    double hv = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < OCEAN_N * OCEAN_N; ++i)
        {
            g_ocean.h0[c][i].re *= sc, g_ocean.h0[c][i].im *= sc;
            g_ocean.h0m[c][i].re *= sc, g_ocean.h0m[c][i].im *= sc;
            hv += (double)(g_ocean.h0[c][i].re * g_ocean.h0[c][i].re + g_ocean.h0[c][i].im * g_ocean.h0[c][i].im) * 2.0;
        }
    g_ocean.hscale = fmaxf(3.0f * sqrtf((float)hv), 1e-3f);
    memset(g_ocean.foam, 0, sizeof g_ocean.foam);
    for (int i = 0; i < OCEAN_N / 2; ++i)
        g_ocean.tw[i] = (OcC){ cosf(6.2831853f * (float)i / OCEAN_N), sinf(6.2831853f * (float)i / OCEAN_N) };
    for (int i = 0; i < OCEAN_N; ++i)
    {
        int r = 0;
        for (int b = 0; b < OCEAN_LOG; ++b)
            r |= ((i >> b) & 1) << (OCEAN_LOG - 1 - b);
        g_ocean.rev[i] = r;
    }
    g_ocean.made_waves = g_water_set.waves, g_ocean.made_dir = g_water_turn[0] * 7.0f + g_water_turn[1];
    g_ocean.made_turb = g_water_set.turbulence, g_ocean.made_size = g_water_set.size, g_ocean.made = 1;
}

/* an inverse FFT (e^+i, unscaled) of N points, stride s apart */
static void ocean_fft(OcC* a, int s)
{
    for (int i = 0; i < OCEAN_N; ++i)
    {
        const int j = g_ocean.rev[i];
        if (j > i)
        {
            const OcC t = a[i * s];
            a[i * s] = a[j * s], a[j * s] = t;
        }
    }
    for (int len = 2; len <= OCEAN_N; len <<= 1)
    {
        const int half = len >> 1, step = OCEAN_N / len;
        for (int i = 0; i < OCEAN_N; i += len)
            for (int j = 0; j < half; ++j)
            {
                const OcC w = g_ocean.tw[j * step];
                OcC* p = &a[(i + j) * s];
                OcC* q = &a[(i + j + half) * s];
                const OcC t = { q->re * w.re - q->im * w.im, q->re * w.im + q->im * w.re };
                q->re = p->re - t.re, q->im = p->im - t.im;
                p->re += t.re, p->im += t.im;
            }
    }
}

static void ocean_fft2(OcC* a)
{
    for (int m = 0; m < OCEAN_N; ++m)
        ocean_fft(a + m * OCEAN_N, 1);
    for (int n = 0; n < OCEAN_N; ++n)
        ocean_fft(a + n, OCEAN_N);
}

static uint8_t ocean_byte(float v)
{
    v = v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v;
    return (uint8_t)(v + 0.5f);
}

/* one cascade at time t into g_ocean.px (its mips after the top level), its foam carried on over dt */
static void ocean_cascade(int c, float t, float dt)
{
    const float L = g_ocean.L[c];
    const float chop = 1.0f; /* the crests' choppiness (the displacement's scale) */
    for (int m = 0; m < OCEAN_N; ++m)
        for (int n = 0; n < OCEAN_N; ++n)
        {
            const int i = m * OCEAN_N + n;
            const float kx = ocean_k(n, L), kz = ocean_k(m, L), k = sqrtf(kx * kx + kz * kz);
            const float ph = g_ocean.w[c][i] * t, cw = cosf(ph), sw = sinf(ph);
            const OcC a = g_ocean.h0[c][i], b = g_ocean.h0m[c][i];
            /* h(k, t) = h0 e^(iwt) + conj(h0(-k)) e^(-iwt) */
            const OcC h = { a.re * cw - a.im * sw + b.re * cw + b.im * sw, a.re * sw + a.im * cw - b.re * sw + b.im * cw };
            const float ik = k > 1e-6f ? 1.0f / k : 0.0f;
            /* f1 = i kx h + i (i kz h): the slopes, x real and z imaginary */
            g_ocean.f1[i] = (OcC){ -kx * h.im - kz * h.re, kx * h.re - kz * h.im };
            /* f2 = kx^2/k h + i kz^2/k h: the displacement's dDx/dx and dDz/dz */
            const float axx = kx * kx * ik, azz = kz * kz * ik, axz = kx * kz * ik;
            g_ocean.f2[i] = (OcC){ axx * h.re - azz * h.im, axx * h.im + azz * h.re };
            /* f3 = kx kz/k h + i h: dDx/dz and the height */
            g_ocean.f3[i] = (OcC){ axz * h.re - h.im, axz * h.im + h.re };
        }
    ocean_fft2(g_ocean.f1), ocean_fft2(g_ocean.f2), ocean_fft2(g_ocean.f3);
    const float fade = expf(-dt / 2.6f); /* whitecaps linger a few seconds, thinning into the lace (the shader's threshold) */
    uint8_t* top = g_ocean.px;
    for (int i = 0; i < OCEAN_N * OCEAN_N; ++i)
    {
        /* the surface displaced x - chop D: its slopes steeper at the crests (they bunch up), flatter in the troughs */
        const float jx = 1.0f - chop * g_ocean.f2[i].re, jz = 1.0f - chop * g_ocean.f2[i].im, jxz = chop * g_ocean.f3[i].re;
        const float J = jx * jz - jxz * jxz;
        const float sx = g_ocean.f1[i].re / fmaxf(jx, 0.3f), sz = g_ocean.f1[i].im / fmaxf(jz, 0.3f);
        const float fnew = fminf(fmaxf((0.82f - J) * 3.0f, 0.0f), 1.0f);
        float* f = &g_ocean.foam[c][i];
        *f = fmaxf(*f * fade, fnew);
        uint8_t* p = top + i * 4; /* B, G, R, A in memory (A8R8G8B8) */
        p[0] = ocean_byte(*f * 255.0f);
        p[1] = ocean_byte(128.0f + sz / OCEAN_SMAX * 127.0f);
        p[2] = ocean_byte(128.0f + sx / OCEAN_SMAX * 127.0f);
        p[3] = ocean_byte(128.0f + g_ocean.f3[i].im / g_ocean.hscale * 127.0f);
    }
    /* the mips: each texel the mean of the four under it */
    const uint8_t* src = top;
    uint8_t* dst = top + OCEAN_N * OCEAN_N * 4;
    for (int sz = OCEAN_N / 2; sz >= 1; sz >>= 1)
    {
        for (int y = 0; y < sz; ++y)
            for (int x = 0; x < sz; ++x)
                for (int ch = 0; ch < 4; ++ch)
                {
                    const int w2 = sz * 2;
                    const int s = src[((2 * y) * w2 + 2 * x) * 4 + ch] + src[((2 * y) * w2 + 2 * x + 1) * 4 + ch] +
                                  src[((2 * y + 1) * w2 + 2 * x) * 4 + ch] + src[((2 * y + 1) * w2 + 2 * x + 1) * 4 + ch];
                    dst[(y * sz + x) * 4 + ch] = (uint8_t)((s + 2) >> 2);
                }
        src = dst, dst += sz * sz * 4;
    }
    uint32_t off = 0;
    for (uint32_t lv = 0, sz = OCEAN_N; lv < OCEAN_MIPS; ++lv, sz >>= 1)
    {
        gfx_tex_upload(g_ocean.tex[c], 0, lv, g_ocean.px + off, sz * 4);
        off += sz * sz * 4;
    }
}

/* The ocean's two textures for this frame (once a frame), or 0 when there are none */
static int ocean_update(void)
{
    if (g_ocean.failed)
        return 0;
    if (!g_ocean.tex[0])
    {
        for (int c = 0; c < 2; ++c)
            g_ocean.tex[c] = gfx_tex_create(GFX_TEX_2D, 21u /* A8R8G8B8 */, OCEAN_N, OCEAN_N, OCEAN_MIPS, GFX_USE_SAMPLE);
        if (!g_ocean.tex[0] || !g_ocean.tex[1])
        {
            gfx_tex_destroy(g_ocean.tex[0]), gfx_tex_destroy(g_ocean.tex[1]);
            g_ocean.tex[0] = g_ocean.tex[1] = NULL, g_ocean.failed = 1;
            return 0;
        }
        g_ocean.frame = g_scene.frame - 1;
    }
    if (!g_ocean.made || g_ocean.made_waves != g_water_set.waves || g_ocean.made_turb != g_water_set.turbulence ||
        g_ocean.made_dir != g_water_turn[0] * 7.0f + g_water_turn[1] || g_ocean.made_size != g_water_set.size)
        ocean_make();
    if (g_ocean.frame != g_scene.frame)
    {
        g_ocean.frame = g_scene.frame;
        /* its own clock: the waves move as deep water's do (the size setting stretches them and slows them with it), water.speed
         * only nudging it (0.7 + 0.1 x speed: the old waves wanted speed 3 to look alive, the ocean at 3 runs true) */
        const uint64_t now = gfx_now_ns();
        double dt = g_ocean.last_ns ? (double)(now - g_ocean.last_ns) * 1e-9 : 0.0;
        if (dt > 0.25)
            dt = 0.0;
        g_ocean.last_ns = now;
        dt *= 0.7 + 0.1 * fmin(fmax((double)g_water_set.speed, 0.0), 4.0);
        g_ocean.clock = fmod(g_ocean.clock + dt, 600.0);
        const double t = g_ocean.clock;
        ocean_cascade(0, (float)t, (float)dt);
        ocean_cascade(1, (float)t, (float)dt);
    }
    return 1;
}

/* the device lost: the textures went with it */
static void ocean_lost(void)
{
    gfx_tex_destroy(g_ocean.tex[0]), gfx_tex_destroy(g_ocean.tex[1]);
    g_ocean.tex[0] = g_ocean.tex[1] = NULL, g_ocean.failed = 0;
}
