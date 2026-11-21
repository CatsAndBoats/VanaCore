#include "abtest.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "chat.hpp"
#include "graphics.hpp"
#include "ini.hpp"
#include "log.hpp"

namespace vanacore::abtest {
namespace {

std::wstring g_ini;
std::wstring g_logs;     // the logs folder, with its closing backslash
log::Report g_report;    // logs\abtest-<date>-<time>.log: kept whether or not logging is on

struct Period {
    int mode = 0;
    double ms = 0.0; // the frames' time measured in it
    unsigned frames = 0;
    std::vector<float> times; // each frame's, ms
};

struct Test {
    bool running = false;
    std::wstring section, key;
    std::string name;         // as typed: dlss
    std::wstring values[2];   // A and B
    std::wstring original;    // the setting before, to put back
    bool had_original = false;
    double seconds = 8.0;     // a period
    double settle = 2.0;      // not timed after a switch
    std::vector<int> schedule; // the periods' modes: A B B A ...
    size_t at = 0;            // the period running
    LARGE_INTEGER started{};  // ... since
    LARGE_INTEGER last{};     // the last frame
    std::vector<float> samples[2]; // frame times, ms, by mode
    int shown[2] = {1, 1};    // frames shown per game frame (frame generation) while timed
    std::vector<Period> periods;
};
Test g_test;
LARGE_INTEGER g_frequency{};

std::wstring widen(const std::string& text)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// To the player's own chat log alone (chat::echo: never sent to anyone), and to the test's report file.
void echo(const std::string& text)
{
    chat::echo(text);
    g_report.write(text);
}

// To the report file alone: the detail the chat log doesn't need.
void detail(const std::string& text)
{
    g_report.write(text);
}

// A [section] key of vanacore.ini as it stands, "-" when it isn't set
std::string ini_value(const wchar_t* section, const wchar_t* key)
{
    wchar_t value[128] = L"";
    const DWORD n = GetPrivateProfileStringW(section, key, L"\x01", value, 128, g_ini.c_str());
    return n == 1 && value[0] == L'\x01' ? "-" : narrow(value);
}

double seconds_between(const LARGE_INTEGER& from, const LARGE_INTEGER& to)
{
    return static_cast<double>(to.QuadPart - from.QuadPart) / static_cast<double>(g_frequency.QuadPart);
}

void set_value(const std::wstring* value)
{
    ini::set(g_ini, g_test.section.c_str(), g_test.key.c_str(), value != nullptr ? value->c_str() : nullptr);
    graphics::reload();
}

std::string format(const char* pattern, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof buffer, pattern, args);
    va_end(args);
    return buffer;
}

// A mode's frames: frames a second (over the time timed), the average and the 50th, 95th and 99th percentile frame
float percentile(std::vector<float> sorted, double share)
{
    if (sorted.empty())
        return 0.0f;
    const size_t at = std::min(sorted.size() - 1, static_cast<size_t>(share * static_cast<double>(sorted.size())));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(at), sorted.end());
    return sorted[at];
}

