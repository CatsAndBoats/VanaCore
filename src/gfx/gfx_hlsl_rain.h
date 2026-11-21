/* VanaCore's rain on the characters (GfxFsKey.wet, d3d8_rain.h rain_character): the wet look laid over a lit fixed-function
 * draw's finished colour (emit_ff_fs, after the texture stages), from texture matrix 6 - its bytes 0-3 the wetness (0 to 1),
 * the time, the drops' and the sheen's strength; bytes 4-7 the world's up in view space and how many drops (a squall's more) -
 * and matrix 7, the view's inverse, for where the pixel stands in the world. Darker and glossier, as wet cloth and leather go:
 * a sharp lobe of each directional light (the sun, the moon) and the sky (the fog's colour) in the glancing view; the drops
 * landing on what faces up as rings that spread and fade from a bright hit, and runnels down the sides.
 * gfx_hlsl.c includes this before emit_ff_fs; it is no file of its own. */
#pragma once

/* the functions, before the pixel function: a hash of a cell, a value noise, and the drops landing on a level surface at world
 * xz p (yalms) at time t: a cell of 0.16 yalms gets a drop about every second at a new random spot in it (half the cells at `many`
 * 1): a dot at the hit and a thin ring spreading out over the cell and fading: sparse, never a haze, never a star */
static void emit_rain_functions(Sb* b)
{
    sb_printf(b,
        "float rain_hash(float2 p) { float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973)); q += dot(q, q.yzx + 33.33); return frac((q.x + q.y) * q.z); }\n"
        "float rain_noise(float2 p) { float2 i = floor(p), f = frac(p); f = f * f * (3.0 - 2.0 * f);\n"
        "  return lerp(lerp(rain_hash(i), rain_hash(i + float2(1, 0)), f.x), lerp(rain_hash(i + float2(0, 1)), rain_hash(i + float2(1, 1)), f.x), f.y); }\n"
        "float rain_rings(float2 p, float t, float many) {\n"
        "  float2 c = p / 0.16; float2 ci = floor(c); float r = 0.0;\n"
        "  for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {\n"
        "    float2 k = ci + float2(x, y); float h = rain_hash(k), h2 = rain_hash(k + 7.3);\n"
        "    if (rain_hash(k + 3.1) > many * 0.5) continue;\n"
        "    float cyc = t * (0.8 + 0.8 * h2) + h * 7.0; float n = floor(cyc), life = cyc - n;\n"
        "    float2 at = k + float2(rain_hash(k + n * 0.37), rain_hash(k + 2.1 + n * 0.53));\n"
        "    float d = length(c - at);\n"
        "    float ring = exp(-abs(d - life * 0.5) * 16.0) * (1.0 - life) * (1.0 - life);\n"
        "    float hit = exp(-d * d * 60.0) * saturate(1.0 - life * 4.0);\n"
        "    r += ring * 0.6 + hit;\n"
        "  }\n"
        "  return r;\n}\n");
}

/* the wet look over `cur` (the draw's finished colour), from wet_N and wet_pe (emit_fs_signature: the lit normal and the view-space
 * position at the pixel) */
