/* The zone's draws sent together ([graphics.effects] batch; gfx_d3d12.c includes this once, after draw_encode).
 *
 * Why: an open zone at a long draw distance is thousands of draws a frame, and each one costs the CPU the same whatever it
 * draws - its pipeline found, its textures and streams bound, its uniforms copied, the driver's own work - so the game waits on
 * the draws, the GPU idle half the frame. The Sanctuary of Zi'Tah at drawdistance 8 (2026-11-11, at the outpost): 6,100 draws
 * a frame, 35 fps, 22 ms of the 28 ms frame in the Direct3D calls; at 1x 1,450 draws and 60 fps. Its extra pieces are the same
 * few meshes again and again - 944 of one cedar, 766 of another - and of the 3,229 opaque draws of the zone in a frame, 3,101
 * fell into 117 groups of four or more of one mesh, texture and state, which can go as 117 draws, each drawing its group's
 * pieces once apiece (DrawInstanced, each piece's transforms in a stream the draw doesn't read: GfxVsKey.inst, gfx_hlsl.c).
 *
 * The game never draws two of one mesh one after the other (it walks its zone by place), so the draws are kept while it draws
 * its zone (gfx_batch, around FFXiMain's zone draw: core/zonebatch.cpp) and sent at its end: first the opaque groups, then
 * the rest (the leaves' blended edges, decals) in the order they came. The opaque ones write the depth and need no order; the
 * blended ones write none and are drawn over the zone's opaque parts, as they ought to be (before, an opaque piece drawn after a
 * blended one in front of it covered it). A draw that can't wait (its vertices or indices in memory the game reuses, a vertex
 * or pixel shader of the game's) sends what's kept first and is drawn at once; so does anything else the back end is asked
 * (targets, a clear, a copy, a buffer or texture a kept draw reads changing: BATCH_TOUCH).
 *
 * Each piece is still its own shadow caster (batch_casters): the sun's maps and their cache draw them one by one, as before. A
 * group whose pipeline isn't built yet is drawn piece by piece meanwhile. Off, or on Direct3D 11, nothing changes.
 *
 * The blended ones are grouped too (2026-11-12): with the opaque ones sent together, the leaves' blended edges were the most of
 * what was left, some 2,200 draws a frame of Zi'Tah from above its canopy at 10x, one by one. A blended draw that tests the depth
 * but writes none goes in a group of its own kind (BatchGroup.blended), drawn where its first piece came among the rest kept in
 * order (a mark in rest), after every opaque draw as before; its pieces in the order they came. Drawn so, blended edges of two
 * meshes overlapping may go the other way round: the game's order was its walk's, by place, never back to front either. */

#define BATCH_SLOTS 1024 /* the hash's slots (a power of two) */
#define BATCH_MOST 16384 /* draws kept at most before they're sent */
#define BATCH_GROUP_MARK 0x80000000u /* in rest: a blended group drawn there (its index below), not a draw's place in bytes */

typedef struct BatchGroup
{
    GfxDraw d;       /* the first of them as it came (its uniforms hold its piece's transforms) */
    uint32_t need;   /* the uniforms' bytes the draw reads (draw_encode's), compared */
    uint32_t n;      /* pieces */
    int32_t next;    /* the next group in its hash slot, -1 none */
    uint8_t slot;    /* the stream the pieces' transforms go in */
    uint8_t blended; /* blended, writing no depth: drawn at its mark among the rest, not with the opaque groups */
} BatchGroup;

typedef struct BatchPiece
{
    GfxInst inst;
    uint32_t group;
} BatchPiece;

/* The first of batch_slot's tests a draw fails (batch_why), BATCH_WHY_BUFFERS past them (its streams or indices: the game's
 * memory, rewritten, or no stream free): the log counts them for the blended draws left out */
enum
{
    BATCH_WHY_KIND,    /* opaque without a depth write, or blended with one; or a caster of the caster pass's */
    BATCH_WHY_PROGRAM, /* through a shader of the game's, in screen space (rhw), or without the depth test */
    BATCH_WHY_SPECIAL, /* water, a normal map, a character, an effect's light, the interface's */
    BATCH_WHY_ZBIAS,   /* an opaque one lifted by a z-bias (a decal) */
    BATCH_WHY_FILL,    /* points, or an opaque one pulled toward the camera (u.pad0[2]: a water tile's, a decal's) */
    BATCH_WHY_ANCHOR,  /* no place in the world (anchor[3] 0) */
    BATCH_WHY_PRIM,    /* not a list or strip of triangles */
    BATCH_WHY_MOTION,  /* dlss_motion on: each piece's motion drawn again */
    BATCH_WHY_BUFFERS,
    BATCH_WHY_N
};

