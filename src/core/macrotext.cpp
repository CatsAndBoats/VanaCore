#include "macrotext.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "textcache.hpp"
#include "uispace.hpp"

namespace vanacore::macrotext {
namespace {

// The macro palette's own draw (its handler's slot 4, FUN_101A2280, inside the window's draw; read live 2026-11-12) prints each
// of its ten macros' names at its key's place (GetButtonLocate of key i + 1: the key's left; its top + 4 or + 2):
//   a name of letters and digits alone (FUN_101B7FA0) by the keytop printer (FUN_101B7EC0: chatfont.cpp's hook), the room the
//   key's width or 38;
//   any other by the fitted text draw (FUN_1018F160, __cdecl: x, y, the text, its colour, the room, plain), which prints the
//   font sheet's text from x, its cells' top at y, cut to the letters that fit the room less 14 (then "..." unless `plain`:
//   the palette's is), the room the key's width or 48. Six other callers (lists' names). sub esp,41Ch; or ecx,-1; xor eax,eax;
//   push ebx; push ebp; mov ebp,[esp+430h]; push esi; push edi; mov edi,ebp; repne scasb
constexpr char kFitted[] = "81 EC 1C 04 00 00 83 C9 FF 33 C0 53 55 8B AC 24 30 04 00 00 56 57 8B FD F2 AE";
constexpr char kFittedStart[] = {'\x81', '\xEC', '\x1C', '\x04', '\x00', '\x00'};
constexpr int kFittedMargin = 14; // the room it leaves free
constexpr int kCellBaseline = 14; // the menu font's 16 x 16 cell: its capitals stand on row 14
using Fitted = int(__cdecl*)(int x, int y, const char* text, std::uint32_t colour, int room, int plain);
Fitted g_fitted = nullptr;

// A key is one of the window's buttons, a shape of the "menu    keytops3" set (its 120 to 129 Ctrl1 to Ctrl0, 130 to 139 Alt1
// to Alt0, 40 x 32; 474 to 493 the wider palette's; read live 2026-11-12): the key's square ("menu    gauge"), then "Ctrl"
// or "Alt", a "menu    menu2fon" quad a letter (from x 7 or 12, standing on y 30), then the number, one "menu    keytop"
// quad (x 31, standing on 30 too; 1 to 9 from u 136 of v 96, 12 apart, 0 at 3, 107). The window's draw (FUN_101173D0) draws
// it as any button, by the scaled shape draw: 80808080h plain, 80805020h the cursor's (gold), 30808080h greyed.
// The shape (buttons.cpp's): its quads' list (+4: a node's next at +0, its quad at +10h) and their count (+14h, read as the
// draw starts); a quad's texture's name (+32h), its first corner's texture place (+64h, +68h: fractions of the sheet) and its
// own corners in the shape's units (x shorts at +C0h, y at +C8h).
constexpr std::ptrdiff_t kList = 4, kCount = 0x14, kNodeQuad = 0x10, kName = 0x32, kU = 0x64, kV = 0x68, kOwnX = 0xC0, kOwnY = 0xC8;
constexpr char kLetterSheet[] = "menu    menu2fon", kDigitSheet[] = "menu    keytop  ";
constexpr std::size_t kNameSize = 16;
constexpr int kQuadsMost = 16;
constexpr int kDigitU = 136, kDigitV = 96, kDigitStep = 12, kZeroU = 3, kZeroV = 107, kNear = 2;
constexpr float kSpaceGap = 1.5f; // the number further than this from the letters: a space between them (the game's "Alt 1")

constexpr std::size_t kLetters = 20; // a text kept: a macro's name is 8 letters, a key's label 6
struct Text {
    int area;             // VANA_FONT_MACRO_NAMES (a macro's name) or VANA_FONT_MACROBAR (a key's label)
    int x, base;          // its pen's start and its baseline, the interface's units
    std::uint32_t colour; // the game's draw's (A R G B, 80h full)
    char text[kLetters];
};
constexpr int kTextsMost = 96;
Text g_texts[kTextsMost] = {}; // this frame's, kept as the game drew its palettes (draw takes them)
int g_count = 0;
DWORD g_when = 0;             // GetTickCount as a palette's draw began
// The palettes' layouts drawn since the texts were last taken: two can be up in a frame (the one going beside the one coming,
// macrobar.cpp; a Ctrl or Alt palette left up), and each keeps its texts, the way the game draws each of them
constexpr int kPalettesMost = 4;
const char* g_drawn[kPalettesMost] = {};
int g_drawn_count = 0;
constexpr DWORD kStale = 250; // texts older than this were never drawn (the interface hidden): dropped

bool g_palette = false;     // a macro palette's window is being drawn
std::uint32_t g_colour = 0; // macrobar.font.color, 0xFFRRGGBB; 0 the game's
bool g_bare = false;        // macrobar.prefix=0

text::SpriteCache<64> g_sprites; // the texts' pictures, each made once and kept while it's drawn (textcache.hpp)

template <typename T> T at(const void* base, std::ptrdiff_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
    return value;
}

// The bar's palettes ("menu    mcr1pall", "mcr2pall", the long bar's "mcr1long", "mcr2long") and the rows Macros > Edit shows
// in their place ("mcr1edit", "mcr2edit", the long bar's "mcr1edlo", "mcr2edlo"), whose keys and names are the palettes': left
// to the game's letters there, the menu cursor beside a key gave some of its name back to the game's font ("SOLOPLAY" read
// "SOL OPL A", seen 2026-11-14)
bool palette_name(const char* name)
{
    if (std::memcmp(name, "menu    mcr", 11) != 0 || (name[11] != '1' && name[11] != '2'))
        return false;
    for (const char* kind : {"pall", "long", "edit", "edlo"})
        if (std::memcmp(name + 12, kind, 4) == 0)
            return true;
    return false;
}

// A key's label, as its shape lays it
struct Label {
    int cut = 0;        // the shape's quads before its label's first
    int letters = 0;    // its "menu2fon" quads: 4 "Ctrl", 3 "Alt"
    int left = 0;       // its first letter's left, the shape's units
    int right = 0;      // its last letter's right
    int base = 0;       // what its letters and number stand on
    int digit_left = 0; // its number's left
    char digit = 0;     // '0' to '9'
};

// The number a "keytop" quad shows, by its place on the sheet; 0 none of them
char digit_of(const char* quad)
{
    const int u = static_cast<int>(std::lround(at<float>(quad, kU) * 256.0f));
    const int v = static_cast<int>(std::lround(at<float>(quad, kV) * 256.0f));
    if (std::abs(u - kZeroU) <= kNear && std::abs(v - kZeroV) <= kNear)
        return '0';
    for (int n = 1; n <= 9; ++n)
        if (std::abs(u - (kDigitU + kDigitStep * (n - 1))) <= kNear && std::abs(v - kDigitV) <= kNear)
            return static_cast<char>('0' + n);
    return 0;
}

// `shape`'s label, if it's a key's: something drawn first (the square), then nothing but 3 or 4 letters and one number
bool label_of(const char* shape, Label& label)
{
    __try {
        const int count = at<int>(shape, kCount);
        const char* node = at<const char*>(shape, kList);
        bool any = false;
        if (count < 2 || count > kQuadsMost)
            return false;
        for (int i = 0; i < count; ++i, node = at<const char*>(node, 0)) {
            const char* quad = node != nullptr ? at<const char*>(node, kNodeQuad) : nullptr;
            if (quad == nullptr)
                return false;
            const bool letter = std::memcmp(quad + kName, kLetterSheet, kNameSize) == 0;
            const bool digit = std::memcmp(quad + kName, kDigitSheet, kNameSize) == 0;
            if (!letter && !digit) {
                if (any)
                    return false;
                continue;
            }
            if (label.digit != 0) // (its number is its last)
                return false;
            const auto* xs = reinterpret_cast<const short*>(quad + kOwnX);
            const auto* ys = reinterpret_cast<const short*>(quad + kOwnY);
            const int left = std::min<int>(std::min(xs[0], xs[1]), std::min(xs[2], xs[3]));
            const int right = std::max<int>(std::max(xs[0], xs[1]), std::max(xs[2], xs[3]));
            const int bottom = std::max<int>(std::max(ys[0], ys[1]), std::max(ys[2], ys[3]));
            if (!any)
                label.cut = i, label.left = left, label.right = left, label.base = bottom;
            any = true;
            label.left = std::min(label.left, left), label.base = std::max(label.base, bottom);
            if (letter) {
                ++label.letters;
                label.right = std::max(label.right, right);
            } else if ((label.digit = digit_of(quad)) == 0) {
                return false;
            } else {
                label.digit_left = left;
            }
        }
        return label.digit != 0 && label.cut > 0 && (label.letters == 3 || label.letters == 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void keep(int area, int x, int base, std::uint32_t colour, const char* text)
{
    if (g_count == kTextsMost || text[0] == '\0')
        return;
    Text& t = g_texts[g_count++];
    t.area = area, t.x = x, t.base = base, t.colour = colour;
    strncpy_s(t.text, text, _TRUNCATE);
}

// A text's look: its area's True Font as built with its outline in the same picture, in the game's draw's colour (80h a
// channel full) or, a key's label, macrobar.font.color. False while the area's is off (or its letters left out: size 0)
bool style_of(const Text& t, text::Style& style, bool& upper)
{
    text::Style letters, stroked;
    float opacity = 0.0f;
    if (!chatfont::area_style(t.area, letters, stroked, upper, opacity))
        return false;
    const std::uint32_t rgb = t.area == VANA_FONT_MACROBAR && g_colour != 0 ? g_colour : 0;
    style = letters;
    for (int i = 0; i < 3; ++i) {
        const int shift = 16 - 8 * i;
        style.color[i] = rgb != 0 ? static_cast<float>((rgb >> shift) & 0xFF)
                                  : std::min(255.0f, static_cast<float>((t.colour >> shift) & 0xFF) * 2.0f);
    }
    style.color[3] = 1.0f;
    style.stroke = stroked.stroke, style.exact_stroke = stroked.exact_stroke;
    style.stroke_color[3] = stroked.stroke > 0 ? opacity : 0.0f;
    style.background[3] = 0.0f;
    return true;
}

// One text from its pen's start on its baseline, as see-through as the game drew it (a greyed key's label)
void draw_text(void* device, unsigned width, unsigned height, const Text& t)
{
    text::Style style;
    bool upper = false;
    if (!style_of(t, style, upper))
        return;
    std::wstring shown(t.text, t.text + std::strlen(t.text));
    if (upper)
        for (wchar_t& c : shown)
            c = static_cast<wchar_t>(std::towupper(c));
    text::Sprite& sprite = g_sprites.get(shown, style);
    float px = 0.0f, py = 0.0f, scale = 0.0f;
    if (!uispace::to_pixels(static_cast<float>(t.x), static_cast<float>(t.base), width, height, px, py, scale) ||
        !sprite.upload(device))
        return;
    const float opacity = std::min(1.0f, static_cast<float>(t.colour >> 24) / 128.0f);
    sprite.draw(device, static_cast<int>(std::lround(px)) - sprite.pen_left(),
                static_cast<int>(std::lround(py)) - sprite.baseline(), opacity);
}

// The game's fitted text draw: a palette's macro's name kept instead (a name with a space or a symbol in it)
int __cdecl fitted_hook(int x, int y, const char* text, std::uint32_t colour, int room, int plain)
{
    if (g_palette && name(x, y + kCellBaseline, text, colour, room - kFittedMargin))
        return 0; // (the palette reads nothing back)
    return g_fitted(x, y, text, colour, room, plain);
}

} // namespace

void look(std::uint32_t colour, bool bare)
{
    g_colour = colour, g_bare = bare;
}

void install(HMODULE game)
{
    const std::uintptr_t fitted = scan::find(game, kFitted);
    if (fitted == 0 || std::memcmp(reinterpret_cast<const void*>(fitted), kFittedStart, sizeof kFittedStart) != 0 ||
        !imports::detour(reinterpret_cast<Fitted>(fitted), sizeof kFittedStart, &fitted_hook, g_fitted))
        log::write(L"Macro bar: the game's fitted text draw wasn't found in FFXiMain.dll: a macro's name with a space or a symbol "
                   L"in it keeps True Font's letters laid over the game's, the rest are drawn whole");
}

Window::Window(const char* layout) : was_(g_palette)
{
    g_palette = layout != nullptr && palette_name(layout + 0x46);
    if (!g_palette)
        return;
    // This frame's texts start with its first palette: a palette drawn again (no draw between them: the interface hidden) is a
    // new frame's. Reset by each palette, the one drawn last kept only its own and a bar whose Ctrl or Alt palette lingered
    // beside it was left with its keys empty (reported 2026-11-13: labels and names gone until the bar was opened again)
    bool again = g_drawn_count == kPalettesMost;
    for (int i = 0; i < g_drawn_count && !again; ++i)
        again = g_drawn[i] == layout;
    if (again)
        g_count = 0, g_drawn_count = 0;
    g_drawn[g_drawn_count++] = layout;
    g_when = GetTickCount();
}

Window::~Window()
{
    g_palette = was_;
}

Key::Key(void* shape, int x, int y, float sx, float sy, std::uint32_t colour) : count_(nullptr), all_(0)
{
    Label label;
    if (!g_palette || shape == nullptr || !chatfont::area_on(VANA_FONT_MACROBAR) || !label_of(static_cast<const char*>(shape), label))
        return;
    count_ = reinterpret_cast<int*>(static_cast<char*>(shape) + kCount);
    all_ = *count_;
    *count_ = label.cut;
    if (std::fabs(sx - 1.0f) > 0.001f || std::fabs(sy - 1.0f) > 0.001f)
        return; // (a picked key's flash, its copies growing and fading: the square alone)
    char text[kLetters] = {};
    const bool space = static_cast<float>(label.digit_left - label.right) > kSpaceGap;
    if (g_bare)
        sprintf_s(text, "%c", label.digit);
    else
        sprintf_s(text, "%s%s%c", label.letters == 4 ? "Ctrl" : "Alt", space ? " " : "", label.digit);
    keep(VANA_FONT_MACROBAR, x + (g_bare ? label.digit_left : label.left), y + label.base, colour, text);
}

Key::~Key()
{
    if (count_ != nullptr)
        *count_ = all_;
}

bool name(int x, int baseline, const char* text, std::uint32_t colour, int room)
{
    if (!g_palette || text == nullptr || !chatfont::area_on(VANA_FONT_MACRO_NAMES))
        return false;
    char shown[kLetters] = {};
    std::size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        const auto c = static_cast<unsigned char>(text[n]);
        if (c < 0x20 || c > 0x7E || n + 1 >= kLetters)
            return false; // (past ASCII: the game's own letters)
        shown[n] = static_cast<char>(c);
    }
    text::Style letters, stroked;
    bool upper = false;
    float opacity = 0.0f;
    if (!chatfont::area_style(VANA_FONT_MACRO_NAMES, letters, stroked, upper, opacity))
        return true; // (its letters left out: the names' size 0)
    for (std::size_t i = 0; upper && i < n; ++i)
        shown[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(shown[i])));
    // the letters that fit, as the game's printers stop before one that would pass the room
    while (n > 0 && chatfont::text_width(VANA_FONT_MACRO_NAMES, shown) > static_cast<float>(room))
        shown[--n] = '\0';
    keep(VANA_FONT_MACRO_NAMES, x, baseline, colour, shown);
    return true;
}

void draw(void* device, unsigned width, unsigned height)
{
    const int count = g_count;
    g_count = 0, g_drawn_count = 0;
    if (count == 0 || GetTickCount() - g_when > kStale)
        return;
    g_sprites.begin(device);
    for (int i = 0; i < count; ++i)
        draw_text(device, width, height, g_texts[i]);
}

} // namespace vanacore::macrotext
