/* The graphics back end under the D3D8 front end (d3d8.c): Metal on macOS (gfx_metal.m), nothing
 * elsewhere yet (gfx_null.c).
 *
 * d3d8.c keeps every D3D8 semantic - objects, state, state blocks, what a lock returns - and hands
 * this layer resolved work: textures to create and fill, the render targets, clears, and draw
 * packets. A draw packet is the fixed-function (or shader) state reduced to a key that selects
 * the generated shaders and pipeline (GfxVsKey, GfxFsKey, GfxPipeKey), the values those shaders
 * read (GfxU), and the vertex and index bytes the draw touches.
 *
 * Formats are D3DFORMAT values; the back end maps them. Matrices are D3D's: row vectors, row-major
 * storage, v * M. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct GfxTex GfxTex;
typedef struct GfxBuf GfxBuf;
typedef struct GfxInst GfxInst;

enum
{
    GFX_TEX_2D,
    GFX_TEX_CUBE,
};

enum
{
    GFX_USE_SAMPLE,
    GFX_USE_RT,    /* a render target that can also be sampled */
    GFX_USE_DEPTH, /* a depth-stencil surface */
};

/* D3DPRIMITIVETYPE */
enum
{
    GFX_POINTLIST = 1,
    GFX_LINELIST,
    GFX_LINESTRIP,
    GFX_TRIANGLELIST,
    GFX_TRIANGLESTRIP,
    GFX_TRIANGLEFAN,
};

/* vertex element types: D3DVSDT */
enum
{
    GFX_FLOAT1,
    GFX_FLOAT2,
    GFX_FLOAT3,
    GFX_FLOAT4,
    GFX_D3DCOLOR,
    GFX_UBYTE4,
    GFX_SHORT2,
    GFX_SHORT4,
};

/* the fixed-function input registers (D3DVSDE), also the v# of a vertex shader */
enum
{
    GFX_R_POSITION,
    GFX_R_BLENDWEIGHT,
    GFX_R_BLENDINDICES,
    GFX_R_NORMAL,
    GFX_R_PSIZE,
    GFX_R_DIFFUSE,
    GFX_R_SPECULAR,
    GFX_R_TEXCOORD0, /* .. 14 */
    GFX_R_POSITION2 = 15,
    GFX_R_NORMAL2,
    GFX_NREGS,
};

#define GFX_NSTREAMS 4
#define GFX_NLIGHTS 8
#define GFX_NVSC 96
#define GFX_NPSC 8

typedef struct GfxElem
{
    uint8_t used, stream, type, pad;
} GfxElem;

/* What selects a vertex function. Zeroed, then filled: the bytes are hashed and compared. */
typedef struct GfxVsKey
{
    uint32_t prog;     /* 0: fixed function; else a vertex shader (GfxDraw.vs_tokens), this is its hash */
    GfxElem el[GFX_NREGS];
    uint8_t rhw;       /* XYZRHW positions: already transformed */
    uint8_t lighting, normalize, localviewer, specular;
    uint8_t src_diffuse, src_specular, src_ambient, src_emissive; /* D3DMCS: 0 material, 1 color1, 2 color2 */
    uint8_t nlights, light_type[GFX_NLIGHTS];                    /* D3DLIGHTTYPE of each enabled light */
    uint8_t fog_vertex; /* D3DFOGMODE computed per vertex (FOGVERTEXMODE with no table fog), 0 none */
    uint8_t range_fog;
    uint8_t ntex;       /* texture coordinate outputs */
    uint8_t tci[8];     /* D3DTSS_TEXCOORDINDEX: index | generation mode << 4 */
    uint8_t ttf[8];     /* D3DTSS_TEXTURETRANSFORMFLAGS: count | 0x80 projected */
    uint8_t flat;
    uint8_t pixel;      /* the lighting per pixel rather than per vertex (the back end sets it: the
                         * scene effects' light setting); the vertex function passes the normal on */
    uint8_t shadow;     /* drawn again from the sun (the back end's shadow map): the position the
                         * function makes goes through the matrix in buffer 5; 2, fading out of the map
                         * (only a share of its texels kept, the share in the same buffer: sun_f); 3
                         * (GFX_VS_MIRROR), drawn again for the water's mirror (gfx_scene_mirror): its position
                         * through that matrix, its colour its own */
    uint8_t centered;   /* XYZRHW drawn with today's pixel centres, not D3D8's: a render target copied at another
                         * size, which D3D8's moved half a texel (d3d8.c build_draw) */
    uint8_t cpos;       /* a vertex shader also passes its clip-space position on (VOut.cp): a pixel shader's normal map
                         * works out where each pixel is from it (gfx_hlsl.c emit_normal_relight); 2, and its normal input's
                         * (v3) clip-space direction (VOut.cn: the program run again with the position a unit along it), for
                         * the zone's smooth normals (gfx_d3d12.c vn_replay) */
    uint8_t motion;     /* VanaCore's per-draw motion for DLSS (the back end's: gfx_d3d12.c mo_draw), 0 none: the draw again,
                         * its pixels' motion written in place of their colour (GFX_MO_*) */
    uint8_t inst;       /* drawn once for each of the zone's pieces that share it (the back end's: gfx_d3d12_batch.h), 0 not: each
                         * piece's transforms (GfxInst) in stream inst - 1, one the draw doesn't read, by SV_InstanceID. In the
                         * byte the struct had spare (its size is the same) */
} GfxVsKey;

#define GFX_VS_MIRROR 3 /* GfxVsKey.shadow: the water's mirror */

/* GfxVsKey.motion: where the position the frame before comes from - the low bits: GFX_MO_SAME (the vertices are the
 * frame before's, only the matrix moved) or 1 + the stream (t0..t3) the frame before's positions were put in, float3
 * each in the order of this frame's (vertex i at i * 12) - and which of its pixels count, by the colour it would draw:
 * all (an opaque draw), those with alpha (blended over), or those that add light (blended onto) */
#define GFX_MO_SAME 7
#define GFX_MO_SRC 7
#define GFX_MO_ALPHA 8
#define GFX_MO_ADD 16
/* ... or, with this bit, the plants' mask for AMD's FSR (gfx_d3d12.c mask_draw): the draw again, past its alpha test, writes 1 and
 * its view z into the mask target, no motion (any draw's, a vertex shader's too) */
#define GFX_MO_MASK 32

typedef struct GfxStage
{
    uint8_t cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, result;
    uint8_t tex;       /* 0 none bound, 1 2D, 2 cube */
    uint8_t projected; /* divide the coordinates by their last component */
    uint8_t ncoord;    /* components the vertex function writes for this stage: 2, 3 or 4 */
} GfxStage;

