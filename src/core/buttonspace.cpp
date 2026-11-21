#include "buttonspace.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "buttons.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::buttonspace {
namespace {

// A game window (CTkMenuCtrlData) is made from its layout by SetResourceData (FUN_10117B40, hudwin.cpp's hook, which tells
// made). Its pieces are a list at +14h (the first node; a node's next at +0, its piece at +10h). A piece's kind is the
// short at +0: 0 the frame, 1 a part drawn from a shape at +4 (a button: a shape with "menu    button" bars, buttons.cpp's).
// A part's rect in its layout's units is at +32h (left, top, right, bottom: shorts, from the window's place), which the
// window's placing (SetLocate, FUN_10118D10) turns into its place and size (FUN_101196D0: +2Ah, +2Ch, +2Eh, +30h, the
// layout's units times the interface's scale): the window draw, the cursor and the mouse all go by those. The frame's
// size is at +22h/+24h, which SetLocate gives the game's resize (FUN_1011A1A0: the window's frame +3Ah..+40h, the frame
// drawn stretched to it) as it places the frame. Read in the code and live 2026-11-05.
//
// So a window is spaced by moving its parts' rects down (each row of buttons by the rows above it, times the spacing, and
// any other part below the first row with them), its frame made taller by as much as the last part moved, and SetLocate at
// its own place: the game's own placing does the rest. Each window's rects as made are kept (from its first draw with
// spacing, once its handler has set it up), so a change of the setting spaces the windows that are up again from them at
// their next draw, and 0 (or the game's style) puts them back. A window laid out over another (+34h flag 1000h: the Action
// Menu, "menu    playermo", and the lists it opens, over the chat, their bottoms on its top) grows up, its bottom kept,
// the rest down. Not an NPC's questions ("menu    query", "menu    gmtell": chat.cpp sizes those to their rows each frame).
// The game's frame wells (its glossy row slots, drawn stretched) are hidden with buttons.style=flat, which spacing goes
// with (buttons.cpp).
constexpr std::ptrdiff_t kWinLayout = 4, kWinPieces = 0x14, kWinFlags = 0x34, kWinFrame = 0x3A;
constexpr std::uint32_t kWinOverAnchor = 0x1000;
constexpr std::ptrdiff_t kLayoutName = 0x46;
constexpr char kQueryName[] = "menu    query   ", kCustomName[] = "menu    gmtell  ";
constexpr std::ptrdiff_t kNodeNext = 0, kNodeItem = 0x10;
constexpr std::ptrdiff_t kKind = 0, kShape = 4, kFrameHeight = 0x24, kRect = 0x32, kHeight = 0x30;
constexpr short kFrameKind = 0, kPartKind = 1;
// A shape: its quads' list (+4: the first node, a node's next at +0, its quad at +10h) and their count (+14h); a quad's
// picture's name at +32h
constexpr std::ptrdiff_t kShapeList = 4, kShapeCount = 0x14, kName = 0x32;
constexpr char kBar[] = "menu    button";
constexpr int kPiecesAtMost = 128, kQuadsAtMost = 64;

// SetLocate (thiscall window, x, y; ret 8), matched past its first 8 bytes (itemlist.cpp's detour) as hudwin.cpp finds it
constexpr char kSetLocate[] = "53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54";
constexpr std::ptrdiff_t kSetLocatePrologue = 8;
using SetLocate = void(__thiscall*)(void* window, int x, int y);

bool g_tried = false;
SetLocate g_set_locate = nullptr;

struct Piece {
    char* at;
    bool button;
    short left, top, right, bottom;
};

// A part kept: where it is, its rect's top and bottom as made, and how many rows of buttons are above it
struct Kept {
    char* at;
    short top, bottom, rows;
};

// A window seen with spacing: its parts below its first row of buttons as made (none: it has no stack of buttons, left
// alone), its frame's height as made, a button's height on the screen to its layout's, whether it grows up, the spacing
// it has now, and the draw it was last seen at (the table's oldest goes when it's full: a window closed)
struct Window {
    void* window;
    unsigned seen;
    bool up;
    int applied, count;
    char* frame;
    short frame_height;
    double scale;
    Kept parts[kPiecesAtMost];
};
constexpr int kWindowsAtMost = 48;
Window g_windows[kWindowsAtMost] = {};
unsigned g_draws = 0;

template <typename T>
T at(const void* base, std::ptrdiff_t offset)
{
    T value{};
    std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
    return value;
}

void put(void* base, std::ptrdiff_t offset, short value)
{
    std::memcpy(static_cast<char*>(base) + offset, &value, sizeof value);
}

void find()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t locate = game != nullptr ? scan::find(game, kSetLocate) : 0;
    if (locate == 0) {
        log::write(L"Buttons: the game's window placing wasn't found in FFXiMain.dll: [client] buttons.spacing does nothing");
        return;
    }
    g_set_locate = reinterpret_cast<SetLocate>(locate - kSetLocatePrologue);
}

