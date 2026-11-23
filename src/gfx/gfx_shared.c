/* gfx_shared.h: what both back ends' scene effects do the same way. */
#define COBJMACROS
#include "gfx_shared.h"

#include <windows.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gfx_player_light(const GfxScene* s, float strength, float radius, float warm, float day, float plight[4], float plight2[4])
{
    memset(plight, 0, 4 * sizeof(float));
    memset(plight2, 0, 4 * sizeof(float));
    const float on = fminf(fmaxf(strength, 0.0f), 2.0f), n = fminf(fmaxf(s->night, 0.0f), 1.0f), k = on * n;
    /* by day (and indoors) a lamp adds nothing a reader sees on ground the sun already lights: there the pool shows by what is
     * round it going darker, up to 0.9 of its light off at 1, less as the night comes in. Day is the clock's (night_clock), not
     * Night's: with Night off at night the world was darkened round the player as by day (2026-11-05, the grass dropped by half
     * when Night was switched off at 03:12); now there's neither the lamp nor the darkening */
    const float nc = fminf(fmaxf(s->night_clock, 0.0f), 1.0f);
    const float dim = on > 0.0f ? 0.9f * fminf(fmaxf(day, 0.0f), 1.0f) * (1.0f - nc) : 0.0f;
    if (!(k > 0.0f || dim > 0.0f) || s->focus[3] == 0.0f)
        return;
    const float* v = s->view;
    const float* f = s->focus;
    for (int i = 0; i < 3; ++i)
        plight[i] = f[0] * v[i] + f[1] * v[4 + i] + f[2] * v[8 + i] + v[12 + i];
    const float r = fminf(fmaxf(radius, 2.0f), 30.0f), w = fminf(fmaxf(warm, 0.0f), 1.0f);
    plight[3] = r;
    /* 2.5 at 1: about the light the night took away (d3d8_night.h: the ambient to a quarter, the moon's to two thirds) given
     * back under the lamp; warm leans it to a torch's orange */
    const float gain = 2.5f * k, tint[3] = { 1.0f, 1.0f - 0.22f * w, 1.0f - 0.48f * w };
    for (int i = 0; i < 3; ++i)
        plight2[i] = gain * tint[i];
    plight2[3] = dim;
}

void gfx_grass_lines(float strength, float width, float depth, float fade, float vh, const float toon[4], const float toon2[4],
    float toon4[4])
{
    memset(toon4, 0, 4 * sizeof(float));
    if (!(strength > 0.0f))
        return;
    const float lw = width * vh / 1080.0f;
    toon4[0] = fminf(strength, 1.0f);
    toon4[1] = width > 0.0f ? fmaxf(lw, 1.0f) : toon[1];
    toon4[2] = depth > 0.0f ? fmaxf(depth, 1e-3f) : toon[2];
    toon4[3] = fade > 0.0f ? fade : toon2[0];
}

static float g_pool[16]; /* gfx_player_pool's; g_pool[3] (the radius) 0: none this frame */
static float g_hfog[16]; /* gfx_height_fog's; g_hfog[3] (the density) 0: none this frame */

void gfx_player_pool_keep(const float plight[4], const float plight2[4], const float up[3])
{
    memset(g_pool, 0, sizeof g_pool);
    if (plight[3] <= 0.0f)
        return;
    memcpy(g_pool, plight, 4 * sizeof(float));
    memcpy(g_pool + 4, plight2, 4 * sizeof(float));
    memcpy(g_pool + 8, up, 3 * sizeof(float));
}

void gfx_player_pool_forget(void) { g_pool[3] = 0.0f, g_hfog[3] = 0.0f; }

void gfx_height_fog_keep(const float fogc[4], const float fogp[4], const float up[3], const float sun[4], const float suncol[3])
{
    memset(g_hfog, 0, sizeof g_hfog);
    if (!(fogc[3] > 0.0f))
        return;
    const float glow = sun[3] * fogp[2];
    memcpy(g_hfog, fogc, 4 * sizeof(float));
    g_hfog[4] = fogp[0], g_hfog[5] = fogp[1], g_hfog[6] = fogp[3], g_hfog[7] = suncol[0] * glow;
    g_hfog[8] = up[0], g_hfog[9] = up[1], g_hfog[10] = up[2], g_hfog[11] = suncol[1] * glow;
    g_hfog[12] = sun[0], g_hfog[13] = sun[1], g_hfog[14] = sun[2], g_hfog[15] = suncol[2] * glow;
}

