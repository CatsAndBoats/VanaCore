/* VanaCore's weathers ([graphics] weather*, d3d8_set_weather_fx): particles in the air round the camera for the server's weathers
 * other than rain (d3d8_rain.h has rain, squall and the thunder weathers' rain). One kind for each element's pair, the double
 * weather heavier and windier than the single:
 *   fire  (4 hot spell, 5 heat wave)      embers rising and flickering, glowing
 *   earth (8 dust storm, 9 sand storm)    grit streaking past on the wind, fine dust, big faint clouds of it drifting
 *   wind  (10 wind, 11 gales)             leaves tumbling past, faint lines of the wind
 *   ice   (12 snow, 13 blizzards)         flakes falling and swaying near, a finer fall further out; driven sideways in a blizzard
 *   light (16 auroras, 17 stellar glare)  motes of light drifting up and twinkling
 *   dark  (18 gloom, 19 darkness)         ash rising, dark wisps drifting
 * Each layer is a field of particles in a box round the camera that wraps as it moves (never spawned, never landing): what leaves
 * one side comes in at the other, faded out at the box's edge so the wrap isn't seen. The rain's shelter map (cover_at) keeps them
 * out from under roofs, bridges and caves, the scene's depth hides them behind the world, the fog takes the far ones. A weather
 * comes and goes over some 8 s, a change of weather crossfades. Only in a zone whose model DAT has that weather (the rain's
 * g_rain_zone_weathers), or as weather.preview says whatever the weather.
 * d3d8.c includes this after d3d8_rain.h (its weather, zone, shelter and particle vertices) and calls weather_pass after rain_pass.
 * It is no file of its own. */
#pragma once

static WeatherSettings g_wx_set = { 1, { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f }, -1 };

void d3d8_set_weather_fx(const WeatherSettings* settings)
{
    g_wx_set = *settings;
    for (int i = 0; i < WX_KINDS; ++i)
        g_wx_set.amount[i] = g_wx_set.amount[i] < 0.0f ? 0.0f : g_wx_set.amount[i] > 3.0f ? 3.0f : g_wx_set.amount[i];
}

enum { WX_DOT, WX_STREAK, WX_LEAF };
/* one layer of a kind: `count` particles at amount 1 in a box `reach` yalms each way round the camera across, `height` up and
 * down; falling at `fall` yalms a second (the world's y is down: less than 0 rises), each its own within `fall_jit` of it; carried
 * by the kind's wind times `wind`, swaying `sway` yalms a second; `size` yalms across (within `size_jit` of it); drawn stretched
 * along the motion by `stretch` (0 round), `alpha` at most, twinkling by `twinkle` (0 none to 1 out) `rate` times a second;
 * blended over (0) or added (1, glowing); lit by the scene (1) or its own light (0); one of three colours */
typedef struct WxLayer
{
    uint16_t count;
    uint8_t shape, add;
    float reach, height, fall, fall_jit, wind, sway, size, size_jit, stretch, alpha, twinkle, rate;
    uint8_t lit;
    uint32_t col[3];
} WxLayer;

#define WX_LAYERS 3
/* a kind: its layers, its wind in yalms a second (the double weather's times `gust`), how much more of it falls in the double */
typedef struct WxKind
{
    const char* name;
    float wind, gust, more;
    WxLayer layer[WX_LAYERS];
} WxKind;

