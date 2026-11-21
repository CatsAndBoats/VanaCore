// Included by equipstats.cpp inside its namespace (its helpers and data, after equipstats_check.h): /vana statsdump, the
// Equipment window's stats worked out again into the log, each step of them: the data read, what latent effects are
// tested against, each equipped item (its id, its name and where its modifiers come from: the server's data, or its
// description over them where CatsEyeXI has changed the item since), each of its modifiers, latent effects (on or off)
// and augments (the extdata's id, n and multiplier, as the server works them out), the sets on, then each stat's sum in
// the server's units and as the window shows it. For a report such as "Skillchain damage +600%" (the unit was 1, the
// server's is 100).

constexpr const char* kSlotNames[kSlots] = {"main", "sub", "range", "ammo", "head", "body", "hands", "legs",
                                            "feet", "neck", "waist", "ear1", "ear2", "ring1", "ring2", "back"};

void dump_line(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    log::write(L"Stats dump: %hs", line);
}

// A stat's value as the server has it and as the window shows it: "SKILLCHAINDMG 600 = +6% (Skillchain damage, unit 100)"
std::string dump_value(int stat, int value)
{
    const Stat& s = g_stats[stat];
    Cell cell{"", "", {}};
    write_value(cell, value, s, true);
    char out[192];
    std::snprintf(out, sizeof out, "%s %d = %s (%s, unit %d%s)", s.name.c_str(), value, cell.value, s.label.c_str(), s.scale,
                  s.highest ? ", the highest alone" : "");
    return out;
}

void dump_mods(const char* what, const std::vector<Mod>& mods)
{
    for (const Mod& m : mods)
        dump_line("    %s %s", what, dump_value(m.stat, m.value).c_str());
}

const char* dump_source(std::uint16_t id)
{
    const char* raw = ashita::resource_item_description(id);
    if (raw == nullptr)
        return "the server's data (its description can't be read)";
    const auto described = g_described.find(id);
    if (described == g_described.end())
        return "its description over the server's data (an item the data has no description of: new since)";
    if (described->second != equipdesc::fingerprint(equipdesc::ascii(raw)))
        return "its description over the server's data (its description has changed since the data was made)";
    return "the server's data (its description as when the data was made)";
}

void dump_latents(const ashita::ItemSlot& item, int slot, const World& world, std::vector<int>& sums)
{
    const auto found = g_latents.find(item.id);
    if (found == g_latents.end())
        return;
    for (const Latent& latent : found->second) {
        const bool on = active(latent, slot, world);
        dump_line("    latent (condition %d, parameter %d) %s: %s", latent.condition, latent.param, on ? "on" : "off",
                  dump_value(latent.stat, latent.value).c_str());
        if (on)
            add_mod(sums, latent.stat, latent.value);
    }
}

// augment_sums, each step said
void dump_augments(const ashita::ItemSlot& item, std::vector<int>& sums)
{
    if (g_charged.count(item.id) != 0) {
        dump_line("    a charged item: its extdata isn't augments");
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const auto word = value_at<std::uint16_t>(item.extra + 2 + i * 2);
        if (word == 0)
            continue;
        const int id = word & 0x7FF, n = word >> 11;
        const auto found = g_augments.find(static_cast<std::uint16_t>(id));
        if (found == g_augments.end()) {
            dump_line("    augment %d (n %d, word %04X): nothing the window shows", id, n, word);
            continue;
        }
        for (const AugmentMod& mod : found->second) {
            const int base = mod.value > 0 ? mod.value + n : mod.value - n;
            const auto value = static_cast<std::int16_t>(base * std::max<int>(mod.multiplier, 1));
            dump_line("    augment %d (n %d): (%d %c %d) x %d: %s", id, n, mod.value, mod.value > 0 ? '+' : '-', n, mod.multiplier,
                      dump_value(mod.stat, value).c_str());
            add_mod(sums, mod.stat, value);
        }
    }
}

void dump_item(const ashita::ItemSlot& item, int slot, const World& world, std::vector<int>& sums)
{
    const char* name = ashita::resource_item_name(item.id);
    dump_line("  %s: %d %s, from %s", kSlotNames[slot], item.id, name != nullptr ? name : "?", dump_source(item.id));
    const auto sql = g_items.find(item.id);
    const std::vector<Mod>& used = item_mods(item.id);
    if (sql != g_items.end())
        dump_mods("server", sql->second);
    const bool same = sql != g_items.end() ? sql->second.size() == used.size() &&
                                                 std::equal(used.begin(), used.end(), sql->second.begin(),
                                                            [](const Mod& a, const Mod& b) { return a.stat == b.stat && a.value == b.value; })
                                           : used.empty();
    if (!same)
        dump_mods("used (the description's over the server's)", used);
    for (const Mod& mod : used)
        add_mod(sums, mod.stat, mod.value);
    dump_latents(item, slot, world, sums);
    dump_augments(item, sums);
}

