#pragma once

// Resources in VanaCore.Core.dll (VanaCore.Core.rc): the Lua scripts addons run first.
#define IDR_LUA_VANA 101     // lua\vana.lua: every addon
#define IDR_LUA_ASHITA 102   // lua\compat\ashita.lua: addons written for Ashita
#define IDR_LUA_WINDOWER 103 // lua\compat\windower.lua: addons written for Windower
#define IDR_LUA_SETTINGS 104 // lua\settings.lua: every addon (vana.settings, vana.json)
#define IDR_LUA_NET 105      // lua\net.lua: every addon (vana.promise, vana.http, vana.ws, vana.async)

// addonpkg.manifest: Windows' common controls 6, for the addon package dialogs (addonpkg_win.cpp) that rundll32 shows. A
// resource id of its own, not 2: Windows only takes it up when addonpkg_win.cpp activates it, never at the DLL's load.
#define IDR_ADDONPKG_MANIFEST 120
