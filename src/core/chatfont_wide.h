// chatfont.cpp's, inside its namespace after the atlas's helpers (Cut, cut, place, blit, widen, style_of): each area's glyphs as
// built (Sheet), and True Font's letters past ASCII. An area's font is built with ASCII's letters alone (build); a cell of the
// font sheet past them is built into it once the graphics layer has seen one drawn in that area with no glyph
// (vanacore_gfx_wanted_glyphs: d3d8_chatfont_wide.h), a few a frame (grow), and until then it's the game's. Its character is the
// game's own reading of its text (Shift-JIS, and FFXI's row 85 of European letters: gametext.hpp): the kana, kanji and
// full-width signs of a game in Japanese, ñ and é. Until 2026-11-08 every letter but ASCII stayed the game's, whatever font
// was chosen (a player playing in Japanese: "I cannot change the font for Japanese characters"). The Japanese menus' labels'
// words (jplabels.cpp) are glyphs of an area the same way, their codes VANA_LABEL_WORDS on.

// The game's reading of one character of its text (0x1011CA80, cdecl: the address of the text's pointer, moved past what it
// read): its cell, row * 64 + column, ASCII's the byte less 0x20 and a Shift-JIS pair's by the game's own packing of JIS X
// 0208's rows (0x1011C540; the lead bytes 81-9F, E0-EF and FA-FC, 0x1011D630). push esi; mov esi,[esp+8]; push edi; mov edi,
// [esi]; push edi; call (one byte?); add esp,4; test al,al; jz; mov edx,[esi]; xor eax,eax; inc edx; mov [esi],edx; mov al,
// [edi]; pop edi; sub eax,20h; pop esi; ret
constexpr char kReadCode[] = "56 8B 74 24 08 57 8B 3E 57 E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 0F 8B 16 33 C0 42 89 16 8A 07 5F 83 E8 20 5E C3";
constexpr int kCells = VANA_LABEL_WORDS; // the font sheet's: 64 x 128; then the Japanese labels' words
constexpr int kCodes = VANA_GLYPH_CODES; // (d3d8_chatfont.h CF_CODES)
constexpr int kFullWidth = 14;  // the game's advance for a cell past its widths table (0x440 on, the kanji: 0x1011D440)
constexpr int kGrowMost = 32;   // glyphs past ASCII built in a frame, in all the areas (each a picture and its outline's)
constexpr int kAtlasMost = 8192; // an area's atlas no taller: its letters past that stay the game's

std::vector<wchar_t> g_cell_letter; // each cell's character (0: none), from kReadCode, once
bool g_cells_looked = false;

// An area's glyphs as built: their pictures (kept, so more are added without making the rest again), where the atlas's shelves
// are, and what they were made from
struct Sheet {
    Want want;
    float sx = 0.0f, sy = 0.0f;
    text::Style style, outlined; // its letters' and their outline's
    float stroke = 0.0f;         // the outline's width, the screen's pixels (0 none)
    std::vector<Cut> letters, outlines;
    std::vector<VanaChatGlyph> glyphs;
    int x = 0, y = 0, row = 0;
    std::vector<bool> tried; // the codes past ASCII asked for (built, or with no character or words)
    bool full = false;       // the atlas reached kAtlasMost
    double ms = 0.0;         // building past ASCII took, since built (logged once)
    int added = 0;
};
Sheet g_sheet[kAreas];

// Shift-JIS pair lead, trail as the game's text has it: its character, 0 for none (code page 932's "・" for a pair it hasn't, a
// private use one, a control)
wchar_t letter_of_pair(unsigned lead, unsigned trail)
{
    const char pair[2] = {static_cast<char>(lead), static_cast<char>(trail)};
    const std::wstring wide = gametext::to_wide(std::string_view(pair, 2));
    if (wide.size() != 1 || wide[0] < 0x20 || (wide[0] >= 0xE000 && wide[0] <= 0xF8FF))
        return 0;
    return wide[0] == 0x30FB && !(lead == 0x81 && trail == 0x45) ? 0 : wide[0];
}

