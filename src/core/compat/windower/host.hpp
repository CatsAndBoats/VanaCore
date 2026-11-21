#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Windower 4 plugins hosted by VanaCore: VanaCore implements the objects Windower gives them
// (abi.hpp) over its own chat, text, image and packet handling. Game thread only, except where noted.
//
// Plugins see VanaCore's folder as Windower's. Windower keeps its plugins' files beside them in
// <Windower>\plugins (settings\, resources\, a folder per plugin); VanaCore's Windower plugins
// live in plugins\windower, so a Windower plugin's file access under <VanaCore>\plugins\ is sent
// to plugins\windower\ (settings\FFXIDB.xml is plugins\windower\settings\FFXIDB.xml).
namespace vanacore::windower {

// `install_dir` is VanaCore's folder with a trailing backslash (ANSI). From VanaCoreInstall: it
// watches for plugin DLLs loading, to redirect their file access before their code runs.
void install(const std::string& install_dir);

// Loads the plugin DLL at `path`, named `display` (as its file spells it). False with the reason in
// `error`.
bool load(const std::string& display, const std::string& path, std::string& error);
// Unloads plugin `name` (any case). False if it isn't loaded.
bool unload(const std::string& name);
// The game is closing: every plugin unloaded, the last first (left as they are while one is being called).
void shutdown();
bool is_loaded(const std::string& name);

struct Info {
    std::string display;
    std::string name;   // as the plugin gives it
    std::string author;
    std::uint32_t interface_version = 0;
    bool crashed = false;
};
bool info(const std::string& name, Info& out);
std::vector<Info> list();

// A command line about to run: "//<plugin> args" goes to that plugin. True when a plugin took it.
bool on_command(const std::string& line);
// A mouse event (plugins::on_mouse): each plugin's Mouse, `blocked` as it goes. True when one took it.
bool on_mouse(int type, int x, int y, int delta, bool blocked);
// A key's window message (plugins::on_key): each Windower 4 plugin's Keyboard, with the key's DirectInput code and
// whether it's down (its arguments as Windower addons' 'keyboard' event has them: no plugin seen so far does anything
// with them). True when one took it.
bool on_key(WPARAM key, LPARAM flags, bool blocked);

// A "//" command nothing else handled (the line without "//"): plugins' UnhandledCommand. True
// when one took it.
bool on_unhandled(std::string_view line);

void on_device(void* device);
// Each frame, inside its scene (between text::begin and text::end): plugins' PreRender (Windower 3
// plugins' frame call), their text and primitives, their PostRender.
void draw(void* device);

// Each buffer the game receives, decrypted, as the game will read it: Windower's game state.
void on_received(const std::uint8_t* buffer, std::size_t size);

// True when a Windower plugin is loaded (they see every packet).
bool wants_packets();
// A packet (see plugins::on_packet). True when a plugin blocked it.
bool on_packet(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::uint8_t* modified,
               bool injected, bool blocked);

// Where a Windower plugin's path really is: <VanaCore>\plugins\<x>... is
// <VanaCore>\plugins\windower\<x>... unless <x> is ashita or windower. Other paths unchanged.
std::wstring redirect(std::wstring_view path);

} // namespace vanacore::windower
