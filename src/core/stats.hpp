#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

// /vana stats: where a frame's time goes, as a tree like Unreal Engine's GPU Visualizer, in a window of its own
// (ImGui). Per frame, averaged over half a second:
//
//   the game's own work         (what's left of the frame once the rest is taken out)
//   its Direct3D calls          sorted into draw calls, textures, vertex and index buffers, lighting, render state,
//                               shaders and render targets, each down to the calls themselves
//   its sleeps
//   VanaCore                    its settings and chat, ImGui, plugins (each, with its memory), each Lua addon, its
//                               graphics, its interface
//   the frame rate limit, Present
//   the GPU (DirectX 12)        the game's scene, the sun's shadow maps, ambient occlusion, bloom, god rays, the composite,
//                               depth of field, FXAA, DLSS and the interface, from timestamps the GPU writes between them
//   counters                    draws, uploads, pipelines made
//   the render thread, sampled  (Record Stats Report) by module and, in FFXiMain.dll, by function: sampler.hpp
//
// Nothing is measured while the window is closed (a Scope costs one test). Record Stats Report is a recording: from
// its press to the press that stops it (or the window's close), each look is added up as well, and the stop writes
// all of it to logs\stats-<date>-<time>.txt whether or not logging is on, with the frames over twice their average
// sampled on their own and a line a look of how it went; the PC, the timeline, the lag spikes, the shadows and the
// settings are sections of the same file (one file for a player to send). /vana stats save
// writes what the window shows at any time (the whole recording, while one runs or after it, until the next).
//
//   /vana stats            opens it, or closes it
//   /vana stats on|off
//   /vana stats save
namespace vanacore::stats {

// From VanaCoreInstall: VanaCore's folder (the report goes in its logs folder; the sampler's tables in its addons).
void install(const wchar_t* install_dir);

// What of VanaCore's own frame is timed: each a part of its Present hook.
enum Section {
    kSettings,      // vanacore.ini's changes, the FPS counter, the window's title
    kChatPackets,   // the chat log and the packets' events
    kScreenshots,
    kGameConfig,
    kImGui,         // Dear ImGui's frame and its draw lists, /vana graph and /vana stats
    kPlugins,       // Ashita plugins' drawing and Present events (each plugin's calls in it: add_plugin_event)
    kAddons,        // Lua addons (add_addon: each by name)
    kGraphicsFront, // the graphics layer's per frame work
    kInterface,     // VanaCore's own drawing: its label, the FPS counter, ImGui
    kSectionCount
};

extern bool g_collecting; // the window is open: timing is on
inline bool collecting() { return g_collecting; }

void add(Section section, double ms);
void add_addon(std::string_view name, double ms);
// A call into an addon's Lua for `event` ("frame", "mouse", "text_in", ...): its time, for the addon's own window.
void add_addon_event(const std::string& addon, std::string_view event, double ms);

// A call into a native plugin (`event`: what for, as the plugin hosts name it: "Direct3DPresent", "HandleIncomingPacket"),
// for its window: its time less that of the calls into other plugins it made meanwhile (one raising an event the others
// handle). The calls made in the kPlugins part are its line in the tree as well.
void add_plugin_event(const std::string& plugin, std::string_view event, double ms);

// Times what's in its scope as `section`, if anything is being measured.
class Scope {
public:
    explicit Scope(Section section) : section_(section), on_(collecting())
    {
        if (on_)
            begin();
    }
    ~Scope()
    {
        if (on_)
            finish();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    void begin();
    void finish();
    Section section_;
    bool on_;
    LARGE_INTEGER start_{};
};

// Times a Lua addon's frame: add_addon with its name, if anything is being measured.
class AddonScope {
public:
    explicit AddonScope(const std::string& name) : name_(name), on_(collecting())
    {
        if (on_)
            QueryPerformanceCounter(&start_);
    }
    ~AddonScope();
    AddonScope(const AddonScope&) = delete;
    AddonScope& operator=(const AddonScope&) = delete;

private:
    const std::string& name_;
    bool on_;
    LARGE_INTEGER start_{};
};

// Times a call into a native plugin (compat/ashita and compat/windower's call_plugin) as add_plugin_event, if anything is
// being measured and it's on `game_thread` (the window's numbers are the game thread's).
class PluginScope {
public:
    PluginScope(const std::string& name, const char* event, bool game_thread);
    ~PluginScope();
    PluginScope(const PluginScope&) = delete;
    PluginScope& operator=(const PluginScope&) = delete;

private:
    const std::string& name_;
    const char* event_;
    bool on_;
    double outer_ = 0.0; // the time of the calls nested in the one around this, put back after it
    LARGE_INTEGER start_{};
};

// Each frame, from the Present hook, on the render thread: counts it, and every half second works out the tree.
void on_frame();

// Each frame, between gui::begin_frame and gui::end_frame: the window, while it's open.
void draw();

// "stats ..." (its words after /vana): true, it's handled.
bool command(const std::vector<std::string>& parts);

} // namespace vanacore::stats