int gfx_height_fog(float m[16])
{
    if (!(g_hfog[3] > 0.0f))
        return 0;
    memcpy(m, g_hfog, sizeof g_hfog);
    return 1;
}

int gfx_player_pool(float m[16])
{
    if (g_pool[3] <= 0.0f)
        return 0;
    memcpy(m, g_pool, sizeof g_pool);
    return 1;
}

int gfx_dlss_cpu_sync_wanted(void)
{
    char v[16];
    const DWORD n = GetEnvironmentVariableA("VANA_DLSS_FENCE", v, sizeof v);
    if (n && n < sizeof v)
    {
        if (!_stricmp(v, "cpu"))
            return 1;
        if (!_stricmp(v, "gpu"))
            return 0;
    }
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL;
}

/* --- the sun's glide ------------------------------------------------------------------------------------------------- */
void gfx_sun_glide(GfxSunGlide* g, float at[3], const float* w, int now)
{
    if (now || w[0] * g->to[0] + w[1] * g->to[1] + w[2] * g->to[2] < 0.995f)
    {
        memcpy(at, w, 12), memcpy(g->from, w, 12), memcpy(g->to, w, 12);
        g->p = 1.0f;
        return;
    }
    memcpy(g->from, at, 12), memcpy(g->to, w, 12);
    g->p = 0.0f;
}

void gfx_sun_glide_on(GfxSunGlide* g, float at[3])
{
    /* by time, not frames: upstream's 150 frames are 2.5 s at 60 a second, about the game's 2.4 s between steps. Counted
     * in frames, at 30 a second the glide never caught up, and at 144 it held still most of the time again */
    const uint64_t now = gfx_now_ns();
    const double dt = g->was_ns && now > g->was_ns ? fmin((double)(now - g->was_ns) * 1e-9, 0.1) : 1.0 / 60.0;
    g->was_ns = now;
    if (!(g->p < 1.0f))
        return;
    g->p = fminf(g->p + (float)(dt / 2.5), 1.0f);
    for (int j = 0; j < 3; ++j)
        at[j] = g->from[j] + (g->to[j] - g->from[j]) * g->p;
    const float l = sqrtf(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]);
    if (l > 0.0f)
        at[0] /= l, at[1] /= l, at[2] /= l;
}

/* --- the shadows round the light's turns at 06:00 and 18:00 --------------------------------------------------------------- */
/* 17:30 to 18:30: the sun's shadows going out by 18:00 and the moon's coming in after it, a smoothstep down and up again. The
 * game's light turns 180 degrees, sun to moon, at 18:00 (d3d8_sun.c clock_pick: not before the clock says, nor before the moon
 * is voted for, a few seconds after at most), and sun_dusk holds both over the horizon, so the long shadows swung across the
 * ground in a frame; they now turn while there are none (0.005 of their strength 1.25 Vana'diel minutes, 3 seconds, after). 1
 * outside it, and where the minute isn't known */
static float dusk_ramp(float minute)
{
    const float turn = 1080.0f, half = 30.0f; /* 18:00, half an hour each side */
    const float t = fabsf(minute - turn) / half;
    return t < 1.0f ? t * t * (3.0f - 2.0f * t) : 1.0f;
}

float gfx_dawn(float minute, float* ramp)
{
    const float from = 330.0f, up = 360.0f, over = 366.0f, full = 390.0f; /* 05:30, 06:00, 06:06, 06:30 */
    *ramp = dusk_ramp(minute);
    if (!(minute > from && minute < full))
        return 0.0f;
    const float t = (minute - from) / (full - from);
    *ramp = t * t * (3.0f - 2.0f * t);
    if (minute >= over)
        return 0.0f;
    return GFX_DAWN_DAY * (minute < up ? 1.0f : 1.0f - (minute - up) / (over - up));
}

