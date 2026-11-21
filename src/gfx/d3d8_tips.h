/* True Font's tooltips (an item's details, VANA_FONT_TIPS): their rows moved and their parts' own look. d3d8_chatfont.h
 * includes this after CfFont, CfGlyph and g_cf, whose names it uses. It is no file of its own.
 *
 * A tooltip's rows are found from its kept letters (by its window's clip, their runs': CfTip), and each row is given a part
 * (VANA_TIP_*) by its text (cf_tip_parts): the top row the item's name; the next its type when its letters were printed as
 * the type's (d3d8_tip_type: tipicons.cpp, which may write it "Body - All Races") or it starts with '[' or '('
 * ("[Body]All Races"); the job list from the row starting "Lv." ("Lv.57 WAR/MNK/...") and the rows of job names after it;
 * the last row its crafter's signature when it's a name in brackets ("[Voltiz]"); the rest its stats and description. Then (cf_spread) the rows are moved down tips.font.padding.top, spread to
 * tips.font.lineheight, each part in its own font where its size isn't the rows' (VANA_FONT_TIPS_TITLE + the part), the rows
 * after a larger one lower as much as it is, a part's first row lower by the space above it (VanaTipPart.space), its letters
 * in its colour (VanaTipPart.color), its numbers in theirs (VanaTipPart.numbers: cf_number), as see-through as it is
 * (VanaTipPart.fade); the game's pictures in a row (its elements' icons: cf_icon) moved and sized with it. What the rows need
 * beyond their spacing goes to chatfont.cpp's SetLines hook (d3d8_tips_extra), which makes the box taller, and how wide its
 * widest row is laid (d3d8_tips_width), which makes it wider.
 *
 * Its item's rarity (tips.rarity.*, g_tip: told around the details window's draw, whatever draws its letters): the name in its
 * colour (kept letters marked as they're kept: CfGlyph.rarity) and a gradient of it down from the box's top (tips_gradient). */
#pragma once

#define CF_TIPS 8      /* tooltips a frame */
#define CF_TIP_ROWS 32 /* rows a tooltip */

#define CF_TIP_MARGIN 8.0f /* the interface's pixels a widened box keeps right of its widest row (cf_tips_width) */

typedef struct CfTipRow
{
    float py;         /* the game's pen (its cells' top) */
    float x;          /* its leftmost letter's pen across */
    float x1;         /* its rightmost letter's right (its pen and a cell; a space not counted) */
    float to;         /* where its pen goes */
    float end;        /* its rightmost letter's right as the game has it (cf_tip_widths): a right-aligned row's made larger from it */
    float wider;      /* how much wider its capitals laid it than the game's letters (cf_tip_upper) */
    float lead_x[3];  /* its three leftmost letters' pens across ... */
    uint16_t lead[3]; /* ... and their codes (0xFFFF none) */
    float tail_x;     /* its rightmost letter's pen (not a space) ... */
    uint16_t tail;    /* ... and its code (0xFFFF none) */
    uint8_t names;    /* every letter a capital, '/' or a space: a row of job names */
    uint8_t part;     /* VANA_TIP_* */
    uint8_t typed;    /* a letter of it printed as the type row's (CfGlyph.typed) */
} CfTipRow;

typedef struct CfTip
{
    float vp[4];  /* its window's clip */
    float x0, x1; /* its letters' span across */
    int nrows;
    CfTipRow row[CF_TIP_ROWS]; /* top to bottom */
} CfTip;

/* The tooltips laid last (cf_spread, the frame's together): their leftmost letter's pen and top row's, their widest row's
 * right as their parts lay it and as the game has it (cf_tip_place), for the box's width (d3d8_tips_width) */
static struct
{
    float x0, top, laid, game;
} g_tip_laid[CF_TIPS];
static int g_tip_nlaid;

void d3d8_set_tip_parts(const VanaTipPart* parts)
{
    memset(g_cf.parts, 0, sizeof g_cf.parts);
    if (parts)
        memcpy(g_cf.parts, parts, sizeof g_cf.parts);
}

float d3d8_tips_extra(void) { return g_cf.tips_seen ? g_cf.tips_extra : -1.0f; }

static int cf_tips_moved(void);

/* The width the box at left, top, right, bottom needs (vanacore_gfx_tips_width): its tooltip's widest row and CF_TIP_MARGIN
 * from its left, when its parts laid that row further right than the game has it: no more than they made it wider */
