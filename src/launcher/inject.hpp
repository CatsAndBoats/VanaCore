#pragma once

#include <string>

#include <vanacore/boot_info.hpp>

namespace vanacore {

struct LaunchOptions {
    std::wstring executable;   // game or bootloader to start
    std::wstring command_line; // full command line, argv[0] included
    std::wstring core_dll;     // absolute path to VanaCore.Core.dll
    std::wstring install_dir;  // handed to the core through BootInfo
    std::wstring settings_ini; // absolute; handed to the core through BootInfo
    bool attached = false;     // handed to the core through BootInfo
    const GameLogin* login = nullptr; // VanaCore's own login: the core starts the game with it (VanaCoreRunGame)
    bool retail = false;       // [client] retail=1 or --retail: handed to the core through BootInfo
    std::wstring server;       // the server's host: handed to the core through BootInfo
};

// launch_and_inject's `error` when Windows won't start the executable without administrator rights (retail's pol.exe asks
// for them in its manifest), for a caller that can ask for them.
inline constexpr wchar_t kElevationRequired[] = L"the program asks for administrator rights";

// Starts the executable suspended, loads VanaCore.Core.dll into it, runs VanaCoreInstall, then
// resumes it and hands back its process handle in `process` (the caller closes it). On failure
// the process is terminated and `error` says why.
bool launch_and_inject(const LaunchOptions& options, void*& process, std::wstring& error);

} // namespace vanacore
