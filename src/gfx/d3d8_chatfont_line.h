/* The chat input line as one picture (vanacore_gfx_set_input_line: chatline.cpp lays its visible text out whole in the input
 * font, DirectWrite's letters where the font puts them, and gives a caret of its own). The game's letters drawn while the line
 * is (d3d8_input_line) are kept as the line's (CfGlyph.line), the ones the font has no letter for too (cf_what: ñ, é, kana...),
 * and none of them is laid or drawn by itself (cf_draw): the picture goes where the line's pen is, in the first of them's draw
 * state, its outline in the outlines' pass, the caret over it. With no picture this frame each of them the font has is drawn at
 * the game's place. Something drawn over one of them has the whole line drawn into the target as the game drew it
 * (cf_line_game). Included by d3d8_chatfont.h after g_cf (the drawing helpers it uses are defined further on there). */
#pragma once

static int cf_clip(const GfxDraw* d, const GfxDraw* lay, const float map[4], const float rt[2], float c[4]);
static float cf_screen(const GfxDraw* run, int c, float p, const float map[4], const float rt[2]);
static void cf_affine(const GfxDraw* run, const float map[4], const float rt[2], float aff[4]);
static void cf_over(GfxDraw* d, const GfxDraw* lay, const float c[4], uint8_t* v);
static void cf_game(uint32_t i);
static void cf_glyph(uint32_t i, float x, const GfxDraw* lay, const float map[4], const float rt[2], int outline);
static int cf_in_pass(uint32_t n, int outline);

static struct
{
    int on;              /* the game is drawing the line now (d3d8_input_line): the letters it draws are the line's */
    int ready;           /* this frame's picture came (d3d8_set_input_line) */
    VanaLinePicture pic; /* ... it, its pixels in `pixels` */
    uint32_t* pixels;
    uint32_t serial, have; /* the picture's serial; pixels' room */
    GfxTex* tex;           /* made from pixels when it's wanted (gone with the device: cf_line_lost) */
    uint32_t tw, th, tserial;
} g_cl;

void d3d8_input_line(int on)
{
    g_cl.on = on != 0;
    if (on)
        g_cl.ready = 0;
}

void d3d8_set_input_line(const VanaLinePicture* line)
{
    g_cl.ready = 0;
    if (!line || !line->pixels || !line->w || !line->h || line->w > 8192 || line->h > 1024)
        return;
    const uint32_t need = line->w * line->h;
    if (line->serial != g_cl.serial || need > g_cl.have)
    {
        if (need > g_cl.have)
        {
            uint32_t* p = (uint32_t*)realloc(g_cl.pixels, (size_t)need * 4);
            if (!p)
                return;
            g_cl.pixels = p, g_cl.have = need;
        }
        memcpy(g_cl.pixels, line->pixels, (size_t)need * 4);
        g_cl.serial = line->serial;
    }
    g_cl.pic = *line, g_cl.pic.pixels = NULL, g_cl.ready = 1;
}

/* The frame's end: next frame's picture is its own */
static void cf_line_frame(void) { g_cl.ready = 0, g_cl.on = 0; }

static void cf_line_lost(void) { gfx_tex_destroy(g_cl.tex), g_cl.tex = NULL, g_cl.tw = g_cl.th = 0; }

/* The texture holding the picture: made at its size, filled when its serial is new */
static int cf_line_texture(void)
{
    const VanaLinePicture* p = &g_cl.pic;
    if (g_cl.tex && (g_cl.tw != p->w || g_cl.th != p->h))
        cf_line_lost();
    if (!g_cl.tex && (g_cl.tex = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, p->w, p->h, 1, GFX_USE_SAMPLE)) != NULL)
        g_cl.tw = p->w, g_cl.th = p->h, g_cl.tserial = g_cl.serial + 1;
    if (g_cl.tex && g_cl.tserial != g_cl.serial)
        gfx_tex_upload(g_cl.tex, 0, 0, g_cl.pixels, p->w * 4), g_cl.tserial = g_cl.serial;
    return g_cl.tex != NULL;
}

