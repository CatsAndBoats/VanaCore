/* Texture packs' normal maps and Stone Relief's parallax, as both back ends (gfx_d3d12.c, gfx_d3d11.c) key and set them:
 * their [graphics.effects] settings and a draw's pixel-function key for them. Included from gfx.h. */
#pragma once

/* the parallax on a draw's key (gfx_hlsl.c emit_pom): only with its normal map lit (nmap 1-3) and [graphics.effects] pom on;
 * its depth in thousandths of the texture's width (pom_depth), and the stones' shade from the sun (pom_shadow) */
static inline void gfx_pom_key(GfxFsKey* fs, float pom, float depth, float shadow)
{
    if (fs->nmap < 1 || fs->nmap > 3 || !fs->pom || pom < 0.5f)
    {
        fs->pom = fs->pom_shadow = 0;
        return;
    }
    const float dp = depth * 1000.0f + 0.5f;
    fs->pom = (uint8_t)(dp < 1.0f ? 1.0f : dp > 255.0f ? 255.0f : dp);
    fs->pom_shadow = shadow >= 0.5f;
}

/* the normal maps on a draw's key, as gfx_d3d12.c has them: the lit pixel function's normal bent, or the game's own shaders lit
 * again from the vertex shader's clip-space position (gfx_hlsl.c emit_normal_relight); 1 lit, 2 the bent normal as the
 * colour, 3 lit with the maps' Y turned over */
static inline void gfx_nmap_key(GfxVsKey* vs, GfxFsKey* fs, float normalmap, float view)
{
    if (normalmap < 0.5f || (fs->prog ? !vs->prog : !vs->pixel))
        fs->nmap = 0;
    else if (fs->nmap)
    {
        fs->nmap = (uint8_t)(1 + (view >= 1.5f ? 2 : view >= 0.5f ? 1 : 0));
        vs->cpos = fs->prog != 0;
    }
}

/* the back ends' FxSettings fields and settings-table entries for them */
#define GFX_POM_FX_FIELDS float pom, pom_depth, pom_shadow; /* the parallax: on, the joints' depth (a share of the width), shade */
#define GFX_POM_FX_KEYS                                                                                                       \
    { "pom", offsetof(FxSettings, pom), 0.0f }, { "pom_depth", offsetof(FxSettings, pom_depth), 0.02f },                     \
        { "pom_shadow", offsetof(FxSettings, pom_shadow), 1.0f },
#define GFX_NMAP_FX_KEYS                                                                                                      \
    { "normalmap", offsetof(FxSettings, normalmap), 0.0f }, { "normalmap_strength", offsetof(FxSettings, normalmap_strength), 1.0f }, \
        { "normalmap_view", offsetof(FxSettings, normalmap_view), 0.0f },

/* The composite's (gfx_d3d12.c FX_HLSL_2): a normal-mapped draw lit its bumps by the sun as it drew (gfx_hlsl.c
 * emit_normal_relight's nm_gain: the side of a stone facing the sun up to 1.8 times, the far side down to 0.3) before anything
 * knew where the shadows fall: in a building's shadow each stone still had a lit face and a dark one, the shadow broken up
 * stone by stone (Upper Jeuno, 2026-11-26). Where the maps (and the contact shadow) have the sun blocked, that gain is taken
 * off again: the bent normal (the detail target's, normalmap_fx) and the surface's own plane (the depth's) against the game's
 * sun, as the draw weighed them. Not where the depth's plane is no plane: along the join of two pieces of floor, a step of a
 * pixel in the depth, it leans every way, and the gain taken off there drew a thin bright line down each join (Bastok Markets,
 * 2026-11-27): off where it is over 50 degrees from the bent normal, and taken off at most 1.6 times either way */
#define GFX_FX_NM_SHADOW_HLSL                                                                       \
    "  if (u.sun.w > 0.0 && z != 0.0 && (u.smap.x > 0.0 || u.shadow.x > 0.0)) {\n"                  \
    "    float3 Pn = view_pos(i.pos.xy, z);\n"                                                      \
    "    float4 nb = normal_det(u.clar.w, i.pos.xy, Pn);\n"                                         \
    "    if (nb.w > 0.0) {\n"                                                                       \
    "      float4 np = normal_depth(2, i.pos.xy, Pn);\n"                                            \
    "      if (np.w > 0.0 && dot(np.xyz, nb.xyz) > 0.64) {\n"                                       \
    "        float ng = clamp(1.0 + 0.8 * (dot(nb.xyz, u.sun.xyz) - dot(np.xyz, u.sun.xyz)), 0.625, 1.6);\n" \
    "        float vis = saturate((u.smap.x > 0.0 ? os.y : 1.0) * (u.shadow.x > 0.0 ? os.z : 1.0));\n" \
    "        c.rgb *= lerp(1.0 / ng, 1.0, vis);\n"                                                  \
    "      }\n"                                                                                     \
    "    }\n"                                                                                       \
    "  }\n"
