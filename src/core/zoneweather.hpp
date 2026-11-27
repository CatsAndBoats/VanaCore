#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vanacore::zoneweather {

// The weathers zone `zone`'s model DAT has a sky and light for (the weather folders of its environment tree, "weat"): a bit
// each, bit n for 0x057's weather n (0 clear to 19 darkness). The DAT is the one the game loads, through pivot's overlays.
// False (`mask` 0) when it can't be read or has none. Read once a zone (its section headers only), then kept. Any thread.
bool weathers(int zone, std::uint32_t& mask);

// The weathers whose folder in zone `zone`'s model DAT carries falling rain of its own (a texture named rain or rai1 in the
// folder itself, the drops the game draws round the camera), as weathers' bits. Most indoor zones' rain and squall folders
// are a sky and light alone - Alzadaal Undersea Ruins, the Temple of Uggalepih, Crawlers' Nest, the Boyahda Tree...: the game
// draws no rain there. False (`mask` 0) when it can't be read or has none.
bool rains(int zone, std::uint32_t& mask);

// --- zoneweather.cpp's, for weatherlook.cpp too -------------------------------------------------------------------------
// One section of a zone DAT as walk_sections hands it over: its id, kind and size (header included), the folders open round it
// (innermost last), and the file (a HANDLE), placed just after the section's 16-byte header
struct Section
{
    std::array<char, 4> id{};
    unsigned kind = 0;
    long long bytes = 0;
    const std::vector<std::array<char, 4>>* folders = nullptr;
    void* file = nullptr;
};

// Each section of the DAT at `path` but the folders' opening and closing ones, in order; false when it can't be opened
bool walk_sections(const std::wstring& path, const std::function<void(const Section&)>& visit);

// --- weatherlook.cpp ----------------------------------------------------------------------------------------------------
// A weather's look borrowed from another zone's files, for a weather the zone the player is in has none of (its model DAT has
// no environment for it): what the renderer lays over the zone's own (d3d8_weather_look.h). The fog's colour and the sky's
// slices are the donor's own; the fog's distance and the light are ratios, the donor's weather over its clear weather at the
// same hour, applied to the zone's own (so a zone keeps its palette, made greyer, darker or nearer as the weather makes the
// donor's).
struct Look
{
    float fog[3] = {};       // the fog's colour, 0 to 1
    float fog_ratio = 1.0f;  // the fog's start and end, times this
    float amb_t[3] = {};     // the terrain's ambient light, times these
    float sun_t[3] = {};     // ... its sun or moon
    float amb_m[3] = {};     // the characters' (the model light) ambient
    float sun_m[3] = {};     // ... and sun or moon
    bool sky = false;        // the slices below are a sky (not an indoor environment)
    float slice[8][3] = {};  // the dome's colours, horizon to zenith, 0 to 1
    float elev[8] = {};      // ... at these heights (0 the horizon, 1 the zenith)
};

// The look of weather `weather` (0x057's id) at Vana'diel minute `minute` (0 to 1440) for zone `zone`: true with `out` when the
// zone is outdoors, has no environment of its own for that weather, and a donor zone's is read. The zones' files are read in
// the background (section headers and the environments only), so it's false for the first frames a zone or weather is new.
bool look(int zone, int weather, double minute, Look& out);

// The donor read for weather `weather` (0 none found or not yet), for the log
int look_donor(int weather);

} // namespace vanacore::zoneweather
