/* The graphics back end on Direct3D 11 (renderer=d3d11): what gfx.h asks for, on one ID3D11Device. The
 * Direct3D 12 back end's design (gfx_d3d12.c) in Direct3D 11's terms:
 *
 *   - One immediate context. The driver orders the work, knows what the GPU still reads, and keeps a
 *     released object alive until it's done with it: no fences, barriers or deferred releases here.
 *   - Vertex, index and uniform bytes are copied per draw into dynamic buffers (written NO_OVERWRITE),
 *     so the game may rewrite a buffer the moment a draw returns, as D3D lets it: vertices and indices
 *     into chunks DISCARDed once a frame (what a frame wrote stays to its end: Chunks), the uniforms
 *     into a ring DISCARDed when it wraps, bound as a range of it (Direct3D 11.1).
 *   - Binding: the uniforms at b0, the vertex streams as raw buffers t0..t3 - the vertex function
 *     fetches by hand, as on Direct3D 12, and a stream's place in its buffer is added to the offsets
 *     in the uniforms - each stage's texture at t4..t11 (a cube's at t12..t19) and its sampler at
 *     s0..s7.
 *   - The generated HLSL (gfx_hlsl.c in its Direct3D 11 form) is compiled as Shader Model 5.0 off the
 *     game's thread and kept by key, as the Direct3D 12 back end's pipelines are: the draws that need
 *     a pair still compiling are left out meanwhile, and every pair built is recorded in the cache
 *     folder, so a later session builds them in the background at start. Blending, depth-stencil,
 *     rasterizer and sampler states are made on first use and kept.
 *   - Textures are DEFAULT textures filled with UpdateSubresource. Direct3D 11 has no component
 *     mapping: X8R8G8B8 is B8G8R8X8, and L8 and A8L8 are widened to BGRA8 on upload, as the 16-bit
 *     color formats are on both.
 *   - Clears of a whole target are the context's own; of rectangles, a triangle each through a
 *     scissor.
 *   - The back buffer reaches the window through a flip swap chain, drawn at the window's size, with
 *     VanaCore's interface layer over it.
 *   - The scene effects (gfx_scene_done) are Direct3D 12's, from the same HLSL fitted to Direct3D 11's
 *     registers: ambient occlusion, sun shadows, fog, bloom, god rays, the grade, the cel look, depth
 *     of field, FXAA, the scene filter, anisotropic world textures, per-pixel light and sharpening at
 *     Present.
 *   - AMD's FSR ([graphics] fsr) runs in the DLSS helper, as Direct3D 12's does, on textures and a fence
 *     of this device's shared with it (g_up). */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define GFX_PREFIX gfx11
#include "gfx_rename.h"
#include "gfx.h"
#include "gfx_backend.h"
#include "gfx_hlsl.h"
#include "gfx_shared.h"
#include "../dlss/dlss_ipc.h"

#define GFX_PROBES 8 /* reads of one surface per frame that keep their own history (gfx_tex_read_async) */
#define GFX_READBACKS (3 * GFX_PROBES)
#define VCHUNK_SIZE (8u << 20) /* a chunk of a frame's vertex bytes */
#define ICHUNK_SIZE (2u << 20) /* ... index bytes */
#define CRING_SIZE (16u << 20) /* uniforms: GfxU's window a draw */
#define U_CONSTANTS 224        /* GfxU (3536 bytes) in 16-byte constants, rounded up to the 16 a binding takes */
#define T_TEX2D 4              /* registers: t0..t3 the streams, t4..t11 the stages' 2D textures, */
#define T_TEXCUBE 12           /* ... t12..t19 their cube textures */

/* D3DFORMAT values the back end maps */
enum
{
    F_A8R8G8B8 = 21,
    F_X8R8G8B8 = 22,
    F_R5G6B5 = 23,
    F_X1R5G5B5 = 24,
    F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26,
    F_A8 = 28,
    F_L8 = 50,
    F_A8L8 = 51,
    F_V8U8 = 60,
    F_D16 = 80,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* what an upload widens to BGRA8 (and a readback narrows back) */
enum
{
    CONV_NONE,
    CONV_565,
    CONV_X555,
    CONV_1555,
    CONV_4444,
    CONV_L8,
    CONV_A8L8,
};

struct GfxTex
{
    ID3D11Texture2D* res;
    ID3D11ShaderResourceView* srv;
    DXGI_FORMAT dxfmt; /* the texture's format (a depth surface's: its DSV's) */
    int type, use, conv;
    uint32_t fmt, w, h, levels; /* D3D's sizes */
    uint32_t rw, rh;            /* the texture's: DXT top levels round up to whole blocks */
    uint32_t faces;
    uint32_t block; /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t texel; /* bytes per texel in the texture */
    int has_stencil, x8;
    IUnknown** views; /* per subresource: its render-target (or depth-stencil) view, made on first use */
    /* gfx_tex_read_async: copies in staging textures, and the frame, surface and read each was for */
    ID3D11Texture2D* rb[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame; /* the frame the reads below were counted in */
    uint32_t rb_count; /* reads of this surface so far in that frame */
    /* the scene effects (gfx_scene_done) */
    uint32_t mips;                      /* levels in the texture: `levels`, or a large color target's whole chain */
    ID3D11ShaderResourceView* srv_mips; /* ... that chain, for the draw that makes the finished scene smaller; else NULL */
    uint64_t scene;       /* its chain holds its first level as it is now (the scene filter): 0 once it is drawn to again */
    uint32_t filled;      /* the levels uploaded so far, a bit each (anisotropic filtering wants them all) */
    GfxTex* depth_seen;   /* color targets: the depth its first level was last drawn with */
    GfxTex* depth_world;  /* ... the depth the scene's casters (its world) were last drawn with: what the scene effects
                           * read - a later pass on it with another depth does not change it */
    uint64_t sun_seen;    /* the frame a shadow caster last sampled it (alpha-tested) */
    uint8_t sun_cached;   /* the zone's cached casters may sample it */
    GfxTex *rt_prev, *rt_next; /* every color target, so a depth surface's end clears depth_seen */
};

struct GfxBuf
{
    ID3D11Buffer* res;
    ID3D11ShaderResourceView* srv; /* raw: the vertex function's */
    uint32_t size;
    uint64_t sun_seen;  /* the frame a shadow caster last drew from it */
    uint8_t sun_cached; /* the zone's cached casters may draw from it */
    uint32_t uploads;   /* times it was written, and the frame it last was: written again lately, it's the */
    uint64_t upload_frame; /* game's to rewrite (the shadow cache keeps copies of what's drawn from it) */
    uint64_t twice;     /* the last frame it was written in more than once (REWRITTEN) */
};

/* a dynamic buffer written front to back, draw after draw */
typedef struct Ring
{
    ID3D11Buffer* buf;
    uint32_t size, at;
    uint32_t gen;     /* counts its DISCARDs: what was written before one is gone */
    int discard_only; /* NO_OVERWRITE isn't allowed for it here: every write DISCARDs */
} Ring;

/* The draws' vertex and index bytes: dynamic buffers (chunks) a frame writes front to back, the first
 * write of each in a frame DISCARDing it and the rest NO_OVERWRITE, so what a frame wrote stays as it was
 * to the frame's end - when the scene is done, the sun's shadow casters are drawn again from it. A frame
 * that fills one goes on in the next, made when first needed. */
typedef struct Chunk
{
    ID3D11Buffer* buf;
    ID3D11ShaderResourceView* srv; /* the vertex chunks' raw view */
    uint32_t size;
    uint64_t frame; /* the frame that last DISCARDed it */
    uint32_t gen;   /* its DISCARDs: what was written before one is gone */
} Chunk;

typedef struct Chunks
{
    Chunk* c;
    uint32_t n, cur, at; /* the chunks; the one this frame writes, and where */
    uint64_t frame;      /* the frame cur and at are this frame's for */
    uint32_t size;       /* a chunk's */
    UINT bind, misc;
    int discard_only;    /* NO_OVERWRITE isn't allowed for them here: every write DISCARDs */
} Chunks;

static ID3D11Device* g_dev;
static ID3D11Device1* g_dev1;
static ID3D11DeviceContext* g_ctx;
static ID3D11DeviceContext1* g_ctx1; /* constant buffer ranges; NULL: one small buffer, DISCARDed per draw */
static D3D_FEATURE_LEVEL g_level;
static Chunks g_vring, g_iring; /* the draws' vertex and index bytes */
static Ring g_cring;            /* their uniforms */
static ID3D11Buffer* g_util_cb; /* the utility passes' few constants */
static ID3D11Query* g_idle;     /* gfx_finish's */
static uint64_t g_serial = 1;   /* the frame being drawn */
static volatile LONG g_failures;
static int g_sync_pipelines;

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex* g_ds;
static GfxTex* g_scratch_depth;
static int g_targets_bound;

/* the window */
static HWND g_window;
static IDXGIFactory2* g_factory;
static IDXGISwapChain1* g_swap;
static ID3D11RenderTargetView* g_swap_rtv;
static uint32_t g_swap_w, g_swap_h, g_want_w, g_want_h;
static int g_vsync, g_tearing;
static int g_vsync_asked; /* gfx_set_vsync has been called: [graphics] vsync, which the game's own device must not undo */
static GfxTex* g_present_layer_tex;
static int g_overlay = 1;
static double g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";
static char g_adapter[128];
static IDXGIAdapter3* g_adapter3; /* the card, for gfx_video_memory (NULL without DXGI 1.4) */

/* the utility passes: present, the interface layer, the frame-rate overlay, clears of rectangles */
static ID3D11VertexShader *g_present_vs, *g_overlay_vs, *g_clear_vs;
static ID3D11PixelShader *g_present_ps, *g_layer_ps, *g_overlay_ps, *g_clear_ps;
static ID3D11SamplerState* g_linear;
static ID3D11BlendState *g_blend_none, *g_blend_alpha, *g_blend_premul, *g_blend_nocolor;
static ID3D11RasterizerState *g_raster_plain, *g_raster_scissor;
static ID3D11DepthStencilState* g_depth_off;

static ID3D11PixelShader* g_cas_ps; /* the frame sharpened at Present (the scene effects' sharpen) */

/* The scene effects' settings: FFXI_FX_* in the environment at start (fx_config), then gfx_fx_set
 * (VanaCore's vanacore.ini) or FFXI_FX_FILE. gfx_d3d12.c's, key for key. */
typedef struct FxSettings
{
    float fx, ao, radius, grade, sat, contrast, sharpen, filter, aniso, tex_sharp, fog, fog_falloff, fog_height, fog_max, fog_floor, fog_sun,
        fog_g, bloom, threshold, rays, rays_decay, rays_length, light, sun, sun_distance, sun_soft,
        sun_face, sun_min, sun_direct, sun_dusk, sun_casters, sun_near, temporal, debug;
    float ao_quality; /* the occlusion's taps a pixel a frame: 0 low (6), 1 medium (10), 2 high (16), 3 ultra (24), 4 max (32);
                       * every one at the scene's own size */
    float sun_free, sun_free_az, sun_free_el; /* the shadows' light placed by hand: 1 on, its bearing round the horizon and its
                                               * height over it, degrees; the hour, the cloud and a room are left out (gfx_d3d12.c's) */
    float sun_tilt; /* the game's sun and moon tilted this many degrees south (gfx_shared.c sun_tilt) */
    float sun_moon; /* 0: no shadows from the moon, outdoors through the night (gfx_shared.c gfx_sun_light) */
    float sun_game_shading; /* 1: the shadows weighed against the game's own smooth shading (sun_map's Ns); 0 the maps alone */
    float sun_smooth;       /* the sun on the zone by its own smooth normals (gfx_d3d11_smooth.h vn_replay): 1 on, 0 the shadow
                             * maps' alone. Always on in the game (gfx_d3d12.c's) */
    float sun_soft_zone;    /* the zone's shadows blurred this far across its surfaces, world units (fx_blur; gfx_d3d12.c's) */
    float sun_curved;       /* 1: the zone's curved rock and ground shaded as the smooth surface their normals describe where they
                             * turn from the sun (gfx_d3d11_smooth.h zn_draw); 0 by the polygons, as the maps have them */
    float sun_rings;  /* the sun's shadow maps' taps a pixel (sun_look, 16): fewer past each of three rings out from the camera,
                       * 12, 8, then 6; 1 on, 0 all of them everywhere */
    float sun_ring_spacing; /* ... how far apart the rings are, world units */
    float sun_detail; /* the near map's texels across, 512-8192 (sun_size) */
    float sun_fade;   /* seconds the zone's shadows the cache lets go of take to fade (cache_let_go); 0 at once */
    float sun_handoff; /* the share of the sharp map's half-width over which it fades into the far one (gfx_d3d12.c's) */
    float normalmap, normalmap_strength, normalmap_view; /* texture packs' normal maps (gfx_d3d12.c's): on, how far they bend, the
                                                         * debug view (1 the bent normal as the colour, 2 lit with Y turned over) */
    float moghouse;   /* how much of the sun's shadows stay indoors (GfxScene.indoors), 0 none to 1 all (gfx_d3d12.c's) */
    /* the cel look: ink lines, brightness bands, smoothing */
    float outline, outline_width, outline_depth, outline_crease, outline_fade, bands, band_soft, smooth;
    /* ... and 1 (the default) leaves VanaCore's grass out of the lines (gfx_d3d12.c's) */
    float outline_nograss;
    /* the lines' width on characters: Direct3D 12's only (the characters' depth), the world's width here */
    float outline_width_chars;
    /* ... and with outline_nograss, the grass's own lines (gfx_grass_lines) */
    float outline_grass, outline_width_grass, outline_depth_grass, outline_fade_grass;
    /* depth of field around the player; dof_sky 1 leaves the sky (its clouds, the sun and moon) sharp */
    float dof, dof_focus, dof_player, dof_range, dof_far, dof_near, dof_sky;
    /* anti-aliasing: 1 FXAA on the finished scene (runs with the effects off too) */
    float aa;
    /* the colour grade's tone, -1..1: brightness, shadows, highlights, warmth (gfx_d3d12.c's); its hue, -180..180 degrees */
    float brightness, shadows, highlights, warmth, hue;
    /* clarity: local contrast over the finished composite, 0 off (gfx_d3d12.c's) */
    float clarity;
    /* the moonlight through the night, 0 to 1 (gfx_d3d12.c's moon_grade, laid in the composite here) */
    float night_moon;
    /* the grade's warmth through the Vana'diel day: warmer by day, cooler at night (gfx_d3d12.c's) */
    float day_night_warmth;
    /* the player's light: strength, the pool's radius, its colour, how dark round it by day (gfx_d3d12.c's) */
    float player_light, player_light_radius, player_light_warm, player_light_day;
    /* the height fog's colour: 0 the zone's own, 2 this one (red, green, blue); 1 (matched to the sky) is Direct3D
     * 12's alone, here the zone's */
    float fog_mode, fog_cr, fog_cg, fog_cb;
    /* VanaCore's glow round trusts and avatars: how strong and how wide, 1 as it was (gfx_d3d12.c's) */
    float glow_strength, glow_size, glow_quality;
} FxSettings;
static FxSettings g_fxs;
static GfxTex* g_rts; /* every color target (depth_seen) */
static void sun_forget(GfxBuf* b, GfxTex* t);
static void fx_reload(void);

/* --- small hash maps (key bytes -> pointer): gfx_d3d12.c's -------------------------------------------------- */
typedef struct MapEnt
{
    uint64_t hash;
    void* key;
    size_t klen;
    void* obj;
} MapEnt;

typedef struct Map
{
    MapEnt* e;
    uint32_t cap, n;
} Map;

static uint64_t fnv(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h1 = 0x9747B28Cu ^ (uint32_t)n, h2 = 0x811C9DC5u;
    for (; n >= 4; b += 4, n -= 4)
    {
        uint32_t w;
        memcpy(&w, b, 4);
        h2 = (h2 ^ w) * 0x01000193u;
        w *= 0xCC9E2D51u;
        w = (w << 15) | (w >> 17);
        h1 ^= w * 0x1B873593u;
        h1 = ((h1 << 13) | (h1 >> 19)) * 5 + 0xE6546B64u;
    }
    for (; n; ++b, --n)
        h1 = (h1 ^ *b) * 0x01000193u, h2 = (h2 ^ *b) * 0x01000193u;
    h1 ^= h1 >> 16, h1 *= 0x85EBCA6Bu, h1 ^= h1 >> 13, h1 *= 0xC2B2AE35u, h1 ^= h1 >> 16;
    h2 ^= h2 >> 15, h2 *= 0x2C1B3C6Du, h2 ^= h2 >> 12;
    uint64_t h = (uint64_t)h2 << 32 | h1;
    return h ? h : 1;
}

static MapEnt* map_find(Map* m, const void* key, size_t klen, uint64_t h)
{
    if (!m->cap)
        return NULL;
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        MapEnt* e = &m->e[i];
        if (!e->hash)
            return NULL;
        if (e->hash == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e;
    }
}

static void* map_get(Map* m, const void* key, size_t klen)
{
    MapEnt* e = map_find(m, key, klen, fnv(key, klen));
    return e ? e->obj : NULL;
}

static void map_put(Map* m, const void* key, size_t klen, void* obj)
{
    if ((m->n + 1) * 2 > m->cap)
    {
        Map old = *m;
        m->cap = m->cap ? m->cap * 2 : 256;
        m->e = (MapEnt*)calloc(m->cap, sizeof *m->e);
        m->n = 0;
        for (uint32_t i = 0; i < old.cap; ++i)
            if (old.e[i].hash)
            {
                uint32_t j = (uint32_t)old.e[i].hash & (m->cap - 1);
                while (m->e[j].hash)
                    j = (j + 1) & (m->cap - 1);
                m->e[j] = old.e[i];
                m->n++;
            }
        free(old.e);
    }
    uint64_t h = fnv(key, klen);
    uint32_t j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].hash)
        j = (j + 1) & (m->cap - 1);
    m->e[j].hash = h;
    m->e[j].key = malloc(klen);
    memcpy(m->e[j].key, key, klen);
    m->e[j].klen = klen;
    m->e[j].obj = obj;
    m->n++;
}

static Map g_blends, g_depths, g_rasters, g_samplers;

/* --- what's bound on the context, so a draw sets only what changed ----------------------------------------- */
static struct
{
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11BlendState* blend;
    ID3D11DepthStencilState* depth;
    uint32_t stencil_ref;
    ID3D11RasterizerState* raster;
    D3D11_VIEWPORT vp;
    int topo;
    ID3D11ShaderResourceView* streams[GFX_NSTREAMS];
    ID3D11ShaderResourceView* tex[16]; /* t4..t19 */
    ID3D11SamplerState* samp[8];
    ID3D11Buffer* ib;
    DXGI_FORMAT ib_fmt;
    uint32_t ib_off;
} g_bound;

/* after a pass that binds its own, and when the targets change (Direct3D 11 unbinds a texture from the
 * stages when it becomes a target): nothing is taken as bound */
static void bound_forget(void)
{
    memset(&g_bound, 0xFF, sizeof g_bound);
}

static void bound_forget_textures(void)
{
    memset(g_bound.tex, 0xFF, sizeof g_bound.tex);
}

/* --- formats ---------------------------------------------------------------------------------------------- */
static DXGI_FORMAT dx_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel)
{
    *conv = CONV_NONE, *block = 0, *texel = 4;
    if (use == GFX_USE_DEPTH)
        return fmt == F_D16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
    switch (fmt)
    {
    case F_A8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X8R8G8B8: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case F_R5G6B5: *conv = CONV_565; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X1R5G5B5: *conv = CONV_X555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A1R5G5B5: *conv = CONV_1555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A4R4G4B4: *conv = CONV_4444; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A8: *texel = 1; return DXGI_FORMAT_A8_UNORM;
    case F_L8: *conv = CONV_L8; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A8L8: *conv = CONV_A8L8; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_V8U8: *texel = 2; return DXGI_FORMAT_R8G8_SNORM;
    }
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        return *block = 8, DXGI_FORMAT_BC1_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        return *block = 16, DXGI_FORMAT_BC2_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        return *block = 16, DXGI_FORMAT_BC3_UNORM;
    if (fmt == FOURCC('A', 'T', 'I', '2'))
        return *block = 16, DXGI_FORMAT_BC5_UNORM;
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

/* bytes per texel in D3D's layout */
static uint32_t d3d_bpp(uint32_t fmt)
{
    switch (fmt)
    {
    case F_A8:
    case F_L8: return 1;
    case F_R5G6B5:
    case F_X1R5G5B5:
    case F_A1R5G5B5:
    case F_A4R4G4B4:
    case F_A8L8:
    case F_V8U8: return 2;
    default: return 4;
    }
}

static uint32_t expand(uint32_t v, int bits)
{
    return bits == 1 ? (v ? 255u : 0u) : (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}

/* one row of a D3D format the texture holds widened, into BGRA8 */
static void convert_row(int conv, const uint8_t* src, uint8_t* dst, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t r, g, b, a;
        if (conv == CONV_L8)
            r = g = b = src[x], a = 255;
        else if (conv == CONV_A8L8)
            r = g = b = src[2 * x], a = src[2 * x + 1];
        else
        {
            uint32_t p = (uint32_t)src[2 * x] | ((uint32_t)src[2 * x + 1] << 8);
            switch (conv)
            {
            case CONV_565: r = expand(p >> 11, 5), g = expand((p >> 5) & 63, 6), b = expand(p & 31, 5), a = 255; break;
            case CONV_X555: r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = 255; break;
            case CONV_1555:
                r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = (p & 0x8000) ? 255 : 0;
                break;
            default: r = (p >> 8 & 15) * 17, g = (p >> 4 & 15) * 17, b = (p & 15) * 17, a = (p >> 12) * 17; break;
            }
        }
        dst[4 * x] = (uint8_t)b, dst[4 * x + 1] = (uint8_t)g, dst[4 * x + 2] = (uint8_t)r, dst[4 * x + 3] = (uint8_t)a;
    }
}

/* rows read back in the texture's layout -> D3D's */
static void copy_out(const GfxTex* t, const uint8_t* s, uint32_t w, uint32_t h, uint32_t row, void* dst, uint32_t pitch)
{
    uint32_t bpp = d3d_bpp(t->fmt);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* d = (uint8_t*)dst + (size_t)y * pitch;
        if (!t->conv)
            memcpy(d, s + (size_t)y * row, (size_t)w * bpp);
        else
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = s + (size_t)y * row + 4 * x;
                uint32_t b = p[0], g = p[1], r = p[2], a = p[3], v;
                switch (t->conv)
                {
                case CONV_L8: d[x] = (uint8_t)r; continue;
                case CONV_A8L8: d[2 * x] = (uint8_t)r, d[2 * x + 1] = (uint8_t)a; continue;
                case CONV_565: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
                case CONV_4444: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
                default: v = (a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
                }
                d[2 * x] = (uint8_t)v, d[2 * x + 1] = (uint8_t)(v >> 8);
            }
    }
}

/* --- ring buffers ----------------------------------------------------------------------------------------- */
static int ring_make(Ring* r, uint32_t size, UINT bind, UINT misc)
{
    D3D11_BUFFER_DESC d = { size, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE, misc, 0 };
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &d, NULL, &r->buf)))
        return 0;
    r->size = size, r->at = 0;
    return 1;
}

/* A chunk of a frame's vertex (or index) bytes, made the first time a frame needs it (or more room),
 * larger when one draw's bytes are */
static Chunk* chunk_make(Chunks* r, uint32_t i, uint32_t need)
{
    if (i >= r->n)
    {
        r->c = (Chunk*)realloc(r->c, (i + 1) * sizeof(Chunk));
        memset(r->c + r->n, 0, (i + 1 - r->n) * sizeof(Chunk));
        r->n = i + 1;
    }
    Chunk* k = &r->c[i];
    if (k->srv)
        ID3D11ShaderResourceView_Release(k->srv), k->srv = NULL;
    if (k->buf)
        ID3D11Buffer_Release(k->buf), k->buf = NULL; /* the driver keeps it while the GPU still reads it */
    uint32_t size = need > r->size ? (need + 0xFFFFFu) & ~0xFFFFFu : r->size;
    D3D11_BUFFER_DESC d = { size, D3D11_USAGE_DYNAMIC, r->bind, D3D11_CPU_ACCESS_WRITE, r->misc, 0 };
    k->size = 0, k->frame = 0, k->gen++; /* what its casters point at is gone */
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &d, NULL, &k->buf)))
    {
        fprintf(stderr, "[recomp] gfx: a %u-byte chunk for the draws' %s failed\n", size, r->misc ? "vertices" : "indices");
        k->buf = NULL;
        return NULL;
    }
    if (r->misc & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        v.BufferEx.NumElements = size / 4;
        v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)k->buf, &v, &k->srv)))
        {
            ID3D11Buffer_Release(k->buf), k->buf = NULL;
            return NULL;
        }
    }
    k->size = size;
    return k;
}

/* `count` pieces of this frame's bytes in one write, each at a multiple of `align` (their places in at[]):
 * a draw's streams all in the one chunk. The chunk's number, or -1 when they can't go. */
static int chunks_putv(Chunks* r, const void* const* data, const uint32_t* n, int count, uint32_t align, uint32_t* at)
{
    uint32_t total = 0;
    for (int i = 0; i < count; ++i)
        total = ((total + align - 1) & ~(align - 1)) + n[i];
    if (r->frame != g_serial) /* a new frame starts at the first chunk again */
        r->frame = g_serial, r->cur = 0, r->at = 0;
    uint32_t off = (r->at + align - 1) & ~(align - 1);
    Chunk* k = r->cur < r->n ? &r->c[r->cur] : NULL;
    if (k && k->buf && off && off + total > k->size) /* full: on in the next */
        r->cur++, off = 0, k = r->cur < r->n ? &r->c[r->cur] : NULL;
    if (!k || !k->buf || k->size < total)
    {
        k = chunk_make(r, r->cur, total);
        off = 0;
        if (!k)
            return -1;
    }
    /* the first write of a frame DISCARDs (the GPU may still read the frame before's), the rest go after
     * what the frame wrote (NO_OVERWRITE); where NO_OVERWRITE isn't allowed, every write DISCARDs */
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (k->frame != g_serial || r->discard_only)
        mode = D3D11_MAP_WRITE_DISCARD, off = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)k->buf, 0, mode, 0, &m)))
        return -1;
    if (mode == D3D11_MAP_WRITE_DISCARD)
        k->frame = g_serial, k->gen++;
    for (int i = 0; i < count; ++i)
    {
        off = (off + align - 1) & ~(align - 1);
        if (data[i])
            memcpy((uint8_t*)m.pData + off, data[i], n[i]);
        at[i] = off;
        off += n[i];
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)k->buf, 0);
    r->at = off;
    return (int)r->cur;
}

static int chunks_put(Chunks* r, const void* data, uint32_t n, uint32_t align, uint32_t* at)
{
    return chunks_putv(r, &data, &n, 1, align, at);
}

/* --- the device lost ----------------------------------------------------------------------------------------- */
static int g_removed; /* a driver timeout, the driver restarting: found at Present or as something couldn't be made */

/* 1 once the device is gone, said once (in place of a failure's own line: everything made after it fails the same way) */
static int removed(void)
{
    if (!g_removed && g_dev)
    {
        const HRESULT why = ID3D11Device_GetDeviceRemovedReason(g_dev);
        if (FAILED(why))
        {
            g_removed = 1;
            fprintf(stderr, "[recomp] gfx: device removed (%08lx)\n", (unsigned long)why);
        }
    }
    return g_removed;
}

int gfx_lost(void) { return g_removed; }

/* Not written for Direct3D 11 yet: a lost Direct3D 11 device closes the game as before (graphics::close_if_lost). The
 * front end's half (d3d8_device_lost, d3d8_device_restored) is the same for both back ends when it is. */
int gfx_shutdown(void) { return 0; }

/* --- static buffers ----------------------------------------------------------------------------------------- */
GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_dev || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = size;
    uint32_t bytes = (size + 15) & ~15u;
    D3D11_BUFFER_DESC d = { bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_INDEX_BUFFER, 0,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
    D3D11_SHADER_RESOURCE_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    v.BufferEx.NumElements = bytes / 4;
    v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &d, NULL, &b->res)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)b->res, &v, &b->srv)))
    {
        if (!removed())
            fprintf(stderr, "[recomp] gfx: buffer of %u bytes failed\n", size);
        if (b->res)
            ID3D11Buffer_Release(b->res);
        free(b);
        return NULL;
    }
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (g_bound.streams[s] == b->srv)
            g_bound.streams[s] = (ID3D11ShaderResourceView*)(uintptr_t)-1;
    if (g_bound.ib == b->res)
        g_bound.ib = (ID3D11Buffer*)(uintptr_t)-1;
    if (b->sun_seen == g_serial || b->sun_cached)
        sun_forget(b, NULL); /* the casters that draw from it are drawn no more */
    ID3D11ShaderResourceView_Release(b->srv);
    ID3D11Buffer_Release(b->res); /* the driver keeps it while the GPU still reads it */
    free(b);
}

static void buf_upload_at(GfxBuf* b, uint32_t off, const void* data, uint32_t size);

/* in command order: draws before this read the old contents, draws after the new */
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b || !data)
        return;
    if (size > b->size)
        size = b->size;
    if (!size)
        return;
    /* new contents: a caster recorded from the old (this frame's, or the zone's kept) is not it */
    if (b->sun_seen == g_serial || b->sun_cached)
        sun_forget(b, NULL);
    if (b->uploads && b->upload_frame == g_serial)
        b->twice = g_serial;
    b->uploads++, b->upload_frame = g_serial;
    buf_upload_at(b, 0, data, size);
}

/* `size` bytes into the buffer at `off`, in command order */
static void buf_upload_at(GfxBuf* b, uint32_t off, const void* data, uint32_t size)
{
    D3D11_BOX box = { off, 0, 0, off + size, 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource*)b->res, 0, &box, data, size, 0);
}

/* --- textures ------------------------------------------------------------------------------------------------ */
static uint32_t subresource(const GfxTex* t, uint32_t face, uint32_t level)
{
    return level + face * t->mips;
}

/* a chain of this format can be made by GenerateMips (the scene filter's) */
static int mip_autogen(DXGI_FORMAT f)
{
    UINT s = 0;
    return SUCCEEDED(ID3D11Device_CheckFormatSupport(g_dev, f, &s)) && (s & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN);
}

static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = (t->w >> level) ? (t->w >> level) : 1;
    *h = (t->h >> level) ? (t->h >> level) : 1;
}

/* the texture's own size of a level (differs from D3D's only for rounded-up DXT) */
static void phys_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = (t->rw >> level) ? (t->rw >> level) : 1;
    *h = (t->rh >> level) ? (t->rh >> level) : 1;
}

GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    t->dxfmt = dx_format(fmt, use, &t->conv, &t->block, &t->texel);
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
    if (type == GFX_TEX_CUBE)
        t->h = t->w;
    t->faces = type == GFX_TEX_CUBE ? 6 : 1;
    t->rw = t->w, t->rh = t->h;
    if (t->block) /* the top level of a block-compressed texture in whole blocks */
        t->rw = (t->rw + 3) & ~3u, t->rh = (t->rh + 3) & ~3u;
    uint32_t most = 1;
    for (uint32_t s = t->rw > t->rh ? t->rw : t->rh; s > 1; s >>= 1)
        most++;
    if (t->levels > most)
        t->levels = most;
    t->mips = t->levels;
    UINT misc = type == GFX_TEX_CUBE ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    /* a large color target (FFXI's background) gets a whole mip chain: the finished scene is made
     * smaller through it rather than one bilinear sample per screen pixel (scene_mips). Large is the scene's
     * size at FSR's share too: the trust and avatar glow's picture follows it, and VanaCore's glow (gfx_glow)
     * blurs through the chain (gfx_d3d12.c says more). */
    const uint32_t long_side = t->w > t->h ? t->w : t->h, short_side = t->w > t->h ? t->h : t->w;
    if (use == GFX_USE_RT && type == GFX_TEX_2D && t->levels == 1 && long_side >= 640 && short_side >= 360 &&
        mip_autogen(t->dxfmt))
    {
        while ((t->w >> t->mips) || (t->h >> t->mips))
            t->mips++;
        misc |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }
    t->has_stencil = t->dxfmt == DXGI_FORMAT_D24_UNORM_S8_UINT;
    t->x8 = fmt == F_X8R8G8B8;
    /* depth is typeless underneath, so the scene effects can read it (as R16 or R24) */
    DXGI_FORMAT resfmt = use != GFX_USE_DEPTH ? t->dxfmt
        : t->dxfmt == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    D3D11_TEXTURE2D_DESC d = { t->rw, t->rh, t->mips, t->faces, resfmt, { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | (use == GFX_USE_RT ? D3D11_BIND_RENDER_TARGET : 0) | (use == GFX_USE_DEPTH ? D3D11_BIND_DEPTH_STENCIL : 0),
        0, misc };
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->res)))
    {
        if (!removed())
            fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
        free(t);
        return NULL;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.Format = use != GFX_USE_DEPTH ? t->dxfmt : t->dxfmt == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    if (type == GFX_TEX_CUBE)
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE, v.TextureCube.MipLevels = t->levels;
    else
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = t->levels;
    /* the view the game's draws see: its own levels (a chain the scene filter adds is not the game's) */
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)t->res, &v, &t->srv)))
        t->srv = NULL;
    if (t->mips > t->levels)
    {
        v.Texture2D.MipLevels = t->mips;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)t->res, &v, &t->srv_mips)))
            t->srv_mips = NULL;
    }
    if (use != GFX_USE_SAMPLE)
        t->views = (IUnknown**)calloc(t->faces * t->mips, sizeof(IUnknown*));
    if (use == GFX_USE_RT)
    {
        t->rt_next = g_rts;
        if (g_rts)
            g_rts->rt_prev = t;
        g_rts = t;
    }
    return t;
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    if (g_rt == t)
        g_rt = NULL, g_targets_bound = 0;
    if (g_ds == t)
        g_ds = NULL, g_targets_bound = 0;
    if (g_present_layer_tex == t)
        g_present_layer_tex = NULL;
    for (int i = 0; i < 16; ++i)
        if (g_bound.tex[i] == t->srv || (t->srv_mips && g_bound.tex[i] == t->srv_mips))
            g_bound.tex[i] = (ID3D11ShaderResourceView*)(uintptr_t)-1;
    if (t->use == GFX_USE_RT)
    {
        if (t->rt_prev)
            t->rt_prev->rt_next = t->rt_next;
        else
            g_rts = t->rt_next;
        if (t->rt_next)
            t->rt_next->rt_prev = t->rt_prev;
    }
    else if (t->use == GFX_USE_DEPTH)
        for (GfxTex* r = g_rts; r; r = r->rt_next)
        {
            if (r->depth_seen == t)
                r->depth_seen = NULL;
            if (r->depth_world == t)
                r->depth_world = NULL;
        }
    if (t->sun_seen == g_serial || t->sun_cached)
        sun_forget(NULL, t); /* the casters that sample it are drawn no more */
    for (uint32_t i = 0; t->views && i < t->faces * t->mips; ++i)
        if (t->views[i])
            IUnknown_Release(t->views[i]);
    free(t->views);
    for (int i = 0; i < GFX_READBACKS; ++i)
        if (t->rb[i])
            ID3D11Texture2D_Release(t->rb[i]);
    if (t->srv_mips)
        ID3D11ShaderResourceView_Release(t->srv_mips);
    if (t->srv)
        ID3D11ShaderResourceView_Release(t->srv);
    ID3D11Texture2D_Release(t->res);
    free(t);
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* src,
    uint32_t pitch)
{
    if (!t || !src || level >= t->levels || face >= t->faces || !w || !h || t->use == GFX_USE_DEPTH)
        return;
    if (level < 32)
        t->filled |= 1u << level;
    uint32_t lw, lh;
    level_size(t, level, &lw, &lh);
    if (x >= lw || y >= lh)
        return;
    if (x + w > lw)
        w = lw - x;
    if (y + h > lh)
        h = lh - y;
    uint32_t rows = t->block ? (h + 3) / 4 : h;
    uint32_t cols = t->block ? (w + 3) / 4 : w; /* blocks or texels per row */
    uint32_t pw, ph;
    phys_size(t, level, &pw, &ph);
    if (t->block)
    {
        uint32_t most_c = (pw + 3) / 4 - x / 4, most_r = (ph + 3) / 4 - y / 4;
        cols = cols < most_c ? cols : most_c;
        rows = rows < most_r ? rows : most_r;
    }
    /* the box in texels: whole blocks, but not past the level's edge */
    uint32_t right = t->block ? x + cols * 4 : x + cols, bottom = t->block ? y + rows * 4 : y + rows;
    right = right < pw ? right : pw, bottom = bottom < ph ? bottom : ph;
    D3D11_BOX box = { x, y, 0, right, bottom, 1 };
    const void* data = src;
    uint32_t row_pitch = pitch;
    uint8_t* widened = NULL;
    if (t->conv)
    {
        widened = (uint8_t*)malloc((size_t)cols * 4 * rows);
        if (!widened)
            return;
        for (uint32_t r = 0; r < rows; ++r)
            convert_row(t->conv, (const uint8_t*)src + (size_t)r * pitch, widened + (size_t)r * cols * 4, cols);
        data = widened, row_pitch = cols * 4;
    }
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource*)t->res, subresource(t, face, level), &box, data, row_pitch, 0);
    free(widened);
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* a staging texture a level of `t` can be copied into */
static ID3D11Texture2D* staging(const GfxTex* t, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, t->dxfmt, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    ID3D11Texture2D* s = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &s)))
        return NULL;
    return s;
}

static void copy_level(GfxTex* t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, ID3D11Texture2D* to)
{
    D3D11_BOX box = { 0, 0, 0, w, h, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource*)to, 0, 0, 0, 0, (ID3D11Resource*)t->res, subresource(t, face, level),
        &box);
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || !dst || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    /* one staging texture kept for the size read last (the game reads the same few again and again) */
    static ID3D11Texture2D* kept;
    static DXGI_FORMAT kept_fmt;
    static uint32_t kept_w, kept_h;
    if (!kept || kept_fmt != t->dxfmt || kept_w != w || kept_h != h)
    {
        if (kept)
            ID3D11Texture2D_Release(kept);
        kept = staging(t, w, h), kept_fmt = t->dxfmt, kept_w = w, kept_h = h;
        if (!kept)
            return;
    }
    copy_level(t, face, level, w, h, kept);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)kept, 0, D3D11_MAP_READ, 0, &m))) /* waits for the GPU */
    {
        copy_out(t, (const uint8_t*)m.pData, w, h, m.RowPitch, dst, pitch);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)kept, 0);
    }
}

