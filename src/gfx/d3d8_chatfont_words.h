/* True Font's windows' font sheet letters a word at a time (cf_row_words), with the chat line's caret (cf_caret) and the texts the
 * game centres or right-aligns (CfGlyph.centred: the status icons' timers; CfGlyph.right: the skill panels' levels, the Status
 * window's numbers). Included by d3d8_chatfont.h before cf_row (cf_kept_digit from d3d8_chatfont_big.h, cf_scale). */
#pragma once

/* The letters drawn from here (`on`) to here (off) are one text, its draw's own number (CfGlyph.centred, never 0: 65,535 before
 * one comes again, where a frame draws a few hundred), centred on x or (`right`, CfGlyph.right's) laid by it */
static void cf_text_mark(int on, int right, float x)
{
    if (on)
        g_cf.centred_n = (uint16_t)(g_cf.centred_n % 65535u + 1u), g_cf.centre_x = x;
    g_cf.centring = on ? g_cf.centred_n : 0, g_cf.right = (uint8_t)(on ? right : 0);
}

void d3d8_text_centred(int on, float x) { cf_text_mark(on, 0, x); }

void d3d8_text_right(int on) { cf_text_mark(on, 1, 0.0f); }

/* The texts VanaCore draws whole (d3d8_text_whole), by their draw's number: the last CF_TEXTS (a frame's two stats boxes draw
 * about 140), each its first CF_TEXT_MOST - 1 characters */
#define CF_TEXTS 512u
#define CF_TEXT_MOST 48u
static struct
{
    uint16_t id;
    char text[CF_TEXT_MOST];
} cf_texts[CF_TEXTS];

void d3d8_text_whole(int on, int right, const char* text)
{
    cf_text_mark(on, right ? 3 : 2, 0.0f);
    if (!on || !text)
        return;
    cf_texts[g_cf.centred_n % CF_TEXTS].id = g_cf.centred_n;
    char* to = cf_texts[g_cf.centred_n % CF_TEXTS].text;
    uint32_t n = 0;
    for (; n + 1 < CF_TEXT_MOST && text[n]; ++n)
        to[n] = text[n];
    to[n] = 0;
}

/* Whole text `id`'s spaces (d3d8_text_whole), for its `n` letters of a pass in their order across: after[m] how many of the text's
 * spaces come after the m-th, as it was given. 0 when its letters aren't the text's (one covered, a text cut short): the game's
 * gaps tell then. Its spaces the game drew (a letter of code 0 each) take their own room: none after. */
static int cf_whole_spaces(uint16_t id, uint32_t n, uint8_t* after)
{
    const char* text = cf_texts[id % CF_TEXTS].id == id ? cf_texts[id % CF_TEXTS].text : "";
    uint32_t letters = 0, all = 0;
    for (; text[all]; ++all)
        letters += text[all] != ' ';
    if (!n || (n != letters && n != all) || all + 1 >= CF_TEXT_MOST)
        return 0;
    memset(after, 0, n);
    for (uint32_t c = 0, m = 0; n == letters && text[c]; ++c)
        if (text[c] != ' ')
            ++m;
        else if (m > 0 && m <= n)
            after[m - 1]++;
    return 1;
}

/* Kept glyph i a text input's caret: its "|" with the next kept glyph on its row at its own pen. The line's draw
 * (FsTextInputDisplay's, 0x101B6DC0; the text boxes' twin of it, 0x101C4840: the macro editor's line) draws the text before the
 * caret with a "|" on its end, then the rest from where that text ends by the game's widths, the "|" left out of them (read live
 * 2026-11-07): the caret takes no room and the letter after it starts where it does. The rest was laid as a row of its own from
 * the game's place, past the font's narrower text: a gap opened at the caret, and closed as it blinked off (the line one draw
 * then; reported 2026-11-07, the macro editor's 2026-11-09). Any window font's (cf_per_word): a "|" the game laid takes its width */
