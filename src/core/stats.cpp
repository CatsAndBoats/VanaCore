#include "stats.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <share.h>
#include <shellapi.h>

#include <vanacore/version.hpp>

#include "../gfx/vanacore_gfx.h"
#include "chat.hpp"
#include "addons.hpp"
#include "frametime.hpp"
#include "graphics.hpp"
#include "gui.hpp"
#include "layout.hpp"
#include "hwmon.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "perf.hpp"
#include "plugin_memory.hpp"
#include "plugins.hpp"
#include "sampler.hpp"
#include "settings.hpp"
#include "vanaui.hpp"

namespace vanacore::stats {

using namespace vanaui;

bool g_collecting = false;

namespace {

constexpr double kRefresh = 0.5; // s between looks at what's been collected
constexpr double kDipMs = 1000.0 / 15.0; // a frame this long or longer is a lag spike (under 15 fps): it's snapshotted
constexpr double kLongRefresh = 5.0; // ... with "5s average" ticked
bool g_average = false;              // "5s average": one look every five seconds, at what the five seconds came to

double window_seconds()
{
    return g_average ? kLongRefresh : kRefresh;
}

const char* window_words()
{
    return g_average ? "five seconds" : "half a second";
}

constexpr const char* kSectionNames[kSectionCount] = {
    "Settings, FPS counter, window title", "Chat and packets",      "Screenshots",
    "Game config watch",                   "Dear ImGui (frames and draw lists)", "Plugins (their drawing and Present)",
    "Lua addons",                          "Graphics layer (per frame)", "Interface drawing (label, FPS counter, ImGui)",
};

// A line of the tree
struct Node {
    std::string name;
    double ms = 0.0;
    double calls = -1.0;  // a frame's, when it has them
    bool counter = false; // `ms` is a count, `unit` what of
    bool header = false;  // a heading: no numbers of its own
    bool addon = false;   // a Lua addon: a click opens its window
    bool plugin = false;  // a native plugin: likewise
    bool measure = false; // open, the GPU's composite is measured by feature (vanacore_gfx_measure)
    double kb = -1.0;     // its memory, the Memory column (an addon's Lua state, a plugin's allocations, the game's); -1 none
    std::string unit;
    std::string note;
    std::vector<Node> children;
};

// What's been collected since the last look
struct Acc {
    unsigned frames = 0;
    double frame_sum = 0.0;
    unsigned parts = 0; // frames Detailed timing saw
    double game = 0.0, direct3d = 0.0, sleeps = 0.0, vanacore = 0.0, limit = 0.0, present = 0.0;
    unsigned gfx = 0;
    double encode = 0.0, gpu_wait = 0.0, swap = 0.0, helper = 0.0, gpu = 0.0, draws = 0.0, upload = 0.0, skipped = 0.0;
    double pipelines = 0.0;
    double grass = 0.0, grass_tufts = 0.0; // making squares of grass tufts (of direct3d), and the tufts made
    double redrawn = 0.0;                  // the moving things drawn again for DLSS's or FSR's motion and masks
};

// A stretch of frames, collected: a look's (half a second's, or five), or a recording's (its looks added up)
struct Window {
    Acc acc;
    double seconds = 0.0;
    double sections[kSectionCount] = {};
    std::map<std::string, double> addons;
    std::map<std::string, double> plugins; // a plugin's calls in the kPlugins part
    std::map<std::string, std::pair<double, double>> methods; // a Direct3D method's ms and calls
    double gpu[VANA_GPU_SCOPES] = {}; // the GPU's ms a frame by what it was doing, times its frames
    double gpu_parts[VANA_GPU_SCOPES][VANA_GPU_PARTS] = {}; // ... each by part, likewise
    double gpu_frames = 0.0;
    double gpu_full = 0.0; // ... of them, those whose parts' timestamps ran out
    double features[VANA_GPU_FEATURES] = {}; // the composite's features' cost (while measured), likewise
    double feature_frames[VANA_GPU_FEATURES] = {};
    bool feature_off[VANA_GPU_FEATURES] = {}; // the last look's
    bool measuring = false;                   // ... and whether it was measuring them
    double nr = 0.0, sr = 0.0, fg = 0.0; // DLSS's jobs, likewise
    double dlss_frames = 0.0;
    double worst = 0.0;   // the slowest frame, ms
    unsigned slow = 0;    // frames that took over twice their look's average
    double slow_ms = 0.0; // ... their time
};

void add(Window& into, const Window& w)
{
    Acc& a = into.acc;
    const Acc& b = w.acc;
    a.frames += b.frames, a.frame_sum += b.frame_sum, a.parts += b.parts;
    a.game += b.game, a.direct3d += b.direct3d, a.sleeps += b.sleeps, a.vanacore += b.vanacore, a.limit += b.limit, a.present += b.present;
    a.gfx += b.gfx;
    a.encode += b.encode, a.gpu_wait += b.gpu_wait, a.swap += b.swap, a.helper += b.helper, a.gpu += b.gpu, a.draws += b.draws;
    a.upload += b.upload, a.skipped += b.skipped, a.pipelines += b.pipelines;
    a.grass += b.grass, a.grass_tufts += b.grass_tufts, a.redrawn += b.redrawn;
    into.seconds += w.seconds;
    for (int s = 0; s < kSectionCount; ++s)
        into.sections[s] += w.sections[s];
    for (const auto& [name, ms] : w.addons)
        into.addons[name] += ms;
    for (const auto& [name, ms] : w.plugins)
        into.plugins[name] += ms;
    for (const auto& [name, m] : w.methods) {
        auto& to = into.methods[name];
        to.first += m.first;
        to.second += m.second;
    }
    for (int i = 0; i < VANA_GPU_SCOPES; ++i) {
        into.gpu[i] += w.gpu[i];
        for (int p = 0; p < VANA_GPU_PARTS; ++p)
            into.gpu_parts[i][p] += w.gpu_parts[i][p];
    }
    into.gpu_frames += w.gpu_frames;
    into.gpu_full += w.gpu_full;
    for (int f = 0; f < VANA_GPU_FEATURES; ++f) {
        into.features[f] += w.features[f];
        into.feature_frames[f] += w.feature_frames[f];
        into.feature_off[f] = w.feature_off[f];
    }
    into.measuring = w.measuring;
    into.nr += w.nr, into.sr += w.sr, into.fg += w.fg, into.dlss_frames += w.dlss_frames;
    into.worst = std::max(into.worst, w.worst);
    into.slow += w.slow, into.slow_ms += w.slow_ms;
}

// The recording (Record Stats Report): from its press to the press that stops it, each look added up, with the sampler's
// samples of the same time, for the report the stop writes. It stays after it stops, until the next press starts another.
struct Item { // a line of the profiler's trees, as the recording saw it: a row of the table of each item over time
    std::string name;
    int parent = -1;
    bool counter = false; // its numbers are counts, not % of the frame
    float peak = 0.0f;
};
struct Point { // a look of it, for how it went over time
    float at = 0.0f; // s into the recording, at the look's end
    float fps = 0.0f, frame_ms = 0.0f, worst_ms = 0.0f, gpu_ms = 0.0f;
    unsigned slow = 0;
    bool cpu = false, gpu = false;
    unsigned cpu_mhz = 0, gpu_mhz = 0;
    int gpu_temp = 0;
    float game_pct = 0.0f, d3d_pct = 0.0f, sleep_pct = 0.0f, vana_pct = 0.0f, limit_pct = 0.0f; // each of the look's frame time, %
    unsigned dips = 0;   // frames under 15 fps
    std::string top;     // what took the most of it: the addons and VanaCore's parts, with their %
    std::string dip_in;  // where the thread was in the dip frames (when sampled): its hottest function, with its %
    std::vector<std::pair<int, float>> items; // every line of the profiler's trees: its item, and its % of the frame (a counter's count)
};
struct Recording {
    bool have = false;
    LARGE_INTEGER since{}, until{}; // until: 0 while it runs
    char started[16] = "";          // its start, the time of day
    Window total;
    std::vector<Point> timeline;
    std::vector<Item> items;
    std::map<std::pair<int, std::string>, int> item_at; // (parent, name) -> its index in items
    std::wstring dips_file; // the lag spike snapshots' file: one a recording, each dip added to it
    std::wstring shadows_file; // the shadows' file, every frame (stats_shadows.hpp): "" while none is being written
    std::wstring shadows_done; // ... once the recording has stopped it, till the report takes it in
    std::wstring folded_into;  // the report the stop wrote, which took those files in (and deleted them)
    unsigned dips = 0;      // snapshots in it
    float dip_at = -1000.0f; // the last one's time, s into the recording
};
Recording g_recording;

std::wstring g_logs;
LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_last_frame{}, g_window_start{};
Acc g_acc;
double g_sections[kSectionCount];
int g_running[kSectionCount] = {}; // Scopes going on now, by part (the game's thread)
std::map<std::string, double> g_addons;
std::map<std::string, double> g_plugins; // a plugin's calls in the kPlugins part
double g_plugin_nested = 0.0;           // the time of the plugin calls in the one going on now (PluginScope)
std::vector<sampler::Span> g_spans; // the look's frames

// ---- one addon's window ---------------------------------------------------------------------------------------------
struct EventRow {
    std::string name;
    double ms = 0.0;    // a frame's
    double calls = 0.0; // a frame's
};
std::map<std::string, std::map<std::string, std::pair<double, unsigned>>> g_events; // addon -> event -> ms, calls (this window)
std::map<std::string, std::vector<EventRow>> g_event_view;                           // ... as of the last look, per frame
std::map<std::string, double> g_addon_view;                                          // addon -> ms a frame, as of the last look
std::string g_detail;                  // the addon whose window is open
std::deque<float> g_memory;            // its Lua memory (KB), a point per look
std::string g_memory_of;               // ... whose

// ---- one plugin's window: the same, for a native plugin -------------------------------------------------------------
using Calls = std::map<std::string, std::pair<double, unsigned>, std::less<>>; // event -> ms, calls
std::map<std::string, Calls> g_plugin_events;                    // plugin -> its calls (this window)
std::map<std::string, std::vector<EventRow>> g_plugin_event_view; // ... as of the last look, per frame
std::map<std::string, double> g_plugin_view;                      // plugin -> all its calls' ms a frame, as of the last look
std::string g_plugin_detail;                                      // the plugin whose window is open
std::deque<float> g_plugin_memory;                                // its memory (KB), a point per look
std::string g_plugin_memory_of;                                   // ... whose
bool g_profiling = false;              // a sampling profile is running, for g_profile_of
std::string g_profile_of;
LARGE_INTEGER g_profile_since{};
struct Share {
    std::string what;
    double samples = 0.0;
};
struct Profile {
    std::string addon;
    std::string error;
    double total = 0.0;
    std::vector<Share> states, lines, functions;
    double seconds = 0.0; // how long it sampled
};
Profile g_profile; // the last one stopped

bool g_open = false;
bool g_hold = false; // Pause: the numbers stay as they are
bool g_sample = false; // Record Stats Report: the recording runs
bool g_saved_now = false; // the stop has just written the recording's report: the button reads Saved for a moment
bool save_report();
void shadows_start(); // (stats_shadows.hpp)
void shadows_stop();
std::string settings_text();
std::string scrub_secrets(std::string text);
char g_built_at[16] = "";       // the time the numbers on show were worked out
bool g_have = false;   // a tree has been made

// What's shown
Node g_cpu;       // the frame: its children are the parts
Node g_gpu;       // DirectX 12's GPU time by what it was doing
Node g_counters;
Node g_sampled;   // the render thread's samples, the recording's
Node g_slow;      // ... those in its slow frames
double g_frame_ms = 0.0, g_gpu_ms = 0.0, g_fps = 0.0;
bool g_gpu_seen = false;
bool g_sampled_seen = false;

// ---- collecting ---------------------------------------------------------------------------------------------------

void watch(const frametime::Parts& p)
{
    Acc& a = g_acc;
    ++a.parts;
    a.game += std::max(0.0, p.frame - p.direct3d - p.sleeps - p.vanacore - p.limit - p.present);
    a.direct3d += p.direct3d, a.sleeps += p.sleeps, a.vanacore += p.vanacore, a.limit += p.limit, a.present += p.present;
    VanaGfxFrame g{};
    if (vanacore_gfx_frame_stats(&g)) {
        ++a.gfx;
        a.encode += g.encode_ms, a.gpu_wait += g.gpu_wait_ms, a.swap += g.swap_ms, a.helper += g.helper_wait_ms, a.gpu += g.gpu_ms;
        a.draws += g.draws, a.upload += g.upload_kb, a.skipped += g.skipped, a.pipelines += g.pipelines;
        a.grass += g.grass_ms, a.grass_tufts += g.grass_tufts, a.redrawn += g.redrawn;
    }
}

// The recording ends: what it has stays, and its report is written (save_report: one .txt, the lag spikes, the shadows
// and the settings in it), whether the press that stops it ended it or the window's close
void stop_recording()
{
    if (!sampler::running())
        return;
    sampler::stop();
    shadows_stop();
    QueryPerformanceCounter(&g_recording.until);
    if (g_recording.total.acc.frames == 0) {
        chat::echo("stats: the recording ended before a look was taken; nothing saved");
        return;
    }
    if (save_report())
        g_saved_now = true;
}

// What's been collected for the look is dropped
void drop_window()
{
    g_acc = {};
    std::fill(std::begin(g_sections), std::end(g_sections), 0.0);
    g_addons.clear();
    g_events.clear();
    g_plugins.clear();
    g_plugin_events.clear();
    g_spans.clear();
}

void set_open(bool open)
{
    if (open == g_open)
        return;
    g_open = open;
    g_collecting = open;
    drop_window();
    g_last_frame = {};
    g_window_start = {};
    g_event_view.clear();
    g_addon_view.clear();
    g_plugin_event_view.clear();
    g_plugin_view.clear();
    if (open) {
        frametime::watch(&watch);
        frametime::profile(true);
        hwmon::acquire();
    } else {
        frametime::unwatch(&watch);
        frametime::profile(false);
        hwmon::release();
        stop_recording();
        g_sample = false;
        g_have = false;
        if (g_profiling) {
            std::string report, error;
            addons::profile_stop(g_profile_of, report, error);
            g_profiling = false;
        }
        g_detail.clear();
        g_plugin_detail.clear();
    }
}

// ---- sorting the Direct3D methods ---------------------------------------------------------------------------------

bool has(const char* name, const char* part)
{
    return std::strstr(name, part) != nullptr;
}

const char* category(const char* name)
{
    if (has(name, "DrawPrimitive") || has(name, "DrawIndexedPrimitive") || has(name, "Patch") || has(name, "ProcessVertices"))
        return "Draw calls";
    if (has(name, "Shader"))
        return "Shaders";
    if (has(name, "Light") || has(name, "Material"))
        return "Lighting and materials";
    if (has(name, "Buffer") || has(name, "StreamSource") || has(name, "SetIndices") || has(name, "GetIndices"))
        return "Vertex and index buffers";
    if (has(name, "Texture") || has(name, "CopyRects") || has(name, "ImageSurface") || has(name, "SurfaceLevel") ||
        has(name, "VolumeTexture") || has(name, "CubeTexture"))
        return "Textures";
    if (has(name, "RenderTarget") || has(name, "DepthStencil") || has(name, "BackBuffer") || std::strcmp(name, "Clear") == 0 ||
        has(name, "BeginScene") || has(name, "EndScene"))
        return "Render targets and scenes";
    if (has(name, "RenderState") || has(name, "TextureStageState") || has(name, "Transform") || has(name, "Viewport") ||
        has(name, "ClipPlane") || has(name, "StateBlock") || has(name, "ValidateDevice"))
        return "Render state and transforms";
    return "Other calls";
}

void sort_by_time(std::vector<Node>& nodes)
{
    std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return a.ms > b.ms; });
}