/* A surface read several times a frame (the game reuses one 16x16 target for more than one probe)
 * keeps each read's history apart, as on Direct3D 12: the k-th read this frame gets the newest
 * finished copy of the k-th read of an earlier frame, never another probe's pixels. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || !dst || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    if (t->rb_frame != g_serial)
        t->rb_frame = g_serial, t->rb_count = 0;
    uint32_t index = t->rb_count++;
    if (index >= GFX_PROBES)
    {
        gfx_tex_read(t, face, level, dst, pitch); /* more reads a frame than we keep apart */
        return;
    }
    /* the newest earlier copy of this read the GPU has finished: newest first, older ones while the
     * GPU is still on it */
    int read = -1;
    uint64_t below = g_serial;
    for (;;)
    {
        int best = -1;
        for (int i = 0; i < GFX_READBACKS; ++i)
            if (t->rb[i] && t->rb_serial[i] && t->rb_serial[i] < below && t->rb_index[i] == index && t->rb_face[i] == face &&
                t->rb_level[i] == level && (best < 0 || t->rb_serial[i] > t->rb_serial[best]))
                best = i;
        if (best < 0)
            break;
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)t->rb[best], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr))
        {
            copy_out(t, (const uint8_t*)m.pData, w, h, m.RowPitch, dst, pitch);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)t->rb[best], 0);
            read = best;
            break;
        }
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        below = t->rb_serial[best];
    }
    if (read < 0)
        gfx_tex_read(t, face, level, dst, pitch); /* nothing finished for this read yet: wait, once */
    /* this read's copy for a later frame, in the slot longest unused (not the one just read) */
    int slot = -1;
    for (int i = 0; i < GFX_READBACKS; ++i)
        if (i != read && (slot < 0 || t->rb_serial[i] < t->rb_serial[slot]))
            slot = i;
    if (slot < 0)
        return;
    if (!t->rb[slot])
    {
        t->rb[slot] = staging(t, w, h);
        if (!t->rb[slot])
            return;
    }
    else
    {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(t->rb[slot], &d);
        if (d.Width != w || d.Height != h)
        {
            ID3D11Texture2D_Release(t->rb[slot]);
            t->rb[slot] = staging(t, w, h);
            if (!t->rb[slot])
                return;
        }
    }
    copy_level(t, face, level, w, h, t->rb[slot]);
    t->rb_serial[slot] = g_serial, t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst, uint32_t dface,
    uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src == dst || src->dxfmt != dst->dxfmt || src->conv != dst->conv || !w || !h)
        return;
    if (slevel >= src->levels || dlevel >= dst->levels || sface >= src->faces || dface >= dst->faces)
        return;
    uint32_t sw, sh, dw, dh;
    level_size(src, slevel, &sw, &sh);
    level_size(dst, dlevel, &dw, &dh);
    if (src->use == GFX_USE_DEPTH && (sx || sy || dx || dy || w != sw || h != sh || sw != dw || sh != dh))
        return; /* Direct3D 11 copies depth whole subresources only */
    D3D11_BOX box = { sx, sy, 0, sx + w, sy + h, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource*)dst->res, subresource(dst, dface, dlevel), dx, dy, 0,
        (ID3D11Resource*)src->res, subresource(src, sface, slevel), src->use == GFX_USE_DEPTH ? NULL : &box);
    if (!dlevel)
        dst->scene = 0;
}

/* --- render targets ---------------------------------------------------------------------------------------------- */
static IUnknown* target_view(GfxTex* t, uint32_t face, uint32_t level)
{
    uint32_t i = subresource(t, face, level);
    if (t->views[i])
        return t->views[i];
    if (t->use == GFX_USE_DEPTH)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = t->dxfmt;
        v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipSlice = level;
        ID3D11DepthStencilView* dsv = NULL;
        if (SUCCEEDED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource*)t->res, &v, &dsv)))
            t->views[i] = (IUnknown*)dsv;
    }
    else
    {
        D3D11_RENDER_TARGET_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = t->dxfmt;
        if (t->type == GFX_TEX_CUBE)
        {
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray.MipSlice = level, v.Texture2DArray.FirstArraySlice = face, v.Texture2DArray.ArraySize = 1;
        }
        else
            v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, v.Texture2D.MipSlice = level;
        ID3D11RenderTargetView* rtv = NULL;
        if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)t->res, &v, &rtv)))
            t->views[i] = (IUnknown*)rtv;
    }
    return t->views[i];
}

static void color_size(uint32_t* w, uint32_t* h)
{
    level_size(g_rt, g_rt_level, w, h);
}

/* the depth surface for the current color target: the bound one, or a scratch one of the color
 * target's size when they differ (D3D8 lets depth be larger; Direct3D 11 wants them equal) */
static GfxTex* depth_attachment(void)
{
    if (!g_ds || !g_rt)
        return NULL;
    uint32_t w, h;
    color_size(&w, &h);
    if (g_ds->w == w && g_ds->h == h)
        return g_ds;
    if (!g_scratch_depth || g_scratch_depth->w != w || g_scratch_depth->h != h || g_scratch_depth->dxfmt != g_ds->dxfmt)
    {
        gfx_tex_destroy(g_scratch_depth);
        g_scratch_depth = gfx_tex_create(GFX_TEX_2D, g_ds->fmt, w, h, 1, GFX_USE_DEPTH);
    }
    return g_scratch_depth;
}

static int bind_targets(void)
{
    if (!g_rt)
        return 0;
    GfxTex* ds = depth_attachment();
    if (!g_rt_face && !g_rt_level)
    {
        if (ds)
            g_rt->depth_seen = ds; /* the scene effects read it */
        g_rt->scene = 0;           /* drawn to: its chain is behind */
    }
    if (g_targets_bound)
        return 1;
    ID3D11RenderTargetView* rtv = (ID3D11RenderTargetView*)target_view(g_rt, g_rt_face, g_rt_level);
    ID3D11DepthStencilView* dsv = ds ? (ID3D11DepthStencilView*)target_view(ds, 0, 0) : NULL;
    if (!rtv)
        return 0;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, dsv);
    bound_forget_textures(); /* a texture now a target was unbound from the stages */
    g_targets_bound = 1;
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
    g_targets_bound = 0;
}

/* --- shaders, compiled off the game's thread ----------------------------------------------------------------
 * A pair for a key seen for the first time is compiled on the thread pool; the draws that need it are
 * left out until it is ready (the game never waits for the compiler). Every key built is recorded in
 * the cache folder, and at start-up the recorded keys are built again in the background, so a second
 * session has them before it needs them. gfx_set_sync_pipelines(1) (the tests) builds in place and
 * records nothing. (gfx_d3d12.c's pipelines, less the state Direct3D 11 keeps apart from shaders.) */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

static int alpha_tested(const GfxFsKey* k) { return k->alpha_func && k->alpha_func != 8; }

typedef struct Shaders
{
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
} Shaders;

#define SHADERS_FAILED ((void*)1)
#define SHADER_MAGIC 0x31443344u /* "D3D1" */

typedef struct ShaderEntry
{
    void* volatile state; /* NULL while building, SHADERS_FAILED, or the Shaders */
    void* volatile job;   /* the start-up rebuild's ShaderJob until a thread takes it to build (warm_callback) */
    int hurried;          /* a draw wanted it before the rebuild got to it: sent ahead on the default pool (shaders_for) */
} ShaderEntry;

typedef struct ShaderJob
{
    LibKey k;
    uint32_t *vs, *ps; /* copies of the shader tokens behind k.vs.prog / fs.prog */
    uint32_t nvs, nps;
    ShaderEntry* e;
    int record;
} ShaderJob;

static Map g_shader_map; /* LibKey -> ShaderEntry, the game's thread's */
static char g_shader_cache[1024];
static CRITICAL_SECTION g_cache_file_lock;

void gfx_set_sync_pipelines(int on) { g_sync_pipelines = on; }
uint32_t gfx_failures(void) { return (uint32_t)g_failures; }

static uint32_t* copy_tok(const uint32_t* t, uint32_t* n)
{
    *n = 0;
    if (!t)
        return NULL;
    uint32_t k = 0;
    while (k < 65536 && t[k] != 0x0000FFFFu)
        k++;
    k++;
    uint32_t* c = (uint32_t*)malloc(4u * k);
    memcpy(c, t, 4u * k);
    *n = k;
    return c;
}

static ID3DBlob* compile(const char* src, const char* entry, const char* target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: HLSL compile failed (%s): %s\n%s\n", entry, err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?",
            src);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static Shaders* build_shaders(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    char* src = gfx_hlsl_generate(&k->vs, &k->fs, vs, ps);
    if (!src)
    {
        InterlockedIncrement(&g_failures);
        return NULL;
    }
    ID3DBlob* v = compile(src, "vs_main", "vs_5_0");
    ID3DBlob* f = v ? compile(src, "fs_main", "ps_5_0") : NULL;
    free(src);
    Shaders* s = NULL;
    if (v && f)
    {
        s = (Shaders*)calloc(1, sizeof *s);
        if (FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &s->vs)) ||
            FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(f), ID3D10Blob_GetBufferSize(f), NULL, &s->ps)))
        {
            InterlockedIncrement(&g_failures);
            if (s->vs)
                ID3D11VertexShader_Release(s->vs);
            free(s);
            s = NULL;
        }
    }
    if (v)
        ID3D10Blob_Release(v);
    if (f)
        ID3D10Blob_Release(f);
    return s;
}

/* one record of the cache file: magic, the key, then each shader's tokens (count first) */
static void record_job(const ShaderJob* j)
{
    if (!g_shader_cache[0])
        return;
    uint32_t hdr[2] = { SHADER_MAGIC, (uint32_t)sizeof(LibKey) };
    EnterCriticalSection(&g_cache_file_lock);
    FILE* f = fopen(g_shader_cache, "ab");
    if (f)
    {
        fwrite(hdr, 4, 2, f);
        fwrite(&j->k, sizeof j->k, 1, f);
        fwrite(&j->nvs, 4, 1, f);
        if (j->nvs)
            fwrite(j->vs, 4, j->nvs, f);
        fwrite(&j->nps, 4, 1, f);
        if (j->nps)
            fwrite(j->ps, 4, j->nps, f);
        fclose(f);
    }
    LeaveCriticalSection(&g_cache_file_lock);
}

static void run_job(ShaderJob* j)
{
    Shaders* s = build_shaders(&j->k, j->vs, j->ps);
    InterlockedExchangePointer(&j->e->state, s ? (void*)s : SHADERS_FAILED);
    if (s && j->record)
        record_job(j);
    free(j->vs);
    free(j->ps);
    free(j);
}

static void CALLBACK job_callback(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst;
    run_job((ShaderJob*)ctx);
}

/* a start-up rebuild's pair, built by whichever comes first: its turn on the rebuild's few threads, or a draw wanting it
 * (shaders_for); the other finds the job taken and does nothing. The entries live as long as the process */
static void CALLBACK warm_callback(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst;
    ShaderJob* j = (ShaderJob*)InterlockedExchangePointer(&((ShaderEntry*)ctx)->job, NULL);
    if (j)
        run_job(j);
}

/* warm: one of the start-up rebuild's (prewarm_shaders), on its own pool (gfx_warm_env) */
static void queue_job(const LibKey* k, const uint32_t* vs, uint32_t nvs, const uint32_t* ps, uint32_t nps, int record, int warm)
{
    if (map_get(&g_shader_map, k, sizeof *k))
        return;
    ShaderEntry* e = (ShaderEntry*)calloc(1, sizeof *e);
    map_put(&g_shader_map, k, sizeof *k, e);
    ShaderJob* j = (ShaderJob*)calloc(1, sizeof *j);
    j->k = *k, j->e = e, j->record = record;
    if (nvs)
        j->vs = (uint32_t*)malloc(4u * nvs), memcpy(j->vs, vs, 4u * nvs), j->nvs = nvs;
    if (nps)
        j->ps = (uint32_t*)malloc(4u * nps), memcpy(j->ps, ps, 4u * nps), j->nps = nps;
    if (warm && !g_sync_pipelines)
    {
        e->job = j;
        if (TrySubmitThreadpoolCallback(warm_callback, e, (PTP_CALLBACK_ENVIRON)gfx_warm_env()))
            return;
        e->job = NULL; /* the game's thread's: no draw has seen the entry yet */
    }
    if (g_sync_pipelines || !TrySubmitThreadpoolCallback(job_callback, j, NULL))
        run_job(j);
}

/* at start-up: the keys earlier sessions built, built again in the background */
static void prewarm_shaders(void)
{
    const char* dir = getenv("FFXI_CACHE_DIR");
    char path[900];
    if (dir && *dir)
        snprintf(path, sizeof path, "%s", dir);
    else if (getenv("LOCALAPPDATA"))
        snprintf(path, sizeof path, "%s\\FFXI", getenv("LOCALAPPDATA"));
    else
        return;
    CreateDirectoryA(path, NULL);
    snprintf(g_shader_cache, sizeof g_shader_cache, "%s\\shaders.d3d11.v1", path);
    FILE* f = fopen(g_shader_cache, "rb");
    if (!f)
        return;
    uint32_t n = 0, hdr[2];
    while (fread(hdr, 4, 2, f) == 2 && hdr[0] == SHADER_MAGIC && hdr[1] == sizeof(LibKey))
    {
        LibKey k;
        uint32_t nvs = 0, nps = 0, *vs = NULL, *ps = NULL;
        int ok = fread(&k, sizeof k, 1, f) == 1 && fread(&nvs, 4, 1, f) == 1 && nvs <= 65536;
        if (ok && nvs)
            vs = (uint32_t*)malloc(4u * nvs), ok = fread(vs, 4, nvs, f) == nvs;
        ok = ok && fread(&nps, 4, 1, f) == 1 && nps <= 65536;
        if (ok && nps)
            ps = (uint32_t*)malloc(4u * nps), ok = fread(ps, 4, nps, f) == nps;
        if (ok)
            queue_job(&k, vs, nvs, ps, nps, 0, 1), n++;
        free(vs);
        free(ps);
        if (!ok)
            break;
    }
    fclose(f);
    fprintf(stderr, "[recomp] gfx: building %u shader pairs from %s in the background, %u at a time\n", n, g_shader_cache,
        gfx_warm_threads() ? gfx_warm_threads() : n);
}

static int g_pair_waiting; /* shaders_for's last NULL was a pair still compiling (not one that failed) */

/* the pair for a draw's key, queued for building the first time (NULL until built) */
static Shaders* shaders_for(const LibKey* k, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    g_pair_waiting = 0;
    /* draws in a row mostly share a pair: the last one's key is compared before any hashing */
    static LibKey last_key;
    static ShaderEntry* last;
    ShaderEntry* e = last && !memcmp(k, &last_key, sizeof *k) ? last : NULL;
    if (!e)
    {
        e = (ShaderEntry*)map_get(&g_shader_map, k, sizeof *k);
        if (!e)
        {
            uint32_t nvs = 0, nps = 0;
            uint32_t* vs = k->vs.prog ? copy_tok(vs_tokens, &nvs) : NULL;
            uint32_t* ps = k->fs.prog ? copy_tok(ps_tokens, &nps) : NULL;
            queue_job(k, vs, nvs, ps, nps, !g_sync_pipelines, 0);
            free(vs);
            free(ps);
            e = (ShaderEntry*)map_get(&g_shader_map, k, sizeof *k);
        }
        if (!e)
            return NULL;
        last_key = *k, last = e;
    }
    void* st = InterlockedCompareExchangePointer(&e->state, NULL, NULL);
    if (!st && e->job && !e->hurried)
    {
        /* still waiting its turn in the start-up rebuild: a draw wants it now, so it goes ahead on the default pool */
        e->hurried = 1;
        TrySubmitThreadpoolCallback(warm_callback, e, NULL);
    }
    g_pair_waiting = st == NULL;
    return st && st != SHADERS_FAILED ? (Shaders*)st : NULL;
}

/* --- fixed state: blending, depth-stencil, rasterizer, samplers ------------------------------------------------ */
static D3D11_BLEND blend_factor(uint32_t f, int x8, int alpha)
{
    switch (f)
    {
    case 1: return D3D11_BLEND_ZERO;
    case 2: return D3D11_BLEND_ONE;
    case 3: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR; /* the alpha factors take no colors */
    case 4: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 5: return D3D11_BLEND_SRC_ALPHA;
    case 6: return D3D11_BLEND_INV_SRC_ALPHA;
    case 7: return x8 ? D3D11_BLEND_ONE : D3D11_BLEND_DEST_ALPHA;
    case 8: return x8 ? D3D11_BLEND_ZERO : D3D11_BLEND_INV_DEST_ALPHA;
    case 9: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 10: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 11: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D11_BLEND_OP_MIN;
    case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC compare(uint32_t f)
{
    switch (f)
    {
    case 1: return D3D11_COMPARISON_NEVER;
    case 2: return D3D11_COMPARISON_LESS;
    case 3: return D3D11_COMPARISON_EQUAL;
    case 4: return D3D11_COMPARISON_LESS_EQUAL;
    case 5: return D3D11_COMPARISON_GREATER;
    case 6: return D3D11_COMPARISON_NOT_EQUAL;
    case 7: return D3D11_COMPARISON_GREATER_EQUAL;
    default: return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D11_STENCIL_OP_ZERO;
    case 3: return D3D11_STENCIL_OP_REPLACE;
    case 4: return D3D11_STENCIL_OP_INCR_SAT;
    case 5: return D3D11_STENCIL_OP_DECR_SAT;
    case 6: return D3D11_STENCIL_OP_INVERT;
    case 7: return D3D11_STENCIL_OP_INCR;
    case 8: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

typedef struct BlendKey
{
    GfxPipeKey pipe;
    uint8_t x8, pad[3];
} BlendKey;

static ID3D11BlendState* blend_state(const GfxPipeKey* p, int x8)
{
    BlendKey k;
    memset(&k, 0, sizeof k);
    k.pipe = *p, k.x8 = (uint8_t)x8;
    memset(k.pipe.pad, 0, sizeof k.pipe.pad);
    ID3D11BlendState* s = (ID3D11BlendState*)map_get(&g_blends, &k, sizeof k);
    if (s)
        return s;
    D3D11_BLEND_DESC d;
    memset(&d, 0, sizeof d);
    D3D11_RENDER_TARGET_BLEND_DESC* c = &d.RenderTarget[0];
    uint32_t wm = p->write_mask;
    c->RenderTargetWriteMask = (UINT8)(((wm & 1) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) | ((wm & 2) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((wm & 4) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | ((wm & 8) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
    c->SrcBlend = c->SrcBlendAlpha = D3D11_BLEND_ONE;
    c->DestBlend = c->DestBlendAlpha = D3D11_BLEND_ZERO;
    c->BlendOp = c->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (p->blend)
    {
        uint32_t sf = p->src, df = p->dst;
        if (sf == 12) /* BOTHSRCALPHA */
            sf = 5, df = 6;
        else if (sf == 13) /* BOTHINVSRCALPHA */
            sf = 6, df = 5;
        c->BlendEnable = TRUE;
        c->SrcBlend = blend_factor(sf, x8, 0), c->SrcBlendAlpha = blend_factor(sf, x8, 1);
        c->DestBlend = blend_factor(df, x8, 0), c->DestBlendAlpha = blend_factor(df, x8, 1);
        c->BlendOp = c->BlendOpAlpha = blend_op(p->op);
        if (p->over) /* the interface layer: its alpha is how much of the frame each pixel covers */
            c->SrcBlendAlpha = D3D11_BLEND_ONE, c->DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA, c->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    if (FAILED(ID3D11Device_CreateBlendState(g_dev, &d, &s)))
        return NULL;
    map_put(&g_blends, &k, sizeof k, s);
    return s;
}

static ID3D11DepthStencilState* depth_state(const GfxDepthKey* k)
{
    ID3D11DepthStencilState* s = (ID3D11DepthStencilState*)map_get(&g_depths, k, sizeof *k);
    if (s)
        return s;
    D3D11_DEPTH_STENCIL_DESC d;
    memset(&d, 0, sizeof d);
    d.DepthEnable = k->zenable ? TRUE : FALSE;
    d.DepthWriteMask = k->zenable && k->zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = k->zenable ? compare(k->zfunc) : D3D11_COMPARISON_ALWAYS;
    if (k->stencil)
    {
        D3D11_DEPTH_STENCILOP_DESC st = { stencil_op(k->sfail), stencil_op(k->szfail), stencil_op(k->spass), compare(k->sfunc) };
        d.StencilEnable = TRUE;
        d.StencilReadMask = k->sread;
        d.StencilWriteMask = k->swrite;
        d.FrontFace = st;
        d.BackFace = st;
    }
    if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &d, &s)))
        return NULL;
    map_put(&g_depths, k, sizeof *k, s);
    return s;
}

typedef struct RasterKey
{
    uint8_t cull, fill, shadow, noclip; /* shadow: the sun's map's own bias (sun_draw); noclip: GfxDepthKey.noclip */
    int32_t zbias;
} RasterKey;

static ID3D11RasterizerState* raster_state(const RasterKey* k)
{
    ID3D11RasterizerState* s = (ID3D11RasterizerState*)map_get(&g_rasters, k, sizeof *k);
    if (s)
        return s;
    D3D11_RASTERIZER_DESC d;
    memset(&d, 0, sizeof d);
    /* D3D's front faces are clockwise on screen; CULL_CCW (the default) culls the back ones */
    d.FillMode = k->fill == 2 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    d.CullMode = k->cull == 3 ? D3D11_CULL_BACK : k->cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    d.FrontCounterClockwise = FALSE;
    d.DepthBias = -k->zbias; /* as in the D3D12 backend: a unit a step of D3DRS_ZBIAS, the rest a distance (gfx_hlsl_vs_return) */
    d.SlopeScaledDepthBias = -(float)k->zbias * 0.03125f;
    if (k->shadow) /* the map's own bias: surfaces at a slant to the sun are pushed back more */
        d.DepthBias = 0, d.SlopeScaledDepthBias = 1.5f;
    d.DepthClipEnable = k->noclip ? FALSE : TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &d, &s)))
        return NULL;
    map_put(&g_rasters, k, sizeof *k, s);
    return s;
}

static D3D11_TEXTURE_ADDRESS_MODE address(uint32_t a)
{
    switch (a)
    {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static ID3D11SamplerState* sampler(const GfxSampler* k)
{
    ID3D11SamplerState* s = (ID3D11SamplerState*)map_get(&g_samplers, k, sizeof *k);
    if (s)
        return s;
    D3D11_SAMPLER_DESC d;
    memset(&d, 0, sizeof d);
    int mn = k->min >= 2, mg = k->mag >= 2, mp = k->mip >= 2; /* linear, else point */
    d.Filter = (D3D11_FILTER)((mn ? 0x10 : 0) | (mg ? 0x4 : 0) | (mp ? 0x1 : 0));
    d.MaxAnisotropy = 1;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1)
        d.Filter = D3D11_FILTER_ANISOTROPIC, d.MaxAnisotropy = k->max_aniso > 16 ? 16 : k->max_aniso;
    d.AddressU = address(k->addr_u);
    d.AddressV = address(k->addr_v);
    d.AddressW = address(k->addr_w);
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    d.BorderColor[0] = ((k->border >> 16) & 255) / 255.0f;
    d.BorderColor[1] = ((k->border >> 8) & 255) / 255.0f;
    d.BorderColor[2] = (k->border & 255) / 255.0f;
    d.BorderColor[3] = (k->border >> 24) / 255.0f;
    d.MinLOD = k->max_level;
    d.MaxLOD = k->mip == 0 ? (float)k->max_level : D3D11_FLOAT32_MAX; /* no mipmapping: that one level */
    d.MipLODBias = k->lod_bias / 16.0f; /* FSR's, for the world drawn smaller (up_lod_bias) */
    if (k->lod_cap && d.MaxLOD > (float)(k->lod_cap - 1))
        d.MaxLOD = (float)(k->lod_cap - 1);
    if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &d, &s)))
        return g_linear; /* any sampler beats none */
    map_put(&g_samplers, k, sizeof *k, s);
    return s;
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
static void set_viewport(const uint32_t vp[6])
{
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    uint32_t w, h;
    color_size(&w, &h);
    float x = (float)vp[0], y = (float)vp[1], vw = (float)vp[2], vh = (float)vp[3];
    if (x > w)
        x = (float)w;
    if (y > h)
        y = (float)h;
    if (x + vw > w)
        vw = w - x;
    if (y + vh > h)
        vh = h - y;
    D3D11_VIEWPORT v = { x, y, vw, vh, zmin, zmax };
    if (memcmp(&v, &g_bound.vp, sizeof v))
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v), g_bound.vp = v;
}

/* index count for a D3D primitive count */
static uint32_t vertex_count(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case GFX_POINTLIST: return n;
    case GFX_LINELIST: return n * 2;
    case GFX_LINESTRIP: return n + 1;
    case GFX_TRIANGLELIST: return n * 3;
    case GFX_TRIANGLESTRIP: return n + 2;
    case GFX_TRIANGLEFAN: return n * 3; /* as a list */
    default: return 0;
    }
}

static D3D11_PRIMITIVE_TOPOLOGY topology(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case GFX_LINELIST: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case GFX_LINESTRIP: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case GFX_TRIANGLESTRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static void set_topology(D3D11_PRIMITIVE_TOPOLOGY t)
{
    if ((int)t != g_bound.topo)
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, t), g_bound.topo = (int)t;
}

static void set_index_buffer(ID3D11Buffer* b, DXGI_FORMAT f, uint32_t off)
{
    if (b != g_bound.ib || f != g_bound.ib_fmt || off != g_bound.ib_off)
        ID3D11DeviceContext_IASetIndexBuffer(g_ctx, b, f, off), g_bound.ib = b, g_bound.ib_fmt = f, g_bound.ib_off = off;
}

/* the bytes of the uniforms a draw's functions read: the lights only when lit, the vertex shader's
 * constants only for a vertex shader, the pixel shader's only for a pixel shader (the range bound covers
 * the whole struct: that's what the functions are compiled against) */
static size_t uniforms_need(const GfxDraw* d)
{
    size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        need = offsetof(GfxU, psc);
    if (d->fs.prog)
        need = sizeof(GfxU);
    return need;
}

/* each input register's offset moved by its stream's place in the buffer it's bound from */
static void patch_offsets(GfxU* to, const GfxU* from, const GfxVsKey* vs, const uint32_t base[GFX_NSTREAMS])
{
    for (int reg = 0; reg < GFX_NREGS; ++reg)
        if (vs->el[reg].used && vs->el[reg].stream < GFX_NSTREAMS)
            to->offset[reg] = from->offset[reg] + (int32_t)base[vs->el[reg].stream];
}

/* `need` bytes of uniforms into the ring (their offsets moved by base when it's given): the range's place
 * in *at. 0 when it can't. */
static int uniforms_put(const GfxU* src, size_t need, const GfxVsKey* vs, const uint32_t* base, uint32_t* at)
{
    Ring* r = &g_cring;
    uint32_t bytes = U_CONSTANTS * 16;
    uint32_t off = g_ctx1 ? (r->at + 255) & ~255u : 0;
    D3D11_MAP mode = g_ctx1 ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
    if (off + bytes > r->size || r->discard_only)
        off = 0, mode = D3D11_MAP_WRITE_DISCARD;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)r->buf, 0, mode, 0, &m)))
        return 0;
    if (mode == D3D11_MAP_WRITE_DISCARD)
        r->gen++;
    GfxU* u = (GfxU*)((uint8_t*)m.pData + off);
    memcpy(u, src, need);
    if (base)
        patch_offsets(u, src, vs, base);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)r->buf, 0);
    r->at = off + bytes;
    *at = off;
    return 1;
}

/* the ring's range at `at` as the uniforms (b0) */
static void uniforms_bind(uint32_t at)
{
    Ring* r = &g_cring;
    if (g_ctx1)
    {
        UINT first = at / 16, count = U_CONSTANTS;
        ID3D11DeviceContext1_VSSetConstantBuffers1(g_ctx1, 0, 1, &r->buf, &first, &count);
        ID3D11DeviceContext1_PSSetConstantBuffers1(g_ctx1, 0, 1, &r->buf, &first, &count);
    }
    else
    {
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &r->buf);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &r->buf);
    }
}

/* --- the sun's shadow casters ------------------------------------------------------------------------------------
 * Each opaque draw of the scene (GfxDraw.caster) is recorded as it's drawn - its functions' key, the
 * buffers its vertices and indices come from, its uniforms, an alpha test's textures and samplers - and
 * when the scene is done they're drawn again into the sun's maps (sun_draw): the same functions, with the
 * clip-space position they make taken through the camera's inverse into the sun's view (the shadow key,
 * gfx_hlsl_vs_return). gfx_d3d12.c's, but for Direct3D 11's buffers: the uniforms' ring moves on (a DISCARD)
 * every few thousand draws, and what was written before is gone, so a caster's uniforms are kept in memory
 * too (g_caster_u) and written into the ring again when it moved on since. Its vertices and indices stay
 * where the frame wrote them (Chunks); only where every write DISCARDs (no NO_OVERWRITE for them) is one
 * whose chunk was written again left out. */
typedef struct Caster
{
    LibKey lib;
    const uint32_t *vs, *ps;       /* the game's shader tokens behind lib.vs.prog / fs.prog */
    GfxBuf* buf[GFX_NSTREAMS + 1]; /* the static buffers it draws from (the last: its indices); NULL: a ring's, or none */
    uint32_t base[GFX_NSTREAMS];   /* each stream's place in its buffer (the vertex ring's for a ring stream) */
    uint8_t ring[GFX_NSTREAMS];    /* the stream's vertices are in this frame's vertex bytes (g_vring) */
    uint8_t iring;                 /* its indices are in this frame's index bytes (g_iring) */
    uint16_t vchunk, ichunk;       /* ... their chunks */
    uint32_t vgen, igen;           /* ... and those chunks' generations then */
    uint32_t ioff;                 /* its first index's byte in its index buffer */
    DXGI_FORMAT ifmt;              /* its indices' format; UNKNOWN: none */
    uint32_t uoff, ugen;           /* its uniforms' place in the uniforms' ring, and that ring's generation then */
    uint32_t cpu_u;                /* ... and in g_caster_u */
    ID3D11ShaderResourceView* srv[16]; /* an alpha test's textures (t4..t19) and samplers */
    ID3D11SamplerState* samp[8];
    GfxTex* tex[8];
    uint32_t n, vstart, need;      /* need: the uniforms' bytes */
    uint8_t topo;                  /* D3D11_PRIMITIVE_TOPOLOGY */
    uint8_t fixed;                 /* every vertex and index from buffers the game keeps (the zone's): cached (sun_cache) */
    uint8_t dead;                  /* a buffer or texture it draws from went, or changed: not drawn */
    uint8_t has_clip0;             /* clip0 is known (a fixed one: gfx_draw_clip0) */
    uint8_t own;                   /* vertices or indices of a buffer the game rewrites, as drawn, in the ring (GFX_CASTER_COPY_MOST) */
    uint8_t cull;                  /* its D3DCULL, and whether srv and samp hold every stage's (not an alpha test's alone): */
    uint8_t textured;              /* for the water's mirror, which draws it again in colour (gfx_d3d11_mirror.h) */
    float clip0[4];                /* its first vertex's clip-space position: where it stands (sun_cache_update) */
} Caster;
static int mirror_wanted(void); /* gfx_d3d11_mirror.h: the water asks for the mirror, so the casters are kept for it too */

static Caster* g_casters;
static uint32_t g_ncasters, g_casters_cap;
static void snap_note(const GfxDraw* d, Caster* rec);
#define SNAP_VOLATILE 300 /* frames */
/* a buffer the game has written again lately: the cache can't draw from it later (a copy is kept) */
#define VOLATILE(b) ((b)->uploads > 1 && (b)->upload_frame + SNAP_VOLATILE > g_serial)
/* ... and written again within a frame lately: between draws from it, so what was drawn before is gone by the scene's end */
#define REWRITTEN(b) ((b)->twice && (b)->twice + SNAP_VOLATILE > g_serial)
/* A caster's vertices or indices from a buffer the game rewrites between draws (REWRITTEN) are copied into the frame's ring
 * as they are when drawn, up to this many bytes a stream (from XI on Anything, rubymatrix/xi-on-anything,
 * runtime/portable/gfx_d3d12.c: caster_snapshot). The game sways each tree of a kind through one buffer, uploading it
 * again before the next tree is drawn, and the sun's maps are drawn once the scene is done: drawn from the buffer, every
 * tree but the last would have the last one's vertices, so the upload let the earlier ones go (sun_forget) and only the
 * last tree of a kind cast a shadow. Past it (or GFX_CASTER_COPY_FRAME a frame), or when the ring can't take them, the caster
 * reads the buffer, as before. (A caster drawn from a buffer before its first upload of a frame still reads the buffer, and
 * that upload lets it go: the first frame a buffer is rewritten, it isn't REWRITTEN yet.) */
static uint8_t* g_caster_u; /* this frame's casters' uniforms, as drawn */
static uint32_t g_caster_u_len, g_caster_u_cap;

static void casters_clear(void)
{
    g_ncasters = 0;
    g_caster_u_len = 0;
}

/* NULL when there is no room for another: the draw isn't kept for the shadows, and is drawn all the same */
static Caster* caster_new(const GfxDraw* d)
{
    if (g_ncasters == g_casters_cap)
    {
        const uint32_t cap = g_casters_cap ? g_casters_cap * 2 : 1024;
        Caster* more = (Caster*)realloc(g_casters, cap * sizeof(Caster));
        if (!more)
            return NULL;
        g_casters = more, g_casters_cap = cap;
    }
    Caster* c = &g_casters[g_ncasters++];
    memset(c, 0, sizeof *c);
    c->lib.vs = d->vs, c->lib.fs = d->fs;
    c->vs = gfx_tokens_kept(d->vs.prog, 0, d->vs_tokens), c->ps = gfx_tokens_kept(d->fs.prog, 1, d->ps_tokens);
    c->fixed = d->prim != GFX_TRIANGLEFAN && (!d->indices || d->ibuf);
    c->cull = d->cull;
    c->dead = 1; /* until it is drawn */
    return c;
}

/* room for a caster's uniforms in g_caster_u: its place there. NULL when a block that big isn't to be found (a 32-bit game's
 * address space runs out at a long draw distance): the buffer stays as it was, and the draw isn't kept for the shadows */
static GfxU* caster_u(size_t need, uint32_t* at)
{
    if (g_caster_u_len + need > g_caster_u_cap)
    {
        uint32_t cap = (g_caster_u_len + (uint32_t)need) * 2 + 65536;
        uint8_t* more = (uint8_t*)realloc(g_caster_u, cap);
        if (!more) /* as much as it needs, and a little more */
            more = (uint8_t*)realloc(g_caster_u, cap = g_caster_u_len + (uint32_t)need + 65536);
        if (!more)
            return NULL;
        g_caster_u = more, g_caster_u_cap = cap;
    }
    *at = g_caster_u_len;
    g_caster_u_len += ((uint32_t)need + 15) & ~15u;
    return (GfxU*)(g_caster_u + *at);
}

/* The scene filter: a large render target drawn smaller onto a large target (FFXI's world onto the
 * back buffer - or the copy of it the game shows some frames, while the camera moves) is sampled
 * through its mips, made here from it as it is now. Every frame alike: when only some were, the fine
 * detail of the sky flickered between the two. */
static void scene_mips(const GfxDraw* d)
{
    if (g_fxs.fx == 0.0f || g_fxs.filter == 0.0f || !d->vs.rhw || !g_rt)
        return;
    uint32_t tw, th;
    color_size(&tw, &th);
    if (tw < 512 || th < 512 || d->small_rt)
        return; /* onto a large target only: not the flare's probe, nor the game's 256x256 targets (gfx_d3d12.c's) */
    for (int i = 0; i < 8; ++i)
    {
        GfxTex* t = d->tex[i];
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        if (!wanted || !t || t == g_rt || !t->srv_mips || t->mips < 2 || t->scene)
            continue;
        if (t->w <= tw && t->h <= th)
            continue; /* not made smaller */
        ID3D11DeviceContext_GenerateMips(g_ctx, t->srv_mips);
        t->scene = g_serial;
    }
}

/* each stage mostly samples the same way draw after draw: its last key before the lookup */
static ID3D11SamplerState* stage_sampler(int i, const GfxSampler* k)
{
    static GfxSampler key[8];
    static ID3D11SamplerState* last[8];
    if (last[i] && !memcmp(k, &key[i], sizeof *k))
        return last[i];
    key[i] = *k;
    return last[i] = sampler(k);
}

/* the draw itself: n vertices or indices */
static void draw_call(const GfxDraw* d, uint32_t n, int indexed)
{
    if (indexed)
        ID3D11DeviceContext_DrawIndexed(g_ctx, n, 0, 0);
    else
        ID3D11DeviceContext_Draw(g_ctx, n, d->vertex_start);
}

static int up_on_draw(const GfxDraw* d);
static void up_draw_over(const GfxDraw* d);
static GfxTex* up_sampled(GfxTex* t);
static int8_t up_lod_bias(void);
static void draw_main(const GfxDraw* d);

/* the zone's draws sent together is Direct3D 12's (gfx_d3d12_batch.h): here they're drawn as they come */
void gfx_batch(int open) { (void)open; }

void gfx_draw(const GfxDraw* d)
{
    if (!g_dev || !d->count || d->sought) /* (the caster pass's draws are Direct3D 12's sun maps' alone: gfx_seek_reach is 0 here) */
        return;
    /* FSR: the scene through it once it's all drawn (the first draw reading it); the interface drawn into it before then
     * held, and drawn over FSR's output (at its size) after */
    const int up = up_on_draw(d);
    if (up)
    {
        if (up == 1)
            up_draw_over(d); /* else held, drawn over it later */
        return;
    }
    draw_main(d);
}

/* a caster's uniforms kept in memory too, as drawn (caster_u: the ring moves on before the scene is done, often), into *cu; the
 * caster itself, or NULL with no room to keep them: it stays dead (not kept for the shadows), and is drawn as any draw */
static Caster* caster_with_u(Caster* rec, size_t need, GfxU** cu)
{
    *cu = rec ? caster_u(need, &rec->cpu_u) : NULL;
    return *cu ? rec : NULL;
}

