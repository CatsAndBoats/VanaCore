#include "mousediag.hpp"

#include <windowsx.h>

#include <intrin.h>

#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "imports.hpp"
#include "log.hpp"

namespace vanacore::mousediag {
namespace {

bool g_on = false;
std::mutex g_lock;
std::map<std::pair<std::wstring, void*>, std::pair<unsigned, ULONGLONG>> g_seen; // (what, caller module): count, last
unsigned g_lines = 0;
constexpr unsigned kMaxLines = 3000;

// True for the first few of each (what, caller module), then once a second
bool wanted(const wchar_t* what, void* caller)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(caller), &module);
    const std::lock_guard lock(g_lock);
    if (g_lines >= kMaxLines)
        return false;
    auto& [count, last] = g_seen[{what, module}];
    const ULONGLONG now = GetTickCount64();
    if (count++ < 5 || now - last >= 1000) {
        last = now;
        ++g_lines;
        return true;
    }
    return false;
}

std::wstring module_of(void* caller)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(caller), &module);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    wchar_t text[MAX_PATH + 32];
    swprintf_s(text, L"%ls+0x%X", name != nullptr ? name + 1 : path,
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(module)));
    return text;
}

decltype(&GetCursorPos) g_get_cursor_pos = nullptr;
decltype(&ScreenToClient) g_screen_to_client = nullptr;
decltype(&ClientToScreen) g_client_to_screen = nullptr;
decltype(&GetClientRect) g_get_client_rect = nullptr;
decltype(&GetWindowRect) g_get_window_rect = nullptr;
decltype(&GetSystemMetrics) g_get_system_metrics = nullptr;
decltype(&MapWindowPoints) g_map_window_points = nullptr;
decltype(&GetMessagePos) g_get_message_pos = nullptr;

BOOL WINAPI get_cursor_pos(LPPOINT point)
{
    const BOOL ok = g_get_cursor_pos(point);
    if (void* caller = _ReturnAddress(); wanted(L"GetCursorPos", caller))
        log::write(L"Mouse trace: GetCursorPos -> %ld,%ld (%ls)", point != nullptr ? point->x : 0, point != nullptr ? point->y : 0,
                   module_of(caller).c_str());
    return ok;
}

BOOL WINAPI screen_to_client(HWND window, LPPOINT point)
{
    const POINT before = point != nullptr ? *point : POINT{};
    const BOOL ok = g_screen_to_client(window, point);
    if (void* caller = _ReturnAddress(); wanted(L"ScreenToClient", caller))
        log::write(L"Mouse trace: ScreenToClient(%p, %ld,%ld) -> %ld,%ld (%ls)", window, before.x, before.y,
                   point != nullptr ? point->x : 0, point != nullptr ? point->y : 0, module_of(caller).c_str());
    return ok;
}

BOOL WINAPI client_to_screen(HWND window, LPPOINT point)
{
    const POINT before = point != nullptr ? *point : POINT{};
    const BOOL ok = g_client_to_screen(window, point);
    if (void* caller = _ReturnAddress(); wanted(L"ClientToScreen", caller))
        log::write(L"Mouse trace: ClientToScreen(%p, %ld,%ld) -> %ld,%ld (%ls)", window, before.x, before.y,
                   point != nullptr ? point->x : 0, point != nullptr ? point->y : 0, module_of(caller).c_str());
    return ok;
}

BOOL WINAPI get_client_rect(HWND window, LPRECT rect)
{
    const BOOL ok = g_get_client_rect(window, rect);
    if (void* caller = _ReturnAddress(); wanted(L"GetClientRect", caller) && rect != nullptr)
        log::write(L"Mouse trace: GetClientRect(%p) -> %ld,%ld %ldx%ld (%ls)", window, rect->left, rect->top,
                   rect->right - rect->left, rect->bottom - rect->top, module_of(caller).c_str());
    return ok;
}

BOOL WINAPI get_window_rect(HWND window, LPRECT rect)
{
    const BOOL ok = g_get_window_rect(window, rect);
    if (void* caller = _ReturnAddress(); wanted(L"GetWindowRect", caller) && rect != nullptr)
        log::write(L"Mouse trace: GetWindowRect(%p) -> %ld,%ld %ldx%ld (%ls)", window, rect->left, rect->top,
                   rect->right - rect->left, rect->bottom - rect->top, module_of(caller).c_str());
    return ok;
}

int WINAPI get_system_metrics(int index)
{
    const int value = g_get_system_metrics(index);
    if (void* caller = _ReturnAddress(); wanted((L"GetSystemMetrics " + std::to_wstring(index)).c_str(), caller))
        log::write(L"Mouse trace: GetSystemMetrics(%d) -> %d (%ls)", index, value, module_of(caller).c_str());
    return value;
}

