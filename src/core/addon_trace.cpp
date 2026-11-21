#include "addon_trace.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <iterator>
#include <set>
#include <tuple>
#include <string>
#include <vector>

#include <vanacore/version.hpp>

#include "addons.hpp"
#include "chat.hpp"
#include "frametime.hpp"
#include "hwmon.hpp"
#include "ini.hpp"
#include "log.hpp"

namespace vanacore::addon_trace {

namespace {

constexpr ULONGLONG kHeartbeatMs = 10000;
constexpr ULONGLONG kGeometryMs = 2000; // a window's move or resize is noted at most this often
constexpr ULONGLONG kStaleMs = 5000;    // an addon that hasn't called Begin for a window this long has stopped drawing it

struct Seen {
    Window state;
    bool seen = false;
    bool drawn = false; // called Begin recently
    ULONGLONG drawn_at = 0;
    ULONGLONG geometry_at = 0;
    float last_x = 0, last_y = 0, last_w = 0, last_h = 0;
    int vertices = -1; // at its last End
    int indices = 0;
    float content_w = 0, content_h = 0;
    bool drew_nothing_noted = false;
};

struct Stats {
    std::map<std::string, int> events;
    int begins = 0;
    std::map<std::string, Seen> windows;
    int in_delivered = 0, out_delivered = 0;
    std::map<std::uint16_t, int> blocked_in, blocked_out;
    int added_out = 0, added_in = 0;
    int chat = 0, commands = 0;
    std::map<std::pair<bool, std::uint16_t>, int> blocked_count; // per direction and id, for thinning the log
    bool not_loaded_noted = false;
    bool was_loaded = false;
};

std::mutex g_lock;
std::atomic<bool> g_any{false}; // g_traced isn't empty: what every call checks first, as nearly all of them come while nothing is traced
std::set<std::string> g_traced;
std::map<std::string, Stats> g_stats;
std::wstring g_ini;
ULONGLONG g_next_heartbeat = 0;

// ---- a timed trace (/vana trace <addon> <seconds>): what it gathers, under g_lock --------------------------------------

constexpr int kMostSeconds = 600;
constexpr size_t kSlowestCalls = 15;

// One of the timed addon's slowest calls
struct Call {
    double at = 0; // seconds into the trace
    std::string event;
    double ms = 0;
    double kb = 0; // its Lua memory's change over the call; below 0, its garbage collector ran in it
};

struct EventTime {
    int calls = 0;
    double ms = 0, most = 0;
    int over_1ms = 0, over_5ms = 0;
};

// A frame of the game while the trace ran: its parts (frametime.hpp), and the timed addon's time in it
struct Frame {
    float at = 0;
    float frame = 0, direct3d = 0, sleeps = 0, vanacore = 0, limit = 0, present = 0, slowest = 0;
    const char* slowest_name = "";
    float addon = 0, top = 0; // the addon's time in it, and its slowest call's
    int top_event = -1;       // that call's event (Session::names)
};

// An addon's time while the trace ran (every addon's, for the timed one's share)
struct Share {
    int calls = 0;
    double ms = 0, most_call = 0, most_frame = 0;
    double now = 0; // in this frame so far
};

struct Session {
    bool on = false;
    std::string addon;
    bool added = false; // it wasn't traced before: it isn't after
    int seconds = 0;
    LARGE_INTEGER since{};
    ULONGLONG until = 0;
    std::string started; // "2026-11-10 14:23:00"
    std::map<std::string, EventTime, std::less<>> events;
    std::vector<std::string> names; // the events Frame::top_event indexes
    std::vector<Call> slowest;      // slowest first
    std::vector<Frame> frames;
    bool first_frame = true; // the first frame's parts began before the trace (long before, when the timing was off)
    std::map<std::string, Share> shares;
    double top = 0; // the timed addon's slowest call in this frame so far, and its event
    int top_event = -1;
    // its Lua memory, KB
    bool memory_seen = false;
    double kb_start = 0, kb_end = 0, kb_peak = 0, kb_made = 0, kb_freed = 0;
    int gc_calls = 0;
    // what it did: the trace's counts, added up before each heartbeat clears them
    int packets_in = 0, packets_out = 0, added_in = 0, added_out = 0, chat = 0, commands = 0, begins = 0;
    std::map<std::uint16_t, int> blocked_in, blocked_out;
    std::vector<std::pair<std::string, int>> windows; // its ImGui windows at the end, and the vertices each last drew
    bool profiling = false;
    std::string profile_error;
};

Session g_session;
std::atomic<bool> g_timing{false}; // g_session.on, read without the lock
std::wstring g_logs;               // <VanaCore>\logs\, where the report goes

std::string normal(std::string_view name)
{
    if (name.starts_with("addon "))
        name.remove_prefix(6);
    std::string out(name);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> split_names(const std::string& list)
{
    std::vector<std::string> out;
    std::string current;
    for (const char c : list + ",") {
        if (c == ',' || c == ' ' || c == ';' || c == '\t') {
            if (!current.empty())
                out.push_back(normal(current));
            current.clear();
        } else {
            current += c;
        }
    }
    return out;
}

std::string joined()
{
    std::string out;
    for (const std::string& name : g_traced)
        out += (out.empty() ? "" : ",") + name;
    return out;
}

void write_line(const std::string& addon, const std::string& text)
{
    log::write(L"Trace %hs: %hs", addon.c_str(), text.c_str());
}

std::string format(const char* text, va_list args)
{
    char buffer[2048];
    const int length = vsnprintf(buffer, sizeof(buffer), text, args);
    if (length < 0)
        return text;
    return std::string(buffer, static_cast<size_t>(length) < sizeof(buffer) ? static_cast<size_t>(length) : sizeof(buffer) - 1);
}

// Under g_lock: a line for a traced addon.
void line(const std::string& addon, const char* text, ...)
{
    va_list args;
    va_start(args, text);
    write_line(addon, format(text, args));
    va_end(args);
}

std::string printable(std::string_view text, size_t most = 160)
{
    std::string out;
    for (const char c : text) {
        if (out.size() >= most) {
            out += "...";
            break;
        }
        out += (static_cast<unsigned char>(c) < 0x20 && c != '\t') ? '?' : c;
    }
    return out;
}

std::string describe(const Window& w)
{
    char text[256];
    snprintf(text, sizeof(text), "shown=%d open=%s collapsed=%d pos=(%.0f,%.0f) size=(%.0fx%.0f) screen=%.0fx%.0f%s", w.shown ? 1 : 0,
             !w.has_open ? "n/a" : w.open_after ? "1" : "0", w.collapsed ? 1 : 0, w.x, w.y, w.w, w.h, w.screen_w, w.screen_h,
             (w.x + w.w <= 0 || w.y + w.h <= 0 || w.x >= w.screen_w || w.y >= w.screen_h) ? " OFF-SCREEN" : "");
    return text;
}

void save_list()
{
    if (g_ini.empty())
        return;
    std::string list;
    for (const std::string& name : g_traced)
        if (!(g_session.on && g_session.added && name == g_session.addon)) // a timed trace's addon isn't kept
            list += (list.empty() ? "" : ",") + name;
    ini::set(g_ini, L"logging", L"trace.addons", std::wstring(list.begin(), list.end()).c_str());
}

// The status line, built by the caller while it holds g_lock. It is echoed only after the lock is released:
// chat::echo runs the line back through the game's add-line path and the text handler, which calls on() below,
// which locks g_lock again. Echoing under the lock would re-lock it on this thread (a deadlock or, with our
// logging IM_ASSERT, a thrown std::system_error swallowed by the chat handler).
std::string status_text()
{
    if (g_traced.empty())
        return "addon trace: off";
    std::string timed;
    if (g_session.on) {
        const ULONGLONG now = GetTickCount64();
        timed = "; timing " + g_session.addon + ", " + std::to_string(now < g_session.until ? (g_session.until - now + 999) / 1000 : 0) +
                " s left";
    }
    return "addon trace: " + joined() + timed + (log::enabled() ? "" : " (logging is off: /vana logs on)");
}

// A trace's counts, cleared after each heartbeat
void clear_counts(Stats& stats)
{
    stats.events.clear();
    stats.begins = 0;
    stats.in_delivered = stats.out_delivered = 0;
    stats.blocked_in.clear();
    stats.blocked_out.clear();
    stats.added_in = stats.added_out = 0;
    stats.chat = stats.commands = 0;
}

// The timed addon's counts into the timed trace's, before they're cleared
void fold(const Stats& stats)
{
    Session& s = g_session;
    s.packets_in += stats.in_delivered;
    s.packets_out += stats.out_delivered;
    s.added_in += stats.added_in;
    s.added_out += stats.added_out;
    s.chat += stats.chat;
    s.commands += stats.commands;
    s.begins += stats.begins;
    for (const auto& [id, count] : stats.blocked_in)
        s.blocked_in[id] += count;
    for (const auto& [id, count] : stats.blocked_out)
        s.blocked_out[id] += count;
}

double seconds_since(const LARGE_INTEGER& since)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart - since.QuadPart) / static_cast<double>(frequency.QuadPart);
}

int event_index(Session& s, std::string_view event)
{
    for (size_t i = 0; i < s.names.size(); ++i)
        if (s.names[i] == event)
            return static_cast<int>(i);
    s.names.emplace_back(event);
    return static_cast<int>(s.names.size()) - 1;
}

void keep_slowest(Session& s, Call call)
{
    const auto at = std::upper_bound(s.slowest.begin(), s.slowest.end(), call.ms, [](double ms, const Call& c) { return ms > c.ms; });
    s.slowest.insert(at, std::move(call));
    if (s.slowest.size() > kSlowestCalls)
        s.slowest.pop_back();
}

void memory_change(Session& s, double before, double after)
{
    if (!s.memory_seen) {
        s.memory_seen = true;
        s.kb_start = s.kb_peak = before;
    }
    s.kb_end = after;
    s.kb_peak = std::max({s.kb_peak, before, after});
    if (after >= before) {
        s.kb_made += after - before;
    } else {
        s.kb_freed += before - after;
        ++s.gc_calls;
    }
}

// Under g_lock, while the timed trace runs: a call into an addon's Lua
void timed_call(const std::string& name, std::string_view event, double ms, double kb_before, double kb_after)
{
    Session& s = g_session;
    Share& share = s.shares[name];
    ++share.calls;
    share.ms += ms;
    share.most_call = std::max(share.most_call, ms);
    share.now += ms;
    if (name != s.addon)
        return;
    auto found = s.events.find(event);
    if (found == s.events.end())
        found = s.events.emplace(std::string(event), EventTime{}).first;
    EventTime& e = found->second;
    ++e.calls;
    e.ms += ms;
    e.most = std::max(e.most, ms);
    e.over_1ms += ms > 1.0 ? 1 : 0;
    e.over_5ms += ms > 5.0 ? 1 : 0;
    if (ms > s.top) {
        s.top = ms;
        s.top_event = event_index(s, event);
    }
    memory_change(s, kb_before, kb_after);
    if (s.slowest.size() < kSlowestCalls || ms > s.slowest.back().ms)
        keep_slowest(s, {seconds_since(s.since), std::string(event), ms, kb_after - kb_before});
}

// frametime::watch's sink, on the render thread at the start of each Present: the frame that ended, while the timed trace runs
void on_parts(const frametime::Parts& p)
{
    const std::lock_guard lock(g_lock);
    Session& s = g_session;
    if (!s.on)
        return;
    if (!s.first_frame) {
        Frame f;
        f.at = static_cast<float>(seconds_since(s.since));
        f.frame = static_cast<float>(p.frame), f.direct3d = static_cast<float>(p.direct3d), f.sleeps = static_cast<float>(p.sleeps);
        f.vanacore = static_cast<float>(p.vanacore), f.limit = static_cast<float>(p.limit), f.present = static_cast<float>(p.present);
        f.slowest = static_cast<float>(p.slowest), f.slowest_name = p.slowest_name != nullptr ? p.slowest_name : "";
        const auto timed = s.shares.find(s.addon);
        f.addon = timed != s.shares.end() ? static_cast<float>(timed->second.now) : 0.0f;
        f.top = static_cast<float>(s.top), f.top_event = s.top_event;
        s.frames.push_back(f);
        for (auto& [name, share] : s.shares)
            share.most_frame = std::max(share.most_frame, share.now);
    }
    s.first_frame = false;
    for (auto& [name, share] : s.shares)
        share.now = 0;
    s.top = 0;
    s.top_event = -1;
}

// "30" or "30s": a timed trace's seconds
bool seconds_word(const std::string& word, int& seconds)
{
    std::string digits = word;
    if (!digits.empty() && digits.back() == 's')
        digits.pop_back();
    if (digits.empty() || digits.size() > 6 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    seconds = std::atoi(digits.c_str());
    return true;
}

#include "addon_trace_report.hpp" // the timed trace's report: report_text, summary_text

// /vana trace <addon> <seconds>
void start_timed(const std::vector<std::string>& names, int seconds)
{
    std::string refused;
    if (names.size() != 1 || names[0] == "off" || names[0] == "list" || names[0] == "+" || seconds <= 0) {
        refused = "addon trace: a timed trace is of one addon, for 1 to 600 seconds: /vana trace <addon> <seconds>";
    } else {
        const std::lock_guard lock(g_lock);
        if (g_session.on)
            refused = "addon trace: timing " + g_session.addon + " already (/vana trace list says how long it has left; /vana trace off ends it)";
    }
    if (!refused.empty()) {
        chat::echo(refused);
        return;
    }
    std::string addon = names[0];
    if (addon[0] == '+')
        addon.erase(0, 1);
    seconds = std::min(seconds, kMostSeconds);
    {
        const std::lock_guard lock(g_lock);
        g_session = Session{};
        g_session.addon = addon;
        g_session.seconds = seconds;
        g_session.added = g_traced.insert(addon).second;
        g_any = true;
        clear_counts(g_stats[addon]);
    }
    // Before it's on: the profile's start runs in the addon's Lua, and isn't one of its calls
    std::string error;
    const bool profiling = addons::profile_start(addon, error);
    hwmon::acquire();
    frametime::watch(&on_parts);
    {
        const std::lock_guard lock(g_lock);
        g_session.profiling = profiling;
        g_session.profile_error = error;
        SYSTEMTIME time;
        GetLocalTime(&time);
        char started[32];
        snprintf(started, sizeof(started), "%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                 time.wSecond);
        g_session.started = started;
        QueryPerformanceCounter(&g_session.since);
        g_session.until = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
        g_session.on = true;
        g_timing = true;
    }
    log::write(L"Addon trace: timing %hs for %d s%hs%hs", addon.c_str(), seconds, profiling ? "" : " (no Lua profile: ", profiling ? "" : (error + ")").c_str());
    chat::echo("addon trace: timing " + addon + " for " + std::to_string(seconds) +
               " s (its calls, memory and Lua profile, and the game's frames); the report goes in logs\\ at the end");
}

// The timed trace's end: its time is up, or /vana trace off. Not under g_lock: the profile's stop runs in the addon's Lua.
void finish_timed(const char* why)
{
    Session s;
    {
        const std::lock_guard lock(g_lock);
        if (!g_session.on)
            return;
        g_session.on = false;
        g_timing = false;
        const auto stats = g_stats.find(g_session.addon);
        if (stats != g_stats.end()) {
            fold(stats->second);
            clear_counts(stats->second);
            for (const auto& [title, seen] : stats->second.windows)
                g_session.windows.emplace_back(title, seen.vertices);
        }
        if (g_session.added) {
            g_traced.erase(g_session.addon);
            g_stats.erase(g_session.addon);
        }
        g_any = !g_traced.empty();
        s = std::move(g_session);
        g_session = Session{};
    }
    frametime::unwatch(&on_parts);
    const double length = seconds_since(s.since);
    std::string profile, error;
    if (s.profiling && !addons::profile_stop(s.addon, profile, error))
        s.profile_error = error;
    const hwmon::Reading hardware = hwmon::latest();
    hwmon::release();
    const std::string text = report_text(s, length, why, profile, hardware);
    std::string name = "trace-" + s.addon + "-";
    for (const wchar_t c : log::file_stamp())
        name += static_cast<char>(c);
    name += ".txt";
    CreateDirectoryW(g_logs.c_str(), nullptr);
    FILE* f = nullptr;
    std::string where;
    if (!g_logs.empty() && _wfopen_s(&f, (g_logs + std::wstring(name.begin(), name.end())).c_str(), L"wb") == 0 && f != nullptr) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        where = "logs\\" + name;
    }
    for (const std::string& line : summary_text(s, length, why, where)) {
        log::write(L"Addon trace: %hs", line.c_str());
        chat::echo(line);
    }
}

} // namespace

void install(const wchar_t* install_dir, const wchar_t* settings_ini)
{
    g_logs = install_dir != nullptr ? std::wstring(install_dir) + L"\\logs\\" : L"";
    g_ini = settings_ini != nullptr ? settings_ini : L"";
    if (g_ini.empty())
        return;
    wchar_t names[512] = {};
    GetPrivateProfileStringW(L"logging", L"trace.addons", L"", names, 512, g_ini.c_str());
    std::string text;
    for (const wchar_t* c = names; *c != L'\0'; ++c)
        text += *c < 0x80 ? static_cast<char>(*c) : '?';
    const std::lock_guard lock(g_lock);
    for (const std::string& name : split_names(text))
        g_traced.insert(name);
    g_any = !g_traced.empty();
    if (!g_traced.empty())
        log::write(L"Addon trace: %hs ([logging] trace.addons)", joined().c_str());
}

void command(const std::vector<std::string>& words)
{
    std::vector<std::string> names;
    for (const std::string& word : words)
        for (const std::string& name : split_names(word))
            names.push_back(name);
    // "/vana trace <addon> 30": a timed trace
    int seconds = 0;
    if (names.size() >= 2 && seconds_word(names.back(), seconds)) {
        names.pop_back();
        start_timed(names, seconds);
        return;
    }
    const bool off = !names.empty() && (names[0] == "off" || names[0] == "none" || names[0] == "0");
    if (off)
        finish_timed("stopped by /vana trace off"); // its report, with what it has so far
    std::string message;
    {
        const std::lock_guard lock(g_lock);
        if (!(names.empty() || names[0] == "list")) {
            if (off) {
                g_traced.clear();
                g_stats.clear();
            } else {
                // "/vana trace +name" adds one; otherwise the names given are the whole list
                const bool add = words[0].size() > 1 && words[0][0] == '+';
                if (!add)
                    g_traced.clear();
                for (std::string name : names) {
                    if (!name.empty() && name[0] == '+')
                        name.erase(0, 1);
                    if (!name.empty())
                        g_traced.insert(name);
                }
                // a timed trace's addon stays traced to its end (and isn't after, if the new list hasn't it)
                if (g_session.on && g_traced.insert(g_session.addon).second)
                    g_session.added = true;
                for (auto it = g_stats.begin(); it != g_stats.end();)
                    it = g_traced.count(it->first) != 0 ? std::next(it) : g_stats.erase(it);
            }
            g_any = !g_traced.empty();
            g_next_heartbeat = 0;
            save_list();
            log::write(L"Addon trace: %hs", g_traced.empty() ? "off" : joined().c_str());
        }
        message = status_text();
    }
    chat::echo(message);
}

bool on(std::string_view addon)
{
    if (!g_any.load(std::memory_order_relaxed))
        return false;
    const std::lock_guard lock(g_lock);
    return !g_traced.empty() && g_traced.count(normal(addon)) != 0;
}

bool any()
{
    return g_any.load(std::memory_order_relaxed);
}

bool timing()
{
    return g_timing.load(std::memory_order_relaxed);
}

void say(std::string_view addon, const char* text, ...)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    {
        const std::lock_guard lock(g_lock);
        if (g_traced.count(name) == 0)
            return;
    }
    va_list args;
    va_start(args, text);
    write_line(name, format(text, args));
    va_end(args);
}

