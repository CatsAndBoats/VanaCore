/* What the two back ends' scene effects do the same way (gfx_d3d11.c, gfx_d3d12.c), kept once here: the sun's glide, the
 * shader tokens the shadow casters keep, the shadow casters' copies' budget and FSR 1's sizes. Not renamed per back end
 * (gfx_rename.h): one copy for both. The game's thread. */
#pragma once

#include <stdint.h>

#include "gfx.h"

/* The sun as the shadows follow it (from XI on Anything, rubymatrix/xi-on-anything, gfx_d3d12.c): the game moves it on in
 * steps (a quarter degree each game minute, every 2.4 s), and the shadows glide from where they are to each over the next 2.5
 * seconds rather than jump their whole edge - long ones at dawn and dusk most. at: where they are now (the back end's sunw),
 * from where toward to, p of the way there (1: there). */
typedef struct GfxSunGlide
{
    float from[3], to[3], p;
    uint64_t was_ns; /* the last frame's time (gfx_sun_glide_on) */
} GfxSunGlide;

/* The game's sun direction w taken: the glide toward it starts from `at`. At once (now, or a turn past about 6 degrees: the
 * game's light turning over to the moon at 06:00 and 18:00, a new place) `at` jumps there, as before the glide. */
void gfx_sun_glide(GfxSunGlide* g, float at[3], const float* w, int now);
/* A frame of the glide: `at` moved on by the time since the last (2.5 s the whole way, whatever the frame rate) */
void gfx_sun_glide_on(GfxSunGlide* g, float at[3]);

/* The sun's shadows round sunrise (sun_dusk), for a Vana'diel minute of the day (0 to 1440): the game's light is the moon's
 * going out through 05:00 to 06:00 (its colour reaches nothing) and the sun's comes on at full at 06:00, so the zone had no
 * shadows until then and then all of them at once. Returns the strength the shadows have from 05:30 to 06:00 with no light of the
 * game's to give it (GFX_DAWN_DAY, the day factor), going out over the six minutes after 06:00, by which time the game's own
 * light has taken over; *ramp is what the shadows' strength is multiplied by through 05:30 to 06:30 (0 to 1 as a smoothstep, 1
 * outside it), so neither the floor nor the game's light coming on shows as a step. 0 and 1 where the minute isn't known (<= 0).
 * At dusk *ramp also goes 1 to 0 through 17:30 to 18:00 and back to 1 by 18:30 (the day 0 there): the game's light turns 180
 * degrees, sun to moon, at 18:00, and the shadows turn with it while there are none. */
#define GFX_DAWN_DAY 0.85f
float gfx_dawn(float minute, float* ramp);

/* Round the game's two turns of its light, sun to moon at 18:00 and moon to sun at 06:00 (180 degrees in a frame, the sun's light
 * coming on at full): 0 at each turn, 1 GFX_TURN_HALF Vana'diel minutes either side and beyond, a smoothstep between; 1 where the
 * minute isn't known (<= 0). The shadows' strength with "Shadows at night" on (gfx_sun_light's night_turns) and the game's own
 * sun and moon light (d3d8_sun.c d3d8_brighten) go by it, so neither turn shows */
#define GFX_TURN_HALF 30.0f
float gfx_light_turn(float minute);

/* gfx_scene_done's sun_dusk round sunrise, gfx_dawn's two factors (day: the strength the shadows have, ramp: what everything they
 * are is multiplied by). minute 0 (sun_dusk off, or no minute known) gives day 0 and ramp 1. While day is above 0 the shadows
 * fall from where the sun is about to be (its bearing at 06:00, the world's +x, as low as `low`, the sine DUSK_LOW): sw (the
 * light's direction in the world's axes, wy the sign of the world's up) is set to it and *height to `low`, so the maps keep
 * the bearing the game's light has when it takes over. */
typedef struct
{
    float day, ramp;
} GfxDawn;
GfxDawn gfx_dawn_sun(float minute, float low, float wy, float sw[3], float* height);

/* the direction the shadows fall from, sw (in the world's axes) through the view's rotation, normalized, into out[0..2] */
void gfx_sun_shadow_dir(float out[3], const float sw[3], const float* view);

