#include "zoneload.hpp"

#include <windows.h>

#include <intrin.h>
#include <lua.hpp>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

#include "imports.hpp"
#include "log.hpp"
#include "lua_fields.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase; // VanaCore.Core.dll: its own opens aren't the game's

namespace vanacore::zoneload {
namespace {

using lua_fields::set_bool;
using lua_fields::set_number;
using lua_fields::set_string;

// The phases' ends, in ms. Each has a longest, so a load the rules misread never holds the screen.
constexpr double kZoneQuiet = 500;       // after the zone-in, no data file on the render thread: the zone is in
constexpr double kZoneLongest = 15000;   // after the zone-in: the zone is taken as in whatever the files say
constexpr double kPeopleQuiet = 2000;    // after the people's first update (0x00E), no data file on another thread
constexpr double kPeopleLongest = 25000; // after the zone is in: the people are taken as in
constexpr double kServerLongest = 60000; // after a zone-out with no zone-in (a logout): forgotten
constexpr double kLogAfterZoneIn = 15000; // of the new zone the timeline log keeps
constexpr double kGap = 40;               // between two frames that gets a line in the log
constexpr double kWaitUsual = 900;        // a zone-out to its zone-in, until one is timed (measured: 870 to 895 ms)

enum class Phase { None, Server, Zone, People };
// While the game waits for the server (measured): the zone-out (0x00B) comes; 150 ms on the game says it's leaving
// (0x00D out); 600 ms on it asks the new zone in (0x00A out); 130 ms on (a round trip) the server answers with it all
enum class Step { Leaving, Connecting, Waiting };

// What one zone needed last time (cache\zoneload.txt): the data files read for it on the render thread and on others
struct Expected {
    unsigned zone = 0, people = 0;
};

std::mutex g_lock; // all below; files are opened on any thread
thread_local bool t_held = false; // this thread holds g_lock: a file it opens meanwhile (the cache, the log) is ours

// g_lock taken, and the hooks told so: they would wait for it on this thread for ever (and std::mutex throws)
struct Held {
    Held() { g_lock.lock(); t_held = true; }
    ~Held() { t_held = false; g_lock.unlock(); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
};

LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_start{};
std::atomic<bool> g_tracking{false}; // from a zone-out (or the login's zone-in) until its people are in
std::atomic<bool> g_broken{false};   // something here threw: it stays out of the way until the game restarts
std::wstring g_logs;                 // the logs folder, with its closing backslash
std::wstring g_cache;                // cache\zoneload.txt
std::wstring g_ini;                  // the settings (the session's copy)
std::atomic<bool> g_log_on{false};   // [logging] zoneload, read again as each load starts (/vana > Developer)
std::map<unsigned, Expected> g_expected;
std::atomic<DWORD> g_render_thread{0};
double g_wait = kWaitUsual; // ms from a zone-out to its zone-in, the last ones' (eased)

struct Load {
    Phase phase = Phase::None;
    bool login = false; // the first zone after picking a character: no zone-out came before it
    unsigned zone = 0, previous = 0;
    double zone_in_at = -1, last_zone_file = -1, ready_at = -1, people_at = -1, last_people_file = -1;
    unsigned zone_files = 0, people_files = 0;
    bool zone_quiet = false; // the zone phase ended by its files stopping, not by its longest
    Step step = Step::Leaving;
    double asked_at = -1, answer = -1; // the game's 0x00A out; the server's answer to it, ms after
    unsigned out_type = 0;             // the zone-out's type (byte 4): 2 a zone change
    Expected expected;
    std::string file; // the last data file, "ROM/1/41.DAT"
} g_load;

// The timeline log ([logging] zoneload=1)
log::Report g_report;
std::atomic<bool> g_logging{false}; // g_report is open
struct Timeline {
    double last_frame = -1, count_from = 0, longest_gap = 0;
    unsigned counted = 0, frames = 0, files = 0;
    long long bytes = 0;
    std::bitset<512> seen, sent;
} g_timeline;

// The game's last packets out, so a zone-out's log starts with what led to it ([logging] zoneload)
struct Sent {
    LARGE_INTEGER at{};
    std::uint16_t id = 0, length = 0;
    std::uint8_t head[32] = {};
};
Sent g_sent[8];
unsigned g_sent_next = 0;

// Something threw: said once, and everything here stops for this run (the game goes on as without it)
void broke(const char* where)
{
    if (!g_broken.exchange(true))
        log::write(L"Zone loading: an error in %hs; the loading screen is off until the game restarts", where);
    g_tracking = false;
}

double now_ms()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - g_start.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
}

// A line in the timeline log, under g_lock: the time from the load's start and the thread first
void note(const char* format, ...)
{
    if (!g_logging)
        return;
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    char head[48];
    snprintf(head, sizeof head, "%9.1f ms  thread %-6lu ", now_ms(), GetCurrentThreadId());
    g_report.write(std::string(head) + text);
}

void end_timeline(const char* why)
{
    if (!g_logging)
        return;
    note("end (%s): %u frames, the longest gap between two %.0f ms; %u files opened, %lld bytes", why,
         g_timeline.frames, g_timeline.longest_gap, g_timeline.files, g_timeline.bytes);
    g_report.close();
    g_logging = false;
}

std::string utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

// cache\zoneload.txt: "<zone> <zone files> <people files>" a line; anything else in it is skipped
void load_cache()
{
    std::ifstream in(g_cache);
    std::string line;
    while (std::getline(in, line)) {
        unsigned zone = 0, files = 0, people = 0;
        if (sscanf_s(line.c_str(), "%u %u %u", &zone, &files, &people) == 3 && zone < 0x10000 && files < 100000 &&
            people < 100000)
            g_expected[zone] = {files, people};
    }
}

void save_cache()
{
    std::ofstream out(g_cache, std::ios::trunc);
    for (const auto& [zone, expected] : g_expected)
        out << zone << ' ' << expected.zone << ' ' << expected.people << '\n';
}

// Under g_lock: a load starts (a zone-out, or the login's zone-in)
// [logging] zoneload, from the settings now: the /vana switch takes effect at the next load
bool log_wanted()
{
    return !g_ini.empty() && GetPrivateProfileIntW(L"logging", L"zoneload", 0, g_ini.c_str()) != 0;
}

void start(bool login)
{
    end_timeline("another load started");
    QueryPerformanceCounter(&g_start);
    const unsigned previous = login ? 0 : g_load.zone;
    g_load = Load{};
    g_load.phase = Phase::Server;
    g_load.login = login;
    g_load.previous = previous;
    g_timeline = Timeline{};
    g_log_on = log_wanted();
    if (g_log_on && !g_logs.empty())
        g_logging = g_report.open(g_logs, L"zoneload", log::file_stamp());
    g_tracking = true;
    note(login ? "login's zone-in: the timeline starts" : "zone-out (0x00B): the timeline starts");
}

// Under g_lock: the people are in too; what this zone needed is kept for the next time
void finish(const char* why)
{
    note("people in (%s): %u data files on the render thread, %u on others", why, g_load.zone_files,
         g_load.people_files);
    if (g_load.zone != 0 && g_load.zone_quiet && g_load.zone_files > 0 && g_load.people_at >= 0) {
        g_expected[g_load.zone] = {g_load.zone_files, g_load.people_files};
        if (!g_cache.empty())
            save_cache();
    }
    g_load.phase = Phase::None;
    g_tracking = false;
}

std::uint16_t word(const std::uint8_t* at)
{
    return static_cast<std::uint16_t>(at[0] | (at[1] << 8));
}

// "ROM/1/41.DAT" from a game data file's path (the game's own ROM folders or an overlay's rom), or empty if it isn't one
std::string data_file(const wchar_t* path)
{
    std::wstring lower(path);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return std::towlower(c); });
    std::replace(lower.begin(), lower.end(), L'\\', L'/');
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".dat") != 0)
        return {};
    const size_t rom = lower.rfind(L"/rom");
    if (rom == std::wstring::npos || lower.find(L"/user/") != std::wstring::npos)
        return {};
    std::string out = utf8(path + rom + 1);
    std::replace(out.begin(), out.end(), '\\', '/');
    for (size_t i = 0; i < 3 && i < out.size(); ++i)
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    return out;
}

