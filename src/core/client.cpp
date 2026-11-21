#include "client.hpp"

#include <windows.h>

#include <vanacore/version.hpp>

#include <atomic>
#include <string>

#include "log.hpp"

namespace vanacore::client {
namespace {

constexpr wchar_t kViewer[] = L"PlayOnline Viewer"; // what PlayOnline calls its windows: "PlayOnline Viewer Ver.1.18.15e"
constexpr wchar_t kMark[] = L" - VanaCore v";
constexpr DWORD kTitleEvery = 1000; // ms between looks at the windows
constexpr DWORD kAsk = 200;         // ms a window's own thread has to answer before it's left for the next look

std::atomic<bool> g_retail{false};
std::atomic<int> g_marked{0};
std::string g_server; // set_server's, lower case

// `window`'s title, asked of its own thread with a time limit (a window whose thread is busy isn't waited for)
bool title_of(HWND window, std::wstring& text)
{
    wchar_t buffer[256] = {};
    DWORD_PTR length = 0;
    if (SendMessageTimeoutW(window, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(buffer), SMTO_ABORTIFHUNG, kAsk, &length) == 0)
        return false;
    text = buffer;
    return true;
}

// PlayOnline's windows get VanaCore's name and version on their title bars, so a window says it has VanaCore in it
BOOL CALLBACK mark_window(HWND window, LPARAM)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    std::wstring title;
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) || !title_of(window, title) ||
        !title.starts_with(kViewer) || title.find(kMark) != std::wstring::npos)
        return TRUE;
    const std::wstring marked = title + kMark + vanacore::kVersion;
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(marked.c_str()), SMTO_ABORTIFHUNG, kAsk, &result) != 0 &&
        g_marked.fetch_add(1) < 5)
        log::write(L"Retail: window title \"%ls\"", marked.c_str());
    return TRUE;
}

DWORD WINAPI watch_titles(LPVOID)
{
    for (;;) {
        EnumWindows(&mark_window, 0);
        Sleep(kTitleEvery);
    }
}

} // namespace

void install(const wchar_t* ini, bool forced)
{
    const bool has_ini = ini != nullptr && ini[0] != L'\0';
    const bool retail = forced || (has_ini && GetPrivateProfileIntW(L"client", L"retail", 0, ini) != 0);
    g_retail = retail;
    if (!retail)
        return;
    log::write(L"Retail: PlayOnline and FFXI run as installed ([client] retail=1 or --retail): their registry keys and folders are "
               L"the machine's own, and only the settings [ffxi] and [ffxi.registry] name are VanaCore's");
    // [client] retail.title=0 leaves PlayOnline's window titles as they are
    if (!has_ini || GetPrivateProfileIntW(L"client", L"retail.title", 1, ini) != 0) {
        if (HANDLE thread = CreateThread(nullptr, 0, &watch_titles, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
}

bool retail()
{
    return g_retail;
}

void set_server(const wchar_t* host)
{
    g_server.clear();
    for (std::size_t at = 0; host != nullptr && host[at] != L'\0'; ++at) {
        const wchar_t c = host[at];
        g_server += c < 0x80 ? static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c) : '?';
    }
    if (!g_server.empty())
        log::write(L"Server: %hs", g_server.c_str());
}

const std::string& server()
{
    return g_server;
}

} // namespace vanacore::client
