#include "castbar.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "hudwin.hpp"
#include "log.hpp"
#include "menuplace.h"
#include "outline.hpp"
#include "scan.hpp"

namespace vanacore::castbar {
namespace {

// The cast bar is a window of the menu manager like the party list's, with a layout named in the game's data
// ("menu    casttime", 16 characters at the layout's +0x46, a window's +4 being its layout). Left out of the window draw,
// it isn't seen at all, the frame a cast opens it in included (a hide that clears its flags after the frame shows it
// for one). The game makes the window again for each cast, at its own place.
constexpr char kLayout[16 + 1] = "menu    casttime";
constexpr std::ptrdiff_t kWindowLayout = 4;
constexpr std::ptrdiff_t kLayoutName = 0x46;

// The manager and SetLocate are found as the status icons' are (menuplace.h). The manager: the area its windows are laid out in (left, top, right, bottom, 16 bits each) at +0x7C. A window:
// +0x3A..+0x40 its frame (left, top, right, bottom). (Its place as opened, +0x42, isn't where the game shows the cast bar:
// 16,94 against a frame at 16,47, seen live 2026-10-20; the game's place is the frame a new window is first drawn at.)
constexpr std::ptrdiff_t kAreaLeft = 0x7C;
constexpr std::ptrdiff_t kWinFrame = 0x3A;

// castbar.x / castbar.y's defaults: the game's own place, 16,47 to 144,71 of a 1920 x 1200 interface (the top left)
constexpr double kDefaultX = 4.2, kDefaultY = 3.9;

using SetLocate = void(__thiscall*)(void* window, int x, int y);

// castbar.background=0: the window's +0x69 cleared before its draw, which then leaves out the layout's pieces (its frame)
// and still calls its owner's draw (the gauge, the percent and its "%": CTkCastTime::OnDrawPrimitive in the PS2 client),
// as alliancelist.cpp's party.background does. Each cast's window is a new one, made with the flag set.
constexpr std::ptrdiff_t kWinPieces = 0x69;

// [client] castbar.*
bool g_hide = false;
bool g_hide_logged = false;
bool g_frame = true;
const char* g_bare = nullptr; // the window whose flag VanaCore cleared
bool g_frame_logged = false;
bool g_move = false;
double g_x = kDefaultX; // castbar.x: the bar's middle, percent of the interface's width
double g_y = kDefaultY; // castbar.y: its top, percent of the interface's height

bool g_searched = false;
char* g_manager = nullptr;
SetLocate g_set_locate = nullptr;
bool g_placed = false; // the bar has been put somewhere, to go back when it's off

// The bar as last seen (for its box while no cast is on): its size, and where the game put it (the frame the window had
// when first drawn, before it's moved)
int g_seen_w = 128, g_seen_h = 24;
bool g_seen_game = false;
int g_game_left = 0, g_game_top = 0;
const char* g_window = nullptr;

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

using menuplace::number;

bool found()
{
    return menuplace::find(g_searched, g_manager, g_set_locate,
                           L"Cast bar: the game's window placing wasn't found in FFXiMain.dll: [client] castbar.move does nothing");
}

// Where a bar `width` x `height` goes for X (its middle) and Y (its top), percents of the area: its top-left corner; false
// if the area is empty
bool wanted(double x, double y, int width, int height, int& want_x, int& want_y)
{
    const int area_left = at<short>(g_manager, kAreaLeft), area_top = at<short>(g_manager, kAreaLeft + 2);
    const int area_w = at<short>(g_manager, kAreaLeft + 4) - area_left, area_h = at<short>(g_manager, kAreaLeft + 6) - area_top;
    if (area_w <= 0 || area_h <= 0 || width <= 0)
        return false;
    want_x = area_left + static_cast<int>(std::lround(area_w * x / 100.0)) - width / 2;
    want_y = area_top + static_cast<int>(std::lround(area_h * y / 100.0));
    want_x = std::clamp(want_x, area_left, std::max(area_left, area_left + area_w - width));
    want_y = std::clamp(want_y, area_top, std::max(area_top, area_top + area_h - height));
    return true;
}

// The bar, before it's drawn: seen (its size and the game's place, for the box) and put where castbar.move says
void place(char* window)
{
    const int left = at<short>(window, kWinFrame), top = at<short>(window, kWinFrame + 2);
    const int width = at<short>(window, kWinFrame + 4) - left, height = at<short>(window, kWinFrame + 6) - top;
    if (width <= 0 || height <= 0)
        return;
    g_seen_w = width, g_seen_h = height;
    if (window != g_window) { // a new cast's window, where the game puts it
        g_window = window;
        g_game_left = left, g_game_top = top;
        g_seen_game = true;
    }
    if (!g_move && !g_placed)
        return;
    if (!found() || g_manager == nullptr)
        return;
    int want_x = g_game_left, want_y = g_game_top;
    if (g_move && !wanted(g_x, g_y, width, height, want_x, want_y))
        return;
    g_placed = g_move;
    if (left != want_x || top != want_y)
        g_set_locate(window, want_x, want_y);
}

// The bar's frame drawn or not, as castbar.background says
void frame(char* window)
{
    std::uint8_t& pieces = at<std::uint8_t>(window, kWinPieces);
    if (g_frame) {
        if (g_bare == window)
            pieces = 1;
        g_bare = nullptr;
        return;
    }
    if (pieces == 0)
        return;
    pieces = 0;
    g_bare = window;
    if (!g_frame_logged) {
        g_frame_logged = true;
        log::write(L"Cast bar: drawn without its frame ([client] castbar.background=0)");
    }
}

} // namespace

void reload(const wchar_t* ini)
{
    g_hide = GetPrivateProfileIntW(L"client", L"castbar.hide", 0, ini) != 0;
    g_frame = GetPrivateProfileIntW(L"client", L"castbar.background", 1, ini) != 0;
    g_move = GetPrivateProfileIntW(L"client", L"castbar.move", 0, ini) != 0;
    g_x = number(L"castbar.x", kDefaultX, 0, 100, ini);
    g_y = number(L"castbar.y", kDefaultY, 0, 100, ini);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

bool skip(const void* ctrl)
{
    if (ctrl == nullptr)
        return false;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWindowLayout);
        if (layout == nullptr || std::memcmp(layout + kLayoutName, kLayout, 16) != 0)
            return false;
        place(static_cast<char*>(const_cast<void*>(ctrl)));
        frame(static_cast<char*>(const_cast<void*>(ctrl)));
        if (!g_hide)
            return false;
        if (!g_hide_logged) {
            g_hide_logged = true;
            log::write(L"Cast bar: left out of the window draw ([client] castbar.hide=1)");
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool outline(double* moved_x, double* moved_y)
{
    if (!found() || g_manager == nullptr)
        return false;
    const int area_left = at<short>(g_manager, kAreaLeft), area_top = at<short>(g_manager, kAreaLeft + 2);
    const int area_w = at<short>(g_manager, kAreaLeft + 4) - area_left, area_h = at<short>(g_manager, kAreaLeft + 6) - area_top;
    if (area_w <= 0 || area_h <= 0)
        return false;
    // where castbar.x and .y put it (or the game, as it last opened it), at the size it last had
    outline::Box box;
    if (!g_move && g_seen_game) {
        box.left = g_game_left;
        box.top = g_game_top;
    } else if (!wanted(g_move ? g_x : kDefaultX, g_move ? g_y : kDefaultY, g_seen_w, g_seen_h, box.left, box.top)) {
        return false;
    }
    // castbar.scale (hudwin_scale.h): as big as the bar is drawn, about its middle top
    const double scale = hudwin::scale("castbar");
    const int shown_w = static_cast<int>(std::lround(g_seen_w * scale)), shown_h = static_cast<int>(std::lround(g_seen_h * scale));
    box.left += (g_seen_w - shown_w) / 2;
    box.right = box.left + shown_w;
    box.bottom = box.top + shown_h;
    outline::Drag drag;
    if (!outline::draw("castbar", box, 0, drag))
        return false;
    // dragged: the bar goes where the box is taken to (castbar.move on) at once
    if (drag.held) {
        g_move = true;
        g_x = std::clamp(std::round((drag.left + shown_w / 2.0 - area_left) * 10000.0 / area_w) / 100.0, 0.0, 100.0);
        g_y = std::clamp(std::round((drag.top - area_top) * 10000.0 / area_h) / 100.0, 0.0, 100.0);
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = g_x;
        *moved_y = g_y;
    }
    return true;
}

} // namespace vanacore::castbar
