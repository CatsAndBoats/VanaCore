#define PSAPI_VERSION 2
#include "perf.hpp"

#include <windows.h>

#include <psapi.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "chat.hpp"
#include "graphics.hpp"
#include "gui.hpp"
#include "log.hpp"
#include "vanaui.hpp"

namespace vanacore::perf {

using namespace vanaui;

namespace {

constexpr int kSamples = 120;                      // points: 1 minute, one every 500 ms
constexpr double kInterval = 0.5;                  // s, a point
constexpr int kSubs = 5;                           // a point is the average of this many samples
constexpr double kSubInterval = kInterval / kSubs; // s, a sample: every 100 ms

enum Id {
    kFps,
    kShownFps,
    kFrame,
    kWorst,
    kCpu,
    kMemory,
    kAddress,
    kFreeBlock,
    kCount
};

struct Metric {
    const char* name; // the list's
    const char* csv;  // the CSV's column
    const char* unit;
    int decimals;
    bool from_zero;  // the graph's bottom is 0 (else the lowest value in it)
    bool time;       // a time: the 95th and 99th percentile are worth saying
    float red_under; // the line is red where the metric is under this (FPS under 30); 0: nowhere
};

constexpr float kLowFps = 30.0f;

// (where each frame goes, and DirectX 12's GPU time, draws and uploads, were here behind a Detailed timing switch: /vana
// stats has them)
constexpr Metric kMetrics[kCount] = {
    {"FPS", "fps", "fps", 1, true, false, kLowFps},
    {"FPS shown (with frame generation)", "shown_fps", "fps", 1, true, false, kLowFps},
    {"Frame time", "frame_ms", "ms", 2, true, true, 0.0f},
    {"Worst frame in each point", "worst_frame_ms", "ms", 2, true, true, 0.0f},
    {"CPU used by the game", "cpu_cores", "cores", 2, true, false, 0.0f},
    {"Memory (private)", "memory_mb", "MB", 0, false, false, 0.0f},
    {"Address space used (of the 32-bit game's 4 GB)", "address_mb", "MB", 0, false, false, 0.0f},
    {"Address space: the largest free block", "largest_free_mb", "MB", 0, false, false, 0.0f},
};

// What the frames since the last sample added up to
struct Acc {
    int frames = 0;
    double frame_sum = 0, worst = 0;
};

std::wstring g_logs; // the logs folder, with its closing backslash
LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_last_frame{}, g_window_start{}; // the last frame; the last sample
LARGE_INTEGER g_sample_due{};                   // when the next sample is
ULONGLONG g_last_cpu = 0;

// The point being made: its samples so far
struct Pending {
    float sum[kCount] = {}; // each metric's samples added up (the worst frame: the highest of them)
    int samples = 0;
};
Pending g_pending;

float g_values[kCount][kSamples] = {};
int g_count = 0; // points held
int g_head = 0;  // where the next one goes
Acc g_acc;

bool g_open = false;
bool g_paused = false;
int g_selected = kFps;

ULONGLONG process_cpu()
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime, k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime, u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart; // 100 ns
}

double private_mb()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters))
        return 0.0;
    return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
}

// The largest free block of the address space, in bytes: walked for on a thread of its own every 2 s (some thousands of
// regions, a few ms), started at install
std::atomic<unsigned long long> g_largest_free{0};
bool g_walker = false;

DWORD WINAPI walk_thread(void*)
{
    for (;;) {
        MEMORY_BASIC_INFORMATION region{};
        unsigned long long largest = 0;
        for (auto at = reinterpret_cast<const BYTE*>(0x10000); VirtualQuery(at, &region, sizeof region) == sizeof region;) {
            if (region.State == MEM_FREE)
                largest = std::max<unsigned long long>(largest, region.RegionSize);
            const auto* next = static_cast<const BYTE*>(region.BaseAddress) + region.RegionSize;
            if (next <= at)
                break; // the top of the address space
            at = next;
        }
        g_largest_free = largest;
        Sleep(2000);
    }
}

void start_walker()
{
    if (g_walker)
        return;
    g_walker = true;
    if (const HANDLE thread = CreateThread(nullptr, 0, &walk_thread, nullptr, 0, nullptr)) {
        SetThreadPriority(thread, THREAD_PRIORITY_LOWEST);
        CloseHandle(thread);
    }
}

