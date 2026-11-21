/* VanaCore's own copy of the zone's meshes for the sun's shadows ([graphics.effects] sun_zone_mesh; zonemesh.h).
 *
 * Why: the sun's maps took the zone from what the game drew - its draws for the camera, and casters.cpp's pass, which has the
 * game's renderer draw each piece round the player every few frames and keeps references to the game's buffers. The game
 * draws a piece with the render states the piece before it left, so in the pass's order some pieces came out blended or not
 * depth-writing and were turned down as casters for most rounds (Ru'Lude Gardens, 2026-11-04: two pieces by the player gave
 * casters 55 frames in every 432, the sun through the ceiling onto the floor, then not). Here the zone's meshes are VanaCore's
 * own, read from the zone's DAT (core/zonemesh.cpp), laid in the world once and kept on the GPU: nothing the game draws, frees
 * or rewrites changes them while the player is in the zone.
 *
 * They reach the maps the way the caster pass's draws do (gfx_draw with `sought` 2, in world space, each part by its own
 * `piece`): the sun's cache keeps each and draws it into the maps every frame, the still layers among them, and while they come
 * the game's own draws of the zone are left out of the maps (gfx_sun_cache.h sun_pass_only). A share goes each frame, each part
 * again every ZM_ROUND frames, inside the cache's keep (PIECE_KEEP). Characters still cast from the game's draws. Direct3D 12's
 * (Direct3D 11 has no caster pass: its gfx_draw drops `sought` draws). */
#include "zonemesh.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

#define FMT_A8R8G8B8 21u  /* D3DFMT (as grass.c and snow.c) */
#define ZM_ROUND 8        /* frames a round of the parts takes (casters.cpp kRoundWalks) */
#define ZM_ALPHA_REF 47.0f /* a cut-out's texel is kept over this alpha: the game's textures hold half-scale alpha (0x80 solid)
                            * and the game tests 4 x the vertex's alpha (0x80) x the texture's, xim's cut at 0.375: 48 and up */

float g_gfx_zone_mesh = 1.0f;

typedef struct ZRange
{
    uint32_t first, count; /* vertices */
    int32_t texture;       /* -1 solid (zm.pos); else zm.tex's (zm.cut) */
} ZRange;

static struct
{
    int zone;               /* the zone they're for (0: none held) */
    GfxBuf *pos, *cut;      /* solid: x, y, z; cut out: x, y, z, u, v */
    GfxBuf *cpos, *cnrm;    /* the curved surfaces' triangles alone (zm_curved): x, y, z; nx, ny, nz, curve (VanaZoneMesh.nrm) */
    uint32_t ncurved;       /* ... their vertices */
    uint32_t npos;          /* pos's vertices */
    ZRange* ranges;
    uint32_t nranges;
    GfxTex** tex;
    uint32_t ntex;
    uint32_t next;          /* the part handed over next (zonemesh_scene) */
} zm;

static void zm_free(void)
{
    gfx_buf_destroy(zm.pos);
    gfx_buf_destroy(zm.cut);
    gfx_buf_destroy(zm.cpos);
    gfx_buf_destroy(zm.cnrm);
    for (uint32_t i = 0; i < zm.ntex; ++i)
        gfx_tex_destroy(zm.tex[i]);
    free(zm.tex);
    free(zm.ranges);
    memset(&zm, 0, sizeof zm);
}

void zonemesh_set_zone(int zone)
{
    if (zm.zone && zone != zm.zone)
        zm_free(); /* the last zone's meshes cast nothing in this one; its own are handed over once read */
}

void zonemesh_device_lost(void)
{
    zm_free(); /* handed over again (core: zone_casters::forget) */
}

static GfxBuf* zm_buf(const float* v, uint32_t floats)
{
    if (!v || !floats)
        return NULL;
    GfxBuf* b = gfx_buf_create(floats * 4u);
    if (b)
        gfx_buf_upload(b, v, floats * 4u);
    return b;
}

