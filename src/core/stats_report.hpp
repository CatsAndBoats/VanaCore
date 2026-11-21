/* The report: print, hardware_line, minutes, print_timeline, print_matrix, matrix_csv, write_dip and save_report, what
 * Record Stats Report and /vana stats save write (the tree, the recording, the hardware), all of it in one .txt: the PC and
 * VanaCore, the frames, the lag spikes, the shadows, the settings, each its own section (players send it as it is: one file,
 * and no .ini, which Windows won't let a browser download). stats.cpp includes this inside its
 * namespace just after look, after Node, Window, Recording, Point, the profile and the gathering, whose names it uses, and
 * before the window, which uses its names. It is no file of its own. */
#pragma once

// ---- the report ---------------------------------------------------------------------------------------------------------

void print(std::string& out, const Node& n, int depth, double frame_ms)
{
    char line[256];
    if (n.counter)
        std::snprintf(line, sizeof line, "%*s%-60s %10.1f %s\n", depth * 2, "", n.name.c_str(), n.ms, n.unit.c_str());
    else if (n.calls >= 0.0)
        std::snprintf(line, sizeof line, "%*s%-60s %8.3f ms %6.1f%%  %8.1f calls\n", depth * 2, "", n.name.c_str(), n.ms,
                      frame_ms > 0 ? 100.0 * n.ms / frame_ms : 0.0, n.calls);
    else
        std::snprintf(line, sizeof line, "%*s%-60s %8.3f ms %6.1f%%\n", depth * 2, "", n.name.c_str(), n.ms,
                      frame_ms > 0 ? 100.0 * n.ms / frame_ms : 0.0);
    out += line;
    for (const Node& c : n.children)
        print(out, c, depth + 1, frame_ms);
}