/* the draw itself (gfx_draw's, and FSR's held draws drawn again: up_replay) */
static void draw_main(const GfxDraw* d)
{
    if (!g_dev || !d->count)
        return;
    if (!bind_targets())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    scene_mips(d);
    GfxTex* ds = depth_attachment();
    LibKey k;
    memset(&k, 0, sizeof k);
    k.vs = d->vs, k.fs = d->fs;
    /* the world's lit draws lit per pixel (gfx_hlsl.c pixel_lit) */
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
        k.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1; /* 1: the sun per pixel, the game's torches per vertex */
    /* the normal maps, as Direct3D 12 has them (gfx_d3d12.c): the lit pixel function's normal bent, or the game's own shaders lit
     * again from the vertex shader's clip-space position (gfx_hlsl.c emit_normal_relight). No detail pass here */
    if (g_fxs.normalmap < 0.5f || (k.fs.prog ? !k.vs.prog : !k.vs.pixel))
        k.fs.nmap = 0;
    else if (k.fs.nmap)
    {
        k.fs.nmap = (uint8_t)(1 + (g_fxs.normalmap_view >= 1.5f ? 2 : g_fxs.normalmap_view >= 0.5f ? 1 : 0));
        k.vs.cpos = k.fs.prog != 0;
    }
    Shaders* sh = shaders_for(&k, d->vs_tokens, d->ps_tokens);
    if (!sh && k.vs.pixel)
    {
        /* lit per pixel, still compiling: lit per vertex meanwhile (a new mix of the game's lights would
         * blink the object out for a few frames) */
        k.vs.pixel = 0;
        sh = shaders_for(&k, d->vs_tokens, d->ps_tokens);
    }
    if (!sh)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE); /* still compiling (or failed) */
        return;
    }
    /* the fixed state */
    ID3D11BlendState* blend = blend_state(&d->pipe, g_rt->x8);
    GfxDepthKey dk;
    memset(&dk, 0, sizeof dk);
    if (ds)
    {
        dk = d->depth;
        if (!ds->has_stencil)
            dk.stencil = 0;
        if (!dk.stencil)
            dk.sfail = dk.szfail = dk.spass = dk.sfunc = dk.sread = dk.swrite = 0;
        dk.noclip = 0, memset(dk.pad, 0, sizeof dk.pad); /* (noclip is the rasterizer's, below) */
    }
    ID3D11DepthStencilState* depth = depth_state(&dk);
    RasterKey rk = { d->cull, (uint8_t)(d->fill == 2 ? 2 : 3), 0, (uint8_t)(ds && d->depth.noclip), ds ? d->zbias : 0 };
    ID3D11RasterizerState* raster = raster_state(&rk);
    if (!blend || !depth || !raster)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE);
        return;
    }
    if (blend != g_bound.blend)
    {
        static const float factor[4] = { 1, 1, 1, 1 };
        ID3D11DeviceContext_OMSetBlendState(g_ctx, blend, factor, 0xFFFFFFFFu), g_bound.blend = blend;
    }
    uint32_t ref = dk.stencil ? d->stencil_ref & 0xFF : 0;
    if (depth != g_bound.depth || ref != g_bound.stencil_ref)
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth, ref), g_bound.depth = depth, g_bound.stencil_ref = ref;
    if (raster != g_bound.raster)
        ID3D11DeviceContext_RSSetState(g_ctx, raster), g_bound.raster = raster;
    set_viewport(d->vp);
    if (sh->vs != g_bound.vs)
        ID3D11DeviceContext_VSSetShader(g_ctx, sh->vs, NULL, 0), g_bound.vs = sh->vs;
    if (sh->ps != g_bound.ps)
        ID3D11DeviceContext_PSSetShader(g_ctx, sh->ps, NULL, 0), g_bound.ps = sh->ps;

    /* the stages' textures and samplers: an unbound slot reads as the null view (zeros) */
    ID3D11ShaderResourceView* tex[16];
    ID3D11SamplerState* samp[8];
    memset(tex, 0, sizeof tex);
    for (int i = 0; i < 8; ++i)
    {
        int wanted = d->fs.prog || i < d->fs.nstages || (i == 7 && (d->fs.rain == 2 || k.fs.nmap)) ? d->fs.st[i].tex : 0;
        samp[i] = g_linear;
        if (!wanted)
            continue;
        GfxTex* t = up_sampled(d->tex[i]); /* the scene's target once FSR has made it larger: its output */
        GfxSampler sk = d->samp[i];
        ID3D11ShaderResourceView* view = t ? t->srv : NULL;
        if (t && g_fxs.fx != 0.0f)
        {
            /* the world's solid textures: Texture Filtering's mode and sharpness (gfx.h gfx_world_filter) */
            gfx_world_filter(&sk, d, i, t->scene, t->levels, t->filled, g_fxs.aniso, g_fxs.tex_sharp);
            /* the finished scene made smaller (FFXI's background onto the back buffer): through its
             * mips (scene_mips), anisotropic for a squeeze that differs across and down */
            if (t->scene && t->srv_mips)
                sk.min = 3, sk.mag = 2, sk.mip = 2, sk.max_aniso = 16, sk.max_level = 0, view = t->srv_mips;
        }
        /* the world drawn smaller for FSR: its textures' mips chosen as at the size it's shown at (log2 of the render
         * size over the shown one), else they're a level or two blurrier than FSR makes the rest */
        if (t && !d->vs.rhw && sk.mip >= 1 && !t->scene)
            sk.lod_bias = (int8_t)(sk.lod_bias + up_lod_bias()); /* (Texture Sharpness's with it) */
        samp[i] = stage_sampler(i, &sk);
        if (!t || !view || t == g_rt || (wanted == 2) != (t->type == GFX_TEX_CUBE))
            continue; /* unbound, or the target being drawn to */
        tex[wanted == 2 ? 8 + i : i] = view;
    }
    if (memcmp(tex, g_bound.tex, sizeof tex))
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 16, tex), memcpy(g_bound.tex, tex, sizeof tex);
    if (memcmp(samp, g_bound.samp, sizeof samp))
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 8, samp), memcpy(g_bound.samp, samp, sizeof samp);

    /* the scene's casters: the depth they are drawn with is the scene's (the effects read it), and each
     * is kept for the sun's shadow maps */
    if (d->caster && !g_rt_face && !g_rt_level && ds)
        g_rt->depth_world = ds;
    const int mirrored = mirror_wanted(); /* the water's mirror draws the casters again, in colour */
    Caster* rec = d->caster && ((g_fxs.fx != 0.0f && g_fxs.sun > 0.0f) || mirrored) ? caster_new(d) : NULL;

    /* the vertex streams: a static buffer's own view, or this draw's bytes in the ring's (all of them in
     * one write). A caster's from a buffer the game rewrites between draws go in that write too, as they are
     * now (GFX_CASTER_COPY_MOST): the caster's alone, the draw reads the buffer */
    ID3D11ShaderResourceView* streams[GFX_NSTREAMS];
    uint32_t base[GFX_NSTREAMS], cbase[GFX_NSTREAMS]; /* cbase: the caster's, its copies' places among them */
    const void* up[2 * GFX_NSTREAMS];
    uint32_t up_n[2 * GFX_NSTREAMS], up_at[2 * GFX_NSTREAMS];
    int nup = 0, up_s[GFX_NSTREAMS], ncopy = 0, copy_s[GFX_NSTREAMS];
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        streams[s] = NULL, base[s] = 0;
        if (d->buf[s])
        {
            streams[s] = d->buf[s]->srv, base[s] = d->buf_off[s];
            if (rec)
            {
                if (VOLATILE(d->buf[s]))
                    rec->fixed = 0;
                /* a copy kept to the scene's end: not where every write DISCARDs */
                if (REWRITTEN(d->buf[s]) && d->cpu[s] && d->cpu_size[s] && d->cpu_size[s] <= GFX_CASTER_COPY_MOST && !g_vring.discard_only &&
                    gfx_caster_copy_ok(d->cpu_size[s], g_serial))
                    copy_s[ncopy++] = s;
                else
                    rec->buf[s] = d->buf[s], d->buf[s]->sun_seen = g_serial;
            }
        }
        else if (d->data[s] && d->size[s])
            up[nup] = d->data[s], up_n[nup] = d->size[s], up_s[nup++] = s;
    }
    int vc = -1;
    if (ncopy)
    {
        for (int i = 0; i < ncopy; ++i)
            up[nup + i] = d->cpu[copy_s[i]], up_n[nup + i] = d->cpu_size[copy_s[i]];
        vc = chunks_putv(&g_vring, up, up_n, nup + ncopy, 16, up_at);
        if (vc < 0) /* no room: the caster reads the buffers, as before */
        {
            for (int i = 0; i < ncopy; ++i)
                rec->buf[copy_s[i]] = d->buf[copy_s[i]], d->buf[copy_s[i]]->sun_seen = g_serial;
            ncopy = 0;
        }
    }
    if (nup && vc < 0)
        vc = chunks_putv(&g_vring, up, up_n, nup, 16, up_at);
    if (nup && vc < 0)
    {
        gfx_prof_skip(GFX_SKIP_NO_BUFFER);
        return;
    }
    for (int i = 0; i < nup; ++i)
    {
        streams[up_s[i]] = g_vring.c[vc].srv, base[up_s[i]] = up_at[i];
        if (rec)
            rec->ring[up_s[i]] = 1, rec->fixed = 0;
    }
    memcpy(cbase, base, sizeof base);
    for (int i = 0; i < ncopy; ++i)
        cbase[copy_s[i]] = up_at[nup + i], rec->ring[copy_s[i]] = 1, rec->own = 1, rec->fixed = 0;
    if (rec && vc >= 0)
        rec->vchunk = (uint16_t)vc, rec->vgen = g_vring.c[vc].gen;
    if (memcmp(streams, g_bound.streams, sizeof streams))
        ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, GFX_NSTREAMS, streams), memcpy(g_bound.streams, streams, sizeof streams);
    size_t need = uniforms_need(d);
    uint32_t uoff = 0;
    const GfxU* du = &d->u;
    GfxU un;
    if (k.fs.nmap) /* the normal maps' strength, in the fog density's neighbour (as gfx_d3d12.c has it) */
        memcpy(&un, &d->u, need), un.params2[1] = g_fxs.normalmap_strength, du = &un;
    GfxU* cu; /* its uniforms kept in memory too, as drawn (caster_with_u) */
    rec = caster_with_u(rec, need, &cu);
    if (rec)
    {
        memcpy(cu, du, need);
        patch_offsets(cu, du, &d->vs, cbase);
        /* with copies of its own its places aren't the draw's: the draw's uniforms go in apart, and the caster's (cu) as
         * the maps are drawn - its generation is never the ring's, so sun_draw puts them in from the kept copy */
        if (ncopy ? !uniforms_put(du, need, &d->vs, base, &uoff) : !uniforms_put(cu, need, NULL, NULL, &uoff))
            return;
        memcpy(rec->base, cbase, sizeof cbase);
        rec->uoff = uoff, rec->ugen = ncopy ? g_cring.gen - 1u : g_cring.gen, rec->need = (uint32_t)need;
        rec->topo = (uint8_t)topology(d->prim), rec->n = vertex_count(d->prim, d->count), rec->vstart = d->vertex_start;
        rec->ifmt = DXGI_FORMAT_UNKNOWN;
        if (alpha_tested(&d->fs) || mirrored)
        {
            rec->textured = (uint8_t)mirrored;
            memcpy(rec->srv, tex, sizeof tex);
            memcpy(rec->samp, samp, sizeof samp);
            for (int i = 0; i < 8; ++i)
                if (tex[i] || tex[8 + i])
                    rec->tex[i] = d->tex[i], d->tex[i]->sun_seen = g_serial;
        }
    }
    else if (!uniforms_put(du, need, &d->vs, base, &uoff))
        return;
    uniforms_bind(uoff);

    uint32_t n = vertex_count(d->prim, d->count);
    set_topology(topology(d->prim));
    if (d->prim == GFX_TRIANGLEFAN)
    {
        /* no fans in Direct3D 11: a list with the same vertices */
        uint32_t* idx = (uint32_t*)malloc((size_t)n * 4);
        if (!idx)
            return;
        for (uint32_t i = 0; i < d->count; ++i)
        {
            uint32_t kk[3] = { 0, i + 1, i + 2 };
            for (int j = 0; j < 3; ++j)
            {
                if (!d->indices)
                    idx[3 * i + j] = d->vertex_start + kk[j];
                else if (d->index_size == 2)
                    idx[3 * i + j] = ((const uint16_t*)d->indices)[kk[j]];
                else
                    idx[3 * i + j] = ((const uint32_t*)d->indices)[kk[j]];
            }
        }
        uint32_t off = 0;
        int ic = chunks_put(&g_iring, idx, n * 4, 4, &off);
        free(idx);
        if (ic < 0)
            return;
        set_index_buffer(g_iring.c[ic].buf, DXGI_FORMAT_R32_UINT, off);
        draw_call(d, n, 1);
        if (rec)
            rec->iring = 1, rec->ichunk = (uint16_t)ic, rec->igen = g_iring.c[ic].gen, rec->ioff = off, rec->ifmt = DXGI_FORMAT_R32_UINT;
    }
    else if (d->ibuf)
    {
        DXGI_FORMAT f = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        set_index_buffer(d->ibuf->res, f, d->ibuf_off);
        draw_call(d, n, 1);
        if (rec)
        {
            rec->ioff = d->ibuf_off, rec->ifmt = f;
            /* as the vertices: indices the game rewrites between draws, as they are now, in the ring (GFX_CASTER_COPY_MOST) */
            int ic = -1;
            uint32_t off = 0;
            if (VOLATILE(d->ibuf))
                rec->fixed = 0;
            if (REWRITTEN(d->ibuf) && d->indices && (size_t)n * d->index_size <= GFX_CASTER_COPY_MOST && !g_iring.discard_only &&
                gfx_caster_copy_ok(n * d->index_size, g_serial))
                ic = chunks_put(&g_iring, d->indices, n * d->index_size, 4, &off);
            if (ic >= 0)
                rec->iring = 1, rec->ichunk = (uint16_t)ic, rec->igen = g_iring.c[ic].gen, rec->ioff = off, rec->own = 1, rec->fixed = 0;
            else
                rec->buf[GFX_NSTREAMS] = d->ibuf, d->ibuf->sun_seen = g_serial;
        }
    }
    else if (d->indices)
    {
        uint32_t off = 0;
        DXGI_FORMAT f = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        int ic = chunks_put(&g_iring, d->indices, n * d->index_size, 4, &off);
        if (ic < 0)
            return;
        set_index_buffer(g_iring.c[ic].buf, f, off);
        draw_call(d, n, 1);
        if (rec)
            rec->iring = 1, rec->ichunk = (uint16_t)ic, rec->igen = g_iring.c[ic].gen, rec->ioff = off, rec->ifmt = f;
    }
    else
        draw_call(d, n, 0);
    if (rec)
    {
        rec->dead = 0;
        rec->has_clip0 = rec->fixed && (uint8_t)gfx_draw_clip0(d, rec->clip0);
        snap_note(d, rec);
    }
}

/* --- the utility passes' constants ------------------------------------------------------------------------------- */
static void util_constants(const void* data, uint32_t size)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_util_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, data, size);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_util_cb, 0);
    }
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 1, 1, &g_util_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 1, 1, &g_util_cb);
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt)
        return;
    if (!g_ds)
        flags &= 1;
    if (!flags || !bind_targets())
        return;
    GfxTex* ds = depth_attachment();
    if (!ds || !ds->has_stencil)
        flags &= ~4u;
    float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
    uint32_t w, h;
    color_size(&w, &h);
    if (!nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h)
    {
        if (flags & 1)
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, (ID3D11RenderTargetView*)target_view(g_rt, g_rt_face, g_rt_level), c);
        UINT f = ((flags & 2) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 4) ? D3D11_CLEAR_STENCIL : 0);
        if (ds && f)
            ID3D11DeviceContext_ClearDepthStencilView(g_ctx, (ID3D11DepthStencilView*)target_view(ds, 0, 0), f, z, (UINT8)stencil);
        return;
    }
    /* rectangles (within the viewport): a triangle over the target through a scissor each, writing what
     * the flags say - the color, the depth (at z), the stencil (its value) */
    int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
    int32_t whole[4] = { vx0, vy0, vx1, vy1 };
    if (!nrects)
        rects = whole, nrects = 1;
    struct
    {
        float color[4];
        float depth[4];
    } k = { { c[0], c[1], c[2], c[3] }, { z, 0, 0, 0 } };
    util_constants(&k, sizeof k);
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (flags & 2) ? TRUE : FALSE;
    dd.DepthWriteMask = (flags & 2) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (flags & 4)
    {
        D3D11_DEPTH_STENCILOP_DESC st = { D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
        dd.StencilEnable = TRUE, dd.StencilReadMask = 0xFF, dd.StencilWriteMask = 0xFF, dd.FrontFace = st, dd.BackFace = st;
    }
    ID3D11DepthStencilState* depth = NULL;
    ID3D11Device_CreateDepthStencilState(g_dev, &dd, &depth);
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, (flags & 1) ? g_blend_none : g_blend_nocolor, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth, stencil & 0xFF);
    ID3D11DeviceContext_RSSetState(g_ctx, g_raster_scissor);
    D3D11_VIEWPORT v = { 0, 0, (float)w, (float)h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_clear_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_clear_ps, NULL, 0);
    for (uint32_t i = 0; i < nrects; ++i)
    {
        int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
        int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
        x1 = x1 < (int32_t)w ? x1 : (int32_t)w, y1 = y1 < (int32_t)h ? y1 : (int32_t)h;
        x0 = x0 > 0 ? x0 : 0, y0 = y0 > 0 ? y0 : 0;
        if (x1 <= x0 || y1 <= y0)
            continue;
        D3D11_RECT sc = { x0, y0, x1, y1 };
        ID3D11DeviceContext_RSSetScissorRects(g_ctx, 1, &sc);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    }
    if (depth)
        ID3D11DepthStencilState_Release(depth);
    bound_forget();
}

/* --- the window ------------------------------------------------------------------------------------------------ */
static void swap_views(void)
{
    if (g_swap_rtv)
        ID3D11RenderTargetView_Release(g_swap_rtv), g_swap_rtv = NULL;
    ID3D11Texture2D* buf = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void**)&buf)))
    {
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)buf, NULL, &g_swap_rtv);
        ID3D11Texture2D_Release(buf);
    }
}

/* the swap chain at the window's size in pixels */
static void swap_fit(void)
{
    if (g_removed) /* nothing will show again: not tried frame after frame */
        return;
    int pw = 0, ph = 0;
    RECT rc;
    if (g_window && GetClientRect(g_window, &rc))
        pw = rc.right, ph = rc.bottom;
    if (pw <= 0 || ph <= 0)
        pw = (int)g_want_w, ph = (int)g_want_h;
    if (pw <= 0 || ph <= 0 || ((uint32_t)pw == g_swap_w && (uint32_t)ph == g_swap_h))
        return;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    if (g_swap_rtv)
        ID3D11RenderTargetView_Release(g_swap_rtv), g_swap_rtv = NULL;
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(g_swap, 0, (UINT)pw, (UINT)ph, DXGI_FORMAT_UNKNOWN,
        g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr))
    {
        if (!removed())
            fprintf(stderr, "[recomp] gfx: swap chain resize to %dx%d failed (%08lx)\n", pw, ph, (unsigned long)hr);
    }
    else
        g_swap_w = (uint32_t)pw, g_swap_h = (uint32_t)ph;
    swap_views();
    g_targets_bound = 0;
}

static void make_swap_chain(HWND hwnd)
{
    if (!hwnd || !g_factory)
    {
        fprintf(stderr, "[recomp] gfx: no window for the swap chain\n");
        return;
    }
    IDXGIFactory5* f5 = NULL;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(g_factory, &IID_IDXGIFactory5, (void**)&f5)))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            g_tearing = allow != FALSE;
        IDXGIFactory5_Release(f5);
    }
    int pw = 0, ph = 0;
    RECT rc;
    if (GetClientRect(hwnd, &rc))
        pw = rc.right, ph = rc.bottom;
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = pw > 0 ? (UINT)pw : 640, sd.Height = ph > 0 ? (UINT)ph : 480;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(g_factory, (IUnknown*)g_dev, hwnd, &sd, NULL, NULL, &g_swap);
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: swap chain failed (%08lx)\n", (unsigned long)hr);
        g_swap = NULL;
        return;
    }
    IDXGIFactory2_MakeWindowAssociation(g_factory, hwnd, DXGI_MWA_NO_ALT_ENTER); /* full screen is the window's */
    g_swap_w = sd.Width, g_swap_h = sd.Height;
    swap_views();
}

/* --- frames ---------------------------------------------------------------------------------------------------- */
/* "60 FPS 16.7ms": the overlay's text, from the presents of the last half second */
static void fps_tick(void)
{
    double now = (double)gfx_now_ns() * 1e-9;
    if (!g_fps_since)
        g_fps_since = now;
    g_fps_frames++;
    double dt = now - g_fps_since;
    if (dt >= 0.5)
    {
        double fps = g_fps_frames / dt;
        snprintf(g_fps_text, sizeof g_fps_text, "%.0f FPS %.1fms", fps, dt * 1000.0 / g_fps_frames);
        g_fps_since = now, g_fps_frames = 0;
    }
}

static void draw_overlay(uint32_t w, uint32_t h)
{
    struct
    {
        float rect[4], scale;
        uint32_t n;
        float size[2];
        uint32_t text[32];
    } u;
    memset(&u, 0, sizeof u);
    for (const char* c = g_fps_text; *c && u.n < 32; ++c)
    {
        uint32_t k = *c >= '0' && *c <= '9' ? (uint32_t)(*c - '0') : *c == 'F' ? 10 : *c == 'P' ? 11 : *c == 'S' ? 12
            : *c == 'm' ? 13 : *c == 's' ? 14 : *c == '.' ? 15 : 16;
        u.text[u.n++] = k;
    }
    u.scale = (float)(h >= 1400 ? 3 : 2);
    u.rect[0] = u.rect[1] = 4 * u.scale;
    u.rect[2] = (float)(u.n * 6 + 3) * u.scale, u.rect[3] = 11 * u.scale;
    u.size[0] = (float)w, u.size[1] = (float)h;
    util_constants(&u, sizeof u);
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend_alpha, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_overlay_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_overlay_ps, NULL, 0);
    ID3D11DeviceContext_Draw(g_ctx, 4, 0);
}

void gfx_present_layer(GfxTex* layer) { g_present_layer_tex = layer; }
#include "gfx_d3d11_backdrop.h" /* gfx_present_backdrops (not drawn on Direct3D 11 yet) */

static void fx_rebuild_take(void);

void gfx_present(GfxTex* bb)
{
    if (!g_dev)
        return;
    int presented = 0;
    if (g_swap && bb && bb->srv && g_present_ps)
    {
        swap_fit();
        if (g_swap_rtv)
        {
            ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_swap_rtv, NULL);
            D3D11_VIEWPORT v = { 0, 0, (float)g_swap_w, (float)g_swap_h, 0, 1 };
            ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
            ID3D11DeviceContext_RSSetState(g_ctx, g_raster_plain);
            ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_depth_off, 0);
            static const float factor[4] = { 1, 1, 1, 1 };
            ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend_none, factor, 0xFFFFFFFFu);
            ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(g_ctx, g_present_vs, NULL, 0);
            /* contrast-adaptive sharpening with the scene effects (sharpen, 0..1) */
            float sharpen[4] = { g_fxs.fx != 0.0f && g_fxs.grade > 0.0f ? g_fxs.sharpen : 0.0f, 0, 0, 0 };
            if (sharpen[0] > 0.0f && g_cas_ps)
            {
                util_constants(sharpen, sizeof sharpen);
                ID3D11DeviceContext_PSSetShader(g_ctx, g_cas_ps, NULL, 0);
            }
            else
                ID3D11DeviceContext_PSSetShader(g_ctx, g_present_ps, NULL, 0);
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 1, &bb->srv);
            ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_linear);
            ID3D11DeviceContext_Draw(g_ctx, 3, 0);
            /* VanaCore's interface, drawn at the window's size, over the frame at whatever size it was drawn */
            GfxTex* layer = g_present_layer_tex;
            if (layer && layer->srv && g_layer_ps)
            {
                ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend_premul, factor, 0xFFFFFFFFu);
                ID3D11DeviceContext_PSSetShader(g_ctx, g_layer_ps, NULL, 0);
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 1, &layer->srv);
                ID3D11DeviceContext_Draw(g_ctx, 3, 0);
            }
            if (g_overlay && g_overlay_ps)
                draw_overlay(g_swap_w, g_swap_h);
            /* nothing the frame drew from stays bound: next frame it may be a target again */
            ID3D11ShaderResourceView* none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 1, &none);
            presented = 1;
        }
    }
    g_present_layer_tex = NULL;
    g_targets_bound = 0;
    bound_forget();
    fps_tick();
    fx_reload();
    casters_clear();
    gfx_watch_skips();
    if (presented)
    {
        HRESULT hr = IDXGISwapChain1_Present(g_swap, g_vsync ? 1 : 0, !g_vsync && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            removed();
    }
    g_serial++;
    fx_rebuild_take(); /* between frames: nothing of a frame's is half made */
}

uint64_t gfx_frame(void) { return g_serial; }

void gfx_finish(void)
{
    if (!g_dev || !g_idle)
        return;
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous*)g_idle);
    BOOL done = FALSE;
    while (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous*)g_idle, &done, sizeof done, 0) == S_FALSE)
        SwitchToThread();
}

void gfx_resize(uint32_t w, uint32_t h) { g_want_w = w, g_want_h = h; }

/* --- scene effects (gfx_scene_done) ------------------------------------------------------------------------------
 * gfx_d3d12.c's scene effects on Direct3D 11: the same passes from the same HLSL (gfx_fx_hlsl), fitted to
 * Direct3D 11's registers (fx_source) - a pass's inputs at t0..t9, the plain sampler at s0 and the shadow
 * lookups' comparison sampler at s1, its blur direction at b1, the uniforms (FxU) at b0. Every pass is one
 * triangle over its target: one of the effects' own (FxTex), or the scene's. A pixel function draws onto
 * any format here, so there's one of each, not one per scene format. The functions build on the thread
 * pool the first time a scene asks (fx_ready); the scene filter's chain is made by GenerateMips
 * (scene_mips). Settings: FFXI_FX=1 and FFXI_FX_<KEY> (fx_config), FFXI_FX_FILE while the game runs, or
 * gfx_fx_set. */
typedef struct FxU
{
    float proj[4], zp[4], vp[4], size[4], ao[4], grade[4], hand[4], up[4], sun[4], suncol[4], sunuv[4], fogc[4], fogp[4],
        bloom[4], rays[4], toon[4], toon2[4], toon3[4], dof[4], dof2[4], shadow[4], lmat[16], smap[4], smap2[4], reproj[16],
        hist[4], lmatn[16], smapn[4], smapn2[4], sunsh[4], aoq[4], glow[4], glow2[4], zlc[4], zlcol[4], zlp[4], zl[64][4], grade2[4],
        refl[4], refl2[4], refl3[4], post[4], post2[4], grade3[4], post3[4], clar[4],
        bounce[4], elight[4], elight2[4], elight3[4], aor[4], /* (Direct3D 12's passes: the shared HLSL's uniforms) */
        aoc[4], aoc2[4], /* ... the occlusion on characters (Direct3D 12's: here 0, as the sparkles' after them were read from these),
                          * but aoc.z (sun_soft_zone) and aoc2.z (sun_map's gate: sun_game_shading) */
        spc[4], spx[4], spz[4], spp[4], sp[3 * GFX_SPARKLES][4],
        post4[4], /* the moonlight (moon_grade, laid in the composite here): GfxScene.night, night_moon */
        plight[4], plight2[4], /* the player's light (gfx_player_light) */
        gmc[4], gmx[4], gmz[4], gm[4 * GFX_GROUND_MARKS][4], /* the rings of light on the ground (ground_marks) */
        rain[4], /* (Direct3D 12's rain wet ground, gfx_d3d12.c: 0 here) */
        lmatm[16], smapm[4], smapm2[4], // cppcheck-suppress unusedStructMember ; Direct3D 12's middle map (sun_mid): 0 here; smapm2[2] sun_handoff
        toon4[4], /* the lines on VanaCore's grass (gfx_grass_lines) */
        rtx[4], rbf[4]; // cppcheck-suppress unusedStructMember ; Direct3D 12's ray-traced reflections' blur and relief fade: 0 here
} FxU;

/* the sun's shadow map: 32-bit depth, read through its view */
typedef struct SunMap
{
    ID3D11Texture2D* res;
    ID3D11ShaderResourceView* srv;
    ID3D11DepthStencilView* dsv;
    int failed;
    uint32_t size; /* texels across */
} SunMap;

/* the effects' own targets: one level, a view for reading and one for drawing */
typedef struct FxTex
{
    ID3D11Texture2D* res;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView* rtv;
    DXGI_FORMAT fmt;
    uint32_t w, h;
} FxTex;

enum
{
    FX_AO,
    FX_BLUR,
    FX_BRIGHT,
    FX_DOWN,
    FX_GAUSS,
    FX_RAYMASK,
    FX_RAYS,
    FX_COMP,
    FX_MIP, /* Direct3D 12's chain; GenerateMips here */
    FX_SMOOTH,
    FX_FOCUS,
    FX_DOFPREP,
    FX_DOFBLUR,
    FX_DOFCOMP,
    FX_FXAA,
    FX_TEMPORAL,
    FX_LINZ,
    FX_ZMIP,
    FX_GLOWB,
    FX_GLOWC,
    FX_LOWPASS,
    FX_CLARITY,
    FX_GLOWG,
    FX_NPS,
};

static const char* const FX_ENTRY[FX_NPS] = { "fx_ao", "fx_blur", "fx_bright", "fx_down", "fx_gauss", "fx_raymask", "fx_rays",
    "fx_comp", "fx_mip", "fx_smooth", "fx_focus", "fx_dofprep", "fx_dofblur", "fx_dofcomp", "fx_fxaa", "fx_temporal", "fx_linz",
    "fx_zmip", "fx_glowb", "fx_glowc", "fx_lowpass", "fx_clarity", "fx_glowg" };

struct SunTrace
{
    uint64_t serial;
    uint32_t live, cached, skipped, gone, own, depth, ran, hist, held;
    float fog[3], dl, al, strength, day, sun[3], cam[3], across, rays;
    float far_r, near_r, reach, to; /* the maps' sphere radii, the sharp map's reach, the player's distance from the camera (sun_map) */
};

static struct
{
    volatile LONG state; /* 0 not built yet, 1 building, 2 ready, 3 failed */
    volatile LONG step, steps; /* fx_build's progress: steps made of steps (0: it hasn't started), for gfx_wait_progress */
    volatile PVOID at;         /* ... and the step it's on, in words (a literal's; NULL between builds), for gfx_wait_step */
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps[FX_NPS];
    ID3D11Buffer* cb;         /* FxU, at b0 */
    ID3D11Buffer* sun_cb;     /* the sun's map's matrix, at b2: the casters' vertex functions' (gfx_hlsl_vs_return) */
    ID3D11SamplerState* cmp;  /* the shadow lookups': 1 where the point is no deeper than the map, filtered across texels */
    FxTex src, ao0, ao1, b1a, b1b, b2a, b2b, ra, rb, sm;
    FxTex lz[4]; /* the occlusion's depth and its smaller levels (fx_linz) */
    FxTex glow[2]; /* VanaCore's glow's blur, across then down (gfx_glow) */
    FxTex glow_kept; /* ... the frame's glows as laid, premultiplied, at its size (gfx_glow_over_grass), and its frame */
    uint64_t glow_kept_serial;
    FxTex cl[2];   /* clarity's blur (about a quarter size) */
    FxTex d0, d1, foc[2]; /* depth of field: half size, and the focus distance (this frame's, the last) */
    int foc_i;             /* foc[foc_i] holds the last frame's focus */
    uint64_t foc_ns;       /* when it was measured */
    /* what the fog and rays follow, eased from frame to frame (fx_ease): the game's values can change
     * between frames, and the effects should not pop with them */
    int eased; /* ease (the last scene was the frame before); else take the new values */
    uint64_t eased_serial;
    float fog_on, fogc[3], up[3], sun[3], suncol[3];
    int fogc_set;         /* fogc holds a colour to ease from (else the next is taken at once) */
    FxTex hist[2];        /* the occlusion and shadows after the temporal pass: this frame's and the one before */
    int hist_at;          /* which of hist[] the frame before wrote */
    uint64_t hist_serial; /* the frame it was written (0: none) */
    float prev_view[16], prev_proj[16], prev_cam[3];
    SunMap smap, smapn;   /* the sun's shadow maps: far, near */
    /* toward the sun in the world, as the last lit draw gave it, and the frame it was seen */
    float sunw[3];
    uint64_t sunw_seen;
    /* sunw gliding to the game's latest step (gfx_sun_glide) */
    GfxSunGlide glide;
    /* a turn of more than a degree from one lit draw, not yet taken: the direction, how many frames in a
     * row have given it, and the last of them */
    float sun_cand[3];
    uint32_t sun_cand_n;
    uint64_t sun_cand_serial;
    /* how much of the game's light is the sun's (its diffuse against the ambient), eased: weather and
     * clouds dim it, and the shadows fade with it */
    float direct;
    uint32_t st_drawn_this, st_cached;
    float st_across;
    /* the maps' cascades, for the flicker watch and the trace (sun_map): the radius of the sphere each map is cut through (0: no
     * sharp map), the sharp map's reach, the player's distance from the camera (-1: none) */
    float st_far_r, st_near_r, st_reach, st_to;
    /* the trace (gfx_trace_dump): each scene done */
    uint32_t tr_live, tr_cached, tr_skipped, tr_gone, tr_depth;
    uint32_t tr_building; /* ... of the skipped, the ones whose pair is still compiling (not failed) */
    /* the last frame's casters, and those left out while their pairs compile, and its frame (gfx_waiting) */
    uint32_t wait_casters, wait_building;
    uint64_t wait_serial;
    float tr_strength, tr_day, tr_dl, tr_al, tr_fog[3];
    /* ... and the flicker watch's (fx_watch_frame): the effects ran; the frame before was blended in (1), or
     * why not (0 no shadows to blend, 2 none that frame, 3 another size, 4 the camera moved, 5 temporal off);
     * the god rays' strength; a lit draw's other light held back from the sun */
    uint32_t tr_ran, tr_hist, tr_held;
    float tr_rays;
    struct SunTrace trace[1200];
    uint32_t ntrace;
    /* the flicker watch's frame before, to compare with */
    struct
    {
        uint64_t serial;
        uint32_t ran, hist, fog, depth_kind;
        const void* depth;
        float sun[3], cam[3], smap, rays;
        float far_r, near_r; /* the maps' sphere radii (sun_map) */
    } w;
    /* the scene's depth before the grass (gfx_scene_world_depth): its copy, the depth it's of, and the frame */
    GfxTex* wdepth;
    GfxTex* wdepth_of;
    GfxTex* wdepth_glow; /* ... the depth it's of, copied for the glow laid again over the grass alone (wdepth_of NULL then) */
    uint64_t wdepth_serial;
} g_fx;

/* a toward b by k; the first time, b */
static void fx_ease(float* a, const float* b, int n, float k)
{
    for (int i = 0; i < n; ++i)
        a[i] = g_fx.eased ? a[i] + (b[i] - a[i]) * k : b[i];
}

static void normalize3(float* v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
        v[0] /= l, v[1] /= l, v[2] /= l;
}

