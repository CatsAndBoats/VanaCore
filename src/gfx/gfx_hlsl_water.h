/* VanaCore's water, for a draw made it (GfxFsKey.water; d3d8_water.h water_shade) in place of the game's layers over it. No
 * textures of the game's: a sum of travelling sine waves (the slope of each, in the world's x and z; the water is flat at the
 * tile's height) and two layers of fine ripples (water_noise, a value noise scrolling two ways) bend the surface normal, and the
 * colour is the sky mirrored in it - the zone's fog colour at the horizon, a blue overhead, by day - against what's under the
 * surface, by Fresnel, with the sun and moon's glint on top. In the water pass (water 2) what's under the surface is the scene
 * as it was drawn (copies of its colour and depth in stages 6 and 7, d3d8_water.h water_pass_draw): the depth under each pixel
 * says how far the light goes through the water to the bed - the bed shows through in the shallows, the water's own colour takes
 * over with depth, each colour at its own rate so the bed goes blue-green before it goes - and the colour is read where the
 * waves bend the view to, never from above the surface; a soft edge and a line of foam where the depth reaches nothing at the
 * shore. For the game's water baked into its ground (water 3, d3d8_water.h water_ground_pass) the scene's depth under the
 * surface is the ground's own, so the bed's depth comes with each vertex instead (the first texture coordinate's x: how far
 * under the surface the zone's collision has its water floor), and the light's way through the water is that depth along the
 * view; what shows through is still the scene's colour, read where the waves bend the view. In its place (water 1) the water's
 * own colour alone, by the view's angle. The light is the zone's own (the draw's
 * lights, as an upward-facing surface would take them), so it dims at night; fog is applied after (emit_fs_tail).
 *   Time is texture matrix 7's byte 3, wrapped at 600 s: each wave turns a whole number of times in that, and the ripples scroll
 * a whole number of their 64-cell periods, so the wrap is seamless. Matrix 7's bytes 4-15 are the settings (waves, size,
 * reflection, glint, brightness, colour, shallowness, turbulence, the direction's cosine and sine, the glint's size, the ripples'
 * size), matrix 6's bytes 0-3 a picked colour, 4-10 the pass's depth reading (the projection's z scale, offset and hand, the
 * viewport's depth range, the target's size), 11-15 how far one sees in, the refraction, the shore's width, the foam and the
 * ripples' strength; matrix 3 is the draw's world matrix (the vertex function takes the world position through it) and matrix 4
 * the view (the world's up and the surface normal go through it, so a turned or scaled model's water lies as a tile's does). Each wave and ripple layer fades out where its length nears the pixels' (the world distance one pixel
 * covers), so a distant sea doesn't crawl. gfx_hlsl.c includes this after emit_fs_tail, whose names it uses; it is no file of
 * its own. */
#pragma once

/* the texture and sampler of stage i, as the back end names them (gfx_hlsl_sample's) */
static void water_tex(char* tex, char* smp, size_t n, int i)
{
#ifdef VANACORE
    if (gfx_hlsl_d3d11)
    {
        snprintf(tex, n, "tx2_%d", i);
        snprintf(smp, n, "smp_%d", i);
        return;
    }
#endif
    const char c = "xyzw"[i & 3];
    snprintf(tex, n, "tx2[ti[%d].%c]", i >> 2, c);
    snprintf(smp, n, "smp[si[%d].%c]", i >> 2, c);
}

/* the functions the water's pixel function calls, at file scope before it: a hash, a value noise with its gradient over a
 * 64-cell period, and the view-space z a depth value stands for */
static void emit_water_functions(Sb* b)
{
    sb_printf(b,
        "float water_hash(float2 p) {\n"
        "  float3 q = frac(p.xyx * 0.1031);\n"
        "  q += dot(q, q.yzx + 33.33);\n"
        "  return frac((q.x + q.y) * q.z);\n"
        "}\n"
        /* value noise at x (cell units), periodic every 64 cells: its value (0 to 1) and its gradient per cell */
        "float3 water_noise(float2 x) {\n"
        "  float2 c = floor(x), f = x - c;\n"
        "  c -= 64.0 * floor(c / 64.0);\n"
        "  float2 c1 = c + 1.0;\n"
        "  c1 -= 64.0 * floor(c1 / 64.0);\n"
        "  float2 w = f * f * (3.0 - 2.0 * f), dw = 6.0 * f * (1.0 - f);\n"
        "  float a = water_hash(c), bb = water_hash(float2(c1.x, c.y)), cc = water_hash(float2(c.x, c1.y)), d = water_hash(c1);\n"
        "  float ab = bb - a, cd = d - cc;\n"
        "  float v = lerp(a + ab * w.x, cc + cd * w.x, w.y);\n"
        "  return float3(v, dw.x * lerp(ab, cd, w.y), dw.y * ((cc + cd * w.x) - (a + ab * w.x)));\n"
        "}\n"
        /* the view-space z of a depth value (clip w = z * hand): zp the projection's z scale, offset and hand and the viewport's
         * least depth, zr the viewport's depth range; the far plane (the sky, cleared depth) is far below the surface at zs */
        /* Worley cells for the foam's bubbles: the nearest and second-nearest of the feature points of the cell of x and its
         * eight neighbours (one a cell, drifting round the cell's middle with time, so the cells pop and reform) */
        "float2 water_cells(float2 x, float t) {\n"
        "  float2 c = floor(x), f = x - c;\n"
        "  float d1 = 8.0, d2 = 8.0;\n"
        "  [unroll] for (int j = -1; j <= 1; ++j) [unroll] for (int i = -1; i <= 1; ++i) {\n"
        "    float2 o = float2(i, j), cc = c + o;\n"
        "    cc -= 64.0 * floor(cc / 64.0);\n"
        "    float h1 = water_hash(cc), h2 = water_hash(cc + 17.3);\n"
        "    float2 q = o + 0.5 + 0.38 * float2(sin(t * (0.6 + h1) + h1 * 6.2832), cos(t * (0.5 + h2) + h2 * 6.2832)) - f;\n"
        "    float d = dot(q, q);\n"
        "    if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d; }\n"
        "  return float2(sqrt(d1), sqrt(d2));\n"
        "}\n"
        "float water_viewz(float d, float4 zp, float zr, float zs) {\n"
        "  d = (d - zp.w) / max(zr, 1e-6);\n"
        "  float z = zp.y / (d * zp.z - zp.x);\n"
        "  if (d >= 0.999999 || !(z * zp.z > 0.0)) return zs * 1000.0;\n"
        "  return z;\n"
        "}\n");
}

