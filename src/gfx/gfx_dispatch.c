/* gfx.h's device functions, going to the back end gfx_select chose (gfx_backend.h): Direct3D 12 unless
 * renderer=d3d11 asked for Direct3D 11. One call through a table each: nothing next to a draw's cost.
 *
 * One thing is done here for both back ends: small screen-space quads drawn one after another with the same
 * state go on as one draw (quads_flush). FFXI draws its text a glyph at a time, one DrawPrimitiveUP of a
 * two-triangle strip each: a page of battle text (a bard's songs landing on a party) is 400-800 draws a frame
 * for as long as the lines show, and a back end's cost per draw - the pipeline, the descriptors, the uniforms'
 * copy, the vertices' upload - is the same for four vertices as for thousands. Every other call flushes what
 * is kept first, so the back end sees the draws it would have seen, in their order. */
#include "gfx_backend.h"
#include "zonemesh.h" /* g_gfx_zone_mesh: [graphics.effects] sun_zone_mesh */
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const GfxBackend* g_backend = &gfx12_backend;

/* --- small screen-space quads kept for one draw ---------------------------------------------------------- */
#define QUAD_MAX 4096     /* quads in one draw */
#define QUAD_MAX_PX 64.0f /* a quad wider or taller than this (a window's back, the scene copied) is drawn on its own */
static int g_quads_on = 0; /* [graphics.effects] textbatch (off until it has been proven in play) */
static GfxDraw g_quad;     /* the kept draw: the first quad's state, the vertices put in at the flush */
static uint8_t* g_quad_bytes;
static uint32_t g_quad_cap, g_quad_n, g_quad_stride;
static uint32_t g_quads_drawn, g_quads_merged;           /* this frame's count: quads kept, draws they went out as */
static uint32_t g_quads_drawn_last, g_quads_merged_last; /* ... the last whole frame's */
/* the targets as last set (d3d8.c sets them before every draw, mostly to what they are: only a change flushes, or the
 * kept quads would go out one at a time) */
static GfxTex *g_target_color, *g_target_depth;
static uint32_t g_target_face, g_target_level;
static int g_target_known;

/* The chat's glyphs with a shadow (GfxDraw.text_shadow): kept as the quads are, whatever textbatch says, with their
 * shadows' quads beside them; at the flush every shadow goes first and the glyphs over them, so no glyph's shadow
 * falls on the glyph before it */
static GfxDraw g_text;
static uint8_t *g_text_bytes, *g_text_shadow_bytes;
static uint32_t g_text_cap, g_text_shadow_cap, g_text_n, g_text_stride, g_text_offsets;

/* A two-triangle strip of transformed vertices, from memory the draw brought (not a buffer), at most QUAD_MAX_PX
 * on a side: a glyph, an icon, a gauge's piece. The vertex bytes are the game's, read now (they are gone after the
 * call). */
static int quad_shaped(const GfxDraw* d)
{
    if (!d->vs.rhw || d->prim != GFX_TRIANGLESTRIP || d->count != 2 || d->indices || d->ibuf ||
        d->vertex_start || d->u.vofs || d->caster || !d->data[0] || d->buf[0])
        return 0;
    for (int s = 1; s < GFX_NSTREAMS; ++s)
        if (d->data[s] || d->buf[s])
            return 0;
    const uint32_t stride = (uint32_t)d->u.stride[0], off = (uint32_t)d->u.offset[GFX_R_POSITION];
    const GfxElem* pos = &d->vs.el[GFX_R_POSITION];
    if (!pos->used || pos->stream || stride < 16 || stride > 256 || off + 8 > stride || d->size[0] < 4 * stride)
        return 0;
    float xlo = 1e30f, xhi = -1e30f, ylo = 1e30f, yhi = -1e30f;
    for (uint32_t i = 0; i < 4; ++i)
    {
        float x, y;
        memcpy(&x, (const uint8_t*)d->data[0] + i * stride + off, 4);
        memcpy(&y, (const uint8_t*)d->data[0] + i * stride + off + 4, 4);
        xlo = x < xlo ? x : xlo, xhi = x > xhi ? x : xhi, ylo = y < ylo ? y : ylo, yhi = y > yhi ? y : yhi;
    }
    /* (written so that a NaN fails) */
    return xhi - xlo <= QUAD_MAX_PX && yhi - ylo <= QUAD_MAX_PX && xhi - xlo >= 0.0f && yhi - ylo >= 0.0f;
}