// Under g_lock: a data file the game opened, counted toward the zone or its people
void count(const std::string& file)
{
    const double t = now_ms();
    g_load.file = file;
    if (g_load.phase == Phase::Zone && GetCurrentThreadId() == g_render_thread) {
        ++g_load.zone_files;
        g_load.last_zone_file = t;
    } else if (g_load.phase == Phase::Zone || g_load.phase == Phase::People) {
        ++g_load.people_files;
        g_load.last_people_file = t;
    }
}

void opened(const wchar_t* path, HANDLE handle, bool game)
{
    if (!g_tracking || path == nullptr || t_held)
        return;
    const Held held;
    if (!g_tracking)
        return;
    if (handle == INVALID_HANDLE_VALUE) {
        note("open failed: %s", utf8(path).c_str());
        return;
    }
    if (g_logging) {
        LARGE_INTEGER size{};
        GetFileSizeEx(handle, &size);
        ++g_timeline.files;
        g_timeline.bytes += size.QuadPart;
        note("open %s (%lld bytes)%s", utf8(path).c_str(), static_cast<long long>(size.QuadPart),
             game ? "" : " (VanaCore)");
    }
    if (game) {
        const std::string file = data_file(path);
        if (!file.empty())
            count(file);
    }
}

void opened(const char* path, HANDLE handle, bool game)
{
    if (!g_tracking || path == nullptr || t_held)
        return;
    wchar_t wide[MAX_PATH * 2] = {};
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, static_cast<int>(std::size(wide)) - 1) > 0)
        opened(wide, handle, game);
}

