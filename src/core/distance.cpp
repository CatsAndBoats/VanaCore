#include "distance.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <vector>

#include "imports.hpp"
#include "game.hpp"
#include "log.hpp"
#include "packets.hpp"
#include "plugins.hpp"
#include "scan.hpp"

namespace vanacore::distance {
namespace {

// FFXiMain.dll's draw distance (FUN_10187d70): how far the zone (kind 0) and the characters (kind 1)
// are drawn, the area's distance times a multiplier the game keeps in two floats. The game sets them
// from its own config, 0.5 to 1 (the launcher's drawdistance addon made them 5). Ashita's drawdistance
// addon finds them the same way:
//   mov eax,ecx; dec eax; je +8; fmul [scenery]; jmp +6; fmul [characters]
constexpr char kMultipliers[] = "8B C1 48 74 08 D8 0D ?? ?? ?? ?? EB 06 D8 0D";
constexpr std::uintptr_t kSceneryAt = 7, kMobsAt = 0xF;

// The game's fog (FUN_10187be0, a thiscall: color*, end*, start*, kind), which every fog it draws
// with comes from: the area's fog range times the same draw distance, so a farther draw distance
// pushes the fog out with it.
//   push ebp; push esi; mov esi,ecx; push edi; mov eax,[esi-1Ch]; cmp eax,1; mov eax,[esp+10h]; je;
//   mov ecx,[esp+14h]; mov edx,[esp+18h]; mov dword [eax],-1 (no area: white fog, 0 to 400)
constexpr char kFog[] = "55 56 8B F1 57 8B 46 E4 83 F8 01 8B 44 24 10 74 ?? 8B 4C 24 14 8B 54 24 18 C7 00 FF FF FF FF";
constexpr std::size_t kFogPrologue = 5; // push ebp; push esi; mov esi,ecx; push edi

// What can be targeted (drawdistance.target=game). The game has no line of sight: Tab, the mouse and the
// nearest-target keys take any character it drew this frame, out to 50 yalms, walls or not. A character past
// the characters' draw distance isn't drawn (FUN_100C6120 marks it, actor+0xB2), so a farther drawdistance.mobs
// makes doors and NPCs behind walls targetable that the game's own distance hides. The game's check, a cdecl
// (actor, player's, types, types refused, self too) that every target pick calls (FUN_10083240):
//   sub esp,8; push ebx; push ebp; push esi; push edi; mov ecx,<target list>; call; mov edi,[esp+1Ch]; mov ebp,eax;
//   mov ecx,edi; mov [esp+14h],ebp; call; cmp al,1; sete [esp+1Ch]
constexpr char kTargetable[] = "83 EC 08 53 55 56 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 7C 24 1C 8B E8 8B CF 89 6C 24 14 "
                               "E8 ?? ?? ?? ?? 3C 01 0F 94 44 24 1C";
constexpr std::size_t kTargetablePrologue = 5; // sub esp,8; push ebx; push ebp
// The characters' draw distance (FUN_1018A3B0, a thiscall on the game's config: kind 1), the area's distance times
// the multiplier: a character whose depth (actor+0x8C, the camera's; 0.1 for those always drawn) is past it isn't
// drawn. The config is the global the multipliers' code loads next (kMultipliers + kConfigAt: mov ecx,[config]).
//   push ecx; mov eax,[esp+8]; push esi; push eax; mov esi,ecx; mov dword [esp+8],100.0; call <multiplier>
constexpr char kCharDistance[] = "51 8B 44 24 08 56 50 8B F1 C7 44 24 08 00 00 C8 42 E8";
constexpr std::uintptr_t kConfigAt = 0x13, kConfigArea = 0x1C, kActorDepth = 0x8C;

// The zone's view lists: for the part of the zone the camera is in, the zone's file lists the pieces to
// draw from there (renderer+0x3912C, from FUN_1016AC10), made for the game's own draw distance. Past it
// the ground is left out while the big trees stay listed, so a farther draw distance can show trees standing
// on nothing (Windurst Woods from its ditches). With no list, the game walks the zone's quadtree instead,
// every piece in view: far more pieces drawn, a large cost in frames, so it's drawdistance.full's alone (off
// by default; 0.7.42 to 0.7.47 did it whenever drawdistance was above 1). The jump to that walk, in the
// zone's draw (0x1017AD68):
//   mov eax,[esp+20h]; cmp eax,ebx; je <quadtree>; cmp [esi-4],ebx; je; mov edx,[ebp+3944Ch]; lea esi,[eax+4]
constexpr char kViewList[] = "8B 44 24 20 3B C3 ?? ?? ?? ?? ?? ?? 39 5E FC 0F 84 ?? ?? ?? ?? 8B 95 ?? ?? ?? ?? 8D 70 04";
constexpr std::uintptr_t kViewListJump = 6; // the je (0F 84 rel32); ours is a jmp there (E9 rel32+1, nop)
constexpr std::size_t kViewListSize = 6;

// Far copies. Some zones place a cheap copy of an object on the spot of the detailed one, for seeing it from
// across the map, and list the two in view lists that never overlap: the list the camera stands in picks one
// (Ru'Aun Gardens' m_bri_* bridges, m_osid_* god islands and pip_*_n pillars over bri_*_h, osid_*_h and pip_*;
// Eastern Adoulin's low* walls). The quadtree has no such choice, so past the view lists both draw, fighting
// over the same pixels. Each zone's pairs are found when it loads (two pieces with different meshes whose boxes
// mostly overlap, IoU >= 0.5, and that no list holds together, as the XI Model Viewer does); then, in the
// quadtree's chunk loop, a piece the camera's list leaves out isn't drawn while its twin is listed, and with
// neither listed (out past the game's own distance) only the far copy is. Its chunk test, in FUN_1017B370:
//   mov edx,[ebx+60h]; mov eax,[edx+eax*4]; cmp byte [eax+8],1; jbe <next>; mov cl,[eax+0Fh]; test cl,cl
constexpr char kNodeChunks[] = "8B 53 60 8B 04 82 80 78 08 01 76 ?? 8A 48 0F 84 C9";
constexpr std::uintptr_t kNodeChunkTest = 6; // cmp byte [eax+8],1; jbe (6 bytes): ours jumps to the stub
constexpr std::size_t kNodeChunkSize = 6;
// The zone renderer (esi there): its two zone slots (FUN_10177EF0) and the camera's view list
constexpr std::uintptr_t kZoneSlots = 0x60, kZoneSlotSize = 0x78, kCameraList = 0x3912C;
constexpr std::uintptr_t kSlotChunkCount = 0x24, kSlotChunks = 0x28, kSlotListCount = 0x2C, kSlotLists = 0x30;
// A chunk (PositionedMeshBlock): its placement's far flag (the record's +0x45, 2 on far copies), high mesh and the
// world corners of that mesh's box (FUN_101791D0). The lists are {count, chunk pointers} (FUN_101799F0).
constexpr std::uintptr_t kChunkSize = 0x16C, kChunkFar = 0x0A, kChunkMesh = 0x18, kChunkCorners = 0xF4;
constexpr int kMaxLists = 4096;

// Past the lists (drawdistance.fill, drawdistance above 1, without drawdistance.full): the zone drawn from the camera's list as the
// game does, then the quadtree walked too for the big pieces the list leaves out (kFillMin across or more: the
// ground and buildings; narrower than kFillSize of their distance is too small to make out). The lists are made
// for the game's own fog, so they leave out ground that can be seen past it, through gaps (Windurst Woods from
// its ditches lists pieces out to about 250 but leaves out others from 40 on); most pieces they leave out are
// small props, and those stay out. The list walk's end, which goes on to the quadtree's instead of to the end:
//   mov [esp+20h],ebx; jmp <end>; cmp [esi],ebx (the quadtree's start); je; mov eax,[ebp+3944Ch]; mov [esp+14h],4
constexpr char kListEnd[] = "89 5C 24 20 E9 ?? ?? ?? ?? 39 1E 74 ?? 8B 85 ?? ?? ?? ?? C7 44 24 14 04 00 00 00";
constexpr std::uintptr_t kListEndJump = 4, kQuadtreeStart = 9; // the jmp to the end (5 bytes); the quadtree's walk
constexpr std::size_t kListEndSize = 5;
// The slot has lists (the list walk needs it), the camera's eye, and the draw distance squared as the game set it this
// frame (DrawDistanceSq: its own times the multiplier, squared; what each piece's distance is tested against)
constexpr std::uintptr_t kSlotHasLists = 0x34, kCameraEye = 0x390F0, kDrawDistanceSq = 0x39424;
constexpr float kFillMin = 25.0f, kFillSize = 0.02f;

// The zones held at the game's own scenery distance (drawdistance.large_off). With no view lists the game draws a zone
// whole from its quadtree, every piece in view out to the draw distance, and at 5x Al'Taieu's is nearly all of it (7,385
// pieces, most made to be seen 400 to 800 out): about 2,000 pieces within reach went to 5,300, 8,300 draws a frame against
// 3,500, 27 fps against 60 (2026-11-04). A list, not every zone without lists: the Sanctuary of Zi'Tah has none either and
// lost its extended distance to that (0.8.164, players' reports); ships' zones keep theirs (their sea and coast). Any
// other zone without lists is logged as it loads, for this list
constexpr std::uint16_t kLargeZones[] = {33}; // Al'Taieu
constexpr ULONGLONG kWholeLinger = 1500; // ms the zone stays held after its quadtree was last walked (zoning, a frame unwalked)

// One of the game's multipliers, and ours in its place.
struct Multiplier {
    float* field = nullptr;
    float game = 0.0f; // the game's own (0 until the game has set it)
    float set = -1.0f; // ours, written there; < 0 the game's is
};

// The game's thiscall, called as a fastcall (this in ecx; edx unused; the callee pops the rest).
using FogFunction = void(__fastcall*)(void* self, void* edx, std::uint32_t* color, float* end, float* start, int kind);

// Only touched on the game's thread.
Settings g_settings;
Settings g_logged;             // what the log last said
bool g_searched = false;
Multiplier g_scenery, g_mobs;
bool g_fog_searched = false;
FogFunction g_fog = nullptr;   // the game's fog, through the detour's trampoline
bool g_list_searched = false;
std::uintptr_t g_list_jump = 0;                       // the je; 0 not found
std::uint8_t g_list_game[kViewListSize] = {};         // the game's bytes there
std::uint8_t g_list_ours[kViewListSize] = {};         // our jmp to the quadtree
bool g_list_skipped = false;                          // ours is in

// One zone slot's far copies, worked out when its zone is loaded
struct Twins {
    std::uintptr_t chunks = 0;           // the zone they're for: its chunks and lists as the slot had them
    int count = 0;
    std::uintptr_t lists = 0;
    int list_count = 0;
    std::vector<std::uintptr_t> starts;  // each list's address
    std::vector<int> first, twins;       // chunk i's twins: twins[first[i]] to twins[first[i + 1]]
    std::vector<std::uint8_t> rank_far;  // ... and what makes one the far copy: the placement says so,
    std::vector<int> rank_lists;         // or more of the lists hold it
    std::uintptr_t camera = 1;           // the camera's list the hidden ones are for (1: none worked out)
    bool camera_known = false;           // ... is one of this zone's (listed says what it holds)
    bool far_logged = false;             // the log has said the camera stood where this zone has no list (far_hidden)
    bool whole = false;                  // one of kLargeZones, with no view lists: held (drawdistance.large_off)
    std::vector<std::uint8_t> listed, hidden;
    std::vector<std::uint8_t> cap;       // portal caps (find_caps): never drawn past the lists
    std::vector<std::uint8_t> backdrop;  // backdrop pieces (find_backdrops): drawn only where the camera's list has them
};
Twins g_twins[2];
bool g_node_searched = false;
std::uintptr_t g_node_test = 0;                       // the chunk test; 0 not found
std::uint8_t g_node_game[kNodeChunkSize] = {};        // the game's bytes there
std::uint8_t g_node_ours[kNodeChunkSize] = {};        // our jmp to the stub
std::uint8_t* g_node_stub = nullptr;
bool g_node_hooked = false;
bool g_fill = false;                                  // past the lists: the quadtree after the list (kListEnd)
bool g_end_searched = false;
std::uintptr_t g_end_jump = 0;                        // the list walk's jmp to the end; 0 not found
std::uint8_t g_end_game[kListEndSize] = {};
std::uint8_t g_end_ours[kListEndSize] = {};
std::uint8_t* g_end_stub = nullptr;
bool g_end_hooked = false;
bool g_whole_walked = false;   // twin_hidden saw a whole zone's piece since the last frame (Twins::whole)
ULONGLONG g_whole_at = 0;      // when apply() last found it had
bool g_whole_held = false;     // the zone at the game's own distance for it
// The game's thiscall, called as a fastcall (this in ecx; edx unused; the callee pops kind)
using CharDistanceFunction = float(__fastcall*)(std::uintptr_t config, void* edx, int kind);
using TargetableFunction = std::uint32_t(__cdecl*)(std::uintptr_t actor, std::uintptr_t player, std::uint32_t types,
                                                   std::uint32_t refused, char self);
bool g_target_searched = false;
TargetableFunction g_targetable = nullptr;            // the game's check, through the detour's trampoline
CharDistanceFunction g_char_distance = nullptr;
std::uintptr_t* g_config = nullptr;                   // where the game keeps its config's pointer

void __fastcall fog(void* self, void* edx, std::uint32_t* color, float* end, float* start, int kind)
{
    // fog=game: worked out with the game's own multipliers, put back for the draw distance after
    Multiplier* const multipliers[] = {&g_scenery, &g_mobs};
    float ours[2] = {};
    const bool natural = g_settings.fog == Fog::Game;
    for (int i = 0; i < 2 && natural; ++i)
        if (multipliers[i]->set >= 0 && multipliers[i]->game > 0) {
            ours[i] = *multipliers[i]->field;
            *multipliers[i]->field = multipliers[i]->game;
        }
    // A zone held at the game's own distance (drawdistance.large_off) keeps the fog of the distance asked for: only the
    // pieces drawn come back, not the haze
    const bool held = g_whole_held && g_settings.fog == Fog::Distance;
    const float game = held ? *g_scenery.field : 0.0f;
    if (held)
        *g_scenery.field = g_settings.scenery;
    g_fog(self, edx, color, end, start, kind);
    if (held)
        *g_scenery.field = game;
    for (int i = 0; i < 2 && natural; ++i)
        if (multipliers[i]->set >= 0 && multipliers[i]->game > 0)
            *multipliers[i]->field = ours[i];
    if (g_settings.fog == Fog::Custom && start != nullptr && end != nullptr) {
        *start = g_settings.fog_start;
        *end = g_settings.fog_end;
    }
}

// drawdistance.target=game: what the game's check takes, less the characters drawn past the game's own distance,
// which it would have hidden
std::uint32_t __cdecl targetable(std::uintptr_t actor, std::uintptr_t player, std::uint32_t types, std::uint32_t refused,
                                 char self)
{
    const std::uint32_t taken = g_targetable(actor, player, types, refused, self);
    if ((taken & 0xFF) == 0 || g_settings.target_drawn || actor == 0 || g_mobs.set < 0 || g_mobs.game <= 0 ||
        g_mobs.set <= g_mobs.game)
        return taken;
    const std::uintptr_t config = *g_config;
    if (config == 0 || *reinterpret_cast<const std::uintptr_t*>(config + kConfigArea) == 0)
        return taken; // no area: the game hides no character by distance
    // the game's own distance, its multiplier put back while it's worked out (as fog() does)
    *g_mobs.field = g_mobs.game;
    const float own = g_char_distance(config, nullptr, 1);
    *g_mobs.field = g_mobs.set;
    return *reinterpret_cast<const float*>(actor + kActorDepth) > own ? taken & ~0xFFu : taken;
}

// The game's target check detoured the first time it's wanted; it stays (targetable() passes straight through)
void hook_targets()
{
    g_target_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t check = game != nullptr ? scan::find(game, kTargetable) : 0;
    const std::uintptr_t distance = game != nullptr ? scan::find(game, kCharDistance) : 0;
    g_char_distance = reinterpret_cast<CharDistanceFunction>(distance);
    if (check == 0 || distance == 0 || g_config == nullptr ||
        !imports::detour(reinterpret_cast<TargetableFunction>(check), kTargetablePrologue, &targetable, g_targetable)) {
        log::write(L"Graphics: couldn't find the game's target check; characters can be targeted as far as they're "
                   L"drawn ([graphics] drawdistance.target)");
        return;
    }
    log::write(L"Graphics: the game's target check is at FFXiMain.dll+0x%X ([graphics] drawdistance.target)",
               static_cast<unsigned>(check - reinterpret_cast<std::uintptr_t>(game)));
}

// Ours in the game's multiplier, or the game's put back; learns the game's own whenever the field
// holds something other than ours (the game set it, from its config).
void keep(Multiplier& multiplier, float wanted)
{
    const float now = *multiplier.field;
    if (multiplier.set < 0 || now != multiplier.set)
        multiplier.game = now;
    if (wanted > 0) {
        if (now != wanted)
            *multiplier.field = wanted;
        multiplier.set = wanted;
    } else if (multiplier.set >= 0) {
        *multiplier.field = multiplier.game;
        multiplier.set = -1.0f;
    }
}

bool write_code(std::uintptr_t at, const void* bytes, std::size_t size)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    return true;
}

struct Box {
    float lo[3], hi[3];
};

Box chunk_box(std::uintptr_t chunk)
{
    const auto* corner = reinterpret_cast<const float*>(chunk + kChunkCorners);
    Box box{{corner[0], corner[1], corner[2]}, {corner[0], corner[1], corner[2]}};
    for (int i = 1; i < 8; ++i)
        for (int k = 0; k < 3; ++k) {
            const float v = corner[i * 3 + k];
            box.lo[k] = v < box.lo[k] ? v : box.lo[k];
            box.hi[k] = v > box.hi[k] ? v : box.hi[k];
        }
    return box;
}

// Intersection over union, each box padded half a unit so flat pieces (a floor, a wall) have a volume
float overlap(const Box& a, const Box& b)
{
    float both = 1.0f, va = 1.0f, vb = 1.0f;
    for (int k = 0; k < 3; ++k) {
        const float lo = (a.lo[k] > b.lo[k] ? a.lo[k] : b.lo[k]) - 0.25f;
        const float hi = (a.hi[k] < b.hi[k] ? a.hi[k] : b.hi[k]) + 0.25f;
        if (hi <= lo)
            return 0.0f;
        both *= hi - lo;
        va *= a.hi[k] - a.lo[k] + 0.5f;
        vb *= b.hi[k] - b.lo[k] + 0.5f;
    }
    return both / (va + vb - both);
}

// Portal caps. Castle Zvahl Baileys places b_00 (b_01 in [S]) 38 times: one flat 40x40 wall on the edges of the
// zone's 40-unit grid, held by a few of its view lists, the ones for spots whose list leaves out the part of the
// castle behind it, so the game shuts off the view where it stops drawing. Past the lists the part behind is drawn
// and the caps stand across the corridors as blank walls. A cap: held by at least one list but under kCapShare of
// them, no thickness across x or z, and kCapMin or more up and along (as the XI Model Viewer finds them; real walls
// have thickness, Castle Zvahl Keep's are 0.76).
constexpr float kCapMin = 20.0f, kCapThin = 0.01f, kCapShare = 0.15f;

void find_caps(Twins& t, const std::vector<int>& held)
{
    int caps = 0;
    for (int i = 0; i < t.count; ++i) {
        const std::uintptr_t chunk = t.chunks + static_cast<std::uintptr_t>(i) * kChunkSize;
        if (held[i] == 0 || held[i] >= kCapShare * t.list_count ||
            *reinterpret_cast<const std::uintptr_t*>(chunk + kChunkMesh) == 0)
            continue;
        const Box box = chunk_box(chunk);
        const float x = box.hi[0] - box.lo[0], y = box.hi[1] - box.lo[1], z = box.hi[2] - box.lo[2];
        if (y < kCapMin || !((x < kCapThin && z >= kCapMin) || (z < kCapThin && x >= kCapMin)))
            continue;
        if (t.cap.empty())
            t.cap.resize(t.count);
        t.cap[i] = 1;
        ++caps;
    }
    if (caps > 0)
        log::write(L"Graphics: this zone has %d walls that only shut off the view where its lists stop; past the "
                   L"view lists, they aren't drawn ([graphics] drawdistance)",
                   caps);
}

// Backdrops. Mount Zhayolm's sky holds rock ribbons (back_3_m, back_4_m, back_4b_m, back_5_m) made to be seen from
// afar: a list draws the ones from about 135 out and leaves out those nearer, which up close float over the cliffs
// as loose slabs. Past the lists the quadtree drew those too; a piece whose mesh is named back_* is drawn only
// where the camera's list holds it.
constexpr char kBackdrop[] = "back_";
constexpr std::uintptr_t kMeshName = 8; // a mesh's name, 16 characters padded with spaces

void find_backdrops(Twins& t)
{
    int backdrops = 0;
    for (int i = 0; i < t.count; ++i) {
        const auto mesh = *reinterpret_cast<const std::uintptr_t*>(t.chunks + static_cast<std::uintptr_t>(i) * kChunkSize +
                                                                   kChunkMesh);
        if (mesh == 0 || std::strncmp(reinterpret_cast<const char*>(mesh + kMeshName), kBackdrop, sizeof kBackdrop - 1) != 0)
            continue;
        if (t.backdrop.empty())
            t.backdrop.resize(t.count);
        t.backdrop[i] = 1;
        ++backdrops;
    }
    if (backdrops > 0)
        log::write(L"Graphics: this zone has %d backdrop pieces (back_*); past the view lists, they're drawn only where "
                   L"the camera's list has them ([graphics] drawdistance)",
                   backdrops);
}

// The zone the player is in (the zone-in packet's until the party list has it)
std::uint16_t player_zone()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    return packets::player_zone(game::party(members, alliance) ? members[0].zone : 0);
}

