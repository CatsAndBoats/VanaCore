#include "lua_ashita.hpp"

#include <windows.h>

#include <shellapi.h>

#include <lua.hpp>

#include <cstdint>
#include <map>
#include <regex>
#include <string>
#include <string_view>

#include "chat.hpp"
#include "compat/ashita/host.hpp"
#include "gui.hpp"

namespace vanacore::lua_ashita {
namespace {

std::wstring widen(const char* text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1)
        return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), length);
    out.resize(static_cast<size_t>(length) - 1);
    return out;
}

void set_function(lua_State* L, const char* name, lua_CFunction function)
{
    lua_pushcfunction(L, function);
    lua_setfield(L, -2, name);
}

// ashita.object(name[, interface]): the object Ashita plugins get by that name ("fonts", "primitives", "resources", "inventory",
// "device"), interface 4.16's or (4.30) 4.30's, a light userdata for compat/ashita.lua's FFI, or nil.
int ashita_object(lua_State* L)
{
    void* object = ashita::addon_object(luaL_checkstring(L, 1), luaL_optnumber(L, 2, 4.16) > 4.2);
    if (object == nullptr)
        lua_pushnil(L);
    else
        lua_pushlightuserdata(L, object);
    return 1;
}

// ashita.string(table, index | text, [language]): the resource manager's GetString: the string at that index (nil if
// none), or the index of that text (-1 if none); language 0 the default, 1 Japanese, 2 English.
int ashita_string(lua_State* L)
{
    const char* table = luaL_checkstring(L, 1);
    const auto language = static_cast<uint32_t>(luaL_optinteger(L, 3, 0));
    if (lua_type(L, 2) == LUA_TSTRING) {
        lua_pushinteger(L, ashita::resource_string_index(table, lua_tostring(L, 2), language));
        return 1;
    }
    const char* text = ashita::resource_string(table, static_cast<uint32_t>(luaL_checkinteger(L, 2)), language);
    if (text == nullptr)
        lua_pushnil(L);
    else
        lua_pushstring(L, text);
    return 1;
}

// ashita.item(id | name[, language]): the resource manager's GetItemById or GetItemByName for addons, a light userdata for
// compat/ashita.lua's FFI, or nil: the item without its Bitmap (unless a plugin had the whole one made), which
// ashita.item_icon(id) gives as a string ('' for none), read from the item's DAT.
int ashita_item(lua_State* L)
{
    const void* item = lua_type(L, 1) == LUA_TSTRING
                           ? ashita::resource_addon_item_by_name(lua_tostring(L, 1), static_cast<uint32_t>(luaL_optinteger(L, 2, 0)))
                           : ashita::resource_addon_item(static_cast<uint32_t>(luaL_checkinteger(L, 1)));
    if (item == nullptr)
        lua_pushnil(L);
    else
        lua_pushlightuserdata(L, const_cast<void*>(item));
    return 1;
}

int ashita_item_icon(lua_State* L)
{
    const std::string icon = ashita::resource_item_icon(static_cast<uint32_t>(luaL_checkinteger(L, 1)));
    lua_pushlstring(L, icon.data(), icon.size());
    return 1;
}

// ashita.item_stub(address): whether an item ashita.item gave (its address as a number) is VanaCore's own, without its
// Bitmap, and so has its icon from ashita.item_icon; false for a whole one a plugin's hook gave (its Bitmap is in it).
int ashita_item_stub(lua_State* L)
{
    lua_pushboolean(L, ashita::resource_item_is_stub(static_cast<std::uintptr_t>(luaL_checknumber(L, 1))));
    return 1;
}

// ashita.win32(name, value): the Win32 calls IAshitaCore passes on for addons: SetFocus(window) and
// SetForegroundWindow(window), the window as a number; GetSystemMetrics(index). The result as a number.
int ashita_win32(lua_State* L)
{
    const std::string name = luaL_checkstring(L, 1);
    const auto value = static_cast<std::uintptr_t>(luaL_checknumber(L, 2));
    if (name == "SetFocus")
        lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(SetFocus(reinterpret_cast<HWND>(value)))));
    else if (name == "SetForegroundWindow")
        lua_pushnumber(L, SetForegroundWindow(reinterpret_cast<HWND>(value)));
    else if (name == "GetSystemMetrics")
        lua_pushnumber(L, GetSystemMetrics(static_cast<int>(value)));
    else
        return luaL_error(L, "ashita.win32: %s isn't one of VanaCore's", name.c_str());
    return 1;
}

