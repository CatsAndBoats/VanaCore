// VanaCoreApp.dll's two exports (exports.def): the API it was built with, which VanaCoreApp.exe checks before anything
// else, and the app itself: the installer when it's run from a folder without VanaCore (install.cpp), the uninstaller with
// --uninstall, else the home.

#include "app_api.h"
#include "home.h"
#include "install.h"

extern "C" unsigned __cdecl VanaCoreAppApi()
{
    return VANACOREAPP_API;
}

extern "C" int __cdecl VanaCoreAppRun(const VanaCoreAppHost* host, int argc, wchar_t** argv)
{
    if (host == nullptr || host->size < sizeof(VanaCoreAppHost) || host->api != VANACOREAPP_API)
        return 1;
    if (host->splash_cancelled())
        return 0;
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--uninstall") == 0)
            return vcapp::uninstall(*host);
    if (!vcapp::has_vanacore(host->dir)) {
        vcapp::Installer installer(*host);
        return installer.run();
    }
    vcapp::Home home(*host);
    return home.run();
}