typedef struct GfxFsKey
{
    uint32_t prog; /* 0: the texture stages; else a pixel shader (GfxDraw.ps_tokens), this is its hash */
    GfxStage st[8];
    uint8_t nstages;
    uint8_t alpha_func; /* D3DCMPFUNC, 0 or 8 when the test is off */
    uint8_t fog;        /* D3DFOGMODE: pixel (table) fog; 4 = the vertex function's fog factor */
    uint8_t specular_add;
    uint8_t flat;
    uint8_t nmap; /* VanaCore's normal maps: stage 7's texture is the normal map of stage 0's (BC5, X and Y), the lit
                   * pixel function bends the surface normal by it: 1 as is, 2 shown as the colour, 3 with its Y turned over;
                   * the back end's: 4 and 5 its detail pass, 6 the smooth-normal pass (gfx_d3d12.c vn_replay): the draw's
                   * interpolated normal out, no map */
    uint8_t water; /* a zone's water tile drawn as VanaCore's water (gfx_hlsl_water.h) instead of the stage cascade: the tile's
                    * world position and the time ride in texture matrix 7, the settings after them and in matrix 6 (d3d8_water.h
                    * water_shade). 1 in its place in the world, opaque; 2 the water pass (water_pass): drawn after the world with
                    * copies of the scene's colour and depth in stages 6 and 7, read for the depth under the surface and what
                    * shows through it */
    uint8_t text_shadow; /* a text shadow (gfx_dispatch.c text_flush): black, its alpha the ink of the texture's glyph (the
                          * less of its alpha and its brightest channel) times u.tfactor.a, in place of the stages' colour */
    uint8_t pool; /* a world draw after the effects ran (d3d8_casters.h late_pool): the player light's pool, which the composite laid
                   * over the draws before it, laid over this one's own colour (gfx_hlsl.c emit_pool), from texture matrix 5 */
    uint8_t hfog; /* a world draw after the effects ran (d3d8_casters.h late_pool): the height fog, which the composite laid over
                   * the draws before it, laid over this one's own colour (gfx_hlsl.c emit_hfog), from texture matrix 4: 1 toward the
                   * fog's colour (opaque, or SRCALPHA onto INVSRCALPHA), 2 its colour taken off (adding onto ONE), 3 toward the fog's
                   * colour times its alpha (ONE onto INVSRCALPHA, premultiplied) */
    uint8_t wet;  /* a character's draw in the rain (d3d8_rain.h rain_character): the lit pixel function lays the wet look over its
                   * finished colour (gfx_hlsl_rain.h emit_wet) from texture matrices 6 (the wetness, the time, the strengths, the
                   * world's up) and 7 (the view's inverse) */
    uint8_t rain; /* VanaCore's rain pass (d3d8_rain.h rain_pass): a screen-space draw after the world over copies of the scene's
                   * colour and depth (stages 6 and 7, as the water pass's) and the shelter map (stage 5): the streaks, the splashes,
                   * the wet ground (gfx_hlsl_rain.h emit_rain_fs) */
    uint8_t pad[4]; /* (the Direct3D 12 back end's LibKey, this and GfxVsKey, is read in pairs of words: gfx_sun_cache.h lib_hash) */
} GfxFsKey;

/* The render pipeline beyond the functions: blending and the color write mask. */
typedef struct GfxPipeKey
{
    uint8_t blend, src, dst, op; /* D3DBLEND, D3DBLENDOP */
    uint8_t write_mask;          /* D3DCOLORWRITEENABLE */
    uint8_t over;                /* into VanaCore's interface layer: alpha as coverage (ONE, INVSRCALPHA), so
                                  * the layer comes out premultiplied for gfx_present */
    uint8_t pad[2];
} GfxPipeKey;

typedef struct GfxDepthKey
{
    uint8_t zenable, zwrite, zfunc, stencil;
    uint8_t sfail, szfail, spass, sfunc;
    uint8_t sread, swrite;
    uint8_t noclip; /* not cut at the near and far planes, its depth held to the viewport's range instead (DepthClipEnable off):
                     * VanaCore's sea on to the horizon past the game's draw distance (d3d8_water_sea.h) */
    uint8_t pad[1];
} GfxDepthKey;

typedef struct GfxLight
{
    float diffuse[4], specular[4], ambient[4];
    float pos[4];  /* camera space; w = range */
    float dir[4];  /* camera space, toward the light for directional lights; w = falloff */
    float att[4];  /* attenuation 0, 1, 2 */
    float spot[4]; /* cos(theta / 2), cos(phi / 2) */
} GfxLight;

/* The values the generated functions read: one buffer for both stages. Layout is mirrored in
 * gfx_msl.c's MSL struct; keep them together. */
typedef struct GfxU
{
    float wvp[16], wv[16], wvit[16];
    float texm[8][16];
    float mat_d[4], mat_a[4], mat_s[4], mat_e[4];
    float params[4];  /* material power, alpha ref (0..255), fog start, fog end */
    float ambient[4]; /* D3DRS_AMBIENT */
    float tfactor[4];
    float fogcolor[4];
    float params2[4]; /* fog density */
    float vp[4];      /* viewport x, y, width, height: the XYZRHW mapping */
    int32_t vofs, pad0[3];
    int32_t stride[GFX_NSTREAMS];
    int32_t offset[20]; /* per input register */
    GfxLight light[GFX_NLIGHTS];
    float vsc[GFX_NVSC][4];
    float psc[GFX_NPSC][4];
} GfxU;

typedef struct GfxSampler
{
    uint8_t addr_u, addr_v, addr_w; /* D3DTEXTUREADDRESS */
    uint8_t mag, min, mip;          /* D3DTEXTUREFILTERTYPE */
    uint8_t max_aniso, max_level;
    uint8_t lod_cap; /* 0, or the coarsest mip sampled + 1 (texture packs: glyph sheets whose small mips blend
                      * neighbouring glyphs into each one's edges) */
    int8_t lod_bias; /* the mip chosen moved by this many sixteenths (the back end's: DLSS's, below 0) */
    uint8_t pad[2];
    uint32_t border;
} GfxSampler;

