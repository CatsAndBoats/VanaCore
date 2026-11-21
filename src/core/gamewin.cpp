#include "gamewin.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "addons.hpp"
#include "chatfont.hpp"
#include "gameui.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::gamewin {
namespace {

// The menu manager, found as gameevents.cpp finds its list of windows (mov ecx, <manager> where it walks them)
constexpr char kManager[] = "32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8";
constexpr std::ptrdiff_t kManagerAt = 3;
constexpr std::ptrdiff_t kCurrent = 0x54; // the window the keys go to (menulog.cpp's)
constexpr std::ptrdiff_t kNodeNext = 0, kNodeItem = 0x10;
constexpr int kMostNodes = 128;
// A window's
constexpr std::ptrdiff_t kLayout = 4, kState = 0x10, kPieces = 0x14, kRect = 0x3A, kCursor = 0x4C, kCount = 0x58, kDown = 0xB8;
// ... the key input's own tests: +0x56 frames before it takes a key again, +0x5C set while it takes them at all
constexpr std::ptrdiff_t kWait = 0x56, kTakesKeys = 0x5C;
constexpr std::ptrdiff_t kLayoutName = 0x46;
constexpr int kClosing = 0xE;
constexpr int kMostRows = 96;
// A piece's (a button's)
constexpr short kButton = 1;
constexpr std::ptrdiff_t kShape = 4, kPieceState = 0x10, kPlace = 0x2A, kId = 0x3A, kShown = 0x3C;
constexpr int kGreyed = 5;
// A shape's and a quad's
constexpr std::ptrdiff_t kShapeFirst = 4;
constexpr std::ptrdiff_t kQuadName = 0x32, kQuadBlend = 0x2E, kCorners = 0x50, kCorner = 0x1C, kAlpha = 0x13, kU = 0x14, kV = 0x18;
constexpr std::ptrdiff_t kOwnX = 0xC0, kOwnY = 0xC8;
constexpr int kMostQuads = 128;
constexpr char kFontSheet[] = "font    font";   // the labels' letters' sheet (its name's first 12)
constexpr char kBar[] = "menu    button";      // the buttons' bars: the rows have them
constexpr char kMenu[] = "menu    ";
constexpr float kSheet = 256.0f;                // the labels' sheet's texels a side
constexpr std::uint8_t kShadowBlend = 2;        // a quad's +0x2E for the labels' faint shadow (subtracted), 0 the letters
constexpr std::uint8_t kFaint = 0x40;           // ... and its alpha below this
constexpr int kLabelSpace = 3;                  // a gap this wide between a label's letters is a space

// The game's text conversion (cdecl on a char** it moves on: the font sheet's cell of the character there; FFXiMain's
// 0x1011CA80), SetCursol (0x101189B0, thiscall: the row, 0; ret 8) and a window's key input (0x10117E60, thiscall: the key;
// ret 4), each one place in the 2025-12-26 FFXiMain.dll
constexpr char kToCell[] = "56 8B 74 24 08 57 8B 3E 57 E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 0F 8B 16 33 C0 42 89 16 8A 07 5F 83 E8 20 5E C3";
constexpr char kSetCursor[] = "83 EC 08 56 57 8B 7C 24 14 8B F1 66 85 FF 7F 05 BF 01 00 00 00 66 8B 46 58 66 3B F8 7E 02 8B F8 66 8B 46 4C";
constexpr char kKeyInput[] = "51 55 56 8B F1 57 83 7E 10 0E 75 09 5F 5E 33 C0 5D 59 C2 04 00";
using ToCell = int(__cdecl*)(const std::uint8_t**);
using SetCursor = void(__fastcall*)(void* window, void* edx, int row, int flag);
using KeyInput = int(__fastcall*)(void* window, void* edx, int key);
constexpr std::size_t kCells = 0x2200; // the sheet's cells (64 x 128) and the icons' past them, 0x21F1 the last seen

constexpr ULONGLONG kWatchMs = 3000; // a window asked for has its draws kept this long after
constexpr std::size_t kMostGlyphs = 4096, kMostPictures = 256;

using Name = std::array<char, 16>;

bool g_searched = false;
const char* const* g_manager = nullptr;
ToCell g_to_cell = nullptr;
SetCursor g_set_cursor = nullptr;
KeyInput g_key_input = nullptr;
std::vector<std::uint16_t> g_sjis; // a cell's character, Shift-JIS (one byte below 0x100, else lead << 8 | trail); 0 none

// A letter or a label's letter the window's draw made, the interface's units
struct Glyph {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    std::string text;
    std::uint32_t colour = 0;
};

// What a window's draws made, its last one
struct Capture {
    const void* window = nullptr;
    Name name{};
    ULONGLONG at = 0;
    std::vector<Glyph> glyphs;
    std::vector<Picture> pictures;
};

struct Watch {
    Name name;
    ULONGLONG until;
};

// An addon's watch (vana.gameui.watch): the windows it hears of
struct Watcher {
    std::string key;
    std::vector<Name> names;
    bool all = false;
};

// A watched window seen drawn: what its last draw showed, to tell a change from the same again
struct Seen {
    const void* window;
    Name name;
    std::uint64_t signature;
};

std::vector<Watch> g_watch;
std::vector<Watcher> g_watchers;
std::vector<Seen> g_seen;
std::vector<Capture> g_captures;
Capture* g_now = nullptr; // the window being drawn's, while it's kept
bool g_ever = false;      // a window was read or watched: the quad hook is wanted

void search()
{
    if (g_searched)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_searched = true;
    const std::uintptr_t manager = scan::find(game, kManager);
    if (manager != 0)
        std::memcpy(&g_manager, reinterpret_cast<const void*>(manager + kManagerAt), sizeof g_manager);
    g_to_cell = reinterpret_cast<ToCell>(scan::find(game, kToCell));
    g_set_cursor = reinterpret_cast<SetCursor>(scan::find(game, kSetCursor));
    g_key_input = reinterpret_cast<KeyInput>(scan::find(game, kKeyInput));
    log::write(L"Game windows: the menu manager %ls, the text conversion %ls, SetCursol %ls, the key input %ls (vana.gameui.window, press)",
               g_manager != nullptr ? L"found" : L"NOT found", g_to_cell != nullptr ? L"found" : L"NOT found",
               g_set_cursor != nullptr ? L"found" : L"NOT found", g_key_input != nullptr ? L"found" : L"NOT found");
}

bool lead_byte(int b)
{
    return (b > 0x80 && b < 0xA0) || (b > 0xDF && b < 0xF0) || (b > 0xF9 && b < 0xFD); // FFXiMain's 0x1011D630
}

// Every character the game's conversion takes to a cell, the first for each cell; ASCII alone without it
void build_sjis()
{
    g_sjis.assign(kCells, 0);
    for (int b = 0x20; b < 0x100; ++b)
        if (!lead_byte(b))
            g_sjis[static_cast<std::size_t>(b - 0x20)] = static_cast<std::uint16_t>(b);
    if (g_to_cell == nullptr)
        return;
    for (int lead = 0x81; lead <= 0xFC; ++lead) {
        if (!lead_byte(lead))
            continue;
        for (int trail = 0x40; trail <= 0xFC; ++trail) {
            if (trail == 0x7F)
                continue;
            const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(lead), static_cast<std::uint8_t>(trail), 0};
            const std::uint8_t* at = bytes;
            const int cell = g_to_cell(&at);
            if (cell >= 0 && static_cast<std::size_t>(cell) < kCells && g_sjis[static_cast<std::size_t>(cell)] == 0)
                g_sjis[static_cast<std::size_t>(cell)] = static_cast<std::uint16_t>(lead << 8 | trail);
        }
    }
}

