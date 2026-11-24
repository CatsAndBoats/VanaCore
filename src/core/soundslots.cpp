#include "soundslots.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "imports.hpp"
#include "log.hpp"
#include "registry.hpp"
#include "scan.hpp"

namespace vanacore::soundslots {
namespace {

constexpr int kGame = 20;              // the slots FFXI makes
constexpr int kMost = 100;             // the most VanaCore makes
constexpr std::uint32_t kSlot = 0xF4;  // one slot's bytes
constexpr std::uint32_t kGameEnd = kGame * kSlot; // 1310h: where a walk over the slots stops

// The sound setup's slots (FUN_10249F40): push 4+20*F4h; ...; call <new>; ...; lea edi,[eax+4]; mov [eax],14h;
// mov esi,14h; <each slot's part at +88h made>; mov [<the slots>],edi
constexpr char kSetup[] = "68 14 13 00 00 89 2D ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF 89 2D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                          "83 C4 04 3B C5 74 ?? 8D 78 04 C7 00 14 00 00 00 BE 14 00 00 00 8D 97 88 00 00 00 8B CA E8 ?? ?? ?? ?? "
                          "81 C2 F4 00 00 00 4E 75 F0 89 3D";
constexpr std::ptrdiff_t kSetupSize = 1, kSetupNew = 27, kSetupCount = 44, kSetupLoop = 49, kSetupSlots = 77;
// Then the slots cleared: mov ecx,4C4h (their dwords); xor eax,eax; rep stosd; mov edi,[<the music's>]
constexpr char kClear[] = "B9 C4 04 00 00 33 C0 F3 AB 8B 3D";
constexpr std::size_t kClearWithin = 0x400; // after the setup's start
// The sound thread (FUN_102441D0): while running: <the queue's work>(1); Sleep(16)
constexpr char kThread[] = "56 8B 74 24 08 8B 46 08 85 C0 74 ?? 57 8B 3D ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 6A 10 FF D7";
// The cap (FUN_10038140): mov edi,[<max sounds>]; call; test al,al; jz; mov al,[]; test al,al; jz; mov edi,14h (20 for
// a while, whatever the setting); mov ecx,[<sound>]; push -1; push -1; push 5 (count the sounds playing); call
constexpr char kCap[] = "8B 3D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? A0 ?? ?? ?? ?? 84 C0 74 05 BF 14 00 00 00 8B 0D ?? ?? ?? ?? "
                        "6A FF 6A FF 6A 05 E8";
constexpr std::ptrdiff_t kCapSetting = 2, kCapForced = 25;
// Where the walks over the slots are, about the setup: 26 of them, and the debug list's (add ecx,1310h; add esi,74h),
// which fills a caller's 20 entries and is left at 20
constexpr std::ptrdiff_t kWalksBefore = 0x6000, kWalksAfter = 0x1000;
constexpr int kWalks = 26;

using New = void*(__cdecl*)(std::size_t);
using CreateThreadFunction = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

int g_slots = 0; // asked for: 21 to 100
bool g_done = false;
HMODULE g_game = nullptr;
CreateThreadFunction g_create_thread = nullptr;

std::uint32_t read32(std::uintptr_t at)
{
    std::uint32_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof value);
    return value;
}

bool write32(std::uintptr_t at, std::uint32_t value)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), sizeof value, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), &value, sizeof value);
    VirtualProtect(reinterpret_cast<void*>(at), sizeof value, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), sizeof value);
    return true;
}

// A walk's end at `p` (its 1310h): lea r,[r+1310h], add r,1310h or add eax,1310h
bool walk_end(const std::uint8_t* p)
{
    if (read32(reinterpret_cast<std::uintptr_t>(p)) != kGameEnd)
        return false;
    const std::uint8_t op = p[-2], modrm = p[-1];
    return modrm == 0x05 /* add eax */ || (op == 0x81 && (modrm & 0xF8) == 0xC0) ||
           (op == 0x8D && (modrm & 0xC0) == 0x80 && (modrm & 7) != 4);
}

bool debug_list(const std::uint8_t* p)
{
    return p[-2] == 0x81 && p[-1] == 0xC1 && p[4] == 0x83 && p[5] == 0xC6 && p[6] == 0x74;
}

// The walks' ends about the setup; empty unless there are as many as VanaCore knows, and the debug list's
std::vector<std::uintptr_t> find_walks(std::uintptr_t setup)
{
    std::vector<std::uintptr_t> walks;
    int lists = 0;
    for (std::uintptr_t at = setup - kWalksBefore; at < setup + kWalksAfter; ++at) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(at);
        if (!walk_end(p))
            continue;
        if (debug_list(p))
            ++lists;
        else
            walks.push_back(at);
    }
    if (walks.size() != kWalks || lists != 1)
        walks.clear();
    return walks;
}

struct Sites {
    std::uintptr_t setup = 0, clear = 0, cap = 0;
    std::vector<std::uintptr_t> walks;
};