void loaded(std::string_view addon, const std::string& listens)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    Stats& stats = g_stats[name];
    stats.was_loaded = true;
    stats.not_loaded_noted = false;
    line(name, "loaded; listens for: %s", listens.empty() ? "(nothing)" : listens.c_str());
}

void unloaded(std::string_view addon, const std::string& reason)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    g_stats[name].was_loaded = false;
    line(name, "unloaded%s%s", reason.empty() ? "" : ": ", reason.c_str());
}

void event(std::string_view addon, std::string_view event, double ms, double kb_before, double kb_after)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_session.on)
        timed_call(name, event, ms, kb_before, kb_after);
    if (g_traced.count(name) == 0)
        return;
    ++g_stats[name].events[std::string(event)];
}

void command_line(const std::string& text, int mode, bool injected, std::string_view handled_by, bool after_addons)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(g_lock);
    if (g_traced.empty())
        return;
    const std::string who = std::string(handled_by);
    const std::string what = who.empty()  ? "offered to the addons"
                             : after_addons ? "no addon took it, then " + who + " handled it"
                                            : "handled by " + who + ", it never reached the addons";
    for (const std::string& name : g_traced)
        line(name, "command \"%s\" (mode %d%s): %s", printable(text).c_str(), mode, injected ? ", queued" : "", what.c_str());
}

