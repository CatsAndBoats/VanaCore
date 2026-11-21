#include "host.hpp"

#include <windows.h>

#include <vanacore/log_file.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "../../chat.hpp"
#include "../../crash.hpp"
#include "../../d3d8.hpp"
#include "../../addons.hpp"
#include "../../log.hpp"
#include "../../plugin_fault.hpp"
#include "../../plugin_memory.hpp"
#include "../../pluginapi.hpp"
#include "../../stats.hpp"
#include "../plugin_move.hpp"
#include "backend.hpp"

namespace vanacore::ashita {
namespace {

// Plugins are called through 4.30's declaration: IPluginBase's slots are the same in 4.16 and
// 4.30 (tools/gen_ashita_abi.py --test checks both), and its arguments differ only in which
// version's IAshitaCore pointer they carry.
using PluginObject = sdk::v430::IPlugin;
using PluginFlags = sdk::Ashita::PluginFlags;

struct Plugin {
    std::string key;     // lower case
    std::string display; // as its file spells it
    std::string path;
    HMODULE module = nullptr;
    PluginObject* object = nullptr;
    sdk::v430::export_DestroyPlugin_f destroy = nullptr;
    Links* links = nullptr;
    double interface_version = 0;
    std::string name;
    std::string author;
    double version = 0;
    uint32_t flags = 0;
    int32_t priority = 0;
    bool crashed = false;   // faulted: never called again, and its DLL stays loaded
    bool unloading = false; // asked to unload while plugins were being called
};

std::vector<std::unique_ptr<Plugin>> g_plugins; // call order: priority, then load order
Links g_v416;
Links g_v430;
std::string g_install;
std::wstring g_ini;
void* g_device = nullptr;
DWORD g_thread = 0;          // the game's thread (the first to call in)
std::string g_calling;       // the plugin being called on the game thread
std::string g_calling_addon; // ... or else the Ashita addon ("addon <name>"): what they make is theirs
int g_depth = 0;             // calls into plugins in progress
bool g_closing = false;      // the game is closing (shutdown): plugins' DLLs stay loaded (one may have hooked the window)
bool g_built = false;

std::mutex g_unimplemented_lock;
std::set<std::string> g_unimplemented;

// Plugins' input callbacks, by alias in the order they were added, each with the plugin that added
// it: called as that plugin (guarded), and gone when it unloads, whether it took them off or not.
struct InputCallback {
    std::string alias;
    std::string owner; // the plugin's display name
    InputDevice device = InputDevice::Keyboard;
    sdk::getdevicedatacallback_f device_data = nullptr;   // the keyboard's or a controller's
    sdk::getdevicestatecallback_f device_state = nullptr; // ...
    sdk::keyboardcallback_f keyboard = nullptr;
    sdk::mousecallback_f on_mouse = nullptr;
    sdk::controllercallback_f controller = nullptr;
    sdk::xinputgetstatecallback_f xinput_state = nullptr;
    sdk::xinputcallback_f xinput = nullptr;
};
std::vector<InputCallback> g_input;

bool uses(const Plugin& plugin, PluginFlags flag)
{
    return (plugin.flags & static_cast<uint32_t>(flag)) != 0;
}

// a fault in a plugin's code (plugin_fault.hpp)
using plugin_fault::describe;
using plugin_fault::Fault;
using plugin_fault::guarded;

void say_error(const std::string& text)
{
    chat::write(chat::kDefaultMode, chat::header() + chat::colour(chat::kError, text), "VanaCore");
}

// Calls into `plugin` (as the plugin being called), guarded: a fault disables it.
template <typename Call>
bool call_plugin(Plugin& plugin, const char* what, Call call)
{
    if (plugin.crashed)
        return false;
    const std::string outer = g_calling;
    g_calling = plugin.display;
    ++g_depth;
    Fault fault;
    bool ok = false;
    {
        const stats::PluginScope timing(plugin.display, what, g_thread != 0 && GetCurrentThreadId() == g_thread);
        ok = guarded(call, fault);
    }
    --g_depth;
    g_calling = outer;
    if (!ok && fault.code == crash::kCppException) {
        // Thrown and not caught: its frames were unwound, as a catch in Ashita would; it carries on.
        const std::string reason = fault.message[0] != '\0' ? fault.message : "a C++ exception";
        log::write(L"Ashita plugins: %hs failed in %hs: %hs (it carries on)", plugin.display.c_str(), what, reason.c_str());
        say_error("(Ashita Plugin) " + plugin.display + " failed in " + what + ": " + reason);
        return false;
    }
    if (!ok) {
        plugin.crashed = true;
        pluginapi::release(plugin.display); // what it asked of VanaCore's plugin API undone, as an addon's that crashes
        const std::string details = describe(fault);
        log::write(L"Ashita plugins: %hs crashed in %hs: %hs; it's disabled (its DLL stays loaded)", plugin.display.c_str(),
                   what, details.c_str());
        say_error("(Ashita Plugin) " + plugin.display + " crashed (" + details + "): disabled until the game restarts");
    }
    return ok;
}

Plugin* find(std::string_view name)
{
    const std::string key = lower(name);
    for (auto& plugin : g_plugins)
        if (plugin->key == key && !plugin->unloading)
            return plugin.get();
    return nullptr;
}

// The plugins to call now (a copy: a call can load or unload plugins).
std::vector<Plugin*> snapshot()
{
    std::vector<Plugin*> out;
    out.reserve(g_plugins.size());
    for (auto& plugin : g_plugins)
        if (!plugin->crashed && !plugin->unloading)
            out.push_back(plugin.get());
    return out;
}

void remove(Plugin& plugin)
{
    std::erase_if(g_input, [&](const InputCallback& callback) { return callback.owner == plugin.display; });
    if (!plugin.crashed) {
        call_plugin(plugin, "Release", [&] { plugin.object->Release(); });
        release_objects(plugin.display);
        call_plugin(plugin, "its destructor", [&] {
            if (plugin.destroy != nullptr)
                plugin.destroy(plugin.object);
            else
                delete plugin.object;
        });
    }
    if (!plugin.crashed && !g_closing) {
        Fault fault;
        auto free = [&] { FreeLibrary(plugin.module); };
        if (!guarded(free, fault))
            log::write(L"Ashita plugins: %hs crashed while its DLL unloaded: %hs", plugin.display.c_str(),
                       describe(fault).c_str());
        plugin_memory::unloaded(plugin.module);
    } else if (!plugin.crashed) {
        // the game is closing: its DLL stays loaded
    } else {
        release_objects(plugin.display);
        log::write(L"Ashita plugins: %hs crashed earlier: its DLL stays loaded", plugin.display.c_str());
    }
    pluginapi::release(plugin.display); // (after its Release and destructor, which may still ask)
    for (auto it = g_plugins.begin(); it != g_plugins.end(); ++it) {
        if (it->get() == &plugin) {
            g_plugins.erase(it);
            break;
        }
    }
}

// Unloads plugins that asked to go while plugins were being called.
void process_unloads()
{
    if (g_depth != 0)
        return;
    for (size_t i = 0; i < g_plugins.size();) {
        if (g_plugins[i]->unloading)
            remove(*g_plugins[i]);
        else
            ++i;
    }
}

void build()
{
    if (g_built)
        return;
    g_built = true;
    g_v416.version = 416;
    g_v416.interface_version = 4.16;
    g_v430.version = 430;
    g_v430.interface_version = 4.30;
    build_core(g_v416, g_v430);
    build_memory(g_v416, g_v430);
    build_resources(g_v416, g_v430);
    build_fonts(g_v416, g_v430);
    build_gui(g_v416, g_v430);
}

Links* links_for(double interface_version)
{
    if (std::fabs(interface_version - 4.16) < 0.0001)
        return &g_v416;
    if (std::fabs(interface_version - 4.30) < 0.0001)
        return &g_v430;
    return nullptr;
}

std::string text_of(const char* text)
{
    return text != nullptr ? std::string(text) : std::string();
}

} // namespace

namespace sdk {

void unimplemented(const char* method)
{
    const std::string caller(calling());
    {
        std::lock_guard lock(g_unimplemented_lock);
        if (!g_unimplemented.insert(method).second)
            return;
    }
    log::write(L"Ashita plugins: %hs called %hs, which VanaCore doesn't implement yet (it did nothing)",
               caller.empty() ? "a plugin" : caller.c_str(), method);
}

void* zeros()
{
    alignas(16) static unsigned char block[65536] = {};
    return block;
}

} // namespace sdk

const std::string& install_path()
{
    return g_install;
}

const std::wstring& settings_ini()
{
    return g_ini;
}

void* device()
{
    return g_device;
}

std::string_view calling()
{
    if (g_thread != 0 && GetCurrentThreadId() != g_thread)
        return {};
    return g_calling.empty() ? std::string_view(g_calling_addon) : std::string_view(g_calling);
}

void* addon_object(const std::string& name, bool v430)
{
    build();
    if (name == "device")
        return g_device;
    // the objects interface 4.16's (or 4.30's) plugins get, by the names compat/ashita.lua asks for
    const Links& a = v430 ? g_v430 : g_v416;
    const std::pair<const char*, void* const*> objects[] = {
        {"core", &a.core},           {"properties", &a.properties}, {"chat", &a.chat},         {"config", &a.config},
        {"fonts", &a.fonts},         {"input", &a.input},           {"memory", &a.memory},     {"offsets", &a.offsets},
        {"packets", &a.packets},     {"plugins", &a.plugins},       {"pol_plugins", &a.pol_plugins},
        {"pointers", &a.pointers},   {"primitives", &a.primitives}, {"resources", &a.resources}, {"log", &a.log},
        {"autofollow", &a.autofollow}, {"castbar", &a.castbar},     {"entity", &a.entity},     {"inventory", &a.inventory},
        {"party", &a.party},         {"player", &a.player},         {"recast", &a.recast},     {"target", &a.target},
        {"controller", &a.controller}, {"keyboard", &a.keyboard},   {"mouse", &a.mouse},       {"xinput", &a.xinput},
    };
    for (const auto& [object_name, object] : objects)
        if (name == object_name)
            return *object;
    return nullptr;
}

std::string set_calling_addon(const std::string& name)
{
    std::string before = g_calling_addon.empty() ? std::string() : g_calling_addon.substr(6);
    g_calling_addon = name.empty() ? std::string() : "addon " + name;
    return before;
}

void release_addon(const std::string& name)
{
    release_objects("addon " + name);
}

bool same(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void install(const std::string& install_dir, const std::wstring& ini)
{
    g_install = install_dir;
    g_ini = ini;
    install_layout(install_dir);
}

bool load(const std::string& display, const std::string& path, const std::string& args, std::string& error)
{
    if (g_thread == 0)
        g_thread = GetCurrentThreadId();
    build();
    if (find(display) != nullptr) {
        error = display + " is already loaded";
        return false;
    }
    plugin_memory::loading(display, path); // what it allocates is counted from before its DllMain
    const HMODULE module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD code = GetLastError();
    plugin_memory::loaded(module, display);
    // its DLL let go of, uncounted
    const auto free_module = [&] {
        FreeLibrary(module);
        plugin_memory::unloaded(module);
    };
    if (module == nullptr) {
        char text[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, sizeof(text), nullptr);
        std::string reason(text);
        while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r' || reason.back() == '.'))
            reason.pop_back();
        error = "Windows couldn't load its DLL (" + reason + ", error " + std::to_string(code) + ")";
        return false;
    }
    const auto get_version = reinterpret_cast<sdk::v430::export_GetInterfaceVersion_f>(GetProcAddress(module, "expGetInterfaceVersion"));
    const auto create = reinterpret_cast<sdk::v430::export_CreatePlugin_f>(GetProcAddress(module, "expCreatePlugin"));
    if (get_version == nullptr || create == nullptr) {
        free_module();
        error = "it isn't an Ashita plugin (no expCreatePlugin or expGetInterfaceVersion)";
        return false;
    }

    auto plugin = std::make_unique<Plugin>();
    plugin->key = lower(display);
    plugin->display = display;
    plugin->path = path;
    plugin->module = module;
    plugin->destroy = reinterpret_cast<sdk::v430::export_DestroyPlugin_f>(GetProcAddress(module, "expDestroyPlugin"));
    Plugin& p = *plugin;
    if (!call_plugin(p, "expGetInterfaceVersion", [&] { p.interface_version = get_version(); })) {
        error = "it crashed reporting its interface version";
        return false; // its DLL stays loaded: it's broken
    }
    p.links = links_for(p.interface_version);
    if (p.links == nullptr) {
        free_module();
        char version[32];
        sprintf_s(version, "%.2f", p.interface_version);
        error = std::string("it's built for Ashita interface ") + version + "; VanaCore hosts 4.16 and 4.30";
        return false;
    }
    if (!call_plugin(p, "expCreatePlugin", [&] { p.object = create(args.c_str()); }) || p.object == nullptr) {
        if (!p.crashed)
            free_module();
        error = p.crashed ? "it crashed starting" : "it didn't start (expCreatePlugin returned nothing)";
        return false;
    }
    call_plugin(p, "its details", [&] {
        p.name = text_of(p.object->GetName());
        p.author = text_of(p.object->GetAuthor());
        p.version = p.object->GetVersion();
        p.flags = p.object->GetFlags();
        p.priority = p.object->GetPriority();
    });

    // In call order: lower priorities first, the same priority in load order.
    auto at = g_plugins.begin();
    while (at != g_plugins.end() && (*at)->priority <= p.priority)
        ++at;
    g_plugins.insert(at, std::move(plugin));

    bool started = false;
    call_plugin(p, "Initialize", [&] {
        started = p.object->Initialize(reinterpret_cast<sdk::v430::IAshitaCore*>(p.links->core),
                                       reinterpret_cast<sdk::v430::ILogManager*>(p.links->log),
                                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module)));
    });
    if (!started || p.crashed) {
        error = p.crashed ? "it crashed starting" : "its Initialize failed";
        if (!p.crashed)
            p.unloading = true;
        process_unloads();
        return false;
    }
    if (g_device != nullptr && uses(p, PluginFlags::UseDirect3D))
        call_plugin(p, "Direct3DInitialize",
                    [&] { p.object->Direct3DInitialize(static_cast<sdk::IDirect3DDevice8*>(g_device)); });
    log::write(L"Ashita plugins: loaded %hs (%hs %.2f by %hs, interface %.2f, flags 0x%X, priority %d) from %hs",
               p.display.c_str(), p.name.c_str(), p.version, p.author.c_str(), p.interface_version, p.flags, p.priority,
               p.path.c_str());
    return true;
}

