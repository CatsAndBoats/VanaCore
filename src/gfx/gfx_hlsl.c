/* HLSL (Shader Model 5.1) for a draw's keys (gfx.h): the Direct3D 12 back end's counterpart of
 * gfx_msl.c, with the same fixed-function pipeline - transform, lighting, fog, texture coordinate
 * generation and transforms, the texture stage cascade, alpha test - as one vertex and one pixel
 * function per key pair, and vs.1.x / ps.1.x shaders translated (gfx_hlsl_shaders.c). Pure C: the
 * D3D12 side (gfx_d3d12.c) compiles the text and caches the result by key.
 *
 * Conventions the functions follow (the root signature is gfx_d3d12.c's):
 *   - matrices are D3D's bytes in a column_major float4x4, so `mul(m, v)` is D3D's v * M;
 *   - D3D8 puts pixel centers on integer coordinates and D3D12 on half-integers: every clip-space
 *     position moves by half a pixel (the "position fixup"), as on Metal;
 *   - vertex fetch is by hand from the stream buffers (raw buffers t0..t3), stride and per-register
 *     offset in the uniforms, so one function serves any offsets and strides;
 *   - the uniforms are b0 in both stages; textures and samplers are indices into the descriptor
 *     heaps (b1), Texture2D in space1 and TextureCube in space2. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_hlsl.h"

_Static_assert(sizeof(GfxU) == 3536, "GfxU must match the HLSL struct U");

void sb_printf(Sb* b, const char* fmt, ...)
{
    va_list ap;
    for (;;)
    {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = b->cap ? vsnprintf(b->s + b->len, room, fmt, ap) : -1;
        va_end(ap);
        if (n >= 0 && (size_t)n < room)
        {
            b->len += (size_t)n;
            return;
        }
        b->cap = b->cap ? b->cap * 2 : 8192;
        if (n >= 0 && b->cap < b->len + (size_t)n + 1)
            b->cap = b->len + (size_t)n + 1;
        b->s = (char*)realloc(b->s, b->cap);
    }
}

#ifdef VANACORE
int gfx_hlsl_d3d11;

/* PRELUDE's texture and sampler declarations for Direct3D 11 (gfx_hlsl_d3d11): a register per stage */
static const char PRELUDE_D3D11_RESOURCES[] =
    "Texture2D tx2_0 : register(t4); Texture2D tx2_1 : register(t5); Texture2D tx2_2 : register(t6); Texture2D tx2_3 : register(t7);\n"
    "Texture2D tx2_4 : register(t8); Texture2D tx2_5 : register(t9); Texture2D tx2_6 : register(t10); Texture2D tx2_7 : register(t11);\n"
    "TextureCube txc_0 : register(t12); TextureCube txc_1 : register(t13); TextureCube txc_2 : register(t14); TextureCube txc_3 : register(t15);\n"
    "TextureCube txc_4 : register(t16); TextureCube txc_5 : register(t17); TextureCube txc_6 : register(t18); TextureCube txc_7 : register(t19);\n"
    "SamplerState smp_0 : register(s0); SamplerState smp_1 : register(s1); SamplerState smp_2 : register(s2); SamplerState smp_3 : register(s3);\n"
    "SamplerState smp_4 : register(s4); SamplerState smp_5 : register(s5); SamplerState smp_6 : register(s6); SamplerState smp_7 : register(s7);\n";
#endif

static const char PRELUDE[] =
    "#define INFINITY asfloat(0x7F800000u)\n"
    "struct Light { float4 diffuse, specular, ambient, pos, dir, att, spot; };\n"
    "struct U {\n"
    "  float4x4 wvp, wv, wvit;\n"
    "  float4x4 texm[8];\n"
    "  float4 mat_d, mat_a, mat_s, mat_e, params, ambient, tfactor, fogcolor, params2, vp;\n"
    "  int4 vofs, stride;\n"
    "  int4 offset[5];\n"
    "  Light light[8];\n"
    "  float4 vsc[96];\n"
    "  float4 psc[8];\n"
    "};\n"
    "cbuffer CU : register(b0) { U u; };\n"
    "cbuffer Bind : register(b1) { uint4 ti[2]; uint4 si[2]; };\n"
    "cbuffer Sun : register(b2) { float4x4 sun_m; float4 sun_f; };\n"
    "ByteAddressBuffer s0 : register(t0);\n"
    "ByteAddressBuffer s1 : register(t1);\n"
    "ByteAddressBuffer s2 : register(t2);\n"
    "ByteAddressBuffer s3 : register(t3);\n"
    "Texture2D tx2[] : register(t0, space1);\n"
    "TextureCube txc[] : register(t0, space2);\n"
    "SamplerState smp[] : register(s0);\n"
    "float4 ld_color(ByteAddressBuffer b, int a) { uint c = b.Load(a); return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }\n"
    "float4 ld_ubyte4(ByteAddressBuffer b, int a) { uint c = b.Load(a); return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24); }\n"
    "float4 ld_short2(ByteAddressBuffer b, int a) { int c = asint(b.Load(a)); return float4((c << 16) >> 16, c >> 16, 0, 1); }\n"
    "float4 ld_short4(ByteAddressBuffer b, int a) { int2 c = asint(b.Load2(a)); return float4((c.x << 16) >> 16, c.x >> 16, (c.y << 16) >> 16, c.y >> 16); }\n";

/* lit per pixel: the vertex function passes on what the lighting starts from (the normal, the
 * position and the material colors), and the pixel function lights - the same equations, but a
 * highlight no longer depends on where the vertices fall. pixel 1: the directional lights (the sun,
 * the moon) per pixel, the point and spot lights per vertex as the game made them - its torches
 * flicker, and a pool of light per pixel pulses with them; pixel 2: all of them per pixel.
 * (gfx_msl.c's) */
static int pixel_lit(const GfxVsKey* k) { return k->pixel && k->lighting && !k->rhw && !k->flat && !k->prog; }
static int point_per_vertex(const GfxVsKey* k)
{
    if (k->pixel != 1)
        return 0;
    for (int i = 0; i < k->nlights; ++i)
        if (k->light_type[i] != 3)
            return 1;
    return 0;
}

/* the vertex function's output: what the pixel function reads. water: a zone's water tile (GfxFsKey.water) also passes
 * on where the pixel stands in the world and in view space; pool (GfxFsKey.pool), where it stands in view space */
static void emit_vout(Sb* b, const GfxVsKey* k, int water, int pool)
{
    int ntex = k->ntex;
    const char* fl = k->flat ? "nointerpolation " : "";
    sb_printf(b, "struct VOut {\n  float4 pos : SV_Position;\n  %sfloat4 d : COLOR0;\n  %sfloat4 s : COLOR1;\n", fl, fl);
    if (pixel_lit(k))
        sb_printf(b, "  float3 n : LN;\n  float4 pe : LPE;\n  float4 md : LMD;\n  float4 ma : LMA;\n  float4 ms : LMS;\n  float4 me : LME;\n%s",
            point_per_vertex(k) ? "  float4 pa : LPA;\n  float4 pd : LPD;\n  float4 ps : LPS;\n" : "");
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  float4 t%d : TEXCOORD%d;\n", i, i);
    if (k->cpos)
        sb_printf(b, "  float4 cp : CLIPP;\n");
    if (k->cpos >= 2) /* the normal's clip-space direction (gfx_hlsl_shaders.c) */
        sb_printf(b, "  float4 cn : CLIPN;\n");
    if (k->motion && !(k->motion & GFX_MO_MASK)) /* VanaCore's motion pass: the clip-space position this frame (without DLSS's jitter) and the frame before */
        sb_printf(b, "  float4 mc : MOTC;\n  float4 mp : MOTP;\n");
    if (water)
        sb_printf(b, "  float3 wp : WATP;\n  float3 wpe : WATE;\n");
    if (pool)
        sb_printf(b, "  float3 pp : POOLP;\n");
    sb_printf(b, "  float fog : FOG;\n  float ez : EZ;\n};\n");
}

