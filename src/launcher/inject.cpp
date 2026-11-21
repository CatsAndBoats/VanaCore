#include "inject.hpp"

#include <windows.h>

#include <vanacore/boot_info.hpp>

#include <cstdint>
#include <memory>

namespace vanacore {
namespace {

struct HandleCloser {
    void operator()(HANDLE h) const
    {
        if (h != nullptr && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::wstring win32_error(const wchar_t* what)
{
    return std::wstring(what) + L" failed (error " + std::to_wstring(GetLastError()) + L")";
}

// Copies `size` bytes into a new allocation in the target process. Caller frees it.
void* remote_copy(HANDLE process, const void* data, size_t size)
{
    void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote != nullptr && !WriteProcessMemory(process, remote, data, size, nullptr)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return nullptr;
    }
    return remote;
}

// Runs start(param) on a new thread in the target process and waits for its exit code.
bool remote_call(HANDLE process, std::uintptr_t start, void* param, DWORD& exit_code)
{
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), param,
                                     0, nullptr));
    if (!thread)
        return false;
    WaitForSingleObject(thread.get(), INFINITE);
    return GetExitCodeThread(thread.get(), &exit_code) != FALSE;
}

// RVA of VanaCoreInstall, read from a local mapping of the DLL that runs none of its code.
DWORD install_export_rva(const std::wstring& dll)
{
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (local == nullptr)
        return 0;
    const auto proc = reinterpret_cast<std::uintptr_t>(GetProcAddress(local, kInstallExport));
    const DWORD rva = proc != 0 ? static_cast<DWORD>(proc - reinterpret_cast<std::uintptr_t>(local)) : 0;
    FreeLibrary(local);
    return rva;
}

} // namespace

bool launch_and_inject(const LaunchOptions& options, void*& game, std::wstring& error)
{
    const DWORD install_rva = install_export_rva(options.core_dll);
    if (install_rva == 0) {
        error = L"could not find VanaCoreInstall in " + options.core_dll;
        return false;
    }

    // The game (and private-server bootloaders) expect to run from their own folder.
    wchar_t full_path[MAX_PATH];
    wchar_t* file_part = nullptr;
    if (GetFullPathNameW(options.executable.c_str(), MAX_PATH, full_path, &file_part) == 0 || file_part == nullptr) {
        error = win32_error(L"resolving the executable path");
        return false;
    }
    const std::wstring working_dir(full_path, file_part);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring command_line = options.command_line; // CreateProcessW may write to this buffer
    if (!CreateProcessW(full_path, command_line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        working_dir.c_str(), &startup, &info)) {
        error = GetLastError() == ERROR_ELEVATION_REQUIRED ? std::wstring(kElevationRequired) : win32_error(L"CreateProcessW");
        return false;
    }
    Handle process(info.hProcess);
    Handle main_thread(info.hThread);

    const auto fail = [&](std::wstring message) {
        TerminateProcess(process.get(), 1);
        error = std::move(message);
        return false;
    };

    // 1. LoadLibraryW(core_dll) inside the game. kernel32 is mapped at the same address in every
    //    32-bit process of this boot session, so our own LoadLibraryW address is valid there.
    const size_t path_bytes = (options.core_dll.size() + 1) * sizeof(wchar_t);
    void* remote_path = remote_copy(process.get(), options.core_dll.c_str(), path_bytes);
    if (remote_path == nullptr)
        return fail(win32_error(L"writing the DLL path into the game"));
    DWORD module_base = 0; // a thread exit code holds the whole HMODULE on 32-bit
    const bool loaded =
        remote_call(process.get(), reinterpret_cast<std::uintptr_t>(&LoadLibraryW), remote_path, module_base);
    VirtualFreeEx(process.get(), remote_path, 0, MEM_RELEASE);
    if (!loaded || module_base == 0)
        return fail(L"LoadLibraryW could not load " + options.core_dll + L" into the game");

    // 2. VanaCoreInstall(&boot), outside the loader lock, before the game's own code runs.
    BootInfo boot{};
    boot.size = sizeof(boot);
    boot.version = kBootInfoVersion;
    wcsncpy_s(boot.install_dir, options.install_dir.c_str(), _TRUNCATE);
    wcsncpy_s(boot.settings_ini, options.settings_ini.c_str(), _TRUNCATE);
    boot.attached = options.attached ? 1 : 0;
    boot.retail = options.retail ? 1 : 0;
    wcsncpy_s(boot.server, options.server.c_str(), _TRUNCATE);
    if (options.login != nullptr) {
        boot.logged_in = 1;
        boot.login = *options.login;
    }
    void* remote_boot = remote_copy(process.get(), &boot, sizeof(boot));
    if (remote_boot == nullptr)
        return fail(win32_error(L"writing boot info into the game"));
    DWORD result = 1;
    const bool ran = remote_call(process.get(), std::uintptr_t{module_base} + install_rva, remote_boot, result);
    VirtualFreeEx(process.get(), remote_boot, 0, MEM_RELEASE);
    if (!ran || result != 0)
        return fail(L"VanaCoreInstall failed (code " + std::to_wstring(result) + L")" +
                    (result == 1 ? L": VanaCore.exe and VanaCore.Core.dll are from different builds. Copy both from "
                                   L"the same build."
                                 : L""));

    ResumeThread(main_thread.get());
    game = process.release();
    return true;
}

} // namespace vanacore