static int cf_caret(uint32_t i)
{
    const CfGlyph* k = g_cf.kept;
    return cf_per_word(k[i].font) && k[i].code == '|' - 0x20 && i + 1 < g_cf.nkept && k[i + 1].font == k[i].font &&
        k[i + 1].py == k[i].py && fabsf(k[i + 1].px - k[i].px) <= 1.0f;
}

/* The game's width for kept glyph a (the interface's pixels, as it drew it): a label's letter's sprite's, else the font sheet
 * cell's (the font's picture was centred in it: chatfont.cpp), at the size the game drew it (CfGlyph.gs: smaller) */
static float cf_game_width(const CfGlyph* a, const CfFont* f, float per)
{
    const VanaChatGlyph* g = &f->glyphs[f->slot[a->code]];
    return a->label_w > 0.0f ? a->label_w : (2.0f * (g->x - g->lx) + g->adv) * a->scale[0] / per * a->gs[0];
}

/* The word of kept glyphs `word` to n - 1 ending in a digit (a number, "L.75") moved to end where the game's does, as the game
 * right-aligns its columns' numbers (laid from its start, the Change Jobs list's levels in a wider font ran into the next
 * column's job names, reported 2026-11-06), never nearer than `before` (a space after the word before). `own` is the last
 * letter's advance in the font. A word with a space the game drew in it (code 0: the lists' printer draws every letter, its
 * spaces too, so a name is one word) is a name, not a number: "Cursed Helm -1" in a bazaar's list was moved whole to end
 * where the game's wider letters end, and stood in from the other names (reported 2026-11-07). Never moved left past its draw's
 * clip (the game's viewport) where it starts inside it: the help bar's title box draws the page's name in a clip that starts
 * at its first letter, and a macro book named "DLSS5", moved to end where the game's narrower word does, lost half its D
 * (reported 2026-11-11). */
static void cf_word_moved(uint32_t word, uint32_t n, float* e, float own, float game, float before)
{
    const CfGlyph* k = g_cf.kept;
    if (!cf_kept_digit(&k[n - 1]))
        return;
    for (uint32_t m = word; m < n; ++m)
        if (k[m].code == 0)
            return;
    const float clip = fminf(0.0f, g_cf.runs[k[word].run].u.vp[0] - e[word]); /* (its clip's left, from where it starts) */
    const float shift = fmaxf(fmaxf(k[n - 1].px + game - (e[n - 1] + own), before - e[word]), clip);
    for (uint32_t m = word; m < n; ++m)
        e[m] += shift;
}

/* A row of a window's font sheet letters (kept glyphs i to j, cf_per_word's fonts) a word at a time: each word from where the
 * game's starts, its letters by the font's own advances (each was centred in the game's width for it: a wider font's letters
 * ran into each other, Raikiri's at 14 px, every list's at 29). A letter further on from the last than the game's width for
 * it (a space) starts another word, at the game's place or, where the font's last word reaches past it, a space after that;
 * the words moved by cf_word_moved. The caret takes no room (cf_caret): it stands where the letters each side of it meet. */
static void cf_row_words(uint32_t i, uint32_t j, float* e, const float map[4], const float rt[2])
{
    const CfGlyph* k = g_cf.kept;
    const CfFont* f = &g_cf.font[k[i].font];
    const float per = map[1] * cf_scale(&g_cf.runs[k[i].run], 0) / rt[0]; /* the screen's pixels per interface pixel */
    const float space = per > 0.0f && f->slot[0] >= 0 ? f->glyphs[f->slot[0]].adv / per : 0.0f;
    float before = -1.0e9f; /* where the word before this one ends, a space on */
    for (uint32_t n = i; n < j; ++n) /* (the game's places, as they stay with no scale to lay by) */
        e[n] = k[n].px;
    for (uint32_t n = i + 1, word = i; per > 0.0f && n <= j; ++n)
    {
        const float own = f->glyphs[f->slot[k[n - 1].code]].adv * k[n - 1].scale[0] / per, game = cf_game_width(&k[n - 1], f, per);
        if (n < j && cf_caret(n - 1))
            e[n] = e[n - 1], e[n - 1] -= own * 0.5f; /* (no room: the bar where the letters meet) */
        else if (n < j && k[n].px - k[n - 1].px <= game + 1.0f)
            e[n] = e[n - 1] + own;
        else
        {
            cf_word_moved(word, n, e, own, game, before);
            before = e[n - 1] + own + space;
            if (n < j)
                e[n] = fmaxf(k[n].px, before), word = n;
        }
    }
}

