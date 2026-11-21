#include "buffwin.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "outline.hpp"
#include "log.hpp"
#include "menuplace.h"
#include "scan.hpp"

namespace vanacore::buffwin {
namespace {

// The status icons are a window of the menu manager (CTkMenuMng, a global in FFXiMain.dll), found as targetwin.cpp finds
// its window: by its layout's name, 16 characters at the layout's +0x46, a window's +4 being its layout. Proven live
// 2026-10-18: SetLocate on it moves the icons, and the place holds across buff changes (a window made again at a zone
// change comes back at the game's place, so it is put once a frame).
constexpr char kLayout[16 + 1] = "menu    buff    ";

// The manager's global and SetLocate: menuplace.h

// The manager: its windows' list at +0 (a node: +0 the next, +0x10 its window, +0x14 set to skip) and the area its windows
// are laid out in (left, top, right, bottom, 16 bits each) at +0x7C
constexpr std::ptrdiff_t kAreaLeft = 0x7C;
// A window: +4 its layout, +0x3A..+0x40 its frame (left, top, right, bottom), +0x42..+0x48 its place as opened, +0x70 to
// be deleted
constexpr std::ptrdiff_t kWinLayout = 4, kLayoutName = 0x46, kWinFrame = 0x3A, kWinOpened = 0x42, kWinDeleting = 0x70;
// The window is a few units tall: its icons are drawn from its corner, 24 units square (measured live), more rows going down
constexpr int kIconHeight = 24;

// The icons' places are the window's buttons': its parts are a list at +0x14 shaped as the manager's (a node: +0 the next,
// +0x10 its part, +0x14 set to skip), a part's kind the short at +0 (1 a button), a button's rect in the window at +0x32
// (left, top, right, bottom, from its layout, which the window's placing, SetLocate, turns into its place at +0x2A) and its
// id at +0x3A (the game's lookup, FUN_10117DD0, reads them so). Seen live 2026-11-06: 36 buttons, ids 1 to 36 in the
// statuses' order, nine to a row 26 units apart (the first 2,2-26,26, the tenth 2,28-26,52) in a window 240 units wide.
// The list draws status n at button n's place and the mouse picks by those places; the cursor keys step the buttons by id
// (next and previous), not by rows. buffs.columns lays the buttons out that many to a row instead, and the game does the rest.
constexpr std::ptrdiff_t kWinParts = 0x14, kButtonRect = 0x32, kButtonId = 0x3A;
constexpr short kButtonKind = 1;
constexpr int kButtons = 36, kGameColumns = 9, kPitch = 26, kGameWidth = 240, kPartsAtMost = 128;

// The window's list (YkWndList, vtable 0x1033A0D0) draws each status with two callbacks its owner gave it (set up by
// FUN_1021ffe0 from FUN_10226030: the icon's at the list's +0x14, the timer's at +0x18; __cdecl list, +2Ch, x, y, status,
// flag), x and y a button's rect as drawn, so the buttons' places are where the grid is. Seen live 2026-10-26:
//   icon  FUN_10226630: the picture drawn by FUN_100268E0 (__thiscall shape: x + 1, y + 1, scale 1.5 (0.75 while the
//         owner's +65h is 1), colour, 0; ret 14h), called after  push 0; push ebx; push eax; mov eax,[esp+28h]; inc ecx;
//         inc eax; push ecx; push eax; mov ecx,edx
//   timer FUN_10226740: its "%dh" / "%dm" / seconds, drawn by FUN_101C7F10 (__cdecl x + 0Eh, y + 14h, the text, colour,
//         0.85, 0.85, 1; the caller cleans up 1Ch) after  add ecx,14h; add edx,0Eh; push 1; push 0.85; push 0.85;
//         push 80808080h; push eax; push ecx; push edx
// buffs.scale redirects those two calls: where they draw is taken from the window's corner by the scale, and their sizes
// multiplied by it. The mouse's pick of a button (0x101188b0) is asked the same way, so a click lands on the icon it's on.
constexpr char kIconCallback[] = "55 56 57 E8 ?? ?? ?? ?? 8B 6C 24 10 8D 04 40 8B CD 8D 34 80 8B 44 24 20 50 C1 E6 02 E8 ?? ?? ?? ?? "
                                 "8B F8 85 FF 0F 84 ?? ?? ?? ?? 53 68 C3 00 00 00 BB 80 80 80 80";
constexpr char kIconCallBefore[] = "6A 00 53 50 8B 44 24 28 41 40 51 50 8B CA"; // the call is the next byte
constexpr char kTimerCallback[] = "83 EC 10 53 56 57 68 C3 00 00 00 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                                  "8B 7C 24 30 8B 5C 24 20 8D 04 40 57 8B CB 8D 34 80";
constexpr char kTimerCallBefore[] = "83 C1 14 83 C2 0E 6A 01 68 9A 99 59 3F 68 9A 99 59 3F 68 80 80 80 80 50 51 52";
constexpr std::size_t kIconCallBeforeSize = 14, kTimerCallBeforeSize = 26, kCallbackSpan = 0x180;

// The square round the status the cursor is on (the button's picture, "menu    corner", drawn in gold at the button's own
// place, and the cursor's own, 34 units across, drawn centred on the button) is not drawn by those callbacks but by the
// window's own draw (FUN_101173D0, drawn inside chat.cpp's ctrl_draw_hook, which tells us the window): its loop over the
// buttons and its cursor, each by the shape draw with a scale (FUN_1011F880, __thiscall shape: x, y, scale x, scale y,
// colour, centred, blending, quads to skip; ret 20h). Centred, it moves x and y back by half of (scale - 1) times the
// shape's width and height (the shorts at the shape's +0x20..+0x26).
// Seen live 2026-10-30: the button's call (at +0x17 of its signature) is after  push 0; push 0; push 0; push ebx; push eax;
// push ecx; movsx eax,[esp+38h]; movsx edx,di; push edx; push eax; mov ecx,[esp+3Ch]  and the cursor's (at +0x1B) after
// push 0; push 0; push 1; movsx ecx,di; push eax; push 1.0; movsx edx,bx; push 1.0; push ecx; push edx; mov ecx,esi
// (the cursor's call is the one all three of the cursor's draws end in). With the scale at 175 and the centred flag left on,
// the square sat up and to the left of its icon; with it cleared on the live game (one byte), it sat on it
constexpr char kButtonShape[] = "6A 00 6A 00 6A 00 53 50 51 0F BF 44 24 38 0F BF D7 52 50 8B 4C 24 3C E8";
constexpr char kCursorShape[] = "6A 00 6A 00 6A 01 0F BF CF 50 68 00 00 80 3F 0F BF D3 68 00 00 80 3F 51 52 8B CE E8";
constexpr std::ptrdiff_t kButtonShapeCall = 23, kCursorShapeCall = 27, kShapeBox = 0x20;

using SetLocate = void(__thiscall*)(void* window, int x, int y);
using ShapeDraw = void(__fastcall*)(void* shape, void* edx, int x, int y, float scale, std::uint32_t colour, int flag);
using TextDraw = void(__cdecl*)(int x, int y, const char* text, std::uint32_t colour, float scale_x, float scale_y, int flag);
using MenuShapeDraw = void(__fastcall*)(void* shape, void* edx, int x, int y, float scale_x, float scale_y, std::uint32_t colour,
                                        int centred, int blending, int skip);

// [client] buffs.*
bool g_move = false;
double g_x = 7.4; // buffs.x: the window's left edge, percent of the interface's width (the game's is 142 of 1920)
double g_y = 6;   // buffs.y: the bottom of the first row of icons, percent of the interface's height (the game's is 72 of 1200)
double g_scale = 1; // buffs.scale: the icons' size against the game's (the setting is a percent)
int g_columns = kGameColumns; // buffs.columns: icons to a row
bool g_arranged = false; // the buttons were laid out other than the game's way, to go back when it's 9 again

bool g_searched = false;
char* g_manager = nullptr;
SetLocate g_set_locate = nullptr;
bool g_placed = false; // the window has been put somewhere, to go back when it's off

bool g_hooked = false; // the two draws are redirected (done the first time a scale is asked for)
ShapeDraw g_shape_game = nullptr;
TextDraw g_text_game = nullptr;
MenuShapeDraw g_menu_shape_game = nullptr;
char* g_window = nullptr; // the window as of the last frame: the draws scale from its corner
const char* g_drawing = nullptr; // the window whose control the game is drawing now (enter_window)

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

using menuplace::number;

// A size in the interface's units as the scale makes it
int scaled(int units) { return static_cast<int>(std::lround(units * g_scale)); }

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

// A call's offset, written in one store (the game's thread runs this code)
bool write_offset(std::uintptr_t call, std::uintptr_t target)
{
    const std::uintptr_t where = call + 1;
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(where), 4, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Buff icons: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(where),
                   GetLastError());
        return false;
    }
    *reinterpret_cast<volatile std::int32_t*>(where) = static_cast<std::int32_t>(target - (call + 5));
    VirtualProtect(reinterpret_cast<void*>(where), 4, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(where), 4);
    return true;
}