// Each cell's character, by asking the game's reading of every Shift-JIS pair which cell it is: the first pair for a cell (the
// game reads the pairs it hasn't, FA to FC's and the like, as the cell of "〓", 81 AC, which comes before them), but for "◯",
// 81 FC, which it reads as full-width "３"'s cell, 246 (its check for JIS 227F, which sends it to 0xF2, is one past ◯'s 227E):
// that cell stays "３". Read off the game's conversion 2026-11-08 (every other cell has one letter; "Ñ" 639 and "¿" 767 as seen
// in the chat log 2026-11-07, "あ" 306, "漢" 1432).
void find_cells()
{
    g_cells_looked = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kReadCode) : 0;
    if (at == 0) {
        log::write(L"True Font: the game's reading of its text wasn't found in FFXiMain.dll: letters past ASCII (Japanese, ñ) "
                   L"stay the game's");
        return;
    }
    const auto read = reinterpret_cast<NextCode>(at);
    g_cell_letter.assign(kCells, 0);
    int found = 0;
    for (unsigned lead = 0x81; lead <= 0xFC; ++lead)
        for (unsigned trail = 0x40; trail <= 0xFC; ++trail) {
            const char pair[3] = {static_cast<char>(lead), static_cast<char>(trail), '\0'};
            const char* text = pair;
            const int code = static_cast<short>(read(&text));
            if (text != pair + 2 || code < kAscii || code >= kCells || g_cell_letter[code] != 0 || (lead == 0x81 && trail == 0xFC))
                continue;
            g_cell_letter[code] = letter_of_pair(lead, trail);
            found += g_cell_letter[code] != 0 ? 1 : 0;
        }
    log::write(L"True Font: %d of the font sheet's cells past ASCII are letters (\x3042 cell %d, \x6F22 %d, \xF1 %d)", found,
               static_cast<int>(std::find(g_cell_letter.begin(), g_cell_letter.end(), L'\x3042') - g_cell_letter.begin()),
               static_cast<int>(std::find(g_cell_letter.begin(), g_cell_letter.end(), L'\x6F22') - g_cell_letter.begin()),
               static_cast<int>(std::find(g_cell_letter.begin(), g_cell_letter.end(), L'\xF1') - g_cell_letter.begin()));
}

// The game's advance for cell `code`, the interface's pixels: its widths table's, the full cell past it
int cell_width(int code)
{
    const int width = code < static_cast<int>(kWidths) ? g_game[code] : kFullWidth;
    return width > 0 ? width : kFullWidth;
}

// Cell `code`, character c, a full-width letter: kana, kanji, the CJK signs and full-width forms, or any the game gives 13 or
// more (JIS X 0208's signs, Greek, Cyrillic, box drawing). Laid on the game's own grid (add_glyph); the rest, ñ and the like,
// as ASCII's are
bool full_width(int code, wchar_t c)
{
    return cell_width(code) >= 13 || (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF01 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6);
}

std::wstring capitals(const std::wstring& text)
{
    std::wstring upper = text;
    if (!upper.empty())
        CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size()));
    return upper;
}

// `text` has a kana, a kanji or a full-width form in it (a label's words in truefont.japanese's font)
bool japanese_in(const std::wstring& text)
{
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return (c >= 0x3000 && c <= 0x9FFF) || (c >= 0xFF00 && c <= 0xFFEF); });
}

// Code `code`'s text: a font sheet cell's character, a Japanese label's words; empty for none
std::wstring text_of(int code)
{
    if (code < kCells)
        return code < static_cast<int>(g_cell_letter.size()) && g_cell_letter[code] != 0 ? std::wstring(1, g_cell_letter[code]) : L"";
    const char* utf8 = jplabels::words(code - kCells);
    const int n = utf8 != nullptr ? MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0) : 0;
    std::wstring text(n > 1 ? static_cast<std::size_t>(n - 1) : 0, L' ');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, text.data(), n);
    return text;
}

// Text `text` (a character, or a Japanese label's words) for code `code` into area `which`'s sheet: its picture and its
// outline's, placed on the atlas's shelves (false: wider than the atlas, left out). A label's words as a label's letter is: in
// the area's font (truefont.japanese's when that's set and they're Japanese), its advance its own, not centred (it's laid in
// its label's width as it's drawn: d3d8_chatfont_jp.h). ASCII and
// the European letters as built before: in the area's font, centred in the game's width for them in the windows' areas, their
// advance the font's (a window's words laid by it: cf_row_words), the chat's widths in the game's table. A full-width letter in
// truefont.japanese's font where that's set, no larger than the game's full cell, centred in the game's width for it and
// advancing by that alone (its glyph's adv, with nothing to take off its x: lx), as the game lays them: a line of Japanese is
// as long as the game's, in every area, the chat's too, whatever the area's size.
bool add_glyph(int which, int code, const std::wstring& text, text::Sprite& sprite)
{
    Sheet& s = g_sheet[which];
    const Want& want = s.want;
    const bool word = code >= kCells;
    const bool wide = !word && !text.empty() && text[0] >= 0x80 && full_width(code, text[0]); // (not kExtras' [ and ])
    text::Style style = s.style, outlined = s.outlined;
    if ((wide || (word && japanese_in(text))) && !want.japanese.empty())
        style.family = outlined.family = want.japanese;
    if (wide) {
        const long most = std::lround(kFullWidth * want.widths * (s.sx < s.sy ? s.sx : s.sy));
        style.size = outlined.size = style.size < most ? style.size : static_cast<int>(most < 1 ? 1 : most);
    }
    const std::wstring letter = want.upper ? capitals(text) : text;
    sprite.set(letter, style);
    const float game = word ? 0.0f : static_cast<float>(cell_width(code)) * s.sx * want.widths;
    const float shift = !word && (wide || centred(which)) ? (game - sprite.exact_advance()) * 0.5f : 0.0f;
    VanaChatGlyph glyph{};
    glyph.code = static_cast<std::uint16_t>(code);
    glyph.adv = wide ? game : sprite.exact_advance();
    if (code < kAscii)
        g_advance[which][code] = s.sx > 0.0f ? sprite.exact_advance() / s.sx : 0.0f;
    Cut l = cut(sprite, shift), o;
    if (l.w >= kAtlasWidth)
        return false;
    if (which == kChat && !wide && !word && code < static_cast<int>(kWidths))
        glyph.frac = widen(sprite, code, s.sx);
    glyph.lx = wide ? l.x : l.x - shift;
    place(l, s.x, s.y, s.row);
    if (s.stroke > 0.0f) {
        sprite.set(letter, outlined);
        o = cut(sprite, shift);
        place(o, s.x, s.y, s.row);
    }
    s.letters.push_back(std::move(l)), s.outlines.push_back(std::move(o)), s.glyphs.push_back(glyph);
    return true;
}