float d3d8_tips_width(float left, float top, float right, float bottom)
{
    for (int t = 0; t < g_tip_nlaid && cf_tips_moved(); ++t) /* (none moved: what was laid when they were is stale) */
        if (g_tip_laid[t].x0 >= left && g_tip_laid[t].x0 < right && g_tip_laid[t].top >= top && g_tip_laid[t].top < bottom)
        {
            const float grew = g_tip_laid[t].laid - g_tip_laid[t].game;
            const float need = fminf(g_tip_laid[t].laid + CF_TIP_MARGIN - left, right - left + grew);
            return grew > 0.0f && need > right - left ? need : right - left;
        }
    return 0.0f;
}

/* [client] tips.rarity.* (d3d8_set_tip_rarity), and the item details window the game draws now (d3d8_tip_item: chat.cpp's
 * window draw, its item's colour from itemrarity.cpp) */
static struct
{
    int name;               /* its name's letters in its item's rarity colour (chatfont_keep, cf_tip_letter) */
    float strength, height; /* the gradient's alpha at the box's top (0 none) and how far down it fades out (tips_gradient) */
    int drawing, laid;      /* the window's draw is under way; its gradient is drawn (over its first background) */
    uint32_t rgb;           /* its item's rarity colour, 0xFFRRGGBB; 0 none */
    int type;               /* its type row's text is being printed (d3d8_tip_type) */
    int equipped;           /* its name is being printed in its equipped colour (d3d8_tip_equipped): kept over the rarity's */
} g_tip;

void d3d8_set_tip_rarity(int name, float strength, float height)
{
    g_tip.name = name != 0;
    g_tip.strength = strength > 0.0f ? (strength < 1.0f ? strength : 1.0f) : 0.0f; /* (written so that a NaN is 0) */
    g_tip.height = height >= 4.0f ? (height <= 256.0f ? height : 256.0f) : 4.0f;
}

void d3d8_tip_item(int drawing, int has, uint32_t rgb)
{
    g_tip.drawing = drawing != 0, g_tip.laid = 0;
    g_tip.rgb = drawing && has ? 0xFF000000u | (rgb & 0xFFFFFFu) : 0;
}

void d3d8_tip_type(int drawing) { g_tip.type = drawing != 0; }

void d3d8_tip_equipped(int on) { g_tip.equipped = on != 0; }

/* A letter kept in area `which` now: its item's rarity colour for a tooltip's name (tips.rarity.name), 0 none (an equipped
 * item's name keeps its equipped colour: cf_tip_worn) */
static uint32_t cf_tip_rarity(int which)
{
    return which == VANA_FONT_TIPS && g_tip.drawing && g_tip.name && !g_tip.equipped ? g_tip.rgb : 0;
}

/* ... an equipped item's name in its equipped colour (d3d8_tip_equipped): the game's colour kept (cf_tip_letter) */
static uint8_t cf_tip_worn(int which) { return (uint8_t)(which == VANA_FONT_TIPS && g_tip.equipped); }

/* ... a tooltip's type row's (d3d8_tip_type) */
static uint8_t cf_tip_typed(int which) { return (uint8_t)(which == VANA_FONT_TIPS && g_tip.type); }

/* tips_gradient's two bands (v: its strip of six) kept inside the background's corners ([client] window.corners: d3d8_corners.h
 * wc_band), drawn as g */
static void tips_gradient_cut(GfxDraw* g, const uint8_t* v)
{
    static uint8_t cut[2 * 3 * (WC_MOST + 8) * 28];
    uint32_t k = 0;
    for (int b = 0; b < 2; ++b)
    {
        float x[2], y[2];
        uint32_t c[2];
        memcpy(&x[0], v, 4), memcpy(&x[1], v + 28, 4);
        memcpy(&y[0], v + 28 * (2 * b) + 4, 4), memcpy(&y[1], v + 28 * (2 * b + 2) + 4, 4);
        memcpy(&c[0], v + 28 * (2 * b) + 16, 4), memcpy(&c[1], v + 28 * (2 * b + 2) + 16, 4);
        k += wc_band(cut + 28 * k, (uint32_t)(uintptr_t)v, x, y, c);
    }
    g->prim = GFX_TRIANGLELIST, g->count = k / 3;
    if (k && set_streams(g, 0, k, (uint32_t)(uintptr_t)cut, 28))
        gfx_draw(g);
}

/* The details window's background (the window texture's quad, 64 x 24 or more, as window_alpha finds one) just drawn as `d`
 * from vertices `up` (a strip of 4 at stride 28: XYZRHW, diffuse, one texture): its item's rarity colour laid over its top,
 * tips.rarity.gradient.strength at the top, fading out (eased: (1 - t)^2 at 40%) tips.rarity.gradient.height lower. The first
 * background in the window only; its frame, its icon and its text are drawn after, over it. */