/* Kept glyph j one of centred or right-aligned text i's (CfGlyph.centred: one draw of the game's), not laid yet, in pass `shadow`
 * (the game's shadow copy under a label's letter, blend op REVSUBTRACT, 1; its letters 0; either, -1); a centred or whole one's in
 * its font (a right-aligned one's are put in one: cf_right_font) */
static int cf_centred_with(uint32_t i, uint32_t j, int shadow)
{
    const CfGlyph* k = g_cf.kept;
    return !cf_laid[j] && k[j].centred == k[i].centred && (k[i].right == 1 || k[j].font == k[i].font) &&
        (shadow < 0 || (g_cf.runs[k[j].run].pipe.op == 3) == shadow);
}

/* Right-aligned text i's letters (CfGlyph.right) are all the menus' labels' or big letters' (the titles' and Status & Numbers'
 * fonts: the skill panels', the Status window's, an item's count in its details), so it's laid whole (cf_centred_word). One with a
 * letter of another font's (the clock's, the party and alliance lists', a trade's, Current Gil's, the Equipment window's), or a
 * picture, or a font sheet cell, is left to that font's own rules (cf_big_words, cf_label_word): it's no longer marked */
static int cf_right_whole(uint32_t i)
{
    CfGlyph* k = g_cf.kept;
    const uint16_t id = k[i].centred;
    int whole = 1;
    for (uint32_t j = i; j < g_cf.nkept && whole; ++j)
        if (k[j].centred == id)
            whole = !k[j].pic && k[j].label_w > 0.0f && (k[j].font == VANA_FONT_STATUS || k[j].font == VANA_FONT_TITLES);
    for (uint32_t j = i; j < g_cf.nkept && !whole; ++j)
        if (k[j].centred == id)
            k[j].centred = 0, k[j].right = 0;
    return whole;
}

/* Right-aligned text i's letters in one font: where it has big letters (Status & Numbers': the digits), the labels' smaller ones
 * the game draws among them (its brackets, signs and slashes: "(", ")", "/", "+") taken into their font, standing on their baseline,
 * each pass apart (the game's shadow copies have their own), as a big letters' row takes them (cf_big_word). In the titles' font,
 * the Craft Skills' "(" and ")" were another size from its digits */
static void cf_right_font(uint32_t i)
{
    CfGlyph* k = g_cf.kept;
    const CfFont* big = &g_cf.font[VANA_FONT_STATUS];
    for (int shadow = 0; shadow < 2; ++shadow)
    {
        int at = -1; /* (a big letter of this pass: the baseline) */
        for (uint32_t j = i; j < g_cf.nkept && at < 0; ++j)
            at = cf_centred_with(i, j, shadow) && k[j].font == VANA_FONT_STATUS ? (int)j : -1;
        for (uint32_t j = i; at >= 0 && j < g_cf.nkept; ++j)
            if (cf_centred_with(i, j, shadow) && k[j].font != VANA_FONT_STATUS && big->slot[k[j].code] >= 0)
                k[j].font = VANA_FONT_STATUS, k[j].py = k[at].py;
    }
}