typedef struct GfxDraw
{
    GfxVsKey vs;
    GfxFsKey fs;
    GfxPipeKey pipe;
    GfxDepthKey depth;
    const uint32_t* vs_tokens; /* the shader behind vs.prog / fs.prog */
    const uint32_t* ps_tokens;
    GfxU u;
    GfxTex* tex[8];
    GfxSampler samp[8];
    uint8_t cull;   /* D3DCULL */
    uint8_t caster; /* an opaque draw of the frame's 3D scene: it casts the sun's shadow */
    uint8_t fill;   /* D3DFILLMODE */
    uint8_t overlay; /* the interface drawn into the world's target after its effects (names, floating text): with
                      * DLSS, drawn over its output at the output's size, not upscaled with the world */
    uint8_t water;  /* the game's water: its vertices are a water model's, made while the game loaded it (d3d8.c water_model);
                     * 1 a flat one, 2 one that isn't (a waterfall, a river running downhill) */
    uint8_t small_rt; /* its target is under 512 on a side as the game made it, or a glow target ([graphics] glow draws those
                       * larger): the scene filter leaves it alone (scene_mips), whatever size it's drawn at */
    uint8_t screen_fx; /* 3D fixed to the screen, not placed in the world (the Records of Eminence's "New Challenge" banner):
                        * the light from the effects leaves it out (d3d8.c screen_effect) */
    uint8_t el_lantern; /* a lantern's or torch's flame ([graphics.effects] elight_lantern_tex: its texture's hash): the light from
                         * the effects takes it at elight_lantern's share (gfx_d3d12.c el_emit); 2 an effect that gives far
                         * more light than it looks to (elight_dim_tex), at elight_dim's; 3 an effect on a character (a
                         * spell, an ability: d3d8.c character_effect), at elight_actor's */
    uint8_t dlss5_skip; /* an additive draw DLSS 5 isn't to take as a light ([graphics.effects] dlss5_skip_tex: its texture's
                         * hash; Al'Taieu's portal pools): left out of its glow (gfx_d3d12.c el_emit, fx_dlssg) */
    uint8_t el_early; /* an additive effect's own model near the camera (a zone's door glow, made once, round the camera) drawn
                       * before the world's scene ended: the light from the effects takes it too (gfx_d3d12.c el_emit) */
    uint8_t rain_fx; /* rain: VanaCore's drops and splashes (d3d8_rain.h particles_draw), the game's own rain (its texture,
                      * rain_game_tex): left out of the reflections' effects while [graphics.effects] refl_no_rain (gfx_d3d12_rtfx.h) */
    uint8_t is_char; /* a character's draw (players, NPCs, monsters and what they wear: d3d8.c is_character): the back end marks its
                      * pixels in the stencil while ao_chars needs them told from the world (gfx_d3d12.c cmask_usable) */
    uint8_t sought; /* a piece of the zone round the player, drawn by the game for the sun's maps alone whatever the camera sees
                     * (d3d8.c seek_note, VanaCore's caster pass): kept as a caster (gfx_d3d12.c caster_only), never drawn into
                     * the scene; 2 in world space (u.wvp its world matrix alone: a fixed-function draw), 1 through the camera */
    uint16_t piece_draw; /* ... its place among its piece's draws, and the piece (the game's own: its address; vanacore_gfx_seek),
                          * 0 not the pass's: the sun's cache keeps each by these, once (gfx_sun_cache.h piece_see) */
    uint32_t piece;
    int32_t zbias;  /* D3DRS_ZBIAS */
    uint32_t stencil_ref;
    uint32_t label; /* an overlay draw of a character's name (vanacore_gfx_label): the character, 0 none; frame generation
                     * finds the name again by it the next frame (gfx_d3d12.c fgn_*) */
    uint32_t vp[6]; /* D3DVIEWPORT8 (MinZ, MaxZ as float bits) */
    /* vertices: stream s holds vertex (first + i) at data[s] + i * stride - copied per draw - or,
     * for a static buffer, at buf_off[s] + i * stride in buf[s] */
    const void* data[GFX_NSTREAMS];
    GfxBuf* buf[GFX_NSTREAMS];
    uint32_t buf_off[GFX_NSTREAMS];
    uint32_t size[GFX_NSTREAMS];
    /* the primitive: indices (16 or 32 bit) or vertices from vertex_start */
    uint32_t prim, count; /* D3D primitive type and primitive count */
    const void* indices;
    uint32_t index_size;
    GfxBuf* ibuf;      /* the indices are also in this static buffer, at ibuf_off (4-byte aligned) */
    uint32_t ibuf_off;
    uint32_t vertex_start;
    /* the scene's casters (caster), for the sun's shadow cache: each stream's vertices as the CPU has
     * them, a static buffer's too (vertex `first` on, cpu_size[s] bytes), and where the object stands:
     * its world matrix's origin, anchor[3] = 1 (0 when its vertices are in the world already - characters,
     * made again each frame). A copy of an object the cache can't draw again from the game's own buffers
     * (a tree the game rewrites as it sways) is kept, to cast its shadow while it's out of view. */
    const void* cpu[GFX_NSTREAMS];
    uint32_t cpu_size[GFX_NSTREAMS];
    float anchor[4];
    /* a glyph of the chat's text with [client] chat.shadow on (d3d8.c chat_glyph): 1 a shadow under it, a pixel down and to
     * the right, 2 an outline round it; drawn first, darkening what's behind by text_alpha (0-255) where the glyph's ink is */
    uint8_t text_shadow, text_alpha;
    /* the same draw for ninst of the zone's pieces at once (vs.inst; the back end's batch, gfx_d3d12_batch.h): each one's
     * transforms and anchor (inst[i]); u holds the first's. 0 a draw of its own */
    uint32_t ninst;
    const GfxInst* inst;
} GfxDraw;

/* Texture Filtering ([graphics.effects] aniso: 0 the game's own, 1 trilinear, 2 to 16 anisotropic), laid over what the game
 * asked for on the world's solid textures (not the interface's, not the sky's or effects' - which write no depth: the sky's
 * clouds, mapped at steep angles near the horizon, flicker through an anisotropic filter as the camera turns), whatever filter
 * it asked for; down a texture's chain only as far as it has been filled without a gap, so a level never uploaded isn't sampled
 * (2026-11-27: it took only textures the game asked a linear filter for with every level filled). Texture Sharpness (tex_sharp,
 * 0 to 10): the mips chosen up to 2 levels finer. Stage i of draw d; the scene's own target is left to the back end's scene
 * filter. PROBE (2026-11-27, to come out): which of these the ground fails, five lines in the log. */
