#include "chatline.hpp"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "gametext.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "text.hpp"

namespace vanacore::chatline {
namespace {

// FsTextInputDisplay's line draw (0x101B6DC0, __thiscall, ret 18h; its one caller FsTextInputDisplay::OnDrawPrimitive): the
// line's display text (the chat manager's GetDisplayText, phraselist.cpp) and its length, the pen's x and y, the room it has,
// and whether the caret shows (the caller's count of its calls: shown from the 10th to the 24th of every 25, so it blinks by
// frames). The text is scrolled to the room (0x1018ED40), then drawn by the text draw below a piece at a time: with the caret
// shown, the text before it with a '|' on its end (the call at kCaretCall), then the rest from where that text ends by the
// game's widths, the '|' not counted; a 7F code past FF and FE (a colour: an auto-translate phrase's) ends a piece too. Read
// live 2026-11-07. mov eax,1C44h; call __chkstk; mov eax,[esp+1C4Ch]; push ebx; push ebp; mov ebx,ecx; push esi; cmp eax,168h;
// push edi
constexpr char kDisplay[] = "B8 44 1C 00 00 E8 ?? ?? ?? ?? 8B 84 24 4C 1C 00 00 53 55 8B D9 56 3D 68 01 00 00 57";
constexpr std::ptrdiff_t kCaretCall = 0x129;
using Display = int(__fastcall*)(void* self, void* edx, const char* text, int length, int x, int y, int room, int caret);
Display g_display = nullptr;
// The text boxes' line draw (0x101C4840, __thiscall, ret 18h), FsTextInputDisplay's twin: the same arguments, the same pieces
// (the text before the caret with a '|' on its end by the call at kBoxCaretCall, the rest from where that ends by the game's
// widths). The macro editor's line being typed into is one (its one call a frame, from 0x101A4428, the caret argument always
// 1: read live 2026-11-09), the text boxes' another (0x10210610: the caret theirs while typed into). Its letters were the
// window's font, laid a word at a time: past the caret the rest stood apart from it, a gap (reported 2026-11-09). sub esp,
// 0C5Ch; push ebx; push ebp; push esi; push edi; mov edi,[esp+0C74h]; mov ebx,ecx; cmp edi,41Ah
constexpr char kBox[] = "81 EC 5C 0C 00 00 53 55 56 57 8B BC 24 74 0C 00 00 8B D9 81 FF 1A 04 00 00";
constexpr std::ptrdiff_t kBoxCaretCall = 0x10E;
Display g_box = nullptr;
// The game's text draw (0x1018F2A0, __cdecl: x, y, the text, its colour): the font sheet's letters from pen x, y (the cell's
// top-left, the interface's pixels), by the menu font's string print (0x1011BC80) at 1 x 1. mov eax,[esp+10h]; mov ecx,
// [esp+0Ch]; mov edx,[esp+8]; push 3F800000h; push 3F800000h; push 44h; push eax
constexpr char kText[] = "8B 44 24 10 8B 4C 24 0C 8B 54 24 08 68 00 00 80 3F 68 00 00 80 3F 6A 44 50";
using Text = int(__cdecl*)(int x, int y, const char* text, std::uint32_t colour);
Text g_text = nullptr;
std::uintptr_t g_caret_return = 0, g_box_return = 0; // where the text draw of the text before the caret returns to, in each
std::uintptr_t g_caret_now = 0;                      // ... in the line being drawn

// A piece of the line the game drew (the text draw's): where, what, and whether it's the text before the caret with its '|'
struct Piece {
    int x = 0, y = 0;
    std::string text;
    bool caret = false;
};
bool g_drawing = false; // inside the line's draw, in True Font's way
std::vector<Piece> g_pieces;

// The picture made last, what it's of, and the caret's blink
struct Made {
    std::wstring text;
    text::Style letters, stroked;
    text::Sprite fill, outline, measure, bar;
    std::vector<std::uint32_t> pixels;
    VanaLinePicture pic{};
    std::uint32_t serial = 0;
    std::wstring blink_text;
    std::size_t blink_caret = 0;
    ULONGLONG changed = 0;
    bool told = false;
} g;

// The display text has nothing but letters in it, the caret's 7F FF and the 7F FE the game passes over: no colour's 7F code
// (an auto-translate phrase, the list's offer), no icon (EF xx: a phrase's braces), no control byte. Any of those leaves the line
// to the letters, each where its draw puts it.
bool plain(const unsigned char* text, int length)
{
    for (int i = 0; i < length && text[i] != 0; ++i) {
        const unsigned b = text[i];
        if (b == 0x7F) {
            if (i + 1 >= length || (text[i + 1] != 0xFF && text[i + 1] != 0xFE))
                return false;
            ++i;
        } else if (b < 0x20 || b == 0x80 || b == 0xA0 || b == 0xEF || b >= 0xFD) {
            return false;
        } else if ((b >= 0x81 && b <= 0x9F) || b >= 0xE0) { // (a two-byte letter: the 85 row's ñ, é, kana, kanji; E0-FC, FD up left above)
            if (i + 1 >= length || text[i + 1] == 0)
                return false;
            ++i;
        }
    }
    return true;
}

std::uintptr_t call_target(std::uintptr_t call) { return call + 5 + *reinterpret_cast<const std::int32_t*>(call + 1); }

// `sprite`'s ink, premultiplied white (every channel its alpha), into `pixels` (`width` across) from row `top`
void blit(std::vector<std::uint32_t>& pixels, int width, const text::Sprite& sprite, int top)
{
    const std::vector<std::uint32_t>& from = sprite.pixels();
    for (int y = 0; y < sprite.height(); ++y)
        for (int x = 0; x < sprite.width(); ++x) {
            const std::uint32_t a = from[static_cast<std::size_t>(y) * sprite.width() + x] >> 24;
            pixels[static_cast<std::size_t>(top + y) * width + x] = a << 24 | a << 16 | a << 8 | a;
        }
}

// The caret's top and bottom from the baseline: as tall as the font's own "|" is drawn, not the whole line's height (the
// letters' box, `fh` tall, less its padding of 1 each end: it reached the chat line's top and bottom edges, 2026-11-07),
// which it's left at when the font draws no "|"
void caret_rows(const text::Style& letters, int fh)
{
    VanaLinePicture& p = g.pic;
    p.caret_top = 1.0f - p.base_y, p.caret_bottom = static_cast<float>(fh - 1) - p.base_y;
    g.bar.set(L"|", letters);
    const std::vector<std::uint32_t>& ink = g.bar.pixels();
    int top = -1, bottom = -1;
    for (int y = 0; y < g.bar.height(); ++y)
        for (int x = 0; x < g.bar.width(); ++x)
            if ((ink[static_cast<std::size_t>(y) * g.bar.width() + x] >> 24) > 0x40) {
                top = top < 0 ? y : top, bottom = y;
                break;
            }
    if (top >= 0)
        p.caret_top = static_cast<float>(top - g.bar.baseline()), p.caret_bottom = static_cast<float>(bottom + 1 - g.bar.baseline());
}

// The picture of `shown`: its letters, their outline under them (the stroked style's, when it has a stroke) and a white 3 x 3
// in the top-right corner, the caret's ink; as wide as the next 128 up, so typing seldom makes the texture again
void paint(const std::wstring& shown, const text::Style& letters, const text::Style& stroked)
{
    g.text = shown, g.letters = letters, g.stroked = stroked;
    g.fill.set(shown, letters);
    const bool outlined = stroked.stroke > 0;
    if (outlined)
        g.outline.set(shown, stroked);
    const int fw = g.fill.width(), fh = g.fill.height();
    const int ow = outlined ? g.outline.width() : 0, oh = outlined ? g.outline.height() : 0;
    const int width = ((std::max)(fw, ow) + 4 + 127) / 128 * 128, height = (std::max)(fh + oh, 3);
    g.pixels.assign(static_cast<std::size_t>(width) * height, 0);
    blit(g.pixels, width, g.fill, 0);
    if (outlined)
        blit(g.pixels, width, g.outline, fh);
    for (int y = 0; y < 3; ++y)
        for (int x = width - 3; x < width; ++x)
            g.pixels[static_cast<std::size_t>(y) * width + x] = 0xFFFFFFFFu;
    VanaLinePicture& p = g.pic;
    p.w = static_cast<std::uint32_t>(width), p.h = static_cast<std::uint32_t>(height), p.serial = ++g.serial;
    p.fill_w = static_cast<std::uint32_t>(fw), p.fill_h = static_cast<std::uint32_t>(fh);
    p.out_y = static_cast<std::uint32_t>(fh), p.out_w = static_cast<std::uint32_t>(ow), p.out_h = static_cast<std::uint32_t>(oh);
    p.pen_x = static_cast<float>(g.fill.pen_left()), p.base_y = static_cast<float>(g.fill.baseline());
    p.out_pen_x = outlined ? static_cast<float>(g.outline.pen_left()) : 0.0f;
    p.out_base_y = outlined ? static_cast<float>(g.outline.baseline()) : 0.0f;
    caret_rows(letters, fh);
    p.caret_w = static_cast<float>((std::max)(1L, std::lround(static_cast<float>(letters.size) / 18.0f)));
    p.caret_pad = outlined ? (stroked.exact_stroke > 0.0f ? stroked.exact_stroke : static_cast<float>(stroked.stroke)) : 0.0f;
}

// How far the caret is from the pen: the font's advance over the text before it (the screen's pixels)
float caret_at(const std::wstring& shown, std::size_t caret)
{
    if (caret == 0)
        return 0.0f;
    if (caret >= shown.size())
        return g.fill.exact_advance();
    g.measure.set(shown.substr(0, caret), g.letters);
    return g.measure.exact_advance();
}

// Whether the caret is lit: for Windows' caret blink time after the text or the caret last moved, then off and on as long
bool caret_lit(const std::wstring& shown, std::size_t caret)
{
    const ULONGLONG now = GetTickCount64();
    if (shown != g.blink_text || caret != g.blink_caret)
        g.blink_text = shown, g.blink_caret = caret, g.changed = now;
    const UINT blink = GetCaretBlinkTime();
    return blink == 0 || blink == INFINITE || (now - g.changed) / blink % 2 == 0;
}

// The pieces the game drew made one line, with the caret's place in it (npos: none came)
std::wstring joined(std::size_t& caret)
{
    std::wstring shown;
    caret = std::wstring::npos;
    for (const Piece& piece : g_pieces) {
        std::wstring part = gametext::to_wide(piece.text);
        if (piece.caret && caret == std::wstring::npos && !part.empty() && part.back() == L'|') {
            part.pop_back();
            shown += part;
            caret = shown.size();
            continue;
        }
        shown += part;
    }
    return shown;
}

// The line the game just drew, as one picture to the graphics layer (none when it drew nothing)
void show(const text::Style& letters, const text::Style& stroked, bool upper)
{
    if (g_pieces.empty()) {
        vanacore_gfx_set_input_line(nullptr);
        return;
    }
    std::size_t caret = 0;
    std::wstring shown = joined(caret);
    if (upper)
        for (wchar_t& c : shown)
            c = static_cast<wchar_t>(std::towupper(c));
    if (g.serial == 0 || shown != g.text || !(letters == g.letters) || !(stroked == g.stroked))
        paint(shown, letters, stroked);
    VanaLinePicture pic = g.pic;
    pic.pixels = g.pixels.data();
    pic.x = static_cast<float>(g_pieces.front().x), pic.y = static_cast<float>(g_pieces.front().y);
    pic.caret = caret != std::wstring::npos ? caret_at(shown, caret) : -1.0f;
    pic.caret_lit = caret != std::wstring::npos && caret_lit(shown, caret) ? 1u : 0u;
    vanacore_gfx_set_input_line(&pic);
    if (!g.told)
        g.told = true, log::write(L"True Font: the chat line is drawn as one picture in its font, with VanaCore's caret");
}

int __cdecl text_hook(int x, int y, const char* text, std::uint32_t colour)
{
    if (g_drawing && text != nullptr)
        g_pieces.push_back({x, y, text, reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_caret_now});
    return g_text(x, y, text, colour);
}

// A line draw's arguments (Display's)
struct Line {
    void* self;
    void* edx;
    const char* text;
    int length, x, y, room, caret;
};

int game_draw(Display draw, const Line& l, int caret) { return draw(l.self, l.edx, l.text, l.length, l.x, l.y, l.room, caret); }

// Line `l` drawn by `draw`, whose text draw of the text before the caret returns to `caret_return`. With True Font's input
// line, its letters are kept for one picture when the text is plain, the caret drawn with `caret`: the chat line's always 1
// (its place is read from that, lit or not by caret_lit), a text box's its own (a caret only while it's typed into: one box a
// frame, never with the chat line open). Else a text box's (`box`) letters are the input line's font each, as the chat line's
// are by its window: a macro line kept its font with a phrase in it, its caret's gap closed (d3d8_chatfont_words.h cf_caret)
int drawn(Display draw, std::uintptr_t caret_return, const Line& l, int caret, bool box)
{
    text::Style letters, stroked;
    bool upper = false;
    if (l.text == nullptr || l.length < 0 || !chatfont::input_style(letters, stroked, upper))
        return game_draw(draw, l, l.caret);
    const bool one = plain(reinterpret_cast<const unsigned char*>(l.text), l.length) && caret != 0;
    if (!one && !box)
        return game_draw(draw, l, l.caret);
    if (!one) {
        const int was = vanacore_gfx_text_area(VANA_FONT_INPUT);
        const int result = game_draw(draw, l, l.caret);
        vanacore_gfx_text_area(was);
        return result;
    }
    letters.crisp = stroked.crisp = true; // (whole pixels, as Windows' own text boxes: the letters' upright strokes sharp)
    g_pieces.clear();
    g_drawing = true, g_caret_now = caret_return;
    vanacore_gfx_input_line(1);
    const int result = game_draw(draw, l, caret);
    vanacore_gfx_input_line(0);
    g_drawing = false;
    show(letters, stroked, upper);
    return result;
}

int __fastcall display_hook(void* self, void* edx, const char* text, int length, int x, int y, int room, int caret)
{
    return drawn(g_display, g_caret_return, {self, edx, text, length, x, y, room, caret}, 1, false);
}

int __fastcall box_hook(void* self, void* edx, const char* text, int length, int x, int y, int room, int caret)
{
    return drawn(g_box, g_box_return, {self, edx, text, length, x, y, room, caret}, caret, true);
}

// The line draw at `at` whose text draw of the text before the caret is the call at `call`, calling `draw`
bool calls(std::uintptr_t at, std::ptrdiff_t call, std::uintptr_t draw)
{
    return at != 0 && draw != 0 && *reinterpret_cast<const std::uint8_t*>(at + call) == 0xE8 && call_target(at + call) == draw;
}

} // namespace

void install(HMODULE game)
{
    const std::uintptr_t display = scan::find(game, kDisplay), draw = scan::find(game, kText), box = scan::find(game, kBox);
    const bool found = calls(display, kCaretCall, draw);
    if (found)
        g_caret_return = display + kCaretCall + 5, g_pieces.reserve(8);
    if (!found || !imports::detour(reinterpret_cast<Text>(draw), 8, &text_hook, g_text) ||
        !imports::detour(reinterpret_cast<Display>(display), 5, &display_hook, g_display)) {
        log::write(L"True Font: the chat line's draw wasn't found in FFXiMain.dll: its letters are each laid where the game's "
                   L"are, with the game's caret");
        return;
    }
    if (calls(box, kBoxCaretCall, draw)) {
        g_box_return = box + kBoxCaretCall + 5;
        if (imports::detour(reinterpret_cast<Display>(box), 6, &box_hook, g_box)) // (sub esp,0C5Ch: 6 bytes)
            return;
    }
    log::write(L"True Font: the text boxes' line draw wasn't found in FFXiMain.dll: the macro editor's line is laid as its "
               L"window's letters");
}

} // namespace vanacore::chatline
