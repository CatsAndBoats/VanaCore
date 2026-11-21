/* Direct3D 8 for 64-bit hosts (R3.1): the interface FFXiMain draws through, implemented by us.
 *
 * The translated game still speaks D3D8 - Direct3DCreate8, then COM calls on IDirect3DDevice8 and
 * its resources - so this file is the D3D8 front end the Metal renderer (R3.2) sits under.
 * It owns what the game can observe:
 *
 *   - the COM objects (guest memory: a vtable pointer and our object index; every method is a
 *     thunk named "d3d8.dll!IDirect3DDevice8::SetRenderState" and so on, so a method the game
 *     reaches that is not written yet traps with its name),
 *   - resource memory the game locks (textures, surfaces, vertex and index buffers live in the
 *     guest window, allocated on first lock),
 *   - device state with D3D8 semantics - defaults, readback (the game calls GetTransform and
 *     GetViewport millions of times), state-block recording, capture and apply, the references
 *     bound resources hold, the stream/index reset after the ...UP draws.
 *
 * What is drawn goes to the graphics back end (gfx.h: Metal on macOS, R3.2): every texture and
 * render target has a GfxTex beside its guest memory, and Clear, the Draw* calls and Present turn
 * the device state into the back end's clears and draw packets (see "drawing" below).
 *
 * Which copy of a resource is current: for textures the game fills (the managed and system pools)
 * the guest memory is, and a level is uploaded when it is next drawn with after a lock; for render
 * targets and depth the GPU is, and a lock reads the level back (and an unlock writes it again).
 * Vertex and index buffers stay in guest memory; each draw copies the range it uses.
 *
 * The method set is the one the game was measured to use (specs/FFXiMain.2026-08-22.d3d8-surface.txt); vtable order is the D3D8 ABI, as the R1 proxy
 * (d3d8proxy.cpp) lists it. */
#include <math.h>
#ifdef VANACORE
#include <float.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "d3d8.h"
#include "d3d8_sun.h"
#include "floors.h"
#include "gfx.h"
#include "grass.h"
#include "zonemesh.h"
#include "gthread.h"
#include "snow.h"
#include "sky.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000Eu

/* D3DFORMAT */
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_R5G6B5 23u
#define FMT_X1R5G5B5 24u
#define FMT_A1R5G5B5 25u
#define FMT_A4R4G4B4 26u
#define FMT_A8 28u
#define FMT_L8 50u
#define FMT_A8L8 51u
#define FMT_V8U8 60u
#define FMT_D16 80u
#define FMT_D24S8 75u
#define FMT_D24X8 77u
#define FMT_INDEX16 101u
#define FMT_INDEX32 102u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')
#define FMT_ATI2 FOURCC('A', 'T', 'I', '2') /* BC5: a normal map's X and Y */

/* D3DRESOURCETYPE */
#define RT_SURFACE 1u
#define RT_TEXTURE 3u
#define RT_VOLUMETEXTURE 4u
#define RT_CUBETEXTURE 5u
#define RT_VERTEXBUFFER 6u
#define RT_INDEXBUFFER 7u

#define USAGE_RENDERTARGET 1u
#define USAGE_DEPTHSTENCIL 2u

/* D3DCAPS8 (212 bytes) as Windows 11's d3d8 reports it for a HAL device (NVIDIA RTX A4500,
 * captured 2026-09-24): vertex shader 1.1, pixel shader 1.4, 8 texture stages, 16384 textures,
 * 8 lights, 16 streams. Everything here is within what Metal on an M1 provides. */
static const uint32_t CAPS8[53] = {
    0x00000001, 0x00000000, 0x00020000, 0x200a0000, 0x00000420, 0x8000000f, 0x00000001, 0x001bbef0, 0x00000ef2,
    0x00f37191, 0x000000ff, 0x00001fff, 0x00001fff, 0x000000ff, 0x00084208, 0x0001ecc5, 0x03030700, 0x03030300,
    0x03030300, 0x0000003f, 0x0000003f, 0x0000001f, 0x00004000, 0x00004000, 0x00000800, 0x00002000, 0x00004000,
    0x00000010, 0x501502f9, 0xccbebc20, 0xccbebc20, 0x4cbebc20, 0x4cbebc20, 0x00000000, 0x000000ff, 0x00180008,
    0x03feffff, 0x00000008, 0x00000008, 0x0000003b, 0x00000008, 0x00000008, 0x00000004, 0x00000000, 0x46000000,
    0x00ffffff, 0x00ffffff, 0x00000010, 0x000000ff, 0xfffe0101, 0x00000100, 0xffff0104, 0x477fe000,
};

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* FFXI_DRAWLOG (see cap_present) */
/* FFXI_DRAWLOG's path, or d3d8_set_drawlog's; empty: no capture. The renderer's own copy, so nothing outside can hand it a
 * pointer (2026-10-10: a Lua buffer's address was written over the old pointer and freed before Present read it). */
static char g_cap_path[1024];
static void scene_forget(GfxTex* t);
static FILE* g_cap;
static uint32_t g_cap_esp, g_cap_frame, g_cap_n;
static float g_bg_rects[16][4]; /* the window backgrounds drawn so far this frame (their frames are found beside them) */
static unsigned g_bg_count;
static void cap_present(void);
static void scene_present(void);
static uint32_t g_water_tiles; /* VanaCore's water drawn since the last Present (water_shade) */
static void water_feet_turn(void); /* d3d8_water.h: the waterfalls' feet seen this frame are the next frame's */
static void water_kept_drop(const GfxTex* t, const GfxBuf* b); /* d3d8_water.h: a draw kept for the water pass that reads them is dropped */
static uint32_t g_water_shape_tiles; /* of them, tiles found by their shape: the game's layers over them go (water_layer) */
static uint32_t g_water_kept_n; /* of them, kept for the water pass (d3d8_water.h water_keep), drawn at the scene's end (water_pass) */
static void water_pass(GfxTex* color, GfxTex* depth, const GfxScene* s);
static void rain_pass(GfxTex* color, GfxTex* depth, const GfxScene* s);
static void rain_set_zone(int zone);
static void water_set_zone(int zone); /* d3d8_water_level.h: a ferry's zone (g_water_ship) */
static int rain_game_skip(const GfxDraw* d, uint32_t tex0);
static void water_device_lost(void); /* the collision's water's buffer (d3d8_water.h) */
static void rain_mask_lost(void);     /* the wet ground's mask (d3d8_rain.h) */

/* --- formats ----------------------------------------------------------------------------------------- */
static uint32_t fmt_block(uint32_t f) /* bytes per 4x4 block, or 0 */
{
    return f == FMT_DXT1 ? 8u : (f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5 || f == FMT_ATI2) ? 16u : 0u;
}

static uint32_t fmt_bytes(uint32_t f) /* bytes per pixel for the uncompressed formats */
{
    switch (f)
    {
    case FMT_A8:
    case FMT_L8:
    case 41: /* P8 */
    case 52: /* A4L4 */
        return 1;
    case FMT_R5G6B5:
    case FMT_X1R5G5B5:
    case FMT_A1R5G5B5:
    case FMT_A4R4G4B4:
    case FMT_A8L8:
    case FMT_V8U8:
    case FMT_D16:
    case 73: /* D15S1 */
    case FMT_INDEX16:
        return 2;
    default: return 4;
    }
}

static uint32_t fmt_pitch(uint32_t f, uint32_t w)
{
    uint32_t b = fmt_block(f);
    return b ? ((w + 3) / 4 ? (w + 3) / 4 : 1) * b : w * fmt_bytes(f);
}

static uint32_t fmt_size(uint32_t f, uint32_t w, uint32_t h)
{
    return fmt_pitch(f, w) * (fmt_block(f) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h);
}

/* --- objects ----------------------------------------------------------------------------------------- */
enum
{
    O_FREE,
    O_D3D,
    O_DEVICE,
    O_TEXTURE,
    O_CUBE,
    O_VB,
    O_IB,
    O_SURFACE,
};

typedef struct Obj
{
    uint8_t kind, implicit; /* implicit: the swap chain's back buffer, the auto depth buffer */
    int32_t refs;
    uint32_t guest;     /* the COM pointer the game holds */
    uint32_t container; /* surfaces of a texture: its COM pointer; their references are the texture's */
    uint32_t format, usage, pool, width, height, levels, size, fvf;
    uint32_t mem; /* guest memory, on first lock */
    uint32_t* subs; /* textures: level surfaces (cubes: face * levels + level) */
    uint32_t nsubs;
    uint32_t lod, priority;
    GfxTex* gpu;         /* textures, render targets, depth surfaces */
    GfxBuf* gbuf;        /* static vertex and index buffers: the GPU's copy */
    uint8_t gbuf_dirty;  /* ... which a lock that writes makes stale */
    uint8_t face, level; /* surfaces of a texture: where in it */
    uint8_t dirty;       /* surfaces of a texture: guest memory is newer than the GPU's copy */
    uint8_t gpu_locked;  /* a GPU-owned surface is locked: written back at unlock */
    GfxTex* repl;        /* textures: a texture pack's replacement, drawn with instead of gpu */
    uint32_t repl_pad;   /* ... whose glyph quads are drawn this many texels wider each side */
    uint32_t repl_pad_min; /* ... when they are at least this many texels tall */
    const struct PackEntry* repl_entry; /* ... and its glyph table, if it has one */
    GfxTex* nrm;         /* textures: a texture pack's normal map for it (<hash>_<w>x<h>.n.dds), BC5 */
    uint8_t fwd;         /* textures: ZBIAS steps added to what's drawn with it (kForward) */
    uint32_t pw, ph;     /* render targets drawn at another size than the game's (the window's, the scene's
                          * multiple of it): gpu's size, where the game sees width x height; 0 when the same */
    uint8_t grow;        /* GROW_*: what pw x ph follows */
    uint64_t hash;       /* textures: the hash of the first level (texture_hash), 0 until the grass asked */
    uint8_t water;       /* vertex buffers: made while the game loaded a water model (d3d8_water_loading) */
} Obj;
#ifdef VANACORE
static uint64_t texture_hash(Obj* t);
#endif

/* A render target (a texture's or a standalone surface: the back buffer, its depth buffer) drawn at the size the
 * frame is shown at (GROW_SCREEN), or the frame's 3D scene at a multiple of it ([graphics] background, DLSS:
 * GROW_SCENE), or a small one the game shrinks a large one into (the trust and avatar glow's 256x256) at a quarter of
 * the largest's size (GROW_GLOW). Sizes change between frames (resize_targets). */
enum { GROW_SCREEN = 1, GROW_SCENE = 2, GROW_GLOW = 4 };

/* FFXI's glow round trusts and avatars: the character drawn alone into a large target (g_glow_src), shrunk into
 * a small one (g_glow_dst, 256x256) and laid back over the scene tinted, seven times a few pixels apart; then the
 * small picture laid down where it's solid, and the character itself over that. [graphics] glow (d3d8_set_glow):
 * 64-4096 the game's, the small target drawn at that size (GROW_GLOW); 0 off: no shrinking, no tinted copies, the
 * solid copy from the full-size picture (the character crisp, no glow); -1 custom: the same with VanaCore's glow
 * (gfx_glow) in place of the copies, in their tint. (glow_draw) */
static int g_glow_mode = 1024;
static uint32_t g_glow_dst, g_glow_src; /* guest pointers: the small target's texture, the large one's */
static int g_glow_laid; /* VanaCore's glow is down for this set of tinted copies (once for the seven): until the next
                         * shrinking or solid copy, or the frame's end */
/* [graphics] characters.brightness (d3d8_set_character_light): the light on the characters times this (character_light) */
static float g_char_light = 1.0f;
/* the surface the frame's 3D scene was last drawn into and the depth surface with it (guest pointers) */
static uint32_t g_scene_obj, g_scene_ds;
/* the depth surface of the world's last counted 3D draw, for the grass drawn when the scene is finished */
static GfxTex* g_grass_depth;

static Obj* g_objs;
static uint32_t g_nobjs, g_capobjs, g_free_hint = 1;
static uint32_t g_vtbl[O_SURFACE + 1];

static uint32_t obj_new(int kind)
{
    uint32_t i = g_free_hint;
    while (i < g_nobjs && g_objs[i].kind != O_FREE)
        i++;
    if (i >= g_nobjs)
    {
        if (g_nobjs + 2 > g_capobjs)
        {
            g_capobjs = g_capobjs ? g_capobjs * 2 : 1024;
            g_objs = (Obj*)realloc(g_objs, g_capobjs * sizeof *g_objs);
        }
        if (!g_nobjs)
            g_objs[g_nobjs++].kind = O_FREE; /* index 0 is never an object */
        i = g_nobjs++;
    }
    g_free_hint = i + 1;
    Obj* o = &g_objs[i];
    memset(o, 0, sizeof *o);
    o->kind = (uint8_t)kind;
    o->refs = 1;
    o->guest = gheap_alloc(8, 1);
    wr32(o->guest, g_vtbl[kind]);
    wr32(o->guest + 4, i);
    return o->guest;
}

static Obj* obj(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i && i < g_nobjs && g_objs[i].kind != O_FREE && g_objs[i].guest == p ? &g_objs[i] : NULL;
}

static uint32_t obj_release(uint32_t p);
#ifdef VANACORE
static void private_free_all(uint32_t guest);
#endif

static void obj_destroy(Obj* o)
{
#ifdef VANACORE
    private_free_all(o->guest);
#endif
    if (o->guest == g_scene_obj)
        g_scene_obj = 0;
    if (o->guest == g_scene_ds)
        g_scene_ds = 0;
    for (uint32_t i = 0; i < o->nsubs; ++i)
    {
        Obj* s = obj(o->subs[i]);
        if (s)
        {
            s->container = 0;
            s->refs = 1;
            obj_release(s->guest);
        }
    }
    free(o->subs);
    scene_forget(o->gpu);
    water_kept_drop(o->gpu, o->gbuf);
    gfx_tex_destroy(o->gpu);
    gfx_buf_destroy(o->gbuf);
    if (o->mem)
        gheap_free(o->mem);
    uint32_t idx = rd32(o->guest + 4);
    wr32(o->guest, 0);
    gheap_free(o->guest);
    o->kind = O_FREE;
    if (idx < g_free_hint)
        g_free_hint = idx;
}

static uint32_t obj_addref(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_addref(o->container);
    return (uint32_t)++o->refs;
}

static uint32_t obj_release(uint32_t p)
{
    Obj* o = obj(p);
    if (!o)
        return 0;
    if (o->container)
        return obj_release(o->container);
    if (o->refs > 0)
        o->refs--;
    if (!o->refs && !o->implicit && o->kind != O_DEVICE && o->kind != O_D3D)
    {
        obj_destroy(o);
        return 0;
    }
    return (uint32_t)o->refs;
}

/* a pointer the device or a state block holds: references the new one, releases the old */
static void bind(uint32_t* slot, uint32_t p)
{
    if (*slot == p)
        return;
    if (p)
        obj_addref(p);
    uint32_t old = *slot;
    *slot = p;
    if (old)
        obj_release(old);
}

static uint32_t obj_mem(Obj* o)
{
    if (!o->mem)
        o->mem = gheap_alloc(o->size ? o->size : 16, 1);
    return o->mem;
}

static uint32_t new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t pool, uint32_t container)
{
    uint32_t p = obj_new(O_SURFACE);
    Obj* s = obj(p);
    s->format = fmt;
    s->width = w;
    s->height = h;
    s->usage = usage;
    s->pool = pool;
    s->size = fmt_size(fmt, w, h);
    s->container = container;
    if (!container && (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)))
        s->gpu = gfx_tex_create(GFX_TEX_2D, fmt, w, h, 1, (usage & USAGE_DEPTHSTENCIL) ? GFX_USE_DEPTH : GFX_USE_RT);
    return p;
}

/* the GPU texture behind a surface, and which face and level of it */
static GfxTex* surface_gpu(const Obj* s, uint32_t* face, uint32_t* level)
{
    if (s->container)
    {
        const Obj* t = obj(s->container);
        *face = s->face, *level = s->level;
        return t ? t->gpu : NULL;
    }
    *face = *level = 0;
    return s->gpu;
}

/* render targets and depth: the GPU holds the current pixels, not guest memory */
static int gpu_owned(const Obj* s)
{
    uint32_t f, l;
    return (s->usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && surface_gpu(s, &f, &l);
}

static void mark_dirty(Obj* s)
{
    s->dirty = 1;
    Obj* t = s->container ? obj(s->container) : NULL;
    if (t)
        t->dirty = 1;
}

/* --- texture packs -------------------------------------------------------------------------------------
 * d3d8_texture_pack(folder): high-resolution replacements for the game's own textures (the font, say), made by
 * tools/make_texpack.py. Each is <folder>/<hash>_<w>x<h>.dds: the FNV-1a hash of the first level of a w x h texture
 * as the game uploads it, and the replacement, DXT1/3/5 with its mipmaps. The replacement keeps the original's layout
 * at a larger size, so the game's texture coordinates (fractions of the texture) still land on the same art; it is
 * drawn with instead of the original, filtered with its mipmaps, and dropped again if the game writes something else
 * into the texture. Only textures of a size some entry has are hashed. FFXI_TEXLOG=1 hashes and logs every texture
 * instead, to find the one to replace; with FFXI_TEXDUMP=<folder> as well, each level hashed is written there as
 * <hash>.bin, in D3D's layout. */
typedef struct PackEntry
{
    uint64_t hash;
    uint32_t w, h, pad, pad_min; /* _pad<N>[min<M>] in the name: texels each glyph quad drawn with it is
                                  * widened by (only quads at least M texels tall) */
    float* glyphs;               /* <entry>.glyphs beside it: each glyph's ink box (x0 x1 y0 y1, texels) */
    uint32_t nglyphs;
    char* path;
    GfxTex* tex;
    uint8_t tried; /* loaded, or failed to */
} PackEntry;

static PackEntry* g_pack;
static uint32_t g_npack;

/* Zone pieces the game hangs so close to a wall that the wall's decals (ZBIAS 8) come through them where they touch
 * it, keyed like a pack's entries: whatever is drawn with the texture gets these ZBIAS steps on top of its own, so it
 * keeps ahead of the decals at any distance (build_draw). Only its depth moves: nothing shifts on screen. */
static const struct
{
    uint64_t hash;
    uint32_t w, h, steps;
} kForward[] = {
    { 0xc96650b7dfe310b2ull, 256, 512, 16 }, /* Bastok Markets' banners (and their shading, ZBIAS 8 of its own) */
};

/* VanaCore's normal maps: <folder>/<hash>_<w>x<h>.n.dds beside a pack's entries (tools: xi tex normals), keyed like
 * them by the hash of the first level the game uploads. The game draws the texture as it always did; the lit pixel
 * function bends the surface normal by the map (gfx_hlsl.c emit_normal_map). FFXI_NORMALMAP: 0 off, 1 on (a map
 * with Y up), 2 shows the bent normal as the colour, 3 on with Y down; 1 when unset. */
typedef struct NormEntry
{
    uint64_t hash;
    uint32_t w, h;
    char* path;
    GfxTex* tex;
    uint8_t tried;
} NormEntry;
static NormEntry* g_nrm;
static uint32_t g_nnrm;
static int g_nmap_mode = -1;
static float g_sun_view[3]; /* the last directional light a draw lit by, in view space, toward it (normal maps on shader draws) */

static int normal_map_mode(void)
{
    if (g_nmap_mode < 0)
        g_nmap_mode = getenv("FFXI_NORMALMAP") && getenv("FFXI_NORMALMAP")[0] ? atoi(getenv("FFXI_NORMALMAP")) : 1;
    return g_nmap_mode;
}
static int g_texlog = -1;

void d3d8_texture_pack(const char* dir)
{
    PlatDir* d = plat_dir_open(dir);
    if (!d)
    {
        rt_log("[recomp] textures: cannot read %s\n", dir);
        return;
    }
    uint32_t added = 0;
    for (const char* name; (name = plat_dir_next(d));)
    {
        unsigned long long hash;
        uint32_t w, h;
        char ext[8];
        uint32_t pad = 0, pad_min = 0;
        if (sscanf(name, "%16llx_%ux%u_pad%umin%u.%7s", &hash, &w, &h, &pad, &pad_min, ext) != 6 &&
            (pad_min = 0, sscanf(name, "%16llx_%ux%u_pad%u.%7s", &hash, &w, &h, &pad, ext) != 5) &&
            (pad = 0, sscanf(name, "%16llx_%ux%u.%7s", &hash, &w, &h, ext) != 4))
            continue;
        if (!strcmp(ext, "n.dds") && strlen(name) >= 16 && !pad && !pad_min)
        {
            size_t pn = strlen(dir) + strlen(name) + 2;
            char* np = (char*)malloc(pn);
            snprintf(np, pn, "%s%c%s", dir, plat_path_sep, name);
            g_nrm = (NormEntry*)realloc(g_nrm, (g_nnrm + 1) * sizeof *g_nrm);
            g_nrm[g_nnrm++] = (NormEntry){ hash, w, h, np, NULL, 0 };
            continue;
        }
        if (strcmp(ext, "dds") || strlen(name) < 16)
            continue;
        g_pack = (PackEntry*)realloc(g_pack, (g_npack + 1) * sizeof *g_pack);
        size_t n = strlen(dir) + strlen(name) + 2;
        char* path = (char*)malloc(n);
        snprintf(path, n, "%s%c%s", dir, plat_path_sep, name);
        PackEntry e = { hash, w, h, pad, pad_min, NULL, 0, path, NULL, 0 };
        char gp[1100];
        snprintf(gp, sizeof gp, "%.*s.glyphs", (int)(strlen(path) - 4), path);
        FILE* gf = fopen(gp, "r");
        float b[4];
        while (gf && fscanf(gf, "%f %f %f %f", &b[0], &b[1], &b[2], &b[3]) == 4)
        {
            e.glyphs = (float*)realloc(e.glyphs, (e.nglyphs + 1) * 4 * sizeof(float));
            memcpy(e.glyphs + 4 * e.nglyphs++, b, sizeof b);
        }
        if (gf)
            fclose(gf);
        g_pack[g_npack++] = e;
        added++;
    }
    plat_dir_close(d);
    rt_log("[recomp] textures: %u replacement%s from %s\n", added, added == 1 ? "" : "s", dir);
}

/* A DDS file's texture: DXT1/3/5 only, every level in the file. NULL if it is not one. */
static GfxTex* load_dds(const char* path)
{
    size_t size = 0;
    unsigned char* f = plat_read_file(path, &size);
    GfxTex* t = NULL;
    uint32_t fmt = 0, w = 0, h = 0, levels = 0;
    if (f && size >= 128 && !memcmp(f, "DDS ", 4) && (f[80] & 4))
    {
        h = (uint32_t)f[12] | f[13] << 8 | f[14] << 16 | (uint32_t)f[15] << 24;
        w = (uint32_t)f[16] | f[17] << 8 | f[18] << 16 | (uint32_t)f[19] << 24;
        levels = (uint32_t)f[28] | f[29] << 8;
        fmt = FOURCC(f[84], f[85], f[86], f[87]);
    }
    uint32_t blk = fmt_block(fmt);
    if (blk && w && h && w <= 16384 && h <= 16384)
    {
        levels = levels ? levels : 1;
        size_t off = 128, need = 0;
        for (uint32_t l = 0; l < levels; ++l)
            need += (size_t)((((w >> l) ? (w >> l) : 1) + 3) / 4) * ((((h >> l) ? (h >> l) : 1) + 3) / 4) * blk;
        if (off + need <= size && (t = gfx_tex_create(GFX_TEX_2D, fmt, w, h, levels, GFX_USE_SAMPLE)))
            for (uint32_t l = 0; l < levels; ++l)
            {
                uint32_t lw = (w >> l) ? (w >> l) : 1, lh = (h >> l) ? (h >> l) : 1;
                gfx_tex_upload(t, 0, l, f + off, (lw + 3) / 4 * blk);
                off += (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * blk;
            }
    }
    if (!t)
        rt_log("[recomp] textures: %s is not a DXT1/3/5 DDS with its levels; not used\n", path);
    free(f);
    return t;
}

/* The replacement for a texture whose first level was just uploaded, or NULL; *pad its entry's, *fwd its kForward steps. */
static GfxTex* texture_replacement(const Obj* t, const Obj* level0, uint32_t* pad, uint32_t* pad_min, const PackEntry** entry,
    GfxTex** nrm, uint8_t* fwd)
{
    *pad = *pad_min = 0, *entry = NULL, *nrm = NULL, *fwd = 0;
    if (g_texlog < 0)
        g_texlog = getenv("FFXI_TEXLOG") && getenv("FFXI_TEXLOG")[0] == '1';
    if (t->kind != O_TEXTURE)
        return NULL;
    int sized = g_texlog;
    for (uint32_t i = 0; i < g_npack && !sized; ++i)
        sized = g_pack[i].w == t->width && g_pack[i].h == t->height;
    for (uint32_t i = 0; i < g_nnrm && !sized; ++i)
        sized = g_nrm[i].w == t->width && g_nrm[i].h == t->height;
    for (size_t i = 0; i < sizeof kForward / sizeof kForward[0] && !sized; ++i)
        sized = kForward[i].w == t->width && kForward[i].h == t->height;
    if (!sized)
        return NULL;
    uint32_t pitch = fmt_pitch(level0->format, level0->width), blk = fmt_block(level0->format);
    size_t n = (size_t)pitch * (blk ? (level0->height + 3) / 4 : level0->height);
    const uint8_t* p = GUEST_PTR(level0->mem);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    for (size_t i = 0; i < sizeof kForward / sizeof kForward[0]; ++i)
        if (kForward[i].hash == hash && kForward[i].w == t->width && kForward[i].h == t->height)
        {
            *fwd = (uint8_t)kForward[i].steps;
            rt_log("[recomp] textures: %ux%u %016llx drawn %u ZBIAS steps nearer\n", t->width, t->height, (unsigned long long)hash,
                kForward[i].steps);
        }
    for (uint32_t i = 0; i < g_nnrm && normal_map_mode(); ++i)
    {
        NormEntry* e = &g_nrm[i];
        if (e->hash != hash || e->w != t->width || e->h != t->height)
            continue;
        if (!e->tried)
        {
            e->tried = 1;
            e->tex = load_dds(e->path);
            if (e->tex)
                rt_log("[recomp] textures: %ux%u %016llx has the normal map %s\n", t->width, t->height, (unsigned long long)hash, e->path);
        }
        *nrm = e->tex;
        break;
    }
    for (uint32_t i = 0; i < g_npack; ++i)
    {
        PackEntry* e = &g_pack[i];
        if (e->hash != hash || e->w != t->width || e->h != t->height)
            continue;
        if (!e->tried)
        {
            e->tried = 1;
            e->tex = load_dds(e->path);
            if (e->tex)
                rt_log("[recomp] textures: %ux%u %016llx replaced by %s\n", t->width, t->height, (unsigned long long)hash, e->path);
        }
        *pad = e->pad, *pad_min = e->pad_min, *entry = e;
        return e->tex;
    }
    if (g_texlog)
        rt_log("[recomp] textures: %ux%u format %08x hash %016llx texture %08x (no replacement)\n", t->width, t->height,
            t->format, (unsigned long long)hash, t->guest);
    if (g_texlog && getenv("FFXI_TEXDUMP")) /* ... and the level itself, to make a replacement from */
    {
        char dp[512];
        snprintf(dp, sizeof dp, "%s/%016llx.bin", getenv("FFXI_TEXDUMP"), (unsigned long long)hash);
        FILE* df = fopen(dp, "rb");
        if (df) /* dumped already */
            fclose(df);
        else if ((df = fopen(dp, "wb")) != NULL)
        {
            fwrite(p, 1, n, df), fclose(df);
            snprintf(dp, sizeof dp, "%s/index.txt", getenv("FFXI_TEXDUMP"));
            if ((df = fopen(dp, "a")) != NULL)
                fprintf(df, "%016llx %ux%u %08x\n", (unsigned long long)hash, t->width, t->height, t->format), fclose(df);
        }
    }
    return NULL;
}

/* A texture about to be drawn with: levels written since their last upload go up first. */
static GfxTex* texture_for_draw(uint32_t p, int* kind)
{
    Obj* t = obj(p);
    if (!t || !t->gpu || (t->kind != O_TEXTURE && t->kind != O_CUBE))
        return NULL;
    if (t->dirty)
    {
        for (uint32_t i = 0; i < t->nsubs; ++i)
        {
            Obj* s = obj(t->subs[i]);
            if (s && s->dirty && s->mem && !gpu_owned(s))
            {
                gfx_tex_upload(t->gpu, s->face, s->level, GUEST_PTR(s->mem), fmt_pitch(s->format, s->width));
                if (i == 0)
                    t->repl = texture_replacement(t, s, &t->repl_pad, &t->repl_pad_min, &t->repl_entry, &t->nrm, &t->fwd);
            }
            if (s)
                s->dirty = 0;
        }
        t->dirty = 0;
    }
    *kind = t->kind == O_CUBE ? 2 : 1;
    return t->repl ? t->repl : t->gpu;
}

/* A stage of a draw has a texture the game set but nothing to draw it with (texture_for_draw): the stage reads opaque
 * white. Why, in the log (the first ones): the minimap drawn white at times was suspected of it. */
static void unresolved_texture(uint32_t p, int stage)
{
    static uint32_t said;
    if (said >= 40)
        return;
    said++;
    Obj* t = obj(p);
    rt_log("[recomp] d3d8: a draw's stage %d texture %08x draws as white: %s (%ux%u format %u, pool %u)%s\n", stage, p,
        !t ? "no such object" : !t->gpu ? "no GPU texture" : "not a texture", t ? t->width : 0, t ? t->height : 0,
        t ? t->format : 0, t ? t->pool : 0, said == 40 ? " (the rest aren't logged)" : "");
}

#ifdef VANACORE
/* Ashita's Minimap draws its map as the only screen-space draw of FVF 0x344 (stage 0 the map, stage 1 its mask), and it
 * comes out white at times (logging in, Abyssea's conflux warps). Each time what that draw is drawn with changes, the log
 * says what it is now: the stages as drawn, alpha test, fog, specular, blending, culling and texture coordinates. */
static void minimap_note(const GfxDraw* d)
{
    static GfxFsKey last_fs;
    static GfxPipeKey last_pipe;
    static uint8_t last_tci[3], last_ttf[3], last_cull, last_zenable;
    static uint32_t said;
    if (said >= 200 || (!memcmp(&last_fs, &d->fs, sizeof last_fs) && !memcmp(&last_pipe, &d->pipe, sizeof last_pipe) &&
            !memcmp(last_tci, d->vs.tci, 3) && !memcmp(last_ttf, d->vs.ttf, 3) && last_cull == d->cull &&
            last_zenable == d->depth.zenable))
        return;
    last_fs = d->fs, last_pipe = d->pipe, last_cull = d->cull, last_zenable = d->depth.zenable;
    memcpy(last_tci, d->vs.tci, 3), memcpy(last_ttf, d->vs.ttf, 3);
    said++;
    char st[3][96] = { "", "", "" };
    for (int i = 0; i < 3 && i < d->fs.nstages; ++i)
    {
        const GfxStage* s = &d->fs.st[i];
        snprintf(st[i], sizeof st[i], " | s%d c%u(%x,%x,%x) a%u(%x,%x,%x) result %x sampled %u tci %x ttf %x", i, s->cop, s->ca1,
            s->ca2, s->ca0, s->aop, s->aa1, s->aa2, s->aa0, s->result, s->tex, d->vs.tci[i], d->vs.ttf[i]);
    }
    rt_log("[recomp] d3d8: frame %u: Minimap's map draw now: stages %u, pixel shader %08x, alpha test %u, fog %u, specular %u, "
           "blend %u %u/%u op %u, mask %x, interface layer %u, cull %u, z %u%s%s%s%s\n", g_cap_frame, d->fs.nstages, d->fs.prog,
        d->fs.alpha_func, d->fs.fog, d->fs.specular_add, d->pipe.blend, d->pipe.src, d->pipe.dst, d->pipe.op, d->pipe.write_mask,
        d->pipe.over, d->cull, d->depth.zenable, st[0], st[1], st[2], said == 200 ? " (the rest aren't logged)" : "");
}
#endif

/* --- device state ------------------------------------------------------------------------------------- */
#define NXF 280 /* transforms: D3DTS 0..23, then WORLDMATRIX(0..255) = 256..511 */
#define MAX_LIGHTS 64
#define NSTREAMS 16
#define NVSC 256
#define NPSC 8

typedef struct Light
{
    uint32_t v[26]; /* D3DLIGHT8 */
    uint32_t enabled;
} Light;

typedef struct State
{
    uint32_t rs[256];
    uint32_t tss[8][32];
    uint32_t tex[8];
    float xf[NXF][16];
    uint32_t vp[6];
    uint32_t mat[17];
    Light light[MAX_LIGHTS];
    uint32_t clip[6][4];
    uint32_t stream[NSTREAMS], stride[NSTREAMS];
    uint32_t ib, base_vertex;
    uint32_t vs, ps;
    float vsc[NVSC][4], psc[NPSC][4];
} State;

/* what a state block holds */
typedef struct Mask
{
    uint8_t rs[256], tss[8][32], tex[8], xf[NXF], vp, mat, light[MAX_LIGHTS], lighten[MAX_LIGHTS], clip[6],
        stream[NSTREAMS], ib, vs, ps, vsc[NVSC], psc[NPSC];
} Mask;

typedef struct Block
{
    State s;
    Mask m;
} Block;

/* where each input register comes from: an FVF's, or a vertex shader declaration's */
typedef struct Layout
{
    GfxElem el[GFX_NREGS];
    int32_t offset[GFX_NREGS];
    uint8_t rhw;
} Layout;

typedef struct Shader
{
    uint32_t* decl;
    uint32_t ndecl;
    uint32_t* func;
    uint32_t nfunc;
    Layout lay;    /* vertex shaders */
    uint32_t hash; /* of func: the back end's key */
} Shader;

typedef struct Dev
{
    uint32_t guest, d3d, hwnd, behavior, device_type;
    uint32_t pp[13]; /* D3DPRESENT_PARAMETERS */
    uint32_t backbuffer, depth, rt, ds;
    State cur;
    Block* rec;
    Block** blocks;
    uint32_t nblocks;
    Shader* vs;
    uint32_t nvs;
    Shader* ps;
    uint32_t nps;
    int cursor;
} Dev;

static Dev g_dev; /* the game makes one device */
static int g_dev_vsync; /* what its CreateDevice gave gfx_init (d3d8_device_vsync) */
/* One past the highest transform, light and vertex shader constant ever set, in any state: past
 * them every state holds the defaults, so a state block's apply or capture stops there (a
 * D3DSBT_ALL block is otherwise 280 matrices, 64 lights and 256 constants a copy). Never lowered. */
static int g_xf_top, g_light_top, g_vsc_top;

static void raise_top(int* top, uint32_t n)
{
    if ((int)n > *top)
        *top = (int)n;
}

/* [graphics] resolution=window: the back buffer, its depth buffer and the game's screen-shaped
 * targets are drawn at the window's size. The 3D scene's target: DLSS's share of that size
 * (g_scene_scale), else [graphics] background's size (g_scene_w x g_scene_h), else the game's own
 * (its background resolution). Set by VanaCore, applied at the next Present. */
static int g_draw_window;
static float g_scene_scale;
static uint32_t g_scene_w, g_scene_h;
static int g_resized; /* resize_targets has sized the targets for the current settings */
/* VanaCore draws its own interface in the back buffer's drawn pixels, not the game's: kept square
 * whatever the window's shape (d3d8_set_own_pixels) */
static int g_own_pixels;
/* /fillmode: the world's draws with this D3DFILLMODE in place of the game's, 1 point or 2 wireframe (d3d8_set_fill_mode:
 * graphics.cpp); 0 leaves the game's. The interface's draws (rhw) and VanaCore's own (g_own_pixels) stay as they are. */
static int g_fill_mode;
/* [graphics] untextured (d3d8_set_untextured): the world's opaque draws take a plain white texel for their base texture
 * (world_look); g_plain is that texel's texture, made at the first such draw and let go with the device */
static int g_untextured;
static GfxTex* g_plain;
/* ... into this layer of the window's size (ui_layer_bound) */
static GfxTex* g_ui_layer;
static uint32_t g_ui_w, g_ui_h;
static int g_ui_drawn; /* drawn into this frame */
static int g_raise_taken; /* the rectangles d3d8_raise_take copied this frame, not put back yet */
/* The game's interface was squeezed toward the middle this frame ([ui] aspect: ui_squeeze mapped one of its draws).
 * VanaCore's text over the game's windows follows the squeeze only then: with a menu target of another shape than
 * the aspect's, the game's interface isn't squeezed, and neither must that text be (d3d8_ui_squeezed). */
static int g_ui_squeezed;

/* A 16x16 occlusion probe tests a quad at the sky's depth (z 0x3f7ffffe, 1 - 2^-23: the game's
 * probe for the sun and its lens flare) since the last probe was read; see lock_rect. */
static int g_probe_sky;
static uint32_t g_d3d;

static int xf_index(uint32_t ts)
{
    return ts < 24 ? (int)ts : ts >= 256 && ts < 512 ? (int)(24 + ts - 256) : -1;
}

static void default_light(Light* l)
{
    memset(l, 0, sizeof *l);
    l->v[0] = 3; /* D3DLIGHT_DIRECTIONAL */
    l->v[1] = l->v[2] = l->v[3] = f2u(1.0f);
    l->v[18] = f2u(1.0f); /* direction (0, 0, 1) */
}

static void state_defaults(State* s, uint32_t w, uint32_t h, int zbuffer)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < NXF; ++i)
        s->xf[i][0] = s->xf[i][5] = s->xf[i][10] = s->xf[i][15] = 1.0f;
    uint32_t* rs = s->rs;
    rs[7] = zbuffer ? 1 : 0;           /* ZENABLE */
    rs[8] = 3;                         /* FILLMODE solid */
    rs[9] = 2;                         /* SHADEMODE gouraud */
    rs[14] = 1;                        /* ZWRITEENABLE */
    rs[16] = 1;                        /* LASTPIXEL */
    rs[19] = 2;                        /* SRCBLEND one */
    rs[20] = 1;                        /* DESTBLEND zero */
    rs[22] = 3;                        /* CULLMODE ccw */
    rs[23] = 4;                        /* ZFUNC lessequal */
    rs[25] = 8;                        /* ALPHAFUNC always */
    rs[37] = f2u(1.0f);                /* FOGEND */
    rs[38] = f2u(1.0f);                /* FOGDENSITY */
    rs[53] = rs[54] = rs[55] = 1;      /* STENCILFAIL/ZFAIL/PASS keep */
    rs[56] = 8;                        /* STENCILFUNC always */
    rs[58] = rs[59] = 0xFFFFFFFFu;     /* STENCILMASK, STENCILWRITEMASK */
    rs[60] = 0xFFFFFFFFu;              /* TEXTUREFACTOR */
    rs[136] = 1;                       /* CLIPPING */
    rs[137] = 1;                       /* LIGHTING */
    rs[141] = 1;                       /* COLORVERTEX */
    rs[142] = 1;                       /* LOCALVIEWER */
    rs[145] = 1;                       /* DIFFUSEMATERIALSOURCE color1 */
    rs[146] = 2;                       /* SPECULARMATERIALSOURCE color2 */
    rs[154] = rs[155] = f2u(1.0f);     /* POINTSIZE, POINTSIZE_MIN */
    rs[158] = f2u(1.0f);               /* POINTSCALE_A */
    rs[162] = 0xFFFFFFFFu;             /* MULTISAMPLEMASK */
    rs[164] = f2u(1.0f);               /* PATCHSEGMENTS */
    rs[166] = f2u(64.0f);              /* POINTSIZE_MAX */
    rs[168] = 0xF;                     /* COLORWRITEENABLE */
    rs[171] = 1;                       /* BLENDOP add */
    rs[172] = 3;                       /* POSITIONORDER cubic */
    rs[173] = 1;                       /* NORMALORDER linear */
    for (uint32_t t = 0; t < 8; ++t)
    {
        uint32_t* v = s->tss[t];
        v[1] = t ? 1 : 4; /* COLOROP: disable / modulate */
        v[2] = 2;         /* COLORARG1 texture */
        v[3] = 1;         /* COLORARG2 current */
        v[4] = t ? 1 : 2; /* ALPHAOP: disable / selectarg1 */
        v[5] = 2;
        v[6] = 1;
        v[11] = t;        /* TEXCOORDINDEX */
        v[13] = v[14] = v[25] = 1; /* ADDRESSU/V/W wrap */
        v[16] = v[17] = 1;         /* MAGFILTER, MINFILTER point */
        v[21] = 1;                 /* MAXANISOTROPY */
        v[26] = v[27] = v[28] = 1; /* COLORARG0, ALPHAARG0, RESULTARG current */
    }
    s->vp[2] = w;
    s->vp[3] = h;
    s->vp[5] = f2u(1.0f);
}

