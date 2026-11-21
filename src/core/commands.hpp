#pragma once

#include <string_view>

// VanaCore's commands switched off (/vana > Client > Commands, [commands] <name>=0): VanaCore leaves such a command alone,
// so it goes on to the plugins, the addons and the game as if VanaCore had none, for a player whose own addon or plugin
// has a command of that name (/copy, /cancel, /freecam). /name and //name go together. The /vana window's own (/hud,
// /gamepad, /addons, /plugins) read the same keys (vana.lua). Those VanaCore can't do without (/addon, /plugin, /exec,
// /vana, /wait, /bind, /alias) stay on whatever the file says.
namespace vanacore::commands {

void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// False if `word` (a line's first word, "/copy" or "//copy", any case) is a command switched off.
bool on(std::string_view word);

} // namespace vanacore::commands