GfxDawn gfx_dawn_sun(float minute, float low, float wy, float sw[3], float* height)
{
    GfxDawn d;
    d.day = gfx_dawn(minute, &d.ramp);
    if (d.day > 0.0f)
        sw[0] = sqrtf(1.0f - low * low), sw[1] = low * wy, sw[2] = 0.0f, *height = low;
    return d;
}

void gfx_sun_shadow_dir(float out[3], const float sw[3], const float* view)
{
    for (int j = 0; j < 3; ++j)
        out[j] = sw[0] * view[j] + sw[1] * view[4 + j] + sw[2] * view[8 + j];
    const float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > 0.0f)
        out[0] /= l, out[1] /= l, out[2] /= l;
}

/* the cloud's share of the light, sun_direct: the sun's luminance over the light's, eased frame to frame; each as the world's lit
 * draws have it, raised by Bright night, Sun strength and Indoor brightness (GfxScene.brighten) */
static void sun_direct_measure(const GfxSunLightIn* in, GfxSunLight* o)
{
    const GfxScene* s = in->s;
    o->dl = 0.2126f * s->sun_color[0] + 0.7152f * s->sun_color[1] + 0.0722f * s->sun_color[2];
    o->al = 0.2126f * s->ambient[0] + 0.7152f * s->ambient[1] + 0.0722f * s->ambient[2];
    o->dl *= 1.0f + fmaxf(s->brighten[0], 0.0f), o->al *= 1.0f + fmaxf(s->brighten[1], 0.0f);
    const float r = o->dl / fmaxf(o->dl + o->al, 1e-3f);
    const float want = fminf(fmaxf((r - in->sun_direct) / 0.3f, 0.0f), 1.0f);
    o->direct = in->eased && o->direct >= 0.0f ? o->direct + (want - o->direct) * 0.05f : want;
}

/* [graphics.effects] sun_tilt: the shadows' light turned that many degrees toward the south (the world's -z) about the east-west
 * axis, as a sun's path is at a latitude. The game's sun and moon run exactly through the world's x-y plane, east to overhead to
 * west, so every wall facing north or south stood edge-on to them all day and night: a line in the shadow maps, lit or shaded by
 * less than a texel and by the sign of a light's z of a few millionths, which flipped from frame to frame - those walls flickered
 * as the sun stepped, worst at low angles (Ru'Lude Gardens' palace hall, 2026-11-04). Tilted, a south wall faces the light and a
 * north one is turned from it, steadily. 0: as the game has it. Not a sun placed by hand (sun_free) */
static void sun_tilt(float sw[3], float wy, float degrees, float* height)
{
    if (!(degrees != 0.0f))
        return;
    const float t = fminf(fmaxf(degrees, -45.0f), 45.0f) * 0.017453293f, c = cosf(t), sn = sinf(t);
    const float up = sw[1] * wy, north = sw[2];
    const float up2 = up * c + north * sn, north2 = north * c - up * sn;
    sw[1] = up2 * wy, sw[2] = north2;
    *height = up2;
}

/* [graphics.effects] sun_moon 0, "Shadows at night" off: how much of this frame's shadows are the moon's, to be taken out. The
 * clock's night is the moon's (d3d8_sun.c clock_body: the sun from 06:00 to 18:00): from 18:00, where sun_dusk's ramp has them
 * out already as the game's light turns to the moon, until 05:30, where sun_dusk brings the sun's in from nothing (06:00 without
 * sun_dusk, the sun's height bringing them in), so neither end is a step. Outdoors only: under a zone's fixed light or in a Mog
 * House (GfxScene.indoors) the light isn't the moon, and moghouse says. 0 by day or with the clock not known */
static float moon_shadows(const GfxScene* s, float sun_dusk)
{
    const float m = s->vana_minute;
    if (!(m > 0.0f) || (m < 1080.0f && m >= (sun_dusk != 0.0f ? 330.0f : 360.0f)))
        return 0.0f;
    return 1.0f - fminf(fmaxf(s->indoors, 0.0f), 1.0f);
}