void rt_log(const char* fmt, ...);
static inline void gfx_world_filter(GfxSampler* sk, const GfxDraw* d, int i, int scene, uint32_t levels, uint32_t filled,
                                    float aniso, float sharp)
{
    static uint32_t n[7], said; /* (PROBE) binds: not depth-writing, projected, scene, one level, gap at level 1, taken, all */
    if (d->vs.rhw)
        return;
    const int why = !d->depth.zwrite ? 0 : d->fs.st[i].projected ? 1 : scene ? 2 : levels < 2 ? 3 : !(filled & 2u) ? 4 : 5;
    ++n[why];
    if (++n[6] % 200000 == 0 && said++ < 5)
        rt_log("[recomp] Texture Filtering probe: of %u world binds, no depth write %u, projected %u, scene %u, one level %u, "
               "level 1 never filled %u, filtered %u\n", n[6], n[0], n[1], n[2], n[3], n[4], n[5]);
    if (why < 5)
        return;
    uint32_t ready = 0;
    while (ready < levels && ready < 32 && ((filled >> ready) & 1u))
        ++ready;
    if (sharp > 0.0f)
        sk->lod_bias = (int8_t)(-(sharp > 10.0f ? 10.0f : sharp) * 3.2f); /* (sixteenths of a level: 10 is 2 levels) */
    if (!(aniso >= 1.0f))
        return;
    sk->mag = 2, sk->mip = 2, sk->min = aniso > 1.0f ? 3 : 2;
    sk->max_aniso = (uint8_t)(aniso > 16.0f ? 16.0f : aniso);
    if (ready < levels && (sk->lod_cap == 0 || sk->lod_cap > ready))
        sk->lod_cap = (uint8_t)ready;
}

/* A piece's own part of a batched draw (GfxDraw.inst): GfxU's wvp, wv and wvit as they lie there (column after column, as
 * the HLSL reads a matrix), then GfxDraw.anchor. The vertex function reads the matrices by SV_InstanceID (gfx_hlsl.c), 208
 * bytes a piece */
struct GfxInst
{
    float m[48];
    float anchor[4];
};
#define GFX_INST_BYTES 208

/* Brings the device up on an SDL window. 0 on failure (nothing is drawn, nothing fails). */
int gfx_init(void* sdl_window, int vsync);
#ifdef VANACORE
/* gfx_init's window is the game's HWND. A first call with none brings the device up alone (so a
 * machine without Direct3D 12 is known before the game is given Direct3D 8); a later call with one
 * puts the swap chain on it. gfx_set_vsync: whether Present waits for the display. */
void gfx_set_vsync(int on);
/* The graphics card's name ("" until the device is up). */
const char* gfx_adapter_name(void);
/* This process's video memory in use and Windows' budget for it, in bytes (DXGI's, so any card: the figure NVML's
 * readings can't give on an AMD or Intel card). 1 when known; 0 with no device up, or a DXGI before 1.4. */
int gfx_video_memory(unsigned long long* used, unsigned long long* budget);
#endif
/* A window of this many pixels: the back buffer's new size after Reset. */
void gfx_resize(uint32_t w, uint32_t h);

/* A vertex or index buffer the game fills once and draws many times: kept on the GPU, uploaded
 * whole when it changes (into a fresh buffer when the GPU may still read the old contents). */
GfxBuf* gfx_buf_create(uint32_t size);
void gfx_buf_destroy(GfxBuf* b);
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size);

GfxTex* gfx_tex_create(int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use);
void gfx_tex_destroy(GfxTex* t);
/* A whole level from D3D's layout (pitch bytes per row, or per block row for DXT). */
void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch);
/* A rectangle of a level (4x4 aligned for DXT); src points at its first texel. */
void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch);
/* A whole level back into D3D's layout (waits for the GPU). */
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
/* The same without waiting: the newest copy of the level the GPU has finished (1-3 frames old),
 * and a new copy queued behind this frame's work. Only the first read of a level waits. For
 * read-only locks the game polls every frame (FFXI's occlusion probe). */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
/* A rectangle between textures of one format. */
void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h,
    GfxTex* dst, uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy);

/* The render target (a level of a texture, or a cube face) and depth-stencil surface draws go to. */
void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth);
/* Clear(Count, pRects, Flags, Color, Z, Stencil) within the viewport (D3DVIEWPORT8). */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t vp[6]);
void gfx_draw(const GfxDraw* d);
/* The game draws its zone (1, FFXiMain's zone draw begins: core/zonebatch.cpp) and has drawn it (0). Between them, with
 * [graphics.effects] batch on, the back end keeps the zone's draws and sends those of one mesh, texture and state together,
 * one draw for all of their pieces (gfx_d3d12_batch.h); Direct3D 11 draws them as they come. */
void gfx_batch(int open);
/* The player light's pool as the scene effects' composite laid it this frame (gfx_shared.c): the back ends keep it once they
 * have it (plight, plight2: gfx_player_light's; up: the world's up in view space, as the composite has it), gfx_scene_done
 * forgets it first. gfx_player_pool gives it as a draw's texture matrix 5 (GfxFsKey.pool): plight, plight2, up, then 0; 0 and
 * nothing written when this frame's composite laid none. */
void gfx_player_pool_keep(const float plight[4], const float plight2[4], const float up[3]);
void gfx_player_pool_forget(void);
int gfx_player_pool(float m[16]);
/* The height fog as the composite laid it this frame (gfx_shared.c), for the world's draws after the effects (the game's water
 * layers, drawn after them, had none and stood out unfogged against the fogged shore). The back ends keep it from the
 * composite's uniforms (fogc, fogp, up, sun, suncol: the same as FxU's), gfx_scene_done forgets it first (with the pool).
 * gfx_height_fog gives it as a draw's texture matrix 4 (GfxFsKey.hfog): fog colour and density at the camera's height;
 * falloff, most fog, scattering g, then the sun glow's colour (suncol times sun.w times fog_sunlight / 5) in the 4th of each of the
 * last three rows; up; the sun's direction. 0 and nothing written when this frame's composite laid no fog. */
void gfx_height_fog_keep(const float fogc[4], const float fogp[4], const float up[3], const float sun[4], const float suncol[3]);
int gfx_height_fog(float m[16]);
/* The camera and light of a frame's 3D scene, as the scene effects need them. */
/* the most zone lines a scene carries (the zone's; the effects draw the nearest) */
#define GFX_ZONELINES 64
/* the most places a scene carries sparkles rising from, or a character's aura (vanacore_gfx_set_sparkles), and each one's
 * floats */