void push(const float (&v)[kCount])
{
    for (int m = 0; m < kCount; ++m)
        g_values[m][g_head] = v[m];
    g_head = (g_head + 1) % kSamples;
    g_count = std::min(g_count + 1, kSamples);
}

// The frames since the last sample, as a sample's values
void sample(double seconds, float (&v)[kCount])
{
    const Acc& a = g_acc;
    const double frames = a.frames;
    v[kFps] = static_cast<float>(frames / seconds);
    v[kShownFps] = v[kFps] * static_cast<float>(graphics::shown_per_frame());
    v[kFrame] = frames > 0 ? static_cast<float>(a.frame_sum / frames) : 0.0f;
    v[kWorst] = static_cast<float>(a.worst);
    const ULONGLONG cpu = process_cpu();
    if (g_last_cpu != 0 && cpu >= g_last_cpu)
        v[kCpu] = static_cast<float>(static_cast<double>(cpu - g_last_cpu) / 1.0e7 / seconds); // cores' worth
    g_last_cpu = cpu;
    v[kMemory] = static_cast<float>(private_mb());
    const AddressSpace space = address_space();
    v[kAddress] = static_cast<float>(space.used_mb);
    v[kFreeBlock] = static_cast<float>(space.largest_free_mb);
}

// A point from its samples: their average (the worst frame: the worst of them)
void close_point()
{
    const Pending& p = g_pending;
    float v[kCount] = {};
    for (int m = 0; m < kCount; ++m)
        v[m] = m == kWorst ? p.sum[m] : p.sum[m] / static_cast<float>(p.samples);
    if (!g_paused)
        push(v);
    g_pending = {};
}

// Every 100 ms: the frames since the last sample as one, and every fifth sample a point
void close_sample(double seconds, const LARGE_INTEGER& now)
{
    float v[kCount] = {};
    sample(seconds, v);
    Pending& p = g_pending;
    for (int m = 0; m < kCount; ++m)
        p.sum[m] = m == kWorst ? std::max(p.sum[m], v[m]) : p.sum[m] + v[m];
    ++p.samples;
    g_acc = {};
    g_window_start = now;
    if (p.samples >= kSubs)
        close_point();
}

// A metric's points, oldest first
std::vector<float> series(int metric)
{
    std::vector<float> out;
    out.reserve(static_cast<size_t>(g_count));
    for (int i = 0; i < g_count; ++i)
        out.push_back(g_values[metric][g_count < kSamples ? i : (g_head + i) % kSamples]);
    return out;
}

