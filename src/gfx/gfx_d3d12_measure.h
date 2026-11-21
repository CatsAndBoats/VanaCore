/* gfx_d3d12.c include unit: the composite's features measured (vanacore_gfx_measure): cf_off, comp_measure. Included
 * once, inside its VANACORE block, where gfx_d3d12.c had them */
/* The composite's uniforms with feature v off (CF_ALL: as they are; CF_NONE: every one off), as its shader tests them;
 * 0 when it's off already */
static int cf_off(FxU* u, int v)
{
    switch (v)
    {
    case CF_ALL:
        return 1;
    case CF_SHADE:
        if (!(u->ao[1] > 0.0f || u->shadow[0] > 0.0f || u->smap[0] > 0.0f))
            return 0;
        u->ao[1] = u->shadow[0] = u->smap[0] = 0.0f;
        return 1;
    case CF_LINES:
        if (!(u->toon[0] > 0.0f))
            return 0;
        u->toon[0] = 0.0f;
        return 1;
    case CF_CREASE:
        if (!(u->toon[0] > 0.0f && u->toon[3] > 0.0f))
            return 0;
        u->toon[3] = 0.0f;
        return 1;
    case CF_BANDS:
        if (!(u->toon2[1] > 0.0f))
            return 0;
        u->toon2[1] = 0.0f;
        return 1;
    case CF_FOG:
        if (!(u->fogc[3] > 0.0f))
            return 0;
        u->fogc[3] = 0.0f;
        return 1;
    case CF_ZONE:
        if (!(u->zlc[0] > 0.0f))
            return 0;
        u->zlc[0] = 0.0f;
        return 1;
    case CF_BLOOM:
        if (!(u->bloom[1] > 0.0f))
            return 0;
        u->bloom[1] = 0.0f;
        return 1;
    case CF_RAYS:
        if (!(u->rays[0] > 0.0f && u->sunuv[2] > 0.0f))
            return 0;
        u->rays[0] = 0.0f;
        return 1;
    case CF_NONE:
        for (int k = CF_SHADE; k < CF_NONE; ++k)
            cf_off(u, k);
        return 1;
    }
    return 0;
}

/* The composite's features measured (vanacore_gfx_measure): its shader drawn again into a target of its own, after the
 * real one, with the frame's feature off - one a frame, in turn, skipping those off already - its time GS_MEASURE's. A
 * later scene in the frame is measured with the same one off, so a frame's measure is all its composites', as the
 * composite's own time is. */
static void comp_measure(GfxTex* color, int fmt, const float vp[4], const FxU* u, const int32_t* in, int n)
{
    Frame* f = &g_frames[g_frame];
    if (!gfx_profiling || !g_sq || !f->timed || f->closed || g_serial >= g_meas.until)
    {
        if (g_fx.meas.res && g_serial > g_meas.until + 120) /* stopped a while ago: its target let go */
        {
            defer((IUnknown*)g_fx.meas.res, NULL, -1);
            defer(NULL, &g_srv, g_fx.meas.srv);
            defer(NULL, &g_rtv, g_fx.meas.rtv);
            memset(&g_fx.meas, 0, sizeof g_fx.meas);
        }
        return;
    }
    if (!fx_tex(&g_fx.meas, color->dxfmt, color->w, color->h))
        return;
    FxU m = *u;
    if (f->measured < 0)
    {
        for (int k = 0; k < CF_N; ++k)
        {
            const int v = (g_meas.next + k) % CF_N;
            m = *u;
            if (cf_off(&m, v))
            {
                f->measured = v;
                break;
            }
            g_meas.off[v] = g_serial;
        }
        g_meas.next = (f->measured + 1) % CF_N;
        g_meas.at[f->measured] = g_serial;
    }
    else
        cf_off(&m, f->measured);
    const int scope = gpu_scope_cur(), part = gpu_part_cur();
    fx_state(&g_fx.meas, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu_scope(GS_MEASURE);
    fx_draw(heap_cpu(&g_rtv, g_fx.meas.rtv), g_fx.meas.w, g_fx.meas.h, g_fx.by_fmt[fmt].comp, vp, &m, in, n, 0, 0);
    gpu_scope(scope);
    gpu_part(part);
}
