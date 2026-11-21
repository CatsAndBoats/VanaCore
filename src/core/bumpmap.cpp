#include "bumpmap.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::bumpmap {
namespace {

// FFXiMain.dll's bump mapping (RE 2026-10-11; see memory ffxi-bump-mapping-re). The renderer object (hung from a
// global) keeps, as offsets:
constexpr std::uintptr_t kSlots = 0x60;      // the zones' slots: 2 of 0x78 bytes, +0 in use, +0x58 the bump DAT's resource
constexpr std::uintptr_t kSlotSize = 0x78;
constexpr int kSlotCount = 2;
constexpr std::uintptr_t kBumpResource = 0x58;
constexpr std::uintptr_t kShader = 0x3946C;  // the first of the five vertex shaders' handles; 0 until they're made
constexpr std::uintptr_t kPixelShader = 0x39494;
constexpr std::uintptr_t kLoaded = 0x39498;  // the bump shaders are there: each zone load brings the bump DAT, the chunks use it
constexpr std::uintptr_t kDraw = 0x3949C;    // the zone's meshes are drawn with the bump shader (== 1)
constexpr std::uintptr_t kKept = 0x394A0;    // kDraw's value for the next zone load (the game copies it back then)

// The renderer's startup routine (a thiscall with no arguments; this is the renderer):
//   sub esp,4Ch; push esi; mov esi,ecx; lea ecx,[esp+2Ch]; mov [esp+34h],20000000h; mov [esp+38h],40020000h
// It clears kLoaded and kDraw, and if the option's global is set (and the device can) assembles the shaders and
// sets both to 1.
constexpr char kSetup[] = "83 EC 4C 56 8B F1 8D 4C 24 2C C7 44 24 34 00 00 00 20 C7 44 24 38 00 00 02 40";

// ... and the global it asks, in the step that clears them:
//   xor eax,eax; mov [esi+39498h],eax; mov [esi+3949Ch],eax; cmp [option],eax
constexpr char kOption[] = "33 C0 89 86 98 94 03 00 89 86 9C 94 03 00 39 05 ?? ?? ?? ??";
constexpr std::uintptr_t kOptionAt = 16;

// The global the renderer hangs from, where the zone's chunks are built:
//   mov ecx,[renderer]; mov [esp+78h],eax; add esp,8; cmp [ecx+39498h],ebp
constexpr char kRenderer[] = "8B 0D ?? ?? ?? ?? 89 44 24 78 83 C4 08 39 A9 98 94 03 00";
constexpr std::uintptr_t kRendererAt = 2;

// Where the game copies kKept to kDraw for each zone load (and the object holding the zone's number, +1B4h, which
// keeps it off in three zones: 0xB6, 0x2D3, 0x2E6):
//   mov eax,[esi+394A0h]; mov [esi+3949Ch],eax; mov ecx,[zone]; cmp ecx,ebx; je; mov ecx,[ecx+1B4h]; sub ecx,0B6h
constexpr char kZone[] = "8B 86 A0 94 03 00 89 86 9C 94 03 00 8B 0D ?? ?? ?? ?? 3B CB 74 ?? 8B 89 B4 01 00 00 81 E9 B6 00 00 00";
constexpr std::uintptr_t kZoneAt = 14;
constexpr std::uint32_t kZoneIdAt = 0x1B4;
constexpr std::uint32_t kNoBumpZones[] = {0xB6, 0x2D3, 0x2E6};

using Setup = void(__fastcall*)(void* renderer, void* edx);

// Only touched on the game's thread.
bool g_searched = false;
bool g_found = false;
Setup g_setup = nullptr;
std::uint32_t* g_option = nullptr;       // the game's option, as the game's config sets it
std::uint32_t* const* g_renderer = nullptr; // the global the renderer is in
const std::uint32_t* const* g_zone = nullptr;
bool g_game_wants = false;               // the option as the game started with it
bool g_game_known = false;
bool g_failed = false;                   // the shaders couldn't be made (the device can't); until the mode changes
bool g_wanted_known = false;
bool g_wanted_last = false;

std::uint32_t& field(std::uint8_t* renderer, std::uintptr_t at)
{
    return *reinterpret_cast<std::uint32_t*>(renderer + at);
}

// Anything of the current zones' bump DATs still loaded: the game frees them only while kLoaded is set
bool zone_data(std::uint8_t* renderer)
{
    for (int i = 0; i < kSlotCount; ++i)
        if (field(renderer, kSlots + kSlotSize * i + kBumpResource) != 0)
            return true;
    return false;
}

// kSlots' first slot is in use: the renderer has a zone, so its startup is done
bool started(std::uint8_t* renderer)
{
    return field(renderer, kSlots) != 0;
}

// The zone the game has loaded (0 before it has one)
std::uint32_t current_zone()
{
    if (g_zone == nullptr || *g_zone == nullptr)
        return 0;
    return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(*g_zone) + kZoneIdAt);
}