// Every place the slots' 20 is; nullptr, else what isn't as VanaCore knows it
const wchar_t* find_sites(Sites& s)
{
    s.setup = scan::find(g_game, kSetup);
    if (s.setup == 0)
        return L"the sound setup";
    s.clear = scan::find(s.setup, kClearWithin, kClear);
    if (s.clear == 0)
        return L"the sound setup's clearing";
    s.cap = scan::find(g_game, kCap);
    if (s.cap == 0)
        return L"the max sounds check";
    s.walks = find_walks(s.setup);
    if (s.walks.empty())
        return L"the walks over the sound slots";
    return nullptr;
}

// The code made to make and walk `slots` slots; false if a write failed (then the game is left half changed: the
// caller doesn't swap the slots, which the code then walks past the end of, so it's logged as a fault)
bool rewrite(const Sites& s, std::uint32_t slots)
{
    const std::uint32_t bytes = slots * kSlot;
    bool ok = write32(s.setup + kSetupSize, 4 + bytes) && write32(s.setup + kSetupCount, slots) &&
              write32(s.setup + kSetupLoop, slots) && write32(s.clear + 1, bytes / 4) && write32(s.cap + kCapForced, slots);
    for (const std::uintptr_t walk : s.walks)
        ok = ok && write32(walk, bytes);
    return ok;
}

// The 20 slots the game made swapped for `slots`, from its own allocator (it frees them at the end, by the count before
// them). The old ones, empty, are left.
bool swap(const Sites& s, std::uint32_t slots)
{
    auto* const global = reinterpret_cast<std::uint8_t**>(read32(s.setup + kSetupSlots));
    std::uint8_t* const old = *global;
    if (old == nullptr || read32(reinterpret_cast<std::uintptr_t>(old) - 4) != kGame)
        return false;
    const auto game_new = reinterpret_cast<New>(s.setup + kSetupNew + 5 + static_cast<std::int32_t>(read32(s.setup + kSetupNew + 1)));
    auto* const block = static_cast<std::uint8_t*>(game_new(4 + slots * kSlot));
    if (block == nullptr)
        return false;
    std::memset(block, 0, 4 + slots * kSlot); // as the game leaves its own: made, then cleared
    std::memcpy(block, &slots, sizeof slots);
    *global = block + 4;
    return true;
}

void make_slots()
{
    Sites s;
    if (const wchar_t* missing = find_sites(s)) {
        log::write(L"Sound: %ls isn't as VanaCore knows it in FFXiMain.dll: the game keeps its 20 sound slots", missing);
        return;
    }
    const auto slots = static_cast<std::uint32_t>(g_slots);
    if (!rewrite(s, slots)) {
        log::write(L"Sound: FFXiMain.dll's code couldn't be changed for %u sound slots", slots);
        return;
    }
    if (!swap(s, slots)) {
        log::write(L"Sound: the game's 20 sound slots couldn't be swapped for %u", slots);
        return;
    }
    // what the game was given for max sounds, made what there are slots for (more would ask for slots there aren't)
    *reinterpret_cast<std::uint32_t*>(read32(s.cap + kCapSetting)) = slots;
    const auto base = reinterpret_cast<std::uintptr_t>(g_game);
    log::write(L"Sound: %u sound slots (the game's 20: setup +0x%X, %d walks)", slots, static_cast<unsigned>(s.setup - base),
               kWalks);
}

HANDLE WINAPI create_thread(LPSECURITY_ATTRIBUTES security, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter,
                            DWORD flags, LPDWORD id)
{
    // The sound thread is started once the slots are made and before anything else uses them
    if (!g_done && scan::find(reinterpret_cast<std::uintptr_t>(start), sizeof kThread / 3, kThread) ==
                       reinterpret_cast<std::uintptr_t>(start)) {
        g_done = true;
        make_slots();
    }
    return g_create_thread(security, stack, start, parameter, flags, id);
}

void on_game_loaded(HMODULE module)
{
    g_game = module;
    if (!imports::hook(module, "KERNEL32.dll", "CreateThread", &create_thread, g_create_thread))
        log::write(L"Sound: FFXiMain.dll's CreateThread can't be hooked: the game keeps its 20 sound slots");
}

} // namespace

void install()
{
    const std::wstring text = registry::setting("0029");
    const long asked = text.empty() ? 0 : std::wcstol(text.c_str(), nullptr, 10);
    if (asked <= kGame)
        return;
    g_slots = static_cast<int>(std::min<long>(asked, kMost));
    if (asked > kMost)
        log::write(L"Sound: sound.max_sounds=%ld is more than the %d sound slots VanaCore makes: %d", asked, kMost, kMost);
    if (!imports::when_loaded(L"FFXiMain.dll", &on_game_loaded))
        log::write(L"Sound: FFXiMain.dll can't be watched for: the game keeps its 20 sound slots");
}

} // namespace vanacore::soundslots