// --- The CreateFile hooks ------------------------------------------------------------------------------------------
// Each module's import is hooked on its own, and each keeps the function its import had before (pivot's, another
// plugin's, or Windows'), so a call goes on to what that module would have called without VanaCore's counting.

using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

struct Hooked {
    HMODULE module = nullptr;
    CreateFileA_t a = nullptr;
    CreateFileW_t w = nullptr;
};
constexpr int kMaxHooked = 256;
Hooked g_hooked[kMaxHooked];   // written under g_hook_lock, then published by g_hooked_count
std::atomic<int> g_hooked_count{0};
std::mutex g_hook_lock;
CreateFileA_t g_windows_a = nullptr; // KERNEL32's own, for a call from a module not in the list
CreateFileW_t g_windows_w = nullptr;

HMODULE module_of(const void* address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &module);
    return module;
}

const Hooked* hooked(HMODULE module)
{
    const int count = g_hooked_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        if (g_hooked[i].module == module)
            return &g_hooked[i];
    return nullptr;
}

HANDLE WINAPI create_file_a(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                            DWORD flags, HANDLE templ)
{
    const HMODULE caller = module_of(_ReturnAddress());
    const Hooked* entry = hooked(caller);
    const CreateFileA_t next = entry != nullptr && entry->a != nullptr ? entry->a : g_windows_a;
    HANDLE handle = next(path, access, share, security, disposition, flags, templ);
    if (g_tracking) {
        const DWORD error = GetLastError(); // the caller reads CreateFile's, not ours
        try {
            opened(path, handle, caller != reinterpret_cast<HMODULE>(&__ImageBase));
        } catch (...) {
            broke("CreateFileA");
        }
        SetLastError(error);
    }
    return handle;
}

HANDLE WINAPI create_file_w(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
                            DWORD flags, HANDLE templ)
{
    const HMODULE caller = module_of(_ReturnAddress());
    const Hooked* entry = hooked(caller);
    const CreateFileW_t next = entry != nullptr && entry->w != nullptr ? entry->w : g_windows_w;
    HANDLE handle = next(path, access, share, security, disposition, flags, templ);
    if (g_tracking) {
        const DWORD error = GetLastError();
        try {
            opened(path, handle, caller != reinterpret_cast<HMODULE>(&__ImageBase));
        } catch (...) {
            broke("CreateFileW");
        }
        SetLastError(error);
    }
    return handle;
}

