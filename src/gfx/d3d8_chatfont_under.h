/* True Font's letters drawn into the interface's target under a later draw, not over its laying: the Equipment window's
 * slot names under an item's picture, and the plain font sheet text of the menus', the item lists' and Menu Lists' windows under
 * the mouse cursor, a menu opened over a list (Use / Drop over the Items list) or a highlight's frame. Included by
 * d3d8_chatfont.h after cf_draw (cf_glyph, cf_label_word, cf_row, cf_in_pass).
 *
 * A letter a later draw covered used to go back to the game, as it drew it (cf_game): under the cursor a name's letters were the
 * game's font among True Font's, and a long name's last letter ("Hydro Baghnakhs" with the cursor on its row: "Hydro Baghnakh  s")
 * stood at the game's pen, which is further right than True Font's words end (reported 2026-11-13). The covered text's word is
 * laid in True Font like the rest, under the draw that covers it. */
#pragma once

/* The Equipment window's slot names a draw about to be made covers (an item's icon over its square: the game draws its squares'
 * names, the "menu    menu2fon" sheet's letters at about a third opaque, then each icon over one, the name showing faintly round
 * it; read live 2026-11-06), taken by cf_covers (cf_under_add: CF_UNDER of them), then drawn into the interface's target at once
 * (cf_under), in True Font, under that draw as the game's would be: their row laid as at the laying (cf_label_word; a font
 * sheet's row cf_row), each covered letter's word with it (its letters CF_UNDER_GAP or less
 * apart: the names stand a square apart), in a space of the screen's pixels at the last laying's scale (g_cf.sx: the fonts'
 * pictures are the screen's pixels) brought down to the target's own (g_cf.into, cf_over). The names under every item were the
 * game's, as other letters a later draw covers still are (cf_game; reported 2026-11-06) */
#define CF_UNDER 1024 /* (a list's rows under a menu opened over it: about 17 rows of 15 letters) */
#define CF_UNDER_GAP 20.0f
static uint16_t cf_under_list[CF_UNDER];
static uint32_t cf_nunder;
static uint8_t cf_under_mark[CF_MOST]; /* (1 taken by cf_under_add, 2 a word's letter drawn with one) */

/* Whether kept glyph g is one cf_under can lay under a draw: the Equipment window's slot names, or a plain font sheet letter (no
 * label's sprite, picture, centred, scrolling or chat line letter: each has its own laying) of the menus', the item lists', Menu
 * Lists', the help's, the Target window's or a cf_whole area's (the trade windows', Current Gil's, the Profile panel's, the
 * Equipment stats') text, which cf_row lays a word at a time from the game's pens (cf_per_word). A letter of the others went back
 * to the game's font whenever the mouse pointer or a highlight was drawn over it (asked about 2026-11-14: "all the old examples") */
static int cf_under_font(const CfGlyph* g)
{
    if (g->pic)
        return 0;
    if (g->font == VANA_FONT_EQUIP)
        return 1;
    const int plain = g->font == VANA_FONT_MENUS || g->font == VANA_FONT_ITEMS || g->font == VANA_FONT_LISTS ||
                      g->font == VANA_FONT_HELP || g->font == VANA_FONT_TARGET || cf_whole(g->font);
    return plain && !(g->label_w > 0.0f) && !g->centred && !g->rolled && !g->line && !g->cut;
}

static int cf_under_add(uint32_t i)
{
    const CfGlyph* g = &g_cf.kept[i];
    if (cf_under_mark[i]) /* (taken already, from another of the grid's cells) */
        return 1;
    if (!cf_under_font(g) || !(g_cf.sx > 0.0f) || !(g_cf.sy > 0.0f) || cf_nunder >= CF_UNDER || !cf_texture(&g_cf.font[g->font]))
        return 0;
    cf_under_mark[i] = 1;
    cf_under_list[cf_nunder++] = (uint16_t)i;
    return 1;
}

/* Kept letters a and b (b straight after a) on one row as cf_row lays them */
static int cf_under_joins(uint32_t a, uint32_t b)
{
    const CfGlyph* k = g_cf.kept;
    return !k[a].pic && !k[b].pic && !k[a].drawn && !k[b].drawn && k[b].py == k[a].py && k[b].px > k[a].px &&
           k[b].font == k[a].font && k[b].rolled == k[a].rolled;
}

/* Kept letters a and b (b straight after a) of one word just laid: its font's, on its row, a letter's gap apart */
static int cf_under_mate(uint32_t a, uint32_t b)
{
    const CfGlyph* k = g_cf.kept;
    return cf_laid[a] && cf_laid[b] && !k[a].drawn && !k[b].drawn && k[a].font == k[b].font && cf_same_row(&k[a], &k[b]) &&
           fabsf(k[b].px - k[a].px) <= CF_UNDER_GAP;
}

/* Covered letter i's row laid (e, map, rt as cf_draw's: the slot names are the "menu2fon" sheet's letters, laid as a label's,
 * cf_label_word; a font sheet's row as cf_row lays it) and its word's letters marked to be drawn */
static void cf_under_word(uint32_t i, float* e, const float map[4], const float rt[2])
{
    const CfGlyph* k = g_cf.kept;
    uint32_t start = i;
    if (k[i].label_w > 0.0f)
    {
        for (uint32_t j = 0; j < i; ++j) /* (from its row's first letter kept, as cf_draw lays it) */
            if (cf_label_with(i, j, 0))
            {
                start = j;
                break;
            }
        cf_label_word(start, e, map, rt);
    }
    else
    {
        while (start > 0 && cf_under_joins(start - 1, start))
            --start;
        const uint32_t end = cf_row(start, e, map, rt);
        for (uint32_t m = start; m < end; ++m)
            cf_laid[m] = 1;
    }
    for (uint32_t m = i; m > 0 && cf_under_mate(m - 1, m); --m)
        cf_under_mark[m - 1] |= 2;
    for (uint32_t m = i + 1; m < g_cf.nkept && cf_under_mate(m - 1, m); ++m)
        cf_under_mark[m] |= 2;
}

static void cf_under(void)
{
    static float e[CF_MOST];
    static GfxDraw lay;
    CfGlyph* k = g_cf.kept;
    if (!cf_nunder)
        return;
    memset(cf_laid, 0, g_cf.nkept); /* (cf_row reads it; cf_draw sets it again for the laying) */
    const GfxDraw* run = &g_cf.runs[k[cf_under_list[0]].run];
    const float map[4] = { 0.0f, g_cf.sx / cf_scale(run, 0), 0.0f, g_cf.sy / cf_scale(run, 1) }, rt[2] = { 1.0f, 1.0f };
    for (uint32_t n = 0; n < cf_nunder; ++n)
        if (!cf_laid[cf_under_list[n]])
            cf_under_word(cf_under_list[n], e, map, rt);
    memset(&lay, 0, sizeof lay);
    lay.vp[2] = lay.vp[3] = 16384, lay.small_rt = run->small_rt, lay.overlay = run->overlay;
    g_cf.into[0] = 1.0f / map[1], g_cf.into[1] = 1.0f / map[3];
    for (int outline = 1; outline >= 0; --outline)
        for (uint32_t m = 0; m < g_cf.nkept; ++m)
            if (cf_under_mark[m] && !k[m].drawn && cf_in_pass(m, outline))
                cf_glyph(m, e[m], &lay, map, rt, outline);
    g_cf.into[0] = g_cf.into[1] = 0.0f;
    for (uint32_t m = 0; m < g_cf.nkept; ++m)
        if (cf_under_mark[m])
            cf_under_mark[m] = 0, k[m].drawn = 1;
    cf_nunder = 0;
}