// ---- the tree ---------------------------------------------------------------------------------------------------------

Node leaf(std::string name, double ms, double calls = -1.0)
{
    Node n;
    n.name = std::move(name);
    n.ms = ms;
    n.calls = calls;
    return n;
}

Node counter(std::string name, double value, std::string unit)
{
    Node n;
    n.name = std::move(name);
    n.ms = value;
    n.counter = true;
    n.unit = std::move(unit);
    return n;
}

// The look's collecting as a Window; its frames that took over twice their average into `slow`
Window gather(double seconds, std::vector<sampler::Span>& slow, std::vector<sampler::Span>& dip)
{
    Window w;
    w.acc = g_acc;
    w.seconds = seconds;
    std::copy(std::begin(g_sections), std::end(g_sections), w.sections);
    w.addons = g_addons;
    w.plugins = g_plugins;
    frametime::Method methods[128];
    const int method_count = frametime::take_methods(methods, 128);
    for (int i = 0; i < method_count; ++i) {
        auto& m = w.methods[methods[i].name];
        m.first += methods[i].ms;
        m.second += methods[i].calls;
    }
    VanaGfxScopes scopes{};
    if (vanacore_gfx_scopes(&scopes) && scopes.frames > 0) {
        w.gpu_frames = scopes.frames;
        w.gpu_full = scopes.full_frames;
        for (int i = 0; i < VANA_GPU_SCOPES; ++i) {
            w.gpu[i] = static_cast<double>(scopes.ms[i]) * w.gpu_frames;
            for (int p = 0; p < VANA_GPU_PARTS; ++p)
                w.gpu_parts[i][p] = static_cast<double>(scopes.part_ms[i][p]) * w.gpu_frames;
        }
        w.measuring = scopes.measuring != 0;
        for (int f = 0; f < VANA_GPU_FEATURES; ++f) {
            w.feature_off[f] = scopes.feature_off[f] != 0;
            if (scopes.feature_ms[f] >= 0.0f) {
                w.features[f] = static_cast<double>(scopes.feature_ms[f]) * w.gpu_frames;
                w.feature_frames[f] = w.gpu_frames;
            }
        }
        VanaGfxDlssTimes times{};
        if (vanacore_gfx_dlss_times(&times)) {
            w.dlss_frames = w.gpu_frames;
            w.nr = times.nr_ms * w.dlss_frames, w.sr = times.sr_ms * w.dlss_frames, w.fg = times.fg_ms * w.dlss_frames;
        }
    }
    const double average = w.acc.frames > 0 ? w.acc.frame_sum / w.acc.frames : 0.0;
    for (const sampler::Span& span : g_spans) {
        const double ms = static_cast<double>(span.end - span.start) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
        w.worst = std::max(w.worst, ms);
        if (ms >= kDipMs)
            dip.push_back(span);
        if (ms > 2.0 * average) {
            ++w.slow;
            w.slow_ms += ms;
            slow.push_back(span);
        }
    }
    return w;
}

// What's shown of a Window: the frame's tree, the GPU's, the counters
struct View {
    Node cpu, gpu, counters;
    double frame_ms = 0.0, gpu_ms = 0.0, fps = 0.0;
    bool gpu_seen = false;
};

std::string memory_words(double kb);

// The plugins part's lines: every plugin loaded, with what its calls in the part took (`fk`: a frame of the look) and its
// memory beside it, and what the part did besides (drawing Ashita's font and primitive objects: plugins' and addons')
void plugin_lines(Node& part, const Window& w, double fk)
{
    double named = 0.0;
    for (const auto& [name, kind] : plugins::loaded_list()) {
        const auto found = w.plugins.find(name);
        Node plugin = leaf(name, found != w.plugins.end() ? found->second * fk : 0.0);
        plugin.plugin = true;
        plugin_memory::Usage use;
        if (plugin_memory::usage(name, use)) {
            plugin.kb = use.heap_kb + use.virtual_kb;
            part.kb = std::max(0.0, part.kb) + plugin.kb;
        }
        named += plugin.ms;
        part.children.push_back(std::move(plugin));
    }
    sort_by_time(part.children);
    if (!part.children.empty() && part.ms - named > 0.0005)
        part.children.push_back(leaf("Drawing their fonts and primitives (Ashita addons' too)", part.ms - named));
}

// VanaCore's part s, with each addon (kAddons) or plugin (kPlugins) under it; `fk`: a frame of the look
Node part_node(const Window& w, int s, double fk)
{
    Node part = leaf(kSectionNames[s], w.sections[s] * fk);
    if (s == kAddons) {
        for (const auto& [name, ms] : w.addons) {
            Node addon = leaf(name, ms * fk);
            addon.addon = true;
            addons::Info info;
            if (addons::info(name, info)) {
                addon.kb = info.memory_kb;
                part.kb = std::max(0.0, part.kb) + info.memory_kb;
            }
            part.children.push_back(std::move(addon));
        }
        sort_by_time(part.children);
    } else if (s == kPlugins) {
        plugin_lines(part, w, fk);
    }
    return part;
}