static const WxKind g_wx_kinds[WX_KINDS] = {
    { "fire", 1.2f, 1.6f, 1.8f,
      { { 260, WX_DOT, 1, 16.0f, 7.0f, -0.9f, 0.5f, 0.6f, 0.45f, 0.035f, 0.5f, 0.0f, 0.95f, 0.8f, 2.2f, 0, { 0xFF7A1Eu, 0xFFB040u, 0xE8401Au } },
        { 500, WX_DOT, 1, 30.0f, 12.0f, -0.6f, 0.4f, 0.5f, 0.3f, 0.02f, 0.4f, 0.0f, 0.7f, 0.9f, 3.0f, 0, { 0xFF9030u, 0xFFD070u, 0xFF6020u } },
        { 0 } } },
    { "earth", 6.0f, 1.7f, 1.8f,
      { { 2200, WX_STREAK, 0, 16.0f, 7.0f, 0.4f, 0.3f, 1.0f, 0.6f, 0.006f, 0.4f, 0.05f, 0.55f, 0.0f, 0.0f, 1, { 0xC9A672u, 0xB08A58u, 0xD8BC8Cu } },
        { 1400, WX_DOT, 0, 26.0f, 10.0f, 0.15f, 0.2f, 0.8f, 0.5f, 0.022f, 0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 1, { 0xC4A06Au, 0xA88456u, 0xD2B482u } },
        { 110, WX_DOT, 0, 30.0f, 5.0f, 0.0f, 0.1f, 0.6f, 0.3f, 1.8f, 0.6f, 0.0f, 0.07f, 0.0f, 0.0f, 1, { 0xC09C68u, 0xB49060u, 0xCCAE7Eu } } } },
    { "wind", 4.5f, 1.9f, 2.0f,
      { { 140, WX_LEAF, 0, 20.0f, 8.0f, 0.5f, 0.4f, 1.0f, 1.3f, 0.07f, 0.4f, 0.0f, 0.95f, 0.0f, 0.8f, 1, { 0x6E8A2Eu, 0xA07A32u, 0x8C5A24u } },
        { 220, WX_STREAK, 0, 20.0f, 6.0f, 0.0f, 0.05f, 1.7f, 0.2f, 0.004f, 0.3f, 0.3f, 0.14f, 0.6f, 0.4f, 1, { 0xE8ECF0u, 0xDDE4EAu, 0xF2F4F6u } },
        { 0 } } },
    { "ice", 1.2f, 6.0f, 2.2f,
      { { 2400, WX_DOT, 0, 14.0f, 9.0f, 1.1f, 0.45f, 0.6f, 0.55f, 0.03f, 0.5f, 0.0f, 0.95f, 0.0f, 0.0f, 1, { 0xFFFFFFu, 0xF2F6FFu, 0xE8EEF8u } },
        { 3200, WX_DOT, 0, 45.0f, 18.0f, 1.0f, 0.4f, 0.6f, 0.4f, 0.05f, 0.4f, 0.0f, 0.6f, 0.0f, 0.0f, 1, { 0xFFFFFFu, 0xEEF2FAu, 0xE4EAF4u } },
        { 0 } } },
    { "light", 0.6f, 1.4f, 1.8f,
      { { 420, WX_DOT, 1, 20.0f, 8.0f, -0.18f, 0.15f, 0.5f, 0.35f, 0.035f, 0.5f, 0.0f, 0.9f, 1.0f, 1.4f, 0, { 0x7DF2B0u, 0x7CC8FFu, 0xC890FFu } },
        { 300, WX_DOT, 1, 32.0f, 12.0f, -0.1f, 0.1f, 0.4f, 0.25f, 0.06f, 0.4f, 0.0f, 0.5f, 1.0f, 0.9f, 0, { 0xFFF0B8u, 0xFFFFFFu, 0xFFE08Au } },
        { 0 } } },
    { "dark", 0.8f, 1.5f, 1.8f,
      { { 1100, WX_DOT, 0, 18.0f, 8.0f, -0.3f, 0.25f, 0.5f, 0.4f, 0.03f, 0.6f, 0.0f, 0.75f, 0.3f, 0.6f, 0, { 0x1A1420u, 0x2A1E36u, 0x0E0A12u } },
        { 70, WX_DOT, 0, 26.0f, 6.0f, -0.15f, 0.1f, 0.5f, 0.3f, 1.4f, 0.6f, 0.0f, 0.08f, 0.0f, 0.0f, 0, { 0x16101Eu, 0x221832u, 0x0C0810u } },
        { 0 } } },
};