// A zone held at the game's own scenery distance (drawdistance.large_off): the first slot's, with no view lists, and one of
// kLargeZones. Worked out as it loads; one without lists that isn't in kLargeZones is logged
bool drawn_whole(const Twins& t)
{
    if (&t != &g_twins[0] || t.count <= 1 || t.list_count > 1)
        return false;
    const std::uint16_t zone = player_zone();
    const bool large = std::find(std::begin(kLargeZones), std::end(kLargeZones), zone) != std::end(kLargeZones);
    if (!large)
        log::write(L"Graphics: zone %u has no view lists (%d pieces), drawn at your draw distance ([graphics] "
                   L"drawdistance.large_off holds only Al'Taieu's)",
                   static_cast<unsigned>(zone), t.count);
    return large;
}

// The far copies of the zone in `slot` (kZoneSlots), its lists checked first: anything that isn't the game's
// leaves the zone without (drawn as the quadtree has it).
void find_twins(Twins& t, std::uintptr_t slot)
{
    t = Twins{};
    t.count = *reinterpret_cast<const int*>(slot + kSlotChunkCount);
    t.chunks = *reinterpret_cast<const std::uintptr_t*>(slot + kSlotChunks);
    t.list_count = *reinterpret_cast<const int*>(slot + kSlotListCount);
    t.lists = *reinterpret_cast<const std::uintptr_t*>(slot + kSlotLists);
    const int count = t.count, lists = t.list_count;
    t.whole = drawn_whole(t);
    if (count <= 1 || t.chunks == 0 || lists <= 1 || lists > kMaxLists || t.lists == 0)
        return;
    const std::uintptr_t end = t.chunks + static_cast<std::uintptr_t>(count) * kChunkSize;
    const int words = (lists + 63) / 64;
    std::vector<std::uint64_t> in(static_cast<std::size_t>(count) * words); // chunk i is in list l
    std::vector<int> held(count);
    std::uintptr_t at = t.lists;
    for (int l = 0; l < lists; ++l) {
        const int n = *reinterpret_cast<const int*>(at);
        if (n < 0 || n > count)
            return;
        t.starts.push_back(at);
        const auto* chunk = reinterpret_cast<const std::uintptr_t*>(at + 4);
        for (int k = 0; k < n; ++k) {
            if (chunk[k] < t.chunks || chunk[k] >= end || (chunk[k] - t.chunks) % kChunkSize != 0)
                return;
            const auto i = static_cast<std::size_t>((chunk[k] - t.chunks) / kChunkSize);
            std::uint64_t& word = in[i * words + l / 64];
            const std::uint64_t bit = std::uint64_t{1} << (l % 64);
            held[i] += (word & bit) == 0;
            word |= bit;
        }
        at += 4 + static_cast<std::uintptr_t>(n) * 4;
    }
    find_caps(t, held);
    find_backdrops(t);
    // Pieces in a list and with a mesh, by the 64-unit squares (x, z) their boxes cover
    std::vector<Box> boxes(count);
    std::unordered_map<std::uint64_t, std::vector<int>> squares;
    const auto square = [](float v) { return static_cast<std::int32_t>(std::floor(v / 64.0f)); };
    for (int i = 0; i < count; ++i) {
        const std::uintptr_t chunk = t.chunks + static_cast<std::uintptr_t>(i) * kChunkSize;
        if (held[i] == 0 || *reinterpret_cast<const std::uintptr_t*>(chunk + kChunkMesh) == 0)
            continue;
        boxes[i] = chunk_box(chunk);
        const std::int32_t x0 = square(boxes[i].lo[0]), x1 = square(boxes[i].hi[0]);
        const std::int32_t z0 = square(boxes[i].lo[2]), z1 = square(boxes[i].hi[2]);
        if (x1 - x0 > 64 || z1 - z0 > 64) // the sky and such: nothing's twin
            continue;
        for (std::int32_t x = x0; x <= x1; ++x)
            for (std::int32_t z = z0; z <= z1; ++z)
                squares[(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z)]
                    .push_back(i);
    }
    std::vector<std::vector<int>> found(count);
    int pairs = 0;
    for (const auto& [key, in_square] : squares)
        for (std::size_t a = 0; a < in_square.size(); ++a)
            for (std::size_t b = a + 1; b < in_square.size(); ++b) {
                const int i = in_square[a], j = in_square[b];
                const auto mesh = [&](int c) {
                    return *reinterpret_cast<const std::uintptr_t*>(t.chunks + static_cast<std::uintptr_t>(c) * kChunkSize +
                                                                    kChunkMesh);
                };
                if (mesh(i) == mesh(j) || overlap(boxes[i], boxes[j]) < 0.5f)
                    continue;
                bool together = false;
                for (int w = 0; w < words && !together; ++w)
                    together = (in[static_cast<std::size_t>(i) * words + w] & in[static_cast<std::size_t>(j) * words + w]) != 0;
                if (together)
                    continue;
                bool known = false; // two boxes over the same squares meet in each
                for (const int k : found[i])
                    known |= k == j;
                if (known)
                    continue;
                found[i].push_back(j);
                found[j].push_back(i);
                ++pairs;
            }
    t.listed.resize(count); // the lists are good: what the camera's holds can be known
    if (pairs == 0)
        return;
    t.first.resize(count + 1);
    t.rank_far.resize(count);
    t.rank_lists = held;
    for (int i = 0; i < count; ++i) {
        t.first[i] = static_cast<int>(t.twins.size());
        t.twins.insert(t.twins.end(), found[i].begin(), found[i].end());
        t.rank_far[i] = *reinterpret_cast<const std::uint8_t*>(t.chunks + static_cast<std::uintptr_t>(i) * kChunkSize +
                                                               kChunkFar) != 0;
    }
    t.first[count] = static_cast<int>(t.twins.size());
    t.hidden.resize(count);
    log::write(L"Graphics: this zone has %d pieces drawn near and far as two copies; past the view lists, one of "
               L"each is drawn ([graphics] drawdistance)",
               pairs);
}