#define GFX_SPARKLES 16
#define GFX_SPARKLE_FLOATS 12
/* the most rings of light on the ground a scene carries (vanacore_gfx_set_ground_marks: addons' vana.world.marks), and each
 * one's floats: its centre (x, height, y), radius, colour (rgb), strength, shape (0 round, 1 square), the height of the
 * column of light over it (0 none), the glyph over that (0 none, 1 to 8: A, B, C, D, 1, 2, 3, 4), the glyph's size, how high
 * over the ring the glyph's centre is (0: just over the column's top), three spare */
#define GFX_GROUND_MARKS 16
#define GFX_GROUND_MARK_FLOATS 16

typedef struct GfxScene
{
    float proj[16];      /* D3DTS_PROJECTION: a perspective one */
    float view[16];      /* D3DTS_VIEW */
    float sun_dir[4];    /* camera space, toward the light; w = 1 when the scene had a directional light */
    float sun_color[4];  /* its diffuse color */
    float ambient[4];    /* D3DRS_AMBIENT */
    float fogcolor[4];
    float fog[4];        /* fog start, end; z = 1 when fog was on */
    uint32_t vp[6];      /* the viewport the 3D draws used (D3DVIEWPORT8) */
    uint32_t cam_draw;   /* the scene's 3D draw (1, 2, ...) the camera came from this frame; 0: none, the last one's kept */
    uint32_t sun_draw;   /* ... and the sun (sun_dir), for the flicker watch */
    float focus[4];      /* where the player stands, in the world; w = 1 when known (the shadows' sharp map) */
    /* VanaCore's zone lines (vanacore_gfx_set_zonelines): boxes in the world, each its centre (x, height, y as
     * the game draws), its turn about the up axis (radians), then its full width, height and depth, and how far
     * above its middle the ground is under it (0: at the middle; the lines, dots and sparkles start from it) */
    float zonelines[GFX_ZONELINES][8];
    uint32_t nzonelines;
    float zoneline_color[4]; /* their colour (rgb), a how strongly they're drawn (0: not at all) */
    uint32_t zoneline_style; /* 0 a curtain, 1 a line floating across, 2 that line in dots, 3 sparkles, 4 dots on the floor */
    uint32_t zoneline_glow;  /* 1: a halo round them */
    uint32_t present_w, present_h; /* the size the scene's target is shown at (the back buffer's drawn size): DLSS's output */
    float zoneline_height;   /* the line's (and the dots') height above the ground under each box (its middle without one) */
    float zoneline_size;     /* lines', dots' and sparkles' size, times their own */
    float zoneline_fade;     /* how far (world units) their ends fade out, toward the box's ends or the walls (0: none) */
    /* VanaCore's sparkles (vanacore_gfx_set_sparkles): places in the world they rise from, or characters with an aura, each
     * its centre on the ground (x, height, y as the game draws), the disc's radius, its colour (rgb), how high they rise, its
     * style (0 sparkles, 1 an aura), size, amount and strength */
    float sparkles[GFX_SPARKLES][GFX_SPARKLE_FLOATS];
    uint32_t nsparkles;
    float sparkle_size;      /* the sparkles' size, times their own (an aura's is its place's alone) */
    float sparkle_density;   /* how many to a square unit of the disc, times their own (as size) */
    float sparkle_speed;     /* how fast they rise and come and go, times their own */
    uint32_t sparkle_match;  /* 1: each the colour of the scene where it was born (its place's where that isn't seen) */
    /* rings of light on the ground (vanacore_gfx_set_ground_marks): each its centre (x, height, y as the game draws), its
     * radius, its colour (rgb) and strength, its shape and the height of the column of light over it; laid onto the ground
     * the scene's depth shows, so what stands in front of one hides it and it follows a slope */
    float ground_marks[GFX_GROUND_MARKS][GFX_GROUND_MARK_FLOATS];
    uint32_t nground_marks;
    /* 1 in a Mog House, or while the sun hasn't been seen to move with the clock in this zone (a zone lit by a fixed light:
     * d3d8_sun.c sun_indoors), easing to 0 once it has: the sun's shadows scaled by the moghouse setting by it; 0 outdoors */
    float indoors;
    /* 1 while a home point's teleport is under way (vanacore_gfx_set_warping): the glow effects lighting eases down to
     * elight_warp times its strength, and back after */
    uint32_t warping;
    /* the Vana'diel minute of the day, 0 to 1440 (vanacore_gfx_set_vana_minute); 0 until it is known: the sun's shadows before
     * the sun is up (gfx_dawn) */
    float vana_minute;
    /* how far into the night it is, 0 by day to 1 (d3d8_sun.c night_ease: the clock, outdoors only, eased), while [graphics.effects]
     * night is on, else 0: the moonlight's colour (night_moon) is laid by this much (the world's light is dimmed in d3d8.c) */
    float night;
    /* 1 by day, -1 at night, between them through dusk and dawn, 0 indoors or while the clock isn't known (d3d8_sun.c night_ease,
     * whatever night is): [graphics.effects] day_night_warmth adds this times itself to the colour grade's warmth */
    float day_night;
    /* how far into the night the clock is, as night but whatever [graphics.effects] night is (0 indoors, and while the clock
     * isn't known): the player's light darkens round its pool by day only (gfx_player_light), not at night with Night off */
    float night_clock;
    /* the world's directional and ambient light raised by this share each, 0 none (d3d8_sun.c d3d8_brighten: night_bright,
     * night_bright_ambient, day_bright, indoor_bright): the shadows' sun_direct measures the light as it is drawn, so a moon made
     * as strong as the sun brings its shadows in as the sun's do */
    float brighten[2];
    /* 1: VanaCore's water pass drew this frame over a copy of the depth taken before it (gfx_scene_snapshot) and the sun's shadows
     * are to leave it alone ([graphics] water.shadows 0, d3d8_water.h water_pass): fx_ao takes a pixel whose depth the pass
     * changed as lit, with no occlusion */
    uint32_t water_noshadow;
    float rain_sheen; /* the rain's wet sheen on the characters, laid by the effects (gfx_d3d12.c fx_comp): wetness x rain.sheen, 0 none */
} GfxScene;

/* VanaCore's DLSS (Direct3D 12; the other back ends say no): quality -1 off, else an NVSDK_NGX_PerfQuality_Value
 * (0 performance, 1 balanced, 2 quality, 3 ultra performance, 5 DLAA); helper the path of VanaCore.Dlss.exe, log
 * the file it writes to (either NULL: kept). The scene's target should be drawn at the size the quality asks for
 * (d3d8's scene scale); gfx_scene_done then runs DLSS on it, and the frame's draws read its output in its place. */
