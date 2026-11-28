/* gfx_d3d12_rt_hlsl.h: the ray-traced reflections' part of the effects' HLSL (gfx_d3d12.c FX_HLSL; refl_rt), Direct3D 12's alone
 * (ZL_LATE): RT_TRACE_HLSL the walk through a tree (bvh_trace) and a triangle's colour (tri_tex), before refl_trace; RT_SSR_HLSL
 * fx_ssr's use of them, after the screen's own ray. Kept apart for the length of gfx_d3d12.c. */
#pragma once

/* The sky as a map for the ray-traced reflections (gfx_d3d12_rt.h sky_map_pass): SKYMAP_W x SKYMAP_H texels, the world's
 * directions above the horizon (azimuth across from -pi, the zenith at the top row, the horizon at the bottom), and one column
 * more at x = SKYMAP_W: this frame's one sky colour (fx_skycol's). Each texel takes the screen's sky in its direction while the
 * picture shows it, and keeps it (alpha 1, seen) when the camera turns away; one never seen is the one colour (alpha 0), and a
 * seen one eases toward it very slowly, so a sky that changes while it's out of view doesn't stay as it was for ever. A ray
 * that leaves the zone reads it (fx_ssr's input 11, u.rtx.z 1 while it's made): looking down from a stair, the sky over the
 * floor is above the picture, which had only the one colour for it, a flat grey under an overcast evening */
#define SKYMAP_W 256
#define SKYMAP_H 64
#define SKYMAP_WS "256"
#define SKYMAP_HS "64"
#define SKY_MAP_HLSL \
    "float4 sky_map(FO i) {\n" \
    "  int2 t = int2(i.pos.xy);\n" \
    "  float4 now = texel(3, int2(0, 0));\n" \
    "  if (t.x >= " SKYMAP_WS ") return float4(now.rgb, 1.0);\n" \
    "  float4 prev = texel(2, t);\n" \
    "  float seen = prev.a;\n" \
    "  float3 c = seen > 0.0 ? lerp(prev.rgb, now.rgb, 0.0005) : now.rgb;\n" \
    "  float az = (float(t.x) + 0.5) * (6.2831853 / " SKYMAP_WS ".0) - 3.1415927;\n" \
    "  float a = (1.0 - (float(t.y) + 0.5) / " SKYMAP_HS ".0) * 1.5707963;\n" \
    "  float3 dw = float3(cos(a) * cos(az), -sin(a), cos(a) * sin(az));\n" \
    "  float3 dv = float3(dot(u.rtx.xyz, dw), dot(u.rbf.xyz, dw), dot(u.bloom.xyz, dw));\n" \
    "  float3 q = to_px(dv * 1e4);\n" \
    "  if (q.z > 0.1 && in_view(q.xy) && view_z(texel(1, int2(q.xy)).r) == 0.0) {\n" \
    "    float3 sc = texel(0, int2(q.xy)).rgb;\n" \
    "    c = seen > 0.0 ? lerp(c, sc, 0.2) : sc;\n" \
    "    seen = 1.0;\n" \
    "  }\n" \
    "  return float4(c, seen);\n" \
    "}\n"

