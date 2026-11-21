#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Runs a Lua file with VanaCore's patched LuaJIT: luarun file.lua [args...] (arg[1..] as given). */
int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("luarun file.lua [args]\n");
        return 2;
    }
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    for (int i = 1; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "arg");
    int failed = luaL_dofile(L, argv[1]);
    if (failed)
        printf("%s\n", lua_tostring(L, -1));
    lua_close(L);
    return failed ? 1 : 0;
}