/* the settings, where the pixel stands and the surface's slope from the swell and the ripples: g (the world's x and z), fp (the
 * world distance a pixel covers), foam_n (the ripples' height, for the foam) */
static void emit_water_surface(Sb* b, int water)
{
    static const struct
    {
        float lambda, slope, angle;
        int turns; /* revolutions in the 600 s */
    } WAVE[] = {
        { 9.0f, 0.040f, 0.20f, 125 }, { 6.3f, 0.035f, 1.10f, 150 }, { 4.1f, 0.030f, 2.30f, 185 }, { 2.7f, 0.028f, 3.40f, 228 },
        { 1.7f, 0.022f, 4.40f, 288 }, { 1.1f, 0.018f, 5.20f, 357 }, { 0.7f, 0.012f, 0.80f, 448 },
    };
    sb_printf(b,
        "  float4x4 wm = u.texm[7];\n"
        "  float t = wm[3][0];\n"
        "  float4 q1 = float4(wm[0][1], wm[1][1], wm[2][1], wm[3][1]);\n" /* waves, size, reflection, glint */
        "  float4 q2 = float4(wm[0][2], wm[1][2], wm[2][2], wm[3][2]);\n" /* brightness, colour, shallowness, turbulence */
        "  float4 q3 = float4(wm[0][3], wm[1][3], wm[2][3], wm[3][3]);\n" /* the waves' direction (cos, sin), the glint's size, the ripples' size */
        "  float4x4 wt = u.texm[6];\n"
        "  float4 tn = float4(wt[0][0], wt[1][0], wt[2][0], wt[3][0]);\n" /* a picked colour (rgb) when w is 1 */
        "  float4 zp = float4(wt[0][1], wt[1][1], wt[2][1], wt[3][1]);\n" /* the projection's z scale, offset and hand; the viewport's least depth */
        "  float4 ws = float4(wt[0][2], wt[1][2], wt[2][2], wt[3][2]);\n" /* the viewport's depth range, the target's size, how far one sees in */
        "  float4 wq = float4(wt[0][3], wt[1][3], wt[2][3], wt[3][3]);\n" /* refraction, the shore's width, foam, ripples */
        "  float3 p = vin.wp;\n"
        "  float3 pe = vin.wpe;\n"
        "  float dist = length(pe);\n"
        "  float3 V = -pe / max(dist, 1e-4);\n"
        "  float3 up = normalize(mul(u.texm[4], float4(0.0, -1.0, 0.0, 0.0)).xyz);\n"
        "  float fp = max(length(ddx(p.xz)), length(ddy(p.xz))) + 1e-4;\n"
        "  float2 g = float2(0.0, 0.0);\n"
        "  float ocean_foam = 0.0, ocean_h = 0.0;\n");
    if (water == 4)
    {
        /* the FFT ocean (d3d8_water_ocean.h): its swell and chop cascades in stages 4 and 5, tiling 223 and 31.7 yalms times
         * the square root of the size setting (q1.y); R and G their slopes (by OCEAN_SMAX, 1.25), B the foam, A the height */
        char t4[32], s4[32], t5[32], s5[32];
        water_tex(t4, s4, sizeof t4, 4);
        water_tex(t5, s5, sizeof t5, 5);
        sb_printf(b,
            "  { float osz = sqrt(max(q1.y, 0.25));\n"
            "    float4 oa = %s.Sample(%s, p.xz / (223.0 * osz));\n"
            "    float4 ob = %s.Sample(%s, p.xz / (31.7 * osz));\n"
            "    g = ((oa.rg - 0.50196) + (ob.rg - 0.50196)) * (1.25 * 255.0 / 127.0);\n"
            "    ocean_foam = saturate(oa.b + ob.b * 0.8);\n"
            "    ocean_h = (oa.a - 0.50196) * 2.0 + (ob.a - 0.50196); }\n",
            t4, s4, t5, s5);
    }
    for (size_t i = 0; water != 4 && i < sizeof WAVE / sizeof WAVE[0]; ++i)
    {
        const float k = 6.2831853f / WAVE[i].lambda;
        /* the direction turned by q3 (the setting's); the small ripples (the last four) are scaled by the turbulence, q2.w */
        const float cx = cosf(WAVE[i].angle), sx = sinf(WAVE[i].angle);
        sb_printf(b,
            "  { float2 dr = float2(%.6f * q3.x - %.6f * q3.y, %.6f * q3.y + %.6f * q3.x);\n"
            "    float ph = %.6f * dot(p.xz, dr) / q1.y + %.6f * t;\n"
            "    g += (%.6f * q1.x * %s * saturate(%.6f * q1.y / fp - 1.0)) * cos(ph) * dr; }\n",
            cx, sx, cx, sx, k, 6.2831853f * (float)WAVE[i].turns / 600.0f, WAVE[i].slope, i >= 3 ? "q2.w" : "1.0",
            WAVE[i].lambda / 6.0f);
    }
    /* the fine ripples: two layers of value noise, two octaves each, cells of 0.55 and 1 yalm (times the setting), turned 0.7 rad
     * one way and 1.1 the other from the waves' direction and scrolling along their own, 192 and 128 cells in the 600 s (whole
     * periods: seamless at the wrap); a layer's height 8 and 12 mm times the setting, its slope that over its cell. Each fades
     * out as the waves do where its cells near the pixels' size */
    sb_printf(b,
        "  float foam_n = 0.0;\n"
        "  { float rs = max(q3.w, 0.25);\n"
        "    float2 da = float2(0.764842 * q3.x - 0.644218 * q3.y, 0.764842 * q3.y + 0.644218 * q3.x);\n"
        "    float2 db = float2(0.453596 * q3.x + 0.891207 * q3.y, 0.453596 * q3.y - 0.891207 * q3.x);\n"
        "    float ca = 0.55 * rs, cb = 1.0 * rs;\n"
        "    float2 xa = float2(dot(p.xz, da), dot(p.xz, float2(-da.y, da.x))) / ca + float2(0.32 * t, 0.0);\n"
        "    float2 xb = float2(dot(p.xz, db), dot(p.xz, float2(-db.y, db.x))) / cb - float2(0.213333 * t, 0.0);\n"
        "    float3 na = water_noise(xa), na2 = water_noise(xa * 2.0 + 7.1);\n"
        "    float3 nb = water_noise(xb), nb2 = water_noise(xb * 2.0 + 3.7);\n"
        "    float fa = saturate(ca / (6.0 * fp) - 1.0), fb = saturate(cb / (6.0 * fp) - 1.0);\n"
        "    float2 ga = (na.yz + na2.yz) * (0.008 * wq.w * fa / ca), gb = (nb.yz + nb2.yz) * (0.012 * wq.w * fb / cb);\n"
        "    g += ga.x * da + ga.y * float2(-da.y, da.x) + gb.x * db + gb.y * float2(-db.y, db.x);\n"
        "    foam_n = na.x * 0.6 + nb.x * 0.4; }\n"
        /* a waterfall's foot (matrix 5's rows 1-3: x, z, radius, strength; d3d8_water.h water_feet_set): within its radius the
         * water churns, a fast value noise of 0.6-yalm cells scrolling 576 and 448 cells in the 600 s (whole periods), and its
         * foam (foot_n, for emit_water_depth) */
        "  float foot = 0.0, foot_n = 0.0;\n"
        /* (the matrices are column_major: bytes 4-7, 8-11 and 12-15 are columns 1 to 3, read as q1 to q3 are) */
        "  { float4x4 fm = u.texm[5]; float fs = 0.0;\n"
        "    float4 f1 = float4(fm[0][1], fm[1][1], fm[2][1], fm[3][1]);\n"
        "    float4 f2 = float4(fm[0][2], fm[1][2], fm[2][2], fm[3][2]);\n"
        "    float4 f3 = float4(fm[0][3], fm[1][3], fm[2][3], fm[3][3]);\n"
        "    if (f1.z > 0.0) { foot += 1.0 - smoothstep(0.35, 1.5, length(p.xz - f1.xy) / f1.z); fs = f1.w; }\n"
        "    if (f2.z > 0.0) { foot += 1.0 - smoothstep(0.35, 1.5, length(p.xz - f2.xy) / f2.z); fs = f2.w; }\n"
        "    if (f3.z > 0.0) { foot += 1.0 - smoothstep(0.35, 1.5, length(p.xz - f3.xy) / f3.z); fs = f3.w; }\n"
        "    foot = saturate(foot) * fs;\n"
        /* three octaves of the value noise, cells of 0.5, 0.24 and 0.11 yalms, each scrolling whole periods in the 600 s; the
         * froth (foot_n) is their sum, read through a soft ramp in emit_water_depth so it comes out as froth, not as the
         * noise's cells */
        "    if (foot > 0.0) { float2 xc = p.xz / 0.5 + float2(0.96 * t, -0.746667 * t);\n"
        "      float3 nc = water_noise(xc), nc2 = water_noise(xc * 2.1 + 5.3), nc3 = water_noise(xc * 4.3 - 11.7);\n"
        "      float fc = saturate(0.5 / (6.0 * fp) - 1.0), fc2 = saturate(0.24 / (6.0 * fp) - 1.0), fc3 = saturate(0.11 / (6.0 * fp) - 1.0);\n"
        "      g += (nc.yz * fc + 0.5 * nc2.yz * fc2 + 0.25 * nc3.yz * fc3) * (0.045 * foot);\n"
        "      foot_n = nc.x * 0.5 + nc2.x * 0.3 * fc2 + nc3.x * 0.2 * fc3 + (1.0 - fc2) * 0.15 + (1.0 - fc3) * 0.1; } }\n"
        /* and flatter at grazing angles (under about 15 degrees): the big swells' slopes flipped the reflection between the sky
         * and the body there, high-contrast bands across the far sea (Lower Jeuno, waves 3, size 4) */
        "%s"
        "  float3 N = normalize(mul(u.texm[4], float4(normalize(float3(-g.x, -1.0, -g.y)), 0.0)).xyz);\n"
        "  float cv = saturate(dot(N, V));\n"
        "  float F = saturate((0.02 + 0.98 * pow(1.0 - cv, 5.0)) * q1.z);\n"
        "  float ry = saturate(dot(reflect(-V, N), up));\n",
        /* the ocean's slopes are already filtered by distance (the mips): only a little flatter at grazing angles */
        water == 4 ? "  g *= lerp(0.55, 1.0, saturate(dot(up, V) * 4.0));\n"
                   : "  g *= lerp(0.3, 1.0, saturate(dot(up, V) * 4.0)) / (1.0 + dist * 0.004);\n");
}

