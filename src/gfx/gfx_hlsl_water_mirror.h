/* gfx_hlsl_water_mirror.h: an include unit of gfx_hlsl_water.h (in its scope: Sb, sb_printf, water_tex). The world mirrored in
 * VanaCore's water, for the pass's pixel function (emit_water_depth) when the draw has the mirror in stage 3 (d3d8_water_mirror.h
 * water_mirror_bind; gfx.h gfx_scene_mirror): the frame's opaque world drawn from the camera mirrored about the water's level,
 * the size of the scene's viewport or a share of it, its alpha 0 where the world was drawn and 1 where nothing was. */
#pragma once

/* Into `sky` (what the surface mirrors) and `F` (how much of it shows), before the pixel's colour is put together.
 *   The sky first: the surface's sky was two colours by the mirrored ray's height (emit_water_light), so under a pink dusk
 * the lake mirrored navy. The sky is as good as endlessly far, so what a level surface mirrors of it is the sky in the
 * direction of the view turned over about the surface, wherever on the screen that direction lies: the scene's own colour
 * there (the pass's copy, stage 6) where the scene has nothing nearer (its depth, stage 7, the far plane's), clouds, dusk and
 * all; where something nearer covers that place, the first sky above it on the screen (six steps up to the top). The ray
 * passes nowhere near what covers it (a tree on the quay), and the two colours in its place drew the tree's crown, turned
 * over about the horizon, in navy on a grey sea. A direction off the screen's top or sides is read at the edge, and fades back to the two colours over the next quarter
 * of the screen. Any level's water takes it.
 *   Then the world: the mirror is read at the pixel's own place in the viewport - the mirrored camera sees a level surface's
 * point where the camera does - softened over five taps a texel and a quarter apart. Where the world was drawn in it, it
 * stands in for the sky, and the surface mirrors at least 55% there (times the strength, and no more than the shore's fade
 * allows): by Fresnel alone a lake seen from a character's height mirrors a tenth, and the shore in it was barely a tint.
 *   Both are pushed along the waves' lean on the screen (dn, as the refraction is; more down the screen than across, as a
 * mirrored shape is drawn out toward the viewer on moving water), less far off. Texture matrix 2's bytes 0-4: the strength
 * (water.mirror), the mirror's level (water more than 0.3 yalms from it loses the mirrored world over the next 1.2: it's
 * another level's picture), how far the waves push it (water.mirror.waves), and the projection's x and y scales (the
 * direction's place on the screen). Not with the camera under the surface (hs) */
static void emit_water_mirror(Sb* b)
{
    char mt[32], ms[32], ct[32], cs[32], dt[32], ds[32];
    water_tex(mt, ms, sizeof mt, 3);
    water_tex(ct, cs, sizeof ct, 6);
    water_tex(dt, ds, sizeof dt, 7);
    sb_printf(b,
        "  { float4x4 mm = u.texm[2];\n"
        "    float4 mq = float4(mm[0][0], mm[1][0], mm[2][0], mm[3][0]);\n"
        "    float mall = saturate(mq.x);\n"
        "    float mk = mall * (1.0 - smoothstep(0.3, 1.5, abs(p.y - mq.y)));\n"
        "    if (mall > 0.001 && hs < 0.0) {\n"
        "      float2 mo = float2(dn.x, -dn.y * 2.5) * (0.12 * mq.z * min(1.0, 20.0 / dist));\n"
        "      float3 mr = reflect(-V, up);\n"
        "      float mrw = mr.z * zp.z;\n"
        "      if (mrw > 0.02) {\n"
        "        float2 suv = float2(0.5 + 0.5 * mr.x * mq.w / mrw, 0.5 - 0.5 * mr.y * mm[0][1] / mrw) + mo;\n"
        "        float2 sout = max(abs(suv - 0.5) - 0.5, 0.0);\n"
        "        float2 tuv = clamp((u.vp.xy + saturate(suv) * u.vp.zw) / wsz, uvlo, uvhi);\n"
        "        float skyhit = 0.0;\n"
        "        float2 squv = tuv;\n"
        "        [unroll] for (int sstep = 0; sstep < 6; ++sstep) {\n" /* (not si: the global array the samplers are named by) */
        "          float2 q = float2(tuv.x, lerp(tuv.y, uvlo.y, sstep / 5.0));\n"
        "          float sd = (%s.SampleLevel(%s, q, 0).r - zp.w) / max(ws.x, 1e-6);\n"
        "          if (skyhit < 0.5 && sd >= 0.99999) { skyhit = 1.0; squv = q; }\n"
        "        }\n"
        "        float sk = skyhit * (1.0 - smoothstep(0.0, 0.25, max(sout.x, sout.y)));\n"
        "        hsky = %s.SampleLevel(%s, squv, 0).rgb, hsk = sk * mall;\n" /* (the far sea's fog takes it: emit_water_depth wfog) */
        "        sky = lerp(sky, hsky, hsk);\n"
        "      }\n"
        "      float mw, mh;\n"
        "      %s.GetDimensions(mw, mh);\n"
        "      float2 muv = clamp((vin.pos.xy - u.vp.xy) / u.vp.zw + mo, 0.002, 0.998);\n"
        "      float2 mtx = 1.25 / float2(mw, mh);\n"
        "      float4 mc = %s.SampleLevel(%s, muv, 0) * 0.36;\n"
        "      mc += %s.SampleLevel(%s, muv + mtx * float2(1.0, 0.4), 0) * 0.16;\n"
        "      mc += %s.SampleLevel(%s, muv + mtx * float2(-0.4, 1.0), 0) * 0.16;\n"
        "      mc += %s.SampleLevel(%s, muv + mtx * float2(-1.0, -0.4), 0) * 0.16;\n"
        "      mc += %s.SampleLevel(%s, muv + mtx * float2(0.4, -1.0), 0) * 0.16;\n"
        "      float mcov = saturate(1.0 - mc.a);\n"
        "      sky = lerp(sky, sky * (1.0 - mcov) + mc.rgb, mk);\n"
        "      F = lerp(F, max(F, saturate(0.55 * mq.x) * edge * (1.0 - foot)), mcov * mk);\n"
        "    } }\n",
        dt, ds, ct, cs, mt, mt, ms, mt, ms, mt, ms, mt, ms, mt, ms);
}
