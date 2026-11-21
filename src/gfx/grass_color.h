/* What colours are grass (split out of grass.c): green grass, and the yellow-olive of dry grass (grass.dry). Pure functions of a texel,
 * inline as they were: grass.c's tex_sort runs them over every texel of a texture and ground_ok over every sample of a path's edge. */
#pragma once
#include <stdint.h>

/* green grass: green over red and blue (West Ronfaure's grass is about 75, 95, 50; its dirt paths 82, 76, 65) */
static inline int green_ok(const uint8_t rgb[3])
{
    return rgb[1] >= 28 && rgb[1] * 100 >= rgb[0] * 104 && rgb[1] * 100 >= rgb[2] * 122;
}

/* the yellow-olive of dry grass (grass.dry): a hue of 46 to 70 degrees, not grey; its saturation ((max - min) / max) in *sat. East
 * Sarutabaruta's grass is about 120, 111, 73 (48 degrees, saturation 0.39), half its texels past 46; its dirt and tan ground under
 * 42, as Ronfaure's paths (39). The olive-grey dirt of some paths is in these hues too, only greyer (about 0.25): which of the two
 * a texture holds is tex_sort's, from all its colours in these hues together */
static inline int dry_ok(const uint8_t rgb[3], float* sat)
{
    const int r = rgb[0], gr = rgb[1], b = rgb[2], mx = r > gr ? (r > b ? r : b) : (gr > b ? gr : b), mn = r < gr ? (r < b ? r : b) : (gr < b ? gr : b);
    if (mx < 28 || (mx - mn) * 100 < mx * 15 || b == mx)
        return 0;
    /* the hue, from red (0) through yellow (60) to green (120): red highest, 60 (g - b) / (mx - mn); green highest, 60 (2 + (b - r) /
     * (mx - mn)) */
    const int hue = mx == r ? 60 * (gr - b) / (mx - mn) : 120 + 60 * (b - r) / (mx - mn);
    if (hue < 46 || hue > 70)
        return 0;
    *sat = (float)(mx - mn) / (float)mx;
    return 1;
}