/* v# for an element: a float4 read from its stream (shared with gfx_hlsl_shaders.c's vs.1.x) */
void gfx_hlsl_fetch(Sb* b, const GfxVsKey* k, int reg);
void gfx_hlsl_fetch(Sb* b, const GfxVsKey* k, int reg)
{
    const GfxElem* e = &k->el[reg];
    if (!e->used)
    {
        sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg);
        return;
    }
    sb_printf(b, "  int ad%d = vi * u.stride[%d] + u.offset[%d][%d];\n", reg, e->stream, reg >> 2, reg & 3);
    int s = e->stream;
    switch (e->type)
    {
    case GFX_FLOAT1: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load(ad%d)), 0, 0, 1);\n", reg, s, reg); break;
    case GFX_FLOAT2: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load2(ad%d)), 0, 1);\n", reg, s, reg); break;
    case GFX_FLOAT3: sb_printf(b, "  float4 v%d = float4(asfloat(s%d.Load3(ad%d)), 1);\n", reg, s, reg); break;
    case GFX_FLOAT4: sb_printf(b, "  float4 v%d = asfloat(s%d.Load4(ad%d));\n", reg, s, reg); break;
    case GFX_D3DCOLOR: sb_printf(b, "  float4 v%d = ld_color(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_UBYTE4: sb_printf(b, "  float4 v%d = ld_ubyte4(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_SHORT2: sb_printf(b, "  float4 v%d = ld_short2(s%d, ad%d);\n", reg, s, reg); break;
    case GFX_SHORT4: sb_printf(b, "  float4 v%d = ld_short4(s%d, ad%d);\n", reg, s, reg); break;
    default: sb_printf(b, "  float4 v%d = float4(0, 0, 0, 1);\n", reg); break;
    }
}

static int elem_components(uint8_t type)
{
    switch (type)
    {
    case GFX_FLOAT1: return 1;
    case GFX_FLOAT2:
    case GFX_SHORT2: return 2;
    case GFX_FLOAT3: return 3;
    default: return 4;
    }
}

static void emit_vs_signature(Sb* b, const GfxVsKey* k)
{
    if (!k->inst)
    {
        sb_printf(b, "VOut vs_main(uint vid : SV_VertexID) {\n  VOut o = (VOut)0;\n  int vi = int(vid) + u.vofs.x;\n");
        return;
    }
    /* drawn for many pieces at once (GfxVsKey.inst): this piece's transforms from the stream the back end put them in
     * (GfxInst, GFX_INST_BYTES each), a column at a time as the uniforms keep them (column_major), in place of u's */
    sb_printf(b, "VOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n  VOut o = (VOut)0;\n"
                 "  int vi = int(vid) + u.vofs.x;\n  uint ib = iid * %du;\n", GFX_INST_BYTES);
    static const char* const NAME[3] = { "iwvp", "iwv", "iwvit" };
    for (int m = 0; m < 3; ++m)
        sb_printf(b,
            "  float4x4 %s = transpose(float4x4(asfloat(s%d.Load4(ib + %d)), asfloat(s%d.Load4(ib + %d)), "
            "asfloat(s%d.Load4(ib + %d)), asfloat(s%d.Load4(ib + %d))));\n",
            NAME[m], k->inst - 1, m * 64, k->inst - 1, m * 64 + 16, k->inst - 1, m * 64 + 32, k->inst - 1, m * 64 + 48);
}

/* the transforms the vertex function reads: the uniforms', or this piece's when drawn for many (emit_vs_signature) */
static const char* vs_wvp(const GfxVsKey* k) { return k->inst ? "iwvp" : "u.wvp"; }
static const char* vs_wv(const GfxVsKey* k) { return k->inst ? "iwv" : "u.wv"; }
static const char* vs_wvit(const GfxVsKey* k) { return k->inst ? "iwvit" : "u.wvit"; }

/* drawn into the sun's maps (GfxVsKey.shadow 1, 2): depth alone, no colour. Not the water's mirror (GFX_VS_MIRROR), whose
 * position goes through the same matrix and whose colour is the draw's own */
static int depth_only(const GfxVsKey* k) { return k->shadow && k->shadow != GFX_VS_MIRROR; }

/* drawn from the sun: whatever clip-space position the function makes (the camera's, from the
 * transforms or a vertex shader's constants) goes on through the camera's inverse into the sun's
 * view - one matrix, so any draw of the scene can be drawn again into the shadow map. The pixel
 * centers' fixup is undone first: it is the camera's viewport's, not the map's. */
void gfx_hlsl_vs_return(Sb* b, const GfxVsKey* k)
{
    if (k->shadow)
        sb_printf(b, "  o.pos.x -= o.pos.w / u.vp.z;\n  o.pos.y += o.pos.w / u.vp.w;\n  o.pos = mul(sun_m, o.pos);\n");
    /* VanaCore's DLSS: the scene's draws moved by this frame's sub-pixel step (gfx_dlss_jitter, in vofs.yz;
     * 0 for every other draw) - not into the sun's map. A decal (D3DRS_ZBIAS, d3d8.c) is given the depth of a
     * point vofs.w nearer the camera: the projection's z is w less the near plane (FFXI's far plane is too far
     * to count), so the near plane is w - z, and depth (1 - near / w) moves near * vofs.w / w^2. */
    else
    {
        sb_printf(b, "  o.pos.xy += float2(asfloat(u.vofs.y), asfloat(u.vofs.z)) * o.pos.w;\n");
        sb_printf(b, "  if (u.vofs.w != 0 && o.pos.w > 0.0) o.pos.z -= asfloat(u.vofs.w) * (o.pos.w - o.pos.z) / o.pos.w;\n");
    }
    sb_printf(b, "  return o;\n}\n");
}

/* clip-space position fixup: D3D8's pixel centers onto D3D12's */
static void emit_fixup(Sb* b)
{
    sb_printf(b, "  o.pos.x += o.pos.w / u.vp.z;\n  o.pos.y -= o.pos.w / u.vp.w;\n");
}

/* a D3DMATERIALCOLORSOURCE: the vertex color when the vertex has it, else the material's */
static const char* mcs(const GfxVsKey* k, int src, const char* mat)
{
    return src == 1 && k->el[GFX_R_DIFFUSE].used ? "v5" : src == 2 && k->el[GFX_R_SPECULAR].used ? "v6" : mat;
}

static void emit_fog_factor(Sb* b, const char* dst, int mode, const char* dist)
{
    switch (mode)
    {
    case 1: sb_printf(b, "  %s = saturate(exp(-u.params2.x * %s));\n", dst, dist); break;
    case 2: sb_printf(b, "  %s = saturate(exp(-(u.params2.x * %s) * (u.params2.x * %s)));\n", dst, dist, dist); break;
    case 3: sb_printf(b, "  %s = saturate((u.params.w - %s) / (u.params.w - u.params.z));\n", dst, dist); break;
    default: sb_printf(b, "  %s = 1.0;\n", dst); break;
    }
}

