// Included by equipstats.cpp inside its namespace (its data and helpers, after equipstats_dump.h): vana.gamedata.item_stats
// (vana.game.native_item_stats, which lua/vana.lua hands on), one item's stats as the Equipment window counts them, for
// addons (asked for 2026-11-11 for yunagearopt, which read them from the wiki): the modifiers VanaCore uses (item_mods: the
// server's, equip_mods.json's, with its description's numbers over them where CatsEyeXI has changed the item since), the
// server's alone and which of the two it is, its latent effects, the gear sets it's in, given its extdata its augments',
// and each stat named in them with its label and unit. docs/addons/04-game-data.md.

// Every modifier the data names that isn't one of its stats, the window's rows (the game's own window's: DEF, HP, MP, STR
// to CHR; elemental magic evasion; added effects, weapon skills, a race only...), as a stat after them: labelled by its
// name, unit 1, `shown` false, so the items', latent effects', augments' and sets' modifiers keep it for item_stats. The
// Equipment and Check windows' rows skip it; /vana statsdump says it.
void add_unshown(const json::Value& v, StatIndex& index)
{
    const auto add = [&](const std::string& name) {
        if (!name.empty() && index.count(name) == 0 && g_stats.size() < 0xFFFF) {
            index.emplace(name, static_cast<int>(g_stats.size()));
            g_stats.push_back({name, 1, false, false, "", name, false});
        }
    };
    const auto add_first = [&](const json::Value& row) { // a row's first, its modifier's name
        if (row.is_array() && !row.array.empty() && row.array[0].is_string())
            add(row.array[0].string);
    };
    if (const json::Value* items = v.find("items"); items != nullptr && items->is_object())
        for (const auto& item : items->object)
            for (const auto& mod : item.second.object)
                add(mod.first);
    for (const char* list : {"latents", "augments"})
        if (const json::Value* rows = v.find(list); rows != nullptr && rows->is_object())
            for (const auto& entry : rows->object)
                for (const json::Value& row : entry.second.array)
                    add_first(row);
    if (const json::Value* sets = v.find("sets"); sets != nullptr && sets->is_object())
        for (const auto& set : sets->object)
            if (set.second.is_array() && set.second.array.size() >= 4)
                for (const json::Value& mod : set.second.array[3].array)
                    add_first(mod);
}

void set_int(lua_State* L, const char* key, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

void set_text(lua_State* L, const char* key, const std::string& value)
{
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, key);
}

// Where an item's `mods` come from: "server" (its description as when the data was made), "description" (changed since,
// or an item the data has no description of: its numbers over the server's), "unread" (the item DATs can't be read)
const char* source_of(std::uint16_t id)
{
    const char* raw = ashita::resource_item_description(id);
    if (raw == nullptr)
        return "unread";
    const auto described = g_described.find(id);
    return described != g_described.end() && described->second == equipdesc::fingerprint(equipdesc::ascii(raw)) ? "server"
                                                                                                                  : "description";
}

// `mods` as { NAME = value } at `key`, each stat noted in `named` (for "stats")
void push_mods(lua_State* L, const char* key, const std::vector<Mod>& mods, std::vector<bool>& named)
{
    lua_createtable(L, 0, static_cast<int>(mods.size()));
    for (const Mod& m : mods) {
        set_int(L, g_stats[m.stat].name.c_str(), m.value);
        named[m.stat] = true;
    }
    lua_setfield(L, -2, key);
}

// "latents": { { mod, value, condition, param }, ... } (the server's LATENT and its parameter); none: left out
void push_latents(lua_State* L, std::uint16_t id, std::vector<bool>& named)
{
    const auto found = g_latents.find(id);
    if (found == g_latents.end())
        return;
    lua_createtable(L, static_cast<int>(found->second.size()), 0);
    int n = 0;
    for (const Latent& latent : found->second) {
        lua_createtable(L, 0, 4);
        set_text(L, "mod", g_stats[latent.stat].name);
        set_int(L, "value", latent.value);
        set_int(L, "condition", latent.condition);
        set_int(L, "param", latent.param);
        lua_rawseti(L, -2, ++n);
        named[latent.stat] = true;
    }
    lua_setfield(L, -2, "latents");
}

