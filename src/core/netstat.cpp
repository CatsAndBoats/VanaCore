#include "netstat.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hideui.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "uispace.hpp"

namespace vanacore::netstat {
namespace {

// YkNetData::Draw(x, y) in the PS2 client's names (FUN_101ffa60), __thiscall, ret 8: the Network window's contents at
// its top left corner, each frame it's shown (the main menu open). The window's arrows (animated, green, yellow or red
// by the connection's quality) at the corner, the quality at +14h,+0Ah followed by a '%' from font 0, then "S" and the
// bytes sent a second at +3Ah/+44h,+4, "R" and those received at +3Ah/+44h,+10h, in YkDrawString's slanted font.
//
// sub esp,10h; mov eax,[game]; push esi; test eax,eax; push edi; mov [esp+8],ecx; jz; mov eax,[stats];
// push ebx; add eax,4630h; push ebp; push eax; call (the received). The offset (4638h in retail's build) is read from the
// code: kStatsOffset.
constexpr char kNetDraw[] = "83 EC 10 A1 ?? ?? ?? ?? 56 85 C0 57 89 4C 24 08 0F 84 ?? ?? ?? ?? A1 ?? ?? ?? ?? 53 "
                            "05 ?? ?? 00 00 55 50 E8";
constexpr std::size_t kPrologue = 8;          // sub esp,10h; mov eax,[game]
constexpr std::ptrdiff_t kGameAt = 4;         // the game's object: nothing's drawn without it
constexpr std::ptrdiff_t kStatsAt = 0x17;     // the network's: received, sent (bytes a second) and quality (0-100)
constexpr std::ptrdiff_t kStatsOffset = 0x1D; // in it
constexpr std::ptrdiff_t kLabelCall = 0x52;   // __cdecl (table, id): the game's text ("R" 0Ch,5Dh; "S" 0Ch,5Ch)
constexpr std::ptrdiff_t kStringCall = 0x5D;  // YkDrawString
constexpr std::ptrdiff_t kFrameCall = 0xFC;   // __thiscall (first, quality), ret 8: the arrows' frame, animated
constexpr std::ptrdiff_t kShapeCall = 0x11D;  // YmMenuShape::Draw, __thiscall (x, y, colour, 0, 0)
constexpr std::ptrdiff_t kFontCall = 0x14D;   // the fonts' getter (font 0's '%')
// YkDrawString(x, y, text, colour) passes its arguments on with a scale: mov eax,[esp+10h]; mov ecx,[esp+0Ch];
// mov edx,[esp+8]; push 0; push 1.0f; push 1.0f; push eax; mov eax,[esp+14h]; push ecx; push edx; push eax; call
constexpr std::ptrdiff_t kScaledCall = 0x20;

// YkWndNetStat::OnDrawPrimitive (FUN_101ffcf0): the window's rect (GetWindowLocate), then its POL messages or, with
// m_net at +44h, the network's: sub esp,8; push esi; mov esi,ecx; push edi; lea eax,[esp+8]; mov ecx,[esi+8]; ...
constexpr char kWindowDraw[] = "83 EC 08 56 8B F1 57 8D 44 24 08 8B 4E 08 33 FF 50 66 89 7C 24 0C 66 89 7C 24 0E "
                               "66 89 7C 24 10 66 89 7C 24 12 E8 ?? ?? ?? ?? 8B 46 14 2B C7 74 1B 48 75 2A 8B 4C 24 0A "
                               "8B 54 24 08 51 52 8D 4E 44 E8";
constexpr std::ptrdiff_t kLocateCall = 0x25;
constexpr std::ptrdiff_t kNetCall = 0x41; // YkNetData::Draw
constexpr std::ptrdiff_t kNetAt = 0x44;  // YkNetData in the window
constexpr std::ptrdiff_t kCtrlAt = 8;    // the window's control (its rect)
constexpr int kWidth = 112, kHeight = 30; // the window, if its rect can't be read

// The network's frame (FUN_100f9d80) keeps when the server's last packet came (+4188h) and, from it, how long ago that
// was in ms (about 400 while the server talks; once a second while it's quiet, growing): mov [esi+4190h],ecx;
// mov eax,51EB851Fh; mov ecx,[...]; mul dword ptr [esi+4190h]. Its state at +0Ch is 4 once the game gives up on the
// server (after +41B0h ms, 90 s).
constexpr char kSilence[] = "89 8E ?? ?? ?? ?? B8 1F 85 EB 51 8B 0D ?? ?? ?? ?? F7 A6";
constexpr std::ptrdiff_t kSilenceAt = 2;
constexpr std::ptrdiff_t kStateAt = 0xC;
constexpr std::uint8_t kLost = 4;
constexpr std::uint32_t kFailingMs = 2000; // the server quiet this long: Failing

constexpr std::uint32_t kText = 0x60808080;  // the game's colour for the window's text
constexpr std::uint32_t kWhite = 0x80808080; // and its arrows
constexpr std::uint32_t kGreenText = 0x80408040; // network.simple's Connected, with .simple.green
constexpr std::uint32_t kAmber = 0x80806020; // network.simple's Failing
constexpr std::uint32_t kRed = 0x80803030;   // and Disconnected
constexpr int kGlyph = 11;                   // font 0's digits' height
constexpr int kInset = 6;                    // the line's room from the window's sides
constexpr int kAway = -2000;                 // text measured off the screen
constexpr float kArrowsMiddle[2] = {10.5f, 16.5f}; // the arrows' (21 wide, 8 to 25 down)

using NetDraw = void(__thiscall*)(void* self, int x, int y);
using Label = const char*(__cdecl*)(int table, int id);
using DrawString = int(__cdecl*)(int x, int y, const char* text, std::uint32_t colour, float sx, float sy, int flag);
using Frame = int(__thiscall*)(void* self, int first, int quality);
using Shape = void(__thiscall*)(void* shape, int x, int y, std::uint32_t colour, int, int);
using Font = void*(__cdecl*)(int index);
using Locate = void(__thiscall*)(void* ctrl, short* rect); // left, top, right, bottom

enum class Icon { Arrows, Dot, None };
enum class Layout { Game, Line };
enum class Status { Connected, Failing, Disconnected };

// [client] network*
Icon g_icon = Icon::Arrows;
Layout g_layout = Layout::Game;
bool g_percent = true;
bool g_sent = true, g_received = true; // .sent, .received: S and R shown
bool g_simple = false;  // .simple: Connected, Failing or Disconnected in place of the numbers
bool g_simple_green = false; // .simple.green: Connected in green, not the game's white
bool g_hide = false;    // .hide: nothing while no menu is up (the window's frame gone)
bool g_panel = true;    // .panel=0: the window gone, frame and numbers (helpbar.cpp leaves the frame out)
bool g_background = true; // .background=0: the frame and its "Network" caption gone, the numbers stay
unsigned g_draws = 0;   // the game's drawing of the contents, counted (helpbar.cpp tells where it's called from)
std::string g_text;     // .text, for layout=line; empty: S and R, after the quality with .percent
int g_dx = 0, g_dy = 0; // .x, .y: the numbers moved by this many of the interface's units
float g_dot_size = 10;  // .dot.size, in the interface's units

bool g_tried = false;
NetDraw g_draw = nullptr; // the game's, through the trampoline
void* const* g_game = nullptr;
char* const* g_stats = nullptr;
std::int32_t g_stats_offset = 0;
std::int32_t g_silence_offset = 0; // 0: not found (network.simple goes by R alone)
Label g_label = nullptr;
DrawString g_string = nullptr;
Frame g_frame = nullptr;
Shape g_shape = nullptr;
Font g_font = nullptr;
Locate g_locate = nullptr;
bool g_space = false; // the interface's area found: the dot can be placed

// The dot for VanaCore's interface, as the game's drawing of the window left it this frame
struct Pending {
    bool set = false;
    float x = 0, y = 0;      // its middle, in the interface's units
    std::uint32_t rgb = 0;
};
Pending g_pending;
text::Sprite g_dot;
int g_dot_pixels = 0; // the sprite's: its size and colour
std::uint32_t g_dot_rgb = 0;
void* g_device = nullptr;

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

// Anything the game's own look doesn't have: the window's drawn by VanaCore
bool custom()
{
    return g_icon != Icon::Arrows || g_layout != Layout::Game || !g_percent || !g_sent || !g_received || g_simple ||
           g_hide || !g_panel || !g_background || g_dx != 0 || g_dy != 0;
}

constexpr std::uint32_t kGreen = 0x46D160, kYellow = 0xF2B33D, kRedDot = 0xE5484D, kGrey = 0x808080;

// The dot's colour by the quality, at the game's arrows' steps: none, red under 30, yellow under 60, green
std::uint32_t dot_rgb(int quality)
{
    return quality <= 0 ? kGrey : quality < 30 ? kRedDot : quality < 60 ? kYellow : kGreen;
}

// network.simple's word: Disconnected once nothing came for a whole 3 s count (R 0) with the server quiet, or the game
// has given up; Failing while the server's quiet (the game stalls: monsters stop, actions hang) or R is still 0 from it
Status status(const char* net, int received)
{
    if (net[kStateAt] == kLost)
        return Status::Disconnected;
    const std::uint32_t silence =
        g_silence_offset != 0 ? *reinterpret_cast<const std::uint32_t*>(net + g_silence_offset) : 0;
    const bool quiet = silence >= kFailingMs;
    if (received <= 0)
        return quiet || g_silence_offset == 0 ? Status::Disconnected : Status::Failing;
    return quiet ? Status::Failing : Status::Connected;
}

// The arrows' picture this frame, animated as the game does
void* arrows(char* self, int quality)
{
    const int first = quality <= 0 ? 0 : quality < 30 ? 9 : quality < 60 ? 5 : 1;
    const int frame = g_frame(self, first, first == 0 ? 0 : quality);
    return reinterpret_cast<void**>(self)[frame];
}

// A shape's left and right edges from where it's drawn (+20h, +22h)
int shape_width(const void* shape)
{
    const auto* edges = reinterpret_cast<const short*>(static_cast<const char*>(shape) + 0x20);
    const int width = edges[1] - edges[0];
    return width > 0 && width < 64 ? width : 21;
}

void place_dot(float x, float y, std::uint32_t rgb)
{
    if (!g_space)
        return;
    g_pending.set = true;
    g_pending.x = x;
    g_pending.y = y;
    g_pending.rgb = rgb;
}

int measure(const std::string& text) { return static_cast<short>(g_string(kAway, kAway, text.c_str(), 0, 1, 1, 0)) - kAway; }

// The game's places: the icon at the corner, the quality, S over R
void draw_game(char* self, int x, int y, int sent, int received, int quality)
{
    if (g_icon == Icon::Arrows)
        g_shape(arrows(self, quality), x, y, kWhite, 0, 0);
    else if (g_icon == Icon::Dot)
        place_dot(x + kArrowsMiddle[0], y + kArrowsMiddle[1], dot_rgb(quality));
    x += g_dx;
    y += g_dy;
    char number[16];
    if (g_percent) {
        std::snprintf(number, sizeof number, "%3d", quality);
        const int end = static_cast<short>(g_string(x + 0x14, y + 0xA, number, kText, 1, 1, 0));
        const auto* font = g_font != nullptr ? static_cast<void* const*>(g_font(0)) : nullptr;
        const auto* glyphs = font != nullptr ? static_cast<void* const*>(*font) : nullptr;
        if (glyphs != nullptr && glyphs['%' - 0x20] != nullptr)
            g_shape(glyphs['%' - 0x20], end, y + 0xA, kText, 0, 0);
    }
    // S over R, or the one shown in the middle row (the quality's)
    const int top = g_sent && g_received ? y + 4 : y + 0xA;
    if (g_sent) {
        g_string(x + 0x3A, top, g_label(0xC, 0x5C), kText, 1, 1, 0);
        std::snprintf(number, sizeof number, "%d", sent);
        g_string(x + 0x44, top, number, kText, 1, 1, 0);
    }
    if (g_received) {
        const int row = g_sent ? y + 0x10 : top;
        g_string(x + 0x3A, row, g_label(0xC, 0x5D), kText, 1, 1, 0);
        std::snprintf(number, sizeof number, "%d", received);
        g_string(x + 0x44, row, number, kText, 1, 1, 0);
    }
}

// The line without network.text: the quality, S and R as shown, closer together with the quality to fit
std::string default_pattern()
{
    std::string out = g_percent ? "{p}%" : "";
    const auto add = [&out](const char* item) {
        if (!out.empty())
            out += g_percent ? " " : " - ";
        out += item;
    };
    if (g_sent)
        add(g_percent ? "S{s}" : "S {s}");
    if (g_received)
        add(g_percent ? "R{r}" : "R {r}");
    return out;
}

// network.text (or its default) with {s}, {r} and {p} filled in
std::string line_text(int sent, int received, int quality)
{
    const std::string pattern = !g_text.empty() ? g_text : default_pattern();
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}') {
            const char token = pattern[i + 1];
            if (token == 's' || token == 'r' || token == 'p') {
                out += std::to_string(token == 's' ? sent : token == 'r' ? received : quality);
                i += 2;
                continue;
            }
        }
        out += pattern[i];
    }
    return out;
}

