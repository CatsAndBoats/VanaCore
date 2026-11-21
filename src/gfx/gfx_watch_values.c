/* The flicker watch's toggle finder (split out of gfx_d3d12.c, whose gfx_watch it says its finds through): one for both back ends,
 * the frame it goes by is the one that's drawing (gfx_frame goes to the selected back end). */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gfx.h"

/* The toggle finder: the values a frame is made from, each named by a string literal and fed once a frame (gfx_watch_value),
 * while profiling. One that turns round WATCH_TURNS times within WATCH_SPAN frames (up, down, up: a step small against the
 * value is no move, and a ramp up or down has no turn) is said once a window, with its last WATCH_RING frames, 0 the lowest
 * of them and 9 the highest: a flicker that comes and goes is a value that goes to and fro with it. */
#define WATCH_VALUES 192
#define WATCH_SPAN 180
#define WATCH_TURNS 3
#define WATCH_RING 64
static struct WatchValue
{
    const char* name;
    float cur, lo, hi;
    int dir;                /* the way it last moved: 1 up, -1 down, 0 not yet */
    uint32_t turns, n;      /* the turns this window; the values fed since it was first */
    uint64_t window, fed;   /* the frame the window began at; the frame last fed */
    float ring[WATCH_RING]; /* the last values */
} g_wv[WATCH_VALUES];
static uint32_t g_wv_n;

static void watch_value_say(const struct WatchValue* w)
{
    const uint32_t k = w->n < WATCH_RING ? w->n : WATCH_RING;
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t i = 0; i < k; ++i)
    {
        const float v = w->ring[(w->n - k + i) % WATCH_RING];
        lo = fminf(lo, v), hi = fmaxf(hi, v);
    }
    char shape[WATCH_RING + 1];
    for (uint32_t i = 0; i < k; ++i)
    {
        const float v = w->ring[(w->n - k + i) % WATCH_RING];
        shape[i] = hi - lo < 1e-6f ? '.' : (char)('0' + (int)(9.0f * (v - lo) / (hi - lo) + 0.5f));
    }
    shape[k] = 0;
    gfx_watch("%s: went to and fro, %u turns in %u frames, between %g and %g; the last %u frames: %s", w->name, w->turns,
        WATCH_SPAN, w->lo, w->hi, k, shape);
}

void gfx_watch_value(const char* name, float v)
{
    if ((!gfx_profiling && !gfx_watching) || !name || v != v)
        return;
    struct WatchValue* w = NULL;
    for (uint32_t i = 0; i < g_wv_n; ++i)
        if (g_wv[i].name == name || !strcmp(g_wv[i].name, name))
        {
            w = &g_wv[i];
            break;
        }
    const uint64_t f = gfx_frame();
    if (!w)
    {
        if (g_wv_n >= WATCH_VALUES)
            return;
        w = &g_wv[g_wv_n++];
        memset(w, 0, sizeof *w);
        w->name = name, w->cur = w->lo = w->hi = v, w->window = f;
    }
    if (w->n && w->fed == f) /* fed twice in a frame: the later one */
    {
        w->ring[(w->n - 1) % WATCH_RING] = v, w->cur = v;
        return;
    }
    if (f >= w->window + WATCH_SPAN)
    {
        if (w->turns >= WATCH_TURNS)
            watch_value_say(w);
        w->window = f, w->turns = 0, w->dir = 0, w->lo = w->hi = w->cur;
    }
    const float step = v - w->cur;
    if (fabsf(step) > 1e-3f * (1.0f + fabsf(w->cur)))
    {
        const int dir = step > 0.0f ? 1 : -1;
        if (w->dir && dir != w->dir)
            w->turns++;
        w->dir = dir;
        w->lo = fminf(w->lo, v), w->hi = fmaxf(w->hi, v);
    }
    w->cur = v;
    w->ring[w->n % WATCH_RING] = v;
    w->n++, w->fed = f;
}
