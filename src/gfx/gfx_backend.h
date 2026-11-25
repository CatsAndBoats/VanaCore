/* VanaCore's graphics back ends: Direct3D 12 (gfx_d3d12.c, renderer=d3d12) and Direct3D 11
 * (gfx_d3d11.c, renderer=d3d11). Each defines gfx.h's device functions under a prefix of its own
 * (gfx_rename.h: gfx12_draw, gfx11_draw), and gfx_dispatch.c's gfx_* go to the one gfx_select chose,
 * so the front end (d3d8.c) calls gfx_draw as it always has.
 *
 * The frame profile and the flicker watch (gfx_profiling, gfx_now_ns, gfx_prof_*, gfx_watch,
 * gfx_watch_dump_due) are one for both: gfx_d3d12.c's. */
#pragma once

#include "gfx.h"

/* return type, name (after gfx_), parameters, the arguments passed on */
#define GFX_BACKEND_FUNCTIONS(X)                                                                                       \
    X(int, init, (void* window, int vsync), (window, vsync))                                                           \
    X(void, set_vsync, (int on), (on))                                                                                 \
    X(const char*, adapter_name, (void), ())                                                                           \
    X(int, video_memory, (unsigned long long* used, unsigned long long* budget), (used, budget)) \
    X(void, resize, (uint32_t w, uint32_t h), (w, h))                                                                  \
    X(GfxBuf*, buf_create, (uint32_t size), (size))                                                                    \
    X(void, buf_destroy, (GfxBuf * b), (b))                                                                            \
    X(void, buf_upload, (GfxBuf * b, const void* data, uint32_t size), (b, data, size))                                \
    X(GfxTex*, tex_create, (int type, uint32_t d3dfmt, uint32_t w, uint32_t h, uint32_t levels, int use),              \
        (type, d3dfmt, w, h, levels, use))                                                                             \
    X(void, tex_destroy, (GfxTex * t), (t))                                                                            \
    X(void, tex_upload, (GfxTex * t, uint32_t face, uint32_t level, const void* src, uint32_t pitch),                  \
        (t, face, level, src, pitch))                                                                                  \
    X(void, tex_upload_rect,                                                                                           \
        (GfxTex * t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* src,   \
            uint32_t pitch),                                                                                           \
        (t, face, level, x, y, w, h, src, pitch))                                                                      \
    X(void, tex_read, (GfxTex * t, uint32_t face, uint32_t level, void* dst, uint32_t pitch), (t, face, level, dst, pitch)) \
    X(void, tex_read_async, (GfxTex * t, uint32_t face, uint32_t level, void* dst, uint32_t pitch),                    \
        (t, face, level, dst, pitch))                                                                                  \
    X(void, copy,                                                                                                      \
        (GfxTex * src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex * dst, \
            uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy),                                                \
        (src, sface, slevel, sx, sy, w, h, dst, dface, dlevel, dx, dy))                                                \
    X(void, set_targets, (GfxTex * color, uint32_t face, uint32_t level, GfxTex * depth), (color, face, level, depth)) \
    X(void, clear,                                                                                                     \
        (uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,             \
            const uint32_t vp[6]),                                                                                     \
        (nrects, rects, flags, color, z, stencil, vp))                                                                 \
    X(void, draw, (const GfxDraw* d), (d))                                                                             \
    X(void, batch, (int open), (open))                                                                                 \
    X(void, scene_done, (GfxTex * color, const GfxScene* s), (color, s))                                               \
    X(void, scene_world_depth, (GfxTex * depth), (depth))                                                              \
    X(int, scene_snapshot, (GfxTex * color, GfxTex * *colour_copy, GfxTex * *depth_copy, uint32_t * w, uint32_t * h),    \
        (color, colour_copy, depth_copy, w, h))                                                                        \
    X(GfxTex*, scene_mirror, (const GfxTex* color, const GfxScene* s, float level, float scale, float reach),          \
        (color, s, level, scale, reach))                                                                               \
    X(void, scene_floors, (GfxTex * id, GfxTex * depth, int state), (id, depth, state))                                \
    X(int, scene_floors_wanted, (void), ())                                                                            \
    X(void, scene_rt_geometry, (const void* data, uint32_t bytes, uint32_t node_bytes, int scene, uint32_t attr_at, GfxTex* const* tex, uint32_t ntex), (data, bytes, node_bytes, scene, attr_at, tex, ntex))      \
    X(void, scene_rain_mask, (GfxTex * mask, float wet, float reflect, float puddles), (mask, wet, reflect, puddles))  \
    X(int, scene_rain_mask_wanted, (void), ())                                                                         \
    X(float, seek_reach, (float* stretch), (stretch))                                                                  \
    X(int, sun_record, (const char* path, uint32_t max_mb), (path, max_mb))                                            \
    X(void, glow, (GfxTex * src, const float* tint), (src, tint))                                                      \
    X(void, glow_over_grass, (GfxTex * color, GfxTex * depth), (color, depth))                                         \
    X(int, sky, (const GfxSky* s), (s))                                                                                \
    X(void, dlss_set, (int quality, const wchar_t* helper, const wchar_t* log), (quality, helper, log))                \
    X(int, dlss_jitter, (float* ndc), (ndc))                                                                           \
    X(int, dlss_state, (char* message, int size), (message, size))                                                     \
    X(void, dlss_framegen, (int count), (count))                                                                       \
    X(int, dlss_framegen_state, (char* message, int size), (message, size))                                            \
    X(unsigned, waiting, (void), ())                                                                                   \
    X(int, wait_progress, (unsigned bit, unsigned* done, unsigned* total), (bit, done, total))                         \
    X(int, wait_step, (unsigned bit, char* words, int size), (bit, words, size))                                       \
    X(void, fx_set, (const char* key, float v), (key, v))                                                              \
    X(void, fx_rebuild, (void), ())                                                                                    \
    X(void, trace_dump, (const char* path), (path))                                                                    \
    X(uint64_t, frame, (void), ())                                                                                     \
    X(void, present, (GfxTex * backbuffer), (backbuffer))                                                              \
    X(void, present_layer, (GfxTex * layer), (layer))                                                                  \
    X(int, present_backdrops, (const float* rects, int n), (rects, n))                                                 \
    X(void, present_layer_below, (GfxTex * layer), (layer))                                                            \
    X(void, finish, (void), ())                                                                                        \
    X(void, set_sync_pipelines, (int on), (on))                                                                        \
    X(uint32_t, failures, (void), ())                                                                                  \
    X(int, lost, (void), ())                                                                                           \
    X(int, shutdown, (void), ())