/* The light the sun's shadows fall from this frame, and their strength (both back ends' gfx_scene_done). From the lit draws' sun
 * (sunw, in the world; up the world's up, eased): at dusk and dawn FFXI's light stays strong while it sets past the horizon (it
 * goes on lighting the world from under it), so with sun_dusk the shadows keep its bearing but fall from no lower than
 * GFX_DUSK_LOW over the horizon - long - and fade only once it's well under (GFX_DUSK_GONE; not lower: flat ground under a sun
 * that low shades itself in the map), and they're brought in round sunrise (gfx_dawn_sun). The strength `day` comes in as the
 * sun's height gives it and goes out scaled by the cloud (sun_direct: the sun's share of the scene's light, eased into `direct`
 * frame to frame while `measured`: the sun is the game's own this frame and not held; dl and al its luminances, for the trace),
 * the dawn, and a room (GfxScene.indoors, by moghouse). Or, sun_free, the light is where the panel puts it (sun_free_az a
 * bearing round the horizon, sun_free_el a height over it, degrees), at full strength whatever the hour, the cloud or the room:
 * for testing shadows at the angle a report was at. */
#define GFX_DUSK_LOW 0.3f   /* a sine: about 17 degrees up */
#define GFX_DUSK_GONE -0.5f /* ... 30 down */
typedef struct GfxSunLightIn
{
    const GfxScene* s;  /* its view (the world's up from its up row), minute, indoors and light */
    const float* sunw;  /* toward the sun in the world, as the lit draws gave it */
    const float* up;    /* the world's up, eased */
    float sun_free, sun_free_az, sun_free_el, sun_dusk, sun_direct, moghouse; /* [graphics.effects] */
    int measured, eased; /* the cloud's share is measured this frame, and eased toward (else taken) */
    float sun_tilt;      /* [graphics.effects] sun_tilt: degrees toward the south (gfx_shared.c sun_tilt) */
    float sun_moon;      /* [graphics.effects] sun_moon: 0 no shadows from the moon outdoors at night (gfx_shared.c moon_shadows) */
} GfxSunLightIn;
typedef struct GfxSunLight
{
    float sw[3];  /* toward the light the shadows fall from, in the world */
    float height; /* its sine over the horizon, after the dusk clamp and the dawn */
    float day;    /* in: the strength the sun's height gives; out: scaled by the cloud, the dawn and the room */
    float direct; /* in and out: the cloud's share, eased (unchanged unless measured) */
    float dl, al; /* the sun's and the ambient's luminance when measured */
    int free_sun; /* the light placed by hand (sun_free) */
} GfxSunLight;
void gfx_sun_light(const GfxSunLightIn* in, GfxSunLight* out);

/* The card Direct3D 12 made its device on, kept for the process: its adapter's LUID (the two halves) and its make (vendor,
 * device and subsystem ids). A device made after a lost one (native.c vanacore_gfx_device_recover), Direct3D 12's again or
 * Direct3D 11's in its place, goes on this card, not on DXGI's high-performance pick of the moment: right after a hang that
 * was the processor's own graphics, and the game ran at 0-3 fps (an RX 7900 XTX, 2026-11-01 and 11-02; on 11-02 the LUID
 * kept wasn't listed either). gfx_card_known is 0 before Direct3D 12 made a device, as when renderer=d3d11 from the start,
 * and after gfx_card_forget (any card will do). */
void gfx_card_remember(uint32_t low, int32_t high, uint32_t vendor, uint32_t device, uint32_t subsys);
int gfx_card_known(void);
void gfx_card_forget(void);
/* The remembered card among the adapters of `factory` (an IDXGIFactory1 or later), as an IDXGIAdapter1* with a reference for the
 * caller: the one with its LUID, else one of its make (a card Windows restarted comes back under a new LUID). NULL when none is
 * remembered or the card isn't listed (yet: AMD's driver takes seconds to come back). The adapters listed go in the log when
 * the card isn't found by its LUID, once for each different list. */
void* gfx_card_find(void* factory);

/* A shader's tokens as shadow casters hold them: the game's own are freed by DeleteVertexShader/DeletePixelShader (a zone
 * change), but an entry of the sun's cache or a snap may build its shaders from them long after. One copy per program (its
 * hash, as the shaders' keys have it; `pixel` 1 for a pixel shader), kept for the session. t itself for a fixed function
 * (prog 0) or no tokens. */
const uint32_t* gfx_tokens_kept(uint32_t prog, int pixel, const uint32_t* t);

/* A shadow caster's vertices or indices from a buffer the game rewrites between draws are copied as they are when drawn
 * (from XI on Anything, rubymatrix/xi-on-anything, runtime/portable/gfx_d3d12.c: caster_snapshot): up to
 * GFX_CASTER_COPY_MOST bytes a stream, GFX_CASTER_COPY_FRAME a frame. True if n more bytes fit this frame's (`serial`), and
 * they're counted. */
#define GFX_CASTER_COPY_MOST (1u << 20)
#define GFX_CASTER_COPY_FRAME (8u << 20) /* a buffer rewritten for 300 frames costs no more than this */
int gfx_caster_copy_ok(uint32_t n, uint64_t serial);