void report()
{
    const Test& t = g_test;
    double fps[2] = {}, mean[2] = {};
    echo(format("%s: %s (A) against %s (B), %zu periods of %.0f s", t.name.c_str(), narrow(t.values[0]).c_str(),
               narrow(t.values[1]).c_str(), t.schedule.size(), t.seconds));
    for (int mode = 0; mode < 2; ++mode) {
        const std::vector<float>& s = t.samples[mode];
        double total = 0.0;
        for (const float ms : s)
            total += ms;
        if (s.empty() || total <= 0.0) {
            echo(format("  %s: no frames timed", narrow(t.values[mode]).c_str()));
            continue;
        }
        mean[mode] = total / static_cast<double>(s.size());
        fps[mode] = 1000.0 / mean[mode];
        std::string line = format("  %s: %.1f fps, %.2f ms a frame (median %.2f, 95%% %.2f, 99%% %.2f), %zu frames",
                                  narrow(t.values[mode]).c_str(), fps[mode], mean[mode], percentile(s, 0.50),
                                  percentile(s, 0.95), percentile(s, 0.99), s.size());
        if (t.shown[mode] > 1)
            line += format("; %.0f shown (frame generation %dx)", fps[mode] * t.shown[mode], t.shown[mode]);
        echo(line);
    }
    if (fps[0] > 0.0 && fps[1] > 0.0)
        echo(format("  B against A: %+.1f%% frames a second, %+.2f ms a frame", (fps[1] / fps[0] - 1.0) * 100.0,
                   mean[1] - mean[0]));
    std::string each = "  each period:";
    for (const Period& p : t.periods)
        each += p.frames != 0 ? format(" %s %.1f", p.mode == 0 ? "A" : "B", 1000.0 * p.frames / p.ms)
                              : format(" %s -", p.mode == 0 ? "A" : "B");
    echo(each);
    // the report file's detail: each period's frames
    for (size_t i = 0; i < t.periods.size(); ++i) {
        const Period& p = t.periods[i];
        if (p.frames == 0) {
            detail(format("period %zu, %s = %s: no frames timed", i + 1, p.mode == 0 ? "A" : "B", narrow(t.values[p.mode]).c_str()));
            continue;
        }
        detail(format("period %zu, %s = %s: %u frames in %.1f s, %.1f fps, %.2f ms a frame (median %.2f, 95%% %.2f, 99%% %.2f, worst %.2f)",
                      i + 1, p.mode == 0 ? "A" : "B", narrow(t.values[p.mode]).c_str(), p.frames, p.ms / 1000.0, 1000.0 * p.frames / p.ms,
                      p.ms / p.frames, percentile(p.times, 0.50), percentile(p.times, 0.95), percentile(p.times, 0.99),
                      *std::max_element(p.times.begin(), p.times.end())));
    }
}

void finish(bool completed)
{
    set_value(g_test.had_original ? &g_test.original : nullptr);
    g_test.running = false;
    if (completed)
        report();
    else
        echo(format("%s: stopped; back to %s", g_test.name.c_str(),
                    g_test.had_original ? narrow(g_test.original).c_str() : "its default"));
    if (g_report.is_open()) {
        const std::string file = narrow(g_report.path().substr(g_report.path().rfind(L'\\') + 1));
        g_report.close();
        chat::echo("the details are in logs\\" + file);
    }
}

void start_period(const LARGE_INTEGER& now)
{
    Test& t = g_test;
    const int mode = t.schedule[t.at];
    set_value(&t.values[mode]);
    t.started = now;
    t.periods.push_back({mode, 0.0, 0, {}});
}

void usage()
{
    echo("/vana abtest <setting>=<a>,<b> [seconds, 8] [rounds, 2]: flips the setting between a and b in A B B A order "
        "and times the frames of each (stand still). For example: /vana abtest dlss=off,on, /vana abtest dlss.framegen=off,2x, "
        "/vana abtest dlss.quality=35,100; another section's setting as section/key. /vana abtest stop puts it back.");
}

} // namespace

void install(const wchar_t* ini, const wchar_t* install_dir)
{
    g_ini = ini != nullptr ? ini : L"";
    g_logs = install_dir != nullptr ? install_dir : L"";
    if (!g_logs.empty() && g_logs.back() != L'\\')
        g_logs += L'\\';
    g_logs += L"logs\\";
    QueryPerformanceFrequency(&g_frequency);
}