static void emit_wet(Sb* b, const GfxVsKey* vk)
{
    sb_printf(b,
        "  {\n"
        "    float4x4 rn_t = transpose(u.texm[6]);\n"
        "    float rn_w = rn_t[0].x, rn_time = rn_t[0].y, rn_drops = rn_t[0].z, rn_sheen = rn_t[0].w, rn_many = rn_t[1].w;\n"
        "    float3 rn_up = rn_t[1].xyz;\n"
        "    float3 rn_N = normalize(wet_N);\n"
        "    float3 rn_V = normalize(-wet_pe);\n"
        "    if (dot(rn_N, rn_V) < 0.0) rn_N = -rn_N;\n"
        "    float rn_fu = saturate(dot(rn_N, rn_up));\n"
        "    float3 rn_wp = mul(u.texm[7], float4(wet_pe, 1.0)).xyz;\n"
        "    cur.rgb *= 1.0 - 0.3 * rn_w * saturate(rn_sheen);\n"
        "    float rn_ring = rain_rings(rn_wp.xz, rn_time, rn_many * 3.0) * smoothstep(0.15, 0.6, rn_fu);\n"
        /* the runnels: a noise stretched down the sides, moving down the world (its y is down: the pattern's v falls as y grows;
         * it ran up with the world right way up), its film's height h. Some body: the noise's slope across the film (two taps a
         * hundredth of a yalm off, in the world) bends the normal as a bead's rounded top would, which the light's sheen below
         * follows; a darker wet core and a thin bright rim at its edge */
        "    float2 rn_q = float2((rn_wp.x + rn_wp.z) * 24.0, -rn_wp.y * 3.0 + rn_time * 0.7);\n"
        "    float rn_n0 = rain_noise(rn_q);\n"
        "    float rn_side = 1.0 - rn_fu;\n"
        "    float rn_run = smoothstep(0.62, 0.8, rn_n0) * rn_side;\n"
        "    float rn_h = smoothstep(0.55, 0.85, rn_n0) * rn_side;\n"
        "    float3 rn_g = float3(rain_noise(rn_q + float2(0.24, 0.0)) - rn_n0, rain_noise(rn_q + float2(0.0, -0.03)) - rn_n0, 0.0);\n"
        "    rn_g = float3(rn_g.x, rn_g.y, rn_g.x) * 100.0 * smoothstep(0.5, 0.75, rn_n0) * rn_side;\n"
        "    float3 rn_gv = mul(float4(rn_g, 0.0), u.texm[7]).xyz;\n" /* the world's slope into view space: the inverse's transpose */
        "    rn_gv -= rn_N * dot(rn_gv, rn_N);\n"
        "    rn_N = normalize(rn_N - rn_gv * (0.03 * rn_w));\n"
        "    float rn_rim = saturate(rn_h * (1.0 - rn_h) * 4.0) * (1.0 - rn_run);\n"
        "    cur.rgb *= 1.0 - 0.18 * rn_run * rn_w;\n"
        "    float3 rn_lc = float3(0, 0, 0), rn_l0 = float3(0, 0, 0);\n");
    /* the light's colour from the draw's directional lights (none indoors: no sun sheen); its direction the scene's sun handed over
     * in matrix 7's fourth row (d3d8_rain.h rain_sun_keep) - the world's, not whatever the game gives this draw - else the draw's */
    for (int i = 0; i < vk->nlights; ++i)
    {
        if (vk->light_type[i] != 3)
            continue;
        sb_printf(b, "    rn_lc = max(rn_lc, u.light[%d].diffuse.rgb); if (dot(rn_l0, rn_l0) == 0.0) rn_l0 = u.light[%d].dir.xyz;\n", i, i);
    }
    /* the sheen: mostly a share facing the light alone, which stays put as the camera orbits, and a faint broad lobe (a tight
     * one, 90, slid across the body with every turn of the camera); the sky in what faces up, barely any at the rim. The
     * runnels and rings are laid in the world (matrix 7 the view's inverse), so they stay on the body too */
#ifdef VANACORE
    /* Direct3D 12 lays the sheen in its effects instead (gfx_d3d12.c fx_comp: a facing smoothed over the depth, the occlusion's
     * openness), where the models' flat panels don't light up whole: here the darkening, the drops and the runnels alone */
    if (!gfx_hlsl_d3d11)
    {
        sb_printf(b,
            "    float3 rn_sky = u.fogcolor.rgb * 0.75 + 0.05;\n"
            "    cur.rgb += rn_sky * (rn_rim * 0.12 + rn_run * 0.06) * (rn_w * rn_sheen);\n"
            "    cur.rgb = lerp(cur.rgb, rn_sky * 0.8, saturate(rn_ring) * (0.25 * rn_w * rn_drops));\n"
            "    cur.rgb = saturate(cur.rgb);\n"
            "    if (rn_t[2].y > 0.5) cur.rgb = float3(saturate(rn_ring * 4.0), 0.0, 0.0);\n"
            "  }\n");
        return;
    }
#endif
    sb_printf(b,
        "    float4 rn_sun = u.texm[7][3];\n"
        "    float3 rn_l = rn_sun.w > 0.5 ? normalize(rn_sun.xyz) : rn_l0;\n"
        "    float rn_ndl = saturate(dot(rn_N, rn_l));\n"
        "    float rn_spec = pow(saturate(dot(rn_N, normalize(rn_V + rn_l))), 24.0) * saturate(rn_ndl * 4.0);\n"
        "    float3 rn_sp = rn_lc * (0.12 * rn_spec + 0.4 * rn_ndl * rn_ndl * rn_ndl * rn_ndl);\n"
        "    float rn_fr = pow(1.0 - saturate(dot(rn_N, rn_V)), 3.0);\n"
        /* the sky's light: the fog's colour, at night dark (a floor of 0.22 over it lit the characters pale at night) */
        "    float3 rn_sky = u.fogcolor.rgb * 0.75 + 0.05;\n"
        "    float rn_k = rn_w * rn_sheen;\n"
        "    cur.rgb += (min(rn_sp, 0.35) * (0.35 + 0.8 * rn_run) + rn_sky * (rn_fr * 0.025 + rn_fu * 0.07 + rn_run * 0.06 + rn_rim * 0.12)) * rn_k;\n"
        "    cur.rgb = lerp(cur.rgb, rn_sky * 0.8, saturate(rn_ring) * (0.25 * rn_w * rn_drops));\n"
        "    cur.rgb = saturate(cur.rgb);\n"
        /* the diagnostics (rain.force): 2 the world position's fractions as colour; 3 facing up three ways - the normal through the view's
         * inverse (red), the up handed over in matrix 6's bytes 4-6 (green) and its column reading in bytes 12-14 (blue); 4 the rings alone */
        "    if (rn_t[2].x > 0.5 && rn_t[2].x < 1.5) cur.rgb = frac(rn_wp);\n"
        "    if (rn_t[2].x > 1.5) cur.rgb = float3(saturate(-mul(u.texm[7], float4(rn_N, 0.0)).y), rn_fu, saturate(dot(rn_N, rn_t[3].xyz)));\n"
        "    if (rn_t[2].y > 0.5) cur.rgb = float3(saturate(rn_ring * 4.0), 0.0, 0.0);\n"
        "  }\n");
}

