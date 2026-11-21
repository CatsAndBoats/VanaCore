/* d3d8.c's include unit: [client] window.image, a picture of the player's own drawn in place of the window backgrounds' (the
 * 128x128 window texture's quads window_alpha finds), each window's quad sampling it as window.image.fit says, its alpha the
 * windows' opacity (window.opacity, the chat's, the chat line's). Its frames and everything inside are drawn over it as ever.
 * winimage.cpp decodes the file and hands the pixels here (vanacore_gfx_set_window_image); the texture is made from them at
 * the first window drawn, and again after a lost device. */

static struct
{
    uint32_t* argb;  /* the picture: 0xAARRGGBB, rows top down (straight alpha); NULL: none */
    uint32_t w, h;
    int fit;         /* 0 fill (scaled to cover the window, the rest cropped), 1 stretch, 2 tile (the picture's own pixels),
                      * 3 screen (one picture over the whole interface, each window showing the part behind it) */
    int chat, input; /* the chat windows and the chat line take it too (they match every window's look) */
    GfxTex* tex;     /* made from argb (winimage_ready), with its mipmaps */
    int failed;      /* the texture couldn't be made: the game's own backgrounds until the next picture */
} g_wimg;

static void winimage_lost(void)
{
    if (g_wimg.tex) /* (none at VanaCore's start, before the back end is there: winimage.cpp's first picture comes then) */
        gfx_tex_destroy(g_wimg.tex), g_wimg.tex = NULL;
}

void d3d8_set_window_image(const uint32_t* argb, uint32_t w, uint32_t h)
{
    free(g_wimg.argb), g_wimg.argb = NULL, g_wimg.w = g_wimg.h = 0, g_wimg.failed = 0;
    winimage_lost();
    if (!argb || !w || !h || w > 8192 || h > 8192)
        return;
    const size_t bytes = (size_t)w * h * 4;
    if ((g_wimg.argb = (uint32_t*)malloc(bytes)) == NULL)
        return;
    memcpy(g_wimg.argb, argb, bytes);
    g_wimg.w = w, g_wimg.h = h;
}

void d3d8_set_window_image_look(int fit, int chat, int input)
{
    g_wimg.fit = fit >= 0 && fit <= 3 ? fit : 0;
    g_wimg.chat = chat != 0, g_wimg.input = input != 0;
}

/* Level `src` (w x h) halved into `dst` (each texel the average of the 2x2 under it, the last row or column repeated at an odd
 * edge) */
static void winimage_halve(const uint32_t* src, uint32_t w, uint32_t h, uint32_t* dst, uint32_t dw, uint32_t dh)
{
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x)
        {
            const uint32_t x0 = 2 * x < w ? 2 * x : w - 1, y0 = 2 * y < h ? 2 * y : h - 1;
            const uint32_t x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
            const uint32_t p[4] = { src[y0 * w + x0], src[y0 * w + x1], src[y1 * w + x0], src[y1 * w + x1] };
            uint32_t out = 0;
            for (int c = 0; c < 32; c += 8)
                out |= ((((p[0] >> c) & 255) + ((p[1] >> c) & 255) + ((p[2] >> c) & 255) + ((p[3] >> c) & 255) + 2) / 4) << c;
            dst[y * dw + x] = out;
        }
}

/* The picture's texture, made (with every mipmap: a large picture drawn into a small window) the first time it's asked for */
static int winimage_ready(void)
{
    if (g_wimg.tex || !g_wimg.argb || g_wimg.failed)
        return g_wimg.tex != NULL;
    uint32_t levels = 1;
    while ((g_wimg.w >> levels) || (g_wimg.h >> levels))
        ++levels;
    g_wimg.tex = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, g_wimg.w, g_wimg.h, levels, GFX_USE_SAMPLE);
    uint32_t* scratch = g_wimg.tex ? (uint32_t*)malloc((size_t)g_wimg.w * g_wimg.h * 4) : NULL;
    if (!scratch)
    {
        gfx_tex_destroy(g_wimg.tex), g_wimg.tex = NULL, g_wimg.failed = 1;
        rt_log("[recomp] d3d8: window.image: its texture (%ux%u) couldn't be made: the game's backgrounds\n", g_wimg.w, g_wimg.h);
        return 0;
    }
    gfx_tex_upload(g_wimg.tex, 0, 0, g_wimg.argb, g_wimg.w * 4);
    const uint32_t* src = g_wimg.argb;
    uint32_t w = g_wimg.w, h = g_wimg.h;
    for (uint32_t level = 1; level < levels; ++level)
    {
        const uint32_t dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        /* (each level halved from the one before it, in place at the scratch's start: a level's texels come from rows at or below
         * its own, so writing it over the one it's made from never reads a texel already written) */
        winimage_halve(src, w, h, scratch, dw, dh);
        gfx_tex_upload(g_wimg.tex, 0, level, scratch, dw * 4);
        src = scratch, w = dw, h = dh;
    }
    free(scratch);
    return 1;
}

