#include "lua_maxhp.hpp"

#include <lua.hpp>
#include <windows.h>

#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

#include "maxhp_ask.hpp"

namespace vanacore::lua_maxhp {
namespace {

// Answers that came, until the addon that asked collects them. One that never does (it was unloaded) goes after this long.
constexpr std::uint64_t kKeepMs = 30000;

struct Collected {
    std::vector<maxhp::Answer> answers;
    std::uint64_t at = 0;
};
std::unordered_map<unsigned, Collected> g_collected;

void store(unsigned token, const maxhp::Answer* answers, unsigned count)
{
    const std::uint64_t now = GetTickCount64();
    for (auto it = g_collected.begin(); it != g_collected.end();)
        it = now - it->second.at > kKeepMs ? g_collected.erase(it) : std::next(it);
    g_collected[token] = {std::vector<maxhp::Answer>(answers, answers + count), now};
}

int refuse(lua_State* L, const char* why)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

int request(lua_State* L)
{
    std::uint32_t ids[maxhp::kMaxIds];
    unsigned count = 0;
    if (lua_istable(L, 1)) {
        const int length = static_cast<int>(lua_objlen(L, 1));
        for (int i = 1; i <= length; ++i) {
            if (count == maxhp::kMaxIds)
                return refuse(L, "at most 8 ids in one request");
            lua_rawgeti(L, 1, i);
            if (!lua_isnumber(L, -1))
                return refuse(L, "the ids must be numbers (server ids)");
            ids[count++] = static_cast<std::uint32_t>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
    } else if (lua_isnumber(L, 1)) {
        ids[count++] = static_cast<std::uint32_t>(lua_tonumber(L, 1));
    } else {
        return refuse(L, "ask with a server id, or a list of them");
    }
    unsigned token = 0;
    const char* why = maxhp::ask(ids, count, &store, {}, &token);
    if (why != nullptr)
        return refuse(L, why);
    lua_pushinteger(L, token);
    return 1;
}

int answers(lua_State* L)
{
    const auto found = g_collected.find(static_cast<unsigned>(luaL_checkinteger(L, 1)));
    if (found == g_collected.end()) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(found->second.answers.size()), 0);
    int index = 0;
    for (const maxhp::Answer& answer : found->second.answers) {
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, answer.id);
        lua_setfield(L, -2, "id");
        lua_pushnumber(L, answer.hp);
        lua_setfield(L, -2, "hp");
        lua_pushnumber(L, answer.hp_max);
        lua_setfield(L, -2, "hp_max");
        lua_pushstring(L, maxhp::status_name(answer.status));
        lua_setfield(L, -2, "status");
        lua_rawseti(L, -2, ++index);
    }
    g_collected.erase(found);
    return 1;
}

} // namespace

void add(lua_State* L)
{
    lua_pushcfunction(L, &request);
    lua_setfield(L, -2, "native_request_max_hp");
    lua_pushcfunction(L, &answers);
    lua_setfield(L, -2, "native_max_hp_answers");
}

} // namespace vanacore::lua_maxhp