float percentile(std::vector<float> values, double share)
{
    if (values.empty())
        return 0.0f;
    const size_t at = std::min(values.size() - 1, static_cast<size_t>(share * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

// ---- logs\graph-<date>-<time>.csv --------------------------------------------------------------------------------

bool save_csv(std::wstring& name)
{
    if (g_count == 0 || g_logs.empty())
        return false;
    CreateDirectoryW(g_logs.c_str(), nullptr);
    name = L"graph-" + log::file_stamp() + L".csv";
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_logs + name).c_str(), L"w") != 0 || f == nullptr)
        return false;
    std::fprintf(f, "seconds_ago");
    for (const Metric& m : kMetrics)
        std::fprintf(f, ",%s", m.csv);
    std::fprintf(f, "\n");
    for (int i = 0; i < g_count; ++i) {
        std::fprintf(f, "%.2f", static_cast<double>(g_count - 1 - i) * kInterval);
        for (int m = 0; m < kCount; ++m)
            std::fprintf(f, ",%.3f", static_cast<double>(g_values[m][g_count < kSamples ? i : (g_head + i) % kSamples]));
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

void save_and_say()
{
    std::wstring name;
    if (!save_csv(name)) {
        chat::echo(g_count == 0 ? "graph: nothing recorded yet" : "graph: the CSV couldn't be written");
        return;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), utf8.data(), bytes, nullptr, nullptr);
    chat::echo("graph: the last " + std::to_string(static_cast<int>(g_count * kInterval)) + " s in logs\\" + utf8);
}

// ---- the window (the /vana window's look: vanaui.hpp) --------------------------------------------------------------

// A value with its unit: "34.3 fps", "25.11 ms", "2489 draws", "3" (pipelines: no unit)
std::string words(const Metric& m, double value)
{
    char text[48];
    std::snprintf(text, sizeof text, "%.*f%s%s", m.decimals, value, m.unit[0] != '\0' ? " " : "", m.unit);
    return text;
}

// The metric's drop-down, as the /vana window's: a sunken box with the name and a chevron, the list opening under it
void metric_picker(float width)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = S(30);
    const bool clicked = ImGui::InvisibleButton("##metric", ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + width, at.y + h);
    const float t = ease("##metric.lit", hovered ? 1.0f : 0.0f, 22.0f);
    dl->AddRectFilled(at, end, u32(P::well), S(4));
    dl->AddRect(at, end, u32(mix(P::well_edge, P::accent_lo, t)), S(4));
    const float cy = at.y + h * 0.5f;
    text_mid(dl, at.x + S(10), cy, kMetrics[g_selected].name, P::text, g_fonts.text, kSmall + 0.5f);
    icon(dl, end.x - S(14), cy + S(1), kIconDown, P::dim, 11.0f);
    if (clicked)
        ImGui::OpenPopup("##metrics");
    ImGui::SetNextWindowPos(ImVec2(at.x, end.y + S(4)));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    if (ImGui::BeginPopup("##metrics")) {
        push_font(g_fonts.text, kSmall + 0.5f);
        for (int m = 0; m < kCount; ++m) {
            if (ImGui::Selectable(kMetrics[m].name, m == g_selected))
                g_selected = m;
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }
}

// The graph's line and the wash under it down to `bottom`, red where the value is under `red_under` (FPS under 30) and
// the accent elsewhere: drawn as runs of one colour, a step that crosses `red_under` changing colour where it crosses
void line(ImDrawList* dl, const std::vector<ImVec2>& points, const std::vector<float>& values, float red_under, float bottom)
{
    std::vector<ImVec2> run;
    run.reserve(points.size() + 1);
    bool red = values.front() < red_under;
    const auto flush = [&] {
        const unsigned rgb = red ? P::rose : P::accent;
        for (size_t i = 1; i < run.size(); ++i)
            dl->AddQuadFilled(run[i - 1], run[i], ImVec2(run[i].x, bottom), ImVec2(run[i - 1].x, bottom), u32(rgb, red ? 0.16f : 0.10f));
        if (run.size() >= 2)
            dl->AddPolyline(run.data(), static_cast<int>(run.size()), u32(rgb), ImDrawFlags_None, 1.5f);
    };
    run.push_back(points.front());
    for (size_t i = 1; i < points.size(); ++i) {
        if ((values[i] < red_under) != red) {
            const float t = (red_under - values[i - 1]) / (values[i] - values[i - 1]);
            const ImVec2 cross(points[i - 1].x + (points[i].x - points[i - 1].x) * t, points[i - 1].y + (points[i].y - points[i - 1].y) * t);
            run.push_back(cross);
            flush();
            run.assign(1, cross);
            red = !red;
        }
        run.push_back(points[i]);
    }
    flush();
}

// The points as a line on a sunken box, the area under it washed, the top, middle and bottom values at the left and the
// latest large at the top right; `message` in the box instead when there's nothing to draw. The newest point is at the
// right: a full minute spans the box, and a shorter run grows from the right. The mouse on the box says what a point was.
void plot(const Metric& metric, const std::vector<float>& values, float height, const char* message)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 end(origin.x + width, origin.y + height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, end, u32(P::well), S(4));
    dl->AddRect(origin, end, u32(P::well_edge), S(4));
    ImGui::Dummy(ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    if (message != nullptr) {
        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(origin.x + S(16), origin.y + S(16)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - S(32));
        push_font(g_fonts.text, kSmall + 0.5f);
        ImGui::TextColored(vec4(P::dim), "%s", message);
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(after);
        return;
    }
    // the box's inside: room at the top for the latest value, a margin round the line
    const float left = origin.x + S(12), right = end.x - S(12), top = origin.y + S(36), bottom = end.y - S(12);
    float lowest = values.front(), highest = values.front();
    for (const float v : values)
        lowest = std::min(lowest, v), highest = std::max(highest, v);
    const float lo = metric.from_zero ? 0.0f : lowest;
    float hi = highest;
    if (hi - lo < 1e-6f)
        hi = lo + 1.0f;
    hi += (hi - lo) * 0.08f; // headroom over the highest
    const auto y_of = [&](float v) { return bottom - (bottom - top) * std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f); };
    // the grid: the top, the middle and the bottom, each with its value
    for (int i = 0; i < 3; ++i) {
        const float v = hi - (hi - lo) * static_cast<float>(i) * 0.5f;
        const float y = std::floor(y_of(v)) + 0.5f;
        dl->AddLine(ImVec2(left, y), ImVec2(right, y), u32(P::line));
        text_mid(dl, left + S(4), y - S(8), words(metric, v).c_str(), P::dim, g_fonts.text, kSmall - 2.0f);
    }
    // the line
    const int n = static_cast<int>(values.size());
    const float step = (right - left) / static_cast<float>(kSamples - 1);
    std::vector<ImVec2> points;
    points.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        points.emplace_back(right - step * static_cast<float>(n - 1 - i), y_of(values[static_cast<size_t>(i)]));
    line(dl, points, values, metric.red_under, bottom);
    dl->AddCircleFilled(points.back(), S(3), u32(values.back() < metric.red_under ? P::rose : P::accent2));
    // the latest, large, at the top right
    text_right_mid(dl, right, origin.y + S(19), words(metric, values.back()).c_str(), P::white, g_fonts.semi, kBig - 6.0f);
    // the mouse on the box: the point nearest it, and what it was
    if (hovered) {
        const float mx = ImGui::GetMousePos().x;
        const int i = std::clamp(static_cast<int>(std::floor((mx - points.front().x) / step + 0.5f)), 0, n - 1);
        const ImVec2& p = points[static_cast<size_t>(i)];
        dl->AddLine(ImVec2(p.x, top), ImVec2(p.x, bottom), u32(P::faint));
        dl->AddCircle(p, S(4), u32(P::white), 0, 1.5f);
        ImGui::SetTooltip("%s\n%.1f s ago", words(metric, values[static_cast<size_t>(i)]).c_str(), static_cast<double>(n - 1 - i) * kInterval);
    }
}