/* FSR 1 (VanaCore's own, in AMD's FSR's place): for the scene `s` drawn into a target cw x ch at `factor` (the render size
 * over the output's), the viewport's part of the target (vp: x, y, width, height), its size (in_w x in_h) and the size it's
 * shown at (out_w x out_h). 0 when this frame's scene isn't one to make larger: a scene larger than it's shown (the game's
 * 4096 glow picture, a frame mid-resize), or one drawn at its full size by a mode that draws it smaller (the frame FSR came
 * on in). */
int gfx_fsr1_sizes(const GfxScene* s, uint32_t cw, uint32_t ch, float factor, float vp[4], uint32_t* in_w, uint32_t* in_h,
    uint32_t* out_w, uint32_t* out_h);

/* 1 when the fence between the game and VanaCore.Dlss.exe is not shared but waited for on the CPU (dlss_ipc.h, cpu_sync):
 * under Wine and Proton, which can't open a fence from another process (the helper dies in vkImportSemaphoreWin32HandleKHR).
 * VANA_DLSS_FENCE=cpu asks for it on Windows too (to try it), =gpu keeps the shared fence under Wine. */
int gfx_dlss_cpu_sync_wanted(void);

/* VanaCore's player light ([graphics.effects] player_light, the composite's player_light): a light hung over the player, shining
 * down, a pool of light round them: through the Vana'diel night it lights the pool, by day what is round the pool goes darker.
 * From the scene `s` (its focus, view, night for the light, night_clock for the darkening: Night off at night does neither)
 * and the settings (strength 0 off, to 2; radius of the pool in world units; warm 0 white to 1 a torch's orange; day how dark round the pool by day, 0 to 1): plight = the player's feet in view space, w the
 * pool's radius (0: none this frame: off, no player); plight2 = its colour times its gain (0 by day), w the share of the light
 * taken off round the pool (0 at night). The light hangs max(0.8 radius, 3) over them. */
void gfx_player_light(const GfxScene* s, float strength, float radius, float warm, float day, float plight[4], float plight2[4]);

/* The ink lines on VanaCore's grass ([graphics.effects] outline_grass, outline_width_grass, outline_depth_grass, outline_fade_grass;
 * the composite's toon4), with the grass apart from the world's (outline_nograss): toon4 = how dark (a share of the lines'
 * strength, toon.x; 0 none), their width in pixels on a scene vh lines high (as wide on a 1080-line screen), the silhouette's
 * threshold and the distance they're gone by. A width, threshold or fade of 0 is the world's (toon.y, toon.z, toon2.x). */
void gfx_grass_lines(float strength, float width, float depth, float fade, float vh, const float toon[4], const float toon2[4],
    float toon4[4]);

/* The scene effects' shader build (fx_build in gfx_d3d11.c and gfx_d3d12.c), watched: the 30-odd functions compile in seconds
 * with Microsoft's d3dcompiler_47.dll and in minutes with Wine's own (vkd3d-shader's HLSL compiler, Proton's too), and until
 * they're built the effects draw nothing (no shadows, occlusion, grade, zone lines, sparkles, addons' ground rings) with
 * nothing said: that went unseen on a Mac (CrossOver) through a hundred versions. begin notes the start and says so when the
 * compiler is Wine's; tick (from fx_ready while it builds) says once when it has gone past 20 s; end logs ready or failed
 * with the time taken. gfx_hlsl_compiler_is_wine: "Wine builtin DLL", winebuild's mark in d3dcompiler_47.dll's DOS stub. */
int gfx_hlsl_compiler_is_wine(void);
void gfx_fx_build_begin(const char* backend);
void gfx_fx_build_tick(void);
void gfx_fx_build_end(int ok);

/* The start-up rebuild's own thread pool (prewarm_shaders in gfx_d3d11.c, prewarm_pipelines in gfx_d3d12.c): half the
 * processors, 1 to 4 threads. The rebuild queued every cached key on the default pool at once, and Wine's starts a new worker
 * for each submission while the others are busy, up to 500: under Proton 654 pairs were hundreds of vkd3d-shader compiles side
 * by side in the 32-bit address space, and a login in the first 50 s ran out of memory (LuaJIT's "not enough memory", a
 * bad_alloc, the crash dump not written: Arnd, 2026-11-18 to 21). gfx_warm_env is the pool's PTP_CALLBACK_ENVIRON for
 * TrySubmitThreadpoolCallback, NULL (the default pool) when it couldn't be made; gfx_warm_threads its maximum (0 then). */
void* gfx_warm_env(void);
unsigned gfx_warm_threads(void);
