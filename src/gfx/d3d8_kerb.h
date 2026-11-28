/* VanaCore's kerbs: a row of kerb stones laid where a zone's paving changes from one kind to another (the big setts of Upper
 * Jeuno's squares meeting its strips of small ones), where the game puts the two textures edge to edge. From a texture pack
 * ([graphics] textures): kerbs_<zone>.bin, the kerbs as a triangle list in the world, each vertex as the zone's ground has
 * them (FVF 0x152: position, normal, the ground's baked colour there, uv; 36 bytes), after a 16-byte header ("KRB1", the
 * zone, the vertex count, the width); and the kerb's texture beside it, kerbs_<zone>.dds with its normal map (.n.dds) and
 * height map (.h.dds), the paving's own kinds (tools: ground-relief kerbs.py, kerb_tex.py).
 *
 * Drawn as the game would draw them: once a frame, right after the frame's first draw of paving with a height map (a
 * fixed-function draw, lit, MODULATE2X by the vertex colour), with that draw's state - its lights, material, fog and
 * stages - but the kerbs' vertices (from guest memory, as a DrawPrimitiveUP's), the world matrix the identity, stage 0 the
 * kerb's texture and ZBIAS a step nearer. So they go through all a paving draw does: the normal map, the parallax, the
 * sun's shadows. */

static int g_kerb_zone, g_kerb_have = -1;  /* the zone the player is in; the zone whose kerbs are held (-1: look again) */
static char* g_kerb_dir[8];
static int g_nkerb_dir;
static uint32_t g_kerb_tex;                /* the kerb's texture, a texture object of our own (guest pointer) */
static uint32_t g_kerb_mem, g_kerb_cap, g_kerb_n; /* the vertices in guest memory, its size, how many */
static GfxTex *g_kerb_nrm, *g_kerb_hgt;
static uint32_t g_kerb_frame, g_kerb_drawn_frame; /* Presents, and the one whose frame has its kerbs */
static int g_kerb_busy;

static void kerb_add_dir(const char* dir)
{
    if (g_nkerb_dir < (int)(sizeof g_kerb_dir / sizeof g_kerb_dir[0]))
    {
        size_t n = strlen(dir) + 1;
        g_kerb_dir[g_nkerb_dir] = (char*)malloc(n);
        memcpy(g_kerb_dir[g_nkerb_dir++], dir, n);
    }
}

static void kerb_set_zone(int zone)
{
    if (zone != g_kerb_zone)
        g_kerb_zone = zone, g_kerb_have = -1;
}

static void kerb_present(void) { ++g_kerb_frame; }

/* The device lost: the texture object's colour went with the others' (d3d8_device_lost); its maps are ours */
static void kerb_lost(void)
{
    gfx_tex_destroy(g_kerb_nrm), g_kerb_nrm = NULL;
    gfx_tex_destroy(g_kerb_hgt), g_kerb_hgt = NULL;
    Obj* o = obj(g_kerb_tex);
    if (o)
        o->nrm = o->hgt = NULL;
    g_kerb_have = -1;
}