bool zone_keeps_it_off()
{
    const std::uint32_t zone = current_zone();
    for (const std::uint32_t off : kNoBumpZones)
        if (zone == off)
            return true;
    return false;
}

bool search()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const std::uintptr_t setup = scan::find(game, kSetup);
    const std::uintptr_t option = scan::find(game, kOption);
    const std::uintptr_t renderer = scan::find(game, kRenderer);
    const std::uintptr_t zone = scan::find(game, kZone);
    if (setup == 0 || option == 0 || renderer == 0 || zone == 0) {
        log::write(L"Graphics: couldn't find the game's bump mapping (setup %ls, option %ls, renderer %ls, zone %ls); "
                   L"it stays as the game's option says ([graphics] bump)",
                   setup ? L"found" : L"missing", option ? L"found" : L"missing", renderer ? L"found" : L"missing",
                   zone ? L"found" : L"missing");
        return true;
    }
    g_setup = reinterpret_cast<Setup>(setup);
    g_option = *reinterpret_cast<std::uint32_t* const*>(option + kOptionAt);
    g_renderer = *reinterpret_cast<std::uint32_t* const* const*>(renderer + kRendererAt);
    g_zone = *reinterpret_cast<const std::uint32_t* const* const*>(zone + kZoneAt);
    g_found = g_option != nullptr && g_renderer != nullptr;
    if (g_found)
        log::write(L"Graphics: the game's bump mapping: setup at FFXiMain.dll+0x%X ([graphics] bump)",
                   static_cast<unsigned>(setup - reinterpret_cast<std::uintptr_t>(game)));
    return true;
}

} // namespace

void apply(Mode mode)
{
    if (!g_searched && !search())
        return;
    if (!g_found)
        return;
    auto* const renderer = reinterpret_cast<std::uint8_t*>(*g_renderer);
    if (renderer == nullptr || !started(renderer))
        return;
    // The game's own, as it was before anything here touched it
    if (!g_game_known) {
        g_game_known = true;
        g_game_wants = *g_option != 0;
    }
    const bool wanted = mode == Mode::On ? true : mode == Mode::Off ? false : g_game_wants;
    if (!g_wanted_known || wanted != g_wanted_last) {
        g_failed = false;
        if (g_wanted_known || mode != Mode::Game)
            log::write(wanted ? L"Graphics: bump mapping on: from the next zone ([graphics] bump)"
                              : L"Graphics: bump mapping off ([graphics] bump)");
        g_wanted_known = true;
        g_wanted_last = wanted;
    }
    const bool made = field(renderer, kShader) != 0 && field(renderer, kPixelShader) != 0;
    if (wanted) {
        if (g_failed)
            return;
        if (!made) {
            // The game's own routine, which also clears kLoaded and kDraw and sets both: kDraw back to what it was,
            // for the zone load to raise it with the bump DAT in hand
            const std::uint32_t draw = field(renderer, kDraw);
            *g_option = 1;
            g_setup(renderer, nullptr);
            field(renderer, kDraw) = draw;
            if (field(renderer, kShader) == 0 || field(renderer, kPixelShader) == 0 || field(renderer, kLoaded) == 0) {
                g_failed = true;
                *g_option = 0;
                field(renderer, kLoaded) = 0;
                log::write(L"Graphics: the game couldn't make its bump mapping shaders (the graphics device can't); "
                           L"it stays off ([graphics] bump)");
                return;
            }
            log::write(L"Graphics: the game's bump mapping shaders made");
        }
        *g_option = 1;
        field(renderer, kLoaded) = 1;
        field(renderer, kKept) = 1;
        // This zone's bump DAT is there already (it was loaded before and turned off): bump again at once, but for the
        // three zones the game keeps it off in
        if (field(renderer, kDraw) == 0 && zone_data(renderer) && !zone_keeps_it_off())
            field(renderer, kDraw) = 1;
        return;
    }
    *g_option = 0;
    field(renderer, kDraw) = 0;
    field(renderer, kKept) = 0;
    // Left set while a zone holds its bump DAT: the game frees it only then. Cleared when the zone changes (the next
    // zone load brings none).
    if (!zone_data(renderer))
        field(renderer, kLoaded) = 0;
}

} // namespace vanacore::bumpmap
