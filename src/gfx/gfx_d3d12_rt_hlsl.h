/* gfx_d3d12_rt_hlsl.h: the ray-traced reflections' part of the effects' HLSL (gfx_d3d12.c FX_HLSL; refl_rt), Direct3D 12's alone
 * (ZL_LATE): RT_TRACE_HLSL the walk through a tree (bvh_trace) and a triangle's colour (tri_tex), before refl_trace; RT_SSR_HLSL
 * fx_ssr's use of them, after the screen's own ray. Kept apart for the length of gfx_d3d12.c. */
#pragma once

#define RT_TRACE_HLSL \
    /* the zone's collision as a tree (rtbvh.h; Direct3D 12 only): its nodes, 32 bytes each, then its triangles, 36; the frame's \
     * words beside it. bvh_trace finds the nearest hit; with the meshes' tree, tri_tex its colour */ \
    "#ifdef ZL_LATE\n" \
    "ByteAddressBuffer g_bvh : register(t0, space3);\n" \
    "ByteAddressBuffer g_rtv : register(t1, space3);\n" \
    /* the meshes' tree's own words for a triangle (ab: where they start, zonemesh.c: 10 words each): the three corners' uv (6), the \
     * three vertex colours (3: 0xAARRGGBB), and the texture (bits 0 to 15: its index + 1 in the frame words' list from byte 96, 0 \
     * none; bit 16: cut out by its alpha). The colour of the surface a ray found: vc the vertex colour there */ \
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
    "  uint sp = 0u, node = 0u;\n" \
    "  float best = -1.0, tm = tmax;\n" \
    "  [loop] for (int it = 0; it < 640; ++it) {\n" \
    "    uint nb = node * 32u;\n" \
    "    float4 n0 = asfloat(g_bvh.Load4(nb)), n1 = asfloat(g_bvh.Load4(nb + 16u));\n" \
    "    bool pop = true;\n" \
    "    if (box_t(n0, n1, o, inv, tmin, tm) < 1e29) {\n" \
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
    "              /* (a cut-out's clear texel is no surface: the ray goes on) */\n" \
    /* (only a cut-out mesh's, bit 16, at the game's cut: the game draws every other mesh solid whatever its texture's alpha holds - \
     * Bastok Markets' stone and wood are clear over nine texels in ten, a mask for the decals blended over them - and a cut by it \
     * left holes in every wall) */ \
    "              if (ab != 0u && (g_bvh.Load(ab + (a + k) * 40u + 36u) & 0x10000u) != 0u) {\n" \
    "                float3 vcx;\n" \
    "                if (tri_tex(ab, a + k, float2(uu, vv), 0.0, vcx).a < 0.185) continue;\n" \
    "              }\n" \
    "              best = t; tm = t;\n" \
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
    "          node = na ? a : cb;\n" \
    "          pop = false;\n" \
    "        }\n" \
    "      }\n" \
    "    }\n" \
    "    if (pop) {\n" \
    "      if (sp == 0u) break;\n" \
    "      node = stack[--sp];\n" \
    "    }\n" \
    "  }\n" \
    "  return best;\n" \
    "}\n" \
    "#endif\n" \
    ""