/* The zone's kerbs from the first pack that has them, or none */
static void kerb_load(void)
{
    g_kerb_have = g_kerb_zone;
    g_kerb_n = 0;
    Obj* o = obj(g_kerb_tex);
    if (o)
    {
        gfx_tex_destroy(o->gpu), o->gpu = NULL;
        o->nrm = o->hgt = NULL;
    }
    gfx_tex_destroy(g_kerb_nrm), g_kerb_nrm = NULL;
    gfx_tex_destroy(g_kerb_hgt), g_kerb_hgt = NULL;
    if (!g_kerb_zone)
        return;
    for (int i = 0; i < g_nkerb_dir; ++i)
    {
        char p[1100];
        snprintf(p, sizeof p, "%s%ckerbs_%d.bin", g_kerb_dir[i], plat_path_sep, g_kerb_zone);
        size_t size = 0;
        unsigned char* f = plat_read_file(p, &size);
        if (!f)
            continue;
        uint32_t n = size >= 16 ? (uint32_t)f[8] | f[9] << 8 | f[10] << 16 | (uint32_t)f[11] << 24 : 0;
        if (size < 16 || memcmp(f, "KRB1", 4) || n % 3 || 16 + (size_t)n * 36 > size || !n)
        {
            rt_log("[recomp] kerbs: %s is not a kerbs file\n", p);
            free(f);
            continue;
        }
        if (n * 36 > g_kerb_cap)
        {
            if (g_kerb_mem)
                gheap_free(g_kerb_mem);
            g_kerb_cap = n * 36, g_kerb_mem = gheap_alloc(g_kerb_cap, 1);
        }
        memcpy(GUEST_PTR(g_kerb_mem), f + 16, (size_t)n * 36);
        free(f);
        snprintf(p, sizeof p, "%s%ckerbs_%d.dds", g_kerb_dir[i], plat_path_sep, g_kerb_zone);
        GfxTex* col = load_dds(p);
        if (!col)
            return;
        snprintf(p, sizeof p, "%s%ckerbs_%d.n.dds", g_kerb_dir[i], plat_path_sep, g_kerb_zone);
        g_kerb_nrm = load_dds(p);
        snprintf(p, sizeof p, "%s%ckerbs_%d.h.dds", g_kerb_dir[i], plat_path_sep, g_kerb_zone);
        g_kerb_hgt = g_kerb_nrm ? load_dds(p) : NULL;
        if (!g_kerb_tex)
            g_kerb_tex = obj_new(O_TEXTURE);
        o = obj(g_kerb_tex);
        o->gpu = col, o->repl = NULL, o->nrm = g_kerb_nrm, o->hgt = g_kerb_hgt;
        o->format = FMT_DXT5, o->width = o->height = 256, o->levels = 1, o->dirty = 0;
        g_kerb_n = n;
        rt_log("[recomp] kerbs: zone %d, %u vertices from %s (normal map %s, height map %s)\n", g_kerb_zone, n, g_kerb_dir[i],
            g_kerb_nrm ? "yes" : "no", g_kerb_hgt ? "yes" : "no");
        return;
    }
}

static void draw(uint32_t prim, uint32_t count, uint32_t start, uint32_t indices, uint32_t index_size, uint32_t up_data,
    uint32_t up_stride);

/* After each draw (draw_packet): the first relief paving of the frame brings the kerbs */
static void kerb_after(const GfxDraw* d)
{
    if (!g_relief || g_kerb_busy || g_kerb_drawn_frame == g_kerb_frame || !g_kerb_zone)
        return;
    if (g_kerb_have != g_kerb_zone)
        kerb_load();
    if (!g_kerb_n || !d->fs.pom || d->fs.prog || d->vs.prog || d->vs.rhw || !d->vs.nlights || d->pipe.blend)
        return;
    g_kerb_drawn_frame = g_kerb_frame;
    g_kerb_busy = 1;
    State* s = &g_dev.cur;
    uint32_t vs = s->vs, tex0 = s->tex[0], tex1 = s->tex[1], zbias = s->rs[47], cop1 = s->tss[1][1], cull = s->rs[22];
    float world[16];
    memcpy(world, s->xf[24], sizeof world);
    static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    memcpy(s->xf[24], ident, sizeof ident);
    s->vs = 0x152; /* XYZ | NORMAL | DIFFUSE | TEX1 */
    s->tex[0] = g_kerb_tex, s->tex[1] = 0, s->tss[1][1] = 1; /* stage 1 off */
    s->rs[47] = zbias + 1;
    s->rs[22] = 1; /* CULLMODE none: either side up */
    draw(4 /* TRIANGLELIST */, g_kerb_n / 3, 0, 0, 0, g_kerb_mem, 36);
    memcpy(s->xf[24], world, sizeof world);
    s->vs = vs, s->tex[0] = tex0, s->tex[1] = tex1, s->rs[47] = zbias, s->tss[1][1] = cop1, s->rs[22] = cull;
    g_kerb_busy = 0;
}

/* --- world mappings: a paving laid by the world position instead of the game's own coordinates ---------------------------
 * The game lays a texture on each piece of a zone's floor by that piece's own coordinates, shifted, turned, mirrored or
 * scaled piece by piece (Upper Jeuno's big setts at four sizes, 8 to 31 yalms a repeat): with art of plain rows of stones
 * the joins read as stones sliced through. A pack's <hash>_<w>x<h>.uv gives, per zone, one map of the world to the texture -
 * lines of "zone a b c d": u = a x + b z, v = c x + d z (yalms) - and a fixed-function draw of that texture in that zone has
 * stage 0's coordinates made from its position instead (VanaCore's texture coordinate mode 8: the mesh's position through a
 * texture matrix of the piece's world matrix, then the map). Tools: ground-relief uv_maps.py (what the game's maps are). */
