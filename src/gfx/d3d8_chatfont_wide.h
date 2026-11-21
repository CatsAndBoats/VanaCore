/* True Font's letters past ASCII: the kana, kanji and full-width signs of a game in Japanese, the European letters' row (ñ, é).
 * chatfont.cpp builds each area's glyphs for ASCII alone, and for any other cell of the font sheet once one is drawn in that
 * area: a cell kept with no glyph in its area's font is noted here (cf_want_glyph), chatfont.cpp takes the notes each frame
 * (d3d8_wanted_glyphs) and sends the font again with them, and until then those letters are the game's. (Before 2026-11-08 every
 * letter but ASCII stayed the game's whatever font was chosen: a player in Japanese reported theirs couldn't be changed.)
 * Included by d3d8_chatfont.h after g_cf. */
#pragma once

static uint8_t cf_wanted[VANA_FONTS][CF_CODES / 8]; /* each area's cells drawn with no glyph, a bit each, since last taken */
static uint8_t cf_wanted_any[VANA_FONTS];

/* Cell `code` drawn in area `font` (`cell`: it is a font sheet cell, not a label's letter): noted when that area's font is on and
 * has no glyph for it (ASCII's are always built) */
static void cf_want_glyph(int font, uint32_t code, int cell)
{
    if (!cell || font < 0 || font >= VANA_FONTS || code < 0x5F || code >= CF_CODES)
        return;
    const CfFont* f = &g_cf.font[font];
    if (!f->on || f->slot[code] >= 0)
        return;
    cf_wanted[font][code >> 3] |= (uint8_t)(1u << (code & 7));
    cf_wanted_any[font] = 1;
}

/* Font f about to be replaced by `in` with glyphs only added on the end of its own (chatfont.cpp's letters past ASCII): the frame's
 * kept glyphs stay, each one's slot the same. Any other font drops them (d3d8_set_chat_font). With the menus at Fill window the
 * kept glyphs are drawn at the frame's end (chatfont_present), after chatfont.cpp sends the fonts, so a font grown every frame
 * new letters came lost those frames' letters. */
static int cf_grown(const CfFont* f, const VanaTextFont* in)
{
    if (!f->on || !f->glyphs || !in || !in->atlas || !in->w || !in->h || !in->glyphs || in->count < f->nglyphs || in->count > 32767)
        return 0;
    for (uint32_t i = 0; i < f->nglyphs; ++i)
        if (in->glyphs[i].code != f->glyphs[i].code)
            return 0;
    return 1;
}

uint32_t d3d8_wanted_glyphs(int font, uint16_t* codes, uint32_t most)
{
    if (font < 0 || font >= VANA_FONTS || !codes || !cf_wanted_any[font])
        return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < CF_CODES / 8; ++i)
    {
        uint8_t* bits = &cf_wanted[font][i];
        for (uint32_t b = 0; *bits && b < 8; ++b)
        {
            if (!(*bits & (1u << b)))
                continue;
            if (n == most)
                return n; /* (the rest stay noted, and cf_wanted_any with them) */
            codes[n++] = (uint16_t)(i * 8 + b);
            *bits &= (uint8_t)~(1u << b);
        }
    }
    cf_wanted_any[font] = 0;
    return n;
}
