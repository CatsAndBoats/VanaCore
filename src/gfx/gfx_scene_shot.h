/* The scene before the game's interface, copied for a screenshot without it (gfx.h gfx_scene_shot_want, gfx_scene_shot: the MCP
 * server's screenshot tool with ui false). An include unit of both back ends (gfx_d3d12.c, gfx_d3d11.c), each defining SHOT_UP
 * first - its upscaler's state (g_dlss, g_up), for the scene noted there (pend_color, pend_serial) and the output made of it
 * (out, scene, out_serial). It uses their statics and is no file of its own.
 *
 * The picture is the scene's viewport as the frame shows it - the world, VanaCore's effects, the game's own (they're drawn into
 * the scene after gfx_scene_done), the upscaler and the colour grade - taken at the last moment before the interface:
 *  - where the back end holds the scene until the game is done with it (an upscaler, the colour grade laid last, the names
 *    drawn at the window's size), once that has run and before the names held for it go over it (shot_after_run): the
 *    upscaler's output where it made one, else the scene itself;
 *  - else before the first interface draw into the scene (the names over heads; the game's windows where the scene is the back
 *    buffer), or before the draw laying it onto the frame, whichever comes first (shot_on_draw).
 * A frame with no scene (gfx_scene_done never called: the title's menus, a loading screen) takes none: it stays asked for. */
#pragma once

static struct
{
    int want;              /* asked for, and not taken yet */
    GfxTex* scene;         /* the scene gfx_scene_done last had while one was wanted (compared, and read only once matched) ... */
    uint64_t scene_serial; /* ... the frame that was ... */
    uint32_t vp[4];        /* ... its viewport in the target ... */
    uint64_t shown;        /* ... and the pixels it's shown in (present_w x present_h; 0 not known) */
    GfxTex* tex;           /* the copy: the picture alone, at its own size. NULL till one is taken, and once it's read */
} g_shot;

static void shot_release(void)
{
    gfx_tex_destroy(g_shot.tex);
    g_shot.tex = NULL, g_shot.scene = NULL, g_shot.scene_serial = 0;
}

/* gfx.h: one is wanted (1), from the next frame that has a scene; or not any more (0). Either lets go of a copy not read */
void gfx_scene_shot_want(int on)
{
    shot_release();
    g_shot.want = on != 0;
}

/* gfx.h: the copy's size and D3D format, 0 while none has been taken; with pixels, its rows too (as gfx_tex_read), and it's let go */
int gfx_scene_shot(uint32_t* w, uint32_t* h, uint32_t* d3dfmt, void* pixels, uint32_t pitch)
{
    GfxTex* t = g_shot.tex;
    if (!t)
        return 0;
    *w = t->w, *h = t->h, *d3dfmt = t->fmt;
    if (pixels)
    {
        gfx_tex_read(t, 0, 0, pixels, pitch);
        shot_release();
    }
    return 1;
}

/* gfx_scene_done's: this frame's scene, and its viewport (the whole target where that isn't one within it, as the effects take it) */
static void shot_scene(GfxTex* color, const GfxScene* s)
{
    if (!g_shot.want || !color || color->type != GFX_TEX_2D || color->use != GFX_USE_RT)
        return;
    g_shot.scene = color, g_shot.scene_serial = g_serial;
    g_shot.shown = (uint64_t)s->present_w * s->present_h;
    memcpy(g_shot.vp, s->vp, sizeof g_shot.vp);
    if (s->vp[2] < 16 || s->vp[3] < 16 || s->vp[0] + s->vp[2] > color->w || s->vp[1] + s->vp[3] > color->h)
        g_shot.vp[0] = g_shot.vp[1] = 0, g_shot.vp[2] = color->w, g_shot.vp[3] = color->h;
}

/* The picture out of `from`: the scene itself, or the upscaler's output for it (the scene's whole target at the output's scale,
 * as up_draw_over and dlss_draw_over take it). Asked for no more, whether or not the copy could be made */
static void shot_take(GfxTex* from)
{
    const GfxTex* scene = g_shot.scene;
    g_shot.want = 0;
    if (!from || !scene || !scene->w || !scene->h || from->type != GFX_TEX_2D || from->use != GFX_USE_RT)
        return;
    const double kx = (double)from->w / scene->w, ky = (double)from->h / scene->h;
    uint32_t x = (uint32_t)(g_shot.vp[0] * kx + 0.5), y = (uint32_t)(g_shot.vp[1] * ky + 0.5);
    uint32_t w = (uint32_t)(g_shot.vp[2] * kx + 0.5), h = (uint32_t)(g_shot.vp[3] * ky + 0.5);
    if (x >= from->w || y >= from->h)
        x = y = 0;
    w = w > from->w - x ? from->w - x : w, h = h > from->h - y ? from->h - y : h;
    if (!w || !h)
        return;
    gfx_tex_destroy(g_shot.tex);
    g_shot.tex = gfx_tex_create(GFX_TEX_2D, from->fmt, w, h, 1, GFX_USE_RT); /* (the source's use: the same format for gfx_copy) */
    if (!g_shot.tex)
        return;
    gfx_copy(from, 0, 0, x, y, w, h, g_shot.tex, 0, 0, 0, 0);
    g_targets_bound = 0; /* `from` may be the target being drawn into: bound again, in its state, by the next draw */
}

/* Once the back end has run the scene it held (the upscaler, the grade), before the interface held for it is drawn over it */
static void shot_after_run(void)
{
    if (!g_shot.want || g_shot.scene_serial != g_serial || SHOT_UP.pend_color != g_shot.scene)
        return;
    shot_take(SHOT_UP.out && SHOT_UP.out_serial == g_serial && SHOT_UP.scene == g_shot.scene ? SHOT_UP.out : g_shot.scene);
}

/* Before each draw, for a scene the back end doesn't hold (shot_after_run has those): the first interface draw into it, or
 * the draw laying it onto the frame - not a grab of it into a small target (a distortion effect's, the sun flare's probe) */
static void shot_on_draw(const GfxDraw* d)
{
    if (!g_shot.want || g_shot.scene_serial != g_serial || SHOT_UP.pend_serial == g_serial || !g_rt)
        return;
    int reads = 0;
    for (int i = 0; i < 8; ++i)
        reads |= d->tex[i] == g_shot.scene;
    if (reads && (uint64_t)g_rt->w * g_rt->h * 2 < g_shot.shown)
        reads = 0;
    if (g_rt == g_shot.scene ? d->overlay : reads)
        shot_take(g_shot.scene);
}
