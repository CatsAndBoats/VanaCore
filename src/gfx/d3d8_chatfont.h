/* [client] chat.font=sharp, menu.font=sharp and the rest of True Font's areas: the interface's text drawn from VanaCore's own
 * glyphs, made from a TrueType font at the screen's size (chatfont.cpp). d3d8.c includes this just before draw_packet, after obj, g_dev, g_scene,
 * surface_gpu, g_squeeze, g_own_pixels, g_chat_drawing and rt_log, whose names it uses. It is no file of its own.
 *
 * FFXI draws its interface's text a glyph at a time (chat_glyph: a 16 x 16 cell of its 64 x 128 font sheet, the cell's number
 * the character's code) into its interface's target, which is smaller than the screen (1920x1200 for a 2560x1600 window at
 * resolution.ui auto-75), and lays the target onto the screen at the frame's end (opaque, alpha-tested, stretched): the
 * letters are the sheet's 16-pixel art made larger twice. A glyph drawn into that target whose character VanaCore has a glyph
 * for is kept instead (chatfont_keep), in its area's font (g_cf.font, VANA_FONT_*): the chat's while the game draws a chat row
 * (g_chat_drawing, d3d8_chat_drawing: chatfont.cpp's hooks on the chat windows' draw and on the row drawer the chat log
 * maximized uses too, which is how the target is known, or by the font sheet's letters while an addon hides the game's chat:
 * cf_learn), else the area of the window it's drawn in (g_cf.area,
 * d3d8_text_area: the menus', the item lists', the help's, the tooltips'), and the menus' labels' letters and the name plates' big ones the
 * buttons' (while a button's shape is drawn: d3d8_button_drawing), else the titles', and the Status window's (cf_label). When the target is laid onto the screen the kept
 * glyphs are drawn over it from VanaCore's atlases, at the screen's own pixels, in the game's state but for the texture
 * (chatfont_laid): the game's colours, its blend (not chat.shadow's shadow, which is the game font's: True Font's is its
 * outline). What the game draws into the target after them where one of them stands (a window opened over another) has that
 * glyph drawn into the target first, as the game drew it (chatfont_cover: found on a grid, CF_CELL pixels a cell). Kept
 * glyphs the target never took to the screen are lost for that frame; the glyphs are then the game's until it lays the target
 * again (chatfont_present).
 *
 * With the menus' size the back buffer's ([ffxi] resolution.ui the game's resolution: Fill window) the game makes no menu
 * target and draws its interface straight onto the screen (uiscale.cpp), so there's nothing to lay: the back buffer is the
 * interface's target (cf_target), and the kept glyphs go onto it before a draw over them (chatfont_cover) and at the frame's
 * end (chatfont_present), the runs' own mapping the screen's (cf_onto_screen). */
#pragma once
#ifdef VANACORE
#include "vanacore_gfx.h"

#define CF_CODES VANA_GLYPH_CODES /* the font sheet's cells (64 x 128: VANA_LABEL_WORDS), then the Japanese labels' words */
#define CF_MOST 8192  /* glyphs kept in a frame */
#define CF_RUNS 64    /* draw states among them (each window's clip, a shadow or none) */
#define CF_CELL 64    /* the cover test's grid: cells this many interface pixels a side, */
#define CF_GRID 64    /* ... this many a side (4096 pixels) */

typedef struct CfFont
{
    int on;
    uint32_t* atlas; /* premultiplied white, aw x ah */
    uint32_t aw, ah;
    GfxTex* tex; /* made from atlas when it's wanted (gone with the device: chatfont_lost) */
    VanaChatGlyph* glyphs;
    uint32_t nglyphs;
    int16_t slot[CF_CODES]; /* the glyph for a code, -1 none */
    float baseline;         /* below a cell's top, the interface's pixels */
    float outline;          /* the outlines' opacity (0..1), 0 none */
    float line;             /* its rows' spacing to the game's (cf_spread) */
    float size;             /* its em, the interface's pixels */
    float pad;              /* its rows moved down this far (the tooltips': cf_spread) */
    int upper;              /* its lower-case letters are capitals (VanaTextFont.upper: cf_fit_word) */
} CfFont;

typedef struct CfGlyph
{
    float px, py;       /* the pen: the cell's top-left, in the interface's pixels */
    float lo[2], hi[2]; /* the cell, a little in from its edges (what covers it: chatfont_cover) */
    uint16_t code;
    uint8_t run, drawn; /* its draw state (g_cf.runs); drawn into the target already, as the game drew it */
    uint8_t font;       /* g_cf.font's (VANA_FONT_*) */
    uint8_t pic;        /* the game's own picture in a tooltip's row (an element's icon: cf_icon), moved with its row (dy) */
    uint8_t fade;       /* a tooltip's letter: how see-through, 0 as the game's to 255 not drawn (VanaTipPart.fade); 255 a list's
                         * number left out too (cf_big_word) */
    uint8_t typed;      /* ... printed as its type row's (d3d8_tip_type) */
    uint8_t worn;       /* ... of an equipped item's name in its equipped colour (d3d8_tip_equipped): that kept (cf_tip_letter) */
    uint8_t cut;        /* a letter of the "menu2fon" sheet (VanaLabelGlyph.sheet 1): drawn REVSUBTRACT it's the letter itself,
                         * cut out of its picture (the chat line's Say / Shout tab), not a shadow copy (cf_draw) */
    uint8_t rolled;     /* a scrolling line's letter (d3d8_text_rolling): laid where the game's pen is, centred in its width
                         * as chatfont.cpp made it, never a word at a time (cf_row) */
    uint8_t clip;       /* the window shape it was drawn in, kept to (window.corners: d3d8_corners.h wc_keep), 0 none */
    uint8_t line;       /* a letter of the chat line, which is drawn as one picture (d3d8_chatfont_line.h) */
    uint16_t centred;   /* a text the game centres (d3d8_text_centred: the status icons' timers) or right-aligns (`right`,
                         * d3d8_text_right): its draw's number, each draw another; 0 none. Its letters one text (cf_centred_word) */
    uint8_t right;      /* ... 1 laid to end where the game's does; a whole text's (d3d8_text_whole) 2 from its left, 3 to its end; 0 centred on cx */
    float cx;           /* ... the middle the game centres it on, the interface's pixels */
    float gs[2];        /* a font sheet cell's size as the game drew it, across and down: its sprite's to the sheet's 16 (the
                         * timers' 0.85); 1 for the rest. The game's widths and baseline are that much smaller */
    float dy;          /* ... how far down, the interface's pixels (cf_spread) */
    float row_x;        /* a tooltip's row's leftmost letter's pen, which a part's row is made wider from (d3d8_tips.h) */
    uint32_t color;     /* its letter's colour, 0xFFRRGGBB (a tooltip part's: VanaTipPart.color); 0 the game's */
    uint32_t rarity;    /* a tooltip's letter: its item's rarity colour, 0xFFRRGGBB, its name's with tips.rarity.name; 0 none */
    float label_w;      /* a menu label's letter or a big one: its sprite's width, the interface's pixels; 0 not one */
    float scale[2];     /* ... and its picture's size to its font's, across and down: the game's own scale for the sprite (1: a
                         * texel to a pixel; "Communication" is squeezed across to fit its button) times the letter's capitals'
                         * height to the labels' (VanaLabelGlyph.cap: the name plates' big letters, in the Status window and the
                         * clock at about half their size, are 19 to the labels' 9) */
} CfGlyph;

static struct
{
    CfFont font[VANA_FONTS]; /* each area's (d3d8_set_chat_font) */
    int area;                /* the font sheet's text drawn now is in: a window's (d3d8_text_area), 0 the menus' */
    int lists;               /* ... its font sheet text Menu Lists' (VANA_TEXT_LISTS: cf_area), its labels still the area's */
    int button;              /* a menu button's shape is being drawn, nested (d3d8_button_drawing): its labels' letters the buttons' */
    int clock;               /* the clock is being drawn, nested (d3d8_clock_drawing): its letters the clock's */
    int game;                /* letters no label's are being drawn, nested (d3d8_game_letters): no sprite taken for one (cf_label) */
    int rolling;             /* a scrolling line's letters are being drawn, nested (d3d8_text_rolling): each laid where the game's is (CfGlyph.rolled) */
    uint16_t centring, centred_n; /* a centred or right-aligned text's letters are being drawn: its number (CfGlyph.centred), 0
                                   * none; the last one's */
    uint8_t right;                /* ... how it's laid (CfGlyph.right) */
    float centre_x;               /* ... its middle (CfGlyph.cx) */
    uint8_t label_fixed;     /* a main menu label the game squeezed drawn at the font's full size (d3d8_label_fixed, cf_unsqueeze) */
    uint32_t macro_color;    /* the macro bar's keys' True Font letters' colour, 0xFFRRGGBB, 0 the game's (d3d8_set_macrobar) */
    int macro_bare;          /* ... their "Ctrl" / "Alt" left out, the numbers alone (cf_macro_prefix) */
    uint8_t hide[VANA_FONTS]; /* an area's letters left out altogether (d3d8_hide_letters, cf_hidden): 1 the Equipment window's slot
                               * names (equip.font=hide), 2 every letter of the area (its True Font size 0) */
    float into[2];           /* kept glyphs drawn into the interface's target itself, not over its laying: its pixels per
                              * pixel of the screen-sized space they're laid in (cf_under; 0 laying onto the screen: cf_over) */
    VanaLabelGlyph labels[255]; /* the label letters' sprites (d3d8_set_label_glyphs) */
    uint8_t label_at[3][256 * 256]; /* each sheet's texels (VanaLabelGlyph.sheet): the index + 1 in labels of the sprite there, 0 none */
    /* the interface's target */
    uint32_t target; /* the texture (guest pointer) the chat rows were last drawn into, or the back buffer (g_dev.backbuffer) */
    float k[2];      /* the target's drawn pixels per interface pixel, from the chat's and the letters' draws (cf_take_scale) */
    int k_wide[2];   /* ... taken from a draw clipped CF_WIDE units or more that way (exact enough to keep the fonts) */
    float sx, sy;    /* the screen's pixels per interface pixel where that was last laid onto the screen */
    VanaTipPart parts[VANA_TIP_PARTS]; /* the tooltips' parts' look (d3d8_set_tip_parts) */
    float tips_extra;                  /* what the tooltips laid last needed beyond their rows' spacing (d3d8_tips_extra) */
    int tips_seen;                     /* ... the last laying had tooltips */
    int spread;                        /* cf_spread ran since the laying (or, straight onto the screen, the frame) began */
    uint32_t chat_ago; /* frames since the chat's rows were drawn (cf_learn) */
    uint32_t unlaid;   /* frames the interface's target (a texture) went without being seen laid onto the screen */
    int rest;        /* kept glyphs were lost: none kept until the target is laid again */
    uint32_t lost;   /* frames that lost theirs */
    /* the game's own word on its interface's target (d3d8_set_interface_target): read, and the texture, 0 the back buffer */
    int told_known;
    uint32_t told;
    /* the game's letter printer's draw (d3d8_letter_draw): on 1 while the printer runs, 2 once its draw came to chatfont_keep (that
     * draw is the letter, whatever its shape: cf_printed); the letter's cell, its pen (the interface's pixels) and its size (times
     * 16 pixels) */
    struct
    {
        int on;
        uint32_t code;
        int x, y;
        float sx, sy;
    } printer;
    /* the target (a texture) laid onto the screen by assumption (cf_assume): frames in a row its glyphs went unlaid, and
     * whether it's on (off again at a laying seen) */
    uint32_t unlaid_kept;
    int assume;
    /* diagnostics ([client] truefont.debug: d3d8_chat_font_debug): counts since the last report (d3d8_chat_font_report), and
     * how many draws of each kind are still to be described in the log this report */
    int debug;
    struct
    {
        uint32_t draws, own, no_rt, scene, told_other, chat_other, other, chat, cells, cell_nofont, labels, icons, kept, rest;
        uint32_t learn_chat, learn_told, learn_cell, cover_game, cover_screen, lay_seen, lay_ok, lay_nomap, lay_elsewhere;
        uint32_t assumed, flushed, lost;
        uint32_t printed, printer_nodraw, rescued; /* the printer's letters whose draw came, whose never did, and those kept by its word alone */
        int say_cell, say_lay, say_other, say_printer;
    } n;
    /* this frame's */
    CfGlyph kept[CF_MOST];
    uint32_t nkept;
    uint8_t* bytes; /* the kept glyphs' vertices, 4 each at their run's stride, CF_STRIDE apart */
    uint32_t cap;
    GfxDraw runs[CF_RUNS];
    uint32_t nruns, last;
    float box[4];                       /* round every kept glyph: left, top, right, bottom */
    int32_t head[CF_GRID * CF_GRID];    /* each cell's first entry, -1 none */
    int32_t next[4 * CF_MOST];          /* an entry's next in its cell */
    uint16_t who[4 * CF_MOST];          /* an entry's kept glyph */
    uint32_t nentries;
} g_cf;
#define CF_STRIDE 256u /* room for one glyph's vertices (quad_shaped's largest stride is 64) */