/* D3D's lighting from N and pe (view space) and the material colors cd, ca, cs, ce in scope, into
 * lit_d and lit_s. which: 0 every light; 1 the directional ones, with the rest's from the vertex function (vin.pa, ps:
 * everything but the directional lights, lit and clamped per vertex as D3D clamps it - so the game's torches light as the
 * game drew them: unclamped and spread across a triangle, a vertex by a torch lit far past white lit the whole face, and
 * stepped as the game swapped the torches it gives each draw while the camera moved; from XI on Anything,
 * rubymatrix/xi-on-anything 98ebf00); 2 the rest alone, leaving amb, dif and spc for it to pass on */
static void emit_lighting(Sb* b, const GfxVsKey* k, int pixel, int which)
{
    if (which == 2)
        sb_printf(b, "  float3 amb = float3(0, 0, 0), dif = float3(0, 0, 0), spc = float3(0, 0, 0);\n");
    else if (which == 1)
        sb_printf(b, "  float3 amb = float3(0, 0, 0), dif = float3(0, 0, 0), spc = float3(0, 0, 0);\n");
    else
        sb_printf(b, "  float3 amb = u.ambient.rgb, dif = float3(0, 0, 0), spc = float3(0, 0, 0);\n");
    if (k->specular)
        sb_printf(b, "  float3 V = %s;\n", k->localviewer ? "normalize(-pe)" : "float3(0, 0, -1)");
    for (int i = 0; i < k->nlights; ++i)
    {
        int t = k->light_type[i];
        if ((which == 1 && t != 3) || (which == 2 && t == 3))
            continue;
        sb_printf(b, "  {\n    Light L = u.light[%d];\n", i);
        if (t == 3)
            sb_printf(b, "    float3 l = L.dir.xyz;\n    float a = 1.0;\n");
        else
        {
            sb_printf(b,
                "    float3 lv = L.pos.xyz - pe;\n    float d = length(lv);\n    float3 l = lv / max(d, 1e-20);\n"
                "    float a = d > L.pos.w ? 0.0 : 1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20);\n");
            /* per pixel, D3D's hard edge at the range is a ring, and its attenuation near the light (far
             * over 1, where no vertex ever was) a white spot - both flash as the game's lights flicker
             * and move: the last quarter of the range fades, and the light is at most its colour */
            if (pixel)
                sb_printf(b, "    a = min(a, 1.0) * saturate((L.pos.w - d) / max(0.25 * L.pos.w, 1e-6));\n");
            if (t == 2)
                sb_printf(b,
                    "    float rho = dot(-l, L.dir.xyz);\n"
                    "    a *= rho > L.spot.x ? 1.0 : rho <= L.spot.y ? 0.0 : pow(saturate((rho - L.spot.y) / (L.spot.x - L.spot.y)), L.dir.w);\n");
        }
        sb_printf(b, "    amb += L.ambient.rgb * a;\n    float ndl = max(dot(N, l), 0.0);\n    dif += L.diffuse.rgb * (ndl * a);\n");
        if (k->specular)
            sb_printf(b, "    if (ndl > 0.0) spc += L.specular.rgb * (pow(max(dot(N, normalize(V + l)), 0.0), u.params.x) * a);\n");
        sb_printf(b, "  }\n");
    }
    if (which == 1)
        sb_printf(b,
            "  float4 lit_d = saturate(float4(vin.pa.rgb + ca.rgb * amb + cd.rgb * dif, cd.a));\n"
            "  float4 lit_s = saturate(float4(vin.ps.rgb + cs.rgb * spc, cs.a));\n");
    else if (which == 0)
        sb_printf(b,
            "  float4 lit_d = saturate(float4(ce.rgb + ca.rgb * amb + cd.rgb * dif, cd.a));\n"
            "  float4 lit_s = saturate(float4(cs.rgb * spc, cs.a));\n");
}

static void emit_ff_vs(Sb* b, const GfxVsKey* k, int water, int pool)
{
    emit_vs_signature(b, k);
    for (int r = 0; r < GFX_NREGS; ++r) /* unused registers are constants the compiler drops */
        gfx_hlsl_fetch(b, k, r);
    if (k->rhw)
    {
        sb_printf(b,
            "  float w = v0.w != 0.0 ? 1.0 / v0.w : 1.0;\n"
            "  float2 ndc = float2((v0.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (v0.y - u.vp.y) / u.vp.w * 2.0);\n"
            "  o.pos = float4(ndc * w, v0.z * w, w);\n"
            "  float3 pe = float3(0, 0, w);\n"
            "  o.ez = w;\n");
    }
    else
    {
        sb_printf(b,
            "  float4 P = float4(v0.xyz, 1.0);\n"
            "  o.pos = mul(%s, P);\n"
            "  float3 pe = mul(%s, P).xyz;\n"
            "  o.ez = pe.z;\n",
            vs_wvp(k), vs_wv(k));
        /* VanaCore's motion pass (gfx_d3d12.c mo_draw): where the vertex was the frame before, through that frame's transform
         * (in the Sun buffer, which only the sun's maps read otherwise): from this frame's position (GFX_MO_SAME), or the
         * frame before's, float3 each in a stream of its own */
        if (k->motion && !(k->motion & GFX_MO_MASK))
        {
            const int src = k->motion & GFX_MO_SRC;
            sb_printf(b, "  o.mc = o.pos;\n");
            if (src >= 1 && src <= GFX_NSTREAMS)
                sb_printf(b, "  o.mp = mul(sun_m, float4(asfloat(s%d.Load3(vi * 12)), 1.0));\n", src - 1);
            else
                sb_printf(b, "  o.mp = mul(sun_m, P);\n");
        }
        /* the water's world position: through the draw's world matrix, which rides in texture matrix 3 (d3d8_water.h water_shade;
         * no stage of the water's uses it), so a turned or scaled model's water takes the waves' coordinates from the world as a
         * tile does; and where it stands in view space */
        if (water)
            sb_printf(b, "  o.wp = mul(u.texm[3], P).xyz; o.wpe = pe;\n");
        if (pool)
            sb_printf(b, "  o.pp = pe;\n");
    }
    if (!(k->rhw && k->centered))
        emit_fixup(b);
    int need_normal = k->lighting || 0;
    for (int i = 0; i < k->ntex; ++i)
        need_normal |= (k->tci[i] >> 4) == 1 || (k->tci[i] >> 4) == 3;
    if (need_normal && !k->rhw)
    {
        sb_printf(b, "  float3 N = mul(%s, float4(v3.xyz, 0.0)).xyz;\n", vs_wvit(k));
        if (k->normalize)
            sb_printf(b, "  N = normalize(N);\n");
    }
    else
        sb_printf(b, "  float3 N = float3(0, 0, 1);\n");

    const char* dif_in = k->el[GFX_R_DIFFUSE].used ? "v5" : "float4(1, 1, 1, 1)";
    const char* spe_in = k->el[GFX_R_SPECULAR].used ? "v6" : "float4(0, 0, 0, 0)";
    if (k->lighting && !k->rhw)
    {
        sb_printf(b, "  float4 cd = %s, ca = %s, cs = %s, ce = %s;\n", mcs(k, k->src_diffuse, "u.mat_d"),
            mcs(k, k->src_ambient, "u.mat_a"), mcs(k, k->src_specular, "u.mat_s"), mcs(k, k->src_emissive, "u.mat_e"));
        if (pixel_lit(k)) /* the length of N too: without NORMALIZENORMALS a scaled one lights more */
        {
            sb_printf(b, "  o.n = N;\n  o.pe = float4(pe, length(N));\n  o.md = cd; o.ma = ca; o.ms = cs; o.me = ce;\n"
                         "  o.d = cd;\n  o.s = cs;\n");
            if (point_per_vertex(k))
            {
                sb_printf(b, "  {\n");
                emit_lighting(b, k, 0, 2);
                sb_printf(b, "  o.pa = float4(saturate(ce.rgb + ca.rgb * (u.ambient.rgb + amb) + cd.rgb * dif), 0);\n"
                             "  o.pd = float4(0, 0, 0, 0); o.ps = float4(saturate(cs.rgb * spc), 0);\n  }\n");
            }
        }
        else
        {
            emit_lighting(b, k, 0, 0);
            sb_printf(b, "  o.d = lit_d;\n  o.s = lit_s;\n");
        }
    }
    else
        sb_printf(b, "  o.d = %s;\n  o.s = %s;\n", dif_in, spe_in);

    if (k->fog_vertex && !k->rhw)
    {
        sb_printf(b, "  float fd = %s;\n", k->range_fog ? "length(pe)" : "abs(pe.z)");
        emit_fog_factor(b, "o.fog", k->fog_vertex, "fd");
    }
    else
        sb_printf(b, "  o.fog = %s.a;\n", k->el[GFX_R_SPECULAR].used ? "v6" : "float4(1, 1, 1, 1)");

    for (int i = 0; i < k->ntex; ++i)
    {
        int idx = k->tci[i] & 7, gen = k->tci[i] >> 4, count = k->ttf[i] & 7;
        if (k->rhw)
            gen = 0;
        switch (gen)
        {
        case 1: sb_printf(b, "  float4 c%d = float4(N, 1.0);\n", i); break;
        case 2: sb_printf(b, "  float4 c%d = float4(pe, 1.0);\n", i); break;
        case 3: sb_printf(b, "  float4 c%d = float4(reflect(%s, N), 1.0);\n", i, k->localviewer ? "normalize(pe)" : "float3(0, 0, 1)"); break;
        default:
        {
            int reg = GFX_R_TEXCOORD0 + idx;
            if (!k->el[reg].used)
                sb_printf(b, "  float4 c%d = float4(0, 0, 0, 1);\n", i);
            else if (count)
            {
                /* D3D fills the component after the last one given with 1, which is how a 2D
                 * texture matrix carries its translation in the third row */
                int n = elem_components(k->el[reg].type);
                char y[16] = "1.0", z[16] = "1.0", w[16] = "1.0";
                if (n > 1)
                    snprintf(y, sizeof y, "v%d.y", reg);
                if (n > 2)
                    snprintf(z, sizeof z, "v%d.z", reg);
                if (n > 3)
                    snprintf(w, sizeof w, "v%d.w", reg);
                sb_printf(b, "  float4 c%d = float4(v%d.x, %s, %s, %s);\n", i, reg, y, z, w);
            }
            else
                sb_printf(b, "  float4 c%d = v%d;\n", i, reg);
            break;
        }
        }
        if (count && !k->rhw)
            sb_printf(b, "  c%d = mul(u.texm[%d], c%d);\n", i, i, i);
        sb_printf(b, "  o.t%d = c%d;\n", i, i);
    }
    gfx_hlsl_vs_return(b, k);
}