/* --- the rain pass (GfxFsKey.rain, d3d8_rain.h rain_pass) ------------------------------------------------------------------ */
/* the pass's own functions: the scene's depth to a view z (the water's), the streaks at a point of a layer, the splashes'
 * bright hits alone (for the crowns) */
static void emit_rain_pass_functions(Sb* b)
{
    sb_printf(b,
        "float rain_viewz(float d, float4 zp, float zr, float zs) {\n"
        "  d = (d - zp.w) / max(zr, 1e-6);\n"
        "  float z = zp.y / (d * zp.z - zp.x);\n"
        "  if (d >= 0.999999 || !(z * zp.z > 0.0)) return zs * 1000.0;\n"
        "  return z;\n"
        "}\n"
        /* the streaks of a layer: in the world's angle space s round the camera (the ray's azimuth and its elevation, radians:
         * a column of constant azimuth is a vertical line in the world, so the streaks fall straight down and stay where they are
         * as the camera turns), scaled by k (streaks per radian: the near layers' few and large, the far ones' many and short),
         * the azimuth wrapping after nc columns (k times 2 pi, whole): columns a unit apart (the share `dens` of them), each a
         * hair-thin line (hw, in columns: about a pixel) falling at `speed` units a second, lit for `len` of each unit with soft
         * ends, its own length, speed and brightness; the nearest two columns */
        "float rain_streaks(float2 s, float k, float nc, float t, float dens, float speed, float len, float hw) {\n"
        "  float2 c = s * k; float ci = floor(c.x - 0.5); float r = 0.0;\n"
        "  for (int x = 0; x <= 1; ++x) {\n"
        "    float id = ci + x; id -= nc * floor(id / nc); float h = rain_hash(float2(id, k));\n"
        "    if (h > dens * 0.5) continue;\n"
        "    float sp = speed * (0.75 + 0.5 * rain_hash(float2(id + 3.7, k)));\n"
        "    float cyc = c.y * 0.5 + t * sp + h * 5.0;\n"
        "    float n = floor(cyc), ph = cyc - n;\n"
        "    float2 seed = float2(id + 17.1, k + n * 0.618);\n"
        "    float h2 = rain_hash(seed), h3 = rain_hash(seed + 5.3), h4 = rain_hash(seed + 11.7);\n"
        "    float at = ci + x + 0.1 + 0.8 * h2;\n"
        "    float m = 1.0 - smoothstep(0.0, hw, abs(c.x - at));\n"
        "    if (m <= 0.0) continue;\n"
        "    float ln = len * (0.4 + 0.9 * h3);\n"
        "    r += m * (0.35 + 0.65 * h4) * smoothstep(0.0, 0.4 * ln, ph) * (1.0 - smoothstep(0.6 * ln, ln, ph));\n"
        "  }\n"
        "  return r;\n"
        "}\n"
        "float rain_roof(float2 rg, float covy) { float v = rg.x * 255.0 * 256.0 + rg.y * 255.0; return v >= 65534.0 ? 1e9 : covy + v / 256.0 - 128.0; }\n"
        "float rain_hits(float2 p, float t, float many) {\n"
        "  float2 c = p / 0.16; float2 ci = floor(c); float r = 0.0;\n"
        "  for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {\n"
        "    float2 k = ci + float2(x, y); float h = rain_hash(k), h2 = rain_hash(k + 7.3);\n"
        "    if (rain_hash(k + 3.1) > many * 0.5) continue;\n"
        "    float2 at = k + float2(h, h2);\n"
        "    float cyc = t * (0.8 + 0.8 * h2) + h * 7.0; float n = floor(cyc), life = cyc - n;\n"
        "    at = k + float2(rain_hash(k + n * 0.37), rain_hash(k + 2.1 + n * 0.53));\n"
        "    float d = length(c - at);\n"
        "    r += exp(-d * d * 40.0) * saturate(1.0 - life * 4.0);\n"
        "  }\n"
        "  return r;\n"
        "}\n");
}

/* The lens: droplets on the glass while the rain reaches the camera (none under a roof: texture matrix 6's byte 2 is rain.lens
 * times 0 there), a few dozen at a time in a grid of cells over the screen (more at the sides and the top), each a small
 * round lens that bends the scene behind it (the copy read with an offset toward its rim), a bright rim on its upper edge,
 * sliding down after a moment with a thin wet trail, fading as its cycle ends; lingering and drying off with the wetness
 * after the rain stops. Laid last, over the haze and the rain. */
/* the droplets on the lens: beads in cells of the screen (aspect units), each with a life - a drop arrives as a hit (a burst
 * that pulls back into one bead with a few satellites), sits, and a big one slides off down a wandering rivulet while a
 * small one dries; a bead is a lens (the scene behind it upside down and compressed, so its top is dark and its bottom the
 * sky), with a thin dark rim and a hard speck of light; lopsided, flat where it hangs, mostly tiny, few big; none over
 * the middle of the view, more towards the edges. The sliding beads come from the cells above, so those are looked at too */
