#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Sparkles ([graphics] sparkles): points of light rising out of places on the ground - the Nyzul Isle portal in the
// Alzadaal Undersea Ruins, or what [graphics] sparkles.places names - twinkling as they go and gone before the top,
// drawn by the effects (Direct3D 12 and 11) with the zone lines. The current zone's places are handed over as the
// player zones. And a GM's aura ([graphics] gmaura): flames of light rising from the floor round every character within
// 60 yalms that shows a GM's icon, handed over each frame as they move.
namespace vanacore::sparkles {

struct Place {
    std::uint16_t zone = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f; // the ground at its middle: the game's x and y, and its height (game::Entity's z)
    float radius = 4.0f;                // the disc they rise from
    float height = 3.0f;                // how high they rise
    std::uint32_t colour = 0xA8E8FF;    // 0xRRGGBB
    bool operator==(const Place&) const = default;
};

struct Settings {
    bool on = true;
    float strength = 1.0f; // 0 to 2
    float size = 1.0f;     // their size, times their own
    float density = 1.0f;  // how many, times their own
    float speed = 1.0f;    // how fast they rise and come and go, times their own
    bool match = true;     // each the colour of the scene where it's born (its place's colour where that isn't seen)
    std::vector<Place> places;
    bool aura = false;                   // gmaura: an aura rising round every character showing a GM's icon
    std::uint32_t aura_colour = 0xFF2A10; // 0xRRGGBB
    float aura_strength = 1.0f;          // 0 to 2
    bool operator==(const Settings&) const = default;
};

// sparkles.places: "zone:x,y,z,radius,height[,#RRGGBB]", a ';' between places. What isn't a place is logged and left out.
std::vector<Place> parse(const std::wstring& text);

// sparkles.places when the settings haven't it: the Nyzul Isle portal
const wchar_t* default_places();

// Each frame, on the game's thread, while VanaCore's renderer draws the game (Direct3D 12 or 11).
void apply(const Settings& settings);

} // namespace vanacore::sparkles
