/* The timed trace's report (/vana trace <addon> <seconds>): report_text, what logs\trace-<addon>-<date>-<time>.txt holds, and
 * summary_text, its gist for the chat. addon_trace.cpp includes this inside its namespace, after the Session it reads and
 * before start_timed and finish_timed, which use it. It is no file of its own. */
#pragma once

// ---- the report ---------------------------------------------------------------------------------------------------------

void add(std::string& out, const char* text, ...)
{
    va_list args;
    va_start(args, text);
    out += format(text, args);
    va_end(args);
}

// The game's frames while the trace ran, added up
struct FrameSums {
    size_t count = 0;
    double sum = 0, average = 0, median = 0, p95 = 0, p99 = 0, slowest = 0;
    double addon = 0, addon_most = 0;                                      // the timed addon's Lua in them: in all, the most in one
    double direct3d = 0, sleeps = 0, vanacore = 0, limit = 0, present = 0; // the average frame's parts
    int addon_over_1ms = 0, addon_over_5ms = 0;                            // frames its Lua took over 1 ms, 5 ms in
    double hitch_line = 0;   // a hitch: a frame over twice the median and 10 ms over it
    int hitches = 0;
    int explained = 0;       // hitches its Lua took at least half the time over the median in
    double hitch_addon = 0, hitch_addon_most = 0;
};

FrameSums sum_frames(const std::vector<Frame>& frames)
{
    FrameSums f;
    f.count = frames.size();
    if (frames.empty())
        return f;
    std::vector<float> times;
    times.reserve(frames.size());
    for (const Frame& x : frames) {
        times.push_back(x.frame);
        f.sum += x.frame;
        f.addon += x.addon;
        f.addon_most = std::max(f.addon_most, static_cast<double>(x.addon));
        f.direct3d += x.direct3d, f.sleeps += x.sleeps, f.vanacore += x.vanacore, f.limit += x.limit, f.present += x.present;
        f.addon_over_1ms += x.addon > 1.0f ? 1 : 0;
        f.addon_over_5ms += x.addon > 5.0f ? 1 : 0;
    }
    const double n = static_cast<double>(frames.size());
    f.average = f.sum / n;
    f.direct3d /= n, f.sleeps /= n, f.vanacore /= n, f.limit /= n, f.present /= n;
    std::sort(times.begin(), times.end());
    const auto under = [&](double share) { return static_cast<double>(times[std::min(times.size() - 1, static_cast<size_t>(share * n))]); };
    f.median = under(0.5), f.p95 = under(0.95), f.p99 = under(0.99), f.slowest = times.back();
    f.hitch_line = std::max(f.median * 2.0, f.median + 10.0);
    for (const Frame& x : frames) {
        if (x.frame < f.hitch_line)
            continue;
        ++f.hitches;
        f.hitch_addon += x.addon;
        f.hitch_addon_most = std::max(f.hitch_addon_most, static_cast<double>(x.addon));
        f.explained += x.addon >= 0.5 * (x.frame - f.median) ? 1 : 0;
    }
    if (f.hitches > 0)
        f.hitch_addon /= f.hitches;
    return f;
}

// The Lua profile's report (addons::profile_stop): "T\t<samples>", "S\t<VM state>\t<samples>", "L\t<samples>\t<file:line>",
// "F\t<samples>\t<function>"
struct Sampled {
    std::string key;
    double samples = 0;
};
struct Samples {
    double total = 0;
    std::vector<Sampled> states, lines, functions;
    double share(const char* state) const
    {
        for (const Sampled& s : states)
            if (s.key == state)
                return total > 0 ? 100.0 * s.samples / total : 0.0;
        return 0.0;
    }
};