static void tips_gradient(const GfxDraw* d, uint32_t prim, uint32_t n, uint32_t up, uint32_t stride)
{
    static GfxDraw g;
    static uint8_t v[6 * 28];
    const float at[3] = { 0.0f, 0.4f, 1.0f }, fade[3] = { 1.0f, 0.36f, 0.0f };
    if (g_wc.kind == 1) /* the background went in its corners' shape (d3d8_corners.h): the quad it came as */
        prim = 5, n = 4, up = g_wc.quad;
    if (!g_tip.drawing || g_tip.laid || !g_tip.rgb || !(g_tip.strength > 0.0f) || !up || prim != 5 || n != 4 || stride != 28 ||
        g_dev.cur.vs != 0x144)
        return;
    const Obj* t = obj(g_dev.cur.tex[0]);
    if (!t || t->width != 128 || t->height != 128)
        return;
    float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
    for (uint32_t i = 0; i < 4; ++i)
        for (int c = 0; c < 2; ++c)
        {
            const float p = u2f(rd32(up + 28 * i + 4 * c));
            lo[c] = fminf(lo[c], p), hi[c] = fmaxf(hi[c], p);
        }
    if (!(hi[0] - lo[0] >= 64.0f && hi[1] - lo[1] >= 24.0f))
        return;
    g_tip.laid = 1;
    const float h = fminf(g_tip.height, hi[1] - lo[1]);
    for (int i = 0; i < 6; ++i) /* (the strip's order: top left, top right, then down a row) */
    {
        const float x = (i & 1) ? hi[0] : lo[0], y = lo[1] + h * at[i / 2];
        const uint32_t a = (uint32_t)(255.0f * g_tip.strength * fade[i / 2] + 0.5f), colour = (g_tip.rgb & 0xFFFFFFu) | a << 24;
        memcpy(v + 28 * i, GUEST_PTR(up), 28); /* (the first vertex's depth, rhw and texture coordinates) */
        memcpy(v + 28 * i, &x, 4), memcpy(v + 28 * i + 4, &y, 4), memcpy(v + 28 * i + 16, &colour, 4);
    }
    g = *d;
    /* the vertices' colour and alpha alone, no texture (as window_look's solid border), blended over the background */
    g.fs.st[0].cop = 2, g.fs.st[0].ca1 = 0, g.fs.st[0].aop = 2, g.fs.st[0].aa1 = 0;
    g.pipe.blend = 1, g.pipe.src = 5, g.pipe.dst = 6, g.pipe.op = 1;
    g.prim = 5, g.count = 4, g.text_shadow = 0;
    if (g_wc.kind == 1)
        tips_gradient_cut(&g, v);
    else if (set_streams(&g, 0, 6, (uint32_t)(uintptr_t)v, 28))
        gfx_draw(&g);
}

/* The tooltips' rows are moved or look other than the game's (cf_spread): spread (tips.font.lineheight), moved down
 * (tips.font.padding.top), a part in its own size, colour or space, or the name in its rarity's colour (tips.rarity.name) */
static int cf_tips_moved(void)
{
    const CfFont* f = &g_cf.font[VANA_FONT_TIPS];
    int parts = g_tip.name;
    for (int p = 0; p < VANA_TIP_PARTS; ++p)
        parts |= g_cf.font[VANA_FONT_TIPS_TITLE + p].on || g_cf.parts[p].color || g_cf.parts[p].numbers || g_cf.parts[p].space != 0.0f ||
            g_cf.parts[p].fade > 0.0f || g_cf.parts[p].upper;
    return f->on && (f->line != 1.0f || f->pad != 0.0f || parts);
}

/* Part p's font: its own (VANA_FONT_TIPS_TITLE + p) when that's on, else the rows' */
static const CfFont* cf_part_font(int p)
{
    const CfFont* f = &g_cf.font[VANA_FONT_TIPS_TITLE + p];
    return f->on ? f : &g_cf.font[VANA_FONT_TIPS];
}

/* How much wider than the game's a row in font `font` is laid: a tooltip part's own font's size over the rows' (its letters
 * centred in the game's widths made as much wider: chatfont.cpp), else 1 */
static float cf_part_scale(int font)
{
    const CfFont* f = &g_cf.font[VANA_FONT_TIPS];
    const int part = font >= VANA_FONT_TIPS_TITLE && font < VANA_FONT_TIPS_TITLE + VANA_TIP_PARTS && g_cf.font[font].on;
    return part && f->size > 0.0f ? g_cf.font[font].size / f->size : 1.0f;
}

/* How far part p's rows stand lower than the game's (their baselines): as much as its capitals are taller than the rows'
 * font's, so their tops stay where the game's are */