/* --- pixel: the texture stage cascade --------------------------------------------------------------- */
static void arg_expr(char* out, size_t n, int a)
{
    const char* base;
    switch (a & 0xF)
    {
    case 0: base = "vin.d"; break;
    case 1: base = "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u.tfactor"; break;
    case 4: base = "vin.s"; break;
    case 5: base = "tmp"; break;
    default: base = "cur"; break;
    }
    char v[64];
    if (a & 0x20)
        snprintf(v, sizeof v, "%s.aaaa", base);
    else
        snprintf(v, sizeof v, "%s", base);
    if (a & 0x10)
        snprintf(out, n, "(1.0 - %s)", v);
    else
        snprintf(out, n, "%s", v);
}

/* D3DTEXTUREOP on float4 arguments; the caller keeps .rgb or .a */
static void op_expr(char* out, size_t n, int op, const char* a1, const char* a2, const char* a0)
{
    switch (op)
    {
    case 2: snprintf(out, n, "%s", a1); break;
    case 3: snprintf(out, n, "%s", a2); break;
    case 4: snprintf(out, n, "(%s * %s)", a1, a2); break;
    case 5: snprintf(out, n, "(%s * %s * 2.0)", a1, a2); break;
    case 6: snprintf(out, n, "(%s * %s * 4.0)", a1, a2); break;
    case 7: snprintf(out, n, "(%s + %s)", a1, a2); break;
    case 8: snprintf(out, n, "(%s + %s - 0.5)", a1, a2); break;
    case 9: snprintf(out, n, "((%s + %s - 0.5) * 2.0)", a1, a2); break;
    case 10: snprintf(out, n, "(%s - %s)", a1, a2); break;
    case 11: snprintf(out, n, "(%s + %s - %s * %s)", a1, a2, a1, a2); break;
    case 12: snprintf(out, n, "lerp(%s, %s, vin.d.a)", a2, a1); break;
    case 13: snprintf(out, n, "lerp(%s, %s, tex.a)", a2, a1); break;
    case 14: snprintf(out, n, "lerp(%s, %s, u.tfactor.a)", a2, a1); break;
    case 15: snprintf(out, n, "(%s + %s * (1.0 - tex.a))", a1, a2); break;
    case 16: snprintf(out, n, "lerp(%s, %s, cur.a)", a2, a1); break;
    case 17: snprintf(out, n, "%s", a1); break; /* PREMODULATE: the next stage's texture is not known here */
    case 18: snprintf(out, n, "(%s + %s.a * %s)", a1, a1, a2); break;
    case 19: snprintf(out, n, "(%s * %s + %s.a)", a1, a2, a1); break;
    case 20: snprintf(out, n, "((1.0 - %s.a) * %s + %s)", a1, a2, a1); break;
    case 21: snprintf(out, n, "((1.0 - %s) * %s + %s.a)", a1, a2, a1); break;
    case 24: snprintf(out, n, "(float4)(saturate(dot((%s.rgb - 0.5) * 2.0, (%s.rgb - 0.5) * 2.0)))", a1, a2); break;
    case 25: snprintf(out, n, "(%s + %s * %s)", a0, a1, a2); break;
    case 26: snprintf(out, n, "(%s * %s + (1.0 - %s) * %s)", a0, a1, a0, a2); break;
    default: snprintf(out, n, "%s", a1); break; /* the bump ops: the environment map is not emulated */
    }
}

/* whether op uses the argument arg (0 Arg1, 1 Arg2, 2 Arg0): as op_expr does */
static int op_reads_arg(int op, int arg)
{
    switch (op)
    {
    case 3: return arg == 1;
    case 25:
    case 26: return 1;
    case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16:
    case 18: case 19: case 20: case 21: case 24: return arg <= 1;
    default: return arg == 0;
    }
}

/* whether a stage's operations read its texture (TEXTURE as an argument they use, or a blend by its alpha) */
static int stage_reads_texture(const GfxStage* s)
{
    const uint8_t col[3] = { s->ca1, s->ca2, s->ca0 }, alp[3] = { s->aa1, s->aa2, s->aa0 };
    if (s->cop == 13 || s->cop == 15 || s->aop == 13 || s->aop == 15) /* blends by the texture's alpha */
        return 1;
    for (int i = 0; i < 3; ++i)
        if ((op_reads_arg(s->cop, i) && (col[i] & 0xF) == 2) || (s->aop > 1 && op_reads_arg(s->aop, i) && (alp[i] & 0xF) == 2))
            return 1;
    return 0;
}

