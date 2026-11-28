#include "game.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "log.hpp"
#include "packets.hpp"
#include "scan.hpp"

namespace vanacore::game {
namespace {

// Where FFXI keeps its data (FFXiMain.dll, CatsEyeXI's client). Each pattern is a piece of the
// game's own code that uses the structure; the address is read out of the instruction.

// mov edx,[esi+0Ch] / mov eax,[edx+ebp] / mov eax,[eax*4+entities]: the array of entity pointers.
constexpr char kEntitiesPattern[] = "8B 56 0C 8B 04 2A 8B 04 85";
constexpr size_t kEntitiesOffset = 9;
// inc edi / cmp edi,900h / jl: the number of entity slots.
constexpr char kEntityCountPattern[] = "47 81 FF ?? ?? ?? ?? 7C ?? 89";
constexpr size_t kEntityCountOffset = 3;
// ... lea eax,[ecx*4+members]: the 18 party members (0x7C bytes each).
constexpr char kPartyPattern[] = "0F BE C3 8D 0C 52 56 57 8B F5 8D 04 48";
constexpr size_t kPartyOffset = 23;
// ... call / mov eax,[player]: the player's stats (HPMax first).
constexpr char kPlayerPattern[] = "6A 01 8D 44 24 2C 68 80 80 80 80 66 83 ?? ?? 50 66 83 ?? ?? 51 52 E8 ?? ?? ?? ?? A1";
constexpr size_t kPlayerOffset = 28;
// mov [esp+16h],al / mov eax,[target] / xor bl,bl / mov cl,[eax+22h]: the global pointing to the target data (0xD4 bytes,
// allocated by the game), read from the code. One match in CatsEyeXI's client and retail's of 2026-09 and 2026-11.
constexpr char kTargetPointerPattern[] = "13 00 88 44 24 16 A1 ?? ?? ?? ?? 32 DB 8A 48 22 8B 46 70 85 C0 88 4C 24 17";
constexpr size_t kTargetPointerOffset = 7;
// Without it: ... mov ecx,[global], the global kTargetPointer bytes before that one. The distance is CatsEyeXI's and
// retail's of 2026-09; retail's of 2026-11 has it 0x2F0 (a global before it went), and its target index read garbage.
constexpr char kTargetPattern[] = "53 56 8B F1 8B 48 04 33 DB 3B CB 75 06 5E 33 C0 5B 59 C3 8B 0D";
constexpr size_t kTargetOffset = 45;
constexpr size_t kTargetPointer = 0x2F4;
// The pet status packet's handler (0x068): mov eax,[esp+0Ch] / push esi / push edi / mov cx,[eax+4] ... mov edi,pet /
// rep movsd: the game's copy of the packet from its byte 4 (24 bytes: the owner's index at +2 and id at +4, the pet's
// index at +8, HP% +0xA, MP% +0xB, TP +0xC, its target's id +0x10).
constexpr char kPetPattern[] = "8B 44 24 0C 56 57 66 8B 48 04 8D 70 04 8B C1 83 E0 3F 83 E8 02";
constexpr size_t kPetOffset = 61;
// mov ecx,[character] / test ecx,ecx / je / mov eax,[esp+4] / cmp al,10h ...: uint8_t __cdecl (int job), a job's level
// (the ones Ashita's IPlayer::GetJobLevel calls), and with cmp al,18h its master level.
constexpr char kJobLevelPattern[] = "8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 10 73 ?? 25 FF 00 00 00";
constexpr char kJobMasterLevelPattern[] = "8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 18 73 ?? 25 FF 00 00 00";
// The mission log packet's handler (0x056): mov edx,[esp+0Ch] / push esi / push edi / mov ax,[edx+24h] / cmp ax,0FFFFh /
// jne / mov al,[edx+4] / mov [nation],al: the current missions, kept 6 bytes before the nation byte (Zilart first).
constexpr char kMissionsPattern[] = "8B 54 24 0C 56 57 66 8B 42 24 66 3D FF FF 0F 85 ?? ?? ?? ?? 8A 42 04 A2";
constexpr size_t kMissionsOffset = 24;
constexpr size_t kMissionsNation = 6;
constexpr size_t kMissionsSize = 0x28;
namespace missions_field {
constexpr size_t kZilart = 0x00, kNationMission = 0x04, kNation = 0x06, kPromathia = 0x08, kPromathiaProgress = 0x0C,
                 kAddons = 0x10, kAdoulin = 0x14, kRhapsodies = 0x18, kVoracious = 0x1C, kDeclined = 0x20, kLoaded = 0x24;
}
constexpr size_t kPetSize = 0x18;
namespace pet_field {
constexpr size_t kOwnerIndex = 0x02, kOwnerId = 0x04, kIndex = 0x08, kHpp = 0x0A, kMpp = 0x0B, kTp = 0x0C, kTargetId = 0x10;
}

// Entity fields (0x20C bytes read).
constexpr size_t kEntitySize = 0x20C;
namespace entity_field {
constexpr size_t kX = 0x04, kZ = 0x08, kY = 0x0C, kTargetIndex = 0x74, kServerId = 0x78, kName = 0x7C,
                 kNameLength = 24, kSpeed = 0x98, kDistance = 0xD8, kHeading = 0xE4, kHpp = 0xEC, kType = 0xEE,
                 kRace = 0xEF, kModels = 0xFC, kRenderFlags = 0x120, kNameFlags = 0x124, kNameFlags2 = 0x128, kStatusServer = 0x16C, kStatus = 0x170,
                 kClaim = 0x188, kSpawnFlags = 0x1D0, kTargetedIndex = 0x1F8, kPetIndex = 0x1FA,
                 kModelSize = 0x204, kHitboxSize = 0x208;
// The rest of the record (0x2AC bytes in all; the last words past the 0x20C read are read apart, so a record that ends
// early only loses them)
constexpr size_t kAnimSpeed = 0x9C, kDoorId = 0xF8, kCostume = 0x166, kInteractionTarget = 0x146, kStatusEvent = 0x174,
                 kZoneId = 0x18C, kAnimations = 0x190, kAnimationTime = 0x1B8, kAnimationStep = 0x1BA,
                 kAnimationPlay = 0x1BC, kEmoteTarget = 0x1BE, kEmoteId = 0x1C0, kLinkshellColor = 0x1D4, kNameColor = 0x1D8,
                 kCampaign = 0x1DA, kMountId = 0x1DB, kLastActionId = 0x1EC, kBallista = 0x1FF;
constexpr size_t kTail = 0x2A0, kTailSize = 8, kFellow = 0, kTrustOwner = 4; // (kFellow and kTrustOwner from kTail)
}
// Party member fields (0x7C bytes each), and the alliance record they point to.
constexpr size_t kMemberSize = 0x7C;
namespace member_field {
constexpr size_t kAlliance = 0x00, kName = 0x06, kNameLength = 18, kServerId = 0x18, kTargetIndex = 0x1C, kHp = 0x24,
                 kMp = 0x28, kTp = 0x2C, kHpp = 0x30, kMpp = 0x31, kZone = 0x32, kFlags = 0x38, kMainJob = 0x6D,
                 kMainJobLevel = 0x6E, kSubJob = 0x6F, kSubJobLevel = 0x70, kActive = 0x7A;
}
namespace alliance_field {
constexpr size_t kLeader = 0x00, kPartyLeaders = 0x04, kPartyCounts = 0x13;
}
// Player fields (0x438 bytes: Ashita's player_t, read live against the game's on 2026-10-25).
constexpr size_t kPlayerSize = 0x438;
namespace player_field {
constexpr size_t kHpMax = 0x00, kMpMax = 0x04, kMainJob = 0x08, kMainJobLevel = 0x09, kSubJob = 0x0A, kSubJobLevel = 0x0B,
                 kExp = 0x0C, kExpNeeded = 0x0E, kStats = 0x10, kStatModifiers = 0x1E, kAttack = 0x2C, kDefense = 0x2E,
                 kResists = 0x30, kTitle = 0x40, kRank = 0x42, kRankPoints = 0x44, kHomepoint = 0x46, kNation = 0x4C,
                 kResidence = 0x4D, kSuLevel = 0x4E, kItemLevels = 0x50, kUnity = 0x54, kUnityEvaluationPartial = 0x58,
                 kUnityEvaluation = 0x5A, kUnityChatColor = 0x5C, kMastery = 0x60, kMasteryExp = 0x64, kMasteryExpNeeded = 0x68,
                 kCombatSkills = 0x6C, kCraftSkills = 0xCC, kMountRecast = 0x1E4, kDataLoaded = 0x1EC, kLimitPoints = 0x1EE,
                 kMeritBits = 0x1F0, kMeritPointsMax = 0x1F2, kJobPoints = 0x1F8, kJobPointsEntry = 6, kHomepoints = 0x28C,
                 kStatusIcons = 0x2CC, kStatusTimers = 0x30C, kZoning = 0x3AC, kSubMap = 0x3F0, kBuffs = 0x3F8;
}
// Target fields (two 0x28-byte entries, then flags).
constexpr size_t kTargetSize = 0x78;
namespace target_field {
constexpr size_t kEntry = 0x28, kIndex = 0x00, kServerId = 0x04, kActive = 0x20, kIsSubTarget = 0x52, kLocked = 0x5C,
                 kMenuOpen = 0x74;
}

bool g_resolved = false;
std::uintptr_t g_entities = 0;  // address of the entity pointer array
unsigned g_entity_count = 0;
std::uintptr_t g_party = 0;     // address of the first party member
std::uintptr_t g_player = 0;    // address of the player's stats
std::uintptr_t g_target_pointer = 0; // the global pointing to the target data
std::uintptr_t g_pet = 0;       // the game's copy of the pet's status packet
std::uintptr_t g_missions = 0;  // the current missions
std::uintptr_t g_job_level = 0; // the game's getters: a job's level, and its master level
std::uintptr_t g_job_master_level = 0;

bool copy(void* out, std::uintptr_t address, size_t size)
{
    if (address == 0)
        return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
T get(const unsigned char* raw, size_t offset)
{
    T value;
    std::memcpy(&value, raw + offset, sizeof(T));
    return value;
}

std::string text(const unsigned char* raw, size_t offset, size_t length)
{
    const char* start = reinterpret_cast<const char*>(raw + offset);
    return std::string(start, strnlen(start, length));
}

// The 32-bit value in the game's code at pattern + offset (0 if the pattern isn't there).
std::uintptr_t operand(HMODULE game, const char* pattern, size_t offset, const wchar_t* what)
{
    const std::uintptr_t found = scan::find(game, pattern);
    std::uint32_t value = 0;
    if (found == 0 || !copy(&value, found + offset, sizeof(value)))
        log::write(L"Game data: %ls not found in FFXiMain.dll", what);
    return value;
}

void resolve()
{
    if (g_resolved)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return; // not yet: try again next time
    g_resolved = true;
    g_entities = operand(game, kEntitiesPattern, kEntitiesOffset, L"the entity list");
    g_entity_count = static_cast<unsigned>(operand(game, kEntityCountPattern, kEntityCountOffset, L"the entity count"));
    if (g_entity_count == 0 || g_entity_count > 0x4000)
        g_entity_count = g_entities != 0 ? 0x900 : 0;
    g_party = operand(game, kPartyPattern, kPartyOffset, L"the party");
    g_player = operand(game, kPlayerPattern, kPlayerOffset, L"the player");
    if (scan::find(game, kTargetPointerPattern) != 0)
        g_target_pointer = operand(game, kTargetPointerPattern, kTargetPointerOffset, L"the target");
    else if (const std::uintptr_t base = operand(game, kTargetPattern, kTargetOffset, L"the target"))
        g_target_pointer = base + kTargetPointer;
    g_pet = operand(game, kPetPattern, kPetOffset, L"the pet");
    if (const std::uintptr_t nation = operand(game, kMissionsPattern, kMissionsOffset, L"the missions"))
        g_missions = nation - kMissionsNation;
    g_job_level = scan::find(game, kJobLevelPattern);
    g_job_master_level = scan::find(game, kJobMasterLevelPattern);
    if (g_job_level == 0 || g_job_master_level == 0)
        log::write(L"Game data: the job level getters not found in FFXiMain.dll");
    const auto at = [&](std::uintptr_t address) {
        return address >= reinterpret_cast<std::uintptr_t>(game) ? address - reinterpret_cast<std::uintptr_t>(game) : 0;
    };
    log::write(L"Game data: entities at FFXiMain.dll+0x%X (%u), party +0x%X, player +0x%X, target +0x%X", at(g_entities),
               g_entity_count, at(g_party), at(g_player), at(g_target_pointer));
}

// The record's fields past the first dozen (see Entity), `raw` its first kEntitySize bytes, `pointer` where it is
void read_more(const unsigned char* raw, std::uintptr_t pointer, Entity& out)
{
    using namespace entity_field;
    out.anim_speed = get<float>(raw, kAnimSpeed);
    out.door_id = get<std::uint32_t>(raw, kDoorId);
    std::memcpy(out.render, raw + kRenderFlags, sizeof(out.render));
    out.costume_id = get<std::uint16_t>(raw, kCostume);
    out.status_event = get<std::uint32_t>(raw, kStatusEvent);
    out.zone_id = get<std::uint32_t>(raw, kZoneId);
    std::memcpy(out.animations, raw + kAnimations, sizeof(out.animations));
    out.animation_time = get<std::uint16_t>(raw, kAnimationTime);
    out.animation_step = get<std::uint16_t>(raw, kAnimationStep);
    out.animation_play = raw[kAnimationPlay];
    out.emote_target_index = get<std::uint16_t>(raw, kEmoteTarget);
    out.emote_id = get<std::uint16_t>(raw, kEmoteId);
    out.linkshell_color = get<std::uint32_t>(raw, kLinkshellColor);
    out.name_color = get<std::uint16_t>(raw, kNameColor);
    out.campaign_name_flag = raw[kCampaign];
    out.mount_id = raw[kMountId];
    out.last_action_id = get<std::uint32_t>(raw, kLastActionId);
    out.interaction_target_index = get<std::uint16_t>(raw, kInteractionTarget);
    out.ballista_flags = raw[kBallista];
    unsigned char tail[kTailSize] = {};
    if (copy(tail, pointer + kTail, sizeof(tail))) {
        out.fellow_index = get<std::uint16_t>(tail, kFellow);
        out.trust_owner_index = get<std::uint16_t>(tail, kTrustOwner);
    } else {
        out.fellow_index = out.trust_owner_index = 0;
    }
}

} // namespace

unsigned entity_count()
{
    resolve();
    return g_entity_count;
}

bool entity(unsigned index, Entity& out)
{
    resolve();
    if (g_entities == 0 || index >= g_entity_count)
        return false;
    std::uint32_t pointer = 0;
    unsigned char raw[kEntitySize];
    if (!copy(&pointer, g_entities + index * 4, sizeof(pointer)) || pointer == 0 || !copy(raw, pointer, kEntitySize))
        return false;
    using namespace entity_field;
    out.index = get<std::uint32_t>(raw, kTargetIndex);
    out.id = get<std::uint32_t>(raw, kServerId);
    out.name = text(raw, kName, kNameLength);
    out.x = get<float>(raw, kX);
    out.y = get<float>(raw, kY);
    out.z = get<float>(raw, kZ);
    out.heading = get<float>(raw, kHeading);
    out.distance = get<float>(raw, kDistance);
    out.speed = get<float>(raw, kSpeed);
    out.hpp = raw[kHpp];
    out.type = raw[kType];
    out.race = raw[kRace];
    out.status = get<std::uint32_t>(raw, kStatus);
    out.status_server = get<std::uint32_t>(raw, kStatusServer);
    out.spawn_flags = get<std::uint32_t>(raw, kSpawnFlags);
    out.model_size = get<float>(raw, kModelSize);
    out.hitbox_size = get<float>(raw, kHitboxSize);
    out.claim = get<std::uint32_t>(raw, kClaim);
    out.target_index = get<std::uint16_t>(raw, kTargetedIndex);
    out.pet_index = get<std::uint16_t>(raw, kPetIndex);
    out.render_flags = get<std::uint32_t>(raw, kRenderFlags);
    out.name_flags = get<std::uint32_t>(raw, kNameFlags);
    std::memcpy(out.models, raw + kModels, sizeof(out.models));
    read_more(raw, pointer, out);
    // A slot the game reuses keeps its index; anything else isn't a live entity.
    return out.index == index;
}

unsigned entity_index_by_id(std::uint32_t id)
{
    resolve();
    if (id == 0 || g_entities == 0 || g_entity_count == 0)
        return 0;
    static std::uint32_t pointers[0x900];
    const unsigned count = g_entity_count < 0x900 ? g_entity_count : 0x900;
    if (!copy(pointers, g_entities, count * 4))
        return 0;
    using namespace entity_field;
    for (unsigned index = 0; index < count; ++index) {
        std::uint32_t found[2] = {}; // the target index, then the server id
        if (pointers[index] != 0 && copy(found, pointers[index] + kTargetIndex, sizeof(found)) && found[0] == index &&
            found[1] == id)
            return index;
    }
    return 0;
}

unsigned entity_places(float* out, unsigned most, float within)
{
    resolve();
    if (g_entities == 0 || g_entity_count == 0 || most == 0)
        return 0;
    static std::uint32_t pointers[0x900];
    const unsigned count = g_entity_count < 0x900 ? g_entity_count : 0x900;
    if (!copy(pointers, g_entities, count * 4))
        return 0;
    using namespace entity_field;
    const float within2 = within * within;
    unsigned n = 0;
    for (unsigned index = 0; index < count && n < most; ++index) {
        if (pointers[index] == 0)
            continue;
        unsigned char raw[kType + 1];
        float hitbox = 0.0f;
        if (!copy(raw, pointers[index], sizeof(raw)) || get<std::uint32_t>(raw, kTargetIndex) != index ||
            get<float>(raw, kDistance) > within2)
            continue;
        float* at = out + n++ * 4;
        at[0] = get<float>(raw, kX), at[1] = get<float>(raw, kZ), at[2] = get<float>(raw, kY);
        if (raw[kType] > 1) { // a fixed model or a door: a home point, a waypoint, a streetlamp
            at[3] = -1.0f;
            continue;
        }
        copy(&hitbox, pointers[index] + kHitboxSize, sizeof(hitbox));
        at[3] = hitbox > 0.0f && hitbox < 20.0f ? hitbox : 0.0f;
    }
    return n;
}

unsigned gm_places(float* out, unsigned most, float within)
{
    resolve();
    if (g_entities == 0 || g_entity_count <= 0x400 || most == 0)
        return 0;
    static std::uint32_t pointers[0x900];
    const unsigned count = g_entity_count < 0x900 ? g_entity_count : 0x900;
    if (!copy(pointers + 0x400, g_entities + 0x400 * 4, (count - 0x400) * 4))
        return 0;
    using namespace entity_field;
    const float within2 = within * within;
    unsigned n = 0;
    for (unsigned index = 0x400; index < count && n < most; ++index) { // the player characters' slots
        unsigned char raw[kNameFlags2 + 4];
        if (pointers[index] == 0 || !copy(raw, pointers[index], sizeof(raw)) || get<std::uint32_t>(raw, kTargetIndex) != index ||
            raw[kType] != 0 || get<float>(raw, kDistance) > within2)
            continue;
        // a GM's icon, and not hidden: a GM hidden (!hide) or invisible has Flags2's 0x1C0 (the server's invisible and silence
        // flags; seen on 2026-10-22, set while hidden and clear once visible). Other players' games are sent a despawn and
        // nothing more for a hidden GM, so there's no entity there at all.
        const std::uint32_t flags2 = get<std::uint32_t>(raw, kNameFlags2);
        if ((flags2 >> 12 & 7) == 0 || (flags2 & 0x1C0) != 0)
            continue;
        float* at = out + n++ * 4;
        at[0] = get<float>(raw, kX), at[1] = get<float>(raw, kZ), at[2] = get<float>(raw, kY);
        at[3] = static_cast<float>(raw[kRace]);
    }
    return n;
}

bool party(PartyMember (&members)[18], Alliance& alliance)
{
    resolve();
    unsigned char raw[kMemberSize * 18];
    if (g_party == 0 || !copy(raw, g_party, sizeof(raw)))
        return false;
    using namespace member_field;
    for (unsigned slot = 0; slot < 18; ++slot) {
        const unsigned char* member = raw + slot * kMemberSize;
        PartyMember& out = members[slot];
        out.slot = slot;
        out.active = member[kActive] != 0;
        out.name = text(member, kName, kNameLength);
        out.id = get<std::uint32_t>(member, kServerId);
        out.target_index = get<std::uint32_t>(member, kTargetIndex);
        out.hp = get<std::uint32_t>(member, kHp);
        out.mp = get<std::uint32_t>(member, kMp);
        out.tp = get<std::uint32_t>(member, kTp);
        out.hpp = member[kHpp];
        out.mpp = member[kMpp];
        out.zone = get<std::uint16_t>(member, kZone);
        out.flags = get<std::uint32_t>(member, kFlags);
        out.main_job = member[kMainJob];
        out.main_job_level = member[kMainJobLevel];
        out.sub_job = member[kSubJob];
        out.sub_job_level = member[kSubJobLevel];
    }
    alliance = {};
    unsigned char info[0x18];
    if (copy(info, get<std::uint32_t>(raw, kAlliance), sizeof(info))) {
        alliance.leader = get<std::uint32_t>(info, alliance_field::kLeader);
        for (int i = 0; i < 3; ++i) {
            alliance.party_leaders[i] = get<std::uint32_t>(info, alliance_field::kPartyLeaders + i * 4);
            alliance.party_counts[i] = static_cast<std::int8_t>(info[alliance_field::kPartyCounts + i]);
        }
    }
    const unsigned overflow = party_overflow(alliance.party_counts[0], alliance.party_counts[1]);
    for (unsigned slot = 6; slot < 6 + overflow; ++slot)
        members[slot] = PartyMember{.slot = slot};
    alliance.party_counts[0] = static_cast<std::int8_t>(alliance.party_counts[0] - overflow);
    return true;
}

bool party_vitals(PartyVital (&members)[18])
{
    resolve();
    unsigned char raw[kMemberSize * 18];
    if (g_party == 0 || !copy(raw, g_party, sizeof(raw)))
        return false;
    using namespace member_field;
    for (unsigned slot = 0; slot < 18; ++slot) {
        const unsigned char* member = raw + slot * kMemberSize;
        PartyVital& out = members[slot];
        out.active = member[kActive] != 0;
        out.id = get<std::uint32_t>(member, kServerId);
        out.target_index = get<std::uint32_t>(member, kTargetIndex);
        out.hp = get<std::uint32_t>(member, kHp);
        out.mp = get<std::uint32_t>(member, kMp);
        out.tp = get<std::uint32_t>(member, kTp);
        out.hpp = member[kHpp];
        out.mpp = member[kMpp];
    }
    return true;
}

bool player_max(std::uint32_t& hp_max, std::uint32_t& mp_max)
{
    resolve();
    std::uint32_t raw[2];
    if (g_player == 0 || !copy(raw, g_player + player_field::kHpMax, sizeof(raw)))
        return false;
    hp_max = raw[0], mp_max = raw[1]; // (kHpMax and kMpMax are the first two words)
    return true;
}

std::string local_player_name()
{
    PartyMember members[18];
    Alliance alliance;
    Entity self;
    if (!party(members, alliance) || !members[0].active || members[0].id == 0 || members[0].target_index == 0 ||
        !entity(members[0].target_index, self) || self.id != members[0].id)
        return {};
    return self.name;
}

std::uint32_t local_player_id()
{
    PartyMember members[18];
    Alliance alliance;
    Entity self;
    if (!party(members, alliance) || !members[0].active || members[0].id == 0 || members[0].target_index == 0 ||
        !entity(members[0].target_index, self) || self.id != members[0].id)
        return 0;
    return members[0].id;
}

bool player(Player& out)
{
    resolve();
    unsigned char raw[kPlayerSize];
    if (g_player == 0 || !copy(raw, g_player, sizeof(raw)))
        return false;
    using namespace player_field;
    out.hp_max = get<std::uint32_t>(raw, kHpMax);
    out.mp_max = get<std::uint32_t>(raw, kMpMax);
    out.main_job = raw[kMainJob];
    out.main_job_level = raw[kMainJobLevel];
    out.sub_job = raw[kSubJob];
    out.sub_job_level = raw[kSubJobLevel];
    // Just after logging in the game has the player in its party list but not their jobs yet: boot.txt's addons, loaded
    // then ([addons] delay.login), read job 0 (LuAshitacast: "Profile not found matching: <name>_NON"). On Ashita they
    // load at the title screen and never see it. The server's newest word on the jobs instead, while still in its zone.
    if (out.main_job == 0) {
        double zone_in_at = 0, zone_out_at = 0, jobs_at = 0;
        const std::string zone_in = packets::last(true, 0x00A, &zone_in_at);
        packets::last(true, 0x00B, &zone_out_at);
        const std::string jobs = packets::last(true, 0x01B, &jobs_at);
        if (!zone_in.empty() && zone_in_at > zone_out_at) {
            if (jobs.size() > 0x0B && jobs_at > zone_in_at) {
                out.main_job = static_cast<std::uint8_t>(jobs[0x08]);
                out.sub_job = static_cast<std::uint8_t>(jobs[0x0B]);
            } else if (zone_in.size() > 0xB7) {
                out.main_job = static_cast<std::uint8_t>(zone_in[0xB4]);
                out.sub_job = static_cast<std::uint8_t>(zone_in[0xB7]);
            }
        }
    }
    out.exp = get<std::uint16_t>(raw, kExp);
    out.exp_needed = get<std::uint16_t>(raw, kExpNeeded);
    std::memcpy(out.stats, raw + kStats, sizeof(out.stats));
    std::memcpy(out.stat_modifiers, raw + kStatModifiers, sizeof(out.stat_modifiers));
    out.attack = get<std::int16_t>(raw, kAttack);
    out.defense = get<std::int16_t>(raw, kDefense);
    std::memcpy(out.resists, raw + kResists, sizeof(out.resists));
    out.title = get<std::uint16_t>(raw, kTitle);
    out.rank = get<std::uint16_t>(raw, kRank);
    out.rank_points = get<std::uint16_t>(raw, kRankPoints);
    out.homepoint = get<std::uint16_t>(raw, kHomepoint);
    out.nation = raw[kNation];
    out.residence = raw[kResidence];
    out.su_level = raw[kSuLevel]; // the word's low byte is the level
    out.item_level_highest = raw[kItemLevels];
    out.item_level = raw[kItemLevels + 1];
    out.item_level_main = raw[kItemLevels + 2];
    out.item_level_ranged = raw[kItemLevels + 3];
    const std::uint32_t unity = get<std::uint32_t>(raw, kUnity); // faction in bits 0-4, points in bits 10-26
    out.unity_faction = static_cast<std::uint8_t>(unity & 0x1F);
    out.unity_points = (unity >> 10) & 0x1FFFF;
    out.unity_evaluation_partial = get<std::uint16_t>(raw, kUnityEvaluationPartial);
    out.unity_evaluation = get<std::uint16_t>(raw, kUnityEvaluation);
    out.unity_chat_color = get<std::uint32_t>(raw, kUnityChatColor);
    out.mastery_job = raw[kMastery];
    out.mastery_level = raw[kMastery + 1];
    out.mastery_flags = raw[kMastery + 2];
    out.mastery_exp = get<std::uint32_t>(raw, kMasteryExp);
    out.mastery_exp_needed = get<std::uint32_t>(raw, kMasteryExpNeeded);
    std::memcpy(out.combat_skills, raw + kCombatSkills, sizeof(out.combat_skills));
    std::memcpy(out.craft_skills, raw + kCraftSkills, sizeof(out.craft_skills));
    out.mount_recast = get<std::uint32_t>(raw, kMountRecast);
    out.mount_recast_timer = get<std::uint32_t>(raw, kMountRecast + 4);
    out.data_loaded = raw[kDataLoaded];
    out.limit_points = get<std::uint16_t>(raw, kLimitPoints);
    // merit points in bits 0-6, assimilation points 7-12, then the limit breaker, exp locked and limit mode flags
    const std::uint16_t merit_bits = get<std::uint16_t>(raw, kMeritBits);
    out.merit_points = static_cast<std::uint8_t>(merit_bits & 0x7F);
    out.assimilation_points = static_cast<std::uint8_t>((merit_bits >> 7) & 0x3F);
    out.limit_breaker = (merit_bits & 0x2000) != 0;
    out.exp_locked = (merit_bits & 0x4000) != 0;
    out.limit_mode = (merit_bits & 0x8000) != 0;
    out.merit_points_max = raw[kMeritPointsMax];
    for (size_t job = 0; job < 24; ++job) {
        const size_t at = kJobPoints + 4 + job * kJobPointsEntry; // a flags dword, then { capacity, points, spent } a job
        out.job_points[job].capacity = get<std::uint16_t>(raw, at);
        out.job_points[job].points = get<std::uint16_t>(raw, at + 2);
        out.job_points[job].spent = get<std::uint16_t>(raw, at + 4);
    }
    std::memcpy(out.homepoints, raw + kHomepoints, sizeof(out.homepoints));
    std::memcpy(out.status_icons, raw + kStatusIcons, sizeof(out.status_icons));
    std::memcpy(out.status_timers, raw + kStatusTimers, sizeof(out.status_timers));
    out.zoning = get<std::uint32_t>(raw, kZoning) != 0;
    out.sub_map = get<std::uint32_t>(raw, kSubMap);
    std::memcpy(out.buffs, raw + kBuffs, sizeof(out.buffs));
    return true;
}

bool pet(Pet& out)
{
    resolve();
    unsigned char raw[kPetSize];
    if (g_pet == 0 || !copy(raw, g_pet, sizeof(raw)))
        return false;
    using namespace pet_field;
    out.owner_index = get<std::uint16_t>(raw, kOwnerIndex);
    out.owner_id = get<std::uint32_t>(raw, kOwnerId);
    out.index = get<std::uint16_t>(raw, kIndex);
    out.hpp = raw[kHpp];
    out.mpp = raw[kMpp];
    out.tp = get<std::uint16_t>(raw, kTp);
    out.target_id = get<std::uint32_t>(raw, kTargetId);
    return true;
}

bool job_levels(std::uint8_t levels[24], std::uint8_t master_levels[24])
{
    resolve();
    if (g_job_level == 0 || g_job_master_level == 0)
        return false;
    const auto level = reinterpret_cast<std::uint8_t(__cdecl*)(int)>(g_job_level);
    const auto master = reinterpret_cast<std::uint8_t(__cdecl*)(int)>(g_job_master_level);
    __try {
        for (int job = 0; job < 24; ++job) {
            levels[job] = level(job);
            master_levels[job] = master(job);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool missions(Missions& out)
{
    resolve();
    unsigned char raw[kMissionsSize];
    if (g_missions == 0 || !copy(raw, g_missions, sizeof(raw)))
        return false;
    using namespace missions_field;
    out.nation = raw[kNation];
    out.nation_mission = get<std::uint16_t>(raw, kNationMission);
    out.zilart = get<std::uint32_t>(raw, kZilart);
    out.promathia = get<std::uint32_t>(raw, kPromathia);
    out.promathia_progress = get<std::uint32_t>(raw, kPromathiaProgress);
    out.addons = get<std::uint32_t>(raw, kAddons);
    out.adoulin = get<std::uint32_t>(raw, kAdoulin);
    out.rhapsodies = get<std::uint32_t>(raw, kRhapsodies);
    out.voracious = get<std::uint32_t>(raw, kVoracious);
    out.declined = get<std::uint32_t>(raw, kDeclined);
    out.loaded = get<std::uint32_t>(raw, kLoaded);
    return true;
}

std::string linkshell_name(const std::uint8_t* packed, size_t size)
{
    std::string name;
    for (size_t i = 0; i < 20; ++i) {
        const size_t bit = i * 6, byte = bit / 8;
        if (byte >= size)
            break;
        const unsigned word = (static_cast<unsigned>(packed[byte]) << 8) | (byte + 1 < size ? packed[byte + 1] : 0u);
        const unsigned value = (word >> (10 - bit % 8)) & 0x3F;
        // The server ends a name with six 1 bits (a 63), except when the name stops four bits short of a byte's end
        // (EncodeStringLinkshell): after 2, 6, 10... letters it writes just 1111 and then zeros, which reads as 60, a "7"
        // ("Exodus" came out "Exodus7"). A real 7 there is followed by the ones of its own padding, never by zeros alone.
        if (value == 60 && bit % 8 == 4) {
            bool padding = byte + 1 >= size || (packed[byte + 1] & 0x3F) == 0;
            for (size_t at = byte + 2; padding && at < size; ++at)
                padding = packed[at] == 0;
            if (padding)
                break;
        }
        if (value >= 1 && value <= 26)
            name += static_cast<char>('a' + value - 1);
        else if (value >= 27 && value <= 52)
            name += static_cast<char>('A' + value - 27);
        else if (value >= 53 && value <= 62)
            name += static_cast<char>('0' + value - 53);
        else
            break; // 0 ends the name; 63 is the server's padding after it
    }
    return name;
}

// What the game keeps no readable copy of, from the packets as they come (on_packet).
namespace {
std::mutex g_store_lock;
Linkshell g_linkshells[2];
std::map<std::uint16_t, Merit> g_merits;
} // namespace

Linkshell linkshell(unsigned slot)
{
    const std::lock_guard lock(g_store_lock);
    return slot < 2 ? g_linkshells[slot] : Linkshell{};
}

std::vector<Merit> merits()
{
    const std::lock_guard lock(g_store_lock);
    std::vector<Merit> list;
    list.reserve(g_merits.size());
    for (const auto& [id, merit] : g_merits)
        list.push_back(merit);
    return list;
}

void on_packet(bool incoming, const std::uint8_t* p, size_t length)
{
    if (!incoming || length < 4)
        return;
    const std::uint16_t id = static_cast<std::uint16_t>((p[0] | (p[1] << 8)) & 0x1FF);
    const auto word = [p](size_t at) { return static_cast<std::uint16_t>(p[at] | (p[at + 1] << 8)); };
    const auto dword = [p](size_t at) { std::uint32_t v; std::memcpy(&v, p + at, 4); return v; };
    const auto text = [p](size_t at, size_t most) { return std::string(reinterpret_cast<const char*>(p + at), strnlen(reinterpret_cast<const char*>(p + at), most)); };
    const std::lock_guard lock(g_store_lock);
    switch (id) {
    case 0x00B: // zone out; byte 4 is 1 for a logout: the character's linkshells and merits go with them
        if (length > 4 && p[4] == 1) {
            g_linkshells[0] = g_linkshells[1] = Linkshell{};
            g_merits.clear();
        }
        break;
    case 0x0CC: { // linkshell message: levels in byte 5 (read, write, public edit, 2 bits each, then the slot), the message
                  // at 8 (128 bytes), its time at 0x88, who set it at 0x8C (16), the packed name at 0xA0 (16)
        if (length < 0xB0)
            break;
        const unsigned slot = (p[5] >> 6) & 3;
        if (slot >= 2)
            break;
        Linkshell& ls = g_linkshells[slot];
        ls.known = true;
        ls.read_level = p[5] & 3;
        ls.write_level = (p[5] >> 2) & 3;
        ls.public_edit_level = (p[5] >> 4) & 3;
        ls.message = text(0x08, 128);
        ls.time = dword(0x88);
        ls.poster = text(0x8C, 16);
        ls.name = linkshell_name(p + 0xA0, 16);
        break;
    }
    case 0x08C: { // merits: a count at 4, then { id, next cost, count } (u16, u8, u8) each; the full list comes in several
        const size_t count = length >= 8 ? std::min<size_t>(word(4), (length - 8) / 4) : 0;
        for (size_t i = 0; i < count; ++i) {
            const size_t at = 8 + i * 4;
            const std::uint16_t merit = word(at);
            if (merit == 0)
                continue;
            g_merits[merit] = Merit{merit, p[at + 3], p[at + 2]};
        }
        break;
    }
    default:
        break;
    }
}

bool target(Target& out)
{
    resolve();
    std::uint32_t address = 0;
    unsigned char raw[kTargetSize];
    if (g_target_pointer == 0 || !copy(&address, g_target_pointer, sizeof(address)) ||
        !copy(raw, address, sizeof(raw)))
        return false;
    using namespace target_field;
    // While a sub target is being chosen, the first entry is the sub target and the second the
    // main target it was chosen from.
    out.is_sub_target = raw[kIsSubTarget] != 0;
    const unsigned char* main = raw + (out.is_sub_target ? kEntry : 0);
    const unsigned char* sub = raw + (out.is_sub_target ? 0 : kEntry);
    out.index = get<std::uint32_t>(main, kIndex);
    out.id = get<std::uint32_t>(main, kServerId);
    out.active = main[kActive] != 0;
    out.sub_index = out.is_sub_target ? get<std::uint32_t>(sub, kIndex) : 0;
    out.sub_id = out.is_sub_target ? get<std::uint32_t>(sub, kServerId) : 0;
    out.sub_active = out.is_sub_target && sub[kActive] != 0;
    out.locked = (get<std::uint32_t>(raw, kLocked) & 1) != 0;
    out.menu_open = raw[kMenuOpen] != 0;
    for (int slot = 0; slot < 2; ++slot) {
        out.raw_index[slot] = get<std::uint32_t>(raw + slot * kEntry, kIndex);
        out.raw_id[slot] = get<std::uint32_t>(raw + slot * kEntry, kServerId);
        out.raw_active[slot] = raw[slot * kEntry + kActive] != 0;
    }
    return true;
}

Addresses addresses()
{
    resolve();
    return {g_entities, g_entity_count, g_party, g_player, g_target_pointer};
}

} // namespace vanacore::game
