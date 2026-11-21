#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

struct lua_State;

// An addon's crash: a hardware exception (access violation, ...) its FFI code (or a bug in VanaCore's API) causes, which
// LuaJIT passes on, caught so that the addon alone stops (addons.cpp), and said in words.
namespace vanacore::addon_fault {

// A hardware exception (access violation, ...) in an addon's code, which LuaJIT passes on.
struct Fault {
    DWORD code = 0;
    void* address = nullptr;
    ULONG_PTR access = 0; // access violations: 0 read, 1 write, 8 execute
    ULONG_PTR target = 0; // the address it tried to use
    std::uintptr_t callers[6] = {}; // return addresses on the stack, nearest first (crash::callers)
    int caller_count = 0;
    char lua[200] = {}; // where the addon's Lua was (lua_location)
};

constexpr int kCrashed = -1; // not a lua_pcall status

// lua_pcall, or kCrashed at a fault, `fault` said (with where L's Lua was). Everything else, LuaJIT's own error exceptions
// too, goes on up.
int guarded_pcall(lua_State* L, int nargs, int nresults, int handler, Fault& fault);

// lua_close (it runs the addon's finalizers), or false at a fault
bool guarded_close(lua_State* L, Fault& fault);

// "access violation writing 0x0000F0EC at FFXiMain.dll+0x1234 from VanaCore.Core.dll+0x5678 (items.lua:48 in cstr)": the
// fault, the code that called it, and the addon's Lua; on a line of its own (the log's), the return addresses further up.
std::string describe(const Fault& fault);

} // namespace vanacore::addon_fault