/* The state a setter writes: the block being recorded, or the device. */
static State* target(Mask** m)
{
    if (g_dev.rec)
    {
        *m = &g_dev.rec->m;
        return &g_dev.rec->s;
    }
    *m = NULL;
    return &g_dev.cur;
}

/* to[i] = from[i] where m[i] (0 or 1), eight at a time: a group with none set is skipped, one with
 * all set is one copy. n is a multiple of 8. */
static void copy_masked(uint32_t* to, const uint32_t* from, const uint8_t* m, int n)
{
    for (int i = 0; i < n; i += 8)
    {
        uint64_t group;
        memcpy(&group, m + i, 8);
        if (group == 0x0101010101010101ull)
            memcpy(to + i, from + i, 32);
        else if (group)
            for (int j = i; j < i + 8; ++j)
                if (m[j])
                    to[j] = from[j];
    }
}

/* Copies the masked entries from `from` to `to` (Apply: block -> device; Capture: device -> block). */
static void state_copy(State* to, const State* from, const Mask* m)
{
    copy_masked(to->rs, from->rs, m->rs, 256);
    for (int t = 0; t < 8; ++t)
    {
        copy_masked(to->tss[t], from->tss[t], m->tss[t], 32);
        if (m->tex[t])
            bind(&to->tex[t], from->tex[t]);
    }
    for (int i = 0; i < g_xf_top; ++i)
        if (m->xf[i])
            memcpy(to->xf[i], from->xf[i], 64);
    if (m->vp)
        memcpy(to->vp, from->vp, sizeof to->vp);
    if (m->mat)
        memcpy(to->mat, from->mat, sizeof to->mat);
    for (int i = 0; i < g_light_top; ++i)
    {
        if (m->light[i])
            memcpy(to->light[i].v, from->light[i].v, sizeof to->light[i].v);
        if (m->lighten[i])
            to->light[i].enabled = from->light[i].enabled;
    }
    for (int i = 0; i < 6; ++i)
        if (m->clip[i])
            memcpy(to->clip[i], from->clip[i], 16);
    for (int i = 0; i < NSTREAMS; ++i)
        if (m->stream[i])
        {
            bind(&to->stream[i], from->stream[i]);
            to->stride[i] = from->stride[i];
        }
    if (m->ib)
    {
        bind(&to->ib, from->ib);
        to->base_vertex = from->base_vertex;
    }
    if (m->vs)
        to->vs = from->vs;
    if (m->ps)
        to->ps = from->ps;
    for (int i = 0; i < g_vsc_top; ++i)
        if (m->vsc[i])
            memcpy(to->vsc[i], from->vsc[i], 16);
    for (int i = 0; i < NPSC; ++i)
        if (m->psc[i])
            memcpy(to->psc[i], from->psc[i], 16);
}

static void block_free(Block* b)
{
    for (int t = 0; t < 8; ++t)
        bind(&b->s.tex[t], 0);
    for (int i = 0; i < NSTREAMS; ++i)
        bind(&b->s.stream[i], 0);
    bind(&b->s.ib, 0);
    free(b);
}

static uint32_t block_token(Block* b)
{
    for (uint32_t i = 0; i < g_dev.nblocks; ++i)
        if (!g_dev.blocks[i])
        {
            g_dev.blocks[i] = b;
            return i + 1;
        }
    g_dev.blocks = (Block**)realloc(g_dev.blocks, (g_dev.nblocks + 1) * sizeof *g_dev.blocks);
    g_dev.blocks[g_dev.nblocks++] = b;
    return g_dev.nblocks;
}

static Block* block_of(uint32_t token)
{
    return token && token <= g_dev.nblocks ? g_dev.blocks[token - 1] : NULL;
}

/* --- IUnknown, shared by every interface ----------------------------------------------------------------- */
#ifdef VANACORE
static int iid_is(uint32_t p, const uint32_t want[4])
{
    return p && !memcmp(GUEST_PTR(p), want, 16);
}

/* QueryInterface(riid, ppvObj): the object itself for IUnknown and the interfaces it implements */
static void Unknown_QueryInterface(Guest* g)
{
    static const uint32_t UNKNOWN[4] = { 0x00000000u, 0x00000000u, 0x000000C0u, 0x46000000u };
    static const uint32_t D3D[4] = { 0x1DD9E8DAu, 0x4D401C77u, 0xFE98CFB0u, 0x1295FFFDu };
    static const uint32_t DEVICE[4] = { 0x7385E5DFu, 0x41D58FE8u, 0xB4D7B686u, 0xCFB64785u };
    static const uint32_t RESOURCE[4] = { 0x1B36BB7Bu, 0x410A09B7u, 0x147D45B4u, 0x3FB3D730u };
    static const uint32_t BASETEX[4] = { 0xB4211CFAu, 0x4A9F51B9u, 0x99DB78ABu, 0x8E67BBB2u };
    static const uint32_t TEXTURE[4] = { 0xE4CDD575u, 0x4F012866u, 0xEC7E2EB1u, 0x5893ECE1u };
    static const uint32_t CUBE[4] = { 0x3EE5B968u, 0x4C342ACAu, 0x0C7EB58Bu, 0x50B7193Du };
    static const uint32_t VB[4] = { 0x8AEEEAC7u, 0x44D405F9u, 0x0B0091B5u, 0x95CBF10Du };
    static const uint32_t IB[4] = { 0x0E689C9Au, 0x44A0053Du, 0x0EDB929Du, 0x860F753Du };
    static const uint32_t SURFACE[4] = { 0xB96EEBCAu, 0x4EA5B326u, 0xF52F2F88u, 0xDD21E0BAu };
    Obj* o = obj(ARG(0));
    uint32_t riid = ARG(1);
    int k = o ? o->kind : 0, yes = 0;
    if (iid_is(riid, UNKNOWN))
        yes = o != NULL;
    else if (k == O_D3D)
        yes = iid_is(riid, D3D);
    else if (k == O_DEVICE)
        yes = iid_is(riid, DEVICE);
    else if (k == O_SURFACE)
        yes = iid_is(riid, SURFACE);
    else if (k == O_TEXTURE || k == O_CUBE || k == O_VB || k == O_IB)
        yes = iid_is(riid, RESOURCE) || ((k == O_TEXTURE || k == O_CUBE) && iid_is(riid, BASETEX)) ||
            (k == O_TEXTURE && iid_is(riid, TEXTURE)) || (k == O_CUBE && iid_is(riid, CUBE)) || (k == O_VB && iid_is(riid, VB)) ||
            (k == O_IB && iid_is(riid, IB));
    if (!ARG(2))
        RET(0x80004003u, 3); /* E_POINTER */
    if (!yes)
    {
        wr32(ARG(2), 0);
        RET(E_NOINTERFACE, 3);
    }
    obj_addref(ARG(0));
    wr32(ARG(2), ARG(0));
    RET(D3D_OK, 3);
}
#else
static void Unknown_QueryInterface(Guest* g)
{
    wr32(ARG(2), 0);
    RET(E_NOINTERFACE, 3);
}
#endif
static void Unknown_AddRef(Guest* g) { RET(obj_addref(ARG(0)), 1); }
static void Unknown_Release(Guest* g) { RET(obj_release(ARG(0)), 1); }

/* --- IDirect3D8 -------------------------------------------------------------------------------------------- */
static void desktop(uint32_t* w, uint32_t* h, uint32_t* hz)
{
    user32_desktop_mode(w, h, hz);
}

static const uint32_t MODES[][2] = { { 640, 480 },  { 800, 600 },   { 1024, 768 },  { 1152, 864 },  { 1280, 720 },
                                     { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },  { 1440, 900 },  { 1600, 900 },
                                     { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 } };

/* the display modes offered: the list up to the desktop's size, then the desktop, in both formats */
static uint32_t modes(uint32_t (*out)[4], uint32_t max)
{
    uint32_t dw, dh, hz, n = 0;
    desktop(&dw, &dh, &hz);
    const uint32_t fmts[2] = { FMT_X8R8G8B8, FMT_R5G6B5 };
    for (int f = 0; f < 2; ++f)
    {
        int have_desktop = 0;
        for (size_t i = 0; i < sizeof MODES / sizeof MODES[0]; ++i)
            if (MODES[i][0] <= dw && MODES[i][1] <= dh && n < max)
            {
                have_desktop |= MODES[i][0] == dw && MODES[i][1] == dh;
                out[n][0] = MODES[i][0], out[n][1] = MODES[i][1], out[n][2] = hz, out[n][3] = fmts[f], n++;
            }
        if (!have_desktop && n < max)
            out[n][0] = dw, out[n][1] = dh, out[n][2] = hz, out[n][3] = fmts[f], n++;
    }
    return n;
}

static void IDirect3D8_GetAdapterCount(Guest* g) { RET(1, 1); }

static void IDirect3D8_GetAdapterIdentifier(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t p = ARG(3);
    memset(GUEST_PTR(p), 0, 1068);
#ifdef VANACORE
    strcpy((char*)GUEST_PTR(p), "vanacore-d3d12");
    strcpy((char*)GUEST_PTR(p + 512), "VanaCore Direct3D 12");
#else
    strcpy((char*)GUEST_PTR(p), "ffxi-metal");
    strcpy((char*)GUEST_PTR(p + 512), "FFXI Metal");
#endif
    wr32(p + 1024, 0x00010000u); /* DriverVersion 1.0.0.0 */
    wr32(p + 1028, 0x00060000u);
    wr32(p + 1032, 0x106B); /* VendorId: Apple */
    wr32(p + 1064, 1);      /* WHQLLevel */
    RET(D3D_OK, 4);
}

static void IDirect3D8_GetAdapterModeCount(Guest* g)
{
    uint32_t m[64][4];
    RET(ARG(1) ? 0 : modes(m, 64), 2);
}

static void IDirect3D8_EnumAdapterModes(Guest* g)
{
    uint32_t m[64][4], n = modes(m, 64), i = ARG(2);
    if (ARG(1) || i >= n)
        RET(D3DERR_INVALIDCALL, 4);
    for (int k = 0; k < 4; ++k)
        wr32(ARG(3) + 4u * (uint32_t)k, m[i][k]);
    RET(D3D_OK, 4);
}

static void write_desktop_mode(uint32_t p)
{
    uint32_t w, h, hz;
    desktop(&w, &h, &hz);
    wr32(p, w);
    wr32(p + 4, h);
    wr32(p + 8, hz);
    wr32(p + 12, FMT_X8R8G8B8);
}

static void IDirect3D8_GetAdapterDisplayMode(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 3);
    write_desktop_mode(ARG(2));
    RET(D3D_OK, 3);
}

static void IDirect3D8_CheckDeviceType(Guest* g) { RET(ARG(1) ? D3DERR_INVALIDCALL : D3D_OK, 6); }

/* What a Metal device can back. Paletted and 3-3-2 formats are out, as on current D3D9 drivers. */
static int format_ok(uint32_t usage, uint32_t rtype, uint32_t f)
{
    if (rtype == RT_VERTEXBUFFER || rtype == RT_INDEXBUFFER)
        return 1;
    if (rtype == RT_VOLUMETEXTURE)
        return 0;
    if (usage & USAGE_DEPTHSTENCIL)
        return f == FMT_D16 || f == FMT_D24S8 || f == FMT_D24X8;
    if (usage & USAGE_RENDERTARGET)
        return f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8 || f == FMT_R5G6B5 || f == FMT_X1R5G5B5 || f == FMT_A1R5G5B5;
    switch (f)
    {
    case FMT_A8R8G8B8:
    case FMT_X8R8G8B8:
    case FMT_R5G6B5:
    case FMT_X1R5G5B5:
    case FMT_A1R5G5B5:
    case FMT_A4R4G4B4:
    case FMT_A8:
    case FMT_L8:
    case FMT_A8L8:
    case FMT_V8U8:
    case FMT_DXT1:
    case FMT_DXT2:
    case FMT_DXT3:
    case FMT_DXT4:
    case FMT_DXT5:
        return 1;
    case FMT_D16:
    case FMT_D24S8:
    case FMT_D24X8:
        return rtype == RT_SURFACE || rtype == RT_TEXTURE;
    default: return 0;
    }
}