/* the light on the water (Lc, spc from the sun and the moon), the sky mirrored in it and the water's own colours */
static void emit_water_light(Sb* b, const GfxVsKey* vk)
{
    sb_printf(b,
        "  float3 Lc = u.ambient.rgb;\n"
        "  float3 spc = float3(0.0, 0.0, 0.0);\n"
        /* the wavelets as relief (water.relief, matrix 5's byte 3): a broad lobe of the sun on the slopes that face it (spcb), and
         * the crests lit and the troughs shaded by how the perturbed normal leans toward the light against the flat surface (rel) */
        "  float q_rel = u.texm[5][3][0];\n"
        "  float3 spcb = float3(0.0, 0.0, 0.0);\n"
        "  float rel = 0.0;\n");
    for (int i = 0; i < vk->nlights; ++i)
    {
        if (vk->light_type[i] != 3) /* the sun and the moon; points and spots are per vertex and leave the sea alone */
            continue;
        sb_printf(b,
            "  { Light L = u.light[%d];\n"
            "    Lc += L.ambient.rgb + L.diffuse.rgb * max(dot(up, L.dir.xyz), 0.0);\n"
            "    spc += L.diffuse.rgb * pow(max(dot(N, normalize(V + L.dir.xyz)), 0.0), 220.0 / q3.z);\n"
            "    spcb += L.diffuse.rgb * pow(max(dot(N, normalize(V + L.dir.xyz)), 0.0), 14.0);\n"
            "    rel += dot(N, L.dir.xyz) - dot(up, L.dir.xyz); }\n",
            i);
    }
    sb_printf(b,
        "  float day = saturate(dot(Lc, float3(0.30, 0.59, 0.11)));\n"
        "  float3 hor = u.fogcolor.rgb;\n"
        "  float3 zen = lerp(float3(0.01, 0.02, 0.05), float3(0.12, 0.22, 0.42), day);\n"
        "  float3 sky = lerp(hor, zen, pow(ry, 0.45));\n"
        /* the shallows' colour is the brighter: a sunlit sand bed under a yalm or two of water (Rabao's pond) read as mud with the
         * darker one, and a shaded stream (West Ronfaure's) went navy beside the game's pale sheet */
        "  float3 deepc = tn.a > 0.5 ? tn.rgb * 0.4 : lerp(float3(0.015, 0.07, 0.16), float3(0.025, 0.13, 0.10), q2.y);\n"
        "  float3 shalc = tn.a > 0.5 ? tn.rgb : lerp(float3(0.09, 0.30, 0.46), float3(0.14, 0.42, 0.28), q2.y);\n"
        "  float3 deep = deepc * ((0.3 + 0.7 * day) * q2.x);\n"
        "  float3 shallow = shalc * ((0.3 + 0.7 * day) * q2.x);\n");
}

