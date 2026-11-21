#include "lua_gamewin.hpp"

#include <windows.h>

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gameui.hpp"
#include "gamewin.hpp"
#include "lua_api.hpp"
#include "lua_fields.hpp"
#include "ui.hpp"
#include "uispace.hpp"

namespace vanacore::lua_gamewin {
namespace {

// Where the interface's units are on the screen addons draw on (ui.screen's pixels); while the interface's area isn't known,
// the units as they are (scale 1)
struct Screen {
    unsigned width = 0, height = 0;
    bool known = false;
};

Screen screen()
{
    Screen s;
    ui::screen(s.width, s.height);
    s.known = s.width > 0 && s.height > 0 && uispace::find(GetModuleHandleW(L"FFXiMain.dll"));
    return s;
}

using lua_fields::set_bool;
using lua_fields::set_number;
using lua_fields::set_string;

// x, y, w, h of `rect` (the interface's units) into the table on the top, as the screen's pixels; the pixels a unit
double set_rect(lua_State* L, const Screen& s, const gamewin::Rect& rect)
{
    float x0 = static_cast<float>(rect.left), y0 = static_cast<float>(rect.top);
    float x1 = static_cast<float>(rect.right), y1 = static_cast<float>(rect.bottom);
    float scale = 1.0f;
    if (s.known) {
        float px = 0, py = 0, qx = 0, qy = 0;
        if (uispace::to_pixels(x0, y0, s.width, s.height, px, py, scale) && uispace::to_pixels(x1, y1, s.width, s.height, qx, qy, scale))
            x0 = px, y0 = py, x1 = qx, y1 = qy;
        else
            scale = 1.0f;
    }
    set_number(L, "x", std::round(x0 * 10.0f) / 10.0f);
    set_number(L, "y", std::round(y0 * 10.0f) / 10.0f);
    set_number(L, "w", std::round((x1 - x0) * 10.0f) / 10.0f);
    set_number(L, "h", std::round((y1 - y0) * 10.0f) / 10.0f);
    return scale;
}

// The colour the game draws `colour` as (0xAARRGGBB, 0x80 full: drawn twice as bright), '#RRGGBB'
void set_colour(lua_State* L, std::uint32_t colour)
{
    char text[8];
    const auto channel = [colour](int shift) { return std::min(0xFFu, ((colour >> shift) & 0xFFu) * 2u); };
    std::snprintf(text, sizeof text, "#%02X%02X%02X", channel(16), channel(8), channel(0));
    set_string(L, "color", text);
}

void push_rows(lua_State* L, const Screen& s, const std::vector<gamewin::Row>& rows)
{
    lua_createtable(L, static_cast<int>(rows.size()), 0);
    int i = 0;
    for (const gamewin::Row& row : rows) {
        lua_createtable(L, 0, 10);
        set_number(L, "id", row.id);
        set_rect(L, s, row.rect);
        set_string(L, "text", row.text);
        set_number(L, "state", row.state);
        set_bool(L, "cursor", row.cursor);
        set_bool(L, "disabled", row.disabled);
        set_bool(L, "drawn", row.drawn);
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "rows");
}

void push_lines(lua_State* L, const Screen& s, const std::vector<gamewin::Line>& lines)
{
    lua_createtable(L, static_cast<int>(lines.size()), 0);
    int i = 0;
    for (const gamewin::Line& line : lines) {
        lua_createtable(L, 0, 6);
        set_string(L, "text", line.text);
        set_rect(L, s, line.rect);
        set_colour(L, line.colour);
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "lines");
}

void push_pictures(lua_State* L, const Screen& s, const std::vector<gamewin::Picture>& pictures)
{
    lua_createtable(L, static_cast<int>(pictures.size()), 0);
    int i = 0;
    for (const gamewin::Picture& picture : pictures) {
        lua_createtable(L, 0, 6);
        set_string(L, "name", picture.name);
        set_rect(L, s, picture.rect);
        set_colour(L, picture.colour);
        lua_rawseti(L, -2, ++i);
    }
    lua_setfield(L, -2, "pictures");
}

// gameui.window(name): one of the game's windows as it stands ('menuwind', as menu_open names it), nil when it isn't up
int window(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    gamewin::Window w;
    if (!gamewin::read(name, w)) {
        lua_pushnil(L);
        return 1;
    }
    const Screen s = screen();
    lua_createtable(L, 0, 16);
    set_string(L, "name", w.name);
    set_number(L, "scale", set_rect(L, s, w.rect));
    set_number(L, "cursor", w.cursor);
    set_number(L, "count", w.count);
    set_bool(L, "focused", w.focused);
    set_bool(L, "closing", w.closing);
    set_bool(L, "hidden", w.hidden);
    set_bool(L, "drawn", w.drawn);
    push_rows(L, s, w.rows);
    push_lines(L, s, w.lines);
    push_pictures(L, s, w.pictures);
    return 1;
}

// gameui.windows(): the names of the game's windows up
int windows(lua_State* L)
{
    const std::vector<std::string> list = gamewin::names();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    int i = 0;
    for (const std::string& name : list) {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

// gameui.native_press(name, row, event): the window's cursor put on `row` (0 where it is), `event` given to it (0 none)
int press(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    const int row = static_cast<int>(luaL_optinteger(L, 2, 0));
    const int event = static_cast<int>(luaL_optinteger(L, 3, 0));
    const gamewin::Press result = gamewin::press(name, row, event);
    if (result == gamewin::Press::Done) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    if (result == gamewin::Press::NotUp)
        lua_pushfstring(L, "'%s' isn't up", name);
    else if (result == gamewin::Press::NoRow)
        lua_pushfstring(L, "'%s' has no row %d", name, row);
    else if (result == gamewin::Press::Busy)
        lua_pushfstring(L, "'%s' isn't taking keys now", name);
    else
        lua_pushstring(L, "the game's menu functions weren't found (vanacore.log says which)");
    return 2;
}

// gameui.native_watch(names): the windows this addon watches, all of them ({ 'configwi', 'prty1' }, '*' every one; {} none);
// lua/vana.lua keeps each watch's function and gives the union
int watch(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const lua_api::Context& c = lua_api::context(L);
    std::vector<std::string> names;
    for (int i = 1;; ++i) {
        lua_rawgeti(L, 1, i);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            break;
        }
        names.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    gamewin::watch(gameui::key(c.source, c.name), names);
    return 0;
}

} // namespace

void add(lua_State* L)
{
    lua_pushcfunction(L, &window);
    lua_setfield(L, -2, "window");
    lua_pushcfunction(L, &windows);
    lua_setfield(L, -2, "windows");
    lua_pushcfunction(L, &press);
    lua_setfield(L, -2, "native_press");
    lua_pushcfunction(L, &watch);
    lua_setfield(L, -2, "native_watch");
}

} // namespace vanacore::lua_gamewin
