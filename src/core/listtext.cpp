#include "listtext.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "gameui.hpp"
#include "log.hpp"
#include "text.hpp"
#include "textcache.hpp"
#include "uispace.hpp"

namespace vanacore::listtext {
namespace {

constexpr std::ptrdiff_t kLayoutName = 0x46; // a layout's name, 16 letters
constexpr std::ptrdiff_t kRect = 0x3A;       // a window's left, top, right and bottom (shorts, the interface's units)
// The windows taken, by their layout's own name's first letters (after "menu    "): Items, Satchel and the other bags
// ("inventor"), Mog House's storage ("mnstorag", "itmstora") and the banks'
constexpr const char* kLists[] = {"inventor", "mnstorag", "itmstora", "bank"};
constexpr char kEquipLayout[16] = {'m', 'e', 'n', 'u', ' ', ' ', ' ', ' ', 'e', 'q', 'u', 'i', 'p', ' ', ' ', ' '};
constexpr int kCellBaseline = 14; // the menu font's 16 x 16 cell: its capitals stand on row 14, below the text draw's y
constexpr std::size_t kLetters = 64;
constexpr int kTextsMost = 256, kWindowsMost = 96, kPiecesMost = 16;
constexpr DWORD kStale = 250; // texts older than this were never drawn (the interface hidden): dropped

// The Equipment window's squares, left to right in rows of four, and the name each has (the same words in English and Japanese:
// the digits of "Ear 1" and "Ring 2" are the game's own small numerals, left out with its letters)
constexpr int kSlots = 16, kPerRow = 4, kSquareSide = 32, kSquaresMost = 32, kIconsMost = 32;
constexpr const char* kSlotNames[kSlots] = {"Main", "Sub",  "Range", "Ammo", "Head", "Neck", "Ear 1", "Ear 2",
                                            "Body", "Hands", "Ring 1", "Ring 2", "Back", "Waist", "Legs", "Feet"};
// A name's top left in from its square's (the interface's units), its colour (the game's: 80h a channel full, alpha 66h of 80h)
constexpr int kPadLeft = 3, kPadTop = 2;
constexpr std::uint32_t kSlotColour = 0x66707078;
constexpr std::uint32_t kStatColour = 0x80808080; // a stat's: white, full (equipstats.cpp's kWhite)
constexpr int kRowGap = 16; // squares whose tops are further apart than this are on different rows (a row is 35 apart)
constexpr int kIconSlack = 2; // an icon covers a square when the square's middle is inside the icon's 32, this much out

struct Placed {
    const void* window;
    short rect[4];
};
struct Text {
    const void* window; // the list's
    int index;          // its place in the frame's windows, in the order they were drawn
    int x, base;        // its pen's start and its baseline, the interface's units (corner: its top)
    std::uint32_t colour; // the game's draw's (A R G B, 80h full)
    bool corner;        // x, base are where the text's top left goes (a slot name): not a pen on a baseline (a list's name)
    int area;           // the True Font area it's in (VANA_FONT_ITEMS, VANA_FONT_EQUIP, VANA_FONT_EQUIP_STATS)
    bool right;         // x is where its pen ends, not where it starts (a stat's number)
    bool own;           // cut to its own box, not its window's rectangle (a stats box that stands outside the window)
    char text[kLetters];
};
// A square or an icon the Equipment window drew: its window (and its place in the frame's windows) and top left
struct Spot {
    const void* window;
    int index;
    int x, y;
    int row;
};
// A rectangle in the drawn pixels: left and top in, right and bottom out
struct Box {
    int left, top, right, bottom;
};
struct Pieces {
    Box box[kPiecesMost] = {};
    int count = 0;
    void add(const Box& b)
    {
        if (b.right > b.left && b.bottom > b.top && count < kPiecesMost)
            box[count++] = b;
    }
};
// This frame's windows in the drawn pixels (`known`: its rectangle could be placed), the order they were drawn in
struct Frame {
    Box boxes[kWindowsMost] = {};
    bool known[kWindowsMost] = {};
    int windows = 0;
};
// The items' True Font as built, and whether its letters are capitals
struct Look {
    text::Style style;
    bool upper = false;
};

Placed g_windows[kWindowsMost] = {}; // this frame's windows as they were drawn (draw takes them)
int g_window_count = 0;
int g_now = -1;       // the item list being drawn's place in them, -1 none
int g_equip_now = -1; // the Equipment window being drawn's, -1 none
int g_cur = -1;       // the window being drawn's, whichever it is (a stat is kept in it), -1 none
Text g_texts[kTextsMost] = {};
int g_count = 0;
Spot g_squares[kSquaresMost] = {}, g_icons[kIconsMost] = {}; // the Equipment window's squares and icons this frame
int g_square_count = 0, g_icon_count = 0;
DWORD g_when = 0; // GetTickCount as an item list (or the Equipment window) was last drawn

text::SpriteCache<256> g_sprites; // the names' and stats' pictures, each made once and kept while it's drawn (textcache.hpp)

bool read_rect(const void* window, short (&rect)[4])
{
    __try {
        std::memcpy(rect, static_cast<const char*>(window) + kRect, sizeof rect);
        return rect[2] > rect[0] && rect[3] > rect[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool list_window(const char* layout)
{
    const char* name = layout + kLayoutName;
    if (std::memcmp(name, "menu    ", 8) != 0)
        return false;
    for (const char* list : kLists)
        if (std::strncmp(name + 8, list, std::strlen(list)) == 0)
            return true;
    return false;
}

bool equip_layout(const char* layout) { return std::memcmp(layout + kLayoutName, kEquipLayout, sizeof kEquipLayout) == 0; }

bool g_stats_on = false; // [client] equip.stats (set_stats)

// The stats' text is VanaCore's own: equip.stats is on and its area's True Font is (the Check window's box is in the menus' when
// the stats' own is off: equipstats_check.h)
bool stats_wanted()
{
    return g_stats_on && (chatfont::area_on(VANA_FONT_EQUIP_STATS) || chatfont::area_on(VANA_FONT_MENUS));
}

// Window `window`'s texts of an earlier draw (it's drawn again): let go
void forget(const void* window)
{
    int n = 0;
    for (int i = 0; i < g_count; ++i)
        if (g_texts[i].window != window)
            g_texts[n++] = g_texts[i];
    g_count = n;
}

// `spots`' (`count` of them) of window `window`'s: let go
void forget_in(Spot* spots, int& count, const void* window)
{
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (spots[i].window != window)
            spots[n++] = spots[i];
    count = n;
}

// The same for `window`'s squares and icons
void forget_spots(const void* window)
{
    forget_in(g_squares, g_square_count, window);
    forget_in(g_icons, g_icon_count, window);
}

// A spot of window `window` (placed `index`) at x, y, noted once
void note(Spot* spots, int& count, const void* window, int index, int x, int y)
{
    for (int i = 0; i < count; ++i)
        if (spots[i].window == window && spots[i].x == x && spots[i].y == y)
            return;
    if (count < kSquaresMost)
        spots[count++] = {window, index, x, y, 0};
}

bool to_box(const short (&rect)[4], unsigned width, unsigned height, Box& box)
{
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f, scale = 0.0f;
    if (!uispace::to_pixels(static_cast<float>(rect[0]), static_cast<float>(rect[1]), width, height, x0, y0, scale) ||
        !uispace::to_pixels(static_cast<float>(rect[2]), static_cast<float>(rect[3]), width, height, x1, y1, scale))
        return false;
    box = {static_cast<int>(std::lround(x0)), static_cast<int>(std::lround(y0)), static_cast<int>(std::lround(x1)),
           static_cast<int>(std::lround(y1))};
    return true;
}

bool overlaps(const Box& a, const Box& b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

// `pieces` less what `hole` covers: each piece it touches becomes the bands of it that are left (above, below, left and right)
void cut(Pieces& pieces, const Box& hole)
{
    Pieces left;
    for (int i = 0; i < pieces.count; ++i) {
        const Box& q = pieces.box[i];
        if (!overlaps(q, hole)) {
            left.add(q);
            continue;
        }
        const int top = std::max(q.top, hole.top), bottom = std::min(q.bottom, hole.bottom);
        left.add({q.left, q.top, q.right, hole.top});
        left.add({q.left, hole.bottom, q.right, q.bottom});
        left.add({q.left, top, hole.left, bottom});
        left.add({hole.right, top, q.right, bottom});
    }
    pieces = left;
}

// `area`'s (the items' or the Equipment slots') True Font as built, with its outline in the same picture. False while it's off (or
// its letters left out: size 0)
bool look_of(int area, Look& look)
{
    text::Style stroked;
    float opacity = 0.0f;
    if (!chatfont::area_style(area, look.style, stroked, look.upper, opacity))
        return false;
    look.style.stroke = stroked.stroke, look.style.exact_stroke = stroked.exact_stroke;
    look.style.stroke_color[3] = stroked.stroke > 0 ? opacity : 0.0f;
    look.style.background[3] = 0.0f;
    return true;
}

// How big a text is in VanaCore's own layer (the window's pixels, graphics.hpp own_pixels) against the sizes True Font's areas are
// built at (the game's frame's pixels, chatfont.hpp: the screen's pixels per interface pixel): the window's pixels per interface
// unit over those. 1 where the window is the frame's size; with the frame drawn larger than the window ([ffxi] resolution.ui or
// the scene 3840 x 2160 on a 2560 wide window) it's under 1, and a text sized by the area came out that much too large (reported
// 2026-11-14: the Equipment stats 1.4 times what they were as the game's letters)
float layer_ratio(unsigned width, unsigned height)
{
    float sx = 0.0f, sy = 0.0f, px = 0.0f, py = 0.0f, scale = 0.0f;
    if (!vanacore_gfx_chat_scale(&sx, &sy) || !(sy > 0.0f) || !uispace::to_pixels(0.0f, 0.0f, width, height, px, py, scale) ||
        !(scale > 0.0f))
        return 1.0f;
    const float ratio = scale / sy;
    return std::fabs(ratio - 1.0f) < 0.03f ? 1.0f : std::clamp(ratio, 0.25f, 4.0f);
}

// `look` for a layer `ratio` the frame's size: its size and its outline that much
void fit_look(Look& look, float ratio)
{
    if (ratio == 1.0f)
        return;
    look.style.size = std::max(1, static_cast<int>(std::lround(static_cast<float>(look.style.size) * ratio)));
    look.style.exact_stroke *= ratio;
    look.style.stroke = static_cast<int>(std::ceil(look.style.exact_stroke));
}

// The Looks of the areas this frame's texts are in (the items', the stats'), each built once a frame at `ratio` (layer_ratio); none
// for an area that's off
struct Looks {
    static constexpr int kMost = 4;
    int area[kMost] = {};
    bool ok[kMost] = {};
    Look look[kMost];
    int count = 0;
    float ratio = 1.0f;
    const Look* of(int wanted_area)
    {
        for (int i = 0; i < count; ++i)
            if (area[i] == wanted_area)
                return ok[i] ? &look[i] : nullptr;
        if (count == kMost)
            return nullptr;
        const int at = count++;
        area[at] = wanted_area;
        ok[at] = look_of(wanted_area, look[at]);
        if (ok[at])
            fit_look(look[at], ratio);
        return ok[at] ? &look[at] : nullptr;
    }
};

// The game's colour (80h a channel full) as the text's own, 0 to 255
void set_colour(text::Style& style, std::uint32_t colour)
{
    for (int i = 0; i < 3; ++i)
        style.color[i] = std::min(255.0f, static_cast<float>(colour >> (16 - 8 * i) & 0xFF) * 2.0f);
    style.color[3] = 1.0f;
}

// What of box `own` (a text's picture) is left inside text `t`'s list's rectangle after the windows drawn after the list
Pieces visible(const Box& own, const Text& t, const Frame& frame)
{
    const Box& list = frame.boxes[t.index];
    Pieces pieces;
    if (t.own)
        pieces.add(own);
    else
        pieces.add({std::max(own.left, list.left), std::max(own.top, list.top), std::min(own.right, list.right),
                    std::min(own.bottom, list.bottom)});
    for (int w = t.index + 1; w < frame.windows && pieces.count > 0; ++w)
        if (frame.known[w])
            cut(pieces, frame.boxes[w]);
    return pieces;
}

// One text from its pen's start on its baseline (a slot name: its top left at its place), as see-through as the game drew it, the
// parts of it no window drawn later covers
void draw_text(void* device, unsigned width, unsigned height, const Look& look, const Text& t, const Frame& frame)
{
    text::Style style = look.style;
    set_colour(style, t.colour);
    std::wstring shown(t.text, t.text + std::strlen(t.text));
    if (look.upper)
        for (wchar_t& c : shown)
            c = static_cast<wchar_t>(std::towupper(c));
    text::Sprite& sprite = g_sprites.get(shown, style);
    float px = 0.0f, py = 0.0f, scale = 0.0f;
    if (!frame.known[t.index] ||
        !uispace::to_pixels(static_cast<float>(t.x), static_cast<float>(t.base), width, height, px, py, scale) || !sprite.upload(device))
        return;
    const int x = static_cast<int>(std::lround(px)) - sprite.pen_left() - (t.right ? sprite.advance() : 0);
    const int y = static_cast<int>(std::lround(py)) - (t.corner ? 0 : sprite.baseline());
    const Box own = {x, y, x + sprite.width(), y + sprite.height()};
    const Pieces pieces = visible(own, t, frame);
    const float opacity = std::min(1.0f, static_cast<float>(t.colour >> 24) / 128.0f);
    for (int i = 0; i < pieces.count; ++i) {
        const text::Crop crop = {pieces.box[i].left, pieces.box[i].top, pieces.box[i].right, pieces.box[i].bottom};
        sprite.draw(device, x, y, opacity, 1.0f, text::Wave{}, &crop);
    }
}

// The Equipment window's squares of the first one's window, row by row and left to right (a row's squares share a top, to within
// kRowGap): how many
int ordered_squares(Spot (&out)[kSquaresMost])
{
    int n = 0;
    for (int i = 0; i < g_square_count; ++i)
        if (g_squares[i].window == g_squares[0].window)
            out[n++] = g_squares[i];
    std::sort(out, out + n, [](const Spot& a, const Spot& b) { return a.y < b.y; });
    for (int i = 1, row = 0; i < n; ++i) {
        if (out[i].y - out[i - 1].y > kRowGap)
            ++row;
        out[i].row = row;
    }
    std::sort(out, out + n, [](const Spot& a, const Spot& b) { return a.row != b.row ? a.row < b.row : a.x < b.x; });
    return n;
}

// An item's icon was drawn over the middle of this square: the game's name was under it, so ours isn't drawn
bool covered(const Spot& square)
{
    const int mx = square.x + kSquareSide / 2, my = square.y + kSquareSide / 2;
    for (int i = 0; i < g_icon_count; ++i) {
        const Spot& icon = g_icons[i];
        if (icon.window == square.window && mx >= icon.x - kIconSlack && mx < icon.x + kSquareSide + kIconSlack &&
            my >= icon.y - kIconSlack && my < icon.y + kSquareSide + kIconSlack)
            return true;
    }
    return false;
}

// Each of the Equipment window's 16 slots' name, in the squares the game drew this frame that no icon covers. Nothing unless all
// 16 squares are there in four rows: the names can't be told apart otherwise
void draw_slots(void* device, unsigned width, unsigned height, const Look& look, const Frame& frame)
{
    Spot squares[kSquaresMost] = {};
    const int n = g_square_count > 0 ? ordered_squares(squares) : 0;
    if (n != kSlots || squares[n - 1].row != kSlots / kPerRow - 1)
        return;
    static bool said = false;
    if (!said && g_icon_count > 0) { // (once: where the first square and icon were, if a name was drawn over an icon it's seen here)
        said = true;
        log::write(L"Equipment slot names: %d icons, the first at %d,%d; the squares' first at %d,%d, the last at %d,%d", g_icon_count,
                   g_icons[0].x, g_icons[0].y, squares[0].x, squares[0].y, squares[n - 1].x, squares[n - 1].y);
    }
    for (int i = 0; i < n; ++i) {
        if (covered(squares[i]))
            continue;
        Text t = {};
        t.window = squares[i].window, t.index = squares[i].index, t.x = squares[i].x + kPadLeft, t.base = squares[i].y + kPadTop;
        t.colour = kSlotColour, t.corner = true, t.area = VANA_FONT_EQUIP;
        const std::size_t length = std::strlen(kSlotNames[i]);
        std::memcpy(t.text, kSlotNames[i], length < kLetters - 1 ? length : kLetters - 1);
        draw_text(device, width, height, look, t, frame);
    }
}

} // namespace

bool wanted()
{
    return chatfont::area_on(VANA_FONT_ITEMS);
}

bool equip_wanted()
{
    return chatfont::area_on(VANA_FONT_EQUIP);
}

Window::Window(const void* window, const char* layout)
    : was_(g_now), was_equip_(g_equip_now), was_cur_(g_cur), index_(-1), window_(window)
{
    g_now = g_equip_now = g_cur = -1;
    short rect[4];
    const bool items = wanted(), slots = equip_wanted(), stats = stats_wanted();
    if (window == nullptr || !(items || slots || stats) || gameui::veiled(window) || !read_rect(window, rect))
        return;
    if (g_window_count == kWindowsMost) // (the interface hidden: nothing draws, so nothing takes them)
        g_window_count = 0, g_count = 0, g_square_count = 0, g_icon_count = 0;
    Placed& placed = g_windows[g_window_count];
    placed.window = window;
    std::memcpy(placed.rect, rect, sizeof rect);
    index_ = g_window_count++;
    g_cur = index_;
    if (stats)
        forget(window); // (its stats of an earlier draw)
    if (layout == nullptr)
        return;
    if (items && list_window(layout)) {
        g_now = index_;
        forget(window);
        g_when = GetTickCount();
    }
    if (slots && equip_layout(layout)) {
        g_equip_now = index_;
        forget_spots(window);
        g_when = GetTickCount();
    }
}

bool in_equip()
{
    return g_equip_now >= 0;
}

void equip_square(int x, int y)
{
    if (g_equip_now >= 0)
        note(g_squares, g_square_count, g_windows[g_equip_now].window, g_equip_now, x, y);
}

// (The Equipment window's icon draw passes its place as two shorts read from one place on its stack, so x and y come with the next
// short in their upper half: only the lower 16 bits are the place, as the game's own draw takes them. Seen at the icon draw
// 2026-11-13: x 0x008E0142, y 0x0162008E for the place 322, 142.)
void equip_icon(int x, int y)
{
    if (g_equip_now >= 0)
        note(g_icons, g_icon_count, g_windows[g_equip_now].window, g_equip_now, static_cast<short>(x), static_cast<short>(y));
}

Window::~Window()
{
    g_now = was_;
    g_equip_now = was_equip_;
    g_cur = was_cur_;
    short rect[4];
    // (the rectangle as the draw left it: a window the game places as it draws is laid out inside its draw)
    if (index_ >= 0 && index_ < g_window_count && g_windows[index_].window == window_ && read_rect(window_, rect))
        std::memcpy(g_windows[index_].rect, rect, sizeof rect);
}

bool keep(int x, int y, const char* text, std::uint32_t colour)
{
    if (g_now < 0 || text == nullptr || g_count == kTextsMost)
        return false;
    Text& t = g_texts[g_count];
    std::size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        const auto c = static_cast<unsigned char>(text[n]);
        if (c < 0x20 || c > 0x7E || n + 1 >= kLetters)
            return false; // (past ASCII, or longer than a name: the game's own letters)
        t.text[n] = static_cast<char>(c);
    }
    if (n == 0)
        return false;
    t.text[n] = '\0';
    t.window = g_windows[g_now].window, t.index = g_now, t.x = x, t.base = y + kCellBaseline, t.colour = colour;
    t.corner = false, t.area = VANA_FONT_ITEMS, t.right = false, t.own = false;
    ++g_count;
    return true;
}

bool keep_stat(int area, int x, int y, const char* text, bool right)
{
    if (g_cur < 0 || g_cur >= g_window_count || text == nullptr || g_count == kTextsMost || !chatfont::area_on(area))
        return false;
    Text& t = g_texts[g_count];
    std::size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        const auto c = static_cast<unsigned char>(text[n]);
        if (c < 0x20 || c > 0x7E || n + 1 >= kLetters)
            return false; // (past ASCII, or longer than a stat's line: the game's own letters)
        t.text[n] = static_cast<char>(c);
    }
    if (n == 0)
        return false;
    t.text[n] = '\0';
    t.window = g_windows[g_cur].window, t.index = g_cur, t.x = x, t.base = y + kCellBaseline, t.colour = kStatColour;
    t.corner = false, t.area = area, t.right = right, t.own = true;
    ++g_count;
    g_when = GetTickCount();
    return true;
}

void set_stats(bool on)
{
    g_stats_on = on;
}

void draw(void* device, unsigned width, unsigned height)
{
    const int count = g_count, windows = g_window_count;
    g_count = 0, g_window_count = 0;
    Look slots;
    const bool equip = g_square_count > 0 && look_of(VANA_FONT_EQUIP, slots);
    if ((count > 0 || equip) && GetTickCount() - g_when <= kStale) {
        Frame frame;
        frame.windows = windows;
        for (int w = 0; w < windows; ++w)
            frame.known[w] = to_box(g_windows[w].rect, width, height, frame.boxes[w]);
        g_sprites.begin(device);
        Looks looks;
        looks.ratio = layer_ratio(width, height);
        for (int i = 0; i < count; ++i)
            if (const Look* look = looks.of(g_texts[i].area))
                draw_text(device, width, height, *look, g_texts[i], frame);
        if (equip)
            draw_slots(device, width, height, slots, frame);
    }
    g_square_count = 0, g_icon_count = 0;
}

} // namespace vanacore::listtext
