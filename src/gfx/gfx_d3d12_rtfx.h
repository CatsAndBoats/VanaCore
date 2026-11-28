/* gfx_d3d12_rtfx.h: an include unit of gfx_d3d12.c (in its scope: g_dev, g_fxs, g_rt, g_ds, g_serial, draw_encode). The game's
 * effects in the ray-traced reflections ([graphics.effects] refl_rt_fx): a spell, a hike point's glow, a lamp's flare are no
 * surface - blended over the finished picture, no depth, in neither the screen's depth nor the zone's tree - so no ray from the
 * floor finds them. Each effect that adds light (3D, blended ONE + ADD, not the interface: as el_emit takes them), and each unlit
 * model blended over the world (a home point's crystal), is drawn again as it comes, from the scene's own camera, into a layer of
 * its own at half the scene's size each way: its colour (every effect's light added up, no depth test) and its depth (the
 * nearest effect's, a second draw of each into the depth alone). fx_zlate, once the game has drawn its last effect, sends each
 * reflecting floor pixel's ray through that layer as the screen-space reflections go through the scene - stepped across the
 * picture, stopping where it passes behind the world - and where it passes an effect's depth, takes the effect's colour there, by
 * the pixel's reflection weight.
 *
 * Till 2026-11-28 the effects were drawn mirrored about one floor level, then two, picked from the floors the view looked onto: a
 * mirror is true only for floors at its level, and on Whitegate's stairs the walkway and the stairs took both levels and the
 * pool below lost its home point crystal's reflection. A ray through the effects' own depth has no level: every floor shows what
 * its ray meets. */
#pragma once

#define MFX_SCALE 0.5f /* the layer's size, of the scene's each way */

static struct
{
    GfxTex *t, *d;   /* the effects' colour and their depth: drawn this frame, read by fx_zlate */
    GfxTex* scene;   /* the scene they're taken from (g_rt then) */
    uint64_t serial; /* the frame they're being drawn in (0 none) */
    uint64_t drawn;  /* the frame they were last begun in */
    float sx, sy;    /* the layer's size over the scene's: a draw's viewport by it (viewport_for) */
    uint32_t n;      /* effects drawn into it */
    float up[4];     /* this frame's up in view space times the effects' strength, for fx_zlate (mfx_refl_in) */
} g_mfx;

/* the models drawn before the scene ends, kept to be drawn into the layer (mfx_rec_new, mfx_replay) */
static struct
{
    Caster* c;
    GfxPipeKey* pipe;
    GfxDepthKey* depth;
    D3D12_GPU_VIRTUAL_ADDRESS* inst; /* a batch's pieces' transforms (gfx_d3d12_batch.h), copied to the ring; 0 a draw of its own */
    uint32_t* ninst;                 /* ... how many, and in which stream (GfxVsKey.inst) in the top byte */
    uint32_t n, cap;
    uint64_t serial; /* the frame they're of */
} g_mfr;

static int mfx_wanted(void)
{
    return g_dev && g_fxs.fx != 0.0f && g_fxs.refl > 0.0f && g_fxs.refl_rt >= 2.0f && g_fxs.refl_rt_fx > 0.0f;
}

static void mfx_release(void)
{
    gfx_tex_destroy(g_mfx.t), gfx_tex_destroy(g_mfx.d);
    memset(&g_mfx, 0, sizeof g_mfx);
    g_mfr.n = 0, g_mfr.serial = 0; /* (its arrays kept for the next) */
}

/* The models the game draws before the world's scene ends (the effects' compose runs at the first particle): a hike point's
 * crystal, blended over the world without writing depth. Too early for mfx_emit (this frame's layer isn't begun), each is kept as
 * draw_encode keeps a caster - its uniforms, streams, indices and textures, in this frame's ring - with its blend and depth test,
 * and drawn into the layer once it's begun (mfx_replay). Not the zone's decals (D3DRS_ZBIAS: a stain, a patch of other stone
 * blended over a wall): the wall's reflection has them where the screen shows the wall, and laid on their own, over nothing,
 * each came out a bright square on the floor */