static int quad_mergeable(const GfxDraw* d) { return g_quads_on && quad_shaped(d); }

/* The same state: everything a draw carries but its vertices (data, size, cpu) and its primitive. The keys are
 * zeroed then filled (gfx.h), the samplers padded by hand, GfxU all numbers: byte compares hold. */
static int quad_same(const GfxDraw* a, const GfxDraw* b)
{
    return !memcmp(&a->vs, &b->vs, sizeof a->vs) && !memcmp(&a->fs, &b->fs, sizeof a->fs) &&
        !memcmp(&a->pipe, &b->pipe, sizeof a->pipe) && !memcmp(&a->depth, &b->depth, sizeof a->depth) &&
        a->vs_tokens == b->vs_tokens && a->ps_tokens == b->ps_tokens && !memcmp(&a->u, &b->u, sizeof a->u) &&
        !memcmp(a->tex, b->tex, sizeof a->tex) && !memcmp(a->samp, b->samp, sizeof a->samp) && a->cull == b->cull &&
        a->caster == b->caster && a->fill == b->fill && a->overlay == b->overlay && a->water == b->water &&
        a->small_rt == b->small_rt && a->screen_fx == b->screen_fx &&
        a->el_lantern == b->el_lantern && a->dlss5_skip == b->dlss5_skip && a->zbias == b->zbias &&
        a->stencil_ref == b->stencil_ref && a->label == b->label &&
        !memcmp(a->vp, b->vp, sizeof a->vp) && a->text_shadow == b->text_shadow && a->text_alpha == b->text_alpha;
}

static const uint8_t g_strip_order[6] = { 0, 1, 2, 2, 1, 3 };

/* The kept glyphs: their shadows (black over, ONE/INVSRCALPHA, so it darkens whatever the target is laid over too),
 * then the glyphs as the game drew them */
static void text_flush(void)
{
    if (!g_text_n)
        return;
    GfxDraw s = g_text;
    s.prim = GFX_TRIANGLELIST, s.count = 2 * g_text_n * g_text_offsets;
    s.data[0] = s.cpu[0] = g_text_shadow_bytes, s.size[0] = s.cpu_size[0] = g_text_n * g_text_offsets * 6 * g_text_stride;
    s.fs.text_shadow = 1, s.fs.alpha_func = 0, s.fs.fog = 0, s.fs.specular_add = 0;
    s.pipe.blend = 1, s.pipe.src = 2 /* ONE */, s.pipe.dst = 6 /* INVSRCALPHA */, s.pipe.op = 1;
    s.u.tfactor[3] = (float)g_text.text_alpha / 255.0f;
    g_backend->draw(&s);
    GfxDraw d = g_text;
    d.prim = GFX_TRIANGLELIST, d.count = 2 * g_text_n;
    d.data[0] = d.cpu[0] = g_text_bytes, d.size[0] = d.cpu_size[0] = g_text_n * 6 * g_text_stride;
    g_text_n = 0;
    g_backend->draw(&d);
}

static int grow(uint8_t** bytes, uint32_t* cap, uint32_t need)
{
    if (need <= *cap)
        return 1;
    uint32_t c = *cap ? *cap : 65536;
    while (c < need)
        c *= 2;
    uint8_t* p = (uint8_t*)realloc(*bytes, c);
    if (!p)
        return 0;
    *bytes = p, *cap = c;
    return 1;
}