/* every setting: its key in the file, FFXI_FX_<KEY> in the environment, and its default (gfx_d3d12.c's) */
static const struct
{
    const char* key;
    size_t at;
    float def;
} FX_SETTINGS[] = {
    { "fx", offsetof(FxSettings, fx), 0.0f },
    { "ao", offsetof(FxSettings, ao), 1.0f },
    { "radius", offsetof(FxSettings, radius), 2.0f },
    { "grade", offsetof(FxSettings, grade), 1.0f },
    { "sat", offsetof(FxSettings, sat), 1.12f },
    { "contrast", offsetof(FxSettings, contrast), 0.2f },
    { "sharpen", offsetof(FxSettings, sharpen), 0.3f },
    { "filter", offsetof(FxSettings, filter), 1.0f },
    { "aniso", offsetof(FxSettings, aniso), 16.0f },
    { "tex_sharp", offsetof(FxSettings, tex_sharp), 0.0f }, /* Texture Sharpness: the world's mips 0 to 2 levels finer (gfx_world_filter) */
    { "fog", offsetof(FxSettings, fog), 0.01f },
    { "fog_falloff", offsetof(FxSettings, fog_falloff), 0.08f },
    { "fog_height", offsetof(FxSettings, fog_height), 2.0f },
    { "fog_floor", offsetof(FxSettings, fog_floor), 4.0f },
    { "fog_max", offsetof(FxSettings, fog_max), 0.6f },
    { "fog_sunlight", offsetof(FxSettings, fog_sun), 2.5f }, /* (fog_sun till 2026-11-27, 0 to 2: now 0 to 10, as /vana shows it) */
    { "fog_glow", offsetof(FxSettings, fog_g), 0.95f }, /* (fog_g till 2026-11-27; low, medium, high are graphics.cpp effect_word's) */
    { "bloom", offsetof(FxSettings, bloom), 1.5f },
    { "threshold", offsetof(FxSettings, threshold), 0.85f },
    { "rays", offsetof(FxSettings, rays), 0.6f },
    { "rays_decay", offsetof(FxSettings, rays_decay), 0.965f },
    { "rays_length", offsetof(FxSettings, rays_length), 0.85f },
    { "light", offsetof(FxSettings, light), 1.0f },
    { "sun", offsetof(FxSettings, sun), 0.6f },
    { "sun_distance", offsetof(FxSettings, sun_distance), 150.0f },
    { "sun_soft", offsetof(FxSettings, sun_soft), 0.0f },
    { "sun_smooth", offsetof(FxSettings, sun_smooth), 1.0f },
    { "sun_soft_zone", offsetof(FxSettings, sun_soft_zone), 0.5f },
    { "sun_curved", offsetof(FxSettings, sun_curved), 1.0f },
    { "normalmap", offsetof(FxSettings, normalmap), 0.0f },
    { "normalmap_strength", offsetof(FxSettings, normalmap_strength), 1.0f },
    { "normalmap_view", offsetof(FxSettings, normalmap_view), 0.0f },
    { "sun_face", offsetof(FxSettings, sun_face), 1.0f },
    { "sun_min", offsetof(FxSettings, sun_min), 0.0f },
    { "sun_direct", offsetof(FxSettings, sun_direct), 0.5f },
    { "sun_dusk", offsetof(FxSettings, sun_dusk), 1.0f },
    { "sun_free", offsetof(FxSettings, sun_free), 0.0f },
    { "sun_free_az", offsetof(FxSettings, sun_free_az), 170.0f },
    { "sun_tilt", offsetof(FxSettings, sun_tilt), 15.0f }, /* the game's sun and moon tilted south (gfx_shared.c sun_tilt) */
    { "sun_moon", offsetof(FxSettings, sun_moon), 1.0f }, /* shadows from the moon at night (gfx_shared.c gfx_sun_light) */
    { "sun_game_shading", offsetof(FxSettings, sun_game_shading), 0.0f },
    { "sun_free_el", offsetof(FxSettings, sun_free_el), 41.0f },
    { "sun_casters", offsetof(FxSettings, sun_casters), 0.0f }, /* the zone casts too */
    { "sun_near", offsetof(FxSettings, sun_near), 10.0f }, /* 25, then 50 until 2026-11-23 (gfx_d3d12.c) */
    { "moghouse", offsetof(FxSettings, moghouse), 0.0f },
    { "sun_handoff", offsetof(FxSettings, sun_handoff), 0.5f },
    { "temporal", offsetof(FxSettings, temporal), 0.99f },
    { "ao_quality", offsetof(FxSettings, ao_quality), 3.0f },
    { "sun_rings", offsetof(FxSettings, sun_rings), 1.0f },
    { "sun_ring_spacing", offsetof(FxSettings, sun_ring_spacing), 15.0f },
    { "sun_detail", offsetof(FxSettings, sun_detail), 4096.0f },
    { "sun_fade", offsetof(FxSettings, sun_fade), 15.0f },
    { "debug",offsetof(FxSettings, debug), 0.0f },
    { "outline", offsetof(FxSettings, outline), 0.0f },
    { "outline_width", offsetof(FxSettings, outline_width), 1.0f },
    { "outline_depth", offsetof(FxSettings, outline_depth), 0.1f },
    { "outline_crease", offsetof(FxSettings, outline_crease), 0.25f },
    { "outline_fade", offsetof(FxSettings, outline_fade), 60.0f },
    { "outline_nograss", offsetof(FxSettings, outline_nograss), 1.0f },
    { "outline_width_chars", offsetof(FxSettings, outline_width_chars), 0.0f },
    { "outline_grass", offsetof(FxSettings, outline_grass), 0.0f },
    { "outline_width_grass", offsetof(FxSettings, outline_width_grass), 0.0f },
    { "outline_depth_grass", offsetof(FxSettings, outline_depth_grass), 0.0f },
    { "outline_fade_grass", offsetof(FxSettings, outline_fade_grass), 0.0f },
    { "bands", offsetof(FxSettings, bands), 0.0f },
    { "band_soft", offsetof(FxSettings, band_soft), 0.15f },
    { "smooth", offsetof(FxSettings, smooth), 0.0f },
    { "dof", offsetof(FxSettings, dof), 0.0f },
    { "dof_focus", offsetof(FxSettings, dof_focus), 0.0f },
    { "dof_player", offsetof(FxSettings, dof_player), 0.0f },
    { "dof_range", offsetof(FxSettings, dof_range), 0.5f },
    { "dof_far", offsetof(FxSettings, dof_far), 3.0f },
    { "dof_near", offsetof(FxSettings, dof_near), 0.0f },
    { "dof_sky", offsetof(FxSettings, dof_sky), 0.0f },
    { "aa", offsetof(FxSettings, aa), 0.0f },
    { "brightness", offsetof(FxSettings, brightness), 0.0f },
    { "shadows", offsetof(FxSettings, shadows), 0.0f },
    { "highlights", offsetof(FxSettings, highlights), 0.0f },
    { "warmth", offsetof(FxSettings, warmth), 0.0f },
    { "hue", offsetof(FxSettings, hue), 0.0f },
    { "clarity", offsetof(FxSettings, clarity), 0.0f },
    { "night_moon", offsetof(FxSettings, night_moon), 0.0f },
    { "day_night_warmth", offsetof(FxSettings, day_night_warmth), 0.0f },
    { "player_light", offsetof(FxSettings, player_light), 0.0f },
    { "player_light_radius", offsetof(FxSettings, player_light_radius), 8.0f },
    { "player_light_warm", offsetof(FxSettings, player_light_warm), 0.5f },
    { "player_light_day", offsetof(FxSettings, player_light_day), 0.5f },
    { "glow_strength", offsetof(FxSettings, glow_strength), 1.0f },
    { "glow_size", offsetof(FxSettings, glow_size), 1.0f },
    { "glow_quality", offsetof(FxSettings, glow_quality), 1.0f },
    { "fog_mode", offsetof(FxSettings, fog_mode), 0.0f },
    { "fog_cr", offsetof(FxSettings, fog_cr), 0.72f },
    { "fog_cg", offsetof(FxSettings, fog_cg), 0.78f },
    { "fog_cb", offsetof(FxSettings, fog_cb), 0.86f },
};

/* a quality setting - low, medium, high, ultra, max - as 0 to 4 */
static int fx_level(float q)
{
    return q < 0.5f ? 0 : q > 3.5f ? 4 : (int)(q + 0.5f);
}

/* the occlusion's taps a pixel a frame, by ao_quality */
static const float AO_TAPS[5] = { 6, 10, 16, 24, 32 };

static float* fx_setting(const char* key)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
        if (!strcmp(FX_SETTINGS[i].key, key))
            return (float*)((char*)&g_fxs + FX_SETTINGS[i].at);
    return NULL;
}

/* v NaN: back to the setting's default (a key taken out of the settings) */
void gfx_fx_set(const char* key, float v)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
        if (!strcmp(FX_SETTINGS[i].key, key))
            *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = v == v ? v : FX_SETTINGS[i].def;
}

static char g_fx_file[1024];
static __time64_t g_fx_mtime;

/* the settings file, when it changed since the last look (from Present, twice a second) */
static void fx_reload(void)
{
    static uint64_t last;
    uint64_t now = gfx_now_ns();
    if (!g_fx_file[0] || now - last < 500000000ull)
        return;
    last = now;
    struct __stat64 st;
    if (_stat64(g_fx_file, &st) || st.st_mtime == g_fx_mtime)
        return;
    g_fx_mtime = st.st_mtime;
    FILE* f = fopen(g_fx_file, "r");
    if (!f)
        return;
    char line[256], key[64];
    float v, *p;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %63[a-z_] = %f", key, &v) == 2 && (p = fx_setting(key)))
            *p = v;
    fclose(f);
    fprintf(stderr, "[recomp] gfx: scene effects %s from %s\n", g_fxs.fx != 0.0f ? "on" : "off", g_fx_file);
}

static void fx_config(void)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
    {
        char name[64], *c;
        snprintf(name, sizeof name, i ? "FFXI_FX_%s" : "FFXI_FX", FX_SETTINGS[i].key);
        for (c = name; *c; ++c)
            if (*c >= 'a' && *c <= 'z')
                *c -= 32;
        const char* v = getenv(name);
        *(float*)((char*)&g_fxs + FX_SETTINGS[i].at) = v && *v ? (float)atof(v) : FX_SETTINGS[i].def;
    }
    const char* dbg = getenv("FFXI_FX_DEBUG"); /* also by name */
    if (dbg)
        g_fxs.debug = !strcmp(dbg, "ao") ? 1.0f : !strcmp(dbg, "fog") ? 2.0f : !strcmp(dbg, "bloom") ? 3.0f
            : !strcmp(dbg, "rays") ? 4.0f : !strcmp(dbg, "shadow") ? 5.0f : (float)atof(dbg);
    const char* file = getenv("FFXI_FX_FILE");
    if (file && *file)
        snprintf(g_fx_file, sizeof g_fx_file, "%s", file);
}

/* gfx_fx_hlsl in Direct3D 11's registers: its heap tables, and the helpers that index them (the k-th
 * input's tap, texel and size, the blur direction, the shadow maps' comparison), put in their place.
 * NULL when it isn't in the form this was fitted to (a change there wants one here). The lint's d3d11fx check
 * (tools/lint/d3d11_fx.py) does these steps over the sources and compiles the result: a change to them wants one there. */
static char* fx_source(void)
{
    static const char BIND_AT[] = "cbuffer FB : register(b1)";
    static const char BIND_END[] = "SamplerComparisonState smpc[] : register(s0, space1);\n";
    static const char HELP_AT[] = "uint slot(uint k)";
    static const char HELP_END[] = "int2 fdir() { return int2(asint(fsi[0].y), asint(fsi[0].z)); }\n";
    /* the detail target (gfx_d3d12.c nout_begin), by its heap slot: here at t20, past the draws' textures (t4..t19), with the
     * scene's smooth normals alone (gfx_d3d11_smooth.h vn_replay: smooth_at; normal maps are Direct3D 12's, and normal_det's
     * flags, u.clar.zw, are 0) */
    static const char NDET[] = "tx2[(uint)u.clar.y].Load(int3(int2(px), 0))";
    static const char NDET11[] = "fdet.Load(int3(int2(px), 0))";
    static const char CMP[] = "tx2[slot(km)].SampleCmpLevelZero(smpc[fsi[0].w], ";
    static const char BIND[] =
        "cbuffer FB : register(b1) { int4 fdir_; };\n"
        "Texture2D<float4> fin0 : register(t0); Texture2D<float4> fin1 : register(t1);\n"
        "Texture2D<float4> fin2 : register(t2); Texture2D<float4> fin3 : register(t3);\n"
        "Texture2D<float4> fin4 : register(t4); Texture2D<float4> fin5 : register(t5);\n"
        "Texture2D<float4> fin6 : register(t6); Texture2D<float4> fin7 : register(t7);\n"
        "Texture2D<float4> fin8 : register(t8); Texture2D<float4> fin9 : register(t9);\n"
        "Texture2D<float4> fdet : register(t20);\n"
        /* ... and at t21 the zone's curved surfaces: the pixel's lift onto its smooth surface, and its curve (zn_draw; sun_map's
         * g_lift) */
        "Texture2D<float4> flift : register(t21);\n"
        "#define LIFT_AT(px) flift.Load(int3(int2(px), 0)).rgb\n"
        /* (the rain's wet ground's mask is Direct3D 12's: none, u.rain.w 0) */
        "#define RAIN_AT(px) float4(0.0, 0.0, 0.0, 0.0)\n"
        "SamplerState fsmp : register(s0);\n"
        "SamplerComparisonState fcmp : register(s1);\n";
    /* k is a literal wherever these are used: each folds to the one texture */
    static const char HELP[] =
        "float4 tap(uint k, float2 uv) {\n"
        "  if (k == 0) return fin0.SampleLevel(fsmp, uv, 0); if (k == 1) return fin1.SampleLevel(fsmp, uv, 0);\n"
        "  if (k == 2) return fin2.SampleLevel(fsmp, uv, 0); if (k == 3) return fin3.SampleLevel(fsmp, uv, 0);\n"
        "  if (k == 4) return fin4.SampleLevel(fsmp, uv, 0); if (k == 5) return fin5.SampleLevel(fsmp, uv, 0);\n"
        "  if (k == 6) return fin6.SampleLevel(fsmp, uv, 0); return fin7.SampleLevel(fsmp, uv, 0);\n"
        "}\n"
        "float4 tapl(uint k, float2 uv, float lv) {\n"
        "  if (k == 0) return fin0.SampleLevel(fsmp, uv, lv); if (k == 1) return fin1.SampleLevel(fsmp, uv, lv);\n"
        "  if (k == 2) return fin2.SampleLevel(fsmp, uv, lv); if (k == 3) return fin3.SampleLevel(fsmp, uv, lv);\n"
        "  if (k == 4) return fin4.SampleLevel(fsmp, uv, lv); if (k == 5) return fin5.SampleLevel(fsmp, uv, lv);\n"
        "  if (k == 6) return fin6.SampleLevel(fsmp, uv, lv); return fin7.SampleLevel(fsmp, uv, lv);\n"
        "}\n"
        "float4 texel(uint k, int2 p) {\n"
        "  int3 q = int3(p, 0);\n"
        "  if (k == 0) return fin0.Load(q); if (k == 1) return fin1.Load(q); if (k == 2) return fin2.Load(q);\n"
        "  if (k == 3) return fin3.Load(q); if (k == 4) return fin4.Load(q); if (k == 5) return fin5.Load(q);\n"
        "  if (k == 6) return fin6.Load(q); if (k == 7) return fin7.Load(q); if (k == 8) return fin8.Load(q); return fin9.Load(q);\n"
        "}\n"
        "float2 size_of(uint k) {\n"
        "  uint w = 1, h = 1;\n"
        "  if (k == 0) fin0.GetDimensions(w, h); else if (k == 1) fin1.GetDimensions(w, h);\n"
        "  else if (k == 2) fin2.GetDimensions(w, h); else if (k == 3) fin3.GetDimensions(w, h);\n"
        "  else if (k == 4) fin4.GetDimensions(w, h); else if (k == 5) fin5.GetDimensions(w, h);\n"
        "  else if (k == 6) fin6.GetDimensions(w, h); else fin7.GetDimensions(w, h);\n"
        "  return float2(w, h);\n"
        "}\n"
        "float cmp_tap(uint k, float2 uv, float z) {\n"
        "  if (k == 1) return fin1.SampleCmpLevelZero(fcmp, uv, z);\n"
        "  if (k == 2) return fin2.SampleCmpLevelZero(fcmp, uv, z);\n"
        "  if (k == 7) return fin7.SampleCmpLevelZero(fcmp, uv, z);\n"
        "  return fin0.SampleCmpLevelZero(fcmp, uv, z);\n"
        "}\n"
        "int2 fdir() { return fdir_.xy; }\n";
    const char* src = gfx_fx_hlsl();
    const char* b0 = strstr(src, BIND_AT);
    const char* b1 = b0 ? strstr(b0, BIND_END) : NULL;
    const char* h0 = b1 ? strstr(b1, HELP_AT) : NULL;
    const char* h1 = h0 ? strstr(h0, HELP_END) : NULL;
    const char* n0 = h1 ? strstr(h1, NDET) : NULL;
    const char* c0 = n0 ? strstr(n0, CMP) : NULL;
    if (!c0 || strstr(n0 + 1, NDET))
        return NULL;
    b1 += sizeof BIND_END - 1, h1 += sizeof HELP_END - 1;
    const char* n1 = n0 + sizeof NDET - 1;
    const char* rest = c0 + sizeof CMP - 1;
    size_t n = strlen(src) + sizeof BIND + sizeof HELP + sizeof NDET11 + 64;
    char* out = (char*)malloc(n);
    if (!out)
        return NULL;
    snprintf(out, n, "%.*s%s%.*s%s%.*s%s%.*scmp_tap(km, %s", (int)(b0 - src), src, BIND, (int)(h0 - b1), b1, HELP, (int)(n0 - h1), h1,
        NDET11, (int)(c0 - n1), n1, rest);
    /* ... and every comparison after it (sun_gate's one hard tap, 4e133616, came before sun_taps' loop): refused, a second one
     * left every scene effect off here from 0.8.142 to 0.8.152, shadows, occlusion and all. Each reads map km; the call is shorter */
    for (char* c; (c = strstr(out, CMP)) != NULL;)
    {
        static const char TAP[] = "cmp_tap(km, ";
        memcpy(c, TAP, sizeof TAP - 1);
        memmove(c + sizeof TAP - 1, c + sizeof CMP - 1, strlen(c + sizeof CMP - 1) + 1);
    }
    /* nothing of Direct3D 12's tables left */
    if (strstr(out, "tx2[") || strstr(out, "smp[") || strstr(out, "smpc[") || strstr(out, "fti[") || strstr(out, "fsi[") ||
        strstr(out, "slot("))
    {
        free(out);
        return NULL;
    }
    return out;
}

static void up_build(const char* src);

/* the functions: on the thread pool the first time a scene asks (the game never waits for the
 * compiler), or in place for the tests */
static void fx_build(void)
{
    gfx_fx_build_begin("d3d11");
    char* src = fx_source();
    if (!src)
        fprintf(stderr, "[recomp] gfx: the scene effects' HLSL isn't in the form Direct3D 11's registers were fitted to\n");
    /* the vertex shader, each pixel shader but FX_MIP's, then up_build's seven */
    InterlockedExchange(&g_fx.step, 0);
    InterlockedExchange(&g_fx.steps, FX_NPS + 7);
    InterlockedExchangePointer(&g_fx.at, (PVOID)gfx_fx_words("fx_vs"));
    ID3DBlob* b = src ? compile(src, "fx_vs", "vs_5_0") : NULL;
    int ok = b && SUCCEEDED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_fx.vs));
    if (b)
        ID3D10Blob_Release(b);
    InterlockedIncrement(&g_fx.step);
    for (int i = 0; ok && i < FX_NPS; ++i)
    {
        if (i == FX_MIP)
            continue;
        InterlockedExchangePointer(&g_fx.at, (PVOID)gfx_fx_words(FX_ENTRY[i]));
        b = compile(src, FX_ENTRY[i], "ps_5_0");
        ok = b && SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL,
                      &g_fx.ps[i]));
        if (b)
            ID3D10Blob_Release(b);
        InterlockedIncrement(&g_fx.step);
    }
    if (ok)
        up_build(src); /* FSR's passes, from the same HLSL */
    InterlockedExchangePointer(&g_fx.at, NULL);
    free(src);
    gfx_fx_build_end(ok);
    InterlockedExchange(&g_fx.state, ok ? 2 : 3);
}

static void CALLBACK fx_build_job(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst;
    (void)ctx;
    fx_build();
}

static int fx_ready(void)
{
    LONG st = InterlockedCompareExchange(&g_fx.state, 0, 0);
    if (st == 0)
    {
        InterlockedExchange(&g_fx.state, 1);
        if (g_sync_pipelines || !TrySubmitThreadpoolCallback(fx_build_job, NULL, NULL))
            fx_build();
        st = InterlockedCompareExchange(&g_fx.state, 0, 0);
    }
    if (st == 1)
        gfx_fx_build_tick(); /* still building: a word once it's taking long */
    return st == 2;
}

static void fx_tex_free(FxTex* t)
{
    if (t->rtv)
        ID3D11RenderTargetView_Release(t->rtv);
    if (t->srv)
        ID3D11ShaderResourceView_Release(t->srv);
    if (t->res)
        ID3D11Texture2D_Release(t->res);
    memset(t, 0, sizeof *t);
}

/* an effect target of this size and format in *t, made again when either changes */
static int fx_tex(FxTex* t, DXGI_FORMAT fmt, uint32_t w, uint32_t h)
{
    if (t->res && t->w == w && t->h == h && t->fmt == fmt)
        return 1;
    fx_tex_free(t);
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, fmt, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0 };
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->res)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)t->res, NULL, &t->srv)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)t->res, NULL, &t->rtv)))
    {
        fx_tex_free(t);
        return 0;
    }
    t->fmt = fmt, t->w = w, t->h = h;
    return 1;
}

/* the uniforms (b0) this pass and the ones after it read */
static void fx_constants(const FxU* u)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_fx.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, u, sizeof *u);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_fx.cb, 0);
    }
}

/* nothing the passes read stays bound (the next draws may make it a target); the game's draws bind
 * their own again */
static void fx_unbind(void)
{
    ID3D11ShaderResourceView* none[8] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 8, none);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 20, 2, none); /* the smooth normals (vn_replay) and the curved surfaces' lift
                                                                    * (zn_draw): the next scene draws into them */
    g_targets_bound = 0;
    bound_forget();
}

/* one full-screen triangle into a target (drawn within vp), reading in[0..n) at t0..; the uniforms u,
 * or (NULL) the last pass's */
static int g_fx_blend; /* fx_draw blends over what's there by the function's alpha (gfx_glow); 2 its colour premultiplied */

static void fx_draw(ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, const float vp[4], const FxU* u,
    ID3D11ShaderResourceView* const* in, int n, int dx, int dy)
{
    if (!rtv || !ps)
        return;
    /* the inputs after the target: Direct3D 11 unbinds a view of what becomes a target, and won't bind a
     * view of what is one */
    ID3D11ShaderResourceView* srv[10] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 10, srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    for (int i = 0; i < n && i < 10; ++i)
        srv[i] = in[i];
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 10, srv);
    if (u)
        fx_constants(u);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_fx.cb);
    int32_t fb[4] = { dx, dy, 0, 0 };
    util_constants(fb, sizeof fb);
    D3D11_VIEWPORT v = { vp[0], vp[1], vp[2], vp[3], 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
    ID3D11DeviceContext_RSSetState(g_ctx, g_raster_plain);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_depth_off, 0);
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_fx_blend == 2 ? g_blend_premul : g_fx_blend ? g_blend_alpha : g_blend_none, factor,
        0xFFFFFFFFu);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_fx.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, ps, NULL, 0);
    ID3D11SamplerState* samp[2] = { g_linear, g_fx.cmp };
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 2, samp);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    g_targets_bound = 0; /* the game's next draw binds its own again */
    bound_forget();
}

/* into an effect target, all of it */
/* the occlusion's depth and its levels (fx_linz), for an occlusion of aw x ah */
static int fx_lz(uint32_t aw, uint32_t ah)
{
    for (int lv = 0; lv < 4; ++lv, aw = (aw + 1) / 2, ah = (ah + 1) / 2)
        if (!fx_tex(&g_fx.lz[lv], DXGI_FORMAT_R32_FLOAT, aw, ah))
            return 0;
    return 1;
}

static void fx_pass(FxTex* to, int kind, const FxU* u, ID3D11ShaderResourceView* const* in, int n, int dx, int dy)
{
    float vp[4] = { 0, 0, (float)to->w, (float)to->h };
    fx_draw(to->rtv, g_fx.ps[kind], vp, u, in, n, dx, dy);
}

/* the scene as it is now into g_fx.src (its first level; a copy the passes read while the last draws over it) */
static void fx_copy_scene(GfxTex* color)
{
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource*)g_fx.src.res, 0, 0, 0, 0, (ID3D11Resource*)color->res, 0, NULL);
}

/* the inverse of a 4x4 matrix (row-major); 0 when it has none */
static int mat_inverse(float* out, const float* m)
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f)
        return 0;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return 1;
}

static void mat_mul(float* o, const float* a, const float* b)
{
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, t, sizeof t);
}

enum { SUN_MAP = 4096, SUN_CACHE_FRAMES = 60 * 30 };

/* The zone's casters, kept after they leave the view: the game draws only what the camera sees, but
 * a low sun throws the shadows of what is behind and beside the camera into it. Each caster drawn
 * from buffers the game keeps is kept with a copy of its uniforms and that frame's camera (clip
 * space back to the world), and drawn into the map from there while it is out of view - for 30
 * seconds, or until the camera jumps (a new zone). A buffer or texture it draws from that goes or
 * changes (sun_forget) ends it. (gfx_d3d12.c's) */
typedef struct CacheKey
{
    GfxBuf* buf[GFX_NSTREAMS + 1];
    uint32_t base[GFX_NSTREAMS], ioff, n, vstart;
    LibKey lib;
} CacheKey;

typedef struct Cached
{
    Caster c;             /* as last drawn; its own uniforms are u */
    GfxU* u;              /* ... c.need bytes of them */
    float clip_world[16]; /* that frame's camera: clip space back to the world */
    float cam[3];         /* ... and where it was */
    float at[3];          /* where it stands (caster_at), when has_at */
    uint8_t has_at;
    uint32_t next;        /* the next entry of its key (a copy of the same mesh), index + 1 */
    uint64_t seen, ring_serial;
    uint64_t fade_ns;     /* when it was let go of (cache_let_go): fading since; 0 not */
    uint32_t ring_u, ring_gen; /* u in the uniforms' ring, once written this frame, and the ring's generation then */
} Cached;

static Cached* g_cache;
static uint32_t g_ncache, g_cache_cap;
static Map g_cache_map; /* CacheKey -> index + 1 */
static float g_cache_cam[3];

/* How long what's out of view is kept: SUN_CACHE_FRAMES after it was last seen - but for as long as the
 * camera stays within SUN_KEEP_NEAR units of where it was then, so what stands around the player keeps
 * casting however long they look away from it. */
#define SUN_KEEP_NEAR 40.0f

static int cache_expired(const Cached* ce)
{
    if (ce->seen + SUN_CACHE_FRAMES >= g_serial)
        return 0;
    const float dx = ce->cam[0] - g_cache_cam[0], dy = ce->cam[1] - g_cache_cam[1], dz = ce->cam[2] - g_cache_cam[2];
    return dx * dx + dy * dy + dz * dz > SUN_KEEP_NEAR * SUN_KEEP_NEAR;
}

/* [graphics.effects] sun_fade: what the cache lets go of - no longer kept (cache_expired), or a piece of the zone the game
 * stopped drawing in plain view - goes on casting for sun_fade seconds, fainter and fainter (GfxVsKey.shadow 2), rather than
 * its shadow going all at once; drawn by the game again, it's whole again (gfx_d3d12.c's) */
static uint64_t g_fade_now; /* this frame's time (sun_cache_update) */

static void cache_let_go(Cached* ce)
{
    if (!ce->fade_ns)
        ce->fade_ns = g_fade_now ? g_fade_now : 1;
}

/* how much of entry ce still casts: 1 whole, under 1 fading (eased in and out), 0 gone */
static float cache_share(const Cached* ce)
{
    if (!ce->fade_ns)
        return 1.0f;
    const double len = fmin(fmax((double)g_fxs.sun_fade, 0.0), 120.0) * 1e9;
    if (!(len > 0.0) || (double)(g_fade_now - ce->fade_ns) >= len)
        return 0.0f;
    const float t = (float)((double)(g_fade_now - ce->fade_ns) / len);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* let go of and faded: dropped (sun_cache_trim, snap_trim) */
static int cache_done(const Cached* ce)
{
    return ce->fade_ns && cache_share(ce) <= 0.0f;
}

/* --- the shadow cache's copies ------------------------------------------------------------------------------
 * An object placed by a world matrix (GfxDraw.anchor: a tree, a prop) that the cache above can't draw
 * again from the game's buffers - they're rewritten: a dynamic buffer, or a static one written again
 * (FFXI rewrites its trees as they sway) - is kept as a copy: its vertices and indices as last drawn, in
 * chunks of the back end's own, with its uniforms and that frame's camera, and drawn into the map from
 * there while it's out of view, as the cache's are. Characters (their vertices in the world already, no
 * anchor) aren't kept: they move. One whose place is in plain view near the camera but that the game
 * didn't draw is gone - it moved, or another LOD of it is drawn - and is dropped. */
#define SNAP_MAX 4096
#define SNAP_CHUNK (4u << 20)
#define SNAP_CHUNKS 12 /* 48 MB at most */

typedef struct SnapKey
{
    LibKey lib;
    GfxTex* tex[8];
    uint32_t n, vstart, count, prim, index_size, size[GFX_NSTREAMS];
    /* the vertices the copy starts at (the first index in it, as vofs takes it there) and their strides: a draw of
     * another range of the same buffer, the same size, is another copy - the copy's indices taken through another
     * range's vofs read outside it (past its chunk: the GPU faults) */
    int32_t vofs, stride[GFX_NSTREAMS];
    int32_t at[3]; /* where it stands, in quarter units */
} SnapKey;

typedef struct Snap
{
    Cached k;      /* drawn as the cache's are; its Caster draws from its chunk (base, ioff) */
    SnapKey key;
    float at[3];   /* where it stands */
    int chunk;     /* the chunk holding its copies; -1 none */
} Snap;

typedef struct SnapChunk
{
    GfxBuf* buf;
    uint32_t used, live; /* bytes handed out; the copies in it still kept */
} SnapChunk;

static Snap* g_snaps;
static uint32_t g_nsnaps, g_snaps_cap;
static Map g_snap_map; /* SnapKey -> index + 1 */
static SnapChunk g_chunks[SNAP_CHUNKS];
static int g_chunk_at = -1; /* the chunk new copies go into */

static void map_clear(Map* m)
{
    for (uint32_t i = 0; i < m->cap; ++i)
        if (m->e[i].hash)
            free(m->e[i].key);
    free(m->e);
    memset(m, 0, sizeof *m);
}

static void chunk_free(int i)
{
    gfx_buf_destroy(g_chunks[i].buf);
    memset(&g_chunks[i], 0, sizeof g_chunks[i]);
    if (g_chunk_at == i)
        g_chunk_at = -1;
}

/* a chunk with room for `bytes` more (a new one when the one filling up is full); -1 when there's none */
static int snap_chunk(uint32_t bytes)
{
    if (bytes > SNAP_CHUNK)
        return -1;
    if (g_chunk_at >= 0 && g_chunks[g_chunk_at].live == 0)
        g_chunks[g_chunk_at].used = 0; /* every copy in it went: from the start again */
    if (g_chunk_at >= 0 && g_chunks[g_chunk_at].used + bytes <= SNAP_CHUNK)
        return g_chunk_at;
    for (int i = 0; i < SNAP_CHUNKS; ++i)
        if (!g_chunks[i].buf)
        {
            g_chunks[i].buf = gfx_buf_create(SNAP_CHUNK);
            if (!g_chunks[i].buf)
                return -1;
            /* the full one is let go of once its last copy is (snap_release) */
            if (g_chunk_at >= 0 && g_chunks[g_chunk_at].live == 0)
                chunk_free(g_chunk_at);
            g_chunk_at = i;
            return i;
        }
    return -1;
}

static int snap_room(uint32_t bytes)
{
    if (g_chunk_at >= 0 && (g_chunks[g_chunk_at].live == 0 || g_chunks[g_chunk_at].used + bytes <= SNAP_CHUNK))
        return 1;
    for (int i = 0; i < SNAP_CHUNKS; ++i)
        if (!g_chunks[i].buf)
            return 1;
    return 0;
}

static void snap_release(Snap* sp)
{
    free(sp->k.u);
    sp->k.u = NULL;
    if (sp->chunk >= 0 && --g_chunks[sp->chunk].live == 0 && sp->chunk != g_chunk_at)
        chunk_free(sp->chunk);
    sp->chunk = -1;
}

static int snap_newest_first(const void* a, const void* b)
{
    const uint64_t x = ((const Snap*)a)->k.seen, y = ((const Snap*)b)->k.seen;
    return x > y ? -1 : x < y ? 1 : 0;
}

/* drops what went, and what's no longer kept (cache_expired; all of it when all is true); with
 * `room`, the older half too; the map made again */
static void snap_trim(int all, int room)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nsnaps; ++i)
    {
        Snap* sp = &g_snaps[i];
        if (cache_expired(&sp->k))
            cache_let_go(&sp->k);
        if (all || sp->k.c.dead || sp->chunk < 0 || cache_done(&sp->k))
            snap_release(sp);
        else
            g_snaps[n++] = *sp;
    }
    g_nsnaps = n;
    if (room && g_nsnaps > 1)
    {
        qsort(g_snaps, g_nsnaps, sizeof(Snap), snap_newest_first);
        const uint32_t keep = g_nsnaps / 2;
        for (uint32_t i = keep; i < g_nsnaps; ++i)
            snap_release(&g_snaps[i]);
        g_nsnaps = keep;
    }
    map_clear(&g_snap_map);
    for (uint32_t i = 0; i < g_nsnaps; ++i)
        map_put(&g_snap_map, &g_snaps[i].key, sizeof(SnapKey), (void*)(uintptr_t)(i + 1));
}

static Snap* snap_find(const SnapKey* k)
{
    uintptr_t at = (uintptr_t)map_get(&g_snap_map, k, sizeof *k);
    return at ? &g_snaps[at - 1] : NULL;
}

/* A caster just drawn: kept as a copy if it's one the cache can't draw again (above), or brought up to
 * date (its uniforms: this frame's camera) if it's kept already. */
/* The shadows' casters over the last few seconds, for the log (fx_stats): what the frames' casters were,
 * and what the cache and its copies kept and drew */
static struct
{
    uint64_t frames, casters, fixed, placed, placed_fixed, made, refused_cpu, refused_room, cache_drawn, copies_drawn, stale, gone,
        culled;
    uint64_t own; /* casters from buffers the game rewrites, read as drawn from the ring (GFX_CASTER_COPY_MOST) */
    uint64_t fading; /* kept casters drawn fading (cache_let_go) */
    uint64_t since;
} g_sst;

static void fx_stats(void)
{
    if (!gfx_watching) /* a line every 5 s: only with [graphics] profile or a /vana debug recording */
        return;
    g_sst.frames++;
    const uint64_t now = gfx_now_ns();
    if (!g_sst.since)
        g_sst.since = now;
    if (now - g_sst.since < 5000000000ull)
        return;
    const double f = (double)g_sst.frames;
    fprintf(stderr, "[recomp] gfx: shadows, a frame over the last 5 s: %.0f casters (%.0f the cache can keep; %.0f placed objects, "
                    "%.0f of them the cache can keep; %.0f from buffers the game rewrites, read as drawn); the cache holds %u, %.0f "
                    "drawn into the maps; %.0f left out past the maps' sides; copies held %u, %.0f drawn; "
                    "%llu copies made, %llu refused (%llu without the vertices, %llu for room), %llu dropped as seen gone; the cache %llu let go of; "
                    "%.0f drawn fading\n",
        g_sst.casters / f, g_sst.fixed / f, g_sst.placed / f, g_sst.placed_fixed / f, g_sst.own / f, g_ncache, g_sst.cache_drawn / f,
        g_sst.culled / f, g_nsnaps, g_sst.copies_drawn / f, (unsigned long long)g_sst.made,
        (unsigned long long)(g_sst.refused_cpu + g_sst.refused_room), (unsigned long long)g_sst.refused_cpu,
        (unsigned long long)g_sst.refused_room, (unsigned long long)g_sst.stale, (unsigned long long)g_sst.gone, g_sst.fading / f);
    memset(&g_sst, 0, sizeof g_sst);
    g_sst.since = now;
}

static void snap_note(const GfxDraw* d, Caster* rec)
{
    if (!rec)
        return;
    g_sst.casters++, g_sst.fixed += rec->fixed, g_sst.own += rec->own;
    if (d->anchor[3] != 0.0f)
        g_sst.placed++, g_sst.placed_fixed += rec->fixed;
    if (rec->fixed || rec->dead || d->anchor[3] == 0.0f || d->prim == GFX_TRIANGLEFAN || g_fxs.sun_casters == 1.0f)
        return;
    SnapKey k;
    memset(&k, 0, sizeof k);
    k.lib = rec->lib;
    memcpy(k.tex, rec->tex, sizeof k.tex);
    k.n = rec->n, k.vstart = d->vertex_start, k.count = d->count, k.prim = d->prim;
    k.vofs = d->u.vofs;
    memcpy(k.stride, d->u.stride, sizeof k.stride);
    k.index_size = d->indices ? d->index_size : 0;
    uint32_t bytes = 0;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        if (!d->buf[s] && !(d->data[s] && d->size[s]))
            continue;
        if (!d->cpu[s] || !d->cpu_size[s])
        {
            g_sst.refused_cpu++;
            return;
        }
        k.size[s] = d->cpu_size[s];
        bytes += (k.size[s] + 15) & ~15u;
    }
    bytes += (rec->n * k.index_size + 15) & ~15u;
    for (int j = 0; j < 3; ++j)
        k.at[j] = (int32_t)floorf(d->anchor[j] * 4.0f + 0.5f);
    Snap* sp = snap_find(&k);
    if (!sp || sp->k.c.dead || sp->chunk < 0)
    {
        /* a copy made (again) */
        if (sp || g_nsnaps >= SNAP_MAX || !snap_room(bytes))
        {
            snap_trim(0, g_nsnaps >= SNAP_MAX || !snap_room(bytes));
            sp = snap_find(&k);
        }
        const int ch = g_nsnaps < SNAP_MAX ? snap_chunk(bytes) : -1;
        if (ch < 0)
        {
            g_sst.refused_room++;
            return;
        }
        g_sst.made++;
        if (!sp)
        {
            if (g_nsnaps == g_snaps_cap)
            {
                g_snaps_cap = g_snaps_cap ? g_snaps_cap * 2 : 256;
                g_snaps = (Snap*)realloc(g_snaps, g_snaps_cap * sizeof(Snap));
            }
            sp = &g_snaps[g_nsnaps++];
            memset(sp, 0, sizeof *sp);
            sp->key = k, sp->chunk = -1;
            map_put(&g_snap_map, &k, sizeof k, (void*)(uintptr_t)g_nsnaps);
        }
        else
            snap_release(sp);
        SnapChunk* c = &g_chunks[ch];
        Caster* cs = &sp->k.c;
        *cs = *rec;
        cs->fixed = 1, cs->dead = 0, cs->iring = 0, cs->own = 0;
        for (int j = 0; j < 8; ++j) /* the textures it samples: their going ends it (gfx_tex_destroy, sun_forget) */
            if (cs->tex[j])
                cs->tex[j]->sun_cached = 1;
        for (int s = 0; s <= GFX_NSTREAMS; ++s)
            cs->buf[s] = NULL; /* its copies are its own: no buffer of the game's ends them */
        for (int s = 0; s < GFX_NSTREAMS; ++s)
        {
            cs->ring[s] = 0, cs->base[s] = 0;
            if (!k.size[s])
                continue;
            buf_upload_at(c->buf, c->used, d->cpu[s], k.size[s]);
            cs->buf[s] = c->buf, cs->base[s] = c->used;
            c->used += (k.size[s] + 15) & ~15u;
        }
        if (k.index_size)
        {
            const uint32_t ib = rec->n * k.index_size;
            buf_upload_at(c->buf, c->used, d->indices, ib);
            cs->buf[GFX_NSTREAMS] = c->buf, cs->ioff = c->used;
            c->used += (ib + 15) & ~15u;
        }
        c->live++;
        sp->chunk = ch;
        sp->k.u = (GfxU*)malloc(sizeof(GfxU));
    }
    sp->k.seen = g_serial, sp->k.fade_ns = 0; /* its camera is taken when the scene is done (sun_cache_update) */
    memcpy(sp->at, d->anchor, 12);
    memcpy(sp->k.u, &d->u, rec->need);
    patch_offsets(sp->k.u, &d->u, &d->vs, sp->k.c.base); /* its streams' places in its chunk */
    sp->k.c.need = rec->need;
}

/* a place well inside the camera's view, in front of it */
static int in_plain_view(const float* at, const GfxScene* s)
{
    float v[4], c[4];
    for (int j = 0; j < 4; ++j)
        v[j] = at[0] * s->view[j] + at[1] * s->view[4 + j] + at[2] * s->view[8 + j] + s->view[12 + j];
    for (int j = 0; j < 4; ++j)
        c[j] = v[0] * s->proj[j] + v[1] * s->proj[4 + j] + v[2] * s->proj[8 + j] + v[3] * s->proj[12 + j];
    if (c[3] < 1.0f)
        return 0; /* behind the camera, or at it */
    return fabsf(c[0]) < 0.9f * c[3] && fabsf(c[1]) < 0.9f * c[3];
}

/* a kept copy's place in plain view near the camera, which the game didn't draw: it isn't there any more
 * (it moved, or another LOD of it is drawn) */
static int snap_stale(const Snap* sp, const GfxScene* s, const float* cam)
{
    const float dx = sp->at[0] - cam[0], dy = sp->at[1] - cam[1], dz = sp->at[2] - cam[2];
    if (dx * dx + dy * dy + dz * dz > 40.0f * 40.0f)
        return 0;
    return in_plain_view(sp->at, s);
}