// The modules pivot hooks: the game's and the other programs' DLLs, and the C runtime's (which the game's fopen goes
// through), not Windows' own
void on_module(HMODULE module)
{
    if (imports::is_windows_module(module) && GetModuleHandleW(L"ucrtbase.dll") != module &&
        GetModuleHandleW(L"msvcrt.dll") != module)
        return;
    const std::lock_guard lock(g_hook_lock);
    if (hooked(module) != nullptr)
        return; // (a module can be passed more than once: its imports are ours already)
    const int at = g_hooked_count.load(std::memory_order_relaxed);
    if (at >= kMaxHooked)
        return;
    Hooked entry;
    entry.module = module;
    for (const char* dll : {"KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll"}) {
        CreateFileA_t a = nullptr;
        CreateFileW_t w = nullptr;
        if (imports::hook(module, dll, "CreateFileA", &create_file_a, a) && a != nullptr && entry.a == nullptr)
            entry.a = a;
        if (imports::hook(module, dll, "CreateFileW", &create_file_w, w) && w != nullptr && entry.w == nullptr)
            entry.w = w;
    }
    if (entry.a == nullptr && entry.w == nullptr)
        return;
    g_hooked[at] = entry;
    g_hooked_count.store(at + 1, std::memory_order_release);
}

// --- Each frame -----------------------------------------------------------------------------------------------------

// Under g_lock: the timeline's frame lines
void time_frame(double t)
{
    Timeline& line = g_timeline;
    if (!g_logging)
        return;
    if (line.frames == 0)
        note("the first frame since the start (the render thread)");
    else if (t - line.last_frame >= kGap)
        note("frame after a gap of %.0f ms", t - line.last_frame);
    if (line.last_frame >= 0)
        line.longest_gap = std::max(line.longest_gap, t - line.last_frame);
    ++line.frames;
    ++line.counted;
    if (t - line.count_from >= 1000) {
        note("%u frames in the last %.0f ms", line.counted, t - line.count_from);
        line.count_from = t;
        line.counted = 0;
    }
    line.last_frame = t;
    if (g_load.zone_in_at >= 0 && t - g_load.zone_in_at >= kLogAfterZoneIn)
        end_timeline("15 s after the zone-in");
}

// Under g_lock: the load's phase moves on
void advance(double t)
{
    if (g_load.phase == Phase::Server && t >= kServerLongest) {
        note("no zone-in in a minute: forgotten (a logout)");
        end_timeline("no zone-in");
        g_load.phase = Phase::None;
        g_tracking = false;
        return;
    }
    if (g_load.phase == Phase::Zone) {
        const bool quiet = t - std::max(g_load.zone_in_at, g_load.last_zone_file) >= kZoneQuiet;
        if (quiet || t - g_load.zone_in_at >= kZoneLongest) {
            g_load.phase = Phase::People;
            g_load.ready_at = t;
            g_load.zone_quiet = quiet;
            note("zone in (%s): %u data files on the render thread", quiet ? "its files stopped" : "15 s",
                 g_load.zone_files);
        }
    }
    if (g_load.phase != Phase::People)
        return;
    if (g_load.people_at >= 0 && t - std::max(g_load.people_at, g_load.last_people_file) >= kPeopleQuiet)
        finish("their files stopped");
    else if (t - g_load.ready_at >= kPeopleLongest)
        finish("25 s");
}

const char* phase_name(Phase phase)
{
    switch (phase) {
    case Phase::Server: return "server";
    case Phase::Zone: return "zone";
    case Phase::People: return "people";
    default: return "none";
    }
}

const char* step_name(Step step)
{
    switch (step) {
    case Step::Connecting: return "connecting";
    case Step::Waiting: return "waiting";
    default: return "leaving";
    }
}