// `over`: the time the counts that aren't a frame's are over ("in this half second")
View build(const Window& w, const char* over)
{
    View v;
    const Acc& a = w.acc;
    const double frames = static_cast<double>(a.frames);
    const double fk = frames > 0 ? 1.0 / frames : 0.0;
    const double pk = a.parts > 0 ? 1.0 / a.parts : 0.0;
    v.frame_ms = a.frame_sum * fk;
    v.fps = w.seconds > 0 ? frames / w.seconds : 0.0;

    // ---- the CPU
    Node& cpu = v.cpu;
    cpu.name = "Frame (the render thread)";
    cpu.ms = v.frame_ms;
    if (const double mb = hwmon::latest().ram_game_mb; mb > 0.0)
        cpu.kb = mb * 1024.0; // the whole game's private memory (Task Manager's Commit size)

    // the Direct3D calls, by what they are about
    std::map<std::string, Node> categories;
    for (const auto& [name, m] : w.methods) {
        const char* what = category(name.c_str());
        Node& c = categories[what];
        c.name = what;
        c.ms += m.first * fk;
        c.calls = std::max(0.0, c.calls) + m.second * fk;
        c.children.push_back(leaf(name, m.first * fk, m.second * fk));
    }
    Node d3d;
    d3d.name = "Direct3D calls (the game's, into the driver)";
    d3d.ms = a.direct3d * pk;
    for (auto& [name, c] : categories) {
        sort_by_time(c.children);
        d3d.children.push_back(std::move(c));
    }
    sort_by_time(d3d.children);
    if (a.gfx > 0) {
        d3d.children.push_back(leaf("(DirectX 12) drawing the calls into command lists, inside the above", a.encode / a.gfx));
        d3d.children.push_back(leaf("(DirectX 12) waiting for the GPU to free a frame, inside the above", a.gpu_wait / a.gfx));
        d3d.children.push_back(leaf("Grass: squares of tufts made as you come up to them (grass.build), inside the above", a.grass / a.gfx));
    }

    // VanaCore's own work, by part, with each addon
    Node vanacore;
    vanacore.name = "VanaCore";
    vanacore.ms = a.vanacore * pk;
    double accounted = 0.0;
    for (int s = 0; s < kSectionCount; ++s) {
        Node part = part_node(w, s, fk);
        accounted += part.ms;
        if (part.ms > 0.0005 || (s == kPlugins && !part.children.empty())) // the plugins' while any is loaded, to click one
            vanacore.children.push_back(std::move(part));
    }
    sort_by_time(vanacore.children);
    if (vanacore.ms - accounted > 0.005)
        vanacore.children.push_back(leaf("Other (hooks, input, the rest of Present's work)", vanacore.ms - accounted));

    Node present = leaf("Present", a.present * pk);
    if (a.gfx > 0) {
        present.children.push_back(leaf("(DirectX 12) waiting for a back buffer (the swap chain)", a.swap / a.gfx));
        present.children.push_back(leaf("(DirectX 12) waiting on the DLSS / FSR helper", a.helper / a.gfx));
    }

    cpu.children.push_back(leaf("The game's own work (its logic, animation, physics, the rest)", a.game * pk));
    cpu.children.push_back(std::move(d3d));
    cpu.children.push_back(leaf("Sleeps (the game's own frame pacing)", a.sleeps * pk));
    cpu.children.push_back(std::move(vanacore));
    cpu.children.push_back(leaf("Frame rate limit (fps.max)", a.limit * pk));
    cpu.children.push_back(std::move(present));

    // ---- the GPU
    if (w.gpu_frames > 0) {
        v.gpu_seen = true;
        v.gpu.name = "GPU (DirectX 12)";
        if (w.gpu_full > 0.0) {
            char note[96];
            std::snprintf(note, sizeof note, "%.0f%% of frames ran out of timestamps: their parts are lumped together",
                          100.0 * w.gpu_full / w.gpu_frames);
            v.gpu.note = note;
        }
        const double gk = 1.0 / w.gpu_frames;
        for (int i = 0; i < VANA_GPU_SCOPES; ++i) {
            const double ms = w.gpu[i] * gk;
            v.gpu.ms += ms;
            if (ms > 0.0005) {
                Node scope = leaf(vanacore_gfx_scope_name(i), ms);
                scope.measure = i == VANA_GPU_SCOPE_COMPOSITE;
                // its passes, the GPU idle between command lists, the rest
                for (int p = 0; p < VANA_GPU_PARTS; ++p) {
                    const double part_ms = w.gpu_parts[i][p] * gk;
                    if (part_ms <= 0.0005)
                        continue;
                    Node part = leaf(vanacore_gfx_part_name(i, p), part_ms);
                    if (i == VANA_GPU_SCOPE_DLSS && p == VANA_GPU_PART_WAIT_DLSS && w.dlss_frames > 0) {
                        // what DLSS's jobs took of the wait, by the helper's own timestamps: the rest is its turn on the
                        // GPU and the handoff between the two processes
                        const double dk = 1.0 / w.dlss_frames;
                        const double nr = w.nr * dk, sr = w.sr * dk;
                        if (nr > 0.0005)
                            part.children.push_back(leaf("DLSS 5 (neural rendering), the helper's own timing", nr));
                        part.children.push_back(leaf("Upscaling (DLSS or FSR), the helper's own timing", sr));
                        part.children.push_back(leaf("The rest (its turn on the GPU, the handoff)", std::max(0.0, part_ms - nr - sr)));
                    }
                    if (i == VANA_GPU_SCOPE_COMPOSITE && p == VANA_GPU_PART_COMP) {
                        // each feature's cost: the whole draw less the draw without it, measured while Composite is open
                        Node features[VANA_GPU_FEATURES];
                        int parents[VANA_GPU_FEATURES];
                        bool any = false;
                        for (int f = 1; f < VANA_GPU_FEATURES; ++f) {
                            features[f] = leaf(vanacore_gfx_feature_name(f, &parents[f]), 0.0);
                            if (w.feature_off[f])
                                features[f].note = "off";
                            else if (w.feature_frames[f] > 0)
                                features[f].ms = w.features[f] / w.feature_frames[f], any = true;
                            else
                                features[f].name.clear();
                        }
                        for (int f = VANA_GPU_FEATURES - 1; f >= 1; --f)
                            if (!features[f].name.empty() && parents[f] > 0 && !features[parents[f]].name.empty())
                                features[parents[f]].children.push_back(std::move(features[f])), features[f].name.clear();
                        for (int f = 1; f < VANA_GPU_FEATURES; ++f)
                            if (!features[f].name.empty())
                                part.children.push_back(std::move(features[f]));
                        sort_by_time(part.children);
                        if (any)
                            part.note = "a feature: the draw less the draw without it";
                        else if (w.measuring)
                            part.note = "measuring each feature...";
                    }
                    scope.children.push_back(std::move(part));
                }
                sort_by_time(scope.children);
                if (scope.children.size() == 1 && scope.children[0].children.empty() && !scope.measure)
                    scope.children.clear(); // one part: the scope itself
                if (i == VANA_GPU_SCOPE_DLSS && w.dlss_frames > 0) {
                    const double fg = w.fg / w.dlss_frames;
                    if (fg > 0.0005 && graphics::shown_per_frame() > 1)
                        scope.children.push_back(leaf("Frame generation, one frame, on the helper's queue (after the scene, not in the above)", fg));
                }
                v.gpu.children.push_back(std::move(scope));
            }
        }
        v.gpu_ms = v.gpu.ms;
    }

    // ---- the counters
    v.counters.name = "Counters (DirectX 12)";
    v.counters.header = true;
    if (a.gfx > 0) {
        v.counters.children.push_back(counter("Draws a frame", a.draws / a.gfx, "draws"));
        v.counters.children.push_back(counter("Uploaded a frame", a.upload / a.gfx, "KB"));
        v.counters.children.push_back(counter(std::string("Pipelines made (") + over + ")", a.pipelines, ""));
        v.counters.children.push_back(counter("Draws left out a frame", a.skipped / a.gfx, "draws"));
        v.counters.children.push_back(counter("Grass tufts made a frame", a.grass_tufts / a.gfx, "tufts"));
        // (their GPU time can't be told apart from the game's draws they're drawn among: /abtest
        // graphics.effects/dlss_reactive_moving=0,0.5 says what it is)
        if (a.redrawn > 0.0)
            v.counters.children.push_back(
                counter("Moving things drawn again for DLSS or FSR a frame (their GPU time is in The game's draws)", a.redrawn / a.gfx, "draws"));
    }
    return v;
}

Node share_node(const sampler::Share& share, double frame_ms)
{
    Node n = leaf(share.name, share.share * frame_ms);
    char text[32];
    std::snprintf(text, sizeof text, "%.1f%% of samples", share.share * 100.0);
    n.note = text;
    for (const sampler::Share& child : share.children)
        n.children.push_back(share_node(child, frame_ms));
    return n;
}

// The sampler's recording as shares of a `frame_ms` frame: by subsystem, the call tree, the hot functions and by module
// (`all`), or the call tree and the hot functions
Node sampled_tree(const sampler::Result& result, double frame_ms, bool all)
{
    Node out;
    out.ms = frame_ms;
    if (!result.have_functions)
        out.note = "ffximain_functions.bin not found: FFXiMain.dll by 256-byte place";
    if (all) {
        // by what the functions are labelled as (data\ffximain_labels.txt): the innermost labelled function the thread
        // was inside of, so what a labelled system calls counts for it
        Node groups = leaf("By subsystem (the innermost labelled function the thread was inside of)", frame_ms);
        for (const sampler::Share& group : result.groups)
            groups.children.push_back(share_node(group, frame_ms));
        out.children.push_back(std::move(groups));
    }
    // the call tree, each function with all it calls
    Node tree = leaf("Call tree (a function and everything it calls)", frame_ms);
    for (const sampler::Share& child : result.tree.children)
        tree.children.push_back(share_node(child, frame_ms));
    out.children.push_back(std::move(tree));
    Node hot = leaf("Hot functions (the thread was in them itself)", 0.0);
    for (const sampler::Share& function : result.hot) {
        hot.children.push_back(share_node(function, frame_ms));
        hot.ms += hot.children.back().ms;
    }
    out.children.push_back(std::move(hot));
    if (all) {
        Node modules = leaf("By module (where the thread was)", frame_ms);
        for (const sampler::Share& module : result.modules)
            if (module.share >= 0.002)
                modules.children.push_back(share_node(module, frame_ms));
        out.children.push_back(std::move(modules));
    }
    return out;
}

// The recording's average frame, in ms
double recording_frame_ms(const Recording& r)
{
    return r.total.acc.frames > 0 ? r.total.acc.frame_sum / r.total.acc.frames : 0.0;
}

// How long the recording has run, in seconds
double recording_seconds(const Recording& r)
{
    LARGE_INTEGER end = r.until;
    if (end.QuadPart == 0)
        QueryPerformanceCounter(&end);
    return static_cast<double>(end.QuadPart - r.since.QuadPart) / static_cast<double>(g_frequency.QuadPart);
}

// The recording's samples as trees, into `all` and `slow` (empty when there are none); false: no samples yet
bool sampled_trees(Node& all, Node& slow)
{
    all = {};
    slow = {};
    const sampler::Result everything = sampler::result();
    if (everything.samples == 0)
        return false;
    all = sampled_tree(everything, recording_frame_ms(g_recording), true);
    all.name = "Where the render thread is, sampled (" + std::to_string(everything.samples) + " samples)";
    const sampler::Result slowest = sampler::result(true);
    const Window& t = g_recording.total;
    if (slowest.samples > 0 && t.slow > 0) {
        const double each = t.slow_ms / t.slow;
        slow = sampled_tree(slowest, each, false);
        char name[200];
        std::snprintf(name, sizeof name, "The slow frames, sampled (%u frames over twice the average round them, %.1f ms each; %u samples)", t.slow,
                      each, slowest.samples);
        slow.name = name;
    }
    return true;
}

