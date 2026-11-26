/* d3d8_water_tris.h: an include unit of d3d8_water.h (in its scope: g_water_set, gfx_draw). The water's triangles debug view
 * ([graphics.effects] debug=34, g_water_set.tris): every draw of VanaCore's water, each triangle a flat colour of its own
 * (by its primitive id) shaded by its face's slope, then its triangles' edges over it in wireframe. The flag rides in texture
 * matrix 2's byte 15 (free in both the mirror's and the rain's use of the matrix: water_mirror_bind, rain_water_bind), read by
 * the pixel function's tail (gfx_hlsl.c emit_fs_tail): 1 the flat colour, 2 the edges' colour */
#pragma once

#define WATER_TRIS_BIAS 2 /* the edges' depth bias toward the camera (D3D8 ZBIAS steps): drawn over their own triangles */

/* d drawn: as it is, or with the triangles view on, flat and then its edges */
static void water_tris_draw(GfxDraw* d)
{
    if (!g_water_set.tris || !d->fs.water)
    {
        gfx_draw(d);
        return;
    }
    const float was = d->u.texm[2][15];
    d->u.texm[2][15] = 1.0f;
    gfx_draw(d);
    static GfxDraw w; /* (a GfxDraw is several kilobytes: not on the stack) */
    w = *d;
    w.fill = 2; /* WIREFRAME */
    w.u.texm[2][15] = 2.0f;
    w.pipe.blend = 0;
    w.depth.zenable = 1, w.depth.zwrite = 0, w.depth.zfunc = 4; /* LESSEQUAL */
    w.zbias = WATER_TRIS_BIAS;
    gfx_draw(&w);
    d->u.texm[2][15] = was;
}
