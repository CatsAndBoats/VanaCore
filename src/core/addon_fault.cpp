#include "addon_fault.hpp"

#include <lua.hpp>

#include <vanacore/log_file.hpp>

#include <cstdio>
#include <iterator>

#include "crash.hpp"

namespace vanacore::addon_fault {
namespace {

// Where `L`'s Lua was at a fault, into `out`: up to three Lua functions, innermost first ("items.lua:48 in cstr <
// items.lua:64 in get"). Read in the exception filter, before the unwind. Code LuaJIT has compiled shows where it was
// entered, which can be a line or a loop before the fault. Empty if the frames can't be read.
void lua_location(lua_State* L, char* out, size_t size)
{
    out[0] = '\0';
    __try {
        size_t used = 0;
        int shown = 0;
        lua_Debug ar;
        for (int level = 0; shown < 3 && level < 16 && lua_getstack(L, level, &ar) != 0; ++level) {
            if (lua_getinfo(L, "Sln", &ar) == 0 || ar.currentline <= 0)
                continue; // a C function
            const char* file = ar.short_src;
            for (const char* c = ar.short_src; *c != '\0'; ++c)
                if (*c == '\\' || *c == '/')
                    file = c + 1;
            const int n = std::snprintf(out + used, size - used, "%s%s:%d%s%s", shown != 0 ? " < " : "", file, ar.currentline,
                                        ar.name != nullptr ? " in " : "", ar.name != nullptr ? ar.name : "");
            if (n < 0 || used + static_cast<size_t>(n) >= size)
                break;
            used += static_cast<size_t>(n);
            ++shown;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

// Catches the exceptions an addon's FFI code (or a bug in VanaCore's API) can cause; everything
// else, including LuaJIT's own error exceptions, goes on up. With `L`, where its Lua was.
int catch_fault(const EXCEPTION_POINTERS* info, Fault& fault, lua_State* L)
{
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    switch (record.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        fault.code = record.ExceptionCode;
        fault.address = record.ExceptionAddress;
        if (record.NumberParameters >= 2) {
            fault.access = record.ExceptionInformation[0];
            fault.target = record.ExceptionInformation[1];
        }
        fault.caller_count = crash::callers(*info->ContextRecord, fault.callers, static_cast<int>(std::size(fault.callers)));
        if (L != nullptr)
            lua_location(L, fault.lua, sizeof(fault.lua));
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

std::string address_text(const void* address)
{
    std::string out;
    for (const wchar_t c : describe_address(address)) // module names: ASCII
        out += static_cast<char>(c);
    return out;
}

} // namespace

int guarded_pcall(lua_State* L, int nargs, int nresults, int handler, Fault& fault)
{
    __try {
        return lua_pcall(L, nargs, nresults, handler);
    } __except (catch_fault(GetExceptionInformation(), fault, L)) {
        return kCrashed;
    }
}

bool guarded_close(lua_State* L, Fault& fault)
{
    __try {
        lua_close(L); // runs the addon's finalizers
        return true;
    } __except (catch_fault(GetExceptionInformation(), fault, nullptr)) { // (the state half closed: its frames aren't read)
        return false;
    }
}

// "access violation writing 0x0000F0EC at FFXiMain.dll+0x1234 from VanaCore.Core.dll+0x5678 (items.lua:48 in cstr)": the
// fault, the code that called it, and the addon's Lua; on a line of its own (the log's), the return addresses further up.
std::string describe(const Fault& fault)
{
    const wchar_t* name = describe_status(fault.code);
    char text[160];
    if (fault.code == EXCEPTION_ACCESS_VIOLATION)
        sprintf_s(text, "access violation %s 0x%08lX", fault.access == 1 ? "writing" : fault.access == 8 ? "executing" : "reading",
                  static_cast<unsigned long>(fault.target));
    else
        sprintf_s(text, "%ls (0x%08lX)", name != nullptr ? name : L"exception", fault.code);
    std::string out = std::string(text) + " at " + address_text(fault.address);
    if (fault.caller_count > 0)
        out += " from " + address_text(reinterpret_cast<const void*>(fault.callers[0]));
    if (fault.lua[0] != '\0')
        out += std::string(" (") + fault.lua + ")";
    if (fault.caller_count > 1) {
        out += "\nand on the stack:";
        for (int i = 1; i < fault.caller_count; ++i)
            out += " " + address_text(reinterpret_cast<const void*>(fault.callers[i]));
    }
    return out;
}

} // namespace vanacore::addon_fault