// What the window shows, from the look's Window
void refresh(const Window& w)
{
    const double fk = w.acc.frames > 0 ? 1.0 / w.acc.frames : 0.0;

    // each addon's cost and what it was for, per frame, for its window
    g_addon_view.clear();
    for (const auto& [name, ms] : g_addons)
        g_addon_view[name] = ms * fk;
    g_event_view.clear();
    for (const auto& [name, events] : g_events) {
        std::vector<EventRow>& rows = g_event_view[name];
        for (const auto& [event, value] : events)
            rows.push_back({event, value.first * fk, value.second * fk});
        std::sort(rows.begin(), rows.end(), [](const EventRow& x, const EventRow& y) { return x.ms > y.ms; });
    }
    if (!g_detail.empty()) {
        addons::Info info;
        if (g_memory_of != g_detail) {
            g_memory.clear();
            g_memory_of = g_detail;
        }
        if (addons::info(g_detail, info)) {
            g_memory.push_back(static_cast<float>(info.memory_kb));
            while (g_memory.size() > 120)
                g_memory.pop_front();
        }
    }

    // each plugin's calls, all of them, per frame, for its window
    g_plugin_view.clear();
    g_plugin_event_view.clear();
    for (const auto& [name, events] : g_plugin_events) {
        std::vector<EventRow>& rows = g_plugin_event_view[name];
        double total = 0.0;
        for (const auto& [event, value] : events) {
            rows.push_back({event, value.first * fk, value.second * fk});
            total += value.first * fk;
        }
        std::sort(rows.begin(), rows.end(), [](const EventRow& x, const EventRow& y) { return x.ms > y.ms; });
        g_plugin_view[name] = total;
    }
    if (!g_plugin_detail.empty()) {
        if (g_plugin_memory_of != g_plugin_detail) {
            g_plugin_memory.clear();
            g_plugin_memory_of = g_plugin_detail;
        }
        plugin_memory::Usage use;
        if (plugin_memory::usage(g_plugin_detail, use)) {
            g_plugin_memory.push_back(static_cast<float>(use.heap_kb + use.virtual_kb));
            while (g_plugin_memory.size() > 120)
                g_plugin_memory.pop_front();
        }
    }

    View v = build(w, g_average ? "in these five seconds" : "in this half second");
    g_cpu = std::move(v.cpu);
    g_gpu = std::move(v.gpu);
    g_counters = std::move(v.counters);
    g_frame_ms = v.frame_ms;
    g_fps = v.fps;
    g_gpu_ms = v.gpu_ms;
    g_gpu_seen = v.gpu_seen;

    // the sampled render thread: the recording so far
    g_sampled_seen = g_recording.have && sampled_trees(g_sampled, g_slow);

    SYSTEMTIME time;
    GetLocalTime(&time);
    std::snprintf(g_built_at, sizeof g_built_at, "%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    g_have = true;
}

// A look at what's been collected, `seconds` of it, up to `now`: into the recording while one runs, and shown
void write_dip(const Window& w, const std::vector<sampler::Span>& dip, const Point& p);

// A line of a tree into the recording's items, with its % of the look's frame time (a counter's count), and its children
void flatten(Recording& r, Point& p, const Node& n, int parent, double frame_ms)
{
    const auto made = r.item_at.try_emplace({parent, n.name}, static_cast<int>(r.items.size()));
    if (made.second)
        r.items.push_back({n.name, parent, n.counter, 0.0f});
    const int at = made.first->second;
    const float value = n.counter ? static_cast<float>(n.ms) : (frame_ms > 0.0 ? static_cast<float>(100.0 * n.ms / frame_ms) : 0.0f);
    p.items.emplace_back(at, value);
    r.items[static_cast<size_t>(at)].peak = std::max(r.items[static_cast<size_t>(at)].peak, value);
    for (const Node& c : n.children)
        flatten(r, p, c, at, frame_ms);
}

// Everything the profiler shows for a look, as the recording's items: the CPU's tree, the GPU's, the counters, the sampled thread
void record_items(Recording& r, Point& p, const Window& w)
{
    const View v = build(w, "in a look");
    flatten(r, p, v.cpu, -1, v.frame_ms);
    if (v.gpu_seen)
        flatten(r, p, v.gpu, -1, v.frame_ms);
    flatten(r, p, v.counters, -1, v.frame_ms);
    const sampler::Result sampled = sampler::look();
    if (sampled.samples > 0) {
        Node tree = sampled_tree(sampled, v.frame_ms, true);
        tree.name = "Where the render thread is, sampled";
        flatten(r, p, tree, -1, v.frame_ms);
    }
}

// What a look's frame time went to, as percentages, into `p`
void fill_shares(Point& p, const Window& w)
{
    const Acc& a = w.acc;
    if (a.frames == 0 || a.parts == 0 || a.frame_sum <= 0.0)
        return;
    const double of_parts = 100.0 * a.frames / (a.frame_sum * a.parts); // a part's sum over the frames Detailed timing saw
    p.game_pct = static_cast<float>(a.game * of_parts);
    p.d3d_pct = static_cast<float>(a.direct3d * of_parts);
    p.sleep_pct = static_cast<float>(a.sleeps * of_parts);
    p.vana_pct = static_cast<float>(a.vanacore * of_parts);
    p.limit_pct = static_cast<float>((a.limit + a.present) * of_parts);
    const double of_frames = 100.0 / a.frame_sum;
    std::vector<std::pair<std::string, double>> who;
    for (const auto& [name, ms] : w.addons)
        who.emplace_back(name, ms * of_frames);
    for (int s = 0; s < kSectionCount; ++s)
        if (s != kAddons)
            who.emplace_back(kSectionNames[s], w.sections[s] * of_frames);
    std::sort(who.begin(), who.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    for (size_t i = 0; i < who.size() && i < 3 && who[i].second >= 1.0; ++i) {
        char text[96];
        std::snprintf(text, sizeof text, "%s%s %.0f%%", p.top.empty() ? "" : ", ", who[i].first.c_str(), who[i].second);
        p.top += text;
    }
}

void look(double seconds, LARGE_INTEGER now)
{
    std::vector<sampler::Span> slow, dip;
    const Window w = gather(seconds, slow, dip);
    if (sampler::running()) {
        sampler::take(slow, now.QuadPart, dip);
        add(g_recording.total, w);
        Point p;
        p.at = static_cast<float>(static_cast<double>(now.QuadPart - g_recording.since.QuadPart) / static_cast<double>(g_frequency.QuadPart));
        p.fps = static_cast<float>(w.seconds > 0 ? w.acc.frames / w.seconds : 0.0);
        p.frame_ms = static_cast<float>(w.acc.frames > 0 ? w.acc.frame_sum / w.acc.frames : 0.0);
        p.worst_ms = static_cast<float>(w.worst);
        p.slow = w.slow;
        if (w.gpu_frames > 0) {
            double gpu = 0.0;
            for (int i = 0; i < VANA_GPU_SCOPES; ++i)
                gpu += w.gpu[i];
            p.gpu_ms = static_cast<float>(gpu / w.gpu_frames);
        }
        const hwmon::Reading hw = hwmon::latest();
        p.cpu = hw.cpu, p.cpu_mhz = hw.cpu_mhz;
        p.gpu = hw.gpu, p.gpu_mhz = hw.gpu_mhz, p.gpu_temp = hw.gpu_temp;
        fill_shares(p, w);
        record_items(g_recording, p, w);
        p.dips = static_cast<unsigned>(dip.size());
        if (!dip.empty()) {
            const sampler::Result in = sampler::dip();
            if (!in.hot.empty()) {
                char text[160];
                std::snprintf(text, sizeof text, "%s %.0f%% of the dip frames", in.hot[0].name.c_str(), in.hot[0].share * 100.0);
                p.dip_in = text;
            }
        }
        g_recording.timeline.push_back(p);
        // a lag spike (a frame under 15 fps): all of what that look held, written now. At most one in three seconds, forty a recording.
        if (!dip.empty() && p.at - g_recording.dip_at >= 3.0f && g_recording.dips < 40) {
            g_recording.dip_at = p.at;
            write_dip(w, dip, p);
        }
    }
    if (!g_hold)
        refresh(w);
}

#include "stats_report.hpp" // the report (Record Stats Report, /vana stats save): print ... save_report
#include "stats_shadows.hpp" // the shadows every frame of a recording, and every setting beside each report

// ---- the window --------------------------------------------------------------------------------------------------------

// A part's colour, by its place among its parts: the /vana window's colours, then round the hues
ImU32 colour(int index, float alpha = 1.0f)
{
    static const unsigned kParts[] = {P::accent, P::amber, P::teal, P::rose, P::purple, P::green, P::cyan, P::yellow};
    if (index >= 0 && index < IM_ARRAYSIZE(kParts))
        return u32(kParts[index], alpha);
    return ImColor::HSV(std::fmod(static_cast<float>(index) * 0.137f, 1.0f), 0.45f, 0.9f, alpha);
}

// A bar of the parts of `total`, each its width and colour, with its name when the mouse is on it; `label` before it
void stacked_bar(const char* id, const char* label, const Node& root, double total)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float label_w = S(42);
    const float width = ImGui::GetContentRegionAvail().x - label_w, height = S(12), row = S(20);
    const float top = origin.y + std::floor((row - height) * 0.5f), left = origin.x + label_w, radius = height * 0.5f;
    ImDrawList* list = ImGui::GetWindowDrawList();
    text_mid(list, origin.x, origin.y + row * 0.5f, label, P::faint, g_fonts.semi, kSmall - 1.0f);
    list->AddRectFilled(ImVec2(left, top), ImVec2(left + width, top + height), u32(P::well), radius);
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool over_bar = ImGui::IsWindowHovered() && mouse.y >= top && mouse.y < top + height;
    // the parts that show, so the first and last take the bar's rounded ends
    int first = -1, last = -1, i = 0;
    for (const Node& part : root.children) {
        if (total > 0.0 && static_cast<float>(part.ms / total) * width >= 0.5f) {
            if (first < 0)
                first = i;
            last = i;
        }
        ++i;
    }
    float x = left;
    i = 0;
    for (const Node& part : root.children) {
        const float w = total > 0.0 ? std::min(static_cast<float>(part.ms / total) * width, left + width - x) : 0.0f;
        const bool hovered = over_bar && mouse.x >= x && mouse.x < x + w;
        if (w >= 0.5f) {
            const ImDrawFlags corners = (i == first ? ImDrawFlags_RoundCornersLeft : 0) | (i == last ? ImDrawFlags_RoundCornersRight : 0);
            const float gap = i == last || w < S(3) ? 0.0f : 1.0f; // a hairline between parts
            const float round = corners != 0 ? radius : 0.0f;
            const ImDrawFlags flags = corners != 0 ? corners : ImDrawFlags_RoundCornersNone;
            list->AddRectFilled(ImVec2(x, top), ImVec2(x + w - gap, top + height), colour(i), round, flags);
            if (hovered)
                list->AddRectFilled(ImVec2(x, top), ImVec2(x + w - gap, top + height), u32(P::white, 0.25f), round, flags);
        }
        if (hovered)
            ImGui::SetTooltip("%s\n%.2f ms (%.1f%%)", part.name.c_str(), part.ms, total > 0 ? 100.0 * part.ms / total : 0.0);
        x += w;
        ++i;
    }
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(label_w + width, row));
    ImGui::PopID();
}

// The recording over time: each look's average frame (blue) and its slowest (amber), with what a look was when the
// mouse is on it
void timeline_plot(const std::vector<Point>& points)
{
    if (points.size() < 2)
        return;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = S(64);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), u32(P::well), S(4));
    list->AddRect(origin, ImVec2(origin.x + width, origin.y + height), u32(P::well_edge), S(4));
    float top = 1.0f;
    for (const Point& p : points)
        top = std::max(top, p.worst_ms);
    top *= 1.1f;
    const float step = width / static_cast<float>(points.size() - 1);
    const auto y = [&](float ms) { return origin.y + height * (1.0f - std::min(ms / top, 1.0f)); };
    for (size_t i = 1; i < points.size(); ++i) {
        const float x0 = origin.x + step * static_cast<float>(i - 1), x1 = origin.x + step * static_cast<float>(i);
        list->AddLine(ImVec2(x0, y(points[i - 1].worst_ms)), ImVec2(x1, y(points[i].worst_ms)), u32(P::amber));
        list->AddLine(ImVec2(x0, y(points[i - 1].frame_ms)), ImVec2(x1, y(points[i].frame_ms)), u32(P::accent), 1.5f);
    }
    char scale[32];
    std::snprintf(scale, sizeof scale, "%.0f ms", top);
    text_mid(list, origin.x + S(6), origin.y + S(10), scale, P::dim, g_fonts.text, kSmall - 1.0f);
    ImGui::Dummy(ImVec2(width, height));
    if (ImGui::IsItemHovered()) {
        const float at = (ImGui::GetMousePos().x - origin.x) / step;
        const Point& p = points[static_cast<size_t>(std::clamp(at + 0.5f, 0.0f, static_cast<float>(points.size() - 1)))];
        ImGui::SetTooltip("%s into the recording\nframe %.2f ms (%.1f fps), the slowest %.1f ms%s", minutes(p.at, true).c_str(), p.frame_ms, p.fps,
                          p.worst_ms, p.slow > 0 ? ("\n" + std::to_string(p.slow) + " slow (over twice the average)").c_str() : "");
    }
    push_font(g_fonts.text, kSmall);
    ImGui::TextDisabled("Each look's frame (blue) and its slowest (amber), over the recording");
    ImGui::PopFont();
}