// One line in the middle of the window, the icon before it (the dot in `rgb`); made smaller to fit if it must
void draw_line(char* self, const short* rect, const std::string& text, std::uint32_t colour, int quality, std::uint32_t rgb)
{
    const int width = text.empty() ? 0 : measure(text);
    void* picture = g_icon == Icon::Arrows ? arrows(self, quality) : nullptr;
    const int icon = picture != nullptr ? shape_width(picture) + 2 : g_icon == Icon::Dot ? static_cast<int>(g_dot_size) + 4 : 0;
    const int room = rect[2] - rect[0] - kInset * 2;
    float scale = 1.0f;
    if (width > 0 && icon + width > room)
        scale = std::max(0.6f, static_cast<float>(room - icon) / width);
    const float left = rect[0] + (rect[2] - rect[0] - (icon + width * scale)) * 0.5f + g_dx;
    const int top = rect[1] + static_cast<int>(std::lround((rect[3] - rect[1] - kGlyph * scale) * 0.5f)) + g_dy;
    if (picture != nullptr)
        g_shape(picture, static_cast<int>(std::lround(left)), rect[1] + g_dy, kWhite, 0, 0);
    else if (g_icon == Icon::Dot)
        place_dot(left + g_dot_size * 0.5f, top + kGlyph * scale * 0.5f, rgb);
    if (!text.empty())
        g_string(static_cast<int>(std::lround(left)) + icon, top, text.c_str(), colour, scale, scale, 0);
}

