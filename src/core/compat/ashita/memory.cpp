// IMemoryManager and the game data objects: entities, the party, the player, the target, the
// inventory, auto-follow and the cast bar. Plugins get the game's own structures (in the SDK's
// layouts, abi.hpp's Ashita::FFXI), read and written in place.

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <utility>

#include "../../game.hpp"
#include "../../packets.hpp"
#include "../../scan.hpp"
#include "backend.hpp"
#include "host.hpp"

namespace vanacore::ashita {
namespace {

// A field's type, for a member path such as Movement.LocalPosition.X.
#define VANACORE_FIELD_TYPE(Struct, field) std::remove_cvref_t<decltype(std::declval<Struct&>().field)>

////////////////////////////////////////////////////////////////////////////////////////////////////
// Where the structures are. The four game.cpp finds come from there; the rest are found here the
// first time they're needed, each from a piece of the game's code that uses it.

// mov eax,[character] / lea edx,[eax+ecx*4+inventory]: the character object and the inventory's
// place in it.
constexpr char kInventoryPattern[] = "A1 ?? ?? ?? ?? 8D 94 88 ?? ?? ?? ?? 89 90 ?? ?? ?? ?? E9";
// ... mov ecx,autofollow: the auto-follow structure.
constexpr char kAutoFollowPattern[] = "8B CF E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B E8 85 ED 75 0C B9";
// ... mov eax,[castbar]: the global holding the cast bar.
constexpr char kCastBarPattern[] = "85 F6 74 ?? A1 ?? ?? ?? ?? 85 C0 74 ?? 8B 48 08";
// mov ecx,3Ch / lea esi,[eax+4] / mov edi,icons: the party's status icons.
constexpr char kStatusIconsPattern[] = "B9 3C 00 00 00 8D 70 04 BF ?? ?? ?? ?? F3 A5";
// The key item check: index < 0x80 (4096 key items; retail has fewer), then the table.
constexpr char kKeyItemsPattern[] = "8B 44 24 04 85 C0 7C ?? 3D ?? ?? ?? ?? 7D ?? 8B 4C 24 08 8B 14 85";
// uint8_t __cdecl (int job): a job's level, and its master level.
constexpr char kJobLevelPattern[] = "8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 10 73 ?? 25 FF 00 00 00";
constexpr char kJobMasterLevelPattern[] = "8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 18 73 ?? 25 FF 00 00 00";
// void* __cdecl (): the abilities' bits once they're loaded, else null: packet 0xAC's 224 bytes (weapon skills, job
// abilities, pet commands, traits: 512, 512, 512 and 256 bits), by ability id.
constexpr char kAbilitiesPattern[] =
    "A1 ?? ?? ?? ?? 85 C0 74 ?? 8A 88 ?? ?? ?? ?? 84 C9 74 ?? 8A 88 ?? ?? ?? ?? 84 C9 74 ?? 05 ?? ?? ?? ?? C3 33 C0 C3";
// void* __cdecl (): the known-spells bits once they're loaded, else null (the third match).
constexpr char kSpellsPattern[] = "A1 ?? ?? ?? ?? 85 C0 75 ?? C3 8A 88 ?? ?? ?? ?? 84 C9 75 ?? 33 C0 C3 05 ?? ?? ?? ?? C3";
// ... mov al,[pet mp]: the pet's MP percent.
constexpr char kPetMpPattern[] = "84 C0 75 03 33 C0 C3 33 C0 A0 ?? ?? ?? ?? C3";
// The pet's info, copied from its packet (mov edi,pet): TP at +0x0C.
constexpr char kPetPattern[] = "8B 44 24 0C 56 57 66 8B 48 04 8D 70 04 8B C1 83 E0 3F 83 E8 02";
// ... mov ecx,recasts / call: the abilities' recasts (Ashita's recast.abilities): 31 timer slots (abilityrecast_t), then
// their timers (uint32_t, 1/60 s) at +0xF8, counted down each frame.
constexpr char kAbilityRecastPattern[] = "89 41 24 E9 ?? ?? ?? ?? 8B 46 ?? 6A 00 6A 00 50 8B CE E8";
// push esi / mov esi,recasts / cmp word [esi],0 / jle / call / sub [esi],ax ... add esi,2 / cmp esi,end: the spells'
// recasts (recast.spells), uint16_t by spell id, 1/60 s, counted down each frame. Both operands are read from the code
// each time they're wanted: cexislots moves the array into its own DLL and makes it 0x1001 ids (the game's is 0x401),
// by writing both operands, after the game's code is there.
constexpr char kSpellRecastPattern[] = "56 BE ?? ?? ?? ?? 66 83 3E 00 ?? ?? ?? ?? ?? ?? ?? 66 29 06";
constexpr size_t kSpellRecastEnd = 0x24; // the cmp esi,end's operand, after add esi,2 (83 C6 02) / cmp esi (81 FE)
// The target window (the PS2 client's g_pTkTarget, a CTkTarget): TargetON's mov ecx,[window] before it names it.
constexpr char kTargetWindowPattern[] = "8B 0D ?? ?? ?? ?? 8B 5C 24 10 8B F8 8A 41 65 84 C0";

struct Places {
    bool resolved = false;
    std::uintptr_t character = 0;      // the global holding the character object
    std::uint32_t inventory = 0;       // the inventory's offset in it
    std::uintptr_t autofollow = 0;
    std::uintptr_t castbar = 0;        // the global holding the cast bar
    std::uintptr_t status_icons = 0;
    std::uintptr_t key_items = 0;      // uint32_t[0x80]
    std::uintptr_t job_level = 0;      // functions
    std::uintptr_t job_master_level = 0;
    std::uintptr_t spells = 0;
    std::uintptr_t abilities = 0; // functions
    std::uintptr_t pet_mp = 0;
    std::uintptr_t pet = 0;
    std::uintptr_t ability_recasts = 0;
    std::uintptr_t spell_recasts = 0;     // the code's mov esi,recasts, its operand at +2 (not the array: see below)
    std::uintptr_t target_window = 0; // the global holding it
};
Places g_places;

std::uint32_t code_dword(std::uintptr_t address)
{
    std::uint32_t value = 0;
    if (address != 0)
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

std::uintptr_t find_in_game(const char* pattern, unsigned count = 0)
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(game) + dos->e_lfanew);
    return scan::find(reinterpret_cast<std::uintptr_t>(game), nt->OptionalHeader.SizeOfImage, pattern, count);
}

// The dword at the pattern's match + offset (0 if it isn't found).
std::uintptr_t operand(const char* pattern, size_t offset, unsigned count = 0)
{
    const std::uintptr_t found = find_in_game(pattern, count);
    return found != 0 ? code_dword(found + offset) : 0;
}

const Places& places()
{
    if (!g_places.resolved && GetModuleHandleW(L"FFXiMain.dll") != nullptr) {
        g_places.resolved = true;
        if (const std::uintptr_t found = find_in_game(kInventoryPattern)) {
            g_places.character = code_dword(found + 1);
            g_places.inventory = code_dword(found + 8);
        }
        g_places.autofollow = operand(kAutoFollowPattern, 25);
        g_places.castbar = operand(kCastBarPattern, 5);
        g_places.status_icons = operand(kStatusIconsPattern, 9);
        g_places.key_items = operand(kKeyItemsPattern, 22);
        g_places.job_level = find_in_game(kJobLevelPattern);
        g_places.job_master_level = find_in_game(kJobMasterLevelPattern);
        g_places.spells = find_in_game(kSpellsPattern, 2);
        g_places.abilities = find_in_game(kAbilitiesPattern);
        g_places.pet_mp = operand(kPetMpPattern, 10);
        g_places.pet = operand(kPetPattern, 61);
        g_places.ability_recasts = operand(kAbilityRecastPattern, 25);
        g_places.spell_recasts = find_in_game(kSpellRecastPattern);
        g_places.target_window = operand(kTargetWindowPattern, 2);
    }
    return g_places;
}

std::uintptr_t deref(std::uintptr_t global)
{
    return global != 0 ? *reinterpret_cast<const std::uintptr_t*>(global) : 0;
}

FFXI::entity_t* entity(std::uint32_t index)
{
    const game::Addresses at = game::addresses();
    if (at.entities == 0 || index >= at.entity_count)
        return nullptr;
    return reinterpret_cast<FFXI::entity_t**>(at.entities)[index];
}

FFXI::party_t* party()
{
    return reinterpret_cast<FFXI::party_t*>(game::addresses().party);
}

FFXI::allianceinfo_t* alliance()
{
    FFXI::party_t* p = party();
    return p != nullptr ? reinterpret_cast<FFXI::allianceinfo_t*>(p->Members[0].AllianceInfo) : nullptr;
}

// Party 1's members past 6 (game::party_overflow): their slots read as empty, the count as 6.
unsigned party_overflow()
{
    const FFXI::allianceinfo_t* a = alliance();
    return a != nullptr ? game::party_overflow(a->PartyMemberCount1, a->PartyMemberCount2) : 0;
}

FFXI::partymember_t* member(std::uint32_t index)
{
    FFXI::party_t* p = party();
    if (p == nullptr || index >= 18)
        return nullptr;
    if (index >= 6 && index < 6 + party_overflow()) {
        static FFXI::partymember_t empty;
        empty = {};
        return &empty;
    }
    return &p->Members[index];
}

FFXI::partystatusicons_t* status_icons()
{
    return reinterpret_cast<FFXI::partystatusicons_t*>(places().status_icons);
}

FFXI::player_t* player()
{
    return reinterpret_cast<FFXI::player_t*>(game::addresses().player);
}

FFXI::target_t* target()
{
    return reinterpret_cast<FFXI::target_t*>(deref(game::addresses().target_pointer));
}

FFXI::targetwindow_t* target_window()
{
    return reinterpret_cast<FFXI::targetwindow_t*>(deref(places().target_window));
}

FFXI::inventory_t* inventory()
{
    const Places& at = places();
    const std::uintptr_t character = deref(at.character);
    return character != 0 && at.inventory != 0 ? reinterpret_cast<FFXI::inventory_t*>(character + at.inventory) : nullptr;
}

FFXI::autofollow_t* autofollow()
{
    return reinterpret_cast<FFXI::autofollow_t*>(places().autofollow);
}

FFXI::castbar_t* castbar()
{
    return reinterpret_cast<FFXI::castbar_t*>(deref(places().castbar));
}

// Copies `text` into a fixed-size name field, always terminated.
template <typename Char, size_t N>
void copy_name(Char (&field)[N], const char* text, size_t limit = N)
{
    const size_t size = limit < N ? limit : N;
    size_t i = 0;
    for (; text != nullptr && text[i] != '\0' && i + 1 < size; ++i)
        field[i] = static_cast<Char>(text[i]);
    field[i] = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// IEntity

#define ENTITY(Name, field)                                                                                     \
    static VANACORE_FIELD_TYPE(FFXI::entity_t, field) Get##Name(void*, uint32_t i)                               \
    {                                                                                                           \
        const FFXI::entity_t* e = entity(i);                                                                    \
        return e != nullptr ? e->field : VANACORE_FIELD_TYPE(FFXI::entity_t, field){};                          \
    }                                                                                                           \
    static void Set##Name(void*, uint32_t i, VANACORE_FIELD_TYPE(FFXI::entity_t, field) value)                   \
    {                                                                                                           \
        if (FFXI::entity_t* e = entity(i))                                                                      \
            e->field = value;                                                                                   \
    }

struct EntityImpl {
    static FFXI::entity_t* GetRawEntity(void*, uint32_t i) { return entity(i); }
    static uint32_t GetEntityMapSize(void*) { return game::addresses().entity_count; }
    static const char* GetName(void*, uint32_t i)
    {
        const FFXI::entity_t* e = entity(i);
        return e != nullptr ? reinterpret_cast<const char*>(e->Name) : nullptr;
    }
    static void SetName(void*, uint32_t i, const char* name)
    {
        if (FFXI::entity_t* e = entity(i))
            copy_name(e->Name, name, 24); // the last 4 bytes are flags
    }
    static uintptr_t GetAttachment(void*, uint32_t i, uint32_t slot)
    {
        const FFXI::entity_t* e = entity(i);
        return e != nullptr && slot < 12 ? e->Attachments[slot] : 0;
    }
    static void SetAttachment(void*, uint32_t i, uint32_t slot, uintptr_t value)
    {
        if (FFXI::entity_t* e = entity(i); e != nullptr && slot < 12)
            e->Attachments[slot] = value;
    }
    static uint32_t GetAnimation(void*, uint32_t i, uint32_t slot)
    {
        const FFXI::entity_t* e = entity(i);
        return e != nullptr && slot < 10 ? e->Animations[slot] : 0;
    }
    static void SetAnimation(void*, uint32_t i, uint32_t slot, uint32_t value)
    {
        if (FFXI::entity_t* e = entity(i); e != nullptr && slot < 10)
            e->Animations[slot] = value;
    }
    static const char* GetMonstrosityName(void*, uint32_t i)
    {
        const FFXI::entity_t* e = entity(i);
        return e != nullptr ? reinterpret_cast<const char*>(e->MonstrosityName) : nullptr;
    }
    static void SetMonstrosityName(void*, uint32_t i, const char* name)
    {
        if (FFXI::entity_t* e = entity(i))
            copy_name(e->MonstrosityName, name);
    }
    static const char* GetMonstrosityNameAbbr(void*, uint32_t i)
    {
        const FFXI::entity_t* e = entity(i);
        return e != nullptr ? reinterpret_cast<const char*>(e->MonstrosityNameAbbr) : nullptr;
    }
    static void SetMonstrosityNameAbbr(void*, uint32_t i, const char* name)
    {
        if (FFXI::entity_t* e = entity(i))
            copy_name(e->MonstrosityNameAbbr, name);
    }
    static uint8_t* GetCustomProperties(void*, uint32_t i)
    {
        FFXI::entity_t* e = entity(i);
        return e != nullptr ? e->CustomProperties : nullptr;
    }
    static void SetCustomProperties(void*, uint32_t i, uint8_t* data)
    {
        if (FFXI::entity_t* e = entity(i); e != nullptr && data != nullptr)
            std::memcpy(e->CustomProperties, data, sizeof(e->CustomProperties));
    }
    static uint8_t* GetBallistaInfo(void*, uint32_t i)
    {
        FFXI::entity_t* e = entity(i);
        return e != nullptr ? e->BallistaInfo : nullptr;
    }
    static void SetBallistaInfo(void*, uint32_t i, uint8_t* data)
    {
        if (FFXI::entity_t* e = entity(i); e != nullptr && data != nullptr)
            std::memcpy(e->BallistaInfo, data, sizeof(e->BallistaInfo));
    }

    ENTITY(ActionTimer1, ActionTimer1)
    ENTITY(ActionTimer2, ActionTimer2)
    ENTITY(ActorLockFlag, ActorLockFlag)
    ENTITY(ActorPointer, ActorPointer)
    ENTITY(AnimationPlay, AnimationPlay)
    ENTITY(AnimationSpeed, AnimationSpeed)
    ENTITY(AnimationStep, AnimationStep)
    ENTITY(AnimationTime, AnimationTime)
    ENTITY(AreaDisplayTargetIndex, AreaDisplayTargetIndex)
    ENTITY(BallistaFlags, BallistaFlags)
    ENTITY(CampaignNameFlag, CampaignNameFlag)
    ENTITY(ClaimStatus, ClaimStatus)
    ENTITY(CostumeId, CostumeId)
    ENTITY(CraftAnimationEffect, CraftAnimationEffect)
    ENTITY(CraftAnimationStep, CraftAnimationStep)
    ENTITY(CraftParam, CraftParam)
    ENTITY(CraftServerId, CraftServerId)
    ENTITY(CraftTimerUnknown, CraftTimerUnknown)
    ENTITY(Distance, Distance)
    ENTITY(DoorId, DoorId)
    ENTITY(EmoteId, EmoteId)
    ENTITY(EmoteIdString, EmoteIdString)
    ENTITY(EmoteTargetActorPointer, EmoteTargetActorPointer)
    ENTITY(EmoteTargetIndex, EmoteTargetIndex)
    ENTITY(EnvironmentAreaId, EnvironmentAreaId)
    ENTITY(EventPointer, EventPointer)
    ENTITY(FellowTargetIndex, FellowTargetIndex)
    ENTITY(FishingActionCountdown, FishingActionCountdown)
    ENTITY(FishingRodCastTime, FishingRodCastTime)
    ENTITY(FishingUnknown0000, FishingUnknown0000)
    ENTITY(FishingUnknown0001, FishingUnknown0001)
    ENTITY(FishingUnknown0002, FishingUnknown0002)
    ENTITY(HPPercent, HPPercent)
    ENTITY(Heading, Heading)
    ENTITY(InteractionTargetIndex, InteractionTargetIndex)
    ENTITY(IsDirty, IsDirty)
    ENTITY(LastActionActorPointer, LastActionActorPointer)
    ENTITY(LastActionId, LastActionId)
    ENTITY(LastPositionPitch, Movement.LastPosition.Pitch)
    ENTITY(LastPositionRoll, Movement.LastPosition.Roll)
    ENTITY(LastPositionW, Movement.LastPosition.W)
    ENTITY(LastPositionX, Movement.LastPosition.X)
    ENTITY(LastPositionY, Movement.LastPosition.Y)
    ENTITY(LastPositionYaw, Movement.LastPosition.Yaw)
    ENTITY(LastPositionZ, Movement.LastPosition.Z)
    ENTITY(LinkshellColor, LinkshellColor)
    ENTITY(LocalMoveCount, LocalMoveCount)
    ENTITY(LocalPositionPitch, Movement.LocalPosition.Pitch)
    ENTITY(LocalPositionRoll, Movement.LocalPosition.Roll)
    ENTITY(LocalPositionW, Movement.LocalPosition.W)
    ENTITY(LocalPositionX, Movement.LocalPosition.X)
    ENTITY(LocalPositionY, Movement.LocalPosition.Y)
    ENTITY(LocalPositionYaw, Movement.LocalPosition.Yaw)
    ENTITY(LocalPositionZ, Movement.LocalPosition.Z)
    ENTITY(LookAxisX, LookAxisX)
    ENTITY(LookAxisY, LookAxisY)
    ENTITY(LookBody, Look.Body)
    ENTITY(LookFeet, Look.Feet)
    ENTITY(LookHair, Look.Hair)
    ENTITY(LookHands, Look.Hands)
    ENTITY(LookHead, Look.Head)
    ENTITY(LookLegs, Look.Legs)
    ENTITY(LookMain, Look.Main)
    ENTITY(LookRanged, Look.Ranged)
    ENTITY(LookSub, Look.Sub)
    ENTITY(ModelHitboxSize, ModelHitboxSize)
    ENTITY(ModelSize, ModelSize)
    ENTITY(ModelStartTime, ModelStartTime)
    ENTITY(ModelTime, ModelTime)
    ENTITY(ModelUpdateFlags, ModelUpdateFlags)
    ENTITY(MogMotionId, MogMotionId)
    ENTITY(MonstrosityFlag, MonstrosityFlag)
    ENTITY(MonstrosityNameAbbrEnd, MonstrosityNameAbbrEnd)
    ENTITY(MonstrosityNameEnd, MonstrosityNameEnd)
    ENTITY(MonstrosityNameId, MonstrosityNameId)
    ENTITY(Mou4, Mou4)
    ENTITY(MountId, MountId)
    ENTITY(MouthCounter, MouthCounter)
    ENTITY(MouthWaitCounter, MouthWaitCounter)
    ENTITY(MoveDeltaPitch, Movement.Move.DeltaPitch)
    ENTITY(MoveDeltaRoll, Movement.Move.DeltaRoll)
    ENTITY(MoveDeltaW, Movement.Move.DeltaW)
    ENTITY(MoveDeltaX, Movement.Move.DeltaX)
    ENTITY(MoveDeltaY, Movement.Move.DeltaY)
    ENTITY(MoveDeltaYaw, Movement.Move.DeltaYaw)
    ENTITY(MoveDeltaZ, Movement.Move.DeltaZ)
    ENTITY(MoveW, Movement.Move.W)
    ENTITY(MoveX, Movement.Move.X)
    ENTITY(MoveY, Movement.Move.Y)
    ENTITY(MoveZ, Movement.Move.Z)
    ENTITY(MovementSpeed, MovementSpeed)
    ENTITY(MovementSpeed2, MovementSpeed2)
    ENTITY(NameColor, NameColor)
    ENTITY(Next, Next)
    ENTITY(NpcSpeechFrame, NpcSpeechFrame)
    ENTITY(NpcWalkMode, NpcWalkMode)
    ENTITY(NpcWalkPosition1, NpcWalkPosition1)
    ENTITY(NpcWalkPosition2, NpcWalkPosition2)
    ENTITY(PankrationEnabled, PankrationEnabled)
    ENTITY(PankrationFlagFlip, PankrationFlagFlip)
    ENTITY(PetTargetIndex, PetTargetIndex)
    ENTITY(PopEffect, PopEffect)
    ENTITY(Race, Race)
    ENTITY(RenderFlags0, Render.Flags0)
    ENTITY(RenderFlags1, Render.Flags1)
    ENTITY(RenderFlags2, Render.Flags2)
    ENTITY(RenderFlags3, Render.Flags3)
    ENTITY(RenderFlags4, Render.Flags4)
    ENTITY(RenderFlags5, Render.Flags5)
    ENTITY(RenderFlags6, Render.Flags6)
    ENTITY(RenderFlags7, Render.Flags7)
    ENTITY(RenderFlags8, Render.Flags8)
    ENTITY(ServerId, ServerId)
    ENTITY(SpawnFlags, SpawnFlags)
    ENTITY(Status, Status)
    ENTITY(StatusEvent, StatusEvent)
    ENTITY(StatusServer, StatusServer)
    ENTITY(TargetIndex, TargetIndex)
    ENTITY(TargetedIndex, TargetedIndex)
    ENTITY(TrustOwnerTargetIndex, TrustOwnerTargetIndex)
    ENTITY(TurnSpeed, TurnSpeed)
    ENTITY(TurnSpeedHead, TurnSpeedHead)
    ENTITY(Type, Type)
    ENTITY(UpdateMask, UpdateMask)
    ENTITY(UpdateRequestDelay, UpdateRequestDelay)
    ENTITY(WarpTargetIndex, WarpTargetIndex)
    ENTITY(ZoneId, ZoneId)
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IParty

#define MEMBER(Name, field)                                                                                     \
    static VANACORE_FIELD_TYPE(FFXI::partymember_t, field) GetMember##Name(void*, uint32_t i)                    \
    {                                                                                                           \
        const FFXI::partymember_t* m = member(i);                                                               \
        return m != nullptr ? m->field : VANACORE_FIELD_TYPE(FFXI::partymember_t, field){};                     \
    }
#define ALLIANCE(Name, field)                                                                                   \
    static VANACORE_FIELD_TYPE(FFXI::allianceinfo_t, field) GetAlliance##Name(void*)                             \
    {                                                                                                           \
        const FFXI::allianceinfo_t* a = alliance();                                                             \
        return a != nullptr ? a->field : VANACORE_FIELD_TYPE(FFXI::allianceinfo_t, field){};                    \
    }
#define ICONS(Name, field)                                                                                      \
    static VANACORE_FIELD_TYPE(FFXI::statusiconsentry_t, field) GetStatusIcons##Name(void*, uint32_t i)          \
    {                                                                                                           \
        const FFXI::partystatusicons_t* s = status_icons();                                                     \
        return s != nullptr && i < 5 ? s->Members[i].field : VANACORE_FIELD_TYPE(FFXI::statusiconsentry_t, field){}; \
    }

struct PartyImpl {
    static FFXI::party_t* GetRawStructure(void*) { return party(); }
    static FFXI::partystatusicons_t* GetRawStructureStatusIcons(void*) { return status_icons(); }
    static uint8_t GetMemberNumber(void*, uint32_t i)
    {
        const FFXI::partymember_t* m = member(i);
        return m != nullptr ? m->MemberNumber : 0;
    }
    static const char* GetMemberName(void*, uint32_t i)
    {
        const FFXI::partymember_t* m = member(i);
        return m != nullptr ? reinterpret_cast<const char*>(m->Name) : nullptr;
    }
    static uint16_t GetMemberTreasureLot(void*, uint32_t i, uint32_t slot)
    {
        const FFXI::partymember_t* m = member(i);
        return m != nullptr && slot < 10 ? m->TreasureLots[slot] : 0;
    }
    static const char* GetMemberMonstrosityName(void*, uint32_t i)
    {
        const FFXI::partymember_t* m = member(i);
        return m != nullptr ? reinterpret_cast<const char*>(m->MonstrosityName) : nullptr;
    }
    static uint8_t* GetStatusIcons(void*, uint32_t i)
    {
        FFXI::partystatusicons_t* s = status_icons();
        return s != nullptr && i < 5 ? s->Members[i].StatusIcons : nullptr;
    }
    ALLIANCE(LeaderServerId, AllianceLeaderServerId)
    ALLIANCE(PartyLeaderServerId1, PartyLeaderServerId1)
    ALLIANCE(PartyLeaderServerId2, PartyLeaderServerId2)
    ALLIANCE(PartyLeaderServerId3, PartyLeaderServerId3)
    ALLIANCE(PartyVisible1, PartyVisible1)
    ALLIANCE(PartyVisible2, PartyVisible2)
    ALLIANCE(PartyVisible3, PartyVisible3)
    static int8_t GetAlliancePartyMemberCount1(void*)
    {
        const FFXI::allianceinfo_t* a = alliance();
        return a != nullptr ? static_cast<int8_t>(a->PartyMemberCount1 - party_overflow()) : 0;
    }
    ALLIANCE(PartyMemberCount2, PartyMemberCount2)
    ALLIANCE(PartyMemberCount3, PartyMemberCount3)
    ALLIANCE(Invited, Invited)
    ALLIANCE(InviteParty, InviteParty)
    MEMBER(FlagMask, FlagMask)
    MEMBER(HP, HP)
    MEMBER(HPPercent, HPPercent)
    MEMBER(HPPercent2, HPPercent2)
    MEMBER(Index, Index)
    MEMBER(IsActive, IsActive)
    MEMBER(LastUpdatedTimestamp, LastUpdatedTimestamp)
    MEMBER(MP, MP)
    MEMBER(MPPercent, MPPercent)
    MEMBER(MPPercent2, MPPercent2)
    MEMBER(MainJob, MainJob)
    MEMBER(MainJobLevel, MainJobLevel)
    MEMBER(MasterBreaker, MasterBreaker)
    MEMBER(MasterLevel, MasterLevel)
    MEMBER(MonstrosityItemId, MonstrosityItemId)
    MEMBER(MonstrosityPrefixFlag1, MonstrosityPrefixFlag1)
    MEMBER(MonstrosityPrefixFlag2, MonstrosityPrefixFlag2)
    MEMBER(ServerId, ServerId)
    MEMBER(ServerId2, ServerId2)
    MEMBER(SubJob, SubJob)
    MEMBER(SubJobLevel, SubJobLevel)
    MEMBER(TP, TP)
    MEMBER(TargetIndex, TargetIndex)
    MEMBER(Zone, Zone)
    MEMBER(Zone2, Zone2)
    ICONS(BitMask, BitMask)
    ICONS(ServerId, ServerId)
    ICONS(TargetIndex, TargetIndex)
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IPlayer

#define PLAYER(Name, field)                                                                                     \
    static VANACORE_FIELD_TYPE(FFXI::player_t, field) Get##Name(void*)                                           \
    {                                                                                                           \
        const FFXI::player_t* p = player();                                                                     \
        return p != nullptr ? p->field : VANACORE_FIELD_TYPE(FFXI::player_t, field){};                          \
    }

template <typename T, typename Group>
T element(const Group& group, uint32_t index)
{
    constexpr uint32_t count = sizeof(Group) / sizeof(T);
    return index < count ? reinterpret_cast<const T*>(&group)[index] : T{};
}

struct PlayerImpl {
    static FFXI::player_t* GetRawStructure(void*) { return player(); }
    static int16_t GetStat(void*, uint32_t id)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? element<int16_t>(p->Stats, id) : 0;
    }
    static int16_t GetStatModifier(void*, uint32_t id)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? element<int16_t>(p->StatsModifiers, id) : 0;
    }
    static int16_t GetResist(void*, uint32_t id)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? element<int16_t>(p->Resists, id) : 0;
    }
    static uint32_t GetUnityFaction(void*)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? p->UnityInfo.Bits.Faction : 0;
    }
    static uint32_t GetUnityPoints(void*)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? p->UnityInfo.Bits.Points : 0;
    }
    static uint8_t GetMasteryUnknown0000(void*) // 4.16's name for what is padding
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? p->padding0063 : 0;
    }
    static FFXI::combatskill_t GetCombatSkill(void*, uint32_t id)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? element<FFXI::combatskill_t>(p->CombatSkills, id) : FFXI::combatskill_t{};
    }
    static FFXI::craftskill_t GetCraftSkill(void*, uint32_t id)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? element<FFXI::craftskill_t>(p->CraftSkills, id) : FFXI::craftskill_t{};
    }
    static uint16_t GetAbilityRecast(void*, uint32_t i)
    {
        const FFXI::player_t* p = player();
        return p != nullptr && i < 31 ? p->AbilityInfo[i].Recast : 0;
    }
    static uint8_t GetAbilityRecastCalc1(void*, uint32_t i)
    {
        const FFXI::player_t* p = player();
        return p != nullptr && i < 31 ? p->AbilityInfo[i].RecastCalc1 : 0;
    }
    static uint16_t GetAbilityRecastTimerId(void*, uint32_t i)
    {
        const FFXI::player_t* p = player();
        return p != nullptr && i < 31 ? p->AbilityInfo[i].TimerId : 0;
    }
    static int16_t GetAbilityRecastCalc2(void*, uint32_t i)
    {
        const FFXI::player_t* p = player();
        return p != nullptr && i < 31 ? p->AbilityInfo[i].RecastCalc2 : 0;
    }
    static uint32_t GetMountRecast(void*)
    {
        const FFXI::player_t* p = player();
        return p != nullptr ? p->MountRecast.Recast : 0;
    }
    static uint8_t* GetHomepointMasks(void*)
    {
        FFXI::player_t* p = player();
        return p != nullptr ? p->HomepointMasks : nullptr;
    }
    static uint16_t GetCapacityPoints(void*, uint32_t job)
    {
        const FFXI::player_t* p = player();
        return p != nullptr && job < 24 ? p->JobPoints.Jobs[job].CapacityPoints : 0;
    }
    static uint16_t GetJobPoints(void*, uint32_t job)
    {
        const FFXI::player_t* p = player();
        return p != nullptr && job < 24 ? p->JobPoints.Jobs[job].Points : 0;
    }
    static uint16_t GetJobPointsSpent(void*, uint32_t job)
    {
        const FFXI::player_t* p = player();
        return p != nullptr && job < 24 ? p->JobPoints.Jobs[job].PointsSpent : 0;
    }
    static int16_t* GetStatusIcons(void*)
    {
        FFXI::player_t* p = player();
        return p != nullptr ? p->StatusIcons : nullptr;
    }
    static uint32_t* GetStatusTimers(void*)
    {
        FFXI::player_t* p = player();
        return p != nullptr ? p->StatusTimers : nullptr;
    }
    static int16_t* GetBuffs(void*)
    {
        FFXI::player_t* p = player();
        return p != nullptr ? p->Buffs : nullptr;
    }
    static uint8_t GetPetMPPercent(void*)
    {
        const std::uintptr_t at = places().pet_mp;
        return at != 0 ? *reinterpret_cast<const uint8_t*>(at) : 0;
    }
    static uint32_t GetPetTP(void*)
    {
        const std::uintptr_t at = places().pet;
        return at != 0 ? *reinterpret_cast<const uint32_t*>(at + 0x0C) : 0;
    }
    static uint8_t GetJobLevel(void*, uint32_t job)
    {
        const std::uintptr_t function = places().job_level;
        return function != 0 ? reinterpret_cast<uint8_t(__cdecl*)(uint32_t)>(function)(job) : 0;
    }
    static uint8_t GetJobMasterLevel(void*, uint32_t job)
    {
        const std::uintptr_t function = places().job_master_level;
        return function != 0 ? reinterpret_cast<uint8_t(__cdecl*)(uint32_t)>(function)(job) : 0;
    }
    // Ashita's values: 0 at the title screen, 1 between zones (from just after the server's zone-out until the player is
    // back: FancyChat and others read 0 as a logout), 2 once the character is in the world.
    static uint8_t GetLoginStatus(void*)
    {
        const FFXI::partymember_t* self = member(0);
        const FFXI::entity_t* e = self != nullptr ? entity(self->TargetIndex) : nullptr;
        const bool in_world = e != nullptr && self->TargetIndex != 0 && e->ServerId == self->ServerId;
        const bool zoning = packets::zoning(in_world); // also what clears it once the player is back
        return in_world ? 2 : zoning ? 1 : 0;
    }
    static bool HasAbilityData(void*) { return abilities() != nullptr; }
    static bool HasAbility(void*, uint32_t id)
    {
        const uint8_t* bits = abilities();
        return bits != nullptr && id < kAbilityBits && ((bits[id >> 3] >> (id & 7)) & 1) != 0;
    }
    // weapon skills: the abilities' first 512 bits, by their ids; pet commands: the bits from 0x400
    static bool HasWeaponSkill(void* c, uint32_t id) { return id < 0x200 && HasAbility(c, id); }
    static bool HasPetCommand(void* c, uint32_t id) { return id >= 0x400 && id < 0x600 && HasAbility(c, id); }
    static bool HasTrait(void* c, uint32_t id) { return id >= 0x600 && id < 0x700 && HasAbility(c, id); } // traits: from 0x600
    static bool HasSpellData(void*) { return spells() != nullptr; }
    static bool HasSpell(void*, uint32_t id)
    {
        const uint8_t* bits = spells();
        return bits != nullptr && id < 0x400 && ((bits[id >> 3] >> (id & 7)) & 1) != 0;
    }
    static bool HasKeyItem(void*, uint32_t id)
    {
        const std::uintptr_t table = places().key_items;
        if (table == 0 || id >= 0x80 * 32)
            return false;
        return ((reinterpret_cast<const uint32_t*>(table)[id >> 5] >> (id & 31)) & 1) != 0;
    }

    static constexpr uint32_t kAbilityBits = 0xE0 * 8;
    static const uint8_t* abilities()
    {
        const std::uintptr_t function = places().abilities;
        return function != 0 ? reinterpret_cast<const uint8_t*(__cdecl*)()>(function)() : nullptr;
    }
    static const uint8_t* spells()
    {
        const std::uintptr_t function = places().spells;
        return function != 0 ? reinterpret_cast<const uint8_t*(__cdecl*)()>(function)() : nullptr;
    }

    PLAYER(AssimilationPoints, AssimilationPoints)
    PLAYER(Attack, Attack)
    PLAYER(DataLoadedFlags, DataLoadedFlags)
    PLAYER(Defense, Defense)
    PLAYER(ExpCurrent, ExpCurrent)
    PLAYER(ExpNeeded, ExpNeeded)
    PLAYER(HPMax, HPMax)
    PLAYER(HighestItemLevel, HighestItemLevel)
    PLAYER(Homepoint, Homepoint)
    PLAYER(IsExperiencePointsLocked, IsExperiencePointsLocked)
    PLAYER(IsLimitBreaker, IsLimitBreaker)
    PLAYER(IsLimitModeEnabled, IsLimitModeEnabled)
    PLAYER(IsZoning, IsZoning)
    PLAYER(ItemLevel, ItemLevel)
    PLAYER(LimitPoints, LimitPoints)
    PLAYER(MPMax, MPMax)
    PLAYER(MainHandItemLevel, MainHandItemLevel)
    PLAYER(MainJob, MainJob)
    PLAYER(MainJobLevel, MainJobLevel)
    PLAYER(MasteryExp, MasteryExp)
    PLAYER(MasteryExpNeeded, MasteryExpNeeded)
    PLAYER(MasteryFlags, MasteryFlags)
    PLAYER(MasteryJob, MasteryJob)
    PLAYER(MasteryJobLevel, MasteryJobLevel)
    PLAYER(MeritPoints, MeritPoints)
    PLAYER(MeritPointsMax, MeritPointsMax)
    PLAYER(MountRecastTimerId, MountRecast.TimerId)
    PLAYER(Nation, Nation)
    PLAYER(RangedItemLevel, RangedItemLevel)
    PLAYER(Rank, Rank)
    PLAYER(RankPoints, RankPoints)
    PLAYER(Residence, Residence)
    PLAYER(StatusOffset1W, StatusOffset1.W)
    PLAYER(StatusOffset1X, StatusOffset1.X)
    PLAYER(StatusOffset1Y, StatusOffset1.Y)
    PLAYER(StatusOffset1Z, StatusOffset1.Z)
    PLAYER(StatusOffset2W, StatusOffset2.W)
    PLAYER(StatusOffset2X, StatusOffset2.X)
    PLAYER(StatusOffset2Y, StatusOffset2.Y)
    PLAYER(StatusOffset2Z, StatusOffset2.Z)
    PLAYER(StatusOffset3W, StatusOffset3.W)
    PLAYER(StatusOffset3X, StatusOffset3.X)
    PLAYER(StatusOffset3Y, StatusOffset3.Y)
    PLAYER(StatusOffset3Z, StatusOffset3.Z)
    PLAYER(StatusOffset4W, StatusOffset4.W)
    PLAYER(StatusOffset4X, StatusOffset4.X)
    PLAYER(StatusOffset4Y, StatusOffset4.Y)
    PLAYER(StatusOffset4Z, StatusOffset4.Z)
    PLAYER(SuLevel, SuLevel)
    PLAYER(SubJob, SubJob)
    PLAYER(SubJobLevel, SubJobLevel)
    PLAYER(SubMapNum, SubMapNum)
    PLAYER(Title, Title)
    PLAYER(UnityChatColorFlag, UnityChatColorFlag)
    PLAYER(UnityPartialPersonalEvalutionPoints, UnityPartialPersonalEvalutionPoints)
    PLAYER(UnityPersonalEvaluationPoints, UnityPersonalEvaluationPoints)
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ITarget (the target window, GetRawStructureWindow and its Window* getters, isn't found yet)

#define TARGET_ENTRY(Name, field)                                                                               \
    static VANACORE_FIELD_TYPE(FFXI::targetentry_t, field) Get##Name(void*, uint32_t i)                          \
    {                                                                                                           \
        const FFXI::target_t* t = target();                                                                     \
        return t != nullptr && i < 2 ? t->Targets[i].field : VANACORE_FIELD_TYPE(FFXI::targetentry_t, field){}; \
    }
#define TARGET_WINDOW(Name, field)                                                                              \
    static VANACORE_FIELD_TYPE(FFXI::targetwindow_t, field) GetWindow##Name(void*)                               \
    {                                                                                                           \
        const FFXI::targetwindow_t* w = target_window();                                                         \
        return w != nullptr ? w->field : VANACORE_FIELD_TYPE(FFXI::targetwindow_t, field){};                     \
    }
#define TARGET(Name, field)                                                                                     \
    static VANACORE_FIELD_TYPE(FFXI::target_t, field) Get##Name(void*)                                           \
    {                                                                                                           \
        const FFXI::target_t* t = target();                                                                     \
        return t != nullptr ? t->field : VANACORE_FIELD_TYPE(FFXI::target_t, field){};                          \
    }

struct TargetImpl {
    static FFXI::target_t* GetRawStructure(void*) { return target(); }
    // (Ashita's force flag has no counterpart in the game's call)
    static void SetTarget(void*, uint32_t index, bool) { set_target(index); }
    static uint32_t GetTargetIndex(void*, uint32_t i)
    {
        const FFXI::target_t* t = target();
        return t != nullptr && i < 2 ? t->Targets[i].Index : 0;
    }
    static float GetTargetPosF(void*, uint32_t i)
    {
        const FFXI::target_t* t = target();
        return t != nullptr && i < 4 ? t->TargetPosF[i] : 0.0f;
    }
    static const char* GetLastTargetName(void*)
    {
        const FFXI::target_t* t = target();
        return t != nullptr ? reinterpret_cast<const char*>(t->LastTargetName) : nullptr;
    }
    TARGET_ENTRY(ActorPointer, ActorPointer)
    TARGET_ENTRY(ArrowPositionW, ArrowPosition.W)
    TARGET_ENTRY(ArrowPositionX, ArrowPosition.X)
    TARGET_ENTRY(ArrowPositionY, ArrowPosition.Y)
    TARGET_ENTRY(ArrowPositionZ, ArrowPosition.Z)
    TARGET_ENTRY(Checksum, Checksum)
    TARGET_ENTRY(EntityPointer, EntityPointer)
    TARGET_ENTRY(IsActive, IsActive)
    TARGET_ENTRY(IsArrowActive, IsArrowActive)
    TARGET_ENTRY(IsModelActor, IsModelActor)
    TARGET_ENTRY(ServerId, ServerId)
    TARGET(ActionAoeCallback, ActionAoeCallback)
    TARGET(ActionAoeRange, ActionAoeRange)
    TARGET(ActionCallback, ActionCallback)
    TARGET(ActionId, ActionId)
    TARGET(ActionTargetActive, ActionTargetActive)
    TARGET(ActionTargetMaxYalms, ActionTargetMaxYalms)
    TARGET(ActionTargetServerId, ActionTargetServerId)
    TARGET(ActionType, ActionType)
    TARGET(CancelCallback, CancelCallback)
    TARGET(DeactivateTarget, DeactivateTarget)
    TARGET(DefaultMode, DefaultMode)
    TARGET(FocusTargetIndex, FocusTargetIndex)
    TARGET(FocusTargetServerId, FocusTargetServerId)
    TARGET(IsActionAoe, IsActionAoe)
    TARGET(IsAutoNotice, IsAutoNotice)
    TARGET(IsMenuOpen, IsMenuOpen)
    TARGET(IsMouseRequestCancel, IsMouseRequestCancel)
    TARGET(IsMouseRequestStack, IsMouseRequestStack)
    TARGET(IsPlayerMoving, IsPlayerMoving)
    TARGET(IsSubTargetActive, IsSubTargetActive)
    TARGET(IsWalk, IsWalk)
    TARGET(LastTargetChecksum, LastTargetChecksum)
    TARGET(LastTargetIndex, LastTargetIndex)
    TARGET(LastTargetServerId, LastTargetServerId)
    TARGET(LockedOnFlags, LockedOnFlags)
    TARGET(MenuTargetLock, MenuTargetLock)
    TARGET(ModeChangeLock, ModeChangeLock)
    TARGET(MyroomCallback, MyroomCallback)
    TARGET(OldNotice, OldNotice)
    TARGET(SubTargetFlags, SubTargetFlags)

    // The target window
    static FFXI::targetwindow_t* GetRawStructureWindow(void*) { return target_window(); }
    static const char* GetWindowName(void*)
    {
        const FFXI::targetwindow_t* w = target_window();
        return w != nullptr ? reinterpret_cast<const char*>(w->Name) : nullptr;
    }
    static uint32_t GetWindowAnkShape(void*, uint32_t i)
    {
        const FFXI::targetwindow_t* w = target_window();
        return w != nullptr && i < 16 ? w->m_pAnkShape[i] : 0;
    }
    TARGET_WINDOW(EntityPointer, EntityPointer)
    TARGET_WINDOW(FrameChildrenOffsetX, FrameChildrenOffsetX)
    TARGET_WINDOW(FrameChildrenOffsetY, FrameChildrenOffsetY)
    TARGET_WINDOW(IconPositionX, IconPositionX)
    TARGET_WINDOW(IconPositionY, IconPositionY)
    TARGET_WINDOW(FrameOffsetX, FrameOffsetX)
    TARGET_WINDOW(FrameOffsetY, FrameOffsetY)
    TARGET_WINDOW(LockShape, LockShape)
    TARGET_WINDOW(ServerId, ServerId)
    TARGET_WINDOW(HPPercent, HPPercent)
    TARGET_WINDOW(DeathFlag, DeathFlag)
    TARGET_WINDOW(ReraiseFlag, ReraiseFlag)
    TARGET_WINDOW(DeathNameColor, DeathNameColor)
    TARGET_WINDOW(IsWindowLoaded, IsWindowLoaded)
    TARGET_WINDOW(HelpString, HelpString)
    TARGET_WINDOW(HelpTitle, HelpTitle)
    TARGET_WINDOW(Sub, m_Sub)
    TARGET_WINDOW(AnkNum, m_AnkNum)
    TARGET_WINDOW(AnkX, m_AnkX)
    TARGET_WINDOW(AnkY, m_AnkY)
    TARGET_WINDOW(SubAnkX, m_SubAnkX)
    TARGET_WINDOW(SubAnkY, m_SubAnkY)
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IRecast: the abilities' timer slots and the spells' recasts, as the game counts them down (1/60 s)

constexpr uint32_t kAbilityRecasts = 31, kSpellRecasts = 0x401;

// The spells' recasts and how many there are, as the game's code addresses them now (0 and 0 until it's there).
std::pair<std::uintptr_t, uint32_t> spell_recast_array()
{
    const std::uintptr_t code = places().spell_recasts;
    if (code == 0)
        return {0, 0};
    const std::uintptr_t begin = code_dword(code + 2);
    uint32_t count = kSpellRecasts;
    // add esi,2 / cmp esi,end: the end is one past the last id's, so count = (end - begin) / 2. Only when the code
    // is as expected, else the game's own count.
    constexpr uint8_t kAddCmp[] = {0x83, 0xC6, 0x02, 0x81, 0xFE}; // add esi,2 / cmp esi,imm32
    if (std::memcmp(reinterpret_cast<const void*>(code + kSpellRecastEnd - 5), kAddCmp, sizeof(kAddCmp)) == 0) {
        const std::uintptr_t end = code_dword(code + kSpellRecastEnd);
        if (end > begin && end - begin <= 0x20000)
            count = static_cast<uint32_t>((end - begin) / 2);
    }
    return {begin, count};
}

const FFXI::abilityrecast_t* ability_recast(uint32_t i)
{
    const std::uintptr_t at = places().ability_recasts;
    return at != 0 && i < kAbilityRecasts ? reinterpret_cast<const FFXI::abilityrecast_t*>(at) + i : nullptr;
}

struct RecastImpl {
    static uint32_t GetAbilityRecast(void*, uint32_t i)
    {
        const FFXI::abilityrecast_t* r = ability_recast(i);
        return r != nullptr ? r->Recast : 0;
    }
    static uint8_t GetAbilityCalc1(void*, uint32_t i)
    {
        const FFXI::abilityrecast_t* r = ability_recast(i);
        return r != nullptr ? r->RecastCalc1 : 0;
    }
    static uint32_t GetAbilityTimerId(void*, uint32_t i)
    {
        const FFXI::abilityrecast_t* r = ability_recast(i);
        return r != nullptr ? r->TimerId : 0;
    }
    static int16_t GetAbilityCalc2(void*, uint32_t i)
    {
        const FFXI::abilityrecast_t* r = ability_recast(i);
        return r != nullptr ? r->RecastCalc2 : 0;
    }
    static uint32_t GetAbilityTimer(void*, uint32_t i)
    {
        const std::uintptr_t at = places().ability_recasts;
        return at != 0 && i < kAbilityRecasts ? reinterpret_cast<const uint32_t*>(at + 0xF8)[i] : 0;
    }
    static uint32_t GetSpellTimer(void*, uint32_t i)
    {
        const auto [at, count] = spell_recast_array();
        return at != 0 && i < count ? reinterpret_cast<const uint16_t*>(at)[i] : 0;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IInventory

constexpr uint32_t kContainers = static_cast<uint32_t>(FFXI::Enums::Container::Max);
constexpr uint32_t kEquipmentSlots = static_cast<uint32_t>(FFXI::Enums::EquipmentSlot::Max);

#define INVENTORY(Name, field)                                                                                  \
    static VANACORE_FIELD_TYPE(FFXI::inventory_t, field) Get##Name(void*)                                        \
    {                                                                                                           \
        const FFXI::inventory_t* v = inventory();                                                               \
        return v != nullptr ? v->field : VANACORE_FIELD_TYPE(FFXI::inventory_t, field){};                       \
    }
#define INVENTORY_SET(Name, field)                                                                              \
    INVENTORY(Name, field)                                                                                      \
    static void Set##Name(void*, VANACORE_FIELD_TYPE(FFXI::inventory_t, field) value)                            \
    {                                                                                                           \
        if (FFXI::inventory_t* v = inventory())                                                                 \
            v->field = value;                                                                                   \
    }

struct InventoryImpl {
    static FFXI::inventory_t* GetRawStructure(void*) { return inventory(); }
    // Never null: plugins and addons dereference it without a check (Lootwhore's `->Id`, find's `.Id`) and
    // loop up to GetContainerCountMax inclusive, so a slot past the end, or before login, is an empty item
    // (as it would read in the game's own zeroed memory).
    static FFXI::item_t* GetContainerItem(void*, uint32_t container, uint32_t index)
    {
        static FFXI::item_t empty{};
        FFXI::inventory_t* v = inventory();
        if (v != nullptr && container < kContainers && index < 81)
            return &v->Containers[container].Items[index];
        empty = FFXI::item_t{};
        return &empty;
    }
    // The slots a container can hold. The game keeps one more (every container's slot 0 is reserved, gil in the
    // inventory: the server sends 81 for an 80-slot bag), but addons count 1..max and call it full at
    // GetContainerCount() >= max (XIUI, HXUI, chatmon), so slot 0 isn't in it.
    static uint32_t GetContainerCountMax(void*, uint32_t container)
    {
        const uint32_t capacity = container_capacity(container);
        return capacity > 0 ? capacity - 1 : 0;
    }
    // The slots holding an item (the inventory's slot 0 isn't one)
    static uint32_t GetContainerCount(void*, uint32_t container)
    {
        const FFXI::inventory_t* v = inventory();
        if (v == nullptr || container >= kContainers)
            return 0;
        const uint32_t capacity = std::min<uint32_t>(v->ContainerMaxCapacity[container], 81);
        uint32_t count = 0;
        for (uint32_t index = 1; index < capacity; ++index)
            if (v->Containers[container].Items[index].Id != 0)
                ++count;
        return count;
    }
    // Never null, for the same reason as GetContainerItem.
    static FFXI::treasureitem_t* GetTreasurePoolItem(void*, uint32_t index)
    {
        static FFXI::treasureitem_t empty{};
        FFXI::inventory_t* v = inventory();
        if (v != nullptr && index < 10)
            return &v->TreasurePool[index];
        empty = FFXI::treasureitem_t{};
        return &empty;
    }
    static FFXI::equipmententry_t* GetEquippedItem(void*, uint32_t index)
    {
        FFXI::inventory_t* v = inventory();
        return v != nullptr && index < kEquipmentSlots ? &v->Equipment[index] : nullptr;
    }
    static FFXI::itemcheck_t* GetCheckEquippedItem(void*, uint32_t index)
    {
        FFXI::inventory_t* v = inventory();
        return v != nullptr && index < kEquipmentSlots ? &v->CheckEquipment[index] : nullptr;
    }
    static uint8_t* GetCheckLinkshellName(void*)
    {
        FFXI::inventory_t* v = inventory();
        return v != nullptr ? v->CheckLinkshellName : nullptr;
    }
    static const char* GetSearchComment(void*)
    {
        const FFXI::inventory_t* v = inventory();
        return v != nullptr ? v->SearchComment : nullptr;
    }
    INVENTORY(CheckBallistaChevronCount, CheckBallistaChevronCount)
    INVENTORY(CheckBallistaChevronFlags, CheckBallistaChevronFlags)
    INVENTORY(CheckBallistaFlags, CheckBallistaFlags)
    INVENTORY(CheckFlags, CheckFlags)
    INVENTORY(CheckLinkshellColor, CheckLinkshellColor)
    INVENTORY(CheckLinkshellIconSetId, CheckLinkshellIconSetId)
    INVENTORY(CheckLinkshellIconSetIndex, CheckLinkshellIconSetIndex)
    INVENTORY(CheckMainJob, CheckMainJob)
    INVENTORY(CheckMainJob2, CheckMainJob2)
    INVENTORY(CheckMainJobLevel, CheckMainJobLevel)
    INVENTORY(CheckMasteryFlags, CheckMasteryFlags)
    INVENTORY(CheckMasteryLevel, CheckMasteryLevel)
    INVENTORY(CheckServerId, CheckServerId)
    INVENTORY(CheckSubJob, CheckSubJob)
    INVENTORY(CheckSubJobLevel, CheckSubJobLevel)
    INVENTORY(CheckTargetIndex, CheckTargetIndex)
    INVENTORY(ContainerUpdateCounter, ContainerUpdateCounter)
    INVENTORY(ContainerUpdateFlags, ContainerUpdateFlags)
    INVENTORY(DisplayItemPointer, DisplayItemPointer)
    INVENTORY(DisplayItemSlot, DisplayItemSlot)
    INVENTORY(TreasurePoolItemCount, TreasurePoolItemCount)
    INVENTORY(TreasurePoolStatus, TreasurePoolStatus)
    INVENTORY_SET(CraftCallback, CraftCallback)
    INVENTORY_SET(CraftStatus, CraftStatus)
    INVENTORY_SET(CraftTimestampAttempt, CraftTimestampAttempt)
    INVENTORY_SET(CraftTimestampResponse, CraftTimestampResponse)
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IAutoFollow, ICastBar

#define AUTOFOLLOW(Name, field)                                                                                 \
    static VANACORE_FIELD_TYPE(FFXI::autofollow_t, field) Get##Name(void*)                                       \
    {                                                                                                           \
        const FFXI::autofollow_t* a = autofollow();                                                             \
        return a != nullptr ? a->field : VANACORE_FIELD_TYPE(FFXI::autofollow_t, field){};                      \
    }                                                                                                           \
    static void Set##Name(void*, VANACORE_FIELD_TYPE(FFXI::autofollow_t, field) value)                           \
    {                                                                                                           \
        if (FFXI::autofollow_t* a = autofollow())                                                               \
            a->field = value;                                                                                   \
    }

struct AutoFollowImpl {
    static FFXI::autofollow_t* GetRawStructure(void*) { return autofollow(); }
    AUTOFOLLOW(FollowDeltaW, FollowDeltaW)
    AUTOFOLLOW(FollowDeltaX, FollowDeltaX)
    AUTOFOLLOW(FollowDeltaY, FollowDeltaY)
    AUTOFOLLOW(FollowDeltaZ, FollowDeltaZ)
    AUTOFOLLOW(FollowTargetIndex, FollowTargetIndex)
    AUTOFOLLOW(FollowTargetServerId, FollowTargetServerId)
    AUTOFOLLOW(IsAutoRunning, IsAutoRunning)
    AUTOFOLLOW(IsCameraLocked, IsCameraLocked)
    AUTOFOLLOW(IsCameraLockedOn, IsCameraLockedOn)
    AUTOFOLLOW(IsFirstPersonCamera, IsFirstPersonCamera)
    AUTOFOLLOW(TargetIndex, TargetIndex)
    AUTOFOLLOW(TargetServerId, TargetServerId)
};

#define CASTBAR(Name, field)                                                                                    \
    static VANACORE_FIELD_TYPE(FFXI::castbar_t, field) Get##Name(void*)                                          \
    {                                                                                                           \
        const FFXI::castbar_t* c = castbar();                                                                   \
        return c != nullptr ? c->field : VANACORE_FIELD_TYPE(FFXI::castbar_t, field){};                         \
    }                                                                                                           \
    static void Set##Name(void*, VANACORE_FIELD_TYPE(FFXI::castbar_t, field) value)                              \
    {                                                                                                           \
        if (FFXI::castbar_t* c = castbar())                                                                     \
            c->field = value;                                                                                   \
    }

struct CastBarImpl {
    static FFXI::castbar_t* GetRawStructure(void*) { return castbar(); }
    CASTBAR(CastType, CastType)
    CASTBAR(Count, Count)
    CASTBAR(Max, Max)
    CASTBAR(Percent, Percent)
};

struct MemoryImpl {
    static void* GetAutoFollow(void* c) { return links(c).autofollow; }
    static void* GetCastBar(void* c) { return links(c).castbar; }
    static void* GetEntity(void* c) { return links(c).entity; }
    static void* GetInventory(void* c) { return links(c).inventory; }
    static void* GetParty(void* c) { return links(c).party; }
    static void* GetPlayer(void* c) { return links(c).player; }
    static void* GetRecast(void* c) { return links(c).recast; }
    static void* GetTarget(void* c) { return links(c).target; }
};

} // namespace

namespace {

// Whether `actor` is `e`'s (its +0x70 the entity). An entity's actor pointer outlives the actor it names for a moment (a
// mob despawning, the zone being let go): one that can't be read is no one's.
bool actor_of(std::uintptr_t actor, const FFXI::entity_t* e)
{
    __try {
        return *reinterpret_cast<const std::uintptr_t*>(actor + 0x70) == reinterpret_cast<std::uintptr_t>(e);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

// The game's own call sites target an entity with the target manager's SetTarget(actor, 1, 0), thiscall, ret 0xC (the
// pointer manager's target.settarget: mov ecx,[manager]; add esp,8; test ecx,ecx; je; test eax,eax; je; push 0; push 1;
// push eax; call). It takes the entity's actor, not the entity - it reads the entity back from the actor's +0x70 - and given
// the entity it faulted (from XI on Anything, rubymatrix/xi-on-anything 79ceb05).
bool set_target(uint32_t index)
{
    using SetTargetFn = bool(__thiscall*)(void* manager, void* actor, int a, int b);
    static SetTargetFn fn = nullptr;
    static void** manager = nullptr;
    static bool searched = false;
    if (!searched) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return false;
        searched = true;
        const std::uintptr_t site = scan::find(game, "8B 0D ?? ?? ?? ?? 83 C4 08 85 C9 74 ?? 85 C0 74 ?? 6A 00 6A 01 50 E8");
        if (site != 0) {
            manager = *reinterpret_cast<void** const*>(site + 2);
            fn = reinterpret_cast<SetTargetFn>(site + 0x1B + *reinterpret_cast<const std::int32_t*>(site + 0x17));
        }
    }
    if (fn == nullptr || manager == nullptr || *manager == nullptr)
        return false;
    FFXI::entity_t* e = entity(index);
    const std::uintptr_t actor = e != nullptr ? e->ActorPointer : 0;
    // no actor (out of render range), or one that isn't this entity's (or is already gone)
    if (actor == 0 || !actor_of(actor, e))
        return false;
    fn(*manager, reinterpret_cast<void*>(actor), 1, 0);
    return true;
}

const uint8_t* ability_bits()
{
    return PlayerImpl::abilities();
}

const uint8_t* spell_bits()
{
    return PlayerImpl::spells();
}

const uint8_t* key_item_bits()
{
    return reinterpret_cast<const uint8_t*>(places().key_items);
}

const uint8_t* ability_recasts()
{
    return reinterpret_cast<const uint8_t*>(places().ability_recasts);
}

const uint16_t* spell_recasts(uint32_t* count)
{
    const auto [at, n] = spell_recast_array();
    if (count != nullptr)
        *count = n;
    return reinterpret_cast<const uint16_t*>(at);
}

uint32_t container_capacity(uint32_t container)
{
    const FFXI::inventory_t* v = inventory();
    return v != nullptr && container < kContainers ? v->ContainerMaxCapacity[container] : 0;
}

bool container_item(uint32_t container, uint32_t index, ItemSlot& out)
{
    const FFXI::inventory_t* v = inventory();
    if (v == nullptr || container >= kContainers || index >= 81)
        return false;
    const FFXI::item_t& item = v->Containers[container].Items[index];
    out.id = item.Id;
    out.index = item.Index;
    out.count = item.Count;
    out.flags = item.Flags;
    out.price = item.Price;
    static_assert(sizeof(out.extra) == sizeof(item.Extra));
    std::memcpy(out.extra, item.Extra, sizeof(out.extra));
    return true;
}

bool equipped(uint32_t slot, uint32_t& container, uint32_t& index)
{
    const FFXI::inventory_t* v = inventory();
    if (v == nullptr || slot >= kEquipmentSlots)
        return false;
    index = v->Equipment[slot].Index & 0xFF;
    container = (v->Equipment[slot].Index >> 8) & 0xFF;
    return true;
}

void build_memory(Links& a, Links& b)
{
    namespace v416 = sdk::v416;
    namespace v430 = sdk::v430;
    struct Nothing {};
    a.memory = make<v416::MemoryManagerObject, MemoryImpl>(&a);
    b.memory = make<v430::MemoryManagerObject, MemoryImpl>(&b);
    a.autofollow = make<v416::AutoFollowObject, AutoFollowImpl>(&a);
    b.autofollow = make<v430::AutoFollowObject, AutoFollowImpl>(&b);
    a.castbar = make<v416::CastBarObject, CastBarImpl>(&a);
    b.castbar = make<v430::CastBarObject, CastBarImpl>(&b);
    a.entity = make<v416::EntityObject, EntityImpl>(&a);
    b.entity = make<v430::EntityObject, EntityImpl>(&b);
    a.inventory = make<v416::InventoryObject, InventoryImpl>(&a);
    b.inventory = make<v430::InventoryObject, InventoryImpl>(&b);
    a.party = make<v416::PartyObject, PartyImpl>(&a);
    b.party = make<v430::PartyObject, PartyImpl>(&b);
    a.player = make<v416::PlayerObject, PlayerImpl>(&a);
    b.player = make<v430::PlayerObject, PlayerImpl>(&b);
    a.recast = make<v416::RecastObject, RecastImpl>(&a);
    b.recast = make<v430::RecastObject, RecastImpl>(&b);
    a.target = make<v416::TargetObject, TargetImpl>(&a);
    b.target = make<v430::TargetObject, TargetImpl>(&b);
}

} // namespace vanacore::ashita