#define RT_TRACE_HLSL \
    /* a floor's own colour where it reflects (fx_refl, a its reflection's weight), by how wet it is (u.refl.y, half the strength, refl): wet stone \
     * is darker and its colour deeper. Dry to half way it is as drawn. With the reflection sharper the wetter (the blur, fx_ssr's \
     * rough_ and u.rtx.x), the strength is one look from a dry sheen to a wet mirror: wetness (refl_wet) was a setting of its own, \
     * and couldn't be told from the strength */ \
    "float3 wet_floor(float3 c, float a) {\n" \
    "  float k = smoothstep(0.3, 1.0, u.refl.y) * smoothstep(0.0, 0.2, a);\n" \
    "  float l = dot(c, LUMA);\n" \
    "  return lerp(c, max(lerp(float3(l, l, l), c, 1.25), 0.0) * 0.72, k);\n" \
    "}\n" \
    /* the floor's own relief on a traced reflection (fx_refl, at the scene's size, after the blur; refl_bump, u.elight2.y): the \
     * share of it a pixel keeps, less in a groove - a pixel darker than both its neighbours across it, which a shadow's edge, \
     * darker on one side alone, is not. For the screen's reflections fx_ssr takes it off the weight (its amt); done there for \
     * the traced ones, before their blur, the grooves and tile edges went soft with the reflection, Surface Depth doing nothing \
     * under a wide blur: they are left to this (u.rtx.y), 1 for the screen's */ \
    /* (looked for a pixel either side and three: a groove a few pixels wide is no darker than the pixel next to it, and by that \
     * one alone nothing showed at 1600 lines. Up to nine tenths off at refl_bump 2, the slider's top: at six it couldn't be told \
     * from none under a wide blur) */ \
    /* the share of the floor's relief (refl_bump, Surface Depth) a point of the floor takes, by where it is: none past 70 units \
     * from the camera, the texture's detail smaller than a pixel there; and with refl_bump_near (u.rbf.w, Surface Depth Fade: \
     * u.rbf.xyz the player's feet in view space) none at the player's feet, all of it that far from them - a clean mirror to \
     * stand in, the stones breaking it up farther off. The far fade starts later with it, so the whole of it is reached */ \
    /* whether the zone's collision floor at a scene pixel (the floors' depth, input 8) is tilted from the surface drawn there (N) by \
     * more than about 20 degrees: a stair, its treads drawn level over a ramp of collision. fx_ssr's floor test took a tread \
     * wherever it happened to lie near the ramp, and which those were changed with every step of the camera: patches of reflection \
     * coming and going over the steps (Whitegate, 2026-11-27). A sloped floor drawn as a slope is its own collision's slope. Each \
     * way the side nearer in depth, so a collision edge isn't read as a slope */ \
    "float floor_depth_at(int2 q) {\n" \
    "  return view_z(texel(8, clamp(q, int2(u.vp.xy), int2(u.vp.xy + u.vp.zw) - 1)).r);\n" \
    "}\n" \
    "bool floor_tilted(int2 q, float3 N) {\n" \
    "  float z0 = floor_depth_at(q);\n" \
    "  if (z0 == 0.0) return false;\n" \
    "  float zr = floor_depth_at(q + int2(2, 0)), zl = floor_depth_at(q - int2(2, 0));\n" \
    "  float zd = floor_depth_at(q + int2(0, 2)), zu = floor_depth_at(q - int2(0, 2));\n" \
    "  bool rx = zr != 0.0 && (zl == 0.0 || abs(zr - z0) <= abs(zl - z0)), ry = zd != 0.0 && (zu == 0.0 || abs(zd - z0) <= abs(zu - z0));\n" \
    "  float2 c = float2(q) + 0.5;\n" \
    "  float3 p0 = view_pos(c, z0);\n" \
    "  float3 dx = rx ? view_pos(c + float2(2.0, 0.0), zr) - p0 : p0 - view_pos(c - float2(2.0, 0.0), zl);\n" \
    "  float3 dy = ry ? view_pos(c + float2(0.0, 2.0), zd) - p0 : p0 - view_pos(c - float2(0.0, 2.0), zu);\n" \
    "  if ((zr == 0.0 && zl == 0.0) || (zd == 0.0 && zu == 0.0)) return false;\n" \
    "  float3 n = cross(dx, dy);\n" \
    "  float nn = dot(n, n);\n" \
    /* (... or from up by more than about 25 degrees: beside a character, N is worked out partly from the character's depth and \
     * tilts, and lined up with the ramp by chance, the treads round their outline took the sky's reflection, a pale band hugging \
     * them on a stair, Whitegate 2026-11-28. u.up is eased over frames, a few degrees off while the camera pitches: a stair's ramp \
     * is 35 and more) */ \
    "  if (!(nn > 1e-12)) return false;\n" \
    "  n *= rsqrt(nn);\n" \
    "  return abs(dot(n, N)) < 0.94 || abs(dot(n, u.up.xyz)) < 0.9;\n" \
    "}\n" \
    "float bump_fade(float3 P, float dist) {\n" \
    "  float nr = u.rbf.w;\n" \
    "  float fo = 1.0 - smoothstep(max(25.0, nr + 10.0), max(70.0, nr + 55.0), dist);\n" \
    "  return nr > 0.0 ? fo * smoothstep(0.0, nr, length(P - u.rbf.xyz)) : fo;\n" \
    "}\n" \
    "float relief_dip(int2 px, float lc, int s) {\n" \
    "  float lr = dot(texel(1, px + int2(s, 0)).rgb, LUMA), ll = dot(texel(1, px - int2(s, 0)).rgb, LUMA);\n" \
    "  float lu = dot(texel(1, px - int2(0, s)).rgb, LUMA), ld = dot(texel(1, px + int2(0, s)).rgb, LUMA);\n" \
    "  return (max(min(lr, ll), min(lu, ld)) - lc) / max(0.25 * (lr + ll + lu + ld), 0.04);\n" \
    "}\n" \
    /* 1 on a narrow strip of floor (fx_ssr's floor mask): 0.65 yalm either side of the pixel along the floor's own plane, across \
     * some direction, the picture is off that plane on both - a drop, or something rising. A low wall's top, a rail's, a crate's \
     * lid, a ledge along a wall is floor in the zone's collision, stone as often as not, and took a reflection - a strip of sky \
     * along the top of a wall. A platform's edge, or a floor's foot against a wall, is off on one side only; what is under 1.3 \
     * yalms across is no floor anyone walks. By the pixel's own normal, each place taken to the screen: measured along u.up, a \
     * step of pixels either side, a floor far off was higher on one side and lower on the other whenever the camera pitched (up \
     * is eased over frames) or the ground sloped, and the far half of the mask went black with every move of the camera. What \
     * stands well nearer than the place (someone in front of it) says nothing of it */ \
    /* (0 the floor goes on there, 1 it doesn't, 2 not known: off the picture. Counted as going on, a railing's top by the picture's \
     * edge kept its reflection whenever one side's place fell off it, and lost it when it didn't: white streaks along the top that \
     * came and went as the camera moved, Whitegate 2026-11-27. Now the side in the picture decides) */ \
    "int strip_off(float3 Q, float3 P, float3 N) {\n" \
    "  float3 q = to_px(Q);\n" \
    "  if (!(q.z > 0.1) || !in_view(q.xy)) return 2;\n" \
    "  float z = view_z(texel(0, int2(q.xy)).r);\n" \
    "  if (z == 0.0) return 1;\n" \
    "  float3 S = view_pos(floor(q.xy) + 0.5, z);\n" \
    "  return abs(dot(S - P, N)) > 0.3 && z * u.hand.x > q.z - 1.5 ? 1 : 0;\n" \
    "}\n" \
    "float narrow_top(float3 P, float3 N) {\n" \
    "  float3 t1 = cross(N, P);\n" \
    "  if (dot(t1, t1) < 1e-8) return 0.0;\n" \
    "  t1 = normalize(t1) * 0.65;\n" \
    "  float3 t2 = cross(N, t1);\n" \
    "  float nt = 0.0;\n" \
    "  [unroll] for (int k = 0; k < 4; ++k) {\n" \
    "    float3 d = k == 0 ? t1 : k == 1 ? t2 : (k == 2 ? t1 + t2 : t1 - t2) * 0.7071;\n" \
    "    int sa = strip_off(P + d, P, N), sb = strip_off(P - d, P, N);\n" \
    "    if (sa != 0 && sb != 0 && sa + sb < 4) nt = 1.0;\n" \
    "  }\n" \
    "  return nt;\n" \
    "}\n" \
    "float refl_relief(int2 px, float3 c, float dist) {\n" \
    "  float bump = min(u.elight2.y, 2.0) * 0.45 * bump_fade(view_pos(float2(px) + 0.5, dist * u.hand.x), dist);\n" \
    "  if (u.rtx.y < 0.5 || bump <= 0.0) return 1.0;\n" \
    "  int s = max(int(u.vp.w / 1080.0 + 0.5), 1);\n" \
    "  float lc = dot(c, LUMA);\n" \
    "  return 1.0 - bump * saturate(3.0 * max(relief_dip(px, lc, s), relief_dip(px, lc, 3 * s)));\n" \
    "}\n" \
    /* the zone's collision as a tree (rtbvh.h; Direct3D 12 only): its nodes, 32 bytes each, then its triangles, 36; the frame's \
     * words beside it. bvh_trace finds the nearest hit; with the meshes' tree, tri_tex its colour */ \
    "#ifdef ZL_LATE\n" \
    "ByteAddressBuffer g_bvh : register(t0, space3);\n" \
    "ByteAddressBuffer g_rtv : register(t1, space3);\n" \
    /* what the last walk met, for Render visual 33: 1 it ran out of steps, 2 two surfaces at one place, 4 its stack was full */ \
    "static uint g_rtf = 0u;\n" \
    "static float4 g_rts = float4(0.0, 0.0, 0.0, 0.0);\n" \
    /* the meshes' tree's own words for a triangle (ab: where they start, zonemesh.c: 10 words each): the three corners' uv (6), the \
     * three vertex colours (3: 0xAARRGGBB), and the texture (bits 0 to 15: its index + 1 in the frame words' list from byte 96, 0 \
     * none; bit 16: cut out by its alpha; bit 17: a thing that moves). The colour of the surface a ray found: vc the vertex colour there */ \
    "float4 tri_tex(uint ab, uint ti, float2 b, float lod, out float3 vc) {\n" \
    "  uint p = ab + ti * 40u;\n" \
    "  uint4 r0 = g_bvh.Load4(p), r1 = g_bvh.Load4(p + 16u);\n" \
    "  uint2 r2 = g_bvh.Load2(p + 32u);\n" \
    "  float w0 = 1.0 - b.x - b.y;\n" \
    "  float2 uv = asfloat(r0.xy) * w0 + asfloat(r0.zw) * b.x + asfloat(r1.xy) * b.y;\n" \
    "  float3 k0 = float3(float((r1.z >> 16) & 255u), float((r1.z >> 8) & 255u), float(r1.z & 255u));\n" \
    "  float3 k1 = float3(float((r1.w >> 16) & 255u), float((r1.w >> 8) & 255u), float(r1.w & 255u));\n" \
    "  float3 k2 = float3(float((r2.x >> 16) & 255u), float((r2.x >> 8) & 255u), float(r2.x & 255u));\n" \
    "  vc = (k0 * w0 + k1 * b.x + k2 * b.y) * (1.0 / 255.0);\n" \
    "  uint tx = r2.y & 0xFFFFu;\n" \
    "  if (tx == 0u) return float4(0.5, 0.5, 0.5, 1.0);\n" \
    "  uint sl = g_rtv.Load(96u + (tx - 1u) * 4u);\n" \
    "  if (sl == 0u) return float4(0.5, 0.5, 0.5, 1.0);\n" \
    "  return rt_tex(sl, uv - floor(uv), lod);\n" \
    "}\n" \
    /* the nearest hit along the ray, children taken nearest first, a found hit shortening the ray so the far boxes are skipped; the \
     * first triangle met is not the nearest, and a far wall's colour showed through a near one. With `first`, any hit at all ends it \
     * (a ray toward the sun, which only asks whether something stands in the way) */ \
    "float box_t(float4 n0, float4 n1, float3 o, float3 inv, float tmin, float tm) {\n" \
    "  float3 ta = (n0.xyz - o) * inv, tc = (n1.xyz - o) * inv;\n" \
    "  float3 tn = min(ta, tc), tf = max(ta, tc);\n" \
    "  float te = max(max(tn.x, tn.y), max(tn.z, tmin)), tl = min(min(tf.x, tf.y), min(tf.z, tm));\n" \
    "  return te <= tl ? te : 1e30;\n" \
    "}\n" \
    "float bvh_trace(float3 o, float3 d, float tmin, float tmax, uint tb, uint ab, bool first, out uint ht, out float2 hb) {\n" \
    "  ht = 0u; hb = float2(0.0, 0.0);\n" \
    "  float3 inv = 1.0 / (d + float3(d.x == 0.0 ? 1e-8 : 0.0, d.y == 0.0 ? 1e-8 : 0.0, d.z == 0.0 ? 1e-8 : 0.0));\n" \
    "  uint stack[32];\n" \
    "  uint sp = 0u;\n" \
    "  float best = -1.0, tm = tmax;\n" \
    "  bool done = false;\n" \
    "  g_rtf = 0u;\n" \
    /* (n0, n1: the node the walk stands in, met: the ray goes through its box. A child gone down into was read and tested by its \
     * parent, and is not read and tested again: every node was, two reads and a box test of each of the walk's steps for nothing - \
     * a third of the reflection ray's time. Only a node taken off the stack is tested, the ray having shortened since it was put by) */ \
    "  float4 n0 = asfloat(g_bvh.Load4(0u)), n1 = asfloat(g_bvh.Load4(16u));\n" \
    "  bool met = box_t(n0, n1, o, inv, tmin, tm) < 1e29;\n" \
    "  [loop] for (int it = 0; it < 640; ++it) {\n" \
    "    bool pop = true;\n" \
    "    if (met) {\n" \
    "      uint a = asuint(n0.w), cnt = asuint(n1.w);\n" \
    "      if (cnt > 0u) {\n" \
    "        for (uint k = 0u; k < cnt; ++k) {\n" \
    "          uint tp = tb + (a + k) * 36u;\n" \
    "          float3 v0 = asfloat(g_bvh.Load3(tp)), e1 = asfloat(g_bvh.Load3(tp + 12u)), e2 = asfloat(g_bvh.Load3(tp + 24u));\n" \
    "          float3 pv = cross(d, e2);\n" \
    "          float det = dot(e1, pv);\n" \
    "          if (abs(det) > 1e-9) {\n" \
    "            float idt = 1.0 / det;\n" \
    "            float3 tv = o - v0;\n" \
    "            float uu = dot(tv, pv) * idt;\n" \
    "            float3 qv = cross(tv, e1);\n" \
    "            float vv = dot(d, qv) * idt;\n" \
    "            float t = dot(e2, qv) * idt;\n" \
    "            if (uu >= 0.0 && vv >= 0.0 && uu + vv <= 1.0 && t > tmin && t < tm) {\n" \
    /* (g_rts: the ground lying level within its w of the ray's start is passed by - ground laid over ground, a path or a drain's \
     * rim a hand above the pavement, met at once from beneath. Only what lies level: the ray used to start that far along \
     * instead, and beside a wall it started past the wall, found the sky, and left a pale blue strip along the wall's foot) */ \
    "              if (t < g_rts.w) { float3 gn = cross(e1, e2); if (abs(dot(gn, g_rts.xyz)) > 0.7 * length(gn)) continue; }\n" \
    /* (two surfaces at one place - a tile's triangle stored twice, a far copy laid over the near one - each a hair nearer than the \
     * other from one ray to the next: the one first in the tree is taken, always. By which was nearer, the two traded places with \
     * every small move of the ray, pixel to pixel and frame to frame, and that part of the reflection flickered. tm stays a hair \
     * past the hit so the other is still met) */ \
    "              float te = 0.002 + 0.0003 * t;\n" \
    "              if (best > 0.0 && t > best - te) { g_rtf |= 2u; if (a + k > ht) continue; }\n" \
    "              /* (a cut-out's clear texel is no surface: the ray goes on) */\n" \
    /* (only a cut-out mesh's, bit 16, at the game's cut: the game draws every other mesh solid whatever its texture's alpha holds - \
     * Bastok Markets' stone and wood are clear over nine texels in ten, a mask for the decals blended over them - and a cut by it \
     * left holes in every wall) */ \
    "              if (ab != 0u && (g_bvh.Load(ab + (a + k) * 40u + 36u) & 0x10000u) != 0u) {\n" \
    "                float3 vcx;\n" \
    "                if (tri_tex(ab, a + k, float2(uu, vv), 0.0, vcx).a < 0.185) continue;\n" \
    "              }\n" \
    "              best = t; tm = t + te;\n" \
    "              ht = a + k; hb = float2(uu, vv);\n" \
    "              if (first) return t;\n" \
    "            }\n" \
    "          }\n" \
    "        }\n" \
    "      } else if (sp < 30u) {\n" \
    "        uint cb = a + 1u;\n" \
    "        float4 a0 = asfloat(g_bvh.Load4(a * 32u)), a1 = asfloat(g_bvh.Load4(a * 32u + 16u));\n" \
    "        float4 b0 = asfloat(g_bvh.Load4(cb * 32u)), b1 = asfloat(g_bvh.Load4(cb * 32u + 16u));\n" \
    "        float ea = box_t(a0, a1, o, inv, tmin, tm), eb = box_t(b0, b1, o, inv, tmin, tm);\n" \
    "        if (ea < 1e29 || eb < 1e29) {\n" \
    "          bool na = ea <= eb;\n" \
    "          if (na ? eb < 1e29 : ea < 1e29) stack[sp++] = na ? cb : a;\n" \
    "          if (na) { n0 = a0; n1 = a1; } else { n0 = b0; n1 = b1; }\n" \
    "          pop = false;\n" \
    "        }\n" \
    "      } else g_rtf |= 4u;\n" \
    "    }\n" \
    "    if (pop) {\n" \
    "      if (sp == 0u) { done = true; break; }\n" \
    "      uint nb = stack[--sp] * 32u;\n" \
    "      n0 = asfloat(g_bvh.Load4(nb)); n1 = asfloat(g_bvh.Load4(nb + 16u));\n" \
    "      met = box_t(n0, n1, o, inv, tmin, tm) < 1e29;\n" \
    "    }\n" \
    "  }\n" \
    "  if (!done) g_rtf |= 1u;\n" \
    "  return best;\n" \
    "}\n" \
    "#endif\n" \
    ""