// A row of figures, each its name small and dim over its value (as the Graphics page's readings)
struct Figure {
    const char* name;
    std::string value;
};

void figures(const std::vector<Figure>& items)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, row = S(44);
    const float cw = std::floor(width / static_cast<float>(items.size()));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = at.x;
    for (const Figure& f : items) {
        text_mid(dl, x, at.y + S(10), f.name, P::dim, g_fonts.text, kSmall - 1.0f);
        text_mid(dl, x, at.y + S(30), f.value.c_str(), P::text, g_fonts.semi, kText);
        x += cw;
    }
    ImGui::Dummy(ImVec2(width, row));
}

} // namespace

void install(const wchar_t* install_dir)
{
    g_logs = install_dir != nullptr ? install_dir : L"";
    if (!g_logs.empty() && g_logs.back() != L'\\')
        g_logs += L'\\';
    g_logs += L"logs\\";
    QueryPerformanceFrequency(&g_frequency);
    start_walker();
}

AddressSpace address_space()
{
    AddressSpace out;
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) {
        out.total_mb = static_cast<double>(status.ullTotalVirtual) / (1024.0 * 1024.0);
        out.used_mb = static_cast<double>(status.ullTotalVirtual - status.ullAvailVirtual) / (1024.0 * 1024.0);
    }
    out.largest_free_mb = static_cast<double>(g_largest_free.load()) / (1024.0 * 1024.0);
    return out;
}

