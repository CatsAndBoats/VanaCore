/* The graphics back end on Direct3D 12 (Windows x64): what gfx.h asks for, on one ID3D12Device.
 * The same design as gfx_metal.m, in D3D12's terms:
 *
 *   - Frames: one command list recording at a time, executed at Present (or when the CPU needs a
 *     result, or in chunks mid-frame so the GPU starts early); up to three frames in flight, each
 *     with its own command allocator and upload ring - vertex, index and uniform bytes are copied
 *     into the ring per draw, so the game may rewrite a buffer the moment a draw returns, as D3D
 *     lets it. One fence counts submissions; a resource remembers the submission that last used it.
 *   - Resource states: textures carry their state (whole resource) and transition on use; buffers
 *     decay to COMMON after every submission, so they are tracked per command list.
 *   - Binding: one root signature for everything - the uniforms (b0), the draw's texture and sampler
 *     heap indices as root constants (b1), the vertex streams as raw root SRVs (t0..t3), and the whole
 *     shader-visible heaps as unbounded Texture2D / TextureCube / sampler tables. A texture's SRV lives
 *     in the heap for its lifetime; a draw binds nothing but 16 indices.
 *   - Pipelines come from the generated HLSL (gfx_hlsl.c), compiled with D3DCompile (SM 5.1) and
 *     built off the game's thread, cached by key: shaders, blending, depth-stencil, rasterizer and
 *     topology type (D3D12 bakes all of them into the pipeline). Samplers are cached by key.
 *   - Clears are D3D12's own (with rectangles); the back buffer reaches the window through a flip
 *     swap chain on the SDL window, drawn scaled with the frame-rate overlay on top.
 *   - Textures are default-heap resources filled through the ring with CopyTextureRegion. Formats
 *     D3D12 has no sampled match for (the 16-bit color ones) are widened to BGRA8 on upload, as on
 *     Metal; the swizzled ones (X8R8G8B8, L8, A8L8) are SRV component mappings. */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#ifndef VANACORE
#include <SDL3/SDL.h>
#endif
#if defined(FFXI_UWP)
#include "uwp_bridge.h"
#endif
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef VANACORE
/* one of VanaCore's two back ends (gfx_backend.h): its gfx.h functions are gfx12_* */
#define GFX_PREFIX gfx12
#include "gfx_rename.h"
#endif
#include "gfx.h"
#include "gfx_hlsl.h"
#include "gfx_shared.h"
#ifdef VANACORE
#include "../dlss/dlss_ipc.h"
#endif

#define FRAMES 3
#define GFX_PROBES 8 /* reads of one surface per frame that keep their own history */
#define GFX_READBACKS ((FRAMES + 1) * GFX_PROBES)
#define RING_CHUNK (8u << 20)
#define SRV_HEAP 65536u    /* shader-visible CBV/SRV/UAV descriptors: one per texture */
#define SAMPLER_HEAP 2048u /* the most a shader-visible sampler heap may hold */
#define RTV_HEAP 4096u
#define DSV_HEAP 512u
#define SRV_NULL_2D 0 /* heap slots holding null views: an unbound Texture2D / TextureCube */
#define SRV_NULL_CUBE 1

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
    F_D24S8 = 75,
    F_D24X8 = 77,
    F_D16 = 80,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum
{
    CONV_NONE,
    CONV_565,
    CONV_X555,
    CONV_1555,
    CONV_4444,
};

/* the root signature's parameters */
enum
{
    ROOT_U,       /* CBV b0: GfxU (the utility functions' own constants too) */
    ROOT_BIND,    /* 16 constants b1: texture heap indices ti[8], sampler heap indices si[8] */
    ROOT_STREAM0, /* raw SRVs t0..t3: the vertex streams */
    ROOT_TEX2D = ROOT_STREAM0 + GFX_NSTREAMS,
    ROOT_TEXCUBE,
    ROOT_SAMPLERS,
    ROOT_SUN,     /* 20 constants b2: the sun's shadow map's matrix (its draws, gfx_hlsl_vs_return), then a fading caster's share and
                   * the map's texel offset (sun_f: cache_let_go) */
    ROOT_SAMPCMP, /* the sampler heap again, as comparison samplers (s0, space1): the shadow lookups */
    ROOT_COUNT,
};

struct GfxTex
{
    ID3D12Resource* res;
    DXGI_FORMAT dxfmt;
    int type, use, conv;
    uint32_t fmt, w, h, levels; /* D3D's sizes */
    uint32_t rw, rh;            /* the resource's: DXT top levels round up to whole blocks */
    uint32_t faces;
    uint32_t block;       /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t texel;       /* bytes per texel in D3D12's layout */
    uint64_t used;        /* the last submission that referenced it */
    int has_stencil, x8;
    D3D12_RESOURCE_STATES state;
    int32_t srv;          /* its slot in the shader-visible heap, or -1 */
    int32_t* views;       /* RTV (or DSV) slots per subresource, created on first use; -1 until then */
    /* asynchronous readbacks (gfx_tex_read_async): staging buffers and the submission each was recorded in */
    ID3D12Resource* rb[GFX_READBACKS];
    uint8_t* rb_cpu[GFX_READBACKS];
    uint64_t rb_size[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame; /* the frame the reads below were counted in */
    uint32_t rb_count; /* reads of this surface so far in that frame */
    /* the scene effects (gfx_scene_done) */
    uint32_t mips;        /* levels in the resource: `levels`, or a large color target's whole chain */
    int32_t srv_mips;     /* ... that chain, for the draw that makes the finished scene smaller; else -1 */
    int32_t* mip_srv;     /* one level each (0 .. mips - 2), which the next is made from */
    uint64_t scene;       /* its chain holds its first level as it is now (the scene filter): 0 once it is drawn to again */
    uint32_t filled;      /* the levels uploaded so far, a bit each (anisotropic filtering wants them all) */
    GfxTex* depth_seen;   /* color targets: the depth its first level was last drawn with */
    GfxTex* depth_world;  /* ... the depth the scene's casters (its world) were last drawn with: what the scene effects
                           * read - a later pass on it with another depth does not change it */
    uint64_t sun_seen;    /* the frame a shadow caster last sampled it (alpha-tested) */
    uint8_t sun_cached;   /* the zone's cached casters may sample it */
    uint32_t mo_batch;    /* the moving things' redraws waiting (mo_keep) that sample it or test its depth: drawn before it changes */
    uint32_t batch;       /* the zone's draws kept (gfx_d3d12_batch.h) that sample it: sent before it changes or goes (BATCH_TOUCH) */
    GfxTex *rt_prev, *rt_next; /* every color target, so a depth surface's end clears depth_seen */
};

/* Static buffers live in slabs (from XI on Anything, rubymatrix/xi-on-anything, runtime/portable/gfx_d3d12.c): one
 * resource carved into equal blocks of a power-of-two size, 256 bytes to 256 KB, or for a larger buffer a resource of
 * its own (gfx_buf_create). A committed resource a buffer cost 0.7 ms each on an Xbox One X, and a zone load makes over
 * a thousand. Resource states are the slab's: a barrier covers every block in it. */
typedef struct Slab
{
    ID3D12Resource* res;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    D3D12_RESOURCE_STATES state; /* in command list `list`; COMMON in any other (buffers decay) */
    uint32_t list;
    int cls;                     /* its blocks' size class, or -1: the whole resource is one buffer */
    uint32_t* free_blocks;       /* the offsets of its blocks not in use */
    uint32_t nfree, nblocks;
    struct Slab* next;           /* its class's slabs */
} Slab;

struct GfxBuf
{
    Slab* slab;
    uint32_t off;                  /* its place in the slab's resource */
    D3D12_GPU_VIRTUAL_ADDRESS gpu; /* the slab's, plus off */
    uint32_t size;
    uint64_t sun_seen;  /* the frame a shadow caster last drew from it */
    uint8_t sun_cached; /* the zone's cached casters may draw from it */
    uint32_t uploads;   /* times it was written, and the frame it last was: written again lately, it's the */
    uint64_t upload_frame; /* game's to rewrite (the shadow cache keeps copies of what's drawn from it) */
    uint64_t twice;     /* the last frame it was written in more than once (REWRITTEN) */
    /* its last upload's bytes in the ring (0: none there), and the frame that ring is: a shadow caster drawn from it while
     * the game rewrites it between draws draws from them (caster_bytes) */
    D3D12_GPU_VIRTUAL_ADDRESS up_gpu;
    uint32_t up_size;
    uint64_t up_serial;
    uint32_t mo_batch; /* the moving things' redraws waiting (mo_keep) that read it where it is: its upload or end draws them first */
    uint32_t batch;    /* the zone's draws kept (gfx_d3d12_batch.h) that read it: sent before it changes or goes (BATCH_TOUCH) */
};

#ifdef VANACORE
/* the moving things' redraws kept to be drawn together (mo_keep, mo_flush): the batch waiting, and how many are in it. A buffer
 * or texture they read (its mo_batch) draws them before it changes or goes */
static void mo_flush(int timed);
static uint32_t g_mob_id = 1, g_mob_n;
#define MO_TOUCH(x) \
    do \
    { \
        if (g_mob_n && (x) && (x)->mo_batch == g_mob_id) \
            mo_flush(1); \
    } while (0)
#else
#define MO_TOUCH(x) ((void)0)
#endif
#ifdef VANACORE
/* the zone's draws kept to be sent together (gfx_d3d12_batch.h): this window's mark, and how many are kept. A buffer or texture
 * one reads (its batch) sends them before it changes or goes; anything else the back end does sends them first (BATCH_SETTLE) */
static void batch_flush(void);
static uint32_t g_batch_id = 1, g_batch_n;
#define BATCH_TOUCH(x) \
    do \
    { \
        if (g_batch_n && (x) && (x)->batch == g_batch_id) \
            batch_flush(); \
    } while (0)
#define BATCH_SETTLE() \
    do \
    { \
        if (g_batch_n) \
            batch_flush(); \
    } while (0)
#else
#define BATCH_TOUCH(x) ((void)0)
#define BATCH_SETTLE() ((void)0)
#endif

typedef struct Chunk
{
    ID3D12Resource* res;
    uint8_t* cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    uint32_t used, size;
} Chunk;

/* the GPU's time by what it was doing (profile, VanaCore's /vana stats): a timestamp where each scope starts, a scope
 * lasting until the next, the frame's last ending the final one */
enum
{
    GS_SCENE,     /* the game's scene: the world, its characters and particles */
    GS_SUN,       /* the sun's shadow maps */
    GS_SETUP,     /* the scene effects' setup (the scene copied) */
    GS_AO,        /* ambient occlusion and contact shadows */
    GS_BLOOM,
    GS_RAYS,      /* god rays */
    GS_COMPOSITE, /* fog, shadows, colour grade, outlines, smoothing */
    GS_REFL,      /* reflections and bounce light */
    GS_DOF,
    GS_FXAA,
    GS_DLSS,
    GS_UI,        /* the interface drawn over the scene, and the rest */
    GS_MEASURE,   /* the composite drawn again, measuring its features (vanacore_gfx_measure): the profile's own */
    GS_N
};
/* ... and within a scope, the part of it (gpu_part), till the next part or scope: GP_REST what isn't one of its parts;
 * GP_GAP the GPU between two command lists (idle, waiting for the next), or GP_WAIT_* where the queue waits on the
 * DLSS helper there */
enum
{
    GP_REST,
    GP_GAP,
    GP_GLOW,        /* VanaCore's glow (gfx_glow) */
    GP_MIPS,        /* a large texture's chain, for it drawn smaller (scene_mips) */
    GP_SUN_FAR,     /* the sun's maps */
    GP_SUN_NEAR,
    GP_SUN_CHARS,   /* the characters' own map (sun_soft_chars) */
    GP_COPY,        /* the scene copied, for passes to read */
    GP_AO_DEPTH,    /* the occlusion's depth and its levels */
    GP_AO,
    GP_AO_BLUR,
    GP_AO_TEMPORAL,
    GP_BLOOM_BRIGHT,
    GP_BLOOM_BLUR,
    GP_BLOOM_EIGHTH,
    GP_RAYS_MASK,
    GP_RAYS,
    GP_RAYS_BLUR,
    GP_SMOOTH,      /* the cel look's smoothing */
    GP_COMP,        /* the composite's shader */
    GP_SKY,         /* the sky's colour (fog_color=sky) */
    GP_CLARITY_BLUR,
    GP_CLARITY,
    GP_SSR,         /* reflections: their layer, at the occlusion's size */
    GP_GI,          /* bounce light: its layer, at the occlusion's size */
    GP_GI_TEMPORAL, /* ... blended with the frame before */
    GP_REFL,        /* both laid over the scene */
    GP_DOF_FOCUS,
    GP_DOF_HALF,
    GP_DOF_BLUR,
    GP_DOF_LAY,
    GP_FXAA,
    GP_DLSS_IN,     /* DLSS's inputs drawn */
    GP_DLSS_OUT,    /* its output onto the frame */
    GP_WAIT_DLSS,   /* the queue waiting on the helper's DLSS */
    GP_REPLAY,      /* the interface held for DLSS, drawn over its output */
    GP_FG_COPY,     /* frame generation's inputs copied */
    GP_WAIT_FG,     /* the queue waiting on the helper's frame generation */
    GP_PRESENT,     /* the frame onto the window */
    GP_LAYER,       /* VanaCore's interface over it */
    GP_OVERLAY,     /* the FPS overlay */
    GP_ELIGHT,      /* the light from the effects (el_note, el_run) */
    /* (the moving things drawn again for their motion and the upscaler's masks, mo_draw and mask_draw, are no part of their
     * own: drawn one by one among the game's draws, a timestamp either side of each took the game's draws still running
     * before it as theirs, and a crowd's ran out the frame's timestamps, the rest of the scene going to whichever was last.
     * They're the game's draws' (GP_REST), counted in Prof.redraws) */
    GP_FG_NAMES,    /* the names over heads drawn onto frame generation's frames (fgn_frame) */
    GP_TRUESKY,     /* VanaCore's sky, in place of the game's dome (gfx_sky) */
    GP_OCCL,        /* the occlusion test: the zone's pieces against the scene's depth (gfx_d3d12_occl.h) */
    GP_N
};
#define GPU_QS 1024 /* scope timestamps a frame: more are dropped */
#define GPU_QS_SCOPES 32 /* ... the last of them kept for the scopes' starts, so a frame of many parts still times each scope */

typedef struct Frame
{
    ID3D12CommandAllocator* alloc;
    Chunk* chunks;
    uint32_t nchunks, cur;
    uint32_t ring_most, ring_uses; /* the most chunks it used since its last trim, and its frames since (ring_trim) */
    uint64_t fence;   /* the last submission of the frame */
    int timed;        /* its GPU timestamps were resolved (profile) */
    int marks;        /* ... which of the scene effects' (prof_mark) were taken */
    int scope_n;      /* its scopes' timestamps taken (gpu_scope), the first at the start and one more at the end */
    uint8_t scope_id[GPU_QS];
    uint8_t part_id[GPU_QS]; /* ... each one's part (gpu_part) */
    int closed;       /* its last timestamp taken (frame_end): no more */
    int full;         /* its parts' timestamps ran out: a part's time from there is the one before it's */
    int measured;     /* the composite's feature its draws were measured without this frame (vanacore_gfx_measure), -1 none */
} Frame;

static ID3D12Device* g_dev;
static uint32_t g_device_gen; /* goes up at each teardown (gfx_shutdown): what a function keeps of the last device in statics of
                               * its own (pipeline_for, draw_encode) is dropped */
static ID3D12CommandQueue* g_queue;
static ID3D12GraphicsCommandList* g_list;
static int g_list_open;
static uint32_t g_list_id;    /* counts command lists recorded (buffer state decay) */
static ID3D12Fence* g_fence;
static HANDLE g_fence_event;
static uint64_t g_fence_value; /* the last submission signalled */
static ID3D12RootSignature* g_root;
static Frame g_frames[FRAMES];
static uint32_t g_frame;       /* index into g_frames */
static uint64_t g_serial = 1;  /* the frame being recorded */
static int g_frame_open;
static uint32_t g_cmd_draws;   /* draws in the command list being recorded */
static uint64_t g_gpu_ns;      /* GPU time of finished frames (profile) */
static uint64_t g_gpu_sun_ns, g_gpu_fx_ns; /* ... of it the sun's shadow maps, and the other scene effects */
/* a frame's GPU timestamps (profile): its start, the scene effects' start, the sun's maps done, the effects
 * done, its end */
enum { QN = 5 };

/* descriptor heaps: shader-visible SRVs and samplers, CPU-only RTVs and DSVs */
typedef struct Heap
{
    ID3D12DescriptorHeap* heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    UINT inc;
    int32_t* free_slots;
    uint32_t nfree;
} Heap;

static Heap g_srv, g_samp, g_rtv, g_dsv;
/* the texture each g_srv slot was made for (gfx_tex_create), NULL once it's let go: a draw whose texture's view slot is
 * another texture's says so in the log (draw_encode) */
static GfxTex** g_srv_owner;

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex* g_ds;
static GfxTex* g_scratch_depth;
static int g_targets_bound;
static ID3D12PipelineState* g_bound_pso;
static int g_bound_topo;
static void drawn_forget(void);
static ID3D12Resource* g_dummy;
static ID3D12Resource* g_sync_rb; /* gfx_tex_read's staging buffer */
static uint64_t g_sync_rb_size;

/* the window */
#ifdef VANACORE
static HWND g_window; /* the game's own */
#else
static SDL_Window* g_window;
#endif
static IDXGIFactory4* g_factory;
static IDXGISwapChain3* g_swap;
static ID3D12Resource* g_swap_buf[FRAMES];
static D3D12_RESOURCE_STATES g_swap_state[FRAMES];
static int32_t g_swap_rtv[FRAMES];
/* The present queue: a direct queue of its own that owns the swap chain. Everything written into the swap chain's buffers,
 * and every Present, goes on it: the game's queue (g_queue) draws each frame into a texture of its own (g_show's, or frame
 * generation's shown[]) and signals g_fence; this queue waits for that, copies the texture into the swap chain's buffer
 * and presents (show_present). On the game's queue the copy sat behind whatever the game had queued since - the next
 * frame's work, and with DLSS its waits on the helper (dlss_run, fg_present) - so a frame reached the screen a frame
 * late and frame generation's frames came in pairs, the game held back behind them. g_pfence is signalled here after
 * each copy; the game's queue waits for it before drawing into that texture again. g_queue stands in when it couldn't
 * be made. */
static ID3D12CommandQueue* g_pqueue;
static ID3D12Fence* g_pfence;
static uint64_t g_pvalue; /* its last value signalled */
static HANDLE g_pfence_event;
static uint32_t g_swap_w, g_swap_h, g_want_w, g_want_h;
static int g_vsync, g_tearing;
static int g_vsync_asked; /* gfx_set_vsync has been called: [graphics] vsync, which the game's own device must not undo */
static ID3D12PipelineState *g_present_pso, *g_overlay_pso;
static uint32_t g_present_samp;
static uint32_t g_cmp_samp; /* the shadow lookups' comparison sampler */
/* the frame-rate overlay: presents counted over half-second windows */
static int g_overlay = 1;
static double g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";

/* The scene effects' settings: FFXI_FX_* in the environment, then FFXI_FX_FILE while the game runs
 * (gfx_scene_done, fx_reload), or gfx_fx_set. fx = 0 is the game as it was. As gfx_metal.m. */
typedef struct FxSettings
{
    float fx, ao, radius, grade, sat, contrast, sharpen, filter, aniso, fog, fog_falloff, fog_height, fog_max, fog_floor, fog_sun,
        fog_g, bloom, threshold, rays, rays_decay, rays_length, light, sun, sun_distance, sun_soft,
        sun_face, sun_min, sun_direct, sun_dusk, sun_casters, sun_near, temporal, debug;
    float ao_quality; /* the occlusion's taps a pixel a frame: 0 low (6), 1 medium (10), 2 high (16), 3 ultra (24), 4 max (32);
                       * every one at the scene's own size */
    float ao_rings;   /* ... fewer of them past each of three rings out from the camera (fx_ao): 1 on, 0 all of them everywhere */
    float ao_ring_spacing; /* ... how far apart the rings are, world units */
    float sun_free, sun_free_az, sun_free_el; /* the shadows' light placed by hand: 1 on, its bearing round the horizon and its
                                               * height over it, degrees (sun_map's caller); the hour, the cloud and a room are left out */
    float sun_tilt; /* the game's sun and moon tilted this many degrees south (gfx_shared.c sun_tilt) */
    float sun_moon; /* 0: no shadows from the moon, outdoors through the night (gfx_shared.c gfx_sun_light) */
    float sun_game_shading; /* 1: the shadows weighed against the game's own smooth shading (sun_map's Ns); 0 the maps alone */
    float sun_centre; /* 1: the sun's maps cut round the player (sun_fit), whatever the camera does; 0: round the camera's view */
    float sun_gate;   /* one part of the sun's shadows turned off, to find which one changes them (SUN_GATE_*); 0 none */
    float sun_seek;   /* 1: every piece of the zone round the player drawn for the sun's maps whatever the camera sees (gfx_seek_reach) */
    /* The sun's parts switched off one at a time, saved (sun_gate's bits aren't): the cache of casters out of view (sun_cache_off:
     * nothing kept, the caster pass off with it), the world's pieces the game draws after the effects taken as casters for the
     * next frame (sun_cache_late), the sharp map (sun_map), and the cull of casters past a map's sides (beyond_map); 1 on, 0 off */
    float sun_cache, sun_late, sun_sharp, sun_cull;
    float ao_chars;   /* the occlusion's strength on characters as a share of ao's: 1 the same, 0 none (fx_ao, the stencil's mask) */
    float ao_soft_chars; /* ... and how far it's smoothed across a character's own surface, world units each way (fx_blur): 0 none */
    float sun_rings;  /* the sun's shadow maps' taps a pixel (sun_look, 16): fewer past each of three rings out from the camera,
                       * 12, 8, then 6; 1 on, 0 all of them everywhere */
    float sun_ring_spacing; /* ... how far apart the rings are, world units */
    float sun_detail; /* the near map's texels across, 512-8192 (sun_size) */
    float sun_far_hold; /* frames the far map's still layer is kept between redraws (sun_map), 0 every frame; at most 15 */
    float sun_near_hold; /* frames the sharp map's still layer is kept at most (g_still_near, near_stale), 0 the map drawn whole
                          * every frame; at most 600 */
    float sun_mid;    /* the middle map's reach round the player, world units (gfx_sun_mid.h); 0 none */
    float sun_roll;   /* 1: a still layer is drawn again over SUN_SLICES frames, a slice each (gfx_sun_layers.h), not all in one; 0 whole */
    float sun_plants; /* 0: the zone's plants (draw_plant: trees, bushes, grass) cast none of the sun's shadows (sun_draw); 1 they do */
    float sun_plants_still; /* 1: a plant casts the pose it was first seen in, kept still (gfx_sun_cache.h plant_still); 0 as it sways */
    float sun_fade;  /* seconds the zone's shadows the cache lets go of take to fade (cache_let_go); 0 at once */
    float sun_soft_chars; /* the shadows on characters (sun_map_view) blurred this far across them, world units (fx_blur); 0 (the
                           * default): the zone's smoothing (sun_soft_zone) on them as on the rest - their own, narrower, let a
                           * Rock Golem's rocky shading flicker from frame to frame (Sanctuary of Zi'Tah, 2026-11-02) */
    float normalmap;  /* texture packs' normal maps: 0 off (the panel's toggle) */
    float normalmap_strength; /* ... how far they bend the normal: 1 as made, 0 flat, up to 4 */
    float normalmap_view;     /* ... 0 lit, 1 the bent normal as the colour, 2 lit with the maps' Y turned over */
    float normalmap_refl;     /* ... reflections follow them (the detail target, nout_begin) */
    float normalmap_fx;       /* ... so do the occlusion and the sun's shadows */
    float moghouse;           /* how much of the sun's shadows stay indoors (GfxScene.indoors: a Mog House, a zone lit by a fixed
                               * light), 0 none to 1 all */
#ifdef VANACORE
    /* the cel look: ink lines, brightness bands, smoothing */
    float outline, outline_width, outline_depth, outline_crease, outline_fade, bands, band_soft, smooth;
    /* ... the width of the lines on characters (outline_width is the world's), 0 the same (fx_comp, char_line) */
    float outline_width_chars;
    /* ... and 1 (the default) leaves VanaCore's grass out of the lines: none on it, and the world's found as if it weren't there
     * (fx_comp, from the depth before the grass) */
    float outline_nograss;
    /* depth of field around the player; dof_sky 1 leaves the sky (its clouds, the sun and moon) sharp */
    float dof, dof_focus, dof_player, dof_range, dof_far, dof_near, dof_sky;
    /* anti-aliasing: 1 FXAA on the finished scene (runs with the effects off too) */
    float aa;
    /* 1: DLSS's comparison - the scene still drawn small, but stretched as the game stretches it, no DLSS */
    float dlss_compare;
    /* 1: without DLSS too, the finished scene copied at the size it's shown (plain_run) and the names over heads drawn
     * over it at that size, as DLSS has them ([graphics] nameplates.font=sharp, whose letters are the screen's pixels) */
    float names_native;
    /* 1 (the default): with frame generation, the names over heads drawn again onto each frame it makes, each where it
     * would be between the game's frames (fgn_*), not left to frame generation, which shows them where the next frame has
     * them ([graphics] nameplates.framegen) */
    float names_fg;
    /* DLSS 5 (neural rendering) before DLSS, where the helper has it: on, its look (0 default, 1 natural, 2
     * cinematic), its strength, its local tone and structure strengths, the skin's structure strength (-1: its own),
     * and 1 to have it find its own mask */
    float dlss5, dlss5_style, dlss5_intensity, dlss5_tone, dlss5_structure, dlss5_skin, dlss5_mask;
    /* DLSS 5's advanced settings (the /vana window's DLSS 5 > Advanced Settings): its passes (1 to 3, each on the one
     * before's picture) and the later passes' share of the local strengths; its picture blended back with the colour
     * (dlss5_mix: 0 the colour, 1 DLSS 5's picture; dlss5_colour: 0 the colour's hue, 1 DLSS 5's; dlss5_brighten: the most
     * brighter a pixel may end, times, 0 no limit; 1 by default: it restyles, and lights only from the glow it's handed
     * (dlss5_fx_light), since on its own it turned Tu'Lia's bright halls white); 1: DLSS's history reset each frame DLSS 5
     * runs; 1: DLSS 5 made again
     * when its look (style, strengths, mask) changes, once it holds still for a second (dlss_nr_settled) */
    float dlss5_passes, dlss5_taper, dlss5_mix, dlss5_colour, dlss5_brighten, dlss5_rebuild;
    /* the size DLSS 5 runs at, a share of the scene's each way (1 the scene's; 0.75, 0.5 cheaper): its change to the scene
     * laid back onto it at the scene's size (the helper's nrscale) */
    float dlss5_scale;
    /* the models the DLSS helper runs: DLSS's render preset (0 its own choice; 11 K, 13 M, 6 F...) and DLSS 5's */
    float dlss_preset, dlss5_preset;
    /* DLSS 5 and the game's effects (spells, flames, glows: what it draws into the scene after its world). 1: kept out of
     * DLSS 5 - it's handed the scene as it was before them, and they're added back onto its picture before DLSS
     * (dlss_run, the helper's fxadd). 1: its lights - their additive draws (el_emit) lifted above white in the colour
     * DLSS 5 is handed, dlss5_fx_gain times as bright, so it has something to light the scene from */
    float dlss5_fx_out, dlss5_fx_light, dlss5_fx_gain;
    /* ... and the share of that the characters' effects (spells, abilities, weapon skills: el_lantern 3) are taken at:
     * big bright sprites, a sea of white at the flames' gain (0: not lights for it at all); the brightest the glow
     * lifts a pixel to, times white (fx_dlssc; 0: no ceiling); and the glow's exposure: the share of the picture the
     * lights may fill (their mean, fx_dlssm) before the gain eases down in step - a beam that fills the hall gives about
     * the light a crystal does, instead of lighting everything at full gain (0: none) */
    float dlss5_fx_actor, dlss5_fx_cap, dlss5_fx_norm;
    /* 1: the light from the effects (elight, el_run) laid onto DLSS's output after DLSS 5, not into the scene DLSS 5 is
     * handed */
    float dlss5_elight_post;
    /* 1: each moving thing's own motion for DLSS (and FSR, and frame generation): the characters and the game's effects
     * drawn again into a target of their own with where they were the frame before (mo_draw), in place of the camera's
     * motion alone, worked out from the depth behind them. 0 (the default): the camera's motion alone, exact for the static
     * world, and the masks below say where it isn't the thing's own; found by matching draws to the frame before's, the
     * motion was a guess, and each way the match broke was a glitch (an effect's model with frame generation still takes
     * its own: fg_fx_motion). [graphics.effects] dlss_motion_all, in vanacore.ini alone: it was dlss_motion, the /vana
     * window's Motion for characters and effects, and it broke more than it mended, so a 1 saved under that name is dropped */
    float dlss_motion;
    /* with frame generation, the least alpha (or light added) at which an effect's model (more than a quad) takes its own
     * motion (mo_draw): a home point's crystal is faint layers, each mostly under the 0.2 the rest go by; 0 leaves every
     * effect the scene's motion */
    float fg_fx_motion;
    /* 1: the zone's plants (alpha-tested, placed by a world matrix or drawn by the game's vertex shaders: trees, grass, and
     * VanaCore's grass) take their own motion too (mo_draw). 0 (the default) leaves them the depth's: given their own, frame
     * generation tore them, and its optical flow follows their sway itself */
    float dlss_motion_plants;
    /* 1 (the default): the characters take their own motion with dlss_motion off (mo_draw: each part matched to the frame
     * before's nearest of its kind) and stay out of the moving things' mask where they have it. Given the camera's alone, a
     * running character was fetched from where the ground behind it was and blurred, and the mask that stood in for its
     * motion made it flicker */
    float dlss_motion_chars;
    /* 1 (the default): the moving things' redraws into the motion target and the mask are kept and drawn together once the
     * scene is drawn (mo_keep, mo_flush); 0 draws each just after its own draw, as they were at first */
    float dlss_motion_batch;
    /* the upscaler's masks of what isn't the static world (fx_dlssr; dlss_ipc.h reactive, tc): how far DLSS or FSR is to lean on
     * this frame's pixels instead of its history, 0 to 1, over the game's effects (dlss_reactive: every blended draw after the
     * scene - spells, glows, a maw's vortex - found from the scene with and without them, dlss_keep_before) and over what writes
     * depth and moves on its own (dlss_reactive_moving: characters, plants, VanaCore's grass, a boat; mask_draw). The motion
     * handed over is the camera's alone (fx_dlssmv) unless dlss_motion asks for theirs, so without these they're fetched from
     * where the wall behind them was and smear, double or freeze. 0: no mask of that kind */
    float dlss_reactive, dlss_reactive_moving;
    /* reflections on floors and bounce light (screen space, a pass after the composite): strength (0 off), wetness, roughness,
     * how far in world units, quality (0-4: the ray's steps and the blur's taps, a table in gfx_scene_done), the sun's glint;
     * bounce light's strength (0 off), radius and quality (0-4: its samples) */
    float refl, refl_wet, refl_rough, refl_dist, refl_quality, refl_sun, gi, gi_radius, gi_quality;
    /* ... and which kinds of ground reflect, by the zone's floors (gfx_scene_floors, floors.c: the collision's footstep kinds):
     * 0 any (the colour alone), 1 (the default) stone and metal, 2 wood too, 3 anything built (props as well) */
    float refl_floors;
    /* ... and how much the floor's own picture bends them (a fake bump map: its brightness read as height, so grooves and
     * tile edges tilt the reflection), 0 none; fades out with distance (fx_ssr) */
    float refl_bump;
    /* ... and how far a ray that leaves the floor low is tilted up (its rise above the floor's plane, 0 none): seen from a low
     * camera the mirror's rays skim the ground and find only the walls' dark feet far off, and the reflection stopped
     * part way down the floor (fx_ssr) */
    float refl_lift;
    /* the colour grade's zones: midtones and whites (-1..1), soft highlights (0..1: a shoulder in place of the hard clip) and a
     * tint on the lights (-1 cool .. 1 warm); and clarity (local contrast, 0 off). The grade is laid last (grade_last), after
     * every effect; clarity, sharpen and smooth go off with it (grade 0) */
    float midtones, whites, rolloff, light_tint, clarity;
    /* the moonlight laid last (moon_grade) through the Vana'diel night while night is on (GfxScene.night): how far the picture
     * goes to a cool blue-grey, 0 to 1 */
    float night_moon;
    /* the grade's warmth through the Vana'diel day (GfxScene.day_night): this much warmer by day and cooler at night, outdoors */
    float day_night_warmth;
    /* the player's light (player_light, gfx_shared.c gfx_player_light): its strength (0 off, to 2), the pool's radius in world
     * units, its colour (0 white to 1 a torch's orange), and how dark round the pool by day (0 to 1) */
    float player_light, player_light_radius, player_light_warm, player_light_day;
    /* the colour grade's tone, -1..1 (0 leaves it as it is): brightness (a gamma, so black and white stay), and how much
     * the darks and the lights are lifted (or pulled down); and its warmth, -1 cool .. 1 warm */
    float brightness, shadows, highlights, warmth;
    /* the colour grade's hue: every colour turned round the colour wheel this many degrees, -180..180 (0 leaves it) */
    float hue;
    /* the height fog's colour: 0 the zone's own, 1 matched to the sky (fx_skycol), 2 this one (red, green, blue) */
    float fog_mode, fog_cr, fog_cg, fog_cb;
    /* the light from the effects (el_run): strength (0 off), and how far it reaches in world units */
    float elight, elight_radius;
    /* ... and times what while a home point's teleport is under way (GfxScene.warping): its pillars round the character
     * light everything near */
    float elight_warp;
    /* ... and the share of the light a lantern's or torch's flame gives (GfxDraw.el_lantern: its texture is listed in
     * [graphics.effects] elight_lantern_tex), of what it would: 1 as the rest */
    float elight_lantern;
    /* ... and an effect's that gives far more light than it looks to (GfxDraw.el_lantern 2: elight_dim_tex) */
    float elight_dim;
    /* ... and the strength of an effect's on a character - a spell, an ability (GfxDraw.el_lantern 3: d3d8.c
     * character_effect) - in elight's place, whatever elight is (0 with it: the light from the effects is off) */
    float elight_actor;
#endif
    /* VanaCore's glow round trusts and avatars (gfx_glow), whatever the effects: how strong (a multiple of the game's
     * tint's alpha, 1 as it was) and how wide (a multiple of the blur's step) */
    float glow_strength, glow_size, glow_quality;
    /* the sun on the zone by its own smooth normals (vn_replay, sun_map): 1 on, 0 the shadow maps' alone. Always on in the game
     * (not an ini key or a panel option since 2026-10-31: off, a low sun lit rock in hard-edged triangles and walls flickered
     * worse); the tests turn it off to see what the maps alone do */
    float sun_smooth;
    /* the zone's shadows blurred this far across its own surfaces, world units (fx_blur), rounding off the polygons' edges; 0 none */
    float sun_soft_zone;
    float sun_curved; /* 1: the zone's curved rock and ground shaded as the smooth surface their normals describe where they turn
                       * from the sun (zn_draw); 0 by the polygons, as the maps have them */
} FxSettings;
static FxSettings g_fxs;
static GfxTex* g_rts; /* every color target (depth_seen) */
static ID3D12PipelineState* g_present_cas_pso;
#ifdef VANACORE
static ID3D12PipelineState* g_present_layer_pso; /* VanaCore's interface layer over the frame */
static GfxTex* g_present_layer;                  /* ... for this Present */

void gfx_present_layer(GfxTex* layer) { g_present_layer = layer; }
#endif

/* FFXI_D3D12_DEBUG=1 or FFXI_D3D12_GBV=1: the debug layer's messages, printed after each submission */
static ID3D12InfoQueue* g_info;

/* GPU timestamps, two per frame (profile) */
static ID3D12QueryHeap* g_queries;
static ID3D12Resource* g_query_rb;
static uint64_t* g_query_cpu;
static uint64_t g_ts_freq;
/* ... and the scopes' (GPU_QS a frame), with the time each has had since vanacore_gfx_scopes was last asked */
static ID3D12QueryHeap* g_sq;
static ID3D12Resource* g_sq_rb;
static uint64_t* g_sq_cpu;
static uint64_t g_scope_ns[GS_N];
static uint64_t g_part_ns[GS_N][GP_N];
static uint32_t g_scope_frames;
static uint32_t g_scope_full; /* ... of them, the frames whose parts' timestamps ran out (Frame.full) */
/* the scope and part the GPU's work is in now (gpu_scope, gpu_part), and the part from a command list's end till the next
 * starts (GP_GAP; GP_WAIT_* for the one list before the queue waits on the DLSS helper) */
static int g_gs_scope, g_gs_part, g_gap_part = GP_GAP;

/* The composite's features, each measured by its draw without it (vanacore_gfx_measure): CF_ALL the draw as it is,
 * CF_NONE with them all off */
enum
{
    CF_ALL,
    CF_SHADE, /* the occlusion and shadows laid on */
    CF_LINES, /* the cel look's ink lines */
    CF_CREASE, /* ... their creases */
    CF_BANDS,
    CF_FOG,
    CF_ZONE,  /* VanaCore's zone lines */
    CF_BLOOM,
    CF_RAYS,
    CF_NONE,
    CF_N
};
static struct
{
    uint64_t until;   /* measuring till this frame (g_serial) */
    int next;         /* the feature to measure next */
    double ns[CF_N], n[CF_N]; /* each one's draws' time a frame, and its frames, both fading (x0.9 a frame of it) */
    uint64_t at[CF_N];  /* the last frame it was measured */
    uint64_t off[CF_N]; /* ... and was off already (so it wasn't: it costs nothing) */
} g_meas;

/* --- small hash maps (key bytes -> pointer) ------------------------------------------------------------ */
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

/* Key bytes to 64 bits, four bytes at a time in two 32-bit lanes (MurmurHash3's and FNV-1a's
 * steps): a byte-at-a-time 64-bit FNV-1a costs a 32-bit x86 a multiply helper call per byte, and a
 * draw's pipeline key is ~270 bytes. The low lane picks the bucket. */
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

/* Keys of one size to a number (0: none), the keys in the map's own table. The shadow cache's maps (its cells,
 * cache_link; its copies, snap_find) are made again whole every 256 frames (sun_cache_trim, snap_trim): a Map's
 * malloc and free of each key were thousands of heap calls in that one frame - 150-250 ms on a PC whose heap calls
 * went through a compatibility mode's shims (AcLayers.dll, 2026-10-23). Emptied, it keeps its table. */
typedef struct KeySlot
{
    uint64_t hash; /* 0: empty */
    uint32_t v;
} KeySlot; /* (its key right after it) */

typedef struct KeyMap
{
    uint8_t* e; /* cap slots of stride bytes */
    uint32_t cap, n, stride;
} KeyMap;

static KeySlot* kmap_at(const KeyMap* m, uint32_t i)
{
    return (KeySlot*)(m->e + (size_t)i * m->stride);
}

/* the slot of key (klen bytes, its hash h); NULL: not in it */
static KeySlot* kmap_find(const KeyMap* m, const void* key, size_t klen, uint64_t h)
{
    if (!m->cap)
        return NULL;
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        KeySlot* s = kmap_at(m, i);
        if (!s->hash)
            return NULL;
        if (s->hash == h && !memcmp(s + 1, key, klen))
            return s;
    }
}

static uint32_t kmap_get(const KeyMap* m, const void* key, size_t klen)
{
    const KeySlot* s = kmap_find(m, key, klen, fnv(key, klen));
    return s ? s->v : 0;
}

/* a table of cap slots (a power of two) for keys of klen bytes, what m held put in again */
static void kmap_table(KeyMap* m, uint32_t cap, size_t klen)
{
    const KeyMap old = *m;
    m->stride = (uint32_t)(sizeof(KeySlot) + ((klen + 7) & ~(size_t)7));
    m->cap = cap, m->n = 0;
    m->e = (uint8_t*)calloc(cap, m->stride);
    for (uint32_t i = 0; i < old.cap; ++i)
    {
        const KeySlot* o = kmap_at(&old, i);
        if (!o->hash)
            continue;
        uint32_t j = (uint32_t)o->hash & (cap - 1);
        while (kmap_at(m, j)->hash)
            j = (j + 1) & (cap - 1);
        memcpy(kmap_at(m, j), o, old.stride);
        m->n++;
    }
    free(old.e);
}

/* key -> v: its number changed, or it put in */
static void kmap_set(KeyMap* m, const void* key, size_t klen, uint32_t v)
{
    const uint64_t h = fnv(key, klen);
    KeySlot* s = kmap_find(m, key, klen, h);
    if (!s)
    {
        if ((m->n + 1) * 2 > m->cap)
            kmap_table(m, m->cap ? m->cap * 2 : 256, klen);
        uint32_t j = (uint32_t)h & (m->cap - 1);
        while (kmap_at(m, j)->hash)
            j = (j + 1) & (m->cap - 1);
        s = kmap_at(m, j);
        s->hash = h;
        memcpy(s + 1, key, klen);
        m->n++;
    }
    s->v = v;
}

/* emptied for about `want` keys: its table kept, unless that's too small or four times too big */
static void kmap_clear(KeyMap* m, uint32_t want, size_t klen)
{
    uint32_t cap = 256;
    while (cap < want * 2 && cap < (1u << 30))
        cap *= 2;
    if (m->cap < cap || m->cap > cap * 4)
    {
        free(m->e);
        memset(m, 0, sizeof *m);
        kmap_table(m, cap, klen);
        return;
    }
    memset(m->e, 0, (size_t)m->cap * m->stride);
    m->n = 0;
}

static Map g_pipes, g_samplers;
static void sun_forget(GfxBuf* b, GfxTex* t);
static void forget_note(const GfxBuf* b, const GfxTex* t, const char* what);

/* --- the frame profile ------------------------------------------------------------------------------------ */
int gfx_profiling;
int gfx_watching;
uint32_t gfx_profile_epoch;

typedef struct Prof
{
    uint64_t frames, draws, bytes, pipelines, front_ns, draw_ns, sem_ns, drawable_ns, present_ns, last_ns, since_ns;
    uint64_t redraws; /* the moving things drawn again for DLSS's or FSR's motion and masks (mo_encode, mask_draw) */
    uint64_t skips[GFX_NSKIPS];
    uint64_t shim_ns, probe_ns;
} Prof;

static Prof g_prof;
#ifdef VANACORE
#include "vanacore_gfx.h"
#include "zonemesh.h" /* zonemesh_resident: the zone's own meshes in place of the caster pass (gfx_sun_cache.h gfx_seek_reach) */
#include "grass.h"
static int g_profile_env;              /* the profile as [graphics] profile had it at the start */
static Prof g_prof_prev;               /* g_prof at the last frame: this frame's, the difference */
static uint64_t g_gpu_prev;            /* ... g_gpu_ns */
static uint64_t g_helper_wait_ns;      /* this frame's waits on the DLSS helper */
static VanaGfxFrame g_frame_stats;     /* the last frame's */
static int g_frame_stats_ok;
#endif

uint64_t gfx_now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
}

/* --- the flicker watch -------------------------------------------------------------------------------------
 * A frame that differs from the one before in what the scene effects follow - whether they ran and over
 * how much of the world, the camera and sun they shade and light by, the frame before that they blend
 * with, draws left out while their pipelines build - goes to the log as it happens ("flicker watch:
 * frame N: ..."; N is the first column of trace_gfx.txt): for a one-frame flicker seen once in a while,
 * what that frame did differently. Bursts are held to 20 lines, then one a second. The first line after
 * a quiet minute also has the traces written a second later, with the frames before and after it
 * (flicker.txt and flicker_gfx.txt beside trace.txt: d3d8.c's scene_trace). */
#ifdef VANACORE
#include "gfx_backend.h"
/* the frame of the back end that's drawing: the watch is one for both (gfx_backend.h) */
static uint64_t watch_serial(void) { return gfx_selected_d3d11() ? gfx11_backend.frame() : g_serial; }
#else
#define watch_serial() g_serial
#endif
static struct
{
    double tokens;
    uint64_t last_ns, line_ns, held;
    uint64_t dump_at;           /* the frame the traces are written at (0: none due) */
    uint32_t skips[GFX_NSKIPS]; /* draws dropped this frame (gfx_prof_skip) */
    /* the frame before's, to compare with (fx_watch_frame) */
    uint64_t serial;
    uint32_t ran, hist, fog, depth_kind, cam_draw;
    const void* depth;
    float sun[3], cam[3], smap, rays;
    float far_r, near_r; /* the maps' sphere radii (sun_map) */
} g_watch;

void gfx_watch(const char* fmt, ...)
{
    if (!gfx_profiling && !gfx_watching) /* up to a line a second: only with [graphics] profile or a /vana debug recording */
        return;
    uint64_t now = gfx_now_ns();
    g_watch.tokens = g_watch.last_ns ? fmin(20.0, g_watch.tokens + (double)(now - g_watch.last_ns) * 1e-9) : 20.0;
    g_watch.last_ns = now;
    if (g_watch.tokens < 1.0)
    {
        g_watch.held++;
        return;
    }
    g_watch.tokens -= 1.0;
    if (!g_watch.line_ns || now - g_watch.line_ns > 60000000000ull)
        g_watch.dump_at = watch_serial() + 60;
    g_watch.line_ns = now;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_watch.held)
        fprintf(stderr, "[recomp] gfx: flicker watch: frame %llu: %s (%llu lines held back before it)\n",
            (unsigned long long)watch_serial(), line, (unsigned long long)g_watch.held);
    else
        fprintf(stderr, "[recomp] gfx: flicker watch: frame %llu: %s\n", (unsigned long long)watch_serial(), line);
    g_watch.held = 0;
}

uint64_t gfx_frame(void) { return g_serial; }

int gfx_watch_dump_due(void)
{
    if (!g_watch.dump_at || watch_serial() < g_watch.dump_at)
        return 0;
    g_watch.dump_at = 0;
    return 1;
}

/* at the frame's end: its draws left out, and why */
static void watch_skips(void)
{
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "pipeline still building", "no target" };
    char why[256];
    int at = 0;
    uint32_t n = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_watch.skips[i])
        {
            at += snprintf(why + at, sizeof why - (size_t)at, "%s%s %u", n ? ", " : "", WHY[i], g_watch.skips[i]);
            n += g_watch.skips[i];
            if (at >= (int)sizeof why)
                break;
        }
    if (n)
        gfx_watch("%u draws left out (%s): what they draw is missing from this frame", n, why);
    memset(g_watch.skips, 0, sizeof g_watch.skips);
}

#ifdef VANACORE
void gfx_watch_skips(void) { watch_skips(); }
#endif

void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason)
{
    if (reason >= 0 && reason < GFX_NSKIPS)
        g_prof.skips[reason]++, g_watch.skips[reason]++;
}

static DWORD g_present_thread;

void gfx_prof_shim(uint64_t ns)
{
    if (GetCurrentThreadId() == g_present_thread)
        g_prof.shim_ns += ns;
}

/* at each Present: every two seconds, the average frame and where it went */
static void prof_frame(uint64_t present_start)
{
    static uint32_t epoch;
    if (epoch != gfx_profile_epoch)
    {
        /* the profile just came on: what was counted while it was off (draws, uploads, skips) isn't this window's,
         * and its window starts now - not when it went off, which made the first line one long frame */
        epoch = gfx_profile_epoch;
        memset(&g_prof, 0, sizeof g_prof);
        g_gpu_ns = g_gpu_sun_ns = g_gpu_fx_ns = 0;
#ifdef VANACORE
        g_prof_prev = g_prof, g_gpu_prev = 0, g_helper_wait_ns = 0;
#endif
    }
    uint64_t now = gfx_now_ns();
    g_prof.present_ns += now - present_start;
    g_prof.frames++;
#ifdef VANACORE
    {
        const Prof* a = &g_prof;
        const Prof* b = &g_prof_prev;
        VanaGfxFrame* o = &g_frame_stats;
        o->d3d_ms = (float)((double)(a->front_ns - b->front_ns) * 1e-6);
        o->encode_ms = (float)((double)(a->draw_ns - b->draw_ns) * 1e-6);
        o->present_ms = (float)((double)(a->present_ns - b->present_ns) * 1e-6);
        o->gpu_wait_ms = (float)((double)(a->sem_ns - b->sem_ns) * 1e-6);
        o->swap_ms = (float)((double)(a->drawable_ns - b->drawable_ns) * 1e-6);
        o->helper_wait_ms = (float)((double)g_helper_wait_ns * 1e-6);
        o->gpu_ms = g_gpu_ns >= g_gpu_prev ? (float)((double)(g_gpu_ns - g_gpu_prev) * 1e-6) : 0.0f;
        o->draws = (uint32_t)(a->draws - b->draws);
        o->upload_kb = (uint32_t)((a->bytes - b->bytes) / 1024u);
        o->pipelines = (uint32_t)(a->pipelines - b->pipelines);
        o->redrawn = (uint32_t)(a->redraws - b->redraws);
        uint64_t skipped = 0;
        for (int i = 0; i < GFX_NSKIPS; ++i)
            skipped += a->skips[i] - b->skips[i];
        o->skipped = (uint32_t)skipped;
        {
            uint64_t grass_ns;
            grass_made_take(&grass_ns, &o->grass_tufts);
            o->grass_ms = (float)((double)grass_ns * 1e-6);
        }
        g_frame_stats_ok = 1;
        g_prof_prev = g_prof;
        g_gpu_prev = g_gpu_ns;
        g_helper_wait_ns = 0;
    }
#endif
    if (!g_prof.since_ns)
        g_prof.since_ns = now;
    if (now - g_prof.since_ns < 2000000000ull)
        return;
    double f = (double)g_prof.frames, ms = 1e-6 / f;
    double frame = (double)(now - g_prof.since_ns) * ms;
    double front = (double)g_prof.front_ns * ms, enc = (double)g_prof.draw_ns * ms;
    double sem = (double)g_prof.sem_ns * ms, drw = (double)g_prof.drawable_ns * ms, pres = (double)g_prof.present_ns * ms;
    double shims = (double)g_prof.shim_ns * ms, probe = (double)g_prof.probe_ns * ms;
    double gpu = (double)g_gpu_ns * ms, gsun = (double)g_gpu_sun_ns * ms, gfx = (double)g_gpu_fx_ns * ms;
    g_gpu_ns = g_gpu_sun_ns = g_gpu_fx_ns = 0;
    if (shims > 0)
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game code %.2f + API calls %.2f (draws %.2f [encode %.2f], probe wait %.2f, "
            "present %.2f [swap chain %.2f], other %.2f) | GPU %.2f ms (sun maps %.2f, other effects %.2f) | %.0f draws, %.0f KB up, "
            "%llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - shims, shims, front, enc, probe, pres, drw,
            shims - front - probe - pres, gpu, gsun, gfx, (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0,
            (unsigned long long)g_prof.pipelines);
    else
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode %.2f) + present %.2f (gpu wait %.2f, swap chain %.2f) | "
            "GPU %.2f ms (sun maps %.2f, other effects %.2f) | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, enc, pres, sem, drw, gpu, gsun, gfx,
            (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "pipeline not ready", "no target" };
    int any = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_prof.skips[i])
            fprintf(stderr, "%s%s %llu", any++ ? ", " : "[gfx]   skipped draws (2 s): ", WHY[i], (unsigned long long)g_prof.skips[i]);
    if (any)
        fprintf(stderr, "\n");
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
#ifdef VANACORE
    g_prof_prev = g_prof;
    g_gpu_prev = 0;
#endif
}

static volatile LONG g_failures;

uint32_t gfx_failures(void) { return (uint32_t)g_failures; }

/* --- descriptor heaps ------------------------------------------------------------------------------------- */
static int heap_init(Heap* h, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t n, int visible, uint32_t reserved)
{
    D3D12_DESCRIPTOR_HEAP_DESC d = { type, n, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_dev, &d, &IID_ID3D12DescriptorHeap, (void**)&h->heap)))
        return 0;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h->heap, &h->cpu);
    if (visible)
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h->heap, &h->gpu);
    h->inc = ID3D12Device_GetDescriptorHandleIncrementSize(g_dev, type);
    h->free_slots = (int32_t*)malloc(sizeof(int32_t) * n);
    h->nfree = 0;
    for (uint32_t i = n; i-- > reserved;) /* low slots first */
        h->free_slots[h->nfree++] = (int32_t)i;
    return 1;
}

static int32_t heap_alloc(Heap* h)
{
    return h->nfree ? h->free_slots[--h->nfree] : -1;
}

static void heap_free(Heap* h, int32_t slot)
{
    if (slot >= 0)
        h->free_slots[h->nfree++] = slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(const Heap* h, int32_t slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c = { h->cpu.ptr + (SIZE_T)slot * h->inc };
    return c;
}

/* --- lifetimes: what the GPU may still read is released once the fence passes it --------------------------- */
typedef struct Dead
{
    IUnknown* obj;
    Heap* heap; /* or a descriptor slot of this heap */
    int32_t slot;
    uint64_t fence;
    Slab* slab; /* or a block of this slab (a static buffer's), at offset slot */
} Dead;

static Dead* g_dead;
static uint32_t g_ndead, g_cap_dead;
static void slab_put(Slab* s, uint32_t off);

static uint64_t completed(void)
{
    return ID3D12Fence_GetCompletedValue(g_fence);
}

/* the submission the commands being recorded now will belong to */
static uint64_t pending(void)
{
    return g_fence_value + 1;
}

static void defer(IUnknown* obj, Heap* heap, int32_t slot)
{
    if (!obj && (!heap || slot < 0))
        return;
    if (g_ndead == g_cap_dead)
    {
        g_cap_dead = g_cap_dead ? g_cap_dead * 2 : 256;
        g_dead = (Dead*)realloc(g_dead, g_cap_dead * sizeof *g_dead);
    }
    g_dead[g_ndead++] = (Dead){ obj, heap, slot, pending() };
}

/* a static buffer's block (gfx_buf_destroy): back in its slab's free list once the GPU is past what may read it */
static void defer_block(Slab* s, uint32_t off)
{
    if (g_ndead == g_cap_dead)
    {
        g_cap_dead = g_cap_dead ? g_cap_dead * 2 : 256;
        g_dead = (Dead*)realloc(g_dead, g_cap_dead * sizeof *g_dead);
    }
    g_dead[g_ndead++] = (Dead){ NULL, NULL, (int32_t)off, pending(), s };
}

static void collect(void)
{
    uint64_t done = completed();
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_ndead; ++i)
    {
        Dead* d = &g_dead[i];
        if (d->fence <= done)
        {
            if (d->slab)
                slab_put(d->slab, (uint32_t)d->slot);
            else if (d->obj)
                IUnknown_Release(d->obj);
            else
            {
                if (d->heap == &g_srv && g_srv_owner && (uint32_t)d->slot < SRV_HEAP)
                    g_srv_owner[d->slot] = NULL;
                heap_free(d->heap, d->slot);
            }
        }
        else
            g_dead[k++] = *d;
    }
    g_ndead = k;
}

/* everything deferred released now, whatever its fence (gfx_shutdown: the GPU is past it all, or the device is gone) */
static void collect_all(void)
{
    for (uint32_t i = 0; i < g_ndead; ++i)
    {
        Dead* d = &g_dead[i];
        if (d->slab)
            slab_put(d->slab, (uint32_t)d->slot);
        else if (d->obj)
            IUnknown_Release(d->obj);
        else
        {
            if (d->heap == &g_srv && g_srv_owner && (uint32_t)d->slot < SRV_HEAP)
                g_srv_owner[d->slot] = NULL;
            heap_free(d->heap, d->slot);
        }
    }
    g_ndead = 0;
}

/* each kind of message printed MESSAGE_REPEATS times: a mistake made every draw (GPU-based validation's especially)
 * would otherwise fill the log */
#define MESSAGE_REPEATS 8
static void print_messages(void)
{
    static uint16_t seen[2048]; /* by D3D12_MESSAGE_ID (the last for any past it) */
    UINT64 n = ID3D12InfoQueue_GetNumStoredMessages(g_info);
    for (UINT64 i = 0; i < n; ++i)
    {
        SIZE_T len = 0;
        ID3D12InfoQueue_GetMessage(g_info, i, NULL, &len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (m && SUCCEEDED(ID3D12InfoQueue_GetMessage(g_info, i, m, &len)))
        {
            uint16_t* c = &seen[(unsigned)m->ID < 2048u ? (unsigned)m->ID : 2047u];
            if (*c < MESSAGE_REPEATS)
                fprintf(stderr, "[d3d12] %s%s\n", m->pDescription, ++*c == MESSAGE_REPEATS ? " (the rest of these aren't logged)" : "");
        }
        free(m);
    }
    ID3D12InfoQueue_ClearStoredMessages(g_info);
}

/* --- the device lost, and GPU crash data (FFXI_D3D12_DRED: VanaCore's [logging] gpu) -------------------------
 * The device lost (a driver timeout - AMD's "driver timeout" box - a bad address, the driver restarting), why is
 * logged once (gpu_lost), from Present, a fence wait or the game crashing after it. With FFXI_D3D12_DRED=1 Windows'
 * Device Removed Extended Data is on too (make_device): the driver keeps how far the GPU got through each command
 * list (auto-breadcrumbs) and, on a page fault, the address and what was at it; each pass is marked in the list
 * (gpu_scope: scene, sun shadow maps, bloom, ...), so the log says which pass and which operation the card stopped
 * at. Each operation is recorded, a little slower: off unless asked for. */
static int g_dred;      /* on for this device */
static int g_lost_said; /* the loss logged already */
static char g_card[300]; /* the card and its driver, as the start-up line says, for the loss report */
static int g_dred_scope; /* the pass marked last (a GS_*) */

typedef void (*GpuLine)(const char* line);

static void gpu_say(GpuLine out, const char* fmt, ...)
{
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    out(buf);
}

static void gpu_log_line(const char* line)
{
    fprintf(stderr, "[recomp] gfx: %s\n", line);
}

static const wchar_t* const k_pass_mark[] = { L"pass: the game's scene", L"pass: sun shadow maps", L"pass: effects setup",
    L"pass: ambient occlusion", L"pass: bloom", L"pass: god rays", L"pass: composite", L"pass: reflections",
    L"pass: depth of field", L"pass: FXAA", L"pass: DLSS", L"pass: the interface", L"pass: the composite measured (profile)" };
typedef char pass_marks_match_the_scopes[sizeof k_pass_mark / sizeof k_pass_mark[0] == GS_N ? 1 : -1];

/* Each draw is marked too: a mark of its own just before it, and here what the draw was, list by list (the last
 * NOTE_LISTS sent), so the report names the draw the card stopped at. The marks' own text comes back only where
 * Windows keeps breadcrumb contexts (the 2026-10-09 NVIDIA reports had none: "no pass marked"), so the report finds
 * a note by counting marks: every mark in a list, a pass's or a draw's, has one, in order. */
enum { NOTE_PASS, NOTE_DRAW };
enum { DN_INDEXED = 1, DN_RHW = 2, DN_VS = 4, DN_PS = 8, DN_DETAIL = 16 };
typedef struct DrawNote
{
    const char* what;                 /* NOTE_DRAW's: whose (a string that lives on) */
    uint8_t kind, scope, prim, flags; /* NOTE_*; the pass (GS_*, GS_N: the frame onto the window); the game's GFX_*; DN_* */
    uint32_t count, start;            /* the vertices (or indices) drawn, from */
    uint32_t vbytes;                  /* the game's: stream 0's bytes from where the draw reads it */
    uint32_t tw, th, tfmt;            /* stage 0's texture (the game's) */
    uint32_t rw, rh, rfmt;            /* the target */
} DrawNote;
#define NOTE_LISTS 64
static struct
{
    DrawNote* n;
    uint32_t count, cap;
    int broken; /* a note it couldn't keep: its marks and notes no longer pair */
} g_notes[NOTE_LISTS];
static uint32_t g_notes_sent; /* lists sent: the one recorded now keeps its notes in g_notes[g_notes_sent % NOTE_LISTS] */

static DrawNote* note_add(int kind)
{
    uint32_t i = g_notes_sent % NOTE_LISTS;
    if (g_notes[i].count == g_notes[i].cap)
    {
        uint32_t cap = g_notes[i].cap ? g_notes[i].cap * 2 : 512;
        DrawNote* n = (DrawNote*)realloc(g_notes[i].n, cap * sizeof *n);
        if (!n)
        {
            g_notes[i].broken = 1;
            return NULL;
        }
        g_notes[i].n = n, g_notes[i].cap = cap;
    }
    DrawNote* n = &g_notes[i].n[g_notes[i].count++];
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind, n->scope = (uint8_t)g_dred_scope;
    return n;
}

/* the mark DRED keeps with the breadcrumbs for a pass (scope; GS_N: the frame onto the window), a wide string: PIX's
 * unicode marker */
static void dred_mark(ID3D12GraphicsCommandList* l, int scope)
{
    const wchar_t* what = scope < GS_N ? k_pass_mark[scope] : L"pass: the frame onto the window";
    ID3D12GraphicsCommandList_SetMarker(l, 0, what, (UINT)((wcslen(what) + 1) * sizeof(wchar_t)));
    DrawNote* n = note_add(NOTE_PASS);
    if (n)
        n->scope = (uint8_t)scope;
}

/* a draw's mark, just before it (DRED on): its note, for the caller to fill in, or NULL */
static DrawNote* dred_draw(ID3D12GraphicsCommandList* l, const char* what, uint32_t count)
{
    if (!g_dred)
        return NULL;
    ID3D12GraphicsCommandList_SetMarker(l, 0, L"draw", (UINT)sizeof L"draw");
    DrawNote* n = note_add(NOTE_DRAW);
    if (n)
        n->what = what, n->count = count;
    return n;
}

static const char* removed_reason(HRESULT hr)
{
    switch ((unsigned long)hr)
    {
    case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG: the graphics card took too long on its work (a driver timeout)";
    case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED: the graphics card went away, or its driver was updated or restarted";
    case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET: the graphics card was reset (badly formed commands)";
    case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR: the graphics driver failed";
    case 0x887A0001: return "DXGI_ERROR_INVALID_CALL: the graphics card was given something invalid";
    case 0x8007000E: return "E_OUTOFMEMORY: out of graphics memory";
    default: return "";
    }
}

static const char* breadcrumb_op(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "mark";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "begin event";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "end event";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "draw";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "draw indexed";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "copy buffer";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "copy texture";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "copy resource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "resolve";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "clear target";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "clear depth";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "barrier";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "present";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "resolve queries";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "begin submission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "end submission";
    case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "write buffer";
    case 46: return "list begins"; /* D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST (not in older SDKs) */
    default: return NULL;
    }
}

static const char* allocation_type(D3D12_DRED_ALLOCATION_TYPE t)
{
    switch (t)
    {
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor heap";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command list";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command allocator";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "query heap";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "fence";
    default: return "object";
    }
}

/* the mark (pass) in force at operation `at`: the last at or before it */
static const wchar_t* breadcrumb_mark(const D3D12_AUTO_BREADCRUMB_NODE1* n, UINT at)
{
    const wchar_t* mark = NULL;
    UINT best = 0;
    for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c)
    {
        const D3D12_DRED_BREADCRUMB_CONTEXT* x = &n->pBreadcrumbContexts[c];
        if (x->BreadcrumbIndex <= at && (!mark || x->BreadcrumbIndex >= best) && x->pContextString)
            mark = x->pContextString, best = x->BreadcrumbIndex;
    }
    return mark;
}

static const char* prim_name(uint32_t p)
{
    switch (p)
    {
    case GFX_POINTLIST: return "points";
    case GFX_LINELIST: return "lines";
    case GFX_LINESTRIP: return "a line strip";
    case GFX_TRIANGLELIST: return "triangles";
    case GFX_TRIANGLESTRIP: return "a triangle strip";
    case GFX_TRIANGLEFAN: return "a triangle fan";
    default: return "?";
    }
}

static void note_say(GpuLine out, const char* head, const DrawNote* n)
{
    char what[96] = "", tex[48] = "", onto[48] = "";
    if (n->prim) /* the game's */
        snprintf(what, sizeof what, " (%s, %s from %u; %s, %s; stream 0 %u bytes)", prim_name(n->prim),
            n->flags & DN_INDEXED ? "indexed" : "vertices", n->start,
            n->flags & DN_VS ? "a vertex shader" : n->flags & DN_RHW ? "pre-transformed" : "fixed function",
            n->flags & DN_PS ? "a pixel shader" : "texture stages", n->vbytes);
    if (n->tw)
        snprintf(tex, sizeof tex, ", texture %ux%u format %u", n->tw, n->th, n->tfmt);
    if (n->rw)
        snprintf(onto, sizeof onto, n->rfmt ? ", onto %ux%u format %u" : ", onto %ux%u", n->rw, n->rh, n->rfmt);
    gpu_say(out, "GPU:   %s %s: %u %s%s%s%s", head, n->what ? n->what : "?", n->count,
        n->flags & DN_INDEXED ? "indices" : "vertices", what, tex, onto);
}

static UINT node_marks(const D3D12_AUTO_BREADCRUMB_NODE1* node)
{
    UINT marks = 0;
    for (UINT i = 0; i < node->BreadcrumbCount; ++i)
        marks += node->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_SETMARKER;
    return marks;
}

static int node_ours(const D3D12_AUTO_BREADCRUMB_NODE1* node)
{
    return node->pCommandListDebugNameW && !wcscmp(node->pCommandListDebugNameW, L"VanaCore's frame");
}

/* How many lists of ours were sent after the last one DRED holds. The loss is found at Present, so the lists sent
 * between the card stopping and that Present count in g_notes_sent but never reach DRED (a 2026-10-17 RTX 5080
 * report: "13 marks, 7 notes"). The shift where every list of ours DRED holds has as many marks as its notes; -1 if
 * none fits. */
static int notes_skip(const D3D12_AUTO_BREADCRUMB_NODE1* head, uint32_t vcount)
{
    for (uint32_t skip = 0; skip < NOTE_LISTS; ++skip)
    {
        uint32_t vindex = 0, checked = 0;
        int fits = 1;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = head; n && vindex < vcount && fits; n = n->pNext)
        {
            if (!node_ours(n))
                continue;
            const uint32_t back = vcount - vindex++ + skip; /* 1: the last list sent */
            if (back > g_notes_sent)
                fits = 0; /* more lists than were sent */
            else if (back < NOTE_LISTS)
            {
                const uint32_t slot = (g_notes_sent - back) % NOTE_LISTS;
                fits = !g_notes[slot].broken && g_notes[slot].count == node_marks(n);
                checked++;
            }
        }
        if (fits && checked)
            return (int)skip;
    }
    return -1;
}

/* The draw the card stopped at, from the notes (DrawNote): node is the list of ours sent `back` before the last (1:
 * the last); its marks, counted up to the operation it stopped at, give the note. */
static void notes_report(const D3D12_AUTO_BREADCRUMB_NODE1* node, UINT done, uint32_t back, GpuLine out)
{
    if (back >= NOTE_LISTS || back > g_notes_sent)
    {
        gpu_say(out, "GPU:   (its draws: not kept, too long ago)");
        return;
    }
    const uint32_t slot = (g_notes_sent - back) % NOTE_LISTS;
    UINT marks = 0, upto = 0;
    for (UINT i = 0; i < node->BreadcrumbCount; ++i)
        if (node->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_SETMARKER)
        {
            marks++;
            if (i <= done)
                upto = marks;
        }
    if (g_notes[slot].broken || marks != g_notes[slot].count || upto == 0)
    {
        gpu_say(out, "GPU:   (its draws: %u marks, %u notes - they don't pair)", marks, g_notes[slot].count);
        return;
    }
    const DrawNote* nt = g_notes[slot].n;
    uint32_t at = upto - 1, pass = at;
    while (pass > 0 && nt[pass].kind != NOTE_PASS)
        pass--;
    const int scope = nt[pass].kind == NOTE_PASS ? nt[pass].scope : nt[at].scope;
    gpu_say(out, "GPU:   in %ls", scope < GS_N ? k_pass_mark[scope] : L"pass: the frame onto the window");
    if (nt[at].kind == NOTE_DRAW)
        note_say(out, "the draw it stopped at:", &nt[at]);
    else
        gpu_say(out, "GPU:   before that pass's first draw");
    int after = 0;
    for (uint32_t i = at + 1; i < g_notes[slot].count && after < 3; ++i)
        if (nt[i].kind == NOTE_DRAW)
            note_say(out, after++ ? "and then" : "then", &nt[i]);
}

static void breadcrumbs_report(const D3D12_AUTO_BREADCRUMB_NODE1* n, GpuLine out)
{
    /* ours (not DXGI's own): matched to the lists sent, the last of them the last sent */
    uint32_t vcount = 0, vindex = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE1* v = n; v && vcount < 256; v = v->pNext)
        if (node_ours(v))
            vcount++;
    const int skip = notes_skip(n, vcount);
    if (skip > 0)
        gpu_say(out, "GPU: %d list(s) of ours sent after the card stopped (they never reached it)", skip);
    int lists = 0, unfinished = 0;
    for (; n && lists < 256; n = n->pNext, ++lists)
    {
        const int ours = node_ours(n);
        if (ours)
            vindex++;
        const UINT count = n->BreadcrumbCount;
        const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (done >= count)
            continue; /* finished: not where it stopped */
        if (++unfinished > 3)
            continue;
        const wchar_t* name = n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"(unnamed)";
        const wchar_t* queue = n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"(unnamed)";
        gpu_say(out, "GPU: command list %ls on queue %ls: %u of its %u operations done%s", name, queue, done, count,
            done == 0 ? " (not started)" : "");
        if (done == 0)
            continue;
        const wchar_t* pass = breadcrumb_mark(n, done);
        if (pass || !ours) /* else the notes say (notes_report) */
            gpu_say(out, "GPU:   it stopped in %ls", pass ? pass : L"(no pass marked)");
        for (UINT i = done > 10 ? done - 10 : 0; i < count && i <= done + 3; ++i)
        {
            const char* op = breadcrumb_op(n->pCommandHistory[i]);
            const wchar_t* mark = NULL;
            for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c)
                if (n->pBreadcrumbContexts[c].BreadcrumbIndex == i)
                    mark = n->pBreadcrumbContexts[c].pContextString;
            char opn[24];
            if (!op)
                snprintf(opn, sizeof opn, "op %d", (int)n->pCommandHistory[i]), op = opn;
            gpu_say(out, "GPU:   %s %5u %s%ls%ls", i < done ? "done   " : i == done ? "STOPPED" : "not yet", i, op,
                mark ? L" - " : L"", mark ? mark : L"");
        }
        if (ours) /* no shift fitting: as recorded, and the report says they don't pair */
            notes_report(n, done, vcount - (vindex - 1) + (skip > 0 ? (uint32_t)skip : 0), out);
    }
    gpu_say(out, "GPU: %d command lists recorded, %d not finished", lists, unfinished);
}

/* breadcrumbs_report on made-up breadcrumbs, for VanaCore.GfxTest: two lists of ours sent, DXGI's between them, the
 * first stopped at its first draw (as the 2026-10-09 NVIDIA reports had it), then `after` more sent that never reached
 * DRED (sent after the card stopped, before Present found it). Its lines go to out; 0 (none) with DRED on, whose notes
 * these would take the place of. */
int vanacore_gfx_dred_notes_test(void (*out)(const char* line), int after)
{
    if (g_dred || after < 0 || after > 8)
        return 0;
    const uint32_t sent_was = g_notes_sent;
    for (int l = 0; l < 2 + after; ++l)
        g_notes[(7 + l) % NOTE_LISTS].count = 0, g_notes[(7 + l) % NOTE_LISTS].broken = 0;
    g_notes_sent = 7;
    DrawNote* nt = note_add(NOTE_PASS);
    nt->scope = GS_SCENE;
    nt = note_add(NOTE_DRAW);
    nt->what = "the game's", nt->count = 1234, nt->start = 7, nt->prim = GFX_TRIANGLESTRIP, nt->vbytes = 99999;
    nt->tw = nt->th = 256, nt->tfmt = 21, nt->rw = nt->rh = 2048, nt->rfmt = 21;
    nt = note_add(NOTE_DRAW);
    nt->what = "the game's", nt->count = 6, nt->prim = GFX_TRIANGLELIST;
    g_notes_sent = 8;
    nt = note_add(NOTE_PASS);
    nt->scope = GS_UI;
    nt = note_add(NOTE_DRAW);
    nt->what = "the frame rate", nt->count = 4;
    for (int l = 0; l < after; ++l) /* marks unlike 7's and 8's, as the next frame's lists' would be */
    {
        g_notes_sent = 9 + (uint32_t)l;
        for (int k = l % 2 ? 1 : 4; k > 0; --k)
            note_add(NOTE_DRAW)->what = "a list sent after";
    }
    g_notes_sent = 9 + (uint32_t)after;
    const D3D12_AUTO_BREADCRUMB_OP M = D3D12_AUTO_BREADCRUMB_OP_SETMARKER, B = D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
        D = D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED, P = D3D12_AUTO_BREADCRUMB_OP_PRESENT;
    D3D12_AUTO_BREADCRUMB_OP h7[] = { (D3D12_AUTO_BREADCRUMB_OP)46, M, B, M, D, B, M, D };
    D3D12_AUTO_BREADCRUMB_OP hx[] = { P };
    D3D12_AUTO_BREADCRUMB_OP h8[] = { (D3D12_AUTO_BREADCRUMB_OP)46, M, M, D };
    UINT d7 = 4, dx = 0, d8 = 0;
    D3D12_AUTO_BREADCRUMB_NODE1 n8, nx, n7;
    memset(&n8, 0, sizeof n8), memset(&nx, 0, sizeof nx), memset(&n7, 0, sizeof n7);
    n7.pCommandListDebugNameW = n8.pCommandListDebugNameW = L"VanaCore's frame";
    nx.pCommandListDebugNameW = L"Internal DXGI CommandList";
    n7.pCommandQueueDebugNameW = nx.pCommandQueueDebugNameW = n8.pCommandQueueDebugNameW = L"VanaCore";
    n7.BreadcrumbCount = 8, n7.pLastBreadcrumbValue = &d7, n7.pCommandHistory = h7, n7.pNext = &nx;
    nx.BreadcrumbCount = 1, nx.pLastBreadcrumbValue = &dx, nx.pCommandHistory = hx, nx.pNext = &n8;
    n8.BreadcrumbCount = 4, n8.pLastBreadcrumbValue = &d8, n8.pCommandHistory = h8;
    breadcrumbs_report(&n7, out);
    g_notes_sent = sent_was;
    for (int l = 0; l < 2 + after; ++l)
        g_notes[(7 + l) % NOTE_LISTS].count = 0;
    return 1;
}

static void allocations_report(const char* what, const D3D12_DRED_ALLOCATION_NODE1* a, GpuLine out)
{
    int n = 0;
    for (; a && n < 8; a = a->pNext, ++n)
        gpu_say(out, "GPU:   %s: %s %ls", what, allocation_type(a->AllocationType),
            a->ObjectNameW ? a->ObjectNameW : L"(unnamed)");
    if (!n)
        gpu_say(out, "GPU:   %s: nothing", what);
}

/* The device lost: why, and with DRED where the card stopped - logged once, `where` saying what found it. 1 if it's
 * lost. */
static int gpu_lost(const char* where, GpuLine out)
{
    if (!g_dev || g_lost_said)
        return g_lost_said;
    const HRESULT why = ID3D12Device_GetDeviceRemovedReason(g_dev);
    if (SUCCEEDED(why))
        return 0;
    g_lost_said = 1;
    const char* text = removed_reason(why);
    gpu_say(out, "GPU: the graphics card stopped (found %s): %08lX%s%s", where, (unsigned long)why, text[0] ? " " : "", text);
    gpu_say(out, "GPU: card: %s", g_card[0] ? g_card : "?");
    if (!g_dred)
    {
        gpu_say(out, "GPU: for where it stopped, turn on [logging] gpu (the launcher's Log GPU crash data) and play "
                     "till it happens again");
        return 1;
    }
    ID3D12DeviceRemovedExtendedData1* dred = NULL;
    if (FAILED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12DeviceRemovedExtendedData1, (void**)&dred)))
    {
        gpu_say(out, "GPU: no crash data from Windows (it needs Windows 10 2004 or later)");
        return 1;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc;
    memset(&bc, 0, sizeof bc);
    if (SUCCEEDED(ID3D12DeviceRemovedExtendedData1_GetAutoBreadcrumbsOutput1(dred, &bc)))
        breadcrumbs_report(bc.pHeadAutoBreadcrumbNode, out);
    else
        gpu_say(out, "GPU: no breadcrumbs from Windows");
    D3D12_DRED_PAGE_FAULT_OUTPUT1 pf;
    memset(&pf, 0, sizeof pf);
    if (SUCCEEDED(ID3D12DeviceRemovedExtendedData1_GetPageFaultAllocationOutput1(dred, &pf)) && pf.PageFaultVA)
    {
        gpu_say(out, "GPU: page fault: the card read or wrote GPU address 0x%llX", (unsigned long long)pf.PageFaultVA);
        allocations_report("there now", pf.pHeadExistingAllocationNode, out);
        allocations_report("freed there lately", pf.pHeadRecentFreedAllocationNode, out);
    }
    else
        gpu_say(out, "GPU: no page fault (not a bad address: a hang or the driver)");
    ID3D12DeviceRemovedExtendedData1_Release(dred);
    return 1;
}

int gfx_lost(void) { return g_lost_said; }

/* 1 when a call into the device failed because the device is gone (the loss logged, once, in place of the call's own
 * failure line: every texture, buffer and pipeline asked for after it fails the same way) */
static int failed_lost(HRESULT hr, const char* where)
{
    return (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && gpu_lost(where, gpu_log_line);
}

#ifdef VANACORE
static void helpers_check(void);
#endif

static void wait_fence(uint64_t v)
{
    const uint64_t done = completed();
    if (done == UINT64_MAX) /* what a lost device's fences say */
        gpu_lost("waiting for the graphics card", gpu_log_line);
    if (done >= v)
        return;
    ID3D12Fence_SetEventOnCompletion(g_fence, v, g_fence_event);
#ifdef VANACORE
    /* in steps: the queue may be waiting on the DLSS helper (DLSS's or frame generation's frame handed over), and a helper
     * that crashed or stopped would leave it, and this, waiting for good. Between them it's let through (helpers_check):
     * the frame's own checks for that (dlss_run, fg_watch) aren't reached while this thread is here */
    while (WaitForSingleObject(g_fence_event, 100) == WAIT_TIMEOUT)
        helpers_check();
#else
    WaitForSingleObject(g_fence_event, INFINITE);
#endif
}

/* the command list being recorded, opened (and set up) if none is */
static ID3D12GraphicsCommandList* list(void);

static ID3D12Resource* make_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state);

/* the GPU's timestamps for the profile: made the first time it's on */
static void prof_queries(void)
{
    if (g_queries || !g_dev)
        return;
    D3D12_QUERY_HEAP_DESC qh = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, QN * FRAMES, 0 };
    if (SUCCEEDED(ID3D12Device_CreateQueryHeap(g_dev, &qh, &IID_ID3D12QueryHeap, (void**)&g_queries)))
    {
        g_query_rb = make_buffer(8 * QN * FRAMES, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (g_query_rb)
            ID3D12Resource_Map(g_query_rb, 0, NULL, (void**)&g_query_cpu);
        ID3D12CommandQueue_GetTimestampFrequency(g_queue, &g_ts_freq);
        D3D12_QUERY_HEAP_DESC sq = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, GPU_QS * FRAMES, 0 };
        if (SUCCEEDED(ID3D12Device_CreateQueryHeap(g_dev, &sq, &IID_ID3D12QueryHeap, (void**)&g_sq)))
        {
            g_sq_rb = make_buffer(8 * GPU_QS * FRAMES, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            if (g_sq_rb)
                ID3D12Resource_Map(g_sq_rb, 0, NULL, (void**)&g_sq_cpu);
        }
    }
}

#ifdef VANACORE
void vanacore_gfx_record(int on)
{
    if (gfx_selected_d3d11())
    {
        /* no frame profile here, but the flicker watch: a recording, or [graphics] profile (FFXI_PROFILE) */
        const char* prof = getenv("FFXI_PROFILE");
        gfx_watching = on || (prof && prof[0] && prof[0] != '0');
        return;
    }
    if (on)
    {
        prof_queries();
        if (!gfx_profiling)
            gfx_profile_epoch++; /* the counters start over (prof_frame, fx_stats, d3d8.c's scenes) */
        gfx_profiling = 1;
    }
    else
        gfx_profiling = g_profile_env;
    g_frame_stats_ok = 0;
}

int vanacore_gfx_frame_stats(VanaGfxFrame* out)
{
    if (!gfx_profiling || !g_frame_stats_ok || gfx_selected_d3d11())
        return 0;
    *out = g_frame_stats;
    return 1;
}

typedef char gpu_scopes_match_the_header[GS_N == VANA_GPU_SCOPES ? 1 : -1];
typedef char gpu_scope_dlss_matches_the_header[GS_DLSS == VANA_GPU_SCOPE_DLSS ? 1 : -1];
typedef char upscaler_dlss_matches_the_header[VANA_UPSCALER_DLSS == VANA_GFX_UPSCALER_DLSS ? 1 : -1];
typedef char upscaler_fsr_matches_the_header[VANA_UPSCALER_FSR == VANA_GFX_UPSCALER_FSR ? 1 : -1];
typedef char fg_dlss_matches_the_header[VANA_FG_DLSS == VANA_GFX_FG_DLSS ? 1 : -1];
typedef char fg_fsr_matches_the_header[VANA_FG_FSR == VANA_GFX_FG_FSR ? 1 : -1];
typedef char gpu_scope_composite_matches_the_header[GS_COMPOSITE == VANA_GPU_SCOPE_COMPOSITE ? 1 : -1];
typedef char gpu_parts_fit_the_header[GP_N <= VANA_GPU_PARTS ? 1 : -1];
typedef char gpu_part_comp_matches_the_header[GP_COMP == VANA_GPU_PART_COMP ? 1 : -1];
typedef char gpu_part_wait_matches_the_header[GP_WAIT_DLSS == VANA_GPU_PART_WAIT_DLSS ? 1 : -1];
typedef char gpu_features_match_the_header[CF_N == VANA_GPU_FEATURES ? 1 : -1];

int vanacore_gfx_scopes(VanaGfxScopes* out)
{
    if (!gfx_profiling || gfx_selected_d3d11() || !g_scope_frames)
        return 0;
    memset(out, 0, sizeof *out);
    const double k = 1e-6 / (double)g_scope_frames;
    for (int i = 0; i < GS_N; ++i)
    {
        out->ms[i] = (float)((double)g_scope_ns[i] * k);
        for (int p = 0; p < GP_N; ++p)
            out->part_ms[i][p] = (float)((double)g_part_ns[i][p] * k);
    }
    out->frames = g_scope_frames;
    out->full_frames = g_scope_full;
    g_scope_full = 0;
    memset(g_scope_ns, 0, sizeof g_scope_ns);
    memset(g_part_ns, 0, sizeof g_part_ns);
    g_scope_frames = 0;
    /* the features, while they're measured: the whole draw's time less the draw's without each */
    out->measuring = g_serial < g_meas.until;
    const double whole = g_meas.n[CF_ALL] > 0.0 ? g_meas.ns[CF_ALL] / g_meas.n[CF_ALL] * 1e-6 : -1.0;
    for (int v = 0; v < CF_N; ++v)
    {
        const double ms = g_meas.n[v] > 0.0 ? g_meas.ns[v] / g_meas.n[v] * 1e-6 : -1.0;
        out->feature_off[v] = (uint8_t)(out->measuring && g_meas.off[v] > g_meas.at[v]);
        if (!out->measuring)
            out->feature_ms[v] = -1.0f;
        else if (out->feature_off[v])
            out->feature_ms[v] = 0.0f;
        else if (v == CF_ALL || v == CF_NONE)
            out->feature_ms[v] = (float)ms;
        else
            out->feature_ms[v] = ms >= 0.0 && whole >= 0.0 ? (float)fmax(whole - ms, 0.0) : -1.0f;
    }
    return 1;
}

const char* vanacore_gfx_scope_name(int i)
{
    static const char* const NAMES[GS_N] = { "The game's scene (world, characters, particles)", "Sun shadow maps",
        "Effects setup (the scene copied)", "Ambient occlusion", "Bloom", "God rays",
        "Composite (fog, shadows, colour grade, outlines)", "Reflections, bounce light and clarity", "Depth of field", "FXAA", "DLSS",
        "The interface drawn over the scene, and the rest", "The profile measuring the composite (an extra draw a frame, while it's open)" };
    return i >= 0 && i < GS_N ? NAMES[i] : "";
}

const char* vanacore_gfx_part_name(int scope, int part)
{
    static const char* const NAMES[GP_N] = { "The rest (state changes between its passes)",
        "The GPU idle between command lists (waiting for the CPU to hand it more)",
        "VanaCore's glow (a character's picture blurred, laid over)", "Mip chains for textures drawn smaller (the scene filter)",
        "The far map (sun_distance)", "The near, sharp map (sun_near)", "The characters' own map (sun_soft_chars)",
        "The scene copied (for the passes to read)", "Depth for it (linear, and three smaller levels)",
        "The lookups (occlusion, the sun's maps)", "Blurred (across, then down)",
        "Blended with the frame before (temporal)", "The bright parts, at a quarter size", "Blurred at a quarter size",
        "At an eighth, blurred", "The bright sky (the mask)", "The rays (64 steps toward the sun)", "Blurred",
        "Smoothing (the cel look's filter, smooth)", "The composite shader (all its features in one pass)",
        "The sky's colour (fog_color=sky)", "Clarity: its blur (at a quarter size)", "Clarity: laid over",
        "Reflections (a layer at the occlusion's size)", "Bounce light (a layer at the occlusion's size)",
        "Bounce light blended with the frame before (temporal)", "Reflections and bounce light laid over the scene",
        "The focus (one texel)", "The scene at half size", "The blur (40 taps)",
        "Laid over the scene", "FXAA", "Its inputs (colour, depth, motion)", "Its output onto the frame (and the colour grade, laid last)",
        "Waiting on the helper's DLSS (its own queue)", "The interface held for DLSS, drawn over its output",
        "Frame generation: the frame handed over (copied)", "Waiting on the helper's frame generation (its own queue)",
        "The frame onto the window (and sharpen)", "VanaCore's interface (ImGui) laid over", "The FPS overlay",
        "Light from the effects (what flames and spells added, blurred, laid over)",
        "Frame generation: the names over heads drawn onto its frames (nameplates.framegen)",
        "VanaCore's sky (sky), in place of the game's dome",
        "The occlusion test (the zone's pieces against the scene's depth: occl)" };
    if (scope < 0 || scope >= GS_N || part < 0 || part >= GP_N)
        return "";
    if (part == GP_REST && scope == GS_SCENE)
        return "The game's draws";
    if (part == GP_REST && scope == GS_SUN)
        return "The rest (the cache, clears, state changes)";
    if (part == GP_REST && scope == GS_UI)
        return "The game's interface draws, and the rest";
    if (part == GP_REST && scope == GS_MEASURE)
        return "The composite shader, drawn again";
    if (part == GP_GAP && scope == GS_SCENE)
        return "The GPU idle between command lists (waiting for the game's draws)";
    return NAMES[part];
}

const char* vanacore_gfx_feature_name(int feature, int* parent)
{
    static const char* const NAMES[CF_N] = { "The whole draw, as measured", "Ambient occlusion and shadows laid on",
        "Ink lines (outline)", "Creases (outline_crease)", "Cel bands (bands)", "Height fog (fog)", "Zone lines",
        "Bloom laid on", "God rays laid on",
        "Every feature off (reading the scene and depth, writing it)" };
    if (parent)
        *parent = feature == CF_CREASE ? CF_LINES : -1;
    return feature >= 0 && feature < CF_N ? NAMES[feature] : "";
}

void vanacore_gfx_measure(void)
{
    if (g_serial >= g_meas.until) /* again after a while: from nothing */
        memset(&g_meas, 0, sizeof g_meas);
    g_meas.until = g_serial + 30;
}
#endif

/* the frame's GPU timestamp i (profile), once a frame: 1 the scene effects start, 2 the sun's maps are drawn,
 * 3 the effects are done */
static void prof_mark(int i)
{
    if (!gfx_profiling || !g_queries)
        return;
    ID3D12GraphicsCommandList* l = list();
    Frame* f = &g_frames[g_frame];
    if (!f->timed || (f->marks & (1 << i)))
        return;
    ID3D12GraphicsCommandList_EndQuery(l, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, QN * g_frame + i);
    f->marks |= 1 << i;
}

/* the scope the frame is in now (GS_SCENE at its start), and its part */
static int gpu_scope_cur(void) { return g_gs_scope; }
static int gpu_part_cur(void) { return g_gs_part; }

/* a timestamp into l: the GPU's time from here is scope's, part's (profile) */
static void gpu_stamp(ID3D12GraphicsCommandList* l, int scope, int part)
{
    Frame* f = &g_frames[g_frame];
    if (!gfx_profiling || !g_sq || !f->timed || f->closed || f->scope_n < 1)
        return;
    if (f->scope_id[f->scope_n - 1] == scope && f->part_id[f->scope_n - 1] == part)
        return;
    /* out of them: the rest of the frame's parts (then its scopes) are the one before's */
    if (f->scope_n >= GPU_QS - 1 || (f->scope_n >= GPU_QS - GPU_QS_SCOPES && f->scope_id[f->scope_n - 1] == scope))
    {
        f->full = 1;
        return;
    }
    ID3D12GraphicsCommandList_EndQuery(l, g_sq, D3D12_QUERY_TYPE_TIMESTAMP, GPU_QS * g_frame + f->scope_n);
    f->scope_id[f->scope_n] = (uint8_t)scope, f->part_id[f->scope_n] = (uint8_t)part;
    f->scope_n++;
}

/* what the GPU does from here is `id`'s, until the next scope starts (profile) */
static void gpu_scope(int id)
{
    if (g_dred && id != g_dred_scope)
    {
        g_dred_scope = id;
        if (g_list_open) /* else the list marks it as it opens */
            dred_mark(g_list, id);
    }
    ID3D12GraphicsCommandList* l = gfx_profiling && g_sq ? list() : NULL;
    g_gs_scope = id, g_gs_part = GP_REST;
    if (l)
        gpu_stamp(l, id, GP_REST);
}

/* ... and within the scope, `part`'s, until the next part or scope */
static void gpu_part(int part)
{
    ID3D12GraphicsCommandList* l = gfx_profiling && g_sq ? list() : NULL;
    g_gs_part = part;
    if (l)
        gpu_stamp(l, g_gs_scope, part);
}

/* --- frames, command lists and the upload ring ------------------------------------------------------------ */
/* The ring gives back what a big moment took (a zone's textures and buffers uploaded in one frame: tens of 8 MB chunks,
 * kept for good, in a 32-bit game's address space and private memory): every RING_TRIM_USES of a frame's slot (about 30 s
 * at 60 fps, as there are FRAMES slots), the chunks past the most it used in them (and RING_KEEP) are released. Called
 * from frame_begin after the slot's fence is waited on, so the GPU has done with all of them */
#define RING_TRIM_USES 600u
#define RING_KEEP 2u
static void ring_trim(Frame* f)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < f->nchunks; ++i)
        if (f->chunks[i].used)
            used = i + 1;
    if (used > f->ring_most)
        f->ring_most = used;
    if (++f->ring_uses < RING_TRIM_USES)
        return;
    const uint32_t keep = f->ring_most > RING_KEEP ? f->ring_most : RING_KEEP;
    for (uint32_t i = keep; i < f->nchunks; ++i)
        ID3D12Resource_Release(f->chunks[i].res);
    if (f->nchunks > keep)
        f->nchunks = keep;
    f->ring_most = 0, f->ring_uses = 0;
}

static void frame_begin(void)
{
    if (g_frame_open)
        return;
    Frame* f = &g_frames[g_frame];
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    wait_fence(f->fence);
    if (gfx_profiling)
    {
        g_prof.sem_ns += gfx_now_ns() - t0;
        if (f->timed && g_query_cpu && g_ts_freq)
        {
            const uint64_t* q = &g_query_cpu[QN * g_frame];
            const double k = 1e9 / (double)g_ts_freq;
            if (q[4] > q[0])
                g_gpu_ns += (uint64_t)((double)(q[4] - q[0]) * k);
            if (q[2] > q[1])
                g_gpu_sun_ns += (uint64_t)((double)(q[2] - q[1]) * k);
            if (q[3] > q[2])
                g_gpu_fx_ns += (uint64_t)((double)(q[3] - q[2]) * k);
            if (g_sq_cpu && f->scope_n > 0)
            {
                const uint64_t* sq = &g_sq_cpu[GPU_QS * g_frame];
                uint64_t measured = 0;
                for (int i = 0; i < f->scope_n; ++i)
                    if (sq[i + 1] > sq[i])
                    {
                        const uint64_t ns = (uint64_t)((double)(sq[i + 1] - sq[i]) * k);
                        g_scope_ns[f->scope_id[i]] += ns;
                        g_part_ns[f->scope_id[i]][f->part_id[i]] += ns;
                        if (f->scope_id[i] == GS_MEASURE)
                            measured += ns;
                    }
                g_scope_frames++;
                g_scope_full += f->full != 0;
                if (f->measured >= 0 && measured)
                {
                    g_meas.ns[f->measured] = g_meas.ns[f->measured] * 0.9 + (double)measured;
                    g_meas.n[f->measured] = g_meas.n[f->measured] * 0.9 + 1.0;
                }
            }
        }
    }
    f->timed = 0, f->marks = 0, f->scope_n = 0, f->closed = 0, f->full = 0, f->measured = -1;
    g_gs_scope = GS_SCENE, g_gs_part = GP_REST, g_gap_part = GP_GAP;
    ID3D12CommandAllocator_Reset(f->alloc);
    ring_trim(f);
    for (uint32_t i = 0; i < f->nchunks; ++i)
        f->chunks[i].used = 0;
    f->cur = 0;
    collect();
    g_frame_open = 1;
    g_dred_scope = GS_SCENE; /* a frame starts with the game's scene */
}

/* the command list being recorded, opened (and set up) if none is */
static ID3D12GraphicsCommandList* list(void)
{
    frame_begin();
    if (g_list_open)
        return g_list;
    Frame* f = &g_frames[g_frame];
    ID3D12GraphicsCommandList_Reset(g_list, f->alloc, NULL);
    ID3D12DescriptorHeap* heaps[2] = { g_srv.heap, g_samp.heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g_list, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, g_root);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_TEX2D, g_srv.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_TEXCUBE, g_srv.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_SAMPLERS, g_samp.gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, ROOT_SAMPCMP, g_samp.gpu);
    if (gfx_profiling && g_queries && !f->timed)
    {
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, QN * g_frame);
        f->timed = 1;
        if (g_sq)
        {
            ID3D12GraphicsCommandList_EndQuery(g_list, g_sq, D3D12_QUERY_TYPE_TIMESTAMP, GPU_QS * g_frame);
            f->scope_id[0] = (uint8_t)g_gs_scope, f->part_id[0] = (uint8_t)g_gs_part;
            f->scope_n = 1;
        }
    }
    else /* a list after the frame's first: the GPU back from between them, on what it was doing */
        gpu_stamp(g_list, g_gs_scope, g_gs_part);
    if (g_dred) /* each list says the pass it starts in, its notes from none */
    {
        g_notes[g_notes_sent % NOTE_LISTS].count = 0, g_notes[g_notes_sent % NOTE_LISTS].broken = 0;
        dred_mark(g_list, g_dred_scope);
    }
    g_list_open = 1;
    g_list_id++;
    g_targets_bound = 0;
    g_bound_pso = NULL;
    g_bound_topo = -1;
    drawn_forget();
    return g_list;
}

/* executes what is recorded; the frame stays open (its ring is still in use) */
static void submit(int wait)
{
    if (!g_list_open)
        return;
    gpu_stamp(g_list, g_gs_scope, g_gap_part); /* from its end till the next list starts: the GPU between them */
    g_gap_part = GP_GAP;
    ID3D12GraphicsCommandList_Close(g_list);
    ID3D12CommandList* l = (ID3D12CommandList*)g_list;
    /* the device lost: nothing sent to it (NVIDIA's driver crashes on a list sent after the loss, 10-25 s later);
     * its fences say everything's done (UINT64_MAX), so the waits for this one return at once */
    if (!g_lost_said)
    {
        ID3D12CommandQueue_ExecuteCommandLists(g_queue, 1, &l);
        ID3D12CommandQueue_Signal(g_queue, g_fence, g_fence_value + 1);
    }
    g_frames[g_frame].fence = ++g_fence_value;
    if (g_dred)
        g_notes_sent++;
    g_list_open = 0;
    g_cmd_draws = 0;
    if (wait)
        wait_fence(g_fence_value);
    if (g_info)
        print_messages();
}

static ID3D12Resource* make_buffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource* r = NULL;
    const HRESULT hr = ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL, &IID_ID3D12Resource, (void**)&r);
    if (FAILED(hr))
    {
        if (!failed_lost(hr, "making a buffer"))
            fprintf(stderr, "[recomp] gfx: buffer of %llu bytes failed\n", (unsigned long long)size);
        return NULL;
    }
    return r;
}

typedef struct Alloc
{
    uint8_t* cpu;
    ID3D12Resource* res;
    uint64_t off;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
} Alloc;

/* n bytes of this frame's ring, with `reach` bytes (n or more) of its chunk from where they start: a draw's uniforms take
 * the part of GfxU the draw fills (n), the next allocation starting after it, and a whole GfxU's room (reach) is needed
 * only where a chunk ends, since the functions are compiled against the whole struct and the GPU mustn't read past the
 * chunk (a uniform past what a draw fills was never its own: stale ring bytes, as before). A whole GfxU each was 3.5 KB a
 * draw, ~2 KB of it unused: 80 MB of chunks in a 32-bit game (10 of them, 3 or 4 a frame), in NVIDIA's 32 MB upload pools,
 * 136 MB. A large request gets an upload buffer of its own */
static Alloc ring_part(size_t n, size_t reach, size_t align)
{
    frame_begin();
    Alloc a = { 0 };
    g_prof.bytes += n;
    if (reach > RING_CHUNK / 2)
    {
        a.res = make_buffer(reach, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!a.res)
            return a;
        ID3D12Resource_Map(a.res, 0, NULL, (void**)&a.cpu);
        a.gpu = ID3D12Resource_GetGPUVirtualAddress(a.res);
        defer((IUnknown*)a.res, NULL, -1); /* released once this frame's work is done */
        return a;
    }
    Frame* f = &g_frames[g_frame];
    for (;; f->cur++)
    {
        if (f->cur == f->nchunks)
        {
            Chunk c = { 0 };
            c.res = make_buffer(RING_CHUNK, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!c.res)
                return a;
            ID3D12Resource_Map(c.res, 0, NULL, (void**)&c.cpu);
            c.gpu = ID3D12Resource_GetGPUVirtualAddress(c.res);
            c.size = RING_CHUNK;
            f->chunks = (Chunk*)realloc(f->chunks, (f->nchunks + 1) * sizeof(Chunk));
            f->chunks[f->nchunks++] = c;
        }
        Chunk* c = &f->chunks[f->cur];
        uint32_t at = (uint32_t)((c->used + align - 1) & ~(align - 1));
        if (at + reach <= c->size)
        {
            c->used = at + (uint32_t)n;
            a.cpu = c->cpu + at, a.res = c->res, a.off = at, a.gpu = c->gpu + at;
            return a;
        }
    }
}

static Alloc ring(size_t n, size_t align)
{
    return ring_part(n, n, align);
}

static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list(), 1, &b);
}

static void tex_state(GfxTex* t, D3D12_RESOURCE_STATES want)
{
    if (t->state != want)
    {
        barrier(t->res, t->state, want);
        t->state = want;
    }
    t->used = pending();
}

#define BUF_READ (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDEX_BUFFER)

static void buf_state(GfxBuf* b, D3D12_RESOURCE_STATES want)
{
    list();
    Slab* s = b->slab; /* the state is the slab's: every block in it */
    D3D12_RESOURCE_STATES cur = s->list == g_list_id ? s->state : D3D12_RESOURCE_STATE_COMMON;
    if (cur != want)
        barrier(s->res, cur, want);
    s->state = want, s->list = g_list_id;
}

/* --- formats ---------------------------------------------------------------------------------------------- */
#define MAP4(r, g, b, a) D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a)
#define ONE D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1

static DXGI_FORMAT dx_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* texel, UINT* mapping)
{
    *conv = CONV_NONE, *block = 0, *texel = 4;
    *mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (use == GFX_USE_DEPTH)
        return fmt == F_D16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
    switch (fmt)
    {
    case F_A8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X8R8G8B8: *mapping = MAP4(0, 1, 2, ONE); return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_R5G6B5: *conv = CONV_565; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_X1R5G5B5: *conv = CONV_X555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A1R5G5B5: *conv = CONV_1555; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A4R4G4B4: *conv = CONV_4444; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case F_A8: *texel = 1; return DXGI_FORMAT_A8_UNORM;
    case F_L8: *texel = 1, *mapping = MAP4(0, 0, 0, ONE); return DXGI_FORMAT_R8_UNORM;
    case F_A8L8: *texel = 2, *mapping = MAP4(0, 0, 0, 1); return DXGI_FORMAT_R8G8_UNORM;
    case F_V8U8: *texel = 2; return DXGI_FORMAT_R8G8_SNORM;
    }
    if (fmt == FOURCC('D', 'X', 'T', '1'))
        return *block = 8, DXGI_FORMAT_BC1_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '2') || fmt == FOURCC('D', 'X', 'T', '3'))
        return *block = 16, DXGI_FORMAT_BC2_UNORM;
    if (fmt == FOURCC('D', 'X', 'T', '4') || fmt == FOURCC('D', 'X', 'T', '5'))
        return *block = 16, DXGI_FORMAT_BC3_UNORM;
    if (fmt == FOURCC('A', 'T', 'I', '2')) /* BC5: a normal map's X and Y (VanaCore's texture packs) */
        return *block = 16, DXGI_FORMAT_BC5_UNORM;
    return DXGI_FORMAT_B8G8R8A8_UNORM;
}

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

/* one row of a 16-bit D3D format into BGRA8 */
static void convert_row(int conv, const uint8_t* src, uint8_t* dst, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x)
    {
        uint32_t p = (uint32_t)src[2 * x] | ((uint32_t)src[2 * x + 1] << 8), r, g, b, a;
        switch (conv)
        {
        case CONV_565: r = expand(p >> 11, 5), g = expand((p >> 5) & 63, 6), b = expand(p & 31, 5), a = 255; break;
        case CONV_X555: r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = 255; break;
        case CONV_1555:
            r = expand((p >> 10) & 31, 5), g = expand((p >> 5) & 31, 5), b = expand(p & 31, 5), a = (p & 0x8000) ? 255 : 0;
            break;
        default: r = (p >> 8 & 15) * 17, g = (p >> 4 & 15) * 17, b = (p & 15) * 17, a = (p >> 12) * 17; break;
        }
        dst[4 * x] = (uint8_t)b, dst[4 * x + 1] = (uint8_t)g, dst[4 * x + 2] = (uint8_t)r, dst[4 * x + 3] = (uint8_t)a;
    }
}

/* --- static buffers --------------------------------------------------------------------------------------------- */
#define SLAB_SIZE (2u << 20)
#define SLAB_MIN_SHIFT 8 /* 256-byte blocks, the smallest class */
#define SLAB_CLASSES 11  /* up to 256 KB blocks; a larger buffer has a resource of its own */
static Slab* g_slabs[SLAB_CLASSES];

/* a slab of `size` bytes: blocks of class cls, or (cls -1) one buffer */
static Slab* slab_new(uint64_t size, int cls)
{
    /* a slab of blocks has 64 KB past its last: a draw reading past its buffer's end (its indices beyond its vertices) reads
     * the next block's bytes, or these, never past the resource - which the root views don't check, a page fault. A buffer
     * made on its own is rounded to 64 KB by the device, as before slabs */
    ID3D12Resource* r = make_buffer(cls >= 0 ? size + 65536u : size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!r)
        return NULL;
    Slab* s = (Slab*)calloc(1, sizeof *s);
    if (!s)
    {
        ID3D12Resource_Release(r);
        return NULL;
    }
    s->res = r;
    s->gpu = ID3D12Resource_GetGPUVirtualAddress(r);
    s->state = D3D12_RESOURCE_STATE_COMMON;
    s->cls = cls;
    if (cls >= 0)
    {
        const uint32_t block = 1u << (SLAB_MIN_SHIFT + cls);
        s->nblocks = SLAB_SIZE / block;
        s->free_blocks = (uint32_t*)malloc(s->nblocks * sizeof *s->free_blocks);
        if (!s->free_blocks)
        {
            ID3D12Resource_Release(r);
            free(s);
            return NULL;
        }
        for (uint32_t i = 0; i < s->nblocks; ++i) /* handed out from offset 0 up */
            s->free_blocks[i] = (s->nblocks - 1 - i) * block;
        s->nfree = s->nblocks;
    }
    if (g_dred) /* named for a page fault's report: what it's for */
    {
        wchar_t name[80];
        if (cls >= 0)
            swprintf(name, 80, L"static buffers: a slab of %u-byte blocks", 1u << (SLAB_MIN_SHIFT + cls));
        else
            swprintf(name, 80, L"static buffer of %llu bytes", (unsigned long long)size);
        ID3D12Resource_SetName(r, name);
    }
    return s;
}

/* a block back in its slab's free list (collect: the GPU is past what may read it). A slab left wholly free is let go
 * when the others of its class have room enough - another wholly free, or half a slab's blocks between them - so a zone's
 * buffers going don't keep their memory, and buffers made and let go near a slab's edge (the grass's chunks) don't make and
 * let go a 2 MB resource over and over: one free slab is kept. */
static void slab_put(Slab* s, uint32_t off)
{
    s->free_blocks[s->nfree++] = off;
    if (s->nfree < s->nblocks)
        return;
    uint32_t spare = 0;
    int free_other = 0;
    for (const Slab* o = g_slabs[s->cls]; o; o = o->next)
        if (o != s)
            spare += o->nfree, free_other |= o->nfree == o->nblocks;
    if (!free_other && spare < s->nblocks / 2)
        return;
    Slab** at = &g_slabs[s->cls];
    while (*at && *at != s)
        at = &(*at)->next;
    if (!*at)
        return;
    *at = s->next;
    /* nothing recorded reads it: every block's last use is done (their fences passed), and no list is open */
    ID3D12Resource_Release(s->res);
    free(s->free_blocks);
    free(s);
}

GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!g_dev || !size)
        return NULL;
    /* the device lost: no buffer (a slab's block is had without asking the device, which would say so) */
    if (gpu_lost("making a buffer", gpu_log_line))
        return NULL;
    /* (no slack of its own past its end: a slab has 64 KB past its last block, slab_new; one of 256 bytes fits a 256-byte
     * block, not a 512) */
    const uint64_t need = ((uint64_t)size + 15) & ~15ull;
    /* FFXI_D3D12_SLABS=0 ([graphics] slabs=0): each buffer a slab of its own, a committed resource as before them */
    static int slabs = -1;
    if (slabs < 0)
        slabs = !(getenv("FFXI_D3D12_SLABS") && getenv("FFXI_D3D12_SLABS")[0] == '0');
    int cls = slabs ? 0 : SLAB_CLASSES;
    while (cls < SLAB_CLASSES && (1ull << (SLAB_MIN_SHIFT + cls)) < need)
        cls++;
    Slab* s = NULL;
    uint32_t off = 0;
    if (cls < SLAB_CLASSES)
    {
        for (s = g_slabs[cls]; s && !s->nfree; s = s->next)
            ;
        if (!s && (s = slab_new(SLAB_SIZE, cls)) != NULL)
            s->next = g_slabs[cls], g_slabs[cls] = s;
        if (s)
            off = s->free_blocks[--s->nfree];
    }
    else
        s = slab_new(need, -1);
    if (!s)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    if (!b)
    {
        if (s->cls < 0)
            ID3D12Resource_Release(s->res), free(s);
        else
            s->free_blocks[s->nfree++] = off; /* never used: free again at once */
        return NULL;
    }
    b->slab = s, b->off = off, b->gpu = s->gpu + off;
    b->size = size;
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b)
        return;
    MO_TOUCH(b);
    BATCH_TOUCH(b);
    if (b->sun_seen == g_serial || b->sun_cached)
        sun_forget(b, NULL), forget_note(b, NULL, "freed"); /* the casters that draw from it are drawn no more */
    if (b->slab->cls < 0)
    {
        defer((IUnknown*)b->slab->res, NULL, -1);
        free(b->slab);
    }
    else
        defer_block(b->slab, b->off);
    free(b);
}

static D3D12_GPU_VIRTUAL_ADDRESS buf_upload_at(GfxBuf* b, uint32_t off, const void* data, uint32_t size);

/* in command order: draws recorded before this read the old contents, draws after the new */
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b || !g_dev)
        return;
    if (size > b->size)
        size = b->size;
    if (!size)
        return;
    MO_TOUCH(b); /* the redraws waiting that read the old contents where they are: drawn first */
    BATCH_TOUCH(b); /* ... and the zone's draws kept */
    /* new contents: a caster recorded from the old (this frame's, or the zone's kept) is not it */
    if (b->sun_seen == g_serial || b->sun_cached)
        sun_forget(b, NULL), forget_note(b, NULL, "written again");
    if (b->uploads && b->upload_frame == g_serial)
        b->twice = g_serial;
    b->uploads++, b->upload_frame = g_serial;
    const D3D12_GPU_VIRTUAL_ADDRESS up = buf_upload_at(b, 0, data, size);
    /* the ring's copy is there till the frame's work is done; one too large for the ring has an upload buffer of its own,
     * which may go sooner (after the submission it was made in), so it isn't kept */
    b->up_gpu = (size_t)size + 16 <= RING_CHUNK / 2 ? up : 0, b->up_size = size, b->up_serial = g_serial;
}

/* `size` bytes into the buffer at `off`, in command order: their copy in this frame's ring, or 0 */
static D3D12_GPU_VIRTUAL_ADDRESS buf_upload_at(GfxBuf* b, uint32_t off, const void* data, uint32_t size)
{
    Alloc a = ring((size_t)size + 16, 16); /* the slack: a caster drawn from the copy (caster_bytes) reads a last element whole */
    if (!a.cpu)
        return 0;
    memcpy(a.cpu, data, size);
    buf_state(b, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(list(), b->slab->res, b->off + off, a.res, a.off, size);
    buf_state(b, BUF_READ);
    return a.gpu;
}

/* --- textures ------------------------------------------------------------------------------------------------ */
static uint32_t subresource(const GfxTex* t, uint32_t face, uint32_t level)
{
    return level + face * t->mips;
}

GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    UINT mapping;
    t->dxfmt = dx_format(fmt, use, &t->conv, &t->block, &t->texel, &mapping);
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
    if (type == GFX_TEX_CUBE)
        t->h = t->w;
    t->faces = type == GFX_TEX_CUBE ? 6 : 1;
    t->rw = t->w, t->rh = t->h;
    if (t->block) /* D3D12 wants the top level of a block-compressed texture in whole blocks */
        t->rw = (t->rw + 3) & ~3u, t->rh = (t->rh + 3) & ~3u;
    uint32_t most = 1;
    for (uint32_t s = t->rw > t->rh ? t->rw : t->rh; s > 1; s >>= 1)
        most++;
    if (t->levels > most)
        t->levels = most;
    t->mips = t->levels;
    /* a large color target (FFXI's background) gets a whole mip chain: the finished scene is made
     * smaller through it rather than one bilinear sample per screen pixel (gfx_scene_done). Large is the scene's
     * size at DLSS's or FSR's share too (1280x800 for 2560x1600 at Performance): the trust and avatar glow's
     * picture follows the scene's size, and VanaCore's glow (gfx_glow) blurs through the chain, so with both
     * sides held to 1024 it drew nothing with DLSS on. Not FFXI's own square effect targets (512x512 and under). */
    const uint32_t long_side = t->w > t->h ? t->w : t->h, short_side = t->w > t->h ? t->h : t->w;
    if (use == GFX_USE_RT && type == GFX_TEX_2D && t->levels == 1 && long_side >= 640 && short_side >= 360)
        while ((t->w >> t->mips) || (t->h >> t->mips))
            t->mips++;
    t->has_stencil = t->dxfmt == DXGI_FORMAT_D24_UNORM_S8_UINT;
    t->x8 = fmt == F_X8R8G8B8;
    t->srv = t->srv_mips = -1;
    /* depth is typeless underneath, so the scene effects can read it (as R16 or R24) */
    DXGI_FORMAT resfmt = use != GFX_USE_DEPTH ? t->dxfmt
        : t->dxfmt == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    D3D12_RESOURCE_FLAGS flags = use == GFX_USE_RT ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
        : use == GFX_USE_DEPTH ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                               : D3D12_RESOURCE_FLAG_NONE;
    t->state = use == GFX_USE_RT ? D3D12_RESOURCE_STATE_RENDER_TARGET
        : use == GFX_USE_DEPTH  ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                : D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, t->rw, t->rh, (UINT16)t->faces, (UINT16)t->mips, resfmt,
        { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    const HRESULT hr = ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, t->state, NULL,
        &IID_ID3D12Resource, (void**)&t->res);
    if (FAILED(hr))
    {
        if (!failed_lost(hr, "making a texture"))
            fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed\n", w, h, fmt);
        free(t);
        return NULL;
    }
    if (g_dred) /* named for a page fault's report: its size and what it's for */
    {
        wchar_t name[80];
        swprintf(name, 80, L"texture %ux%u%ls format %u %ls", w, h, type == GFX_TEX_CUBE ? L" cube" : L"", fmt,
            use == GFX_USE_RT ? L"render target" : use == GFX_USE_DEPTH ? L"depth" : L"sampled");
        ID3D12Resource_SetName(t->res, name);
    }
    /* the view the game's draws see: its own levels (a chain the scene filter adds is not the game's) */
    t->srv = heap_alloc(&g_srv);
    if (t->srv >= 0)
    {
        if (g_srv_owner)
            g_srv_owner[t->srv] = t;
        D3D12_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = use != GFX_USE_DEPTH ? t->dxfmt
            : t->dxfmt == DXGI_FORMAT_D16_UNORM ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        v.Shader4ComponentMapping = use != GFX_USE_DEPTH ? mapping : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (type == GFX_TEX_CUBE)
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, v.TextureCube.MipLevels = t->levels;
        else
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = t->levels;
        ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->srv));
        if (t->mips > t->levels)
        {
            /* the whole chain, and each level alone to make the next from */
            t->srv_mips = heap_alloc(&g_srv);
            if (t->srv_mips >= 0)
            {
                v.Texture2D.MipLevels = t->mips;
                ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->srv_mips));
            }
            t->mip_srv = (int32_t*)malloc(sizeof(int32_t) * (t->mips - 1));
            for (uint32_t l = 0; l + 1 < t->mips; ++l)
            {
                t->mip_srv[l] = heap_alloc(&g_srv);
                v.Texture2D.MostDetailedMip = l, v.Texture2D.MipLevels = 1;
                if (t->mip_srv[l] >= 0)
                    ID3D12Device_CreateShaderResourceView(g_dev, t->res, &v, heap_cpu(&g_srv, t->mip_srv[l]));
                else
                    t->srv_mips = -1; /* no descriptor for a level: no chain */
            }
        }
    }
    else
        fprintf(stderr, "[recomp] gfx: out of texture descriptors\n");
    if (use != GFX_USE_SAMPLE)
    {
        uint32_t n = t->faces * t->mips;
        t->views = (int32_t*)malloc(sizeof(int32_t) * n);
        for (uint32_t i = 0; i < n; ++i)
            t->views[i] = -1;
    }
    if (use == GFX_USE_RT)
    {
        t->rt_next = g_rts;
        if (g_rts)
            g_rts->rt_prev = t;
        g_rts = t;
    }
    return t;
}

static void release_views(GfxTex* t)
{
    if (!t->views)
        return;
    for (uint32_t i = 0; i < t->faces * t->mips; ++i)
        defer(NULL, t->use == GFX_USE_DEPTH ? &g_dsv : &g_rtv, t->views[i]);
    free(t->views);
    t->views = NULL;
}

#ifdef VANACORE
static void fgn_forget_tex(const GfxTex* t);
#endif

void gfx_tex_destroy(GfxTex* t)
{
    if (!t)
        return;
    MO_TOUCH(t);
    BATCH_TOUCH(t);
#ifdef VANACORE
    fgn_forget_tex(t); /* frame generation's names, kept to the frame's end, don't draw with it */
#endif
    if (g_rt == t)
        g_rt = NULL, g_targets_bound = 0;
    if (g_ds == t)
        g_ds = NULL, g_targets_bound = 0;
    defer((IUnknown*)t->res, NULL, -1);
    defer(NULL, &g_srv, t->srv);
    defer(NULL, &g_srv, t->srv_mips);
    for (uint32_t l = 0; t->mip_srv && l + 1 < t->mips; ++l)
        defer(NULL, &g_srv, t->mip_srv[l]);
    free(t->mip_srv);
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
        sun_forget(NULL, t), forget_note(NULL, t, "freed"); /* the casters that sample it are drawn no more */
    release_views(t);
    for (int i = 0; i < GFX_READBACKS; ++i)
        defer((IUnknown*)t->rb[i], NULL, -1);
    free(t);
}

static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = (t->w >> level) ? (t->w >> level) : 1;
    *h = (t->h >> level) ? (t->h >> level) : 1;
}

/* the resource's own size of a level (differs from D3D's only for rounded-up DXT) */
static void phys_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = (t->rw >> level) ? (t->rw >> level) : 1;
    *h = (t->rh >> level) ? (t->rh >> level) : 1;
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!g_dev || !t || level >= t->levels || face >= t->faces || !w || !h || t->use == GFX_USE_DEPTH)
        return;
    MO_TOUCH(t);
    BATCH_TOUCH(t);
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
    if (t->block)
    {
        uint32_t pw, ph;
        phys_size(t, level, &pw, &ph);
        uint32_t most_c = (pw + 3) / 4 - x / 4, most_r = (ph + 3) / 4 - y / 4;
        cols = cols < most_c ? cols : most_c;
        rows = rows < most_r ? rows : most_r;
    }
    uint32_t row_bytes = t->block ? cols * t->block : cols * t->texel;
    uint32_t up_pitch = (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    Alloc a = ring((size_t)up_pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t r = 0; r < rows; ++r)
    {
        if (t->conv)
            convert_row(t->conv, s + (size_t)r * pitch, a.cpu + (size_t)r * up_pitch, cols);
        else
            memcpy(a.cpu + (size_t)r * up_pitch, s + (size_t)r * pitch, row_bytes);
    }
    tex_state(t, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    memset(&dst, 0, sizeof dst);
    memset(&from, 0, sizeof from);
    dst.pResource = t->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = subresource(t, face, level);
    from.pResource = a.res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Offset = a.off;
    from.PlacedFootprint.Footprint.Format = t->dxfmt;
    from.PlacedFootprint.Footprint.Width = t->block ? cols * 4 : cols;
    from.PlacedFootprint.Footprint.Height = t->block ? rows * 4 : rows;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = up_pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &dst, x, y, 0, &from, NULL);
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!g_dev || !t || level >= t->levels)
        return;
    BATCH_TOUCH(t);
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* rows of a level read back in D3D12's layout -> D3D's */
static void copy_out(const GfxTex* t, const uint8_t* s, uint32_t w, uint32_t h, uint32_t row, void* dst, uint32_t pitch)
{
    uint32_t bpp = d3d_bpp(t->fmt);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* d = (uint8_t*)dst + (size_t)y * pitch;
        if (!t->conv)
            memcpy(d, s + (size_t)y * row, (size_t)w * bpp);
        else /* 16-bit color targets: back from BGRA8 */
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = s + (size_t)y * row + 4 * x;
                uint32_t b = p[0], g = p[1], r = p[2], a = p[3], v;
                switch (t->conv)
                {
                case CONV_565: v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; break;
                case CONV_4444: v = (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4; break;
                default: v = (a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3; break;
                }
                d[2 * x] = (uint8_t)v, d[2 * x + 1] = (uint8_t)(v >> 8);
            }
    }
}

static uint32_t readback_row(const GfxTex* t, uint32_t w)
{
    return (w * t->texel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

/* a copy of the level into buf, recorded with the frame's other work */
static void queue_readback(GfxTex* t, uint32_t face, uint32_t level, uint32_t w, uint32_t h, uint32_t row, ID3D12Resource* buf)
{
    tex_state(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    memset(&dst, 0, sizeof dst);
    memset(&from, 0, sizeof from);
    from.pResource = t->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = subresource(t, face, level);
    dst.pResource = buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = t->dxfmt;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = row;
    D3D12_BOX box = { 0, 0, 0, w, h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &dst, 0, 0, 0, &from, &box);
}

#ifdef VANACORE
static void zl_on_read(const GfxTex* t);
#endif
void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!g_dev || !t || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    BATCH_SETTLE(); /* (it may read what they draw into) */
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = readback_row(t, w);
    uint64_t size = (uint64_t)row * h;
    if (size > g_sync_rb_size)
    {
        defer((IUnknown*)g_sync_rb, NULL, -1);
        g_sync_rb = make_buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        g_sync_rb_size = g_sync_rb ? size : 0;
        if (!g_sync_rb)
            return;
    }
#ifdef VANACORE
    zl_on_read(t); /* the scene read back: its zone lines laid first */
#endif
    queue_readback(t, face, level, w, h, row, g_sync_rb);
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    submit(1);
    if (gfx_profiling)
        g_prof.probe_ns += gfx_now_ns() - t0;
    uint8_t* p = NULL;
    D3D12_RANGE r = { 0, (SIZE_T)size };
    if (SUCCEEDED(ID3D12Resource_Map(g_sync_rb, 0, &r, (void**)&p)))
    {
        copy_out(t, p, w, h, row, dst, pitch);
        D3D12_RANGE none = { 0, 0 };
        ID3D12Resource_Unmap(g_sync_rb, 0, &none);
    }
}

/* A surface read several times a frame (the game reuses one 16x16 target for more than one
 * probe: copy a region, lock, read; copy another, lock, read) keeps each read's history apart:
 * the k-th read this frame gets the k-th read of the newest frame the GPU has finished, never
 * another probe's pixels. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!g_dev || !t || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    BATCH_SETTLE();
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t row = readback_row(t, w);
    uint64_t size = (uint64_t)row * h;
    if (t->rb_frame != g_serial)
        t->rb_frame = g_serial, t->rb_count = 0;
    uint32_t index = t->rb_count++;
    if (index >= GFX_PROBES)
    {
        gfx_tex_read(t, face, level, dst, pitch); /* more reads a frame than we keep apart */
        return;
    }
    uint64_t done = completed();
    int best = -1;
    for (int i = 0; i < GFX_READBACKS; ++i)
        if (t->rb[i] && t->rb_serial[i] && t->rb_serial[i] <= done && t->rb_index[i] == index && t->rb_face[i] == face &&
            t->rb_level[i] == level && (best < 0 || t->rb_serial[i] > t->rb_serial[best]))
            best = i;
    if (best < 0)
        gfx_tex_read(t, face, level, dst, pitch); /* nothing finished for this read yet: wait, once */
    else
        copy_out(t, t->rb_cpu[best], w, h, row, dst, pitch);
    /* this read's copy for a later frame: a slot the GPU is done with, not the one just read */
    done = completed();
    int slot = -1;
    for (int i = 0; i < GFX_READBACKS && slot < 0; ++i)
        if (i != best && (!t->rb_serial[i] || t->rb_serial[i] <= done))
            slot = i;
    if (slot < 0)
        return;
    if (!t->rb[slot] || t->rb_size[slot] < size)
    {
        defer((IUnknown*)t->rb[slot], NULL, -1);
        t->rb[slot] = make_buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        t->rb_size[slot] = t->rb[slot] ? size : 0;
        t->rb_cpu[slot] = NULL;
        if (!t->rb[slot])
            return;
        ID3D12Resource_Map(t->rb[slot], 0, NULL, (void**)&t->rb_cpu[slot]); /* readback memory stays mapped */
    }
    queue_readback(t, face, level, w, h, row, t->rb[slot]);
    t->rb_serial[slot] = pending(), t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!g_dev || !src || !dst || src == dst || src->dxfmt != dst->dxfmt || src->conv != dst->conv || !w || !h)
        return;
    if (slevel >= src->levels || dlevel >= dst->levels || sface >= src->faces || dface >= dst->faces)
        return;
    MO_TOUCH(dst);
    BATCH_SETTLE(); /* (it may copy what they draw into) */
    if (src->use == GFX_USE_DEPTH) /* D3D12 copies depth whole subresources only */
    {
        uint32_t sw, sh, dw, dh;
        level_size(src, slevel, &sw, &sh);
        level_size(dst, dlevel, &dw, &dh);
        if (sx || sy || dx || dy || w != sw || h != sh || sw != dw || sh != dh)
            return;
    }
    tex_state(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex_state(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    from.pResource = src->res;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = subresource(src, sface, slevel);
    to.pResource = dst->res;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = subresource(dst, dface, dlevel);
    D3D12_BOX box = { sx, sy, 0, sx + w, sy + h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, dx, dy, 0, &from, src->use == GFX_USE_DEPTH ? NULL : &box);
    if (!dlevel)
        dst->scene = 0;
}

/* --- render targets ---------------------------------------------------------------------------------------------- */
/* Draws recorded since the last submission. A frame goes to the GPU in chunks - executed when the
 * targets change with this many behind it, or mid-scene every SPLIT_DRAWS - so the GPU works while
 * the game builds the rest, and a mid-frame readback (the game's per-frame probe) waits only for
 * the tail. */
#define CHUNK_DRAWS 96
#define SPLIT_DRAWS 320

static D3D12_CPU_DESCRIPTOR_HANDLE target_view(GfxTex* t, uint32_t face, uint32_t level)
{
    uint32_t i = subresource(t, face, level);
    Heap* h = t->use == GFX_USE_DEPTH ? &g_dsv : &g_rtv;
    if (t->views[i] < 0)
    {
        t->views[i] = heap_alloc(h);
        if (t->views[i] < 0)
        {
            fprintf(stderr, "[recomp] gfx: out of %s descriptors\n", t->use == GFX_USE_DEPTH ? "DSV" : "RTV");
            t->views[i] = 0;
        }
        if (t->use == GFX_USE_DEPTH)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt;
            v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipSlice = level;
            ID3D12Device_CreateDepthStencilView(g_dev, t->res, &v, heap_cpu(h, t->views[i]));
        }
        else
        {
            D3D12_RENDER_TARGET_VIEW_DESC v;
            memset(&v, 0, sizeof v);
            v.Format = t->dxfmt;
            if (t->type == GFX_TEX_CUBE)
            {
                v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.MipSlice = level, v.Texture2DArray.FirstArraySlice = face, v.Texture2DArray.ArraySize = 1;
            }
            else
                v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D, v.Texture2D.MipSlice = level;
            ID3D12Device_CreateRenderTargetView(g_dev, t->res, &v, heap_cpu(h, t->views[i]));
        }
    }
    return heap_cpu(h, t->views[i]);
}

static void color_size(uint32_t* w, uint32_t* h)
{
    level_size(g_rt, g_rt_level, w, h);
}

/* the depth surface for the current color target: the bound one, or a scratch one of the color
 * target's size when they differ (D3D8 lets depth be larger; D3D12 wants them equal) */
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

/* the targets bound on the command list, in their target states */
/* The normal-map detail target (nout_begin): while g_nout, the draw being encoded is a second pass of a normal-mapped draw,
 * into it, writing its bent view-space normal and its view z */
static int g_nout;
static D3D12_CPU_DESCRIPTOR_HANDLE nout_view(void);
/* The motion pass (mo_draw, [graphics.effects] dlss_motion): while g_mout, the draw being encoded is a moving thing's second
 * pass, into the motion target (mout_view), its pixels' motion in place of their colour: GfxVsKey.motion g_mo_key, the frame
 * before's transform in the Sun buffer (g_mo_m), the least alpha or light of its pixels that count after it (sun_f.x,
 * g_mo_f), and the frame before's positions, where they're its own, in stream g_mo_slot (at g_mo_va) */
static int g_mout;
static int g_maskout; /* ... with g_mout: the plants' mask for FSR (mask_draw), into its own target (R16G16_FLOAT), no motion */
static uint8_t g_mo_key;
static int g_mo_slot = -1;
static D3D12_GPU_VIRTUAL_ADDRESS g_mo_va;
static float g_mo_m[16];
static float g_mo_f[4];
static D3D12_CPU_DESCRIPTOR_HANDLE mout_view(void);
#ifdef VANACORE
/* The scene's draw just encoded, while gfx_draw asks (g_enc_take): what it bound, which its redraws kept to be drawn later
 * (mo_keep) bind again - its textures, its uniforms and its streams' and indices' copies in the ring */
static struct
{
    const GfxDraw* d; /* the draw, once it's encoded */
    uint32_t bind[16];
    GfxTex* tex[8]; /* the textures bound (as read: dlss_sampled), NULL where none is */
    D3D12_GPU_VIRTUAL_ADDRESS vb[GFX_NSTREAMS], ub;
    D3D12_INDEX_BUFFER_VIEW ibv; /* BufferLocation 0: drawn without indices */
} g_enc;
static int g_enc_take;
#endif

static int bind_targets(void)
{
    if (!g_rt)
        return 0;
    ID3D12GraphicsCommandList* l = list();
    GfxTex* ds = depth_attachment();
    if (g_rt->state != D3D12_RESOURCE_STATE_RENDER_TARGET || (ds && ds->state != D3D12_RESOURCE_STATE_DEPTH_WRITE))
        g_targets_bound = 0;
    tex_state(g_rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ds)
        tex_state(ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (ds && !g_rt_face && !g_rt_level)
        g_rt->depth_seen = ds; /* the scene effects read it */
    if (!g_rt_face && !g_rt_level)
        g_rt->scene = 0; /* drawn to: its chain is behind */
    if (!g_targets_bound)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_mout ? mout_view() : g_nout ? nout_view() : target_view(g_rt, g_rt_face, g_rt_level), dsv;
        if (ds)
            dsv = target_view(ds, 0, 0);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, ds ? &dsv : NULL);
        uint32_t w, h;
        color_size(&w, &h);
        D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        g_targets_bound = 1;
    }
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (!g_dev || (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds))
        return;
    BATCH_SETTLE(); /* the zone's draws kept: into the targets they were drawn for */
#ifdef VANACORE
    if (g_mob_n) /* the moving things' redraws waiting: drawn against the depth they were kept with, as it is */
        mo_flush(1);
#endif
    if (g_cmd_draws >= CHUNK_DRAWS)
        submit(0);
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
    g_targets_bound = 0;
}

/* --- pipelines, built off the game's thread ---------------------------------------------------------------
 * A pipeline for a key seen for the first time is built on the thread pool; the draws that need it
 * are skipped until it is ready (a new effect may miss its first frames; the game never waits for
 * the shader compiler). Every key built is recorded in the pipeline cache file, and at start-up the
 * recorded keys are built again in the background, so a second session has them before it needs
 * them. gfx_set_sync_pipelines(1) (the tests) builds in place and records nothing. */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

static int alpha_tested(const GfxFsKey* k) { return k->alpha_func && k->alpha_func != 8; }

typedef struct PipeKey
{
    LibKey lib;
    GfxPipeKey pipe;
    GfxDepthKey depth;
    uint32_t color, dsv; /* DXGI_FORMAT */
    int32_t zbias;
    uint8_t x8, cull, fill, topo; /* topo: D3D12_PRIMITIVE_TOPOLOGY_TYPE */
} PipeKey;

static char g_pipe_failed; /* PIPE_FAILED: a pipeline that failed to build, at an address no pipeline has ((void*)1 before) */
#define PIPE_FAILED ((void*)&g_pipe_failed)
#define PIPE_MAGIC 0x31443344u /* "D3D1" */

typedef struct PipeEntry
{
    void* volatile state; /* NULL while building, PIPE_FAILED, or the ID3D12PipelineState */
    void* volatile job;   /* the start-up rebuild's PipeJob until a thread takes it to build (warm_callback) */
    int hurried;          /* a draw wanted it before the rebuild got to it: sent ahead on the default pool (pipeline_for) */
} PipeEntry;

typedef struct PipeJob
{
    PipeKey k;
    uint32_t *vs, *ps; /* copies of the shader tokens behind k.lib.vs.prog / fs.prog */
    uint32_t nvs, nps;
    PipeEntry* e;
    int record;
} PipeJob;

/* compiled shaders, shared by the pipelines that differ only in fixed state (the thread pool's) */
typedef struct Shaders
{
    ID3DBlob *vs, *ps;
} Shaders;

static int g_sync_pipelines;
static char g_pipe_cache[1024];
static CRITICAL_SECTION g_pipe_file_lock;
static SRWLOCK g_shader_lock = SRWLOCK_INIT;
static Map g_shaders; /* LibKey -> Shaders*, under g_shader_lock */
static volatile LONG g_pipes_building;
static volatile LONG g_warm_calls;  /* warm_callback's submissions not yet run: each holds its entry (builds_done waits for them) */
static volatile LONG g_warm_cancel; /* the device going (gfx_shutdown): the rebuild's jobs not yet begun are dropped, not built */

void gfx_set_sync_pipelines(int on) { g_sync_pipelines = on; }

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

/* Compiled shaders kept on disk (FFXI_CACHE_DIR, or %LOCALAPPDATA%/FFXI, in shaders/): D3DCompile takes most of a second
 * for the scene effects' or a big pipeline's, and every launch made them again - the effects came on seconds into the
 * first zone, and a pipeline new to the session left its draws out for a frame or two. Each by a hash of its source,
 * entry point, target and flags; written to a file of its own, then renamed, so a reader never sees half of one. */
static char g_shader_dir[900];

static void shader_cache_init(void)
{
    const char* dir = getenv("FFXI_CACHE_DIR");
    char base[800];
    if (dir && *dir)
        snprintf(base, sizeof base, "%s", dir);
    else if (getenv("LOCALAPPDATA"))
        snprintf(base, sizeof base, "%s\\FFXI", getenv("LOCALAPPDATA"));
    else
        return;
    CreateDirectoryA(base, NULL);
    snprintf(g_shader_dir, sizeof g_shader_dir, "%s\\shaders", base);
    CreateDirectoryA(g_shader_dir, NULL);
}

static ID3DBlob* shader_cache_read(const char* path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    ID3DBlob* b = NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    if (size >= 32 && size < (16u << 20) && SUCCEEDED(D3DCreateBlob(size, &b)))
    {
        uint8_t* p = (uint8_t*)ID3D10Blob_GetBufferPointer(b);
        uint32_t total = 0;
        if (!ReadFile(f, p, size, &got, NULL) || got != size || memcmp(p, "DXBC", 4) || (memcpy(&total, p + 24, 4), total != size))
        {
            ID3D10Blob_Release(b);
            b = NULL;
        }
    }
    CloseHandle(f);
    return b;
}

static void shader_cache_write(const char* path, ID3DBlob* code)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD put = 0, size = (DWORD)ID3D10Blob_GetBufferSize(code);
    int ok = WriteFile(f, ID3D10Blob_GetBufferPointer(code), size, &put, NULL) && put == size;
    CloseHandle(f);
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileA(tmp);
}

static ID3DBlob* compile(const char* src, const char* entry, const char* target)
{
    char cached[1024] = "";
    if (g_shader_dir[0])
    {
        const UINT flags = D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        uint64_t h = fnv(src, strlen(src));
        h ^= fnv(entry, strlen(entry)) * 0x9E3779B97F4A7C15ull;
        h ^= (fnv(target, strlen(target)) + flags) * 0xC2B2AE3D27D4EB4Full;
        snprintf(cached, sizeof cached, "%s\\%016llx.dxbc", g_shader_dir, (unsigned long long)h);
        ID3DBlob* b = shader_cache_read(cached);
        if (b)
            return b;
    }
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target,
        D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: HLSL compile failed (%s): %s\n%s\n", entry, err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?",
            src);
        code = NULL;
    }
    if (err)
        ID3D10Blob_Release(err);
    if (code && cached[0])
        shader_cache_write(cached, code);
    return code;
}

static Shaders* shaders(const LibKey* k, const uint32_t* vs, const uint32_t* ps)
{
    AcquireSRWLockShared(&g_shader_lock);
    Shaders* s = (Shaders*)map_get(&g_shaders, k, sizeof *k);
    ReleaseSRWLockShared(&g_shader_lock);
    if (s)
        return s->vs && s->ps ? s : NULL;
    s = (Shaders*)calloc(1, sizeof *s);
    char* src = gfx_hlsl_generate(&k->vs, &k->fs, vs, ps);
    if (src)
    {
        s->vs = compile(src, "vs_main", "vs_5_1");
        s->ps = s->vs ? compile(src, "fs_main", "ps_5_1") : NULL;
        free(src);
    }
    else
        InterlockedIncrement(&g_failures);
    AcquireSRWLockExclusive(&g_shader_lock);
    Shaders* had = (Shaders*)map_get(&g_shaders, k, sizeof *k);
    if (had) /* another job compiled it meanwhile */
    {
        if (s->vs)
            ID3D10Blob_Release(s->vs);
        if (s->ps)
            ID3D10Blob_Release(s->ps);
        free(s);
        s = had;
    }
    else
        map_put(&g_shaders, k, sizeof *k, s);
    ReleaseSRWLockExclusive(&g_shader_lock);
    return s->vs && s->ps ? s : NULL;
}

static D3D12_BLEND blend_factor(uint32_t f, int x8, int alpha)
{
    switch (f)
    {
    case 1: return D3D12_BLEND_ZERO;
    case 2: return D3D12_BLEND_ONE;
    case 3: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR; /* the alpha factors take no colors */
    case 4: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 5: return D3D12_BLEND_SRC_ALPHA;
    case 6: return D3D12_BLEND_INV_SRC_ALPHA;
    case 7: return x8 ? D3D12_BLEND_ONE : D3D12_BLEND_DEST_ALPHA;
    case 8: return x8 ? D3D12_BLEND_ZERO : D3D12_BLEND_INV_DEST_ALPHA;
    case 9: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case 10: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 11: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}

static D3D12_BLEND_OP blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D12_BLEND_OP_SUBTRACT;
    case 3: return D3D12_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D12_BLEND_OP_MIN;
    case 5: return D3D12_BLEND_OP_MAX;
    default: return D3D12_BLEND_OP_ADD;
    }
}

static D3D12_COMPARISON_FUNC compare(uint32_t f)
{
    switch (f)
    {
    case 1: return D3D12_COMPARISON_FUNC_NEVER;
    case 2: return D3D12_COMPARISON_FUNC_LESS;
    case 3: return D3D12_COMPARISON_FUNC_EQUAL;
    case 4: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 5: return D3D12_COMPARISON_FUNC_GREATER;
    case 6: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 7: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

static D3D12_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return D3D12_STENCIL_OP_ZERO;
    case 3: return D3D12_STENCIL_OP_REPLACE;
    case 4: return D3D12_STENCIL_OP_INCR_SAT;
    case 5: return D3D12_STENCIL_OP_DECR_SAT;
    case 6: return D3D12_STENCIL_OP_INVERT;
    case 7: return D3D12_STENCIL_OP_INCR;
    case 8: return D3D12_STENCIL_OP_DECR;
    default: return D3D12_STENCIL_OP_KEEP;
    }
}

/* the pipeline for a key, or NULL */
static ID3D12PipelineState* build_pipeline(const PipeKey* k, const uint32_t* vs, const uint32_t* ps)
{
    Shaders* s = shaders(&k->lib, vs, ps);
    if (!s)
        return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = g_root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s->vs), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(s->vs);
    /* drawn into the sun's shadow map: depth alone, but for an alpha test or a fade (shadow 2) */
    int shadow = k->lib.vs.shadow;
    if (!shadow || shadow == 2 || alpha_tested(&k->lib.fs))
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(s->ps), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(s->ps);
    D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
    uint32_t wm = k->pipe.write_mask;
    c->RenderTargetWriteMask = (UINT8)(((wm & 1) ? D3D12_COLOR_WRITE_ENABLE_RED : 0) | ((wm & 2) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((wm & 4) ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) | ((wm & 8) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0));
    c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
    c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
    c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    c->LogicOp = D3D12_LOGIC_OP_NOOP;
    if (k->pipe.blend)
    {
        uint32_t sf = k->pipe.src, df = k->pipe.dst;
        if (sf == 12) /* BOTHSRCALPHA */
            sf = 5, df = 6;
        else if (sf == 13) /* BOTHINVSRCALPHA */
            sf = 6, df = 5;
        c->BlendEnable = TRUE;
        c->SrcBlend = blend_factor(sf, k->x8, 0), c->SrcBlendAlpha = blend_factor(sf, k->x8, 1);
        c->DestBlend = blend_factor(df, k->x8, 0), c->DestBlendAlpha = blend_factor(df, k->x8, 1);
        c->BlendOp = c->BlendOpAlpha = blend_op(k->pipe.op);
#ifdef VANACORE
        if (k->pipe.over) /* the interface layer: its alpha is how much of the frame each pixel covers */
            c->SrcBlendAlpha = D3D12_BLEND_ONE, c->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA, c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
#endif
    }
    pd.SampleMask = UINT_MAX;
    /* D3D's front faces are clockwise on screen; CULL_CCW (the default) culls the back ones */
    pd.RasterizerState.FillMode = k->fill == 2 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = k->cull == 3 ? D3D12_CULL_MODE_BACK : k->cull == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    /* D3DRS_ZBIAS (0-16, what the game's decals use to sit on a wall): a depth unit a step, and a little for a slope,
       the rest a distance in the vertex function (d3d8.c, gfx_hlsl_vs_return). 84 units a step (5e-6 of the depth range,
       as the D3D9 path has it) is a third of a unit at 30 yalms with the game's near plane at 0.1, metres further off:
       the walls' weathering came through the banners hung in front of them. */
    pd.RasterizerState.DepthBias = -k->zbias;
    pd.RasterizerState.SlopeScaledDepthBias = -(float)k->zbias * 0.03125f;
    if (shadow) /* the map's own bias: surfaces at a slant to the sun are pushed back more */
        pd.RasterizerState.DepthBias = 0, pd.RasterizerState.SlopeScaledDepthBias = 1.5f;
    pd.RasterizerState.DepthClipEnable = k->depth.noclip ? FALSE : TRUE; /* (VanaCore's sea past the far plane: GfxDepthKey) */
    const GfxDepthKey* dk = &k->depth;
    pd.DepthStencilState.DepthEnable = dk->zenable ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = dk->zenable && dk->zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = dk->zenable ? compare(dk->zfunc) : D3D12_COMPARISON_FUNC_ALWAYS;
    if (dk->stencil)
    {
        D3D12_DEPTH_STENCILOP_DESC st = { stencil_op(dk->sfail), stencil_op(dk->szfail), stencil_op(dk->spass), compare(dk->sfunc) };
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = dk->sread;
        pd.DepthStencilState.StencilWriteMask = dk->swrite;
        pd.DepthStencilState.FrontFace = st;
        pd.DepthStencilState.BackFace = st;
    }
    pd.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)k->topo;
    pd.NumRenderTargets = k->color != DXGI_FORMAT_UNKNOWN ? 1 : 0; /* none: the shadow map's depth alone */
    pd.RTVFormats[0] = (DXGI_FORMAT)k->color;
    pd.DSVFormat = (DXGI_FORMAT)k->dsv;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p);
    if (FAILED(hr))
    {
        InterlockedIncrement(&g_failures);
        if (!failed_lost(hr, "making a pipeline"))
            fprintf(stderr, "[recomp] gfx: pipeline failed (%08lx)\n", (unsigned long)hr);
        return NULL;
    }
    return p;
}

/* one record of the cache file: magic, the key, then each shader's tokens (count first) */
static void record_job(const PipeJob* j)
{
    if (!g_pipe_cache[0])
        return;
    uint32_t hdr[2] = { PIPE_MAGIC, (uint32_t)sizeof(PipeKey) };
    EnterCriticalSection(&g_pipe_file_lock);
    FILE* f = fopen(g_pipe_cache, "ab");
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
    LeaveCriticalSection(&g_pipe_file_lock);
}

static void run_job(PipeJob* j)
{
    ID3D12PipelineState* p = build_pipeline(&j->k, j->vs, j->ps);
    InterlockedExchangePointer(&j->e->state, p ? (void*)p : PIPE_FAILED);
    if (p && j->record)
        record_job(j);
    free(j->vs);
    free(j->ps);
    free(j);
    InterlockedDecrement(&g_pipes_building);
}

static void CALLBACK job_callback(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst;
    run_job((PipeJob*)ctx);
}

/* a start-up rebuild's pipeline, built by whichever comes first: its turn on the rebuild's few threads, or a draw wanting it
 * (pipeline_for); the other finds the job taken and does nothing. With the device going, a job not yet begun is dropped */
static void CALLBACK warm_callback(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst;
    PipeEntry* e = (PipeEntry*)ctx;
    PipeJob* j = (PipeJob*)InterlockedExchangePointer(&e->job, NULL);
    if (j && InterlockedCompareExchange(&g_warm_cancel, 0, 0))
    {
        InterlockedExchangePointer(&e->state, PIPE_FAILED);
        free(j->vs);
        free(j->ps);
        free(j);
        InterlockedDecrement(&g_pipes_building);
    }
    else if (j)
        run_job(j);
    InterlockedDecrement(&g_warm_calls); /* the entry isn't touched after this: gfx_shutdown may free it */
}

static int warm_submit(PipeEntry* e, PTP_CALLBACK_ENVIRON env)
{
    InterlockedIncrement(&g_warm_calls);
    if (TrySubmitThreadpoolCallback(warm_callback, e, env))
        return 1;
    InterlockedDecrement(&g_warm_calls);
    return 0;
}

/* warm: one of the start-up rebuild's (prewarm_pipelines), on its own pool (gfx_warm_env) */
static void queue_job(const PipeKey* k, const uint32_t* vs, uint32_t nvs, const uint32_t* ps, uint32_t nps, int record, int warm)
{
    if (map_get(&g_pipes, k, sizeof *k))
        return;
    PipeEntry* e = (PipeEntry*)calloc(1, sizeof *e);
    map_put(&g_pipes, k, sizeof *k, e);
    PipeJob* j = (PipeJob*)calloc(1, sizeof *j);
    j->k = *k, j->e = e, j->record = record;
    if (nvs)
        j->vs = (uint32_t*)malloc(4u * nvs), memcpy(j->vs, vs, 4u * nvs), j->nvs = nvs;
    if (nps)
        j->ps = (uint32_t*)malloc(4u * nps), memcpy(j->ps, ps, 4u * nps), j->nps = nps;
    g_prof.pipelines++;
    InterlockedIncrement(&g_pipes_building);
    if (warm && !g_sync_pipelines)
    {
        e->job = j;
        if (warm_submit(e, (PTP_CALLBACK_ENVIRON)gfx_warm_env()))
            return;
        e->job = NULL; /* the game's thread's: no draw has seen the entry yet */
    }
    if (g_sync_pipelines || !TrySubmitThreadpoolCallback(job_callback, j, NULL))
        run_job(j);
}

/* at start-up: the keys earlier sessions built, built again in the background */
static void prewarm_pipelines(void)
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
    snprintf(g_pipe_cache, sizeof g_pipe_cache, "%s\\pipelines.d3d12.v1", path);
    InterlockedExchange(&g_warm_cancel, 0); /* a new device: what gfx_shutdown dropped went with the old one's entries */
    FILE* f = fopen(g_pipe_cache, "rb");
    if (!f)
        return;
    uint32_t n = 0, hdr[2];
    while (fread(hdr, 4, 2, f) == 2 && hdr[0] == PIPE_MAGIC && hdr[1] == sizeof(PipeKey))
    {
        PipeKey k;
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
    fprintf(stderr, "[recomp] gfx: building %u pipelines from %s in the background, %u at a time\n", n, g_pipe_cache,
        gfx_warm_threads() ? gfx_warm_threads() : n);
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case GFX_LINELIST:
    case GFX_LINESTRIP: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

static int g_pipe_waiting; /* pipeline_for's last NULL was a pipeline still building (not one that failed) */

/* the pipeline for a key, queued for building the first time (NULL until built) */
static ID3D12PipelineState* pipeline_for(const PipeKey* k, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    g_pipe_waiting = 0;
    /* draws in a row mostly share a pipeline: the last one's key is compared before any hashing (the moving things' redraws,
     * kept with each of the scene's - mo_keep - the last of their own) */
    static PipeKey last_keys[2];
    static PipeEntry* lasts[2];
    static uint32_t last_gen;
    if (last_gen != g_device_gen) /* the device torn down: the entries went with their map */
        lasts[0] = lasts[1] = NULL, last_gen = g_device_gen;
    PipeKey* const last_key = &last_keys[g_mout != 0];
    PipeEntry** const last = &lasts[g_mout != 0];
    PipeEntry* e = *last && !memcmp(k, last_key, sizeof *k) ? *last : NULL;
    if (!e)
    {
        e = (PipeEntry*)map_get(&g_pipes, k, sizeof *k);
        if (!e)
        {
            uint32_t nvs = 0, nps = 0;
            uint32_t* vs = k->lib.vs.prog ? copy_tok(vs_tokens, &nvs) : NULL;
            uint32_t* ps = k->lib.fs.prog ? copy_tok(ps_tokens, &nps) : NULL;
            queue_job(k, vs, nvs, ps, nps, !g_sync_pipelines, 0);
            free(vs);
            free(ps);
            e = (PipeEntry*)map_get(&g_pipes, k, sizeof *k);
        }
        if (!e)
            return NULL;
        *last_key = *k, *last = e;
    }
    void* st = InterlockedCompareExchangePointer(&e->state, NULL, NULL);
    if (!st && e->job && !e->hurried)
    {
        /* still waiting its turn in the start-up rebuild: a draw wants it now, so it goes ahead on the default pool */
        e->hurried = 1;
        warm_submit(e, NULL);
    }
    g_pipe_waiting = st == NULL;
    return st && st != PIPE_FAILED ? (ID3D12PipelineState*)st : NULL;
}

#ifdef VANACORE
/* The characters' stencil bit (ao_chars, Render visual 19): a character's draw (GfxDraw is_char, d3d8.c is_character) writes
 * CHAR_STENCIL into the scene's stencil as it draws - exact, in the same frame, where the characters' depth drawn again and
 * matched to the scene's missed by a hair, a flicker - and fx_cmask reads it back as a mask for the occlusion (fx_ao). The depth
 * is cleared with it each frame (gfx_clear). Where the depth has no stencil, or the game uses the stencil itself, it's the
 * characters' depth drawn again (sun_map_view) as it was. */
#define CHAR_STENCIL 0x80
static int g_game_stencil; /* the game drew with the stencil on (a draw that isn't a character's): left as it is, no bit of ours */
static int cmask_usable(const GfxTex* ds);
#endif

static ID3D12PipelineState* pipeline(const GfxDraw* d, GfxTex* ds)
{
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.lib.vs = d->vs, k.lib.fs = d->fs, k.pipe = d->pipe;
    k.color = (uint32_t)g_rt->dxfmt;
    k.dsv = ds ? (uint32_t)ds->dxfmt : (uint32_t)DXGI_FORMAT_UNKNOWN;
    if (ds)
    {
        k.depth = d->depth;
        if (!ds->has_stencil)
            k.depth.stencil = 0;
#ifdef VANACORE
        if (k.depth.stencil && !g_game_stencil)
        {
            g_game_stencil = 1;
            fprintf(stderr, "[recomp] gfx: the game draws with the stencil: the characters' share of the occlusion (ao_chars) stays on "
                            "their depth drawn again\n");
        }
        if (!k.depth.stencil && d->is_char && cmask_usable(ds))
            k.depth.stencil = 1, k.depth.sfail = k.depth.szfail = 1, k.depth.spass = 3, k.depth.sfunc = 8, k.depth.sread = 0xFF,
            k.depth.swrite = CHAR_STENCIL; /* KEEP, KEEP, REPLACE, ALWAYS: our bit only */
#endif
        if (!k.depth.stencil)
            k.depth.sfail = k.depth.szfail = k.depth.spass = k.depth.sfunc = k.depth.sread = k.depth.swrite = 0;
    }
    k.x8 = (uint8_t)g_rt->x8;
    k.cull = d->cull, k.fill = d->fill == 2 ? 2 : 3;
    k.zbias = ds ? d->zbias : 0;
    k.topo = (uint8_t)topology_type(d->prim);
    /* the world's lit draws lit per pixel (gfx_hlsl.c pixel_lit) */
    if (g_fxs.fx != 0.0f && g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat)
        k.lib.vs.pixel = g_fxs.light >= 2.0f ? 2 : 1; /* 1: the sun per pixel, the game's torches per vertex */
    /* the normal map bends the lit pixel function's normal: nothing to bend without it. The game's own shaders (a vertex shader and
     * a pixel shader) are lit again by it instead, from the vertex shader's clip-space position (gfx_hlsl.c emit_normal_relight) */
    if (g_fxs.normalmap < 0.5f || (k.lib.fs.prog ? !k.lib.vs.prog : !k.lib.vs.pixel))
        k.lib.fs.nmap = 0;
    else if (k.lib.fs.nmap)
    {
        k.lib.fs.nmap = (uint8_t)(1 + (g_fxs.normalmap_view >= 1.5f ? 2 : g_fxs.normalmap_view >= 0.5f ? 1 : 0));
        k.lib.vs.cpos = k.lib.fs.prog != 0;
    }
    if (g_nout && k.lib.fs.nmap) /* the detail pass: the bent normal and z into the detail target, tested against the scene's depth */
    {
        k.lib.fs.nmap = k.lib.fs.nmap == 3 ? 5 : 4; /* (5: the maps' Y turned over, as lit) */
        k.color = (uint32_t)DXGI_FORMAT_R16G16B16A16_FLOAT;
        memset(&k.pipe, 0, sizeof k.pipe);
        k.pipe.write_mask = 15;
        memset(&k.depth, 0, sizeof k.depth);
        k.depth.zenable = 1, k.depth.zfunc = 4; /* D3DCMP_LESSEQUAL, no write */
        k.x8 = 0;
    }
    if (g_mout) /* the motion pass (mo_draw): each shown pixel's motion into the motion target, tested against the scene's depth as
                 * the draw was (an opaque one's own depth, which it drew: equal), lit by nothing */
    {
        k.lib.vs.motion = g_mo_key;
        k.lib.vs.pixel = 0, k.lib.vs.cpos = 0, k.lib.fs.nmap = 0;
        k.color = (uint32_t)(g_maskout ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT);
        memset(&k.pipe, 0, sizeof k.pipe);
        k.pipe.write_mask = 15;
        k.depth.zwrite = 0;
        if (k.depth.zenable)
            k.depth.zfunc = 4; /* D3DCMP_LESSEQUAL */
        k.depth.stencil = k.depth.sfail = k.depth.szfail = k.depth.spass = k.depth.sfunc = k.depth.sread = k.depth.swrite = 0;
        k.x8 = 0;
    }
#ifdef VANACORE
    void rt_log(const char* fmt, ...);
    if (d->fs.nmap) /* what became of the draws that have a normal map (the log says once per texture why it's unused) */
    {
        static GfxTex* seen[64];
        static uint32_t nseen, total, used;
        total++, used += k.lib.fs.nmap != 0;
        if (!k.lib.fs.nmap && d->tex[0])
        {
            uint32_t i = 0;
            while (i < nseen && seen[i] != d->tex[0])
                ++i;
            if (i == nseen && nseen < 64)
            {
                seen[nseen++] = d->tex[0];
                rt_log("[gfx] a normal map is unused on a %ux%u draw: lighting %d, flat %d, screen-space %d, vertex shader %d, light setting %.0f, "
                       "effects %.0f (draws with maps so far: %u, used: %u)\n", d->tex[0]->w, d->tex[0]->h, d->vs.lighting, d->vs.flat, d->vs.rhw,
                    d->vs.prog != 0, g_fxs.light, g_fxs.fx, total, used);
            }
        }
    }
#endif
    ID3D12PipelineState* p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    if (!p && k.lib.vs.pixel)
    {
        /* lit per pixel, still building: lit per vertex meanwhile (a new mix of the game's lights
         * would blink the object out for a few frames) */
        k.lib.vs.pixel = 0;
        k.lib.fs.nmap = 0;
        p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    }
    else if (!p && k.lib.vs.cpos)
    {
        /* the shader draw's normal-map variant is still building: drawn without it meanwhile, not left out */
        k.lib.vs.cpos = 0;
        k.lib.fs.nmap = 0;
        p = pipeline_for(&k, d->vs_tokens, d->ps_tokens);
    }
    return p;
}

static D3D12_TEXTURE_ADDRESS_MODE address(uint32_t a)
{
    switch (a)
    {
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

/* the sampler heap slot for a key: made on first use, kept */
static uint32_t sampler(const GfxSampler* k)
{
    void* s = map_get(&g_samplers, k, sizeof *k);
    if (s)
        return (uint32_t)(uintptr_t)s - 1;
    int32_t slot = heap_alloc(&g_samp);
    if (slot < 0)
        return g_present_samp; /* the heap is full: any sampler beats none */
    D3D12_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    D3D12_FILTER_TYPE mn = k->min >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mg = k->mag >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mp = k->mip >= 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(mn, mg, mp, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.MaxAnisotropy = 1;
    if ((k->min == 3 || k->mag == 3) && k->max_aniso > 1)
        sd.Filter = D3D12_FILTER_ANISOTROPIC, sd.MaxAnisotropy = k->max_aniso > 16 ? 16 : k->max_aniso;
    sd.AddressU = address(k->addr_u);
    sd.AddressV = address(k->addr_v);
    sd.AddressW = address(k->addr_w);
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.BorderColor[0] = ((k->border >> 16) & 255) / 255.0f;
    sd.BorderColor[1] = ((k->border >> 8) & 255) / 255.0f;
    sd.BorderColor[2] = (k->border & 255) / 255.0f;
    sd.BorderColor[3] = (k->border >> 24) / 255.0f;
    sd.MipLODBias = k->lod_bias / 16.0f;
    sd.MinLOD = k->max_level;
    sd.MaxLOD = k->mip == 0 ? (float)k->max_level : D3D12_FLOAT32_MAX; /* no mipmapping: that one level */
    if (k->lod_cap && sd.MaxLOD > (float)(k->lod_cap - 1))
        sd.MaxLOD = (float)(k->lod_cap - 1);
    ID3D12Device_CreateSampler(g_dev, &sd, heap_cpu(&g_samp, slot));
    map_put(&g_samplers, k, sizeof *k, (void*)(uintptr_t)(slot + 1));
    return (uint32_t)slot;
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
/* What the game's draws last bound on the command list, so a draw binds only what changed (each
 * binding is a trip into the driver; the game makes 1,000-2,000 draws a frame). Forgotten (all
 * ones: no binding looks like that) with each new command list and after the passes that bind
 * their own. */
static struct
{
    uint32_t bind[16];
    D3D12_GPU_VIRTUAL_ADDRESS stream[GFX_NSTREAMS];
    D3D12_VIEWPORT vp;
    D3D12_INDEX_BUFFER_VIEW ibv;
    uint64_t stencil_ref;
} g_drawn;

static void drawn_forget(void) { memset(&g_drawn, 0xFF, sizeof g_drawn); }

static void set_index_buffer(const D3D12_INDEX_BUFFER_VIEW* v)
{
    if (memcmp(v, &g_drawn.ibv, sizeof *v))
        ID3D12GraphicsCommandList_IASetIndexBuffer(g_list, v), g_drawn.ibv = *v;
}

/* a draw's viewport (x, y, w, h, then the depth range's floats), within the target's size */
static D3D12_VIEWPORT viewport_for(const uint32_t vp[6])
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
    D3D12_VIEWPORT v = { x, y, vw, vh, zmin, zmax };
    return v;
}

static void set_viewport(const uint32_t vp[6])
{
    const D3D12_VIEWPORT v = viewport_for(vp);
    if (memcmp(&v, &g_drawn.vp, sizeof v))
        ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &v), g_drawn.vp = v;
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

static D3D12_PRIMITIVE_TOPOLOGY topology(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case GFX_LINELIST: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case GFX_LINESTRIP: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case GFX_TRIANGLESTRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static void set_pso(ID3D12PipelineState* p)
{
    if (p != g_bound_pso)
        ID3D12GraphicsCommandList_SetPipelineState(g_list, p), g_bound_pso = p;
}

static void set_topology(D3D12_PRIMITIVE_TOPOLOGY t)
{
    if ((int)t != g_bound_topo)
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, t), g_bound_topo = (int)t;
}


/* --- the sun's shadow map: the scene's casters, drawn again from the sun (gfx_scene_done) --------------
 * Each opaque draw of the scene (GfxDraw.caster) is recorded as it is encoded - its pipeline key and
 * the GPU addresses of its streams, uniforms and indices (the ring's, alive until the frame's work
 * is done, or a static buffer's), and for an alpha test its textures' heap slots - and when the
 * scene is done they are drawn again into a depth map from the sun: the same functions, with the
 * clip-space position they make taken through the camera's inverse into the sun's view (the shadow
 * key, gfx_hlsl_vs_return). The scene effects then look each pixel up in it. (gfx_metal.m's) */
typedef struct Caster
{
    LibKey lib;
    const uint32_t *vs, *ps; /* the game's shader tokens behind lib.vs.prog / fs.prog */
    D3D12_GPU_VIRTUAL_ADDRESS vb[GFX_NSTREAMS], ub;
    D3D12_INDEX_BUFFER_VIEW ibv; /* BufferLocation 0: no indices */
    GfxBuf* buf[GFX_NSTREAMS + 1]; /* the static buffers it draws from (the last: its indices); the ring's are NULL */
    GfxTex* tex[8];                /* an alpha test's textures, and their heap slots with the samplers' */
    uint32_t bind[16];
    uint32_t n, vstart, need; /* need: the uniforms' bytes */
    uint32_t cpu_u;           /* its uniforms in g_caster_u (for the zone's cache), or ~0 */
    uint8_t topo;             /* D3D12_PRIMITIVE_TOPOLOGY */
    uint8_t fixed;            /* every vertex and index from buffers the game keeps (the zone's): cached (sun_cache) */
    uint8_t dead;             /* a buffer or texture it draws from went, or changed: not drawn */
    uint8_t has_clip0;        /* clip0 is known (a fixed one: gfx_draw_clip0) */
    uint8_t own;              /* vertices or indices of a buffer the game rewrites, as drawn, in the ring (caster_bytes) */
    uint8_t plant;            /* a tree's, a bush's or grass (draw_plant): left out of the sun's maps with sun_plants 0 */
    uint8_t sought;           /* a piece of the zone round the player, drawn for the sun's maps alone by the caster pass (GfxDraw.sought,
                               * caster_only), not in the scene */
    uint8_t world;            /* ... in world space (GfxDraw.sought 2): its uniforms' transform is its world matrix alone, so it goes
                               * into a map by the map's own matrix, not through the camera (sun_draw); clip0 is where it stands */
    float clip0[4];           /* its first vertex's clip-space position: where it stands (sun_cache_update) */
    uint32_t piece;           /* the caster pass's piece it's of (GfxDraw.piece), 0 none, and its place among the piece's draws: */
    uint16_t piece_draw;      /* the cache keeps it by these (gfx_sun_cache.h piece_see) */
    uint8_t snapped;          /* kept as a copy the caster pass has drawn (snap_note: from a buffer the game rewrites, a tree it sways),
                               * the copy drawn in its place while the pass runs (live_left_out, snap_drawn) */
    ID3D12PipelineState* sun_ps; /* the pipeline it's drawn into the sun's maps with, once found (sun_draw) */
    ID3D12PipelineState* sun_fps; /* ... and fading out of them (GfxVsKey.shadow 2, cache_let_go) */
    ID3D12PipelineState* vn_ps;   /* ... and its smooth normal into the detail target (vn_replay) */
    uint32_t entry;             /* the cache's entry it was drawn as this scene, index + 1 (sun_cache_update); 0 none */
} Caster;

/* A caster's uniforms its shadow reads (sun_draw): all of them when it's alpha tested; else its position's alone -
 * the transforms, the viewport, the vertices' layout and a vertex shader's constants (that pipeline has no lighting,
 * texturing or fog): a half to a fifth of the bytes, for each of the thousands of an open zone's casters a frame */
static void sun_u_copy(void* to, const void* from, const Caster* c)
{
    if (alpha_tested(&c->lib.fs))
    {
        memcpy(to, from, c->need);
        return;
    }
    uint8_t* d = (uint8_t*)to;
    const uint8_t* s = (const uint8_t*)from;
    memcpy(d, s, offsetof(GfxU, wvit)); /* wvp, wv */
    memcpy(d + offsetof(GfxU, vp), s + offsetof(GfxU, vp), offsetof(GfxU, light) - offsetof(GfxU, vp));
    if (c->lib.vs.prog && c->need >= offsetof(GfxU, psc))
        memcpy(d + offsetof(GfxU, vsc), s + offsetof(GfxU, vsc), offsetof(GfxU, psc) - offsetof(GfxU, vsc));
}

static Caster* g_casters;
static uint32_t g_ncasters, g_casters_cap;
static int snap_note(const GfxDraw* d, const Caster* rec);
#define SNAP_VOLATILE 300 /* frames */
/* a buffer the game has written again lately: the cache can't draw from it later (a copy is kept) */
#define VOLATILE(b) ((b)->uploads > 1 && (b)->upload_frame + SNAP_VOLATILE > g_serial)
/* ... and written again within a frame lately: between draws from it, so what was drawn before is gone by the scene's end */
#define REWRITTEN(b) ((b)->twice && (b)->twice + SNAP_VOLATILE > g_serial)

/* A caster's vertices or indices from a buffer the game rewrites between draws (REWRITTEN), as they are now: n bytes at
 * `off` in it, which the game's own memory has at cpu (from XI on Anything, rubymatrix/xi-on-anything,
 * runtime/portable/gfx_d3d12.c: caster_snapshot). The game sways each tree of a kind through one buffer, uploading it
 * again before the next tree is drawn, and the sun's maps are drawn once the scene is done: drawn from the buffer, every
 * tree but the last would have the last one's vertices, so the upload let the earlier ones go (sun_forget) and only the
 * last tree of a kind cast a shadow. The caster reads the ring instead: this frame's upload of the buffer when it had one
 * (no copy made), else a copy of the bytes (1 MB at most, GFX_CASTER_COPY_FRAME a frame). 0 when neither can be had: it reads
 * the buffer, as before. (A caster drawn from a buffer before its first upload of a frame still reads the buffer, and that
 * upload lets it go: the first frame a buffer is rewritten, it isn't REWRITTEN yet.) */
_Static_assert(GFX_CASTER_COPY_MOST + 16 <= RING_CHUNK / 2, "a caster's copy must come from a ring chunk (ring), which lasts the frame");

/* n bytes at off in b (its whole from off, n 0) in this frame's ring: its upload this frame, when that had them; else 0 */
static D3D12_GPU_VIRTUAL_ADDRESS buf_ring(const GfxBuf* b, uint32_t off, uint32_t n)
{
    if (off < b->size && b->up_serial == g_serial && b->up_gpu && (uint64_t)off + (n ? n : b->size - off) <= b->up_size)
        return b->up_gpu + off;
    return 0;
}

static D3D12_GPU_VIRTUAL_ADDRESS caster_bytes(const GfxBuf* b, uint32_t off, const void* cpu, uint32_t n)
{
    if (off >= b->size)
        return 0;
    const D3D12_GPU_VIRTUAL_ADDRESS up = buf_ring(b, off, n);
    if (up)
        return up;
    if (!cpu || !n || n > GFX_CASTER_COPY_MOST || !gfx_caster_copy_ok(n, g_serial))
        return 0;
    Alloc a = ring((size_t)n + 16, 16); /* the slack: a last element read whole */
    if (!a.cpu)
        return 0;
    memcpy(a.cpu, cpu, n);
    return a.gpu;
}

static ID3D12PipelineState* fx_mip_pso(DXGI_FORMAT fmt);
static void fx_mips(GfxTex* t, ID3D12PipelineState* p);
static uint8_t* g_caster_u; /* the fixed casters' uniforms this frame, for the cache */
static uint32_t g_caster_u_len, g_caster_u_cap;

/* The scene's casters, and their uniforms, are kept a scene longer: the zone's cache (sun_cache_update) doesn't copy the
 * thousands it draws again each frame, only those that went out of view, from the set they were last drawn in (cache_take).
 * casters_clear swaps the two sets; generation numbers tell them apart (Cached.pend_gen). */
typedef struct CasterSet
{
    Caster* c;
    uint32_t n, cap;
    uint8_t* u;
    uint32_t ucap;
    uint32_t* pend; /* the cache's entries drawn as one of these, maybe waiting on it (Cached.pend_gen) */
    uint32_t npend, pend_cap;
    uint32_t gen;
    int updated;            /* sun_cache_update has matched these to the cache (with this camera:) */
    float clip_world[16], cam[3];
} CasterSet;
static CasterSet g_was;               /* the set before g_casters' */
static CasterSet g_set = { .gen = 1 }; /* g_casters' own: its pend, gen, updated, clip_world and cam (c, n, u: the globals) */
static void cache_pend_flush(CasterSet* s);
static void sun_cache_late(void);
/* how many of the frame's casters the scene's maps saw (sun_cache_update): those after are the world's pieces the game drew
 * after the effects ran (d3d8.c scene_note's late casters), taken into the cache at the frame's end (sun_cache_late) */
static uint32_t g_casters_scene;
static uint64_t g_sought_serial; /* the frame the caster pass last drew in (caster_only; gfx_sun_cache.h sun_pass_only) */

static void casters_clear(void)
{
    g_casters_scene = 0;
    if (!g_ncasters && !g_caster_u_len && !g_set.npend)
        return; /* none since the last (the frame's end after its scene's): the set before stays */
    cache_pend_flush(&g_was); /* reused now: the entries still waiting on it copied first */
    CasterSet t = g_was;
    g_was = g_set;
    g_was.c = g_casters, g_was.n = g_ncasters, g_was.cap = g_casters_cap, g_was.u = g_caster_u, g_was.ucap = g_caster_u_cap;
    g_casters = t.c, g_casters_cap = t.cap, g_caster_u = t.u, g_caster_u_cap = t.ucap;
    g_set.pend = t.pend, g_set.pend_cap = t.pend_cap, g_set.npend = 0;
    g_set.gen = g_was.gen + 1, g_set.updated = 0;
    g_ncasters = 0;
    g_caster_u_len = 0;
}

/* a draw the zone's: from buffers the game keeps, and not a fan (Caster.fixed: the sun's cache may keep it). The rest, drawn
 * from the game's own memory each frame, are the characters' (sun_draw's chars, sun_map_view) */
static int caster_fixed(const GfxDraw* d) { return d->prim != GFX_TRIANGLEFAN && (!d->indices || d->ibuf); }

/* the zone's plants, and VanaCore's grass: alpha tested and placed by a world matrix, or drawn by the game's vertex shaders
 * (a tree swayed by its constants, the grass by the time in them) - what dlss_motion_plants and sun_plants mean by plants */
static int draw_plant(const GfxDraw* d)
{
    return (d->vs.prog != 0 && d->vs_tokens != NULL) || (alpha_tested(&d->fs) && d->anchor[3] != 0.0f);
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
    c->fixed = caster_fixed(d);
    c->plant = (uint8_t)draw_plant(d);
    c->cpu_u = ~0u;
    return c;
}

/* A draw kept as a caster: every one while the sun's shadows are on (sun: their maps, and the characters' own depth,
 * sun_map_view); with them off, the characters' alone, for the ink lines' own width on them and the occlusion's own
 * strength (outline_width_chars, ao_chars:
 * gfx_scene_done draws sun_map_view for it, which takes the casters that aren't fixed; a zone buffer the game rewrites, a
 * swaying tree's, counts as the zone's here, though the sun's maps take it as not fixed). With none kept, the shadows off
 * left that map undrawn, and characters took the world's width (outline_width) whatever theirs was set to. */
static int caster_wanted(const GfxDraw* d)
{
    if (g_fxs.sun > 0.0f)
        return 1;
#ifdef VANACORE
    /* (the occlusion's share on characters and Render visual 19 need the characters' depth only where the stencil can't tell them) */
    if ((g_fxs.outline > 0.0f && g_fxs.outline_width_chars > 0.0f) ||
        (((g_fxs.ao > 0.0f && g_fxs.ao_chars != 1.0f) || (int)g_fxs.debug == 19) && !cmask_usable(g_ds)))
        return !caster_fixed(d);
#endif
    return 0;
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
    /* onto a large target only: not the sun flare's 16x16 occlusion probe, nor the game's 256x256 targets, which in a crowd it
     * draws its world into some 40 times a frame, the world drawn to between each - a chain made for each, the GPU at 100%
     * (from XI on Anything, rubymatrix/xi-on-anything, gfx_d3d12.c; those keep the game's one bilinear sample, as in D3D8).
     * By the game's size too (small_rt): [graphics] glow draws its 256x256 glow targets larger */
    if (tw < 512 || th < 512 || d->small_rt)
        return;
    for (int i = 0; i < 8; ++i)
    {
        GfxTex* t = d->tex[i];
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        if (!wanted || !t || t == g_rt || t->srv_mips < 0 || t->mips < 2 || t->scene)
            continue;
        if (t->w <= tw && t->h <= th)
            continue; /* not made smaller */
        ID3D12PipelineState* mp = fx_mip_pso(t->dxfmt);
        if (!mp)
            continue;
        const int part_was = gpu_part_cur();
        gpu_part(GP_MIPS);
        fx_mips(t, mp);
        gpu_part(part_was);
        t->scene = g_serial;
    }
}

static void draw_encode(const GfxDraw* d);
static void caster_only(const GfxDraw* d);
#ifdef VANACORE
static int nout_wanted(const GfxDraw* d);
static float g_vn_f[4];  /* the smooth normals' sun_f (vn_replay): the projection's 1/P00, 1/P11 and P23, from the scene before */
static int nout_begin(void);
static GfxTex* dlss_sampled(GfxTex* t);
static int dlss_on_draw(const GfxDraw* d);
static void dlss_draw_over(const GfxDraw* d);
static void el_on_draw(const GfxDraw* d);
static void zl_on_draw(const GfxDraw* d);
static void el_emit(const GfxDraw* d);
static void mo_draw(const GfxDraw* d);
static void mask_draw(const GfxDraw* d);
static int g_fg_hudless_next; /* the draw just made laid the scene onto the frame (frame generation's hudless) */
static void fg_note_hudless(void);
static int g_fgn_named_next; /* ... from the output with the names over it: the frame copied after it (fgn_note_named) */
static void fgn_note_named(void);
static void fg_hudless_clean(const GfxDraw* d);
static int8_t dlss_lod_bias(void);
static uint32_t g_nheld;
static size_t g_held_len;
#endif

static void draw_one(const GfxDraw* d);
#ifdef VANACORE
static int batch_take(const GfxDraw* d); /* (gfx_d3d12_batch.h) */
static void batch_casters(GfxDraw* d, uint32_t first, size_t need);
#endif

void gfx_draw(const GfxDraw* d)
{
    if (!g_dev || !d->count)
        return;
#ifdef VANACORE
    if (batch_take(d)) /* kept while the game draws its zone, to go with the rest of its mesh's pieces */
        return;
#endif
    draw_one(d);
}

/* A draw as it comes (gfx_draw), or one of the zone's batch (gfx_d3d12_batch.h: one for ninst pieces) */
static void draw_one(const GfxDraw* d)
{
    if (d->sought) /* the caster pass's: into the sun's maps alone */
    {
        caster_only(d);
        return;
    }
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
#ifdef VANACORE
    zl_on_draw(d); /* the zone lines, over the scene once its world is drawn */
    el_on_draw(d); /* the light from the effects, before DLSS takes the scene */
    const int dlss = dlss_on_draw(d);
    if (dlss)
    {
        if (dlss == 1)
            dlss_draw_over(d); /* else held, drawn over it later */
        if (gfx_profiling)
            g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
        return;
    }
#endif
    scene_mips(d);
#ifdef VANACORE
    /* the draw laying the scene onto the frame, with frame generation on: made first without the interface held for DLSS
     * over it (the names over heads), for frame generation's hudless */
    if (g_fg_hudless_next)
        fg_hudless_clean(d);
    g_enc.d = NULL, g_enc_take = 1; /* what it binds, for its redraws (mo_keep) */
#endif
    draw_encode(d);
#ifdef VANACORE
    g_enc_take = 0;
    /* (a batch's pieces stand still, opaque or a leaf's blended edge, without normal maps: none of these is for them,
     * gfx_d3d12_batch.h batch_slot) */
    if (!d->ninst)
    {
        el_emit(d);   /* a light from the effects: drawn again into its own target */
        mo_draw(d);   /* a moving thing: drawn again into the motion target, with where it was the frame before (dlss_motion) */
        mask_draw(d); /* a moving thing: drawn again into the upscaler's mask of what isn't the static world */
    }
    /* a normal-mapped scene draw is drawn again into the detail target (its bent normal and z), for the effects that read normals */
    if (d->fs.nmap && d->caster && !g_nout && !d->ninst && nout_wanted(d) && nout_begin())
    {
        g_nout = 1, g_targets_bound = 0;
        draw_encode(d);
        g_nout = 0, g_targets_bound = 0;
    }
#endif
#ifdef VANACORE
    if (g_fgn_named_next)
        fgn_note_named();
    if (g_fg_hudless_next)
        fg_note_hudless();
#endif
    /* a long scene goes to the GPU in pieces */
    if (g_cmd_draws >= SPLIT_DRAWS)
        submit(0);
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
}

/* a draw's texture that doesn't read as itself, in the log (the first ones): the minimap drawn white at times was
 * suspected of it */
static void bind_note(const GfxTex* t, int stage, const char* what)
{
    static uint32_t said;
    if (said >= 40)
        return;
    said++;
    fprintf(stderr, "[recomp] gfx: frame %llu: a draw's stage %d texture %ux%u format %u (view %d, slot's texture %ux%u): %s%s\n",
        (unsigned long long)g_serial, stage, t->w, t->h, t->fmt, t->srv,
        t->srv >= 0 && g_srv_owner && g_srv_owner[t->srv] ? g_srv_owner[t->srv]->w : 0,
        t->srv >= 0 && g_srv_owner && g_srv_owner[t->srv] ? g_srv_owner[t->srv]->h : 0, what,
        said == 40 ? " (the rest aren't logged)" : "");
}

/* the game's draw's mark and note (DRED on): n vertices or indices */
static void game_note(ID3D12GraphicsCommandList* l, const GfxDraw* d, uint32_t n, int indexed)
{
    DrawNote* dn = dred_draw(l, g_nout ? "the game's (again, into the normal-map detail target)"
                                : g_mout ? "the game's (again, into the motion target)" : "the game's", n);
    if (!dn)
        return;
    dn->prim = (uint8_t)d->prim, dn->start = indexed ? 0 : d->vertex_start;
    dn->flags = (uint8_t)((indexed ? DN_INDEXED : 0) | (d->vs.rhw ? DN_RHW : 0) | (d->vs.prog ? DN_VS : 0) |
        (d->fs.prog ? DN_PS : 0) | (g_nout ? DN_DETAIL : 0));
    dn->vbytes = d->buf[0] ? d->buf[0]->size - d->buf_off[0] : d->size[0];
    if (d->tex[0])
        dn->tw = d->tex[0]->w, dn->th = d->tex[0]->h, dn->tfmt = d->tex[0]->fmt;
    if (g_rt)
        dn->rw = g_rt->w, dn->rh = g_rt->h, dn->rfmt = g_rt->fmt;
}

/* the draw itself: n vertices or indices */
static void draw_call(ID3D12GraphicsCommandList* l, const GfxDraw* d, uint32_t n, int indexed)
{
    game_note(l, d, n, indexed);
    const UINT pieces = d->ninst ? d->ninst : 1; /* a batch's: each of its pieces (gfx_d3d12_batch.h) */
    if (indexed)
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, n, pieces, 0, 0, 0);
    else
        ID3D12GraphicsCommandList_DrawInstanced(l, n, pieces, d->vertex_start, 0);
}

/* the zone's casters are kept (sun_cache) with their uniforms (need bytes of them): a copy the CPU can read (the ring's is
 * write-combined) */
static void caster_keep_u(const GfxDraw* d, Caster* rec, size_t need)
{
    if (!rec->fixed || g_fxs.sun_casters == 1.0f)
        return;
    uint8_t* more = g_caster_u;
    if (g_caster_u_len + need > g_caster_u_cap)
    {
        uint32_t cap = (g_caster_u_len + (uint32_t)need) * 2 + 65536;
        more = (uint8_t*)realloc(g_caster_u, cap);
        if (!more) /* as much as it needs, and a little more */
            more = (uint8_t*)realloc(g_caster_u, cap = g_caster_u_len + (uint32_t)need + 65536);
        if (more)
            g_caster_u = more, g_caster_u_cap = cap;
    }
    if (more)
    {
        sun_u_copy(g_caster_u + g_caster_u_len, &d->u, rec);
        rec->cpu_u = g_caster_u_len, g_caster_u_len += (uint32_t)need;
    }
    else /* no room to keep its uniforms (a 32-bit game's address space ran out): not the zone's cache's, but drawn each frame */
        rec->fixed = 0;
}

static void draw_encode(const GfxDraw* d)
{
    if (!bind_targets())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GfxTex* ds = depth_attachment();
    ID3D12PipelineState* p = pipeline(d, ds);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE); /* still building (or failed) */
        return;
    }
    ID3D12GraphicsCommandList* l = g_list;
#ifdef VANACORE
    if (g_enc_take)
        memset(g_enc.tex, 0, sizeof g_enc.tex), g_enc.ibv.BufferLocation = 0;
#endif
    /* textures first: their transitions go before the draw */
    uint32_t bind[16];
    for (int i = 0; i < 8; ++i)
    {
        GfxTex* t = d->tex[i];
#ifdef VANACORE
        t = dlss_sampled(t);
#endif
        int wanted = d->fs.prog || i < d->fs.nstages || (i == 7 && (d->fs.nmap || d->fs.rain == 2)) ? d->fs.st[i].tex : 0;
        bind[i] = wanted == 2 ? SRV_NULL_CUBE : SRV_NULL_2D;
        bind[8 + i] = g_present_samp;
        if (!wanted)
            continue;
        GfxSampler sk = d->samp[i];
        int32_t view = t ? t->srv : -1;
        if (t && g_fxs.fx != 0.0f)
        {
            /* the world's solid textures (not the interface's, not the sky's or effects' - which write
             * no depth: the sky's clouds, mapped at steep angles near the horizon, flicker through an
             * anisotropic filter as the camera turns), where every mip is there: trilinear and
             * anisotropic, so ground and walls at a slant stay sharp and do not swim */
            if (!d->vs.rhw && d->depth.zwrite && !d->fs.st[i].projected && g_fxs.aniso > 1.0f && t->levels > 1 &&
                t->levels < 32 && t->filled == (1u << t->levels) - 1 && sk.min >= 2)
                sk.min = 3, sk.mip = 2, sk.max_aniso = (uint8_t)(g_fxs.aniso > 16.0f ? 16.0f : g_fxs.aniso);
            /* the finished scene made smaller (FFXI's background onto the back buffer): through its
             * mips (scene_mips), anisotropic for a squeeze that differs across and down */
            else if (t->scene && t->srv_mips >= 0)
                sk.min = 3, sk.mag = 2, sk.mip = 2, sk.max_aniso = 16, sk.max_level = 0, view = t->srv_mips;
        }
#ifdef VANACORE
        /* the world drawn smaller for DLSS: its textures' mips chosen as at the size it's shown at (NVIDIA's
         * log2 of the render size over the shown one), else they're a level or two blurrier than DLSS makes the rest */
        if (t && !d->vs.rhw && sk.mip >= 1 && !t->scene)
            sk.lod_bias = dlss_lod_bias();
#endif
        /* each stage mostly samples the same way draw after draw: its last key before the lookup */
        static GfxSampler stage_key[8];
        static uint32_t stage_slot[8], stage_known, stage_gen;
        if (stage_gen != g_device_gen) /* the device torn down: the sampler heap with it */
            stage_known = 0, stage_gen = g_device_gen;
        if ((stage_known >> i & 1) && !memcmp(&sk, &stage_key[i], sizeof sk))
            bind[8 + i] = stage_slot[i];
        else
        {
            bind[8 + i] = stage_slot[i] = sampler(&sk);
            stage_key[i] = sk, stage_known |= 1u << i;
        }
        if (t && view < 0)
            bind_note(t, i, "no view (out of descriptors), so it reads as the null view");
        if (!t || view < 0 || t == g_rt || (wanted == 2) != (t->type == GFX_TEX_CUBE))
            continue; /* unbound, or the target being drawn to: reads as the null view */
        if (view == t->srv && g_srv_owner && g_srv_owner[view] != t)
            bind_note(t, i, "its view slot is another texture's (or one let go), so it reads that one");
        tex_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        bind[i] = (uint32_t)view;
#ifdef VANACORE
        if (g_enc_take)
            g_enc.tex[i] = t;
#endif
    }
    if (g_targets_bound == 0 || g_rt->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
        bind_targets();
    set_pso(p);
    set_topology(topology(d->prim));
    set_viewport(d->vp);
    {
        uint32_t sref = d->stencil_ref;
        int suse = ds && ds->has_stencil && d->depth.stencil;
#ifdef VANACORE
        if (!d->depth.stencil && d->is_char && cmask_usable(ds)) /* its pipeline writes the characters' bit (pipeline) */
            suse = 1, sref = CHAR_STENCIL;
#endif
        if (suse && sref != g_drawn.stencil_ref)
            ID3D12GraphicsCommandList_OMSetStencilRef(l, sref), g_drawn.stencil_ref = sref;
    }
    if (memcmp(bind, g_drawn.bind, sizeof bind))
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0), memcpy(g_drawn.bind, bind, sizeof bind);

    /* the uniforms the draw's functions read: the lights only when lit, the vertex shader's
     * constants only for a vertex shader, the pixel shader's only for a pixel shader (the ring
     * keeps room for the whole struct where a chunk ends: that is what the functions are compiled
     * against, ring_part) */
    size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        need = offsetof(GfxU, psc);
    if (d->fs.prog)
        need = sizeof(GfxU);
    Alloc u = ring_part(need, sizeof(GfxU), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!u.cpu)
        return;
    memcpy(u.cpu, &d->u, need);
    if (d->fs.nmap) /* the normal maps' strength, in the fog density's neighbour */
        ((GfxU*)u.cpu)->params2[1] = g_fxs.normalmap_strength;
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, u.gpu);
#ifdef VANACORE
    if (g_enc_take)
        g_enc.ub = u.gpu;
#endif
    /* the scene's casters: the depth they are drawn with is the scene's (the effects read it), and
     * each is kept for the sun's shadow map */
    if (d->caster && !g_rt_face && !g_rt_level && ds)
        g_rt->depth_world = ds;
    Caster* rec = d->caster && !g_nout && !g_mout && g_fxs.fx != 0.0f && caster_wanted(d) ? caster_new(d) : NULL;
    if (g_mout) /* the motion pass's transform the frame before (gfx_hlsl.c: its VS reads it as the sun's maps read theirs), and
                 * the least alpha of its pixels that count (sun_f.x) */
    {
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 16, g_mo_m, 0);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 4, g_mo_f, 16);
    }
    /* a batch's pieces' transforms (gfx_d3d12_batch.h): in a stream the draw doesn't read; each piece's caster is drawn alone */
    const int inst_slot = d->ninst ? d->vs.inst - 1 : -1;
    if (rec)
        rec->lib.vs.inst = 0;
    if (rec)
    {
        rec->dead = 1; /* until it is drawn */
        rec->ub = u.gpu, rec->need = (uint32_t)need, rec->topo = (uint8_t)topology(d->prim);
        rec->n = vertex_count(d->prim, d->count), rec->vstart = d->vertex_start;
        if (alpha_tested(&d->fs))
        {
            memcpy(rec->bind, bind, sizeof bind);
            for (int i = 0; i < 8; ++i)
                if (bind[i] != SRV_NULL_2D && bind[i] != SRV_NULL_CUBE)
                    rec->tex[i] = d->tex[i], d->tex[i]->sun_seen = g_serial;
        }
    }
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(g_dummy), own = 0;
        if (d->buf[s])
        {
            buf_state(d->buf[s], BUF_READ);
            va = d->buf[s]->gpu + d->buf_off[s];
            if (rec)
            {
                if (VOLATILE(d->buf[s]))
                    rec->fixed = 0;
                /* one the game rewrites between draws: the caster reads its vertices as they are now, in the ring
                 * (caster_bytes), and the buffer's next upload doesn't end it */
                if (REWRITTEN(d->buf[s]))
                    own = caster_bytes(d->buf[s], d->buf_off[s], d->cpu[s], d->cpu_size[s]);
                if (own)
                    rec->own = 1, rec->fixed = 0; /* the ring's this frame: never the cache's (it keeps addresses) */
                else
                    rec->buf[s] = d->buf[s], d->buf[s]->sun_seen = g_serial;
            }
        }
        else if (d->data[s] && d->size[s])
        {
            Alloc v = ring(d->size[s] + 16, 16); /* the slack: a last element read whole */
            if (!v.cpu)
                return;
            memcpy(v.cpu, d->data[s], d->size[s]);
            va = v.gpu;
            if (rec)
                rec->fixed = 0;
        }
        if (g_mout && s == g_mo_slot && g_mo_va)
            va = g_mo_va; /* the motion pass's: the frame before's positions (mo_draw), in a stream the draw doesn't read */
        if (rec)
            rec->vb[s] = own ? own : va;
        if (s == inst_slot)
        {
            Alloc v = ring((size_t)d->ninst * GFX_INST_BYTES, 16);
            if (!v.cpu)
                return;
            memcpy(v.cpu, d->inst, (size_t)d->ninst * GFX_INST_BYTES);
            va = v.gpu; /* (the casters' stays the dummy: theirs is the draw alone) */
        }
        if (va != g_drawn.stream[s])
            ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + s, va), g_drawn.stream[s] = va;
#ifdef VANACORE
        if (g_enc_take)
            g_enc.vb[s] = va;
#endif
    }
    if (rec)
        caster_keep_u(d, rec, need);

    uint32_t n = vertex_count(d->prim, d->count);
    D3D12_INDEX_BUFFER_VIEW ibv = { 0 };
    if (d->prim == GFX_TRIANGLEFAN)
    {
        /* no fans in D3D12: a list with the same vertices */
        Alloc a = ring((size_t)n * 4, 16);
        if (!a.cpu)
            return;
        uint32_t* idx = (uint32_t*)a.cpu;
        for (uint32_t i = 0; i < d->count; ++i)
        {
            uint32_t k[3] = { 0, i + 1, i + 2 };
            for (int j = 0; j < 3; ++j)
            {
                if (!d->indices)
                    idx[3 * i + j] = d->vertex_start + k[j];
                else if (d->index_size == 2)
                    idx[3 * i + j] = ((const uint16_t*)d->indices)[k[j]];
                else
                    idx[3 * i + j] = ((const uint32_t*)d->indices)[k[j]];
            }
        }
        ibv.BufferLocation = a.gpu, ibv.SizeInBytes = n * 4, ibv.Format = DXGI_FORMAT_R32_UINT;
        set_index_buffer(&ibv);
        draw_call(l, d, n, 1);
        if (rec)
            rec->ibv = ibv;
    }
    else if (d->ibuf)
    {
        buf_state(d->ibuf, BUF_READ);
        ibv.BufferLocation = d->ibuf->gpu + d->ibuf_off;
        ibv.SizeInBytes = d->ibuf->size - d->ibuf_off;
        ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        set_index_buffer(&ibv);
        draw_call(l, d, n, 1);
        if (rec)
        {
            rec->ibv = ibv;
            D3D12_GPU_VIRTUAL_ADDRESS own = 0; /* as the vertices: indices the game rewrites between draws, as they are now */
            if (VOLATILE(d->ibuf))
                rec->fixed = 0;
            if (REWRITTEN(d->ibuf))
                own = caster_bytes(d->ibuf, d->ibuf_off, d->indices, n * d->index_size);
            if (own)
                rec->ibv.BufferLocation = own, rec->ibv.SizeInBytes = n * d->index_size, rec->own = 1, rec->fixed = 0;
            else
                rec->buf[GFX_NSTREAMS] = d->ibuf, d->ibuf->sun_seen = g_serial;
        }
    }
    else if (d->indices)
    {
        Alloc a = ring((size_t)n * d->index_size, 16);
        if (!a.cpu)
            return;
        memcpy(a.cpu, d->indices, (size_t)n * d->index_size);
        ibv.BufferLocation = a.gpu, ibv.SizeInBytes = n * d->index_size;
        ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        set_index_buffer(&ibv);
        draw_call(l, d, n, 1);
        if (rec)
            rec->ibv = ibv;
    }
    else
    {
        draw_call(l, d, n, 0);
    }
    if (rec)
    {
        rec->dead = 0;
        rec->has_clip0 = rec->fixed && (uint8_t)gfx_draw_clip0(d, rec->clip0);
        rec->snapped = (uint8_t)snap_note(d, rec); /* (the game's own sway of a tree the pass has: its copy in its place) */
#ifdef VANACORE
        if (d->ninst > 1) /* a batch's: each of its other pieces a caster of its own, as if drawn alone */
            batch_casters((GfxDraw*)d, (uint32_t)(rec - g_casters), need);
#endif
    }
#ifdef VANACORE
    if (g_enc_take)
        g_enc.ibv = ibv, memcpy(g_enc.bind, bind, sizeof bind), g_enc.d = d;
#endif
    g_cmd_draws++;
}

#ifdef VANACORE
#include "gfx_d3d12_batch.h" /* gfx_batch: the zone's draws of one mesh sent together */
#else
void gfx_batch(int open) { (void)open; }
#endif

/* A piece of the zone the camera's view left out, drawn by the game for the sun's maps alone (GfxDraw.sought: VanaCore's caster
 * pass, casters.cpp; d3d8.c seek_note): kept as a caster as draw_encode keeps the scene's - its uniforms, streams and indices, an
 * alpha test's textures and samplers - and drawn into nothing now. The cache takes it as it does the scene's (cache_see), so it
 * casts in the frames between the pass's visits too. */
/* ... an alpha test's textures and samplers, as draw_encode binds them for the scene's casters: a stage without one, or with one
 * that isn't the draw's kind, gets a null view */
static void caster_only_textures(const GfxDraw* d, Caster* rec)
{
    for (int i = 0; i < 8; ++i)
    {
        const int wanted = d->fs.prog || i < d->fs.nstages || (i == 7 && d->fs.nmap) ? d->fs.st[i].tex : 0;
        rec->bind[i] = wanted == 2 ? SRV_NULL_CUBE : SRV_NULL_2D;
        rec->bind[8 + i] = g_present_samp;
        if (!wanted)
            continue;
        GfxSampler sk = d->samp[i];
        rec->bind[8 + i] = sampler(&sk);
        GfxTex* t = d->tex[i];
        if (!t || t->srv < 0 || t == g_rt || (wanted == 2) != (t->type == GFX_TEX_CUBE))
            continue;
        rec->bind[i] = (uint32_t)t->srv, rec->tex[i] = t, t->sun_seen = g_serial;
    }
}

/* ... its vertex streams: the game's buffers as they are (one it rewrites copied, caster_bytes), or the draw's own memory copied
 * into the ring; 0 when the ring is full */
static int caster_only_streams(const GfxDraw* d, Caster* rec)
{
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(g_dummy), own = 0;
        if (d->buf[s])
        {
            buf_state(d->buf[s], BUF_READ);
            va = d->buf[s]->gpu + d->buf_off[s];
            if (VOLATILE(d->buf[s]))
                rec->fixed = 0;
            if (REWRITTEN(d->buf[s]))
                own = caster_bytes(d->buf[s], d->buf_off[s], d->cpu[s], d->cpu_size[s]);
            if (own)
                rec->own = 1, rec->fixed = 0;
            else
                rec->buf[s] = d->buf[s], d->buf[s]->sun_seen = g_serial;
        }
        else if (d->data[s] && d->size[s])
        {
            Alloc v = ring(d->size[s] + 16, 16);
            if (!v.cpu)
                return 0;
            memcpy(v.cpu, d->data[s], d->size[s]);
            va = v.gpu, rec->fixed = 0;
        }
        rec->vb[s] = own ? own : va;
    }
    return 1;
}

/* ... and its indices the same way (n its vertices); 0 when the ring is full */
static int caster_only_indices(const GfxDraw* d, Caster* rec, uint32_t n)
{
    if (d->ibuf)
    {
        buf_state(d->ibuf, BUF_READ);
        rec->ibv.BufferLocation = d->ibuf->gpu + d->ibuf_off, rec->ibv.SizeInBytes = d->ibuf->size - d->ibuf_off;
        rec->ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        D3D12_GPU_VIRTUAL_ADDRESS own = 0;
        if (VOLATILE(d->ibuf))
            rec->fixed = 0;
        if (REWRITTEN(d->ibuf))
            own = caster_bytes(d->ibuf, d->ibuf_off, d->indices, n * d->index_size);
        if (own)
            rec->ibv.BufferLocation = own, rec->ibv.SizeInBytes = n * d->index_size, rec->own = 1, rec->fixed = 0;
        else
            rec->buf[GFX_NSTREAMS] = d->ibuf, d->ibuf->sun_seen = g_serial;
    }
    else if (d->indices)
    {
        Alloc a = ring((size_t)n * d->index_size, 16);
        if (!a.cpu)
            return 0;
        memcpy(a.cpu, d->indices, (size_t)n * d->index_size);
        rec->ibv.BufferLocation = a.gpu, rec->ibv.SizeInBytes = n * d->index_size;
        rec->ibv.Format = d->index_size == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    }
    return 1;
}

/* A draw of the caster pass's or of the zone's own meshes (zonemesh.c), into the sun's maps alone. Not with sun_seek 0: only what
 * the game drew for the camera casts then. The zone's meshes came on all the same, an eighth of them a frame (ZM_ROUND), and
 * sun_pass_only (sun_seek's) is what keeps each part drawn every frame: without it each eighth was in the maps one frame in 8 and
 * the shadows flickered, under the trees and on them (Wajaom Woodlands, a player's report, 2026-11-22). VanaCore's temporal pass
 * evened it out; DLSS and FSR, with that pass off, showed it */
static void caster_only(const GfxDraw* d)
{
    if (g_fxs.fx == 0.0f || g_fxs.sun <= 0.0f || g_fxs.sun_seek == 0.0f || d->prim == GFX_TRIANGLEFAN)
        return;
    Caster* rec = caster_new(d);
    if (!rec)
        return;
    rec->dead = 1, rec->sought = 1, rec->world = d->sought == 2; /* (dead until all of it is had) */
    rec->piece = d->piece, rec->piece_draw = d->piece_draw;
    g_sought_serial = g_serial;     /* the pass drew this frame (sun_pass_only) */
    size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        need = offsetof(GfxU, psc);
    if (d->fs.prog)
        need = sizeof(GfxU);
    Alloc u = ring_part(need, sizeof(GfxU), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!u.cpu)
        return;
    memcpy(u.cpu, &d->u, need);
    rec->ub = u.gpu, rec->need = (uint32_t)need, rec->topo = (uint8_t)topology(d->prim);
    rec->n = vertex_count(d->prim, d->count), rec->vstart = d->vertex_start;
    if (alpha_tested(&d->fs))
        caster_only_textures(d, rec);
    if (!caster_only_streams(d, rec))
        return;
    caster_keep_u(d, rec, need);
    if (!caster_only_indices(d, rec, rec->n))
        return;
    rec->dead = 0;
    rec->has_clip0 = rec->fixed && (uint8_t)gfx_draw_clip0(d, rec->clip0);
    rec->snapped = (uint8_t)snap_note(d, rec);
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!g_dev || !g_rt)
        return;
    BATCH_SETTLE();
    if (!g_ds)
        flags &= 1;
#ifdef VANACORE
    /* the characters' stencil bit starts each frame clear, with the depth (a game that clears the depth alone leaves the stencil) */
    if ((flags & 2) && !(flags & 4) && cmask_usable(g_ds))
        flags |= 4, stencil = 0;
    if ((flags & 6) && g_mob_n) /* the redraws waiting test the depth as it is now */
        mo_flush(1);
#endif
    if (!flags || !bind_targets())
        return;
    float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f };
    uint32_t w, h;
    color_size(&w, &h);
    int whole = !nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h;
    /* the viewport, intersected with each rectangle */
    D3D12_RECT stack[16], *r = stack;
    uint32_t n = 0;
    if (!whole)
    {
        int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
        int32_t whole_rect[4] = { vx0, vy0, vx1, vy1 };
        if (!nrects)
            rects = whole_rect, nrects = 1;
        if (nrects > 16)
            r = (D3D12_RECT*)malloc(sizeof(D3D12_RECT) * nrects);
        for (uint32_t i = 0; i < nrects; ++i)
        {
            int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
            int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
            x1 = x1 < (int32_t)w ? x1 : (int32_t)w, y1 = y1 < (int32_t)h ? y1 : (int32_t)h;
            x0 = x0 > 0 ? x0 : 0, y0 = y0 > 0 ? y0 : 0;
            if (x1 > x0 && y1 > y0)
                r[n++] = (D3D12_RECT){ x0, y0, x1, y1 };
        }
        if (!n)
            goto out;
    }
    if (flags & 1)
        ID3D12GraphicsCommandList_ClearRenderTargetView(g_list, target_view(g_rt, g_rt_face, g_rt_level), c, n, whole ? NULL : r);
    GfxTex* ds = depth_attachment();
    if (ds && (flags & 6))
    {
        D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)(((flags & 2) ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((flags & 4) && ds->has_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
        if (f)
            ID3D12GraphicsCommandList_ClearDepthStencilView(g_list, target_view(ds, 0, 0), f, z, (UINT8)stencil, n, whole ? NULL : r);
    }
out:
    if (r != stack)
        free(r);
}

/* --- the window ------------------------------------------------------------------------------------------------ */
static void swap_release(void)
{
    for (int i = 0; i < FRAMES; ++i)
    {
        if (g_swap_buf[i])
            ID3D12Resource_Release(g_swap_buf[i]);
        g_swap_buf[i] = NULL;
    }
}

static void swap_acquire(void)
{
    for (UINT i = 0; i < FRAMES; ++i)
    {
        IDXGISwapChain3_GetBuffer(g_swap, i, &IID_ID3D12Resource, (void**)&g_swap_buf[i]);
        g_swap_state[i] = D3D12_RESOURCE_STATE_PRESENT;
        if (g_swap_rtv[i] < 0)
            g_swap_rtv[i] = heap_alloc(&g_rtv);
        ID3D12Device_CreateRenderTargetView(g_dev, g_swap_buf[i], NULL, heap_cpu(&g_rtv, g_swap_rtv[i]));
    }
}

#ifdef VANACORE
static void fg_wait_idle(void);
#endif
static void pqueue_idle(void);

/* the swap chain at the window's size in pixels (all GPU work finished first, the present queue's too: its buffers go) */
static void swap_fit(void)
{
    if (g_lost_said) /* nothing will show again: not tried frame after frame */
        return;
    int pw = 0, ph = 0;
#ifdef VANACORE
    RECT rc;
    if (g_window && GetClientRect(g_window, &rc))
        pw = rc.right, ph = rc.bottom;
#else
    if (g_window)
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
#endif
    if (pw <= 0 || ph <= 0)
        pw = (int)g_want_w, ph = (int)g_want_h;
    if (pw <= 0 || ph <= 0 || ((uint32_t)pw == g_swap_w && (uint32_t)ph == g_swap_h))
        return;
#ifdef VANACORE
    fg_wait_idle();
#endif
    submit(1);
    pqueue_idle();
    swap_release();
    HRESULT hr = IDXGISwapChain3_ResizeBuffers(g_swap, FRAMES, (UINT)pw, (UINT)ph, DXGI_FORMAT_B8G8R8A8_UNORM,
        g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr))
    {
        if (!failed_lost(hr, "resizing the swap chain"))
            fprintf(stderr, "[recomp] gfx: swap chain resize to %dx%d failed (%08lx)\n", pw, ph, (unsigned long)hr);
    }
    else
        g_swap_w = (uint32_t)pw, g_swap_h = (uint32_t)ph;
    swap_acquire();
}

/* --- frames ---------------------------------------------------------------------------------------------------- */
static void fx_rebuild_take(void);

static void frame_end(void)
{
#ifdef VANACORE
    BATCH_SETTLE(); /* the zone's draws kept are this frame's too */
    mo_flush(1); /* the moving things' redraws are this frame's (its ring, its targets) */
#endif
    sun_cache_late(); /* the world's pieces drawn after the effects: into the cache, casting from the next frame on */
    casters_clear();
    list();
    if (gfx_profiling && g_queries && g_frames[g_frame].timed)
    {
        for (int i = 1; i < QN - 1; ++i) /* a frame without the scene effects: theirs taken here, of no length */
            prof_mark(i);
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, QN * g_frame + QN - 1);
        ID3D12GraphicsCommandList_ResolveQueryData(g_list, g_queries, D3D12_QUERY_TYPE_TIMESTAMP, QN * g_frame, QN, g_query_rb,
            8ull * QN * g_frame);
        Frame* sf = &g_frames[g_frame];
        if (g_sq && sf->scope_n > 0)
        {
            ID3D12GraphicsCommandList_EndQuery(g_list, g_sq, D3D12_QUERY_TYPE_TIMESTAMP, GPU_QS * g_frame + sf->scope_n);
            ID3D12GraphicsCommandList_ResolveQueryData(g_list, g_sq, D3D12_QUERY_TYPE_TIMESTAMP, GPU_QS * g_frame,
                (UINT)sf->scope_n + 1, g_sq_rb, 8ull * GPU_QS * g_frame);
        }
        sf->closed = 1;
    }
    submit(0);
    g_frame_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    watch_skips();
    g_serial++;
    fx_rebuild_take(); /* between frames: nothing of a frame's is half made */
}

/* "60 FPS 16.7ms": the text of the overlay, from the presents of the last half second */
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
    Alloc a = ring(sizeof u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    memcpy(a.cpu, &u, sizeof u);
    set_pso(g_overlay_pso);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(g_list, ROOT_U, a.gpu);
    dred_draw(g_list, "the frame rate", 4);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 4, 1, 0, 0);
}

/* The frame (heap slot src, in the pixel shaders' state) onto a target of the window's size (the swap chain's
 * buffer, or one of frame generation's frames): sharpened with the scene effects' sharpen, VanaCore's interface
 * layer and the frame-rate overlay over it */
static void present_draw(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h, int32_t src)
{
    ID3D12GraphicsCommandList* l = list();
    const int part_was = gpu_part_cur();
    gpu_part(GP_PRESENT);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT v = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    /* contrast-adaptive sharpening with the scene effects (sharpen, 0..1; the colour grade's: off with it) */
    float sharpen = g_fxs.fx != 0.0f && g_fxs.grade > 0.0f ? g_fxs.sharpen : 0.0f;
    uint32_t bind[16] = { 0 };
    bind[0] = (uint32_t)src, bind[8] = g_present_samp;
    memcpy(&bind[1], &sharpen, 4);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
    set_pso(sharpen > 0.0f && g_present_cas_pso ? g_present_cas_pso : g_present_pso);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DrawNote* dn = dred_draw(l, "the frame onto the window", 3);
    if (dn)
        dn->rw = w, dn->rh = h;
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
#ifdef VANACORE
    /* VanaCore's interface, drawn at the window's size, over the frame at whatever size it was drawn */
    GfxTex* layer = g_present_layer;
    if (layer && layer->srv >= 0 && g_present_layer_pso)
    {
        tex_state(layer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        gpu_part(GP_LAYER);
        bind[0] = (uint32_t)layer->srv;
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
        set_pso(g_present_layer_pso);
        dred_draw(l, "VanaCore's interface layer onto the window", 3);
        ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
    }
#endif
    if (g_overlay && g_overlay_pso)
    {
        gpu_part(GP_OVERLAY);
        draw_overlay(w, h);
    }
    gpu_part(part_was);
    g_targets_bound = 0;
    drawn_forget();
}

/* --- scene effects (gfx_scene_done) -----------------------------------------------------------------------------
 * gfx_metal.m's scene effects on Direct3D 12, the same passes and shaders (MSL -> HLSL). On the
 * finished 3D scene, in place, when fx is on:
 *   - ambient occlusion: view-space positions rebuilt from the scene's depth and projection, a spiral
 *     of samples around each pixel (Scalable Ambient Obscurance, simplified), then a depth-aware blur
 *     across and down;
 *   - sun shadows, in the same pass and blur: each pixel looked up in the sun's shadow maps (the scene's
 *     casters drawn again from the sun, sun_map: a near cascade and a far one, soft by the casters'
 *     distance), and a short ray toward the sun through the depth buffer for contact shadows where
 *     things meet the ground; by day; then over time (the frame before's, where it agrees);
 *   - height fog: the density falls off with world height (up from the view matrix), integrated along
 *     each view ray; lit by the sun when looking toward it (Henyey-Greenstein scattering);
 *   - bloom: the bright parts, at a quarter and an eighth of the scene, blurred and added back;
 *   - god rays: the sky's bright pixels around the sun, blurred along lines toward the sun's place
 *     on screen (the sun is the scene's directional light);
 *   - a color grade (saturation, a contrast curve).
 * The draw that makes the scene smaller for the back buffer samples it through mips made for it then
 * (scene_mips). The frame is sharpened at Present (present_cas_fs). light = 1 lights the world's lit
 * draws per pixel rather than per vertex (gfx_hlsl.c).
 *
 * Every pass is one triangle over its target with the draws' root signature: the uniforms (FxU) at
 * b0, and in the 16 root constants the heap slots of its inputs (fti) and the sampler and blur
 * direction (fsi). Depth is read through its SRV (the depth surfaces are typeless underneath).
 * Settings: FFXI_FX=1 and FFXI_FX_<KEY> (the table in fx_config), FFXI_FX_FILE while the game runs,
 * or gfx_fx_set. debug shows one part alone: 1 occlusion, 2 fog, 3 bloom, 4 god rays, 5 sun shadows (map and
 * contact); VanaCore's: 6 the ink lines, 7 the depth of field's blur, 8 the bounce light's layer alone (gi on). */
/* In parts, joined once at runtime (fx_hlsl): MSVC's C compiler can't take one string literal past 65,535 bytes (it runs
 * out of memory at its end, C1060). Keep each part well under that. */
static const char FX_HLSL_1[] =
    "struct FxU {\n"
    "  float4 proj;   // P00, P11, P20, P21\n"
    "  float4 zp;     // P22, P32, viewport MinZ, MaxZ\n"
    "  float4 vp;     // the scene viewport in target pixels: x, y, width, height\n"
    "  float4 size;   // target width, height; occlusion width, height\n"
    "  float4 ao;     // radius, strength, bias, largest radius in pixels\n"
    "  float4 grade;  // strength, saturation, contrast, debug\n"
    "  float4 hand;   // P23: 1 for a left-handed projection, -1 for a right-handed one\n"
    "  float4 up;     // world up in view space: height above the camera = dot(P, up.xyz); w = fog_falloff * fog_floor\n"
    "  float4 sun;    // view space, toward the light; w = 1 when the scene has one\n"
    "  float4 suncol; // its color\n"
    "  float4 sunuv;  // its place in the viewport (0..1), z = how much of it shows (0 behind the camera); w 1 when the detail\n"
    "                 // target holds the scene's smooth normals (vn_replay: smooth_at)\n"
    "  float4 fogc;   // fog color, a = density at the camera's height\n"
    "  float4 fogp;   // falloff with height, most fog, sun glow, its forward scattering (g)\n"
    "  float4 bloom;  // threshold, strength, knee; w the grade's warmth\n"
    "  float4 rays;   // strength, decay, length\n"
    "  float4 toon;   // the cel look: line strength, line width in pixels, silhouette and crease thresholds\n"
    "  float4 toon2;  // lines' fade distance, brightness bands, band softness\n"
    "  float4 toon3;  // smoothing strength, its radius in pixels, 1 the grass left out of the lines (input 8)\n"
    "  float4 dof;    // depth of field: widest blur in pixels, sharp range and far ramp (times the focus distance), near blur\n"
    "  float4 dof2;   // a fixed focus distance (0: the player's), the focus's easing this frame, 1 to take it outright, 1 to leave the sky sharp\n"
    "  float4 shadow; // contact shadows: strength (0: none, or night), ray length, thickness, how far out\n"
    "  float4x4 lmat; // view space to the sun's map: x, y -1..1 (y up), z 0..1\n"
    "  float4 smap;   // the map: strength (0: none), a texel in world units, depth bias, penumbra (map width per depth unit)\n"
    "  float4 smap2;  // depth units per map width, sun_face, sun_min, world units per depth unit\n"
    "  float4x4 reproj; // view space to the frame before's clip space\n"
    "  float4 hist;   // 1 when the frame before is there to blend with, the pattern's turn this frame, its weight\n"
    "  float4x4 lmatn; // the near cascade: view space to its map\n"
    "  float4 smapn;  // its texel in world units, depth bias, penumbra, slope\n"
    "  float4 smapn2; // its depth units, 1 when it is there; sun_soft_chars: 1 on, how far the shadows on characters are blurred (world units)\n"
    "  float4 sunsh;  // view space, toward the light the shadows fall from: the sun, or held over the horizon (sun_dusk)\n"
    "  float4 aoq;    // the occlusion's samples a pixel this frame; 1 for hard shadow edges (sun_soft 0); the depth's step;\n"
    "                 // 1 where the occlusion's pattern is held still, 2 where DLSS or FSR gathers the frames (the sun's too)\n"
    "  float4 glow;   // VanaCore's glow: its tint (rgb), and alpha times how strong\n"
    "  float4 glow2;  // the input's level the pass reads, the blur's step in its texels, 1 to premultiply\n"
    "  float4 zlc;    // VanaCore's zone lines: how many, how far they're drawn to, the line's height, their size\n"
    "  float4 zlcol;  // ... their colour, and how strongly\n"
    "  float4 zlp;    // ... their style (0 fill, 1 line, 2 dots, 3 stars), 1 for a glow, the time in seconds, how far their ends fade\n"
    "  float4 zl[64]; // ... each four: its centre in view space, then its axes (the second the world's up), w half its size along each\n"
    "  float4 grade2; // the grade's tone, -1..1: brightness, shadows, highlights\n"
    "  float4 refl;   // reflections: strength, wetness, roughness, how far (world units)\n"
    "  float4 refl2;  // steps, the sun's glint, bounce light's strength, the pattern's turn\n"
    "  float4 refl3;  // bounce light's radius (world units), its samples, the reflection's blur taps\n"
    "  float4 post;   // the colour grade, laid last (fx_post): strength, saturation, contrast, warmth\n"
    "  float4 post2;  // ... brightness, shadows, highlights; w = 1 where it reads the pixel in place (a copy of the scene), else the input filtered\n"
    "  float4 grade3; // the composite's own grade's zones (Direct3D 11's, which grades there; Direct3D 12's is last: post3)\n"
    "  float4 post3;  // the grade's zones: midtones, whites, light tint, soft highlights\n"
    "  float4 clar;   // clarity: strength\n"
    "  float4 bounce; // bounce light's layer: its width and height (its own size, smaller than the occlusion's); w the reflections' lift (refl_lift)\n"
    "  float4 elight; // VanaCore's light from the effects: strength, reach (world units), threshold, most gain\n"
    "  float4 elight2; // ... its first level's reach in scene pixels; y the reflections' fake bump (refl_bump); z how much of the lanterns' light is left out, w the dimmed effects' (el_run)\n"
    "  float4 elight3; // the characters' effects (spells, abilities): x their own strength (elight_actor), y 1 while any was drawn this frame; z 1 laid onto DLSS's output (fx_elc); w 1 while DLSS 5 has effects to leave out of its lights (fx_dlssg)\n"
    "  float4 aor;    // the occlusion's rings (ao_rings): 1 on, how far apart they are (world units); the shadows' (sun_rings), the same\n"
    "  float4 aoc;    // the occlusion on characters (ao_chars): x its share of ao's strength; y 1 while the characters' depth is input 9;\n"
    "                 // z the zone's shadows blurred this far across its surfaces (sun_soft_zone, world units); w 1 while the characters'\n"
    "                 // stencil mask (fx_cmask) is fx_ao's and fx_blur's input 11 and fx_comp's 10: the share is applied in fx_ao, not in fx_comp\n"
    "  float4 aoc2;   // x how far the occlusion is smoothed across a character's own surface (ao_soft_chars, world units each way); y 1 on\n"
    "                 // fx_blur's second use, that smoothing alone, after the temporal pass; z the sun's parts turned off (sun_gate's SG_*\n"
    "                 // under 256: fx_ao, sun_map); w the zone's curved surfaces' target's heap slot + 1, 0 none (LIFT_AT)\n"
    "  float4 spc;    // VanaCore's sparkles: how many places, their time in seconds (at their speed), their size, how many to a square unit\n"
    "  float4 spx;    // ... the world's x axis in view space\n"
    "  float4 spz;    // ... the world's other ground axis in view space\n"
    "  float4 spp;    // ... 1 for each the scene's colour where it was born (input 0), the time in seconds (their twinkle); z 1 where\n"
    "                 // input 1 is the characters' mask blurred (an aura's glow)\n"
    "  float4 sp[48]; // ... each three: its centre on the ground in view space, w the disc's radius; its colour, w how high they\n"
    "                 // rise; its style (0 sparkles, 1 a character's aura), size, amount and strength\n"
    "  float4 post4;  // the moonlight (moon_grade): how far into the night (GfxScene.night), its strength (night_moon); z the\n"
    "                 // grade's hue in radians (hue_turn)\n"
    "  float4 plight; // the player's light (player_light): their feet in view space, w the pool's radius (0: none)\n"
    "  float4 plight2; // ... its colour times its gain (0 by day), w the share of the light taken off round it (by day)\n"
    "  float4 gmc;    // VanaCore's rings of light on the ground (vanacore_gfx_set_ground_marks): how many, the time in seconds\n"
    "  float4 gmx;    // ... the world's x axis in view space\n"
    "  float4 gmz;    // ... the world's other ground axis in view space\n"
    "  float4 gm[64]; // ... each four: its centre in view space, w its radius; its colour, w how strongly; its shape (1 a\n"
    "                 // square), the height of the column of light over it (0 none), its glyph (0 none, 1-8) and the glyph's size;\n"
    "                 // how high over the ring the glyph's centre is (0: just over the column)\n"
    "  float4 rain;   // the rain's wet ground (fx_ssr): how wet (0 dry), rain.reflect, rain.puddles, its mask's heap slot + 1 (0 none)\n"
    "  float4x4 lmatm; // the middle cascade (sun_mid, input 7): view space to its map\n"
    "  float4 smapm;  // its texel in world units, depth bias, penumbra, slope\n"
    "  float4 smapm2; // its depth units, 1 when it is there\n"
    "};\n"
    "cbuffer CU : register(b0) { FxU u; };\n"
    "cbuffer FB : register(b1) { uint4 fti[2]; uint4 fsi[2]; };\n"
    "Texture2D<float4> tx2[] : register(t0, space1);\n"
    "SamplerState smp[] : register(s0);\n"
    /* the zone's curved surfaces at px (zn_draw's target, by its heap slot + 1 in u.aoc2.w; 0: none drawn). Here, in the part
     * Direct3D 11 swaps for its own registers (fx_source's BIND has its LIFT_AT, t21) */
    "#define LIFT_AT(px) (u.aoc2.w > 0.0 ? tx2[(uint)u.aoc2.w - 1].Load(int3(int2(px), 0)).rgb : float3(0.0, 0.0, 0.0))\n"
    /* the rain's wet ground's mask at px (d3d8_rain.h rain_mask_draw, by its heap slot + 1 in u.rain.w; 0: none), here for the
     * same reason (Direct3D 11's BIND: none) */
    "#define RAIN_AT(px) (u.rain.w > 0.0 ? tx2[(uint)u.rain.w - 1].Load(int3(int2(px), 0)) : float4(0.0, 0.0, 0.0, 0.0))\n"
    "SamplerComparisonState smpc[] : register(s0, space1);\n"
    "struct FO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "FO fx_vs(uint vid : SV_VertexID) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  FO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);\n"
    "static const uint BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
    /* the k-th input's heap slot, the sampler, the blur direction */
    "uint slot(uint k) { return k < 8 ? fti[(k >> 2) & 1][k & 3] : fsi[1][k & 3]; }\n"
    "float4 tap(uint k, float2 uv) { return tx2[slot(k)].SampleLevel(smp[fsi[0].x], uv, 0); }\n"
    "float4 tapl(uint k, float2 uv, float lv) { return tx2[slot(k)].SampleLevel(smp[fsi[0].x], uv, lv); }\n"
    "float4 texel(uint k, int2 p) { return tx2[slot(k)].Load(int3(p, 0)); }\n"
    "float2 size_of(uint k) { uint w, h; tx2[slot(k)].GetDimensions(w, h); return float2(w, h); }\n"
    "int2 fdir() { return int2(asint(fsi[0].y), asint(fsi[0].z)); }\n"
    /* view-space z of a depth value (clip w = z * P23, so z is negative in front of a right-handed
     * camera); 0 for the far plane (the sky, cleared depth) */
    "float view_z(float d) {\n"
    "  d = (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6);\n"
    "  float z = u.zp.y / (d * u.hand.x - u.zp.x);\n"
    "  if (d >= 0.999999 || !(z * u.hand.x > 0.0)) return 0.0;\n"
    "  return z;\n"
    "}\n"
    "float3 view_pos(float2 px, float z) {\n"
    "  float2 ndc = float2((px.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (px.y - u.vp.y) / u.vp.w * 2.0);\n"
    "  float w = z * u.hand.x;\n"
    "  return float3((ndc.x * w - u.proj.z * z) / u.proj.x, (ndc.y * w - u.proj.w * z) / u.proj.y, z);\n"
    "}\n"
    /* the depth is input k */
    "float3 pos_at(uint k, float2 px) {\n"
    "  px = clamp(px, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "  px = floor(px) + 0.5;\n"
    "  return view_pos(px, view_z(texel(k, int2(px)).r));\n"
    "}\n"
    /* the normal a normal-mapped draw made at px (nout_begin's target, heap slot u.clar.y), w 1; w 0 when `flag` is off, nothing was
     * drawn there, or what was (its z in alpha) is not the surface at P: something nearer has been drawn over it since. Not the
     * zone's smooth normals in the same target (twice as long: smooth_at). (One read of the target for both: Direct3D 11's
     * fx_source puts none in its place, once) */
    "float4 detail_at(float2 px) { return tx2[(uint)u.clar.y].Load(int3(int2(px), 0)); }\n"
    "float4 normal_det(float flag, float2 px, float3 P) {\n"
    "  if (flag > 0.0) {\n"
    "    float4 nd = detail_at(px);\n"
    "    if (nd.w != 0.0 && dot(nd.xyz, nd.xyz) < 2.0 && abs(abs(nd.w) - abs(P.z)) < 0.01 * abs(P.z) + 0.05)\n"
    "      return float4(nd.xyz, 1.0);\n"
    "  }\n"
    "  return float4(0.0, 0.0, 0.0, 0.0);\n"
    "}\n"
    /* the scene's smooth normal at px (vn_replay: its interpolated vertex normal, in the detail target longer than a normal
     * map's), w 1 where the game lights the surface by it (twice as long), 2 where it doesn't (three times: a zone shader's baked
     * colour); w 0 where there's none, or something else is at P now */
    "float4 smooth_at(float2 px, float3 P) {\n"
    "  if (u.sunuv.w > 0.0) {\n"
    "    float4 nd = detail_at(px);\n"
    "    float l2 = dot(nd.xyz, nd.xyz);\n"
    "    if (nd.w != 0.0 && l2 > 2.0 && abs(abs(nd.w) - abs(P.z)) < 0.01 * abs(P.z) + 0.05)\n"
    "      return float4(nd.xyz * rsqrt(l2), l2 > 6.25 ? 2.0 : 1.0);\n"
    "  }\n"
    "  return float4(0.0, 0.0, 0.0, 0.0);\n"
    "}\n"
    /* in the sun (1) or not (0): a ray from P toward the sun, stepped through the depth buffer (input 0);
     * a surface in front of it (by less than the thickness: what is far nearer the camera hides the
     * ray, it does not block it) shades P. Only on surfaces that face the sun (the rest are dark from
     * their own lighting), and fading out with distance, where the steps grow coarse. */
    "float sun_shadow(float3 P, float3 N, float dist, float k) {\n"
    "  float nl = dot(N, u.sunsh.xyz);\n"
    "  float fade = smoothstep(0.0, 0.15, nl) * (1.0 - smoothstep(0.6 * u.shadow.w, u.shadow.w, dist));\n"
    "  if (fade <= 0.0) return 1.0;\n"
    "  const int NS = 16;\n"
    "  float3 O = P + N * (0.01 * dist);\n"
    "  [loop] for (int s = 0; s < NS; ++s) {\n"
    "    float a = (float(s) + k) / float(NS);\n"
    "    float3 R = O + u.sunsh.xyz * (u.shadow.y * a * a);\n"
    "    float rd = R.z * u.hand.x;\n"
    "    if (rd <= 0.05) break;\n"
    "    float2 ndc = float2(R.x * u.proj.x + R.z * u.proj.z, R.y * u.proj.y + R.z * u.proj.w) / rd;\n"
    "    float2 q = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "    if (any(q < u.vp.xy) || any(q >= u.vp.xy + u.vp.zw)) break;\n"
    "    float sd = view_z(texel(0, int2(q)).r) * u.hand.x;\n"
    "    float in_front = rd - sd;\n"
    /* weaker the farther along the hit: no hard edge where the ray ends */
    "    if (sd > 0.0 && in_front > 0.005 * rd + 0.02 && in_front < u.shadow.z + 0.01 * rd) return 1.0 - fade * (1.0 - a * a);\n"
    "  }\n"
    "  return 1.0;\n"
    "}\n"
    /* in the sun (1) or not (0) by a shadow map (input km), softer the farther the shadow falls from what
     * casts it (percentage-closer soft shadows): the casters' average depth around the point sets how
     * wide the filter is; sixteen samples on a disk turned by the pixel's step of the 4x4 pattern, which
     * the blur then averages. P is moved off its surface by a texel and more with distance (no acne).
     * p: the cascade's texel (world units), depth bias, penumbra (map width per depth unit), slope
     * (depth units per map width); du its depth units; edge how far inside it P is (0 out) */
    /* ... in an order whose first 6, 8 and 12 are as even round the middle as all 16 (sun_taps takes the first n): the first n of
     * the order before leant to one side, up to half the disk's radius */
    "static const float2 DISK[16] = { float2(-0.82, -0.88), float2(0.31, 0.92), float2(0.54, -0.47), float2(0.79, 0.19),\n"
    "  float2(0.15, -0.33), float2(-0.92, 0.46), float2(0.44, -0.98), float2(-0.42, 0.87), float2(0.97, 0.76),\n"
    "  float2(0.34, 0.29), float2(-0.26, -0.42), float2(-0.94, -0.40), float2(-0.38, 0.28), float2(0.95, -0.77),\n"
    "  float2(-0.09, -0.93), float2(-0.03, 0.04) };\n"
    /* the maps' taps at dist: 16, and past each of the shadows' rings (sun_rings: aor.w world units apart, out from the camera) 12,
     * 8, then 6. A far pixel's filter covers a small patch of the screen, and the blur and the temporal pass even out the
     * grain; each ring's edge is moved by the pixel's step of the pattern (k), so the change is dithered, not a line */
    "int sun_taps(float dist, float k) {\n"
    "  if (u.aor.z <= 0.0) return 16;\n"
    "  float ring = dist / u.aor.w + 0.3 * (k - 0.5);\n"
    "  return ring < 1.0 ? 16 : ring < 2.0 ? 12 : ring < 3.0 ? 8 : 6;\n"
    "}\n"
    /* nl: N . the light's direction. A face
     * turned edge-on to the light needs its lift off the surface, and its depth bias, to grow as the surface leans
     * away from the map's planes: the lift by 1/N.L (a normal moved a texel gains nl of a texel toward the light),
     * the bias by the surface's slope to the light (tan, one at 45 degrees as before): stripes (acne) on walls
     * turned nearly parallel to the sun, which moved with each texel step of the map following the camera. A face
     * turned from the light (nl < 0: FFXI's smooth-shaded ground, lit by the game) is lifted straight toward the light
     * instead, as far: lifted along N (away from the light) or across it, it lands behind its own surface in the map -
     * the nearer edge-on, the farther behind - and shades itself. Toward the light it never does, so ground turning
     * through edge-on stays lit on both sides */
    /* bd: how far the casters it found stand from P along the light (their average; world units), 1e4 with none */
    /* skip: casters within this many world units of P along the light are left out (0 none): the surface's own rock, not a shadow
     * cast onto it (sun_map's faces turned from the light, whose neighbours shade them from the shared edge on) */
    /* g_snug: the look for casters and the filter drawn in from 32 texels' reach to 8 (1: all the way), sun_map's for a point it
     * lifted onto a curved surface's bulge: the wide ones' taps landed on the polygons beside it, the bulge's own */
    "static float g_snug = 0.0;\n"
    "float sun_look(uint km, float4x4 lm, float4 p, float du, float minw, float3 P, float3 N, float dist, float k, float nl, float skip,\n"
    "    float wide, out float edge, out float bd) {\n"
    "  bd = 1e4;\n"
    /* 0.1, not 0.2: ground under a low sun (10 degrees up is 0.17) and rock turned within 6 degrees of the light had half the bias
     * they need - bands of lit and shaded texels down the face (acne), a hatching inside whole triangles */
    "  float graze = 1.0 / max(nl, 0.1);\n"
    "  float lean = max(1.0, sqrt(max(1.0 - nl * nl, 0.0)) * graze);\n"
    "  float3 lift = nl < 0.0 ? u.sunsh.xyz : N;\n"
    /* ... the depth's (lean, below), not the lift's, on a face toward the light: lifted out along its normal by up to 15 texels
     * as it neared edge-on, a face under an overhang was looked up a unit or two off, out in the overhang's shadow, while its
     * neighbour turned more to the light, lifted less, stayed in the sun - the shadow went face by face down a low-polygon
     * rock, its edges the polygons' (the Crag of Holla's foot under its top, 2026-11-20). A texel and a half at most there,
     * times the 1.5: the shadow falls where the overhang casts it. Ground under a 10 degree sun kept no new bands with it */
    "  float3 Q = P + lift * (1.5 * p.x * (nl < 0.0 ? graze : min(graze, 1.5)) + 0.002 * dist);\n"
    /* sun_gate 64, the simplest shadow: P lifted a texel and a half off its plane and one toward the light, one look (the
     * comparison sampler's 2x2), the map's own bias, a hard edge */
    "  int gate = int(u.aoc2.z);\n"
    "  if ((gate & 64) != 0) Q = P + N * (1.5 * p.x) + u.sunsh.xyz * p.x;\n"
    "  float reach = lerp(32.0, 8.0, g_snug);\n"
    "  float4 lc = mul(lm, float4(Q, 1.0));\n"
    "  float2 uv = float2(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);\n"
    "  float2 e = abs(lc.xy);\n"
    /* sun_gate 16: no fading edge, in the map or out */
    "  edge = lc.z >= 1.0 ? 0.0 : (gate & 80) != 0 ? (max(e.x, e.y) < 1.0 ? 1.0 : 0.0) : 1.0 - smoothstep(0.8, 0.95, max(e.x, e.y));\n"
    "  if (edge <= 0.0) return 1.0;\n"
    "  if ((gate & 64) != 0) return tx2[slot(km)].SampleCmpLevelZero(smpc[fsi[0].w], uv, lc.z - p.y);\n"
    "  float z = lc.z - p.y, sz = size_of(km).x, tx = 1.0 / sz;\n"
    "  float a = k * 6.2831853, ca = cos(a), sa = sin(a);\n"
    "  float bs = 0.0, bn = 0.0;\n"
    /* the casters looked for with no fewer than 8 (sun_taps): they aren't turned with the pattern, and with too few a shadow's
     * soft edge, where only some of them find one, could find none and come out lit */
    "  int n = sun_taps(dist, k), nb = max(n, 8);\n"
    "  [loop] for (int s = 0; s < nb; ++s) {\n"
    "    float2 q = clamp((uv + DISK[s] * (reach * tx)) * sz, 0.0, sz - 1.0);\n"
    "    float d = texel(km, int2(q)).r;\n"
    "    if (d < z) { bs += d; bn += 1.0; }\n"
    "  }\n"
    "  if (bn == 0.0) return 1.0;\n"
    "  bd = (z - bs / bn) * du;\n"
    /* the penumbra: wider the farther the caster, never narrower than 2 cm (no hard pixel edge); with no
     * softness (sun_soft 0) half a texel, as sharp as the map has it */
    "  float least = u.aoq.y > 0.0 ? 0.5 * tx : max(1.5 * tx, 0.02 / (p.x * sz));\n"
    /* wide: a filter of at least this many texels' reach (sun_map's walls turned edge-on, whose stripes it averages out) */
    "  float pen = clamp((z - bs / bn) * p.z, max(least, wide * tx), reach * tx);\n"
    /* casters nearer the surface than sun_min are its own neighbouring faces (a ledge of the same
     * rock): no shadow from them. sun_min 0 keeps them all (smoothstep with equal edges is undefined: from XI on Anything,
     * rubymatrix/xi-on-anything ef145a0) */
    "  float own = minw > 0.0 ? smoothstep(0.5 * minw, 1.5 * minw, (z - bs / bn) * du) : 1.0;\n"
    /* a wider filter reaches farther across the surface, to points of it nearer the sun: the bias
     * grows with it (a slope of one); never under the soft edge's floor of a texel and a half, whatever the
     * filter is: with hard edges (sun_soft 0) half a texel left a slope speckled (acne), and a sloped roof
     * seen against the sun showed it as a pattern of dots in bands */
    "  float zc = z - (skip > 0.0 ? skip / du : 0.0) - max(pen, 1.5 * tx) * p.w * lean, sum = 0.0;\n"
    "  [loop] for (int t = 0; t < n; ++t) {\n"
    "    float2 dk = float2(ca * DISK[t].x - sa * DISK[t].y, sa * DISK[t].x + ca * DISK[t].y);\n"
    "    sum += tx2[slot(km)].SampleCmpLevelZero(smpc[fsi[0].w], uv + dk * pen, zc);\n"
    "  }\n"
    "  return lerp(1.0, sum / float(n), own);\n"
    "}\n"
    /* in the sun (1) or not (0) by the maps (inputs 1 far, 2 near, 7 middle): the near cascade where P is in it, the middle
     * one (sun_mid) beyond it, the far one beyond that, each blended across the edge of the one inside it. wall 0 but on a flat
     * wall (flank_ok) and on the grass. Ng the surface's own plane (from the depth); Ns the zone's smooth normal there
     * (smooth_at), w 0 where there's none */
    /* g_minc: on a character's pixel (sun_char, set by fx_ao), casters within half a unit of it along the light are left out, as
     * sun_min leaves them out everywhere: its own plates and limbs. A Rock Golem shaded its body with its own rock, in blotches
     * that jumped as it moved (characters keep no history in the temporal pass), "flickering like crazy" (Sanctuary of Zi'Tah,
     * 2026-11-02 23:08; gone with sun_min 0.5, which would take the zone's rocks' own shading too) */
    "static float g_minc = 0.0;\n"
    /* the zone's own curved surface at the pixel (zn_draw's target, LIFT_AT: by its heap slot here, t21 in Direct3D 11): how far
     * it lies under the smooth surface its vertex normals describe (x) and the bulge's own height there (y), world units, and how
     * curved, 0 to 1 (z). Set by fx_ao for its own pixel alone */
    "static float3 g_lift = float3(0.0, 0.0, 0.0);\n"
    /* the middle map's share of the last sun_cascades past the sharp one's (Render visual 23) */
    "static float g_em = 0.0;\n"
    /* en: the sharp map's share; ef: the rest's (the middle map's and the far one's together), the pixel lit where none reaches */
    "float sun_cascades(float3 P, float3 N, float dist, float k, float nl, float skip, float wide, out float en, out float ef) {\n"
    "  float s = 1.0, bd, bdf, minw = max(u.smap2.z, g_minc), em = 0.0;\n"
    "  en = 0.0, ef = 0.0;\n"
    "  if (u.smapn2.y > 0.0)\n"
    "    s = sun_look(2, u.lmatn, u.smapn, u.smapn2.x, minw, P, N, dist, k, nl, skip, wide, en, bd);\n"
    "  if (en < 1.0) {\n"
    "    float sr = 1.0;\n"
    "    if (u.smapm2.y > 0.0)\n"
    "      sr = sun_look(7, u.lmatm, u.smapm, u.smapm2.x, minw, P, N, dist, k, nl, skip, wide, em, bd);\n"
    "    if (em < 1.0) {\n"
    "      float sf = sun_look(1, u.lmat, float4(u.smap.yzw, u.smap2.x), u.smap2.w, minw, P, N, dist, k, nl, skip, wide, ef, bdf);\n"
    "      sr = lerp(sf, sr, em);\n"
    "    }\n"
    "    ef = lerp(ef, 1.0, em);\n"
    "    s = lerp(sr, s, en);\n"
    "  }\n"
    "  g_em = em;\n"
    "  return s;\n"
    "}\n"
    /* how much of sun_map's edge-on fade the pixel took (its flank), for Render visual 21 (fx_ao) */
    "static float g_flank = 0.0;\n"
    /* ... the maps' own shadow before anything below changes it (22), and how much of it is the sharp map's and the middle one's (23) */
    "static float g_raw = 1.0, g_en = 0.0, g_emv = 0.0;\n"
    /* ... and how far sun_map trusts the smooth normal there (28) */
    "static float g_trust = 0.0;\n"
    "float sun_map(float3 P, float3 N, float3 Ng, float4 Ns, float dist, float k, float wall) {\n"
    "  int gate = int(u.aoc2.z);\n"
    "  float nl = dot(N, u.sunsh.xyz), nlg = dot(Ng, u.sunsh.xyz);\n"
    /* turned from the sun: shaded by the angle only as much as sun_face asks (FFXI's own lighting shades
     * its rock already; per face, low-polygon rock turns into a patchwork). The map decides on those faces
     * too: FFXI lights its ground with smooth normals, so a facet of it turned a little from the sun is lit
     * by the game, and taking the map's shadow off it (as this did) left lit triangles inside shadows. */
    /* The angle is the smooth normal's (Ns) where the scene has one, else the surface's as before (a normal map's, or the depth's
     * plane): by the plane, each triangle of a curved low-polygon surface was its own flat grey, a checkerboard of squares over
     * the Crag of Dem's sides (Konschtat, 2026-11-11; Xarcabard's snow, 2026-11-10), where the game shades it smoothly. It goes
     * by the shadows' own sun and takes no shadow away, so it stays with sun_game_shading off (gate 256, below) */
    /* ... but only where the smooth normal lies within some 26 degrees of the plane (tr; none past 37): on a curved shape it
     * is never farther off than half the turn between two of its polygons, while a building's is averaged across its corners
     * and leans up to 45 degrees off the wall beside one (Render visual 28: in stripes along Southern San d'Oria's ledges), and
     * by it a lit wall under a high sun was shaded. Untrusted, the angle is the plane's as it always was. (8 and 16 degrees
     * until 2026-11-21: an octagonal rock, its polygons 45 degrees apart and its normals 22 off them, was trusted in blobs and
     * shaded polygon by polygon elsewhere - a grey box with hard edges in the lit patch on the Crag of Holla's foot.)
     * The shadow's own edge round a curved shape (dark blocks with a toothed edge over the Crag of Dem's dome under a grazing
     * sun, sun_game_shading off) is NOT taken from the smooth normal, though that draws it as a ramp: tried 2026-11-11, even
     * within tr a flat wall a few degrees past edge-on reads as a curved shape's terminator, and walls in a tower's shadow
     * were lit in streaks. Nor does fading the maps' shadow in by the caster's distance on near edge-on polygons reach those
     * blocks (their caster is the dome's far bulk, not the quad before). It wants the meshes told apart, not the pixels */
    /* (a curved surface of the zone's own, g_lift.z, is trusted by how curved: told from an averaged corner on the CPU) */
    "  float cv = Ns.w > 0.0 ? saturate(g_lift.z) : 0.0;\n"
    "  float tr = Ns.w > 0.0 ? max(smoothstep(0.80, 0.90, dot(Ns.xyz, Ng)), cv) : 0.0;\n"
    "  g_trust = tr;\n"
    "  float nfs = dot(Ns.xyz, u.sunsh.xyz);\n"
    "  float nf = lerp(nl, nfs, tr);\n"
    "  float face = lerp(1.0 - 0.6 * u.smap2.y, 1.0, smoothstep(-0.3, 0.25, nf));\n"
    /* The map's bias goes by the surface's own plane (Ng, the depth's), not by the normal a normal map put on it: a bump tilted
     * toward the sun lifted the point and biased it by far less than its polygon needs, and in a noise down the texture (a normal
     * map's N moves per pixel) it landed on a different texel each time - lit and shaded bands inside one face */
    /* The zone's own curved surfaces (cv; Direct3D 11's zn_draw, sun_curved): the maps hold the polygons, and a polygon turned
     * from the light is shaded whole by its neighbours in front of it - round the Crag of Dem's dome the shadow's edge was a
     * staircase of quads 8 units across, and under a high sun its lower half a wall of dark squares. The surface its normals
     * describe lies outside those polygons (they are cut across its bulge), and from a point on it turned to the light nothing
     * of the same bulge stands in the way: so the maps are asked from there (g_lift: the point moved out along its normal onto
     * that surface, Phong tessellation's, and half as far again), on polygons turned from the light or within 11 degrees of
     * edge-on to it (none past 20). What another rock casts on it is still found, at any distance, a hollow has no bulge (lift
     * 0: as the maps have it), and the lift is a unit or less, so what stands over the surface still shades it. Tried and
     * dropped, 2026-11-11: leaving the casters out by their distance (8 to 24 units: a shoulder of the dome in its other
     * shoulder's shadow came out lit, or the staircase stayed); lifting by the filter's reach or a share of the polygon's size
     * as well (the wall under the dome's overhang and the spire's neck left their shadow: lit shards under a high sun) */
    "  float3 Pm = P, Nm = Ng;\n"
    "  float nlm = nlg;\n"
    "  if (cv > 0.0) {\n"
    "    float wl = cv * (1.0 - smoothstep(0.2, 0.35, nlg)), wt = cv * (1.0 - smoothstep(0.1, 0.2, nlg));\n"
    "    Pm = P + Ns.xyz * (g_lift.x * 1.5 * wl + g_lift.y * wt);\n"
    "    Nm = normalize(lerp(Ng, Ns.xyz, wl));\n"
    "    nlm = dot(Nm, u.sunsh.xyz);\n"
    "    g_snug = wl;\n"
    "  }\n"
    "  float en, ef, s = sun_cascades(Pm, Nm, dist, k, nlm, 0.0, 0.0, en, ef);\n"
    "  g_snug = 0.0;\n"
    "  g_raw = lerp(1.0, s, max(en, ef)), g_en = en, g_emv = g_em;\n"
    /* sun_gate: 64 the maps' shadow alone; 8 no shading by the angle (face); 2 no second look on faces turned away (below) */
    "  if ((gate & 64) != 0) return g_raw;\n"
    "  if ((gate & 8) != 0) face = 1.0;\n"
    /* ... and the shadow's edge there is its normal's own: none while the normal has it turned toward the light by 3 degrees,
     * all of it from 11.5 turned away, the ramp the game's own light fades across - on polygons facing the light too, so the
     * two meet at the edge they share. Flat walls and averaged corners have cv 0: as the maps have them */
    /* (a polygon itself turned well toward the light, 23 degrees and more, keeps its light whatever its normals say: within a
     * curved shape they lie no farther than half the turn between two polygons from it, and where they do - a skirt's, averaged
     * with the ground's - a lit polygon was shaded whole) */
    "  if (cv > 0.0) s = lerp(s, min(s, max(smoothstep(-0.20, 0.05, nfs), smoothstep(0.35, 0.55, nlg))), cv);\n"
    /* The maps go by the polygons (vn_replay): a face turned from the light by its own plane (Ng) is shaded by its neighbours
     * in front of it from the edge they share on - a hard line down a low-polygon rock under a low sun - where the smooth normal
     * (Ns, the game's own) has it lit yet. What stands within 3 units of it along the light is its own rock, left to the smooth
     * normal (below); what is farther is a shadow cast onto it (a pillar's, an overhang's), kept. Casters are told apart one by
     * one, not by their average distance: a face in a real shadow with a few of its own edges in its filter came out lit */
    "  if (Ns.w > 0.0 && (gate & (2 | 256)) == 0) {\n"
    /* (not the zone's curved surfaces: asked from their lifted point above) */
    "    float turned = (1.0 - smoothstep(-0.02, 0.08, nlg)) * (1.0 - cv);\n"
    "    float e1, e2;\n"
    "    if (turned > 0.0) s = lerp(s, sun_cascades(P, Ng, dist, k, nlg, 3.0, 0.0, e1, e2), turned);\n"
    "  }\n"
    /* a wall turned edge-on to the light (a high moon down Whitegate's walls): each of the maps' texels covers a stripe
     * of it down the light, and what juts out above (a ledge, the coping) shades whole stripes or not by a texel's step,
     * bars over the whole wall. It takes next to none of that light: the maps' shadow fades out as it turns edge-on.
     * Steep faces only (the ground is lit by smooth normals, above), flat ones (flank_ok): not characters or rounded models
     * (in a building's shadow a band round their sides would light up, facet by facet) */
    /* (nor the zone's curved surfaces: cv) */
    "  float flank = wall * (1.0 - cv) * (1.0 - smoothstep(0.05, 0.25, abs(nl))) * (1.0 - smoothstep(0.4, 0.6, abs(dot(N, u.up.xyz))));\n"
    "  g_flank = flank;\n"
    /* ... but only where the shadow there is patchy: a wall in a real shadow (an overhang's, a cliff's, under a high sun every
     * flat wall is edge-on) is shaded all over, not in stripes. A wider filter (4 texels' reach) tells them apart: lit by a
     * share of it, the stripes are taken away as before; all shaded, the shadow stays, never darker than the narrow filter's.
     * (Eased over 0..0.6 of 6 texels, with flank_ok eased too, it lifted the shadow off far more walls and the whole zone
     * flickered worse: Port Jeuno, 2026-11-01 23:56. Back as it was.) */
    "  float lit = s;\n"
    "  if (flank > 0.0) {\n"
    "    float e3, e4;\n"
    "    lit = max(s, smoothstep(0.05, 0.4, sun_cascades(P, Ng, dist, k, nlg, 0.0, 4.0, e3, e4)));\n"
    "  }\n"
    "  float m = lerp(1.0, min(lerp(s, lit, flank), face), max(en, ef));\n"
    /* ... and by the smooth normal, as smooth shading lights it. Where the game lights the surface by it (w 1: its sun goes as
     * the normal turns from it, smoothly, across the faces), the maps take away only that light: all of it facing the sun, none
     * turned away, where the game gave none - the dark side is the game's own. Where it doesn't (w 2: a zone shader's baked
     * colour), the sun is ours alone: whole facing it, shaded as the surface turns away, as dark as a shadow - out past the
     * maps too. What the game gave is measured by the game's own light (u.sun), not the shadows' (u.sunsh): they part with a
     * sun placed by hand, the tilt (sun_tilt) and the dusk clamp, and ground the game lit full in a low placed sun's shadow,
     * turned from that sun by a few degrees, had the shadow taken off it - a statue's shadow in the maps, none on the ground
     * (Lower Jeuno, 2026-11-04) */
    /* (256: sun_game_shading off - the maps alone shade, nothing weighed against the game's own light) */
    "  if ((gate & 256) != 0) return m;\n"
    "  if (Ns.w > 1.5) m = min(m, smoothstep(-0.05, 0.25, dot(Ns.xyz, u.sunsh.xyz)));\n"
    "  else if (Ns.w > 0.0) m = lerp(1.0, m, smoothstep(0.0, 0.25, dot(Ns.xyz, u.sun.w > 0.0 ? u.sun.xyz : u.sunsh.xyz)));\n"
    "  return m;\n"
    "}\n"
    /* 1 where the scene's pixel at px is a character's (sun_soft_chars, fx_blur; flank_ok): the stencil's mask where their draws
     * marked it (cmask_pass, input 11 of fx_ao and fx_blur alike, u.aoc.w): exact, the same frame. Else the characters' own depth
     * from the camera (input 10, sun_map_view: its viewport 0..1, the scene's zp.zw) is the scene's there, within 10 cm and a
     * hundredth of the distance, at any of the four texels round it (it's coarser than the screen) - a test a pixel at a
     * character's edge, or a hair off in depth, passes one frame and fails the next: the blur on it came and went, a flicker */
    /* (the mask first: fx_ao's g_minc asks with the characters' own blur off, sun_soft_chars 0, and their depth not drawn) */
    "float sun_char(float2 px, float3 P, float dist) {\n"
    "  if (u.aoc.w > 0.0) return texel(11, int2(floor(px))).r > 0.5 ? 1.0 : 0.0;\n"
    "  if (u.smapn2.z <= 0.0) return 0.0;\n"
    "  float2 sz = size_of(10);\n"
    "  int2 q = int2(floor((px - u.vp.xy) / u.vp.zw * sz - 0.5));\n"
    "  float tol = 0.1 + 0.01 * dist;\n"
    "  [unroll] for (int t = 0; t < 4; ++t) {\n"
    "    float d = texel(10, clamp(q + int2(t & 1, t >> 1), 0, int2(sz) - 1)).r;\n"
    "    float z = d < 1.0 ? view_z(u.zp.z + d * (u.zp.w - u.zp.z)) : 0.0;\n"
    "    if (z != 0.0 && abs(z - P.z) < tol) return 1.0;\n"
    "  }\n"
    "  return 0.0;\n"
    "}\n"
    /* sun_char for the ink lines (outline_width_chars): the characters' depth is input 9 of the composite */
    "bool char_line(float2 px, float3 P, float dist) {\n"
    "  float2 sz = size_of(9);\n"
    "  int2 q = int2(floor((px - u.vp.xy) / u.vp.zw * sz - 0.5));\n"
    "  float tol = 0.1 + 0.01 * dist;\n"
    "  [unroll] for (int t = 0; t < 4; ++t) {\n"
    "    float d = texel(9, clamp(q + int2(t & 1, t >> 1), 0, int2(sz) - 1)).r;\n"
    "    float z = d < 1.0 ? view_z(u.zp.z + d * (u.zp.w - u.zp.z)) : 0.0;\n"
    "    if (z != 0.0 && abs(z - P.z) < tol) return true;\n"
    "  }\n"
    "  return false;\n"
    "}\n"
    /* char_line's test for Render visual 19: how many of the four texels of the characters' depth round the pixel match it
     * (0 not a character's, 4 well inside one, 1-3 where it is a texel or two from the edge or a hair off in depth) */
    "int char_hits(float2 px, float3 P, float dist) {\n"
    "  float2 sz = size_of(9);\n"
    "  int2 q = int2(floor((px - u.vp.xy) / u.vp.zw * sz - 0.5));\n"
    "  float tol = 0.1 + 0.01 * dist;\n"
    "  int n = 0;\n"
    "  [unroll] for (int t = 0; t < 4; ++t) {\n"
    "    float d = texel(9, clamp(q + int2(t & 1, t >> 1), 0, int2(sz) - 1)).r;\n"
    "    float z = d < 1.0 ? view_z(u.zp.z + d * (u.zp.w - u.zp.z)) : 0.0;\n"
    "    if (z != 0.0 && abs(z - P.z) < tol) n++;\n"
    "  }\n"
    "  return n;\n"
    "}\n"
    /* The occlusion's depth, at its own size: the view z of the scene pixel each of its texels stands on (as
     * fx_ao's P; input 0 the scene's depth), then levels of half the size, each texel one of the 2x2 above it
     * in turn (SAO's rotated grid: a depth that is there, never a mix across an edge). The occlusion's far taps
     * read the small levels, which stay in the cache: from the scene's full depth (4096 across) its taps, up to
     * a tenth of the view apart, missed it nearly every time. */
    "float4 fx_linz(FO i) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + i.uv * u.vp.zw) + 0.5;\n"
    "  return float4(view_z(texel(0, int2(px)).r), 0.0, 0.0, 0.0);\n"
    "}\n"
    "float4 fx_zmip(FO i) : SV_Target {\n"
    "  int2 p = int2(i.pos.xy), hi = int2(size_of(0)) - 1;\n"
    "  return texel(0, min(p * 2 + int2(p.y & 1, p.x & 1), hi));\n"
    "}\n"
    /* the scene's position at scene pixel q from those levels (inputs 3-6), read at the one whose texels are
     * about an eighth of r, the tap's distance in scene pixels */
    "float3 pos_lz(float2 q, float r) {\n"
    "  float2 t = (q - u.vp.xy) * u.size.zw / u.vp.zw;\n"
    "  int lv = clamp(int(floor(log2(max(r * u.size.z / u.vp.z, 1.0)))) - 3, 0, 3);\n"
    "  int2 p = int2(t) >> lv;\n"
    "  float z;\n"
    "  [branch] if (lv == 0) z = texel(3, p).r;\n"
    "  else if (lv == 1) z = texel(4, p).r;\n"
    "  else if (lv == 2) z = texel(5, p).r;\n"
    "  else z = texel(6, p).r;\n"
    "  return view_pos(q, z);\n"
    "}\n"
    /* the surface's slope along one screen axis e at px (input 0 the depth; zq the depth buffer's step at P in
     * view units, ps a pixel's width there): a difference across as many pixels as it takes for the steps of
     * the depth to be a fiftieth of the run (a 16-bit depth is a centimetre or more a step, and across one pixel
     * it tilts the normal by far more than the occlusion's bias: a slope shows it as bands), kept to where
     * the surface stays one plane (1/z is linear across a plane); else the neighbor nearer in depth (no
     * smearing across edges). A tenth left the normal some 6 degrees astray, in diagonal bands down every
     * face the depth's steps crossed: the sun's shadows read it for their lift, the edge-on fade and the trust in the
     * smooth normal, and a face nearly edge-on to the sun came out striped (the Crag of Holla's foot, 2026-11-20) */
    "float3 ao_axis(float2 px, float3 P, float2 e, float zq, float ps) {\n"
    "  float n = exp2(ceil(log2(clamp(zq / (0.02 * ps), 1.0, 16.0))));\n"
    "  [loop] for (int t = 0; t < 5; ++t) {\n"
    "    float3 a = pos_at(0, px + e * n), b = pos_at(0, px - e * n);\n"
    "    if (a.z != 0.0 && b.z != 0.0 &&\n"
    "        abs(1.0 / a.z + 1.0 / b.z - 2.0 / P.z) * P.z * P.z <= 2.0 * zq + 0.1 * n * ps) return a - b;\n"
    "    if (n <= 1.0) break;\n"
    "    n *= 0.5;\n"
    "  }\n"
    "  float3 r = pos_at(0, px + e) - P, l = P - pos_at(0, px - e);\n"
    /* at the viewport's edge one side is the pixel itself (pos_at clamps to it): zero, its cross a NaN (from XI on
     * Anything, rubymatrix/xi-on-anything 6e26f8b) */
    "  return (abs(r.z) < abs(l.z) && dot(r, r) > 0.0) || dot(l, l) == 0.0 ? r : l;\n"
    "}\n"
    /* the occlusion's taps at dist, of NS (ao_quality): past each of the rings (ao_rings: aor.y world units apart, out from
     * the camera) fewer, all inside the first, then 75%, 50% and 30%, never under 4. Each ring's edge is moved by the pixel's
     * step of the 4x4 pattern (b), so the change is dithered across the blur's block, not a line. Never fewer than the
     * blur's block needs to cover the taps' disc (rpx pixels round): its 16 pixels' taps, about one each pixel of it. Out to
     * a disc of ~13 pixels (some 100 world units) that is all of them: cut there, the frame's grain grew by up to 1.8x and
     * shimmered where the temporal pass gathers few frames (the Max quality was Medium's past the second ring) */
    "int ao_taps(int NS, float dist, float rpx, float b) {\n"
    "  if (u.aor.x <= 0.0) return NS;\n"
    "  float ring = dist / u.aor.y + 0.3 * (b - 0.5);\n"
    "  float keep = ring < 1.0 ? 1.0 : ring < 2.0 ? 0.75 : ring < 3.0 ? 0.5 : 0.3;\n"
    "  return max(int(float(NS) * keep + 0.5), min(NS, max(int(ceil(0.2 * rpx * rpx)), 4)));\n"
    "}\n"
    /* how far sun_map's edge-on fade may reach P (its wall): where the surface goes on flat a quarter of a unit round P along
     * both of the screen's axes (to one side of each at least: by a corner, under a ledge, it stays), and not on a character
     * (sun_char). The fade is for the bars down a wall; on a rounded model (a moogle, a barrel, a character sun_char doesn't
     * find) each facet turned edge-on to the light came out lit on its own, a patchwork of lit triangles inside a room's
     * shadow. Ng: the normal from the depth (a normal map's would tilt a flat wall off its own plane); zq the depth's step
     * at P. No taps where the fade takes nothing anyway (sun_map's edge-on and steep test) */
    "float flank_ok(float2 px, float3 P, float3 Ng, float3 N, float dist, float zq) {\n"
    "  if (abs(dot(N, u.sunsh.xyz)) >= 0.25 || abs(dot(N, u.up.xyz)) >= 0.6) return 0.0;\n"
    "  float rp = max(0.25 * u.proj.y * 0.5 * u.vp.w / dist, 2.0);\n"
    "  float2 flat_ = float2(0.0, 0.0);\n"
    "  [unroll] for (int t = 0; t < 4; ++t) {\n"
    "    float3 Q = pos_at(0, px + (t < 2 ? float2(rp, 0.0) : float2(0.0, rp)) * ((t & 1) ? -1.0 : 1.0)), v = Q - P;\n"
    "    if (Q.z != 0.0 && abs(dot(v, Ng)) <= 0.05 * length(v) + 2.0 * zq) flat_[t >> 1] = 1.0;\n"
    "  }\n"
    "  return flat_.x * flat_.y * (1.0 - sun_char(px, P, dist));\n"
    "}\n"
    /* occlusion in x (1 open, 0 closed), distance in y (0: sky), the sun by the map in z and by contact
     * in w (1 lit, 0 shaded); inputs the depth, the sun's two maps and the occlusion's depth (fx_linz).
     * With VanaCore's grass, input 0 is the depth before it (gfx_scene_world_depth) and input 8 the depth with
     * it: the grass casts no occlusion or contact shadow, and a pixel it covers takes none, only the sun's maps
     * at its own place, by the slope of the ground behind it (below) */
    "float4 fx_ao(FO i) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + i.uv * u.vp.zw) + 0.5;\n"
    /* VanaCore's water with water.shadows 0 (u.aoc2.z's 512; input 9 the depth before the water pass, gfx_scene_snapshot): where
     * the pass changed the depth, lit and with no occlusion */
    "  if ((int(u.aoc2.z) & 512) != 0 && texel(9, int2(px)).r != texel(8, int2(px)).r)\n"
    "    return float4(1.0, max(pos_at(8, px).z * u.hand.x, 0.0), 1.0, 1.0);\n"
    "  int2 cell = int2(i.pos.xy) & 3;\n"
    /* the sun's shadows' pattern turns each frame for the temporal pass; held still where DLSS or FSR gathers the frames
     * (u.aoq.w 2): that pass is off then, and the upscaler kept each frame's new grain, a flicker on walls and characters.
     * The maps are looked up where the frame's jitter drew the surface (u.hand.zw, render pixels; 0 without DLSS or FSR): the
     * depth at px is the point the jittered draw put there, and taken at px itself it stood off the surface by up to half a
     * pixel, a different way each frame. On a face the sun grazes that moved the shadow's edge several pixels, and with the
     * temporal pass off FSR showed it: the edge of a block's shadow by a spire flickered (2026-11-04, gone with FSR off) */
    "  float b = (float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0;\n"
    "  float k = u.aoq.w > 1.5 ? b : frac(b + u.hist.y);\n"
    "  if (texel(8, int2(px)).r != texel(0, int2(px)).r) {\n"
    "    float3 G = pos_at(8, px);\n"
    "    float gd = G.z * u.hand.x;\n"
    "    float gm = 1.0;\n"
    /* the tuft takes the shadow the ground round it does: by that ground's plane and smooth normal, the ground behind it at px
     * (input 0, the depth before the grass; vn_replay drew the smooth normals against it). By the up normal, on a hill turned
     * from a low sun - shaded by the game's own light, which the maps leave alone (sun_map) - the tufts took the hill's own
     * shadow: dark patches on a slope that had none (Batallia Downs, 2026-11-03). Up as before where what's behind is the sky, a
     * steep face, or ground well beyond the blade (a crest against another hill) */
    "    if (u.smap.x > 0.0 && u.sun.w > 0.0 && gd > 0.0) {\n"
    "      float3 Nb = u.up.xyz;\n"
    "      float4 Nsb = float4(0.0, 0.0, 0.0, 0.0);\n"
    "      float3 B = pos_at(0, px);\n"
    "      float bd = B.z * u.hand.x;\n"
    "      if (bd > 0.0) {\n"
    "        float zq = u.aoq.z * B.z * B.z / (max(u.zp.w - u.zp.z, 1e-6) * abs(u.zp.y));\n"
    "        float ps = bd / (0.5 * u.proj.y * u.vp.w);\n"
    "        float3 nc = cross(ao_axis(px, B, float2(1, 0), zq, ps), ao_axis(px, B, float2(0, 1), zq, ps));\n"
    "        if (dot(nc, nc) > 1e-24) {\n"
    "          float3 Nbg = normalize(nc);\n"
    "          if (dot(Nbg, B) > 0.0) Nbg = -Nbg;\n"
    "          float w = smoothstep(0.5, 0.7, dot(Nbg, u.up.xyz)) * (1.0 - smoothstep(3.0, 8.0, length(B - G)));\n"
    "          Nb = normalize(lerp(u.up.xyz, Nbg, w));\n"
    "          float4 Ns = (int(u.aoc2.z) & 1) == 0 ? smooth_at(px, B) : float4(0.0, 0.0, 0.0, 0.0);\n"
    "          if (Ns.w > 0.0) Nsb = float4(normalize(lerp(u.up.xyz, Ns.xyz, w)), Ns.w);\n"
    "        }\n"
    "      }\n"
    "      gm = sun_map(view_pos(px - u.hand.zw, G.z), Nb, Nb, Nsb, gd, k, 0.0);\n"
    "    }\n"
    "    return float4(1.0, max(gd, 0.0), gm, 1.0);\n"
    "  }\n"
    "  float3 P = pos_at(0, px);\n"
    "  float dist = P.z * u.hand.x;\n"
    "  if (dist <= 0.0) return float4(1.0, 0.0, 1.0, 1.0);\n"
    /* the surface normal from the depth around the pixel (ao_axis), and the depth's step at P (zq) */
    "  float zq = u.aoq.z * P.z * P.z / (max(u.zp.w - u.zp.z, 1e-6) * abs(u.zp.y));\n"
    "  float ps = dist / (0.5 * u.proj.y * u.vp.w);\n"
    "  float3 dx = ao_axis(px, P, float2(1, 0), zq, ps), dy = ao_axis(px, P, float2(0, 1), zq, ps);\n"
    "  float3 nc = cross(dx, dy);\n"
    "  float3 N = dot(nc, nc) > 1e-24 ? normalize(nc) : -normalize(P);\n"
    "  if (dot(N, P) > 0.0) N = -N;\n"
    "  float3 Ng = N;\n"
    /* sun_gate (u.aoc2.z): 128 no normal maps' normals, 32 no contact shadows, 1 no smooth normals, 4 no edge-on fade */
    "  int gate = int(u.aoc2.z);\n"
    "  float4 ndet = normal_det(u.clar.w, px, P);\n"
    "  if (ndet.w > 0.0 && (gate & 128) == 0) N = ndet.xyz;\n"
    /* the samples turned and scaled by one of 16 steps, a 4x4 ordered pattern over the pixels (the blur
     * averages exactly one 4x4 block, fx_blur), and a turn each frame the temporal pass evens out: k, above */
    "  float sh = 1.0, mp = 1.0;\n"
    "  if (u.shadow.x > 0.0 && u.sun.w > 0.0 && (gate & 32) == 0) sh = sun_shadow(P, N, dist, k);\n"
    "  float4 Ns = (gate & 1) == 0 ? smooth_at(px, P) : float4(0.0, 0.0, 0.0, 0.0);\n"
    "  if (u.smap.x > 0.0 && u.sun.w > 0.0) {\n"
    "    g_minc = sun_char(px, P, dist) > 0.0 ? 0.5 : 0.0;\n"
    "    g_lift = Ns.w > 0.0 ? LIFT_AT(px) : float3(0.0, 0.0, 0.0);\n"
    "    mp = sun_map(view_pos(px - u.hand.zw, P.z), N, Ng, Ns, dist, k, (gate & 4) == 0 ? flank_ok(px, P, Ng, N, dist, zq) : 0.0);\n"
    "    g_lift = float3(0.0, 0.0, 0.0);\n"
    "  }\n"
    /* Render visuals, in the map's slot: 21 where sun_map's edge-on fade takes the maps' stripes off a wall (black: all of it);
     * 22 the maps' shadow before sun_map changes it; 23 the sharp map's share (white: all of it; grey: the middle map's, sun_mid;
     * black: the far map's); 24 the smooth normals (grey
     * where the game lights by them, white where its baked colour); 25 the surface's plane from the depth, as it faces the sun
     * (white facing it, grey edge-on, black turned away); 28 how far sun_map trusts the smooth normal (white: wholly, dark grey:
     * there is one but it leans too far from the plane, black: none); 29 the zone's curved surfaces (sun_curved: dark grey where
     * one is, whiter the farther the pixel is lifted onto its smooth surface, white from 2 units; black: none) */
    "  int dv = int(u.grade.w);\n"
    "  if (dv == 21) return float4(1.0, dist, 1.0 - g_flank, 1.0);\n"
    "  if (dv == 22) return float4(1.0, dist, g_raw, 1.0);\n"
    "  if (dv == 23) return float4(1.0, dist, g_en + (1.0 - g_en) * 0.5 * g_emv, 1.0);\n"
    "  if (dv == 24) return float4(1.0, dist, 0.5 * Ns.w, 1.0);\n"
    "  if (dv == 25) return float4(1.0, dist, 0.5 + 0.5 * dot(Ng, u.sunsh.xyz), 1.0);\n"
    "  if (dv == 28) return float4(1.0, dist, Ns.w > 0.0 ? 0.25 + 0.75 * g_trust : 0.0, 1.0);\n"
    "  if (dv == 29) return float4(1.0, dist, Ns.w > 0.0 && LIFT_AT(px).z > 0.0 ? 0.25 + 0.75 * saturate(0.5 * (LIFT_AT(px).x + LIFT_AT(px).y)) : 0.0, 1.0);\n"
    "  float rad = u.ao.x, rpx = min(rad * u.proj.y * 0.5 * u.vp.w / dist, u.ao.w);\n"
    "  if (u.ao.y <= 0.0 || rpx < 2.0) return float4(1.0, dist, mp, sh);\n"
    /* a few taps a frame (ao_quality), turned each frame: the temporal pass gathers them. Held still (u.aoq.w) where DLSS or
     * FSR gathers the frames (it takes a new pattern each frame for detail and keeps it, a shimmer) or the temporal pass
     * gathers too few to even out the pattern's 16 steps (temporal under 0.9: a new grain each frame, a shimmer); held
     * still, the blur's 4x4 block already averages them */
    "  float ka = u.aoq.w > 0.0 ? b : k;\n"
    "  int NS = ao_taps(max(int(u.aoq.x), 1), dist, rpx, b);\n"
    "  float sum = 0.0;\n"
    "  [loop] for (int s = 0; s < NS; ++s) {\n"
    "    float a = (float(s) + ka) / float(NS);\n"
    "    float ang = float(s) * 2.3999632 + ka * 6.2831853;\n"
    "    float2 q = px + float2(cos(ang), sin(ang)) * (a * rpx);\n"
    /* off the screen: nothing known there (clamped to the edge, the edge's own pixels would shade it,
     * and move with the camera) */
    "    if (any(q < u.vp.xy) || any(q >= u.vp.xy + u.vp.zw)) continue;\n"
    "    float3 Q = pos_lz(q, a * rpx);\n"
    /* a surface far nearer the camera floats in front of this one: it hides it, it does not shade it */
    "    if (Q.z == 0.0 || dist - Q.z * u.hand.x > 0.5 * rad) continue;\n"
    "    float3 v = Q - P;\n"
    "    float vv = dot(v, v), vn = dot(v, N);\n"
    "    float q2 = vv / (rad * rad), fall = saturate(1.0 - q2 * q2);\n"
    /* a tap's height over the surface counts from the depth's step: nearer than that is the buffer's own
     * rounding, not a thing in the way */
    "    sum += fall * max((vn - zq) * rsqrt(vv + 1e-6) - u.ao.z, 0.0);\n"
    "  }\n"
    /* none on surfaces seen edge-on: there the normal from depth is unreliable */
    "  float facing = smoothstep(0.1, 0.4, dot(N, -P) / dist);\n"
    "  float occ = 3.0 * facing * sum / float(NS);\n"
    /* ao_chars: a character's pixel (the stencil mask its draws wrote, input 11: fx_cmask) takes its own share of the occlusion
     * here, before the blur and the temporal pass: a pixel that changes hands between frames is averaged, not a flicker */
    "  if (u.aoc.w > 0.0 && texel(11, int2(px)).r > 0.5) occ *= u.aoc.x;\n"
    "  return float4(saturate(1.0 - occ), dist, mp, sh);\n"
    "}\n"
    /* the occlusion (x) and the sun by map and by contact (y, z) at a scene pixel from the four nearest
     * occlusion texels (input k), each weighted by how near its distance is to this pixel's: an edge's
     * occlusion stays on its own side */
    "float3 ao_at(uint k, float2 uv, float dist) {\n"
    "  if (dist <= 0.0) return float3(1.0, 1.0, 1.0);\n"
    "  float2 g = uv * u.size.zw - 0.5, f = frac(g);\n"
    "  int2 i0 = int2(floor(g)), hi = int2(u.size.zw) - 1;\n"
    "  float3 s = float3(0.0, 0.0, 0.0);\n"
    "  float w = 0.0;\n"
    "  [unroll] for (int n = 0; n < 4; ++n) {\n"
    "    int2 o = int2(n & 1, n >> 1);\n"
    "    float4 t = texel(k, clamp(i0 + o, int2(0, 0), hi));\n"
    "    float bw = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);\n"
    "    float dw = t.y > 0.0 ? 1.0 / (1e-3 + abs(t.y - dist) / dist) : 1e-3;\n"
    "    s += t.xzw * bw * dw; w += bw * dw;\n"
    "  }\n"
    "  return w > 0.0 ? s / w : float3(1.0, 1.0, 1.0);\n"
    "}\n"
    /* the occlusion over four pixels, centered (the ends at half weight: one period of the 4x4
     * pattern); the shadows only over their neighbours (their softness is the maps' own, and the
     * temporal pass evens their noise) */
    /* ao_soft_chars: on a character the occlusion blurred over aoc2.x world units each way, in 13 taps that far apart (at most 8
     * pixels), on its own surface (3% of the distance in depth) and over character pixels only (the stencil's mask, input 11: a
     * tap on the ground by its feet isn't taken). A low-polygon model's facets shade each other in flat patches the game's
     * smooth lighting hides. Run after the temporal pass (u.aoc2.y 1, fx_blur's second use): taps this far apart on the
     * occlusion as the passes before left it, a few taps' worth each frame in a 4x4 pattern, beat with the pattern in bands */
    "float ao_soft(int2 p, int2 hi, int2 dir, float4 c, float2 uv) {\n"
    "  if (u.aoc.w <= 0.0 || u.aoc2.x <= 0.0 || texel(11, int2(floor(u.vp.xy + uv * u.vp.zw))).r <= 0.5) return c.x;\n"
    "  float st = clamp(u.aoc2.x * u.proj.y * 0.5 * u.size.w / c.y / 6.0, 0.5, 8.0);\n"
    "  float ws = 0.0, wt = 0.0;\n"
    "  [unroll] for (int m = -6; m <= 6; ++m) {\n"
    "    int2 tp = clamp(p + int2(round(float2(dir) * (float(m) * st))), int2(0, 0), hi);\n"
    "    float4 t = texel(0, tp);\n"
    "    float k = exp(-0.08 * float(m * m)) * saturate(1.0 - abs(t.y - c.y) / (0.03 * c.y));\n"
    "    if (t.y > 0.0 && texel(11, int2(floor(u.vp.xy + ((float2(tp) + 0.5) / u.size.zw) * u.vp.zw))).r > 0.5) { ws += t.x * k; wt += k; }\n"
    "  }\n"
    "  return wt > 0.0 ? ws / wt : c.x;\n"
    "}\n"
    "float4 fx_blur(FO i) : SV_Target {\n"
    "  int2 p = int2(i.pos.xy), hi = int2(u.size.zw) - 1, dir = fdir();\n"
    "  float4 c = texel(0, p);\n"
    "  if (c.y <= 0.0) return c;\n"
    "  if (u.aoc2.y > 0.0) return float4(ao_soft(p, hi, dir, c, i.uv), c.y, c.zw);\n"
    "  float s = c.x, w = 1.0, sw = 1.0;\n"
    "  float2 ss = c.zw;\n"
    "  [unroll] for (int n = -2; n <= 2; ++n) {\n"
    "    if (n == 0) continue;\n"
    "    float4 t = texel(0, clamp(p + dir * n, int2(0, 0), hi));\n"
    "    float k = (abs(n) == 2 ? 0.5 : 1.0) * saturate(1.0 - abs(t.y - c.y) / (0.03 * c.y));\n"
    "    s += t.x * k; w += k;\n"
    "    if (abs(n) == 1 && u.aoq.y == 0.0) { ss += t.zw * (0.5 * k); sw += 0.5 * k; } /* hard edges: kept */\n"
    "  }\n"
    "  float4 o = float4(s / w, c.y, ss / sw);\n"
    /* sun_soft_chars: on a character the shadows (by the map and by contact) blurred over smapn2.w world units each way, in
     * 13 taps that far apart, on its own surface (3% of the distance in depth). The map's test stays sharp (a wider filter
     * there takes a wider bias with it, and the characters' own shadows - an arm's on the chest, the hair's on the face, a
     * few cm from what casts them - went) */
    "  if (u.smapn2.z > 0.0 && sun_char(u.vp.xy + i.uv * u.vp.zw, float3(0.0, 0.0, c.y * u.hand.x), c.y) > 0.0) {\n"
    "    float st = max(u.smapn2.w * u.proj.y * 0.5 * u.size.w / c.y / 6.0, 0.5);\n"
    "    float2 cs = 0.0;\n"
    "    float cw = 0.0;\n"
    "    [unroll] for (int m = -6; m <= 6; ++m) {\n"
    "      float4 t = texel(0, clamp(p + int2(round(float2(dir) * (float(m) * st))), int2(0, 0), hi));\n"
    "      float k = exp(-0.08 * float(m * m)) * saturate(1.0 - abs(t.y - c.y) / (0.03 * c.y));\n"
    "      if (t.y > 0.0) { cs += t.zw * k; cw += k; }\n"
    "    }\n"
    "    if (cw > 0.0) o.zw = cs / cw;\n"
    "  } else if (u.aoc.z > 0.0) {\n"
    /* sun_soft_zone: the same on the zone, so a low-polygon surface's shadows - the map's polygon-edged terminators and cast
     * shadows, the lit and shaded bands of a face turned from the light - come out as a smooth ramp, not a hard step at an edge */
    /* ... but not on level ground: a character's, a log's or a tree's shadow there is a real shape and stays as sharp as the map
     * draws it. The ground's slope from the distances three texels along each way (the same surface: within 6% of this
     * one's), as the share of the way to flat between 0.85 and 0.97 of straight up; nearer than 30 units only (a depth that
     * far is too coarse for a slope), where a face is big enough on the screen to show */
    "    float2 ppt = u.vp.zw / u.size.zw;\n"
    "    float2 sp = u.vp.xy + i.uv * u.vp.zw;\n"
    "    float4 ta = texel(0, clamp(p + int2(3, 0), int2(0, 0), hi)), tb = texel(0, clamp(p + int2(0, 3), int2(0, 0), hi));\n"
    "    float lvl = 0.0;\n"
    "    if (ta.y > 0.0 && tb.y > 0.0 && abs(ta.y - c.y) < 0.06 * c.y && abs(tb.y - c.y) < 0.06 * c.y) {\n"
    "      float3 P0 = view_pos(sp, c.y * u.hand.x);\n"
    "      float3 nn = cross(view_pos(sp + float2(3.0 * ppt.x, 0.0), ta.y * u.hand.x) - P0, view_pos(sp + float2(0.0, 3.0 * ppt.y), tb.y * u.hand.x) - P0);\n"
    "      float l2 = dot(nn, nn);\n"
    "      if (l2 > 1e-12) lvl = smoothstep(0.85, 0.97, abs(dot(nn * rsqrt(l2), u.up.xyz)));\n"
    "    }\n"
    "    lvl = max(lvl, smoothstep(30.0, 45.0, c.y));\n"
    "    if (lvl < 0.99) {\n"
    "      float st = max(u.aoc.z * u.proj.y * 0.5 * u.size.w / c.y / 6.0, 0.5);\n"
    "      float2 zs = 0.0;\n"
    "      float zw = 0.0;\n"
    "      [unroll] for (int m = -6; m <= 6; ++m) {\n"
    "        float4 t = texel(0, clamp(p + int2(round(float2(dir) * (float(m) * st))), int2(0, 0), hi));\n"
    "        float k = exp(-0.08 * float(m * m)) * saturate(1.0 - abs(t.y - c.y) / (0.03 * c.y));\n"
    "        if (t.y > 0.0) { zs += t.zw * k; zw += k; }\n"
    "      }\n"
    "      if (zw > 0.0) o.zw = lerp(zs / zw, o.zw, lvl);\n"
    "    }\n"
    "  }\n"
    "  return o;\n"
    "}\n"
    /* the occlusion and shadows over time: each texel's world point found in the frame before (its
     * camera), and that frame's result (input 1) blended in where its distance agrees (no smearing
     * across what moved or came into view). The sampling patterns turn every frame, so this averages
     * them into a result that holds still as the camera moves. The frame before's result is held to
     * what this frame has around the texel on the same surface (the patterns' spread): a shadow that
     * moves over still ground - a character running - comes in whole and leaves no trail, where the
     * blend lagged it (faint, smeared). */
    "float4 fx_temporal(FO i) : SV_Target {\n"
    "  float4 c = texel(0, int2(i.pos.xy));\n"
    "  if (c.y <= 0.0 || u.hist.x == 0.0) return c;\n"
    "  float2 px = u.vp.xy + i.uv * u.vp.zw;\n"
    "  float3 P = view_pos(px, c.y * u.hand.x);\n"
    "  float4 pc = mul(u.reproj, float4(P, 1.0));\n"
    "  if (pc.w <= 1e-4) return c;\n"
    "  float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(puv < 0.0) || any(puv > 1.0)) return c;\n"
    "  float4 h = tap(1, puv);\n"
    "  if (!(h.y > 0.0) || abs(h.y - pc.w) > 0.04 * pc.w) return c;\n"
    "  int2 p = int2(i.pos.xy), hi = int2(u.size.zw) - 1;\n"
    "  float3 lo = c.xzw, up = c.xzw;\n"
    "  float same = 0.0;\n"
    "  [unroll] for (int dy = -1; dy <= 1; ++dy)\n"
    "    [unroll] for (int dx = -1; dx <= 1; ++dx) {\n"
    "      float4 t = texel(0, clamp(p + int2(dx, dy), int2(0, 0), hi));\n"
    "      if (t.y > 0.0 && abs(t.y - c.y) < 0.05 * c.y) { lo = min(lo, t.xzw); up = max(up, t.xzw); same += 1.0; }\n"
    "    }\n"
    /* the occlusion's few taps a frame differ more from frame to frame than the shadows' lookups: held
     * more loosely (tightly, it shimmered), still too near this frame's to trail a character running.
     * Where few of the 3x3 are on this surface (a character's thin parts and silhouette: a strap, a sword, a
     * leg's edge) the range above is the frame's own pixel or two, the history was held to that and the
     * frame's noise went straight through, a new pattern every frame (the character flickered while it
     * stood still): the range is widened by how few agree (x5 on a lone pixel, x1 on a whole surface). What
     * moved onto a pixel has another distance, and the check above gives the frame's own */
    "  const float3 give = float3(0.06, 0.02, 0.02) * (1.0 + 4.0 * (9.0 - same) / 8.0);\n"
    "  float3 m = lerp(c.xzw, clamp(h.xzw, lo - give, up + give), u.hist.z);\n"
    "  return float4(m.x, c.y, m.y, m.z);\n"
    "}\n"
    /* VanaCore's glow (gfx_glow): the character's own picture blurred - a 9-tap gaussian across, reading its
     * chain's level glow2.x and premultiplying, then down - and laid over the scene in the tint (the character
     * is drawn over it after) */
    "static const float GLOW_W[5] = { 0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162 };\n"
    "float4 fx_glowb(FO i) : SV_Target {\n"
    "  float lv = u.glow2.x;\n"
    "  float2 tx = float2(fdir()) * u.glow2.y / max(floor(size_of(0) / exp2(lv)), 1.0);\n"
    "  float4 c = float4(0.0, 0.0, 0.0, 0.0);\n"
    "  [unroll] for (int k = -4; k <= 4; ++k) {\n"
    "    float4 t = tapl(0, i.uv + tx * float(k), lv);\n"
    "    if (u.glow2.z > 0.0) t.rgb *= t.a;\n"
    "    c += t * GLOW_W[abs(k)];\n"
    "  }\n"
    "  return c;\n"
    "}\n"
    "float4 fx_glowc(FO i) : SV_Target {\n"
    "  float4 g = tap(0, i.uv);\n"
    "  float4 c = float4(saturate(g.rgb / max(g.a, 1e-4) * u.glow.rgb), saturate(g.a * u.glow.w));\n"
    "  return u.glow2.w > 0.0 ? float4(c.rgb * c.a, c.a) : c;\n" /* premultiplied: into Direct3D 11's layer of the frame's glows */
    "}\n"
    /* ... and again over VanaCore's grass, drawn over it (gfx_glow_over_grass): the frame's glows as they were laid, premultiplied
     * (input 0, gfx_glow's layer), only where the grass covers the scene: its depth (input 1) isn't the depth before it (input 2) */
    "float4 fx_glowg(FO i) : SV_Target {\n"
    "  int2 px = int2(i.pos.xy);\n"
    "  if (texel(1, px).r == texel(2, px).r) discard;\n"
    "  return tap(0, i.uv);\n"
    "}\n"
    /* bloom's source: the scene at a quarter size (four bilinear taps), what is over the threshold,
     * with a soft knee */
    "float4 fx_bright(FO i) : SV_Target {\n"
    "  float2 uv = (u.vp.xy + i.uv * u.vp.zw) / u.size.xy, t = 1.0 / u.size.xy;\n"
    "  float3 c = 0.25 * (tap(0, uv + t * float2(-1, -1)).rgb + tap(0, uv + t * float2(1, -1)).rgb +\n"
    "                     tap(0, uv + t * float2(-1, 1)).rgb + tap(0, uv + t * float2(1, 1)).rgb);\n"
    /* shaded as fx_comp will shade it (input 1, the occlusion and the sun's shadows, when there are any): the scene is copied
     * before them, and a character in a cliff's shadow, drawn dark, still glowed as bright as in the open (from XI on
     * Anything, rubymatrix/xi-on-anything 6e26f8b) */
    "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) {\n"
    "    float4 os = tap(1, i.uv);\n"
    "    if (os.y > 0.0) c *= lerp(1.0, os.x, u.ao.y) * lerp(1.0, os.z, u.smap.x) * lerp(1.0, os.w, u.shadow.x);\n"
    "  }\n"
    "  float l = max(c.r, max(c.g, c.b)), k = u.bloom.z;\n"
    "  float soft = clamp(l - u.bloom.x + k, 0.0, 2.0 * k);\n"
    "  soft = soft * soft / (4.0 * k + 1e-5);\n"
    "  return float4(c * (max(soft, l - u.bloom.x) / max(l, 1e-5)), 1.0);\n"
    "}\n"
    /* half the size of the source: four bilinear taps */
    "float4 fx_down(FO i) : SV_Target {\n"
    "  float2 tx = 1.0 / size_of(0);\n"
    "  return 0.25 * (tap(0, i.uv + tx * float2(-1, -1)) + tap(0, i.uv + tx * float2(1, -1)) +\n"
    "                 tap(0, i.uv + tx * float2(-1, 1)) + tap(0, i.uv + tx * float2(1, 1)));\n"
    "}\n"
    /* a 9-tap gaussian in five bilinear taps, dir texels apart */
    "float4 fx_gauss(FO i) : SV_Target {\n"
    "  float2 tx = float2(fdir()) / size_of(0);\n"
    "  float4 c = tap(0, i.uv) * 0.2270270;\n"
    "  c += (tap(0, i.uv + tx * 1.3846154) + tap(0, i.uv - tx * 1.3846154)) * 0.3162162;\n"
    "  c += (tap(0, i.uv + tx * 3.2307692) + tap(0, i.uv - tx * 3.2307692)) * 0.0702703;\n"
    "  return c;\n"
    "}\n"
    /* what the god rays start from: the sky's bright pixels, more of them nearer the sun; inputs the
     * scene and its depth. (VanaCore: FFXI's skies are dim - grey cloud about a third bright - so the
     * sky counts from 15% (full at 60%, upstream's 35-90% left them nearly black), out to a screen's
     * height from the sun) */
    "float4 fx_raymask(FO i) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + i.uv * u.vp.zw) + 0.5;\n"
    "  if (view_z(texel(1, int2(px)).r) != 0.0) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float3 c = tap(0, px / u.size.xy).rgb;\n"
    "  float2 d = (i.uv - u.sunuv.xy) * float2(u.proj.y / u.proj.x, 1.0);\n"
    "  float glow = saturate(1.0 - length(d) / 1.0);\n"
    "  return float4(c * smoothstep(0.15, 0.6, dot(c, LUMA)) * glow * glow, 1.0);\n"
    "}\n"
    /* the mask gathered along the line toward the sun, fading with each step */
    "float4 fx_rays(FO i) : SV_Target {\n"
    "  const int NS = 64;\n"
    "  float2 uv = i.uv, st = (i.uv - u.sunuv.xy) * (u.rays.z / float(NS));\n"
    "  float3 acc = float3(0.0, 0.0, 0.0);\n"
    "  float w = 1.0;\n"
    "  [loop] for (int n = 0; n < NS; ++n) { acc += tap(0, uv).rgb * w; w *= u.rays.y; uv -= st; }\n"
    "  return float4(acc * (4.0 / float(NS)), 1.0);\n"
    "}\n"
    "float3 screen(float3 a, float3 b) { return 1.0 - (1.0 - saturate(a)) * (1.0 - saturate(b)); }\n"
    /* the cel look (VanaCore). The surface normal at px from its neighbors on the side nearer in
     * depth, as fx_ao; w = 0 where they give none (the viewport's edge). Depth is input k. */
    "float4 normal_depth(uint k, float2 px, float3 P) {\n"
    "  float3 r = pos_at(k, px + float2(1, 0)) - P, l = P - pos_at(k, px - float2(1, 0));\n"
    "  float3 dn = pos_at(k, px + float2(0, 1)) - P, up = P - pos_at(k, px - float2(0, 1));\n"
    "  float3 dx = abs(r.z) < abs(l.z) ? r : l, dy = abs(dn.z) < abs(up.z) ? dn : up;\n"
    "  float3 N = cross(dx, dy);\n"
    "  float nn = dot(N, N);\n"
    "  if (!(nn > 1e-20)) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  N *= rsqrt(nn);\n"
    "  return float4(dot(N, P) > 0.0 ? -N : N, 1.0);\n"
    "}\n"
    /* ... the normal-mapped draw's where it made one (the effects that follow normal maps), else that from the depth */
    "float4 normal_at(uint k, float2 px, float3 P) {\n"
    "  float4 nd = normal_det(u.clar.w, px, P);\n"
    "  return nd.w > 0.0 ? nd : normal_depth(k, px, P);\n"
    "}\n"
    "float4 normal_at_r(uint k, float2 px, float3 P) {\n"
    "  float4 nd = normal_det(u.clar.z, px, P);\n"
    "  return nd.w > 0.0 ? nd : normal_depth(k, px, P);\n"
    "}\n"
    /* 1 / distance at px, 0 for the sky */
    "float inv_dist(uint k, float2 px) {\n"
    "  float d = pos_at(k, px).z * u.hand.x;\n"
    "  return d > 0.0 ? 1.0 / d : 0.0;\n"
    "}\n"
    /* how much of an ink line the pixel at px (P, dist from the camera) is, 0..1; depth is input k (a literal: Direct3D 11's
     * inputs fold to one texture); chr, the pixel is a character's (char_line) */
    "float outline_at(uint k, float2 px, float3 P, float dist, float lw, bool chr) {\n"
    "  float2 ax = float2(lw, 0.0), ay = float2(0.0, lw);\n"
    "  float izc = 1.0 / dist;\n"
    /* a silhouette: nearer than its neighbors across it on average, by more than a plane would be
     * (1 / distance is linear over a plane on screen, so its second difference is 0 there); only
     * the near side, so the line is on the object and not the wall behind it */
    "  float sil = max(-(inv_dist(k, px - ax) + inv_dist(k, px + ax) - 2.0 * izc),\n"
    "                  -(inv_dist(k, px - ay) + inv_dist(k, px + ay) - 2.0 * izc)) / izc;\n"
    "  float e = smoothstep(u.toon.z, 2.0 * u.toon.z, sil);\n"
    /* a crease: the surface turns by more than the threshold between this pixel and a neighbor on
     * the same surface (one across a silhouette doesn't count) */
    "  if (u.toon.w > 0.0 && e < 1.0) {\n"
    "    float4 nc = normal_at(k, px, P);\n"
    "    float cr = 0.0;\n"
    "    [unroll] for (int n = 0; n < 4; ++n) {\n"
    "      float2 q = px + (n == 0 ? ax : n == 1 ? -ax : n == 2 ? ay : -ay);\n"
    "      float3 Q = pos_at(k, q);\n"
    "      float dq = Q.z * u.hand.x;\n"
    "      if (nc.w == 0.0 || dq <= 0.0 || abs(dq - dist) > 0.1 * dist) continue;\n"
    "      float4 nq = normal_at(k, q, Q);\n"
    "      if (nq.w <= 0.0) continue;\n"
    "      float c1 = 1.0 - dot(nc.xyz, nq.xyz);\n"
    /* the world's fold against a character (the ground round a foot) isn't the world's line to draw at its width: the
     * character's own side draws the contact at theirs (outline_width_chars; char_line only where the fold would show) */
    "      if (c1 > u.toon.w && c1 > cr && !chr && u.toon3.w > 0.0 && char_line(q, Q, dq)) continue;\n"
    "      cr = max(cr, c1);\n"
    "    }\n"
    "    e = max(e, smoothstep(u.toon.w, 1.5 * u.toon.w, cr));\n"
    "  }\n"
    /* gone by the fade distance, so far scenery doesn't turn to scribble */
    "  return e * (1.0 - smoothstep(0.5 * u.toon2.x, u.toon2.x, dist));\n"
    "}\n"
    /* brightness in toon2.y steps between black and white (hue and saturation kept), each step's edge
     * softened over toon2.z of a step; black stays black and white white */
    "float3 band(float3 c) {\n"
    "  float L = dot(c, LUMA), n = u.toon2.y, w = max(0.5 * u.toon2.z, 1e-3);\n"
    "  float t = L * n - 0.5;\n"
    "  float q = (floor(t) + smoothstep(0.5 - w, 0.5 + w, frac(t)) + 0.5) / n;\n"
    "  return c * min(q / max(L, 1e-4), 4.0);\n"
    "}\n"
    /* the scene with its texture detail flattened and its edges kept (a Kuwahara filter): each pixel
     * takes the mean of whichever of the four squares around it is most even; input 0 the scene */
    "float4 fx_smooth(FO i) : SV_Target {\n"
    "  int2 p = int2(i.pos.xy), hi = int2(u.size.xy) - 1;\n"
    "  int r = int(u.toon3.y), st = max(1, r / 2);\n"
    "  float4 c0 = texel(0, p);\n"
    "  float3 best = c0.rgb;\n"
    "  float bv = 1e30;\n"
    "  [unroll] for (int q = 0; q < 4; ++q) {\n"
    "    int2 sg = int2((q & 1) != 0 ? 1 : -1, (q & 2) != 0 ? 1 : -1);\n"
    "    float3 m = float3(0.0, 0.0, 0.0);\n"
    "    float s = 0.0, s2 = 0.0, n = 0.0;\n"
    "    [loop] for (int y = 0; y <= r; y += st)\n"
    "      [loop] for (int x = 0; x <= r; x += st) {\n"
    "        float3 c = texel(0, clamp(p + sg * int2(x, y), int2(0, 0), hi)).rgb;\n"
    "        float l = dot(c, LUMA);\n"
    "        m += c; s += l; s2 += l * l; n += 1.0;\n"
    "      }\n"
    "    m /= n; s /= n;\n"
    "    float v = s2 / n - s * s;\n"
    "    if (v < bv) { bv = v; best = m; }\n"
    "  }\n"
    "  return float4(lerp(c0.rgb, best, u.toon3.x), c0.a);\n"
    "}\n"
    /* inputs: the scene's copy, the occlusion, the depth, bloom at a quarter and an eighth, the rays,
     * and the scene to start from (the copy, or it smoothed for the cel look) */
    /* VanaCore's zone lines: each a box (u.zl), in one of four styles (zlp.x): 0 a curtain over what's behind it,
     * the more of it the ray through the pixel crosses the stronger, strongest at its middle (where the ground
     * is, about) and below, fading to its top; 1 a line floating across it zlc.z above its middle, at least a
     * pixel and a half wide; 2 that line in dots half a unit apart, a pulse running along them; 3 sparkles
     * drifting up its middle from that height, twinkling; lines, dots and sparkles zlc.w times their size. With glow (zlp.y) a
     * halo round each; all fading out with distance. Each is hidden behind what the scene has nearer (its
     * depth); a dot, a sparkle or a stretch of line whose own place, and a unit above it, are behind the scene isn't
     * drawn at all, nor its halo (the box runs on into the walls either side of a passage). With zlp.w, they fade out over that
     * many units toward where they stop: the box's ends, or the scene (zl_ends: four steps of it looked at each
     * way along the box's width; a step counts only where the scene is just in front of it, zl_stopped: something
     * nearer the eye that hides a stretch of open air doesn't end the line) */
    "float zl_hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }\n"
    "bool zl_seen(float3 V) {\n"
    "  float rd = V.z * u.hand.x;\n"
    "  if (rd <= 0.0) return false;\n"
    "  float2 ndc = float2(V.x * u.proj.x + V.z * u.proj.z, V.y * u.proj.y + V.z * u.proj.w) / rd;\n"
    "  float2 q = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "  if (any(q < u.vp.xy) || any(q >= u.vp.xy + u.vp.zw)) return true;\n"
    "  float sd = view_z(texel(2, int2(q)).r) * u.hand.x;\n"
    "  return sd <= 0.0 || rd < sd + max(0.1, 0.01 * rd);\n"
    "}\n"
    /* a point the scene has just in front of it (a wall the line runs into), not one a nearer thing hides: a rail
     * or a post between the eye and a stretch of open air is no end of the line. margin: how far in front of the
     * point the scene may be and still count (a line running into a wall seen at a slant has its face that much
     * nearer along the ray) */
    "bool zl_stopped(float3 V, float margin) {\n"
    "  float rd = V.z * u.hand.x;\n"
    "  if (rd <= 0.0) return false;\n"
    "  float2 ndc = float2(V.x * u.proj.x + V.z * u.proj.z, V.y * u.proj.y + V.z * u.proj.w) / rd;\n"
    "  float2 q = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "  if (any(q < u.vp.xy) || any(q >= u.vp.xy + u.vp.zw)) return false;\n"
    "  float sd = view_z(texel(2, int2(q)).r) * u.hand.x;\n"
    "  return sd > 0.0 && rd >= sd + max(0.1, 0.01 * rd) && rd - sd < margin;\n"
    "}\n"
    "float zl_ends(float3 P, float3 X, float s, float hx) {\n"
    "  float fade = u.zlp.w;\n"
    "  if (fade <= 0.0) return 1.0;\n"
    "  float room = hx - abs(s), st = fade / 4.0;\n"
    "  [loop] for (int k = 1; k <= 4; ++k) {\n"
    "    float o = st * k, m = 0.5 + 1.5 * o;\n"
    "    if (o < room && (zl_stopped(P + X * o, m) || zl_stopped(P - X * o, m))) room = o - 0.5 * st;\n"
    "  }\n"
    "  return saturate(room / fade);\n"
    "}\n"
    /* a point in a wall, not one a ledge or a step in front hides: the scene hides it and a unit above it too (seen from
     * below a stair's top, a dot just behind it is half shown, and its pixels are cut one by one) */
    "bool zl_shown(float3 P) { return zl_seen(P) || zl_seen(P + u.up.xyz); }\n"
    /* how far the ray through the pixel passes from P (1e9: P behind the eye, or in a wall); k: how much of it the scene
     * at the pixel leaves, from how far behind P the scene is there (half where it's level with it, faded over a stretch
     * growing with distance: a hard cut flickers row by row where the line runs just over the ground seen at a slant) */
    "float zl_point(float3 D, float a, float3 P, float tmax, out float t, out float k) {\n"
    "  t = dot(P, D) / a;\n"
    "  k = 0.0;\n"
    "  if (t <= 0.0 || !zl_shown(P)) return 1e9;\n"
    "  float len = sqrt(a), soft = max(0.25, 0.015 * t * len);\n"
    "  k = saturate(0.5 + (tmax - t) * len / soft);\n"
    "  return k > 0.0 ? length(D * t - P) : 1e9;\n"
    "}\n"
    /* where along the line (P0, X, half-length hx) it passes nearest the ray */
    "float zl_s(float3 D, float a, float3 P0, float3 X, float hx) {\n"
    "  float b = dot(D, X), d = -dot(D, P0), e = -dot(X, P0), den = a - b * b;\n"
    "  return clamp(abs(den) > 1e-6 ? (a * e - b * d) / den : 0.0, -hx, hx);\n"
    "}\n"
    /* w: how far the colour has been taken to the lines' own (the fill's weight, or 0): the late pass (fx_zlate) lays the
     * result over the scene by it */
    "float3 zone_lines_w(float3 c, float2 px, float z, out float w) {\n"
    "  float3 D = view_pos(px, u.hand.x);\n"
    "  float a2 = dot(D, D), len = sqrt(a2), tmax = z != 0.0 ? z * u.hand.x : 1e9;\n"
    "  float pw = 2.0 / (u.proj.y * u.vp.w);\n"
    "  int style = int(u.zlp.x);\n"
    "  bool glow = u.zlp.y > 0.0;\n"
    "  float tm = u.zlp.z, fill = 0.0, emit = 0.0, hot = 0.0, sz = u.zlc.w > 0.0 ? u.zlc.w : 1.0;\n"
    "  [loop] for (int b = 0; b < int(u.zlc.x); ++b) {\n"
    "    float3 C = u.zl[b * 4].xyz;\n"
    "    float4 ax = u.zl[b * 4 + 1], az = u.zl[b * 4 + 3];\n"
    "    float h = max(u.zl[b * 4 + 2].w, 0.01);\n"
    "    float dn = dot(D, az.xyz), tp = abs(dn) > 1e-4 ? dot(C, az.xyz) / dn : -1.0;\n"
    "    float2 q = float2(0.0, 0.0);\n"
    "    bool plane = tp > 0.0 && tp < tmax;\n"
    "    if (plane) { float3 Q = D * tp - C; q = float2(dot(Q, ax.xyz), dot(Q, u.up.xyz)); }\n"
    "    float far_ = saturate((u.zlc.y - (plane ? tp : tmax) * len) / (0.25 * u.zlc.y));\n"
    /* the dots lying on the floor inside the box (style 4): wherever the box's middle is, these are on what the
     * scene shows of the ground there. A pixel whose surface is within the box's width and height, nearly flat and
     * facing up (from the depth's neighbors, as the cel look's), lights by its distance to the nearest dot along
     * the box's middle, its radius grown where the surface is seen at a slant (a pixel then covers more of it). */
    "    if (style == 4) {\n"
    "      if (z == 0.0) continue;\n"
    "      float3 S = view_pos(px, z), Q = S - C;\n"
    "      float sw = dot(Q, ax.xyz), dd = dot(Q, az.xyz);\n"
    "      if (abs(sw) > ax.w + 0.5 || abs(dd) > 2.0 || abs(dot(Q, u.up.xyz)) > h + 0.5) continue;\n"
    "      float4 nd = normal_depth(2, px, S);\n"
    "      if (nd.w <= 0.0 || dot(nd.xyz, u.up.xyz) < 0.6) continue;\n"
    /* a pixel covers 1 / facing times as much of a surface seen at a slant, along the way it slopes from the eye:
     * a dot is longer that way by as much (its distance there counted as facing times as far), as wide as ever across */
    "      float dist = length(S), facing = max(abs(dot(nd.xyz, S)) / dist, 0.35);\n"
    "      float3 tv = S / dist, tw = tv - nd.xyz * dot(tv, nd.xyz);\n"
    "      float tl = length(tw);\n"
    "      tw = tl > 1e-4 ? tw / tl : ax.xyz;\n"
    "      float r = sz * max(0.1, 2.0 * pw * dist);\n"
    "      float L = 0.5 * clamp(sz, 0.5, 4.0), best = 0.0, core_best = 0.0;\n"
    "      [loop] for (int k = -1; k <= 1; ++k) {\n"
    "        float sd = clamp((round(sw / L) + k) * L, -ax.w, ax.w);\n"
    "        float3 off = ax.xyz * (sw - sd) + az.xyz * dd;\n"
    "        float along = dot(off, tw);\n"
    "        float dd2 = length(float2(length(off - tw * along), along * facing));\n"
    "        float core = exp(-dd2 * dd2 / (r * r));\n"
    "        float halo = glow ? 0.4 * exp(-dd2 / (3.0 * r)) : 0.0;\n"
    "        if (core + halo < 0.002) continue;\n"
    "        float ends = zl_ends(S - ax.xyz * (sw - sd) - az.xyz * dd + u.up.xyz * 0.3, ax.xyz, sd, ax.w);\n"
    "        float pulse = 0.7 + 0.3 * sin(tm * 3.0 - sd * 1.5);\n"
    "        best = max(best, (core + halo) * pulse * ends);\n"
    "        core_best = max(core_best, core * pulse * ends);\n"
    "      }\n"
    "      far_ = saturate((u.zlc.y - dist) / (0.25 * u.zlc.y));\n"
    "      emit = max(emit, best * far_);\n"
    "      hot = max(hot, core_best * far_);\n"
    "    } else if (style == 1 || style == 2) {\n"
    "      float3 P0 = C + u.up.xyz * (u.zl[b * 4].w + u.zlc.z);\n"
    "      float s = zl_s(D, a2, P0, ax.xyz, ax.w);\n"
    "      if (style == 1) {\n"
    "        float t, vis;\n"
    "        float d = zl_point(D, a2, P0 + ax.xyz * s, tmax, t, vis);\n"
    "        if (d > 1e8) continue;\n"
    "        float dist = t * len;\n"
    "        far_ = saturate((u.zlc.y - dist) / (0.25 * u.zlc.y));\n"
    "        float w = sz * max(0.04, 1.5 * pw * dist);\n"
    "        float core = 1.0 - smoothstep(0.5 * w, w, d);\n"
    "        float halo = glow ? 0.6 * exp(-d / max(0.3, 4.0 * pw * dist)) : 0.0;\n"
    "        if (core + halo < 0.002) continue;\n"
    "        float ends = zl_ends(P0 + ax.xyz * s, ax.xyz, s, ax.w) * vis;\n"
    "        emit = max(emit, (core + halo) * ends * far_);\n"
    "        hot = max(hot, core * ends * far_);\n"
    "      } else {\n"
    /* each dot is seen or hidden by its own place (the line's nearest point to the ray hidden says nothing of the dots
     * either side of it, and dropped whole columns of them behind a stair's top) */
    "        float L = 0.5 * clamp(sz, 0.5, 4.0);\n"
    "        float best = 0.0, core_best = 0.0;\n"
    "        [loop] for (int k = -1; k <= 1; ++k) {\n"
    "          float sd = clamp((round(s / L) + k) * L, -ax.w, ax.w), td, vis;\n"
    "          float dd = zl_point(D, a2, P0 + ax.xyz * sd, tmax, td, vis);\n"
    "          if (dd > 1e8) continue;\n"
    "          float r = sz * max(0.1, 2.0 * pw * td * len);\n"
    "          float pulse = 0.7 + 0.3 * sin(tm * 3.0 - sd * 1.5);\n"
    "          float core = exp(-dd * dd / (r * r));\n"
    "          float halo = glow ? 0.4 * exp(-dd / (3.0 * r)) : 0.0;\n"
    "          if (core + halo < 0.002) continue;\n"
    "          float ends = zl_ends(P0 + ax.xyz * sd, ax.xyz, sd, ax.w) * vis;\n"
    "          float ff = saturate((u.zlc.y - td * len) / (0.25 * u.zlc.y));\n"
    "          best = max(best, (core + halo) * pulse * ends * ff);\n"
    "          core_best = max(core_best, core * pulse * ends * ff);\n"
    "        }\n"
    "        emit = max(emit, best);\n"
    "        hot = max(hot, core_best);\n"
    "      }\n"
    "    } else if (style == 3) {\n"
    "      float base = u.zl[b * 4].w + u.zlc.z;\n"
    "      if (!plane || abs(q.x) > ax.w + 1.0 || q.y - base > h + 1.0 || q.y - base < -1.0) continue;\n"
    "      float dist = tp * len, r = sz * max(0.05, 1.2 * pw * dist);\n"
    "      const float S = 1.25;\n"
    "      float2 cell = floor(q / S);\n"
    "      float best = 0.0, core_best = 0.0;\n"
    "      [loop] for (int j = -1; j <= 1; ++j)\n"
    "        [loop] for (int i = -1; i <= 1; ++i) {\n"
    "          float2 id = cell + float2(i, j);\n"
    "          float h1 = zl_hash(id + float2(b * 17.0, 0.0)), h2 = zl_hash(id * 1.7 + float2(3.1, b * 5.0));\n"
    "          float ph = frac(h2 + tm * 0.12);\n"
    "          float2 at = (id + float2(h1, ph)) * S;\n"
    "          float2 dp = q - at;\n"
    "          float d = length(dp);\n"
    "          float tw = (0.6 + 0.4 * sin(tm * 3.0 + h1 * 40.0)) * sin(3.14159 * ph);\n"
    "          float core = exp(-d * d / (r * r));\n"
    "          float flare = exp(-abs(dp.x) / (0.35 * r)) * exp(-abs(dp.y) / (4.0 * r)) + exp(-abs(dp.y) / (0.35 * r)) * exp(-abs(dp.x) / (4.0 * r));\n"
    "          float halo = glow ? 0.35 * exp(-d / (3.0 * r)) : 0.0;\n"
    "          float inside = (1.0 - smoothstep(ax.w - S, ax.w, abs(at.x))) * saturate(1.0 - max(at.y - base, 0.0) / h) * step(-0.3, at.y - base);\n"
    "          if (inside <= 0.0 || !zl_shown(C + ax.xyz * at.x + u.up.xyz * at.y)) continue;\n"
    "          float lit = (core + 0.5 * flare + halo) * tw * inside;\n"
    "          if (lit < 0.002) continue;\n"
    "          float ends = zl_ends(C + ax.xyz * at.x + u.up.xyz * at.y, ax.xyz, at.x, ax.w);\n"
    "          best = max(best, lit * ends);\n"
    "          core_best = max(core_best, core * tw * inside * ends);\n"
    "        }\n"
    "      emit = max(emit, best * far_);\n"
    "      hot = max(hot, core_best * far_);\n"
    "    } else {\n"
    "      float t0 = 0.0, t1 = tmax;\n"
    "      [unroll] for (int k = 1; k < 4; ++k) {\n"
    "        float4 a = u.zl[b * 4 + k];\n"
    "        float o = -dot(C, a.xyz), d = dot(D, a.xyz);\n"
    "        if (abs(d) < 1e-6) { if (abs(o) > a.w) t1 = -1.0; }\n"
    "        else { float ta = (-a.w - o) / d, tb = (a.w - o) / d; t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb)); }\n"
    "      }\n"
    "      if (t1 > t0) {\n"
    "        float fade = saturate(1.0 - max(dot(D * t0 - C, u.up.xyz), 0.0) / h);\n"
    "        float ff = saturate((u.zlc.y - t0 * len) / (0.25 * u.zlc.y));\n"
    "        float k = (1.0 - exp(-0.5 * (t1 - t0) * len)) * fade * fade * ff;\n"
    "        if (u.zlp.w > 0.0 && k > 0.002) {\n"
    "          float3 E = D * t0;\n"
    "          float ex = dot(E - C, ax.xyz);\n"
    "          k *= zl_ends(E, ax.xyz, ex, ax.w);\n"
    "        }\n"
    "        fill = max(fill, k);\n"
    "        if (glow) emit = max(emit, 0.4 * k);\n"
    "      }\n"
    "      if (glow && plane) {\n"
    "        float dx = max(abs(q.x) - ax.w, 0.0), dy = max(q.y - h, 0.0);\n"
    "        float out_ = length(float2(dx, dy));\n"
    "        if (out_ > 0.0 && q.y > -0.5) {\n"
    "          float fade = saturate(1.0 - max(q.y, 0.0) / (h + 1.0));\n"
    "          emit = max(emit, 0.45 * exp(-out_ / 0.75) * fade * fade * far_);\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  w = saturate(fill * u.zlcol.a);\n"
    "  c = lerp(c, u.zlcol.rgb, w);\n"
    "  return c + (u.zlcol.rgb * saturate(emit) + 0.6 * saturate(hot)) * u.zlcol.a;\n"
    "}\n"
    "float3 zone_lines(float3 c, float2 px, float z) {\n"
    "  float w;\n"
    "  return zone_lines_w(c, px, z, w);\n"
    "}\n"
    /* VanaCore's sparkles (vanacore_gfx_set_sparkles), the light they add: at each place (u.sp), points of light rising out
     * of a disc on the ground. Each lives two to four seconds from a spot of the disc it picks anew each life: it fades in,
     * rises (quickest at first) drifting a little, and is gone before the top; it twinkles, and now and then flashes a
     * four-pointed glint. A pixel looks at them only where its ray crosses the column they rise in (the disc and a unit round
     * it); each is hidden by what the scene has nearer at that pixel. With spp.x, each is the colour of the scene (input 0)
     * where it was born, at its brightest (its hue): its place's colour where that spot is off the screen, behind something
     * nearer, or black. Their life and rise go by spc.y (their speed's time), the twinkle by spp.y.
     * A place of style 1 is an aura round a character standing there: flames, streaks of light rising from the floor on a ring
     * round them (drawing in a little as they go, gone by the top), and a glow round the character like the trusts' and
     * avatars' (gfx_glow): the characters' mask (cmask_pass) taken small and blurred (zl_run, input 1, spp.z 1), so it follows
     * the outline, kept to the pixels whose ray passes within about a yalm of the aura's axis (dax) and hidden over the
     * character itself and what's nearer (ka), breathing a little. With no mask, a halo hugging the column instead: solid out
     * to the body's width (0.45 R), a gaussian past it, fading over its top. Each place's size, amount
     * and strength are its own (sp[3p + 2]); a sparkle's size and amount also times spc.z and spc.w. */
    "float3 sparkles(float2 px, float z) {\n"
    "  float3 D = view_pos(px, u.hand.x);\n"
    "  float a2 = dot(D, D), len = sqrt(a2), tmax = z != 0.0 ? z * u.hand.x : 1e9;\n"
    "  float pw = 2.0 / (u.proj.y * u.vp.w), tm = u.spc.y, tt = u.spp.y;\n"
    "  float3 X = u.spx.xyz, Y = u.up.xyz, Z = u.spz.xyz, sum = float3(0.0, 0.0, 0.0);\n"
    "  [loop] for (int p = 0; p < int(u.spc.x); ++p) {\n"
    "    float3 C = u.sp[p * 3].xyz;\n"
    "    float4 ps = u.sp[p * 3 + 2];\n"
    "    float R = u.sp[p * 3].w, H = u.sp[p * 3 + 1].w, m = R + 1.0;\n"
    "    float2 oh = -float2(dot(C, X), dot(C, Z)), dh = float2(dot(D, X), dot(D, Z));\n"
    "    float qa = dot(dh, dh), qb = dot(oh, dh), qc = dot(oh, oh) - m * m, t0 = 0.0, t1 = tmax * 1.05;\n"
    "    if (qa > 1e-8) {\n"
    "      float disc = qb * qb - qa * qc;\n"
    "      if (disc < 0.0) continue;\n"
    "      float sq = sqrt(disc);\n"
    "      t0 = max(t0, (-qb - sq) / qa), t1 = min(t1, (-qb + sq) / qa);\n"
    "    } else if (qc > 0.0) continue;\n"
    "    float oy = -dot(C, Y), dy = dot(D, Y);\n"
    "    if (abs(dy) > 1e-6) {\n"
    "      float ta = (-1.0 - oy) / dy, tb = (H + 1.0 - oy) / dy;\n"
    "      t0 = max(t0, min(ta, tb)), t1 = min(t1, max(ta, tb));\n"
    "    } else if (oy < -1.0 || oy > H + 1.0) continue;\n"
    "    if (t1 <= t0) continue;\n"
    "    float3 col = u.sp[p * 3 + 1].rgb, here = float3(0.0, 0.0, 0.0);\n"
    "    float hot = 0.0, sz = u.spc.z * ps.y;\n"
    "    if (ps.x > 0.5) {\n"
    "      int na = int(clamp(40.0 * ps.z, 8.0, 160.0));\n"
    "      [loop] for (int i = 0; i < na; ++i) {\n"
    "        float fi = float(i) + float(p) * 257.0;\n"
    "        float life = 0.7 + 0.6 * zl_hash(float2(fi, 2.9));\n"
    "        float age = tt / life + zl_hash(float2(fi, 4.3)), cyc = floor(age), ph = age - cyc;\n"
    "        float h1 = zl_hash(float2(fi, cyc * 0.41 + 2.0)), h2 = zl_hash(float2(cyc * 0.67 + 8.0, fi));\n"
    "        float ang = 6.2831853 * h1;\n"
    "        float2 g = float2(cos(ang), sin(ang)) * R * (0.65 + 0.35 * h2) * (1.0 - 0.3 * ph);\n"
    "        float3 P = C + X * g.x + Z * g.y + Y * (H * (0.6 + 0.4 * h2) * ph);\n"
    "        float t = dot(P, D) / a2;\n"
    "        if (t <= 0.0) continue;\n"
    "        float3 off = D * t - P;\n"
    "        float dist = t * len, w = ps.y * max(0.03, 1.5 * pw * dist), ou = dot(off, Y);\n"
    "        float3 op = off - Y * ou;\n"
    "        float e2 = dot(op, op) / (w * w) + ou * ou / (49.0 * w * w);\n"
    "        if (e2 > 9.0) continue;\n"
    "        float k = saturate(0.5 + (tmax - t) * len / max(0.1, 0.01 * dist));\n"
    "        here += col * exp(-e2) * smoothstep(0.0, 0.08, ph) * (1.0 - ph) * (1.0 - ph) * k;\n"
    "      }\n"
    "      float s = zl_s(D, a2, C + Y * (0.5 * H), Y, 0.5 * H);\n"
    "      float3 A = C + Y * (0.5 * H + s);\n"
    "      float ta = dot(A, D) / a2;\n"
    "      if (ta > 0.0) {\n"
    "        float dax = length(D * ta - A), hgt = saturate(s / H + 0.5);\n"
    "        float ka = saturate(0.5 + (tmax - ta) * len / max(0.1, 0.01 * ta * len));\n"
    "        float breathe = 0.9 + 0.1 * sin(tt * 3.0 + hgt * 4.0), halo;\n"
    "        if (u.spp.z > 0.5) {\n"
    "          float g = tap(1, px / u.size.xy).r;\n"
    "          halo = pow(saturate(1.6 * g), 1.3) * (1.0 - smoothstep(1.0, 1.6, dax));\n"
    "        } else {\n"
    "          float edge = max(dax - 0.45 * R, 0.0) / (0.55 * R);\n"
    "          halo = 0.9 * exp(-edge * edge) * (1.0 - smoothstep(0.7, 1.0, hgt));\n"
    "        }\n"
    "        here += col * halo * breathe * ka;\n"
    "      }\n"
    "      sum += here * ps.w;\n"
    "      continue;\n"
    "    }\n"
    "    int n = int(clamp(u.spc.w * ps.z * 2.5 * R * R, 6.0, 200.0));\n"
    "    [loop] for (int i = 0; i < n; ++i) {\n"
    "      float fi = float(i) + float(p) * 257.0;\n"
    "      float life = 2.0 + 2.0 * zl_hash(float2(fi, 1.3));\n"
    "      float age = tm / life + zl_hash(float2(fi, 7.1)), cyc = floor(age), ph = age - cyc;\n"
    "      float h1 = zl_hash(float2(fi, cyc * 0.37 + 11.0)), h2 = zl_hash(float2(cyc * 0.53 + 3.0, fi));\n"
    "      float h3 = zl_hash(float2(fi * 1.7, cyc * 0.29 + 5.0));\n"
    "      float ang = 6.2831853 * h1, rise = H * (0.5 + 0.5 * h3) * ph * (1.5 - 0.5 * ph);\n"
    "      float2 b = float2(cos(ang), sin(ang)) * R * sqrt(h2);\n"
    "      float2 g = b + 0.3 * ph * float2(sin(tm * 1.3 + h3 * 40.0), cos(tm * 1.1 + h1 * 40.0));\n"
    "      float3 P = C + X * g.x + Z * g.y + Y * rise;\n"
    "      float t = dot(P, D) / a2;\n"
    "      if (t <= 0.0) continue;\n"
    "      float3 off = D * t - P;\n"
    "      float dist = t * len, d = length(off), r = sz * max(0.02, 0.8 * pw * dist);\n"
    "      if (d > 20.0 * r) continue;\n"
    "      float k = saturate(0.5 + (tmax - t) * len / max(0.1, 0.01 * dist));\n"
    "      float env = smoothstep(0.0, 0.15, ph) * (1.0 - smoothstep(0.4, 0.95, ph)) * saturate((90.0 - dist) / 30.0) * k;\n"
    "      if (env <= 0.0) continue;\n"
    "      float tw = 0.5 + 0.5 * sin(tt * (8.0 + 7.0 * h3) + h1 * 60.0);\n"
    "      float glint = pow(saturate(sin(tt * (1.7 + 2.3 * h2) + h3 * 30.0)), 24.0);\n"
    "      float2 dp = abs(off.xy) / r;\n"
    "      float core = exp(-d * d / (r * r));\n"
    "      float flare = (exp(-dp.x / 0.3 - dp.y / 4.0) + exp(-dp.y / 0.3 - dp.x / 4.0)) * (0.15 + 1.2 * glint);\n"
    "      float3 ci = col;\n"
    "      if (u.spp.x > 0.0) {\n"
    "        float3 B = C + X * b.x + Z * b.y;\n"
    "        float rb = B.z * u.hand.x;\n"
    "        float2 nb = float2(B.x * u.proj.x + B.z * u.proj.z, B.y * u.proj.y + B.z * u.proj.w) / max(rb, 1e-4);\n"
    "        float2 qb = u.vp.xy + float2(nb.x * 0.5 + 0.5, 0.5 - nb.y * 0.5) * u.vp.zw;\n"
    "        if (rb > 0.0 && all(qb >= u.vp.xy) && all(qb < u.vp.xy + u.vp.zw)) {\n"
    "          float sd = view_z(texel(2, int2(qb)).r) * u.hand.x;\n"
    "          float3 s = texel(0, int2(qb)).rgb;\n"
    "          float sm = max(max(s.r, s.g), s.b);\n"
    "          if ((sd <= 0.0 || sd > rb - max(0.3, 0.02 * rb)) && sm > 0.02) ci = s / sm;\n"
    "        }\n"
    "      }\n"
    "      here += ci * (core * (0.6 + 0.4 * tw + 1.5 * glint) + flare + 0.2 * exp(-d / (2.5 * r))) * env;\n"
    "      hot = max(hot, core * (0.5 + 0.5 * tw + glint) * env);\n"
    "    }\n"
    "    sum += (here + 0.5 * saturate(hot)) * ps.w;\n"
    "  }\n"
    "  return sum;\n"
    "}\n"
    /* debug 10: the first zone line's pixels (those the ray through passes within a few pixels of), red where this
     * pixel's own scene is nearer than the line there, green where the line's place is behind the scene at its own
     * pixel (zl_seen), the rest of the scene darkened */
    "float3 zl_debug(float3 c, float2 px, float z) {\n"
    "  float3 D = view_pos(px, u.hand.x);\n"
    "  float a2 = dot(D, D), len = sqrt(a2), tmax = z != 0.0 ? z * u.hand.x : 1e9;\n"
    "  float3 P0 = u.zl[0].xyz + u.up.xyz * (u.zl[0].w + u.zlc.z);\n"
    "  float4 ax = u.zl[1];\n"
    "  float b = dot(D, ax.xyz), d = -dot(D, P0), e = -dot(ax.xyz, P0), den = a2 - b * b;\n"
    "  float s = abs(den) > 1e-6 ? (a2 * e - b * d) / den : 0.0;\n"
    "  s = clamp(s, -ax.w, ax.w);\n"
    "  float3 P = P0 + ax.xyz * s;\n"
    "  float t = dot(P, D) / a2;\n"
    "  float pw = 2.0 / (u.proj.y * u.vp.w);\n"
    "  if (t <= 0.0 || length(D * t - P) > 3.0 * pw * t * len) return c * 0.4;\n"
    "  return float3(t >= tmax ? 1.0 : 0.0, zl_seen(P) ? 0.0 : 1.0, 0.25);\n"
    "}\n"
    /* VanaCore's rings of light on the ground (vanacore_gfx_set_ground_marks: an addon's marks under targets, its waymarks),
     * the light they add: at each ring (u.gm, three float4s) the pixel's place in the scene (its depth) is taken against the
     * ring's centre, in the world's ground axes; where it lies on the ring's line (a circle, or a square of the same
     * half-width: gm_r), within its own height of the centre, a bright line, a soft glow round it and a faint pool inside, all
     * breathing, and a ripple going out from it every 1.6 seconds. Read from the depth, the ring lies on the ground as it
     * is, a slope too, and whatever stands in front of it (a character, the monster it marks) has its own depth there and
     * takes none of it. A pixel's own size at that distance keeps the line about two pixels wide however far off. Nothing
     * on the sky (z 0), and fading out past 50 units.
     * A ring with a height (gm[3m + 2].y) has a column of light rising from it to that height, as a waymark saying where to
     * stand: the pixel's ray is cut to the column (a cylinder, or a box, over the ring from the ground to its top) and to
     * the scene's depth, so what stands in front of the column takes none of it, and what stands in it is lit in front only;
     * along what's left its light is summed at six steps, brightest at the column's wall and faint within, in streaks round
     * it drifting upward, thinning to nothing at the top. The column shows against the sky too. */
    /* the waymarks' glyphs (ground_marks): A, B, C, D, 1, 2, 3 and 4 as strokes in a box 2 across and 2 high (y down), the
     * segments of each in GM_SEG, from GM_GLYPH[g].x, GM_GLYPH[g].y of them; the same strokes Clawmark draws for itself */
    "static const float4 GM_SEG[46] = {\n"
    "  float4(-0.8, 1.0, 0.0, -1.0), float4(0.0, -1.0, 0.8, 1.0), float4(-0.5, 0.3, 0.5, 0.3),\n"
    "  float4(-0.65, 1.0, -0.65, -1.0), float4(-0.65, -1.0, 0.25, -1.0), float4(0.25, -1.0, 0.7, -0.6),\n"
    "  float4(0.7, -0.6, 0.7, -0.35), float4(0.7, -0.35, 0.3, -0.05), float4(0.3, -0.05, -0.65, -0.05),\n"
    "  float4(0.3, -0.05, 0.75, 0.3), float4(0.75, 0.3, 0.75, 0.65), float4(0.75, 0.65, 0.3, 1.0),\n"
    "  float4(0.3, 1.0, -0.65, 1.0), float4(0.75, -0.6, 0.3, -1.0), float4(0.3, -1.0, -0.35, -1.0),\n"
    "  float4(-0.35, -1.0, -0.8, -0.55), float4(-0.8, -0.55, -0.8, 0.55), float4(-0.8, 0.55, -0.35, 1.0),\n"
    "  float4(-0.35, 1.0, 0.3, 1.0), float4(0.3, 1.0, 0.75, 0.6), float4(-0.65, 1.0, -0.65, -1.0),\n"
    "  float4(-0.65, -1.0, 0.15, -1.0), float4(0.15, -1.0, 0.8, -0.45), float4(0.8, -0.45, 0.8, 0.45),\n"
    "  float4(0.8, 0.45, 0.15, 1.0), float4(0.15, 1.0, -0.65, 1.0), float4(-0.45, -0.55, 0.1, -1.0),\n"
    "  float4(0.1, -1.0, 0.1, 1.0), float4(-0.5, 1.0, 0.7, 1.0), float4(-0.75, -0.6, -0.35, -1.0),\n"
    "  float4(-0.35, -1.0, 0.35, -1.0), float4(0.35, -1.0, 0.75, -0.6), float4(0.75, -0.6, 0.75, -0.2),\n"
    "  float4(0.75, -0.2, -0.75, 1.0), float4(-0.75, 1.0, 0.75, 1.0), float4(-0.7, -1.0, 0.7, -1.0),\n"
    "  float4(0.7, -1.0, 0.05, -0.15), float4(0.05, -0.15, 0.4, -0.15), float4(0.4, -0.15, 0.75, 0.2),\n"
    "  float4(0.75, 0.2, 0.75, 0.6), float4(0.75, 0.6, 0.35, 1.0), float4(0.35, 1.0, -0.35, 1.0),\n"
    "  float4(-0.35, 1.0, -0.75, 0.65), float4(0.45, 1.0, 0.45, -1.0), float4(0.45, -1.0, -0.8, 0.45),\n"
    "  float4(-0.8, 0.45, 0.8, 0.45),\n"
    "};\n"
    "static const int2 GM_GLYPH[8] = { int2(0, 3), int2(3, 10), int2(13, 7), int2(20, 6), int2(26, 3), int2(29, 6), int2(35, 8), int2(43, 3) };\n"
    "float gm_r(float3 d, float3 X, float3 Z, float sq) {\n"
    "  float2 q = float2(dot(d, X), dot(d, Z));\n"
    "  return sq > 0.5 ? max(abs(q.x), abs(q.y)) : length(q);\n"
    "}\n"
    "float3 ground_marks(float2 px, float z) {\n"
    "  float3 X = u.gmx.xyz, Y = u.up.xyz, Z = u.gmz.xyz, sum = float3(0.0, 0.0, 0.0);\n"
    "  float3 D = view_pos(px, u.hand.x);\n"
    "  float len = length(D), tmax = z != 0.0 ? z * u.hand.x : 1e9, tm = u.gmc.y;\n"
    "  float3 P = D * tmax;\n"
    "  float dist = tmax * len, pwk = 2.0 / (u.proj.y * u.vp.w);\n"
    "  float gfade = z != 0.0 ? saturate((60.0 - dist) / 10.0) : 0.0;\n"
    "  float breath = 0.8 + 0.2 * sin(tm * 2.6), t = frac(tm / 1.6);\n"
    "  [loop] for (int m = 0; m < int(u.gmc.x); ++m) {\n"
    "    float3 C = u.gm[m * 4].xyz;\n"
    "    float R = u.gm[m * 4].w, hmax = max(1.5, 0.6 * R);\n"
    "    float4 cs = u.gm[m * 4 + 1];\n"
    "    float sq = u.gm[m * 4 + 2].x, H = u.gm[m * 4 + 2].y;\n"
    "    float3 hi = lerp(cs.rgb, float3(1.0, 1.0, 1.0), 0.35);\n"
    "    if (gfade > 0.0) {\n"
    "      float3 d = P - C;\n"
    "      float h = abs(dot(d, Y)), r = gm_r(d, X, Z, sq);\n"
    "      if (h <= hmax && r <= 1.9 * R) {\n"
    "        float w = max(0.05 * R, 1.5 * pwk * dist), e = (r - R) / w, g = (r - R) / (0.35 * R);\n"
    "        float line_ = exp(-e * e), glow = 0.22 * breath * exp(-g * g);\n"
    "        float pool = r < R ? 0.07 * breath * (0.6 + 0.4 * smoothstep(0.0, R, r)) : 0.0;\n"
    "        float er = (r - R * (1.0 + 0.5 * t)) / w, ripple = 0.5 * (1.0 - t) * (1.0 - t) * exp(-er * er);\n"
    "        float hk = 1.0 - smoothstep(0.5, hmax, h);\n"
    "        sum += (hi * 0.9 * line_ + cs.rgb * (glow + pool + ripple)) * cs.w * gfade * hk;\n"
    "      }\n"
    "    }\n"
    /* the glyph over the column's top (over the ring, with no column): the sign's strokes on a plane facing the camera, its
     * centre T a little over the top, or gh (gm[4m + 3].x) over the ring when the ring says, gsz half its height; the pixel's
     * ray meets the plane at Pg, and the sign is lit where
     * Pg is near a stroke, a bright core with a glow round it, breathing. Only where the scene is behind the plane (the depth,
     * or the sky): a character or a wall in front of it hides it, as of the ring. A pixel's size there keeps the core's edge
     * a pixel soft. */
    "    float gid = u.gm[m * 4 + 2].z, gsz = u.gm[m * 4 + 2].w;\n"
    "    if (gid >= 1.0 && gid <= 8.0 && gsz > 0.0) {\n"
    "      float gh = u.gm[m * 4 + 3].x;\n"
    "      float3 T = C + Y * (gh > 0.0 ? gh : H + (H > 0.0 ? 0.4 : 0.6) + gsz);\n"
    "      float tz = T.z * u.hand.x;\n"
    "      if (tz > 0.1 && (z == 0.0 || z * u.hand.x > tz)) {\n"
    "        float3 Pg = D * tz;\n"
    "        float2 q = float2(Pg.x - T.x, T.y - Pg.y) / gsz;\n"
    "        if (abs(q.x) < 1.3 && abs(q.y) < 1.3) {\n"
    "          int2 gr = GM_GLYPH[int(gid) - 1];\n"
    "          float dmin = 1e9;\n"
    "          [loop] for (int s = gr.x; s < gr.x + gr.y; ++s) {\n"
    "            float4 sg = GM_SEG[s] * float4(0.82, 1.0, 0.82, 1.0);\n"
    "            float2 pa = q - sg.xy, ba = sg.zw - sg.xy;\n"
    "            float hh = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));\n"
    "            dmin = min(dmin, length(pa - ba * hh));\n"
    "          }\n"
    "          float pg = pwk * tz / gsz, e = dmin - 0.11;\n"
    "          float core = 1.0 - smoothstep(-pg, pg, e), gl = exp(-max(e, 0.0) * max(e, 0.0) / 0.05);\n"
    "          float gb = 0.85 + 0.15 * sin(tm * 2.2 + float(m) * 1.3), gfd = saturate((70.0 - tz) / 15.0);\n"
    "          sum += (lerp(cs.rgb, float3(1.0, 1.0, 1.0), 0.3) * core + cs.rgb * 0.5 * gl * gb) * cs.w * gfd;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    if (H <= 0.0) continue;\n"
    "    float cx = dot(C, X), cy = dot(C, Y), cz = dot(C, Z), dx = dot(D, X), dy = dot(D, Y), dz = dot(D, Z);\n"
    "    float t0 = 0.0, t1 = tmax;\n"
    "    bool hit = true;\n"
    "    if (sq > 0.5) {\n"
    "      if (abs(dx) > 1e-6) { float ta = (cx - R) / dx, tb = (cx + R) / dx; t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb)); }\n"
    "      else hit = abs(cx) <= R;\n"
    "      if (abs(dz) > 1e-6) { float ta = (cz - R) / dz, tb = (cz + R) / dz; t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb)); }\n"
    "      else hit = hit && abs(cz) <= R;\n"
    "    } else {\n"
    "      float qa = dx * dx + dz * dz, qb = -(cx * dx + cz * dz), qc = cx * cx + cz * cz - R * R;\n"
    "      if (qa > 1e-8) {\n"
    "        float disc = qb * qb - qa * qc;\n"
    "        if (disc < 0.0) hit = false;\n"
    "        else { float sr = sqrt(disc); t0 = max(t0, (-qb - sr) / qa); t1 = min(t1, (-qb + sr) / qa); }\n"
    "      } else hit = qc <= 0.0;\n"
    "    }\n"
    "    if (abs(dy) > 1e-6) { float ta = cy / dy, tb = (cy + H) / dy; t0 = max(t0, min(ta, tb)); t1 = min(t1, max(ta, tb)); }\n"
    "    else hit = hit && -cy >= 0.0 && -cy <= H;\n"
    "    if (!hit || t1 <= t0) continue;\n"
    "    float bfade = saturate((70.0 - t0 * len) / 15.0);\n"
    "    if (bfade <= 0.0) continue;\n"
    "    float dt = (t1 - t0) / 6.0, acc = 0.0;\n"
    "    [unroll] for (int i = 0; i < 6; ++i) {\n"
    "      float3 d = D * (t0 + (float(i) + 0.5) * dt) - C;\n"
    "      float2 q = float2(dot(d, X), dot(d, Z)) / R;\n"
    "      float hf = saturate(dot(d, Y) / H), rf = sq > 0.5 ? max(abs(q.x), abs(q.y)) : length(q);\n"
    "      float ang = sq > 0.5 ? (abs(q.x) > abs(q.y) ? (q.x > 0.0 ? 0.0 : 0.5) + 0.25 * q.y : (q.y > 0.0 ? 0.25 : 0.75) + 0.25 * q.x)\n"
    "                           : atan2(q.y, q.x) * 0.15915494;\n"
    "      float s1 = 0.5 + 0.5 * sin(ang * 56.549 + tm * 0.6), s2 = 0.5 + 0.5 * sin(ang * 144.513 - tm * 1.1 + hf * 2.0);\n"
    "      float s3 = 0.65 + 0.35 * sin(hf * 18.0 - tm * 3.5 + ang * 301.593);\n"
    "      float streak = (0.35 + 0.65 * s1 * s2) * s3;\n"
    "      float e = (rf - 0.9) / 0.12, wall = exp(-e * e), fill = rf < 1.0 ? 0.12 * (1.0 - rf * rf) : 0.0;\n"
    "      float hk = pow(1.0 - hf, 1.6) * smoothstep(0.0, 0.04, hf);\n"
    "      acc += (wall * streak + fill * (0.6 + 0.4 * s3)) * hk;\n"
    "    }\n"
    "    acc *= dt * len / R;\n"
    "    sum += lerp(cs.rgb, hi, 0.5) * acc * 0.8 * cs.w * bfade;\n"
    "  }\n"
    "  return sum;\n"
    "}\n"
    /* the zone lines laid over the finished scene, after the game's late world draws (the effects' compose runs at the
     * first of its unfogged blended draws, and what it draws after - a lit overlay on a bridge's deck - went over them):
     * the pass's colour is premultiplied (blended ONE, 1 - alpha: the fill's lerp, and the light added) */
    "float4 fx_zlate(FO i) : SV_Target {\n"
    "  int2 px = int2(i.pos.xy);\n"
    "  float z = view_z(texel(2, px).r);\n"
    "  if (int(u.grade.w) == 10) {\n"
    "    float3 d = zl_debug(float3(0.0, 0.0, 0.0), i.pos.xy, z);\n"
    "    return float4(d, any(d > 0.0) ? 1.0 : 0.0);\n"
    "  }\n"
    "  float w;\n"
    "  float3 r = zone_lines_w(float3(0.0, 0.0, 0.0), i.pos.xy, z, w);\n"
    "  if (u.spc.x > 0.0) r += sparkles(i.pos.xy, z);\n"
    "  if (u.gmc.x > 0.0) r += ground_marks(i.pos.xy, z);\n"
    "  return float4(r, w);\n"
    "}\n"
    /* VanaCore's reflections and bounce light over the finished composite (screen space), each a layer of its own at the
     * occlusion's size (fx_ssr, fx_gi: inputs the depth (0), the scene as the composite left it (1), the occlusion and sun (2)
     * and the occlusion's depth levels (3-6)), then laid over the scene at its size (fx_refl). u.refl: strength, wetness,
     * roughness, how far; u.refl2: steps, the sun's glint, bounce strength, the pattern's turn; u.refl3: the bounce's radius,
     * its samples, the blur's taps, no green ground. */
    /* the grades' zones: the midtones and the whites as gains by how light each pixel is (a gain, so a colour keeps its hue), and a
     * tint on the lights only. z: midtones, whites, light tint (-1 cool .. 1 warm). */
    "float3 zone_gain(float3 x, float4 z) {\n"
    "  float lum = dot(x, LUMA);\n"
    "  x *= max(1.0 + 0.7 * z.x * 4.0 * lum * (1.0 - lum) + 1.0 * z.y * smoothstep(0.3, 0.9, lum), 0.0);\n"
    "  float t = z.z * smoothstep(0.15, 0.7, lum);\n"
    "  return x * float3(1.0 + 0.3 * t, 1.0 + 0.06 * t, 1.0 - 0.3 * t);\n"
    "}\n"
    /* the end of the grade: a hard clip, or (s up to 1) a shoulder from 0.9 down to 0.3 that eases the lights toward white, so
     * lifting them doesn't flatten them into it */
    "float3 soft_clip(float3 x, float s) {\n"
    "  x = max(x, 0.0);\n"
    "  if (s <= 0.0) return saturate(x);\n"
    "  float k = lerp(0.9, 0.3, saturate(s));\n"
    "  float3 over = max(x - k, 0.0);\n"
    "  return min(x, k) + (1.0 - k) * (1.0 - exp(-over / (1.0 - k)));\n"
    "}\n"
    /* clarity (local contrast): the viewport's part of the scene at about a quarter of its size (four bilinear taps; then
     * blurred by fx_gauss), and then each pixel's lead on that blur, as a gain, strongest in the middle and lights */
    "float4 fx_lowpass(FO i) : SV_Target {\n"
    "  float2 uv = (u.vp.xy + i.uv * u.vp.zw) / u.size.xy, t = 1.0 / u.size.xy;\n"
    "  float3 c = 0.25 * (tap(0, uv + t * float2(-1, -1)).rgb + tap(0, uv + t * float2(1, -1)).rgb +\n"
    "                     tap(0, uv + t * float2(-1, 1)).rgb + tap(0, uv + t * float2(1, 1)).rgb);\n"
    "  return float4(c, 1.0);\n"
    "}\n"
    "float4 fx_clarity(FO i) : SV_Target {\n"
    "  float4 c = texel(0, int2(i.pos.xy));\n"
    "  float3 b = tap(1, i.uv).rgb;\n"
    "  float l = dot(c.rgb, LUMA);\n"
    "  float w = smoothstep(0.08, 0.45, l) * (1.0 - smoothstep(0.85, 1.0, l));\n"
    "  float lead = clamp(dot(c.rgb - b, LUMA) / max(l, 0.1), -0.5, 0.5);\n"
    "  return float4(saturate(c.rgb * (1.0 + lead * u.clar.x * 1.5 * w)), c.a);\n"
    "}\n"
    /* VanaCore's light from the effects (el_run): the game's additive draws after its effects - flames, glows, spells - drawn
     * again into a target of their own (input 0, el_emit) light what is near them. fx_elx: that light at about a quarter size
     * (fx_lowpass's taps), past a threshold and eased in over the next half (a faint wide haze, a light shaft's sheet, gives
     * little; a flame's core all of it); its distance the nearest surface round it (a flame writes no depth: the wall behind
     * it, or the character holding it; none where all of it is sky, the sun's and moon's sprites), times its brightness in
     * alpha: blurred together, alpha over brightness stays the distance of the light that reached a texel */
    "float4 fx_elx(FO i) : SV_Target {\n"
    "  float2 uv = (u.vp.xy + i.uv * u.vp.zw) / u.size.xy, t = 1.0 / u.size.xy;\n"
    "  float3 e = 0.25 * (tap(0, uv + t * float2(-1, -1)).rgb + tap(0, uv + t * float2(1, -1)).rgb +\n"
    "                     tap(0, uv + t * float2(-1, 1)).rgb + tap(0, uv + t * float2(1, 1)).rgb);\n"
    /* the lanterns' flames (input 2, el_emit: the same draws again, alone) leave only elight2.z of themselves out of it */
    "  if (u.elight2.z != 0.0) {\n"
    "    float3 lt = 0.25 * (tap(2, uv + t * float2(-1, -1)).rgb + tap(2, uv + t * float2(1, -1)).rgb +\n"
    "                        tap(2, uv + t * float2(-1, 1)).rgb + tap(2, uv + t * float2(1, 1)).rgb);\n"
    "    e = max(e - u.elight2.z * lt, 0.0);\n"
    "  }\n"
    /* ... and the dimmed effects (input 3) elight2.w of theirs */
    "  if (u.elight2.w != 0.0) {\n"
    "    float3 dm = 0.25 * (tap(3, uv + t * float2(-1, -1)).rgb + tap(3, uv + t * float2(1, -1)).rgb +\n"
    "                        tap(3, uv + t * float2(-1, 1)).rgb + tap(3, uv + t * float2(1, 1)).rgb);\n"
    "    e = max(e - u.elight2.w * dm, 0.0);\n"
    "  }\n"
    /* ... and the effects on characters - spells, abilities (input 4, elight3.y: drawn this frame) are taken out whole: they
     * are a light of their own, at their own strength (elight3.x, elight_actor), past the threshold on their own and added
     * back scaled. The rest is scaled by elight.x here, not in fx_elc, so the two strengths never meet. */
    "  float3 sp = float3(0.0, 0.0, 0.0);\n"
    "  if (u.elight3.y != 0.0) {\n"
    "    sp = 0.25 * (tap(4, uv + t * float2(-1, -1)).rgb + tap(4, uv + t * float2(1, -1)).rgb +\n"
    "                 tap(4, uv + t * float2(-1, 1)).rgb + tap(4, uv + t * float2(1, 1)).rgb);\n"
    "    e = max(e - sp, 0.0);\n"
    "  }\n"
    "  float l = dot(e, LUMA), ls = dot(sp, LUMA);\n"
    "  if (l <= u.elight.z && ls <= u.elight.z) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float2 px = uv * u.size.xy;\n"
    "  float d = 1e9;\n"
    "  [unroll] for (int k = 0; k < 9; ++k) {\n"
    "    float2 q = clamp(floor(px + float2(k % 3 - 1, k / 3 - 1) * (0.01 * u.vp.w)), u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "    float z = view_z(texel(1, int2(q)).r);\n"
    "    if (z != 0.0) d = min(d, z * u.hand.x);\n"
    "  }\n"
    "  if (d >= 1e9) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  e *= saturate((l - u.elight.z) / 0.5) * u.elight.x;\n"
    "  e += sp * (saturate((ls - u.elight.z) / 0.5) * u.elight3.x);\n"
    "  return float4(e, dot(e, LUMA) * d);\n"
    "}\n";
static const char FX_HLSL_2[] =
    /* ... fx_elc: the scene (a copy, input 7) lit. The levels (inputs 1-5: each the one before at half its size, blurred, its
     * reach twice as far - together a light falls off about as one over the distance squared) up to the one that reaches
     * elight.y world units at the pixel's distance (the depth, input 0), each where its light's distance is near the pixel's.
     * Only the surface under the game's own glows is lit (the scene less the light drawn into it, input 6): a lamp's halo
     * isn't lit again by itself. As a gain over that surface's brightness round the pixel, so each is lit in its own colour
     * and dark ground at night is lit too, eased to at most elight.w times the light it had; and a little added (a surface
     * near black takes some). Limits, eased in so a lamp's light is as before:
     * - a light as wide as the reach (a portal pool's ring art, a home point's beam) is in every level as brightly, and was
     *   taken five times over; a lamp's sums to about 1.33 times its strongest level. At most 1.75 times that level.
     * - the surface's brightness the gain is over is at least 0.4 of its brightest channel, not its luma alone: a navy or
     *   saturated blue surface (luma a fifth of its blue) took the gain of a near-black one.
     * - the gain eases (in its colour: its brightest channel) to what takes the surface round the pixel, by its brightest
     *   channel, to 0.9: already bright ground under a strong light (Al'Taieu's pale tiles) keeps its pattern instead of
     *   going past white to flat white; a dark wall by a lamp, far from that, is lit as before.
     * - and no channel takes more than twice the light reaching it (the portal pool's rim took many times its light in
     *   blue); grey and warm walls stay under it.
     * "Round the pixel" is the pixel and those of the four neighbours (6 px off) within twice its brightness: a dark
     * character on pale ground lowered the ground's brightness beside it, and the ground took a bright outline round them.
     * Render visual 9: the lights and the light they add, on black.
     * elight3.z: laid onto DLSS's output (dlss5_elight_post): the copy (input 7) is the output's, at its size; the depth and
     * the lights' picture (0, 6) stay the scene's, read at the scene pixel under each output pixel (u.vp the scene's
     * viewport, i.uv across the output's), where the frame's DLSS jitter moved the scene's draws to (u.hist.zw, render
     * pixels; as fx_depthup): the output holds still, the scene is jittered, so without it the light crawled along every
     * edge of Hu'Xzoi's striped door, a render pixel a frame. elight3.z 2: the depth (input 0) is the output's own, at its
     * size and held along edges (dlss_depth_up), read at the output pixel: the scene's flipped between a pillar and the wall
     * behind it along the pillar's edge as the jitter moved it, and the light there flickered */
    "float4 fx_elc(FO i) : SV_Target {\n"
    "  int2 p = int2(i.pos.xy);\n"
    "  bool up = u.elight3.z != 0.0;\n"
    "  float2 rp = up ? clamp(u.vp.xy + i.uv * u.vp.zw + u.hist.zw, u.vp.xy, u.vp.xy + u.vp.zw - 1.0) : i.pos.xy;\n"
    "  float2 luv = up ? i.uv + u.hist.zw / u.vp.zw : i.uv;\n"
    "  int2 q = int2(rp);\n"
    "  float4 c = texel(7, p);\n"
    "  float3 e = texel(6, q).rgb;\n"
    "  bool dbg = int(u.grade.w) == 9;\n"
    "  float z = view_z(texel(0, u.elight3.z == 2.0 ? p : q).r);\n"
    "  if (z == 0.0) return dbg ? float4(e, c.a) : c;\n"
    "  float dist = z * u.hand.x, R = u.elight.y;\n"
    "  float lvr = log2(max(R * u.proj.y * 0.5 * u.vp.w / dist, 1e-3) / u.elight2.x);\n"
    "  float3 L = float3(0.0, 0.0, 0.0);\n"
    "  float lk = 0.0;\n"
    "  [unroll] for (uint k = 0; k < 5; ++k) {\n"
    "    float wk = saturate(lvr + 1.0 - float(k));\n"
    "    float4 t = tap(1 + k, luv);\n"
    "    float de = t.a / max(dot(t.rgb, LUMA), 1e-6);\n"
    "    float wz = saturate(1.0 - abs(de - dist) / R);\n"
    "    L += t.rgb * (wk * wz * wz);\n"
    "    lk = max(lk, dot(t.rgb, LUMA) * (wk * wz * wz));\n"
    "  }\n"
    "  float ll = dot(L, LUMA), lc = 1.75 * max(lk, 1e-6);\n"
    "  L *= 1.0 / pow(1.0 + pow(ll / lc, 8.0), 0.125);\n"
    "  float3 s = max(c.rgb - e, 0.0);\n"
    "  float2 uv = rp / u.size.xy, t = 6.0 / u.size.xy;\n"
    "  float2 cv = up ? i.pos.xy / size_of(7) : uv;\n"
    "  float bo = max(dot(s, LUMA), 0.4 * max(s.r, max(s.g, s.b)));\n"
    "  float3 sl = s;\n"
    "  float m = max(c.r, max(c.g, c.b)), ws = 1.0;\n"
    "  [unroll] for (uint n = 0; n < 4; ++n) {\n"
    "    float2 o = float2((n & 1) != 0 ? 1.0 : -1.0, (n & 2) != 0 ? 1.0 : -1.0);\n"
    "    float3 ck = tap(7, cv + t * o).rgb, sk = max(ck - tap(6, uv + t * o).rgb, 0.0);\n"
    "    float bk = max(dot(sk, LUMA), 0.4 * max(sk.r, max(sk.g, sk.b)));\n"
    "    float wk = saturate(2.0 - max(bk, bo) / max(min(bk, bo), 0.02));\n"
    "    sl += sk * wk, m += max(ck.r, max(ck.g, ck.b)) * wk, ws += wk;\n"
    "  }\n"
    "  sl /= ws, m /= ws;\n"
    "  float b = max(dot(sl, LUMA), 0.4 * max(sl.r, max(sl.g, sl.b)));\n"
    "  float3 g = L / max(b, 0.06);\n"
    "  g /= 1.0 + g / u.elight.w;\n"
    "  float gm = max(0.9 / max(m, 1e-3) - 1.0, 1e-4), gx = max(g.r, max(g.g, g.b));\n"
    "  g *= 1.0 / pow(1.0 + pow(gx / gm, 4.0), 0.25);\n"
    "  float3 sg = s * g, sc = max(2.0 * L, 1e-5);\n"
    "  sg /= pow(1.0 + pow(sg / sc, 8.0), 0.125);\n"
    "  float3 add = sg + 0.05 * min(L, 1.0);\n"
    "  if (dbg) return float4(saturate(e + add), c.a);\n"
    "  return float4(c.rgb + add, c.a);\n"
    "}\n"
    "float ign(float2 p, float turn) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)) + turn)); }\n"
    /* a view-space point's pixel (xy) and distance (z): the projection the sun's contact shadow uses */
    "float3 to_px(float3 R) {\n"
    "  float rd = R.z * u.hand.x;\n"
    "  float2 ndc = float2(R.x * u.proj.x + R.z * u.proj.z, R.y * u.proj.y + R.z * u.proj.w) / max(rd, 1e-4);\n"
    "  return float3(u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw, rd);\n"
    "}\n"
    "bool in_view(float2 q) { return all(q >= u.vp.xy + 1.0) && all(q < u.vp.xy + u.vp.zw - 1.0); }\n"
    /* the distance at a scene pixel; 1e5 for the sky */
    "float dist_at(float2 q) {\n"
    "  q = clamp(q, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "  float z = view_z(texel(0, int2(q)).r);\n"
    "  return z != 0.0 ? z * u.hand.x : 1e5;\n"
    "}\n"
    /* the reflections: whether VanaCore's grass covers a scene pixel whose world (input 0, the depth before the grass) is at view z:
     * the depth with the grass (input 9, u.refl3.w bit 1) nearer by more than the depth's rounding */
    "bool grass_over(int2 q, float z) {\n"
    "  if ((int(u.refl3.w + 0.5) & 2) == 0) return false;\n"
    "  float zg = view_z(texel(9, q).r);\n"
    "  float d = z != 0.0 ? z * u.hand.x : 1e5;\n"
    "  return zg != 0.0 && d - zg * u.hand.x > 0.02 + 0.004 * zg * u.hand.x;\n"
    "}\n"
    /* A ray from P along R, stepped through the depth buffer, the steps growing with the distance: it hits where it passes behind
     * a surface by less than a thickness that grows with the depth, refined by halving. xy the hit's pixel (or, for the sky, the
     * last sky pixel the ray was over), z 1, w 1 for a surface and 2 for the sky; w 0 for a miss. tt: how far along it was. */
    "float4 refl_trace(float3 P, float3 R, float dist, float k, float tmax, int NS, out float tt) {\n"
    "  float t0 = 0.03 * dist + 0.05;\n"
    "  float t_lo = t0, last_sky = 0.0;\n"
    "  float2 qs = u.vp.xy, ql = u.vp.xy;\n"
    "  float t_ql = 0.0;\n"
    "  tt = 0.0;\n"
    "  [loop] for (int s = 0; s < NS; ++s) {\n"
    "    float a = (float(s) + k) / float(NS);\n"
    "    float t = t0 + (tmax - t0) * a * a;\n"
    "    float3 q = to_px(P + R * t);\n"
    "    if (q.z <= 0.1 || !in_view(q.xy)) break;\n"
    "    ql = q.xy; t_ql = t;\n"
    "    float sd = dist_at(q.xy), diff = q.z - sd;\n"
    "    if (sd > 9e4) { last_sky = 1.0; qs = q.xy; t_lo = t; continue; }\n"
    "    last_sky = 0.0;\n"
    "    if (diff > 0.02 * q.z && diff < 0.2 + 0.08 * q.z) {\n"
    "      float lo = t_lo, hi = t;\n"
    "      [unroll] for (int j = 0; j < 4; ++j) {\n"
    "        float m = 0.5 * (lo + hi);\n"
    "        float3 qm = to_px(P + R * m);\n"
    "        if (qm.z - dist_at(qm.xy) > 0.0) hi = m; else lo = m;\n"
    "      }\n"
    /* ... but where the ray was already behind that surface's depth just before (lo), it went behind its outline and not into
     * its face: a character over the floor behind it, which the floor would show as a pale halo round them. It goes on */
    "      float3 qh = to_px(P + R * hi), qo = to_px(P + R * lo);\n"
    "      if (qo.z > dist_at(qh.xy) + 0.02 * qh.z + 0.05) { t_lo = t; continue; }\n"
    /* ... and where VanaCore's grass covers what it hit, the picture there is the grass, which isn't reflected: it goes on */
    "      int2 qi = int2(clamp(qh.xy, u.vp.xy, u.vp.xy + u.vp.zw - 1.0));\n"
    "      if (grass_over(qi, view_z(texel(0, qi).r))) { t_lo = t; continue; }\n"
    "      tt = hi;\n"
    "      return float4(qh.xy, 1.0, 1.0);\n"
    "    }\n"
    "    t_lo = t;\n"
    "  }\n"
    "  if (last_sky > 0.5) { tt = t_lo; return float4(qs, 1.0, 2.0); }\n"
    /* no hit: the ray left the picture or ran out of steps with something still in front of it. w 3 and the last pixel it was over
     * (a screen-space reflection can't see what the picture doesn't hold: fx_ssr shows nothing for it) */
    "  if (t_ql > 0.0) { tt = t_ql; return float4(ql, 1.0, 3.0); }\n"
    "  return float4(0.0, 0.0, 0.0, 0.0);\n"
    "}\n"
    /* the brightness of the scene as the composite left it (input 1) at a scene pixel, kept inside the viewport: the fake bump
     * map's height (refl_bump). The sun's shadows (by map and by contact, input 2) are divided back out as the composite laid
     * them on (fx_comp: the same lookup at the same pixel and distance): a shadow's edge read as a step in the floor and bent
     * the reflection along it, and the whole shadow read as a crack and lost its reflection. y the shadows' light there (1 in
     * the sun): the shadows' layer is at the occlusion's size, so right on an edge the division leaves a thin line, and fx_ssr
     * eases the bump off where the light changes across its taps */
    "float2 luma_at(float2 q) {\n"
    "  q = clamp(q, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "  float l = dot(texel(1, int2(q)).rgb, LUMA), sh = 1.0;\n"
    "  if (u.shadow.x > 0.0 || u.smap.x > 0.0) {\n"
    "    float d = dist_at(q);\n"
    "    if (d < 1e5) {\n"
    "      float3 os = ao_at(2, (q - u.vp.xy) / u.vp.zw, d);\n"
    "      sh = max(lerp(1.0, os.y, u.smap.x) * lerp(1.0, os.z, u.shadow.x), 0.15);\n"
    "      l /= sh;\n"
    "    }\n"
    "  }\n"
    "  return float2(l, sh);\n"
    "}\n"
    /* whether a scene pixel is ground that reflects, by the floors' layer (input 7: its red the floor's kind; input 8 the floors'
     * depth) and the bits of the mask (rw: as fx_ssr's): 1 or 0, the depth's
     * mismatch eased over a short range so a floor's own steps and joints don't flip it from pixel to pixel. y is 1 where
     * something stands over the floor (the picture nearer than the floor's layer by more than the mismatch allows: a character,
     * a crate, a wall's foot), which is not the floor changing kind: the feathering leaves such pixels out */
    "float2 floor_ok(int2 q, int rw) {\n"
    "  float zq = view_z(texel(0, q).r);\n"
    "  if (zq == 0.0) return float2(0.0, 0.0);\n"
    "  float dq = zq * u.hand.x, ok = 1.0, occ = 0.0;\n"
    "  if ((rw >> 2) != 0) {\n"
    "    int t = int(texel(7, q).r * 15.9375 + 0.5) - 1;\n"
    "    float fz = view_z(texel(8, q).r);\n"
    "    if (fz != 0.0 && (fz - zq) * u.hand.x > 0.07 + 0.014 * dq) occ = 1.0;\n"
    "    if (t < 0 || ((rw >> 2) & (1 << t)) == 0) return float2(0.0, occ);\n"
    "    ok *= 1.0 - smoothstep(0.04 + 0.008 * dq, 0.07 + 0.014 * dq, abs(fz - zq));\n"
    "  }\n"
    "  return float2(ok, occ);\n"
    "}\n"
    /* one tap of the mask's feathering ring in fx_ssr (n 0 to 11: the angle by n / 3, the radius by n % 3, out to fr): floor_ok
     * there, y 1 where the tap isn't to be counted (something stands over the floor, or the picture rises above the pixel's
     * level) */
    "float2 ring_tap(float2 px, float3 P, float hup, int rw, float fr, int n) {\n"
    "  float ang = float(n / 3) * 1.5707963 + float(n % 3) * 0.5235988;\n"
    "  float rr_ = fr * float(n % 3 + 1) * (1.0 / 3.0);\n"
    "  float2 q = clamp(floor(px + float2(cos(ang), sin(ang)) * rr_), u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "  float2 fo = floor_ok(int2(q), rw);\n"
    "  float zq = view_z(texel(0, int2(q)).r);\n"
    "  if (zq != 0.0 && dot(view_pos(q + 0.5, zq) - P, u.up.xyz) > hup) fo.y = 1.0;\n"
    "  return fo;\n"
    "}\n"
    /* the reflections' layer, at the occlusion's size (u.size.zw): the reflected colour times its weight, plus the sun's glint
     * (rgb), the weight in a: laid over as c * (1 - a) + rgb. The floor: a wet surface, more mirror-like the lower it's seen
     * from (Fresnel), and darker materials more than pale ones */
    "float4 fx_ssr(FO i) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + i.uv * u.vp.zw) + 0.5;\n"
    "  float z = view_z(texel(0, int2(px)).r);\n"
    "  if (z == 0.0) return float4(0.0, 0.0, 0.0, 0.0);\n"
    /* VanaCore's grass takes no reflection (input 0 is the world without it: the ground under a tuft) */
    "  if (grass_over(int2(px), z)) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float3 P = view_pos(px, z);\n"
    "  float dist = z * u.hand.x;\n"
    "  float4 nn = normal_at_r(0, px, P);\n"
    "  if (nn.w == 0.0) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float3 N = nn.xyz, V = normalize(-P);\n"
    "  float NoV = saturate(dot(N, V));\n"
    "  float flat_ = smoothstep(0.6, 0.85, dot(N, u.up.xyz));\n"
    "  float4 c = texel(1, int2(px));\n"
    /* the rain's wet look on the characters (u.rays.w: the wetness times rain.sheen, d3d8_rain.h; input 10 the characters' mask,
     * u.aoc.w): this same reflection on them, whichever way they face, the floor's tests left out, and the occlusion its mask:
     * what's open wet, a crease or an arm's inside dry */
    "  bool chr = u.rays.w > 0.0 && u.aoc.w > 0.0 && texel(10, int2(px)).r > 0.5;\n"
    "  if (chr) flat_ = u.ao.y > 0.0 ? smoothstep(0.3, 0.9, ao_at(2, i.uv, dist).x) : 1.0;\n"
    /* the zone's floors' layer (floors.c, input 7). Its red (u.refl3.w bits 2 on: the ground's kind, the floors' depth input 8):
     * where a floor lies at the pixel's depth, its kind, a bit each over bit 1 (1 object, 2 path,
     * 4 grass, 8 sand, 16 snow, 32 stone, 64 metal, 128 wood, 256 shallow water, 512 deep), says whether there's a reflection; a kind
     * not among them, or no floor at that depth (a crate's top over the ground, the water over its bed, ground out of reach), and
     * there's none */
    "  int rw = int(u.refl3.w + 0.5);\n"
    /* The mask is feathered: the pixel's own answer (floor_ok) and the share of the pixels in two rings round it (4% of the
     * screen's height out, four ways at each of three radii) that agree, so a reflection fades out toward where the mask ends (grass, a path) over
     * a short run and doesn't stop at a pixel's edge. It only fades inside the floor: the share is half on the edge itself (a
     * straight boundary) and all of it a ring's width in. A pixel with something standing over the floor (a character, a crate)
     * isn't counted either way: the floor keeps its reflection right up to its outline, where the composite's depth weighting
     * keeps the edge, and there's no haze round a character the ring would otherwise take out */
    /* (... nor anything else that rises above the floor's level at the pixel: a staircase's steps and sides are wood, or under
     * the stairs' collision ramp, not floor that reflects, and counted, they took the reflection out of the stone beside the
     * stairs and in front of the bottom step, a pale band all round them. Ground that changes kind does it at the floor's own
     * level) */
    "  float mw = 1.0;\n"
    "  if (!chr && (rw >> 2) != 0) {\n"
    "    float a = floor_ok(int2(px), rw).x;\n"
    "    if (a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "    float fr = 0.04 * u.vp.w;\n"
    "    float sum = a, cnt = 1.0;\n"
    "    float hup = 0.2 + 0.01 * dist;\n"
    /* the outer ring first (n 2, 5, 8, 11): where the pixel and those four are all floor (or not counted), the share is 1 and
     * the other eight taps, most of the floor's interior, are not taken; a pixel with anything else near it takes all twelve */
    "    bool inside = a >= 0.999;\n"
    "    [unroll] for (int m = 0; m < 4; ++m) {\n"
    "      float2 fo = ring_tap(px, P, hup, rw, fr, m * 3 + 2);\n"
    "      sum += fo.x * (1.0 - fo.y); cnt += 1.0 - fo.y;\n"
    "      inside = inside && (fo.x >= 0.999 || fo.y > 0.5);\n"
    "    }\n"
    "    if (!inside) {\n"
    "      [unroll] for (int m = 0; m < 8; ++m) {\n"
    "        float2 fo = ring_tap(px, P, hup, rw, fr, (m / 2) * 3 + (m % 2));\n"
    "        sum += fo.x * (1.0 - fo.y); cnt += 1.0 - fo.y;\n"
    "      }\n"
    "    }\n"
    "    mw = smoothstep(0.5, 0.95, sum / cnt);\n"
    "    if (mw <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "    flat_ *= mw;\n"
    "  }\n"
    /* green ground (grass, moss): no reflection or glint on it, by the pixel's own colour */
    "  if (!chr && (rw & 1) != 0) {\n"
    "    float gr = c.g / max(c.r, 0.02), gb = c.g / max(c.b, 0.02);\n"
    "    flat_ *= 1.0 - smoothstep(1.0, 1.12, gr) * smoothstep(1.12, 1.3, gb) * step(0.04, c.g);\n"
    "  }\n"
    /* Render visual 14: what the floor test made of the pixel (flat_, its facing, the floors' mask), fx_refl shows it as it is */
    "  if (int(u.grade.w) == 14) return float4(flat_, NoV, mw, 1.0);\n"
    /* (seen at the lowest angles, where it's most a mirror: a cut at 0.05, about 3 degrees, left the far floor bare from a low
     * camera, a line across it that came nearer as the camera went down) */
    "  if (!(flat_ > 0.0 && NoV > 0.002)) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  flat_ *= smoothstep(0.002, 0.012, NoV);\n"
    "  float k = ign(px, u.refl2.w);\n"
    /* the floor's own brightness with the sun's shadows divided out (luma_at: l0.x), so a shadow doesn't change how much the floor
     * reflects: the picture's luma is low in a shadow, which made the reflection strongest there and end at the shadow's edge */
    "  float2 l0 = luma_at(px);\n"
    "  float wet = u.refl.y, luma = l0.x;\n"
    "  float3 lay = float3(0.0, 0.0, 0.0);\n"
    "  float w = 0.0;\n"
    "  float F = 0.04 + 0.96 * pow(1.0 - NoV, 5.0);\n"
    "  float amt = u.refl.x * flat_ * saturate(0.3 * wet + F * (0.5 + wet)) * saturate(1.4 - luma);\n"
    /* the rain's wet ground (u.rain, the rain pass's mask; only with the reflections on): these same reflections, as wet floor
     * shows them (the wetness at its most), in patches - the mask's green a noise laid in the world (and the low ground),
     * which rain.puddles cuts at a level, more of the ground wet the higher it is - and none between them. rain.reflect how
     * strong. The floor's own gloss and bump: a wet patch is the floor's reflection, no sharper */
    "  float rwet = 0.0, rough_ = u.refl.z;\n"
    "  if (chr) amt = saturate(u.refl.x * flat_ * saturate(0.3 + 1.5 * F) * saturate(1.4 - luma) * u.rays.w), wet = 1.0;\n"
    "  else if (u.rain.w > 0.0) {\n"
    "    float4 rnm = RAIN_AT(px);\n"
    "    float cut = 0.78 - 0.24 * u.rain.z;\n"
    "    rwet = smoothstep(cut - 0.08, cut + 0.08, rnm.g) * saturate(rnm.r * 1.5) * saturate(u.rain.x * 1.15);\n"
    "    float wamt = u.refl.x * flat_ * saturate(0.3 + 1.5 * F) * saturate(1.4 - luma) * u.rain.y;\n"
    "    amt = lerp(amt, max(amt, saturate(wamt)), rwet);\n"
    "    wet = max(wet, rwet);\n"
    "  }\n"
    /* Render visual 30 (/vana debug's Puddles): the wet patches alone, for fx_refl to show in red */
    "  if (int(u.grade.w) == 30) return float4(rwet, 0.0, 0.0, 1.0);\n"
    /* a fake bump map: the floor's own picture (input 1, its sun shadows divided out: luma_at) read as a height, bright high and its
     * grooves and tile edges low, tilts the normal the reflection and the glint are taken from, along the floor's two
     * directions (across the screen, and away from the camera), by how much brighter each side is than the other relative to the
     * neighbourhood, so the tilt doesn't follow the stone's tone. Fades out with distance, where the texture's detail is
     * smaller than a pixel and would only shimmer, and takes some of the reflection out of the cracks (darker than around them).
     * Eased off across a shadow's edge (the sun's light differing between the taps), where what's left of the edge after
     * luma_at would still read as a ridge */
    "  float3 Nb = N;\n"
    "  float bump = u.elight2.y * (1.0 - smoothstep(25.0, 70.0, dist));\n"
    "  if (bump > 0.0) {\n"
    "    float s = max(floor(u.vp.z / u.size.z + 0.5), 1.0);\n"
    "    float2 br = luma_at(px + float2(s, 0.0)), bl = luma_at(px - float2(s, 0.0));\n"
    "    float2 bu = luma_at(px - float2(0.0, s)), bd = luma_at(px + float2(0.0, s)), bc = l0;\n"
    "    float shi = max(max(max(br.y, bl.y), max(bu.y, bd.y)), bc.y), slo = min(min(min(br.y, bl.y), min(bu.y, bd.y)), bc.y);\n"
    "    bump *= 1.0 - smoothstep(0.1, 0.6, (shi - slo) / shi);\n"
    "    float lr = br.x, ll = bl.x, lu = bu.x, ld = bd.x;\n"
    "    float m = max(0.25 * (lr + ll + lu + ld), 0.04);\n"
    "    float3 ty = P - N * dot(P, N), tx = float3(1.0, 0.0, 0.0) - N * N.x;\n"
    "    ty *= rsqrt(max(dot(ty, ty), 1e-6));\n"
    "    tx *= rsqrt(max(dot(tx, tx), 1e-6));\n"
    "    float3 t = (0.8 * bump) * (((lr - ll) / m) * tx + ((lu - ld) / m) * ty);\n"
    "    t /= max(1.0, length(t) / 0.5);\n"
    "    Nb = normalize(N - t);\n"
    "    amt *= 1.0 - saturate(bump) * 0.6 * saturate(2.0 * (m - bc.x) / m);\n"
    "  }\n"
    "  float3 R = reflect(-V, Nb);\n"
    /* a ray that leaves the floor low, tilted up (refl_lift): its rise above the floor's plane at least about the lift, eased
     * in so a steeper one is hardly changed */
    "  if (u.bounce.w > 0.0) {\n"
    "    float rn = dot(R, N);\n"
    "    R = normalize(R + N * (sqrt(rn * rn + u.bounce.w * u.bounce.w) - rn));\n"
    "  }\n"
    /* (a tilt at a low angle can turn the ray into the floor: held above it) */
    "  R = normalize(R + N * max(0.05 - dot(R, N), 0.0));\n"
    /* the steps by how much the reflection shows (amt, how much of the layer's pixel it can be): the full count from 0.25 up
     * (a low angle, a wet floor), about a third of it at 0.05 and under, never fewer than 16 or the quality's own where that
     * is less; none under 0.003 (a bright pixel), where nothing of it would show */
    "  int ns_all = int(u.refl2.x);\n"
    "  int ns = max(min(ns_all, 16), int(float(ns_all) * lerp(0.35, 1.0, saturate((amt - 0.05) / 0.2)) + 0.5));\n"
    "  float tt = 0.0;\n"
    "  float4 h = float4(0.0, 0.0, 0.0, 0.0);\n"
    "  if (amt > 0.003) h = refl_trace(P, R, dist, k, u.refl.w, ns, tt);\n"
    /* a surface or the sky only: a ray that left the picture or went behind something (w 3) has nothing to show and the floor
     * shows no reflection there. Its last pixel stood in for it once, and every floor pixel under a pillar or by the screen's
     * side took the same pixel, stretched into streaks down the floor */
    "  float conf = h.w > 0.5 && h.w < 2.5 ? 1.0 : 0.0;\n"
    "  if (h.w > 1.5) conf *= smoothstep(0.0, 0.3, dot(R, u.up.xyz));\n"
    "  conf *= smoothstep(0.0, 0.25, R.z * u.hand.x);\n"
    "  if (conf > 0.0) {\n"
    "    float2 e = (h.xy - u.vp.xy) / u.vp.zw;\n"
    /* a hit near the picture's edges fades out, so a reflection that runs out of the picture eases away and doesn't stop at a
     * line; the sky (its last pixel, at the top as often as not) only toward the sides */
    "    float sf = smoothstep(0.0, 0.1, min(e.x, 1.0 - e.x));\n"
    "    float ef = h.w < 1.5 ? smoothstep(0.0, 0.1, min(e.y, 1.0 - e.y)) : 1.0;\n"
    "    conf *= sf * ef;\n"
    "    if (h.w < 1.5) conf *= 1.0 - smoothstep(0.5 * u.refl.w, u.refl.w, tt);\n"
    /* blurred more the farther the thing reflected is from the floor it shows on (a glossy cone) */
    "    float rr = rough_ * saturate(tt / (dist + tt)) * 0.05 * u.vp.w;\n"
    /* a hit on a silhouette (the distance jumps within a couple of pixels of it) reads its own pixel, not the bilinear mix across
     * the edge, and the blur's taps that land on the other side of it are replaced by it: the reflected object's rim keeps its
     * own colour and doesn't pick up the wall behind it as a fringe */
    "    float d0 = dist_at(h.xy), es = max(1.0, floor(u.vp.w / 600.0 + 0.5));\n"
    "    bool edge = false;\n"
    "    if (h.w < 1.5) {\n"
    "      float dm = max(max(abs(dist_at(h.xy + float2(es, 0.0)) - d0), abs(dist_at(h.xy - float2(es, 0.0)) - d0)),\n"
    "                     max(abs(dist_at(h.xy + float2(0.0, es)) - d0), abs(dist_at(h.xy - float2(0.0, es)) - d0)));\n"
    "      edge = dm > 0.06 * d0 + 0.05;\n"
    "    }\n"
    "    float3 c0 = edge ? texel(1, int2(h.xy)).rgb : tap(1, h.xy / u.size.xy).rgb;\n"
    "    float3 rc = c0;\n"
    "    if (rr > 1.0) {\n"
    "      int nb = int(u.refl3.z);\n"
    "      float ang = k * 6.2831853;\n"
    "      [loop] for (int j = 0; j < nb; ++j) {\n"
    "        float a2 = ang + float(j) * (6.2831853 / float(nb));\n"
    "        float2 q = clamp(h.xy + float2(cos(a2), sin(a2)) * rr, u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "        float3 sc = tap(1, q / u.size.xy).rgb;\n"
    "        if (edge && abs(dist_at(q) - d0) > 0.06 * d0 + 0.05) sc = c0;\n"
    "        rc += sc;\n"
    "      }\n"
    "      rc /= float(nb + 1);\n"
    "    }\n"
    "    w = saturate(amt * conf);\n"
    "    lay = rc * w;\n"
    "  }\n"
    /* the sun's glint on the wet floor, where it is lit: from the light the shadows fall from (sunsh, held over the horizon at
     * dusk, so the glint and the shadows that block it agree), and faded as the shadows are (sunsh.w: the game's direct light,
     * the zone's fade-in), since with no shadow map drawn there is nothing to say the building is in the way */
    "  if (u.refl2.y > 0.0 && u.sun.w > 0.0 && u.sunsh.w > 0.0) {\n"
    "    float3 Hs = u.sunsh.xyz + V;\n"
    "    float3 H = Hs * rsqrt(max(dot(Hs, Hs), 1e-6));\n"
    "    float gloss = lerp(260.0, 30.0, u.refl.z);\n"
    "    float spec = pow(saturate(dot(Nb, H)), gloss) *(0.04 + 0.96 * pow(1.0 - saturate(dot(H, V)), 5.0));\n"
    "    float vis = 1.0;\n"
    "    if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) {\n"
    "      float3 os = ao_at(2, i.uv, dist);\n"
    "      vis = lerp(1.0, os.y, u.smap.x) * lerp(1.0, os.z, u.shadow.x);\n"
    "    }\n"
    /* the sun's own direction through the picture: the shadow maps are drawn from the light held over the horizon at dusk
     * (sun_dusk), so a wall or gate that blocks the real, lower sun but not the held one leaves the floor 'lit' in them. A
     * ray from the floor toward the sun, through the depth buffer, that meets a surface blocks the glint */
    /* (only where the glint would show: not in a shadow, which has taken it already, nor where it is under a 1024th) */
    "    float gl = spec * vis * u.sunsh.w * smoothstep(0.0, 0.15, dot(u.sun.xyz, u.up.xyz)) * flat_ * u.refl.x * u.refl2.y * lerp(0.4, 1.0, wet) * 2.0;\n"
    "    if (spec > 1e-4 && gl > 1.0 / 1024.0) {\n"
    "      float tts;\n"
    /* (half the steps: it only asks whether something blocks the sun) */
    "      float4 hs = refl_trace(P, u.sun.xyz, dist, k, max(u.refl.w, 100.0), max(min(ns_all, 16), ns_all / 2), tts);\n"
    "      if (hs.w > 0.5 && hs.w < 1.5) gl = 0.0;\n"
    "    }\n"
    "    float3 hue = u.suncol.rgb / max(max(u.suncol.r, u.suncol.g), max(u.suncol.b, 0.1));\n"
    "    lay += hue * gl;\n"
    "  }\n"
    "  return float4(lay, w);\n"
    "}\n"
    /* the bounce light's layer, at its own size (u.bounce: about 1000 pixels across, soft anyway): the colour of the lit surfaces
     * near each one, gathered around it in a spiral from the occlusion's depth levels (the far samples from the coarser ones, as
     * the occlusion's) and weighted by how squarely they face it and how close they are, times how squarely the surface faces
     * the camera (rgb: it tints the surface it falls on, and fills in the corners); its distance in a, for the temporal pass.
     * The colours are read from input 1 as a small copy of the scene (the viewport at the layer's size, so the scattered reads
     * stay in cache). The pattern turns with the frame (u.hist.y) only where the temporal pass gathers enough of them to even
     * it out; else (u.aoq.w: DLSS or FSR gathering, or the temporal pass under 0.9) it is held still, as the occlusion's, or
     * it would shimmer */
    /* (the pattern's noise is the layer texel's own (i.pos), not the scene pixel it stands for: those are 2 or 4 pixels apart
     * on a big screen, and ign read that far apart is slanted stripes ten texels wide, not a grain. Neither the blur nor the
     * temporal pass, which holds the history to the texels round it, took them out: streaks down a wall at max quality on a
     * 4K screen, moving as the pattern turned) */
    /* the surface normal from the neighbours `st` pixels away on the side nearer in depth (normal_depth's, from further
     * out: one pixel's step reads the depth buffer's own steps as tilt, and a flat floor lights itself from them) */
    /* (a neighbour on a character, by the stencil's mask - input 7, where u.aoc.w - is not the surface: a floor pixel beside a boot
     * took the leg for its slope and stood as a wall facing the camera, a glow along every character's feet. The side that is
     * not a character's is used; with both or neither it is the nearer in depth, as it was) */
    "bool gi_chr(float2 q) { return u.aoc.w > 0.0 && texel(7, int2(clamp(q, u.vp.xy, u.vp.xy + u.vp.zw - 1.0))).r > 0.5; }\n"
    "float3 gi_nrm(float2 px, float3 P, float st) {\n"
    "  float2 ex = float2(st, 0.0), ey = float2(0.0, st);\n"
    "  float3 r = pos_at(0, px + ex) - P, l = P - pos_at(0, px - ex);\n"
    "  float3 dn = pos_at(0, px + ey) - P, up = P - pos_at(0, px - ey);\n"
    "  bool rc = gi_chr(px + ex), lc = gi_chr(px - ex), dc = gi_chr(px + ey), uc = gi_chr(px - ey);\n"
    "  float3 dx = rc != lc ? (rc ? l : r) : (abs(r.z) < abs(l.z) ? r : l);\n"
    "  float3 dy = dc != uc ? (dc ? up : dn) : (abs(dn.z) < abs(up.z) ? dn : up);\n"
    "  float3 N = cross(dx, dy);\n"
    "  float nn = dot(N, N);\n"
    "  if (!(nn > 1e-20)) return float3(0.0, 0.0, 0.0);\n"
    "  N *= rsqrt(nn);\n"
    "  return dot(N, P) > 0.0 ? -N : N;\n"
    "}\n"
    "float4 fx_gi(FO i) : SV_Target {\n"
    "  float2 px = floor(u.vp.xy + i.uv * u.vp.zw) + 0.5;\n"
    "  float z = view_z(texel(0, int2(px)).r);\n"
    "  if (z == 0.0) return float4(0.0, 0.0, 0.0, 0.0);\n"
    /* a character's texel is no layer at all (no distance, so gi_at gives it no weight): its own gather is bright against the
     * floor it stands on, and the world's pixels beside it took it for a neighbour, a white rim round every boot */
    "  if (u.aoc.w > 0.0 && texel(7, int2(px)).r > 0.5) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float3 P = view_pos(px, z);\n"
    "  float dist = z * u.hand.x;\n"
    "  float4 nd = normal_det(u.clar.z, px, P);\n"
    "  float3 N = nd.w > 0.0 ? nd.xyz : gi_nrm(px, P, 2.0 * max(floor(u.vp.z / u.bounce.x + 0.5), 1.0));\n"
    "  if (dot(N, N) == 0.0) return float4(0.0, 0.0, 0.0, dist);\n"
    "  float facing = smoothstep(0.1, 0.4, saturate(dot(N, normalize(-P))));\n"
    "  float rad = u.refl3.x, rpx = min(rad * u.proj.y * 0.5 * u.vp.w / dist, 0.18 * u.vp.w);\n"
    "  if (rpx < 3.0 || facing <= 0.0) return float4(0.0, 0.0, 0.0, dist);\n"
    "  int NS = int(u.refl3.y);\n"
    /* (held still, u.aoq.w, as the occlusion's: the same pattern every frame, which the blur's block averages) */
    "  float k = ign(i.pos.xy, u.aoq.w > 0.0 ? 0.0 : u.hist.y);\n"
    "  float3 acc = float3(0.0, 0.0, 0.0);\n"
    "  [loop] for (int s = 0; s < NS; ++s) {\n"
    "    float a = (float(s) + k) / float(NS);\n"
    "    float ang = float(s) * 2.3999632 + k * 6.2831853;\n"
    "    float r = a * rpx + 2.0;\n"
    "    float2 q = px + float2(cos(ang), sin(ang)) * r;\n"
    "    if (!in_view(q)) continue;\n"
    /* a character gives none: the bounce light is the world's, so what they wear lights nothing (the stencil's mask, input 7,
     * where u.aoc.w) */
    "    if (u.aoc.w > 0.0 && texel(7, int2(q)).r > 0.5) continue;\n"
    "    float3 Q = pos_lz(q, r);\n"
    "    float qd = Q.z * u.hand.x;\n"
    "    if (qd <= 0.0 || dist - qd > 0.5 * rad) continue;\n"
    "    float3 v = Q - P;\n"
    "    float vv = dot(v, v), h = dot(v, N), cs = h * rsqrt(vv + 1e-6);\n"
    "    float fall = saturate(1.0 - vv / (rad * rad));\n"
    /* only what rises clear of the surface's plane counts: its flat neighbours sit within the depth buffer's steps of it, a
     * little above it half the time, and gathered bright that prints the steps' contours on the floor as streaks */
    "    if (h <= 0.01 * dist + 0.03 || cs <= 0.15) continue;\n"
    "    acc += tap(1, (q - u.vp.xy) / u.vp.zw).rgb * ((cs - 0.15) * fall * fall);\n"
    "  }\n"
    /* the samples' weights sum to about a tenth of what the light a surface's surroundings give it would: times four, a bright
     * wall beside shaded ground shows */
    "  return float4(min(acc * (4.0 * facing / float(NS)), 2.0), dist);\n"
    "}\n"
    /* the bounce light over time, as fx_temporal: each texel's world point found in the frame before (its camera), and that
     * frame's result (input 1) blended in where its distance agrees, held to what this frame has around the texel on the same
     * surface. With few samples a frame and the pattern turning, this averages them into a result that holds still */
    "float4 fx_gitemp(FO i) : SV_Target {\n"
    "  float4 c = texel(0, int2(i.pos.xy));\n"
    "  if (c.a <= 0.0 || u.hist.w == 0.0) return c;\n"
    "  float2 px = u.vp.xy + i.uv * u.vp.zw;\n"
    "  float3 P = view_pos(px, c.a * u.hand.x);\n"
    "  float4 pc = mul(u.reproj, float4(P, 1.0));\n"
    "  if (pc.w <= 1e-4) return c;\n"
    "  float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
    "  if (any(puv < 0.0) || any(puv > 1.0)) return c;\n"
    "  float4 h = tap(1, puv);\n"
    "  if (!(h.a > 0.0) || abs(h.a - pc.w) > 0.04 * pc.w) return c;\n"
    "  int2 p = int2(i.pos.xy), hi = int2(u.bounce.xy) - 1;\n"
    "  float3 lo = c.rgb, up = c.rgb;\n"
    "  [unroll] for (int dy = -1; dy <= 1; ++dy)\n"
    "    [unroll] for (int dx = -1; dx <= 1; ++dx) {\n"
    "      float4 t = texel(0, clamp(p + int2(dx, dy), int2(0, 0), hi));\n"
    "      if (t.a > 0.0 && abs(t.a - c.a) < 0.05 * c.a) { lo = min(lo, t.rgb); up = max(up, t.rgb); }\n"
    "    }\n"
    "  const float give = 0.02;\n"
    /* (its history weighs more than the occlusion's: a gather of a dozen samples over a wide disc is noisy, and only this
     * averages it over frames) */
    "  float keep = clamp(max(0.5 * (1.0 + u.hist.z), 1.0 - 0.01 * u.refl3.y), 0.0, 0.96);\n"
    "  return float4(lerp(c.rgb, clamp(h.rgb, lo - give, up + give), keep), c.a);\n"
    "}\n"
    /* the bounce light's layer smoothed over its neighbours along one direction (the pass's: across, then down), each weighted
     * by how near its distance is to the texel's, so a corner's edge stays on its own side: a texel's dozen samples become
     * its neighbours' several dozen together */
    "float4 fx_giblur(FO i) : SV_Target {\n"
    "  int2 p = int2(i.pos.xy), hi = int2(u.bounce.xy) - 1, dir = fdir();\n"
    "  float4 c = texel(0, p);\n"
    "  if (c.a <= 0.0) return c;\n"
    "  float3 s = c.rgb;\n"
    "  float w = 1.0;\n"
    "  [unroll] for (int n = -3; n <= 3; ++n) {\n"
    "    if (n == 0) continue;\n"
    "    float4 t = texel(0, clamp(p + dir * (n * int(u.bounce.z)), int2(0, 0), hi));\n"
    "    float k = (1.0 - abs(float(n)) * 0.25) * (t.a > 0.0 ? saturate(1.0 - abs(t.a - c.a) / (0.05 * c.a)) : 0.0);\n"
    "    s += t.rgb * k; w += k;\n"
    "  }\n"
    "  return float4(s / w, c.a);\n"
    "}\n"
    /* a layer at a scene pixel from the four nearest texels of input k, each weighted by how near its distance (input kd, the
     * occlusion's depth) is to the pixel's, as ao_at: an edge's reflection stays on its own side */
    /* (the reflections' texels are one jittered ray each at the occlusion's size: the four nearest, bilinear, left the edges of a
     * reflection - where its rays stop hitting, where the ground's kind or the floor's slope cuts it - as a staircase of the
     * layer's texels. The 3x3 round the nearest texel, a tent wider than bilinear's, still weighted by distance, so a
     * geometry edge stays on its own side but a reflection's own edge eases out over a texel or two) */
    /* (... and where none of the nine is near the pixel's distance - something thinner or smaller than a texel of the layer
     * standing in front of the floor, a character a way off - the weights are all small alike, and normalised they laid the
     * floor's reflection from behind it over it: a pale haze on everyone in front of a bright reflection, worst with the
     * camera low. The nearest texel in depth must be within a tenth of the pixel's distance for all of it, none past a
     * quarter) */
    /* (the layer's nine texels read first: where all are empty, most of the picture - walls, the sky, ground that doesn't
     * reflect - the answer is nothing and the depth isn't read) */
    "float4 lay_at(uint k, uint kd, float2 uv, float dist) {\n"
    "  float2 g = uv * u.size.zw - 0.5;\n"
    "  int2 n0 = int2(floor(g + 0.5)), hi = int2(u.size.zw) - 1;\n"
    "  float4 lt[9];\n"
    "  bool some = false;\n"
    "  [unroll] for (int j = 0; j < 9; ++j) {\n"
    "    lt[j] = texel(k, clamp(n0 + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), hi));\n"
    "    some = some || any(lt[j] != 0.0);\n"
    "  }\n"
    "  if (!some) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float4 s = float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float w = 0.0, best = 0.0;\n"
    "  [unroll] for (int n = 0; n < 9; ++n) {\n"
    "    int2 o = int2(n % 3 - 1, n / 3 - 1), p = clamp(n0 + o, int2(0, 0), hi);\n"
    "    float2 d = abs(float2(n0 + o) - g);\n"
    "    float bw = saturate(1.5 - d.x) * saturate(1.5 - d.y);\n"
    "    float td = texel(kd, p).r * u.hand.x;\n"
    "    float dw = td > 0.0 ? 1.0 / (1e-3 + abs(td - dist) / dist) : 1e-3;\n"
    "    s += lt[n] * (bw * dw); w += bw * dw; best = max(best, dw);\n"
    "  }\n"
    "  if (!(w > 0.0)) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  return (s / w) * saturate((best - 4.0) / 6.0);\n"
    "}\n"
    /* the bounce light's layer (input k, at u.bounce's size) at a scene pixel: as lay_at, each texel weighted by how near its
     * own distance (its alpha) is to the pixel's */
    "float4 gi_at(uint k, float2 uv, float dist) {\n"
    "  float2 g = uv * u.bounce.xy - 0.5, f = frac(g);\n"
    "  int2 i0 = int2(floor(g)), hi = int2(u.bounce.xy) - 1;\n"
    "  float4 s = float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float w = 0.0;\n"
    "  [unroll] for (int n = 0; n < 4; ++n) {\n"
    "    int2 o = int2(n & 1, n >> 1), p = clamp(i0 + o, int2(0, 0), hi);\n"
    "    float4 t = texel(k, p);\n"
    "    float bw = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);\n"
    /* (the weight falls with the square of the relative distance, and not steeply at a floor's own slope: a sharp one picks
     * one texel of the four by a hair's difference in depth and the pick flips as the camera moves, a shimmer of blocks) */
    "    float rd = abs(t.a - dist) / dist;\n"
    "    float dw = t.a > 0.0 ? 1.0 / (0.02 + 60.0 * rd * rd) : 1e-3;\n"
    "    s += t * (bw * dw); w += bw * dw;\n"
    "  }\n"
    "  return w > 0.0 ? s / w : float4(0.0, 0.0, 0.0, 0.0);\n"
    "}\n"
    /* Render visual 18's colours, by the floors' kind of ground: object, path, grass, sand, snow, stone, metal, wood, shallow water,
     * deep water, and any other (/vana debug's legend, debug.lua KINDS) */
    "static const float3 KIND_COL[11] = { float3(0.71, 0.55, 0.90), float3(0.78, 0.71, 0.55), float3(0.43, 0.78, 0.35),\n"
    "  float3(0.90, 0.82, 0.43), float3(0.95, 0.96, 1.0), float3(0.91, 0.58, 0.36), float3(0.91, 0.39, 0.55),\n"
    "  float3(0.66, 0.42, 0.24), float3(0.35, 0.78, 0.90), float3(0.24, 0.43, 0.86), float3(1.0, 0.0, 1.0) };\n"
    /* the reflections (input 2) and bounce light (3) laid over the composite (1) at the scene's size; the depth is input 0 and
     * the occlusion's depth (the reflections' texels' distances) input 4 */
    "float4 fx_refl(FO i) : SV_Target {\n"
    "  int2 px = int2(i.pos.xy);\n"
    "  float4 c = texel(1, px);\n"
    "  float z = view_z(texel(0, px).r);\n"
    /* Render visual 17 and 18 (/vana debug's Reflections and Reflection ground): over the picture in grey, dimmed */
    "  int dview = int(u.grade.w);\n"
    "  float grey = 0.35 * dot(c.rgb, LUMA);\n"
    "  if (z == 0.0) return dview == 17 || dview == 18 ? float4(grey.xxx, c.a) : dview == 30 ? float4(dot(c.rgb, LUMA).xxx, c.a) : c;\n"
    /* Render visual 1-6 (the occlusion, fog, bloom, god rays, the sun's shadows, the ink lines: one layer alone, from fx_comp):
     * as it came, not with its own reflection laid over it - a wet floor mirrored the shadows' mask as a grey smear under the
     * walls (the reflections trace the composite, which is that layer then) */
    "  if ((dview >= 1 && dview <= 6) || dview == 19 || dview == 26 || dview == 31 || dview == 32) return c;\n"
    /* Render visual 18: each pixel a floor of the zone lies at coloured by its kind of ground as the reflections read it
     * (inputs 5 and 6, the floors' layer and its depth, as fx_ssr's 7 and 8: a snowfield's stone is snow there, floors.c),
     * solid for the kinds the reflections keep to (refl_floors), striped for the rest. Grey where no floor lies at the pixel's
     * depth (someone standing on it, a crate's top, ground out of reach), and all of it grey without the layer (Ground on
     * Any, a zone whose collision isn't read) */
    "  if (dview == 18) {\n"
    "    float3 o = grey.xxx;\n"
    "    int rw = int(u.refl3.w + 0.5);\n"
    "    if ((rw >> 2) != 0) {\n"
    "      int t = int(texel(5, px).r * 15.9375 + 0.5) - 1;\n"
    "      float fz = view_z(texel(6, px).r), dq = z * u.hand.x;\n"
    "      if (t >= 0 && fz != 0.0 && abs(fz - z) < 0.07 + 0.014 * dq) {\n"
    "        float sp = 8.0 * max(floor(u.vp.w / 1080.0 + 0.5), 1.0);\n"
    "        bool keep = ((rw >> 2) & (1 << t)) != 0, band_ = frac((i.pos.x + i.pos.y) / (2.0 * sp)) < 0.5;\n"
    "        if (keep || band_) o = lerp(o, KIND_COL[min(t, 10)] * (0.55 + 0.45 * saturate(1.5 * dot(c.rgb, LUMA))), 0.85);\n"
    "      }\n"
    "    }\n"
    "    return float4(o, c.a);\n"
    "  }\n"
    "  float dist = z * u.hand.x;\n"
    "  float3 o = c.rgb;\n"
    "  if (u.refl.x > 0.0) {\n"
    "    float4 L = lay_at(2, 4, i.uv, dist);\n"
    /* Render visual 12: the reflections' layer as laid over (its colour, twice), 13 its weight, 14 the layer's texel as fx_ssr
     * left it (the floor test's parts) */
    "    int dv = int(u.grade.w);\n"
    "    if (dv == 12) return float4(saturate(L.rgb * 2.0), c.a);\n"
    "    if (dv == 13) return float4(saturate(L.aaa * 2.0), c.a);\n"
    "    if (dv == 14) return float4(texel(2, int2(i.uv * u.size.zw)).rgb, c.a);\n"
    /* Render visual 30: fx_ssr's wet patches (its texel's red) bright red over the picture in grey */
    "    if (dv == 30) return float4(lerp(dot(c.rgb, LUMA).xxx, float3(1.0, 0.0, 0.0), saturate(texel(2, int2(i.uv * u.size.zw)).r)), c.a);\n"
    /* only on ground that faces up, as fx_ssr's: the layer's texels are at the occlusion's size, and a character's outline
     * just in front of the floor is near enough in depth to take the floor's texels round it, a pale haze over them */
    /* (... not on a character while the rain's reflection is on them: fx_ssr's chr, the stencil's mask as input 7) */
    "    if ((L.a > 0.0 || any(L.rgb > 0.0)) && !(u.rays.w > 0.0 && u.aoc.w > 0.0 && texel(7, px).r > 0.5)) {\n"
    "      float3 P = view_pos(i.pos.xy, z);\n"
    "      float4 nn = normal_at_r(0, i.pos.xy, P);\n"
    "      L *= nn.w > 0.0 ? smoothstep(0.6, 0.85, dot(nn.xyz, u.up.xyz)) : 0.0;\n"
    "    }\n"
    /* Render visual 17: the reflections as laid over, at twice their weight, in their own colour over the grey: where they land
     * and what they show */
    "    if (dv == 17) return float4(grey * (1.0 - saturate(2.0 * L.a)) + saturate(2.0 * L.rgb), c.a);\n"
    "    o = o * (1.0 - saturate(L.a)) + L.rgb;\n"
    "  }\n"
    "  if (dview == 17) return float4(grey.xxx, c.a);\n"
    "  if (dview == 30) return float4(dot(c.rgb, LUMA).xxx, c.a);\n"
    "  if (u.refl2.z > 0.0) {\n"
    "    float4 G = gi_at(3, i.uv, dist);\n"
    /* not on a character (the stencil's mask, input 7, where u.aoc.w): the bounce light is the world's */
    "    if (u.aoc.w > 0.0 && texel(7, px).r > 0.5) G = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (int(u.grade.w) == 8) return float4(saturate(G.rgb * (u.refl2.z * 4.0)), c.a);\n"
    /* what the surface turns the light into: its colour's hue at a fixed level, not its own brightness (a shaded surface is
     * where bounce light shows, and it is dark), neutral where it is too dark to have a hue, and less on what is bright
     * already */
    "    float l = dot(c.rgb, LUMA);\n"
    "    float3 alb = lerp(float3(1.0, 1.0, 1.0), clamp(c.rgb / max(l, 0.08), 0.4, 1.6), saturate(l / 0.2));\n"
    "    o += G.rgb * alb * (u.refl2.z * 1.2 * (1.0 - 0.6 * smoothstep(0.6, 1.0, l)));\n"
    "  }\n"
    "  return float4(o, c.a);\n"
    "}\n"
    /* the grade's hue ([graphics.effects] hue, u.post4.z in radians): every colour turned round the grey axis by `a`, greys left
     * as they are (Rodrigues' rotation; what turns below 0 is held at 0) */
    "float3 hue_turn(float3 x, float a) {\n"
    "  if (a == 0.0) return x;\n"
    "  float3 k = float3(0.57735, 0.57735, 0.57735);\n"
    "  float ca = cos(a);\n"
    "  return max(x * ca + cross(k, x) * sin(a) + k * dot(k, x) * (1.0 - ca), 0.0);\n"
    "}\n"
    /* the colour grade, laid last (fx_post): on the upscaler's output, or on the scene itself once the game is done drawing into it
     * (grade_last), so every effect is under it. The composite's own (below) is Direct3D 11's; Direct3D 12 turns that off */
    "float3 post_grade(float3 c) {\n"
    "  float3 x = c;\n"
    "  if (u.post2.x != 0.0) x = pow(max(x, 0.0), exp2(-u.post2.x));\n"
    "  float lum = dot(x, LUMA);\n"
    "  x *= max(1.0 + u.post2.y * (1.0 - smoothstep(0.0, 0.6, lum)) + 0.6 * u.post2.z * smoothstep(0.4, 1.0, lum), 0.0);\n"
    "  x *= float3(1.0 + 0.12 * u.post.w, 1.0 + 0.02 * u.post.w, 1.0 - 0.12 * u.post.w);\n"
    "  x = zone_gain(x, u.post3);\n"
    "  x = hue_turn(x, u.post4.z);\n"
    "  x = lerp(dot(x, LUMA).xxx, x, u.post.y);\n"
    "  x = soft_clip(x, u.post3.w);\n"
    "  x = lerp(x, x * x * (3.0 - 2.0 * x), u.post.z);\n"
    "  return lerp(c, x, u.post.x);\n"
    "}\n"
    /* the moonlight ([graphics.effects] night_moon), through the Vana'diel night (u.post4.x, GfxScene.night: 0 by day): the picture
     * drained of its colour toward a cool blue-grey of its own brightness, as eyes see by the moon, its darks lifted a little into
     * blue (night isn't black). What glows - a lamp, a fire, a spell: bright and coloured - keeps its own colour */
    "float3 moon_grade(float3 c) {\n"
    "  float n = u.post4.x * u.post4.y;\n"
    "  if (n <= 0.0) return c;\n"
    "  float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b));\n"
    "  float glow = smoothstep(0.45, 0.85, hi) * smoothstep(0.2, 0.5, (hi - lo) / max(hi, 1e-4));\n"
    "  float3 moon = dot(c, LUMA) * float3(0.86, 0.98, 1.26) + float3(0.008, 0.013, 0.03);\n"
    "  return lerp(c, moon, saturate(n) * (1.0 - glow));\n"
    "}\n"
    /* VanaCore's player light ([graphics.effects] player_light, gfx_shared.c gfx_player_light): a light hangs over the player
     * (u.plight: their feet in view space, w the pool's radius; plight2: its colour times its gain, w the share taken off round
     * it) and shines down, a soft pool round them; m = how far in its cone. At night what is in the cone is lit by how it faces
     * the light (the normal from the depth, input 2, the nearer side each way), what already glows is left: a gain on the
     * scene's own light (the composite's sun). By day the gain is 0 and the composite darkens what is outside it (plight2.w) */
    "float3 player_light(float2 px, float3 P, float3 c, out float m) {\n"
    "  float r = u.plight.w, h = max(0.8 * r, 3.0);\n"
    "  float3 v = P - (u.plight.xyz + u.up.xyz * h);\n"
    "  float d = length(v), D = sqrt(h * h + r * r);\n"
    "  m = 0.0;\n"
    "  if (!(d > 1e-4) || d > 1.25 * D) return float3(0.0, 0.0, 0.0);\n"
    "  float3 dir = v / d;\n"
    "  m = smoothstep(h / D, h * rsqrt(h * h + 0.09 * r * r), dot(dir, -u.up.xyz)) * (1.0 - smoothstep(0.5, 1.0, d / (1.25 * D)));\n"
    "  float k = m * (1.0 - smoothstep(0.35, 0.9, max(c.r, max(c.g, c.b))));\n"
    "  if (k <= 0.0 || u.plight2.r <= 0.0) return float3(0.0, 0.0, 0.0);\n"
    /* (which way the surface faces: a normal map's normal, else the game's own smooth one (smooth_at: the sun's, Direct3D 12
     * with sun shadows on), else the depth's. The depth's is one per polygon: a character's showed each of them, a flat patch
     * lit or dark beside the next ("it's not smoothing around characters", 2026-11-04), so by it the facing counts for less) */
    "  float4 Nn = normal_det(u.clar.w, px, P);\n"
    "  float lo = 0.3, ndl = 1.0;\n"
    "  if (Nn.w <= 0.0) Nn = smooth_at(px, P);\n"
    "  if (Nn.w <= 0.0) { Nn = normal_depth(2, px, P); lo = 0.75; }\n"
    "  if (Nn.w > 0.0) ndl = saturate(dot(Nn.xyz, -dir));\n"
    "  return u.plight2.rgb * k * lerp(lo, 1.0, ndl);\n"
    "}\n"
    /* Render visual 15 (the shadows' rings, sun_taps) and 16 (the occlusion's, fx_ao): the scene tinted by the ring each pixel is
     * in at distance d (r: on, how far apart; off, all inside the first): green all the taps, then yellow, orange, and red past
     * the third, a dark line at each ring's edge. The /vana debug window's Show rings */
    "float3 ring_tint(float3 c, float d, float2 r) {\n"
    "  float t = r.x > 0.0 ? d / max(r.y, 1.0) : 0.0;\n"
    "  float3 k = t < 1.0 ? float3(0.62, 0.81, 0.42) : t < 2.0 ? float3(0.91, 0.83, 0.36) : t < 3.0 ? float3(0.91, 0.58, 0.36)\n"
    "                                                                                 : float3(0.93, 0.44, 0.47);\n"
    "  float3 o = lerp(c, k * (0.35 + 0.65 * dot(c, LUMA)), 0.6);\n"
    "  float e = abs(frac(t + 0.5) - 0.5);\n"
    "  if (t > 0.5 && t < 3.5) o *= lerp(0.25, 1.0, smoothstep(0.0, 1.5 * fwidth(t), e));\n"
    "  return o;\n"
    "}\n"
    /* the characters' mask (cmask_pass): white where the stencil test passes, the characters' bit set by their draws */
    "float4 fx_cmask(FO i) : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n"
    "float4 fx_comp(FO i) : SV_Target {\n"
    "  int2 px = int2(i.pos.xy);\n"
    "  float4 c = texel(6, px);\n"
    "  int dbg = int(u.grade.w);\n"
    "  float z = view_z(texel(2, px).r);\n"
    "  float3 os = float3(1.0, 1.0, 1.0);\n"
    "  if (u.ao.y > 0.0 || u.shadow.x > 0.0 || u.smap.x > 0.0) os = ao_at(1, i.uv, z * u.hand.x);\n"
    /* what glows (a lamp's glass, a lit doorway: near white in a colour) is light, not a surface: the occlusion leaves it, as
     * it greyed the lamps it stood next to (from XI on Anything, rubymatrix/xi-on-anything, gfx_hlsl.c) */
    "  float o = lerp(os.x, 1.0, smoothstep(0.6, 0.95, max(c.r, max(c.g, c.b)))), sun = lerp(1.0, os.y, u.smap.x) * lerp(1.0, os.z, u.shadow.x);\n"
    /* ao_chars: on a character the occlusion at its own share of ao's strength (the characters' depth is input 9, as for their
     * lines); a share past 1 can't take it below nothing */
    "  if (u.aoc.y > 0.0 && z != 0.0 && char_line(i.pos.xy, view_pos(i.pos.xy, z), z * u.hand.x)) o = saturate(lerp(1.0, o, u.aoc.x));\n"
    /* Render visual 19: which pixels ao_chars takes for a character's, over the picture in grey. With the stencil's mask
     * (u.aoc.w: input 10) red where a character's draw marked the pixel, exactly; without, from the characters' depth drawn
     * again: red where all four of the texels round the pixel match (char_line), yellow where one to three do (an edge, or a
     * hair off in depth: the pixels that can change hands between frames). Grey, not a character's (the world's strength) */
    "  if (dbg == 19) {\n"
    "    float gr = 0.35 * dot(c.rgb, LUMA);\n"
    "    if (u.aoc.w > 0.0) return float4(texel(10, px).r > 0.5 ? float3(1.0, 0.15, 0.15) : float3(gr, gr, gr), c.a);\n"
    "    int hits = z != 0.0 && u.aoc.y > 0.0 ? char_hits(i.pos.xy, view_pos(i.pos.xy, z), z * u.hand.x) : 0;\n"
    "    float3 m = hits >= 4 ? float3(1.0, 0.15, 0.15) : hits > 0 ? float3(1.0, 0.85, 0.1) : float3(gr, gr, gr);\n"
    "    return float4(m, c.a);\n"
    "  }\n"
    /* Render visual 26 (/vana debug's Grass): VanaCore's grass red, shaded by its own brightness, the rest grey. A pixel the grass
     * covers is one whose depth (input 2) isn't the depth before the grass (input 8, gfx_scene_world_depth; the depth itself
     * when this scene has none: all grey) */
    "  if (dbg == 26) {\n"
    "    float l = dot(c.rgb, LUMA);\n"
    "    return float4(texel(8, px).r != texel(2, px).r ? float3(1.0, 0.1, 0.1) * (0.45 + 0.55 * saturate(2.0 * l)) : l.xxx, c.a);\n"
    "  }\n"
    /* Render visual 27: the far map itself (input 10), as drawn this frame, over the scene's viewport (the game's 3D target is
     * square, stretched to the window): grey its depth from the sun (darker nearer), magenta where nothing was drawn. Centred on the
     * player (sun_centre), it holds still as the camera turns: a shadow that comes and goes there is in the map, one that comes
     * and goes only in the picture is its reading */
    "  if (dbg == 27) {\n"
    "    float side = min(u.vp.z, u.vp.w);\n"
    "    float2 q = (i.pos.xy - u.vp.xy) / side;\n"
    "    if (any(q < 0.0) || any(q >= 1.0)) return float4(c.rgb * 0.25, c.a);\n"
    /* through size_of, not tx2[] itself: Direct3D 11 takes this HLSL too, and its fx_source refuses what it has no helper for */
    "    float d = texel(10, int2(q * size_of(10))).r;\n"
    "    return float4(d >= 1.0 ? float3(1.0, 0.0, 1.0) : d.xxx, c.a);\n"
    "  }\n"
    "  if (dbg == 1) return float4(o, o, o, c.a);\n"
    "  if (dbg == 5) return float4(sun, sun, sun, c.a);\n"
    "  if ((dbg >= 21 && dbg <= 25) || dbg == 28 || dbg == 29) return float4(os.y, os.y, os.y, c.a);\n"
    "  if (dbg == 15 && z != 0.0) return float4(ring_tint(c.rgb, z * u.hand.x, u.aor.zw), c.a);\n"
    /* the occlusion's by the share of its taps fx_ao takes (ao_taps: a ring cuts none while the disc is wide) */
    "  if (dbg == 16 && z != 0.0) {\n"
    "    float d = z * u.hand.x, rpx = min(u.ao.x * u.proj.y * 0.5 * u.vp.w / d, u.ao.w);\n"
    "    int2 cell = px & 3;\n"
    "    int NS = max(int(u.aoq.x), 1);\n"
    "    float s = float(ao_taps(NS, d, rpx, (float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0)) / float(NS);\n"
    "    return float4(ring_tint(c.rgb, s >= 1.0 ? 0.0 : s >= 0.75 ? 1.5 : s >= 0.5 ? 2.5 : 3.5, float2(1.0, 1.0)), c.a);\n"
    "  }\n"
    /* (the player's light where it reaches, as a light of its own: it fills the shadows the moon casts under it; by day the
     * world outside it darker, the sky left) */
    "  float3 pl = float3(0.0, 0.0, 0.0);\n"
    "  float pm = 1.0;\n"
    "  if (u.plight.w > 0.0 && z != 0.0) {\n"
    "    pl = player_light(i.pos.xy, view_pos(i.pos.xy, z), c.rgb, pm);\n"
    "    pm = lerp(1.0 - u.plight2.w, 1.0, pm);\n"
    "  }\n"
    "  c.rgb *= lerp(1.0, o, u.ao.y) * (sun + pl) * pm;\n"
    /* the cel look's bands on the world (the sky's gradient stays), then its lines, under the fog */
    "  if (z != 0.0 && u.toon2.y > 0.0) c.rgb = band(c.rgb);\n"
    "  if (u.toon.x > 0.0) {\n"
    "    float e = 0.0;\n"
    /* VanaCore's grass left out (toon3.z, outline_nograss; input 8 the depth before it): a pixel it covers takes no line, the
     * others theirs from the world alone */
    "    bool ng = u.toon3.z > 0.0;\n"
    "    if (z != 0.0 && !(ng && texel(8, px).r != texel(2, px).r)) {\n"
    "      float3 P = view_pos(i.pos.xy, z);\n"
    "      float lw = u.toon.y;\n"
    "      bool chr = u.toon3.w > 0.0 && char_line(i.pos.xy, P, z * u.hand.x);\n"
    "      if (chr) lw = u.toon3.w;\n"
    "      e = ng ? outline_at(8, i.pos.xy, P, z * u.hand.x, lw, chr)\n"
    "             : outline_at(2, i.pos.xy, P, z * u.hand.x, lw, chr);\n"
    "    }\n"
    "    if (dbg == 6) return float4(1.0 - e, 1.0 - e, 1.0 - e, c.a);\n"
    "    c.rgb *= 1.0 - u.toon.x * e;\n"
    "  }\n"
    "  float f = 0.0;\n"
    "  if (u.fogc.a > 0.0 && z != 0.0) {\n"
    "    float3 P = view_pos(i.pos.xy, z);\n"
    "    float d = length(P), bd = u.fogp.x * dot(P, u.up.xyz);\n"
    /* density a * exp(-b * height above the camera), integrated from the camera to P; below the camera it thickens
     * only down to fog_floor (up.w = b * fog_floor), as thick as that from there on down: low water, a harbour or a
     * valley seen from above once drowned in a fog that grew without limit with the drop (issue 93) */
    "    float k = abs(bd) > 1e-4 ? (1.0 - exp(-bd)) / bd : 1.0;\n"
    "    if (bd < -u.up.w) { float ef = exp(u.up.w); k = (ef - 1.0 + (-bd - u.up.w) * ef) / -bd; }\n"
    "    f = min(1.0 - exp(-u.fogc.a * d * k), u.fogp.y);\n"
    /* the sun's light scattered toward the camera (Henyey-Greenstein, 1 looking straight at it) */
    "    float g = u.fogp.w, cs = dot(P / max(d, 1e-5), u.sun.xyz);\n"
    "    float sunk = u.sun.w * u.fogp.z * pow((1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cs, 1e-5), 1.5);\n"
    /* the zone's fog colour, or (grade2.w, fog_color=sky) the sky's, once there's one (input 7) */
    "    float4 skyc = float4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (u.grade2.w > 0.5) skyc = texel(7, int2(0, 0));\n"
    "    float3 fogrgb = (u.grade2.w > 0.5 && skyc.a > 0.0) ? skyc.rgb : u.fogc.rgb;\n"
    "    c.rgb = lerp(c.rgb, fogrgb + u.suncol.rgb * sunk, f);\n"
    "  }\n"
    /* not where they're laid after the world (ZL_LATE: VanaCore's Direct3D 12, fx_zlate): inlined, they were most of
     * this pass's code and its compile (28 s of 29) */
    "#ifndef ZL_LATE\n"
    "  if (dbg == 10 && u.zlc.x > 0.0) return float4(zl_debug(c.rgb, i.pos.xy, z), c.a);\n"
    "  if (u.zlc.x > 0.0) c.rgb = zone_lines(c.rgb, i.pos.xy, z);\n"
    "  if (u.spc.x > 0.0) c.rgb += sparkles(i.pos.xy, z);\n"
    "  if (u.gmc.x > 0.0) c.rgb += ground_marks(i.pos.xy, z);\n"
    "#endif\n"
    "  if (dbg == 2) return float4(f, f, f, c.a);\n"
    "  float3 add = float3(0.0, 0.0, 0.0);\n"
    "  if (u.bloom.y > 0.0) {\n"
    "    float3 bl = tap(3, i.uv).rgb * 0.6 + tap(4, i.uv).rgb * 0.8;\n"
    "    if (dbg == 3) return float4(bl, c.a);\n"
    "    add += bl * u.bloom.y;\n"
    "  }\n"
    /* (VanaCore: the rays take the sun light's hue, not its brightness, which is often dim) */
    "  if (u.rays.x > 0.0 && u.sunuv.z > 0.0) {\n"
    "    float3 hue = u.suncol.rgb / max(max(u.suncol.r, u.suncol.g), max(u.suncol.b, 0.1));\n"
    "    float3 r = tap(5, i.uv).rgb * hue * u.sunuv.z;\n"
    "    if (dbg == 4) return float4(r, c.a);\n"
    "    add += r * u.rays.x;\n"
    "  }\n"
    "  c.rgb = screen(c.rgb, add);\n"
    /* Direct3D 11's colour grade, here in the composite (Direct3D 12 lays its own last, fx_post, and gives this a strength of 0):
     * the tone first: brightness as a gamma (black and white stay where they are), then the darks and the lights lifted or
     * pulled down by how dark or light each pixel is (a gain, so a colour keeps its hue) */
    "  if (u.grade.x != 0.0) {\n"
    "    float3 x = c.rgb;\n"
    "    if (u.grade2.x != 0.0) x = pow(max(x, 0.0), exp2(-u.grade2.x));\n"
    "    float lum = dot(x, LUMA);\n"
    "    x *= max(1.0 + u.grade2.y * (1.0 - smoothstep(0.0, 0.6, lum)) + 0.6 * u.grade2.z * smoothstep(0.4, 1.0, lum), 0.0);\n"
    "    x *= float3(1.0 + 0.12 * u.bloom.w, 1.0 + 0.02 * u.bloom.w, 1.0 - 0.12 * u.bloom.w);\n"
    "    x = zone_gain(x, u.grade3);\n"
    "    x = hue_turn(x, u.post4.z);\n"
    "    x = lerp(dot(x, LUMA).xxx, x, u.grade.y);\n"
    "    x = soft_clip(x, u.grade3.w);\n"
    "    x = lerp(x, x * x * (3.0 - 2.0 * x), u.grade.z);\n"
    "    c.rgb = lerp(c.rgb, x, u.grade.x);\n"
    "  }\n"
    "  c.rgb = moon_grade(c.rgb);\n" /* (Direct3D 11's: Direct3D 12's composite has u.post4 0, and lays it last, fx_post) */
    "  return c;\n"
    "}\n"
    /* a level of the scene's chain from the one above it: the bilinear tap at a 2x2 block's middle */
    "float4 fx_mip(FO i) : SV_Target { return tap(0, i.uv); }\n"
    /* ... and the colour grade's, laid last (in place of fx_mip's copy): onto the upscaler's output (input 0 the output, sized to
     * the viewport it is drawn into), or, with u.post2.w, onto the scene itself from a copy of it (the same pixel) */
    "float4 fx_post(FO i) : SV_Target {\n"
    "  float4 c = u.post2.w > 0.5 ? texel(0, int2(i.pos.xy)) : tap(0, i.uv);\n"
    "  c.rgb = moon_grade(post_grade(c.rgb));\n"
    "  return c;\n"
    "}\n"
    /* VanaCore's DLSS inputs, at (0, 0) of targets the size of the scene's viewport (input 0 the scene's depth):
     * the depth as it is, and each pixel's motion, in render pixels to where it was the frame before - the
     * camera's alone, through its view and projection then (u.reproj); the sky's as if far off. With dlss_motion
     * (u.hand.y 1), input 1 is the moving things' own (mo_draw: the motion as a share of the viewport, made render pixels
     * here, and the view z of what moved), taken where they were drawn and nothing drawn after them is nearer (the depth:
     * an effect, which writes none, is in front of it). Never past twice the viewport: a point barely in front of the
     * camera the frame before (a huge monster's body sweeping past it) made a motion past a half float's largest, infinite
     * in the target, or no number at all (NaN), and DLSS carried that in its history as a haze over the whole picture till
     * it was made again (min and max take the number over a NaN, so the clamp makes one off the screen). The depth is an
     * effect's model's where frame generation has its motion (the motion target's w 2: mo_draw's fg_model) and it's in front:
     * given its motion over the wall's depth, frame generation drew a home point's crystal twice and tore it into blocks */
    "float4 fx_dlssz(FO i) : SV_Target {\n"
    "  int2 p = int2(u.vp.xy + i.pos.xy);\n"
    "  float d = texel(0, p).r;\n"
    "  if (u.hand.y > 0.5) {\n"
    "    float4 m = texel(1, p);\n"
    "    float zf = abs(view_z(d));\n"
    "    if (m.w > 1.5 && abs(m.x) < 16384.0 && m.z > 0.0 && (zf == 0.0 || m.z < zf)) {\n"
    "      float dn = (u.zp.y / (m.z * u.hand.x) + u.zp.x) * u.hand.x;\n"
    "      d = saturate(dn * (u.zp.w - u.zp.z) + u.zp.z);\n"
    "    }\n"
    "  }\n"
    "  return float4(d, 0.0, 0.0, 0.0);\n"
    "}\n"
    "float4 fx_dlssmv(FO i) : SV_Target {\n"
    "  float2 p = u.vp.xy + i.pos.xy;\n"
    "  float lim = 2.0 * max(u.vp.z, u.vp.w);\n"
    "  if (u.hand.y > 0.5) {\n"
    "    float4 m = texel(1, int2(p));\n"
    "    float zf = abs(view_z(texel(0, int2(p)).r));\n"
    "    if (abs(m.x) < 16384.0 && abs(m.y) < 16384.0 && (zf == 0.0 || m.z <= zf * 1.01 + 0.05))\n"
    "      return float4(clamp(m.xy * u.vp.zw, -lim, lim), 0.0, 0.0);\n"
    "  }\n"
    "  float z = view_z(texel(0, int2(p)).r);\n"
    "  float3 P = view_pos(p, z != 0.0 ? z : u.hand.x * 1000.0);\n"
    "  float4 pc = mul(u.reproj, float4(P, 1.0));\n"
    "  if (pc.w <= 1e-5) return float4(0.0, 0.0, 0.0, 0.0);\n"
    "  float2 ndc = pc.xy / pc.w;\n"
    "  float2 prev = u.vp.xy + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * u.vp.zw;\n"
    "  return float4(clamp(prev - p, -lim, lim), 0.0, 0.0);\n"
    "}\n"
    /* The upscaler's masks of what isn't the static world (dlss_run; dlss_ipc.h reactive, tc): where DLSS or FSR is to lean on
     * this frame's pixels instead of its history, which the camera's motion (fx_dlssmv) would fetch from the wrong place.
     * The game's effects: inputs 3 and 4 the scene with them and without (dlss_keep_before), the most any channel changed
     * eased in from u.hist.z to u.hist.w (8-bit, gamma-encoded) times u.hist.x (dlss_reactive; 0: not this frame). What moves
     * on its own: input 1 the moving things drawn again (mask_draw: 1 and the view z; u.hand.w 1 when there are any this
     * frame), where nothing drawn after them is nearer (input 0 the scene's depth), and VanaCore's grass, where the depth
     * before it (input 2, gfx_scene_world_depth) differs, at u.hand.z (dlss_reactive_moving). u.hist.y 0: the reactive mask,
     * the larger of the two; 1: the transparency and composition mask, the effects alone */
    "float4 fx_dlssr(FO i) : SV_Target {\n"
    "  int2 p = int2(u.vp.xy + i.pos.xy);\n"
    "  float fx = 0.0;\n"
    "  if (u.hist.x > 0.0) {\n"
    "    float3 dc = abs(texel(3, p).rgb - texel(4, p).rgb);\n"
    "    fx = saturate((max(dc.r, max(dc.g, dc.b)) - u.hist.z) / max(u.hist.w - u.hist.z, 1e-4)) * u.hist.x;\n"
    "  }\n"
    "  if (u.hist.y > 0.5) return float4(fx, 0.0, 0.0, 0.0);\n"
    "  float d = texel(0, p).r;\n"
    "  float m = abs(d - texel(2, p).r) > 1e-7 ? 1.0 : 0.0;\n"
    "  if (u.hand.w > 0.5) {\n"
    "    float2 q = texel(1, p).xy;\n"
    "    float zf = abs(view_z(d));\n"
    "    if (q.x > 0.5 && (zf == 0.0 || q.y <= zf * 1.01 + 0.05)) m = 1.0;\n"
    "  }\n"
    "  return float4(max(fx, m * u.hand.z), 0.0, 0.0, 0.0);\n"
    "}\n"
    /* DLSS 5's lights (dlss5_fx_light): the game's additive draws (input 0, el_emit: the sprites as drawn, on black) at a
     * quarter of the viewport, past the glow effects lighting's threshold (u.elight.z) and eased in over the next half, as
     * fx_elx takes them (a sprite's faint haze across its whole quad gives nothing, its core all of it); the characters'
     * effects among them (input 2, el_part 2, when any was drawn: elight3.y) at their own share of it (elight3.x,
     * dlss5_fx_actor: a spell's sprite is big and bright, and at the flames' gain was a sea of white); those it isn't to
     * take as lights (input 3, el_part 3, dlss5_skip_tex: Al'Taieu's portal pools, which it made coloured blocks of;
     * elight3.w) taken out whole; none where all
     * round it is sky (no depth, input 1: the sun's and moon's sprites); taken down to an eighth and blurred after (fx_down,
     * fx_gauss) and laid onto what DLSS 5 sees as a soft glow in the sprite's colour (fx_dlssc). Handed the sprite itself,
     * lifted whole, DLSS 5 restyled a spell's quad as a thing in the scene (a dark panel with a glowing emblem on it); a
     * glow over the ground it takes for light */
    "float4 fx_dlssg(FO i) : SV_Target {\n"
    "  float2 uv = (u.vp.xy + i.uv * u.vp.zw) / u.size.xy, t = 1.0 / u.size.xy;\n"
    "  float3 e = 0.25 * (tap(0, uv + t * float2(-1, -1)).rgb + tap(0, uv + t * float2(1, -1)).rgb +\n"
    "                     tap(0, uv + t * float2(-1, 1)).rgb + tap(0, uv + t * float2(1, 1)).rgb);\n"
    "  if (u.elight3.y != 0.0) {\n"
    "    float3 sp = 0.25 * (tap(2, uv + t * float2(-1, -1)).rgb + tap(2, uv + t * float2(1, -1)).rgb +\n"
    "                        tap(2, uv + t * float2(-1, 1)).rgb + tap(2, uv + t * float2(1, 1)).rgb);\n"
    "    e = max(e - sp, 0.0) + sp * u.elight3.x;\n"
    "  }\n"
    "  if (u.elight3.w != 0.0) {\n"
    "    float3 sk = 0.25 * (tap(3, uv + t * float2(-1, -1)).rgb + tap(3, uv + t * float2(1, -1)).rgb +\n"
    "                        tap(3, uv + t * float2(-1, 1)).rgb + tap(3, uv + t * float2(1, 1)).rgb);\n"
    "    e = max(e - sk, 0.0);\n"
    "  }\n"
    "  float l = dot(e, LUMA);\n"
    "  if (l <= u.elight.z) return float4(0.0, 0.0, 0.0, 1.0);\n"
    "  float2 px = uv * u.size.xy;\n"
    "  bool sky = true;\n"
    "  [unroll] for (int k = 0; k < 9; ++k) {\n"
    "    float2 q = clamp(floor(px + float2(k % 3 - 1, k / 3 - 1) * (0.01 * u.vp.w)), u.vp.xy, u.vp.xy + u.vp.zw - 1.0);\n"
    "    sky = sky && view_z(texel(1, int2(q)).r) == 0.0;\n"
    "  }\n"
    "  if (sky) return float4(0.0, 0.0, 0.0, 1.0);\n"
    "  return float4(e * saturate((l - u.elight.z) / 0.5), 1.0);\n"
    "}\n"
    /* the lights' mean, one texel (dlss5_fx_norm): the glow at an eighth (input 0, blurred) averaged over a 16x16 grid,
     * as luma, eased over the frames with the last frame's (input 1; u.hist.x 1 when there is one). fx_dlssc divides the
     * gain by how far it's past dlss5_fx_norm: a light that fills the hall (Tu'Lia's beam, a big white blob to the blur)
     * gives the picture about the light a small one does, instead of lighting every wall at full gain; a crystal keeps its
     * gain */
    "float4 fx_dlssm(FO i) : SV_Target {\n"
    "  float3 s = float3(0.0, 0.0, 0.0);\n"
    "  [loop] for (int y = 0; y < 16; ++y)\n"
    "    [loop] for (int x = 0; x < 16; ++x)\n"
    "      s += tap(0, (float2(x, y) + 0.5) / 16.0).rgb;\n"
    "  float m = dot(s / 256.0, LUMA);\n"
    "  float last = u.hist.x != 0.0 ? texel(1, int2(0, 0)).x : m;\n"
    "  return float4(lerp(last, m, 0.1), 0.0, 0.0, 1.0);\n"
    "}\n"
    /* DLSS 5's: the viewport's part of the scene's colour (input 0) onto the float target it takes, with u.hand.z times
     * the glow from the effects (input 1, fx_dlssg's, read across the viewport: dlss5_fx_light's lights, past white; 0
     * without) added - not over the sky (no depth there, input 2: the sun, the moon, the stars and the sky's glows aren't
     * lights for it), and no further than u.hand.w (dlss5_fx_cap): the glow lifts a pixel to that brightness at most, in
     * its own colour (a pixel the scene already has brighter is left as it is). A flame's glow is small and stays well
     * under it; Tu'Lia's beam of light fills a hall near white, and handed it at six times white DLSS 5 blew the hall out
     * and painted coloured blocks on it (0: no ceiling); and the effects' own picture (dlss5_fx_out): the finished scene
     * (input 0) less the scene before them (input 1). The gain is divided by how far the lights' mean (input 3, fx_dlssm)
     * is past u.hand.y (dlss5_fx_norm; 0 without): a big light's gain eases down to what a small one's gives */
    "float4 fx_dlssc(FO i) : SV_Target {\n"
    "  int2 p = int2(u.vp.xy + i.pos.xy);\n"
    "  float3 c = texel(0, p).rgb;\n"
    "  float k = u.hand.z != 0.0 && view_z(texel(2, p).r) != 0.0 ? u.hand.z : 0.0;\n"
    "  if (k != 0.0 && u.hand.y > 0.0) k /= max(texel(3, int2(0, 0)).x / u.hand.y, 1.0);\n"
    "  float3 g = k * tap(1, i.uv).rgb;\n"
    "  float lg = dot(g, LUMA), room = max(u.hand.w - dot(c, LUMA), 0.0);\n"
    "  if (u.hand.w > 0.0 && lg > room) g *= room / lg;\n"
    "  return float4(c + g, 1.0);\n"
    "}\n"
    "float4 fx_dlssd(FO i) : SV_Target {\n"
    "  int2 p = int2(u.vp.xy + i.pos.xy);\n"
    "  return float4(texel(0, p).rgb - texel(1, p).rgb, 0.0);\n"
    "}\n"
    /* the scene's depth (input 0) at the output's size, for the interface drawn over DLSS's output to test against:
     * each output pixel the render pixel under it (u.size: the output's size; u.vp.zw: the scene's), where the
     * frame's jitter (u.vp.xy, render pixels) moved the scene's draws to: the world is jittered, the names laid over the
     * output are not, so without it a name behind someone's arm flickered along the arm's edge, a render pixel a frame.
     * That still leaves the edge a render pixel wide that each frame's jitter samples differently (two output pixels at
     * DLSS's sizes): a name behind an arm wiggled along it, worst at a low frame rate (DLSS 5). So with the frame before's
     * (input 1, at the output's size: u.hist.x 1 when it's there) the depth is that one, held within what the render
     * pixels around this one hold now - kept along an edge, where they disagree, and this frame's elsewhere */
    "float fx_depthup(FO i) : SV_Depth {\n"
    "  float2 q = i.pos.xy * (u.vp.zw / u.size.xy) + u.vp.xy;\n"
    "  int2 c = int2(clamp(q, float2(0.0, 0.0), u.vp.zw - 1.0)), e = int2(u.vp.zw) - 1;\n"
    "  float z = texel(0, c).r;\n"
    "  if (u.hist.x == 0.0) return z;\n"
    "  float lo = z, hi = z;\n"
    "  [unroll] for (int y = -1; y <= 1; ++y)\n"
    "    [unroll] for (int x = -1; x <= 1; ++x) {\n"
    "      float t = texel(0, clamp(c + int2(x, y), int2(0, 0), e)).r;\n"
    "      lo = min(lo, t), hi = max(hi, t);\n"
    "    }\n"
    "  return clamp(texel(1, int2(i.pos.xy)).r, lo, hi);\n"
    "}\n"
    /* frame generation's frames (fgn_frame): the depth for the names drawn into them, the scene's at the output's size
     * (input 0, dlss_depth_up's) moved by the camera to where the made frame has it, u.hist.y of the way back toward the
     * frame before (u.reproj: this frame's clip space to the frame before's; u.zp.zw the viewport's MinZ, MaxZ; u.size.xy
     * the output's size). Each pixel takes the depth of the point that comes to it, found by going back along the
     * camera's motion from it, twice; the names are moved back by their own motion (fgn_frame), so one behind a head stays
     * behind it */
    "float2 namez_was(float2 q) {\n"
    "  float d = texel(0, clamp(int2(q), int2(0, 0), int2(u.size.xy) - 1)).r;\n"
    "  float4 c = mul(u.reproj, float4(q.x / u.size.x * 2.0 - 1.0, 1.0 - q.y / u.size.y * 2.0,\n"
    "                                  (d - u.zp.z) / max(u.zp.w - u.zp.z, 1e-6), 1.0));\n"
    "  if (!(c.w > 1e-6)) return q;\n"
    "  float2 n = c.xy / c.w;\n"
    "  return float2((n.x + 1.0) * 0.5 * u.size.x, (1.0 - n.y) * 0.5 * u.size.y);\n"
    "}\n"
    "float fx_namez(FO i) : SV_Depth {\n"
    "  float2 p = i.pos.xy, q = p;\n"
    "  [unroll] for (int k = 0; k < 2; ++k)\n"
    "    q = p + u.hist.y * (q - namez_was(q));\n"
    "  return texel(0, clamp(int2(q), int2(0, 0), int2(u.size.xy) - 1)).r;\n"
    "}\n"
    /* ... and the frame's interface over a made frame (blended SRC_ALPHA, 1 - alpha): the finished frame (input 0) where it
     * differs from the frame as the scene was laid onto it (input 1, the names on it), as frame generation laid it */
    "float4 fx_fgui(FO i) : SV_Target {\n"
    "  int2 p = int2(i.pos.xy);\n"
    "  float4 f = texel(0, p);\n"
    "  float3 d = abs(f.rgb - texel(1, p).rgb);\n"
    "  return float4(f.rgb, max(d.r, max(d.g, d.b)) > 0.0005 ? 1.0 : 0.0);\n"
    "}\n"
    /* FSR 1 (fsr1_run, gfx_d3d11.c up_fsr1_run): AMD's FidelityFX Super Resolution 1.0, ported from its ffx_fsr1.h (MIT,
     * Copyright (c) 2021 Advanced Micro Devices, Inc.; THIRD_PARTY_NOTICES.md) - EASU, its edge-adaptive upscale, then
     * RCAS, its sharpening; the 32-bit path, with taps by texel in place of its gathers. Both read input 0 within u.vp (its
     * rect in its texture: x, y, width, height) and draw the rect u.size (x, y, width, height in the target); RCAS's
     * strength, exp2(-stops), is in clarity's slot (u.clar.x). Each pixel's luma is AMD's (green plus half red and blue). */
    "float fsr_luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "float3 fsr_at(int2 p) { return texel(0, clamp(p, int2(u.vp.xy), int2(u.vp.xy + u.vp.zw) - 1)).rgb; }\n"
    /* the direction and length of the edge through one of the 2x2 taps nearest, by its + of five (a above, b left, c
     * itself, d right, e below), weighted w (its bilinear share) */
    "void easu_set(inout float2 dir, inout float len, float w, float la, float lb, float lc, float ld, float le) {\n"
    "  float dx = ld - lb, dy = le - la;\n"
    "  dir += float2(dx, dy) * w;\n"
    "  float lx = saturate(abs(dx) / max(max(abs(ld - lc), abs(lc - lb)), 1e-6));\n"
    "  float ly = saturate(abs(dy) / max(max(abs(le - lc), abs(lc - la)), 1e-6));\n"
    "  len += (lx * lx + ly * ly) * w;\n"
    "}\n"
    /* one of the 12 taps into the sum, by its lanczos-2 like weight along the edge (no sin or sqrt) */
    "void easu_tap(inout float3 ac, inout float aw, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {\n"
    "  float2 v = float2(off.x * dir.x + off.y * dir.y, off.y * dir.x - off.x * dir.y) * len;\n"
    "  float d2 = min(dot(v, v), clp);\n"
    "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
    "  wb *= wb; wa *= wa;\n"
    "  float w = (1.5625 * wb - 0.5625) * wa;\n"
    "  ac += c * w; aw += w;\n"
    "}\n"
    "float4 fx_easu(FO i) : SV_Target {\n"
    "  float2 pp = (i.pos.xy - u.size.xy) * (u.vp.zw / u.size.zw) - 0.5;\n"
    "  float2 fp = floor(pp);\n"
    "  pp -= fp;\n"
    "  int2 f0 = int2(fp + u.vp.xy);\n"
    /*    b c
     *  e f g h
     *  i j k l
     *    n o    */
    "  float3 tb = fsr_at(f0 + int2(0, -1)), tc = fsr_at(f0 + int2(1, -1));\n"
    "  float3 te = fsr_at(f0 + int2(-1, 0)), tf = fsr_at(f0), tg = fsr_at(f0 + int2(1, 0)), th = fsr_at(f0 + int2(2, 0));\n"
    "  float3 ti = fsr_at(f0 + int2(-1, 1)), tj = fsr_at(f0 + int2(0, 1)), tk = fsr_at(f0 + int2(1, 1)), tl = fsr_at(f0 + int2(2, 1));\n"
    "  float3 tn = fsr_at(f0 + int2(0, 2)), to = fsr_at(f0 + int2(1, 2));\n"
    "  float bl = fsr_luma(tb), cl = fsr_luma(tc), el = fsr_luma(te), fl = fsr_luma(tf), gl = fsr_luma(tg), hl = fsr_luma(th);\n"
    "  float il = fsr_luma(ti), jl = fsr_luma(tj), kl = fsr_luma(tk), ll = fsr_luma(tl), nl = fsr_luma(tn), ol = fsr_luma(to);\n"
    "  float2 dir = float2(0.0, 0.0);\n"
    "  float len = 0.0;\n"
    "  easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bl, el, fl, gl, jl);\n"
    "  easu_set(dir, len, pp.x * (1.0 - pp.y), cl, fl, gl, hl, kl);\n"
    "  easu_set(dir, len, (1.0 - pp.x) * pp.y, fl, il, jl, kl, nl);\n"
    "  easu_set(dir, len, pp.x * pp.y, gl, jl, kl, ll, ol);\n"
    "  float dr = dot(dir, dir);\n"
    "  bool none = dr < 1.0 / 32768.0;\n"
    "  dir = none ? float2(1.0, dir.y) : dir * rsqrt(max(dr, 1e-12));\n"
    "  len = len * 0.5;\n"
    "  len *= len;\n"
    /* the kernel stretched along the edge (to sqrt(2) on a diagonal) and squeezed across it, its window from about
     * sqrt(2) to beyond 2 the more edge there is */
    "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 + (0.21 - 0.5) * len;\n"
    "  float clp = 1.0 / lob;\n"
    "  float3 ac = float3(0.0, 0.0, 0.0);\n"
    "  float aw = 0.0;\n"
    "  easu_tap(ac, aw, float2(0.0, -1.0) - pp, dir, len2, lob, clp, tb);\n"
    "  easu_tap(ac, aw, float2(1.0, -1.0) - pp, dir, len2, lob, clp, tc);\n"
    "  easu_tap(ac, aw, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, ti);\n"
    "  easu_tap(ac, aw, float2(0.0, 1.0) - pp, dir, len2, lob, clp, tj);\n"
    "  easu_tap(ac, aw, float2(0.0, 0.0) - pp, dir, len2, lob, clp, tf);\n"
    "  easu_tap(ac, aw, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, te);\n"
    "  easu_tap(ac, aw, float2(1.0, 1.0) - pp, dir, len2, lob, clp, tk);\n"
    "  easu_tap(ac, aw, float2(2.0, 1.0) - pp, dir, len2, lob, clp, tl);\n"
    "  easu_tap(ac, aw, float2(2.0, 0.0) - pp, dir, len2, lob, clp, th);\n"
    "  easu_tap(ac, aw, float2(1.0, 0.0) - pp, dir, len2, lob, clp, tg);\n"
    "  easu_tap(ac, aw, float2(1.0, 2.0) - pp, dir, len2, lob, clp, to);\n"
    "  easu_tap(ac, aw, float2(0.0, 2.0) - pp, dir, len2, lob, clp, tn);\n"
    /* no ringing: kept within the four nearest's range; alpha the nearest's */
    "  float3 mn = min(min(tf, tg), min(tj, tk)), mx = max(max(tf, tg), max(tj, tk));\n"
    "  float3 c = min(mx, max(mn, ac / max(aw, 1e-6)));\n"
    "  int2 nq = int2(fp + u.vp.xy + step(0.5, pp));\n"
    "  return float4(c, texel(0, clamp(nq, int2(u.vp.xy), int2(u.vp.xy + u.vp.zw) - 1)).a);\n"
    "}\n"
    /* RCAS: each pixel and its + of four, sharpened by as much as the four's range lets it go without clipping (AMD's
     * limit, 1/4 - 1/16) */
    "float4 fx_rcas(FO i) : SV_Target {\n"
    "  int2 p = int2(floor(i.pos.xy - u.size.xy + u.vp.xy));\n"
    "  float4 e4 = texel(0, clamp(p, int2(u.vp.xy), int2(u.vp.xy + u.vp.zw) - 1));\n"
    "  float3 e = saturate(e4.rgb);\n"
    "  float3 b = saturate(fsr_at(p + int2(0, -1))), d = saturate(fsr_at(p + int2(-1, 0)));\n"
    "  float3 f = saturate(fsr_at(p + int2(1, 0))), h = saturate(fsr_at(p + int2(0, 1)));\n"
    "  float3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
    "  float3 hit_min = min(mn4, e) / max(4.0 * mx4, 1e-5);\n"
    "  float3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);\n"
    "  float3 lobes = max(-hit_min, hit_max);\n"
    "  float lobe = max(-0.1875, min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * u.clar.x;\n"
    "  return float4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), e4.a);\n"
    "}\n"
    /* the sky's colour, one texel (fog_color=sky): the average of the lowest sky pixel in each of 24 columns across the
     * top of the view (inputs: the scene, its depth, the last frame's colour), eased over the frames; held when there is
     * too little sky in view (looking down), and a = 0 till there has been some */
    "float4 fx_skycol(FO i) : SV_Target {\n"
    "  float3 acc = float3(0.0, 0.0, 0.0);\n"
    "  float n = 0.0;\n"
    "  for (int x = 0; x < 24; ++x) {\n"
    "    for (int y = 11; y >= 0; --y) {\n"
    "      float2 f = (float2(x, y) + 0.5) / float2(24.0, 12.0);\n"
    "      int2 q = int2(u.vp.xy + f * float2(u.vp.z, u.vp.w * 0.7));\n"
    "      if (view_z(texel(1, q).r) == 0.0) { acc += texel(0, q).rgb; n += 1.0; break; }\n"
    "    }\n"
    "  }\n"
    "  float4 last = texel(2, int2(0, 0));\n"
    "  if (n < 4.0) return last;\n"
    "  float3 now = acc / n;\n"
    "  return float4(last.a > 0.0 ? lerp(last.rgb, now, 0.04) : now, 1.0);\n"
    "}\n"
    /* depth of field (VanaCore). How blurred a point dist away is, 0 (sharp) .. 1, with the focus f
     * away: sharp within dof.y times f past it (and half that before it), then blurring over dof.z
     * times f; before it only with dof.w. The sky is as far as it gets, or sharp (dof2.w, dof_sky). */
    "float coc_of(float dist, float f) {\n"
    "  if (dist <= 0.0) return u.dof2.w > 0.0 ? 0.0 : 1.0;\n"
    "  float r = dist / max(f, 1e-3);\n"
    "  float far_ = saturate((r - 1.0 - u.dof.y) / max(u.dof.z, 1e-3));\n"
    "  float near_ = saturate((1.0 - 0.5 * u.dof.y - r) / 0.5) * u.dof.w;\n"
    "  return max(far_, near_);\n"
    "}\n"
    /* the focus distance, one texel: the nearest surface in a box a little below the screen's middle,
     * where FFXI's camera keeps the player, eased toward from the last frame's (dof2.y); or dof2.x when
     * it's fixed. Inputs: depth, the last frame's focus */
    "float4 fx_focus(FO i) : SV_Target {\n"
    "  if (u.dof2.x > 0.0) return float4(u.dof2.x, 0.0, 0.0, 0.0);\n"
    "  float best = 0.0;\n"
    "  [unroll] for (int y = 0; y < 5; ++y)\n"
    "    [unroll] for (int x = 0; x < 5; ++x) {\n"
    "      float2 px = u.vp.xy + float2(0.46 + 0.02 * x, 0.45 + 0.05 * y) * u.vp.zw;\n"
    "      float d = pos_at(0, px).z * u.hand.x;\n"
    "      if (d > 0.0 && (best == 0.0 || d < best)) best = d;\n"
    "    }\n"
    "  float prev = texel(1, int2(0, 0)).r;\n"
    "  if (!(prev > 0.0) || u.dof2.z > 0.0) prev = 0.0;\n"
    "  if (best == 0.0) return float4(prev > 0.0 ? prev : 10.0, 0.0, 0.0, 0.0);\n"
    "  return float4(prev > 0.0 ? lerp(prev, best, u.dof2.y) : best, 0.0, 0.0, 0.0);\n"
    "}\n"
    /* the scene at half size (one bilinear tap per 2x2), its blur amount in alpha; inputs the scene,
     * depth, the focus */
    "float4 fx_dofprep(FO i) : SV_Target {\n"
    "  float2 px = u.vp.xy + i.uv * u.vp.zw;\n"
    "  float3 c = tap(0, px / u.size.xy).rgb;\n"
    "  float dist = pos_at(1, px).z * u.hand.x;\n"
    "  return float4(c, coc_of(dist, texel(2, int2(0, 0)).r));\n"
    "}\n"
    /* a gather over a disc as wide as this pixel's blur, each sample counted only where its own blur
     * reaches this far, so the sharp player doesn't smear over the blurred scenery behind; the disc
     * turned by a 4x4 ordered pattern, the same every frame. Input: fx_dofprep's */
    "float4 fx_dofblur(FO i) : SV_Target {\n"
    "  float2 sz = size_of(0);\n"
    "  float4 c0 = tap(0, i.pos.xy / sz);\n"
    "  float R = c0.a * u.dof.x * 0.5;\n"
    "  if (R < 0.5) return c0;\n"
    "  const int NS = 40;\n"
    "  int2 cell = int2(i.pos.xy) & 3;\n"
    "  float k = (float(BAYER[cell.y * 4 + cell.x]) + 0.5) / 16.0;\n"
    "  float3 acc = c0.rgb;\n"
    "  float w = 1.0;\n"
    "  [loop] for (int s = 0; s < NS; ++s) {\n"
    "    float a = sqrt((float(s) + k) / float(NS));\n"
    "    float ang = float(s) * 2.3999632 + k * 6.2831853;\n"
    "    float2 o = float2(cos(ang), sin(ang)) * (a * R);\n"
    "    float4 q = tap(0, (i.pos.xy + o) / sz);\n"
    "    float wt = saturate(q.a * u.dof.x * 0.5 - a * R + 1.0);\n"
    "    acc += q.rgb * wt; w += wt;\n"
    "  }\n"
    "  return float4(acc / w, c0.a);\n"
    "}\n"
    /* the finished scene and its blurred copy, by each pixel's own blur amount; inputs the scene, the
     * blurred half-size copy, depth, the focus */
    "float4 fx_dofcomp(FO i) : SV_Target {\n"
    "  int2 px = int2(i.pos.xy);\n"
    "  float4 c = texel(0, px);\n"
    "  float coc = coc_of(view_z(texel(2, px).r) * u.hand.x, texel(3, int2(0, 0)).r);\n"
    "  if (int(u.grade.w) == 7) return float4(coc, coc, coc, c.a);\n"
    "  float3 b = tap(1, (i.pos.xy - u.vp.xy) / u.vp.zw).rgb;\n"
    "  return float4(lerp(c.rgb, b, saturate(coc * u.dof.x * 0.5)), c.a);\n"
    "}\n"
    /* FXAA on the finished scene (input 0, its copy): where the brightness changes sharply, the edge's
     * direction is found, followed both ways to its ends, and the pixel takes a sample moved across
     * the edge by how far along it lies (a staircase step becomes a slope); a pixel unlike all its
     * neighbours (a speck, a thin line) is pulled toward them. Brightness is the stored colour's. */
    "static const float FXAA_STEP[10] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 4.0, 8.0, 8.0 };\n"
    "float fxaa_luma(float2 p, float2 inv) { return dot(tap(0, p * inv).rgb, LUMA); }\n"
    "float4 fx_fxaa(FO i) : SV_Target {\n"
    "  float2 inv = 1.0 / size_of(0), p = i.pos.xy;\n"
    "  float4 c = tap(0, p * inv);\n"
    "  float m = dot(c.rgb, LUMA);\n"
    "  float n = fxaa_luma(p + float2(0, -1), inv), s = fxaa_luma(p + float2(0, 1), inv);\n"
    "  float w = fxaa_luma(p + float2(-1, 0), inv), e = fxaa_luma(p + float2(1, 0), inv);\n"
    "  float lo = min(m, min(min(n, s), min(w, e))), hi = max(m, max(max(n, s), max(w, e)));\n"
    "  float range = hi - lo;\n"
    "  if (range < max(0.0312, hi * 0.125)) return c;\n"
    "  float nw = fxaa_luma(p + float2(-1, -1), inv), ne = fxaa_luma(p + float2(1, -1), inv);\n"
    "  float sw = fxaa_luma(p + float2(-1, 1), inv), se = fxaa_luma(p + float2(1, 1), inv);\n"
    "  float avg = (2.0 * (n + s + w + e) + nw + ne + sw + se) / 12.0;\n"
    "  float sub = smoothstep(0.0, 1.0, saturate(abs(avg - m) / range));\n"
    "  sub = sub * sub * 0.75;\n"
    /* an edge along x changes brightness from row to row */
    "  float rows = 2.0 * abs(n - 2.0 * m + s) + abs(nw - 2.0 * w + sw) + abs(ne - 2.0 * e + se);\n"
    "  float cols = 2.0 * abs(w - 2.0 * m + e) + abs(nw - 2.0 * n + ne) + abs(sw - 2.0 * s + se);\n"
    "  bool horz = rows >= cols;\n"
    "  float l1 = horz ? n : w, l2 = horz ? s : e;\n"
    "  bool first = abs(l1 - m) >= abs(l2 - m);\n"
    "  float grad = 0.25 * max(abs(l1 - m), abs(l2 - m)), local = 0.5 * ((first ? l1 : l2) + m);\n"
    "  float2 across = horz ? float2(0, first ? -1 : 1) : float2(first ? -1 : 1, 0);\n"
    "  float2 along = horz ? float2(1, 0) : float2(0, 1);\n"
    /* on the edge, halfway to the neighbour across it; each way along until the brightness leaves it */
    "  float2 q = p + across * 0.5, a = q - along, b = q + along;\n"
    "  float ea = fxaa_luma(a, inv) - local, eb = fxaa_luma(b, inv) - local;\n"
    "  bool da = abs(ea) >= grad, db = abs(eb) >= grad;\n"
    "  [loop] for (int k = 1; k < 10 && !(da && db); ++k) {\n"
    "    if (!da) { a -= along * FXAA_STEP[k]; ea = fxaa_luma(a, inv) - local; da = abs(ea) >= grad; }\n"
    "    if (!db) { b += along * FXAA_STEP[k]; eb = fxaa_luma(b, inv) - local; db = abs(eb) >= grad; }\n"
    "  }\n"
    "  float ta = dot(p - a, along), tb = dot(b - p, along);\n"
    "  float end = ta < tb ? ea : eb, offset = 0.5 - min(ta, tb) / (ta + tb);\n"
    /* only on the side of the edge the nearer end turns away from */
    "  if ((end < 0.0) == (m < local)) offset = 0.0;\n"
    "  return float4(tap(0, (p + across * max(offset, sub)) * inv).rgb, c.a);\n"
    "}\n";

/* The scene effects' HLSL whole: its parts joined, once. Here VanaCore lays the zone lines after the world (fx_zlate,
 * zl_run), so the compose leaves them out (ZL_LATE); Direct3D 11 composes them, and is given it without (gfx_fx_hlsl) */
#ifdef VANACORE
static const char FX_PREFIX[] = "#define ZL_LATE 1\n";
#else
static const char FX_PREFIX[] = "";
#endif
static const char* fx_hlsl(void)
{
    static char whole[sizeof FX_PREFIX - 1 + sizeof FX_HLSL_1 + sizeof FX_HLSL_2 - 1];
    if (!whole[0])
    {
        memcpy(whole, FX_PREFIX, sizeof FX_PREFIX - 1);
        memcpy(whole + sizeof FX_PREFIX - 1, FX_HLSL_1, sizeof FX_HLSL_1 - 1);
        memcpy(whole + sizeof FX_PREFIX - 1 + sizeof FX_HLSL_1 - 1, FX_HLSL_2, sizeof FX_HLSL_2);
    }
    return whole;
}
/* What each of its entry points does, in words for the notice while it compiles (gfx_wait_step: "Compiling shaders" over
 * "Ambient occlusion"); the entry point's own name for one not here */
static const char* fx_words(const char* entry)
{
    static const struct
    {
        const char* entry;
        const char* words;
    } WORDS[] = {
        { "fx_vs", "Vertex shader" },
        { "fx_ao", "Ambient occlusion" },
        { "fx_blur", "Occlusion blur" },
        { "fx_bright", "Bloom threshold" },
        { "fx_down", "Bloom downsample" },
        { "fx_gauss", "Gaussian blur" },
        { "fx_raymask", "God rays mask" },
        { "fx_rays", "God rays" },
        { "fx_comp", "Scene composite" },
        { "fx_mip", "Scene mip chain" },
        { "fx_smooth", "Smoothing" },
        { "fx_focus", "Depth of field focus" },
        { "fx_dofprep", "Depth of field prepass" },
        { "fx_dofblur", "Depth of field blur" },
        { "fx_dofcomp", "Depth of field composite" },
        { "fx_fxaa", "Anti-aliasing" },
        { "fx_temporal", "Temporal filter" },
        { "fx_linz", "Linear depth" },
        { "fx_zmip", "Depth mip chain" },
        { "fx_glowb", "Glow blur" },
        { "fx_glowc", "Glow composite" },
        { "fx_glowg", "Glow over the grass" },
        { "fx_dlssz", "DLSS depth" },
        { "fx_dlssmv", "DLSS motion vectors" },
        { "fx_dlssr", "FSR plants' mask" },
        { "fx_cmask", "Characters' mask" },
        { "fx_dlssc", "DLSS colour" },
        { "fx_post", "Colour grading, last" },
        { "fx_refl", "Reflections and bounce light composite" },
        { "fx_lowpass", "Clarity low pass" },
        { "fx_clarity", "Clarity" },
        { "fx_ssr", "Reflections" },
        { "fx_gi", "Bounce light" },
        { "fx_gitemp", "Bounce light temporal filter" },
        { "fx_giblur", "Bounce light blur" },
        { "fx_elx", "Glow effects lighting" },
        { "fx_elc", "Glow effects lighting composite" },
        { "fx_zlate", "Zone lines" },
        { "fx_dlssd", "DLSS effects" },
        { "fx_dlssg", "DLSS 5 lights" },
        { "fx_dlssm", "DLSS 5 lights' mean" },
        { "fx_easu", "FSR 1 upscaling" },
        { "fx_rcas", "FSR 1 sharpening" },
        { "fx_depthup", "Interface depth" },
        { "fx_namez", "Names in frame generation's frames" },
        { "fx_skycol", "Sky colour" },
        { "sky_ps", "VanaCore's sky" },
        { "occl_test", "Occlusion test" },
    };
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; ++i)
        if (strcmp(WORDS[i].entry, entry) == 0)
            return WORDS[i].words;
    return entry;
}
#ifdef VANACORE
/* the Direct3D 11 back end's too, in its registers (gfx_d3d11.c fx_source): without FX_PREFIX, its compose has the lines */
const char* gfx_fx_hlsl(void)
{
    return fx_hlsl() + sizeof FX_PREFIX - 1;
}
const char* gfx_fx_words(const char* entry)
{
    return fx_words(entry);
}
#endif

typedef struct FxU
{
    float proj[4], zp[4], vp[4], size[4], ao[4], grade[4], hand[4], up[4], sun[4], suncol[4], sunuv[4], fogc[4], fogp[4],
        bloom[4], rays[4], toon[4], toon2[4], toon3[4], dof[4], dof2[4], shadow[4], lmat[16], smap[4], smap2[4], reproj[16],
        hist[4], lmatn[16], smapn[4], smapn2[4], sunsh[4], aoq[4], glow[4], glow2[4], zlc[4], zlcol[4], zlp[4], zl[64][4], grade2[4],
        refl[4], refl2[4], refl3[4], post[4], post2[4], grade3[4], post3[4], clar[4], bounce[4],
        elight[4], elight2[4], elight3[4], aor[4], aoc[4], aoc2[4], spc[4], spx[4], spz[4], spp[4], sp[3 * GFX_SPARKLES][4],
        post4[4], plight[4], plight2[4], // cppcheck-suppress unusedStructMember ; filled by gfx_player_light, read by the shader
        gmc[4], gmx[4], gmz[4], gm[4 * GFX_GROUND_MARKS][4], /* the rings of light on the ground (ground_marks) */
        rain[4], /* the rain's wet ground for the floor reflections (fx_ssr): how wet, rain.reflect, rain.puddles, its mask's slot + 1 */
        lmatm[16], smapm[4], smapm2[4]; /* the middle map (sun_mid, gfx_sun_mid.h) */
} FxU;

/* the sun's shadow map: depth, read through its SRV */
typedef struct SunMap
{
    ID3D12Resource* res;
    int32_t srv, dsv;
    D3D12_RESOURCE_STATES state;
    int failed;
    uint32_t size; /* texels across */
} SunMap;

/* the effects' own targets: one level, a heap slot for reading and one for drawing */
typedef struct FxTex
{
    ID3D12Resource* res;
    DXGI_FORMAT fmt;
    uint32_t w, h;
    int32_t srv, rtv;
    D3D12_RESOURCE_STATES state;
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
    FX_MIP,
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
    FX_DLSSZ,
    FX_DLSSMV,
    FX_DLSSC,
    FX_POST,
    FX_REFL,
    FX_LOWPASS,
    FX_CLARITY,
    FX_SSR,
    FX_GI,
    FX_GITEMP,
    FX_GIBLUR,
    FX_ELX,
    FX_ELC,
    FX_ZLATE,
    FX_DLSSD,
    FX_DLSSG,
    FX_DLSSM,
    FX_DLSSR,
    FX_CMASK,
    FX_GLOWG,
    FX_NPS,
};

static const char* const FX_ENTRY[FX_NPS] = { "fx_ao", "fx_blur", "fx_bright", "fx_down", "fx_gauss", "fx_raymask", "fx_rays",
    "fx_comp", "fx_mip", "fx_smooth", "fx_focus", "fx_dofprep", "fx_dofblur", "fx_dofcomp", "fx_fxaa", "fx_temporal", "fx_linz",
    "fx_zmip", "fx_glowb", "fx_glowc", "fx_dlssz", "fx_dlssmv", "fx_dlssc", "fx_post", "fx_refl", "fx_lowpass", "fx_clarity", "fx_ssr", "fx_gi", "fx_gitemp", "fx_giblur",
    "fx_elx", "fx_elc", "fx_zlate", "fx_dlssd", "fx_dlssg", "fx_dlssm", "fx_dlssr", "fx_cmask", "fx_glowg" };

#define FX_FORMATS 8

static struct
{
    volatile LONG state; /* 0 not built yet, 1 building, 2 ready, 3 failed */
    volatile LONG step, steps; /* fx_build's progress: steps made of steps (0: it hasn't started), for gfx_wait_progress */
    volatile PVOID at;         /* ... and the step it's on, in words (a literal's; NULL between builds), for gfx_wait_step */
    ID3DBlob* vs;
    ID3DBlob* ps[FX_NPS];
    ID3DBlob* depthup; /* fx_depthup: not one of the passes (the effects don't wait on it), NULL if it didn't compile */
    ID3DBlob *namez, *fgui; /* fx_namez, fx_fgui (fgn_frame): likewise - without them frame generation lays the names */
    ID3DBlob* skycol;  /* fx_skycol: likewise */
    ID3DBlob* truesky; /* VanaCore's sky (sky_ps, its own source: gfx_d3d12_sky.h): likewise - without it the game's dome stays */
    ID3DBlob *easu, *rcas; /* FSR 1's (fx_easu, fx_rcas): likewise - without them FSR 1 is off, nothing else */
    ID3D12PipelineState* skycol_pso;
    ID3D12PipelineState* glow_keep; /* VanaCore's glow into the frame's layer of them (gfx_glow: glow_kept), blended */
    ID3D12PipelineState* pso[FX_NPS]; /* FX_AO .. FX_RAYS: onto their own formats */
    struct
    {
        DXGI_FORMAT fmt;
        ID3D12PipelineState* comp;
        ID3D12PipelineState* mip;
        ID3D12PipelineState* smooth;
        ID3D12PipelineState* dofcomp;
        ID3D12PipelineState* fxaa;
        ID3D12PipelineState* glow; /* VanaCore's glow onto it, blended (gfx_glow) */
        ID3D12PipelineState* glowg; /* ... and again over the grass, premultiplied (gfx_glow_over_grass) */
        ID3D12PipelineState* post; /* the colour grade, laid last: onto the upscaler's output (a copy of it, graded), or the scene in place */
        ID3D12PipelineState* refl; /* reflections and bounce light over the composite */
        ID3D12PipelineState* clarity; /* local contrast over the composite */
        ID3D12PipelineState* elight;  /* the light from the effects over it (el_run) */
        ID3D12PipelineState* zlate;   /* the zone lines over it, after the game's late draws (zl_run), blended ONE, 1 - alpha */
        ID3D12PipelineState* easu;    /* FSR 1's upscale (fsr1_run), onto its output or between its passes */
        ID3D12PipelineState* rcas;    /* ... and its sharpening */
        ID3D12PipelineState* sky;     /* VanaCore's sky in place of the game's dome (gfx_sky) */
    } by_fmt[FX_FORMATS]; /* FX_COMP and FX_MIP draw onto the scene's target: one per format */
    uint32_t nfmt;
    FxTex src, ao0, ao1, b1a, b1b, b2a, b2b, ra, rb, sm;
    FxTex rl;      /* the reflections' layer, at the occlusion's size (fx_ssr) */
    FxTex cmask;   /* the characters' mask (cmask_pass), R8, the scene's size: white where a character's draw marked the stencil */
    uint64_t cmask_serial; /* the frame it was made for */
    FxTex amask[2]; /* ... taken small and blurred for an aura's glow (zl_run), across then down */
    FxTex gi, gic; /* the bounce light's layer, at its own smaller size (fx_gi), and the scene at that size to read its colours from */
    FxTex lz[4]; /* the occlusion's depth and its smaller levels (fx_linz) */
    FxTex glow[2]; /* VanaCore's glow's blur, across then down (gfx_glow) */
    FxTex glow_kept; /* ... the frame's glows as laid, premultiplied, at its size: laid again over the grass (gfx_glow_over_grass) */
    uint64_t glow_kept_serial; /* the frame it holds */
    FxTex cl[2];   /* clarity's blur (about a quarter size) */
    FxTex nrl[3];  /* DLSS 5's lights (dlss5_fx_light): the glow from the effects at a quarter of the viewport, then an eighth, blurred (and its across) */
    FxTex nrm[2];  /* ... and their mean, one texel, eased: this frame's and the last's (fx_dlssm, dlss5_fx_norm) */
    int nrm_i;     /* nrm[nrm_i] holds the last */
    uint64_t nrm_serial; /* the frame it's of */
    FxTex pp;      /* the scene (or the upscaler's output) copied to read, for the colour grade laid on it in place (grade_in_place) and
                    * for depth of field on the upscaler's output (dof_after) */
    FxTex up1[2];  /* FSR 1's passes before the last, at its output's size (fsr1_run) */
    FxTex sky[2];  /* the sky's colour, one texel: this frame's and the last (fx_skycol) */
    FxTex meas;    /* the composite drawn again, measured (comp_measure): let go once that stops */
    int sky_i;     /* sky[sky_i] holds the last */
    FxTex d0, d1, foc[2]; /* depth of field: half size, and the focus distance (this frame's, the last) */
    int foc_i;             /* foc[foc_i] holds the last frame's focus */
    uint64_t foc_ns;       /* when it was measured */
    /* what the fog and rays follow, eased from frame to frame (fx_ease): the game's values can
     * change between frames, and the effects should not pop with them */
    int eased; /* ease (the last scene was the frame before); else take the new values */
    uint64_t eased_serial;
    float fog_on, fogc[3], up[3], sun[3], suncol[3];
    int fogc_set;         /* fogc holds a colour to ease from (else the next is taken at once) */
    FxTex hist[2];        /* the occlusion and shadows after the temporal pass: this frame's and the one before */
    int hist_at;          /* which of hist[] the frame before wrote */
    uint64_t hist_serial; /* the frame it was written (0: none) */
    float prev_view[16], prev_proj[16], prev_cam[3];
    uint64_t prev_serial; /* the frame they are from (0: none) */
    FxTex gih[2];         /* the bounce light after its temporal pass: this frame's and the one before */
    int gih_at;           /* which of gih[] the frame before wrote */
    uint64_t gih_serial;  /* the frame it was written (0: none) */
    SunMap smap, smapn;   /* the sun's shadow maps: far, near */
    SunMap smaps;         /* the far map's still layer (sun_far_hold, g_still) */
    SunMap smapns;        /* the near map's (sun_near_hold, g_still_near) */
    SunMap smapsb, smapnsb; /* ... the maps their next layers are drawn into a slice a frame, and swapped with (sun_roll) */
    SunMap smapm, smapms, smapmsb; /* the middle map (sun_mid), its still layer (g_still_mid) and its layer's back map (gfx_sun_mid.h) */
    SunMap smapv;         /* this frame's characters' depth from the camera: which pixels are a character (sun_map_view) */
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
    /* the shadows' profile (FFXI_PROFILE): frames, frames with their own sun, with a map, the fewest
     * and most casters */
    uint32_t st_frames, st_own, st_map, st_cmin, st_cmax, st_drawn_this, st_cached;
    float st_across;
    /* the maps' cascades, for the flicker watch and the trace (sun_map): the radius of the sphere each map is cut through (the still
     * layer's where there is one; 0 no sharp map), the sharp map's reach, the player's distance from the camera (-1: none); and the
     * layers drawn again this scene (bit 0 the far map's, bit 1 the sharp map's) with why (sun_why_text's bits) */
    float st_far_r, st_near_r, st_reach, st_to;
    uint32_t st_redrawn, st_why_far, st_why_near;
    /* the trace (gfx_trace_dump): each scene done */
    uint32_t tr_live, tr_cached, tr_skipped, tr_nou, tr_depth; /* (nou: a kept caster left out, no room in the ring for its uniforms) */
    uint32_t tr_building; /* ... of the skipped, the ones whose pipelines are still building (not failed) */
    /* the last scene's casters, and those left out while their pipelines build, and its frame (gfx_waiting) */
    uint32_t wait_casters, wait_building;
    uint64_t wait_serial;
    float tr_strength, tr_day, tr_dl, tr_al, tr_fog[3];
    /* ... and the flicker watch's (fx_watch_frame): the effects ran; the frame before was blended in (1), or
     * why not (0 no shadows to blend, 2 none that frame, 3 another size, 4 the camera moved, 5 temporal off);
     * contact shadows' and god rays' strength; a lit draw's other light held back from the sun */
    uint32_t tr_ran, tr_hist, tr_held;
    float tr_rays;
    /* coming in gently (0-1): the effects' occlusion and shadows when they come on (their shaders just built, a
     * zone just loaded), and the shadows after the camera jumps (a new zone, a warp: the cache of the zone's
     * casters out of view starts empty and fills as they're seen, and the sun's light may still be settling);
     * the time and frame of the last scene with effects */
    float fade_in, shadows_in;
    uint64_t fade_ns, fade_serial;
    struct SunTrace { uint64_t serial; uint32_t live, cached, skipped, own, depth, ran, hist, held, redrawn, why_far, why_near; float fog[3], dl, al, strength, day, sun[3], cam[3], across, rays, far_r, near_r, reach, to; } trace[1200];
    uint32_t ntrace;
    /* the scene's depth before the grass (gfx_scene_world_depth): its copy, the depth it's of, and the frame */
    GfxTex* wdepth;
    GfxTex* wdepth_of;
    GfxTex* wdepth_glow; /* ... the depth it's of, copied for the glow laid again over the grass alone (wdepth_of NULL then) */
    uint64_t wdepth_serial;
    /* the zone's floors drawn as a layer this frame (gfx_scene_floors, floors.c): the kind of ground under each pixel (red) and
     * the floors' depth; why the floors weren't drawn (state); the frame they're of */
    GfxTex* floors_id;
    GfxTex* floors_depth;
    int floors_state;
    uint64_t floors_serial;
    /* the rain's wet ground this frame (gfx_scene_rain_mask, d3d8_rain.h): its mask, how wet, rain.reflect, rain.puddles */
    GfxTex* rain_mask;
    float rain_wet, rain_reflect, rain_puddles;
    uint64_t rain_serial;
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

/* every setting: its key in the file, FFXI_FX_<KEY> in the environment, and its default */
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
    { "fog", offsetof(FxSettings, fog), 0.004f },
    { "fog_falloff", offsetof(FxSettings, fog_falloff), 0.08f },
    { "fog_height", offsetof(FxSettings, fog_height), 2.0f },
    { "fog_floor", offsetof(FxSettings, fog_floor), 4.0f },
    { "fog_max", offsetof(FxSettings, fog_max), 0.5f },
    { "fog_sun", offsetof(FxSettings, fog_sun), 0.5f },
    { "fog_g", offsetof(FxSettings, fog_g), 0.6f },
    { "bloom", offsetof(FxSettings, bloom), 1.5f },
    { "threshold", offsetof(FxSettings, threshold), 0.85f },
    { "rays", offsetof(FxSettings, rays), 0.6f },
    { "rays_decay", offsetof(FxSettings, rays_decay), 0.965f },
    { "rays_length", offsetof(FxSettings, rays_length), 0.85f },
    { "light", offsetof(FxSettings, light), 1.0f },
    { "sun", offsetof(FxSettings, sun), 0.6f },
    { "sun_distance", offsetof(FxSettings, sun_distance), 150.0f },
    { "sun_soft", offsetof(FxSettings, sun_soft), 0.0f },
    { "sun_face", offsetof(FxSettings, sun_face), 1.0f },
    { "sun_min", offsetof(FxSettings, sun_min), 0.0f },
    { "sun_direct", offsetof(FxSettings, sun_direct), 0.5f },
    { "sun_dusk", offsetof(FxSettings, sun_dusk), 1.0f },
    { "sun_free", offsetof(FxSettings, sun_free), 0.0f },
    { "sun_free_az", offsetof(FxSettings, sun_free_az), 170.0f },
    { "sun_tilt", offsetof(FxSettings, sun_tilt), 15.0f }, /* the game's sun and moon tilted south (gfx_shared.c sun_tilt) */
    { "sun_moon", offsetof(FxSettings, sun_moon), 1.0f }, /* shadows from the moon at night (gfx_shared.c gfx_sun_light) */
    { "sun_game_shading", offsetof(FxSettings, sun_game_shading), 0.0f }, /* the maps alone shade (fx_ao's gate) */
    { "sun_free_el", offsetof(FxSettings, sun_free_el), 41.0f },
    { "sun_centre", offsetof(FxSettings, sun_centre), 1.0f },
    { "sun_gate", offsetof(FxSettings, sun_gate), 0.0f },
    { "sun_seek", offsetof(FxSettings, sun_seek), 1.0f },
    { "sun_cache", offsetof(FxSettings, sun_cache), 1.0f },
    { "sun_late", offsetof(FxSettings, sun_late), 0.0f },
    { "sun_sharp", offsetof(FxSettings, sun_sharp), 1.0f },
    { "sun_cull", offsetof(FxSettings, sun_cull), 0.0f },
#ifdef VANACORE
    { "sun_casters", offsetof(FxSettings, sun_casters), 0.0f }, /* the zone casts too */
#else
    { "sun_casters", offsetof(FxSettings, sun_casters), 1.0f },
#endif
    /* 25 until 2026-11-21: with the middle map off again, thin shadows past it were the far map's coarse texels and popped
     * in and out as its edge went by with the player walking (East Ronfaure [S]); registry.cpp's reset_old_defaults takes a
     * saved 25 out once */
    { "sun_near", offsetof(FxSettings, sun_near), 50.0f },
    { "sun_mid", offsetof(FxSettings, sun_mid), 0.0f }, /* off again (2026-11-21: GPU timeouts reported after 0.8.284 turned it on) */
    { "moghouse", offsetof(FxSettings, moghouse), 0.0f }, /* XI on Anything's default: none */
    { "temporal", offsetof(FxSettings, temporal), 0.99f },
    { "ao_quality", offsetof(FxSettings, ao_quality), 3.0f },
    { "ao_rings", offsetof(FxSettings, ao_rings), 1.0f },
    { "ao_ring_spacing", offsetof(FxSettings, ao_ring_spacing), 15.0f },
    { "ao_chars", offsetof(FxSettings, ao_chars), 1.25f },
    { "ao_soft_chars", offsetof(FxSettings, ao_soft_chars), 0.4f },
    { "sun_rings", offsetof(FxSettings, sun_rings), 1.0f },
    { "sun_ring_spacing", offsetof(FxSettings, sun_ring_spacing), 15.0f },
    { "sun_detail", offsetof(FxSettings, sun_detail), 4096.0f },
    { "sun_far_hold", offsetof(FxSettings, sun_far_hold), 3.0f },
    { "sun_near_hold", offsetof(FxSettings, sun_near_hold), 30.0f },
    { "sun_roll", offsetof(FxSettings, sun_roll), 0.0f }, /* off: a player's shadows strobed with it every 4 frames (2026-11-05) */
    { "sun_plants", offsetof(FxSettings, sun_plants), 1.0f },
    { "sun_plants_still", offsetof(FxSettings, sun_plants_still), 1.0f },
    { "sun_fade", offsetof(FxSettings, sun_fade), 1.0f },
    { "sun_smooth", offsetof(FxSettings, sun_smooth), 1.0f },
    { "sun_soft_chars", offsetof(FxSettings, sun_soft_chars), 0.0f },
    { "sun_soft_zone", offsetof(FxSettings, sun_soft_zone), 0.5f },
    { "sun_curved", offsetof(FxSettings, sun_curved), 1.0f },
    { "normalmap", offsetof(FxSettings, normalmap), 0.0f },
    { "normalmap_strength", offsetof(FxSettings, normalmap_strength), 1.0f },
    { "normalmap_view", offsetof(FxSettings, normalmap_view), 0.0f },
    { "normalmap_refl", offsetof(FxSettings, normalmap_refl), 1.0f },
    { "normalmap_fx", offsetof(FxSettings, normalmap_fx), 1.0f },
    { "debug", offsetof(FxSettings, debug), 0.0f },
#ifdef VANACORE
    { "outline", offsetof(FxSettings, outline), 0.0f },
    { "outline_width", offsetof(FxSettings, outline_width), 1.0f },
    { "outline_width_chars", offsetof(FxSettings, outline_width_chars), 0.0f },
    { "outline_depth", offsetof(FxSettings, outline_depth), 0.1f },
    { "outline_crease", offsetof(FxSettings, outline_crease), 0.25f },
    { "outline_fade", offsetof(FxSettings, outline_fade), 60.0f },
    { "outline_nograss", offsetof(FxSettings, outline_nograss), 1.0f },
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
    { "dlss_compare", offsetof(FxSettings, dlss_compare), 0.0f },
    { "names_native", offsetof(FxSettings, names_native), 0.0f },
    { "names_fg", offsetof(FxSettings, names_fg), 1.0f },
    { "dlss5", offsetof(FxSettings, dlss5), 0.0f },
    { "dlss5_style", offsetof(FxSettings, dlss5_style), 1.0f },
    { "dlss5_intensity", offsetof(FxSettings, dlss5_intensity), 1.0f },
    { "dlss5_tone", offsetof(FxSettings, dlss5_tone), 1.0f },
    { "dlss5_structure", offsetof(FxSettings, dlss5_structure), 1.0f },
    { "dlss5_skin", offsetof(FxSettings, dlss5_skin), -1.0f },
    { "dlss5_mask", offsetof(FxSettings, dlss5_mask), 1.0f },
    { "dlss5_passes", offsetof(FxSettings, dlss5_passes), 1.0f },
    { "dlss5_taper", offsetof(FxSettings, dlss5_taper), 0.5f },
    { "dlss5_mix", offsetof(FxSettings, dlss5_mix), 1.0f },
    { "dlss5_colour", offsetof(FxSettings, dlss5_colour), 1.0f },
    { "dlss5_brighten", offsetof(FxSettings, dlss5_brighten), 1.0f },
    { "dlss5_rebuild", offsetof(FxSettings, dlss5_rebuild), 0.0f },
    { "dlss5_scale", offsetof(FxSettings, dlss5_scale), 1.0f },
    { "dlss_preset", offsetof(FxSettings, dlss_preset), 0.0f },
    { "dlss5_preset", offsetof(FxSettings, dlss5_preset), 0.0f },
    { "dlss5_fx_out", offsetof(FxSettings, dlss5_fx_out), 0.0f },
    { "dlss5_fx_light", offsetof(FxSettings, dlss5_fx_light), 1.0f },
    { "dlss5_fx_gain", offsetof(FxSettings, dlss5_fx_gain), 5.0f },
    { "dlss5_fx_actor", offsetof(FxSettings, dlss5_fx_actor), 0.25f },
    { "dlss5_fx_cap", offsetof(FxSettings, dlss5_fx_cap), 1.25f },
    { "dlss5_fx_norm", offsetof(FxSettings, dlss5_fx_norm), 0.02f },
    { "dlss5_elight_post", offsetof(FxSettings, dlss5_elight_post), 1.0f },
    { "dlss_motion_all", offsetof(FxSettings, dlss_motion), 0.0f }, /* (dlss_motion before: a 1 saved then isn't taken up) */
    { "fg_fx_motion", offsetof(FxSettings, fg_fx_motion), 0.03f },
    { "dlss_motion_plants", offsetof(FxSettings, dlss_motion_plants), 0.0f },
    { "dlss_motion_chars", offsetof(FxSettings, dlss_motion_chars), 1.0f },
    { "dlss_motion_batch", offsetof(FxSettings, dlss_motion_batch), 1.0f },
    { "dlss_reactive", offsetof(FxSettings, dlss_reactive), 0.8f },
    { "dlss_reactive_moving", offsetof(FxSettings, dlss_reactive_moving), 0.5f },
    { "refl", offsetof(FxSettings, refl), 0.0f },
    { "refl_wet", offsetof(FxSettings, refl_wet), 0.5f },
    { "refl_rough", offsetof(FxSettings, refl_rough), 0.3f },
    { "refl_dist", offsetof(FxSettings, refl_dist), 40.0f },
    { "refl_quality", offsetof(FxSettings, refl_quality), 3.0f },
    { "refl_sun", offsetof(FxSettings, refl_sun), 1.0f },
    { "refl_floors", offsetof(FxSettings, refl_floors), 1.0f },
    { "refl_bump", offsetof(FxSettings, refl_bump), 1.3f },
    { "refl_lift", offsetof(FxSettings, refl_lift), 0.15f },
    { "gi", offsetof(FxSettings, gi), 0.0f },
    { "gi_radius", offsetof(FxSettings, gi_radius), 5.0f },
    { "gi_quality", offsetof(FxSettings, gi_quality), 1.09f },
    { "midtones", offsetof(FxSettings, midtones), 0.0f },
    { "whites", offsetof(FxSettings, whites), 0.0f },
    { "rolloff", offsetof(FxSettings, rolloff), 0.0f },
    { "light_tint", offsetof(FxSettings, light_tint), 0.0f },
    { "clarity", offsetof(FxSettings, clarity), 0.0f },
    { "night_moon", offsetof(FxSettings, night_moon), 0.5f },
    { "day_night_warmth", offsetof(FxSettings, day_night_warmth), 0.0f },
    { "player_light", offsetof(FxSettings, player_light), 0.0f },
    { "player_light_radius", offsetof(FxSettings, player_light_radius), 8.0f },
    { "player_light_warm", offsetof(FxSettings, player_light_warm), 0.5f },
    { "player_light_day", offsetof(FxSettings, player_light_day), 0.5f },
    { "glow_strength", offsetof(FxSettings, glow_strength), 1.0f },
    { "glow_size", offsetof(FxSettings, glow_size), 1.0f },
    { "glow_quality", offsetof(FxSettings, glow_quality), 1.0f },
    { "brightness", offsetof(FxSettings, brightness), 0.0f },
    { "shadows", offsetof(FxSettings, shadows), 0.0f },
    { "highlights", offsetof(FxSettings, highlights), 0.0f },
    { "warmth", offsetof(FxSettings, warmth), 0.0f },
    { "hue", offsetof(FxSettings, hue), 0.0f },
    { "fog_mode", offsetof(FxSettings, fog_mode), 0.0f },
    { "fog_cr", offsetof(FxSettings, fog_cr), 0.72f },
    { "fog_cg", offsetof(FxSettings, fog_cg), 0.78f },
    { "fog_cb", offsetof(FxSettings, fog_cb), 0.86f },
    { "elight", offsetof(FxSettings, elight), 2.5f },
    { "elight_radius", offsetof(FxSettings, elight_radius), 30.0f },
    { "elight_warp", offsetof(FxSettings, elight_warp), 0.2f },
    { "elight_lantern", offsetof(FxSettings, elight_lantern), 0.5f },
    { "elight_dim", offsetof(FxSettings, elight_dim), 0.2f },
    { "elight_actor", offsetof(FxSettings, elight_actor), 0.4f },
#endif
};

/* a quality setting - low, medium, high, ultra, max - as 0 to 4 */
static int fx_level(float q)
{
    return q < 0.5f ? 0 : q > 3.5f ? 4 : (int)(q + 0.5f);
}

/* a table of the five levels read at q (0 to 4), straight between its entries: a quality between the whole numbers */
static float fx_lerp5(const float t[5], float q)
{
    q = fminf(fmaxf(q, 0.0f), 4.0f);
    const int i = (int)q;
    return i >= 4 ? t[4] : t[i] + (t[i + 1] - t[i]) * (q - (float)i);
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
            : !strcmp(dbg, "rays") ? 4.0f : !strcmp(dbg, "shadow") ? 5.0f : !strcmp(dbg, "gi") ? 8.0f : (float)atof(dbg);
    const char* file = getenv("FFXI_FX_FILE");
    if (file && *file)
        snprintf(g_fx_file, sizeof g_fx_file, "%s", file);
#ifndef VANACORE /* VanaCore sets them from vanacore.ini itself */
    else if (getenv("LOCALAPPDATA"))
        snprintf(g_fx_file, sizeof g_fx_file, "%s\\FFXI\\fx.txt", getenv("LOCALAPPDATA"));
#endif
}

/* a pipeline for one pass: the full-screen triangle into one target of this format, no blending */
/* blend 1: over what's there by the function's alpha; 2: its colour premultiplied (what's there times 1 - alpha, plus it) */
static ID3D12PipelineState* fx_make_pso_ex(ID3DBlob* ps, DXGI_FORMAT fmt, int blend)
{
    if (!g_fx.vs || !ps)
        return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = g_root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g_fx.vs), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(g_fx.vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
    c->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
    c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
    c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    c->LogicOp = D3D12_LOGIC_OP_NOOP;
    if (blend)
    {
        c->BlendEnable = TRUE;
        c->SrcBlend = blend == 2 ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA, c->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        c->SrcBlendAlpha = D3D12_BLEND_ZERO, c->DestBlendAlpha = D3D12_BLEND_ONE;
        if (blend != 2)
            c->SrcBlendAlpha = D3D12_BLEND_ONE, c->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    }
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = NULL;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p)))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: a scene-effect pipeline failed (format %d)\n", (int)fmt);
        return NULL;
    }
    return p;
}

static ID3D12PipelineState* fx_make_pso(ID3DBlob* ps, DXGI_FORMAT fmt) { return fx_make_pso_ex(ps, fmt, 0); }

#ifdef VANACORE
/* the characters' mask's pipeline (fx_cmask): into an R8 target, drawn where the scene's stencil has the characters' bit
 * (CHAR_STENCIL: their draws wrote it), the depth-stencil bound read-only; no depth test. NULL if it can't be made: the
 * characters' depth drawn again does it then (cmask_usable) */
static ID3D12PipelineState* fx_make_pso_cmask(ID3DBlob* ps)
{
    if (!g_fx.vs || !ps)
        return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = g_root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g_fx.vs), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(g_fx.vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
    c->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
    c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
    c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    c->LogicOp = D3D12_LOGIC_OP_NOOP;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = TRUE;
    pd.DepthStencilState.StencilReadMask = CHAR_STENCIL;
    pd.DepthStencilState.StencilWriteMask = 0;
    D3D12_DEPTH_STENCILOP_DESC st = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL };
    pd.DepthStencilState.FrontFace = st;
    pd.DepthStencilState.BackFace = st;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = NULL;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p)))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: the characters' mask pipeline failed; their share of the occlusion uses their depth drawn again\n");
        return NULL;
    }
    return p;
}
#endif

/* a pipeline that writes the depth of a pass's pixel shader (SV_Depth) into a depth target of this format: the
 * full-screen triangle, always passing, no colour target */
static ID3D12PipelineState* fx_make_depth_pso(ID3DBlob* ps, DXGI_FORMAT dsv)
{
    if (!g_fx.vs || !ps)
        return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = g_root;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g_fx.vs), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(g_fx.vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 0;
    pd.DSVFormat = dsv;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* p = NULL;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p)))
    {
        InterlockedIncrement(&g_failures);
        fprintf(stderr, "[recomp] gfx: the depth pipeline failed (format %d)\n", (int)dsv);
        return NULL;
    }
    return p;
}

/* the comp and mip pipelines onto a scene target of this format: made on first use, kept */
static int fx_by_fmt(DXGI_FORMAT fmt)
{
    for (uint32_t i = 0; i < g_fx.nfmt; ++i)
        if (g_fx.by_fmt[i].fmt == fmt)
            return (int)i;
    if (g_fx.nfmt == FX_FORMATS)
        return -1;
    uint32_t i = g_fx.nfmt;
    g_fx.by_fmt[i].fmt = fmt;
    g_fx.by_fmt[i].comp = fx_make_pso(g_fx.ps[FX_COMP], fmt);
    g_fx.by_fmt[i].mip = fx_make_pso(g_fx.ps[FX_MIP], fmt);
    g_fx.by_fmt[i].smooth = fx_make_pso(g_fx.ps[FX_SMOOTH], fmt);
    g_fx.by_fmt[i].dofcomp = fx_make_pso(g_fx.ps[FX_DOFCOMP], fmt);
    g_fx.by_fmt[i].fxaa = fx_make_pso(g_fx.ps[FX_FXAA], fmt);
    g_fx.by_fmt[i].glow = fx_make_pso_ex(g_fx.ps[FX_GLOWC], fmt, 1);
    g_fx.by_fmt[i].glowg = fx_make_pso_ex(g_fx.ps[FX_GLOWG], fmt, 2);
    g_fx.by_fmt[i].post = fx_make_pso(g_fx.ps[FX_POST], fmt);
    g_fx.by_fmt[i].refl = fx_make_pso(g_fx.ps[FX_REFL], fmt);
    g_fx.by_fmt[i].clarity = fx_make_pso(g_fx.ps[FX_CLARITY], fmt);
    g_fx.by_fmt[i].elight = fx_make_pso(g_fx.ps[FX_ELC], fmt);
    g_fx.by_fmt[i].zlate = fx_make_pso_ex(g_fx.ps[FX_ZLATE], fmt, 2);
    g_fx.by_fmt[i].easu = fx_make_pso(g_fx.easu, fmt);
    g_fx.by_fmt[i].rcas = fx_make_pso(g_fx.rcas, fmt);
    g_fx.by_fmt[i].sky = fx_make_pso(g_fx.truesky, fmt);
    g_fx.nfmt = i + 1; /* after the pipelines: the game's thread reads by_fmt only up to nfmt */
    return (int)i;
}

static int fx_ready(void);

/* the pipeline that makes a level of a chain of this format from the one above (scene_mips), or NULL */
static ID3D12PipelineState* fx_mip_pso(DXGI_FORMAT fmt)
{
    int i = fx_ready() ? fx_by_fmt(fmt) : -1;
    return i >= 0 ? g_fx.by_fmt[i].mip : NULL;
}

static const char* sky_hlsl(void); /* VanaCore's sky's own source (gfx_d3d12_sky.h) */
static void occl_build(void);      /* the occlusion test's shaders and pipelines (gfx_d3d12_occl.h) */

/* the shaders and pipelines: on the thread pool the first time a scene asks (the game never waits
 * for the compiler), or in place for the tests */
static void fx_build(void)
{
    gfx_fx_build_begin("d3d12");
    /* the vertex shader, each pixel shader, the pipelines made from them (one step), FSR 1's two, VanaCore's sky, the scene
     * target's pipelines, the depth's, the names' for frame generation and the sky's colour */
    InterlockedExchange(&g_fx.step, 0);
    InterlockedExchange(&g_fx.steps, FX_NPS + 10);
#define FX_AT(words) InterlockedExchangePointer(&g_fx.at, (PVOID)(words))
    const char* src = fx_hlsl();
    FX_AT(fx_words("fx_vs"));
    int ok = (g_fx.vs = compile(src, "fx_vs", "vs_5_1")) != NULL;
    InterlockedIncrement(&g_fx.step);
    for (int i = 0; ok && i < FX_NPS; ++i)
    {
        FX_AT(fx_words(FX_ENTRY[i]));
        ok = (g_fx.ps[i] = compile(src, FX_ENTRY[i], "ps_5_1")) != NULL;
        InterlockedIncrement(&g_fx.step);
    }
    FX_AT("Effect pipelines");
    for (int i = 0; ok && i <= FX_RAYS; ++i)
        ok = (g_fx.pso[i] = fx_make_pso(g_fx.ps[i], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_TEMPORAL] = fx_make_pso(g_fx.ps[FX_TEMPORAL], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_FOCUS] = fx_make_pso(g_fx.ps[FX_FOCUS], DXGI_FORMAT_R32_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_LINZ] = fx_make_pso(g_fx.ps[FX_LINZ], DXGI_FORMAT_R32_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_ZMIP] = fx_make_pso(g_fx.ps[FX_ZMIP], DXGI_FORMAT_R32_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_GLOWB] = fx_make_pso(g_fx.ps[FX_GLOWB], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_LOWPASS] = fx_make_pso(g_fx.ps[FX_LOWPASS], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DLSSZ] = fx_make_pso(g_fx.ps[FX_DLSSZ], DXGI_FORMAT_R32_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DLSSMV] = fx_make_pso(g_fx.ps[FX_DLSSMV], DXGI_FORMAT_R16G16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DLSSR] = fx_make_pso(g_fx.ps[FX_DLSSR], DXGI_FORMAT_R8_UNORM)) != NULL;
    ok = ok && (g_fx.pso[FX_DLSSC] = fx_make_pso(g_fx.ps[FX_DLSSC], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DLSSD] = fx_make_pso(g_fx.ps[FX_DLSSD], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DLSSG] = fx_make_pso(g_fx.ps[FX_DLSSG], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DLSSM] = fx_make_pso(g_fx.ps[FX_DLSSM], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DOFPREP] = fx_make_pso(g_fx.ps[FX_DOFPREP], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_DOFBLUR] = fx_make_pso(g_fx.ps[FX_DOFBLUR], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_SSR] = fx_make_pso(g_fx.ps[FX_SSR], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_GI] = fx_make_pso(g_fx.ps[FX_GI], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_GITEMP] = fx_make_pso(g_fx.ps[FX_GITEMP], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_GIBLUR] = fx_make_pso(g_fx.ps[FX_GIBLUR], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    ok = ok && (g_fx.pso[FX_ELX] = fx_make_pso(g_fx.ps[FX_ELX], DXGI_FORMAT_R16G16B16A16_FLOAT)) != NULL;
    if (ok) /* left NULL, the glow isn't laid again over the grass */
        g_fx.glow_keep = fx_make_pso_ex(g_fx.ps[FX_GLOWC], DXGI_FORMAT_R16G16B16A16_FLOAT, 1);
#ifdef VANACORE
    if (ok) /* not needed by the others: left NULL, the characters' share of the occlusion has its older way */
        g_fx.pso[FX_CMASK] = fx_make_pso_cmask(g_fx.ps[FX_CMASK]);
#endif
    InterlockedIncrement(&g_fx.step);
    if (ok)
    {
        FX_AT(fx_words("fx_easu"));
        g_fx.easu = compile(src, "fx_easu", "ps_5_1");
        InterlockedIncrement(&g_fx.step);
        FX_AT(fx_words("fx_rcas"));
        g_fx.rcas = compile(src, "fx_rcas", "ps_5_1");
        InterlockedIncrement(&g_fx.step);
        if (!g_fx.easu || !g_fx.rcas)
            fprintf(stderr, "[recomp] gfx: FSR 1's passes didn't compile; FSR 1 is off\n");
    }
    else
        InterlockedExchangeAdd(&g_fx.step, 2);
    FX_AT(fx_words("sky_ps")); /* before the scene target's pipelines, which take its own */
    if (ok && !(g_fx.truesky = compile(sky_hlsl(), "sky_ps", "ps_5_1")))
        fprintf(stderr, "[recomp] gfx: VanaCore's sky didn't compile; the game's sky stays\n");
    InterlockedIncrement(&g_fx.step);
    FX_AT(fx_words("occl_test"));
    if (ok)
        occl_build();
    InterlockedIncrement(&g_fx.step);
    FX_AT("Scene target pipelines");
    if (ok) /* the usual scene target's, ready before it is needed */
        fx_by_fmt(DXGI_FORMAT_B8G8R8A8_UNORM);
    InterlockedIncrement(&g_fx.step);
    FX_AT(fx_words("fx_depthup"));
    if (ok && !(g_fx.depthup = compile(src, "fx_depthup", "ps_5_1")))
        fprintf(stderr, "[recomp] gfx: the depth for the interface over DLSS didn't compile; it is drawn without a depth test\n");
    InterlockedIncrement(&g_fx.step);
    FX_AT(fx_words("fx_namez"));
    if (ok && (!(g_fx.namez = compile(src, "fx_namez", "ps_5_1")) || !(g_fx.fgui = compile(src, "fx_fgui", "ps_5_1"))))
        fprintf(stderr, "[recomp] gfx: the names' passes for frame generation didn't compile; its frames have the names where "
                        "the next frame has them\n");
    InterlockedIncrement(&g_fx.step);
    FX_AT(fx_words("fx_skycol"));
    if (ok && (g_fx.skycol = compile(src, "fx_skycol", "ps_5_1")) != NULL)
        g_fx.skycol_pso = fx_make_pso(g_fx.skycol, DXGI_FORMAT_R16G16B16A16_FLOAT);
    else if (ok)
        fprintf(stderr, "[recomp] gfx: the sky's colour for the fog didn't compile; the fog keeps the zone's colour\n");
    InterlockedIncrement(&g_fx.step);
    FX_AT(NULL);
#undef FX_AT
    gfx_fx_build_end(ok);
    InterlockedExchange(&g_fx.state, ok ? 2 : 3);
}

static void CALLBACK fx_build_job(PTP_CALLBACK_INSTANCE inst, void* ctx)
{
    (void)inst, (void)ctx;
    fx_build();
}

/* The effects' fade in for this scene (g_fx.fade_in): over 0.6 s after they haven't run for half a second (never yet,
 * a zone loading: frames went by without them); the shadows' over 1.2 s after the camera's jump (sun_cache_update).
 * Multiplies the occlusion's strength; the shadows take both. One slow frame between two with effects (a hitch: a
 * screenshot, a file loading) isn't a gap: faded out by it, the scene flashed bright as the occlusion and shadows
 * came back. */
static float fx_fade_in(void)
{
    if (g_sync_pipelines) /* the tests: each frame as it is, checked as soon as it's drawn */
        return g_fx.fade_in = g_fx.shadows_in = 1.0f;
    const uint64_t now = gfx_now_ns();
    const float dt = g_fx.fade_ns && now > g_fx.fade_ns ? (float)((double)(now - g_fx.fade_ns) * 1e-9) : 1.0f;
    if (!g_fx.fade_ns || (dt > 0.5f && g_serial > g_fx.fade_serial + 1))
        g_fx.fade_in = 0.0f, g_fx.shadows_in = 0.0f;
    g_fx.fade_ns = now, g_fx.fade_serial = g_serial;
    const float step = fminf(dt, 0.1f);
    g_fx.fade_in = fminf(g_fx.fade_in + step / 0.6f, 1.0f);
    g_fx.shadows_in = fminf(g_fx.shadows_in + step / 1.2f, g_fx.fade_in);
    return g_fx.fade_in;
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

/* an effect target of this size and format in *t, made again when either changes */
static int fx_tex(FxTex* t, DXGI_FORMAT fmt, uint32_t w, uint32_t h)
{
    if (t->res && t->w == w && t->h == h && t->fmt == fmt)
        return 1;
    if (t->res)
    {
        defer((IUnknown*)t->res, NULL, -1);
        defer(NULL, &g_srv, t->srv);
        defer(NULL, &g_rtv, t->rtv);
    }
    memset(t, 0, sizeof *t);
    t->srv = t->rtv = -1;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, fmt, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            &IID_ID3D12Resource, (void**)&t->res)))
    {
        t->res = NULL;
        return 0;
    }
    t->fmt = fmt, t->w = w, t->h = h, t->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    t->srv = heap_alloc(&g_srv), t->rtv = heap_alloc(&g_rtv);
    if (t->srv < 0 || t->rtv < 0)
        return 0;
    ID3D12Device_CreateShaderResourceView(g_dev, t->res, NULL, heap_cpu(&g_srv, t->srv));
    ID3D12Device_CreateRenderTargetView(g_dev, t->res, NULL, heap_cpu(&g_rtv, t->rtv));
    return 1;
}

static void fx_state(FxTex* t, D3D12_RESOURCE_STATES want)
{
    if (t->state != want)
    {
        barrier(t->res, t->state, want);
        t->state = want;
    }
}

/* --- the present queue (g_pqueue): the frames onto the screen ------------------------------------------------- */
static ID3D12CommandQueue* present_queue(void) { return g_pqueue ? g_pqueue : g_queue; }

/* till g_pfence reaches v (the present queue past a copy), at most ms (a lost device's fences read done) */
static void pfence_wait(uint64_t v, DWORD ms)
{
    const uint64_t until = gfx_now_ns() + (uint64_t)ms * 1000000u;
    while (g_pfence && ID3D12Fence_GetCompletedValue(g_pfence) < v)
    {
        const uint64_t now = gfx_now_ns();
        if (now >= until)
            return;
        ID3D12Fence_SetEventOnCompletion(g_pfence, v, g_pfence_event);
        WaitForSingleObject(g_pfence_event, (DWORD)((until - now) / 1000000u) + 1);
    }
}

/* the present queue done with everything handed to it: before the swap chain's buffers go (swap_fit), with the textures
 * it copies from (they're sized to the swap chain, made again after) */
static void pqueue_idle(void)
{
    if (!g_pqueue || !g_pfence)
        return;
    ID3D12CommandQueue_Signal(g_pqueue, g_pfence, ++g_pvalue);
    pfence_wait(g_pvalue, 5000);
}

/* the copies' command list (one, reset with the caller's allocator), and the plain present's frames (gfx_present) */
static struct
{
    ID3D12GraphicsCommandList* list;
    FxTex tex[FRAMES];          /* the frame as drawn for the window, each frame's in turn */
    uint64_t copied[FRAMES];    /* g_pfence's value once each was copied into the swap chain (0: never) */
    ID3D12CommandAllocator* alloc[FRAMES];
    uint64_t alloc_done[FRAMES];
    int next;
} g_show;

static int show_init(void)
{
    if (g_show.list)
        return 1;
    for (int j = 0; j < FRAMES; ++j)
        if (!g_show.alloc[j] && FAILED(ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           &IID_ID3D12CommandAllocator, (void**)&g_show.alloc[j])))
            return g_show.alloc[j] = NULL, 0;
    if (FAILED(ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_show.alloc[0], NULL,
            &IID_ID3D12GraphicsCommandList, (void**)&g_show.list)))
        return g_show.list = NULL, 0;
    ID3D12GraphicsCommandList_Close(g_show.list);
    if (g_dred)
        ID3D12GraphicsCommandList_SetName(g_show.list, L"VanaCore's present");
    return 1;
}

/* The frame t (drawn on the game's queue, left in the pixel shaders' state, drawn once g_fence reaches `ready`) copied
 * into the swap chain's current buffer and presented, on the present queue. The copy is recorded with alloc, free again
 * once g_pfence reaches *alloc_done (set to this copy's value). Returns g_pfence's value for the copy, for the game's
 * queue to wait on before it draws into t again; 0 when nothing was shown. From one thread at a time: the game's, or
 * frame generation's present thread while that shows the frames (fg_wait_idle between). */
static uint64_t show_present(FxTex* t, uint64_t ready, ID3D12CommandAllocator* alloc, uint64_t* alloc_done, const char* where)
{
    if (!g_swap || !g_show.list || !t->res || g_lost_said)
        return 0;
    pfence_wait(*alloc_done, 1000);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(g_show.list, alloc, NULL);
    const UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(g_swap);
    D3D12_RESOURCE_BARRIER b[2];
    memset(b, 0, sizeof b);
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = g_swap_buf[i];
    b[1].Transition.pResource = t->res;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = g_swap_state[i], b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(g_show.list, 2, b);
    ID3D12GraphicsCommandList_CopyResource(g_show.list, g_swap_buf[i], t->res);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST, b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE, b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(g_show.list, 2, b);
    ID3D12GraphicsCommandList_Close(g_show.list);
    ID3D12CommandQueue* q = present_queue();
    ID3D12CommandList* cl = (ID3D12CommandList*)g_show.list;
    ID3D12CommandQueue_Wait(q, g_fence, ready); /* the frame drawn (the game's queue signals it at its submission) */
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, &cl);
    ID3D12CommandQueue_Signal(q, g_pfence, ++g_pvalue);
    *alloc_done = g_pvalue;
    g_swap_state[i] = D3D12_RESOURCE_STATE_PRESENT;
    const HRESULT hr = IDXGISwapChain3_Present(g_swap, g_vsync ? 1 : 0, !g_vsync && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        gpu_lost(where, gpu_log_line);
    return g_pvalue;
}

/* The normal-map detail target: the scene's size, RGBA16F, the bent view-space normal in rgb and the view z in a, 0 where no
 * normal-mapped surface was drawn. The effects take their normal from it, where its z is the depth buffer's (a nearer draw
 * without a map since hides the one it holds), instead of working one out from the depth. Cleared by the first draw into it
 * after the effects read it; read through u.clar (y: its heap slot, z: reflections use it, w: the other effects do). */
static FxTex g_nd;
static int g_nd_clear = 1, g_nd_valid;

static D3D12_CPU_DESCRIPTOR_HANDLE nout_view(void) { return heap_cpu(&g_rtv, g_nd.rtv); }

static int nout_wanted(const GfxDraw* d)
{
    if (g_fxs.fx == 0.0f || g_fxs.normalmap < 0.5f || !d->fs.nmap || g_rt_face || g_rt_level || !depth_attachment())
        return 0;
    if (!(g_fxs.normalmap_refl >= 0.5f && g_fxs.refl > 0.0f) && !(g_fxs.normalmap_fx >= 0.5f))
        return 0;
    if (d->fs.prog)
        return d->vs.prog != 0;
    return g_fxs.light != 0.0f && d->vs.lighting && !d->vs.rhw && !d->vs.prog && !d->vs.flat;
}

static int nout_begin(void)
{
    uint32_t w, h;
    color_size(&w, &h);
    if (!w || !h || !fx_tex(&g_nd, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h))
        return 0;
    fx_state(&g_nd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (g_nd_clear)
    {
        const float zero[4] = { 0, 0, 0, 0 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list(), heap_cpu(&g_rtv, g_nd.rtv), zero, 0, NULL);
        g_nd_clear = 0;
    }
    g_nd_valid = 1;
    return 1;
}

/* The scene's own smooth normals for the sun's shading (sun_smooth, always on in the game; sun_map). The shadow maps go by the
 * polygons: a low-polygon rock under a low sun was shaded from the very edge between a face lit and one turned away, a hard
 * line, where the game's own smooth normals light it on past that edge and turn it from the sun across its faces. Once the
 * scene is done, its casters (each opaque draw, as kept for the sun's maps: caster_new) are drawn again into the detail
 * target against its finished depth - only what shows - each with its interpolated normal in view space: a lit
 * fixed-function draw's (most of a zone - Qufim's rock and snow - and the characters: the game lights them by it, per pixel
 * with the light setting), and a zone shader's (a vertex shader with a float3 normal input: its pieces' baked colour, no
 * light, so the sun is all the maps' and this). Drawn from what the casters keep - their uniforms in the frame's ring, their
 * streams and indices, an alpha test's textures - as the sun's maps draw them: a second draw encoded for each as the game
 * drew it took Qufim 2.3 ms of the CPU a frame. Not a draw with a normal map (the detail pass has its bent normal); not
 * before a scene has given the projection (g_vn_f). 1 when it drew any (u.sunuv.w) */
static int vn_caster(const Caster* c)
{
    const GfxVsKey* vk = &c->lib.vs;
    /* (not the caster pass's: out of the camera's view, or hidden from it) */
    if (c->dead || c->sought || vk->rhw || !vk->el[GFX_R_NORMAL].used || (c->lib.fs.nmap && g_fxs.normalmap >= 0.5f))
        return 0;
    return vk->prog ? vk->el[GFX_R_NORMAL].type == GFX_FLOAT3 : g_fxs.light != 0.0f && vk->lighting && !vk->flat;
}

static int vn_replay(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    if (g_fxs.sun <= 0.0f || g_fxs.sun_smooth < 0.5f || g_vn_f[2] == 0.0f || !g_ncasters || !depth || depth->use != GFX_USE_DEPTH)
        return 0;
    if (!fx_tex(&g_nd, DXGI_FORMAT_R16G16B16A16_FLOAT, color->w, color->h))
        return 0;
    ID3D12GraphicsCommandList* l = list();
    fx_state(&g_nd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (g_nd_clear)
    {
        const float zero[4] = { 0, 0, 0, 0 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(l, heap_cpu(&g_rtv, g_nd.rtv), zero, 0, NULL);
        g_nd_clear = 0;
    }
    tex_state(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(&g_rtv, g_nd.rtv), dsv = target_view(depth, 0, 0);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, &dsv);
    D3D12_RECT sc = { 0, 0, (LONG)color->w, (LONG)color->h };
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    set_viewport(s->vp);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 4, g_vn_f, 16); /* (gfx_hlsl.c emit_fs_tail: sun_f) */
    g_targets_bound = 0;
    drawn_forget();
    D3D12_GPU_VIRTUAL_ADDRESS bound_vb[GFX_NSTREAMS];
    memset(bound_vb, 0, sizeof bound_vb);
    D3D12_INDEX_BUFFER_VIEW bound_ib;
    memset(&bound_ib, 0, sizeof bound_ib);
    /* the key the caster before had, and its pipeline: the zone's draws in a row mostly share one */
    LibKey last;
    uint8_t last_topo = 0;
    ID3D12PipelineState* last_ps = NULL;
    uint32_t drawn = 0;
    for (uint32_t i = 0; i < g_ncasters; ++i)
    {
        Caster* cs = &g_casters[i];
        if (!vn_caster(cs))
            continue;
        const int at = alpha_tested(&cs->lib.fs);
        ID3D12PipelineState* ps = cs->vn_ps;
        if (!ps && last_ps && last_topo == cs->topo && !memcmp(&last, &cs->lib, sizeof last))
            ps = last_ps;
        if (!ps)
        {
            PipeKey pk;
            memset(&pk, 0, sizeof pk);
            pk.lib = cs->lib;
            GfxVsKey* vk = &pk.lib.vs;
            vk->shadow = 0, vk->motion = 0;
            vk->pixel = vk->prog ? 0 : 1; /* (a lit fixed-function draw passes its normal on: its own, per pixel) */
            vk->cpos = (uint8_t)(vk->prog ? 2 : 0);
            if (!at) /* its colour isn't wanted: its stages and pixel shader left out */
                memset(&pk.lib.fs, 0, sizeof pk.lib.fs);
            pk.lib.fs.nmap = 6;
            pk.color = (uint32_t)DXGI_FORMAT_R16G16B16A16_FLOAT, pk.dsv = (uint32_t)depth->dxfmt;
            pk.pipe.write_mask = 15;
            pk.depth.zenable = 1, pk.depth.zfunc = 4; /* D3DCMP_LESSEQUAL, no write: the scene's own depth decides */
            pk.cull = 1, pk.fill = 3;
            pk.topo = (uint8_t)(cs->topo == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                : cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINELIST || cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
                ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
            ps = pipeline_for(&pk, cs->vs, at ? cs->ps : NULL);
            if (!ps)
                continue; /* building: next frame */
        }
        cs->vn_ps = ps, last = cs->lib, last_topo = cs->topo, last_ps = ps;
        if (at)
        {
            for (int t = 0; t < 8; ++t)
                if (cs->tex[t])
                    tex_state(cs->tex[t], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, cs->bind, 0);
        }
        set_pso(ps);
        set_topology((D3D12_PRIMITIVE_TOPOLOGY)cs->topo);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, cs->ub);
        for (int j = 0; j <= GFX_NSTREAMS; ++j)
            if (cs->buf[j])
                buf_state(cs->buf[j], BUF_READ);
        for (int st = 0; st < GFX_NSTREAMS; ++st)
            if (cs->vb[st] != bound_vb[st])
                ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + st, cs->vb[st]), bound_vb[st] = cs->vb[st];
        DrawNote* dn = dred_draw(l, "a smooth normal (the scene's caster again)", cs->n);
        if (dn)
            dn->start = cs->ibv.BufferLocation ? 0 : cs->vstart, dn->flags = cs->ibv.BufferLocation ? DN_INDEXED : 0;
        if (cs->ibv.BufferLocation)
        {
            if (memcmp(&cs->ibv, &bound_ib, sizeof bound_ib))
                ID3D12GraphicsCommandList_IASetIndexBuffer(l, &cs->ibv), bound_ib = cs->ibv;
            ID3D12GraphicsCommandList_DrawIndexedInstanced(l, cs->n, 1, 0, 0, 0);
        }
        else
            ID3D12GraphicsCommandList_DrawInstanced(l, cs->n, 1, cs->vstart, 0);
        drawn++;
    }
    g_targets_bound = 0; /* the game's next draw binds its own again */
    drawn_forget();
    if (drawn)
        g_nd_valid = 1;
    return drawn != 0;
}

/* --- the zone's own curved surfaces (zn_draw, [graphics.effects] sun_curved) -------------------------------------------------
 * The shadow maps go by the polygons, and a polygon turned from the sun is in its own shadow all of it at once: round a curved
 * low-polygon shape (the Crag of Dem's dome, a hillside) the shadow's own edge was a staircase of whole polygons, and under a
 * high sun the dome's lower half a wall of dark squares. VanaCore reads the zone's own meshes for the maps (zonemesh.c), and each
 * vertex's normal with them, and from the faces and edges round a vertex how far the surface there is curved and smoothly shaded
 * and how high it bulges (core/zonemesh.cpp curve_of: rock and ground alone, 0 all over a building's mesh). Once the scene is
 * done the curved triangles are drawn against its finished depth (no write: only what shows, pulled a little toward the camera,
 * their positions being worked out another way than the game's own draws'), into a target of their own (g_nh): how far the pixel
 * lies under the smooth surface its three corners' normals describe (Phong tessellation's point for it), the bulge's height
 * there, and the curve. sun_map asks the shadow maps from the pixel's place on that smooth surface, not from the polygon cut
 * across under it, and shades the surface's own edge by its normal's angle to the light; every other pixel is as it was. One
 * draw, the zone's curved vertices (none in a town). The text is both back ends' (gfx_zn_hlsl; gfx_d3d11_smooth.h has Direct3D
 * 11's zn_draw): the positions and normals raw buffers at t0 and t1, read by the vertex's number, three to a triangle */
static const char ZN_HLSL[] =
    "cbuffer ZN : register(ZN_REG) { float4x4 zn_view; float4x4 zn_proj; float4 zn_vp; };\n"
    "ByteAddressBuffer zn_p : register(t0);\n"
    "ByteAddressBuffer zn_n : register(t1);\n"
    "struct ZO { float4 pos : SV_Position; float3 pv : ZPV; float3 nv : ZNV; float cv : ZCV; float ez : ZEZ; float3 b : ZB;\n"
    "  float lv : ZLV;\n"
    "  nointerpolation float3 n0 : ZNA; nointerpolation float3 n1 : ZNB; nointerpolation float3 n2 : ZNC;\n"
    "  nointerpolation float3 d : ZD; };\n"
    "struct ZT { float4 n : SV_Target0; float4 lift : SV_Target1; };\n"
    "ZO zn_vs(uint vid : SV_VertexID) {\n"
    "  float3 p = asfloat(zn_p.Load3(vid * 12));\n"
    "  float4 n = asfloat(zn_n.Load4(vid * 16));\n"
    "  float4 v = mul(zn_view, float4(p, 1.0));\n"
    "  float4 c = mul(zn_proj, v);\n"
    "  ZO o;\n"
    /* (the normal is 1 + the bulge's height at the vertex long, 0 none: zonemesh.cpp curve_of) */
    "  float nl = length(n.xyz);\n"
    "  o.pv = v.xyz; o.nv = mul(zn_view, float4(n.xyz / max(nl, 1e-6), 0.0)).xyz; o.cv = n.w; o.ez = c.w;\n"
    "  o.lv = max(nl - 1.0, 0.0);\n"
    /* the triangle's three corners (a list, three vertices each), for the pixel's place on the surface their normals describe:
     * each corner's normal, and how far along it the corner stands (its tangent plane), the same from all three */
    "  uint k = vid % 3, t = vid - k;\n"
    "  o.b = float3(k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0);\n"
    "  float3 q[3], m[3];\n"
    "  float least = 1.0;\n"
    "  for (uint j = 0; j < 3; ++j) {\n"
    "    q[j] = mul(zn_view, float4(asfloat(zn_p.Load3((t + j) * 12)), 1.0)).xyz;\n"
    "    float4 nj = asfloat(zn_n.Load4((t + j) * 16));\n"
    "    float3 w = mul(zn_view, float4(nj.xyz, 0.0)).xyz;\n"
    "    m[j] = w * rsqrt(max(dot(w, w), 1e-12));\n"
    "    least = min(least, nj.w);\n"
    "  }\n"
    /* a triangle with a flat corner is no curved surface's (a wall's, between its own flat corners and a moulding's curved
     * ones: curved by the pixel, it was shaded in bands down the wall, Southern San d'Oria under a high sun) */
    "  o.cv *= saturate(4.0 * least);\n"
    "  o.n0 = m[0]; o.n1 = m[1]; o.n2 = m[2];\n"
    "  o.d = float3(dot(q[0], m[0]), dot(q[1], m[1]), dot(q[2], m[2]));\n"
    /* (Direct3D 8's pixel centres, as the game's own draws are moved: gfx_hlsl.c) */
    "  c.x += c.w / zn_vp.x; c.y -= c.w / zn_vp.y;\n"
    "  o.pos = c;\n"
    "  return o;\n"
    "}\n"
    /* the pixel: its normal turned to the camera's side (n), and what the second target takes */
    "float4 zn_lift(ZO i, out float3 n) {\n"
    "  if (i.cv <= 0.02 || dot(i.nv, i.nv) < 1e-12) discard;\n"
    /* turned to the side of the face the camera sees, as vn_replay's (gfx_hlsl.c emit_fs_tail) */
    "  float3 g = cross(ddx(i.pv), ddy(i.pv));\n"
    "  if (dot(g, i.pv) > 0.0) g = -g;\n"
    "  n = normalize(i.nv);\n"
    "  if (dot(n, g) < 0.0) n = -n;\n"
    /* the lift (sun_map's g_lift): Phong tessellation's point for the pixel - it dropped onto each corner's tangent plane, the
     * three weighed as the corners are - and how far out along the normal that is. A bulge's corners' planes pass over the
     * polygon, so the point lies outside it; a hollow's pass under: no lift. Never more than 4 units (a stray normal) */
    "  float3 e = float3(dot(i.pv, i.n0), dot(i.pv, i.n1), dot(i.pv, i.n2)) - i.d;\n"
    "  float3 up = -(i.b.x * e.x * i.n0 + i.b.y * e.y * i.n1 + i.b.z * e.z * i.n2);\n"
    "  return float4(clamp(dot(up, n), 0.0, 4.0) * saturate(i.cv), min(i.lv, 8.0) * saturate(i.cv), saturate(i.cv), 0.0);\n"
    "}\n"
    /* Direct3D 11's: the normal too, three times as long (a baked surface's: read only where vn_replay drew none, sun_smooth 0) */
    "ZT zn_ps(ZO i) {\n"
    "  ZT o;\n"
    "  float3 n;\n"
    "  o.lift = zn_lift(i, n);\n"
    "  o.n = float4(n * 3.0, i.ez);\n"
    "  return o;\n"
    "}\n"
    /* Direct3D 12's: the one target (its vn_replay always draws the normals) */
    "float4 zn_ps1(ZO i) : SV_Target {\n"
    "  float3 n;\n"
    "  return zn_lift(i, n);\n"
    "}\n";

const char* gfx_zn_hlsl(void) { return ZN_HLSL; }

/* the curved surfaces at each pixel (zn_draw's target; the effects read it by its heap slot, u.aoc2.w: LIFT_AT) */
static FxTex g_nh;
/* the depth vn_replay drew against this scene (NULL: it drew nothing): zn_draw's too */
static GfxTex* g_zn_depth;
static struct
{
    ID3DBlob *vs, *fs;
    ID3D12PipelineState* ps;
    DXGI_FORMAT dsv; /* the depth format ps was made for */
    int failed;
} g_zn;

static int zn_ready(DXGI_FORMAT dsv)
{
    if (g_zn.failed)
        return 0;
    if (g_zn.ps && g_zn.dsv == dsv)
        return 1;
    if (!g_zn.vs)
    {
        /* (the constants at b0, the draws' own uniforms' place: ROOT_U) */
        static const char REG[] = "#define ZN_REG b0\n";
        char* src = (char*)malloc(sizeof REG + sizeof ZN_HLSL);
        if (src)
        {
            memcpy(src, REG, sizeof REG - 1), memcpy(src + sizeof REG - 1, ZN_HLSL, sizeof ZN_HLSL);
            g_zn.vs = compile(src, "zn_vs", "vs_5_1");
            g_zn.fs = g_zn.vs ? compile(src, "zn_ps1", "ps_5_1") : NULL;
            free(src);
        }
    }
    ID3D12PipelineState* p = NULL;
    if (g_zn.vs && g_zn.fs)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        memset(&pd, 0, sizeof pd);
        pd.pRootSignature = g_root;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g_zn.vs), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(g_zn.vs);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g_zn.fs), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(g_zn.fs);
        D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
        c->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
        c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
        c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        c->LogicOp = D3D12_LOGIC_OP_NOOP;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        /* toward the camera: the same surface as the game drew, by a few of the depth's steps and a slope's worth */
        pd.RasterizerState.DepthBias = dsv == DXGI_FORMAT_D16_UNORM ? -1 : -48;
        pd.RasterizerState.SlopeScaledDepthBias = -1.0f;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.DSVFormat = dsv;
        pd.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p)))
            p = NULL;
    }
    if (!p)
    {
        g_zn.failed = 1;
        fprintf(stderr, "[recomp] gfx: the zone's curved surfaces' pass couldn't be made; the sun's shadows go by the polygons there\n");
        return 0;
    }
    if (g_zn.ps)
        defer((IUnknown*)g_zn.ps, NULL, -1);
    g_zn.ps = p, g_zn.dsv = dsv;
    return 1;
}

/* 1 when the zone's curved surfaces were drawn into g_nh (cleared first), against `depth` */
static int zn_draw(GfxTex* color, GfxTex* depth, const GfxScene* s)
{
    GfxBuf *pos, *nrm;
    uint32_t n;
    if (g_fxs.sun_curved < 0.5f || !depth || depth->use != GFX_USE_DEPTH || s->proj[11] == 0.0f || !zonemesh_resident() ||
        !zonemesh_normals(&pos, &nrm, &n) || !zn_ready(depth->dxfmt))
        return 0;
    if (!fx_tex(&g_nh, DXGI_FORMAT_R16G16B16A16_FLOAT, color->w, color->h) || g_nh.srv < 0)
        return 0;
    Alloc a = ring(144, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return 0;
    const float vp[4] = { (float)(s->vp[2] ? s->vp[2] : color->w), (float)(s->vp[3] ? s->vp[3] : color->h), 0.0f, 0.0f };
    memcpy(a.cpu, s->view, 64);
    memcpy((uint8_t*)a.cpu + 64, s->proj, 64);
    memcpy((uint8_t*)a.cpu + 128, vp, 16);
    ID3D12GraphicsCommandList* l = list();
    fx_state(&g_nh, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float zero[4] = { 0, 0, 0, 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(&g_rtv, g_nh.rtv), dsv = target_view(depth, 0, 0);
    ID3D12GraphicsCommandList_ClearRenderTargetView(l, rtv, zero, 0, NULL);
    tex_state(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, &dsv);
    D3D12_RECT sc = { 0, 0, (LONG)color->w, (LONG)color->h };
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    set_viewport(s->vp);
    g_targets_bound = 0;
    drawn_forget();
    set_pso(g_zn.ps);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, a.gpu);
    buf_state(pos, BUF_READ);
    buf_state(nrm, BUF_READ);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0, pos->gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + 1, nrm->gpu);
    dred_draw(l, "the zone's curved surfaces (the sun's shading)", n);
    ID3D12GraphicsCommandList_DrawInstanced(l, n, 1, 0, 0);
    g_targets_bound = 0; /* the game's next draw binds its own again */
    drawn_forget();
    fx_state(&g_nh, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return 1;
}

#ifdef VANACORE
/* Each moving thing's own motion for DLSS ([graphics.effects] dlss_motion; mo_draw has the rest): the motion target, the
 * scene's size, R16G16B16A16_FLOAT - each pixel's motion as a share of the viewport to where it was the frame before (not
 * pixels: the draws' u.vp is in the game's pixels, and the scene is drawn at DLSS's share of the screen; fx_dlssmv makes it
 * render pixels), how far from the camera what moved was there (view z), and 2 for an effect's model with frame generation
 * (its depth handed on: fx_dlssz; 1 the rest), where a moving thing was drawn into it;
 * MO_NONE elsewhere, and where something drawn later in front of it hid it (fx_dlssmv: the depth says so), the camera's
 * motion from the depth - and the frames' draws, to find each in the frame before's */
#define MO_SLOTS 32768   /* draws a frame kept (a power of two); past that, they move with the camera. A zone drawn from its
                          * quadtree is 12,000 draws (Abyssea-Konschtat): 16384 ran its probes long */
#define MO_JUMP 2.0f     /* the farthest a vertex (or an object placed by a world matrix) goes in a frame and is the same draw */
#define MO_NONE 32768.0f /* the motion target where nothing moving was drawn (fx_dlssmv: anything past 16384) */
#define MO_MOST 262144u  /* the most vertices a draw's positions are kept for */
#define MO_CHAR_JUMP 0.6f /* the farthest a character's part goes in a frame and is the same part (mo_nearest): a neighbour in
                           * the same gear stands closer than MO_JUMP, and a part new in view was given its motion */
typedef struct MoEntry
{
    uint64_t serial;  /* the frame it's of (not its table's: a slot free) */
    uint64_t key;     /* the draw: what it draws, and which of the frame's draws like it it is (mo_draw) */
    uint64_t claim;   /* the frame a character's part took this one as where it was (mo_nearest): one part each */
    float wvp[16];    /* its transform */
    float anchor[4];  /* where its world matrix put it (GfxDraw.anchor; [3] 0: unknown) */
    uint32_t pos, n;  /* its positions, float3 each, at pos in its frame's arena; n 0: none kept (its buffer's own) */
    uint32_t cst;     /* a vertex shader's constants (GFX_NVSC float4s), 1 + where in the arena; 0 none kept */
    float clip0[4];   /* ... and its first vertex's clip-space position as the shader made it (gfx_draw_clip0; has_clip 1) */
    uint8_t has_clip;
} MoEntry;
typedef struct MoCount
{
    uint64_t serial, id;
    uint32_t n;
} MoCount;
typedef struct MoStood /* a kind of draw stood at an anchor in its frame (mo_stand; mo_stood asks the frame before's) */
{
    uint64_t serial, key;
} MoStood;
static struct
{
    FxTex t;               /* the motion target */
    uint64_t clear_serial; /* the frame it was cleared for: drawn into this frame (mout_begin) */
    FxTex m;               /* the moving things' mask (mask_draw): 1 and the view z where one was drawn, R16G16_FLOAT, the scene's size */
    uint64_t m_clear;      /* the frame it was cleared for (mask_begin) */
    MoEntry* e[2];         /* the draws of this frame (e[cur]) and of the frame before */
    float* arena[2];       /* ... their positions */
    uint32_t len[2], cap[2];
    uint64_t serial[2];    /* the frame each is of (0: none) */
    int cur;
    MoCount* count;        /* this frame's draws of each kind so far */
    MoStood* stood[2];     /* where each kind of draw stood, this frame's (stood[cur]) and the frame before's (mo_stood) */
    uint32_t drawn, kept;  /* this frame's: draws into the target, and draws kept */
} g_mo;
/* the scene draw mo_draw last found moved since the frame before (its anchor, its vertices or its constants), and the frame: for
 * mask_draw, which marks an object placed by a world matrix only when it moved (a boat, a lift: not a lamp post) */
static const GfxDraw* g_mo_moved;
static uint64_t g_mo_moved_serial;
/* the character mo_draw last gave its own motion (found in the frame before: moved, or standing still with the camera's), and
 * the frame: mask_draw leaves it out of the moving things' mask */
static const GfxDraw* g_mo_own;
static uint64_t g_mo_own_serial;

static D3D12_CPU_DESCRIPTOR_HANDLE mout_view(void) { return heap_cpu(&g_rtv, g_maskout ? g_mo.m.rtv : g_mo.t.rtv); }

/* The moving things' mask target (mask_draw), cleared once a frame like the motion target (mout_begin): 0 when it can't be had */
static int mask_begin(void)
{
    uint32_t w, h;
    color_size(&w, &h);
    if (!w || !h)
        return 0;
    if (g_mo.m_clear == g_serial ? g_mo.m.w != w || g_mo.m.h != h : !fx_tex(&g_mo.m, DXGI_FORMAT_R16G16_FLOAT, w, h))
        return 0;
    fx_state(&g_mo.m, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (g_mo.m_clear != g_serial)
    {
        const float none[4] = { 0, 0, 0, 0 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list(), heap_cpu(&g_rtv, g_mo.m.rtv), none, 0, NULL);
        g_mo.m_clear = g_serial;
    }
    return 1;
}

/* The motion target for the target being drawn to (its size), cleared once a frame: 0 when it can't be had, or the frame's is
 * another size (another target than the scene's) */
static int mout_begin(void)
{
    uint32_t w, h;
    color_size(&w, &h);
    if (!w || !h)
        return 0;
    if (g_mo.clear_serial == g_serial ? g_mo.t.w != w || g_mo.t.h != h : !fx_tex(&g_mo.t, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h))
        return 0;
    fx_state(&g_mo.t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (g_mo.clear_serial != g_serial)
    {
        const float none[4] = { MO_NONE, MO_NONE, 0, 0 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list(), mout_view(), none, 0, NULL);
        g_mo.clear_serial = g_serial;
    }
    return 1;
}
#else
static D3D12_CPU_DESCRIPTOR_HANDLE mout_view(void) { return nout_view(); }
#endif

/* one full-screen triangle into a target (w x h, drawn within vp), reading heap slots in[0..n) */
/* an effect pass's name, by its pipeline (the GPU crash data's notes) */
static const char* fx_name(ID3D12PipelineState* p)
{
    for (int i = 0; i < FX_NPS; ++i)
        if (p == g_fx.pso[i])
            return FX_ENTRY[i];
    for (uint32_t i = 0; i < FX_FORMATS; ++i)
    {
        if (p == g_fx.by_fmt[i].comp)
            return "fx_comp (onto the scene)";
        if (p == g_fx.by_fmt[i].mip)
            return "fx_mip (the scene filter's mip chain)";
        if (p == g_fx.by_fmt[i].smooth)
            return "fx_smooth (onto the scene)";
        if (p == g_fx.by_fmt[i].dofcomp)
            return "fx_dofcomp (onto the scene)";
        if (p == g_fx.by_fmt[i].fxaa)
            return "fx_fxaa (onto the scene)";
        if (p == g_fx.by_fmt[i].glow)
            return "fx_glowc (the glow onto its target)";
        if (p == g_fx.by_fmt[i].glowg)
            return "fx_glowg (the glow again over the grass)";
        if (p == g_fx.by_fmt[i].post)
            return "fx_post (the colour grade, last)";
        if (p == g_fx.by_fmt[i].refl)
            return "fx_refl (onto the scene)";
        if (p == g_fx.by_fmt[i].clarity)
            return "fx_clarity (onto the scene)";
        if (p == g_fx.by_fmt[i].zlate)
            return "fx_zlate (the zone lines onto the scene)";
        if (p == g_fx.by_fmt[i].easu)
            return "fx_easu (FSR 1's upscale)";
        if (p == g_fx.by_fmt[i].rcas)
            return "fx_rcas (FSR 1's sharpening)";
        if (p == g_fx.by_fmt[i].sky)
            return "sky_ps (VanaCore's sky, in place of the game's dome)";
    }
    if (p == g_fx.skycol_pso)
        return "fx_skycol";
    return "an effect pass";
}

/* a depth-stencil fx_draw binds with its target, and the stencil reference it draws with (the characters' mask reads the scene's
 * stencil: cmask_pass); NULL for none, as every other pass */
static const D3D12_CPU_DESCRIPTOR_HANDLE* g_fx_dsv;
static int g_fx_sref;

static void fx_draw(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h, ID3D12PipelineState* p, const float vp[4],
    const FxU* u, const int32_t* in, int n, int dx, int dy)
{
    ID3D12GraphicsCommandList* l = list();
    uint32_t bind[16] = { 0 };
    for (int i = 0; i < n && i < 12; ++i) /* inputs 8 on after the sampler's and blur's (fsi[1]) */
        bind[i < 8 ? i : i + 4] = in[i] >= 0 ? (uint32_t)in[i] : SRV_NULL_2D;
    bind[8] = g_present_samp, bind[9] = (uint32_t)dx, bind[10] = (uint32_t)dy, bind[11] = g_cmp_samp;
    if (u)
    {
        Alloc a = ring(sizeof *u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (!a.cpu)
            return;
        memcpy(a.cpu, u, sizeof *u);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, a.gpu);
    }
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, g_fx_dsv);
    D3D12_VIEWPORT v = { vp[0], vp[1], vp[2], vp[3], 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
    set_pso(p);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (g_fx_dsv)
        ID3D12GraphicsCommandList_OMSetStencilRef(l, (UINT)g_fx_sref); /* (drawn_forget below: the game's next draw sets its own) */
    DrawNote* dn = dred_draw(l, g_dred ? fx_name(p) : NULL, 3);
    if (dn)
        dn->rw = w, dn->rh = h;
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
    g_targets_bound = 0; /* the game's next draw binds its own again */
    drawn_forget();
}

/* fx_draw into a depth target (the pass's SV_Depth), no colour */
static void fx_draw_depth(D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t w, uint32_t h, ID3D12PipelineState* p, const FxU* u,
    const int32_t* in)
{
    ID3D12GraphicsCommandList* l = list();
    uint32_t bind[16] = { 0 };
    bind[0] = in[0] >= 0 ? (uint32_t)in[0] : SRV_NULL_2D;
    bind[1] = in[1] >= 0 ? (uint32_t)in[1] : SRV_NULL_2D;
    bind[8] = g_present_samp, bind[11] = g_cmp_samp;
    Alloc a = ring(sizeof *u, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (!a.cpu)
        return;
    memcpy(a.cpu, u, sizeof *u);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, a.gpu);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 0, NULL, FALSE, &dsv);
    D3D12_VIEWPORT v = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, bind, 0);
    set_pso(p);
    set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DrawNote* dn = dred_draw(l, "fx_depthup (DLSS's depth for the interface drawn over it)", 3);
    if (dn)
        dn->rw = w, dn->rh = h;
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
    g_targets_bound = 0;
    drawn_forget();
}

/* the occlusion's depth and its levels (fx_linz), for an occlusion of aw x ah */
static int fx_lz(uint32_t aw, uint32_t ah)
{
    for (int lv = 0; lv < 4; ++lv, aw = (aw + 1) / 2, ah = (ah + 1) / 2)
        if (!fx_tex(&g_fx.lz[lv], DXGI_FORMAT_R32_FLOAT, aw, ah))
            return 0;
    return 1;
}

/* into an effect target, all of it, from inputs already readable */
static void fx_pass(FxTex* to, int kind, const FxU* u, const int32_t* in, int n, int dx, int dy)
{
    fx_state(to, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float vp[4] = { 0, 0, (float)to->w, (float)to->h };
    fx_draw(heap_cpu(&g_rtv, to->rtv), to->w, to->h, g_fx.pso[kind], vp, u, in, n, dx, dy);
    fx_state(to, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

static int g_aura_on; /* a place of style 1 (a character's aura) is in the scene (sparkles): it glows by the characters' mask */

#include "gfx_d3d12_cmask.h" /* the characters' mask from the stencil (cmask_usable, cmask_pass) */

static void sub_barrier(GfxTex* t, uint32_t sub, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t->res;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list(), 1, &b);
}

/* the scene's chain, each level from the one above it; the texture ends readable */
static void fx_mips(GfxTex* t, ID3D12PipelineState* p)
{
    tex_state(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (uint32_t lv = 1; lv < t->mips; ++lv)
    {
        sub_barrier(t, lv - 1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        uint32_t w, h;
        level_size(t, lv, &w, &h);
        float vp[4] = { 0, 0, (float)w, (float)h };
        fx_draw(target_view(t, 0, lv), w, h, p, vp, NULL, &t->mip_srv[lv - 1], 1, 0, 0);
    }
    sub_barrier(t, t->mips - 1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    t->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    t->used = pending();
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

enum { SUN_MAP = 4096 };

#include "gfx_sun_cache.h" /* the shadow cache: Cached, g_cache, the snaps, cache_see, sun_cache_update, fx_stats */

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
    float c[3];     /* the sphere's centre in the world, before the snap (near_stale) */
} SunCascade;

/* casters stand up to this many units sunward of a cascade's sphere (a cliff over the camera) */
#define SUN_BACK 200.0f

#include "gfx_sun_record.h" /* gfx_sun_record: every frame's shadow state while a /vana stats recording runs (srec_*) */

/* grow: the sphere's radius times this (1: the slice's own; the near map's still layer takes a margin round it) */
static void sun_fit(const GfxScene* s, const float* invV, const float* L, float t0, float t1, uint32_t size, float grow,
    SunCascade* k)
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
    R = ceilf(R * grow); /* whole units: the texel's size holds still */
    /* sun_centre: round the player, out to t1 (the far map's reach, the sharp map's), whatever the camera does. Round the view's
     * slice, as above, every turn or tilt of the camera moved the map, the ground and walls it covered and its fading edge - a
     * building's shadow on the walls came and went as the camera moved with the sun and the player still (Port Jeuno,
     * 2026-11-01). Round the player it moves only as the player does, snapped to its texels below like the view's. */
    if (g_fxs.sun_centre != 0.0f && s->focus[3] != 0.0f)
    {
        memcpy(c, s->focus, sizeof c);
        R = ceilf(t1 * grow);
    }
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
    float cz = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
    float back = SUN_BACK, range = 2.0f * R + back, z0 = cz - R - back;
    memcpy(k->c, c, sizeof k->c);
    float S[16] = {
        r[0] / R, u[0] / R, f[0] / range, 0,
        r[1] / R, u[1] / R, f[1] / range, 0,
        r[2] / R, u[2] / R, f[2] / range, 0,
        -cx / R, -cy / R, -z0 / range, 1,
    };
    memcpy(k->S, S, sizeof S);
    mat_mul(k->lmat, invV, S);
    k->texel = tx, k->bias = 0.03f / range, k->range = range, k->across = 2.0f * R;
    k->ox = nx - 0.5f * (float)size, k->oy = ny + 0.5f * (float)size;
    /* the penumbra's radius grows by sun_soft for each unit from the caster: in the map's width
     * (2R across) per unit of its depth (range deep) */
    k->soft = fmaxf(g_fxs.sun_soft, SUN_SIZE) * range / (2.0f * R);
    k->slope = 2.0f * R / range;
}

/* a shadow map let go (after the frames that draw with it) */
static void sun_release(SunMap* m)
{
    if (m->res)
    {
        defer((IUnknown*)m->res, NULL, -1);
        defer(NULL, &g_srv, m->srv);
        defer(NULL, &g_dsv, m->dsv);
    }
    m->res = NULL, m->srv = m->dsv = -1;
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
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, size, size, 1, 1, DXGI_FORMAT_R32_TYPELESS, { 1, 0 },
        D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    D3D12_CLEAR_VALUE cv;
    memset(&cv, 0, sizeof cv);
    cv.Format = DXGI_FORMAT_D32_FLOAT, cv.DepthStencil.Depth = 1.0f;
    m->srv = heap_alloc(&g_srv), m->dsv = heap_alloc(&g_dsv);
    if (m->srv < 0 || m->dsv < 0 ||
        FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&m->res)))
    {
        fprintf(stderr, "[recomp] gfx: the sun's shadow map (%ux%u) failed; no map shadows\n", size, size);
        heap_free(&g_srv, m->srv), heap_free(&g_dsv, m->dsv);
        m->res = NULL, m->srv = m->dsv = -1, m->failed = 1;
        return 0;
    }
    m->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_SHADER_RESOURCE_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.Format = DXGI_FORMAT_R32_FLOAT;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, v.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, m->res, &v, heap_cpu(&g_srv, m->srv));
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    memset(&dv, 0, sizeof dv);
    dv.Format = DXGI_FORMAT_D32_FLOAT, dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateDepthStencilView(g_dev, m->res, &dv, heap_cpu(&g_dsv, m->dsv));
    return 1;
}

static void sun_state(SunMap* m, D3D12_RESOURCE_STATES want)
{
    if (m->res && m->state != want)
        barrier(m->res, m->state, want), m->state = want;
}

/* How far past a map's sides a caster may stand (where its first vertex is: a corner of an object, of a
 * piece of the zone) and still reach into it. Its shadow falls along the light, where the map's x and y
 * don't change: only the caster's own size reaches across them. */

/* True when a caster standing at `at` can't reach the cascade's map (SUN_REACH or more past its sides). The
 * cache keeps what was seen for 10 seconds (30 before), far past the maps with a long draw distance: thousands of
 * casters, each drawn into both maps every frame, until a relog cleared them. */
/* The cascade the casters are culled against (beyond_map) while a map is drawn: the far map's for the sharp map too (sun_map),
 * so the sharp map never lacks a caster the far map has. The sharp map is the only one looked at where it reaches, and zoomed
 * out it reaches the whole plaza: a building culled from it alone by its first vertex (96 units past a map 174 across) left
 * the plaza lit while the far map held its shadow unused (Port Jeuno, 2026-11-01 22:57, the sharp layer drawn again through a
 * smaller sphere). NULL: the map's own. */
static const SunCascade* g_sun_cull;
static const SunCascade* sun_cull(const SunCascade* k) { return g_sun_cull ? g_sun_cull : k; }

static int beyond_map(const SunCascade* k, const float at[3])
{
    if (g_fxs.sun_cull == 0.0f || (sun_gate() & SG_CULL))
        return 0;
    const float x = at[0] * k->S[0] + at[1] * k->S[4] + at[2] * k->S[8] + k->S[12];
    const float y = at[0] * k->S[1] + at[1] * k->S[5] + at[2] * k->S[9] + k->S[13];
    const float edge = 1.0f + SUN_REACH * 2.0f / k->across; /* the map: -1..1 over its 2R across */
    return fabsf(x) > edge || fabsf(y) > edge;
}

/* sun_draw's part of the casters: all of them; the still ones (the zone's and its kept copies, this frame's from buffers the game
 * keeps or sways objects through: SUN_STILL); or the moving ones alone (this frame's others - characters, effects - over what the
 * map holds, not cleared first: SUN_MOVING) */
enum { SUN_ALL = 0, SUN_STILL = 1, SUN_MOVING = 2 };

/* the key of caster cs's pipeline into the sun's maps (at: it's alpha tested); shadow 2, fading out of them (cache_let_go) */
static void sun_pipe_key(const Caster* cs, int at, int shadow, PipeKey* pk)
{
    memset(pk, 0, sizeof *pk);
    pk->lib = cs->lib;
    pk->lib.vs.shadow = (uint8_t)shadow, pk->lib.vs.pixel = 0;
    if (!at)
    {
        /* the position alone: one pipeline serves every draw with the same vertex layout */
        GfxVsKey* vk = &pk->lib.vs;
        vk->lighting = vk->normalize = vk->localviewer = vk->specular = 0;
        vk->src_diffuse = vk->src_specular = vk->src_ambient = vk->src_emissive = 0;
        vk->nlights = 0, memset(vk->light_type, 0, sizeof vk->light_type);
        vk->fog_vertex = vk->range_fog = 0, vk->ntex = 0, vk->flat = 0;
        memset(vk->tci, 0, sizeof vk->tci), memset(vk->ttf, 0, sizeof vk->ttf);
        memset(&pk->lib.fs, 0, sizeof pk->lib.fs);
    }
    pk->depth.zenable = 1, pk->depth.zwrite = 1, pk->depth.zfunc = 2;
    pk->color = (uint32_t)DXGI_FORMAT_UNKNOWN, pk->dsv = (uint32_t)DXGI_FORMAT_D32_FLOAT;
    pk->cull = 1, pk->fill = 3;
    pk->topo = (uint8_t)(cs->topo == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
        : cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINELIST || cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
        : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
}

#include "gfx_sun_slice.h" /* sun_clears, sun_leaves_out: the plants sun_plants leaves out, and (sun_roll) a layer's other slices */

/* One of this frame's casters sun_draw leaves out of a map: dead; the zone's where characters alone cast (chars: the characters'
 * own map, sun_map_view; sun_casters 1: the zone's shadows are baked into its colours already, and the game tints them for the hour
 * and the weather), a character's where the zone alone does (sun_casters 2: the game's own shadows stay under characters); and the
 * caster pass's from the characters' map (they're the zone's, out of the camera's view). */
static int caster_left_out(const Caster* cs, int chars)
{
    if (cs->dead)
        return 1;
    if ((chars || g_fxs.sun_casters == 1.0f) && cs->fixed)
        return 1;
    if (!chars && g_fxs.sun_casters == 2.0f && !cs->fixed)
        return 1;
    /* the zone's from the caster pass alone while it draws (sun_pass_only): not the game's own draws of it */
    if (!chars && cs->fixed && !cs->sought && sun_pass_only())
        return 1;
    return chars && cs->sought;
}

/* a live caster's matrix into the map, bound when it isn't: the caster pass's in world space (Caster.world: its uniforms' transform
 * is its world matrix alone) by the map's own (S), the rest through this frame's camera (M); the one bound now returned */
static int live_matrix(ID3D12GraphicsCommandList* l, const Caster* cs, int bound, const float* S, const float* M)
{
    if (cs->world != bound)
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 16, cs->world ? S : M, 0);
    return cs->world;
}

/* a kept caster's matrix into cascade k's map, bound when it isn't: the caster pass's in world space (a plant's still pose:
 * Caster.world) by the map's own (S), the rest through the camera it was last seen through (Cached.clip_world). bound: 0 none, 1
 * bound_cw's, 2 the map's own; the one bound now returned */
static int kept_matrix(ID3D12GraphicsCommandList* l, const Cached* ce, const SunCascade* k, float* bound_cw, int bound)
{
    if (ce->c.world)
    {
        if (bound != 2)
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 16, k->S, 0);
        return 2;
    }
    if (bound != 1 || memcmp(bound_cw, ce->clip_world, 64))
    {
        float mm[16];
        mat_mul(mm, ce->clip_world, k->S);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 16, mm, 0);
        memcpy(bound_cw, ce->clip_world, 64);
    }
    return 1;
}

/* this frame's caster cs left out of cascade k's map by sun_draw (its arguments): caster_left_out's; one the cache draws in its
 * place (cache_instead: the caster pass's, kept by piece; a plant's still pose); not of the part drawn; standing past the map's
 * reach (counted) */
static int live_left_out(const Caster* cs, const SunCascade* k, const float* clip_world, int chars, int part, int cache)
{
    if (caster_left_out(cs, chars))
        return 1;
    if (cache && ((cs->entry && cache_instead(&g_cache[cs->entry - 1])) || (cs->snapped && sun_pass_only())))
        return 1;
    const int still = cs->fixed || cs->own;
    if ((part == SUN_STILL && !still) || (part == SUN_MOVING && still))
        return 1;
    float at[3];
    if (caster_at(cs, clip_world, at) && beyond_map(sun_cull(k), at))
    {
        g_sst.culled++;
        return 1;
    }
    return 0;
}

/* the casters into one cascade's map: this frame's, and the zone's kept from before (cache); chars 1, this frame's
 * characters alone; part, which of them (SUN_ALL ...) */
static uint32_t sun_draw(SunMap* m, const GfxScene* s, const float* invP, const float* invV, const SunCascade* k, int cache,
    int chars, int part)
{
    float clip_world[16], M[16];
    mat_mul(clip_world, invP, invV);
    mat_mul(M, clip_world, k->S); /* the camera's clip space -> the map */
    ID3D12GraphicsCommandList* l = list();
    sun_state(m, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = heap_cpu(&g_dsv, m->dsv);
    if (sun_clears(part))
        ID3D12GraphicsCommandList_ClearDepthStencilView(l, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    /* the cache off, or the zone from the caster pass alone: of the cache, what cache_drawn takes - the pass's draws, kept by
     * piece, while it runs; else the plants' still poses */
    const int still_only = cache && part != SUN_MOVING && (sun_cache_off() || sun_pass_only());
    if (part == SUN_MOVING)
        cache = 0;
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 0, NULL, FALSE, &dsv);
    D3D12_VIEWPORT v = { 0, 0, (float)m->size, (float)m->size, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)m->size, (LONG)m->size };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 16, M, 0);
    g_targets_bound = 0;
    drawn_forget();
    /* what's bound, not bound again: draws in a row mostly share their vertices and indices (copies of one mesh), and
     * the kept ones the camera they were last seen through */
    D3D12_GPU_VIRTUAL_ADDRESS bound_vb[GFX_NSTREAMS];
    memset(bound_vb, 0, sizeof bound_vb);
    D3D12_INDEX_BUFFER_VIEW bound_ib;
    memset(&bound_ib, 0, sizeof bound_ib);
    float bound_cw[16];
    int cw_bound = 0, live_world = 0; /* (live_world: the live casters' matrix bound, 0 M, 1 the map's own: live_matrix) */
    /* the cache's: those sun_cache_update listed (g_cache_vis), then its copies */
    const uint32_t nvis = cache ? g_ncache_vis : 0, kept = g_ncasters + nvis;
    uint32_t drawn = 0, total = kept + (cache ? g_nsnaps : 0);
    for (uint32_t i = 0; i < total; ++i)
    {
        Caster* cs;
        Cached* ce = NULL;
        if (i < g_ncasters)
        {
            cs = &g_casters[i];
            if (live_left_out(cs, k, clip_world, chars, part, cache))
                continue;
        }
        else
        {
            if (g_fxs.sun_casters == 1.0f)
                break;
            /* the zone out of view: as it was drawn when last seen, through that frame's camera; then the
             * copies (snap_note) */
            if (i < kept)
            {
                const CacheVis* v = &g_cache_vis[i - g_ncasters]; /* not gone, not drawn this frame, still kept */
                if ((v->i & CACHE_VIS_AT) && beyond_map(sun_cull(k), v->at))
                {
                    g_sst.culled++;
                    continue;
                }
                ce = &g_cache[v->i & ~CACHE_VIS_AT];
                if (!cache_drawn(ce, still_only))
                    continue;
            }
            else
            {
                Snap* sp = &g_snaps[i - kept];
                ce = &sp->k;
                if (!snap_drawn(sp, still_only))
                    continue;
                buf_state(g_chunks[sp->chunk].buf, BUF_READ); /* its chunk read in this list's state (as the casters' below) */
                if (ce->has_at && beyond_map(sun_cull(k), ce->at))
                {
                    g_sst.culled++;
                    continue;
                }
            }
            cs = &ce->c;
        }
        if (sun_leaves_out(cs, ce, i, kept, chars))
            continue;
        const int at = alpha_tested(&cs->lib.fs);
        ID3D12PipelineState* ps = cs->sun_ps; /* a kept caster's, found the first frame it was drawn from the cache */
        /* this frame's caster the cache keeps: the entry's (the same LibKey; the pipeline is the key's and the topology's), and the
         * entry gets this one's */
        Caster* kin = !ce && cs->entry && g_cache[cs->entry - 1].c.topo == cs->topo ? &g_cache[cs->entry - 1].c : NULL;
        if (!ps && kin && kin->sun_ps)
            ps = cs->sun_ps = kin->sun_ps;
        if (!ps)
        {
            PipeKey pk;
            sun_pipe_key(cs, at, 1, &pk);
            ps = pipeline_for(&pk, cs->vs, cs->ps);
            if (!ps)
            {
                g_fx.tr_skipped++;
                g_fx.tr_building += g_pipe_waiting;
                continue;
            }
            cs->sun_ps = ps;
            if (kin)
                kin->sun_ps = ps;
        }
        /* let go of and fading (cache_let_go): a share of its texels, through its fading pipeline - drawn whole while that one
         * builds */
        const float share = ce ? cache_share(ce) : 1.0f;
        if (share < 1.0f)
        {
            if (!cs->sun_fps)
            {
                PipeKey pk;
                sun_pipe_key(cs, at, 2, &pk);
                cs->sun_fps = pipeline_for(&pk, cs->vs, cs->ps);
            }
            if (cs->sun_fps)
            {
                const float f[4] = { share, k->ox, k->oy, 0.0f };
                ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 4, f, 16);
                ps = cs->sun_fps;
                g_sst.fading++;
            }
        }
        D3D12_GPU_VIRTUAL_ADDRESS ub = cs->ub;
        if (ce)
        {
            if (ce->ring_serial != g_serial) /* its uniforms, once a frame (both cascades read them) */
            {
                Alloc a = ring_part(ce->c.need, sizeof(GfxU), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
                if (!a.cpu)
                {
                    g_fx.tr_nou++;
                    continue;
                }
                sun_u_copy(a.cpu, ce->u, &ce->c);
                ce->ring_u = a.gpu, ce->ring_serial = g_serial;
            }
            ub = ce->ring_u;
            cw_bound = kept_matrix(l, ce, k, bound_cw, cw_bound);
            g_fx.tr_cached++;
            if (i >= kept)
                g_sst.copies_drawn++;
            else
                g_sst.cache_drawn++;
        }
        else
            g_fx.tr_live++, live_world = live_matrix(l, cs, live_world, k->S, M);
        if (m == &g_fx.smaps || m == &g_fx.smapsb) /* the far layer took it (the flicker watch's holes, gfx_sun_slice.h) */
            slice_mark(ce ? (i < kept ? (uint32_t)(ce - g_cache) : ~0u) : cs->entry ? cs->entry - 1 : ~0u);
        if (at)
        {
            for (int t = 0; t < 8; ++t)
                if (cs->tex[t])
                    tex_state(cs->tex[t], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, cs->bind, 0);
        }
        set_pso(ps);
        set_topology((D3D12_PRIMITIVE_TOPOLOGY)cs->topo);
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, ub);
        /* its static buffers read in this list's state: a slab (shared by hundreds of buffers) left promoted by these reads
         * unrecorded made the list's next barrier on any block of it start from the wrong state (from XI on Anything, which
         * does the same here) */
        for (int j = 0; j <= GFX_NSTREAMS; ++j)
            if (cs->buf[j])
                buf_state(cs->buf[j], BUF_READ);
        for (int st = 0; st < GFX_NSTREAMS; ++st)
            if (cs->vb[st] != bound_vb[st])
                ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + st, cs->vb[st]), bound_vb[st] = cs->vb[st];
        DrawNote* dn = dred_draw(l, !ce ? "a sun shadow caster (this frame's)" : i < kept
            ? "a sun shadow caster (the zone's, kept)" : "a sun shadow caster (a kept copy)", cs->n);
        if (dn)
            dn->start = cs->ibv.BufferLocation ? 0 : cs->vstart, dn->flags = cs->ibv.BufferLocation ? DN_INDEXED : 0,
            dn->rw = dn->rh = m->size;
        if (cs->ibv.BufferLocation)
        {
            if (memcmp(&cs->ibv, &bound_ib, sizeof bound_ib))
                ID3D12GraphicsCommandList_IASetIndexBuffer(l, &cs->ibv), bound_ib = cs->ibv;
            ID3D12GraphicsCommandList_DrawIndexedInstanced(l, cs->n, 1, 0, 0, 0);
        }
        else
            ID3D12GraphicsCommandList_DrawInstanced(l, cs->n, 1, cs->vstart, 0);
        drawn++;
    }
    g_targets_bound = 0; /* the game's next draw binds its own again */
    drawn_forget();
    return drawn;
}

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
        o[3] = b[7]; /* how far above its middle the ground is: the lines, dots and sparkles start from it */
        /* the line along the box's longer side across, the plane facing its shorter: a zone's boxes come either way
         * (South San d'Oria's 15 across x and 2 deep, West Ronfaure's 8.7 across x and 25 deep) */
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

/* VanaCore's sparkles and auras into the effects' uniforms: the places within SP_FAR of the camera (cam) with any strength,
 * each its centre in view space and the disc's radius, its colour and how high they rise, its style, size, amount and
 * strength; the world's ground axes in view space; none without the camera */
#define SP_FAR 120.0f
static void sparkles(const GfxScene* s, const float* cam, FxU* u)
{
    u->spc[0] = 0.0f;
    g_aura_on = 0;
    if (!cam || s->nsparkles == 0)
        return;
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->nsparkles && i < GFX_SPARKLES; ++i)
    {
        const float* p = s->sparkles[i];
        const float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
        if (!(p[11] > 0.0f) || sqrtf(dx * dx + dy * dy + dz * dz) - p[3] > SP_FAR)
            continue;
        g_aura_on |= p[8] > 0.5f;
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
    /* their own time, at their speed: a speed changed (a slider dragged) carries on from where they are */
    static uint64_t last_ns;
    static double t;
    const uint64_t now = gfx_now_ns();
    if (last_ns && now > last_ns)
        t = fmod(t + fmin((double)(now - last_ns) / 1e9, 0.25) * (s->sparkle_speed > 0.0f ? s->sparkle_speed : 1.0f), 3600.0);
    last_ns = now;
    u->spc[0] = (float)n, u->spc[1] = (float)t;
    u->spp[0] = s->sparkle_match ? 1.0f : 0.0f, u->spp[1] = (float)fmod((double)now / 1e9, 3600.0);
    u->spp[2] = g_aura_on ? 1.0f : 0.0f; /* an aura among them: zl_run gives its glow the characters' mask, or clears this */
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

/* Why a still layer is drawn again (still_stale and near_stale leave it in g_stale_why; the flicker watch, the trace and fx_stats
 * say it): its age, a gap (it wasn't what the map was made from the frame before, or it's the first), other settings, the sun
 * turned, the far map's radius changed or the sharp map's slice shrank, the camera moved past its margin */
enum { WHY_AGE = 1, WHY_GAP = 2, WHY_SETTINGS = 4, WHY_SUN = 8, WHY_RADIUS = 16, WHY_MOVED = 32, WHY_SHRUNK = 64 };
#define WHY_NAMES 7
static uint32_t g_stale_why;

/* The far map's still layer ([graphics.effects] sun_far_hold): the zone's casters (the cache's, its copies, this frame's from
 * buffers the game keeps, and the objects it sways through one buffer: trees) drawn every few frames into g_fx.smaps through the
 * cascade fitted then (k); each frame the far map starts as a copy of it and this frame's moving casters (characters, effects)
 * go over it, so what moves is never a frame behind. A tree's sway, a door, or a far elevator's shadow moves in steps there
 * (and in the sharp map's still layer, g_still_near). The middle map's (sun_mid, gfx_sun_mid.h) is made the same way: g_still_mid. */
typedef struct
{
    SunCascade k;    /* the cascade it was drawn through */
    uint64_t serial; /* the frame it was drawn (0: to be drawn) */
    uint64_t seen;   /* the last frame the far map was made from it */
    float L[3], dfar, casters, soft, plants; /* the sun and the settings it was drawn for (dfar: the map's reach) */
    uint32_t drawn;  /* its casters */
} SunStill;
static SunStill g_still, g_still_mid;

/* the still layer (st) is to be drawn again: hold frames since it was, a frame without its map since, other settings, the sun
 * turned (more than about 0.08 degrees: its glide is far slower than that over a few frames, the turn at 06:00 and 18:00 far
 * more), or this frame's cascade (cf: the camera's view now) gone from the layer's by more than a 20th of its radius - moved
 * along the sun's axes or grown - which leaves the far edge of the map, near sun_distance, a few units short for a few frames */
static int still_stale(const SunStill* st, const SunCascade* cf, const float* L, float dfar, uint32_t hold)
{
    const SunCascade* k = &st->k;
    uint32_t why = 0;
    if (!st->serial || st->seen + 1 != g_serial)
        why |= WHY_GAP;
    else if (g_serial - st->serial > hold)
        why |= WHY_AGE;
    if (dfar != st->dfar || g_fxs.sun_casters != st->casters || g_fxs.sun_soft != st->soft || g_fxs.sun_plants != st->plants)
        why |= WHY_SETTINGS;
    if (L[0] * st->L[0] + L[1] * st->L[1] + L[2] * st->L[2] < 0.999999f)
        why |= WHY_SUN;
    if (!why)
    {
        const float R = 0.5f * k->across, Rn = 0.5f * cf->across;
        if (fabsf(Rn - R) > 0.02f * R)
            why |= WHY_RADIUS;
        else
        {
            /* the centres along the map's x and y (S[12], S[13]: minus the centre over R), and the near end along the light (S[14]:
             * minus it over the range) */
            const float dx = cf->S[12] * Rn - k->S[12] * R, dy = cf->S[13] * Rn - k->S[13] * R;
            const float dz = cf->S[14] * cf->range - k->S[14] * k->range;
            if (dx * dx + dy * dy > 0.0025f * R * R || fabsf(dz) > 0.05f * R)
                why |= WHY_MOVED;
        }
    }
    g_stale_why = why;
    return why != 0;
}

/* The sharp map's still layer ([graphics.effects] sun_near_hold): as the far map's, but drawn through a cascade fitted with a
 * margin round the sharp map's slice (SUN_NEAR_MARGIN) and kept for as long as this frame's slice still lies inside it - the
 * camera moved or turned less than the margin, the sun turned less than the far map's step - and sun_near_hold frames at
 * most; each frame the sharp map starts as a copy of it and this frame's moving casters go over it. Drawn whole every frame,
 * Kazham's sharp map took 13.6 ms at 4096 texels and 56.6 at 8192 (2026-10-28, an RTX 5070 Ti laptop held at 1150 MHz): some
 * 5000 casters a frame, the view's and the cache's, filled at the map's whole size. */
#define SUN_NEAR_MARGIN 1.25f
static struct
{
    SunCascade k;    /* the cascade it was drawn through (wider than the slice's) */
    uint64_t serial; /* the frame it was drawn (0: to be drawn) */
    uint64_t seen;   /* the last frame the sharp map was made from it */
    float L[3], casters, soft, plants; /* the sun and the settings it was drawn for */
    uint32_t size;   /* its texels across (sun_detail) */
} g_still_near;

/* the near layer is to be drawn again: hold frames since it was, a frame without the sharp map since, another size or other
 * settings, the sun turned (as still_stale), or this frame's slice (cn) no longer inside the layer's cascade - its sphere, where
 * the layer's map puts its centre, past the layer's square (a couple of texels kept spare), its casters' reach (SUN_BACK
 * sunward of it) past the layer's depth - or under 0.6 of the layer's size (the camera came in close: sharper drawn again) */
static int near_stale(const SunCascade* cn, const float* L, uint32_t size, uint32_t hold)
{
    const SunCascade* k = &g_still_near.k;
    uint32_t why = 0;
    if (!g_still_near.serial || g_still_near.seen + 1 != g_serial)
        why |= WHY_GAP;
    else if (g_serial - g_still_near.serial > hold)
        why |= WHY_AGE;
    if (size != g_still_near.size || g_fxs.sun_casters != g_still_near.casters || g_fxs.sun_soft != g_still_near.soft ||
        g_fxs.sun_plants != g_still_near.plants)
        why |= WHY_SETTINGS;
    if (L[0] * g_still_near.L[0] + L[1] * g_still_near.L[1] + L[2] * g_still_near.L[2] < 0.999999f)
        why |= WHY_SUN;
    if (!why)
    {
        const float R = 0.5f * k->across, Rn = 0.5f * cn->across, *c = cn->c;
        if (Rn < 0.6f * R)
            why |= WHY_SHRUNK;
        else
        {
            const float x = c[0] * k->S[0] + c[1] * k->S[4] + c[2] * k->S[8] + k->S[12];
            const float y = c[0] * k->S[1] + c[1] * k->S[5] + c[2] * k->S[9] + k->S[13];
            const float z = (c[0] * k->S[2] + c[1] * k->S[6] + c[2] * k->S[10] + k->S[14]) * k->range; /* from the layer's sunward end */
            const float side = (R - Rn - 2.0f * k->texel) / R; /* how far off the layer's centre the slice's may stand, over R */
            if (fabsf(x) > side || fabsf(y) > side || z - Rn - SUN_BACK < 0.0f || z + Rn > k->range)
                why |= WHY_MOVED;
        }
    }
    g_stale_why = why;
    return why != 0;
}

/* A still layer drawn again (sun_map; why: g_stale_why): counted by why for fx_stats, and said to the flicker watch where it was cut
 * through a sphere of another radius than the layer before's. Its texels then change size, so every shadow edge in the map falls
 * on a new grid and moves by up to a texel (the sharp map's radius goes by whole units of its reach, which follows the camera's
 * distance from the player; a layer cut through the same sphere only ever moves by whole texels). was, now: the layers' widths
 * (0: none before); size: the map's texels across */
static void sun_layer_note(int sharp, float was, float now, uint32_t size, uint32_t why, float reach, float to)
{
    uint64_t* n = sharp ? g_sst.why_near : g_sst.why_far;
    for (int i = 0; i < 7; ++i)
        if (why & (1u << i))
            n[i]++;
    g_fx.st_redrawn |= sharp ? 2u : 1u;
    if (sharp)
        g_fx.st_why_near = why;
    else
        g_fx.st_why_far = why;
    if (was <= 0.0f || fabsf(now - was) <= 0.5f)
        return;
    if (sharp)
        g_sst.radius_near++;
    else
        g_sst.radius_far++;
    char text[96];
    sun_why_text(why, text, sizeof text);
    gfx_watch("the %s map's layer was drawn again (%s) through a sphere of radius %.0f units, was %.0f: its texels went %.4f -> %.4f "
              "units, so every shadow edge in it moved onto a new grid (reach %.1f, the player %.1f from the camera)",
        sharp ? "sharp" : "far", text, 0.5f * now, 0.5f * was, was / (float)size, now / (float)size, reach, to);
}

/* the sharp map's radius, its reach and the player's distance from the camera, for fx_stats' least and most */
static void sun_range_note(float near_r, float reach, float to)
{
    if (!g_sst.range_set)
    {
        g_sst.range_set = 1;
        g_sst.near_lo = g_sst.near_hi = near_r, g_sst.reach_lo = g_sst.reach_hi = reach, g_sst.to_lo = g_sst.to_hi = to;
        return;
    }
    g_sst.near_lo = fminf(g_sst.near_lo, near_r), g_sst.near_hi = fmaxf(g_sst.near_hi, near_r);
    g_sst.reach_lo = fminf(g_sst.reach_lo, reach), g_sst.reach_hi = fmaxf(g_sst.reach_hi, reach);
    g_sst.to_lo = fminf(g_sst.to_lo, to), g_sst.to_hi = fmaxf(g_sst.to_hi, to);
}

/* sun_gate said to the log when it changes, and both still layers drawn again at once (a layer drawn with a part off went on
 * serving the map until its age ran out) */
static void sun_gate_note(uint32_t gate)
{
    static uint32_t told;
    if (gate == told)
        return;
    static const char* const NAMES[] = { "smooth normals", "second look on faces turned away", "edge-on fade", "shading by the angle",
        "maps' fading edge", "contact shadows", "everything but one look in the maps (simplest)", "normal maps", "sharp map",
        "kept casters (the cache)", "culling", "still layers", "letting kept casters go when in plain view and not drawn" };
    char list[512] = "";
    for (uint32_t b = 0; b < sizeof NAMES / sizeof NAMES[0]; ++b)
        if (gate & (1u << b))
            snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? ", " : "", NAMES[b]);
    fprintf(stderr, "[recomp] gfx: shadows: parts turned off (sun_gate %u): %s\n", gate, list[0] ? list : "none");
    told = gate, g_still.serial = 0, g_still_near.serial = 0, g_still_mid.serial = 0;
}

/* The player's distance from the camera that the sharp map's reach is made from, held. The reach sets the radius of the sphere the
 * map is cut through (sun_fit: whole units), and a radius that steps changes the size of the map's texels - every shadow edge in it
 * falls on a new grid and moves a little: a flicker for as long as the camera moves in and out (a wall pushing it, the wheel; a
 * 10 s recording had the radius step 23..31 units while the player stood 4.5..9.3 from the camera, and stand still when it didn't
 * move). So the distance is rounded up to 3 units and kept at the most of the last 10 seconds: the radius grows when the camera
 * goes back past what it covers, and goes down only after 10 s closer. to < 0 (no player): as it is */
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

#include "gfx_sun_layers.h" /* wide_layer, near_layer: the still layers drawn whole, or (sun_roll) a slice a frame */

/* a still layer's hold, frames (sun_far_hold, sun_near_hold: 0 none, `most` at most), or none while sun_gate turns the layers off */
static uint32_t sun_hold(uint32_t gate, float setting, float most)
{
    return gate & SG_HOLD ? 0u : (uint32_t)fminf(fmaxf(setting, 0.0f), most);
}

#include "gfx_sun_mid.h" /* sun_mid_map: the middle map between the sharp one and the far one (sun_mid) */

/* The sun's maps for the scene: a near cascade over the first sun_near units the camera sees, where
 * characters stand and shadows are looked at closely (a 4096 map over some 40 units: texels of a
 * centimetre), a far one out to sun_distance, and with sun_mid a middle one between them. Fills the effects' uniforms for
 * them; 0 when nothing was drawn. */
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
    /* (round the player, sun_centre: the sharp map reaches sun_near round them, not past them from the camera) */
    if (g_fxs.sun_centre != 0.0f && s->focus[3] != 0.0f)
        reach = fmaxf(reach, 10.0f);
    else
        reach = to >= 0.0f ? reach + sun_to_held(to) : fmaxf(reach, 10.0f);
    {
        static int told;
        if (to >= 0.0f && !told)
            told = 1, fprintf(stderr, "[recomp] gfx: shadows: the player %.1f units from the camera: the sharp map reaches %.1f\n", to,
                reach);
    }
    float dnear = fminf(reach, dfar);
    const SunCtx cx = { s, invP, invV, L, dnear, to };
    SunCascade cfar, cnear;
    sun_fit(s, invV, L, 0.5f, dfar, SUN_MAP, 1.0f, &cfar);
    if (!sun_target(&g_fx.smap, SUN_MAP))
        return 0;
    gpu_part(GP_SUN_FAR);
    /* sun_far_hold: the far map's still casters drawn every few frames into a layer of their own, which the map starts from
     * each frame, the moving ones drawn over it; through the cascade the layer was drawn for (its sphere and the sun then),
     * looked at from this frame's camera */
    const SunCascade* kf = &cfar;
    uint32_t drawn;
    const uint32_t gate = sun_gate();
    sun_gate_note(gate);
    const uint32_t hold = sun_hold(gate, g_fxs.sun_far_hold, 15.0f);
    if (hold && sun_target(&g_fx.smaps, SUN_MAP))
    {
        wide_layer(&WIDE_FAR, &cx, &cfar, dfar, hold);
        kf = &g_still.k;
        sun_state(&g_fx.smaps, D3D12_RESOURCE_STATE_COPY_SOURCE);
        sun_state(&g_fx.smap, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyResource(list(), g_fx.smap.res, g_fx.smaps.res);
        drawn = g_still.drawn + sun_draw(&g_fx.smap, s, invP, invV, kf, 1, 0, SUN_MOVING);
        g_still.seen = g_serial;
        g_sst.still_frames++;
    }
    else
    {
        g_still.serial = 0;
        if (!hold && g_fx.smaps.res)
            sun_release(&g_fx.smaps), sun_release(&g_fx.smapsb); /* their 64 MB back while it's off */
        drawn = sun_draw(&g_fx.smap, s, invP, invV, &cfar, 1, 0, SUN_ALL);
    }
    gpu_part(GP_REST);
    for (int j = 0; j < 4; ++j) /* its footprint, for what the cache lets go of (cache_unseen) */
        g_far_xy[j] = kf->S[j * 4], g_far_xy[4 + j] = kf->S[j * 4 + 1];
    g_far_edge = 1.0f + SUN_REACH * 2.0f / kf->across, g_far_set = 1;
    mat_mul(u->lmat, invV, kf->S); /* this frame's view space to the map (sun_fit's lmat, for the cascade drawn) */
    srec_map(0, kf, kf != &cfar);
    u->smap[1] = kf->texel, u->smap[2] = kf->bias, u->smap[3] = kf->soft;
    u->smap2[0] = kf->slope, u->smap2[3] = kf->range;
    u->smapn2[1] = 0.0f;
    g_fx.st_near_r = 0.0f;
    const uint32_t nsize = sun_size(g_fxs.sun_detail);
    g_sun_cull = kf; /* the sharp map's casters culled by the far map's footprint: never fewer than the far map's (the middle one's too) */
    sun_mid_map(&cx, dfar, gate, u);
    if (dnear >= 2.0f && g_fxs.sun_sharp != 0.0f && !(gate & SG_NEAR) && sun_target(&g_fx.smapn, nsize))
    {
        sun_fit(s, invV, L, 0.5f, dnear, nsize, 1.0f, &cnear);
        gpu_part(GP_SUN_NEAR);
        /* sun_near_hold: the sharp map's still casters drawn into a layer of their own when it's stale (near_stale), which the
         * map starts from each frame, the moving ones drawn over it; through the layer's cascade */
        const SunCascade* kn = &cnear;
        const uint32_t nhold = sun_hold(gate, g_fxs.sun_near_hold, 600.0f);
        if (nhold && sun_target(&g_fx.smapns, nsize))
        {
            near_layer(&cx, &cnear, nsize, nhold);
            kn = &g_still_near.k;
            sun_state(&g_fx.smapns, D3D12_RESOURCE_STATE_COPY_SOURCE);
            sun_state(&g_fx.smapn, D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12GraphicsCommandList_CopyResource(list(), g_fx.smapn.res, g_fx.smapns.res);
            sun_draw(&g_fx.smapn, s, invP, invV, kn, 1, 0, SUN_MOVING);
            g_still_near.seen = g_serial;
            g_sst.near_frames++;
        }
        else
        {
            g_still_near.serial = 0;
            if (!nhold && g_fx.smapns.res)
                sun_release(&g_fx.smapns), sun_release(&g_fx.smapnsb); /* their 64 MB (256 at 8192) back while it's off */
            sun_draw(&g_fx.smapn, s, invP, invV, &cnear, 1, 0, SUN_ALL);
        }
        gpu_part(GP_REST);
        mat_mul(u->lmatn, invV, kn->S); /* this frame's view space to the map (sun_fit's lmat, for the cascade drawn) */
        srec_map(1, kn, kn != &cnear);
        u->smapn[0] = kn->texel, u->smapn[1] = kn->bias, u->smapn[2] = kn->soft, u->smapn[3] = kn->slope;
        u->smapn2[0] = kn->range, u->smapn2[1] = 1.0f;
        g_fx.st_near_r = 0.5f * kn->across;
    }
    g_sun_cull = NULL;
    g_fx.st_across = kf->across;
    g_fx.st_cached = g_ncache;
    g_fx.st_far_r = 0.5f * kf->across, g_fx.st_reach = dnear, g_fx.st_to = to;
    sun_range_note(g_fx.st_near_r, dnear, to);
    return drawn != 0;
}

/* Which pixels are a character, for the softer shadows on them (sun_soft_chars): this frame's characters alone drawn again
 * through the camera - their own clip space, M the identity - into a depth of their own, stretched square over the
 * viewport. fx_ao takes a pixel as a character's where this depth is the scene's (sun_char). The cascade's across 0: none
 * culled by beyond_map (its edge is infinite), the camera's frustum is the game's to cull. Fills u->smapn2.zw; 0 when
 * nothing was drawn. */
#define VIEW_MAP 2048
static int sun_map_view(const GfxScene* s, FxU* u)
{
    if (!g_ncasters)
        return 0;
    float invP[16], invV[16];
    if (!mat_inverse(invP, s->proj) || !mat_inverse(invV, s->view))
        return 0;
    SunCascade c;
    memset(&c, 0, sizeof c);
    mat_mul(c.S, s->view, s->proj);
    if (!sun_target(&g_fx.smapv, VIEW_MAP))
        return 0;
    gpu_part(GP_SUN_CHARS);
    const uint32_t drawn = sun_draw(&g_fx.smapv, s, invP, invV, &c, 0, 1, SUN_ALL);
    gpu_part(GP_REST);
    if (!drawn)
        return 0;
    /* z: on; w: how far the shadows on them are blurred (fx_blur), world units */
    u->smapn2[2] = 1.0f;
    u->smapn2[3] = fminf(g_fxs.sun_soft_chars, 0.2f);
    return 1;
}

void gfx_trace_dump(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "frame own-sun strength day live cached no-pipeline map-across sun(x y z) camera(x y z) depth(1 as drawn, 2 another since) "
               "sun-light ambient fog(r g b) ran history(1 blended, 0 no shadows, 2 gap, 3 size, 4 camera moved, 5 off) rays "
               "sun-held far-radius sharp-radius sharp-reach player-distance layers-drawn(1 far, 2 sharp) why-far why-sharp(1 age, 2 gap, "
               "4 settings, 8 sun, 16 radius, 32 moved, 64 shrunk)\n");
    uint32_t n = g_fx.ntrace < 1200 ? g_fx.ntrace : 1200;
    for (uint32_t k = g_fx.ntrace - n; k < g_fx.ntrace; ++k)
    {
        const struct SunTrace* t = &g_fx.trace[k % 1200];
        fprintf(f, "%llu %u %.2f %.2f %u %u %u %.0f %.3f %.3f %.3f %.1f %.1f %.1f %u %.3f %.3f %.3f %.3f %.3f %u %u %.2f %u "
                   "%.0f %.0f %.1f %.1f %u %u %u\n",
            (unsigned long long)t->serial, t->own, t->strength, t->day, t->live, t->cached, t->skipped, t->across, t->sun[0], t->sun[1],
            t->sun[2], t->cam[0], t->cam[1], t->cam[2], t->depth, t->dl, t->al, t->fog[0], t->fog[1], t->fog[2], t->ran, t->hist,
            t->rays, t->held, t->far_r, t->near_r, t->reach, t->to, t->redrawn, t->why_far, t->why_near);
    }
    fclose(f);
}

/* the shadow settings in force and the size of the scene they're drawn at, said to the log at the first scene and whenever they
 * change (a flicker report's context: "d3d12", the reach, the sharp map's size, the holds, the softness) */
static void sun_setup_note(const GfxTex* color)
{
    const float v[] = { g_fxs.sun, g_fxs.sun_distance, g_fxs.sun_near, g_fxs.sun_detail, g_fxs.sun_soft, g_fxs.sun_soft_zone,
        g_fxs.sun_soft_chars, g_fxs.sun_min, g_fxs.sun_face, g_fxs.sun_rings, g_fxs.sun_far_hold, g_fxs.sun_near_hold, g_fxs.sun_fade,
        g_fxs.sun_plants, g_fxs.sun_casters, g_fxs.sun_direct, g_fxs.sun_dusk, g_fxs.temporal, g_fxs.sun_mid };
    static float last[sizeof v / sizeof v[0]];
    static int told;
    if (told && !memcmp(last, v, sizeof v))
        return;
    told = 1, memcpy(last, v, sizeof v);
    fprintf(stderr, "[recomp] gfx: shadows: setup (d3d12): sun %.2f, reach %.0f, middle map %.0f, sharp past the player %.0f at %.0f "
                    "texels, softness %.3f "
                    "(zone %.2f, characters %.3f), ignore casters under %.1f, shade faces turned away %.2f, rings %s, holds far %.0f sharp %.0f, "
                    "fade %.0f s, plants %s, casters %.0f, fade under cloud %.2f, dusk %s, temporal %.2f, a scene of %ux%u\n", v[0], v[1], v[18],
        v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9] != 0.0f ? "on" : "off", v[10], v[11], v[12], v[13] != 0.0f ? "on" : "off", v[14], v[15],
        v[16] != 0.0f ? "on" : "off", v[17], color->w, color->h);
}

/* the flicker watch's look at a scene (gfx_watch): what differs from the frame before */
static void fx_watch_frame(const GfxScene* s, const struct SunTrace* t, const GfxTex* color, const GfxTex* depth, int ready, int fmt)
{
    static const char* const HIST[] = { "no shadows to blend", "", "the frame before had none", "its size changed", "the camera moved",
        "temporal is off" };
    int prev = g_watch.serial && g_watch.serial + 1 == g_serial;
    float dx = t->cam[0] - g_watch.cam[0], dy = t->cam[1] - g_watch.cam[1], dz = t->cam[2] - g_watch.cam[2];
    int jump = dx * dx + dy * dy + dz * dz > 25.0f; /* a cut or a warp: everything changes, rightly */
    /* what the frame's effects were made from, for the toggle finder */
    gfx_watch_value("sun: map shadows' strength", t->strength);
    gfx_watch_value("sun: how much of the game's light is the sun's (day)", t->day);
    gfx_watch_value("sun: a lit draw's own light", (float)t->own);
    gfx_watch_value("sun: another light held back", (float)t->held);
    gfx_watch_value("sun: light's brightness", t->dl);
    gfx_watch_value("sun: ambient's brightness", t->al);
    gfx_watch_value("sun: direction x", t->sun[0]);
    gfx_watch_value("sun: direction y", t->sun[1]);
    gfx_watch_value("sun: direction z", t->sun[2]);
    /* the sun runs in the world's x-y plane (z 0): a wall edge-on to it (its normal along z) is lit or turned away by z's sign
     * alone, so z's tiny turns (millionths) are fed too */
    gfx_watch_value("sun: direction z, in millionths", t->sun[2] * 1e6f);
    gfx_watch_value("sun: direction z's sign", t->sun[2] > 0.0f ? 1.0f : t->sun[2] < 0.0f ? -1.0f : 0.0f);
    gfx_watch_value("sun: direction x, in millionths", t->sun[0] * 1e6f);
    gfx_watch_value("sun: direction y, in millionths", t->sun[1] * 1e6f);
    gfx_watch_value("fog: red", t->fog[0]);
    gfx_watch_value("fog: green", t->fog[1]);
    gfx_watch_value("fog: blue", t->fog[2]);
    gfx_watch_value("fog: on at the camera draw", s->fog[2]);
    gfx_watch_value("fog: start at the camera draw", s->fog[0]);
    gfx_watch_value("fog: end at the camera draw", s->fog[1]);
    gfx_watch_value("god rays' strength", t->rays);
    gfx_watch_value("effects: ran", (float)t->ran);
    gfx_watch_value("effects: shadows blended with the frame before (history)", (float)t->hist);
    gfx_watch_value("effects: shadow casters left out", (float)t->skipped);
    gfx_watch_value("effects: depth buffer (1 as drawn, 2 another since)", (float)t->depth);
    gfx_watch_value("scene: camera taken from draw", (float)s->cam_draw);
    gfx_watch_value("scene: sun taken from draw", (float)s->sun_draw);
    gfx_watch_value("scene: indoors", s->indoors);
    gfx_watch_value("scene: home point teleport (warping)", (float)s->warping);
    /* the maps' cascades: a sphere that changes radius changes the size of its map's texels, and every edge in it moves */
    gfx_watch_value("shadows: the far map's sphere radius", t->far_r);
    gfx_watch_value("shadows: the sharp map's sphere radius", t->near_r);
    gfx_watch_value("shadows: the sharp map's reach", t->reach);
    gfx_watch_value("shadows: the player's distance from the camera", t->to);
    gfx_watch_value("camera: moved since the frame before", prev ? sqrtf(dx * dx + dy * dy + dz * dz) : 0.0f);
    sun_setup_note(color);
    if (!t->ran && prev && g_watch.ran)
    {
        char why[128];
        if (!ready)
            snprintf(why, sizeof why, "their shaders are still building");
        else if (fmt < 0 || !g_fx.by_fmt[fmt].comp)
            snprintf(why, sizeof why, "no pipeline for the scene's format %d", (int)color->dxfmt);
        else if (!depth)
            snprintf(why, sizeof why, "no depth buffer was drawn with the scene");
        else if (depth->w != color->w || depth->h != color->h)
            snprintf(why, sizeof why, "the depth buffer is %ux%u, the scene %ux%u", depth->w, depth->h, color->w, color->h);
        else if (depth->srv < 0)
            snprintf(why, sizeof why, "the depth buffer can't be read");
        else if (s->proj[11] == 0.0f)
            snprintf(why, sizeof why, "the camera's projection isn't a perspective one");
        else
            snprintf(why, sizeof why, "an effect target couldn't be made");
        gfx_watch("the scene effects didn't run (%s): no grade, fog, shadows, bloom or god rays this frame", why);
    }
    if (t->ran && prev && g_watch.ran && !jump)
    {
        float c = t->sun[0] * g_watch.sun[0] + t->sun[1] * g_watch.sun[1] + t->sun[2] * g_watch.sun[2];
        int had = g_watch.sun[0] != 0.0f || g_watch.sun[1] != 0.0f || g_watch.sun[2] != 0.0f; /* not the first sun */
        if (had && c < 0.99985f)
            gfx_watch("the sun turned %.1f degrees (%s): the shadows, god rays and fog's glow turn with it",
                acosf(fmaxf(fminf(c, 1.0f), -1.0f)) * 57.29578f, s->sun_dir[3] != 0.0f ? "a lit draw's light" : "no lit draw");
        if (fabsf(t->strength - g_watch.smap) > 0.05f)
            gfx_watch("the map shadows went %.2f -> %.2f (sun up %.2f; casters drawn %u, from the cache %u, left out while their "
                      "pipelines build %u)", g_watch.smap, t->strength, t->day, t->live, t->cached, t->skipped);
        else if (t->skipped)
            gfx_watch("%u shadow casters left out: their shadow pipelines are still building", t->skipped);
        if (fabsf(t->rays - g_watch.rays) > 0.1f)
            gfx_watch("the god rays went %.2f -> %.2f", g_watch.rays, t->rays);
        if (g_watch.hist == 1 && t->hist >= 2 && t->hist <= 4)
            gfx_watch("the frame before wasn't blended in (%s): this frame's shadows are unsmoothed", HIST[t->hist]);
        if ((s->fog[2] != 0.0f) != (g_watch.fog != 0))
            gfx_watch("the camera draw's fog went %s: the effects' fog fades %s", s->fog[2] != 0.0f ? "on" : "off",
                s->fog[2] != 0.0f ? "in" : "out");
        if (depth != g_watch.depth || t->depth != g_watch.depth_kind)
            gfx_watch("the effects read another depth buffer (%s)", t->depth == 2 ? "the one the world was drawn with, another since" : "the last one drawn with");
        /* a map cut through a sphere of another radius with no layer drawn again (those say it themselves, sun_layer_note): the
         * camera moved the player's distance (the sharp map's reach follows it), or the sharp map came in or went */
        if (g_watch.far_r > 0.0f && t->far_r > 0.0f && !(t->redrawn & 1) && fabsf(t->far_r - g_watch.far_r) >= 0.5f)
            gfx_watch("the far map's sphere went radius %.0f -> %.0f units: its texels changed size, and every shadow edge in it moved "
                      "onto a new grid (the camera moved %.2f units)", g_watch.far_r, t->far_r, sqrtf(dx * dx + dy * dy + dz * dz));
        if (g_watch.near_r > 0.0f && t->near_r > 0.0f && !(t->redrawn & 2) && fabsf(t->near_r - g_watch.near_r) >= 0.5f)
            gfx_watch("the sharp map's sphere went radius %.0f -> %.0f units (reach %.1f, the player %.1f from the camera): its texels "
                      "changed size, and every shadow edge in it moved onto a new grid (the camera moved %.2f units)", g_watch.near_r,
                t->near_r, t->reach, t->to, sqrtf(dx * dx + dy * dy + dz * dz));
        if ((g_watch.near_r > 0.0f) != (t->near_r > 0.0f))
            gfx_watch("the sharp map %s: the shadows near the player are cut from %s", t->near_r > 0.0f ? "came back" : "went",
                t->near_r > 0.0f ? "its texels again" : "the far map's texels");
    }
    g_watch.far_r = t->far_r, g_watch.near_r = t->near_r;
    g_watch.serial = g_serial, g_watch.ran = t->ran, g_watch.hist = t->hist, g_watch.fog = s->fog[2] != 0.0f;
    g_watch.depth = depth, g_watch.depth_kind = t->depth, g_watch.cam_draw = s->cam_draw;
    memcpy(g_watch.sun, t->sun, 12), memcpy(g_watch.cam, t->cam, 12);
    g_watch.smap = t->strength, g_watch.rays = t->rays;
}

#ifdef VANACORE
/* FXAA over the whole finished scene: its copy, then the pass from the copy onto it */
static void fx_antialias(GfxTex* color, int fmt)
{
    if (!g_fx.by_fmt[fmt].fxaa || !fx_tex(&g_fx.src, color->dxfmt, color->w, color->h))
        return;
    ID3D12GraphicsCommandList* l = list();
    const int scope_was = gpu_scope_cur(), part_was = gpu_part_cur();
    gpu_scope(GS_FXAA);
    gpu_part(GP_COPY);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    from.pResource = color->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, from.SubresourceIndex = 0;
    to.pResource = g_fx.src.res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, to.SubresourceIndex = 0;
    tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
    fx_state(&g_fx.src, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &from, NULL);
    fx_state(&g_fx.src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float vp[4] = { 0.0f, 0.0f, (float)color->w, (float)color->h };
    gpu_part(GP_FXAA);
    fx_draw(target_view(color, 0, 0), color->w, color->h, g_fx.by_fmt[fmt].fxaa, vp, NULL, &g_fx.src.srv, 1, 0, 0);
    gpu_scope(scope_was);
    gpu_part(part_was);
}
#endif

/* VanaCore's glow ([graphics] glow=custom: d3d8.c glow_draw, in place of the game's): the character's picture
 * src, drawn alone by the game, blurred at a quarter of its size (its chain's level 2: a 9-tap gaussian across,
 * then down, its colour premultiplied) and laid over the target being drawn in the tint - one blended pass, where
 * the game laid its 256x256 copy down seven times */
static uint32_t g_glow_calls; /* the glow laid this frame (gfx_glow), for the toggle finder */
static float g_glow_alpha;

/* VanaCore's glow just blurred (glow[1], w x h; its uniforms u) into the frame's layer of them too (cleared at the frame's first),
 * for the grass drawn over them once the world is done: the glow is laid partway through it (gfx_glow_over_grass) */
static void glow_keep(uint32_t w, uint32_t h, const FxU* u)
{
    if (!g_fx.glow_keep)
        return;
    if (g_fx.glow_kept.w != w || g_fx.glow_kept.h != h)
        g_fx.glow_kept_serial = 0;
    if (!fx_tex(&g_fx.glow_kept, DXGI_FORMAT_R16G16B16A16_FLOAT, w, h))
        return;
    fx_state(&g_fx.glow_kept, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float none[4] = { 0, 0, 0, 0 }, vp[4] = { 0, 0, (float)w, (float)h };
    if (g_fx.glow_kept_serial != g_serial)
        ID3D12GraphicsCommandList_ClearRenderTargetView(list(), heap_cpu(&g_rtv, g_fx.glow_kept.rtv), none, 0, NULL);
    fx_draw(heap_cpu(&g_rtv, g_fx.glow_kept.rtv), w, h, g_fx.glow_keep, vp, u, &g_fx.glow[1].srv, 1, 0, 0);
    fx_state(&g_fx.glow_kept, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_fx.glow_kept_serial = g_serial;
}

void gfx_glow(GfxTex* src, const float tint[4])
{
    BATCH_SETTLE();
    if (!g_dev || !src || !g_rt || src->type != GFX_TEX_2D || src->srv_mips < 0 || src->mips < 3 || !fx_ready())
    {
        static int said; /* why the glow is missing, once: it went quiet with DLSS on */
        if (src && src->type == GFX_TEX_2D && (src->srv_mips < 0 || src->mips < 3) && !said)
            said = 1, fprintf(stderr, "[recomp] gfx: the trusts' and avatars' glow (glow=custom) isn't drawn: the picture it's "
                "made from (%ux%u) has no mip chain\n", src->w, src->h);
        return;
    }
    int fmt = fx_by_fmt(g_rt->dxfmt);
    ID3D12PipelineState* mp = fx_mip_pso(src->dxfmt);
    if (fmt < 0 || !g_fx.by_fmt[fmt].glow || !mp)
        return;
    const int part_was = gpu_part_cur();
    gpu_part(GP_GLOW);
    g_glow_calls++, g_glow_alpha = tint[3];
    if (!src->scene) /* its chain, from the picture as it is now */
    {
        fx_mips(src, mp);
        src->scene = g_serial;
    }
    /* glow.size widens the blur's step; a step past the quality's limit (its taps then show as separate copies) is
     * halved instead, one level further down the chain, whose texels are already averaged; higher qualities also
     * blur again (glow.quality 0-3: step limit 2.5, 1.5, 1 and 0.75 texels, 1, 1, 2 and 3 rounds) */
    static const float STEP_LIMIT[4] = { 2.5f, 1.5f, 1.0f, 0.75f };
    const int q = (int)fminf(fmaxf(g_fxs.glow_quality + 0.5f, 0.0f), 3.0f);
    float step = 1.5f * fminf(fmaxf(g_fxs.glow_size, 0.1f), 4.0f);
    uint32_t lv = 2;
    while (step > STEP_LIMIT[q] && lv < 6 && lv + 1 < (uint32_t)src->mips)
        lv++, step *= 0.5f;
    const uint32_t w = (src->w >> lv) ? (src->w >> lv) : 1, h = (src->h >> lv) ? (src->h >> lv) : 1;
    const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!fx_tex(&g_fx.glow[0], RGBA, w, h) || !fx_tex(&g_fx.glow[1], RGBA, w, h))
    {
        gpu_part(part_was);
        return;
    }
    FxU u;
    memset(&u, 0, sizeof u);
    memcpy(u.glow, tint, sizeof u.glow);
    u.glow[3] *= fmaxf(g_fxs.glow_strength, 0.0f);
    u.glow2[0] = (float)lv, u.glow2[1] = step, u.glow2[2] = 1.0f;
    fx_pass(&g_fx.glow[0], FX_GLOWB, &u, &src->srv_mips, 1, 1, 0);
    u.glow2[0] = 0.0f, u.glow2[2] = 0.0f;
    fx_pass(&g_fx.glow[1], FX_GLOWB, &u, &g_fx.glow[0].srv, 1, 0, 1);
    for (int again = 1; again < (q == 0 ? 1 : q); ++again) /* more rounds: smoother, over its own result */
    {
        fx_pass(&g_fx.glow[0], FX_GLOWB, &u, &g_fx.glow[1].srv, 1, 1, 0);
        fx_pass(&g_fx.glow[1], FX_GLOWB, &u, &g_fx.glow[0].srv, 1, 0, 1);
    }
    glow_keep(w, h, &u);
    uint32_t tw, th;
    level_size(g_rt, g_rt_level, &tw, &th);
    tex_state(g_rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float vp[4] = { 0, 0, (float)tw, (float)th };
    fx_draw(target_view(g_rt, g_rt_face, g_rt_level), tw, th, g_fx.by_fmt[fmt].glow, vp, &u, &g_fx.glow[1].srv, 1, 0, 0);
    gpu_part(part_was);
}

#include "gfx_d3d12_sky.h" /* gfx_sky, sky_hlsl: VanaCore's sky in place of the game's dome */
#include "gfx_d3d12_occl.h" /* occl_scene: the zone's pieces tested against the scene's depth ([graphics] occl) */

#ifdef VANACORE
/* --- The light from the effects (VanaCore) -----------------------------------------------------------------------
 * FFXI lights its world with the sun, the moon and the zone's own torches; a fire, a spell, a home point's crystal are
 * glowing sprites that light nothing. Here they do, in screen space: after the scene's effects (el_note) each additive
 * draw the game makes into it - flames, glows, spells: what adds light, not smoke or the zone's alpha-blended sheets - is
 * drawn again into a target of its own (el_emit); once the game has drawn the rest of its world (el_on_draw: when DLSS
 * would run), that light is taken small (fx_elx), blurred over five levels of doubling reach, and the scene lit by it
 * (fx_elc, from a copy): each surface near a light, by its distance and the light's, lit in its own colour.
 * [graphics.effects] elight, elight_radius; Render visual 9 shows the lights and what they add. */
static struct
{
    GfxTex* emit;        /* the additive draws after the effects, again, on black (the scene's size and format: their
                          * pipelines draw into it as they are) */
    GfxTex* part[4];     /* ... alone, made when one is drawn (el_emit): the lanterns' flames (GfxDraw.el_lantern 1), the
                          * dimmed effects (2) and the characters' effects (3), each taken at its share (fx_elx); and
                          * the ones DLSS 5 isn't to take as lights (GfxDraw.dlss5_skip), taken out of its glow (fx_dlssg) */
    uint64_t part_serial[4]; /* the frame each was cleared for */
    FxTex x;             /* their light, at about a quarter size (fx_elx) */
    FxTex lv[7], tmp[7]; /* ... at half that and smaller, each blurred (tmp: across, before down); five read */
    FxU u;               /* the effects' uniforms for the scene: its projection, viewport and sizes */
    GfxTex *color, *depth;
    int fmt;
    uint32_t present_w, present_h; /* the scene's size on the screen: a read into less isn't it laid onto the frame */
    uint64_t pend_serial;          /* the frame noted (el_note), till el_run */
    uint64_t post_serial;          /* ... left for DLSS's output (dlss5_elight_post: el_on_draw), till dlss_run lays it */
    uint64_t emit_serial;         /* the frame `emit` is of: drawn into till el_run, or with hint till the scene's through DLSS */
    uint64_t early_serial;        /* ... begun before the effects, at an effect's own model (GfxDraw.el_early: el_begin) */
    int hint;                    /* DLSS 5 reads it too, as its lights (dlss5_fx_light: el_note) */
    uint32_t warping;              /* GfxScene.warping, as noted */
    float warp_k;                  /* its strength's share now, easing to elight_warp while warping and back to 1 after */
    uint64_t warp_ns;              /* ... when it was last eased */
    uint32_t taken;                /* the additive draws taken as lights this frame (el_emit), for the toggle finder */
    uint64_t ran_serial;           /* the frame el_run last lit a scene */
} g_el;

/* After the effects (gfx_scene_done, the uniforms in g_el.u): the target the frame's additive draws go into again,
 * cleared. light: the scene is lit by them (el_run; 0: nor do its uniforms need to be there); hint: DLSS 5 reads them too
 * (dlss5_fx_light: their picture past white in its colour, dlss_run) */
/* The emit target for the scene `color`, its size and format, cleared. 0: none */
static int el_clear(GfxTex* color)
{
    if (g_el.emit && (g_el.emit->w != color->w || g_el.emit->h != color->h || g_el.emit->fmt != color->fmt))
        gfx_tex_destroy(g_el.emit), g_el.emit = NULL;
    if (!g_el.emit)
        g_el.emit = gfx_tex_create(GFX_TEX_2D, color->fmt, color->w, color->h, 1, GFX_USE_RT);
    if (!g_el.emit || g_el.emit->srv < 0)
        return 0;
    static const float black[4] = { 0, 0, 0, 0 };
    tex_state(g_el.emit, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list(), target_view(g_el.emit, 0, 0), black, 0, NULL);
    return 1;
}

/* An effect's own model drawn before the world's scene ended (GfxDraw.el_early, el_emit): the emit target begun at it, so
 * it's a light every frame and not only when the game's order put it after the first particle (Hu'Xzoi's door glow) */
static void el_begin(GfxTex* color)
{
    g_el.emit_serial = g_el.early_serial = 0;
    if (color->srv < 0 || !el_clear(color))
        return;
    g_el.color = color;
    g_el.emit_serial = g_el.early_serial = g_serial;
}

static void el_note(GfxTex* color, GfxTex* depth, int fmt, const GfxScene* s, int light, int hint)
{
    /* begun at an effect's own model this frame (el_begin): what it took is kept */
    const int begun = g_el.early_serial == g_serial && g_el.emit_serial == g_serial && g_el.color == color && g_el.emit &&
                      g_el.emit->w == color->w && g_el.emit->h == color->h && g_el.emit->fmt == color->fmt;
    g_el.pend_serial = g_el.post_serial = g_el.emit_serial = g_el.early_serial = 0;
    if (color->srv < 0 || (light && (!depth || depth->srv < 0 || !g_fx.pso[FX_ELX] || !g_fx.by_fmt[fmt].elight)))
        return;
    if (!begun && !el_clear(color))
        return;
    g_el.color = color, g_el.depth = depth, g_el.fmt = fmt;
    g_el.present_w = s->present_w, g_el.present_h = s->present_h;
    g_el.warping = s->warping;
    g_el.pend_serial = light ? g_serial : 0;
    g_el.emit_serial = g_serial;
    g_el.hint = hint;
}

/* The light from the effects off: its targets let go */
static void el_off(void)
{
    g_el.pend_serial = g_el.post_serial = g_el.emit_serial = g_el.early_serial = 0;
    if (g_el.emit)
        gfx_tex_destroy(g_el.emit), g_el.emit = NULL;
    for (int k = 0; k < 4; ++k)
    {
        if (g_el.part[k])
            gfx_tex_destroy(g_el.part[k]), g_el.part[k] = NULL;
        g_el.part_serial[k] = 0;
    }
}

/* The share each part's light is taken at: 0 the lanterns' (elight_lantern), 1 the dimmed effects' (elight_dim). The
 * characters' effects (part 2) are no share: a strength of their own (elight_actor, fx_elx) */
static float el_share(int k)
{
    return k ? g_fxs.elight_dim : g_fxs.elight_lantern;
}

/* A part's target for this frame (0 the lanterns', 1 the dimmed effects', 2 the characters' effects', 3 those DLSS 5 isn't
 * to take as lights), made if there isn't one (the emit target's size) and cleared once. NULL: none */
static GfxTex* el_part(int k)
{
    GfxTex* e = g_el.emit;
    GfxTex** t = &g_el.part[k];
    if (*t && ((*t)->w != e->w || (*t)->h != e->h || (*t)->fmt != e->fmt))
        gfx_tex_destroy(*t), *t = NULL, g_el.part_serial[k] = 0;
    if (!*t)
        *t = gfx_tex_create(GFX_TEX_2D, e->fmt, e->w, e->h, 1, GFX_USE_RT);
    if (!*t || (*t)->srv < 0)
        return NULL;
    if (g_el.part_serial[k] != g_serial)
    {
        static const float black[4] = { 0, 0, 0, 0 };
        tex_state(*t, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list(), target_view(*t, 0, 0), black, 0, NULL);
        g_el.part_serial[k] = g_serial;
    }
    return *t;
}

/* After a draw (gfx_draw): one of the game's that adds light into the noted scene - 3D, blended with the destination
 * kept whole and added to (D3DBLEND_ONE, D3DBLENDOP_ADD), not the interface - drawn again into the light's own target,
 * against the scene's depth as it was. Till el_run; for DLSS 5 (g_el.hint), till the scene is through DLSS: an effect the
 * game draws after the names over heads is still one it's to see as a light (dlss5_fx_light) */
static void el_emit(const GfxDraw* d)
{
    /* an effect's own model before the scene ended, with the light on: the target begun at the first of them (el_begin) */
    if (d->el_early && g_el.emit_serial != g_serial && g_rt && !g_rt_face && !g_rt_level && g_fxs.fx != 0.0f &&
        g_fxs.elight > 0.0f && g_fxs.elight_radius > 0.0f)
        el_begin(g_rt);
    if (g_el.emit_serial != g_serial || g_rt != g_el.color || g_rt_face || g_rt_level || !g_el.emit || d->vs.rhw ||
        d->overlay || d->screen_fx || !d->pipe.blend || d->pipe.dst != 2 || d->pipe.op != 1)
        return;
    g_el.taken++;
    GfxTex* rt = g_rt;
    GfxTex* seen = g_el.emit->depth_seen;
    g_rt = g_el.emit, g_targets_bound = 0;
    draw_encode(d);
    g_el.emit->depth_seen = seen;
    g_rt = rt, g_targets_bound = 0;
    /* a lantern's flame, a dimmed effect or a character's effect (el_lantern): drawn again alone, so the light takes only its
     * share of it (fx_elx) */
    if (d->el_lantern == 3 || (d->el_lantern && d->el_lantern <= 2 && el_share(d->el_lantern - 1) != 1.0f))
    {
        GfxTex* lant = el_part(d->el_lantern - 1);
        if (lant)
        {
            GfxTex* lseen = lant->depth_seen;
            g_rt = lant, g_targets_bound = 0;
            draw_encode(d);
            lant->depth_seen = lseen;
            g_rt = rt, g_targets_bound = 0;
        }
    }
    /* one DLSS 5 isn't to take as a light (dlss5_skip_tex): drawn again alone too, for its glow to leave out (fx_dlssg); the
     * scene's own light from the effects keeps it */
    if (d->dlss5_skip && g_el.hint)
    {
        GfxTex* skip = el_part(3);
        if (skip)
        {
            GfxTex* sseen = skip->depth_seen;
            g_rt = skip, g_targets_bound = 0;
            draw_encode(d);
            skip->depth_seen = sseen;
            g_rt = rt, g_targets_bound = 0;
        }
    }
}

/* --- The zone lines, late (VanaCore) -------------------------------------------------------------------------------
 * The effects' compose runs at the first of the game's unfogged blended draws, and the world goes on after it - the
 * zone's own late draws (a lit overlay on a bridge's deck) went over the lines laid in it. So the lines (u.zlc, zl_note)
 * wait: once the game has drawn the rest of its world (zl_on_draw, as el_on_draw), the lines are laid over the scene,
 * against its depth as it is then (zl_run). */
static struct
{
    GfxTex *color, *depth;
    int fmt;
    FxU u;                /* the scene's uniforms, with the lines in them */
    uint32_t present_w, present_h;
    uint64_t pend_serial; /* the frame noted (zl_note), till zl_run */
    int mask;             /* the characters' mask (g_fx.cmask) was made for that frame: an aura's glow is by it */
} g_zl;

/* After the effects (gfx_scene_done): the lines of this frame's scene are to go over it once the world is drawn */
static void zl_note(GfxTex* color, GfxTex* depth, int fmt, const GfxScene* s)
{
    g_zl.pend_serial = 0;
    if (!depth || depth->srv < 0 || color->srv < 0 || fmt < 0 || !g_fx.by_fmt[fmt].zlate)
        return;
    g_zl.color = color, g_zl.depth = depth, g_zl.fmt = fmt; /* (g_zl.u: set where the scene's uniforms are) */
    g_zl.present_w = s->present_w, g_zl.present_h = s->present_h;
    g_zl.mask = g_fx.cmask_serial == g_serial;
    g_zl.pend_serial = g_serial;
}

static void zl_run(void)
{
    GfxTex *color = g_zl.color, *depth = g_zl.depth;
    if (!color || !depth || !g_fx.by_fmt[g_zl.fmt].zlate)
        return;
    FxU* u = &g_zl.u;
    const int scope_was = gpu_scope_cur(), part_was = gpu_part_cur();
    gpu_scope(GS_REFL);
    /* the sparkles each the colour of the scene where they're born (u->spp): a copy of it as it is now to read (input 0) */
    int32_t scene = -1;
    if (u->spc[0] > 0.0f && u->spp[0] > 0.0f && fx_tex(&g_fx.src, color->dxfmt, color->w, color->h))
    {
        gpu_part(GP_COPY);
        D3D12_TEXTURE_COPY_LOCATION to, from;
        memset(&to, 0, sizeof to);
        memset(&from, 0, sizeof from);
        from.pResource = color->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, from.SubresourceIndex = 0;
        to.pResource = g_fx.src.res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, to.SubresourceIndex = 0;
        tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
        fx_state(&g_fx.src, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, 0, 0, 0, &from, NULL);
        fx_state(&g_fx.src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        scene = g_fx.src.srv;
    }
    else
        u->spp[0] = 0.0f; /* their places' colours */
    /* an aura's glow (sparkles(), input 1): the characters' mask of this frame taken small and blurred - the shape of the
     * trusts' and avatars' glow, a screen-space blur of the character's own picture; without it (spp.z 0) the column's halo */
    int32_t halo = -1;
    if (u->spc[0] > 0.0f && u->spp[2] > 0.5f && g_zl.mask && g_fx.cmask.res && g_fx.cmask.w == color->w && g_fx.cmask.h == color->h &&
        g_fx.pso[FX_DOWN] && g_fx.pso[FX_GAUSS])
    {
        const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const uint32_t div = color->w > 2048 ? 4 : 2, aw = (color->w + div - 1) / div, ah = (color->h + div - 1) / div;
        if (fx_tex(&g_fx.amask[0], RGBA, aw, ah) && fx_tex(&g_fx.amask[1], RGBA, aw, ah))
        {
            gpu_part(GP_GLOW);
            fx_state(&g_fx.cmask, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            fx_pass(&g_fx.amask[0], FX_DOWN, u, &g_fx.cmask.srv, 1, 0, 0);
            for (int round = 0; round < 3; ++round)
            {
                fx_pass(&g_fx.amask[1], FX_GAUSS, u, &g_fx.amask[0].srv, 1, 2, 0);
                fx_pass(&g_fx.amask[0], FX_GAUSS, u, &g_fx.amask[1].srv, 1, 0, 2);
            }
            halo = g_fx.amask[0].srv;
        }
    }
    if (halo < 0)
        u->spp[2] = 0.0f;
    gpu_part(GP_REFL);
    tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const int32_t in[3] = { scene, halo, depth->srv };
    const float vp[4] = { u->vp[0], u->vp[1], u->vp[2], u->vp[3] };
    fx_draw(target_view(color, 0, 0), color->w, color->h, g_fx.by_fmt[g_zl.fmt].zlate, vp, u, in, 3, 0, 0);
    color->scene = 0; /* its chain is behind (scene_mips) */
    gpu_scope(scope_was);
    gpu_part(part_was);
}

/* Before each draw (el_on_draw's moments: the first of the interface drawn into the scene, or the first draw reading it
 * into a target its size): the frame's noted lines laid over it, once */
static void zl_on_draw(const GfxDraw* d)
{
    if (g_zl.pend_serial != g_serial || !g_rt)
        return;
    int reads = 0;
    for (int i = 0; i < 8; ++i)
        reads |= d->tex[i] == g_zl.color;
    if (reads && (uint64_t)g_rt->w * g_rt->h * 2 < (uint64_t)g_zl.present_w * g_zl.present_h)
        reads = 0;
    if (g_rt == g_zl.color ? !d->overlay : !reads)
        return;
    g_zl.pend_serial = 0;
    zl_run();
}

/* The scene read back (gfx_tex_read) before anything drew over it: the frame's noted lines laid first, once */
static void zl_on_read(const GfxTex* t)
{
    if (g_zl.pend_serial != g_serial || t != g_zl.color)
        return;
    g_zl.pend_serial = 0;
    zl_run();
}

/* The game done with its world (all of it drawn into the scene): the scene lit by what its additive draws added. onto:
 * DLSS's output lit instead (dlss5_elight_post: dlss_run), its viewport's part ovp; NULL the scene */
static void dlss_jitter_now(float jit[2]);
static GfxTex* dlss_depth_up(void);
static void el_run(GfxTex* onto, const float ovp[4])
{
    GfxTex *color = g_el.color, *depth = g_el.depth, *emit = g_el.emit;
    const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!color || !depth || !emit)
        return;
    GfxTex* lit = onto ? onto : color; /* the one drawn over, from a copy of it */
    FxTex* copy = onto ? &g_fx.pp : &g_fx.src;
    const float vw = g_el.u.vp[2], vh = g_el.u.vp[3];
    const uint32_t div = vw > 2048 ? 4 : vw > 1024 ? 2 : 1; /* bloom's size */
    uint32_t w = (uint32_t)((vw + div - 1) / div), h = (uint32_t)((vh + div - 1) / div);
    if (!fx_tex(&g_el.x, RGBA, w, h))
        return;
    /* the five levels read: from the first, or (a reach past 12 and 24 world units) one or two further down the chain, each
     * twice as wide - a long reach's light carries across most of the screen, its near ones a little softer */
    const float reach = fmaxf(g_fxs.elight_radius, 0.5f);
    const int first = reach > 24.0f ? 2 : reach > 12.0f ? 1 : 0;
    int n = 0;
    for (; n < 5 + first; ++n)
    {
        w = (w + 1) / 2, h = (h + 1) / 2;
        if (w < 2 || h < 2)
            break;
        if (!fx_tex(&g_el.lv[n], RGBA, w, h) || !fx_tex(&g_el.tmp[n], RGBA, w, h))
            return;
    }
    if (n <= first || !fx_tex(copy, lit->dxfmt, lit->w, lit->h))
        return;
    FxU* u = &g_el.u;
    /* a home point's teleport (GfxScene.warping): the strength eases to elight_warp of itself over a quarter of a second, and
     * back over one once it's done, by time */
    const uint64_t now = gfx_now_ns();
    const float dt = g_el.warp_ns && now > g_el.warp_ns ? fminf((float)((double)(now - g_el.warp_ns) * 1e-9), 0.1f) : 0.0f;
    const float warp_to = g_el.warping ? fminf(fmaxf(g_fxs.elight_warp, 0.0f), 1.0f) : 1.0f;
    if (!g_el.warp_ns)
        g_el.warp_k = 1.0f;
    g_el.warp_ns = now;
    g_el.warp_k = warp_to < g_el.warp_k ? fmaxf(g_el.warp_k - dt * 4.0f, warp_to) : fminf(g_el.warp_k + dt, warp_to);
    const float strength = fmaxf(g_fxs.elight, 0.0f) * g_el.warp_k;
    /* how strong, how far, the threshold, and the most a surface's light is multiplied by: 3, more with a strength over 1 */
    u->elight[0] = strength, u->elight[1] = reach, u->elight[2] = 0.03f;
    u->elight[3] = 3.0f * fmaxf(strength, 1.0f);
    u->elight2[0] = 4.0f * u->vp[2] / (float)g_el.lv[first].w; /* a level's reach: about two of its blur's sigmas */    u->grade[3] = g_fxs.debug; /* Render visual, as it is now */
    const int scope_was = gpu_scope_cur(), part_was = gpu_part_cur();
    gpu_scope(GS_REFL);
    gpu_part(GP_ELIGHT);
    tex_state(emit, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    /* the lanterns' flames and the dimmed effects drawn this frame (el_emit) leave out all but elight_lantern and elight_dim
     * of their light (elight2.z, elight2.w); the characters' effects are a light of their own at elight_actor (elight3) */
    int32_t x_in[5] = { emit->srv, depth->srv, emit->srv, emit->srv, emit->srv };
    for (int k = 0; k < 3; ++k)
    {
        const int on = g_el.part[k] && g_el.part_serial[k] == g_serial && (k == 2 || el_share(k) != 1.0f);
        if (on)
            tex_state(g_el.part[k], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), x_in[2 + k] = g_el.part[k]->srv;
        if (k < 2)
            u->elight2[2 + k] = on ? 1.0f - fminf(fmaxf(el_share(k), 0.0f), 4.0f) : 0.0f;
        else /* the characters' effects: their own strength, whatever elight is (fx_elx) */
            u->elight3[0] = fmaxf(g_fxs.elight_actor, 0.0f), u->elight3[1] = on ? 1.0f : 0.0f;
    }
    /* onto DLSS's output: the scene's inputs read where this frame's jitter moved the scene's draws to (fx_elc), and the
     * depth the output's (dlss_depth_up: held along edges from the frame before), where it can be made - the scene's own
     * at a pillar's edge is the pillar one frame and the wall behind the next, as the jitter moves it, and the light there
     * flickered (elight3.z 2) */
    GfxTex* zup = onto ? dlss_depth_up() : NULL;
    if (zup && zup->srv < 0)
        zup = NULL;
    u->elight3[2] = onto ? (zup ? 2.0f : 1.0f) : 0.0f;
    if (onto)
        dlss_jitter_now(&u->hist[2]);
    else
        u->hist[2] = u->hist[3] = 0.0f;
    g_el.ran_serial = g_serial;
    gfx_watch_value("light from the effects: strength", strength);
    gfx_watch_value("light from the effects: characters' effects among them", u->elight3[1]);
    gfx_watch_value("light from the effects: lanterns' share left out", u->elight2[2]);
    gfx_watch_value("light from the effects: dimmed effects' share left out", u->elight2[3]);
    fx_pass(&g_el.x, FX_ELX, u, x_in, 5, 0, 0);
    for (int k = 0; k < n; ++k)
    {
        fx_pass(&g_el.lv[k], FX_DOWN, u, k ? &g_el.lv[k - 1].srv : &g_el.x.srv, 1, 0, 0);
        fx_pass(&g_el.tmp[k], FX_GAUSS, u, &g_el.lv[k].srv, 1, 2, 0);
        fx_pass(&g_el.lv[k], FX_GAUSS, u, &g_el.tmp[k].srv, 1, 0, 2);
    }
    /* the scene (or DLSS's output) as it is, read while the pass draws over it */
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to);
    memset(&from, 0, sizeof from);
    from.pResource = lit->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, from.SubresourceIndex = 0;
    to.pResource = copy->res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, to.SubresourceIndex = 0;
    tex_state(lit, D3D12_RESOURCE_STATE_COPY_SOURCE);
    fx_state(copy, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, 0, 0, 0, &from, NULL);
    fx_state(copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (zup)
        tex_state(zup, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    int32_t c_in[8] = { zup ? zup->srv : depth->srv, -1, -1, -1, -1, -1, emit->srv, copy->srv };
    for (int k = first; k < n && k < first + 5; ++k)
        c_in[1 + k - first] = g_el.lv[k].srv;
    tex_state(lit, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float vp[4] = { u->vp[0], u->vp[1], u->vp[2], u->vp[3] };
    fx_draw(target_view(lit, 0, 0), lit->w, lit->h, g_fx.by_fmt[g_el.fmt].elight, onto ? ovp : vp, u, c_in, 8, 0, 0);
    u->elight3[2] = 0.0f;
    if (!onto)
        color->scene = 0; /* its chain is behind (scene_mips) */
    gpu_scope(scope_was);
    gpu_part(part_was);
}

static int dlss_nr_pending(const GfxTex* color);

/* Before each draw (as dlss_on_draw, and before it): the frame's noted scene lit once the game is done drawing its world
 * into it - at the first of the interface drawn into it (the names over heads, which mustn't light anything), or the
 * first draw reading it into a target its size (the scene laid onto the frame; not a distortion's 256x256 grab of it, nor
 * the sun flare's probe) */
static void el_on_draw(const GfxDraw* d)
{
    if (g_el.pend_serial != g_serial || !g_rt)
        return;
    int reads = 0;
    for (int i = 0; i < 8; ++i)
        reads |= d->tex[i] == g_el.color;
    if (reads && (uint64_t)g_rt->w * g_rt->h * 2 < (uint64_t)g_el.present_w * g_el.present_h)
        reads = 0;
    if (g_rt == g_el.color ? !d->overlay : !reads)
        return;
    g_el.pend_serial = 0;
    if (!g_el.hint)
        g_el.emit_serial = 0; /* its draws done with (DLSS 5 has them to the end: an effect after the names over heads too) */
    /* after DLSS 5 (dlss5_elight_post), where it takes this scene: onto its output (dlss_run), or into the scene still at
     * the draw DLSS runs at if it didn't (dlss_on_draw) */
    if (g_fxs.dlss5_elight_post != 0.0f && dlss_nr_pending(g_el.color))
    {
        g_el.post_serial = g_serial;
        return;
    }
    el_run(NULL, NULL);
}

#endif

#ifdef VANACORE
/* --- DLSS and FSR (VanaCore) ---------------------------------------------------------------------------------
 * NVIDIA's DLSS is 64-bit only and FFXI is 32-bit: VanaCore.Dlss.exe (src/dlss), a 64-bit helper on the same
 * GPU, runs it on textures shared with it (dlss_ipc.h). Each frame the scene's colour (drawn at the smaller
 * size [graphics] dlss asks for, its draws jittered by a sub-pixel step: gfx_dlss_jitter), its depth and its
 * motion (the camera's, from the depth and the frame before's camera) go into the shared inputs; the helper's
 * output, at the size the scene is shown at, is copied into `out`, which the game's draws then sample in place
 * of the scene's target (draw_encode) - the game lays it onto the back buffer as it would the scene.
 * AMD's FSR ([graphics] fsr: g_dlss.upscaler VANA_UPSCALER_FSR) goes the same way, through the same helper started
 * for it: the same inputs, jitter and output, on any make's card (FSR 3.1; FSR 4 on a Radeon RX 7000 or 9000), and
 * the camera's planes and the frame's time with them. DLSS 5 is DLSS's alone; frame generation is NVIDIA's or AMD's
 * with either (g_fg.kind). Either is also handed the masks of what isn't the static world (fx_dlssr: the game's effects,
 * and what moves on its own), the one thing the camera's motion can't describe and VanaCore can't honestly guess
 * (dlss_motion's matching of draws to the frame before's is kept as an opt-in). */
#define VENDOR_NVIDIA 0x10DE
/* NVIDIA's PCI device ids. Turing's RTX 20 (TU102, TU104, TU106) is 0x1E00 to 0x1F7F, but for three TU106 cards without
 * the tensor cores DLSS runs on: the GTX 1660 SUPER (0x1F09), a GTX 1650 (0x1F0A) and the CMP 40HX (0x1F0B). Turing's
 * GTX 16 and MX cards (TU117 from 0x1F80, TU116 from 0x2180) have none, nor has any card before Turing (a GTX 1080 is
 * 0x1B80). */
#define DEVICE_RTX20 0x1E00u
#define DEVICE_RTX20_END 0x1F80u
#define DEVICE_RTX20_GTX 0x1F09u /* ... the three TU106 cards without them: 0x1F09 to 0x1F0B */
#define DEVICE_RTX20_GTX_END 0x1F0Cu
#define DEVICE_RTX30 0x2200u /* ... Ampere (RTX 30) starts at 0x2200 */
#define DEVICE_RTX40 0x2680u /* ... Ada (RTX 40) starts at 0x2680; Ampere ends below it */
static UINT g_vendor; /* the device's graphics card's PCI vendor (make_device): DLSS is NVIDIA's only */
static UINT g_device_id; /* ... and its device */

/* DLSS on an NVIDIA RTX 20 or later, DLSS 5 (nr) on an RTX 40 or later. Cards without tensor cores (a GTX 1080) crashed
 * with it on, and with [graphics] dlss kept on, again at each start: they never start the helper. Frame generation (an
 * RTX 40 or later) isn't checked here: NGX tells the helper, and DLSS runs without it on an RTX 20 or 30. */
static int dlss_card(int nr)
{
    const UINT id = g_device_id;
    if (g_vendor != VENDOR_NVIDIA)
        return 0;
    if (nr)
        return id >= DEVICE_RTX40;
    return id >= DEVICE_RTX30
           || (id >= DEVICE_RTX20 && id < DEVICE_RTX20_END && (id < DEVICE_RTX20_GTX || id >= DEVICE_RTX20_GTX_END));
}

static struct
{
    int quality;              /* NVSDK_NGX_PerfQuality_Value, or -1: off (FSR's too: the same scales, near enough) */
    int upscaler;             /* VANA_UPSCALER_*: DLSS, or FSR (vanacore_gfx_set_upscaler) */
    int started;              /* ... the one the helper was started for (another is started for the other) */
    float sharpness;          /* FSR's own sharpening, 0 (off) to 1 */
    int version_want;         /* FSR's version asked for: 0 its own choice, else a major version (2, 3, 4) */
    uint64_t ran_ns;          /* when a frame last went to the helper: FSR's time between frames */
    wchar_t helper[MAX_PATH], log[MAX_PATH];
    int failed;               /* given up: no helper, DLSS not there, or the helper stopped answering */
    HANDLE map, go, ready, proc;
    HANDLE go_fg;             /* frame generation's (".fg") */
    /* No fence shared with the helper (cpu_sync: Wine and Proton can't open one from another process): the helper sets
     * ".done" (".fgdone") where it would signal the fence, a thread of ours (br_thread) signals it from that, and the game's
     * thread waits for its queue (cpu_ev) before it sets ".go" (".fg") - dlss_post */
    int cpu_sync;
    HANDLE done, done_fg, cpu_ev;
    VanaDlssShared* sh;
    ID3D12Fence* fence;
    uint64_t value;           /* the fence's last value: frames take two, the game's (inputs ready) and the helper's */
    uint64_t last;            /* the last frame's (the game's) value, and when it was handed over */
    ULONGLONG last_at;
    ID3D12Resource* res[7];   /* shared: colour, depth, motion, output; with DLSS 5, the effects' own picture (fx); and the masks
                               * of what isn't the static world (fx_dlssr): reactive, and the effects alone (tc, FSR's) */
    int32_t rtv[2], srv_out;  /* depth's and motion's render-target views; the output's shader view */
    int32_t rtv_r, rtv_t;     /* the masks' (reactive, tc) */
    int32_t rtv_c;            /* the colour's, when it's drawn (nr) */
    int32_t rtv_fx;           /* the effects' picture's (nr) */
    int views;                /* ... made (they're released with the textures) */
    uint32_t in_w, in_h, out_w, out_h;
    DXGI_FORMAT cfmt;
    int made_quality;
    int made_preset;          /* the models it was made for (DLSS's preset * 100 + DLSS 5's) */
    int nr;                   /* DLSS 5 first: the colour handed over as R16G16B16A16_FLOAT, drawn not copied */
    /* the game's effects kept out of DLSS 5 (dlss5_fx_out): the scene as it was before them (dlss_frame: its copy, the frame
     * it's of), handed to DLSS 5 in its place, their own picture (res[4]) beside it */
    GfxTex* before;
    uint64_t before_serial;
    GfxTex* out;              /* the output as the game's draws sample it, in place of `scene` */
    GfxTex* scene;
    uint64_t out_serial;      /* the frame `out` holds */
    uint64_t ran_serial;      /* the last frame DLSS ran on */
    float prev_view[16], prev_proj[16], prev_cam[3];
    uint64_t prev_serial;
    int reset;
    uint32_t n;               /* frames so far: the jitter's place in its sequence */
    float jit[2];             /* this frame's, in render pixels */
    uint64_t jit_serial;
    int said;                 /* the state last logged */
    char why[160];            /* ... why it gave up (3, 4) */
    int lost;                 /* ... the helper crashed or stopped answering (helper_lost): let go when it's tried again */
    ULONGLONG made_at;        /* new textures handed to the helper, when */
    /* the scene as its effects finished it (gfx_scene_done), through DLSS when a draw first reads it: the game draws
     * on into it after its effects - the sky's parts, the lights' glows, spells, the names over heads - and only
     * then lays it onto the back buffer */
    uint64_t pend_serial;
    uint64_t noted;           /* the last frame a scene was noted for DLSS (one with a 3D scene, not a menu's) */
    GfxTex *pend_color, *pend_depth;
    int pend_fmt;
    int graded;    /* the colour grade was laid on what dlss_run is making of this scene (grade_in_place, or fx_post for the output) */
    int dof_late;  /* depth of field was left out of this scene's composite for the output (dof_after) */
    GfxScene pend_s;
    /* with frame generation on: `out` as DLSS (or FSR) made it, before the interface held for it went over it (the names
     * over heads, floating text, damage numbers), and the frame it's of: frame generation's hudless is made from it
     * (fg_hudless_clean) */
    GfxTex* clean;
    uint64_t clean_serial;
    /* FSR 1 (fsr1_run), in AMD's FSR's place: it ran (`out` is its, made for in_w..out_h), it couldn't (off), it was logged */
    int fsr1, fsr1_failed, fsr1_said;
} g_dlss = { -1 };

static void wd_start(void);
static void wd_stop(void);
static void br_start(void);
static void br_stop(void);
static void dlss_post(HANDLE go, ID3D12Fence* fence, uint64_t frame);

/* DLSS 5 is to take this frame's scene `color`: noted for it (dlss_frame) and not run yet */
static int dlss_nr_pending(const GfxTex* color)
{
    return g_dlss.pend_serial == g_serial && g_dlss.pend_color == color && g_dlss.nr &&
           g_dlss.upscaler == VANA_UPSCALER_DLSS && !g_dlss.failed;
}

/* this frame's DLSS jitter, in render pixels: where the scene's draws were moved to (0, 0 without one) */
static void dlss_jitter_now(float jit[2])
{
    const int on = g_dlss.jit_serial == g_serial;
    jit[0] = on ? g_dlss.jit[0] : 0.0f, jit[1] = on ? g_dlss.jit[1] : 0.0f;
}

/* the render size over the output's at each quality (NVSDK_NGX_PerfQuality_Value: performance, balanced,
 * quality, ultra performance, ultra quality, DLAA) */
static float dlss_factor(int quality)
{
    static const float k[] = { 0.5f, 0.58f, 0.667f, 0.333f, 0.77f, 1.0f };
    return quality >= 0 && quality < 6 ? k[quality] : 1.0f;
}

static float halton(uint32_t i, uint32_t b)
{
    float f = 1.0f, r = 0.0f;
    for (; i; i /= b)
        f /= (float)b, r += f * (float)(i % b);
    return r;
}

/* the upscaler asked for, by name: the log's and /vana's */
static const char* upscaler_name(void)
{
    return g_dlss.upscaler == VANA_UPSCALER_FSR ? "FSR" : "DLSS";
}

/* FSR 1 (fsr1_run: VanaCore's own, in this process) in AMD's FSR's place: asked for ([graphics] fsr.version=1), or AMD's
 * FSR isn't there or gave up (its DLLs missing, nothing for this card, the helper gone). Any card with DirectX 11 or 12
 * runs it. */
static int fsr1_wanted(void)
{
    return g_dlss.quality >= 0 && g_dlss.upscaler == VANA_UPSCALER_FSR && !g_dlss.fsr1_failed &&
           (g_dlss.version_want == 1 || g_dlss.failed);
}

/* The upscaler given up (off till it's turned on again): DLSS's or AMD's FSR's helper, and FSR 1 too where it was to stand
 * in for it */
static int dlss_gave_up(void)
{
    if (g_dlss.upscaler == VANA_UPSCALER_FSR && (g_dlss.version_want == 1 || g_dlss.failed))
        return g_dlss.fsr1_failed;
    return g_dlss.failed;
}

/* whether DLSS or AMD's FSR gathers this frame's scene (not FSR 1: one frame alone) and is to be told what isn't the static
 * world (the masks: mask_draw, fx_dlssr; dlss_reactive, dlss_reactive_moving) */
static int mask_wanted(void)
{
    return g_dlss.quality >= 0 && !dlss_gave_up() && !fsr1_wanted() && g_fxs.dlss_compare == 0.0f &&
           (g_fxs.dlss_reactive > 0.0f || g_fxs.dlss_reactive_moving > 0.0f);
}

/* The models the helper was made for, as one number: DLSS's preset * 100 + DLSS 5's; FSR's version asked for */
static int dlss_model_key(void)
{
    return g_dlss.upscaler == VANA_UPSCALER_FSR ? 100000 + g_dlss.version_want
                                                : (int)g_fxs.dlss_preset * 100 + (int)g_fxs.dlss5_preset;
}

/* What DLSS 5 is made with besides its model: its passes and their taper (each pass a feature made for them) and, with
 * dlss5_rebuild, its look - style, intensity, the local tone and structure, the skin's, the mask. NVIDIA's DLL is handed
 * those each frame too, but by what OptiScaler's DLSS 5 work found it reads them only as it's made, so a change to them
 * shows only once DLSS 5 is made again. Taken as settled once unchanged for a second (dlss_nr_settled): a slider being
 * dragged changes them every frame, DLSS 5 takes seconds to make, and making it again frame after frame wears NVIDIA's
 * driver out (the feature stops answering till the game restarts). g_dlss_nr_made: the values it was last made with
 * (dlss_targets), the settled ones */
enum { DLSS_NR_KEYS = 9 };
static float g_dlss_nr_made[DLSS_NR_KEYS];

static void dlss_nr_now(float* v)
{
    const int look = g_fxs.dlss5_rebuild != 0.0f;
    v[0] = fminf(fmaxf(roundf(g_fxs.dlss5_passes), 1.0f), 3.0f);
    v[1] = fminf(fmaxf(g_fxs.dlss5_taper, 0.0f), 1.0f);
    v[2] = look ? g_fxs.dlss5_style : 0.0f, v[3] = look ? g_fxs.dlss5_intensity : 0.0f;
    v[4] = look ? g_fxs.dlss5_tone : 0.0f, v[5] = look ? g_fxs.dlss5_structure : 0.0f;
    v[6] = look ? g_fxs.dlss5_skin : 0.0f, v[7] = look ? g_fxs.dlss5_mask : 0.0f;
    v[8] = g_fxs.dlss5_scale > 0.0f && g_fxs.dlss5_scale < 0.99f ? fmaxf(g_fxs.dlss5_scale, 0.25f) : 1.0f;
}

static const float* dlss_nr_settled(void)
{
    static float seen[DLSS_NR_KEYS], settled[DLSS_NR_KEYS];
    static uint64_t since;
    static int have;
    float now_v[DLSS_NR_KEYS];
    dlss_nr_now(now_v);
    const uint64_t now = GetTickCount64();
    if (!have || memcmp(now_v, seen, sizeof seen))
    {
        memcpy(seen, now_v, sizeof seen), since = now;
        if (!have)
            memcpy(settled, now_v, sizeof settled), have = 1;
    }
    else if (now - since >= 1000)
        memcpy(settled, now_v, sizeof settled);
    return settled;
}

/* The camera's near and far planes (far 0: none - the depth goes to 1 only at infinity, as FFXI's does) and its
 * vertical field of view (radians), from its projection (Direct3D's, row vectors; either hand) */
static void cam_planes(const float* proj, float* zn, float* zf, float* fov)
{
    const float hand = proj[11] < 0.0f ? -1.0f : 1.0f;
    const float a = proj[10], b = proj[14];
    *zn = fabsf(a) > 1e-6f ? fabsf(b / a) : 0.1f;
    *zf = fabsf(a - hand) > 1e-6f ? fabsf(b / (a - hand)) : 0.0f;
    *fov = 2.0f * atanf(1.0f / (fabsf(proj[5]) > 1e-6f ? fabsf(proj[5]) : 1.0f));
}

static void dlss_say(int state, const char* why)
{
    if (state == g_dlss.said)
        return;
    g_dlss.said = state;
    fprintf(stderr, "[recomp] gfx: %s: %s\n", upscaler_name(), why);
    if (state >= 3)
    {
        /* the reason, without its "; off" */
        snprintf(g_dlss.why, sizeof g_dlss.why, "%s", why);
        char* off = strstr(g_dlss.why, "; off");
        if (off)
            *off = 0;
    }
}

/* the helper started, with the shared memory, events and fence it opens by name */
static int dlss_start(void)
{
    wchar_t base[96];
    swprintf(base, 96, L"%ls%lu", VANA_DLSS_MAP, (unsigned long)GetCurrentProcessId());
    g_dlss.map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(VanaDlssShared), base);
    g_dlss.sh = g_dlss.map ? (VanaDlssShared*)MapViewOfFile(g_dlss.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanaDlssShared)) : NULL;
    wchar_t name[128];
    swprintf(name, 128, L"%ls.go", base);
    g_dlss.go = CreateEventW(NULL, FALSE, FALSE, name);
    swprintf(name, 128, L"%ls.ready", base);
    g_dlss.ready = CreateEventW(NULL, FALSE, FALSE, name);
    swprintf(name, 128, L"%ls.fg", base);
    g_dlss.go_fg = CreateEventW(NULL, FALSE, FALSE, name);
    /* (the fences the helper can't open from another process, under Wine and Proton: each side's own, and CPU waits) */
    g_dlss.cpu_sync = gfx_dlss_cpu_sync_wanted();
    swprintf(name, 128, L"%ls.done", base);
    g_dlss.done = CreateEventW(NULL, FALSE, FALSE, name);
    swprintf(name, 128, L"%ls.fgdone", base);
    g_dlss.done_fg = CreateEventW(NULL, FALSE, FALSE, name);
    g_dlss.cpu_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_dlss.sh || !g_dlss.go || !g_dlss.ready || !g_dlss.go_fg || !g_dlss.done || !g_dlss.done_fg || !g_dlss.cpu_ev ||
        FAILED(ID3D12Device_CreateFence(g_dev, 0, g_dlss.cpu_sync ? D3D12_FENCE_FLAG_NONE : D3D12_FENCE_FLAG_SHARED,
            &IID_ID3D12Fence, (void**)&g_dlss.fence)))
    {
        dlss_say(4, "couldn't set up what the helper shares");
        return 0;
    }
    memset(g_dlss.sh, 0, sizeof *g_dlss.sh);
    g_dlss.sh->version = VANA_DLSS_VERSION;
    g_dlss.sh->cpu_sync = g_dlss.cpu_sync;
    g_dlss.sh->game_pid = GetCurrentProcessId();
    g_dlss.sh->upscaler = g_dlss.started = g_dlss.upscaler; /* the helper serves this one */
    LUID luid;
    ID3D12Device_GetAdapterLuid(g_dev, &luid);
    g_dlss.sh->luid_low = luid.LowPart, g_dlss.sh->luid_high = luid.HighPart;
    wchar_t cmd[3 * MAX_PATH];
    swprintf(cmd, 3 * MAX_PATH, L"\"%ls\" %lu \"%ls\"", g_dlss.helper, (unsigned long)GetCurrentProcessId(), g_dlss.log);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si), si.cb = sizeof si;
    if (!CreateProcessW(g_dlss.helper, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | ABOVE_NORMAL_PRIORITY_CLASS, NULL, NULL,
            &si, &pi))
    {
        dlss_say(4, "VanaCore.Dlss.exe didn't start (is it in the dlss folder?)");
        return 0;
    }
    CloseHandle(pi.hThread);
    g_dlss.proc = pi.hProcess;
    wd_start();
    if (g_dlss.cpu_sync)
        br_start();
    fprintf(stderr, "[recomp] gfx: %s: the helper started (process %lu)%s\n", upscaler_name(), (unsigned long)pi.dwProcessId,
        g_dlss.cpu_sync ? "; no fence shared with it (Wine or Proton can't open one from another process), its frames waited for on the CPU" : "");
    return 1;
}

/* res as an NT handle in the helper's process */
static uint64_t dlss_share(ID3D12DeviceChild* res)
{
    HANDLE mine = NULL, theirs = NULL;
    if (FAILED(ID3D12Device_CreateSharedHandle(g_dev, res, NULL, GENERIC_ALL, NULL, &mine)))
        return 0;
    if (!DuplicateHandle(GetCurrentProcess(), mine, g_dlss.proc, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS))
        theirs = NULL;
    CloseHandle(mine);
    return (uint64_t)(uintptr_t)theirs;
}

static ID3D12Resource* dlss_texture(DXGI_FORMAT fmt, uint32_t w, uint32_t h, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, fmt, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    ID3D12Resource* r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void**)&r)))
        return NULL;
    return r;
}

/* the scene's depth at DLSS's output size, for the interface drawn over it: this frame's (g_zup_cur) and the frame
 * before's, which it's held to (fx_depthup); g_zup_serial the frame the last was made in */
static GfxTex* g_dlss_zups[2];
static int g_zup_cur;
static uint64_t g_zup_serial;
static GfxTex* g_dlss_replay_depth;     /* ... while the held interface is drawn over DLSS's output (NULL: no depth test) */
static ID3D12PipelineState* g_zup_pso; /* fx_depthup into a depth target of g_zup_fmt */
static DXGI_FORMAT g_zup_fmt;
/* frame generation's frames (fgn_frame): fx_namez into a depth target of g_fgn_z_fmt, fx_fgui onto a frame of g_fgn_ui_fmt */
static ID3D12PipelineState *g_fgn_z_pso, *g_fgn_ui_pso;
static DXGI_FORMAT g_fgn_z_fmt, g_fgn_ui_fmt;

/* The scene effects' shaders compiled again (gfx.h gfx_fx_rebuild: VanaCore's Recompile shaders): asked for from any
 * thread, taken at the end of a frame (frame_end) once no build is on. Every shader and every pipeline made from one is
 * let go - the pipelines after the GPU's frames (defer) - and the build state put back to not built, so the next scene
 * that asks starts it over on the thread pool, as the first did (fx_ready); the effects are off till it's done, and the
 * notice shows it. */
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
#define LET_GO_PSO(p) do { if (p) defer((IUnknown*)(p), NULL, -1), (p) = NULL; } while (0)
#define LET_GO_BLOB(b) do { if (b) ID3D10Blob_Release(b), (b) = NULL; } while (0)
    LET_GO_BLOB(g_fx.vs);
    for (int i = 0; i < FX_NPS; ++i)
    {
        LET_GO_BLOB(g_fx.ps[i]);
        LET_GO_PSO(g_fx.pso[i]);
    }
    LET_GO_BLOB(g_fx.depthup);
    LET_GO_BLOB(g_fx.namez);
    LET_GO_BLOB(g_fx.fgui);
    LET_GO_BLOB(g_fx.skycol);
    LET_GO_BLOB(g_fx.truesky);
    LET_GO_BLOB(g_fx.easu);
    LET_GO_BLOB(g_fx.rcas);
    LET_GO_PSO(g_fx.skycol_pso);
    LET_GO_PSO(g_fx.glow_keep);
    for (uint32_t i = 0; i < FX_FORMATS; ++i)
    {
        LET_GO_PSO(g_fx.by_fmt[i].comp);
        LET_GO_PSO(g_fx.by_fmt[i].mip);
        LET_GO_PSO(g_fx.by_fmt[i].smooth);
        LET_GO_PSO(g_fx.by_fmt[i].dofcomp);
        LET_GO_PSO(g_fx.by_fmt[i].fxaa);
        LET_GO_PSO(g_fx.by_fmt[i].glow);
        LET_GO_PSO(g_fx.by_fmt[i].glowg);
        LET_GO_PSO(g_fx.by_fmt[i].post);
        LET_GO_PSO(g_fx.by_fmt[i].refl);
        LET_GO_PSO(g_fx.by_fmt[i].clarity);
        LET_GO_PSO(g_fx.by_fmt[i].elight);
        LET_GO_PSO(g_fx.by_fmt[i].zlate);
        LET_GO_PSO(g_fx.by_fmt[i].easu);
        LET_GO_PSO(g_fx.by_fmt[i].rcas);
        LET_GO_PSO(g_fx.by_fmt[i].sky);
    }
    memset(g_fx.by_fmt, 0, sizeof g_fx.by_fmt);
    g_fx.nfmt = 0;
    LET_GO_PSO(g_zup_pso);
    LET_GO_PSO(g_fgn_z_pso);
    LET_GO_PSO(g_fgn_ui_pso);
    LET_GO_BLOB(g_occl.zmax_ps);
    LET_GO_BLOB(g_occl.test_ps);
    LET_GO_PSO(g_occl.zmax);
    LET_GO_PSO(g_occl.test);
#undef LET_GO_PSO
#undef LET_GO_BLOB
    InterlockedExchange(&g_fx.step, 0);
    InterlockedExchange(&g_fx.steps, 0);
    InterlockedExchange(&g_fx.state, 0);
    fprintf(stderr, "[recomp] gfx: the scene effects' shaders let go; compiled again at the next scene\n");
}

static void fgn_release(void);

static void dlss_release(void)
{
    for (int i = 0; i < 7; ++i)
        if (g_dlss.res[i])
            defer((IUnknown*)g_dlss.res[i], NULL, -1), g_dlss.res[i] = NULL;
    if (g_dlss.views)
    {
        if (g_dlss.rtv_r >= 0)
            defer(NULL, &g_rtv, g_dlss.rtv_r);
        if (g_dlss.rtv_t >= 0)
            defer(NULL, &g_rtv, g_dlss.rtv_t);
        for (int i = 0; i < 2; ++i)
            if (g_dlss.rtv[i] >= 0)
                defer(NULL, &g_rtv, g_dlss.rtv[i]);
        if (g_dlss.rtv_c >= 0)
            defer(NULL, &g_rtv, g_dlss.rtv_c);
        if (g_dlss.rtv_fx >= 0)
            defer(NULL, &g_rtv, g_dlss.rtv_fx);
        if (g_dlss.srv_out >= 0)
            defer(NULL, &g_srv, g_dlss.srv_out);
        g_dlss.views = 0;
    }
    if (g_dlss.out)
        gfx_tex_destroy(g_dlss.out), g_dlss.out = NULL;
    if (g_dlss.clean)
        gfx_tex_destroy(g_dlss.clean), g_dlss.clean = NULL;
    g_dlss.clean_serial = 0;
    for (int i = 0; i < 2; ++i)
        if (g_dlss_zups[i])
            gfx_tex_destroy(g_dlss_zups[i]), g_dlss_zups[i] = NULL;
    g_zup_serial = 0;
    if (g_dlss.before)
        gfx_tex_destroy(g_dlss.before), g_dlss.before = NULL;
    g_dlss.before_serial = 0;
    g_dlss.fsr1 = 0;
    fgn_release();
}

/* the shared inputs and output for these sizes, and `out` (the scene target's shape, at the output's scale); nr:
 * the colour in the format DLSS 5 takes */
static int dlss_targets(GfxTex* color, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, int nr)
{
    dlss_release();
    const DXGI_FORMAT cfmt = nr ? DXGI_FORMAT_R16G16B16A16_FLOAT : color->dxfmt;
    g_dlss.res[0] = dlss_texture(cfmt, in_w, in_h, nr ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
    g_dlss.res[1] = dlss_texture(DXGI_FORMAT_R32_FLOAT, in_w, in_h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    g_dlss.res[2] = dlss_texture(DXGI_FORMAT_R16G16_FLOAT, in_w, in_h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    g_dlss.res[3] = dlss_texture(DXGI_FORMAT_R8G8B8A8_UNORM, out_w, out_h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (int i = 0; i < 4; ++i)
        if (!g_dlss.res[i])
            return 0;
    g_dlss.views = 1, g_dlss.rtv[0] = g_dlss.rtv[1] = g_dlss.rtv_c = g_dlss.rtv_fx = g_dlss.srv_out = g_dlss.rtv_r = g_dlss.rtv_t = -1;
    /* the masks of what isn't the static world (fx_dlssr): the reactive mask (FSR's; DLSS's bias-current-colour mask) and, for
     * FSR, the transparency and composition mask. The upscaler runs without one it couldn't have, as before there were any */
    g_dlss.res[5] = dlss_texture(DXGI_FORMAT_R8_UNORM, in_w, in_h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    g_dlss.rtv_r = g_dlss.res[5] ? heap_alloc(&g_rtv) : -1;
    if (g_dlss.rtv_r >= 0)
        ID3D12Device_CreateRenderTargetView(g_dev, g_dlss.res[5], NULL, heap_cpu(&g_rtv, g_dlss.rtv_r));
    if (g_dlss.upscaler == VANA_UPSCALER_FSR)
    {
        g_dlss.res[6] = dlss_texture(DXGI_FORMAT_R8_UNORM, in_w, in_h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        g_dlss.rtv_t = g_dlss.res[6] ? heap_alloc(&g_rtv) : -1;
        if (g_dlss.rtv_t >= 0)
            ID3D12Device_CreateRenderTargetView(g_dev, g_dlss.res[6], NULL, heap_cpu(&g_rtv, g_dlss.rtv_t));
    }
    for (int i = 0; i < 2; ++i)
    {
        g_dlss.rtv[i] = heap_alloc(&g_rtv);
        if (g_dlss.rtv[i] < 0)
            return 0;
        ID3D12Device_CreateRenderTargetView(g_dev, g_dlss.res[1 + i], NULL, heap_cpu(&g_rtv, g_dlss.rtv[i]));
    }
    if (nr)
    {
        g_dlss.rtv_c = heap_alloc(&g_rtv);
        if (g_dlss.rtv_c < 0)
            return 0;
        ID3D12Device_CreateRenderTargetView(g_dev, g_dlss.res[0], NULL, heap_cpu(&g_rtv, g_dlss.rtv_c));
        /* the effects' own picture, for the frames they're kept out of DLSS 5 (dlss5_fx_out): made with it, so turning that
         * on and off doesn't make DLSS 5 again (seconds) */
        g_dlss.res[4] = dlss_texture(DXGI_FORMAT_R16G16B16A16_FLOAT, in_w, in_h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        g_dlss.rtv_fx = g_dlss.res[4] ? heap_alloc(&g_rtv) : -1;
        if (g_dlss.rtv_fx >= 0)
            ID3D12Device_CreateRenderTargetView(g_dev, g_dlss.res[4], NULL, heap_cpu(&g_rtv, g_dlss.rtv_fx));
    }
    g_dlss.srv_out = heap_alloc(&g_srv);
    if (g_dlss.srv_out < 0)
        return 0;
    ID3D12Device_CreateShaderResourceView(g_dev, g_dlss.res[3], NULL, heap_cpu(&g_srv, g_dlss.srv_out));
    /* `out`: the scene's target's shape, so the game's draws read the viewport's part of it where they read
     * the scene's */
    uint32_t ow = (uint32_t)((double)color->w * out_w / in_w + 0.5), oh = (uint32_t)((double)color->h * out_h / in_h + 0.5);
    g_dlss.out = gfx_tex_create(GFX_TEX_2D, color->fmt, ow < out_w ? out_w : ow, oh < out_h ? out_h : oh, 1, GFX_USE_RT);
    if (!g_dlss.out)
        return 0;
    VanaDlssShared* sh = g_dlss.sh;
    sh->color = dlss_share((ID3D12DeviceChild*)g_dlss.res[0]);
    sh->depth = dlss_share((ID3D12DeviceChild*)g_dlss.res[1]);
    sh->motion = dlss_share((ID3D12DeviceChild*)g_dlss.res[2]);
    sh->output = dlss_share((ID3D12DeviceChild*)g_dlss.res[3]);
    sh->fence = g_dlss.cpu_sync ? 0 : dlss_share((ID3D12DeviceChild*)g_dlss.fence);
    if (!sh->color || !sh->depth || !sh->motion || !sh->output || (!sh->fence && !g_dlss.cpu_sync))
        return 0;
    /* (without it, or its view, the effects go through DLSS 5 as they're drawn: the helper's nr_fx_ok stays 0) */
    sh->fx = g_dlss.res[4] && g_dlss.rtv_fx >= 0 ? dlss_share((ID3D12DeviceChild*)g_dlss.res[4]) : 0;
    sh->nr_fx_ok = 0;
    sh->reactive = g_dlss.res[5] && g_dlss.rtv_r >= 0 ? dlss_share((ID3D12DeviceChild*)g_dlss.res[5]) : 0;
    sh->tc = g_dlss.res[6] && g_dlss.rtv_t >= 0 ? dlss_share((ID3D12DeviceChild*)g_dlss.res[6]) : 0;
    sh->in_w = in_w, sh->in_h = in_h, sh->out_w = out_w, sh->out_h = out_h;
    sh->color_format = (uint32_t)cfmt, sh->output_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sh->quality = g_dlss.quality, sh->depth_inverted = 0;
    {
        /* FSR's: the depth with no far plane where the projection has none (FFXI's), or one too far to count */
        float zn, zf, fov;
        cam_planes(g_dlss.pend_s.proj, &zn, &zf, &fov);
        sh->depth_infinite = zf <= 0.0f || zf > zn * 1.0e6f;
    }
    sh->sr_version_want = g_dlss.version_want;
    sh->nr = nr;
    sh->sr_preset = (int32_t)g_fxs.dlss_preset, sh->nr_preset = (int32_t)g_fxs.dlss5_preset;
    /* DLSS 5's passes, as settled (dlss_nr_settled: what it's made again for when they change) */
    memcpy(g_dlss_nr_made, dlss_nr_settled(), sizeof g_dlss_nr_made);
    sh->nr_passes = (int32_t)g_dlss_nr_made[0], sh->nr_taper = g_dlss_nr_made[1], sh->nr_scale = g_dlss_nr_made[8];
    sh->generation++;
    g_dlss.in_w = in_w, g_dlss.in_h = in_h, g_dlss.out_w = out_w, g_dlss.out_h = out_h;
    g_dlss.cfmt = color->dxfmt, g_dlss.made_quality = g_dlss.quality, g_dlss.nr = nr;
    g_dlss.made_preset = dlss_model_key();
    g_dlss.reset = 1;
    fprintf(stderr, "[recomp] gfx: %s: the scene drawn at %ux%u, shown at %ux%u%s\n", upscaler_name(), in_w, in_h, out_w,
        out_h, nr ? ", through DLSS 5 first" : "");
    return 1;
}

/* AMD's FSR DLLs beside the helper (a VanaCore build ships them in its dlss folder: third_party/fsr/fetch-fsr.ps1's) */
static int fsr_dlls_here(void)
{
    static const wchar_t* const names[] = { L"amd_fidelityfx_loader_dx12.dll", L"amd_fidelityfx_upscaler_dx12.dll" };
    const wchar_t* slash = wcsrchr(g_dlss.helper, L'\\');
    if (!slash)
        return 0;
    const int dir = (int)(slash - g_dlss.helper) + 1;
    for (int i = 0; i < 2; ++i)
    {
        wchar_t dll[MAX_PATH];
        swprintf(dll, MAX_PATH, L"%.*ls%ls", dir, g_dlss.helper, names[i]);
        if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
            return 0;
    }
    return 1;
}

static void dlss_let_go(void);
static void helpers_let_through(int dlss_too);
static void helper_lost(const char* why);

/* the helper's state: ready (1), not yet (0), or gone for good (-1, failed set) */
static int dlss_helper_ready(void)
{
    if (g_dlss.failed)
        return -1;
    /* a helper started for the other upscaler (DLSS, FSR): let go, and one started for this one */
    if (g_dlss.sh && g_dlss.started != g_dlss.upscaler)
        dlss_let_go();
    /* DLSS on another make's card (AMD, Intel), or on an NVIDIA without DLSS (before the RTX 20, a GTX 16); FSR without
     * AMD's DLLs: the helper isn't started at all */
    if (g_dlss.upscaler == VANA_UPSCALER_DLSS && !dlss_card(0))
    {
        dlss_say(3, "it needs an NVIDIA RTX 20 series card or later; off");
        return g_dlss.failed = 1, -1;
    }
    if (g_dlss.upscaler == VANA_UPSCALER_FSR && !g_dlss.sh && !fsr_dlls_here())
    {
        dlss_say(3, "AMD's FSR DLLs aren't in the dlss folder; off");
        return g_dlss.failed = 1, -1;
    }
    if (!g_dlss.sh && !dlss_start())
        return g_dlss.failed = 1, -1;
    int32_t st = g_dlss.sh->state;
    /* ended: having said why (it can't run here), as below; else it crashed, or ended unasked (helper_lost: its exit code) */
    if (WaitForSingleObject(g_dlss.proc, 0) == WAIT_OBJECT_0 && st != VANA_DLSS_UNAVAILABLE && st != VANA_DLSS_FAILED)
    {
        helper_lost(NULL);
        return -1;
    }
    if (st == VANA_DLSS_STARTING)
        return 0;
    if (st != VANA_DLSS_READY)
    {
        /* a frame it was handed isn't coming back */
        helpers_let_through(1);
        char why[300];
        snprintf(why, sizeof why, "%s; off", g_dlss.sh->message);
        dlss_say(st == VANA_DLSS_UNAVAILABLE ? 3 : 4, why);
        return g_dlss.failed = 1, -1;
    }
    dlss_say(2, "ready");
    return 1;
}

/* The helper's `done` (or fg_done) at `want` within ms: it only records and submits a frame's work, well under a
 * millisecond as a rule, but a frame handed over while it's on the last would find it behind */
static int helper_caught_up(volatile uint64_t* done, uint64_t want, int ms)
{
    const uint64_t until = gfx_now_ns() + (uint64_t)ms * 1000000u;
    for (int spins = 0; *done < want; ++spins)
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

/* A wait of the game's frame on the helper (or on frame generation's present thread) of more than 4 ms, in the log
 * (at most 5 a second): how long, for what, and how far the helper had got. A hitch with DLSS on is one of these. */
static void helper_wait_note(uint64_t t0, const char* what, uint64_t want, uint64_t had)
{
    const uint64_t ns = gfx_now_ns() - t0;
    g_helper_wait_ns += ns;
    if (ns < 4000000u)
        return;
    static uint64_t second_at;
    static int said;
    const uint64_t now = gfx_now_ns();
    if (now - second_at > 1000000000u)
        second_at = now, said = 0;
    if (++said > 5)
        return;
    fprintf(stderr, "[recomp] gfx: %s: the frame waited %.1f ms for %s (frame %llu; it had handed over %llu)\n",
        upscaler_name(), (double)ns / 1e6, what, (unsigned long long)want, (unsigned long long)had);
}

/* names_native without DLSS: the scene noted at gfx_scene_done, all of it drawn now, copied filtered into `out` at the
 * size it's shown (its target's shape), for this frame's draws to read in its place as they read DLSS's, and the names
 * drawn into it after its effects to be drawn over it at that size (dlss_draw_over). Nothing when it's shown at its
 * own size: they're drawn at that size already. `out` is this copy's while g_dlss.in_w is 0 (DLSS's sets it). */
static void plain_run(void)
{
    GfxTex* color = g_dlss.pend_color;
    const GfxScene* s = &g_dlss.pend_s;
    const int fmt = g_dlss.pend_fmt;
    const uint32_t ow = s->present_w, oh = s->present_h;
    if (!color || color->srv < 0 || fmt < 0 || !g_fx.by_fmt[fmt].mip || !ow || !oh || (ow == color->w && oh == color->h) ||
        ow > 16384 || oh > 16384)
        return;
    if (!g_dlss.out || g_dlss.in_w || g_dlss.out->w != ow || g_dlss.out->h != oh || g_dlss.out->fmt != color->fmt)
    {
        dlss_release();
        g_dlss.in_w = g_dlss.in_h = g_dlss.out_w = g_dlss.out_h = 0; /* DLSS's made again when it's back */
        g_dlss.out = gfx_tex_create(GFX_TEX_2D, color->fmt, ow, oh, 1, GFX_USE_RT);
        if (!g_dlss.out)
            return;
        fprintf(stderr, "[recomp] gfx: the scene copied at %ux%u (drawn at %ux%u) for the names over heads\n", ow, oh,
            color->w, color->h);
    }
    const int part_was = gpu_part_cur();
    gpu_part(GP_DLSS_OUT);
    FxU u;
    memset(&u, 0, sizeof u);
    u.size[0] = (float)ow, u.size[1] = (float)oh;
    const float vp[4] = { 0, 0, (float)ow, (float)oh };
    tex_state(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(g_dlss.out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    fx_draw(target_view(g_dlss.out, 0, 0), ow, oh, g_fx.by_fmt[fmt].mip, vp, &u, &color->srv, 1, 0, 0);
    gpu_part(part_was);
    g_dlss.scene = color, g_dlss.out_serial = g_serial;
}

/* --- The colour grade, laid last ------------------------------------------------------------------------------------
 * It is the last thing done to the picture, so every effect is under it: VanaCore's own (reflections, bounce light, depth of
 * field, anti-aliasing, the zone lines, the glow and the light from the effects) and the game's, drawn into the scene after
 * gfx_scene_done, and what the upscaler makes of them (DLSS 5's look included). Where an upscaler runs (dlss_run, fsr1_run) it
 * goes onto its output, in the pass that makes it where nothing is drawn onto it after, else (grade_in_place) in place, from a
 * copy; with none, in place on the scene itself at the draw that first reads it (dlss_run's no-upscaler branch: grade_scene).
 * The interface drawn into the scene (names over heads) is held out of it till then, and drawn after. */

/* how far into the night this scene is while [graphics.effects] night is on (GfxScene.night, taken as the composite's uniforms are
 * made): the moonlight laid with the grade (moon_grade) */
static float g_night_now;
/* ... and where it is in the day, 1 by day to -1 at night, 0 indoors (GfxScene.day_night): the warmth's day_night_warmth */
static float g_day_night_now;

/* the moonlight is laid (moon_grade): night is on, it's night, night_moon isn't 0 */
static int moon_wanted(void)
{
    return g_night_now > 0.0f && g_fxs.night_moon > 0.0f;
}

/* the grade (or the moonlight, laid with it) is on, and there's a pass for the scene's format */
static int grade_wanted(int fmt)
{
    return g_fxs.fx != 0.0f && (g_fxs.grade > 0.0f || moon_wanted()) && fmt >= 0 && g_fx.by_fmt[fmt].post;
}

/* its settings in the uniforms (fx_post's: post, post2, post3) */
static void grade_uniforms(FxU* u)
{
    u->post[0] = g_fxs.grade, u->post[1] = g_fxs.sat, u->post[2] = g_fxs.contrast;
    u->post[3] = g_fxs.warmth + fminf(fmaxf(g_fxs.day_night_warmth, 0.0f), 2.0f) * g_day_night_now; /* warmer by day, cooler at night */
    u->post2[0] = g_fxs.brightness, u->post2[1] = g_fxs.shadows, u->post2[2] = g_fxs.highlights;
    u->post3[0] = g_fxs.midtones, u->post3[1] = g_fxs.whites, u->post3[2] = g_fxs.light_tint, u->post3[3] = g_fxs.rolloff;
    u->post4[0] = g_night_now, u->post4[1] = fminf(fmaxf(g_fxs.night_moon, 0.0f), 1.0f);
    u->post4[2] = fminf(fmaxf(g_fxs.hue, -180.0f), 180.0f) * 0.0174532925f; /* the hue, degrees to radians (hue_turn) */
}

/* The grade laid on `t` where it is, within the viewport vp: that part of it copied to read (g_fx.pp), then drawn back graded.
 * `t` ends in the render target state, the GPU part `part`. Nothing when the grade is off or its textures can't be made */
static void grade_in_place(GfxTex* t, int fmt, const float vp[4], int part)
{
    if (!t || !grade_wanted(fmt) || !fx_tex(&g_fx.pp, t->dxfmt, t->w, t->h))
        return;
    const LONG x0 = (LONG)floorf(vp[0] < 0.0f ? 0.0f : vp[0]), y0 = (LONG)floorf(vp[1] < 0.0f ? 0.0f : vp[1]);
    LONG x1 = (LONG)ceilf(vp[0] + vp[2]), y1 = (LONG)ceilf(vp[1] + vp[3]);
    x1 = x1 > (LONG)t->w ? (LONG)t->w : x1, y1 = y1 > (LONG)t->h ? (LONG)t->h : y1;
    if (x1 <= x0 || y1 <= y0)
        return;
    FxU u;
    memset(&u, 0, sizeof u);
    grade_uniforms(&u);
    u.post2[3] = 1.0f; /* fx_post reads the pixel itself from the copy, not the input filtered over the viewport */
    u.vp[0] = vp[0], u.vp[1] = vp[1], u.vp[2] = vp[2], u.vp[3] = vp[3];
    u.size[0] = (float)t->w, u.size[1] = (float)t->h;
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to), memset(&from, 0, sizeof from);
    from.pResource = t->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = g_fx.pp.res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
    gpu_part(GP_COPY);
    tex_state(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    fx_state(&g_fx.pp, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, (UINT)x0, (UINT)y0, 0, &from, &box);
    fx_state(&g_fx.pp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu_part(part);
    fx_draw(target_view(t, 0, 0), t->w, t->h, g_fx.by_fmt[fmt].post, vp, &u, &g_fx.pp.srv, 1, 0, 0);
    g_dlss.graded = 1;
}

/* The grade on the scene `color` itself (dlss_run, where no upscaler took it): its viewport as the effects' */
static void grade_scene(GfxTex* color, const GfxScene* s, int fmt)
{
    if (!color || !grade_wanted(fmt))
        return;
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > color->w || vy + vh > color->h)
        vx = vy = 0, vw = (float)color->w, vh = (float)color->h;
    const float vp[4] = { vx, vy, vw, vh };
    const int part_was = gpu_part_cur();
    grade_in_place(color, fmt, vp, GP_DLSS_OUT);
    gpu_part(part_was);
}

/* Depth of field's focus distance for the scene s: dof_focus when it's set (a fixed one, from the camera). Else, with
 * dof_player (the free camera's Focus: Player), how far into the view your character stands, wherever on the screen: 0, the
 * nearest thing at the screen's middle (fx_focus), is where the game's own camera keeps the character, not where a free one
 * has it. Not while the character's place isn't known or it's behind the camera: the screen's middle then */
static float dof_focus_now(const GfxScene* s)
{
    if (g_fxs.dof_focus > 0.0f || g_fxs.dof_player == 0.0f || !s || s->focus[3] == 0.0f)
        return g_fxs.dof_focus;
    const float z = (s->focus[0] * s->view[2] + s->focus[1] * s->view[6] + s->focus[2] * s->view[10] + s->view[14]) *
                    (s->proj[11] < 0.0f ? -1.0f : 1.0f);
    return z > 0.25f ? z : 0.0f;
}

/* Depth of field's settings in the uniforms: its widest blur dof pixels on a 1080-line screen, as the lines are; vh the
 * viewport's height at the size it's drawn */
static void dof_params(FxU* u, float vh, const GfxScene* s)
{
    const float dr = g_fxs.dof * vh / 1080.0f;
    u->dof[0] = dr > 48.0f ? 48.0f : dr;
    u->dof[1] = g_fxs.dof_range > 0.0f ? g_fxs.dof_range : 0.0f;
    u->dof[2] = g_fxs.dof_far;
    u->dof[3] = g_fxs.dof_near > 1.0f ? 1.0f : g_fxs.dof_near;
    u->dof2[0] = dof_focus_now(s);
    u->dof2[3] = g_fxs.dof_sky != 0.0f ? 1.0f : 0.0f;
}

/* Depth of field on `color` (the finished scene, or the upscaler's output: dof_after), in the viewport vp of u: its copy
 * (sharp) in `copy`, the focus, a blurred copy at half size, then the two mixed by each pixel's blur amount. `depth` is read
 * at color's own pixels. 0 when it's off or its textures can't be made */
static int dof_run(GfxTex* color, FxTex* copy, GfxTex* depth, int fmt, FxU* u, const float vp[4])
{
    const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!(u->dof[0] >= 1.0f && g_fx.by_fmt[fmt].dofcomp && fx_tex(&g_fx.d0, RGBA, (uint32_t)(u->vp[2] + 1) / 2, (uint32_t)(u->vp[3] + 1) / 2) &&
          fx_tex(&g_fx.d1, RGBA, g_fx.d0.w, g_fx.d0.h) && fx_tex(&g_fx.foc[0], DXGI_FORMAT_R32_FLOAT, 1, 1) &&
          fx_tex(&g_fx.foc[1], DXGI_FORMAT_R32_FLOAT, 1, 1)))
    {
        g_fx.foc_ns = 0; /* off: the next time it's on, the focus starts where it is */
        return 0;
    }
    gpu_scope(GS_DOF);
    /* the focus eases about 6 times a second's worth; after a pause it's taken as it is */
    uint64_t now = gfx_now_ns();
    double dt = g_fx.foc_ns ? (double)(now - g_fx.foc_ns) * 1e-9 : 1.0;
    g_fx.foc_ns = now;
    u->dof2[1] = (float)(1.0 - exp(-dt * 6.0));
    u->dof2[2] = dt > 0.5 ? 1.0f : 0.0f;
    FxTex* last = &g_fx.foc[g_fx.foc_i];
    FxTex* focus = &g_fx.foc[g_fx.foc_i ^ 1];
    g_fx.foc_i ^= 1;
    fx_state(last, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to), memset(&from, 0, sizeof from);
    from.pResource = color->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = copy->res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu_part(GP_COPY);
    tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
    fx_state(copy, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, 0, 0, 0, &from, NULL);
    fx_state(copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    int32_t focus_in[2] = { depth->srv, last->srv };
    gpu_part(GP_DOF_FOCUS);
    fx_pass(focus, FX_FOCUS, u, focus_in, 2, 0, 0);
    int32_t prep_in[3] = { copy->srv, depth->srv, focus->srv };
    gpu_part(GP_DOF_HALF);
    fx_pass(&g_fx.d0, FX_DOFPREP, u, prep_in, 3, 0, 0);
    gpu_part(GP_DOF_BLUR);
    fx_pass(&g_fx.d1, FX_DOFBLUR, u, &g_fx.d0.srv, 1, 0, 0);
    tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
    int32_t dof_in[4] = { copy->srv, g_fx.d1.srv, depth->srv, focus->srv };
    gpu_part(GP_DOF_LAY);
    fx_draw(target_view(color, 0, 0), color->w, color->h, g_fx.by_fmt[fmt].dofcomp, vp, u, dof_in, 4, 0, 0);
    gpu_part(GP_REST);
    return 1;
}

/* Depth of field on the upscaler's output (dlss_run, fsr1_run), where the composite left it (dof_late): blurred before it, the
 * blur's edges moved with the jitter and the upscaler gathered them frame on frame - a shimmer along them. The depth the
 * output's (dlss_depth_up), the blur as wide on the screen. `out` ends in the render target state, the GPU part `part` */
static void dof_after(GfxTex* out, GfxTex* scene, const GfxScene* s, int fmt, const float ovp[4], int part)
{
    if (!g_dlss.dof_late || !out || fmt < 0 || !g_fx.by_fmt[fmt].dofcomp || !fx_tex(&g_fx.pp, out->dxfmt, out->w, out->h))
        return;
    g_dlss.scene = scene; /* (dlss_depth_up reads its size) */
    GfxTex* zup = dlss_depth_up();
    if (!zup || zup->srv < 0)
        return;
    FxU u;
    memset(&u, 0, sizeof u);
    float minz, maxz;
    memcpy(&minz, &s->vp[4], 4);
    memcpy(&maxz, &s->vp[5], 4);
    if (maxz <= minz)
        minz = 0, maxz = 1;
    u.proj[0] = s->proj[0], u.proj[1] = s->proj[5], u.proj[2] = s->proj[8], u.proj[3] = s->proj[9];
    u.zp[0] = s->proj[10], u.zp[1] = s->proj[14], u.zp[2] = minz, u.zp[3] = maxz;
    u.vp[0] = ovp[0], u.vp[1] = ovp[1], u.vp[2] = ovp[2], u.vp[3] = ovp[3];
    u.size[0] = (float)out->w, u.size[1] = (float)out->h;
    u.hand[0] = s->proj[11] < 0.0f ? -1.0f : 1.0f;
    u.grade[3] = g_fxs.debug; /* Render visual 7 shows the blur amount */
    dof_params(&u, ovp[3], s);
    const int scope_was = gpu_scope_cur(), part_was = gpu_part_cur();
    dof_run(out, &g_fx.pp, zup, fmt, &u, ovp);
    gpu_scope(scope_was);
    gpu_part(part_was);
    tex_state(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu_part(part);
}

/* FSR 1 in AMD's FSR's place (fsr1_wanted): the scene noted at gfx_scene_done, all of it drawn now, made larger into `out`
 * here, for this frame's draws to read in its place as they read DLSS's - EASU (fx_easu), then RCAS (fx_rcas) with
 * [graphics] fsr.sharpness above 0, then the colour grade (unless depth of field is still to go onto the output: it goes
 * after that), each pass onto the next's input (up1) and the last onto `out`. One frame alone (no depth, motion or jitter: the effects' anti-aliasing and temporal
 * passes stay on for it), so softer than AMD's FSR 3.1 or 4. At native it only sharpens (RCAS at 0 is a copy). */
static void fsr1_run(void)
{
    GfxTex* color = g_dlss.pend_color;
    const GfxScene* s = &g_dlss.pend_s;
    const int fmt = g_dlss.pend_fmt;
    if (g_dlss.sh)
        dlss_let_go(); /* AMD's FSR's helper (FSR 1 asked for since, or it gave up): not wanted now */
    if (!g_dlss.fsr1_said)
    {
        g_dlss.fsr1_said = 1;
        if (g_dlss.version_want == 1)
            fprintf(stderr, "[recomp] gfx: FSR: FSR 1 ([graphics] fsr.version=1)\n");
        else
            fprintf(stderr, "[recomp] gfx: FSR: FSR 1 in AMD's FSR's place (%s)\n", g_dlss.why[0] ? g_dlss.why : "it isn't there");
    }
    if (!color || color->srv < 0 || fmt < 0 || !g_fx.by_fmt[fmt].mip)
        return;
    ID3D12PipelineState *easu = g_fx.by_fmt[fmt].easu, *rcas = g_fx.by_fmt[fmt].rcas;
    if (!easu || !rcas)
    {
        dlss_say(4, g_fx.easu && g_fx.rcas ? "FSR 1's pipelines failed; off" : "FSR 1's passes didn't compile; off");
        g_dlss.fsr1_failed = 1;
        return;
    }
    float vp[4];
    uint32_t in_w, in_h, out_w, out_h;
    if (!gfx_fsr1_sizes(s, color->w, color->h, dlss_factor(g_dlss.quality), vp, &in_w, &in_h, &out_w, &out_h))
        return; /* not a scene to make larger this frame (gfx_shared.h) */
    const float vx = vp[0], vy = vp[1];
    if (!g_dlss.fsr1 || !g_dlss.out || in_w != g_dlss.in_w || in_h != g_dlss.in_h || out_w != g_dlss.out_w ||
        out_h != g_dlss.out_h || g_dlss.out->fmt != color->fmt)
    {
        dlss_release();
        /* `out`: the scene's target's shape, so the game's draws read the viewport's part of it where they read the scene's */
        const uint32_t ow = (uint32_t)((double)color->w * out_w / in_w + 0.5), oh = (uint32_t)((double)color->h * out_h / in_h + 0.5);
        g_dlss.out = gfx_tex_create(GFX_TEX_2D, color->fmt, ow < out_w ? out_w : ow, oh < out_h ? out_h : oh, 1, GFX_USE_RT);
        if (!g_dlss.out)
        {
            dlss_say(4, "FSR 1 couldn't make its textures; off");
            g_dlss.fsr1_failed = 1;
            return;
        }
        g_dlss.in_w = in_w, g_dlss.in_h = in_h, g_dlss.out_w = out_w, g_dlss.out_h = out_h;
        g_dlss.cfmt = color->dxfmt, g_dlss.made_quality = -3; /* not the helper's: made again for it */
        g_dlss.fsr1 = 1;
        fprintf(stderr, "[recomp] gfx: FSR 1: the scene drawn at %ux%u, shown at %ux%u\n", in_w, in_h, out_w, out_h);
    }
    const int part_was = gpu_part_cur();
    gpu_part(GP_DLSS_OUT);
    const float kx = (float)out_w / in_w, ky = (float)out_h / in_h;
    const float ovp[4] = { vx * kx, vy * ky, (float)out_w, (float)out_h }, whole[4] = { 0, 0, (float)out_w, (float)out_h };
    const int native = in_w == out_w && in_h == out_h;
    FxU u;
    memset(&u, 0, sizeof u);
    ID3D12PipelineState* pass[3];
    int n = 0;
    if (!native)
        pass[n++] = easu;
    if (native || g_dlss.sharpness > 0.0f)
    {
        /* RCAS's strength: fsr.sharpness 0 to 1 as AMD's FSR 2 and 3 take it, from 2 stops below the most to the most */
        u.clar[0] = g_dlss.sharpness > 0.0f ? exp2f(-2.0f * (1.0f - g_dlss.sharpness)) : 0.0f;
        pass[n++] = rcas;
    }
    /* the colour grade, as on DLSS's output: in the last pass where nothing is drawn onto the output after it (depth of field,
     * dof_after, is), else in place after that */
    const int fold = grade_wanted(fmt) && !g_dlss.dof_late;
    if (fold)
    {
        grade_uniforms(&u);
        pass[n++] = g_fx.by_fmt[fmt].post;
        g_dlss.graded = 1;
    }
    int32_t src = color->srv;
    float from[4] = { vx, vy, (float)in_w, (float)in_h };
    tex_state(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (int k = 0; k < n; ++k)
    {
        memcpy(u.vp, from, sizeof from);
        if (k == n - 1)
        {
            memcpy(u.size, ovp, sizeof ovp);
            tex_state(g_dlss.out, D3D12_RESOURCE_STATE_RENDER_TARGET);
            fx_draw(target_view(g_dlss.out, 0, 0), g_dlss.out->w, g_dlss.out->h, pass[k], ovp, &u, &src, 1, 0, 0);
            break;
        }
        FxTex* to = &g_fx.up1[k & 1];
        if (!fx_tex(to, color->dxfmt, out_w, out_h))
        {
            gpu_part(part_was);
            dlss_say(4, "FSR 1 couldn't make its textures; off");
            g_dlss.fsr1_failed = 1;
            return;
        }
        memcpy(u.size, whole, sizeof whole);
        fx_state(to, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fx_draw(heap_cpu(&g_rtv, to->rtv), to->w, to->h, pass[k], whole, &u, &src, 1, 0, 0);
        fx_state(to, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        src = to->srv;
        memcpy(from, whole, sizeof whole);
    }
    /* depth of field, where the composite left it for here, then the colour grade over it, as after DLSS */
    dof_after(g_dlss.out, color, s, fmt, ovp, GP_DLSS_OUT);
    if (!fold)
        grade_in_place(g_dlss.out, fmt, ovp, GP_DLSS_OUT);
    gpu_part(part_was);
    g_dlss.scene = color, g_dlss.out_serial = g_serial;
}

/* The scene noted at gfx_scene_done, all of it drawn now (a draw reads it): through DLSS into `out`, for this
 * frame's draws to read in its place; with none, the colour grade on the scene itself (grade_scene). dlss_run, below,
 * grades the scene in place where this didn't make an output with it. */
static void dlss_keep_clean(void);
static int mo_wanted(void);

static void upscale_run(void)
{
    GfxTex* color = g_dlss.pend_color;
    GfxTex* depth = g_dlss.pend_depth;
    const GfxScene* s = &g_dlss.pend_s;
    int fmt = g_dlss.pend_fmt;
    if (g_dlss.quality < 0 || dlss_gave_up() || g_fxs.dlss_compare != 0.0f)
    {
        /* no upscaler (or the comparison with it): the scene graded where it is, first, so the copy at the size it's shown has it */
        grade_scene(color, s, fmt);
        if (g_fxs.names_native != 0.0f && g_fxs.dlss_compare == 0.0f)
            plain_run();
        return;
    }
    if (fsr1_wanted())
    {
        fsr1_run();
        return;
    }
    if (!depth || depth->srv < 0 || fmt < 0 || !g_fx.by_fmt[fmt].mip || !g_fx.pso[FX_DLSSZ] ||
        !g_fx.pso[FX_DLSSMV] || dlss_helper_ready() != 1)
        return;
    /* the helper's last frame: handed to its queue within half a second, or it's let go. This frame waits a little for
     * it first (shown without DLSS instead, it flickers) */
    VanaDlssShared* sh = g_dlss.sh;
    const uint64_t waited_from = gfx_now_ns();
    const int caught_up = !g_dlss.last || helper_caught_up(&sh->done, g_dlss.last, 100);
    if (g_dlss.last)
        helper_wait_note(waited_from, g_dlss.upscaler == VANA_UPSCALER_FSR ? "the helper's FSR" : "the helper's DLSS", g_dlss.last,
            sh->done);
    if (!caught_up)
    {
        if (GetTickCount64() - g_dlss.last_at < 500)
            return; /* still on it: this frame as it is */
        helper_lost("the helper stopped answering");
        return;
    }
    float vx = (float)s->vp[0], vy = (float)s->vp[1], vw = (float)s->vp[2], vh = (float)s->vp[3];
    if (vw < 16 || vh < 16 || vx + vw > color->w || vy + vh > color->h)
        vx = vy = 0, vw = (float)color->w, vh = (float)color->h;
    uint32_t in_w = (uint32_t)vw, in_h = (uint32_t)vh, out_w, out_h;
    if (s->present_w && s->present_h)
        out_w = (uint32_t)(vw * s->present_w / color->w + 0.5f), out_h = (uint32_t)(vh * s->present_h / color->h + 0.5f);
    else
        out_w = (uint32_t)(vw / dlss_factor(g_dlss.quality) + 0.5f), out_h = (uint32_t)(vh / dlss_factor(g_dlss.quality) + 0.5f);
    if (out_w < in_w || out_h < in_h)
        return; /* a scene larger than it's shown (the game's 4096 glow picture, a frame mid-resize): as it is */
    if (in_w >= out_w && dlss_factor(g_dlss.quality) < 0.99f)
        return; /* drawn at its full size by a mode that draws it smaller: the frame DLSS came on in, before the scene
                 * is made smaller (DLSS made for it would be made again the next frame) */
    /* DLSS 5 first ([graphics] dlss5), on an RTX 40 or later where the helper has it: gone again when the helper says it
     * failed */
    const int nr = g_dlss.upscaler == VANA_UPSCALER_DLSS && g_fxs.dlss5 != 0.0f && dlss_card(1) && sh->nr_available &&
                   color->srv >= 0 && g_fx.pso[FX_DLSSC];
    if (dlss_model_key() != g_dlss.made_preset && g_dlss.out)
        g_dlss.made_quality = -2; /* a model (or FSR's version) changed: made again, as for a new quality */
    /* ... as when what DLSS 5 is made with has changed and held still (its passes; with dlss5_rebuild, its look) */
    if (nr && g_dlss.nr && g_dlss.out && memcmp(dlss_nr_settled(), g_dlss_nr_made, sizeof g_dlss_nr_made))
        g_dlss.made_quality = -2;
    if (in_w != g_dlss.in_w || in_h != g_dlss.in_h || out_w != g_dlss.out_w || out_h != g_dlss.out_h ||
        color->dxfmt != g_dlss.cfmt || g_dlss.quality != g_dlss.made_quality || nr != g_dlss.nr || !g_dlss.out)
    {
        if (!dlss_targets(color, in_w, in_h, out_w, out_h, nr))
        {
            dlss_say(4, "couldn't make its textures; off");
            g_dlss.failed = 1;
            dlss_release();
            return;
        }
        /* the helper makes DLSS (or FSR) for them (seconds with DLSS 5, or at a large size): the scene as it is till it has */
        SetEvent(g_dlss.go);
        g_dlss.made_at = GetTickCount64();
        return;
    }
    if (sh->made != sh->generation)
    {
        if (GetTickCount64() - g_dlss.made_at > 30000)
        {
            dlss_say(4, "the helper didn't make it in 30 seconds; off");
            g_dlss.failed = 1;
        }
        return;
    }
    /* the camera from the frame before (the motion): its view and projection, if it was the frame just before
     * and not a jump away (a new zone, a cut) */
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
    int reset = g_dlss.reset || g_dlss.prev_serial + 1 != g_serial;
    if (!reset)
    {
        float dx = vinv[12] - g_dlss.prev_cam[0], dy = vinv[13] - g_dlss.prev_cam[1], dz = vinv[14] - g_dlss.prev_cam[2];
        reset = !(dx * dx + dy * dy + dz * dz <= 25.0f); /* (a camera that's no number: made again too) */
    }
    gfx_watch_value("DLSS: history reset", (float)reset);
    float m[16];
    mat_mul(m, vinv, reset ? s->view : g_dlss.prev_view);
    mat_mul(u.reproj, m, reset ? s->proj : g_dlss.prev_proj);
    memcpy(g_dlss.prev_view, s->view, 64), memcpy(g_dlss.prev_proj, s->proj, 64), memcpy(g_dlss.prev_cam, vinv + 12, 12);
    g_dlss.prev_serial = g_serial;
    /* the inputs: the viewport's part of the scene, its depth, its motion; left in COMMON for the helper */
    ID3D12GraphicsCommandList* l = list();
    gpu_part(GP_DLSS_IN);
    float vp0[4] = { 0, 0, vw, vh };
    int fx_out = 0; /* the game's effects kept out of DLSS 5 this frame (dlss5_fx_out) */
    if (g_dlss.nr)
    {
        /* for DLSS 5: drawn onto the float colour it takes. With the game's effects kept out of it (dlss5_fx_out), the
         * scene before them (dlss_keep_before) and their own picture beside it - the finished scene less that - which the
         * helper adds onto DLSS 5's picture before DLSS. With them as its lights (dlss5_fx_light), a glow from their additive
         * draws (el_emit, on black: their bright parts, blurred - fx_dlssg) laid over what DLSS 5 sees, dlss5_fx_gain times
         * as bright as drawn, past white; not the sprites themselves lifted (it restyled a spell's quad as a thing) */
        fx_out = g_dlss.before && g_dlss.before_serial == g_serial && g_dlss.before->w == color->w &&
                 g_dlss.before->h == color->h && g_dlss.res[4] && g_dlss.rtv_fx >= 0 && sh->nr_fx_ok && g_fx.pso[FX_DLSSD];
        const int lit = g_fxs.dlss5_fx_light != 0.0f && g_el.emit && g_el.emit_serial == g_serial && g_el.color == color &&
                        g_el.emit->srv >= 0 && g_fx.pso[FX_DLSSG];
        GfxTex* base = fx_out ? g_dlss.before : color;
        const float gain = fminf(fmaxf(g_fxs.dlss5_fx_gain, 1.0f), 64.0f);
        int32_t glow = -1, mean = -1;
        tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (lit)
        {
            /* the glow: the additive draws past the glow effects lighting's threshold (el_run's) at a quarter of the
             * viewport, the characters' effects among them at their share (dlss5_fx_actor), then at an eighth, blurred
             * across and down */
            const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
            const uint32_t qw = (in_w + 3) / 4, qh = (in_h + 3) / 4, ew = (qw + 1) / 2, eh = (qh + 1) / 2;
            GfxTex* actor = g_el.part[2] && g_el.part_serial[2] == g_serial && g_el.part[2]->srv >= 0 ? g_el.part[2] : NULL;
            /* ... less those it isn't to take as lights (dlss5_skip_tex: el_part 3) */
            GfxTex* skip = g_el.part[3] && g_el.part_serial[3] == g_serial && g_el.part[3]->srv >= 0 ? g_el.part[3] : NULL;
            u.elight[2] = 0.03f;
            u.elight3[0] = fminf(fmaxf(g_fxs.dlss5_fx_actor, 0.0f), 4.0f), u.elight3[1] = actor ? 1.0f : 0.0f;
            u.elight3[3] = skip ? 1.0f : 0.0f;
            tex_state(g_el.emit, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (actor)
                tex_state(actor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (skip)
                tex_state(skip, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (ew >= 2 && eh >= 2 && fx_tex(&g_fx.nrl[0], RGBA, qw, qh) && fx_tex(&g_fx.nrl[1], RGBA, ew, eh) &&
                fx_tex(&g_fx.nrl[2], RGBA, ew, eh))
            {
                const int32_t gi[4] = { g_el.emit->srv, depth->srv, actor ? actor->srv : g_el.emit->srv,
                                        skip ? skip->srv : g_el.emit->srv };
                fx_pass(&g_fx.nrl[0], FX_DLSSG, &u, gi, 4, 0, 0);
                fx_pass(&g_fx.nrl[1], FX_DOWN, &u, &g_fx.nrl[0].srv, 1, 0, 0);
                fx_pass(&g_fx.nrl[2], FX_GAUSS, &u, &g_fx.nrl[1].srv, 1, 2, 0);
                fx_pass(&g_fx.nrl[1], FX_GAUSS, &u, &g_fx.nrl[2].srv, 1, 0, 2);
                glow = g_fx.nrl[1].srv;
                /* ... and the lights' mean, one texel, eased with the last frame's (fx_dlssm): the gain's exposure */
                if (g_fxs.dlss5_fx_norm > 0.0f && g_fx.pso[FX_DLSSM])
                {
                    FxTex* last = &g_fx.nrm[g_fx.nrm_i];
                    FxTex* now = &g_fx.nrm[g_fx.nrm_i ^ 1];
                    const int held = last->res && g_fx.nrm_serial + 1 == g_serial;
                    if (fx_tex(now, RGBA, 1, 1))
                    {
                        u.hist[0] = held ? 1.0f : 0.0f;
                        const int32_t mi[2] = { glow, held ? last->srv : glow };
                        fx_pass(now, FX_DLSSM, &u, mi, 2, 0, 0);
                        u.hist[0] = 0.0f;
                        g_fx.nrm_i ^= 1, g_fx.nrm_serial = g_serial;
                        mean = now->srv;
                    }
                }
            }
        }
        u.hand[1] = mean >= 0 ? fminf(fmaxf(g_fxs.dlss5_fx_norm, 0.0f), 1.0f) : 0.0f; /* the gain's exposure (0: none) */
        u.hand[2] = glow >= 0 ? gain : 0.0f;
        u.hand[3] = glow >= 0 ? fminf(fmaxf(g_fxs.dlss5_fx_cap, 0.0f), 64.0f) : 0.0f; /* its ceiling (0: none) */
        /* ... and the depth, the sky left out of the lights, and the lights' mean (fx_dlssc) */
        const int32_t c_in[4] = { base->srv, glow >= 0 ? glow : base->srv, depth->srv, mean >= 0 ? mean : base->srv };
        tex_state(base, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(g_dlss.res[0], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fx_draw(heap_cpu(&g_rtv, g_dlss.rtv_c), in_w, in_h, g_fx.pso[FX_DLSSC], vp0, &u, c_in, 4, 0, 0);
        barrier(g_dlss.res[0], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        u.hand[1] = 0.0f, u.hand[2] = 0.0f, u.hand[3] = 0.0f;
        if (fx_out)
        {
            const int32_t d_in[2] = { color->srv, g_dlss.before->srv };
            tex_state(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barrier(g_dlss.res[4], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            fx_draw(heap_cpu(&g_rtv, g_dlss.rtv_fx), in_w, in_h, g_fx.pso[FX_DLSSD], vp0, &u, d_in, 2, 0, 0);
            barrier(g_dlss.res[4], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        }
    }
    else
    {
        tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(g_dlss.res[0], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to, from;
        memset(&to, 0, sizeof to), memset(&from, 0, sizeof from);
        from.pResource = color->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = g_dlss.res[0], to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box = { (UINT)vx, (UINT)vy, 0, (UINT)vx + in_w, (UINT)vy + in_h, 1 };
        ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &from, &box);
        barrier(g_dlss.res[0], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    /* the moving things' own motion where they were drawn this frame (dlss_motion, or an effect's model with frame generation:
     * mo_draw), the camera's elsewhere */
    const int mo = mo_wanted() && g_mo.clear_serial == g_serial && g_mo.t.res && g_mo.t.w == color->w && g_mo.t.h == color->h;
    if (mo)
        fx_state(&g_mo.t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (int i = 0; i < 2; ++i)
    {
        const int32_t z_in[2] = { depth->srv, mo ? g_mo.t.srv : depth->srv };
        u.hand[1] = mo ? 1.0f : 0.0f; /* (the depth's too: an effect's model's with frame generation, fx_dlssz) */
        barrier(g_dlss.res[1 + i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fx_draw(heap_cpu(&g_rtv, g_dlss.rtv[i]), in_w, in_h, g_fx.pso[i ? FX_DLSSMV : FX_DLSSZ], vp0, &u, z_in, 2, 0, 0);
        barrier(g_dlss.res[1 + i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }
    u.hand[1] = 0.0f;
    if (g_dlss.res[5] && g_dlss.rtv_r >= 0 && sh->reactive && g_fx.pso[FX_DLSSR])
    {
        /* the masks of what isn't the static world (fx_dlssr): the reactive mask (res[5]: FSR's, and DLSS's bias-current-colour
         * mask) and, for FSR, the transparency and composition mask (res[6]). The game's effects, from the scene with them (color)
         * and without (before: the scene as noted at gfx_scene_done, dlss_keep_before), at dlss_reactive; the moving things drawn
         * again this frame (mask_draw), where nothing nearer was drawn after them, and VanaCore's grass, where it wrote depth this
         * frame (the depth before it, gfx_scene_world_depth, differs), at dlss_reactive_moving. Not wanted (both 0, Compare): the
         * masks are cleared, so the helper reads none */
        const int wanted = mask_wanted();
        GfxTex* wd = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial && g_fx.wdepth->srv >= 0 ? g_fx.wdepth : depth;
        const int drawn = wanted && g_mo.m_clear == g_serial && g_mo.m.res && g_mo.m.w == color->w && g_mo.m.h == color->h;
        const int fxd = wanted && g_fxs.dlss_reactive > 0.0f && g_dlss.before && g_dlss.before_serial == g_serial &&
                        g_dlss.before->srv >= 0 && g_dlss.before->w == color->w && g_dlss.before->h == color->h && color->srv >= 0;
        if (wanted)
        {
            if (wd != depth)
                tex_state(wd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (drawn)
                fx_state(&g_mo.m, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (fxd)
            {
                tex_state(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                tex_state(g_dlss.before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        const int32_t r_in[5] = { depth->srv, drawn ? g_mo.m.srv : depth->srv, wd->srv, fxd ? color->srv : depth->srv,
                                  fxd ? g_dlss.before->srv : depth->srv };
        u.hand[2] = fminf(fmaxf(g_fxs.dlss_reactive_moving, 0.0f), 1.0f), u.hand[3] = drawn ? 1.0f : 0.0f;
        u.hist[0] = fxd ? fminf(fmaxf(g_fxs.dlss_reactive, 0.0f), 1.0f) : 0.0f;
        u.hist[2] = 0.05f, u.hist[3] = 0.25f; /* a change under a 20th of white is noise (a faint haze); from a quarter, all of it */
        const float none[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 2; ++i)
        {
            ID3D12Resource* mres = i ? g_dlss.res[6] : g_dlss.res[5];
            const int32_t mrtv = i ? g_dlss.rtv_t : g_dlss.rtv_r;
            if (!mres || mrtv < 0 || (i && !sh->tc))
                continue;
            u.hist[1] = (float)i; /* 1: the transparency and composition mask, the effects alone */
            barrier(mres, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (wanted)
                fx_draw(heap_cpu(&g_rtv, mrtv), in_w, in_h, g_fx.pso[FX_DLSSR], vp0, &u, r_in, 5, 0, 0);
            else
                ID3D12GraphicsCommandList_ClearRenderTargetView(list(), heap_cpu(&g_rtv, mrtv), none, 0, NULL);
            barrier(mres, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        }
        u.hand[2] = 0.0f, u.hand[3] = 0.0f, u.hist[0] = 0.0f, u.hist[1] = 0.0f, u.hist[2] = 0.0f, u.hist[3] = 0.0f;
    }
    g_gap_part = GP_WAIT_DLSS; /* the queue waits on the helper (below) till the next list */
    submit(0);
    /* handed over: the fence at `frame` once the inputs are drawn, the helper's at frame + 1 */
    const uint64_t frame = g_dlss.value + 1;
    g_dlss.value = frame + 1;
    ID3D12CommandQueue_Signal(g_queue, g_dlss.fence, frame);
    sh->render_w = in_w, sh->render_h = in_h;
    sh->jitter_x = g_dlss.jit_serial == g_serial ? g_dlss.jit[0] : 0.0f;
    sh->jitter_y = g_dlss.jit_serial == g_serial ? g_dlss.jit[1] : 0.0f;
    sh->motion_scale_x = sh->motion_scale_y = 1.0f;
    sh->reset = reset, sh->sharpness = g_dlss.upscaler == VANA_UPSCALER_FSR ? g_dlss.sharpness : 0.0f;
    if (g_dlss.upscaler == VANA_UPSCALER_FSR)
    {
        /* FSR's: the camera's planes (far 0: none) and field of view, and the time since the frame before it had (ms) */
        float zn, zf, fov;
        cam_planes(s->proj, &zn, &zf, &fov);
        sh->sr_near = zn, sh->sr_far = zf, sh->sr_fov = fov;
        const uint64_t now = gfx_now_ns();
        const double ms = g_dlss.ran_ns ? (double)(now - g_dlss.ran_ns) / 1e6 : 16.7;
        sh->sr_frame_ms = (float)(ms < 1.0 ? 1.0 : ms > 100.0 ? 100.0 : ms);
        g_dlss.ran_ns = now;
    }
    sh->nr = g_dlss.nr, sh->nr_style = (int32_t)g_fxs.dlss5_style, sh->nr_intensity = g_fxs.dlss5_intensity;
    sh->nr_fx = fx_out;
    sh->nr_tone = g_fxs.dlss5_tone, sh->nr_structure = g_fxs.dlss5_structure, sh->nr_skin = g_fxs.dlss5_skin;
    sh->nr_auto_mask = g_fxs.dlss5_mask != 0.0f;
    /* DLSS 5's picture blended back with the colour (the helper skips it at 1, 1, 0) */
    sh->nr_mix = fminf(fmaxf(g_fxs.dlss5_mix, 0.0f), 4.0f), sh->nr_colour = fminf(fmaxf(g_fxs.dlss5_colour, 0.0f), 1.0f);
    sh->nr_brighten = g_fxs.dlss5_brighten >= 1.0f ? fminf(g_fxs.dlss5_brighten, 64.0f) : 0.0f;
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        sh->posted_qpc = q.QuadPart;
    }
    MemoryBarrier();
    sh->frame = frame;
    g_dlss.last = frame; /* (before the helper can answer: br_thread reads it) */
    dlss_post(g_dlss.go, g_dlss.fence, frame);
    g_dlss.last_at = GetTickCount64(), g_dlss.reset = 0;
    ID3D12CommandQueue_Wait(g_queue, g_dlss.fence, frame + 1);
    /* the output into `out`, where the viewport's part of the scene is, at the output's scale */
    l = list();
    gpu_part(GP_DLSS_OUT);
    barrier(g_dlss.res[3], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(g_dlss.out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float kx = (float)out_w / in_w, ky = (float)out_h / in_h;
    float ovp[4] = { vx * kx, vy * ky, (float)out_w, (float)out_h };
    ID3D12PipelineState* onto = g_fx.by_fmt[fmt].mip;
    /* the colour grade, on DLSS's output: in this pass where nothing is drawn onto the output after it (depth of field,
     * dof_after, and DLSS 5's light from the effects are), else in place after them */
    const int el_post = g_el.post_serial == g_serial && g_el.color == color && g_dlss.nr;
    const int fold = grade_wanted(fmt) && !g_dlss.dof_late && !el_post;
    if (fold)
    {
        grade_uniforms(&u);
        onto = g_fx.by_fmt[fmt].post;
        g_dlss.graded = 1;
    }
    fx_draw(target_view(g_dlss.out, 0, 0), g_dlss.out->w, g_dlss.out->h, onto, ovp, &u, &g_dlss.srv_out, 1, 0, 0);
    barrier(g_dlss.res[3], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    /* the light from the effects, left for after DLSS 5 (dlss5_elight_post: el_on_draw), onto its output */
    if (g_el.post_serial == g_serial && g_el.color == color && g_dlss.nr)
    {
        g_el.post_serial = 0;
        el_run(g_dlss.out, ovp);
        gpu_part(GP_DLSS_OUT);
    }
    /* depth of field, where the composite left it for here (dof_late), then the colour grade over everything, on DLSS's
     * output (after DLSS 5 too) */
    dof_after(g_dlss.out, color, s, fmt, ovp, GP_DLSS_OUT);
    if (!fold)
        grade_in_place(g_dlss.out, fmt, ovp, GP_DLSS_OUT);
    g_dlss.scene = color, g_dlss.out_serial = g_dlss.ran_serial = g_serial;
    dlss_keep_clean(); /* before the interface held for it goes over it (dlss_replay) */
}

/* The scene noted at gfx_scene_done, all of it drawn now: through the upscaler, or not (upscale_run), and graded whichever it
 * was: the upscaler's output has the grade, and a scene it left as it is (the helper not ready, a frame it passed over) has
 * it laid on it where it is */
static void dlss_run(void)
{
    GfxTex* color = g_dlss.pend_color;
    g_dlss.graded = 0;
    upscale_run();
    if (!g_dlss.graded && !(g_dlss.out_serial == g_serial && g_dlss.scene == color))
        grade_scene(color, &g_dlss.pend_s, g_dlss.pend_fmt);
}

/* The scene as it is now - its world drawn, VanaCore's effects over it, the game's own effects not yet - copied: for the
 * upscaler's masks (fx_dlssr, dlss_run: what the game draws into the scene after this, its effects, is what differs from it
 * then, and gets no history), and with the game's effects kept out of DLSS 5 (dlss5_fx_out) for DLSS 5, where DLSS 5 runs
 * and the helper can add them back onto its picture (nr_fx_ok); the light VanaCore makes from them (el_run) goes round DLSS
 * 5 with them, not through it */
static void dlss_keep_before(GfxTex* color)
{
    g_dlss.before_serial = 0;
    const int fx_out = g_dlss.nr && g_fxs.dlss5_fx_out != 0.0f && g_dlss.sh && g_dlss.sh->nr_fx_ok && g_dlss.res[4];
    const int mask = g_dlss.sh && g_dlss.res[5] && g_fxs.dlss_reactive > 0.0f && mask_wanted();
    if ((!fx_out && !mask) || color->srv < 0)
        return;
    if (g_dlss.before && (g_dlss.before->w != color->w || g_dlss.before->h != color->h || g_dlss.before->fmt != color->fmt))
        gfx_tex_destroy(g_dlss.before), g_dlss.before = NULL;
    if (!g_dlss.before)
        g_dlss.before = gfx_tex_create(GFX_TEX_2D, color->fmt, color->w, color->h, 1, GFX_USE_RT);
    if (!g_dlss.before || g_dlss.before->srv < 0)
        return;
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to), memset(&from, 0, sizeof from);
    from.pResource = color->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, from.SubresourceIndex = 0;
    to.pResource = g_dlss.before->res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, to.SubresourceIndex = 0;
    const int part_was = gpu_part_cur();
    gpu_part(GP_COPY);
    tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex_state(g_dlss.before, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, 0, 0, 0, &from, NULL);
    tex_state(g_dlss.before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu_part(part_was);
    g_dlss.before_serial = g_serial;
}

/* After the effects: the scene noted, for DLSS and the colour grade once the game is done drawing into it */
static void dlss_frame(GfxTex* color, GfxTex* depth, const GfxScene* s, int fmt)
{
    /* the comparison: as the game would stretch it (graded, as the picture it's compared with is) */
    if (g_fxs.dlss_compare == 0.0f)
    {
        if (!s->present_w || s->present_w >= color->w)
            g_dlss.noted = g_serial; /* not the game's 4096 glow picture, larger than it's shown (dlss_run leaves it) */
        dlss_keep_before(color);
    }
    g_dlss.pend_color = color, g_dlss.pend_depth = depth, g_dlss.pend_fmt = fmt;
    memcpy(&g_dlss.pend_s, s, sizeof *s);
    g_dlss.pend_serial = g_serial;
    g_dlss_replay_depth = NULL;
    g_nheld = 0, g_held_len = 0; /* a frame before's, its scene never read: gone with it */
}

/* The interface drawn into the scene before the game is done with it (names, floating text - the game may draw more
 * of the world after them: a glare, a spell, which DLSS must have): held, with copies of their vertices and indices,
 * and drawn over DLSS's output once it has run (dlss_replay); drawn into the scene as the game drew them if it
 * didn't. A draw from a buffer the game may write again before then isn't held (DLSS runs at it instead). */
typedef struct
{
    GfxDraw d;
    GfxTex *rt, *ds;
    uint32_t face, level;
    size_t data[GFX_NSTREAMS], indices; /* offsets + 1 into g_held_mem (0: none) */
} Held;
static Held* g_held;
static uint32_t g_cap_held;
static uint8_t* g_held_mem;
static size_t g_held_cap;

/* a copy of n bytes at p: its offset + 1 (0 for none, (size_t)-1 when there's no memory for it) */
static size_t held_copy(const void* p, size_t n)
{
    if (!p || !n)
        return 0;
    if (g_held_len + n + 16 > g_held_cap)
    {
        size_t cap = (g_held_len + n + 16) * 2 + 65536;
        uint8_t* m = (uint8_t*)realloc(g_held_mem, cap);
        if (!m)
            return (size_t)-1;
        g_held_mem = m, g_held_cap = cap;
    }
    size_t at = g_held_len;
    memcpy(g_held_mem + at, p, n);
    g_held_len += (n + 15) & ~(size_t)15;
    return at + 1;
}

static int dlss_hold(const GfxDraw* d)
{
    if (d->ibuf || d->caster)
        return 0;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (d->buf[s])
            return 0;
    if (g_nheld == g_cap_held)
    {
        uint32_t cap = g_cap_held ? g_cap_held * 2 : 64;
        Held* more = (Held*)realloc(g_held, cap * sizeof *more);
        if (!more)
            return 0;
        g_held = more, g_cap_held = cap;
    }
    Held* h = &g_held[g_nheld];
    const size_t len = g_held_len;
    h->d = *d;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        h->data[s] = held_copy(d->data[s], d->size[s]);
        h->d.cpu[s] = NULL, h->d.cpu_size[s] = 0;
    }
    const uint32_t n = d->prim == GFX_TRIANGLEFAN ? d->count + 2 : vertex_count(d->prim, d->count);
    h->indices = held_copy(d->indices, (size_t)n * d->index_size);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (h->data[s] == (size_t)-1 || h->indices == (size_t)-1)
            return g_held_len = len, 0;
    h->rt = g_rt, h->ds = g_ds, h->face = g_rt_face, h->level = g_rt_level;
    g_nheld++;
    return 1;
}

static void dlss_draw_over(const GfxDraw* d);

/* The scene's depth at DLSS's output size (the render pixel under each output pixel), so the interface drawn over
 * DLSS's output - the names over heads - is hidden behind what's in front of it as it is without DLSS. NULL when it
 * can't be made (the interface is then drawn over everything). */
static GfxTex* dlss_depth_up(void)
{
    if (g_zup_serial == g_serial && g_dlss_zups[g_zup_cur])
        return g_dlss_zups[g_zup_cur]; /* made this frame already (the light from the effects, then the held interface) */
    GfxTex *depth = g_dlss.pend_depth, *scene = g_dlss.scene, *out = g_dlss.out;
    if (!depth || !scene || !out || depth->use != GFX_USE_DEPTH || depth->srv < 0 || !g_fx.depthup || !g_fx.vs)
        return NULL;
    /* the frame before's, if it was the frame just before, at this size, and the camera didn't jump (DLSS's reset) */
    GfxTex* prev = g_dlss_zups[g_zup_cur];
    const int held = prev && prev->w == out->w && prev->h == out->h && prev->dxfmt == depth->dxfmt && prev->srv >= 0 &&
                     g_zup_serial + 1 == g_serial && !(g_dlss.sh && g_dlss.sh->reset);
    g_zup_cur ^= 1;
    GfxTex* zup = g_dlss_zups[g_zup_cur];
    if (!zup || zup->w != out->w || zup->h != out->h || zup->dxfmt != depth->dxfmt)
    {
        gfx_tex_destroy(zup);
        zup = g_dlss_zups[g_zup_cur] = gfx_tex_create(GFX_TEX_2D, depth->fmt, out->w, out->h, 1, GFX_USE_DEPTH);
    }
    g_zup_serial = 0;
    if (!zup)
        return NULL;
    if (!g_zup_pso || g_zup_fmt != depth->dxfmt)
    {
        if (g_zup_pso)
            defer((IUnknown*)g_zup_pso, NULL, -1), g_zup_pso = NULL;
        g_zup_pso = fx_make_depth_pso(g_fx.depthup, depth->dxfmt);
        g_zup_fmt = depth->dxfmt;
    }
    if (!g_zup_pso)
        return NULL;
    FxU u;
    memset(&u, 0, sizeof u);
    u.size[0] = (float)out->w, u.size[1] = (float)out->h;
    u.vp[2] = (float)scene->w, u.vp[3] = (float)scene->h;
    if (g_dlss.jit_serial == g_serial)
        u.vp[0] = g_dlss.jit[0], u.vp[1] = g_dlss.jit[1];
    u.hist[0] = held ? 1.0f : 0.0f;
    tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (held)
        tex_state(prev, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(zup, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const int32_t in[2] = { depth->srv, held ? prev->srv : -1 };
    fx_draw_depth(target_view(zup, 0, 0), out->w, out->h, g_zup_pso, &u, in);
    g_zup_serial = g_serial;
    return zup;
}

static void fgn_keep(void);

static void dlss_replay(void)
{
    for (uint32_t i = 0; i < g_nheld; ++i)
    {
        Held* h = &g_held[i];
        for (int s = 0; s < GFX_NSTREAMS; ++s)
            h->d.data[s] = h->data[s] ? g_held_mem + h->data[s] - 1 : NULL;
        h->d.indices = h->indices ? g_held_mem + h->indices - 1 : NULL;
        if (g_dlss.out_serial == g_serial && h->rt == g_dlss.scene)
            dlss_draw_over(&h->d);
        else
        {
            GfxTex *rt = g_rt, *ds = g_ds;
            uint32_t face = g_rt_face, level = g_rt_level;
            g_rt = h->rt, g_ds = h->ds, g_rt_face = h->face, g_rt_level = h->level, g_targets_bound = 0;
            draw_encode(&h->d);
            g_rt = rt, g_ds = ds, g_rt_face = face, g_rt_level = level, g_targets_bound = 0;
        }
    }
    fgn_keep(); /* for frame generation's frames, at the frame's end */
    g_nheld = 0, g_held_len = 0;
}

/* Before each draw: the noted scene through DLSS once it's all drawn - at the first draw reading it (the composite
 * onto the back buffer); the interface drawn into it before then held, and drawn over DLSS's output (at its size)
 * after. 1 when dlss_draw_over draws this draw, 2 when it's held, 0 to draw it as usual. A 3D draw into the scene's target
 * after DLSS ran ends its output's use (the scene read as it is from then). */
static int dlss_on_draw(const GfxDraw* d)
{
    if (g_dlss.pend_serial == g_serial && g_dlss.pend_color)
    {
        if (g_rt == g_dlss.pend_color && d->overlay && dlss_hold(d))
            return 2;
        int reads = 0;
        for (int i = 0; i < 8; ++i)
            reads |= d->tex[i] == g_dlss.pend_color;
        /* a read into a small target isn't the scene laid onto the frame: a distortion effect's grab of it (into
         * 256x256, then drawn back into the scene in 3D - the vortex by a monster's spawn point), the sun flare's
         * 16x16 probe. DLSS waits for the composite: run at the grab, the 3D drawn back after it ended DLSS's output
         * for the frame, and the game laid the small scene onto the frame stretched */
        if (reads && g_rt &&
            (uint64_t)g_rt->w * g_rt->h * 2 < (uint64_t)g_dlss.pend_s.present_w * g_dlss.pend_s.present_h)
            reads = 0;
        if (g_rt == g_dlss.pend_color ? d->overlay : reads)
        {
            g_dlss.pend_serial = 0;
            const int scope_was = gpu_scope_cur(), part_was = gpu_part_cur();
            mo_flush(0); /* the effects' redraws into the motion target, before it's read (in the game's draws, not DLSS's) */
            gpu_scope(GS_DLSS);
            dlss_run();
            gpu_scope(scope_was);
            /* the light from the effects left for DLSS 5's output, which it didn't make this frame: into the scene, as
             * without it, before the draw reading it */
            if (g_el.post_serial == g_serial)
            {
                g_el.post_serial = 0;
                el_run(NULL, NULL);
            }
            if (g_nheld)
                gpu_part(GP_REPLAY);
            g_dlss_replay_depth = g_dlss.out_serial == g_serial ? dlss_depth_up() : NULL;
            dlss_replay();
            gpu_part(part_was);
        }
    }
    if (g_dlss.out_serial != g_serial || !g_rt)
        return 0;
    if (g_rt != g_dlss.scene)
    {
        /* the scene laid onto the frame: the frame then is frame generation's hudless */
        for (int i = 0; i < 8; ++i)
            g_fg_hudless_next |= d->tex[i] == g_dlss.scene;
        return 0;
    }
    if (d->overlay)
        return 1;
    g_dlss.out_serial = 0;
    return 0;
}

static void draw_encode(const GfxDraw* d);

/* An interface draw into the scene's target after DLSS ran: onto its output instead, its viewport at the output's
 * scale, tested against the scene's depth at the output's size (dlss_depth_up) rather than the scene's own (smaller) */
static void dlss_draw_over(const GfxDraw* d)
{
    static GfxDraw over;
    over = *d;
    GfxTex* scene = g_dlss.scene;
    double kx = (double)g_dlss.out->w / scene->w, ky = (double)g_dlss.out->h / scene->h;
    over.vp[0] = (uint32_t)(over.vp[0] * kx + 0.5), over.vp[1] = (uint32_t)(over.vp[1] * ky + 0.5);
    over.vp[2] = (uint32_t)(over.vp[2] * kx + 0.5), over.vp[3] = (uint32_t)(over.vp[3] * ky + 0.5);
    GfxTex* zup = g_dlss_replay_depth; /* the scene's depth at the output's size, if it could be made */
    if (!zup)
        over.depth.zenable = 0;
    over.depth.zwrite = 0;
    GfxTex *rt = g_rt, *ds = g_ds;
    uint32_t face = g_rt_face, level = g_rt_level;
    g_rt = g_dlss.out, g_ds = zup, g_rt_face = g_rt_level = 0, g_targets_bound = 0;
    GfxTex* seen = g_dlss.out->depth_seen;
    draw_encode(&over);
    g_dlss.out->depth_seen = seen; /* the depth bound for this was for it: not what the scene's effects read */
    g_rt = rt, g_ds = ds, g_rt_face = face, g_rt_level = level, g_targets_bound = 0;
}

/* --- Each moving thing's own motion for DLSS ([graphics.effects] dlss_motion) -------------------------------------
 * DLSS (FSR, frame generation) is told where each pixel was the frame before. From the depth and the camera alone
 * (fx_dlssmv), a character's swinging arm or a spell's flight reads as standing still, and the game's effects - which write
 * no depth - as moving with the wall behind them: DLSS smears them, and DLSS 5 has nothing to go on for them. Here the
 * moving things are drawn again (mo_draw) into the motion target (g_mo.t), each pixel's motion in place of its colour: the
 * scene's draws that move (vertices made again each frame - the characters - or a world matrix that moved) before its
 * effects, and every one of the game's effects after them. fx_dlssmv takes it where it has some. Off by default
 * (dlss_motion 0): the match below is a guess, and each way it broke was a glitch (a character warped, a mote's wall torn, a
 * crystal doubled); the camera's motion is handed over alone, and the upscaler's masks (mask_draw, fx_dlssr) say where it
 * isn't the thing's own. With frame generation an effect's model still takes its own (fg_model), which it tore without.
 *
 * Where a draw was the frame before comes from finding it among the frame before's draws: by what it draws (its texture,
 * its buffers, its vertices' count and layout) and which of the frame's draws like it it is (the first, the second...). A
 * draw whose vertices are made again keeps its positions for the next frame (float3 each, in an arena a frame); one drawn
 * from a buffer the game keeps, its transform. One not found (new, or an effect whose particles came and went), or found
 * too far from where it is now (a vertex more than MO_JUMP away: not the same one), is left to the depth's motion - but for
 * an effect, which moves with the camera at its own depth then, not the wall's behind it. */

/* the frame's tables, the first time a frame asks: this frame's afresh, the last one's the frame before's if it was the
 * frame just before. 0 when there's no memory for them */
static int mo_frame(void)
{
    if (!g_mo.e[0])
    {
        g_mo.e[0] = (MoEntry*)calloc(MO_SLOTS, sizeof(MoEntry));
        g_mo.e[1] = (MoEntry*)calloc(MO_SLOTS, sizeof(MoEntry));
        g_mo.count = (MoCount*)calloc(MO_SLOTS, sizeof(MoCount));
        g_mo.stood[0] = (MoStood*)calloc(MO_SLOTS, sizeof(MoStood));
        g_mo.stood[1] = (MoStood*)calloc(MO_SLOTS, sizeof(MoStood));
        if (!g_mo.e[0] || !g_mo.e[1] || !g_mo.count || !g_mo.stood[0] || !g_mo.stood[1])
        {
            free(g_mo.e[0]), free(g_mo.e[1]), free(g_mo.count), free(g_mo.stood[0]), free(g_mo.stood[1]);
            g_mo.e[0] = g_mo.e[1] = NULL, g_mo.count = NULL, g_mo.stood[0] = g_mo.stood[1] = NULL;
            return 0;
        }
    }
    if (g_mo.serial[g_mo.cur] == g_serial)
        return 1;
    const uint64_t last = g_mo.serial[g_mo.cur];
    g_mo.cur ^= 1;
    g_mo.serial[g_mo.cur] = g_serial, g_mo.len[g_mo.cur] = 0;
    if (last + 1 != g_serial)
        g_mo.serial[g_mo.cur ^ 1] = 0; /* not the frame just before: nothing to find in it */
    g_mo.drawn = g_mo.kept = 0;
    return 1;
}

/* the entry for key in table t (made in this frame's, with make), or NULL */
static MoEntry* mo_find(int t, uint64_t key, int make)
{
    MoEntry* tab = g_mo.e[t];
    const uint64_t serial = g_mo.serial[t];
    if (!tab || !serial)
        return NULL;
    uint32_t i = (uint32_t)(key ^ key >> 29) & (MO_SLOTS - 1);
    for (int tries = 0; tries < 64; ++tries, i = (i + 1) & (MO_SLOTS - 1))
    {
        MoEntry* e = &tab[i];
        if (e->serial != serial)
        {
            if (!make)
                return NULL;
            memset(e, 0, sizeof *e);
            e->serial = serial, e->key = key;
            return e;
        }
        if (e->key == key)
            return make ? NULL : e; /* (made twice in a frame: a key's occurrence is counted, so never) */
    }
    return NULL;
}

/* which of this frame's draws like id this one is: 0, 1, 2... */
static uint32_t mo_occurrence(uint64_t id)
{
    uint32_t i = (uint32_t)(id ^ id >> 31) & (MO_SLOTS - 1);
    for (int tries = 0; tries < 64; ++tries, i = (i + 1) & (MO_SLOTS - 1))
    {
        MoCount* c = &g_mo.count[i];
        if (c->serial != g_serial)
        {
            c->serial = g_serial, c->id = id, c->n = 1;
            return 0;
        }
        if (c->id == id)
            return c->n++;
    }
    return 0xFFFFFFFFu; /* (the table's that full: it's an occurrence of its own) */
}

static uint64_t mo_hash(const void* p, size_t n, uint64_t h)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static int fg_wanted(void);

/* whether DLSS (or AMD's FSR) is taking this frame's scene, with depth and motion (FSR 1 has no motion), and the moving things'
 * own motion is asked for: dlss_motion, the characters' (dlss_motion_chars), or frame generation's effect models' (fg_fx_motion;
 * mo_draw draws those alone then) */
static int mo_wanted(void)
{
    return (g_fxs.dlss_motion != 0.0f || g_fxs.dlss_motion_chars != 0.0f || (g_fxs.fg_fx_motion > 0.0f && fg_wanted())) &&
           g_dlss.quality >= 0 && !dlss_gave_up() &&
           !fsr1_wanted() && g_dlss.in_w && g_dlss.sh && g_fxs.dlss_compare == 0.0f;
}

/* an effect's transform the frame before as the camera alone moved it: where it is in the world now (this frame's view taken
 * off; an effect drawn round the camera, with the view's turn alone, is the camera's offset from it, which comes out the same)
 * through the frame before's view and projection. 0 when there's no frame just before */
static int mo_camera(const GfxDraw* d, float m[16])
{
    float vinv[16], world[16], t[16];
    if (g_dlss.prev_serial + 1 != g_serial || !mat_inverse(vinv, g_dlss.pend_s.view))
        return 0;
    mat_mul(world, d->u.wv, vinv);
    mat_mul(t, world, g_dlss.prev_view);
    mat_mul(m, t, g_dlss.prev_proj);
    return 1;
}

/* a vertex at clip-space a now and b the frame before a quarter of the screen or more apart: not the same draw found (two alike
 * that swapped places in the frame's order), whatever moved */
static int mo_far_clip(const float* a, const float* b)
{
    if (a[3] <= 1e-5f || b[3] <= 1e-5f)
        return 0; /* behind the camera, either: nothing to tell by */
    const float dx = a[0] / a[3] - b[0] / b[3], dy = a[1] / a[3] - b[1] / b[3];
    return dx * dx + dy * dy > 0.25f;
}

/* ... a vertex at p now (through now_m) and at q the frame before (through was_m) */
static int mo_far(const float* p, const float* q, const float* now_m, const float* was_m)
{
    float a[4], b[4];
    for (int j = 0; j < 4; ++j)
    {
        a[j] = p[0] * now_m[j] + p[1] * now_m[4 + j] + p[2] * now_m[8 + j] + now_m[12 + j];
        b[j] = q[0] * was_m[j] + q[1] * was_m[4 + j] + q[2] * was_m[8 + j] + was_m[12 + j];
    }
    return mo_far_clip(a, b);
}

/* room for `floats` more in frame c's arena: 0 when there's none to be had */
static int mo_room(int c, uint32_t floats)
{
    if (g_mo.len[c] + floats > g_mo.cap[c])
    {
        uint32_t cap = (g_mo.len[c] + floats) * 2 + 65536;
        float* more = (float*)realloc(g_mo.arena[c], (size_t)cap * sizeof(float));
        if (more)
            g_mo.arena[c] = more, g_mo.cap[c] = cap;
    }
    return g_mo.len[c] + floats <= g_mo.cap[c];
}

int gfx_draw_clip0(const GfxDraw* d, float out[4]);

/* whether the frame before drew one of this kind (mo_draw's) standing just where anchor is: an object placed by a world
 * matrix that hasn't moved, found under another's occurrence - an alike ahead of it in the frame's order came into view
 * or left it, and the one at its occurrence the frame before is the next along (a row of pillar caps a step apart) */
static int mo_stood(uint64_t kind, const float* anchor)
{
    /* (by the kind and the anchor at once, in the frame before's stood table: a walk of the kind's occurrences, each a
     * probe, went quadratic in a crowd of alike pieces - a zone drawn from its quadtree, 12,000 draws a frame, spent a
     * third of the frame here) */
    const MoStood* tab = g_mo.stood[g_mo.cur ^ 1];
    const uint64_t serial = g_mo.serial[g_mo.cur ^ 1];
    if (!tab || !serial)
        return 0;
    const uint64_t key = mo_hash(anchor, 3 * sizeof *anchor, kind) | 1;
    uint32_t i = (uint32_t)(key ^ key >> 29) & (MO_SLOTS - 1);
    for (int tries = 0; tries < 64; ++tries, i = (i + 1) & (MO_SLOTS - 1))
    {
        const MoStood* e = &tab[i];
        if (e->serial != serial)
            return 0;
        if (e->key == key)
            return 1;
    }
    return 0;
}

/* this frame drew one of this kind standing at anchor (its world matrix's place; [3] 0: it has none, nothing noted) */
static void mo_stand(uint64_t kind, const float* anchor)
{
    MoStood* tab = g_mo.stood[g_mo.cur];
    const uint64_t serial = g_mo.serial[g_mo.cur];
    if (!tab || !serial || anchor[3] == 0.0f)
        return;
    const uint64_t key = mo_hash(anchor, 3 * sizeof *anchor, kind) | 1;
    uint32_t i = (uint32_t)(key ^ key >> 29) & (MO_SLOTS - 1);
    for (int tries = 0; tries < 64; ++tries, i = (i + 1) & (MO_SLOTS - 1))
    {
        MoStood* e = &tab[i];
        if (e->serial != serial)
        {
            e->serial = serial, e->key = key;
            return;
        }
        if (e->key == key)
            return;
    }
}

/* the frame before's draw of this kind (mo_draw's) with n positions that stood nearest pos's (its first, last and two between):
 * a character's part, found by where it is rather than by its place in the frame's order. Two in the same gear swap places in
 * that order as others come into view or leave it, and the one at a part's occurrence the frame before was then the other's;
 * two halves of one body alike in all but where they are share their first vertex's neighbourhood, not the rest.
 * A part new in view has no part the frame before: its nearest alike was a neighbour's in the same gear (two players in one
 * hat, a step apart), which gave it that one's place as where it had been - FSR fetched its history from there and showed
 * the other's hat through it. So none counts that any of its samples left by more than MO_CHAR_JUMP, or that another part took
 * already this frame; a part left without one is the Stability mask's (mo_draw) */
static const MoEntry* mo_nearest(uint64_t kind, uint32_t n, const float* pos)
{
    const uint32_t at[4] = {0, n / 3, 2 * n / 3, n - 1};
    MoEntry* best = NULL;
    float best_d2 = 0.0f;
    for (uint32_t occ = 0; occ < 256; ++occ)
    {
        MoEntry* e = mo_find(g_mo.cur ^ 1, mo_hash(&occ, sizeof occ, kind) | 1, 0);
        if (!e)
            break;
        if (e->n != n || e->claim == g_serial)
            continue;
        const float* p = g_mo.arena[g_mo.cur ^ 1] + e->pos;
        float d2 = 0.0f, far2 = 0.0f;
        for (int j = 0; j < 4; ++j)
        {
            const float *a = p + 3 * at[j], *b = pos + 3 * at[j];
            const float s = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
            d2 += s;
            far2 = s > far2 ? s : far2;
        }
        if (far2 > MO_CHAR_JUMP * MO_CHAR_JUMP)
            continue;
        if (!best || d2 < best_d2)
            best = e, best_d2 = d2;
    }
    if (best)
        best->claim = g_serial;
    return best;
}

static int fg_wanted(void);

/* --- The moving things' redraws, drawn together ([graphics.effects] dlss_motion_batch) ----------------------------------
 * A redraw into the motion target (mo_encode) or the mask (mask_draw) was encoded just after its draw: the targets bound for it
 * and the scene's again after, its pipeline and the scene's in turn, its uniforms copied again - as dear as a draw of its own,
 * for each of a crowd's 1,300 character parts (6 ms of a 27 ms frame in a town's crowd, 2026-10-25). Each is kept instead
 * (mo_keep), with what its draw bound (g_enc), and they're drawn together (mo_flush): before the targets are read (dlss_run),
 * when the scene is done, at the frame's end, or before what they read changes - the depth they're tested against (another
 * target, a clear), a buffer or texture (its mo_batch: an upload, its end). Tested against the scene's finished depth, a moving
 * thing behind something drawn after it leaves no motion over that */
typedef struct MoRec
{
    ID3D12PipelineState* pso;
    GfxTex* ds;                    /* the depth it's tested against */
    GfxTex* tex[8];                /* the textures it samples, for their state */
    GfxBuf* buf[GFX_NSTREAMS + 1]; /* the buffers it reads where they are (the last: its indices), for their state */
    D3D12_GPU_VIRTUAL_ADDRESS vb[GFX_NSTREAMS], ub;
    D3D12_INDEX_BUFFER_VIEW ibv; /* BufferLocation 0: no indices */
    D3D12_VIEWPORT vp;
    uint32_t bind[16];
    uint32_t n, vstart;
    float sun[20]; /* the Sun buffer's: g_mo_m, then g_mo_f */
    uint8_t topo;  /* D3D12_PRIMITIVE_TOPOLOGY */
} MoRec;

static struct
{
    MoRec* r;
    uint32_t n, cap;
} g_mob[2]; /* the motion target's (0) and the mask's (1) */

/* d's redraw (g_mout, g_maskout and the rest set as for draw_encode) kept for mo_flush: 1 when it's kept (or left out: its
 * pipeline is still building, as draw_encode leaves it), 0 when it's to be drawn now - dlss_motion_batch 0, or its own draw
 * wasn't the one gfx_draw just encoded */
static int mo_keep(const GfxDraw* d)
{
    if (g_fxs.dlss_motion_batch == 0.0f || g_enc.d != d)
        return 0;
    GfxTex* ds = depth_attachment();
    ID3D12PipelineState* p = pipeline(d, ds);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE);
        return 1;
    }
    const int k = g_maskout != 0;
    if (g_mob[k].n == g_mob[k].cap)
    {
        const uint32_t cap = g_mob[k].cap ? g_mob[k].cap * 2 : 1024;
        MoRec* more = (MoRec*)realloc(g_mob[k].r, cap * sizeof(MoRec));
        if (!more)
            return 0;
        g_mob[k].r = more, g_mob[k].cap = cap;
    }
    MoRec* r = &g_mob[k].r[g_mob[k].n];
    r->pso = p, r->ds = ds;
    memcpy(r->tex, g_enc.tex, sizeof r->tex);
    memcpy(r->bind, g_enc.bind, sizeof r->bind);
    memcpy(r->vb, g_enc.vb, sizeof r->vb);
    r->ub = g_enc.ub, r->ibv = g_enc.ibv;
    memset(r->buf, 0, sizeof r->buf);
    /* a buffer of the game's: its copy in the ring when this frame's upload of it had what the draw reads (a character's,
     * rewritten between draws), else read where it is, and its next upload draws these first */
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        GfxBuf* b = d->buf[s];
        if (!b || r->vb[s] != b->gpu + d->buf_off[s])
            continue;
        const D3D12_GPU_VIRTUAL_ADDRESS up = buf_ring(b, d->buf_off[s], d->cpu_size[s]);
        if (up)
            r->vb[s] = up;
        else
            r->buf[s] = b, b->mo_batch = g_mob_id;
    }
    if (g_mout && g_mo_slot >= 0 && g_mo_slot < GFX_NSTREAMS && g_mo_va)
        r->vb[g_mo_slot] = g_mo_va; /* the frame before's positions (mo_draw) */
    r->n = vertex_count(d->prim, d->count), r->vstart = d->vertex_start;
    if (d->prim != GFX_TRIANGLEFAN && d->ibuf && r->ibv.BufferLocation)
    {
        const uint32_t ib = r->n * d->index_size;
        const D3D12_GPU_VIRTUAL_ADDRESS up = buf_ring(d->ibuf, d->ibuf_off, ib);
        if (up)
            r->ibv.BufferLocation = up, r->ibv.SizeInBytes = ib;
        else
            r->buf[GFX_NSTREAMS] = d->ibuf, d->ibuf->mo_batch = g_mob_id;
    }
    for (int i = 0; i < 8; ++i)
        if (r->tex[i])
            r->tex[i]->mo_batch = g_mob_id;
    if (ds)
        ds->mo_batch = g_mob_id;
    r->vp = viewport_for(d->vp);
    r->topo = (uint8_t)topology(d->prim);
    memcpy(r->sun, g_mo_m, sizeof g_mo_m), memcpy(r->sun + 16, g_mo_f, sizeof g_mo_f);
    g_mob[k].n++, g_mob_n++;
    return 1;
}

/* The redraws kept (mo_keep), drawn: the motion target's, then the mask's, each bound once. timed: their time counts as the
 * draws' encoding (not from within gfx_draw, which counts its own) */
static void mo_flush(int timed)
{
    if (!g_mob_n)
        return;
    const uint64_t t0 = timed && gfx_profiling ? gfx_now_ns() : 0;
    g_mob_n = 0, g_mob_id++; /* (first: what's marked with the batch drawn below asks for nothing more) */
    for (int k = 0; k < 2; ++k)
    {
        FxTex* t = k ? &g_mo.m : &g_mo.t;
        if (!g_mob[k].n || !t->res)
        {
            g_mob[k].n = 0;
            continue;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_cpu(&g_rtv, t->rtv);
        const char* what = k ? "the game's (again, into the moving things' mask, kept)" : "the game's (again, into the motion target, kept)";
        GfxTex* bound_ds = NULL;
        int bound = 0;
        for (uint32_t i = 0; i < g_mob[k].n; ++i)
        {
            const MoRec* r = &g_mob[k].r[i];
            fx_state(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
            for (int j = 0; j < 8; ++j)
                if (r->tex[j])
                    tex_state(r->tex[j], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            for (int s = 0; s <= GFX_NSTREAMS; ++s)
                if (r->buf[s])
                    buf_state(r->buf[s], BUF_READ);
            if (r->ds)
                tex_state(r->ds, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            ID3D12GraphicsCommandList* l = list();
            if (!bound || r->ds != bound_ds)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dsv;
                if (r->ds)
                    dsv = target_view(r->ds, 0, 0);
                ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, r->ds ? &dsv : NULL);
                const D3D12_RECT sc = { 0, 0, (LONG)t->w, (LONG)t->h };
                ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
                bound = 1, bound_ds = r->ds;
            }
            set_pso(r->pso);
            set_topology((D3D12_PRIMITIVE_TOPOLOGY)r->topo);
            if (memcmp(&r->vp, &g_drawn.vp, sizeof r->vp))
                ID3D12GraphicsCommandList_RSSetViewports(l, 1, &r->vp), g_drawn.vp = r->vp;
            if (memcmp(r->bind, g_drawn.bind, sizeof r->bind))
                ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, r->bind, 0), memcpy(g_drawn.bind, r->bind, sizeof r->bind);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, r->ub);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_SUN, 20, r->sun, 0);
            for (int s = 0; s < GFX_NSTREAMS; ++s)
                if (r->vb[s] != g_drawn.stream[s])
                    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + s, r->vb[s]), g_drawn.stream[s] = r->vb[s];
            DrawNote* dn = dred_draw(l, what, r->n);
            if (dn)
                dn->flags = r->ibv.BufferLocation ? DN_INDEXED : 0, dn->start = r->ibv.BufferLocation ? 0 : r->vstart;
            if (r->ibv.BufferLocation)
            {
                set_index_buffer(&r->ibv);
                ID3D12GraphicsCommandList_DrawIndexedInstanced(l, r->n, 1, 0, 0, 0);
            }
            else
                ID3D12GraphicsCommandList_DrawInstanced(l, r->n, 1, r->vstart, 0);
            if (++g_cmd_draws >= SPLIT_DRAWS) /* (as a long scene goes: the GPU working while the rest is encoded) */
                submit(0), bound = 0;
        }
        g_mob[k].n = 0;
    }
    g_targets_bound = 0; /* the target being drawn to, bound again for the next draw */
    if (t0)
        g_prof.draw_ns += gfx_now_ns() - t0;
}

/* mo_draw's draw into the motion target: d again, with m its transform the frame before, src where its positions then are
 * (GFX_MO_SAME: this frame's; else stream slot's, at va, pbytes of them before a shader's constants) */
static int mo_encode(const GfxDraw* d, const float m[16], int src, int slot, D3D12_GPU_VIRTUAL_ADDRESS va, uint32_t pbytes,
                     int fg_model)
{
    /* which of its pixels count: a blended draw's that show (its alpha over the scene, or the light it adds), an opaque one's
     * all (its alpha test aside) */
    uint8_t key8 = (uint8_t)src;
    if (d->pipe.blend)
    {
        if (d->pipe.dst == 2) /* D3DBLEND_ONE: it adds */
            key8 |= GFX_MO_ADD | (d->pipe.src == 5 ? GFX_MO_ALPHA : 0); /* (by its alpha where SRCALPHA is its factor) */
        else
            key8 |= GFX_MO_ALPHA;
    }
    if (!mout_begin())
        return 0;
    g_mout = 1, g_mo_key = key8, g_mo_slot = slot, g_mo_va = va;
    memcpy(g_mo_m, m, sizeof g_mo_m);
    g_mo_f[0] = fg_model ? g_fxs.fg_fx_motion : 0.2f;
    g_mo_f[1] = fg_model ? 2.0f : 1.0f; /* (its depth handed on too: fx_dlssz) */
    g_mo_f[2] = (float)pbytes;          /* a shader's: where its constants start in the stream (0: no positions before them) */
    if (!mo_keep(d))
    {
        g_targets_bound = 0;
        draw_encode(d);
        g_targets_bound = 0;
    }
    g_mout = 0, g_mo_slot = -1, g_mo_va = 0;
    g_mo.drawn++, g_prof.redraws++;
    return 1;
}

/* After a draw (gfx_draw): a moving thing drawn again into the motion target, with where it was the frame before */
static void mo_draw(const GfxDraw* d)
{
    g_mo_own = NULL; /* (this draw's alone: the draw before's may have stood at the same address) */
    if (!mo_wanted() || !g_rt || g_rt_face || g_rt_level || d->vs.rhw || d->overlay || d->screen_fx || d->water ||
        d->fs.water || !d->count)
        return;
    /* the game's effects (blended, into the scene after its world), or the scene's own draws before them that write the
     * depth (the world, the characters: the scene's casters, or anything drawing into the target DLSS took the frame before) */
    const int fx = g_dlss.pend_serial == g_serial && g_rt == g_dlss.pend_color;
    if (fx ? !d->pipe.blend : !(d->caster == 1 || (g_rt == g_dlss.scene && d->depth.zenable && d->depth.zwrite)))
        return;
    /* with frame generation, an effect's lone sprite (one quad) keeps the motion of the scene behind it: its pixels are mostly
     * that scene, and frame generation moved it with the sprite - the wall round a twinkling mote torn into blocks, flickering
     * as the mote's alpha crossed the motion's threshold each frame (Ru'Lude's motes). Its optical flow follows them itself.
     * An effect's model (more than a quad) keeps its own, down to fg_fx_motion's alpha: given the wall's, a home point's
     * crystal came out twice, a step apart, in every made frame */
    const int fg_model = fx && fg_wanted() && d->count > 2 && g_fxs.fg_fx_motion > 0.0f;
    if (fx && fg_wanted() && !fg_model)
        return;
    if (g_fxs.dlss_motion == 0.0f && !fg_model && (fx || g_fxs.dlss_motion_chars == 0.0f))
        return; /* the moving things' own motion not asked for: the camera's, and the masks (mask_draw, fx_dlssr) say where it's wrong */
    /* ... and an effect's model then takes the camera's motion at its own depth, as if it stood still in the world (a home
     * point's crystal, a maw's vortex: they do), not a match to the frame before's draws: the maw's parts change places in the
     * draw order, a part was now and then given another's place as where it was, and frame generation tore the maw along it */
    if (fg_model && g_fxs.dlss_motion == 0.0f)
    {
        float m[16];
        if ((d->vs.prog != 0 && d->vs_tokens != NULL) || !mo_camera(d, m))
            return; /* (a shader's: the depth's motion, as the camera's would need its program run with it) */
        mo_encode(d, m, GFX_MO_SAME, -1, 0, 0, fg_model);
        return;
    }
    const GfxElem* pe = &d->vs.el[GFX_R_POSITION];
    if (!pe->used || pe->stream >= GFX_NSTREAMS || (pe->type != GFX_FLOAT3 && pe->type != GFX_FLOAT4))
        return;
    const int ps = pe->stream;
    const uint8_t* cpu = (const uint8_t*)d->cpu[ps];
    const uint32_t bytes = d->cpu_size[ps], stride = (uint32_t)d->u.stride[ps], at = (uint32_t)d->u.offset[GFX_R_POSITION];
    if (!cpu || !stride || bytes < at + 12 || !mo_frame())
        return;
    /* made again this frame (the draw's own copy, or a buffer uploaded since the frame before): its positions are kept */
    const GfxBuf* b = d->buf[ps];
    const int built = !b || b->upload_frame + 1 >= g_serial;
    /* a vertex shader's draw: it moves by its constants too (a zone's tree, placed by them; VanaCore's grass, swayed by the time
     * in them), so they're kept with it (GFX_NVSC float4s a draw: only an alpha-tested one's - the plants - unless it's made again
     * or placed by a world matrix) and the program is run again with the frame before's for where it was (gfx_hlsl_vs1) */
    const int prog = d->vs.prog != 0 && d->vs_tokens != NULL;
    /* a character: its vertices made again each frame, in the world already (no world matrix, no shader) */
    const int chr = !fx && built && !prog && d->anchor[3] == 0.0f;
    if (g_fxs.dlss_motion == 0.0f && !chr)
        return; /* the characters' alone (dlss_motion_chars) */
    if (!fx && !built && d->anchor[3] == 0.0f && !(prog && alpha_tested(&d->fs)))
        return; /* the zone's own, from a buffer the game keeps, placed nowhere a world matrix says: it never moves */
    /* the zone's plants (and VanaCore's grass): the depth's motion unless asked (dlss_motion_plants). Frame generation tore them
     * when given their own, and its optical flow follows their sway by itself; a shader's draw stays out with them (its motion
     * is theirs alone: nothing else of the scene is drawn by one and moves) */
    if (!fx && g_fxs.dlss_motion_plants == 0.0f && draw_plant(d))
        return;
    uint32_t n = (bytes - at - 12) / stride + 1;
    if (n > MO_MOST)
        return;
    /* the draw, by what it draws, and which of the frame's draws like it it is */
    struct
    {
        const void *tex, *buf, *ibuf;
        uint32_t buf_off, bytes, stride, at, vofs, count, prim, index_size, ibuf_off, vstart, prog;
        uint8_t fx, built, blend, dst;
    } id;
    memset(&id, 0, sizeof id);
    id.tex = d->tex[0], id.buf = b, id.ibuf = d->ibuf;
    id.buf_off = b ? d->buf_off[ps] : 0, id.bytes = bytes, id.stride = stride, id.at = at, id.vofs = (uint32_t)d->u.vofs;
    id.count = d->count, id.prim = d->prim, id.index_size = d->index_size, id.ibuf_off = d->ibuf_off, id.vstart = d->vertex_start;
    id.prog = d->vs.prog;
    id.fx = (uint8_t)fx, id.built = (uint8_t)built, id.blend = d->pipe.blend, id.dst = d->pipe.dst;
    /* a character's part: where the game put its vertices and indices this frame isn't what it is - a buffer it fills again
     * each frame, at a place that shifts as others are drawn before it, which left a part not found (the camera's motion and
     * the mask) one frame and found the next, flickering; mo_nearest tells the alike apart by where they stand */
    if (chr)
        id.buf = id.ibuf = NULL, id.buf_off = id.ibuf_off = id.vstart = id.vofs = 0;
    const uint64_t kind = mo_hash(&id, sizeof id, 0xCBF29CE484222325ull);
    const uint32_t occ = mo_occurrence(kind);
    const uint64_t key = mo_hash(&occ, sizeof occ, kind) | 1;
    MoEntry* now = mo_find(g_mo.cur, key, 1);
    const MoEntry* was = chr ? NULL : mo_find(g_mo.cur ^ 1, key, 0);
    /* this frame's, kept for the next: its transform, where it stands, and (made again) its positions */
    const float* pos = NULL;
    if (now)
    {
        memcpy(now->wvp, d->u.wvp, sizeof now->wvp);
        memcpy(now->anchor, d->anchor, sizeof now->anchor);
        mo_stand(kind, d->anchor);
        const int c = g_mo.cur;
        if (prog) /* (before the positions: the arena may move) */
        {
            const uint32_t nc = GFX_NVSC * 4;
            if (mo_room(c, nc))
            {
                memcpy(g_mo.arena[c] + g_mo.len[c], d->u.vsc, sizeof d->u.vsc);
                now->cst = g_mo.len[c] + 1, g_mo.len[c] += nc;
            }
            now->has_clip = (uint8_t)gfx_draw_clip0(d, now->clip0);
        }
        if (built)
        {
            if (mo_room(c, 3 * n))
            {
                float* p = g_mo.arena[c] + g_mo.len[c];
                for (uint32_t i = 0; i < n; ++i)
                    memcpy(p + 3 * i, cpu + at + (size_t)i * stride, 12);
                now->pos = g_mo.len[c], now->n = n, g_mo.len[c] += 3 * n;
                pos = p;
            }
        }
        g_mo.kept++;
    }
    if (chr && pos)
        was = mo_nearest(kind, n, pos); /* a character's part: by where it is, not its place in the order */
    /* the frame before's, if it's plausibly the same draw: nothing in it moved further than MO_JUMP */
    const float* prev = was && was->n == n && pos ? g_mo.arena[g_mo.cur ^ 1] + was->pos : NULL;
    const float* pcst = prog && was && was->cst ? g_mo.arena[g_mo.cur ^ 1] + was->cst - 1 : NULL;
    int found = was && (built ? prev != NULL : 1) && (!prog || pcst), moving = 0;
    if (found && was->anchor[3] != 0.0f && d->anchor[3] != 0.0f)
    {
        const float dx = d->anchor[0] - was->anchor[0], dy = d->anchor[1] - was->anchor[1], dz = d->anchor[2] - was->anchor[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        found = d2 <= MO_JUMP * MO_JUMP;
        moving = d2 > 0.0f;
        /* ... unless one like it stood just here the frame before: this one, under another occurrence. Given the next
         * one's place as where it was, it moved a step that frame, and frame generation tore it along the way */
        if (moving && !built && mo_stood(kind, d->anchor))
            return;
    }
    if (found && prev)
    {
        float most = 0.0f;
        for (uint32_t i = 0; i < 3 * n; i += 3)
        {
            const float dx = pos[i] - prev[i], dy = pos[i + 1] - prev[i + 1], dz = pos[i + 2] - prev[i + 2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            most = d2 > most ? d2 : most;
        }
        found = most <= MO_JUMP * MO_JUMP;
        moving |= most > 0.0f;
    }
    if (found && pcst && memcmp(pcst, d->u.vsc, sizeof d->u.vsc) != 0)
        moving = 1; /* its constants changed: placed elsewhere, or swayed by them */
    if (found && prog)
    {
        /* through its program: this frame's first vertex against the frame before's, as the shader made them (u.wvp isn't its) */
        found = now && now->has_clip && was->has_clip && !mo_far_clip(now->clip0, was->clip0);
    }
    else if (found)
    {
        float p0[3];
        memcpy(p0, cpu + at, 12);
        found = !mo_far(p0, prev ? prev : p0, d->u.wvp, was->wvp);
    }
    if (!fx && found && moving)
        g_mo_moved = d, g_mo_moved_serial = g_serial; /* (mask_draw: an object placed by a world matrix that moved) */
    /* where it was: the frame before's transform, with its positions or this frame's; an effect not found, the camera's
     * motion at its own depth. A scene draw that stands still (or wasn't found) is the depth's already */
    int src = GFX_MO_SAME, slot = -1;
    float m[16];
    if (found && (fx || moving))
    {
        memcpy(m, was->wvp, sizeof m);
        if (prev || prog)
        {
            /* a stream the draw doesn't read, for the frame before's positions (and a shader's constants) */
            int used = 0;
            for (int r = 0; r < GFX_NREGS; ++r)
                if (d->vs.el[r].used)
                    used |= 1 << d->vs.el[r].stream;
            for (int st = 0; st < GFX_NSTREAMS && slot < 0; ++st)
                if (!(used >> st & 1))
                    slot = st;
            if (slot >= 0)
                src = 1 + slot;
            else if (prog || !fx || !mo_camera(d, m))
                return;
        }
    }
    else
    {
        if (chr && found)
            g_mo_own = d, g_mo_own_serial = g_serial; /* a character standing still: the depth's motion is its own */
        if (prog || !fx || !mo_camera(d, m))
            return; /* (a shader's effect not found: the depth's motion, as the camera's would need its program run with it) */
    }
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    const uint32_t pbytes = prev ? n * 12 : 0, cbytes = prog ? (uint32_t)sizeof d->u.vsc : 0;
    if (src != GFX_MO_SAME)
    {
        Alloc a = ring((size_t)pbytes + cbytes + 16, 16); /* the slack: a last element read whole */
        if (!a.cpu)
            return;
        if (prev)
            memcpy(a.cpu, prev, pbytes);
        if (prog)
            memcpy((uint8_t*)a.cpu + pbytes, pcst, cbytes); /* the frame before's constants, after the positions (gfx_hlsl_vs1: sun_f.z) */
        va = a.gpu;
    }
    if (mo_encode(d, m, src, slot, va, pbytes, fg_model) && chr)
        g_mo_own = d, g_mo_own_serial = g_serial;
}

/* An object placed by a world matrix (anchor), drawn from a buffer the game keeps: whether it stood elsewhere the frame before
 * (mask_draw, with the moving things' own motion not asked for: mo_draw's tables are this pass's alone then). By the draw's kind
 * (what it draws) and which of the frame's draws like it it is, as mo_draw finds a draw; one not found (new in view), or whose
 * kind stood just here under another occurrence (the draw order changed), didn't move. A wrong match marks a still thing for a
 * frame, which only softens it there: nothing like a wrong motion */
static int mask_moved(const GfxDraw* d)
{
    const GfxElem* pe = &d->vs.el[GFX_R_POSITION];
    const int ps = pe->stream;
    const GfxBuf* b = d->buf[ps];
    if (!mo_frame())
        return 0;
    struct
    {
        const void *tex, *buf, *ibuf;
        uint32_t buf_off, count, prim, vstart, index_size, ibuf_off;
    } id;
    memset(&id, 0, sizeof id);
    id.tex = d->tex[0], id.buf = b, id.ibuf = d->ibuf;
    id.buf_off = b ? d->buf_off[ps] : 0, id.count = d->count, id.prim = d->prim, id.vstart = d->vertex_start;
    id.index_size = d->index_size, id.ibuf_off = d->ibuf_off;
    const uint64_t kind = mo_hash(&id, sizeof id, 0xCBF29CE484222325ull);
    const uint32_t occ = mo_occurrence(kind);
    const uint64_t key = mo_hash(&occ, sizeof occ, kind) | 1;
    const MoEntry* was = mo_find(g_mo.cur ^ 1, key, 0);
    MoEntry* now = mo_find(g_mo.cur, key, 1);
    if (now)
    {
        memcpy(now->wvp, d->u.wvp, sizeof now->wvp);
        memcpy(now->anchor, d->anchor, sizeof now->anchor);
        mo_stand(kind, d->anchor);
        g_mo.kept++;
    }
    if (!was || was->anchor[3] == 0.0f)
        return 0;
    const float dx = d->anchor[0] - was->anchor[0], dy = d->anchor[1] - was->anchor[1], dz = d->anchor[2] - was->anchor[2];
    if (dx * dx + dy * dy + dz * dz == 0.0f)
        return 0;
    return !mo_stood(kind, d->anchor);
}

/* After a draw (gfx_draw), with DLSS or AMD's FSR gathering the frames: a depth-writing draw of the scene that moves on its own,
 * drawn again into the moving things' mask (g_mo.m: 1 and the view z), where it shows past its alpha test - a character (its
 * vertices made again each frame), the zone's plants and VanaCore's grass (a tree's leaves, grass: the game's vertex shaders', or
 * placed by a world matrix with their vertices rewritten as they sway), and an object placed by a world matrix that stood elsewhere
 * the frame before (a boat, a lift). The motion they're given is the camera's (fx_dlssmv) unless dlss_motion asks for theirs, which
 * fetches their history from where the wall behind them was: the mask (fx_dlssr, dlss_reactive_moving) has the upscaler lean on
 * this frame's picture there instead. A static zone draw from a buffer the game keeps stays out: the camera's motion is its own,
 * exactly, and the history is where the upscaler's gain is. */
static void mask_draw(const GfxDraw* d)
{
    if (!mask_wanted() || g_fxs.dlss_reactive_moving <= 0.0f || !g_rt || g_rt_face || g_rt_level || g_mout || g_nout ||
        g_rt != g_dlss.scene || !g_dlss.res[5] || d->vs.rhw || d->overlay || d->screen_fx || d->water || d->fs.water ||
        !d->count || d->pipe.blend || !d->depth.zenable || !d->depth.zwrite)
        return;
    const GfxElem* pe = &d->vs.el[GFX_R_POSITION];
    if (!pe->used || pe->stream >= GFX_NSTREAMS)
        return;
    const GfxBuf* b = d->buf[pe->stream];
    const int built = !b || b->upload_frame + 1 >= g_serial; /* its vertices made again this frame: a character, a swaying tree */
    const int prog = d->vs.prog != 0 && d->vs_tokens != NULL;   /* a vertex shader's: the plants, VanaCore's grass */
    /* a character mo_draw gave its own motion: its history is fetched from where it was, and leaning on this frame's jittered
     * picture there instead only made it flicker (dlss_motion_chars) */
    if (built && !prog && d->anchor[3] == 0.0f && g_mo_own_serial == g_serial && g_mo_own == d)
        return;
    if (!built && !(prog && alpha_tested(&d->fs)))
    {
        /* placed by a world matrix, from a buffer the game keeps: moving only if it stood elsewhere the frame before. With the
         * moving things' own motion asked for (dlss_motion), mo_draw has just judged that (g_mo_moved); else by its anchor
         * against the frame before's (mask_moved: with frame generation alone mo_draw keeps only the effects' models, under
         * keys of their own, so the tables are shared without a clash) */
        if (d->anchor[3] == 0.0f)
            return;
        if (g_fxs.dlss_motion != 0.0f && mo_wanted() ? !(g_mo_moved_serial == g_serial && g_mo_moved == d) : !mask_moved(d))
            return;
    }
    if (!mask_begin())
        return;
    g_mout = 1, g_maskout = 1, g_mo_key = GFX_MO_MASK, g_mo_slot = -1, g_mo_va = 0;
    memset(g_mo_f, 0, sizeof g_mo_f);
    if (!mo_keep(d))
    {
        g_targets_bound = 0;
        draw_encode(d);
        g_targets_bound = 0;
    }
    g_mout = 0, g_maskout = 0;
    g_prof.redraws++;
}

static void fg_release(void);

/* The helper and everything shared with it let go, for another to be started (dlss_helper_ready): it's ended, or it's
 * told to end (quit; it serves the other upscaler) - ended for it if it hasn't within a second. A frame of the game's
 * queue still waiting on it (DLSS's or frame generation's) is let through first: the queue is waited on below. */
static void dlss_let_go(void)
{
    wd_stop(); /* before the fence and the process handle go */
    br_stop();
    helpers_let_through(1); /* ended or not: one that crashed signals nothing */
    if (g_dlss.sh && g_dlss.proc && WaitForSingleObject(g_dlss.proc, 0) != WAIT_OBJECT_0)
    {
        g_dlss.sh->quit = 1;
        SetEvent(g_dlss.go);
        if (WaitForSingleObject(g_dlss.proc, 1000) != WAIT_OBJECT_0)
            TerminateProcess(g_dlss.proc, 0);
        fprintf(stderr, "[recomp] gfx: the %s helper let go\n", g_dlss.started == VANA_UPSCALER_FSR ? "FSR" : "DLSS");
    }
    g_dlss.last = 0;
    submit(1);
    dlss_release();
    fg_release();
    if (g_dlss.sh)
        UnmapViewOfFile(g_dlss.sh), g_dlss.sh = NULL;
    HANDLE* handles[] = { &g_dlss.map, &g_dlss.go, &g_dlss.ready, &g_dlss.go_fg, &g_dlss.done, &g_dlss.done_fg, &g_dlss.cpu_ev,
                          &g_dlss.proc };
    for (size_t i = 0; i < sizeof handles / sizeof handles[0]; ++i)
        if (*handles[i])
            CloseHandle(*handles[i]), *handles[i] = NULL;
    if (g_dlss.fence)
        defer((IUnknown*)g_dlss.fence, NULL, -1), g_dlss.fence = NULL;
    g_dlss.value = 0, g_dlss.in_w = g_dlss.in_h = g_dlss.out_w = g_dlss.out_h = 0;
    g_dlss.ran_ns = 0;
}

/* DLSS (or FSR) turned on again after it gave up: tried again - with the helper as it is, or started again if it ended or
 * was lost (helper_lost: one that stopped answering may still be stuck, and its fence is left behind with it)
 * (dlss_helper_ready lets go of one started for the other upscaler) */
static void dlss_retry(void)
{
    g_dlss.failed = 0, g_dlss.said = 0, g_dlss.why[0] = 0;
    g_dlss.fsr1_failed = 0, g_dlss.fsr1_said = 0;
    if (g_dlss.proc && (g_dlss.lost || WaitForSingleObject(g_dlss.proc, 0) == WAIT_OBJECT_0))
        dlss_let_go();
    else
        helpers_let_through(1);
    g_dlss.lost = 0;
    fprintf(stderr, "[recomp] gfx: %s: trying again\n", upscaler_name());
}

void gfx_dlss_set(int quality, const wchar_t* helper, const wchar_t* log)
{
    if (quality != g_dlss.quality)
    {
        if (quality < 0)
            fprintf(stderr, "[recomp] gfx: %s off\n", upscaler_name());
        else
            fprintf(stderr, "[recomp] gfx: %s on, quality %d\n", upscaler_name(), quality);
    }
    const int was = g_dlss.quality;
    g_dlss.quality = quality < 0 || quality > 5 ? -1 : quality;
    /* tried again when turned on, or at a new quality - but for AMD's FSR that isn't here (3: its DLLs, this card), FSR 1
     * standing in for it, which goes on at the new one */
    if (g_dlss.quality >= 0 && g_dlss.quality != was && (g_dlss.failed || g_dlss.fsr1_failed) &&
        !(was >= 0 && g_dlss.said == 3 && fsr1_wanted()))
        dlss_retry();
    if (helper)
        wcsncpy(g_dlss.helper, helper, MAX_PATH - 1);
    if (log)
        wcsncpy(g_dlss.log, log, MAX_PATH - 1);
}

int gfx_dlss_jitter(float ndc[2])
{
    if (g_dlss.quality < 0 || g_dlss.failed || fsr1_wanted() || !g_dlss.sh || g_dlss.sh->state != VANA_DLSS_READY || !g_dlss.in_w ||
        g_fxs.dlss_compare != 0.0f || g_dlss.sh->made != g_dlss.sh->generation)
        return 0;
    if (g_dlss.jit_serial != g_serial)
    {
        /* Halton (2, 3), as many steps as DLSS wants: 8 times the area scale; FSR's the same, of the scale it's drawn at
         * (ceil(8 * n^2), n the output's width over the render's) */
        float k = dlss_factor(g_dlss.quality);
        uint32_t phases = (uint32_t)(8.0f / (k * k) + 0.5f);
        if (g_dlss.upscaler == VANA_UPSCALER_FSR && g_dlss.out_w)
        {
            k = (float)g_dlss.in_w / (float)g_dlss.out_w;
            phases = (uint32_t)ceilf(8.0f / (k * k) - 0.001f);
        }
        uint32_t i = g_dlss.n++ % (phases < 8 ? 8 : phases) + 1;
        g_dlss.jit[0] = halton(i, 2) - 0.5f, g_dlss.jit[1] = halton(i, 3) - 0.5f;
        g_dlss.jit_serial = g_serial;
    }
    ndc[0] = 2.0f * g_dlss.jit[0] / (float)g_dlss.in_w;
    ndc[1] = -2.0f * g_dlss.jit[1] / (float)g_dlss.in_h;
    return 1;
}

int gfx_dlss_state(char* message, int size)
{
    /* starting: the helper starting, or making DLSS for new textures, or none of its frames back yet; FSR 1 till it's run */
    const int fsr1 = fsr1_wanted();
    int st = g_dlss.quality < 0 ? 0 : fsr1 ? (g_dlss.fsr1 && g_dlss.out ? 2 : 1) : dlss_gave_up() ? (g_dlss.said == 3 ? 3 : 4)
        : !g_dlss.sh || g_dlss.sh->state == VANA_DLSS_STARTING || g_dlss.sh->done == 0 || g_dlss.sh->made != g_dlss.sh->generation ? 1
        : 2;
    if (message && size > 0)
    {
        if (st == 2 && g_fxs.dlss_compare != 0.0f)
            snprintf(message, size, "comparing: %ux%u stretched without %s", g_dlss.in_w, g_dlss.in_h, upscaler_name());
        else if (st == 2 && fsr1)
            snprintf(message, size, "Game %ux%u -> FSR 1 %ux%u", g_dlss.in_w, g_dlss.in_h, g_dlss.out_w, g_dlss.out_h);
        else if (st == 1 && fsr1)
            snprintf(message, size, "%s", "FSR 1");
        else if (st == 2 && g_dlss.upscaler == VANA_UPSCALER_FSR) /* the version it runs: FSR 4.1.1, FSR 3.1.5 */
            snprintf(message, size, "Game %ux%u -> %s %ux%u", g_dlss.in_w, g_dlss.in_h,
                g_dlss.sh->sr_version[0] ? g_dlss.sh->sr_version : "FSR", g_dlss.out_w, g_dlss.out_h);
        else if (st == 2 && g_dlss.nr && g_dlss.sh->nr_w) /* the size DLSS 5 runs at (dlss5_scale), as the helper made it */
            snprintf(message, size, "Game %ux%u -> DLSS 5 %ux%u -> DLSS %ux%u", g_dlss.in_w, g_dlss.in_h, g_dlss.sh->nr_w,
                g_dlss.sh->nr_h, g_dlss.out_w, g_dlss.out_h);
        else if (st == 2 && g_dlss.nr)
            snprintf(message, size, "Game %ux%u -> DLSS 5 -> DLSS %ux%u", g_dlss.in_w, g_dlss.in_h, g_dlss.out_w, g_dlss.out_h);
        else if (st == 2 && g_fxs.dlss5 != 0.0f) /* asked for, and not there: why */
            snprintf(message, size, "Game %ux%u -> DLSS %ux%u (DLSS 5: %s)", g_dlss.in_w, g_dlss.in_h, g_dlss.out_w, g_dlss.out_h,
                !dlss_card(1)                ? "needs an NVIDIA RTX 40 series card or later"
                : g_dlss.sh->nr_message[0] ? g_dlss.sh->nr_message
                                             : "not available");
        else if (st == 2)
            snprintf(message, size, "Game %ux%u -> DLSS %ux%u", g_dlss.in_w, g_dlss.in_h, g_dlss.out_w, g_dlss.out_h);
        else
            snprintf(message, size, "%s", g_dlss.why[0] ? g_dlss.why : g_dlss.sh ? g_dlss.sh->message : "");
    }
    return st;
}

#ifdef VANACORE
int vanacore_gfx_dlss_times(VanaGfxDlssTimes* out)
{
    const VanaDlssShared* sh = g_dlss.sh;
    if (!sh || !sh->gpu_timed || g_dlss.quality < 0 || g_dlss.failed)
        return 0;
    out->nr_ms = g_dlss.nr ? sh->gpu_nr_ms : 0.0f;
    out->sr_ms = sh->gpu_sr_ms;
    out->fg_ms = sh->gpu_fg_ms;
    return 1;
}

int vanacore_gfx_dlss_supported(char* why, int size)
{
    const char* text = gfx_selected_d3d11() || !g_dev ? "needs DirectX 12"
                       : !dlss_card(0)                ? "needs an NVIDIA RTX 20 series card or later"
                                                      : "";
    if (why && size > 0)
        snprintf(why, size, "%s", text);
    return text[0] == 0;
}

int vanacore_gfx_dlss5_supported(char* why, int size)
{
    char text[200] = "";
    if (gfx_selected_d3d11() || !g_dev)
        snprintf(text, sizeof text, "needs DirectX 12");
    else if (!dlss_card(1))
        snprintf(text, sizeof text, "needs an NVIDIA RTX 40 series card or later");
    else
    {
        /* NVIDIA's DLL beside the helper (VanaCore doesn't ship it); then, once the helper is up, NGX's own answer */
        wchar_t dll[MAX_PATH];
        const wchar_t* slash = wcsrchr(g_dlss.helper, L'\\');
        const size_t dir = slash ? (size_t)(slash - g_dlss.helper) + 1 : 0;
        swprintf(dll, MAX_PATH, L"%.*lsnvngx_dlssnr.dll", (int)dir, g_dlss.helper);
        const VanaDlssShared* sh = g_dlss.sh;
        if (!dir || GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
            snprintf(text, sizeof text, "needs NVIDIA's nvngx_dlssnr.dll in the dlss folder");
        else if (sh && sh->state == VANA_DLSS_READY && !sh->nr_available)
            snprintf(text, sizeof text, "%s", sh->nr_message[0] ? sh->nr_message : "NVIDIA's NGX doesn't give it here");
    }
    if (why && size > 0)
        snprintf(why, size, "%s", text);
    return text[0] == 0;
}

void vanacore_gfx_set_upscaler(int upscaler, float sharpness, int version)
{
    upscaler = upscaler == VANA_UPSCALER_FSR ? VANA_UPSCALER_FSR : VANA_UPSCALER_DLSS;
    if (upscaler != g_dlss.upscaler)
    {
        g_dlss.upscaler = upscaler;
        fprintf(stderr, "[recomp] gfx: the upscaler: %s\n", upscaler == VANA_UPSCALER_FSR ? "AMD FSR" : "NVIDIA DLSS");
        /* the other one tried afresh: what gave up was this one's (its helper's let go when it's next needed) */
        if (g_dlss.failed || g_dlss.fsr1_failed)
            dlss_retry();
        g_dlss.said = 0, g_dlss.why[0] = 0;
    }
    g_dlss.sharpness = sharpness < 0.0f ? 0.0f : sharpness > 1.0f ? 1.0f : sharpness;
    version = version >= 1 && version <= 4 ? version : 0; /* 1: FSR 1 (fsr1_run) */
    if (version != g_dlss.version_want)
        g_dlss.fsr1_said = 0;
    g_dlss.version_want = version;
    gfx11_upscaler(upscaler, sharpness, version); /* Direct3D 11's FSR, the same */
}

int vanacore_gfx_fsr_supported(char* why, int size)
{
    if (gfx_selected_d3d11())
        return gfx11_fsr_supported(why, size); /* FSR on Direct3D 11 (gfx_d3d11.c) */
    const VanaDlssShared* sh = g_dlss.sh;
    /* FSR 1 runs where AMD's FSR doesn't (fsr1_wanted): only without its passes is it AMD's FSR or nothing */
    const LONG fx = InterlockedCompareExchange(&g_fx.state, 0, 0);
    const int fsr1 = fx != 3 && (fx != 2 || (g_fx.easu && g_fx.rcas));
    const char* text = gfx_selected_d3d11() || !g_dev ? "needs DirectX 12"
                       : fsr1                       ? ""
                       : !fsr_dlls_here()           ? "needs AMD's FSR DLLs in the dlss folder"
                       /* the helper started for FSR, and AMD's FSR has nothing for this card */
                       : sh && g_dlss.started == VANA_UPSCALER_FSR && sh->state == VANA_DLSS_UNAVAILABLE
                           ? (sh->message[0] ? sh->message : "AMD's FSR doesn't run on this card")
                           : "";
    if (why && size > 0)
        snprintf(why, size, "%s", text);
    return text[0] == 0;
}
#endif

/* The mip bias, in sixteenths, for the world's textures drawn into the scene DLSS makes larger: log2 of the size it's
 * drawn at over the size it's shown at (-16 at half each way), so they're as sharp as at the shown size. 0 without
 * DLSS, at DLAA, and on other targets. */
static int8_t dlss_lod_bias(void)
{
    if (g_dlss.quality < 0 || dlss_gave_up() || !g_dlss.in_w || !g_dlss.out_w || !g_rt || g_rt != g_dlss.scene ||
        g_fxs.dlss_compare != 0.0f)
        return 0;
    float b = log2f((float)g_dlss.in_w / (float)g_dlss.out_w) * 16.0f;
    return (int8_t)(b > 0.0f ? 0 : b < -32.0f ? -32 : (int)(b - 0.5f));
}

static int g_fg_hudless_draw; /* fg_hudless_clean's draw: the scene read as DLSS made it, without the interface over it */
static GfxTex* g_fgn_sample;  /* fgn_frame's draw laying the names' layer over a made frame: the layer, read in the scene's place */

/* the scene's target, read by this frame's draws once DLSS has made it larger: its output (as DLSS made it, for
 * frame generation's hudless: g_fg_hudless_draw; the names' layer for its frames: g_fgn_sample) */
static GfxTex* dlss_sampled(GfxTex* t)
{
    if (g_fgn_sample && t && t == g_dlss.scene)
        return g_fgn_sample;
    if (!t || t != g_dlss.scene || g_dlss.out_serial != g_serial || !g_dlss.out)
        return t;
    return g_fg_hudless_draw && g_dlss.clean && g_dlss.clean_serial == g_serial ? g_dlss.clean : g_dlss.out;
}

/* --- DLSS frame generation (VanaCore) ----------------------------------------------------------------------
 * Through the DLSS helper too (dlss_ipc.h), at the frame's end: the finished frame, the scene alone before the
 * game's interface went over it (hudless: the frame just after the draw that laid the scene onto it), and DLSS's
 * depth and motion (this frame's) make `use` frames between the frame before and this one. Each of those, then this
 * frame, gets VanaCore's interface over it (shown[]), and a thread of its own puts them on the screen evenly spaced
 * over the time a frame takes the game - the made ones first, the real one last (held back that long: frame
 * generation's latency) - through the present queue (show_present), so they don't wait behind the game's next frame
 * on its queue. Needs DLSS or FSR running ([graphics] dlss or fsr, any mode). NVIDIA's (DLSS-FG, with DLSS) or
 * AMD's (FSR frame generation, with either: one frame between two; g_fg.kind), the same way: the helper makes them. */
enum { FG_MAX = 3 };
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static struct
{
    int count;              /* frames made between two ([graphics] dlss.framegen): 0 off, 1 (2x) .. 3 (4x) */
    int kind;               /* VANA_FG_*: NVIDIA's (DLSS's) or AMD's (vanacore_gfx_set_framegen_kind) */
    int made_kind;          /* ... the one the helper was asked to make (with the frame's textures) */
    int use;                /* ... as many as this card makes (the helper's fg_count_max: 1 before RTX 50) */
    int failed, unavailable;
    int starting;           /* the helper making frame generation for new textures */
    char why[160];
    ID3D12Fence* fence;     /* shared with the helper */
    uint64_t value, last;   /* the fence's last value; the last frame handed over (0: none waiting), and when */
    ULONGLONG last_at;
    ID3D12Resource *real, *hudless, *out[FG_MAX]; /* shared: the frame, the scene alone, the frames made */
    int32_t out_srv[FG_MAX];
    int views;
    uint32_t w, h, in_w, in_h, made_use;
    DXGI_FORMAT fmt;
    uint64_t hudless_serial; /* the frame `hudless` holds */
    float prev_view[16], prev_proj[16];
    uint64_t prev_serial, on_serial;
    FxTex shown[2][FG_MAX + 1]; /* the frames as shown, two sets: the thread shows one while the next is drawn */
    /* the present thread and its jobs: posted by the game's thread, done by it, in turn (set: sequence & 1) */
    HANDLE thread, wake, idle, pwake, timer;
    volatile LONG posted, done;
    volatile LONG quit;     /* the thread asked to end (fg_stop_thread) */
    struct
    {
        int frames;         /* the made ones, then the real one */
        uint64_t ready;     /* the queue's fence value they're drawn at */
        uint64_t copied;    /* g_pfence's value once the present queue has copied the last of them: the set's free then */
        double spacing;     /* seconds between them */
    } job[2];
    ID3D12CommandAllocator* alloc[FG_MAX + 1]; /* the copies' (show_present), one for each frame of a job */
    uint64_t alloc_done[FG_MAX + 1];
    double interval;        /* seconds between the game's frames, smoothed */
    uint64_t last_ns;
    int noted_fps;          /* the cap the display was logged for (fg_note_display): 0 to log it again */
} g_fg;
static volatile LONG g_fps_max = 60; /* the game's frame rate cap ([client] fps.max: vanacore_gfx_set_fps_max) */

/* whether frame generation is asked for and hasn't failed (mo_draw: the effects' motion is left out for it) */
static int fg_wanted(void)
{
    return g_fg.count > 0 && !g_fg.failed;
}

/* the frame generation asked for, by name: the log's */
static const char* fg_name(void)
{
    return g_fg.kind == VANA_FG_FSR ? "FSR frame generation" : "DLSS frame generation";
}

static void fg_fail(int unavailable, const char* why)
{
    g_fg.failed = 1, g_fg.unavailable = unavailable;
    snprintf(g_fg.why, sizeof g_fg.why, "%s", why);
    fprintf(stderr, "[recomp] gfx: %s: %s; off\n", fg_name(), why);
}

/* --- the helper lost ---------------------------------------------------------------------------------------------
 * The game's queue waits on the helper for each frame it hands over (DLSS's fence at last + 1, frame generation's at
 * its own), and only the helper signals them: one that crashed or stopped answering would leave the queue, and the game
 * waiting on it, stopped for good (Windows doesn't time out a queue that waits). Let through from here, what the helper
 * ran turned off till it's turned on again, and the player told once (core/graphics.cpp, vanacore_gfx_helper_lost). */
#define HELPER_STUCK_MS 3000 /* the game held on its queue (wait_fence) this long by a frame the helper hasn't taken */
static int g_helper_lost;           /* times lost */
static char g_helper_lost_why[320]; /* ... the last time's words, for the player */

/* One wait let through, once the game's own signal (last) is: one of ours before it would be undone by it (the queue's
 * Signal sets the value it's given) and leave the wait on last + 1 for good. 1: nothing waits on it now */
static int helper_through(ID3D12Fence* fence, uint64_t* last)
{
    if (!*last || !fence)
        return 1;
    if (ID3D12Fence_GetCompletedValue(fence) < *last)
        return 0;
    ID3D12Fence_Signal(fence, *last + 1);
    *last = 0;
    return 1;
}

/* Frame generation's wait let through, and DLSS's (dlss_too), each once the queue has reached it, in whichever order
 * the queue holds them; after 2 seconds (the queue stopped short of them) regardless */
static void helpers_let_through(int dlss_too)
{
    const ULONGLONG until = GetTickCount64() + 2000;
    for (;;)
    {
        const int fg = helper_through(g_fg.fence, &g_fg.last);
        const int dlss = !dlss_too || helper_through(g_dlss.fence, &g_dlss.last);
        if (fg && dlss)
            return;
        if (GetTickCount64() >= until)
            break;
        Sleep(1);
    }
    if (g_fg.last && g_fg.fence)
        ID3D12Fence_Signal(g_fg.fence, g_fg.last + 1);
    g_fg.last = 0;
    if (dlss_too && g_dlss.last && g_dlss.fence)
        ID3D12Fence_Signal(g_dlss.fence, g_dlss.last + 1);
    if (dlss_too)
        g_dlss.last = 0;
    fprintf(stderr, "[recomp] gfx: the queue didn't reach the helper's frame within 2 s; let through regardless\n");
}

/* how the helper ended, by its exit code: a crash's exception named, or the code */
static const char* helper_exit_text(char* out, size_t size)
{
    DWORD code = 0;
    if (!g_dlss.proc || !GetExitCodeProcess(g_dlss.proc, &code) || code == STILL_ACTIVE)
    {
        snprintf(out, size, "the helper ended");
        return out;
    }
    const char* what = code == 0xC0000005u ? "an access violation"
                       : code == 0xC0000409u ? "a fail-fast or stack buffer overrun"
                       : code == 0xC0000374u ? "heap corruption"
                       : code == 0xC00000FDu ? "a stack overflow"
                       : code == 0xC000001Du ? "an illegal instruction"
                       : code == 0xC0000135u ? "a DLL it needs is missing"
                       : code == 0xC0000142u ? "a DLL failed to start"
                       : code == 0xE06D7363u ? "an unhandled C++ exception"
                                             : NULL;
    if (what)
        snprintf(out, size, "the helper crashed (0x%08lX, %s)", (unsigned long)code, what);
    else if (code >= 0xC0000000u)
        snprintf(out, size, "the helper crashed (0x%08lX)", (unsigned long)code);
    else
        snprintf(out, size, "the helper ended (exit code %lu%s)", (unsigned long)code, code == 2 ? ", it couldn't use what the game shares with it: usually a dlss\\VanaCore.Dlss.exe from another update than VanaCore.Core.dll, see logs\\dlss.log" : "");
    return out;
}

/* The helper crashed, ended unasked or stopped answering (why; NULL: how it ended, from what it said or its exit code):
 * the queue let through, DLSS (or FSR) and frame generation off till they're turned on again (dlss_retry starts another
 * helper then), and the player told once */
static void helper_lost(const char* why)
{
    if (g_dlss.lost)
    {
        helpers_let_through(1);
        return;
    }
    char ended[256];
    const VanaDlssShared* sh = g_dlss.sh;
    if (!why)
    {
        if (sh && (sh->state == VANA_DLSS_FAILED || sh->state == VANA_DLSS_UNAVAILABLE) && sh->message[0])
        {
            snprintf(ended, sizeof ended, "%s", sh->message);
            why = ended;
        }
        else
            why = helper_exit_text(ended, sizeof ended);
    }
    /* ... and what it was doing then, from the shared memory (the helper's doing: there whether or not it lived to write its
     * crash report into vanacore.log, which comes just before this) */
    char full[400];
    if (sh && sh->doing[0])
    {
        char was[sizeof sh->doing + 1];
        memcpy(was, sh->doing, sizeof sh->doing), was[sizeof sh->doing] = 0;
        fprintf(stderr, "[recomp] gfx: %s: the helper was %s (the game's frame %llu) when it was lost; its crash report, if it "
            "wrote one, is just before this (and logs\\dlss-crash-*.dmp)\n", upscaler_name(), was,
            (unsigned long long)sh->doing_frame);
        snprintf(full, sizeof full, "%s while %s", why, was);
        why = full;
    }
    helpers_let_through(1);
    const int fg_was = g_fg.count > 0 && !g_fg.failed;
    char off[300];
    snprintf(off, sizeof off, "%s; off", why);
    dlss_say(4, off);
    g_dlss.failed = 1, g_dlss.lost = 1;
    if (fg_was)
        fg_fail(0, why);
    snprintf(g_helper_lost_why, sizeof g_helper_lost_why, "%s%s stopped: %s%s", upscaler_name(),
        fg_was ? " and frame generation" : "", why, fsr1_wanted() ? "; FSR 1 runs in its place" : "");
    g_helper_lost++;
}

/* wait_fence's, between its steps: a frame handed to a helper that's ended, or that it hasn't taken in HELPER_STUCK_MS
 * (it's stuck, and the game with it), let through and the helper given up */
static void helpers_check(void)
{
    const VanaDlssShared* sh = g_dlss.sh;
    if (!sh || (!g_dlss.last && !g_fg.last))
        return;
    const ULONGLONG now = GetTickCount64();
    if (g_dlss.proc && WaitForSingleObject(g_dlss.proc, 0) == WAIT_OBJECT_0)
        helper_lost(NULL);
    else if ((g_dlss.last && sh->done < g_dlss.last && now - g_dlss.last_at >= HELPER_STUCK_MS) ||
             (g_fg.last && sh->fg_done < g_fg.last && now - g_fg.last_at >= HELPER_STUCK_MS))
        helper_lost("the helper stopped answering");
}

/* --- the watchdog ------------------------------------------------------------------------------------------------
 * The checks above run on the game's thread and judge the helper by its `done`, the frame it took. They don't see the
 * game's queue waiting on a fence the helper never signals while `done` moves on: the helper's own queue waiting on the
 * game's signal, which never reaches it (a fence shared between two processes that doesn't carry its value across, as
 * vkd3d-proton under Proton may do with the FSR 3.1 helper), the two queues waiting on each other. Nor are they reached
 * when the game's thread is stuck in a call the wait holds up (Present, behind the queue). This thread looks every
 * 250 ms: a fence sat at the game's own signal for WATCHDOG_STUCK_MS, the helper's not come, and the wait is let
 * through (the fence signalled from here), the line written from here, and the game's thread told (g_wd_trip) to give
 * the helper up, as helper_lost does, at its next frame (fg_watch). */
#define WATCHDOG_MS 250
#define WATCHDOG_STUCK_MS 3000
static HANDLE g_wd_thread, g_wd_event;
static volatile LONG g_wd_trip;
static char g_wd_why[160];

typedef struct
{
    uint64_t seen;   /* the game's signal the fence was last found at (0: not at one) */
    ULONGLONG since; /* ... and since when */
} WdSeen;

/* a 64-bit value the game's thread writes, read whole on x86 */
static uint64_t wd_load(volatile uint64_t* p)
{
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)p, 0, 0);
}

/* A fence let through if it has sat at `last` (the game's own signal, the frame handed over) for WATCHDOG_STUCK_MS: the
 * queue reached it and the helper's last + 1 hasn't come. A fence short of `last` is the game's own queue behind, not
 * the helper (and a signal from here then would be undone by the queue's own); one past it, the helper answered. */
static void wd_check(const char* what, ID3D12Fence* fence, volatile uint64_t* last_p, uint64_t took, WdSeen* w, ULONGLONG now)
{
    const uint64_t last = wd_load(last_p);
    if (!fence || !last || ID3D12Fence_GetCompletedValue(fence) != last)
    {
        w->seen = 0;
        return;
    }
    if (w->seen != last)
    {
        w->seen = last, w->since = now;
        return;
    }
    if (now - w->since < WATCHDOG_STUCK_MS || wd_load(last_p) != last)
        return;
    const VanaDlssShared* sh = g_dlss.sh;
    char doing[sizeof sh->doing + 1] = "";
    if (sh)
        memcpy(doing, sh->doing, sizeof sh->doing), doing[sizeof sh->doing] = 0;
    fprintf(stderr, "[recomp] gfx: %s: the game's queue sat at its signal %llu for %llu ms, the helper's not come (the helper had "
        "taken frame %llu; it was %s); the wait let through from the watchdog thread\n", what, (unsigned long long)last,
        (unsigned long long)(now - w->since), (unsigned long long)took, doing[0] ? doing : "doing nothing it said");
    ID3D12Fence_Signal(fence, last + 1);
    snprintf(g_wd_why, sizeof g_wd_why, "the graphics card's wait on the helper's %s never ended", what);
    MemoryBarrier();
    InterlockedExchange(&g_wd_trip, 1);
    w->seen = 0;
}

static DWORD WINAPI wd_thread(LPVOID event)
{
    WdSeen dlss = { 0, 0 }, fg = { 0, 0 };
    while (WaitForSingleObject((HANDLE)event, WATCHDOG_MS) == WAIT_TIMEOUT)
    {
        VanaDlssShared* sh = g_dlss.sh;
        if (!sh || g_wd_trip)
            continue;
        const ULONGLONG now = GetTickCount64();
        wd_check(upscaler_name(), g_dlss.fence, &g_dlss.last, wd_load(&sh->done), &dlss, now);
        if (!g_wd_trip)
            wd_check("frame generation", g_fg.fence, &g_fg.last, wd_load(&sh->fg_done), &fg, now);
    }
    return 0;
}

static void wd_start(void)
{
    if (g_wd_thread)
        return;
    InterlockedExchange(&g_wd_trip, 0);
    g_wd_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wd_thread = g_wd_event ? CreateThread(NULL, 0, wd_thread, g_wd_event, 0, NULL) : NULL;
    if (!g_wd_thread)
    {
        fprintf(stderr, "[recomp] gfx: the helper's watchdog thread didn't start\n");
        if (g_wd_event)
            CloseHandle(g_wd_event), g_wd_event = NULL;
    }
}

/* ended before the fences and the helper's process handle go (dlss_let_go) */
static void wd_stop(void)
{
    if (g_wd_thread)
    {
        SetEvent(g_wd_event);
        if (WaitForSingleObject(g_wd_thread, 5000) == WAIT_OBJECT_0)
            CloseHandle(g_wd_thread), CloseHandle(g_wd_event);
        else
            fprintf(stderr, "[recomp] gfx: the helper's watchdog thread didn't end in 5 s\n"); /* (its handles left to it) */
        g_wd_thread = g_wd_event = NULL;
    }
    InterlockedExchange(&g_wd_trip, 0);
}

/* --- no fence shared with the helper ------------------------------------------------------------------------------
 * Wine and Proton can't open a D3D12 fence from another process: vkd3d-proton imports the shared one as a Vulkan
 * semaphore, Wine logs "d3d12 fence from other process" and vkImportSemaphoreWin32HandleKHR dies in an access violation
 * in the helper, which the game takes for a helper lost (FSR 1 in FSR 4's place). So under Wine the two sides keep a fence
 * each and the CPU does what the shared one did, a hand-over each way (dlss_ipc.h, cpu_sync):
 *  - to the helper: the game's thread waits for its queue to reach the frame (dlss_post), then sets ".go" (".fg");
 *  - back: the helper runs, waits for its own queue, sets `done` and then ".done" (".fgdone"); br_thread signals the game's
 *    fence to frame + 1, which the game's queue has been waiting on. Only for the frame still waited on (the watchdog and
 *    helpers_let_through may have let it through already: a late answer to a frame before it would let the next one by).
 * (gfx_dlss_cpu_sync_wanted: VANA_DLSS_FENCE=cpu tries it on Windows, =gpu keeps the shared fence under Wine.) */
static HANDLE g_br_thread, g_br_stop;
static CRITICAL_SECTION g_br_cs; /* the fences the thread signals, not released under it */
static int g_br_cs_ready;

/* the game's thread: the helper let in once the game's queue has reached `frame` (the fence is the game's own, signalled
 * by its queue: this is a wait for it on the CPU); after 2 seconds (the queue stuck, or the helper gone), regardless */
static void dlss_post(HANDLE go, ID3D12Fence* fence, uint64_t frame)
{
    if (g_dlss.cpu_sync && g_dlss.cpu_ev && fence && ID3D12Fence_GetCompletedValue(fence) < frame &&
        SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, frame, g_dlss.cpu_ev)))
    {
        const ULONGLONG until = GetTickCount64() + 2000;
        /* (the value checked each time round: an event left set by a wait given up on before is not the queue's arrival) */
        while (ID3D12Fence_GetCompletedValue(fence) < frame)
        {
            WaitForSingleObject(g_dlss.cpu_ev, 20);
            if (GetTickCount64() >= until || (g_dlss.proc && WaitForSingleObject(g_dlss.proc, 0) == WAIT_OBJECT_0))
                break;
        }
    }
    SetEvent(go);
}

static void br_through(ID3D12Fence* fence, uint64_t* last_p, uint64_t took)
{
    const uint64_t last = wd_load(last_p);
    if (fence && last && took == last)
        ID3D12Fence_Signal(fence, last + 1);
}

static DWORD WINAPI br_thread(LPVOID unused)
{
    (void)unused;
    HANDLE waits[3] = { g_dlss.done, g_dlss.done_fg, g_br_stop };
    for (;;)
    {
        const DWORD r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0 && r != WAIT_OBJECT_0 + 1)
            return 0;
        VanaDlssShared* sh = g_dlss.sh;
        EnterCriticalSection(&g_br_cs);
        if (sh)
        {
            if (r == WAIT_OBJECT_0)
                br_through(g_dlss.fence, &g_dlss.last, wd_load(&sh->done));
            else
                br_through(g_fg.fence, &g_fg.last, wd_load(&sh->fg_done));
        }
        LeaveCriticalSection(&g_br_cs);
    }
}

static void br_start(void)
{
    if (g_br_thread || !g_dlss.done || !g_dlss.done_fg)
        return;
    if (!g_br_cs_ready)
        InitializeCriticalSection(&g_br_cs), g_br_cs_ready = 1;
    g_br_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_br_thread = g_br_stop ? CreateThread(NULL, 0, br_thread, NULL, 0, NULL) : NULL;
    if (!g_br_thread)
    {
        fprintf(stderr, "[recomp] gfx: the helper's answer thread didn't start (the game's queue would wait on it for good)\n");
        if (g_br_stop)
            CloseHandle(g_br_stop), g_br_stop = NULL;
    }
}

/* ended before the fences and the helper's events go (dlss_let_go) */
static void br_stop(void)
{
    if (!g_br_thread)
        return;
    SetEvent(g_br_stop);
    if (WaitForSingleObject(g_br_thread, 5000) == WAIT_OBJECT_0)
        CloseHandle(g_br_thread), CloseHandle(g_br_stop);
    else
        fprintf(stderr, "[recomp] gfx: the helper's answer thread didn't end in 5 s\n"); /* (its handles left to it) */
    g_br_thread = g_br_stop = NULL;
}

/* the game's thread's part, each frame: the helper given up as helper_lost does, where the watchdog let a wait through */
static void wd_take(void)
{
    if (!g_wd_trip)
        return;
    InterlockedExchange(&g_wd_trip, 0);
    if (g_dlss.proc && WaitForSingleObject(g_dlss.proc, 0) == WAIT_OBJECT_0)
        helper_lost(NULL);
    else
        helper_lost(g_wd_why);
}

/* The debug window's DLSS crash tests: the helper fails as kind says (VANA_CRASH_TEST_*) at its next frame's upscale. 1 asked;
 * 0, and why, with no helper running here to ask (DLSS and FSR off, not started yet, given up; Direct3D 11) */
int vanacore_gfx_dlss_crash_test(int kind, char* why, int size)
{
    const char* no = NULL;
    if (!g_dev)
        no = "needs DirectX 12";
    else if (!g_dlss.sh || g_dlss.failed)
        no = "needs DLSS or FSR running";
    else if (g_dlss.sh->state != VANA_DLSS_READY)
        no = "the helper isn't ready yet";
    if (why && size > 0)
        snprintf(why, size, "%s", no ? no : "");
    if (no)
        return 0;
    fprintf(stderr, "[recomp] gfx: %s: crash test %d asked for (/vana debug): the helper fails at its next frame\n", upscaler_name(),
        kind);
    g_dlss.sh->crash_test = kind;
    return 1;
}

int vanacore_gfx_helper_lost(char* why, int size)
{
    if (why && size > 0)
        snprintf(why, size, "%s", g_helper_lost_why);
    return g_helper_lost;
}

static void fg_sleep_until(uint64_t t_ns)
{
    for (;;)
    {
        uint64_t now = gfx_now_ns();
        if (now >= t_ns)
            return;
        uint64_t left = t_ns - now;
        if (left > 1500000 && g_fg.timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left - 1000000) / 100);
            if (SetWaitableTimer(g_fg.timer, &due, 0, NULL, NULL, FALSE))
            {
                WaitForSingleObject(g_fg.timer, 100);
                continue;
            }
        }
        if (left > 1500000)
            Sleep(1);
        else
            YieldProcessor();
    }
}

/* The present thread: each job's frames from when the GPU has drawn them, the first at once, the rest `spacing`
 * apart, each onto the swap chain through the present queue (show_present); when the next job is already waiting (the
 * screen slower than the frames: vsync), straight to the real one */
static DWORD WINAPI fg_present_thread(void* arg)
{
    (void)arg;
    for (;;)
    {
        WaitForSingleObject(g_fg.wake, INFINITE);
        if (InterlockedCompareExchange(&g_fg.quit, 0, 0))
            return 0;
        while (g_fg.done < g_fg.posted)
        {
            const LONG seq = g_fg.done;
            const int set = seq & 1;
            MemoryBarrier();
            const int frames = g_fg.job[set].frames;
            const uint64_t spacing = (uint64_t)(g_fg.job[set].spacing * 1e9);
            if (ID3D12Fence_GetCompletedValue(g_fence) < g_fg.job[set].ready)
            {
                ID3D12Fence_SetEventOnCompletion(g_fence, g_fg.job[set].ready, g_fg.pwake);
                WaitForSingleObject(g_fg.pwake, 1000);
            }
            const uint64_t t0 = gfx_now_ns();
            for (int k = 0; k < frames; ++k)
            {
                const int behind = g_fg.posted > seq + 1;
                if (behind && k < frames - 1)
                    continue;
                if (k && !behind)
                    fg_sleep_until(t0 + spacing * (uint64_t)k);
                show_present(&g_fg.shown[set][k], g_fg.job[set].ready, g_fg.alloc[k], &g_fg.alloc_done[k],
                    "at Present (frame generation's)");
            }
            g_fg.job[set].copied = g_pvalue; /* the game's queue waits for this before it draws into the set again */
            MemoryBarrier();
            InterlockedIncrement(&g_fg.done);
            SetEvent(g_fg.idle);
        }
    }
    return 0;
}

/* the present thread done with every job: before the game's thread presents, or the swap chain's buffers change */
static void fg_wait_idle(void)
{
    while (g_fg.thread && g_fg.done < g_fg.posted)
        WaitForSingleObject(g_fg.idle, 100);
}

static int fg_start_thread(void)
{
    if (g_fg.thread)
        return 1;
    g_fg.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_fg.idle = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_fg.pwake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_fg.timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_fg.wake || !g_fg.idle || !g_fg.pwake)
        return 0;
    for (int k = 0; k <= FG_MAX; ++k)
        if (!g_fg.alloc[k] && FAILED(ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                  &IID_ID3D12CommandAllocator, (void**)&g_fg.alloc[k])))
            return g_fg.alloc[k] = NULL, 0;
    if (!show_init() || !g_pfence)
        return 0;
    g_fg.thread = CreateThread(NULL, 0, fg_present_thread, NULL, 0, NULL);
    if (!g_fg.thread)
        return 0;
    SetThreadPriority(g_fg.thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return 1;
}

static void fg_release(void)
{
    if (g_fg.real)
        defer((IUnknown*)g_fg.real, NULL, -1), g_fg.real = NULL;
    if (g_fg.hudless)
        defer((IUnknown*)g_fg.hudless, NULL, -1), g_fg.hudless = NULL;
    for (int k = 0; k < FG_MAX; ++k)
    {
        if (g_fg.out[k])
            defer((IUnknown*)g_fg.out[k], NULL, -1), g_fg.out[k] = NULL;
        if (g_fg.views && g_fg.out_srv[k] >= 0)
            defer(NULL, &g_srv, g_fg.out_srv[k]);
        g_fg.out_srv[k] = -1;
    }
    g_fg.views = 0, g_fg.w = g_fg.h = 0;
}

/* the present thread ended (its jobs shown, or dropped on a lost device), its events, timer and allocators let go
 * (gfx_shutdown): 1 when it has, 0 when it didn't end in 5 s */
static int fg_stop_thread(void)
{
    if (g_fg.thread)
    {
        fg_wait_idle();
        InterlockedExchange(&g_fg.quit, 1);
        SetEvent(g_fg.wake);
        if (WaitForSingleObject(g_fg.thread, 5000) != WAIT_OBJECT_0)
        {
            fprintf(stderr, "[recomp] gfx: frame generation's present thread didn't end in 5 s\n");
            InterlockedExchange(&g_fg.quit, 0);
            return 0;
        }
        CloseHandle(g_fg.thread), g_fg.thread = NULL;
        InterlockedExchange(&g_fg.quit, 0);
    }
    HANDLE* handles[] = { &g_fg.wake, &g_fg.idle, &g_fg.pwake, &g_fg.timer };
    for (size_t i = 0; i < sizeof handles / sizeof handles[0]; ++i)
        if (*handles[i])
            CloseHandle(*handles[i]), *handles[i] = NULL;
    for (int k = 0; k <= FG_MAX; ++k)
        if (g_fg.alloc[k])
            ID3D12CommandAllocator_Release(g_fg.alloc[k]), g_fg.alloc[k] = NULL;
    g_fg.posted = g_fg.done = 0;
    memset(g_fg.alloc_done, 0, sizeof g_fg.alloc_done);
    memset(g_fg.job, 0, sizeof g_fg.job);
    return 1;
}

/* the shared frame, hudless and outputs for a frame w x h of fmt, and frame generation made for them (the helper's,
 * at its next frame: fg_generation) */
static int fg_targets(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    fg_wait_idle();
    fg_release();
    if (!g_fg.fence && FAILED(ID3D12Device_CreateFence(g_dev, 0, g_dlss.cpu_sync ? D3D12_FENCE_FLAG_NONE : D3D12_FENCE_FLAG_SHARED,
            &IID_ID3D12Fence, (void**)&g_fg.fence)))
        return g_fg.fence = NULL, 0;
    for (int k = 0; k < FG_MAX; ++k)
        g_fg.out_srv[k] = -1;
    g_fg.views = 1;
    g_fg.real = dlss_texture(fmt, w, h, D3D12_RESOURCE_FLAG_NONE);
    g_fg.hudless = dlss_texture(fmt, w, h, D3D12_RESOURCE_FLAG_NONE);
    if (!g_fg.real || !g_fg.hudless)
        return 0;
    for (int k = 0; k < g_fg.use; ++k)
    {
        g_fg.out[k] = dlss_texture(fmt, w, h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!g_fg.out[k] || (g_fg.out_srv[k] = heap_alloc(&g_srv)) < 0)
            return 0;
        ID3D12Device_CreateShaderResourceView(g_dev, g_fg.out[k], NULL, heap_cpu(&g_srv, g_fg.out_srv[k]));
    }
    VanaDlssShared* sh = g_dlss.sh;
    sh->fg_backbuffer = dlss_share((ID3D12DeviceChild*)g_fg.real);
    sh->fg_hudless = dlss_share((ID3D12DeviceChild*)g_fg.hudless);
    for (int k = 0; k < FG_MAX; ++k)
        sh->fg_output[k] = k < g_fg.use ? dlss_share((ID3D12DeviceChild*)g_fg.out[k]) : 0;
    sh->fg_fence = g_dlss.cpu_sync ? 0 : dlss_share((ID3D12DeviceChild*)g_fg.fence);
    if (!sh->fg_backbuffer || !sh->fg_hudless || !sh->fg_output[0] || (!sh->fg_fence && !g_dlss.cpu_sync))
        return 0;
    sh->fg_w = w, sh->fg_h = h, sh->fg_format = (uint32_t)fmt, sh->fg_count = (uint32_t)g_fg.use;
    sh->fg_kind = g_fg.kind;
    sh->fg_generation++;
    g_fg.w = w, g_fg.h = h, g_fg.fmt = fmt, g_fg.made_use = (uint32_t)g_fg.use, g_fg.made_kind = g_fg.kind;
    g_fg.in_w = g_dlss.in_w, g_fg.in_h = g_dlss.in_h;
    g_fg.prev_serial = 0;
    g_fg.noted_fps = 0;
    fprintf(stderr, "[recomp] gfx: %s: %ux%u, %d frame(s) made between two\n", fg_name(), w, h, g_fg.use);
    return 1;
}

/* The display frame generation's frames go to, in the log once it's running (again when its textures are made anew or
 * fps.max changes): the refresh rate of the monitor the window is on, the window's mode, vsync and tearing, and the
 * frames shown a second (the game's cap x frames per game frame). When that's more than the display shows, a line of
 * its own: the present thread then waits at each Present and holds the game back. Log only. */
static void fg_note_display(void)
{
    const int fps = (int)g_fps_max, per = g_fg.use + 1, shown = fps * per;
    g_fg.noted_fps = fps;
    HWND hwnd = NULL;
    if (!g_swap || FAILED(IDXGISwapChain3_GetHwnd(g_swap, &hwnd)) || !hwnd)
        return;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    const int have_mon = mon && GetMonitorInfoW(mon, (MONITORINFO*)&mi);
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    const int hz = have_mon && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
        ? (int)dm.dmDisplayFrequency : 0; /* 0 or 1: the display's default, not known */
    char dev[64] = "";
    if (have_mon)
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev, sizeof dev, NULL, NULL);
    BOOL exclusive = FALSE;
    IDXGISwapChain3_GetFullscreenState(g_swap, &exclusive, NULL);
    RECT wr;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const int covers = have_mon && GetWindowRect(hwnd, &wr) && wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
        wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    const char* mode = exclusive ? "full screen (exclusive)"
        : covers && !(style & WS_CAPTION) ? "borderless full screen"
        : IsZoomed(hwnd) ? "windowed (maximised)" : "windowed";
    char rate[32];
    if (hz)
        snprintf(rate, sizeof rate, "%d Hz", hz);
    else
        snprintf(rate, sizeof rate, "an unknown refresh rate");
    fprintf(stderr, "[recomp] gfx: %s: display %s at %s, %s, vsync %s, tearing %s; %dx at %d FPS = %d frames a second shown\n",
        fg_name(), dev[0] ? dev : "(unknown)", rate, mode, g_vsync ? "on" : "off",
        !g_tearing ? "not supported" : g_vsync ? "supported (not used: vsync is on)" : "allowed", per, fps, shown);
    if (hz && shown > hz + 1) /* + 1: 119.88 Hz reads as 119 */
        fprintf(stderr, "[recomp] gfx: %s %dx at %d FPS = %d frames a second on a %d Hz display: the present thread will hold the "
            "game back; lower [client] fps.max or [graphics] dlss.framegen\n", fg_name(), per, fps, shown, hz);
}

/* the whole of src into dst (shared, left in COMMON) */
static void fg_copy(ID3D12Resource* dst, GfxTex* src)
{
    ID3D12GraphicsCommandList* l = list();
    tex_state(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to), memset(&from, 0, sizeof from);
    from.pResource = src->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = dst, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &from, NULL);
    barrier(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
}

/* With frame generation on, DLSS's (or FSR's) output just made: kept as it is (g_dlss.clean) before the interface held
 * for it is drawn over it (the names over heads, floating text, damage numbers), for frame generation's hudless
 * (fg_hudless_clean). Let go once frame generation is off. */
static void dlss_keep_clean(void)
{
    GfxTex* out = g_dlss.out;
    g_dlss.clean_serial = 0;
    if (!g_fg.count || g_fg.failed || !out)
    {
        if (g_dlss.clean && !g_fg.count)
            gfx_tex_destroy(g_dlss.clean), g_dlss.clean = NULL;
        if (!g_fg.count)
            fgn_release(); /* the names' copies for its frames too */
        return;
    }
    if (!g_dlss.clean || g_dlss.clean->w != out->w || g_dlss.clean->h != out->h || g_dlss.clean->fmt != out->fmt)
    {
        gfx_tex_destroy(g_dlss.clean);
        g_dlss.clean = gfx_tex_create(GFX_TEX_2D, out->fmt, out->w, out->h, 1, GFX_USE_RT);
        if (!g_dlss.clean)
            return;
    }
    ID3D12GraphicsCommandList* l = list();
    tex_state(out, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex_state(g_dlss.clean, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyResource(l, g_dlss.clean->res, out->res);
    tex_state(g_dlss.clean, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(out, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_dlss.clean_serial = g_serial;
}

/* the whole of src (shared, in COMMON, left so) into dst: fg_copy's way back */
static void fg_copy_back(GfxTex* dst, ID3D12Resource* src)
{
    ID3D12GraphicsCommandList* l = list();
    tex_state(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to), memset(&from, 0, sizeof from);
    from.pResource = src, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = dst->res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &from, NULL);
    barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

static void fgn_note_lay(const GfxDraw* d);

/* Before the draw that lays the scene (DLSS's output) onto the frame: that draw made first from the output as DLSS made
 * it (g_dlss.clean, without the interface held for DLSS over it), the frame then copied as frame generation's hudless,
 * and the frame put back as it was for the draw itself. Taken after the draw (fg_note_hudless, still the way when
 * there's no clean output), hudless had the names over heads in it: frame generation took them for the scene and moved
 * them with what's behind them, so in the frames it made they tore and doubled as the camera turned. Now they're the
 * interface to it, as the chat and menus are (and with nameplates.framegen, drawn onto its frames here: fgn_frame). */
static void fg_hudless_clean(const GfxDraw* d)
{
    GfxTex* frame = g_rt;
    if (!g_fg.count || g_fg.failed || !g_fg.hudless || !g_fg.real || g_fg.hudless_serial == g_serial || !g_dlss.clean ||
        g_dlss.clean_serial != g_serial || g_dlss.out_serial != g_serial || !frame || frame->type != GFX_TEX_2D ||
        g_rt_face || g_rt_level || frame->w != g_fg.w || frame->h != g_fg.h || frame->dxfmt != g_fg.fmt)
        return;
    const int part_was = gpu_part_cur();
    gpu_part(GP_FG_COPY);
    fg_copy(g_fg.real, frame); /* the frame before the draw, kept (fg_present copies the finished frame into it again) */
    g_targets_bound = 0;
    g_fg_hudless_draw = 1;
    draw_encode(d);
    g_fg_hudless_draw = 0;
    fg_copy(g_fg.hudless, frame);
    fg_copy_back(frame, g_fg.real);
    g_targets_bound = 0;
    g_fg.hudless_serial = g_serial;
    fgn_note_lay(d); /* for the names in the frames it makes: this draw, and the frame after it (fgn_note_named) */
    gpu_part(part_was);
}

/* After the draw that laid the scene (DLSS's output) onto the frame: the frame as it is then, without the game's
 * interface, is frame generation's hudless */
static void fg_note_hudless(void)
{
    g_fg_hudless_next = 0;
    if (!g_fg.count || g_fg.failed || !g_fg.hudless || g_fg.hudless_serial == g_serial || !g_rt || g_rt->type != GFX_TEX_2D ||
        g_rt_level || g_rt->w != g_fg.w || g_rt->h != g_fg.h || g_rt->dxfmt != g_fg.fmt)
        return;
    fg_copy(g_fg.hudless, g_rt);
    g_targets_bound = 0;
    g_fg.hudless_serial = g_serial;
}

/* Whether the helper has the frame generation asked for (NVIDIA's: fg_available; AMD's: fg_fsr_available), and why not */
static int fg_here(const VanaDlssShared* sh, const char** why)
{
    const int amd = g_fg.kind == VANA_FG_FSR;
    if (why)
        *why = amd ? sh->fg_fsr_message : sh->fg_message;
    return amd ? sh->fg_fsr_available : sh->fg_available;
}

/* A frame generation the game's queue still waits on, let through (the helper's let go: dlss_let_go) */
/* the helper's last frame generation: handed to its queue within half a second, or it's let go (the game's queue
 * waits on it): the fence signalled from here. The helper ended or not answering, everything it ran is off (helper_lost) */
static void fg_watch(void)
{
    VanaDlssShared* sh = g_dlss.sh;
    wd_take();
    if (!g_fg.last || !sh || sh->fg_done >= g_fg.last)
        return;
    if (g_dlss.proc && WaitForSingleObject(g_dlss.proc, 0) == WAIT_OBJECT_0)
    {
        helper_lost(NULL);
        return;
    }
    const char* why = NULL;
    /* a helper started for the other upscaler (let go next: dlss_helper_ready) isn't asked: FSR's says NVIDIA's frame
     * generation isn't there, and switched from FSR to DLSS it stayed off for good */
    const int here = g_dlss.started != g_dlss.upscaler || fg_here(sh, &why);
    int gone = !here || sh->state != VANA_DLSS_READY || g_dlss.failed;
    if (!gone && GetTickCount64() - g_fg.last_at < 500)
        return;
    if (!gone)
    {
        helper_lost("the helper stopped answering");
        return;
    }
    helpers_let_through(0);
    if (!g_fg.failed)
        fg_fail(!here, !here && why[0] ? why : "the helper stopped answering");
}

static void mat_identity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* --- The names over heads in frame generation's frames (VanaCore, [graphics] nameplates.framegen) ------------------
 * Frame generation makes its frames from the scene alone (hudless) and lays the frame's interface over them as the real
 * frame after them has it. The names over heads, drawn over DLSS's output with the rest of the interface held for it
 * (dlss_replay), were laid with it: in each frame it made, a name was where the real frame has it while the world under
 * it was only part of the way there, so the names moved at the game's frame rate, a step ahead of the world in the made
 * frames - the more, the faster the camera turned or rose and fell on a slope. So frame generation is handed the scene
 * alone for the frame as well (fg_present), and each frame it makes is finished here (fgn_frame):
 *   - copied (g_fgn.made);
 *   - the interface held for DLSS this frame (fgn_keep) drawn into a layer at DLSS's output size (g_fgn.layer,
 *     premultiplied: GfxPipeKey.over), each character's name (its draws carry the character: GfxDraw.label) moved back
 *     toward where it was the frame before - by how far the middle of its draws' box moved since then (fgn_boxes), times
 *     how far before the real frame the made one is - and tested against the scene's depth moved back the same way by
 *     the camera (fx_namez); what has no character (floating text, damage numbers) where the real frame has it;
 *   - the layer laid over it by the game's own draw that laid the scene onto the frame (g_fgn.lay, fgn_note_lay),
 *     reading the layer in the scene's place, blended over (ONE, 1 - alpha);
 *   - the real frame's interface over that: wherever the finished frame differs from the frame just after the scene was
 *     laid onto it with the names (g_fgn.named, fgn_note_named), the finished frame's pixel (fx_fgui), as frame
 *     generation had it.
 * The real frame is shown as the game finished it. A frame without a character's name, a draw laying the scene that
 * can't be kept, a texture let go, a pass that didn't compile: the frame goes through frame generation as before. */

typedef struct
{
    uint32_t label;
    float lo[2], hi[2]; /* its draws' box, as shares of their viewport (u.vp) across and down */
} FgnBox;

static struct
{
    Held* held;              /* the interface held for DLSS this frame (fgn_keep), and the bytes it points into */
    uint32_t nheld, cap_held;
    uint8_t* mem;
    size_t mem_cap;
    uint64_t kept_serial;    /* the frame they're of (0: none) */
    Held lay;                /* the draw that laid the scene onto the frame (fgn_note_lay), and its bytes */
    uint8_t* lay_mem;
    size_t lay_cap;
    uint64_t lay_serial;
    GfxTex* named;           /* the frame just after it, the names on it (fgn_note_named) */
    uint64_t named_serial;
    GfxTex *made, *layer, *z; /* a made frame being finished, the names' layer, its depth (fx_namez) */
    FgnBox *now, *before;    /* each character's box this frame, by label, and the frame before's */
    uint32_t nnow, nbefore, cap_now, cap_before;
    uint64_t boxes_serial;   /* the frame `now` is of */
    int failed;              /* a pipeline of its passes couldn't be made: off */
    int said;                /* the log said it runs */
    const char* whys[8];     /* ... and why not, each reason once */
    uint32_t nwhy;
} g_fgn;

static int fgn_wanted(void)
{
    return g_fxs.names_fg != 0.0f && fg_wanted() && !g_fgn.failed;
}

/* why this frame's made frames don't have their names drawn here, in the log the first time for each reason */
static int fgn_not(const char* why)
{
    for (uint32_t i = 0; i < g_fgn.nwhy; ++i)
        if (g_fgn.whys[i] == why)
            return 0;
    if (g_fgn.nwhy < sizeof g_fgn.whys / sizeof g_fgn.whys[0])
        g_fgn.whys[g_fgn.nwhy++] = why;
    fprintf(stderr, "[recomp] gfx: %s: the names over heads are left to it in its frames: %s ([graphics] "
                    "nameplates.framegen)\n", fg_name(), why);
    return 0;
}

/* a kept draw, its vertices and indices pointed at in mem */
static void fgn_point(GfxDraw* d, const Held* h, const uint8_t* mem)
{
    *d = h->d;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        d->data[s] = h->data[s] ? mem + h->data[s] - 1 : NULL;
    d->indices = h->indices ? mem + h->indices - 1 : NULL;
}

/* *mem at least `need` bytes */
static int fgn_grow(uint8_t** mem, size_t* cap, size_t need)
{
    if (need <= *cap)
        return 1;
    size_t c = *cap ? *cap : 65536;
    while (c < need)
        c *= 2;
    uint8_t* m = (uint8_t*)realloc(*mem, c);
    if (!m)
        return 0;
    *mem = m, *cap = c;
    return 1;
}

/* n bytes at p into *mem at *len (dlss_hold's way): their offset + 1 (0 for none, (size_t)-1 when there's no memory) */
static size_t fgn_copy(uint8_t** mem, size_t* cap, size_t* len, const void* p, size_t n)
{
    if (!p || !n)
        return 0;
    if (!fgn_grow(mem, cap, *len + n + 16))
        return (size_t)-1;
    size_t at = *len;
    memcpy(*mem + at, p, n);
    *len += (n + 15) & ~(size_t)15;
    return at + 1;
}

/* A held draw of one quad of transformed vertices from memory it brought (a glyph, an icon), and two draws alike but
 * for their vertices (gfx_dispatch.c quad_same's way, the character's too): a name is a glyph a draw, hundreds a
 * frame in a town, so for each made frame those of one name are drawn as one list of triangles (fgn_keep) */
static int fgn_quad(const Held* h)
{
    const GfxDraw* d = &h->d;
    if (!d->vs.rhw || d->prim != GFX_TRIANGLESTRIP || d->count != 2 || h->indices || d->vertex_start || d->u.vofs ||
        !h->data[0] || d->u.stride[0] <= 0 || d->size[0] < 4 * (uint32_t)d->u.stride[0])
        return 0;
    for (int s = 1; s < GFX_NSTREAMS; ++s)
        if (h->data[s])
            return 0;
    return 1;
}

static int fgn_same(const GfxDraw* a, const GfxDraw* b)
{
    return a->label == b->label && !memcmp(&a->vs, &b->vs, sizeof a->vs) && !memcmp(&a->fs, &b->fs, sizeof a->fs) &&
        !memcmp(&a->pipe, &b->pipe, sizeof a->pipe) && !memcmp(&a->depth, &b->depth, sizeof a->depth) &&
        a->vs_tokens == b->vs_tokens && a->ps_tokens == b->ps_tokens && !memcmp(&a->u, &b->u, sizeof a->u) &&
        !memcmp(a->tex, b->tex, sizeof a->tex) && !memcmp(a->samp, b->samp, sizeof a->samp) && a->cull == b->cull &&
        a->caster == b->caster && a->fill == b->fill && a->overlay == b->overlay && a->water == b->water &&
        a->small_rt == b->small_rt && a->screen_fx == b->screen_fx &&
        a->el_lantern == b->el_lantern && a->dlss5_skip == b->dlss5_skip && a->zbias == b->zbias &&
        a->stencil_ref == b->stencil_ref && !memcmp(a->vp, b->vp, sizeof a->vp) && a->text_shadow == b->text_shadow &&
        a->text_alpha == b->text_alpha;
}

static int fgn_box_order(const void* a, const void* b)
{
    const uint32_t x = ((const FgnBox*)a)->label, y = ((const FgnBox*)b)->label;
    return x < y ? -1 : x > y;
}

static const FgnBox* fgn_find(const FgnBox* box, uint32_t n, uint32_t label)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (box[mid].label < label)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && box[lo].label == label ? &box[lo] : NULL;
}

/* Each character's box this frame from the kept draws (its name's and the line's under it), the frame before's kept
 * as it was if it was the frame just before */
static void fgn_boxes(void)
{
    if (g_fgn.boxes_serial != g_serial)
    {
        if (g_fgn.boxes_serial + 1 == g_serial)
        {
            FgnBox* t = g_fgn.before;
            const uint32_t cap = g_fgn.cap_before;
            g_fgn.before = g_fgn.now, g_fgn.cap_before = g_fgn.cap_now, g_fgn.nbefore = g_fgn.nnow;
            g_fgn.now = t, g_fgn.cap_now = cap;
        }
        else
            g_fgn.nbefore = 0;
        g_fgn.boxes_serial = g_serial;
    }
    g_fgn.nnow = 0;
    FgnBox* b = NULL;
    for (uint32_t i = 0; i < g_fgn.nheld; ++i)
    {
        const Held* h = &g_fgn.held[i];
        const GfxDraw* d = &h->d;
        const GfxElem* pos = &d->vs.el[GFX_R_POSITION];
        const uint32_t stride = (uint32_t)d->u.stride[0], off = (uint32_t)d->u.offset[GFX_R_POSITION];
        if (!d->label || h->rt != g_dlss.scene || !d->vs.rhw || d->vs.prog || !pos->used || pos->stream || !h->data[0] ||
            stride < 8 || off + 8 > stride || !(d->u.vp[2] > 0.0f) || !(d->u.vp[3] > 0.0f))
            continue;
        if (!b || b->label != d->label)
        {
            b = NULL;
            for (uint32_t j = 0; j < g_fgn.nnow && !b; ++j)
                if (g_fgn.now[j].label == d->label)
                    b = &g_fgn.now[j];
            if (!b)
            {
                if (g_fgn.nnow == g_fgn.cap_now)
                {
                    const uint32_t cap = g_fgn.cap_now ? g_fgn.cap_now * 2 : 64;
                    FgnBox* more = (FgnBox*)realloc(g_fgn.now, cap * sizeof *more);
                    if (!more)
                        break;
                    g_fgn.now = more, g_fgn.cap_now = cap;
                }
                b = &g_fgn.now[g_fgn.nnow++];
                b->label = d->label;
                b->lo[0] = b->lo[1] = 1e30f, b->hi[0] = b->hi[1] = -1e30f;
            }
        }
        const uint8_t* v = g_fgn.mem + h->data[0] - 1 + off;
        for (uint32_t k = 0, n = d->size[0] / stride; k < n; ++k, v += stride)
        {
            float p[2];
            memcpy(p, v, sizeof p);
            for (int j = 0; j < 2; ++j)
            {
                const float s = (p[j] - d->u.vp[j]) / d->u.vp[2 + j];
                if (s == s) /* (not a NaN) */
                    b->lo[j] = s < b->lo[j] ? s : b->lo[j], b->hi[j] = s > b->hi[j] ? s : b->hi[j];
            }
        }
    }
    qsort(g_fgn.now, g_fgn.nnow, sizeof *g_fgn.now, fgn_box_order);
}

/* How far a character's name moved since the frame before, as shares of its viewport: 0 (and 0 back) for one that
 * wasn't there, or that moved half the screen or more in a frame (a teleport; the camera's jumps are frame generation's
 * reset) */
static int fgn_moved(uint32_t label, float m[2])
{
    m[0] = m[1] = 0.0f;
    const FgnBox* a = fgn_find(g_fgn.now, g_fgn.nnow, label);
    const FgnBox* b = a ? fgn_find(g_fgn.before, g_fgn.nbefore, label) : NULL;
    if (!b || !(a->lo[0] <= a->hi[0]) || !(a->lo[1] <= a->hi[1]) || !(b->lo[0] <= b->hi[0]) || !(b->lo[1] <= b->hi[1]))
        return 0;
    for (int j = 0; j < 2; ++j)
        m[j] = (a->lo[j] + a->hi[j] - b->lo[j] - b->hi[j]) * 0.5f;
    if (!(fabsf(m[0]) < 0.5f) || !(fabsf(m[1]) < 0.5f))
    {
        m[0] = m[1] = 0.0f;
        return 0;
    }
    return 1;
}

/* The interface held for DLSS this frame, just drawn over its output (dlss_replay): kept, with its bytes, for frame
 * generation's frames at the frame's end - a run of quads alike (fgn_quad, fgn_same) as one list of triangles - and
 * each character's box worked out */
static void fgn_keep(void)
{
    static const uint8_t STRIP[6] = { 0, 1, 2, 2, 1, 3 };
    g_fgn.kept_serial = 0, g_fgn.nheld = 0;
    if (!fgn_wanted() || !g_nheld || g_dlss.out_serial != g_serial)
        return;
    if (g_fgn.cap_held < g_nheld)
    {
        Held* more = (Held*)realloc(g_fgn.held, g_nheld * sizeof *more);
        if (!more)
            return;
        g_fgn.held = more, g_fgn.cap_held = g_nheld;
    }
    size_t len = 0;
    Held* run = NULL; /* the last kept while it's a list of quads more can join */
    for (uint32_t i = 0; i < g_nheld; ++i)
    {
        const Held* h = &g_held[i];
        const int quad = fgn_quad(h);
        const size_t stride = quad ? (size_t)h->d.u.stride[0] : 0;
        const uint8_t* v = quad ? g_held_mem + h->data[0] - 1 : NULL;
        Held* k = run;
        if (!quad || !run || run->rt != h->rt || run->ds != h->ds || run->face != h->face || run->level != h->level ||
            !fgn_same(&run->d, &h->d))
        {
            len = (len + 15) & ~(size_t)15;
            k = &g_fgn.held[g_fgn.nheld++];
            *k = *h;
            run = NULL;
            if (!quad)
            {
                for (int s = 0; s < GFX_NSTREAMS; ++s)
                    k->data[s] = fgn_copy(&g_fgn.mem, &g_fgn.mem_cap, &len, h->data[s] ? g_held_mem + h->data[s] - 1 : NULL,
                        h->d.size[s]);
                const uint32_t n = h->d.prim == GFX_TRIANGLEFAN ? h->d.count + 2 : vertex_count(h->d.prim, h->d.count);
                k->indices = fgn_copy(&g_fgn.mem, &g_fgn.mem_cap, &len, h->indices ? g_held_mem + h->indices - 1 : NULL,
                    (size_t)n * h->d.index_size);
                for (int s = 0; s < GFX_NSTREAMS; ++s)
                    if (k->data[s] == (size_t)-1 || k->indices == (size_t)-1)
                        return;
                continue;
            }
            k->data[0] = len + 1;
            k->d.prim = GFX_TRIANGLELIST, k->d.count = 0, k->d.size[0] = 0;
            run = k;
        }
        /* the quad's strip as two triangles at the end of the run */
        if (!fgn_grow(&g_fgn.mem, &g_fgn.mem_cap, len + 6 * stride))
            return;
        for (int j = 0; j < 6; ++j)
            memcpy(g_fgn.mem + len + j * stride, v + STRIP[j] * stride, stride);
        len += 6 * stride;
        k->d.count += 2, k->d.size[0] += (uint32_t)(6 * stride);
    }
    g_fgn.kept_serial = g_serial;
    fgn_boxes();
}

/* The draw laying the scene onto the frame, just made from DLSS's output as it was made (fg_hudless_clean): kept with
 * its bytes, to lay the names' layer the same way; the frame after the draw itself (the names on it) to be copied
 * (fgn_note_named). Only one that reads the scene through its first stage, from memory it brought (dlss_hold's) */
static void fgn_note_lay(const GfxDraw* d)
{
    g_fgn.lay_serial = 0;
    g_fgn_named_next = 0;
    if (!fgn_wanted() || g_fgn.kept_serial != g_serial)
        return;
    if (d->ibuf || d->fs.prog || d->tex[0] != g_dlss.scene)
    {
        fgn_not("the scene is laid onto the frame by a draw it can't do again");
        return;
    }
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (d->buf[s])
        {
            fgn_not("the scene is laid onto the frame by a draw it can't do again");
            return;
        }
    Held* h = &g_fgn.lay;
    size_t len = 0;
    h->d = *d;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        h->data[s] = fgn_copy(&g_fgn.lay_mem, &g_fgn.lay_cap, &len, d->data[s], d->size[s]);
        h->d.cpu[s] = NULL, h->d.cpu_size[s] = 0;
    }
    const uint32_t n = d->prim == GFX_TRIANGLEFAN ? d->count + 2 : vertex_count(d->prim, d->count);
    h->indices = fgn_copy(&g_fgn.lay_mem, &g_fgn.lay_cap, &len, d->indices, (size_t)n * d->index_size);
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (h->data[s] == (size_t)-1 || h->indices == (size_t)-1)
            return;
    h->rt = g_rt, h->ds = g_ds, h->face = g_rt_face, h->level = g_rt_level;
    g_fgn.lay_serial = g_serial;
    g_fgn_named_next = 1;
}

/* level 0 of src into dst (texture copies keep to it: a large target can have a chain of smaller levels) */
static void fgn_copy_level(ID3D12Resource* dst, ID3D12Resource* src)
{
    D3D12_TEXTURE_COPY_LOCATION to, from;
    memset(&to, 0, sizeof to), memset(&from, 0, sizeof from);
    from.pResource = src, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = dst, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ID3D12GraphicsCommandList_CopyTextureRegion(list(), &to, 0, 0, 0, &from, NULL);
}

/* After the draw that laid the scene with the names over it onto the frame: the frame then, copied (g_fgn.named) */
static void fgn_note_named(void)
{
    g_fgn_named_next = 0;
    g_fgn.named_serial = 0;
    GfxTex* frame = g_rt;
    if (g_fgn.lay_serial != g_serial || !frame || frame->type != GFX_TEX_2D || g_rt_face || g_rt_level)
        return;
    GfxTex* n = g_fgn.named;
    if (!n || n->w != frame->w || n->h != frame->h || n->dxfmt != frame->dxfmt)
    {
        gfx_tex_destroy(n);
        n = g_fgn.named = gfx_tex_create(GFX_TEX_2D, frame->fmt, frame->w, frame->h, 1, GFX_USE_RT);
        if (!n || n->dxfmt != frame->dxfmt || n->srv < 0)
        {
            gfx_tex_destroy(n), g_fgn.named = NULL;
            fgn_not("couldn't make a copy of the frame");
            return;
        }
    }
    const int part_was = gpu_part_cur();
    gpu_part(GP_FG_NAMES);
    tex_state(frame, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex_state(n, D3D12_RESOURCE_STATE_COPY_DEST);
    fgn_copy_level(n->res, frame->res);
    tex_state(n, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(frame, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_targets_bound = 0;
    g_fgn.named_serial = g_serial;
    gpu_part(part_was);
}

/* *t, a texture of this format and size, made again when either changes */
static int fgn_tex(GfxTex** t, uint32_t fmt, uint32_t w, uint32_t h, int use)
{
    if (*t && (*t)->fmt == fmt && (*t)->w == w && (*t)->h == h)
        return 1;
    gfx_tex_destroy(*t);
    *t = gfx_tex_create(GFX_TEX_2D, fmt, w, h, 1, use);
    if (*t && (*t)->srv >= 0)
        return 1;
    gfx_tex_destroy(*t), *t = NULL;
    return 0;
}

static void fgn_release(void)
{
    GfxTex** const t[] = { &g_fgn.named, &g_fgn.made, &g_fgn.layer, &g_fgn.z };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; ++i)
        if (*t[i])
            gfx_tex_destroy(*t[i]), *t[i] = NULL;
    g_fgn.kept_serial = g_fgn.lay_serial = g_fgn.named_serial = 0;
}

/* a texture let go (gfx_tex_destroy): the kept draws that read it, or draw into it, aren't drawn again */
static void fgn_forget_tex(const GfxTex* t)
{
    if (g_fgn.kept_serial != g_serial)
        g_fgn.nheld = 0;
    for (uint32_t i = 0; i < g_fgn.nheld; ++i)
    {
        Held* h = &g_fgn.held[i];
        for (int k = 0; k < 8; ++k)
            if (h->d.tex[k] == t)
                h->rt = NULL;
        if (h->rt == t)
            h->rt = NULL;
    }
    for (int k = 0; k < 8; ++k)
        if (g_fgn.lay.d.tex[k] == t)
            g_fgn.lay_serial = 0;
    if (t == g_fgn.named)
        g_fgn.named_serial = 0;
}

/* Whether this frame's made frames get their names here (fgn_frame), with what that needs made: its textures (a made
 * frame's, the frame's size and format; the layer, the output's size; its depth, the scene's depth's format, when the
 * names were tested against it) and its passes' pipelines */
static int fgn_ready(GfxTex* bb)
{
    GfxTex *out = g_dlss.out, *scene = g_dlss.scene;
    if (!fgn_wanted() || g_fgn.kept_serial != g_serial || !out || !scene)
        return 0;
    int named = 0;
    for (uint32_t i = 0; i < g_fgn.nheld && !named; ++i)
        named = g_fgn.held[i].d.label && g_fgn.held[i].rt == scene;
    if (!named) /* no character's name this frame: frame generation lays the rest as before */
        return 0;
    if (g_fgn.lay_serial != g_serial || g_fgn.named_serial != g_serial || g_fg.hudless_serial != g_serial)
        return fgn_not("the scene wasn't laid onto the frame by a draw it could keep");
    if (!g_fgn.named || g_fgn.named->w != bb->w || g_fgn.named->h != bb->h || g_fgn.named->dxfmt != bb->dxfmt ||
        bb->dxfmt != g_fg.fmt || bb->srv < 0)
        return fgn_not("the scene was laid onto another target than the frame shown");
    GfxTex* zup = g_zup_serial == g_serial ? g_dlss_zups[g_zup_cur] : NULL;
    if (!g_fx.fgui || (zup && !g_fx.namez)) /* not built yet (the effects' build, on the thread pool), or didn't compile */
        return InterlockedCompareExchange(&g_fx.state, 0, 0) >= 2 ? fgn_not("its passes didn't compile") : 0;
    if (!fgn_tex(&g_fgn.made, bb->fmt, bb->w, bb->h, GFX_USE_RT) || g_fgn.made->dxfmt != bb->dxfmt ||
        !fgn_tex(&g_fgn.layer, F_A8R8G8B8, out->w, out->h, GFX_USE_RT) ||
        (zup && !fgn_tex(&g_fgn.z, zup->fmt, out->w, out->h, GFX_USE_DEPTH)))
        return fgn_not("couldn't make its textures");
    if (!g_fgn_ui_pso || g_fgn_ui_fmt != bb->dxfmt)
    {
        if (g_fgn_ui_pso)
            defer((IUnknown*)g_fgn_ui_pso, NULL, -1);
        g_fgn_ui_pso = fx_make_pso_ex(g_fx.fgui, bb->dxfmt, 1);
        g_fgn_ui_fmt = bb->dxfmt;
    }
    if (zup && (!g_fgn_z_pso || g_fgn_z_fmt != zup->dxfmt))
    {
        if (g_fgn_z_pso)
            defer((IUnknown*)g_fgn_z_pso, NULL, -1);
        g_fgn_z_pso = fx_make_depth_pso(g_fx.namez, zup->dxfmt);
        g_fgn_z_fmt = zup->dxfmt;
    }
    if (!g_fgn_ui_pso || (zup && !g_fgn_z_pso))
    {
        g_fgn.failed = 1;
        return fgn_not("its passes' pipelines couldn't be made; off");
    }
    if (!g_fgn.said)
    {
        g_fgn.said = 1;
        fprintf(stderr, "[recomp] gfx: %s: the names over heads drawn onto its frames, each where it is between the game's "
                        "frames ([graphics] nameplates.framegen)\n", fg_name());
    }
    return 1;
}

/* Made frame k (of g_fg.use, before the real one; fgn_ready said yes) finished: copied, the names drawn over it where
 * they'd be then, the frame's interface over that. reset: the camera jumped (no motion). The texture to show */
static GfxTex* fgn_frame(int k, GfxTex* bb, int reset)
{
    GfxTex *made = g_fgn.made, *layer = g_fgn.layer, *out = g_dlss.out, *scene = g_dlss.scene;
    GfxTex* zup = g_zup_serial == g_serial ? g_dlss_zups[g_zup_cur] : NULL;
    GfxTex* z = zup ? g_fgn.z : NULL;
    ID3D12GraphicsCommandList* l = list();
    const int part_was = gpu_part_cur();
    gpu_part(GP_FG_NAMES);
    /* how far back toward the frame before it is: the made frames are spread evenly between that and the real one */
    const float back = (float)(g_fg.use - k) / (float)(g_fg.use + 1);
    /* the frame as frame generation made it */
    barrier(g_fg.out[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    tex_state(made, D3D12_RESOURCE_STATE_COPY_DEST);
    fgn_copy_level(made->res, g_fg.out[k]);
    barrier(g_fg.out[k], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    /* the layer cleared; its depth the scene's, moved back by the camera */
    tex_state(layer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(l, target_view(layer, 0, 0), clear, 0, NULL);
    if (z)
    {
        FxU u;
        memset(&u, 0, sizeof u);
        u.size[0] = (float)out->w, u.size[1] = (float)out->h;
        float minz, maxz;
        memcpy(&minz, &g_dlss.pend_s.vp[4], 4);
        memcpy(&maxz, &g_dlss.pend_s.vp[5], 4);
        if (!(maxz > minz))
            minz = 0.0f, maxz = 1.0f;
        u.zp[2] = minz, u.zp[3] = maxz;
        if (reset || !g_dlss.sh)
            mat_identity(u.reproj);
        else
            memcpy(u.reproj, g_dlss.sh->clip_to_prev_clip, 64);
        u.hist[1] = back;
        tex_state(zup, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        tex_state(z, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const int32_t in[2] = { zup->srv, -1 };
        fx_draw_depth(target_view(z, 0, 0), out->w, out->h, g_fgn_z_pso, &u, in);
    }
    /* the interface held for DLSS into the layer as dlss_draw_over drew it over the output, each name moved back */
    GfxTex *rt = g_rt, *ds = g_ds;
    const uint32_t face = g_rt_face, level = g_rt_level;
    g_rt = layer, g_ds = z, g_rt_face = g_rt_level = 0, g_targets_bound = 0;
    const double kx = (double)out->w / scene->w, ky = (double)out->h / scene->h;
    static GfxDraw o;
    for (uint32_t i = 0; i < g_fgn.nheld; ++i)
    {
        const Held* h = &g_fgn.held[i];
        if (h->rt != scene)
            continue;
        fgn_point(&o, h, g_fgn.mem);
        o.vp[0] = (uint32_t)(o.vp[0] * kx + 0.5), o.vp[1] = (uint32_t)(o.vp[1] * ky + 0.5);
        o.vp[2] = (uint32_t)(o.vp[2] * kx + 0.5), o.vp[3] = (uint32_t)(o.vp[3] * ky + 0.5);
        o.depth.zwrite = 0;
        if (!z)
            o.depth.zenable = 0;
        float m[2];
        if (o.label && !reset && o.vs.rhw && fgn_moved(o.label, m)) /* the viewport the other way: the draw back by `back` of m */
            o.u.vp[0] += back * m[0] * o.u.vp[2], o.u.vp[1] += back * m[1] * o.u.vp[3];
        /* premultiplied, its alpha how much each pixel covers; what adds light adds it and covers nothing */
        if (o.pipe.blend && o.pipe.dst == 2 /* ONE */)
            o.pipe.write_mask &= 7;
        else if (o.pipe.blend)
        {
            o.pipe.over = 1;
            if (o.pipe.write_mask & 7)
                o.pipe.write_mask |= 8;
        }
        draw_encode(&o);
    }
    /* the layer over the made frame by the draw that laid the scene onto the frame, reading the layer in the scene's
     * place: its alpha through the stages as it is (the scene's own meant nothing to the draw), blended over */
    fgn_point(&o, &g_fgn.lay, g_fgn.lay_mem);
    o.pipe.blend = 1, o.pipe.src = 2 /* ONE */, o.pipe.dst = 6 /* INVSRCALPHA */, o.pipe.op = 1 /* ADD */, o.pipe.over = 0;
    o.pipe.write_mask &= 7;
    o.fs.st[0].aop = 2 /* SELECTARG1 */, o.fs.st[0].aa1 = 2 /* TEXTURE */;
    for (int s = 1; s < o.fs.nstages && s < 8; ++s)
        if (o.fs.st[s].aop != 1 /* DISABLE */)
            o.fs.st[s].aop = 2, o.fs.st[s].aa1 = 1 /* CURRENT */;
    o.fs.alpha_func = 0;
    o.depth.zenable = 0, o.depth.zwrite = 0, o.depth.stencil = 0;
    g_rt = made, g_ds = NULL, g_rt_face = g_rt_level = 0, g_targets_bound = 0;
    g_fgn_sample = layer;
    draw_encode(&o);
    g_fgn_sample = NULL;
    g_rt = rt, g_ds = ds, g_rt_face = face, g_rt_level = level, g_targets_bound = 0;
    /* the frame's interface over it, where the real frame has one */
    tex_state(bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(g_fgn.named, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(made, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float vp[4] = { 0.0f, 0.0f, (float)made->w, (float)made->h };
    const int32_t in[2] = { bb->srv, g_fgn.named->srv };
    fx_draw(target_view(made, 0, 0), made->w, made->h, g_fgn_ui_pso, vp, NULL, in, 2, 0, 0);
    gpu_part(part_was);
    return made;
}

/* frame generation handed the scene alone as the frame too (fgn_ready): no interface for it to lay over its frames */
static void fgn_copy_shared(ID3D12Resource* dst, ID3D12Resource* src)
{
    barrier(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyResource(list(), dst, src);
    barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
}

/* The frame through frame generation, then onto the screen by the present thread: 1 if so, 0 to present it as
 * usual (off, not ready, or not this frame: DLSS didn't run on its scene) */
static int fg_present(GfxTex* bb)
{
    VanaDlssShared* sh = g_dlss.sh;
    if (!g_fg.count || g_fg.failed || !g_swap || !bb || bb->srv < 0 || bb->type != GFX_TEX_2D || !g_present_pso ||
        g_dlss.failed || fsr1_wanted() || !sh || sh->state != VANA_DLSS_READY || g_dlss.ran_serial != g_serial || !g_dlss.in_w ||
        g_dlss.started != g_dlss.upscaler) /* (the other upscaler's helper, till it's let go: not this one's to ask, fg_watch) */
        return 0;
    {
        const char* why = NULL;
        if (!fg_here(sh, &why))
        {
            fg_fail(1, why[0] ? why : "frame generation isn't available here");
            return 0;
        }
    }
    if (g_fg.last)
    {
        const uint64_t waited_from = gfx_now_ns();
        const int caught_up = helper_caught_up(&sh->fg_done, g_fg.last, 100);
        helper_wait_note(waited_from, "the helper's frame generation", g_fg.last, sh->fg_done);
        if (!caught_up)
            return 0; /* the helper's still on the last (fg_watch) */
    }
    /* as many as the card makes: NVIDIA's up to 3 (an RTX 50), AMD's one */
    const int most = g_fg.kind == VANA_FG_FSR ? 1 : sh->fg_count_max;
    g_fg.use = most >= 1 && g_fg.count > most ? most : g_fg.count;
    if (!fg_start_thread())
    {
        fg_fail(0, "its present thread didn't start");
        return 0;
    }
    swap_fit(); /* the present thread let finish first, when the window's size changed */
    if (bb->w != g_fg.w || bb->h != g_fg.h || bb->dxfmt != g_fg.fmt || (uint32_t)g_fg.use != g_fg.made_use ||
        g_dlss.in_w != g_fg.in_w || g_dlss.in_h != g_fg.in_h || g_fg.kind != g_fg.made_kind)
    {
        if (!fg_targets(bb->w, bb->h, bb->dxfmt))
        {
            fg_release();
            fg_fail(0, "couldn't make its textures");
            return 0;
        }
        SetEvent(g_dlss.go_fg); /* the helper makes frame generation for them: frames as usual till it has */
        g_fg.starting = 1;
        return 0;
    }
    if (sh->fg_made != sh->fg_generation)
        return 0;
    g_fg.starting = 0;
    if (g_fg.noted_fps != (int)g_fps_max)
        fg_note_display();
    /* this job's set: free once the job two before it is shown */
    const LONG seq = g_fg.posted;
    const uint64_t waited_from = gfx_now_ns();
    while (g_fg.done < seq - 1)
        WaitForSingleObject(g_fg.idle, 100);
    helper_wait_note(waited_from, "frame generation's present thread", (uint64_t)(seq - 1), (uint64_t)g_fg.done);
    const int set = seq & 1;
    for (int k = 0; k <= g_fg.use; ++k)
        if (!fx_tex(&g_fg.shown[set][k], DXGI_FORMAT_B8G8R8A8_UNORM, g_swap_w, g_swap_h))
        {
            fg_fail(0, "couldn't make its frames");
            return 0;
        }
    /* the camera: this frame's (the scene DLSS ran on, without its jitter), the frame before's */
    const GfxScene* s = &g_dlss.pend_s;
    float vinv[16], pinv[16], m[16], m2[16];
    if (!mat_inverse(vinv, s->view) || !mat_inverse(pinv, s->proj))
        return 0;
    /* the frame, and the scene alone when no draw laid it onto the frame (frame generation then guesses the interface);
     * the scene alone as the frame too when the names go onto its frames here, with the frame's interface (fgn_frame) */
    gpu_part(GP_FG_COPY);
    const int fgn = fgn_ready(bb);
    if (fgn)
        fgn_copy_shared(g_fg.real, g_fg.hudless);
    else
        fg_copy(g_fg.real, bb);
    if (g_fg.hudless_serial != g_serial)
        fg_copy(g_fg.hudless, bb);
    const int reset = g_fg.prev_serial + 1 != g_serial || sh->reset;
    memcpy(sh->view_to_clip, s->proj, 64);
    memcpy(sh->clip_to_view, pinv, 64);
    if (reset)
        mat_identity(sh->clip_to_prev_clip), mat_identity(sh->prev_clip_to_clip);
    else
    {
        mat_mul(m, pinv, vinv);
        mat_mul(m2, m, g_fg.prev_view);
        mat_mul(sh->clip_to_prev_clip, m2, g_fg.prev_proj);
        if (!mat_inverse(sh->prev_clip_to_clip, sh->clip_to_prev_clip))
            mat_identity(sh->prev_clip_to_clip);
    }
    memcpy(g_fg.prev_view, s->view, 64), memcpy(g_fg.prev_proj, s->proj, 64);
    g_fg.prev_serial = g_serial;
    const float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f; /* right-handed: the camera looks down -z */
    for (int j = 0; j < 3; ++j)
        sh->cam_right[j] = vinv[j], sh->cam_up[j] = vinv[4 + j], sh->cam_fwd[j] = hand * vinv[8 + j], sh->cam_pos[j] = vinv[12 + j];
    const float a = s->proj[10], b = s->proj[14];
    const float zn = fabsf(a) > 1e-6f ? fabsf(b / a) : 0.1f;
    const float zf = fabsf(a - hand) > 1e-6f ? fabsf(b / (a - hand)) : 1000.0f;
    sh->cam_near = zn, sh->cam_far = zf > zn ? zf : zn + 1.0f;
    sh->cam_fov = 2.0f * atanf(1.0f / (fabsf(s->proj[5]) > 1e-6f ? fabsf(s->proj[5]) : 1.0f));
    sh->cam_aspect = fabsf(s->proj[0]) > 1e-6f ? fabsf(s->proj[5] / s->proj[0]) : 1.0f;
    sh->fg_motion_scale[0] = sh->fg_motion_scale[1] = 1.0f; /* DLSS's motion: render pixels already */
    sh->fg_reset = reset;
    /* AMD's: this frame's jitter and the part of the depth and motion drawn (the upscaler's), the time since the frame
     * before, and the frame's number (one more each frame: one skipped starts it afresh) */
    sh->fg_jitter_x = g_dlss.jit_serial == g_serial ? g_dlss.jit[0] : 0.0f;
    sh->fg_jitter_y = g_dlss.jit_serial == g_serial ? g_dlss.jit[1] : 0.0f;
    sh->fg_render_w = g_dlss.in_w, sh->fg_render_h = g_dlss.in_h;
    {
        const double ms = g_fg.last_ns ? (double)(gfx_now_ns() - g_fg.last_ns) / 1e6 : 16.7;
        sh->fg_frame_ms = (float)(ms < 1.0 ? 1.0 : ms > 100.0 ? 100.0 : ms);
    }
    sh->fg_frame_id = g_serial;
    /* handed over: the fence at `frame` once the frame is copied, the helper's at frame + 1 */
    g_gap_part = GP_WAIT_FG; /* the queue waits on the helper (below) till the next list */
    submit(0);
    g_gs_part = GP_REST; /* the next list's, once the queue's waited */
    const uint64_t frame = g_fg.value + 1;
    g_fg.value = frame + 1;
    ID3D12CommandQueue_Signal(g_queue, g_fg.fence, frame);
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        sh->fg_posted_qpc = q.QuadPart;
    }
    MemoryBarrier();
    sh->fg_frame = frame;
    g_fg.last = frame; /* (before the helper can answer: br_thread reads it) */
    dlss_post(g_dlss.go_fg, g_fg.fence, frame);
    g_fg.last_at = GetTickCount64();
    ID3D12CommandQueue_Wait(g_queue, g_fg.fence, frame + 1);
    /* ... and the present queue past its copies of this set's last frames (its job two before this one, shown by now)
     * before they're drawn over */
    if (g_fg.job[set].copied && g_pfence)
        ID3D12CommandQueue_Wait(g_queue, g_pfence, g_fg.job[set].copied);
    /* the frames as shown: each made one, then the real one, VanaCore's interface over each */
    for (int k = 0; k <= g_fg.use; ++k)
    {
        FxTex* t = &g_fg.shown[set][k];
        fx_state(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
        GfxTex* finished = k < g_fg.use && fgn ? fgn_frame(k, bb, reset) : NULL;
        if (finished)
        {
            tex_state(finished, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            present_draw(heap_cpu(&g_rtv, t->rtv), t->w, t->h, finished->srv);
        }
        else if (k < g_fg.use)
        {
            barrier(g_fg.out[k], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            present_draw(heap_cpu(&g_rtv, t->rtv), t->w, t->h, g_fg.out_srv[k]);
            barrier(g_fg.out[k], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            tex_state(bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            present_draw(heap_cpu(&g_rtv, t->rtv), t->w, t->h, bb->srv);
        }
        fx_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    g_present_layer = NULL;
    /* the time between the game's frames, smoothed: each job's frames spread over it */
    const uint64_t now = gfx_now_ns();
    double dt = g_fg.last_ns ? (double)(now - g_fg.last_ns) * 1e-9 : 1.0 / 60.0;
    g_fg.last_ns = now;
    if (dt > 0.1)
        dt = 0.1;
    g_fg.interval = g_fg.interval > 0.0 ? g_fg.interval * 0.9 + dt * 0.1 : dt;
    for (int k = 0; k <= g_fg.use; ++k)
        fps_tick(); /* the frames shown */
    fx_reload();
    g_fg.on_serial = g_serial;
    frame_end();
    g_fg.job[set].frames = g_fg.use + 1;
    g_fg.job[set].ready = g_fence_value;
    g_fg.job[set].spacing = g_fg.interval / (double)(g_fg.use + 1);
    MemoryBarrier();
    InterlockedIncrement(&g_fg.posted);
    SetEvent(g_fg.wake);
    return 1;
}

void gfx_dlss_framegen(int count)
{
    count = count < 0 ? 0 : count > FG_MAX ? FG_MAX : count;
    if (count != g_fg.count)
    {
        if (count)
            fprintf(stderr, "[recomp] gfx: %s %dx\n", fg_name(), count + 1);
        else
            fprintf(stderr, "[recomp] gfx: %s off\n", fg_name());
        g_fg.failed = 0; /* tried again */
        g_fg.noted_fps = 0;
    }
    g_fg.count = count;
}

void vanacore_gfx_set_fps_max(int fps)
{
    g_fps_max = fps > 0 ? fps : 60;
}

int gfx_dlss_framegen_state(char* message, int size)
{
    char text[200] = "";
    int st;
    if (!g_fg.count)
        st = 0;
    else if (fsr1_wanted()) /* AMD's frame generation takes FSR 3.1's or 4's depth and motion: FSR 1 has none */
        st = 3, snprintf(text, sizeof text, "%s", "needs FSR 3.1 or 4 (FSR 1 is running)");
    else if (g_fg.failed)
        st = g_fg.unavailable ? 3 : 4, snprintf(text, sizeof text, "%s", g_fg.why);
    else if (g_fg.on_serial && g_serial - g_fg.on_serial <= 3)
    {
        const double fps = g_fg.interval > 0.0 ? 1.0 / g_fg.interval : 0.0;
        st = 2;
        const VanaDlssShared* sh = g_dlss.sh;
        if (g_fg.kind == VANA_FG_FSR) /* AMD's, by its version: FSR FG 4.0.1, FSR FG 3.1.6 */
            snprintf(text, sizeof text, "%s 2x: %.0f -> %.0f FPS", sh && sh->fg_version[0] ? sh->fg_version : "FSR FG", fps, fps * 2);
        else
            snprintf(text, sizeof text, "%dx: %.0f -> %.0f FPS%s", g_fg.use + 1, fps, fps * (g_fg.use + 1),
                g_fg.use < g_fg.count ? " (3x and 4x need an RTX 50)" : "");
    }
    else
        st = 1, snprintf(text, sizeof text, "%s", g_dlss.quality < 0 ? "needs DLSS or FSR on" : g_fg.starting ? "starting"
                                                : g_dlss.upscaler == VANA_UPSCALER_FSR ? "waiting for FSR" : "waiting for DLSS");
    if (message && size > 0)
        snprintf(message, size, "%s", text);
    return st;
}

void vanacore_gfx_set_framegen_kind(int kind)
{
    kind = kind == VANA_FG_FSR ? VANA_FG_FSR : VANA_FG_DLSS;
    if (kind != g_fg.kind)
    {
        g_fg.kind = kind;
        g_fg.failed = 0; /* the other one tried */
        fprintf(stderr, "[recomp] gfx: frame generation: %s\n", kind == VANA_FG_FSR ? "AMD's (FSR)" : "NVIDIA's (DLSS)");
    }
}
#endif

#ifdef VANACORE
/* DLSS is made and its helper has it, so dlss_run will take this frame's scene: depth of field then goes onto its output
 * (dof_after), else into the composite. Looks only (the helper isn't started by it). */
static int dlss_will_run(void)
{
    if (fsr1_wanted()) /* FSR 1 (fsr1_run): made for the last scene */
        return g_fxs.dlss_compare == 0.0f && g_dlss.fsr1 && g_dlss.out != NULL;
    const VanaDlssShared* sh = g_dlss.sh;
    return g_dlss.quality >= 0 && !g_dlss.failed && g_fxs.dlss_compare == 0.0f && sh && g_dlss.out && sh->state == VANA_DLSS_READY &&
        sh->made == sh->generation;
}

/* What the picture is still waiting on (gfx.h's GFX_WAIT_*): DLSS starting while there are scenes for it (not at a menu
 * without one, where it waits), DLSS 5 with it, frame generation starting, the effects' own shaders compiling, and a
 * tenth or more of the sun's shadow casters left out while their pipelines build (a few new ones come into view aren't
 * worth a notice) */
unsigned gfx_waiting(void)
{
    unsigned bits = 0;
    const VanaDlssShared* sh = g_dlss.sh;
    if (g_dlss.noted && g_dlss.noted + 3 >= g_serial && gfx_dlss_state(NULL, 0) == 1)
    {
        bits |= GFX_WAIT_DLSS;
        if (g_fxs.dlss5 != 0.0f && dlss_card(1) && (!sh || sh->state == VANA_DLSS_STARTING || sh->nr_available))
            bits |= GFX_WAIT_DLSS5;
    }
    else if (g_fg.count && !g_fg.failed && g_fg.starting)
        bits |= GFX_WAIT_FRAMEGEN;
    if (g_fxs.fx != 0.0f && InterlockedCompareExchange(&g_fx.state, 0, 0) == 1)
        bits |= GFX_WAIT_EFFECTS;
    if (g_fx.wait_serial + 3 >= g_serial && g_fx.wait_building && g_fx.wait_building * 10 >= g_fx.wait_casters)
        bits |= GFX_WAIT_SHADOWS;
    return bits;
}

/* DLSS's (or AMD's FSR's) start, as far as it has come, for the notice: 0 the helper starting (its process, till it's in
 * the shared memory), 1 it loading NGX (AMD's FSR), 2 it making DLSS for the textures (DLSS 5 first, with it: seconds),
 * 3 the first frame not back yet, 4 done. FSR 1, in this process, has no stages: -1 */
static int dlss_stage(void)
{
    const VanaDlssShared* sh = g_dlss.sh;
    if (fsr1_wanted())
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

/* ... and frame generation's: 0 the helper making it for the frame's textures, 1 its first frames not back yet, 2 done */
static int fg_stage(void)
{
    const VanaDlssShared* sh = g_dlss.sh;
    if (!sh || sh->fg_made != sh->fg_generation)
        return 0;
    return sh->fg_done == 0 ? 1 : 2;
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
        const int st = dlss_stage();
        if (st < 0)
            return 0;
        *total = 4, *done = (unsigned)st;
        return 1;
    }
    if (bit == GFX_WAIT_FRAMEGEN)
    {
        *total = 2, *done = (unsigned)fg_stage();
        return 1;
    }
    return 0;
}

/* What one GFX_WAIT_* bit of gfx_waiting is at now (gfx.h): the shader fx_build is compiling, DLSS's start's stage
 * (dlss_stage), frame generation's, in words; "" and 0 without */
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
        const int fsr = g_dlss.upscaler == VANA_UPSCALER_FSR;
        switch (dlss_stage())
        {
        case 0: snprintf(words, (size_t)size, "Starting the %s helper", fsr ? "FSR" : "DLSS"); return 1;
        case 1: snprintf(words, (size_t)size, "%s", fsr ? "Loading AMD's FSR" : "Loading NVIDIA's NGX"); return 1;
        case 2:
            snprintf(words, (size_t)size, "Making %s for %ux%u -> %ux%u", fsr ? "FSR" : g_dlss.nr ? "DLSS 5 and DLSS" : "DLSS",
                g_dlss.in_w, g_dlss.in_h, g_dlss.out_w, g_dlss.out_h);
            return 1;
        case 3: snprintf(words, (size_t)size, "%s", "Waiting for the first frame back"); return 1;
        default: return 0;
        }
    }
    if (bit == GFX_WAIT_FRAMEGEN)
    {
        const char* what = g_fg.kind == VANA_FG_FSR ? "FSR frame generation" : "DLSS frame generation";
        if (fg_stage() == 0)
            snprintf(words, (size_t)size, "Making %s at %ux%u", what, g_fg.w, g_fg.h);
        else
            snprintf(words, (size_t)size, "%s", "Waiting for its first frames back");
        return 1;
    }
    return 0;
}

#include "gfx_d3d12_measure.h" /* cf_off, comp_measure: the composite's features measured (vanacore_gfx_measure) */
#endif

/* gfx.h: the scene's depth before the grass, copied whole (Direct3D copies depth no other way) for this scene's occlusion and
 * contact shadows, its ink lines without the grass, and the reflections, which trace the world without it (gfx_scene_done) */
void gfx_scene_world_depth(GfxTex* depth)
{
    BATCH_SETTLE();
    g_fx.wdepth_of = g_fx.wdepth_glow = NULL;
    int lines = 0;
#ifdef VANACORE
    lines = (g_fxs.outline > 0.0f && g_fxs.outline_nograss != 0.0f) || (int)g_fxs.debug == 26; /* ... and Render visual 26's grass */
#endif
    /* ... and VanaCore's glow laid again over the grass (gfx_glow_over_grass), which alone leaves the others as they were */
    const int others = (g_fxs.fx != 0.0f && (g_fxs.ao > 0.0f || g_fxs.refl > 0.0f || lines)) || mask_wanted();
    if (!g_dev || !(others || g_fx.glow_kept_serial == g_serial) || !depth || depth->use != GFX_USE_DEPTH || depth->type != GFX_TEX_2D ||
        depth->srv < 0)
        return;
    if (!g_fx.wdepth || g_fx.wdepth->w != depth->w || g_fx.wdepth->h != depth->h || g_fx.wdepth->dxfmt != depth->dxfmt)
    {
        gfx_tex_destroy(g_fx.wdepth);
        g_fx.wdepth = gfx_tex_create(GFX_TEX_2D, depth->fmt, depth->w, depth->h, 1, GFX_USE_DEPTH);
    }
    if (!g_fx.wdepth || g_fx.wdepth->srv < 0)
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
    if (!depth || depth->use != GFX_USE_DEPTH || depth->type != GFX_TEX_2D || depth->w != color->w || depth->h != color->h || depth->srv < 0)
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
    if (!g_snap_color || !g_snap_depth || g_snap_color->srv < 0 || g_snap_depth->srv < 0)
        return 0;
    gfx_copy(color, 0, 0, 0, 0, color->w, color->h, g_snap_color, 0, 0, 0, 0);
    gfx_copy(depth, 0, 0, 0, 0, depth->w, depth->h, g_snap_depth, 0, 0, 0, 0);
    *colour_copy = g_snap_color, *depth_copy = g_snap_depth, *w = color->w, *h = color->h;
    g_snap_serial = (uint32_t)g_serial;
    return 1;
}

#include "gfx_d3d12_floors.h" /* the glow over the grass, the floors and the rain's mask for the reflections (gfx_scene_floors) */

void gfx_scene_done(GfxTex* color, const GfxScene* s)
{
    if (!g_dev)
        return;
    int aa = 0; /* anti-aliasing, which runs with the effects off too */
    int dlss = 0;
    int gathers = 0; /* ... and gathers the frames, jittered (DLSS, AMD's FSR): not FSR 1's, one frame alone */
#ifdef VANACORE
    int grade_last = 0; /* the colour grade is to be laid on this scene once the game is done drawing into it (grade_scene, dlss_run) */
    int dof_late = 0;  /* depth of field is left for the upscaler's output (dof_after) */
    int el_u = 0;    /* the light from the effects: its uniforms are in g_el.u */
    int zl_late = 0;   /* the zone lines are left for after the world (zl_note): the compose has none; saved here */
    float zl_zlc[4], zl_zlcol[4], zl_zlp[4], zl_zl[64][4];
    BATCH_SETTLE(); /* the zone's draws kept, before the scene is taken */
    mo_flush(1); /* the moving things' redraws, against the world's finished depth */
    occl_scene(color, s); /* the zone's pieces against the world's finished depth, effects or not ([graphics] occl) */
    dlss = g_dlss.quality >= 0 && !dlss_gave_up();
    gathers = dlss && !fsr1_wanted();
    aa = g_fxs.aa != 0.0f && !gathers; /* DLSS smooths edges itself; FSR 1 wants them smoothed first */
    /* the scene copied at the size it's shown instead (plain_run), for the names over heads */
    const int plain = !dlss && g_fxs.names_native != 0.0f;
    if (!dlss && !plain && g_dlss.out && !g_dlss.in_w)
        dlss_release(); /* that copy, not wanted now */
#else
    const int plain = 0;
#endif
    if (!g_dev || (g_fxs.fx == 0.0f && !aa && !dlss && !plain) || !color || color->type != GFX_TEX_2D ||
        color->use != GFX_USE_RT)
    {
        casters_clear();
        return;
    }
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    prof_mark(1);
    gpu_scope(GS_SETUP);
    int ready = fx_ready();
    /* the depth the scene's casters (its world) were drawn with - a later pass on the target with
     * another depth does not change it - else the last one */
    GfxTex* depth = color->depth_world ? color->depth_world : color->depth_seen;
    g_fx.tr_depth = color->depth_world && color->depth_world != color->depth_seen ? 2 : depth ? 1 : 0;
    int fmt = ready ? fx_by_fmt(color->dxfmt) : -1;
    if (g_fxs.fx != 0.0f && fmt >= 0 && g_fx.by_fmt[fmt].comp && depth && depth->w == color->w && depth->h == color->h &&
        depth->srv >= 0 && s->proj[11] != 0.0f)
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
        /* ... but no larger than the window it's shown in (the swap chain's size), each way: a scene drawn bigger (the game's
         * Background resolution over the window's, no DLSS or FSR) is made smaller to be shown, and texels past one a shown
         * pixel were work nobody saw - Kazham's 3072x3072 scene in a 2560x1550 window took 15.5 ms for the occlusion at Max,
         * against 2.3 at FSR's 1495x884 (2026-10-28, an RTX 5070 Ti laptop held at 1150 MHz). A texel is then a shown pixel,
         * as with the scene at the window's own size, not one of four (the shimmer above); and square on the screen, where a
         * square scene in a wide window gave texels twice as tall as wide. ao_at and lay_at take them back to the scene's
         * pixels by distance */
        uint32_t bdiv = vw > 2048 ? 4 : vw > 1024 ? 2 : 1;
        uint32_t aw = (uint32_t)vw, ah = (uint32_t)vh;
        if (g_swap_w && g_swap_h)
            aw = aw < g_swap_w ? aw : g_swap_w, ah = ah < g_swap_h ? ah : g_swap_h;
        uint32_t bw = (uint32_t)((vw + bdiv - 1) / bdiv), bh = (uint32_t)((vh + bdiv - 1) / bdiv);
        float hand = s->proj[11] < 0.0f ? -1.0f : 1.0f;
        FxU u = {
            { s->proj[0], s->proj[5], s->proj[8], s->proj[9] },
            { s->proj[10], s->proj[14], minz, maxz },
            { vx, vy, vw, vh },
            { (float)color->w, (float)color->h, (float)aw, (float)ah },
            { g_fxs.radius, g_fxs.ao * fx_fade_in(), 0.15f, vh * 0.1f },
            { g_fxs.grade, g_fxs.sat, g_fxs.contrast, g_fxs.debug },
            { hand, 0, 0, 0 },
        };
#ifdef VANACORE
        /* the colour grade is laid last (grade_last, fx_post), over everything the game draws into the scene after this, not in
         * the composite: which has none here (strength 0); the moonlight with it (u.post4 left 0 here) */
        u.grade[0] = 0.0f;
        /* where this frame's jitter drew the scene, for the sun's maps' lookups (fx_ao): DLSS or AMD's FSR gathering it */
        if (gathers)
            dlss_jitter_now(&u.hand[2]);
        g_night_now = s->night, g_day_night_now = s->day_night;
        gfx_player_light(s, g_fxs.player_light, g_fxs.player_light_radius, g_fxs.player_light_warm, g_fxs.player_light_day,
                         u.plight, u.plight2);
        /* reflections and bounce light: a pass of their own after the composite */
        u.refl[0] = g_fxs.refl, u.refl[1] = fminf(fmaxf(g_fxs.refl_wet, 0.0f), 1.0f), u.refl[2] = fminf(fmaxf(g_fxs.refl_rough, 0.0f), 1.0f);
        u.refl[3] = fmaxf(g_fxs.refl_dist, 1.0f);
        u.elight2[1] = fminf(fmaxf(g_fxs.refl_bump, 0.0f), 2.0f); /* (elight2.x is the light's, set where it runs) */
        /* quality: low, medium, high, ultra, max - the reflections' (refl_quality) by the level; the bounce light's (gi_quality,
         * 0 low to 4 max, anywhere between: its panel's slider has twelve places) read straight between these tables'
         * entries. It takes fewer samples a frame where its temporal pass gathers them over frames (GI_SAMPLES_T); where the
         * pattern is held still (gi_still, as the occlusion's) the full count, which the blur averages */
        static const float REFL_STEPS[5] = { 16, 28, 48, 80, 128 }, GI_SAMPLES[5] = { 6, 10, 16, 24, 40 },
            GI_SAMPLES_T[5] = { 4, 6, 8, 10, 14 }, BLUR_TAPS[5] = { 4, 4, 4, 8, 12 };
        const int rq = fx_level(g_fxs.refl_quality);
        const float gqf = fminf(fmaxf(g_fxs.gi_quality, 0.0f), 4.0f);
        const int gi_temporal = g_fxs.temporal > 0.0f && !gathers;
        /* the bounce light's pattern held still, as the occlusion's (u.aoq.w, fx_gi): DLSS or FSR gathering the frames takes a
         * new pattern each frame for detail and keeps it, a shimmer; and so does our own temporal pass with too few frames to
         * even it out (under 0.9) */
        const int gi_still = gathers || g_fxs.temporal < 0.9f;
        /* the bounce light's layer: about 1000 pixels across (bloom's size; a quarter of a 4096 scene, half of 1920) up to
         * medium, then finer with the quality, twice as fine at max: it is soft, and its edges follow the depth where it is
         * laid over the scene (gi_at) */
        const float gdiv = fmaxf((float)bdiv * exp2f(-fminf(fmaxf((gqf - 1.0f) / 3.0f, 0.0f), 1.0f)), 1.0f);
        const uint32_t gw = (uint32_t)ceilf(vw / gdiv), gh = (uint32_t)ceilf(vh / gdiv);
        u.bounce[0] = (float)gw, u.bounce[1] = (float)gh;
        u.refl2[0] = REFL_STEPS[rq];
        u.refl2[1] = g_fxs.refl_sun, u.refl2[2] = g_fxs.gi;
        /* ... the reflections' pattern turning from frame to frame only where DLSS gathers the frames (else it would shimmer);
         * the bounce light's turns with its temporal pass's (u.hist.y) unless held still (gi_still) */
        u.refl2[3] = gathers ? (float)fmod((double)g_serial * 0.6180339887, 1.0) : 0.0f;
        /* (the temporal pass gathers the frames' samples, so it takes the smaller count; held still, nothing does) */
        u.refl3[0] = fmaxf(g_fxs.gi_radius, 0.2f),
        u.refl3[1] = floorf(fx_lerp5(gi_temporal && !gi_still ? GI_SAMPLES_T : GI_SAMPLES, gqf) + 0.5f);
        u.refl3[2] = BLUR_TAPS[rq];
        /* bit 0: no reflections on green ground (the pixel's colour), always; bit 1: VanaCore's grass drawn this scene, the world
         * traced from the depth before it (gfx_scene_world_depth, input 0) and the depth with it input 9, so the grass neither
         * takes a reflection nor shows in one; over them, the kinds of ground the reflections keep to by the zone's floors
         * (refl_floors; the layer drawn this frame: a bit each, 1 object, 2 path, 4 grass, 8 sand, 16 snow, 32 stone, 64 metal,
         * 128 wood, 256 shallow water, 512 deep), none without the floors (a zone whose collision isn't read: the colour alone) */
        {
            const int fm = (int)g_fxs.refl_floors;
            const int fresh = g_fx.floors_serial == g_serial && g_fx.floors_id && g_fx.floors_id->srv >= 0;
            const int layer = fm >= 1 && fresh && g_fx.floors_state == 1 && g_fx.floors_depth && g_fx.floors_depth->srv >= 0;
            const int grass = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial && g_fx.wdepth->srv >= 0;
            const int kinds = !layer ? 0 : fm == 1 ? 32 | 64 : fm == 2 ? 32 | 64 | 128 : 32 | 64 | 128 | 1;
            u.refl3[3] = 1.0f + (grass ? 2.0f : 0.0f) + 4.0f * (float)kinds;
            /* the floors asked for and not read yet (a zone just entered, reflections just turned on): no reflections until they
             * are, not a moment of them on every ground */
            if (fm >= 1 && !layer && !(g_fx.floors_serial == g_serial && g_fx.floors_state == 2))
                u.refl[0] = 0.0f;
            gfx_watch_value("reflections: floors layer ready", (float)layer);
            gfx_watch_value("reflections: strength", u.refl[0]);
            gfx_watch_value("grass: drawn before the scene's depth", (float)grass);
        }
        /* the rain's wet ground (rain.reflect): the reflections in wet patches (fx_ssr, by the mask, read by its heap slot);
         * with them off, none */
        u.rain[0] = u.rain[1] = u.rain[2] = u.rain[3] = 0.0f;
        if (u.refl[0] > 0.0f && rain_reflects() && g_fx.rain_serial == g_serial && g_fx.rain_mask->srv >= 0)
        {
            const int fm = (int)g_fxs.refl_floors;
            const int waiting = fm >= 1 && !((int)(u.refl3[3] + 0.5f) >> 2) && !(g_fx.floors_serial == g_serial && g_fx.floors_state == 2);
            if (!waiting)
            {
                u.rain[0] = g_fx.rain_wet, u.rain[1] = g_fx.rain_reflect, u.rain[2] = g_fx.rain_puddles;
                u.rain[3] = (float)(g_fx.rain_mask->srv + 1);
            }
        }
        gfx_watch_value("reflections: the rain's wetness", u.rain[0]);
        gfx_watch_value("occlusion: strength (eased in)", u.ao[1]);
        /* few samples a frame, more noise: the bounce light's smoothing reaches twice as far apart (u.bounce.z) and its history
         * weighs more (fx_gitemp) the fewer there are */
        u.bounce[2] = u.refl3[1] <= 6.0f ? 2.0f : 1.0f;
        u.bounce[3] = fminf(fmaxf(g_fxs.refl_lift, 0.0f), 1.0f); /* (the reflections' lift: fx_ssr) */
        u.clar[0] = g_fxs.clarity;
        /* the scene's smooth normals (vn_replay) beside the detail normals its normal-mapped draws made (nout_begin), readable
         * now and cleared again by the next ones; against the depth before VanaCore's grass when it was drawn, so the ground
         * under it has one too (fx_ao's grass takes the ground's) */
        const int pre_grass = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial && g_fx.wdepth->srv >= 0;
        const int vn = vn_replay(color, pre_grass ? g_fx.wdepth : depth, s);
        g_zn_depth = vn ? (pre_grass ? g_fx.wdepth : depth) : NULL; /* (zn_draw's, once the sun's maps are drawn) */
        gfx_watch_value("smooth normals: drawn this scene", (float)vn);
        gfx_watch_value("smooth normals: detail target valid", (float)(g_nd_valid != 0));
        gfx_watch_value("casters: kept for the sun's maps", (float)(g_ncasters > 0));
        if (g_nd_valid && g_nd.res && g_nd.srv >= 0)
        {
            fx_state(&g_nd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            u.clar[1] = (float)g_nd.srv;
            u.clar[2] = g_fxs.normalmap_refl >= 0.5f ? 1.0f : 0.0f;
            u.clar[3] = g_fxs.normalmap_fx >= 0.5f ? 1.0f : 0.0f;
            u.sunuv[3] = vn ? 1.0f : 0.0f; /* ... and the smooth ones among them */
        }
        g_nd_valid = 0, g_nd_clear = 1;
        /* the projection the next scene's smooth normals are taken into view space by */
        if (s->proj[0] != 0.0f && s->proj[5] != 0.0f && s->proj[11] != 0.0f)
            g_vn_f[0] = 1.0f / s->proj[0], g_vn_f[1] = 1.0f / s->proj[5], g_vn_f[2] = s->proj[11] < 0.0f ? -1.0f : 1.0f;
#endif
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
            const float* fogwant = s->fogcolor;
#ifdef VANACORE
            const float fogcustom[3] = { g_fxs.fog_cr, g_fxs.fog_cg, g_fxs.fog_cb };
            if (g_fxs.fog_mode == 2.0f)
                fogwant = fogcustom;
#endif
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
            u.fogp[0] = g_fxs.fog_falloff, u.fogp[1] = g_fxs.fog_max, u.fogp[2] = g_fxs.fog_sun;
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
             * gfx_sun_light, the same for Direct3D 11) */
            const GfxSunLightIn in = { s, g_fx.sunw, g_fx.up, g_fxs.sun_free, g_fxs.sun_free_az, g_fxs.sun_free_el, g_fxs.sun_dusk,
                g_fxs.sun_direct, g_fxs.moghouse, own && !held, g_fx.eased, g_fxs.sun_tilt, g_fxs.sun_moon };
            GfxSunLight sl = { { 0 } };
            sl.day = day, sl.direct = g_fx.direct;
            gfx_sun_light(&in, &sl);
            srec_light(&sl, in.measured);
            const float* sw = sl.sw;
            day = sl.day;
            if (in.measured)
                g_fx.direct = sl.direct, g_fx.tr_dl = sl.dl, g_fx.tr_al = sl.al;
            gfx_sun_shadow_dir(u.sunsh, sw, s->view);
            u.sunsh[3] = 1.0f;
            gfx_watch_value("sun: shadows' light z (after the dusk clamp), in millionths", sw[2] * 1e6f);
            seek_stretch_note(sl.height); /* (the caster pass's stretch, and the watch value) */
            /* sunsh.w: how much the sun lights the world (the glint's share; the shadows' own strength is below) */
            u.sunsh[3] = day * g_fx.shadows_in;
            /* (contact shadows, the shader's short ray toward the sun: gone, 2026-11-02 - strength 0 leaves the ray untaken) */
            u.shadow[0] = 0.0f, u.shadow[1] = 0.1f, u.shadow[2] = 0.3f, u.shadow[3] = 40.0f;
            gpu_scope(GS_SUN);
            float sday = 0.0f;
            if (g_fxs.sun > 0.0f && day > 0.0f && sun_map(s, sw, &u))
                sday = g_fxs.sun * day, g_fx.st_drawn_this = 1;
            /* sun_soft_chars: the shadows on characters softer than the rest (fx_ao, sun_char) */
            if (sday > 0.0f && g_fxs.sun_soft_chars > 0.0f)
                sun_map_view(s, &u);
            u.smap[0] = sday;
            u.smap[0] *= g_fx.shadows_in;
            /* ... and over the smooth normals the zone's own curved surfaces (zn_draw), where the maps are drawn */
            u.aoc2[3] = u.smap[0] > 0.0f && g_zn_depth && zn_draw(color, g_zn_depth, s) ? (float)(g_nh.srv + 1) : 0.0f;
            g_zn_depth = NULL;
            prof_mark(2);
            gpu_scope(GS_SETUP);
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
        int use_cm = 0; /* the occlusion's share on characters by the stencil's mask (cmask_pass), not their depth drawn again */
#ifdef VANACORE
        /* outline_width_chars: the characters' own lines. Which pixels are a character's is the characters' depth from the camera
         * (sun_map_view, the soft shadows' too): drawn here when the shadows haven't (their draws kept for it with the shadows
         * off, caster_wanted), and without turning their blur on */
        int char_lines = 0, char_ao = 0;
        {
            /* ... and ao_chars' (the occlusion's own strength on characters), from the same depth - or, where the stencil can
             * tell them (cmask_usable: their draws marked it), from that: exactly, with no depth to draw again */
            const int want_lines = g_fxs.outline > 0.0f && g_fxs.outline_width_chars > 0.0f;
            const int want_ao = (g_fxs.ao > 0.0f && g_fxs.ao_chars != 1.0f) || (int)g_fxs.debug == 19; /* 19: Render visual's mask */
            use_cm = cmask_usable(depth); /* (the smoothing asks for it too) */
            const int old_ao = want_ao && !use_cm;
            if ((want_lines || old_ao) && have_v)
            {
                if (u.smapn2[2] > 0.0f)
                    char_lines = want_lines, char_ao = old_ao;
                else if (sun_map_view(s, &u))
                    char_lines = want_lines, char_ao = old_ao, u.smapn2[2] = 0.0f, u.smapn2[3] = 0.0f;
            }
        }
#endif
        /* the frame before, for the temporal passes (the occlusion's and the bounce light's): its camera, if it was the
         * frame just before, at this size, and not a jump away */
        {
            /* the camera the frame before: known, the frame just before this one, and not a jump away */
            int cam = have_v && g_fx.prev_serial && g_fx.prev_serial + 1 == g_serial;
            if (cam)
            {
                float dx = vinv[12] - g_fx.prev_cam[0], dy = vinv[13] - g_fx.prev_cam[1], dz = vinv[14] - g_fx.prev_cam[2];
                cam = dx * dx + dy * dy + dz * dz < 25.0f;
            }
            /* the occlusion's history: written the frame before, at this size */
            const int recent = g_fx.hist_serial && g_fx.hist_serial + 1 == g_serial;
            int ok = recent && g_fx.hist[0].res && g_fx.hist[0].w == aw && g_fx.hist[0].h == ah && have_v;
            g_fx.tr_hist = g_fxs.temporal <= 0.0f ? 5 : !recent ? 2 : !ok ? 3 : !cam ? 4 : 1;
            ok = ok && cam;
            if (cam)
            {
                float m[16];
                mat_mul(m, vinv, g_fx.prev_view);
                mat_mul(u.reproj, m, g_fx.prev_proj);
            }
            u.hist[0] = ok ? 1.0f : 0.0f;
            /* the bounce light's history, likewise (u.hist.w) */
            u.hist[3] = cam && g_fx.gih_serial && g_fx.gih_serial + 1 == g_serial && g_fx.gih[0].res &&
                    g_fx.gih[0].w == (uint32_t)u.bounce[0] && g_fx.gih[0].h == (uint32_t)u.bounce[1] ? 1.0f : 0.0f;
            /* the patterns' turn: a golden-ratio step each frame, for the temporal pass to even out
             * (without it, the same every frame) */
            u.hist[1] = g_fxs.temporal > 0.0f ? (float)fmod((double)g_serial * 0.6180339887, 1.0) : 0.0f;
            u.hist[2] = fminf(fmaxf(g_fxs.temporal, 0.0f), 0.99f);
            u.aoq[0] = AO_TAPS[fx_level(g_fxs.ao_quality)];
            u.aoq[1] = g_fxs.sun_soft <= 0.0f ? 1.0f : 0.0f;
            /* the depth buffer's step (fx_ao: the normal and the taps keep clear of what a step is) */
            u.aoq[2] = depth->fmt == F_D16 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
            /* the occlusion's pattern held still (fx_ao): DLSS or FSR gathers the frames, or the temporal pass too few to even
             * out its 16 steps (a new grain each frame, a shimmer); 2 where they gather, the sun's shadows' held still too */
            u.aoq[3] = gathers ? 2.0f : g_fxs.temporal < 0.9f ? 1.0f : 0.0f;
            /* the occlusion's rings (fx_ao): fewer taps past each, ao_ring_spacing world units apart */
            u.aor[0] = g_fxs.ao_rings > 0.0f ? 1.0f : 0.0f;
            u.aor[1] = fmaxf(g_fxs.ao_ring_spacing, 1.0f);
            /* ... and the sun's shadows' (sun_look): fewer of the maps' taps past each, sun_ring_spacing apart */
            u.aor[2] = g_fxs.sun_rings > 0.0f ? 1.0f : 0.0f;
            u.aor[3] = fmaxf(g_fxs.sun_ring_spacing, 1.0f);
            zone_lines(s, have_v ? vinv + 12 : NULL, &u);
            sparkles(s, have_v ? vinv + 12 : NULL, &u);
            ground_marks(s, have_v ? vinv + 12 : NULL, &u);
#ifdef VANACORE
            /* the lines go over the scene after the game's late world draws (zl_run), not into this compose, where
             * what the zone draws after it covered them: kept aside, the compose given none (it has no code for them,
             * ZL_LATE: without the late pass's pipeline there are no lines). The sparkles go with them (u.spc, left as
             * it is: the compose has no code for them either), and the rings on the ground (u.gmc) */
            zl_late = (u.zlc[0] > 0.0f || u.spc[0] > 0.0f || u.gmc[0] > 0.0f) && g_fx.by_fmt[fmt].zlate;
            if (zl_late)
            {
                memcpy(zl_zlc, u.zlc, sizeof zl_zlc), memcpy(zl_zlcol, u.zlcol, sizeof zl_zlcol);
                memcpy(zl_zlp, u.zlp, sizeof zl_zlp), memcpy(zl_zl, u.zl, sizeof zl_zl);
                u.zlc[0] = 0.0f;
            }
#endif
            memcpy(g_fx.prev_view, s->view, 64), memcpy(g_fx.prev_proj, s->proj, 64);
            g_fx.prev_serial = g_serial;
            if (have_v)
                memcpy(g_fx.prev_cam, vinv + 12, 12);
        }
        u.bloom[0] = g_fxs.threshold, u.bloom[1] = g_fxs.bloom, u.bloom[2] = 0.25f;
        u.rays[0] = u.sunuv[2] > 0.0f ? g_fxs.rays : 0.0f, u.rays[1] = g_fxs.rays_decay, u.rays[2] = g_fxs.rays_length;
#ifdef VANACORE
        u.rays[3] = use_cm ? g_fx_rain : 0.0f, g_fx_rain = s->rain_sheen; /* the rain's reflection on the characters (fx_ssr) */
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
        u.toon3[0] = g_fxs.grade <= 0.0f ? 0.0f : g_fxs.smooth > 1.0f ? 1.0f : g_fxs.smooth; /* the colour grade's: off with it */
        u.toon3[1] = sr < 2.0f ? 2.0f : sr > 6.0f ? 6.0f : sr;
        /* ... and VanaCore's grass left out of the lines, given this scene's depth before it (gfx_scene_world_depth) */
        u.toon3[2] = g_fxs.outline_nograss != 0.0f && g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial ? 1.0f : 0.0f;
        /* ... and the characters' lines' width, the same way (char_lines: their depth is there) */
        {
            float cw = g_fxs.outline_width_chars * vh / 1080.0f;
            u.toon3[3] = char_lines ? (cw < 1.0f ? 1.0f : cw) : 0.0f;
        }
        u.aoc[0] = fminf(fmaxf(g_fxs.ao_chars, 0.0f), 2.0f), u.aoc[1] = char_ao ? 1.0f : 0.0f, u.aoc[3] = use_cm ? 1.0f : 0.0f;
        /* z: the zone's shadows blurred this far across its surfaces (fx_blur), world units; only where the sun's maps are drawn */
        u.aoc[2] = g_fxs.sun > 0.0f ? fminf(fmaxf(g_fxs.sun_soft_zone, 0.0f), 2.0f) : 0.0f;
        u.aoc2[0] = use_cm ? fminf(fmaxf(g_fxs.ao_soft_chars, 0.0f), 2.0f) : 0.0f;
        /* the shader's parts of the sun turned off (sun_gate); and, with sun_game_shading 0 (the default), the two that weigh the
         * shadows against the game's own shading (its smooth normals: the shadow taken off a surface the game turns from its
         * sun, and a face's second look past its own rock): with them the maps' shadows were lost wherever the game's light and
         * the shadows' parted (a sun placed by hand, sun_tilt, the dusk clamp) - a statue's shadow in the maps, none on the
         * ground (Lower Jeuno, 2026-11-04), the tops of walls under a ceiling lit. Off, 256: no second look and no shadow taken
         * off by the game's shading (sun_map) - the smooth normals stay for the shading by the angle (sun_face), which by the
         * polygon's plane came out a flat grey a triangle (the Crag of Dem, 2026-11-11) */
        u.aoc2[2] = (float)((sun_gate() & 255u) | (g_fxs.sun_game_shading == 0.0f ? 256u : 0u));
        dof_params(&u, vh, s);
#endif
        g_fx.tr_rays = u.rays[0] * u.sunuv[2];
        if (!(u.ao[1] > 0.0f || u.smap[0] > 0.0f))
            g_fx.tr_hist = 0;
        const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (fx_tex(&g_fx.src, color->dxfmt, color->w, color->h) && fx_tex(&g_fx.ao0, RGBA, aw, ah) && fx_tex(&g_fx.ao1, RGBA, aw, ah) &&
            fx_tex(&g_fx.b1a, RGBA, bw, bh) && fx_tex(&g_fx.b1b, RGBA, bw, bh) && fx_tex(&g_fx.b2a, RGBA, (bw + 1) / 2, (bh + 1) / 2) &&
            fx_tex(&g_fx.b2b, RGBA, (bw + 1) / 2, (bh + 1) / 2) && fx_tex(&g_fx.ra, RGBA, bw, bh) && fx_tex(&g_fx.rb, RGBA, bw, bh) &&
            (!use_cm || fx_tex(&g_fx.cmask, DXGI_FORMAT_R8_UNORM, color->w, color->h)))
        {
            ID3D12GraphicsCommandList* l = list();
            g_fx.tr_ran = 1;
            int lz_ok = 0; /* the occlusion's depth and its levels (fx_lz) built this frame: the bounce light reads them too */
            /* the scene as it was, which the passes read while the last one draws over it */
            gpu_part(GP_COPY);
            tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
            fx_state(&g_fx.src, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION to, from;
            memset(&to, 0, sizeof to);
            memset(&from, 0, sizeof from);
            from.pResource = color->res, from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, from.SubresourceIndex = 0;
            to.pResource = g_fx.src.res, to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, to.SubresourceIndex = 0;
            ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &from, NULL);
            fx_state(&g_fx.src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            tex_state(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            FxTex* ao_out = &g_fx.ao0;
            gpu_scope(GS_AO);
#ifdef VANACORE
            if (use_cm) /* the characters' mask, from the stencil their draws wrote: fx_ao's input 11 (and Render visual 19's) */
            {
                gpu_part(GP_AO);
                cmask_pass(depth, &g_fx.cmask);
                g_fx.cmask_serial = g_serial;
            }
#endif
            if (u.ao[1] > 0.0f || u.smap[0] > 0.0f)
            {
                sun_state(&g_fx.smap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                sun_state(&g_fx.smapn, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                sun_state(&g_fx.smapm, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                sun_state(&g_fx.smapv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                const int sunm = u.smap[0] > 0.0f;
                /* the world without VanaCore's grass (its depth before the grass, gfx_scene_world_depth: this scene's), and the
                 * depth with it last (fx_ao) */
                GfxTex* world = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial ? g_fx.wdepth : depth;
                if (world != depth)
                    tex_state(world, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                /* input 10: the characters' depth from the camera (sun_map_view), with sun_soft_chars */
                /* input 11: the characters' mask from the stencil (cmask_pass), for ao_chars' share; input 7: the middle map (sun_mid) */
                if (!sunm)
                    u.smapn2[2] = 0.0f, u.smapm2[1] = 0.0f;
                int32_t ao_in[12] = { world->srv, sunm ? g_fx.smap.srv : -1, sunm && u.smapn2[1] > 0.0f ? g_fx.smapn.srv : -1,
                    -1, -1, -1, -1, u.smapm2[1] > 0.0f ? g_fx.smapm.srv : -1, depth->srv, -1,
                    u.smapn2[2] > 0.0f ? g_fx.smapv.srv : -1, use_cm ? g_fx.cmask.srv : -1 };
                if (u.ao[1] > 0.0f && fx_lz(aw, ah))
                {
                    gpu_part(GP_AO_DEPTH);
                    fx_pass(&g_fx.lz[0], FX_LINZ, &u, &world->srv, 1, 0, 0);
                    for (int lv = 1; lv < 4; ++lv)
                        fx_pass(&g_fx.lz[lv], FX_ZMIP, &u, &g_fx.lz[lv - 1].srv, 1, 0, 0);
                    for (int lv = 0; lv < 4; ++lv)
                        ao_in[3 + lv] = g_fx.lz[lv].srv;
                    lz_ok = 1;
                }
                else
                    u.ao[1] = 0.0f; /* no depth for it: none this frame */
                if (s && s->water_noshadow && g_snap_depth && g_snap_serial == (uint32_t)g_serial && g_snap_depth->w == depth->w &&
                    g_snap_depth->h == depth->h && g_snap_depth->srv >= 0) /* input 9: the depth before the water (fx_ao's water mask) */
                {
                    tex_state(g_snap_depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    ao_in[9] = g_snap_depth->srv, u.aoc2[2] = (float)((uint32_t)u.aoc2[2] | 512u);
                }
                gpu_part(GP_AO);
                fx_pass(&g_fx.ao0, FX_AO, &u, ao_in, 12, 0, 0);
                gpu_part(GP_AO_BLUR);
                /* input 10 the characters' depth (sun_soft_chars: fx_blur blurs the shadows on them), 11 their mask (ao_soft_chars) */
                int32_t blur_in[12] = { g_fx.ao0.srv, -1, -1, -1, -1, -1, -1, -1, -1, -1, ao_in[10], ao_in[11] };
                fx_pass(&g_fx.ao1, FX_BLUR, &u, blur_in, 12, 1, 0);
                blur_in[0] = g_fx.ao1.srv;
                fx_pass(&g_fx.ao0, FX_BLUR, &u, blur_in, 12, 0, 1);
                if (g_fxs.temporal > 0.0f && !gathers && fx_tex(&g_fx.hist[0], RGBA, aw, ah) && fx_tex(&g_fx.hist[1], RGBA, aw, ah))
                {
                    gpu_part(GP_AO_TEMPORAL);
                    int to_i = g_fx.hist_at ^ 1;
                    fx_state(&g_fx.hist[g_fx.hist_at], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    int32_t t_in[2] = { g_fx.ao0.srv, g_fx.hist[g_fx.hist_at].srv };
                    fx_pass(&g_fx.hist[to_i], FX_TEMPORAL, &u, t_in, 2, 0, 0);
                    ao_out = &g_fx.hist[to_i];
                    g_fx.hist_at = to_i, g_fx.hist_serial = g_serial;
                }
                /* ao_soft_chars: the smoothing on characters, from what the passes left (the history stays as it was), into ao0 */
                if (use_cm && u.aoc2[0] > 0.0f && u.ao[1] > 0.0f)
                {
                    FxU us = u;
                    us.aoc2[1] = 1.0f;
                    int32_t soft_in[12] = { ao_out->srv, -1, -1, -1, -1, -1, -1, -1, -1, -1, ao_in[10], ao_in[11] };
                    gpu_part(GP_AO_BLUR);
                    fx_pass(&g_fx.ao1, FX_BLUR, &us, soft_in, 12, 1, 0);
                    soft_in[0] = g_fx.ao1.srv;
                    fx_pass(&g_fx.ao0, FX_BLUR, &us, soft_in, 12, 0, 1);
                    ao_out = &g_fx.ao0;
                }
            }
            gpu_scope(GS_BLOOM);
            if (u.bloom[1] > 0.0f)
            {
                gpu_part(GP_BLOOM_BRIGHT);
                /* the occlusion and the shadows too when they were drawn (fx_bright shades by them) */
                const int shaded = u.ao[1] > 0.0f || u.smap[0] > 0.0f;
                int32_t bright_in[2] = { g_fx.src.srv, ao_out->srv };
                fx_pass(&g_fx.b1a, FX_BRIGHT, &u, bright_in, shaded ? 2 : 1, 0, 0);
                gpu_part(GP_BLOOM_BLUR);
                fx_pass(&g_fx.b1b, FX_GAUSS, &u, &g_fx.b1a.srv, 1, 2, 0);
                fx_pass(&g_fx.b1a, FX_GAUSS, &u, &g_fx.b1b.srv, 1, 0, 2);
                gpu_part(GP_BLOOM_EIGHTH);
                fx_pass(&g_fx.b2a, FX_DOWN, &u, &g_fx.b1a.srv, 1, 0, 0);
                fx_pass(&g_fx.b2b, FX_GAUSS, &u, &g_fx.b2a.srv, 1, 2, 0);
                fx_pass(&g_fx.b2a, FX_GAUSS, &u, &g_fx.b2b.srv, 1, 0, 2);
            }
            gpu_scope(GS_RAYS);
            if (u.rays[0] > 0.0f)
            {
                int32_t mask_in[2] = { g_fx.src.srv, depth->srv };
                gpu_part(GP_RAYS_MASK);
                fx_pass(&g_fx.ra, FX_RAYMASK, &u, mask_in, 2, 0, 0);
                gpu_part(GP_RAYS);
                fx_pass(&g_fx.rb, FX_RAYS, &u, &g_fx.ra.srv, 1, 0, 0);
                gpu_part(GP_RAYS_BLUR);
                fx_pass(&g_fx.ra, FX_GAUSS, &u, &g_fx.rb.srv, 1, 1, 0);
                fx_pass(&g_fx.rb, FX_GAUSS, &u, &g_fx.ra.srv, 1, 0, 1);
            }
            gpu_scope(GS_COMPOSITE);
            /* every input readable, including the ones this frame skipped */
            FxTex* all[] = { ao_out, &g_fx.ao0, &g_fx.b1a, &g_fx.b2a, &g_fx.rb };
            for (int i = 0; i < 5; ++i)
                fx_state(all[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            float vp[4] = { vx, vy, vw, vh };
            /* the scene the composite starts from: smoothed for the cel look */
            int32_t base = g_fx.src.srv;
            if (u.toon3[0] > 0.0f && g_fx.by_fmt[fmt].smooth && fx_tex(&g_fx.sm, color->dxfmt, color->w, color->h))
            {
                fx_state(&g_fx.sm, D3D12_RESOURCE_STATE_RENDER_TARGET);
                gpu_part(GP_SMOOTH);
                fx_draw(heap_cpu(&g_rtv, g_fx.sm.rtv), g_fx.sm.w, g_fx.sm.h, g_fx.by_fmt[fmt].smooth, vp, &u, &g_fx.src.srv, 1, 0, 0);
                gpu_part(GP_REST);
                fx_state(&g_fx.sm, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                base = g_fx.sm.srv;
            }
            int32_t comp_in[11] = { g_fx.src.srv, ao_out->srv, depth->srv, g_fx.b1a.srv, g_fx.b2a.srv, g_fx.rb.srv, base, -1, -1, -1, -1 };
            int comp_n = 8;
#ifdef VANACORE
            /* the lines without the grass: the depth before it, the ninth input; Render visual 26 (the grass in red) too, the depth
             * itself when this scene has none before it (no grass drawn: nothing differs) */
            if (u.toon3[2] > 0.0f || (int)g_fxs.debug == 26)
            {
                GfxTex* before = g_fx.wdepth && g_fx.wdepth_of == depth && g_fx.wdepth_serial == g_serial ? g_fx.wdepth : depth;
                if (before != depth)
                    tex_state(before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                comp_in[8] = before->srv, comp_n = 9;
            }
            /* ... and the characters' depth for their lines, and the occlusion's strength on them, the tenth */
            if (u.toon3[3] > 0.0f || u.aoc[1] > 0.0f)
            {
                sun_state(&g_fx.smapv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                comp_in[9] = g_fx.smapv.srv, comp_n = 10;
            }
            /* ... or, for Render visual 19 with the stencil's mask, that, the eleventh; for 27, the far map itself */
            if (use_cm && ((int)g_fxs.debug == 19 || u.rays[3] > 0.0f))
                comp_in[10] = g_fx.cmask.srv, comp_n = 11;
            if ((int)g_fxs.debug == 27 && g_fx.smap.res)
            {
                sun_state(&g_fx.smap, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                comp_in[10] = g_fx.smap.srv, comp_n = 11;
            }
#endif
#ifdef VANACORE
            /* fog_color=sky: the sky's colour for the height fog, one texel from this frame's scene (and the last frame's,
             * to ease from), read by the composite as its eighth input */
            if (g_fxs.fog_mode == 1.0f && g_fxs.fog > 0.0f && g_fx.skycol_pso && fx_tex(&g_fx.sky[0], RGBA, 1, 1) &&
                fx_tex(&g_fx.sky[1], RGBA, 1, 1))
            {
                FxTex* last_sky = &g_fx.sky[g_fx.sky_i];
                FxTex* sky = &g_fx.sky[g_fx.sky_i ^ 1];
                g_fx.sky_i ^= 1;
                fx_state(last_sky, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                fx_state(sky, D3D12_RESOURCE_STATE_RENDER_TARGET);
                float vp1[4] = { 0, 0, 1, 1 };
                int32_t sky_in[3] = { g_fx.src.srv, depth->srv, last_sky->srv };
                gpu_part(GP_SKY);
                fx_draw(heap_cpu(&g_rtv, sky->rtv), 1, 1, g_fx.skycol_pso, vp1, &u, sky_in, 3, 0, 0);
                gpu_part(GP_REST);
                fx_state(sky, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                comp_in[7] = sky->srv;
                u.grade2[3] = 1.0f;
            }
#endif
            tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            gpu_part(GP_COMP);
            fx_draw(target_view(color, 0, 0), color->w, color->h, g_fx.by_fmt[fmt].comp, vp, &u, comp_in, comp_n, 0, 0);
            gpu_part(GP_REST);
#ifdef VANACORE
            comp_measure(color, fmt, vp, &u, comp_in, comp_n);
            /* the colour grade, which the composite doesn't do, is laid last: once the game is done drawing into the scene */
            grade_last = grade_wanted(fmt);
            /* clarity: the composite's lead on its own blur, as a gain (a copy to read, its blur at about a quarter size); the
             * colour grade's, off with it */
            if (g_fxs.clarity > 0.0f && g_fxs.grade > 0.0f && g_fx.by_fmt[fmt].clarity && g_fx.pso[FX_LOWPASS] && fx_tex(&g_fx.cl[0], RGBA, bw, bh) &&
                fx_tex(&g_fx.cl[1], RGBA, bw, bh))
            {
                gpu_scope(GS_REFL);
                gpu_part(GP_COPY);
                tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
                fx_state(&g_fx.src, D3D12_RESOURCE_STATE_COPY_DEST);
                ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &from, NULL);
                fx_state(&g_fx.src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                gpu_part(GP_CLARITY_BLUR);
                fx_pass(&g_fx.cl[0], FX_LOWPASS, &u, &g_fx.src.srv, 1, 0, 0);
                fx_pass(&g_fx.cl[1], FX_GAUSS, &u, &g_fx.cl[0].srv, 1, 2, 0);
                fx_pass(&g_fx.cl[0], FX_GAUSS, &u, &g_fx.cl[1].srv, 1, 0, 2);
                tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                int32_t clar_in[2] = { g_fx.src.srv, g_fx.cl[0].srv };
                gpu_part(GP_CLARITY);
                fx_draw(target_view(color, 0, 0), color->w, color->h, g_fx.by_fmt[fmt].clarity, vp, &u, clar_in, 2, 0, 0);
                gpu_scope(GS_COMPOSITE);
            }
            /* reflections and bounce light over the composite: a copy of it to read; the reflections in a layer at the
             * occlusion's size (the scene's; fx_ssr), the bounce light in one of its own at about 1000
             * across (fx_gi: its samples from the occlusion's depth levels and its colours from a copy of the scene at its size,
             * blended with the frame before where the temporal pass runs), then both laid over the scene at its size (fx_refl),
             * the layers' texels weighted by how near their distance is to each pixel's */
            const int ssr = u.refl[0] > 0.0f, gi = u.refl2[2] > 0.0f;
            if ((ssr || gi) && g_fx.by_fmt[fmt].refl && g_fx.pso[FX_SSR] && g_fx.pso[FX_GI] && g_fx.pso[FX_GITEMP] &&
                (!gi || g_fx.pso[FX_LOWPASS]) && (lz_ok || fx_lz(aw, ah)) && (!ssr || fx_tex(&g_fx.rl, RGBA, aw, ah)) &&
                (!gi || (fx_tex(&g_fx.gi, RGBA, (uint32_t)u.bounce[0], (uint32_t)u.bounce[1]) &&
                         fx_tex(&g_fx.gic, RGBA, (uint32_t)u.bounce[0], (uint32_t)u.bounce[1]))))
            {
                gpu_scope(GS_REFL);
                gpu_part(GP_COPY);
                tex_state(color, D3D12_RESOURCE_STATE_COPY_SOURCE);
                fx_state(&g_fx.src, D3D12_RESOURCE_STATE_COPY_DEST);
                ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &from, NULL);
                fx_state(&g_fx.src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                if (!lz_ok)
                {
                    gpu_part(GP_AO_DEPTH);
                    fx_pass(&g_fx.lz[0], FX_LINZ, &u, &depth->srv, 1, 0, 0);
                    for (int lv = 1; lv < 4; ++lv)
                        fx_pass(&g_fx.lz[lv], FX_ZMIP, &u, &g_fx.lz[lv - 1].srv, 1, 0, 0);
                    lz_ok = 1;
                }
                int32_t lay_in[7] = { depth->srv, g_fx.src.srv, ao_out->srv, g_fx.lz[0].srv, g_fx.lz[1].srv, g_fx.lz[2].srv,
                    g_fx.lz[3].srv };
                FxTex* gi_out = &g_fx.gi;
                if (ssr)
                {
                    gpu_part(GP_SSR);
                    /* ... and the zone's floors' layer (input 7, the floors' depth 8) where the reflections keep to kinds of
                     * ground; with VanaCore's grass, the world's depth before it as input 0 and the depth with it 9 (u.refl3.w) */
                    int32_t ssr_in[11] = { lay_in[0], lay_in[1], lay_in[2], lay_in[3], lay_in[4], lay_in[5], lay_in[6], -1, -1, -1, -1 };
                    const int rw = (int)(u.refl3[3] + 0.5f);
                    if (rw >> 2)
                    {
                        tex_state(g_fx.floors_id, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        tex_state(g_fx.floors_depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        ssr_in[7] = g_fx.floors_id->srv, ssr_in[8] = g_fx.floors_depth->srv;
                    }
                    if (rw & 2)
                    {
                        tex_state(g_fx.wdepth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        ssr_in[0] = g_fx.wdepth->srv, ssr_in[9] = depth->srv;
                    }
                    if (u.rain[3] > 0.0f)
                        tex_state(g_fx.rain_mask, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    int ssr_n = rw >= 2 ? 10 : 7;
#ifdef VANACORE
                    /* the characters' mask as input 10 while the rain's reflection is on them (fx_ssr's chr) */
                    if (use_cm && u.rays[3] > 0.0f)
                    {
                        fx_state(&g_fx.cmask, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        ssr_in[10] = g_fx.cmask.srv, ssr_n = 11;
                    }
#endif
                    fx_pass(&g_fx.rl, FX_SSR, &u, ssr_in, ssr_n, 0, 0);
                }
                if (gi)
                {
                    gpu_part(GP_GI);
                    /* the scene at the layer's size, for its colours (input 1; the rest as the reflections') */
                    fx_pass(&g_fx.gic, FX_LOWPASS, &u, &g_fx.src.srv, 1, 0, 0);
                    int32_t gi_in[8] = { depth->srv, g_fx.gic.srv, ao_out->srv, g_fx.lz[0].srv, g_fx.lz[1].srv, g_fx.lz[2].srv,
                        g_fx.lz[3].srv, -1 };
#ifdef VANACORE
                    /* the characters' mask (this frame's) as input 7: they are no source of bounce light */
                    if (use_cm)
                        gi_in[7] = g_fx.cmask.srv;
#endif
                    fx_pass(&g_fx.gi, FX_GI, &u, gi_in, 8, 0, 0);
                    /* smoothed over neighbours, across and down (through the copy's target, done with): the gather's few
                     * samples a texel are noisy, and this is where the shimmer is taken out before the temporal pass */
                    fx_pass(&g_fx.gic, FX_GIBLUR, &u, &g_fx.gi.srv, 1, 1, 0);
                    fx_pass(&g_fx.gi, FX_GIBLUR, &u, &g_fx.gic.srv, 1, 0, 1);
                    if (gi_temporal && fx_tex(&g_fx.gih[0], RGBA, g_fx.gi.w, g_fx.gi.h) &&
                        fx_tex(&g_fx.gih[1], RGBA, g_fx.gi.w, g_fx.gi.h))
                    {
                        gpu_part(GP_GI_TEMPORAL);
                        int to_i = g_fx.gih_at ^ 1;
                        fx_state(&g_fx.gih[g_fx.gih_at], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        int32_t t_in[2] = { g_fx.gi.srv, g_fx.gih[g_fx.gih_at].srv };
                        fx_pass(&g_fx.gih[to_i], FX_GITEMP, &u, t_in, 2, 0, 0);
                        gi_out = &g_fx.gih[to_i];
                        g_fx.gih_at = to_i, g_fx.gih_serial = g_serial;
                    }
                }
                tex_state(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                int32_t refl_in[8] = { depth->srv, g_fx.src.srv, ssr ? g_fx.rl.srv : -1, gi ? gi_out->srv : -1, g_fx.lz[0].srv, -1, -1,
                    -1 };
                int refl_n = 5;
#ifdef VANACORE
                /* the characters' mask (cmask_pass, this frame's) as input 7: the bounce light is the world's, not on them, and the
                 * rain's reflection is on them (fx_refl leaves its facing test off there) */
                if (use_cm && (gi || u.rays[3] > 0.0f))
                    refl_in[7] = g_fx.cmask.srv, refl_n = 8;
#endif
                /* Render visual 18 (the kinds of ground): the zone's floors' layer and its depth as inputs 5 and 6 */
                if ((int)u.grade[3] == 18 && ((int)(u.refl3[3] + 0.5f) >> 2))
                {
                    tex_state(g_fx.floors_id, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    tex_state(g_fx.floors_depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    refl_in[5] = g_fx.floors_id->srv, refl_in[6] = g_fx.floors_depth->srv, refl_n = refl_n < 7 ? 7 : refl_n;
                }
                gpu_part(GP_REFL);
                fx_draw(target_view(color, 0, 0), color->w, color->h, g_fx.by_fmt[fmt].refl, vp, &u, refl_in, refl_n, 0, 0);
                gpu_scope(GS_COMPOSITE);
            }
            /* depth of field, last, on the finished scene; where an upscaler runs, on its output instead (dof_after: the blur
             * made before it shimmered along its edges as the jitter moved them) */
            dof_late = u.dof[0] >= 1.0f && depth->use == GFX_USE_DEPTH && g_fx.depthup && g_fx.vs && dlss_will_run();
            if (!dof_late)
                dof_run(color, &g_fx.src, depth, fmt, &u, vp);
            /* the light from the effects: their uniforms, for el_note once the scene is finished */
            el_u = g_fxs.elight > 0.0f && g_fxs.elight_radius > 0.0f;
            if (el_u)
                memcpy(&g_el.u, &u, sizeof u);
            /* the zone lines' uniforms: the scene's as they are now, with the lines put back */
            if (zl_late)
            {
                memcpy(&g_zl.u, &u, sizeof u);
                memcpy(g_zl.u.zlc, zl_zlc, sizeof zl_zlc), memcpy(g_zl.u.zlcol, zl_zlcol, sizeof zl_zlcol);
                memcpy(g_zl.u.zlp, zl_zlp, sizeof zl_zlp), memcpy(g_zl.u.zl, zl_zl, sizeof zl_zl);
            }
#endif
        }
    }
#ifdef VANACORE
    /* anti-aliasing, last on the finished 3D scene and before it is made smaller */
    if (aa && fmt >= 0 && s->proj[11] != 0.0f)
        fx_antialias(color, fmt);
    /* the light from what the game draws into the finished scene next (el_emit, el_run); and those draws' own picture alone
     * for DLSS 5, where they're its lights (dlss5_fx_light), whether or not they light the scene themselves */
    const int el_dlss5 = dlss && g_dlss.nr && g_fxs.dlss5_fx_light != 0.0f && fmt >= 0 && s->proj[11] != 0.0f;
    if (el_u || el_dlss5)
        el_note(color, depth, fmt, s, el_u, el_dlss5);
    else
        el_off();
    /* the zone lines, over the scene once its world is drawn: the uniforms as the compose had them, with the lines back */
    g_zl.pend_serial = 0;
    if (zl_late)
        zl_note(color, depth, fmt, s);
    /* DLSS, last: the finished scene made larger, read in its place by the frame's draws (or copied at that size); and the colour
     * grade over it, or over the scene itself with no DLSS, both once the game is done drawing into it */
    if ((dlss || plain || grade_last) && fmt >= 0 && s->proj[11] != 0.0f)
    {
        g_dlss.dof_late = dof_late;
        dlss_frame(color, depth, s, fmt);
    }
#endif
    color->scene = 0; /* the effects changed it: its chain is behind (scene_mips) */
    if (gfx_profiling)
    {
        static uint64_t last;
        g_fx.st_frames++;
        g_fx.st_own += s->sun_dir[3] != 0.0f;
        g_fx.st_map += g_fx.st_drawn_this;
        g_fx.st_cmin = g_fx.st_frames == 1 || g_ncasters < g_fx.st_cmin ? g_ncasters : g_fx.st_cmin;
        g_fx.st_cmax = g_ncasters > g_fx.st_cmax ? g_ncasters : g_fx.st_cmax;
        uint64_t now = gfx_now_ns();
        if (now - last > 2000000000ull)
        {
            fprintf(stderr, "[recomp] gfx: shadows: %u frames, %u with the sun's own light, %u with a map; casters %u..%u; "
                "%u cached; map %.0f units across; sun %.2f %.2f %.2f\n", g_fx.st_frames, g_fx.st_own, g_fx.st_map, g_fx.st_cmin,
                g_fx.st_cmax, g_fx.st_cached, g_fx.st_across, g_fx.sunw[0], g_fx.sunw[1], g_fx.sunw[2]);
            last = now, g_fx.st_frames = g_fx.st_own = g_fx.st_map = g_fx.st_cmax = 0;
        }
    }
    {
        struct SunTrace* t = &g_fx.trace[g_fx.ntrace++ % 1200];
        float inv[16];
        t->serial = g_serial, t->live = g_fx.tr_live, t->cached = g_fx.tr_cached, t->skipped = g_fx.tr_skipped;
        memcpy(t->fog, g_fx.tr_fog, 12);
        t->own = s->sun_dir[3] != 0.0f, t->depth = g_fx.tr_depth, t->dl = g_fx.tr_dl, t->al = g_fx.tr_al;
        t->strength = g_fx.tr_strength, t->day = g_fx.tr_day;
        memcpy(t->sun, g_fx.sunw, 12);
        if (mat_inverse(inv, s->view))
            memcpy(t->cam, inv + 12, 12);
        t->across = g_fx.st_across;
        t->far_r = g_fx.st_far_r, t->near_r = g_fx.st_near_r, t->reach = g_fx.st_reach, t->to = g_fx.st_to;
        t->redrawn = g_fx.st_redrawn, t->why_far = g_fx.st_why_far, t->why_near = g_fx.st_why_near;
        t->ran = g_fx.tr_ran, t->hist = g_fx.tr_hist, t->held = g_fx.tr_held, t->rays = g_fx.tr_rays;
        g_fx.st_redrawn = g_fx.st_why_far = g_fx.st_why_near = 0;
        srec_frame(s, t, color); /* (a /vana stats recording's shadows file) */
        if (g_fxs.fx != 0.0f)
            fx_watch_frame(s, t, color, depth, ready, fmt);
        if (g_fx.wait_serial != g_serial) /* the frame's scenes together */
            g_fx.wait_casters = g_fx.wait_building = 0, g_fx.wait_serial = g_serial;
        g_fx.wait_casters += g_fx.tr_live + g_fx.tr_cached + g_fx.tr_skipped, g_fx.wait_building += g_fx.tr_building;
        g_fx.tr_live = g_fx.tr_cached = g_fx.tr_skipped = g_fx.tr_nou = g_fx.tr_building = 0, g_fx.tr_strength = g_fx.tr_day = 0;
        g_fx.tr_ran = g_fx.tr_hist = g_fx.tr_held = 0, g_fx.tr_rays = 0;
    }
    g_fx.st_drawn_this = 0;
    if (g_fxs.fx != 0.0f && g_fxs.sun > 0.0f)
        fx_stats();
    casters_clear();
    prof_mark(2), prof_mark(3);
    gpu_scope(GS_UI);
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0;
}

void gfx_present(GfxTex* bb)
{
    if (!g_dev)
        return;
    if (gfx_profiling) /* what this frame did, once, for the toggle finder */
    {
        gfx_watch_value("glow: laid over the scene (gfx_glow calls)", (float)g_glow_calls);
        gfx_watch_value("glow: tint's strength", g_glow_calls ? g_glow_alpha : 0.0f);
#ifdef VANACORE
        gfx_watch_value("light from the effects: lit a scene", (float)(g_el.ran_serial == g_serial));
        gfx_watch_value("light from the effects: any draw taken as a light", (float)(g_el.taken > 0));
        g_el.taken = 0;
#endif
    }
    g_glow_calls = 0;
    uint64_t present_start = gfx_profiling ? gfx_now_ns() : 0;
    g_present_thread = GetCurrentThreadId();
#ifdef VANACORE
    mo_flush(1); /* (the moving things' redraws never wait into the next frame: they're drawn from its ring) */
    /* through frame generation, its frames shown by its own thread; else as usual, once that thread's done */
    fg_watch();
    if (fg_present(bb))
    {
        if (gfx_profiling)
            prof_frame(present_start);
        return;
    }
    fg_wait_idle();
#endif
    /* the frame drawn for the window into one of g_show's textures (in turn), on the game's queue; then copied into the
     * swap chain's buffer and presented on the present queue (show_present), which waits for the drawing */
    int j = -1;
    if (g_swap && bb && bb->srv >= 0 && g_present_pso && show_init())
    {
        swap_fit();
        FxTex* t = &g_show.tex[g_show.next];
        if (fx_tex(t, DXGI_FORMAT_B8G8R8A8_UNORM, g_swap_w, g_swap_h))
        {
            j = g_show.next;
            g_show.next = (j + 1) % FRAMES;
            /* the present queue past its copy of what this texture held (FRAMES frames ago) before it's drawn into */
            if (g_show.copied[j] && g_pfence && !g_lost_said)
                ID3D12CommandQueue_Wait(g_queue, g_pfence, g_show.copied[j]);
            tex_state(bb, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            fx_state(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (g_dred)
                dred_mark(list(), GS_N);
            present_draw(heap_cpu(&g_rtv, t->rtv), t->w, t->h, bb->srv);
#ifdef VANACORE
            g_present_layer = NULL;
#endif
            fx_state(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
    fps_tick();
    fx_reload();
    frame_end();
    if (j >= 0)
    {
        uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
        g_show.copied[j] = show_present(&g_show.tex[j], g_fence_value, g_show.alloc[j], &g_show.alloc_done[j], "at Present");
        if (gfx_profiling)
            g_prof.drawable_ns += gfx_now_ns() - t0;
    }
    if (gfx_profiling)
        prof_frame(present_start);
}

void gfx_finish(void)
{
    if (!g_dev)
        return;
    BATCH_SETTLE();
    submit(1);
}

void gfx_resize(uint32_t w, uint32_t h)
{
    g_want_w = w, g_want_h = h;
}

/* --- start-up -------------------------------------------------------------------------------------------------- */
static ID3D12RootSignature* make_root(void)
{
    D3D12_DESCRIPTOR_RANGE tex2d = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0 };
    D3D12_DESCRIPTOR_RANGE texcube = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0 };
    D3D12_DESCRIPTOR_RANGE samps = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 0, 0 };
    D3D12_ROOT_PARAMETER p[ROOT_COUNT];
    memset(p, 0, sizeof p);
    p[ROOT_U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[ROOT_U].Descriptor.ShaderRegister = 0;
    p[ROOT_U].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[ROOT_BIND].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[ROOT_BIND].Constants.ShaderRegister = 1;
    p[ROOT_BIND].Constants.Num32BitValues = 16;
    p[ROOT_BIND].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        p[ROOT_STREAM0 + s].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[ROOT_STREAM0 + s].Descriptor.ShaderRegister = (UINT)s;
        p[ROOT_STREAM0 + s].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    p[ROOT_TEX2D].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_TEX2D].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_TEX2D].DescriptorTable.pDescriptorRanges = &tex2d;
    p[ROOT_TEX2D].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_TEXCUBE].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_TEXCUBE].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_TEXCUBE].DescriptorTable.pDescriptorRanges = &texcube;
    p[ROOT_TEXCUBE].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[ROOT_SAMPLERS].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_SAMPLERS].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_SAMPLERS].DescriptorTable.pDescriptorRanges = &samps;
    p[ROOT_SAMPLERS].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE sampcmp = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 1, 0 };
    p[ROOT_SUN].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[ROOT_SUN].Constants.ShaderRegister = 2;
    p[ROOT_SUN].Constants.Num32BitValues = 20;
    p[ROOT_SUN].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; /* (sun_f: a fading caster's pixel function too) */
    p[ROOT_SAMPCMP].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[ROOT_SAMPCMP].DescriptorTable.NumDescriptorRanges = 1, p[ROOT_SAMPCMP].DescriptorTable.pDescriptorRanges = &sampcmp;
    p[ROOT_SAMPCMP].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = { ROOT_COUNT, p, 0, NULL,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS };
    ID3DBlob *blob = NULL, *err = NULL;
    ID3D12RootSignature* root = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
        fprintf(stderr, "[recomp] gfx: root signature: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?");
    else if (FAILED(ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                 &IID_ID3D12RootSignature, (void**)&root)))
        root = NULL;
    if (blob)
        ID3D10Blob_Release(blob);
    if (err)
        ID3D10Blob_Release(err);
    return root;
}

/* the present or overlay pipeline, onto the swap chain's format */
static ID3D12PipelineState* util_pipeline(const char* vs, const char* ps, int blend)
{
    ID3DBlob* v = compile(gfx_hlsl_util, vs, "vs_5_1");
    ID3DBlob* f = v ? compile(gfx_hlsl_util, ps, "ps_5_1") : NULL;
    ID3D12PipelineState* p = NULL;
    if (v && f)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        memset(&pd, 0, sizeof pd);
        pd.pRootSignature = g_root;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(v), pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(v);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(f), pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(f);
        D3D12_RENDER_TARGET_BLEND_DESC* c = &pd.BlendState.RenderTarget[0];
        c->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        c->SrcBlend = c->SrcBlendAlpha = D3D12_BLEND_ONE;
        c->DestBlend = c->DestBlendAlpha = D3D12_BLEND_ZERO;
        c->BlendOp = c->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (blend)
            c->BlendEnable = TRUE, c->SrcBlend = blend == 2 ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA,
            c->DestBlend = D3D12_BLEND_INV_SRC_ALPHA; /* 2: premultiplied */
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pd.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(g_dev, &pd, &IID_ID3D12PipelineState, (void**)&p)))
            p = NULL;
    }
    if (v)
        ID3D10Blob_Release(v);
    if (f)
        ID3D10Blob_Release(f);
    return p;
}

#ifdef VANACORE
static char g_adapter[128]; /* the graphics card's name, once the device is up */
static IDXGIAdapter3* g_adapter3; /* the card, for gfx_video_memory (NULL without DXGI 1.4) */

const char* gfx_adapter_name(void) { return g_adapter; }
int gfx_video_memory(unsigned long long* used, unsigned long long* budget)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO vm;
    if (!g_adapter3 || !g_dev || FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(g_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
        return 0;
    *used = vm.CurrentUsage, *budget = vm.Budget;
    return 1;
}

int vanacore_gfx_gpu_lost(void (*out)(const char* line))
{
    return gfx_selected_d3d11() ? 0 : gpu_lost("as the game crashed", out);
}
#endif

/* make_device found no device this back end can draw on: none at feature level 11_0, or one below resource binding
 * tier 2 (gfx12_device_refused) */
static int g_device_refused;

/* the debug layer (FFXI_D3D12_DEBUG=1) and GPU crash data (FFXI_D3D12_DRED=1), set before the device is made: 1 with the
 * debug layer on */
static int debug_layers(void)
{
    /* FFXI_D3D12_GBV=1: GPU-based validation, the debug layer checking on the GPU what it can't see from here - a
     * texture read in the wrong state, a descriptor index past the heap or never written - which hang or fault the
     * card where Direct3D 11 would have caught them. Many times slower; the debug layer comes with it */
    const char* gbv = getenv("FFXI_D3D12_GBV");
    const int gpu_check = gbv && gbv[0] == '1';
    const char* dbg = getenv("FFXI_D3D12_DEBUG");
    if (gpu_check)
        dbg = "1";
    if (dbg && dbg[0] == '1')
    {
        ID3D12Debug* d = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&d)))
        {
            ID3D12Debug_EnableDebugLayer(d);
            ID3D12Debug1* d1 = NULL;
            if (gpu_check && SUCCEEDED(ID3D12Debug_QueryInterface(d, &IID_ID3D12Debug1, (void**)&d1)))
            {
                ID3D12Debug1_SetEnableGPUBasedValidation(d1, TRUE);
                ID3D12Debug1_Release(d1);
                fprintf(stderr, "[recomp] gfx: D3D12 debug layer on, with GPU-based validation (much slower)\n");
            }
            else if (gpu_check)
                fprintf(stderr, "[recomp] gfx: D3D12 debug layer on; GPU-based validation isn't there (no ID3D12Debug1)\n");
            else
                fprintf(stderr, "[recomp] gfx: D3D12 debug layer on\n");
            ID3D12Debug_Release(d);
        }
        else
            fprintf(stderr, "[recomp] gfx: no D3D12 debug layer (it needs Windows' Graphics Tools)\n");
    }
    /* GPU crash data (gpu_lost): set before the device is made; Windows' own, no debug layer needed */
    const char* dred = getenv("FFXI_D3D12_DRED");
    g_dred = 0;
    if (dred && dred[0] == '1')
    {
        ID3D12DeviceRemovedExtendedDataSettings* ds = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&ds)))
        {
            ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
            ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
            ID3D12DeviceRemovedExtendedDataSettings1* ds1 = NULL; /* the passes' marks kept with the breadcrumbs */
            if (SUCCEEDED(ID3D12DeviceRemovedExtendedDataSettings_QueryInterface(ds, &IID_ID3D12DeviceRemovedExtendedDataSettings1,
                    (void**)&ds1)))
            {
                ID3D12DeviceRemovedExtendedDataSettings1_SetBreadcrumbContextEnablement(ds1, D3D12_DRED_ENABLEMENT_FORCED_ON);
                ID3D12DeviceRemovedExtendedDataSettings1_Release(ds1);
            }
            ID3D12DeviceRemovedExtendedDataSettings_Release(ds);
            g_dred = 1;
            fprintf(stderr, "[recomp] gfx: GPU crash data on: if the graphics card stops, where it was goes in the log "
                            "(a little slower)\n");
        }
        else
            fprintf(stderr, "[recomp] gfx: GPU crash data: Windows didn't turn it on (it needs Windows 10 1903 or later)\n");
    }
    return dbg && dbg[0] == '1';
}

/* the device on the high-performance adapter, else the default one */
static int make_device(void)
{
    const int debug = debug_layers();
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&g_factory)))
        g_factory = NULL;
    IDXGIFactory6* f6 = NULL;
    IDXGIAdapter1* adapter = NULL;
    /* made again after a lost device: on the card it was on (gfx_card_find), not DXGI's pick of the moment, which right after
     * an RX 7900 XTX's hang was the processor's own graphics, the game at 0-3 fps (2026-11-01, 11-02). None while that card
     * isn't listed: vanacore_gfx_device_recover tries again */
    const int same_card = gfx_card_known();
    if (same_card)
    {
        adapter = g_factory ? (IDXGIAdapter1*)gfx_card_find(g_factory) : NULL;
        if (!adapter)
        {
            if (g_factory)
                IDXGIFactory4_Release(g_factory), g_factory = NULL;
            return 0;
        }
    }
    else if (g_factory && SUCCEEDED(IDXGIFactory4_QueryInterface(g_factory, &IID_IDXGIFactory6, (void**)&f6)))
    {
        IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void**)&adapter);
        IDXGIFactory6_Release(f6);
    }
    HRESULT hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g_dev);
    if (FAILED(hr) && adapter && !same_card)
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g_dev);
    if (adapter)
        IDXGIAdapter1_Release(adapter), adapter = NULL;
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: no Direct3D 12 device (%08lx)\n", (unsigned long)hr);
        g_dev = NULL;
        g_device_refused = 1;
        return 0;
    }
    /* the card the device is on (the default one if the high-performance one had none): its name and make, and for
     * the log its ids, driver and memory */
    char name[128] = "?", about[160] = "";
    LUID luid;
    ID3D12Device_GetAdapterLuid(g_dev, &luid);
    uint32_t vendor = 0, device = 0, subsys = 0; /* its make, for gfx_card_remember */
    if (g_factory && SUCCEEDED(IDXGIFactory4_EnumAdapterByLuid(g_factory, luid, &IID_IDXGIAdapter1, (void**)&adapter)))
    {
        DXGI_ADAPTER_DESC1 ad;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &ad)))
        {
            vendor = ad.VendorId, device = ad.DeviceId, subsys = ad.SubSysId;
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
#ifdef VANACORE
            g_vendor = ad.VendorId, g_device_id = ad.DeviceId;
#endif
            LARGE_INTEGER umd; /* the driver's version, as the driver's own files carry it */
            char driver[48] = "?";
            if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd)))
                snprintf(driver, sizeof driver, "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                    LOWORD(umd.LowPart));
            /* its memory: DedicatedVideoMemory is a SIZE_T, capped near 3 GB in this 32-bit process; Windows' budget for
             * the game is 64-bit */
            unsigned long long mb = (unsigned long long)(ad.DedicatedVideoMemory >> 20);
            IDXGIAdapter3* a3 = NULL;
            DXGI_QUERY_VIDEO_MEMORY_INFO vm;
            if (SUCCEEDED(IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter3, (void**)&a3)))
            {
                if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)) && vm.Budget)
                    mb = vm.Budget >> 20;
                IDXGIAdapter3_Release(a3);
            }
            snprintf(about, sizeof about, " (vendor %04X, device %04X, driver %s, %llu MB of video memory for the game)",
                ad.VendorId, ad.DeviceId, driver, mb);
        }
        /* kept for gfx_video_memory (released with the device) */
        if (g_adapter3)
            IDXGIAdapter3_Release(g_adapter3), g_adapter3 = NULL;
        if (FAILED(IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter3, (void**)&g_adapter3)))
            g_adapter3 = NULL;
        IDXGIAdapter1_Release(adapter);
    }
    gfx_card_remember(luid.LowPart, (int32_t)luid.HighPart, vendor, device, subsys); /* for a device made again if this one is lost */
    D3D12_FEATURE_DATA_D3D12_OPTIONS o;
    memset(&o, 0, sizeof o);
    ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    /* Tier 2: the pixel stage reads its textures and samplers by index out of the whole heaps (make_root's unbounded
     * tables, 65536 SRVs and 2048 samplers), where tier 1 has 128 SRVs and 16 samplers a stage. Wine's Direct3D 12
     * (vkd3d, the only one a 32-bit game gets under CrossOver: D3DMetal is 64-bit only) reports tier 1 wherever Vulkan
     * has 16 samplers a stage, as MoltenVK on a Mac does */
    if (o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
    {
        fprintf(stderr, "[recomp] gfx: %s has resource binding tier %d; the back end needs tier 2\n", name, (int)o.ResourceBindingTier);
        ID3D12Device_Release(g_dev);
        g_dev = NULL;
        g_device_refused = 1;
        return 0;
    }
    snprintf(g_card, sizeof g_card, "%s%s", name, about);
    fprintf(stderr, "[recomp] gfx: Direct3D 12 on %s%s\n", name, about);
#ifdef VANACORE
    snprintf(g_adapter, sizeof g_adapter, "%s", name);
#endif
    /* with the debug layer: its messages, printed after each submission (print_messages) */
    if (debug && FAILED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12InfoQueue, (void**)&g_info)))
        g_info = NULL;
    return 1;
}

#if defined(FFXI_UWP)
/* UWP: no window handle; the swap chain is for composition, and the app shows it in a SwapChainPanel */
static void make_swap_chain(SDL_Window* win)
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = pw > 0 ? (UINT)pw : 640, sd.Height = ph > 0 ? (UINT)ph : 480;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    g_tearing = 0; /* composition swap chains cannot tear */
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForComposition(g_factory, (IUnknown*)present_queue(), &sd, NULL, &sc1);
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: swap chain failed (%08lx)\n", (unsigned long)hr);
        return;
    }
    uwp_attach_swapchain(sc1);
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g_swap);
    IDXGISwapChain1_Release(sc1);
    g_swap_w = sd.Width, g_swap_h = sd.Height;
    for (int i = 0; i < FRAMES; ++i)
        g_swap_rtv[i] = -1;
    swap_acquire();
}
#else
#ifdef VANACORE
static void make_swap_chain(HWND hwnd)
{
    if (!hwnd || !g_factory)
    {
        fprintf(stderr, "[recomp] gfx: no window for the swap chain\n");
        return;
    }
#else
static void make_swap_chain(SDL_Window* win)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd || !g_factory)
    {
        fprintf(stderr, "[recomp] gfx: no window handle for the swap chain: %s\n", SDL_GetError());
        return;
    }
#endif
    IDXGIFactory5* f5 = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(g_factory, &IID_IDXGIFactory5, (void**)&f5)))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            g_tearing = allow != FALSE;
        IDXGIFactory5_Release(f5);
    }
    int pw = 0, ph = 0;
#ifdef VANACORE
    RECT rc;
    if (GetClientRect(hwnd, &rc))
        pw = rc.right, ph = rc.bottom;
#else
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
#endif
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = pw > 0 ? (UINT)pw : 640, sd.Height = ph > 0 ? (UINT)ph : 480;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FRAMES;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* sc1 = NULL;
    /* on the present queue: Present is queued on the swap chain's queue, and the game's has the next frame's work (and
     * its waits on the DLSS helper) in front by the time a frame is shown */
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(g_factory, (IUnknown*)present_queue(), hwnd, &sd, NULL, NULL, &sc1);
    if (FAILED(hr))
    {
        fprintf(stderr, "[recomp] gfx: swap chain failed (%08lx)\n", (unsigned long)hr);
        return;
    }
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&g_swap);
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory4_MakeWindowAssociation(g_factory, hwnd, DXGI_MWA_NO_ALT_ENTER); /* full screen is the window's (SDL) */
    g_swap_w = sd.Width, g_swap_h = sd.Height;
    for (int i = 0; i < FRAMES; ++i)
        g_swap_rtv[i] = -1;
    swap_acquire();
}
#endif

#ifdef VANACORE
void gfx_set_vsync(int on) { g_vsync = on, g_vsync_asked = 1; }
#endif

/* gfx_init's present didn't build: its shaders don't compile with this d3dcompiler_47 (gfx12_shaders_failed) */
static int g_shaders_failed;

int gfx12_shaders_failed(void) { return g_shaders_failed; }
int gfx12_device_refused(void) { return g_device_refused; }

/* what gfx_init made, let go when it fails partway (nothing has drawn with any of it yet), and the last of gfx_shutdown's
 * teardown: the device's count left, 0 once it's gone */
static ULONG unmake_device(void)
{
    ULONG held = 0;
    if (g_dummy)
        ID3D12Resource_Release(g_dummy), g_dummy = NULL;
    Heap* heaps[] = { &g_srv, &g_samp, &g_rtv, &g_dsv };
    for (int i = 0; i < 4; ++i)
    {
        if (heaps[i]->heap)
            ID3D12DescriptorHeap_Release(heaps[i]->heap);
        free(heaps[i]->free_slots);
        memset(heaps[i], 0, sizeof *heaps[i]);
    }
    free(g_srv_owner), g_srv_owner = NULL;
    if (g_root)
        ID3D12RootSignature_Release(g_root), g_root = NULL;
    if (g_fence)
        ID3D12Fence_Release(g_fence), g_fence = NULL;
    if (g_fence_event)
        CloseHandle(g_fence_event), g_fence_event = NULL;
    if (g_list)
        ID3D12GraphicsCommandList_Release(g_list), g_list = NULL;
    for (int i = 0; i < FRAMES; ++i)
        if (g_frames[i].alloc)
            ID3D12CommandAllocator_Release(g_frames[i].alloc), g_frames[i].alloc = NULL;
    if (g_show.list)
        ID3D12GraphicsCommandList_Release(g_show.list), g_show.list = NULL;
    for (int i = 0; i < FRAMES; ++i)
        if (g_show.alloc[i])
            ID3D12CommandAllocator_Release(g_show.alloc[i]), g_show.alloc[i] = NULL;
    if (g_pfence)
        ID3D12Fence_Release(g_pfence), g_pfence = NULL;
    if (g_pfence_event)
        CloseHandle(g_pfence_event), g_pfence_event = NULL;
    if (g_pqueue)
        ID3D12CommandQueue_Release(g_pqueue), g_pqueue = NULL;
    if (g_queue)
        ID3D12CommandQueue_Release(g_queue), g_queue = NULL;
    if (g_info)
        ID3D12InfoQueue_Release(g_info), g_info = NULL;
    if (g_dev)
        held = ID3D12Device_Release(g_dev), g_dev = NULL;
    if (g_factory)
        IDXGIFactory4_Release(g_factory), g_factory = NULL;
    if (g_adapter3)
        IDXGIAdapter3_Release(g_adapter3), g_adapter3 = NULL;
    return held;
}

/* --- the device torn down (gfx.h gfx_shutdown) --------------------------------------------------------------------
 * After the graphics card was lost (a driver timeout: the device and everything on it are gone, and no new device is to be
 * had until the old one is let go of - Direct3D 12 makes one device per card in a process, and D3D12CreateDevice hands back
 * the one there is, lost or not), or in a test. Everything this file made on the device is released: the front end's
 * objects went before it (d3d8_device_lost), and the render targets' list and the slabs' blocks say if any didn't. The
 * settings stay (the upscaler's, the effects', vsync, the window), and the shaders' compiled code; gfx_init then brings a
 * new device up as at the start, the game's pipelines from the cache file. */
#ifdef VANACORE
/* a map's keys and table let go (what its entries point at is released first) */
static void map_free(Map* m)
{
    for (uint32_t i = 0; i < m->cap; ++i)
        if (m->e[i].hash)
            free(m->e[i].key);
    free(m->e);
    memset(m, 0, sizeof *m);
}

/* the thread pool's builds done: the pipelines' (queue_job, and warm_callback's calls still queued, each holding its entry) and
 * the scene effects' (fx_ready). 0 when one is still on after ms */
static int builds_done(DWORD ms)
{
    const ULONGLONG until = GetTickCount64() + ms;
    while (InterlockedCompareExchange(&g_pipes_building, 0, 0) != 0 || InterlockedCompareExchange(&g_warm_calls, 0, 0) != 0 ||
           InterlockedCompareExchange(&g_fx.state, 0, 0) == 1)
    {
        if (GetTickCount64() >= until)
            return 0;
        Sleep(1);
    }
    return 1;
}

/* an effect target, or a shadow map, let go at once (not deferred: the device goes with it) */
static void fx_tex_drop(FxTex* t)
{
    if (t->res)
        ID3D12Resource_Release(t->res);
    memset(t, 0, sizeof *t);
    t->srv = t->rtv = -1;
}

static void sun_map_drop(SunMap* m)
{
    if (m->res)
        ID3D12Resource_Release(m->res);
    memset(m, 0, sizeof *m);
    m->srv = m->dsv = -1;
}

int gfx_shutdown(void)
{
    if (!g_dev)
        return 1;
    const int lost = g_lost_said;
    fprintf(stderr, "[recomp] gfx: the device torn down%s\n", lost ? ": it was lost" : "");
#ifdef VANACORE
    batch_reset(), g_batch.open = 0; /* the zone's draws kept die with the device, undrawn */
#endif
    /* the threads and the helper first: nothing else touches the device from here */
    if (!fg_stop_thread())
        return 0;
    dlss_let_go();
    InterlockedExchange(&g_warm_cancel, 1); /* the start-up rebuild's queue drained, not built (warm_callback) */
    if (!builds_done(15000))
    {
        InterlockedExchange(&g_warm_cancel, 0); /* the device stays: what's left of the rebuild is built after all */
        fprintf(stderr, "[recomp] gfx: a build on the thread pool didn't finish in 15 s: the device can't be let go of\n");
        return 0;
    }
    /* the GPU past everything (a lost device's fences read done at once), nothing in flight */
    if (g_list_open)
        submit(0);
    if (!lost && g_queue && g_fence)
    {
        ID3D12CommandQueue_Signal(g_queue, g_fence, ++g_fence_value);
        wait_fence(g_fence_value);
    }
    pqueue_idle();
    /* this file's own textures and targets: the effects', the sun's maps, DLSS's, frame generation's, the measuring's */
    gfx_tex_destroy(g_scratch_depth), g_scratch_depth = NULL;
    el_off();
    fx_tex_drop(&g_el.x);
    for (int i = 0; i < 7; ++i)
        fx_tex_drop(&g_el.lv[i]), fx_tex_drop(&g_el.tmp[i]);
    g_el.color = g_el.depth = NULL;
    gfx_tex_destroy(g_fx.wdepth), g_fx.wdepth = NULL, g_fx.wdepth_of = g_fx.wdepth_glow = NULL, g_fx.wdepth_serial = 0;
    gfx_tex_destroy(g_snap_color), gfx_tex_destroy(g_snap_depth), g_snap_color = g_snap_depth = NULL;
    g_fx.glow_kept_serial = 0;
    g_fx.floors_id = g_fx.floors_depth = NULL, g_fx.floors_state = 0;
    g_dlss_replay_depth = NULL, g_zup_cur = 0;
    g_dlss.scene = g_dlss.pend_color = g_dlss.pend_depth = NULL;
    g_dlss.pend_serial = g_dlss.noted = g_dlss.out_serial = g_dlss.ran_serial = g_dlss.prev_serial = 0;
    g_fgn.nheld = 0;
    FxTex* fx[] = { &g_fx.src, &g_fx.ao0, &g_fx.ao1, &g_fx.b1a, &g_fx.b1b, &g_fx.b2a, &g_fx.b2b, &g_fx.ra, &g_fx.rb, &g_fx.sm,
        &g_fx.rl, &g_fx.cmask, &g_fx.amask[0], &g_fx.amask[1], &g_fx.gi, &g_fx.gic, &g_fx.lz[0], &g_fx.lz[1], &g_fx.lz[2], &g_fx.lz[3], &g_fx.glow[0], &g_fx.glow[1],
        &g_fx.glow_kept, &g_fx.cl[0], &g_fx.cl[1], &g_fx.nrl[0], &g_fx.nrl[1], &g_fx.nrl[2], &g_fx.nrm[0], &g_fx.nrm[1], &g_fx.pp,
        &g_fx.up1[0], &g_fx.up1[1], &g_fx.sky[0], &g_fx.sky[1], &g_fx.meas, &g_fx.d0, &g_fx.d1,
        &g_fx.foc[0], &g_fx.foc[1], &g_fx.hist[0], &g_fx.hist[1], &g_fx.gih[0], &g_fx.gih[1], &g_nd, &g_nh, &g_mo.t, &g_mo.m };
    for (size_t i = 0; i < sizeof fx / sizeof fx[0]; ++i)
        fx_tex_drop(fx[i]);
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k <= FG_MAX; ++k)
            fx_tex_drop(&g_fg.shown[s][k]);
    for (int i = 0; i < FRAMES; ++i)
        fx_tex_drop(&g_show.tex[i]);
    occl_drop();
    memset(g_show.copied, 0, sizeof g_show.copied), memset(g_show.alloc_done, 0, sizeof g_show.alloc_done), g_show.next = 0;
    g_fx.hist_at = g_fx.gih_at = g_fx.foc_i = g_fx.sky_i = g_fx.nrm_i = 0;
    g_fx.hist_serial = g_fx.gih_serial = g_fx.prev_serial = g_fx.nrm_serial = g_fx.foc_ns = g_fx.eased_serial = 0;
    g_fx.eased = 0, g_fx.fade_ns = g_fx.fade_serial = 0;
    g_fx.wait_casters = g_fx.wait_building = 0, g_fx.wait_serial = 0;
    g_nd_clear = 1, g_nd_valid = 0, g_mo.clear_serial = g_mo.m_clear = 0;
    g_zn_depth = NULL;
    g_mout = g_maskout = 0, g_mo_slot = -1, g_mo_va = 0;
    g_mob[0].n = g_mob[1].n = 0, g_mob_n = 0, g_mob_id++; /* (the redraws kept: the old device's) */
    SunMap* maps[] = { &g_fx.smap, &g_fx.smapn, &g_fx.smaps, &g_fx.smapns, &g_fx.smapsb, &g_fx.smapnsb,
        &g_fx.smapv, &g_fx.smapm, &g_fx.smapms, &g_fx.smapmsb };
    for (size_t i = 0; i < sizeof maps / sizeof maps[0]; ++i)
        sun_map_drop(maps[i]);
    memset(&g_still, 0, sizeof g_still);
    memset(&g_still_near, 0, sizeof g_still_near);
    memset(&g_still_mid, 0, sizeof g_still_mid);
    memset(&g_roll_far, 0, sizeof g_roll_far);
    memset(&g_roll_near, 0, sizeof g_roll_near);
    memset(&g_roll_mid, 0, sizeof g_roll_mid);
    /* the sun's cache, the copies of the zone's meshes (their buffers) and the casters: addresses and pipelines of the old
     * device, every one */
    sun_cache_trim(1);
    snap_trim(1, 0);
    for (int i = 0; i < SNAP_CHUNKS; ++i)
        if (g_chunks[i].buf)
            chunk_free(i);
    g_ncasters = 0, g_caster_u_len = 0, g_was.n = 0, g_was.npend = g_set.npend = 0, g_ncache_vis = 0;
    g_nheld = 0, g_held_len = 0;
    drawn_forget();
    /* frame generation's fence (shared with the helper, which is gone) */
    if (g_br_cs_ready)
        EnterCriticalSection(&g_br_cs); /* (br_thread may be signalling it) */
    if (g_fg.fence)
        ID3D12Fence_Release(g_fg.fence), g_fg.fence = NULL;
    if (g_br_cs_ready)
        LeaveCriticalSection(&g_br_cs);
    g_fg.value = g_fg.last = 0, g_fg.starting = 0, g_fg.hudless_serial = g_fg.prev_serial = g_fg.on_serial = 0, g_fg.noted_fps = 0;
    /* the scene effects' pipelines and shaders (deferred, released below; built again at the next scene), the utility
     * pipelines, the game's pipelines and the samplers: made again as they're asked for */
    InterlockedExchange(&g_fx_rebuild, 1);
    fx_rebuild_take();
    ID3D12PipelineState** psos[] = { &g_present_pso, &g_present_cas_pso, &g_overlay_pso, &g_present_layer_pso };
    for (size_t i = 0; i < sizeof psos / sizeof psos[0]; ++i)
        if (*psos[i])
            ID3D12PipelineState_Release(*psos[i]), *psos[i] = NULL;
    /* the curved surfaces' pass (zn_ready): its pipeline is the old device's; the two shader blobs are kept */
    if (g_zn.ps)
        ID3D12PipelineState_Release(g_zn.ps), g_zn.ps = NULL;
    g_zn.dsv = DXGI_FORMAT_UNKNOWN, g_zn.failed = 0;
    for (uint32_t i = 0; i < g_pipes.cap; ++i)
        if (g_pipes.e[i].hash)
        {
            PipeEntry* e = (PipeEntry*)g_pipes.e[i].obj;
            if (e->state && e->state != PIPE_FAILED)
                ID3D12PipelineState_Release((ID3D12PipelineState*)e->state);
            free(e);
        }
    map_free(&g_pipes);
    map_free(&g_samplers);
    g_device_gen++;
    /* the profile's timestamps, the readback staging buffer */
    if (g_query_rb)
        ID3D12Resource_Release(g_query_rb), g_query_rb = NULL, g_query_cpu = NULL;
    if (g_queries)
        ID3D12QueryHeap_Release(g_queries), g_queries = NULL;
    if (g_sq_rb)
        ID3D12Resource_Release(g_sq_rb), g_sq_rb = NULL, g_sq_cpu = NULL;
    if (g_sq)
        ID3D12QueryHeap_Release(g_sq), g_sq = NULL;
    if (g_sync_rb)
        ID3D12Resource_Release(g_sync_rb), g_sync_rb = NULL, g_sync_rb_size = 0;
    /* everything deferred, released now; then the frames' upload rings and the static buffers' slabs */
    collect_all();
    for (int i = 0; i < FRAMES; ++i)
    {
        Frame* f = &g_frames[i];
        for (uint32_t c = 0; c < f->nchunks; ++c)
            ID3D12Resource_Release(f->chunks[c].res);
        free(f->chunks), f->chunks = NULL, f->nchunks = f->cur = 0, f->fence = 0;
        f->timed = f->marks = f->scope_n = f->closed = f->full = 0, f->measured = -1;
    }
    g_frame = 0, g_frame_open = 0, g_list_open = 0, g_cmd_draws = 0, g_fence_value = 0, g_pvalue = 0;
    uint32_t blocks_out = 0, targets_out = 0;
    for (int c = 0; c < SLAB_CLASSES; ++c)
    {
        Slab* s = g_slabs[c];
        g_slabs[c] = NULL;
        while (s)
        {
            Slab* next = s->next;
            blocks_out += s->nblocks - s->nfree;
            ID3D12Resource_Release(s->res);
            free(s->free_blocks);
            free(s);
            s = next;
        }
    }
    for (const GfxTex* t = g_rts; t; t = t->rt_next)
        if (++targets_out <= 4)
            fprintf(stderr, "[recomp] gfx: a render target still held: %ux%u, format %u\n", t->w, t->h, (unsigned)t->fmt);
    if (blocks_out || targets_out)
        fprintf(stderr, "[recomp] gfx: %u static buffers and %u render targets are still held by their owners: the device stays held\n",
            blocks_out, targets_out);
    /* the swap chain, the targets bound, the crash data's notes, the loss */
    swap_release();
    if (g_swap)
        IDXGISwapChain3_Release(g_swap), g_swap = NULL;
    g_swap_w = g_swap_h = 0;
    for (int i = 0; i < FRAMES; ++i)
        g_swap_rtv[i] = -1;
    g_rt = g_ds = NULL, g_targets_bound = 0, g_bound_pso = NULL, g_bound_topo = -1, g_present_layer = NULL;
    for (int i = 0; i < NOTE_LISTS; ++i)
        g_notes[i].count = 0, g_notes[i].broken = 0;
    g_notes_sent = 0, g_dred_scope = GS_SCENE, g_lost_said = 0;
    /* the device, its queues, lists, fences, heaps and root signature: the count left says whether something still holds it */
    const ULONG held = unmake_device();
    if (held)
        fprintf(stderr, "[recomp] gfx: the device is still held after the teardown (%lu references): a child of it leaked, or an "
                        "overlay's hook holds it; no new device can be made\n", (unsigned long)held);
    else
        fprintf(stderr, "[recomp] gfx: the device let go of\n");
    return held == 0;
}

/* Tests: the device removed, as a driver timeout would (ID3D12Device5::RemoveDevice), and found lost. 1 when done; 0 with no
 * device up, or without ID3D12Device5 (Windows 10 before 1809) */
int gfx12_lose_device(void)
{
    ID3D12Device5* d5 = NULL;
    if (!g_dev || FAILED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12Device5, (void**)&d5)))
        return 0;
    ID3D12Device5_RemoveDevice(d5);
    ID3D12Device5_Release(d5);
    if (!gpu_lost("in a test", gpu_log_line))
        g_lost_said = 1; /* (the reason read as none: lost all the same) */
    return 1;
}
#else
int gfx_shutdown(void) { return 0; }
#endif

int gfx_init(void* window, int vsync)
{
#ifdef VANACORE
    /* up already (vanacore_gfx_start, before the game has a window): the swap chain goes on this one */
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
#else
    if (g_dev)
        return 1;
#endif
    if (!make_device())
        return 0;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void**)&g_queue);
    /* the present queue (g_pqueue), at high priority so its copies get in soon between the game's work; the game's
     * queue stands in without one */
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void**)&g_pqueue)))
    {
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void**)&g_pqueue)))
        {
            g_pqueue = NULL;
            fprintf(stderr, "[recomp] gfx: no present queue of its own: the frames go to the window through the game's\n");
        }
    }
    ID3D12Device_CreateFence(g_dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g_pfence);
    g_pfence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (int i = 0; i < FRAMES; ++i)
        ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&g_frames[i].alloc);
    ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, NULL, &IID_ID3D12GraphicsCommandList,
        (void**)&g_list);
    ID3D12GraphicsCommandList_Close(g_list);
    if (g_dred && g_queue && g_list) /* their names in the GPU crash data */
    {
        ID3D12CommandQueue_SetName(g_queue, L"VanaCore");
        ID3D12GraphicsCommandList_SetName(g_list, L"VanaCore's frame");
        if (g_pqueue)
            ID3D12CommandQueue_SetName(g_pqueue, L"VanaCore's present");
    }
    ID3D12Device_CreateFence(g_dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&g_fence);
    g_fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    static int pipe_lock_made; /* (once: gfx_init runs again after gfx_shutdown) */
    if (!pipe_lock_made)
        InitializeCriticalSection(&g_pipe_file_lock), pipe_lock_made = 1;
    g_root = make_root();
    if (!g_queue || !g_list || !g_fence || !g_pfence || !g_pfence_event || !g_root ||
        !heap_init(&g_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_HEAP, 1, 2) ||
        !heap_init(&g_samp, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP, 1, 0) ||
        !heap_init(&g_rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_HEAP, 0, 0) ||
        !heap_init(&g_dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_HEAP, 0, 0))
    {
        fprintf(stderr, "[recomp] gfx: Direct3D 12 set-up failed\n");
        unmake_device();
        return 0;
    }
    free(g_srv_owner);
    g_srv_owner = (GfxTex**)calloc(SRV_HEAP, sizeof *g_srv_owner);
    /* the null views every unbound slot reads, and a sampler for the present */
    D3D12_SHADER_RESOURCE_VIEW_DESC nv;
    memset(&nv, 0, sizeof nv);
    nv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, nv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, NULL, &nv, heap_cpu(&g_srv, SRV_NULL_2D));
    nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, nv.TextureCube.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g_dev, NULL, &nv, heap_cpu(&g_srv, SRV_NULL_CUBE));
    GfxSampler linear;
    memset(&linear, 0, sizeof linear);
    linear.addr_u = linear.addr_v = linear.addr_w = 3, linear.mag = linear.min = 2;
    g_present_samp = sampler(&linear);
    /* the shadow lookups': 1 where the point is no deeper than the map, filtered across texels */
    int32_t cmp = heap_alloc(&g_samp);
    g_cmp_samp = cmp >= 0 ? (uint32_t)cmp : g_present_samp;
    if (cmp >= 0)
    {
        D3D12_SAMPLER_DESC cd;
        memset(&cd, 0, sizeof cd);
        cd.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        cd.AddressU = cd.AddressV = cd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        cd.MaxAnisotropy = 1;
        cd.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        cd.MaxLOD = D3D12_FLOAT32_MAX;
        ID3D12Device_CreateSampler(g_dev, &cd, heap_cpu(&g_samp, cmp));
    }
    g_dummy = make_buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);

    shader_cache_init();
    /* without the present nothing reaches the window: a black one, the game running behind it. Wine's d3dcompiler_47
     * (vkd3d-shader's HLSL compiler, Proton's too) has no Shader Model 5.1 resource arrays, which every shader here
     * reads its textures through, so none of them compiles there: Direct3D 11 stands in (native.c) */
    g_present_pso = util_pipeline("present_vs", "present_fs", 0);
    if (!g_present_pso)
    {
        fprintf(stderr, "[recomp] gfx: Direct3D 12's present didn't build (its shaders didn't compile: Wine's or Proton's "
                        "d3dcompiler_47? under Wine or Proton, winetricks d3dcompiler_47 puts Microsoft's in the prefix)\n");
        g_shaders_failed = 1;
        unmake_device();
        return 0;
    }
    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
#ifdef VANACORE
    g_profile_env = gfx_profiling;
#endif
    if (gfx_profiling)
        prof_queries();
    const char* show = getenv("FFXI_FPS");
    g_overlay = !(show && show[0] == '0');
    g_present_cas_pso = util_pipeline("present_vs", "present_cas_fs", 0);
#ifdef VANACORE
    g_present_layer_pso = util_pipeline("present_vs", "present_layer_fs", 2);
#endif
    fx_config();
    g_overlay_pso = util_pipeline("overlay_vs", "overlay_fs", 1);
    if (!g_sync_pipelines)
    {
        prewarm_pipelines();
        /* the scene effects' shaders built now, on the thread pool: ready by the first zone (the title and the
         * character list come first), rather than coming on seconds into it */
        fx_ready();
    }
    g_vsync = vsync;
    if (window)
    {
#ifdef VANACORE
        g_window = (HWND)window;
#else
        g_window = (SDL_Window*)window;
#endif
        make_swap_chain(g_window);
    }
    return 1;
}

#ifdef VANACORE
#include "gfx_backend.h"
GFX_BACKEND_TABLE(gfx12_backend)
#endif