#define RT_SSR_HLSL \
    /* ray-traced reflections (refl_rt; Direct3D 12 only, ZL_LATE): where the screen has no hit, the ray goes on through the tree of \
     * the zone's collision (rtbvh.h), in the world: g_rtv has the view's inverse to take the ray there, and says whether a tree is \
     * bound (word 16; word 17 where its triangles start) */ \
    "  bool rthit = false;\n" \
    "  float3 rtc = float3(0.0, 0.0, 0.0), rtd = float3(1.0, 0.0, 0.0);\n" \
    "  float rtw = 0.0;\n" \
    "#ifdef ZL_LATE\n" \
    /* (word 16 is 2 with the zone's meshes bound, refl_rt 2: they take every ray, not only the misses, so what the floor shows \
     * doesn't move with the camera) */ \
    "  uint4 rti = g_rtv.Load4(64u);\n" \
    "  float3 upn = asfloat(g_rtv.Load3(80u));\n" \
    "  if (amt > 0.003 && (h.w < 0.5 || h.w > 2.5 || rti.x == 2u) && dot(R, upn) > 0.02) {\n" \
    "    if (rti.x != 0u) {\n" \
    "      float4 m0 = asfloat(g_rtv.Load4(0u)), m1 = asfloat(g_rtv.Load4(16u)), m2 = asfloat(g_rtv.Load4(32u)), m3 = asfloat(g_rtv.Load4(48u));\n" \
    "      float3 ow = P.x * m0.xyz + P.y * m1.xyz + P.z * m2.xyz + m3.xyz;\n" \
    /* (the tree's ray is tilted by the floor's relief as the screen's is, Nb's tilt off N: refl_bump, Surface Depth. It went by \
     * the clean normal for a while - tilted, each stone and tile edge shows another part of what it reflects, which read as a \
     * checker of lines - and Surface Depth then did next to nothing with the reflections traced: a flat mirror under the stones, \
     * where the screen's were wet cobbles. The slider is the say in it: at 0 it is the mirror) */ \
    /* (N is worked out from the depth, a little different at each pixel and frame: the screen's blur hid it, the tree's sharp hits \
     * showed it as a jitter and patches of other colours. A floor is flat for the most part: drawn most of the way to up) */ \
    "      float3 Nf = normalize(lerp(N, upn, 0.85));\n" \
    /* (and not lifted: refl_lift tilts a low ray up because the screen has nothing to show along the floor; the tree has the whole zone, \
     * and the tilt, which changes with the view's angle, swam the reflection like jelly as the camera moved up and down) */ \
    "      float3 Rt = reflect(-V, normalize(Nf + (Nb - N)));\n" \
    "      float3 dw = Rt.x * m0.xyz + Rt.y * m1.xyz + Rt.z * m2.xyz;\n" \
    "      float rtmax = max(u.refl.w, 100.0);\n" \
    "      uint ht; float2 hb;\n" \
    /* (the meshes' tree starts the ray off the floor it leaves: ground laid over ground, a path or a drain's rim a hand above the \
     * pavement, is a surface the ray meets at once from beneath, a dark band that isn't there) */ \
    "      g_rts = float4(upn.x * m0.xyz + upn.y * m1.xyz + upn.z * m2.xyz, rti.x == 2u ? max(0.03 * dist + 0.05, 0.4) : 0.0);\n" \
    "      float th = bvh_trace(ow, dw, rti.x == 2u ? 0.02 : 0.03 * dist + 0.05, rtmax, rti.y, rti.x == 2u ? rti.z : 0u, false, ht, hb);\n" \
    "      g_rts.w = 0.0;\n" \
    "      uint rtf = g_rtf;\n" \
    /* (the meshes' tree has the zone and no one in it: where the screen found a surface nearer than the tree's, a player, a \
     * monster, a thing that moves, the screen's is shown) */ \
    /* (only a character: the screen's ray often finds the floor itself a hand away at a low angle, which is no one standing there, \
     * and took the tree's answer from half the floor) */ \
    /* (the characters' mask is bound only while the rain wets them, so it can't say which is one: a surface the screen found \
     * clearly nearer than the tree's, and well above the floor the ray left, is something standing in front of it) */ \
    /* (... or over a thing that moves, bit 17 - a door, a lift, in the tree where the zone's file has it and on the screen where the \
     * game draws it, not the same place: whatever the screen's ray found, nearer than it or not) */ \
    /* (... but where the characters' mask is there (u.aoc.w; input 10 with the ray tracing on, cmask_in_ssr), a hit on a character \
     * is one at any height: by the height alone, everyone's reflection was cut off at the knees, the 0.4 above the floor the \
     * tree's wall or floor behind them, Port Jeuno 2026-11-27) */ \
    "      bool mover = rti.x == 2u && th > 0.0 && (g_bvh.Load(rti.z + ht * 40u + 36u) & 0x20000u) != 0u;\n" \
    /* (... where the ray truly meets them: the screen's ray "hits" where it passes behind a surface by up to its thickness, a yalm \
     * and more at ten, and a ray from the steps behind a player passing behind them took them for a hit, a pale echo of their \
     * outline on the steps, Whitegate 2026-11-28. Their surface within a third of a yalm of the ray's point, nearer than the tree's) */ \
    "      bool hchr = rti.x == 2u && u.aoc.w > 0.0 && h.w > 0.5 && h.w < 1.5 && texel(10, int2(h.xy)).r > 0.5 && (th <= 0.0 || tt < th) &&\n" \
    "        length(pos_at(0, h.xy) - (P + R * tt)) < 0.35 + 0.01 * tt;\n" \
    "      bool nearer = rti.x == 2u && h.w > 0.5 && h.w < 1.5 && (hchr || ((th <= 0.0 || mover || tt < th - 0.3) && tt * dot(R, upn) > 0.4));\n" \
    /* (... and not the zone's own: the screen's ray goes by the bumped normal, tilted by the floor's grooves, and found a wall's \
     * foot or a step nearer than the tree's clean ray did - a speckle of the screen's reflection across the floor, each pixel \
     * dithered anew every frame, which flickered. So the tree is asked for the very point the screen found: a surface of its own \
     * there, and the tree's ray stands - its hit, or the sky where it met nothing: only what the tree hasn't is the screen's) */ \
    "      if (nearer && !hchr) {\n" \
    "        float3 Hs = pos_at(0, h.xy) - P;\n" \
    "        float hl = max(length(Hs), 1e-3), ht_ = 0.35 + 0.02 * hl;\n" \
    "        uint ct; float2 cb_;\n" \
    "        float3 hdw = (Hs.x * m0.xyz + Hs.y * m1.xyz + Hs.z * m2.xyz) / hl;\n" \
    "        if (bvh_trace(ow, hdw, max(hl - ht_, 0.05), hl + ht_, rti.y, rti.z, true, ct, cb_) > 0.0 && (g_bvh.Load(rti.z + ct * 40u + 36u) & 0x20000u) == 0u) nearer = false;\n" \
    "      }\n" \
    "      if (th > 0.0 && !nearer) {\n" \
    "        float lm = luma_at(px).x;\n" \
    "        rthit = true;\n" \
    "        rtw = 1.0 - smoothstep(0.5 * rtmax, rtmax, th);\n" \
    "        rtc = lerp(c.rgb, float3(lm, lm, lm), 0.5) * 0.8;\n" \
    /* the meshes' tree: the surface's own texture, by its vertices' colours (the zone's baked light) and the light the floor has here */ \
    "        if (rti.x == 2u) {\n" \
    "          float3 vc;\n" \
    "          float3 tc = tri_tex(rti.z, ht, hb, min(1.5 + log2(1.0 + th * 0.05) * 1.5, 6.0), vc).rgb;\n" \
    /* the light on it as the game lights the zone's own draws: its vertex colour by the scene's ambient light (the frame's word 23, \
     * 10 bits a channel of 0 to 2) and the sun's by the triangle's facing (its edges, turned to the ray), then its texture, twice \
     * over. A guess at it from the fog's colour was half as bright again as the screen's picture of the same wall in the shade */ \
    "          uint tq = rti.y + ht * 36u;\n" \
    "          float3 nw = cross(asfloat(g_bvh.Load3(tq + 12u)), asfloat(g_bvh.Load3(tq + 24u)));\n" \
    "          float3 nv = float3(dot(m0.xyz, nw), dot(m1.xyz, nw), dot(m2.xyz, nw));\n" \
    "          nv *= rsqrt(max(dot(nv, nv), 1e-12));\n" \
    "          if (dot(nv, Rt) > 0.0) nv = -nv;\n" \
    "          uint aw = g_rtv.Load(92u);\n" \
    "          float3 amb = float3(float(aw & 1023u), float((aw >> 10) & 1023u), float((aw >> 20) & 1023u)) * (1.0 / 511.0);\n" \
    "          float ndl = u.sun.w > 0.0 ? saturate(dot(nv, u.sun.xyz)) : 0.0;\n" \
    "          rtc = tc * saturate(vc * (amb + u.suncol.rgb * ndl)) * 2.0;\n" \
    /* the tree's colour has no sun shadow, occlusion, colour grade or the game's own lighting (they are made in later passes): where \
     * the screen shows the very point the tree found (taken to the screen, the screen's depth there the same), the screen's pixel has \
     * them all, so it is taken, easing out toward the picture's edges to the tree's */ \
    /* (eased in, not switched: by how near the screen's depth there is to the point's, how squarely the surface faces the camera - \
     * a wall seen edge-on matches by chance, a pixel on and one off, the screen's bright stone and the tree's darker colour in a \
     * patchwork of blocks - and how far in from the picture's edges) */ \
    "          float3 Hv = P + Rt * th, hq = to_px(Hv), scol = rtc;\n" \
    "          float sbw = 0.0;\n" \
    "          if (hq.z > 0.1 && in_view(hq.xy)) {\n" \
    "            float tol = 0.02 * hq.z + 0.15;\n" \
    "            float dm = 1.0 - smoothstep(tol, 2.0 * tol, abs(dist_at(hq.xy) - hq.z));\n" \
    /* (not a character's pixel there: one standing against the wall the tree's ray met, within its depth, came out a second, \
     * ghost reflection of them inside the wall's, Whitegate 2026-11-27) */ \
    "            if (u.aoc.w > 0.0 && texel(10, int2(hq.xy)).r > 0.5) dm = 0.0;\n" \
    "            float fc = smoothstep(0.2, 0.45, dot(nv, -normalize(Hv)));\n" \
    "            float2 se = (hq.xy - u.vp.xy) / u.vp.zw;\n" \
    "            float sfd = smoothstep(0.0, 0.2, min(se.x, 1.0 - se.x)) * smoothstep(0.0, 0.2, min(se.y, 1.0 - se.y));\n" \
    "            sbw = dm * fc * sfd;\n" \
    "            scol = tap(1, hq.xy / u.size.xy).rgb;\n" \
    "          }\n" \
    /* the sun's shadow on what the screen doesn't show: the screen's picture has the shadows laid over it (fx_comp, by the sun's \
     * maps), and a wall the tree alone finds had none - sunlit in a courtyard that lies in shade, twice and more as bright as the \
     * same stone beside it on the screen. A second ray, from the point toward the sun through the same tree, any hit ending it; \
     * then as the maps' shadow is laid: sun_min in the shade, a surface turned from the sun by sun_face, at the shadows' strength */ \
    "          bool sunlit = false;\n" \
    "          if (sbw < 0.98 && u.smap.x > 0.0 && u.sunsh.w > 0.0) {\n" \
    "            float nls = dot(nv, u.sunsh.xyz);\n" \
    "            float sm = lerp(1.0 - 0.6 * u.smap2.y, 1.0, smoothstep(-0.3, 0.25, nls));\n" \
    "            if (nls > 0.0) {\n" \
    "              float3 sdw = u.sunsh.x * m0.xyz + u.sunsh.y * m1.xyz + u.sunsh.z * m2.xyz;\n" \
    "              uint st; float2 stb;\n" \
    /* (from a little off the surface, along its own facing: from the point itself, a ray low over a curved wall met the wall's \
     * next triangle at once, here and not a pixel on) */ \
    "              float3 so = ow + dw * th + (nv.x * m0.xyz + nv.y * m1.xyz + nv.z * m2.xyz) * (0.04 + 0.002 * th);\n" \
    "              if (bvh_trace(so, sdw, 0.05, 400.0, rti.y, rti.z, true, st, stb) > 0.0) sm = min(sm, u.smap2.z); else sunlit = nls > 0.2;\n" \
    "            }\n" \
    "            rtc *= max(lerp(1.0, sm, u.smap.x), 0.15);\n" \
    "          }\n" \
    /* ... and what stands over it: the screen's picture has the occlusion laid over it too (fx_comp, u.ao.y its strength), and the \
     * tree's own colour of an arch's underside and the walls of the recess beneath it was the open wall's - a gate's reflection \
     * with its inside as bright as its face. A third ray, short (6 units), up and a little out from the surface, any hit ending \
     * it: half the light under cover, an open wall meeting nothing. Only for what the sun doesn't reach (what it lights isn't \
     * under cover, and the ray for every hit was 3.5 ms more at 1600 lines with a whole plaza reflecting) */ \
    "          if (sbw < 0.98 && u.ao.y > 0.0 && !sunlit) {\n" \
    "            float3 nwd = nv.x * m0.xyz + nv.y * m1.xyz + nv.z * m2.xyz;\n" \
    "            uint at_; float2 ab_;\n" \
    "            float ta = bvh_trace(ow + dw * th + nwd * (0.04 + 0.002 * th), normalize(g_rts.xyz + nwd * 0.6), 0.05, 6.0, rti.y, rti.z, true, at_, ab_);\n" \
    "            if (ta > 0.0) rtc *= lerp(1.0, 0.5, saturate(u.ao.y));\n" \
    "          }\n" \
    /* the zone's fog over what is far, as the world has it */ \
    "          rtc = lerp(rtc, u.fogc.rgb, u.fogc.a > 0.0 ? min(1.0 - exp(-u.fogc.a * th), u.fogp.y) : 0.0);\n" \
    "          rtc = lerp(rtc, scol, sbw);\n" \
    /* Render visual 33's colour of a pixel the tree's ray shows: red its own colour, yellow the screen's picture of the same \
     * point, orange between the two (it turns with the camera); white two surfaces at one place, magenta the walk ran out of \
     * steps, cyan its stack was full - what a part of the reflection that flickers is likely to be */ \
    "          rtd = lerp(float3(1.0, 0.0, 0.0), float3(1.0, 1.0, 0.0), sbw);\n" \
    "          if ((rtf & 2u) != 0u) rtd = float3(1.0, 1.0, 1.0);\n" \
    "          if ((rtf & 4u) != 0u) rtd = float3(0.0, 1.0, 1.0);\n" \
    "          if ((rtf & 1u) != 0u) rtd = float3(1.0, 0.0, 1.0);\n" \
    "        }\n" \
    "      } else if (rti.x == 2u && th <= 0.0 && rti.w == 1u && !nearer) {\n" \
    /* a ray that leaves the zone with the screen having nothing for it: the sky, which the tree hasn't: its colour as the sky pass \
     * has it (one texel, input 11: the lowest sky in the view's top, held while none is in view) */ \
    "        rthit = true;\n" \
    /* (the colour is the sky's near the horizon, where the view's top shows it: a touch bluer and darker toward the top of the sky \
     * a ray going up finds - seen from above, the whole floor took the dusk's pink. Not less of it: it was cut to a quarter for a \
     * ray going straight up, so from stairs or a hill, the higher the player, the less sky in the floor, Whitegate 2026-11-27) */ \
    "        float el = saturate(dot(Rt, upn));\n" \
    "        rtw = smoothstep(0.0, 0.1, el);\n" \
    "        bool smap = u.rtx.z > 0.5;\n" \
    "        float3 sk = texel(11, int2(smap ? " SKYMAP_WS " : 0, 0)).rgb;\n" \
    "        rtd = (rtf & 1u) != 0u ? float3(1.0, 0.0, 1.0) : float3(0.0, 0.3, 1.0);\n" \
    "        rtc = lerp(sk, dot(sk, LUMA) * float3(0.75, 0.85, 1.1), 0.5 * el) * lerp(1.0, 0.85, el);\n" \
    /* the sky map in the ray's direction (the world's, y down), where it has seen the sky (SKY_MAP_HLSL) */ \
    "        if (smap) {\n" \
    "          float3 dn = normalize(dw);\n" \
    "          float sa = asin(saturate(-dn.y)), saz = atan2(dn.z, dn.x);\n" \
    "          float2 st = float2(clamp((saz + 3.1415927) * (" SKYMAP_WS ".0 / 6.2831853), 0.5, " SKYMAP_WS ".0 - 0.5), clamp((1.0 - sa / 1.5707963) * " SKYMAP_HS ".0, 0.5, " SKYMAP_HS ".0 - 0.5));\n" \
    "          float4 sm = tap(11, st / size_of(11));\n" \
    "          rtc = lerp(rtc, sm.rgb, saturate(sm.a));\n" \
    "        }\n" \
    /* ... and the sky itself where the screen shows it that way (the sky is as far as it gets: the ray's direction from the \
     * camera), its clouds and its colour there, easing to the one colour toward the picture's edges: that one colour alone \
     * read as no sky at all under an overcast evening, Whitegate 2026-11-27 */ \
    "        float3 sq = to_px(Rt * 1e4);\n" \
    "        if (sq.z > 0.1 && in_view(sq.xy) && dist_at(sq.xy) > 9e4) {\n" \
    "          float2 sqe = (sq.xy - u.vp.xy) / u.vp.zw;\n" \
    "          float sqw = smoothstep(0.0, 0.08, min(sqe.x, 1.0 - sqe.x)) * smoothstep(0.0, 0.08, min(sqe.y, 1.0 - sqe.y));\n" \
    "          rtc = lerp(rtc, tap(1, sq.xy / u.size.xy).rgb, sqw);\n" \
    "        }\n" \
    "      }\n" \
    "    }\n" \
    "  }\n" \
    "#endif\n" \
    ""

