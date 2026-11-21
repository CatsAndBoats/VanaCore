#include "session.hpp"

#include <windows.h>

#include <vanacore/boot_info.hpp>

#include <atomic>
#include <cstdio>

#include "imports.hpp"
#include "log.hpp"

namespace vanacore::session {
namespace {

bool g_attached = false;
decltype(&CreateMutexA) g_create_mutex = nullptr;

// FFXi.dll's "only one copy" check: it creates a mutex named "Final Fantasy XI" and, if one exists (error 183), goes
// down its "already running" path, which needs PlayOnline's interface and faults without it. Ashita and Windower hand
// every copy a mutex of its own, so several clients run side by side; VanaCore does the same.
HANDLE WINAPI create_mutex(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCSTR name)
{
    if (name == nullptr || _stricmp(name, "Final Fantasy XI") != 0)
        return g_create_mutex(attributes, owner, name);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
        log::write(L"Game start: FFXI's single-copy mutex \"%hs\" is made unnamed, so another client can run beside this one",
                   name);
    const HANDLE mutex = g_create_mutex(attributes, owner, nullptr);
    SetLastError(ERROR_SUCCESS); // not ERROR_ALREADY_EXISTS, whatever the last call left
    return mutex;
}

void on_module(HMODULE module)
{
    if (!imports::is_windows_module(module))
        imports::hook(module, "KERNEL32.dll", "CreateMutexA", &create_mutex, g_create_mutex);
}

} // namespace

void install(bool attached)
{
    g_attached = attached;
    imports::for_each_module(&on_module);
}

void game_started()
{
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return;
    wchar_t name[64];
    swprintf_s(name, kGameStartedEvent, GetCurrentProcessId());
    // Opens VanaCore.exe's event, or creates it if VanaCore.exe hasn't yet. The handle stays open
    // so the event outlives this call either way.
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name);
    if (event != nullptr)
        SetEvent(event);
    if (g_attached) {
        log::write(L"Game started");
        return;
    }
    // The game (xiloader runs it inside pol.exe) shares VanaCore.exe's console: let go of it, so
    // the window can close and closing it can't end the game. The bootloader's later console
    // output goes nowhere; attached=1 keeps it.
    FreeConsole();
    log::write(L"Game started; detached from the console (attached=0)");
}

} // namespace vanacore::session