// The window's corner the icons are scaled from, if `window` is still the status icons' (a window made again at a zone
// change is a new one; the last frame's can be freed by then)
bool corner(const char* window, int& left, int& top)
{
    __try {
        if (window == nullptr)
            return false;
        const char* layout = *reinterpret_cast<char* const*>(window + kWinLayout);
        if (layout == nullptr || std::memcmp(layout + kLayoutName, kLayout, 16) != 0)
            return false;
        left = *reinterpret_cast<const short*>(window + kWinFrame);
        top = *reinterpret_cast<const short*>(window + kWinFrame + 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A place the game draws at, taken from the window's corner by the scale
int from_corner(int at, int origin) { return origin + static_cast<int>(std::lround((at - origin) * g_scale)); }

void __fastcall shape_hook(void* shape, void* edx, int x, int y, float scale, std::uint32_t colour, int flag)
{
    int left = 0, top = 0;
    if (g_scale != 1.0 && corner(g_window, left, top)) {
        x = from_corner(x, left);
        y = from_corner(y, top);
        scale = static_cast<float>(scale * g_scale);
    }
    g_shape_game(shape, edx, x, y, scale, colour, flag);
}

void __cdecl text_hook(int x, int y, const char* text, std::uint32_t colour, float scale_x, float scale_y, int flag)
{
    int left = 0, top = 0;
    if (g_scale != 1.0 && corner(g_window, left, top)) {
        x = from_corner(x, left);
        y = from_corner(y, top);
        scale_x = static_cast<float>(scale_x * g_scale);
        scale_y = static_cast<float>(scale_y * g_scale);
    }
    g_text_game(x, y, text, colour, scale_x, scale_y, flag);
}

// The width and height the game's centring of a shape goes by (0x1011f880 reads them off the shape)
bool shape_size(const void* shape, int& width, int& height)
{
    __try {
        const short* box = reinterpret_cast<const short*>(static_cast<const char*>(shape) + kShapeBox);
        width = std::abs(box[1] - box[0]);
        height = std::abs(box[3] - box[2]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The status icons' square and cursor: drawn at the button's own place and size, so here too, but only inside the status
// icons' window (the same calls draw every window's buttons and cursor). The cursor's square is drawn centred, which makes
// the game move it back by half of what its scale adds to its size: that is done here, on the game's own place, and the
// square is drawn from its corner like the icons, or it sat up and to the left of the icon it picks
void __fastcall menu_shape_hook(void* shape, void* edx, int x, int y, float scale_x, float scale_y, std::uint32_t colour,
                                int centred, int blending, int skip)
{
    int left = 0, top = 0;
    if (g_scale != 1.0 && g_drawing != nullptr && corner(g_drawing, left, top)) {
        int width = 0, height = 0;
        if (centred != 0 && shape_size(shape, width, height)) {
            x -= static_cast<short>(static_cast<int>(width * (scale_x - 1.0f) * 0.5f));
            y -= static_cast<short>(static_cast<int>(height * (scale_y - 1.0f) * 0.5f));
        }
        centred = 0;
        x = from_corner(x, left);
        y = from_corner(y, top);
        scale_x = static_cast<float>(scale_x * g_scale);
        scale_y = static_cast<float>(scale_y * g_scale);
    }
    g_menu_shape_game(shape, edx, x, y, scale_x, scale_y, colour, centred, blending, skip);
}

// The window's draw's two calls of the shape draw, found by their code and redirected (they pass the game's values through
// outside the status icons' window)
void install_menu_hooks(HMODULE game)
{
    const std::uintptr_t button = scan::find(game, kButtonShape), cursor = scan::find(game, kCursorShape);
    if (button == 0 || cursor == 0 || !is_call(button + kButtonShapeCall) || !is_call(cursor + kCursorShapeCall) ||
        call_target(button + kButtonShapeCall) != call_target(cursor + kCursorShapeCall)) {
        log::write(L"Buff icons: the drawing of their selection square wasn't found in FFXiMain.dll: [client] buffs.scale leaves it");
        return;
    }
    g_menu_shape_game = reinterpret_cast<MenuShapeDraw>(call_target(button + kButtonShapeCall));
    if (write_offset(button + kButtonShapeCall, reinterpret_cast<std::uintptr_t>(&menu_shape_hook)) &&
        write_offset(cursor + kCursorShapeCall, reinterpret_cast<std::uintptr_t>(&menu_shape_hook)))
        log::write(L"Buff icons: the drawing of their selection square hooked for buffs.scale");
}

// The two calls inside the game's drawing of an icon and of its timer, found by their code and redirected (once; they
// pass the game's own values through while the scale is 1)
void install_hooks(HMODULE game)
{
    g_hooked = true;
    install_menu_hooks(game);
    const std::uintptr_t icon = scan::find(game, kIconCallback), timer = scan::find(game, kTimerCallback);
    const std::uintptr_t icon_call = icon != 0 ? scan::find(icon, kCallbackSpan, kIconCallBefore) : 0;
    const std::uintptr_t timer_call = timer != 0 ? scan::find(timer, kCallbackSpan, kTimerCallBefore) : 0;
    if (icon_call == 0 || timer_call == 0 || !is_call(icon_call + kIconCallBeforeSize) ||
        !is_call(timer_call + kTimerCallBeforeSize)) {
        log::write(L"Buff icons: their drawing wasn't found in FFXiMain.dll: [client] buffs.scale does nothing");
        return;
    }
    const std::uintptr_t shape = icon_call + kIconCallBeforeSize, text = timer_call + kTimerCallBeforeSize;
    g_shape_game = reinterpret_cast<ShapeDraw>(call_target(shape));
    g_text_game = reinterpret_cast<TextDraw>(call_target(text));
    if (write_offset(shape, reinterpret_cast<std::uintptr_t>(&shape_hook)) &&
        write_offset(text, reinterpret_cast<std::uintptr_t>(&text_hook)))
        log::write(L"Buff icons: their drawing hooked for buffs.scale");
}

bool found()
{
    return menuplace::find(g_searched, g_manager, g_set_locate,
                           L"Buff icons: the game's window placing wasn't found in FFXiMain.dll: [client] buffs.* does nothing");
}

// The status icons' window in the manager's list, or null
char* find_window()
{
    int count = 0;
    for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 512; node = at<char*>(node, 0), ++count) {
        char* window = at<char*>(node, 0x10);
        if (window == nullptr || at<std::uint8_t>(node, 0x14) != 0 || at<std::uint8_t>(window, kWinDeleting) != 0)
            continue;
        const char* layout = at<char*>(window, kWinLayout);
        if (layout != nullptr && std::memcmp(layout + kLayoutName, kLayout, 16) == 0)
            return window;
    }
    return nullptr;
}

// The row's width in the interface's units for a window `frame_width` wide as the game made it (nine to a row): wider or
// narrower by a pitch for each icon more or fewer that buffs.columns puts in a row
int row_width(int frame_width)
{
    return frame_width + (g_columns - kGameColumns) * kPitch;
}

// The window's buttons by id (1 to kButtons; the rest null): how many there are
int buttons_of(char* window, char* (&buttons)[kButtons + 1])
{
    int found = 0, count = 0;
    for (char* node = at<char*>(window, kWinParts); node != nullptr && count < kPartsAtMost; node = at<char*>(node, 0), ++count) {
        char* part = at<char*>(node, 0x10);
        if (part == nullptr || at<std::uint8_t>(node, 0x14) != 0 || at<short>(part, 0) != kButtonKind)
            continue;
        const int id = at<short>(part, kButtonId);
        if (id >= 1 && id <= kButtons && buttons[id] == nullptr) {
            buttons[id] = part;
            ++found;
        }
    }
    return found;
}

// buffs.columns: the buttons laid out g_columns to a row from the first one's place, a pitch apart (the game's own
// spacing), their rects rewritten where they differ and the window placed again where it is, which puts each button from
// its rect. 9 is the game's own layout, so nothing moves then.
void arrange(char* window)
{
    char* buttons[kButtons + 1] = {};
    if (buttons_of(window, buttons) == 0 || buttons[1] == nullptr)
        return;
    const short* first = &at<short>(buttons[1], kButtonRect);
    const int left = first[0], top = first[1], width = first[2] - first[0], height = first[3] - first[1];
    bool moved = false;
    for (int id = 2; id <= kButtons; ++id) {
        if (buttons[id] == nullptr)
            continue;
        short* rect = &at<short>(buttons[id], kButtonRect);
        const auto x = static_cast<short>(left + (id - 1) % g_columns * kPitch);
        const auto y = static_cast<short>(top + (id - 1) / g_columns * kPitch);
        if (rect[0] == x && rect[1] == y && rect[2] == x + width && rect[3] == y + height)
            continue;
        rect[0] = x;
        rect[1] = y;
        rect[2] = static_cast<short>(x + width);
        rect[3] = static_cast<short>(y + height);
        moved = true;
    }
    if (moved)
        g_set_locate(window, at<short>(window, kWinFrame), at<short>(window, kWinFrame + 2));
    g_arranged = g_columns != kGameColumns;
}

// Where a window `width` wide goes for X and Y (percents of the area), its top-left corner; false if the area is empty
bool wanted(double x, double y, int width, int& want_x, int& want_y)
{
    const int area_left = at<short>(g_manager, kAreaLeft), area_top = at<short>(g_manager, kAreaLeft + 2);
    const int area_w = at<short>(g_manager, kAreaLeft + 4) - area_left, area_h = at<short>(g_manager, kAreaLeft + 6) - area_top;
    if (area_w <= 0 || area_h <= 0 || width <= 0)
        return false;
    want_x = area_left + static_cast<int>(std::lround(area_w * x / 100.0));
    want_y = area_top + static_cast<int>(std::lround(area_h * y / 100.0)) - scaled(kIconHeight); // the window's top
    want_x = std::clamp(want_x, area_left, std::max(area_left, area_left + area_w - width));
    want_y = std::clamp(want_y, area_top, std::max(area_top, area_top + area_h - scaled(kIconHeight)));
    return true;
}

void place(char* window)
{
    if (!g_move && !g_placed)
        return;
    const int left = at<short>(window, kWinFrame), top = at<short>(window, kWinFrame + 2);
    const int width = scaled(row_width(at<short>(window, kWinFrame + 4) - left)); // the row as wide as it's drawn
    int want_x = at<short>(window, kWinOpened), want_y = at<short>(window, kWinOpened + 2); // the game's own place
    if (g_move && !wanted(g_x, g_y, width, want_x, want_y))
        return;
    g_placed = g_move;
    if (left != want_x || top != want_y)
        g_set_locate(window, want_x, want_y);
}

// The first row of icons' left, top and width in the interface's units: the window's own frame where it's found, else
// where buffs.x and buffs.y (or the game's place, the defaults) put it
bool icons_at(int& left, int& top, int& width)
{
    __try {
        if (char* window = find_window()) {
            left = at<short>(window, kWinFrame);
            top = at<short>(window, kWinFrame + 2);
            width = at<short>(window, kWinFrame + 4) - left;
            if (width < kIconHeight)
                width = kGameWidth;
            width = scaled(row_width(width));
            return true;
        }
        width = scaled(row_width(kGameWidth));
        return wanted(g_move ? g_x : 7.4, g_move ? g_y : 6, width, left, top);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void reload(const wchar_t* ini)
{
    g_move = GetPrivateProfileIntW(L"client", L"buffs.move", 0, ini) != 0;
    g_x = number(L"buffs.x", 7.4, 0, 100, ini);
    g_y = number(L"buffs.y", 6, 0, 100, ini);
    g_scale = number(L"buffs.scale", 100, 25, 300, ini) / 100.0;
    g_columns = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", L"buffs.columns", kGameColumns, ini)), 1, kButtons);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    const bool scale = g_scale != 1.0;
    const bool columns = g_columns != kGameColumns || g_arranged;
    if (!g_move && !g_placed && !scale && !columns && g_window == nullptr)
        return;
    if (!found() || g_manager == nullptr)
        return;
    if (scale && !g_hooked)
        install_hooks(GetModuleHandleW(L"FFXiMain.dll"));
    __try {
        char* window = find_window();
        g_window = scale ? window : nullptr;
        if (window != nullptr) {
            place(window);
            if (columns)
                arrange(window);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_window = nullptr;
    }
}

const void* enter_window(const void* ctrl)
{
    const void* outer = g_drawing;
    g_drawing = static_cast<const char*>(ctrl);
    return outer;
}

void leave_window(const void* outer)
{
    g_drawing = static_cast<const char*>(outer);
}

bool map_point(const void* window, const int* point, int mapped[2])
{
    int left = 0, top = 0;
    if (g_scale == 1.0 || point == nullptr || window == nullptr || window != g_window ||
        !corner(static_cast<const char*>(window), left, top))
        return false;
    mapped[0] = left + static_cast<int>(std::lround((point[0] - left) / g_scale));
    mapped[1] = top + static_cast<int>(std::lround((point[1] - top) / g_scale));
    return true;
}

bool outline(double* moved_x, double* moved_y)
{
    if (!found() || g_manager == nullptr)
        return false;
    int left = 0, top = 0, width = 0;
    if (!icons_at(left, top, width))
        return false;
    outline::Drag drag;
    if (!outline::draw("buffs", {left, top, left + width, top + scaled(kIconHeight)}, g_columns, drag))
        return false;
    // dragged: the icons go where the box is taken to (buffs.move on) at once; the caller saves them when it's let go
    if (drag.held) {
        const int area_left = at<short>(g_manager, kAreaLeft), area_top = at<short>(g_manager, kAreaLeft + 2);
        const int area_w = at<short>(g_manager, kAreaLeft + 4) - area_left, area_h = at<short>(g_manager, kAreaLeft + 6) - area_top;
        if (area_w > 0 && area_h > 0) {
            g_move = true;
            g_x = std::clamp(std::round((drag.left - area_left) * 10000.0 / area_w) / 100.0, 0.0, 100.0);
            g_y = std::clamp(std::round((drag.top + scaled(kIconHeight) - area_top) * 10000.0 / area_h) / 100.0, 0.0, 100.0); // the row's bottom
        }
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = g_x;
        *moved_y = g_y;
    }
    return true;
}

} // namespace vanacore::buffwin