bool unload(const std::string& name)
{
    Plugin* plugin = find(name);
    if (plugin == nullptr)
        return false;
    plugin->unloading = true;
    process_unloads();
    log::write(L"Ashita plugins: unloaded %hs", name.c_str());
    return true;
}

void shutdown()
{
    if (g_depth != 0) {
        log::write(L"Ashita plugins: the game is closing while a plugin is running: left as they are");
        return;
    }
    g_closing = true;
    for (size_t i = g_plugins.size(); i-- > 0;) {
        if (i >= g_plugins.size())
            continue;
        const std::string display = g_plugins[i]->display;
        remove(*g_plugins[i]);
        log::write(L"Ashita plugins: unloaded %hs", display.c_str());
    }
}

bool is_loaded(const std::string& name)
{
    return find(name) != nullptr;
}

bool info(const std::string& name, Info& out)
{
    const Plugin* plugin = find(name);
    if (plugin == nullptr)
        return false;
    out = {plugin->display, plugin->author, plugin->version, plugin->interface_version, plugin->crashed};
    return true;
}

std::vector<Info> list()
{
    std::vector<Info> out;
    for (const auto& plugin : g_plugins)
        if (!plugin->unloading)
            out.push_back({plugin->display, plugin->author, plugin->version, plugin->interface_version, plugin->crashed});
    return out;
}