static float cf_part_drop(int p)
{
    const CfFont *t = cf_part_font(p), *f = &g_cf.font[VANA_FONT_TIPS];
    const int h = 'H' - 0x20;
    if (t == f)
        return 0.0f;
    if (t->slot[h] < 0 || f->slot[h] < 0 || !(g_cf.sy > 0.0f))
        return (t->size - f->size) * 0.7f;
    return (f->glyphs[f->slot[h]].y - t->glyphs[t->slot[h]].y) / g_cf.sy; /* (y: the picture's top above the baseline) */
}

/* Draw d's letter, its vertices v, in colour `color` (0xFFRRGGBB; 0 leaves the game's): the game's colour is the vertices'
 * diffuse, times the texture by the first stage (MODULATE, 2X: 0x80 is full, 4X), so it's the colour over that; its alpha
 * kept. Another stage or no diffuse: the game's colour. */
static void cf_tint(const GfxDraw* d, uint8_t* v, uint32_t color)
{
    const uint32_t stride = (uint32_t)d->u.stride[0], at = (uint32_t)d->u.offset[GFX_R_DIFFUSE];
    const int op = d->fs.st[0].cop, times = op == 4 ? 1 : op == 5 ? 2 : op == 6 ? 4 : 0;
    if (!color || !times || d->fs.prog || !d->vs.el[GFX_R_DIFFUSE].used || d->vs.el[GFX_R_DIFFUSE].stream || at + 4 > stride)
        return;
    uint32_t rgb = 0;
    for (int shift = 0; shift < 24; shift += 8)
        rgb |= ((((color >> shift) & 0xFFu) + (uint32_t)times / 2) / (uint32_t)times) << shift;
    for (uint32_t k = 0; k < 4; ++k)
    {
        uint32_t c;
        memcpy(&c, v + k * stride + at, 4);
        c = (c & 0xFF000000u) | rgb;
        memcpy(v + k * stride + at, &c, 4);
    }
}

/* How stage st takes its colour (`alpha` 0) or its alpha (1) from the diffuse: 1 by SELECTARG1/2 or MODULATE (2X, 4X) of
 * D3DTA_DIFFUSE (the texture's times it, as the game's letters are); 2 the texture's alone (SELECTARG1 of D3DTA_TEXTURE), now
 * made to take it times the diffuse's; 0 neither */
static int cf_by_diffuse(GfxStage* st, int alpha)
{
    uint8_t* op = alpha ? &st->aop : &st->cop;
    const uint8_t a1 = (alpha ? st->aa1 : st->ca1) & 0x3F;
    uint8_t* a2 = alpha ? &st->aa2 : &st->ca2;
    if (*op == 2 && a1 == 2)
    {
        *op = 4, *a2 = 0; /* (MODULATE the texture's by the diffuse's) */
        return 2;
    }
    const int d1 = a1 == 0, d2 = (*a2 & 0x3F) == 0; /* (D3DTA_DIFFUSE) */
    return (*op == 2 && d1) || (*op == 3 && d2) || (*op >= 4 && *op <= 6 && (d1 || d2));
}

/* Draw d's letter, its vertices v, `fade` more see-through (0 none, 255 gone: CfGlyph.fade), by what the first stage takes from
 * the diffuse (cf_by_diffuse) times what's left: its alpha where the letter is blended by it (SRCALPHA); its colour where the
 * letter is added as it is (ONE), as the game's are (ONE/ONE: "blend 1 2/2" in the log), whose alpha shows nothing (only the
 * outline faded, 5 November); both where it's premultiplied (ONE/INVSRCALPHA). A pixel shader's or no diffuse: as it is. */
static void cf_fade(GfxDraw* d, uint8_t* v, uint8_t fade)
{
    const uint32_t stride = (uint32_t)d->u.stride[0], at = (uint32_t)d->u.offset[GFX_R_DIFFUSE], keep = 255u - fade;
    if (!fade || d->fs.prog || !d->vs.el[GFX_R_DIFFUSE].used || d->vs.el[GFX_R_DIFFUSE].stream || at + 4 > stride)
        return;
    const int added = d->pipe.blend && d->pipe.src == 2; /* (ONE) */
    const int rgb = added ? cf_by_diffuse(&d->fs.st[0], 0) : 0;
    const int alpha = !added || d->pipe.dst == 6 /* (INVSRCALPHA) */ ? cf_by_diffuse(&d->fs.st[0], 1) : 0;
    if (!rgb && !alpha)
        return;
    for (uint32_t k = 0; k < 4; ++k)
    {
        uint32_t c, out = 0;
        memcpy(&c, v + k * stride + at, 4);
        for (int shift = 0; shift < 32; shift += 8)
        {
            const int how = shift == 24 ? alpha : rgb;
            const uint32_t was = c >> shift & 0xFFu;
            out |= (how == 2 ? keep : how ? (was * keep + 127u) / 255u : was) << shift;
        }
        memcpy(v + k * stride + at, &out, 4);
    }
}