static void cache_key(CacheKey* k, const Caster* c)
{
    memset(k, 0, sizeof *k);
    memcpy(k->buf, c->buf, sizeof k->buf);
    memcpy(k->base, c->base, sizeof k->base);
    k->ioff = c->ioff, k->n = c->n, k->vstart = c->vstart, k->lib = c->lib;
}

/* The zone's copies of one mesh - FFXI draws a zone's trees of a kind from one buffer, each placed by its
 * vertex shader's constants - share a key: they're told apart by where each stands (its first vertex, run
 * through its function on the CPU: gfx_draw_clip0, taken back to the world through that frame's camera).
 * The entries of a key are chained from the one the map holds (Cached.next). */
static int caster_at(const Caster* c, const float* clip_world, float at[3])
{
    if (!c->has_clip0)
        return 0;
    float w[4];
    for (int j = 0; j < 4; ++j)
        w[j] = c->clip0[0] * clip_world[j] + c->clip0[1] * clip_world[4 + j] + c->clip0[2] * clip_world[8 + j] + c->clip0[3] * clip_world[12 + j];
    if (!(fabsf(w[3]) > 1e-20f))
        return 0;
    for (int j = 0; j < 3; ++j)
        at[j] = w[j] / w[3];
    return isfinite(at[0]) && isfinite(at[1]) && isfinite(at[2]);
}

/* the entry of the chain from head for a caster standing at pos (NULL: not known): the nearest within a unit
 * not yet brought up to date this frame, else the one at the very place that was (the same drawn again) */
static Cached* cache_find(uintptr_t head, const float* pos)
{
    Cached *best = NULL, *again = NULL;
    float bd = 1.0f;
    for (uintptr_t i = head; i; i = g_cache[i - 1].next)
    {
        Cached* ce = &g_cache[i - 1];
        if (!pos || !ce->has_at)
        {
            if (!pos && !ce->has_at)
                return ce;
            continue;
        }
        const float dx = ce->at[0] - pos[0], dy = ce->at[1] - pos[1], dz = ce->at[2] - pos[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (ce->seen != g_serial)
        {
            if (d < bd)
                bd = d, best = ce;
        }
        else if (d < 1e-4f && !again)
            again = ce;
    }
    return best ? best : again;
}

/* entry i onto its key's chain */
static void cache_link(const CacheKey* k, uint32_t i)
{
    const uintptr_t head = (uintptr_t)map_get(&g_cache_map, k, sizeof *k);
    if (!head)
    {
        g_cache[i].next = 0;
        map_put(&g_cache_map, k, sizeof *k, (void*)(uintptr_t)(i + 1));
    }
    else
        g_cache[i].next = g_cache[head - 1].next, g_cache[head - 1].next = i + 1;
}

static void cache_map_free(void)
{
    for (uint32_t i = 0; i < g_cache_map.cap; ++i)
        if (g_cache_map.e[i].hash)
            free(g_cache_map.e[i].key);
    free(g_cache_map.e);
    memset(&g_cache_map, 0, sizeof g_cache_map);
}

/* a buffer or texture gone, or given new contents: this frame's casters and the cache's that draw
 * from it are drawn no more (the cache takes such a caster again when it is next drawn as it is) */
static void sun_forget(GfxBuf* b, GfxTex* t)
{
    for (int pass = 0; pass < 3; ++pass)
    {
        uint32_t n = pass == 2 ? g_nsnaps : pass ? g_ncache : g_ncasters;
        for (uint32_t i = 0; i < n; ++i)
        {
            Caster* c = pass == 2 ? &g_snaps[i].k.c : pass ? &g_cache[i].c : &g_casters[i];
            int hit = 0;
            for (int j = 0; b && j <= GFX_NSTREAMS; ++j)
                hit |= c->buf[j] == b;
            for (int j = 0; t && j < 8; ++j)
                hit |= c->tex[j] == t;
            if (hit)
                c->dead = 1;
        }
    }
    if (b)
        b->sun_seen = 0, b->sun_cached = 0;
    if (t)
        t->sun_seen = 0, t->sun_cached = 0;
}

/* drops what's no longer kept (cache_expired), or went (all of it when all is true), and
 * rebuilds the map */
static void sun_cache_trim(int all)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        if (cache_expired(&g_cache[i]))
            cache_let_go(&g_cache[i]);
        if (all || g_cache[i].c.dead || cache_done(&g_cache[i]))
            free(g_cache[i].u);
        else
            g_cache[n++] = g_cache[i];
    }
    g_ncache = n;
    cache_map_free();
    for (uint32_t i = 0; i < n; ++i)
    {
        CacheKey k;
        cache_key(&k, &g_cache[i].c);
        cache_link(&k, i);
    }
}

/* this frame's fixed casters into the cache (new ones added, seen ones brought up to date), and what the
 * cache holds that stands in plain view but the game didn't draw dropped: it moved, or went (another LOD
 * of it is drawn, or the game draws it no more) */
static void sun_cache_update(const GfxScene* s, const float* clip_world, const float* cam)
{
    g_fade_now = gfx_now_ns();
    float dx = cam[0] - g_cache_cam[0], dy = cam[1] - g_cache_cam[1], dz = cam[2] - g_cache_cam[2];
    if (dx * dx + dy * dy + dz * dz > 50.0f * 50.0f)
    {
        sun_cache_trim(1);
        snap_trim(1, 0);
        g_fx.fogc_set = 0;
    }
    memcpy(g_cache_cam, cam, 12);
    if (!(g_serial & 255))
        sun_cache_trim(0), snap_trim(0, 0);
    /* the copies drawn this frame: their uniforms were this frame's camera */
    for (uint32_t i = 0; i < g_nsnaps; ++i)
        if (g_snaps[i].k.seen == g_serial)
            memcpy(g_snaps[i].k.clip_world, clip_world, 64), memcpy(g_snaps[i].k.cam, cam, 12);
    if (g_fxs.sun_casters == 1.0f)
        return; /* characters alone cast: nothing of the zone's to keep */
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        const Caster* c = &g_casters[i];
        if (!c->fixed || c->dead)
            continue;
        CacheKey k;
        cache_key(&k, c);
        float pos[3];
        const int has_at = caster_at(c, clip_world, pos);
        Cached* ce = cache_find((uintptr_t)map_get(&g_cache_map, &k, sizeof k), has_at ? pos : NULL);
        if (!ce)
        {
            if (g_ncache == g_cache_cap)
            {
                g_cache_cap = g_cache_cap ? g_cache_cap * 2 : 1024;
                g_cache = (Cached*)realloc(g_cache, g_cache_cap * sizeof(Cached));
            }
            ce = &g_cache[g_ncache++];
            memset(ce, 0, sizeof *ce);
            ce->u = (GfxU*)malloc(sizeof(GfxU));
            cache_link(&k, g_ncache - 1);
        }
        /* as drawn this frame: live again if something it drew from had gone, and the same buffer now
         * holds what the game draws */
        ce->c = *c;
        for (int j = 0; j <= GFX_NSTREAMS; ++j)
            if (c->buf[j])
                c->buf[j]->sun_cached = 1;
        for (int j = 0; j < 8; ++j)
            if (c->tex[j])
                c->tex[j]->sun_cached = 1;
        memcpy(ce->u, g_caster_u + c->cpu_u, c->need);
        memcpy(ce->clip_world, clip_world, 64);
        memcpy(ce->cam, cam, 12);
        ce->seen = g_serial, ce->ring_serial = 0, ce->fade_ns = 0;
        ce->has_at = (uint8_t)has_at;
        if (has_at)
            memcpy(ce->at, pos, 12);
    }
    /* in plain view but not drawn, or no longer kept: let go of (fading, sun_fade), and dropped once faded */
    for (uint32_t i = 0; i < g_ncache; ++i)
    {
        Cached* ce = &g_cache[i];
        if (ce->c.dead || ce->seen == g_serial)
            continue;
        if (!ce->fade_ns && ((ce->has_at && in_plain_view(ce->at, s)) || cache_expired(ce)))
            cache_let_go(ce), g_sst.gone++;
        if (cache_done(ce))
            ce->c.dead = 1;
    }
}

/* The sun's own size: its disc is half a degree across, so a shadow's edge softens by about this much (in
 * radius) for each unit between the caster and the ground. sun_soft is never less: at 0 a character's shadow
 * is still hard (a centimetre over a couple of units), but a cliff's 30 units away no longer ends in
 * razor-sharp teeth of its low-polygon top. */
#define SUN_SIZE 0.0047f

/* One cascade of the sun's map: an orthographic view along the sun over a sphere around the slice
 * [t0, t1] of what the camera sees, its center snapped to whole texels (the shadows' edges hold still
 * as the camera moves). */
typedef struct SunCascade
{
    float S[16];    /* the world to the map */
    float lmat[16]; /* view space to the map: x, y -1..1, z 0..1 */
    float texel, bias, soft, slope, range, across;
    float ox, oy;   /* the world's texel at the map's pixel (x, y) is (x + ox, oy - y): a fading caster's pattern (sun_f) */
} SunCascade;

static void sun_fit(const GfxScene* s, const float* invV, const float* L, float t0, float t1, uint32_t size, SunCascade* k)
{
    float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f, p[8][3], c[3] = { 0, 0, 0 };
    for (int i = 0; i < 8; ++i)
    {
        float t = i < 4 ? t0 : t1, z = t * hand, nx = (i & 1) ? 1.0f : -1.0f, ny = (i & 2) ? 1.0f : -1.0f;
        float v[4] = { (nx * t - s->proj[8] * z) / s->proj[0], (ny * t - s->proj[9] * z) / s->proj[5], z, 1.0f };
        for (int j = 0; j < 3; ++j)
            p[i][j] = v[0] * invV[j] + v[1] * invV[4 + j] + v[2] * invV[8 + j] + invV[12 + j], c[j] += p[i][j] / 8.0f;
    }
    float R = 0.0f;
    for (int i = 0; i < 8; ++i)
    {
        float dx = p[i][0] - c[0], dy = p[i][1] - c[1], dz = p[i][2] - c[2];
        R = fmaxf(R, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    R = ceilf(R); /* whole units: the texel's size holds still */
    /* the sun's axes: f the way its light goes, r and u across */
    float f[3] = { -L[0], -L[1], -L[2] }, up[3] = { 0, 1, 0 };
    if (fabsf(f[1]) > 0.9f)
        up[0] = 1, up[1] = 0;
    float r[3] = { up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0] };
    normalize3(r);
    float u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
    float tx = 2.0f * R / (float)size;
    float nx = floorf((c[0] * r[0] + c[1] * r[1] + c[2] * r[2]) / tx), ny = floorf((c[0] * u[0] + c[1] * u[1] + c[2] * u[2]) / tx);
    float cx = nx * tx, cy = ny * tx;
    k->ox = nx - 0.5f * (float)size, k->oy = ny + 0.5f * (float)size;
    float cz = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
    /* casters stand up to 200 units sunward of the sphere (a cliff over the camera) */
    float back = 200.0f, range = 2.0f * R + back, z0 = cz - R - back;
    float S[16] = {
        r[0] / R, u[0] / R, f[0] / range, 0,
        r[1] / R, u[1] / R, f[1] / range, 0,
        r[2] / R, u[2] / R, f[2] / range, 0,
        -cx / R, -cy / R, -z0 / range, 1,
    };
    memcpy(k->S, S, sizeof S);
    mat_mul(k->lmat, invV, S);
    k->texel = tx, k->bias = 0.03f / range, k->range = range, k->across = 2.0f * R;
    /* the penumbra's radius grows by sun_soft for each unit from the caster: in the map's width
     * (2R across) per unit of its depth (range deep) */
    k->soft = fmaxf(g_fxs.sun_soft, SUN_SIZE) * range / (2.0f * R);
    k->slope = 2.0f * R / range;
}

/* a shadow map let go */
static void sun_release(SunMap* m)
{
    if (m->dsv)
        ID3D11DepthStencilView_Release(m->dsv);
    if (m->srv)
        ID3D11ShaderResourceView_Release(m->srv);
    if (m->res)
        ID3D11Texture2D_Release(m->res);
    m->res = NULL, m->srv = NULL, m->dsv = NULL;
}

/* the sharp map's size: sun_detail texels across, 512-8192 (8192: 256 MB); 0 and 1 as they were, 4096 and 8192 */
static uint32_t sun_size(float detail)
{
    if (detail < 0.5f)
        return SUN_MAP;
    if (detail < 1.5f)
        return 2 * SUN_MAP;
    uint32_t n = 512;
    while (n < 8192 && (float)n * 1.5f <= detail)
        n *= 2;
    return n;
}

/* a shadow map (32-bit depth, size square), made the first time and again at another size */
static int sun_target(SunMap* m, uint32_t size)
{
    if (m->res && m->size == size)
        return 1;
    if (m->failed && m->size == size)
        return 0;
    sun_release(m);
    m->size = size, m->failed = 0;
    D3D11_TEXTURE2D_DESC d = { size, size, 1, 1, DXGI_FORMAT_R32_TYPELESS, { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SHADER_RESOURCE_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.Format = DXGI_FORMAT_R32_FLOAT, v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = 1;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv;
    memset(&dv, 0, sizeof dv);
    dv.Format = DXGI_FORMAT_D32_FLOAT, dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &m->res)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)m->res, &v, &m->srv)) ||
        FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource*)m->res, &dv, &m->dsv)))
    {
        fprintf(stderr, "[recomp] gfx: the sun's shadow map (%ux%u) failed; no map shadows\n", size, size);
        if (m->srv)
            ID3D11ShaderResourceView_Release(m->srv);
        if (m->res)
            ID3D11Texture2D_Release(m->res);
        m->res = NULL, m->srv = NULL, m->dsv = NULL, m->failed = 1;
        return 0;
    }
    return 1;
}

/* the matrix the casters' positions go through into the map (b2), then a fading caster's share of its texels and cascade k's
 * texel offset (sun_f: cache_let_go) */
static void sun_matrix(const float* m, float share, const SunCascade* k)
{
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_fx.sun_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
    {
        const float f[4] = { share, k->ox, k->oy, 0.0f };
        memcpy(ms.pData, m, 64);
        memcpy((uint8_t*)ms.pData + 64, f, 16);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_fx.sun_cb, 0);
    }
}

/* How far past a map's sides a caster may stand (where its first vertex is: a corner of an object, of a
 * piece of the zone) and still reach into it. Its shadow falls along the light, where the map's x and y
 * don't change: only the caster's own size reaches across them. */
#define SUN_REACH 96.0f

/* True when a caster standing at `at` can't reach the cascade's map (SUN_REACH or more past its sides). The
 * cache keeps what was seen for 30 seconds, far past the maps with a long draw distance: thousands of
 * casters, each drawn into both maps every frame, until a relog cleared them. */
static int beyond_map(const SunCascade* k, const float at[3])
{
    const float x = at[0] * k->S[0] + at[1] * k->S[4] + at[2] * k->S[8] + k->S[12];
    const float y = at[0] * k->S[1] + at[1] * k->S[5] + at[2] * k->S[9] + k->S[13];
    const float edge = 1.0f + SUN_REACH * 2.0f / k->across; /* the map: -1..1 over its 2R across */
    return fabsf(x) > edge || fabsf(y) > edge;
}

/* the casters into one cascade's map: this frame's, and the zone's kept from before (cache); chars 1, this frame's
 * characters alone (as gfx_d3d12.c's) */
static uint32_t sun_draw(SunMap* m, const GfxScene* s, const float* invP, const float* invV, const SunCascade* k, int cache,
    int chars)
{
    float clip_world[16], M[16];
    mat_mul(clip_world, invP, invV);
    mat_mul(M, clip_world, k->S); /* the camera's clip space -> the map */
    ID3D11ShaderResourceView* none[10] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 10, none); /* the map may still be bound for reading */
    ID3D11DeviceContext_ClearDepthStencilView(g_ctx, m->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, m->dsv);
    D3D11_VIEWPORT v = { 0, 0, (float)m->size, (float)m->size, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
    GfxDepthKey dk;
    memset(&dk, 0, sizeof dk);
    dk.zenable = 1, dk.zwrite = 1, dk.zfunc = 2;
    RasterKey rk = { 1, 3, 1, 0, 0 };
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend_none, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, depth_state(&dk), 0);
    ID3D11DeviceContext_RSSetState(g_ctx, raster_state(&rk));
    sun_matrix(M, 1.0f, k);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 2, 1, &g_fx.sun_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 2, 1, &g_fx.sun_cb); /* (sun_f: a fading caster's pixel function) */
    g_targets_bound = 0;
    bound_forget();
    uint32_t drawn = 0, total = g_ncasters + (cache ? g_ncache + g_nsnaps : 0);
    for (uint32_t i = 0; i < total; ++i)
    {
        Caster* cs;
        Cached* ce = NULL;
        if (i < g_ncasters)
        {
            cs = &g_casters[i];
            /* sun_casters 1: characters alone cast - the zone's shadows are baked into its colours
             * already, and the game tints them for the hour and the weather; 2: the zone alone (the
             * game's own shadows stay under characters) */
            if (cs->dead || ((chars || g_fxs.sun_casters == 1.0f) && cs->fixed) || (!chars && g_fxs.sun_casters == 2.0f && !cs->fixed))
                continue;
            float at[3];
            if (caster_at(cs, clip_world, at) && beyond_map(k, at))
            {
                g_sst.culled++;
                continue;
            }
        }
        else
        {
            if (g_fxs.sun_casters == 1.0f)
                break;
            /* the zone out of view: as it was drawn when last seen, through that frame's camera; then the
             * copies (snap_note) */
            if (i < g_ncasters + g_ncache)
                ce = &g_cache[i - g_ncasters];
            else
            {
                Snap* sp = &g_snaps[i - g_ncasters - g_ncache];
                ce = &sp->k;
                if (!ce->c.dead && ce->seen != g_serial && sp->chunk >= 0 && snap_stale(sp, s, invV + 12))
                    ce->c.dead = 1, g_sst.stale++; /* in plain view, and not drawn: it moved, or went */
                if (sp->chunk < 0)
                    continue;
            }
            if (!ce->c.dead && ce->seen != g_serial && cache_expired(ce))
                cache_let_go(ce); /* fading (sun_fade) */
            if (ce->c.dead || ce->seen == g_serial || cache_done(ce))
                continue;
            if (ce->has_at && beyond_map(k, ce->at))
            {
                g_sst.culled++;
                continue;
            }
            cs = &ce->c;
        }
        /* vertices or indices in a chunk written again since they were drawn (only where every write
         * DISCARDs): gone */
        int rv = 0;
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            rv |= cs->ring[s];
        if ((rv && g_vring.c[cs->vchunk].gen != cs->vgen) || (cs->iring && g_iring.c[cs->ichunk].gen != cs->igen))
        {
            g_fx.tr_gone++;
            continue;
        }
        LibKey lk = cs->lib;
        lk.vs.shadow = 1, lk.vs.pixel = 0;
        int at = alpha_tested(&cs->lib.fs);
        if (!at)
        {
            /* the position alone: one pair serves every draw with the same vertex layout */
            GfxVsKey* vk = &lk.vs;
            vk->lighting = vk->normalize = vk->localviewer = vk->specular = 0;
            vk->src_diffuse = vk->src_specular = vk->src_ambient = vk->src_emissive = 0;
            vk->nlights = 0, memset(vk->light_type, 0, sizeof vk->light_type);
            vk->fog_vertex = vk->range_fog = 0, vk->ntex = 0, vk->flat = 0;
            memset(vk->tci, 0, sizeof vk->tci), memset(vk->ttf, 0, sizeof vk->ttf);
            memset(&lk.fs, 0, sizeof lk.fs);
        }
        /* let go of and fading (cache_let_go): a share of its texels, through its fading pair - drawn whole while that one
         * builds */
        const float share = ce ? cache_share(ce) : 1.0f;
        Shaders* sh = NULL;
        if (share < 1.0f)
        {
            lk.vs.shadow = 2;
            sh = shaders_for(&lk, cs->vs, cs->ps);
            lk.vs.shadow = 1;
        }
        const int fading = sh != NULL;
        if (!sh)
            sh = shaders_for(&lk, cs->vs, cs->ps);
        if (!sh)
        {
            g_fx.tr_skipped++;
            g_fx.tr_building += g_pair_waiting;
            continue;
        }
        uint32_t uoff;
        if (ce)
        {
            /* its uniforms, once a frame (both cascades read them), unless the ring moved on between */
            if (ce->ring_serial != g_serial || ce->ring_gen != g_cring.gen)
            {
                if (!uniforms_put(ce->u, ce->c.need, NULL, NULL, &ce->ring_u))
                    continue;
                ce->ring_serial = g_serial, ce->ring_gen = g_cring.gen;
            }
            uoff = ce->ring_u;
            float mm[16];
            mat_mul(mm, ce->clip_world, k->S);
            sun_matrix(mm, fading ? share : 1.0f, k);
            g_sst.fading += fading;
            g_fx.tr_cached++;
            if (i >= g_ncasters + g_ncache)
                g_sst.copies_drawn++;
            else
                g_sst.cache_drawn++;
        }
        else
        {
            /* the ring moved on since it was drawn: its uniforms again, from the copy */
            if (cs->ugen != g_cring.gen)
            {
                if (!uniforms_put((const GfxU*)(g_caster_u + cs->cpu_u), cs->need, NULL, NULL, &cs->uoff))
                    continue;
                cs->ugen = g_cring.gen;
            }
            uoff = cs->uoff;
            g_fx.tr_live++;
        }
        if (at)
        {
            if (memcmp(cs->srv, g_bound.tex, sizeof cs->srv))
                ID3D11DeviceContext_PSSetShaderResources(g_ctx, T_TEX2D, 16, cs->srv), memcpy(g_bound.tex, cs->srv, sizeof cs->srv);
            if (memcmp(cs->samp, g_bound.samp, sizeof cs->samp))
                ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 8, cs->samp), memcpy(g_bound.samp, cs->samp, sizeof cs->samp);
        }
        ID3D11PixelShader* ps = at || fading ? sh->ps : NULL; /* depth alone, but for an alpha test or a fade */
        if (sh->vs != g_bound.vs)
            ID3D11DeviceContext_VSSetShader(g_ctx, sh->vs, NULL, 0), g_bound.vs = sh->vs;
        if (ps != g_bound.ps)
            ID3D11DeviceContext_PSSetShader(g_ctx, ps, NULL, 0), g_bound.ps = ps;
        set_topology((D3D11_PRIMITIVE_TOPOLOGY)cs->topo);
        uniforms_bind(uoff);
        ID3D11ShaderResourceView* st[GFX_NSTREAMS];
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            st[s] = cs->buf[s] ? cs->buf[s]->srv : cs->ring[s] ? g_vring.c[cs->vchunk].srv : NULL;
        if (memcmp(st, g_bound.streams, sizeof st))
            ID3D11DeviceContext_VSSetShaderResources(g_ctx, 0, GFX_NSTREAMS, st), memcpy(g_bound.streams, st, sizeof st);
        if (cs->ifmt != DXGI_FORMAT_UNKNOWN)
        {
            ID3D11Buffer* ib = cs->iring ? g_iring.c[cs->ichunk].buf : cs->buf[GFX_NSTREAMS] ? cs->buf[GFX_NSTREAMS]->res : NULL;
            if (!ib)
                continue;
            set_index_buffer(ib, cs->ifmt, cs->ioff);
            ID3D11DeviceContext_DrawIndexed(g_ctx, cs->n, 0, 0);
        }
        else
            ID3D11DeviceContext_Draw(g_ctx, cs->n, cs->vstart);
        drawn++;
    }
    g_targets_bound = 0; /* the game's next draw binds its own again */
    bound_forget();
    return drawn;
}

#include "gfx_d3d11_smooth.h" /* vn_replay: the scene's own smooth normals for the sun's shading (sun_smooth) */
#include "gfx_d3d11_mirror.h" /* gfx_scene_mirror: the world mirrored in the water ([graphics] water.mirror) */

/* VanaCore's zone lines into the effects' uniforms: the ZL_MAX nearest the camera (cam) within ZL_FAR, each its
 * centre and axes in view space, its half sizes along them; none without the camera */
#define ZL_MAX 16
#define ZL_FAR 200.0f
static void zone_lines(const GfxScene* s, const float* cam, FxU* u)
{
    u->zlc[0] = 0.0f;
    if (!cam || s->nzonelines == 0 || !(s->zoneline_color[3] > 0.0f))
        return;
    uint32_t pick[ZL_MAX], n = 0;
    float pd[ZL_MAX];
    for (uint32_t i = 0; i < s->nzonelines && i < GFX_ZONELINES; ++i)
    {
        const float* b = s->zonelines[i];
        const float dx = b[0] - cam[0], dy = b[1] - cam[1], dz = b[2] - cam[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz) - 0.5f * sqrtf(b[4] * b[4] + b[5] * b[5] + b[6] * b[6]);
        if (d > ZL_FAR || (n == ZL_MAX && d >= pd[ZL_MAX - 1]))
            continue;
        uint32_t at = n < ZL_MAX ? n++ : ZL_MAX - 1;
        for (; at > 0 && pd[at - 1] > d; --at)
            pd[at] = pd[at - 1], pick[at] = pick[at - 1];
        pd[at] = d, pick[at] = i;
    }
    for (uint32_t j = 0; j < n; ++j)
    {
        const float* b = s->zonelines[pick[j]];
        const float c = cosf(b[3]), sn = sinf(b[3]);
        const float axes[3][3] = { { c, 0.0f, -sn }, { 0.0f, 1.0f, 0.0f }, { sn, 0.0f, c } };
        float* o = u->zl[j * 4];
        for (int k = 0; k < 3; ++k)
            o[k] = b[0] * s->view[k] + b[1] * s->view[4 + k] + b[2] * s->view[8 + k] + s->view[12 + k];
        o[3] = b[7]; /* how far above its middle the ground is (as gfx_d3d12.c's) */
        /* the line along the box's longer side across, the plane facing its shorter (as gfx_d3d12.c's) */
        const int swap = b[6] > b[4];
        for (int a = 0; a < 3; ++a)
        {
            const int from = swap && a != 1 ? 2 - a : a;
            float* ax = u->zl[j * 4 + 1 + a];
            for (int k = 0; k < 3; ++k)
                ax[k] = axes[from][0] * s->view[k] + axes[from][1] * s->view[4 + k] + axes[from][2] * s->view[8 + k];
            normalize3(ax);
            ax[3] = 0.5f * b[4 + from];
        }
    }
    u->zlc[0] = (float)n, u->zlc[1] = ZL_FAR * 0.75f, u->zlc[2] = s->zoneline_height;
    u->zlc[3] = s->zoneline_size > 0.0f ? s->zoneline_size : 1.0f;
    memcpy(u->zlcol, s->zoneline_color, sizeof u->zlcol);
    u->zlp[0] = (float)s->zoneline_style, u->zlp[1] = s->zoneline_glow ? 1.0f : 0.0f;
    u->zlp[2] = (float)fmod((double)gfx_now_ns() / 1e9, 3600.0), u->zlp[3] = s->zoneline_fade;
}

/* VanaCore's sparkles into the effects' uniforms, as gfx_d3d12.c's */
#define SP_FAR 120.0f
static void sparkles(const GfxScene* s, const float* cam, FxU* u)
{
    u->spc[0] = 0.0f;
    if (!cam || s->nsparkles == 0)
        return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->nsparkles && i < GFX_SPARKLES; ++i)
    {
        const float* p = s->sparkles[i];
        const float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
        if (!(p[11] > 0.0f) || sqrtf(dx * dx + dy * dy + dz * dz) - p[3] > SP_FAR)
            continue;
        float* o = u->sp[n * 3];
        for (int k = 0; k < 3; ++k)
            o[k] = p[0] * s->view[k] + p[1] * s->view[4 + k] + p[2] * s->view[8 + k] + s->view[12 + k];
        o[3] = p[3];
        memcpy(u->sp[n * 3 + 1], p + 4, sizeof(float) * 8);
        n++;
    }
    if (n == 0)
        return;
    for (int k = 0; k < 3; ++k)
        u->spx[k] = s->view[k], u->spz[k] = s->view[8 + k];
    normalize3(u->spx), normalize3(u->spz);
    static uint64_t last_ns;
    static double t;
    const uint64_t now = gfx_now_ns();
    if (last_ns && now > last_ns)
        t = fmod(t + fmin((double)(now - last_ns) / 1e9, 0.25) * (s->sparkle_speed > 0.0f ? s->sparkle_speed : 1.0f), 3600.0);
    last_ns = now;
    u->spc[0] = (float)n, u->spc[1] = (float)t;
    u->spp[0] = s->sparkle_match ? 1.0f : 0.0f, u->spp[1] = (float)fmod((double)now / 1e9, 3600.0);
    u->spc[2] = s->sparkle_size > 0.0f ? s->sparkle_size : 1.0f;
    u->spc[3] = s->sparkle_density > 0.0f ? s->sparkle_density : 1.0f;
}

/* VanaCore's rings of light on the ground (vanacore_gfx_set_ground_marks) into the effects' uniforms: those within GM_FAR of
 * the camera (cam), each its centre in view space and its radius, then its colour and strength, then its shape, its
 * column's height, its glyph, the glyph's size and height; the world's ground axes in view space with them; none without the
 * camera */
#define GM_FAR 70.0f
static void ground_marks(const GfxScene* s, const float* cam, FxU* u)
{
    u->gmc[0] = 0.0f;
    if (!cam || s->nground_marks == 0)
        return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->nground_marks && i < GFX_GROUND_MARKS; ++i)
    {
        const float* p = s->ground_marks[i];
        const float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
        if (!(p[3] > 0.0f) || !(p[7] > 0.0f) || sqrtf(dx * dx + dy * dy + dz * dz) - p[3] > GM_FAR)
            continue;
        float* o = u->gm[n * 4];
        for (int k = 0; k < 3; ++k)
            o[k] = p[0] * s->view[k] + p[1] * s->view[4 + k] + p[2] * s->view[8 + k] + s->view[12 + k];
        o[3] = p[3];
        memcpy(u->gm[n * 4 + 1], p + 4, sizeof(float) * 4);
        memcpy(u->gm[n * 4 + 2], p + 8, sizeof(float) * 4);
        memcpy(u->gm[n * 4 + 3], p + 12, sizeof(float) * 4);
        n++;
    }
    if (n == 0)
        return;
    for (int k = 0; k < 3; ++k)
        u->gmx[k] = s->view[k], u->gmz[k] = s->view[8 + k];
    normalize3(u->gmx), normalize3(u->gmz);
    u->gmc[0] = (float)n, u->gmc[1] = (float)fmod((double)gfx_now_ns() / 1e9, 3600.0);
}

/* --- FSR on Direct3D 11 (VanaCore) -------------------------------------------------------------------------------
 * AMD's FSR is Direct3D 12's and 64-bit, and FFXI is 32-bit: the DLSS helper (VanaCore.Dlss.exe, dlss_ipc.h) runs it
 * for Direct3D 11 too, on textures and a fence of this device's shared with it (NT handles: Direct3D 12 opens Direct3D
 * 11's), the way gfx_d3d12.c's DLSS and FSR go. Each frame the scene's colour (drawn at the smaller size [graphics] fsr
 * asks for, its draws jittered by a sub-pixel step: gfx_dlss_jitter), its depth and its motion (gfx_d3d12.c's passes,
 * fx_dlssz and fx_dlssmv, from the same HLSL) go into the shared inputs; the helper's output, at the size the scene is
 * shown at, is drawn into `out`, which the frame's draws then sample in place of the scene's target. The game's
 * interface drawn into the scene before the game is done with it (the names over heads) is held and drawn over the
 * output at its size, tested against the scene's depth made that size (fx_depthup). Direct3D 11 can't signal a fence
 * from the processor, as gfx_d3d12.c does to let a frame through when its helper stops answering: so the context
 * waits on the helper's work only once the helper has handed it to its queue (sh->done) - a helper that ends never
 * leaves the game's GPU waiting. FSR alone here (DLSS is Direct3D 12's), and no frame generation. */
static struct
{
    int quality;           /* gfx_dlss_set's: an NVSDK_NGX_PerfQuality_Value (the scale it's drawn at), or -1 off */
    int fsr;               /* the upscaler asked for is FSR (gfx11_upscaler): DLSS's quality turns nothing on here */
    float sharpness;       /* FSR's own sharpening, 0 (off) to 1 (gfx11_upscaler) */
    int version_want;      /* FSR's version asked for: 0 its own choice, else a major version (2, 3, 4) */
    wchar_t helper[MAX_PATH], log[MAX_PATH];
    int failed;            /* given up: no helper, FSR not there, or the helper stopped answering */
    int said;              /* the state last logged */
    char why[160];         /* ... why it gave up (3, 4) */
    HANDLE map, go, ready, go_fg, proc;
    /* No fence shared (cpu_sync: Wine and Proton can't open one from another process): the fence is this side's own, the
     * game's thread waits for it on the CPU before it sets ".go", and the helper sets ".done" when its work is through
     * (dlss_ipc.h) */
    int cpu_sync;
    HANDLE done, done_fg, cpu_ev; /* (done_fg: the helper opens both, frame generation is Direct3D 12's alone) */
    VanaDlssShared* sh;
    ID3D11Device5* dev5;   /* the fence's device and context (Direct3D 11.4: Windows 10 1703 or later) */
    ID3D11DeviceContext4* ctx4;
    ID3D11Fence* fence;    /* shared: the game's at `frame` (inputs drawn), the helper's at frame + 1 (output) */
    uint64_t value;
    ID3D11Texture2D* res[4];         /* shared: colour, depth, motion, output */
    ID3D11RenderTargetView* rtv[3];  /* colour's, depth's and motion's */
    ID3D11ShaderResourceView* srv_out;
    uint32_t in_w, in_h, out_w, out_h;
    DXGI_FORMAT cfmt;
    int made_quality, made_version;
    GfxTex* out;           /* the output as the frame's draws sample it, in place of `scene` */
    GfxTex* scene;
    uint64_t out_serial;   /* the frame `out` holds */
    float prev_view[16], prev_proj[16], prev_cam[3];
    uint64_t prev_serial;
    int reset;
    uint32_t n;            /* frames so far: the jitter's place in its sequence */
    float jit[2];          /* this frame's, in render pixels */
    uint64_t jit_serial;
    ULONGLONG made_at;     /* new textures handed to the helper, when */
    uint64_t ran_ns;       /* when a frame last went to the helper: FSR's time between frames */
    /* the scene as its effects finished it (gfx_scene_done), through FSR when a draw first reads it */
    uint64_t pend_serial, noted;
    GfxTex *pend_color, *pend_depth;
    GfxScene pend_s;
    /* the passes (up_build, with the effects'): fx_dlssz, fx_dlssmv, fx_dlssc (the colour onto the shared input: a
     * B8G8R8X8 scene, X8R8G8B8's here, isn't one FSR reads), fx_mip (the output onto `out`), fx_depthup */
    ID3D11PixelShader *ps_z, *ps_mv, *ps_color, *ps_copy, *ps_zup;
    ID3D11DepthStencilState* zwrite; /* fx_depthup's: always passes, written */
    GfxTex* zup;           /* the scene's depth at the output's size, for the interface drawn over it */
    GfxTex* replay_depth;  /* ... while the held interface is drawn over the output (NULL: no depth test) */
    /* FSR 1 (up_fsr1_run), in AMD's FSR's place: its passes (fx_easu, fx_rcas), EASU's output when RCAS follows; it ran
     * (`out` is its, made for in_w..out_h), it couldn't (off), it was logged */
    ID3D11PixelShader *ps_easu, *ps_rcas;
    FxTex mid;
    int fsr1, fsr1_failed, fsr1_said;
} g_up = { -1 };

/* The scene effects' shaders compiled again (gfx.h gfx_fx_rebuild: VanaCore's Recompile shaders): asked for from any
 * thread, taken at the end of a frame (gfx_present) once no build is on. The effects' functions and FSR's (up_build's)
 * are let go (Direct3D 11 keeps what a frame still uses) and the build state put back to not built, so the next scene
 * that asks starts it over on the thread pool, as the first did (fx_ready); the effects are off till it's done, and the
 * notice shows it. FSR 1 gets another go if its passes were the ones missing. */
static volatile LONG g_fx_rebuild;

void gfx_fx_rebuild(void)
{
    InterlockedExchange(&g_fx_rebuild, 1);
}

static void fx_rebuild_take(void)
{
    if (!InterlockedCompareExchange(&g_fx_rebuild, 0, 0) || InterlockedCompareExchange(&g_fx.state, 0, 0) == 1)
        return;
    InterlockedExchange(&g_fx_rebuild, 0);
#define LET_GO_PS(p) do { if (p) ID3D11PixelShader_Release(p), (p) = NULL; } while (0)
    if (g_fx.vs)
        ID3D11VertexShader_Release(g_fx.vs), g_fx.vs = NULL;
    for (int i = 0; i < FX_NPS; ++i)
        LET_GO_PS(g_fx.ps[i]);
    LET_GO_PS(g_up.ps_z);
    LET_GO_PS(g_up.ps_mv);
    LET_GO_PS(g_up.ps_color);
    LET_GO_PS(g_up.ps_copy);
    LET_GO_PS(g_up.ps_zup);
    LET_GO_PS(g_up.ps_easu);
    LET_GO_PS(g_up.ps_rcas);
#undef LET_GO_PS
    g_up.fsr1_failed = 0, g_up.fsr1_said = 0;
    InterlockedExchange(&g_fx.step, 0);
    InterlockedExchange(&g_fx.steps, 0);
    InterlockedExchange(&g_fx.state, 0);
    fprintf(stderr, "[recomp] gfx: the scene effects' shaders let go; compiled again at the next scene\n");
}

/* the render size over the output's at each quality (NVSDK_NGX_PerfQuality_Value: performance, balanced, quality, ultra
 * performance, ultra quality, native) - near enough AMD's own (2x, 1.7x, 1.5x, 3x, 1x) */
static float up_factor(int quality)
{
    static const float k[] = { 0.5f, 0.58f, 0.667f, 0.333f, 0.77f, 1.0f };
    return quality >= 0 && quality < 6 ? k[quality] : 1.0f;
}

static float up_halton(uint32_t i, uint32_t b)
{
    float f = 1.0f, r = 0.0f;
    for (; i; i /= b)
        f /= (float)b, r += f * (float)(i % b);
    return r;
}

static void up_say(int state, const char* why)
{
    if (state == g_up.said)
        return;
    g_up.said = state;
    fprintf(stderr, "[recomp] gfx: FSR (Direct3D 11): %s\n", why);
    if (state >= 3)
    {
        snprintf(g_up.why, sizeof g_up.why, "%s", why);
        char* off = strstr(g_up.why, "; off");
        if (off)
            *off = 0;
    }
}

