#pragma once

// The game's menu font ("font    font", the English menu DAT's ROM\119\51) as its own menus' words are made of it: each
// letter where it is in the font's texture and how far it moves the next one on. The texture holds the letters in ASCII
// order, rows 13 apart (A-D from u 77 of v 26, E-P v 39, Q-Y v 52, a-g v 65, h-s v 78, t-z v 92), a short letter 2 lower
// in its row than a tall one. The sizes and the advances are the game's own, each the most common across every menu word in
// ROM\119\51.DAT (its sheets' parts on "font    font", drawn a texel to a pixel): words set with them read as the game's
// do (Config > Gameplay's captions are exactly these, 5 between words). Z and 0-9 aren't known yet: a letter not here is
// left as a gap. configmenu.cpp's labels and gamesettings.cpp's captions.
namespace vanacore::menufont {

struct Glyph {
    char c;
    int y, u, v, w, h, advance;
};

inline constexpr Glyph kGlyphs[] = {
    {'A', 1, 77, 26, 9, 11, 9},   {'B', 1, 87, 26, 9, 11, 9},   {'C', 1, 97, 26, 9, 12, 9},   {'D', 1, 107, 26, 9, 11, 9},
    {'E', 1, 0, 39, 9, 11, 9},    {'F', 1, 10, 39, 9, 11, 8},   {'G', 1, 20, 39, 9, 12, 9},   {'H', 1, 31, 39, 9, 11, 9},
    {'I', 1, 42, 39, 6, 11, 5},   {'J', 1, 48, 39, 9, 12, 9},   {'K', 1, 58, 39, 9, 11, 9},   {'L', 1, 69, 39, 9, 11, 9},
    {'M', 1, 80, 39, 9, 11, 9},   {'N', 1, 91, 39, 9, 11, 9},   {'O', 1, 102, 39, 9, 12, 9},  {'P', 1, 113, 39, 9, 11, 8},
    {'Q', 1, 0, 52, 9, 12, 9},    {'R', 1, 11, 52, 10, 11, 9},  {'S', 1, 22, 52, 9, 12, 9},   {'T', 1, 31, 52, 9, 11, 8},
    {'U', 1, 41, 52, 9, 12, 8},   {'V', 1, 52, 52, 10, 11, 9},  {'W', 1, 63, 52, 9, 11, 9},   {'X', 1, 76, 52, 9, 11, 9},
    {'Y', 1, 88, 52, 9, 11, 9},
    {'a', 3, 49, 67, 8, 9, 8},    {'b', 1, 58, 65, 9, 11, 8},   {'c', 3, 68, 67, 9, 9, 9},    {'d', 1, 78, 65, 9, 11, 9},
    {'e', 3, 89, 67, 9, 9, 9},    {'f', 1, 99, 65, 7, 11, 6},   {'g', 3, 107, 67, 9, 12, 9},  {'h', 1, 0, 78, 9, 11, 9},
    {'i', 1, 10, 78, 5, 11, 5},   {'j', 1, 16, 78, 8, 14, 7},   {'k', 1, 25, 78, 9, 11, 9},   {'l', 1, 35, 78, 6, 11, 6},
    {'m', 3, 42, 80, 9, 9, 9},    {'n', 3, 56, 80, 9, 9, 9},    {'o', 3, 66, 80, 9, 9, 9},    {'p', 3, 76, 80, 9, 12, 9},
    {'q', 3, 87, 80, 9, 12, 9},   {'r', 3, 98, 80, 7, 9, 7},    {'s', 3, 106, 80, 9, 9, 9},   {'t', 2, 0, 92, 6, 10, 6},
    {'u', 3, 7, 93, 9, 9, 9},     {'v', 3, 17, 93, 9, 9, 8},    {'w', 3, 27, 93, 9, 9, 8},    {'x', 3, 39, 93, 9, 9, 9},
    {'y', 3, 50, 93, 9, 12, 8},   {'z', 3, 60, 93, 8, 9, 8},
};

// The letter `c`, or nullptr (a space, or one not known yet)
inline const Glyph* glyph(char c)
{
    for (const Glyph& g : kGlyphs)
        if (g.c == c)
            return &g;
    return nullptr;
}

} // namespace vanacore::menufont
