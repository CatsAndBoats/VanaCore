// Included by equipstats.cpp inside its namespace (its helpers and data): the gear sets' bonuses, equip_mods.json's
// "sets" (the server's gear_sets.lua, CatsEyeXI's own override of it), counted as the server's
// xi.gear_sets.checkForGearSet counts them: each equipped item a piece of each set it's in (twice where a set lists it
// twice), and a set with `least` pieces or more on gives each of its modifiers' value for min(pieces, most) pieces. The
// server also leaves out a piece above the player's level (level sync); this doesn't.

struct GearSet {
    int least = 2, most = 16;
    struct Bonus {
        std::uint16_t stat = 0;
        std::vector<std::int16_t> values; // for least, least + 1, ... pieces
    };
    std::vector<Bonus> bonuses;
};
std::vector<GearSet> g_sets;
std::unordered_map<std::uint16_t, std::vector<std::uint16_t>> g_set_of; // each item's sets (index into g_sets)

std::size_t read_sets(const json::Value& v, const StatIndex& index)
{
    const json::Value* sets = v.find("sets");
    if (sets == nullptr || !sets->is_object())
        return 0;
    for (const auto& [key, row] : sets->object) {
        if (!row.is_array() || row.array.size() < 4 || !row.array[0].is_array() || !row.array[3].is_array() || g_sets.size() >= 0xFFFF)
            continue;
        GearSet set{static_cast<int>(row.array[1].number), static_cast<int>(row.array[2].number), {}};
        for (const json::Value& mod : row.array[3].array) {
            const int stat = mod.is_array() && mod.array.size() >= 2 ? stat_of(index, mod.array[0]) : -1;
            if (stat < 0)
                continue;
            GearSet::Bonus bonus{static_cast<std::uint16_t>(stat), {}};
            for (std::size_t i = 1; i < mod.array.size(); ++i)
                bonus.values.push_back(to_16(static_cast<int>(mod.array[i].number)));
            set.bonuses.push_back(std::move(bonus));
        }
        if (set.bonuses.empty())
            continue;
        const auto at = static_cast<std::uint16_t>(g_sets.size());
        for (const json::Value& id : row.array[0].array)
            if (id.is_number())
                g_set_of[static_cast<std::uint16_t>(id.number)].push_back(at);
        g_sets.push_back(std::move(set));
    }
    return g_sets.size();
}

// What the sets among the worn items (their ids, 0 for an empty slot) add
void set_sums(std::vector<int>& sums, const std::uint16_t* worn, int count)
{
    std::unordered_map<std::uint16_t, int> pieces;
    for (int i = 0; i < count; ++i)
        if (const auto found = worn[i] != 0 ? g_set_of.find(worn[i]) : g_set_of.end(); found != g_set_of.end())
            for (const std::uint16_t set : found->second)
                ++pieces[set];
    for (const auto& [set, n] : pieces) {
        const GearSet& s = g_sets[set];
        if (n < s.least || s.most < s.least)
            continue;
        const auto tier = static_cast<std::size_t>(std::min(n, s.most) - s.least);
        for (const GearSet::Bonus& bonus : s.bonuses)
            if (tier < bonus.values.size())
                add_mod(sums, bonus.stat, bonus.values[tier]);
    }
}
