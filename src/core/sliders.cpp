#include "sliders.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "d3d8.hpp"
#include "itemslot.hpp"
#include "text.hpp"

namespace vanacore::sliders {
namespace {

// A slider, seen live 2026-11-20 on Window Settings ("menu    conf5w1", Config > Windows), is drawn from the "menu    gauge"
// picture, 64 x 64 texels (its parts below in texels of it):
// - in the window's frame (its kind-0 piece's shape, drawn through the shape drawing with a quad to skip): a half dot at
//   each end (texels 0-4 and 4-8 of rows 8-16, the two halves of the dot the window's headings have, 8 x 16 units each)
//   and the bar between them (rows 0-8 across, 192 x 16 units, added at alpha 0x31); the first slider of that window also
//   has a picture of rows 16-32 at its start (texels 48-64), left out with it;
// - the slider's button (a type-1 piece the cursor goes to): its shape is a bar the same size at alpha 0, unseen, drawn gold
//   (0x805020) while the cursor's on it;
// - its value, by the Config window's own draw (FUN_10195260, __thiscall on the window's handler: the slider's id, the
//   share, the colour; ret 0Ch): the handler's shape +1Ch, one quad of rows 1-7 across (192 x 12 units, 2 down) drawn at
//   the button's place by the scaled shape drawing with a scale across of the share (FUN_101952D0 skips it at a value of 0).
// The flat style finds each slider in the frame's shape from its ends and bar, leaves those out, draws its own bar in its
// place, and its fill and knob where the game draws its value; the button's gold says which one the cursor's on.
constexpr char kGauge[] = "menu    gauge";
constexpr int kTexels = 64; // the picture's texels a side: the parts' corners are compared in them
struct Texels {
    int u0, v0, u1, v1;
    bool operator==(const Texels&) const = default;
};
constexpr Texels kBar = {0, 0, 64, 8}, kLeftEnd = {0, 8, 4, 16}, kRightEnd = {4, 8, 8, 16}, kFill = {0, 1, 64, 7};

// A quad (buttons.cpp's, itemslot.cpp's): its picture's name (+32h), the game's texture (+4Ch: its Direct3D texture at +40h,
// else +44h), its four corners (+50h, 1Ch apart: x, y, z, rhw, colour B G R A, u, v) and its own corners in the shape's
// units (x shorts at +C0h, y at +C8h: top left, top right, bottom left, bottom right)
constexpr std::ptrdiff_t kName = 0x32, kTexture = 0x4C, kCorners = 0x50, kCorner = 0x1C, kColour = 0x10, kAlpha = 0x13;
constexpr std::ptrdiff_t kU = 0x14, kV = 0x18, kOwnX = 0xC0, kOwnY = 0xC8;
constexpr std::size_t kQuadSize = 0xD0;
constexpr std::ptrdiff_t kD3d = 0x40, kD3dElse = 0x44;
// A shape: its quads' list (+4: a node's next at +0, its quad at +10h) and their count (+14h)
constexpr std::ptrdiff_t kShapeList = 4, kShapeCount = 0x14, kNodeQuad = 0x10;
constexpr int kQuadsAtMost = 256;
constexpr int kBlendOver = 1 << 2 | 1 << 6; // itemslot.cpp's: blended over what's under it
constexpr int kOpaque = 0x80;               // a corner's alpha for opaque (doubled as it's drawn)
constexpr std::uint32_t kNeutral = 0x80808080;
constexpr std::uint32_t kGold = 0x805020, kGoldOff = 0x402810; // the cursor's button's colour (greyed out)
constexpr int kGetDevice = 3;
constexpr float kFine = 4.0f; // the drawn quads' own units a pixel: a quarter of one (the round parts' edges)
constexpr float kNear = 3.0f; // pixels a value's place may be off its slider's

enum class Mark { None, Game, Round, Square, Bar };

struct Style {
    bool flat = false;
    int height = 4; // the bar's thickness (the interface's units)
    bool round = true;
    std::uint32_t track = 0x3A4266, fill = 0x8C9EFF, selected = 0x8C9EFF; // (0xRRGGBB)
    int track_alpha = kOpaque;
    Mark knob = Mark::Round;
    int knob_size = 10;
    std::uint32_t knob_colour = 0xFFFFFF;
    Mark ends = Mark::None;
    int ends_size = 6;
    std::uint32_t ends_colour = 0x8A8FA6;
};
Style g_style;

// The parts of the sliders in the shape being drawn (Shape): each quad's part and its slider, the sliders in its units
enum class Part { Bar, LeftEnd, RightEnd, Other };
struct Box {
    int left, top, right, bottom;
};
struct Found {
    const char* quad;
    Part part;
    int slider;
};
struct Layout {
    Box bar, left, right;
};
constexpr int kFoundAtMost = 96, kLayoutsAtMost = 24;
Found g_found[kFoundAtMost] = {};
int g_found_count = 0;
Layout g_layouts[kLayoutsAtMost] = {};
int g_layout_count = 0;

// The sliders drawn in the windows being drawn (nested: each window's from its mark), in pixels: the bar's ends, the game
// bar's top and bottom, the draw's scale (the interface's units to pixels), whether the cursor's on it and its value drawn
struct Row {
    float left, right, top, bottom, scale;
    bool selected, filled;
};
constexpr int kRowsAtMost = 64, kDepthAtMost = 8;
Row g_rows[kRowsAtMost] = {};
int g_row_count = 0;
int g_marks[kDepthAtMost] = {};
int g_depth = 0;

// The pictures drawn with, made on the game's device: white, and a white dot (its edge soft over a texel); a stand-in for
// the game's texture each (its Direct3D texture at +40h and +44h), and the quad copied to draw with (a slider's own)
constexpr int kDot = 32;
void* g_device = nullptr;
void* g_white = nullptr;
void* g_dot = nullptr;
alignas(8) std::uint8_t g_stand_white[kD3dElse + sizeof(void*)] = {};
alignas(8) std::uint8_t g_stand_dot[kD3dElse + sizeof(void*)] = {};
alignas(8) std::uint8_t g_quad[kQuadSize] = {};
bool g_have_quad = false;

bool gauge(const char* quad)
{
    return std::memcmp(quad + kName, kGauge, sizeof kGauge - 1) == 0;
}

// A quad's texture corners as texels of the gauge, and its own corners' box
Texels texels_of(const char* quad)
{
    float lo_u = 2.0f, lo_v = 2.0f, hi_u = -1.0f, hi_v = -1.0f;
    for (int c = 0; c < 4; ++c) {
        float u = 0.0f, v = 0.0f;
        std::memcpy(&u, quad + kCorners + c * kCorner + kU, sizeof u);
        std::memcpy(&v, quad + kCorners + c * kCorner + kV, sizeof v);
        lo_u = std::min(lo_u, u), hi_u = std::max(hi_u, u);
        lo_v = std::min(lo_v, v), hi_v = std::max(hi_v, v);
    }
    const auto texel = [](float f) { return static_cast<int>(std::lround(f * kTexels)); };
    return {texel(lo_u), texel(lo_v), texel(hi_u), texel(hi_v)};
}

Box box_of(const char* quad)
{
    const auto* xs = reinterpret_cast<const short*>(quad + kOwnX);
    const auto* ys = reinterpret_cast<const short*>(quad + kOwnY);
    return {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
            *std::max_element(ys, ys + 4)};
}

bool unseen(const char* quad) // every corner's alpha 0 (a button's bar: its place, not drawn)
{
    for (int c = 0; c < 4; ++c)
        if (quad[kCorners + c * kCorner + kAlpha] != 0)
            return false;
    return true;
}

// The gauge quads of `shape` into `quads` (up to `most`), their texels and boxes beside them; their count
struct Seen {
    const char* quad;
    Texels t;
    Box b;
};
int gauge_quads(const char* shape, Seen* quads, int most)
{
    int count = 0;
    __try {
        const int total = *reinterpret_cast<const int*>(shape + kShapeCount);
        const char* node = *reinterpret_cast<char* const*>(shape + kShapeList);
        for (int i = 0; node != nullptr && i < total && i < kQuadsAtMost && count < most;
             ++i, node = *reinterpret_cast<char* const*>(node)) {
            const char* quad = *reinterpret_cast<char* const*>(node + kNodeQuad);
            if (quad != nullptr && gauge(quad))
                quads[count++] = {quad, texels_of(quad), box_of(quad)};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return count;
}

void note(const char* quad, Part part, int slider)
{
    if (g_found_count < kFoundAtMost)
        g_found[g_found_count++] = {quad, part, slider};
}

// The bar beside left end `l` (its top and bottom, starting where the end stops) and the right end past it; -1 for none
int bar_after(const Seen* quads, int count, const Seen& l, int& right)
{
    right = -1;
    int bar = -1;
    for (int i = 0; i < count; ++i) {
        const Seen& q = quads[i];
        if (q.b.top != l.b.top || q.b.bottom != l.b.bottom || q.b.left < l.b.right - 1)
            continue;
        if (q.t == kBar && std::abs(q.b.left - l.b.right) <= 1 && (bar < 0 || q.b.left < quads[bar].b.left))
            bar = i;
        else if (q.t == kRightEnd && (right < 0 || q.b.left < quads[right].b.left))
            right = i;
    }
    return bar >= 0 && right >= 0 && std::abs(quads[right].b.left - quads[bar].b.right) <= 1 ? bar : -1;
}

// The sliders in `shape`: each one's ends and bar, and any other gauge picture inside its place (left out with it)
void scan(const char* shape)
{
    Seen quads[kQuadsAtMost];
    const int count = gauge_quads(shape, quads, kQuadsAtMost);
    for (int i = 0; i < count && g_layout_count < kLayoutsAtMost; ++i) {
        if (quads[i].t != kLeftEnd)
            continue;
        int right = -1;
        const int bar = bar_after(quads, count, quads[i], right);
        if (bar < 0)
            continue;
        const int slider = g_layout_count++;
        g_layouts[slider] = {quads[bar].b, quads[i].b, quads[right].b};
        note(quads[i].quad, Part::LeftEnd, slider);
        note(quads[right].quad, Part::RightEnd, slider);
        note(quads[bar].quad, Part::Bar, slider);
    }
    for (int i = 0; i < count && g_layout_count > 0; ++i) {
        const Box& b = quads[i].b;
        const int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        bool known = false;
        for (int f = 0; f < g_found_count && !known; ++f)
            known = g_found[f].quad == quads[i].quad;
        for (int s = 0; s < g_layout_count && !known; ++s) {
            const Layout& l = g_layouts[s];
            if (cx >= l.left.left && cx <= l.right.right && cy >= l.bar.top && cy <= l.bar.bottom) {
                note(quads[i].quad, Part::Other, s);
                known = true;
            }
        }
    }
}

const Found* found(const char* quad)
{
    for (int i = 0; i < g_found_count; ++i)
        if (g_found[i].quad == quad)
            return &g_found[i];
    return nullptr;
}

// --- drawing ---

void* device_of(const char* quad)
{
    const char* texture = *reinterpret_cast<char* const*>(quad + kTexture);
    void* d3d = texture != nullptr ? *reinterpret_cast<void* const*>(texture + kD3d) : nullptr;
    if (d3d == nullptr && texture != nullptr)
        d3d = *reinterpret_cast<void* const*>(texture + kD3dElse);
    void* device = nullptr;
    if (d3d == nullptr || FAILED(d3d8::call(d3d, kGetDevice, &device)) || device == nullptr)
        return nullptr;
    d3d8::call<ULONG>(device, d3d8::slot::Release);
    return device;
}

// A size x size A8R8G8B8 picture on `device`, white, each texel's alpha as `alpha` says (its centre's x and y)
template <typename Alpha>
void* make_picture(void* device, int size, Alpha alpha)
{
    void* picture = nullptr;
    if (FAILED(d3d8::call(device, d3d8::slot::CreateTexture, static_cast<UINT>(size), static_cast<UINT>(size), 1u, DWORD{0},
                          d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged, &picture)) ||
        picture == nullptr)
        return nullptr;
    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(picture, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<RECT*>(nullptr), DWORD{0}))) {
        d3d8::call<ULONG>(picture, d3d8::slot::Release);
        return nullptr;
    }
    for (int y = 0; y < size; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(locked.bits) + y * locked.pitch);
        for (int x = 0; x < size; ++x) {
            const float a = std::clamp(alpha(x + 0.5f, y + 0.5f), 0.0f, 1.0f);
            row[x] = static_cast<std::uint32_t>(std::lround(a * 255.0f)) << 24 | 0xFFFFFF;
        }
    }
    d3d8::call(picture, d3d8::slot::TextureUnlockRect, 0u);
    return picture;
}

// The pictures on the device of `quad`'s texture, made the first time (and again on another device: the old ones went
// with it); false if they can't be had
bool pictures_for(const char* quad)
{
    void* device = device_of(quad);
    if (device == nullptr)
        return false;
    if (device == g_device)
        return g_white != nullptr && g_dot != nullptr; // (made, or failed to be: not tried again on it)
    g_device = device;
    g_white = make_picture(device, 4, [](float, float) { return 1.0f; });
    const float r = kDot / 2.0f - 0.5f;
    g_dot = make_picture(device, kDot, [r](float x, float y) {
        return r + 0.5f - std::hypot(x - kDot / 2.0f, y - kDot / 2.0f);
    });
    std::memcpy(g_stand_white + kD3d, &g_white, sizeof g_white);
    std::memcpy(g_stand_white + kD3dElse, &g_white, sizeof g_white);
    std::memcpy(g_stand_dot + kD3d, &g_dot, sizeof g_dot);
    std::memcpy(g_stand_dot + kD3dElse, &g_dot, sizeof g_dot);
    return g_white != nullptr && g_dot != nullptr;
}

// `quad` (a slider's) kept to draw with, on the pictures of its device; false if they can't be had
bool take_quad(const char* quad)
{
    if (!pictures_for(quad))
        return false;
    std::memcpy(g_quad, quad, kQuadSize);
    g_have_quad = true;
    return true;
}

struct Rect {
    float left, top, right, bottom; // pixels
};

// `r` in `rgb` at `alpha`: the white picture, or the dot's texels from u0 to u1 across (a dot, or half of one)
void paint(const Rect& r, std::uint32_t rgb, int alpha, bool dot = false, float u0 = 0.0f, float u1 = 1.0f)
{
    if (!g_have_quad || r.right - r.left < 0.25f || r.bottom - r.top < 0.25f || alpha <= 0)
        return;
    const std::uint8_t* stand = dot ? g_stand_dot : g_stand_white;
    std::memcpy(g_quad + kTexture, &stand, sizeof stand);
    auto* xs = reinterpret_cast<short*>(g_quad + kOwnX);
    auto* ys = reinterpret_cast<short*>(g_quad + kOwnY);
    const auto fine = [](float px) { return static_cast<short>(std::lround(px * kFine)); };
    xs[0] = xs[2] = fine(r.left);
    xs[1] = xs[3] = fine(r.right);
    ys[0] = ys[1] = fine(r.top);
    ys[2] = ys[3] = fine(r.bottom);
    // a corner's colour is doubled as it's drawn: half the colour wanted
    const auto half = [rgb](int shift) { return static_cast<std::uint8_t>(((rgb >> shift & 0xFF) + 1) / 2); };
    const std::uint8_t bgra[4] = {half(0), half(8), half(16), static_cast<std::uint8_t>(alpha)};
    for (int c = 0; c < 4; ++c) {
        std::memcpy(g_quad + kCorners + c * kCorner + kColour, bgra, 4);
        const float u = dot ? ((c & 1) != 0 ? u1 : u0) : 0.5f, v = dot ? ((c & 2) != 0 ? 1.0f : 0.0f) : 0.5f;
        std::memcpy(g_quad + kCorners + c * kCorner + kU, &u, sizeof u);
        std::memcpy(g_quad + kCorners + c * kCorner + kV, &v, sizeof v);
    }
    itemslot::draw_game(g_quad, 0, 0, 1.0f / kFine, 1.0f / kFine, kNeutral, kBlendOver);
}

// A bar from `left` to `right` round `cy`, `h` pixels thick: its ends round with sliders.round (an oval when it's no longer
// than it's thick)
void bar(float left, float right, float cy, float h, std::uint32_t rgb, int alpha)
{
    const float top = std::round(cy - h / 2.0f), bottom = top + h;
    if (right - left <= 0.0f)
        return;
    if (!g_style.round || h < 3.0f) {
        paint({left, top, right, bottom}, rgb, alpha);
        return;
    }
    const float cap = h / 2.0f;
    if (right - left <= h) {
        paint({left, top, right, bottom}, rgb, alpha, true);
        return;
    }
    paint({left, top, left + cap, bottom}, rgb, alpha, true, 0.0f, 0.5f);
    paint({left + cap, top, right - cap, bottom}, rgb, alpha);
    paint({right - cap, top, right, bottom}, rgb, alpha, true, 0.5f, 1.0f);
}

// A knob or an end: `kind` round (`x`, `cy`), `size` pixels
void mark(Mark kind, float x, float cy, float size, std::uint32_t rgb)
{
    const float half = size / 2.0f;
    switch (kind) {
    case Mark::Round:
        paint({x - half, cy - half, x + half, cy + half}, rgb, kOpaque, true);
        break;
    case Mark::Square:
        paint({std::round(x - half), std::round(cy - half), std::round(x - half) + size, std::round(cy - half) + size}, rgb, kOpaque);
        break;
    case Mark::Bar: {
        const float w = std::max(2.0f, std::round(size / 4.0f));
        paint({std::round(x - w / 2.0f), std::round(cy - half), std::round(x - w / 2.0f) + w, std::round(cy - half) + size}, rgb, kOpaque);
        break;
    }
    default:
        break;
    }
}

float pixels(int units, float scale)
{
    return std::max(1.0f, std::round(static_cast<float>(units) * scale));
}

float middle(const Row& row)
{
    return (row.top + row.bottom) / 2.0f;
}

void knob(const Row& row, float x)
{
    mark(g_style.knob, x, middle(row), pixels(g_style.knob_size, row.scale), g_style.knob_colour);
}

// The slider whose bar `x`, `y` (pixels: a button's or a value's place) starts, or null
Row* row_at(float x, float y)
{
    for (int i = g_row_count - 1; i >= 0; --i) {
        Row& row = g_rows[i];
        if (std::abs(x - row.left) <= kNear && y >= row.top - kNear && y <= row.bottom + kNear)
            return &row;
    }
    return nullptr;
}

// The frame's bar of slider `s` drawn at `x`, `y` and the scale: kept as a row, its own bar and ends drawn in its place
void draw_bar(const char* quad, const Layout& s, int x, int y, float sx, float sy)
{
    if (!take_quad(quad))
        return;
    const auto px_x = [x, sx](int units) { return static_cast<float>(x) + static_cast<float>(units) * sx; };
    const auto px_y = [y, sy](int units) { return static_cast<float>(y) + static_cast<float>(units) * sy; };
    Row row = {px_x(s.bar.left), px_x(s.bar.right), px_y(s.bar.top), px_y(s.bar.bottom), sy, false, false};
    if (g_row_count < kRowsAtMost)
        g_rows[g_row_count++] = row;
    const float cy = middle(row);
    bar(row.left, row.right, cy, pixels(g_style.height, sy), g_style.track, g_style.track_alpha);
    if (g_style.ends == Mark::None || g_style.ends == Mark::Game)
        return;
    // each in the middle of the game's end's place
    const float size = pixels(g_style.ends_size, sy);
    const auto centre = [x, sx](const Box& b) { return static_cast<float>(x) + static_cast<float>(b.left + b.right) * sx / 2.0f; };
    mark(g_style.ends, centre(s.left), cy, size, g_style.ends_colour);
    mark(g_style.ends, centre(s.right), cy, size, g_style.ends_colour);
}

// A slider's value (the fill, drawn at its button's place, stretched across by its share): its own fill and knob instead
bool draw_value(const char* quad, int x, int y, float sx, float sy)
{
    const Box b = box_of(quad);
    const float left = static_cast<float>(x) + static_cast<float>(b.left) * sx;
    const float right = static_cast<float>(x) + static_cast<float>(b.right) * sx;
    const float cy = static_cast<float>(y) + static_cast<float>(b.top + b.bottom) * sy / 2.0f;
    Row* row = row_at(left, cy);
    if (row == nullptr || !take_quad(quad))
        return false;
    const float end = row->left + std::clamp(right - left, 0.0f, row->right - row->left);
    bar(row->left, end, middle(*row), pixels(g_style.height, row->scale), row->selected ? g_style.selected : g_style.fill, kOpaque);
    knob(*row, end);
    row->filled = true;
    return true;
}

// --- settings ---

bool colour_of(const wchar_t* ini, const wchar_t* key, std::uint32_t& rgb)
{
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 64, ini);
    float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (text[0] == L'\0' || !text::parse_colour(text, c))
        return false;
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f))); };
    rgb = byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
    return true;
}

