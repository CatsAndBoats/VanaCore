#include "lua_entity.hpp"

#include <lua.hpp>

#include <cmath>
#include <cstdint>
#include <string>

#include "entityhp.hpp"
#include "game.hpp"
#include "lua_fields.hpp"
#include "maxhp_ask.hpp"
#include "widgets.hpp"

namespace vanacore::lua_entity {
namespace {

using lua_fields::set_bool;
using lua_fields::set_number;
using lua_fields::set_string;

// A list of numbers, as `key`
template <typename T>
void set_list(lua_State* L, const char* key, const T* values, int count)
{
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, values[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, key);
}

// `prefix`_max, _low, _high and _source of a maximum, into the table on the top
void set_one_max(lua_State* L, const char* prefix, const entityhp::Max& max)
{
    const std::string base = std::string(prefix) + "_max";
    set_string(L, (base + "_source").c_str(), entityhp::source_name(max.source));
    if (max.source == entityhp::Source::None)
        return;
    set_number(L, base.c_str(), max.value);
    set_number(L, (base + "_low").c_str(), max.low);
    set_number(L, (base + "_high").c_str(), max.high);
}

// The health numbers of a party member, into the table on the top
void set_vitals(lua_State* L, const entityhp::Vitals& vitals)
{
    set_number(L, "party_slot", vitals.party_slot);
    set_number(L, "hp", vitals.hp);
    set_number(L, "mp", vitals.mp);
    set_number(L, "tp", vitals.tp);
    set_number(L, "hpp", vitals.hpp);
    set_number(L, "mpp", vitals.mpp);
    set_one_max(L, "hp", vitals.hp_max);
    set_one_max(L, "mp", vitals.mp_max);
}

// What the record holds beyond the first dozen fields (the second half of game::Entity)
void set_more(lua_State* L, const game::Entity& entity)
{
    set_number(L, "anim_speed", entity.anim_speed);
    set_number(L, "door_id", entity.door_id);
    set_list(L, "render", entity.render, 9);
    set_number(L, "status_server", entity.status_server);
    set_number(L, "status_event", entity.status_event);
    set_number(L, "costume_id", entity.costume_id);
    set_number(L, "zone_id", entity.zone_id);
    set_list(L, "animations", entity.animations, 10);
    set_number(L, "animation_time", entity.animation_time);
    set_number(L, "animation_step", entity.animation_step);
    set_number(L, "animation_play", entity.animation_play);
    set_number(L, "emote_target_index", entity.emote_target_index);
    set_number(L, "emote_id", entity.emote_id);
    set_number(L, "linkshell_color", entity.linkshell_color);
    set_number(L, "name_color", entity.name_color);
    set_number(L, "campaign_name_flag", entity.campaign_name_flag);
    set_number(L, "mount_id", entity.mount_id);
    set_number(L, "last_action_id", entity.last_action_id);
    set_number(L, "interaction_target_index", entity.interaction_target_index);
    set_number(L, "ballista_flags", entity.ballista_flags);
    set_number(L, "fellow_index", entity.fellow_index);
    set_number(L, "trust_owner_index", entity.trust_owner_index);
}

// The entity's table, on the top of the stack
void push_entity(lua_State* L, const game::Entity& entity)
{
    lua_createtable(L, 0, 64);
    set_number(L, "index", entity.index);
    set_number(L, "id", entity.id);
    set_string(L, "name", entity.name);
    set_number(L, "x", entity.x);
    set_number(L, "y", entity.y);
    set_number(L, "z", entity.z);
    set_number(L, "heading", entity.heading);
    set_number(L, "distance", entity.distance);
    set_number(L, "distance_yalms", entity.distance > 0.0f ? std::sqrt(entity.distance) : 0.0);
    set_number(L, "speed", entity.speed);
    set_number(L, "hpp", entity.hpp);
    set_number(L, "type", entity.type);
    set_number(L, "race", entity.race);
    set_number(L, "status", entity.status);
    set_number(L, "spawn_flags", entity.spawn_flags);
    set_bool(L, "is_pc", (entity.spawn_flags & 0x01) != 0);
    set_bool(L, "is_npc", (entity.spawn_flags & 0x02) != 0);
    set_bool(L, "is_monster", (entity.spawn_flags & 0x10) != 0);
    set_bool(L, "is_self", (entity.spawn_flags & 0x0D) == 0x0D);
    set_number(L, "model_size", entity.model_size);
    set_number(L, "hitbox_size", entity.hitbox_size);
    set_number(L, "claim", entity.claim);
    set_number(L, "claim_id", entity.claim & 0xFFFF);
    set_bool(L, "claimed", (entity.claim >> 16) != 0);
    set_number(L, "target_index", entity.target_index);
    set_number(L, "pet_index", entity.pet_index);
    set_number(L, "render_flags", entity.render_flags);
    set_number(L, "name_flags", entity.name_flags);
    set_bool(L, "valid_target", (entity.render_flags & 0x200) != 0);
    // where its name is on the screen (vana.world.name_at's), left out while it isn't drawn
    float name_x = 0, name_top = 0, name_bottom = 0;
    if (widgets::name_on_screen(entity.index, entity.x, entity.y, entity.z, name_x, name_top, name_bottom)) {
        set_number(L, "name_x", name_x);
        set_number(L, "name_top", name_top);
        set_number(L, "name_bottom", name_bottom);
    }
    set_list(L, "models", entity.models, 9);
    set_more(L, entity);
    entityhp::Vitals vitals;
    if (entityhp::vitals(entity.id, vitals))
        set_vitals(L, vitals);
    else {
        // outside the party only the percent is known, unless the server was asked (vana.game.request_max_hp)
        set_one_max(L, "hp", maxhp::known(entity.id));
        set_one_max(L, "mp", {});
    }
}

// vana.game.entity(index)
int game_entity(lua_State* L)
{
    const auto index = static_cast<unsigned>(luaL_checkinteger(L, 1));
    game::Entity entity;
    if (game::entity(index, entity))
        push_entity(L, entity);
    else
        lua_pushnil(L);
    return 1;
}

// vana.game.entity_by_id(id)
int game_entity_by_id(lua_State* L)
{
    const auto id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
    game::Entity entity;
    if (const unsigned index = game::entity_index_by_id(id); index != 0 && game::entity(index, entity) && entity.id == id)
        push_entity(L, entity);
    else
        lua_pushnil(L);
    return 1;
}

// vana.game.vitals(id)
int game_vitals(lua_State* L)
{
    const auto id = static_cast<std::uint32_t>(luaL_checknumber(L, 1));
    entityhp::Vitals vitals;
    if (!entityhp::vitals(id, vitals)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 16);
    set_number(L, "id", vitals.id);
    set_vitals(L, vitals);
    return 1;
}

} // namespace

void set_max(lua_State* L, std::uint32_t id)
{
    entityhp::Vitals vitals;
    if (entityhp::vitals(id, vitals)) {
        set_one_max(L, "hp", vitals.hp_max);
        set_one_max(L, "mp", vitals.mp_max);
    } else {
        set_one_max(L, "hp", {});
        set_one_max(L, "mp", {});
    }
}

void add(lua_State* L)
{
    lua_pushcfunction(L, &game_entity);
    lua_setfield(L, -2, "entity");
    lua_pushcfunction(L, &game_entity_by_id);
    lua_setfield(L, -2, "entity_by_id");
    lua_pushcfunction(L, &game_vitals);
    lua_setfield(L, -2, "vitals");
}

} // namespace vanacore::lua_entity