/* A text the game centres or right-aligns (kept glyph i's), laid by the font's own advances: centred on the middle the game
 * centres it on (CfGlyph.cx: the status icons' timers, each under its icon, wider or narrower than the game's in the middle; laid
 * from the game's left, a narrower font's sat left of the icon's middle, reported 2026-11-07), or (CfGlyph.right) ending where the
 * game's last letter's sprite ends, as the game right-aligns it (the skill panels' levels: laid as words by the big letters' rules,
 * the Craft Skills' "(7)" was "(" centred in its narrow sprite, apart from the 7, and ")" over it, "( )7", reported 2026-11-08; a
 * right-aligned text of another font's is left to its rules: cf_right_whole). A whole text VanaCore drew (d3d8_text_whole: right
 * 2) from the game's left, or (3) ending where the game's does. Its letters across in their order, the font's space where the
 * game's letters stand apart (a whole text's where its text has them: cf_whole_spaces); each pass apart (the game's shadow copy, a
 * little right of the letters, kept that far from them); every one marked laid. */
static void cf_centred_word(uint32_t i, float* e, const float map[4], const float rt[2])
{
    static uint16_t order[CF_MOST];
    static uint8_t after[CF_MOST];
    const CfGlyph* k = g_cf.kept;
    if (k[i].right == 1 && !cf_right_whole(i))
        return;
    if (k[i].right == 1)
        cf_right_font(i);
    const CfFont* f = &g_cf.font[k[i].font];
    const float per = map[1] * cf_scale(&g_cf.runs[k[i].run], 0) / rt[0]; /* the screen's pixels per interface pixel */
    const float space = per > 0.0f && f->slot[0] >= 0 ? f->glyphs[f->slot[0]].adv / per : 0.0f;
    float left = k[i].px; /* the text's left as the game drew it, either pass's */
    for (uint32_t j = i; j < g_cf.nkept; ++j)
        left = cf_centred_with(i, j, -1) ? fminf(left, k[j].px) : left;
    for (int shadow = 0; shadow < 2; ++shadow)
    {
        uint32_t n = 0;
        float end = -1.0e9f; /* (where the game's letters of this pass end: a right-aligned text's) */
        for (uint32_t j = i; j < g_cf.nkept; ++j)
        {
            if (!cf_centred_with(i, j, shadow))
                continue;
            end = (k[i].right & 1) && per > 0.0f ? fmaxf(end, k[j].px + cf_game_width(&k[j], &g_cf.font[k[j].font], per)) : end;
            uint32_t m = n++;
            for (; m > 0 && k[order[m - 1]].px > k[j].px; --m) /* (into its place across) */
                order[m] = order[m - 1];
            order[m] = (uint16_t)j;
        }
        const int told = k[i].right >= 2 && cf_whole_spaces(k[i].centred, n, after); /* (a whole text's spaces where it has them) */
        float pen = 0.0f;
        for (uint32_t m = 0; m < n; ++m)
        {
            const CfGlyph* a = &k[order[m]];
            const CfFont* af = &g_cf.font[a->font]; /* (a right-aligned text's letter the big letters' font has none for: its own) */
            const float own = per > 0.0f ? af->glyphs[af->slot[a->code]].adv * a->scale[0] / per : 0.0f;
            const int apart = told ? after[m]
                                   : m + 1 < n && per > 0.0f && k[order[m + 1]].px - a->px - cf_game_width(a, af, per) > 1.5f * a->scale[0];
            e[order[m]] = pen, pen += own + (m + 1 < n ? (float)apart * space : 0.0f);
        }
        const float start = !(per > 0.0f) || !n ? 0.0f
            : (k[i].right & 1)                  ? end - pen
            : k[i].right                        ? k[order[0]].px
                                                : k[i].cx - pen * 0.5f + (k[order[0]].px - left);
        for (uint32_t m = 0; m < n; ++m)
            e[order[m]] = per > 0.0f ? e[order[m]] + start : k[order[m]].px, cf_laid[order[m]] = 1;
    }
}
