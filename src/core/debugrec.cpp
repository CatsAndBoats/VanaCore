#include "debugrec.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "chat.hpp"
#include "frametime.hpp"
#include "graphics.hpp"
#include "log.hpp"

namespace vanacore::debugrec {
namespace {

std::wstring g_logs;  // the logs folder, with its closing backslash
std::wstring g_stamp; // the recording's start, as its files' names carry it
log::Report g_report; // logs\debug-<date>-<time>.log

struct Frame {
    double at = 0;    // ms from the recording's start to this frame's end
    float frame = 0, game = 0, direct3d = 0, sleeps = 0, vanacore = 0, limit = 0, present = 0;
    float slowest = 0;
    const char* slowest_name = "";
    bool gfx = false; // Direct3D 12's numbers there
    float encode = 0, gpu_wait = 0, swap = 0, helper = 0, gpu = 0;
    unsigned draws = 0, upload_kb = 0, pipelines = 0, skipped = 0;
    int shown = 1; // frames shown for this one (frame generation)
};

struct Line {
    double at; // ms from the recording's start
    std::wstring text;
};

bool g_running = false;
double g_seconds = 30.0;
LARGE_INTEGER g_start{}, g_frequency{};
std::vector<Frame> g_frames;
std::mutex g_lines_lock;
std::vector<Line> g_lines;
constexpr size_t kMaxLines = 6000;

double ms_since_start()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_start.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
}

std::string format(const char* pattern, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof buffer, pattern, args);
    va_end(args);
    return buffer;
}