#define RT_BLUR_HLSL \
    /* the ray-traced reflections' blur (refl_rt_blur, u.rtx.x texels): a Gaussian (sigma half the radius) over the layer's texels, \
     * each still weighted by how near its distance is to the pixel's, so it doesn't run across an edge; where the 3x3 above has \
     * nothing it ends, so the blur doesn't spread past where the reflection does */ \
    "  if (u.rtx.x > 0.5) {\n" \
    "    float rr_ = u.rtx.x;\n" \
    "    float stp = max(1.0, rr_ * 0.25);\n" \
    "    int nr = rr_ < 4.0 ? int(ceil(rr_)) : 4;\n" \
    "    float inv = -2.0 / (rr_ * rr_);\n" \
    "    float2 jit = rr_ < 4.0 ? float2(0.0, 0.0) : (float2(frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453), frac(sin(dot(uv, float2(39.3468, 11.135))) * 24634.6345)) - 0.5) * stp;\n" \
    "    float4 bs = float4(0.0, 0.0, 0.0, 0.0);\n" \
    "    float bw_ = 0.0, bbest = 0.0;\n" \
    "    [loop] for (int oy = -nr; oy <= nr; ++oy) {\n" \
    "      [loop] for (int ox = -nr; ox <= nr; ++ox) {\n" \
    "        float2 off = float2(ox, oy) * stp + jit;\n" \
    "        int2 p2 = clamp(n0 + int2(round(off)), int2(0, 0), hi);\n" \
    "        float gw = exp(dot(off, off) * inv);\n" \
    "        float td2 = texel(kd, min(int2((float2(p2) + 0.5) * dmap), int2(dsz) - 1)).r * u.hand.x;\n" \
    "        float dw2 = td2 > 0.0 ? 1.0 / (1e-3 + abs(td2 - dist) / dist) : 1e-3;\n" \
    "        bs += texel(k, p2) * (gw * dw2); bw_ += gw * dw2; bbest = max(bbest, dw2);\n" \
    "      }\n" \
    "    }\n" \
    "    if (!(bw_ > 0.0)) return float4(0.0, 0.0, 0.0, 0.0);\n" \
    "    return (bs / bw_) * saturate((bbest - 4.0) / 6.0);\n" \
    "  }\n" \
    ""

