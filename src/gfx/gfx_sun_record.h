/* The shadows' record (gfx_sun_record; Direct3D 12): while a /vana stats recording runs (core/stats_shadows.hpp), every frame's
 * state of the sun's shadows as a line of logs\stats-shadows-<date>-<time>.csv - the scene the front end handed over (the clock,
 * indoors, the night, the light it picked and its colour, the ambient, the player, the camera, the projection with DLSS's jitter),
 * the light the shadows fell from (after the dusk clamp, the dawn and the tilt), how strong (the cloud's share, the room, the ease
 * in), each map's cascade (its sphere's centre, texel, size, the snap and a hash of its matrix, and whether it was a still layer),
 * the casters (this frame's, the cache's, those left out and why), the caster pass and the zone's own meshes, and the effects'
 * state - and above it every [graphics.effects] key as it stood, with a line for each one changed while it ran.
 *
 * Why: a player's shadows strobed every night on their PC (2026-11-05: Aht Urhgan Whitegate, the Jade Sepulcher) and not on
 * another with the same settings; the flicker watch keeps 1200 frames (trace_gfx.txt) and only the ones before its first line in
 * a quiet minute, so what it caught was the shadows being switched back on, not the strobe. This keeps every frame of the
 * recording, up to max_mb (40 MB: some 25 minutes at 60 frames a second), then says it stopped. Included by gfx_d3d12.c after
 * SunCascade, the shadow cache (gfx_sun_cache.h) and FX_SETTINGS. */

#include <stdio.h>
#include <string.h>

static struct
{
    FILE* f;
    uint64_t bytes, max_bytes, start_ns, rows;
    int capped;
    FxSettings fxs; /* the settings as last written: a change is a line of its own */
    /* this frame's, from where each is worked out (cleared after its row) */
    int light_set, measured, far_set, near_set, far_still, near_still;
    float sw[3], height, day, direct, dl, al;
    float far_c[3], far_texel, far_across, far_ox, far_oy, near_c[3], near_texel, near_across, near_ox, near_oy;
    uint32_t far_hash, near_hash;
} g_srec;