bool on_command(const std::string& line, int mode, bool injected, plugins::Commands which, std::string& by)
{
    using plugins::Commands;
    bool handled = false;
    for (Plugin* plugin : snapshot()) {
        if (!uses(*plugin, PluginFlags::UseCommands))
            continue;
        if (which == Commands::BeforeAddons ? plugin->priority >= 0 : which == Commands::AfterAddons && plugin->priority < 0)
            continue;
        call_plugin(*plugin, "HandleCommand", [&] { handled = plugin->object->HandleCommand(mode, line.c_str(), injected); });
        if (handled) {
            by = plugin->display;
            break;
        }
    }
    process_unloads();
    return handled;
}

void on_device(void* device)
{
    if (g_thread == 0)
        g_thread = GetCurrentThreadId();
    g_device = device;
    for (Plugin* plugin : snapshot())
        if (uses(*plugin, PluginFlags::UseDirect3D))
            call_plugin(*plugin, "Direct3DInitialize",
                        [&] { plugin->object->Direct3DInitialize(static_cast<sdk::IDirect3DDevice8*>(device)); });
    process_unloads();
}

void on_present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
{
    if (g_built)
        gui_frame();
    for (Plugin* plugin : snapshot())
        if (uses(*plugin, PluginFlags::UseDirect3D)) {
            plugin_move::drawing_as(plugin->display); // (what it draws moved as Addons > Layout has it)
            call_plugin(*plugin, "Direct3DPresent", [&] { plugin->object->Direct3DPresent(source, dest, window, dirty); });
        }
    plugin_move::drawing_as({});
    process_unloads();
}