/* fx_zlate (after the game's last effect, before its interface): the game's effects, mirrored about the player's level
 * (gfx_d3d12_rtfx.h: input 3, half the scene's size), laid by the reflections' weight (input 4, their layer's alpha) on the floors
 * at that level. For this pass alone u.rtx is the level's plane in view space (mfx_late_in): xyz this frame's up, its length the
 * effects' strength (0: none) and the blur's radius, w the level along it from the camera. Not u.up, which is eased over frames: while the camera pitched it
 * trailed by degrees, a floor 15 yalms off measured two yalms off the level, and the effects left the reflections until it caught
 * up. An effect that adds light is added; one blended over the world (a home point's crystal) left its cover in the mirror's
 * alpha, and takes that share of the pixel. Laid this late, the same frame's: in the next frame's reflections, they trailed the
 * camera. And blurred as the reflections are (refl_rt_blur, RT_BLUR_HLSL's Gaussian, its radius in the layer's texels): the
 * plane's length is that radius plus the strength (0 to 1, so the radius is the whole part less one) - laid sharp, a crystal's
 * reflection stood crisp in a floor whose every other reflection was a soft blur */
#define RT_FX_HLSL \
    /* (two mirrors, each a floor level in view: input 3 by u.rtx, input 5 by u.rbf, its length 0 with one alone, gfx_d3d12_rtfx.h \
     * mfx_late_in) */ \
    "#ifdef ZL_LATE\n" \
    "  [unroll] for (int mi = 0; mi < 2; ++mi) {\n" \
    "  float4 mpl = mi == 0 ? u.rtx : u.rbf;\n" \
    "  int msl = mi == 0 ? 3 : 5;\n" \
    "  float mln = length(mpl.xyz);\n" \
    "  if (mln > 0.001 && z != 0.0) {\n" \
    "    float mbl = ceil(mln) - 1.0, mst = mln - mbl;\n" \
    "    float lvl = 1.0 - smoothstep(0.4, 1.2, abs(dot(view_pos(i.pos.xy, z), mpl.xyz / mln) - mpl.w));\n" \
    "    float mk = saturate(tap(4, i.uv).a) * mst * lvl;\n" \
    "    if (mk > 0.001) {\n" \
    "      float2 mu = (u.vp.xy + i.uv * u.vp.zw) / size_of(2);\n" \
    "      float4 mf = tap(msl, mu);\n" \
    "      if (mbl > 0.5) {\n" \
    "        float2 mt = u.vp.zw / (size_of(2) * size_of(4));\n" \
    "        float stp = max(1.0, mbl * 0.25), inv = -2.0 / (mbl * mbl), bw_ = 0.0;\n" \
    "        int nr = mbl < 4.0 ? int(ceil(mbl)) : 4;\n" \
    "        float2 jit = mbl < 4.0 ? float2(0.0, 0.0) : (float2(frac(sin(dot(i.pos.xy, float2(12.9898, 78.233))) * 43758.5453), frac(sin(dot(i.pos.xy, float2(39.3468, 11.135))) * 24634.6345)) - 0.5) * stp;\n" \
    "        mf = float4(0.0, 0.0, 0.0, 0.0);\n" \
    "        [loop] for (int oy = -nr; oy <= nr; ++oy) {\n" \
    "          [loop] for (int ox = -nr; ox <= nr; ++ox) {\n" \
    "            float2 off = float2(ox, oy) * stp + jit;\n" \
    "            float gw = exp(dot(off, off) * inv);\n" \
    "            mf += tap(msl, mu + off * mt) * gw; bw_ += gw;\n" \
    "          }\n" \
    "        }\n" \
    "        mf /= bw_;\n" \
    "      }\n" \
    "      r += mf.rgb * mk;\n" \
    "      w = 1.0 - (1.0 - w) * (1.0 - saturate(mf.a) * mk);\n" \
    "    }\n" \
    "  }\n" \
    "  }\n" \
    "#endif\n"
