#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chat.hpp"

// Plugins: native DLLs in <VanaCore>\plugins\ashita and plugins\windower, loaded and unloaded in
// game with /plugin load|unload|reload <name> [ashita|windower] and /plugin list. A name in both
// folders loads from ashita unless the command names the folder. Ashita v4 plugins (interface
// 4.16 or 4.30) run on VanaCore's implementation of Ashita's interfaces (compat/ashita), Windower 4
// plugins on its implementation of Windower's (compat/windower).
namespace vanacore::plugins {

void install(const wchar_t* install_dir, const wchar_t* settings_ini);

// [addons] plugins.silent read again (vanacore.ini changed: /vana's switch). silent(): no chat line as a plugin loads or
// unloads (errors still show), as Ashita's plugins.silent; set_silent() (an Ashita plugin's SetSilentPlugins) holds
// until the setting changes.
void reload(const wchar_t* ini);
bool silent();
void set_silent(bool silent);

// /plugin ... (or //plugin ...): `parts` is the line split into words.
void command(const std::vector<std::string>& parts);

// Loads plugin `name` from `kind`'s folder ("ashita" or "windower"), or the first that has it
// (empty `kind`), passing it `args`. False, having said why in the chat log, if it can't.
bool load(const std::string& name, const std::string& kind = "", const std::string& args = "");
bool unload(const std::string& name);

// Whether <name>.dll is in a plugins folder (what /plugin load <name> would find).
bool present(const std::string& name);

// A plugin held off while a VanaCore feature does its job (FrameFix while [client] framepacing is on: framepacing.cpp).
// hold() unloads it if it's loaded and refuses /plugin load for it (boot.txt's too), saying `why` in the chat log;
// release() lets it load again, and loads it if holding it unloaded it or refused a load. Game thread.
void hold(const std::string& name, const std::string& why);
void release(const std::string& name);

// The plugins loaded now: each its name as its file spells it, and its kind ("ashita" or "windower").
std::vector<std::pair<std::string, std::string>> loaded_list();

// A loaded plugin, for /vana stats: its kind in words ("Ashita plugin"), its version and author ("1.16 by Author, for
// Ashita interface 4.3"), and whether it crashed (it's disabled). False if it isn't loaded.
struct Details {
    std::string kind;
    std::string about;
    bool crashed = false;
};
bool details(const std::string& name, Details& out);
// How many times a plugin has loaded or unloaded: when it changes, game code one had rewritten may be
// the game's again (nameplates.cpp looks again for what Ashita's Nameplate plugin rewrites). Game thread.
unsigned changes();

// The game is closing (shutdown.cpp): every plugin unloaded, the last first, as /plugin unload does (without its chat
// line). Only from the game's thread: off it, they're left as they are.
void shutdown(bool as_game_thread);

// Every command line before the game sees it: plugins' command handlers. In Ashita the addons are its Addons plugin,
// priority 0 and loaded near the top of every boot script, so the plugins with a negative priority have a line before
// the addons and the rest after them (PacketFlow's /pf comes after partyfinder's). True when a plugin handled it, `by`
// its name.
enum class Commands { All, BeforeAddons, AfterAddons };
bool on_command(const std::string& line, int mode, bool injected, Commands which, std::string& by);
// A mouse event over the game window (see window.cpp: Windower's types, back buffer pixels; `wparam`
// as its window message has it). True when a plugin took it.
bool on_mouse(int type, int x, int y, int delta, WPARAM wparam);
// A key's window message (WM_KEYDOWN, WM_KEYUP and their WM_SYS forms) on the game window: the
// virtual key and the message's flags. True when a plugin blocked it.
bool on_key(WPARAM key, LPARAM flags);
// The game's DirectInput keyboard reads, filled (input.cpp): plugins may change them. `capacity` is
// how many events `data` has room for.
void on_keyboard_state(void* device, DWORD size, void* data);
void on_keyboard_data(void* device, DWORD object_size, void* data, DWORD* count, DWORD capacity, DWORD flags);

// A "//" command line nothing else took, without its "//" (Windower plugins' UnhandledCommand).
// True when a plugin handled it.
bool on_unhandled(std::string_view line);

// A line of text coming into the chat log or going to the game (chat::Text): Ashita plugins' HandleIncomingText or
// HandleOutgoingText. True: blocked.
bool on_text(bool incoming, chat::Text& text);

// Controllers (gamepad.cpp): Ashita plugins' IController and IXInput callbacks. The state and data ones get each read as
// it is; the button ones each change (true: blocked; the control and its value changed in place).
bool wants_gamepad();
void on_controller_state(void* device, DWORD size, void* data);
void on_controller_data(void* device, DWORD object_size, void* data, DWORD* count);
bool on_controller_button(std::uint32_t& offset, std::int32_t& value, bool injected);
void on_xinput_state(DWORD user, void* state);
bool on_xinput_button(std::uint32_t& button, std::int32_t& value, bool injected);

// A plugin event (IPluginManager::RaiseEvent, from a plugin or an Ashita addon): plugins' HandleEvent, and addons'
// 'plugin_event' at the next frame.
void raise_event(const std::string& name, const std::string& data);

// The game's Direct3D device is made (or remade).
void on_device(void* device);
// Present, before VanaCore draws its overlay.
void on_present(void* device, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty);
// The game's device begins (`begin`, after the real call) or ends (before it) a scene: Ashita plugins'
// Direct3DBeginScene and Direct3DEndScene. Not for the scenes VanaCore and the plugins begin in Present.
void on_scene(void* device, bool begin);
// Draws Ashita's font and primitive objects (plugins' and addons'), under what plugins and addons draw
// in their present events, as Ashita orders them: between text::begin and text::end.
void draw_objects(void* device);
// Draws Windower plugins' text and shapes (and their PreRender, PostRender): between text::begin and
// text::end.
void draw(void* device);

// Each buffer the game receives, after the plugins (decrypted: its header, then the packets), plugins
// loaded or not: what Windower plugins read as the game's state.
void on_received(const std::uint8_t* buffer, std::size_t size);

// True when a loaded plugin handles packets.
bool wants_packets();

// A packet the game received or is about to send (see packets.hpp). True when a plugin blocked it.
// `skip`: the Ashita plugins that don't see it (the ones that added it, see packets::inject).
bool on_packet(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::uint8_t* modified,
               std::uint32_t chunk_size, const std::uint8_t* chunk, bool injected, bool blocked,
               const std::vector<std::string>& skip = {});

} // namespace vanacore::plugins
