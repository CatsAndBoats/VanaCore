/* gfx_d3d12.c's, included once there: mo_draw's tests of where a draw is on the screen, out of it for its length (tools\lint
 * filelen) */
#pragma once

/* an effect's model round the camera or over half the screen (a monster's 2-hour aura, Absolute Virtue's): its vertices this
 * frame (up to 256 of them, spread over the draw) through its transform, any behind the camera or their box on the screen
 * half of it or more. Given the camera's motion at its own depth (fg_model), most of the picture took it, and DLSS and frame
 * generation shook the whole screen while the aura lasted (Fluff, 2026-11-24) */
static int mo_fills_screen(const GfxDraw* d)
{
    const GfxElem* pe = &d->vs.el[GFX_R_POSITION];
    if (!pe->used || pe->stream >= GFX_NSTREAMS || (pe->type != GFX_FLOAT3 && pe->type != GFX_FLOAT4))
        return 0;
    const uint8_t* cpu = (const uint8_t*)d->cpu[pe->stream];
    const uint32_t bytes = d->cpu_size[pe->stream], stride = (uint32_t)d->u.stride[pe->stream];
    const uint32_t at = (uint32_t)d->u.offset[GFX_R_POSITION];
    if (!cpu || !stride || bytes < at + 12)
        return 0;
    const uint32_t n = (bytes - at - 12) / stride + 1, step = n > 256 ? n / 256 : 1;
    const float* m = d->u.wvp;
    float lo[2] = { 1.0f, 1.0f }, hi[2] = { -1.0f, -1.0f };
    for (uint32_t i = 0; i < n; i += step)
    {
        float p[3], c[4];
        memcpy(p, cpu + at + (size_t)i * stride, 12);
        for (int j = 0; j < 4; ++j)
            c[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
        if (!(c[3] > 1e-5f))
            return 1; /* (behind the camera, or no number: round it) */
        for (int k = 0; k < 2; ++k)
        {
            const float v = fminf(fmaxf(c[k] / c[3], -1.0f), 1.0f);
            lo[k] = fminf(lo[k], v), hi[k] = fmaxf(hi[k], v);
        }
    }
    return hi[0] > lo[0] && hi[1] > lo[1] && (hi[0] - lo[0]) * (hi[1] - lo[1]) >= 2.0f; /* (the screen's 4 in clip space) */
}

/* a vertex at clip-space a now and b the frame before a quarter of the screen or more apart: not the same draw found (two alike
 * that swapped places in the frame's order), whatever moved */
static int mo_far_clip(const float* a, const float* b)
{
    if (a[3] <= 1e-5f || b[3] <= 1e-5f)
        return 0; /* behind the camera, either: nothing to tell by */
    const float dx = a[0] / a[3] - b[0] / b[3], dy = a[1] / a[3] - b[1] / b[3];
    return dx * dx + dy * dy > 0.25f;
}

/* ... a vertex at p now (through now_m) and at q the frame before (through was_m) */
static int mo_far(const float* p, const float* q, const float* now_m, const float* was_m)
{
    float a[4], b[4];
    for (int j = 0; j < 4; ++j)
    {
        a[j] = p[0] * now_m[j] + p[1] * now_m[4 + j] + p[2] * now_m[8 + j] + now_m[12 + j];
        b[j] = q[0] * was_m[j] + q[1] * was_m[4 + j] + q[2] * was_m[8 + j] + was_m[12 + j];
    }
    return mo_far_clip(a, b);
}