/* texture slot i sampled at coord: the slot's heap indices are root constants */
void gfx_hlsl_sample(Sb* b, const char* dst, int i, int cube, const char* coord);
void gfx_hlsl_sample(Sb* b, const char* dst, int i, int cube, const char* coord)
{
#ifdef VANACORE
    if (gfx_hlsl_d3d11)
    {
        sb_printf(b, "  %s = %s_%d.Sample(smp_%d, %s);\n", dst, cube ? "txc" : "tx2", i, i, coord);
        return;
    }
#endif
    char c = "xyzw"[i & 3];
    sb_printf(b, "  %s = %s[ti[%d].%c].Sample(smp[si[%d].%c], %s);\n", dst, cube ? "txc" : "tx2", i >> 2, c, i >> 2, c, coord);
}

/* VanaCore's normal maps: the surface normal N (view space, at pe) bent by the BC5 map in stage 7, read at stage 0's
 * coordinates. The game has no tangents, so the frame comes from how the position and the coordinates change from
 * pixel to pixel (Schueler's cotangent frame): it follows any mesh, and costs a few operations a pixel. Coordinates run
 * down the image (D3D's V), so a map with Y up (OpenGL's, chaiNNer's) is turned over here; nmap 3 reads a Y-down map as is. */
static void emit_normal_map(Sb* b, int nmap)
{
    sb_printf(b, "  {\n    float4 nmv;\n");
    gfx_hlsl_sample(b, "nmv", 7, 0, "vin.t0.xy");
    sb_printf(b, "    float2 uv = vin.t0.xy;\n"
                 "    float3 dp1 = ddx(pe), dp2 = ddy(pe);\n"
                 "    float2 du1 = ddx(uv), du2 = ddy(uv);\n"
                 "    float nl = length(N);\n"
                 "    float3 Ng = N / max(nl, 1e-6);\n"
                 "    float3 dp2p = cross(dp2, Ng), dp1p = cross(Ng, dp1);\n"
                 "    float det = dot(dp1, dp2p);\n"
                 "    float3 T = (dp2p * du1.x + dp1p * du2.x) * (det < 0.0 ? -1.0 : 1.0);\n"
                 "    float3 B = (dp2p * du1.y + dp1p * du2.y) * (det < 0.0 ? -1.0 : 1.0);\n"
                 "    float im = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));\n"
                 "    float2 xy = (nmv.rg * 2.0 - 1.0) * u.params2.y;\n"
                 "    float z = sqrt(saturate(1.0 - dot(xy, xy)));\n"
                 "    N = normalize(T * (im * xy.x) + B * (im * %sxy.y) + Ng * z) * nl;\n  }\n",
        nmap == 3 || nmap == 5 ? "" : "-");
}

/* The game's own pixel shaders (the zone's walls and ground: baked colour, no lighting of its own for VanaCore to bend).
 * Where each pixel stands in view space is worked out from the vertex shader's clip-space position (VOut.cp) and the
 * projection's scales (u.params2.zw, d3d8.c); the surface it is on from how that changes pixel to pixel; and the map bends it
 * as emit_normal_map does. The shader's colour is then lit again by the sun (u.tfactor.xyz, toward it): scaled by how much more
 * (or less) the bent normal faces it than the flat one, so a flat map changes nothing and the game's own look stays. */
static void emit_normal_relight(Sb* b, int nmap)
{
    sb_printf(b, "  float nm_gain = 1.0;\n  float3 nm_view = float3(0.5, 0.5, 1.0);\n  float3 nm_N = float3(0.0, 0.0, 0.0);\n"
                 "  if (u.params2.z > 0.0) {\n"
                 "    float3 pe = float3(vin.cp.xy / vin.cp.w * u.params2.zw * vin.cp.w, vin.cp.w);\n"
                 "    float3 N = normalize(cross(ddx(pe), ddy(pe)));\n"
                 "    if (dot(N, pe) > 0.0) N = -N;\n"
                 "    float3 Ng0 = N;\n");
    emit_normal_map(b, nmap);
    sb_printf(b, "    float3 L = normalize(u.tfactor.xyz);\n"
                 "    nm_gain = clamp(1.0 + 0.8 * (dot(N, L) - dot(Ng0, L)), 0.3, 1.8);\n"
                 "    nm_view = N * 0.5 + 0.5;\n"
                 "    nm_N = N;\n"
                 "  }\n");
}

/* The pixel function's start. Drawn into the sun's shadow map (an alpha-tested caster), it writes
 * no color: only its alpha test. Lit per pixel, the colors the vertex function would have given
 * are lit here. */
static void emit_fs_signature(Sb* b, const GfxFsKey* k, const GfxVsKey* vk)
{
    (void)k;
    int pix = pixel_lit(vk);
    sb_printf(b, depth_only(vk) ? "void fs_main(VOut %s) {\n" : "float4 fs_main(VOut %s) : SV_Target {\n", pix ? "vin0" : "vin");
    if (pix)
    {
        sb_printf(b, "  VOut vin = vin0;\n  float3 nm_N = float3(0.0, 0.0, 0.0);\n%s  {\n  float3 N = vin.n * (rsqrt(max(dot(vin.n, vin.n), 1e-20)) * %s);\n"
                     "  float3 pe = vin.pe.xyz;\n  float4 cd = vin.md, ca = vin.ma, cs = vin.ms, ce = vin.me;\n",
            k->wet ? "  float3 wet_N = float3(0.0, 0.0, 1.0), wet_pe = float3(0.0, 0.0, 1.0);\n" : "", vk->normalize ? "1.0" : "vin.pe.w");
        if (k->nmap && k->nmap != 6) /* (6: no map, the smooth-normal pass) */
            emit_normal_map(b, k->nmap);
        if (k->wet) /* the rain's wet look (gfx_hlsl_rain.h emit_wet) starts from the lit normal and the position */
            sb_printf(b, "  wet_N = N; wet_pe = pe;\n");
        if (k->nmap >= 4) /* the detail pass: the bent normal goes out (emit_fs_tail); the smooth-normal pass, the normal itself */
            sb_printf(b, "  nm_N = normalize(N);\n");
        emit_lighting(b, vk, 1, point_per_vertex(vk) ? 1 : 0);
        sb_printf(b, "  vin.d = lit_d; vin.s = lit_s;\n");
        if (k->nmap == 2) /* the bent normal as a colour: the lighting is its own, this shows the map is read */
            sb_printf(b, "  vin.d = float4(normalize(N) * 0.5 + 0.5, 1.0); vin.s = float4(0, 0, 0, 0);\n");
        sb_printf(b, "  }\n");
    }
}

/* GfxFsKey.pool: a world draw after the effects ran (d3d8_casters.h late_pool) given the player light's pool as the composite
 * (gfx_d3d12.c fx_comp, player_light) gives the draws before it, over its finished colour: by day what is outside the pool darker
 * by plight2.w, at night the lamp's light in it. Texture matrix 5 holds it (gfx_player_pool): the feet in view space and the
 * radius, the lamp's colour and the day's share, the world's up in view space. Where the composite takes a pixel's facing from
 * the depth, this takes the surface as level, as the water's layers are */