static struct
{
    int open;     /* the game is drawing its zone (gfx_batch) */
    int flushing; /* batch_flush is drawing what was kept */
    int32_t slots[BATCH_SLOTS];
    BatchGroup* groups;
    uint32_t ngroups, groups_cap;
    BatchPiece* pieces;
    uint32_t npieces, pieces_cap;
    GfxInst* sorted; /* the pieces by group, at the flush */
    uint32_t sorted_cap;
    uint32_t* first; /* each group's first piece in sorted */
    uint32_t first_cap;
    uint8_t* bytes; /* the rest, kept in order: each its GfxDraw without the uniforms it doesn't read (batch_pack) */
    uint32_t nbytes, bytes_cap;
    uint32_t* rest; /* ... where each starts in bytes */
    uint32_t nrest, rest_cap;
    GfxDraw scratch;
    /* the log's counts: pieces kept, the draws they went out as, those kept in order, since the last line; of the pieces kept,
     * the blended ones */
    uint32_t kept, sent, ordered, windows, kept_blended;
    uint32_t why[BATCH_WHY_N]; /* ... the blended draws not grouped, by the test they failed (batch_why) */
    uint64_t said_serial;
} g_batch;

static int batch_grow(void** p, uint32_t* cap, uint32_t need, size_t size)
{
    if (need <= *cap)
        return 1;
    uint32_t c = *cap ? *cap : 256;
    while (c < need)
        c *= 2;
    void* more = realloc(*p, (size_t)c * size);
    if (!more)
        return 0;
    *p = more, *cap = c;
    return 1;
}

/* the uniforms' bytes a draw reads (draw_encode's need) */
static uint32_t batch_need(const GfxDraw* d)
{
    if (d->fs.prog)
        return sizeof(GfxU);
    if (d->vs.prog)
        return offsetof(GfxU, psc);
    return (uint32_t)(offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight));
}

/* A draw that can wait to be sent: everything it reads is the back end's (static buffers, textures) or a copy of its own, not
 * memory the game reuses after the call */
static int batch_can_wait(const GfxDraw* d)
{
    if (d->vs.prog || d->fs.prog || d->vs_tokens || d->ps_tokens || d->text_shadow || d->label || d->overlay)
        return 0; /* (a program's tokens are the game's) */
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (d->data[s] && d->size[s])
            return 0;
    /* indices from the game's memory (none in a static buffer, or a fan's, which are read to make a list) */
    return !d->indices || (d->ibuf && d->prim != GFX_TRIANGLEFAN);
}

/* The first of batch_slot's tests a draw fails (BATCH_WHY_*, above), BATCH_WHY_BUFFERS past them */
static int batch_why(const GfxDraw* d)
{
    const int blended = d->pipe.blend != 0;
    if (blended ? (d->caster > 1 || d->depth.zwrite) : (d->caster != 1 || !d->depth.zwrite))
        return BATCH_WHY_KIND;
    if (d->vs.rhw || d->vs.prog || d->fs.prog || !d->depth.zenable)
        return BATCH_WHY_PROGRAM;
    if (d->water || d->fs.water || d->fs.nmap || d->overlay || d->screen_fx || d->is_char || d->el_lantern || d->dlss5_skip ||
        d->el_early || d->label || d->text_shadow)
        return BATCH_WHY_SPECIAL;
    if (d->zbias && !blended)
        return BATCH_WHY_ZBIAS;
    /* (u.pad0[2] is a z-biased draw's pull toward the camera, vofs.w, d3d8.c decal_pull: in the uniforms batch_same holds alike,
     * the vertex shader's in clip space, so each piece of a group is pulled as it was alone) */
    if ((d->u.pad0[2] && !blended) || d->fill == 1)
        return BATCH_WHY_FILL;
    /* (a blended one has none: scene_caster places only the casters; it's placed by a world matrix all the same, and what isn't
     * - a character's, made again each frame - is kept out by is_char and its buffers) */
    if (d->anchor[3] == 0.0f && !blended)
        return BATCH_WHY_ANCHOR;
    if (d->prim != GFX_TRIANGLELIST && d->prim != GFX_TRIANGLESTRIP)
        return BATCH_WHY_PRIM;
    if (g_fxs.dlss_motion != 0.0f)
        return BATCH_WHY_MOTION;
    return BATCH_WHY_BUFFERS;
}

