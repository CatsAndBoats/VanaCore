#pragma once

struct lua_State;

namespace vanacore::addonpkg {

// The package functions (package_waiting, package_info, package_install, package_done, package_release, package_pick,
// load_at_start) into the table on top of `L`'s stack: vana.addons.
void open_lua(lua_State* L);

} // namespace vanacore::addonpkg