/* the caustics on the bed, into the scene's colour read under the pixel (sc), before it is lit through the water */
static void emit_water_caustics(Sb* b, int ground)
{
    /* caustics on the bed (water.caustics, matrix 5's byte 2): two scrolling ridged noise layers (cells 0.9 and 1.4 yalms, whole
     * periods in the 600 s) brighten what shows through, by day, fading with the bed's depth and where their cells near the
     * pixels' */
    sb_printf(b,
        "  { float q_cau = u.texm[5][2][0];\n"
        "    float Dc = %s;\n"
        /* ... not on what stands in the water (a pier's face, a pillar's: the depth under the pixel falling steeply across the
         * screen, and reaching nothing at the waterline), which took the net as a glowing ring round the piers */
        "    float cslope = 1.0 - smoothstep(0.35, 1.2, length(float2(ddx(Dc), ddy(Dc))) / max(fp, 1e-4));\n"
        "    float cstr = q_cau * day * exp(-Dc / max(ws.w * 0.6, 0.3)) * saturate(0.9 / (6.0 * fp) - 1.0) * smoothstep(0.08, 0.35, Dc) * cslope;\n"
        "    if (cstr > 0.002) {\n"
        "      float n1 = water_noise(p.xz / 0.9 + float2(0.21 * t, 0.17 * t)).x;\n"
        "      float n2 = water_noise(p.xz / 1.4 + float2(-0.15 * t, 0.12 * t) + 2.7).x;\n"
        "      float caus = pow(saturate(1.0 - abs(n1 - 0.5) * 2.0), 3.0) * 0.6 + pow(saturate(1.0 - abs(n2 - 0.5) * 2.0), 3.0) * 0.6;\n"
        "      sc *= 1.0 + caus * cstr * 1.1; } }\n",
        ground ? "Dv" : "max(hs * (1.0 - r1), 0.0)");
}