/* The stream a piece of a group's transforms can go in (one the draw doesn't read), or -1: an opaque draw of the zone's own
 * pieces (placed by a world matrix, from buffers the game keeps and doesn't rewrite), through the fixed function, that nothing
 * after it draws again but the sun's maps (no motion of its own, no mask: the zone stands still; no normal map; no decal) - or,
 * *blended 1, a blended one of them that tests the depth and writes none (a leaf's edge: the game lifts those onto their leaf
 * with a z-bias, 8 in Zi'Tah, which the group's pipeline takes as its own and batch_same holds alike) */
static int batch_slot(const GfxDraw* d, int* blended)
{
    *blended = d->pipe.blend != 0;
    if (batch_why(d) != BATCH_WHY_BUFFERS)
        return -1;
    int used = 0;
    for (int r = 0; r < GFX_NREGS; ++r)
        if (d->vs.el[r].used)
            used |= 1 << d->vs.el[r].stream;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        if (d->data[s] && d->size[s])
            return -1;
        if (!(used >> s & 1))
            continue;
        if (!d->buf[s] || VOLATILE(d->buf[s]) || REWRITTEN(d->buf[s]))
            return -1;
    }
    if (d->indices && (!d->ibuf || VOLATILE(d->ibuf) || REWRITTEN(d->ibuf)))
        return -1;
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (!(used >> s & 1))
            return s;
    return -1;
}

/* Two draws of a group: everything alike but the piece's own (its transforms, its anchor, the caster pass's piece) */
static int batch_same(const GfxDraw* a, const GfxDraw* b, uint32_t need)
{
    return !memcmp(&a->vs, &b->vs, sizeof a->vs) && !memcmp(&a->fs, &b->fs, sizeof a->fs) &&
        !memcmp(&a->pipe, &b->pipe, sizeof a->pipe) && !memcmp(&a->depth, &b->depth, sizeof a->depth) &&
        !memcmp(a->tex, b->tex, sizeof a->tex) && !memcmp(a->samp, b->samp, sizeof a->samp) &&
        !memcmp(a->buf, b->buf, sizeof a->buf) && !memcmp(a->buf_off, b->buf_off, sizeof a->buf_off) &&
        !memcmp(a->size, b->size, sizeof a->size) && !memcmp(a->cpu, b->cpu, sizeof a->cpu) &&
        !memcmp(a->cpu_size, b->cpu_size, sizeof a->cpu_size) && !memcmp(a->vp, b->vp, sizeof a->vp) &&
        a->prim == b->prim && a->count == b->count && a->indices == b->indices && a->index_size == b->index_size &&
        a->ibuf == b->ibuf && a->ibuf_off == b->ibuf_off && a->vertex_start == b->vertex_start && a->cull == b->cull &&
        a->stencil_ref == b->stencil_ref && a->small_rt == b->small_rt && a->zbias == b->zbias &&
        !memcmp(a->u.texm, b->u.texm, need - offsetof(GfxU, texm));
}

static uint32_t batch_hash(const GfxDraw* d)
{
    uintptr_t k[8] = { (uintptr_t)d->buf[0], (uintptr_t)d->ibuf, (uintptr_t)d->tex[0], (uintptr_t)d->tex[1], d->buf_off[0],
                       d->ibuf_off, d->vertex_start, d->count };
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; ++i)
        h = (h ^ (uint32_t)k[i]) * 16777619u, h ^= h >> 15;
    return h & (BATCH_SLOTS - 1);
}

/* what a kept draw reads changes or goes (BATCH_TOUCH) only if it's marked with this window's */
static void batch_mark(const GfxDraw* d)
{
    for (int s = 0; s < GFX_NSTREAMS; ++s)
        if (d->buf[s])
            d->buf[s]->batch = g_batch_id;
    if (d->ibuf)
        d->ibuf->batch = g_batch_id;
    for (int i = 0; i < 8; ++i)
        if (d->tex[i])
            d->tex[i]->batch = g_batch_id;
}