/* a cut-out's texture, its mips box-filtered (as grass.c asset_texture), its alpha as the DAT has it */
static GfxTex* zm_texture(const VanaZoneMeshTexture* in)
{
    if (!in->argb || !in->width || !in->height)
        return NULL;
    const int pot = !(in->width & (in->width - 1)) && !(in->height & (in->height - 1));
    uint32_t levels = 1;
    for (uint32_t w = in->width, h = in->height; pot && (w > 1 || h > 1); w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        ++levels;
    GfxTex* t = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, in->width, in->height, levels, GFX_USE_SAMPLE);
    uint32_t* cur = t ? (uint32_t*)malloc((size_t)in->width * in->height * 4) : NULL;
    if (!cur)
    {
        gfx_tex_destroy(t);
        return NULL;
    }
    memcpy(cur, in->argb, (size_t)in->width * in->height * 4);
    uint32_t w = in->width, h = in->height;
    for (uint32_t l = 0; l < levels; ++l)
    {
        gfx_tex_upload(t, 0, l, cur, w * 4);
        if (l + 1 == levels)
            break;
        const uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
            {
                uint32_t sum[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t sx = x * 2 + (uint32_t)(k & 1) < w ? x * 2 + (uint32_t)(k & 1) : w - 1;
                    const uint32_t sy = y * 2 + (uint32_t)(k >> 1) < h ? y * 2 + (uint32_t)(k >> 1) : h - 1;
                    const uint32_t c = cur[(size_t)sy * w + sx];
                    sum[0] += c >> 24, sum[1] += (c >> 16) & 255, sum[2] += (c >> 8) & 255, sum[3] += c & 255;
                }
                cur[(size_t)y * nw + x] = (sum[0] / 4 << 24) | (sum[1] / 4 << 16) | (sum[2] / 4 << 8) | sum[3] / 4;
            }
        w = nw, h = nh;
    }
    free(cur);
    return t;
}

/* The solid triangles curved at all three corners (zonemesh.cpp curve_of: rock and ground; a building's mesh has none), in
 * buffers of their own for Direct3D 11's zn_draw: a town draws none, open country not its walls and props */
static void zm_curved(const VanaZoneMesh* m)
{
    if (!m->nrm)
        return;
    uint32_t n = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        float *p = NULL, *q = NULL;
        if (pass)
        {
            if (!n)
                return;
            p = (float*)malloc((size_t)n * 12u), q = (float*)malloc((size_t)n * 16u);
            if (!p || !q)
            {
                free(p), free(q);
                return;
            }
            n = 0;
        }
        for (uint32_t i = 0; i < m->nranges; ++i)
        {
            const VanaZoneMeshRange* r = &m->ranges[i];
            if (r->texture >= 0 || r->count < 3 || r->first + r->count > m->npos)
                continue;
            for (uint32_t v = r->first; v + 3 <= r->first + r->count; v += 3)
            {
                const float* c = &m->nrm[(size_t)v * 4u];
                if (c[3] <= 0.005f || c[7] <= 0.005f || c[11] <= 0.005f)
                    continue;
                if (pass)
                {
                    memcpy(p + (size_t)n * 3u, &m->pos[(size_t)v * 3u], 36);
                    memcpy(q + (size_t)n * 4u, c, 48);
                }
                n += 3;
            }
        }
        if (pass)
        {
            zm.cpos = zm_buf(p, n * 3u), zm.cnrm = zm_buf(q, n * 4u);
            zm.ncurved = zm.cpos && zm.cnrm ? n : 0;
            free(p), free(q);
        }
    }
}

void zonemesh_set(const VanaZoneMesh* m)
{
    zm_free();
    if (!m || !m->zone || !m->nranges || !m->ranges)
        return;
    zm.ranges = (ZRange*)calloc(m->nranges, sizeof *zm.ranges);
    zm.tex = m->ntextures ? (GfxTex**)calloc(m->ntextures, sizeof *zm.tex) : NULL;
    if (!zm.ranges || (m->ntextures && !zm.tex))
    {
        zm_free();
        return;
    }
    zm.pos = zm_buf(m->pos, m->npos * 3u);
    zm.cut = zm_buf(m->cut, m->ncut * 5u);
    zm_curved(m);
    zm.npos = zm.pos ? m->npos - m->npos % 3 : 0;
    zm.ntex = m->ntextures;
    for (uint32_t i = 0; i < zm.ntex; ++i)
        zm.tex[i] = zm_texture(&m->textures[i]);
    for (uint32_t i = 0; i < m->nranges; ++i)
    {
        const VanaZoneMeshRange* r = &m->ranges[i];
        const int cut = r->texture >= 0;
        if (r->count < 3 || (cut ? !zm.cut || r->first + r->count > m->ncut || (uint32_t)r->texture >= zm.ntex || !zm.tex[r->texture]
                                 : !zm.pos || r->first + r->count > m->npos))
            continue; /* (a cut-out whose texture the DAT hasn't: left out) */
        zm.ranges[zm.nranges++] = (ZRange){ r->first, r->count - r->count % 3, r->texture };
    }
    zm.zone = m->zone;
    rt_log("[recomp] gfx: zone %d's own meshes for the sun's shadows: %u parts, %u solid and %u cut-out vertices (%u KB), %u "
           "textures; %u vertices of curved rock and ground ([graphics.effects] sun_zone_mesh, sun_curved)\n",
        m->zone, zm.nranges, m->npos, m->ncut, (m->npos * 12u + m->ncut * 20u) / 1024u, zm.ntex, zm.ncurved);
}