// The search box's text, lower case: empty shows everything
std::string g_filter;

bool name_matches(const std::string& name)
{
    std::string lower = name;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find(g_filter) != std::string::npos;
}

bool subtree_matches(const Node& n)
{
    if (name_matches(n.name))
        return true;
    for (const Node& child : n.children)
        if (subtree_matches(child))
            return true;
    return false;
}

// With a search, a row shows when its name matches, or a part under it does, or a row above it matched
// A row's name besides: its note dim after it (a plugin's memory, a feature that's off, why a part isn't timed; label_x where
// the name starts), a click opening an addon's or a plugin's window, and the measuring while it's open (`open`, with lines under)
void row_label(const Node& n, float label_x, bool open)
{
    if (!n.note.empty()) {
        const float y = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
        text_mid(ImGui::GetWindowDrawList(), label_x + ImGui::CalcTextSize(n.name.c_str()).x + S(10), y, n.note.c_str(), P::dim, g_fonts.text,
                 kSmall);
    }
    if (n.measure) {
        if (open)
            vanacore_gfx_measure();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("While this is open, its shader is drawn again each frame with one feature off, to measure what each costs");
    }
    if (n.addon) {
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click for this addon's detail");
        if (ImGui::IsItemClicked())
            g_detail = n.name;
    }
    if (n.plugin) {
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click for this plugin's detail: all its calls, and its memory");
        if (ImGui::IsItemClicked())
            g_plugin_detail = n.name;
    }
}

void row(const Node& n, double frame_ms, int top_index, double open_share = 0.0, bool above_matched = false)
{
    const bool filtering = !g_filter.empty();
    const bool matched = filtering && name_matches(n.name);
    if (filtering && !above_matched && !subtree_matches(n))
        return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth;
    const bool is_leaf = n.children.empty();
    if (is_leaf)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (top_index >= 0 || (open_share > 0.0 && n.ms >= open_share * frame_ms))
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (top_index >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, colour(top_index));
        push_font(g_fonts.semi, kText);
    }
    if (filtering && !matched && !above_matched && !is_leaf)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always); // down to what matched
    const float label_x = ImGui::GetCursorScreenPos().x + ImGui::GetTreeNodeToLabelSpacing();
    const bool open = ImGui::TreeNodeEx(n.name.c_str(), flags);
    row_label(n, label_x, open && !is_leaf);
    if (top_index >= 0) {
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
    ImGui::TableNextColumn();
    if (n.header) {
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
    } else if (n.counter) {
        ImGui::Text("%.1f", n.ms);
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", n.unit.c_str());
    } else {
        ImGui::Text("%.3f", n.ms);
        ImGui::TableNextColumn();
        if (n.kb >= 0.0)
            ImGui::TextColored(vec4(P::text2), "%.2f", n.kb / 1024.0); // MB always: the column's title says so
        // CPU: its share of the frame, the number and its bar after it
        ImGui::TableNextColumn();
        const double share = frame_ms > 0 ? n.ms / frame_ms : 0.0;
        char percent[16];
        std::snprintf(percent, sizeof percent, "%.1f%%", 100.0 * share);
        // in the cell's screen place (SameLine's offset counts from the window, not the cell): the number right-aligned
        // in a fixed width, so the bars start together after it
        const float number_w = ImGui::CalcTextSize("100.0%").x;
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(cell.x + number_w - ImGui::CalcTextSize(percent).x, cell.y));
        ImGui::TextColored(vec4(P::text2), "%s", percent);
        ImGui::SetCursorScreenPos(ImVec2(cell.x + number_w + S(10), cell.y));
        meter(share, top_index >= 0 ? colour(top_index) : u32(P::accent));
    }
    if (!is_leaf && open) {
        for (const Node& child : n.children)
            row(child, frame_ms, -1, open_share, above_matched || matched);
        ImGui::TreePop();
    }
}

const char* state_name(const std::string& state)
{
    if (state == "N") return "Compiled Lua (JIT)";
    if (state == "I") return "Interpreted Lua";
    if (state == "C") return "C code (VanaCore's API, libraries, FFI)";
    if (state == "G") return "Garbage collection";
    if (state == "J") return "The JIT compiling";
    return "Other";
}

// "L\t<n>\t<key>" lines of the profile's report into g_profile
void parse_profile(const std::string& report)
{
    g_profile.total = 0.0;
    g_profile.states.clear();
    g_profile.lines.clear();
    g_profile.functions.clear();
    size_t at = 0;
    while (at < report.size()) {
        size_t end = report.find('\n', at);
        if (end == std::string::npos)
            end = report.size();
        const std::string line = report.substr(at, end - at);
        at = end + 1;
        if (line.size() < 3 || line[1] != '\t')
            continue;
        const size_t tab = line.find('\t', 2);
        if (line[0] == 'T') {
            g_profile.total = std::atof(line.c_str() + 2);
        } else if (tab != std::string::npos) {
            const std::string first = line.substr(2, tab - 2), second = line.substr(tab + 1);
            if (line[0] == 'S')
                g_profile.states.push_back({state_name(first), std::atof(second.c_str())});
            else if (line[0] == 'L')
                g_profile.lines.push_back({second, std::atof(first.c_str())});
            else if (line[0] == 'F')
                g_profile.functions.push_back({second, std::atof(first.c_str())});
        }
    }
    std::sort(g_profile.states.begin(), g_profile.states.end(), [](const Share& x, const Share& y) { return x.samples > y.samples; });
}

const ImVec4 kHeading = vec4(P::accent);
const ImVec4 kBad = vec4(P::rose);

// A part of the addon window: its name, and a rule from it to the right edge
void heading(const char* text)
{
    ImGui::Spacing();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    push_font(g_fonts.semi, kText);
    ImGui::TextColored(vec4(P::text2), "%s", text);
    ImGui::PopFont();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float y = std::floor((min.y + max.y) * 0.5f);
    if (max.x + S(8) < right)
        ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x + S(8), y), ImVec2(right, y), u32(P::line));
}

// Text in the semibold at a larger size, for the window's main numbers
void big_text(const char* text, const ImVec4* colour = nullptr, float size = kBig - 4.0f)
{
    push_font(g_fonts.semi, size);
    ImGui::TextColored(colour != nullptr ? *colour : vec4(P::white), "%s", text);
    ImGui::PopFont();
}

// A number at the right of its table cell
void right_text(const char* format, double value)
{
    char text[32];
    std::snprintf(text, sizeof text, format, value);
    const float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x;
    if (room > 0.0f)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room);
    ImGui::TextUnformatted(text);
}

// A column of numbers as wide as its heading or `widest`, the widest it writes, whichever is wider
void number_column(const char* name, const char* widest)
{
    ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, std::max(ImGui::CalcTextSize(name).x, ImGui::CalcTextSize(widest).x));
}

// A faint bar behind what's written in a table cell, `fraction` of its width
void cell_bar(double fraction)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x * static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    if (width >= 1.0f)
        ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + width, at.y + ImGui::GetTextLineHeight()), u32(P::accent, 0.25f), S(2));
}

std::string memory_words(double kb)
{
    char text[32];
    if (kb >= 1024.0)
        std::snprintf(text, sizeof text, "%.2f MB", kb / 1024.0);
    else
        std::snprintf(text, sizeof text, "%.0f KB", kb);
    return text;
}

// A profile's file:line or function from the addon's own folder on: "trove\util.lua:12" for
// "D:\...\addons\ashita\trove\util.lua:12"
std::string short_key(const std::string& key)
{
    std::string lower = key;
    for (char& c : lower)
        c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t at = lower.rfind("\\addons\\");
    if (at == std::string::npos)
        return key;
    at += std::strlen("\\addons\\");
    for (const char* kind : {"ashita\\", "windower\\", "vanacore\\"}) {
        if (lower.compare(at, std::strlen(kind), kind) == 0) {
            at += std::strlen(kind);
            break;
        }
    }
    return key.substr(at);
}

