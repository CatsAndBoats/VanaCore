#include "chattabs.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "uispace.hpp"

namespace vanacore::chattabs {
namespace {

// The chat log open to scroll (its class's vtable 0x10331B68) draws its row of tabs with FUN_101376C0 (__thiscall log, the
// kind of message shown; ret 4), read live 2026-11-08:
//   the log's tabs at +68h, two bytes each (the kind a tab shows, the tab's number), ended by a number 0Bh (FUN_10138340
//   makes them as the log opens: Window 2's only while the second chat window is on);
//   the window's place (FUN_10118B70, __thiscall the window at +8, short rect[4]: left, top, right, bottom, the interface's units);
//   each tab's picture at the log's +80h, [number * 2 + shown] (picture table 0Fh's: 64 x 12, its first three quads the tab,
//   "newtex" and "feppal" twice, then a "menu    menu2fon" quad a letter: those are what True Font laid as one row);
//   each drawn by the plain shape draw (FUN_1011F760: x, y, 80808080h, 0, 0), from the window's left 9 above its top, the next
//   30 units right of a tab not shown and a whole picture right of the shown one: each tab covers the last's end ("Part").
// sub esp,10h; mov eax,[esp+14h] | push ebp; push esi; push edi; mov edi,ecx; push eax; mov [esp+10h],edi; call (the shown
// tab); lea ecx,[esp+14h]; mov [esp+10h],eax; xor eax,eax; push ecx; mov ecx,[edi+8]; mov [esp+18h],ax (x4); call (the
// window's place); mov edx,[esp+16h]; mov cl,[edi+69h]
constexpr char kStrip[] = "?? ?? ?? ?? ?? ?? ?? 55 56 57 8B F9 50 89 7C 24 10 E8 ?? ?? ?? ?? 8D 4C 24 14 89 44 24 10 33 C0 51 "
                          "8B 4F 08 66 89 44 24 18 66 89 44 24 1A 66 89 44 24 1C 66 89 44 24 1E E8 ?? ?? ?? ?? 8B 54 24 16 8A 4F 69";
constexpr char kStripStart[] = {'\x83', '\xEC', '\x10', '\x8B', '\x44', '\x24', '\x14'};
constexpr std::ptrdiff_t kRectCall = 0x38, kShapeCall = 0xB6;
// The scaled shape draw (FUN_1011F880, __thiscall shape, ret 20h: x, y, scale x, scale y, colour, centred, blending, quads to
// skip; buttons.cpp's kShapeDraw, which hooks it: the first 5 bytes left out). Not centred, each quad is laid from x, y at the
// scale; at a scale of 1 both it calls the plain draw. The colour tints the picture (80h a channel: as it is; its alpha fades it)
constexpr char kScaledDraw[] = "?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8";
using StripDraw = void(__fastcall*)(char* log, void* edx, int kind);
using RectOf = void(__fastcall*)(void* window, void* edx, short* rect);
using ShapeDraw = void(__fastcall*)(void* shape, void* edx, int x, int y, std::uint32_t colour, int blending, int skip);
using ScaledDraw = void(__fastcall*)(void* shape, void* edx, int x, int y, float sx, float sy, std::uint32_t colour, int centred,
                                     int blending, int skip);

constexpr std::ptrdiff_t kWindow = 8, kTabs = 0x68, kPictures = 0x80;
constexpr int kEnd = 0x0B;                     // the tabs' end
constexpr int kNumbers = 11;                   // the tab numbers with pictures
constexpr int kMost = (kPictures - kTabs) / 2; // tabs looked at: their pairs end where the pictures start (11 and the end)
constexpr int kAbove = 9;                      // the tabs' top over the window's
constexpr int kGameSpacing = 30, kGameHeight = 12;
constexpr std::uint32_t kPlain = 0x80808080;
// A picture (buttons.cpp's shape): its quads' list (+4: a node's next at +0, its quad at +10h), their count (+14h, read as
// the draw starts), its extent in its units (shorts: left, right, top, bottom); a quad's texture's name (+32h)
constexpr std::ptrdiff_t kList = 4, kCount = 0x14, kNodeQuad = 0x10, kLeft = 0x20, kRight = 0x22, kTop = 0x24, kBottom = 0x26;
constexpr std::ptrdiff_t kName = 0x32;
constexpr char kLetters[] = "menu    menu2fon";
constexpr int kQuadsMost = 64;
// The names in their tabs, the interface's units: standing 2 above a tab's bottom (the game's capitals from 1 of its 12, 9
// tall), cut a unit before the next tab
constexpr float kBaselineUp = 2.0f, kCut = 1.0f;
// Flat tabs' colours while chat.tabs.background.color and .selected.color aren't set (0xRRGGBB)
constexpr std::uint32_t kFlatRgb = 0x2A2E45, kFlatShownRgb = 0xC87A2A;

// The game's language, in its settings (jplabels.cpp's): +8 in them, 0 Japanese
constexpr char kLanguage[] = "8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0F 83 F8 02 74 05 83 F8 03 75 05 "
                             "B8 01 00 00 00 50 E8";
constexpr std::ptrdiff_t kSettingsAt = 2, kLanguageAt = 8;

// Each tab's name by its number (the pictures' letters, read live 2026-11-08: "Window" with a small 1 or 2, "AssistJ"): full,
// then short. Window's is "Window 1" while Window 2's tab is there too
constexpr const char* kNames[kNumbers][2] = {
    {"Window", "Win"},  {"Window 2", "Win 2"},    {"Say", "Say"},           {"Tell", "Tell"},
    {"Party", "PT"},    {"Linkshell", "LS"},      {"Linkshell 2", "LS2"},   {"Assist J", "AJ"},
    {"Assist E", "AE"}, {"Unity", "Unity"},       {"Shout", "Shout"}};

enum Back { kGameBack, kFlatBack, kNoBack };

// [client] chat.tabs.*
struct Settings {
    bool hide = false, short_names = false, centre = false, fit = false;
    int spacing = kGameSpacing, gap = 1, back = kGameBack, opacity = 100;
    float height = kGameHeight, pad = 4.0f, dx = 0.0f, dy = 0.0f;
    float colour[4] = {255.0f, 255.0f, 255.0f, 1.0f}, shown_colour[4] = {255.0f, 255.0f, 255.0f, 1.0f};
    bool tinted = false, shown_tinted = false; // .tint, .selected.tint set (the game's picture's)
    std::uint32_t tint_rgb = 0xFFFFFF, shown_tint_rgb = 0xFFFFFF;
    std::uint32_t back_rgb = kFlatRgb, shown_back_rgb = kFlatShownRgb; // .background.color, .background.selected.color (flat)
};
Settings g_set;

struct Tab {
    int number;
    bool shown;
    int x;    // its left, the interface's units
    int room; // what shows of it: to the next tab's left, the last its whole width
};
struct Row {
    Tab tabs[kMost] = {};
    int count = 0;
    int top = 0, height = kGameHeight; // the tabs' top and height, the interface's units
    bool second = false;               // Window 2's tab is there
    bool fit = false;                  // each as wide as its name (chat.tabs.width=fit), chat.tabs.gap apart
};
Row g_pending; // the row the game drew this frame, its names to draw (draw takes it)

StripDraw g_game = nullptr;
RectOf g_rect = nullptr;
ShapeDraw g_shape = nullptr;
ScaledDraw g_scaled = nullptr;
char* const* g_settings = nullptr;
bool g_tried = false;

text::Sprite g_sprites[kNumbers][2]; // by number, and shown
void* g_device = nullptr;

template <typename T> T at(const void* base, std::ptrdiff_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
    return value;
}

std::uintptr_t call_target(std::uintptr_t call)
{
    return call + 5 + static_cast<std::uintptr_t>(at<std::int32_t>(reinterpret_cast<const void*>(call), 1));
}

bool japanese()
{
    const char* settings = g_settings != nullptr ? *g_settings : nullptr;
    return settings != nullptr && at<int>(settings, kLanguageAt) == 0;
}

// The names are VanaCore's: the Chat tabs' True Font is on (built), in a game in English
bool ours() { return chatfont::area_on(VANA_FONT_CHATTABS) && !japanese(); }

std::string name_of(int number, bool second, bool upper)
{
    std::string name = number == 0 && second ? (g_set.short_names ? "Win 1" : "Window 1") : kNames[number][g_set.short_names ? 1 : 0];
    if (upper)
        for (char& c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

// The quads of `picture` before its first letter (all of them, none there)
int before_letters(const char* picture, int all)
{
    const char* node = at<const char*>(picture, kList);
    for (int i = 0; node != nullptr && i < all && i < kQuadsMost; ++i, node = at<const char*>(node, 0)) {
        const char* quad = at<const char*>(node, kNodeQuad);
        if (quad != nullptr && std::memcmp(quad + kName, kLetters, sizeof kLetters - 1) == 0)
            return i;
    }
    return all;
}

// The game's picture's tint (chat.tabs.tint, the shown tab's .selected.tint: each channel halved, 80h as it is; white as it
// is) and its fade (.background.opacity, as its alpha)
std::uint32_t tint(bool shown)
{
    const bool tinted = shown ? g_set.shown_tinted : g_set.tinted;
    const std::uint32_t rgb = shown ? g_set.shown_tint_rgb : g_set.tint_rgb;
    const auto alpha = static_cast<std::uint32_t>(g_set.opacity * 0x80 / 100);
    return alpha << 24 | (tinted ? (rgb >> 1) & 0x7F7F7Fu : kPlain & 0xFFFFFFu);
}

// A tab's picture drawn `width` x `height` (its own `own_w` x `own_h` stretched to them), its letters left out while the names
// are VanaCore's (the picture's count cut to the quads before its first letter for the draw, the game's put back); nothing of
// it with a flat background or none
void draw_picture(char* picture, int x, int y, int width, int height, bool shown, bool cut)
{
    if (!cut) {
        g_shape(picture, nullptr, x, y, kPlain, 0, 0);
        return;
    }
    if (g_set.back != kGameBack)
        return;
    const int own_w = at<short>(picture, kRight) - at<short>(picture, kLeft), own_h = at<short>(picture, kBottom) - at<short>(picture, kTop);
    auto* count = reinterpret_cast<int*>(picture + kCount);
    const int all = *count;
    *count = before_letters(picture, all);
    if ((width == own_w && height == own_h) || g_scaled == nullptr || own_w <= 0 || own_h <= 0)
        g_shape(picture, nullptr, x, y, tint(shown), 0, 0);
    else
        g_scaled(picture, nullptr, x, y, static_cast<float>(width) / static_cast<float>(own_w),
                 static_cast<float>(height) / static_cast<float>(own_h), tint(shown), 0, 0, 0);
    *count = all;
}

// A tab as wide as its name in the Chat tabs' font and chat.tabs.padding on both sides (chat.tabs.width=fit)
int fit_width(int number, bool second, bool upper)
{
    const float wide = chatfont::text_width(VANA_FONT_CHATTABS, name_of(number, second, upper).c_str());
    return static_cast<int>(std::ceil((wide > 0.0f ? wide : 0.0f) + g_set.pad * 2.0f));
}

int shown_of(const std::uint8_t* tabs, int kind)
{
    for (int i = 0; i < kMost && tabs[i * 2] != kEnd; ++i)
        if (tabs[i * 2] == kind)
            return tabs[i * 2 + 1];
    return -1;
}

// The game's row of tabs laid again: as the game lays it but for chat.tabs.spacing, or each as wide as its name
// (chat.tabs.width=fit) chat.tabs.gap apart; each its picture as chat.tabs.background has it (the game's without its letters,
// tinted and stretched to chat.tabs.height, while the names are VanaCore's); the names kept for draw
void lay(char* log, int kind, bool cut)
{
    const auto* tabs = reinterpret_cast<const std::uint8_t*>(log + kTabs);
    const int shown = shown_of(tabs, kind);
    short rect[4] = {};
    g_rect(at<void*>(log, kWindow), nullptr, rect);
    text::Style unused_letters, unused_stroked;
    bool upper = false;
    float unused_opacity = 0.0f;
    const bool fit = cut && g_set.fit &&
                     chatfont::area_style(VANA_FONT_CHATTABS, unused_letters, unused_stroked, upper, unused_opacity);
    Row row;
    row.fit = fit;
    row.height = cut ? static_cast<int>(g_set.height) : kGameHeight;
    row.top = rect[1] - kAbove + kGameHeight - row.height; // (taller, upward: its bottom stays the game's)
    for (int i = 0; i < kMost && tabs[i * 2 + 1] != kEnd; ++i)
        row.second = row.second || tabs[i * 2 + 1] == 1;
    int x = rect[0];
    for (int i = 0; i < kMost; ++i) {
        const int number = tabs[i * 2 + 1];
        const bool on = number == shown;
        char* picture = number < kNumbers ? at<char*>(log, kPictures + (number * 2 + (on ? 1 : 0)) * 4) : nullptr;
        if (picture == nullptr)
            break;
        const int own = at<short>(picture, kRight) - at<short>(picture, kLeft);
        const int width = fit ? fit_width(number, row.second, upper) : own;
        draw_picture(picture, x, row.top, width, row.height, on, cut);
        row.tabs[row.count++] = {number, on, x, width};
        x += fit ? width + g_set.gap : on ? own : g_set.spacing;
    }
    for (int i = 0; !fit && i + 1 < row.count; ++i) // (the next covers the rest)
        row.tabs[i].room = std::min(row.tabs[i].room, row.tabs[i + 1].x - row.tabs[i].x);
    if (cut)
        g_pending = row;
}

void __fastcall strip_hook(char* log, void* edx, int kind)
{
    if (g_set.hide && g_rect != nullptr) // chat.tabs.hide: nothing of them
        return;
    const bool cut = ours();
    if (!(cut || g_set.spacing != kGameSpacing) || g_rect == nullptr || g_shape == nullptr) {
        g_game(log, edx, kind);
        return;
    }
    lay(log, kind, cut);
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at_strip = game != nullptr ? scan::find(game, kStrip) : 0;
    const auto* code = reinterpret_cast<const std::uint8_t*>(at_strip);
    if (at_strip == 0 || std::memcmp(code, kStripStart, sizeof kStripStart) != 0 || code[kRectCall] != 0xE8 ||
        code[kShapeCall] != 0xE8) {
        log::write(L"Chat tabs: the chat log's tabs weren't found in FFXiMain.dll: [client] chat.tabs.* and their True Font do nothing");
        return;
    }
    g_rect = reinterpret_cast<RectOf>(call_target(at_strip + kRectCall));
    g_shape = reinterpret_cast<ShapeDraw>(call_target(at_strip + kShapeCall));
    g_scaled = reinterpret_cast<ScaledDraw>(scan::find(game, kScaledDraw)); // (none: the tabs' pictures at their own size)
    if (const std::uintptr_t language = scan::find(game, kLanguage); language != 0)
        g_settings = at<char* const*>(reinterpret_cast<const void*>(language), kSettingsAt);
    if (!imports::detour(reinterpret_cast<StripDraw>(at_strip), sizeof kStripStart, &strip_hook, g_game)) {
        g_rect = nullptr;
        log::write(L"Chat tabs: the chat log's tabs couldn't be hooked: [client] chat.tabs.* and their True Font do nothing");
        return;
    }
    log::write(L"Chat tabs: the chat log's tabs hooked at +0x%X", static_cast<unsigned>(at_strip - reinterpret_cast<std::uintptr_t>(game)));
}

// A name's look: the Chat tabs' letters in chat.tabs.color (the shown tab's chat.tabs.selected.color), the font's outline in
// the same picture
text::Style tab_style(const text::Style& letters, const text::Style& stroked, float opacity, bool shown)
{
    text::Style style = letters;
    const float* colour = shown ? g_set.shown_colour : g_set.colour;
    style.color[0] = colour[0], style.color[1] = colour[1], style.color[2] = colour[2], style.color[3] = 1.0f;
    style.stroke = stroked.stroke, style.exact_stroke = stroked.exact_stroke;
    style.stroke_color[3] = stroked.stroke > 0 ? opacity : 0.0f;
    style.background[3] = 0.0f;
    return style;
}

// A flat tab (chat.tabs.background=flat): its colour at chat.tabs.background.opacity, chat.tabs.gap short of the next tab
void draw_flat(void* device, unsigned width, unsigned height, const Row& row, const Tab& tab)
{
    const int wide = row.fit ? tab.room : std::max(1, tab.room - g_set.gap);
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f, scale = 0.0f;
    if (!uispace::to_pixels(static_cast<float>(tab.x), static_cast<float>(row.top), width, height, left, top, scale) ||
        !uispace::to_pixels(static_cast<float>(tab.x + wide), static_cast<float>(row.top + row.height), width, height, right, bottom, scale))
        return;
    const int x0 = static_cast<int>(std::lround(left)), y0 = static_cast<int>(std::lround(top));
    const auto alpha = static_cast<std::uint32_t>(g_set.opacity * 255 / 100);
    text::fill(device, x0, y0, static_cast<int>(std::lround(right)) - x0, static_cast<int>(std::lround(bottom)) - y0,
               alpha << 24 | (tab.shown ? g_set.shown_back_rgb : g_set.back_rgb));
}

// One tab's name: from its left past chat.tabs.padding (or in the middle of what shows of it), cut where the next tab covers it
void draw_name(void* device, unsigned width, unsigned height, const Row& row, const Tab& tab, text::Sprite& sprite)
{
    const float y = static_cast<float>(row.top + row.height) - kBaselineUp + g_set.dy;
    float left = 0.0f, base = 0.0f, scale = 0.0f, start = 0.0f, end = 0.0f, unused = 0.0f;
    if (!uispace::to_pixels(static_cast<float>(tab.x) + g_set.pad + g_set.dx, y, width, height, left, base, scale) ||
        !uispace::to_pixels(static_cast<float>(tab.x) + g_set.dx, y, width, height, start, unused, scale) ||
        !uispace::to_pixels(static_cast<float>(tab.x + tab.room) - kCut, y, width, height, end, unused, scale) || !sprite.upload(device))
        return;
    const float pen = g_set.centre ? std::max(left, (start + end) * 0.5f - sprite.exact_advance() * 0.5f) : left;
    const int x = static_cast<int>(std::lround(pen)) - sprite.pen_left();
    const int top = static_cast<int>(std::lround(base)) - sprite.baseline();
    const int shows = static_cast<int>(std::lround(end)) - x; // its pixels before the cut
    if (shows <= 0 || sprite.texture() == nullptr)
        return;
    if (shows >= sprite.width()) {
        sprite.draw(device, x, top);
        return;
    }
    text::draw_texture(device, sprite.texture(), static_cast<float>(x), static_cast<float>(top), static_cast<float>(shows),
                       static_cast<float>(sprite.height()), 0.0f, 0.0f,
                       static_cast<float>(shows) / static_cast<float>(sprite.texture_width()),
                       static_cast<float>(sprite.height()) / static_cast<float>(sprite.texture_height()), 0xFFFFFFFFu);
}

int clamped(const wchar_t* ini, const wchar_t* key, int low, int high, int fallback)
{
    const int value = static_cast<int>(GetPrivateProfileIntW(L"client", key, fallback, ini));
    return value < low ? low : value > high ? high : value;
}

void read_colour(const wchar_t* ini, const wchar_t* key, float (&colour)[4])
{
    wchar_t text[32] = {};
    colour[0] = colour[1] = colour[2] = 255.0f, colour[3] = 1.0f;
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    text::parse_colour(text, colour);
}

// A background colour (0xRRGGBB): `fallback` while it isn't set (false)
bool read_rgb(const wchar_t* ini, const wchar_t* key, std::uint32_t fallback, std::uint32_t& rgb)
{
    wchar_t text[32] = {};
    float colour[4] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    const bool set = text[0] != L'\0' && text::parse_colour(text, colour);
    rgb = set ? static_cast<std::uint32_t>(colour[0]) << 16 | static_cast<std::uint32_t>(colour[1]) << 8 | static_cast<std::uint32_t>(colour[2])
              : fallback;
    return set;
}

bool is(const wchar_t* ini, const wchar_t* key, const wchar_t* fallback, const wchar_t* value)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, fallback, text, 32, ini);
    return _wcsicmp(text, value) == 0;
}

} // namespace

void reload(const wchar_t* ini)
{
    Settings s;
    s.hide = GetPrivateProfileIntW(L"client", L"chat.tabs.hide", 0, ini) != 0;
    s.short_names = is(ini, L"chat.tabs.names", L"full", L"short");
    s.centre = is(ini, L"chat.tabs.align", L"left", L"centre") || is(ini, L"chat.tabs.align", L"left", L"center");
    s.fit = is(ini, L"chat.tabs.width", L"game", L"fit");
    s.back = is(ini, L"chat.tabs.background", L"game", L"flat") ? kFlatBack : is(ini, L"chat.tabs.background", L"game", L"none") ? kNoBack : kGameBack;
    s.dx = static_cast<float>(clamped(ini, L"chat.tabs.x", -16, 16, 0));
    s.dy = static_cast<float>(clamped(ini, L"chat.tabs.y", -16, 16, 0));
    s.pad = static_cast<float>(clamped(ini, L"chat.tabs.padding", 0, 16, 4));
    s.height = static_cast<float>(clamped(ini, L"chat.tabs.height", 8, 32, kGameHeight));
    s.spacing = clamped(ini, L"chat.tabs.spacing", 16, 64, kGameSpacing);
    s.gap = clamped(ini, L"chat.tabs.gap", 0, 16, 1);
    s.opacity = clamped(ini, L"chat.tabs.background.opacity", 0, 100, 100);
    read_colour(ini, L"chat.tabs.color", s.colour);
    read_colour(ini, L"chat.tabs.selected.color", s.shown_colour);
    s.tinted = read_rgb(ini, L"chat.tabs.tint", 0xFFFFFF, s.tint_rgb);
    s.shown_tinted = read_rgb(ini, L"chat.tabs.selected.tint", 0xFFFFFF, s.shown_tint_rgb);
    read_rgb(ini, L"chat.tabs.background.color", kFlatRgb, s.back_rgb);
    read_rgb(ini, L"chat.tabs.background.selected.color", kFlatShownRgb, s.shown_back_rgb);
    g_set = s;
}

void install(const wchar_t* ini) { reload(ini); }

void on_frame()
{
    if (!g_tried && (chatfont::area_on(VANA_FONT_CHATTABS) || g_set.spacing != kGameSpacing || g_set.hide))
        install_hook();
}

void draw(void* device, unsigned width, unsigned height)
{
    const Row row = g_pending;
    g_pending.count = 0;
    text::Style letters, stroked;
    bool upper = false;
    float opacity = 0.0f;
    if (row.count == 0)
        return;
    if (g_set.back == kFlatBack)
        for (int i = 0; i < row.count; ++i)
            draw_flat(device, width, height, row, row.tabs[i]);
    if (!chatfont::area_style(VANA_FONT_CHATTABS, letters, stroked, upper, opacity))
        return;
    if (device != g_device) {
        for (auto& pair : g_sprites)
            for (text::Sprite& sprite : pair)
                sprite.release();
        g_device = device;
    }
    for (int i = 0; i < row.count; ++i) {
        const Tab& tab = row.tabs[i];
        text::Sprite& sprite = g_sprites[tab.number][tab.shown ? 1 : 0];
        const std::string name = name_of(tab.number, row.second, upper);
        sprite.set(std::wstring(name.begin(), name.end()), tab_style(letters, stroked, opacity, tab.shown));
        draw_name(device, width, height, row, tab, sprite);
    }
}

} // namespace vanacore::chattabs