static void IDirect3D8_CheckDeviceFormat(Guest* g)
{
    RET(!ARG(1) && format_ok(ARG(4), ARG(5), ARG(6)) ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

static void IDirect3D8_CheckDeviceMultiSampleType(Guest* g) { RET(ARG(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 6); }
static void IDirect3D8_CheckDepthStencilMatch(Guest* g) { RET(D3D_OK, 6); }

static void IDirect3D8_GetDeviceCaps(Guest* g)
{
    if (ARG(1))
        RET(D3DERR_INVALIDCALL, 4);
    for (int i = 0; i < 53; ++i)
        wr32(ARG(3) + 4u * (uint32_t)i, CAPS8[i]);
    wr32(ARG(3), ARG(2)); /* DeviceType as asked */
    RET(D3D_OK, 4);
}

static void IDirect3D8_GetAdapterMonitor(Guest* g) { RET(0x00050001u, 2); }

/* CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice) */
static void IDirect3D8_CreateDevice(Guest* g)
{
    wr32(ARG(6), 0);
    if (ARG(1) || g_dev.guest)
        RET(g_dev.guest ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL, 7);
    Dev* d = &g_dev;
    for (int i = 0; i < 13; ++i)
        d->pp[i] = rd32(ARG(5) + 4u * (uint32_t)i);
    d->hwnd = d->pp[6] ? d->pp[6] : ARG(3);
    d->behavior = ARG(4);
    d->device_type = ARG(2);
    d->d3d = ARG(0);
#ifdef VANACORE
    /* The real d3d8 puts the calling thread's x87 unit in single precision unless the device is
     * made with D3DCREATE_FPU_PRESERVE; the game was written against that */
    if (!(d->behavior & 2u))
    {
        unsigned int cw;
        _controlfp_s(&cw, _PC_24, _MCW_PC);
    }
#endif
    if (!d->pp[0] || !d->pp[1])
    {
        uint32_t w = 640, h = 480;
        user32_client_size(d->hwnd, &w, &h);
        if (!d->pp[0])
            d->pp[0] = w;
        if (!d->pp[1])
            d->pp[1] = h;
    }
    if (!d->pp[2])
        d->pp[2] = FMT_X8R8G8B8;
    /* windowed devices ignore the presentation interval and wait for the display; so do we unless
     * a full-screen device asks for IMMEDIATE */
    user32_set_fullscreen(d->hwnd, !d->pp[7]);
    g_dev_vsync = d->pp[7] || d->pp[12] != 0x80000000u;
    gfx_init(user32_sdl_window(d->hwnd), g_dev_vsync);
    obj_addref(d->d3d);
    d->guest = obj_new(O_DEVICE);
    d->backbuffer = new_surface(d->pp[2], d->pp[0], d->pp[1], USAGE_RENDERTARGET, 0, 0);
    obj(d->backbuffer)->implicit = 1, obj(d->backbuffer)->grow = GROW_SCREEN;
    if (d->pp[8])
    {
        d->depth = new_surface(d->pp[9], d->pp[0], d->pp[1], USAGE_DEPTHSTENCIL, 0, 0);
        obj(d->depth)->implicit = 1, obj(d->depth)->grow = GROW_SCREEN;
    }
    bind(&d->rt, d->backbuffer);
    bind(&d->ds, d->depth);
    state_defaults(&d->cur, d->pp[0], d->pp[1], d->pp[8] != 0);
    rt_log("[recomp] d3d8: device %ux%u format %u%s, depth %u, behavior %08x, window %08x\n", d->pp[0], d->pp[1],
        d->pp[2], d->pp[7] ? " windowed" : " fullscreen", d->pp[8] ? d->pp[9] : 0, d->behavior, d->hwnd);
    wr32(ARG(6), d->guest);
    RET(D3D_OK, 7);
}

/* --- IDirect3DDevice8: device -------------------------------------------------------------------------- */
static void IDirect3DDevice8_TestCooperativeLevel(Guest* g) { RET(D3D_OK, 1); }
static void IDirect3DDevice8_GetAvailableTextureMem(Guest* g) { RET(512u << 20, 1); }
static void IDirect3DDevice8_ResourceManagerDiscardBytes(Guest* g) { RET(D3D_OK, 2); }

static void IDirect3DDevice8_GetDirect3D(Guest* g)
{
    obj_addref(g_dev.d3d);
    wr32(ARG(1), g_dev.d3d);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDeviceCaps(Guest* g)
{
    for (int i = 0; i < 53; ++i)
        wr32(ARG(1) + 4u * (uint32_t)i, CAPS8[i]);
    wr32(ARG(1), g_dev.device_type);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDisplayMode(Guest* g)
{
    write_desktop_mode(ARG(1));
    if (!g_dev.pp[7]) /* fullscreen: the mode the device set */
    {
        wr32(ARG(1), g_dev.pp[0]);
        wr32(ARG(1) + 4, g_dev.pp[1]);
        wr32(ARG(1) + 12, g_dev.pp[2]);
    }
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetCreationParameters(Guest* g)
{
    wr32(ARG(1), 0);
    wr32(ARG(1) + 4, g_dev.device_type);
    wr32(ARG(1) + 8, g_dev.hwnd);
    wr32(ARG(1) + 12, g_dev.behavior);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetCursorProperties(Guest* g) { RET(D3D_OK, 4); }
static void IDirect3DDevice8_SetCursorPosition(Guest* g) { RET(0, 4); }

static void IDirect3DDevice8_ShowCursor(Guest* g)
{
    int was = g_dev.cursor;
    g_dev.cursor = ARG(1) != 0;
    RET((uint32_t)was, 2);
}

static void IDirect3DDevice8_Reset(Guest* g)
{
    Dev* d = &g_dev;
    for (int i = 0; i < 13; ++i)
        d->pp[i] = rd32(ARG(1) + 4u * (uint32_t)i);
    if (!d->pp[0] || !d->pp[1])
        user32_client_size(d->hwnd, &d->pp[0], &d->pp[1]);
    if (!d->pp[2])
        d->pp[2] = FMT_X8R8G8B8;
    user32_set_fullscreen(d->hwnd, !d->pp[7]);
    Obj* bb = obj(d->backbuffer);
    bb->format = d->pp[2], bb->width = d->pp[0], bb->height = d->pp[1], bb->size = fmt_size(bb->format, bb->width, bb->height);
    if (bb->mem)
        gheap_free(bb->mem), bb->mem = 0;
    scene_forget(bb->gpu);
    gfx_tex_destroy(bb->gpu);
    bb->gpu = gfx_tex_create(GFX_TEX_2D, bb->format, bb->width, bb->height, 1, GFX_USE_RT);
    bb->pw = bb->ph = 0;
    g_resized = 0; /* resize_targets looks again */
    Obj* z = obj(d->depth);
    if (z)
    {
        z->width = d->pp[0], z->height = d->pp[1], z->size = fmt_size(z->format, z->width, z->height);
        if (z->mem)
            gheap_free(z->mem), z->mem = 0;
        scene_forget(z->gpu);
        gfx_tex_destroy(z->gpu);
        z->gpu = gfx_tex_create(GFX_TEX_2D, z->format, z->width, z->height, 1, GFX_USE_DEPTH);
        z->pw = z->ph = 0;
    }
    bind(&d->rt, d->backbuffer);
    bind(&d->ds, d->depth);
    for (int t = 0; t < 8; ++t)
        bind(&d->cur.tex[t], 0);
    for (int i = 0; i < NSTREAMS; ++i)
        bind(&d->cur.stream[i], 0);
    bind(&d->cur.ib, 0);
    state_defaults(&d->cur, d->pp[0], d->pp[1], d->pp[8] != 0);
    rt_log("[recomp] d3d8: reset to %ux%u\n", d->pp[0], d->pp[1]);
    RET(D3D_OK, 2);
}

/* Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion): the frame is done */
static void (*g_present_hook)(void);

void d3d8_set_present_hook(void (*fn)(void)) { g_present_hook = fn; }

void d3d8_screen_size(uint32_t* w, uint32_t* h)
{
    *w = g_dev.pp[0], *h = g_dev.pp[1];
    if (g_dev.hwnd)
        user32_client_size(g_dev.hwnd, w, h);
}

static void ui_present(void);
static void chatfont_present(void), chatfont_lost(void), chatfont_flush(void); /* d3d8_chatfont.h */
static void winimage_lost(void);                                                /* d3d8_winimage.h */
static void wc_present(void);                                                   /* d3d8_corners.h */
static void resize_targets(void);

static void IDirect3DDevice8_Present(Guest* g)
{
    g_bg_count = 0;
    wc_present();
    g_glow_laid = 0; /* tinted copies with no shrinking or solid copy after them: the next frame's are another glow's */
    cap_present();
    scene_present();
    ui_present();
    chatfont_present();
    if (g_present_hook)
        g_present_hook();
    Obj* bb = obj(g_dev.backbuffer);
#ifdef VANACORE
    gfx_present_layer(g_ui_drawn ? g_ui_layer : NULL);
    g_ui_drawn = 0;
    g_ui_squeezed = 0;
    g_raise_taken = 0;
#endif
    gfx_present(bb ? bb->gpu : NULL);
    resize_targets();
    RET(D3D_OK, 5);
}

static void IDirect3DDevice8_GetBackBuffer(Guest* g)
{
    if (ARG(1))
    {
        wr32(ARG(3), 0);
        RET(D3DERR_INVALIDCALL, 4);
    }
    obj_addref(g_dev.backbuffer);
    wr32(ARG(3), g_dev.backbuffer);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetRasterStatus(Guest* g)
{
    wr32(ARG(1), 1); /* InVBlank */
    wr32(ARG(1) + 4, 0);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetGammaRamp(Guest* g) { RET(0, 3); }

/* --- resources ------------------------------------------------------------------------------------------ */
static uint32_t chain_levels(uint32_t w, uint32_t h, uint32_t levels)
{
    uint32_t full = 1;
    for (uint32_t m = w > h ? w : h; m > 1; m >>= 1)
        full++;
    return !levels || levels > full ? full : levels;
}

/* FFXI draws its interface into a render-target texture of the menu resolution (the registry's 0037 x 0038: half the
 * window, say) and stretches that over the back buffer, so its text is drawn at half the screen's resolution and
 * doubled. A render-target texture smaller than the back buffer and of its shape is instead given a GPU texture of
 * the back buffer's size: the game still sees its own size (locks, descriptions, XYZRHW coordinates), and its draws
 * land on the larger texture through a viewport scaled to match (scale_to_target), so the interface is drawn at the
 * screen's resolution and the final stretch is one to one. FFXI_UI_NATIVE=0 draws it at the menu resolution again. */
/* w x h is no larger than the screen and of the --ui-aspect box's shape (1280x720 for 16:9, 1px either way). The box
 * itself counts (1920x1080 in a 1920x1200 back buffer: resolution.ui=window, which VanaCore fits to the box); the whole
 * screen doesn't. */
static int ui_box_shape(uint32_t w, uint32_t h)
{
    uint32_t bw = g_dev.pp[0], bh = g_dev.pp[1];
    float s = user32_ui_squeeze(g_dev.hwnd), sy = user32_ui_squeeze_y(g_dev.hwnd);
    if ((s >= 1.0f && sy >= 1.0f) || !h || w > bw || h > bh || (w == bw && h == bh))
        return 0;
    double boxw = bw * (double)s, boxh = bh * (double)sy;
    return fabs((double)w * boxh - boxw * h) <= boxh;
}

/* The size the back buffer is drawn at: the window's client area with [graphics]
 * resolution=window, else the game's (as it is while the window is minimized). */
static uint32_t g_render_w, g_render_h; /* d3d8_set_render_size: the game's resolution's size, live, in full screen */

static void drawn_size(uint32_t* w, uint32_t* h)
{
    Obj* bb = obj(g_dev.backbuffer);
    *w = bb && bb->pw ? bb->pw : g_dev.pp[0], *h = bb && bb->ph ? bb->ph : g_dev.pp[1];
    if (!g_dev.hwnd)
        *w = g_dev.pp[0], *h = g_dev.pp[1];
    else if (!g_draw_window)
    {
        /* the game's resolution: in full screen, [ffxi] resolution.game's as it is set now (the game lays itself out at
         * the one it started with, its draws scaled to this), stretched over the screen when it's smaller */
        *w = g_render_w && g_render_h ? g_render_w : g_dev.pp[0];
        *h = g_render_w && g_render_h ? g_render_h : g_dev.pp[1];
    }
    else
        user32_client_size(g_dev.hwnd, w, h);
}

/* A screen-shaped render target no larger than the back buffer (the menu target: the menu resolution,
 * 0037 x 0038), or one of the --ui-aspect box's shape: drawn at the drawn size. */
static int native_size(uint32_t w, uint32_t h, uint32_t* pw, uint32_t* ph)
{
    static int on = -1;
    if (on < 0)
        on = !(getenv("FFXI_UI_NATIVE") && getenv("FFXI_UI_NATIVE")[0] == '0');
    uint32_t bw = g_dev.pp[0], bh = g_dev.pp[1];
    if (!on || !bw || !bh || w > bw || h > bh)
        return 0;
    /* under --ui-aspect the menu target is of the interface box's shape; it still covers the screen,
     * its draws squeezed toward the middle (ui_squeeze) */
    if ((uint64_t)w * bh != (uint64_t)h * bw && !ui_box_shape(w, h))
        return 0;
    drawn_size(pw, ph);
    return 1;
}

/* The object that holds a surface's pixels: its texture, or itself. */
static Obj* pixels_of(const Obj* s)
{
    return s->container ? obj(s->container) : (Obj*)s;
}

/* A surface drawn at another size than the game's: kx, ky from the game's pixels to the drawn ones. */
static int surface_grown(const Obj* s, double* kx, double* ky)
{
    const Obj* o = pixels_of(s);
    if (!o || !o->pw || !o->width || !o->height)
        return 0;
    if (kx)
        *kx = (double)o->pw / o->width, *ky = (double)o->ph / o->height;
    return 1;
}

static int surface_scaled(const Obj* s)
{
    return surface_grown(s, NULL, NULL);
}

/* A surface drawn at another size read at the game's, into bits with pitch: each of the game's
 * pixels the average of the drawn pixels it covers (4-byte formats; the nearest one otherwise). */
static void read_grown(Obj* s, uint8_t* bits, uint32_t pitch)
{
    const Obj* o = pixels_of(s);
    uint32_t face, level;
    GfxTex* g = surface_gpu(s, &face, &level);
    uint32_t pw = o ? o->pw : 0, ph = o ? o->ph : 0, w = s->width, h = s->height, bpp = fmt_bytes(s->format);
    if (!g || !pw || !ph || !bpp)
        return;
    uint8_t* big = (uint8_t*)malloc((size_t)pw * ph * bpp);
    if (!big)
        return;
    gfx_tex_read(g, face, level, big, pw * bpp);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint32_t y0 = (uint32_t)((uint64_t)y * ph / h), y1 = (uint32_t)((uint64_t)(y + 1) * ph / h);
        if (y1 <= y0)
            y1 = y0 + 1;
        uint8_t* out = bits + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; ++x, out += bpp)
        {
            uint32_t x0 = (uint32_t)((uint64_t)x * pw / w), x1 = (uint32_t)((uint64_t)(x + 1) * pw / w);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (bpp != 4)
            {
                memcpy(out, big + ((size_t)y0 * pw + x0) * bpp, bpp);
                continue;
            }
            uint32_t sum[4] = { 0, 0, 0, 0 }, n = 0;
            for (uint32_t yy = y0; yy < y1; ++yy)
                for (uint32_t xx = x0; xx < x1; ++xx, ++n)
                {
                    const uint8_t* q = big + ((size_t)yy * pw + xx) * 4;
                    sum[0] += q[0], sum[1] += q[1], sum[2] += q[2], sum[3] += q[3];
                }
            for (int c = 0; c < 4; ++c)
                out[c] = (uint8_t)((sum[c] + n / 2) / n);
        }
    }
    free(big);
}

/* A viewport (x, y, w, h first) or D3DRECTs from the game's pixels to the current target's. */
static void scale_to_target(uint32_t* vp, int32_t* rects, uint32_t nrects)
{
    const Obj* rt = obj(g_dev.rt);
    double kx, ky;
    if (!rt || !surface_grown(rt, &kx, &ky))
        return;
    if (vp)
        vp[0] = (uint32_t)(vp[0] * kx + 0.5), vp[1] = (uint32_t)(vp[1] * ky + 0.5), vp[2] = (uint32_t)(vp[2] * kx + 0.5),
        vp[3] = (uint32_t)(vp[3] * ky + 0.5);
    for (uint32_t i = 0; i < nrects; ++i)
    {
        int32_t* r = rects + 4 * i;
        r[0] = (int32_t)(r[0] * kx + 0.5), r[1] = (int32_t)(r[1] * ky + 0.5), r[2] = (int32_t)(r[2] * kx + 0.5),
        r[3] = (int32_t)(r[3] * ky + 0.5);
    }
}

static uint32_t new_texture(int kind, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    uint32_t p = obj_new(kind);
    Obj* t = obj(p);
    t->width = w, t->height = h, t->usage = usage, t->format = fmt, t->pool = pool;
    uint32_t nl = t->levels = chain_levels(w, h, levels);
    uint32_t faces = kind == O_CUBE ? 6 : 1;
    uint32_t* subs = t->subs = (uint32_t*)calloc(faces * nl, 4);
    t->nsubs = faces * nl;
    /* new_surface may grow the object table: t is not used past here */
    for (uint32_t f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < nl; ++l)
        {
            uint32_t sp = new_surface(fmt, (w >> l) ? (w >> l) : 1, (h >> l) ? (h >> l) : 1, usage, pool, p);
            subs[f * nl + l] = sp;
            obj(sp)->face = (uint8_t)f, obj(sp)->level = (uint8_t)l;
        }
    uint32_t pw = 0, ph = 0;
    if (kind == O_TEXTURE && (usage & USAGE_RENDERTARGET) && nl == 1 && pool != 2 && native_size(w, h, &pw, &ph))
    {
        obj(p)->grow = GROW_SCREEN;
        if (pw == w && ph == h)
            pw = ph = 0;
        else
            rt_log("[recomp] d3d8: %ux%u render target drawn at %ux%u\n", w, h, pw, ph);
        obj(p)->pw = pw, obj(p)->ph = ph;
    }
    if (pool != 2) /* system memory textures are only ever copied from */
        obj(p)->gpu = gfx_tex_create(kind == O_CUBE ? GFX_TEX_CUBE : GFX_TEX_2D, fmt, pw ? pw : w, ph ? ph : h, nl,
            (usage & USAGE_RENDERTARGET) ? GFX_USE_RT : GFX_USE_SAMPLE);
    return p;
}

/* CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture) */
static void IDirect3DDevice8_CreateTexture(Guest* g)
{
    wr32(ARG(7), 0);
    if (!ARG(1) || !ARG(2) || !format_ok(ARG(4), RT_TEXTURE, ARG(5)))
        RET(D3DERR_INVALIDCALL, 8);
    wr32(ARG(7), new_texture(O_TEXTURE, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)));
    RET(D3D_OK, 8);
}

/* CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture) */
static void IDirect3DDevice8_CreateCubeTexture(Guest* g)
{
    wr32(ARG(6), 0);
    if (!ARG(1) || !format_ok(ARG(3), RT_CUBETEXTURE, ARG(4)))
        RET(D3DERR_INVALIDCALL, 7);
    wr32(ARG(6), new_texture(O_CUBE, ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)));
    RET(D3D_OK, 7);
}

static void IDirect3DDevice8_CreateVolumeTexture(Guest* g)
{
    wr32(ARG(8), 0);
    RET(D3DERR_NOTAVAILABLE, 9);
}

#define USAGE_DYNAMIC 0x200u

/* A buffer the game does not declare dynamic is written once and drawn many times: it gets a GPU
 * copy, refreshed at the next draw after a lock that writes. Dynamic ones are copied per draw. */
static void static_buffer_init(Obj* b)
{
    if (!(b->usage & USAGE_DYNAMIC) && b->size)
    {
        b->gbuf = gfx_buf_create(b->size);
        b->gbuf_dirty = 1;
    }
}

static GfxBuf* static_buffer(Obj* b)
{
    if (!b->gbuf || !b->mem)
        return NULL;
    if (b->gbuf_dirty)
    {
        gfx_buf_upload(b->gbuf, GUEST_PTR(b->mem), b->size);
        b->gbuf_dirty = 0;
    }
    return b->gbuf;
}

/* The game's water models (from XI on Anything, rubymatrix/xi-on-anything, runtime/portable/d3d8.c): FFXI's water is map
 * models under the effect directories of a zone's DAT named for it (mizu, umi1, sea, taki, kawa ...). VanaCore.Core
 * (water.cpp) hooks the game's map model loader and brackets each water model's load with d3d8_water_loading(1) and (0);
 * the vertex buffers made in between are the model's (Obj.water), and a draw from one is the game's water (water_model).
 * Per thread: a loader thread making the buffers of other models meanwhile isn't caught up in it. */
#ifdef _MSC_VER
static __declspec(thread) int g_water_loading;
#else
static _Thread_local int g_water_loading;
#endif
static volatile uint32_t g_water_buffers; /* made so far, for water.cpp's log (d3d8_water_buffers) */

int d3d8_water_loading(int on)
{
    const int was = g_water_loading;
    g_water_loading = on;
    return was;
}

uint32_t d3d8_water_buffers(void)
{
    return g_water_buffers;
}

/* CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer) */
static void IDirect3DDevice8_CreateVertexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_VB);
    Obj* b = obj(p);
    b->size = ARG(1), b->usage = ARG(2), b->fvf = ARG(3), b->pool = ARG(4);
    if (g_water_loading)
        b->water = 1, ++g_water_buffers;
    static_buffer_init(b);
    wr32(ARG(5), p);
    RET(D3D_OK, 6);
}

/* CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer) */
static void IDirect3DDevice8_CreateIndexBuffer(Guest* g)
{
    uint32_t p = obj_new(O_IB);
    Obj* b = obj(p);
    b->size = ARG(1), b->usage = ARG(2), b->format = ARG(3), b->pool = ARG(4);
    static_buffer_init(b);
    wr32(ARG(5), p);
    RET(D3D_OK, 6);
}

/* CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, ppSurface) */
static void IDirect3DDevice8_CreateRenderTarget(Guest* g)
{
    wr32(ARG(6), new_surface(ARG(3), ARG(1), ARG(2), USAGE_RENDERTARGET, 0, 0));
    RET(D3D_OK, 7);
}

/* CreateDepthStencilSurface(Width, Height, Format, MultiSample, ppSurface) */
static void IDirect3DDevice8_CreateDepthStencilSurface(Guest* g)
{
    wr32(ARG(5), new_surface(ARG(3), ARG(1), ARG(2), USAGE_DEPTHSTENCIL, 0, 0));
    RET(D3D_OK, 6);
}

/* CreateImageSurface(Width, Height, Format, ppSurface): system memory */
static void IDirect3DDevice8_CreateImageSurface(Guest* g)
{
    wr32(ARG(4), new_surface(ARG(3), ARG(1), ARG(2), 0, 2, 0));
    RET(D3D_OK, 5);
}

/* CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray).
 * Between two surfaces the GPU owns (the back buffer into a render-target texture, say) the copy is
 * a GPU blit. Otherwise it happens in guest memory - a GPU-owned source is read back first - and the
 * destination is uploaded: at once if the GPU owns it, before its next draw if not. */
static void IDirect3DDevice8_CopyRects(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(4));
    if (!s || !d || s->format != d->format)
        RET(D3DERR_INVALIDCALL, 6);
    uint32_t sface, slevel, dface, dlevel;
    GfxTex* sg = surface_gpu(s, &sface, &slevel);
    GfxTex* dg = surface_gpu(d, &dface, &dlevel);
    int s_gpu = gpu_owned(s), d_gpu = gpu_owned(d);
    /* a render target drawn at another size: read at the game's size into memory, or copied on the GPU
     * to one drawn at the same scale; anything else is skipped */
    double skx = 1, sky = 1, dkx = 1, dky = 1;
    int sgrown = surface_grown(s, &skx, &sky), dgrown = surface_grown(d, &dkx, &dky);
    if ((sgrown || dgrown) && !(!dgrown && !d_gpu) && !(s_gpu && d_gpu && skx == dkx && sky == dky))
    {
        static int said;
        if (!said)
            said = 1, rt_log("[recomp] d3d8: CopyRects between render targets drawn at different sizes is skipped\n");
        RET(D3D_OK, 6);
    }
    uint32_t n = ARG(2) ? ARG(3) : 1, blk = fmt_block(s->format), unit = blk ? 4 : 1, bpp = blk ? blk : fmt_bytes(s->format);
    uint32_t sp = fmt_pitch(s->format, s->width), dp = fmt_pitch(d->format, d->width);
    uint8_t *sm = NULL, *dm = NULL;
    if (!(s_gpu && d_gpu))
    {
        sm = GUEST_PTR(obj_mem(s));
        dm = GUEST_PTR(obj_mem(d));
        if (s_gpu && !(s->usage & USAGE_DEPTHSTENCIL))
        {
            if (sgrown)
                read_grown(s, sm, sp);
            else
                gfx_tex_read(sg, sface, slevel, sm, sp);
        }
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        int32_t l = 0, t = 0, r = (int32_t)s->width, b = (int32_t)s->height;
        if (ARG(2))
        {
            uint32_t rc = ARG(2) + 16 * i;
            l = (int32_t)rd32(rc), t = (int32_t)rd32(rc + 4), r = (int32_t)rd32(rc + 8), b = (int32_t)rd32(rc + 12);
        }
        int32_t x = l, y = t;
        if (ARG(5))
            x = (int32_t)rd32(ARG(5) + 8 * i), y = (int32_t)rd32(ARG(5) + 8 * i + 4);
        if (l < 0 || t < 0 || r > (int32_t)s->width || b > (int32_t)s->height || r <= l || b <= t || x < 0 || y < 0 ||
            x + (r - l) > (int32_t)d->width || y + (b - t) > (int32_t)d->height)
            continue;
        if (s_gpu && d_gpu)
        {
            uint32_t gl = (uint32_t)(l * skx + 0.5), gt = (uint32_t)(t * sky + 0.5), gw = (uint32_t)((r - l) * skx + 0.5),
                     gh = (uint32_t)((b - t) * sky + 0.5);
            gfx_copy(sg, sface, slevel, gl, gt, gw, gh, dg, dface, dlevel, (uint32_t)(x * dkx + 0.5), (uint32_t)(y * dky + 0.5));
            continue;
        }
        uint32_t rows = ((uint32_t)(b - t) + unit - 1) / unit, bytes = ((uint32_t)(r - l) + unit - 1) / unit * bpp;
        uint8_t* drow = dm + ((uint32_t)y / unit) * dp + (uint32_t)x / unit * bpp;
        for (uint32_t row = 0; row < rows; ++row)
            memmove(drow + row * dp, sm + ((uint32_t)t / unit + row) * sp + (uint32_t)l / unit * bpp, bytes);
        if (d_gpu && !(d->usage & USAGE_DEPTHSTENCIL))
            gfx_tex_upload_rect(dg, dface, dlevel, (uint32_t)x, (uint32_t)y, (uint32_t)(r - l), (uint32_t)(b - t), drow, dp);
    }
    if (!d_gpu)
        mark_dirty(d);
    RET(D3D_OK, 6);
}

/* SetRenderTarget(pRenderTarget, pNewZStencil): NULL render target keeps the current one */
static void IDirect3DDevice8_SetRenderTarget(Guest* g)
{
    if (ARG(1))
    {
        bind(&g_dev.rt, ARG(1));
        const Obj* rt = obj(ARG(1));
        if (rt) /* D3D8 resets the viewport to the new target */
        {
            uint32_t* vp = g_dev.cur.vp;
            vp[0] = vp[1] = 0, vp[2] = rt->width, vp[3] = rt->height, vp[4] = 0, vp[5] = f2u(1.0f);
        }
    }
    bind(&g_dev.ds, ARG(2));
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetRenderTarget(Guest* g)
{
    obj_addref(g_dev.rt);
    wr32(ARG(1), g_dev.rt);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetDepthStencilSurface(Guest* g)
{
    wr32(ARG(1), g_dev.ds);
    if (!g_dev.ds)
        RET(0x88760834u, 2); /* D3DERR_NOTFOUND */
    obj_addref(g_dev.ds);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_BeginScene(Guest* g) { RET(D3D_OK, 1); }
/* The game's frame is drawn (VanaCore's own drawing comes after, in its EndScene and Present hooks): True Font's letters that
 * go straight onto the screen go there now, under what's drawn next (chatfont_flush) */
static void IDirect3DDevice8_EndScene(Guest* g)
{
    chatfont_flush();
    RET(D3D_OK, 1);
}
static void apply_targets(void);

/* Clear(Count, pRects, Flags, Color, Z, Stencil): D3DRECTs are x1, y1, x2, y2 */
static void IDirect3DDevice8_Clear(Guest* g)
{
    apply_targets();
    uint32_t n = ARG(2) ? ARG(1) : 0, vp[6];
    memcpy(vp, g_dev.cur.vp, sizeof vp);
    int32_t few[4 * 16], *rects = n <= 16 ? few : (int32_t*)malloc(16u * n);
    if (n)
        memcpy(rects, ARGP(2), 16u * n);
    scale_to_target(vp, rects, n);
    gfx_clear(n, n ? rects : NULL, ARG(3), ARG(4), u2f(ARG(5)), ARG(6), vp);
    if (rects != few)
        free(rects);
    RET(D3D_OK, 7);
}

/* --- IDirect3DDevice8: state ----------------------------------------------------------------------------- */
static void IDirect3DDevice8_SetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->xf[i], ARGP(2), 64);
    raise_top(&g_xf_top, (uint32_t)i + 1);
    if (m)
        m->xf[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_dev.cur.xf[i], 64);
    RET(D3D_OK, 3);
}

/* MultiplyTransform(State, pMatrix): the state's matrix becomes pMatrix * matrix */
static void IDirect3DDevice8_MultiplyTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    float a[16], r[16];
    memcpy(a, ARGP(2), 64);
    const float* b = g_dev.cur.xf[i];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            r[y * 4 + x] = a[y * 4] * b[x] + a[y * 4 + 1] * b[4 + x] + a[y * 4 + 2] * b[8 + x] + a[y * 4 + 3] * b[12 + x];
    Mask* m;
    State* s = target(&m);
    memcpy(s->xf[i], r, 64);
    raise_top(&g_xf_top, (uint32_t)i + 1);
    if (m)
        m->xf[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetViewport(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    memcpy(s->vp, ARGP(1), 24);
    if (m)
        m->vp = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetViewport(Guest* g)
{
    memcpy(ARGP(1), g_dev.cur.vp, 24);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetMaterial(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    memcpy(s->mat, ARGP(1), 68);
    if (m)
        m->mat = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetMaterial(Guest* g)
{
    memcpy(ARGP(1), g_dev.cur.mat, 68);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetLight(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->light[i].v, ARGP(2), 104);
    raise_top(&g_light_top, i + 1);
    if (m)
        m->light[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetLight(Guest* g)
{
    if (ARG(1) >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_dev.cur.light[ARG(1)].v, 104);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_LightEnable(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    if (!s->light[i].v[0]) /* never set: D3D enables a default light */
    {
        default_light(&s->light[i]);
        if (m)
            m->light[i] = 1;
    }
    s->light[i].enabled = ARG(2) != 0;
    raise_top(&g_light_top, i + 1);
    if (m)
        m->lighten[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetLightEnable(Guest* g)
{
    if (ARG(1) >= MAX_LIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev.cur.light[ARG(1)].enabled);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetClipPlane(Guest* g)
{
    if (ARG(1) >= 6)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    memcpy(s->clip[ARG(1)], ARGP(2), 16);
    if (m)
        m->clip[ARG(1)] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetRenderState(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= 256)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    s->rs[i] = ARG(2);
    if (m)
        m->rs[i] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetRenderState(Guest* g)
{
    if (ARG(1) >= 256)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev.cur.rs[ARG(1)]);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_BeginStateBlock(Guest* g)
{
    if (g_dev.rec)
        RET(D3DERR_INVALIDCALL, 1);
    g_dev.rec = (Block*)calloc(1, sizeof(Block));
    RET(D3D_OK, 1);
}

static void IDirect3DDevice8_EndStateBlock(Guest* g)
{
    if (!g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(ARG(1), block_token(g_dev.rec));
    g_dev.rec = NULL;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_ApplyStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    state_copy(&g_dev.cur, &b->s, &b->m);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_CaptureStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 2);
    state_copy(&b->s, &g_dev.cur, &b->m);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeleteStateBlock(Guest* g)
{
    Block* b = block_of(ARG(1));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    block_free(b);
    g_dev.blocks[ARG(1) - 1] = NULL;
    RET(D3D_OK, 2);
}

/* CreateStateBlock(Type, pToken): D3DSBT_ALL 1, PIXELSTATE 2, VERTEXSTATE 3. The pixel and vertex
 * sets here are the categories, not D3D's exact render-state lists: rendering state that would be
 * captured by one or the other is captured by both. */
static void IDirect3DDevice8_CreateStateBlock(Guest* g)
{
    uint32_t type = ARG(1);
    if (type < 1 || type > 3 || g_dev.rec)
        RET(D3DERR_INVALIDCALL, 3);
    Block* b = (Block*)calloc(1, sizeof(Block));
    /* state_copy stops at the high-water marks (g_xf_top, g_light_top, g_vsc_top): past them the block
     * starts as the device is (identity transforms, lights off, zero constants), so one applied after
     * the game first sets such an entry puts its default back, not zeros */
    memcpy(b->s.xf, g_dev.cur.xf, sizeof b->s.xf);
    memcpy(b->s.light, g_dev.cur.light, sizeof b->s.light);
    memcpy(b->s.vsc, g_dev.cur.vsc, sizeof b->s.vsc);
    Mask* m = &b->m;
    memset(m->rs, 1, sizeof m->rs);
    memset(m->tss, 1, sizeof m->tss);
    if (type == 1 || type == 2)
    {
        m->ps = 1;
        memset(m->psc, 1, sizeof m->psc);
    }
    if (type == 1 || type == 3)
    {
        m->vs = 1;
        memset(m->vsc, 1, sizeof m->vsc);
        memset(m->light, 1, sizeof m->light);
        memset(m->lighten, 1, sizeof m->lighten);
    }
    if (type == 1)
    {
        memset(m->tex, 1, sizeof m->tex);
        memset(m->xf, 1, sizeof m->xf);
        memset(m->clip, 1, sizeof m->clip);
        memset(m->stream, 1, sizeof m->stream);
        m->vp = m->mat = m->ib = 1;
    }
    state_copy(&b->s, &g_dev.cur, m);
    wr32(ARG(2), block_token(b));
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTexture(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t t = g_dev.cur.tex[ARG(1)];
    if (t)
        obj_addref(t);
    wr32(ARG(2), t);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetTexture(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    Mask* m;
    State* s = target(&m);
    bind(&s->tex[ARG(1)], ARG(2));
    if (m)
        m->tex[ARG(1)] = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetTextureStageState(Guest* g)
{
    if (ARG(1) >= 8 || ARG(2) >= 32)
        RET(D3DERR_INVALIDCALL, 4);
    wr32(ARG(3), g_dev.cur.tss[ARG(1)][ARG(2)]);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetTextureStageState(Guest* g)
{
    if (ARG(1) >= 8 || ARG(2) >= 32)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    s->tss[ARG(1)][ARG(2)] = ARG(3);
    if (m)
        m->tss[ARG(1)][ARG(2)] = 1;
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_ValidateDevice(Guest* g)
{
    wr32(ARG(1), 1);
    RET(D3D_OK, 2);
}

/* --- IDirect3DDevice8: drawing ----------------------------------------------------------------------------- */
static void draw(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size, uint32_t up_data,
    uint32_t up_stride);

/* DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount) */
static void IDirect3DDevice8_DrawPrimitive(Guest* g)
{
    g_cap_esp = g->esp;
    draw(ARG(1), ARG(3), ARG(2), 0, 0, 0, 0);
    RET(D3D_OK, 4);
}

/* DrawIndexedPrimitive(PrimitiveType, minIndex, NumVertices, startIndex, primCount): the vertex range
 * comes from the indices themselves, not minIndex and NumVertices */
static void IDirect3DDevice8_DrawIndexedPrimitive(Guest* g)
{
    g_cap_esp = g->esp;
    const Obj* ib = obj(g_dev.cur.ib);
    if (ib && ib->mem)
    {
        uint32_t isize = ib->format == FMT_INDEX32 ? 4 : 2;
        draw(ARG(1), ARG(5), 0, ib->mem + ARG(4) * isize, isize, 0, 0);
    }
    else
        gfx_prof_skip(GFX_SKIP_NO_INDICES);
    RET(D3D_OK, 6);
}

/* DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride).
 * The ...UP draws leave stream 0 (and the indices) unset, as D3D8 does. */
/* A glyph quad from a texture whose pack entry has a glyph table: FFXI draws each glyph of its name
 * and small menu fonts as a fixed-width cell from where the glyph starts in the sheet, so the cell also
 * takes in the edge of the glyph beside it there (shown between letters) and cuts the glyph's own lean
 * and outline. The quad is drawn over the ink box of the glyph that starts in the cell instead: the
 * same texels where they overlap, so it lands where the game put it. */
static uint32_t fit_quad(uint32_t data, const Obj* t)
{
    const PackEntry* e = (const PackEntry*)t->repl_entry;
    static uint32_t buf;
    if (!buf)
        buf = gheap_alloc(4 * 28, 1);
    float x[4], u[4], v[4], ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f, xlo = 1e30f, xhi = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20)) * (float)t->width;
        v[i] = u2f(rd32(data + 28u * (uint32_t)i + 24)) * (float)t->height;
        ulo = u[i] < ulo ? u[i] : ulo, uhi = u[i] > uhi ? u[i] : uhi, vlo = v[i] < vlo ? v[i] : vlo, vhi = v[i] > vhi ? v[i] : vhi;
        xlo = x[i] < xlo ? x[i] : xlo, xhi = x[i] > xhi ? x[i] : xhi;
    }
    if (!(uhi > ulo) || !(xhi > xlo))
        return data;
    const float* best = NULL;
    for (uint32_t k = 0; k < e->nglyphs; ++k)
    {
        const float* g = e->glyphs + 4 * k;
        float cy = (g[2] + g[3]) * 0.5f;
        if (cy < vlo || cy > vhi || g[0] < ulo - 3.0f || g[0] >= uhi)
            continue;
        if (!best || g[0] < best[0])
            best = g;
    }
    if (!best)
        return data;
    float k = (xhi - xlo) / (uhi - ulo);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        float nu = u[i] - ulo < uhi - u[i] ? best[0] : best[1];
        wr32(buf + 28u * (uint32_t)i, f2u(x[i] + (nu - u[i]) * k));
        wr32(buf + 28u * (uint32_t)i + 20, f2u(nu / (float)t->width));
    }
    return buf;
}

/* A glyph quad (a strip of four XYZRHW + diffuse + UV vertices) from a texture whose pack entry
 * asks for it, drawn repl_pad texels wider on each side, its UVs moved with it: FFXI cuts each
 * glyph of its italic name font at an upright box, so the lean and the outline at the ends of a
 * name were lost (the neighbours hide it inside the name). A copy: the game's vertices stay. */
static uint32_t widen_quad(uint32_t data, const Obj* t)
{
    static uint32_t buf;
    if (!buf)
        buf = gheap_alloc(4 * 28, 1);
    float x[4], u[4], ulo = 1e30f, uhi = -1e30f, xlo = 1e30f, xhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        float v = u2f(rd32(data + 28u * (uint32_t)i + 24));
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20));
        ulo = u[i] < ulo ? u[i] : ulo, uhi = u[i] > uhi ? u[i] : uhi;
        xlo = x[i] < xlo ? x[i] : xlo, xhi = x[i] > xhi ? x[i] : xhi;
        vlo = v < vlo ? v : vlo, vhi = v > vhi ? v : vhi;
    }
    if (!(uhi > ulo) || !(xhi > xlo) || (vhi - vlo) * (float)t->height < (float)t->repl_pad_min)
        return data;
    float du = (float)t->repl_pad / (float)t->width, dx = du * (xhi - xlo) / (uhi - ulo);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        int lo = u[i] - ulo < uhi - u[i];
        wr32(buf + 28u * (uint32_t)i, f2u(x[i] + (lo ? -dx : dx)));
        wr32(buf + 28u * (uint32_t)i + 20, f2u(u[i] + (lo ? -du : du)));
    }
    return buf;
}

/* An axis-aligned sprite quad drawn point-sampled into the menu target drawn at the screen's size ([ui] native: native_size,
 * its pw set). At the menu resolution a quad's edges fall between pixels; enlarged (2.5x) an edge can fall on a pixel's
 * centre, which then samples the quad's texture exactly at its edge - and a hair past it, wrapping to the far side of the
 * sheet. FFXI draws its wider menu buttons as two strips side by side, the right one starting at u 0 of a sheet whose last
 * column is transparent there: the alpha test dropped that pixel, a black line down the button. The UVs are pulled in by
 * 1/64 of a texel, which point sampling doesn't otherwise show. A copy: the game's vertices stay. (From XI on Anything,
 * rubymatrix/xi-on-anything, d3d8.c inset_quad.) */
static uint32_t inset_quad(uint32_t data, const Obj* t)
{
    const Obj* rt = obj(g_dev.rt);
    const Obj* rtt = rt && rt->container ? obj(rt->container) : NULL;
    if (!rtt || !(rtt->grow & GROW_SCREEN) || !rtt->pw || g_dev.cur.tss[0][16] != 1 /* D3DTEXF_POINT */ || !t->width ||
        !t->height)
        return data;
    float ex = (1.0f / 64.0f) / (float)t->width, ey = (1.0f / 64.0f) / (float)t->height;
    float x[4], y[4], u[4], v[4];
    for (int i = 0; i < 4; ++i)
    {
        x[i] = u2f(rd32(data + 28u * (uint32_t)i)), y[i] = u2f(rd32(data + 28u * (uint32_t)i + 4));
        u[i] = u2f(rd32(data + 28u * (uint32_t)i + 20)), v[i] = u2f(rd32(data + 28u * (uint32_t)i + 24));
    }
    /* axis aligned: vertices 0 and 1 share a row, 0 and 2 a column (a strip) */
    if (fabsf(y[0] - y[1]) > 0.01f || fabsf(y[2] - y[3]) > 0.01f || fabsf(x[0] - x[2]) > 0.01f || fabsf(x[1] - x[3]) > 0.01f ||
        fabsf(v[0] - v[1]) > 1e-6f || fabsf(u[0] - u[2]) > 1e-6f)
        return data;
    static uint32_t buf;
    if (!buf)
        buf = gheap_alloc(4 * 28, 1);
    memcpy(GUEST_PTR(buf), GUEST_PTR(data), 4 * 28);
    float uc = (u[0] + u[1]) * 0.5f, vc = (v[0] + v[2]) * 0.5f;
    for (int i = 0; i < 4; ++i)
    {
        wr32(buf + 28u * (uint32_t)i + 20, f2u(u[i] + (u[i] < uc ? ex : u[i] > uc ? -ex : 0.0f)));
        wr32(buf + 28u * (uint32_t)i + 24, f2u(v[i] + (v[i] < vc ? ey : v[i] > vc ? -ey : 0.0f)));
    }
    return buf;
}

static void IDirect3DDevice8_DrawPrimitiveUP(Guest* g)
{
    g_cap_esp = g->esp;
    if (!(g_dev.cur.vs & 1) && (g_dev.cur.vs & 0xE) == 4 && ARG(3) && rd32(ARG(3) + 8) == 0x3f7ffffeu)
        g_probe_sky = 1; /* transformed vertices at the sky's depth */
    uint32_t data = ARG(3);
    const Obj* t0 = obj(g_dev.cur.tex[0]);
    if (t0 && t0->repl && ARG(1) == 5 && ARG(2) == 2 && ARG(4) == 28 && g_dev.cur.vs == 0x144)
    {
        if (t0->repl_entry && ((const PackEntry*)t0->repl_entry)->nglyphs)
            data = fit_quad(data, t0);
        else if (t0->repl_pad)
            data = widen_quad(data, t0);
    }
    else if (t0 && ARG(1) == 5 && ARG(2) == 2 && ARG(4) == 28 && g_dev.cur.vs == 0x144)
        data = inset_quad(data, t0);
    draw(ARG(1), ARG(2), 0, 0, 0, data, ARG(4));
    bind(&g_dev.cur.stream[0], 0);
    g_dev.cur.stride[0] = 0;
    RET(D3D_OK, 5);
}

/* DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData,
 * IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride) */
static void IDirect3DDevice8_DrawIndexedPrimitiveUP(Guest* g)
{
    g_cap_esp = g->esp;
    draw(ARG(1), ARG(4), 0, ARG(5), ARG(6) == FMT_INDEX32 ? 4 : 2, ARG(7), ARG(8));
    bind(&g_dev.cur.stream[0], 0);
    g_dev.cur.stride[0] = 0;
    bind(&g_dev.cur.ib, 0);
    RET(D3D_OK, 9);
}

/* --- IDirect3DDevice8: shaders and streams ------------------------------------------------------------------ */
static uint32_t* copy_tokens(uint32_t p, uint32_t end, uint32_t* n)
{
    uint32_t k = 0;
    while (k < 65536 && rd32(p + 4 * k) != end)
        k++;
    k++;
    uint32_t* t = (uint32_t*)malloc(4 * k);
    memcpy(t, GUEST_PTR(p), 4 * k);
    *n = k;
    return t;
}

static uint32_t type_size(uint32_t t)
{
    return t == GFX_FLOAT2 || t == GFX_SHORT4 ? 8 : t == GFX_FLOAT3 ? 12 : t == GFX_FLOAT4 ? 16 : 4;
}

static void add_elem(Layout* l, int reg, uint32_t stream, uint32_t type, uint32_t* off)
{
    if (reg >= GFX_NREGS)
        return;
    l->el[reg] = (GfxElem){ 1, (uint8_t)stream, (uint8_t)type, 0 };
    l->offset[reg] = (int32_t)*off;
    *off += type_size(type);
}

/* an FVF code: everything in stream 0, in D3D's order */
static void fvf_layout(uint32_t fvf, Layout* l)
{
    memset(l, 0, sizeof *l);
    uint32_t off = 0, pos = fvf & 0xE;
    if (pos == 4) /* XYZRHW */
    {
        add_elem(l, GFX_R_POSITION, 0, GFX_FLOAT4, &off);
        l->rhw = 1;
    }
    else if (pos)
    {
        add_elem(l, GFX_R_POSITION, 0, GFX_FLOAT3, &off);
        if (pos >= 6) /* XYZB1..5: blend weights, the last one indices with LASTBETA_UBYTE4 */
        {
            int betas = (int)(pos - 4) / 2, weights = betas - ((fvf & 0x1000) ? 1 : 0);
            if (weights > 4)
                weights = 4;
            if (weights > 0)
                add_elem(l, GFX_R_BLENDWEIGHT, 0, GFX_FLOAT1 + (uint32_t)weights - 1, &off);
            if (fvf & 0x1000)
                add_elem(l, GFX_R_BLENDINDICES, 0, GFX_UBYTE4, &off);
        }
    }
    if (fvf & 0x10)
        add_elem(l, GFX_R_NORMAL, 0, GFX_FLOAT3, &off);
    if (fvf & 0x20)
        add_elem(l, GFX_R_PSIZE, 0, GFX_FLOAT1, &off);
    if (fvf & 0x40)
        add_elem(l, GFX_R_DIFFUSE, 0, GFX_D3DCOLOR, &off);
    if (fvf & 0x80)
        add_elem(l, GFX_R_SPECULAR, 0, GFX_D3DCOLOR, &off);
    uint32_t n = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < n && i < 8; ++i)
    {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        add_elem(l, GFX_R_TEXCOORD0 + (int)i, 0, f == 0 ? GFX_FLOAT2 : f == 1 ? GFX_FLOAT3 : f == 2 ? GFX_FLOAT4 : GFX_FLOAT1, &off);
    }
}

/* a vertex shader declaration: D3DVSD_STREAM, then D3DVSD_REG / D3DVSD_SKIP per element */
static void decl_layout(const uint32_t* t, uint32_t n, Layout* l)
{
    memset(l, 0, sizeof *l);
    uint32_t stream = 0, off = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t tok = t[i];
        if (tok == 0xFFFFFFFFu)
            break;
        switch (tok >> 29)
        {
        case 1: stream = tok & 0xF, off = 0; break;
        case 2:
            if ((tok >> 28) & 1)
                off += 4 * ((tok >> 16) & 0xF);
            else
                add_elem(l, (int)(tok & 0x1F), stream, (tok >> 16) & 0xF, &off);
            break;
        case 4: i += 4 * ((tok >> 25) & 0xF); break; /* constants: SetVertexShader loads them */
        case 5: i += (tok >> 24) & 0x1F; break;
        default: break;
        }
    }
}

static uint32_t token_hash(const uint32_t* t, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i)
        for (int b = 0; b < 32; b += 8)
            h = (h ^ ((t[i] >> b) & 255)) * 16777619u;
    return h ? h : 1;
}

/* CreateVertexShader(pDeclaration, pFunction, pHandle, Usage). Handles are odd: D3D8 tells a shader
 * from an FVF code (bit 0 always clear) by that bit. */
static void IDirect3DDevice8_CreateVertexShader(Guest* g)
{
    Dev* d = &g_dev;
    uint32_t i = 0;
    while (i < d->nvs && d->vs[i].decl)
        i++;
    if (i == d->nvs)
    {
        d->vs = (Shader*)realloc(d->vs, (d->nvs + 1) * sizeof *d->vs);
        memset(&d->vs[d->nvs++], 0, sizeof(Shader));
    }
    Shader* s = &d->vs[i];
    s->decl = copy_tokens(ARG(1), 0xFFFFFFFFu, &s->ndecl);
    s->func = ARG(2) ? copy_tokens(ARG(2), 0x0000FFFFu, &s->nfunc) : NULL;
    if (!ARG(2))
        s->nfunc = 0;
    decl_layout(s->decl, s->ndecl, &s->lay);
    s->hash = s->func ? token_hash(s->func, s->nfunc) : 0;
    wr32(ARG(3), ((i + 1) << 1) | 1);
    RET(D3D_OK, 5);
}

static Shader* vs_of(uint32_t h)
{
    uint32_t i = (h >> 1) - 1;
    return (h & 1) && i < g_dev.nvs && g_dev.vs[i].decl ? &g_dev.vs[i] : NULL;
}

static void IDirect3DDevice8_SetVertexShader(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    s->vs = ARG(1);
    if (m)
        m->vs = 1;
    /* D3DVSD_CONST in the declaration: constants loaded with the shader */
    Shader* sh = vs_of(ARG(1));
    for (uint32_t i = 0; sh && i < sh->ndecl && sh->decl[i] != 0xFFFFFFFFu; ++i)
    {
        uint32_t tok = sh->decl[i];
        if (tok >> 29 == 4)
        {
            uint32_t count = (tok >> 25) & 0xF, reg = tok & 0x7F;
            for (uint32_t k = 0; k < count && reg + k < NVSC && i + 1 + 4 * k + 3 < sh->ndecl; ++k)
            {
                memcpy(s->vsc[reg + k], &sh->decl[i + 1 + 4 * k], 16);
                raise_top(&g_vsc_top, reg + k + 1);
                if (m)
                    m->vsc[reg + k] = 1;
            }
            i += 4 * count;
        }
        else if (tok >> 29 == 5)
            i += (tok >> 24) & 0x1F;
    }
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetVertexShader(Guest* g)
{
    wr32(ARG(1), g_dev.cur.vs);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeleteVertexShader(Guest* g)
{
    Shader* s = vs_of(ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    free(s->decl);
    free(s->func);
    memset(s, 0, sizeof *s);
    if (g_dev.cur.vs == ARG(1))
        g_dev.cur.vs = 0;
    RET(D3D_OK, 2);
}

/* Set/GetVertexShaderConstant(Register, pConstantData, ConstantCount) */
static void IDirect3DDevice8_SetVertexShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NVSC || n > NVSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    memcpy(s->vsc[r], ARGP(2), 16 * n);
    raise_top(&g_vsc_top, r + n);
    if (m)
        memset(&m->vsc[r], 1, n);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetVertexShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NVSC || n > NVSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    memcpy(ARGP(2), g_dev.cur.vsc[r], 16 * n);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetStreamSource(Guest* g)
{
    if (ARG(1) >= NSTREAMS)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    bind(&s->stream[ARG(1)], ARG(2));
    s->stride[ARG(1)] = ARG(3);
    if (m)
        m->stream[ARG(1)] = 1;
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetStreamSource(Guest* g)
{
    if (ARG(1) >= NSTREAMS)
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t p = g_dev.cur.stream[ARG(1)];
    if (p)
        obj_addref(p);
    wr32(ARG(2), p);
    wr32(ARG(3), g_dev.cur.stride[ARG(1)]);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_SetIndices(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    bind(&s->ib, ARG(1));
    s->base_vertex = ARG(2);
    if (m)
        m->ib = 1;
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetIndices(Guest* g)
{
    uint32_t p = g_dev.cur.ib;
    if (p)
        obj_addref(p);
    wr32(ARG(1), p);
    wr32(ARG(2), g_dev.cur.base_vertex);
    RET(D3D_OK, 3);
}

/* CreatePixelShader(pFunction, pHandle) */
static void IDirect3DDevice8_CreatePixelShader(Guest* g)
{
    Dev* d = &g_dev;
    uint32_t i = 0;
    while (i < d->nps && d->ps[i].func)
        i++;
    if (i == d->nps)
    {
        d->ps = (Shader*)realloc(d->ps, (d->nps + 1) * sizeof *d->ps);
        memset(&d->ps[d->nps++], 0, sizeof(Shader));
    }
    d->ps[i].func = copy_tokens(ARG(1), 0x0000FFFFu, &d->ps[i].nfunc);
    d->ps[i].hash = token_hash(d->ps[i].func, d->ps[i].nfunc);
    wr32(ARG(2), i + 1);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetPixelShader(Guest* g)
{
    Mask* m;
    State* s = target(&m);
    s->ps = ARG(1);
    if (m)
        m->ps = 1;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetPixelShader(Guest* g)
{
    wr32(ARG(1), g_dev.cur.ps);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_DeletePixelShader(Guest* g)
{
    uint32_t i = ARG(1) - 1;
    if (!ARG(1) || i >= g_dev.nps || !g_dev.ps[i].func)
        RET(D3DERR_INVALIDCALL, 2);
    free(g_dev.ps[i].func);
    memset(&g_dev.ps[i], 0, sizeof(Shader));
    if (g_dev.cur.ps == ARG(1))
        g_dev.cur.ps = 0;
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_SetPixelShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NPSC || n > NPSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    Mask* m;
    State* s = target(&m);
    memcpy(s->psc[r], ARGP(2), 16 * n);
    if (m)
        memset(&m->psc[r], 1, n);
    RET(D3D_OK, 4);
}

/* --- drawing: the device state as the back end's draw packet ------------------------------------------------ */
/* D3D's v * M with row-major matrices: r = a * b */
static void mat_mul(float* r, const float* a, const float* b)
{
    float t[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            t[y * 4 + x] = a[y * 4] * b[x] + a[y * 4 + 1] * b[4 + x] + a[y * 4 + 2] * b[8 + x] + a[y * 4 + 3] * b[12 + x];
    memcpy(r, t, sizeof t);
}

/* the normal matrix: the inverse transpose of m's upper 3x3 (D3D lights in camera space) */
static void normal_matrix(float* r, const float* m)
{
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float s = det != 0.0f ? 1.0f / det : 0.0f;
    memset(r, 0, 64);
    /* inverse transpose = cofactor matrix / det */
    r[0] = A * s, r[1] = B * s, r[2] = C * s;
    r[4] = -(b * i - c * h) * s, r[5] = (a * i - c * g) * s, r[6] = -(a * h - b * g) * s;
    r[8] = (b * f - c * e) * s, r[9] = -(a * f - c * d) * s, r[10] = (a * e - b * d) * s;
    r[15] = 1.0f;
}

static void xform(float* out, const float* v, float w, const float* m) /* (v, w) * m, xyz */
{
    for (int k = 0; k < 3; ++k)
        out[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + w * m[12 + k];
}

static void color4(float* out, uint32_t c)
{
    out[0] = ((c >> 16) & 255) / 255.0f, out[1] = ((c >> 8) & 255) / 255.0f, out[2] = (c & 255) / 255.0f, out[3] = (c >> 24) / 255.0f;
}

/* vertices a D3D primitive count spans */
static uint32_t prim_vertices(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5:
    case 6: return n + 2;
    default: return 0;
    }
}

/* VanaCore's own interface (d3d8_set_own_pixels): drawn into a layer of the window's size, not the back
 * buffer, which gfx_present lays over the frame: sharp and square whatever size the frame is drawn at.
 * Cleared on the first of its draws in a frame. */

static int ui_layer_bound(void)
{
    uint32_t w = 0, h = 0;
    d3d8_screen_size(&w, &h);
    if (!w || !h)
        return 0;
    if (!g_ui_layer || w != g_ui_w || h != g_ui_h)
    {
        gfx_tex_destroy(g_ui_layer);
        g_ui_layer = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, w, h, 1, GFX_USE_RT);
        g_ui_w = w, g_ui_h = h, g_ui_drawn = 0;
        if (!g_ui_layer)
            return 0;
    }
    gfx_set_targets(g_ui_layer, 0, 0, NULL);
    if (!g_ui_drawn)
    {
        uint32_t vp[6] = { 0, 0, w, h, 0, 0x3F800000u };
        gfx_clear(0, NULL, 1, 0x00000000u, 1.0f, 0, vp);
        g_ui_drawn = 1;
    }
    return 1;
}

/* [graphics] background (or DLSS) draws the scene's target and its depth surface at the scene's size (mark_scene).
 * A target of the same size drawn with that depth surface, or that target with another, would test its
 * pixels against the wrong ones: FFXI's sun probe draws a quad at the sky's depth into another target
 * with the scene's depth and counts what passes, so the sun and its flare went missing. The other one
 * is drawn at the scene's size too, from the next frame. */
static void match_scene_size(Obj* rt, Obj* ds)
{
    Obj* o = rt ? pixels_of(rt) : NULL;
    if (!(g_scene_scale > 0 || g_scene_w) || !o || !ds || ds->kind != O_SURFACE || !((o->grow ^ ds->grow) & GROW_SCENE))
        return;
    Obj* other = (o->grow & GROW_SCENE) ? ds : o;
    int fits = o->width == ds->width && o->height == ds->height &&
               (other == ds ? !ds->container
                            : (o->usage & USAGE_RENDERTARGET) && !(o->kind == O_TEXTURE && o->nsubs != 1));
    if (!fits)
    {
        static int said;
        if (said < 4)
            ++said, rt_log("[recomp] d3d8: a %ux%u target drawn with a %ux%u depth surface, one at the 3D scene's "
                           "size and one not: its depth test is off\n", o->width, o->height, ds->width, ds->height);
        return;
    }
    other->grow |= GROW_SCENE;
    g_resized = 0;
    rt_log("[recomp] d3d8: a %ux%u %s drawn with the 3D scene's %s: at the scene's size too\n", o->width, o->height,
        other == ds ? "depth surface" : "target", other == ds ? "target" : "depth surface");
}

static void apply_targets(void)
{
    if (g_own_pixels && g_dev.rt == g_dev.backbuffer && ui_layer_bound())
        return;
    uint32_t cf = 0, cl = 0, df, dl;
    Obj* rt = obj(g_dev.rt);
    Obj* ds = obj(g_dev.ds);
    match_scene_size(rt, ds);
    GfxTex* c = rt ? surface_gpu(rt, &cf, &cl) : NULL;
    GfxTex* z = ds ? surface_gpu(ds, &df, &dl) : NULL;
    gfx_set_targets(c, cf, cl, z);
}

static GfxDraw g_draw; /* one device, one draw at a time */

/* A declaration reading streams past the back ends' GFX_NSTREAMS: the character creation's models (FFXiMain's sqShape renderer)
 * draw from five, a buffer each for the positions, normals, two sets of texture coordinates and the colours (stream 4), and
 * their hair and the like were left out. Their elements are copied into one buffer of their own instead (pack_streams), in
 * the slot of the positions' stream (stream 0 when that's past the end too), so what reads the positions' buffer reads the
 * game's. g_packed is the declaration's layout while the draw is being made, NULL for the others. */
static const Layout* g_packed;
static uint8_t* g_pack_buf;
static size_t g_pack_cap;

/* the packed draw's elements, one after another in its slot */
static void pack_layout(GfxDraw* d, const Layout* lay)
{
    const GfxElem* p = &lay->el[GFX_R_POSITION];
    const uint8_t slot = p->used && p->stream < GFX_NSTREAMS ? p->stream : 0;
    uint32_t off = 0;
    for (int r = 0; r < GFX_NREGS; ++r)
    {
        d->vs.el[r] = lay->el[r];
        if (!lay->el[r].used)
            continue;
        d->vs.el[r].stream = slot;
        d->u.offset[r] = (int32_t)off;
        off += type_size(lay->el[r].type);
    }
}

/* Vertices first .. first + count of each of the declaration's elements into g_pack_buf, as pack_layout placed them; a
 * vertex past the end of its buffer reads zeros. 0 when a stream it reads has nothing bound, or starts past its buffer. */
static int pack_streams(GfxDraw* d, uint32_t first, uint32_t count)
{
    const Layout* lay = g_packed;
    uint32_t stride = 0;
    for (int r = 0; r < GFX_NREGS; ++r)
        if (lay->el[r].used)
            stride += type_size(lay->el[r].type);
    if (!stride || !count)
        return gfx_prof_skip(GFX_SKIP_RANGE), 0;
    size_t need = (size_t)count * stride;
    if (need > g_pack_cap)
    {
        uint8_t* grown = (uint8_t*)realloc(g_pack_buf, need);
        if (!grown)
            return gfx_prof_skip(GFX_SKIP_NO_BUFFER), 0;
        g_pack_buf = grown, g_pack_cap = need;
    }
    for (int r = 0; r < GFX_NREGS; ++r)
    {
        const GfxElem* e = &lay->el[r];
        if (!e->used)
            continue;
        Obj* b = obj(g_dev.cur.stream[e->stream]);
        if (!b || !b->mem)
            return gfx_prof_skip(GFX_SKIP_NO_BUFFER), 0;
        const uint32_t from = g_dev.cur.stride[e->stream], size = type_size(e->type);
        const uint64_t start = (uint64_t)first * from + (uint32_t)lay->offset[r];
        if (start >= b->size)
            return gfx_prof_skip(GFX_SKIP_RANGE), 0;
        const uint8_t* src = GUEST_PTR(b->mem);
        uint8_t* dst = g_pack_buf + d->u.offset[r];
        for (uint32_t i = 0; i < count; ++i, dst += stride)
        {
            const uint64_t at = start + (uint64_t)i * from;
            if (at + size <= b->size)
                memcpy(dst, src + at, size);
            else
                memset(dst, 0, size);
        }
    }
    const int slot = d->vs.el[GFX_R_POSITION].used ? d->vs.el[GFX_R_POSITION].stream : 0;
    d->data[slot] = d->cpu[slot] = g_pack_buf;
    d->size[slot] = d->cpu_size[slot] = (uint32_t)need;
    d->u.stride[slot] = (int32_t)stride;
    return 1;
}

/* The keys and uniforms for the current state; 0 when the state cannot be drawn. */
/* The moon, for [graphics.effects] moon_size and moon_brightness. FFXI places its sun and moon from the camera, each a model
 * made once and drawn blended, unfogged and without writing depth, in a view with no translation (XiArea::SetSunMoonTime: the
 * sun 900 units from the eye, the moon 1000). Taken here by that distance: one draw of the sky's layers nearer than a world
 * but not an effect (camera_effect's 500). Its model is centred on its world matrix's translation, so scaling the matrix's
 * axes grows it where it stands; the brightness goes into the texture factor the sky's blending reads. */
static void moon_adjust(GfxDraw* d, const State* s)
{
    if (g_gfx_moon_size == 1.0f && g_gfx_moon_bright == 1.0f)
        return;
    if (d->vs.rhw || !d->pipe.blend || d->depth.zwrite || d->fs.fog || d->vs.fog_vertex)
        return;
    const float* V = s->xf[2];
    if (V[12] != 0.0f || V[13] != 0.0f || V[14] != 0.0f)
        return;
    const float* W = s->xf[24];
    const float r2 = W[12] * W[12] + W[13] * W[13] + W[14] * W[14];
    if (r2 < 950.0f * 950.0f || r2 > 1050.0f * 1050.0f)
        return;
    if (g_gfx_moon_size != 1.0f)
    {
        float M[16], wv[16];
        memcpy(M, W, sizeof M);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                M[row * 4 + col] *= g_gfx_moon_size;
        mat_mul(wv, M, V);
        mat_mul(d->u.wvp, wv, s->xf[3]);
        memcpy(d->u.wv, wv, 64);
        normal_matrix(d->u.wvit, wv);
    }
    if (g_gfx_moon_bright != 1.0f)
    {
        for (int i = 0; i < 3; ++i)
            d->u.tfactor[i] *= g_gfx_moon_bright;
        d->u.tfactor[3] = fminf(d->u.tfactor[3] * g_gfx_moon_bright, 1.0f);
    }
}

/* The world's draws' look as /fillmode and [graphics] untextured ask - not the interface's (rhw) or VanaCore's own (g_own_pixels).
 * /fillmode: this D3DFILLMODE in place of the game's. untextured: an opaque draw's base texture (stage 0) swapped for a white texel,
 * so the surface shows its light and vertex colours alone; an alpha-tested or blended draw's texture is its shape, and cube,
 * projected and render-target textures aren't a surface's pattern, so those stay. */
static void world_look(GfxDraw* d, const State* s)
{
    if (d->vs.rhw || g_own_pixels)
        return;
    if (g_fill_mode)
        d->fill = (uint8_t)g_fill_mode;
    if (!g_untextured || (d->fs.alpha_func && d->fs.alpha_func != 8) || d->pipe.blend)
        return;
    if (!d->tex[0] || d->fs.st[0].tex != 1 || d->fs.st[0].projected)
        return;
    const Obj* to = obj(s->tex[0]);
    if (!to || (to->usage & USAGE_RENDERTARGET))
        return;
    if (!g_plain && (g_plain = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, 1, 1, 1, GFX_USE_SAMPLE)) != NULL)
    {
        const uint32_t white = 0xFFFFFFFFu;
        gfx_tex_upload(g_plain, 0, 0, &white, 4);
    }
    if (!g_plain)
        return;
    d->tex[0] = g_plain;
    d->samp[0].max_level = 0, d->samp[0].lod_cap = 0;
}

static int build_draw(GfxDraw* d)
{
    State* s = &g_dev.cur;
    const uint32_t* rs = s->rs;
    /* the keys and bindings, and the uniforms up to the lights; lights and shader constants are
     * written where they are used (the back end uploads only those) */
    memset(d, 0, offsetof(GfxDraw, u));
    memset(&d->u, 0, offsetof(GfxU, light));
    memset(d->tex, 0, sizeof(GfxDraw) - offsetof(GfxDraw, tex));
    /* the target's size as the game made it, not as drawn: [graphics] glow draws the game's 256x256 glow targets at up to
     * 2048, and the scene filter is for large targets only (scene_mips) */
    {
        const Obj* rt = obj(g_dev.rt);
        const Obj* px = rt ? pixels_of(rt) : NULL;
        d->small_rt = px && ((px->grow & GROW_GLOW) || (px->width && px->width < 512) || (px->height && px->height < 512));
    }

    static Layout fvf; /* the last FVF's: the same code draw after draw */
    static uint32_t fvf_code = 0xFFFFFFFFu;
    const Layout* lay;
    Shader* vs = NULL;
    if (s->vs & 1)
    {
        vs = vs_of(s->vs);
        if (!vs)
            return gfx_prof_skip(GFX_SKIP_NO_SHADER), 0;
        lay = &vs->lay;
    }
    else
    {
        if (s->vs != fvf_code)
            fvf_layout(s->vs, &fvf), fvf_code = s->vs;
        lay = &fvf;
    }
    g_packed = NULL;
    for (int r = 0; r < GFX_NREGS; ++r)
        if (lay->el[r].used && lay->el[r].stream >= GFX_NSTREAMS)
            g_packed = lay;
    if (g_packed)
        pack_layout(d, lay);
    else
        for (int r = 0; r < GFX_NREGS; ++r)
        {
            d->vs.el[r] = lay->el[r];
            d->u.offset[r] = lay->offset[r];
        }
    if (!lay->el[GFX_R_POSITION].used && !(vs && vs->func))
        return gfx_prof_skip(GFX_SKIP_NO_POSITION), 0;
    d->vs.rhw = lay->rhw;
    /* A render target copied at another size, as FFXI's trust and avatar glow copies its 4096 picture of the
     * character to 256x256 and lays that back over the scene: drawn with today's pixel centres. With D3D8's
     * (vertices on pixel centres, texture coordinates on texel corners) each such copy moved its picture half a
     * texel down and right, and at a large background resolution the glow sat 16 pixels off the character. */
    if (d->vs.rhw)
    {
        const Obj* t0 = obj(s->tex[0]);
        const Obj* to = obj(g_dev.rt);
        if (t0 && to && (t0->usage & USAGE_RENDERTARGET) && (t0->width != to->width || t0->height != to->height))
        {
            d->vs.centered = 1;
            /* the glow's own small target, the character's picture shrunk into it: FFXI makes it 256x256 whatever
             * its background's size, and with a 4096 one each of its texels was 16 pixels (the trust or avatar it
             * lays under itself came out a blur) - drawn at a quarter of the largest target, the ratio it was made
             * for. Not the sun flare's 16x16 occlusion probe. */
            Obj* small = pixels_of(to);
            if ((!small->grow || (small->grow & GROW_GLOW)) && small->kind == O_TEXTURE && small->nsubs == 1 &&
                small->width == small->height && small->width >= 64 && small->width <= 512 && t0->width >= 4 * small->width)
            {
                if (!small->grow)
                    small->grow = GROW_GLOW, g_resized = 0;
                g_glow_dst = small->guest, g_glow_src = s->tex[0];
            }
        }
    }
    if (vs && vs->func)
    {
        d->vs.prog = vs->hash;
        d->vs_tokens = vs->func;
        memcpy(d->u.vsc, s->vsc, sizeof d->u.vsc);
    }
    if (s->ps)
    {
        uint32_t i = s->ps - 1;
        if (i < g_dev.nps && g_dev.ps[i].func)
        {
            d->fs.prog = g_dev.ps[i].hash;
            d->ps_tokens = g_dev.ps[i].func;
            memcpy(d->u.psc, s->psc, sizeof d->u.psc);
        }
    }

    /* transforms: WORLD is D3DTS_WORLDMATRIX(0), index 24 here */
    float wv[16];
    mat_mul(wv, s->xf[24], s->xf[2]);
    mat_mul(d->u.wvp, wv, s->xf[3]);
    memcpy(d->u.wv, wv, 64);
    normal_matrix(d->u.wvit, wv);
    for (int i = 0; i < 8; ++i)
        memcpy(d->u.texm[i], s->xf[16 + i], 64);

    /* D3DMATERIAL8: Diffuse, Ambient, Specular, Emissive, Power */
    memcpy(d->u.mat_d, &s->mat[0], 16);
    memcpy(d->u.mat_a, &s->mat[4], 16);
    memcpy(d->u.mat_s, &s->mat[8], 16);
    memcpy(d->u.mat_e, &s->mat[12], 16);
    d->u.params[0] = u2f(s->mat[16]);
    d->u.params[1] = (float)(rs[24] & 255); /* ALPHAREF */
    d->u.params[2] = u2f(rs[36]);           /* FOGSTART */
    d->u.params[3] = u2f(rs[37]);           /* FOGEND */
    d->u.params2[0] = u2f(rs[38]);          /* FOGDENSITY */
    color4(d->u.ambient, rs[139]);
    d->u.ambient[3] = 0.0f; /* the shaders don't read the ambient's alpha: it carries 1 - characters.brightness (character_light) */
    color4(d->u.tfactor, rs[60]);
    color4(d->u.fogcolor, rs[34]);
    for (int i = 0; i < 4; ++i)
        d->u.vp[i] = (float)s->vp[i];
    memcpy(d->vp, s->vp, sizeof d->vp);
    /* the XYZRHW mapping (u.vp) stays in the game's pixels; VanaCore's own drawing is in the drawn ones */
    if (!(g_own_pixels && g_dev.rt == g_dev.backbuffer))
        scale_to_target(d->vp, NULL, 0);

    int ff_vertex = !d->vs.prog && !lay->rhw;
    if (ff_vertex && rs[137]) /* LIGHTING */
    {
        d->vs.lighting = 1;
        d->vs.normalize = rs[143] != 0;
        d->vs.localviewer = rs[142] != 0;
        d->vs.specular = rs[29] != 0;
        if (rs[141]) /* COLORVERTEX: the material sources */
        {
            d->vs.src_diffuse = (uint8_t)rs[145], d->vs.src_specular = (uint8_t)rs[146];
            d->vs.src_ambient = (uint8_t)rs[147], d->vs.src_emissive = (uint8_t)rs[148];
        }
        const float* view = s->xf[2];
        int n = 0;
        for (int i = 0; i < g_light_top && n < GFX_NLIGHTS; ++i)
        {
            const Light* l = &s->light[i];
            uint32_t type = l->v[0];
            if (!l->enabled || type < 1 || type > 3)
                continue;
            /* D3DLIGHT8: Type, Diffuse, Specular, Ambient, Position, Direction, Range, Falloff,
             * Attenuation0..2, Theta, Phi */
            float v[26];
            memcpy(v, l->v, sizeof v);
            GfxLight* gl = &d->u.light[n];
            memcpy(gl->diffuse, &v[1], 16);
            memcpy(gl->specular, &v[5], 16);
            memcpy(gl->ambient, &v[9], 16);
            xform(gl->pos, &v[13], 1.0f, view);
            gl->pos[3] = v[19];
            float dir[3];
            xform(dir, &v[16], 0.0f, view);
            float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            float k = len > 0 ? (type == 3 ? -1.0f : 1.0f) / len : 0.0f; /* directional: toward the light */
            gl->dir[0] = dir[0] * k, gl->dir[1] = dir[1] * k, gl->dir[2] = dir[2] * k, gl->dir[3] = v[20];
            if (type == 3)
                memcpy(g_sun_view, gl->dir, sizeof g_sun_view);
            gl->att[0] = v[21], gl->att[1] = v[22], gl->att[2] = v[23];
            gl->spot[0] = cosf(v[24] * 0.5f), gl->spot[1] = cosf(v[25] * 0.5f);
            d->vs.light_type[n++] = (uint8_t)type;
        }
        d->vs.nlights = (uint8_t)n;
    }

    if (rs[28]) /* FOGENABLE: table (pixel) fog, else vertex fog, else the vertices' specular alpha */
    {
        if (rs[35])
            d->fs.fog = (uint8_t)rs[35];
        else
        {
            d->fs.fog = 4;
            if (ff_vertex)
                d->vs.fog_vertex = (uint8_t)rs[140];
        }
        d->vs.range_fog = ff_vertex && rs[48];
    }

    /* the texture stages, up to the first disabled one */
    int nst = 0;
    if (!d->fs.prog)
        while (nst < 8 && s->tss[nst][1] != 1)
            nst++;
    d->fs.nstages = (uint8_t)nst;
    int nused = d->fs.prog ? 4 : nst, repl0 = 0;
    d->vs.ntex = (uint8_t)(d->vs.prog || d->fs.prog ? 8 : nst);
    for (int i = 0; i < nused; ++i)
    {
        const uint32_t* t = s->tss[i];
        GfxStage* st = &d->fs.st[i];
        int kind = 0;
        GfxTex* tex = texture_for_draw(s->tex[i], &kind);
        if (tex)
        {
            d->tex[i] = tex;
            st->tex = (uint8_t)kind;
            Obj* to = obj(s->tex[i]);
            uint32_t maxlevel = t[20] > to->lod ? t[20] : to->lod;
            d->samp[i] = (GfxSampler){ (uint8_t)t[13], (uint8_t)t[14], (uint8_t)t[25], (uint8_t)t[16], (uint8_t)t[17], (uint8_t)t[18],
                (uint8_t)t[21], (uint8_t)maxlevel, 0, { 0 }, t[15] };
            if (to->repl && tex == to->repl) /* drawn smaller than it is: linear, between its mipmaps */
            {
                d->samp[i].mag = d->samp[i].min = d->samp[i].mip = 2, d->samp[i].max_level = 0;
                d->samp[i].lod_cap = 2; /* its half-size mip at most: smaller ones blend neighbouring glyphs in */
                if (i == 0)
                    repl0 = 1;
            }
        }
        else if (s->tex[i])
            unresolved_texture(s->tex[i], i);
        if (!d->fs.prog)
        {
            st->cop = (uint8_t)t[1], st->ca1 = (uint8_t)t[2], st->ca2 = (uint8_t)t[3], st->ca0 = (uint8_t)t[26];
            st->aop = (uint8_t)t[4], st->aa1 = (uint8_t)t[5], st->aa2 = (uint8_t)t[6], st->aa0 = (uint8_t)t[27];
            st->result = (uint8_t)t[28];
        }
        uint32_t ttf = t[24], count = ttf & 0xFF;
        st->ncoord = (uint8_t)(count ? count : 2);
        st->projected = (ttf & 256) && count > 1;
        if (!d->vs.prog)
        {
            d->vs.tci[i] = (uint8_t)((t[11] & 7) | ((t[11] >> 16) & 0xF) << 4);
            d->vs.ttf[i] = (uint8_t)(count | ((ttf & 256) ? 0x80 : 0));
        }
    }
    /* a normal map for stage 0's texture rides in stage 7's slot (the stages in use never reach it). The game's own pixel shaders
     * (a zone's walls and ground, with a vertex shader) take it too, once a lit draw has shown where the sun is */
    if (normal_map_mode() && s->tex[0])
    {
        const Obj* n0 = obj(s->tex[0]);
        const float* proj = s->xf[3];
        int shaders = d->fs.prog != 0;
        int sun = g_sun_view[0] != 0.0f || g_sun_view[1] != 0.0f || g_sun_view[2] != 0.0f;
        int ok = d->tex[0] && d->fs.st[0].tex == 1 && !d->fs.st[0].projected &&
            (shaders ? d->vs.prog && proj[0] != 0.0f && proj[5] != 0.0f && sun : nst <= 7);
        if (n0 && n0->kind == O_TEXTURE && n0->nrm && ok)
        {
            static int said[2];
            if (!said[shaders]++)
                rt_log("[recomp] textures: a %s draw with a normal map (%ux%u)\n", shaders ? "shader" : "fixed-function", n0->width, n0->height);
            d->tex[7] = n0->nrm;
            d->fs.st[7].tex = 1;
            d->fs.nmap = (uint8_t)normal_map_mode();
            d->samp[7] = d->samp[0];
            d->samp[7].mag = d->samp[7].min = d->samp[7].mip = 2, d->samp[7].max_level = 0, d->samp[7].lod_cap = 0;
            if (shaders) /* the shader's own constants stay; these the draw's functions don't read otherwise */
            {
                memcpy(d->u.tfactor, g_sun_view, sizeof g_sun_view);
                d->u.params2[2] = 1.0f / proj[0], d->u.params2[3] = 1.0f / proj[5];
            }
        }
        else if (n0 && n0->kind == O_TEXTURE && n0->nrm)
        {
            static int said_off;
            if (said_off < 12)
                said_off++, rt_log("[recomp] textures: a normal map left out on a %ux%u draw: pixel shader %d, vertex shader %d, stages %d, "
                    "stage 0 kind %d, projected %d, sun known %d\n", n0->width, n0->height, shaders, d->vs.prog != 0, nst, d->fs.st[0].tex,
                    d->fs.st[0].projected, sun);
        }
    }
    d->fs.alpha_func = (uint8_t)(rs[15] ? rs[25] : 0);
    d->fs.specular_add = !d->fs.prog && rs[29];
    d->vs.flat = d->fs.flat = rs[9] == 1; /* SHADEMODE flat */

    d->pipe.blend = rs[27] != 0;
    if (d->pipe.blend)
        d->pipe.src = (uint8_t)rs[19], d->pipe.dst = (uint8_t)rs[20], d->pipe.op = (uint8_t)rs[171];
    /* FFXI draws its text adding (ONE, ONE): its font's outline can only brighten what is behind it.
     * A replacement drawn that way has its colour premultiplied by alpha (make_texpack --additive), so
     * over (ONE, INVSRCALPHA) draws the same fill and lets the outline darken, as the game's other
     * lettering does. */
    if (repl0 && d->pipe.blend && d->pipe.src == 2 && d->pipe.dst == 2 && d->pipe.op == 1)
        d->pipe.dst = 6;
    d->pipe.write_mask = (uint8_t)(rs[168] & 0xF);
    d->pipe.over = (uint8_t)(g_own_pixels && g_dev.rt == g_dev.backbuffer); /* into the interface layer */
    if (g_dev.ds)
    {
        d->depth.zenable = rs[7] != 0, d->depth.zwrite = rs[14] != 0, d->depth.zfunc = (uint8_t)rs[23];
        if (rs[52]) /* STENCILENABLE */
        {
            d->depth.stencil = 1;
            d->depth.sfail = (uint8_t)rs[53], d->depth.szfail = (uint8_t)rs[54], d->depth.spass = (uint8_t)rs[55];
            d->depth.sfunc = (uint8_t)rs[56], d->depth.sread = (uint8_t)rs[58], d->depth.swrite = (uint8_t)rs[59];
            d->stencil_ref = rs[57];
        }
    }
    d->cull = (uint8_t)rs[22];
    d->fill = (uint8_t)rs[8];
    world_look(d, s); /* /fillmode and untextured: the world's draws, not the interface's or VanaCore's own */
    d->zbias = (int32_t)rs[47];
    const Obj* fwd = obj(s->tex[0]);
    if (fwd && fwd->fwd && !d->vs.rhw)
        d->zbias += fwd->fwd;
    /* (the zone's decals are also drawn nearer the camera: decal_pull, once the draw's vertices are known) */
    moon_adjust(d, s);
    return 1;
}

/* Points the draw at vertices first .. first + count of the streams its layout reads: the bound
 * vertex buffers, or the ...UP draw's memory as stream 0. */
static int set_streams(GfxDraw* d, uint32_t first, uint32_t count, uint32_t up_data, uint32_t up_stride)
{
    if (g_packed) /* more streams than the back ends take: copied into one (pack_layout) */
        return up_data ? (gfx_prof_skip(GFX_SKIP_STREAM), 0) : pack_streams(d, first, count);
    int used[GFX_NSTREAMS] = { 0 };
    for (int r = 0; r < GFX_NREGS; ++r)
        if (d->vs.el[r].used)
            used[d->vs.el[r].stream] = 1;
    for (int st = 0; st < GFX_NSTREAMS; ++st)
    {
        if (!used[st])
            continue;
        uint32_t stride, base, size;
        if (up_data)
        {
            if (st)
                return gfx_prof_skip(GFX_SKIP_STREAM), 0;
            stride = up_stride, base = up_data, size = 0xFFFFFFFFu;
        }
        else
        {
            const Obj* b = obj(g_dev.cur.stream[st]);
            if (!b || !b->mem)
                return gfx_prof_skip(GFX_SKIP_NO_BUFFER), 0;
            stride = g_dev.cur.stride[st], base = b->mem, size = b->size;
        }
        uint32_t start = first * stride, bytes = stride ? count * stride : 64;
        if (start >= size)
            return gfx_prof_skip(GFX_SKIP_RANGE), 0;
        if (bytes > size - start)
            bytes = size - start;
        d->cpu[st] = GUEST_PTR(base + start), d->cpu_size[st] = bytes; /* for the shadow cache's copies */
        GfxBuf* gb = up_data ? NULL : static_buffer(obj(g_dev.cur.stream[st]));
        if (gb && !(start & 3))
        {
            d->buf[st] = gb, d->buf_off[st] = start;
            d->u.stride[st] = (int32_t)stride;
            continue;
        }
        d->data[st] = GUEST_PTR(base + start);
        d->size[st] = bytes;
        d->u.stride[st] = (int32_t)stride;
    }
    return 1;
}

static void index_range(uint32_t indices, uint32_t size, uint32_t n, uint32_t* lo, uint32_t* hi)
{
    uint32_t a = 0xFFFFFFFFu, b = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t v = size == 4 ? rd32(indices + 4 * i) : rd16(indices + 2 * i);
        a = v < a ? v : a;
        b = v > b ? v : b;
    }
    *lo = a, *hi = b;
}

/* --ui-aspect (user32_set_ui_aspect): FFXI lays its interface out in a fixed space and stretches it to the back
 * buffer, so a window wider than that space stretches the interface. Its menus, text and cursor are pre-transformed
 * (XYZRHW) draws to the back buffer; each is squeezed toward the middle by mapping the viewport's width onto a
 * centered box of the aspect asked for (the XYZRHW mapping reads d->u.vp). Draws that span the whole width - the 3D
 * scene put on the screen, fades, letterboxing - are left full. With a menu resolution other than the window's, FFXI
 * draws the interface into a render-target texture of that size (the menu target) and puts it on the screen as one
 * full-width quad: the draws into it are squeezed the same way, so its full-width ones (the dimming behind a menu,
 * the lobby's copy of the 3D scene) still cover the screen. The mouse is mapped back in user32.c. */
/* Bars across the whole width (the lobby's help bar): a full-width draw no taller than this share of
 * the height is a bar's body, and the frame drawn over it after (edge lines inset by a corner's
 * width, corner pieces at the ends) goes out to the screen's edges with it. Reset at Present. */
#define UI_BAR_MAX_H 0.15f
#define UI_MAX_BARS 8
static struct
{
    uint32_t rt;
    float y0, y1;
} g_ui_bars[UI_MAX_BARS];
static int g_ui_nbars;

/* Where the interface was drawn, on a coarse grid over the screen: this frame's, and the last
 * finished one's, which user32 asks (user32_ui_hit) to tell the interface from the world under the
 * mouse. g_ui_composited: the menu target is on the screen this frame, so what goes over it after
 * is the game's cursor. */
#define UI_GW 128
#define UI_GH 64
static uint8_t g_ui_grid[2][UI_GH][UI_GW];
static int g_ui_cur, g_ui_composited;
#ifdef VANACORE
/* 0 while VanaCore and its plugins draw their own screen-space things: left where they're drawn */
static int g_squeeze = 1;

void d3d8_set_squeeze(int on)
{
    if (!on && g_squeeze)
        chatfont_flush(); /* (True Font's letters onto the screen before VanaCore's own draws go over them) */
    g_squeeze = on != 0;
}

/* Something of the game's was squeezed this frame, and its interface (laid out w x h, the menu resolution) is drawn where
 * the squeeze applies: straight onto the back buffer, or into a menu target of the aspect box's shape. A menu target of
 * another shape (1920x1200 for a 16:9 interface in a 16:10 window) is left filling the screen, though a quad the game
 * draws straight onto the back buffer each frame is squeezed and marks the frame: the Network window's dot went a tenth
 * of the height under "Connected". */
int d3d8_ui_squeezed(uint32_t w, uint32_t h)
{
    return g_ui_squeezed && ((w == g_dev.pp[0] && h == g_dev.pp[1]) || ui_box_shape(w, h));
}

/* Plugins' own drawing moved and scaled (Addons > Layout, the HUD Manager's Addon View: d3d8_set_mover around each
 * plugin's drawing callbacks): its screen-space draws onto the back buffer go x to ax + scale x and y to ay + scale y
 * (mover_draw), and the box of where it put them (unmoved) is noted for it, the last finished frame's kept for
 * d3d8_mover_box. A slot each plugin, given by VanaCore. */
#define MOVERS 32
static struct
{
    float now[4], last[4]; /* x0, y0, x1, y1 */
    int drawn_now, drawn_last;
} g_movers[MOVERS];
static int g_mover = -1;
static float g_mover_ax, g_mover_ay, g_mover_scale = 1.0f;

void d3d8_set_mover(int slot, float ax, float ay, float scale)
{
    g_mover = slot >= 0 && slot < MOVERS ? slot : -1;
    g_mover_ax = ax, g_mover_ay = ay;
    g_mover_scale = scale > 0.0f ? scale : 1.0f;
}

int d3d8_mover_box(int slot, float* box)
{
    if (slot < 0 || slot >= MOVERS || !g_movers[slot].drawn_last)
        return 0;
    memcpy(box, g_movers[slot].last, sizeof g_movers[slot].last);
    return 1;
}

static void movers_present(void)
{
    for (int i = 0; i < MOVERS; ++i)
    {
        memcpy(g_movers[i].last, g_movers[i].now, sizeof g_movers[i].now);
        g_movers[i].drawn_last = g_movers[i].drawn_now;
        g_movers[i].drawn_now = 0;
    }
}

static float g_focus[4]; /* where the player stands (d3d8_set_focus), into each scene */
static float g_zonelines[GFX_ZONELINES][8]; /* the zone's lines (d3d8_set_zonelines), into each scene */
static uint32_t g_nzonelines;
static float g_zoneline_color[4];
static uint32_t g_zoneline_style, g_zoneline_glow;
static float g_zoneline_height = 0.5f, g_zoneline_size = 1.0f, g_zoneline_fade;

void d3d8_set_zonelines(const float* boxes, uint32_t count, const float color[4], uint32_t style, int glow, float height,
                        float size, float fade)
{
    g_zoneline_height = height, g_zoneline_size = size, g_zoneline_fade = fade;
    g_zoneline_style = style < 5 ? style : 0, g_zoneline_glow = glow != 0;
    g_nzonelines = count < GFX_ZONELINES ? count : GFX_ZONELINES;
    if (g_nzonelines)
        memcpy(g_zonelines, boxes, sizeof(float) * 8 * g_nzonelines);
    memcpy(g_zoneline_color, color, sizeof g_zoneline_color);
}

static float g_sparkles[GFX_SPARKLES][GFX_SPARKLE_FLOATS]; /* sparkles' places and auras (d3d8_set_sparkles), into each scene */
static uint32_t g_nsparkles;
static float g_sparkle_size = 1.0f, g_sparkle_density = 1.0f, g_sparkle_speed = 1.0f;
static uint32_t g_sparkle_match;

void d3d8_set_sparkles(const float* places, uint32_t count, float size, float density, float speed, int match)
{
    g_nsparkles = count < GFX_SPARKLES ? count : GFX_SPARKLES;
    if (g_nsparkles)
        memcpy(g_sparkles, places, sizeof g_sparkles[0] * g_nsparkles);
    g_sparkle_size = size, g_sparkle_density = density, g_sparkle_speed = speed;
    g_sparkle_match = match != 0;
}

/* rings of light on the ground (d3d8_set_ground_marks), into each scene */
static float g_ground_marks[GFX_GROUND_MARKS][GFX_GROUND_MARK_FLOATS];
static uint32_t g_nground_marks;

void d3d8_set_ground_marks(const float* marks, uint32_t count)
{
    g_nground_marks = count < GFX_GROUND_MARKS ? count : GFX_GROUND_MARKS;
    if (g_nground_marks)
        memcpy(g_ground_marks, marks, sizeof g_ground_marks[0] * g_nground_marks);
}

void d3d8_set_focus(float x, float y, float z, int known)
{
    g_focus[0] = x, g_focus[1] = y, g_focus[2] = z, g_focus[3] = known ? 1.0f : 0.0f;
}

void d3d8_set_own_pixels(int on)
{
    if (on && !g_own_pixels)
        chatfont_flush(); /* (as d3d8_set_squeeze) */
    g_own_pixels = on != 0;
}

int d3d8_ui_layer(uint32_t* w, uint32_t* h, void* pixels, uint32_t pitch)
{
    if (!g_ui_layer || !g_ui_drawn)
        return 0;
    *w = g_ui_w, *h = g_ui_h;
    if (pixels)
        gfx_tex_read(g_ui_layer, 0, 0, pixels, pitch);
    return 1;
}

/* d3d8_raise_take / d3d8_raise_put: the copies (in the back buffer's drawn pixels, each at its copy's top left) and where
 * they came from. VanaCore takes the game's windows that go over addons (the auto-translate list, the windows over
 * fancychat's chat) at the scene's end, before addons draw over them there (fancychat's chat, in the back buffer), and
 * puts them back after its own interface (ImGui, in the layer) is drawn. A copy's texture is kept for the next frames,
 * grown when a bigger one comes. */
typedef struct
{
    GfxTex* tex;
    uint32_t tw, th, fmt;
    uint32_t at[4]; /* x, y, w, h */
} Raised;
static Raised g_raised[D3D8_RAISE_MOST];

void d3d8_raise_take(int x, int y, int w, int h)
{
    if (g_raise_taken >= D3D8_RAISE_MOST)
        return;
    chatfont_flush(); /* (True Font's letters onto the screen first: the copies hold the game's windows as drawn) */
    Obj* bb = obj(g_dev.backbuffer);
    uint32_t face, level;
    GfxTex* src = bb ? surface_gpu(bb, &face, &level) : NULL;
    if (!src)
        return;
    int r = x + w, b = y + h;
    x = x > 0 ? x : 0, y = y > 0 ? y : 0;
    r = r < (int)bb->width ? r : (int)bb->width, b = b < (int)bb->height ? b : (int)bb->height;
    if (r <= x || b <= y)
        return;
    double kx = 1, ky = 1;
    surface_grown(bb, &kx, &ky);
    uint32_t gx = (uint32_t)(x * kx), gy = (uint32_t)(y * ky);
    uint32_t gw = (uint32_t)ceil(r * kx) - gx, gh = (uint32_t)ceil(b * ky) - gy;
    Raised* raised = &g_raised[g_raise_taken];
    if (!raised->tex || raised->tw < gw || raised->th < gh || raised->fmt != bb->format)
    {
        gfx_tex_destroy(raised->tex);
        raised->tw = gw > raised->tw ? gw : raised->tw, raised->th = gh > raised->th ? gh : raised->th;
        raised->fmt = bb->format;
        raised->tex = gfx_tex_create(GFX_TEX_2D, bb->format, raised->tw, raised->th, 1, GFX_USE_RT);
        if (!raised->tex)
            return;
    }
    gfx_copy(src, face, level, gx, gy, gw, gh, raised->tex, 0, 0, 0, 0);
    raised->at[0] = gx, raised->at[1] = gy, raised->at[2] = gw, raised->at[3] = gh;
    ++g_raise_taken;
}

void d3d8_raise_put(void)
{
    const int taken = g_raise_taken;
    if (!taken)
        return;
    g_raise_taken = 0;
    Obj* bb = obj(g_dev.backbuffer);
    uint32_t face, level;
    GfxTex* dst = bb ? surface_gpu(bb, &face, &level) : NULL;
    if (!dst)
        return;
    /* the layer covers the window as the back buffer does: the same place, in its pixels */
    double kx = 1, ky = 1;
    surface_grown(bb, &kx, &ky);
    const double pw = bb->width * kx, ph = bb->height * ky;
    int32_t rects[D3D8_RAISE_MOST * 4];
    uint32_t cut = 0;
    for (int i = 0; i < taken; ++i)
    {
        const Raised* raised = &g_raised[i];
        if (!raised->tex)
            continue;
        const uint32_t gx = raised->at[0], gy = raised->at[1], gw = raised->at[2], gh = raised->at[3];
        gfx_copy(raised->tex, 0, 0, 0, 0, gw, gh, dst, face, level, gx, gy);
        int32_t* rect = &rects[cut++ * 4];
        rect[0] = (int32_t)floor(gx * g_ui_w / pw), rect[1] = (int32_t)floor(gy * g_ui_h / ph);
        rect[2] = (int32_t)ceil((gx + gw) * g_ui_w / pw), rect[3] = (int32_t)ceil((gy + gh) * g_ui_h / ph);
    }
    if (!g_ui_layer || !g_ui_drawn || !cut)
        return;
    const uint32_t vp[6] = { 0, 0, g_ui_w, g_ui_h, 0, f2u(1.0f) };
    gfx_set_targets(g_ui_layer, 0, 0, NULL);
    gfx_clear(cut, rects, 1, 0x00000000u, 1.0f, 0, vp);
    apply_targets();
}

/* --- the device lost and made again (d3d8.h) ----------------------------------------------------------
 * The graphics card stopped (a driver timeout) and Windows reset it: the device and everything on it are
 * gone. The game never hears of it (TestCooperativeLevel stays D3D_OK, Reset isn't asked for, as FFXI has
 * no code for either): the back end is torn down and brought up again under the same COM pointers, and
 * the objects the game holds get new GPU copies. What they hold comes back from where it is kept already -
 * every lockable surface's pixels and every static buffer's vertices are in guest memory (Obj.mem) for
 * the object's life, and go up again at the next draw as a lock's write does - and render targets are
 * drawn again every frame. A texture pack's DDS was freed after its upload: read from disk again. */

void* d3d8_device_window(void) { return g_dev.guest ? user32_sdl_window(g_dev.hwnd) : NULL; }
int d3d8_device_vsync(void) { return g_dev_vsync; }

void d3d8_device_lost(void)
{
    uint32_t objects = 0;
    for (uint32_t i = 1; i < g_nobjs; ++i)
    {
        Obj* o = &g_objs[i];
        if (o->kind == O_FREE)
            continue;
        if (o->gpu || o->gbuf)
            ++objects;
        scene_forget(o->gpu);
        water_kept_drop(o->gpu, o->gbuf);
        gfx_tex_destroy(o->gpu), o->gpu = NULL;
        gfx_buf_destroy(o->gbuf), o->gbuf = NULL;
        o->repl = o->nrm = NULL, o->repl_entry = NULL, o->repl_pad = o->repl_pad_min = 0; /* the packs', below */
        o->gpu_locked = 0; /* a GPU-owned surface locked across the loss: nothing left to write back to */
    }
    /* the packs' textures: read from disk again at the next upload of a texture they replace (texture_replacement) */
    for (uint32_t i = 0; i < g_npack; ++i)
        gfx_tex_destroy(g_pack[i].tex), g_pack[i].tex = NULL, g_pack[i].tried = 0;
    for (uint32_t i = 0; i < g_nnrm; ++i)
        gfx_tex_destroy(g_nrm[i].tex), g_nrm[i].tex = NULL, g_nrm[i].tried = 0;
    /* VanaCore's own: the interface layer and the raised copies are made again as they're drawn */
    gfx_tex_destroy(g_ui_layer), g_ui_layer = NULL, g_ui_w = g_ui_h = 0, g_ui_drawn = 0;
    gfx_tex_destroy(g_plain), g_plain = NULL;
    chatfont_lost();
    winimage_lost();
    for (int i = 0; i < D3D8_RAISE_MOST; ++i)
        gfx_tex_destroy(g_raised[i].tex), memset(&g_raised[i], 0, sizeof g_raised[i]);
    g_raise_taken = 0;
    g_grass_depth = NULL;
    grass_device_lost();
    water_device_lost();
    rain_mask_lost();
    floors_device_lost();
    snow_device_lost();
    zonemesh_device_lost();
    rt_log("[recomp] d3d8: the device lost: %u objects' textures and buffers let go, made again on the new device\n", objects);
}

void d3d8_device_restored(void)
{
    uint32_t textures = 0, buffers = 0;
    for (uint32_t i = 1; i < g_nobjs; ++i)
    {
        Obj* o = &g_objs[i];
        switch (o->kind)
        {
        case O_TEXTURE:
        case O_CUBE:
            if (o->pool != 2) /* (system memory textures are only ever copied from) */
                o->gpu = gfx_tex_create(o->kind == O_CUBE ? GFX_TEX_CUBE : GFX_TEX_2D, o->format, o->pw ? o->pw : o->width,
                    o->ph ? o->ph : o->height, o->levels, (o->usage & USAGE_RENDERTARGET) ? GFX_USE_RT : GFX_USE_SAMPLE);
            textures += o->gpu != NULL;
            /* every level the game filled goes up again at the texture's next draw (texture_for_draw); a render
             * target's are the GPU's own, drawn again */
            o->dirty = 0;
            for (uint32_t k = 0; k < o->nsubs; ++k)
            {
                Obj* s = obj(o->subs[k]);
                if (!s)
                    continue;
                s->dirty = s->mem && !gpu_owned(s);
                o->dirty |= s->dirty;
            }
            break;
        case O_SURFACE: /* the back buffer, its depth buffer, the game's standalone targets: drawn again */
            if (!o->container && (o->usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)))
                o->gpu = gfx_tex_create(GFX_TEX_2D, o->format, o->pw ? o->pw : o->width, o->ph ? o->ph : o->height, 1,
                    (o->usage & USAGE_DEPTHSTENCIL) ? GFX_USE_DEPTH : GFX_USE_RT);
            textures += o->gpu != NULL;
            break;
        case O_VB:
        case O_IB: /* a static buffer's GPU copy, uploaded from guest memory at its next draw (static_buffer) */
            static_buffer_init(o);
            buffers += o->gbuf != NULL;
            break;
        default:
            break;
        }
    }
    g_resized = 0; /* resize_targets looks at the targets' sizes again */
    rt_log("[recomp] d3d8: the device made again: %u textures and %u static buffers made again, uploaded at their next draw\n",
        textures, buffers);
}

void d3d8_set_render_size(uint32_t w, uint32_t h)
{
    if (w != g_render_w || h != g_render_h)
        g_resized = 0;
    g_render_w = w, g_render_h = h;
}

void d3d8_set_resolution(int window, float scene)
{
    if ((window != 0) != g_draw_window || scene != g_scene_scale)
        g_resized = 0;
    g_draw_window = window != 0;
    g_scene_scale = scene > 0 ? scene : 0;
}

void d3d8_set_background(uint32_t w, uint32_t h)
{
    if (!w || !h)
        w = h = 0;
    if (w > 8192 || h > 8192)
        w = w > 8192 ? 8192 : w, h = h > 8192 ? 8192 : h;
    if (w != g_scene_w || h != g_scene_h)
        g_resized = 0;
    g_scene_w = w, g_scene_h = h;
}
#endif

static int ui_hit(float fx, float fy)
{
    int x = (int)(fx * UI_GW), y = (int)(fy * UI_GH);
    return x >= 0 && x < UI_GW && y >= 0 && y < UI_GH && g_ui_grid[g_ui_cur ^ 1][y][x];
}

/* a box of the screen (0..1 across and down) the interface covers */
static void ui_mark(float x0, float x1, float y0, float y1)
{
    int a = (int)(x0 * UI_GW), b = (int)(x1 * UI_GW), c = (int)(y0 * UI_GH), e = (int)(y1 * UI_GH);
    a = a < 0 ? 0 : a, c = c < 0 ? 0 : c, b = b >= UI_GW ? UI_GW - 1 : b, e = e >= UI_GH ? UI_GH - 1 : e;
    for (int y = c; y <= e; ++y)
        for (int x = a; x <= b; ++x)
            g_ui_grid[g_ui_cur][y][x] = 1;
}

static void ui_present(void)
{
    g_ui_nbars = 0;
    g_ui_composited = 0;
    g_ui_cur ^= 1;
    memset(g_ui_grid[g_ui_cur], 0, sizeof g_ui_grid[0]);
    user32_ui_hit = ui_hit;
#ifdef VANACORE
    movers_present();
#endif
}

/* The draw's x mapped to a + b x (the game's pixels, across the whole target): its viewport - the
 * clip rectangle the game gave it, in the target's pixels - moves and narrows to match, while the
 * XYZRHW mapping (u.vp, the viewport in the game's pixels) stays, so the draw keeps its clip */
static void ui_target_size(float* w, float* h);

static void ui_map_axis(GfxDraw* d, int i, float a, float b)
{
    float x0 = d->u.vp[i], w = d->u.vp[i + 2];
    if (!(w > 0))
        return;
    float k = (float)d->vp[i + 2] / w; /* the target's pixels per game pixel */
    float l = (a + b * x0) * k, r = (a + b * (x0 + w)) * k;
    /* kept within the target: the back ends clip a viewport to it, which shrank the mapping (a bar's edge line, widened past
     * the screen's edge, stopped short of its corner) (from XI on Anything, rubymatrix/xi-on-anything ecf6cf7) */
    float tw, th;
    ui_target_size(&tw, &th);
    const float edge = (i ? th : tw) * k;
    l = l < 0 ? 0 : l;
    r = r > edge ? edge : r;
    d->vp[i] = (uint32_t)(l + 0.5f);
    d->vp[i + 2] = r > l ? (uint32_t)(r - l + 0.5f) : 0;
    /* what rounding the clip took off, given back to the mapping so the draw lands where asked */
    d->u.vp[i] = x0 + ((float)d->vp[i] / k - (a + b * x0)) / b;
    d->u.vp[i + 2] = (float)d->vp[i + 2] / k / b;
}

static void ui_map(GfxDraw* d, float a, float b) { ui_map_axis(d, 0, a, b); }

/* The game's size of the target the draw goes to: the back buffer's, or the menu target's */
static void ui_target_size(float* w, float* h)
{
    const Obj* rt = obj(g_dev.rt);
    const Obj* t = rt && rt->container ? obj(rt->container) : NULL;
    *w = t ? (float)t->width : (float)g_dev.pp[0];
    *h = t ? (float)t->height : (float)g_dev.pp[1];
}

/* The box (x0, y0, x1, y1) of a screen-space (XYZRHW) draw's vertices, as its maker put them: 0 when none could be read */
static int rhw_bounds(const GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride, float box[4])
{
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        const Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 8)
            break;
        float x = u2f(rd32(base + at)), y = u2f(rd32(base + at + 4));
        x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    }
    if (x0 > x1 || y0 > y1)
        return 0;
    box[0] = x0, box[1] = y0, box[2] = x1, box[3] = y1;
    return 1;
}

#ifdef VANACORE
/* Where Ashita's Minimap shows its map: the box of its map draw (minimap_note's), in the pixels plugins draw and get the mouse
 * in, where it's drawn (moved, mover_draw). The mouse hook (window.cpp, its own thread) asks whether a wheel turn is over it:
 * that turn is the plugin's alone (it zooms the map) and the game, which would zoom its camera with it, never sees it. Only
 * while the draw is still being made. */
static volatile int32_t g_mm_box[4];
static volatile uint32_t g_mm_ms;

static void minimap_box(const GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride, int moved)
{
    float b[4];
    if (!rhw_bounds(d, first, n, up_data, up_stride, b))
        return;
    if (moved)
        for (int i = 0; i < 4; ++i)
            b[i] = ((i & 1) ? g_mover_ay : g_mover_ax) + g_mover_scale * b[i];
    g_mm_box[0] = (int32_t)floorf(b[0]), g_mm_box[1] = (int32_t)floorf(b[1]);
    g_mm_box[2] = (int32_t)ceilf(b[2]), g_mm_box[3] = (int32_t)ceilf(b[3]);
    g_mm_ms = (uint32_t)(gfx_now_ns() / 1000000u) | 1u; /* never 0: 0 is "not drawn yet" */
}

static int mover_draw(GfxDraw* d, const float drawn[4]);

/* A screen-space draw: a plugin's moved (d3d8_set_mover), and Ashita's Minimap's map noted where it's drawn */
static void plugin_draw(GfxDraw* d, uint32_t first, uint32_t nverts, uint32_t up_data, uint32_t up_stride)
{
    float drawn[4];
    const int moved = g_mover >= 0 && g_dev.rt == g_dev.backbuffer && rhw_bounds(d, first, nverts, up_data, up_stride, drawn) &&
                      mover_draw(d, drawn);
    if (g_dev.cur.vs == 0x344)
    {
        minimap_note(d);
        minimap_box(d, first, nverts, up_data, up_stride, moved);
    }
}

/* A plugin's screen-space draw onto the back buffer, its box `drawn` (d3d8_set_mover): noted for it, then moved as its
 * owner's move asks. 1 when it was moved. */
static int mover_draw(GfxDraw* d, const float drawn[4])
{
    float* box = g_movers[g_mover].now;
    if (!g_movers[g_mover].drawn_now)
    {
        memcpy(box, drawn, sizeof g_movers[g_mover].now);
        g_movers[g_mover].drawn_now = 1;
    }
    else
    {
        box[0] = drawn[0] < box[0] ? drawn[0] : box[0], box[1] = drawn[1] < box[1] ? drawn[1] : box[1];
        box[2] = drawn[2] > box[2] ? drawn[2] : box[2], box[3] = drawn[3] > box[3] ? drawn[3] : box[3];
    }
    if (g_mover_scale == 1.0f && g_mover_ax == 0.0f && g_mover_ay == 0.0f)
        return 0;
    ui_map_axis(d, 0, g_mover_ax, g_mover_scale);
    ui_map_axis(d, 1, g_mover_ay, g_mover_scale);
    return 1;
}

/* Any thread. 1 when (x, y), in the pixels plugins draw in, is over a minimap drawn within the last half second. */
int d3d8_plugin_wheel_over(int x, int y)
{
    const uint32_t seen = g_mm_ms;
    if (!seen || (uint32_t)(gfx_now_ns() / 1000000u) - seen > 500u)
        return 0;
    return x >= g_mm_box[0] && x <= g_mm_box[2] && y >= g_mm_box[1] && y <= g_mm_box[3];
}
#endif

static void ui_squeeze(GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride, int mark)
{
    float s = user32_ui_squeeze(g_dev.hwnd), sy = user32_ui_squeeze_y(g_dev.hwnd);
    if (s >= 1.0f && sy >= 1.0f)
        return;
    float box[4];
    if (!rhw_bounds(d, first, n, up_data, up_stride, box))
        return;
    float lo = box[0], ylo = box[1], hi = box[2], yhi = box[3];
    /* everything is squeezed toward the target's middle, whatever viewport the draw has (the chat
     * log's text and the menus' titles are clipped to their windows); "the whole width" with some
     * slack: the rules screen's backdrop stops 1.5 pixels short */
    float W, h;
    ui_target_size(&W, &h);
    float slack = 1.0f + W / 256.0f;
    if (sy < 1.0f)
    {
        /* a window taller than the interface's shape: squeezed toward the middle up and down. What
         * covers the whole target stays (the 3D scene put on the screen, fades, the dimming behind a
         * menu); a full-width band at the top or bottom (a cutscene's bars, the lobby's help bar)
         * keeps its outer edge on the screen's */
        float yslack = 1.0f + h / 256.0f, a = h * 0.5f * (1.0f - sy), b = sy;
        int wide = lo <= slack && hi >= W - slack, top = ylo <= yslack, bottom = yhi >= h - yslack;
        if (wide && top && bottom)
            return;
        if (wide && (top || bottom) && yhi > ylo)
        {
            float t0 = top ? 0.0f : a + sy * ylo, t1 = bottom ? h : a + sy * yhi;
            b = (t1 - t0) / (yhi - ylo), a = t0 - b * ylo;
        }
        ui_map_axis(d, 1, a, b);
        if (mark)
        {
            ui_mark(lo / W, hi / W, (a + b * ylo) / h, (a + b * yhi) / h);
#ifdef VANACORE
            g_ui_squeezed = 1;
#endif
        }
        return;
    }
    if (lo <= slack && hi >= W - slack)
    {
        if (yhi - ylo <= h * UI_BAR_MAX_H && g_ui_nbars < UI_MAX_BARS)
            g_ui_bars[g_ui_nbars].rt = g_dev.rt, g_ui_bars[g_ui_nbars].y0 = ylo, g_ui_bars[g_ui_nbars].y1 = yhi,
            g_ui_nbars++;
        return;
    }
    /* a piece of a bar's frame: within a bar's band and at an edge (a corner's width, 64 pixels) */
    float edge = 64.0f * W / 1280.0f, a = W * 0.5f * (1.0f - s), b = s;
    for (int i = 0; i < g_ui_nbars; ++i)
    {
        if (g_ui_bars[i].rt != g_dev.rt || ylo < g_ui_bars[i].y0 - 2.0f || yhi > g_ui_bars[i].y1 + 2.0f)
            continue;
        int left = lo <= edge, right = hi >= W - edge;
        if (left && right) /* an edge line: its ends keep their insets, squeezed */
        {
            float l = lo * s, r = W - (W - hi) * s;
            b = (r - l) / (hi - lo), a = l - b * lo;
            break;
        }
        if (left && hi < W * 0.5f) /* the left corner, squeezed toward the left edge */
        {
            a = 0.0f;
            break;
        }
        if (right && lo > W * 0.5f) /* the right corner */
        {
            a = W * (1.0f - s);
            break;
        }
    }
    ui_map(d, a, b);
    if (mark)
    {
        ui_mark((a + b * lo) / W, (a + b * hi) / W, ylo / h, yhi / h);
#ifdef VANACORE
        g_ui_squeezed = 1;
#endif
    }
}

/* FFXI_DRAWLOG=<path>: while <path>.go exists, the next frame's draws are written to <path> (then
 * .go is removed): the game's return addresses on the guest stack, the texture, and the vertices'
 * box. For finding which of the game's code draws what. */
static void cap_present(void)
{
    static int init;
    if (!init)
    {
        init = 1;
        const char* env = getenv("FFXI_DRAWLOG");
        if (env && *env && !g_cap_path[0])
            snprintf(g_cap_path, sizeof g_cap_path, "%s", env);
        /* without it, ~/Library/Caches/FFXI/draws.txt (still only while draws.txt.go exists) */
#ifndef VANACORE
        if (!g_cap_path[0] && getenv("HOME"))
            snprintf(g_cap_path, sizeof g_cap_path, "%s/Library/Caches/FFXI/draws.txt", getenv("HOME"));
#endif
    }
    g_cap_path[sizeof g_cap_path - 1] = 0; /* a string whatever was written into it */
    if (!g_cap_path[0])
        return;
    ++g_cap_frame;
    if (g_cap)
    {
        fclose(g_cap), g_cap = NULL;
        char go[1024];
        snprintf(go, sizeof go, "%s.go", g_cap_path);
        remove(go);
        return;
    }
    if (g_cap_frame % 30)
        return;
    char go[1024];
    snprintf(go, sizeof go, "%s.go", g_cap_path);
    FILE* f = fopen(go, "rb");
    if (!f)
        return;
    fclose(f);
    g_cap = fopen(g_cap_path, "w");
    g_cap_n = 0;
    uint32_t w, h;
    d3d8_screen_size(&w, &h);
    if (g_cap)
        fprintf(g_cap, "frame %u screen %ux%u backbuffer %ux%u image %08x-%08x\n", g_cap_frame, w, h, g_dev.pp[0],
            g_dev.pp[1], rt_image_lo, rt_image_hi);
}

/* [graphics] drawlog, live: the path the next frame's draws go to while <path>.go exists; NULL or "" stops it. The path
 * is copied: g_cap_path is never a pointer handed in from outside. */
void d3d8_set_drawlog(const char* path)
{
    if (!path)
        path = "";
    if (strcmp(path, g_cap_path) == 0)
        return;
    if (g_cap)
    {
        /* a capture under way is cut short, its .go removed as cap_present would have */
        fclose(g_cap), g_cap = NULL;
        char go[1024];
        snprintf(go, sizeof go, "%s.go", g_cap_path);
        remove(go);
    }
    snprintf(g_cap_path, sizeof g_cap_path, "%s", path);
}

#ifdef VANACORE
/* [graphics.effects] elight_lantern_tex, live: the hashes (texture_hash, 16 hex digits, drawlog's "hash") of the
 * textures a lantern's or torch's flame is drawn with, separated by commas or spaces. A blended 3D draw sampling one
 * is flagged (GfxDraw.el_lantern 1), and the light from the effects takes it at elight_lantern's share. "" none.
 * elight_dim_tex, the same for a zone's effects that give far more light than they look to (GfxDraw.el_lantern 2:
 * Al'Taieu's portal pools' ring), at elight_dim's share. dlss5_skip_tex, the same for the effects DLSS 5 isn't to
 * take as lights (GfxDraw.dlss5_skip: Al'Taieu's portal pools, which it turned into a field of coloured blocks). */
static uint64_t g_lantern_tex[32], g_dim_tex[32], g_skip_tex[32];
static int g_lantern_n, g_dim_n, g_skip_n;

static int hash_list(const char* list, uint64_t* v)
{
    int n = 0;
    for (const char* p = list ? list : ""; *p && n < 32;)
    {
        while (*p && !((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            ++p;
        uint64_t h = 0;
        int digits = 0;
        for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'); ++p, ++digits)
            h = (h << 4) | (uint64_t)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        if (digits)
            v[n++] = h;
    }
    return n;
}

void d3d8_set_lantern_textures(const char* list)
{
    uint64_t v[32];
    const int n = hash_list(list, v);
    memcpy(g_lantern_tex, v, sizeof(uint64_t) * (size_t)n);
    g_lantern_n = n;
}

void d3d8_set_dim_textures(const char* list)
{
    uint64_t v[32];
    const int n = hash_list(list, v);
    memcpy(g_dim_tex, v, sizeof(uint64_t) * (size_t)n);
    g_dim_n = n;
}

void d3d8_set_dlss5_skip_textures(const char* list)
{
    uint64_t v[32];
    const int n = hash_list(list, v);
    memcpy(g_skip_tex, v, sizeof(uint64_t) * (size_t)n);
    g_skip_n = n;
}

/* a texture DLSS 5 isn't to take as a light (dlss5_skip_tex) */
static int dlss5_skip_texture(Obj* t)
{
    if (!g_skip_n || !t)
        return 0;
    const uint64_t h = texture_hash(t);
    for (int i = 0; h && i < g_skip_n; ++i)
        if (g_skip_tex[i] == h)
            return 1;
    return 0;
}

/* 1 a lantern's texture, 2 a dimmed effect's, 0 neither */
static int lantern_texture(Obj* t)
{
    if ((!g_lantern_n && !g_dim_n) || !t)
        return 0;
    const uint64_t h = texture_hash(t);
    for (int i = 0; h && i < g_lantern_n; ++i)
        if (g_lantern_tex[i] == h)
            return 1;
    for (int i = 0; h && i < g_dim_n; ++i)
        if (g_dim_tex[i] == h)
            return 2;
    return 0;
}

/* The characters and fixtures near the player (d3d8_set_characters): where each stands (x, height, y), then a character's
 * hitbox size or -1 for a fixture (a home point, a waypoint, a streetlamp) */
static float g_chars[256][4];
static unsigned g_nchars;

void d3d8_set_characters(const float* xyzr, unsigned count)
{
    g_nchars = count < 256 ? count : 256;
    if (g_nchars)
        memcpy(g_chars, xyzr, sizeof(float) * 4 * g_nchars);
    snow_set_characters(xyzr, g_nchars); /* (the snow's footprints follow them) */
}
#endif

#include "d3d8_cap.h" /* cap_draw: the draw log's line for a draw */

/* --- the scene's end, for the back end's scene effects (gfx_scene_done) ------------------------------
 * The frame's 3D scene is the target that takes depth-tested draws of untransformed vertices and is
 * at least half the back buffer (not the 16x16 probes or smaller effect targets). It is finished
 * when a draw samples it (FFXI draws its background target onto the back buffer as a quad), when
 * transformed vertices go over it on the back buffer (the interface), or at Present. The camera
 * comes from its fixed-function draws: the zone's own shader draws carry theirs in constants. */
static struct
{
    GfxTex* rt;     /* the scene's target */
    /* the world's view: the target that took the most 3D draws the frame before. FFXI draws the world
     * into one 4096 target and copies it into another, where nameplates and effects go on; other
     * targets' scenes get no effects, cast no shadows and give no camera. */
    GfxTex* world;
    int world_done; /* the effects ran on it this frame: once a frame */
    /* this frame's 3D draws per target, and their vertices; laid: its picture laid over another target 3D went into this frame
     * (scene_laid), the game's own offscreen 3D, never the world */
    struct { GfxTex* t; uint32_t n, v; uint8_t laid; } tally[4];
    uint32_t draws; /* 3D draws to it since the effects last ran */
    uint32_t chars; /* ... of them the characters', this frame: the game draws them after its zone (scene_far_along) */
    int done, cam;  /* the effects ran this frame; s holds a camera */
    /* where this frame's camera and sun came from: the draw's number, and for the camera 2 when that
     * draw was fogged (the world), 1 when not; 0 before one is found */
    uint32_t cam_draw, sun_draw, cam_rank;
    GfxScene s;
    /* the profile (FFXI_PROFILE): scenes by how they ended and where the camera came from, 3D draws
     * after the effects ran, and screen-space or undepth-tested draws onto the scene before them
     * (what the effects would shade) */
    uint32_t st_frames, st_why[4], st_cam[3], st_late, st_over, st_over_rhw;
    uint32_t st_late_cast; /* ... of the 3D after the effects, the world's own pieces taken as casters for the next frame (scene_note) */
    uint32_t st_late_out[6]; /* ... and the rest by what kept them out: another target, screen-space, another camera, blended, no
                              * depth write (and not the zone's mesh), other (a water layer) */
    /* the trace (scene_trace): what each of the last frames did with the world's scene */
    uint32_t frame, tr_late, tr_draws, tr_targets;
    char tr_why;
    struct SceneTrace { uint32_t frame, draws, late, targets; char why, ran, changed, cam; uint64_t serial; } trace[1200];
    /* the flicker watch's: the screen-space draw that ended this frame's scene as the interface, and the
     * game's own offscreen 3D laid over the world (scene_composite) this frame - the first one's */
    char end_desc[256], comp_desc[256];
    uint32_t composites;
} g_scene;

static uint32_t g_scene_shown[2]; /* d3d8_scene_shown */

int d3d8_scene_shown(uint32_t* w, uint32_t* h)
{
    *w = g_scene_shown[0], *h = g_scene_shown[1];
    return *w && *h;
}

int d3d8_camera(float* view, float* proj, uint32_t* viewport)
{
    const int have = g_scene.cam && g_scene.s.view[15] != 0.0f && g_scene.s.proj[5] != 0.0f;
    if (have)
        memcpy(view, g_scene.s.view, 64), memcpy(proj, g_scene.s.proj, 64), memcpy(viewport, g_scene.s.vp, 24);
    return have;
}

static uint32_t g_label; /* d3d8_set_label: the character whose name is being drawn, 0 none */

void d3d8_set_label(uint32_t id) { g_label = id; }

/* a texture going away: the scene no longer points at it */
static void scene_forget(GfxTex* t)
{
    if (!t)
        return;
    if (g_scene.rt == t)
        g_scene.rt = NULL, g_scene.draws = 0, g_scene.done = 0;
    if (g_scene.world == t)
        g_scene.world = NULL;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t == t)
            g_scene.tally[i].t = NULL, g_scene.tally[i].n = g_scene.tally[i].v = 0, g_scene.tally[i].laid = 0;
}

/* a target made again (regrow): the scene's pointers move to the new texture */
static void scene_moved(GfxTex* old, GfxTex* now)
{
    if (g_scene.rt == old)
        g_scene.rt = now;
    if (g_scene.world == old)
        g_scene.world = now;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t == old)
            g_scene.tally[i].t = now;
}

/* what the flicker watch's toggle finder (gfx_watch_value, scene_values) is fed from the front end: the other lights the world's
 * lit draws used, the directions they voted for, and what went into the glow's full-size picture and when the glow was laid */
static struct
{
    uint32_t points, spots;            /* enabled point and spot lights the world's lit draws had, this frame */
    float point_luma, spot_luma;       /* the brightest of each */
    uint32_t slots;                    /* the directions the lit draws voted for (d3d8_sun_pick) */
    uint32_t glow_3d, glow_ui;         /* 3D and screen-space draws into the glow's picture, this frame */
    uint32_t glow_lays, glow_probes_at_lay, glow_draws_at_lay; /* the glow laid over the world: when */
    float glow_alpha;
} g_wf;

/* a lit draw's directional lights, a vote each (its first light's counts for more: FFXI's key light; d3d8_sun.c counts them) */
static void sun_vote(const State* s)
{
    int first = 1;
    for (int i = 0; i < g_light_top; ++i)
    {
        const Light* l = &s->light[i];
        if (l->enabled && l->v[0] != 3)
        {
            const float luma = 0.2126f * l->v[1] + 0.7152f * l->v[2] + 0.0722f * l->v[3];
            if (l->v[0] == 1.0f)
                g_wf.points++, g_wf.point_luma = fmaxf(g_wf.point_luma, luma);
            else if (l->v[0] == 2.0f)
                g_wf.spots++, g_wf.spot_luma = fmaxf(g_wf.spot_luma, luma);
        }
        if (!l->enabled || l->v[0] != 3)
            continue;
        float v[26];
        memcpy(v, l->v, sizeof v);
        float len = sqrtf(v[16] * v[16] + v[17] * v[17] + v[18] * v[18]);
        if (!(len > 0.0f))
            continue;
        const float w[3] = { -v[16] / len, -v[17] / len, -v[18] / len };
        d3d8_sun_vote(w, &v[1], first ? 1.0f : 0.5f, g_scene.draws);
        first = 0;
    }
}

static void scene_finish(const char* why)
{
    if (g_cap && g_scene.draws && !g_scene.done)
    {
        const float* p = g_scene.s.proj;
        const uint32_t* vp = g_scene.s.vp;
        const float* f = g_scene.s.fogcolor;
        fprintf(g_cap, "scene done (%s) after %u 3D draws, camera %d from draw %u (fogged %d), sun %d from draw %u, "
            "projection %g %g %g %g / %g %g, viewport %u %u %u %u z %g..%g, fog %.2f %.2f %.2f\n",
            why, g_scene.draws, g_scene.cam, g_scene.cam_draw, g_scene.cam_rank == 2, g_scene.s.sun_dir[3] != 0.0f,
            g_scene.sun_draw, p[0], p[5], p[10], p[11], p[14], p[15], vp[0], vp[1], vp[2], vp[3], u2f(vp[4]), u2f(vp[5]),
            f[0], f[1], f[2]);
    }
    if (g_scene.draws && !g_scene.done)
    {
        g_scene.st_why[!strcmp(why, "sampled") || why[0] == 'e' ? 0 : why[0] == 'i' ? 1 : why[0] == 'p' ? 2 : 3]++;
        g_scene.st_cam[g_scene.cam ? g_scene.cam_rank : 0]++;
    }
    if (g_scene.draws && !g_scene.done && g_scene.cam && (!g_scene.world || g_scene.rt == g_scene.world) &&
        !g_scene.world_done)
    {
        g_wf.slots = d3d8_sun_pick(&g_scene.s, g_scene.frame);
        memcpy(g_scene.s.focus, g_focus, sizeof g_focus);
        memcpy(g_scene.s.zonelines, g_zonelines, sizeof(float) * 8 * g_nzonelines);
        g_scene.s.nzonelines = g_nzonelines;
        memcpy(g_scene.s.zoneline_color, g_zoneline_color, sizeof g_zoneline_color);
        g_scene.s.zoneline_style = g_zoneline_style, g_scene.s.zoneline_glow = g_zoneline_glow;
        g_scene.s.zoneline_height = g_zoneline_height, g_scene.s.zoneline_size = g_zoneline_size;
        g_scene.s.zoneline_fade = g_zoneline_fade;
        memcpy(g_scene.s.sparkles, g_sparkles, sizeof g_sparkles[0] * g_nsparkles);
        g_scene.s.nsparkles = g_nsparkles;
        g_scene.s.sparkle_size = g_sparkle_size;
        g_scene.s.sparkle_density = g_sparkle_density, g_scene.s.sparkle_speed = g_sparkle_speed;
        g_scene.s.sparkle_match = g_sparkle_match;
        memcpy(g_scene.s.ground_marks, g_ground_marks, sizeof g_ground_marks[0] * g_nground_marks);
        g_scene.s.nground_marks = g_nground_marks;
        drawn_size(&g_scene.s.present_w, &g_scene.s.present_h);
        g_scene_shown[0] = g_scene.s.present_w, g_scene_shown[1] = g_scene.s.present_h;
        /* the zone's floors as a layer for the reflections (floors.c), into targets of its own, the scene's set back after */
        floors_scene(g_scene.rt, g_grass_depth, &g_scene.s);
        zonemesh_scene(&g_scene.s); /* the zone's own meshes to the sun's maps (zonemesh.c) */
        snow_scene(g_scene.rt, g_grass_depth, &g_scene.s); /* footprints on the snow (snow.c) */
        grass_scene(g_scene.rt, g_grass_depth, &g_scene.s);
        water_pass(g_scene.rt, g_grass_depth, &g_scene.s); /* VanaCore's water over the world, the grass under it (d3d8_water.h) */
        rain_pass(g_scene.rt, g_grass_depth, &g_scene.s);  /* VanaCore's rain over it all (d3d8_rain.h) */
        gfx_scene_done(g_scene.rt, &g_scene.s);
        g_scene.world_done = 1;
        g_scene.tr_why = why[0] == 's' && why[1] == 'w' ? 'w' : why[0], g_scene.tr_draws = g_scene.draws;
    }
    g_scene.draws = 0;
    g_scene.done = 1;
}

/* A screen-space draw onto the world's target that samples another large target 3D went into this frame:
 * the game's own offscreen 3D laid back over the world partway through it, the world going on after. The
 * flicker watch found such frames ending the world's scene there as the interface, a third of the way into
 * the world, and the rest of it drawn after the effects - unshaded and ungraded for that frame. */
static int scene_composite(const GfxDraw* d)
{
    /* The trust and avatar glow's copies from its small picture (glow_draw), the 3D drawn apart laid back too. Taken for
     * the interface, the first of them ran the effects partway through the world on the frames something glowing was
     * drawn, and the characters after it went unshaded and ungraded in those frames only: they flickered (by the
     * Veridical Conflux). And held as interface, they went over the character's own picture laid after them. */
    if (g_glow_dst && g_dev.cur.tex[0] == g_glow_dst)
        return 1;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            if (d->tex[i] && d->tex[i] == g_scene.tally[j].t && g_scene.tally[j].n && g_scene.tally[j].t != g_scene.rt)
                return 1;
    return 0;
}

/* A screen-space draw into c, an offscreen target 3D already went into this frame, that samples another target 3D went into:
 * that one's picture is laid over c, the game's own offscreen 3D (a see-through character, the Cavernous Maw's Atomos), and it
 * is never the next frame's world (scene_present), however many vertices went into it. Atomos is drawn into a target of its
 * own after the world and laid over it, with more vertices than Batallia Downs [S]'s few large meshes: by the vertices alone it
 * was the world, the grass saw no ground in front of it and the effects ran on its picture (Araedi, 2026-11-21) */
static void scene_laid(const GfxDraw* d, const GfxTex* c)
{
    if (!d->vs.rhw || !c || g_dev.rt == g_dev.backbuffer)
        return;
    int over = 0;
    for (int j = 0; j < 4; ++j)
        over |= g_scene.tally[j].t == c && g_scene.tally[j].n;
    if (!over)
        return;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            if (d->tex[i] && d->tex[i] == g_scene.tally[j].t && g_scene.tally[j].n && g_scene.tally[j].t != c)
                g_scene.tally[j].laid = 1;
}

/* a draw, for the flicker watch's lines */
static void describe_draw(const GfxDraw* d, char* out, size_t n)
{
    int at = snprintf(out, n, "%s draw of %u primitives, blend %s (%u, %u), textures", d->vs.rhw ? "a screen-space" : "a 3D",
        (unsigned)d->count, d->pipe.blend ? "on" : "off", (unsigned)d->pipe.src, (unsigned)d->pipe.dst);
    int any = 0;
    for (int i = 0; i < 8 && at > 0 && (size_t)at < n; ++i)
    {
        if (!d->tex[i])
            continue;
        const Obj* o = obj(g_dev.cur.tex[i]);
        int took = 0;
        for (int j = 0; j < 4; ++j)
            took |= d->tex[i] == g_scene.tally[j].t && g_scene.tally[j].n;
        at += snprintf(out + at, n - (size_t)at, " [%d] %ux%u%s%s", i, o ? o->width : 0, o ? o->height : 0,
            o && (o->usage & 1) ? " render target" : "", d->tex[i] == g_scene.rt ? " (the scene's)" : took ? " (3D went into it this frame)" : "");
        any = 1;
    }
    if (!any && at > 0 && (size_t)at < n)
        snprintf(out + at, n - (size_t)at, " none");
}

/* The effects run after the sky's layers (scene_note): FFXI_SKY_IN_SCENE=0 ([graphics] effects.aftersky=0) ends them at the
 * sky as before */
static int effects_after_sky(void)
{
    static int on = -1;
    if (on < 0)
        on = !(getenv("FFXI_SKY_IN_SCENE") && getenv("FFXI_SKY_IN_SCENE")[0] == '0');
    return on;
}

/* ... which can't go by the view alone: FFXI draws its effects round the camera too (a view with no translation, each placed
 * from the camera by its world matrix: a home point crystal's glow, a flame, a spell), fogged or not. Taken for the sky,
 * none of them ended the scene, it ended at the interface after all of them, and the light from the effects (el_note) had
 * nothing to take its light from. The sky's layers are models the game made once, centred on the camera (the dome, its
 * clouds) or far past the world (the sun, the moon); an effect is near, and built for the frame (a dynamic buffer, or the
 * draw's own memory). An effect's own model (made once, as the sky's) is taken for the world; the next particle ends it */
static int camera_effect(const GfxDraw* d)
{
    const float* W = g_dev.cur.xf[24];
    const float r2 = W[12] * W[12] + W[13] * W[13] + W[14] * W[14];
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    const int built = !e->used || e->stream >= GFX_NSTREAMS || !d->buf[e->stream];
    return built && r2 > 0.0f && r2 < 500.0f * 500.0f;
}

#ifdef VANACORE
/* A draw round the camera (a view with no translation) placed near it: an effect, built or its own model (camera_effect) */
static int camera_near(void)
{
    const float* V = g_dev.cur.xf[2];
    const float* W = g_dev.cur.xf[24];
    const float r2 = W[12] * W[12] + W[13] * W[13] + W[14] * W[14];
    return V[12] == 0.0f && V[13] == 0.0f && V[14] == 0.0f && r2 > 0.0f && r2 < 500.0f * 500.0f;
}
#endif

#ifdef VANACORE
/* An effect drawn on a character - a spell, an ability, a weapon skill - and not the zone's (a lamp, a crystal, a fire):
 * where the effect is (the middle of its vertices through its world and view matrices, then back out of the scene's view:
 * an effect drawn round the camera, as FFXI draws its effects, has an identity view and its world matrix places it in view
 * space) within 3 yalms of where one stands, and 4 above or below. Protect's shell wrapped
 * the caster in light and the glow effects lighting lit them up till they washed out; [graphics.effects] elight_actor
 * lights from these at a strength of its own. Not when a fixture stands nearer it, within 3 yalms and 6 above or below: a
 * home point's crystal glow is the zone's, with the player standing beside it. */
static int character_effect(const GfxDraw* d, uint32_t nverts)
{
    if (!g_nchars)
        return 0;
    const float* W = g_dev.cur.xf[24];
    const float* V = g_dev.cur.xf[2];
    /* the middle of the vertices (up to 32 of them, spread over the draw), in the effect's own space */
    float m[3] = { 0.0f, 0.0f, 0.0f };
    const GfxElem* e = &d->vs.el[GFX_R_POSITION];
    const uint32_t st = e->stream;
    const uint32_t stride = e->used && st < GFX_NSTREAMS ? (uint32_t)d->u.stride[st] : 0;
    if (nverts && stride >= 12 && d->cpu[st] && d->cpu_size[st] >= (nverts - 1) * stride + 12u)
    {
        const uint8_t* p0 = (const uint8_t*)d->cpu[st] + d->u.offset[GFX_R_POSITION];
        const uint32_t step = nverts > 32 ? nverts / 32 : 1;
        uint32_t n = 0;
        for (uint32_t i = 0; i < nverts; i += step, ++n)
        {
            float v[3];
            memcpy(v, p0 + (size_t)i * stride, 12);
            m[0] += v[0], m[1] += v[1], m[2] += v[2];
        }
        m[0] /= (float)n, m[1] /= (float)n, m[2] /= (float)n;
    }
    else if (W[12] == 0.0f && W[13] == 0.0f && W[14] == 0.0f)
        return 0;
    /* through the world and the view as the draw has them: into view space (an effect drawn round the camera has an
     * identity view, so its world matrix puts it straight into view space: -z ahead of the camera, -y below it); then back
     * out through the scene's own view (its inverse: less its translation, times its rotation's transpose) */
    float q[3], pv[3], p[3];
    const float* S = g_scene.s.view;
    if (S[15] == 0.0f)
        return 0;
    for (int j = 0; j < 3; ++j)
        q[j] = m[0] * W[j] + m[1] * W[4 + j] + m[2] * W[8 + j] + W[12 + j];
    for (int j = 0; j < 3; ++j)
        pv[j] = q[0] * V[j] + q[1] * V[4 + j] + q[2] * V[8 + j] + V[12 + j];
    for (int j = 0; j < 3; ++j)
        p[j] = (pv[0] - S[12]) * S[j * 4] + (pv[1] - S[13]) * S[j * 4 + 1] + (pv[2] - S[14]) * S[j * 4 + 2];
    float character = 1e30f, fixture = 1e30f; /* the nearest of each it's on (squared, across the ground) */
    for (unsigned i = 0; i < g_nchars; ++i)
    {
        const float* c = g_chars[i];
        const float dx = p[0] - c[0], dz = p[2] - c[2], d2 = dx * dx + dz * dz, dh = fabsf(p[1] - c[1]);
        if (c[3] < 0.0f)
        {
            if (d2 <= 9.0f && dh <= 6.0f && d2 < fixture)
                fixture = d2;
        }
        else
        {
            const float r = 3.0f + c[3];
            if (d2 <= r * r && dh <= 4.0f && d2 < character)
                character = d2;
        }
    }
    return character < fixture;
}
#endif

/* 3D drawn into the scene after its world that is fixed to the screen, not placed in the world (GfxDraw.screen_fx): the
 * Records of Eminence's "New Challenge" banner is drawn a quarter of a yalm in front of the camera, with a projection of its
 * own. The light from the effects took it for a flame and lit the walls far behind it as brightly as a fire beside them.
 * Within a yalm of the camera (its near plane is a tenth of one), or another projection than the scene's */
static int screen_effect(void)
{
    const float* V = g_dev.cur.xf[2];
    const float* W = g_dev.cur.xf[24];
    const float* P = g_dev.cur.xf[3];
    float t[3]; /* the world matrix's origin from the camera: world times view's translation */
    for (int j = 0; j < 3; ++j)
        t[j] = W[12] * V[j] + W[13] * V[4 + j] + W[14] * V[8 + j] + V[12 + j];
    if (t[0] * t[0] + t[1] * t[1] + t[2] * t[2] < 1.0f)
        return 1;
    const float* S = g_scene.s.proj;
    return S[5] != 0.0f && (fabsf(P[0] - S[0]) > 1e-3f * fabsf(S[0]) || fabsf(P[5] - S[5]) > 1e-3f * fabsf(S[5]));
}

#include "d3d8_casters.h" /* late_world, late_mesh, late_count, late_watch, seek_note: the late draws' and the caster pass's casters */

/* Whether the world's scene is far enough along for a draw to end it for the effects: an effect's own model (scene_note's
 * own_model), or the interface's first screen-space draw and not one laid over the world partway through it. It went by the
 * draws alone, 60% of what the frame before took, and the zone's count isn't steady: as the camera moved it doubled or more for
 * a frame or two, about once a second (The Sanctuary of Zi'Tah with [graphics] occl and batch on, 2026-11-12: 1,410, 3,035,
 * 2,942, then 1,320). The frame after such a burst had under 60% of it, so nothing ended its scene before the game's own copy
 * of it, and the effects went over that one frame's glows and names over heads: a crystal's light shells flashed with the
 * bloom over them, and the names with them (14 frames of 1,200, each the first after a burst). So it is far enough along once
 * a character is drawn into it (the game draws them after its zone, before its effects), and else at 60% of the fewest draws
 * of the last 8 frames, which a burst isn't. `least` is that count (0: none of them had a world). */
static int scene_far_along(uint32_t* least)
{
    uint32_t fewest = 0;
    for (uint32_t k = 1; k <= 8 && k <= g_scene.frame; ++k)
    {
        const uint32_t n = g_scene.trace[(g_scene.frame - k) % 1200].draws;
        if (n && (!fewest || n < fewest))
            fewest = n;
    }
    *least = fewest;
    return g_scene.chars || (fewest >= 50 && g_scene.draws * 5 >= fewest * 3);
}

#ifdef VANACORE
/* After scene_note: a character's draw it took as one of the world's scene's, before its effects ran (the scene's draws were
 * `was` before it), counted for scene_far_along */
static void scene_char(const GfxDraw* d, uint32_t was)
{
    if (d->is_char && g_scene.draws == was + 1)
        g_scene.chars++;
}
#endif

static void scene_note(GfxDraw* d, uint32_t nverts)
{
    d->caster = 0;
    uint32_t face, level;
    const Obj* rt = obj(g_dev.rt);
    GfxTex* c = rt ? surface_gpu(rt, &face, &level) : NULL;
    if (!c)
        return;
    /* DLSS: every 3D draw into the world's target moved by the frame's sub-pixel step (in the uniforms' spare
     * vofs.yz) - the world, its decals, the sky and what's drawn over it after its effects alike: a surface moved
     * over one that wasn't fought it for the depth, flickering, the more the smaller DLSS draws the scene */
    float jit[2];
    if (!d->vs.rhw && (c == g_scene.world || (!g_scene.world && c == g_scene.rt)) && gfx_dlss_jitter(jit))
        memcpy(&d->u.pad0[0], &jit[0], 4), memcpy(&d->u.pad0[1], &jit[1], 4);
    int large = (uint64_t)rt->width * rt->height * 2 >= (uint64_t)g_dev.pp[0] * g_dev.pp[1];
    scene_laid(d, c);
    if (g_scene.draws && !g_scene.done)
    {
        int sampled = 0;
        for (int i = 0; i < 8; ++i)
            sampled |= d->tex[i] == g_scene.rt;
        /* the scene drawn somewhere as large as the screen (copied into the target the interface goes
         * on, or onto the back buffer): not the sun flare's occlusion probe, which copies it into
         * 16x16 - several times a frame, while the world is still being drawn */
        if (sampled && large)
            scene_finish("sampled");
        /* the interface over the scene - on the back buffer, or into the scene's own target: FFXI draws
         * nameplates and floating text there, blended, before it draws the scene onto the back buffer;
         * the effects go under them, not over. (Not the probe's own screen-space quads there, which
         * are opaque - and textured, some frames.) */
        else if (d->vs.rhw && c == g_scene.rt && (g_dev.rt == g_dev.backbuffer || d->pipe.blend))
        {
            /* (but not the game's own offscreen 3D laid over the world partway through it, scene_composite, nor
             * any screen-space draw before most of the world is drawn - no character yet and under 60% of the draws
             * the last frames took, scene_far_along: the interface comes after all of it) */
            uint32_t before;
            const int far_along = scene_far_along(&before);
            int composite = scene_composite(d), mid = before >= 50 && !far_along;
            if (composite || mid)
            {
                if (!g_scene.composites++)
                {
                    int at = snprintf(g_scene.comp_desc, sizeof g_scene.comp_desc, "%s, after %u of the %u world draws the last "
                        "frames had at the least: ", composite ? "the game's own offscreen 3D laid over it" : "a screen-space draw", g_scene.draws, before);
                    if (at > 0 && (size_t)at < sizeof g_scene.comp_desc)
                        describe_draw(d, g_scene.comp_desc + at, sizeof g_scene.comp_desc - (size_t)at);
                }
            }
            else
            {
                describe_draw(d, g_scene.end_desc, sizeof g_scene.end_desc);
                scene_finish("interface");
            }
        }
        /* the effects over the world - flames, glows, spells: blended, writing no depth, and unfogged
         * (the zone's own blended decals among its draws are fogged). The effects go under them: fog
         * read from the depth behind a flame took it for the wall there, and fogged it more or less as
         * it flickered. Not the sky's layers drawn after the world - its dome, clouds, the sun, the moon - which are
         * blended and unfogged too, but drawn round the camera (a view with no translation): ended at them, the effects
         * ran before the sun was drawn and the god rays had none to start from (from XI on Anything,
         * rubymatrix/xi-on-anything da004ab). The game's own effects are drawn round the camera too: one of them ends the
         * scene, fogged or not (camera_effect). [graphics] effects.aftersky=0 ends them at the sky again. */
        else if (!d->vs.rhw && c == g_scene.rt && d->depth.zenable && !d->depth.zwrite && d->pipe.blend)
        {
            const int round_camera = g_dev.cur.xf[2][12] == 0.0f && g_dev.cur.xf[2][13] == 0.0f && g_dev.cur.xf[2][14] == 0.0f;
            /* ... or an effect's own model near the camera, added (Hu'Xzoi's door glow), once most of the world is drawn
             * (a character, or 60% of the draws the last frames took: scene_far_along): the game's effect order put it
             * before the first particle some frames and after it others, and the height fog went over it only when it
             * came before - it flickered */
            int own_model = 0;
#ifdef VANACORE
            uint32_t before;
            own_model = round_camera && d->pipe.dst == 2 && d->pipe.op == 1 && camera_near() && scene_far_along(&before);
#endif
            if (effects_after_sky() && round_camera ? camera_effect(d) || own_model : !d->fs.fog && !d->vs.fog_vertex)
                scene_finish("effects");
        }
    }
    if (c == g_scene.rt && g_scene.draws && !g_scene.done && (d->vs.rhw || !d->depth.zenable))
        g_scene.st_over++, g_scene.st_over_rhw += d->vs.rhw;
    /* the interface into the world's target after its effects - the names over heads, floating text; not the game's
     * own offscreen 3D laid over it (a trust's glow) */
    if (d->vs.rhw && c == g_scene.rt && g_scene.done && !scene_composite(d))
        d->overlay = 1, d->label = g_label;
    if (!d->vs.rhw && !d->vs.prog && c == g_scene.rt && g_scene.done)
        d->screen_fx = (uint8_t)screen_effect();
#ifdef VANACORE
    /* an additive draw of a lantern's flame (elight_lantern_tex), a dimmed effect (elight_dim_tex) or an effect on a
     * character (character_effect): the light from the effects takes it at a share */
    if (!d->vs.rhw && d->pipe.blend && d->pipe.dst == 2 && d->pipe.op == 1)
    {
        if (g_lantern_n || g_dim_n)
            d->el_lantern = (uint8_t)lantern_texture(obj(g_dev.cur.tex[0]));
        if (!d->el_lantern && c == g_scene.rt && g_scene.done && character_effect(d, nverts))
            d->el_lantern = 3;
        /* ... and an effect's own model drawn before the world's scene ended (it ends at the first particle, camera_effect):
         * which of them came after that particle changed from frame to frame, and Hu'Xzoi's door glow lit the room one frame
         * in thirty */
        if (c == g_scene.rt && g_scene.draws && !g_scene.done && (!g_scene.world || c == g_scene.world) &&
            d->depth.zenable && !d->depth.zwrite && camera_near())
            d->el_early = 1;
        /* ... and one DLSS 5 isn't to take as a light (dlss5_skip_tex) */
        d->dlss5_skip = (uint8_t)dlss5_skip_texture(obj(g_dev.cur.tex[0]));
    }
#endif
    /* 3D into a target under half the window's area isn't the world's (the game's offscreen 3D: a grab of the scene, a
     * trust's glow); one of [graphics] background's size is, whatever the window's size: 2560x1600 under a 3840x2160 window
     * is 1% under half of it, and no draw was ever the world's - no effect ran and DLSS never started. Nor is the target
     * already found as the scene (GROW_SCENE): the game makes it at background's size as it starts, and background changed
     * in game after that (2560x1600 to 3840x2160) left it under half again - no scene, so no effects and no DLSS */
    const int bg_sized = (g_scene_w && rt->width == g_scene_w && rt->height == g_scene_h) || (pixels_of(rt)->grow & GROW_SCENE);
    if (d->vs.rhw || !d->depth.zenable || !obj(g_dev.ds) || face || level ||
        (!bg_sized && (uint64_t)rt->width * rt->height * 2 < (uint64_t)g_dev.pp[0] * g_dev.pp[1]))
        return;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t == c || !g_scene.tally[i].t || i == 3)
        {
            if (g_scene.tally[i].t != c)
                g_scene.tally[i].t = c, g_scene.tally[i].n = g_scene.tally[i].v = 0, g_scene.tally[i].laid = 0;
            g_scene.tally[i].n++, g_scene.tally[i].v += nverts;
            break;
        }
    /* 3D into other targets - the game draws characters into one partway through the world, then
     * goes on with the world - is not the world's scene: it neither ends nor starts one */
    if (g_scene.world && c != g_scene.world)
        return;
    /* the world's target (not the back buffer, which 3D may also go over): what [graphics] background
     * and DLSS size (mark_scene) */
    if (g_scene.world && g_dev.rt != g_dev.backbuffer)
        g_scene_obj = g_dev.rt, g_scene_ds = g_dev.ds != g_dev.depth ? g_dev.ds : 0;
    if (c != g_scene.rt) /* a new scene: the one before is done; the new one's camera and sun are its own */
    {
        scene_finish("switch");
        g_scene.rt = c, g_scene.draws = 0, g_scene.chars = 0, g_scene.done = 0, g_scene.cam_rank = 0, g_scene.sun_draw = 0;
        g_scene.s.cam_draw = g_scene.s.sun_draw = 0, g_scene.s.sun_dir[3] = 0.0f;
    }
    const State* s = &g_dev.cur;
    GfxScene* sc = &g_scene.s;
    /* 3D again after the effects ran: drawn as it is. The world's own pieces among them are casters still (late, scene_caster):
     * too late for this frame's maps, the back end takes them into its cache for the next (gfx_d3d12.c sun_cache_late). Before,
     * they cast nothing: the game drew up to a third of a zone after the effects some frames (Port Jeuno, 2026-11-01: 500 of 1900
     * draws for 25 s at a time, its own offscreen 3D laid over the world partway through it), and a building's whole shadow came
     * and went with the game's draw order as the camera moved, whatever the shadow settings. */
    const int late = g_scene.done;
    if (late)
        g_scene.st_late++, g_scene.tr_late++;
    else
    {
        g_scene.draws++;
        uint32_t zf, zl;
        const Obj* zo = obj(g_dev.ds);
        g_grass_depth = zo ? surface_gpu(zo, &zf, &zl) : NULL;
    }
    /* what casts the sun's shadow (the back end's shadow map): the scene's opaque, depth-writing
     * draws - alpha-tested ones too (leaves, fences) - of the world, which is fogged or drawn by the
     * zone's shaders; not the sky, the sun and its flare (unfogged, with views of their own) */
    /* (not one of the game's unlit water layers made VanaCore's water, water_shade: under the water, nothing to shade) */
    const int water_layer_made = d->fs.water && !d->vs.lighting;
    scene_caster(d, c, s, sc, late, water_layer_made); /* (d3d8_casters.h) */
    if (late)
    {
        late_pool(d, c, s, sc); /* the player light's pool, which the composite missed (d3d8_casters.h) */
        late_count(d, c, s, sc);
        return;
    }
    /* the camera and fog from the frame's first depth-writing fixed-function draw, a fogged one
     * (the world) over any other: the sun, its flare and the sky come later with their own views
     * and no fog, and taking theirs made the effects' fog come and go with the camera's angle */
    uint32_t rank = d->fs.fog ? 2u : 1u;
    if (!d->vs.prog && s->xf[3][11] != 0.0f && d->depth.zwrite && rank > g_scene.cam_rank)
    {
        g_scene.cam_rank = rank, g_scene.cam_draw = sc->cam_draw = g_scene.draws;
        memcpy(sc->proj, s->xf[3], 64);
        memcpy(sc->view, s->xf[2], 64);
        memcpy(sc->vp, d->vp, sizeof sc->vp);
        memcpy(sc->ambient, d->u.ambient, 16);
        memcpy(sc->fogcolor, d->u.fogcolor, 16);
        sc->fog[0] = d->u.params[2], sc->fog[1] = d->u.params[3], sc->fog[2] = d->fs.fog ? 1.0f : 0.0f;
        g_scene.cam = 1;
    }
    /* the sun: the directional light the world's lit draws use most (sun_vote, d3d8_sun_pick) */
    if (d->vs.nlights && d->depth.zwrite && !d->vs.rhw && !water_layer_made) /* (a water layer's lights are borrowed) */
    {
        if (!g_scene.sun_draw)
            g_scene.sun_draw = sc->sun_draw = g_scene.draws;
        sun_vote(s);
    }
}

/* Present: the scene ends here if nothing ended it before; the next frame starts over */
/* The trace: while ~/Library/Caches/FFXI/trace.go (VanaCore: cache/trace.go) exists (checked every 30 frames), the
 * last 1200 frames' scenes go to trace.txt beside it and the effects' own to trace_gfx.txt; then .go is removed. For
 * a flicker the eye catches: what the frame before and after it did. The flicker watch (gfx_watch) writes them too,
 * as flicker.txt and flicker_gfx.txt, a second after its first line in a quiet minute. */
static void trace_write(const char* dir, const char* name)
{
    char out[1024];
    snprintf(out, sizeof out, "%s/%s.txt", dir, name);
    FILE* f = fopen(out, "w");
    if (f)
    {
        fprintf(f, "frame effects ended(s sampled, e effects, i interface, p present, w switch, - none) world-draws late-3D targets "
                   "world-changed camera-rank(2 fogged draw, 1 unfogged, 0 none: the last kept) watch-frame\n");
        uint32_t n = g_scene.frame < 1200 ? g_scene.frame : 1200;
        for (uint32_t k = g_scene.frame - n; k < g_scene.frame; ++k)
        {
            struct SceneTrace* e = &g_scene.trace[k % 1200];
            fprintf(f, "%u %d %c %u %u %u %d %d %llu\n", e->frame, e->ran, e->why ? e->why : '?', e->draws, e->late, e->targets,
                e->changed, e->cam, (unsigned long long)e->serial);
        }
        fclose(f);
    }
    snprintf(out, sizeof out, "%s/%s_gfx.txt", dir, name);
    gfx_trace_dump(out);
}

static const char* scene_end_name(char why)
{
    return why == 's' ? "the scene drawn elsewhere" : why == 'e' ? "the first effect over the world" : why == 'i' ? "the interface"
        : why == 'p' ? "Present" : why == 'w' ? "3D into another target" : "nothing";
}

/* the flicker watch's look at the world's scene: how this frame's differs from the one before's */
static void scene_watch(const struct SceneTrace* t, const struct SceneTrace* p)
{
    static const char* const CAM[3] = { "no draw (the last frame's kept)", "an unfogged draw", "a fogged draw (the world)" };
    if (p->ran && !t->ran)
        gfx_watch("the effects didn't run on the world's scene (%u large 3D targets%s): no grade, fog, shadows, bloom or god rays "
                  "this frame", t->targets, t->changed ? ", the world's target changed" : "");
    /* said when it starts, not every frame it goes on (a glowing trust in view, frame after frame) */
    static uint32_t composites_before;
    if (g_scene.composites && !composites_before)
        gfx_watch("screen-space drawing over the world partway through it (%u draws; the first, %s): taken as part of the world, "
                  "not the interface", g_scene.composites, g_scene.comp_desc);
    composites_before = g_scene.composites;
    if (!p->ran || !t->ran)
        return;
    int early = p->draws >= 50 && t->draws * 5 < p->draws * 4 && t->late > p->late + 10;
    if (t->why != p->why)
        gfx_watch("the effects ran at %s (the frame before, at %s)%s%s", scene_end_name(t->why), scene_end_name(p->why),
            t->why == 'i' && !early ? ": " : "", t->why == 'i' && !early ? g_scene.end_desc : "");
    if (early)
        gfx_watch("the effects ran early: after %u of the world's 3D draws (the frame before %u); %u went on after them, "
                  "unshaded and ungraded (the frame before %u)%s%s", t->draws, p->draws, t->late, p->late,
            t->why == 'i' ? ". It ended at " : "", t->why == 'i' ? g_scene.end_desc : "");
    else
        late_watch(t, p);
    if (t->cam != p->cam && t->cam >= 0 && t->cam <= 2 && p->cam >= 0 && p->cam <= 2)
        gfx_watch("the effects' camera came from %s (the frame before, %s)", CAM[(int)t->cam], CAM[(int)p->cam]);
    if (t->changed)
        gfx_watch("the world's target changed");
    if (t->targets != p->targets)
        gfx_watch("3D went to %u large targets (the frame before, %u)", t->targets, p->targets);
}

/* the flicker watch's toggle finder (gfx_watch_value), fed what the front end saw this frame; the counters start over */
static void scene_values(const struct SceneTrace* t)
{
    gfx_watch_value("scene: large 3D targets this frame", (float)t->targets);
    gfx_watch_value("scene: the game's offscreen 3D laid over the world", (float)(g_scene.composites > 0));
    gfx_watch_value("scene: effects ended by (e effects, i interface, p present, s sampled, w switch)", (float)t->why);
    gfx_watch_value("scene: effects ran", (float)t->ran);
    gfx_watch_value("lights: a point light in a lit draw", (float)(g_wf.points > 0));
    gfx_watch_value("lights: the brightest point light", g_wf.point_luma);
    gfx_watch_value("lights: a spot light in a lit draw", (float)(g_wf.spots > 0));
    gfx_watch_value("lights: the brightest spot light", g_wf.spot_luma);
    gfx_watch_value("lights: directions the lit draws voted for", (float)g_wf.slots);
    gfx_watch_value("glow picture: 3D drawn into it", (float)(g_wf.glow_3d > 0));
    gfx_watch_value("glow picture: screen-space draws into it (the light probes)", (float)g_wf.glow_ui);
    gfx_watch_value("glow: laid over the world (glow_draw)", (float)g_wf.glow_lays);
    gfx_watch_value("glow: tint's alpha", g_wf.glow_lays ? g_wf.glow_alpha : 0.0f);
    gfx_watch_value("glow: screen-space draws into its picture before it was laid",
        g_wf.glow_lays ? (float)g_wf.glow_probes_at_lay : -1.0f);
    gfx_watch_value("glow: world draws before it was laid (in 50s)",
        g_wf.glow_lays ? floorf((float)g_wf.glow_draws_at_lay / 50.0f) : -1.0f);
    g_wf.points = g_wf.spots = 0, g_wf.point_luma = g_wf.spot_luma = 0.0f;
    g_wf.glow_3d = g_wf.glow_ui = g_wf.glow_lays = 0;
}

static void scene_trace(GfxTex* world_before)
{
    uint32_t targets = 0;
    for (int i = 0; i < 4; ++i)
        targets += g_scene.tally[i].t != NULL;
    struct SceneTrace* t = &g_scene.trace[g_scene.frame % 1200];
    t->frame = g_scene.frame, t->draws = g_scene.tr_draws, t->late = g_scene.tr_late, t->targets = targets;
    t->why = g_scene.world_done ? g_scene.tr_why : '-', t->ran = (char)g_scene.world_done;
    t->changed = (char)(world_before != g_scene.world), t->cam = (char)g_scene.cam_rank;
    t->serial = gfx_frame();
    if (g_scene.frame)
        scene_watch(t, &g_scene.trace[(g_scene.frame - 1) % 1200]);
    scene_values(t);
    g_scene.end_desc[0] = g_scene.comp_desc[0] = 0, g_scene.composites = 0;
    g_scene.tr_late = g_scene.tr_draws = 0, g_scene.tr_why = 0;
    g_scene.frame++;
    static char dir[900];
#ifdef VANACORE
    if (!dir[0] && getenv("FFXI_CACHE_DIR"))
        snprintf(dir, sizeof dir, "%s", getenv("FFXI_CACHE_DIR"));
#else
    if (!dir[0] && getenv("HOME"))
        snprintf(dir, sizeof dir, "%s/Library/Caches/FFXI", getenv("HOME"));
#endif
    if (dir[0] && gfx_watch_dump_due())
        trace_write(dir, "flicker");
    if (!dir[0] || g_scene.frame % 30)
        return;
    char go[1024];
    snprintf(go, sizeof go, "%s/trace.go", dir);
    struct stat st;
    if (stat(go, &st))
        return;
    remove(go);
    trace_write(dir, "trace");
}

static void scene_present(void)
{
    scene_finish("present");
    GfxTex* world_before = g_scene.world;
    /* next frame's world: this frame's busiest target, by the vertices drawn into it, not the draws. A character the game
     * shows see-through is drawn into a target of its own, as large, and laid over the world: in a zone of few large
     * meshes it took more draws than the world did (Dragon's Aery: the character 62 draws and 7,826 vertices, the cave 54
     * and 84,735), and as the camera turned the world's view went back and forth between them - the grass saw no ground
     * and the effects ran on the character's picture in the frames it was taken. Not a target laid over another (scene_laid),
     * while one that wasn't took any 3D */
    uint32_t best = 0;
    int unlaid = 0;
    for (int i = 0; i < 4; ++i)
        unlaid |= g_scene.tally[i].t && g_scene.tally[i].v && !g_scene.tally[i].laid;
    for (int i = 0; i < 4; ++i)
        if (g_scene.tally[i].t && g_scene.tally[i].v > best && !(unlaid && g_scene.tally[i].laid))
            best = g_scene.tally[i].v, g_scene.world = g_scene.tally[i].t;
    scene_trace(world_before);
    g_scene.world_done = 0;
    g_water_tiles = 0, g_water_shape_tiles = 0, g_water_kept_n = 0;
    water_feet_turn();
    for (int i = 0; i < 4; ++i)
        g_scene.tally[i].t = NULL, g_scene.tally[i].n = g_scene.tally[i].v = 0, g_scene.tally[i].laid = 0;
    static uint32_t st_epoch;
    if (gfx_profiling && st_epoch != gfx_profile_epoch)
    {
        /* the profile just came on: the scenes counted while it was off aren't these 120 frames' */
        st_epoch = gfx_profile_epoch;
        memset(g_scene.st_why, 0, sizeof g_scene.st_why), memset(g_scene.st_cam, 0, sizeof g_scene.st_cam);
        memset(g_scene.st_late_out, 0, sizeof g_scene.st_late_out);
        g_scene.st_frames = g_scene.st_late = g_scene.st_late_cast = g_scene.st_over = g_scene.st_over_rhw = 0;
    }
    if (gfx_profiling && ++g_scene.st_frames == 120)
    {
        fprintf(stderr, "[recomp] d3d8: scenes (120 frames, the last on %s): ended sampled %u, interface %u, present %u, switch %u; camera fogged %u, "
            "unfogged %u, none %u; 3D after the effects %u (%u of it the world's, casting from the next frame; left out: %u into "
            "another target, %u screen-space, %u through another camera, %u blended, %u writing no depth, %u other); over the scene "
            "before them %u (%u screen-space)\n",
            g_scene.rt == g_scene.world ? "the world's view" : "another target", g_scene.st_why[0], g_scene.st_why[1], g_scene.st_why[2],
            g_scene.st_why[3], g_scene.st_cam[2], g_scene.st_cam[1], g_scene.st_cam[0],
            g_scene.st_late, g_scene.st_late_cast, g_scene.st_late_out[0], g_scene.st_late_out[1], g_scene.st_late_out[2],
            g_scene.st_late_out[3], g_scene.st_late_out[4], g_scene.st_late_out[5], g_scene.st_over, g_scene.st_over_rhw);
        memset(g_scene.st_why, 0, sizeof g_scene.st_why), memset(g_scene.st_cam, 0, sizeof g_scene.st_cam);
        memset(g_scene.st_late_out, 0, sizeof g_scene.st_late_out);
        g_scene.st_frames = g_scene.st_late = g_scene.st_late_cast = g_scene.st_over = g_scene.st_over_rhw = 0;
    }
    g_scene.done = 0, g_scene.chars = 0;
    g_scene.cam_draw = g_scene.sun_draw = g_scene.cam_rank = 0;
    /* the next frame's sun is its own: one in view space kept from this frame would be turned by the next
     * frame's camera (gfx_scene_done takes it into the world through it), and the shadows and god rays with
     * it, whenever no lit draw came before the effects. The camera is kept (cam_draw 0 says so). */
    g_scene.s.cam_draw = g_scene.s.sun_draw = 0, g_scene.s.sun_dir[3] = 0.0f;
}

/* --- drawing at the window's size, and the scene at its own (resize_targets) ------------------------
 * At each Present, the targets that follow the drawn size (GROW_SCREEN: the back buffer, its depth
 * buffer, the menu target) or the scene's (GROW_SCENE: the target the 3D scene is drawn into, and
 * its depth surface) are made again when that size changed. The game still sees its own sizes: its
 * viewports and clears are scaled onto them (scale_to_target), reads come back at its size
 * (read_grown). They are all drawn afresh each frame, so nothing is lost. */
static uint32_t g_resized_w, g_resized_h, g_resized_sw, g_resized_sh;
static float g_resized_scale;

/* o's GPU texture made again at pw x ph (0 x 0: the game's size) */
static void regrow(Obj* o, uint32_t pw, uint32_t ph)
{
    if (pw == o->width && ph == o->height)
        pw = ph = 0;
    if ((pw == o->pw && ph == o->ph) || !o->gpu)
        return;
    int depth = (o->usage & USAGE_DEPTHSTENCIL) != 0;
    GfxTex* old = o->gpu;
    o->gpu = gfx_tex_create(GFX_TEX_2D, o->format, pw ? pw : o->width, ph ? ph : o->height, 1,
        depth ? GFX_USE_DEPTH : GFX_USE_RT);
    o->pw = pw, o->ph = ph;
    scene_moved(old, o->gpu);
    gfx_tex_destroy(old);
}

/* The scene's target, once found, is sized by [graphics] background or DLSS: a one-level render-target
 * texture's surface or a standalone render target, and the depth surface drawn with it when it's
 * of the same size. */
static void mark_scene(void)
{
    const Obj* s = obj(g_scene_obj);
    Obj* o = s && s->kind == O_SURFACE ? pixels_of(s) : NULL;
    if (!o || (o->grow & GROW_SCENE) || !(o->usage & USAGE_RENDERTARGET) || (o->kind == O_TEXTURE && o->nsubs != 1))
        return;
    o->grow |= GROW_SCENE;
    Obj* z = obj(g_scene_ds);
    if (z && z->kind == O_SURFACE && !z->container && z->width == o->width && z->height == o->height)
        z->grow |= GROW_SCENE;
    g_resized = 0;
}

void d3d8_scene_size(uint32_t* w, uint32_t* h)
{
    const Obj* s = obj(g_scene_obj);
    Obj* o = s && s->kind == O_SURFACE ? pixels_of(s) : NULL;
    *w = o ? (o->pw ? o->pw : o->width) : 0;
    *h = o ? (o->ph ? o->ph : o->height) : 0;
}

/* a GROW_GLOW target's size: [graphics] glow's, at least its own; its own when the game's glow isn't drawn */
static uint32_t glow_size(const Obj* o)
{
    uint32_t g = g_glow_mode > 0 ? (uint32_t)g_glow_mode : o->width;
    g = g > 4096 ? 4096 : g;
    return g > o->width ? g : o->width;
}

static void resize_targets(void)
{
    if (!obj(g_dev.backbuffer))
        return;
    mark_scene();
    uint32_t dw, dh;
    drawn_size(&dw, &dh);
    float k = g_scene_scale;
    if (g_resized && dw == g_resized_w && dh == g_resized_h && k == g_resized_scale && g_scene_w == g_resized_sw &&
        g_scene_h == g_resized_sh)
        return;
    /* the scene: DLSS's share of the drawn size, neither side past 8192; else [graphics] background's size */
    double f = k;
    if (f > 0 && (dw * f > 8192.0 || dh * f > 8192.0))
        f = 8192.0 / (dw > dh ? dw : dh);
    uint32_t sw = (uint32_t)(dw * f + 0.5), sh = (uint32_t)(dh * f + 0.5);
    if (!(k > 0) && g_scene_w)
        sw = g_scene_w, sh = g_scene_h;
    const int sized = k > 0 || g_scene_w;
    for (uint32_t i = 1; i < g_nobjs; ++i)
    {
        Obj* o = &g_objs[i];
        if (o->kind == O_FREE || !o->grow)
            continue;
        if ((o->grow & GROW_SCENE) && sized)
            regrow(o, sw, sh);
        else if (o->grow & GROW_SCREEN)
            regrow(o, dw, dh);
        else if (o->grow & GROW_GLOW)
        {
            uint32_t g = glow_size(o);
            if (g != (o->pw ? o->pw : o->width))
                rt_log("[recomp] d3d8: the glow's %ux%u render target drawn at %ux%u\n", o->width, o->height, g, g);
            regrow(o, g, g);
        }
        else
            regrow(o, 0, 0);
    }
    /* said when it changes, and the first time when it isn't the game's size */
    if ((dw != g_resized_w || dh != g_resized_h) && (g_resized_w || dw != g_dev.pp[0] || dh != g_dev.pp[1]))
        rt_log("[recomp] d3d8: drawing at %ux%u (the game's %ux%u)\n", dw, dh, g_dev.pp[0], g_dev.pp[1]);
    static uint32_t said_sw, said_sh;
    uint32_t gw = 0, gh = 0;
    if (g_scene_obj)
        d3d8_scene_size(&gw, &gh);
    if (g_scene_obj && gw && (gw != said_sw || gh != said_sh))
    {
        if (k > 0)
            rt_log("[recomp] d3d8: the 3D scene drawn at %ux%u (%.3g times %ux%u)\n", gw, gh, f, dw, dh);
        else
            rt_log("[recomp] d3d8: the 3D scene drawn at %ux%u (%s)\n", gw, gh,
                g_scene_w ? "[graphics] background" : "the game's background resolution");
        said_sw = gw, said_sh = gh;
    }
    g_resized = 1, g_resized_w = dw, g_resized_h = dh, g_resized_scale = k;
    g_resized_sw = g_scene_w, g_resized_sh = g_scene_h;
}

static void draw_packet(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size,
    uint32_t up_data, uint32_t up_stride, uint32_t n);

/* A draw: from `start` (non-indexed), through guest indices, or from ...UP memory. */
static void draw(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size, uint32_t up_data,
    uint32_t up_stride)
{
    uint32_t n = prim_vertices(prim, count);
    if (!n)
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    draw_packet(prim, count, start, indices, index_size, up_data, up_stride, n);
    if (gfx_profiling)
        gfx_prof_front(gfx_now_ns() - t0);
}

/* The game's cursor: a small screen-space draw (its sprite, 18 pixels in a 1280-wide menu) from the cursor's 64x64 sheet
 * whose box holds the point the game was last given the mouse at. The sheet matters: with the mouse outside the interface box
 * that point is raw, and a menu glyph whose unsqueezed place lay under it was taken for the cursor and drawn there, gone from
 * its window (the T of VIT in Status) (from XI on Anything, rubymatrix/xi-on-anything 40c6e45). A sheet an HD pack made
 * larger (pivot's ffxi-hd: 2x, 4x) is the cursor's by the share of it the sprite takes, which the upscale keeps: a cursor
 * frame is a quarter of its sheet or more, a glyph a tenth of its font's or less (CURSOR_SHARE). */
#define CURSOR_SHARE 0.15f
static int ui_cursor_share(const GfxDraw* d, uint32_t base, uint32_t stride, uint32_t size, uint32_t first, uint32_t n)
{
    const GfxElem* tc = &d->vs.el[GFX_R_TEXCOORD0];
    if (!tc->used || tc->stream != d->vs.el[GFX_R_POSITION].stream || tc->type != GFX_FLOAT2)
        return 0;
    float ulo = 1e30f, uhi = -1e30f, vlo = 1e30f, vhi = -1e30f;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_TEXCOORD0];
        if (at > size - 8)
            return 0;
        float u = u2f(rd32(base + at)), v = u2f(rd32(base + at + 4));
        ulo = u < ulo ? u : ulo, uhi = u > uhi ? u : uhi, vlo = v < vlo ? v : vlo, vhi = v > vhi ? v : vhi;
    }
    return uhi - ulo >= CURSOR_SHARE && vhi - vlo >= CURSOR_SHARE && uhi - ulo <= 1.0f && vhi - vlo <= 1.0f;
}

static int ui_is_cursor(GfxDraw* d, uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride)
{
    const Obj* sheet = obj(g_dev.cur.tex[0]);
    if (!sheet || !sheet->width || !sheet->height)
        return 0;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        const Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo = 1e30f, hi = -1e30f, ylo = 1e30f, yhi = -1e30f, W, H, fx, fy;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 8)
            return 0;
        float x = u2f(rd32(base + at)), y = u2f(rd32(base + at + 4));
        lo = x < lo ? x : lo, hi = x > hi ? x : hi, ylo = y < ylo ? y : ylo, yhi = y > yhi ? y : yhi;
    }
    ui_target_size(&W, &H);
    user32_mouse_given(&fx, &fy);
    float mx = fx * W, my = fy * H, most = 48.0f * W / 1280.0f;
    int hit = hi - lo <= most && yhi - ylo <= most && mx >= lo - 2.0f && mx <= hi + 2.0f && my >= ylo - 2.0f &&
        my <= yhi + 2.0f;
    if (hit && (sheet->width > 64 || sheet->height > 64))
        hit = ui_cursor_share(d, base, stride, size, first, n);
    return hit;
}

/* The target arrow: the game puts it over its target's place on the screen, a sprite of the cursor's
 * 64x64 sheet (cursor frames, the menus' hand, the arrow) drawn turned a quarter - the only sprite of
 * the interface drawn so (the lobby, menus and zones checked): the texture's u runs down the screen.
 * It stays where the game put it, over the world. */
static int ui_is_world_pointer(uint32_t first, uint32_t n, uint32_t up_data, uint32_t up_stride)
{
    const Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->width != 64 || t->height != 64 || n != 4 || g_dev.cur.vs != 0x144)
        return 0;
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        const Obj* b = obj(g_dev.cur.stream[0]);
        if (!b || !b->mem)
            return 0;
        base = b->mem, stride = g_dev.cur.stride[0], size = b->size;
    }
    uint32_t a = first * stride, c = a + stride; /* XYZRHW, diffuse, then u v at 20 */
    if (stride < 28 || c + 28 > size)
        return 0;
    float x0 = u2f(rd32(base + a)), y0 = u2f(rd32(base + a + 4)), u0 = u2f(rd32(base + a + 20));
    float x1 = u2f(rd32(base + c)), y1 = u2f(rd32(base + c + 4)), u1 = u2f(rd32(base + c + 20));
    return fabsf(x1 - x0) < 0.5f && fabsf(y1 - y0) >= 1.0f && fabsf(u1 - u0) > 1e-4f;
}

/* A surface of FFXI's menu target: a render-target texture smaller than the screen of the interface
 * box's shape under --ui-aspect (1280x720 for 16:9), which the game stretches over the whole screen */
static int menu_target(const Obj* o)
{
    const Obj* t = o && o->container ? obj(o->container) : NULL;
    return t && t->kind == O_TEXTURE && (t->usage & USAGE_RENDERTARGET) && ui_box_shape(t->width, t->height);
}

/* Window opacity: the interface's window backgrounds (FVF 0x144 quads with the 128x128 window texture) have the alpha they're
 * given (the game's window shapes carry it in their vertex colours: 0xC0 for the chat and help windows) times these. */
static float g_win_alpha = 1.0f, g_chat_alpha = 1.0f;
static float g_border_alpha = 1.0f, g_chat_border_alpha = 1.0f;
static int g_win_tint, g_chat_tint;          /* a colour for the window backgrounds / the chat's: on */
static uint32_t g_win_tint_rgb, g_chat_tint_rgb; /* 0xRRGGBB */
static int g_border_tint, g_chat_border_tint;      /* the same for the frames */
static uint32_t g_border_tint_rgb, g_chat_border_tint_rgb;
static float g_chat_rects[2][4];
static unsigned g_chat_rect_count;

void d3d8_set_window_opacity(float all, float chat)
{
    g_win_alpha = all < 0.0f ? 0.0f : all > 1.0f ? 1.0f : all;
    g_chat_alpha = chat < 0.0f ? 0.0f : chat > 1.0f ? 1.0f : chat;
}

/* The title screen and the character select lay the window texture over the whole picture as a backdrop: a quad of the 128x128
 * texture (four vertices, XYZRHW + diffuse + one texture) that covers the render target from edge to edge. Drawn as the game
 * draws it while the windows look as the game's do, else left out (windows_own_look): it's a window background to the
 * windows' look, so their opacity, colour, picture and corners would lay it solid over the scene and the character. */
static int is_backdrop(uint32_t prim, uint32_t count, uint32_t up_data, uint32_t up_stride, uint32_t n)
{
    if (!up_data || prim != 5 || count != 2 || n != 4 || up_stride != 28 || g_dev.cur.vs != 0x144)
        return 0;
    const Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->width != 128 || t->height != 128)
        return 0;
    float lo_x = 1e30f, lo_y = 1e30f, hi_x = -1e30f, hi_y = -1e30f;
    for (int i = 0; i < 4; ++i)
    {
        float x = u2f(rd32(up_data + 28 * i)), y = u2f(rd32(up_data + 28 * i + 4));
        lo_x = x < lo_x ? x : lo_x, hi_x = x > hi_x ? x : hi_x, lo_y = y < lo_y ? y : lo_y, hi_y = y > hi_y ? y : hi_y;
    }
    float w, h;
    ui_target_size(&w, &h);
    return lo_x <= 1.0f && lo_y <= 1.0f && hi_x >= w - 1.0f && hi_y >= h - 1.0f;
}

/* A quad the game has pushed off the bottom of its target and folded back onto the last row: the party window's empty member
 * slots (a party of fewer than six) lie below the screen with their bottoms clamped to its last row (y 1618.5 in a 1620-high
 * interface), two of them 35% black, which left a dark line a pixel high under the window. The first vertex (the texture's
 * top) is below the target and the third (its bottom) on the last row, above it. */
static int is_folded_off(uint32_t prim, uint32_t count, uint32_t up_data, uint32_t up_stride, uint32_t n)
{
    if (!up_data || prim != 5 || count != 2 || n != 4 || up_stride < 20 || (g_dev.cur.vs != 0x144 && g_dev.cur.vs != 0x44))
        return 0;
    const Obj* rt = obj(g_dev.rt);
    if (!rt || rt->height == 0)
        return 0;
    const float h = (float)rt->height;
    const float top = u2f(rd32(up_data + 4)), bottom = u2f(rd32(up_data + 2 * up_stride + 4));
    return top >= h && bottom < top && bottom >= h - 2.0f;
}

void d3d8_set_window_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb)
{
    g_win_tint = all_on != 0, g_win_tint_rgb = all_rgb & 0xFFFFFFu;
    g_chat_tint = chat_on != 0, g_chat_tint_rgb = chat_rgb & 0xFFFFFFu;
}

void d3d8_set_border_colours(int all_on, uint32_t all_rgb, int chat_on, uint32_t chat_rgb)
{
    g_border_tint = all_on != 0, g_border_tint_rgb = all_rgb & 0xFFFFFFu;
    g_chat_border_tint = chat_on != 0, g_chat_border_tint_rgb = chat_rgb & 0xFFFFFFu;
}

void d3d8_set_border_opacity(float all, float chat)
{
    g_border_alpha = all < 0.0f ? 0.0f : all > 1.0f ? 1.0f : all;
    g_chat_border_alpha = chat < 0.0f ? 0.0f : chat > 1.0f ? 1.0f : chat;
}

static float g_hide_rects[2][4];
static unsigned g_hide_count;

void d3d8_set_hidden_windows(const float* rects, unsigned count)
{
    g_hide_count = count > 2 ? 2 : count;
    for (unsigned i = 0; i < g_hide_count; ++i)
        memcpy(g_hide_rects[i], rects + 4 * i, sizeof g_hide_rects[i]);
}

void d3d8_set_chat_rects(const float* rects, unsigned count)
{
    g_chat_rect_count = count > 2 ? 2 : count;
    for (unsigned i = 0; i < g_chat_rect_count; ++i)
        memcpy(g_chat_rects[i], rects + 4 * i, sizeof g_chat_rects[i]);
}

/* The chat line's window (Enter's input line): its own opacities and colours, over the chat windows' and every window's */
static float g_input_rect[4];
static int g_input_open;
static float g_input_alpha = 1.0f, g_input_border_alpha = 1.0f;
static int g_input_tint, g_input_border_tint;
static uint32_t g_input_tint_rgb, g_input_border_tint_rgb;
static int g_border_solid, g_chat_border_solid, g_input_border_solid; /* frames drawn as solid lines (window_alpha) */

void d3d8_set_border_solid(int all, int chat, int input)
{
    g_border_solid = all != 0, g_chat_border_solid = chat != 0, g_input_border_solid = input != 0;
}

void d3d8_set_input_rect(const float* rect)
{
    g_input_open = rect != NULL;
    if (rect)
        memcpy(g_input_rect, rect, sizeof g_input_rect);
}

void d3d8_set_input_look(float alpha, float border_alpha, int tint_on, uint32_t tint_rgb, int border_on, uint32_t border_rgb)
{
    g_input_alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    g_input_border_alpha = border_alpha < 0.0f ? 0.0f : border_alpha > 1.0f ? 1.0f : border_alpha;
    g_input_tint = tint_on != 0, g_input_tint_rgb = tint_rgb & 0xFFFFFFu;
    g_input_border_tint = border_on != 0, g_input_border_tint_rgb = border_rgb & 0xFFFFFFu;
}

#include "d3d8_winimage.h" /* winimage_for, _verts, _look: [client] window.image, a picture in place of the window backgrounds */
#include "d3d8_corners.h"  /* wc_window, wc_solid, wc_apply: [client] window.corners, the windows' corners cut or rounded */

/* [client] chat.shadow: the chat's text with a shadow (1) or an outline (2) under it, chat.shadow.opacity dark (GfxDraw.text_shadow) */
static int g_text_shadow;
static uint8_t g_text_alpha;
static int g_chat_drawing; /* the game is drawing chat rows, nested (d3d8_chat_drawing: the chat windows', the log maximized's) */

void d3d8_set_chat_shadow(int mode, float opacity)
{
    g_text_shadow = mode == 1 || mode == 2 ? mode : 0;
    opacity = opacity < 0.0f ? 0.0f : opacity > 1.0f ? 1.0f : opacity;
    g_text_alpha = (uint8_t)(opacity * 255.0f + 0.5f);
}

/* A point of the interface in a chat window (inside one of chat.cpp's rectangles), or anywhere while the game draws chat rows
 * (the chat log maximized is no chat window) */
static int chat_place(float cx, float cy)
{
    for (unsigned i = 0; i < g_chat_rect_count; ++i)
        if (cx >= g_chat_rects[i][0] && cx <= g_chat_rects[i][2] && cy >= g_chat_rects[i][1] && cy <= g_chat_rects[i][3])
            return 1;
    return g_chat_drawing != 0;
}

/* A glyph of the chat's text: FFXI draws its text a glyph at a time, a strip of four XYZRHW vertices (16x16 in the
 * interface's pixels) from the font's sheet, its colour added to what's behind it (ONE, ONE), into the interface; one
 * inside a chat window's rectangle is the chat's. Its ink is in the sheet's texels, so the shadow is drawn from them. */
static int chat_glyph(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t up_data, uint32_t up_stride, uint32_t n)
{
    if (!g_text_shadow || !g_text_alpha || !up_data || prim != 5 || count != 2 || n != 4 || up_stride < 16 ||
        !d->vs.rhw || d->vs.prog || d->fs.prog || !d->pipe.blend || d->pipe.src != 2 || d->pipe.dst != 2 || d->pipe.op != 1 ||
        d->fs.st[0].tex != 1)
        return 0;
    /* not VanaCore's own drawing, nor the world's target (names over heads, damage numbers): the interface's (menu_target
     * knows that one only while [ui] aspect squeezes it) */
    if (!g_squeeze || g_own_pixels)
        return 0;
    {
        uint32_t face, level;
        const Obj* rt = obj(g_dev.rt);
        if (rt && g_scene.rt && surface_gpu(rt, &face, &level) == g_scene.rt)
            return 0;
    }
    const Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->kind != O_TEXTURE || (t->usage & USAGE_RENDERTARGET))
        return 0;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (uint32_t i = 0; i < 4; ++i)
        for (int c = 0; c < 2; ++c)
        {
            float v = u2f(rd32(up_data + up_stride * i + 4 * c));
            lo[c] = v < lo[c] ? v : lo[c];
            hi[c] = v > hi[c] ? v : hi[c];
        }
    /* (written so that a NaN fails) */
    if (!(hi[0] - lo[0] > 0.0f && hi[0] - lo[0] <= 48.0f && hi[1] - lo[1] > 0.0f && hi[1] - lo[1] <= 48.0f))
        return 0;
    return chat_place((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f);
}

/* A window background's quad (four vertices, XYZRHW + diffuse + one texture, the 128x128 window texture, at least 64 x 24):
 * its alpha scaled, in a copy of the vertices (the game's own are left as they were). The copy's address, or up_data. */
static uint32_t window_alpha(const GfxDraw* dr, uint32_t prim, uint32_t count, uint32_t up_data, uint32_t up_stride, uint32_t n, int* scaled,
    int* tinted)
{
    static uint8_t copy[4 * 28], copy_border[4 * 28];
    if (!up_data || prim != 5 || count != 2 || n != 4 || up_stride != 28 || g_dev.cur.vs != 0x144)
        return up_data;
    /* Not what VanaCore, the plugins and addons draw in Present (g_squeeze off: their fonts, VanaCore's interface, the
     * damage numbers' preview), nor anything drawn into the world's target (the names over heads, the damage numbers):
     * their text pictures are textures of their own, and one that came out 128x128 ("Tai", "86") was taken for a
     * window's background and drawn in the windows' colour (navy) */
    if (!g_squeeze || g_own_pixels)
        return up_data;
    {
        uint32_t face, level;
        const Obj* rt = obj(g_dev.rt);
        if (rt && g_scene.rt && surface_gpu(rt, &face, &level) == g_scene.rt)
            return up_data;
    }
    const Obj* t = obj(g_dev.cur.tex[0]);
    if (!t)
        return up_data;
    int background = t->width == 128 && t->height == 128, border = t->width == 32 && t->height == 32;
    if (!background && !border)
        return up_data;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 2; ++c)
        {
            float v = u2f(rd32(up_data + 28 * i + 4 * c));
            lo[c] = v < lo[c] ? v : lo[c];
            hi[c] = v > hi[c] ? v : hi[c];
        }
    float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f, a;
    int on_edge = 0, edge_left = 0, edge_right = 0, edge_top = 0, edge_bottom = 0; /* a frame piece: which, on which sides */
    const float* edge_r = NULL;                                                  /* ... of which background */
    /* the chat line's: inside its window (it sits against the chat window's bottom, so it's asked first and more tightly) */
    const int in_input = g_input_open && cx >= g_input_rect[0] - 2.0f && cx <= g_input_rect[2] + 2.0f && cy >= g_input_rect[1] - 2.0f &&
                         cy <= g_input_rect[3] + 2.0f;
    if (background)
    {
        if ((hi[0] - lo[0] < 64.0f || hi[1] - lo[1] < 24.0f) && !in_input) /* (the line's window can be shorter) */
            return up_data;
        if (g_bg_count < 16)
        {
            g_bg_rects[g_bg_count][0] = lo[0], g_bg_rects[g_bg_count][1] = lo[1];
            g_bg_rects[g_bg_count][2] = hi[0], g_bg_rects[g_bg_count][3] = hi[1];
            ++g_bg_count;
        }
        a = g_win_alpha;
    }
    else
    {
        /* a window's frame piece: a quad of the frame texture lying ON the edge of a background drawn before it: a thin strip
         * along a side (up to 8 pixels thick), or a corner square (up to 32), its outer sides on the background's. Not an
         * icon of the same size (the item list's, the rare/ex flags, the network meter) drawn inside the window. */
        const float tol = 1.5f;
        float w = hi[0] - lo[0], h = hi[1] - lo[1];
        for (unsigned i = 0; i < g_bg_count && !on_edge; ++i)
        {
            const float* r = g_bg_rects[i];
            int left = fabsf(lo[0] - r[0]) <= tol, right = fabsf(hi[0] - r[2]) <= tol;
            int top = fabsf(lo[1] - r[1]) <= tol, bottom = fabsf(hi[1] - r[3]) <= tol;
            /* (inside it give or take 2.5: the chat line's window is 22 high and its end pieces 24, 2 over its top or bottom) */
            const float over = 2.5f;
            int within_x = lo[0] >= r[0] - over && hi[0] <= r[2] + over, within_y = lo[1] >= r[1] - over && hi[1] <= r[3] + over;
            if (!within_x || !within_y)
                continue;
            if ((left || right) && (top || bottom) && w <= 32.0f && h <= 32.0f)
                on_edge = 1; /* a corner */
            else if ((top || bottom) && h <= 8.0f)
                on_edge = 2; /* a strip along the top or the bottom */
            else if ((left || right) && w <= 8.0f)
                on_edge = 3; /* a strip down a side */
            if (on_edge)
            {
                edge_r = r;
                edge_left = left, edge_right = right, edge_top = top, edge_bottom = bottom;
            }
        }
        if (!on_edge)
            return up_data;
        a = g_border_alpha;
    }
    /* a window VanaCore was asked to leave bare (the Target window's [client] target.background=0): its background and the
     * frame pieces on its edge drawn with no alpha */
    for (unsigned i = 0; i < g_hide_count; ++i)
    {
        const float* r = g_hide_rects[i];
        if (cx < r[0] - 2.0f || cx > r[2] + 2.0f || cy < r[1] - 2.0f || cy > r[3] + 2.0f)
            continue;
        uint8_t* bare = background ? copy : copy_border;
        memcpy(bare, GUEST_PTR(up_data), 4 * 28);
        for (int v = 0; v < 4; ++v)
        {
            uint32_t d;
            memcpy(&d, bare + 28 * v + 16, 4);
            d &= 0x00FFFFFFu;
            memcpy(bare + 28 * v + 16, &d, 4);
        }
        if (background)
            *scaled = 1; /* its blending is turned on; a frame piece's already is */
        return (uint32_t)(uintptr_t)bare;
    }
    int in_chat = 0;
    for (unsigned i = 0; i < g_chat_rect_count && !in_input; ++i)
        if (cx >= g_chat_rects[i][0] - 8.0f && cx <= g_chat_rects[i][2] + 8.0f && cy >= g_chat_rects[i][1] - 8.0f &&
            cy <= g_chat_rects[i][3] + 8.0f)
            in_chat = 1;
    if (background)
        wc_window(dr, lo, hi, in_input, in_chat); /* [client] window.corners: drawn in their shape (d3d8_corners.h) */
    if (background && winimage_for(in_chat, in_input)) /* [client] window.image: the player's picture in its place */
    {
        *tinted = 4;
        return winimage_verts(copy, up_data, lo, hi, in_input ? g_input_alpha : in_chat ? g_chat_alpha : a);
    }
    /* a background's colour: the chat line's or the chat's own, else the one for every window (else the game's) */
    int tint = background ? g_win_tint : g_border_tint;
    uint32_t rgb = background ? g_win_tint_rgb : g_border_tint_rgb;
    if (in_input) /* the chat line's own (chat.cpp hands it the windows' or the chat's when it matches them) */
    {
        a = background ? g_input_alpha : g_input_border_alpha;
        tint = background ? g_input_tint : g_input_border_tint;
        rgb = background ? g_input_tint_rgb : g_input_border_tint_rgb;
    }
    else if (in_chat) /* the chat windows have their own: the all-windows one doesn't touch them */
    {
        a = background ? g_chat_alpha : g_chat_border_alpha;
        if (background ? g_chat_tint : g_chat_border_tint)
            tint = 1, rgb = background ? g_chat_tint_rgb : g_chat_border_tint_rgb;
    }
    /* a solid border ([client] window.border.solid and the chat's and the chat line's): each frame piece drawn as a line along
     * its window's edge (d3d8_corners.h wc_solid, round the corners with window.corners) */
    if (!background && edge_r && (in_input ? g_input_border_solid : in_chat ? g_chat_border_solid : g_border_solid))
    {
        const uint32_t alpha = (uint32_t)(255.0f * a + 0.5f);
        const WcEdge e = { edge_r, lo, hi, on_edge, edge_left, edge_top, edge_bottom, wc_on(in_input, in_chat),
                           ((tint ? rgb : 0x8A8FA6u) & 0xFFFFFFu) | (alpha > 255 ? 255 : alpha) << 24 };
        return wc_solid(dr, up_data, &e, tinted);
    }
    int fade = a < 0.999f || g_wc.kind == 1; /* (a background in the corners' shape is blended: its edge fades out) */
    if (!fade && !tint)
        return up_data;
    uint8_t* out = background ? copy : copy_border;
    memcpy(out, GUEST_PTR(up_data), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        uint32_t d;
        memcpy(&d, out + 28 * i + 16, 4);
        if (tint)
        {
            /* the colour drawn as it is (no texture, no doubling: see tinted), the game's top-to-bottom shading kept: the
             * vertex's red against the top's 0x61 */
            float shade = background ? (float)((d >> 16) & 0xFF) / 97.0f : 1.0f; /* a frame piece's colour is flat */
            shade = shade > 1.2f ? 1.2f : shade;
            uint32_t r = (uint32_t)((float)((rgb >> 16) & 0xFF) * shade + 0.5f), g = (uint32_t)((float)((rgb >> 8) & 0xFF) * shade + 0.5f),
                     b = (uint32_t)((float)(rgb & 0xFF) * shade + 0.5f);
            d = (d & 0xFF000000u) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
        }
        if (fade)
        {
            /* a background's alpha is doubled by the stage (0x80 is solid): a fraction of 0x80 gives that fraction; a frame
             * piece's is its own, scaled */
            uint32_t alpha = background ? (uint32_t)(128.0f * a + 0.5f) : (uint32_t)((float)(d >> 24) * a + 0.5f);
            d = (d & 0x00FFFFFFu) | (alpha << 24);
        }
        memcpy(out + 28 * i + 16, &d, 4);
    }
    if (background && fade)
        *scaled = 1; /* its blending is turned on; a frame piece's already is */
    if (tint)
        *tinted = background ? 1 : 2; /* 2: a frame piece */
    return (uint32_t)(uintptr_t)out;
}

/* Every window's background and frame as the game draws them: no opacity, colour, picture, solid line or corner shape of
 * VanaCore's (window.*). The title screens' backdrop is drawn only then (is_backdrop). */
static int windows_own_look(void)
{
    return g_win_alpha >= 0.999f && g_border_alpha >= 0.999f && !g_win_tint && !g_border_tint && !g_border_solid &&
           g_corner_shape == 0 && !winimage_for(0, 0);
}

void d3d8_set_glow(int mode)
{
    if (mode != g_glow_mode)
        g_glow_mode = mode, g_resized = 0; /* the small target's size (resize_targets) */
}

/* The flicker watch's count of what goes into the glow's full-size picture (g_glow_src) in a frame: the entity's 3D, and the
 * screen-space quads (the light flares' occlusion probes share it as a scratch, ffxi-glow-picture-occlusion-probe) */
static void glow_watch(const GfxDraw* d)
{
    if (!gfx_profiling || !g_glow_src)
        return;
    const Obj* rt = obj(g_dev.rt);
    const Obj* into = rt ? pixels_of(rt) : NULL;
    if (into && into->guest == g_glow_src)
    {
        if (d->vs.rhw)
            g_wf.glow_ui++;
        else
            g_wf.glow_3d++;
    }
}

/* FFXI's glow as [graphics] glow asks (g_glow_mode): 1 the draw is drawn, 0 left out */
static int glow_draw(GfxDraw* d)
{
    if (g_glow_mode > 0 || !g_glow_dst || !d->vs.rhw)
        return 1;
    /* The shrinking into the small target is drawn: VanaCore's glow reads the full-size picture, never the small one,
     * but a torch's heat haze (and the like) copies the 3D scene to a 256x256 target the same way and lays that
     * back over the scene as a quad, which it must find drawn, not empty (black smudges over the flames). */
    const uint32_t t0 = g_dev.cur.tex[0];
    if (t0 != g_glow_dst && t0 != g_glow_src)
        return 1;
    /* A copy laid back into the target it was made from isn't the glow, whose picture is the character drawn alone,
     * apart from the scene: it's a heat haze or the like, the 3D scene copied small and laid back over itself. Drawn as
     * the game has it: taken for the glow, its quad was left out and VanaCore's glow drawn from the whole scene, or its
     * solid copy drew from the very target it was drawing into. */
    const Obj* rt = obj(g_dev.rt);
    const Obj* into = rt ? pixels_of(rt) : NULL;
    if (into && into->guest == g_glow_src)
        return 1;
    if (t0 == g_glow_dst && g_dev.cur.rs[27] /* ALPHABLENDENABLE */)
    {
        /* a tinted copy, left out; for VanaCore's glow, at the first of them, the glow in their place and into the
         * target they're laid into, in their tint: the vertex colour twice over (MODULATE2X), its alpha four times
         * (MODULATE4X), and seven copies of it. Then and there: kept for the solid copy, it waited for whichever glow
         * draw came next when there was none (a trust's arrival fades its solid copy in, blended) and went into that
         * one's target, the next shrinking's small target with an avatar out. */
        if (g_glow_mode < 0 && !g_glow_laid)
        {
            const GfxElem* e = &d->vs.el[GFX_R_DIFFUSE];
            uint32_t c = 0xFF808080u;
            if (e->used && e->type == GFX_D3DCOLOR && d->cpu[e->stream])
                memcpy(&c, (const uint8_t*)d->cpu[e->stream] + d->u.offset[GFX_R_DIFFUSE], 4);
            float a = 4.0f * (float)(c >> 24) / 255.0f;
            a = a > 1.0f ? 1.0f : a;
            const float tint[4] = { 2.0f * (float)((c >> 16) & 255) / 255.0f, 2.0f * (float)((c >> 8) & 255) / 255.0f,
                2.0f * (float)(c & 255) / 255.0f, 1.5f * (1.0f - powf(1.0f - a, 7.0f)) };
            int kind = 0;
            GfxTex* full = texture_for_draw(g_glow_src, &kind);
            if (full)
            {
                g_wf.glow_lays++, g_wf.glow_probes_at_lay = g_wf.glow_ui, g_wf.glow_draws_at_lay = g_scene.draws;
                g_wf.glow_alpha = tint[3];
                gfx_glow(full, tint);
            }
            g_glow_laid = 1;
        }
        return 0;
    }
    g_glow_laid = 0; /* the shrinking or the solid copy: the next tinted copies are another glow's */
    if (t0 == g_glow_dst) /* the solid copy: from the full-size picture */
    {
        int kind = 0;
        GfxTex* full = texture_for_draw(g_glow_src, &kind);
        /* ... drawn then and there, never held as interface to go over the scene once it's done (scene_note): the game
         * draws into the full-size picture once it has laid it - a light's occlusion test, a quad of alpha 128 and white
         * tested against the world's depth, read into 16x16 - and a held copy read that, a white box past the solid
         * copy's alpha test, cut off by what's in front of the light and covering what's behind it (by the Veridical
         * Conflux in Abyssea, for a frame each time the light's test came last). The small picture it was made from is
         * taken before, so the game's own glow never showed it. */
        if (full)
            d->tex[0] = full, d->overlay = 0;
    }
    return 1;
}

void d3d8_set_character_light(float k)
{
    g_char_light = k >= 0.0f ? k : 1.0f;
}

void d3d8_set_grass(const GrassSettings* settings)
{
    grass_configure(settings);
}

void d3d8_set_grass_zone(int zone)
{
    grass_set_zone(zone);
    rain_set_zone(zone); /* the thunder weathers' dry zones (d3d8_rain.h) */
    water_set_zone(zone); /* ... and a ferry's sea (d3d8_water_level.h) */
}

/* The characters - players, NPCs, monsters and what they wear: FFXI skins them on the CPU each frame, into dynamic
 * buffers already in the world (a world matrix that moves nothing). The zone's objects are placed by a world matrix,
 * from static buffers. */
static int is_character(const GfxDraw* d, uint32_t up_data);

/* A character drawn in the world (GfxDraw.is_char): lit, and not the water, VanaCore's or the game's (the sky's dome, the water's
 * layers and most effects are dynamic buffers under a world matrix that moves nothing too, drawn unlit: the stencil's mask had
 * a sky's worth of red in it) */
static int world_character(const GfxDraw* d, uint32_t up_data)
{
    return d->vs.lighting && !d->water && !d->fs.water && is_character(d, up_data);
}

static int is_character(const GfxDraw* d, uint32_t up_data)
{
    if (d->vs.rhw || d->vs.prog)
        return 0;
    const float* W = g_dev.cur.xf[24];
    if (W[12] != 0.0f || W[13] != 0.0f || W[14] != 0.0f)
        return 0;
    const Obj* vb = up_data ? NULL : obj(g_dev.cur.stream[d->vs.el[GFX_R_POSITION].stream]);
    return up_data || (vb && (vb->usage & USAGE_DYNAMIC));
}

/* The characters' light (is_character, lit by the fixed function's lights) - the ambient and each light's ambient and
 * diffuse, what the vertex colours are lit by - times [graphics] characters.brightness; lit per vertex or per pixel
 * alike. After scene_note: the effects' ambient stays the zone's.
 * Above 1 the light is raised before the shader's clamps (it lifts the dim parts most). Below 1 the light is left as the game
 * gave it and the finished colour is dimmed (the ambient's alpha carries 1 - k, gfx_hlsl.c emit_ff_ps): the light taken down
 * before the clamps brought white clothing out from under them, and the game's light stepping from frame to frame showed on
 * it - a running character's white sleeves bright and dim every 3-4 frames at 0.85 and not at 1. */
static void character_light(GfxDraw* d, uint32_t up_data)
{
    if (g_char_light == 1.0f || !d->vs.lighting || !is_character(d, up_data))
        return;
    if (g_char_light < 1.0f)
    {
        d->u.ambient[3] = 1.0f - g_char_light;
        return;
    }
    for (int c = 0; c < 3; ++c)
        d->u.ambient[c] *= g_char_light;
    for (int i = 0; i < d->vs.nlights; ++i)
        for (int c = 0; c < 3; ++c)
            d->u.light[i].ambient[c] *= g_char_light, d->u.light[i].diffuse[c] *= g_char_light;
}

#include "d3d8_night.h" /* night_light: the world's light through the Vana'diel night ([graphics.effects] night) */

/* D3DRS_ZBIAS: the back end's depth bias is a depth unit a step, as the cards of the game's day had it; the zone's
 * decal is also drawn this far nearer the camera (the uniforms' spare vofs.w, gfx_hlsl_vs_return): 1 cm at the game's
 * 8. A bias in depth alone either loses to the surface up close (where the near plane at 0.1 makes it a hair) or, made
 * larger, moves the decal metres off at a distance: the walls' weathering came through the banners hung a few cm in
 * front of them, and the floors' through the foot of the walls. Not the characters' (is_character): their pieces sit
 * closer together than a centimetre where they meet, and drawn that much nearer, what's behind comes through the piece
 * in front along the seam - slivers at the edges of armour, many pixels wide with the camera close (zoom=distance), too
 * thin to see from the game's own distances. The depth bias alone keeps them as the game drew them. */
static void decal_pull(GfxDraw* d, uint32_t up_data)
{
    if (d->zbias <= 0 || d->vs.rhw || is_character(d, up_data))
        return;
    float pull = (float)d->zbias * 0.00125f;
    memcpy(&d->u.pad0[2], &pull, 4);
}

#ifdef VANACORE
/* The hash of a texture's first level, as texture_replacement keys the texture packs. 0 when the texture has nothing to hash
 * yet. */
static uint64_t texture_hash(Obj* t)
{
    if (t->hash)
        return t->hash;
    if (t->kind != O_TEXTURE || (t->usage & USAGE_RENDERTARGET) || !t->nsubs || !t->subs || t->width < 16 || t->height < 16)
        return 0;
    const Obj* s = obj(t->subs[0]);
    if (!s || !s->mem)
        return 0;
    uint32_t pitch = fmt_pitch(s->format, s->width), blk = fmt_block(s->format);
    size_t n = (size_t)pitch * (blk ? (s->height + 3) / 4 : s->height);
    const uint8_t* p = GUEST_PTR(s->mem);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    t->hash = hash ? hash : 1;
    return t->hash;
}

/* The texture handed to the grass (grass_texture), which reads its colours unless it has them: it lets textures go when its table
 * fills (a long session's zones), so each draw that could be ground hands its texture over again, not only the first */
static void grass_look(Obj* t, uint64_t hash)
{
    const Obj* s = t->nsubs && t->subs ? obj(t->subs[0]) : NULL;
    if (s && s->mem)
        grass_texture(hash, s->format, s->width, s->height, GUEST_PTR(s->mem), fmt_pitch(s->format, s->width));
}

/* capturing the plants round the player (//vana grass capture): the zone's cut-outs, their texture's first level as the game
 * wrote it. The zone's draws are from static buffers (characters and their gear are skinned into dynamic ones, most effects
 * too); a cut-out is alpha-tested, or blended SRCALPHA/INVSRCALPHA (the plants are blended, often after the scene's effects),
 * never added on (an effect's light) */
static void grass_capture_hook(const GfxDraw* d, uint32_t up_data)
{
    if (!grass_capturing() || d->vs.rhw || up_data)
        return;
    const GfxElem* cp = &d->vs.el[GFX_R_POSITION];
    const Obj* cvb = cp->used && cp->stream < GFX_NSTREAMS ? obj(g_dev.cur.stream[cp->stream]) : NULL;
    const int tested = d->fs.alpha_func && d->fs.alpha_func != 8;
    const int cut = d->pipe.blend ? d->pipe.src == 5 && d->pipe.dst == 6 : tested; /* D3DBLEND_SRCALPHA, INVSRCALPHA */
    Obj* st = cvb && !(cvb->usage & USAGE_DYNAMIC) && cut ? obj(g_dev.cur.tex[0]) : NULL;
    const uint64_t sh = st ? texture_hash(st) : 0;
    const Obj* lv = sh && st->nsubs && st->subs ? obj(st->subs[0]) : NULL;
    if (lv && lv->mem)
        grass_capture_note(d, sh, lv->format, lv->width, lv->height, GUEST_PTR(lv->mem), fmt_pitch(lv->format, lv->width));
}

/* An opaque, depth-written draw of the world from a static buffer: a piece of ground, for the grass to look at. After
 * scene_note, so the draw is counted in the scene and its target is the scene's. */
static void grass_hook(const GfxDraw* d, uint32_t prim, uint32_t count, uint32_t first, uint32_t nverts, uint32_t indices,
    uint32_t index_size, uint32_t up_data)
{
    grass_capture_hook(d, up_data);
    if (d->vs.rhw || !d->depth.zenable || !g_scene.draws || g_scene.done)
        return;
    uint32_t face, level;
    Obj* rt = obj(g_dev.rt);
    GfxTex* c = rt ? surface_gpu(rt, &face, &level) : NULL;
    Obj* t0 = obj(g_dev.cur.tex[0]);
    const GfxElem* ep = &d->vs.el[GFX_R_POSITION];
    if (!c || c != g_scene.rt || !t0 || !ep->used || ep->stream >= GFX_NSTREAMS)
        return;
    uint64_t hash = texture_hash(t0);
    if (!hash)
        return;
    Obj* vb = up_data ? NULL : obj(g_dev.cur.stream[ep->stream]);
    /* only the draws that could be ground hand their textures over (the log, all of them: it lists the plants' too); characters'
     * and gear's would fill the grass's table in a long session */
    if (!vb || (vb->usage & USAGE_DYNAMIC))
    {
        if (grass_logging())
            grass_look(t0, hash), grass_reject(hash, 0);
        return;
    }
    /* blended, it's ground only as a layer laid over other ground (grass_layers: SRCALPHA, INVSRCALPHA, depth written or not) */
    if (d->pipe.blend ? !(grass_layers() && d->pipe.src == 5 && d->pipe.dst == 6) : !d->depth.zwrite)
    {
        if (grass_logging())
            grass_look(t0, hash), grass_reject(hash, 1);
        return;
    }
    grass_look(t0, hash);
    GrassSrc s = { hash, g_dev.cur.stream[ep->stream], first, nverts, indices ? GUEST_PTR(indices) : NULL, index_size,
        indices ? (uint32_t)(-d->u.vofs) : 0, prim, count };
    grass_note(d, &s);
}
#endif

#include "d3d8_water.h" /* VanaCore's water: what it is (water_tile_shape, water_model), its draws (water_draw, water_shade) and its pass (water_pass) */
#include "d3d8_rain.h" /* VanaCore's rain: the streaks, splashes and shelter (rain_pass) */

/* The game's own character shadow (Config > Shadows): the character drawn into a render target of the game's, then that
 * texture projected onto the ground round it - a blended 3D draw sampling, through projected coordinates, a texture the game
 * renders into. Left out draw by draw while VanaCore's sun shadows are on and characters cast them (d3d8_set_hide_game_shadows:
 * graphics.cpp, [graphics] gameshadows=auto). game_config.cpp holds the game's option at Off then, but a character's config
 * loading or the Config menu turns it back on for a moment before that's seen, and the two shadows doubled. Left out too where
 * the sun's aren't drawn (night, a Mog House, the effects' pipelines building), as the option is Off there as well. (From XI on
 * Anything, rubymatrix/xi-on-anything, d3d8.c game_shadow_hidden.) */
static int g_hide_game_shadows;

void d3d8_set_hide_game_shadows(int on) { g_hide_game_shadows = on; }

void d3d8_set_fill_mode(int mode) { g_fill_mode = mode == 1 || mode == 2 ? mode : 0; }

void d3d8_set_untextured(int on) { g_untextured = on != 0; }

static uint32_t g_seek;   /* the piece the caster pass is drawing (d3d8_set_seek), 0 none */
static uint16_t g_seek_n; /* ... the draws of it so far */
void d3d8_set_seek(uint32_t piece)
{
    if (piece != g_seek)
    {
        if (g_seek)
            seek_piece_done(g_seek, g_seek_n); /* (the flicker watch: d3d8_casters.h) */
        g_seek_n = 0, g_seek_took = 0, g_seek_why = 0;
    }
    g_seek = piece;
}

static int game_shadow_hidden(const GfxDraw* d)
{
    /* (gfx_sun_chars_now: the environment alone casts VanaCore's at this hour - by day or by night, sun_moon_casters - and the
     * game's own is the characters') */
    if (!g_hide_game_shadows || d->vs.rhw || !d->pipe.blend || d->fs.prog || !gfx_sun_chars_now())
        return 0;
    for (int i = 0; i < d->fs.nstages && i < 8; ++i)
    {
        const Obj* t = obj(g_dev.cur.tex[i]);
        /* the game's shadow picture: a small target of its own, never the world's or the menus' (GROW_SCREEN, GROW_SCENE) */
        if (d->fs.st[i].tex == 1 && d->fs.st[i].projected && t && t->kind == O_TEXTURE && (t->usage & USAGE_RENDERTARGET) &&
            !(t->grow & (GROW_SCREEN | GROW_SCENE)))
        {
            static int told;
            if (!told)
                told = 1, rt_log("[recomp] d3d8: the game's own character shadow drawn with VanaCore's sun shadows on: left out\n");
            return 1;
        }
    }
    return 0;
}

/* a window's piece as window_alpha found it drawn: tinted (1: the vertices' colour alone, the texture's pattern left out; 2: a
 * frame piece whose light the game adds to what's behind it, a colour of the player's own laid over instead; 3: a solid border's
 * line, colour and alpha both the vertices'), or its background scaled (blended by the scaled alpha: the game draws it unblended,
 * the texture being solid); 4: the player's picture in place of a background (d3d8_winimage.h) */
static void window_look(GfxDraw* d, int window_scaled, int window_tinted)
{
    if (window_tinted == 4)
    {
        winimage_look(d);
        return;
    }
    if (window_tinted)
        d->fs.st[0].cop = 3; /* D3DTOP_SELECTARG2: the colour from the vertices alone, the texture's pattern left out */
    if (window_tinted == 3)
    {
        /* a solid border's line: colour and alpha both the vertices' (D3DTOP_SELECTARG1 of D3DTA_DIFFUSE), no texture */
        d->fs.st[0].cop = 2, d->fs.st[0].ca1 = 0, d->fs.st[0].aop = 2, d->fs.st[0].aa1 = 0;
        window_tinted = 2;
    }
    if (window_tinted == 2)
    {
        /* the game adds a frame piece's light to what's behind it (SRCALPHA, ONE): black would add nothing. A colour of
         * the player's own is laid over it instead (SRCALPHA, INVSRCALPHA) */
        d->pipe.blend = 1, d->pipe.src = 5, d->pipe.dst = 6, d->pipe.op = 1;
    }
    if (window_scaled)
    {
        /* the game draws a window's background with blending off (the texture has no alpha: it's solid); with the
         * alpha scaled it's blended onto what's behind it */
        d->pipe.blend = 1, d->pipe.src = 5, d->pipe.dst = 6, d->pipe.op = 1;
    }
}

#include "d3d8_chatfont.h" /* chatfont_keep, _cover, _laid: [client] chat.font's glyphs, at the screen's own pixels */

/* d3d8_drop_draws: how deep in windows drawn into nothing (gamewin.cpp) the game is */
static int g_drop_draws;

void d3d8_drop_draws(int on)
{
    g_drop_draws += on ? 1 : (g_drop_draws > 0 ? -1 : 0);
}

/* A draw made while a window is drawn into nothing: dropped (1), the printer's letter in it noted as drawn, not one True Font
 * missed */
static int drop_draw(void)
{
    if (g_drop_draws <= 0)
        return 0;
    if (g_cf.printer.on == 1)
        g_cf.printer.on = 2;
    return 1;
}

#ifdef VANACORE
/* VanaCore's sky in place of the game's dome (sky.c): a 3D draw scene_note counted into the world's scene (its count was `was`,
 * its target rt_was, before), with its place among them, and whether it's drawn round the camera (view and world matrices with
 * no translation). 1: one of the dome's strips, skipped; 2: the sky to be drawn before it (sky_flush); 0 nothing */
static int sky_hook(const GfxDraw* d, uint32_t prim, uint32_t nverts, uint32_t was, const GfxTex* rt_was)
{
    if (g_scene.done || !(g_scene.draws == was + 1 || (g_scene.draws == 1 && g_scene.rt != rt_was)))
        return 0;
    const float* V = g_dev.cur.xf[2];
    const float* W = g_dev.cur.xf[24];
    const int round = V[12] == 0.0f && V[13] == 0.0f && V[14] == 0.0f && W[12] == 0.0f && W[13] == 0.0f && W[14] == 0.0f;
    return sky_scene_draw(d, prim, nverts, g_scene.draws, round);
}
#endif

static void draw_packet(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size,
    uint32_t up_data, uint32_t up_stride, uint32_t n)
{
    if (drop_draw())
        return;
    GfxDraw* d = &g_draw;
    if (!build_draw(d))
        return;
    if (game_shadow_hidden(d))
        return;
    uint32_t first = start, nverts = n;
    if (indices)
    {
        uint32_t lo, hi;
        index_range(indices, index_size, n, &lo, &hi);
        first = lo + (up_data ? 0 : g_dev.cur.base_vertex);
        nverts = hi - lo + 1;
        d->u.vofs = -(int32_t)lo;
        d->indices = GUEST_PTR(indices);
        d->index_size = index_size;
        Obj* ib = up_data ? NULL : obj(g_dev.cur.ib);
        GfxBuf* gb = ib && indices >= ib->mem && indices < ib->mem + ib->size ? static_buffer(ib) : NULL;
        if (gb && !((indices - ib->mem) & 3) && prim != 6 /* fans are rebuilt from the indices */)
            d->ibuf = gb, d->ibuf_off = indices - ib->mem;
    }
    if ((is_backdrop(prim, count, up_data, up_stride, n) && !windows_own_look()) || is_folded_off(prim, count, up_data, up_stride, n))
        return;
    int window_scaled = 0, window_tinted = 0;
    g_wc.kind = 0;
    up_data = window_alpha(d, prim, count, up_data, up_stride, n, &window_scaled, &window_tinted);
    window_look(d, window_scaled, window_tinted);
    up_data = wc_apply(d, &prim, &count, &nverts, up_data, up_stride); /* [client] window.corners (d3d8_corners.h) */
    if (!set_streams(d, first, nverts, up_data, up_stride))
        return;
    if (g_seek)
    {
        d->piece = g_seek, d->piece_draw = g_seek_n++;
        seek_note(d, prim, count, first, nverts, up_data, up_stride);
        return;
    }
    float water_lo[3] = { 0 }, water_hi[3] = { 0 };
    d->water =(uint8_t)water_model(d, nverts, up_data, water_lo, water_hi);
    if (water_layer(d, prim, nverts, up_data))
        return;
    decal_pull(d, up_data);
    /* after decal_pull: a tile pushed back is pushed back; 2: VanaCore's water to keep for its pass (water_keep, below) */
    const int water_kept = water_draw(d, prim, count, nverts, up_data, d->water, water_lo, water_hi);
    if (water_kept == 1)
        return;
    /* centered (a render target copied at another size, build_draw) is for quads on whole pixels, as the glow's copies and
     * the interface's are. One on half pixels is laid out for D3D8's pixel centres already: the 3D scene's copy onto the
     * screen (-0.5 to 3839.5 from the 4096 background) drawn with today's stopped a pixel short of the right and bottom
     * edges, black there but for the interface's pixels */
    if (d->vs.centered)
    {
        const GfxElem* e = &d->vs.el[GFX_R_POSITION];
        if (e->used && e->stream < GFX_NSTREAMS && d->cpu[e->stream])
        {
            float x;
            memcpy(&x, (const uint8_t*)d->cpu[e->stream] + d->u.offset[GFX_R_POSITION], 4);
            if (fabsf(x - floorf(x) - 0.5f) < 0.01f)
                d->vs.centered = 0;
        }
    }
#ifdef VANACORE
    const uint32_t scene_was = g_scene.draws;
    const GfxTex* scene_rt_was = g_scene.rt;
#endif
    scene_note(d, nverts);
    d->is_char = (uint8_t)world_character(d, up_data);
    night_light(d);
    character_light(d, up_data);
    if (rain_game_skip(d, g_dev.cur.tex[0])) /* the game's own rain left out under VanaCore's (rain.game) */
        return;
    if (water_keep(d, prim, count, water_kept)) /* VanaCore's water to keep: lit as the world is by now; drawn at the scene's end (water_pass) */
        return;
#ifdef VANACORE
    scene_char(d, scene_was);
    if (d->is_char)
        snow_character_draw(d); /* its feet, and lowered into the snow it stands in (snow.c) */
    if (grass_on())
        grass_hook(d, prim, count, first, nverts, indices, index_size, up_data);
#endif
    if (g_cap)
        cap_draw(d, prim, count, first, nverts, up_data, up_stride);
#ifdef VANACORE
    /* a strip of the game's dome: VanaCore's sky is drawn in its place, before the world's first draw (sky.c) */
    const int sky = sky_hook(d, prim, nverts, scene_was, scene_rt_was);
    if (sky == 1)
        return;
    if (d->vs.rhw)
        plugin_draw(d, first, nverts, up_data, up_stride);
    if (d->vs.rhw && g_squeeze && (g_dev.rt == g_dev.backbuffer || menu_target(obj(g_dev.rt))))
#else
    if (d->vs.rhw && (g_dev.rt == g_dev.backbuffer || menu_target(obj(g_dev.rt))))
#endif
    {
        /* the game's cursor is drawn where the mouse was given to it: squeezed when that was
         * unsqueezed (over the interface), as it is when it was given as it is (over the world) */
        int cursor = ui_is_cursor(d, first, nverts, up_data, up_stride);
        if (!cursor && ui_is_world_pointer(first, nverts, up_data, up_stride))
            ; /* the target arrow: where the game put it */
        else if (!cursor || !user32_mouse_raw())
            ui_squeeze(d, first, nverts, up_data, up_stride, !cursor); /* the cursor never marks the interface */
        const Obj* t0 = obj(g_dev.cur.tex[0]);
        if (g_dev.rt == g_dev.backbuffer && t0 && t0->kind == O_TEXTURE && (t0->usage & USAGE_RENDERTARGET) &&
            ui_box_shape(t0->width, t0->height))
            g_ui_composited = 1;
        if (g_cap)
            fprintf(g_cap, "  ui vp %.1f %.1f %.1f %.1f clip %u %u %u %u\n", d->u.vp[0], d->u.vp[1], d->u.vp[2], d->u.vp[3],
                d->vp[0], d->vp[1], d->vp[2], d->vp[3]);
    }
    d->prim = prim, d->count = count;
    /* a point fill (/fillmode point): Direct3D 11 and 12 have none, so a triangle draw goes as its vertices, a point list
     * (a list's 3 a triangle, a strip's or a fan's each once, from the draw's own indices or vertex range) */
    if (d->fill == 1 && prim >= GFX_TRIANGLELIST)
        d->prim = GFX_POINTLIST, d->count = prim == GFX_TRIANGLELIST ? count * 3 : count + 2;
    if (chat_glyph(d, prim, count, up_data, up_stride, n))
        d->text_shadow = (uint8_t)g_text_shadow, d->text_alpha = g_text_alpha;
#ifdef VANACORE
    if (chatfont_keep(d))
        return;
#endif
    wc_cut_draw(d, g_chat_drawing); /* [client] window.corners: a window's contents kept inside them (d3d8_corners.h) */
    apply_targets();
#ifdef VANACORE
    if (sky == 2)
        sky_flush(); /* into the target just bound: the world's, before its first draw */
#endif
    chatfont_cover(d);
    glow_watch(d);
    if (!glow_draw(d))
        return;
    gfx_draw(d);
    chatfont_laid(d);
    tips_gradient(d, prim, n, up_data, up_stride); /* [client] tips.rarity.gradient: over an item's details' background */
}

/* --- resources: IDirect3DResource8, textures, buffers, surfaces ------------------------------------------ */
static void Resource_GetDevice(Guest* g)
{
    obj_addref(g_dev.guest);
    wr32(ARG(1), g_dev.guest);
    RET(D3D_OK, 2);
}

static void Resource_SetPriority(Guest* g)
{
    Obj* o = obj(ARG(0));
    uint32_t old = o ? o->priority : 0;
    if (o)
        o->priority = ARG(1);
    RET(old, 2);
}

static void Resource_GetPriority(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->priority : 0, 1);
}

static void Resource_PreLoad(Guest* g) { RET(0, 1); }

static void Resource_GetType(Guest* g)
{
    Obj* o = obj(ARG(0));
    static const uint32_t types[] = { 0, 0, 0, RT_TEXTURE, RT_CUBETEXTURE, RT_VERTEXBUFFER, RT_INDEXBUFFER, RT_SURFACE };
    RET(o ? types[o->kind] : 0, 1);
}

static void Texture_SetLOD(Guest* g)
{
    Obj* o = obj(ARG(0));
    uint32_t old = o ? o->lod : 0;
    if (o && o->pool == 1) /* managed textures only */
        o->lod = ARG(1) < o->levels ? ARG(1) : o->levels - 1;
    RET(old, 2);
}

static void Texture_GetLOD(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->lod : 0, 1);
}

static void Texture_GetLevelCount(Guest* g)
{
    Obj* o = obj(ARG(0));
    RET(o ? o->levels : 0, 1);
}

static void write_surface_desc(uint32_t p, const Obj* s)
{
    wr32(p, s->format);
    wr32(p + 4, RT_SURFACE);
    wr32(p + 8, s->usage);
    wr32(p + 12, s->pool);
    wr32(p + 16, s->size);
    wr32(p + 20, 0); /* MultiSampleType */
    wr32(p + 24, s->width);
    wr32(p + 28, s->height);
}

static Obj* sub(uint32_t tex, uint32_t face, uint32_t level)
{
    const Obj* t = obj(tex);
    if (!t || level >= t->levels || face >= (t->kind == O_CUBE ? 6u : 1u))
        return NULL;
    return obj(t->subs[face * t->levels + level]);
}

static void Texture_GetLevelDesc(Guest* g)
{
    const Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s);
    RET(D3D_OK, 3);
}

static void Texture_GetSurfaceLevel(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    wr32(ARG(2), s ? s->guest : 0);
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    obj_addref(s->guest);
    RET(D3D_OK, 3);
}

#define LOCK_READONLY 0x10u

/* D3DLOCKED_RECT for a surface and an optional RECT. A surface the GPU owns is read back first
 * and written again at unlock; one the game owns is uploaded before its next draw. */
static void lock_rect(Obj* s, uint32_t locked, uint32_t rect, uint32_t flags)
{
    uint32_t pitch = fmt_pitch(s->format, s->width), bits = obj_mem(s);
    if (gpu_owned(s) && surface_scaled(s))
    {
        /* drawn at another size: read at the game's; what the game writes isn't drawn back */
        if (!(s->usage & USAGE_DEPTHSTENCIL))
            read_grown(s, GUEST_PTR(bits), pitch);
    }
    else if (gpu_owned(s))
    {
        uint32_t face, level;
        GfxTex* g = surface_gpu(s, &face, &level);
        if (!s->gpu_locked && !(s->usage & USAGE_DEPTHSTENCIL))
        {
            /* The game's per-frame probe decides what is drawn from these pixels: a copy even a frame
             * old makes characters flicker. So the read waits (the back end commits each frame in
             * chunks, so the wait is the frame's tail); FFXI_ASYNC_READBACK=1 trades that for
             * the newest finished copy. */
            static int async = -1, visible = -1;
            if (async < 0)
                async = getenv("FFXI_ASYNC_READBACK") && getenv("FFXI_ASYNC_READBACK")[0] == '1';
            if (visible < 0)
                visible = !(getenv("FFXI_PROBE") && !strcmp(getenv("FFXI_PROBE"), "gpu"));
            /* The game's 16x16 occlusion probe (FFXiMain 0x1006c8c0: CopyRects of a 16x16 target, a read-only lock,
             * bit 7 of the blue byte counted over 8x8 samples: 0 hidden .. 256 fully visible) reads fully visible,
             * with no wait for the GPU. Reading it for real stalls the CPU on the whole scene every frame (7-8 ms at
             * a 4096x4096 background), and a late answer makes characters flicker; answering visible draws what the
             * probe would have hidden, which the depth test hides anyway. FFXI_PROBE=gpu reads it. Not so the sky
             * probe: the sun's lens flare draws without a depth test, so it is the probe that hides it behind walls.
             * That one reads a frame late, for free; the flare writes no depth, so the late answer does not feed back
             * into the next. */
            int probe = (flags & LOCK_READONLY) && s->width == 16 && s->height == 16, sky = probe && g_probe_sky;
            if (probe)
                g_probe_sky = 0;
            if (visible && sky)
                gfx_tex_read_async(g, face, level, GUEST_PTR(bits), pitch);
            else if (visible && probe)
                memset(GUEST_PTR(bits), 0xFF, (size_t)pitch * s->height);
            else if (async && (flags & LOCK_READONLY) && s->width * s->height <= 128 * 128)
                gfx_tex_read_async(g, face, level, GUEST_PTR(bits), pitch);
            else
                gfx_tex_read(g, face, level, GUEST_PTR(bits), pitch);
        }
        if (!(flags & LOCK_READONLY) && !(s->usage & USAGE_DEPTHSTENCIL))
            s->gpu_locked = 1;
    }
    else if (!(flags & LOCK_READONLY))
        mark_dirty(s);
    if (rect)
    {
        uint32_t l = rd32(rect), t = rd32(rect + 4), blk = fmt_block(s->format);
        bits += blk ? (t / 4) * pitch + (l / 4) * blk : t * pitch + l * fmt_bytes(s->format);
    }
    wr32(locked, pitch);
    wr32(locked + 4, bits);
}

static void unlock_rect(Obj* s)
{
    if (!s->gpu_locked)
        return;
    uint32_t face, level;
    GfxTex* g = surface_gpu(s, &face, &level);
    gfx_tex_upload(g, face, level, GUEST_PTR(s->mem), fmt_pitch(s->format, s->width));
    s->gpu_locked = 0;
}

/* LockRect(Level, pLockedRect, pRect, Flags) */
static void Texture_LockRect(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 5);
    lock_rect(s, ARG(2), ARG(3), ARG(4));
    RET(D3D_OK, 5);
}

