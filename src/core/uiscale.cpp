#include "uiscale.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstddef>
#include <cstdint>
#include <string>

#include "graphics.hpp"
#include "log.hpp"
#include "registry.hpp"
#include "scan.hpp"
#include "ui.hpp"
#include "window.hpp"

namespace vanacore::uiscale {
namespace {

// The game's display set up at its start (FFXiMain FUN_10010c00, once, 2025-12-26): the scene object (Ashita's
// "CYmdb", which libs\scaling reads) takes the back buffer's size from the graphics object (+0x15c, +0x160) at +0x10,
// and the menus' from 0037 x 0038 at +0x14:
// mov ax,[edx+15Ch]; mov [esi+10h],ax; mov ecx,[gfx]; mov dx,[ecx+160h]; mov [esi+12h],dx; mov ax,[0037];
// mov [esi+14h],ax; mov cx,[0038]
constexpr char kDisplay[] = "66 8B 82 5C 01 00 00 66 89 46 10 8B 0D ?? ?? ?? ?? 66 8B 91 60 01 00 00 66 89 56 12 66 A1 ?? ?? ?? ?? "
                            "66 89 46 14 66 8B 0D ?? ?? ?? ??";
constexpr std::ptrdiff_t kGfxAt = 0xD, kMenuWidthAt = 0x1E, kMenuHeightAt = 0x29;
// then the menu target, when the menus' size isn't the back buffer's: a render-target texture (thiscall gfx: width,
// height, out, usage 1, pool 0) at gfx+0x1e0, and a depth surface of its size (thiscall gfx: width, height) at +0x1e8.
// Each frame the interface is drawn into it while both are there, and the texture's surface is asked for afresh
// (GetSurfaceLevel, SetRenderTarget, Release: FUN_10009e00), so they can be made again between frames.
constexpr char kTarget[] = "53 8D 54 24 38 6A 01 52 51 8B 0D ?? ?? ?? ?? 50 89 5C 24 48 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? "
                           "89 81 E0 01 00 00 8B 15 ?? ?? ?? ?? 8B 44 24 34 89 82 E4 01 00 00 66 8B 46 16 66 8B 4E 14 50 51 "
                           "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 89 81 E8 01 00 00";
constexpr std::ptrdiff_t kTargetGfxAt = 0xB, kCreateTargetAt = 0x14, kCreateDepthAt = 0x45;
// The scene object's global, as Ashita's libs\scaling finds it: mov eax,[scene]; test eax,eax; pop esi; je; ...
constexpr char kScene[] = "A1 ?? ?? ?? ?? 85 C0 5E 74 ?? ?? 80";
// The window that has the help line's text (FUN_101182a0): push ebx; push ebp; push esi; mov ebp,ecx; push edi;
// mov ecx,<the menu manager>; call; cmp eax,ebp; jne; ...; at +0x63 mov ecx,[the help line]; test ecx,ecx
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8";
constexpr std::ptrdiff_t kManagerAt = 7, kHelpLineAt = 0x63;
// A window put at (x, y), its parts with it, its handler told (SetLocate, FUN_10118d10), which calls its resize
// (FUN_1011a1a0: thiscall window, x, y, width, height, 1, 0, 0) at +0x6c. Its first 8 bytes (sub esp,0Ch;
// mov ax,[esp+14h]) are itemlist.cpp's detour prologue, a jmp into VanaCore by the time this looks (after the first
// Present): matched from the push ebx after them, the game's own code whatever VanaCore put before it.
constexpr char kSetLocate[] = "53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54";
constexpr std::ptrdiff_t kSetLocatePrologue = 8, kResizeAt = 0x6C;
// The Config's chat width callbacks: the log's +0x5c with the setting, then its layout (FUN_10161850, which chat.cpp
// detours for chat.move): mov ecx,[log 1]; mov edx,[esp+0Ch]; push edx; mov eax,[ecx]; call [eax+5Ch]; mov ecx,[log 1];
// call <layout>. Log 2's the same with push esi.
constexpr char kLog1[] = "8B 0D ?? ?? ?? ?? 8B 54 24 0C 52 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8";
constexpr char kLog2[] = "8B 0D ?? ?? ?? ?? 56 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8";
constexpr std::ptrdiff_t kLog1LayoutAt = 0x16, kLog2LayoutAt = 0x12;
// ... and log 2's width as log 1's: push 9Ch; call <a Config setting>; push eax; push 0C5h; call <set>
constexpr char kConfig[] = "68 9C 00 00 00 E8 ?? ?? ?? ?? 50 68 C5 00 00 00 E8";
constexpr std::ptrdiff_t kConfigGetAt = 5;
constexpr int kLogWidthSetting[2] = {0x9C, 0xC5};
// A chat log's clip (FUN_10160ad0, called as it draws its lines): the viewport the interface is drawn with kept in a copy
// (x, y, width, height, min z, max z) and the clip set from it: mov ecx,6; mov esi,eax; mov edi,<copy>; rep movsd;
// mov ecx,6; mov esi,<copy>; mov edi,<clip>; rep movsd. Addons read the menus' size there (fancychat: its width and height).
constexpr char kViewport[] = "B9 06 00 00 00 8B F0 BF ?? ?? ?? ?? F3 A5 B9 06 00 00 00 BE ?? ?? ?? ?? BF ?? ?? ?? ?? F3 A5";
constexpr std::ptrdiff_t kViewportAt = 8;

// The graphics object
constexpr std::ptrdiff_t kGfxTarget = 0x1E0, kGfxTargetDepth = 0x1E8;
// The scene object: the back buffer's size, then the menus' (16 bits each)
constexpr std::ptrdiff_t kSceneBack = 0x10, kSceneMenus = 0x14;
// The menu manager (CTkMenuMng, 0x105ECB98): its windows' list at +0 (a node: +0 the next, +0x10 its window, +0x14 set to
// skip), the area its windows are laid out in (left, top, right, bottom, 16 bits each) at +0x7c (uispace.cpp's), and the
// last tops of the five anchors it places windows over at +0xa0 (chat.cpp's replace_menus)
constexpr std::ptrdiff_t kAreaLeft = 0x7C, kAreaRight = 0x80, kAnchorTops = 0xA0;
// A window: +4 its layout (+8 the parsed layout, whose first is its block: +0x13 where it's anchored, 1 the right edge,
// 2 the bottom, 3 both; +0xa its width), +0x3a..+0x40 its frame, +0x42..+0x48 its place as opened, +0x52 and +0x54
// where it is, +0x70 to be deleted, +0x99 laid out in proportion to the area (the lobby's backgrounds)
constexpr std::ptrdiff_t kWinLayout = 0x4, kWinFrame = 0x3A, kWinOpened = 0x42, kWinX = 0x52, kWinY = 0x54,
                         kWinDeleting = 0x70, kWinScaled = 0x99;

using CreateTarget = void*(__thiscall*)(void* gfx, int width, int height, void* out, int usage, int pool);
using CreateDepth = void*(__thiscall*)(void* gfx, int width, int height);
using SetLocate = void(__thiscall*)(void* window, int x, int y);
using Resize = void(__thiscall*)(void* window, int x, int y, int width, int height, int a, int b, int c);
using LogWidth = void(__thiscall*)(void* log, int value);
using LogLayout = void(__thiscall*)(void* log);
using ConfigGet = int(__cdecl*)(int id);
using Release = unsigned long(__stdcall*)(void* object);

std::wstring g_text;    // [ffxi] resolution.ui as last applied (or as the game started with it)
int g_share = 0;        // its window-<share> (registry::window_share), 0 for any other
bool g_retry = false;   // its menu target couldn't be made: the same text again tries again
bool g_pending = false; // another one to apply after the next Present
ULONGLONG g_fit_checked = 0;             // window-<share>: when its size was last worked out (GetTickCount64)
long g_fit_seen_w = 0, g_fit_seen_h = 0; // ... and what it came to
bool g_resized_menus = false;            // unset: the menus were last made the resized window's size (unset_size)
bool g_searched = false;
char** g_gfx = nullptr;
char** g_scene = nullptr;
std::uint32_t* g_menu_width = nullptr; // 0037 and 0038 as the game read them
std::uint32_t* g_menu_height = nullptr;
std::uint32_t* g_viewport = nullptr; // the chat logs' copy of the interface's viewport (kViewport)
char* g_manager = nullptr;
char** g_help = nullptr;
char** g_logs[2] = {};
CreateTarget g_create_target = nullptr;
CreateDepth g_create_depth = nullptr;
SetLocate g_set_locate = nullptr;
Resize g_resize = nullptr;
LogLayout g_log_layout = nullptr;
ConfigGet g_config_get = nullptr;

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

template <typename T> T operand(std::uintptr_t site, std::ptrdiff_t offset)
{
    return *reinterpret_cast<const T*>(site + offset);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    return call + 5 + *reinterpret_cast<const std::int32_t*>(call + 1);
}

std::wstring read_text(const wchar_t* ini)
{
    wchar_t value[64] = {};
    GetPrivateProfileStringW(L"ffxi", L"resolution.ui", L"", value, 64, ini);
    return value;
}

bool found()
{
    if (g_searched)
        return g_create_target != nullptr;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const std::uintptr_t display = scan::find(game, kDisplay);
    const std::uintptr_t target = scan::find(game, kTarget);
    const std::uintptr_t scene = scan::find(game, kScene);
    const std::uintptr_t help = scan::find(game, kHelp);
    if (display == 0 || target == 0 || scene == 0 || help == 0 ||
        operand<char**>(display, kGfxAt) != operand<char**>(target, kTargetGfxAt) ||
        *reinterpret_cast<const std::uint8_t*>(target + kCreateDepthAt) != 0xE8) {
        log::write(L"Interface: the game's menu target wasn't found in FFXiMain.dll: [ffxi] resolution.ui applies from the next "
                   L"start");
        return false;
    }
    g_gfx = operand<char**>(display, kGfxAt);
    g_menu_width = operand<std::uint32_t*>(display, kMenuWidthAt);
    g_menu_height = operand<std::uint32_t*>(display, kMenuHeightAt);
    g_scene = operand<char**>(scene, 1);
    g_manager = operand<char*>(help, kManagerAt);
    g_create_target = reinterpret_cast<CreateTarget>(call_target(target + kCreateTargetAt));
    g_create_depth = reinterpret_cast<CreateDepth>(call_target(target + kCreateDepthAt));
    // what keeps the open windows in place; without them the menus' size still changes
    if (const auto* line = reinterpret_cast<const std::uint8_t*>(help + kHelpLineAt); line[0] == 0x8B && line[1] == 0x0D)
        g_help = operand<char**>(help, kHelpLineAt + 2);
    // (the function from its start: its own prologue there, or a detour's jmp)
    if (const std::uintptr_t match = scan::find(game, kSetLocate); match != 0) {
        const std::uintptr_t locate = match - kSetLocatePrologue;
        const auto* start = reinterpret_cast<const std::uint8_t*>(locate);
        if ((start[0] == 0x83 || start[0] == 0xE9) && start[kResizeAt] == 0xE8) {
            g_set_locate = reinterpret_cast<SetLocate>(locate);
            g_resize = reinterpret_cast<Resize>(call_target(locate + kResizeAt));
        }
    }
    const std::uintptr_t log1 = scan::find(game, kLog1), log2 = scan::find(game, kLog2), config = scan::find(game, kConfig);
    if (log1 != 0 && config != 0) {
        g_logs[0] = operand<char**>(log1, 2);
        g_log_layout = reinterpret_cast<LogLayout>(call_target(log1 + kLog1LayoutAt));
        g_config_get = reinterpret_cast<ConfigGet>(call_target(config + kConfigGetAt));
        if (log2 != 0 && call_target(log2 + kLog2LayoutAt) == reinterpret_cast<std::uintptr_t>(g_log_layout))
            g_logs[1] = operand<char**>(log2, 2);
    }
    if (const std::uintptr_t viewport = scan::find(game, kViewport); viewport != 0)
        g_viewport = operand<std::uint32_t*>(viewport, kViewportAt);
    if (g_set_locate == nullptr || g_log_layout == nullptr)
        log::write(L"Interface: the game's window placing wasn't all found in FFXiMain.dll: with a new [ffxi] resolution.ui "
                   L"open windows may sit away from their edges until they open again");
    return true;
}

void release(void* object)
{
    if (object != nullptr)
        reinterpret_cast<Release>((*static_cast<void***>(object))[2])(object);
}

// Chat log `index`'s window (CTkMenuCtrlData), with the log (CTkDrawMessageWindow) in `log`; null while it isn't shown,
// or its +8 points at memory another window has now (+0xc the window's handler: chat.cpp's chat_ctrl checks the same)
char* log_window(int index, char*& log)
{
    log = g_logs[index] != nullptr ? *g_logs[index] : nullptr;
    char* window = log != nullptr ? at<char*>(log, 8) : nullptr;
    return window != nullptr && at<char*>(window, 0xC) == log ? window : nullptr;
}

// The chat logs lay themselves out from the area, as the Config's chat width settings do it. That layout (FUN_10161850,
// chat.cpp's chat.move after it) makes a log as tall as its place as opened, 8 lines, for its calc (FUN_101611a0) to ease
// it to the lines it shows over the next frames: put back to `heights` (each log's before, -1 unknown) at its new bottom
// instead, so it doesn't open and close again. A log fancychat keeps closed read as the player opening it (its top, the
// log's +0x42, gone up) and fancychat's chat windows hid.
void logs_follow(const int heights[2])
{
    for (int i = 0; i < 2 && g_log_layout != nullptr; ++i) {
        char* log = nullptr;
        char* window = log_window(i, log);
        if (window == nullptr)
            continue;
        const int width = g_config_get(kLogWidthSetting[i]); // -1 with no Config yet: the width left as it is
        if (width >= 0)
            reinterpret_cast<LogWidth>((*reinterpret_cast<void***>(log))[0x5C / 4])(log, width);
        g_log_layout(log);
        if (heights[i] <= 0 || g_resize == nullptr)
            continue;
        const int left = at<short>(window, kWinFrame), right = at<short>(window, kWinFrame + 4);
        const int bottom = at<short>(window, kWinFrame + 6);
        if (bottom - at<short>(window, kWinFrame + 2) != heights[i])
            g_resize(window, left, bottom - heights[i], right - left, heights[i], 1, 0, 0);
    }
}

// The open windows kept where they were in an area now w1 x h1, w0 x h0 before: each as its layout anchors it (moved by
// the area's growth at the edges it's anchored to), one centred as it opened centred again, the lobby's laid out in
// proportion. Its place as opened moves with it, as the game measures from it.
void windows_follow(int w0, int h0, int w1, int h1)
{
    const int dw = w1 - w0, dh = h1 - h0;
    int count = 0;
    // the chat logs' heights before anything moves them
    int log_heights[2] = {-1, -1};
    for (int i = 0; i < 2; ++i) {
        char* log = nullptr;
        if (char* window = log_window(i, log); window != nullptr)
            log_heights[i] = at<short>(window, kWinFrame + 6) - at<short>(window, kWinFrame + 2);
    }
    // (the windows only with SetLocate found; the logs, the help line and the anchors' tops whatever)
    for (char* node = g_set_locate != nullptr ? at<char*>(g_manager, 0) : nullptr; node != nullptr && count < 512;
         node = at<char*>(node, 0), ++count) {
        char* window = at<char*>(node, 0x10);
        if (window == nullptr || at<std::uint8_t>(node, 0x14) != 0 || at<std::uint8_t>(window, kWinDeleting) != 0)
            continue;
        char* layout = at<char*>(window, kWinLayout);
        char* parsed = layout != nullptr ? at<char*>(layout, 8) : nullptr;
        char* block = parsed != nullptr ? at<char*>(parsed, 0) : nullptr;
        if (block == nullptr)
            continue;
        const int anchor = at<std::uint8_t>(block, 0x13);
        const int x = at<short>(window, kWinX), y = at<short>(window, kWinY);
        // where its frame is: the game centres a window (FUN_1011a8c0 and the like) with its resize, which moves the frame
        // alone, never SetLocate's place (+0x52) - that keeps the place it opened at, so the centring is told by the frame
        const int fx = at<short>(window, kWinFrame), fy = at<short>(window, kWinFrame + 2);
        const int fw = at<short>(window, kWinFrame + 4) - fx, fh = at<short>(window, kWinFrame + 6) - fy;
        int ax = 0, ay = 0;
        if (at<std::uint8_t>(window, kWinScaled) != 0) {
            ax = w0 != 0 ? x * w1 / w0 - x : 0, ay = h0 != 0 ? y * h1 / h0 - y : 0;
        } else if (anchor == 0 && fx == at<short>(window, kWinOpened) + (w0 - 512) / 2 &&
                   fy == at<short>(window, kWinOpened + 2) + (h0 - 448) / 2) {
            // centred as it opened (its place as opened plus half the area's room past 512 x 448)
            ax = (w1 - 512) / 2 - (w0 - 512) / 2, ay = (h1 - 448) / 2 - (h0 - 448) / 2;
            if (ax != 0 || ay != 0)
                g_set_locate(window, fx + ax, fy + ay);
            continue;
        } else {
            ax = anchor == 1 || anchor == 3 ? dw : 0, ay = anchor == 2 || anchor == 3 ? dh : 0;
        }
        if (ax == 0 && ay == 0)
            continue;
        for (int k = 0; k < 4; ++k)
            at<short>(window, kWinOpened + 2 * k) = static_cast<short>(at<short>(window, kWinOpened + 2 * k) + (k & 1 ? ay : ax));
        g_set_locate(window, x + ax, y + ay);
        // SetLocate puts the frame at the place. The party list's sits lower, on the bottom of room for 6 rows (opened
        // there, the frame only as tall as its members): moved by the same, or a short list hangs where 6 rows' top is.
        if (fx != x || fy != y)
            g_resize(window, fx + ax, fy + ay, fw, fh, 1, 0, 0);
    }
    logs_follow(log_heights);
    // The help line is its layout's width plus a share of the area's past 512, measured when it opens: the share found
    // from its width now and given of the new area.
    char* help = g_help != nullptr ? *g_help : nullptr;
    char* line = help != nullptr ? at<char*>(help, 8) : nullptr;
    char* layout = line != nullptr ? at<char*>(line, kWinLayout) : nullptr;
    char* parsed = layout != nullptr ? at<char*>(layout, 8) : nullptr;
    char* block = parsed != nullptr ? at<char*>(parsed, 0) : nullptr;
    if (block != nullptr && g_resize != nullptr && w0 > 0) {
        const int x1 = at<short>(line, kWinFrame), y1 = at<short>(line, kWinFrame + 2), x2 = at<short>(line, kWinFrame + 4),
                  y2 = at<short>(line, kWinFrame + 6), own = at<short>(block, 0xA);
        const int width = static_cast<int>(std::lround(static_cast<double>(x2 - x1 - own + 512) * w1 / w0)) + own - 512;
        if (width > 16)
            g_resize(line, x1, y1, width, y2 - y1, 1, 0, 0);
    }
    // the windows over the chat placed again at the next frame's layout
    for (int slot = 0; slot < 5; ++slot)
        at<short>(g_manager, kAnchorTops + slot * 2) = -32768;
}

// The menus at width x height: the menu target made again at that size (none at the back buffer's: the interface drawn
// on it directly, as the game does), and the game's copies of the size (0037 and 0038 as read, the scene's, the menu
// manager's area).
void apply(char* gfx, char* scene, int width, int height)
{
    const int back_w = at<std::uint16_t>(scene, kSceneBack), back_h = at<std::uint16_t>(scene, kSceneBack + 2);
    const int left = at<short>(g_manager, kAreaLeft), top = at<short>(g_manager, kAreaLeft + 2);
    const int w0 = at<short>(g_manager, kAreaRight) - left, h0 = at<short>(g_manager, kAreaRight + 2) - top;
    const bool wants_target = width != back_w || height != back_h;
    if (width == at<std::uint16_t>(scene, kSceneMenus) && height == at<std::uint16_t>(scene, kSceneMenus + 2) &&
        (at<void*>(gfx, kGfxTarget) != nullptr) == wants_target)
        return;
    release(at<void*>(gfx, kGfxTarget));
    release(at<void*>(gfx, kGfxTargetDepth));
    at<void*>(gfx, kGfxTarget) = nullptr, at<void*>(gfx, kGfxTargetDepth) = nullptr;
    if (wants_target) {
        void* out = nullptr;
        void* texture = g_create_target(gfx, width, height, &out, 1, 0);
        void* depth = texture != nullptr ? g_create_depth(gfx, width, height) : nullptr;
        if (texture != nullptr && depth != nullptr) {
            at<void*>(gfx, kGfxTarget) = texture, at<void*>(gfx, kGfxTargetDepth) = depth;
        } else {
            // as the game does when it can't: the interface on the back buffer, at its size
            release(texture), release(depth);
            log::write(L"Interface: a %dx%d menu target couldn't be made: the menus are the back buffer's size", width, height);
            width = back_w, height = back_h;
            g_retry = true; // the same resolution.ui chosen again tries again
        }
    }
    *g_menu_width = static_cast<std::uint32_t>(width), *g_menu_height = static_cast<std::uint32_t>(height);
    at<std::uint16_t>(scene, kSceneMenus) = static_cast<std::uint16_t>(width);
    at<std::uint16_t>(scene, kSceneMenus + 2) = static_cast<std::uint16_t>(height);
    at<short>(g_manager, kAreaRight) = static_cast<short>(left + width);
    at<short>(g_manager, kAreaRight + 2) = static_cast<short>(top + height);
    // The chat logs' copy of the viewport: the new size, as their next lines drawn keep it. A log kept closed draws none
    // (fancychat's, chat.hide), so it held the size before and fancychat measured the closed log against it: with the
    // menus smaller, its top was far enough up to read as opened, and fancychat's chat windows hid till a bigger size.
    if (g_viewport != nullptr)
        g_viewport[2] = static_cast<std::uint32_t>(width), g_viewport[3] = static_cast<std::uint32_t>(height);
    windows_follow(w0, h0, width, height);
    log::write(L"Interface: the menus now %dx%d ([ffxi] resolution.ui%ls%ls), %dx%d before", width, height, g_text.empty() ? L"" : L"=",
               g_text.empty() ? L" unset: as the game started" : g_text.c_str(), w0, h0);
}

// resolution.ui=window-<share>: that share of the part of the game's resolution (back_w x back_h, the back buffer) the
// interface is laid out in. All of it; or, while [ui] aspect squeezes the interface in a window of another shape, the box
// of the aspect's shape the renderer squeezes it into (d3d8.c ui_box_shape: the back buffer's size times the squeeze each
// way), so the menus' target has the box's shape and is squeezed into place, and the aspect takes effect whatever the
// window. The window's shape is the renderer's: the size it was made at in the windowed modes (window::made_size), its
// title bar and borders left out, else its client area.
bool fitted(int share, long back_w, long back_h, long& width, long& height)
{
    if (share <= 0 || back_w < 64 || back_h < 64)
        return false;
    double w = static_cast<double>(back_w), h = static_cast<double>(back_h);
    if (const float aspect = graphics::ui_aspect(); aspect > 0) {
        unsigned window_w = 0, window_h = 0;
        if (!window::made_size(window_w, window_h)) {
            RECT client{};
            const HWND handle = window::handle();
            if (handle != nullptr && GetClientRect(handle, &client))
                window_w = static_cast<unsigned>(client.right), window_h = static_cast<unsigned>(client.bottom);
        }
        if (window_w != 0 && window_h != 0) {
            const double shape = static_cast<double>(window_w) / window_h;
            w *= std::min(1.0, aspect / shape);
            h *= std::min(1.0, shape / aspect);
        }
    }
    // the height first and the width from it, so the shape is the box's to within the renderer's tolerance (half a pixel
    // of the width), and never past the back buffer
    height = std::clamp(std::lround(h * share / 100.0), 64L, back_h);
    width = std::clamp(std::lround(static_cast<double>(height) * w / h), 64L, back_w);
    return true;
}

bool unset()
{
    return g_text.empty() || g_text == L"-1";
}

// resolution.ui unset: what the game started with - [ffxi.registry] 0037 x 0038 or resolution.game's, as registry.cpp gave
// it the game - else the back buffer's. While the window's edges have been dragged to a size of its own (the /vana
// window's Resolution "(resized)", window::resized_client), its inside, so "Same as Resolution" is that size: the menus
// laid out at the game's resolution and squeezed into a smaller window were small. Never past the back buffer.
void unset_size(long back_w, long back_h, long& width, long& height)
{
    width = back_w, height = back_h;
    if (unsigned w = 0, h = 0; window::resized_client(w, h)) {
        width = std::clamp(static_cast<long>(w), 64L, std::max(64L, back_w));
        height = std::clamp(static_cast<long>(h), 64L, std::max(64L, back_h));
        return;
    }
    const long w = std::wcstol(registry::setting("0037").c_str(), nullptr, 10);
    const long h = std::wcstol(registry::setting("0038").c_str(), nullptr, 10);
    if (w >= 64 && h >= 64)
        width = w, height = h;
}

// The menus' size that follows the window: window-<share>'s, or unset's while the window is a size of its own (and once
// more after, to put the game's back)
bool following(long back_w, long back_h, long& width, long& height)
{
    if (g_share > 0)
        return fitted(g_share, back_w, back_h, width, height);
    unsigned w = 0, h = 0;
    if (!unset() || (!g_resized_menus && !window::resized_client(w, h)))
        return false;
    unset_size(back_w, back_h, width, height);
    return true;
}

// resolution.ui=window-<share>, or unset, while the game runs: its size worked out twice a second, and applied once the
// same size has held for two checks running (not at each step of an edge being dragged): the window made at another size,
// [ui] aspect changed, the window or the game's resolution of another shape, the window's edges dragged.
void follow_window()
{
    const ULONGLONG now = GetTickCount64();
    if (g_retry || now - g_fit_checked < 500)
        return;
    g_fit_checked = now;
    // (minimized, the window has no size to go by: left as it is till it's back)
    if (const HWND handle = window::handle(); handle != nullptr && IsIconic(handle))
        return;
    if (!found() || *g_scene == nullptr || registry::registry_overrides("0037"))
        return;
    char* scene = *g_scene;
    long width = 0, height = 0;
    if (!following(at<std::uint16_t>(scene, kSceneBack), at<std::uint16_t>(scene, kSceneBack + 2), width, height))
        return;
    const bool held = width == g_fit_seen_w && height == g_fit_seen_h;
    g_fit_seen_w = width, g_fit_seen_h = height;
    if (held && (width != at<std::uint16_t>(scene, kSceneMenus) || height != at<std::uint16_t>(scene, kSceneMenus + 2)))
        g_pending = true;
}

} // namespace

void install(const wchar_t* ini)
{
    g_text = read_text(ini);
    g_share = registry::window_share(g_text);
}

void reload(const wchar_t* ini)
{
    const std::wstring text = read_text(ini);
    if (text == g_text && !g_retry)
        return;
    g_text = text;
    g_share = registry::window_share(text);
    g_fit_seen_w = g_fit_seen_h = 0;
    g_retry = false;
    g_pending = true;
}

void refresh()
{
    g_fit_seen_w = g_fit_seen_h = 0;
    g_pending = true;
}

bool fit_size(int share, long& width, long& height)
{
    unsigned back_w = 0, back_h = 0;
    ui::game_screen(back_w, back_h);
    return fitted(share, static_cast<long>(back_w), static_cast<long>(back_h), width, height);
}

void after_present()
{
    if (!g_pending && (g_share > 0 || unset()))
        follow_window();
    if (!g_pending)
        return;
    if (!found()) {
        g_pending = !g_searched; // FFXiMain.dll not there yet: again at the next frame
        return;
    }
    char* gfx = *g_gfx;
    char* scene = *g_scene;
    if (gfx == nullptr || scene == nullptr) // the game hasn't made its display yet
        return;
    g_pending = false;
    if (registry::registry_overrides("0037")) {
        log::write(L"Interface: [ffxi] resolution.ui is ignored: [ffxi.registry] 0037 sets the menus' size (registry.override=1)");
        return;
    }
    // Unset (or -1): unset_size. auto and auto-<percent> on the monitor the window is on (the main one before it's there);
    // window-<share>, that share of the game's window (fitted)
    long width = at<std::uint16_t>(scene, kSceneBack), height = at<std::uint16_t>(scene, kSceneBack + 2);
    unsigned resized_w = 0, resized_h = 0;
    g_resized_menus = unset() && window::resized_client(resized_w, resized_h);
    if (unset()) {
        unset_size(width, height, width, height);
    } else if (g_share > 0) {
        if (!fitted(g_share, width, height, width, height))
            return;
    } else if (!registry::parse_resolution(g_text, window::handle(), width, height)) {
        log::write(L"Interface: [ffxi] resolution.ui=%ls isn't a size, auto, auto-<percent>, window or window-<percent>: the "
                   L"menus stay as they are",
                   g_text.c_str());
        return;
    }
    if (width < 64 || height < 64 || width > 16384 || height > 16384) {
        log::write(L"Interface: [ffxi] resolution.ui=%ls is %ldx%ld, out of 64 to 16384: the menus stay as they are",
                   g_text.c_str(), width, height);
        return;
    }
    apply(gfx, scene, static_cast<int>(width), static_cast<int>(height));
}

std::uint32_t menu_target(bool& known)
{
    known = false;
    if (!found())
        return 0;
    char* gfx = *g_gfx;
    if (gfx == nullptr)
        return 0;
    known = true;
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(at<void*>(gfx, kGfxTarget)));
}

} // namespace vanacore::uiscale
