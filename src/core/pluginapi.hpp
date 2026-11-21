#pragma once

#include <windows.h>

#include <string>
#include <vector>

// VanaCore's plugin API (src/shared/vanacore/plugin_api.h, exported as VanaCoreGetPluginApi): what vana.gameui and
// vana.ui.game_on_top do for an addon, for an Ashita plugin (asked for by FancyChat's author, 2026-11-06, moving it to a
// plugin). A plugin is known by the one VanaCore is calling into on the game's thread (ashita::calling): its gameui key is
// "plugin/<name>", its ImGui windows' owner "plugin/<Name>" (as layout::owner_of has them). Game thread.
namespace vanacore::pluginapi {

// compat/ashita/host.cpp, as a plugin goes (unloaded, crashed, the game closing): all it asked for undone. `display` is its
// name as its file spells it.
void release(const std::string& display);

// chat.cpp's find_over, each frame: where the plugins that asked (game_on_top, game_on_top_areas) have their drawing, in the
// game's pixels: `chat_kept` those the game's chat logs don't go over (VANACORE_ON_TOP_NOT_CHAT_LOG), `others` the rest.
void over(std::vector<RECT>& chat_kept, std::vector<RECT>& others);

// phraselist.cpp: true while a plugin draws the chat line itself and has said where its caret is (chat_input_caret), in the
// game's pixels.
bool caret(float& x, float& top, float& bottom);

} // namespace vanacore::pluginapi