static void Texture_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    unlock_rect(s);
    RET(D3D_OK, 2);
}
static void Texture_AddDirtyRect(Guest* g) { RET(D3D_OK, 2); }

static void Cube_GetLevelDesc(Guest* g)
{
    const Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s);
    RET(D3D_OK, 3);
}

/* GetCubeMapSurface(FaceType, Level, ppCubeMapSurface) */
static void Cube_GetCubeMapSurface(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    wr32(ARG(3), s ? s->guest : 0);
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    obj_addref(s->guest);
    RET(D3D_OK, 4);
}

/* LockRect(FaceType, Level, pLockedRect, pRect, Flags) */
static void Cube_LockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (!s)
        RET(D3DERR_INVALIDCALL, 6);
    lock_rect(s, ARG(3), ARG(4), ARG(5));
    RET(D3D_OK, 6);
}

static void Cube_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    unlock_rect(s);
    RET(D3D_OK, 3);
}
static void Cube_AddDirtyRect(Guest* g) { RET(D3D_OK, 3); }

/* Lock(OffsetToLock, SizeToLock, ppbData, Flags) */
static void Buffer_Lock(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b || ARG(1) > b->size)
        RET(D3DERR_INVALIDCALL, 5);
    if (!(ARG(4) & LOCK_READONLY))
        b->gbuf_dirty = 1;
    wr32(ARG(3), obj_mem(b) + ARG(1));
    RET(D3D_OK, 5);
}

