#include "lod.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::lod {
namespace {

// [graphics] lod=high|game. FFXiMain.dll's zone renderer picks each object's mesh every frame from the
// three its placement holds (xi-tools docs/zone/lod-draw-distance.md): the high-detail one within the
// placement's near distance, the medium out to its mid distance, the low past it. So buildings and trees
// swap meshes as the camera moves - the low ones drop whole faces - and the sun's shadows change shape
// with them. Two renderers do it (the zone picks one); in each, the loads of the low and medium meshes
// (the placement's +0x10, +0x14) become loads of the high one (+0x18). No slot is empty (the game fills
// a missing variant from the others), and how far away an object stops being drawn is unchanged.
//   fld [esp+d]; fcomp [reg+0CCh] (mid); fnstsw ax; and eax,4100h; jne; mov r,[reg+10h] (low) ...
//   fld [esp+d]; fcomp [reg+0C8h] (near); ...; jne; mov r,[reg+14h] (medium) ...; mov r,[reg+18h] (high)
struct LodSite
{
    const char* pattern;
    int at[2]; // the low and medium loads' displacement bytes
};
constexpr LodSite kLodSites[] = {
    {"D9 44 24 ?? D8 9F CC 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 57 10 89 54 24 ?? EB ?? "
     "D9 44 24 ?? D8 9F C8 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 47 14 89 44 24 ?? EB ?? 8B 4F 18",
     {0x15, 0x31}},
    {"D9 44 24 ?? D8 99 CC 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 59 10 EB ?? "
     "D9 44 24 ?? D8 99 C8 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 59 14 EB ?? 8B 59 18",
     {0x15, 0x2D}},
};
constexpr std::uint8_t kLodBytes[2] = {0x10, 0x14}; // the game's: low, medium
constexpr std::uint8_t kHighMesh = 0x18;

// lod=range: each object keeps its detail lod.range times farther out. In each site, the camera's
// distance (squared) is compared with the placement's mid and near distances (squared): each compare
// (fld [esp+d]; fcomp [reg+0CCh] or [reg+0C8h], 10 bytes) jumps to a copy of itself that divides the
// distance by lod.range squared first, and back.
constexpr int kLodCompares[2][2] = {{0x00, 0x1C}, {0x00, 0x18}}; // mid, near: from each site
constexpr int kCompareSize = 10;
constexpr int kStubSize = 32;

// Only touched on the game's thread.
bool g_lod_high = false;    // [graphics] lod=high (game is the default)
float g_lod_range = 0.0f;   // lod=range: lod.range; 0 for the other modes
bool g_lod_searched = false;
bool g_lod_applied = false; // the high mesh is drawn at every distance now
bool g_lod_range_applied = false; // the compares go through the stubs now
std::uintptr_t g_lod_site[2] = {};
std::uint8_t* g_lod_stubs = nullptr;               // lod=range's copies of the compares
std::uint8_t g_lod_compare[2][2][kCompareSize] = {}; // the game's compares, as they were
float g_lod_range_sq = 1.0f;                       // read by the stubs

bool write_code(std::uintptr_t at, std::uint8_t value)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    *reinterpret_cast<std::uint8_t*>(at) = value;
    VirtualProtect(reinterpret_cast<void*>(at), 1, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 1);
    return true;
}

bool write_code(std::uintptr_t at, const std::uint8_t* bytes, std::size_t size)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    return true;
}