void command_reached(std::string_view addon, const std::string& text, bool took_it, const std::string& error)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    ++g_stats[name].commands;
    line(name, "command \"%s\" reached it: %s%s%s", printable(text).c_str(), took_it ? "it took it" : "it left it",
         error.empty() ? "" : ", ERROR: ", printable(error, 400).c_str());
}

void window_begin(std::string_view addon, const char* window_name, const Window& state)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    Stats& stats = g_stats[name];
    ++stats.begins;
    Seen& seen = stats.windows[window_name != nullptr ? window_name : ""];
    const ULONGLONG now = GetTickCount64();
    const std::string title = std::string("window \"") + (window_name != nullptr ? window_name : "") + "\"";
    const auto key = [](const Window& w) {
        return std::make_tuple(w.has_open, w.open_before, w.open_after, w.shown, w.collapsed,
                               w.x + w.w <= 0 || w.y + w.h <= 0 || w.x >= w.screen_w || w.y >= w.screen_h);
    };
    const auto geometry = [&] {
        seen.geometry_at = now;
        seen.last_x = state.x;
        seen.last_y = state.y;
        seen.last_w = state.w;
        seen.last_h = state.h;
    };
    if (!seen.seen) {
        line(name, "%s first drawn: %s, flags 0x%X%s", title.c_str(), describe(state).c_str(), state.flags,
             state.has_open && !state.open_before ? " (it passed p_open false: ImGui closes it)" : "");
        geometry();
    } else if (!seen.drawn) {
        line(name, "%s drawn again: %s", title.c_str(), describe(state).c_str());
        geometry();
    } else if (key(state) != key(seen.state)) {
        line(name, "%s changed: %s%s", title.c_str(), describe(state).c_str(),
             state.has_open && seen.state.has_open && seen.state.open_after && !state.open_after ? " (closed)" : "");
        geometry();
    } else if (now - seen.geometry_at >= kGeometryMs &&
               (std::fabs(state.x - seen.last_x) >= 1 || std::fabs(state.y - seen.last_y) >= 1 ||
                std::fabs(state.w - seen.last_w) >= 1 || std::fabs(state.h - seen.last_h) >= 1)) {
        line(name, "%s moved or resized: pos=(%.0f,%.0f) size=(%.0fx%.0f)", title.c_str(), state.x, state.y, state.w, state.h);
        geometry();
    }
    seen.state = state;
    seen.seen = true;
    seen.drawn = true;
    seen.drawn_at = now;
}