static void emit_pool(Sb* b, const char* col)
{
    sb_printf(b,
        "  {\n"
        "    float4x4 pool_t = transpose(u.texm[5]);\n"
        "    float pool_r = pool_t[0].w;\n"
        "    if (pool_r > 0.0) {\n"
        "      float pool_h = max(0.8 * pool_r, 3.0), pool_D = sqrt(pool_h * pool_h + pool_r * pool_r);\n"
        "      float3 pool_v = vin.pp - (pool_t[0].xyz + pool_t[2].xyz * pool_h);\n"
        "      float pool_d = length(pool_v), pool_m = 0.0;\n"
        "      float3 pool_dir = pool_v / max(pool_d, 1e-4);\n"
        "      if (pool_d > 1e-4 && pool_d <= 1.25 * pool_D)\n"
        "        pool_m = smoothstep(pool_h / pool_D, pool_h * rsqrt(pool_h * pool_h + 0.09 * pool_r * pool_r), dot(pool_dir, -pool_t[2].xyz)) *\n"
        "                 (1.0 - smoothstep(0.5, 1.0, pool_d / (1.25 * pool_D)));\n"
        "      float pool_k = pool_m * (1.0 - smoothstep(0.35, 0.9, max(%s.r, max(%s.g, %s.b))));\n"
        "      float3 pool_l = pool_t[1].rgb * (pool_k * lerp(0.75, 1.0, saturate(dot(pool_t[2].xyz, -pool_dir))));\n"
        "      %s.rgb *= (1.0 + pool_l) * lerp(1.0 - pool_t[1].w, 1.0, pool_m);\n"
        "    }\n"
        "  }\n",
        col, col, col, col);
}

/* GfxFsKey.hfog: a world draw after the effects ran (d3d8_casters.h late_pool) given the height fog as the composite (gfx_d3d12.c,
 * its fog) laid it over the draws before, over its finished colour: the density falling off with the height above the camera,
 * integrated along the view ray, and the sun's glow scattered in (Henyey-Greenstein). Texture matrix 4 holds it (gfx_height_fog):
 * the fog's colour and density, then falloff, most fog, g and the glow's red; up and its green; the sun and its blue. Where the
 * composite takes the pixel's place from the depth, this takes the draw's own (vin.pp, view space). mode: GfxFsKey.hfog */
static void emit_hfog(Sb* b, const char* col, int mode)
{
    sb_printf(b,
        "  {\n"
        "    float4x4 hf_t = transpose(u.texm[4]);\n"
        "    float3 hf_P = vin.pp;\n"
        "    float hf_d = length(hf_P), hf_bd = hf_t[1].x * dot(hf_P, hf_t[2].xyz);\n"
        "    float hf_k = abs(hf_bd) > 1e-4 ? (1.0 - exp(-hf_bd)) / hf_bd : 1.0;\n"
        "    float hf_f = min(1.0 - exp(-hf_t[0].a * hf_d * hf_k), hf_t[1].y);\n"
        "    float hf_g = hf_t[1].z, hf_cs = dot(hf_P / max(hf_d, 1e-5), hf_t[3].xyz);\n"
        "    float hf_s = pow((1.0 - hf_g) * (1.0 - hf_g) / max(1.0 + hf_g * hf_g - 2.0 * hf_g * hf_cs, 1e-5), 1.5);\n"
        "    float3 hf_c = hf_t[0].rgb + float3(hf_t[1].w, hf_t[2].w, hf_t[3].w) * hf_s;\n");
    if (mode == 2) /* adding light: the fog takes it away, as it hides what's behind */
        sb_printf(b, "    %s.rgb *= 1.0 - hf_f;\n", col);
    else if (mode == 3) /* premultiplied: the fog's share of what it covers */
        sb_printf(b, "    %s.rgb = lerp(%s.rgb, hf_c * saturate(%s.a), hf_f);\n", col, col, col);
    else
        sb_printf(b, "    %s.rgb = lerp(%s.rgb, hf_c, hf_f);\n", col, col);
    sb_printf(b, "  }\n");
}

