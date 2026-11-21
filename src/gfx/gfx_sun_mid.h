/* The sun's middle map ([graphics.effects] sun_mid): gfx_d3d12.c includes this just before sun_map, after gfx_sun_layers.h and
 * sun_hold, whose names it uses. It is no file of its own.
 *
 * The far map's 4096 texels are spread over twice sun_distance round the player: some 15 cm a texel at 300, 7 at 150, against the
 * sharp map's centimetre over its 25 units. Past the sharp map every cliff and crag a few tens of units off was shaded by the far
 * map's coarse texels, a toothed edge where the sharp map had a clean one (the Crag of Holla, La Theine Plateau, 2026-11-20).
 * sun_mid puts a third 4096 map between them, out to sun_mid units round the player (at 80, some 4 cm a texel). fx_ao looks in it
 * (input 7, u.lmatm) past the sharp map's edge and in the far map past its own, each blended across the edge of the one inside it
 * (sun_cascades).
 *
 * It's made as the far map is: its still casters drawn into a layer of their own (g_still_mid, g_fx.smapms) every sun_far_hold + 1
 * frames, or when the player has moved a 20th of its radius (still_stale; with sun_roll a slice a frame), and each frame the map
 * starts as a copy of the layer, this frame's moving casters drawn over it. Its casters are culled by the far map's footprint
 * (g_sun_cull, set by sun_map), as the sharp map's are, so it never lacks one the far map has. It costs about what the far map
 * does: a 64 MB map, its 64 MB layer (and 64 more for sun_roll's back map), the copy and the moving casters each frame, the
 * layer's redraw every few frames. Direct3D 12 alone (Direct3D 11's u.smapm2.y stays 0). */
#pragma once

/* the middle map's memory let go (sun_mid off): its three 64 MB maps */
static void sun_mid_release(void)
{
    g_still_mid.serial = 0;
    if (g_fx.smapm.res)
        sun_release(&g_fx.smapm);
    if (g_fx.smapms.res)
        sun_release(&g_fx.smapms);
    if (g_fx.smapmsb.res)
        sun_release(&g_fx.smapmsb);
}

/* The middle map drawn and its uniforms filled (u->smapm2.y 1), or none (0): sun_mid off, no wider than the sharp map's reach
 * (x->reach, when there's a sharp map), or reaching nine tenths of the far map's or more (dfar), where it would only take its
 * place. gate: sun_gate's bits (SG_HOLD draws it whole every frame, as the far map) */
static void sun_mid_map(const SunCtx* x, float dfar, uint32_t gate, FxU* u)
{
    u->smapm2[1] = 0.0f;
    const float dmid = g_fxs.sun_mid;
    const int sharp = g_fxs.sun_sharp != 0.0f && !(gate & SG_NEAR);
    if (dmid <= 0.0f || dmid >= 0.9f * dfar || (sharp && dmid <= x->reach))
    {
        sun_mid_release();
        return;
    }
    SunCascade cmid;
    sun_fit(x->s, x->invV, x->L, 0.5f, dmid, SUN_MAP, 1.0f, &cmid);
    if (!sun_target(&g_fx.smapm, SUN_MAP))
        return;
    gpu_part(GP_SUN_FAR); /* (counted with the far map's) */
    const SunCascade* km = &cmid;
    const uint32_t hold = sun_hold(gate, g_fxs.sun_far_hold, 15.0f);
    if (hold && sun_target(&g_fx.smapms, SUN_MAP))
    {
        wide_layer(&WIDE_MID, x, &cmid, dmid, hold);
        km = &g_still_mid.k;
        sun_state(&g_fx.smapms, D3D12_RESOURCE_STATE_COPY_SOURCE);
        sun_state(&g_fx.smapm, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyResource(list(), g_fx.smapm.res, g_fx.smapms.res);
        sun_draw(&g_fx.smapm, x->s, x->invP, x->invV, km, 1, 0, SUN_MOVING);
        g_still_mid.seen = g_serial;
    }
    else
    {
        g_still_mid.serial = 0;
        if (!hold && g_fx.smapms.res)
            sun_release(&g_fx.smapms), sun_release(&g_fx.smapmsb); /* their 64 MB back while it's off */
        sun_draw(&g_fx.smapm, x->s, x->invP, x->invV, &cmid, 1, 0, SUN_ALL);
    }
    gpu_part(GP_REST);
    mat_mul(u->lmatm, x->invV, km->S); /* this frame's view space to the map (for the cascade drawn) */
    u->smapm[0] = km->texel, u->smapm[1] = km->bias, u->smapm[2] = km->soft, u->smapm[3] = km->slope;
    u->smapm2[0] = km->range, u->smapm2[1] = 1.0f;
}