/* The first kept letter of the line not drawn into the target already, -1 none */
static int32_t cf_line_first(void)
{
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
        if (g_cf.kept[i].line && !g_cf.kept[i].drawn)
            return (int32_t)i;
    return -1;
}

/* Something drawn over the line's letters: all of them into the target as the game drew them, under it */
static void cf_line_game(void)
{
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
        if (g_cf.kept[i].line && !g_cf.kept[i].drawn)
            cf_game(i);
}

/* The first time a picture is drawn: the line's pen as given and its first kept letter's, the interface's pixels (the same
 * when the line doesn't start with a space: the game's pen is where it prints that letter) */
static void cf_line_say(uint32_t i)
{
    static int said;
    if (!said++)
        rt_log("[recomp] d3d8: True Font: the chat line drawn as one picture (%ux%u), its pen %.1f,%.1f, its first letter's "
               "%.1f,%.1f\n", g_cl.pic.w, g_cl.pic.h, g_cl.pic.x, g_cl.pic.y, g_cf.kept[i].px, g_cf.kept[i].py);
}

/* With no picture: each of the line's letters the font has, at the game's place */
static void cf_line_letters(const GfxDraw* lay, const float map[4], const float rt[2], int outline)
{
    const CfFont* f = &g_cf.font[VANA_FONT_INPUT];
    for (uint32_t n = 0; n < g_cf.nkept; ++n)
        if (g_cf.kept[n].line && !g_cf.kept[n].drawn && f->slot[g_cf.kept[n].code] >= 0 && cf_in_pass(n, outline))
            cf_glyph(n, g_cf.kept[n].px, lay, map, rt, outline);
}

/* A part of the picture (texture coordinates t: left, top, right, bottom) over the box q (the screen's pixels), in kept letter
 * i's draw state (`run`, its window's clip c on the screen, aff its mapping), its vertices its, the corners kept in its order */
static void cf_line_quad(uint32_t i, const float q[4], const float t[4], const GfxDraw* lay, const float c[4], const float aff[4],
    int outline)
{
    static GfxDraw d;
    static uint8_t v[4 * 64];
    const CfGlyph* g = &g_cf.kept[i];
    d = g_cf.runs[g->run];
    const uint32_t stride = (uint32_t)d.u.stride[0], pos = (uint32_t)d.u.offset[GFX_R_POSITION];
    const uint32_t tc = (uint32_t)d.u.offset[GFX_R_TEXCOORD0];
    memcpy(v, g_cf.bytes + i * CF_STRIDE, 4 * stride);
    for (uint32_t k = 0; k < 4; ++k)
    {
        float p[2];
        memcpy(p, v + k * stride + pos, 8);
        const int right = p[0] > (g->lo[0] + g->hi[0]) * 0.5f, bottom = p[1] > (g->lo[1] + g->hi[1]) * 0.5f;
        const float at[2] = { q[right ? 2 : 0], q[bottom ? 3 : 1] }, uv[2] = { t[right ? 2 : 0], t[bottom ? 3 : 1] };
        memcpy(v + k * stride + pos, at, 8);
        memcpy(v + k * stride + tc, uv, 8);
    }
    cf_over(&d, lay, c, v);
    d.tex[0] = g_cl.tex;
    memset(&d.samp[0], 0, sizeof d.samp[0]);
    d.samp[0].addr_u = d.samp[0].addr_v = d.samp[0].addr_w = 3; /* CLAMP */
    d.samp[0].mag = d.samp[0].min = 2, d.samp[0].max_aniso = 1; /* LINEAR, no mips */
    d.fs.alpha_func = 0; /* (the game's alpha test cut the picture's soft edges off: cf_glyph's) */
    if (outline) /* (black, the outline's ink times the font's outline opacity, over what's there: cf_glyph's) */
    {
        d.fs.text_shadow = 1, d.fs.alpha_func = 0, d.fs.fog = 0, d.fs.specular_add = 0;
        d.pipe.blend = 1, d.pipe.src = 2 /* ONE */, d.pipe.dst = 6 /* INVSRCALPHA */, d.pipe.op = 1;
        d.u.tfactor[3] = g_cf.font[VANA_FONT_INPUT].outline;
    }
    if (wc_cut_onto(&d, v, g->clip, aff)) /* (window.corners: inside its window's) */
        gfx_draw(&d);
}