// lod=range on (the compares through the stubs, kLodCompares) or off (the game's compares back).
bool range_lod(bool on)
{
    if (on && g_lod_stubs == nullptr) {
        g_lod_stubs = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4 * kStubSize, MEM_COMMIT | MEM_RESERVE,
                                                               PAGE_EXECUTE_READWRITE));
        if (g_lod_stubs == nullptr)
            return false;
    }
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2 && g_lod_site[i] != 0; ++j) {
            const std::uintptr_t at = g_lod_site[i] + kLodCompares[i][j];
            std::uint8_t* const original = g_lod_compare[i][j];
            if (!on) {
                ok &= original[0] == 0 || write_code(at, original, kCompareSize);
                continue;
            }
            std::memcpy(original, reinterpret_cast<const void*>(at), kCompareSize);
            // fld [esp+d]; fcomp [reg+0CCh|0C8h] as the game has it: anything else is someone else's
            // patch (VanaView's lod range), left alone
            if (original[0] != 0xD9 || original[1] != 0x44 || original[2] != 0x24 || original[4] != 0xD8 ||
                (original[5] & 0xF8) != 0x98 || (original[6] != 0xCC && original[6] != 0xC8) || original[7] != 0 ||
                original[8] != 0 || original[9] != 0) {
                original[0] = 0;
                ok = false;
                continue;
            }
            // fld [esp+d]; fdiv [g_lod_range_sq]; fcomp [reg+disp]; jmp back
            std::uint8_t* const stub = g_lod_stubs + (i * 2 + j) * kStubSize;
            const auto scale = reinterpret_cast<std::uintptr_t>(&g_lod_range_sq);
            std::memcpy(stub, original, 4);
            stub[4] = 0xD8, stub[5] = 0x35;
            std::memcpy(stub + 6, &scale, 4);
            std::memcpy(stub + 10, original + 4, 6);
            stub[16] = 0xE9;
            const auto back = static_cast<std::int32_t>(at + kCompareSize - reinterpret_cast<std::uintptr_t>(stub + 21));
            std::memcpy(stub + 17, &back, 4);
            // jmp stub; nop x5
            std::uint8_t jump[kCompareSize] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
            const auto there = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(stub) - (at + 5));
            std::memcpy(jump + 1, &there, 4);
            ok &= write_code(at, jump, kCompareSize);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), g_lod_stubs, 4 * kStubSize);
    return ok;
}

} // namespace

void set(bool high, float range)
{
    g_lod_high = high;
    g_lod_range = range;
}

// lod=high: the zone's objects drawn with their high-detail meshes at any distance (kLodSites); lod=range:
// each keeps its detail lod.range times farther; lod=game puts the game's choice back. Found the first
// time it's wanted, once FFXiMain.dll is there.
void apply()
{
    if (!g_lod_searched) {
        if (!g_lod_high && !(g_lod_range > 0))
            return;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return;
        g_lod_searched = true;
        for (int i = 0; i < 2; ++i) {
            const std::uintptr_t site = scan::find(game, kLodSites[i].pattern);
            const auto* code = reinterpret_cast<const std::uint8_t*>(site);
            // the game's bytes, or ours already there: anything else is someone else's patch, left alone
            if (site != 0 && ((code[kLodSites[i].at[0]] == kLodBytes[0] && code[kLodSites[i].at[1]] == kLodBytes[1]) ||
                              (code[kLodSites[i].at[0]] == kHighMesh && code[kLodSites[i].at[1]] == kHighMesh)))
                g_lod_site[i] = site;
        }
        if (g_lod_site[0] == 0 && g_lod_site[1] == 0) {
            log::write(L"Graphics: couldn't find where the game picks its zone objects' detail; they keep swapping meshes "
                       L"with distance ([graphics] lod)");
            return;
        }
        log::write(L"Graphics: the game picks zone objects' detail at FFXiMain.dll+0x%X and +0x%X ([graphics] lod)",
                   static_cast<unsigned>(g_lod_site[0] ? g_lod_site[0] - reinterpret_cast<std::uintptr_t>(game) : 0),
                   static_cast<unsigned>(g_lod_site[1] ? g_lod_site[1] - reinterpret_cast<std::uintptr_t>(game) : 0));
    }
    const bool range = g_lod_range > 0;
    if (range)
        g_lod_range_sq = g_lod_range * g_lod_range;
    if (range != g_lod_range_applied) {
        const bool ok = range_lod(range);
        g_lod_range_applied = range;
        if (!ok)
            log::write(L"Graphics: couldn't change how far the game keeps zone objects' detail ([graphics] lod=range)");
        else if (range)
            log::write(L"Graphics: zone objects keep their detail %g times farther ([graphics] lod=range)", g_lod_range);
        else if (!g_lod_high)
            log::write(L"Graphics: zone objects' detail as the game picks it by distance ([graphics] lod=game)");
    }
    if (g_lod_high == g_lod_applied)
        return;
    bool ok = true;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2 && g_lod_site[i] != 0; ++j)
            ok &= write_code(g_lod_site[i] + kLodSites[i].at[j], g_lod_high ? kHighMesh : kLodBytes[j]);
    g_lod_applied = g_lod_high;
    log::write(!ok ? L"Graphics: couldn't change how the game picks zone objects' detail ([graphics] lod)"
               : g_lod_high ? L"Graphics: zone objects drawn in full detail at any distance ([graphics] lod=high)"
                            : L"Graphics: zone objects' detail as the game picks it by distance ([graphics] lod=game)");
}

} // namespace vanacore::lod