// Which of the twins draw from the camera's list `camera` (one of the zone's, or another zone's, or none)
void pick_twins(Twins& t, std::uintptr_t camera)
{
    t.camera = camera;
    t.camera_known = false;
    std::fill(t.listed.begin(), t.listed.end(), std::uint8_t{0});
    std::fill(t.hidden.begin(), t.hidden.end(), std::uint8_t{0});
    for (const std::uintptr_t start : t.starts) {
        if (start != camera || t.listed.empty())
            continue;
        t.camera_known = true;
        const int n = *reinterpret_cast<const int*>(start);
        const auto* chunk = reinterpret_cast<const std::uintptr_t*>(start + 4);
        for (int k = 0; k < n; ++k)
            t.listed[(chunk[k] - t.chunks) / kChunkSize] = 1;
    }
    if (t.hidden.empty())
        return;
    // Left out where a twin is listed (as the game would draw it) ...
    const int count = t.count;
    for (int i = 0; i < count; ++i)
        for (int k = t.first[i]; k < t.first[i + 1] && !t.listed[i]; ++k)
            if (t.listed[t.twins[k]]) {
                t.hidden[i] = 1;
                break;
            }
    // ... and with neither listed, the far copy drawn (the placement's flag, else the one more lists hold)
    const auto farther = [&](int a, int b) {
        if (t.rank_far[a] != t.rank_far[b])
            return t.rank_far[a] > t.rank_far[b];
        if (t.rank_lists[a] != t.rank_lists[b])
            return t.rank_lists[a] > t.rank_lists[b];
        return a < b;
    };
    for (int i = 0; i < count; ++i) {
        if (t.listed[i] || t.hidden[i] == 1)
            continue;
        for (int k = t.first[i]; k < t.first[i + 1]; ++k) {
            const int j = t.twins[k];
            if (!t.listed[j] && t.hidden[j] != 1 && farther(j, i)) {
                t.hidden[i] = 2;
                break;
            }
        }
    }
}