static void text_keep(const GfxDraw* d)
{
    static const float SHADOW[1][2] = { { 1, 1 } };
    static const float OUTLINE[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
    const float(*offsets)[2] = d->text_shadow == 2 ? OUTLINE : SHADOW;
    const uint32_t noffsets = d->text_shadow == 2 ? 8 : 1;
    const uint32_t stride = (uint32_t)d->u.stride[0], pos = (uint32_t)d->u.offset[GFX_R_POSITION];
    if (g_text_n && (g_text_n == QUAD_MAX || stride != g_text_stride || !quad_same(&g_text, d)))
        text_flush();
    if (!grow(&g_text_bytes, &g_text_cap, (g_text_n + 1) * 6 * stride) ||
        !grow(&g_text_shadow_bytes, &g_text_shadow_cap, (g_text_n + 1) * noffsets * 6 * stride))
    {
        text_flush(); /* no room: this one as it came, no shadow */
        g_backend->draw(d);
        return;
    }
    if (!g_text_n)
        g_text = *d, g_text_stride = stride, g_text_offsets = noffsets;
    uint8_t* out = g_text_bytes + g_text_n * 6 * stride;
    for (int i = 0; i < 6; ++i)
        memcpy(out + i * stride, (const uint8_t*)d->data[0] + g_strip_order[i] * stride, stride);
    uint8_t* sh = g_text_shadow_bytes + g_text_n * noffsets * 6 * stride;
    for (uint32_t o = 0; o < noffsets; ++o)
        for (int i = 0; i < 6; ++i, sh += stride)
        {
            memcpy(sh, out + i * stride, stride);
            float xy[2];
            memcpy(xy, sh + pos, 8);
            xy[0] += offsets[o][0], xy[1] += offsets[o][1];
            memcpy(sh + pos, xy, 8);
        }
    g_text_n++;
}

/* The kept quads, as one triangle list (each strip's two triangles as 0 1 2 and 2 1 3: the strip's second
 * triangle, wound the same way) */
static void quads_flush(void)
{
    text_flush();
    if (!g_quad_n)
        return;
    GfxDraw d = g_quad;
    d.prim = GFX_TRIANGLELIST, d.count = 2 * g_quad_n;
    d.data[0] = g_quad_bytes, d.size[0] = g_quad_n * 6 * g_quad_stride;
    d.cpu[0] = g_quad_bytes, d.cpu_size[0] = d.size[0];
    g_quad_n = 0;
    g_quads_merged++;
    g_backend->draw(&d);
}

static void quad_keep(const GfxDraw* d)
{
    const uint32_t stride = (uint32_t)d->u.stride[0];
    if (g_quad_n && (g_quad_n == QUAD_MAX || stride != g_quad_stride || !quad_same(&g_quad, d)))
        quads_flush();
    const uint32_t need = (g_quad_n + 1) * 6 * stride;
    if (need > g_quad_cap)
    {
        uint32_t cap = g_quad_cap ? g_quad_cap : 65536;
        while (cap < need)
            cap *= 2;
        uint8_t* p = (uint8_t*)realloc(g_quad_bytes, cap);
        if (!p) /* no room: this one as it came */
        {
            quads_flush();
            g_backend->draw(d);
            return;
        }
        g_quad_bytes = p, g_quad_cap = cap;
    }
    if (!g_quad_n)
        g_quad = *d, g_quad_stride = stride;
    static const uint8_t order[6] = { 0, 1, 2, 2, 1, 3 };
    uint8_t* out = g_quad_bytes + g_quad_n * 6 * stride;
    for (int i = 0; i < 6; ++i)
        memcpy(out + i * stride, (const uint8_t*)d->data[0] + order[i] * stride, stride);
    g_quad_n++;
    g_quads_drawn++;
}

/* Last frame's: how many quads were kept, and the draws they went out as (the frame profile's) */
void gfx_quads_counts(uint32_t* quads, uint32_t* draws)
{
    *quads = g_quads_drawn_last, *draws = g_quads_merged_last;
}

static int g_sh_sent_ok; /* (the shadows' hours, below: what the back end was last handed is known) */

void gfx_select(int d3d11)
{
    g_backend = d3d11 ? &gfx11_backend : &gfx12_backend;
    g_sh_sent_ok = 0; /* handed to the new one at the next scene */
}
int gfx_selected_d3d11(void) { return g_backend == &gfx11_backend; }

int gfx_init(void* window, int vsync)
{
    g_quad_n = 0, g_text_n = 0, g_target_known = 0;
    return g_backend->init(window, vsync);
}
void gfx_set_vsync(int on) { g_backend->set_vsync(on); }
const char* gfx_adapter_name(void) { return g_backend->adapter_name(); }
int gfx_video_memory(unsigned long long* used, unsigned long long* budget) { return g_backend->video_memory(used, budget); }
void gfx_resize(uint32_t w, uint32_t h)
{
    g_quad_n = 0, g_text_n = 0, g_target_known = 0;
    g_backend->resize(w, h);
}
GfxBuf* gfx_buf_create(uint32_t size) { return g_backend->buf_create(size); }
void gfx_buf_destroy(GfxBuf* b)
{
    quads_flush();
    g_backend->buf_destroy(b);
}
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    quads_flush();
    g_backend->buf_upload(b, data, size);
}
GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    return g_backend->tex_create(type, d3dfmt, w, h, levels, use);
}
void gfx_tex_destroy(GfxTex* t)
{
    quads_flush();
    if (t == g_target_color || t == g_target_depth)
        g_target_known = 0;
    g_backend->tex_destroy(t);
}
void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    quads_flush();
    g_backend->tex_upload(t, face, level, src, pitch);
}
void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* src,
    uint32_t pitch)
{
    quads_flush();
    g_backend->tex_upload_rect(t, face, level, x, y, w, h, src, pitch);
}
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    quads_flush();
    g_backend->tex_read(t, face, level, dst, pitch);
}
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    quads_flush();
    g_backend->tex_read_async(t, face, level, dst, pitch);
}
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst, uint32_t dface,
    uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    quads_flush();
    g_backend->copy(src, sface, slevel, sx, sy, w, h, dst, dface, dlevel, dx, dy);
}
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (!g_target_known || color != g_target_color || face != g_target_face || level != g_target_level || depth != g_target_depth)
    {
        quads_flush();
        g_target_color = color, g_target_face = face, g_target_level = level, g_target_depth = depth, g_target_known = 1;
    }
    g_backend->set_targets(color, face, level, depth);
}
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    quads_flush();
    g_backend->clear(nrects, rects, flags, color, z, stencil, vp);
}
void gfx_draw(const GfxDraw* d)
{
    if (d->text_shadow && quad_shaped(d))
    {
        if (g_quad_n)
            quads_flush(); /* the other quads kept go first (there are no glyphs kept while there are) */
        text_keep(d);
        return;
    }
    if (quad_mergeable(d))
    {
        text_flush();
        quad_keep(d);
        return;
    }
    quads_flush();
    g_backend->draw(d);
}
int g_gfx_batch = 1; /* on by default ([graphics.effects] batch) */
GfxOccl g_gfx_occl = { 0 };
void gfx_occl_boxes(const float* boxes, uint32_t count)
{
    GfxOccl* o = &g_gfx_occl;
    if (count > GFX_OCCL_MAX)
        count = GFX_OCCL_MAX;
    free(o->boxes), free(o->result);
    o->boxes = NULL, o->result = NULL, o->count = 0;
    o->result_gen = 0, o->tested = o->hidden = 0;
    o->gen++;
    if (!boxes || !count)
        return;
    o->boxes = (float*)malloc(sizeof(float) * 6 * count);
    o->result = (uint8_t*)calloc(count, 1);
    if (!o->boxes || !o->result)
    {
        free(o->boxes), free(o->result);
        o->boxes = NULL, o->result = NULL;
        return;
    }
    memcpy(o->boxes, boxes, sizeof(float) * 6 * count);
    o->count = count;
}
void gfx_batch(int open)
{
    quads_flush();
    g_backend->batch(open);
}
void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    quads_flush();
    gfx_player_pool_forget(); /* the back end's composite keeps it again if it lays one */
    g_backend->scene_done(color, s);
}
void gfx_scene_world_depth(GfxTex* depth)
{
    quads_flush();
    g_backend->scene_world_depth(depth);
}
int gfx_scene_snapshot(GfxTex* color, GfxTex** colour_copy, GfxTex** depth_copy, uint32_t* w, uint32_t* h)
{
    quads_flush();
    return g_backend->scene_snapshot(color, colour_copy, depth_copy, w, h);
}
GfxTex* gfx_scene_mirror(const GfxTex* color, const GfxScene* s, float level, float scale, float reach)
{
    quads_flush();
    return g_backend->scene_mirror(color, s, level, scale, reach);
}
void gfx_scene_floors(GfxTex* id, GfxTex* depth, int state)
{
    quads_flush();
    g_backend->scene_floors(id, depth, state);
}
int gfx_scene_floors_wanted(void) { return g_backend->scene_floors_wanted(); }
void gfx_scene_rain_mask(GfxTex* mask, float wet, float reflect, float puddles)
{
    g_backend->scene_rain_mask(mask, wet, reflect, puddles);
}
int gfx_scene_rain_mask_wanted(void) { return g_backend->scene_rain_mask_wanted(); }
float gfx_seek_reach(float* stretch) { return g_backend->seek_reach(stretch); }
int gfx_sun_record(const char* path, uint32_t max_mb) { return g_backend->sun_record(path, max_mb); }
void gfx_glow(GfxTex* src, const float tint[4])
{
    quads_flush();
    g_backend->glow(src, tint);
}
void gfx_glow_over_grass(GfxTex* color, GfxTex* depth)
{
    quads_flush();
    g_backend->glow_over_grass(color, depth);
}
int gfx_sky(const GfxSky* s)
{
    if (s)
        quads_flush();
    return g_backend->sky(s);
}
void gfx_dlss_set(int quality, const wchar_t* helper, const wchar_t* log)
{
    quads_flush();
    g_backend->dlss_set(quality, helper, log);
}
int gfx_dlss_jitter(float* ndc) { return g_backend->dlss_jitter(ndc); }
int gfx_dlss_state(char* message, int size) { return g_backend->dlss_state(message, size); }
void gfx_dlss_framegen(int count)
{
    quads_flush();
    g_backend->dlss_framegen(count);
}
int gfx_dlss_framegen_state(char* message, int size) { return g_backend->dlss_framegen_state(message, size); }
unsigned gfx_waiting(void) { return g_backend->waiting(); }
int gfx_wait_progress(unsigned bit, unsigned* done, unsigned* total) { return g_backend->wait_progress(bit, done, total); }
int gfx_wait_step(unsigned bit, char* words, int size) { return g_backend->wait_step(bit, words, size); }
float g_gfx_moon_size = 1.0f, g_gfx_moon_bright = 1.0f;
float g_gfx_night = 0.0f, g_gfx_night_bright = 0.0f;
float g_gfx_indoor_bright = 0.0f, g_gfx_ambient_force = 0.0f, g_gfx_night_bright_ambient = 0.0f;
float g_gfx_day_bright = 0.0f;

