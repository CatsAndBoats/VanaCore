#pragma once
// What VanaCoreApp.exe and VanaCoreApp.dll agree on. The exe (signed, changed rarely) shows the splash, will check for a newer
// DLL there, then loads VanaCoreApp.dll from its own folder and calls VanaCoreAppRun; the DLL (signed, updated freely) has
// every view and all the logic, and closes the splash once its own window is up.
//
// Plain C across the boundary: each side links its own static CRT, so nothing the other allocates (a std::wstring, a FILE*)
// crosses it. A change here that an older side can't take raises VANACOREAPP_API; the exe refuses a DLL whose
// VanaCoreAppApi() isn't its own. A field added at the end, with `size` telling the DLL it's there, needs no raise.

#include <windows.h>

#define VANACOREAPP_API 1

extern "C" {

struct VanaCoreAppHost {
    unsigned size;               // sizeof(VanaCoreAppHost) as the exe was built: a field past it isn't there
    unsigned api;                // VANACOREAPP_API the exe was built with
    const wchar_t* dir;          // the folder VanaCoreApp.exe is in, no trailing backslash
    const wchar_t* exe_version;  // VanaCoreApp.exe's version ("0.1.0")
    HICON icon_big, icon_small;  // VanaCoreApp.exe's icon, for the DLL's windows (the exe keeps them)
    // The splash: its line and progress (0-1, below 0 an endless sweep), closed (both from any thread; after it's closed
    // they do nothing), and whether the player closed it (its X)
    void(__cdecl* splash_status)(const wchar_t* text, float progress);
    void(__cdecl* splash_close)();
    int(__cdecl* splash_cancelled)();
};

// VanaCoreApp.dll's exports (exports.def): the API it was built with, and the app itself (its exit code)
typedef unsigned(__cdecl* VanaCoreAppApiFn)();
typedef int(__cdecl* VanaCoreAppRunFn)(const VanaCoreAppHost* host, int argc, wchar_t** argv);

} // extern "C"
