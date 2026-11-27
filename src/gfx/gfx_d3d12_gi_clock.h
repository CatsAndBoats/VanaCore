/* The bounce light off at night: an include unit of gfx_d3d12.c (its g_fxs and GfxScene), kept out of it for its length.
 *
 * gi_by_clock: the bounce light's share by the Vana'diel clock ([graphics.effects] gi_night 1, else whole): eased out from
 * 17:00 to 18:00 and back in from 05:30 to 06:30, none between. Outdoors only: where the zone is lit by a fixed light
 * (GfxScene.indoors) it stays whole. The clock's 0 is "not known" (midnight too, for a game minute), so the share last worked
 * out is kept through it. */
#pragma once

static float gi_by_clock(const GfxScene* s)
{
    static float share = 1.0f;
    const float m = s->vana_minute;
    if (m > 0.0f)
    {
        const float t = fminf(fmaxf(m >= 720.0f ? (1080.0f - m) / 60.0f : (m - 330.0f) / 60.0f, 0.0f), 1.0f);
        share = t * t * (3.0f - 2.0f * t);
    }
    if (g_fxs.gi_night <= 0.0f)
        return 1.0f;
    const float in = fminf(fmaxf(s->indoors, 0.0f), 1.0f);
    const float v = share + (1.0f - share) * in;
    gfx_watch_value("bounce light: share by the clock", v);
    return v;
}