/* (the moon's two settings and night are the draw preparation's, d3d8.c moon_adjust and night_light, and textbatch is this
 * file's: kept here, where every back end passes; NaN is the default, a key taken out of the settings) */
void gfx_fx_rebuild(void) { g_backend->fx_rebuild(); }

/* [graphics.effects] the shadows by day and by night. sun (the strength) and sun_casters are the day's; sun_moon_strength and
 * sun_moon_casters the night's: from 18:00 to 06:00 outdoors, the hours the game lights the world by the moon (gfx_sun_hours,
 * d3d8_sun.c), while sun_moon ("Shadows at night") is on and the sun isn't placed by hand (sun_free). The back end is handed the
 * hour's pair as sun and sun_casters and draws as it always has; the hand-over at 18:00 and 06:00 falls in the dip that hides the
 * light's turn (gfx_shared.c night_turns), and the maps are drawn again for new casters (sun_map's g_still). The day's strength
 * at 0 (the Shadows switch off: graphics.cpp sets it 0) is no shadows at night either. A NaN (the key not in the settings) is
 * handed on as it is: the back end's default for the day's key (0.6, everything casting), the night's the same */
enum { SH_DAY, SH_DAY_CASTERS, SH_NIGHT, SH_NIGHT_CASTERS, SH_MOON, SH_FREE, SH_KEYS };
static const char* const SH_KEY[SH_KEYS] = { "sun", "sun_casters", "sun_moon_strength", "sun_moon_casters", "sun_moon", "sun_free" };
static float g_sh[SH_KEYS]; /* as set */
static float g_sh_sent[2];  /* the strength and casters the back end was last handed */
static int g_sh_init, g_sh_night; /* g_sh_night: the clock's night, outdoors (gfx_sun_hours) */

