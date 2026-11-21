/* True Font's big letters' rows (the Status window's, the party and alliance lists', the trade windows'): each a word at a
 * time, a number ending where the game's does. Included by d3d8_chatfont.h after cf_label_word (cf_word, cf_laid). */

/* A kept letter a digit (its code the font sheet's, ' ' 0) */
static int cf_kept_digit(const CfGlyph* g) { return g->code >= '0' - 0x20 && g->code <= '9' - 0x20; }

/* Kept letter b no further on from a's sprite than `most` of a's scale */
static int cf_close(const CfGlyph* a, const CfGlyph* b, float most) { return b->px - a->px - a->label_w <= most * a->scale[0]; }

/* Big letter order[m] of a row's (across, n of them) part of a number: a digit, a sign just before one, a decimal point or a
 * thousands' comma between two (the gil's "4,821,789" one number: its groups, each laid to end where the game's does, ran into
 * each other in a larger font: reported 2026-11-06), a per cent sign just after one (the Equipment window's stats' "+5 %": its
 * sign, its digits and its % were three words, the 5 laid to end where the game's does and the font's % centred in the game's
 * narrower one, over the 5: reported 2026-11-06) */
static int cf_number_part(const uint16_t* order, uint32_t n, uint32_t m)
{
    const CfGlyph* k = g_cf.kept;
    const CfGlyph *g = &k[order[m]], *before = m > 0 ? &k[order[m - 1]] : NULL, *after = m + 1 < n ? &k[order[m + 1]] : NULL;
    const int digit_before = before && cf_kept_digit(before), digit_after = after && cf_kept_digit(after);
    switch (g->code + 0x20)
    {
    case '+':
    case '-':
        return digit_after && cf_close(g, after, 1.5f);
    case '.':
    case ',':
        return digit_before && digit_after && cf_close(before, g, 1.5f) && cf_close(g, after, 1.5f);
    case '%':
        return digit_before && cf_close(before, g, 1.5f);
    default:
        return cf_kept_digit(g);
    }
}

/* Big letters a and b (across; na, nb whether each is a number's: cf_number_part) in different words: a gap of more than a space
 * or two between them; where a number starts or stops (a long name runs up to its HP; the Status window's "Lv" and its level,
 * right-aligned 11 units on, were one word with "Dancer" and stretched to end where it does: "D a n c e r", reported
 * 2026-11-06); and between two of a number's a gap apart, where a comma or a space stands that the font may not draw (the gil's
 * "4,821,789" with the titles' font off: the digits were laid as one number over the game's commas) */
static int cf_apart(const CfGlyph* a, const CfGlyph* b, int na, int nb)
{
    const float gap = b->px - a->px - a->label_w, s = a->scale[0];
    return gap > 5.0f * s || na != nb || (na && gap > 1.5f * s);
}

/* Big letter order[m] (of a row's n, across; number: cf_number_part's) a number's unit: a lone letter just after a number, a
 * space or so on (the gil's "G"), laid with it as one word (cf_fit_word) */
static int cf_unit(const uint16_t* order, uint32_t n, uint32_t m, const uint8_t* number)
{
    const CfGlyph* k = g_cf.kept;
    if (m == 0 || !number[m - 1] || number[m])
        return 0;
    const CfGlyph *a = &k[order[m - 1]], *g = &k[order[m]];
    const int c = g->code + 0x20;
    const float gap = g->px - a->px - a->label_w;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) || gap > fmaxf(8.0f * a->scale[0], 2.0f * a->label_w))
        return 0;
    return m + 1 == n || cf_apart(g, &k[order[m + 1]], 0, number[m + 1]);
}

/* Kept letter g's drawn right edge past its pen (its outline's, else its letter's), the interface's pixels (per: the screen's
 * pixels per interface pixel), less the centring in the game's width chatfont.cpp gave a windows' font's pictures (cf_glyph
 * takes it off) */
static float cf_ink_right(const CfGlyph* g, float per)
{
    const CfFont* f = &g_cf.font[g->font];
    const VanaChatGlyph* m = &f->glyphs[f->slot[g->code]];
    const float right = f->outline > 0.0f && m->ow > 0 ? m->ox + (float)m->ow : m->x + (float)m->w;
    return (right - (cf_per_word(g->font) ? m->x - m->lx : 0.0f)) * g->scale[0] / per;
}

