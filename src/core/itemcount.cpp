#include "itemcount.hpp"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "uispace.hpp"

namespace vanacore::itemcount {
namespace {

// YkDrawItemStack(count, size) in the PS2 client's names (FUN_101c9cf0), __cdecl: each frame a bag's window is open, the
// count of what's in the bag and its size, on the title bar's right (g_pTkTitle's window): "/" with YkDrawString, the count
// above it and the size below it right-aligned with YkDrawStringReverse in font 0 (small slanted digits), which takes a
// scale (the game gives 1).
//
// mov eax,[g_pTkTitle]; sub esp,2Ch; push ebx; xor ebx,ebx; push esi; cmp eax,ebx; push edi; jz; cmp [eax+8],ebx; jz;
// lea ecx,[esp+0Ch]; the rect zeroed; push ecx; mov ecx,[eax+8]; call GetWindowLocate
constexpr char kItemStack[] = "A1 ?? ?? ?? ?? 83 EC 2C 53 33 DB 56 3B C3 57 0F 84 ?? ?? ?? ?? 39 58 08 0F 84 ?? ?? ?? ?? "
                              "8D 4C 24 0C 66 89 5C 24 0C 66 89 5C 24 0E 66 89 5C 24 10 66 89 5C 24 12 51 8B 48 08 E8";
constexpr std::size_t kPrologue = 5;          // the mov eax,[g_pTkTitle]
constexpr std::ptrdiff_t kTitleAt = 1;        // its operand
constexpr std::ptrdiff_t kLocateCall = 0x3A;  // call GetWindowLocate
constexpr std::ptrdiff_t kReverseCall = 0xCF; // call YkDrawStringReverse (the count's)
constexpr std::ptrdiff_t kFontCall = 0x16;    // YkDrawStringReverse: push 0; call the font getter (font 0, the numbers')
constexpr std::ptrdiff_t kSizeTop = 0x10;     // the game's size: its top this far over the title bar's bottom
constexpr std::uint32_t kWhite = 0x80808080;  // the game's white (the colour is doubled as it's drawn)
constexpr int kGlyphHeight = 11;              // font 0's cells, if the font can't be read

// The menu font (the "font    moji" texture, 16x16 cells), which the title bar's own text ("Items", "Satchel") and the
// windows' lists are printed in. The game's one routine for a whole string (FUN_1011ba10, __thiscall on the renderer, ret 8:
// the string, int pos[2]) sets the blending, binds the texture, then per letter takes its code, prints it at scale 1 and
// steps on by its width:
// sub esp,8; mov eax,[esp+10h]; push ebx; push ebp; mov ebp,[esp+14h]; mov edx,[eax+4]; push esi; mov esi,ecx; mov ecx,[eax];
// mov [esp+0Ch],ecx; push edi; mov ecx,esi; mov [esp+14h],edx; mov [esp+20h],ebp; call states; mov ecx,esi; call bind
constexpr char kPrintString[] = "83 EC 08 8B 44 24 10 53 55 8B 6C 24 14 8B 50 04 56 8B F1 8B 08 89 4C 24 0C 57 8B CE 89 54 24 14 "
                                "89 6C 24 20 E8 ?? ?? ?? ?? 8B CE E8";
constexpr std::ptrdiff_t kStatesCall = 0x24;  // the blending
constexpr std::ptrdiff_t kBindCall = 0x2B;    // the texture (false if it isn't loaded)
constexpr std::ptrdiff_t kCodeCall = 0x50;    // __cdecl (const char**): a letter's code, the pointer moved past it
constexpr std::ptrdiff_t kPrintCall = 0x93;   // __thiscall, ret 10h: (code, int pos[2], sx, sy)
constexpr std::ptrdiff_t kAdvanceCall = 0x9E; // __cdecl (code, int pos[2]): pos[0] moved on by the letter's width
// The renderer, from the title bar's own drawing (CTkTitle::OnDrawPrimitive, FUN_1014f890), which sets it up and lets its
// help line (KaHelpMes) print: mov ecx,[renderer]; call states; mov ecx,[renderer]; call bind; mov eax,[renderer];
// lea ecx,[esi+0A4h]; mov dword ptr [eax+64h],80808080h
constexpr char kTitleText[] = "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? "
                              "8D 8E A4 00 00 00 C7 40 64 80 80 80 80";
constexpr std::ptrdiff_t kRendererAt = 2;
constexpr std::ptrdiff_t kCornerColours[] = {0x10, 0x2C, 0x48, 0x64}; // the renderer: its quad's corners' colours
constexpr int kMenuCell = 16;
constexpr int kMargin = 4; // between the count and the title bar's right edge, as the title's text is from its left

using ItemStack = void(__cdecl*)(int count, int size);
using Locate = void(__thiscall*)(void* ctrl, short* rect);                 // left, top, right, bottom
using DrawReverse = int(__cdecl*)(int right, int top, const char* text, std::uint32_t colour, int flag, float sx, float sy);
using Font = void*(__cdecl*)(int index);
using States = void(__thiscall*)(void* renderer);
using Bind = std::uint8_t(__thiscall*)(void* renderer);
using NextCode = int(__cdecl*)(const char** text);
using PrintCode = void(__thiscall*)(void* renderer, int code, int* pos, float sx, float sy);
using Advance = std::uint8_t(__cdecl*)(int code, int* pos);

enum class Face { Sharp, Menu, Counter };
enum class Separator { Slash, Brackets, Dash }; // 17/80, 17 (80), 17 - 80

// [client] items.count*
bool g_on = false;
float g_size = 1.5f; // .size, percent / 100 of the game's menu text (16 of its units): True Font's size while .font.size is
                     // unset (the panel's size before 2026-11-09), the game's letters' (truefont.disable=1)
Face g_face = Face::Sharp;
int g_dx = 0, g_dy = 0; // .x, .y: moved by this many of the interface's units
std::uint32_t g_rgb = 0xFFFFFF;
// .font.*: True Font's, as every True Font area's (chatfont.cpp read_want): its font (unset the default font), its size in
// the interface's pixels (0: the count left out), bold, italic, its outline (unset the default font's) and the outline's opacity
std::wstring g_family = L"Segoe UI";
float g_pixels = 24.0f;
bool g_bold = false;
bool g_italic = false;
float g_outline = 0.0f;
float g_outline_opacity = 1.0f;
bool g_hide_total = true;
Separator g_separator = Separator::Slash;
bool g_auto_total = false; // .autototal: the total as big as the count (else at the game's own size)

bool g_tried = false;
ItemStack g_item_stack = nullptr; // the game's, through the trampoline
char** g_title = nullptr;
Locate g_locate = nullptr;
DrawReverse g_draw = nullptr;
Font g_font = nullptr;
char** g_renderer = nullptr;
States g_states = nullptr;
Bind g_bind = nullptr;
NextCode g_next_code = nullptr;
PrintCode g_print = nullptr;
Advance g_advance = nullptr;
bool g_space = false; // the interface's area found: the sharp count can be placed

// The sharp count for VanaCore's interface, as the game's drawing of the bag window left it this frame
struct Pending {
    bool set = false;
    std::wstring count, total; // total empty: not shown
    float right = 0, middle = 0; // in the interface's units
};
Pending g_pending;
text::Sprite g_count_sprite, g_total_sprite;
void* g_device = nullptr;

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

// 0xRRGGBB as the game's vertex colour: alpha 0x80, each channel halved (drawn doubled, so 0x80 is full)
std::uint32_t game_colour(std::uint32_t rgb)
{
    const auto channel = [](unsigned c) { return static_cast<std::uint32_t>((c * 128 + 127) / 255); };
    return 0x80000000u | channel((rgb >> 16) & 0xFF) << 16 | channel((rgb >> 8) & 0xFF) << 8 | channel(rgb & 0xFF);
}

// A digit's height in font 0 (its sprite's top and bottom at +0x24 and +0x26)
int glyph_height()
{
    const auto* font = g_font != nullptr ? static_cast<char* const*>(g_font(0)) : nullptr;
    const auto* glyphs = font != nullptr ? reinterpret_cast<const char* const*>(*font) : nullptr;
    const char* glyph = glyphs != nullptr ? glyphs['0' - 0x20] : nullptr;
    if (glyph == nullptr)
        return kGlyphHeight;
    const int height = *reinterpret_cast<const short*>(glyph + 0x26) - *reinterpret_cast<const short*>(glyph + 0x24);
    return height > 0 && height < 64 ? height : kGlyphHeight;
}

// `text`'s letters' codes (up to 16) and its width at `scale`
int menu_codes(const char* text, float scale, int (&codes)[16], float& width)
{
    int count = 0;
    width = 0;
    while (*text != '\0' && count < 16) {
        const int code = static_cast<short>(g_next_code(&text));
        int step[2] = {};
        g_advance(code, step);
        codes[count++] = code;
        width += step[0] * scale;
    }
    return count;
}

// `text` in the menu font at `scale`, its right edge at `right` and its cell's top at `top`: its left edge
float menu_print(void* renderer, const char* text, float right, int top, float scale)
{
    int codes[16];
    float width = 0;
    const int count = menu_codes(text, scale, codes, width);
    float x = right - width;
    for (int i = 0; i < count; ++i) {
        int pos[2] = {static_cast<int>(std::lround(x)), top};
        g_print(renderer, codes[i], pos, scale, scale);
        int step[2] = {};
        g_advance(codes[i], step);
        x += step[0] * scale;
    }
    return right - width;
}

// The total as the separator writes it after the count: "/80", "(80)", "- 80"; `spaced`: with the space brackets and the
// dash take before them ("17 (80)"), for the game's fonts (the sharp font's layout puts its own in)
void total_text(char (&out)[24], int size, bool spaced)
{
    const int total = static_cast<short>(size) < 0 ? 0 : static_cast<short>(size);
    const char* space = spaced ? " " : "";
    if (g_separator == Separator::Brackets)
        std::snprintf(out, sizeof out, "%s(%d)", space, total);
    else if (g_separator == Separator::Dash)
        std::snprintf(out, sizeof out, "%s- %d", space, total);
    else
        std::snprintf(out, sizeof out, "/%d", total);
}

// The total's scale: the count's with autototal, else the game's own
float total_scale()
{
    return g_auto_total ? g_size : 1.0f;
}

// The count (and the total after it) in the menu font. False if it can't be (the font isn't loaded).
bool draw_menu(const short* rect, const char* count, const char* total)
{
    void* renderer = g_renderer != nullptr ? *g_renderer : nullptr;
    if (renderer == nullptr || g_print == nullptr)
        return false;
    g_states(renderer);
    if (g_bind(renderer) == 0)
        return false;
    auto* corners = static_cast<char*>(renderer);
    for (const std::ptrdiff_t corner : kCornerColours)
        *reinterpret_cast<std::uint32_t*>(corners + corner) = game_colour(g_rgb);
    const float cell = kMenuCell * g_size;
    // in the middle of the title bar's height, as the title's own text is (its cell at top+8 of 30)
    const int top = rect[1] + static_cast<int>(std::floor((rect[3] - rect[1] - cell) / 2)) + 1 + g_dy;
    float right = static_cast<float>(rect[2] - kMargin + g_dx);
    if (total != nullptr) { // the cells' bottoms in line
        const float scale = total_scale();
        right = menu_print(renderer, total, right, top + static_cast<int>(std::lround(cell - kMenuCell * scale)), scale);
    }
    menu_print(renderer, count, right, top, g_size);
    for (const std::ptrdiff_t corner : kCornerColours)
        *reinterpret_cast<std::uint32_t*>(corners + corner) = kWhite;
    return true;
}

// The count in the game's counter font (font 0)
void draw_counter(const short* rect, const char* count, const char* total)
{
    const int right = rect[2] + g_dx;
    const int glyph = glyph_height();
    const int height = static_cast<int>(std::lround(glyph * g_size));
    const std::uint32_t colour = game_colour(g_rgb);
    if (total == nullptr) {
        // the count alone, in the middle of the title bar's height
        g_draw(right - kMargin, rect[1] + (rect[3] - rect[1] - height) / 2 + g_dy, count, colour, 1, g_size, g_size);
        return;
    }
    // the total where the game puts its size (or, as big as the count, in the middle), the count before it on the same
    // bottom line
    const float scale = total_scale();
    const int bottom = g_auto_total ? rect[1] + (rect[3] - rect[1] + height) / 2 + g_dy : rect[3] - kSizeTop + glyph + g_dy;
    const int left = g_draw(right - 1, bottom - static_cast<int>(std::lround(glyph * scale)), total, colour, 1, scale, scale);
    g_draw(left, bottom - height, count, colour, 1, g_size, g_size);
}

std::wstring widen(const char* text)
{
    std::wstring wide;
    for (; *text != '\0'; ++text)
        wide += static_cast<wchar_t>(static_cast<unsigned char>(*text));
    return wide;
}

void __cdecl item_stack_hook(int count, int size)
{
    if (!g_on) {
        // The game's own count, its letters all the game's: True Font took the total's digits (Status & Numbers') and left the
        // count's and the "/" the game's slanted ones, so the "/" stood apart from an upright "80" (reported 2026-11-09)
        vanacore_gfx_game_letters(1);
        g_item_stack(count, size);
        vanacore_gfx_game_letters(0);
        return;
    }
    char* title = *g_title;
    void* ctrl = title != nullptr ? *reinterpret_cast<void**>(title + 8) : nullptr;
    if (ctrl == nullptr)
        return;
    short rect[4] = {};
    g_locate(ctrl, rect);
    char text[16];
    std::snprintf(text, sizeof text, "%d", static_cast<short>(count) < 0 ? 0 : static_cast<short>(count));
    char total[24];
    total_text(total, size, g_face != Face::Sharp);
    if (g_face == Face::Sharp && g_space) {
        if (g_pixels <= 0.0f)
            return; // (True Font's size 0: the count left out, as an area's letters are)
        g_pending.set = true;
        g_pending.count = widen(text);
        g_pending.total = g_hide_total ? std::wstring() : widen(total);
        g_pending.right = static_cast<float>(rect[2] - kMargin + g_dx);
        g_pending.middle = (rect[1] + rect[3]) * 0.5f + g_dy;
        return;
    }
    if (g_face != Face::Counter && draw_menu(rect, text, g_hide_total ? nullptr : total))
        return;
    draw_counter(rect, text, g_hide_total ? nullptr : total);
}

// The menu font's routines, for items.count.font=menu (left out if they aren't found: the counter's font then)
void find_menu_font(HMODULE game)
{
    const std::uintptr_t print = scan::find(game, kPrintString);
    const std::uintptr_t title = scan::find(game, kTitleText);
    const auto* code = reinterpret_cast<const std::uint8_t*>(print);
    if (print == 0 || title == 0 || code[kCodeCall] != 0xE8 || code[kPrintCall] != 0xE8 || code[kAdvanceCall] != 0xE8) {
        log::write(L"Item count: the game's menu font wasn't found in FFXiMain.dll: items.count.font=menu draws the counter's");
        return;
    }
    g_renderer = *reinterpret_cast<char** const*>(title + kRendererAt);
    g_states = reinterpret_cast<States>(call_target(print + kStatesCall));
    g_bind = reinterpret_cast<Bind>(call_target(print + kBindCall));
    g_next_code = reinterpret_cast<NextCode>(call_target(print + kCodeCall));
    g_advance = reinterpret_cast<Advance>(call_target(print + kAdvanceCall));
    g_print = reinterpret_cast<PrintCode>(call_target(print + kPrintCall));
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kItemStack) : 0;
    const auto* code = reinterpret_cast<const std::uint8_t*>(at);
    if (at == 0 || code[kReverseCall] != 0xE8) {
        log::write(L"Item count: the game's bag count drawing wasn't found in FFXiMain.dll: [client] items.count does nothing");
        return;
    }
    const std::uintptr_t reverse = call_target(at + kReverseCall);
    g_title = *reinterpret_cast<char** const*>(at + kTitleAt);
    g_locate = reinterpret_cast<Locate>(call_target(at + kLocateCall));
    g_draw = reinterpret_cast<DrawReverse>(reverse);
    if (reinterpret_cast<const std::uint8_t*>(reverse)[kFontCall] == 0xE8)
        g_font = reinterpret_cast<Font>(call_target(reverse + kFontCall));
    find_menu_font(game);
    g_space = uispace::find(game); // else items.count.font=sharp draws the menu font
    if (!imports::detour(reinterpret_cast<ItemStack>(at), kPrologue, &item_stack_hook, g_item_stack)) {
        log::write(L"Item count: the game's bag count drawing couldn't be hooked: [client] items.count does nothing");
        return;
    }
    log::write(L"Item count: the bag count drawing hooked at +0x%X",
               static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}

// [client] `key` as a number; `none` while it isn't set (or isn't a number)
float read_number(const wchar_t* ini, const wchar_t* key, float none)
{
    wchar_t text[64] = {}, * end = nullptr;
    GetPrivateProfileStringW(L"client", key, L"", text, 64, ini);
    const float value = std::wcstof(text, &end);
    return end == text || std::isnan(value) ? none : value;
}

float bounded(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

// The count's letters: True Font's (2026-11-09: items.count.font menu and counter, the game's two, are gone), the
// title's own (menu) only while [client] truefont.disable=1 has True Font off everywhere (chatfont.cpp)
Face read_face(const wchar_t* ini)
{
    return GetPrivateProfileIntW(L"client", L"truefont.disable", 0, ini) == 0 ? Face::Sharp : Face::Menu;
}

// items.count.font.family, .size, .bold, .italic, .outline, .outline.opacity: True Font's, as every area's (chatfont.cpp
// read_want), on the default font's [client] truefont.family, .outline and .outline.opacity. Its size unset: an older
// items.count.size's (g_size, read first: a percent of 16) in the interface's pixels, as the panel shows it
void read_true_font(const wchar_t* ini)
{
    wchar_t text[128] = {};
    GetPrivateProfileStringW(L"client", L"items.count.font.family", L"", text, 128, ini);
    if (text[0] == L'\0')
        GetPrivateProfileStringW(L"client", L"truefont.family", L"Segoe UI", text, 128, ini);
    g_family = text[0] != L'\0' ? text : L"Segoe UI";
    const float size = read_number(ini, L"items.count.font.size", -1.0f);
    g_pixels = size >= 0.0f && size < 0.5f ? 0.0f
               : !(size >= 0.5f)           ? std::floor(kMenuCell * g_size + 0.5f)
                                           : bounded(size, 0.5f, 48.0f);
    g_bold = GetPrivateProfileIntW(L"client", L"items.count.font.bold", 0, ini) != 0;
    g_italic = GetPrivateProfileIntW(L"client", L"items.count.font.italic", 0, ini) != 0;
    const float outline = read_number(ini, L"items.count.font.outline", read_number(ini, L"truefont.outline", 0.0f));
    g_outline = !(outline > 0.0f) ? 0.0f : bounded(outline, 0.0f, 4.0f);
    const float opacity =
        read_number(ini, L"items.count.font.outline.opacity", read_number(ini, L"truefont.outline.opacity", 100.0f));
    g_outline_opacity = (!(opacity > 0.0f) ? 0.0f : bounded(opacity, 0.0f, 100.0f)) / 100.0f;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"items.count", 0, ini) != 0;
    const int size = static_cast<int>(GetPrivateProfileIntW(L"client", L"items.count.size", 150, ini));
    g_size = (size < 50 ? 50 : size > 300 ? 300 : size) / 100.0f;
    g_face = read_face(ini);
    wchar_t text[64] = {};
    const auto offset = [ini](const wchar_t* key) {
        const int value = static_cast<int>(GetPrivateProfileIntW(L"client", key, 0, ini));
        return value < -200 ? -200 : value > 200 ? 200 : value;
    };
    g_dx = offset(L"items.count.x");
    g_dy = offset(L"items.count.y");
    GetPrivateProfileStringW(L"client", L"items.count.color", L"", text, 64, ini);
    float colour[4] = {255.0f, 255.0f, 255.0f, 1.0f};
    text::parse_colour(text, colour);
    g_rgb = static_cast<std::uint32_t>(colour[0]) << 16 | static_cast<std::uint32_t>(colour[1]) << 8 |
            static_cast<std::uint32_t>(colour[2]);
    read_true_font(ini);
    g_hide_total = GetPrivateProfileIntW(L"client", L"items.count.hidetotal", 1, ini) != 0;
    GetPrivateProfileStringW(L"client", L"items.count.separator", L"slash", text, 64, ini);
    g_separator = _wcsicmp(text, L"brackets") == 0 ? Separator::Brackets
                  : _wcsicmp(text, L"dash") == 0   ? Separator::Dash
                                                   : Separator::Slash;
    g_auto_total = GetPrivateProfileIntW(L"client", L"items.count.autototal", 0, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (!g_tried) // (with items.count off too: the game's own count is kept in the game's letters, item_stack_hook)
        install_hook();
}

void draw(void* device, unsigned width, unsigned height)
{
    if (!g_pending.set)
        return;
    g_pending.set = false;
    if (device != g_device) {
        g_count_sprite.release();
        g_total_sprite.release();
        g_device = device;
    }
    float right = 0, middle = 0, scale = 0;
    if (!uispace::to_pixels(g_pending.right, g_pending.middle, width, height, right, middle, scale))
        return;

    text::Style style;
    style.family = g_family;
    style.weight = g_bold ? FW_BOLD : FW_NORMAL;
    style.italic = g_italic;
    style.color[0] = static_cast<float>((g_rgb >> 16) & 0xFF);
    style.color[1] = static_cast<float>((g_rgb >> 8) & 0xFF);
    style.color[2] = static_cast<float>(g_rgb & 0xFF);
    style.background[3] = 0.0f;
    style.padding = 0;
    style.trim = true; // the box is the letters' own pixels: lined up by what's drawn
    // the outline (True Font's: the interface's pixels, as every area's) on the screen's pixels
    const float stroke = g_outline * scale;
    style.stroke = static_cast<int>(std::ceil(stroke));
    style.exact_stroke = stroke;
    style.stroke_color[3] = style.stroke > 0 ? g_outline_opacity : 0.0f;
    const auto sized = [&style](float pixels) {
        text::Style out = style;
        const int size = static_cast<int>(std::lround(pixels));
        out.size = size > 4 ? size : 4;
        return out;
    };
    // The total at the count's size is one string with it ("19/80", "19 (80)", "19 - 80"): the letters spaced by the font.
    // Otherwise (autototal off) it's at the game's menu text's size (16 of the interface's pixels), a sprite of its own after
    // the count, set by the font's pen as one string's letters would be (the slash right after the count, no gap for the
    // count's lean or outline), a space before brackets and the dash.
    const float total_size = g_auto_total ? g_pixels : static_cast<float>(kMenuCell);
    const bool joined = !g_pending.total.empty() && total_size == g_pixels;
    const float total_pixels = total_size * scale;
    g_count_sprite.set(joined ? g_pending.count + (g_separator == Separator::Slash ? L"" : L" ") + g_pending.total
                              : g_pending.count,
                       sized(g_pixels * scale));
    if (!g_count_sprite.upload(device))
        return;
    float x = right - g_count_sprite.width();
    const int top = static_cast<int>(std::lround(middle - g_count_sprite.height() * 0.5f));
    if (!g_pending.total.empty() && !joined) {
        g_total_sprite.set(g_pending.total, sized(total_pixels));
        if (g_total_sprite.upload(device)) {
            // on the count's baseline, its right edge at the title bar's margin
            const int total_left = static_cast<int>(std::lround(right - g_total_sprite.width()));
            const int total_top = top + g_count_sprite.baseline() - g_total_sprite.baseline();
            g_total_sprite.draw(device, total_left, total_top);
            const float gap = g_separator == Separator::Slash ? 0.0f : total_pixels * 0.25f;
            const float count_pen = total_left + g_total_sprite.pen_left() - gap - g_count_sprite.advance();
            x = count_pen - g_count_sprite.pen_left();
        }
    }
    g_count_sprite.draw(device, static_cast<int>(std::lround(x)), top);
}

} // namespace vanacore::itemcount