/* a draw kept in its group: 0 when there's no room (it's drawn as it came). A new blended group's mark goes among the rest
 * kept in order, where its first piece came */
static int batch_group(const GfxDraw* d, int slot, int blended)
{
    const uint32_t need = batch_need(d), h = batch_hash(d);
    int32_t g = g_batch.slots[h];
    while (g >= 0 && (g_batch.groups[g].need != need || !batch_same(&g_batch.groups[g].d, d, need)))
        g = g_batch.groups[g].next;
    if (!batch_grow((void**)&g_batch.pieces, &g_batch.pieces_cap, g_batch.npieces + 1, sizeof(BatchPiece)))
        return 0;
    if (g < 0)
    {
        if (!batch_grow((void**)&g_batch.groups, &g_batch.groups_cap, g_batch.ngroups + 1, sizeof(BatchGroup)) ||
            (blended && !batch_grow((void**)&g_batch.rest, &g_batch.rest_cap, g_batch.nrest + 1, sizeof(uint32_t))))
            return 0;
        g = (int32_t)g_batch.ngroups++;
        BatchGroup* ng = &g_batch.groups[g];
        ng->d = *d, ng->need = need, ng->n = 0, ng->slot = (uint8_t)slot, ng->next = g_batch.slots[h];
        ng->blended = (uint8_t)(blended != 0);
        g_batch.slots[h] = g;
        if (blended)
            g_batch.rest[g_batch.nrest++] = BATCH_GROUP_MARK | (uint32_t)g;
    }
    BatchPiece* p = &g_batch.pieces[g_batch.npieces++];
    memcpy(p->inst.m, d->u.wvp, sizeof p->inst.m); /* wvp, wv, wvit: GfxU's first 48 floats */
    memcpy(p->inst.anchor, d->anchor, sizeof p->inst.anchor);
    p->group = (uint32_t)g;
    g_batch.groups[g].n++;
    return 1;
}

/* the rest kept in order: its GfxDraw without the uniforms past the ones it reads */
static int batch_pack(const GfxDraw* d)
{
    const uint32_t need = batch_need(d);
    const uint32_t head = (uint32_t)offsetof(GfxDraw, u), tail = (uint32_t)(sizeof(GfxDraw) - offsetof(GfxDraw, u) - sizeof(GfxU));
    const uint32_t size = (4 + head + need + tail + 7) & ~7u;
    if (!batch_grow((void**)&g_batch.bytes, &g_batch.bytes_cap, g_batch.nbytes + size, 1) ||
        !batch_grow((void**)&g_batch.rest, &g_batch.rest_cap, g_batch.nrest + 1, sizeof(uint32_t)))
        return 0;
    uint8_t* at = g_batch.bytes + g_batch.nbytes;
    memcpy(at, &need, 4);
    memcpy(at + 4, d, head);
    memcpy(at + 4 + head, &d->u, need);
    memcpy(at + 4 + head + need, (const uint8_t*)d + head + sizeof(GfxU), tail);
    g_batch.rest[g_batch.nrest++] = g_batch.nbytes;
    g_batch.nbytes += size;
    return 1;
}

static const GfxDraw* batch_unpack(uint32_t off)
{
    const uint8_t* at = g_batch.bytes + off;
    uint32_t need;
    memcpy(&need, at, 4);
    const uint32_t head = (uint32_t)offsetof(GfxDraw, u), tail = (uint32_t)(sizeof(GfxDraw) - offsetof(GfxDraw, u) - sizeof(GfxU));
    GfxDraw* d = &g_batch.scratch;
    memcpy(d, at + 4, head);
    memcpy(&d->u, at + 4 + head, need);
    memcpy((uint8_t*)d + head + sizeof(GfxU), at + 4 + head + need, tail);
    return d;
}

static void batch_reset(void)
{
    g_batch.ngroups = g_batch.npieces = g_batch.nbytes = g_batch.nrest = 0;
    g_batch_n = 0;
    memset(g_batch.slots, 0xFF, sizeof g_batch.slots);
    g_batch_id = g_batch_id + 1 ? g_batch_id + 1 : 1; /* (the marks of the window before are stale) */
}