std::string sjis_of(int code)
{
    if (g_sjis.empty())
        build_sjis();
    if (code < 0 || static_cast<std::size_t>(code) >= g_sjis.size() || g_sjis[static_cast<std::size_t>(code)] == 0)
        return {};
    const std::uint16_t c = g_sjis[static_cast<std::size_t>(code)];
    if (c < 0x100)
        return std::string(1, static_cast<char>(c));
    return std::string{static_cast<char>(c >> 8), static_cast<char>(c & 0xFF)};
}

// ---- reading the game's memory (no C++ objects here: structured exceptions) ----

struct RawWindow {
    char name[16];
    int state;
    short rect[4];
    short cursor, count, down, wait;
    std::uint8_t keys;
};

struct RawRow {
    const char* shape;
    int state;
    short place[4]; // x, y, width, height
    short id;
    std::uint8_t shown;
};

struct RawGlyph {
    short lo[2], hi[2]; // its own corners' box, the shape's units
    int u, v;           // its sprite's top left on the sheet, texels
    std::uint8_t blend, alpha;
};

bool name_of(const char* window, char (&name)[16])
{
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(window + kLayout);
        if (layout == nullptr)
            return false;
        std::memcpy(name, layout + kLayoutName, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The windows up into `out`, and the one the keys go to
int read_list(const char** out, int most, const char*& current)
{
    current = nullptr;
    int count = 0;
    __try {
        current = *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(g_manager) + kCurrent);
        int nodes = 0;
        for (const char* node = *g_manager; node != nullptr && nodes < kMostNodes && count < most;
             node = *reinterpret_cast<const char* const*>(node + kNodeNext), ++nodes) {
            const char* window = *reinterpret_cast<const char* const*>(node + kNodeItem);
            if (window != nullptr)
                out[count++] = window;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

bool read_window(const char* window, RawWindow& out)
{
    if (!name_of(window, out.name))
        return false;
    __try {
        std::memcpy(&out.state, window + kState, sizeof out.state);
        std::memcpy(out.rect, window + kRect, sizeof out.rect);
        std::memcpy(&out.cursor, window + kCursor, sizeof out.cursor);
        std::memcpy(&out.count, window + kCount, sizeof out.count);
        std::memcpy(&out.down, window + kDown, sizeof out.down);
        std::memcpy(&out.wait, window + kWait, sizeof out.wait);
        out.keys = *reinterpret_cast<const std::uint8_t*>(window + kTakesKeys);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int read_rows(const char* window, RawRow* out, int most)
{
    int count = 0;
    __try {
        int nodes = 0;
        for (const char* node = *reinterpret_cast<const char* const*>(window + kPieces); node != nullptr && nodes < kMostNodes && count < most;
             node = *reinterpret_cast<const char* const*>(node + kNodeNext), ++nodes) {
            const char* piece = *reinterpret_cast<const char* const*>(node + kNodeItem);
            if (piece == nullptr || *reinterpret_cast<const short*>(piece) != kButton)
                continue;
            RawRow& row = out[count++];
            row.shape = *reinterpret_cast<const char* const*>(piece + kShape);
            std::memcpy(&row.state, piece + kPieceState, sizeof row.state);
            std::memcpy(row.place, piece + kPlace, sizeof row.place);
            std::memcpy(&row.id, piece + kId, sizeof row.id);
            row.shown = *reinterpret_cast<const std::uint8_t*>(piece + kShown);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

// A quad's texture coordinates' least u and v, as the sheet's texels
void quad_uv(const char* quad, int& u, int& v)
{
    float lo_u = 1.0f, lo_v = 1.0f;
    for (int c = 0; c < 4; ++c) {
        float cu = 0, cv = 0;
        std::memcpy(&cu, quad + kCorners + c * kCorner + kU, sizeof cu);
        std::memcpy(&cv, quad + kCorners + c * kCorner + kV, sizeof cv);
        lo_u = std::min(lo_u, cu);
        lo_v = std::min(lo_v, cv);
    }
    u = static_cast<int>(std::lround(lo_u * kSheet));
    v = static_cast<int>(std::lround(lo_v * kSheet));
}

// A quad's own corners' box
void quad_box(const char* quad, short (&lo)[2], short (&hi)[2])
{
    short x[4], y[4];
    std::memcpy(x, quad + kOwnX, sizeof x);
    std::memcpy(y, quad + kOwnY, sizeof y);
    lo[0] = *std::min_element(x, x + 4), hi[0] = *std::max_element(x, x + 4);
    lo[1] = *std::min_element(y, y + 4), hi[1] = *std::max_element(y, y + 4);
}

// The label's letters' quads in a button's shape
int read_glyphs(const char* shape, RawGlyph* out, int most)
{
    int count = 0;
    __try {
        int nodes = 0;
        for (const char* node = *reinterpret_cast<const char* const*>(shape + kShapeFirst); node != nullptr && nodes < kMostQuads && count < most;
             node = *reinterpret_cast<const char* const*>(node + kNodeNext), ++nodes) {
            const char* quad = *reinterpret_cast<const char* const*>(node + kNodeItem);
            if (quad == nullptr || std::memcmp(quad + kQuadName, kFontSheet, sizeof kFontSheet - 1) != 0)
                continue;
            RawGlyph& g = out[count++];
            quad_box(quad, g.lo, g.hi);
            quad_uv(quad, g.u, g.v);
            g.blend = *reinterpret_cast<const std::uint8_t*>(quad + kQuadBlend);
            g.alpha = *reinterpret_cast<const std::uint8_t*>(quad + kCorners + kAlpha);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

// ---- names, labels, text ----

// "menuwind" or "menu    menuwind" as the layout has it: 16 characters, spaces after
Name layout_name(const std::string& name)
{
    Name out;
    out.fill(' ');
    const bool full = name.size() > 8 && name.compare(0, 8, kMenu) == 0;
    const std::string word = full ? name.substr(8) : name;
    std::memcpy(out.data(), kMenu, 8);
    std::memcpy(out.data() + 8, word.data(), std::min<std::size_t>(word.size(), 8));
    return out;
}

std::string word_of(const char (&name)[16])
{
    const bool prefixed = std::memcmp(name, kMenu, 8) == 0;
    std::string word(name + (prefixed ? 8 : 0), prefixed ? 8 : 16);
    while (!word.empty() && (word.back() == ' ' || word.back() == '\0'))
        word.pop_back();
    return word;
}

bool same(const Name& a, const char* b)
{
    return std::memcmp(a.data(), b, 16) == 0;
}

// A button's label from its letters: each letter is there twice (a faint shadow 2 right and down, then the letter): the
// shadows left out, the rest in order across, a gap kLabelSpace wide or more a space
std::string label_of(const char* shape)
{
    if (shape == nullptr)
        return {};
    RawGlyph glyphs[kMostQuads];
    int n = read_glyphs(shape, glyphs, kMostQuads);
    std::vector<RawGlyph> letters;
    for (int i = 0; i < n; ++i)
        if (glyphs[i].blend != kShadowBlend && glyphs[i].alpha >= kFaint)
            letters.push_back(glyphs[i]);
    if (letters.empty() && n > 0) // (no shadow told apart: the second half, when the two are the same letters)
        letters.assign(glyphs + n / 2, glyphs + n);
    std::stable_sort(letters.begin(), letters.end(), [](const RawGlyph& a, const RawGlyph& b) { return a.lo[0] < b.lo[0]; });
    std::string text;
    int right = 0;
    for (const RawGlyph& g : letters) {
        if (!text.empty() && g.lo[0] - right >= kLabelSpace)
            text += ' ';
        const char c = chatfont::label_char(g.u, g.v);
        text += c != 0 ? c : '?';
        right = g.hi[0];
    }
    return text;
}

bool overlaps_down(const Line& run, const Glyph& g)
{
    const float top = std::max(static_cast<float>(run.rect.top), g.y);
    const float bottom = std::min(static_cast<float>(run.rect.bottom), g.y + g.h);
    return bottom - top >= std::min(static_cast<float>(run.rect.bottom - run.rect.top), g.h) * 0.5f;
}

// The draw's letters joined into runs: each next letter of the same colour, beside the one before on its row, goes on its
// run (a gap of a few pixels a space); a run the same as one before it a pixel or two off (a shadow drawn first) replaces it
std::vector<Line> lines_of(const std::vector<Glyph>& glyphs)
{
    std::vector<Line> runs;
    for (const Glyph& g : glyphs) {
        const float unit = std::max(1.0f, g.h / 16.0f);
        Line* run = runs.empty() ? nullptr : &runs.back();
        const bool join = run != nullptr && run->colour == g.colour && overlaps_down(*run, g) &&
                          g.x >= run->rect.right - 2 * unit && g.x - run->rect.right <= 16.0f * unit;
        if (!join) {
            runs.push_back({g.text, {static_cast<int>(g.x), static_cast<int>(g.y), static_cast<int>(std::ceil(g.x + g.w)),
                                     static_cast<int>(std::ceil(g.y + g.h))},
                            g.colour});
            continue;
        }
        if (g.x - run->rect.right >= 3.0f * unit && !run->text.empty() && run->text.back() != ' ' && g.text != " ")
            run->text += ' ';
        run->text += g.text;
        run->rect.right = std::max(run->rect.right, static_cast<int>(std::ceil(g.x + g.w)));
        run->rect.top = std::min(run->rect.top, static_cast<int>(g.y));
        run->rect.bottom = std::max(run->rect.bottom, static_cast<int>(std::ceil(g.y + g.h)));
    }
    std::vector<Line> out;
    for (Line& run : runs) {
        while (!run.text.empty() && run.text.back() == ' ')
            run.text.pop_back();
        if (run.text.empty())
            continue;
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const Line& l) {
                                     return l.text == run.text && std::abs(l.rect.left - run.rect.left) <= 2 &&
                                            std::abs(l.rect.top - run.rect.top) <= 2;
                                 }),
                  out.end());
        out.push_back(std::move(run));
    }
    return out;
}

// ---- the windows asked for ----

void watch(const Name& name)
{
    const ULONGLONG until = GetTickCount64() + kWatchMs;
    for (Watch& w : g_watch)
        if (w.name == name) {
            w.until = until;
            return;
        }
    g_watch.push_back({name, until});
}

bool watched(const char (&name)[16])
{
    const ULONGLONG now = GetTickCount64();
    g_watch.erase(std::remove_if(g_watch.begin(), g_watch.end(), [now](const Watch& w) { return w.until < now; }), g_watch.end());
    return std::any_of(g_watch.begin(), g_watch.end(), [&](const Watch& w) { return same(w.name, name); });
}

// The capture of a window's draw, kept for it (no window drawn now: g_captures can grow)
Capture& capture_of(const void* window, const char (&name)[16])
{
    const ULONGLONG now = GetTickCount64();
    g_captures.erase(std::remove_if(g_captures.begin(), g_captures.end(), [now](const Capture& c) { return now - c.at > 2 * kWatchMs; }),
                     g_captures.end());
    for (Capture& c : g_captures)
        if (c.window == window && same(c.name, name))
            return c;
    Capture c;
    c.window = window;
    std::memcpy(c.name.data(), name, 16);
    g_captures.push_back(std::move(c));
    return g_captures.back();
}

// The first window up whose layout is `name`, and the one the keys go to
const char* find_window(const Name& name, const char*& current)
{
    current = nullptr;
    search();
    if (g_manager == nullptr)
        return nullptr;
    const char* windows[kMostNodes];
    const int n = read_list(windows, kMostNodes, current);
    for (int i = 0; i < n; ++i) {
        char have[16];
        if (name_of(windows[i], have) && same(name, have))
            return windows[i];
    }
    return nullptr;
}

void read_rows_into(const char* window, short down, int cursor, std::vector<Row>& rows)
{
    RawRow raw[kMostRows];
    const int n = read_rows(window, raw, kMostRows);
    for (int i = 0; i < n; ++i) {
        const RawRow& r = raw[i];
        Row row;
        row.id = r.id;
        row.rect = {r.place[0], r.place[1] + down, r.place[0] + r.place[2], r.place[1] + down + r.place[3]};
        row.text = label_of(r.shape);
        row.state = r.state;
        row.cursor = r.id == cursor;
        row.disabled = r.state == kGreyed;
        row.drawn = r.shown != 0;
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id < b.id; });
}

// ---- watches ----

bool wanted_by(const Watcher& w, const char* name)
{
    return w.all || std::any_of(w.names.begin(), w.names.end(), [name](const Name& n) { return same(n, name); });
}

bool wanted(const char* name)
{
    return std::any_of(g_watchers.begin(), g_watchers.end(), [name](const Watcher& w) { return wanted_by(w, name); });
}

// 'gameui_window' to every addon watching the window
void tell(const Name& name, const char* what)
{
    char raw[16];
    std::memcpy(raw, name.data(), 16);
    const std::string word = word_of(raw);
    for (const Watcher& w : g_watchers)
        if (wanted_by(w, raw))
            addons::queue_window(w.key, word, what);
}

std::uint64_t fnv(std::uint64_t hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// What a draw of the window showed, as one number: its place, cursor and rows as the game has them, and the text and pictures
// the draw made
std::uint64_t signature(const void* window, const Capture& c)
{
    std::uint64_t h = 1469598103934665603ull;
    RawWindow w{};
    if (read_window(static_cast<const char*>(window), w)) {
        const short shown[] = {w.rect[0], w.rect[1], w.rect[2], w.rect[3], w.cursor, w.count, w.down};
        h = fnv(h, shown, sizeof shown);
        h = fnv(h, &w.state, sizeof w.state);
    }
    RawRow rows[kMostRows];
    const int n = read_rows(static_cast<const char*>(window), rows, kMostRows);
    for (int i = 0; i < n; ++i) {
        h = fnv(h, &rows[i].state, sizeof rows[i].state);
        h = fnv(h, rows[i].place, sizeof rows[i].place);
        h = fnv(h, &rows[i].id, sizeof rows[i].id);
        h = fnv(h, &rows[i].shown, sizeof rows[i].shown);
    }
    for (const Glyph& g : c.glyphs) {
        const int at[] = {static_cast<int>(g.x), static_cast<int>(g.y)};
        h = fnv(fnv(fnv(h, g.text.data(), g.text.size()), at, sizeof at), &g.colour, sizeof g.colour);
    }
    for (const Picture& p : c.pictures)
        h = fnv(fnv(fnv(h, p.name.data(), p.name.size()), &p.rect, sizeof p.rect), &p.colour, sizeof p.colour);
    return h;
}

// A watched window's draw done: 'open' the first time it's seen, 'change' when it shows something else
void noted(const void* window, const Capture& c)
{
    const std::uint64_t sig = signature(window, c);
    for (Seen& s : g_seen)
        if (s.window == window && s.name == c.name) {
            if (s.signature != sig) {
                s.signature = sig;
                tell(c.name, "change");
            }
            return;
        }
    g_seen.push_back({window, c.name, sig});
    tell(c.name, "open");
}

} // namespace

std::vector<std::string> names()
{
    std::vector<std::string> out;
    search();
    if (g_manager == nullptr)
        return out;
    const char* windows[kMostNodes];
    const char* current = nullptr;
    const int n = read_list(windows, kMostNodes, current);
    for (int i = 0; i < n; ++i) {
        char name[16];
        if (name_of(windows[i], name))
            out.push_back(word_of(name));
    }
    return out;
}

bool read(const std::string& name, Window& out)
{
    const Name want = layout_name(name);
    watch(want);
    g_ever = true;
    const char* current = nullptr;
    const char* window = find_window(want, current);
    RawWindow w{};
    if (window == nullptr || !read_window(window, w))
        return false;
    out = Window{};
    out.name = word_of(w.name);
    out.rect = {w.rect[0], w.rect[1], w.rect[2], w.rect[3]};
    out.cursor = w.cursor;
    out.count = w.count;
    out.focused = window == current;
    out.closing = w.state == kClosing;
    out.hidden = gameui::veiled(window);
    read_rows_into(window, w.down, w.cursor, out.rows);
    for (const Capture& c : g_captures)
        if (c.window == window && same(c.name, w.name)) {
            out.drawn = true;
            out.lines = lines_of(c.glyphs);
            out.pictures = c.pictures;
            break;
        }
    return true;
}

Press press(const std::string& name, int row, int event)
{
    const char* current = nullptr;
    char* window = const_cast<char*>(find_window(layout_name(name), current));
    RawWindow w{};
    if (window == nullptr || !read_window(window, w) || w.state == kClosing)
        return Press::NotUp;
    if ((row != 0 && g_set_cursor == nullptr) || (event != 0 && g_key_input == nullptr))
        return Press::NotFound;
    if (event != 0 && (w.keys == 0 || w.wait > 0))
        return Press::Busy; // the key input would let the event go (the window isn't taking keys, or not yet)
    if (row != 0) {
        if (row < 1 || row > w.count)
            return Press::NoRow;
        g_set_cursor(window, nullptr, row, 0);
    }
    if (event != 0)
        g_key_input(window, nullptr, event);
    return Press::Done;
}

void watch(const std::string& key, const std::vector<std::string>& names)
{
    g_watchers.erase(std::remove_if(g_watchers.begin(), g_watchers.end(), [&](const Watcher& w) { return w.key == key; }),
                     g_watchers.end());
    if (!names.empty()) {
        Watcher w;
        w.key = key;
        for (const std::string& name : names) {
            if (name == "*")
                w.all = true;
            else
                w.names.push_back(layout_name(name));
        }
        // the windows it watches that are up are 'open' again at their next draw, for it to hear (and its others, again)
        g_seen.erase(std::remove_if(g_seen.begin(), g_seen.end(), [&](const Seen& s) { return wanted_by(w, s.name.data()); }),
                     g_seen.end());
        g_watchers.push_back(std::move(w));
        g_ever = true;
    }
    // windows no one watches any more aren't followed
    g_seen.erase(std::remove_if(g_seen.begin(), g_seen.end(), [](const Seen& s) { return !wanted(s.name.data()); }), g_seen.end());
}

void release(const std::string& key)
{
    watch(key, {});
}

void on_frame()
{
    if (g_seen.empty() || g_manager == nullptr)
        return;
    const char* windows[kMostNodes];
    const char* current = nullptr;
    const int n = read_list(windows, kMostNodes, current);
    for (auto it = g_seen.begin(); it != g_seen.end();) {
        bool up = false;
        for (int i = 0; i < n && !up; ++i) {
            RawWindow w{};
            up = windows[i] == it->window && read_window(windows[i], w) && same(it->name, w.name) && w.state != kClosing;
        }
        if (up) {
            ++it;
            continue;
        }
        const Name name = it->name;
        it = g_seen.erase(it);
        tell(name, "close");
    }
}

// ---- the hooks' ----

Drawing::Drawing(const void* ctrl) : ctrl_(ctrl)
{
    if (ctrl == nullptr)
        return;
    const bool veiled = gameui::veiled(ctrl);
    if (veiled) {
        vanacore_gfx_drop_draws(1);
        dropped_ = true;
    }
    // (a window drawn inside another's: the outer one's)
    if (g_now != nullptr || (g_watch.empty() && g_watchers.empty() && !veiled))
        return;
    char name[16];
    if (!name_of(static_cast<const char*>(ctrl), name) || (!veiled && !watched(name) && !wanted(name)))
        return;
    Capture& c = capture_of(ctrl, name);
    c.at = GetTickCount64();
    c.glyphs.clear();
    c.pictures.clear();
    g_now = &c;
    kept_ = true;
}

Drawing::~Drawing()
{
    if (kept_) {
        if (!g_watchers.empty() && wanted(g_now->name.data()))
            noted(ctrl_, *g_now); // 'open' or 'change' for the addons watching it, heard at Present
        g_now = nullptr;
    }
    if (dropped_)
        vanacore_gfx_drop_draws(0);
}

void letter(int code, int x, int y, float sx, float sy, std::uint32_t colour)
{
    if (g_now == nullptr || g_now->glyphs.size() >= kMostGlyphs)
        return;
    std::string text = sjis_of(code);
    if (text.empty())
        return;
    const int width = chatfont::game_width(code);
    g_now->glyphs.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(width > 0 ? width : 8) * sx,
                             16.0f * sy, std::move(text), colour});
}

void quad(const char* quad, int x, int y, float sx, float sy, std::uint32_t colour)
{
    if (g_now == nullptr || quad == nullptr)
        return;
    short lo[2], hi[2];
    quad_box(quad, lo, hi);
    const float x0 = x + lo[0] * sx, y0 = y + lo[1] * sy, x1 = x + hi[0] * sx, y1 = y + hi[1] * sy;
    const char* name = quad + kQuadName;
    if (std::memcmp(name, kFontSheet, sizeof kFontSheet - 1) == 0) {
        const auto blend = *reinterpret_cast<const std::uint8_t*>(quad + kQuadBlend);
        const auto alpha = *reinterpret_cast<const std::uint8_t*>(quad + kCorners + kAlpha);
        int u = 0, v = 0;
        quad_uv(quad, u, v);
        const char c = chatfont::label_char(u, v);
        if (blend == kShadowBlend || alpha < kFaint || c == 0 || g_now->glyphs.size() >= kMostGlyphs)
            return;
        g_now->glyphs.push_back({x0, y0, x1 - x0, y1 - y0, std::string(1, c), colour});
        return;
    }
    if (std::memcmp(name, kBar, sizeof kBar - 1) == 0 || g_now->pictures.size() >= kMostPictures)
        return;
    std::string named(name, 16);
    while (!named.empty() && (named.back() == ' ' || named.back() == '\0'))
        named.pop_back();
    g_now->pictures.push_back({std::move(named),
                               {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(std::ceil(x1)), static_cast<int>(std::ceil(y1))},
                               colour});
}

bool wants_quads()
{
    return g_ever || gameui::veiling();
}

} // namespace vanacore::gamewin