static Caster* mfx_rec_new(const GfxDraw* d)
{
    if (!mfx_wanted() || g_mfxout || g_mout || g_nout || g_mfx.serial == g_serial || !g_mfx.scene || g_rt != g_mfx.scene || g_rt_face ||
        g_rt_level || d->vs.rhw || d->overlay || d->screen_fx || !d->pipe.blend || !d->depth.zenable || d->depth.zwrite || d->zbias)
        return NULL;
    if (g_mfr.serial != g_serial)
        g_mfr.n = 0, g_mfr.serial = g_serial;
    if (g_mfr.n == g_mfr.cap)
    {
        const uint32_t cap = g_mfr.cap ? g_mfr.cap * 2 : 64;
        Caster* c = (Caster*)realloc(g_mfr.c, cap * sizeof(Caster));
        if (c)
            g_mfr.c = c;
        GfxPipeKey* p = c ? (GfxPipeKey*)realloc(g_mfr.pipe, cap * sizeof(GfxPipeKey)) : NULL;
        if (p)
            g_mfr.pipe = p;
        GfxDepthKey* z = p ? (GfxDepthKey*)realloc(g_mfr.depth, cap * sizeof(GfxDepthKey)) : NULL;
        if (z)
            g_mfr.depth = z;
        D3D12_GPU_VIRTUAL_ADDRESS* iv = z ? (D3D12_GPU_VIRTUAL_ADDRESS*)realloc(g_mfr.inst, cap * sizeof *iv) : NULL;
        if (iv)
            g_mfr.inst = iv;
        uint32_t* in = iv ? (uint32_t*)realloc(g_mfr.ninst, cap * sizeof *in) : NULL;
        if (!in)
            return NULL;
        g_mfr.ninst = in, g_mfr.cap = cap;
    }
    /* a batch of the zone's blended pieces (a hike point's crystal is one): their transforms, as draw_encode hands them */
    D3D12_GPU_VIRTUAL_ADDRESS iva = 0;
    if (d->ninst)
    {
        Alloc v = ring((size_t)d->ninst * GFX_INST_BYTES, 16);
        if (!v.cpu || !d->vs.inst)
            return NULL;
        memcpy(v.cpu, d->inst, (size_t)d->ninst * GFX_INST_BYTES);
        iva = v.gpu;
    }
    const uint32_t i = g_mfr.n++;
    g_mfr.inst[i] = iva, g_mfr.ninst[i] = d->ninst ? d->ninst | (uint32_t)d->vs.inst << 24 : 0;
    Caster* c = &g_mfr.c[i];
    memset(c, 0, sizeof *c);
    c->lib.vs = d->vs, c->lib.fs = d->fs;
    c->vs = gfx_tokens_kept(d->vs.prog, 0, d->vs_tokens), c->ps = gfx_tokens_kept(d->fs.prog, 1, d->ps_tokens);
    c->cull = d->cull, c->cpu_u = ~0u, c->mfx = 1;
    g_mfr.pipe[i] = d->pipe, g_mfr.depth[i] = d->depth;
    return c;
}

/* An effect's pipeline in the layer, by the pass (g_mfxout): 2 its depth alone (written, the nearest kept: no colour), 1 its colour
 * with no depth test (every effect's light adds up, as on the screen, where none writes depth). The scene's own camera, the
 * draw's own culling */
static void mfx_pass_key(PipeKey* k, int pass)
{
    k->lib.vs.pixel = 0, k->lib.vs.motion = 0, k->lib.vs.cpos = 0;
    k->lib.fs.nmap = 0, k->lib.fs.wet = 0, k->lib.fs.pool = 0, k->lib.fs.hfog = 0;
    k->depth.stencil = k->depth.sfail = k->depth.szfail = k->depth.spass = k->depth.sfunc = k->depth.sread = k->depth.swrite = 0;
    k->zbias = 0;
    if (pass == 2)
        k->depth.zenable = 1, k->depth.zwrite = 1, k->depth.zfunc = 4 /* LESSEQUAL */, k->pipe.write_mask = 0;
    else
        k->depth.zenable = 0, k->depth.zwrite = 0;
}