// ashita.parse_auto_translate(text, brackets): the text with its auto-translate phrases spelled out by the game
int ashita_parse_auto_translate(lua_State* L)
{
    size_t size = 0;
    const char* text = luaL_checklstring(L, 1, &size);
    const std::string out = chat::parse_auto_translate(std::string_view(text, size), lua_toboolean(L, 2) != 0);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// ashita.gui_visible([visible]): whether ImGui's windows are drawn (IGuiManager's Get/SetVisible)
int ashita_gui_visible(lua_State* L)
{
    if (lua_gettop(L) >= 1)
        gui::set_visible(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, gui::visible());
    return 1;
}

// ashita.regex, as Ashita's (std::regex, ECMAScript): search(text, pattern) every match, each { the match, its groups
// ... } (nil if none); match(text, pattern) the whole text's, as one of those (nil if it doesn't); replace(text,
// pattern, with) ($1 for a group); split(text, pattern) the pieces between. A bad pattern: nil (search, match), the
// text as it was (replace), the text alone (split). Patterns kept compiled (chat addons run them on every line).
const std::regex* regex_of(lua_State* L, int index)
{
    static std::map<std::string, std::regex> cache;
    const std::string pattern = luaL_checkstring(L, index);
    const auto found = cache.find(pattern);
    if (found != cache.end())
        return &found->second;
    try {
        if (cache.size() >= 256)
            cache.clear();
        return &cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript)).first->second;
    } catch (const std::regex_error&) {
        return nullptr;
    }
}

void push_match(lua_State* L, const std::smatch& m)
{
    lua_createtable(L, static_cast<int>(m.size()), 0);
    for (size_t g = 0; g < m.size(); ++g) {
        const std::string part = m[g].str();
        lua_pushlstring(L, part.data(), part.size());
        lua_rawseti(L, -2, static_cast<int>(g + 1));
    }
}

int regex_search(lua_State* L)
{
    const std::string text = luaL_checkstring(L, 1);
    const std::regex* re = regex_of(L, 2);
    if (re == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    int n = 0;
    lua_newtable(L);
    try {
        for (auto it = std::sregex_iterator(text.begin(), text.end(), *re); it != std::sregex_iterator(); ++it) {
            push_match(L, *it);
            lua_rawseti(L, -2, ++n);
        }
    } catch (const std::regex_error&) {
    }
    if (n == 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    return 1;
}

int regex_match(lua_State* L)
{
    const std::string text = luaL_checkstring(L, 1);
    const std::regex* re = regex_of(L, 2);
    std::smatch m;
    if (re == nullptr || !std::regex_match(text, m, *re)) {
        lua_pushnil(L);
        return 1;
    }
    push_match(L, m);
    return 1;
}

int regex_replace(lua_State* L)
{
    const std::string text = luaL_checkstring(L, 1);
    const std::regex* re = regex_of(L, 2);
    const std::string with = luaL_optstring(L, 3, "");
    if (re == nullptr) {
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    const std::string out = std::regex_replace(text, *re, with);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int regex_split(lua_State* L)
{
    const std::string text = luaL_checkstring(L, 1);
    const std::regex* re = regex_of(L, 2);
    lua_newtable(L);
    if (re == nullptr) {
        lua_pushlstring(L, text.data(), text.size());
        lua_rawseti(L, -2, 1);
        return 1;
    }
    int n = 0;
    for (auto it = std::sregex_token_iterator(text.begin(), text.end(), *re, -1); it != std::sregex_token_iterator(); ++it) {
        const std::string part = it->str();
        lua_pushlstring(L, part.data(), part.size());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// ashita.execute(file, parameters): the file opened as Windows opens it (luashitacast's profile in its editor)
int ashita_execute(lua_State* L)
{
    const std::wstring file = widen(luaL_checkstring(L, 1));
    const std::wstring parameters = widen(luaL_optstring(L, 2, ""));
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", file.c_str(), parameters.empty() ? nullptr : parameters.c_str(), nullptr, SW_SHOWNORMAL));
    lua_pushboolean(L, result > 32);
    return 1;
}

// ashita.console(show): the game's console window (with attached=1) shown or hidden
int ashita_console(lua_State* L)
{
    if (const HWND console = GetConsoleWindow())
        ShowWindow(console, lua_toboolean(L, 1) ? SW_SHOW : SW_HIDE);
    return 0;
}

} // namespace

void push(lua_State* L, double ashita_interface, double ashita_libs)
{
    lua_newtable(L);
    set_function(L, "object", &ashita_object);
    set_function(L, "string", &ashita_string);
    set_function(L, "item", &ashita_item);
    set_function(L, "item_icon", &ashita_item_icon);
    set_function(L, "item_stub", &ashita_item_stub);
    set_function(L, "win32", &ashita_win32);
    set_function(L, "parse_auto_translate", &ashita_parse_auto_translate);
    set_function(L, "gui_visible", &ashita_gui_visible);
    set_function(L, "execute", &ashita_execute);
    set_function(L, "console", &ashita_console);
    lua_pushnumber(L, ashita_interface);
    lua_setfield(L, -2, "interface");
    lua_pushnumber(L, ashita_libs);
    lua_setfield(L, -2, "libs");
    lua_newtable(L);
    set_function(L, "search", &regex_search);
    set_function(L, "match", &regex_match);
    set_function(L, "replace", &regex_replace);
    set_function(L, "split", &regex_split);
    lua_setfield(L, -2, "regex");
}

} // namespace vanacore::lua_ashita