/* One word of big letters (order[0..n), across) laid by the font's own advances from where the game's starts (cf_word), then
 * stretched about its start to end where the game's last sprite does, by a little: the game centres some words by its own
 * widths (the M of "Movement" was centred in the game's narrower M and stood left of its column), but a narrower font's word
 * stretched further came out letter-spaced ("D a n c e r"), so a word the font makes much narrower is laid from its start
 * as it is. A lone letter centred in its sprite. A `number` (1: cf_number_part's) is never stretched: laid by the font's own
 * advances, ending where the game's does (the game right-aligns them); one with its unit (2: cf_unit) ends with the unit's
 * drawn edge 2 pixels before the game's unit starts, the whole amount moved left with it: the delivery boxes put their gil
 * coin where the game's "G" stands (read live 2026-11-06: both at x 515.8), so the font's G, ending at that sprite's end and
 * then 2 pixels inside it, stood over the coin (reported twice 2026-11-06). A font in capitals (CfFont.upper) isn't squeezed:
 * its words are wider than the game's lower-case ones, and their pens drawn closer ran the letters into each other. */
static void cf_fit_word(const uint16_t* order, uint32_t n, int number, float* e, const float map[4], const float rt[2])
{
    const CfGlyph* k = g_cf.kept;
    const CfGlyph *first = &k[order[0]], *last = &k[order[n - 1]];
    const CfFont* f = &g_cf.font[last->font];
    const float per = map[1] * cf_scale(&g_cf.runs[last->run], 0) / rt[0]; /* the screen's pixels per interface pixel */
    const float adv = per > 0.0f ? f->glyphs[f->slot[last->code]].adv * last->scale[0] / per : 0.0f;
    e[order[0]] = n == 1 && adv > 0.0f ? first->px + (first->label_w - adv) * 0.5f : first->px;
    for (uint32_t m = 1; m < n; ++m)
        e[order[m]] = cf_word(&k[order[m - 1]], &k[order[m]], e[order[m - 1]], map, rt);
    if (adv > 0.0f && number)
    {
        const float end = number == 2 ? cf_ink_right(last, per) + last->label_w + 2.0f * last->scale[0] : adv;
        const float right = last->px + last->label_w - end - e[order[n - 1]];
        for (uint32_t m = 0; m < n; ++m)
            e[order[m]] += right;
        return;
    }
    const float span = e[order[n - 1]] - first->px, game = last->px + last->label_w - adv - first->px;
    const float stretch = n > 1 && adv > 0.0f && span > 0.0f ? game / span : 1.0f;
    if (stretch < 0.75f || stretch > 1.1f || (stretch < 1.0f && f->upper))
        return;
    for (uint32_t m = 1; m < n; ++m)
        e[order[m]] = first->px + (e[order[m]] - first->px) * stretch;
}

/* A word of a big letters' row (order[0..n), across), if any of it is a big letter: laid (cf_fit_word), its labels' letters
 * taken into the big letters' font with it (the game draws "+", "%", "(" and the Status window's job names, "Ninja", from the
 * labels' smaller letters: they were the titles' font, another size, and split the word) and every letter on the row's
 * baseline `py`. A word of labels alone is left to cf_label_word. `font` the row's (cf_big: the Status area's, the party
 * list's or the alliance lists', whose labels' letters are theirs already); `number` whether it's a number (cf_fit_word): the
 * lists' numbers in their own size's font while it's on (party.font.numbers.size), on the same baseline (every font's: kBaseline).
 * Whether it was laid. */
