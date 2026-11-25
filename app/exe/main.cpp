// VanaCoreApp.exe: the splash, VanaCoreApp.dll's update check (update.cpp), then the DLL, which has the app itself. Kept small and
// changed rarely: a new signed exe starts its certificate's reputation over, a new DLL doesn't (the way the CatsEyeXI
// launcher's bootstrap and CatsEyeXI-Launcher.App.dll split).

#include <windows.h>

#include <shellapi.h>

#include <string>

#include "app_api.h"
#include "splash.h"
#include "update.h"
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

bool has_argument(const wchar_t* name)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; ++i)
        found = _wcsicmp(argv[i], name) == 0;
    LocalFree(argv);
    return found;
}

// A note on the splash, in amber, long enough to read; the start goes on after it
void show_note(vcapp::Splash& splash, const std::wstring& note)
{
    splash.status(note, -1, true);
    for (int i = 0; i < 40 && !splash.cancelled(); ++i)
        Sleep(50);
}

// VanaCoreApp.dll brought up to date from GitHub (update.cpp), unless --no-update. Never stops the start: anything that
// doesn't work out is a note. False: the player closed the splash.
bool check_for_update(vcapp::Splash& splash, const std::wstring& dir)
{
    if (has_argument(L"--no-update"))
        return true;
    vcapp::UpdateUi ui;
    ui.status = [&](const std::wstring& text, float progress) { splash.status(text, progress); };
    ui.cancelled = [&] { return splash.cancelled(); };
    const std::wstring note = vcapp::update_dll(dir, ui);
    if (!note.empty() && !splash.cancelled())
        show_note(splash, note);
    return !splash.cancelled();
}

struct Loaded {
    HMODULE dll = nullptr;
    VanaCoreAppRunFn run = nullptr;
};

// VanaCoreApp.dll from this folder (its full path: never one found elsewhere), its API this exe's; why not, when it isn't
Loaded load_app(const std::wstring& path, std::wstring& why)
{
    Loaded loaded;
    loaded.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (loaded.dll == nullptr) {
        why = L"VanaCoreApp.dll couldn't be loaded (error " + std::to_wstring(GetLastError()) + L").";
        return loaded;
    }
    const auto api = reinterpret_cast<VanaCoreAppApiFn>(GetProcAddress(loaded.dll, "VanaCoreAppApi"));
    loaded.run = reinterpret_cast<VanaCoreAppRunFn>(GetProcAddress(loaded.dll, "VanaCoreAppRun"));
    if (api == nullptr || loaded.run == nullptr || api() != VANACOREAPP_API) {
        why = L"VanaCoreApp.dll is for another version of VanaCoreApp.exe (API " + std::to_wstring(api != nullptr ? api() : 0) +
              L", this one takes " + std::to_wstring(VANACOREAPP_API) + L").";
        FreeLibrary(loaded.dll);
        loaded = Loaded{};
    }
    return loaded;
}

// VanaCoreApp.dll from this folder, its API checked, then the app: its exit code. An updated DLL that won't load gives way
// to the one it replaced.
int run_app(const std::wstring& dir, HINSTANCE instance)
{
    const std::wstring path = dir + L"\\VanaCoreApp.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && !vcapp::restore_dll(dir)) {
        fail(L"VanaCoreApp.dll isn't in " + dir + L".\n\nReinstall VanaCore App to put it back.");
        return 1;
    }
    g_splash->status(L"Loading\x2026", -1);
    std::wstring why;
    Loaded loaded = load_app(path, why);
    if (loaded.dll == nullptr && vcapp::restore_dll(dir)) {
        show_note(*g_splash, L"The update didn't load: back to the version before");
        g_splash->status(L"Loading\x2026", -1);
        loaded = load_app(path, why);
    }
    if (loaded.dll == nullptr) {
        fail(why + L"\n\nReinstall VanaCore App.");
        return 1;
    }
    const VanaCoreAppRunFn run = loaded.run;
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
        const std::wstring dir = exe_dir();
        if (check_for_update(splash, dir))
            code = run_app(dir, instance);
        splash.close();
        g_splash = nullptr;
    }
    if (com)
        CoUninitialize();
    return code;
}