static void Buffer_Unlock(Guest* g) { RET(obj(ARG(0)) ? D3D_OK : D3DERR_INVALIDCALL, 1); }

static void VertexBuffer_GetDesc(Guest* g)
{
    const Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t v[6] = { 100 /* D3DFMT_VERTEXDATA */, RT_VERTEXBUFFER, b->usage, b->pool, b->size, b->fvf };
    memcpy(ARGP(1), v, sizeof v);
    RET(D3D_OK, 2);
}

static void IndexBuffer_GetDesc(Guest* g)
{
    const Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t v[5] = { b->format, RT_INDEXBUFFER, b->usage, b->pool, b->size };
    memcpy(ARGP(1), v, sizeof v);
    RET(D3D_OK, 2);
}

static void Surface_GetContainer(Guest* g)
{
    Obj* s = obj(ARG(0));
    uint32_t c = s && s->container ? s->container : g_dev.guest;
    obj_addref(c);
    wr32(ARG(2), c);
    RET(D3D_OK, 3);
}

static void Surface_GetDesc(Guest* g)
{
    const Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    write_surface_desc(ARG(1), s);
    RET(D3D_OK, 2);
}

/* LockRect(pLockedRect, pRect, Flags) */
static void Surface_LockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 4);
    lock_rect(s, ARG(1), ARG(2), ARG(3));
    RET(D3D_OK, 4);
}

