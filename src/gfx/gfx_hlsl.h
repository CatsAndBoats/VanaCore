/* HLSL generation for the Direct3D 12 back end (gfx_hlsl.c, gfx_hlsl_shaders.c): the same keys and
 * token streams as gfx_msl.h, as Shader Model 5.1. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

typedef struct Sb
{
    char* s;
    size_t len, cap;
} Sb;

void sb_printf(Sb* b, const char* fmt, ...);

#ifdef VANACORE
/* 1 generates for Direct3D 11 (gfx_d3d11.c): Shader Model 5.0, each stage's texture at a register of
 * its own (Texture2D t4-t11, TextureCube t12-t19, samplers s0-s7) where Direct3D 12's are indices into
 * its descriptor heaps. Set before the first gfx_hlsl_generate. */
extern int gfx_hlsl_d3d11;
#endif

/* The source holding vs_main and fs_main for a key pair, or NULL when a shader uses something the
 * translator does not know (malloc'd; the caller frees). */
char* gfx_hlsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens);

/* vs.1.0/1.1 and ps.1.0-1.3 token streams: the function bodies (after the signature, before the
 * fragment tail, which reads r0). 0 when the shader cannot be translated. */
int gfx_hlsl_vs1(Sb* b, const GfxVsKey* k, const uint32_t* tokens);
/* The vertex function's end: the shadow pass's matrix (GfxVsKey.shadow), then its return. */
void gfx_hlsl_vs_return(Sb* b, const GfxVsKey* k);
int gfx_hlsl_ps1(Sb* b, const GfxFsKey* k, const uint32_t* tokens);

/* The clip-space position (before the pixel centers' fixup) the draw's vertex function - its vertex shader
 * or the fixed function's transform - gives its first vertex, worked out on the CPU from GfxDraw.cpu. 0
 * when it can't be (no vertices on the CPU, XYZRHW, an opcode it doesn't know). */
int gfx_draw_clip0(const GfxDraw* d, float out[4]);
/* The same for the position pos (x, y, z) in place of the first vertex's: the draw's model-to-clip transform, a point at a time. */
int gfx_draw_clip_pos(const GfxDraw* d, const float pos[3], float out[4]);

/* The utility functions every device needs (present, the frame-rate overlay), one source. */
extern const char gfx_hlsl_util[];
