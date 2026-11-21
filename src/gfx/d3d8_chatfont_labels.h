/* True Font's menu labels a word at a time (cf_label_word): the letters of one label of the buttons', the titles' or an area's of
 * their own, laid by the font's own advances from where the game's word starts (cf_word). Included by d3d8_chatfont.h after
 * cf_word and cf_laid, before cf_big_words. */

/* How narrow a squeezed main menu label's letters are drawn at most, to the font's own: a little narrower reads as the same
 * letters, the game's 0.60 to 0.78 as squashed ones (cf_unsqueeze) */
#define CF_NARROWEST 0.85f

/* A main menu label the game squeezed across to fit its button (the Macros menu's "Edit Equip.Set": its letters 0.60 to 0.78
 * as wide as their texels, as tall as theirs; "Edit Macro" over it 1.00, read live 2026-11-09; "super squashed"): its
 * letters as tall as the menu's other labels, and at the font's own width (mainmenu.font.fixed, g_cf.label_fixed: on unset) or
 * as narrow as the game's on average but no narrower than CF_NARROWEST; what doesn't fit runs on past the button, as an
 * unsqueezed label in a font larger than the game's does ("Current Time"). The whole word made smaller to end where the game's
 * does instead, "Communication" stood about two thirds as tall as the labels around it (Okreya, 2026-11-09); at 85% wide, as
 * tall as "Current Time" (12 px capitals, 9 px x-height, measured live), it still read as small. The label's `n` letters
 * in `order`, left to right */
static void cf_unsqueeze(const uint16_t* order, uint32_t n)
{
    CfGlyph* k = g_cf.kept;
    if (n == 0 || k[order[0]].font != VANA_FONT_MAINMENU)
        return;
    int squeezed = 0;
    float across = 0.0f; /* (the game's letters' widths to their heights, added up) */
    for (uint32_t m = 0; m < n; ++m)
    {
        const CfGlyph* g = &k[order[m]];
        squeezed |= g->scale[0] < g->scale[1] * 0.97f;
        across += g->scale[1] > 0.0f ? g->scale[0] / g->scale[1] : 1.0f;
    }
    if (!squeezed)
        return;
    /* (every letter alike: the game's vary with its rounding) */
    const float keep = g_cf.label_fixed ? 1.0f : fmaxf(across / (float)n, CF_NARROWEST);
    for (uint32_t m = 0; m < n; ++m)
        k[order[m]].scale[0] = k[order[m]].scale[1] * keep;
}

/* Kept glyph j one of menu label glyph i's (cf_label_word): its font's, its pass's (op: the game's shadow copy its own), on
 * its row and not laid yet; drawn straight after the letters before it from i (`joined`), else in the same window's clip */
static int cf_label_with(uint32_t i, uint32_t j, int joined)
{
    const CfGlyph* k = g_cf.kept;
    const GfxDraw *a = &g_cf.runs[k[i].run], *b = &g_cf.runs[k[j].run];
    return !cf_laid[j] && k[j].font == k[i].font && b->pipe.op == a->pipe.op && cf_same_row(&k[j], &k[i]) &&
        (joined || !memcmp(a->u.vp, b->u.vp, sizeof a->u.vp));
}

/* How far right of another row's label in its window a menu label may start and still be laid from where that one starts, the
 * interface's pixels (cf_label_left): the game places some a little in from the rest of their column, the Equip Set menu's
 * Options' "Delete" 3 screen pixels right of "Edit", "Copy" ... "Style Lock" (its "D" no wider than their capitals; read live
 * 2026-11-09, True Font off and on: "the text still isn't aligned") */
#define CF_COLUMN 4.0f

/* Kept glyphs a and b drawn in one window's clip, in one pass (op: the game's shadow copy its own) */
static int cf_same_window(uint32_t a, uint32_t b)
{
    const GfxDraw *x = &g_cf.runs[g_cf.kept[a].run], *y = &g_cf.runs[g_cf.kept[b].run];
    return x->pipe.op == y->pipe.op && !memcmp(x->u.vp, y->u.vp, sizeof x->u.vp);
}

/* Kept glyph m a label's first letter: no letter of its font just before it on its row in its window */
static int cf_label_first(uint32_t m)
{
    const CfGlyph* k = g_cf.kept;
    for (uint32_t q = 0; q < g_cf.nkept; ++q)
        if (k[q].font == k[m].font && cf_same_row(&k[q], &k[m]) && k[q].px < k[m].px && k[q].px >= k[m].px - 16.0f &&
            cf_same_window(q, m))
            return 0;
    return 1;
}

/* Where the menu label whose first letter is kept glyph `first` starts: the game's place, or the leftmost start of another row's
 * label of its font in its window up to CF_COLUMN left of it, so a column's labels start together. The buttons' labels alone
 * (Buttons, Main Commands, Action Menu): the slot names under the Equipment window's squares, each centred, stay the game's */
static float cf_label_left(uint32_t first)
{
    const CfGlyph* k = g_cf.kept;
    const int font = k[first].font;
    float left = k[first].px;
    if (font != VANA_FONT_BUTTONS && font != VANA_FONT_MAINMENU && font != VANA_FONT_ACTIONMENU)
        return left;
    for (uint32_t m = 0; m < g_cf.nkept; ++m)
        if (k[m].label_w > 0.0f && k[m].font == k[first].font && k[m].px < left && k[m].px >= k[first].px - CF_COLUMN &&
            !cf_same_row(&k[m], &k[first]) && cf_same_window(m, first) && cf_label_first(m))
            left = k[m].px;
    return left;
}

/* A menu label's letters from kept glyph i (its font's, the buttons' or the titles': one pass of them, on one row, each set on
 * the first's baseline, cf_label_with) laid out by cf_word left to right, and marked laid: the game draws some out of order
 * ("Magic" as M g i a c, "Key Items" its I last), and some a part after both passes (the Search menu's "Linkshell 2": its 2
 * after the shadow and the letters of "Linkshell", so it was left at the game's place, far past True Font's narrower word).
 * Gives the kept glyph after i. */
static uint32_t cf_label_word(uint32_t i, float* e, const float map[4], const float rt[2])
{
    static uint16_t order[CF_MOST];
    CfGlyph* k = g_cf.kept;
    uint32_t n = 0;
    for (uint32_t j = i, joined = 1; j < g_cf.nkept; ++j)
    {
        if (!cf_label_with(i, j, (int)joined))
        {
            joined = 0;
            continue;
        }
        k[j].py = k[i].py, cf_laid[j] = 1;
        uint32_t m = n++;
        for (; m > 0 && k[order[m - 1]].px > k[j].px; --m) /* (into its place across) */
            order[m] = order[m - 1];
        order[m] = (uint16_t)j;
    }
    cf_unsqueeze(order, n); /* (a main menu label the game squeezed: as tall as the others, a little narrower) */
    e[order[0]] = cf_label_left(order[0]); /* (with its column's other labels) */
    for (uint32_t m = 1; m < n; ++m)
        e[order[m]] = cf_word(&k[order[m - 1]], &k[order[m]], e[order[m - 1]], map, rt);
    return i + 1;
}