/* fx_build's, with the effects' own: the passes FSR needs from the same HLSL (on the thread pool) */
static void up_build(const char* src)
{
    static const char* const entry[5] = { "fx_dlssz", "fx_dlssmv", "fx_dlssc", "fx_mip", "fx_depthup" };
    ID3D11PixelShader** ps[5] = { &g_up.ps_z, &g_up.ps_mv, &g_up.ps_color, &g_up.ps_copy, &g_up.ps_zup };
    for (int i = 0; i < 5; ++i)
    {
        InterlockedExchangePointer(&g_fx.at, (PVOID)gfx_fx_words(entry[i]));
        ID3DBlob* b = src ? compile(src, entry[i], "ps_5_0") : NULL;
        if (!b || FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL,
                      ps[i])))
        {
            *ps[i] = NULL;
            fprintf(stderr, "[recomp] gfx: FSR (Direct3D 11): %s didn't compile\n", entry[i]);
        }
        if (b)
            ID3D10Blob_Release(b);
        InterlockedIncrement(&g_fx.step);
    }
    /* FSR 1's (up_fsr1_run): without them FSR 1 is off, nothing else */
    static const char* const fsr1[2] = { "fx_easu", "fx_rcas" };
    ID3D11PixelShader** ps1[2] = { &g_up.ps_easu, &g_up.ps_rcas };
    for (int i = 0; i < 2; ++i)
    {
        InterlockedExchangePointer(&g_fx.at, (PVOID)gfx_fx_words(fsr1[i]));
        ID3DBlob* b = src ? compile(src, fsr1[i], "ps_5_0") : NULL;
        if (!b || FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL,
                      ps1[i])))
        {
            *ps1[i] = NULL;
            fprintf(stderr, "[recomp] gfx: FSR 1 (Direct3D 11): %s didn't compile\n", fsr1[i]);
        }
        if (b)
            ID3D10Blob_Release(b);
        InterlockedIncrement(&g_fx.step);
    }
}

/* FSR 1 (up_fsr1_run: VanaCore's own, in this process) in AMD's FSR's place, as gfx_d3d12.c's fsr1_wanted: asked for
 * ([graphics] fsr.version=1), or AMD's FSR isn't there or gave up (its DLLs missing, nothing for this card, no Direct3D
 * 11.4, the helper gone) */
static int up_fsr1(void)
{
    return g_up.quality >= 0 && g_up.fsr && !g_up.fsr1_failed && (g_up.version_want == 1 || g_up.failed);
}

/* FSR given up (off till it's turned on again): AMD's helper, and FSR 1 too where it was to stand in for it */
static int up_gave_up(void)
{
    return g_up.version_want == 1 || g_up.failed ? g_up.fsr1_failed : 0;
}

/* AMD's FSR DLLs beside the helper (a VanaCore build ships them in its dlss folder) */
static int up_dlls_here(void)
{
    static const wchar_t* const names[] = { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll" };
    const wchar_t* slash = wcsrchr(g_up.helper, L'\\');
    if (!slash)
        return 0;
    const int dir = (int)(slash - g_up.helper) + 1;
    for (int i = 0; i < 2; ++i)
    {
        wchar_t dll[MAX_PATH];
        swprintf(dll, MAX_PATH, L"%.*ls%ls", dir, g_up.helper, names[i]);
        if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
            return 0;
    }
    return 1;
}

/* an object of this device as an NT handle in the helper's process (0: it couldn't be) */
static uint64_t up_pass_on(HANDLE mine)
{
    HANDLE theirs = NULL;
    if (!mine)
        return 0;
    if (!DuplicateHandle(GetCurrentProcess(), mine, g_up.proc, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS))
        theirs = NULL;
    CloseHandle(mine);
    return (uint64_t)(uintptr_t)theirs;
}

static uint64_t up_share(ID3D11Texture2D* t)
{
    IDXGIResource1* r = NULL;
    HANDLE h = NULL;
    if (FAILED(ID3D11Texture2D_QueryInterface(t, &IID_IDXGIResource1, (void**)&r)))
        return 0;
    if (FAILED(IDXGIResource1_CreateSharedHandle(r, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h)))
        h = NULL;
    IDXGIResource1_Release(r);
    return up_pass_on(h);
}

/* the helper started for FSR, with the shared memory, events and fence it opens by name and handle */
static int up_start(void)
{
    wchar_t base[96];
    swprintf(base, 96, L"%ls%lu", VANA_DLSS_MAP, (unsigned long)GetCurrentProcessId());
    g_up.map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(VanaDlssShared), base);
    g_up.sh = g_up.map ? (VanaDlssShared*)MapViewOfFile(g_up.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanaDlssShared)) : NULL;
    wchar_t name[128];
    swprintf(name, 128, L"%ls.go", base);
    g_up.go = CreateEventW(NULL, FALSE, FALSE, name);
    swprintf(name, 128, L"%ls.ready", base);
    g_up.ready = CreateEventW(NULL, FALSE, FALSE, name);
    swprintf(name, 128, L"%ls.fg", base);
    g_up.go_fg = CreateEventW(NULL, FALSE, FALSE, name);
    g_up.cpu_sync = gfx_dlss_cpu_sync_wanted();
    swprintf(name, 128, L"%ls.done", base);
    g_up.done = CreateEventW(NULL, FALSE, FALSE, name);
    swprintf(name, 128, L"%ls.fgdone", base);
    g_up.done_fg = CreateEventW(NULL, FALSE, FALSE, name);
    g_up.cpu_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_up.dev5 && FAILED(ID3D11Device_QueryInterface(g_dev, &IID_ID3D11Device5, (void**)&g_up.dev5)))
        g_up.dev5 = NULL;
    if (!g_up.ctx4 && FAILED(ID3D11DeviceContext_QueryInterface(g_ctx, &IID_ID3D11DeviceContext4, (void**)&g_up.ctx4)))
        g_up.ctx4 = NULL;
    if (!g_up.dev5 || !g_up.ctx4)
    {
        up_say(3, "it needs Direct3D 11.4 (Windows 10 1703 or later) to share a fence with the helper; off");
        return 0;
    }
    if (!g_up.sh || !g_up.go || !g_up.ready || !g_up.go_fg || !g_up.done || !g_up.done_fg || !g_up.cpu_ev ||
        FAILED(ID3D11Device5_CreateFence(g_up.dev5, 0, g_up.cpu_sync ? D3D11_FENCE_FLAG_NONE : D3D11_FENCE_FLAG_SHARED,
            &IID_ID3D11Fence, (void**)&g_up.fence)))
    {
        up_say(4, "couldn't set up what the helper shares");
        return 0;
    }
    memset(g_up.sh, 0, sizeof *g_up.sh);
    g_up.sh->version = VANA_DLSS_VERSION;
    g_up.sh->cpu_sync = g_up.cpu_sync;
    g_up.sh->game_pid = GetCurrentProcessId();
    g_up.sh->upscaler = VANA_UPSCALER_FSR;
    /* the helper's device on this one's graphics card */
    IDXGIDevice* dxgi = NULL;
    IDXGIAdapter* adapter = NULL;
    DXGI_ADAPTER_DESC ad;
    memset(&ad, 0, sizeof ad);
    if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void**)&dxgi)))
    {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)))
            IDXGIAdapter_GetDesc(adapter, &ad), IDXGIAdapter_Release(adapter);
        IDXGIDevice_Release(dxgi);
    }
    g_up.sh->luid_low = ad.AdapterLuid.LowPart, g_up.sh->luid_high = ad.AdapterLuid.HighPart;
    wchar_t cmd[3 * MAX_PATH];
    swprintf(cmd, 3 * MAX_PATH, L"\"%ls\" %lu \"%ls\"", g_up.helper, (unsigned long)GetCurrentProcessId(), g_up.log);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si), si.cb = sizeof si;
    if (!CreateProcessW(g_up.helper, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi))
    {
        up_say(4, "VanaCore.Dlss.exe didn't start (is it in the dlss folder?)");
        return 0;
    }
    CloseHandle(pi.hThread);
    g_up.proc = pi.hProcess;
    fprintf(stderr, "[recomp] gfx: FSR (Direct3D 11): the helper started (process %lu)%s\n", (unsigned long)pi.dwProcessId,
        g_up.cpu_sync ? "; no fence shared with it (Wine or Proton can't open one from another process), its frames waited for on the CPU" : "");
    return 1;
}

static void up_release(void)
{
    for (int i = 0; i < 3; ++i)
        if (g_up.rtv[i])
            ID3D11RenderTargetView_Release(g_up.rtv[i]), g_up.rtv[i] = NULL;
    if (g_up.srv_out)
        ID3D11ShaderResourceView_Release(g_up.srv_out), g_up.srv_out = NULL;
    for (int i = 0; i < 4; ++i)
        if (g_up.res[i])
            ID3D11Texture2D_Release(g_up.res[i]), g_up.res[i] = NULL;
    if (g_up.out)
        gfx_tex_destroy(g_up.out), g_up.out = NULL;
    if (g_up.zup)
        gfx_tex_destroy(g_up.zup), g_up.zup = NULL;
    fx_tex_free(&g_up.mid);
    g_up.fsr1 = 0;
}

/* The helper and everything shared with it let go (it ended): another is started at the next frame for FSR */
static void up_let_go(void)
{
    if (g_up.sh && g_up.proc && WaitForSingleObject(g_up.proc, 0) != WAIT_OBJECT_0)
    {
        g_up.sh->quit = 1;
        SetEvent(g_up.go);
        if (WaitForSingleObject(g_up.proc, 1000) != WAIT_OBJECT_0)
            TerminateProcess(g_up.proc, 0);
    }
    gfx_finish(); /* nothing of this device's in flight on what's shared */
    up_release();
    if (g_up.sh)
        UnmapViewOfFile(g_up.sh), g_up.sh = NULL;
    HANDLE* handles[] = { &g_up.map, &g_up.go, &g_up.ready, &g_up.go_fg, &g_up.done, &g_up.done_fg, &g_up.cpu_ev, &g_up.proc };
    for (size_t i = 0; i < sizeof handles / sizeof handles[0]; ++i)
        if (*handles[i])
            CloseHandle(*handles[i]), *handles[i] = NULL;
    if (g_up.fence)
        ID3D11Fence_Release(g_up.fence), g_up.fence = NULL;
    g_up.value = 0, g_up.in_w = g_up.in_h = g_up.out_w = g_up.out_h = 0, g_up.ran_ns = 0;
}

/* a shared texture of this device's (the helper opens it by its NT handle) */
static ID3D11Texture2D* up_texture(DXGI_FORMAT fmt, uint32_t w, uint32_t h, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, fmt, { 1, 0 }, D3D11_USAGE_DEFAULT, bind, 0,
        D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
    ID3D11Texture2D* t = NULL;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t)) ? t : NULL;
}

/* the shared inputs and output for these sizes, `out` (the scene target's shape, at the output's scale), and FSR made
 * for them by the helper (generation) */
static int up_targets(GfxTex* color, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h)
{
    up_release();
    const DXGI_FORMAT cfmt = color->dxfmt == DXGI_FORMAT_B8G8R8X8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM : color->dxfmt;
    g_up.res[0] = up_texture(cfmt, in_w, in_h, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    g_up.res[1] = up_texture(DXGI_FORMAT_R32_FLOAT, in_w, in_h, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    g_up.res[2] = up_texture(DXGI_FORMAT_R16G16_FLOAT, in_w, in_h, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    g_up.res[3] = up_texture(DXGI_FORMAT_R8G8B8A8_UNORM, out_w, out_h, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    for (int i = 0; i < 4; ++i)
        if (!g_up.res[i])
            return 0;
    for (int i = 0; i < 3; ++i)
        if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)g_up.res[i], NULL, &g_up.rtv[i])))
            return g_up.rtv[i] = NULL, 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_up.res[3], NULL, &g_up.srv_out)))
        return g_up.srv_out = NULL, 0;
    uint32_t ow = (uint32_t)((double)color->w * out_w / in_w + 0.5), oh = (uint32_t)((double)color->h * out_h / in_h + 0.5);
    g_up.out = gfx_tex_create(GFX_TEX_2D, color->fmt, ow < out_w ? out_w : ow, oh < out_h ? out_h : oh, 1, GFX_USE_RT);
    if (!g_up.out)
        return 0;
    VanaDlssShared* sh = g_up.sh;
    sh->color = up_share(g_up.res[0]);
    sh->depth = up_share(g_up.res[1]);
    sh->motion = up_share(g_up.res[2]);
    sh->output = up_share(g_up.res[3]);
    {
        HANDLE h = NULL;
        if (g_up.cpu_sync || FAILED(ID3D11Fence_CreateSharedHandle(g_up.fence, NULL, GENERIC_ALL, NULL, &h)))
            h = NULL;
        sh->fence = up_pass_on(h);
    }
    if (!sh->color || !sh->depth || !sh->motion || !sh->output || (!sh->fence && !g_up.cpu_sync))
        return 0;
    sh->in_w = in_w, sh->in_h = in_h, sh->out_w = out_w, sh->out_h = out_h;
    sh->color_format = (uint32_t)cfmt, sh->output_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sh->quality = g_up.quality, sh->depth_inverted = 0, sh->nr = 0;
    {
        /* the depth with no far plane where the projection has none (FFXI's: 1 - near / w), or one too far to count */
        const float* p = g_up.pend_s.proj;
        const float hand = p[11] < 0.0f ? -1.0f : 1.0f;
        const float zn = fabsf(p[10]) > 1e-6f ? fabsf(p[14] / p[10]) : 0.1f;
        const float zf = fabsf(p[10] - hand) > 1e-6f ? fabsf(p[14] / (p[10] - hand)) : 0.0f;
        sh->depth_infinite = zf <= 0.0f || zf > zn * 1.0e6f;
    }
    sh->sr_version_want = g_up.version_want;
    sh->generation++;
    g_up.in_w = in_w, g_up.in_h = in_h, g_up.out_w = out_w, g_up.out_h = out_h;
    g_up.cfmt = color->dxfmt, g_up.made_quality = g_up.quality, g_up.made_version = g_up.version_want;
    g_up.reset = 1;
    fprintf(stderr, "[recomp] gfx: FSR (Direct3D 11): the scene drawn at %ux%u, shown at %ux%u\n", in_w, in_h, out_w, out_h);
    return 1;
}

/* the helper's state: ready (1), not yet (0), or gone for good (-1, failed set) */
static int up_helper_ready(void)
{
    if (g_up.failed)
        return -1;
    if (!g_up.sh && !up_dlls_here())
    {
        up_say(3, "AMD's FSR DLLs aren't in the dlss folder; off");
        return g_up.failed = 1, -1;
    }
    if (!g_up.sh && !up_start())
        return g_up.failed = 1, -1;
    if (WaitForSingleObject(g_up.proc, 0) == WAIT_OBJECT_0)
    {
        up_say(4, "the helper ended; off");
        return g_up.failed = 1, -1;
    }
    int32_t st = g_up.sh->state;
    if (st == VANA_DLSS_STARTING)
        return 0;
    if (st != VANA_DLSS_READY)
    {
        char why[300];
        snprintf(why, sizeof why, "%s; off", g_up.sh->message);
        up_say(st == VANA_DLSS_UNAVAILABLE ? 3 : 4, why);
        return g_up.failed = 1, -1;
    }
    up_say(2, "ready");
    return 1;
}

/* The helper's `done` at `want` within ms (it records and submits a frame's work in well under a millisecond) */
static int up_caught_up(uint64_t want, int ms)
{
    if (g_up.cpu_sync)
    {
        /* (the helper's `done` comes once its work is through: its event, not a spin) */
        const ULONGLONG limit = GetTickCount64() + (ULONGLONG)ms;
        while (g_up.sh->done < want)
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= limit)
                return 0;
            WaitForSingleObject(g_up.done, (DWORD)(limit - now));
        }
        return 1;
    }
    const uint64_t until = gfx_now_ns() + (uint64_t)ms * 1000000u;
    for (int spins = 0; g_up.sh->done < want; ++spins)
    {
        if (gfx_now_ns() >= until)
            return 0;
        if (spins < 2000)
            YieldProcessor();
        else
            Sleep(spins < 2100 ? 0 : 1);
    }
    return 1;
}

/* FSR 1 in AMD's FSR's place (up_fsr1), as gfx_d3d12.c's fsr1_run: the scene noted at gfx_scene_done, all of it drawn
 * now, made larger into `out` here - EASU (fx_easu), then RCAS (fx_rcas) with [graphics] fsr.sharpness above 0, through
 * `mid` - for this frame's draws to read in its place. No helper, depth, motion or jitter: one frame alone, so softer than
 * AMD's FSR 3.1 or 4. At native it only sharpens (RCAS at 0 is a copy). */
static void up_fsr1_run(void)
{
    GfxTex* color = g_up.pend_color;
    const GfxScene* s = &g_up.pend_s;
    if (g_up.sh)
        up_let_go(); /* AMD's FSR's helper (FSR 1 asked for since, or it gave up): not wanted now */
    if (!g_up.fsr1_said)
    {
        g_up.fsr1_said = 1;
        if (g_up.version_want == 1)
            fprintf(stderr, "[recomp] gfx: FSR (Direct3D 11): FSR 1 ([graphics] fsr.version=1)\n");
        else
            fprintf(stderr, "[recomp] gfx: FSR (Direct3D 11): FSR 1 in AMD's FSR's place (%s)\n", g_up.why[0] ? g_up.why : "it isn't there");
    }
    if (!g_up.ps_easu || !g_up.ps_rcas)
    {
        up_say(4, "FSR 1's passes didn't compile; off");
        g_up.fsr1_failed = 1;
        return;
    }
    float vp[4];
    uint32_t in_w, in_h, out_w, out_h;
    if (!gfx_fsr1_sizes(s, color->w, color->h, up_factor(g_up.quality), vp, &in_w, &in_h, &out_w, &out_h))
        return; /* not a scene to make larger this frame (gfx_shared.h) */
    const float vx = vp[0], vy = vp[1];
    if (!g_up.fsr1 || !g_up.out || in_w != g_up.in_w || in_h != g_up.in_h || out_w != g_up.out_w || out_h != g_up.out_h ||
        g_up.out->fmt != color->fmt)
    {
        up_release();
        /* `out`: the scene target's shape, at the output's scale */
        const uint32_t ow = (uint32_t)((double)color->w * out_w / in_w + 0.5), oh = (uint32_t)((double)color->h * out_h / in_h + 0.5);
        g_up.out = gfx_tex_create(GFX_TEX_2D, color->fmt, ow < out_w ? out_w : ow, oh < out_h ? out_h : oh, 1, GFX_USE_RT);
        if (!g_up.out)
        {
            up_say(4, "FSR 1 couldn't make its textures; off");
            g_up.fsr1_failed = 1;
            return;
        }
        g_up.in_w = in_w, g_up.in_h = in_h, g_up.out_w = out_w, g_up.out_h = out_h;
        g_up.cfmt = color->dxfmt, g_up.made_quality = -3; /* not the helper's: made again for it */
        g_up.fsr1 = 1;
        fprintf(stderr, "[recomp] gfx: FSR 1 (Direct3D 11): the scene drawn at %ux%u, shown at %ux%u\n", in_w, in_h, out_w, out_h);
    }
    const float kx = (float)out_w / in_w, ky = (float)out_h / in_h;
    const float ovp[4] = { vx * kx, vy * ky, (float)out_w, (float)out_h }, whole[4] = { 0, 0, (float)out_w, (float)out_h };
    const int native = in_w == out_w && in_h == out_h, sharp = native || g_up.sharpness > 0.0f;
    FxU u;
    memset(&u, 0, sizeof u);
    u.vp[0] = vx, u.vp[1] = vy, u.vp[2] = (float)in_w, u.vp[3] = (float)in_h;
    ID3D11ShaderResourceView* src = color->srv;
    if (!native && sharp)
    {
        /* EASU into `mid`, for RCAS to read */
        if (!fx_tex(&g_up.mid, g_up.out->dxfmt, out_w, out_h))
        {
            up_say(4, "FSR 1 couldn't make its textures; off");
            g_up.fsr1_failed = 1;
            return;
        }
        memcpy(u.size, whole, sizeof whole);
        fx_draw(g_up.mid.rtv, g_up.ps_easu, whole, &u, &src, 1, 0, 0);
        memcpy(u.vp, whole, sizeof whole);
        src = g_up.mid.srv;
    }
    /* the last onto `out`, where the viewport's part of the scene is: RCAS (its strength fsr.sharpness 0 to 1 as AMD's FSR
     * 2 and 3 take it, from 2 stops below the most to the most), or EASU alone */
    memcpy(u.size, ovp, sizeof ovp);
    u.clar[0] = g_up.sharpness > 0.0f ? exp2f(-2.0f * (1.0f - g_up.sharpness)) : 0.0f;
    fx_draw((ID3D11RenderTargetView*)target_view(g_up.out, 0, 0), sharp ? g_up.ps_rcas : g_up.ps_easu, ovp, &u, &src, 1, 0, 0);
    fx_unbind();
    g_up.scene = color, g_up.out_serial = g_serial;
}

/* The scene noted at gfx_scene_done, all of it drawn now (a draw reads it): through FSR into `out`, for this frame's
 * draws to read in its place */
static void up_run(void)
{
    GfxTex* color = g_up.pend_color;
    GfxTex* depth = g_up.pend_depth;
    const GfxScene* s = &g_up.pend_s;
    if (g_up.quality < 0 || up_gave_up() || !color || !color->srv || color->type != GFX_TEX_2D)
        return;
    if (InterlockedCompareExchange(&g_fx.state, 0, 0) != 2)
        return; /* the passes still building (with the effects') */
    if (up_fsr1())
    {
        up_fsr1_run();
        return;
    }
    if (!depth || !depth->srv || !g_up.ps_z || !g_up.ps_mv || !g_up.ps_color || !g_up.ps_copy)
        return;
    if (up_helper_ready() != 1)
        return;
    VanaDlssShared* sh = g_up.sh;
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > color->w || vy + vh > color->h)
        vx = vy = 0, vw = (float)color->w, vh = (float)color->h;
    uint32_t in_w = (uint32_t)vw, in_h = (uint32_t)vh, out_w, out_h;
    if (s->present_w && s->present_h)
        out_w = (uint32_t)(vw * s->present_w / color->w + 0.5f), out_h = (uint32_t)(vh * s->present_h / color->h + 0.5f);
    else
        out_w = (uint32_t)(vw / up_factor(g_up.quality) + 0.5f), out_h = (uint32_t)(vh / up_factor(g_up.quality) + 0.5f);
    if (out_w < in_w || out_h < in_h)
        return; /* a scene larger than it's shown (the game's 4096 glow picture, a frame mid-resize): as it is */
    if (in_w >= out_w && up_factor(g_up.quality) < 0.99f)
        return; /* drawn at its full size by a mode that draws it smaller: the frame FSR came on in */
    if (in_w != g_up.in_w || in_h != g_up.in_h || out_w != g_up.out_w || out_h != g_up.out_h || color->dxfmt != g_up.cfmt ||
        g_up.quality != g_up.made_quality || g_up.version_want != g_up.made_version || !g_up.out)
    {
        if (!up_targets(color, in_w, in_h, out_w, out_h))
        {
            up_say(4, "couldn't make its textures; off");
            g_up.failed = 1;
            up_release();
            return;
        }
        /* the helper makes FSR for them: the scene as it is till it has */
        SetEvent(g_up.go);
        g_up.made_at = GetTickCount64();
        return;
    }
    if (sh->made != sh->generation)
    {
        if (GetTickCount64() - g_up.made_at > 30000)
        {
            up_say(4, "the helper didn't make it in 30 seconds; off");
            g_up.failed = 1;
        }
        return;
    }
    /* the camera from the frame before (the motion): its view and projection, if it was the frame just before and not a
     * jump away (a new zone, a cut) */
    float vinv[16];
    if (!mat_inverse(vinv, s->view))
        return;
    FxU u;
    memset(&u, 0, sizeof u);
    u.proj[0] = s->proj[0], u.proj[1] = s->proj[5], u.proj[2] = s->proj[8], u.proj[3] = s->proj[9];
    float minz, maxz;
    memcpy(&minz, &s->vp[4], 4);
    memcpy(&maxz, &s->vp[5], 4);
    if (maxz <= minz)
        minz = 0, maxz = 1;
    u.zp[0] = s->proj[10], u.zp[1] = s->proj[14], u.zp[2] = minz, u.zp[3] = maxz;
    u.vp[0] = vx, u.vp[1] = vy, u.vp[2] = vw, u.vp[3] = vh;
    u.size[0] = (float)color->w, u.size[1] = (float)color->h;
    u.hand[0] = s->proj[11] < 0.0f ? -1.0f : 1.0f;
    int reset = g_up.reset || g_up.prev_serial + 1 != g_serial;
    if (!reset)
    {
        float dx = vinv[12] - g_up.prev_cam[0], dy = vinv[13] - g_up.prev_cam[1], dz = vinv[14] - g_up.prev_cam[2];
        reset = !(dx * dx + dy * dy + dz * dz <= 25.0f); /* (a camera that's no number: made again too) */
    }
    float m[16];
    mat_mul(m, vinv, reset ? s->view : g_up.prev_view);
    mat_mul(u.reproj, m, reset ? s->proj : g_up.prev_proj);
    memcpy(g_up.prev_view, s->view, 64), memcpy(g_up.prev_proj, s->proj, 64), memcpy(g_up.prev_cam, vinv + 12, 12);
    g_up.prev_serial = g_serial;
    /* the inputs: the viewport's part of the scene, its depth, its motion */
    const float vp0[4] = { 0, 0, vw, vh };
    fx_draw(g_up.rtv[0], g_up.ps_color, vp0, &u, &color->srv, 1, 0, 0);
    fx_draw(g_up.rtv[1], g_up.ps_z, vp0, NULL, &depth->srv, 1, 0, 0);
    fx_draw(g_up.rtv[2], g_up.ps_mv, vp0, NULL, &depth->srv, 1, 0, 0);
    fx_unbind();
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL); /* the shared inputs no longer this device's targets */
    /* handed over: the fence at `frame` once the inputs are drawn (sent now: the helper's queue waits on it), the
     * helper's at frame + 1 */
    const uint64_t frame = g_up.value + 1;
    g_up.value = frame + 1;
    ID3D11DeviceContext4_Signal(g_up.ctx4, g_up.fence, frame);
    ID3D11DeviceContext_Flush(g_ctx);
    sh->render_w = in_w, sh->render_h = in_h;
    sh->jitter_x = g_up.jit_serial == g_serial ? g_up.jit[0] : 0.0f;
    sh->jitter_y = g_up.jit_serial == g_serial ? g_up.jit[1] : 0.0f;
    sh->motion_scale_x = sh->motion_scale_y = 1.0f;
    sh->reset = reset, sh->sharpness = g_up.sharpness, sh->nr = 0;
    {
        /* FSR's: the camera's planes (far 0: none) and field of view, and the time since the frame before it had (ms) */
        const float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f;
        const float a = s->proj[10], b = s->proj[14];
        sh->sr_near = fabsf(a) > 1e-6f ? fabsf(b / a) : 0.1f;
        sh->sr_far = fabsf(a - hand) > 1e-6f ? fabsf(b / (a - hand)) : 0.0f;
        sh->sr_fov = 2.0f * atanf(1.0f / (fabsf(s->proj[5]) > 1e-6f ? fabsf(s->proj[5]) : 1.0f));
        const uint64_t now = gfx_now_ns();
        const double ms = g_up.ran_ns ? (double)(now - g_up.ran_ns) / 1e6 : 16.7;
        sh->sr_frame_ms = (float)(ms < 1.0 ? 1.0 : ms > 100.0 ? 100.0 : ms);
        g_up.ran_ns = now;
    }
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        sh->posted_qpc = q.QuadPart;
    }
    MemoryBarrier();
    sh->frame = frame;
    if (g_up.cpu_sync)
    {
        /* no fence shared with the helper: this thread waits for the context's signal, then lets the helper in (2 s at most:
         * the helper gone, or the card stuck) */
        const ULONGLONG until = GetTickCount64() + 2000;
        if (ID3D11Fence_GetCompletedValue(g_up.fence) < frame &&
            SUCCEEDED(ID3D11Fence_SetEventOnCompletion(g_up.fence, frame, g_up.cpu_ev)))
            while (ID3D11Fence_GetCompletedValue(g_up.fence) < frame)
            {
                WaitForSingleObject(g_up.cpu_ev, 20);
                if (GetTickCount64() >= until || WaitForSingleObject(g_up.proc, 0) == WAIT_OBJECT_0)
                    break;
            }
    }
    SetEvent(g_up.go);
    g_up.reset = 0;
    /* the context waits on the helper's output only once its work is on its queue: a helper that stopped can't leave this
     * device waiting (Direct3D 11 can't signal the fence itself). Without a shared fence the helper's work is through
     * when it says so, and there's nothing for the context to wait on */
    if (!up_caught_up(frame, g_up.cpu_sync ? 2000 : 200))
    {
        up_say(4, "the helper stopped answering; off");
        g_up.failed = 1;
        return;
    }
    if (!g_up.cpu_sync)
        ID3D11DeviceContext4_Wait(g_up.ctx4, g_up.fence, frame + 1);
    /* the output into `out`, where the viewport's part of the scene is, at the output's scale */
    float kx = (float)out_w / in_w, ky = (float)out_h / in_h;
    const float ovp[4] = { vx * kx, vy * ky, (float)out_w, (float)out_h };
    fx_draw((ID3D11RenderTargetView*)target_view(g_up.out, 0, 0), g_up.ps_copy, ovp, &u, &g_up.srv_out, 1, 0, 0);
    fx_unbind();
    g_up.scene = color, g_up.out_serial = g_serial;
}

/* The interface drawn into the scene before the game is done with it (names, floating text - the game may draw more of
 * the world after them, a glare or a spell, which FSR must have): held, with copies of its vertices and indices, and
 * drawn over FSR's output once it has run (up_replay); drawn into the scene as the game drew it if it didn't. A draw
 * from a buffer the game may write again before then isn't held (FSR runs at it instead). */
typedef struct
{
    GfxDraw d;
    GfxTex *rt, *ds;
    uint32_t face, level;
    size_t data[GFX_NSTREAMS], indices; /* offsets + 1 into g_up_mem (0: none) */
} UpHeld;
static UpHeld* g_up_held;
static uint32_t g_up_nheld, g_up_cap_held;
static uint8_t* g_up_mem;
static size_t g_up_len, g_up_cap;

/* a copy of n bytes at p: its offset + 1 (0 for none, (size_t)-1 when there's no memory for it) */
static size_t up_copy_bytes(const void* p, size_t n)
{
    if (!p || !n)
        return 0;
    if (g_up_len + n + 16 > g_up_cap)
    {
        size_t cap = (g_up_len + n + 16) * 2 + 65536;
        uint8_t* m = (uint8_t*)realloc(g_up_mem, cap);
        if (!m)
            return (size_t)-1;
        g_up_mem = m, g_up_cap = cap;
    }
    size_t at = g_up_len;
    memcpy(g_up_mem + at, p, n);
    g_up_len += (n + 15) & ~(size_t)15;
    return at + 1;
}

static int up_hold(const GfxDraw* d)
{
    if (d->ibuf || d->caster)
        return 0;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (d->buf[s])
            return 0;
    if (g_up_nheld == g_up_cap_held)
    {
        uint32_t cap = g_up_cap_held ? g_up_cap_held * 2 : 64;
        UpHeld* more = (UpHeld*)realloc(g_up_held, cap * sizeof *more);
        if (!more)
            return 0;
        g_up_held = more, g_up_cap_held = cap;
    }
    UpHeld* h = &g_up_held[g_up_nheld];
    const size_t len = g_up_len;
    h->d = *d;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        h->data[s] = up_copy_bytes(d->data[s], d->size[s]);
        h->d.cpu[s] = NULL, h->d.cpu_size[s] = 0;
    }
    const uint32_t n = d->prim == GFX_TRIANGLEFAN ? d->count + 2 : vertex_count(d->prim, d->count);
    h->indices = up_copy_bytes(d->indices, (size_t)n * d->index_size);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (h->data[s] == (size_t)-1 || h->indices == (size_t)-1)
            return g_up_len = len, 0;
    h->rt = g_rt, h->ds = g_ds, h->face = g_rt_face, h->level = g_rt_level;
    g_up_nheld++;
    return 1;
}

/* After the effects: the scene noted, for FSR once the game is done drawing into it */
static void up_frame(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    if (!s->present_w || s->present_w >= color->w)
        g_up.noted = g_serial; /* not the game's 4096 glow picture, larger than it's shown (up_run leaves it) */
    g_up.pend_color = color, g_up.pend_depth = depth;
    memcpy(&g_up.pend_s, s, sizeof *s);
    g_up.pend_serial = g_serial;
    g_up.replay_depth = NULL;
    g_up_nheld = 0, g_up_len = 0; /* a frame before's, its scene never read: gone with it */
}

static void draw_main(const GfxDraw* d);

/* The scene's depth at the output's size (the render pixel under each output pixel), so the interface drawn over FSR's
 * output - the names over heads - is hidden behind what's in front of it as it is without FSR. NULL when it can't be
 * made (the interface is then drawn over everything). */
static GfxTex* up_depth_up(void)
{
    GfxTex *depth = g_up.pend_depth, *scene = g_up.scene, *out = g_up.out;
    if (!depth || !scene || !out || depth->use != GFX_USE_DEPTH || !depth->srv || !g_up.ps_zup || !g_fx.vs)
        return NULL;
    if (!g_up.zup || g_up.zup->w != out->w || g_up.zup->h != out->h || g_up.zup->dxfmt != depth->dxfmt)
    {
        gfx_tex_destroy(g_up.zup);
        g_up.zup = gfx_tex_create(GFX_TEX_2D, depth->fmt, out->w, out->h, 1, GFX_USE_DEPTH);
    }
    if (!g_up.zup)
        return NULL;
    if (!g_up.zwrite)
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_up.zwrite)))
            return g_up.zwrite = NULL, NULL;
    }
    ID3D11DepthStencilView* dsv = (ID3D11DepthStencilView*)target_view(g_up.zup, 0, 0);
    if (!dsv)
        return NULL;
    FxU u;
    memset(&u, 0, sizeof u);
    u.size[0] = (float)out->w, u.size[1] = (float)out->h;
    u.vp[2] = (float)scene->w, u.vp[3] = (float)scene->h;
    if (g_up.jit_serial == g_serial) /* where this frame's jitter moved the scene (fx_depthup) */
        u.vp[0] = g_up.jit[0], u.vp[1] = g_up.jit[1];
    ID3D11ShaderResourceView* srv[8] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 8, srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, dsv);
    srv[0] = depth->srv;
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, srv);
    fx_constants(&u);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_fx.cb);
    D3D11_VIEWPORT v = { 0, 0, (float)out->w, (float)out->h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &v);
    ID3D11DeviceContext_RSSetState(g_ctx, g_raster_plain);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_up.zwrite, 0);
    static const float factor[4] = { 1, 1, 1, 1 };
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend_none, factor, 0xFFFFFFFFu);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_fx.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_up.ps_zup, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_linear);
    ID3D11DeviceContext_Draw(g_ctx, 3, 0);
    fx_unbind();
    return g_up.zup;
}

/* An interface draw into the scene's target after FSR ran: onto its output instead, its viewport at the output's
 * scale, tested against the scene's depth at the output's size (up_depth_up) rather than the scene's own (smaller) */
static void up_draw_over(const GfxDraw* d)
{
    static GfxDraw over;
    over = *d;
    GfxTex* scene = g_up.scene;
    double kx = (double)g_up.out->w / scene->w, ky = (double)g_up.out->h / scene->h;
    over.vp[0] = (uint32_t)(over.vp[0] * kx + 0.5), over.vp[1] = (uint32_t)(over.vp[1] * ky + 0.5);
    over.vp[2] = (uint32_t)(over.vp[2] * kx + 0.5), over.vp[3] = (uint32_t)(over.vp[3] * ky + 0.5);
    GfxTex* zup = g_up.replay_depth;
    if (!zup)
        over.depth.zenable = 0;
    over.depth.zwrite = 0;
    GfxTex *rt = g_rt, *ds = g_ds;
    uint32_t face = g_rt_face, level = g_rt_level;
    g_rt = g_up.out, g_ds = zup, g_rt_face = g_rt_level = 0, g_targets_bound = 0;
    GfxTex* seen = g_up.out->depth_seen;
    draw_main(&over);
    g_up.out->depth_seen = seen; /* the depth bound for this was for it: not what the scene's effects read */
    g_rt = rt, g_ds = ds, g_rt_face = face, g_rt_level = level, g_targets_bound = 0;
}

static void up_replay(void)
{
    for (uint32_t i = 0; i < g_up_nheld; ++i)
    {
        UpHeld* h = &g_up_held[i];
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            h->d.data[s] = h->data[s] ? g_up_mem + h->data[s] - 1 : NULL;
        h->d.indices = h->indices ? g_up_mem + h->indices - 1 : NULL;
        if (g_up.out_serial == g_serial && h->rt == g_up.scene)
            up_draw_over(&h->d);
        else
        {
            GfxTex *rt = g_rt, *ds = g_ds;
            uint32_t face = g_rt_face, level = g_rt_level;
            g_rt = h->rt, g_ds = h->ds, g_rt_face = h->face, g_rt_level = h->level, g_targets_bound = 0;
            draw_main(&h->d);
            g_rt = rt, g_ds = ds, g_rt_face = face, g_rt_level = level, g_targets_bound = 0;
        }
    }
    g_up_nheld = 0, g_up_len = 0;
}

#define SHOT_UP g_up
#include "gfx_scene_shot.h" /* gfx_scene_shot: the scene before the interface, for a screenshot without it */

/* Before each draw: the noted scene through FSR once it's all drawn - at the first draw reading it (the composite onto
 * the back buffer); the interface drawn into it before then held, and drawn over FSR's output (at its size) after. 1
 * when up_draw_over draws this draw, 2 when it's held, 0 to draw it as usual. A 3D draw into the scene's target after
 * FSR ran ends its output's use (the scene read as it is from then). */