/* The caret over the picture (left: the picture's left on the screen, base: the baseline), the white corner's ink; in the
 * outlines' pass as much wider and taller each way as the outline is */
static void cf_line_caret(uint32_t i, float left, float base, const GfxDraw* lay, const float c[4], const float aff[4], int outline)
{
    const VanaLinePicture* p = &g_cl.pic;
    const float pad = outline ? p->caret_pad : 0.0f;
    const float x = floorf(left + p->pen_x + p->caret - p->caret_w * 0.5f + 0.5f);
    const float q[4] = { x - pad, base + p->caret_top - pad, x + p->caret_w + pad, base + p->caret_bottom + pad };
    const float u = ((float)p->w - 1.5f) / (float)p->w, v = 1.5f / (float)p->h; /* (the 3 x 3's middle texel) */
    const float t[4] = { u, v, u, v };
    cf_line_quad(i, q, t, lay, c, aff, outline);
}

/* The line's picture over the laid target in this pass (outline 1: its outline, when the font has one; 0: its letters and the
 * caret), from the line's pen on its first letter's baseline, the picture's texels the screen's pixels */
static void cf_line_draw(const GfxDraw* lay, const float map[4], const float rt[2], int outline)
{
    const int32_t i = cf_line_first();
    if (i < 0)
        return;
    if (!g_cl.ready || !cf_line_texture())
    {
        cf_line_letters(lay, map, rt, outline);
        return;
    }
    const VanaLinePicture* p = &g_cl.pic;
    const CfFont* f = &g_cf.font[VANA_FONT_INPUT];
    if (outline && !(f->outline > 0.0f && p->out_h > 0))
        return;
    const GfxDraw* run = &g_cf.runs[g_cf.kept[i].run];
    float c[4], aff[4];
    if (!cf_clip(run, lay, map, rt, c))
        return;
    cf_affine(run, map, rt, aff);
    cf_line_say((uint32_t)i);
    /* (the game scrolls a long line by its own widths: one wider in the font has its end, the caret, moved back in) */
    const float end = cf_screen(run, 0, p->x, map, rt) + (p->caret >= 0.0f ? p->caret + p->caret_w + 2.0f : 0.0f);
    const float pen = cf_screen(run, 0, p->x, map, rt) - fmaxf(0.0f, end - c[2]);
    const float base = floorf(cf_screen(run, 1, p->y + f->baseline, map, rt) + 0.5f);
    const float left = floorf(pen - p->pen_x + 0.5f);
    const float W = (float)p->w, H = (float)p->h;
    if (outline)
    {
        const float o = floorf(pen - p->out_pen_x + 0.5f), top = base - p->out_base_y;
        const float q[4] = { o, top, o + (float)p->out_w, top + (float)p->out_h };
        const float t[4] = { 0.0f, (float)p->out_y / H, (float)p->out_w / W, (float)(p->out_y + p->out_h) / H };
        cf_line_quad((uint32_t)i, q, t, lay, c, aff, 1);
    }
    else
    {
        const float top = base - p->base_y;
        const float q[4] = { left, top, left + (float)p->fill_w, top + (float)p->fill_h };
        const float t[4] = { 0.0f, 0.0f, (float)p->fill_w / W, (float)p->fill_h / H };
        cf_line_quad((uint32_t)i, q, t, lay, c, aff, 0);
    }
    if (p->caret >= 0.0f && p->caret_lit)
        cf_line_caret((uint32_t)i, left, base, lay, c, aff, outline);
}
