/* The tufts' vertex function (g_tuft_vs) and the token macros it's written in. grass.c includes this where it stood, after
 * its constants' makers (kept_state, kind_light, grass_slope.h slope_light), which name them. It is no file of its own. */
#pragma once

/* The tufts' vertex function, vs.1.1 tokens (translated like the game's own, gfx_hlsl_shaders.c). In (PVert): v0 the root from the
 * square's middle and u, v8 the corner from the root and v, v5 its colour, v9 the keep draw and height band (PVert.keep). The
 * constants: c0-c3 the view-projection's columns, c4 the player, c6 (-1 / 0.2R, 5, the edge's thinning: -1 / (1 - KEEP_EDGE) R,
 * 1 / (1 - KEEP_EDGE)), c7 (0, 1, 0.3, 1e-6), c8 / c9 / c10 the sway's waves (x's share, z's, the time's, in turns), c11
 * (2, -1, 4, 32), c12 / c13 the rings' ramps (dd * c13 + c12, each 0 to 1) and c14 their steps, c15 the square's middle and its
 * roots' step (GChunk.org), c16 (the corners' step, 1 / 32767, the kind's sway share, 0), c17 the light on them (k, k, k, 1:
 * grass_set_light, times the colour), c18 / c19 the game's two directional lights' directions in the world, c20-c22 their slope
 * light's terms (grass_slope.h slope_light). v9.zw (PVert.keep's blue and alpha) the ground's light normal's x and z. */