Samples read_samples(const std::string& report)
{
    Samples out;
    size_t at = 0;
    while (at < report.size()) {
        const size_t end = std::min(report.find('\n', at), report.size());
        std::vector<std::string> fields;
        for (size_t from = at; from <= end;) {
            const size_t tab = std::min(report.find('\t', from), end);
            fields.push_back(report.substr(from, tab - from));
            from = tab + 1;
        }
        at = end + 1;
        if (fields.size() == 2 && fields[0] == "T")
            out.total = std::atof(fields[1].c_str());
        else if (fields.size() == 3 && fields[0] == "S")
            out.states.push_back({fields[1], std::atof(fields[2].c_str())});
        else if (fields.size() == 3 && (fields[0] == "L" || fields[0] == "F"))
            (fields[0] == "L" ? out.lines : out.functions).push_back({fields[2], std::atof(fields[1].c_str())});
    }
    std::sort(out.states.begin(), out.states.end(), [](const Sampled& x, const Sampled& y) { return x.samples > y.samples; });
    return out;
}

const char* vm_state(const std::string& state)
{
    switch (state.empty() ? ' ' : state[0]) {
    case 'N': return "compiled";
    case 'I': return "interpreted";
    case 'C': return "C";
    case 'G': return "garbage collection";
    case 'J': return "the JIT compiling";
    default: return "other";
    }
}

int total_calls(const Session& s)
{
    int calls = 0;
    for (const auto& [name, e] : s.events)
        calls += e.calls;
    return calls;
}

// The event its Lua spent the most in (nullptr: none)
const std::pair<const std::string, EventTime>* busiest(const Session& s)
{
    const std::pair<const std::string, EventTime>* most = nullptr;
    for (const auto& e : s.events)
        if (most == nullptr || e.second.ms > most->second.ms)
            most = &e;
    return most;
}

// "What it found": how heavy it is, and the likely why
void findings_cost(std::string& out, const Session& s, const FrameSums& f, double length)
{
    const double frames = static_cast<double>(std::max<size_t>(f.count, 1));
    const double per_frame = f.addon / frames, share = f.sum > 0 ? 100.0 * f.addon / f.sum : 0.0;
    if (total_calls(s) == 0) {
        add(out, "- It made no calls in %.0f s: it isn't loaded, or nothing it listens for happened (/addon list).\n", length);
        return;
    }
    if (per_frame < 0.25 && f.addon_most < 5.0)
        add(out, "- Light: %.3f ms of a frame (%.1f%% of the frame time), never over %.1f ms in one frame. It doesn't look like what "
                 "slows the game; the frames below say what does.\n", per_frame, share, f.addon_most);
    else if (per_frame < 1.0)
        add(out, "- Moderate: %.3f ms of a frame (%.1f%% of the frame time), at most %.1f ms in one frame.\n", per_frame, share,
            f.addon_most);
    else
        add(out, "- Heavy: %.2f ms of every frame (%.1f%% of the frame time). Without it the game would run at about %.0f fps "
                 "instead of %.0f, if nothing else holds the frame rate.\n", per_frame, share,
            1000.0 / std::max(0.5, f.average - per_frame), f.average > 0 ? 1000.0 / f.average : 0.0);
    if (per_frame >= 0.25 && f.limit > per_frame)
        add(out, "- The frame rate limit waited %.2f ms a frame, more than its Lua takes: with the limit on, it doesn't lower "
                 "the frame rate yet.\n", f.limit);
    if (f.hitches > 0 && f.explained > 0)
        add(out, "- %d of the %d hitches (frames over %.0f ms) came with its Lua taking at least half the time the frame went "
                 "over: it caused those (its slowest calls below say in which events).\n", f.explained, f.hitches, f.hitch_line);
    else if (f.hitches > 0)
        add(out, "- None of the %d hitches (frames over %.0f ms) came from its Lua (at most %.2f ms in one): the slowest frames "
                 "below show where their time went.\n", f.hitches, f.hitch_line, f.hitch_addon_most);
    const auto* most = busiest(s);
    double all = 0;
    for (const auto& [name, e] : s.events)
        all += e.ms;
    if (most != nullptr && all > 0 && most->second.ms >= 0.5 * all && s.events.size() > 1)
        add(out, "- Most of its time (%.0f%%) is its \"%s\" handlers: %.1f calls a second, %.3f ms each on average, %.2f at most.\n",
            100.0 * most->second.ms / all, most->first.c_str(), most->second.calls / std::max(length, 0.001),
            most->second.ms / most->second.calls, most->second.most);
}

