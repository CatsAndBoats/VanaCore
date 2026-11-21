#include "zoneweather.hpp"

#include <windows.h>

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dats.hpp"

namespace vanacore::zoneweather {
namespace {

// The weather folders of a zone's environment tree, in 0x057's order (Ashita's xi.weather): clear, sunny, cloudy, fog, hot
// spell, heat wave, rain, squall, dust storm, sand storm, wind, gales, snow, blizzards, thunder, thunderstorms, auroras,
// stellar glare, gloom, darkness (as xi-model-viewer's ui/js/environment.js reads them)
constexpr std::array<const char*, 20> kFolders = {"fine", "suny", "clod", "mist", "dryw", "heat", "rain", "squl", "dust", "sand",
                                                  "wind", "stom", "snow", "bliz", "thdr", "bolt", "aura", "ligt", "fogd", "dark"};

std::mutex g_lock;
std::unordered_map<int, std::uint32_t> g_read; // zone -> its weathers (0: none, or not readable)

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

// Walks the DAT's section headers, 16 bytes each: a 4-character id, then a u32 with the kind in its low 7 bits and the size
// (header included) in 16-byte units in the next 20. Kind 0x01 opens a folder named by its id, 0x00 closes one, 0x2F is an
// environment (one an hour: 0000, 0600, 1200...). One under a weather folder whose own folder is "weat" is that weather's;
// other trees (ev01, ev02...) are single objects' own light. Only the headers are read: a zone DAT is 10 to 20 MB.
std::uint32_t read_weathers(const std::wstring& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER size{};
    std::uint32_t mask = 0;
    if (GetFileSizeEx(file, &size)) {
        std::vector<std::array<char, 4>> folders;
        long long pos = 0;
        unsigned char head[16];
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
            } else if (kind == 0x2F) {
                for (std::size_t i = folders.size(); i-- > 1;) {
                    const int weather = folder_weather(folders[i]);
                    if (weather < 0)
                        continue;
                    if (std::memcmp(folders[i - 1].data(), "weat", 4) == 0)
                        mask |= 1u << weather;
                    break;
                }
            }
            pos += bytes;
        }
    }
    CloseHandle(file);
    return mask;
}

} // namespace

bool weathers(int zone, std::uint32_t& mask)
{
    mask = 0;
    if (zone <= 0 || zone > 0xFFFF)
        return false;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        const auto found = g_read.find(zone);
        if (found != g_read.end()) {
            mask = found->second;
            return mask != 0;
        }
    }
    const std::wstring path = dats::path(zone_model(zone));
    if (path.empty())
        return false; // (not kept: the tables may not be read yet)
    const std::uint32_t read = read_weathers(path);
    std::lock_guard<std::mutex> hold(g_lock);
    g_read[zone] = read;
    mask = read;
    return mask != 0;
}

} // namespace vanacore::zoneweather
