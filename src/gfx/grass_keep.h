/* The tufts' quads as a square keeps them: keep_blades, keep_sprite and keep_flower (kvert each corner), standing on the slope
 * of the ground under them (set_lean, lean), and the colour a tuft takes from the ground round it (tuft_colour). grass.c includes
 * this just after kept_room, after g, g_kept, g_cover_tint, KVert, GWTri, GTex, FLOWERS, BLADE_VARIANTS, pack_color, tex_near
 * and shade_tint, whose names it uses. It is no file of its own. */
#pragma once

static KVert kvert(float x, float y, float z, uint32_t col, float u, float v, const float root[3], uint32_t keep)
{
    const KVert k = { x, y, z, g_cover_tint ? g_cover_tint : col, u, v, root[0], root[1], root[2], keep };
    return k;
}

/* how far the ground under the tuft being made falls (draw space, y down) a unit along x and along z: its triangle's slope (build_step).
 * A tuft's quads follow it, each end of a quad as far up or down as the ground there: standing level on a hillside, a quad 0.8 wide
 * hung its downhill end clear of the ground by a third of its height, its blades' bases a straight edge in the air, and sank its
 * uphill end into the ground, the blades cut off along a line (Batallia Downs' hills) */
static float g_lean[2];

static float lean(float dx, float dz) { return g_lean[0] * dx + g_lean[1] * dz; }

/* g_lean for triangle tri's tufts: from its plane, at most 50 degrees' */
static void set_lean(const GWTri* tri)
{
    const float* p0 = tri->p[0];
    const float e1[3] = { tri->p[1][0] - p0[0], tri->p[1][1] - p0[1], tri->p[1][2] - p0[2] },
                e2[3] = { tri->p[2][0] - p0[0], tri->p[2][1] - p0[1], tri->p[2][2] - p0[2] };
    const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
    g_lean[0] = g_lean[1] = 0.0f;
    if (ny != 0.0f)
        g_lean[0] = -nx / ny, g_lean[1] = -nz / ny;
    const float steep = sqrtf(g_lean[0] * g_lean[0] + g_lean[1] * g_lean[1]);
    if (steep > 1.2f)
        g_lean[0] *= 1.2f / steep, g_lean[1] *= 1.2f / steep;
}

/* a tuft's colour at (uv) on map tx, its own texel in texel: the ground's round it, as it's seen; in shadow all round (too dark to
 * read as any colour), shade_tint's of its texel */
static void tuft_colour(const GTex* tx, const float uv[2], uint8_t texel[3], int dry)
{
    uint8_t seen[3];
    if (tex_near(tx, uv[0], uv[1], seen) && (seen[0] >= 28 || seen[1] >= 28 || seen[2] >= 28))
        memcpy(texel, seen, sizeof seen);
    else
        shade_tint(tx, texel, dry);
}

/* one tuft of the drawn blades of asset ai (0 type 1, 1 type 2): two crossed quads standing on p, h tall, w wide; col its colour */
static void keep_blades(int ai, const float p[3], float yaw, float h, float w, const float col[3], uint32_t keep, uint32_t pick)
{
    KVert* v = kept_room(ai, 8);
    if (!v)
        return;
    const float root[3] = { p[0], p[1] + 0.04f, p[2] };
    /* halved: they're drawn MODULATE2X (kept_draw), so grass.match can lift them past the picture's own shade */
    const uint32_t bottom = pack_color(col[0] * 0.375f, col[1] * 0.375f, col[2] * 0.375f), top = pack_color(col[0] * 0.5f, col[1] * 0.5f, col[2] * 0.5f);
    const float ca = cosf(yaw) * w * 0.5f, sa = sinf(yaw) * w * 0.5f;
    /* type 1: its picture's one tuft (make_tuft1_texture); type 2: one of the tufts in its picture (make_tuft_texture), each quad of
     * it flipped or not */
    const float cell = ai ? 1.0f / BLADE_VARIANTS : 1.0f, ua = ai ? (float)(pick % BLADE_VARIANTS) * cell : 0.0f;
    for (int q = 0; q < 2; ++q)
    {
        const float dx = q ? -sa : ca, dz = q ? ca : sa; /* the second quad the first turned a quarter */
        const int flip = ai && ((pick >> (8 + q)) & 1);
        const float u0 = flip ? ua + cell : ua, u1 = flip ? ua : ua + cell, dy = lean(dx, dz);
        v[0] = kvert(p[0] - dx, p[1] + 0.04f - dy, p[2] - dz, bottom, u0, 1, root, keep);
        v[1] = kvert(p[0] + dx, p[1] + 0.04f + dy, p[2] + dz, bottom, u1, 1, root, keep);
        v[2] = kvert(p[0] - dx, p[1] - h - dy, p[2] - dz, top, u0, 0, root, keep);
        v[3] = kvert(p[0] + dx, p[1] - h + dy, p[2] + dz, top, u1, 0, root, keep);
        v += 4;
    }
}