static void emit_rain_lens(Sb* b, const char* ct, const char* cs)
{
    sb_printf(b,
        "  float lensk = ym[2][0];\n"
        "  float lpres = max(saturate(fall), wet * wet * wet);\n"
        "  if (lensk > 0.0 && lpres > 0.01) {\n"
        /* in units of the window's height: the scene's target may be another shape than the window ([graphics] background, a
         * square 3072x3072 say) and is stretched over it, so a round bead here is round there */
        "    float asp = ym[3][0];\n"
        "    float sx = asp * u.vp.w / u.vp.z;\n"
        "    float2 g = float2((vin.pos.x - u.vp.x) * sx, vin.pos.y - u.vp.y) / u.vp.w;\n"
        "    float2 gtouv = u.vp.w / wsz / float2(sx, 1.0);\n"
        "    float2 gi = floor(g / 0.085);\n"
        "    float3 lcol = col.rgb; float lw = 0.0;\n"
        "    for (int y = -5; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {\n"
        "      float2 k = gi + float2(x, y); float h = rain_hash(k + 31.7);\n"
        "      float per = 6.0 + 8.0 * h;\n"
        "      float cyc = t / per + h * 3.0; float n = floor(cyc); float age = (cyc - n) * per;\n"
        "      float2 c0 = (k + float2(0.2 + 0.6 * rain_hash(k + n * 0.41 + 5.0), 0.2 + 0.6 * rain_hash(k + n + 7.0))) * 0.085;\n"
        "      float cd = length(c0 - float2(0.5 * asp, 0.5));\n"
        "      float edgew = smoothstep(0.17, 0.42, cd) * (0.5 + 1.2 * cd);\n"
        "      if (rain_hash(k + n * 0.73 + 2.0) > lensk * lpres * 0.16 * edgew) continue;\n"
        "      float hr = rain_hash(k + n * 0.19 + 9.0);\n"
        "      float r = 0.0035 + 0.011 * hr * hr * hr + 0.003 * hr;\n"
        "      float big = step(0.0105, r);\n"
        "      float ts = per * (0.25 + 0.3 * rain_hash(k + n * 0.29 + 3.0));\n"
        "      float sl = big * max(age - ts, 0.0);\n"
        "      float slide = 0.03 * pow(sl, 1.7) * (0.6 + 0.4 * h);\n"
        "      if (y < -1 && slide < 0.085 * float(-y - 1)) continue;\n"
        "      float wob = 0.35 * r * sin(slide * 25.0 + h * 7.0);\n"
        "      float2 c = c0 + float2(wob, slide);\n"
        "      float endf = saturate((per - age) * (big > 0.5 ? 1.5 : 0.6));\n"
        "      float life = saturate(age * 12.0) * endf;\n"
        "      float2 dv = g - c;\n"
        "      dv.y /= 1.0 + 0.3 * (rain_hash(k + n * 0.57 + 11.0) - 0.4) + 0.5 * saturate(slide * 30.0);\n"
        "      float an = atan2(dv.y, dv.x); float d = length(dv);\n"
        "      float rr = r * (1.0 + 0.1 * sin(3.0 * an + h * 6.28) + 0.07 * sin(5.0 * an + hr * 6.28)) * (1.0 - 0.12 * smoothstep(-0.2, 0.3, dv.y / max(r, 1e-4)));\n"
        "      float burst = 1.0 + 0.5 * pow(saturate(1.0 - age / 0.15), 2.0);\n"
        "      float q = d / max(rr * burst, 1e-5);\n"
        "      if (q < 1.0) {\n"
        "        float2 off = -dv * (4.5 + 3.0 * q * q) * gtouv;\n"
        "        float3 see = %s.SampleLevel(%s, clamp(uv0 + off, uvlo, uvhi), 0).rgb * 1.12;\n"
        "        float lum = dot(see, float3(0.3, 0.5, 0.2));\n"
        "        see *= 1.0 - 0.5 * smoothstep(0.8, 1.0, q);\n"
        "        see += 0.12 * smoothstep(0.95, 0.7, q) * saturate(dv.y / max(d, 1e-4)) * lum;\n"
        "        float2 hp = dv / max(rr, 1e-5) - float2(-0.32, -0.38);\n"
        "        see += (0.5 + 0.4 * (burst - 1.0)) * saturate(lum * 1.5 + 0.1) * smoothstep(0.3, 0.0, length(hp));\n"
        "        lcol = see;\n"
        "        lw = max(lw, life * smoothstep(1.0, 0.9, q));\n"
        "      } else if (age < 0.5 && q < 2.6) {\n"
        /* the hit's satellites: three tiny beads flung round it, gone in half a second */
        "        float sat = 0.0;\n"
        "        for (int i = 0; i < 3; ++i) {\n"
        "          float sa = h * 6.28 + float(i) * 2.1 + hr; float sr = rr * (1.5 + 0.6 * rain_hash(k + float(i) + n));\n"
        "          sat = max(sat, smoothstep(0.3, 0.2, length(dv - float2(cos(sa), sin(sa)) * sr) / max(rr, 1e-5)));\n"
        "        }\n"
        "        if (sat > 0.0) { lcol = col.rgb * 1.25 + 0.05; lw = max(lw, sat * saturate(1.0 - age / 0.5)); }\n"
        "      } else if (big > 0.5 && slide > 0.0 && g.y < c.y && g.y > c0.y) {\n"
        /* the rivulet: one thin crooked track from where the bead sat to where it is, its edges dark, a little clearer in it */
        "        float ty = g.y - c0.y; float tx = c0.x + 0.35 * r * sin(ty * 25.0 + h * 7.0);\n"
        "        float tw = abs(g.x - tx) / max(r * 0.26, 1e-5);\n"
        "        if (tw < 1.0) {\n"
        "          float3 see = %s.SampleLevel(%s, clamp(uv0 + float2(0.0, 0.003), uvlo, uvhi), 0).rgb;\n"
        "          lcol = see * (1.04 - 0.22 * smoothstep(0.6, 1.0, tw));\n"
        "          lw = max(lw, endf * smoothstep(1.0, 0.8, tw) * 0.7);\n"
        "        }\n"
        "      }\n"
        "    }\n"
        /* the mist of tiny beads: no lens, a brighter speck with a dark ring, denser towards the edges */
        "    float2 mi = floor(g / 0.028); float mh = rain_hash(mi + 3.3);\n"
        "    float2 mg = (mi + 0.5) * 0.028; float md = length((mg - float2(0.5 * asp, 0.5)));\n"
        "    if (mh < lensk * lpres * 0.3 * smoothstep(0.2, 0.5, md)) {\n"
        "      float2 mc = (mi + float2(0.25 + 0.5 * rain_hash(mi + 1.1), 0.25 + 0.5 * rain_hash(mi + 2.2))) * 0.028;\n"
        "      float mq = length(g - mc) / (0.0022 + 0.002 * rain_hash(mi + 4.4));\n"
        "      if (mq < 1.0) { lcol = col.rgb * (1.2 - 0.6 * smoothstep(0.6, 1.0, mq)); lw = max(lw, smoothstep(1.0, 0.8, mq) * 0.9); }\n"
        "    }\n"
        "    col.rgb = lerp(col.rgb, lcol, saturate(lw));\n"
        "  }\n",
        ct, cs, ct, cs);
}

