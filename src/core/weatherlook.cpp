#include "zoneweather.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dats.hpp"

namespace vanacore::zoneweather {
namespace {

// The weather folders of a zone's environment tree, in 0x057's order (as zoneweather.cpp's)
constexpr std::array<const char*, 20> kFolders = {"fine", "suny", "clod", "mist", "dryw", "heat", "rain", "squl", "dust", "sand",
                                                  "wind", "stom", "snow", "bliz", "thdr", "bolt", "aura", "ligt", "fogd", "dark"};

// A light config of an environment (0x2F): the sun's, the moon's and the ambient colour, the fog's colour, its end and start,
// and the diffuse light's strength (xim's EnvironmentSection, xi-model-viewer's environment.js readLightConfig)
struct Light
{
    float sun[3], moon[3], amb[3], fog[3];
    float fog_end, fog_start, mult;
};

// One environment (0x2F section, one an hour under a weather's folder): its hour, indoors (no sky), the characters' light
// (model, +0x0C) and the terrain's (+0x2C), the dome's radius, spokes and 8 slices (colours +0x6C, heights +0x8C)
struct Frame
{
    int hour = 0;
    bool indoors = false;
    Light model{}, terrain{};
    float radius = 0.0f;
    unsigned spokes = 0;
    float slice[8][3]{};
    float elev[8]{};
};

struct Zone
{
    std::array<std::vector<Frame>, 20> frames; // by weather, by hour
    bool outdoors = false;                     // an environment of its has a sky
};

constexpr std::size_t kEnvBytes = 0xAC;

std::uint32_t zone_model(int zone)
{
    return zone < 0x100 ? 100u + static_cast<std::uint32_t>(zone) : 0x147B3u + static_cast<std::uint32_t>(zone - 0x100);
}

int folder_weather(const char* id)
{
    for (std::size_t i = 0; i < kFolders.size(); ++i) {
        if (std::memcmp(id, kFolders[i], 4) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

float f32(const unsigned char* p)
{
    float v;
    std::memcpy(&v, p, 4);
    return std::isfinite(v) ? v : 0.0f;
}

void rgb(const unsigned char* p, float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<float>(p[i]) / 255.0f;
}

Light read_light(const unsigned char* p)
{
    Light l{};
    rgb(p, l.sun), rgb(p + 4, l.moon), rgb(p + 8, l.amb), rgb(p + 12, l.fog);
    l.fog_end = f32(p + 16), l.fog_start = f32(p + 20), l.mult = f32(p + 24);
    return l;
}

Frame read_frame(const unsigned char* d, int hour)
{
    Frame f;
    f.hour = hour;
    std::uint32_t indoors = 0;
    std::memcpy(&indoors, d, 4);
    f.indoors = indoors == 1;
    f.model = read_light(d + 0x0C), f.terrain = read_light(d + 0x2C);
    f.radius = f32(d + 0x68);
    f.spokes = static_cast<unsigned>(d[0x5E] | d[0x5F] << 8);
    for (int i = 0; i < 8; ++i)
        rgb(d + 0x6C + i * 4, f.slice[i]), f.elev[i] = f32(d + 0x8C + i * 4);
    return f;
}

// The zone's model DAT walked by its section headers (as zoneweather.cpp's read_weathers), each environment under a weather's
// folder in "weat" read: 0xAC bytes after its 16-byte header
std::shared_ptr<Zone> read_zone(int zone)
{
    auto out = std::make_shared<Zone>();
    const std::wstring path = dats::path(zone_model(zone));
    if (path.empty())
        return out;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size)) {
        std::vector<std::array<char, 4>> folders;
        long long pos = 0;
        unsigned char head[16 + kEnvBytes];
        while (pos + 16 <= size.QuadPart) {
            LARGE_INTEGER at{};
            at.QuadPart = pos;
            DWORD got = 0;
            if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN) || !ReadFile(file, head, 16, &got, nullptr) || got != 16)
                break;
            std::uint32_t meta = 0;
            std::memcpy(&meta, head + 4, 4);
            const long long bytes = static_cast<long long>((meta >> 7) & 0xFFFFFu) * 16;
            if (bytes <= 0)
                break;
            const unsigned kind = meta & 0x7Fu;
            std::array<char, 4> id{};
            std::memcpy(id.data(), head, 4);
            if (kind == 0x01) {
                folders.push_back(id);
            } else if (kind == 0x00) {
                if (!folders.empty())
                    folders.pop_back();
            } else if (kind == 0x2F && bytes >= static_cast<long long>(16 + kEnvBytes) && folders.size() >= 2 &&
                       std::memcmp(folders[folders.size() - 2].data(), "weat", 4) == 0) {
                const int weather = folder_weather(folders.back().data());
                int hhmm = 0;
                bool digits = true;
                for (int i = 0; i < 4; ++i) {
                    digits = digits && id[i] >= '0' && id[i] <= '9';
                    hhmm = hhmm * 10 + (id[i] - '0');
                }
                if (weather >= 0 && digits && ReadFile(file, head + 16, static_cast<DWORD>(kEnvBytes), &got, nullptr) && got == kEnvBytes) {
                    const Frame f = read_frame(head + 16, (hhmm / 100) % 24);
                    out->frames[static_cast<std::size_t>(weather)].push_back(f);
                    out->outdoors = out->outdoors || (!f.indoors && f.radius > 0.0f && f.spokes > 0);
                }
            }
            pos += bytes;
        }
    }
    CloseHandle(file);
    for (auto& list : out->frames)
        std::sort(list.begin(), list.end(), [](const Frame& a, const Frame& b) { return a.hour < b.hour; });
    return out;
}

// Zones whose files have each weather's look, tried first, in order (outdoors, as the weather is best known there): Ronfaure
// and La Theine for the calm ones and the rain, the Altepa deserts for the heat and the sand, Sauromugue and Buburimu for the
// wind, Xarcabard and Beaucedine for the snow and the auroras, Qufim and the Gustabergs for thunder, Ro'Maeve for stellar glare,
// Batallia Downs and Xarcabard for gloom and darkness. Past these, every zone's files are looked through (donor_search).
const std::array<std::vector<int>, 20> kDonors = {{
    {100, 101, 102},           {100, 101, 102, 103},      {100, 101, 102, 104},  {102, 104, 105, 123, 124},
    {114, 125, 103, 113, 120}, {114, 125, 103, 113},      {100, 101, 104, 105},  {104, 105, 123, 124, 106},
    {114, 125, 120, 118},      {114, 125, 120},           {120, 118, 107, 108},  {120, 118, 107, 108},
    {112, 111, 126, 128},      {112, 111, 126},           {126, 106, 107, 121},  {126, 106, 107},
    {112, 111, 5},             {122, 113, 130, 5},        {105, 112, 104, 110},  {112, 105, 110},
}};

std::mutex g_lock;
std::map<int, std::shared_ptr<Zone>> g_zones;  // zone -> what its files say (read)
std::array<int, 20> g_donor{};                 // weather -> the donor zone, 0 not looked for yet, -1 none
std::atomic<bool> g_busy{false};               // a read in the background

std::shared_ptr<Zone> cached(int zone)
{
    std::lock_guard<std::mutex> hold(g_lock);
    const auto it = g_zones.find(zone);
    return it == g_zones.end() ? nullptr : it->second;
}

std::shared_ptr<Zone> keep(int zone, std::shared_ptr<Zone> z)
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_zones.emplace(zone, std::move(z)).first->second;
}