#define VS_D(type, n, mask) (0x80000000u | ((uint32_t)(type) << 28) | ((uint32_t)(mask) << 16) | (n))
#define VS_S(type, n, sw) (0x80000000u | ((uint32_t)(type) << 28) | ((uint32_t)(sw) << 16) | (n))
#define VS_NEG(src) ((src) | (1u << 24))
#define SW(a, b, c, d) ((a) | (b) << 2 | (c) << 4 | (d) << 6)
#define XYZW SW(0, 1, 2, 3)
#define XXXX SW(0, 0, 0, 0)
#define YYYY SW(1, 1, 1, 1)
#define ZZZZ SW(2, 2, 2, 2)
#define WWWW SW(3, 3, 3, 3)
#define TR(n, sw) VS_S(0, n, sw) /* r# */
#define VR(n, sw) VS_S(1, n, sw) /* v# */
#define CR(n, sw) VS_S(2, n, sw) /* c# */
#define TD(n, mask) VS_D(0, n, mask)
static const uint32_t g_tuft_vs[] = {
    0xFFFE0101u, /* vs.1.1 */
    /* r8: the root (w: how far the corner sways, its height over the root times the kind's share); r9: the corner from the root;
     * r10: the texture coordinate */
    4 /* mad */, TD(8, 0x7), VR(0, XYZW), CR(15, WWWW), CR(15, XYZW),
    5 /* mul */, TD(9, 0x7), VR(8, XYZW), CR(16, XXXX),
    11 /* max */, TD(8, 0x8), VS_NEG(TR(9, YYYY)), CR(7, XXXX),
    5 /* mul */, TD(8, 0x8), TR(8, WWWW), CR(16, ZZZZ),
    1 /* mov */, TD(10, 0xF), CR(7, SW(0, 0, 0, 1)),
    5 /* mul */, TD(10, 0x1), VR(0, WWWW), CR(16, YYYY),
    5 /* mul */, TD(10, 0x2), VR(8, WWWW), CR(16, YYYY),
    /* r1.y: how far the root is from the player over the ground; r0.y: above or below */
    3 /* sub */, TD(0, 0xF), TR(8, XYZW), CR(4, XYZW),
    5 /* mul */, TD(1, 0xF), TR(0, XYZW), TR(0, XYZW),
    2 /* add */, TD(1, 0x1), TR(1, XXXX), TR(1, ZZZZ),
    2 /* add */, TD(1, 0x1), TR(1, XXXX), CR(7, WWWW),
    7 /* rsq */, TD(1, 0x2), TR(1, XXXX),
    5 /* mul */, TD(1, 0x2), TR(1, XXXX), TR(1, YYYY),
    /* r2.x grown (whole to 0.8 R, nothing at R), r2.y kept (all to 0.65 R, none at R), r2.z this one kept */
    4 /* mad */, TD(2, 0x3), TR(1, YYYY), CR(6, SW(0, 2, 0, 2)), CR(6, SW(1, 3, 1, 3)),
    11 /* max */, TD(2, 0x3), TR(2, XYZW), CR(7, XXXX),
    10 /* min */, TD(2, 0x3), TR(2, XYZW), CR(7, YYYY),
    /* the rings: the share kept falls by c14's steps over c12/c13's ramps (whole near the player, then 80%, 50%, 30%) */
    4 /* mad */, TD(7, 0x7), TR(1, YYYY), CR(13, XYZW), CR(12, XYZW),
    11 /* max */, TD(7, 0x7), TR(7, XYZW), CR(7, XXXX),
    10 /* min */, TD(7, 0x7), TR(7, XYZW), CR(7, YYYY),
    8 /* dp3 */, TD(7, 0x8), TR(7, XYZW), CR(14, XYZW),
    2 /* add */, TD(7, 0x8), CR(7, YYYY), VS_NEG(TR(7, WWWW)),
    5 /* mul */, TD(2, 0x2), TR(2, YYYY), TR(7, WWWW),
    13 /* sge */, TD(2, 0x4), TR(2, YYYY), VR(9, XXXX),
    /* r3.z: within its height band (v9.y * 32) and 0.3 of the distance of the player's height */
    11 /* max */, TD(3, 0x1), TR(0, YYYY), VS_NEG(TR(0, YYYY)),
    5 /* mul */, TD(3, 0x2), VR(9, YYYY), CR(11, WWWW),
    4 /* mad */, TD(3, 0x2), TR(1, YYYY), CR(7, ZZZZ), TR(3, YYYY),
    13 /* sge */, TD(3, 0x4), TR(3, YYYY), TR(3, XXXX),
    /* r2.w: the tuft's scale; r4 the vertex scaled about the root */
    5 /* mul */, TD(2, 0x8), TR(2, XXXX), TR(2, ZZZZ),
    5 /* mul */, TD(2, 0x8), TR(2, WWWW), TR(3, ZZZZ),
    4 /* mad */, TD(4, 0x7), TR(9, XYZW), TR(2, WWWW), TR(8, XYZW),
    1 /* mov */, TD(4, 0x8), CR(7, YYYY),
    /* the sway: two waves over the field (x, z), each sin(pi y) of y = 2 frac(turns) - 1 as 4 y (1 - |y|) */
    5 /* mul */, TD(5, 0x3), TR(8, XXXX), CR(8, XYZW),
    4 /* mad */, TD(5, 0x3), TR(8, ZZZZ), CR(9, XYZW), TR(5, XYZW),
    2 /* add */, TD(5, 0x3), TR(5, XYZW), CR(10, XYZW),
    19 /* frc */, TD(5, 0x3), TR(5, XYZW),
    4 /* mad */, TD(5, 0x3), TR(5, XYZW), CR(11, XXXX), CR(11, YYYY),
    11 /* max */, TD(6, 0x3), TR(5, XYZW), VS_NEG(TR(5, XYZW)),
    2 /* add */, TD(6, 0x3), CR(7, YYYY), VS_NEG(TR(6, XYZW)),
    5 /* mul */, TD(5, 0x3), TR(5, XYZW), TR(6, XYZW),
    5 /* mul */, TD(5, 0x3), TR(5, XYZW), CR(11, ZZZZ),
    5 /* mul */, TD(6, 0x1), TR(8, WWWW), TR(2, WWWW),
    4 /* mad */, TD(4, 0x1), TR(5, XXXX), TR(6, XXXX), TR(4, XXXX),
    4 /* mad */, TD(4, 0x4), TR(5, YYYY), TR(6, XXXX), TR(4, ZZZZ),
    20 /* m4x4 */, VS_D(4, 0, 0xF) /* oPos */, TR(4, XYZW), CR(0, XYZW),
    /* the light on its slope (grass_slope.h): r0 its ground's normal, x and z from v9.zw (0 to 1 for -1 to 1), y the rest of it,
     * up (y is down: negative); r1.xy its n . l for c18's and c19's lights, at least 0; r3 c20 + c21 r1.x + c22 r1.y (each
     * constant through r5: one a step), times c17 */
    4 /* mad */, TD(0, 0x5), VR(9, SW(2, 2, 3, 3)), CR(11, XXXX), CR(11, YYYY),
    5 /* mul */, TD(1, 0x5), TR(0, XYZW), TR(0, XYZW),
    2 /* add */, TD(1, 0x1), TR(1, XXXX), TR(1, ZZZZ),
    2 /* add */, TD(1, 0x1), CR(7, YYYY), VS_NEG(TR(1, XXXX)),
    11 /* max */, TD(1, 0x1), TR(1, XXXX), CR(7, WWWW),
    7 /* rsq */, TD(1, 0x2), TR(1, XXXX),
    5 /* mul */, TD(0, 0x2), VS_NEG(TR(1, XXXX)), TR(1, YYYY),
    8 /* dp3 */, TD(1, 0x1), TR(0, XYZW), CR(18, XYZW),
    8 /* dp3 */, TD(1, 0x2), TR(0, XYZW), CR(19, XYZW),
    11 /* max */, TD(1, 0x3), TR(1, XYZW), CR(7, XXXX),
    1 /* mov */, TD(5, 0x7), CR(21, XYZW),
    4 /* mad */, TD(3, 0x7), TR(5, XYZW), TR(1, XXXX), CR(20, XYZW),
    1 /* mov */, TD(5, 0x7), CR(22, XYZW),
    4 /* mad */, TD(3, 0x7), TR(5, XYZW), TR(1, YYYY), TR(3, XYZW),
    1 /* mov */, TD(3, 0x8), CR(7, YYYY),
    5 /* mul */, TD(3, 0xF), TR(3, XYZW), CR(17, XYZW),
    5 /* mul */, VS_D(5, 0, 0xF) /* oD0 */, VR(5, XYZW), TR(3, XYZW),
    1 /* mov */, VS_D(6, 0, 0xF) /* oT0 */, TR(10, XYZW),
    0x0000FFFFu,
};