/* wfog: the fog's colour is the water's wfog (emit_water_fs declares it), not the zone's */
static void emit_fs_tail(Sb* b, const GfxFsKey* k, const GfxVsKey* vk, const char* col, int wfog)
{
    /* VanaCore's smooth-normal pass (nmap 6, gfx_d3d12.c vn_replay): the vertex normal in view space - a vertex shader's from its
     * clip-space direction (VOut.cn) through the projection's 1/P00, 1/P11 and P23 (sun_f.xyz: the scene's, FFXI's right-handed);
     * a lit fixed-function draw's the one it's lit by (nm_N, emit_fs_signature) - turned to the side of the face the camera sees
     * (the face's own normal from where its pixels are: before the alpha test, whose discard would leave the derivatives without
     * neighbours) */
    const int vn = k->nmap == 6 && !depth_only(vk) ? (vk->cpos >= 2 ? 2 : pixel_lit(vk) ? 1 : 0) : 0;
    if (vn == 2)
        sb_printf(b, "  float3 vn_p = vin.cp.xyw * sun_f.xyz;\n"
                     "  float3 vn_g = cross(ddx(vn_p), ddy(vn_p));\n"
                     "  float3 vn_n = vin.cn.xyw * sun_f.xyz;\n");
    else if (vn == 1)
        sb_printf(b, "  float3 vn_p = vin.pe.xyz;\n"
                     "  float3 vn_g = cross(ddx(vn_p), ddy(vn_p));\n"
                     "  float3 vn_n = nm_N;\n");
    if (k->alpha_func && k->alpha_func != 8)
    {
        static const char* const cmp[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        if (k->alpha_func == 1)
            sb_printf(b, "  clip(-1.0);\n");
        else if (k->alpha_func < 8)
            sb_printf(b, "  if (!(round(saturate(%s.a) * 255.0) %s u.params.y)) discard;\n", col, cmp[k->alpha_func]);
    }
    if (depth_only(vk)) /* depth alone */
    {
        /* shadow 2, a caster fading out of the map (gfx_d3d12.c sun_fade): the share sun_f.x of its texels kept, by an 8x8
         * ordered pattern fixed to the world (sun_f.yz: the map's texel offset, so it holds still as the map follows the
         * camera). The maps' soft lookups average it into a fainter shadow. */
        if (vk->shadow == 2)
            sb_printf(b, "  int2 sunf_q = int2(floor(vin.pos.x) + sun_f.y, sun_f.z - floor(vin.pos.y));\n"
                         "  int sunf_x = sunf_q.x ^ sunf_q.y, sunf_y = sunf_q.y;\n"
                         "  int sunf_b = ((sunf_x & 1) << 5) | ((sunf_y & 1) << 4) | ((sunf_x & 2) << 2) | ((sunf_y & 2) << 1) |\n"
                         "    ((sunf_x & 4) >> 1) | ((sunf_y & 4) >> 2);\n"
                         "  if (float(sunf_b) + 0.5 >= sun_f.x * 64.0) discard;\n");
        sb_printf(b, "}\n");
        return;
    }
    /* VanaCore's motion pass (gfx_d3d12.c mo_draw): each pixel's motion, as a share of the viewport to where it was the frame
     * before (fx_dlssmv makes it DLSS's render pixels: u.vp here is in the game's pixels, not the scene's drawn ones), and its
     * distance from the camera (view z) and sun_f.y (2: an effect's model whose depth fx_dlssz hands on), in place of its
     * colour - where the draw shows: a blended one's pixels with alpha, or adding light, of sun_f.x or more (a fifth: the clear quad round a spark isn't the spark; less for an effect's model
     * with frame generation, fg_fx_motion) */
    /* the plants' mask for FSR (gfx_d3d12.c mask_draw): where the draw shows past its alpha test, 1 and its view z */
    if (vk->motion & GFX_MO_MASK)
    {
        sb_printf(b, "  return float4(1.0, abs(vin.ez), 0.0, 0.0);\n}\n");
        return;
    }
    if (vk->motion)
    {
        if (vk->motion & GFX_MO_ADD) /* the light it adds: its colour, times its alpha where that's the source's factor */
        {
            sb_printf(b, "  float mo_w = max(saturate(%s.r), max(saturate(%s.g), saturate(%s.b)));\n", col, col, col);
            if (vk->motion & GFX_MO_ALPHA)
                sb_printf(b, "  mo_w *= saturate(%s.a);\n", col);
            sb_printf(b, "  if (mo_w < sun_f.x) discard;\n");
        }
        else if (vk->motion & GFX_MO_ALPHA)
            sb_printf(b, "  if (saturate(%s.a) < sun_f.x) discard;\n", col);
        sb_printf(b,
            "  if (vin.mp.w <= 1e-5 || vin.mc.w <= 1e-5) return float4(0.0, 0.0, abs(vin.ez), sun_f.y);\n"
            "  return float4((vin.mp.xy / vin.mp.w - vin.mc.xy / vin.mc.w) * float2(0.5, -0.5), abs(vin.ez), sun_f.y);\n}\n");
        return;
    }
    /* ... stored longer than a normal map's, so the effects tell them apart (fx normal_det, smooth_at): twice its length where the
     * game lights the surface by it (a fixed-function draw), three times where it doesn't (a zone shader's: its baked colour) */
    if (k->nmap == 6)
    {
        if (vn)
            sb_printf(b, "  if (dot(vn_n, vn_n) < 1e-20 || dot(vn_g, vn_g) < 1e-30) discard;\n"
                         "  vn_n = normalize(vn_n);\n"
                         "  if (dot(vn_g, vn_p) > 0.0) vn_g = -vn_g;\n"
                         "  if (dot(vn_n, vn_g) < 0.0) vn_n = -vn_n;\n"
                         "  return float4(vn_n * %s, vin.ez);\n}\n", vn == 2 ? "3.0" : "2.0");
        else
            sb_printf(b, "  discard;\n  return float4(0.0, 0.0, 0.0, 0.0);\n}\n");
        return;
    }
    if (k->nmap >= 4) /* the detail pass (d3d12 nout_begin): the bent normal and the view z, after the alpha test above */
    {
        sb_printf(b, "  return float4(nm_N, vin.ez);\n}\n");
        return;
    }
    if (k->fog)
    {
        if (k->fog == 4)
            sb_printf(b, "  float f = saturate(vin.fog);\n");
        else
        {
            sb_printf(b, "  float f;\n  float fd = abs(vin.ez);\n");
            emit_fog_factor(b, "f", k->fog, "fd");
        }
        /* VanaCore's water fogs to its own wfog (gfx_hlsl_water.h emit_water_depth: open water to the sky over the horizon) */
        sb_printf(b, "  %s.rgb = lerp(%s, %s.rgb, f);\n", col, wfog ? "wfog" : "u.fogcolor.rgb", col);
    }
    if (k->pool && !k->prog && !vk->prog && !vk->rhw) /* after the fog, as the composite's is over the game's fogged picture */
        emit_pool(b, col);
    if (k->hfog && !k->prog && !vk->prog && !vk->rhw) /* after the pool, as the composite lays its fog after it */
        emit_hfog(b, col, k->hfog);
    sb_printf(b, "  return %s;\n}\n", col);
}

#include "gfx_hlsl_water.h" /* emit_water_functions, emit_water_fs: VanaCore's water (GfxFsKey.water) */
#include "gfx_hlsl_rain.h"  /* emit_rain_functions, emit_wet: the characters wet in the rain (GfxFsKey.wet) */

static void emit_ff_fs(Sb* b, const GfxFsKey* k, const GfxVsKey* vk)
{
    emit_fs_signature(b, k, vk);
    if ((k->rain == 1 || k->rain == 3) && !depth_only(vk)) /* VanaCore's rain pass over the scene, or its wet ground's mask (gfx_hlsl_rain.h) */
    {
        emit_rain_fs(b, k->rain == 3);
        emit_fs_tail(b, k, vk, "col", 0);
        return;
    }
    if (k->water && !depth_only(vk) && !vk->rhw)
    {
        emit_water_fs(b, vk, k->water, k->fog, (k->st[3].tex != 0) | (k->st[2].tex != 0) << 1);
        emit_fs_tail(b, k, vk, "col", 1);
        return;
    }
    sb_printf(b, "  float4 cur = vin.d, tmp = float4(0, 0, 0, 0), tex = float4(1, 1, 1, 1);\n");
    for (int i = 0; i < k->nstages; ++i)
    {
        const GfxStage* s = &k->st[i];
        char coord[64];
        if (s->tex == 1)
        {
            if (s->projected)
            {
                const char* w = s->ncoord == 3 ? "z" : s->ncoord == 4 ? "w" : "y";
                snprintf(coord, sizeof coord, "vin.t%d.xy / vin.t%d.%s", i, i, w);
            }
            else
                snprintf(coord, sizeof coord, "vin.t%d.xy", i);
            gfx_hlsl_sample(b, "tex", i, 0, coord);
        }
        else if (s->tex == 2)
        {
            snprintf(coord, sizeof coord, "vin.t%d.xyz", i);
            gfx_hlsl_sample(b, "tex", i, 1, coord);
        }
        else if (stage_reads_texture(s))
            /* a stage the game left on with no texture bound, reading TEXTURE: skipped, as Direct3D does one (the stage is
             * off: CURRENT, the vertex colour at stage 0, carries on). A stage 0 of the default MODULATE with nothing bound
             * is how untextured vertex-coloured draws are made. White made Ashita's Minimap a white disc whenever the game's
             * login scene or an effect left stage 2 on (MODULATEALPHA_ADDCOLOR of CURRENT and TEXTURE): it adds the mask's
             * alpha to the whole map; black (a null texture in a pixel shader) made a stage 0 draw black. Normal frames have
             * no such stage past stage 0 (checked over ~6000 draws). */
            continue;
        char a1[64], a2[64], a0[64], ce[512], ae[512];
        arg_expr(a1, sizeof a1, s->ca1);
        arg_expr(a2, sizeof a2, s->ca2);
        arg_expr(a0, sizeof a0, s->ca0);
        op_expr(ce, sizeof ce, s->cop, a1, a2, a0);
        const char* dst = s->result == 5 ? "tmp" : "cur";
        if (s->cop == 24) /* DOTPRODUCT3 goes to every channel, alpha too */
        {
            sb_printf(b, "  %s = saturate(%s);\n", dst, ce);
            continue;
        }
        if (s->aop > 1)
        {
            arg_expr(a1, sizeof a1, s->aa1);
            arg_expr(a2, sizeof a2, s->aa2);
            arg_expr(a0, sizeof a0, s->aa0);
            op_expr(ae, sizeof ae, s->aop, a1, a2, a0);
            sb_printf(b, "  { float3 c = saturate(%s.rgb); float a = saturate(%s.a); %s = float4(c, a); }\n", ce, ae, dst);
        }
        else /* alpha disabled: the alpha carries on unchanged */
            sb_printf(b, "  { float3 c = saturate(%s.rgb); %s = float4(c, %s.a); }\n", ce, dst, i ? "cur" : "vin.d");
    }
    if (k->specular_add)
        sb_printf(b, "  cur.rgb = saturate(cur.rgb + vin.s.rgb);\n");
    if (k->rain == 2 && !depth_only(vk)) /* the rain's splashes, where the scene is (gfx_hlsl_rain.h) */
        emit_rain_soft(b);
    /* the rain's wet look over the finished colour (gfx_hlsl_rain.h): a character lit per pixel into the scene - not the sun's,
     * motion or normal passes, whose keys carry the flag along */
    if (k->wet && pixel_lit(vk) && !depth_only(vk) && !vk->motion && k->nmap < 4)
        emit_wet(b, vk);
    /* [graphics] characters.brightness below 1 (d3d8.c character_light): the characters' finished colour times it, handed over as
     * 1 - k in the ambient's spare alpha (0 for every other draw). After every clamp above, so it is the game's picture dimmed:
     * the light dimmed before them let the clamps stop hiding what the game's light does from frame to frame (white sleeves
     * flickering on a running character) */
    if (vk->lighting)
        sb_printf(b, "  cur.rgb *= 1.0 - u.ambient.w;\n");
    if (k->text_shadow) /* the glyph's ink, whether the sheet keeps it in its alpha or its colour (FFXI adds its text's colour) */
        sb_printf(b, "  cur = float4(0.0, 0.0, 0.0, saturate(min(tex.a, max(tex.r, max(tex.g, tex.b)))) * u.tfactor.a);\n");
    emit_fs_tail(b, k, vk, "cur", 0);
}

char* gfx_hlsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
#ifdef VANACORE
    if (gfx_hlsl_d3d11)
    {
        /* PRELUDE with Direct3D 11's registers in place of Direct3D 12's heap tables (the Bind buffer and
         * the unbounded arrays); the uniforms, the Sun buffer and the streams as they are */
        static const char tables_end[] = "SamplerState smp[] : register(s0);\n";
        const char* bind = strstr(PRELUDE, "cbuffer Bind");
        const char* sun = strstr(PRELUDE, "cbuffer Sun");
        const char* tables = strstr(PRELUDE, "Texture2D tx2[]");
        const char* rest = strstr(PRELUDE, tables_end) + sizeof tables_end - 1;
        sb_printf(&b, "%.*s", (int)(bind - PRELUDE), PRELUDE);
        sb_printf(&b, "%.*s", (int)(tables - sun), sun);
        sb_printf(&b, "%s%s", PRELUDE_D3D11_RESOURCES, rest);
    }
    else
#endif
        sb_printf(&b, "%s", PRELUDE);
    /* VanaCore's water (GfxFsKey.water): a fixed-function draw whose pixels are lit here, not per pixel by the effects' lights */
    GfxVsKey water_vk;
    int water = fk->water && !vk->prog && !fk->prog && !vk->rhw;
    if (water)
    {
        water_vk = *vk;
        water_vk.pixel = 0;
        vk = &water_vk;
    }
    /* the player light's pool (GfxFsKey.pool): a fixed-function draw's view-space position goes on to the pixel function */
    /* ... and the height fog's (GfxFsKey.hfog) */
    const int pool = (fk->pool || fk->hfog) && !vk->prog && !fk->prog && !vk->rhw;
    emit_vout(&b, vk, water, pool);
    if (vk->prog)
    {
        if (!gfx_hlsl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk, water, pool);
    if (water && !depth_only(vk)) /* the water's own functions, before the pixel function (gfx_hlsl_water.h) */
        emit_water_functions(&b);
    if (fk->wet && !fk->prog && pixel_lit(vk) && !depth_only(vk) && !vk->motion && fk->nmap < 4) /* the rain's (gfx_hlsl_rain.h) */
        emit_rain_functions(&b);
    if (fk->rain && !fk->prog && !vk->prog && !depth_only(vk)) /* the rain pass's (gfx_hlsl_rain.h) */
        emit_rain_functions(&b), emit_rain_pass_functions(&b);
    if (fk->prog)
    {
        emit_fs_signature(&b, fk, vk);
        int relight = fk->nmap && fk->nmap < 6 && vk->cpos && !depth_only(vk);
        if (relight)
            emit_normal_relight(&b, fk->nmap);
        if (!gfx_hlsl_ps1(&b, fk, ps_tokens))
            goto fail;
        if (relight)
            sb_printf(&b, "  r0.rgb = %s;\n", fk->nmap == 2 ? "nm_view" : "saturate(r0.rgb * nm_gain)");
        emit_fs_tail(&b, fk, vk, "r0", 0);
    }
    else
        emit_ff_fs(&b, fk, vk);
    return b.s;
fail:
    free(b.s);
    return NULL;
}

/* --- the device's own functions: the back buffer to the window, and the frame-rate overlay -------------- */
const char gfx_hlsl_util[] =
    "cbuffer Bind : register(b1) { uint4 ti[2]; uint4 si[2]; };\n"
    "Texture2D tx2[] : register(t0, space1);\n"
    "SamplerState smp[] : register(s0);\n"
    "struct PO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "PO present_vs(uint vid : SV_VertexID) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  PO o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "float4 present_fs(PO i) : SV_Target {\n"
    "  return float4(tx2[ti[0].x].Sample(smp[si[0].x], i.uv).rgb, 1.0);\n"
    "}\n"
    /* VanaCore's interface layer over the frame: premultiplied, as drawn */
    "float4 present_layer_fs(PO i) : SV_Target {\n"
    "  return tx2[ti[0].x].Sample(smp[si[0].x], i.uv);\n"
    "}\n"
    /* the same, sharpened by k (0..1, ti[0].y): contrast-adaptive, a negative lobe over the four
     * neighbors that shrinks where the neighborhood is already near black or white (no halos on hard
     * edges); gfx_metal.m's present_cas_fs */
    "float4 present_cas_fs(PO i) : SV_Target {\n"
    "  uint w, h; tx2[ti[0].x].GetDimensions(w, h);\n"
    "  float2 tx = 1.0 / float2(w, h);\n"
    "  float k = asfloat(ti[0].y);\n"
    "  SamplerState s = smp[si[0].x];\n"
    "  float3 c = tx2[ti[0].x].Sample(s, i.uv).rgb;\n"
    "  float3 n = tx2[ti[0].x].Sample(s, i.uv - float2(0, tx.y)).rgb, so = tx2[ti[0].x].Sample(s, i.uv + float2(0, tx.y)).rgb;\n"
    "  float3 we = tx2[ti[0].x].Sample(s, i.uv - float2(tx.x, 0)).rgb, e = tx2[ti[0].x].Sample(s, i.uv + float2(tx.x, 0)).rgb;\n"
    "  float3 mn = min(c, min(min(n, so), min(we, e))), mx = max(c, max(max(n, so), max(we, e)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));\n"
    "  float3 lobe = -amp * lerp(0.125, 0.2, saturate(k));\n"
    "  return float4(saturate((c + (n + so + we + e) * lobe) / (1.0 + 4.0 * lobe)), 1.0);\n"
    "}\n"
    /* the frame-rate overlay: a 5x7 bitmap font drawn per pixel, no texture */
    "cbuffer OU : register(b0) { float4 rect; float scale; uint n; float2 size; uint4 text[8]; };\n"
    "static const uint FONT[17 * 7] = {\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00 };\n"
    "float4 overlay_vs(uint vid : SV_VertexID) : SV_Position {\n"
    "  float2 c = rect.xy + float2((vid & 1) ? rect.z : 0.0, (vid & 2) ? rect.w : 0.0);\n"
    "  return float4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0, 1);\n"
    "}\n"
    "float4 overlay_fs(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 p = (pos.xy - rect.xy) / scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  if (p.x >= 0.0 && cell < int(n) && gx < 5 && gy >= 0 && gy < 7) {\n"
    "    uint ch = text[cell >> 2][cell & 3];\n"
    "    if ((FONT[ch * 7 + uint(gy)] >> (4 - gx)) & 1) return float4(1.0, 0.85, 0.2, 1.0);\n"
    "  }\n"
    "  return float4(0, 0, 0, 0.55);\n"
    "}\n";