// One set: { items = { ids, sorted; an item the set lists twice twice }, least, most, bonuses = { NAME = { value for
// least pieces, least + 1, ... } } }
void push_set(lua_State* L, std::uint16_t set, std::vector<bool>& named)
{
    const GearSet& s = g_sets[set];
    std::vector<std::uint16_t> items;
    for (const auto& [item, sets] : g_set_of)
        for (const std::uint16_t in : sets)
            if (in == set)
                items.push_back(item);
    std::sort(items.begin(), items.end());
    lua_createtable(L, 0, 4);
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        lua_pushinteger(L, items[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "items");
    set_int(L, "least", s.least);
    set_int(L, "most", s.most);
    lua_createtable(L, 0, static_cast<int>(s.bonuses.size()));
    for (const GearSet::Bonus& bonus : s.bonuses) {
        lua_createtable(L, static_cast<int>(bonus.values.size()), 0);
        for (std::size_t i = 0; i < bonus.values.size(); ++i) {
            lua_pushinteger(L, bonus.values[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        lua_setfield(L, -2, g_stats[bonus.stat].name.c_str());
        named[bonus.stat] = true;
    }
    lua_setfield(L, -2, "bonuses");
}

// "sets": each set the item is in; none: left out
void push_sets(lua_State* L, std::uint16_t id, std::vector<bool>& named)
{
    const auto found = g_set_of.find(id);
    if (found == g_set_of.end())
        return;
    lua_createtable(L, static_cast<int>(found->second.size()), 0);
    int n = 0;
    for (const std::uint16_t set : found->second) {
        push_set(L, set, named);
        lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, -2, "sets");
}

// "augments": the extdata's augments' modifiers as the server works them out (augment_sums), { NAME = value }
void push_augments(lua_State* L, std::uint16_t id, const std::uint8_t* extra, std::vector<bool>& named)
{
    std::vector<int> sums(g_stats.size(), 0);
    augment_sums(sums, id, extra);
    lua_newtable(L);
    for (std::size_t stat = 0; stat < sums.size(); ++stat)
        if (sums[stat] != 0) {
            set_int(L, g_stats[stat].name.c_str(), sums[stat]);
            named[stat] = true;
        }
    lua_setfield(L, -2, "augments");
}

// "stats": { NAME = { label, unit, percent, highest?, shown? } } for each stat named above (the value / unit is what's
// shown); shown false for one the Equipment window doesn't show (add_unshown: its name its label)
void push_stats(lua_State* L, const std::vector<bool>& named)
{
    lua_newtable(L);
    for (std::size_t stat = 0; stat < named.size(); ++stat) {
        if (!named[stat])
            continue;
        const Stat& s = g_stats[stat];
        lua_createtable(L, 0, 4);
        set_text(L, "label", s.label);
        set_int(L, "unit", s.scale);
        lua_pushboolean(L, s.percent ? 1 : 0);
        lua_setfield(L, -2, "percent");
        if (s.highest) {
            lua_pushboolean(L, 1);
            lua_setfield(L, -2, "highest");
        }
        if (!s.shown) {
            lua_pushboolean(L, 0);
            lua_setfield(L, -2, "shown");
        }
        lua_setfield(L, -2, s.name.c_str());
    }
    lua_setfield(L, -2, "stats");
}

// The data's state, read here and now the first time (as /vana statsdump): nullptr when it's there, else why not
const char* data_missing()
{
    int expected = kUnread;
    if (!g_dir.empty() && g_data.compare_exchange_strong(expected, kReading))
        read_data();
    switch (g_data.load()) {
    case kRead: return nullptr;
    case kReading: return "still being read: ask again";
    case kUnread: return "not ready yet: ask again";
    default: return "addons\\vanacore\\libs\\gamedata\\equip_mods.json wasn't read";
    }
}

// vana.gamedata.item_stats(id[, extra]) -> table | nil, why (game thread)
int item_stats_lua(lua_State* L)
{
    const lua_Integer id = luaL_checkinteger(L, 1);
    std::size_t length = 0;
    const char* given = lua_type(L, 2) == LUA_TSTRING ? lua_tolstring(L, 2, &length) : nullptr;
    const char* why = id <= 0 || id > 0xFFFF ? "not an item id" : data_missing();
    if (why != nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    const auto item = static_cast<std::uint16_t>(id);
    std::vector<bool> named(g_stats.size(), false);
    lua_createtable(L, 0, 12);
    set_int(L, "id", item);
    if (const char* name = ashita::resource_item_name(item); name != nullptr)
        set_text(L, "name", gametext::to_utf8(name));
    if (const char* raw = ashita::resource_item_description(item); raw != nullptr)
        set_text(L, "description", equipdesc::ascii(raw));
    lua_pushstring(L, source_of(item));
    lua_setfield(L, -2, "source");
    push_mods(L, "mods", item_mods(item), named);
    const auto sql = g_items.find(item);
    push_mods(L, "server", sql != g_items.end() ? sql->second : std::vector<Mod>{}, named);
    push_latents(L, item, named);
    push_sets(L, item, named);
    if (given != nullptr) {
        std::uint8_t extra[sizeof(ashita::ItemSlot::extra)] = {};
        std::memcpy(extra, given, std::min(length, sizeof extra));
        push_augments(L, item, extra, named);
    }
    push_stats(L, named);
    lua_createtable(L, 0, 2);
    set_text(L, "generated", g_generated);
    set_text(L, "source", g_source);
    lua_setfield(L, -2, "data");
    return 1;
}