/* one of asset ai's sprites standing on p: two crossed quads of the sprite's rectangle of its picture, h tall, its width from its
 * shape times wmul; col what the picture's colours are multiplied by (0 to 4: they're drawn MODULATE4X, so a dark sprite can be
 * brought up to the ground's colour) */
static void keep_sprite(int ai, int ri, const float p[3], float yaw, float h, float wmul, const float col[3], uint32_t keep)
{
    const GAsset* as = &g.asset[ai];
    const SRect* r = &as->r[ri];
    KVert* v = kept_room(ai, 8);
    if (!v)
        return;
    const float u0 = (r->x0 + 0.5f) / (float)as->w, u1 = (r->x1 - 0.5f) / (float)as->w, v0 = (r->y0 + 0.5f) / (float)as->h,
                v1 = (r->y1 - 0.5f) / (float)as->h;
    const float w = h * (r->x1 - r->x0) / (r->y1 - r->y0) * wmul;
    const float root[3] = { p[0], p[1] + 0.04f, p[2] };
    const uint32_t bottom = pack_color(col[0] * 0.175f, col[1] * 0.175f, col[2] * 0.175f), top = pack_color(col[0] * 0.25f, col[1] * 0.25f, col[2] * 0.25f);
    const float ca = cosf(yaw) * w * 0.5f, sa = sinf(yaw) * w * 0.5f;
    for (int q = 0; q < 2; ++q)
    {
        const float dx = q ? -sa : ca, dz = q ? ca : sa, dy = lean(dx, dz); /* the second quad the first turned a quarter */
        v[0] = kvert(p[0] - dx, p[1] + 0.04f - dy, p[2] - dz, bottom, u0, v1, root, keep);
        v[1] = kvert(p[0] + dx, p[1] + 0.04f + dy, p[2] + dz, bottom, u1, v1, root, keep);
        v[2] = kvert(p[0] - dx, p[1] - h - dy, p[2] - dz, top, u0, v0, root, keep);
        v[3] = kvert(p[0] + dx, p[1] - h + dy, p[2] + dz, top, u1, v0, root, keep);
        v += 4;
    }
}

/* one flower: crossed pairs of quads, a stem (green) up to 56% of h and the bloom (bloom) above it */
static void keep_flower(float x, float y, float z, float yaw, float h, const float stem[3], const float bloom[3], uint32_t keep)
{
    KVert* v = kept_room(FLOWERS, 16);
    if (!v)
        return;
    const float w = h * 0.9f, split = h * 0.56f, root[3] = { x, y + 0.04f, z };
    const uint32_t cs = pack_color(stem[0], stem[1], stem[2]), cb = pack_color(bloom[0], bloom[1], bloom[2]);
    const float ca = cosf(yaw) * w * 0.5f, sa = sinf(yaw) * w * 0.5f;
    for (int q = 0; q < 2; ++q)
    {
        const float dx = q ? -sa : ca, dz = q ? ca : sa, dy = lean(dx, dz); /* the second quad the first turned a quarter */
        v[0] = kvert(x - dx, y + 0.04f - dy, z - dz, cs, 0, 1, root, keep), v[1] = kvert(x + dx, y + 0.04f + dy, z + dz, cs, 1, 1, root, keep);
        v[2] = kvert(x - dx, y - split - dy, z - dz, cs, 0, 0.44f, root, keep), v[3] = kvert(x + dx, y - split + dy, z + dz, cs, 1, 0.44f, root, keep);
        v[4] = v[2], v[5] = v[3]; /* the bloom's quad from the stem's top */
        v[4].col = v[5].col = g_cover_tint ? g_cover_tint : cb;
        v[6] = kvert(x - dx, y - h - dy, z - dz, cb, 0, 0, root, keep), v[7] = kvert(x + dx, y - h + dy, z + dz, cb, 1, 0, root, keep);
        v += 8;
    }
}