void gfx_dlss_set(int quality, const wchar_t* helper, const wchar_t* log);
/* this frame's jitter for the scene's 3D draws, as a clip-space offset (times w); 0 while DLSS isn't drawing */
int gfx_dlss_jitter(float* ndc);
/* 0 off, 1 starting, 2 on, 3 not available here, 4 failed; message (the helper's) into message */
int gfx_dlss_state(char* message, int size);
/* DLSS frame generation (with DLSS on; RTX 40 or later): count frames made between two the game draws, 0 off, 1 (2x)
 * to 3 (4x, RTX 50: fewer where the card makes fewer). The frames go to the screen from a thread of their own, spaced
 * over the time a frame takes the game. */
void gfx_dlss_framegen(int count);
/* 0 off, 1 waiting (for DLSS), 2 on, 3 not available here, 4 failed; what it's doing, or why not, into message */
int gfx_dlss_framegen_state(char* message, int size);
/* What the picture is still waiting on as of the last frames, GFX_WAIT_* bits (0 nothing): for VanaCore's notice while
 * it's made, so a picture drawn small and stretched, or without its shadows, doesn't look like nothing happened */
#define GFX_WAIT_DLSS 1u     /* DLSS starting (its helper, or DLSS made for new sizes): the scene drawn small, stretched */
#define GFX_WAIT_DLSS5 2u    /* ... and DLSS 5 with it */
#define GFX_WAIT_FRAMEGEN 4u /* frame generation starting */
#define GFX_WAIT_EFFECTS 8u  /* the scene effects' own shaders compiling */
#define GFX_WAIT_SHADOWS 16u /* many of the sun's shadow casters left out while their pipelines build */
unsigned gfx_waiting(void);
/* How far one of those bits has come, for the notice's count: *done of *total steps (shaders compiled of all of them;
 * shadow casters ready of those drawn), 1 when it has one, 0 when it doesn't (or hasn't started) */
int gfx_wait_progress(unsigned bit, unsigned* done, unsigned* total);
/* What one of those bits is at now, in words for the notice ("Ambient occlusion": the shader compiling; "Shadow casters'
 * pipelines"), into words: 1 when it has them, 0 (words "") when it doesn't */
int gfx_wait_step(unsigned bit, char* words, int size);

/* The frame's 3D scene is finished in color (a render target's first level, drawn with depth
 * testing), before anything samples it or draws the interface over it. The back end's scene
 * effects - ambient occlusion from the depth it was drawn with, color grading - run on it in place
 * (FFXI_FX=1); back ends without them do nothing. */
void gfx_scene_done(GfxTex* color, const GfxScene* s);
/* The scene's depth as it is now, before VanaCore's grass is drawn into it (grass.c), kept for this frame's scene effects: their
 * occlusion reads the world from it, so the grass casts none, and skips the pixels the grass covers (those
 * keep the sun's shadow maps, at the grass's own place); the reflections trace the world from it, so the grass neither shows in
 * them nor takes one. Nothing while the effects have none of those on. */
void gfx_scene_world_depth(GfxTex* depth);
/* Copies of the scene's colour target and of the depth it was last drawn with, as they are now, for VanaCore's water pass
 * (d3d8_water.h water_pass): the water reads the bed's depth and colour under each of its pixels from them while it draws into
 * the target itself. The back end keeps the two (made again at another size or format); w and h are the target's size. 0 when
 * there is nothing to copy (no device, a target not drawn with a depth of its own size): the water is then drawn without them. */
int gfx_scene_snapshot(GfxTex* color, GfxTex** colour_copy, GfxTex** depth_copy, uint32_t* w, uint32_t* h);
/* A screenshot of the scene without the game's interface (gfx_scene_shot.h; the MCP server's screenshot tool with ui false).
 * gfx_scene_shot_want(1) asks for one: the back end copies the next frame's scene as the frame shows it - the game's own effects
 * in it, made larger by the upscaler, graded - just before the game's interface goes into it or over it (the names over heads,
 * then its windows on the back buffer); a frame without a scene takes none, and it stays asked for until gfx_scene_shot_want(0).
 * gfx_scene_shot gives the copy: its size and D3D format (0 while none has been taken), and with pixels its rows in D3D's layout
 * (as gfx_tex_read; waits for the GPU), after which it's let go. */
void gfx_scene_shot_want(int on);
int gfx_scene_shot(uint32_t* w, uint32_t* h, uint32_t* d3dfmt, void* pixels, uint32_t pitch);
/* The world mirrored in a level of water, for VanaCore's water pass (d3d8_water_mirror.h): this frame's opaque world draws, as
 * the back end keeps them for the sun's maps (its casters), drawn again from the scene's camera mirrored about the level plane
 * y = level (the world's up is -y) into a target `scale` times the scene's viewport each way: what stands above the level alone
 * (the plane is the pass's near plane), nothing farther from the camera than `reach` (0: all of it). The back end keeps the
 * target (made again at another size); its alpha is 0 where the world was drawn and 1 where nothing was (the sky's place). NULL
 * when there is none to give (no device, no casters kept yet: the first call asks for them from the next frame on, and each call
 * keeps them kept for a few frames more). */
GfxTex* gfx_scene_mirror(const GfxTex* color, const GfxScene* s, float level, float scale, float reach);
/* The zone's floors drawn this frame as a layer of their own (floors.c), with the scene's camera, the viewport's size: id's red is
 * the kind of ground (the game's footsteps': object, path, grass, sand, snow, stone, metal, wood, shallow water, deep water, as
 * 1 to 10 times 16; 0 no floor) and depth the floors' depth. The reflections keep to the kinds asked for ([graphics.effects]
 * refl_floors) where a floor lies at a pixel's depth. NULL: not drawn this frame; state says why the floors weren't: 0 not read
 * yet (the reflections wait), 2 the zone has none (any ground); 1 they're in id and depth. gfx_scene_floors_wanted: 1 while the
 * back end would use the floors (the reflections on, keeping to kinds of ground), so they're drawn only then. */
void gfx_scene_floors(GfxTex* id, GfxTex* depth, int state);
/* the rain's wet ground this frame (d3d8_rain.h rain_mask_draw): its mask at the scene's size (red wet, green puddles, blue
 * open), how wet (0 to 1, eased), rain.reflect and rain.puddles; NULL none. Direct3D 12's floor reflections mirror the scene
 * there. _wanted: whether anything would read it */
void gfx_scene_rain_mask(GfxTex* mask, float wet, float reflect, float puddles);
int gfx_scene_rain_mask_wanted(void);
int gfx_scene_floors_wanted(void);
/* The zone's collision as a bounding-volume tree (rtbvh.h; `bytes` of it, the triangles from `node_bytes`) for the ray-traced
 * reflections (Direct3D 12, [graphics.effects] refl_rt): kept in a buffer the reflection shader walks. NULL or 0: let the last go */
