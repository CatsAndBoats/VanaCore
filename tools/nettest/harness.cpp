// Test harness: one LuaJIT state with VanaCore's real vana.lua, settings.lua (json) and net.lua over the real net engine.
#include <windows.h>

#include <lua.hpp>

#include <cstdio>
#include <string>

#include "lua_net.hpp"
#include "net.hpp"

static double now()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static int l_log(lua_State* L)
{
    std::printf("[log] %s\n", lua_tostring(L, 1));
    return 0;
}
static int l_clock(lua_State* L)
{
    lua_pushnumber(L, now());
    return 1;
}
static int l_chat(lua_State* L)
{
    std::printf("%s\n", lua_tostring(L, 1));
    return 0;
}
static int l_report(lua_State* L)
{
    std::printf("[REPORT %s] %s\n%s\n", lua_tostring(L, 1), lua_tostring(L, 2), lua_isstring(L, 3) ? lua_tostring(L, 3) : "");
    return 0;
}
static int l_noop(lua_State*) { return 0; }

static std::string slurp(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::printf("can't open %s\n", path);
        std::exit(2);
    }
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        s.append(buf, n);
    std::fclose(f);
    return s;
}

static void check(lua_State* L, int rc, const char* what)
{
    if (rc != 0) {
        std::printf("%s failed: %s\n", what, lua_tostring(L, -1));
        std::exit(3);
    }
}

int main(int argc, char** argv)
{
    // argv: <vana.lua> <settings.lua> <net.lua> <test.lua> [seconds]
    if (argc < 5)
        return 1;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    lua_newtable(L); // vana
    lua_pushcfunction(L, l_log);
    lua_setfield(L, -2, "log");
    lua_newtable(L);
    lua_pushcfunction(L, l_clock);
    lua_setfield(L, -2, "clock");
    lua_setfield(L, -2, "time");
    lua_newtable(L);
    lua_pushcfunction(L, l_chat);
    lua_setfield(L, -2, "write");
    lua_setfield(L, -2, "chat");
    lua_newtable(L);
    lua_pushstring(L, "C:\\nowhere");
    lua_setfield(L, -2, "install");
    lua_pushstring(L, "C:\\nowhere\\addons");
    lua_setfield(L, -2, "addons");
    lua_setfield(L, -2, "paths");
    lua_newtable(L); // vana.fs, vana.game: never reached by the tests
    lua_setfield(L, -2, "fs");
    lua_newtable(L);
    lua_setfield(L, -2, "game");
    lua_setglobal(L, "vana");

    // vana.lua
    std::string src = slurp(argv[1]);
    check(L, luaL_loadbuffer(L, src.data(), src.size(), "@vana.lua"), "load vana.lua");
    lua_newtable(L);
    lua_pushcfunction(L, l_report);
    lua_setfield(L, -2, "report");
    lua_pushcfunction(L, l_noop);
    lua_setfield(L, -2, "subscribe");
    check(L, lua_pcall(L, 1, 1, 0), "run vana.lua");
    int hooks = luaL_ref(L, LUA_REGISTRYINDEX);

    // settings.lua (vana.json)
    src = slurp(argv[2]);
    check(L, luaL_loadbuffer(L, src.data(), src.size(), "@settings.lua"), "load settings.lua");
    lua_rawgeti(L, LUA_REGISTRYINDEX, hooks);
    lua_pushstring(L, "Test");
    check(L, lua_pcall(L, 2, 0, 0), "run settings.lua");

    lua_gc(L, LUA_GCCOLLECT, 0);
    const int before = lua_gc(L, LUA_GCCOUNT, 0);

    // net.lua
    src = slurp(argv[3]);
    check(L, luaL_loadbuffer(L, src.data(), src.size(), "@net.lua"), "load net.lua");
    lua_rawgeti(L, LUA_REGISTRYINDEX, hooks);
    vanacore::lua_net::push(L);
    check(L, lua_pcall(L, 2, 0, 0), "run net.lua");
    lua_gc(L, LUA_GCCOLLECT, 0);
    std::printf("[harness] net.lua adds %d KB to the state (%d KB before it)\n", lua_gc(L, LUA_GCCOUNT, 0) - before, before);

    // the test
    src = slurp(argv[4]);
    check(L, luaL_loadbuffer(L, src.data(), src.size(), "@test.lua"), "load test.lua");
    check(L, lua_pcall(L, 0, 0, 0), "run test.lua");

    const double limit = argc > 5 ? std::atof(argv[5]) : 60;
    const double started = now();
    double slowest = 0;
    long frames = 0;
    while (now() - started < limit) {
        lua_getglobal(L, "finished");
        const bool done = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (done)
            break;
        lua_rawgeti(L, LUA_REGISTRYINDEX, hooks);
        lua_getfield(L, -1, "frame");
        const double t0 = now();
        if (lua_pcall(L, 0, 0, 0) != 0) {
            std::printf("frame error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        const double spent = now() - t0;
        if (spent > slowest)
            slowest = spent;
        lua_pop(L, 1);
        ++frames;
        Sleep(16);
    }
    std::printf("[harness] %ld frames, slowest frame %.3f ms, %.1f s\n", frames, slowest * 1000, now() - started);
    lua_getglobal(L, "finished");
    const bool ok = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    std::printf("[harness] closing the state (gc cancels what is left)\n");
    const double c0 = now();
    lua_close(L);
    std::printf("[harness] lua_close took %.3f ms\n", (now() - c0) * 1000);
    std::printf("[harness] %s\n", ok ? "FINISHED" : "TIMED OUT");
    return ok ? 0 : 1;
}
