// VanaCoreApp.dll's two exports (exports.def): the API it was built with, which VanaCoreApp.exe checks before anything
// else, and the app itself.

#include "app_api.h"
#include "home.h"

extern "C" unsigned __cdecl VanaCoreAppApi()
{
    return VANACOREAPP_API;
}

extern "C" int __cdecl VanaCoreAppRun(const VanaCoreAppHost* host, int argc, wchar_t** argv)
{
    static_cast<void>(argc);
    static_cast<void>(argv);
    if (host == nullptr || host->size < sizeof(VanaCoreAppHost) || host->api != VANACOREAPP_API)
        return 1;
    if (host->splash_cancelled())
        return 0;
    vcapp::Home home(*host);
    return home.run();
}