// Past the lists, after the list walk: a piece the list didn't draw drawn only if it's big (kFillMin) and big enough
// at its distance to make out
bool fill_hidden(const Twins& t, std::uintptr_t renderer, std::uintptr_t chunk, std::size_t i)
{
    if (!t.camera_known || t.listed[i])
        return true; // drawn by the list (or what it drew can't be known)
    const auto* eye = reinterpret_cast<const float*>(renderer + kCameraEye);
    const Box box = chunk_box(chunk);
    float near_sq = 0.0f, size_sq = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = eye[k] < box.lo[k] ? box.lo[k] - eye[k] : eye[k] > box.hi[k] ? eye[k] - box.hi[k] : 0.0f;
        near_sq += d * d;
        size_sq += (box.hi[k] - box.lo[k]) * (box.hi[k] - box.lo[k]);
    }
    return size_sq < kFillMin * kFillMin || size_sq < kFillSize * kFillSize * near_sq;
}

// Where the camera has no view list (none for its cell, or an empty one), the game walks its quadtree instead: every
// piece in view out to the draw distance, which at 5x is most of an open zone (Abyssea-Konschtat, 2026-10-23: 12,000
// draws a frame, 17 fps, where its lists had 2,300 and 60). The lists are made for the game's own distance, so there
// the quadtree is held to that (1x), or to where the fog ends when ours lies farther: nothing past it shows anyway.
// True leaves the piece undrawn.
bool far_hidden(Twins& t, std::uintptr_t renderer, std::uintptr_t chunk)
{
    const float mult = g_scenery.field != nullptr ? *g_scenery.field : 0.0f;
    const float drawn_sq = *reinterpret_cast<const float*>(renderer + kDrawDistanceSq);
    float cap_sq = mult > 0.0f && drawn_sq > 0.0f ? drawn_sq / (mult * mult) : 0.0f;
    if (g_settings.fog == Fog::Custom && g_settings.fog_end * g_settings.fog_end > cap_sq)
        cap_sq = g_settings.fog_end * g_settings.fog_end;
    if (cap_sq <= 0.0f)
        return false;
    if (!t.far_logged) {
        t.far_logged = true;
        log::write(L"Graphics: the camera is where this zone has no view list; the zone is drawn from its quadtree there, "
                   L"as far as %.0f ([graphics] drawdistance: the game's own distance, or the fog's end)",
                   std::sqrt(cap_sq));
    }
    const auto* eye = reinterpret_cast<const float*>(renderer + kCameraEye);
    const Box box = chunk_box(chunk);
    float near_sq = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = eye[k] < box.lo[k] ? box.lo[k] - eye[k] : eye[k] > box.hi[k] ? eye[k] - box.hi[k] : 0.0f;
        near_sq += d * d;
    }
    return near_sq > cap_sq;
}