float gfx_light_turn(float minute)
{
    if (!(minute > 0.0f))
        return 1.0f;
    float t = 1.0f;
    for (int k = 0; k < 2; ++k)
        t = fminf(t, fabsf(minute - (k ? 1080.0f : 360.0f)) / GFX_TURN_HALF);
    return t * t * (3.0f - 2.0f * t);
}

/* "Shadows at night" on (sun_moon, the default): the shadows don't fade round the night: the moon's shadows go on till 06:00 and
 * the sun's from then, and the two turns of the game's light, 180 degrees at 06:00 and at 18:00, are hidden by a dip either side
 * (gfx_light_turn), so the shadows don't swing across the ground in one frame; with sun_dusk 0 (minute 0 here) the light is at the
 * horizon then and there's nothing to hide. The dip was 2 Vana'diel minutes either side (some 10 s in all) after one game found no
 * shadows at 05:40 (2026-11-06: the moon's gone by the cloud's share, the sun's at 0.04 of their strength); in that 10 s the
 * moon's shadows went, swung round and the sun's came, in steps of the clock's seconds, "super abrupt" (2026-11-13). It's half an
 * hour either side now, 05:30 to 06:30 and 17:30 to 18:30 - three-quarters of their strength at 05:40 - as the game's own light
 * is eased round the same turns (d3d8_sun.c). GfxDawn's day 0, its ramp that dip */
static GfxDawn night_turns(float minute)
{
    GfxDawn d = { 0.0f, gfx_light_turn(minute) };
    return d;
}

/* ... and how much of the cloud's share (sun_direct) is taken: all of it by day, none through the night (17:30 to 06:30), eased
 * over the half hour either side. From 05:00 the game's moon goes out (its colour reaches nothing), so the share took the moon's
 * shadows to nothing an hour before dawn; through the night the moon's light is weak beside the ambient anyway */