#define WX_CAP_PER 4.0f /* a layer's pool: its count times this (amount 3 x the double's 1.8 is 5.4: past the pool, no more) */
typedef struct WxPart
{
    float p[3];
    float seed, s2;
} WxPart;
typedef struct WxPool
{
    WxPart* part;
    uint32_t cap;
    int laid; /* scattered round the camera once */
} WxPool;
static WxPool g_wx_pool[WX_KINDS][WX_LAYERS];
static float g_wx_amt[WX_KINDS];    /* how much of each kind is in the air now, eased */
static float g_wx_double[WX_KINDS]; /* 0 single to 1 double, eased with it */
static double g_wx_clock;
static uint64_t g_wx_last_ns;
static uint32_t g_wx_seed = 0x2545F491u;

static float wx_rand(void)
{
    g_wx_seed ^= g_wx_seed << 13, g_wx_seed ^= g_wx_seed >> 17, g_wx_seed ^= g_wx_seed << 5;
    return (float)(g_wx_seed & 0xFFFFFFu) / 16777216.0f;
}

/* the weather the effects follow: weather.preview's, else the server's while a packet has given one; -1 none */
static int wx_weather(void)
{
    if (g_wx_set.preview >= 0)
        return g_wx_set.preview;
    return g_rain_weather_known ? (int)g_rain_weather : -1;
}

/* the kind a weather is (-1 none: clear, sunny, cloudy, fog, the rain's and the thunder's) */
static int wx_kind_of(int weather)
{
    switch (weather)
    {
    case 4: case 5: return 0;
    case 8: case 9: return 1;
    case 10: case 11: return 2;
    case 12: case 13: return 3;
    case 16: case 17: return 4;
    case 18: case 19: return 5;
    default: return -1;
    }
}

/* the zone has that weather in its model DAT (or it isn't known): none in a zone the server gives a weather it has no look for */
static int wx_zone_has(int weather)
{
    if (g_wx_set.preview >= 0 || !(g_rain_zone_weathers & 0x80000000u))
        return 1;
    return (g_rain_zone_weathers >> weather) & 1u;
}

static void wx_lay(WxPool* pool, const WxLayer* l, const float cam[3])
{
    const uint32_t cap = (uint32_t)((float)l->count * WX_CAP_PER);
    if (!pool->part)
    {
        pool->part = (WxPart*)calloc(cap, sizeof(WxPart));
        if (!pool->part)
            return;
        pool->cap = cap;
    }
    for (uint32_t i = 0; i < pool->cap; ++i)
    {
        WxPart* q = &pool->part[i];
        q->p[0] = cam[0] + (wx_rand() * 2.0f - 1.0f) * l->reach;
        q->p[1] = cam[1] + (wx_rand() * 2.0f - 1.0f) * l->height;
        q->p[2] = cam[2] + (wx_rand() * 2.0f - 1.0f) * l->reach;
        q->seed = wx_rand(), q->s2 = wx_rand();
    }
    pool->laid = 1;
}

static float wx_wrap(float v, float c, float r)
{
    const float d = v - c;
    if (d > r)
        return v - 2.0f * r * ceilf((d - r) / (2.0f * r));
    if (d < -r)
        return v + 2.0f * r * ceilf((-r - d) / (2.0f * r));
    return v;
}

/* the kinds eased toward the weather's, once a frame: in over 8 s, out over 6 */
static void wx_tick(float dt)
{
    const int weather = wx_weather();
    const int kind = g_wx_set.on && weather >= 0 && wx_zone_has(weather) ? wx_kind_of(weather) : -1;
    for (int k = 0; k < WX_KINDS; ++k)
    {
        const int dbl = kind == k && (weather & 1);
        const float want = kind == k ? g_wx_set.amount[k] * (dbl ? g_wx_kinds[k].more : 1.0f) : 0.0f;
        const float rate = want > g_wx_amt[k] ? 1.0f / 8.0f : 1.0f / 6.0f;
        const float step = dt * rate * fmaxf(1.0f, fabsf(want - g_wx_amt[k]));
        g_wx_amt[k] += g_wx_amt[k] < want ? fminf(step, want - g_wx_amt[k]) : -fminf(step, g_wx_amt[k] - want);
        if (kind == k)
            g_wx_double[k] += ((dbl ? 1.0f : 0.0f) - g_wx_double[k]) * fminf(dt * 0.25f, 1.0f);
    }
}