void window_end(std::string_view addon, const char* window_name, int vertices, int indices, float content_w, float content_h)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    Seen& seen = g_stats[name].windows[window_name != nullptr ? window_name : ""];
    seen.vertices = vertices;
    seen.indices = indices;
    seen.content_w = content_w;
    seen.content_h = content_h;
    // Said once when it first has nothing in it, and once when that changes
    const bool empty = vertices <= 0 && seen.state.shown;
    if (empty != seen.drew_nothing_noted) {
        seen.drew_nothing_noted = empty;
        line(name, "window \"%s\" %s", window_name != nullptr ? window_name : "",
             empty ? "is shown but drew nothing inside (no vertices)" : "has content again");
    }
}

void packet_delivered(std::string_view addon, bool incoming, std::uint16_t id, std::uint32_t size, bool blocked)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    Stats& stats = g_stats[name];
    ++(incoming ? stats.in_delivered : stats.out_delivered);
    if (!blocked)
        return;
    ++(incoming ? stats.blocked_in : stats.blocked_out)[id];
    const int count = ++stats.blocked_count[{incoming, id}];
    if (count <= 3 || count % 100 == 0)
        line(name, "blocked %s packet 0x%03X (%u bytes), %d so far", incoming ? "received" : "sent", id, size, count);
}