// From the stub in the quadtree's chunk loop: true leaves the chunk undrawn
bool __stdcall twin_hidden(std::uintptr_t renderer, std::uintptr_t chunk)
{
    for (int s = 0; s < 2; ++s) {
        const std::uintptr_t slot = renderer + kZoneSlots + s * kZoneSlotSize;
        const std::uintptr_t chunks = *reinterpret_cast<const std::uintptr_t*>(slot + kSlotChunks);
        const int count = *reinterpret_cast<const int*>(slot + kSlotChunkCount);
        if (chunk < chunks || count <= 0 || chunk >= chunks + static_cast<std::uintptr_t>(count) * kChunkSize)
            continue;
        Twins& t = g_twins[s];
        if (t.chunks != chunks || t.count != count ||
            t.list_count != *reinterpret_cast<const int*>(slot + kSlotListCount) ||
            t.lists != *reinterpret_cast<const std::uintptr_t*>(slot + kSlotLists))
            find_twins(t, slot);
        g_whole_walked |= t.whole;
        const std::uintptr_t camera = *reinterpret_cast<const std::uintptr_t*>(renderer + kCameraList);
        if (camera != t.camera)
            pick_twins(t, camera);
        const auto i = static_cast<std::size_t>((chunk - chunks) / kChunkSize);
        if (!t.cap.empty() && t.cap[i] != 0)
            return true;
        if (!t.backdrop.empty() && t.backdrop[i] != 0 && t.camera_known && !t.listed[i])
            return true;
        if (!t.hidden.empty() && t.hidden[i] != 0)
            return true;
        // the zone's first slot is the one the game draws from the camera's list, when it has one; without one (or
        // with an empty one) it's walking the quadtree, held to the game's own reach unless drawdistance.full asks for
        // everything. Only for a zone that has view lists at all (find_twins' own bar): a ship's has none, the quadtree is
        // all it is ever drawn from, and holding that to the game's reach left the sea and the coast out of the boat ride
        const bool listed = camera != 0 && *reinterpret_cast<const int*>(camera) != 0;
        if (s == 0 && !listed && t.list_count > 1 && !g_list_skipped && far_hidden(t, renderer, chunk))
            return true;
        // far off and behind what was drawn, as the renderer's last tests found it ([graphics] occl)
        if (s == 0 && occlusion::hidden(renderer, chunks, count, i))
            return true;
        if (g_fill && s == 0 && listed && *reinterpret_cast<const int*>(slot + kSlotHasLists) != 0)
            return fill_hidden(t, renderer, chunk, i);
        return false;
    }
    return false;
}