// vana.game.loading() -> nil when no zone is loading, else { phase = "server" | "zone" | "people", login, zone,
// previous, elapsed (s from the start), ready (the zone is in, its people may not be), zone_files, zone_expected,
// people_files, people_expected (0: not known, the zone not loaded since VanaCore had this), file ("ROM/1/41.DAT"),
// step (phase "server" after a zone-out: "leaving" | "connecting" | "waiting"), wait (s: how long the wait usually is),
// answer (ms the server took to answer the game's 0x00A, once it has; a round trip), out_type (the zone-out's type) }.
// The state is copied under the lock and the table made after it: a Lua error can't leave the lock held.
int lua_loading(lua_State* L)
{
    Load load;
    double elapsed = 0, wait = kWaitUsual;
    if (!g_broken) {
        try {
            const Held held;
            load = g_load;
            elapsed = now_ms() / 1000.0;
            wait = g_wait;
        } catch (...) {
            broke("vana.game.loading");
            load = Load{};
        }
    }
    if (load.phase == Phase::None) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 16);
    set_string(L, "phase", phase_name(load.phase));
    set_bool(L, "login", load.login);
    set_number(L, "zone", load.zone);
    set_number(L, "previous", load.previous);
    set_number(L, "elapsed", elapsed);
    set_bool(L, "ready", load.phase == Phase::People);
    set_number(L, "zone_files", load.zone_files);
    set_number(L, "zone_expected", load.expected.zone);
    set_number(L, "people_files", load.people_files);
    set_number(L, "people_expected", load.expected.people);
    set_string(L, "file", load.file);
    if (load.phase == Phase::Server && !load.login)
        set_string(L, "step", step_name(load.step));
    set_number(L, "wait", wait / 1000.0);
    if (load.answer >= 0)
        set_number(L, "answer", load.answer);
    set_number(L, "out_type", load.out_type);
    return 1;
}

// Under g_lock: the wait for the server is over; how long it took is kept for the next one's bar
void time_wait(double now)
{
    if (g_load.asked_at >= 0)
        g_load.answer = now - g_load.asked_at;
    if (g_load.login || now > kServerLongest)
        return;
    g_wait = g_wait + (now - g_wait) * 0.5;
    note("the wait: %.0f ms (the server answered in %.0f ms); the next is drawn as %.0f", now, g_load.answer, g_wait);
}

// Under g_lock: the server named the zone (0x00A)
void zone_in(const std::uint8_t* packet, std::size_t length)
{
    if (g_load.phase == Phase::None)
        start(true); // no zone-out came first: the login's zone, after picking a character
    if (g_load.phase != Phase::Server)
        return;
    g_load.zone = length >= 0x34 ? word(packet + 0x30) : 0u;
    g_load.zone_in_at = now_ms();
    g_load.phase = Phase::Zone;
    time_wait(g_load.zone_in_at);
    const auto known = g_expected.find(g_load.zone);
    if (known != g_expected.end())
        g_load.expected = known->second;
    note("zone-in (0x00A): zone %u (last time %u and %u data files)", g_load.zone, g_load.expected.zone,
         g_load.expected.people);
}

// For the log: the first 64 bytes as hex, 8 to a group
std::string hex_of(const std::uint8_t* data, std::size_t length)
{
    std::string text;
    char two[4];
    for (std::size_t i = 0; i < std::min<std::size_t>(length, 64); ++i) {
        snprintf(two, sizeof two, "%02X", data[i]);
        text += (i != 0 && i % 8 == 0) ? " " : "";
        text += two;
    }
    return text;
}

// Under g_lock, for the log: the packets sent in the 5 s before the timeline's start
void note_sent_before()
{
    for (unsigned k = 0; k < std::size(g_sent); ++k) {
        const Sent& sent = g_sent[(g_sent_next + k) % std::size(g_sent)];
        const double ms = (sent.at.QuadPart - g_start.QuadPart) * 1000.0 / g_frequency.QuadPart;
        if (sent.length != 0 && ms > -5000)
            note("sent before it, %.1f ms: 0x%03X, %u bytes: %s", ms, sent.id, sent.length,
                 hex_of(sent.head, std::min<std::size_t>(sent.length, sizeof sent.head)).c_str());
    }
}

// Under g_lock, for the log: one packet out kept for note_sent_before
void keep_sent(std::uint16_t id, const std::uint8_t* packet, std::size_t length)
{
    Sent& sent = g_sent[g_sent_next++ % std::size(g_sent)];
    QueryPerformanceCounter(&sent.at);
    sent.id = id;
    sent.length = static_cast<std::uint16_t>(std::min<std::size_t>(length, 0xFFFF));
    std::copy_n(packet, std::min<std::size_t>(length, sizeof sent.head), sent.head);
}