void packet_added(std::string_view from, bool incoming, std::uint16_t id, const std::uint8_t* data, std::uint32_t size)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(from);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    Stats& stats = g_stats[name];
    ++(incoming ? stats.added_in : stats.added_out);
    std::string bytes;
    char one[8];
    for (std::uint32_t i = 4; i < size && i < 16; ++i) {
        snprintf(one, sizeof(one), "%02X ", data[i]);
        bytes += one;
    }
    line(name, "added %s packet 0x%03X (%u bytes), first bytes after the header: %s", incoming ? "received" : "sent", id, size,
         bytes.c_str());
}

void packet_result(std::string_view from, bool incoming, std::uint16_t id, const char* what)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(from);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    line(name, "its %s packet 0x%03X: %s", incoming ? "received" : "sent", id, what);
}

void chat_line(std::string_view addon, int mode, std::string_view text)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    ++g_stats[name].chat;
    line(name, "chat line (mode %d): %s", mode, printable(text).c_str());
}

void queued_command(std::string_view addon, int mode, std::string_view text, bool queued)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    const std::string name = normal(addon);
    const std::lock_guard lock(g_lock);
    if (g_traced.count(name) == 0)
        return;
    line(name, "%s command (mode %d): %s", queued ? "queued" : "ran", mode, printable(text).c_str());
}