/* The tooltip kept glyph g is in (its run's clip), among n: a new one when `add` and there's room. -1 for none. */
static int cf_tip(CfTip* tip, int* n, const CfGlyph* g, int add)
{
    const float* vp = g_cf.runs[g->run].u.vp;
    for (int t = 0; t < *n; ++t)
        if (!memcmp(tip[t].vp, vp, sizeof tip[t].vp))
            return t;
    if (!add || *n == CF_TIPS)
        return -1;
    memcpy(tip[*n].vp, vp, sizeof tip[*n].vp);
    tip[*n].x0 = 1e30f, tip[*n].x1 = -1e30f, tip[*n].nrows = 0;
    return (*n)++;
}

/* Tooltip t's row whose pen is at py: a new one in its place, top to bottom, when `add` and there's room. -1 for none. */
static int cf_tip_row(CfTip* t, float py, int add)
{
    int r = 0;
    while (r < t->nrows && t->row[r].py < py)
        ++r;
    if (r < t->nrows && t->row[r].py == py)
        return r;
    if (!add || t->nrows == CF_TIP_ROWS)
        return -1;
    memmove(&t->row[r + 1], &t->row[r], (size_t)(t->nrows - r) * sizeof t->row[0]);
    CfTipRow* w = &t->row[r];
    memset(w, 0, sizeof *w);
    w->py = py, w->x = 1e30f, w->x1 = -1e30f, w->names = 1;
    w->tail = 0xFFFF, w->tail_x = -1e30f;
    for (int i = 0; i < 3; ++i)
        w->lead[i] = 0xFFFF, w->lead_x[i] = 1e30f;
    ++t->nrows;
    return r;
}

/* The character of code `code` (ASCII's: the byte less 0x20), 0 for another */
static int cf_char(uint16_t code) { return code < 0x5F ? code + 0x20 : 0; }

/* Kept letter g into its row w: its leftmost three letters and its rightmost (a space not counted, as the game pads a row
 * out with them), whether every one is a job name's */
static void cf_tip_note(CfTipRow* w, const CfGlyph* g)
{
    const int c = cf_char(g->code);
    w->x = fminf(w->x, g->px), w->typed |= g->typed;
    if (c != ' ' && g->px > w->tail_x)
        w->tail = g->code, w->tail_x = g->px, w->x1 = g->px + 16.0f;
    w->names &= (uint8_t)((c >= 'A' && c <= 'Z') || c == '/' || c == ' ');
    for (int i = 0; i < 3; ++i)
        if (g->px < w->lead_x[i])
        {
            for (int j = 2; j > i; --j)
                w->lead[j] = w->lead[j - 1], w->lead_x[j] = w->lead_x[j - 1];
            w->lead[i] = g->code, w->lead_x[i] = g->px;
            return;
        }
}

/* Row w starts a job list: "Lv." or "Lv" and a digit */
static int cf_tip_level(const CfTipRow* w)
{
    const int c = cf_char(w->lead[2]);
    return cf_char(w->lead[0]) == 'L' && cf_char(w->lead[1]) == 'v' && (c == '.' || (c >= '0' && c <= '9'));
}

/* Each of tooltip t's rows' part (VANA_TIP_*), by its text: the top row the title, the next the type when it was printed as
 * the type's (CfTipRow.typed) or starts with '[' or '(', the job list from a row starting "Lv." while the rows after it are job names, a row past the first in angle
 * brackets a timer, the last row (past the second) the signature when it starts with '[' and ends with ']', the rest the
 * stats */
static void cf_tip_parts(CfTip* t)
{
    int r = 1, jobs = 0;
    if (t->nrows > 0)
        t->row[0].part = VANA_TIP_TITLE;
    if (t->nrows > 1 && (t->row[1].typed || cf_char(t->row[1].lead[0]) == '[' || cf_char(t->row[1].lead[0]) == '('))
        t->row[1].part = VANA_TIP_TYPE, r = 2;
    for (; r < t->nrows; ++r)
    {
        jobs = cf_tip_level(&t->row[r]) || (jobs && t->row[r].names);
        t->row[r].part = (uint8_t)(jobs ? VANA_TIP_JOBS : VANA_TIP_STATS);
    }
    for (r = 1; r < t->nrows; ++r) /* a usable item's charges and timers: "<1/1 0:30/[20:00:00, 0:30]>" */
        if (cf_char(t->row[r].lead[0]) == '<' && cf_char(t->row[r].tail) == '>')
            t->row[r].part = VANA_TIP_TIMER;
    CfTipRow* last = t->nrows > 2 ? &t->row[t->nrows - 1] : NULL; /* a crafter's signature: "[Voltiz]", the box's last row */
    if (last && last->part != VANA_TIP_TIMER && cf_char(last->lead[0]) == '[' && cf_char(last->tail) == ']')
        last->part = VANA_TIP_SIGNATURE;
}