/* the shore's foam and its bubbles, over col; dt and ds the depth copy and its sampler (the edge test) */
static void emit_water_foam(Sb* b, const char* dt, const char* ds)
{
    sb_printf(b,
        /* the shore's foam: a band where the water meets anything. Two tests: the bed's depth under the pixel reaching nothing (a
         * beach, the foot of a wall), and the scene's depth around the pixel holding something nearer than the surface (a rock, a
         * pier, a bank rising out of the water: the edge), read at four points 0.35 yalms out and four 0.9 out. A dense rim at
         * either, and streaks of foam over three widths past the bed's, a finer scrolling noise read through a soft ramp that opens
         * toward the rim and surges with the swell's time (its two slow waves beat against each other over the 600 s, seamless
         * at the wrap); the foam is never a flat white, the noise breaks it. wq.y the shore width (at least 0.8 for the band),
         * wq.z the foam's strength */
        "  float fw = max(shore, 0.8);\n"
        /* (none by the bed's depth where that is the vertex's, vown: a sheet's or a pond's, whose depths step from triangle to
         * triangle, and the steps were lines of foam) */
        "  float frim = (1.0 - smoothstep(0.0, fw * 0.6, D)) * (1.0 - vown);\n"
        "  float freach = (1.0 - smoothstep(fw * 0.3, fw * 3.0, D)) * (1.0 - vown);\n"
        "  float fedge = 0.0;\n"
        "  { float2 pxn = float2(0.35, 0.35) / max(fp, 1e-4) / wsz, pxf = float2(0.9, 0.9) / max(fp, 1e-4) / wsz;\n"
        /* (nearer, but only by up to a tenth of the distance, at least 2 yalms: what rises out of the water there. The ferry's rail
         * and the monsters on its deck, a few yalms off in front of the sea 300 out, were outlined in foam, 2026-11-21) */
        "    float zs = pe.z * 0.985, zm = max(pe.z - max(2.0, pe.z * 0.1), 0.0);\n"
        "    float2 on[4] = { float2(pxn.x, 0.0), float2(-pxn.x, 0.0), float2(0.0, pxn.y), float2(0.0, -pxn.y) };\n"
        "    float2 of[4] = { float2(pxf.x, pxf.y), float2(-pxf.x, pxf.y), float2(pxf.x, -pxf.y), float2(-pxf.x, -pxf.y) };\n"
        "    [unroll] for (int ei = 0; ei < 4; ++ei) {\n"
        "      float zn = water_viewz(%s.SampleLevel(%s, clamp(uv0 + on[ei], uvlo, uvhi), 0).r, zp, ws.x, pe.z);\n"
        "      float zf = water_viewz(%s.SampleLevel(%s, clamp(uv0 + of[ei], uvlo, uvhi), 0).r, zp, ws.x, pe.z);\n"
        "      fedge = max(fedge, (zn > zm && zn < zs) ? 1.0 : 0.0);\n"
        "      fedge = max(fedge, (zf > zm && zf < zs) ? 0.4 : 0.0); } }\n"
        "  float fsurge = 0.5 + 0.5 * sin(t * 0.209440 + D * 0.9) * sin(t * 0.146608 + p.x * 0.05 + p.z * 0.07);\n"
        "  float2 xf = p.xz / (0.8 * max(q3.w, 0.25)) + float2(0.32 * t, -0.213333 * t);\n"
        "  float3 nf = water_noise(xf), nf2 = water_noise(xf * 2.7 + 3.3);\n"
        "  float ffine = saturate(0.8 / (6.0 * fp) - 1.0);\n"
        "  float foam_s = nf.x * 0.6 + nf2.x * 0.4 * ffine + (1.0 - ffine) * 0.2;\n"
        /* the streaks past the rim only where the bed is rising toward it (the depth's slope across the screen): a shallow basin
         * a yalm deep everywhere (Bastok Markets' fountain) was foam all over */
        "  float fslope = smoothstep(0.03, 0.2, length(float2(ddx(D), ddy(D))) / max(fp, 1e-4));\n"
        /* the dense rim only where the bed reaches the surface (a tenth of the band) or rises toward it: a flat shelf a hand under
         * the surface (a stream's bank shelves, West Ronfaure's) was a pale flat patch of foam with the shelf's straight edges */
        "  frim *= max(fslope, 1.0 - smoothstep(fw * 0.08, fw * 0.25, D));\n"
        "  float fdense = max(frim, fedge);\n"
        "  float fstreak = max(freach * max(fslope, frim), fedge * 0.7) * smoothstep(0.55 - 0.35 * fdense - 0.15 * fsurge, 0.85 - 0.2 * fdense, foam_s);\n"
        /* (none on a ferry's sea, matrix 5's byte 1 at 3: d3d8_water_sea.h; its whitecaps stay) */
        "  float fl = wq.z * saturate(fdense * (0.2 + 0.8 * smoothstep(0.42, 0.78, foam_s)) + fstreak) * (u.texm[5][1][0] > 2.5 ? 0.0 : 1.0);\n"
        /* the foam as bubbles: Worley cells at two scales (0.32 and 0.11 yalms), bright rims and clearer middles, popping and
         * reforming with time; thin foam breaks into separate bubbles (the cells' middles alone), dense foam (against the bank,
         * under a fall's foot) is whole, with a darkening under it so it sits on the water. The froth at a fall's foot joins it */
        "  float fn = foot > 0.0 ? smoothstep(0.32, 0.72, foot_n) * 0.75 + 0.25 : 0.0;\n"
        "  float fa = saturate(fl * 0.9 + foot * fn * wq.z * 0.9);\n"
        "  if (fa > 0.003) {\n"
        "    float fA = saturate(0.32 / (6.0 * fp) - 1.0), fB = saturate(0.11 / (6.0 * fp) - 1.0);\n"
        "    float2 cA = water_cells(p.xz / 0.32, t * 0.9), cB = water_cells(p.xz / 0.11 + 3.1, t * 1.3);\n"
        /* the rims soft (a crisp net of white lines on the stream's bank was the first cut) and the two scales cross-faded */
        "    float rimA = (1.0 - smoothstep(0.0, 0.34, cA.y - cA.x)) * fA, rimB = (1.0 - smoothstep(0.0, 0.4, cB.y - cB.x)) * fB;\n"
        "    float rim = max(rimA * rimA, rimB * rimB * 0.7);\n"
        "    float bubA = lerp(1.0, 1.0 - smoothstep(0.25, 0.55, cA.x), fA);\n"
        "    float dense = smoothstep(0.25, 0.8, fa);\n"
        "    float body = lerp(bubA, 1.0, dense);\n"
        "    float bright = 0.6 + 0.4 * rim;\n"
        "    float fcov = fa * body * (0.78 + 0.22 * rim);\n"
        "    col.rgb *= 1.0 - 0.18 * dense * fa;\n"
        "    col.rgb = lerp(col.rgb, saturate(Lc * (0.7 + 0.3 * bright) + 0.08), saturate(fcov)); }\n"
        "",
        dt, ds, dt, ds); /* the edge test's depth samples */
}