int number_of(const wchar_t* ini, const wchar_t* key, int fallback, int low, int high)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    wchar_t* end = nullptr;
    const long value = std::wcstol(text, &end, 10);
    return std::clamp(end != text ? static_cast<int>(value) : fallback, low, high);
}

Mark mark_of(const wchar_t* ini, const wchar_t* key, Mark fallback)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    return _wcsicmp(text, L"none") == 0     ? Mark::None
           : _wcsicmp(text, L"game") == 0   ? Mark::Game
           : _wcsicmp(text, L"round") == 0  ? Mark::Round
           : _wcsicmp(text, L"square") == 0 ? Mark::Square
           : _wcsicmp(text, L"bar") == 0    ? Mark::Bar
                                            : fallback;
}

} // namespace

void reload(const wchar_t* ini)
{
    Style s;
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", L"sliders.style", L"", text, 32, ini);
    s.flat = _wcsicmp(text, L"flat") == 0;
    s.height = number_of(ini, L"sliders.height", s.height, 1, 16);
    s.round = number_of(ini, L"sliders.round", 1, 0, 1) != 0;
    colour_of(ini, L"sliders.track.color", s.track);
    s.track_alpha = (number_of(ini, L"sliders.track.opacity", 100, 0, 100) * kOpaque + 50) / 100;
    colour_of(ini, L"sliders.fill.color", s.fill);
    s.selected = s.fill;
    colour_of(ini, L"sliders.selected.color", s.selected);
    s.knob = mark_of(ini, L"sliders.knob", s.knob);
    if (s.knob == Mark::Game)
        s.knob = Mark::None; // (the game has no knob)
    s.knob_size = number_of(ini, L"sliders.knob.size", s.knob_size, 2, 24);
    colour_of(ini, L"sliders.knob.color", s.knob_colour);
    s.ends = mark_of(ini, L"sliders.ends", s.ends);
    s.ends_size = number_of(ini, L"sliders.ends.size", s.ends_size, 2, 16);
    colour_of(ini, L"sliders.ends.color", s.ends_colour);
    g_style = s;
}