// The quadtree's chunk test through twin_hidden (hook) or the game's (off). Found the first time it's wanted.
void hook_node_chunks(bool hook)
{
    if (!g_node_searched) {
        if (!hook)
            return;
        g_node_searched = true;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        const std::uintptr_t site = game != nullptr ? scan::find(game, kNodeChunks) : 0;
        g_node_stub = site != 0 ? static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                                                           PAGE_EXECUTE_READWRITE))
                                : nullptr;
        if (g_node_stub == nullptr) {
            log::write(L"Graphics: couldn't find where the game draws the zone's pieces from its quadtree; far and "
                       L"near copies may both show past the view lists ([graphics] drawdistance)");
            return;
        }
        const std::uintptr_t at = site + kNodeChunkTest;
        std::memcpy(g_node_game, reinterpret_cast<const void*>(at), kNodeChunkSize);
        const std::uintptr_t next = at + kNodeChunkSize + static_cast<std::int8_t>(g_node_game[5]); // the jbe's
        const auto stub = reinterpret_cast<std::uintptr_t>(g_node_stub);
        const auto rel = [](std::uintptr_t from_end, std::uintptr_t to) { return static_cast<std::int32_t>(to - from_end); };
        std::uint8_t* s = g_node_stub;
        // cmp byte [eax+8],1; jbe next; push eax; push ecx; push edx; push eax (chunk); push esi (renderer);
        // call twin_hidden; test al,al; pop edx; pop ecx; pop eax; jne next; jmp back
        const std::uint8_t code[] = {0x80, 0x78, 0x08, 0x01, 0x0F, 0x86, 0, 0, 0, 0, 0x50, 0x51, 0x52, 0x50, 0x56, 0xE8, 0, 0, 0, 0,
                                     0x84, 0xC0, 0x5A, 0x59, 0x58, 0x0F, 0x85, 0, 0, 0, 0, 0xE9, 0, 0, 0, 0};
        std::memcpy(s, code, sizeof code);
        const std::int32_t to_next = rel(stub + 10, next), to_call = rel(stub + 20, reinterpret_cast<std::uintptr_t>(&twin_hidden));
        const std::int32_t to_next2 = rel(stub + 31, next), to_back = rel(stub + 36, at + kNodeChunkSize);
        std::memcpy(s + 6, &to_next, 4);
        std::memcpy(s + 16, &to_call, 4);
        std::memcpy(s + 27, &to_next2, 4);
        std::memcpy(s + 32, &to_back, 4);
        FlushInstructionCache(GetCurrentProcess(), g_node_stub, 64);
        g_node_ours[0] = 0xE9;
        const std::int32_t to_stub = rel(at + 5, stub);
        std::memcpy(g_node_ours + 1, &to_stub, 4);
        g_node_ours[5] = 0x90;
        g_node_test = at;
        log::write(L"Graphics: the zone's quadtree draws its pieces at FFXiMain.dll+0x%X ([graphics] drawdistance)",
                   static_cast<unsigned>(site - reinterpret_cast<std::uintptr_t>(game)));
    }
    if (g_node_test == 0 || hook == g_node_hooked)
        return;
    if (!write_code(g_node_test, hook ? g_node_ours : g_node_game, kNodeChunkSize)) {
        g_node_test = 0;
        return;
    }
    g_node_hooked = hook;
}

