#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Ashita v4 plugins (interface 4.16 or 4.30) hosted by VanaCore: VanaCore implements the
// interfaces they're given (abi.hpp) over its own game data, chat, text and packet handling.
// Game thread only, except where noted.
#include "../../chat.hpp"
#include "../../plugins.hpp"

namespace vanacore::ashita {

// `install_dir` is VanaCore's folder with a trailing backslash: plugins see it as Ashita's
// install path (config\, resources\ and the like are found under it).
void install(const std::string& install_dir, const std::wstring& settings_ini);

// Ashita addons (compat/ashita.lua) reach the objects interface 4.16 plugins get (4.30's with `v430`: for the methods
// only 4.30 has), through LuaJIT's FFI: "fonts", "primitives", "resources", "inventory", or "device" (the game's
// Direct3D 8 device, once it's made). nullptr for another name.
void* addon_object(const std::string& name, bool v430 = false);
// The resource manager's string tables (IResourceManager::GetString), for addons: the string at `index` (null if none),
// or the index of `text` (-1); language 0 the default, 1 Japanese, 2 English.
const char* resource_string(const char* table, uint32_t index, uint32_t language);
// ITarget::SetTarget: entity `index` targeted as the game's own call sites do it. False for one with no actor (out of
// render range) or when the game's target isn't found. Game thread.
bool set_target(uint32_t index);
int32_t resource_string_index(const char* table, const char* text, uint32_t language);
// An item's name in the game's language (IResourceManager::GetItemById's Name[0]), null for none. The item DATs are
// read the first time (about 0.4 s). Any thread.
const char* resource_item_name(uint32_t id);
// An item's English description as its DAT has it (through Pivot: CatsEyeXI's own), raw bytes (cp932, element icons),
// null for none. As resource_item_name. Any thread.
const char* resource_item_description(uint32_t id);
// IResourceManager's GetItemById and GetItemByName (language 0 the default) for addons (compat/ashita.lua): an IItem
// without its Bitmap (ImageSize is there), or the whole one a plugin had made; null for none. resource_item_icon is an
// item's Bitmap, ImageSize bytes ('' for none), read from its DAT. resource_item_is_stub: whether an address one of those
// gave is VanaCore's own item, cut short before its Bitmap (a plugin's, cexidats' for CatsEyeXI's own ids, is whole,
// with its own Bitmap and ImageSize). Any thread.
const void* resource_addon_item(uint32_t id);
const void* resource_addon_item_by_name(const char* name, uint32_t language);
std::string resource_item_icon(uint32_t id);
bool resource_item_is_stub(uintptr_t item);

// The game's data where the Ashita objects read it (memory.cpp), for vana.game: null (or false) until it's there.
const uint8_t* ability_bits();     // packet 0xAC's 224 bytes: weapon skills, job abilities, pet commands, traits
const uint8_t* spell_bits();       // 0x80 bytes, by spell id
const uint8_t* key_item_bits();    // 0x200 bytes, by key item id
const uint8_t* ability_recasts();  // 31 timer slots (8 bytes: the timer id at +3), then their timers (uint32_t) at +0xF8
const uint16_t* spell_recasts(uint32_t* count = nullptr); // by spell id; `count` of them (0x401, or cexislots's 0x1001)
struct ItemSlot {
    uint16_t id, index;
    uint32_t count, flags, price;
    uint8_t extra[28];
};
uint32_t container_capacity(uint32_t container); // the slots it has (inventory's 0 is gil)
bool container_item(uint32_t container, uint32_t index, ItemSlot& out);
bool equipped(uint32_t slot, uint32_t& container, uint32_t& index); // by the game's slot ids
// "/paste" and Ctrl+V: `text` (Shift-JIS) into the chat line at its caret. False when the line isn't open.
bool input_insert(const std::string& text);
// IPacketManager::QueueOutgoingPacket for addons (vana.packets.queue): a packet in the game's own outgoing queue, which
// `fill` fills. False when the game holds that id back.
bool queue_packet(uint16_t id, uint16_t length, uint16_t align, uint32_t p1, uint32_t p2,
                  void (*fill)(uint8_t* packet, void* param), void* param);
// While VanaCore runs Ashita addon `name`'s Lua ("" after): the fonts and primitives it makes are its
// own, freed when it unloads (release_addon) whether it deleted them or not. Returns the one before.
std::string set_calling_addon(const std::string& name);
void release_addon(const std::string& name);
// The plugin (its display name) or Ashita addon ("addon <name>") VanaCore is calling into on the game's thread; empty if none.
std::string_view calling();

// Loads the plugin DLL at `path`, named `display` (as its file spells it), passing it `args`.
// False with the reason in `error`.
bool load(const std::string& display, const std::string& path, const std::string& args, std::string& error);
// Unloads plugin `name` (any case). False if it isn't loaded.
bool unload(const std::string& name);
// The game is closing: every plugin unloaded, the last first (left as they are while one is being called).
void shutdown();
bool is_loaded(const std::string& name);

struct Info {
    std::string display;
    std::string author;
    double version = 0;
    double interface_version = 0;
    bool crashed = false;
};
// The plugin's details, false if it isn't loaded.
bool info(const std::string& name, Info& out);
// The loaded plugins, in the order they're called.
std::vector<Info> list();

// A command line about to run (typed, from a macro or menu, or injected): `which` plugins' command handlers, by their
// priority (plugins.hpp). True when one handled it, `by` its name.
bool on_command(const std::string& line, int mode, bool injected, plugins::Commands which, std::string& by);
// Plugins with UseText: a line coming into the chat log (HandleIncomingText) or going to the game (HandleOutgoingText).
// True: blocked.
bool on_text(bool incoming, chat::Text& text);

// The game's Direct3D device is made; and each frame's Present.
void on_device(void* device);
void on_present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty);
// The game's device begins (after the real call) or ends (before it) a scene: plugins' Direct3DBeginScene and
// Direct3DEndScene, with whether the scene draws on the back buffer. Minimap builds its map texture in the first.
void on_scene(void* device, bool begin);
// Draws plugins' fonts and primitives (between text::begin and text::end).
void draw(void* device);

