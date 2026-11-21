#include "sparkles.hpp"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

#include "../gfx/vanacore_gfx.h"
#include "game.hpp"
#include "log.hpp"

namespace vanacore::sparkles {
namespace {

constexpr unsigned kMaxPlaces = 16; // GFX_SPARKLES, the zone's places and the auras together
constexpr unsigned kMaxZone = 8;    // ... of them the zone's places
constexpr unsigned kFloats = 12;    // GFX_SPARKLE_FLOATS
constexpr ULONGLONG kEvery = 500;   // how often the zone is looked at (ms)
constexpr float kAuraWithin = 60.0f; // yalms

std::uint16_t g_zone = 0xFFFF; // the zone the places were handed over for
ULONGLONG g_looked = 0;
Settings g_sent;               // what was last handed over
bool g_sent_any = false;
unsigned g_sent_auras = 0;     // ... and how many auras with it

// A place as the effects take it: its centre (x, height, y as the game draws), radius, colour, how high, style, size,
// amount and strength
void put(float* o, float x, float height, float y, float radius, std::uint32_t colour, float rise, float style, float strength)
{
    o[0] = x, o[1] = height, o[2] = y, o[3] = radius;
    o[4] = (colour >> 16 & 0xFF) / 255.0f, o[5] = (colour >> 8 & 0xFF) / 255.0f, o[6] = (colour & 0xFF) / 255.0f;
    o[7] = rise, o[8] = style, o[9] = 1.0f, o[10] = 1.0f, o[11] = strength;
}

// An aura's ring and how high it rises, by the character's race: 1-2 Hume, 3-4 Elvaan, 5-6 Tarutaru, 7 Mithra, 8 Galka
void aura_shape(unsigned race, float& radius, float& rise)
{
    switch (race) {
    case 3: case 4: radius = 0.62f, rise = 2.5f; break;
    case 5: case 6: radius = 0.45f, rise = 1.3f; break;
    case 7: radius = 0.55f, rise = 2.1f; break;
    case 8: radius = 0.8f, rise = 2.7f; break;
    default: radius = 0.6f, rise = 2.2f; break;
    }
}

std::uint16_t current_zone()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    if (!game::party(members, alliance) || !members[0].active)
        return 0xFFFF;
    return members[0].zone;
}

// The zone's places, then the auras (gm: 4 floats each, from game::gm_places)
void send(const Settings& settings, const float* gm, unsigned auras)
{
    float places[kMaxPlaces * kFloats];
    unsigned n = 0;
    for (const Place& p : settings.places) {
        if (!settings.on || p.zone != g_zone || n == kMaxZone)
            continue;
        put(places + n++ * kFloats, p.x, p.z, p.y, p.radius, p.colour, p.height, 0.0f, settings.strength);
    }
    for (unsigned i = 0; i < auras && n < kMaxPlaces; ++i) {
        float radius = 0.0f, rise = 0.0f;
        aura_shape(static_cast<unsigned>(gm[i * 4 + 3]), radius, rise);
        put(places + n++ * kFloats, gm[i * 4], gm[i * 4 + 1], gm[i * 4 + 2], radius, settings.aura_colour, rise, 1.0f,
            settings.aura_strength);
    }
    vanacore_gfx_set_sparkles(places, n, settings.size, settings.density, settings.speed, settings.match ? 1 : 0);
    g_sent = settings, g_sent_any = true, g_sent_auras = auras;
}

} // namespace

std::vector<Place> parse(const std::wstring& text)
{
    std::vector<Place> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(L';', at);
        if (end == std::wstring::npos)
            end = text.size();
        const std::wstring part = text.substr(at, end - at);
        at = end + 1;
        if (std::all_of(part.begin(), part.end(), [](wchar_t c) { return std::iswspace(c) != 0; }))
            continue;
        unsigned zone = 0;
        Place p;
        wchar_t colour[16] = {};
        const int got = swscanf_s(part.c_str(), L" %u : %f , %f , %f , %f , %f , %15ls", &zone, &p.x, &p.y, &p.z, &p.radius,
                                  &p.height, colour, static_cast<unsigned>(std::size(colour)));
        if (got < 6 || zone > 0xFFFF) {
            log::write(L"Graphics: sparkles.places: \"%ls\" isn't zone:x,y,z,radius,height[,#RRGGBB]; left out", part.c_str());
            continue;
        }
        p.zone = static_cast<std::uint16_t>(zone);
        p.radius = std::clamp(p.radius, 0.25f, 30.0f);
        p.height = std::clamp(p.height, 0.25f, 30.0f);
        if (got == 7)
            p.colour = std::wcstoul(colour[0] == L'#' ? colour + 1 : colour, nullptr, 16) & 0xFFFFFF;
        out.push_back(p);
    }
    return out;
}

const wchar_t* default_places()
{
    return L"72:20,-20,-4.75,4.5,3.5,#A8E8FF"; // the Nyzul Isle portal (Alzadaal Undersea Ruins, H-9)
}

void apply(const Settings& settings)
{
    const ULONGLONG now = GetTickCount64();
    bool changed = !g_sent_any || !(settings == g_sent);
    if (now - g_looked >= kEvery) {
        g_looked = now;
        const std::uint16_t zone = current_zone();
        if (zone != 0xFFFF && zone != g_zone)
            g_zone = zone, changed = true;
    }
    // the auras where their characters are now: handed over each frame while there are any
    static float gm[kMaxPlaces * 4];
    const unsigned auras = settings.aura && settings.aura_strength > 0.0f ? game::gm_places(gm, kMaxPlaces - kMaxZone, kAuraWithin) : 0;
    if (changed || auras != 0 || g_sent_auras != 0)
        send(settings, gm, auras);
}

} // namespace vanacore::sparkles