static uint32_t srec_hash(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static float srec_setting(const FxSettings* fxs, size_t i)
{
    float v;
    memcpy(&v, (const char*)fxs + FX_SETTINGS[i].at, sizeof v);
    return v;
}

/* every [graphics.effects] key as it stands, its default after it */
static void srec_settings(FILE* f)
{
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
        fprintf(f, "# graphics.effects %s=%g (default %g)\n", FX_SETTINGS[i].key, srec_setting(&g_fxs, i), FX_SETTINGS[i].def);
}

/* the keys changed since the last row, a line each */
static void srec_changes(void)
{
    if (!memcmp(&g_srec.fxs, &g_fxs, sizeof g_fxs))
        return;
    for (size_t i = 0; i < sizeof FX_SETTINGS / sizeof FX_SETTINGS[0]; ++i)
    {
        const float was = srec_setting(&g_srec.fxs, i), now = srec_setting(&g_fxs, i);
        if (memcmp(&was, &now, sizeof now))
            g_srec.bytes += (uint64_t)fprintf(g_srec.f, "# frame %llu: graphics.effects %s %g -> %g\n", (unsigned long long)g_serial,
                FX_SETTINGS[i].key, was, now);
    }
    g_srec.fxs = g_fxs;
}

static const char SREC_COLUMNS[] =
    "frame,ms,minute,indoors,night,night_clock,warping,own,sun_view_x,sun_view_y,sun_view_z,sun_r,sun_g,sun_b,amb_r,amb_g,amb_b,"
    "sun_draw,cam_draw,player_x,player_y,player_z,player_known,cam_x,cam_y,cam_z,proj_00,proj_11,proj_20,proj_21,scene_w,scene_h,"
    "present_w,present_h,"
    "sun_world_x,sun_world_y,sun_world_z,light_set,light_x,light_y,light_z,light_height,light_day,direct,dl,al,measured,held,"
    "shadows_in,strength,day,"
    "far_set,far_cx,far_cy,far_cz,far_texel,far_across,far_ox,far_oy,far_hash,far_still,near_set,near_cx,near_cy,near_cz,near_texel,"
    "near_across,near_ox,near_oy,near_hash,near_still,sharp_radius,sharp_reach,player_dist,layers_redrawn,why_far,why_sharp,"
    "live,cached,no_pipeline,no_uniform_room,building,casters,cache,cache_listed,copies,pass_only,zone_mesh,pass_age,gate,"
    "ran,history,depth,rays,fog_r,fog_g,fog_b,settings_hash\n";

int gfx_sun_record(const char* path, uint32_t max_mb)
{
    if (g_srec.f)
    {
        fprintf(g_srec.f, "# end: %llu frames, %.1f MB\n", (unsigned long long)g_srec.rows, (double)g_srec.bytes / 1048576.0);
        fclose(g_srec.f);
        g_srec.f = NULL;
    }
    if (!path)
        return 0;
    FILE* f = fopen(path, "w");
    if (!f)
        return 0;
    setvbuf(f, NULL, _IOFBF, 1 << 16);
    memset(&g_srec, 0, sizeof g_srec);
    g_srec.f = f, g_srec.start_ns = gfx_now_ns(), g_srec.fxs = g_fxs;
    g_srec.max_bytes = (uint64_t)(max_mb ? max_mb : 40u) * 1048576ull;
    fprintf(f, "# VanaCore's shadows, a line a frame (Direct3D 12) from frame %llu. Columns: "
               "frame the renderer's frame, ms since the start; minute the Vana'diel clock; own 1 when a lit draw gave the sun; sun_view the "
               "picked light toward it in camera space; proj_20/21 the projection's offsets (DLSS's jitter); sun_world the light in "
               "the world; light_* the shadows' own light after the dusk clamp, dawn and tilt; far_/near_ each map's cascade (hash: "
               "its matrix; still: a still layer's); live/cached/no_pipeline/no_uniform_room the casters drawn and left out\n",
        (unsigned long long)g_serial);
    srec_settings(f);
    fputs(SREC_COLUMNS, f);
    return 1;
}

/* the shadows' light this frame (gfx_sun_light's out) */
static void srec_light(const GfxSunLight* sl, int measured)
{
    memcpy(g_srec.sw, sl->sw, sizeof g_srec.sw);
    g_srec.height = sl->height, g_srec.day = sl->day, g_srec.direct = sl->direct, g_srec.dl = sl->dl, g_srec.al = sl->al;
    g_srec.measured = measured, g_srec.light_set = 1;
}

/* a map's cascade as drawn (sun_map): sharp 0 the far map, 1 the sharp one; still, a still layer's */
static void srec_map(int sharp, const SunCascade* k, int still)
{
    float* c = sharp ? g_srec.near_c : g_srec.far_c;
    memcpy(c, k->c, 12);
    if (sharp)
        g_srec.near_set = 1, g_srec.near_texel = k->texel, g_srec.near_across = k->across, g_srec.near_ox = k->ox, g_srec.near_oy = k->oy,
        g_srec.near_hash = srec_hash(k->S, sizeof k->S), g_srec.near_still = still;
    else
        g_srec.far_set = 1, g_srec.far_texel = k->texel, g_srec.far_across = k->across, g_srec.far_ox = k->ox, g_srec.far_oy = k->oy,
        g_srec.far_hash = srec_hash(k->S, sizeof k->S), g_srec.far_still = still;
}

static int srec_scene(FILE* f, const GfxScene* s, const struct SunTrace* t, const GfxTex* color)
{
    return fprintf(f, "%llu,%.2f,%.2f,%.3f,%.3f,%.3f,%u,%u,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.2f,%.2f,%.2f,%.0f,"
                      "%.2f,%.2f,%.2f,%.5f,%.5f,%.6f,%.6f,%u,%u,%u,%u,",
        (unsigned long long)g_serial, (double)(gfx_now_ns() - g_srec.start_ns) / 1e6, s->vana_minute, s->indoors, s->night,
        s->night_clock, s->warping, t->own, s->sun_dir[0], s->sun_dir[1], s->sun_dir[2], s->sun_color[0], s->sun_color[1],
        s->sun_color[2], s->ambient[0], s->ambient[1], s->ambient[2], s->sun_draw, s->cam_draw, s->focus[0], s->focus[1], s->focus[2],
        s->focus[3], t->cam[0], t->cam[1], t->cam[2], s->proj[0], s->proj[5], s->proj[8], s->proj[9], color->w, color->h,
        s->present_w, s->present_h);
}

static int srec_light_row(FILE* f, const struct SunTrace* t)
{
    return fprintf(f, "%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%d,%u,%.3f,%.3f,%.3f,", t->sun[0], t->sun[1],
        t->sun[2], g_srec.light_set, g_srec.sw[0], g_srec.sw[1], g_srec.sw[2], g_srec.height, g_srec.day, g_srec.direct, g_srec.dl,
        g_srec.al, g_srec.measured, t->held, g_fx.shadows_in, t->strength, t->day);
}

static int srec_maps_row(FILE* f, const struct SunTrace* t)
{
    return fprintf(f, "%d,%.2f,%.2f,%.2f,%.5f,%.1f,%.3f,%.3f,%08x,%d,%d,%.2f,%.2f,%.2f,%.5f,%.1f,%.3f,%.3f,%08x,%d,%.1f,%.1f,%.2f,%u,%u,%u,",
        g_srec.far_set, g_srec.far_c[0], g_srec.far_c[1], g_srec.far_c[2], g_srec.far_texel, g_srec.far_across, g_srec.far_ox,
        g_srec.far_oy, g_srec.far_hash, g_srec.far_still, g_srec.near_set, g_srec.near_c[0], g_srec.near_c[1], g_srec.near_c[2],
        g_srec.near_texel, g_srec.near_across, g_srec.near_ox, g_srec.near_oy, g_srec.near_hash, g_srec.near_still, t->near_r, t->reach,
        t->to, t->redrawn, t->why_far, t->why_near);
}

static int srec_casters_row(FILE* f, const struct SunTrace* t)
{
#ifdef VANACORE
    const int zone_mesh = zonemesh_resident();
#else
    const int zone_mesh = 0;
#endif
    const long long age = g_sought_serial ? (long long)(g_serial - g_sought_serial) : -1;
    return fprintf(f, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%lld,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%08x\n", t->live, t->cached, t->skipped,
        g_fx.tr_nou, g_fx.tr_building, g_ncasters, g_ncache, g_ncache_vis, g_nsnaps, sun_pass_only(), zone_mesh, age, sun_gate(),
        t->ran, t->hist, t->depth, t->rays, t->fog[0], t->fog[1], t->fog[2], srec_hash(&g_fxs, sizeof g_fxs));
}

/* the frame's line (gfx_scene_done, the trace filled in and its counters not yet cleared), then this frame's parts cleared */
static void srec_frame(const GfxScene* s, const struct SunTrace* t, const GfxTex* color)
{
    FILE* f = g_srec.f;
    if (!f || g_srec.capped)
        return;
    srec_changes();
    int n = srec_scene(f, s, t, color);
    n += srec_light_row(f, t);
    n += srec_maps_row(f, t);
    n += srec_casters_row(f, t);
    g_srec.bytes += n > 0 ? (uint64_t)n : 0u;
    if (!(++g_srec.rows % 120))
        fflush(f);
    g_srec.light_set = g_srec.far_set = g_srec.near_set = 0;
    if (g_srec.bytes >= g_srec.max_bytes)
    {
        fprintf(f, "# capped at %.0f MB after %llu frames: the recording went on without the rest\n",
            (double)g_srec.max_bytes / 1048576.0, (unsigned long long)g_srec.rows);
        fflush(f);
        g_srec.capped = 1;
    }
}