// a zone that can lend weather `w`: outdoors, with environments for it and for a clear weather (the ratios' base)
bool lends(const Zone& z, int w)
{
    return z.outdoors && !z.frames[static_cast<std::size_t>(w)].empty() &&
           (!z.frames[0].empty() || !z.frames[1].empty() || !z.frames[2].empty());
}

int donor_search(int w)
{
    for (const int zone : kDonors[static_cast<std::size_t>(w)]) {
        auto z = cached(zone);
        if (!z)
            z = keep(zone, read_zone(zone));
        if (lends(*z, w))
            return zone;
    }
    for (int zone = 1; zone < 300; ++zone) {
        auto z = cached(zone);
        if (!z)
            z = keep(zone, read_zone(zone));
        if (lends(*z, w))
            return zone;
    }
    return -1;
}

// one piece of reading started in the background, if none is running: zone `zone`'s files, or a donor for weather `w`
void start(int zone, int w)
{
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true))
        return;
    std::thread([zone, w] {
        if (zone > 0) {
            keep(zone, read_zone(zone));
        } else if (w >= 0) {
            const int found = donor_search(w);
            std::lock_guard<std::mutex> hold(g_lock);
            g_donor[static_cast<std::size_t>(w)] = found;
        }
        g_busy.store(false);
    }).detach();
}

float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

Light lerp_light(const Light& a, const Light& b, float t)
{
    Light l{};
    for (int i = 0; i < 3; ++i) {
        l.sun[i] = lerp(a.sun[i], b.sun[i], t), l.moon[i] = lerp(a.moon[i], b.moon[i], t);
        l.amb[i] = lerp(a.amb[i], b.amb[i], t), l.fog[i] = lerp(a.fog[i], b.fog[i], t);
    }
    l.fog_end = lerp(a.fog_end, b.fog_end, t), l.fog_start = lerp(a.fog_start, b.fog_start, t), l.mult = lerp(a.mult, b.mult, t);
    return l;
}