void on_scene(void* device, bool begin)
{
    // The game's thread only, once it has a device (plugins' list and state aren't locked)
    if (g_thread == 0 || GetCurrentThreadId() != g_thread)
        return;
    std::vector<Plugin*> listening;
    for (Plugin* plugin : snapshot())
        if (uses(*plugin, PluginFlags::UseDirect3D))
            listening.push_back(plugin);
    if (listening.empty())
        return;
    // Ashita tells them whether the scene is on the back buffer (the game's own scene), not a shadow map's or the like
    void* target = nullptr;
    void* back_buffer = nullptr;
    bool on_back_buffer = false;
    if (SUCCEEDED(d3d8::call(device, d3d8::slot::GetRenderTarget, &target)) && target != nullptr) {
        if (SUCCEEDED(d3d8::call(device, d3d8::slot::GetBackBuffer, 0u, d3d8::kBackBufferMono, &back_buffer)) && back_buffer != nullptr) {
            on_back_buffer = target == back_buffer;
            d3d8::call<ULONG>(back_buffer, d3d8::slot::Release);
        }
        d3d8::call<ULONG>(target, d3d8::slot::Release);
    }
    for (Plugin* plugin : listening) {
        if (plugin->unloading)
            continue;
        plugin_move::drawing_as(plugin->display);
        if (begin)
            call_plugin(*plugin, "Direct3DBeginScene", [&] { plugin->object->Direct3DBeginScene(on_back_buffer); });
        else
            call_plugin(*plugin, "Direct3DEndScene", [&] { plugin->object->Direct3DEndScene(on_back_buffer); });
    }
    plugin_move::drawing_as({});
    process_unloads();
}