std::string narrow(const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// To the player's own chat log alone (chat::echo: never sent to anyone).
void echo(const std::string& text)
{
    chat::echo(text);
}

// The recording's report (logs\debug-<date>-<time>.log, kept whether or not logging is on), and the log too when it's on.
void note(const std::string& text)
{
    g_report.write(text);
    log::write(L"Debug: %hs", text.c_str());
}

void tap(const std::wstring& line)
{
    if (!g_running)
        return;
    const std::lock_guard lock(g_lines_lock);
    if (g_lines.size() < kMaxLines)
        g_lines.push_back({ms_since_start(), line});
}

// The log's lines every second or two (frame and renderer statistics): not news around a hitch
bool routine(const std::wstring& line)
{
    for (const wchar_t* prefix : {L"Frames:", L"Graphics: [gfx]", L"Graphics: gfx: shadows", L"Graphics: d3d8: scenes",
                                  L"Graphics: gfx: flicker watch", L"Debug:"})
        if (line.rfind(prefix, 0) == 0)
            return true;
    return false;
}

float percentile(std::vector<float> values, double share)
{
    if (values.empty())
        return 0.0f;
    const size_t at = std::min(values.size() - 1, static_cast<size_t>(share * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

void finish();

void on_frame(const frametime::Parts& p)
{
    if (!g_running)
        return;
    Frame f;
    f.at = ms_since_start();
    f.frame = static_cast<float>(p.frame);
    f.direct3d = static_cast<float>(p.direct3d), f.sleeps = static_cast<float>(p.sleeps);
    f.vanacore = static_cast<float>(p.vanacore), f.limit = static_cast<float>(p.limit), f.present = static_cast<float>(p.present);
    f.game = std::max(0.0f, f.frame - f.direct3d - f.sleeps - f.vanacore - f.limit - f.present);
    f.slowest = static_cast<float>(p.slowest), f.slowest_name = p.slowest_name;
    VanaGfxFrame g{};
    if (vanacore_gfx_frame_stats(&g)) {
        f.gfx = true;
        f.encode = g.encode_ms, f.gpu_wait = g.gpu_wait_ms, f.swap = g.swap_ms, f.helper = g.helper_wait_ms, f.gpu = g.gpu_ms;
        f.draws = g.draws, f.upload_kb = g.upload_kb, f.pipelines = g.pipelines, f.skipped = g.skipped;
    }
    f.shown = graphics::shown_per_frame();
    g_frames.push_back(f);
    if (f.at >= g_seconds * 1000.0)
        finish();
}

// ---- what the recording says ----------------------------------------------------------------------------------------

std::wstring csv_path()
{
    return g_logs + L"frames-" + g_stamp + L".csv";
}

bool write_csv(const std::wstring& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || f == nullptr)
        return false;
    std::fprintf(f, "frame,at_ms,frame_ms,game_ms,direct3d_ms,sleeps_ms,vanacore_ms,limit_ms,present_ms,slowest_call_ms,slowest_call,"
                    "encode_ms,gpu_wait_ms,swap_chain_ms,dlss_helper_wait_ms,gpu_ms,draws,upload_kb,new_pipelines,skipped_draws,shown\n");
    for (size_t i = 0; i < g_frames.size(); ++i) {
        const Frame& r = g_frames[i];
        std::fprintf(f, "%zu,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,", i, r.at, r.frame, r.game, r.direct3d, r.sleeps, r.vanacore,
                     r.limit, r.present, r.slowest, r.slowest_name);
        if (r.gfx)
            std::fprintf(f, "%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%d\n", r.encode, r.gpu_wait, r.swap, r.helper, r.gpu, r.draws, r.upload_kb,
                         r.pipelines, r.skipped, r.shown);
        else
            std::fprintf(f, ",,,,,,,,,%d\n", r.shown);
    }
    std::fclose(f);
    return true;
}

struct Part {
    const char* name;
    float Frame::*field;
};
constexpr Part kParts[] = {{"the game's own work", &Frame::game},   {"its Direct3D calls", &Frame::direct3d},
                           {"its sleeps", &Frame::sleeps},          {"VanaCore's work", &Frame::vanacore},
                           {"VanaCore's frame rate limit", &Frame::limit}, {"Present", &Frame::present}};

void analyse(const std::wstring& csv, bool wrote, const std::vector<Line>& lines)
{
    const size_t n = g_frames.size();
    if (n < 10) {
        echo("debug: too few frames recorded to say anything");
        note("too few frames recorded to say anything");
        return;
    }
    std::vector<float> times;
    times.reserve(n);
    double total = 0;
    for (const Frame& f : g_frames)
        times.push_back(f.frame), total += f.frame;
    const double seconds = total / 1000.0;
    const float median = percentile(times, 0.5), p95 = percentile(times, 0.95), p99 = percentile(times, 0.99);
    const float worst = *std::max_element(times.begin(), times.end());
    // averages
    double avg[std::size(kParts)] = {};
    double gpu = 0, encode = 0, gpu_wait = 0, swap = 0, helper = 0, draws = 0, upload = 0;
    unsigned gfx_frames = 0, pipelines = 0, skipped = 0;
    int shown = 1;
    for (const Frame& f : g_frames) {
        for (size_t k = 0; k < std::size(kParts); ++k)
            avg[k] += f.*(kParts[k].field);
        if (f.gfx) {
            ++gfx_frames;
            gpu += f.gpu, encode += f.encode, gpu_wait += f.gpu_wait, swap += f.swap, helper += f.helper;
            draws += f.draws, upload += f.upload_kb, pipelines += f.pipelines, skipped += f.skipped;
        }
        shown = std::max(shown, f.shown);
    }
    for (double& a : avg)
        a /= static_cast<double>(n);
    const double mean = total / static_cast<double>(n);
    if (gfx_frames) {
        const double k = 1.0 / gfx_frames;
        gpu *= k, encode *= k, gpu_wait *= k, swap *= k, helper *= k, draws *= k, upload *= k;
    }

    echo(format("debug: %zu frames in %.0f s: %.1f fps (median %.1f ms, 95%% %.1f, 99%% %.1f, worst %.1f)%s", n, seconds,
               static_cast<double>(n) / seconds, median, p95, p99, worst,
               shown > 1 ? format("; %.0f shown with frame generation %dx", shown * static_cast<double>(n) / seconds, shown).c_str()
                         : ""));
    note(format("recorded %zu frames in %.1f s: %.1f fps; frame median %.2f ms, 95%% %.2f, 99%% %.2f, worst %.2f", n, seconds,
                static_cast<double>(n) / seconds, median, p95, p99, worst));
    // the spread
    const float edges[] = {8.4f, 16.8f, 20.0f, 25.0f, 33.4f, 50.0f, 100.0f};
    unsigned buckets[std::size(edges) + 1] = {};
    for (const float t : times) {
        size_t b = 0;
        while (b < std::size(edges) && t >= edges[b])
            ++b;
        ++buckets[b];
    }
    std::string spread = "frame times:";
    const char* labels[] = {"<8.4", "8.4-16.8", "16.8-20", "20-25", "25-33", "33-50", "50-100", ">=100"};
    for (size_t b = 0; b < std::size(buckets); ++b)
        if (buckets[b])
            spread += format(" %s ms %u (%.1f%%);", labels[b], buckets[b], 100.0 * buckets[b] / n);
    note(spread);
    // the average frame
    std::string parts = format("the average frame, %.2f ms:", mean);
    for (size_t k = 0; k < std::size(kParts); ++k)
        parts += format("%s %s %.2f", k ? "," : "", kParts[k].name, avg[k]);
    note(parts);
    if (gfx_frames)
        note(format("Direct3D 12 (%u frames): drawing the game's calls %.2f ms, waiting for the GPU %.2f, the swap chain %.2f, the "
                    "DLSS helper %.2f; GPU %.2f ms a frame; %.0f draws, %.0f KB uploaded; %u pipelines made, %u draws left out",
                    gfx_frames, encode, gpu_wait, swap, helper, gpu, draws, upload, pipelines, skipped));
    // what holds it
    std::string held;
    const double sleeps = avg[2], limit = avg[4];
    if (sleeps > 2.0 && mean < 18.5)
        held = format("held at about 60 fps by the game's own frame cap: it sleeps %.1f ms of each frame (DLSS and settings can't make it "
                      "faster here)",
                      sleeps);
    else if (limit > 2.0)
        held = format("held by VanaCore's frame rate limit (fps.max): %.1f ms of each frame waits on it", limit);
    else if (gfx_frames && gpu > mean * 0.85)
        held = format("held by the GPU: %.1f ms of GPU work for a %.1f ms frame (a smaller DLSS size, or fewer effects, would help)", gpu,
                      mean);
    else
        held = format("held by the CPU: the game's own work %.1f ms, its Direct3D calls %.1f ms, VanaCore %.1f ms, of a %.1f ms frame%s",
                      avg[0], avg[1], avg[3], mean, gfx_frames ? format("; the GPU needs %.1f ms", gpu).c_str() : "");
    echo("debug: " + held);
    note(held);

    // hitches: frames well over the usual
    const float threshold = std::max(median * 1.75f, median + 8.0f);
    std::vector<size_t> hitches;
    for (size_t i = 0; i < n; ++i)
        if (g_frames[i].frame >= threshold)
            hitches.push_back(i);
    if (hitches.empty()) {
        echo(format("debug: no hitches (no frame over %.1f ms)", threshold));
        note(format("no hitches (no frame over %.1f ms)", threshold));
    } else {
        // what grew in each: the part furthest over its median
        std::vector<float> medians;
        for (const Part& part : kParts) {
            std::vector<float> v;
            v.reserve(n);
            for (const Frame& f : g_frames)
                v.push_back(f.*(part.field));
            medians.push_back(percentile(v, 0.5));
        }
        unsigned by_part[std::size(kParts)] = {};
        std::vector<std::pair<size_t, size_t>> worst_first; // (frame, part)
        for (const size_t i : hitches) {
            const Frame& f = g_frames[i];
            size_t grew = 0;
            float most = -1e9f;
            for (size_t k = 0; k < std::size(kParts); ++k) {
                const float over = f.*(kParts[k].field) - medians[k];
                if (over > most)
                    most = over, grew = k;
            }
            ++by_part[grew];
            worst_first.push_back({i, grew});
        }
        std::string where;
        for (size_t k = 0; k < std::size(kParts); ++k)
            if (by_part[k])
                where += format("%s%s %u", where.empty() ? "" : ", ", kParts[k].name, by_part[k]);
        // a steady interval?
        std::string pattern;
        if (hitches.size() >= 3) {
            std::vector<double> gaps;
            for (size_t h = 1; h < hitches.size(); ++h)
                gaps.push_back((g_frames[hitches[h]].at - g_frames[hitches[h - 1]].at) / 1000.0);
            double gm = 0;
            for (const double g : gaps)
                gm += g;
            gm /= static_cast<double>(gaps.size());
            double var = 0;
            for (const double g : gaps)
                var += (g - gm) * (g - gm);
            const double cv = gm > 0 ? std::sqrt(var / static_cast<double>(gaps.size())) / gm : 1.0;
            pattern = cv < 0.25 ? format("; they come steadily, about every %.1f s", gm)
                                : format("; no steady interval (on average every %.1f s)", gm);
        }
        echo(format("debug: %zu hitches (frames over %.1f ms): grew in %s%s", hitches.size(), threshold, where.c_str(), pattern.c_str()));
        note(format("%zu hitches (frames over %.1f ms, twice-ish the median): what grew most in each: %s%s", hitches.size(), threshold,
                    where.c_str(), pattern.c_str()));
        // the worst, with what the log said around them
        std::sort(worst_first.begin(), worst_first.end(),
                  [](const auto& a, const auto& b) { return g_frames[a.first].frame > g_frames[b.first].frame; });
        for (size_t w = 0; w < worst_first.size() && w < 10; ++w) {
            const Frame& f = g_frames[worst_first[w].first];
            const size_t k = worst_first[w].second;
            std::string line = format("hitch at %.2f s: %.1f ms; %s %.1f ms (usually %.1f)", (f.at - f.frame) / 1000.0, f.frame,
                                      kParts[k].name, f.*(kParts[k].field), medians[k]);
            if (f.slowest >= 4.0f)
                line += format("; its slowest Direct3D call %s %.1f ms", f.slowest_name, f.slowest);
            if (f.gfx) {
                if (f.helper >= 2.0f)
                    line += format("; waited %.1f ms on the DLSS helper", f.helper);
                if (f.gpu_wait >= 4.0f)
                    line += format("; waited %.1f ms for the GPU", f.gpu_wait);
                if (f.swap >= 4.0f)
                    line += format("; the swap chain %.1f ms", f.swap);
                if (f.pipelines)
                    line += format("; %u pipelines made", f.pipelines);
                if (f.upload_kb > 4 * static_cast<unsigned>(upload + 1))
                    line += format("; %u KB uploaded", f.upload_kb);
            }
            note(line);
            const double from = f.at - f.frame - 30.0, to = f.at + 5.0;
            for (const Line& l : lines)
                if (l.at >= from && l.at <= to && !routine(l.text))
                    note("    the log then: " + narrow(l.text));
        }
    }
    const std::string report = g_report.is_open() ? narrow(g_report.path().substr(g_logs.size())) : "";
    if (wrote) {
        note("every frame: logs\\" + narrow(csv.substr(g_logs.size())));
        echo("debug: every frame in logs\\" + narrow(csv.substr(g_logs.size())) +
             (report.empty() ? "" : ", the details in logs\\" + report));
    } else {
        note("the frames' CSV couldn't be written");
        echo(report.empty() ? "debug: the frames' CSV couldn't be written"
                            : "debug: the details are in logs\\" + report + " (the frames' CSV couldn't be written)");
    }
}

void finish()
{
    g_running = false;
    frametime::record(nullptr);
    frametime::profile(false);
    log::set_tap(nullptr);
    // the log's lines taken out first: analysing writes to the log, which mustn't wait on this lock
    std::vector<Line> lines;
    {
        const std::lock_guard lock(g_lines_lock);
        lines.swap(g_lines);
    }
    const std::wstring csv = csv_path();
    const bool wrote = write_csv(csv);
    analyse(csv, wrote, lines);
    g_report.close();
    g_frames.clear();
    g_frames.shrink_to_fit();
}

} // namespace

void install(const wchar_t* install_dir)
{
    g_logs = install_dir != nullptr ? install_dir : L"";
    if (!g_logs.empty() && g_logs.back() != L'\\')
        g_logs += L'\\';
    g_logs += L"logs\\";
    QueryPerformanceFrequency(&g_frequency);
}

bool command(const std::vector<std::string>& parts)
{
    const std::string option = parts.size() > 1 ? parts[1] : "";
    if (option == "stop") {
        if (g_running)
            finish();
        else
            echo("debug: no recording is running");
        return true;
    }
    if (g_running) {
        echo(format("debug: recording already (%.0f s of %.0f); /vana debug stop ends it now", ms_since_start() / 1000.0, g_seconds));
        return true;
    }
    g_seconds = option.empty() ? 30.0 : std::clamp(std::atof(option.c_str()), 5.0, 120.0);
    g_frames.clear();
    g_frames.reserve(static_cast<size_t>(g_seconds * 250));
    {
        const std::lock_guard lock(g_lines_lock);
        g_lines.clear();
    }
    QueryPerformanceCounter(&g_start);
    // The recording's files are kept whether or not logging is on: the frames' CSV and a report of what it found. With
    // logging off the report has no log lines round the hitches (nothing is written to the log for them to come from).
    g_stamp = log::file_stamp();
    CreateDirectoryW(g_logs.c_str(), nullptr);
    if (!g_report.open(g_logs, L"debug", g_stamp))
        echo("debug: the report file couldn't be made in logs\\; the results are shown here only");
    if (!log::enabled())
        note("logging is off ([logging] enabled), so the log's lines round each hitch aren't here");
    g_running = true;
    log::set_tap(&tap);
    frametime::profile(true);
    frametime::record(&on_frame);
    echo(format("debug: recording %.0f s of frames: play as you were (or stand where it hitches); /vana debug stop ends it early",
               g_seconds));
    note(format("recording %.0f s of frames", g_seconds));
    return true;
}

} // namespace vanacore::debugrec