/* emit_water_depth's part for the ocean (water 4): its crests' glow and whitecaps over `col` */
static void emit_water_whitecaps(Sb* b)
{
    sb_printf(b,
        /* the ocean's crests: the light through the thin water at their tops, seen against the sun (a green-blue glow), and
         * the whitecaps where the FFT's crests fold (ocean_foam). The FFT's foam is soft (64 texels a tile): it is the foam's
         * cover, not its shape. The shape is a network of Worley cell borders at two scales (1.4 and 0.45 yalms times the size
         * setting's root, bent by the waves' slopes so it follows them, drifting slowly), thresholded by the cover: thick cover
         * fills the cells (solid white at a breaking crest), thin cover leaves only the borders (the lace a whitecap leaves as
         * it fades), none leaves nothing. The step is a pixel wide (fwidth): sharp edges at any distance; past where the cells
         * shrink under a few pixels the network fades to its mean, the cover alone */
        "  { float3 sssc = float3(0.03, 0.16, 0.13) * (0.3 + 0.7 * day) * q2.x;\n"
        "    float sss = saturate(ocean_h) * (1.0 - F) * (0.5 + 0.5 * saturate(1.0 - dot(N, up) * 1.4)) * edge;\n"
        "    col.rgb += sssc * sss * 1.6;\n"
        "    float fz = sqrt(max(q1.y, 0.25));\n"
        "    float2 xo = (p.xz + g * 2.0) / fz;\n"
        "    float2 ca = water_cells(xo / 1.4 + float2(0.031 * t, 0.019 * t), t * 0.12);\n"
        "    float2 cb = water_cells(xo / 0.45 + float2(9.1 - 0.05 * t, 3.3 + 0.04 * t), t * 0.25);\n"
        "    float fA = saturate(1.4 * fz / (5.0 * fp) - 1.0), fB = saturate(0.45 * fz / (5.0 * fp) - 1.0);\n"
        "    float la = lerp(0.5, 1.0 - saturate((ca.y - ca.x) / 0.6), fA);\n"
        "    float lb = lerp(0.5, 1.0 - saturate((cb.y - cb.x) / 0.6), fB);\n"
        "    float cov = saturate(ocean_foam * 1.5);\n"
        "    float fv = (la * 0.62 + lb * 0.38) + cov - 1.0;\n"
        "    float fsharp = saturate(fv / max(fwidth(fv), 1e-4) + 0.5) * smoothstep(0.02, 0.12, cov);\n"
        "    float fhalo = smoothstep(-0.22, 0.0, fv) * cov * 0.35;\n"
        "    float wcap = lerp(cov * 0.55, max(fsharp, fhalo), fA) * saturate(wq.z * 0.4) * edge;\n"
        "    float3 fcol = saturate(Lc * 0.92 + 0.06) * lerp(1.0, 0.86 + 0.14 * lb, fB);\n"
        "    col.rgb = lerp(col.rgb, fcol, saturate(wcap * 0.95)); }\n");
}

/* emit_water_depth's end for the ground's water (water 3, 4): its alpha. The sea's horizon sheet (matrix 5's byte 1, d3d8_water.h
 * water_horizon_pass; its colour through the fog as the open water's above) fades in from its inner edge over the first ring
 * (the bed's depth 0 there, SEA_DEPTH at the ring's far side): the water's own shore fade is done within a yalm, a line from
 * above. dt and ds the depth copy and its sampler */
static void emit_water_ground_fade(Sb* b, const char* dt, const char* ds)
{
    sb_printf(b,
        "  float seaf = u.texm[5][1][0];\n"
        "  if (seaf > 0.5 && seaf < 1.5) col.a *= smoothstep(0.0, 24.0, Dv);\n" /* byte 1: column_major, row 1 of column 0 */
        /* the ground's water fades out where its bed reaches the surface (the collision's depth at the vertex), so a water
         * triangle ending on the bank shows no edge of its own; the foam's rim lies just past the fade */
        /* (the whole sea, byte 1 at 2: by the depth read from the scene under it, its own vertices all deep) */
        "  col.a *= smoothstep(0.0, max(shore * 0.5, 0.15), seaf > 1.5 ? D : Dv);\n"
        /* a sheet's holes' cell goes where the ground drawn under it (the depth under the pixel itself) lies at the level: over
         * ground painted as the water (Caedarva Mire's pond) the cells were squares of foam. The clip writes no depth there, so
         * a neighbouring sheet's water under the cell is drawn */
        "  if (vfill > 0.5 && hs < 0.0) {\n"
        "    float rq = water_viewz(%s.SampleLevel(%s, uv0, 0).r, zp, ws.x, pe.z) / pe.z;\n"
        "    col.a *= smoothstep(0.08, 0.25, rq > 1.0 ? hs * (1.0 - rq) : 0.0); }\n"
        "  clip(col.a - 0.01);\n",
        dt, ds);
}

/* the pass (water 2): what's under the surface from the scene's copies, by the depth under each pixel; the shore. The ground's
 * water (water 3): the bed's depth from the vertex (the first texture coordinate's x), the rest the same */