void findings_lua(std::string& out, const Session& s, const Samples& p, double length)
{
    const double made = s.kb_made / std::max(length, 0.001);
    if (p.total >= 50 && p.share("I") >= 40.0)
        add(out, "- %.0f%% of its Lua ran interpreted, not compiled: LuaJIT falls back to its interpreter for code it can't "
                 "compile (some library functions, or a loop it gave up on). Its top lines below say where.\n", p.share("I"));
    if ((p.total >= 50 && p.share("G") >= 10.0) || made >= 1024.0)
        add(out, "- Garbage: it made at least %.0f KB a second, and garbage collection took %.0f%% of its profile. New tables, "
                 "strings or closures on every call (strings built up, tables made in a loop) are the usual cause.\n", made,
            p.share("G"));
    if (p.total >= 50 && p.share("C") >= 50.0)
        add(out, "- %.0f%% of its time is in C: the API functions it calls (VanaCore's, Ashita's or Windower's), libraries or "
                 "FFI. Its top functions below say which.\n", p.share("C"));
    if (s.memory_seen && s.kb_end - s.kb_start >= std::max(1024.0, 0.25 * s.kb_start))
        add(out, "- Its Lua memory grew %.0f KB in %.0f s (%.0f to %.0f KB): it keeps more and more (a table added to and never "
                 "cleared), or its garbage collection is behind.\n", s.kb_end - s.kb_start, length, s.kb_start, s.kb_end);
    for (const auto& [title, vertices] : s.windows)
        if (vertices >= 20000)
            add(out, "- Its window \"%s\" drew %d vertices in its last frame: a lot for ImGui (a long list drawn whole costs "
                     "every frame; ImGuiListClipper draws only the rows shown).\n", title.c_str(), vertices);
    if (!s.profiling)
        add(out, "- No Lua profile: %s.\n", s.profile_error.empty() ? "it couldn't be started" : s.profile_error.c_str());
}

void summary_section(std::string& out, const Session& s, const FrameSums& f, const Samples& p, double length)
{
    const double frames = static_cast<double>(std::max<size_t>(f.count, 1));
    const int calls = total_calls(s);
    out += "\nSummary\n-------\n";
    add(out, "Its Lua: %.3f ms a frame on average (%.2f%% of the frame time), %.1f ms a second; at most %.2f ms in one frame. "
             "%d calls (%.1f a second).\n", f.addon / frames, f.sum > 0 ? 100.0 * f.addon / f.sum : 0.0,
        f.addon / std::max(length, 0.001), f.addon_most, calls, calls / std::max(length, 0.001));
    if (f.count > 0)
        add(out, "The game: %zu frames, %.1f fps. Frame time: %.2f ms on average, median %.2f, 95%% under %.2f, 99%% under %.2f "
                 "(the 1%% low), slowest %.2f.\n", f.count, 1000.0 * frames / std::max(f.sum, 0.001), f.average, f.median, f.p95,
            f.p99, f.slowest);
    else
        out += "The game: no frames timed (the frame timing's watchers were all taken, or the game drew nothing).\n";
    if (f.hitches > 0)
        add(out, "Hitches (frames over %.1f ms: twice the median and 10 ms over it): %d. Its Lua took %.2f ms in them on average, "
                 "%.2f at most; in %d of them, at least half the time the frame went over.\n", f.hitch_line, f.hitches, f.hitch_addon,
            f.hitch_addon_most, f.explained);
    if (s.memory_seen)
        add(out, "Its Lua memory: %.0f KB at the start, %.0f at the end, %.0f at most. It made at least %.0f KB of garbage (%.0f KB "
                 "a second); its garbage collector ran in %d calls, freeing %.0f KB.\n", s.kb_start, s.kb_end, s.kb_peak, s.kb_made,
            s.kb_made / std::max(length, 0.001), s.gc_calls, s.kb_freed);
    if (p.total > 0) {
        std::string states;
        for (const Sampled& st : p.states)
            add(states, "%s%s %.0f%%", states.empty() ? "" : ", ", vm_state(st.key), 100.0 * st.samples / p.total);
        add(out, "Its Lua profile: %.0f samples (about a millisecond each of its Lua running): %s.\n", p.total, states.c_str());
    }
}