#include "d3d8_chatfont_wide.h" /* cf_want_glyph: the cells past ASCII drawn with no glyph, for chatfont.cpp to build */
#include "d3d8_tips.h" /* cf_spread: the tooltips' rows moved and their parts' look */
#include "d3d8_chatfont_line.h" /* cf_line_draw: the chat line as one picture, with VanaCore's caret */

/* v when it's lo to hi, else `fallback` (a NaN too) */
static float cf_within(float v, float lo, float hi, float fallback) { return v >= lo && v <= hi ? v : fallback; }

void d3d8_set_chat_font(int which, const VanaTextFont* in)
{
    if (which < 0 || which >= VANA_FONTS)
        return;
    CfFont* f = &g_cf.font[which];
    const int grown = cf_grown(f, in); /* (the frame's kept glyphs kept: d3d8_chatfont_wide.h) */
    gfx_tex_destroy(f->tex), f->tex = NULL;
    free(f->atlas), f->atlas = NULL;
    free(f->glyphs), f->glyphs = NULL;
    f->on = 0, f->nglyphs = 0, g_cf.nkept = grown ? g_cf.nkept : 0, g_cf.nruns = grown ? g_cf.nruns : 0;
    for (int i = 0; i < CF_CODES; ++i)
        f->slot[i] = -1;
    if (!in || !in->atlas || !in->w || !in->h || !in->glyphs || !in->count || in->count > 32767)
        return;
    f->atlas = (uint32_t*)malloc((size_t)in->w * in->h * 4);
    f->glyphs = (VanaChatGlyph*)malloc(in->count * sizeof *in->glyphs);
    if (!f->atlas || !f->glyphs)
    {
        free(f->atlas), f->atlas = NULL;
        free(f->glyphs), f->glyphs = NULL, g_cf.nkept = 0, g_cf.nruns = 0;
        return;
    }
    memcpy(f->atlas, in->atlas, (size_t)in->w * in->h * 4);
    memcpy(f->glyphs, in->glyphs, in->count * sizeof *in->glyphs);
    f->aw = in->w, f->ah = in->h, f->nglyphs = in->count, f->baseline = in->baseline;
    f->outline = in->outline > 0.0f ? (in->outline < 1.0f ? in->outline : 1.0f) : 0.0f;
    f->line = cf_within(in->line, 0.5f, 3.0f, 1.0f), f->size = cf_within(in->size, 0.5f, 64.0f, 16.0f);
    f->pad = cf_within(in->pad, -32.0f, 64.0f, 0.0f), f->upper = in->upper != 0;
    for (uint32_t i = 0; i < in->count; ++i)
    {
        const VanaChatGlyph* g = &in->glyphs[i];
        if (g->code < CF_CODES && (uint32_t)g->ax + g->w <= in->w && (uint32_t)g->ay + g->h <= in->h && (uint32_t)g->oax + g->ow <= in->w &&
            (uint32_t)g->oay + g->oh <= in->h)
            f->slot[g->code] = (int16_t)i;
    }
    f->on = 1;
}

void d3d8_set_label_glyphs(const VanaLabelGlyph* list, uint32_t count)
{
    memset(g_cf.labels, 0, sizeof g_cf.labels), memset(g_cf.label_at, 0, sizeof g_cf.label_at);
    uint32_t n = 0; /* (by sprite, not by letter: the small 'A' and the big one are two) */
    for (uint32_t i = 0; list && i < count && n < 255; ++i)
    {
        const VanaLabelGlyph* l = &list[i];
        if (l->code >= 96 || !l->w || !l->h || l->u + l->w > 256 || l->v + l->h > 256 || l->sheet > 2)
            continue;
        g_cf.labels[n++] = *l;
        for (int y = l->v; y < l->v + l->h; ++y)
            memset(&g_cf.label_at[l->sheet][y * 256 + l->u], (int)n, l->w);
    }
}

void d3d8_chat_drawing(int on) { g_chat_drawing = on ? g_chat_drawing + 1 : g_chat_drawing > 0 ? g_chat_drawing - 1 : 0; }

void d3d8_button_drawing(int on) { g_cf.button = on ? g_cf.button + 1 : g_cf.button > 0 ? g_cf.button - 1 : 0; }

void d3d8_clock_drawing(int on) { g_cf.clock = on ? g_cf.clock + 1 : g_cf.clock > 0 ? g_cf.clock - 1 : 0; }

void d3d8_game_letters(int on) { g_cf.game = on ? g_cf.game + 1 : g_cf.game > 0 ? g_cf.game - 1 : 0; }

void d3d8_set_macrobar(uint32_t color, int prefix) { g_cf.macro_color = color, g_cf.macro_bare = !prefix; }

void d3d8_label_fixed(int on) { g_cf.label_fixed = (uint8_t)(on != 0); }

void d3d8_hide_letters(int which, int on)
{
    if (which >= 0 && which < VANA_FONTS)
        g_cf.hide[which] = (uint8_t)(on <= 0 ? 0 : on >= 2 ? 2 : 1);
}

void d3d8_text_rolling(int on) { g_cf.rolling = on ? g_cf.rolling + 1 : g_cf.rolling > 0 ? g_cf.rolling - 1 : 0; }

int d3d8_chat_scale(float* sx, float* sy)
{
    *sx = g_cf.sx, *sy = g_cf.sy;
    return g_cf.sx > 0.0f && g_cf.sy > 0.0f;
}

void d3d8_set_interface_target(uint32_t texture, int known)
{
    if (known && (!g_cf.told_known || texture != g_cf.told))
        rt_log("[recomp] d3d8: True Font: the game says its interface is drawn into %s%s\n",
            texture ? "its menu target" : "the screen itself (no menu target)", g_cf.told_known ? " (changed)" : "");
    g_cf.told_known = known != 0, g_cf.told = known ? texture : 0;
}

void d3d8_chat_font_debug(int on) { g_cf.debug = on != 0; }

/* The game's letter printer (chatfont.cpp's hook of FFXiMain's 0x1011B7D0) is about to draw letter `code` of the font sheet at pen
 * x, y sx x sy times 16 pixels (on 1), or has drawn it (on 0): the draw between is that letter whatever its shape (cf_printed). A
 * printer's run whose draw never came to chatfont_keep is counted (the game's draw went another way, or d3d8 dropped it first). */
void d3d8_letter_draw(int on, int code, int x, int y, float sx, float sy)
{
    if (on)
    {
        g_cf.printer.on = 1, g_cf.printer.code = (uint32_t)code, g_cf.printer.x = x, g_cf.printer.y = y;
        g_cf.printer.sx = sx, g_cf.printer.sy = sy;
        return;
    }
    if (g_cf.printer.on == 1)
        g_cf.n.printer_nodraw++;
    g_cf.printer.on = 0;
}

/* A target as a guest pointer described: the back buffer, a texture's size, or the number */
static const char* cf_name(uint32_t target, char* buf, size_t size)
{
    const Obj* t = target ? obj(target) : NULL;
    if (!target)
        snprintf(buf, size, "none");
    else if (target == g_dev.backbuffer)
        snprintf(buf, size, "the screen");
    else if (t && t->kind == O_TEXTURE)
        snprintf(buf, size, "texture %ux%u%s", t->width, t->height, (t->usage & USAGE_RENDERTARGET) ? " (a render target)" : "");
    else
        snprintf(buf, size, "%08x", target);
    return buf;
}

#include "d3d8_chatfont_report.h" /* d3d8_chat_font_report: the state in a line for the log */

static void cf_box(const GfxDraw* d, float lo[2], float hi[2]);

/* [client] truefont.debug: a draw described in the log (`what` it was taken for), the first few of its kind a report */
static int cf_quad(const GfxDraw* d, float lo[2], float hi[2], float tlo[2], float thi[2]);

static void cf_say(int* left, const char* what, const GfxDraw* d)
{
    char rt[48], t0[48], fmt[16] = "no texture", uv[80] = "not a sprite";
    float lo[2], hi[2], tlo[2], thi[2];
    const Obj* t = obj(g_dev.cur.tex[0]);
    if (*left <= 0)
        return;
    --*left;
    cf_box(d, lo, hi);
    if (t && t->format > 0xffff) /* a four-character code (DXT3) */
        snprintf(fmt, sizeof fmt, "%c%c%c%c", (int)(t->format & 0xff), (int)(t->format >> 8 & 0xff), (int)(t->format >> 16 & 0xff),
            (int)(t->format >> 24 & 0xff));
    else if (t)
        snprintf(fmt, sizeof fmt, "format %u", t->format);
    if (cf_quad(d, lo, hi, tlo, thi)) /* the sheet's grid: a cell is 1/64 x 1/128 */
        snprintf(uv, sizeof uv, "sprite, texture coordinates %.5f,%.5f-%.5f,%.5f (1/%.1f x 1/%.1f)", tlo[0], tlo[1], thi[0], thi[1],
            thi[0] > tlo[0] ? 1.0f / (thi[0] - tlo[0]) : 0.0f, thi[1] > tlo[1] ? 1.0f / (thi[1] - tlo[1]) : 0.0f);
    rt_log("[recomp] d3d8: True Font: %s: into %s, texture %s %s, pre-transformed %d, %s, %u vertices of %u bytes, box %.1f,%.1f-%.1f,%.1f, "
           "%s, clip %u,%u %ux%u (the game's %.0f,%.0f %.0fx%.0f), shaders %d/%d, blend %d %d/%d, chat %d, area %d\n",
        what, cf_name(g_dev.rt == g_dev.backbuffer ? g_dev.rt : obj(g_dev.rt) ? obj(g_dev.rt)->container : 0, rt, sizeof rt),
        cf_name(g_dev.cur.tex[0], t0, sizeof t0), fmt, d->vs.rhw, d->buf[0] ? "from a buffer" : d->data[0] ? "the draw's own vertices" : "no vertices",
        d->u.stride[0] ? d->size[0] / (uint32_t)d->u.stride[0] : 0, d->u.stride[0], lo[0], lo[1], hi[0], hi[1], uv, d->vp[0], d->vp[1],
        d->vp[2], d->vp[3], d->u.vp[0], d->u.vp[1], d->u.vp[2], d->u.vp[3], d->vs.prog != 0, d->fs.prog != 0, d->pipe.blend,
        d->pipe.src, d->pipe.dst, g_chat_drawing, g_cf.area);
}

/* A font every letter of its window is (its font sheet text, its labels' letters and its big letters: cf_what), laid as the Status
 * area's (cf_big_words): the trade windows', Current Gil's, the Profile panel's and its titles' (GIL to PROFILE_TITLE, in a row),
 * the Equipment window's stats' (VANA_FONT_EQUIP_STATS: equipstats.cpp's area round the stats; in the window, cf_equip_font) */
