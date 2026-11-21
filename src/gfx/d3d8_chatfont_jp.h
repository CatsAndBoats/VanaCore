/* True Font's menu labels in a game in Japanese. Its menus' labels ("ステータス", "装備変更") are pictures of whole words on the
 * Japanese menu DAT's own sheets ("menu    menufont", "menu    mn6font"...), a label one or more side by side, each drawn
 * twice (a shadow copy taken away, then its letters), not the English labels' letter sprites (cf_label). jplabels.cpp tells
 * which label each quad is as the game draws it (d3d8_label_word): the label's first part is kept as a label's letter whose
 * glyph is the label's words (their code VANA_LABEL_WORDS on, built as they're drawn: chatfont_wide.h), laid in the whole
 * label's width (cf_jp_kept), and its later parts are left out once the first is True Font's; a picture of those sheets that
 * isn't a known label stays the game's, never taken for an English letter where its sprite's place matches one. Included by
 * d3d8_chatfont.h just before cf_what, whose label path it feeds (cf_jp_label) and wraps (cf_jp_what). */
#pragma once

#define CF_JP_GAME 1  /* a picture left the game's */
#define CF_JP_FIRST 2 /* a label's first part: its words */
#define CF_JP_LATER 3 /* a later part of it */

static struct
{
    int what, shadow; /* the quad drawn now (CF_JP_*, 0 none), and its pass: 1 the shadow copy, 0 the letters */
    uint32_t code;    /* the label's words' glyph */
    float wide;       /* the label's width to its first part's */
    int kept[2];      /* each pass's first part is True Font's (its later parts left out) */
    VanaLabelGlyph l; /* the first part as a label's letter (cf_what's label path) */
} cf_jp;

void d3d8_label_word(int what, int shadow, uint32_t code, float wide)
{
    cf_jp.what = what >= CF_JP_GAME && what <= CF_JP_LATER ? what : 0, cf_jp.shadow = shadow ? 1 : 0;
    cf_jp.code = code, cf_jp.wide = wide >= 1.0f && wide <= 64.0f ? wide : 1.0f;
    if (cf_jp.what == CF_JP_FIRST)
        cf_jp.kept[cf_jp.shadow] = 0;
}

/* cf_label, but for a Japanese label sheet's quad: its first part a label's letter of the quad's texels (any sheet size), its
 * baseline and capitals as tall as the English labels' at 12 texels (9: the buttons' font at its size), smaller in proportion */
static const VanaLabelGlyph* cf_jp_label(const GfxDraw* d, float lo[2], float hi[2], int* top, float scale[2])
{
    if (!cf_jp.what)
        return cf_label(d, lo, hi, top, scale);
    const Obj* t = obj(g_dev.cur.tex[0]);
    float tlo[2], thi[2];
    if (cf_jp.what != CF_JP_FIRST || g_cf.game || !t || !t->width || !t->height || t->height > 256 || !cf_quad(d, lo, hi, tlo, thi))
        return NULL;
    const float tw = (thi[0] - tlo[0]) * (float)t->width, th = (thi[1] - tlo[1]) * (float)t->height;
    if (!(tw >= 1.0f && th >= 1.0f))
        return NULL;
    scale[0] = (hi[0] - lo[0]) / tw, scale[1] = (hi[1] - lo[1]) / th;
    *top = (int)floorf(tlo[1] * (float)t->height + 0.5f);
    const int h = (int)(th + 0.5f), cap = (h * 9 + 6) / 12;
    cf_jp.l.code = 0, cf_jp.l.u = 0, cf_jp.l.v = (uint8_t)*top, cf_jp.l.w = (uint8_t)(tw < 255.0f ? tw : 255.0f);
    cf_jp.l.h = (uint8_t)h, cf_jp.l.base = (uint8_t)((h * 10 + 6) / 12), cf_jp.l.cap = (uint8_t)(cap < 1 ? 1 : cap > 9 ? 9 : cap);
    cf_jp.l.sheet = 0; /* (its shadow is a copy of its own, a part apart: not a "menu2fon" letter cut out of its picture) */
    return &cf_jp.l;
}

static int cf_what(const GfxDraw* d, float lo[2], float hi[2], CfWhat* w);

/* cf_what, with a label's later parts left out while its first is True Font's (kept, or left out with its area's letters) */
static int cf_jp_what(const GfxDraw* d, float lo[2], float hi[2], CfWhat* w)
{
    if (cf_jp.what == CF_JP_LATER && cf_jp.kept[cf_jp.shadow])
        return 2;
    const int what = cf_what(d, lo, hi, w);
    if (cf_jp.what == CF_JP_FIRST)
        cf_jp.kept[cf_jp.shadow] = what != 0 && w->l == &cf_jp.l;
    return what;
}

/* Kept glyph g, label letter l: a label's first part laid in the whole label's width, its words squeezed across to fit it as the
 * game squeezes a long label's letters (their advance at the last laying's scale) */
static void cf_jp_kept(CfGlyph* g, const VanaLabelGlyph* l)
{
    if (l != &cf_jp.l)
        return;
    g->label_w *= cf_jp.wide;
    const CfFont* f = &g_cf.font[g->font];
    const int s = g->code < CF_CODES ? f->slot[g->code] : -1;
    const float own = s >= 0 && g_cf.sx > 0.0f ? f->glyphs[s].adv / g_cf.sx * g->scale[0] : 0.0f;
    if (own > g->label_w && g->label_w > 0.0f)
        g->scale[0] *= g->label_w / own;
}