/* A group's pieces drawn one by one: its instanced pipeline isn't built yet (or there's one piece) */
static void batch_each(GfxDraw* d, const GfxInst* inst, uint32_t n)
{
    d->ninst = 0, d->inst = NULL, d->vs.inst = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        memcpy(d->u.wvp, inst[i].m, sizeof inst[i].m);
        memcpy(d->anchor, inst[i].anchor, sizeof inst[i].anchor);
        draw_one(d);
    }
}

/* A group sent (its pieces sorted, from first): one draw for them, or each alone while its pipeline isn't built */
static void batch_send_group(uint32_t g)
{
    BatchGroup* bg = &g_batch.groups[g];
    GfxDraw* d = &bg->d;
    const GfxInst* inst = g_batch.sorted + g_batch.first[g];
    if (bg->n < 2)
    {
        batch_each(d, inst, bg->n);
        return;
    }
    d->vs.inst = (uint8_t)(bg->slot + 1), d->ninst = bg->n, d->inst = inst;
    /* its pipeline built off the game's thread: until it is, the pieces one by one */
    if (!bind_targets() || !pipeline(d, depth_attachment()))
    {
        batch_each(d, inst, bg->n);
        return;
    }
    draw_one(d);
    g_batch.sent++;
}

/* A group's pieces each as it came, unsorted (no room to sort them) */
static void batch_each_unsorted(uint32_t g)
{
    for (uint32_t i = 0; i < g_batch.npieces; ++i)
        if (g_batch.pieces[i].group == g)
            batch_each(&g_batch.groups[g].d, &g_batch.pieces[i].inst, 1);
}

/* What's kept, drawn: the opaque groups (each as one draw for its pieces), then the rest in their order, a blended group at its
 * mark there */
static void batch_flush(void)
{
    if (g_batch.flushing || !g_batch_n)
        return;
    g_batch.flushing = 1;
    const int sorted = batch_grow((void**)&g_batch.sorted, &g_batch.sorted_cap, g_batch.npieces, sizeof(GfxInst)) &&
        batch_grow((void**)&g_batch.first, &g_batch.first_cap, g_batch.ngroups + 1, sizeof(uint32_t));
    if (sorted)
    {
        /* the pieces by group (a counting sort: each group's in the order they came) */
        uint32_t at = 0;
        for (uint32_t g = 0; g < g_batch.ngroups; ++g)
            g_batch.first[g] = at, at += g_batch.groups[g].n;
        g_batch.first[g_batch.ngroups] = at;
        for (uint32_t i = 0; i < g_batch.npieces; ++i)
            g_batch.sorted[g_batch.first[g_batch.pieces[i].group]++] = g_batch.pieces[i].inst;
        for (uint32_t g = 0; g < g_batch.ngroups; ++g)
            g_batch.first[g] -= g_batch.groups[g].n;
        for (uint32_t g = 0; g < g_batch.ngroups; ++g)
            if (!g_batch.groups[g].blended)
                batch_send_group(g);
    }
    else /* no room to sort them: each opaque piece as it came */
        for (uint32_t i = 0; i < g_batch.npieces; ++i)
            if (!g_batch.groups[g_batch.pieces[i].group].blended)
                batch_each(&g_batch.groups[g_batch.pieces[i].group].d, &g_batch.pieces[i].inst, 1);
    uint32_t marks = 0;
    for (uint32_t i = 0; i < g_batch.nrest; ++i)
    {
        const uint32_t r = g_batch.rest[i];
        if (!(r & BATCH_GROUP_MARK))
            draw_one(batch_unpack(r));
        else
        {
            const uint32_t g = r & ~BATCH_GROUP_MARK;
            marks++, g_batch.kept_blended += g_batch.groups[g].n;
            if (sorted)
                batch_send_group(g);
            else
                batch_each_unsorted(g);
        }
    }
    g_batch.kept += g_batch.npieces, g_batch.ordered += g_batch.nrest - marks;
    batch_reset();
    g_batch.flushing = 0;
}