static int sh_same(float a, float b) { return a == b || (a != a && b != b); }

static void sh_init(void)
{
    if (g_sh_init)
        return;
    const uint32_t bits = 0x7FC00000u; /* not set: the default (a quiet NaN) */
    float nan;
    memcpy(&nan, &bits, sizeof nan);
    for (int k = 0; k < SH_KEYS; ++k)
        g_sh[k] = nan;
    g_sh_init = 1;
}

static void sun_hours_push(int force)
{
    sh_init();
    const int day_on = !(g_sh[SH_DAY] <= 0.0f);                              /* (NaN: 0.6) */
    const int free_sun = g_sh[SH_FREE] == g_sh[SH_FREE] && g_sh[SH_FREE] != 0.0f; /* (NaN: off) */
    const int night = g_sh_night && g_sh[SH_MOON] != 0.0f && !free_sun;    /* (sun_moon NaN: on) */
    float strength = g_sh[SH_DAY], casters = g_sh[SH_DAY_CASTERS];
    if (night)
        strength = day_on ? g_sh[SH_NIGHT] : 0.0f, casters = g_sh[SH_NIGHT_CASTERS];
    if (!force && g_sh_sent_ok && sh_same(strength, g_sh_sent[0]) && sh_same(casters, g_sh_sent[1]))
        return;
    g_sh_sent[0] = strength, g_sh_sent[1] = casters, g_sh_sent_ok = 1;
    g_backend->fx_set("sun", strength);
    g_backend->fx_set("sun_casters", casters);
}