static uint32_t wx_colour(const float c[3], float a)
{
    const uint32_t ca = (uint32_t)(255.0f * saturate_f(a));
    return (ca << 24) | ((uint32_t)(saturate_f(c[0]) * 255.0f) << 16) | ((uint32_t)(saturate_f(c[1]) * 255.0f) << 8) |
           (uint32_t)(saturate_f(c[2]) * 255.0f);
}

/* one draw of v[0..n): blended over (SRCALPHA, INVSRCALPHA) in the fog, or added (SRCALPHA, ONE) out of it; the scene's depth
 * tested, not written */
static void wx_draw(const GfxScene* s, const float* v, uint32_t n, int add)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    static GfxDraw d;
    memset(&d, 0, sizeof d);
    d.vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.vs.el[GFX_R_DIFFUSE] = (GfxElem){ 1, 0, GFX_D3DCOLOR, 0 };
    d.u.offset[GFX_R_DIFFUSE] = 12;
    d.u.stride[0] = 16;
    d.data[0] = (void*)v, d.size[0] = n * 16;
    d.prim = GFX_TRIANGLELIST, d.count = n / 3;
    d.fs.nstages = 1;
    d.fs.st[0] = (GfxStage){ 2 /* SELECTARG1 */, 0 /* DIFFUSE */, 0, 1, 2, 0, 0, 1, 1, 0, 0, 2 };
    if (!add && s->fog[2] != 0.0f)
        d.fs.fog = 3, d.u.params[2] = s->fog[0], d.u.params[3] = s->fog[1];
    memcpy(d.u.fogcolor, s->fogcolor, sizeof d.u.fogcolor);
    for (int i = 0; i < 4; ++i)
        d.u.vp[i] = (float)s->vp[i], d.u.mat_d[i] = d.u.tfactor[i] = 1.0f;
    memcpy(d.vp, s->vp, sizeof d.vp);
    mat_mul(d.u.wvp, s->view, s->proj);
    memcpy(d.u.wv, s->view, 64);
    memcpy(d.u.wvit, I, 64);
    d.pipe.write_mask = 0x7;
    d.pipe.blend = 1, d.pipe.src = 5, d.pipe.dst = add ? 2 : 6, d.pipe.op = 1;
    d.depth.zenable = 1, d.depth.zwrite = 0, d.depth.zfunc = 4;
    d.cull = 1, d.fill = 3;
    gfx_draw(&d);
}