bool command(const std::vector<std::string>& parts)
{
    if (parts.size() < 2) {
        if (g_test.running)
            echo(format("%s: period %zu of %zu running", g_test.name.c_str(), g_test.at + 1, g_test.schedule.size()));
        usage();
        return true;
    }
    if (parts[1] == "stop") {
        if (g_test.running)
            finish(false);
        else
            echo("no A/B test is running");
        return true;
    }
    if (g_test.running) {
        echo("an A/B test is already running (/vana abtest stop ends it)");
        return true;
    }
    if (g_ini.empty()) {
        echo("there's no vanacore.ini to change");
        return true;
    }
    const std::string& spec = parts[1];
    const size_t equals = spec.find('='), comma = spec.find(',', equals == std::string::npos ? 0 : equals);
    if (equals == std::string::npos || comma == std::string::npos || equals == 0 || comma == equals + 1 ||
        comma + 1 >= spec.size()) {
        usage();
        return true;
    }
    Test t;
    t.name = spec.substr(0, equals);
    const size_t slash = t.name.find('/');
    t.section = slash == std::string::npos ? L"graphics" : widen(t.name.substr(0, slash));
    t.key = widen(slash == std::string::npos ? t.name : t.name.substr(slash + 1));
    t.values[0] = widen(spec.substr(equals + 1, comma - equals - 1));
    t.values[1] = widen(spec.substr(comma + 1));
    t.seconds = parts.size() > 2 ? std::clamp(std::atof(parts[2].c_str()), 3.0, 120.0) : 8.0;
    const int rounds = parts.size() > 3 ? std::clamp(std::atoi(parts[3].c_str()), 1, 20) : 2;
    t.settle = std::max(1.0, t.seconds / 4.0);
    for (int r = 0; r < rounds; ++r)
        t.schedule.insert(t.schedule.end(), {0, 1, 1, 0});
    wchar_t current[256] = L"";
    const DWORD n = GetPrivateProfileStringW(t.section.c_str(), t.key.c_str(), L"\x01", current, 256, g_ini.c_str());
    t.had_original = !(n == 1 && current[0] == L'\x01');
    t.original = t.had_original ? current : L"";
    t.running = true;
    g_test = std::move(t);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_test.last = now;
    // The report file, kept whether or not logging is on: what was compared, and the settings it was compared under
    if (g_report.open(g_logs, L"abtest", log::file_stamp())) {
        detail(format("A/B test of %s: A = %s, B = %s; %zu periods of %.0f s (%.0f s not timed after each switch), "
                      "the setting was %s",
                      g_test.name.c_str(), narrow(g_test.values[0]).c_str(), narrow(g_test.values[1]).c_str(), g_test.schedule.size(),
                      g_test.seconds, g_test.settle,
                      g_test.had_original ? narrow(g_test.original).c_str() : "not set"));
        detail("vanacore.ini [graphics]: renderer=" + ini_value(L"graphics", L"renderer") + ", effects=" + ini_value(L"graphics", L"effects") +
               ", antialias=" + ini_value(L"graphics", L"antialias") + ", background=" + ini_value(L"graphics", L"background") +
               ", resolution=" + ini_value(L"graphics", L"resolution") + ", vsync=" + ini_value(L"graphics", L"vsync") +
               ", dlss=" + ini_value(L"graphics", L"dlss") + ", dlss.quality=" + ini_value(L"graphics", L"dlss.quality") +
               ", dlss.framegen=" + ini_value(L"graphics", L"dlss.framegen") + "; [client] fps.max=" + ini_value(L"client", L"fps.max"));
    } else {
        chat::echo("the report file couldn't be made in logs\\; the results are shown here only");
    }
    echo(format("%s: %s against %s, %zu periods of %.0f s (about %.0f s): stand still", g_test.name.c_str(),
               narrow(g_test.values[0]).c_str(), narrow(g_test.values[1]).c_str(), g_test.schedule.size(), g_test.seconds,
               g_test.seconds * static_cast<double>(g_test.schedule.size())));
    start_period(now);
    return true;
}

void on_frame()
{
    Test& t = g_test;
    if (!t.running)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double frame = seconds_between(t.last, now);
    t.last = now;
    if (seconds_between(t.started, now) >= t.seconds) {
        if (++t.at >= t.schedule.size()) {
            finish(true);
            return;
        }
        start_period(now);
        return;
    }
    // the frame just ended, timed once the switch has settled
    if (seconds_between(t.started, now) < t.settle || graphics::settling() || frame <= 0.0 || frame > 1.0)
        return;
    const int mode = t.schedule[t.at];
    t.samples[mode].push_back(static_cast<float>(frame * 1000.0));
    t.shown[mode] = std::max(t.shown[mode], graphics::shown_per_frame());
    Period& p = t.periods.back();
    p.ms += frame * 1000.0;
    ++p.frames;
    p.times.push_back(static_cast<float>(frame * 1000.0));
}

} // namespace vanacore::abtest