// the environment at `minute`: the hour's keyframe and the next one's, blended (xim's computeInterpolatedEnvResource)
Frame at(const std::vector<Frame>& frames, double minute)
{
    if (frames.size() == 1)
        return frames[0];
    const int hour = static_cast<int>(minute / 60.0) % 24;
    const Frame* lo = &frames.back();
    for (const Frame& f : frames) {
        if (f.hour <= hour)
            lo = &f;
    }
    const Frame* hi = &frames.front();
    for (const Frame& f : frames) {
        if (f.hour > hour) {
            hi = &f;
            break;
        }
    }
    if (lo->hour == hi->hour)
        return *lo;
    const double t0 = lo->hour * 60.0, t1 = (hi->hour <= lo->hour ? hi->hour + 24 : hi->hour) * 60.0;
    double m = minute;
    if (m < t0)
        m += 1440.0;
    const float t = static_cast<float>(std::clamp((m - t0) / (t1 - t0), 0.0, 1.0));
    Frame f = *lo;
    f.model = lerp_light(lo->model, hi->model, t), f.terrain = lerp_light(lo->terrain, hi->terrain, t);
    for (int i = 0; i < 8; ++i) {
        for (int c = 0; c < 3; ++c)
            f.slice[i][c] = lerp(lo->slice[i][c], hi->slice[i][c], t);
        f.elev[i] = lerp(lo->elev[i], hi->elev[i], t);
    }
    return f;
}

// the sun's share of the diffuse light: all of it from 06:30 to 17:30, the moon's from 18:30 to 05:30, eased between
float sun_share(double minute)
{
    const double m = std::fmod(minute, 1440.0);
    const double up = std::clamp((m - 330.0) / 60.0, 0.0, 1.0), down = std::clamp((1110.0 - m) / 60.0, 0.0, 1.0);
    return static_cast<float>(std::min(up, down));
}

float ratio(float a, float b)
{
    return std::clamp((a + 0.03f) / (b + 0.03f), 0.2f, 3.0f);
}

void ratios(const Light& t, const Light& b, float share, float amb[3], float sun[3])
{
    for (int i = 0; i < 3; ++i) {
        amb[i] = ratio(t.amb[i], b.amb[i]);
        const float lt = (t.sun[i] * share + t.moon[i] * (1.0f - share)) * (t.mult > 0.0f ? t.mult : 1.0f);
        const float lb = (b.sun[i] * share + b.moon[i] * (1.0f - share)) * (b.mult > 0.0f ? b.mult : 1.0f);
        sun[i] = ratio(lt, lb);
    }
}

} // namespace

bool look(int zone, int weather, double minute, Look& out)
{
    if (zone <= 0 || zone > 0xFFFF || weather < 0 || weather >= 20)
        return false;
    const auto here = cached(zone);
    if (!here) {
        start(zone, -1);
        return false;
    }
    if (!here->outdoors || !here->frames[static_cast<std::size_t>(weather)].empty())
        return false; // indoors, or the zone has this weather's look of its own
    int from = 0;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        from = g_donor[static_cast<std::size_t>(weather)];
    }
    if (from == 0) {
        start(0, weather);
        return false;
    }
    const auto lender = from > 0 ? cached(from) : nullptr;
    if (!lender || !lends(*lender, weather))
        return false;
    const auto& base_frames = !lender->frames[0].empty() ? lender->frames[0] : !lender->frames[1].empty() ? lender->frames[1] : lender->frames[2];
    const Frame t = at(lender->frames[static_cast<std::size_t>(weather)], minute), b = at(base_frames, minute);
    const float share = sun_share(minute);
    std::memcpy(out.fog, t.terrain.fog, sizeof out.fog);
    out.fog_ratio = t.terrain.fog_end > 0.0f && b.terrain.fog_end > 0.0f ? std::clamp(t.terrain.fog_end / b.terrain.fog_end, 0.1f, 3.0f) : 1.0f;
    ratios(t.terrain, b.terrain, share, out.amb_t, out.sun_t);
    ratios(t.model, b.model, share, out.amb_m, out.sun_m);
    out.sky = !t.indoors && t.spokes > 0;
    std::memcpy(out.slice, t.slice, sizeof out.slice);
    std::memcpy(out.elev, t.elev, sizeof out.elev);
    return true;
}

int look_donor(int weather)
{
    if (weather < 0 || weather >= 20)
        return 0;
    std::lock_guard<std::mutex> hold(g_lock);
    return g_donor[static_cast<std::size_t>(weather)];
}

} // namespace vanacore::zoneweather