// Area `which`'s sheet to the graphics layer: its atlas made from the pictures, each glyph's place in it
void publish(int which)
{
    Sheet& s = g_sheet[which];
    const int height = s.y + s.row + 1;
    std::vector<std::uint32_t> atlas(static_cast<std::size_t>(kAtlasWidth) * height, 0);
    for (std::size_t n = 0; n < s.glyphs.size(); ++n) {
        const Cut &l = s.letters[n], &o = s.outlines[n];
        blit(atlas, l), blit(atlas, o);
        VanaChatGlyph& g = s.glyphs[n];
        g.ax = static_cast<std::uint16_t>(l.ax), g.ay = static_cast<std::uint16_t>(l.ay);
        g.w = static_cast<std::uint16_t>(l.w), g.h = static_cast<std::uint16_t>(l.h), g.x = l.x, g.y = l.y;
        g.oax = static_cast<std::uint16_t>(o.ax), g.oay = static_cast<std::uint16_t>(o.ay);
        g.ow = static_cast<std::uint16_t>(o.w), g.oh = static_cast<std::uint16_t>(o.h), g.ox = o.x, g.oy = o.y;
    }
    const Want& want = s.want;
    const VanaTextFont font{atlas.data(), kAtlasWidth, static_cast<std::uint32_t>(height), s.glyphs.data(),
                            static_cast<std::uint32_t>(s.glyphs.size()), kBaseline, s.stroke > 0.0f ? want.outline_opacity : 0.0f,
                            want.line, want.size, want.pad, want.upper ? 1u : 0u};
    vanacore_gfx_set_chat_font(which, &font);
}

// Area `which`'s cells the graphics layer saw drawn with no glyph built into its sheet, at most `budget` of them; how many
int grow_area(int which, int budget)
{
    Sheet& s = g_sheet[which];
    std::uint16_t codes[kGrowMost];
    const std::uint32_t n = vanacore_gfx_wanted_glyphs(which, codes, static_cast<std::uint32_t>(budget));
    if (n == 0)
        return 0;
    if (!g_cells_looked)
        find_cells();
    LARGE_INTEGER start, end, hz;
    QueryPerformanceCounter(&start);
    text::Sprite sprite;
    int added = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const int code = codes[i];
        if (code >= kCodes || s.tried[code] || s.full)
            continue;
        const std::wstring text = text_of(code);
        s.tried[code] = true;
        if (text.empty() || !add_glyph(which, code, text, sprite))
            continue;
        ++added;
        s.full = s.y + s.row >= kAtlasMost;
    }
    if (added == 0)
        return 0;
    publish(which);
    QueryPerformanceCounter(&end), QueryPerformanceFrequency(&hz);
    s.ms += static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(hz.QuadPart);
    if (s.added == 0 || s.full)
        log::write(L"True Font: %ls: %d letters past ASCII built into it (%.1f ms)%ls", kArea[which].name, added, s.ms,
                   s.full ? L"; its atlas is full, the rest stay the game's" : L"");
    s.added += added;
    return added;
}

// Once a frame: the cells past ASCII the graphics layer saw drawn with no glyph built into their areas, kGrowMost in all
void grow()
{
    int budget = kGrowMost;
    for (int which = kChat; which < kAreas && budget > 0; ++which)
        if (g_built[which].on && !g_sheet[which].glyphs.empty())
            budget -= grow_area(which, budget);
}