// The list walk going on to the quadtree's (fill: what lies past the game's own draw distance, kListEnd) or to
// the end. Found the first time it's wanted; the quadtree's chunk test must be ours first (fill_hidden).
void fill_past_lists(bool fill)
{
    if (!g_end_searched) {
        if (!fill)
            return;
        g_end_searched = true;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        const std::uintptr_t site = game != nullptr ? scan::find(game, kListEnd) : 0;
        g_end_stub = site != 0 ? static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE,
                                                                          PAGE_EXECUTE_READWRITE))
                               : nullptr;
        if (g_end_stub == nullptr) {
            log::write(L"Graphics: couldn't find where the game ends the zone's view list; past it the far ground may "
                       L"be missing ([graphics] drawdistance)");
            return;
        }
        const std::uintptr_t at = site + kListEndJump;
        std::memcpy(g_end_game, reinterpret_cast<const void*>(at), kListEndSize);
        std::int32_t to_end = 0;
        std::memcpy(&to_end, g_end_game + 1, 4);
        const std::uintptr_t end = at + kListEndSize + to_end, quadtree = site + kQuadtreeStart;
        const auto stub = reinterpret_cast<std::uintptr_t>(g_end_stub);
        const auto rel = [](std::uintptr_t from_end, std::uintptr_t to) { return static_cast<std::int32_t>(to - from_end); };
        // mov esi,[esp+18h] (the slot's quadtree, as the walk had it); cmp dword [esi],0; je end; jmp quadtree
        const std::uint8_t code[] = {0x8B, 0x74, 0x24, 0x18, 0x83, 0x3E, 0x00, 0x0F, 0x84, 0, 0, 0, 0, 0xE9, 0, 0, 0, 0};
        std::memcpy(g_end_stub, code, sizeof code);
        const std::int32_t to_end2 = rel(stub + 13, end), to_quadtree = rel(stub + 18, quadtree);
        std::memcpy(g_end_stub + 9, &to_end2, 4);
        std::memcpy(g_end_stub + 14, &to_quadtree, 4);
        FlushInstructionCache(GetCurrentProcess(), g_end_stub, 32);
        g_end_ours[0] = 0xE9;
        const std::int32_t to_stub = rel(at + 5, stub);
        std::memcpy(g_end_ours + 1, &to_stub, 4);
        g_end_jump = at;
        log::write(L"Graphics: the zone's view list ends at FFXiMain.dll+0x%X ([graphics] drawdistance)",
                   static_cast<unsigned>(site - reinterpret_cast<std::uintptr_t>(game)));
    }
    if (g_end_jump == 0 || fill == g_end_hooked)
        return;
    if (!write_code(g_end_jump, fill ? g_end_ours : g_end_game, kListEndSize)) {
        g_end_jump = 0;
        return;
    }
    g_end_hooked = fill;
    log::write(fill ? L"Graphics: the zone drawn from the game's view lists, and the big pieces they leave out from its "
                      L"quadtree ([graphics] drawdistance.fill)"
                    : L"Graphics: the zone drawn from the game's view lists alone ([graphics] drawdistance.fill)");
}

// The zone drawn past the game's view lists (skip) or from them. Found the first time it's wanted: the
// game's je, or our jmp already there; anything else is someone else's patch, left alone.
void skip_view_lists(bool skip)
{
    if (!g_list_searched) {
        if (!skip)
            return;
        g_list_searched = true;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        const std::uintptr_t site = game != nullptr ? scan::find(game, kViewList) : 0;
        const auto* code = reinterpret_cast<const std::uint8_t*>(site + kViewListJump);
        std::int32_t to = 0; // where the je goes, from the end of its 6 bytes
        if (site != 0 && code[0] == 0x0F && code[1] == 0x84) {
            std::memcpy(&to, code + 2, sizeof to);
            std::memcpy(g_list_game, code, kViewListSize);
        } else if (site != 0 && code[0] == 0xE9 && code[5] == 0x90) {
            std::memcpy(&to, code + 1, sizeof to);
            to -= 1;
            const std::uint8_t je[2] = {0x0F, 0x84};
            std::memcpy(g_list_game, je, sizeof je);
            std::memcpy(g_list_game + 2, &to, sizeof to);
        }
        if (g_list_game[0] == 0) {
            log::write(L"Graphics: couldn't find where the game picks the zone's pieces to draw; past its own draw "
                       L"distance the ground may be missing ([graphics] drawdistance.full)");
            return;
        }
        g_list_jump = site + kViewListJump;
        const std::int32_t jmp = to + 1; // the same place, from the end of a 5-byte jmp
        g_list_ours[0] = 0xE9;
        std::memcpy(g_list_ours + 1, &jmp, sizeof jmp);
        g_list_ours[5] = 0x90;
        g_list_skipped = code[0] == 0xE9;
        log::write(L"Graphics: the zone's view lists are at FFXiMain.dll+0x%X ([graphics] drawdistance.full)",
                   static_cast<unsigned>(site - reinterpret_cast<std::uintptr_t>(game)));
    }
    if (g_list_jump == 0 || skip == g_list_skipped)
        return;
    if (!write_code(g_list_jump, skip ? g_list_ours : g_list_game, kViewListSize)) {
        log::write(L"Graphics: couldn't change how the game picks the zone's pieces to draw ([graphics] drawdistance.full)");
        g_list_jump = 0;
        return;
    }
    g_list_skipped = skip;
    log::write(skip ? L"Graphics: the zone drawn as far as the draw distance reaches, past the game's view lists "
                      L"([graphics] drawdistance.full, drawdistance above 1)"
                    : L"Graphics: the zone drawn from the game's view lists ([graphics] drawdistance.full)");
}