typedef struct UvwEntry
{
    uint64_t hash;
    uint32_t w, h, n;
    int zone[8];
    float m[8][4];
    float rect[8][4]; /* a line's rectangle of the texture (u0 v0 du dv; du 0: the whole of it): the map then in its repeats */
    int flatfade;     /* "flatfade": the game's see-through passes of it drawn without the relief (normal and height maps) */
    int solid;        /* "solid": the game's see-through passes of it drawn opaque - laid by the world, the same stones as below */
} UvwEntry;
static UvwEntry* g_uvw;
static uint32_t g_nuvw;

static void uvw_add(const char* path, uint64_t hash, uint32_t w, uint32_t h)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    UvwEntry e = { hash, w, h, 0, { 0 }, { { 0 } }, { { 0 } }, 0, 0 };
    int z;
    float a, b, c, d, r[4];
    char line[256];
    /* "zone a b c d", or "zone a b c d u0 v0 du dv": that rectangle of the texture alone (an atlas's floor), laid by the world */
    /* ... and "flatfade": where the game lays it as a see-through layer over another floor, faded by its vertices' alpha, that
     * layer drawn flat. Delkfutt's hex ring fades into the plain marble in half its wedges, that layer laid unlike the solid
     * wedges beside it (drawn opaque, its stones overlapped theirs, 2026-11-27); carved, the fade read as ghostly outlines */
    while (e.n < 8 && fgets(line, sizeof line, f))
    {
        if (!strncmp(line, "flatfade", 8))
        {
            e.flatfade = 1;
            continue;
        }
        if (!strncmp(line, "solid", 5)) /* (laid by the world, its faded layer the same stones as the solid one: whole) */
        {
            e.solid = 1;
            continue;
        }
        const int got = sscanf(line, "%d %f %f %f %f %f %f %f %f", &z, &a, &b, &c, &d, &r[0], &r[1], &r[2], &r[3]);
        if (got != 5 && got != 9)
            continue;
        e.zone[e.n] = z, e.m[e.n][0] = a, e.m[e.n][1] = b, e.m[e.n][2] = c, e.m[e.n][3] = d;
        if (got == 9 && r[2] > 0.0f && r[3] > 0.0f)
            memcpy(e.rect[e.n], r, sizeof r);
        e.n++;
    }
    fclose(f);
    if (!e.n && !e.flatfade && !e.solid)
        return;
    UvwEntry* grown = (UvwEntry*)realloc(g_uvw, (g_nuvw + 1) * sizeof *g_uvw);
    if (!grown)
        return;
    g_uvw = grown;
    g_uvw[g_nuvw++] = e;
    rt_log("[recomp] textures: %ux%u %016llx laid by the world in %u zone%s%s (%s)\n", w, h, (unsigned long long)hash, e.n,
        e.n == 1 ? "" : "s", e.flatfade ? ", flat where faded" : e.solid ? ", solid" : "", path);
}

static void uvw_upload(Obj* t, uint64_t hash)
{
    t->uvw = NULL;
    for (uint32_t i = 0; i < g_nuvw && hash; ++i)
        if (g_uvw[i].hash == hash && g_uvw[i].w == t->width && g_uvw[i].h == t->height)
            t->uvw = &g_uvw[i];
}

static int inv4(const float* m, float* o)
{
    float a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c)
            a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0f : 0.0f);
    for (int c = 0; c < 4; ++c)
    {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (fabsf(a[r][c]) > fabsf(a[p][c]))
                p = r;
        if (fabsf(a[p][c]) < 1e-12f)
            return 0;
        for (int k = 0; k < 8; ++k)
        {
            float t = a[c][k];
            a[c][k] = a[p][k], a[p][k] = t;
        }
        float iv = 1.0f / a[c][c];
        for (int k = 0; k < 8; ++k)
            a[c][k] *= iv;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0.0f)
            {
                float f = a[r][c];
                for (int k = 0; k < 8; ++k)
                    a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r * 4 + c] = a[r][c + 4];
    return 1;
}