/* a round soft dot facing the camera (a fan of six, the middle `cc`, the rim clear), never under a pixel or so far off */
static uint32_t wx_dot(float* v, const float p[3], float r, const float* inv, const float eye[3], uint32_t cc, uint32_t cr)
{
    const float dx = p[0] - eye[0], dy = p[1] - eye[1], dz = p[2] - eye[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    const float w = r * (1.0f + dist * 0.02f);
    const float along[3] = { inv[4] * w, inv[5] * w, inv[6] * w }, across[3] = { inv[0] * w, inv[1] * w, inv[2] * w };
    fan_emit(v, p, along, across, 6, cc, cr);
    return 18;
}

/* a leaf: a narrow diamond turning over and over about its own axes as it tumbles; both ends a little clear */
static uint32_t wx_leaf(float* v, const WxPart* q, float r, float t, const float* inv, uint32_t cc, uint32_t cr)
{
    const float spin = t * (2.0f + 3.0f * q->s2) + q->seed * 6.2831853f, flip = t * (1.3f + 2.0f * q->seed) + q->s2 * 6.2831853f;
    const float ca = cosf(spin), sa = sinf(spin), cf = cosf(flip);
    float along[3], across[3];
    for (int i = 0; i < 3; ++i)
    {
        along[i] = (inv[i] * ca + inv[4 + i] * sa) * r;
        across[i] = (inv[4 + i] * ca - inv[i] * sa) * r * (0.12f + 0.38f * fabsf(cf));
    }
    fan_emit(v, q->p, along, across, 4, cc, cr);
    return 12;
}

/* the scene's light on the lit layers: the fog's colour stands for the sky's (a snowy noon's light grey, a night's dark blue) */
static void wx_light(const GfxScene* s, float out[3])
{
    const float lum = 0.3f * s->fogcolor[0] + 0.59f * s->fogcolor[1] + 0.11f * s->fogcolor[2];
    const float k = 0.3f + 0.7f * sqrtf(saturate_f(lum * 1.4f));
    for (int i = 0; i < 3; ++i)
        out[i] = k * (0.85f + 0.15f * (lum > 1e-3f ? saturate_f(s->fogcolor[i] / lum) : 1.0f));
}

/* one layer moved and laid into v: its count by how much of the kind is in the air. Returns the vertices */
static uint32_t wx_layer(float* v, uint32_t room, int k, int li, float dt, float t, const float* inv, const float light[3])
{
    const WxKind* kd = &g_wx_kinds[k];
    const WxLayer* l = &kd->layer[li];
    WxPool* pool = &g_wx_pool[k][li];
    const float eye[3] = { inv[12], inv[13], inv[14] };
    if (!pool->laid)
        wx_lay(pool, l, eye);
    if (!pool->part)
        return 0;
    float amt = g_wx_amt[k];
    if (k == 4) /* light: the auroras' colours (layer 0) the single's, stellar glare's gold (layer 1) the double's */
        amt *= li == 0 ? 1.0f - 0.75f * g_wx_double[k] : 0.35f + 0.65f * g_wx_double[k];
    uint32_t want = (uint32_t)((float)l->count * amt);
    if (want > pool->cap)
        want = pool->cap;
    /* the wind: a slow turn and gusts, the double weather's harder */
    const float ang = 0.7f + 0.5f * sinf(t * 0.017f) + 0.25f * sinf(t * 0.053f + 1.3f);
    const float gust = 1.0f + 0.35f * sinf(t * 0.31f) * sinf(t * 0.13f + 0.7f);
    const float ws = kd->wind * (1.0f + (kd->gust - 1.0f) * g_wx_double[k]) * gust * l->wind;
    const float wind[2] = { cosf(ang) * ws, sinf(ang) * ws };
    const float fade = saturate_f(amt); /* a kind coming or going: fainter as well as fewer */
    uint32_t n = 0;
    for (uint32_t i = 0; i < want && n + 18 <= room; ++i)
    {
        WxPart* q = &pool->part[i];
        const float ph = q->seed * 6.2831853f;
        const float fall = l->fall * (1.0f + l->fall_jit * (q->s2 * 2.0f - 1.0f));
        const float vel[3] = { wind[0] * (0.8f + 0.4f * q->s2) + l->sway * sinf(t * (0.7f + q->s2) + ph), fall,
                               wind[1] * (0.8f + 0.4f * q->s2) + l->sway * cosf(t * (0.6f + q->seed) + ph * 1.7f) };
        q->p[0] = wx_wrap(q->p[0] + vel[0] * dt, eye[0], l->reach);
        q->p[1] = wx_wrap(q->p[1] + vel[1] * dt, eye[1], l->height);
        q->p[2] = wx_wrap(q->p[2] + vel[2] * dt, eye[2], l->reach);
        const float dx = q->p[0] - eye[0], dy = q->p[1] - eye[1], dz = q->p[2] - eye[2];
        const float flat = sqrtf(dx * dx + dz * dz), dist = sqrtf(dx * dx + dy * dy + dz * dz);
        float a = l->alpha * fade;
        a *= 1.0f - saturate_f((flat - l->reach * 0.7f) / (l->reach * 0.3f)); /* the box's edge, where they wrap */
        a *= 1.0f - saturate_f((fabsf(dy) - l->height * 0.75f) / (l->height * 0.25f));
        a *= saturate_f((dist - 0.4f) / (0.6f + l->size * 2.0f)); /* none on the lens */
        if (l->twinkle > 0.0f)
            a *= 1.0f - l->twinkle * (0.5f + 0.5f * sinf(t * l->rate * (0.6f + 0.8f * q->s2) * 6.2831853f + ph));
        if (a < 0.004f)
            continue;
        const float roof = cover_at(q->p[0], q->p[2], eye[1], 0);
        if (roof < COVER_SKY && q->p[1] > roof - COVER_EPS) /* under a roof: none */
            continue;
        const uint32_t pick = l->col[(uint32_t)(q->s2 * 2.999f)];
        float c[3] = { (float)((pick >> 16) & 255u) / 255.0f, (float)((pick >> 8) & 255u) / 255.0f, (float)(pick & 255u) / 255.0f };
        if (l->lit)
            c[0] *= light[0], c[1] *= light[1], c[2] *= light[2];
        const float r = l->size * (1.0f + l->size_jit * (q->seed * 2.0f - 1.0f));
        const uint32_t cc = wx_colour(c, a), cr = wx_colour(c, 0.0f);
        float* o = &v[n * 4];
        if (l->shape == WX_STREAK)
        {
            const float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
            n += 6 * (uint32_t)quad_emit(o, q->p, vel, r, fmaxf(r * 4.0f, sp * l->stretch), eye, cc, cr);
        }
        else if (l->shape == WX_LEAF)
            n += wx_leaf(o, q, r, t, inv, cc, wx_colour(c, a * 0.6f));
        else
            n += wx_dot(o, q->p, r, inv, eye, cc, cr);
    }
    return n;
}

/* After the rain's pass (d3d8.c scene end): the kinds in the air moved and drawn, the blended layers then the glowing ones */
#define WX_FLOATS (1u << 21) /* 2M floats: 131 thousand dots' vertices */
static void weather_pass(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    const uint64_t now = gfx_now_ns();
    double dt = g_wx_last_ns ? (double)(now - g_wx_last_ns) * 1e-9 : 0.0;
    g_wx_last_ns = now;
    if (dt > 0.25 || g_rain_set.freeze) /* a pause or a zone change; rain.freeze holds these still too */
        dt = 0.0;
    g_wx_clock = fmod(g_wx_clock + dt, 3600.0);
    wx_tick((float)dt);
    int any = 0;
    for (int k = 0; k < WX_KINDS; ++k)
        any |= g_wx_amt[k] > 0.001f;
    if (!any || !color || !g_scene.cam)
        return;
    float inv[16];
    if (!water_affine_inv(inv, s->view))
        return;
    if (!(g_rain_fall > 0.001f || g_rain_wet > 0.0f) || !g_rain_set.rain) /* the rain's pass lays the shelter map while it rains */
        cover_update(inv[12], inv[13], inv[14]);
    gfx_set_targets(color, 0, 0, depth);
    static float v[WX_FLOATS];
    float light[3];
    wx_light(s, light);
    const float t = (float)g_wx_clock;
    uint32_t n = 0, n_add = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
            n_add = n;
        for (int k = 0; k < WX_KINDS; ++k)
        {
            if (g_wx_amt[k] <= 0.001f)
            {
                for (int li = 0; li < WX_LAYERS; ++li) /* gone: laid afresh round the camera next time */
                    g_wx_pool[k][li].laid = 0;
                continue;
            }
            for (int li = 0; li < WX_LAYERS; ++li)
            {
                const WxLayer* l = &g_wx_kinds[k].layer[li];
                if (l->count && (int)l->add == pass)
                    n += wx_layer(&v[n * 4], WX_FLOATS / 4 - n, k, li, (float)dt, t, inv, light);
            }
        }
    }
    if (n_add)
        wx_draw(s, v, n_add, 0);
    if (n > n_add)
        wx_draw(s, &v[n_add * 4], n - n_add, 1);
}