static float cloud_share(float minute)
{
    if (!(minute > 0.0f))
        return 1.0f;
    const float t = fminf(fmaxf(minute < 720.0f ? (minute - 390.0f) / 30.0f : (1050.0f - minute) / 30.0f, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void gfx_sun_light(const GfxSunLightIn* in, GfxSunLight* o)
{
    const GfxScene* s = in->s;
    const int all_night = in->sun_moon != 0.0f; /* "Shadows at night" on: no fading round the night (night_turns, cloud_share) */
    float* sw = o->sw;
    sw[0] = in->sunw[0], sw[1] = in->sunw[1], sw[2] = in->sunw[2];
    /* world up is the world's y (or its reverse): the way the eased up points */
    const float wy = in->up[0] * s->view[4] + in->up[1] * s->view[5] + in->up[2] * s->view[6] < 0.0f ? -1.0f : 1.0f;
    float height = sw[1] * wy, across = sqrtf(sw[0] * sw[0] + sw[2] * sw[2]);
    o->free_sun = in->sun_free != 0.0f;
    if (o->free_sun)
    {
        const float az = in->sun_free_az * 0.017453293f, el = fminf(fmaxf(in->sun_free_el, -89.0f), 90.0f) * 0.017453293f;
        sw[0] = cosf(el) * cosf(az), sw[1] = sinf(el) * wy, sw[2] = cosf(el) * sinf(az);
        height = sinf(el), across = cosf(el);
    }
    else if (in->sun_dusk != 0.0f && height < GFX_DUSK_LOW && across > 1e-3f)
    {
        const float k = sqrtf(1.0f - GFX_DUSK_LOW * GFX_DUSK_LOW) / across;
        sw[0] *= k, sw[1] = GFX_DUSK_LOW * wy, sw[2] *= k;
        o->day = fminf(fmaxf((height - GFX_DUSK_GONE) / 0.15f, 0.0f), 1.0f);
    }
    GfxDawn dawn = { 0.0f, 1.0f };
    if (!o->free_sun)
    {
        const float minute = s->vana_minute * (in->sun_dusk != 0.0f);
        dawn = all_night ? night_turns(minute) : gfx_dawn_sun(minute, GFX_DUSK_LOW, wy, sw, &height); /* round sunrise */
        sun_tilt(sw, wy, in->sun_tilt, &height);
    }
    o->height = height;
    if (in->measured)
        sun_direct_measure(in, o);
    const float cloud = all_night ? cloud_share(s->vana_minute) : 1.0f;
    o->day *= in->sun_direct > 0.0f ? 1.0f - cloud * (1.0f - o->direct) : 1.0f;
    o->day = fmaxf(o->day, dawn.day) * dawn.ramp;
    if (in->sun_moon == 0.0f)
        o->day *= 1.0f - moon_shadows(s, in->sun_dusk);
    /* in a Mog House, or under a zone's fixed light (GfxScene.indoors, easing out once the sun moves): the room's ceiling would
     * shade everything in it, so the sun's shadows there are scaled by moghouse (from XI on Anything, rubymatrix/xi-on-anything) */
    const float inside = fminf(fmaxf(s->indoors, 0.0f), 1.0f) * (1.0f - fminf(fmaxf(in->moghouse, 0.0f), 1.0f));
    o->day *= 1.0f - inside;
    if (o->free_sun)
        o->day = 1.0f; /* a sun placed by hand shades at full strength, whatever the hour, the cloud or the room */
}

/* --- the shader tokens kept ------------------------------------------------------------------------------------------ */
/* An open table by program and kind (prog << 1 | pixel), each the tokens' copy: grown at half full */
typedef struct
{
    uint64_t key; /* 0: empty (a program's hash is never 0: prog 0 is the fixed function, not kept) */
    uint32_t* tokens;
} KeptTokens;

static KeptTokens* g_kept;
static uint32_t g_kept_cap, g_kept_n;

static uint32_t kept_slot(const KeptTokens* table, uint32_t cap, uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    uint32_t i = (uint32_t)(h >> 32) & (cap - 1);
    while (table[i].key && table[i].key != key)
        i = (i + 1) & (cap - 1);
    return i;
}

static int kept_grow(void)
{
    const uint32_t cap = g_kept_cap ? g_kept_cap * 2 : 256;
    KeptTokens* table = (KeptTokens*)calloc(cap, sizeof *table);
    if (!table)
        return 0;
    for (uint32_t i = 0; i < g_kept_cap; ++i)
        if (g_kept[i].key)
            table[kept_slot(table, cap, g_kept[i].key)] = g_kept[i];
    free(g_kept);
    g_kept = table, g_kept_cap = cap;
    return 1;
}

/* a copy of the tokens, through the end token (0x0000FFFF) */
static uint32_t* copy_tokens(const uint32_t* t)
{
    uint32_t k = 0;
    while (k < 65536 && t[k] != 0x0000FFFFu)
        k++;
    k++;
    uint32_t* c = (uint32_t*)malloc(4u * k);
    if (c)
        memcpy(c, t, 4u * k);
    return c;
}

const uint32_t* gfx_tokens_kept(uint32_t prog, int pixel, const uint32_t* t)
{
    static uint32_t last_prog[2];
    static const uint32_t* last[2];
    if (!prog || !t)
        return t;
    pixel = pixel != 0;
    if (last[pixel] && last_prog[pixel] == prog)
        return last[pixel];
    const uint64_t key = (uint64_t)prog << 1 | (uint64_t)pixel;
    if ((g_kept_n + 1) * 2 > g_kept_cap && !kept_grow())
        return t;
    KeptTokens* e = &g_kept[kept_slot(g_kept, g_kept_cap, key)];
    if (!e->key)
    {
        uint32_t* c = copy_tokens(t);
        if (!c)
            return t;
        e->key = key, e->tokens = c, ++g_kept_n;
    }
    last_prog[pixel] = prog, last[pixel] = e->tokens;
    return e->tokens;
}

/* --- the shadow casters' copies -------------------------------------------------------------------------------------- */
int gfx_caster_copy_ok(uint32_t n, uint64_t serial)
{
    static uint64_t copied_serial;
    static uint32_t copied;
    if (copied_serial != serial)
        copied_serial = serial, copied = 0;
    if (n > GFX_CASTER_COPY_FRAME || copied + n > GFX_CASTER_COPY_FRAME)
        return 0;
    copied += n;
    return 1;
}

/* --- FSR 1 ------------------------------------------------------------------------------------------------------------ */
int gfx_fsr1_sizes(const GfxScene* s, uint32_t cw, uint32_t ch, float factor, float vp[4], uint32_t* in_w, uint32_t* in_h,
    uint32_t* out_w, uint32_t* out_h)
{
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > cw || vy + vh > ch)
        vx = vy = 0, vw = (float)cw, vh = (float)ch;
    *in_w = (uint32_t)vw, *in_h = (uint32_t)vh;
    if (s->present_w && s->present_h)
        *out_w = (uint32_t)(vw * s->present_w / cw + 0.5f), *out_h = (uint32_t)(vh * s->present_h / ch + 0.5f);
    else
        *out_w = (uint32_t)(vw / factor + 0.5f), *out_h = (uint32_t)(vh / factor + 0.5f);
    vp[0] = vx, vp[1] = vy, vp[2] = vw, vp[3] = vh;
    if (*out_w < *in_w || *out_h < *in_h || *out_w > 16384 || *out_h > 16384)
        return 0;
    if (*in_w >= *out_w && factor < 0.99f)
        return 0;
    return 1;
}

/* --- the card Direct3D 12 was on --------------------------------------------------------------------------------------- */
static struct
{
    uint32_t low;
    int32_t high;
    uint32_t vendor, device, subsys;
    int known;
    char said[600]; /* the adapters gfx_card_find last put in the log, so a try every half second says each list once */
} g_card;

void gfx_card_remember(uint32_t low, int32_t high, uint32_t vendor, uint32_t device, uint32_t subsys)
{
    g_card.low = low, g_card.high = high, g_card.known = 1;
    g_card.vendor = vendor, g_card.device = device, g_card.subsys = subsys;
    g_card.said[0] = 0;
}

int gfx_card_known(void) { return g_card.known; }

void gfx_card_forget(void) { g_card.known = 0; }

void* gfx_card_find(void* factory)
{
    IDXGIFactory1* f = (IDXGIFactory1*)factory;
    if (!g_card.known || !f)
        return NULL;
    IDXGIAdapter1* by_luid = NULL;
    IDXGIAdapter1* by_make = NULL;
    IDXGIAdapter1* a = NULL;
    char seen[sizeof g_card.said] = "";
    size_t used = 0;
    for (UINT i = 0; SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, i, &a)); ++i)
    {
        DXGI_ADAPTER_DESC1 d;
        if (FAILED(IDXGIAdapter1_GetDesc1(a, &d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            IDXGIAdapter1_Release(a);
            continue;
        }
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
        if (used < sizeof seen)
        {
            const int n = snprintf(seen + used, sizeof seen - used, "%s%s (vendor %04X, device %04X, LUID %08lX:%08lX)", used ? "; " : "",
                name, d.VendorId, d.DeviceId, (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
            used += n > 0 ? (size_t)n : 0;
        }
        if (!by_luid && d.AdapterLuid.LowPart == g_card.low && d.AdapterLuid.HighPart == g_card.high)
            by_luid = a;
        else if (!by_make && d.VendorId == g_card.vendor && d.DeviceId == g_card.device && d.SubSysId == g_card.subsys)
            by_make = a;
        else
            IDXGIAdapter1_Release(a);
    }
    if (by_luid)
    {
        if (by_make)
            IDXGIAdapter1_Release(by_make);
        return by_luid;
    }
    if (strcmp(seen, g_card.said) != 0)
    {
        snprintf(g_card.said, sizeof g_card.said, "%s", seen);
        if (by_make)
            fprintf(stderr, "[recomp] gfx: the graphics card the device was on is back under a new LUID; the adapters Windows lists: %s\n",
                seen[0] ? seen : "none");
        else
            fprintf(stderr, "[recomp] gfx: the graphics card the device was on (vendor %04X, device %04X, LUID %08lX:%08lX) isn't "
                            "listed (yet); the adapters Windows lists: %s\n",
                g_card.vendor, g_card.device, (unsigned long)g_card.high, (unsigned long)g_card.low, seen[0] ? seen : "none");
    }
    return by_make;
}

/* --- the scene effects' shader build, watched (gfx_shared.h) ------------------------------------------------------ */
static uint64_t g_fxb_start_ns;
static int g_fxb_wine = -1, g_fxb_warned;

int gfx_hlsl_compiler_is_wine(void)
{
    if (g_fxb_wine >= 0)
        return g_fxb_wine;
    g_fxb_wine = 0;
    HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
    if (!m)
        m = LoadLibraryA("d3dcompiler_47.dll");
    if (!m)
        return 0;
    /* winebuild writes "Wine builtin DLL" into the DOS stub of every DLL it builds, after the DOS header */
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < (LONG)sizeof *dos || dos->e_lfanew > 0x400)
        return 0;
    static const char MARK[] = "Wine builtin DLL";
    const char* p = (const char*)m + sizeof *dos;
    for (LONG n = dos->e_lfanew - (LONG)sizeof *dos - (LONG)(sizeof MARK - 1); n >= 0; --n, ++p)
        if (!memcmp(p, MARK, sizeof MARK - 1))
            return g_fxb_wine = 1;
    return 0;
}

void gfx_fx_build_begin(const char* backend)
{
    g_fxb_start_ns = gfx_now_ns();
    g_fxb_warned = 0;
    if (gfx_hlsl_compiler_is_wine())
        fprintf(stderr, "[recomp] gfx: the HLSL compiler is Wine's own d3dcompiler_47 (vkd3d-shader's): the scene effects' shaders (%s) take "
                        "minutes to build on it, and until they're built the effects draw nothing (no shadows, occlusion, grade or zone lines). "
                        "Microsoft's takes seconds: winetricks d3dcompiler_47 puts it in the prefix\n", backend);
}

void gfx_fx_build_tick(void)
{
    if (g_fxb_warned || !g_fxb_start_ns)
        return;
    const uint64_t now = gfx_now_ns();
    if (now < g_fxb_start_ns + 20ull * 1000000000ull)
        return;
    g_fxb_warned = 1;
    fprintf(stderr, "[recomp] gfx: the scene effects' shaders are still building after %.0f s: no shadows, occlusion, grade or zone lines "
                    "until they're built%s\n", (double)(now - g_fxb_start_ns) * 1e-9,
            gfx_hlsl_compiler_is_wine() ? " (Wine's d3dcompiler_47 is slow at them: winetricks d3dcompiler_47 puts Microsoft's in the prefix)" : "");
}

void gfx_fx_build_end(int ok)
{
    const double took = g_fxb_start_ns ? (double)(gfx_now_ns() - g_fxb_start_ns) * 1e-9 : 0.0;
    fprintf(stderr, ok ? "[recomp] gfx: scene effects ready (built in %.1f s)\n" : "[recomp] gfx: scene effects failed to build after %.1f s; off\n", took);
}

/* --- the start-up rebuild's thread pool (gfx_shared.h), made once, kept for the process ---------------------------- */
static PTP_POOL g_warm_pool;
static TP_CALLBACK_ENVIRON g_warm_env;
static DWORD g_warm_n;
static INIT_ONCE g_warm_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK warm_pool_make(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const DWORD n = si.dwNumberOfProcessors / 2;
    g_warm_n = n < 1 ? 1 : n > 4 ? 4 : n;
    g_warm_pool = CreateThreadpool(NULL);
    if (!g_warm_pool)
        return TRUE;
    SetThreadpoolThreadMaximum(g_warm_pool, g_warm_n);
    InitializeThreadpoolEnvironment(&g_warm_env);
    SetThreadpoolCallbackPool(&g_warm_env, g_warm_pool);
    return TRUE;
}

void* gfx_warm_env(void)
{
    InitOnceExecuteOnce(&g_warm_once, warm_pool_make, NULL, NULL);
    return g_warm_pool ? &g_warm_env : NULL;
}

unsigned gfx_warm_threads(void) { return gfx_warm_env() ? (unsigned)g_warm_n : 0; }