// A mouse event (plugins::on_mouse): plugins' mouse callbacks get it as its window message (`type`
// is the message - WM_MOUSEFIRST, `wparam` as Windows gave it, x and y in the game's pixels); and
// Shift + left drag moves plugins' fonts that can be focused and aren't locked, as Ashita does. True
// when a callback blocked it, or while a font is being dragged (the game doesn't see it).
bool on_mouse(int type, int x, int y, WPARAM wparam);

// The game's DirectInput keyboard reads, as DirectInput filled them (input.cpp): plugins' keyboard
// callbacks may change them (Thirdparty holds down the keys other programs send). `capacity` is how
// many events `data` has room for.
void on_keyboard_state(void* device, DWORD size, void* data);
void on_keyboard_data(void* device, DWORD object_size, void* data, DWORD* count, DWORD capacity, DWORD flags);
// A key's window message (WM_KEYDOWN, WM_KEYUP and their WM_SYS forms): the virtual key and the
// message's flags, as a keyboard hook has them. True when a plugin's callback blocked it.
bool on_key(WPARAM key, LPARAM flags);
// Controllers' callbacks (IController::AddCallback, IXInput::AddCallback): plugins::on_controller_* and on_xinput_*
bool wants_gamepad();
void on_controller_state(void* device, DWORD size, void* data);
void on_controller_data(void* device, DWORD object_size, void* data, DWORD* count);
bool on_controller_button(uint32_t& offset, int32_t& value, bool injected);
void on_xinput_state(DWORD user, void* state);
bool on_xinput_button(uint32_t& button, int32_t& value, bool injected);
// A plugin event raised from outside the plugins (an Ashita addon's RaiseEvent): as plugin_raise
void raise_event(const std::string& name, const std::string& data);

// True when a loaded plugin handles packets.
bool wants_packets();

// Each packet in a buffer the game received (incoming) or is about to send (outgoing), with the
// whole buffer's decrypted contents. `modified` is the packet as it will be used (plugins may
// change it). True when a plugin blocked it. `skip`: the plugins (by their display names) that don't see it.
bool on_packet(bool incoming, uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t chunk_size,
               const uint8_t* chunk, bool injected, bool blocked, const std::vector<std::string>& skip = {});

} // namespace vanacore::ashita