/* From gfx_draw: 1 when the draw is kept (sent at the zone's end, or when something can't wait) */
static int batch_take(const GfxDraw* d)
{
    if (!g_batch.open || g_batch.flushing || d->sought || d->ninst)
        return 0; /* (the caster pass's draw nothing into the scene: no order to keep) */
    if (!g_gfx_batch)
    {
        BATCH_SETTLE();
        return 0;
    }
    if (g_batch.npieces + g_batch.nrest >= BATCH_MOST)
        batch_flush();
    int blended = 0;
    const int slot = batch_slot(d, &blended);
    if (slot >= 0 && batch_group(d, slot, blended))
    {
        batch_mark(d);
        g_batch_n++;
        return 1;
    }
    if (blended && slot < 0) /* (the log's: why a blended one wasn't grouped) */
        g_batch.why[batch_why(d)]++;
    /* an opaque draw that writes the depth needs no order among the zone's: drawn now, with nothing sent first (a swaying
     * tree's trunk, from the buffer the game rewrites for each tree, would otherwise send what's kept at every tree) */
    if (!d->pipe.blend && d->depth.zenable && d->depth.zwrite && !d->vs.rhw && !d->zbias && !d->u.pad0[2] && !d->overlay)
        return 0;
    if (batch_can_wait(d) && batch_pack(d))
    {
        batch_mark(d);
        g_batch_n++;
        return 1;
    }
    BATCH_SETTLE(); /* it can't wait: what's kept goes first */
    return 0;
}

/* The pieces after a group's first as shadow casters of their own, from the first's (draw_encode's rec, at index first): its
 * uniforms with each piece's transforms, kept as the first's were; d's own are put back after */
static void batch_casters(GfxDraw* d, uint32_t first, size_t need)
{
    const Caster proto = g_casters[first];
    float m0[48], a0[4];
    memcpy(m0, d->u.wvp, sizeof m0);
    memcpy(a0, d->anchor, sizeof a0);
    for (uint32_t i = 1; i < d->ninst; ++i)
    {
        memcpy(d->u.wvp, d->inst[i].m, sizeof m0);
        memcpy(d->anchor, d->inst[i].anchor, sizeof a0);
        Alloc u = ring_part(need, sizeof(GfxU), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (!u.cpu)
            break;
        memcpy(u.cpu, &d->u, need);
        Caster* c = caster_new(d);
        if (!c)
            break;
        *c = proto;
        c->ub = u.gpu, c->cpu_u = ~0u, c->entry = 0, c->sun_ps = c->sun_fps = c->vn_ps = NULL;
        caster_keep_u(d, c, need);
        c->has_clip0 = c->fixed && (uint8_t)gfx_draw_clip0(d, c->clip0);
        c->snapped = (uint8_t)snap_note(d, c);
    }
    memcpy(d->u.wvp, m0, sizeof m0);
    memcpy(d->anchor, a0, sizeof a0);
}

void gfx_batch(int open)
{
    if (!g_dev)
        return;
    BATCH_SETTLE(); /* (a window that never closed, or the zone's end) */
    g_batch.open = open != 0;
    if (open)
    {
        if (!g_batch.windows++)
            batch_reset();
        return;
    }
    /* now and then, what it did: [recomp] gfx: batch ... */
    if (g_batch.kept && g_serial >= g_batch.said_serial + 3600)
    {
        const uint32_t* w = g_batch.why;
        fprintf(stderr, "[recomp] gfx: batch: the zone's draws of one mesh sent together, over %u zone draws: %u pieces (%u of them "
                        "blended) as %u draws (%u kept in order behind them); blended ones not grouped: kind %u, program %u, "
                        "special %u, fill %u, no anchor %u, prim %u, motion %u, buffers %u ([graphics.effects] batch)\n",
            g_batch.windows, g_batch.kept, g_batch.kept_blended, g_batch.sent, g_batch.ordered, w[BATCH_WHY_KIND],
            w[BATCH_WHY_PROGRAM], w[BATCH_WHY_SPECIAL], w[BATCH_WHY_FILL], w[BATCH_WHY_ANCHOR], w[BATCH_WHY_PRIM],
            w[BATCH_WHY_MOTION], w[BATCH_WHY_BUFFERS]);
        g_batch.said_serial = g_serial, g_batch.kept = g_batch.sent = g_batch.ordered = g_batch.windows = 0;
        g_batch.kept_blended = 0;
        memset(g_batch.why, 0, sizeof g_batch.why);
    }
}