void gfx_scene_rt_geometry(const void* data, uint32_t bytes, uint32_t node_bytes, int scene, uint32_t attr_at, GfxTex* const* tex, uint32_t ntex);
/* How far round the player VanaCore's caster pass (casters.cpp) has the game draw the zone's pieces for the sun's maps alone
 * (GfxDraw.sought), whatever the camera sees: the maps' reach; and in *stretch (when wanted) how far a piece's shadow reaches along
 * the ground for each unit of its height (the light's cosine over its sine, 3 at most), which the pass takes a tall piece from
 * farther by. 0 while the back end wouldn't keep them: no sun shadows, the zone not casting, no cache to hold them between the
 * frames the pass reaches them (Direct3D 11 has none of it) */
float gfx_seek_reach(float* stretch);
/* The shadows' record (gfx_sun_record.h): every frame's shadow state as a line of the CSV file at `path`, up to max_mb (0: 40), from
 * now until a call with a null path closes it. 1 when it started (Direct3D 12 alone; Direct3D 11 has none) */
int gfx_sun_record(const char* path, uint32_t max_mb);
/* VanaCore's glow round a character the game drew alone into src (a render target as large as the one being
 * drawn): src blurred, in the tint (rgb, and alpha times how strong), laid over the target being drawn. Nothing
 * without the scene effects' functions. */
void gfx_glow(GfxTex* src, const float tint[4]);
/* VanaCore's grass drawn into color and depth this frame over what gfx_glow laid partway through the world (grass.c, after
 * gfx_scene_world_depth): the frame's glows laid again over the pixels the grass covers, so the tufts in front of a glowing avatar
 * don't take its glow away. Nothing with no glow laid this frame. */
void gfx_glow_over_grass(GfxTex* color, GfxTex* depth);
/* VanaCore's sky ([graphics] sky, sky.c): drawn in place of the game's dome, whose draws come first in the frame's world and are
 * skipped. The dome is bands of colour round the camera from the horizon up, its vertices' colours alone; it is kept here as its
 * rings, each its height (the sine of its elevation) and its colour at GFX_SKY_BEARINGS bearings round it, and drawn into the
 * target bound now (the scene's) over the dome's viewport, writing no depth, before the world's first draw - so the world covers
 * it as it covered the dome. Below the lowest ring and above the highest nothing is drawn, as the dome drew nothing there. */
#define GFX_SKY_RINGS 16
#define GFX_SKY_BEARINGS 16
typedef struct GfxSky
{
    float inv[16];  /* clip space to the dome's own: its world-view-projection inverted (D3D's row vectors, v * M) */
    float jitter[2]; /* DLSS's sub-pixel step its draws took (clip space, times w: GfxU.pad0's), taken back out */
    uint32_t vp[6]; /* the dome's viewport (D3DVIEWPORT8, as its draws had it) */
    uint32_t nrings; /* 2 or more */
    float ring[GFX_SKY_RINGS];                    /* each ring's height, the sine of its elevation, from the lowest up */
    float color[GFX_SKY_RINGS][GFX_SKY_BEARINGS][4]; /* ... and its colour (rgba, 0..1) at each bearing: the first toward +z, then
                                                      * on toward +x (atan2(x, z)), in the dome's own space */
} GfxSky;
/* 1 when drawn; 0 when it can't be (a back end without it: Direct3D 11; its shader not built yet). NULL: whether it could be now, the
 * shader's build started if it hasn't been - asked before the dome's first draw is skipped */
int gfx_sky(const GfxSky* s);
/* One scene-effect setting by its key in the settings file (fx, ao, fog, bloom, rays, ...). */
void gfx_fx_set(const char* key, float v);
/* The shadows' hours (gfx_dispatch.c): 1 while the clock is in the moon's hours, 18:00 to 06:00, outdoors (d3d8_sun.c, once a
 * world scene) - the night's strength and casters (sun_moon_strength, sun_moon_casters) go to the back end in the day's place
 * while sun_moon is on. gfx_sun_chars_now: 1 while the casters handed over now take in characters (sun_casters, or the night's,
 * not 2): the game's own character shadow left out then (d3d8.c game_shadow_hidden) */
void gfx_sun_hours(int night);
int gfx_sun_chars_now(void);
/* The scene effects' shaders compiled again (VanaCore's Recompile shaders): asked for now, done at the end of the frame,
 * where every one of them and the pipelines made from them are let go (after the GPU's frames) and the build starts over
 * at the next scene that asks, as at the first (the notice meanwhile). Waits for a build that's on. */
void gfx_fx_rebuild(void);
/* The scene effects' last frames (what the shadows were drawn from), one line each, to a file. */
void gfx_trace_dump(const char* path);
/* The flicker watch: a line about this frame to the log ("flicker watch: frame N: ..."), held back in a
 * burst. gfx_watch_dump_due: 1 once, a second after the first line in a quiet minute - time to write the
 * traces (d3d8.c scene_trace). */
void gfx_watch(const char* fmt, ...);
int gfx_watch_dump_due(void);
/* The flicker watch's toggle finder: a value the frame is made from, named by a string literal and fed once a frame (while
 * profiling or watching, like gfx_watch). One that turns round - up, then down, then up - three times in about three seconds is said
 * ("flicker watch: <name>: went to and fro ...") with its last 64 frames: what a flicker's cause does while the flicker does it. */
void gfx_watch_value(const char* name, float v);
/* The frame being recorded (the watch's and trace_gfx.txt's frame numbers). */
uint64_t gfx_frame(void);

/* The frame is done: the back buffer goes to the window. */
void gfx_present(GfxTex* backbuffer);
#ifdef VANACORE
/* A premultiplied layer of the window's size to lay over the frame at the next gfx_present (VanaCore's
 * own interface), or NULL. */
void gfx_present_layer(GfxTex* layer);
/* Rectangles over which the frame is blurred at the next gfx_present, under the layer: frosted glass behind VanaCore's own
 * windows. `rects`: n of them, 6 floats each (x, y, width, height, corner radius, strength 0 to 1), in the layer's pixels.
 * Asked for again every frame, dropped once presented. 1 when this back end draws them, 0 when it doesn't (nothing is kept then). */