void draw(void* device)
{
    draw_objects(device);
}

namespace {

// Calls each keyboard (or mouse) callback as the plugin that added it. A copy: a callback can add or
// take off callbacks, or unload its plugin.
template <typename Call>
void each_input(InputDevice device, const char* what, Call call)
{
    // Plugins are called on the game's thread only (their list and state aren't locked), once the
    // game has a device (on_device knows its thread)
    if (g_thread == 0)
        return;
    if (GetCurrentThreadId() != g_thread) {
        static bool logged = false;
        if (!logged)
            log::write(L"Ashita plugins: %hs isn't called: the game read its input on another thread", what);
        logged = true;
        return;
    }
    const std::vector<InputCallback> callbacks = g_input;
    for (const InputCallback& callback : callbacks) {
        if (callback.device != device)
            continue;
        if (Plugin* plugin = find(callback.owner))
            call_plugin(*plugin, what, [&] { call(callback); });
    }
    process_unloads();
}

bool has_input(InputDevice device)
{
    return std::any_of(g_input.begin(), g_input.end(), [&](const InputCallback& callback) { return callback.device == device; });
}

} // namespace

void on_keyboard_state(void* device, DWORD size, void* data)
{
    if (!has_input(InputDevice::Keyboard))
        return;
    each_input(InputDevice::Keyboard, "its keyboard callback (GetDeviceState)", [&](const InputCallback& callback) {
        if (callback.device_state != nullptr)
            callback.device_state(device, size, data);
    });
}