static int up_on_draw(const GfxDraw* d)
{
    shot_on_draw(d);
    if (g_up.pend_serial == g_serial && g_up.pend_color)
    {
        if (g_rt == g_up.pend_color && d->overlay && up_hold(d))
            return 2;
        int reads = 0;
        for (int i = 0; i < 8; ++i)
            reads |= d->tex[i] == g_up.pend_color;
        /* a read into a small target isn't the scene laid onto the frame (a distortion effect's grab of it, the sun
         * flare's probe): FSR waits for the composite */
        if (reads && g_rt && (uint64_t)g_rt->w * g_rt->h * 2 < (uint64_t)g_up.pend_s.present_w * g_up.pend_s.present_h)
            reads = 0;
        if (g_rt == g_up.pend_color ? d->overlay : reads)
        {
            g_up.pend_serial = 0;
            up_run();
            shot_after_run();
            g_up.replay_depth = g_up.out_serial == g_serial ? up_depth_up() : NULL;
            up_replay();
        }
    }
    if (g_up.out_serial != g_serial || !g_rt || g_rt != g_up.scene)
        return 0;
    if (d->overlay)
        return 1;
    g_up.out_serial = 0;
    return 0;
}

/* the scene's target, read by this frame's draws once FSR has made it larger: its output */
static GfxTex* up_sampled(GfxTex* t)
{
    return t && t == g_up.scene && g_up.out_serial == g_serial && g_up.out ? g_up.out : t;
}

/* The mip bias, in sixteenths, for the world's textures drawn into the scene FSR makes larger: log2 of the size it's
 * drawn at over the size it's shown at, so they're as sharp as at the shown size. 0 without FSR, at native, and on other
 * targets. */
static int8_t up_lod_bias(void)
{
    if (g_up.quality < 0 || up_gave_up() || !g_up.in_w || !g_up.out_w || !g_rt || g_rt != g_up.scene)
        return 0;
    float b = log2f((float)g_up.in_w / (float)g_up.out_w) * 16.0f;
    return (int8_t)(b > 0.0f ? 0 : b < -32.0f ? -32 : (int)(b - 0.5f));
}

void gfx_dlss_set(int quality, const wchar_t* helper, const wchar_t* log)
{
    if (!g_up.fsr)
        quality = -1;
    if (quality != g_up.quality)
        fprintf(stderr, quality < 0 ? "[recomp] gfx: FSR (Direct3D 11) off\n" : "[recomp] gfx: FSR (Direct3D 11) on, quality %d\n",
            quality);
    const int was = g_up.quality;
    g_up.quality = quality < 0 || quality > 5 ? -1 : quality;
    /* turned on again after it gave up, or at a new quality: tried again, with the helper as it is, or another if it
     * ended - but for AMD's FSR that isn't here (3: its DLLs, this card, no Direct3D 11.4), FSR 1 standing in for it, which
     * goes on at the new one */
    if (g_up.quality >= 0 && g_up.quality != was && (g_up.failed || g_up.fsr1_failed) && !(was >= 0 && g_up.said == 3 && up_fsr1()))
    {
        g_up.failed = 0, g_up.said = 0, g_up.why[0] = 0, g_up.fsr1_failed = 0, g_up.fsr1_said = 0;
        if (g_up.proc && WaitForSingleObject(g_up.proc, 0) == WAIT_OBJECT_0)
            up_let_go();
        fprintf(stderr, "[recomp] gfx: FSR (Direct3D 11): trying again\n");
    }
    if (helper)
        wcsncpy(g_up.helper, helper, MAX_PATH - 1);
    if (log)
        wcsncpy(g_up.log, log, MAX_PATH - 1);
}

/* gfx_d3d12.c's vanacore_gfx_set_upscaler's, for this back end: whether it's FSR (DLSS is Direct3D 12's: nothing here),
 * FSR's sharpening and version */
void gfx11_upscaler(int upscaler, float sharpness, int version)
{
    g_up.fsr = upscaler == VANA_UPSCALER_FSR;
    g_up.sharpness = sharpness < 0.0f ? 0.0f : sharpness > 1.0f ? 1.0f : sharpness;
    version = version >= 1 && version <= 4 ? version : 0; /* 1: FSR 1 (up_fsr1_run) */
    if (version != g_up.version_want)
        g_up.fsr1_said = 0;
    g_up.version_want = version;
}

/* gfx_d3d12.c's vanacore_gfx_fsr_supported, for this back end */
int gfx11_fsr_supported(char* why, int size)
{
    const VanaDlssShared* sh = g_up.sh;
    /* FSR 1 runs where AMD's FSR doesn't (up_fsr1): only without its passes is it AMD's FSR or nothing */
    const LONG fx = InterlockedCompareExchange(&g_fx.state, 0, 0);
    const int fsr1 = fx != 3 && (fx != 2 || (g_up.ps_easu && g_up.ps_rcas));
    const char* text = !g_dev                 ? "needs DirectX 11 or 12"
                       : fsr1                   ? ""
                       : !sh && !up_dlls_here() ? "needs AMD's FSR DLLs in the dlss folder"
                       : g_up.failed && g_up.said == 3 && g_up.why[0] ? g_up.why
                       : sh && sh->state == VANA_DLSS_UNAVAILABLE ? (sh->message[0] ? sh->message : "AMD's FSR doesn't run on this card")
                                                                   : "";
    if (why && size > 0)
        snprintf(why, size, "%s", text);
    return text[0] == 0;
}

int gfx_dlss_jitter(float* ndc)
{
    if (g_up.quality < 0 || g_up.failed || up_fsr1() || !g_up.sh || g_up.sh->state != VANA_DLSS_READY || !g_up.in_w ||
        g_up.sh->made != g_up.sh->generation)
        return 0;
    if (g_up.jit_serial != g_serial)
    {
        /* Halton (2, 3), AMD's sequence length for the scale it's drawn at: ceil(8 * n^2), n the output's width over the
         * render's */
        const float k = g_up.out_w ? (float)g_up.in_w / (float)g_up.out_w : up_factor(g_up.quality);
        uint32_t phases = (uint32_t)ceilf(8.0f / (k * k) - 0.001f);
        uint32_t i = g_up.n++ % (phases < 8 ? 8 : phases) + 1;
        g_up.jit[0] = up_halton(i, 2) - 0.5f, g_up.jit[1] = up_halton(i, 3) - 0.5f;
        g_up.jit_serial = g_serial;
    }
    ndc[0] = 2.0f * g_up.jit[0] / (float)g_up.in_w;
    ndc[1] = -2.0f * g_up.jit[1] / (float)g_up.in_h;
    return 1;
}

int gfx_dlss_state(char* message, int size)
{
    /* starting: the helper starting, or making FSR for new textures, or none of its frames back yet; FSR 1 till it's run */
    const int fsr1 = up_fsr1();
    int st = g_up.quality < 0 ? 0 : fsr1 ? (g_up.fsr1 && g_up.out ? 2 : 1) : up_gave_up() ? (g_up.said == 3 ? 3 : 4)
        : !g_up.sh || g_up.sh->state == VANA_DLSS_STARTING || g_up.sh->done == 0 || g_up.sh->made != g_up.sh->generation ? 1
        : 2;
    if (message && size > 0)
    {
        if (fsr1)
            snprintf(message, size, st == 2 ? "Game %ux%u -> FSR 1 %ux%u" : "FSR 1", g_up.in_w, g_up.in_h, g_up.out_w, g_up.out_h);
        else if (st == 2)
            snprintf(message, size, "Game %ux%u -> %s %ux%u", g_up.in_w, g_up.in_h,
                g_up.sh->sr_version[0] ? g_up.sh->sr_version : "FSR", g_up.out_w, g_up.out_h);
        else
            snprintf(message, size, "%s", g_up.why[0] ? g_up.why : g_up.sh ? g_up.sh->message : "");
    }
    return st;
}

/* frame generation is Direct3D 12's (its present thread, on its swap chain): here, never */
void gfx_dlss_framegen(int count) { (void)count; }
int gfx_dlss_framegen_state(char* message, int size)
{
    if (message && size > 0)
        snprintf(message, size, "%s", "needs DirectX 12");
    return 0;
}

/* What the picture is still waiting on (gfx.h's GFX_WAIT_*; Direct3D 12's gfx_waiting): the effects' own shaders
 * compiling, and a tenth or more of the sun's shadow casters left out while their pairs compile */
unsigned gfx_waiting(void)
{
    unsigned bits = 0;
    /* FSR starting while there are scenes for it (not at a menu without one) */
    if (g_up.noted && g_up.noted + 3 >= g_serial && gfx_dlss_state(NULL, 0) == 1)
        bits |= GFX_WAIT_DLSS;
    if (g_fxs.fx != 0.0f && InterlockedCompareExchange(&g_fx.state, 0, 0) == 1)
        bits |= GFX_WAIT_EFFECTS;
    if (g_fx.wait_serial + 3 >= g_serial && g_fx.wait_building && g_fx.wait_building * 10 >= g_fx.wait_casters)
        bits |= GFX_WAIT_SHADOWS;
    return bits;
}

/* AMD's FSR's start, as far as it has come, for the notice: 0 the helper starting (its process, till it's in the shared
 * memory), 1 it loading AMD's FSR, 2 it making FSR for the textures, 3 the first frame not back yet, 4 done. FSR 1, in this
 * process, has no stages: -1 */
static int up_stage(void)
{
    const VanaDlssShared* sh = g_up.sh;
    if (up_fsr1())
        return -1;
    if (!sh || !sh->helper_pid)
        return 0;
    if (sh->state == VANA_DLSS_STARTING)
        return 1;
    if (sh->made != sh->generation)
        return 2;
    if (sh->done == 0)
        return 3;
    return 4;
}

/* How far one GFX_WAIT_* bit of gfx_waiting has come (gfx.h): its steps made of its steps, 0 when it has no count (yet) */
int gfx_wait_progress(unsigned bit, unsigned* done, unsigned* total)
{
    if (bit == GFX_WAIT_EFFECTS)
    {
        const LONG n = InterlockedCompareExchange(&g_fx.steps, 0, 0), d = InterlockedCompareExchange(&g_fx.step, 0, 0);
        if (n <= 0)
            return 0;
        *total = (unsigned)n;
        *done = d < 0 ? 0 : d > n ? (unsigned)n : (unsigned)d;
        return 1;
    }
    if (bit == GFX_WAIT_SHADOWS && g_fx.wait_casters)
    {
        *total = g_fx.wait_casters;
        *done = g_fx.wait_building < g_fx.wait_casters ? g_fx.wait_casters - g_fx.wait_building : 0;
        return 1;
    }
    if (bit == GFX_WAIT_DLSS)
    {
        const int st = up_stage();
        if (st < 0)
            return 0;
        *total = 4, *done = (unsigned)st;
        return 1;
    }
    return 0;
}

/* What one GFX_WAIT_* bit of gfx_waiting is at now (gfx.h): the shader fx_build is compiling, FSR's start's stage
 * (up_stage), in words; "" and 0 without */
int gfx_wait_step(unsigned bit, char* words, int size)
{
    if (!words || size <= 0)
        return 0;
    words[0] = 0;
    if (bit == GFX_WAIT_EFFECTS)
    {
        const char* at = (const char*)InterlockedCompareExchangePointer(&g_fx.at, NULL, NULL);
        if (!at || !at[0])
            return 0;
        snprintf(words, (size_t)size, "%s", at);
        return 1;
    }
    if (bit == GFX_WAIT_SHADOWS && g_fx.wait_casters)
    {
        snprintf(words, (size_t)size, "%s", "Shadow caster pipelines");
        return 1;
    }
    if (bit == GFX_WAIT_DLSS)
    {
        switch (up_stage())
        {
        case 0: snprintf(words, (size_t)size, "%s", "Starting the FSR helper"); return 1;
        case 1: snprintf(words, (size_t)size, "%s", "Loading AMD's FSR"); return 1;
        case 2: snprintf(words, (size_t)size, "Making FSR for %ux%u -> %ux%u", g_up.in_w, g_up.in_h, g_up.out_w, g_up.out_h); return 1;
        case 3: snprintf(words, (size_t)size, "%s", "Waiting for the first frame back"); return 1;
        default: return 0;
        }
    }
    return 0;
}

/* The player's distance from the camera that the sharp map's reach is made from, held (as gfx_d3d12.c's): the reach sets the
 * radius of the sphere the map is cut through, whole units, and a radius that steps puts every shadow edge in the map on a new
 * grid: a flicker while the camera moves in and out. Rounded up to 3 units, kept at the most of the last 10 seconds. */
static float sun_to_held(float to)
{
    static float held = -1.0f;
    static uint64_t low_since;
    if (to < 0.0f)
        return to;
    const float q = ceilf(to / 3.0f) * 3.0f;
    const uint64_t now = gfx_now_ns();
    if (held < 0.0f || q >= held)
        held = q, low_since = 0;
    else if (!low_since)
        low_since = now;
    else if (now - low_since > 10000000000ull)
        held = q, low_since = 0;
    return held;
}

/* The sun's maps for the scene: a near cascade over the first sun_near units the camera sees, where
 * characters stand and shadows are looked at closely (a 4096 map over some 40 units: texels of a
 * centimetre), and a far one out to sun_distance. Fills the effects' uniforms for both; 0 when
 * nothing was drawn. */
static int sun_map(const GfxScene* s, const float* L, FxU* u)
{
    if (!g_ncasters && !g_ncache && !g_nsnaps)
        return 0;
    float invP[16], invV[16];
    if (!mat_inverse(invP, s->proj) || !mat_inverse(invV, s->view))
        return 0;
    float clip_world[16];
    mat_mul(clip_world, invP, invV);
    sun_cache_update(s, clip_world, invV + 12);
    /* the sharp map reaches sun_near past the player (s->focus): the camera stands some way behind them (farther
     * with a camera addon), and measured from it, a short reach left the far half of their own shadow to the
     * coarse map. Without the player, from the camera, and at least 10. */
    float dfar = fmaxf(g_fxs.sun_distance, 4.0f), reach = fmaxf(g_fxs.sun_near, 3.0f), to = -1.0f;
    if (s->focus[3] != 0.0f)
    {
        const float fx = s->focus[0] - invV[12], fy = s->focus[1] - invV[13], fz = s->focus[2] - invV[14];
        to = sqrtf(fx * fx + fy * fy + fz * fz);
        if (!(to < 60.0f))
            to = -1.0f; /* not where the camera could be following: not taken */
    }
    reach = to >= 0.0f ? reach + sun_to_held(to) : fmaxf(reach, 10.0f);
    {
        static int told;
        if (to >= 0.0f && !told)
            told = 1, fprintf(stderr, "[recomp] gfx: shadows: the player %.1f units from the camera: the sharp map reaches %.1f\n", to,
                reach);
    }
    float dnear = fminf(reach, dfar);
    SunCascade cfar, cnear;
    sun_fit(s, invV, L, 0.5f, dfar, SUN_MAP, &cfar);
    if (!sun_target(&g_fx.smap, SUN_MAP))
        return 0;
    uint32_t drawn = sun_draw(&g_fx.smap, s, invP, invV, &cfar, 1, 0);
    memcpy(u->lmat, cfar.lmat, 64);
    u->smap[1] = cfar.texel, u->smap[2] = cfar.bias, u->smap[3] = cfar.soft;
    u->smap2[0] = cfar.slope, u->smap2[3] = cfar.range;
    u->smapn2[1] = 0.0f;
    g_fx.st_near_r = 0.0f;
    const uint32_t nsize = sun_size(g_fxs.sun_detail);
    if (dnear >= 2.0f && sun_target(&g_fx.smapn, nsize))
    {
        sun_fit(s, invV, L, 0.5f, dnear, nsize, &cnear);
        sun_draw(&g_fx.smapn, s, invP, invV, &cnear, 1, 0);
        memcpy(u->lmatn, cnear.lmat, 64);
        u->smapn[0] = cnear.texel, u->smapn[1] = cnear.bias, u->smapn[2] = cnear.soft, u->smapn[3] = cnear.slope;
        u->smapn2[0] = cnear.range, u->smapn2[1] = 1.0f, u->smapm2[2] = 0.95f - fminf(fmaxf(g_fxs.sun_handoff, 0.05f), 0.9f);
        g_fx.st_near_r = 0.5f * cnear.across;
    }
    g_fx.st_across = cfar.across;
    g_fx.st_cached = g_ncache;
    g_fx.st_far_r = 0.5f * cfar.across, g_fx.st_reach = dnear, g_fx.st_to = to;
    return drawn != 0;
}

void gfx_trace_dump(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "frame own-sun strength day live cached no-pipeline map-across sun(x y z) camera(x y z) depth(1 as drawn, 2 another since) "
               "sun-light ambient fog(r g b) ran history(1 blended, 0 no shadows, 2 gap, 3 size, 4 camera moved, 5 off) rays "
               "sun-held vertices-gone far-radius sharp-radius sharp-reach player-distance\n");
    uint32_t n = g_fx.ntrace < 1200 ? g_fx.ntrace : 1200;
    for (uint32_t k = g_fx.ntrace - n; k < g_fx.ntrace; ++k)
    {
        const struct SunTrace* t = &g_fx.trace[k % 1200];
        fprintf(f, "%llu %u %.2f %.2f %u %u %u %.0f %.3f %.3f %.3f %.1f %.1f %.1f %u %.3f %.3f %.3f %.3f %.3f %u %u %.2f %u %u "
                   "%.0f %.0f %.1f %.1f\n",
            (unsigned long long)t->serial, t->own, t->strength, t->day, t->live, t->cached, t->skipped, t->across, t->sun[0], t->sun[1],
            t->sun[2], t->cam[0], t->cam[1], t->cam[2], t->depth, t->dl, t->al, t->fog[0], t->fog[1], t->fog[2], t->ran, t->hist,
            t->rays, t->held, t->gone, t->far_r, t->near_r, t->reach, t->to);
    }
    fclose(f);
}

/* the shadow settings in force, said to the log at the first scene and whenever they change (a flicker report's context: "d3d11",
 * the reach, the sharp map's size, the softness, the size of the scene) */
static void sun_setup_note(const GfxTex* color)
{
    const float v[] = { g_fxs.sun, g_fxs.sun_distance, g_fxs.sun_near, g_fxs.sun_detail, g_fxs.sun_soft, g_fxs.sun_min, g_fxs.sun_face,
        g_fxs.sun_rings, g_fxs.sun_fade, g_fxs.sun_casters, g_fxs.sun_direct, g_fxs.sun_dusk, g_fxs.temporal };
    static float last[sizeof v / sizeof v[0]];
    static int told;
    if (told && !memcmp(last, v, sizeof v))
        return;
    told = 1, memcpy(last, v, sizeof v);
    fprintf(stderr, "[recomp] gfx: shadows: setup (d3d11): sun %.2f, reach %.0f, sharp past the player %.0f at %.0f texels, softness %.3f, "
                    "ignore casters under %.1f, shade faces turned away %.2f, rings %s, fade %.0f s, casters %.0f, fade under cloud %.2f, "
                    "dusk %s, temporal %.2f, a scene of %ux%u\n", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] != 0.0f ? "on" : "off",
        v[8], v[9], v[10], v[11] != 0.0f ? "on" : "off", v[12], color->w, color->h);
}

/* the flicker watch's look at a scene (gfx_watch): what differs from the frame before */
static void fx_watch_frame(const GfxScene* s, const struct SunTrace* t, const GfxTex* color, const GfxTex* depth, int ready)
{
    static const char* const HIST[] = { "no shadows to blend", "", "the frame before had none", "its size changed", "the camera moved",
        "temporal is off" };
    int prev = g_fx.w.serial && g_fx.w.serial + 1 == g_serial;
    float dx = t->cam[0] - g_fx.w.cam[0], dy = t->cam[1] - g_fx.w.cam[1], dz = t->cam[2] - g_fx.w.cam[2];
    int jump = dx * dx + dy * dy + dz * dz > 25.0f; /* a cut or a warp: everything changes, rightly */
    /* the maps' cascades: a sphere that changes radius changes the size of its map's texels, and every edge in it moves */
    gfx_watch_value("shadows: the far map's sphere radius", t->far_r);
    gfx_watch_value("shadows: the sharp map's sphere radius", t->near_r);
    gfx_watch_value("shadows: the sharp map's reach", t->reach);
    gfx_watch_value("shadows: the player's distance from the camera", t->to);
    gfx_watch_value("camera: moved since the frame before", prev ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f);
    sun_setup_note(color);
    if (!t->ran && prev && g_fx.w.ran)
    {
        char why[128];
        if (!ready)
            snprintf(why, sizeof why, "their shaders are still building");
        else if (!depth)
            snprintf(why, sizeof why, "no depth buffer was drawn with the scene");
        else if (depth->w != color->w || depth->h != color->h)
            snprintf(why, sizeof why, "the depth buffer is %ux%u, the scene %ux%u", depth->w, depth->h, color->w, color->h);
        else if (!depth->srv)
            snprintf(why, sizeof why, "the depth buffer can't be read");
        else if (s->proj[11] == 0.0f)
            snprintf(why, sizeof why, "the camera's projection isn't a perspective one");
        else
            snprintf(why, sizeof why, "an effect target couldn't be made");
        gfx_watch("the scene effects didn't run (%s): no grade, fog, shadows, bloom or god rays this frame", why);
    }
    if (t->ran && prev && g_fx.w.ran && !jump)
    {
        float c = t->sun[0] * g_fx.w.sun[0] + t->sun[1] * g_fx.w.sun[1] + t->sun[2] * g_fx.w.sun[2];
        int had = g_fx.w.sun[0] != 0.0f || g_fx.w.sun[1] != 0.0f || g_fx.w.sun[2] != 0.0f; /* not the first sun */
        if (had && c < 0.99985f)
            gfx_watch("the sun turned %.1f degrees (%s): the shadows, god rays and fog's glow turn with it",
                acosf(fmaxf(fminf(c, 1.0f), -1.0f)) * 57.29578f, s->sun_dir[3] != 0.0f ? "a lit draw's light" : "no lit draw");
        if (fabsf(t->strength - g_fx.w.smap) > 0.05f)
            gfx_watch("the map shadows went %.2f -> %.2f (sun up %.2f; casters drawn %u, from the cache %u, left out while their "
                      "shaders build %u)", g_fx.w.smap, t->strength, t->day, t->live, t->cached, t->skipped);
        else if (t->skipped)
            gfx_watch("%u shadow casters left out: their shadow shaders are still building", t->skipped);
        if (t->gone)
            gfx_watch("%u shadow casters left out: their vertices' buffer was written again before the scene was done",
                t->gone);
        if (fabsf(t->rays - g_fx.w.rays) > 0.1f)
            gfx_watch("the god rays went %.2f -> %.2f", g_fx.w.rays, t->rays);
        if (g_fx.w.hist == 1 && t->hist >= 2 && t->hist <= 4)
            gfx_watch("the frame before wasn't blended in (%s): this frame's shadows are unsmoothed", HIST[t->hist]);
        if ((s->fog[2] != 0.0f) != (g_fx.w.fog != 0))
            gfx_watch("the camera draw's fog went %s: the effects' fog fades %s", s->fog[2] != 0.0f ? "on" : "off",
                s->fog[2] != 0.0f ? "in" : "out");
        if (depth != g_fx.w.depth || t->depth != g_fx.w.depth_kind)
            gfx_watch("the effects read another depth buffer (%s)", t->depth == 2 ? "the one the world was drawn with, another since" : "the last one drawn with");
        /* a map cut through a sphere of another radius (the maps are cut afresh every frame here): the camera moved the player's
         * distance (the sharp map's reach follows it), or the sharp map came in or went */
        if (g_fx.w.far_r > 0.0f && t->far_r > 0.0f && fabsf(t->far_r - g_fx.w.far_r) >= 0.5f)
            gfx_watch("the far map's sphere went radius %.0f -> %.0f units: its texels changed size, and every shadow edge in it moved "
                      "onto a new grid (the camera moved %.2f units)", g_fx.w.far_r, t->far_r, sqrtf(dx * dx + dy * dy + dz * dz));
        if (g_fx.w.near_r > 0.0f && t->near_r > 0.0f && fabsf(t->near_r - g_fx.w.near_r) >= 0.5f)
            gfx_watch("the sharp map's sphere went radius %.0f -> %.0f units (reach %.1f, the player %.1f from the camera): its texels "
                      "changed size, and every shadow edge in it moved onto a new grid (the camera moved %.2f units)", g_fx.w.near_r,
                t->near_r, t->reach, t->to, sqrtf(dx * dx + dy * dy + dz * dz));
        if ((g_fx.w.near_r > 0.0f) != (t->near_r > 0.0f))
            gfx_watch("the sharp map %s: the shadows near the player are cut from %s", t->near_r > 0.0f ? "came back" : "went",
                t->near_r > 0.0f ? "its texels again" : "the far map's texels");
    }
    g_fx.w.far_r = t->far_r, g_fx.w.near_r = t->near_r;
    g_fx.w.serial = g_serial, g_fx.w.ran = t->ran, g_fx.w.hist = t->hist, g_fx.w.fog = s->fog[2] != 0.0f;
    g_fx.w.depth = depth, g_fx.w.depth_kind = t->depth;
    memcpy(g_fx.w.sun, t->sun, 12), memcpy(g_fx.w.cam, t->cam, 12);
    g_fx.w.smap = t->strength, g_fx.w.rays = t->rays;
}

/* FXAA over the whole finished scene: its copy, then the pass from the copy onto it */
static void fx_antialias(GfxTex* color)
{
    if (!g_fx.ps[FX_FXAA] || !fx_tex(&g_fx.src, color->dxfmt, color->w, color->h))
        return;
    fx_copy_scene(color);
    float vp[4] = { 0.0f, 0.0f, (float)color->w, (float)color->h };
    fx_draw((ID3D11RenderTargetView*)target_view(color, 0, 0), g_fx.ps[FX_FXAA], vp, NULL, &g_fx.src.srv, 1, 0, 0);
}

#include "gfx_d3d11_glow.h" /* gfx_glow, gfx_glow_over_grass: VanaCore's glow round avatars and trusts, and again over the grass */

/* gfx.h: VanaCore's sky is Direct3D 12's for now: none here, and the game's dome stays */
int gfx_sky(const GfxSky* s)
{
    (void)s;
    return 0;
}

/* gfx.h: the zone's floors' layer is for Direct3D 12's reflections, which don't run here: none wanted, none kept */
void gfx_scene_floors(GfxTex* id, GfxTex* depth, int state)
{
    (void)id;
    (void)depth;
    (void)state;
}

int gfx_scene_floors_wanted(void) { return 0; }
void gfx_scene_rt_geometry(const void* data, uint32_t bytes, uint32_t node_bytes, int scene, uint32_t attr_at, GfxTex* const* tex, uint32_t ntex)
{
    (void)data, (void)bytes, (void)node_bytes, (void)scene, (void)attr_at, (void)tex, (void)ntex;
}
void gfx_scene_rain_mask(GfxTex* mask, float wet, float reflect, float puddles) { (void)mask, (void)wet, (void)reflect, (void)puddles; }
int gfx_scene_rain_mask_wanted(void) { return 0; }

/* the shadows' record is Direct3D 12's (gfx_sun_record.h) */
int gfx_sun_record(const char* path, uint32_t max_mb)
{
    (void)path;
    (void)max_mb;
    return 0;
}

float gfx_seek_reach(float* stretch)
{
    if (stretch)
        *stretch = 0.0f;
    return 0.0f;
}

/* gfx.h: the scene's depth before the grass, copied whole (Direct3D 11 copies depth no other way) for this scene's occlusion and
 * contact shadows, and its ink lines without the grass (gfx_scene_done) */
void gfx_scene_world_depth(GfxTex* depth)
{
    g_fx.wdepth_of = g_fx.wdepth_glow = NULL;
    /* ... and Render visual 26's grass; and the glow laid again over the grass (gfx_glow_over_grass), which alone leaves the rest */
    const int lines = (g_fxs.outline > 0.0f && g_fxs.outline_nograss != 0.0f) || (int)g_fxs.debug == 26;
    /* ... and the sun's shadows on the tufts (fx_ao): without it each tuft was shaded as ground by its blades' own angle, the
     * grass grey all over, with occlusion off (2026-11-22) */
    const int others = g_fxs.fx != 0.0f && (g_fxs.ao > 0.0f || g_fxs.sun > 0.0f || lines);
    if (!g_dev || !(others || g_fx.glow_kept_serial == g_serial) || !depth || depth->use != GFX_USE_DEPTH || depth->type != GFX_TEX_2D ||
        !depth->srv)
        return;
    if (!g_fx.wdepth || g_fx.wdepth->w != depth->w || g_fx.wdepth->h != depth->h || g_fx.wdepth->dxfmt != depth->dxfmt)
    {
        gfx_tex_destroy(g_fx.wdepth);
        g_fx.wdepth = gfx_tex_create(GFX_TEX_2D, depth->fmt, depth->w, depth->h, 1, GFX_USE_DEPTH);
    }
    if (!g_fx.wdepth || !g_fx.wdepth->srv)
        return;
    gfx_copy(depth, 0, 0, 0, 0, depth->w, depth->h, g_fx.wdepth, 0, 0, 0, 0);
    g_fx.wdepth_of = others ? depth : NULL, g_fx.wdepth_glow = depth, g_fx.wdepth_serial = g_serial;
}

/* gfx.h: the scene's colour and depth copied for VanaCore's water pass (d3d8_water.h water_pass). The depth is the one the target
 * was last drawn with (depth_seen: the scratch one of its size when the game's is larger), so the copy is what its world was
 * drawn against; the colour copy is a plain sampled texture of the target's format (gfx_copy wants the formats the same) */