void log_changes(const Settings& now)
{
    if (now.scenery != g_logged.scenery || now.mobs != g_logged.mobs) {
        wchar_t scenery[32] = L"the game's", mobs[32] = L"the game's";
        if (now.scenery > 0)
            swprintf_s(scenery, L"%g times", now.scenery);
        if (now.mobs > 0)
            swprintf_s(mobs, L"%g times", now.mobs);
        log::write(L"Graphics: draw distance: the zone %ls, characters %ls ([graphics] drawdistance, "
                   L"drawdistance.mobs; the game's own is %g, %g)",
                   scenery, mobs, g_scenery.game, g_mobs.game);
    }
    if (now.fog != g_logged.fog || (now.fog == Fog::Custom && (now.fog_start != g_logged.fog_start ||
                                                                now.fog_end != g_logged.fog_end)))
        log::write(now.fog == Fog::Game     ? L"Graphics: fog where the game's own draw distance puts it ([graphics] fog=game)"
                   : now.fog == Fog::Custom ? L"Graphics: fog from %g to %g ([graphics] fog=custom)"
                                            : L"Graphics: fog as the game sets it for the draw distance ([graphics] fog=distance)",
                   now.fog_start, now.fog_end);
    g_logged = now;
}

// The zone's multiplier for this frame: the game's own (0) while one of kLargeZones is drawn, else
// drawdistance's. The game works its fog out from the same multiplier; fog() puts drawdistance's back while it does, so the
// fog stays where the distance asked for puts it. Known from twin_hidden: the quadtree's walk draws such a zone each frame,
// its hook staying in while held; held until the walk hasn't come by for kWholeLinger (zoned out, or the hook taken out)
float zone_multiplier(const Settings& settings)
{
    const ULONGLONG now = GetTickCount64();
    if (g_whole_walked) {
        g_whole_walked = false;
        g_whole_at = now;
    }
    const bool held = settings.large_off && settings.scenery > 1.0f && g_whole_at != 0 && now - g_whole_at < kWholeLinger;
    if (held != g_whole_held) {
        g_whole_held = held;
        if (held)
            log::write(L"Graphics: this zone has no view lists and %d pieces, so the game draws all of it in view: the zone "
                       L"at the game's own draw distance, the characters at yours ([graphics] drawdistance.large_off)",
                       g_twins[0].count);
        else
            log::write(L"Graphics: the zone at your draw distance again ([graphics] drawdistance.large_off)");
    }
    return held ? 0.0f : settings.scenery;
}

} // namespace

void apply(const Settings& settings)
{
    g_settings = settings;
    occlusion::apply(settings.occl);
    const bool wanted = settings.scenery > 0 || settings.mobs > 0 || settings.fog != Fog::Distance;
    // VanaView is the Ashita plugin that did this; held off while VanaCore sets the draw distance and fog, so the two
    // don't fight. It loads again if you set draw distance and fog back to the game's.
    if (wanted)
        plugins::hold("vanaview", "VanaCore sets the draw distance and fog itself (/vana > Client > Game); it loads "
                                  "again if you set those back to the game's");
    else
        plugins::release("vanaview");
    if (!g_searched) {
        if (!wanted)
            return;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return;
        g_searched = true;
        if (const std::uintptr_t site = scan::find(game, kMultipliers)) {
            g_scenery.field = *reinterpret_cast<float* const*>(site + kSceneryAt);
            g_mobs.field = *reinterpret_cast<float* const*>(site + kMobsAt);
            const auto* config = reinterpret_cast<const std::uint8_t*>(site + kConfigAt);
            if (config[0] == 0x8B && config[1] == 0x0D) // mov ecx,[config]
                g_config = *reinterpret_cast<std::uintptr_t* const*>(config + 2);
            log::write(L"Graphics: the game's draw distance is at FFXiMain.dll+0x%X ([graphics] drawdistance)",
                       static_cast<unsigned>(site - reinterpret_cast<std::uintptr_t>(game)));
        } else {
            log::write(L"Graphics: couldn't find the game's draw distance; it stays the game's own ([graphics] "
                       L"drawdistance)");
        }
    }
    if (g_scenery.field != nullptr) {
        keep(g_scenery, zone_multiplier(settings));
        keep(g_mobs, settings.mobs);
    }
    // Characters drawn farther than the game's own distance: targeted only as far as it (drawdistance.target=game)
    if (!settings.target_drawn && settings.mobs > 0 && g_mobs.field != nullptr && !g_target_searched)
        hook_targets();
    // Past the game's own distances (1x): with drawdistance.full, past its view lists (every piece in view); else the
    // lists, with drawdistance.fill the big pieces they leave out from the quadtree too, so the far ground isn't missing
    const bool past = settings.scenery > 1.0f, past_lists = settings.full && past;
    g_fill = past && !past_lists && settings.fill;
    skip_view_lists(past_lists);
    hook_node_chunks(past);
    fill_past_lists(g_fill && g_node_hooked);
    // The fog's detour goes in the first time it's wanted (fog=game or custom, or a zone held by drawdistance.large_off),
    // and stays (fog=distance passes straight through)
    if ((settings.fog != Fog::Distance || g_whole_held) && !g_fog_searched) {
        g_fog_searched = true;
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        const std::uintptr_t function = game != nullptr ? scan::find(game, kFog) : 0;
        if (function != 0 &&
            imports::detour(reinterpret_cast<FogFunction>(function), kFogPrologue, &fog, g_fog))
            log::write(L"Graphics: the game's fog is worked out at FFXiMain.dll+0x%X ([graphics] fog)",
                       static_cast<unsigned>(function - reinterpret_cast<std::uintptr_t>(game)));
        else
            log::write(L"Graphics: couldn't find the game's fog; it stays as the game sets it ([graphics] fog)");
    }
    if (settings.scenery != g_logged.scenery || settings.mobs != g_logged.mobs || settings.fog != g_logged.fog ||
        settings.fog_start != g_logged.fog_start || settings.fog_end != g_logged.fog_end)
        log_changes(settings);
}

} // namespace vanacore::distance
