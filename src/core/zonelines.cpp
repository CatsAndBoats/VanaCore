#include "zonelines.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "dats.hpp"
#include "game.hpp"
#include "log.hpp"
#include "zonefloor.hpp"

namespace vanacore::zonelines {
namespace {

// The zone's model DAT: file id 100 + the zone below 0x100, 0x147B3 + (zone - 0x100) from there (xi-tools'
// zone_file_id). Its lines, and the ground under them, are zonefloor's.
constexpr unsigned kMaxLines = 64; // GFX_ZONELINES
constexpr ULONGLONG kEvery = 500;  // how often the zone is looked at (ms)
constexpr ULONGLONG kSideEvery = 100; // how often which side of each line the player is on is looked at (ms)

std::uint32_t model_file(std::uint16_t zone)
{
    return zone < 0x100 ? 100u + zone : 0x147B3u + (zone - 0x100u);
}

// The key table that decrypts a zone's collision (zonefloor's, read once for the session)
const std::uint8_t* key_table()
{
    const std::uint8_t* table = zonefloor::game_key_table();
    static bool said = false;
    if (table == nullptr && !said) {
        said = true;
        log::write(L"Zone lines: couldn't find the key table in FFXiMain.dll's file: the ground under them taken from "
                   L"their boxes ([graphics] zonelines)");
    }
    return table;
}

// The zone's lines, each zonefloor::kStride floats. False if its DAT can't be read.
bool read_lines(std::uint16_t zone, std::vector<float>& out, unsigned& floors)
{
    out.clear();
    floors = 0;
    std::vector<std::uint8_t> dat;
    if (!dats::read(model_file(zone), dat))
        return false;
    zonefloor::read_lines(dat, kMaxLines, out);
    if (!out.empty())
        floors = zonefloor::find_floors(dat, key_table(), out);
    return true;
}

std::uint16_t g_zone = 0xFFFF;  // the zone the lines are read for
std::vector<float> g_lines;     // its lines
std::vector<float> g_shown;     // as handed over: but for the fill, each moved onto the face the player is on
std::vector<signed char> g_side; // that face, per line: 1 or -1 along its depth, 0 its middle (not known yet)
ULONGLONG g_looked = 0, g_sided = 0;
Settings g_sent;                 // what was last handed over
bool g_sent_any = false;
unsigned g_sent_count = 0;

std::uint16_t current_zone()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    if (!game::party(members, alliance) || !members[0].active)
        return 0xFFFF;
    return members[0].zone;
}

// The player's place on the ground's plane (x, and the DAT's z: the entity's y). False without one.
bool player_place(float& x, float& y)
{
    game::PartyMember members[18];
    game::Alliance alliance;
    game::Entity me;
    if (!game::party(members, alliance) || !members[0].active || !game::entity(members[0].target_index, me))
        return false;
    x = me.x, y = me.y;
    return true;
}

// A line's depth across, as the effects lay it (the line runs along the box's longer side): the axis on the
// ground's plane, and half the box's size along it.
void depth_axis(const float* l, float& ax, float& ay, float& half)
{
    const float c = std::cos(l[3]), s = std::sin(l[3]);
    const bool swap = l[6] > l[4];
    ax = swap ? c : s, ay = swap ? -s : c;
    half = 0.5f * (swap ? l[4] : l[6]);
}

// Which face of each line the player would walk into: the game zones the player as their place enters the box
// (FFXiMain 0x101743D0, the box at its full size), half its depth before its middle. Kept while the player is in
// a box. True if any changed.
bool look_sides()
{
    float px = 0.0f, py = 0.0f;
    if (!player_place(px, py))
        return false;
    bool changed = false;
    for (std::size_t i = 0; i < g_side.size(); ++i) {
        const float* l = &g_lines[i * zonefloor::kStride];
        float ax, ay, half;
        depth_axis(l, ax, ay, half);
        const float d = (px - l[0]) * ax + (py - l[2]) * ay;
        const signed char side = d >= half ? 1 : d <= -half ? -1 : g_side[i];
        if (side != g_side[i])
            g_side[i] = side, changed = true;
    }
    return changed;
}

void send(const Settings& settings)
{
    const float colour[4] = {(settings.colour >> 16 & 0xFF) / 255.0f, (settings.colour >> 8 & 0xFF) / 255.0f,
                             (settings.colour & 0xFF) / 255.0f, settings.on ? settings.strength : 0.0f};
    const unsigned count = settings.on ? static_cast<unsigned>(g_lines.size() / zonefloor::kStride) : 0u;
    g_shown = g_lines;
    if (settings.style != 0) // the fill shows the whole box
        for (std::size_t i = 0; i < g_side.size(); ++i) {
            float* l = &g_shown[i * zonefloor::kStride];
            float ax, ay, half;
            depth_axis(l, ax, ay, half);
            l[0] += g_side[i] * half * ax;
            l[2] += g_side[i] * half * ay;
        }
    vanacore_gfx_set_zonelines(g_shown.data(), count, colour, settings.style, settings.glow ? 1 : 0, settings.height,
                               settings.size, settings.fade);
    g_sent = settings, g_sent_any = true, g_sent_count = count;
}

} // namespace

void apply(const Settings& settings)
{
    const ULONGLONG now = GetTickCount64();
    bool changed = !g_sent_any || settings.on != g_sent.on || settings.colour != g_sent.colour ||
                   settings.strength != g_sent.strength || settings.style != g_sent.style || settings.glow != g_sent.glow ||
                   settings.height != g_sent.height || settings.size != g_sent.size || settings.fade != g_sent.fade;
    if (settings.on && now - g_looked >= kEvery) {
        g_looked = now;
        const std::uint16_t zone = current_zone();
        if (zone != 0xFFFF && zone != g_zone) {
            g_zone = zone;
            unsigned floors = 0;
            const bool read = read_lines(zone, g_lines, floors);
            g_side.assign(g_lines.size() / zonefloor::kStride, 0);
            g_sided = 0;
            if (read) {
                log::write(L"Zone lines: %u in zone %u (its DAT, file %u), the ground under %u of them from its collision "
                           L"([graphics] zonelines)",
                           static_cast<unsigned>(g_lines.size() / zonefloor::kStride), zone, model_file(zone), floors);
                for (std::size_t i = 0; i + zonefloor::kStride <= g_lines.size(); i += zonefloor::kStride)
                    log::write(L"Zone lines:   at %.1f, %.1f, %.1f (%.1f x %.1f x %.1f): the ground %.2f above its middle",
                               g_lines[i], g_lines[i + 1], g_lines[i + 2], g_lines[i + 4], g_lines[i + 5], g_lines[i + 6],
                               g_lines[i + 7]);
            } else
                log::write(L"Zone lines: couldn't read zone %u's DAT (file %u); none shown ([graphics] zonelines)", zone,
                           model_file(zone));
            changed = true;
        }
    }
    if (settings.on && settings.style != 0 && !g_side.empty() && now - g_sided >= kSideEvery) {
        g_sided = now;
        changed = look_sides() || changed;
    }
    if (changed)
        send(settings);
}

} // namespace vanacore::zonelines
