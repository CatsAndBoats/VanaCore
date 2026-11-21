#include "lua_net.hpp"

#include <lua.hpp>

#include <vanacore/version.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "net.hpp"

namespace vanacore::lua_net {
namespace {

// What one addon state owns in the engine; its __gc (the state closing) cancels everything the addon had going.
struct Mailbox {
    net::Owner owner = 0;
};

int mailbox_gc(lua_State* L)
{
    auto* mailbox = static_cast<Mailbox*>(lua_touserdata(L, 1));
    if (mailbox != nullptr && mailbox->owner != 0) {
        net::destroy(mailbox->owner);
        mailbox->owner = 0;
    }
    return 0;
}

net::Owner owner(lua_State* L)
{
    return static_cast<Mailbox*>(lua_touserdata(L, lua_upvalueindex(1)))->owner;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Reading a spec table

std::string field_string(lua_State* L, int table, const char* name, const char* fallback = "")
{
    lua_getfield(L, table, name);
    std::string value = fallback;
    if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) {
        size_t size = 0;
        const char* text = lua_tolstring(L, -1, &size);
        value.assign(text, size);
    }
    lua_pop(L, 1);
    return value;
}

double field_number(lua_State* L, int table, const char* name, double fallback)
{
    lua_getfield(L, table, name);
    const double value = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

bool field_bool(lua_State* L, int table, const char* name, bool fallback)
{
    lua_getfield(L, table, name);
    const bool value = lua_type(L, -1) == LUA_TBOOLEAN ? lua_toboolean(L, -1) != 0 : fallback;
    lua_pop(L, 1);
    return value;
}

net::Headers field_headers(lua_State* L, int table)
{
    net::Headers headers;
    lua_getfield(L, table, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)) {
                size_t size = 0;
                const char* value = lua_tolstring(L, -1, &size);
                headers.emplace_back(lua_tostring(L, -2), std::string(value, size));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return headers;
}

std::uint64_t size_limit(double value, std::uint64_t fallback)
{
    return value >= 1 ? static_cast<std::uint64_t>(value) : fallback;
}

int fail(lua_State* L, const std::string& error)
{
    lua_pushnil(L);
    lua_pushlstring(L, error.data(), error.size());
    return 2;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// The calls

int net_http(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    net::HttpRequest request;
    request.url = field_string(L, 1, "url");
    request.method = field_string(L, 1, "method", "GET");
    request.headers = field_headers(L, 1);
    request.body = field_string(L, 1, "body");
    request.timeout = field_number(L, 1, "timeout", request.timeout);
    request.max_time = field_number(L, 1, "max_time", request.max_time);
    request.follow = field_bool(L, 1, "follow", true);
    request.max_redirects = static_cast<int>(field_number(L, 1, "max_redirects", request.max_redirects));
    request.save = field_string(L, 1, "save");
    request.insecure = field_bool(L, 1, "insecure", false);
    // a download to a file isn't held in memory, so it has no reason to stop at the 16 MB a body is
    request.max_size = size_limit(field_number(L, 1, "max_size", 0), request.save.empty() ? request.max_size : (1ull << 40));
    net::Id id = 0;
    std::string error;
    if (!net::http(owner(L), std::move(request), id, error))
        return fail(L, error);
    lua_pushnumber(L, id);
    return 1;
}

int net_websocket(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    net::WebSocketRequest request;
    request.url = field_string(L, 1, "url");
    request.headers = field_headers(L, 1);
    request.timeout = field_number(L, 1, "timeout", request.timeout);
    request.keepalive = field_number(L, 1, "keepalive", request.keepalive);
    request.insecure = field_bool(L, 1, "insecure", false);
    request.max_size = size_limit(field_number(L, 1, "max_size", 0), request.max_size);
    lua_getfield(L, 1, "protocols");
    if (lua_istable(L, -1)) {
        for (int i = 1;; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 1);
                break;
            }
            request.protocols.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    net::Id id = 0;
    std::string error;
    if (!net::websocket(owner(L), std::move(request), id, error))
        return fail(L, error);
    lua_pushnumber(L, id);
    return 1;
}

int net_cancel(lua_State* L)
{
    net::cancel(owner(L), static_cast<net::Id>(luaL_checknumber(L, 1)));
    return 0;
}

int net_send(lua_State* L)
{
    size_t size = 0;
    const char* data = luaL_checklstring(L, 2, &size);
    std::string error;
    if (!net::send(owner(L), static_cast<net::Id>(luaL_checknumber(L, 1)), std::string(data, size), lua_toboolean(L, 3) != 0, error))
        return fail(L, error);
    lua_pushboolean(L, 1);
    return 1;
}

int net_close(lua_State* L)
{
    size_t size = 0;
    const char* reason = luaL_optlstring(L, 3, "", &size);
    net::close(owner(L), static_cast<net::Id>(luaL_checknumber(L, 1)), static_cast<int>(luaL_optnumber(L, 2, 1000)),
               std::string(reason, size));
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Events as tables

void set_string(lua_State* L, const char* name, const std::string& value)
{
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, name);
}

void set_number(lua_State* L, const char* name, double value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}

void set_bool(lua_State* L, const char* name, bool value)
{
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, name);
}

std::string lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string trim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
        --end;
    return text.substr(begin, end - begin);
}

// The raw head of an answer ("HTTP/1.1 200 OK\r\nName: value\r\n...") as { name (lower case) = value } (a name that comes
// twice joined with ", ") and, apart, the Set-Cookie lines, which can't be joined.
void push_headers(lua_State* L, const std::string& raw)
{
    lua_createtable(L, 0, 16);
    const int headers = lua_gettop(L);
    lua_newtable(L);
    const int cookies = lua_gettop(L);
    int cookie_count = 0;
    size_t at = raw.find('\n'); // past the status line
    while (at != std::string::npos && at + 1 < raw.size()) {
        const size_t begin = at + 1;
        const size_t end = raw.find('\n', begin);
        const std::string line = raw.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        at = end;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0)
            continue;
        const std::string name = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "set-cookie") {
            lua_pushlstring(L, value.data(), value.size());
            lua_rawseti(L, cookies, ++cookie_count);
        }
        lua_getfield(L, headers, name.c_str());
        if (lua_type(L, -1) == LUA_TSTRING) {
            const std::string joined = std::string(lua_tostring(L, -1)) + ", " + value;
            lua_pushlstring(L, joined.data(), joined.size());
        } else {
            lua_pushlstring(L, value.data(), value.size());
        }
        lua_remove(L, -2);
        lua_setfield(L, headers, name.c_str());
    }
    if (cookie_count > 0)
        lua_setfield(L, headers, "set_cookies"); // pops the list
    else
        lua_pop(L, 1);
    // the headers table is on top
}