static int cf_big_word(const uint16_t* order, uint32_t n, int font, int number, float py, float* e, const float map[4],
                       const float rt[2])
{
    CfGlyph* k = g_cf.kept;
    int big = 0;
    for (uint32_t m = 0; m < n; ++m)
        big |= k[order[m]].font == font;
    if (!big)
        return 0;
    const int list = font == VANA_FONT_PARTY ? VANA_FONT_PARTY_NUMBERS : font == VANA_FONT_ALLIANCE ? VANA_FONT_ALLIANCE_NUMBERS : -1;
    const int own = number && list >= 0 && g_cf.font[list].on ? list : font;
    const int gone = number && list >= 0 && g_cf.hide[list] == 2; /* (party.font.numbers.size 0: the numbers not drawn) */
    for (uint32_t m = 0; m < n; ++m)
    {
        CfGlyph* g = &k[order[m]];
        if (gone)
            g->fade = 255;
        const int to = g_cf.font[own].slot[g->code] >= 0 ? own : font;
        if (g_cf.font[to].slot[g->code] >= 0)
            g->font = (uint8_t)to;
        g->py = py, cf_laid[order[m]] = 1;
    }
    cf_fit_word(order, n, number, e, map, rt);
    return 1;
}

/* A laid word of big letters (order[0..n), across: cf_big_word) moved on to start no nearer than `before` (the end of the word
 * before it and a space), unless it's a number (`number`: it ends where the game's does, as the game right-aligns them); where
 * it ends, a space on. Each word is laid from where the game's starts, by the font's advances: in a font larger than the
 * game's, the words ran into each other (the Equipment window's stats' "Physical damage taken" at 17 px, reported 2026-11-06) */
static float cf_big_after(const uint16_t* order, uint32_t n, int number, float before, float* e, const float map[4], const float rt[2])
{
    const CfGlyph* last = &g_cf.kept[order[n - 1]];
    const CfFont* f = &g_cf.font[last->font];
    const float per = map[1] * cf_scale(&g_cf.runs[last->run], 0) / rt[0]; /* the screen's pixels per interface pixel */
    if (!(per > 0.0f) || f->slot[last->code] < 0)
        return before;
    if (!number && e[order[0]] < before)
    {
        const float shift = before - e[order[0]];
        for (uint32_t m = 0; m < n; ++m)
            e[order[m]] += shift;
    }
    const float space = f->slot[0] >= 0 ? f->glyphs[f->slot[0]].adv : 0.0f;
    return e[order[n - 1]] + (f->glyphs[f->slot[last->code]].adv + space) * last->scale[0] / per;
}

/* The big letters' row from kept glyph i (a cf_big font's): the big letters and the labels' letters of its pass (op: the
 * game's shadow copies are their own) and row not laid yet, wherever they are in the kept list (the game draws each letter a
 * shape of its own: a row's needn't follow each other), across, a word at a time (cf_big_word), a number's unit with it
 * (cf_unit). Not the font sheet's letters (label_w 0) in a font of its own (the trade windows': cf_row's) */
static void cf_big_words(uint32_t i, float* e, const float map[4], const float rt[2])
{
    static uint16_t order[CF_MOST];
    const CfGlyph* k = g_cf.kept;
    const int op = g_cf.runs[k[i].run].pipe.op, big = k[i].font;
    uint32_t n = 0;
    for (uint32_t j = i; j < g_cf.nkept; ++j)
    {
        const int font = k[j].font;
        if (cf_laid[j] || k[j].pic || !(k[j].label_w > 0.0f) ||
            (font != big && ((big != VANA_FONT_STATUS && !cf_whole(big)) || font != VANA_FONT_TITLES)) ||
            g_cf.runs[k[j].run].pipe.op != op || !cf_same_row(&k[j], &k[i]))
            continue;
        uint32_t m = n++;
        for (; m > 0 && k[order[m - 1]].px > k[j].px; --m) /* (into its place across) */
            order[m] = order[m - 1];
        order[m] = (uint16_t)j;
    }
    static uint8_t number[CF_MOST];
    for (uint32_t m = 0; m < n; ++m)
        number[m] = (uint8_t)cf_number_part(order, n, m);
    const float py = k[i].py;
    float before = -1.0e9f; /* the word before's end and a space (cf_big_after) */
    for (uint32_t from = 0, m = 1; m <= n; ++m)
        if (m == n || (cf_apart(&k[order[m - 1]], &k[order[m]], number[m - 1], number[m]) && !cf_unit(order, n, m, number)))
        {
            const int kind = !number[from] ? 0 : number[m - 1] ? 1 : 2;
            if (cf_big_word(order + from, m - from, big, kind, py, e, map, rt))
                before = cf_big_after(order + from, m - from, kind, before, e, map, rt);
            from = m;
        }
}