void events_section(std::string& out, const Session& s, const FrameSums& f, double length)
{
    std::vector<const std::pair<const std::string, EventTime>*> order;
    for (const auto& e : s.events)
        order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const auto* x, const auto* y) { return x->second.ms > y->second.ms; });
    out += "\nIts calls by event (\"frame\": its frame and tasks)\n-------------------------------------------------\n";
    add(out, "%-24s %8s %9s %10s %11s %9s %9s %10s %10s\n", "event", "calls", "a second", "total ms", "ms a frame", "average",
        "most", "over 1 ms", "over 5 ms");
    const double frames = static_cast<double>(std::max<size_t>(f.count, 1));
    for (const auto* e : order)
        add(out, "%-24s %8d %9.1f %10.2f %11.4f %9.4f %9.3f %10d %10d\n", e->first.c_str(), e->second.calls,
            e->second.calls / std::max(length, 0.001), e->second.ms, e->second.ms / frames, e->second.ms / e->second.calls,
            e->second.most, e->second.over_1ms, e->second.over_5ms);
    if (order.empty())
        out += "(none)\n";
    out += "\nIts slowest calls\n-----------------\n";
    add(out, "%8s  %-24s %9s  %s\n", "at s", "event", "ms", "its Lua memory over the call");
    for (const Call& c : s.slowest)
        add(out, "%8.2f  %-24s %9.3f  %+.1f KB%s\n", c.at, c.event.c_str(), c.ms, c.kb, c.kb < 0 ? " (its garbage collector ran)" : "");
}

void frames_section(std::string& out, const Session& s, const FrameSums& f)
{
    if (f.count == 0)
        return;
    out += "\nThe game's frames\n-----------------\n";
    const double rest = std::max(0.0, f.average - f.direct3d - f.sleeps - f.vanacore - f.limit - f.present);
    add(out, "The average frame, %.2f ms: the game's Direct3D calls %.2f, its sleeps %.2f, VanaCore %.2f (addons, plugins, its "
             "interface), the frame rate limit %.2f, Present %.2f, the rest (the game's own work, packets) %.2f. This addon's Lua: "
             "%.3f.\n", f.average, f.direct3d, f.sleeps, f.vanacore, f.limit, f.present, rest, f.addon / static_cast<double>(f.count));
    add(out, "Frames its Lua took over 1 ms in: %d (%.1f%%); over 5 ms: %d.\n", f.addon_over_1ms,
        100.0 * f.addon_over_1ms / static_cast<double>(f.count), f.addon_over_5ms);
    std::vector<const Frame*> worst;
    for (const Frame& x : s.frames)
        worst.push_back(&x);
    const size_t shown = std::min<size_t>(worst.size(), 15);
    std::partial_sort(worst.begin(), worst.begin() + static_cast<std::ptrdiff_t>(shown), worst.end(),
                      [](const Frame* x, const Frame* y) { return x->frame > y->frame; });
    out += "\nThe slowest frames (ms)\n";
    add(out, "%8s %9s %9s  %-28s %9s %8s %9s %8s %8s %9s  %s\n", "at s", "frame", "its Lua", "its slowest call", "Direct3D", "sleeps",
        "VanaCore", "limit", "Present", "the rest", "the slowest Direct3D call");
    for (size_t i = 0; i < shown; ++i) {
        const Frame& x = *worst[i];
        char call[64] = "";
        if (x.top_event >= 0 && static_cast<size_t>(x.top_event) < s.names.size())
            snprintf(call, sizeof(call), "%.2f %s", x.top, s.names[static_cast<size_t>(x.top_event)].c_str());
        const double other = std::max(0.0, static_cast<double>(x.frame - x.direct3d - x.sleeps - x.vanacore - x.limit - x.present));
        add(out, "%8.2f %9.2f %9.3f  %-28s %9.2f %8.2f %9.2f %8.2f %8.2f %9.2f  %.2f %s\n", x.at, x.frame, x.addon, call, x.direct3d,
            x.sleeps, x.vanacore, x.limit, x.present, other, x.slowest, x.slowest_name);
    }
}