/* A window background's quad in the chat windows (in_chat) or the chat line (in_input), or any other: drawn with the picture */
static int winimage_for(int in_chat, int in_input)
{
    if (!g_wimg.argb || !(in_input ? g_wimg.input : in_chat ? g_wimg.chat : 1))
        return 0;
    return winimage_ready();
}

/* The part of a picture (iw x ih) that covers an area (w x h) with its shape kept: its left or top (o) and its width or height
 * (s) in the picture's 0..1, the rest of it cropped */
static void winimage_cover(float w, float h, float iw, float ih, float o[2], float s[2])
{
    const float k = fmaxf(w / iw, h / ih);
    s[0] = w / (iw * k), s[1] = h / (ih * k);
    o[0] = (1.0f - s[0]) * 0.5f, o[1] = (1.0f - s[1]) * 0.5f;
}

/* The background's vertices (up: a strip of 4 at stride 28, XYZRHW, diffuse, u v; lo, hi its corners) copied into `out` with
 * the picture's coordinates and white at alpha a: the address of the copy */
static uint32_t winimage_verts(uint8_t* out, uint32_t up, const float lo[2], const float hi[2], float a)
{
    const float w = hi[0] - lo[0], h = hi[1] - lo[1], iw = (float)g_wimg.w, ih = (float)g_wimg.h;
    float o[2] = { 0.0f, 0.0f }, s[2] = { 1.0f, 1.0f }; /* the picture's part from lo to hi, in its 0..1 */
    if (g_wimg.fit == 0)
        winimage_cover(w, h, iw, ih, o, s);
    else if (g_wimg.fit == 2)
        s[0] = w / iw, s[1] = h / ih;
    else if (g_wimg.fit == 3)
    {
        float sw, sh, so[2], ss[2];
        ui_target_size(&sw, &sh);
        winimage_cover(sw, sh, iw, ih, so, ss);
        o[0] = so[0] + ss[0] * lo[0] / sw, o[1] = so[1] + ss[1] * lo[1] / sh;
        s[0] = ss[0] * w / sw, s[1] = ss[1] * h / sh;
    }
    const uint32_t alpha = (uint32_t)(255.0f * (a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a) + 0.5f), colour = 0x00FFFFFFu | alpha << 24;
    memcpy(out, GUEST_PTR(up), 4 * 28);
    for (int i = 0; i < 4; ++i)
    {
        float p[2], uv[2];
        memcpy(p, out + 28 * i, 8);
        for (int c = 0; c < 2; ++c)
            uv[c] = o[c] + s[c] * ((c ? h : w) > 0.0f ? (p[c] - lo[c]) / (c ? h : w) : 0.0f);
        memcpy(out + 28 * i + 16, &colour, 4);
        memcpy(out + 28 * i + 20, uv, 8);
    }
    return (uint32_t)(uintptr_t)out;
}

/* The draw of a background winimage_verts made: the picture's texture times the vertices' white and alpha (the game's stage
 * doubles its window texture's colour and alpha), filtered between its mipmaps, repeated when tiled, blended over what's behind */
static void winimage_look(GfxDraw* d)
{
    GfxStage* st = &d->fs.st[0];
    d->tex[0] = g_wimg.tex;
    st->tex = 1, st->projected = 0;
    st->cop = 4, st->ca1 = 2, st->ca2 = 0; /* D3DTOP_MODULATE of D3DTA_TEXTURE and D3DTA_DIFFUSE */
    st->aop = 4, st->aa1 = 2, st->aa2 = 0;
    if (!d->fs.prog)
        d->fs.nstages = 1;
    GfxSampler* s = &d->samp[0];
    memset(s, 0, sizeof *s);
    s->addr_u = s->addr_v = s->addr_w = (uint8_t)(g_wimg.fit == 2 ? 1 : 3); /* D3DTADDRESS_WRAP, _CLAMP */
    s->mag = s->min = s->mip = 2;                                           /* D3DTEXF_LINEAR */
    d->pipe.blend = 1, d->pipe.src = 5, d->pipe.dst = 6, d->pipe.op = 1;    /* SRCALPHA, INVSRCALPHA, ADD */
}