/* Part p's rows are right-aligned by the game (a signature, a timer): made larger from their right */
static int cf_tip_right(int p) { return p == VANA_TIP_SIGNATURE || p == VANA_TIP_TIMER; }

/* The interface's pixels letter `code` takes across in font `font` (its advance), 8 (half a cell) when it has no such letter */
static float cf_tip_advance(int font, uint16_t code)
{
    const CfFont* f = &g_cf.font[font];
    const int s = code < CF_CODES ? f->slot[code] : -1;
    return s >= 0 && g_cf.sx > 0.0f ? f->glyphs[s].adv / g_cf.sx : 8.0f;
}

/* Tooltip t's rows' rights (placed: their parts known): each row's as the game has it (CfTipRow.end), its widest as the game
 * has it and as its parts lay it (a part in a larger font made wider from its row's left, a signature's or a timer's from its right: where
 * it stays) into g_tip_laid, for the box's width (d3d8_tips_width) */
static void cf_tip_widths(CfTip* t)
{
    float laid = -1e30f, game = -1e30f;
    for (int r = 0; r < t->nrows; ++r)
    {
        CfTipRow* w = &t->row[r];
        const int font = VANA_FONT_TIPS_TITLE + w->part;
        if (w->tail == 0xFFFF)
            continue;
        w->end = w->tail_x + cf_tip_advance(VANA_FONT_TIPS, w->tail);
        const float own = g_cf.font[font].on && g_cf.font[font].slot[w->tail] >= 0 ? cf_tip_advance(font, w->tail) : w->end - w->tail_x;
        const float right = cf_tip_right(w->part) ? w->end : w->x + (w->tail_x - w->x) * cf_part_scale(font) + own;
        laid = fmaxf(laid, right), game = fmaxf(game, w->end - (cf_tip_right(w->part) ? 0.0f : w->wider));
    }
    if (t->nrows > 0 && game > -1e30f && g_tip_nlaid < CF_TIPS)
    {
        g_tip_laid[g_tip_nlaid].x0 = t->x0, g_tip_laid[g_tip_nlaid].top = t->row[0].py;
        g_tip_laid[g_tip_nlaid].laid = laid, g_tip_laid[g_tip_nlaid].game = game;
        ++g_tip_nlaid;
    }
}

/* Where each of tooltip t's rows goes (CfTipRow.to): from the top row's pen down tips.font.padding.top, each row below the one
 * above as far as the game has it times the font's line, as much more as the row above is larger than the rows' font and the
 * space above a part where it starts; its pen lower as much as its part's capitals are taller (cf_part_drop). What that needs
 * beyond the rows' spacing into g_cf.tips_extra, the most of the tooltips'. */
static void cf_tip_place(CfTip* t)
{
    const CfFont* f = &g_cf.font[VANA_FONT_TIPS];
    float y = t->nrows ? t->row[0].py + f->pad : 0.0f, extra = 0.0f;
    for (int r = 0; r < t->nrows; ++r)
    {
        CfTipRow* w = &t->row[r];
        const float space = r > 0 && w->part != t->row[r - 1].part ? g_cf.parts[w->part].space : 0.0f;
        const float grow = cf_part_font(w->part)->size - f->size;
        y += space;
        w->to = y + cf_part_drop(w->part);
        y += (r + 1 < t->nrows ? (t->row[r + 1].py - w->py) * f->line : 0.0f) + grow;
        extra += space + grow;
    }
    g_cf.tips_extra = fmaxf(g_cf.tips_extra, extra);
}

/* Kept picture p (cf_icon) moved and sized with the row it stands in (tooltip t's, placed: cf_tip_place), the row whose cells
 * hold its middle: its middle as much lower as the row's letters' middles (half its part's drop below its pen's move: their
 * tops stay), and as much further from the row's left and as much larger as its letters are wider. One out of the rows'
 * (above the first, past the side of them) stays, and one more than a cell past its own row's letters (the tooltip's widest
 * row can reach the box's side, a long "<1/1 0:30/[24:00:00, 0:30]>"). One past the name's letters stays too: the Rare and
 * Ex badges, which a long name in a larger font pushed out of the box, are placed where the game draws them (tipicons.cpp:
 * tips.badges.*). */