int gfx_present_backdrops(const float* rects, int n);
/* A premultiplied layer of the window's size drawn UNDER the backdrops at the next gfx_present (the frame, then this, then the
 * blur of both behind each rectangle, then gfx_present_layer's): the windows below the lowest one with a backdrop, so that
 * those are blurred too. NULL for none. Back ends that draw no backdrops never get one. */
void gfx_present_layer_below(GfxTex* layer);
#endif
/* Waits for the GPU (tests). */
void gfx_finish(void);
/* Tests: build each pipeline in place, before its first draw, and keep no pipeline cache file. */
void gfx_set_sync_pipelines(int on);
/* Shaders or pipelines that failed to build so far (tests). */
uint32_t gfx_failures(void);
/* 1 once the graphics card has been lost (DXGI's device removed: a driver timeout, the driver restarting), found at
 * Present, a wait for the GPU or something failing to be made: nothing drawn after it shows, ever. */
int gfx_lost(void);
/* The device torn down, every object the back end made on it released (the front end's textures and buffers first:
 * d3d8_device_lost), its threads stopped and the upscaler's helper let go, ready for gfx_init to bring a new one up on
 * the same window - after the card was lost, or in a test. 1 when the device itself was let go of (Direct3D 12's device
 * is one per card in a process: a child of it still held anywhere keeps the old, broken one, and D3D12CreateDevice would
 * hand that one back), 0 when it's still held somewhere, and no new device can be made. Keeps the shaders' compiled
 * code and the settings. */
int gfx_shutdown(void);

/* Frame profile (FFXI_PROFILE=1): the front end adds the time it spends turning a D3D call into
 * back-end work; the back end logs the breakdown every two seconds from Present. */
/* the moon's size and brightness, a multiple each ([graphics.effects] moon_size, moon_brightness; 1 as the game draws it):
 * taken by d3d8.c's draw preparation, so with any renderer (gfx_fx_set, gfx_dispatch.c) */
extern float g_gfx_moon_size, g_gfx_moon_bright;
/* [graphics.effects] night, 0 to 1 (0 off): how dark the Vana'diel night makes the world's light (d3d8.c night_light), any
 * renderer (gfx_fx_set, gfx_dispatch.c); night_bright, 0 to 3 (0 off), how much brighter instead (the moon's light raised) */
extern float g_gfx_night, g_gfx_night_bright;
/* [graphics.effects] indoor_bright, 0 to 5 (0 off): how much brighter the world's light is indoors (the ambient and the zone's
 * fixed light raised: d3d8_night.h night_light); ambient_force, 0 to 1 (0 off): the least ambient light every lit draw of the
 * world has, day and night, indoors and out (Windower's game_forceambientlight, Ashita's /ambient) */
extern float g_gfx_indoor_bright, g_gfx_ambient_force;
/* [graphics.effects] night_bright_ambient, 0 to 3 (0 off): Bright night's General lighting, the night's ambient raised (night_bright
 * itself raises the moon's light: d3d8_sun.c d3d8_brighten); with night_bright's switch (graphics.cpp) */
extern float g_gfx_night_bright_ambient;
/* [graphics.effects] day_bright, 0 to 3 (0 off): Sun strength, the sun's light raised to 1 + it through the day, outdoors
 * (d3d8_sun.c d3d8_brighten) */
extern float g_gfx_day_bright;
/* Last frame's small screen-space quads drawn as one (gfx_dispatch.c, [graphics.effects] textbatch): how many were kept,
 * and the draws they went out as */
void gfx_quads_counts(uint32_t* quads, uint32_t* draws);
/* [graphics.effects] batch: the zone's draws of one mesh sent together (gfx_batch), 0 or 1; off until it's proven in play */
extern int g_gfx_batch;
/* [graphics.effects] refl_walkable: only the floors one can walk on reflect (floors.c walk_scene), 0 or 1; on by default */
extern int g_gfx_refl_walkable;
/* The occlusion test ([graphics] occl; core/occlusion.cpp, gfx_d3d12_occl.h): each piece of the zone's first slot as a box in
 * the world, tested once a frame against the depth of the scene just drawn (Direct3D 12); what comes back, frames later, says which
 * were hidden behind what was drawn, and the core leaves those undrawn. The boxes are the core's (gfx_occl_boxes, a new set for a
 * new zone: gen); the result is the back end's, for the set it was tested with. Game thread only. */
#define GFX_OCCL_COLS 128                  /* pieces a row of the test's target */
#define GFX_OCCL_MAX (GFX_OCCL_COLS * 256) /* ... and the most it holds */
typedef struct GfxOccl
{
    float* boxes;        /* count * 6: each piece's box, low x, height, y then high (as the game draws them); low x over high: none */
    uint32_t count, gen; /* ... how many, and which set */
    int on;              /* 1: tested each scene */
    float margin;        /* the margin round each piece's box on the screen, thousandths of the scene's width (the camera's moves) */
    uint8_t* result;     /* count bytes: 0 off the screen (or not tested), 128 on it and seen, 255 on it and hidden */
    uint32_t result_gen, result_seq; /* ... the set it is for (0: none yet), and how many results have come back */
    uint32_t tested, hidden;         /* ... the last result's pieces on the screen, and of them hidden */
} GfxOccl;
extern GfxOccl g_gfx_occl;
/* the pieces' boxes (count * 6 floats, at most GFX_OCCL_MAX), copied; a new gen, the old result let go */
void gfx_occl_boxes(const float* boxes, uint32_t count);
extern int gfx_profiling;
/* The flicker watch (gfx_watch, gfx_watch_value) is on with the frame profile or with this: Direct3D 11 has no frame profile
 * (gfx_profiling stays 0 there) but takes a /vana debug recording or [graphics] profile for the watch alone. */
extern int gfx_watching;
/* Goes up each time the profile comes on (/vana stats opened): each counter starts over then, so its first line
 * isn't an average over the time it was off. */
extern uint32_t gfx_profile_epoch;
uint64_t gfx_now_ns(void);
void gfx_prof_front(uint64_t ns);
/* A draw dropped, and why (the profile line counts each reason). */
enum
{
    GFX_SKIP_NO_SHADER,
    GFX_SKIP_STREAM,
    GFX_SKIP_NO_POSITION,
    GFX_SKIP_NO_BUFFER,
    GFX_SKIP_RANGE,
    GFX_SKIP_NO_INDICES,
    GFX_SKIP_PIPELINE,
    GFX_SKIP_NO_TARGET,
    GFX_NSKIPS,
};
void gfx_prof_skip(int reason);
/* Time in a shim call (thunk_timer): counted for the thread that presents, the game's. */
void gfx_prof_shim(uint64_t ns);