int zonemesh_resident(void)
{
    return g_gfx_zone_mesh != 0.0f && zm.nranges != 0;
}

int zonemesh_normals(GfxBuf** pos, GfxBuf** nrm, uint32_t* n)
{
    if (!zm.zone || !zm.cpos || !zm.cnrm || zm.ncurved < 3)
        return 0;
    *pos = zm.cpos, *nrm = zm.cnrm, *n = zm.ncurved;
    return 1;
}

/* part i as a draw of the caster pass's: in the world (sought 2: its matrix the identity, the vertices placed already), both
 * sides, depth written; a cut-out with its texture and the alpha test */
static void zm_draw(uint32_t i, const GfxScene* s)
{
    static GfxDraw d;
    const ZRange* r = &zm.ranges[i];
    const int cut = r->texture >= 0;
    const uint32_t stride = cut ? 20u : 12u;
    memset(&d, 0, sizeof d);
    d.vs.el[GFX_R_POSITION] = (GfxElem){ 1, 0, GFX_FLOAT3, 0 };
    d.u.offset[GFX_R_POSITION] = 0, d.u.stride[0] = (int32_t)stride;
    d.fs.nstages = 1;
    if (cut)
    {
        d.vs.el[GFX_R_TEXCOORD0] = (GfxElem){ 1, 0, GFX_FLOAT2, 0 };
        d.u.offset[GFX_R_TEXCOORD0] = 12;
        d.vs.ntex = 1, d.vs.tci[0] = 0;
        d.fs.st[0] = (GfxStage){ 2 /* SELECTARG1 */, 2 /* TEXTURE */, 0, 1, 2, 2, 0, 1, 1, 1 /* 2D */, 0, 2 };
        d.fs.alpha_func = 5; /* GREATER */
        d.u.params[1] = ZM_ALPHA_REF;
        d.tex[0] = zm.tex[r->texture];
        d.samp[0].addr_u = d.samp[0].addr_v = d.samp[0].addr_w = 1; /* WRAP */
        d.samp[0].mag = d.samp[0].min = d.samp[0].mip = 2;          /* LINEAR */
    }
    else
        d.fs.st[0] = (GfxStage){ 2, 0 /* DIFFUSE */, 0, 1, 2, 0, 0, 1, 1, 0, 0, 2 };
    for (int k = 0; k < 4; ++k)
    {
        d.u.wvp[k * 5] = d.u.wv[k * 5] = 1.0f;
        d.u.mat_d[k] = d.u.mat_a[k] = d.u.ambient[k] = d.u.tfactor[k] = 1.0f;
        d.u.vp[k] = s->vp[k] ? (float)s->vp[k] : 1.0f; /* (the shadow's vertex function divides by its size) */
    }
    memcpy(d.vp, s->vp, sizeof d.vp);
    d.cull = 1; /* NONE: a mirrored placement turns its faces over */
    d.fill = 3;
    d.depth.zenable = 1, d.depth.zwrite = 1, d.depth.zfunc = 4;
    d.prim = GFX_TRIANGLELIST, d.count = r->count / 3;
    d.buf[0] = cut ? zm.cut : zm.pos, d.buf_off[0] = r->first * stride, d.size[0] = r->count * stride;
    d.sought = 2, d.piece = i + 1, d.piece_draw = 0;
    gfx_draw(&d);
}

void zonemesh_scene(const GfxScene* s)
{
    if (!zonemesh_resident() || !s)
        return;
    const uint32_t share = (zm.nranges + ZM_ROUND - 1) / ZM_ROUND;
    for (uint32_t k = 0; k < share; ++k)
    {
        if (zm.next >= zm.nranges)
            zm.next = 0;
        zm_draw(zm.next++, s);
    }
}