static void emit_water_depth(Sb* b, const GfxVsKey* vk, int ground, int fog, int ocean)
{
    char ct[32], cs[32], dt[32], ds[32];
    water_tex(ct, cs, sizeof ct, 6);
    water_tex(dt, ds, sizeof dt, 7);
    sb_printf(b,
        "  float2 wsz = ws.yz;\n"
        "  float2 uv0 = vin.pos.xy / wsz;\n"
        "  float2 uvlo = (u.vp.xy + 1.0) / wsz, uvhi = (u.vp.xy + u.vp.zw - 1.0) / wsz;\n"
        "  float hs = dot(up, pe);\n"); /* the surface's height over the camera: below it, negative */
    if (ground)
        sb_printf(b,
            /* the vertex's depth (d3d8_water.h BAKE_STRIDE), negated for a chunk's own water where the chunk writes depth: the
             * scene's depth under that is the chunk's own, 3 cm down, and read as the bed it was foam from bank to bank (Caedarva
             * Mire's pond, West Ronfaure's pale triangle). BAKE_FILL_TAG over it for a sheet's holes' cells */
            "  float tv = %s;\n"
            "  float vown = tv < 0.0 ? 1.0 : 0.0, vfill = tv > 500.0 ? 1.0 : 0.0;\n"
            "  float Dv = max(tv < 0.0 ? -tv : tv - 1000.0 * vfill, 0.0);\n"
            "  float r0 = 1.0;\n"
            "  float D0 = Dv;\n",
            vk->ntex ? "vin.t0.x" : "0.0");
    else
        sb_printf(b,
            "  float vown = 0.0;\n" /* (the ground's water's flag, emit_water_foam) */
            "  float z0 = water_viewz(%s.SampleLevel(%s, uv0, 0).r, zp, ws.x, pe.z);\n"
            "  float r0 = max(z0 / pe.z, 1.0);\n"
            "  float D0 = max(hs * (1.0 - r0), 0.0);\n",
            dt, ds);
    sb_printf(b,
        /* the view bent by the waves: the normal's lean, on the screen, less far off and in the shallows (nothing is pulled
         * across the shore), and never to a point above the surface (one nearer than it: the first sample's then) */
        "  float3 dn = N - up;\n"
        "  float2 off = float2(dn.x, -dn.y) * (0.05 * wq.x * min(1.0, 8.0 / dist) * saturate(D0 * 2.0) * (1.0 + 1.5 * q_rel * exp(-D0 * 0.5)));\n"
        "  float2 uv1 = clamp(uv0 + off, uvlo, uvhi);\n"
        "  float r1 = water_viewz(%s.SampleLevel(%s, uv1, 0).r, zp, ws.x, pe.z) / pe.z;\n"
        "  if (r1 < 1.0) { uv1 = uv0; r1 = r0; }\n"
        "  float3 sc = %s.SampleLevel(%s, uv1, 0).rgb;\n"
        "  float3 sc_raw = sc;\n",
        dt, ds, ct, cs);
    /* the scene's colour is fogged already, and the tail fogs the water again (emit_fs_tail, by the surface's distance): what
     * shows through is taken back out of the fog by that factor first, so it comes out of the tail as it was */
    if (fog == 4)
        sb_printf(b, "  float ff = saturate(vin.fog);\n");
    else if (fog)
    {
        sb_printf(b, "  float ff;\n  float ffd = abs(vin.ez);\n");
        emit_fog_factor(b, "ff", fog, "ffd");
    }
    /* ffx: the tail's own fog factor (emit_fs_tail works it out the same way), before the floor below; 1 unfogged */
    if (fog)
        sb_printf(b, "  float ffx = ff;\n  ff = max(ff, 0.02);\n  sc = (sc - u.fogcolor.rgb * (1.0 - ff)) / ff;\n");
    else
        sb_printf(b, "  float ffx = 1.0;\n");
    emit_water_caustics(b, ground);
    if (ground)
        sb_printf(b,
            /* how deep the bed is under the surface: the scene's depth under the pixel where it is sane (the ground under the
             * water, which the pass drew first; per pixel, so a pool's bed reads smooth), else the collision's per vertex (facets
             * over a big triangle: Yuhtunga's pool showed its polygon edges) */
            "  float Dp = max(hs * (1.0 - r1), 0.0);\n"
            "  float D = (vown < 0.5 && r1 > 1.0 && Dp < 24.0) ? Dp : Dv;\n" /* (a sane depth: not the far plane; the test no longer
                                                                  * leans on Dv, which differs chunk to chunk and drew a straight seam mid-pool) */
            /* the whole sea (matrix 5's byte 1 at 2): the bed read from the scene only near the shore (its fade and foam), the open
             * sea its own deep past 2 to 6 yalms of it. The game draws the harbour's bed meshes by the camera's distance (Lower
             * Jeuno: zoomed in they're drawn, out they're not), and the sea read them: bright teal over a bed some yalms down,
             * dark once they went */
            "  if (u.texm[5][1][0] > 1.5) D = lerp(D, Dv, smoothstep(2.0, 6.0, D));\n"
            "  if (u.texm[5][1][0] > 2.5) D = Dv;\n" /* a ferry's sea (byte 1 at 3): deep against the hull too, no shore */
            /* (not the horizon's sheet, matrix 5's byte 1: shaded as the tiles it carries on from, or it was a lighter second sea) */
            "  F = max(F, u.texm[5][1][0] > 0.5 ? 0.0 : 0.35 * q1.z);\n" /* the sky in it at least a third (times the reflection setting): what the game
                                                       * paints under a stream is a dark bed meant to be seen through its own bright
                                                       * shimmer sheet, which the water covers; seen through clear water alone the
                                                       * stream was dark navy at noon */
            "  float L = 0.7 * D / max(cv, 0.3);\n"); /* the light's way through the water to it, along the view: shorter than the
                                                          * pass's, so the game's own painting of the stream (its bed, in these zones) shows
                                                          * through, as the game's stream is clear; at 1.6 a stream was dark navy at noon */
    else
        sb_printf(b,
            "  float D = max(hs * (1.0 - r1), 0.0);\n"   /* how deep the bed is under the surface there */
            "  float L = dist * (r1 - 1.0);\n");       /* the light's way through the water to the bed. (The floors once here for
                                                            * Rabao's pond, a depth of 0.3, the way at least the depth and the sky at
                                                            * least a third, made a town's basin opaque navy; Rabao's pond is the game's
                                                            * own draw now) */
    sb_printf(b,
        "  float clar = ws.w;\n"
        "  float dm = 1.0 - exp(-D * 2.0 / (clar * max(q2.z, 0.05)));\n"
        "  float3 bodyc = lerp(shallow, deep, dm);\n"
        /* open water (no bed within reach: the sea past the shallows, the horizon's sheet) is the water's own deep colour with the
         * sky in it at most a little over half: at the grazing angles the open sea is seen at, the mirrored horizon (the fog's colour)
         * took it over. (Its body once took half of what was drawn behind it, meant for the dome's painted sea; behind Lower Jeuno's
         * sea is VanaCore's sky or nothing, and the sea came out a flat grey from above) */
        "  float wopen = smoothstep(8.0, 30.0, D);\n"
        "  F *= 1.0 - 0.45 * wopen;\n"
        /* what shows through is at most 85% of the bed: the water's own colour is in it even a hand deep (the game's basin in
         * Bastok Markets is a clear green over its mosaic) */
        "  float3 tr = exp(-L * float3(1.5, 1.0, 0.8) / clar) * 0.85;\n"
        "  float3 under = sc * tr + bodyc * (1.0 - tr);\n"
        "  float shore = max(wq.y, 0.02);\n"
        "  float edge = smoothstep(0.0, shore * 0.5, D);\n"
        "  F *= edge;\n"
        "  spc *= edge;\n"
        /* the sun's glint off the churned surface came out as a grid of hard-edged bright cells (the glint's high power on the
         * noise's slopes, which flip sign each half cell): under a waterfall's foot the glint stands down and the froth is the light */
        "  spc *= 1.0 - 0.9 * foot;\n"
        "  float4 col = float4(saturate(lerp(under, sky, F) + spc * (1.5 * q1.w)), 1.0);\n"
        /* the relief at a third on open water: from high above, the fine ripples lit as slopes over a deep, even colour were a
         * speckle across the whole sea */
        "  float q_relo = q_rel * (1.0 - 0.67 * wopen);\n"
        /* (the ocean's own slopes are its relief: water.relief 3 over them shaded the swells 0.6 to 1.8, dark and light blotches) */
        "%s"
        /* (the shading held within 0.6 to 1.8: at water.relief 3 the troughs went to black, stripes of black and lit crests) */
        "  col.rgb = saturate(col.rgb * clamp(1.0 + 2.5 * q_relo * rel, 0.6, 1.8) + spcb * (0.22 * q_relo * q1.w) * edge);\n"
        /* the camera under the surface (the third-person camera dipping into a stream; hs is the surface's height over it): a
         * translucent sheet, as the game's is, over the scene as drawn, with no sky in it */
        "  if (hs > 0.0) col.rgb = lerp(sc_raw, shallow * 1.4, 0.45);\n"
        "",
        ocean ? "  q_relo = min(q_relo, 0.3);\n" : "");
    emit_water_foam(b, dt, ds);
    if (ocean)
        emit_water_whitecaps(b);
    sb_printf(b,
        /* the probe ([graphics] water.probe 1 to 3, matrix 5's byte 0): the scene's colour as read, that out of the fog, or
         * how much of it shows through, in place of the water (the tail's fog still falls on it) */
        "  float probe = u.texm[5][0][0];\n"
        "  if (probe > 0.5) col.rgb = probe < 1.5 ? sc_raw : probe < 2.5 ? sc : tr;\n"
        /* open water keeps at least 40% of its own colour through the fog (fe), where the fog took VanaCore's sea and its horizon
         * sheet to the overcast sky's pale grey and the game's sea stays its colour to the horizon. The colour is set so the tail's
         * fog (fogcolor + (col - fogcolor) * ffx) gives that; over the fog's last 2% (ffx under 0.02) it goes to the fog's colour,
         * as the land does. Nothing is read from behind the water (that was VanaCore's sky, or nothing, under Lower Jeuno's sea) */
        "  if (probe < 0.5) {\n"
        "    float3 fwant = lerp(u.fogcolor.rgb, col.rgb, max(ffx, 0.4 * wopen));\n"
        "    col.rgb = u.fogcolor.rgb + (fwant - u.fogcolor.rgb) / max(ffx, 0.02); }\n",
        "");
    if (ground)
        emit_water_ground_fade(b, dt, ds);
}

/* the pixel function's body for a draw made VanaCore's water (GfxFsKey.water): 1 in its place, 2 the water pass, 3 the
 * ground's water in the pass; fog the draw's pixel fog mode (GfxFsKey.fog) */
static void emit_water_fs(Sb* b, const GfxVsKey* vk, int water, int fog)
{
    emit_water_surface(b, water);
    emit_water_light(b, vk);
    if (water >= 2)
        emit_water_depth(b, vk, water >= 3, fog, water == 4);
    else
        sb_printf(b,
            "  float3 body = lerp(deep, shallow, saturate(q2.z * (0.1 + 0.7 * pow(1.0 - cv, 1.5))));\n"
            "  float4 col = float4(saturate(lerp(body, sky, F) + spc * (1.5 * q1.w)), 1.0);\n");
}