void __fastcall draw_hook(char* self, void*, int x, int y)
{
    char* net = *g_stats;
    ++g_draws;
    if (!g_panel || (g_hide && !hideui::window_up())) // the game goes on drawing the contents with the frame gone
        return;
    if (!custom() || *g_game == nullptr || net == nullptr) {
        g_draw(self, x, y);
        return;
    }
    const auto* values = reinterpret_cast<const std::int32_t*>(net + g_stats_offset);
    const int received = values[0], sent = values[1], quality = values[2];
    x = static_cast<short>(x);
    y = static_cast<short>(y);
    if (g_layout == Layout::Game && !g_simple) {
        draw_game(self, x, y, sent, received, quality);
        return;
    }
    short rect[4] = {static_cast<short>(x), static_cast<short>(y), static_cast<short>(x + kWidth),
                     static_cast<short>(y + kHeight)};
    if (void* ctrl = g_locate != nullptr ? *reinterpret_cast<void**>(self - kNetAt + kCtrlAt) : nullptr; ctrl != nullptr)
        g_locate(ctrl, rect);
    if (!g_simple) {
        draw_line(self, rect, line_text(sent, received, quality), kText, quality, dot_rgb(quality));
        return;
    }
    switch (status(net, received)) {
    case Status::Connected:
        draw_line(self, rect, "Connected", g_simple_green ? kGreenText : kText, quality, kGreen);
        break;
    case Status::Failing:
        draw_line(self, rect, "Failing", kAmber, quality, kYellow);
        break;
    case Status::Disconnected:
        draw_line(self, rect, "Disconnected", kRed, quality, kRedDot);
        break;
    }
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kNetDraw) : 0;
    if (at == 0 || !is_call(at + kLabelCall) || !is_call(at + kStringCall) || !is_call(at + kFrameCall) ||
        !is_call(at + kShapeCall) || !is_call(at + kFontCall)) {
        log::write(L"Network window: the game's drawing of it wasn't found in FFXiMain.dll: [client] network.* do nothing");
        return;
    }
    const std::uintptr_t string = call_target(at + kStringCall);
    if (!is_call(string + kScaledCall)) {
        log::write(L"Network window: the game's text drawing wasn't found in FFXiMain.dll: [client] network.* do nothing");
        return;
    }
    g_game = *reinterpret_cast<void* const* const*>(at + kGameAt);
    g_stats = *reinterpret_cast<char* const* const*>(at + kStatsAt);
    std::memcpy(&g_stats_offset, reinterpret_cast<const void*>(at + kStatsOffset), 4);
    g_label = reinterpret_cast<Label>(call_target(at + kLabelCall));
    g_string = reinterpret_cast<DrawString>(call_target(string + kScaledCall));
    g_frame = reinterpret_cast<Frame>(call_target(at + kFrameCall));
    g_shape = reinterpret_cast<Shape>(call_target(at + kShapeCall));
    g_font = reinterpret_cast<Font>(call_target(at + kFontCall));
    // the window's rect, to put a line in its middle (else the game's size)
    if (const std::uintptr_t window = scan::find(game, kWindowDraw); window != 0 && call_target(window + kNetCall) == at)
        g_locate = reinterpret_cast<Locate>(call_target(window + kLocateCall));
    else
        log::write(L"Network window: its rect wasn't found in FFXiMain.dll: network.layout=line takes the game's size");
    if (const std::uintptr_t silence = scan::find(game, kSilence); silence != 0)
        std::memcpy(&g_silence_offset, reinterpret_cast<const void*>(silence + kSilenceAt), 4);
    else
        log::write(L"Network window: the server's last packet time wasn't found in FFXiMain.dll: network.simple goes by R alone");
    g_space = uispace::find(game);
    void* trampoline = imports::detour(reinterpret_cast<void*>(at), kPrologue, reinterpret_cast<void*>(&draw_hook));
    if (trampoline == nullptr) {
        log::write(L"Network window: the game's drawing of it couldn't be hooked: [client] network.* do nothing");
        return;
    }
    g_draw = reinterpret_cast<NetDraw>(trampoline);
    log::write(L"Network window: its drawing hooked at +0x%X", static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}