static void mul4(const float* a, const float* b, float* o)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

static int g_uvw_on, g_uvw_stage, g_uvw_force;
static uint32_t g_uvw_tci, g_uvw_ttf, g_uvw_tci2, g_uvw_ttf2;
static float g_uvw_xf[16], g_uvw_xf2[16];

static void uvw_begin(void)
{
    g_uvw_on = 0;
    State* s = &g_dev.cur;
    if (!g_relief || s->vs != 0x152 || !g_kerb_zone) /* the zone's own fixed-function ground: XYZ | NORMAL | DIFFUSE | TEX1 */
        return;
    const Obj* o = obj(s->tex[0]);
    if (!o || o->kind != O_TEXTURE || !o->uvw)
        return;
    const float* m = NULL;
    const float* rect = NULL;
    for (uint32_t i = 0; i < o->uvw->n && !m; ++i)
        if (o->uvw->zone[i] == g_kerb_zone)
            m = o->uvw->m[i], rect = o->uvw->rect[i][2] > 0.0f ? o->uvw->rect[i] : NULL;
    if (!m)
        return;
    /* row vectors, as D3D: the mesh's [x y z 1] * world * W = [u v 0 1]. From the vertex's own position through the piece's world
     * matrix (texture coordinate mode 8, gfx_hlsl.c): through the camera's (camera space and the view's inverse, as D3D's
     * CAMERASPACEPOSITION), Metalworks' floor slid along as the player walked (2026-11-26) */
    const float W[16] = { m[0], m[2], 0, 0, 0, 0, 0, 0, m[1], m[3], 0, 0, 0, 0, 0, 1 };
    /* the world: the piece's world matrix - or, for a draw through another view than the scene's (the game's drop shadow under
     * a character: the floor under it drawn again round the camera, the view's translation in its world matrix instead), its
     * world and view and the scene view's inverse. Taken as it was, the patch of floor under the player slid along with them
     * (Metalworks' corridor, 2026-11-26) */
    float Wp[16], T[16];
    memcpy(Wp, s->xf[24], sizeof Wp);
    const float* Vs = g_scene.cam && g_scene.s.view[15] != 0.0f ? g_scene.s.view : NULL;
    g_uvw_force = 0;
    /* ... told by its view having no translation while the scene's has (not by the two differing): the scene's view is the
     * frame's first fogged world draw's, the frame before's until then, so pieces drawn before it - the floor under the player,
     * Garlaige - differed from it whenever the camera moved, were "corrected" by the camera's motion since the frame before,
     * and slid off their neighbours' joints while it turned, back in line when it stopped (2026-11-27) */
    const float* V = s->xf[2];
    if (Vs && fabsf(V[12]) + fabsf(V[13]) + fabsf(V[14]) < 1e-4f && fabsf(Vs[12]) + fabsf(Vs[13]) + fabsf(Vs[14]) > 1e-3f)
    {
        g_uvw_force = 1; /* (the drop shadow: a quad round the character, its own 0 to 1 coordinates on the floor's texture) */
        float wv[16], ivs[16];
        if (!inv4(Vs, ivs))
            return;
        mul4(s->xf[24], s->xf[2], wv);
        mul4(wv, ivs, Wp);
    }
    mul4(Wp, W, T);
    /* an atlas's rectangle (GfxFsKey.uvrect): the world's coordinates go out in stage 1's slot (off in these draws) and the pixel
     * function takes them where the game's fall in the rectangle - stage 0 keeps the game's, for the rest of the atlas */
    /* ... in the first coordinate slot the draw leaves unused (its stages in use, up to the first disabled one) and the next: the
     * game's drop shadow under a character draws the floor again with stage 1 its shadow's texture */
    int st = 0;
    if (rect)
    {
        int nst = 0;
        while (nst < 8 && s->tss[nst][1] != 1)
            nst++;
        st = nst < 1 ? 1 : nst;
        if (st + 1 > 7)
            return;
        float R[16] = { 0 };
        R[12] = rect[2], R[13] = rect[3], R[15] = 1.0f; /* the next slot: the rectangle's size, whatever the position */
        T[14] = rect[0], T[15] = rect[1];                /* this one's zw: its corner */
        g_uvw_tci2 = s->tss[st + 1][11], g_uvw_ttf2 = s->tss[st + 1][24];
        memcpy(g_uvw_xf2, s->xf[16 + st + 1], sizeof g_uvw_xf2);
        s->tss[st + 1][11] = 0x80000u, s->tss[st + 1][24] = 4;
        memcpy(s->xf[16 + st + 1], R, sizeof R);
    }
    g_uvw_stage = st;
    g_uvw_tci = s->tss[st][11], g_uvw_ttf = s->tss[st][24];
    memcpy(g_uvw_xf, s->xf[16 + st], sizeof g_uvw_xf);
    s->tss[st][11] = 0x80000u;      /* VanaCore's mode 8: the mesh's own position (gfx_hlsl.c) */
    s->tss[st][24] = rect ? 4 : 2;  /* D3DTTFF_COUNT2, or 4: the rectangle's corner in zw */
    memcpy(s->xf[16 + st], T, sizeof T);
    g_uvw_on = 1;
}