void on_keyboard_data(void* device, DWORD object_size, void* data, DWORD* count, DWORD capacity, DWORD flags)
{
    if (!has_input(InputDevice::Keyboard))
        return;
    each_input(InputDevice::Keyboard, "its keyboard callback (GetDeviceData)", [&](const InputCallback& callback) {
        if (callback.device_data != nullptr)
            callback.device_data(device, object_size, data, count, capacity, flags);
    });
}

bool on_key(WPARAM key, LPARAM flags)
{
    bool blocked = false;
    if (!has_input(InputDevice::Keyboard))
        return false;
    each_input(InputDevice::Keyboard, "its keyboard callback", [&](const InputCallback& callback) {
        if (callback.keyboard != nullptr && callback.keyboard(key, flags, blocked))
            blocked = true;
    });
    return blocked;
}

bool on_mouse(int type, int x, int y, WPARAM wparam)
{
    bool blocked = false;
    // A left press (or double click) reaches the callbacks only with Shift held: plugins' windows
    // (Minimap) move with Shift + left drag, and a plain click stays the game's. Releases always come.
    const bool left_press = type == WM_LBUTTONDOWN - WM_MOUSEFIRST || type == WM_LBUTTONDBLCLK - WM_MOUSEFIRST;
    if (has_input(InputDevice::Mouse) && !(left_press && (GetKeyState(VK_SHIFT) & 0x8000) == 0)) {
        const UINT message = WM_MOUSEFIRST + static_cast<UINT>(type);
        each_input(InputDevice::Mouse, "its mouse callback", [&](const InputCallback& callback) {
            int px = x, py = y;
            plugin_move::unmoved(callback.owner, px, py); // (its drawing moved: the mouse where it put it)
            const LPARAM at = MAKELPARAM(static_cast<WORD>(static_cast<short>(px)), static_cast<WORD>(static_cast<short>(py)));
            if (callback.on_mouse != nullptr && callback.on_mouse(message, wparam, at, blocked))
                blocked = true;
        });
    }
    return drag_objects(type, x, y) || blocked;
}

bool wants_gamepad()
{
    return has_input(InputDevice::Controller) || has_input(InputDevice::XInput);
}

void on_controller_state(void* device, DWORD size, void* data)
{
    if (!has_input(InputDevice::Controller))
        return;
    each_input(InputDevice::Controller, "its controller callback (GetDeviceState)", [&](const InputCallback& callback) {
        if (callback.device_state != nullptr)
            callback.device_state(device, size, data);
    });
}

void on_controller_data(void* device, DWORD object_size, void* data, DWORD* count)
{
    if (!has_input(InputDevice::Controller) || count == nullptr)
        return;
    const DWORD capacity = *count;
    each_input(InputDevice::Controller, "its controller callback (GetDeviceData)", [&](const InputCallback& callback) {
        if (callback.device_data != nullptr)
            callback.device_data(device, object_size, data, count, capacity, 0);
    });
}

bool on_controller_button(uint32_t& offset, int32_t& value, bool injected)
{
    bool blocked = false;
    if (!has_input(InputDevice::Controller))
        return false;
    each_input(InputDevice::Controller, "its controller callback", [&](const InputCallback& callback) {
        if (callback.controller != nullptr && callback.controller(&offset, &value, blocked, injected))
            blocked = true;
    });
    return blocked;
}

