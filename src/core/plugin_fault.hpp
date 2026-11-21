#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// A structured exception (access violation, a C++ exception leaving the plugin, ...) in a native plugin's code, caught by the
// Ashita and Windower hosts (compat/ashita/host.cpp, compat/windower/host.cpp) around each call into a plugin.
namespace vanacore::plugin_fault {

struct Fault {
    DWORD code = 0;
    void* address = nullptr;
    ULONG_PTR access = 0;
    ULONG_PTR target = 0;
    char message[256] = {}; // a C++ exception's what()
    std::uintptr_t caller = 0; // the nearest return address on the stack (crash::callers), 0 for none
};

// guarded's exception filter: `fault` filled in, and every exception handled
int record(const EXCEPTION_POINTERS* info, Fault& fault);

// call(): true, or false at a fault, `fault` said
template <typename Call>
bool guarded(Call& call, Fault& fault)
{
    __try {
        call();
        return true;
    } __except (record(GetExceptionInformation(), fault)) {
        return false;
    }
}

// "access violation writing 0x0000F0EC at plugin.dll+0x1234 from VanaCore.Core.dll+0x5678", "a C++ exception at ...", ...
std::string describe(const Fault& fault);

} // namespace vanacore::plugin_fault
