#pragma once

#include <cstdint>
#include <string>
#include <vector>

// FFXI's own data, read from FFXiMain.dll's memory: entities, the party, the player and the target.
// Where each lives is found by pattern the first time it's asked for (FFXiMain.dll has decrypted
// itself by then). Every read is a protected copy, so a structure that isn't there yet (before
// login, while zoning) reads as missing instead of crashing. Game thread only.
namespace vanacore::game {

// Anything with a target index: players, NPCs, monsters, pets, doors.
struct Entity {
    unsigned index = 0;      // target index
    std::uint32_t id = 0;    // server id
    std::string name;        // as the game has it (Shift-JIS)
    float x = 0.0f;          // position; z is height
    float y = 0.0f;
    float z = 0.0f;
    float heading = 0.0f;    // radians
    float distance = 0.0f;   // to the local player, squared yalms (the game's own value)
    float speed = 0.0f;      // movement speed
    std::uint8_t hpp = 0;
    std::uint8_t type = 0;   // 0 PC, 1 NPC, 2 NPC with a fixed model, 3 door or similar
    std::uint8_t race = 0;
    std::uint32_t status = 0;        // the client's own (Ashita's Status), synced from status_server when the client wants
    std::uint32_t status_server = 0; // as the server last sent it (StatusServer; the player's from 0x037): 4 in every event
    std::uint32_t spawn_flags = 0; // 0x01 PC, 0x02 NPC, 0x10 monster, 0x0D the local player
    float model_size = 0.0f;
    float hitbox_size = 0.0f;
    std::uint32_t claim = 0;       // claim status (low word: the claimer's id)
    std::uint16_t target_index = 0; // what it's targeting (not kept for the local player)
    std::uint16_t pet_index = 0;
    std::uint32_t render_flags = 0; // Flags0: 0x200 set while it can be targeted
    std::uint32_t name_flags = 0;   // Flags1
    std::uint16_t models[9] = {};   // hair, head, body, hands, legs, feet, main, sub, ranged
    // What the entity record holds beyond the above (Ashita's entity_t, the offsets read against its struct on 2026-11-14).
    float anim_speed = 0.0f;
    std::uint32_t door_id = 0;
    std::uint32_t render[9] = {};   // Render.Flags0 to Flags8 (render_flags and name_flags are the first two)
    std::uint16_t costume_id = 0;
    std::uint32_t status_event = 0; // the status while it talks to an NPC or is in an event
    std::uint32_t zone_id = 0;      // only set for the local player, and under conditions
    std::uint32_t animations[10] = {};
    std::uint16_t animation_time = 0;
    std::uint16_t animation_step = 0;
    std::uint8_t animation_play = 0;
    std::uint16_t emote_target_index = 0;
    std::uint16_t emote_id = 0;
    std::uint32_t linkshell_color = 0; // BGR
    std::uint16_t name_color = 0;
    std::uint8_t campaign_name_flag = 0;
    std::uint8_t mount_id = 0;
    std::uint32_t last_action_id = 0;
    std::uint16_t interaction_target_index = 0; // who it last talked to
    std::uint8_t ballista_flags = 0;
    std::uint16_t fellow_index = 0;        // its adventuring fellow
    std::uint16_t trust_owner_index = 0;   // a trust's owner
};

// The entity at target index `index` (0 to entity_count() - 1). False if there isn't one.
bool entity(unsigned index, Entity& out);
// The entities within `within` yalms of the player, up to `most`: 4 floats each into out, x, height (Entity's z), y, then
// a character's (a player, NPC or monster: entity type 0 or 1) hitbox size, or -1 for a fixed model or a door (type 2 or
// 3: a home point, a waypoint, a streetlamp). Light enough for every frame (no names read). How many.
unsigned entity_places(float* out, unsigned most, float within);
// The player characters within `within` yalms of the player that show a GM's icon (their name flags' GM rank, Flags2 bits
// 12-14) and aren't hidden or invisible (Flags2 0x1C0), up to `most`: 4 floats each into out, x, height (Entity's z), y,
// then the race (1-8). Light enough for every frame. How many.
unsigned gm_places(float* out, unsigned most, float within);
// How many entity slots the game has (0x900), or 0 before FFXiMain.dll is found.
unsigned entity_count();
// The target index of the live entity with this server id: 0 for none. Looks at every slot (as Ashita's GetEntityIndexById
// does), so ask when it's needed, not every frame, and keep the index.
unsigned entity_index_by_id(std::uint32_t id);

struct PartyMember {
    bool active = false;
    unsigned slot = 0;        // 0-17: party 1, then alliance parties 2 and 3
    std::string name;
    std::uint32_t id = 0;
    unsigned target_index = 0;
    std::uint32_t hp = 0;
    std::uint32_t mp = 0;
    std::uint32_t tp = 0;
    std::uint8_t hpp = 0;
    std::uint8_t mpp = 0;
    std::uint16_t zone = 0;
    std::uint8_t main_job = 0;
    std::uint8_t main_job_level = 0;
    std::uint8_t sub_job = 0;
    std::uint8_t sub_job_level = 0;
    std::uint32_t flags = 0;
};

struct Alliance {
    std::uint32_t leader = 0;             // the alliance leader's server id
    std::uint32_t party_leaders[3] = {};  // each party's leader
    std::int8_t party_counts[3] = {};     // members in each party
};

// What the game says of each party and alliance slot's health, with no names read: light enough for every frame (entityhp.cpp
// samples it each frame). `slot` 0-17 as party(); a 7th member of party 1 is in its slot here (party() reads it as empty).
struct PartyVital {
    bool active = false;
    std::uint32_t id = 0;
    unsigned target_index = 0;
    std::uint32_t hp = 0, mp = 0, tp = 0;
    std::uint8_t hpp = 0, mpp = 0;
};
// False if the party data can't be read.
bool party_vitals(PartyVital (&members)[18]);
// The local player's maximum HP and MP: the game's own numbers (the server's 0x061, sent again when they change). False while
// the player's stats can't be read.
bool player_max(std::uint32_t& hp_max, std::uint32_t& mp_max);

// The 18 party and alliance slots (party 1 is slots 0-5, slot 0 the local player). False if the
// party data can't be read. A 7th member of party 1 reads as an empty slot and the count as 6 (party_overflow).
bool party(PartyMember (&members)[18], Alliance& alliance);

// How many members party 1 has past its 6, from the game's party counts. LSB lets a 4th trust in while a member is out of
// the zone (its trust check counts the zone's members), so a party can hold 7: the game counts 7 and keeps the 7th in
// party 2's first slot, but its list draws 6. Addons place rows by the count (tparty's numbers sat a row high), so these
// slots (6 on) read as empty and the count as 6. Only without party 2, whose slots they take.
inline unsigned party_overflow(int count1, int count2)
{
    return count1 > 6 && count2 == 0 ? static_cast<unsigned>(count1 > 12 ? 6 : count1 - 6) : 0;
}

// The logged in character's name (as the game has it, Shift-JIS), "" before login, at the character select and while the
// game has no player: the player's party slot and entity agree.
std::string local_player_name();

// The same character's id (0 when local_player_name() is empty): the name, in hex, of its folder under USER.
std::uint32_t local_player_id();

// The game's copy of the player's status (its 0x438-byte STATUS_DATA: the server's 0x061 first, then what the other
// packets fill in).
struct Player {
    std::uint32_t hp_max = 0;
    std::uint32_t mp_max = 0;
    std::uint8_t main_job = 0;
    std::uint8_t main_job_level = 0;
    std::uint8_t sub_job = 0;
    std::uint8_t sub_job_level = 0;
    std::uint16_t exp = 0;                // experience points into the level, and the level's total
    std::uint16_t exp_needed = 0;
    std::int16_t stats[7] = {};           // STR, DEX, VIT, AGI, INT, MND, CHR
    std::int16_t stat_modifiers[7] = {};
    std::int16_t attack = 0;
    std::int16_t defense = 0;
    std::int16_t resists[8] = {};         // fire, ice, wind, earth, lightning, water, light, dark
    std::uint16_t title = 0;
    std::uint16_t rank = 0;               // the nation rank, and the rank points toward the next
    std::uint16_t rank_points = 0;
    std::uint16_t homepoint = 0;          // the home point's zone
    std::uint8_t nation = 0;
    std::uint8_t residence = 0;
    std::uint8_t su_level = 0;            // superior equipment level
    std::uint8_t item_level_highest = 0;  // item levels: the highest equipped, the average's part over 99, main hand, ranged
    std::uint8_t item_level = 0;
    std::uint8_t item_level_main = 0;
    std::uint8_t item_level_ranged = 0;
    std::uint8_t unity_faction = 0;
    std::uint32_t unity_points = 0;
    std::uint16_t unity_evaluation = 0;   // personal evaluation points, and the partial ones toward the next
    std::uint16_t unity_evaluation_partial = 0;
    std::uint32_t unity_chat_color = 0;
    std::uint8_t mastery_job = 0;         // job mastery: the job, its master level, flags, exp into the level and the total
    std::uint8_t mastery_level = 0;
    std::uint8_t mastery_flags = 0;
    std::uint32_t mastery_exp = 0;
    std::uint32_t mastery_exp_needed = 0;
    std::uint16_t combat_skills[48] = {}; // by skill id: the level in the low 15 bits, 0x8000 while capped
    std::uint16_t craft_skills[16] = {};  // by craft (0 fishing ... 11 digging): the rank in the low 5 bits, the level in
                                          // bits 5 to 12, 0x8000 while capped
    std::uint32_t mount_recast = 0;
    std::uint32_t mount_recast_timer = 0;
    std::uint8_t data_loaded = 0;         // the game's flags for which of this the server has sent
    std::uint16_t limit_points = 0;
    std::uint8_t merit_points = 0;
    std::uint8_t merit_points_max = 0;
    std::uint8_t assimilation_points = 0;
    bool limit_breaker = false;           // merit points unlocked
    bool exp_locked = false;              // at the level cap (set with limit mode too)
    bool limit_mode = false;
    struct JobPoints {
        std::uint16_t capacity = 0, points = 0, spent = 0;
    };
    JobPoints job_points[24] = {};        // by job id
    std::uint8_t homepoints[64] = {};     // the home points known, a bit each
    std::int16_t status_icons[32] = {};   // the status timers' icons, and their ends as the server's 0x063 gave them
    std::int32_t status_timers[32] = {};
    bool zoning = false;
    std::uint32_t sub_map = 0;
    std::int16_t buffs[32] = {};          // status effect ids; -1 or 255 for none
};

// The local player's stats and jobs (hp, mp and tp are in party slot 0). False if unreadable. The jobs are the server's
// (its last zone-in 0x00A or job info 0x01B) while the game's still read 0, as they do for a moment after logging in.
bool player(Player& out);

// The player's pet as the game's copy of its last status packet (0x068) has it: index 0 while there's none.
struct Pet {
    unsigned index = 0;          // the pet's target index (0: no pet)
    std::uint8_t hpp = 0;
    std::uint8_t mpp = 0;
    std::uint16_t tp = 0;
    std::uint32_t target_id = 0; // what it's fighting (server id; 0 for nothing)
    unsigned owner_index = 0;    // the owner's target index and server id, as the packet named them
    std::uint32_t owner_id = 0;
};
// False while the game's copy isn't found.
bool pet(Pet& out);

// Each job's level and master level, by job id (0 to 23), from the game's own getters. False while they aren't found.
bool job_levels(std::uint8_t levels[24], std::uint8_t master_levels[24]);

// The current missions, as the game keeps them from the server's mission log (0x056, its "current" log).
struct Missions {
    std::uint8_t nation = 0;             // 0 San d'Oria, 1 Bastok, 2 Windurst
    std::uint16_t nation_mission = 0;    // 0xFFFF while none is under way
    std::uint32_t zilart = 0;            // Rise of the Zilart
    std::uint32_t promathia = 0;         // Chains of Promathia, and its progress (a nibble a chapter)
    std::uint32_t promathia_progress = 0;
    std::uint32_t addons = 0;            // A Crystalline Prophecy in bits 0-3, A Moogle Kupo d'Etat 4-7, A Shantotto Ascension 8-11
    std::uint32_t adoulin = 0;           // Seekers of Adoulin
    std::uint32_t rhapsodies = 0;        // Rhapsodies of Vana'diel
    std::uint32_t voracious = 0;         // The Voracious Resurgence (the 0xFFFE log's word)
    std::uint32_t declined = 0;          // expansions turned down: bit 0 Zilart, 1 ACP, 2 ASA, 3 Promathia, 5 Adoulin, 6 Rhapsodies
    std::uint32_t loaded = 0;            // the game's flags for which logs have come (bit 31: the current missions)
};
// False while the game's copy isn't found.
bool missions(Missions& out);

// A linkshell as its last message packet (0x0CC) described it. The game keeps no copy an addon can read, so VanaCore does.
struct Linkshell {
    bool known = false;          // a message for this slot has come since login
    std::string name;
    std::string message;         // Shift-JIS, as the server sent it
    std::string poster;          // who last set the message
    std::uint32_t time = 0;      // when (Unix seconds)
    std::uint8_t read_level = 0, write_level = 0, public_edit_level = 0;
};
// Slot 0 (the linkshell) or 1 (linkshell 2). Any thread.
Linkshell linkshell(unsigned slot);
// A linkshell's name as the server packs it in its message and in a pearl's extra data: 6 bits a character, high bits
// first (1-26 a-z, 27-52 A-Z, 53-62 0-9), to 20 characters.
std::string linkshell_name(const std::uint8_t* packed, size_t size);

// A merit as the server listed it (0x08C): its id, how many are bought and the next one's cost.
struct Merit {
    std::uint16_t id = 0;
    std::uint8_t count = 0;
    std::uint8_t next = 0;
};
// Every merit the server has listed since login, by id. Any thread.
std::vector<Merit> merits();

// Each packet as it comes (from packets.cpp): keeps what the game has no readable copy of (linkshells, merits). Any thread.
void on_packet(bool incoming, const std::uint8_t* packet, size_t length);

struct Target {
    unsigned index = 0;       // main target (0 if none)
    std::uint32_t id = 0;
    bool active = false;
    unsigned sub_index = 0;   // sub target (<st>), when is_sub_target is set
    std::uint32_t sub_id = 0;
    bool sub_active = false;
    bool is_sub_target = false; // a sub target is being chosen
    bool locked = false;        // locked on
    bool menu_open = false;     // a game menu is open
    // The game's two entries as they are (slot 0 is the sub target while one is being chosen).
    unsigned raw_index[2] = {};
    std::uint32_t raw_id[2] = {};
    bool raw_active[2] = {};
};

// The target window's state. False if unreadable.
bool target(Target& out);

// Where the game keeps these structures, for code that reads them in place (0 for any not found).
struct Addresses {
    std::uintptr_t entities = 0;      // the array of entity pointers
    unsigned entity_count = 0;
    std::uintptr_t party = 0;         // the 18 party members
    std::uintptr_t player = 0;        // the player's stats
    std::uintptr_t target_pointer = 0; // the global holding the target data's address
};
Addresses addresses();

} // namespace vanacore::game