// tick() under g_lock
static void tick_locked(const std::vector<std::string>& loaded_addons)
{
    if (g_traced.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    // A window its addon has stopped drawing
    for (auto& [name, stats] : g_stats)
        for (auto& [title, seen] : stats.windows)
            if (seen.drawn && now - seen.drawn_at >= kStaleMs) {
                seen.drawn = false;
                line(name, "window \"%s\" is no longer drawn (no Begin for %llu s); last: %s", title.c_str(),
                     (now - seen.drawn_at) / 1000, describe(seen.state).c_str());
            }
    if (now < g_next_heartbeat)
        return;
    g_next_heartbeat = now + kHeartbeatMs;
    for (const std::string& name : g_traced) {
        const bool is_loaded = std::find(loaded_addons.begin(), loaded_addons.end(), name) != loaded_addons.end();
        Stats& stats = g_stats[name];
        if (!is_loaded) {
            if (!stats.not_loaded_noted) {
                stats.not_loaded_noted = true;
                line(name, "is not loaded (loaded now: %zu addons)", loaded_addons.size());
            }
            continue;
        }
        stats.was_loaded = true;
        std::string events;
        for (const auto& [event_name, count] : stats.events)
            events += (events.empty() ? "" : ", ") + event_name + " " + std::to_string(count);
        std::string windows;
        for (const auto& [title, seen] : stats.windows) {
            char text[320];
            snprintf(text, sizeof(text), "%s\"%s\" %s %s, content %.0fx%.0f, %d vertices", windows.empty() ? "" : "; ", title.c_str(),
                     seen.drawn ? "drawn" : "NOT drawn", describe(seen.state).c_str(), seen.content_w, seen.content_h,
                     seen.vertices < 0 ? 0 : seen.vertices);
            windows += text;
        }
        std::string blocked;
        for (const auto& [id, count] : stats.blocked_in) {
            char text[48];
            snprintf(text, sizeof(text), "%s0x%03X x%d", blocked.empty() ? "" : ", ", id, count);
            blocked += text;
        }
        line(name, "last %llu s: events [%s]; Begin calls %d; windows [%s]; packets in %d (blocked: %s), out %d; added in %d / out %d; "
                   "chat lines %d; commands %d",
             kHeartbeatMs / 1000, events.empty() ? "none" : events.c_str(), stats.begins, windows.empty() ? "none yet" : windows.c_str(),
             stats.in_delivered, blocked.empty() ? "none" : blocked.c_str(), stats.out_delivered, stats.added_in, stats.added_out,
             stats.chat, stats.commands);
        if (g_session.on && name == g_session.addon)
            fold(stats);
        clear_counts(stats);
    }
}

void tick(const std::vector<std::string>& loaded_addons)
{
    if (!g_any.load(std::memory_order_relaxed))
        return;
    bool due = false;
    {
        const std::lock_guard lock(g_lock);
        due = g_session.on && GetTickCount64() >= g_session.until;
        tick_locked(loaded_addons);
    }
    if (due)
        finish_timed("");
}

} // namespace vanacore::addon_trace