static int cf_whole(int f) { return f == VANA_FONT_TRADE || (f >= VANA_FONT_GIL && f <= VANA_FONT_PROFILE_TITLE) || f == VANA_FONT_EQUIP_STATS; }

/* The font for the font sheet's text drawn now outside the chat rows: its window's area, the menus' if none (cf_whole's their
 * own: every letter in them, their amounts' big letters and their labels' too, cf_what). The Equipment window's text is its
 * slot names' font, VANA_FONT_EQUIP (the names themselves are "menu2fon" letters, cf_equip_font); the stats under it the
 * stats' (equipstats.cpp's area round them) */
static int cf_area(void)
{
    if (g_cf.lists)
        return VANA_FONT_LISTS; /* (a window given Menu Lists: its text alone, its labels its area's, cf_what) */
    return g_cf.area == VANA_FONT_ITEMS || g_cf.area == VANA_FONT_HELP || g_cf.area == VANA_FONT_INPUT || g_cf.area == VANA_FONT_PHRASES ||
                   g_cf.area == VANA_FONT_TIPS || g_cf.area == VANA_FONT_EQUIP || g_cf.area == VANA_FONT_BUFFS || g_cf.area == VANA_FONT_TARGET ||
                   cf_whole(g_cf.area)
               ? g_cf.area
               : VANA_FONT_MENUS;
}

/* The font for the name plates' big letters and the labels' letters drawn now: the party list's or the alliance lists' while
 * their window is drawn (their names and numbers are those letters, not the font sheet's), else -1 (the Status area's, the
 * buttons' or the titles') */
static int cf_list_area(void) { return g_cf.area == VANA_FONT_PARTY || g_cf.area == VANA_FONT_ALLIANCE ? g_cf.area : -1; }

int d3d8_text_area(int area)
{
    const int was = g_cf.area | (g_cf.lists ? VANA_TEXT_LISTS : 0); /* (as it was: the party list's isn't cf_area's) */
    g_cf.area = area & ~VANA_TEXT_LISTS;
    g_cf.lists = (area & VANA_TEXT_LISTS) != 0;
    return was;
}

/* XYZRHW vertices with texture coordinates, in the draw's own memory (not a buffer), at least `n` of them, their stride at
 * most `most`: the positions' and coordinates' offsets in a vertex */
static int cf_vertices(const GfxDraw* d, uint32_t n, uint32_t most, uint32_t* pos, uint32_t* tc)
{
    const uint32_t stride = (uint32_t)d->u.stride[0];
    const GfxElem* ep = &d->vs.el[GFX_R_POSITION];
    const GfxElem* et = &d->vs.el[GFX_R_TEXCOORD0];
    *pos = (uint32_t)d->u.offset[GFX_R_POSITION], *tc = (uint32_t)d->u.offset[GFX_R_TEXCOORD0];
    if (!d->vs.rhw || !d->data[0] || d->buf[0] || !ep->used || ep->stream || !et->used || et->stream)
        return 0;
    return stride >= 16 && stride <= most && *pos + 8 <= stride && *tc + 8 <= stride && d->size[0] >= n * stride;
}

/* The drawn pixels per interface pixel of draw `d` (across: c 0, down: 1): its target's own, which are the game's unless
 * [ui] aspect squeezes the interface or VanaCore draws the target at another size (Obj.pw) */
static float cf_scale(const GfxDraw* d, int c) { return d->u.vp[2 + c] > 0.0f ? (float)d->vp[2 + c] / d->u.vp[2 + c] : 1.0f; }

/* A sprite: a strip of four XYZRHW vertices from the draw's own memory, its box (lo, hi) and its texture coordinates' (tlo, thi) */
static int cf_quad(const GfxDraw* d, float lo[2], float hi[2], float tlo[2], float thi[2])
{
    const uint32_t stride = (uint32_t)d->u.stride[0];
    uint32_t pos, tc;
    if (d->vs.prog || d->fs.prog || d->prim != GFX_TRIANGLESTRIP || d->count != 2 || d->indices || d->vertex_start || d->u.vofs ||
        !cf_vertices(d, 4, 64, &pos, &tc))
        return 0;
    tlo[0] = tlo[1] = lo[0] = lo[1] = 1e30f, thi[0] = thi[1] = hi[0] = hi[1] = -1e30f;
    for (uint32_t i = 0; i < 4; ++i)
    {
        float p[2], t[2];
        memcpy(p, (const uint8_t*)d->data[0] + i * stride + pos, 8);
        memcpy(t, (const uint8_t*)d->data[0] + i * stride + tc, 8);
        for (int c = 0; c < 2; ++c)
        {
            lo[c] = p[c] < lo[c] ? p[c] : lo[c], hi[c] = p[c] > hi[c] ? p[c] : hi[c];
            tlo[c] = t[c] < tlo[c] ? t[c] : tlo[c], thi[c] = t[c] > thi[c] ? t[c] : thi[c];
        }
    }
    return 1;
}

/* A glyph of the font sheet by its shape: a sprite 16 x 16 in the interface's pixels, texture coordinates one cell of the 64 x 128
 * grid. Its box (lo, hi) and the cell's number. */
static int cf_shaped(const GfxDraw* d, float lo[2], float hi[2], uint32_t* code)
{
    float tlo[2], thi[2];
    if (!cf_quad(d, lo, hi, tlo, thi))
        return 0;
    /* (written so that a NaN fails) */
    if (!(fabsf(hi[0] - lo[0] - 16.0f) < 0.01f && fabsf(hi[1] - lo[1] - 16.0f) < 0.01f &&
            fabsf(thi[0] - tlo[0] - 1.0f / 64.0f) < 1e-4f && fabsf(thi[1] - tlo[1] - 1.0f / 128.0f) < 1e-4f))
        return 0;
    const float cu = tlo[0] * 64.0f, cv = tlo[1] * 128.0f;
    const int col = (int)floorf(cu + 0.5f), row = (int)floorf(cv + 0.5f);
    const int whole = fabsf(cu - (float)col) <= 0.01f && fabsf(cv - (float)row) <= 0.01f;
    if (!whole || col < 0 || col >= 64 || row < 0 || row >= 128)
        return 0;
    *code = (uint32_t)(row * 64 + col);
    return 1;
}

/* The draw the game's letter printer is making (d3d8_letter_draw): a letter whatever its sprite's size and texture coordinates,
 * its cell the printer's word; the sprite's box. 0 when the vertices can't be read (nothing could be kept and drawn back from
 * them) or the cell is past the sheet (the icons' codes, 0x2115 up, go another way; a negative one). */
static int cf_printed(const GfxDraw* d, float lo[2], float hi[2], uint32_t* code)
{
    float tlo[2], thi[2];
    if (!cf_quad(d, lo, hi, tlo, thi))
        return 0;
    *code = g_cf.printer.code;
    return *code < VANA_LABEL_WORDS;
}

/* A glyph of the font sheet: the draw the game's letter printer is making, else one shaped as its letters are (both: cf_shaped).
 * Its box and the cell's number. */
static int cf_cell(const GfxDraw* d, float lo[2], float hi[2], uint32_t* code)
{
    return g_cf.printer.on == 2 ? cf_printed(d, lo, hi, code) : cf_shaped(d, lo, hi, code);
}

/* Draw `d` is the game's letter printer's, the first since it was called (d3d8_letter_draw): noted so, and counted, with whether
 * the shape test alone would have taken it for the printer's letter ([client] truefont.debug: the first few said) */
static void cf_printing(const GfxDraw* d)
{
    float lo[2], hi[2];
    uint32_t code = 0;
    if (g_cf.printer.on != 1)
        return;
    g_cf.printer.on = 2;
    g_cf.n.printed++;
    const int shaped = cf_shaped(d, lo, hi, &code) && code == g_cf.printer.code;
    if (!shaped)
        g_cf.n.rescued++;
    if (g_cf.n.say_printer > 0 && (!shaped || g_cf.n.say_printer == 3))
    {
        --g_cf.n.say_printer;
        rt_log("[recomp] d3d8: True Font: the game's letter printer draws cell %u at %d,%d, %.3f x %.3f: %s\n", g_cf.printer.code,
            g_cf.printer.x, g_cf.printer.y, g_cf.printer.sx, g_cf.printer.sy,
            shaped ? "shaped as a letter" : "NOT shaped as a letter (kept by the printer's word)");
    }
}

/* A letter of a menu label: a strip of four XYZRHW vertices from a 256 x 256 texture, a sprite (whole texels), where the "font
 * font" sheet has a label letter, else where the "menu2fon" sheet has one (d3d8_set_label_glyphs; the two sheets' letters are
 * in different places and sizes, so the sprite says which): the letter, the sprite's box and its top (texels), and how it's
 * drawn (scale: the interface's pixels a texel, across and down; a texel to a pixel, or squeezed to fit a button, or the big
 * letters at about half; in the party and alliance lists, any size down to a fifth: their MP and TP are drawn smaller than
 * their HP) */
static const VanaLabelGlyph* cf_label(const GfxDraw* d, float lo[2], float hi[2], int* top, float scale[2])
{
    const Obj* t = obj(g_dev.cur.tex[0]);
    float tlo[2], thi[2];
    if (g_cf.game || !t || t->width != 256 || t->height != 256 || !cf_quad(d, lo, hi, tlo, thi))
        return NULL;
    int r[4];
    const float least = cf_list_area() >= 0 || g_cf.area == VANA_FONT_BUFFS ? 0.2f : 0.4f; /* (the timers' 0.85 of a big letter's) */
    for (int c = 0; c < 2; ++c)
    {
        r[c] = (int)floorf(tlo[c] * 256.0f + 0.5f), r[2 + c] = (int)floorf(thi[c] * 256.0f + 0.5f) - r[c];
        scale[c] = r[2 + c] > 0 ? (hi[c] - lo[c]) / (float)r[2 + c] : 0.0f;
        if (fabsf(tlo[c] * 256.0f - (float)r[c]) > 0.02f || !(scale[c] >= least && scale[c] <= 1.05f) || r[c] < 0)
            return NULL;
    }
    if (r[0] + r[2] > 256 || r[1] + r[3] > 256)
        return NULL;
    /* (the "keytop" sheet's numbers only in the macro bar: the sheet's letters elsewhere are the macros' names) */
    for (int sheet = 0; sheet < (g_cf.area == VANA_FONT_MACROBAR ? 3 : 2); ++sheet)
    {
        const int at = g_cf.label_at[sheet][(r[1] + r[3] / 2) * 256 + r[0] + r[2] / 2];
        const VanaLabelGlyph* l = at ? &g_cf.labels[at - 1] : NULL;
        if (l && abs(r[0] - l->u) <= 2 && abs(r[1] - l->v) <= 2 && abs(r[2] - l->w) <= 2 && abs(r[3] - l->h) <= 2)
            return *top = r[1], l;
    }
    return NULL;
}

/* A picture the game draws in a tooltip's rows that True Font has no letter for (an element's icon, an auto-translate brace,
 * a letter past ASCII), kept while the tooltips' rows are moved (cf_tips_moved) so it moves down with its row (cf_spread):
 * a sprite of at most 20 x 20 of the interface's pixels (not the item's picture, nor the window's frame's sides). Its box. */
static int cf_icon(const GfxDraw* d, float lo[2], float hi[2])
{
    float tlo[2], thi[2];
    if (cf_area() != VANA_FONT_TIPS || !cf_tips_moved() || !cf_quad(d, lo, hi, tlo, thi))
        return 0;
    const float w = hi[0] - lo[0], h = hi[1] - lo[1];
    return w >= 4.0f && w <= 20.0f && h >= 4.0f && h <= 20.0f;
}

