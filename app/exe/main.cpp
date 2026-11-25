// VanaCoreApp.exe: the splash, the update check (to come), then VanaCoreApp.dll, which has the app itself. Kept small and
// changed rarely: a new signed exe starts its certificate's reputation over, a new DLL doesn't (the way the CatsEyeXI
// launcher's bootstrap and CatsEyeXI-Launcher.App.dll split).

#include <windows.h>

#include <shellapi.h>

#include <string>

#include "app_api.h"
#include "splash.h"
#include "version.h"

namespace {

vcapp::Splash* g_splash = nullptr;

std::wstring exe_dir()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\'));
}

void fail(const std::wstring& text)
{
    if (g_splash != nullptr)
        g_splash->close();
    MessageBoxW(nullptr, text.c_str(), L"VanaCore", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void __cdecl host_status(const wchar_t* text, float progress)
{
    if (g_splash != nullptr)
        g_splash->status(text != nullptr ? text : L"", progress);
}

void __cdecl host_close()
{
    if (g_splash != nullptr)
        g_splash->close();
}

int __cdecl host_cancelled()
{
    return g_splash != nullptr && g_splash->cancelled() ? 1 : 0;
}

// To come: VanaCoreApp.dll checked against the published one, and a newer one downloaded (checked, then put in its place)
// before it's loaded. False: the start was cancelled.
bool check_for_update(vcapp::Splash& splash)
{
    splash.status(L"Checking for updates\x2026", -1);
    // (nothing to check yet: the splash kept up a moment, not a flash)
    for (int i = 0; i < 12 && !splash.cancelled(); ++i)
        Sleep(50);
    return !splash.cancelled();
}

// VanaCoreApp.dll from this folder, its API checked, then the app: its exit code
int run_app(const std::wstring& dir, HINSTANCE instance)
{
    const std::wstring path = dir + L"\\VanaCoreApp.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fail(L"VanaCoreApp.dll isn't in " + dir + L".\n\nReinstall VanaCore App to put it back.");
        return 1;
    }
    g_splash->status(L"Loading\x2026", -1);
    // its full path, its own folder for anything it needs: never a VanaCoreApp.dll found elsewhere
    const HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (dll == nullptr) {
        fail(L"VanaCoreApp.dll couldn't be loaded (error " + std::to_wstring(GetLastError()) + L").");
        return 1;
    }
    const auto api = reinterpret_cast<VanaCoreAppApiFn>(GetProcAddress(dll, "VanaCoreAppApi"));
    const auto run = reinterpret_cast<VanaCoreAppRunFn>(GetProcAddress(dll, "VanaCoreAppRun"));
    if (api == nullptr || run == nullptr || api() != VANACOREAPP_API) {
        fail(L"VanaCoreApp.dll is for another version of VanaCoreApp.exe (API " + std::to_wstring(api != nullptr ? api() : 0) +
             L", this one takes " + std::to_wstring(VANACOREAPP_API) + L").\n\nReinstall VanaCore App.");
        FreeLibrary(dll);
        return 1;
    }
    const HICON big = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                    GetSystemMetrics(SM_CYICON), 0));
    const HICON little = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                       GetSystemMetrics(SM_CYSMICON), 0));
    VanaCoreAppHost host{};
    host.size = sizeof host;
    host.api = VANACOREAPP_API;
    host.dir = dir.c_str();
    host.exe_version = L"" VCAPP_EXE_VERSION_STR;
    host.icon_big = big;
    host.icon_small = little;
    host.splash_status = &host_status;
    host.splash_close = &host_close;
    host.splash_cancelled = &host_cancelled;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const int code = run(&host, argc, argv);
    LocalFree(argv);
    // (the DLL stays loaded: its windows' classes and anything it started are gone with the process)
    if (big != nullptr)
        DestroyIcon(big);
    if (little != nullptr)
        DestroyIcon(little);
    return code;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetDllDirectoryW(L""); // the current folder off the DLL search
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    int code = 0;
    {
        vcapp::Splash splash(instance);
        g_splash = &splash;
        splash.show();
        if (check_for_update(splash))
            code = run_app(exe_dir(), instance);
        splash.close();
        g_splash = nullptr;
    }
    if (com)
        CoUninitialize();
    return code;
}
