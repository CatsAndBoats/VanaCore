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

/* Render visual 26 (the grass in red, the rest grey) with Mass Grass's rings shown: gfx_d3d12.c's composite adds a half to the
 * view's number then (grass_rings_shown; read as int elsewhere), and this, at the top of the view's branch (c the scene's colour,
 * z its view depth, px the pixel), keeps a pixel of one of the rings' five colours (grass.c draw_rings' col[]; drawn with no
 * depth, so not the grass's) as it is, and shades the band where the tufts shrink, 0.8 of the radius to it, blue to red, as the
 * shadows' rings tint theirs (ring_tint), the grass in it less: u.plight then holds the player's feet and the radius
 * (grass_rings_view; the distance across the ground, world up taken out) */
#define GRASS_RINGS_HLSL                                                                                                         \
    "    if (frac(u.grade.w) > 0.25) {\n"                                                                                         \
    "      float3 r0 = abs(c.rgb - float3(0.251, 1.0, 0.251)), r1 = abs(c.rgb - float3(1.0, 0.878, 0.251));\n"                    \
    "      float3 r2 = abs(c.rgb - float3(1.0, 0.502, 0.125)), r3 = abs(c.rgb - float3(0.251, 0.627, 1.0));\n"                    \
    "      float3 r4 = abs(c.rgb - float3(1.0, 0.188, 0.188));\n"                                                                 \
    "      float rd = min(min(min(max(r0.x, max(r0.y, r0.z)), max(r1.x, max(r1.y, r1.z))), min(max(r2.x, max(r2.y, r2.z)),\n"    \
    "                 max(r3.x, max(r3.y, r3.z)))), max(r4.x, max(r4.y, r4.z)));\n"                                               \
    "      if (rd < 0.04) return c;\n"                                                                                            \
    "      float3 gv = z != 0.0 ? view_pos(i.pos.xy, z) - u.plight.xyz : float3(0.0, 0.0, 0.0);\n"                              \
    "      float gt = (length(gv - u.up.xyz * dot(gv, u.up.xyz)) / max(u.plight.w, 1.0) - 0.8) * 5.0;\n"                          \
    "      if (u.plight.w > 0.0 && z != 0.0 && gt >= 0.0 && gt <= 1.0) {\n"                                                        \
    "        float gl = dot(c.rgb, LUMA);\n"                                                                                      \
    "        bool gg = texel(8, px).r != texel(2, px).r;\n"                                                                        \
    "        float3 gb = gg ? float3(1.0, 0.1, 0.1) * (0.45 + 0.55 * saturate(2.0 * gl)) : gl.xxx;\n"                            \
    "        float3 gk = lerp(float3(0.251, 0.627, 1.0), float3(1.0, 0.188, 0.188), gt) * (0.35 + 0.65 * gl);\n"                 \
    "        return float4(lerp(gb, gk, gg ? 0.3 : 0.6), c.a);\n"                                                                  \
    "      }\n"                                                                                                                   \
    "    }\n"
