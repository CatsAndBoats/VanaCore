#include "zoneweather.hpp"

#include <windows.h>

#include <array>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dats.hpp"

namespace vanacore::zoneweather {

// Walks a zone DAT's section headers, 16 bytes each: a 4-character id, then a u32 with the kind in its low 7 bits and the size
// (header included) in 16-byte units in the next 20. Kind 0x01 opens a folder named by its id and 0x00 closes one (kept in
// `folders`, not handed over); every other section is. An environment (0x2F, one an hour: 0000, 0600, 1200...) under a weather
// folder whose own folder is "weat" is that weather's; other trees (ev01, ev02...) are single objects' own light. Only the
// headers are read (a zone DAT is 10 to 20 MB), and what a visit reads past its header itself
bool walk_sections(const std::wstring& path, const std::function<void(const Section&)>& visit)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size)) {
        std::vector<std::array<char, 4>> folders;
        Section s;
        s.folders = &folders;
        s.file = file;
        unsigned char head[16];
        for (long long pos = 0; pos + 16 <= size.QuadPart; pos += s.bytes) {
            LARGE_INTEGER at{};
            at.QuadPart = pos;
            DWORD got = 0;
            if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN) || !ReadFile(file, head, 16, &got, nullptr) || got != 16)
                break;
            std::uint32_t meta = 0;
            std::memcpy(&meta, head + 4, 4);
            s.bytes = static_cast<long long>((meta >> 7) & 0xFFFFFu) * 16;
            if (s.bytes <= 0)
                break;
            s.kind = meta & 0x7Fu;
            std::memcpy(s.id.data(), head, 4);
            if (s.kind == 0x01)
                folders.push_back(s.id);
            else if (s.kind == 0x00) {
                if (!folders.empty())
                    folders.pop_back();
            } else
                visit(s);
        }
    }
    CloseHandle(file);
    return true;
}

namespace {

// The weather folders of a zone's environment tree, in 0x057's order (Ashita's xi.weather): clear, sunny, cloudy, fog, hot
// spell, heat wave, rain, squall, dust storm, sand storm, wind, gales, snow, blizzards, thunder, thunderstorms, auroras,
// stellar glare, gloom, darkness (as xi-model-viewer's ui/js/environment.js reads them)
constexpr std::array<const char*, 20> kFolders = {"fine", "suny", "clod", "mist", "dryw", "heat", "rain", "squl", "dust", "sand",
                                                  "wind", "stom", "snow", "bliz", "thdr", "bolt", "aura", "ligt", "fogd", "dark"};

// What a zone's DAT says (0s: none, or not readable)
struct Read
{
    std::uint32_t sky = 0;  // the weathers it has a folder for
    std::uint32_t rain = 0; // ... those whose folder carries falling rain (rains)
};

std::mutex g_lock;
std::unordered_map<int, Read> g_read; // zone -> what its DAT says

// The zone's model DAT: file id 100 + the zone below 0x100, 0x147B3 + (zone - 0x100) from there (as zonelines.cpp)
std::uint32_t zone_model(int zone)
{
    return zone < 0x100 ? 100u + static_cast<std::uint32_t>(zone) : 0x147B3u + static_cast<std::uint32_t>(zone - 0x100);
}

int folder_weather(const std::array<char, 4>& id)
{
    for (std::size_t i = 0; i < kFolders.size(); ++i) {
        if (std::memcmp(id.data(), kFolders[i], 4) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

// What the zone's DAT says: the weathers with an environment under "weat" (sky), and those whose folder carries a texture named
// rain or rai1 laid straight in it, the rain the game draws round the camera in that weather (rains)
Read read_weathers(const std::wstring& path)
{
    Read out;
    walk_sections(path, [&out](const Section& s) {
        const auto& folders = *s.folders;
        if (s.kind == 0x2F) {
            for (std::size_t i = folders.size(); i-- > 1;) {
                const int weather = folder_weather(folders[i]);
                if (weather < 0)
                    continue;
                if (std::memcmp(folders[i - 1].data(), "weat", 4) == 0)
                    out.sky |= 1u << weather;
                break;
            }
        } else if (s.kind == 0x20 && std::memcmp(s.id.data(), "rai", 3) == 0 && folders.size() >= 2 &&
                   std::memcmp(folders[folders.size() - 2].data(), "weat", 4) == 0) {
            const int weather = folder_weather(folders.back());
            if (weather >= 0)
                out.rain |= 1u << weather;
        }
    });
    return out;
}

// the zone's DAT read once, then kept; false when its path isn't known yet (not kept: the tables may not be read yet)
bool lookup(int zone, Read& out)
{
    out = Read{};
    if (zone <= 0 || zone > 0xFFFF)
        return false;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const auto found = g_read.find(zone);
        if (found != g_read.end()) {
            out = found->second;
            return true;
        }
    }
    const std::wstring path = dats::path(zone_model(zone));
    if (path.empty())
        return false;
    out = read_weathers(path);
    std::lock_guard<std::mutex> hold(g_lock);
    g_read[zone] = out;
    return true;
}

} // namespace

bool weathers(int zone, std::uint32_t& mask)
{
    Read read;
    lookup(zone, read);
    mask = read.sky;
    return mask != 0;
}

bool rains(int zone, std::uint32_t& mask)
{
    Read read;
    lookup(zone, read);
    mask = read.rain;
    return mask != 0;
}

} // namespace vanacore::zoneweather