/* the state back as the game set it; d, the draw build_draw just made of it (NULL: none was made) */
static void uvw_end(GfxDraw* d)
{
    if (!g_uvw_on)
        return;
    State* s = &g_dev.cur;
    const int st = g_uvw_stage;
    s->tss[st][11] = g_uvw_tci, s->tss[st][24] = g_uvw_ttf;
    memcpy(s->xf[16 + st], g_uvw_xf, sizeof g_uvw_xf);
    if (st > 0)
    {
        s->tss[st + 1][11] = g_uvw_tci2, s->tss[st + 1][24] = g_uvw_ttf2;
        memcpy(s->xf[16 + st + 1], g_uvw_xf2, sizeof g_uvw_xf2);
    }
    g_uvw_on = 0;
    /* (the game's drop shadows - the floor patch under a character, or under a larger thing as at Whitegate's gates - were drawn
     * with a plain white texture here, against a copy of the stones that drifted while the camera turned; that drift was the
     * stale scene view, fixed in uvw_begin, and the white turned the larger shadows into pale slabs on the ground, 2026-11-27.
     * They are world-laid through their own view now, the same stones as the floor under them, shaded) */
    if (st > 0 && d && !d->vs.prog && !d->fs.prog) /* the rectangle: its two slots made though no stage reads them (build_draw
                                                   * copied their texture matrices, u.texm) */
    {
        if (d->vs.ntex < st + 2)
            d->vs.ntex = (uint8_t)(st + 2);
        d->vs.tci[st] = d->vs.tci[st + 1] = 0x80;
        d->vs.ttf[st] = d->vs.ttf[st + 1] = 4;
        /* the drop shadow's quad under a character (through another view than the scene's): its coordinates are its own, 0 to 1
         * over its square, not the floor's - the whole of it takes the rectangle, not where its coordinates fall (0x80) */
        d->fs.uvrect = (uint8_t)(st | (g_uvw_force ? 0x80 : 0));
    }
}

/* build_draw for a draw packet, with Stone Relief's part (draw_packet): a paving's world mapping round it (uvw_begin, uvw_end),
 * and a texture the pack says is solid (UvwEntry.solid: the game's see-through pass of it drawn opaque) or flat where faded
 * (UvwEntry.flatfade: that pass without its normal and height maps). Whether a draw was made */
static int relief_build_draw(GfxDraw* d)
{
    const Obj* t0 = g_relief ? obj(g_dev.cur.tex[0]) : NULL;
    const UvwEntry* uw = t0 && t0->kind == O_TEXTURE ? t0->uvw : NULL;
    const uint32_t blend0 = g_dev.cur.rs[27];
    const int solid = uw && uw->solid && blend0;
    if (solid)
        g_dev.cur.rs[27] = 0; /* ALPHABLENDENABLE */
    uvw_begin();
    const int built = build_draw(d);
    uvw_end(built ? d : NULL);
    if (solid)
        g_dev.cur.rs[27] = blend0;
    if (built && uw && uw->flatfade && d->pipe.blend && !d->fs.prog)
    {
        d->tex[6] = d->tex[7] = NULL;
        d->fs.st[6].tex = d->fs.st[7].tex = 0;
        d->fs.nmap = 0, d->fs.pom = 0;
    }
    return built;
}