/* The same state as a kept run's: everything but the vertices that a glyph draw can differ by */
static int cf_same(const GfxDraw* a, const GfxDraw* b)
{
    return !memcmp(&a->vs, &b->vs, sizeof a->vs) && !memcmp(&a->fs, &b->fs, sizeof a->fs) &&
        !memcmp(&a->pipe, &b->pipe, sizeof a->pipe) && !memcmp(&a->depth, &b->depth, sizeof a->depth) && a->tex[0] == b->tex[0] &&
        !memcmp(&a->samp[0], &b->samp[0], sizeof a->samp[0]) && !memcmp(a->u.vp, b->u.vp, sizeof a->u.vp) &&
        !memcmp(a->u.params, b->u.params, sizeof a->u.params) && !memcmp(a->u.tfactor, b->u.tfactor, sizeof a->u.tfactor) &&
        !memcmp(a->u.stride, b->u.stride, sizeof a->u.stride) && !memcmp(a->u.offset, b->u.offset, sizeof a->u.offset) &&
        !memcmp(a->vp, b->vp, sizeof a->vp) && a->text_shadow == b->text_shadow && a->text_alpha == b->text_alpha &&
        a->small_rt == b->small_rt && a->overlay == b->overlay;
}

/* The target a draw goes to, if the interface's text can be drawn into it, as a guest pointer: a texture's surface (the menu
 * target), as its texture, not the world's, which has the names over heads; or the back buffer, which the game draws its
 * interface onto itself when it has no menu target (the file's comment). 0 for another, and for VanaCore's own draws. */
static uint32_t cf_target(void)
{
    g_cf.n.draws++;
    if (!g_squeeze || g_own_pixels)
        return g_cf.n.own++, 0;
    Obj* rt = obj(g_dev.rt);
    uint32_t face, level;
    const uint32_t target = g_dev.rt && g_dev.rt == g_dev.backbuffer ? g_dev.rt : rt && rt->container ? rt->container : 0;
    if (!target)
        return g_cf.n.no_rt++, 0;
    if (target != g_dev.backbuffer && g_scene.rt && surface_gpu(rt, &face, &level) == g_scene.rt)
        return g_cf.n.scene++, 0;
    /* the game's own word, when read: its menu target alone, or the screen alone (a texture then is another: the world's
     * names over heads, an effect's copy) */
    if (g_cf.told_known && target != (g_cf.told ? g_cf.told : g_dev.backbuffer))
    {
        if (g_chat_drawing)
            g_cf.n.chat_other++;
        else
            g_cf.n.told_other++;
        return 0;
    }
    return target;
}

/* The kept runs' state the same as `d`'s (the last one's first: glyphs come a run at a time), a new one made if none is, and
 * room for one more glyph's vertices: the run, or CF_RUNS for no room */
static uint32_t cf_run(const GfxDraw* d)
{
    uint32_t run = g_cf.nruns;
    if (g_cf.nruns && cf_same(&g_cf.runs[g_cf.last], d))
        run = g_cf.last;
    for (uint32_t r = 0; run == g_cf.nruns && r < g_cf.nruns; ++r)
        if (cf_same(&g_cf.runs[r], d))
            run = r;
    if (run == CF_RUNS)
        return CF_RUNS;
    if (g_cf.cap < (g_cf.nkept + 1) * CF_STRIDE)
    {
        uint32_t cap = g_cf.cap ? g_cf.cap * 2 : 64 * CF_STRIDE;
        uint8_t* p = (uint8_t*)realloc(g_cf.bytes, cap);
        if (!p)
            return CF_RUNS;
        g_cf.bytes = p, g_cf.cap = cap;
    }
    if (run == g_cf.nruns)
        g_cf.runs[g_cf.nruns++] = *d;
    g_cf.last = run;
    return run;
}

/* The grid's cells a box covers: from c0 to c1 (each across, down), clamped to the grid */
static void cf_cells(const float lo[2], const float hi[2], int c0[2], int c1[2])
{
    for (int c = 0; c < 2; ++c)
    {
        const float a = floorf(lo[c] / CF_CELL), b = floorf(hi[c] / CF_CELL);
        c0[c] = a < 0.0f ? 0 : a > CF_GRID - 1 ? CF_GRID - 1 : (int)a;
        c1[c] = b < 0.0f ? 0 : b > CF_GRID - 1 ? CF_GRID - 1 : (int)b;
    }
}

/* Kept glyph i into the grid's cells and the box round them all */
static void cf_place(uint32_t i)
{
    const CfGlyph* g = &g_cf.kept[i];
    if (!i)
    {
        memset(g_cf.head, -1, sizeof g_cf.head), g_cf.nentries = 0;
        memcpy(g_cf.box, g->lo, 8), memcpy(g_cf.box + 2, g->hi, 8);
    }
    for (int c = 0; c < 2; ++c)
        g_cf.box[c] = fminf(g_cf.box[c], g->lo[c]), g_cf.box[2 + c] = fmaxf(g_cf.box[2 + c], g->hi[c]);
    int c0[2], c1[2];
    cf_cells(g->lo, g->hi, c0, c1);
    for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0] && g_cf.nentries < 4 * CF_MOST; ++x)
        {
            const uint32_t e = g_cf.nentries++;
            g_cf.who[e] = (uint16_t)i, g_cf.next[e] = g_cf.head[y * CF_GRID + x], g_cf.head[y * CF_GRID + x] = (int32_t)e;
        }
}

/* A glyph picture's scale: within 6% of the font's own size, that (its pixels the screen's, not filtered soft) */
static float cf_snap(float s) { return fabsf(s - 1.0f) < 0.06f ? 1.0f : s; }

/* A font whose letters are laid a word at a time from where the game's word starts, not each centred in its sprite: the menus'
 * labels' (the buttons', the titles', the clock's: cf_label_word) and the big letters' (the Status window's words and numbers, the
 * Equipment window's stats: cf_big_words) */
static int cf_worded(int font)
{
    return font == VANA_FONT_BUTTONS || font == VANA_FONT_TITLES || font == VANA_FONT_STATUS || font == VANA_FONT_CLOCK ||
           font == VANA_FONT_PARTY || font == VANA_FONT_ALLIANCE || cf_whole(font) || font == VANA_FONT_MAINMENU ||
           font == VANA_FONT_ACTIONMENU || font == VANA_FONT_MACROBAR || font == VANA_FONT_EQUIP || font == VANA_FONT_BUFFS ||
           font == VANA_FONT_TARGET || font == VANA_FONT_TARGET_TITLE || font >= VANA_FONT_PARTY_NUMBERS;
}

/* A font of big letters' rows (cf_big_words): the Status area's, the party list's, the alliance lists' (cf_list_area), cf_whole's
 * (the trade windows', Current Gil's, the Equipment window's), the Target window's name's */
static int cf_big(int font)
{
    return font == VANA_FONT_STATUS || font == VANA_FONT_PARTY || font == VANA_FONT_ALLIANCE || cf_whole(font) || font == VANA_FONT_TARGET;
}

/* A font of the windows' font sheet text laid a word at a time too (cf_row_words): each word from where the game's starts, its
 * letters by the font's own advances, not each centred in the game's width for it (chatfont.cpp still centres the pictures, so
 * that width is known here: cf_glyph takes the centring off). The menus', the item lists', the help's, the chat input's, the
 * auto-translate list's, the status timers'; not the tooltips' (their box is fitted to the game's widths: d3d8_tips.h) nor the
 * chat's (the game lays it with the font's own widths). cf_whole's too (the trade windows'): their labels' and big letters'
 * pictures centred so as well are laid as the Status area's (cf_big_words) */
static int cf_per_word(int font)
{
    return font == VANA_FONT_MENUS || font == VANA_FONT_LISTS || font == VANA_FONT_ITEMS || font == VANA_FONT_HELP || font == VANA_FONT_INPUT ||
           font == VANA_FONT_PHRASES || font == VANA_FONT_EQUIP || font == VANA_FONT_BUFFS || font == VANA_FONT_TARGET || cf_whole(font);
}

/* Two kept glyphs on one row of text: their pens within a few pixels down of each other (the game's pictures stand some
 * letters 2 lower than the row's: the Status window's job name "Ninja" its j) */
static int cf_same_row(const CfGlyph* a, const CfGlyph* b) { return fabsf(a->py - b->py) <= 2.5f; }

/* What a glyph draw is (chatfont_keep): its character, the font it's kept in (VANA_FONT_*), a label's letter's sprite (and
 * its top and scale: cf_label), or a picture of the game's own (cf_icon) */
typedef struct CfWhat
{
    uint32_t code;
    int which, top, pic;
    float scale[2];
    const VanaLabelGlyph* l;
} CfWhat;

/* The font of label letter `w` (w->l, w->code) in the Equipment window: a small letter or digit of the "menu2fon" sheet's (sheet
 * 1) the slot names', every other the stats': the big letters (the element numbers, Attack's and Defense's), the "font    font"
 * sheet's small ones (the words "Attack" and "Defense" before those numbers, the numbers' signs, "-20": laid in their words,
 * cf_big_words, and kept when the slot names are left out, cf_hidden). The words were the slot names' and went with them when
 * they were Hidden, with equip.stats off (reported 2026-11-07; on, equipstats.cpp moves them out of the window) */
static int cf_equip_font(const CfWhat* w)
{
    const int c = (int)w->l->code + 0x20; /* (the label's: w->code is set from it after, cf_what) */
    const int named = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    return w->l->cap > 9 || !named || w->l->sheet != 1 ? VANA_FONT_EQUIP_STATS : VANA_FONT_EQUIP;
}

/* Letter `w` (a font sheet cell or a label's letter) left out altogether (d3d8_hide_letters): any of an area whose True Font
 * size is 0 (its font's, a small digit its window's), or one of the Equipment window's slot names' (a letter in its font: not
 * a stack's count), or (`digit`) the small digit a label draws after one ("Ear1") */