void shares_section(std::string& out, const Session& s, const FrameSums& f)
{
    std::vector<const std::pair<const std::string, Share>*> order;
    Share all;
    for (const auto& e : s.shares) {
        order.push_back(&e);
        all.calls += e.second.calls;
        all.ms += e.second.ms;
    }
    std::sort(order.begin(), order.end(), [](const auto* x, const auto* y) { return x->second.ms > y->second.ms; });
    const double frames = static_cast<double>(std::max<size_t>(f.count, 1));
    out += "\nEvery addon's Lua while it ran\n------------------------------\n";
    add(out, "%-24s %9s %12s %13s %13s %15s\n", "addon", "calls", "ms a frame", "of the frame", "slowest call", "most in a frame");
    for (const auto* e : order)
        add(out, "%-24s %9d %12.4f %12.2f%% %13.3f %15.3f\n", e->first.c_str(), e->second.calls, e->second.ms / frames,
            f.sum > 0 ? 100.0 * e->second.ms / f.sum : 0.0, e->second.most_call, e->second.most_frame);
    add(out, "%-24s %9d %12.4f %12.2f%%\n", "(all of them)", all.calls, all.ms / frames, f.sum > 0 ? 100.0 * all.ms / f.sum : 0.0);
}

void profile_section(std::string& out, const Samples& p)
{
    if (p.total <= 0)
        return;
    out += "\nIts Lua profile (LuaJIT's jit.profile, a sample each millisecond its Lua ran)\n"
           "---------------------------------------------------------------------------\n";
    const auto list = [&](const char* heading, const std::vector<Sampled>& rows) {
        add(out, "%s\n%9s %7s  %s\n", heading, "samples", "share", "where");
        for (const Sampled& r : rows)
            add(out, "%9.0f %6.1f%%  %s\n", r.samples, 100.0 * r.samples / p.total, r.key.c_str());
    };
    list("The top lines:", p.lines);
    out += "\n";
    list("The top functions:", p.functions);
}

void activity_section(std::string& out, const Session& s)
{
    const auto ids = [](const std::map<std::uint16_t, int>& blocked) {
        std::string text;
        for (const auto& [id, count] : blocked)
            add(text, "%s0x%03X x%d", text.empty() ? "" : ", ", id, count);
        return text.empty() ? std::string("none") : text;
    };
    out += "\nWhat it did\n-----------\n";
    add(out, "Packets it was given: received %d (it blocked: %s), sent %d (it blocked: %s). Packets it added: received %d, sent %d.\n",
        s.packets_in, ids(s.blocked_in).c_str(), s.packets_out, ids(s.blocked_out).c_str(), s.added_in, s.added_out);
    add(out, "Chat lines it wrote: %d. Commands that reached it: %d. ImGui windows it began: %d.\n", s.chat, s.commands, s.begins);
    for (const auto& [title, vertices] : s.windows)
        add(out, "Its window \"%s\": %d vertices in its last frame.\n", title.c_str(), std::max(vertices, 0));
}