/* this frame's early models into the layer, its targets bound: each its depth, then its colour (mfx_pass_key) */
static uint32_t mfx_replay(ID3D12GraphicsCommandList* l)
{
    uint32_t drawn = 0;
    if (g_mfr.serial != g_serial)
        return 0;
    D3D12_INDEX_BUFFER_VIEW bound_ib;
    memset(&bound_ib, 0, sizeof bound_ib);
    for (uint32_t i = 0; i < g_mfr.n; ++i)
    {
        Caster* cs = &g_mfr.c[i];
        if (cs->dead || !cs->n)
            continue;
        const uint32_t pieces = g_mfr.ninst[i] & 0xFFFFFF, islot = g_mfr.ninst[i] >> 24;
        for (int pass = 2; pass >= 1; --pass)
        {
            PipeKey pk;
            memset(&pk, 0, sizeof pk);
            pk.lib = cs->lib;
            pk.lib.vs.inst = (uint8_t)(pieces ? islot : 0);
            pk.color = (uint32_t)g_mfx.t->dxfmt, pk.dsv = (uint32_t)g_mfx.d->dxfmt;
            pk.pipe = g_mfr.pipe[i], pk.depth = g_mfr.depth[i];
            mfx_pass_key(&pk, pass);
            pk.cull = (uint8_t)(cs->cull ? cs->cull : 1), pk.fill = 3;
            pk.topo = (uint8_t)(cs->topo == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                : cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINELIST || cs->topo == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
                ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
            ID3D12PipelineState* ps = pipeline_for(&pk, cs->vs, cs->ps);
            if (!ps)
                continue; /* building: next frame */
            for (int t = 0; t < 8; ++t)
                if (cs->tex[t])
                    tex_state(cs->tex[t], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, ROOT_BIND, 16, cs->bind, 0);
            set_pso(ps);
            set_topology((D3D12_PRIMITIVE_TOPOLOGY)cs->topo);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(l, ROOT_U, cs->ub);
            for (int j = 0; j <= GFX_NSTREAMS; ++j)
                if (cs->buf[j])
                    buf_state(cs->buf[j], BUF_READ);
            for (int st = 0; st < GFX_NSTREAMS; ++st)
            {
                const D3D12_GPU_VIRTUAL_ADDRESS va = pieces && st == (int)islot - 1 ? g_mfr.inst[i] : cs->vb[st];
                if (va)
                    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(l, ROOT_STREAM0 + st, va);
            }
            if (cs->ibv.BufferLocation)
            {
                if (memcmp(&cs->ibv, &bound_ib, sizeof bound_ib))
                    ID3D12GraphicsCommandList_IASetIndexBuffer(l, &cs->ibv), bound_ib = cs->ibv;
                ID3D12GraphicsCommandList_DrawIndexedInstanced(l, cs->n, pieces ? pieces : 1, 0, 0, 0);
            }
            else
                ID3D12GraphicsCommandList_DrawInstanced(l, cs->n, pieces ? pieces : 1, cs->vstart, 0);
        }
        drawn++;
    }
    return drawn;
}

/* fx_zlate's inputs (gfx_d3d12.c zl_run, once the game has drawn its last effect): 3 the effects' colour, 4 the reflections' layer
 * (its alpha each pixel's weight), 5 the effects' depth; u->rtx (this pass's own) this frame's up times the effects' strength, 0
 * with none */
static void mfx_late_in(int32_t* in, FxU* u)
{
    memset(u->rtx, 0, sizeof u->rtx);
    if (!mfx_wanted() || !g_mfx.t || !g_mfx.d || g_mfx.t->srv < 0 || g_mfx.d->srv < 0 || g_mfx.serial != g_serial || !g_mfx.n ||
        !g_fx.rl.res || g_fx.rl.srv < 0)
        return;
    tex_state(g_mfx.t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tex_state(g_mfx.d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fx_state(&g_fx.rl, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    in[3] = g_mfx.t->srv, in[4] = g_fx.rl.srv, in[5] = g_mfx.d->srv;
    memcpy(u->rtx, g_mfx.up, sizeof u->rtx);
}

static int mfx_targets(uint32_t w, uint32_t h, uint32_t depth_fmt)
{
    if (!g_mfx.t || g_mfx.t->w != w || g_mfx.t->h != h)
    {
        gfx_tex_destroy(g_mfx.t);
        g_mfx.t = gfx_tex_create(GFX_TEX_2D, 21 /* D3DFMT_A8R8G8B8 */, w, h, 1, GFX_USE_RT);
    }
    if (!g_mfx.d || g_mfx.d->w != w || g_mfx.d->h != h || g_mfx.d->fmt != depth_fmt)
    {
        gfx_tex_destroy(g_mfx.d);
        g_mfx.d = gfx_tex_create(GFX_TEX_2D, depth_fmt, w, h, 1, GFX_USE_DEPTH);
    }
    return g_mfx.t && g_mfx.d && g_mfx.t->srv >= 0 && g_mfx.d->srv >= 0;
}

/* After they're laid: this frame's layer begun - its colour cleared to none, its depth to the far plane, and the models drawn
 * before the scene ended drawn into it */
static void mfx_begin(GfxTex* color, const GfxScene* s)
{
    g_mfx.serial = 0;
    if (!mfx_wanted())
    {
        if (g_mfx.t)
            mfx_release();
        return;
    }
    const uint32_t w = (uint32_t)((float)color->w * MFX_SCALE + 0.5f), h = (uint32_t)((float)color->h * MFX_SCALE + 0.5f);
    const GfxTex* sd = color->depth_seen;
    if (!s || w < 16 || h < 16 || !mfx_targets(w, h, sd && sd->use == GFX_USE_DEPTH ? sd->fmt : F_D24S8))
        return;
    BATCH_SETTLE(); /* the zone's draws kept: their casters with them */
    const int part_was = gpu_part_cur();
    gpu_part(GP_SSR);
    ID3D12GraphicsCommandList* l = list();
    tex_state(g_mfx.t, D3D12_RESOURCE_STATE_RENDER_TARGET);
    tex_state(g_mfx.d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = target_view(g_mfx.t, 0, 0), dsv = target_view(g_mfx.d, 0, 0);
    static const float none[4] = { 0, 0, 0, 0 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(l, rtv, none, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(l, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT v = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    g_targets_bound = 0;
    drawn_forget();
    const uint32_t early = mfx_replay(l); /* the models drawn before the scene ended (the hike point's crystal) */
    g_targets_bound = 0; /* the game's next draw binds its own again */
    drawn_forget();
    gpu_part(part_was);
    g_mfx.sx = (float)w / (float)color->w, g_mfx.sy = (float)h / (float)color->h;
    if (g_serial % 1200 == 0) /* (the log's, every 20 s or so) */
        fprintf(stderr, "[recomp] gfx: reflections' effects: %u models drawn before the scene ended (of %u kept), %u effects the frame before\n",
            early, g_mfr.serial == g_serial ? g_mfr.n : 0, g_mfx.n);
    g_mfx.scene = color, g_mfx.serial = g_mfx.drawn = g_serial, g_mfx.n = early;
}

/* After a draw (gfx_draw): one of the game's effects that adds light into this frame's scene, drawn again into the layer. And an
 * effect's model blended over the world, SRCALPHA onto INVSRCALPHA, unlit, no depth written: a home point's crystal is one the
 * game draws after its scene's effects, and with the adding ones alone its glow and rings were in the reflection and the crystal
 * wasn't. Not the zone's own blended parts (lit, a decal's by its D3DRS_ZBIAS) nor VanaCore's water */
static int mfx_takes(const GfxDraw* d)
{
    if (!d->pipe.blend || d->pipe.op != 1)
        return 0;
    return d->pipe.dst == 2 || (d->pipe.src == 5 && d->pipe.dst == 6 && !d->vs.lighting && !d->fs.water && !d->zbias && !d->depth.zwrite);
}

/* ... its depth, then its colour (mfx_pass_key) */
static void mfx_emit(const GfxDraw* d)
{
    if (g_mfx.serial != g_serial || !g_mfx.t || g_rt != g_mfx.scene || g_rt_face || g_rt_level || d->vs.rhw || d->overlay ||
        d->screen_fx || !mfx_takes(d))
        return;
    GfxTex *rt = g_rt, *ds = g_ds, *seen = g_mfx.t->depth_seen;
    for (int pass = 2; pass >= 1; --pass)
    {
        g_rt = g_mfx.t, g_ds = g_mfx.d, g_targets_bound = 0, g_mfxout = pass;
        draw_encode(d);
        g_mfxout = 0, g_rt = rt, g_ds = ds, g_targets_bound = 0;
        drawn_forget(); /* (the viewport was the layer's) */
    }
    g_mfx.t->depth_seen = seen;
    g_mfx.n++;
}

/* A draw's viewport drawn again into the layer (viewport_for, g_mfxout): the scene's made the layer's size */
static void mfx_viewport(float* x, float* y, float* w, float* h)
{
    *x *= g_mfx.sx, *w *= g_mfx.sx, *y *= g_mfx.sy, *h *= g_mfx.sy;
}

/* fx_zlate's u.rtx for the effects (mfx_late_in): this frame's up in view space (the view's own, as rt_bind's: u->up is eased and
 * trails a camera that pitches), as long as the effects' strength, the floor the rays leave taken as level (the reflections keep to
 * floors that face up) */
static void mfx_refl_in(int ssr, int32_t* refl_in, int* refl_n, const FxU* u, const GfxScene* s)
{
    (void)refl_in;
    (void)refl_n;
    (void)u;
    const float st = ssr && mfx_wanted() ? fminf(g_fxs.refl_rt_fx, 1.0f) : 0.0f;
    const float sign = s->view[5] < 0.0f ? -1.0f : 1.0f;
    const float up[3] = { s->view[4] * sign, s->view[5] * sign, s->view[6] * sign };
    const float l = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    for (int k = 0; k < 3; ++k)
        g_mfx.up[k] = l > 1e-6f ? up[k] / l * st : 0.0f;
    g_mfx.up[3] = 0.0f;
}

/* the pipeline an effect is drawn again with (pipeline, g_mfxout: the pass) */
static void mfx_key(PipeKey* k, const GfxDraw* d)
{
    (void)d;
    mfx_pass_key(k, g_mfxout);
}