static GfxTex *g_snap_color, *g_snap_depth;
static uint32_t g_snap_serial; /* the frame (g_serial) the copies were last taken in: fx_ao's water mask (GfxScene.water_noshadow) */
int gfx_scene_snapshot(GfxTex* color, GfxTex** colour_copy, GfxTex** depth_copy, uint32_t* w, uint32_t* h)
{
    *colour_copy = *depth_copy = NULL, *w = *h = 0;
    if (!g_dev || !color || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return 0;
    GfxTex* depth = color->depth_seen;
    if (!depth || depth->use != GFX_USE_DEPTH || depth->type != GFX_TEX_2D || depth->w != color->w || depth->h != color->h || !depth->srv)
        return 0;
    if (!g_snap_color || g_snap_color->w != color->w || g_snap_color->h != color->h || g_snap_color->dxfmt != color->dxfmt)
    {
        gfx_tex_destroy(g_snap_color);
        g_snap_color = gfx_tex_create(GFX_TEX_2D, color->fmt, color->w, color->h, 1, GFX_USE_SAMPLE);
    }
    if (!g_snap_depth || g_snap_depth->w != depth->w || g_snap_depth->h != depth->h || g_snap_depth->dxfmt != depth->dxfmt)
    {
        gfx_tex_destroy(g_snap_depth);
        g_snap_depth = gfx_tex_create(GFX_TEX_2D, depth->fmt, depth->w, depth->h, 1, GFX_USE_DEPTH);
    }
    if (!g_snap_color || !g_snap_depth || !g_snap_color->srv || !g_snap_depth->srv)
        return 0;
    gfx_copy(color, 0, 0, 0, 0, color->w, color->h, g_snap_color, 0, 0, 0, 0);
    gfx_copy(depth, 0, 0, 0, 0, depth->w, depth->h, g_snap_depth, 0, 0, 0, 0);
    *colour_copy = g_snap_color, *depth_copy = g_snap_depth, *w = color->w, *h = color->h;
    g_snap_serial = (uint32_t)g_serial;
    return 1;
}

/* Depth of field's focus distance for the scene s (as gfx_d3d12.c's): dof_focus when it's set; else, with dof_player (the
 * free camera's Focus: Player), how far into the view your character stands, wherever on the screen, while its place is
 * known and before the camera; else 0, the nearest thing at the screen's middle (fx_focus) */
static float dof_focus_now(const GfxScene* s)
{
    if (g_fxs.dof_focus > 0.0f || g_fxs.dof_player == 0.0f || !s || s->focus[3] == 0.0f)
        return g_fxs.dof_focus;
    const float z = (s->focus[0] * s->view[2] + s->focus[1] * s->view[6] + s->focus[2] * s->view[10] + s->view[14]) *
                    (s->proj[11] < 0.0f ? -1.0f : 1.0f);
    return z > 0.25f ? z : 0.0f;
}

void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    const int fsr_on = g_up.quality >= 0 && !up_gave_up(); /* FSR, last (up_frame) */
    const int gathers = fsr_on && !up_fsr1();              /* ... AMD's, jittered, gathering the frames: not FSR 1's */
    int aa = g_fxs.aa != 0.0f && !gathers;                 /* AMD's FSR smooths edges itself; FSR 1 wants them smoothed first */
    shot_scene(color, s);
    if (!fsr_on && g_up.out)
    {
        up_release(); /* FSR off: its textures let go (made again when it's back) */
        g_up.in_w = g_up.in_h = g_up.out_w = g_up.out_h = 0;
    }
    if (!g_dev || (g_fxs.fx == 0.0f && !aa && !fsr_on) || !color || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
    {
        casters_clear();
        return;
    }
    int ready = fx_ready();
    /* the depth the scene's casters (its world) were drawn with - a later pass on the target with
     * another depth does not change it - else the last one */
    GfxTex* depth = color->depth_world ? color->depth_world : color->depth_seen;
    g_fx.tr_depth = color->depth_world && color->depth_world != color->depth_seen ? 2 : depth ? 1 : 0;
    if (g_fxs.fx != 0.0f && ready && depth && depth->w == color->w && depth->h == color->h && depth->srv && s->proj[11] != 0.0f)
    {
        /* the scene's viewport, within the target */
        float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
        if (vw < 16 || vh < 16 || vx + vw > color->w || vy + vh > color->h)
            vx = vy = 0, vw = (float)color->w, vh = (float)color->h;
        float minz, maxz;
        memcpy(&minz, &s->vp[4], 4);
        memcpy(&maxz, &s->vp[5], 4);
        if (maxz <= minz)
            minz = 0, maxz = 1;
        /* the occlusion (and the sun's shadows and the reflections with it) at the scene's own size at every quality:
         * at half a scene over 2048 across each of its texels took one of four pixels, the one on an edge or a thin
         * thing changing as the camera moved, and its history let go there (a shimmer at all but max); bloom and
         * rays, soft anyway, at about 1000 */
        uint32_t bdiv = vw > 2048 ? 4 : vw > 1024 ? 2 : 1;
        uint32_t aw = (uint32_t)vw, ah = (uint32_t)vh;
        uint32_t bw = (uint32_t)((vw + bdiv - 1) / bdiv), bh = (uint32_t)((vh + bdiv - 1) / bdiv);
        float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f;
        FxU u = {
            { s->proj[0], s->proj[5], s->proj[8], s->proj[9] },
            { s->proj[10], s->proj[14], minz, maxz },
            { vx, vy, vw, vh },
            { (float)color->w, (float)color->h, (float)aw, (float)ah },
            { g_fxs.radius, g_fxs.ao, 0.15f, vh * 0.1f },
            { g_fxs.grade, g_fxs.sat, g_fxs.contrast, g_fxs.debug },
            { hand, 0, 0, 0 },
        };
        u.grade2[0] = g_fxs.brightness, u.grade2[1] = g_fxs.shadows, u.grade2[2] = g_fxs.highlights;
        /* (bloom.w: the grade's warmth, gfx_d3d12.c's; day_night_warmth's share warmer by day, cooler at night) */
        u.bloom[3] = g_fxs.warmth + fminf(fmaxf(g_fxs.day_night_warmth, 0.0f), 2.0f) * s->day_night;
        u.post4[0] = s->night, u.post4[1] = fminf(fmaxf(g_fxs.night_moon, 0.0f), 1.0f); /* the moonlight (moon_grade) */
        u.post4[2] = fminf(fmaxf(g_fxs.hue, -180.0f), 180.0f) * 0.0174532925f;             /* the grade's hue, radians (hue_turn) */
        gfx_player_light(s, g_fxs.player_light, g_fxs.player_light_radius, g_fxs.player_light_warm, g_fxs.player_light_day,
                         u.plight, u.plight2);
        /* world up in view space: the world's y axis through the inverse view matrix, pointing
         * the way the camera's own up does (FFXI's world y points down) */
        g_fx.eased = g_fx.eased_serial && g_fx.eased_serial + 1 == g_serial;
        g_fx.eased_serial = g_serial;
        float vinv[16], up[3];
        int have_v = mat_inverse(vinv, s->view);
        if (have_v)
        {
            float sign = vinv[5] < 0.0f ? -1.0f : 1.0f;
            up[0] = vinv[1] * sign, up[1] = vinv[5] * sign, up[2] = vinv[9] * sign;
            normalize3(up);
            /* fog where the game fogs its world (its fog color is the zone's), fading in and out */
            float on = s->fog[2] != 0.0f ? 1.0f : 0.0f;
            fx_ease(&g_fx.fog_on, &on, 1, 0.1f);
            /* the fog's colour eased over about a second, whether or not the last scene was the frame
             * before: FFXI sets it per object, and the first fogged object differs from frame to frame
             * near its lights; a new zone or a camera jump (the cache's) takes it at once */
            /* fog_color=custom: this colour in place of the zone's (the height fog's; the game's own fog is as it is) */
            const float fogcustom[3] = { g_fxs.fog_cr, g_fxs.fog_cg, g_fxs.fog_cb };
            const float* fogwant = g_fxs.fog_mode == 2.0f ? fogcustom : s->fogcolor;
            if (g_fxs.fog <= 0.0f)
                g_fx.fogc_set = 0; /* no fog: the next one takes its colour at once */
            else if (!g_fx.fogc_set)
                memcpy(g_fx.fogc, fogwant, 12), g_fx.fogc_set = 1;
            else
                for (int j = 0; j < 3; ++j)
                    g_fx.fogc[j] += (fogwant[j] - g_fx.fogc[j]) * 0.04f;
            memcpy(g_fx.tr_fog, s->fogcolor, 12);
            fx_ease(g_fx.up, up, 3, 0.2f);
            normalize3(g_fx.up);
            memcpy(u.up, g_fx.up, 12);
            u.up[3] = g_fxs.fog_falloff * fmaxf(g_fxs.fog_floor, 0.0f); /* the fog's floor (up.w): falloff x fog_floor */
            gfx_player_pool_keep(u.plight, u.plight2, u.up); /* for the world's draws after the effects (d3d8_casters.h late_pool) */
            memcpy(u.fogc, g_fx.fogc, 12);
            u.fogc[3] = g_fxs.fog * g_fx.fog_on * expf(-g_fxs.fog_falloff * g_fxs.fog_height);
            u.fogp[0] = g_fxs.fog_falloff, u.fogp[1] = g_fxs.fog_max, u.fogp[2] = g_fxs.fog_sun * 0.2f; /* (fog_sunlight, 0 to 10) */
            u.fogp[3] = fminf(fmaxf(g_fxs.fog_g, 0.0f), 0.95f);
        }
        /* the sun, kept in the world: the zone's shader draws carry no light, so a frame gives one
         * only when a lit draw (a character) is in view - the shadows hold the last one rather than
         * come and go with what the camera sees */
        int own = s->sun_dir[3] != 0.0f && have_v, held = 0;
        if (own)
        {
            float w[3];
            for (int j = 0; j < 3; ++j)
                w[j] = s->sun_dir[0] * vinv[j] + s->sun_dir[1] * vinv[4 + j] + s->sun_dir[2] * vinv[8 + j];
            normalize3(w);
            /* the sun moves on in steps of a quarter degree: the shadows glide to each new one (gfx_sun_glide), and hold still
             * between them rather than crawl a little every frame (a move short of the step is the game's own noise) */
            float dot = w[0] * g_fx.glide.to[0] + w[1] * g_fx.glide.to[1] + w[2] * g_fx.glide.to[2];
            /* a turn of more than a degree is taken once four frames in a row give it (a new zone, a
             * cutscene's light): a draw lit by a directional light of its own that was the frame's first
             * lit draw would turn the shadows, the god rays and the fog's glow with it for that frame */
            if (!g_fx.sunw_seen || !g_fx.eased || dot > 0.99985f)
            {
                if (!g_fx.sunw_seen || dot < 0.999995f)
                    gfx_sun_glide(&g_fx.glide, g_fx.sunw, w, !g_fx.sunw_seen || !g_fx.eased);
                g_fx.sun_cand_n = 0;
            }
            else
            {
                float dc = w[0] * g_fx.sun_cand[0] + w[1] * g_fx.sun_cand[1] + w[2] * g_fx.sun_cand[2];
                if (g_fx.sun_cand_n && g_fx.sun_cand_serial + 1 == g_serial && dc > 0.99985f)
                    g_fx.sun_cand_n++;
                else
                    memcpy(g_fx.sun_cand, w, 12), g_fx.sun_cand_n = 1;
                g_fx.sun_cand_serial = g_serial;
                if (g_fx.sun_cand_n >= 4)
                    gfx_sun_glide(&g_fx.glide, g_fx.sunw, w, 0), g_fx.sun_cand_n = 0;
                else
                {
                    held = 1;
                    if (g_fx.sun_cand_n == 1)
                        gfx_watch("the frame's first lit draw (from draw %u) has a light %.1f degrees from the sun's: held back, "
                                  "not followed by the shadows and god rays", s->sun_draw, acosf(fmaxf(fminf(dot, 1.0f), -1.0f)) * 57.29578f);
                }
            }
            gfx_sun_glide_on(&g_fx.glide, g_fx.sunw);
            g_fx.sunw_seen = g_serial;
            if (!held)
                fx_ease(g_fx.suncol, s->sun_color, 3, 0.1f);
        }
        g_fx.tr_held = (uint32_t)held;
        /* (the god rays and the fog's glow too: without a lit draw in view they came and went with it) */
        if (have_v && (own || (g_fx.sunw_seen && g_fx.sunw_seen + 3600 > g_serial)))
        {
            /* back into this frame's view */
            for (int j = 0; j < 3; ++j)
                g_fx.sun[j] = g_fx.sunw[0] * s->view[j] + g_fx.sunw[1] * s->view[4 + j] + g_fx.sunw[2] * s->view[8 + j];
            normalize3(g_fx.sun);
            memcpy(u.sun, g_fx.sun, 12);
            u.sun[3] = 1.0f;
            memcpy(u.suncol, g_fx.suncol, 12);
            /* shadows while the sun (or moon) is up: fading as it nears the horizon */
            float e = g_fx.sun[0] * g_fx.up[0] + g_fx.sun[1] * g_fx.up[1] + g_fx.sun[2] * g_fx.up[2];
            float day = fminf(fmaxf((e - 0.05f) / 0.15f, 0.0f), 1.0f);
            /* the light the shadows fall from, in the world, and their strength: the dusk clamp, the dawn, the cloud (sun_direct,
             * measured while the sun is the game's own and not held) and a room, or (sun_free) where the panel puts it (gfx_shared.c
             * gfx_sun_light, as gfx_d3d12.c's) */
            const GfxSunLightIn in = { s, g_fx.sunw, g_fx.up, g_fxs.sun_free, g_fxs.sun_free_az, g_fxs.sun_free_el, g_fxs.sun_dusk,
                g_fxs.sun_direct, g_fxs.moghouse, own && !held, g_fx.eased, g_fxs.sun_tilt, g_fxs.sun_moon };
            GfxSunLight sl = { { 0 } };
            sl.day = day, sl.direct = g_fx.direct;
            gfx_sun_light(&in, &sl);
            const float* sw = sl.sw;
            day = sl.day;
            if (in.measured)
                g_fx.direct = sl.direct, g_fx.tr_dl = sl.dl, g_fx.tr_al = sl.al;
            gfx_sun_shadow_dir(u.sunsh, sw, s->view);
            u.sunsh[3] = 1.0f;
            /* (contact shadows: gone, 2026-11-02 - strength 0 leaves the shader's ray untaken) */
            u.shadow[0] = 0.0f, u.shadow[1] = 0.1f, u.shadow[2] = 0.3f, u.shadow[3] = 40.0f;
            float sday = 0.0f;
            if (g_fxs.sun > 0.0f && day > 0.0f && sun_map(s, sw, &u))
                sday = g_fxs.sun * day, g_fx.st_drawn_this = 1;
            u.smap[0] = sday;
            g_fx.tr_strength = u.smap[0], g_fx.tr_day = day;
            u.smap2[1] = fminf(fmaxf(g_fxs.sun_face, 0.0f), 1.0f), u.smap2[2] = fmaxf(g_fxs.sun_min, 0.0f);
            /* the sun's place on screen: far along its direction, through the projection */
            const float* sd = g_fx.sun;
            float cw = sd[2] * s->proj[11];
            if (cw > 0.05f)
            {
                float nx = (sd[0] * s->proj[0] + sd[2] * s->proj[8]) / cw;
                float ny = (sd[1] * s->proj[5] + sd[2] * s->proj[9]) / cw;
                u.sunuv[0] = nx * 0.5f + 0.5f, u.sunuv[1] = 0.5f - ny * 0.5f;
                float out = fmaxf(fabsf(u.sunuv[0] - 0.5f), fabsf(u.sunuv[1] - 0.5f)) - 0.5f; /* past the edge */
                u.sunuv[2] = fminf(fmaxf(1.0f - out / 0.5f, 0.0f), 1.0f) * fminf(cw / 0.3f, 1.0f);
            }
        }
        gfx_height_fog_keep(u.fogc, u.fogp, u.up, u.sun, u.suncol); /* for the world's draws after the effects (late_pool) */
        /* the frame before, for the temporal pass: its camera, if it was the frame just before, at this
         * size, and not a jump away */
        {
            int ok = g_fx.hist_serial && g_fx.hist_serial + 1 == g_serial && g_fx.hist[0].res && g_fx.hist[0].w == aw &&
                g_fx.hist[0].h == ah && have_v;
            g_fx.tr_hist = g_fxs.temporal <= 0.0f ? 5 : !(g_fx.hist_serial && g_fx.hist_serial + 1 == g_serial) ? 2 : !ok ? 3 : 1;
            if (ok)
            {
                float dx = vinv[12] - g_fx.prev_cam[0], dy = vinv[13] - g_fx.prev_cam[1], dz = vinv[14] - g_fx.prev_cam[2];
                ok = dx * dx + dy * dy + dz * dz < 25.0f;
                if (!ok)
                    g_fx.tr_hist = 4;
            }
            if (ok)
            {
                float m[16];
                mat_mul(m, vinv, g_fx.prev_view);
                mat_mul(u.reproj, m, g_fx.prev_proj);
                u.hist[0] = 1.0f;
            }
            /* the patterns' turn: a golden-ratio step each frame, for the temporal pass to even out
             * (without it, the same every frame) */
            u.hist[1] = g_fxs.temporal > 0.0f ? (float)fmod((double)g_serial * 0.6180339887, 1.0) : 0.0f;
            u.hist[2] = fminf(fmaxf(g_fxs.temporal, 0.0f), 0.99f);
            u.aoq[0] = AO_TAPS[fx_level(g_fxs.ao_quality)];
            u.aoq[1] = g_fxs.sun_soft <= 0.0f ? 1.0f : 0.0f;
            /* the depth buffer's step (fx_ao: the normal and the taps keep clear of what a step is) */
            u.aoq[2] = depth->fmt == F_D16 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
            /* the occlusion's pattern held still (fx_ao): FSR gathers the frames, or the temporal pass too few to even
             * out its 16 steps (a new grain each frame, a shimmer); 2 where FSR gathers, the sun's shadows' held still too */
            u.aoq[3] = gathers ? 2.0f : g_fxs.temporal < 0.9f ? 1.0f : 0.0f;
            /* the sun's shadows' rings (sun_look): fewer of the maps' taps past each, sun_ring_spacing apart */
            u.aor[0] = 0.0f;
            u.aor[1] = 0.0f;
            u.aor[2] = g_fxs.sun_rings > 0.0f ? 1.0f : 0.0f;
            u.aor[3] = fmaxf(g_fxs.sun_ring_spacing, 1.0f);
            zone_lines(s, have_v ? vinv + 12 : NULL, &u);
            sparkles(s, have_v ? vinv + 12 : NULL, &u);
            ground_marks(s, have_v ? vinv + 12 : NULL, &u);
            memcpy(g_fx.prev_view, s->view, 64), memcpy(g_fx.prev_proj, s->proj, 64);
            if (have_v)
                memcpy(g_fx.prev_cam, vinv + 12, 12);
        }
        u.clar[0] = g_fxs.clarity;
        u.bloom[0] = g_fxs.threshold, u.bloom[1] = g_fxs.bloom, u.bloom[2] = 0.25f;
        u.rays[0] = u.sunuv[2] > 0.0f ? g_fxs.rays : 0.0f, u.rays[1] = g_fxs.rays_decay, u.rays[2] = g_fxs.rays_length;
        /* the cel look: its lines outline_width pixels wide on a 1080-line screen, whatever size the
         * scene is drawn at (FFXI's background is often larger than the screen) */
        float lw = g_fxs.outline_width * vh / 1080.0f, sr = roundf(2.0f * vh / 1080.0f);
        u.toon[0] = g_fxs.outline > 1.0f ? 1.0f : g_fxs.outline;
        u.toon[1] = lw < 1.0f ? 1.0f : lw;
        u.toon[2] = g_fxs.outline_depth > 1e-3f ? g_fxs.outline_depth : 1e-3f;
        u.toon[3] = g_fxs.outline_crease;
        u.toon2[0] = g_fxs.outline_fade > 0.0f ? g_fxs.outline_fade : 1e9f;
        u.toon2[1] = g_fxs.bands >= 2.0f ? floorf(g_fxs.bands) : 0.0f;
        u.toon2[2] = g_fxs.band_soft;
        u.toon3[0] = g_fxs.grade <= 0.0f ? 0.0f : g_fxs.smooth > 1.0f ? 1.0f : g_fxs.smooth;
        u.toon3[1] = sr < 2.0f ? 2.0f : sr > 6.0f ? 6.0f : sr;
        /* ... and VanaCore's grass left out of the lines, given this scene's depth before it (gfx_scene_world_depth) */
        u.toon3[2] = g_fxs.outline_nograss != 0.0f && g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial ? 1.0f : 0.0f;
        if (u.toon3[2] > 0.0f) /* ... with its own lines (outline_grass) */
            gfx_grass_lines(g_fxs.outline_grass, g_fxs.outline_width_grass, g_fxs.outline_depth_grass, g_fxs.outline_fade_grass, vh,
                u.toon, u.toon2, u.toon4);
        /* depth of field: its widest blur dof pixels on a 1080-line screen, as the lines are */
        float dr = g_fxs.dof * vh / 1080.0f;
        u.dof[0] = dr > 48.0f ? 48.0f : dr;
        u.dof[1] = g_fxs.dof_range > 0.0f ? g_fxs.dof_range : 0.0f;
        u.dof[2] = g_fxs.dof_far;
        u.dof[3] = g_fxs.dof_near > 1.0f ? 1.0f : g_fxs.dof_near;
        u.dof2[0] = dof_focus_now(s);
        u.dof2[3] = g_fxs.dof_sky != 0.0f ? 1.0f : 0.0f;
        /* the zone's shadows blurred this far across its surfaces (sun_soft_zone: fx_blur, which keeps fx_ao's uniforms), where the
         * sun's maps are drawn; and with sun_game_shading 0 (the default) the two parts that weigh the shadows against the game's
         * own smooth shading left out (gfx_d3d12.c's: sun_map's bit 256; the smooth normals stay for sun_face's angle) */
        u.aoc[2] = g_fxs.sun > 0.0f ? fminf(fmaxf(g_fxs.sun_soft_zone, 0.0f), 2.0f) : 0.0f;
        u.aoc2[2] = g_fxs.sun_game_shading == 0.0f ? 256.0f : 0.0f;
        /* the scene's smooth normals (vn_replay), against the depth before VanaCore's grass when it was drawn (fx_ao's grass takes
         * the ground's), when a pass reads them: sun_map for the faces turned away (sun_face) or with Game Shading on, the
         * player's light at night (fx_comp) */
        {
            GfxTex* vd = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial ? g_fx.wdepth : depth;
            const int sun_reads = u.smap[0] > 0.0f && (u.smap2[1] > 0.0f || u.aoc2[2] == 0.0f);
            const int want = sun_reads || (u.plight[3] > 0.0f && u.plight2[0] > 0.0f);
            const int vn = vn_replay(color, vd, s, want);
            /* ... and over them the zone's own curved surfaces (zn_draw), where the sun's maps are drawn */
            const int zn = u.smap[0] > 0.0f && g_fxs.sun_curved >= 0.5f ? zn_draw(color, vd, s, vn) : 0;
            u.sunuv[3] = vn || zn ? 1.0f : 0.0f;
        }
        g_fx.tr_rays = u.rays[0] * u.sunuv[2];
        if (!(u.ao[1] > 0.0f || u.smap[0] > 0.0f))
            g_fx.tr_hist = 0;
        const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (fx_tex(&g_fx.src, color->dxfmt, color->w, color->h) && fx_tex(&g_fx.ao0, RGBA, aw, ah) && fx_tex(&g_fx.ao1, RGBA, aw, ah) &&
            fx_tex(&g_fx.b1a, RGBA, bw, bh) && fx_tex(&g_fx.b1b, RGBA, bw, bh) && fx_tex(&g_fx.b2a, RGBA, (bw + 1) / 2, (bh + 1) / 2) &&
            fx_tex(&g_fx.b2b, RGBA, (bw + 1) / 2, (bh + 1) / 2) && fx_tex(&g_fx.ra, RGBA, bw, bh) && fx_tex(&g_fx.rb, RGBA, bw, bh))
        {
            g_fx.tr_ran = 1;
            /* the scene as it was, which the passes read while the last one draws over it */
            fx_copy_scene(color);
            FxTex* ao_out = &g_fx.ao0;
            if (u.ao[1] > 0.0f || u.smap[0] > 0.0f)
            {
                const int sunm = u.smap[0] > 0.0f;
                /* the world without VanaCore's grass (its depth before the grass, gfx_scene_world_depth: this scene's), and the
                 * depth with it last (fx_ao) */
                GfxTex* world = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial ? g_fx.wdepth : depth;
                ID3D11ShaderResourceView* ao_in[10] = { world->srv, sunm ? g_fx.smap.srv : NULL,
                    sunm && u.smapn2[1] > 0.0f ? g_fx.smapn.srv : NULL, NULL, NULL, NULL, NULL, NULL, depth->srv, NULL };
                if (u.ao[1] > 0.0f && fx_lz(aw, ah))
                {
                    fx_pass(&g_fx.lz[0], FX_LINZ, &u, &world->srv, 1, 0, 0);
                    for (int lv = 1; lv < 4; ++lv)
                        fx_pass(&g_fx.lz[lv], FX_ZMIP, &u, &g_fx.lz[lv - 1].srv, 1, 0, 0);
                    for (int lv = 0; lv < 4; ++lv)
                        ao_in[3 + lv] = g_fx.lz[lv].srv;
                }
                else
                    u.ao[1] = 0.0f; /* no depth for it: none this frame */
                if (s && s->water_noshadow && g_snap_depth && g_snap_serial == (uint32_t)g_serial && g_snap_depth->w == depth->w &&
                    g_snap_depth->h == depth->h && g_snap_depth->srv) /* input 9: the depth before the water (fx_ao's water mask) */
                    ao_in[9] = g_snap_depth->srv, u.aoc2[2] = (float)((uint32_t)u.aoc2[2] | 512u);
                fx_pass(&g_fx.ao0, FX_AO, &u, ao_in, 10, 0, 0);
                fx_pass(&g_fx.ao1, FX_BLUR, NULL, &g_fx.ao0.srv, 1, 1, 0);
                fx_pass(&g_fx.ao0, FX_BLUR, NULL, &g_fx.ao1.srv, 1, 0, 1);
                if (g_fxs.temporal > 0.0f && !gathers && fx_tex(&g_fx.hist[0], RGBA, aw, ah) && fx_tex(&g_fx.hist[1], RGBA, aw, ah))
                {
                    int to_i = g_fx.hist_at ^ 1;
                    ID3D11ShaderResourceView* t_in[2] = { g_fx.ao0.srv, g_fx.hist[g_fx.hist_at].srv };
                    fx_pass(&g_fx.hist[to_i], FX_TEMPORAL, NULL, t_in, 2, 0, 0);
                    ao_out = &g_fx.hist[to_i];
                    g_fx.hist_at = to_i, g_fx.hist_serial = g_serial;
                }
            }
            if (u.bloom[1] > 0.0f)
            {
                /* the occlusion and the shadows too when they were drawn (fx_bright shades by them, gfx_d3d12.c's) */
                const int shaded = u.ao[1] > 0.0f || u.smap[0] > 0.0f;
                ID3D11ShaderResourceView* bright_in[2] = { g_fx.src.srv, ao_out->srv };
                fx_pass(&g_fx.b1a, FX_BRIGHT, &u, bright_in, shaded ? 2 : 1, 0, 0);
                fx_pass(&g_fx.b1b, FX_GAUSS, NULL, &g_fx.b1a.srv, 1, 2, 0);
                fx_pass(&g_fx.b1a, FX_GAUSS, NULL, &g_fx.b1b.srv, 1, 0, 2);
                fx_pass(&g_fx.b2a, FX_DOWN, NULL, &g_fx.b1a.srv, 1, 0, 0);
                fx_pass(&g_fx.b2b, FX_GAUSS, NULL, &g_fx.b2a.srv, 1, 2, 0);
                fx_pass(&g_fx.b2a, FX_GAUSS, NULL, &g_fx.b2b.srv, 1, 0, 2);
            }
            if (u.rays[0] > 0.0f)
            {
                ID3D11ShaderResourceView* mask_in[2] = { g_fx.src.srv, depth->srv };
                fx_pass(&g_fx.ra, FX_RAYMASK, &u, mask_in, 2, 0, 0);
                fx_pass(&g_fx.rb, FX_RAYS, NULL, &g_fx.ra.srv, 1, 0, 0);
                fx_pass(&g_fx.ra, FX_GAUSS, NULL, &g_fx.rb.srv, 1, 1, 0);
                fx_pass(&g_fx.rb, FX_GAUSS, NULL, &g_fx.ra.srv, 1, 0, 1);
            }
            float vp[4] = { vx, vy, vw, vh };
            /* the scene the composite starts from: smoothed for the cel look */
            ID3D11ShaderResourceView* base = g_fx.src.srv;
            if (u.toon3[0] > 0.0f && g_fx.ps[FX_SMOOTH] && fx_tex(&g_fx.sm, color->dxfmt, color->w, color->h))
            {
                fx_draw(g_fx.sm.rtv, g_fx.ps[FX_SMOOTH], vp, &u, &g_fx.src.srv, 1, 0, 0);
                base = g_fx.sm.srv;
            }
            /* the ninth, the depth before the grass for the lines without it (u.toon3.z) and Render visual 26 (the grass in red; the
             * depth itself when this scene has none before it: no grass drawn, nothing differs) */
            const int grass_view = (int)g_fxs.debug == 26, before = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial;
            ID3D11ShaderResourceView* comp_in[9] = { g_fx.src.srv, ao_out->srv, depth->srv, g_fx.b1a.srv, g_fx.b2a.srv, g_fx.rb.srv, base,
                NULL, before && (u.toon3[2] > 0.0f || grass_view) ? g_fx.wdepth->srv : grass_view ? depth->srv : NULL };
            fx_draw((ID3D11RenderTargetView*)target_view(color, 0, 0), g_fx.ps[FX_COMP], vp, &u, comp_in, 9, 0, 0);
            /* clarity: the composite's lead on its own blur, as a gain (a copy to read, its blur at about a quarter size) */
            if (g_fxs.clarity > 0.0f && g_fxs.grade > 0.0f && g_fx.ps[FX_LOWPASS] && g_fx.ps[FX_CLARITY] && fx_tex(&g_fx.cl[0], RGBA, bw, bh) &&
                fx_tex(&g_fx.cl[1], RGBA, bw, bh))
            {
                fx_copy_scene(color);
                fx_pass(&g_fx.cl[0], FX_LOWPASS, &u, &g_fx.src.srv, 1, 0, 0);
                fx_pass(&g_fx.cl[1], FX_GAUSS, &u, &g_fx.cl[0].srv, 1, 2, 0);
                fx_pass(&g_fx.cl[0], FX_GAUSS, &u, &g_fx.cl[1].srv, 1, 0, 2);
                ID3D11ShaderResourceView* clar_in[2] = { g_fx.src.srv, g_fx.cl[0].srv };
                fx_draw((ID3D11RenderTargetView*)target_view(color, 0, 0), g_fx.ps[FX_CLARITY], vp, &u, clar_in, 2, 0, 0);
            }
            /* depth of field, last, on the finished scene: its copy (sharp), the focus, a blurred copy
             * at half size, then the two mixed by each pixel's blur amount */
            if (u.dof[0] >= 1.0f && g_fx.ps[FX_DOFCOMP] && fx_tex(&g_fx.d0, RGBA, (uint32_t)(vw + 1) / 2, (uint32_t)(vh + 1) / 2) &&
                fx_tex(&g_fx.d1, RGBA, g_fx.d0.w, g_fx.d0.h) && fx_tex(&g_fx.foc[0], DXGI_FORMAT_R32_FLOAT, 1, 1) &&
                fx_tex(&g_fx.foc[1], DXGI_FORMAT_R32_FLOAT, 1, 1))
            {
                /* the focus eases about 6 times a second's worth; after a pause it's taken as it is */
                uint64_t now = gfx_now_ns();
                double dt = g_fx.foc_ns ? (double)(now - g_fx.foc_ns) * 1e-9 : 1.0;
                g_fx.foc_ns = now;
                u.dof2[1] = (float)(1.0 - exp(-dt * 6.0));
                u.dof2[2] = dt > 0.5 ? 1.0f : 0.0f;
                FxTex* last = &g_fx.foc[g_fx.foc_i];
                FxTex* focus = &g_fx.foc[g_fx.foc_i ^ 1];
                g_fx.foc_i ^= 1;
                fx_copy_scene(color);
                ID3D11ShaderResourceView* focus_in[2] = { depth->srv, last->srv };
                fx_pass(focus, FX_FOCUS, &u, focus_in, 2, 0, 0);
                ID3D11ShaderResourceView* prep_in[3] = { g_fx.src.srv, depth->srv, focus->srv };
                fx_pass(&g_fx.d0, FX_DOFPREP, NULL, prep_in, 3, 0, 0);
                fx_pass(&g_fx.d1, FX_DOFBLUR, NULL, &g_fx.d0.srv, 1, 0, 0);
                ID3D11ShaderResourceView* dof_in[4] = { g_fx.src.srv, g_fx.d1.srv, depth->srv, focus->srv };
                fx_draw((ID3D11RenderTargetView*)target_view(color, 0, 0), g_fx.ps[FX_DOFCOMP], vp, NULL, dof_in, 4, 0, 0);
            }
            else
                g_fx.foc_ns = 0; /* off: the next time it's on, the focus starts where it is */
        }
    }
    /* anti-aliasing, last on the finished 3D scene and before it is made smaller */
    if (aa && ready && s->proj[11] != 0.0f)
        fx_antialias(color);
    /* FSR, last: the finished scene made larger, read in its place by the frame's draws */
    if (fsr_on && s->proj[11] != 0.0f)
        up_frame(color, depth, s);
    color->scene = 0; /* the effects changed it: its chain is behind (scene_mips) */
    {
        struct SunTrace* t = &g_fx.trace[g_fx.ntrace++ % 1200];
        float inv[16];
        memset(t, 0, sizeof *t);
        t->serial = g_serial, t->live = g_fx.tr_live, t->cached = g_fx.tr_cached, t->skipped = g_fx.tr_skipped, t->gone = g_fx.tr_gone;
        memcpy(t->fog, g_fx.tr_fog, 12);
        t->own = s->sun_dir[3] != 0.0f, t->depth = g_fx.tr_depth, t->dl = g_fx.tr_dl, t->al = g_fx.tr_al;
        t->strength = g_fx.tr_strength, t->day = g_fx.tr_day;
        memcpy(t->sun, g_fx.sunw, 12);
        if (mat_inverse(inv, s->view))
            memcpy(t->cam, inv + 12, 12);
        t->across = g_fx.st_across;
        t->far_r = g_fx.st_far_r, t->near_r = g_fx.st_near_r, t->reach = g_fx.st_reach, t->to = g_fx.st_to;
        t->ran = g_fx.tr_ran, t->hist = g_fx.tr_hist, t->held = g_fx.tr_held, t->rays = g_fx.tr_rays;
        if (g_fxs.fx != 0.0f)
            fx_watch_frame(s, t, color, depth, ready);
        if (g_fx.wait_serial != g_serial) /* the frame's scenes together */
            g_fx.wait_casters = g_fx.wait_building = 0, g_fx.wait_serial = g_serial;
        g_fx.wait_casters += g_fx.tr_live + g_fx.tr_cached + g_fx.tr_skipped, g_fx.wait_building += g_fx.tr_building;
        g_fx.tr_live = g_fx.tr_cached = g_fx.tr_skipped = g_fx.tr_gone = g_fx.tr_building = 0, g_fx.tr_strength = g_fx.tr_day = 0;
        g_fx.tr_ran = g_fx.tr_hist = g_fx.tr_held = 0, g_fx.tr_rays = 0;
    }
    g_fx.st_drawn_this = 0;
    if (g_fxs.fx != 0.0f && g_fxs.sun > 0.0f)
        fx_stats();
    casters_clear();
    fx_unbind();
}

/* --- start-up -------------------------------------------------------------------------------------------------- */
/* the utility passes: the back buffer to the window, VanaCore's interface over it, the frame-rate overlay
 * (gfx_hlsl_util's, at Direct3D 11's registers), and clears of rectangles */
static const char UTIL[] =
    "Texture2D tex : register(t4);\n"
    "SamplerState smp : register(s0);\n"
    "struct PO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "PO present_vs(uint vid : SV_VertexID) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  PO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "float4 present_fs(PO i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1.0); }\n"
    "float4 present_layer_fs(PO i) : SV_Target { return tex.Sample(smp, i.uv); }\n"
    /* the frame sharpened by cas.x (0..1): contrast-adaptive, a negative lobe over the four neighbors that
     * shrinks where the neighborhood is already near black or white (gfx_hlsl_util's present_cas_fs) */
    "cbuffer SU : register(b1) { float4 cas; };\n"
    "float4 present_cas_fs(PO i) : SV_Target {\n"
    "  uint w, h; tex.GetDimensions(w, h);\n"
    "  float2 tx = 1.0 / float2(w, h);\n"
    "  float3 c = tex.Sample(smp, i.uv).rgb;\n"
    "  float3 n = tex.Sample(smp, i.uv - float2(0, tx.y)).rgb, so = tex.Sample(smp, i.uv + float2(0, tx.y)).rgb;\n"
    "  float3 we = tex.Sample(smp, i.uv - float2(tx.x, 0)).rgb, e = tex.Sample(smp, i.uv + float2(tx.x, 0)).rgb;\n"
    "  float3 mn = min(c, min(min(n, so), min(we, e))), mx = max(c, max(max(n, so), max(we, e)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));\n"
    "  float3 lobe = -amp * lerp(0.125, 0.2, saturate(cas.x));\n"
    "  return float4(saturate((c + (n + so + we + e) * lobe) / (1.0 + 4.0 * lobe)), 1.0);\n"
    "}\n"
    "cbuffer CU : register(b1) { float4 clear_color; float4 clear_depth; };\n"
    "float4 clear_vs(uint vid : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), clear_depth.x, 1);\n"
    "}\n"
    "float4 clear_fs() : SV_Target { return clear_color; }\n"
    "cbuffer OU : register(b1) { float4 rect; float scale; uint n; float2 size; uint4 text[8]; };\n"
    "static const uint FONT[17 * 7] = {\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00 };\n"
    "float4 overlay_vs(uint vid : SV_VertexID) : SV_Position {\n"
    "  float2 c = rect.xy + float2((vid & 1) ? rect.z : 0.0, (vid & 2) ? rect.w : 0.0);\n"
    "  return float4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0, 1);\n"
    "}\n"
    "float4 overlay_fs(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 p = (pos.xy - rect.xy) / scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  if (p.x >= 0.0 && cell < int(n) && gx < 5 && gy >= 0 && gy < 7) {\n"
    "    uint ch = text[cell >> 2][cell & 3];\n"
    "    if ((FONT[ch * 7 + uint(gy)] >> (4 - gx)) & 1) return float4(1.0, 0.85, 0.2, 1.0);\n"
    "  }\n"
    "  return float4(0, 0, 0, 0.55);\n"
    "}\n";

static ID3D11VertexShader* util_vs(const char* entry)
{
    ID3DBlob* b = compile(UTIL, entry, "vs_5_0");
    ID3D11VertexShader* s = NULL;
    if (b)
    {
        ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s);
        ID3D10Blob_Release(b);
    }
    return s;
}

static ID3D11PixelShader* util_ps(const char* entry)
{
    ID3DBlob* b = compile(UTIL, entry, "ps_5_0");
    ID3D11PixelShader* s = NULL;
    if (b)
    {
        ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s);
        ID3D10Blob_Release(b);
    }
    return s;
}

static ID3D11BlendState* util_blend(D3D11_BLEND src, D3D11_BLEND dst, int on, UINT8 mask)
{
    D3D11_BLEND_DESC d;
    memset(&d, 0, sizeof d);
    D3D11_RENDER_TARGET_BLEND_DESC* c = &d.RenderTarget[0];
    c->BlendEnable = on ? TRUE : FALSE;
    c->SrcBlend = c->SrcBlendAlpha = src;
    c->DestBlend = c->DestBlendAlpha = dst;
    c->BlendOp = c->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    c->RenderTargetWriteMask = mask;
    ID3D11BlendState* s = NULL;
    ID3D11Device_CreateBlendState(g_dev, &d, &s);
    return s;
}

/* The card the device goes on, in *adapter (NULL: the default one). In Direct3D 12's place after its device was lost: the card it
 * was on (gfx_card_find), not DXGI's pick of the moment - right after a hang that was "AMD Radeon(TM) Graphics", the processor's
 * own, and the game ran at 0-3 fps (an RX 7900 XTX, 2026-11-01 and 11-02); 0 while that card isn't listed
 * (vanacore_gfx_device_recover tries again, then forgets it). Else the high-performance one */
static int pick_adapter(IDXGIAdapter1** adapter)
{
    *adapter = NULL;
    if (gfx_card_known())
        return (*adapter = (IDXGIAdapter1*)gfx_card_find(g_factory)) != NULL;
    IDXGIFactory6* f6 = NULL;
    if (g_factory && SUCCEEDED(IDXGIFactory2_QueryInterface(g_factory, &IID_IDXGIFactory6, (void**)&f6)))
    {
        IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void**)adapter);
        IDXGIFactory6_Release(f6);
    }
    return 1;
}

static int make_device(void)
{
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory2, (void**)&g_factory)))
        g_factory = NULL;
    IDXGIAdapter1* adapter;
    if (!pick_adapter(&adapter))
    {
        if (g_factory)
            IDXGIFactory2_Release(g_factory), g_factory = NULL;
        return 0;
    }
    const int on_the_d3d12_card = gfx_card_known();
    const char* dbg = getenv("FFXI_D3D11_DEBUG");
    UINT flags = dbg && dbg[0] == '1' ? D3D11_CREATE_DEVICE_DEBUG : 0;
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter*)adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
        levels, 2, D3D11_SDK_VERSION, &g_dev, &g_level, &g_ctx);
    if (hr == E_INVALIDARG) /* a runtime without 11.1 */
        hr = D3D11CreateDevice((IDXGIAdapter*)adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
            levels + 1, 1, D3D11_SDK_VERSION, &g_dev, &g_level, &g_ctx);
    char name[128] = "?";
    if (adapter)
    {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad)))
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
        /* kept for gfx_video_memory */
        if (g_adapter3)
            IDXGIAdapter3_Release(g_adapter3), g_adapter3 = NULL;
        if (FAILED(IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter3, (void**)&g_adapter3)))
            g_adapter3 = NULL;
        IDXGIAdapter1_Release(adapter);
    }
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: no Direct3D 11 device (%08lx)\n", (unsigned long)hr);
        g_dev = NULL, g_ctx = NULL;
        return 0;
    }
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_ID3D11Device1, (void**)&g_dev1)))
        g_dev1 = NULL;
    if (FAILED(ID3D11DeviceContext_QueryInterface(g_ctx, &IID_ID3D11DeviceContext1, (void**)&g_ctx1)))
        g_ctx1 = NULL;
    /* what Direct3D 11.1 lets the rings do: bind a range of the uniforms' ring, and write the rings a
     * draw at a time */
    D3D11_FEATURE_DATA_D3D11_OPTIONS o;
    memset(&o, 0, sizeof o);
    ID3D11Device_CheckFeatureSupport(g_dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o);
    if (!o.ConstantBufferOffsetting || !o.MapNoOverwriteOnDynamicConstantBuffer)
        g_ctx1 = NULL;
    fprintf(stderr, "[recomp] gfx: Direct3D 11 (feature level %s) on %s%s%s%s\n", g_level >= D3D_FEATURE_LEVEL_11_1 ? "11_1" : "11_0", name,
        on_the_d3d12_card ? " (the card Direct3D 12 was on)" : "",
        g_ctx1 ? "" : "; uniforms a buffer a draw (no Direct3D 11.1 ranges)",
        o.MapNoOverwriteOnDynamicBufferSRV ? "" : "; vertex bytes a buffer a draw");
    g_vring.discard_only = !o.MapNoOverwriteOnDynamicBufferSRV;
    snprintf(g_adapter, sizeof g_adapter, "%s", name);
    return 1;
}

void gfx_set_vsync(int on) { g_vsync = on, g_vsync_asked = 1; }
const char* gfx_adapter_name(void) { return g_adapter; }
int gfx_video_memory(unsigned long long* used, unsigned long long* budget)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO vm;
    if (!g_adapter3 || !g_dev || FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(g_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
        return 0;
    *used = vm.CurrentUsage, *budget = vm.Budget;
    return 1;
}

int gfx_init(void* window, int vsync)
{
    /* up already (VanaCore brings the device up before the game has a window): the swap chain goes on this one */
    if (g_dev)
    {
        /* (the settings were applied before the game's CreateDevice: its windowed device's vsync is only the default) */
        if (!g_vsync_asked)
            g_vsync = vsync;
        if (window && !g_swap)
        {
            g_window = (HWND)window;
            make_swap_chain(g_window);
        }
        return 1;
    }
    gfx_hlsl_d3d11 = 1; /* the generated shaders in Direct3D 11's form */
    if (!make_device())
        return 0;
    InitializeCriticalSection(&g_cache_file_lock);
    g_vring.size = VCHUNK_SIZE, g_vring.bind = D3D11_BIND_SHADER_RESOURCE, g_vring.misc = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    g_iring.size = ICHUNK_SIZE, g_iring.bind = D3D11_BIND_INDEX_BUFFER, g_iring.misc = 0;
    int ok = chunk_make(&g_vring, 0, 0) && chunk_make(&g_iring, 0, 0) &&
        ring_make(&g_cring, g_ctx1 ? CRING_SIZE : U_CONSTANTS * 16, D3D11_BIND_CONSTANT_BUFFER, 0);
    D3D11_BUFFER_DESC ud = { 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &ud, NULL, &g_util_cb));
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Device_CreateQuery(g_dev, &qd, &g_idle);
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_linear));
    g_blend_none = util_blend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, 0, D3D11_COLOR_WRITE_ENABLE_ALL);
    g_blend_nocolor = util_blend(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, 0, 0);
    g_blend_alpha = util_blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, 1, D3D11_COLOR_WRITE_ENABLE_ALL);
    g_blend_premul = util_blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, 1, D3D11_COLOR_WRITE_ENABLE_ALL);
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_raster_plain);
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_raster_scissor);
    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_depth_off);
    g_present_vs = util_vs("present_vs");
    g_present_ps = util_ps("present_fs");
    g_layer_ps = util_ps("present_layer_fs");
    g_clear_vs = util_vs("clear_vs");
    g_clear_ps = util_ps("clear_fs");
    g_overlay_vs = util_vs("overlay_vs");
    g_overlay_ps = util_ps("overlay_fs");
    g_cas_ps = util_ps("present_cas_fs");
    /* the scene effects': their uniforms, the sun's matrix for the casters, the shadow lookups' sampler */
    D3D11_BUFFER_DESC fd = { (sizeof(FxU) + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &fd, NULL, &g_fx.cb));
    fd.ByteWidth = 80; /* sun_m, sun_f */
    ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &fd, NULL, &g_fx.sun_cb));
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(g_dev, &sd, &g_fx.cmp));
    fx_config();
    if (!ok || !g_present_vs || !g_present_ps || !g_clear_vs || !g_clear_ps)
    {
        fprintf(stderr, "[recomp] gfx: Direct3D 11 set-up failed\n");
        ID3D11DeviceContext_Release(g_ctx);
        ID3D11Device_Release(g_dev);
        g_dev = NULL, g_ctx = NULL;
        return 0;
    }
    const char* show = getenv("FFXI_FPS");
    g_overlay = !(show && show[0] == '0');
    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = 0; /* the frame profile is Direct3D 12's */
    gfx_watching = prof && prof[0] && prof[0] != '0'; /* the flicker watch is both's (a recording turns it on: vanacore_gfx_record) */
    bound_forget();
    if (!g_sync_pipelines)
        prewarm_shaders();
    g_vsync = vsync;
    if (window)
    {
        g_window = (HWND)window;
        make_swap_chain(g_window);
    }
    return 1;
}

GFX_BACKEND_TABLE(gfx11_backend)