bool active()
{
    return g_style.flat;
}

Shape::Shape(const void* shape) : scanned_(false)
{
    if (!g_style.flat || shape == nullptr || g_found_count != 0)
        return; // (a shape drawn inside another's: the outer one's parts kept)
    scan(static_cast<const char*>(shape));
    scanned_ = true;
}

Shape::~Shape()
{
    if (scanned_)
        g_found_count = g_layout_count = 0;
}

void enter_window()
{
    if (g_depth < kDepthAtMost)
        g_marks[g_depth] = g_row_count;
    ++g_depth;
}

void leave_window()
{
    if (g_depth == 0)
        return;
    --g_depth;
    if (g_depth >= kDepthAtMost)
        return; // (too deep to have a mark: its sliders go with the window it's in)
    const int first = g_marks[g_depth];
    for (int i = first; i < g_row_count && g_style.flat; ++i)
        if (!g_rows[i].filled)
            knob(g_rows[i], g_rows[i].left); // a value of 0: the game draws no fill
    g_row_count = first;
}

void on_frame()
{
    if (g_depth == 0)
        g_row_count = 0;
}

bool on_quad(const char* quad, int x, int y, float sx, float sy, std::uint32_t colour)
{
    if (!g_style.flat || !gauge(quad))
        return false;
    if (const Found* f = found(quad)) {
        switch (f->part) {
        case Part::Bar:
            draw_bar(quad, g_layouts[f->slider], x, y, sx, sy);
            return true;
        case Part::LeftEnd:
        case Part::RightEnd:
            return g_style.ends != Mark::Game; // (drawn with the bar)
        default:
            return true;
        }
    }
    const Texels t = texels_of(quad);
    if (t == kBar && unseen(quad)) { // a slider's button: gold while the cursor's on it
        const Box b = box_of(quad);
        const std::uint32_t rgb = colour & 0xFFFFFF;
        Row* row = row_at(static_cast<float>(x) + static_cast<float>(b.left) * sx,
                          static_cast<float>(y) + static_cast<float>(b.top + b.bottom) * sy / 2.0f);
        if (row != nullptr)
            row->selected = rgb == kGold || rgb == kGoldOff;
        return false;
    }
    if (t == kFill)
        return draw_value(quad, x, y, sx, sy);
    return false;
}

} // namespace vanacore::sliders