void push_event(lua_State* L, const net::Event& event)
{
    using Kind = net::Event::Kind;
    lua_createtable(L, 0, 8);
    set_number(L, "id", event.id);
    switch (event.kind) {
    case Kind::HttpResponse:
        set_string(L, "kind", "response");
        set_number(L, "status", event.status);
        set_string(L, "reason", event.text);
        set_string(L, "url", event.url);
        push_headers(L, event.headers);
        lua_setfield(L, -2, "headers");
        if (!event.saved.empty())
            set_string(L, "saved", event.saved);
        else
            set_string(L, "body", event.data);
        break;
    case Kind::HttpError:
        set_string(L, "kind", "failed");
        set_string(L, "code", event.code);
        set_string(L, "message", event.text);
        break;
    case Kind::WsOpen:
        set_string(L, "kind", "open");
        set_string(L, "url", event.url);
        set_string(L, "protocol", event.text);
        break;
    case Kind::WsMessage:
        set_string(L, "kind", "message");
        set_string(L, "data", event.data);
        set_bool(L, "binary", event.flag);
        break;
    case Kind::WsError:
        set_string(L, "kind", "error");
        set_string(L, "code", event.code);
        set_string(L, "message", event.text);
        if (event.status != 0)
            set_number(L, "status", event.status);
        break;
    case Kind::WsClose:
        set_string(L, "kind", "close");
        set_number(L, "code", event.status);
        set_string(L, "reason", event.text);
        set_bool(L, "clean", event.flag);
        break;
    }
}

constexpr size_t kEventsPerFrame = 256;                  // the rest wait for the next frame
constexpr size_t kBytesPerFrame = 8u * 1024 * 1024;

int net_poll(lua_State* L)
{
    std::vector<net::Event> events;
    net::poll(owner(L), events, kEventsPerFrame, kBytesPerFrame);
    if (events.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(events.size()), 0);
    for (size_t i = 0; i < events.size(); ++i) {
        push_event(L, events[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

int net_info(lua_State* L)
{
    const net::Stats stats = net::stats(owner(L));
    lua_createtable(L, 0, 4);
    set_number(L, "requests", stats.requests);
    set_number(L, "sockets", stats.sockets);
    set_number(L, "waiting", stats.waiting);
    set_number(L, "workers", net::workers());
    return 1;
}

} // namespace

void push(lua_State* L)
{
    static bool agent_set = false;
    if (!agent_set) {
        agent_set = true;
        net::set_user_agent(std::wstring(L"VanaCore/") + kVersion);
    }

    auto* mailbox = static_cast<Mailbox*>(lua_newuserdata(L, sizeof(Mailbox)));
    mailbox->owner = net::create();
    lua_newtable(L);
    lua_pushcfunction(L, &mailbox_gc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    // kept in the registry, so it lives (and its __gc runs) exactly as long as the state does
    lua_pushvalue(L, -1);
    static const char kMailboxKey = 0;
    lua_pushlightuserdata(L, const_cast<char*>(&kMailboxKey));
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    const struct {
        const char* name = nullptr;
        lua_CFunction function = nullptr;
    } calls[] = {
        {"http", &net_http},   {"websocket", &net_websocket}, {"cancel", &net_cancel}, {"send", &net_send},
        {"close", &net_close}, {"poll", &net_poll},           {"info", &net_info},
    };
    for (const auto& call : calls) {
        lua_pushvalue(L, -2); // the mailbox, the closure's upvalue
        lua_pushcclosure(L, call.function, 1);
        lua_setfield(L, -2, call.name);
    }
    lua_remove(L, -2); // the mailbox
}

} // namespace vanacore::lua_net
