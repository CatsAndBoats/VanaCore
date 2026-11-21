/* What sun_draw leaves out of a map: a plant with sun_plants 0, and (sun_roll) the casters of the other slices of a layer drawn a
 * slice a frame (gfx_sun_layers.h). gfx_d3d12.c includes this just before sun_draw. It is no file of its own. */
#pragma once

/* n slices, this one's phase; keep: not cleared first (the later ones). A caster is in a slice by its cache entry (the same one as
 * this frame's caster and as the cache's own) in blocks of 16, which keep the runs of one mesh's copies together; one the cache
 * hasn't, by its vertices; a copy (snap), by its place. */
static struct { uint32_t n, phase; int keep; } g_sun_slice;

static int sun_clears(int part)
{
    return part != SUN_MOVING && !g_sun_slice.keep;
}

/* The flicker watch's holes: which of the cache's entries a far layer took (its slices', or the whole draw's), one byte each,
 * cleared when the layer is made (gfx_sun_layers.h layer_holes_note): an entry in the map's reach the layer took through none of
 * its slices is a shadow that went for the frames the layer serves */
static uint8_t* g_slice_mark;
static uint32_t g_slice_mark_n;
static void slice_mark(uint32_t entry)
{
    if (entry == ~0u)
        return;
    if (entry >= g_slice_mark_n)
    {
        const uint32_t n = g_ncache + 1024;
        uint8_t* more = (uint8_t*)realloc(g_slice_mark, n);
        if (!more)
            return;
        memset(more + g_slice_mark_n, 0, n - g_slice_mark_n);
        g_slice_mark = more, g_slice_mark_n = n;
    }
    g_slice_mark[entry] = 1;
}

static int sun_leaves_out(const Caster* cs, const Cached* ce, uint32_t i, uint32_t kept, int chars)
{
    if (cs->plant && !chars && g_fxs.sun_plants == 0.0f)
        return 1;
    if (!g_sun_slice.n)
        return 0;
    const uint32_t key = ce ? (i >= kept ? 0x40000000u + (i - kept) : (uint32_t)(ce - g_cache))
        : cs->entry ? cs->entry - 1 : (uint32_t)(cs->vb[0] >> 6) + (uint32_t)cs->vstart;
    return (key >> 4) % g_sun_slice.n != g_sun_slice.phase;
}
