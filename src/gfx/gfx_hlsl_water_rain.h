/* gfx_hlsl_water_rain.h: an include unit of gfx_hlsl_water.h (in its scope: Sb, sb_printf, water_tex, water_hash). The rain's
 * drops landing on VanaCore's water, for the water's pixel function (emit_water_surface) when the draw has the rain's values
 * in texture matrix 2 and its shelter map in stage 2 (d3d8_rain.h rain_water_bind).
 *   The rain's own pass lays its rings by the scene's depth as it was before the water was drawn (gfx_hlsl_rain.h
 * emit_rain_fs: the pass's copies), and its drops land on the zone's collision (d3d8_rain.h drop_spawn): over water both are
 * the bed, so a lake in the rain lay untouched. The surface is the water's pixel function's alone, so the rings are made
 * here, in the world: they stay where they landed as the camera moves, and bend the sky, the mirrored shore and the bed
 * showing through as the waves do. */
#pragma once

/* water_rain, at file scope before the pixel function: the drops landing round x (the world's x and z in cells of 0.4 yalms)
 * at time t (seconds, wrapped at 600). A cell in `many` halves has a drop land in it about once a second at a new spot each
 * time: a ring that spreads to 0.14 yalms over the first 0.6 of the cycle and fades, a fainter one following it at half the
 * radius, and a bright hit at its middle for the first tenth of a second. Returns the surface's slope from them (x, y: per
 * cell, a crest of height 1) and their crests' and hits' brightness (z). A ring reaches less than half a cell from its
 * middle, so the four cells nearest x are all that can touch it. Each cell turns a whole number of cycles in the 600 s and
 * its drops' places repeat with them: seamless at the wrap. The cells repeat every 256 (102 yalms) */
static void emit_water_rain_functions(Sb* b)
{
    sb_printf(b,
        "float3 water_rain(float2 x, float t, float many) {\n"
        "  float2 c0 = floor(x - 0.5);\n"
        "  float3 r = float3(0.0, 0.0, 0.0);\n"
        "  for (int j = 0; j <= 1; ++j) for (int i = 0; i <= 1; ++i) {\n"
        "    float2 k = c0 + float2(i, j);\n"
        "    float2 kk = k - 256.0 * floor(k / 256.0);\n"
        "    float h = water_hash(kk + 0.5), h2 = water_hash(kk + 7.3);\n"
        "    if (water_hash(kk + 3.1) > many * 0.5) continue;\n"
        "    float turns = floor((0.7 + 0.5 * h2) * 600.0 + 0.5);\n"
        "    float cyc = t * turns / 600.0 + h * 7.0;\n"
        "    float n = floor(cyc), life = (cyc - n) / 0.6;\n"
        "    if (life >= 1.0) continue;\n"
        "    n -= turns * floor(n / turns);\n"
        "    float2 dv = x - (k + float2(water_hash(kk + n * 0.37 + 1.7), water_hash(kk + n * 0.53 + 2.1)));\n"
        "    float d = length(dv);\n"
        "    float rad = 0.04 + 0.32 * life, fade = (1.0 - life) * (1.0 - life);\n"
        "    float x1 = (d - rad) / 0.055, x2 = (d - 0.5 * rad) / 0.055;\n"
        "    float e1 = exp(-x1 * x1), e2 = 0.5 * exp(-x2 * x2) * saturate(life * 3.0);\n"
        "    float hit = exp(-d * d * 90.0) * saturate(1.0 - life * 6.0);\n"
        "    r += float3(dv / max(d, 1e-4) * (-2.0 * (x1 * e1 + x2 * e2) * fade), (e1 + e2) * fade + 1.5 * hit);\n"
        "  }\n"
        "  return r;\n"
        "}\n");
}

/* Into `g` (the surface's slope, before the normal is made of it) and `wrain` (the crests' brightness, laid over the finished
 * colour by emit_water_depth). Texture matrix 2's bytes 5-9: how strong (the rain's heaviness times rain.splash and
 * rain.water; 0 none), the rain's clock, how many (the heaviness times rain.density), how far from the camera (rain.range:
 * fading over its last fifth) and whether stage 2 is the shelter map; byte 11 the map's reference height, 12-14 its corner
 * (x, z) and 1 over its width. None under a roof: the map (the roof's height over each 0.75-yalm cell, d3d8_rain.h COVER_N
 * 128 cells across, read as gfx_hlsl_rain.h rain_roof reads it) at the four cells round the pixel, weighed across them, a
 * roof counting from 1.2 yalms over the water as it does over the ground. The rings fade out where they near the pixels'
 * size (fp across and along the view, their mean: at a grazing view a ring is a dash, and still reads as rain), the slopes
 * sooner than the brightness, which is the stronger further off: up close a ring is its relief, from a bank its glint */
static void emit_water_rain(Sb* b)
{
    char mt[32], ms[32];
    water_tex(mt, ms, sizeof mt, 2);
    sb_printf(b,
        "  { float4x4 rw = u.texm[2];\n"
        "    float rk = rw[1][1] * (1.0 - smoothstep(0.8 * rw[0][2], rw[0][2], dist));\n"
        "    float rfp = sqrt(length(ddx(p.xz)) * length(ddy(p.xz))) + 1e-4;\n"
        "    float rsee = saturate(0.2 / rfp - 0.4);\n"
        "    rk *= rsee;\n"
        "    if (rk > 0.001 && rw[1][2] > 0.5) {\n"
        "      float2 tc = (p.xz - float2(rw[0][3], rw[1][3])) * rw[2][3] * 128.0 - 0.5;\n"
        "      float2 tb = floor(tc), tf = tc - tb;\n"
        "      float so = 0.0;\n"
        "      [unroll] for (int oy = 0; oy <= 1; ++oy) [unroll] for (int ox = 0; ox <= 1; ++ox) {\n"
        "        float2 cuv = (tb + float2(ox, oy) + 0.5) / 128.0;\n"
        "        float2 rg = %s.SampleLevel(%s, cuv, 0).rg;\n"
        "        float rv = rg.x * 255.0 * 256.0 + rg.y * 255.0;\n"
        "        float roof = (rv >= 65534.0 || any(cuv != saturate(cuv))) ? 1e9 : rw[3][2] + rv / 256.0 - 128.0;\n"
        "        so += (ox ? tf.x : 1.0 - tf.x) * (oy ? tf.y : 1.0 - tf.y) * (p.y > roof + 1.2 ? 0.0 : 1.0); }\n"
        "      rk *= so; }\n"
        "    if (rk > 0.001) {\n"
        "      float3 rr = water_rain(p.xz / 0.4, rw[2][1], rw[3][1]);\n"
        "      g += rr.xy * (0.35 * rk * saturate(0.05 / rfp - 0.5));\n"
        "      wrain = rr.z * rk * (1.0 + 1.5 * saturate(dist / 24.0)); } }\n",
        mt, ms);
}