void on_xinput_state(DWORD user, void* state)
{
    if (!has_input(InputDevice::XInput))
        return;
    each_input(InputDevice::XInput, "its XInput callback (XInputGetState)", [&](const InputCallback& callback) {
        if (callback.xinput_state != nullptr)
            callback.xinput_state(user, static_cast<XINPUT_STATE*>(state));
    });
}

bool on_xinput_button(uint32_t& button, int32_t& value, bool injected)
{
    bool blocked = false;
    if (!has_input(InputDevice::XInput))
        return false;
    uint8_t control = static_cast<uint8_t>(button);
    int16_t state = static_cast<int16_t>(value);
    each_input(InputDevice::XInput, "its XInput callback", [&](const InputCallback& callback) {
        if (callback.xinput != nullptr && callback.xinput(&control, &state, blocked, injected))
            blocked = true;
    });
    button = control;
    value = state;
    return blocked;
}

void raise_event(const std::string& name, const std::string& data)
{
    plugin_raise(name.c_str(), data.data(), static_cast<uint32_t>(data.size()));
}

bool wants_packets()
{
    for (const auto& plugin : g_plugins)
        if (!plugin->crashed && !plugin->unloading && uses(*plugin, PluginFlags::UsePackets))
            return true;
    return false;
}

bool on_text(bool incoming, chat::Text& text)
{
    // Ashita's buffer for a changed line (4096 bytes): empty until a plugin changes it, which those after honour
    char modified[4096] = {};
    if (text.modified != text.message)
        strncpy_s(modified, text.modified.c_str(), _TRUNCATE);
    int32_t mode = text.mode_modified;
    bool indent = text.indent_modified;
    bool blocked = false;
    for (Plugin* plugin : snapshot()) {
        if (!uses(*plugin, PluginFlags::UseText))
            continue;
        call_plugin(*plugin, incoming ? "HandleIncomingText" : "HandleOutgoingText", [&] {
            const bool block = incoming ? plugin->object->HandleIncomingText(text.mode, text.indent, text.message.c_str(), &mode,
                                                                             &indent, modified, text.injected, blocked)
                                        : plugin->object->HandleOutgoingText(text.mode, text.message.c_str(), &mode, modified,
                                                                             text.injected, blocked);
            blocked = blocked || block;
        });
    }
    process_unloads();
    modified[sizeof modified - 1] = '\0';
    if (modified[0] != '\0')
        text.modified = modified;
    text.mode_modified = mode;
    text.indent_modified = indent;
    return blocked;
}