std::string narrow(const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// What holds the CPU back, in words, "" nothing: the power plan when it caps the clock or turns boost off, else a limit
// Windows puts on a core below its own rated clock (on the battery, or the PC itself: heat, its firmware). A CPU with two
// kinds of core (Intel's P-cores and E-cores) has a rated clock for each, so the slower kind isn't held by being slower.
std::string cpu_held(const hwmon::Reading& r)
{
    if (!r.cpu)
        return "";
    if (r.cpu_plan_state < 100)
        return "held back by Windows' power plan: Maximum processor state " + std::to_string(r.cpu_plan_state) + "%";
    if (r.cpu_plan_mhz != 0)
        return "held back by Windows' power plan: Maximum processor frequency " + std::to_string(r.cpu_plan_mhz) + " MHz";
    if (r.cpu_plan_boost == 0)
        return "no boost: Windows' power plan has it off";
    if (r.cpu_limit_mhz != 0 && r.cpu_limit_mhz < r.cpu_max_mhz)
        return !r.power || r.ac ? "held below its base clock by the PC itself: heat, or its firmware" : "held below its base clock on the battery";
    return "";
}

// The CPU and GPU as they are now, in a line (hwmon.hpp)
std::string hardware_line()
{
    const hwmon::Reading r = hwmon::latest();
    char text[320];
    std::string out;
    if (r.power) {
        std::snprintf(text, sizeof text, "%s%s%s%s   ", r.ac ? "Plugged in" : "ON BATTERY", r.battery >= 0 ? (" " + std::to_string(r.battery) + "%").c_str() : "",
                      r.power_plan.empty() ? "" : (", Windows power plan: " + r.power_plan).c_str(),
                      r.power_mode.empty() ? "" : (", power mode: " + r.power_mode).c_str());
        out += text;
    }
    if (r.cpu) {
        const std::string held = cpu_held(r);
        if (r.cpu_live)
            std::snprintf(text, sizeof text, "CPU %.2f GHz (fastest core %.2f, base %.2f)%s%s", r.cpu_mhz / 1000.0, r.cpu_peak_mhz / 1000.0,
                          r.cpu_max_mhz / 1000.0, held.empty() ? "" : ", ", held.c_str());
        else // no counters (Windows 7), or their first second: only the rated clocks
            std::snprintf(text, sizeof text, "CPU base %.2f GHz%s%s", r.cpu_max_mhz / 1000.0, held.empty() ? "" : ", ", held.c_str());
        out += text;
    }
    if (r.cpu_load) {
        std::snprintf(text, sizeof text, "%s%.0f%% busy, the game %.0f%%", r.cpu ? ", " : "CPU ", r.cpu_busy, r.cpu_game);
        out += text;
    }
    if (r.ram_total_mb > 0) {
        std::snprintf(text, sizeof text, "%sMemory %.1f of %.1f GB, the game %.2f GB", out.empty() ? "" : "   ", r.ram_used_mb / 1024.0,
                      r.ram_total_mb / 1024.0, r.ram_game_mb / 1024.0);
        out += text;
    }
    if (r.gpu) {
        std::snprintf(text, sizeof text, "   GPU %u of %u MHz, %d C, %u of %u W, %u%% busy, video memory %.1f of %.1f GB%s%s", r.gpu_mhz, r.gpu_max_mhz,
                      r.gpu_temp, r.gpu_watts, r.gpu_limit_watts, r.gpu_busy, r.vram_used_mb / 1024.0, r.vram_total_mb / 1024.0,
                      r.throttle != 0 ? ", clocks held down by: " : "", r.throttle != 0 ? r.throttle_text.c_str() : "");
        out += text;
    } else if (!r.gpu_why.empty()) {
        out += "   GPU: " + r.gpu_why;
    }
    const perf::AddressSpace space = perf::address_space();
    if (space.total_mb > 0) {
        std::snprintf(text, sizeof text, "%sAddress space %.2f of %.1f GB, the largest block free %.0f MB", out.empty() ? "" : "   ",
                      space.used_mb / 1024.0, space.total_mb / 1024.0, space.largest_free_mb);
        out += text;
    }
    if (const std::string scene = graphics::scene_words(); !scene.empty())
        out += (out.empty() ? "" : "   ") + scene;
    return out.empty() ? "hardware: no readings yet" : out;
}

std::string minutes(double seconds, bool tenths = false)
{
    char text[32];
    const int whole = static_cast<int>(seconds / 60.0);
    if (tenths)
        std::snprintf(text, sizeof text, "%d:%04.1f", whole, seconds - whole * 60.0);
    else
        std::snprintf(text, sizeof text, "%d:%02d", whole, static_cast<int>(seconds) % 60);
    return text;
}

// The recording over time: a line a look
void print_timeline(std::string& out, const std::vector<Point>& points)
{
    out += "Over the recording, a line a look at the numbers (slowest: its slowest frame; slow: frames over twice the average; dips: frames under 15 fps)\n";
    out += "The percentages are each a part of the look's frame time. After them: what took the most of it, and where the thread was in a dip.\n";
    out += "      time     fps   frame ms   slowest ms   slow  dips   game%    d3d%  sleep%   vana%  limit%   GPU ms   CPU GHz   GPU MHz   GPU C\n";
    char line[384];
    for (const Point& p : points) {
        char cpu[16] = "        -", gpu[32] = "        -       -";
        if (p.cpu)
            std::snprintf(cpu, sizeof cpu, "%9.2f", p.cpu_mhz / 1000.0);
        if (p.gpu)
            std::snprintf(gpu, sizeof gpu, "%9u %7d", p.gpu_mhz, p.gpu_temp);
        std::snprintf(line, sizeof line, "  %8s  %6.1f  %9.2f  %11.2f  %5u  %4u  %6.1f  %6.1f  %6.1f  %6.1f  %6.1f  %7.2f %s %s%s", minutes(p.at, true).c_str(),
                      p.fps, p.frame_ms, p.worst_ms, p.slow, p.dips, p.game_pct, p.d3d_pct, p.sleep_pct, p.vana_pct, p.limit_pct, p.gpu_ms, cpu, gpu,
                      p.dips > 0 ? "  <<" : (p.slow > 0 ? "   <" : ""));
        out += line;
        if (!p.top.empty())
            out += "   " + p.top;
        if (!p.dip_in.empty())
            out += "   | dip: " + p.dip_in;
        out += "\n";
    }
}

// The recording's items in the order they're shown: each line's children after it, the biggest first (by their peak)
std::vector<int> item_order(const Recording& r)
{
    std::vector<std::vector<int>> kids(r.items.size() + 1); // [parent + 1]
    for (size_t i = 0; i < r.items.size(); ++i)
        kids[static_cast<size_t>(r.items[i].parent + 1)].push_back(static_cast<int>(i));
    for (auto& list : kids)
        std::stable_sort(list.begin(), list.end(), [&](int x, int y) { return r.items[static_cast<size_t>(x)].peak > r.items[static_cast<size_t>(y)].peak; });
    std::vector<int> order;
    const std::function<void(int)> walk = [&](int parent) {
        for (const int child : kids[static_cast<size_t>(parent + 1)]) {
            order.push_back(child);
            walk(child);
        }
    };
    walk(-1);
    return order;
}

int item_depth(const Recording& r, int index)
{
    int depth = 0;
    for (int p = r.items[static_cast<size_t>(index)].parent; p >= 0; p = r.items[static_cast<size_t>(p)].parent)
        ++depth;
    return depth;
}

// A table of the recording's items by look: its % of that look's frame time (a counter's count), NaN where the look didn't show it
std::vector<std::vector<float>> item_table(const Recording& r)
{
    std::vector<std::vector<float>> table(r.timeline.size(), std::vector<float>(r.items.size(), std::nanf("")));
    for (size_t t = 0; t < r.timeline.size(); ++t)
        for (const auto& [item, value] : r.timeline[t].items)
            table[t][static_cast<size_t>(item)] = value;
    return table;
}

// Every item the profiler shows, over the recording: a row an item, a column a look, its % of the look's frame time
// (the counters' own counts). Rows that never reach 1% are left out here; the report's csv section has all of them.
void print_matrix(std::string& out, const Recording& r)
{
    out += "Each item over the recording (the profiler's trees: the CPU, the GPU, the counters, the sampled thread), a column a look:\n";
    out += "its % of the look's frame time, a counter's own count; a blank is a look that didn't show it. Items that never reach 1% are in the csv.\n";
    const std::vector<std::vector<float>> table = item_table(r);
    char cell[32];
    std::string line = "  " + std::string(62, ' ');
    for (const Point& p : r.timeline) {
        std::snprintf(cell, sizeof cell, "%7s", minutes(p.at, true).c_str());
        line += cell;
    }
    out += line + "\n";
    line = "  " + std::string("fps");
    line.resize(64, ' ');
    for (const Point& p : r.timeline) {
        std::snprintf(cell, sizeof cell, "%7.1f", p.fps);
        line += cell;
    }
    out += line + "\n";
    line = "  " + std::string("slowest frame, ms");
    line.resize(64, ' ');
    for (const Point& p : r.timeline) {
        std::snprintf(cell, sizeof cell, "%7.0f", p.worst_ms);
        line += cell;
    }
    out += line + "\n";
    for (const int i : item_order(r)) {
        const Item& item = r.items[static_cast<size_t>(i)];
        if (!item.counter && item.peak < 1.0f)
            continue;
        std::string name = std::string(static_cast<size_t>(item_depth(r, i)) * 2, ' ') + item.name;
        if (name.size() > 62)
            name.resize(62);
        line = "  " + name;
        line.resize(64, ' ');
        for (size_t t = 0; t < table.size(); ++t) {
            const float v = table[t][static_cast<size_t>(i)];
            if (std::isnan(v))
                std::snprintf(cell, sizeof cell, "%7s", "");
            else if (item.counter)
                std::snprintf(cell, sizeof cell, "%7.0f", v);
            else
                std::snprintf(cell, sizeof cell, "%7.1f", v);
            line += cell;
        }
        out += line + "\n";
    }
}

// The same as csv, the report's own section: every item, its path, a column a look, the % of the look's frame time (a
// counter's count)
std::string matrix_csv(const Recording& r)
{
    const std::vector<std::vector<float>> table = item_table(r);
    std::string out;
    auto quoted = [](const std::string& text) {
        std::string q = "\"";
        for (const char c : text) {
            if (c == '"')
                q += '"';
            q += c;
        }
        return q + "\"";
    };
    char cell[32];
    out += "item,kind";
    for (const Point& p : r.timeline) {
        std::snprintf(cell, sizeof cell, ",%.1f", p.at);
        out += cell;
    }
    out += "\n";
    out += "fps,fps";
    for (const Point& p : r.timeline) {
        std::snprintf(cell, sizeof cell, ",%.1f", p.fps);
        out += cell;
    }
    out += "\nslowest frame,ms";
    for (const Point& p : r.timeline) {
        std::snprintf(cell, sizeof cell, ",%.1f", p.worst_ms);
        out += cell;
    }
    out += "\n";
    for (const int i : item_order(r)) {
        const Item& item = r.items[static_cast<size_t>(i)];
        std::string full = item.name;
        for (int p = item.parent; p >= 0; p = r.items[static_cast<size_t>(p)].parent)
            full = r.items[static_cast<size_t>(p)].name + " > " + full;
        out += quoted(full) + (item.counter ? ",count" : ",% of frame");
        for (size_t t = 0; t < table.size(); ++t) {
            const float v = table[t][static_cast<size_t>(i)];
            if (std::isnan(v)) {
                out += ",";
            } else {
                std::snprintf(cell, sizeof cell, ",%.3f", v);
                out += cell;
            }
        }
        out += "\n";
    }
    return out;
}

// A lag spike: what the look that held it was, written to the recording's dips file (one a recording, each added to it, as
// it happens, so a crash keeps them; the report takes the file into its Lag spikes section and deletes it)
void write_dip(const Window& w, const std::vector<sampler::Span>& dip, const Point& p)
{
    if (g_logs.empty())
        return;
    Recording& r = g_recording;
    const bool first = r.dips == 0;
    if (first)
        r.dips_file = L"stats-dips-" + log::file_stamp() + L".txt";
    const View v = build(w, "in this look");
    double dip_ms = 0.0, worst = 0.0;
    for (const sampler::Span& span : dip) {
        const double ms = static_cast<double>(span.end - span.start) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
        dip_ms += ms;
        worst = std::max(worst, ms);
    }
    std::string text;
    char head[600];
    if (first) {
        std::snprintf(head, sizeof head,
                      "VanaCore lag spikes: a snapshot of every look (half a second) that held a frame under 15 fps, from the recording started at %s.\n"
                      "Each has what the look's frame time went to (the CPU and GPU trees, with percentages) and, with the sampler, where the thread was in the dip frames alone.\n"
                      "At most one snapshot in three seconds. The recording's own report, written as it stops, has the whole recording and a line a look.\n\n",
                      r.started);
        text += head;
    }
    std::snprintf(head, sizeof head, "==== Spike %u, %s into the recording: %u frame(s) under 15 fps (%.0f ms together, worst %.0f ms); the look %.1f fps, %.1f ms a frame ====\n",
                  r.dips + 1, minutes(p.at, true).c_str(), static_cast<unsigned>(dip.size()), dip_ms, worst, p.fps, p.frame_ms);
    text += head;
    text += "now: " + hardware_line() + "\n";
    std::snprintf(head, sizeof head, "the look's frame time: game %.1f%%, Direct3D %.1f%%, sleeps %.1f%%, VanaCore %.1f%%, limit and Present %.1f%%\n", p.game_pct,
                  p.d3d_pct, p.sleep_pct, p.vana_pct, p.limit_pct);
    text += head;
    if (!p.top.empty())
        text += "the most of it: " + p.top + "\n";
    text += "\n";
    print(text, v.cpu, 0, v.frame_ms);
    if (v.gpu_seen) {
        text += "\n";
        print(text, v.gpu, 0, v.frame_ms);
    }
    if (!v.counters.children.empty()) {
        text += "\n";
        print(text, v.counters, 0, v.frame_ms);
    }
    const sampler::Result in = sampler::dip();
    if (in.samples > 0) {
        Node tree = sampled_tree(in, dip_ms / static_cast<double>(std::max<size_t>(dip.size(), 1)), false);
        char name[160];
        std::snprintf(name, sizeof name, "The dip frames alone, sampled (%u frame(s), %u samples)", static_cast<unsigned>(dip.size()), in.samples);
        tree.name = name;
        text += "\n";
        print(text, tree, 0, tree.ms);
    }
    const size_t from = r.timeline.size() > 8 ? r.timeline.size() - 8 : 0;
    const std::vector<Point> around(r.timeline.begin() + static_cast<std::ptrdiff_t>(from), r.timeline.end());
    text += "\nThe looks leading up to it:\n";
    print_timeline(text, around);
    text += "\n\n";

    CreateDirectoryW(g_logs.c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_logs + r.dips_file).c_str(), L"ab") != 0 || f == nullptr)
        return;
    if (first)
        std::fwrite("\xEF\xBB\xBF", 1, 3, f);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    ++r.dips;
}

