// vana.addons' package functions (addonpkg.hpp), for /vana's install prompt (addons\vanacore\vana\install.lua). Game thread.

#include "addonpkg_lua.hpp"

#include <windows.h>
#include <commdlg.h>

#include <lua.hpp>

#include <iterator>
#include <string>

#include "addonpkg.hpp"
#include "addons.hpp"

namespace vanacore::addonpkg {

namespace {

void field(lua_State* L, const char* name, const std::string& value)
{
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, name);
}

void field(lua_State* L, const char* name, double value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}

bool loaded(const std::string& name)
{
    for (const auto& addon : addons::loaded())
        if (addon.source == "vanacore" && _stricmp(addon.name.c_str(), name.c_str()) == 0)
            return true;
    return false;
}

// vana.addons.package_waiting(): the next package in addons\install\ (claimed by this game until package_done), or nil
int package_waiting(lua_State* L)
{
    const std::wstring path = next_waiting();
    if (path.empty())
        return 0;
    const std::string text = utf8(path);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// vana.addons.package_info(path): what the package holds, read and checked (nothing written): { path, name, title, version,
// author, description, homepage, from, files, bytes, installed_version (nil when it isn't installed), loaded, problem (nil
// when it can be installed) }; nil and why when it isn't a package
int package_info(lua_State* L)
{
    Package p;
    if (!inspect(home(), wide(luaL_checkstring(L, 1)), p)) {
        lua_pushnil(L);
        lua_pushlstring(L, p.problem.data(), p.problem.size());
        return 2;
    }
    lua_newtable(L);
    field(L, "path", utf8(p.path));
    field(L, "name", p.name);
    field(L, "title", p.title.empty() ? p.name : p.title);
    field(L, "version", p.version);
    field(L, "author", p.author);
    field(L, "description", p.description);
    field(L, "homepage", p.homepage);
    field(L, "from", p.from);
    field(L, "files", static_cast<double>(p.files));
    field(L, "bytes", static_cast<double>(p.bytes));
    if (!p.installed_version.empty())
        field(L, "installed_version", p.installed_version);
    if (!p.problem.empty())
        field(L, "problem", p.problem);
    lua_pushboolean(L, loaded(p.name));
    lua_setfield(L, -2, "loaded");
    return 1;
}

// vana.addons.package_install(path): installs it (the addon unloaded first: refused while it's loaded). true, or nil and why.
int package_install(lua_State* L)
{
    Package p;
    std::string error;
    if (!inspect(home(), wide(luaL_checkstring(L, 1)), p))
        error = p.problem;
    else if (loaded(p.name))
        error = p.name + " is loaded: unload it first";
    else if (install(home(), p, error))
        error.clear();
    if (error.empty()) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushlstring(L, error.data(), error.size());
    return 2;
}

// vana.addons.package_done(path): a package offered is done with (moved to addons\install\done\ if it waited there)
int package_done(lua_State* L)
{
    finish(wide(luaL_checkstring(L, 1)));
    return 0;
}

// vana.addons.package_release(path): a package's claim let go, unanswered (vana unloading with its prompt up): offered again
int package_release(lua_State* L)
{
    release(wide(luaL_checkstring(L, 1)));
    return 0;
}

// vana.addons.package_pick(): Windows' Open dialog for a .vcaddon (the game's thread waits in it): its path, or nil
int package_pick(lua_State* L)
{
    wchar_t picked[MAX_PATH * 2] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = GetForegroundWindow();
    dialog.lpstrFilter = L"VanaCore addon packages (*.vcaddon)\0*.vcaddon\0Zip files (*.zip)\0*.zip\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = picked;
    dialog.nMaxFile = static_cast<DWORD>(std::size(picked));
    dialog.lpstrTitle = L"Install an addon package";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog))
        return 0;
    const std::string text = utf8(picked);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// vana.addons.load_at_start(name): scripts\boot.txt loads it at each start (a line added unless one does). true if it does.
int package_load_at_start(lua_State* L)
{
    lua_pushboolean(L, load_at_start(home(), luaL_checkstring(L, 1)));
    return 1;
}

} // namespace

void open_lua(lua_State* L)
{
    const luaL_Reg functions[] = {
        {"package_waiting", &package_waiting}, {"package_info", &package_info}, {"package_install", &package_install},
        {"package_done", &package_done},       {"package_release", &package_release}, {"package_pick", &package_pick},
        {"load_at_start", &package_load_at_start},
    };
    for (const luaL_Reg& f : functions) {
        lua_pushcfunction(L, f.func);
        lua_setfield(L, -2, f.name);
    }
}

} // namespace vanacore::addonpkg