bool on_packet(bool incoming, uint16_t id, uint32_t size, const uint8_t* data, uint8_t* modified, uint32_t chunk_size,
               const uint8_t* chunk, bool injected, bool blocked, const std::vector<std::string>& skip)
{
    for (Plugin* plugin : snapshot()) {
        if (!uses(*plugin, PluginFlags::UsePackets) || std::find(skip.begin(), skip.end(), plugin->display) != skip.end())
            continue;
        call_plugin(*plugin, incoming ? "HandleIncomingPacket" : "HandleOutgoingPacket", [&] {
            const bool block = incoming ? plugin->object->HandleIncomingPacket(id, size, data, modified, chunk_size, chunk, injected, blocked)
                                        : plugin->object->HandleOutgoingPacket(id, size, data, modified, chunk_size, chunk, injected, blocked);
            blocked = blocked || block;
        });
    }
    process_unloads();
    return blocked;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Input callbacks, as IKeyboard and IMouse take them.

void input_add_keyboard(const char* alias, sdk::getdevicedatacallback_f data, sdk::getdevicestatecallback_f state,
                        sdk::keyboardcallback_f keyboard)
{
    if (alias == nullptr)
        return;
    input_remove(alias, InputDevice::Keyboard); // a callback added again under its alias replaces it, as in Ashita
    InputCallback callback;
    callback.alias = alias;
    callback.owner = std::string(calling());
    callback.device_data = data;
    callback.device_state = state;
    callback.keyboard = keyboard;
    if (callback.owner.empty()) {
        log::write(L"Ashita plugins: a keyboard callback (%hs) was added from a plugin's own thread; it isn't called", alias);
        return;
    }
    log::write(L"Ashita plugins: %hs added keyboard callbacks (%hs)%hs%hs%hs", callback.owner.c_str(), alias,
               data != nullptr ? " for DirectInput's buffered reads" : "", state != nullptr ? " for DirectInput's state reads" : "",
               keyboard != nullptr ? " for key messages" : "");
    g_input.push_back(std::move(callback));
}

void input_add_mouse(const char* alias, sdk::mousecallback_f mouse)
{
    if (alias == nullptr)
        return;
    input_remove(alias, InputDevice::Mouse);
    InputCallback callback;
    callback.alias = alias;
    callback.owner = std::string(calling());
    callback.device = InputDevice::Mouse;
    callback.on_mouse = mouse;
    if (callback.owner.empty()) {
        log::write(L"Ashita plugins: a mouse callback (%hs) was added from a plugin's own thread; it isn't called", alias);
        return;
    }
    log::write(L"Ashita plugins: %hs added a mouse callback (%hs)", callback.owner.c_str(), alias);
    g_input.push_back(std::move(callback));
}

void input_add_controller(const char* alias, sdk::getdevicedatacallback_f data, sdk::getdevicestatecallback_f state,
                          sdk::controllercallback_f controller)
{
    if (alias == nullptr)
        return;
    input_remove(alias, InputDevice::Controller);
    InputCallback callback;
    callback.alias = alias;
    callback.owner = std::string(calling());
    callback.device = InputDevice::Controller;
    callback.device_data = data;
    callback.device_state = state;
    callback.controller = controller;
    if (callback.owner.empty()) {
        log::write(L"Ashita plugins: a controller callback (%hs) was added from a plugin's own thread; it isn't called", alias);
        return;
    }
    log::write(L"Ashita plugins: %hs added controller callbacks (%hs)", callback.owner.c_str(), alias);
    g_input.push_back(std::move(callback));
}

void input_add_xinput(const char* alias, sdk::xinputgetstatecallback_f state, sdk::xinputcallback_f xinput)
{
    if (alias == nullptr)
        return;
    input_remove(alias, InputDevice::XInput);
    InputCallback callback;
    callback.alias = alias;
    callback.owner = std::string(calling());
    callback.device = InputDevice::XInput;
    callback.xinput_state = state;
    callback.xinput = xinput;
    if (callback.owner.empty()) {
        log::write(L"Ashita plugins: an XInput callback (%hs) was added from a plugin's own thread; it isn't called", alias);
        return;
    }
    log::write(L"Ashita plugins: %hs added XInput callbacks (%hs)", callback.owner.c_str(), alias);
    g_input.push_back(std::move(callback));
}

void input_remove(const char* alias, InputDevice device)
{
    if (alias == nullptr)
        return;
    std::erase_if(g_input, [&](const InputCallback& callback) { return callback.device == device && callback.alias == alias; });
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// The plugin manager, as plugins see it.

bool plugin_unload(const char* name)
{
    return name != nullptr && unload(name);
}

void plugin_unload_all()
{
    for (auto& plugin : g_plugins)
        plugin->unloading = true;
    process_unloads();
}

bool plugin_loaded(const char* name)
{
    return name != nullptr && find(name) != nullptr;
}

void* plugin_get(const char* name)
{
    Plugin* plugin = name != nullptr ? find(name) : nullptr;
    return plugin != nullptr ? plugin->object : nullptr;
}

void* plugin_get(uint32_t index)
{
    return index < g_plugins.size() ? g_plugins[index]->object : nullptr;
}

uint32_t plugin_count()
{
    return static_cast<uint32_t>(g_plugins.size());
}

void plugin_raise(const char* event, const void* data, uint32_t size)
{
    addons::queue_plugin_event(event != nullptr ? event : "",
                               data != nullptr ? std::string(static_cast<const char*>(data), size) : std::string());
    for (Plugin* plugin : snapshot())
        if (uses(*plugin, PluginFlags::UsePluginEvents))
            call_plugin(*plugin, "HandleEvent", [&] { plugin->object->HandleEvent(event, data, size); });
    process_unloads();
}

} // namespace vanacore::ashita