static void Surface_UnlockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 1);
    unlock_rect(s);
    RET(D3D_OK, 1);
}

#ifdef VANACORE
/* --- methods the game doesn't call, for VanaCore's plugins and addons ------------------------------------------
 * The game uses the set above; an Ashita plugin handed the device (or D3DX inside one) may call
 * more. These behave as d3d8 does; ProcessVertices, the patches, additional swap chains and
 * software devices fall to native.c's stub (D3DERR_INVALIDCALL, logged once). */
#define D3DERR_NOTFOUND 0x88760866u
#define D3DERR_MOREDATA 0x88760867u

/* a COM method with no arguments but `this` (AddRef 1, Release 2) on a real COM object */
static void com_call0(uint32_t p, int slot)
{
    ((uint32_t(__stdcall*)(uint32_t))(uintptr_t)rd32(rd32(p) + 4u * (uint32_t)slot))(p);
}

/* SetPrivateData(refguid, pData, SizeOfData, Flags): with D3DSPD_IUNKNOWN (1) pData is an IUnknown,
 * referenced while it is held */
typedef struct Private
{
    uint32_t guest;
    uint8_t guid[16];
    uint8_t* data;
    uint32_t size, unk;
    struct Private* next;
} Private;

static Private* g_private;

static Private** private_find(uint32_t guest, uint32_t guid)
{
    Private** at = &g_private;
    while (*at && ((*at)->guest != guest || memcmp((*at)->guid, GUEST_PTR(guid), 16)))
        at = &(*at)->next;
    return at;
}