void gfx_sun_hours(int night)
{
    g_sh_night = night != 0;
    sun_hours_push(0);
}

int gfx_sun_chars_now(void)
{
    sh_init();
    const float c = g_sh_sent_ok ? g_sh_sent[1] : g_sh[SH_DAY_CASTERS];
    return !(fabsf(c - 2.0f) < 0.5f); /* (NaN: everything casts) */
}

void gfx_fx_set(const char* key, float v)
{
    quads_flush();
    for (int k = 0; k < SH_KEYS; ++k)
        if (!strcmp(key, SH_KEY[k]))
        {
            sh_init();
            g_sh[k] = v;
            if (k >= SH_MOON)
                g_backend->fx_set(key, v); /* the back end's own too (gfx_shared.c gfx_sun_light) */
            sun_hours_push(1);
            return;
        }
    if (!strcmp(key, "moon_size"))
        g_gfx_moon_size = v == v ? v : 1.0f;
    else if (!strcmp(key, "moon_brightness"))
        g_gfx_moon_bright = v == v ? v : 1.0f;
    else if (!strcmp(key, "night"))
        g_gfx_night = v == v ? fminf(fmaxf(v, 0.0f), 1.0f) : 0.0f;
    else if (!strcmp(key, "night_bright"))
        g_gfx_night_bright = v == v ? fminf(fmaxf(v, 0.0f), 3.0f) : 0.0f;
    else if (!strcmp(key, "night_bright_ambient"))
        g_gfx_night_bright_ambient = v == v ? fminf(fmaxf(v, 0.0f), 3.0f) : 0.0f;
    else if (!strcmp(key, "day_bright"))
        g_gfx_day_bright = v == v ? fminf(fmaxf(v, 0.0f), 3.0f) : 0.0f;
    else if (!strcmp(key, "indoor_bright"))
        g_gfx_indoor_bright = v == v ? fminf(fmaxf(v, 0.0f), 5.0f) : 0.0f;
    else if (!strcmp(key, "ambient_force"))
        g_gfx_ambient_force = v == v ? fminf(fmaxf(v, 0.0f), 1.0f) : 0.0f;
    else if (!strcmp(key, "textbatch"))
        g_quads_on = v == v ? v != 0.0f : 0;
    else if (!strcmp(key, "batch")) /* the zone's draws of one mesh sent together (gfx_d3d12_batch.h): the next zone draw on */
        g_gfx_batch = v == v ? v != 0.0f : 1;
    else if (!strcmp(key, "sun_zone_mesh")) /* the sun's shadows from the zone's own meshes (zonemesh.c) */
        g_gfx_zone_mesh = v == v ? v : 1.0f;
    else
        g_backend->fx_set(key, v);
}
void gfx_trace_dump(const char* path)
{
    quads_flush();
    g_backend->trace_dump(path);
}
uint64_t gfx_frame(void) { return g_backend->frame(); }
void gfx_present(GfxTex* backbuffer)
{
    quads_flush();
    g_quads_drawn_last = g_quads_drawn, g_quads_merged_last = g_quads_merged;
    g_quads_drawn = g_quads_merged = 0;
    g_target_known = 0;
    g_backend->present(backbuffer);
}
void gfx_present_layer(GfxTex* layer)
{
    quads_flush();
    g_backend->present_layer(layer);
}
int gfx_present_backdrops(const float* rects, int n) { return g_backend->present_backdrops(rects, n); }
void gfx_present_layer_below(GfxTex* layer)
{
    quads_flush();
    g_backend->present_layer_below(layer);
}
void gfx_finish(void)
{
    quads_flush();
    g_backend->finish();
}
void gfx_set_sync_pipelines(int on) { g_backend->set_sync_pipelines(on); }
uint32_t gfx_failures(void) { return g_backend->failures(); }
int gfx_lost(void) { return g_backend->lost(); }
int gfx_shutdown(void)
{
    g_quad_n = 0, g_text_n = 0, g_target_known = 0; /* the kept quads' textures die with the device */
    return g_backend->shutdown();
}