typedef struct GfxBackend
{
#define GFX_BACKEND_FIELD(ret, name, params, args) ret(*name) params;
    GFX_BACKEND_FUNCTIONS(GFX_BACKEND_FIELD)
#undef GFX_BACKEND_FIELD
} GfxBackend;

extern const GfxBackend gfx12_backend, gfx11_backend;

/* A back end's table, from its own (renamed) gfx_* functions: at the end of its file */
#define GFX_BACKEND_ENTRY(ret, name, params, args) gfx_##name,
#define GFX_BACKEND_TABLE(table) const GfxBackend table = { GFX_BACKEND_FUNCTIONS(GFX_BACKEND_ENTRY) };

/* The back end gfx.h's device functions go to: 0 Direct3D 12 (the default), 1 Direct3D 11. Before
 * gfx_init. */
void gfx_select(int d3d11);
/* 1 while Direct3D 11 is the one chosen */
int gfx_selected_d3d11(void);
/* Direct3D 11's FSR (gfx_d3d11.c), for gfx_d3d12.c's vanacore_gfx_set_upscaler and vanacore_gfx_fsr_supported: FSR's
 * sharpening and version, and whether it can run on this device (else 0, and why into why) */
void gfx11_upscaler(int upscaler, float sharpness, int version);
int gfx11_fsr_supported(char* why, int size);
/* gfx_d3d12.c's: 1 when its gfx_init failed because its shaders don't compile (Wine's and Proton's d3dcompiler_47 have
 * no Shader Model 5.1 resource arrays), where Direct3D 11's still do */
int gfx12_shaders_failed(void);
/* ... 1 when it failed because there's no device it can draw on: none at feature level 11_0, or one below resource
 * binding tier 2 (Wine's vkd3d on MoltenVK, a Mac under CrossOver), where Direct3D 11 may still have one */
int gfx12_device_refused(void);
/* ... tests: its device removed, as a driver timeout would (ID3D12Device5::RemoveDevice), and found lost, for gfx_shutdown
 * and gfx_init to make it again. 1 when done; 0 with no device up, or without ID3D12Device5 */
int gfx12_lose_device(void);

/* gfx_d3d12.c's, for both: at a frame's end, the flicker watch's line for the draws it left out */
void gfx_watch_skips(void);
/* ... and the scene effects' HLSL (Direct3D 12's registers; gfx_d3d11.c fits it to its own) */
const char* gfx_fx_hlsl(void);
/* ... and the pass over the zone's curved surfaces (zn_vs; zn_ps Direct3D 11's two targets, zn_ps1 Direct3D 12's one), which
 * wants ZN_REG defined before it: the register of its constants (a view matrix, a projection, the viewport's size) */
const char* gfx_zn_hlsl(void);
/* ... and what one of its entry points does, in words for the notice while it compiles ("fx_ao": "Ambient occlusion";
 * the entry point's own name for one it doesn't know) */
const char* gfx_fx_words(const char* entry);