static void private_drop(Private** at)
{
    Private* p = *at;
    *at = p->next;
    if (p->unk)
        com_call0(p->unk, 2);
    free(p->data);
    free(p);
}

static void private_free_all(uint32_t guest)
{
    for (Private** at = &g_private; *at;)
        if ((*at)->guest == guest)
            private_drop(at);
        else
            at = &(*at)->next;
}

static void Resource_SetPrivateData(Guest* g)
{
    if (!obj(ARG(0)) || !ARG(1))
        RET(D3DERR_INVALIDCALL, 5);
    Private** at = private_find(ARG(0), ARG(1));
    if (*at)
        private_drop(at);
    Private* p = (Private*)calloc(1, sizeof *p);
    p->guest = ARG(0);
    memcpy(p->guid, ARGP(1), 16);
    if (ARG(4) & 1)
    {
        p->unk = ARG(2), p->size = 4;
        if (p->unk)
            com_call0(p->unk, 1);
    }
    else
    {
        p->size = ARG(3);
        p->data = (uint8_t*)malloc(p->size ? p->size : 1);
        if (p->size && ARG(2))
            memcpy(p->data, ARGP(2), p->size);
    }
    p->next = g_private;
    g_private = p;
    RET(D3D_OK, 5);
}

/* GetPrivateData(refguid, pData, pSizeOfData) */
static void Resource_GetPrivateData(Guest* g)
{
    Private* p = *private_find(ARG(0), ARG(1));
    if (!p)
        RET(D3DERR_NOTFOUND, 4);
    uint32_t room = ARG(3) ? rd32(ARG(3)) : 0;
    if (ARG(3))
        wr32(ARG(3), p->size);
    if (!ARG(2))
        RET(D3D_OK, 4);
    if (room < p->size)
        RET(D3DERR_MOREDATA, 4);
    if (p->unk)
    {
        com_call0(p->unk, 1);
        wr32(ARG(2), p->unk);
    }
    else if (p->size)
        memcpy(ARGP(2), p->data, p->size);
    RET(D3D_OK, 4);
}

