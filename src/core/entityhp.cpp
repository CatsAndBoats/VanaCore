#include "entityhp.hpp"

#include <iterator>
#include <unordered_map>

#include "entityhp_track.h"
#include "game.hpp"

namespace vanacore::entityhp {
namespace {

// A member as the last frame saw them, and what the samples have said of their maximums
struct Entry {
    unsigned slot = 0;
    game::PartyVital last;
    Track hp, mp;
    std::uint64_t frame = 0; // the last frame they were in the party list
};

std::unordered_map<std::uint32_t, Entry> g_entries;
std::uint64_t g_frame = 0;
std::uint32_t g_self_id = 0; // the local player's id (party slot 0)
std::uint32_t g_self_hp_max = 0, g_self_mp_max = 0;

Max make_max(const Track& track, std::uint32_t value, unsigned percent)
{
    Max out;
    out.value = estimate(track, value, percent);
    if (out.value == 0)
        return out;
    out.source = track.anchor != 0 ? Source::Full : Source::Estimate;
    out.low = bound_low(track);
    out.high = bound_high(track);
    return out;
}

// The game's own maximum, for the local player (the server's 0x061 sets it): its sample's band is the one number
Max game_max(std::uint32_t value)
{
    return Max{Source::Self, value, value, value};
}

} // namespace

const char* source_name(Source source)
{
    switch (source) {
    case Source::Self:
        return "self";
    case Source::Full:
        return "full";
    case Source::Estimate:
        return "estimate";
    case Source::Server:
        return "server";
    default:
        return "none";
    }
}

void on_frame()
{
    game::PartyVital members[18];
    ++g_frame;
    if (!game::party_vitals(members)) {
        g_entries.clear();
        g_self_id = 0;
        return;
    }
    if (!game::player_max(g_self_hp_max, g_self_mp_max))
        g_self_hp_max = g_self_mp_max = 0;
    g_self_id = members[0].active ? members[0].id : 0;
    for (unsigned slot = 0; slot < 18; ++slot) {
        const game::PartyVital& member = members[slot];
        if (!member.active || member.id == 0)
            continue;
        Entry& entry = g_entries[member.id];
        entry.slot = slot;
        entry.last = member;
        entry.frame = g_frame;
        feed(entry.hp, member.hp, member.hpp);
        feed(entry.mp, member.mp, member.mpp);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) // those who left the party start over when they come back
        it = it->second.frame == g_frame ? std::next(it) : g_entries.erase(it);
}

bool vitals(std::uint32_t id, Vitals& out)
{
    const auto found = g_entries.find(id);
    if (id == 0 || found == g_entries.end())
        return false;
    const Entry& entry = found->second;
    out = Vitals{};
    out.id = id;
    out.party_slot = entry.slot;
    out.hp = entry.last.hp, out.mp = entry.last.mp, out.tp = entry.last.tp;
    out.hpp = entry.last.hpp, out.mpp = entry.last.mpp;
    out.hp_max = make_max(entry.hp, entry.last.hp, entry.last.hpp);
    out.mp_max = make_max(entry.mp, entry.last.mp, entry.last.mpp);
    if (id == g_self_id) { // the game has the local player's maximums: no estimate where it has the number
        if (g_self_hp_max != 0)
            out.hp_max = game_max(g_self_hp_max);
        if (g_self_mp_max != 0)
            out.mp_max = game_max(g_self_mp_max);
    }
    return true;
}

} // namespace vanacore::entityhp