void on_frame()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_last_frame.QuadPart != 0) {
        const double ms = static_cast<double>(now.QuadPart - g_last_frame.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
        ++g_acc.frames;
        g_acc.frame_sum += ms;
        g_acc.worst = std::max(g_acc.worst, ms);
    }
    g_last_frame = now;
    const LONGLONG tick = static_cast<LONGLONG>(kSubInterval * static_cast<double>(g_frequency.QuadPart));
    if (g_window_start.QuadPart == 0) {
        g_window_start = now;
        g_sample_due.QuadPart = now.QuadPart + tick;
        return;
    }
    if (now.QuadPart < g_sample_due.QuadPart)
        return;
    const double seconds = static_cast<double>(now.QuadPart - g_window_start.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    close_sample(seconds, now);
    // the next is due 100 ms after this one was, not after this frame (each would run a part of a frame over, and five
    // of them past the point's 500 ms); from now when a frame took longer than a sample
    g_sample_due.QuadPart += tick;
    if (g_sample_due.QuadPart <= now.QuadPart)
        g_sample_due.QuadPart = now.QuadPart + tick;
}

void draw()
{
    if (!g_open || !gui::in_frame())
        return;
    ImGui::SetNextWindowSize(ImVec2(620.0f, 440.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40.0f, 140.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    push_theme();
    const bool shown = begin_window("VanaCore graph");
    if (!shown) {
        end_window(false);
        pop_theme();
        return;
    }
    title_bar(kIconGraph, "Graph", nullptr, open);

    // which metric: the drop-down, the width of the window
    const float row_x = ImGui::GetCursorScreenPos().x, row_w = ImGui::GetContentRegionAvail().x;
    metric_picker(std::max(S(120), row_w));

    const Metric& metric = kMetrics[g_selected];
    const std::vector<float> values = series(g_selected);

    push_font(g_fonts.text, kSmall);
    ImGui::TextDisabled("The last minute, a point every 0.5 s (the average of five 100 ms samples)");
    if (g_paused) {
        ImGui::SameLine(0.0f, S(8));
        ImGui::TextColored(vec4(P::amber), "Paused");
    }
    ImGui::PopFont();

    const char* message = values.empty() ? "Collecting..." : nullptr;

    // the plot takes what's left over the figures and the bottom row
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float figures_h = message == nullptr ? S(44) + spacing : 0.0f, bottom_h = S(30) + spacing;
    const float plot_h = std::max(S(110), ImGui::GetContentRegionAvail().y - figures_h - bottom_h - spacing);
    plot(metric, values, plot_h, message);

    if (message == nullptr) {
        const float latest = values.back();
        float lowest = values.front(), highest = values.front(), total = 0.0f;
        for (const float v : values)
            lowest = std::min(lowest, v), highest = std::max(highest, v), total += v;
        const float average = total / static_cast<float>(values.size());
        std::vector<Figure> items = {{"Now", words(metric, latest)},
                                     {"Average", words(metric, average)},
                                     {"Lowest", words(metric, lowest)},
                                     {"Highest", words(metric, highest)}};
        if (metric.time) {
            items.push_back({"95% of points under", words(metric, percentile(values, 0.95))});
            items.push_back({"99% under", words(metric, percentile(values, 0.99))});
        } else if (g_selected == kAddress || g_selected == kFreeBlock) {
            items.push_back({"All of it", words(metric, address_space().total_mb)});
        } else if (g_selected == kFps) {
            std::vector<float> sorted = values;
            std::sort(sorted.begin(), sorted.end());
            const size_t n = std::max<size_t>(1, sorted.size() / 20);
            float sum = 0.0f;
            for (size_t i = 0; i < n; ++i)
                sum += sorted[i];
            items.push_back({"Slowest 5% of points", words(metric, sum / static_cast<float>(n))});
        }
        figures(items);
    }

    // the bottom row: Pause (Resume, its icon amber, while it's paused), Save CSV and Clear at the right
    const float pause_w = std::max(button_width("Pause", kIconPause, 30.0f), button_width("Resume", kIconPlay, 30.0f));
    if (button("graph_pause", g_paused ? "Resume" : "Pause", g_paused ? kIconPlay : kIconPause, Kind::ghost, 30.0f, g_paused ? P::amber : 0, 1.0f, pause_w))
        g_paused = !g_paused;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", g_paused ? "Carries on from now" : "Keeps the points as they are, to read them");
    const float buttons_w = button_width("Save CSV", kIconSave, 30.0f) + S(8) + button_width("Clear", kIconTrash, 30.0f);
    ImGui::SameLine(0.0f, S(8));
    ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, row_x + row_w - buttons_w), ImGui::GetCursorScreenPos().y));
    if (button("graph_save", "Save CSV", kIconSave, Kind::ghost, 30.0f))
        save_and_say();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes the last minute, every metric, to logs\\graph-<date>-<time>.csv (/vana graph save)");
    ImGui::SameLine(0.0f, S(8));
    if (button("graph_clear", "Clear", kIconTrash, Kind::ghost, 30.0f)) {
        g_count = g_head = 0;
        g_acc = {};
        g_pending = {};
    }

    end_window(true);
    pop_theme();
    if (!open)
        g_open = false;
}

bool command(const std::vector<std::string>& parts)
{
    const std::string option = parts.size() > 1 ? parts[1] : "";
    if (option == "save") {
        save_and_say();
        return true;
    }
    if (option != "" && option != "on" && option != "off") {
        chat::echo("/vana graph [on|off|save]: a window of FPS, frame time and more over the last minute");
        return true;
    }
    g_open = option == "on" ? true : option == "off" ? false : !g_open; // no chat line: the window is the answer
    return true;
}

} // namespace vanacore::perf
