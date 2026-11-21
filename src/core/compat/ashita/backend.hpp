#pragma once

// What VanaCore's Ashita objects share: the per-version object table, the plugin being called,
// and helpers. Each backend file (core.cpp, memory.cpp, ...) defines its interfaces' behaviour as
// Impl structs of static functions (see abi.hpp: a method calls Impl::<Method>(context, args...)
// when Impl has it) and builds its objects for both interface versions.

#include <windows.h>

#include <string>
#include <string_view>

#include "abi.hpp"

namespace vanacore::ashita {

namespace FFXI = sdk::Ashita::FFXI;

// One interface version's objects, as plugins of that version see them. A singleton's context
// points to its version's Links; a font's or primitive's to its own state (which points here).
struct Links {
    int version = 0;              // 416 or 430
    double interface_version = 0; // 4.16 or 4.30

    void* core = nullptr;
    void* properties = nullptr;
    void* chat = nullptr;
    void* config = nullptr;
    void* fonts = nullptr;
    void* gui = nullptr;
    void* input = nullptr;
    void* memory = nullptr;
    void* offsets = nullptr;
    void* packets = nullptr;
    void* plugins = nullptr;
    void* pol_plugins = nullptr;
    void* pointers = nullptr;
    void* primitives = nullptr;
    void* resources = nullptr;
    void* log = nullptr;

    void* autofollow = nullptr;
    void* castbar = nullptr;
    void* entity = nullptr;
    void* inventory = nullptr;
    void* party = nullptr;
    void* player = nullptr;
    void* recast = nullptr;
    void* target = nullptr;

    void* controller = nullptr;
    void* keyboard = nullptr;
    void* mouse = nullptr;
    void* xinput = nullptr;

    // A new font or primitive object of this version, its context `state`; and freeing one.
    void* (*make_font)(void* state) = nullptr;
    void (*free_font)(void* object) = nullptr;
    void* (*make_primitive)(void* state) = nullptr;
    void (*free_primitive)(void* object) = nullptr;
};

inline Links& links(void* context)
{
    return *static_cast<Links*>(context);
}

// A new Object<Impl> (one of abi.hpp's templates) with `context`. Lives as long as the process
// unless its maker frees it.
template <template <class> class Object, class Impl>
void* make(void* context)
{
    auto* object = new Object<Impl>();
    object->context = context;
    return static_cast<void*>(object);
}

template <template <class> class Object, class Impl>
void destroy(void* object)
{
    delete static_cast<Object<Impl>*>(object);
}

// Each backend file's objects, for both versions.
void build_core(Links& v416, Links& v430);
void build_memory(Links& v416, Links& v430);
void build_resources(Links& v416, Links& v430);
void build_fonts(Links& v416, Links& v430);
void build_gui(Links& v416, Links& v430);
// Before plugins draw a frame: what interface 4.16 plugins wrote into their 1.81-shaped ImGuiIO and ImGuiStyle carried
// into VanaCore's, and those copies refreshed (gui.cpp).
void gui_frame();

// VanaCore's folder, with a trailing backslash (what GetInstallPath returns).
const std::string& install_path();
// Ashita's layout on VanaCore's (paths.cpp): <install>\addons\<x> is <install>\addons\ashita\<x> unless <x> is ashita,
// windower or vanacore. install_layout points VanaCore.Core.dll's file imports at that, once; layout_path is it for one
// path that reaches Windows some other way (WIC opens D3DX's files itself).
void install_layout(const std::string& install_dir);
std::wstring layout_path(const std::wstring& path);
// vanacore.ini.
const std::wstring& settings_ini();
// The game's Direct3D device, once it's made.
void* device();
// A texture (IDirect3DTexture8, managed, A8R8G8B8, one level) from an image file at `path`, or else from the image in
// `data` (d3dx.cpp, as D3DX reads them), `colour_key` turned transparent; its size. Null if it can't be read.
void* load_texture(void* device, const std::wstring& path, const void* data, UINT size, DWORD colour_key, UINT& width,
                   UINT& height);

// The plugin VanaCore is calling into on the game's thread, if any: what it creates is its own.
// Empty when a plugin calls from a thread of its own.
std::string_view calling();

// The plugins, as the plugin manager interface sees them (host.cpp).
bool plugin_load(const char* name, const char* args);
bool plugin_unload(const char* name);
void plugin_unload_all();
bool plugin_loaded(const char* name);
void* plugin_get(const char* name);
void* plugin_get(uint32_t index);
uint32_t plugin_count();
void plugin_raise(const char* event, const void* data, uint32_t size);
// A command line from a plugin (IChatManager::ParseCommand / QueueCommand).
void run_command(int32_t mode, const char* command, bool queue);

// Plugins' input callbacks (IKeyboard, IMouse, IController and IXInput's AddCallback), kept by alias with the plugin
// adding them, and taken off (host.cpp).
enum class InputDevice { Keyboard, Mouse, Controller, XInput };
void input_add_keyboard(const char* alias, sdk::getdevicedatacallback_f data, sdk::getdevicestatecallback_f state,
                        sdk::keyboardcallback_f keyboard);
void input_add_mouse(const char* alias, sdk::mousecallback_f mouse);
void input_add_controller(const char* alias, sdk::getdevicedatacallback_f data, sdk::getdevicestatecallback_f state,
                          sdk::controllercallback_f controller);
void input_add_xinput(const char* alias, sdk::xinputgetstatecallback_f state, sdk::xinputcallback_f xinput);
void input_remove(const char* alias, InputDevice device);

// Fonts and primitives a plugin made, freed when it unloads (fonts.cpp).
void release_objects(std::string_view owner);
// Draws the fonts and primitives: once per frame, between text::begin and text::end.
void draw_objects(void* device);
// A mouse event (Windower's types: 0 move, 1 left down, 2 left up; back buffer pixels): Shift + left
// drag moves a font that can be focused and isn't locked. True while one is being dragged.
bool drag_objects(int type, int x, int y);

// Case-insensitive ASCII comparison.
bool same(std::string_view a, std::string_view b);
std::string lower(std::string_view text);

} // namespace vanacore::ashita