// A part of the profile's table: its heading, then what it found, each with its share of the samples
void profile_rows(const char* title, const std::vector<Share>& rows, double total)
{
    if (rows.empty())
        return;
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
    ImGui::TableNextColumn();
    push_font(g_fonts.semi, kText);
    ImGui::TextColored(kHeading, "%s", title);
    ImGui::PopFont();
    const float indent = ImGui::GetStyle().IndentSpacing * 0.5f;
    for (const Share& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        const std::string shown = short_key(row.what);
        const bool cut = shown.size() != row.what.size() || ImGui::CalcTextSize(shown.c_str()).x > ImGui::GetContentRegionAvail().x;
        ImGui::TextUnformatted(shown.c_str());
        if (cut && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", row.what.c_str());
        ImGui::TableNextColumn();
        const double share = total > 0 ? row.samples / total : 0.0;
        cell_bar(share);
        right_text("%.1f%%", 100.0 * share);
        ImGui::TableNextColumn();
        right_text("%.0f", row.samples);
    }
}

// The window for the addon that was clicked in the tree: what it costs, its memory and errors, what its calls were for,
// and a sampling profile of its Lua.
void draw_addon_detail(double frame_ms)
{
    if (g_detail.empty())
        return;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 600.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(840.0f, 100.0f), ImGuiCond_FirstUseEver);
    push_theme();
    const bool shown = begin_window("Addon###vana_addon_detail");
    if (shown) {
        title_bar(kIconAddon, ("Addon: " + g_detail).c_str(), nullptr, open);
        // Pause keeps what the window shows as it is, to be read: the numbers as they were when it was ticked
        static bool hold = false;
        static std::string frozen_of;
        static addons::Info frozen_info;
        static double frozen_ms = 0.0, frozen_frame = 0.0;
        static std::vector<EventRow> frozen_events;
        static std::vector<float> frozen_memory;
        if (frozen_of != g_detail) {
            hold = false;
            frozen_of = g_detail;
        }
        addons::Info live_info;
        const bool live_loaded = addons::info(g_detail, live_info);
        const auto live_cost = g_addon_view.find(g_detail);
        const double live_ms = live_cost != g_addon_view.end() ? live_cost->second : 0.0;
        const auto live_view = g_event_view.find(g_detail);
        const std::vector<EventRow> no_events;
        const std::vector<EventRow>& live_events = live_view != g_event_view.end() ? live_view->second : no_events;
        const std::vector<float> live_memory(g_memory.begin(), g_memory.end());
        // what's shown: Pause's copy, or the live numbers (the frame Pause is ticked in still shows them)
        const bool paused = hold;
        const bool loaded = paused || live_loaded;
        const addons::Info& info = paused ? frozen_info : live_info;
        const double ms = paused ? frozen_ms : live_ms;
        const double shown_frame = paused ? frozen_frame : frame_ms;
        const std::vector<EventRow>& events = paused ? frozen_events : live_events;
        const std::vector<float>& memory = paused ? frozen_memory : live_memory;

        // The top line: what it is and what it listens for, Pause at the right
        const ImGuiStyle& style = ImGui::GetStyle();
        const float pause_x = ImGui::GetWindowContentRegionMax().x - toggle_width("Pause");
        ImGui::PushTextWrapPos(pause_x - style.ItemSpacing.x);
        ImGui::AlignTextToFramePadding();
        if (!loaded) {
            ImGui::TextDisabled("It isn't loaded any more.");
        } else {
            std::string listens;
            for (const std::string& what : info.listens)
                listens += (listens.empty() ? "" : ", ") + what;
            ImGui::Text("%s addon", info.kind.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(listens.empty() ? "listens for nothing beyond the frame" : "also listens for");
            if (!listens.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted(listens.c_str());
            }
        }
        ImGui::PopTextWrapPos();
        ImGui::SameLine(std::max(pause_x, ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + style.ItemSpacing.x));
        if (toggle("addon_pause", "Pause", hold, 26.0f) && hold) {
            frozen_info = live_info;
            frozen_ms = live_ms;
            frozen_frame = frame_ms;
            frozen_events = live_events;
            frozen_memory = live_memory;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keeps the numbers as they are, to read them");

        if (loaded) {
            if (info.crashed)
                ImGui::TextColored(kBad, "It crashed: nothing of it runs any more");
            ImGui::Spacing();

            // Its cost, its memory and its errors, side by side
            static bool show_errors = false;
            static std::string errors_of;
            if (errors_of != g_detail) {
                show_errors = false;
                errors_of = g_detail;
            }
            if (ImGui::BeginTable("addon_numbers", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("cost", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("memory", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("errors", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                ImGui::TableNextRow();
                char text[48];

                ImGui::TableNextColumn();
                ImGui::TextDisabled("Frame cost");
                std::snprintf(text, sizeof text, "%.3f ms", ms);
                big_text(text);
                const double share = shown_frame > 0 ? ms / shown_frame : 0.0;
                ImGui::TextWrapped("%.1f%% of the %.2f ms frame", 100.0 * share, shown_frame);
                meter(share, u32(P::accent));

                ImGui::TableNextColumn();
                ImGui::TextDisabled("Lua memory");
                big_text(memory_words(info.memory_kb).c_str());
                if (memory.size() > 1) {
                    const auto [low, high] = std::minmax_element(memory.begin(), memory.end());
                    const float pad = std::max(1.0f, (*high - *low) * 0.1f); // a flat line shows in the middle
                    ImGui::PlotLines("##addon_memory", memory.data(), static_cast<int>(memory.size()), 0, nullptr, *low - pad,
                                     *high + pad, ImVec2(-FLT_MIN, 36.0f));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Its Lua memory, a point for each look at the numbers: %s to %s.\nClimbing without stopping is a leak.",
                                          memory_words(*low).c_str(), memory_words(*high).c_str());
                }

                ImGui::TableNextColumn();
                ImGui::TextDisabled("Errors");
                std::snprintf(text, sizeof text, "%d", info.errors);
                big_text(text, info.errors > 0 ? &kBad : nullptr);
                if (info.errors > 0 && button("addon_errors_show", show_errors ? "Hide them" : "Show them", nullptr, Kind::ghost))
                    show_errors = !show_errors;
                ImGui::EndTable();
            }
            if (show_errors && info.errors > 0) {
                const std::vector<addons::ErrorEntry> errors = addons::recent_errors(g_detail);
                ImGui::Spacing();
                if (button("addon_errors_copy", "Copy all", nullptr, Kind::ghost)) {
                    std::string all;
                    for (const addons::ErrorEntry& e : errors)
                        all += e.time + "  in " + e.what + (e.count > 1 ? "  (x" + std::to_string(e.count) + ")" : "") + "\n" + e.details + "\n\n";
                    ImGui::SetClipboardText(all.c_str());
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%d kinds, the newest last; the log has them all", static_cast<int>(errors.size()));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, vec4(P::well));
                const bool listed = ImGui::BeginChild("##addon_errors", ImVec2(0.0f, 220.0f), ImGuiChildFlags_Borders);
                ImGui::PopStyleColor();
                if (listed) {
                    for (size_t i = 0; i < errors.size(); ++i) {
                        const addons::ErrorEntry& e = errors[i];
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TextColored(kBad, "%s  in %s%s", e.time.c_str(), e.what.c_str(),
                                           e.count > 1 ? ("  (x" + std::to_string(e.count) + ")").c_str() : "");
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextUnformatted(e.details.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                    if (errors.empty())
                        ImGui::TextDisabled("None kept: they were reported before this window could keep them.");
                }
                ImGui::EndChild();
            }

            heading("What its calls were for (per frame)");
            if (events.empty()) {
                ImGui::TextDisabled("Collecting...");
            } else if (ImGui::BeginTable("addon_events", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
                number_column("ms", "00.000");
                number_column("calls", "000.00");
                number_column("ms a call", "0.0000");
                headers_row();
                for (const EventRow& row : events) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.name.c_str());
                    if (row.name == "frame") {
                        ImGui::SameLine();
                        ImGui::TextDisabled("its tasks, and its present and prerender events");
                    }
                    ImGui::TableNextColumn();
                    right_text("%.3f", row.ms);
                    ImGui::TableNextColumn();
                    right_text("%.2f", row.calls);
                    ImGui::TableNextColumn();
                    right_text("%.4f", row.calls > 0 ? row.ms / row.calls : 0.0);
                }
                ImGui::EndTable();
            }

            heading("Where its Lua spends the time (a sampling profile)");
            if (g_profiling && g_profile_of == g_detail) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const double seconds = static_cast<double>(now.QuadPart - g_profile_since.QuadPart) / static_cast<double>(g_frequency.QuadPart);
                if (button("addon_profile_stop", "Stop and show", nullptr, Kind::danger)) {
                    std::string report, error;
                    g_profile = {};
                    g_profile.addon = g_detail;
                    g_profile.seconds = seconds;
                    if (addons::profile_stop(g_detail, report, error))
                        parse_profile(report);
                    else
                        g_profile.error = error;
                    g_profiling = false;
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kBad, "Sampling %.1f s", seconds);
            } else if (g_profiling) {
                ImGui::TextDisabled("Sampling %s: stop that first", g_profile_of.c_str());
            } else {
                if (button("addon_profile_start", "Start profiling", nullptr, Kind::ghost)) {
                    std::string error;
                    g_profile = {};
                    g_profile.addon = g_detail;
                    if (addons::profile_start(g_detail, error)) {
                        g_profiling = true;
                        g_profile_of = g_detail;
                        QueryPerformanceCounter(&g_profile_since);
                    } else {
                        g_profile.error = error;
                    }
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Looks where its Lua is every millisecond; costs a little");
            }
            if (g_profile.addon == g_detail) {
                if (!g_profile.error.empty()) {
                    ImGui::TextColored(kBad, "%s", g_profile.error.c_str());
                } else if (g_profile.total > 0) {
                    ImGui::TextDisabled("%.0f samples in %.1f s", g_profile.total, g_profile.seconds);
                    if (ImGui::BeginTable("addon_profile", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("100.0%").x * 2.5f);
                        number_column("Samples", "000000");
                        headers_row();
                        profile_rows("What the VM was doing", g_profile.states, g_profile.total);
                        profile_rows("Hot functions", g_profile.functions, g_profile.total);
                        profile_rows("Hot lines", g_profile.lines, g_profile.total);
                        ImGui::EndTable();
                    }
                } else if (!g_profiling) {
                    ImGui::TextDisabled("Nothing was sampled (it didn't run Lua while it was on)");
                }
            }
        }
    }
    end_window(shown);
    pop_theme();
    if (!open) {
        if (g_profiling && g_profile_of == g_detail) {
            std::string report, error;
            addons::profile_stop(g_detail, report, error);
            g_profiling = false;
        }
        g_detail.clear();
    }
}

// The window for the plugin that was clicked in the tree: all its calls (not only the ones in the frame's plugins part:
// its packets, text, commands and the rest), and its memory (plugin_memory.hpp)
void draw_plugin_detail(double frame_ms)
{
    if (g_plugin_detail.empty())
        return;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(880.0f, 140.0f), ImGuiCond_FirstUseEver);
    push_theme();
    const bool shown = begin_window("Plugin###vana_plugin_detail");
    if (shown) {
        title_bar(kIconAddon, ("Plugin: " + g_plugin_detail).c_str(), nullptr, open);
        plugins::Details details;
        const bool loaded = plugins::details(g_plugin_detail, details);
        plugin_memory::Usage use;
        const bool counted = plugin_memory::usage(g_plugin_detail, use);
        const auto cost = g_plugin_view.find(g_plugin_detail);
        const double ms = cost != g_plugin_view.end() ? cost->second : 0.0;
        const auto view = g_plugin_event_view.find(g_plugin_detail);

        if (!loaded) {
            ImGui::TextDisabled("It isn't loaded any more.");
        } else {
            ImGui::Text("%s", details.kind.c_str());
            if (!details.about.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", details.about.c_str());
            }
            if (details.crashed)
                ImGui::TextColored(kBad, "It crashed: it isn't called any more (its DLL and memory stay)");
            ImGui::Spacing();

            if (ImGui::BeginTable("plugin_numbers", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("cost", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("memory", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableNextRow();
                char text[48];

                ImGui::TableNextColumn();
                ImGui::TextDisabled("Frame cost (all its calls)");
                std::snprintf(text, sizeof text, "%.3f ms", ms);
                big_text(text);
                const double share = frame_ms > 0 ? ms / frame_ms : 0.0;
                ImGui::TextWrapped("%.1f%% of the %.2f ms frame", 100.0 * share, frame_ms);
                meter(share, u32(P::accent));

                ImGui::TableNextColumn();
                ImGui::TextDisabled("Memory it has allocated");
                if (!counted) {
                    big_text("not counted");
                    ImGui::TextWrapped("More plugins were loaded at once than VanaCore counts (the log says)");
                } else {
                    big_text(memory_words(use.heap_kb + use.virtual_kb).c_str());
                    if (g_plugin_memory.size() > 1) {
                        const std::vector<float> memory(g_plugin_memory.begin(), g_plugin_memory.end());
                        const auto [low, high] = std::minmax_element(memory.begin(), memory.end());
                        const float pad = std::max(1.0f, (*high - *low) * 0.1f);
                        ImGui::PlotLines("##plugin_memory", memory.data(), static_cast<int>(memory.size()), 0, nullptr, *low - pad,
                                         *high + pad, ImVec2(-FLT_MIN, 36.0f));
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("What it has allocated, a point for each look at the numbers: %s to %s.\n"
                                              "Climbing without stopping is a leak.",
                                              memory_words(*low).c_str(), memory_words(*high).c_str());
                    }
                    ImGui::TextDisabled("Heap: %s in %u blocks", memory_words(use.heap_kb).c_str(), use.blocks);
                    ImGui::TextDisabled("VirtualAlloc: %s reserved", memory_words(use.virtual_kb).c_str());
                    ImGui::TextDisabled("Its DLL: %s; the most at once: %s", memory_words(use.image_kb).c_str(),
                                        memory_words(use.peak_kb).c_str());
                }
                ImGui::EndTable();
            }
            if (counted && use.counters == 0)
                ImGui::TextColored(vec4(P::amber), "It imports none of the allocators VanaCore counts: its memory can't be seen");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Counted from as it loads: its C runtime and Windows heap blocks, and the address space it reserves. "
                                "Not what other DLLs allocate for it (Direct3D's textures, its runtime's own).");
            ImGui::PopTextWrapPos();
        }

        heading("What its calls were for (per frame)");
        if (view == g_plugin_event_view.end() || view->second.empty()) {
            if (loaded)
                ImGui::TextDisabled("Nothing called it yet (it's timed on the game's thread only)");
        } else if (ImGui::BeginTable("plugin_events", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthStretch);
            number_column("ms", "00.000");
            number_column("calls", "000.00");
            number_column("ms a call", "0.0000");
            headers_row();
            for (const EventRow& row : view->second) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::TableNextColumn();
                right_text("%.3f", row.ms);
                ImGui::TableNextColumn();
                right_text("%.2f", row.calls);
                ImGui::TableNextColumn();
                right_text("%.4f", row.calls > 0 ? row.ms / row.calls : 0.0);
            }
            ImGui::EndTable();
        }
    }
    end_window(shown);
    pop_theme();
    if (!open)
        g_plugin_detail.clear();
}

// A rose pill at the cursor, `width` wide (at least its contents'), centred in a row `row_h` tall: the dot beating, REC
// and how long the recording has run, centred in it
void recording_pill(float row_h, float width)
{
    const std::string recording = "REC " + minutes(recording_seconds(g_recording));
    const float inner = S(14) + text_width(recording.c_str(), g_fonts.semi, kSmall); // the dot (and its gap), then the text
    const float w = std::max(width, S(10) + inner + S(14)), h = S(28);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y + std::floor((row_h - h) * 0.5f), cy = y + std::floor(h * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min(at.x, y), max(at.x + w, y + h);
    dl->AddRectFilled(min, max, u32(mix(P::well, P::rose, 0.14f)), h * 0.5f);
    dl->AddRect(min, max, u32(P::rose, 0.5f), h * 0.5f);
    const float pulse = 0.55f + 0.45f * static_cast<float>(std::sin(ImGui::GetTime() * 4.0));
    const float left = at.x + std::floor((w - inner) * 0.5f);
    dl->AddCircleFilled(ImVec2(left + S(4), cy), S(4.5f), u32(P::rose, pulse));
    text_mid(dl, left + S(14), cy, recording.c_str(), P::rose, g_fonts.semi, kSmall);
    ImGui::Dummy(ImVec2(w, row_h));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Recording since %s (Record Stats Report): the stop writes the report", g_recording.started);
}

// The game's address space against all of it (the 32-bit game's limit), "1.7 of 4.0 GB", and what's used as a fraction
std::string address_words(double& fraction)
{
    const perf::AddressSpace space = perf::address_space();
    fraction = space.total_mb > 0 ? space.used_mb / space.total_mb : 0.0;
    char text[48];
    std::snprintf(text, sizeof text, "%.1f of %.1f GB", space.used_mb / 1024.0, space.total_mb / 1024.0);
    return text;
}

// A reading as a line of the left column: its name at the left and `value` at the right (`narrow` when `value` doesn't fit
// beside the name; the name on a line of its own above when neither does), a bar under them (amber near the limit, rose at
// it or while `hot`), and `tip` with the mouse on it
void reading_row(const char* name, const char* value, const char* narrow, double fraction, bool hot, const std::string& tip)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, name_w = text_width(name, g_fonts.text, kSmall) + S(12);
    const auto fits = [&](const char* text) { return text != nullptr && name_w + text_width(text, g_fonts.semi, kSmall) <= width; };
    const char* const shown = fits(value) || narrow == nullptr ? value : narrow;
    const bool beside = fits(shown);
    const float name_h = beside ? 0.0f : S(18), row = S(36) + name_h;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!beside)
        text_mid(dl, at.x, at.y + S(10), name, P::dim, g_fonts.text, kSmall);
    reading_column(dl, at.x, at.y + name_h + S(18), width, beside ? name : "", shown, fraction, hot);
    ImGui::Dummy(ImVec2(width, row));
    if (ImGui::IsItemHovered() && !tip.empty())
        ImGui::SetTooltip("%s", tip.c_str());
}

// The PC and the graphics card now, read once a second, a reading a line with a tooltip each. The PC: how busy the CPU is
// with this game's share of it, the memory in use with this game's, and the game's address space (a 32-bit game's limit).
// The card, as the Graphics page shows it over its presets (vana.lua's draw_hardware): how busy it is and its temperature,
// its power against its limit, and its video memory with every program's in it; on a card without NVIDIA's readings, this
// game's own video memory against Windows' budget for it (DXGI: any card); no card until a reading has come in.
void hardware_rows(const hwmon::Reading& r)
{
    char text[64], narrow[64];

    heading("PC");
    const std::string held = cpu_held(r);
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    char about[320];
    std::snprintf(about, sizeof about,
                  "How busy all the CPU's cores are, and this game's share of them, as Task Manager counts it: 100%% is\n"
                  "every core, so one thread running flat out on this PC's %lu is %.0f%%.%s%s",
                  info.dwNumberOfProcessors, info.dwNumberOfProcessors > 0 ? 100.0 / info.dwNumberOfProcessors : 0.0,
                  held.empty() ? "" : "\n\nThe CPU: ", held.c_str());
    if (r.cpu_load)
        std::snprintf(text, sizeof text, "%.0f%%, game %.0f%%", r.cpu_busy, r.cpu_game);
    else
        std::snprintf(text, sizeof text, "...");
    reading_row("CPU", text, nullptr, r.cpu_busy / 100.0, !held.empty(), about);
    // the PC's memory with this game's: in whole GB when that doesn't fit beside the name
    if (r.ram_total_mb > 0) {
        std::snprintf(text, sizeof text, "%.1f of %.1f GB, game %.1f GB", r.ram_used_mb / 1024.0, r.ram_total_mb / 1024.0, r.ram_game_mb / 1024.0);
        std::snprintf(narrow, sizeof narrow, "%.0f of %.0f GB, game %.1f", r.ram_used_mb / 1024.0, r.ram_total_mb / 1024.0, r.ram_game_mb / 1024.0);
    } else {
        std::snprintf(text, sizeof text, "...");
        std::snprintf(narrow, sizeof narrow, "...");
    }
    reading_row("Memory", text, narrow, r.ram_total_mb > 0 ? r.ram_used_mb / r.ram_total_mb : 0.0, false,
                "The PC's memory in use (every program's) against all of it, and this game's own\n"
                "(its private memory, Task Manager's Commit size; /vana graph has it over time).");
    double used = 0.0;
    const std::string address = address_words(used);
    reading_row("Address space", address.c_str(), nullptr, used, false,
                "What the game has of the 4 GB a 32-bit program can address (its heaps, DLLs and mapped files),\n"
                "which is what runs out, not the PC's memory. /vana graph has it over time, and the largest block left free.");

    char mine[48] = "";
    if (r.vram_budget_mb > 0)
        std::snprintf(mine, sizeof mine, "%.1f of %.1f GB", r.vram_game_mb / 1024.0, r.vram_budget_mb / 1024.0);
    if (!r.gpu && mine[0] == '\0')
        return;
    heading("Graphics card");
    if (!r.gpu) {
        reading_row("Video memory (game)", mine, nullptr, r.vram_game_mb / r.vram_budget_mb, false,
                    "This game's video memory against what Windows lets it have.\nThe card's load, power and heat need an NVIDIA card.");
        return;
    }
    std::string tip;
    std::snprintf(about, sizeof about, "How busy the card is, and its temperature (%u of %u MHz).", r.gpu_mhz, r.gpu_max_mhz);
    tip = about;
    if (r.throttled)
        tip += "\nHeld back: " + r.throttle_text;
    std::snprintf(text, sizeof text, "%u%%  %d C", r.gpu_busy, r.gpu_temp);
    reading_row("GPU", text, nullptr, r.gpu_busy / 100.0, r.throttled, tip);
    if (r.gpu_limit_watts > 0)
        std::snprintf(text, sizeof text, "%u of %u W", r.gpu_watts, r.gpu_limit_watts);
    else
        std::snprintf(text, sizeof text, "%u W", r.gpu_watts);
    reading_row("Power", text, nullptr, r.gpu_limit_watts > 0 ? static_cast<double>(r.gpu_watts) / r.gpu_limit_watts : 0.0, false,
                "The card's power against its limit.");
    tip = "The card's video memory, with every program's in it.";
    if (mine[0] != '\0')
        tip += std::string("\nThis game: ") + mine + " of its budget";
    std::snprintf(text, sizeof text, "%.1f of %.1f GB", r.vram_used_mb / 1024.0, r.vram_total_mb / 1024.0);
    reading_row("Video memory", text, nullptr, r.vram_total_mb > 0 ? r.vram_used_mb / r.vram_total_mb : 0.0, false, tip);
}

// Small text, dim unless told, wrapped at the column's edge
void note(const char* text, unsigned rgb = P::dim)
{
    push_font(g_fonts.text, kSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(rgb));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// The frame's numbers, one under the other, with what they're averaged over under them
void header()
{
    constexpr float kNumber = kBody + 3.0f; // the numbers' size
    const ImVec4 dim = vec4(P::dim);
    char text[96];
    if (!g_have) {
        big_text("Collecting...", &dim, kNumber);
        std::snprintf(text, sizeof text, "The first numbers come in after %s", window_words());
        note(text);
        return;
    }
    std::snprintf(text, sizeof text, "%.2f ms", g_frame_ms);
    big_text(text, nullptr, kNumber);
    std::snprintf(text, sizeof text, "%.1f fps", g_fps);
    push_font(g_fonts.text, kNumber);
    ImGui::TextColored(dim, "%s", text);
    ImGui::PopFont();
    if (g_gpu_seen) {
        std::snprintf(text, sizeof text, "GPU %.2f ms", g_gpu_ms);
        big_text(text, &kHeading, kNumber);
    }
    std::snprintf(text, sizeof text, "%s over %s%s; hover a bar for its part", g_hold ? "Paused: averaged" : "Averaged", window_words(),
                  g_gpu_seen ? "" : ", GPU not timed (DirectX 12 only)");
    note(text);
}

// Record Stats Report the column's width (Stop Recording while it runs, its dot beating, the REC pill with the
// recording's time under it; Saved, for a moment, once the stop has written the report; Record and Stop when the column
// is too narrow for those), then Pause and 5s average
void controls()
{
    static double saved_at = -10.0;
    if (g_saved_now) {
        saved_at = ImGui::GetTime();
        g_saved_now = false;
    }
    const float room = ImGui::GetContentRegionAvail().x;
    const bool wide = std::max(button_width("Record Stats Report", kIconDot, 30.0f), button_width("Stop Recording", kIconDot, 30.0f)) <= room;
    const char* const record = wide ? "Record Stats Report" : "Record";
    const char* const stop = wide ? "Stop Recording" : "Stop";
    const float w = std::max({button_width(record, kIconDot, 30.0f), button_width(stop, kIconDot, 30.0f),
                              button_width("Saved", kIconCheck, 30.0f), room});
    bool clicked = false;
    if (g_sample) {
        const float pulse = 0.55f + 0.45f * static_cast<float>(std::sin(ImGui::GetTime() * 4.0));
        clicked = button("stats_record", stop, kIconDot, Kind::danger, 30.0f, P::rose, pulse, w);
    } else if (ImGui::GetTime() - saved_at < 2.0) {
        clicked = button("stats_record", "Saved", kIconCheck, Kind::success, 30.0f, 0, 1.0f, w);
    } else {
        clicked = button("stats_record", record, kIconDot, Kind::accent, 30.0f, 0, 1.0f, w);
    }
    if (ImGui::IsItemHovered()) {
        if (g_sample)
            ImGui::SetTooltip("Recording: press it to stop. The stop writes the whole recording to one text file in logs\n"
                              "(the PC, the frames, the lag spikes, the shadows, the settings): send that one file.");
        else
            ImGui::SetTooltip("Records where the render thread is about every millisecond, and the frame's numbers, until it's\n"
                              "pressed again (experimental, costs a little FPS). The stop writes the whole recording to a text file\n"
                              "in logs, one file with everything. /vana stats save writes what this window shows at any time.%s%s",
                              g_saved.empty() ? "" : "\nLast saved: ", g_saved.c_str());
    }
    if (clicked)
        g_sample = !g_sample;
    if (sampler::running()) {
        ImGui::Spacing();
        recording_pill(S(30), w);
    }
    ImGui::Spacing();
    toggle("stats_pause", "Pause", g_hold);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keeps the numbers as they are, to read them");
    // beside Pause when the column has the room, else under it
    if (toggle_width("Pause") + S(18) + toggle_width("5s average") <= ImGui::GetContentRegionAvail().x)
        ImGui::SameLine(0.0f, S(18));
    if (toggle("stats_average", "5s average", g_average)) {
        // what was collected over the other length is dropped: the next look starts afresh (a recording keeps its own)
        drop_window();
        g_window_start = {};
        g_last_frame = {};
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each look at the numbers is the average of five seconds, instead of half a second:\nsteadier to read, slower to follow a change");
}

// The search box, as the /vana window's: darker than the page with a lighter edge (the accent's while it's typed in),
// the magnifier in it
void search_box(char* buffer, size_t size)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = S(32);
    const ImVec2 end(at.x + w, at.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, end, u32(P::search), S(4));
    icon(dl, at.x + S(15), at.y + h * 0.5f, kIconSearch, P::dim, 12.0f);
    ImGui::SetCursorScreenPos(ImVec2(at.x + S(28), at.y));
    ImGui::SetNextItemWidth(w - S(32));
    for (const ImGuiCol frame : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive})
        ImGui::PushStyleColor(frame, vec4(P::search, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), std::floor((h - ImGui::GetFontSize()) * 0.5f)));
    ImGui::InputTextWithHint("##stats_search", "Search: an addon, a texture, a pass, a function...", buffer, size);
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    dl->AddRect(at, end, u32(active ? P::accent_lo : hovered ? mix(P::search_edge, P::accent_lo, 0.5f) : P::search_edge), S(4));
}

} // namespace

void install(const wchar_t* install_dir)
{
    g_logs = install_dir != nullptr ? install_dir : L"";
    if (!g_logs.empty() && g_logs.back() != L'\\')
        g_logs += L'\\';
    sampler::install(g_logs.c_str());
    g_logs += L"logs\\";
    QueryPerformanceFrequency(&g_frequency);
}

void add(Section section, double ms)
{
    g_sections[section] += ms;
}

void add_addon(std::string_view name, double ms)
{
    g_addons[std::string(name)] += ms;
}

void add_addon_event(const std::string& addon, std::string_view event, double ms)
{
    auto& total = g_events[addon][std::string(event)];
    total.first += ms;
    ++total.second;
}

void add_plugin_event(const std::string& plugin, std::string_view event, double ms)
{
    Calls& calls = g_plugin_events[plugin];
    auto it = calls.find(event);
    if (it == calls.end())
        it = calls.emplace(std::string(event), std::pair<double, unsigned>{}).first;
    it->second.first += ms;
    ++it->second.second;
    if (g_running[kPlugins] > 0)
        g_plugins[plugin] += ms;
}

void Scope::begin()
{
    ++g_running[section_];
    QueryPerformanceCounter(&start_);
}

void Scope::finish()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    --g_running[section_];
    add(section_, static_cast<double>(now.QuadPart - start_.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart));
}

PluginScope::PluginScope(const std::string& name, const char* event, bool game_thread)
    : name_(name), event_(event), on_(game_thread && collecting())
{
    if (!on_)
        return;
    outer_ = g_plugin_nested;
    g_plugin_nested = 0.0;
    QueryPerformanceCounter(&start_);
}

PluginScope::~PluginScope()
{
    if (!on_)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double ms = static_cast<double>(now.QuadPart - start_.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
    add_plugin_event(name_, event_, ms - g_plugin_nested);
    g_plugin_nested = outer_ + ms;
}

AddonScope::~AddonScope()
{
    if (!on_)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double ms = static_cast<double>(now.QuadPart - start_.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
    add(kAddons, ms);
    add_addon(name_, ms);
}

void on_frame()
{
    if (!g_open)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_last_frame.QuadPart != 0) {
        ++g_acc.frames;
        g_acc.frame_sum += static_cast<double>(now.QuadPart - g_last_frame.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
        g_spans.push_back({g_last_frame.QuadPart, now.QuadPart});
    }
    g_last_frame = now;
    // Record Stats Report: the sampler looks at the thread that starts it, which is this one. A recording starts a look of
    // its own (what was collected before it is dropped), and the press that stops it ends it with one, then writes it.
    if (g_sample && !sampler::running()) {
        g_sample = sampler::start();
        if (g_sample) {
            g_recording = {};
            g_recording.have = true;
            g_recording.since = now;
            shadows_start();
            SYSTEMTIME time;
            GetLocalTime(&time);
            std::snprintf(g_recording.started, sizeof g_recording.started, "%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
            frametime::Method methods[128];
            frametime::take_methods(methods, 128);
            VanaGfxScopes scopes{};
            vanacore_gfx_scopes(&scopes);
            drop_window();
            g_window_start = now;
            return;
        }
    }
    const bool stopping = !g_sample && sampler::running();
    if (g_window_start.QuadPart == 0) {
        g_window_start = now;
        if (stopping)
            stop_recording();
        return;
    }
    const double seconds = static_cast<double>(now.QuadPart - g_window_start.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    if (seconds < window_seconds() && !stopping)
        return;
    if (g_acc.frames > 0)
        look(seconds, now);
    if (stopping)
        stop_recording();
    drop_window();
    g_window_start = now;
}

void draw()
{
    if (!g_open || !gui::in_frame())
        return;
    draw_addon_detail(g_frame_ms);
    draw_plugin_detail(g_frame_ms);
    // 1000 wide at the least (less on a screen too narrow for it): the window's size is kept in config\imgui.ini
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 620.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(1000.0f, ImGui::GetIO().DisplaySize.x * 0.9f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowPos(ImVec2(60.0f, 100.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    push_theme();
    const bool shown = begin_window("VanaCore stats");
    if (!shown) {
        end_window(false);
        pop_theme();
        return;
    }
    title_bar(kIconStats, "Stats", nullptr, open);

    // Two columns, a rule between them. The left (20%): Record Stats Report with Pause and 5s average, the frame's numbers,
    // the PC's and the card's readings. The right: the power and clocks in a line, the frame's parts as bars, the search
    // and the tree. Each scrolls on its own.
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float gap = S(24), left_w = std::floor((ImGui::GetContentRegionAvail().x - gap) * 0.20f);
    const float bottom = top.y + ImGui::GetContentRegionAvail().y;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(top.x + left_w + gap * 0.5f, top.y), ImVec2(top.x + left_w + gap * 0.5f, bottom), u32(P::line));
    const hwmon::Reading hw = hwmon::latest();
    ImGui::BeginChild("##stats_left", ImVec2(left_w, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
    controls();
    ImGui::Spacing();
    ImGui::Spacing();
    header();
    hardware_rows(hw);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("##stats_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);

    // amber while something holds the PC back
    note(hardware_line().c_str(), hw.throttled || !cpu_held(hw).empty() || (hw.power && !hw.ac) ? P::amber : P::dim);

    if (g_have) {
        ImGui::Spacing();
        stacked_bar("cpu", "CPU", g_cpu, g_frame_ms);
        if (g_gpu_seen)
            stacked_bar("gpu", "GPU", g_gpu, std::max(g_gpu_ms, 0.001));
    }
    ImGui::Spacing();
    {
        static char buffer[64] = "";
        search_box(buffer, sizeof buffer);
        g_filter = buffer;
        for (char& c : g_filter)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (g_have) {
        ImGui::Spacing();

        const auto table = [](const char* id, const auto& rows) {
            if (ImGui::BeginTable(id, 4,ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, S(66));
                ImGui::TableSetupColumn("mem (MB)",ImGuiTableColumnFlags_WidthFixed, S(76));
                ImGui::TableSetupColumn("cpu", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                headers_row();
                rows();
                ImGui::EndTable();
            }
        };
        // the CPU: the frame as one line, its parts under it; then the GPU and the counters
        const auto frame_rows = [&] {
            row(g_cpu, g_frame_ms, 0);
            if (g_gpu_seen)
                row(g_gpu, g_frame_ms, 1);
            if (!g_counters.children.empty())
                row(g_counters, g_frame_ms, 2);
        };
        if (!g_sample && !g_recording.have) {
            table("frame", frame_rows);
        } else if (ImGui::BeginTabBar("stats_tabs")) {
            if (ImGui::BeginTabItem("Frame (CPU and GPU)")) {
                table("frame", frame_rows);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Game code (sampled)")) {
                if (!g_sampled_seen) {
                    ImGui::TextWrapped(g_hold ? "Paused: turn Pause off, and the samples come in at the next look."
                                              : "Sampling... the first results come in at the next look.");
                } else {
                    const std::string length = minutes(recording_seconds(g_recording));
                    ImGui::TextDisabled("%s%s", g_sampled.name.c_str(), g_sampled.note.empty() ? "" : (" - " + g_sampled.note).c_str());
                    if (sampler::running())
                        ImGui::TextDisabled("Recording for %s, since %s: all of it so far", length.c_str(), g_recording.started);
                    else
                        ImGui::TextDisabled("The recording of %s from %s, stopped (%s); Record Stats Report starts another", length.c_str(),
                                            g_recording.started, (g_saved.empty() ? std::string("not saved") : "saved to " + g_saved).c_str());
                    timeline_plot(g_recording.timeline);
                    table("sampled", [&] {
                        int i = 0;
                        for (const Node& group : g_sampled.children) {
                            const int index = i++;
                            row(group, g_sampled.ms, index, index == 1 ? 0.08 : 0.0); // the call tree opens down the functions that matter
                        }
                        if (!g_slow.children.empty())
                            row(g_slow, g_slow.ms, i, 0.08);
                    });
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();

    end_window(true);
    pop_theme();
    if (!open)
        set_open(false);
}

bool command(const std::vector<std::string>& parts)
{
    const std::string option = parts.size() > 1 ? parts[1] : "";
    if (option == "save") {
        save_report();
        return true;
    }
    if (option != "" && option != "on" && option != "off") {
        chat::echo("/vana stats [on|off|save]: where a frame's time goes, as a tree (CPU, Direct3D calls, VanaCore, addons, GPU)");
        return true;
    }
    set_open(option == "on" ? true : option == "off" ? false : !g_open);
    return true;
}

} // namespace vanacore::stats