void hardware_section(std::string& out, const hwmon::Reading& h)
{
    out += "\nThe hardware at the end\n-----------------------\n";
    if (h.cpu)
        add(out, "CPU: %.0f%% busy (this game %.0f%%), %u MHz (rated %u, Windows lets it run at %u); power plan \"%s\"%s.\n",
            h.cpu_busy, h.cpu_game, h.cpu_mhz, h.cpu_max_mhz, h.cpu_limit_mhz, h.power_plan.c_str(),
            h.power && !h.ac ? ", on battery" : "");
    add(out, "Memory: this game %.0f MB; the PC %.0f of %.0f MB in use.\n", h.ram_game_mb, h.ram_used_mb, h.ram_total_mb);
    if (h.gpu)
        add(out, "GPU: %u%% busy, %u of %u MHz, %d C, %u of %u W; its memory %.0f of %.0f MB%s%s.\n", h.gpu_busy, h.gpu_mhz,
            h.gpu_max_mhz, h.gpu_temp, h.gpu_watts, h.gpu_limit_watts, h.vram_used_mb, h.vram_total_mb,
            h.throttled ? "; held down: " : "", h.throttled ? h.throttle_text.c_str() : "");
    else
        add(out, "GPU: no reading%s%s.\n", h.gpu_why.empty() ? "" : ": ", h.gpu_why.c_str());
}

std::string report_text(const Session& s, double length, const char* why, const std::string& profile, const hwmon::Reading& hardware)
{
    const FrameSums f = sum_frames(s.frames);
    const Samples p = read_samples(profile);
    std::string out;
    add(out, "VanaCore %ls: a timed trace of %s, %.1f s from %s (asked for %d s%s%s)\n", kVersion, s.addon.c_str(), length,
        s.started.c_str(), s.seconds, why != nullptr && *why != '\0' ? "; " : "", why != nullptr ? why : "");
    out += "\nWhat it found\n-------------\n";
    findings_cost(out, s, f, length);
    findings_lua(out, s, p, length);
    summary_section(out, s, f, p, length);
    events_section(out, s, f, length);
    frames_section(out, s, f);
    shares_section(out, s, f);
    profile_section(out, p);
    activity_section(out, s);
    hardware_section(out, hardware);
    out += "\nWhile it ran, the frames' parts and every addon's calls were timed: that costs a little, so the frame rate here can "
           "be a touch under the usual.\n";
    return out;
}

// The gist, for the chat and the log
std::vector<std::string> summary_text(const Session& s, double length, const char* why, const std::string& where)
{
    const FrameSums f = sum_frames(s.frames);
    const double frames = static_cast<double>(std::max<size_t>(f.count, 1));
    std::vector<std::string> lines;
    std::string first;
    add(first, "trace %s (%.0f s%s%s): ", s.addon.c_str(), length, why != nullptr && *why != '\0' ? ", " : "", why != nullptr ? why : "");
    if (total_calls(s) == 0)
        add(first, "no calls into its Lua (is it loaded?)");
    else
        add(first, "its Lua %.3f ms a frame (%.1f%% of the frame time), at most %.2f ms in a frame; slowest call %.2f ms (%s)",
            f.addon / frames, f.sum > 0 ? 100.0 * f.addon / f.sum : 0.0, f.addon_most, s.slowest.empty() ? 0.0 : s.slowest[0].ms,
            s.slowest.empty() ? "-" : s.slowest[0].event.c_str());
    lines.push_back(first);
    if (f.count > 0) {
        std::string second;
        add(second, "trace: the game %.0f fps, 1%% low %.1f ms, %d hitches (%d with its Lua the cause)", 1000.0 * frames / std::max(f.sum, 0.001),
            f.p99, f.hitches, f.explained);
        lines.push_back(second);
    }
    lines.push_back(where.empty() ? "trace: the report couldn't be written to logs\\" : "trace: the report is " + where);
    return lines;
}
