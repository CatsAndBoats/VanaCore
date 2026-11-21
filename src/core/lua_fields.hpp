#pragma once

#include <lua.hpp>

#include <string>

// Fields into the Lua table on the top of the stack, for the vana.* tables the API files build (lua_entity.cpp,
// lua_gamewin.cpp).
namespace vanacore::lua_fields {

inline void set_number(lua_State* L, const char* key, double value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

inline void set_bool(lua_State* L, const char* key, bool value)
{
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, key);
}

inline void set_string(lua_State* L, const char* key, const std::string& value)
{
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
}

} // namespace vanacore::lua_fields