static int cf_hidden(const CfWhat* w, int digit)
{
    const int c = (int)w->code + 0x20;
    const int area = digit ? g_cf.area : w->which;
    if (area >= 0 && area < VANA_FONTS && g_cf.hide[area] == 2)
        return 1;
    if (g_cf.hide[VANA_FONT_EQUIP] != 1)
        return 0;
    if (digit)
        return g_cf.area == VANA_FONT_EQUIP && c >= '0' && c <= '9';
    return w->which == VANA_FONT_EQUIP && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

#include "d3d8_chatfont_jp.h" /* cf_jp_label, cf_jp_what: the Japanese menu labels' words */

/* Glyph draw `d`'s box (lo, hi) and what it is (w): 0 for none VanaCore keeps, 2 for one left out (cf_hidden). A font sheet's
 * cell, else a menu label's letter (a button's while one is drawn, else a title's) or a big letter, with a letter of its
 * font's; else a tooltip's icon (the game's picture, moved with its row). */
static int cf_what(const GfxDraw* d, float lo[2], float hi[2], CfWhat* w)
{
    const int cell = cf_cell(d, lo, hi, &w->code);
    w->which = g_chat_drawing ? VANA_FONT_CHAT : g_cl.on ? VANA_FONT_INPUT : cf_area(), w->top = 0, w->pic = 0, w->l = NULL;
    w->scale[0] = w->scale[1] = 1.0f;
    if (!cell && !g_chat_drawing && (w->l = cf_jp_label(d, lo, hi, &w->top, w->scale)) != NULL && !g_cf.clock && w->l->cap <= 9 &&
        w->scale[1] < 0.85f && cf_list_area() < 0 && g_cf.area != VANA_FONT_BUFFS)
    {
        w->code = w->l->code;
        if (cf_hidden(w, 1)) /* (the slot names left out: their "Ear1", "Ring2" digits with them) */
            return 2;
        w->l = NULL; /* (a label's letter drawn smaller than its sprite: the Equipment window's squares' "Ear1", "Ring2" digits,
                      * beside the game's own small letters there, stay the game's; the labels and symbols are drawn a texel to
                      * a pixel down, a squeezed button's across only. The party and alliance lists' small MP and TP are
                      * their font's) */
    }
    if (w->l && g_cf.area == VANA_FONT_PHRASES)
        w->l = NULL; /* (the auto-translate list's count, "1/44": its "/" is a label's letter, its digits the game's own; it stays
                      * the game's whole, not the titles' "/" beside the game's digits: reported 2026-11-06) */
    if (w->l)
        w->which = g_cf.clock ? VANA_FONT_CLOCK
                 : cf_list_area() >= 0 ? cf_list_area()
                 : g_cf.area == VANA_FONT_MACROBAR ? VANA_FONT_MACROBAR /* (the keys' "Ctrl" / "Alt" were the titles', their
                                                                         * numbers the game's: reported 2026-11-06) */
                 : g_cf.area == VANA_FONT_BUFFS ? VANA_FONT_BUFFS /* (the timers are the labels' sheet's letters: they were
                                                                   * Status & Numbers', reported 2026-11-07) */
                 : g_cf.area == VANA_FONT_EQUIP ? cf_equip_font(w) /* (its slot names were the titles' or the buttons', its
                                                                     * numbers the Status area's: reported 2026-11-06) */
                 : cf_whole(g_cf.area) ? g_cf.area /* (its buttons' and labels' too: Trade Windows edited only the amounts) */
                 /* (the Target window's: the target's name is the big letters at about half, 0x101C7F10's shapes of
                  * 0x101C7390(0), not the font sheet's, so it was Status & Numbers' and target.font did nothing (read live
                  * 2026-11-07); "Target" the menu2fon caption's labels, the titles' until asked for apart 2026-11-07) */
                 : g_cf.area == VANA_FONT_TARGET ? (w->l->cap > 9 ? VANA_FONT_TARGET : VANA_FONT_TARGET_TITLE)
                 : w->l->cap > 9 ? VANA_FONT_STATUS
                 : g_cf.button ? (g_cf.area == VANA_FONT_MAINMENU || g_cf.area == VANA_FONT_ACTIONMENU ? g_cf.area
                                                                                                     : VANA_FONT_BUTTONS)
                               : VANA_FONT_TITLES,
        w->code = w->l == &cf_jp.l ? cf_jp.code : w->l->code;
    if ((cell || w->l) && cf_hidden(w, 0))
        return 2;
    const CfFont* f = &g_cf.font[w->which];
    cf_want_glyph(w->which, w->code, cell || w->l == &cf_jp.l); /* (past ASCII with no glyph yet: chatfont.cpp builds it for a frame to come) */
    if ((cell || w->l) && f->on && (f->slot[w->code] >= 0 || (cell && g_cl.on))) /* (the chat line's: all of them, its picture's) */
        return 1;
    if (g_chat_drawing || !cf_icon(d, lo, hi))
        return 0;
    w->which = VANA_FONT_TIPS, w->code = 0, w->pic = 1, w->l = NULL, w->scale[0] = w->scale[1] = 1.0f;
    return 1;
}

/* Draw `d`, into `target`, says where the interface is drawn: the game's own word on it (d3d8_set_interface_target, which
 * cf_target has kept to), else the chat rows' draws (the chat's own windows), else, while the chat isn't drawn (CF_CHAT_GONE
 * frames: an addon's chat in place of the game's, YunaUI's and fancychat's set the windows' draw byte to 0), a letter of the
 * font sheet's, which only the interface draws (the world's target has none: cf_target). Its scale is taken too, from the
 * chat's draws while there are any (with no font as well: the letters' size is worked out from it where the target is laid). */
#define CF_CHAT_GONE 3
/* The interface target's drawn pixels per interface pixel (g_cf.k), from interface draw `d` (a chat row's, a letter's): its
 * drawn clip is whole pixels, so a clip CF_WIDE units or more across (or down) gives the ratio within a thousandth, which keeps
 * the fonts (chatfont.cpp builds them again past 0.002); a narrower clip's ratio is taken only until a wide one's is. The chat
 * windows' clip is 134 units tall: taken each frame, its ratio stepped 0.952 to 0.970 as the window eased open at Fill window,
 * and the chat's font was built again every frame (2026-11-05) */
#define CF_WIDE 512.0f
static void cf_take_scale(const GfxDraw* d)
{
    for (int c = 0; c < 2; ++c)
    {
        const int wide = d->u.vp[2 + c] >= CF_WIDE;
        if (wide || !g_cf.k_wide[c])
            g_cf.k[c] = cf_scale(d, c), g_cf.k_wide[c] = wide;
    }
}

static void cf_learn(const GfxDraw* d, uint32_t target)
{
    uint32_t code;
    float lo[2], hi[2];
    if (g_chat_drawing)
        g_cf.chat_ago = 0;
    const int letter = !g_chat_drawing && cf_cell(d, lo, hi, &code);
    const int from = g_chat_drawing ? 0 : g_cf.told_known ? 1 : g_cf.chat_ago >= CF_CHAT_GONE && letter ? 2 : -1;
    if (target == g_cf.target) /* (known already: the scale taken afresh from the chat's draws and the letters) */
    {
        if (g_chat_drawing || letter)
            cf_take_scale(d);
        return;
    }
    if (from < 0)
        return;
    static const char* const kFrom[3] = { "the chat's rows", "the game's own display", "the font sheet's letters (the game's chat isn't drawn)" };
    static int told[3];
    uint32_t* n[3] = { &g_cf.n.learn_chat, &g_cf.n.learn_told, &g_cf.n.learn_cell };
    ++*n[from];
    if (!told[from]++ || g_cf.debug)
        rt_log("[recomp] d3d8: True Font: the interface is drawn into %s, found from %s\n",
            target == g_dev.backbuffer ? "the screen itself (no menu target: the menus' size is the screen's)" : "its menu target",
            kFrom[from]);
    g_cf.target = target, g_cf.k_wide[0] = g_cf.k_wide[1] = 0;
    cf_take_scale(d);
}

/* Glyph draw `d` shows nothing: its diffuse's alpha 0 at all four corners where the first stage takes the alpha from it
 * (SELECTARG or MODULATE of D3DTA_DIFFUSE, as the game's letters are). VanaCore's own windows hide the game's letters so
 * (statuswindow.cpp, the Status window's labels): True Font kept them, and their outline (which takes no alpha from the
 * letter) stood dark over the window at the font's size. Not kept: the game's draw, which shows nothing. */
static int cf_unseen(const GfxDraw* d)
{
    const uint32_t stride = (uint32_t)d->u.stride[0], at = (uint32_t)d->u.offset[GFX_R_DIFFUSE];
    const GfxStage* st = &d->fs.st[0];
    const uint8_t* v = (const uint8_t*)d->data[0];
    if (!v || d->fs.prog || !d->vs.el[GFX_R_DIFFUSE].used || d->vs.el[GFX_R_DIFFUSE].stream || at + 4 > stride)
        return 0;
    const int a1 = (st->aa1 & 0x3F) == 0, a2 = (st->aa2 & 0x3F) == 0; /* (D3DTA_DIFFUSE) */
    if (!((st->aop == 2 && a1) || (st->aop == 3 && a2) || (st->aop >= 4 && st->aop <= 6 && (a1 || a2))))
        return 0;
    for (uint32_t k = 0; k < 4; ++k)
        if (v[k * stride + at + 3]) /* (the colour's alpha: its high byte) */
            return 0;
    return 1;
}

/* The game's glyph draw `d`, about to go into the interface's target: kept (and not drawn now) when VanaCore has its character,
 * in the chat's font while the game draws a chat row, in its window's area's otherwise, a label's letter in the buttons' (a big
 * letter in the Status window's); a tooltip's icon kept to be moved with its row (cf_spread) */
/* A key's "Ctrl" or "Alt" in the macro bar (a "menu2fon" letter, its shadow copy too, drawn in a VANA_FONT_MACROBAR window)
 * while [client] macrobar.prefix=0 has the keys show their numbers alone: not drawn, the game's letters or True Font's */
static int cf_macro_prefix(const GfxDraw* d)
{
    float lo[2], hi[2], scale[2];
    int top;
    if (!g_cf.macro_bare || g_cf.area != VANA_FONT_MACROBAR || g_chat_drawing)
        return 0;
    const VanaLabelGlyph* l = cf_label(d, lo, hi, &top, scale);
    return l && l->sheet == 1;
}

static int chatfont_keep(const GfxDraw* d)
{
    cf_printing(d);
    if (cf_macro_prefix(d))
        return 1;
    const uint32_t target = cf_target();
    if (!target)
        return 0;
    if (g_chat_drawing)
        g_cf.n.chat++;
    if (g_cf.nkept && target != g_cf.target) /* (another target than the kept glyphs': the game's) */
    {
        g_cf.n.other++;
        cf_say(&g_cf.n.say_other, "a draw into another target than the kept letters'", d);
        return 0;
    }
    cf_learn(d, target);
    if (target != g_cf.target)
    {
        g_cf.n.other++;
        cf_say(&g_cf.n.say_other, "a draw into another target than the interface's", d);
        return 0;
    }
    CfWhat w;
    float lo[2], hi[2];
    const int what = cf_jp_what(d, lo, hi, &w);
    if (!what)
    {
        uint32_t code;
        if (cf_cell(d, lo, hi, &code))
            g_cf.n.cell_nofont++, cf_say(&g_cf.n.say_cell, "a letter of the game's with no letter of True Font's for it", d);
        return 0;
    }
    if (what == 2) /* (left out: neither drawn nor kept, cf_hidden) */
        return 1;
    if (w.pic)
        g_cf.n.icons++;
    else if (w.l)
        g_cf.n.labels++;
    else
        g_cf.n.cells++, cf_say(&g_cf.n.say_cell, g_cf.printer.on == 2 ? "a letter the game's printer draws, kept" : "a letter of the game's, kept", d);
    if (g_cf.rest || g_cf.nkept >= CF_MOST)
        return g_cf.n.rest++, 0;
    const CfFont* f = &g_cf.font[w.which];
    const VanaLabelGlyph* l = w.l;
    const float* scale = w.scale;
    const float* vp = d->u.vp; /* wholly outside its clip, or see-through: the game's, which shows nothing */
    if (hi[0] <= vp[0] || lo[0] >= vp[0] + vp[2] || hi[1] <= vp[1] || lo[1] >= vp[1] + vp[3] || cf_unseen(d))
        return 0;
    const uint32_t run = cf_run(d);
    if (run == CF_RUNS)
        return 0;
    memcpy(g_cf.bytes + g_cf.nkept * CF_STRIDE, d->data[0], 4 * (uint32_t)d->u.stride[0]);
    CfGlyph* g = &g_cf.kept[g_cf.nkept];
    /* (D3D8's pixel centres: the cell is drawn from half a pixel up and left; a label's letter stands on its baseline, `base`
     * below its sprite's top, where the font's letters stand f->baseline below the pen) */
    g->px = lo[0] + 0.5f, g->py = lo[1] + 0.5f + (l ? (float)(l->v + l->base - w.top) * scale[1] - f->baseline : 0.0f);
    const float factor = l && l->cap ? (float)l->cap / 9.0f : 1.0f; /* (the labels' capitals are 9 texels) */
    g->scale[0] = cf_snap(scale[0] * factor), g->scale[1] = cf_snap(scale[1] * factor), g->label_w = l ? hi[0] - lo[0] : 0.0f;
    const float in = w.pic ? 1.0f : 2.0f; /* (a picture's whole sprite, a pixel in) */
    g->lo[0] = lo[0] + in, g->lo[1] = lo[1] + in, g->hi[0] = hi[0] - in, g->hi[1] = hi[1] - 1.0f;
    g->code = (uint16_t)w.code, g->run = (uint8_t)run, g->drawn = 0, g->font = (uint8_t)w.which, g->pic = (uint8_t)w.pic, g->dy = 0.0f;
    cf_jp_kept(g, l); /* (a Japanese label's words laid in the whole label's width) */
    g->row_x = g->px, g->color = w.which == VANA_FONT_MACROBAR ? g_cf.macro_color : 0; /* (macrobar.font.color) */
    g->rarity = cf_tip_rarity(w.which), g->fade = 0, g->typed = cf_tip_typed(w.which);
    g->worn = cf_tip_worn(w.which), g->line = (uint8_t)(g_cl.on && !w.l && !w.pic);
    g->cut = (uint8_t)(l && l->sheet == 1), g->rolled = (uint8_t)(g_cf.rolling > 0);
    g->centred = g_cf.centring, g->right = (uint8_t)(g_cf.centring ? g_cf.right : 0), g->cx = g_cf.centre_x;
    for (int c = 0; c < 2; ++c) /* (the printer's cell is 16 x 16 times its size: a label's and a picture's are as they are) */
        g->gs[c] = l || w.pic || !(hi[c] - lo[c] > 1.0f) ? 1.0f : cf_snap((hi[c] - lo[c]) / 16.0f);
    const float cap = l ? lo[1] + (float)((int)l->v + (int)l->base - w.top - (int)l->cap) * scale[1] : lo[1]; /* (its row's capitals' top) */
    g->clip = wc_keep(lo, hi, cap, g_chat_drawing, !g_cf.nkept);
    cf_place(g_cf.nkept++);
    g_cf.n.kept++;
    return 1;
}

/* Kept glyph i into the target as the game drew it */
static void cf_game(uint32_t i)
{
    static GfxDraw d;
    CfGlyph* g = &g_cf.kept[i];
    d = g_cf.runs[g->run];
    d.data[0] = d.cpu[0] = g_cf.bytes + i * CF_STRIDE;
    d.size[0] = d.cpu_size[0] = 4 * (uint32_t)d.u.stride[0];
    g->drawn = 1;
    gfx_draw(&d);
}

/* Where draw `d` goes: its vertices' box, or its clip when they're in a buffer */
static void cf_box(const GfxDraw* d, float lo[2], float hi[2])
{
    const uint32_t stride = (uint32_t)d->u.stride[0], pos = (uint32_t)d->u.offset[GFX_R_POSITION];
    lo[0] = d->u.vp[0], lo[1] = d->u.vp[1], hi[0] = d->u.vp[0] + d->u.vp[2], hi[1] = d->u.vp[1] + d->u.vp[3];
    if (!d->vs.rhw || !d->data[0] || d->buf[0] || d->vs.el[GFX_R_POSITION].stream || stride < 16 || pos + 8 > stride ||
        d->size[0] < stride || d->size[0] / stride > 4096)
        return;
    lo[0] = lo[1] = 1e30f, hi[0] = hi[1] = -1e30f;
    for (uint32_t i = 0; i < d->size[0] / stride; ++i)
    {
        float p[2];
        memcpy(p, (const uint8_t*)d->data[0] + i * stride + pos, 8);
        for (int c = 0; c < 2; ++c)
            lo[c] = p[c] < lo[c] ? p[c] : lo[c], hi[c] = p[c] > hi[c] ? p[c] : hi[c];
    }
}

static void cf_onto_screen(void);

/* A draw that isn't a kept glyph, its targets set: one into the interface's target drawn where kept glyphs stand (after them,
 * so over them: a window opened on another) has them drawn into the target first, as the game would have; on the screen
 * (no menu target), every kept glyph drawn in its font first (cf_onto_screen), not VanaCore's own layer's draws, which go on
 * the screen after the frame's end. Not one that only adds light (its blend's destination ONE: a highlight, another glyph),
 * which hides nothing and comes out the same in either order. */
/* A draw reading the interface's target (a texture) counted: onto the screen, a laying (chatfont_laid follows it after the
 * draw, when it can), into another target something else's (diagnostics) */
static void cf_watch_laying(const GfxDraw* d)
{
    if (!g_cf.target || g_cf.target == g_dev.backbuffer || g_dev.cur.tex[0] != g_cf.target)
        return;
    if (g_dev.rt == g_dev.backbuffer)
        g_cf.n.lay_seen++, cf_say(&g_cf.n.say_lay, "the interface's target read by a draw onto the screen (a laying?)", d);
    else
        g_cf.n.lay_elsewhere++, cf_say(&g_cf.n.say_lay, "the interface's target read by a draw into another target", d);
}

static int cf_under_add(uint32_t i);
static void cf_under(void);

/* Whether draw `d`, about to be made, goes where kept glyphs stand (on the grid; `screen`: onto the back buffer, the
 * interface's target itself); the first such glyph (or any). Those it covers go into the target first, as the game drew
 * them (cf_game), the Equipment window's slot names in True Font (cf_under) */
static int cf_covers(const GfxDraw* d, int screen)
{
    float lo[2], hi[2];
    cf_box(d, lo, hi);
    if (!(hi[0] > g_cf.box[0] && lo[0] < g_cf.box[2] && hi[1] > g_cf.box[1] && lo[1] < g_cf.box[3]))
        return 0;
    int c0[2], c1[2], any = 0;
    cf_cells(lo, hi, c0, c1);
    for (int y = c0[1]; y <= c1[1]; ++y)
        for (int x = c0[0]; x <= c1[0]; ++x)
            for (int32_t e = g_cf.head[y * CF_GRID + x]; e >= 0; e = g_cf.next[e])
            {
                const CfGlyph* g = &g_cf.kept[g_cf.who[e]];
                if (g->drawn || !(hi[0] > g->lo[0] && lo[0] < g->hi[0] && hi[1] > g->lo[1] && lo[1] < g->hi[1]))
                    continue;
                /* (the auto-translate list's own drawing after its rows, its cursor's bar over the chosen one, isn't over
                 * its letters: that row was all the game's, "[Check]" amid True Font's: reported 2026-11-06. Another
                 * window drawn over the list still is) */
                if (g->font == VANA_FONT_PHRASES && g_cf.area == VANA_FONT_PHRASES)
                    continue;
                if (screen)
                    return 1;
                any = 1;
                g_cf.n.cover_game++;
                if (g->line) /* (the chat line's picture would go over what covers it: the whole line the game's, under it) */
                    cf_line_game();
                else if (!cf_under_add(g_cf.who[e]))
                    cf_game(g_cf.who[e]);
            }
    cf_under();
    return any;
}

static void chatfont_cover(const GfxDraw* d)
{
    cf_watch_laying(d);
    Obj* rt = g_cf.nkept && !g_chat_drawing && !(d->pipe.blend && d->pipe.dst == 2) ? obj(g_dev.rt) : NULL;
    const int screen = rt && g_dev.rt == g_dev.backbuffer;
    if (!rt || (screen ? g_dev.rt : rt->container) != g_cf.target || (screen && g_own_pixels))
        return;
    if (screen && g_cf.assume) /* (the target laid by assumption: the kept glyphs go over its laying, which is any draw now) */
        return;
    if (cf_covers(d, screen) && screen)
        g_cf.n.cover_screen++, cf_onto_screen();
}

/* Where the laying of the interface's target onto the screen (`lay`) puts the target's picture: screen x = map[0] + map[1] u,
 * y = map[2] + map[3] v, in the drawn pixels, u and v across the target 0 to 1 */
static int cf_map(const GfxDraw* lay, float map[4])
{
    const uint32_t stride = (uint32_t)lay->u.stride[0];
    uint32_t pos, tc;
    if (!cf_vertices(lay, 4, 256, &pos, &tc) || !(lay->u.vp[2] > 0) || !(lay->u.vp[3] > 0))
        return 0;
    for (int c = 0; c < 2; ++c)
    {
        float plo = 1e30f, phi = -1e30f, tlo = 0, thi = 0;
        for (uint32_t i = 0; i < 4; ++i)
        {
            float p[2], uv[2];
            memcpy(p, (const uint8_t*)lay->data[0] + i * stride + pos, 8);
            memcpy(uv, (const uint8_t*)lay->data[0] + i * stride + tc, 8);
            if (p[c] < plo)
                plo = p[c], tlo = uv[c];
            if (p[c] > phi)
                phi = p[c], thi = uv[c];
        }
        if (!(fabsf(thi - tlo) > 1e-4f) || !(phi > plo))
            return 0;
        const float a = (phi - plo) / (thi - tlo); /* the laying's units across the whole target */
        /* (D3D8's pixel centres, unless the laying is drawn with today's: its picture half a pixel right and down) */
        const float b = plo + (lay->vs.centered ? 0.0f : 0.5f) - tlo * a;
        const float k = (float)lay->vp[2 + c] / lay->u.vp[2 + c]; /* the drawn pixels per unit */
        map[2 * c] = (float)lay->vp[c] + (b - lay->u.vp[c]) * k, map[2 * c + 1] = a * k;
    }
    return map[1] > 0.0f && map[3] > 0.0f;
}

/* An interface pixel `p` of a kept run (across: c 0, down: 1) on the screen: through the run's own mapping into its target's
 * drawn pixels (rt: their width and height), then the laying's */
static float cf_screen(const GfxDraw* run, int c, float p, const float map[4], const float rt[2])
{
    return map[2 * c] + map[2 * c + 1] * ((float)run->vp[c] + (p - run->u.vp[c]) * cf_scale(run, c)) / rt[c];
}

static int cf_texture(CfFont* f)
{
    if (!f->tex && f->atlas && (f->tex = gfx_tex_create(GFX_TEX_2D, FMT_A8R8G8B8, f->aw, f->ah, 1, GFX_USE_SAMPLE)) != NULL)
        gfx_tex_upload(f->tex, 0, 0, f->atlas, f->aw * 4);
    return f->tex != NULL;
}

/* A glyph's picture in the atlas and where it goes from the pen and the baseline (the screen's pixels): its letter's, or its
 * outline's */
typedef struct CfPic
{
    float ax, ay, w, h, x, y;
} CfPic;

static CfPic cf_pic(const VanaChatGlyph* m, int outline)
{
    CfPic p = { m->ax, m->ay, m->w, m->h, m->x, m->y };
    if (outline)
        p.ax = m->oax, p.ay = m->oay, p.w = m->ow, p.h = m->oh, p.x = m->ox, p.y = m->oy;
    return p;
}

/* The clip the game gave kept run `d` (its target's drawn pixels) on the screen, within the laying's (`lay`; map, rt:
 * cf_screen's), whole pixels: left, top, right, bottom. 0 when none of it shows. */
static int cf_clip(const GfxDraw* d, const GfxDraw* lay, const float map[4], const float rt[2], float c[4])
{
    for (int k = 0; k < 2; ++k)
    {
        const float a = map[2 * k] + map[2 * k + 1] * (float)d->vp[k] / rt[k];
        const float b = map[2 * k] + map[2 * k + 1] * (float)(d->vp[k] + d->vp[2 + k]) / rt[k];
        c[k] = fmaxf(floorf(a + 0.5f), (float)lay->vp[k]), c[2 + k] = fminf(floorf(b + 0.5f), (float)(lay->vp[k] + lay->vp[2 + k]));
    }
    return c[2] > c[0] && c[3] > c[1];
}

/* Kept run `d`'s copy made to go over the laid target: in clip c (cf_clip: the screen's pixels as they are), its vertices v
 * (today's pixel centres), no depth, no chat.shadow (the game font's: True Font has its outline). Into the interface's target
 * itself (g_cf.into, cf_under): its clip that many of the target's pixels to one of c's, which the vertices are laid in */
static void cf_over(GfxDraw* d, const GfxDraw* lay, const float c[4], uint8_t* v)
{
    for (int k = 0; k < 2; ++k)
    {
        const float in = g_cf.into[k] > 0.0f ? g_cf.into[k] : 1.0f;
        d->vp[k] = (uint32_t)floorf(c[k] * in + 0.5f), d->vp[2 + k] = (uint32_t)floorf((c[2 + k] - c[k]) * in + 0.5f);
    }
    d->u.vp[0] = c[0], d->u.vp[1] = c[1], d->u.vp[2] = c[2] - c[0], d->u.vp[3] = c[3] - c[1];
    d->vs.centered = 1;
    d->depth.zenable = d->depth.zwrite = d->depth.stencil = 0;
    d->small_rt = lay->small_rt, d->overlay = lay->overlay;
    d->data[0] = d->cpu[0] = v;
    d->size[0] = d->cpu_size[0] = 4 * (uint32_t)d->u.stride[0];
    d->text_shadow = d->text_alpha = 0;
}

/* A kept run's mapping onto the screen each way (cf_screen): its pixel at the interface's 0, and how many to one (wc_cut_onto) */
static void cf_affine(const GfxDraw* run, const float map[4], const float rt[2], float aff[4])
{
    for (int c = 0; c < 2; ++c)
        aff[2 * c] = cf_screen(run, c, 0.0f, map, rt), aff[2 * c + 1] = cf_screen(run, c, 1.0f, map, rt) - aff[2 * c];
}

/* Kept picture i (cf_icon) over the laid target: the game's own sprite, its texture and state, where the game drew it into
 * the target but moved down with its row (CfGlyph.dy), its middle as much further from its row's left (row_x) and the sprite as
 * much larger as its row's letters are (scale: d3d8_tips.h), filtered as the laying is */
static void cf_picture(uint32_t i, const GfxDraw* lay, const float map[4], const float rt[2])
{
    static GfxDraw d;
    static uint8_t v[4 * 64];
    const CfGlyph* g = &g_cf.kept[i];
    float c[4];
    d = g_cf.runs[g->run];
    const uint32_t stride = (uint32_t)d.u.stride[0], pos = (uint32_t)d.u.offset[GFX_R_POSITION];
    if (!cf_clip(&d, lay, map, rt, c))
        return;
    const float s = g->scale[0], mid[2] = { (g->lo[0] + g->hi[0]) * 0.5f, (g->lo[1] + g->hi[1]) * 0.5f };
    const float to[2] = { g->row_x + (mid[0] - g->row_x) * s, mid[1] + g->dy };
    memcpy(v, g_cf.bytes + i * CF_STRIDE, 4 * stride);
    for (uint32_t k = 0; k < 4; ++k)
    {
        float p[2];
        memcpy(p, v + k * stride + pos, 8);
        /* (D3D8's pixel centres: the sprite's edges half a pixel down and right of its vertices) */
        const float q[2] = { cf_screen(&d, 0, to[0] + (p[0] - mid[0]) * s + 0.5f, map, rt),
            cf_screen(&d, 1, to[1] + (p[1] - mid[1]) * s + 0.5f, map, rt) };
        memcpy(v + k * stride + pos, q, 8);
    }
    float aff[4];
    cf_affine(&d, map, rt, aff);
    cf_over(&d, lay, c, v);
    d.samp[0].mag = d.samp[0].min = 2; /* LINEAR */
    if (wc_cut_onto(&d, v, g->clip, aff)) /* (window.corners: inside its window's) */
        gfx_draw(&d);
}

/* Kept glyph i drawn from its font's atlas at x (its pen across, the interface's pixels), over the laid target (`lay`; map, rt:
 * cf_screen's): its letter in the game's state, or its outline (black, the outline's ink times the font's outline opacity,
 * over what's there: text_flush's shadow) */
static void cf_glyph(uint32_t i, float x, const GfxDraw* lay, const float map[4], const float rt[2], int outline)
{
    static GfxDraw d;
    static uint8_t v[4 * 64];
    const CfGlyph* g = &g_cf.kept[i];
    const CfFont* f = &g_cf.font[g->font];
    const CfPic m = cf_pic(&f->glyphs[f->slot[g->code]], outline);
    if (!f->tex || !(m.w > 0.0f && m.h > 0.0f)) /* a space, or no outline */
        return;
    d = g_cf.runs[g->run];
    const uint32_t stride = (uint32_t)d.u.stride[0], pos = (uint32_t)d.u.offset[GFX_R_POSITION];
    const uint32_t tc = (uint32_t)d.u.offset[GFX_R_TEXCOORD0];
    float c[4];
    if (!cf_clip(&d, lay, map, rt, c))
        return;
    /* on whole pixels, so the atlas's texels are the screen's (a label's letter centred in its sprite's width) */
    const VanaChatGlyph* glyph = &f->glyphs[f->slot[g->code]];
    const float pen = cf_screen(&d, 0, x, map, rt);
    const float sx = g->scale[0], sy = g->scale[1]; /* (a squeezed label's letter squeezed as much, across and down) */
    const float centre = g->label_w > 0.0f && !cf_worded(g->font) /* (the labels' laid a word at a time: cf_word) */
        ? (cf_screen(&d, 0, x + g->label_w, map, rt) - pen - glyph->adv * sx) * 0.5f
        : 0.0f;
    const float unshift = cf_per_word(g->font) && (g->label_w > 0.0f || !g->rolled) ? (glyph->x - glyph->lx) * sx : 0.0f; /* (cf_per_word) */
    const float left = floorf(pen + m.x * sx + centre - unshift + 0.5f);
    /* (on the game's baseline: 14 down a cell drawn at its own size, as much less in a smaller one, the timers' 0.85) */
    const float top = floorf(cf_screen(&d, 1, g->py + f->baseline * g->gs[1], map, rt) + 0.5f) + m.y * sy;
    const float u0 = m.ax / (float)f->aw, u1 = (m.ax + m.w) / (float)f->aw;
    const float v0 = m.ay / (float)f->ah, v1 = (m.ay + m.h) / (float)f->ah;
    memcpy(v, g_cf.bytes + i * CF_STRIDE, 4 * stride);
    for (uint32_t k = 0; k < 4; ++k)
    {
        float p[2];
        memcpy(p, v + k * stride + pos, 8);
        /* (the corner it is, the game's order kept: by the sprite's middle, as a label's letter's pen is on its baseline) */
        const int right = p[0] > (g->lo[0] + g->hi[0]) * 0.5f, bottom = p[1] > (g->lo[1] + g->hi[1]) * 0.5f;
        const float q[2] = { right ? left + m.w * sx : left, bottom ? top + m.h * sy : top };
        const float t[2] = { right ? u1 : u0, bottom ? v1 : v0 };
        memcpy(v + k * stride + pos, q, 8);
        memcpy(v + k * stride + tc, t, 8);
    }
    float aff[4];
    cf_affine(&d, map, rt, aff);
    cf_over(&d, lay, c, v);
    if (!outline)
        cf_tint(&d, v, g->color), cf_fade(&d, v, g->fade);
    d.tex[0] = f->tex;
    memset(&d.samp[0], 0, sizeof d.samp[0]);
    d.samp[0].addr_u = d.samp[0].addr_v = d.samp[0].addr_w = 3; /* CLAMP */
    d.samp[0].mag = d.samp[0].min = 2, d.samp[0].max_aniso = 1; /* LINEAR, no mips */
    /* no alpha test: the game's (its font sheet's letters are cut out at ALPHAREF) threw the atlas's soft edge pixels away, so
     * the letters had no smoothing at all, only full ink or none (the chat on a 4K screen, reported 2026-11-15) */
    d.fs.alpha_func = 0;
    if (outline)
    {
        d.fs.text_shadow = 1, d.fs.alpha_func = 0, d.fs.fog = 0, d.fs.specular_add = 0;
        d.pipe.blend = 1, d.pipe.src = 2 /* ONE */, d.pipe.dst = 6 /* INVSRCALPHA */, d.pipe.op = 1;
        d.u.tfactor[3] = f->outline * (float)(255 - g->fade) / 255.0f; /* (a see-through letter's outline as see-through) */
    }
    if (g->fade == 255 || !wc_cut_onto(&d, v, g->clip, aff)) /* (window.corners: inside its window's) */
        return;
    gfx_draw(&d);
}

/* The pen of kept glyph b, a menu label's letter in the buttons' or the titles' font, after a (the letter left of it), in the interface's
 * pixels: a's pen `at` and the font's own advance for a (a letter centred in its sprite, as the windows' are in the game's
 * widths, stood out of its word where the font's is wider than the game's, the M of "Magic" left of the S of "Status" over it,
 * and the narrow ones spaced out), and the font's space more where the game's gap between them is a space's. The game's
 * letters step on by their sprites' width or a texel less, a space 2 to 6 texels more ("Region Info", "Shut Down",
 * "Character Information": ROM\119\51.DAT's labels). A wider gap is another label's: the game's place, or a space after the
 * font's word where that reaches past it. (Labels side by side on a row were one word up to a 12-texel gap: the Delivery
 * window's "OK" and "Exit", 12 apart under their buttons, came out "OK Exit" under the first: reported 2026-11-06.) */
static float cf_word(const CfGlyph* a, const CfGlyph* b, float at, const float map[4], const float rt[2])
{
    const CfFont* f = &g_cf.font[a->font];
    const float s = a->scale[0], gap = b->px - a->px - a->label_w;
    const float per = map[1] * cf_scale(&g_cf.runs[a->run], 0) / rt[0]; /* the screen's pixels per interface pixel */
    if (!(per > 0.0f))
        return b->px;
    /* (the font's advances: the screen's pixels at the picture's own size) */
    const float pen = at + f->glyphs[f->slot[a->code]].adv * s / per;
    const float space = f->slot[0] >= 0 ? f->glyphs[f->slot[0]].adv * s / per : 0.0f;
    if (gap > 7.0f * s)
        return fmaxf(b->px, pen + space);
    return gap > 1.5f * s ? pen + space : pen;
}

/* The big letters laid already this frame (cf_big_words: a row's are laid together, wherever they are in the kept list), and
 * the labels' letters taken into a row of them */
static uint8_t cf_laid[CF_MOST];

#include "d3d8_chatfont_labels.h" /* cf_label_word: a menu label's letters a word at a time by the font's own advances */

#include "d3d8_chatfont_big.h" /* cf_big_words: the big letters' rows a word at a time, a number with its unit */

#include "d3d8_chatfont_words.h" /* cf_row_words: a window's letters a word at a time, the chat line's caret, centred and right-aligned texts */

/* The pens (e) of kept glyph i's row, none a menu label's: where the game's pens put them, less what the game's whole-pixel
 * widths left off the font's advances before each (VanaChatGlyph.frac; the windows' 0: their glyphs sit in the game's widths),
 * the row stretched to end where the game's does; a tooltip part's row in its own font the game's row made wider from its left
 * as much as the font is larger (its letters centred in the game's widths made as much wider: chatfont.cpp); a picture where it
 * is; a window's letters a word at a time (cf_row_words). Gives the glyph after. */
static uint32_t cf_row(uint32_t i, float* e, const float map[4], const float rt[2])
{
    const CfGlyph* k = g_cf.kept;
    uint32_t j = i + 1;
    e[i] = k[i].px;
    for (; !k[i].pic && j < g_cf.nkept && !k[j].pic && !k[j].drawn && k[j].py == k[i].py && (k[j].px > k[j - 1].px || cf_caret(j - 1)) &&
           k[j].font == k[i].font && k[j].rolled == k[i].rolled && !cf_laid[j]; ++j)
    {
        const CfFont* f = &g_cf.font[k[j - 1].font];
        e[j] = e[j - 1] + (k[j].px - k[j - 1].px) + f->glyphs[f->slot[k[j - 1].code]].frac;
    }
    if (!k[i].pic && cf_per_word(k[i].font) && !k[i].rolled) /* (a scrolling line's each where the game's is: CfGlyph.rolled) */
    {
        cf_row_words(i, j, e, map, rt);
        return j;
    }
    const float span = e[j - 1] - k[i].px, game = k[j - 1].px - k[i].px;
    const float stretch = span > 0.0f && game / span > 0.9f && game / span < 1.1f ? game / span : 1.0f;
    const float wider = cf_part_scale(k[i].font); /* (the row's left: its pictures split it, d3d8_tips.h) */
    for (uint32_t n = i; n < j; ++n)
        e[n] = k[i].font >= VANA_FONT_TIPS_TITLE && k[i].font <= VANA_FONT_TIPS_TIMER ? k[n].row_x + (k[n].px - k[n].row_x) * wider : k[i].px + (e[n] - k[i].px) * stretch;
    return j;
}

/* Whether kept letter n is drawn in this pass (outline 1, then the letters 0: cf_draw's comment says why): not the game's own
 * shadow under a label's or a big letter where the font has an outline, and no outline for a letter cut out of its picture */
static int cf_in_pass(uint32_t n, int outline)
{
    const CfGlyph* g = &g_cf.kept[n];
    const int op3 = g_cf.runs[g->run].pipe.op == 3, cut = op3 && g->cut, shadow = op3 && !cut;
    const int outlined = g_cf.font[g->font].outline > 0.0f;
    return !(shadow && outlined) && (!outline || (outlined && !cut));
}

/* The kept glyphs over the laid target: every outline, then every letter (a letter's neighbour's outline never over it). The
 * big letters' rows first, a word at a time, each ending where the game's does (cf_big_words: they take in the labels' letters
 * among them); then a row at a time (cf_row; the windows' a word at a time), a menu label a word at a time by the font's own
 * advances from where the game's starts (cf_label_word) */
static void cf_draw(const GfxDraw* lay, const float map[4], const float rt[2])
{
    static float e[CF_MOST];
    int fonts = 0;
    for (int f = 0; f < VANA_FONTS; ++f)
        fonts |= cf_texture(&g_cf.font[f]);
    if (!fonts)
        return;
    cf_spread();
    const CfGlyph* k = g_cf.kept;
    /* (a letter a later draw covered, drawn into the target as the game drew it, is no letter of a word or a row here: the
     * Change Job list's "Dancer" was laid as one word with the "Main Job" under the list on its row, and its letters stood apart
     * where the hidden ones' would have gone, "D  a  n cer": reported 2026-11-06) */
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
        cf_laid[i] = k[i].drawn || k[i].line; /* (the chat line's: its picture, cf_line_draw) */
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
        if (k[i].centred && !cf_laid[i]) /* (a text the game centres, the status timers, on its middle; one it right-aligns to its end) */
            cf_centred_word(i, e, map, rt);
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
        if (cf_big(k[i].font) && k[i].label_w > 0.0f && !cf_laid[i]) /* (the trade windows' font sheet letters: cf_row's) */
            cf_big_words(i, e, map, rt);
    for (uint32_t i = 0; i < g_cf.nkept;)
        i = cf_laid[i] ? i + 1 : cf_worded(k[i].font) && k[i].label_w > 0.0f ? cf_label_word(i, e, map, rt) : cf_row(i, e, map, rt);
    /* (the game's own shadow under a label's or a big letter, a copy a little down and right taken off what's there: blend op
     * REVSUBTRACT, gets no outline, and none at all where the font has one, which stands in for it. A "menu2fon" letter drawn
     * so is no shadow but the letter, cut out of its picture: the chat line's tab has no other copy of its letters, and with an
     * outlined font it went blank (2026-11-06). It's drawn with no outline, which would cut a wider hole) */
    for (int outline = 1; outline >= 0; --outline)
    {
        cf_line_draw(lay, map, rt, outline);
        for (uint32_t n = 0; n < g_cf.nkept; ++n)
        {
            if (k[n].pic)
            {
                if (!k[n].drawn && !outline) /* (a picture with the letters: the outlines under it too) */
                    cf_picture(n, lay, map, rt);
            }
            else if (!k[n].drawn && !k[n].line && cf_in_pass(n, outline))
                cf_glyph(n, e[n], lay, map, rt, outline);
        }
    }
}

#include "d3d8_chatfont_under.h" /* cf_under: the slot names an item's picture covers, into the target in True Font */

/* A draw just made: the interface's target laid onto the screen has the kept glyphs drawn over it */
static void chatfont_laid(const GfxDraw* lay)
{
    if (!g_cf.target || g_dev.rt != g_dev.backbuffer || g_dev.cur.tex[0] != g_cf.target)
        return;
    const Obj* t = obj(g_cf.target);
    float map[4];
    if (!t || !t->width || !t->height || !cf_map(lay, map))
    {
        static int told;
        g_cf.n.lay_nomap++;
        if (!told++)
            rt_log("[recomp] d3d8: True Font: the interface's target (%ux%u) is laid onto the screen by a draw it can't follow "
                   "(pre-transformed %d, vertices %s, stride %d, viewport %.0fx%.0f); its letters stay the game's\n",
                t ? t->width : 0, t ? t->height : 0, lay->vs.rhw, lay->buf[0] ? "in a buffer" : lay->data[0] ? "the draw's own" : "none",
                lay->u.stride[0], lay->u.vp[2], lay->u.vp[3]);
        return;
    }
    g_cf.n.lay_ok++;
    g_cf.unlaid = 0, g_cf.unlaid_kept = 0;
    if (g_cf.assume)
        rt_log("[recomp] d3d8: True Font: the interface's target is seen laid onto the screen again: its letters follow it\n");
    g_cf.assume = 0;
    const float rt[2] = { (float)(t->pw ? t->pw : t->width), (float)(t->ph ? t->ph : t->height) };
    /* the glyphs' size: the screen's pixels per interface pixel, through the chat's last draw's mapping (g_cf.k) */
    g_cf.sx = map[1] * g_cf.k[0] / rt[0], g_cf.sy = map[3] * g_cf.k[1] / rt[1], g_cf.rest = 0;
    g_cf.tips_seen = 0, g_cf.spread = 0; /* (cf_spread: the tooltips this laying has) */
    if (g_cf.nkept)
        cf_draw(lay, map, rt);
    g_cf.nkept = 0, g_cf.nruns = 0;
}

/* The interface's target (a texture) taken as laid over the whole screen, as the game lays it (stretched, opaque), when its
 * laying isn't seen (CF_ASSUME frames in a row with glyphs kept and none laid: a laying drawn some way True Font doesn't
 * follow, or taken by another of VanaCore's hands first): the kept glyphs drawn onto the back buffer, bound, as if a laying
 * with that mapping had just been made, and the letters' size from it. Off again when a laying is seen (chatfont_laid). */
#define CF_ASSUME 30
static void cf_assume(void)
{
    static GfxDraw lay;
    const Obj* t = obj(g_cf.target);
    const Obj* bb = obj(g_dev.backbuffer);
    if (!t || !t->width || !t->height || !bb)
        return;
    const float rt[2] = { (float)(t->pw ? t->pw : t->width), (float)(t->ph ? t->ph : t->height) };
    const float w = (float)(bb->pw ? bb->pw : bb->width), h = (float)(bb->ph ? bb->ph : bb->height);
    const float map[4] = { 0.0f, w, 0.0f, h };
    memset(&lay, 0, sizeof lay);
    lay.vp[2] = (uint32_t)w, lay.vp[3] = (uint32_t)h;
    lay.u.vp[2] = w, lay.u.vp[3] = h;
    if (!g_cf.assume)
        rt_log("[recomp] d3d8: True Font: the interface's target (%ux%u) wasn't seen laid onto the screen in %d frames with letters "
               "kept: taken as laid over the whole screen (%.0fx%.0f) from now on, its letters drawn at the frame's end\n",
            t->width, t->height, CF_ASSUME, w, h);
    g_cf.assume = 1, g_cf.n.assumed++;
    g_cf.sx = w * g_cf.k[0] / rt[0], g_cf.sy = h * g_cf.k[1] / rt[1], g_cf.rest = 0;
    g_cf.tips_seen = 0, g_cf.spread = 0;
    if (g_cf.nkept)
        cf_draw(&lay, map, rt);
    g_cf.nkept = 0, g_cf.nruns = 0;
}

/* The game's frame drawn (EndScene), or VanaCore's own drawing about to begin, or the frame's end: the kept glyphs that go
 * straight onto the back buffer (the interface's target when the game has none: cf_onto_screen) go there now, under what's
 * drawn next; a target laid by assumption (cf_assume) likewise, and after CF_ASSUME frames of its laying not being seen */
static void chatfont_flush(void)
{
    if (!g_cf.target || !g_cf.nkept || g_own_pixels)
        return;
    const int screen = g_cf.target == g_dev.backbuffer;
    if (!screen && !g_cf.assume && g_cf.unlaid_kept < CF_ASSUME)
        return;
    /* onto the back buffer whatever is bound (the game's last target, an effect's): the game's targets put back after */
    const uint32_t rt = g_dev.rt, ds = g_dev.ds;
    const int rebind = rt != g_dev.backbuffer;
    if (rebind)
        g_dev.rt = g_dev.backbuffer, g_dev.ds = g_dev.depth;
    apply_targets();
    if (screen)
        cf_onto_screen();
    else
        cf_assume();
    g_cf.n.flushed++;
    if (rebind)
        g_dev.rt = rt, g_dev.ds = ds, apply_targets();
}

/* The kept glyphs drawn now onto the back buffer, the interface's target when the game has no menu target (the file's
 * comment), which is bound: each run's own mapping is the screen's, so the laying's is none (a unit map over a target of
 * a pixel, its clip all of the screen) */
static void cf_onto_screen(void)
{
    static GfxDraw lay;
    static const float map[4] = { 0.0f, 1.0f, 0.0f, 1.0f }, rt[2] = { 1.0f, 1.0f };
    memset(&lay, 0, sizeof lay); /* (small_rt and overlay none: the back buffer's) */
    lay.vp[2] = lay.vp[3] = 16384;
    cf_draw(&lay, map, rt);
    g_cf.nkept = 0, g_cf.nruns = 0;
}

/* The frame's end: on the screen (no menu target) the kept glyphs drawn there, at the chat's draws' own scale; kept glyphs
 * never laid onto the screen are lost, and the next frames' are the game's until it lays the target again */
static void chatfont_present(void)
{
    g_chat_drawing = 0, g_cf.button = 0;
    g_cf.chat_ago += g_cf.chat_ago < 1000;
    cf_line_frame();
    if (g_cf.target && g_cf.target != g_dev.backbuffer && ++g_cf.unlaid == 600)
    {
        const Obj* t = obj(g_cf.target);
        rt_log("[recomp] d3d8: True Font: the interface's target (%ux%u) wasn't seen laid onto the screen in 600 frames%s\n",
            t ? t->width : 0, t ? t->height : 0, g_cf.sx > 0.0f ? " (it was before)" : ": True Font waits for it");
    }
    if (g_cf.target && g_cf.target == g_dev.backbuffer)
        g_cf.sx = g_cf.k[0], g_cf.sy = g_cf.k[1], g_cf.rest = 0;
    chatfont_flush();
    if (g_cf.target && (g_cf.target == g_dev.backbuffer || g_cf.assume))
    {
        if (!g_cf.spread)
            g_cf.tips_seen = 0;
        g_cf.spread = 0;
    }
    if (!g_cf.nkept)
        return;
    g_cf.n.lost++, g_cf.unlaid_kept++; /* (a frame whose letters went unlaid: cf_assume after CF_ASSUME of them in a row) */
    if (!g_cf.lost++ || g_cf.debug)
        rt_log("[recomp] d3d8: True Font: the interface's target wasn't laid onto the screen after its text (%u "
               "glyphs); the game's letters until it is\n", g_cf.nkept);
    g_cf.rest = 1, g_cf.nkept = 0, g_cf.nruns = 0;
}

static void chatfont_lost(void)
{
    for (int f = 0; f < VANA_FONTS; ++f)
        gfx_tex_destroy(g_cf.font[f].tex), g_cf.font[f].tex = NULL;
    cf_line_lost();
    g_cf.nkept = 0, g_cf.nruns = 0;
}
#else
static void chatfont_cover(const GfxDraw* d) { (void)d; }
static void chatfont_laid(const GfxDraw* d) { (void)d; }
static void chatfont_present(void) {}
static void chatfont_lost(void) {}
static void chatfont_flush(void) {}
static void tips_gradient(const GfxDraw* d, uint32_t prim, uint32_t n, uint32_t up, uint32_t stride)
{
    (void)d;
    (void)prim;
    (void)n;
    (void)up;
    (void)stride;
}
#endif