// What the game sends: while it waits for the server, its leaving (0x00D) and its asking for the zone (0x00A) are the
// wait's steps; the log has every packet then, and the first of each kind after
void packet_out(std::uint16_t id, const std::uint8_t* packet, std::size_t length)
{
    if (!g_tracking && !g_log_on)
        return;
    const Held held;
    if (g_log_on)
        keep_sent(id, packet, length);
    if (!g_tracking)
        return;
    if (g_load.phase == Phase::Server) {
        if (id == 0x00D && g_load.step == Step::Leaving)
            g_load.step = Step::Connecting;
        if (id == 0x00A) {
            g_load.step = Step::Waiting;
            g_load.asked_at = now_ms();
        }
        if (g_logging)
            note("sent 0x%03X, %zu bytes: %s", id, length, hex_of(packet, length).c_str());
    } else if (!g_timeline.sent[id]) {
        note("sent 0x%03X, the first since the start", id);
    }
    g_timeline.sent[id] = true;
}

void packet_in(std::uint16_t id, const std::uint8_t* packet, std::size_t length)
{
    if (id == 0x00B) {
        const Held held;
        start(false);
        g_load.out_type = length > 4 ? packet[4] : 0u;
        if (g_logging) {
            note("0x00B, %zu bytes: %s", length, hex_of(packet, length).c_str());
            note_sent_before();
        }
        return;
    }
    if (id != 0x00A && !g_tracking)
        return;
    const Held held;
    if (id == 0x00A) {
        if (g_logging)
            note("0x00A, %zu bytes: %s", length, hex_of(packet, length).c_str());
        zone_in(packet, length);
    } else if (!g_tracking) {
        return;
    } else if (id == 0x00E && g_load.people_at < 0 && g_load.phase != Phase::Server) {
        g_load.people_at = now_ms(); // the first of the people's updates: their models load from here
        note("packet 0x00E: the people start");
    } else if (!g_timeline.seen[id]) {
        note("packet 0x%03X, the first since the start", id);
    }
    g_timeline.seen[id] = true;
}

} // namespace

void install(const wchar_t* ini, const wchar_t* install_dir)
{
    try {
        QueryPerformanceFrequency(&g_frequency);
        QueryPerformanceCounter(&g_start);
        g_ini = ini != nullptr ? ini : L"";
        g_log_on = log_wanted();
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        g_windows_a = reinterpret_cast<CreateFileA_t>(GetProcAddress(kernel, "CreateFileA"));
        g_windows_w = reinterpret_cast<CreateFileW_t>(GetProcAddress(kernel, "CreateFileW"));
        if (g_windows_a == nullptr || g_windows_w == nullptr)
            return; // nothing hooked: no loading screen
        std::wstring dir = install_dir != nullptr ? install_dir : L"";
        if (!dir.empty()) {
            if (dir.back() != L'\\')
                dir += L'\\';
            g_logs = dir + L"logs\\";
            CreateDirectoryW((dir + L"cache").c_str(), nullptr);
            g_cache = dir + L"cache\\zoneload.txt";
            load_cache();
        }
        if (!imports::for_each_module(&on_module))
            log::write(L"Zone loading: can't watch DLLs load, so the loading screen can't count the files read");
    } catch (...) {
        broke("install");
    }
}

void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length)
{
    if (length < 4 || g_broken)
        return;
    try {
        if (!incoming) {
            packet_out(word(packet) & 0x1FF, packet, length);
            return;
        }
        packet_in(word(packet) & 0x1FF, packet, length);
    } catch (...) {
        broke("a packet");
    }
}

void on_present()
{
    g_render_thread = GetCurrentThreadId();
    if (g_broken || (!g_tracking && !g_logging))
        return;
    try {
        const Held held;
        const double t = now_ms();
        time_frame(t);
        if (g_tracking)
            advance(t);
    } catch (...) {
        broke("a frame");
    }
}

void add_lua(lua_State* L)
{
    lua_pushcfunction(L, &lua_loading);
    lua_setfield(L, -2, "loading");
}

} // namespace vanacore::zoneload
