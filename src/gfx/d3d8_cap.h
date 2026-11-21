/* The draw log ([graphics] drawlog, g_cap): one draw's line - its states, textures, shaders, where its vertices come from and
 * its box. d3d8.c includes this just before the scene's end (scene_note), after obj, surface_gpu, static_buffer and the
 * device state, whose names it uses. It is no file of its own. */
#pragma once

static void cap_draw(GfxDraw* d, uint32_t prim, uint32_t count, uint32_t first, uint32_t n, uint32_t up_data,
    uint32_t up_stride)
{
    FILE* f = g_cap;
    const Obj* crt = obj(g_dev.rt);
    fprintf(f, "#%u prim %u count %u n %u vs %x rhw %d rt %s %ux%u %08x vp %u,%u %ux%u", g_cap_n++, prim, count, n, g_dev.cur.vs,
        d->vs.rhw, g_dev.rt == g_dev.backbuffer ? "bb" : "off", crt ? crt->width : 0, crt ? crt->height : 0, g_dev.rt, d->vp[0],
        d->vp[1], d->vp[2], d->vp[3]);
    const uint32_t* crs = g_dev.cur.rs;
    const uint32_t* ct = g_dev.cur.tss[0];
    fprintf(f, " atest %u func %u ref %u blend %u %u/%u op %u tss0 c%u(%x,%x) a%u(%x,%x) tss1 c%u", crs[15], crs[25], crs[24],
        crs[27], crs[19], crs[20], crs[171], ct[1], ct[2], ct[3], ct[4], ct[5], ct[6], g_dev.cur.tss[1][1]);
    Obj* t = obj(g_dev.cur.tex[0]); /* (texture_hash keeps the hash in it) */
    if (t)
        fprintf(f, " tex %08x %ux%u fmt %u", g_dev.cur.tex[0], t->width, t->height, t->format);
#ifdef VANACORE
    if (t) /* its content's hash: what [graphics.effects] elight_lantern_tex lists */
        fprintf(f, " hash %016llx", (unsigned long long)texture_hash(t));
#endif
    fprintf(f, " tci0 %x ttf0 %x", ct[11], ct[24]);
    /* what stages 0 and 1 sample as drawn: 1 a 2D texture, 2 a cube, 0 nothing (opaque white) */
    fprintf(f, " sampled %u %u", d->fs.st[0].tex, d->fs.st[1].tex);
    /* the later stages that are on: their ops, texture, coordinates (index | generation) and transform flags */
    for (int st = 1; st < 4 && g_dev.cur.tss[st][1] != 1; ++st)
    {
        const uint32_t* s = g_dev.cur.tss[st];
        const Obj* to = obj(g_dev.cur.tex[st]);
        fprintf(f, " | s%d c%u(%x,%x) a%u(%x,%x) tci %x ttf %x tex %08x %ux%u", st, s[1], s[2], s[3], s[4], s[5], s[6], s[11], s[24],
            g_dev.cur.tex[st], to ? to->width : 0, to ? to->height : 0);
    }
    static const char* const SOUGHT[2] = { "", " sought 1" }; /* the caster pass's: the sun's maps alone (seek_note) */
    fprintf(f, " z %u/%u zbias %d fog %u/%u lit %u caster %u%s", d->depth.zenable, d->depth.zwrite, (int)d->zbias, d->fs.fog,
        d->vs.fog_vertex, d->vs.lighting, d->caster, SOUGHT[d->sought != 0]);
    {
        /* where the vertices come from: the draw's own memory (UP), or the position stream's buffer, a
         * dynamic one or a static one (and whether it was written again since the game made it) */
        const Obj* vb = up_data ? NULL : obj(g_dev.cur.stream[d->vs.el[GFX_R_POSITION].stream]);
        fprintf(f, " src %s", up_data ? "up" : !vb ? "none" : (vb->usage & USAGE_DYNAMIC) ? "dynamic" : "static");
        if (d->water) /* a water model's (water_model): 1 a level surface, 2 the rest */
            fprintf(f, " water %u", d->water);
        if (d->fs.pool) /* after the effects, the player light's pool laid in its own pixel function (late_pool) */
            fprintf(f, " pool 1");
        if (d->fs.hfog) /* ... and the height fog (late_pool): 1 toward its colour, 2 taken off an added light, 3 premultiplied */
            fprintf(f, " hfog %u", d->fs.hfog);
        if (d->el_lantern) /* the light from the effects' part: 1 a lantern's flame, 2 a dimmed effect, 3 a character's effect */
            fprintf(f, " el %u", d->el_lantern);
        if (d->dlss5_skip) /* not one of DLSS 5's lights (dlss5_skip_tex) */
            fprintf(f, " dlss5skip");
    }
    if (!d->vs.rhw)
    {
        const float* P = g_dev.cur.xf[3];
        const float* V = g_dev.cur.xf[2];
        const float* W = g_dev.cur.xf[24];
        fprintf(f, "\n  proj %.4f %.4f %.4f %.4f view t %.2f %.2f %.2f world t %.2f %.2f %.2f", P[0], P[5], P[8], P[9],
            V[12], V[13], V[14], W[12], W[13], W[14]);
        if (d->vs.lighting)
        {
            /* the material (diffuse, ambient, specular, emissive, power), the colour sources and the viewer */
            const uint32_t* m = g_dev.cur.mat;
            fprintf(f, "\n  mat d %.2f %.2f %.2f %.2f a %.2f %.2f %.2f %.2f s %.2f %.2f %.2f e %.2f %.2f %.2f p %.1f"
                       " colorvertex %u src d%u s%u a%u e%u localviewer %u specular %u",
                u2f(m[0]), u2f(m[1]), u2f(m[2]), u2f(m[3]), u2f(m[4]), u2f(m[5]), u2f(m[6]), u2f(m[7]), u2f(m[8]), u2f(m[9]),
                u2f(m[10]), u2f(m[12]), u2f(m[13]), u2f(m[14]), u2f(m[16]), crs[141], crs[145], crs[146], crs[147], crs[148],
                crs[142], crs[29]);
        }
        fprintf(f, "\n  tfactor %08x", crs[60]);
        /* each stage's texture transform that's on: its matrix's rows */
        for (int st = 0; st < 4 && g_dev.cur.tss[st][1] != 1; ++st)
        {
            if (!(g_dev.cur.tss[st][24] & 7))
                continue;
            const float* M = g_dev.cur.xf[16 + st];
            fprintf(f, " texm%d %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f", st, M[0], M[1], M[2],
                M[3], M[4], M[5], M[6], M[7], M[8], M[9], M[10], M[11], M[12], M[13], M[14], M[15]);
        }
    }
    fprintf(f, "\n  stack");
    for (uint32_t a = g_cap_esp, k = 0; a < g_cap_esp + 0x600 && k < 12; a += 4)
    {
        uint32_t v = rd32(a);
        if (v < rt_image_lo + 6 || v >= rt_image_hi)
            continue;
        if (rd8(v - 5) == 0xE8 || rd8(v - 6) == 0xFF || rd8(v - 3) == 0xFF || rd8(v - 2) == 0xFF)
            fprintf(f, " %08x", v), ++k;
    }
    fprintf(f, "\n");
    uint32_t base, stride, size;
    if (up_data)
        base = up_data, stride = up_stride, size = 0xFFFFFFFFu;
    else
    {
        uint32_t st = d->vs.el[GFX_R_POSITION].stream;
        const Obj* b = obj(g_dev.cur.stream[st]);
        if (!b || !b->mem)
            return;
        base = b->mem, stride = g_dev.cur.stride[st], size = b->size;
    }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t at = (first + i) * stride + (uint32_t)d->u.offset[GFX_R_POSITION];
        if (at > size - 12)
            break;
        for (int c = 0; c < 3; ++c)
        {
            float x = u2f(rd32(base + at + 4 * c));
            lo[c] = x < lo[c] ? x : lo[c];
            hi[c] = x > hi[c] ? x : hi[c];
        }
        if (i < 4 && g_dev.cur.vs == 0x144 && stride == 28) /* XYZRHW, diffuse, one texture: with its UV */
            fprintf(f, "  v%u %.2f %.2f %.4f uv %.5f %.5f diffuse %08x\n", i, u2f(rd32(base + at)), u2f(rd32(base + at + 4)),
                u2f(rd32(base + at + 8)), u2f(rd32(base + at + 20)), u2f(rd32(base + at + 24)), rd32(base + at + 16));
        else if (i < 4 && g_dev.cur.vs == 0x44 && stride == 20) /* XYZRHW and diffuse, no texture */
            fprintf(f, "  v%u %.2f %.2f %.4f diffuse %08x\n", i, u2f(rd32(base + at)), u2f(rd32(base + at + 4)),
                u2f(rd32(base + at + 8)), rd32(base + at + 16));
        else if (i < 4)
            fprintf(f, "  v%u %.2f %.2f %.4f\n", i, u2f(rd32(base + at)), u2f(rd32(base + at + 4)),
                u2f(rd32(base + at + 8)));
    }
    fprintf(f, "  box %.2f..%.2f  %.2f..%.2f  %.4f..%.4f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
}