static void Resource_FreePrivateData(Guest* g)
{
    Private** at = private_find(ARG(0), ARG(1));
    if (!*at)
        RET(D3DERR_NOTFOUND, 2);
    private_drop(at);
    RET(D3D_OK, 2);
}

/* GetFrontBuffer(pDestSurface): the last frame as A8R8G8B8 in a system-memory surface. Windowed, the
 * surface is the desktop's size and the frame lands where the window's client area is on it. */
static void IDirect3DDevice8_GetFrontBuffer(Guest* g)
{
    Obj* d = obj(ARG(1));
    Obj* bb = obj(g_dev.backbuffer);
    if (!d || !bb || !bb->gpu || d->kind != O_SURFACE || d->container || d->format != FMT_A8R8G8B8)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t w = bb->width, h = bb->height, sp = fmt_pitch(bb->format, w), bpp = fmt_bytes(bb->format);
    int r565 = bb->format == FMT_R5G6B5;
    uint8_t* frame = (uint8_t*)malloc((size_t)sp * h);
    if (!frame)
        RET(E_OUTOFMEMORY, 2);
    if (bb->pw)
        read_grown(bb, frame, sp);
    else
        gfx_tex_read(bb->gpu, 0, 0, frame, sp);
    int32_t ox = 0, oy = 0;
    if (g_dev.pp[7] && (d->width != w || d->height != h))
        user32_client_origin(g_dev.hwnd, &ox, &oy);
    uint8_t* dm = GUEST_PTR(obj_mem(d));
    uint32_t dp = fmt_pitch(d->format, d->width);
    for (uint32_t y = 0; y < h; ++y)
    {
        int32_t ty = oy + (int32_t)y;
        if (ty < 0 || ty >= (int32_t)d->height)
            continue;
        for (uint32_t x = 0; x < w; ++x)
        {
            int32_t tx = ox + (int32_t)x;
            if (tx < 0 || tx >= (int32_t)d->width)
                continue;
            const uint8_t* p = frame + (size_t)y * sp + (size_t)x * bpp;
            uint32_t c;
            if (bpp == 4)
                c = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
            else
            {
                uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
                uint32_t r = r565 ? v >> 11 : (v >> 10) & 31, gr = r565 ? (v >> 5) & 63 : (v >> 5) & 31;
                c = ((r * 255 / 31) << 16) | ((r565 ? gr * 255 / 63 : gr * 255 / 31) << 8) | ((v & 31) * 255 / 31);
            }
            c |= 0xFF000000u;
            memcpy(dm + (size_t)ty * dp + (size_t)tx * 4, &c, 4);
        }
    }
    free(frame);
    RET(D3D_OK, 2);
}

/* UpdateTexture(pSourceTexture, pDestinationTexture): each level of the destination from the source
 * level of its size (a system-memory texture into a default-pool one, as a rule) */
static void IDirect3DDevice8_UpdateTexture(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(2));
    if (!s || !d || s->kind != d->kind || (s->kind != O_TEXTURE && s->kind != O_CUBE) || s->format != d->format ||
        s->width < d->width)
        RET(D3DERR_INVALIDCALL, 3);
    uint32_t src = ARG(1), dst = ARG(2), faces = s->kind == O_CUBE ? 6 : 1, levels = d->levels, skip = 0;
    while (skip + 1 < s->levels && (s->width >> skip) > d->width)
        skip++;
    for (uint32_t f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < levels; ++l)
        {
            Obj* ss = sub(src, f, l + skip);
            Obj* ds = sub(dst, f, l);
            if (!ss || !ds || !ss->mem || ss->width != ds->width || ss->height != ds->height)
                continue;
            memcpy(GUEST_PTR(obj_mem(ds)), GUEST_PTR(ss->mem), ds->size < ss->size ? ds->size : ss->size);
            if (gpu_owned(ds))
            {
                uint32_t face, level;
                GfxTex* t = surface_gpu(ds, &face, &level);
                gfx_tex_upload(t, face, level, GUEST_PTR(ds->mem), fmt_pitch(ds->format, ds->width));
            }
            else
                mark_dirty(ds);
        }
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_GetClipPlane(Guest* g)
{
    if (ARG(1) >= 6)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(ARGP(2), g_dev.cur.clip[ARG(1)], 16);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetClipStatus(Guest* g) { RET(D3D_OK, 2); }

static void IDirect3DDevice8_GetClipStatus(Guest* g)
{
    wr32(ARG(1), 0);
    wr32(ARG(1) + 4, 0);
    RET(D3D_OK, 2);
}

static void IDirect3DDevice8_GetInfo(Guest* g) { RET(1 /* S_FALSE: no such information */, 4); }
static void IDirect3DDevice8_SetPaletteEntries(Guest* g) { RET(D3D_OK, 3); }

static void IDirect3DDevice8_GetPaletteEntries(Guest* g)
{
    memset(ARGP(2), 0, 256 * 4);
    RET(D3D_OK, 3);
}

static void IDirect3DDevice8_SetCurrentTexturePalette(Guest* g) { RET(D3D_OK, 2); }

static void IDirect3DDevice8_GetCurrentTexturePalette(Guest* g)
{
    wr32(ARG(1), 0);
    RET(D3D_OK, 2);
}

/* GetGammaRamp(pRamp): SetGammaRamp does nothing, so the ramp is the identity */
static void IDirect3DDevice8_GetGammaRamp(Guest* g)
{
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t i = 0; i < 256; ++i)
            wr16(ARG(1) + 512u * c + 2u * i, (uint16_t)(i * 257u));
    RET(0, 2);
}

static void IDirect3DDevice8_GetPixelShaderConstant(Guest* g)
{
    uint32_t r = ARG(1), n = ARG(3);
    if (r >= NPSC || n > NPSC - r)
        RET(D3DERR_INVALIDCALL, 4);
    memcpy(ARGP(2), g_dev.cur.psc[r], 16 * n);
    RET(D3D_OK, 4);
}

/* the ...Declaration and ...Function getters: (Handle, pData, pSizeOfData), the size alone when pData
 * is NULL */
static void give_tokens(Guest* g, const uint32_t* t, uint32_t n)
{
    uint32_t bytes = 4 * n, room = rd32(ARG(3));
    wr32(ARG(3), bytes);
    if (!ARG(2))
        RET(D3D_OK, 4);
    if (room < bytes)
        RET(D3DERR_MOREDATA, 4);
    if (bytes)
        memcpy(ARGP(2), t, bytes);
    RET(D3D_OK, 4);
}

static void IDirect3DDevice8_GetVertexShaderDeclaration(Guest* g)
{
    Shader* s = vs_of(ARG(1));
    if (!s || !ARG(3))
        RET(D3DERR_INVALIDCALL, 4);
    give_tokens(g, s->decl, s->ndecl);
}

static void IDirect3DDevice8_GetVertexShaderFunction(Guest* g)
{
    Shader* s = vs_of(ARG(1));
    if (!s || !ARG(3))
        RET(D3DERR_INVALIDCALL, 4);
    give_tokens(g, s->func, s->func ? s->nfunc : 0);
}

static void IDirect3DDevice8_GetPixelShaderFunction(Guest* g)
{
    uint32_t i = ARG(1) - 1;
    if (!ARG(1) || i >= g_dev.nps || !g_dev.ps[i].func || !ARG(3))
        RET(D3DERR_INVALIDCALL, 4);
    give_tokens(g, g_dev.ps[i].func, g_dev.ps[i].nfunc);
}

/* Direct3DCreate8 for a native process: the one IDirect3D8, referenced again */
uint32_t d3d8_create(void)
{
    if (!g_d3d)
        g_d3d = obj_new(O_D3D);
    else
        obj_addref(g_d3d);
    return g_d3d;
}
#endif

/* --- entry point and registration ---------------------------------------------------------------------------- */
/* Direct3DCreate8(SDKVersion) */
static void sh_Direct3DCreate8(Guest* g)
{
    if (!g_d3d)
        g_d3d = obj_new(O_D3D);
    else
        obj_addref(g_d3d);
    RET(g_d3d, 1);
}

/* DebugSetMute(BOOL): D3DX silences the debug runtime; cdecl (the caller pops its argument) */
static void sh_DebugSetMute(Guest* g) { RETC(0); }

/* ValidateVertexShader(pShader, pDeclaration, pCaps, ReturnErrors, ppErrors) and ValidatePixelShader(pShader, pCaps,
 * ReturnErrors, ppErrors), stdcall: what D3DX's shader assembler asks of d3d8.dll after assembling (fetched by
 * GetProcAddress, 0x102ed02c in build 2026-09-03). As Wine: the version token decides (vs.1.0/1.1, ps.1.0-1.4), and
 * there are never error strings. The game's shaders are vs.1.1/ps.1.1, which pass. */
#define E_FAIL 0x80004005u
static void sh_ValidateVertexShader(Guest* g)
{
    uint32_t v = ARG(0) ? rd32(ARG(0)) : 0;
    if (ARG(4))
        wr32(ARG(4), 0);
    RET(v == 0xFFFE0100u || v == 0xFFFE0101u ? D3D_OK : E_FAIL, 5);
}

static void sh_ValidatePixelShader(Guest* g)
{
    uint32_t v = ARG(0) ? rd32(ARG(0)) : 0;
    if (ARG(3))
        wr32(ARG(3), 0);
    RET(v >= 0xFFFF0100u && v <= 0xFFFF0104u ? D3D_OK : E_FAIL, 4);
}

#define D(i, m) { "d3d8.dll", #i "::" #m, i##_##m }
#define U(i, m, f) { "d3d8.dll", #i "::" #m, f }
#define UNKNOWN(i) U(i, QueryInterface, Unknown_QueryInterface), U(i, AddRef, Unknown_AddRef), U(i, Release, Unknown_Release)
#define RESOURCE(i)                                                                                                   \
    UNKNOWN(i), U(i, GetDevice, Resource_GetDevice), U(i, SetPriority, Resource_SetPriority),                         \
        U(i, GetPriority, Resource_GetPriority), U(i, PreLoad, Resource_PreLoad), U(i, GetType, Resource_GetType)

static const ShimDef D3D8[] = {
    { "d3d8.dll", "Direct3DCreate8", sh_Direct3DCreate8 },
    { "d3d8.dll", "DebugSetMute", sh_DebugSetMute },
    { "d3d8.dll", "ValidateVertexShader", sh_ValidateVertexShader },
    { "d3d8.dll", "ValidatePixelShader", sh_ValidatePixelShader },
    UNKNOWN(IDirect3D8),
    D(IDirect3D8, GetAdapterCount),
    D(IDirect3D8, GetAdapterIdentifier),
    D(IDirect3D8, GetAdapterModeCount),
    D(IDirect3D8, EnumAdapterModes),
    D(IDirect3D8, GetAdapterDisplayMode),
    D(IDirect3D8, CheckDeviceType),
    D(IDirect3D8, CheckDeviceFormat),
    D(IDirect3D8, CheckDeviceMultiSampleType),
    D(IDirect3D8, CheckDepthStencilMatch),
    D(IDirect3D8, GetDeviceCaps),
    D(IDirect3D8, GetAdapterMonitor),
    D(IDirect3D8, CreateDevice),

    UNKNOWN(IDirect3DDevice8),
    D(IDirect3DDevice8, TestCooperativeLevel),
    D(IDirect3DDevice8, GetAvailableTextureMem),
    D(IDirect3DDevice8, ResourceManagerDiscardBytes),
    D(IDirect3DDevice8, GetDirect3D),
    D(IDirect3DDevice8, GetDeviceCaps),
    D(IDirect3DDevice8, GetDisplayMode),
    D(IDirect3DDevice8, GetCreationParameters),
    D(IDirect3DDevice8, SetCursorProperties),
    D(IDirect3DDevice8, SetCursorPosition),
    D(IDirect3DDevice8, ShowCursor),
    D(IDirect3DDevice8, Reset),
    D(IDirect3DDevice8, Present),
    D(IDirect3DDevice8, GetBackBuffer),
    D(IDirect3DDevice8, GetRasterStatus),
    D(IDirect3DDevice8, SetGammaRamp),
    D(IDirect3DDevice8, CreateTexture),
    D(IDirect3DDevice8, CreateVolumeTexture),
    D(IDirect3DDevice8, CreateCubeTexture),
    D(IDirect3DDevice8, CreateVertexBuffer),
    D(IDirect3DDevice8, CreateIndexBuffer),
    D(IDirect3DDevice8, CreateRenderTarget),
    D(IDirect3DDevice8, CreateDepthStencilSurface),
    D(IDirect3DDevice8, CreateImageSurface),
    D(IDirect3DDevice8, CopyRects),
    D(IDirect3DDevice8, SetRenderTarget),
    D(IDirect3DDevice8, GetRenderTarget),
    D(IDirect3DDevice8, GetDepthStencilSurface),
    D(IDirect3DDevice8, BeginScene),
    D(IDirect3DDevice8, EndScene),
    D(IDirect3DDevice8, Clear),
    D(IDirect3DDevice8, SetTransform),
    D(IDirect3DDevice8, GetTransform),
    D(IDirect3DDevice8, MultiplyTransform),
    D(IDirect3DDevice8, SetViewport),
    D(IDirect3DDevice8, GetViewport),
    D(IDirect3DDevice8, SetMaterial),
    D(IDirect3DDevice8, GetMaterial),
    D(IDirect3DDevice8, SetLight),
    D(IDirect3DDevice8, GetLight),
    D(IDirect3DDevice8, LightEnable),
    D(IDirect3DDevice8, GetLightEnable),
    D(IDirect3DDevice8, SetClipPlane),
    D(IDirect3DDevice8, SetRenderState),
    D(IDirect3DDevice8, GetRenderState),
    D(IDirect3DDevice8, BeginStateBlock),
    D(IDirect3DDevice8, EndStateBlock),
    D(IDirect3DDevice8, ApplyStateBlock),
    D(IDirect3DDevice8, CaptureStateBlock),
    D(IDirect3DDevice8, DeleteStateBlock),
    D(IDirect3DDevice8, CreateStateBlock),
    D(IDirect3DDevice8, GetTexture),
    D(IDirect3DDevice8, SetTexture),
    D(IDirect3DDevice8, GetTextureStageState),
    D(IDirect3DDevice8, SetTextureStageState),
    D(IDirect3DDevice8, ValidateDevice),
    D(IDirect3DDevice8, DrawPrimitive),
    D(IDirect3DDevice8, DrawIndexedPrimitive),
    D(IDirect3DDevice8, DrawPrimitiveUP),
    D(IDirect3DDevice8, DrawIndexedPrimitiveUP),
    D(IDirect3DDevice8, CreateVertexShader),
    D(IDirect3DDevice8, SetVertexShader),
    D(IDirect3DDevice8, GetVertexShader),
    D(IDirect3DDevice8, DeleteVertexShader),
    D(IDirect3DDevice8, SetVertexShaderConstant),
    D(IDirect3DDevice8, GetVertexShaderConstant),
    D(IDirect3DDevice8, SetStreamSource),
    D(IDirect3DDevice8, GetStreamSource),
    D(IDirect3DDevice8, SetIndices),
    D(IDirect3DDevice8, GetIndices),
    D(IDirect3DDevice8, CreatePixelShader),
    D(IDirect3DDevice8, SetPixelShader),
    D(IDirect3DDevice8, GetPixelShader),
    D(IDirect3DDevice8, DeletePixelShader),
    D(IDirect3DDevice8, SetPixelShaderConstant),

    RESOURCE(IDirect3DTexture8),
    U(IDirect3DTexture8, SetLOD, Texture_SetLOD),
    U(IDirect3DTexture8, GetLOD, Texture_GetLOD),
    U(IDirect3DTexture8, GetLevelCount, Texture_GetLevelCount),
    U(IDirect3DTexture8, GetLevelDesc, Texture_GetLevelDesc),
    U(IDirect3DTexture8, GetSurfaceLevel, Texture_GetSurfaceLevel),
    U(IDirect3DTexture8, LockRect, Texture_LockRect),
    U(IDirect3DTexture8, UnlockRect, Texture_UnlockRect),
    U(IDirect3DTexture8, AddDirtyRect, Texture_AddDirtyRect),

    RESOURCE(IDirect3DCubeTexture8),
    U(IDirect3DCubeTexture8, SetLOD, Texture_SetLOD),
    U(IDirect3DCubeTexture8, GetLOD, Texture_GetLOD),
    U(IDirect3DCubeTexture8, GetLevelCount, Texture_GetLevelCount),
    U(IDirect3DCubeTexture8, GetLevelDesc, Cube_GetLevelDesc),
    U(IDirect3DCubeTexture8, GetCubeMapSurface, Cube_GetCubeMapSurface),
    U(IDirect3DCubeTexture8, LockRect, Cube_LockRect),
    U(IDirect3DCubeTexture8, UnlockRect, Cube_UnlockRect),
    U(IDirect3DCubeTexture8, AddDirtyRect, Cube_AddDirtyRect),

    RESOURCE(IDirect3DVertexBuffer8),
    U(IDirect3DVertexBuffer8, Lock, Buffer_Lock),
    U(IDirect3DVertexBuffer8, Unlock, Buffer_Unlock),
    U(IDirect3DVertexBuffer8, GetDesc, VertexBuffer_GetDesc),

    RESOURCE(IDirect3DIndexBuffer8),
    U(IDirect3DIndexBuffer8, Lock, Buffer_Lock),
    U(IDirect3DIndexBuffer8, Unlock, Buffer_Unlock),
    U(IDirect3DIndexBuffer8, GetDesc, IndexBuffer_GetDesc),

    UNKNOWN(IDirect3DSurface8),
    U(IDirect3DSurface8, GetDevice, Resource_GetDevice),
    U(IDirect3DSurface8, GetContainer, Surface_GetContainer),
    U(IDirect3DSurface8, GetDesc, Surface_GetDesc),
    U(IDirect3DSurface8, LockRect, Surface_LockRect),
    U(IDirect3DSurface8, UnlockRect, Surface_UnlockRect),
#ifdef VANACORE
    D(IDirect3DDevice8, GetFrontBuffer),
    D(IDirect3DDevice8, UpdateTexture),
    D(IDirect3DDevice8, GetClipPlane),
    D(IDirect3DDevice8, SetClipStatus),
    D(IDirect3DDevice8, GetClipStatus),
    D(IDirect3DDevice8, GetInfo),
    D(IDirect3DDevice8, SetPaletteEntries),
    D(IDirect3DDevice8, GetPaletteEntries),
    D(IDirect3DDevice8, SetCurrentTexturePalette),
    D(IDirect3DDevice8, GetCurrentTexturePalette),
    D(IDirect3DDevice8, GetGammaRamp),
    D(IDirect3DDevice8, GetPixelShaderConstant),
    D(IDirect3DDevice8, GetVertexShaderDeclaration),
    D(IDirect3DDevice8, GetVertexShaderFunction),
    D(IDirect3DDevice8, GetPixelShaderFunction),
#define PRIVATE(i) U(i, SetPrivateData, Resource_SetPrivateData), U(i, GetPrivateData, Resource_GetPrivateData), \
    U(i, FreePrivateData, Resource_FreePrivateData)
    PRIVATE(IDirect3DTexture8),
    PRIVATE(IDirect3DCubeTexture8),
    PRIVATE(IDirect3DVertexBuffer8),
    PRIVATE(IDirect3DIndexBuffer8),
    PRIVATE(IDirect3DSurface8),
#endif
    { NULL, NULL, NULL },
};

/* vtable order: the D3D8 ABI (d3d8proxy.cpp's tables) */
#define RESOURCE8_NAMES "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData", \
    "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType"
static const char* const kD3D8[] = { "QueryInterface", "AddRef", "Release", "RegisterSoftwareDevice", "GetAdapterCount",
    "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType",
    "CheckDeviceFormat", "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "GetDeviceCaps", "GetAdapterMonitor",
    "CreateDevice", NULL };
static const char* const kDevice[] = { "QueryInterface", "AddRef", "Release", "TestCooperativeLevel",
    "GetAvailableTextureMem", "ResourceManagerDiscardBytes", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode",
    "GetCreationParameters", "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain",
    "Reset", "Present", "GetBackBuffer", "GetRasterStatus", "SetGammaRamp", "GetGammaRamp", "CreateTexture",
    "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
    "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "UpdateTexture", "GetFrontBuffer",
    "SetRenderTarget", "GetRenderTarget", "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform",
    "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight",
    "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState",
    "BeginStateBlock", "EndStateBlock", "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock",
    "CreateStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture", "SetTexture", "GetTextureStageState",
    "SetTextureStageState", "ValidateDevice", "GetInfo", "SetPaletteEntries", "GetPaletteEntries",
    "SetCurrentTexturePalette", "GetCurrentTexturePalette", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant", "GetVertexShaderConstant",
    "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource", "SetIndices",
    "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader", "DeletePixelShader",
    "SetPixelShaderConstant", "GetPixelShaderConstant", "GetPixelShaderFunction", "DrawRectPatch", "DrawTriPatch",
    "DeletePatch", NULL };
static const char* const kTexture[] = { RESOURCE8_NAMES, "SetLOD", "GetLOD", "GetLevelCount", "GetLevelDesc",
    "GetSurfaceLevel", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kCube[] = { RESOURCE8_NAMES, "SetLOD", "GetLOD", "GetLevelCount", "GetLevelDesc",
    "GetCubeMapSurface", "LockRect", "UnlockRect", "AddDirtyRect", NULL };
static const char* const kVB[] = { RESOURCE8_NAMES, "Lock", "Unlock", "GetDesc", NULL };
static const char* const kIB[] = { RESOURCE8_NAMES, "Lock", "Unlock", "GetDesc", NULL };
static const char* const kSurface[] = { "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "GetContainer", "GetDesc", "LockRect", "UnlockRect", NULL };

static uint32_t make_vtbl(const char* iface, const char* const* names)
{
    uint32_t n = 0;
    while (names[n])
        n++;
    uint32_t v = gheap_alloc(4 * n, 1);
    char full[96];
    for (uint32_t i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::%s", iface, names[i]);
        wr32(v + 4 * i, thunk_for("d3d8.dll", full));
    }
    return v;
}

void d3d8_init(void)
{
    thunk_register(D3D8);
}

/* the vtables are guest memory: built once the guest heap is up (from the host, before GameStart) */
void d3d8_setup(void)
{
    g_vtbl[O_D3D] = make_vtbl("IDirect3D8", kD3D8);
    g_vtbl[O_DEVICE] = make_vtbl("IDirect3DDevice8", kDevice);
    g_vtbl[O_TEXTURE] = make_vtbl("IDirect3DTexture8", kTexture);
    g_vtbl[O_CUBE] = make_vtbl("IDirect3DCubeTexture8", kCube);
    g_vtbl[O_VB] = make_vtbl("IDirect3DVertexBuffer8", kVB);
    g_vtbl[O_IB] = make_vtbl("IDirect3DIndexBuffer8", kIB);
    g_vtbl[O_SURFACE] = make_vtbl("IDirect3DSurface8", kSurface);
}