static void cf_icon_row(CfGlyph* p, const CfTip* t)
{
    const float mx = (p->lo[0] + p->hi[0]) * 0.5f, my = (p->lo[1] + p->hi[1]) * 0.5f;
    float best = 8.0f;
    int at = -1;
    if (mx < t->x0 || mx > t->x1)
        return;
    for (int r = 0; r < t->nrows; ++r)
    {
        const float off = fabsf(my - (t->row[r].py + 7.5f)); /* (the cells' middle: the pen half a pixel in from their top) */
        if (off < best)
            best = off, at = r;
    }
    const CfTipRow* w = at >= 0 ? &t->row[at] : NULL;
    if (!w || mx > w->x1 + (w->part == VANA_TIP_TITLE ? 0.0f : 16.0f))
        return;
    p->dy = w->to - w->py - cf_part_drop(w->part) * 0.5f;
    p->row_x = w->x, p->scale[0] = p->scale[1] = cf_part_scale(VANA_FONT_TIPS_TITLE + w->part);
}

/* Kept letter g, in row w of its tooltip, as its part has it: in the part's own font when it has one with the letter, at the
 * row's place (a signature or a timer made larger from its right, where the game right-aligns it), in the part's colour, or its numbers'
 * when it's a number's (`number`: cf_number) and the part has one (the name's in its item's rarity colour when it has one:
 * tips.rarity.name), as see-through as the part is */
static void cf_tip_letter(CfGlyph* g, const CfTipRow* w, int number)
{
    const CfFont* own = &g_cf.font[VANA_FONT_TIPS_TITLE + w->part];
    const VanaTipPart* part = &g_cf.parts[w->part];
    if (own->on && own->slot[g->code] >= 0)
        g->font = (uint8_t)(VANA_FONT_TIPS_TITLE + w->part);
    cf_want_glyph(VANA_FONT_TIPS_TITLE + w->part, g->code, !g->pic); /* (a letter past ASCII the part's font hasn't yet) */
    g->py = w->to, g->row_x = cf_tip_right(w->part) && w->tail != 0xFFFF ? w->end : w->x;
    g->color = w->part == VANA_TIP_TITLE && g->worn     ? 0 /* (the game's: its equipped colour) */
             : w->part == VANA_TIP_TITLE && g->rarity ? g->rarity
             : number && part->numbers                ? part->numbers
                                                      : part->color;
    g->fade = (uint8_t)(255.0f * fminf(fmaxf(part->fade, 0.0f), 1.0f) + 0.5f);
}

static int cf_digit(int c) { return c >= '0' && c <= '9'; }

/* The letter after kept letter k[i] on its row is a digit: k[i + 1] (the game prints a row left to right) at the same pen
 * down, in the same font, no more than a cell on */
static int cf_digit_next(const CfGlyph* k, uint32_t i)
{
    const CfGlyph* n = i + 1 < g_cf.nkept ? &k[i + 1] : NULL;
    return n && !n->pic && n->font == k[i].font && n->py == k[i].py && n->px > k[i].px && n->px - k[i].px <= 16.0f &&
           cf_digit(cf_char(n->code));
}

/* Kept letter k[i] is a number's (a part's numbers' colour, VanaTipPart.numbers): a digit, a + or - just before one, a % just
 * after one, a . or , between two. `after`: the letter before it on its row was a digit. */
static int cf_number(const CfGlyph* k, uint32_t i, int after)
{
    const int c = cf_char(k[i].code);
    if (cf_digit(c))
        return 1;
    if (c == '+' || c == '-')
        return cf_digit_next(k, i);
    if (c == '%')
        return after;
    return (c == '.' || c == ',') && after && cf_digit_next(k, i);
}

/* A tooltip's letter (VANA_FONT_TIPS, not a picture): its tooltip in tip (n of them; a new one when `add`), -1 none */
static int cf_tip_of(CfTip* tip, int* n, const CfGlyph* g, int add)
{
    return g->font == VANA_FONT_TIPS && !g->pic ? cf_tip(tip, n, g, add) : -1;
}

/* Each kept tooltip letter (tooltips tip, n of them, placed) as its row's part has it (cf_tip_letter), knowing which are a
 * number's */
static void cf_tip_letters(CfTip* tip, int* n)
{
    CfGlyph* k = g_cf.kept;
    int prev_t = -1, prev_r = -1, digit = 0; /* (the row of the letter before and whether it was a digit) */
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
    {
        const int t = cf_tip_of(tip, n, &k[i], 0), r = t >= 0 ? cf_tip_row(&tip[t], k[i].py, 0) : -1;
        if (r < 0)
        {
            prev_t = -1;
            continue;
        }
        const int number = cf_number(k, i, t == prev_t && r == prev_r && digit);
        prev_t = t, prev_r = r, digit = cf_digit(cf_char(k[i].code));
        cf_tip_letter(&k[i], &tip[t].row[r], number);
    }
}

/* The game's width for letter `code` in the rows' font (its letters centred in it: chatfont.cpp's build, x - lx the shift),
 * the interface's pixels; 0 when it has no such letter */
