#include "noblink.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::noblink {
namespace {

// Each frame the game walks its entities and, for one whose look changed, deletes its actor and sets it up again (PS2
// client: PutDisplayList; XiAtelBuff::ObjectDelete then ObjectNew): that's the blink. A gear change only sets the slot's
// bit in the entity's ModelUpdateFlags (entity+0xF4: 2 head, 4 body, 8 hands, 0x10 legs, 0x20 feet, 0x40 main, 0x80 sub,
// 0x100 ranged; 1 is the hair or face); this step (FFXiMain.dll VA 0x10095BE0, __cdecl: the entity) is the only place
// that reads them:
//
//   if (equip_changes_allowed(entity) && (entity->flags_f4 != 0 || entity->flags_120 & 1)) {
//       entity->flags_130 &= ~0x20000; delete_actor(entity); entity->flags_f4 = 0; entity->flags_120 &= ~1;
//   }
//
// (flags_120 bit 0 is a race change.) For the local player, with only slot bits set and an actor there, VanaCore leaves
// the step out: the flags stay set, so the model is rebuilt the moment the option is off. A main or sub bit among them
// (0x40, 0x80) lets the step run, and so blink, unless [client] blink.weapons is on.
//
// push esi; mov esi,[esp+8]; mov ecx,esi; call <allowed>; test al,al; jz; cmp word ptr [esi+0F4h],0; jnz; test byte
// ptr [esi+120h],1; jz
constexpr char kStep[] = "56 8B 74 24 08 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 66 83 BE F4 00 00 00 00 75 ?? F6 86 20 01 00 00 01 74 ??";
constexpr std::size_t kPrologue = 5;       // push esi; mov esi,[esp+8]
constexpr std::ptrdiff_t kAllowedCall = 7; // __thiscall (entity): is a look change to be applied (not locked, not in an event...)
// The allowed test begins: push esi; mov esi,ecx; mov al,[esi+0EEh]
constexpr std::uint8_t kAllowedStart[] = {0x56, 0x8B, 0xF1, 0x8A, 0x86, 0xEE, 0x00, 0x00, 0x00};

// The local player's entity (FUN 0x101CAC50, __cdecl, 0 if there isn't one): movzx ax,[player index]; test ax,ax; jz;
// movsx eax,ax; mov eax,[eax*4+entities]; test eax,eax; jnz; xor eax,eax; ret
constexpr char kPlayer[] = "66 A1 ?? ?? ?? ?? 66 85 C0 74 0E 0F BF C0 8B 04 85 ?? ?? ?? ?? 85 C0 75 02 33 C0 C3";

// Entity fields
constexpr std::uintptr_t kActor = 0xA0, kUpdateFlags = 0xF4, kFlags = 0x120;
constexpr std::uint16_t kHairBit = 0x0001;
constexpr std::uint16_t kWeaponBits = 0x0040 | 0x0080; // main hand, sub (off hand)
constexpr std::uint32_t kRaceBit = 0x1;

using Step = void(__cdecl*)(std::uint8_t* entity);
using Player = std::uint8_t*(__cdecl*)();

bool g_on = false;       // [client] blink.disable
bool g_weapons = false;  // [client] blink.weapons: a main or sub change is held too (off: it blinks, as the game does)
bool g_searched = false; // FFXiMain.dll was looked through (g_step != 0: hooked)
Step g_step = nullptr;   // the game's, through the trampoline
Player g_player = nullptr;
unsigned g_skipped = 0;

template <typename T>
T read(const std::uint8_t* at)
{
    T value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

// True for a gear change of the local character's that the game would answer by deleting and setting up its actor again
bool hold(std::uint8_t* entity)
{
    if (!g_on || entity == nullptr || g_player == nullptr || entity != g_player())
        return false;
    if (read<std::uint32_t>(entity + kActor) == 0)
        return false; // no actor yet: nothing to blink, the game builds it
    const std::uint16_t update = read<std::uint16_t>(entity + kUpdateFlags);
    if (update == 0 || (update & kHairBit) != 0)
        return false; // nothing changed, or a hair or face change
    if (!g_weapons && (update & kWeaponBits) != 0)
        return false; // a weapon change, and weapons are left to blink: the whole rebuild goes through, held gear too
    if ((read<std::uint32_t>(entity + kFlags) & kRaceBit) != 0)
        return false; // a race change
    return true;
}

void __cdecl step_hook(std::uint8_t* entity)
{
    if (hold(entity)) {
        if (++g_skipped == 1)
            log::write(L"No blink: the local character's gear change is left out of the model rebuild ([client] blink.disable=1)");
        return;
    }
    g_step(entity);
}

// The game's step and the player getter, the first time they're wanted once FFXiMain.dll is there
void find_and_hook()
{
    if (g_searched)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const std::uintptr_t at = scan::find(game, kStep);
    const std::uintptr_t player = scan::find(game, kPlayer);
    if (at == 0 || player == 0) {
        log::write(L"No blink: the game's model rebuild step wasn't found in FFXiMain.dll: [client] blink.disable does nothing");
        return;
    }
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + kAllowedCall + 1), 4);
    const std::uintptr_t allowed = at + kAllowedCall + 5 + rel;
    if (allowed <= base || std::memcmp(reinterpret_cast<const void*>(allowed), kAllowedStart, sizeof(kAllowedStart)) != 0) {
        log::write(L"No blink: the model rebuild step at +0x%X isn't as expected: [client] blink.disable does nothing",
                   static_cast<unsigned>(at - base));
        return;
    }
    g_player = reinterpret_cast<Player>(player);
    if (!imports::detour(reinterpret_cast<Step>(at), kPrologue, &step_hook, g_step)) {
        g_player = nullptr;
        log::write(L"No blink: the game's model rebuild step couldn't be hooked: [client] blink.disable does nothing");
        return;
    }
    log::write(L"No blink: the game's model rebuild step hooked at +0x%X (player getter +0x%X)",
               static_cast<unsigned>(at - base), static_cast<unsigned>(player - base));
}

} // namespace

void reload(const wchar_t* ini)
{
    const bool on = GetPrivateProfileIntW(L"client", L"blink.disable", 0, ini) != 0;
    if (on != g_on)
        g_skipped = 0;
    g_on = on;
    g_weapons = GetPrivateProfileIntW(L"client", L"blink.weapons", 0, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_on && !g_searched)
        find_and_hook();
}

} // namespace vanacore::noblink