#define RT_SSR_HLSL \
    /* ray-traced reflections (refl_rt; Direct3D 12 only, ZL_LATE): where the screen has no hit, the ray goes on through the tree of \
     * the zone's collision (rtbvh.h), in the world: g_rtv has the view's inverse to take the ray there, and says whether a tree is \
     * bound (word 16; word 17 where its triangles start) */ \
    "  bool rthit = false;\n" \
    "  float3 rtc = float3(0.0, 0.0, 0.0);\n" \
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
    /* (the tree's ray leaves along the floor's own normal, not the bumped one: the bump tilts it by the floor picture's grooves and \
     * tile edges, which the screen's blur hides, and a ray that isn't blurred showed as a checker of lines across the floor) */ \
    /* (N is worked out from the depth, a little different at each pixel and frame: the screen's blur hid it, the tree's sharp hits \
     * showed it as a jitter and patches of other colours. A floor is flat for the most part: drawn most of the way to up) */ \
    "      float3 Nf = normalize(lerp(N, upn, 0.85));\n" \
    /* (and not lifted: refl_lift tilts a low ray up because the screen has nothing to show along the floor; the tree has the whole zone, \
     * and the tilt, which changes with the view's angle, swam the reflection like jelly as the camera moved up and down) */ \
    "      float3 Rt = reflect(-V, Nf);\n" \
    "      float3 dw = Rt.x * m0.xyz + Rt.y * m1.xyz + Rt.z * m2.xyz;\n" \
    "      float rtmax = max(u.refl.w, 100.0);\n" \
    "      uint ht; float2 hb;\n" \
    /* (the meshes' tree starts the ray off the floor it leaves: ground laid over ground, a path or a drain's rim a hand above the \
     * pavement, is a surface the ray meets at once from beneath, a dark band that isn't there) */ \
    "      float th = bvh_trace(ow, dw, rti.x == 2u ? max(0.03 * dist + 0.05, 0.4) : 0.03 * dist + 0.05, rtmax, rti.y, rti.x == 2u ? rti.z : 0u, false, ht, hb);\n" \
    /* (the meshes' tree has the zone and no one in it: where the screen found a surface nearer than the tree's, a player, a \
     * monster, a thing that moves, the screen's is shown) */ \
    /* (only a character: the screen's ray often finds the floor itself a hand away at a low angle, which is no one standing there, \
     * and took the tree's answer from half the floor) */ \
    /* (the characters' mask is bound only while the rain wets them, so it can't say which is one: a surface the screen found \
     * clearly nearer than the tree's, and well above the floor the ray left, is something standing in front of it) */ \
    "      bool nearer = rti.x == 2u && h.w > 0.5 && h.w < 1.5 && th > 0.0 && tt < th - 0.3 && tt * dot(R, upn) > 0.4;\n" \
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
    "          if (sbw < 0.98 && u.smap.x > 0.0 && u.sunsh.w > 0.0) {\n" \
    "            float nls = dot(nv, u.sunsh.xyz);\n" \
    "            float sm = lerp(1.0 - 0.6 * u.smap2.y, 1.0, smoothstep(-0.3, 0.25, nls));\n" \
    "            if (nls > 0.0) {\n" \
    "              float3 sdw = u.sunsh.x * m0.xyz + u.sunsh.y * m1.xyz + u.sunsh.z * m2.xyz;\n" \
    "              uint st; float2 stb;\n" \
    "              if (bvh_trace(ow + dw * th, sdw, 0.08 + 0.003 * th, 400.0, rti.y, rti.z, true, st, stb) > 0.0) sm = min(sm, u.smap2.z);\n" \
    "            }\n" \
    "            rtc *= max(lerp(1.0, sm, u.smap.x), 0.15);\n" \
    "          }\n" \
    /* the zone's fog over what is far, as the world has it */ \
    "          rtc = lerp(rtc, u.fogc.rgb, u.fogc.a > 0.0 ? min(1.0 - exp(-u.fogc.a * th), u.fogp.y) : 0.0);\n" \
    "          rtc = lerp(rtc, scol, sbw);\n" \
    "        }\n" \
    "      } else if (rti.x == 2u && th <= 0.0 && rti.w == 1u && !(h.w > 0.5 && h.w < 1.5)) {\n" \
    /* a ray that leaves the zone with the screen having nothing for it: the sky, which the tree hasn't: its colour as the sky pass \
     * has it (one texel, input 11: the lowest sky in the view's top, held while none is in view) */ \
    "        rthit = true;\n" \
    /* (the colour is the sky's near the horizon, where the view's top shows it: darker and a touch bluer toward the top of the sky a \
     * ray going up finds, and less of it - seen from above, the whole floor took the dusk's pink at full strength) */ \
    "        float el = saturate(dot(Rt, upn));\n" \
    "        rtw = smoothstep(0.0, 0.3, el) * lerp(1.0, 0.45, el);\n" \
    "        float3 sk = texel(11, int2(0, 0)).rgb;\n" \
    "        rtc = lerp(sk, dot(sk, LUMA) * float3(0.75, 0.85, 1.1), 0.5 * el) * lerp(1.0, 0.6, el);\n" \
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
 * at that level. For this pass alone u.rtx is the level's plane in view space (mfx_late_in): xyz this frame's up times the effects'
 * strength (0: none), w the level along it from the camera. Not u.up, which is eased over frames: while the camera pitched it
 * trailed by degrees, a floor 15 yalms off measured two yalms off the level, and the effects left the reflections until it caught
 * up. An effect that adds light is added; one blended over the world (a home point's crystal) left its cover in the mirror's
 * alpha, and takes that share of the pixel. Laid this late, the same frame's: in the next frame's reflections, they trailed the
 * camera */
#define RT_FX_HLSL \
    "#ifdef ZL_LATE\n" \
    "  float mst = length(u.rtx.xyz);\n" \
    "  if (mst > 0.001 && z != 0.0) {\n" \
    "    float lvl = 1.0 - smoothstep(0.4, 1.2, abs(dot(view_pos(i.pos.xy, z), u.rtx.xyz / mst) - u.rtx.w));\n" \
    "    float4 mf = tap(3, (u.vp.xy + i.uv * u.vp.zw) / size_of(2));\n" \
    "    float mk = saturate(tap(4, i.uv).a) * min(mst, 1.0) * lvl;\n" \
    "    r += mf.rgb * mk;\n" \
    "    w = 1.0 - (1.0 - w) * (1.0 - saturate(mf.a) * mk);\n" \
    "  }\n" \
    "#endif\n"
