#include "fightwatch.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "game.hpp"
#include "log.hpp"
#include "screenshot.hpp"
#include "settings.hpp"

namespace vanacore::fightwatch {
namespace {

constexpr unsigned kMonsters = 0x400;     // the target indexes monsters have
constexpr float kNear = 50.0f * 50.0f;    // squared yalms (entity distance's units)
constexpr ULONGLONG kScanIdle = 1000;     // ms between looks for an Aern while there's none
constexpr ULONGLONG kScanActive = 500;    // ... and while one is near
constexpr ULONGLONG kGone = 15000;        // ms without one near before the watch ends
constexpr ULONGLONG kHeartbeat = 3000;    // ms between lighting lines when nothing changed
constexpr ULONGLONG kChangeGap = 250;     // ms at least between lighting lines for a change
constexpr ULONGLONG kSceneEvery = 2000;   // ms between looks at the scene's colour
constexpr int kMostLines = 800;
constexpr unsigned kStep = 8;             // the scene sampled every 8th pixel each way

struct Aern {
    bool found = false;
    unsigned index = 0;
    std::uint32_t id = 0;
    std::string name;
    float yalms = 0.0f;
    unsigned hpp = 0, status = 0;
};

struct State {
    bool active = false;
    ULONGLONG next_scan = 0, last_seen = 0, started = 0;
    Aern aern;                  // the one last logged
    VanaGfxFight logged{};      // the lighting last logged
    bool have_logged = false;
    ULONGLONG logged_at = 0;
    ULONGLONG next_scene = 0;
    bool scene_pending = false;
    float luma_low = 2.0f;      // the fight's darkest scene mean so far
    bool washed = false;        // the scene is washed out now
    bool caught = false;        // this fight's washout has had its screenshot and draw log
    unsigned washouts = 0;
    std::uint32_t lights_max = 0;
    float lit_top = 0.0f;
};

State g;
int g_lines = 0;

bool line(const wchar_t* format, ...)
{
    if (g_lines >= kMostLines)
        return false;
    if (++g_lines == kMostLines) {
        log::write(L"Fight watch: %d lines, no more this session", kMostLines);
        return false;
    }
    wchar_t text[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    log::write(L"Fight watch: %s", text);
    return true;
}

bool is_aern(const std::string& name)
{
    if (name.size() < 6)
        return false;
    std::string tail = name.substr(name.size() - 5);
    for (char& c : tail)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == "'aern";
}

// The nearest live Aern within 50 yalms
Aern nearest_aern()
{
    Aern best;
    const unsigned count = std::min(game::entity_count(), kMonsters);
    game::Entity e;
    for (unsigned i = 1; i < count; ++i) {
        if (!game::entity(i, e) || !(e.spawn_flags & 0x10) || e.hpp == 0 || e.distance <= 0.0f || e.distance > kNear)
            continue;
        if (!is_aern(e.name))
            continue;
        const float yalms = std::sqrt(e.distance);
        if (best.found && yalms >= best.yalms)
            continue;
        best.found = true, best.index = e.index, best.id = e.id, best.name = e.name, best.yalms = yalms;
        best.hpp = e.hpp, best.status = e.status;
    }
    return best;
}

std::wstring logs_dir()
{
    std::wstring dir = settings::shared();
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
    return dir + L"logs\\";
}

// The section's keys as the session's copy has them, a line for each ~400 characters
void log_section(const wchar_t* section)
{
    const std::wstring& ini = settings::copy().empty() ? settings::file() : settings::copy();
    std::vector<wchar_t> buffer(32768, 0);
    const DWORD n = GetPrivateProfileSectionW(section, buffer.data(), static_cast<DWORD>(buffer.size()), ini.c_str());
    std::wstring keys;
    for (const wchar_t* p = buffer.data(); p < buffer.data() + n && *p; p += wcslen(p) + 1) {
        keys += keys.empty() ? L"" : L" ";
        keys += p;
        if (keys.size() > 400)
            line(L"[%s] %s", section, keys.c_str()), keys.clear();
    }
    if (!keys.empty() || n == 0)
        line(L"[%s] %s", section, keys.empty() ? L"(none set: VanaCore's defaults)" : keys.c_str());
}

void log_start(const Aern& a)
{
    game::PartyMember members[18];
    game::Alliance alliance;
    const unsigned zone = game::party(members, alliance) ? members[0].zone : 0;
    line(L"%hs (index %u, id %u) %.1f yalms away, HP %u%%, in zone %u: the lighting and the scene logged until it's gone",
         a.name.c_str(), a.index, a.id, a.yalms, a.hpp, zone);
    char message[512] = {};
    const int dlss = vanacore_gfx_dlss_state(message, sizeof message);
    line(L"renderer %hs; upscaler state %d (0 off, 1 starting, 2 on, 3 not here, 4 failed) %hs", vanacore_gfx_d3d11() ? "d3d11" : "d3d12",
         dlss, message);
    message[0] = 0;
    const int fg = vanacore_gfx_framegen_state(message, sizeof message);
    line(L"frame generation state %d %hs; card %hs", fg, message, vanacore_gfx_adapter());
    log_section(L"graphics");
    log_section(L"graphics.effects");
}

std::wstring rgb(const float* c)
{
    wchar_t text[64];
    swprintf_s(text, L"%.2f %.2f %.2f", c[0], c[1], c[2]);
    return text;
}

float top3(const float* c) { return std::max(c[0], std::max(c[1], c[2])); }

// The lights, most used first: all with full = true, else the six most used and any brighter than white
std::wstring lights_text(const VanaGfxFight& f, bool full)
{
    std::vector<const VanaGfxFightLight*> order;
    for (std::uint32_t i = 0; i < f.nlights; ++i)
        order.push_back(&f.light[i]);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return a->draws > b->draws; });
    std::wstring text;
    wchar_t one[256];
    for (size_t k = 0; k < order.size(); ++k) {
        const VanaGfxFightLight& l = *order[k];
        if (!full && k >= 6 && top3(l.diffuse) <= 1.0f && top3(l.ambient) <= 1.0f)
            continue;
        if (l.type == 3)
            swprintf_s(one, L"; sun-like d %s a %s x%u (chars %u)", rgb(l.diffuse).c_str(), rgb(l.ambient).c_str(), l.draws,
                       l.char_draws);
        else
            swprintf_s(one, L"; %s d %s a %s range %.1f att %g %g %g nearest %.1f x%u (chars %u)", l.type == 1 ? L"point" : L"spot",
                       rgb(l.diffuse).c_str(), rgb(l.ambient).c_str(), l.range, l.att[0], l.att[1], l.att[2], l.nearest, l.draws,
                       l.char_draws);
        text += one;
    }
    if (f.lights_dropped)
        text += L"; " + std::to_wstring(f.lights_dropped) + L" more not kept";
    return text;
}

void log_lighting(const VanaGfxFight& f, const wchar_t* why, bool full)
{
    line(L"%s: frame %u, %u lit draws, their light up to %.2f (%s), NaN/inf %u, material emissive up to %.2f, diffuse %.2f | "
         L"ambient %s | fog %s %s %.0f-%.0f | sun %s %s | %u lights%s",
         why, f.frame, f.lit_draws, f.lit_max, f.lit_max_char ? L"a character" : L"the world", f.nonfinite, f.emissive_max,
         f.diffuse_max, rgb(f.ambient).c_str(), f.fog_on ? L"on" : L"off", rgb(f.fog_color).c_str(), f.fog_start, f.fog_end,
         f.sun_on ? L"on" : L"none", rgb(f.sun_color).c_str(), f.nlights, lights_text(f, full).c_str());
}

bool differs(const float* a, const float* b, float by)
{
    return std::fabs(a[0] - b[0]) > by || std::fabs(a[1] - b[1]) > by || std::fabs(a[2] - b[2]) > by;
}

int band(float lit) { return lit < 1.5f ? 0 : lit < 3.0f ? 1 : lit < 8.0f ? 2 : 3; }

// What's changed since the line before, worth a line of its own ("" for nothing)
const wchar_t* change(const VanaGfxFight& f, const VanaGfxFight& was)
{
    if (f.fog_on != was.fog_on || differs(f.fog_color, was.fog_color, 0.08f))
        return L"the fog changed";
    const float end_was = std::max(was.fog_end, 1.0f);
    if (f.fog_on && std::fabs(f.fog_end - was.fog_end) > 0.25f * end_was)
        return L"the fog's distance changed";
    if (differs(f.ambient, was.ambient, 0.08f))
        return L"the ambient changed";
    if (f.sun_on != was.sun_on || differs(f.sun_color, was.sun_color, 0.1f))
        return L"the sun changed";
    if (band(f.lit_max) != band(was.lit_max))
        return L"the brightest light changed";
    if ((f.nonfinite != 0) != (was.nonfinite != 0))
        return L"a light went NaN or back";
    if (std::fabs(f.emissive_max - was.emissive_max) > 0.25f)
        return L"the emissive changed";
    return L"";
}

void watch_lighting(ULONGLONG now)
{
    VanaGfxFight f{};
    if (!vanacore_gfx_fight_frame(&f))
        return;
    g.lights_max = std::max(g.lights_max, f.nlights);
    g.lit_top = std::max(g.lit_top, f.lit_max);
    const wchar_t* why = g.have_logged ? change(f, g.logged) : L"the lighting";
    const bool beat = now - g.logged_at >= kHeartbeat;
    if (!*why && !beat)
        return;
    if (*why && !beat && now - g.logged_at < kChangeGap)
        return;
    log_lighting(f, *why ? why : L"the lighting", false);
    g.logged = f, g.have_logged = true, g.logged_at = now;
}

void caught(float r, float gr, float b, float white)
{
    const std::wstring stamp = log::file_stamp();
    const std::wstring draws = logs_dir() + L"fightwatch-" + stamp + L"-draws.txt";
    line(L"the scene looks washed out (mean %.2f %.2f %.2f, %.0f%% near white; the fight's darkest mean was %.2f): a "
         L"screenshot, and the next frame's draws into %s",
         r, gr, b, white * 100.0f, g.luma_low, draws.c_str());
    char path[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_ACP, 0, draws.c_str(), -1, path, sizeof path, nullptr, nullptr);
    vanacore_gfx_drawlog_once(path);
    screenshot::request(0);
    VanaGfxFight f{};
    if (vanacore_gfx_fight_frame(&f))
        log_lighting(f, L"the lighting as it washed out", true);
    g.caught = true;
}

void on_scene(bool ok, unsigned width, unsigned height, unsigned stride, const std::vector<unsigned char>& bgr,
              const std::string& error)
{
    g.scene_pending = false;
    if (!g.active)
        return;
    if (!ok || width < kStep || height < kStep) {
        line(L"the scene couldn't be read: %hs", error.c_str());
        g.next_scene = GetTickCount64() + 30000;
        return;
    }
    double sum[3] = {};
    unsigned n = 0, white = 0;
    for (unsigned y = 0; y < height; y += kStep)
        for (unsigned x = 0; x < width; x += kStep) {
            const unsigned char* p = &bgr[static_cast<size_t>(y) * stride + x * 3];
            sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
            white += p[0] > 225 && p[1] > 225 && p[2] > 225;
            ++n;
        }
    const float r = static_cast<float>(sum[0] / n / 255.0), gr = static_cast<float>(sum[1] / n / 255.0),
                b = static_cast<float>(sum[2] / n / 255.0);
    const float luma = 0.2126f * r + 0.7152f * gr + 0.0722f * b, share = static_cast<float>(white) / n;
    g.luma_low = std::min(g.luma_low, luma);
    const bool washed = share > 0.35f || (luma > 0.7f && luma - g.luma_low > 0.2f);
    line(L"the scene: mean %.2f %.2f %.2f (luma %.2f), %.0f%% near white%s", r, gr, b, luma, share * 100.0f,
         washed ? L" - WASHED OUT" : g.washed ? L" - back to normal" : L"");
    if (washed && !g.washed)
        ++g.washouts;
    if (washed && !g.caught)
        caught(r, gr, b, share);
    g.washed = washed;
}

void watch_scene(ULONGLONG now)
{
    if (g.scene_pending || now < g.next_scene)
        return;
    g.next_scene = now + kSceneEvery, g.scene_pending = true;
    screenshot::capture(
        false,
        [](bool ok, unsigned w, unsigned h, unsigned stride, const std::vector<unsigned char>& bgr, const std::string& error) {
            on_scene(ok, w, h, stride, bgr, error);
        },
        false);
}

void start(const Aern& a, ULONGLONG now)
{
    g = State{};
    g.active = true, g.started = now, g.last_seen = now, g.aern = a, g.next_scene = now;
    vanacore_gfx_fight_watch(1);
    log_start(a);
}

void stop(ULONGLONG now)
{
    line(L"no Aern near for %llu s: the watch ends after %llu s, %u washout%s seen, up to %u lights a frame, a lit draw's light "
         L"up to %.2f",
         kGone / 1000, (now - g.started) / 1000, g.washouts, g.washouts == 1 ? L"" : L"s", g.lights_max, g.lit_top);
    vanacore_gfx_fight_watch(0);
    g.active = false;
}

void scan(ULONGLONG now)
{
    g.next_scan = now + (g.active ? kScanActive : kScanIdle);
    const Aern a = nearest_aern();
    if (!a.found) {
        if (g.active && now - g.last_seen > kGone)
            stop(now);
        return;
    }
    if (!g.active) {
        start(a, now);
        return;
    }
    g.last_seen = now;
    if (a.id != g.aern.id || a.hpp != g.aern.hpp || a.status != g.aern.status)
        line(L"%hs (index %u) HP %u%%, status %u, %.1f yalms", a.name.c_str(), a.index, a.hpp, a.status, a.yalms);
    g.aern = a;
}

} // namespace

void on_frame()
{
    if (!log::enabled() || g_lines >= kMostLines) {
        if (g.active)
            vanacore_gfx_fight_watch(0), g.active = false;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= g.next_scan)
        scan(now);
    if (!g.active)
        return;
    watch_lighting(now);
    watch_scene(now);
}

} // namespace vanacore::fightwatch