/* emit_rain_fs's tail for the mask (GfxFsKey.rain 3): `col` the wet ground's mask in place of the rain. dt/ds the depth copy */
static void emit_rain_mask(Sb* b, const char* dt, const char* ds)
{
    /* the wet ground's mask (GfxFsKey.rain 3, d3d8_rain.h rain_mask_draw) for the floor reflections: red where the ground is
     * wet (open to the sky, facing up; the wetness itself is the reflections' u.rain.x), green its wet patches - a noise in two sizes, which fx_ssr cuts by rain.puddles, the
     * low ground all wet: the pixel lower than the ground half a yalm round it (the world's y is down), read from
     * the depth at four points that far out across the screen - blue how open */
    sb_printf(b,
        "  float lowg = 0.0;\n"
        "  { float pxr = 0.5 / max(abs(z0) * u.params2.w * 2.0 / max(u.vp.w, 1.0), 1e-4);\n"
        "    pxr = clamp(pxr, 2.0, 0.08 * u.vp.w);\n"
        "    float ys = 0.0, yw = 0.0;\n"
        "    float2 off[4] = { float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0) };\n"
        "    [unroll] for (int li = 0; li < 4; ++li) {\n"
        "      float2 sp = vin.pos.xy + off[li] * pxr;\n"
        "      float2 suv = clamp(sp / wsz, uvlo, uvhi);\n"
        "      float zs = rain_viewz(%s.SampleLevel(%s, suv, 0).r, zp, ws.x, 1.0);\n"
        "      float2 sn = float2((sp.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (sp.y - u.vp.y) / u.vp.w * 2.0);\n"
        "      float3 Ps = mul(u.texm[3], float4(sn.x * abs(zs) * u.params2.z, sn.y * abs(zs) * u.params2.w, zs, 1.0)).xyz;\n"
        /* only ground: a point on an upright face in front (a tree's trunk, a step's or a wall's foot) was "higher ground"
         * by camera angle alone, and laid puddles beside it that moved as the camera did - so no steeper than some 20 degrees */
        "      float dyl = abs(Ps.y - P.y), dxz = length(Ps.xz - P.xz);\n"
        "      float ok = dyl < 0.6 && dyl < 0.35 * dxz + 0.02 ? 1.0 : 0.0;\n"
        "      ys += Ps.y * ok; yw += ok; }\n"
        "    if (yw > 1.5) lowg = smoothstep(0.015, 0.08, P.y - ys / yw); }\n"
        "  float pn = rain_noise(P.xz * 0.35) * 0.65 + rain_noise(P.xz * 1.3 + 17.0) * 0.35;\n"
        "  float pm = saturate(max(pn, lowg)) * smoothstep(0.55, 0.78, fu) * open * gwet;\n"
        "  col = float4(gopen, pm, open, 1.0);\n",
        dt, ds);
}