static float cf_tip_game_width(uint16_t code)
{
    const CfFont* f = &g_cf.font[VANA_FONT_TIPS];
    const int s = code < CF_CODES ? f->slot[code] : -1;
    if (s < 0 || !(g_cf.sx > 0.0f))
        return 0.0f;
    const VanaChatGlyph* g = &f->glyphs[s];
    return (2.0f * (g->x - g->lx) + g->adv) / g_cf.sx;
}

/* A lower-case letter's capital's code, else code */
static uint16_t cf_capital(uint16_t code)
{
    const int c = cf_char(code);
    return c >= 'a' && c <= 'z' ? (uint16_t)(code - 0x20) : code;
}

/* The letters of tooltips tip (n of them, their parts known) whose part is in capitals (VanaTipPart.upper): each lower-case one
 * made its capital, and each after it on its row as much further on as the capitals before it are wider in the game's widths, as
 * if the game had printed them (a signature's or a timer's row, right-aligned by the game, as much further left: it ends where
 * it did). The row's letters' span moved with them, how much wider into CfTipRow.wider. The game prints a row left to right. */
static void cf_tip_upper(CfTip* tip, int* n)
{
    CfGlyph* k = g_cf.kept;
    int any = 0;
    for (int p = 0; p < VANA_TIP_PARTS; ++p)
        any |= g_cf.parts[p].upper != 0;
    if (!any)
        return;
    for (int pass = 0; pass < 2; ++pass) /* (the rows' totals, then the letters moved) */
    {
        float grown[CF_TIPS][CF_TIP_ROWS] = { { 0 } };
        for (uint32_t i = 0; i < g_cf.nkept; ++i)
        {
            const int t = cf_tip_of(tip, n, &k[i], 0), r = t >= 0 ? cf_tip_row(&tip[t], k[i].py, 0) : -1;
            CfTipRow* w = r >= 0 ? &tip[t].row[r] : NULL;
            if (!w || !g_cf.parts[w->part].upper)
                continue;
            const float was_w = cf_tip_game_width(k[i].code), cap_w = cf_tip_game_width(cf_capital(k[i].code));
            const uint16_t to = was_w > 0.0f && cap_w > 0.0f ? cf_capital(k[i].code) : k[i].code; /* (both in the font) */
            const float more = to != k[i].code ? cap_w - was_w : 0.0f;
            if (pass == 0)
            {
                w->wider += more;
                continue;
            }
            const float was = k[i].px, now = was + grown[t][r] - (cf_tip_right(w->part) ? w->wider : 0.0f);
            grown[t][r] += more;
            k[i].px = now, k[i].code = to;
            if (was == w->x)
                w->x = now;
            if (was == w->tail_x)
                w->tail_x = now, w->tail = to, w->x1 = now + 16.0f;
            tip[t].x0 = fminf(tip[t].x0, now), tip[t].x1 = fmaxf(tip[t].x1, now + 16.0f);
        }
    }
}

/* The tooltips' rows (an item's details, VANA_FONT_TIPS), each tooltip's own (by its window's clip): found and given their
 * parts, placed, their letters and pictures moved to them (the file's comment) */
static void cf_spread(void)
{
    static CfTip tip[CF_TIPS];
    int n = 0;
    CfGlyph* k = g_cf.kept;
    if (!cf_tips_moved())
        return;
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
    {
        const int t = cf_tip_of(tip, &n, &k[i], 1), r = t >= 0 ? cf_tip_row(&tip[t], k[i].py, 1) : -1;
        if (r >= 0)
            cf_tip_note(&tip[t].row[r], &k[i]), tip[t].x0 = fminf(tip[t].x0, k[i].px), tip[t].x1 = fmaxf(tip[t].x1, k[i].px + 16.0f);
    }
    if (!g_cf.spread) /* (on the screen a frame's glyphs can go in more than one go: the frame's tooltips add up) */
        g_cf.tips_extra = 0.0f, g_cf.tips_seen = 0, g_tip_nlaid = 0;
    g_cf.spread = 1, g_cf.tips_seen |= n > 0;
    for (int t = 0; t < n; ++t)
        cf_tip_parts(&tip[t]);
    cf_tip_upper(tip, &n);
    for (int t = 0; t < n; ++t)
        cf_tip_place(&tip[t]), cf_tip_widths(&tip[t]);
    for (uint32_t i = 0; i < g_cf.nkept; ++i)
    {
        const int t = k[i].pic ? cf_tip(tip, &n, &k[i], 0) : -1;
        if (t >= 0)
            cf_icon_row(&k[i], &tip[t]);
    }
    cf_tip_letters(tip, &n);
}