// set_sums, each step said
void dump_sets(const std::uint16_t* worn, std::vector<int>& sums)
{
    std::unordered_map<std::uint16_t, int> pieces;
    for (int i = 0; i < kSlots; ++i)
        if (const auto found = worn[i] != 0 ? g_set_of.find(worn[i]) : g_set_of.end(); found != g_set_of.end())
            for (const std::uint16_t set : found->second)
                ++pieces[set];
    for (const auto& [set, n] : pieces) {
        const GearSet& s = g_sets[set];
        const bool on = n >= s.least && s.most >= s.least;
        dump_line("  set %d: %d pieces (bonus from %d, most %d)%s", set, n, s.least, s.most, on ? "" : ": none yet");
        if (!on)
            continue;
        const auto tier = static_cast<std::size_t>(std::min(n, s.most) - s.least);
        for (const GearSet::Bonus& bonus : s.bonuses)
            if (tier < bonus.values.size()) {
                dump_line("    set %s", dump_value(bonus.stat, bonus.values[tier]).c_str());
                add_mod(sums, bonus.stat, bonus.values[tier]);
            }
    }
}

void dump_world(const World& w)
{
    dump_line("latent effects tested against: %s, job %d/%d level %d, HP %d/%d MP %d/%d TP %d, %s, party %d (%d in zone), zone %d, "
              "Vana'diel hour %d weekday %d moon %d%% (%s), weather %d",
              w.known ? "the player" : "nobody (the player isn't known: every latent effect off)", w.main_job, w.sub_job, w.level,
              w.hp, w.hp_max, w.mp, w.mp_max, w.tp, w.engaged ? "engaged" : "not engaged", w.party, w.in_zone, w.zone, w.hour,
              w.weekday, w.moon, w.moon_way == 1 ? "waning" : w.moon_way == 2 ? "waxing" : "turning", w.weather);
}

// The dump (game thread): the items dumped, -1 with the data missing, -2 while it's still being read
int dump_all()
{
    int expected = kUnread;
    if (!g_dir.empty() && g_data.compare_exchange_strong(expected, kReading))
        read_data(); // (here and now: the window hasn't been opened yet)
    if (g_data.load() == kReading)
        return -2;
    if (g_data.load() != kRead) {
        dump_line("no data: %ls\\addons\\vanacore\\libs\\gamedata\\equip_mods.json wasn't read", g_dir.c_str());
        return -1;
    }
    dump_line("%u stats, %u items, %u augments, %u latent items, %u sets in the data; the window %s", static_cast<unsigned>(g_stats.size()),
              static_cast<unsigned>(g_items.size()), static_cast<unsigned>(g_augments.size()), static_cast<unsigned>(g_latents.size()),
              static_cast<unsigned>(g_sets.size()), g_on ? "on (equip.stats)" : "off (equip.stats=0)");
    const World world = read_world();
    dump_world(world);
    std::vector<int> sums(g_stats.size(), 0);
    std::uint16_t worn[kSlots] = {};
    int count = 0;
    for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
        std::uint32_t container = 0, index = 0;
        ashita::ItemSlot item{};
        if (ashita::equipped(slot, container, index) && index != 0 && ashita::container_item(container, index, item) && item.id != 0) {
            dump_item(item, static_cast<int>(slot), world, sums);
            worn[slot] = item.id;
            ++count;
        }
    }
    dump_sets(worn, sums);
    dump_line("the sums (what the window shows besides the server's totals):");
    for (std::size_t stat = 0; stat < sums.size(); ++stat)
        if (sums[stat] != 0)
            dump_line("  %s", dump_value(static_cast<int>(stat), sums[stat]).c_str());
    const std::lock_guard lock(g_lock);
    if (g_totals.known)
        dump_line("the server's totals (/checkparam): accuracy %d, attack %d, sub %d/%d, ranged %d/%d, defense %d, evasion %d",
                  g_totals.acc, g_totals.att, g_totals.acc_sub, g_totals.att_sub, g_totals.racc, g_totals.ratt, g_totals.def,
                  g_totals.eva);
    else
        dump_line("the server's totals (/checkparam): not asked for yet");
    return count;
}