/* the pixel function's body: `col` out. The scene's colour at the pixel, its depth to where the pixel stands in view and in the
 * world; the streaks on layers out along the view ray that the scene hides past its depth and the shelter keeps out; the
 * splashes where the drops land on what faces up (rings and hits, and a crown that rises off the surface, found by marching down
 * the screen for a hit below). The fog takes the far layers; the wet ground is the reflections' (fx_ssr). */
static void emit_rain_fs(Sb* b, int mask)
{
    char ct[32], cs[32], dt[32], ds[32], mt[32], ms[32];
    water_tex(ct, cs, sizeof ct, 6);
    water_tex(dt, ds, sizeof dt, 7);
    water_tex(mt, ms, sizeof mt, 5);
    sb_printf(b,
        "  float4x4 wt = u.texm[6];\n"
        "  float4 zp = float4(wt[0][1], wt[1][1], wt[2][1], wt[3][1]);\n"
        "  float4 ws = float4(wt[0][2], wt[1][2], wt[2][2], wt[3][2]);\n"
        "  float4x4 rm = u.texm[7];\n"
        "  float t = rm[0][0], fall = rm[1][0], dens = rm[2][0], spl = rm[3][0];\n"
        "  float3 cam = float3(rm[0][1], rm[1][1], rm[2][1]);\n"
        "  float2 cov0 = float2(rm[3][1], rm[0][2]); float covk = rm[1][2]; float wet = rm[2][2]; float hasmap = rm[3][2];\n"
        "  float fogend = rm[0][3], dbg = rm[1][3], covy = rm[2][3], gwet = rm[3][3];\n"
        "  float4x4 ym = u.texm[6];\n"
        "  float2 wsz = ws.yz;\n"
        "  float2 uv0 = vin.pos.xy / wsz;\n"
        "  float2 uvlo = (u.vp.xy + 1.0) / wsz, uvhi = (u.vp.xy + u.vp.zw - 1.0) / wsz;\n"
        "  float4 col = float4(%s.SampleLevel(%s, uv0, 0).rgb, 1.0);\n"
        "  float3 c0 = col.rgb;\n"
        "  float2 ndc = float2((vin.pos.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (vin.pos.y - u.vp.y) / u.vp.w * 2.0);\n"
        "  float z0 = rain_viewz(%s.SampleLevel(%s, uv0, 0).r, zp, ws.x, 1.0);\n"
        "  float3 pe = float3(ndc.x * abs(z0) * u.params2.z, ndc.y * abs(z0) * u.params2.w, z0);\n"
        "  float3 P = mul(u.texm[3], float4(pe, 1.0)).xyz;\n"
        "  float3 ray = P - cam; float dist = length(ray); float3 dir = ray / max(dist, 1e-4);\n"
        "  float3 sky = u.fogcolor.rgb * 0.6 + 0.25;\n"
        "  float3 drop = max(sky, 0.42);\n"
        "  float3 add = float3(0, 0, 0);\n"
        /* the layers, nearest first: each hidden where the scene is nearer than it and where the shelter map has a roof over
         * its point on the view ray; its streaks in the world's angle space round the camera, the ray's azimuth and elevation
         * (the world's y is down) - laid in the screen's before, they turned with the camera */
        "  float hz = length(dir.xz);\n"
        "  float2 ang = float2(atan2(dir.x, dir.z), atan2(-dir.y, max(hz, 1e-5)));\n"
        /* the wind: a lean that drifts (minutes), a squall's hard, along the world's x as the drops near the camera drift (seen
         * across it most, none looking along it), with gusts passing through as waves */
        "  float lean = 0.05 + 0.15 * saturate(fall - 0.9);\n"
        "  float wind = lean * (0.6 + 0.4 * sin(t * 0.09)) * sign(sin(t * 0.013 + 0.4)) * cos(ang.x) + 0.04 * sin(t * 1.1 + ang.y * 3.0 + ang.x * 2.0) + 0.02 * sin(t * 0.5 - ang.x * 4.0);\n"
        "  float2 as = float2(ang.x + ang.y * wind, ang.y);\n"
        "  float pxa = 2.0 * u.params2.w / max(u.vp.w, 1.0);\n"
        "  float dark = 1.0 - saturate(dot(col.rgb, float3(0.3, 0.59, 0.11)) * 1.3);\n"
        "  float gain = 0.5 + 0.5 * dark;\n"
        /* only past the far drops (d3d8_rain.h FAR_REACH, matrix 6's [0][3]): nearer, the rain is drops in the world, which the
         * camera moves through; layers stay round the camera as it goes, and out here that hardly shows */
        "  float lay[2] = { ym[0][3] * 1.05 + 4.0, ym[0][3] * 1.6 + 10.0 };\n"
        "  float lk[2] = { 120.0, 170.0 };\n"
        "  float la[2] = { 0.14, 0.1 };\n"
        "  float ll[2] = { 0.25, 0.2 };\n"
        "  float ls[2] = { 1.3, 1.2 };\n"
        /* straight down or up the columns crowd together into one point (and shimmer): the layers fade out as the ray turns
         * past some 50 degrees off level; the drops near the camera (rain_particles) are the rain there */
        "  float steep = 1.0 - smoothstep(0.75, 0.93, abs(dir.y));\n"
        "  float wide = 1.0 / max(hz, 0.3);\n"
        "  float sacc = 0.0;\n"
        "  for (int i = 0; i < 2; ++i) {\n"
        "    float L = lay[i];\n"
        "    if (L >= dist) break;\n"
        "    float3 Q = cam + dir * L;\n"
        "    float2 cq = (Q.xz - cov0) * covk;\n" /* off the map, no roof (clamped, its edge's roofs ran on out of it) */
        "    float roof = hasmap > 0.5 && all(cq == saturate(cq)) ? rain_roof(%s.SampleLevel(%s, cq, 0).rg, covy) : 1e9;\n"
        "    if (Q.y > roof) continue;\n"
        "    float nc = max(round(lk[i] * 6.2831853), 1.0), kk = nc / 6.2831853;\n"
        "    float hw = max(1.7 * pxa * kk * wide, 0.016);\n"
        "    float s = rain_streaks(as, kk, nc, t, dens * fall, ls[i], ll[i], hw);\n"
        "    float a = la[i] * (fogend > 0.0 ? 1.0 - saturate(L / fogend) : 1.0);\n"
        "    sacc += s * a;\n"
        "  }\n"
        "  add += drop * (sacc * steep * saturate(fall) * gain);\n"
        /* the surface: its facing from the world position's change pixel to pixel */
        "  float3 nw = cross(ddx(P), ddy(P));\n"
        "  float fu = abs(nw.y) * rsqrt(max(dot(nw, nw), 1e-12));\n"
        /* how open the point is, 0 under a roof to 1: the shelter map read at nine cells round it, each cell's roof tested and
         * weighed by a tent 1.5 cells (some 1.1 yalms) wide, so wet fades into dry over a yalm or two instead of the map's cells
         * showing as steps. A roof counts over the point only 1.2 yalms or more above it: a plinth's or a step's top beside the
         * ground (the cell's height taken from its middle) left dry squares round them. 128: d3d8_rain.h COVER_N */
        "  float open = 1.0;\n"
        "  if (hasmap > 0.5) {\n"
        "    float2 tc = (P.xz - cov0) * covk * 128.0 - 0.5; float2 tb = floor(tc + 0.5); float sw = 0.0, so = 0.0;\n"
        "    for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {\n"
        "      float2 c = tb + float2(ox, oy); float2 dd = abs(tc - c);\n"
        "      float wt = max(0.0, 1.0 - dd.x / 1.5) * max(0.0, 1.0 - dd.y / 1.5);\n"
        "      float2 cuv = (c + 0.5) / 128.0;\n"
        "      float rf = all(cuv == saturate(cuv)) ? rain_roof(%s.SampleLevel(%s, cuv, 0).rg, covy) : 1e9;\n"
        "      so += wt * (P.y > rf + 1.2 ? 0.0 : 1.0); sw += wt;\n"
        "    }\n"
        "    open = sw > 0.0 ? so / sw : 1.0;\n"
        "  }\n"
        /* the haze: rain lifts a veil with distance toward the fog's colour (the sky's grey lit it pale at night), none on what
         * is under a roof */
        "  float3 skyg = u.fogcolor.rgb * 0.7 + 0.03;\n"
        "  float haze = 0.22 * saturate(fall) * (1.0 - exp(-dist / 55.0)) * open;\n"
        "  col.rgb = lerp(col.rgb, skyg, haze);\n"
        "  float near = smoothstep(10.0, 16.0, dist) * (1.0 - smoothstep(18.0, 30.0, dist));\n"
        "  float upfac = smoothstep(0.35, 0.75, fu) * open * near;\n"
        "  float ring = rain_rings(P.xz, t, fall * 1.2) * upfac;\n"
        "  col.rgb = lerp(col.rgb, min(drop * 0.85, col.rgb * 1.3 + 0.04), saturate(ring) * (0.25 * spl * saturate(fall)));\n"
        /* the wet ground (rain.ground, gwet) is the reflections' alone (gfx_d3d12.c fx_ssr, by the mask's patches): no darkening,
         * puddles or sheen of the rain pass's own. Read from each triangle's facing they shaded the low-poly ground's and rocks'
         * facets apart, pale and dark triangles (La Theine's bones), and with Reflections off. gopen: the mask's red */
        "  float gopen = smoothstep(0.3, 0.6, fu) * open * gwet;\n"
        "  col.rgb = saturate(col.rgb + add);\n",
        ct, cs, dt, ds, mt, ms, mt, ms);
    if (mask)
    {
        emit_rain_mask(b, dt, ds);
        return;
    }
    emit_rain_lens(b, ct, cs);
    sb_printf(b,
        /* the diagnostics (rain.force 2 to 4): the world position's fractions as read (2), as the transposed matrix reads it (3),
         * and the rain's own light times 3 with the shelter as red (4) */
        "  if (dbg > 0.5 && dbg < 1.5) col.rgb = frac(P);\n"
        "  if (dbg > 1.5 && dbg < 2.5) col.rgb = frac(mul(transpose(u.texm[3]), float4(pe, 1.0)).xyz);\n"
        "  if (dbg > 2.5 && dbg < 4.5) col.rgb = float3(1.0 - open, 0.0, 0.0) * 0.5 + saturate(add * 3.0);\n"
        /* the change the rain made, as out = target x a + rgb (blended ONE, SRCALPHA: d3d8_rain.h rain_pass): a the brightness
         * kept (the haze's and the rings' share), rgb what it lays on. Where the target is the copy, exactly
         * what was worked out; where something was drawn since (the water pass's water), the same change over it */
        "  float kl0 = dot(c0, float3(0.3, 0.59, 0.11)), kl1 = dot(col.rgb, float3(0.3, 0.59, 0.11));\n"
        "  float kk = kl0 > 1e-3 ? saturate(kl1 / kl0) : 1.0;\n"
        "  col = float4(max(col.rgb - c0 * kk, 0.0), kk);\n"
        /* Debug view 31 (dbg 5, /vana debug's Rain): the picture in grey, blue where the shelter has a roof over it, the screen's
         * streaks red over it, the target replaced (a 0: none of it kept); the drops and splashes after it are red
         * (d3d8_rain.h rain_colour) */
        "  float gv = dot(c0, float3(0.3, 0.59, 0.11));\n"
        "  float3 gb = lerp(gv.xxx, float3(0.25, 0.5, 1.0) * (0.35 + 0.65 * gv), (1.0 - open) * 0.7);\n"
        "  if (dbg > 4.5 && dbg < 5.5) col = float4(lerp(gb, float3(1.0, 0.0, 0.0), saturate(sacc * steep * saturate(fall) * 6.0)), 0.0);\n"
        /* Debug view 32 (dbg 6, /vana debug's Rain rings): the picture in grey tinted by the rain's 100-yalm bands out from the
         * camera along the ground (d3d8_rain.h rain_band: green all, then yellow, orange, red, purple), dark lines at the near
         * drops' and the middle ring's edges (16, 48), cyan at the splash reach (matrix 6's [1][3]), white at the rain's
         * (its [0][3]); past it darker, no rain */
        "  if (dbg > 5.5) {\n"
        "    float dh = length((P - cam).xz), sr = ym[1][3], rr = ym[0][3];\n"
        "    float l = dot(c0, float3(0.3, 0.59, 0.11)), bd = floor(dh / 100.0);\n"
        "    float3 k = bd < 1.0 ? float3(0.62, 0.81, 0.42) : bd < 2.0 ? float3(0.91, 0.83, 0.36) : bd < 3.0 ? float3(0.91, 0.58, 0.36)\n"
        "             : bd < 4.0 ? float3(0.93, 0.44, 0.47) : float3(0.70, 0.45, 0.90);\n"
        "    float3 o = dh < rr ? lerp(l.xxx, k * (0.35 + 0.65 * l), 0.6) : l.xxx * 0.45;\n"
        "    float pw = max(fwidth(dh), 1e-3);\n"
        "    o *= lerp(0.3, 1.0, smoothstep(0.0, 1.5 * pw, min(abs(dh - min(16.0, sr)), abs(dh - min(48.0, rr - 8.0)))));\n"
        "    o = lerp(float3(0.2, 0.9, 1.0), o, smoothstep(0.0, 2.5 * pw, abs(dh - sr)));\n"
        "    o = lerp(float3(1.0, 1.0, 1.0), o, smoothstep(0.0, 2.5 * pw, abs(dh - rr)));\n"
        "    col = float4(o, 0.0);\n"
        "  }\n");
}

/* The rain's splashes and rings (GfxFsKey.rain 2, d3d8_rain.h particles_draw): drawn only where they're within half a yalm in
 * front of the surface the scene shows at the pixel (the pass's depth copy in stage 7, its projection values in matrix 6, as
 * the water pass's): over the sky, the sea or past a wall's edge, nothing */
static void emit_rain_soft(Sb* b)
{
    char dt[32], ds[32];
    water_tex(dt, ds, sizeof dt, 7);
    sb_printf(b,
        "  {\n"
        "    float4x4 swt = u.texm[6];\n"
        "    float4 szp = float4(swt[0][1], swt[1][1], swt[2][1], swt[3][1]);\n"
        "    float4 sws = float4(swt[0][2], swt[1][2], swt[2][2], swt[3][2]);\n"
        "    float sz = rain_viewz(%s.SampleLevel(%s, vin.pos.xy / sws.yz, 0).r, szp, sws.x, 1.0);\n"
        "    float pz = rain_viewz(vin.pos.z, szp, sws.x, 1.0);\n"
        "    cur.a *= 1.0 - smoothstep(0.25, 0.5, abs(sz - pz));\n"
        "  }\n",
        dt, ds);
}