// Whether `shape` has a button's bar
bool bars_in(const char* shape)
{
    const int count = at<int>(shape, kShapeCount);
    const char* node = at<const char*>(shape, kShapeList);
    for (int i = 0; node != nullptr && i < count && i < kQuadsAtMost; ++i, node = at<const char*>(node, kNodeNext)) {
        const char* quad = at<const char*>(node, kNodeItem);
        if (quad != nullptr && std::memcmp(quad + kName, kBar, sizeof kBar - 1) == 0)
            return true;
    }
    return false;
}

// The window's parts (kind 1) and its frame; how many parts (-1 if they can't be read)
int parts_of(const void* window, Piece (&parts)[kPiecesAtMost], char*& frame)
{
    __try {
        int count = 0;
        const char* node = at<const char*>(window, kWinPieces);
        for (int i = 0; node != nullptr && i < kPiecesAtMost * 2 && count < kPiecesAtMost; ++i, node = at<const char*>(node, kNodeNext)) {
            char* piece = at<char*>(node, kNodeItem);
            const short kind = piece != nullptr ? at<short>(piece, kKind) : short{-1};
            if (kind == kFrameKind)
                frame = piece;
            if (kind != kPartKind)
                continue;
            const char* shape = at<const char*>(piece, kShape);
            parts[count++] = {piece, shape != nullptr && bars_in(shape), at<short>(piece, kRect), at<short>(piece, kRect + 2),
                              at<short>(piece, kRect + 4), at<short>(piece, kRect + 6)};
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Whether button `b` is in a stack: another button in its column touching it above or below (the page arrows, lone, aren't)
bool stacked(const Piece* parts, int count, const Piece& b)
{
    for (int i = 0; i < count; ++i) {
        const Piece& o = parts[i];
        if (&o != &b && o.button && o.left == b.left && o.right == b.right && (o.top == b.bottom || o.bottom == b.top))
            return true;
    }
    return false;
}

// The stacked buttons' rows (their tops, distinct, in order); how many
int rows_of(const Piece* parts, int count, short (&rows)[kPiecesAtMost])
{
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (parts[i].button && stacked(parts, count, parts[i]) && std::find(rows, rows + n, parts[i].top) == rows + n)
            rows[n++] = parts[i].top;
    std::sort(rows, rows + n);
    return n;
}

// The window as it is now taken as made: its parts at or below its first row of buttons, each with the rows above it, its
// frame's height and its scale (count 0: no stack of buttons, or its parts can't be read)
void keep(void* window, Window& w)
{
    w = {};
    w.window = window;
    Piece parts[kPiecesAtMost];
    char* frame = nullptr;
    const int count = parts_of(window, parts, frame);
    short rows[kPiecesAtMost];
    const int n = count > 0 && frame != nullptr ? rows_of(parts, count, rows) : 0;
    if (n < 2)
        return;
    for (int i = 0; i < count; ++i) {
        const Piece& p = parts[i];
        if (p.button && p.bottom > p.top && w.scale == 0.0)
            w.scale = at<short>(p.at, kHeight) / static_cast<double>(p.bottom - p.top);
        if (p.top < rows[0])
            continue; // above the buttons (the title tab, the page arrows put out of reach)
        const auto above = static_cast<short>(std::count_if(rows, rows + n, [&p](short r) { return r < p.top; }));
        w.parts[w.count++] = {p.at, p.top, p.bottom, above};
    }
    w.frame = frame;
    w.frame_height = at<short>(frame, kFrameHeight);
    if (!(w.scale > 0.0))
        w.count = 0;
}

// Whether the window's parts are still the ones kept (none made again, none gone): written to only then
bool same(const Window& w)
{
    Piece parts[kPiecesAtMost];
    char* frame = nullptr;
    const int count = parts_of(w.window, parts, frame);
    if (count < w.count || frame != w.frame)
        return false;
    for (int i = 0; i < w.count; ++i)
        if (std::none_of(parts, parts + count, [&w, i](const Piece& p) { return p.at == w.parts[i].at; }))
            return false;
    return true;
}

// The window spaced by `spacing` from its rects as made, its frame made as much taller as its last part moved (times its
// scale), and placed again: where it is (VanaCore's hook of the placing, hudwin.cpp's locate, leaves a window put where it
// is alone, where another place would be taken for the game's and moved by the HUD Manager's box again), or up by as much
// as it grew (shrank: down) for one over the chat, its bottom where it was
void space(Window& w, int spacing)
{
    int moved = 0;
    for (int i = 0; i < w.count; ++i) {
        const Kept& p = w.parts[i];
        const int shift = p.rows * spacing;
        put(p.at, kRect + 2, static_cast<short>(p.top + shift));
        put(p.at, kRect + 6, static_cast<short>(p.bottom + shift));
        moved = std::max(moved, shift);
    }
    const int was = at<short>(w.frame, kFrameHeight);
    const int height = w.frame_height + static_cast<int>(std::lround(moved * w.scale));
    put(w.frame, kFrameHeight, static_cast<short>(height));
    w.applied = spacing;
    const int top = at<short>(w.window, kWinFrame + 2);
    g_set_locate(w.window, at<short>(w.window, kWinFrame), w.up ? top + was - height : top);
}

// Whether `window` is an NPC's questions (chat.cpp's), left alone
bool question(const void* window)
{
    const char* layout = at<const char*>(window, kWinLayout);
    return layout != nullptr && (std::memcmp(layout + kLayoutName, kQueryName, 16) == 0 ||
                                 std::memcmp(layout + kLayoutName, kCustomName, 16) == 0);
}

Window* kept_for(const void* window)
{
    for (Window& w : g_windows)
        if (w.window == window)
            return &w;
    return nullptr;
}

// A place in the table: a free one, else the one drawn longest ago
Window& slot()
{
    Window* oldest = &g_windows[0];
    for (Window& w : g_windows) {
        if (w.window == nullptr)
            return w;
        if (w.seen < oldest->seen)
            oldest = &w;
    }
    return *oldest;
}

} // namespace

void made(const void* window)
{
    if (Window* w = kept_for(window); w != nullptr)
        w->window = nullptr; // made again from its layout: as made once more
}

void drawing(void* window)
{
    ++g_draws;
    const int want = buttons::spacing_for(window); // (buttons.spacing with the flat style; the main menu's its own)
    Window* w = window != nullptr ? kept_for(window) : nullptr;
    if (w == nullptr) {
        if (want == 0 || window == nullptr || question(window))
            return;
        w = &slot();
        keep(window, *w);
        w->up = (at<std::uint32_t>(window, kWinFlags) & kWinOverAnchor) != 0;
    }
    w->seen = g_draws;
    if (w->count == 0 || w->applied == want)
        return;
    if (!g_tried)
        find();
    if (g_set_locate == nullptr)
        return;
    if (!same(*w)) {
        w->count = 0; // its parts made again some other way: left as they are
        return;
    }
    space(*w, want);
}

} // namespace vanacore::buttonspace