int WINAPI map_window_points(HWND from, HWND to, LPPOINT points, UINT count)
{
    const POINT before = points != nullptr && count > 0 ? points[0] : POINT{};
    const int result = g_map_window_points(from, to, points, count);
    if (void* caller = _ReturnAddress(); wanted(L"MapWindowPoints", caller))
        log::write(L"Mouse trace: MapWindowPoints(%p -> %p, %u, first %ld,%ld) -> first %ld,%ld (%ls)", from, to, count, before.x,
                   before.y, points != nullptr && count > 0 ? points[0].x : 0, points != nullptr && count > 0 ? points[0].y : 0,
                   module_of(caller).c_str());
    return result;
}

DWORD WINAPI get_message_pos()
{
    const DWORD value = g_get_message_pos();
    if (void* caller = _ReturnAddress(); wanted(L"GetMessagePos", caller))
        log::write(L"Mouse trace: GetMessagePos -> %d,%d (%ls)", GET_X_LPARAM(value), GET_Y_LPARAM(value),
                   module_of(caller).c_str());
    return value;
}

void on_module(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    if (_wcsicmp(name, L"FFXiMain.dll") != 0 && _wcsicmp(name, L"polcore.dll") != 0)
        return;
    int hooked = 0;
    hooked += imports::hook(module, "USER32.dll", "GetCursorPos", &get_cursor_pos, g_get_cursor_pos);
    hooked += imports::hook(module, "USER32.dll", "ScreenToClient", &screen_to_client, g_screen_to_client);
    hooked += imports::hook(module, "USER32.dll", "ClientToScreen", &client_to_screen, g_client_to_screen);
    hooked += imports::hook(module, "USER32.dll", "GetClientRect", &get_client_rect, g_get_client_rect);
    hooked += imports::hook(module, "USER32.dll", "GetWindowRect", &get_window_rect, g_get_window_rect);
    hooked += imports::hook(module, "USER32.dll", "GetSystemMetrics", &get_system_metrics, g_get_system_metrics);
    hooked += imports::hook(module, "USER32.dll", "MapWindowPoints", &map_window_points, g_map_window_points);
    hooked += imports::hook(module, "USER32.dll", "GetMessagePos", &get_message_pos, g_get_message_pos);
    log::write(L"Mouse trace: %ls: %d of its cursor and window calls traced", name, hooked);
}

} // namespace

void install(const wchar_t* ini)
{
    g_on = ini != nullptr && ini[0] != L'\0' && GetPrivateProfileIntW(L"logging", L"mouse", 0, ini) != 0;
    if (!g_on)
        return;
    log::write(L"Mouse trace: on ([logging] mouse=1): the game's cursor and window calls, and its mouse messages");
    imports::for_each_module(&on_module);
}

const wchar_t* button_name(UINT message)
{
    switch (message) {
    case WM_LBUTTONDOWN: return L"WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return L"WM_LBUTTONUP";
    case WM_LBUTTONDBLCLK: return L"WM_LBUTTONDBLCLK";
    case WM_RBUTTONDOWN: return L"WM_RBUTTONDOWN";
    case WM_RBUTTONUP: return L"WM_RBUTTONUP";
    default: return nullptr;
    }
}

void on_entry(UINT message, LPARAM lparam)
{
    if (!g_on)
        return;
    const wchar_t* name = button_name(message);
    if (name == nullptr || !wanted((std::wstring(L"entry ") + name).c_str(), reinterpret_cast<void*>(&on_entry)))
        return;
    log::write(L"Mouse trace: %ls arrives at the game's window at %d,%d (before anything takes it)", name, GET_X_LPARAM(lparam),
               GET_Y_LPARAM(lparam));
}

void on_taken(UINT message, const char* by, int x, int y)
{
    if (!g_on)
        return;
    const wchar_t* name = button_name(message);
    if (name == nullptr || !wanted((std::wstring(L"taken ") + name).c_str(), reinterpret_cast<void*>(&on_taken)))
        return;
    log::write(L"Mouse trace: %ls at %d,%d kept from the game by %hs", name, x, y, by);
}

void on_message(UINT message, LPARAM sent, LPARAM given)
{
    if (!g_on)
        return;
    const wchar_t* what = message == WM_MOUSEMOVE     ? L"WM_MOUSEMOVE"
                          : message == WM_LBUTTONDOWN ? L"WM_LBUTTONDOWN"
                          : message == WM_LBUTTONUP   ? L"WM_LBUTTONUP"
                          : message == WM_RBUTTONDOWN ? L"WM_RBUTTONDOWN"
                                                      : nullptr;
    if (what == nullptr || !wanted(what, reinterpret_cast<void*>(&on_message)))
        return;
    POINT cursor{};
    GetCursorPos(&cursor);
    log::write(L"Mouse trace: %ls sent %d,%d, the game gets %d,%d (the cursor on the screen %ld,%ld)", what, GET_X_LPARAM(sent),
               GET_Y_LPARAM(sent), GET_X_LPARAM(given), GET_Y_LPARAM(given), cursor.x, cursor.y);
}

} // namespace vanacore::mousediag