std::string g_saved; // the last report written, "logs\stats-....txt", for the button's tooltip

std::string reg_text(const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[256];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return "";
    return narrow(buf);
}

// The report's first section: VanaCore, Windows (or Wine), the CPU, the renderer, the card and its driver, the upscaler
std::string pc_info()
{
    std::string out = "VanaCore " + narrow(vanacore::kVersion) + "\n";
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string os = reg_text(nt, L"ProductName");
    const std::string display = reg_text(nt, L"DisplayVersion"), build = reg_text(nt, L"CurrentBuild");
    if (const size_t at = os.find("Windows 10"); at != std::string::npos && std::atoi(build.c_str()) >= 22000)
        os.replace(at, 10, "Windows 11"); // (Windows 11 keeps Windows 10's ProductName)
    out += "Windows: " + (os.empty() ? std::string("unknown") : os) + (display.empty() ? "" : " " + display) + (build.empty() ? "" : ", build " + build);
    using WineVersion = const char*(__cdecl*)();
    if (const auto wine = reinterpret_cast<WineVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")))
        out += std::string(" (Wine ") + wine() + ")";
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const std::string cpu = reg_text(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    out += "\nCPU: " + (cpu.empty() ? std::string("unknown") : cpu) + ", " + std::to_string(si.dwNumberOfProcessors) + " threads\n";
    const graphics::Info g = graphics::info();
    out += "Renderer: " + g.renderer + (g.running != g.renderer ? " (running " + g.running + ")" : "") + "\n";
    out += "Graphics card: " + (g.adapter.empty() ? std::string("not known (the game's own Direct3D 8)") : g.adapter);
    if (!g.driver.empty())
        out += ", driver " + g.driver + (g.driver_version.empty() ? "" : " (" + g.driver_version + ")") +
               (g.driver_date.empty() ? "" : ", dated " + g.driver_date);
    out += "\nUpscaler: " + (g.upscaler.empty() ? std::string("off") : g.upscaler + ": " + (g.upscaler == "dlss" ? g.dlss : g.fsr)) + "\n";
    return out;
}

void section(std::string& text, const char* title)
{
    text += "\n\n---------------- ";
    text += title;
    text += " ----------------\n\n";
}

// A file the recording writes as it runs (the lag spikes, the shadows), for its section: its text, without a BOM
std::string read_log(const std::wstring& name)
{
    std::string out;
    if (name.empty())
        return out;
    const std::wstring path = g_logs + name;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO); // (the shadows' file is still open for writing while a recording runs)
    if (f == nullptr)
        return out;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        out.append(buf, n);
    std::fclose(f);
    if (out.rfind("\xEF\xBB\xBF", 0) == 0)
        out.erase(0, 3);
    return out;
}

// The recording's lag spikes and shadows sections, when the report has a recording
void recording_files(std::string& text)
{
    const Recording& r = g_recording;
    section(text, "Lag spikes");
    if (!r.folded_into.empty() && r.dips_file.empty() && r.shadows_done.empty()) {
        text += "(in the report saved as the recording stopped: logs\\" + narrow(r.folded_into) + ")\n";
        section(text, "Shadows, every frame (csv)");
        text += "(in the report saved as the recording stopped: logs\\" + narrow(r.folded_into) + ")\n";
        return;
    }
    const std::string dips = read_log(r.dips_file);
    text += dips.empty() ? "(none: no frame under 15 fps)\n" : dips;
    section(text, "Shadows, every frame (csv)");
    const std::string shadows = read_log(r.shadows_file.empty() ? r.shadows_done : r.shadows_file);
    text += shadows.empty() ? "(none: the shadows are recorded with Direct3D 12 only)\n" : shadows;
}

// The report is written and the recording over: its lag spikes' and shadows' files are in the report now, deleted
void drop_recording_files(const std::wstring& name)
{
    Recording& r = g_recording;
    if (sampler::running() || (r.dips_file.empty() && r.shadows_done.empty()))
        return;
    for (const std::wstring* file : {&r.dips_file, &r.shadows_done})
        if (!file->empty())
            DeleteFileW((g_logs + *file).c_str());
    r.dips_file.clear();
    r.shadows_done.clear();
    r.folded_into = name;
}

// The logs folder in Windows' Explorer, the report picked, for the player to send it: started, not waited for
void show_in_explorer(const std::wstring& path)
{
    std::wstring command = L"explorer.exe /select,\"" + path + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

bool save_report()
{
    const bool recorded = g_recording.have && g_recording.total.acc.frames > 0;
    if ((!g_have && !recorded) || g_logs.empty()) {
        chat::echo("stats: nothing collected yet");
        return false;
    }
    std::string text = pc_info();
    text += "hardware now: " + hardware_line() + "\n";
    section(text, "Frame data");
    char head[320];
    if (recorded) {
        // the recording: all of it, running or stopped
        const View v = build(g_recording.total, "in the recording");
        const bool running = sampler::running();
        std::snprintf(head, sizeof head, "VanaCore stats: a recording of %s%s, started at %s; frame %.2f ms (%.1f fps) averaged over it%s\n",
                      minutes(g_recording.total.seconds).c_str(), running ? " (still recording)" : "", g_recording.started, v.frame_ms, v.fps,
                      v.gpu_seen ? "" : "; no GPU timing (DirectX 12 only)");
        text += head;
        std::snprintf(head, sizeof head, "its slowest frame %.1f ms; %u slow frames (over twice the average round them)\n", g_recording.total.worst,
                      g_recording.total.slow);
        text += head;
        text += "\n";
        print(text, v.cpu, 0, v.frame_ms);
        if (v.gpu_seen) {
            text += "\n";
            print(text, v.gpu, 0, v.frame_ms);
        }
        if (!v.counters.children.empty()) {
            text += "\n";
            print(text, v.counters, 0, v.frame_ms);
        }
        Node all, slow;
        if (sampled_trees(all, slow)) {
            text += "\n";
            print(text, all, 0, all.ms);
            if (!slow.children.empty()) {
                text += "\n";
                print(text, slow, 0, slow.ms);
            }
        }
        if (!g_recording.timeline.empty()) {
            text += "\n";
            print_timeline(text, g_recording.timeline);
            text += "\n";
            print_matrix(text, g_recording);
        }
    } else {
        std::snprintf(head, sizeof head, "VanaCore stats: frame %.2f ms (%.1f fps)%s, averaged over %s; worked out at %s%s\n", g_frame_ms, g_fps,
                      g_gpu_seen ? "" : "; no GPU timing (DirectX 12 only)", window_words(), g_built_at, g_hold ? " (paused: these numbers are old)" : "");
        text += head;
        text += "\n";
        print(text, g_cpu, 0, g_frame_ms);
        if (g_gpu_seen) {
            text += "\n";
            print(text, g_gpu, 0, g_frame_ms);
        }
        if (!g_counters.children.empty()) {
            text += "\n";
            print(text, g_counters, 0, g_frame_ms);
        }
    }
    CreateDirectoryW(g_logs.c_str(), nullptr);
    const std::wstring name = L"stats-" + log::file_stamp() + L".txt";
    if (recorded && !g_recording.timeline.empty()) {
        section(text, "Every item, a column a look (csv: item, kind, then the look's % of frame time or count; the first rows are the looks' times in s, fps and slowest frame ms)");
        text += matrix_csv(g_recording);
    }
    if (recorded)
        recording_files(text);
    section(text, "Settings");
    text += settings_text(); // (stats_shadows.hpp: every setting in force, the defaults too)
    text = scrub_secrets(std::move(text)); // (stats_shadows.hpp: the login's values out of the whole report, whatever held them)
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_logs + name).c_str(), L"wb") != 0 || f == nullptr) {
        chat::echo("stats: the report couldn't be written");
        return false;
    }
    std::fwrite("\xEF\xBB\xBF", 1, 3, f);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    g_saved = "logs\\" + narrow(name);
    chat::echo("stats: saved " + g_saved + " (send this one file)");
    show_in_explorer(g_logs + name);
    if (recorded)
        drop_recording_files(name);
    return true;
}