// A round dot `size` pixels across in `rgb` (0xRRGGBB): a darker rim, lit up and to the left, its edge smoothed
std::vector<std::uint32_t> dot_image(int size, std::uint32_t rgb)
{
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
    const float radius = size * 0.5f;
    const float rim = size >= 12 ? size / 12.0f : 1.0f;
    const float light_x = radius * 0.7f, light_y = radius * 0.6f;
    const float base[3] = {static_cast<float>((rgb >> 16) & 0xFF), static_cast<float>((rgb >> 8) & 0xFF),
                           static_cast<float>(rgb & 0xFF)};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x + 0.5f - radius, dy = y + 0.5f - radius;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float cover = std::clamp(radius - distance + 0.5f, 0.0f, 1.0f);
            if (cover <= 0.0f)
                continue;
            const float inside = std::clamp(radius - rim - distance + 0.5f, 0.0f, 1.0f);
            const float lx = x + 0.5f - light_x, ly = y + 0.5f - light_y;
            const float near_light = std::clamp(1.0f - std::sqrt(lx * lx + ly * ly) / (radius * 0.9f), 0.0f, 1.0f);
            const float light = near_light * near_light * 0.45f * inside;
            std::uint32_t pixel = static_cast<std::uint32_t>(std::lround(cover * 255)) << 24;
            for (int c = 0; c < 3; ++c) {
                float value = base[c] * (0.35f + 0.65f * inside); // the rim: the colour darkened
                value += (255.0f - value) * light;
                pixel |= static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 255.0f))) << (16 - c * 8);
            }
            pixels[static_cast<std::size_t>(y) * size + x] = pixel;
        }
    }
    return pixels;
}

} // namespace

void reload(const wchar_t* ini)
{
    wchar_t text[128] = {};
    GetPrivateProfileStringW(L"client", L"network.icon", L"arrows", text, 128, ini);
    g_icon = _wcsicmp(text, L"dot") == 0 ? Icon::Dot : _wcsicmp(text, L"none") == 0 ? Icon::None : Icon::Arrows;
    GetPrivateProfileStringW(L"client", L"network.layout", L"game", text, 128, ini);
    g_layout = _wcsicmp(text, L"line") == 0 ? Layout::Line : Layout::Game;
    g_percent = GetPrivateProfileIntW(L"client", L"network.percent", 1, ini) != 0;
    g_sent = GetPrivateProfileIntW(L"client", L"network.sent", 1, ini) != 0;
    g_received = GetPrivateProfileIntW(L"client", L"network.received", 1, ini) != 0;
    g_simple = GetPrivateProfileIntW(L"client", L"network.simple", 0, ini) != 0;
    g_simple_green = GetPrivateProfileIntW(L"client", L"network.simple.green", 0, ini) != 0;
    g_hide = GetPrivateProfileIntW(L"client", L"network.hide", 0, ini) != 0;
    g_panel = GetPrivateProfileIntW(L"client", L"network.panel", 1, ini) != 0;
    g_background = GetPrivateProfileIntW(L"client", L"network.background", 1, ini) != 0;
    GetPrivateProfileStringW(L"client", L"network.text", L"", text, 128, ini);
    g_text.clear();
    for (const wchar_t* c = text; *c != L'\0'; ++c) // the game's font has ASCII
        g_text += *c >= 0x20 && *c < 0x7F ? static_cast<char>(*c) : '?';
    const auto offset = [ini](const wchar_t* key) {
        const int value = static_cast<int>(GetPrivateProfileIntW(L"client", key, 0, ini));
        return value < -100 ? -100 : value > 100 ? 100 : value;
    };
    g_dx = offset(L"network.x");
    g_dy = offset(L"network.y");
    const int size = static_cast<int>(GetPrivateProfileIntW(L"client", L"network.dot.size", 10, ini));
    g_dot_size = static_cast<float>(size < 4 ? 4 : size > 24 ? 24 : size);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (custom() && !g_tried)
        install_hook();
}

bool panel_hidden()
{
    return !g_panel;
}

bool background_hidden()
{
    return !g_background;
}

unsigned contents_draws()
{
    return g_draws;
}

void draw(void* device, unsigned width, unsigned height)
{
    if (!g_pending.set)
        return;
    g_pending.set = false;
    if (device != g_device) {
        g_dot.release();
        g_device = device;
    }
    float x = 0, y = 0, scale = 0;
    if (!uispace::to_pixels(g_pending.x, g_pending.y, width, height, x, y, scale))
        return;
    const int size = std::max(3, static_cast<int>(std::lround(g_dot_size * scale)));
    if (size != g_dot_pixels || g_pending.rgb != g_dot_rgb) {
        g_dot.set_image(dot_image(size, g_pending.rgb), size, size);
        g_dot_pixels = size;
        g_dot_rgb = g_pending.rgb;
    }
    if (g_dot.upload(device))
        g_dot.draw(device, static_cast<int>(std::lround(x - size * 0.5f)), static_cast<int>(std::lround(y - size * 0.5f)));
}

} // namespace vanacore::netstat
