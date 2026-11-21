#include "casters.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::casters {
namespace {

// Why: the sun's shadow maps are drawn from what the game drew this frame, and the game draws only what its camera sees. A building
// beside or behind the camera, whose shadow falls into view, cast nothing until it had been in view once and the shadow cache kept
// it: over Port Jeuno (2026-11-02), moving the camera up 3.5 units made the game draw a building it had left out (7 pieces at
// (-48, 0, 132)), and its whole shadow came onto the plaza, whatever the shadow settings. Not the view lists: with them off
// (drawdistance.full) the building was still left out, by the camera's own view test below.
//
// So the pass draws every piece of the zone within the shadows' reach of the player, round after round, whatever the camera sees -
// not only what the game left out this frame. What the game drew, it drew for the camera: at the detail the piece's distance from
// the camera picks (a wall's low mesh has no railings), and only while the camera's cell has it in its view list, so the shadow
// cache let a piece in plain view go the frame the list dropped it, and the pass sought it again a few frames on: 50 to 130 of them
// a frame with the camera over Port Jeuno's archway, the plaza's shadows shimmering with it (2026-11-02 16:32). The pass measures
// from the player: the renderer's eye is put there while it draws (the detail and the draw distance), and a tall piece is taken
// from farther, as far as its shadow reaches (its height times the light's stretch), so the walls far behind the camera cast onto
// the plaza whether or not the game drew them this frame.

// The game's test of a piece of the zone against the camera's view (FUN_10180670: a thiscall that ignores ecx and pops 8): the
// view-projection and the 8 corners of the piece's box; nonzero when the box is wholly outside one side of the view. Both of the
// game's piece renderers call it once a piece is within the draw distance; the quadtree calls it for its nodes, the UAS meshes for
// theirs. (FUN_10180B30 starts the same; it stores its second corner's y with fst, not fstp.)
//   sub esp,60h; push ebx; push esi; mov esi,[esp+6Ch]; push edi; mov edi,[esp+74h]; xor ecx,ecx; the first corner's x and y
constexpr char kViewTest[] = "83 EC 60 53 56 8B 74 24 6C 57 8B 7C 24 74 33 C9 D9 06 D8 0F D9 46 20 D8 4F 08 DE C1 D9 47 04 D8 4E 10 "
                             "DE C1 D8 46 30 D9 5C 24 0C D9 47 04 D8 4E 14 D9 46 04 D8 0F DE C1 D9 46 24 D8 4F 08 DE C1 D8 46 34 "
                             "D9 5C 24 10";
constexpr std::size_t kViewTestPrologue = 5; // sub esp,60h; push ebx; push esi

// The end of a zone slot's draw (in FUN_10179FD0, the zone's draw, after its walk of the slot's pieces): the pieces held back to
// draw last (a piece's +0x0F) drawn through the renderer the game uses (renderer+0x3949C 2: RenderChunk2 0x1017C8E0, else
// RenderChunk1 0x1017B480; thiscalls: the slot's lights, the piece, 1 for a slot whose pieces move with a matrix of their own;
// they pop 12), then the slot's draw states put back. The pass goes in where those start (kSlotEndPatch: cmp dword
// [ebp+3949Ch],1), ebp the renderer, [esp+18h] the slot walked (its +0x38), [esp+1Ch] its lights:
//   mov eax,[ebp+3949Ch]; xor edi,edi; cmp eax,2; mov eax,[ebp+38D40h]; lea esi,[ebp+37540h]; jne; ... call RenderChunk2; ...
//   call RenderChunk1; ...; cmp dword [ebp+3949Ch],1
constexpr char kSlotEnd[] = "8B 85 9C 94 03 00 33 FF 83 F8 02 8B 85 40 8D 03 00 8D B5 40 75 03 00 75 26 3B C3 7E 46 8B 56 08 8B 06 "
                            "8B 4E 04 52 50 51 8B CD E8 ?? ?? ?? ?? 8B 85 40 8D 03 00 47 83 C6 0C 3B F8 7C E0 EB 24 3B C3 7E 20 "
                            "8B 56 08 8B 06 8B 4E 04 52 50 51 8B CD E8 ?? ?? ?? ?? 8B 85 40 8D 03 00 47 83 C6 0C 3B F8 7C E0 83 BD "
                            "9C 94 03 00 01";
constexpr std::uintptr_t kChunk2Call = 0x2A, kChunk1Call = 0x50, kSlotEndPatch = 0x63;
constexpr std::size_t kSlotEndSize = 7; // cmp dword [ebp+3949Ch],1

// The zone's other draw (FUN_101798A0, after the walk): the slot's UAS lists (slot+0x3C, slot+0x40 of them, 0x1AC bytes each:
// up to 255 piece pointers at +0x198, the count at +0x1A9), each piece drawn through DrawUASSubchunk (0x10182860, a thiscall:
// the slot's lights, the piece; pops 8) - the same distance and view tests, always the high mesh. The walk leaves pieces of
// kind 0 and 1 (+0x08) to these lists (Port Jeuno's archways and 47 others of its 651), so the pass draws those through it too.
//   mov eax,[ebp+40h]; mov ebx,[ebp+3Ch]; cmp eax,edi; jle; add ebx,1A9h; mov al,[ebx]; xor esi,esi; test al,al; jbe;
//   lea edi,[ebx-11h]; mov edx,[edi]; mov eax,[ebp+64h]; mov ecx,[esp+10h]; push edx; push eax; call DrawUASSubchunk
constexpr char kUasLoop[] = "8B 45 40 8B 5D 3C 3B C7 7E ?? 81 C3 A9 01 00 00 8A 03 33 F6 84 C0 76 ?? 8D 7B EF 8B 17 8B 45 64 8B 4C "
                            "24 10 52 50 E8";
constexpr std::uintptr_t kUasCall = 0x26;

// The zone renderer (DAT_105FD284): its two zone slots (FUN_10177EF0), the walk's pointer being the slot's +0x38; a slot's kind
// (3 with renderer+0x158 set: pieces through a matrix of their own, a moving zone - left alone), its pieces; which renderer it
// draws them with; the camera's eye, which all three renderers measure a piece's distance from (fsub [renderer+390F0h]: the
// mesh its detail picks, the draw distance) - the pass puts the player there while it draws, and measures from there when the
// player isn't known
constexpr std::uintptr_t kZoneSlots = 0x60, kZoneSlotSize = 0x78, kSlotWalked = 0x38;
constexpr std::uintptr_t kSlotKind = 0x00, kSlotChunkCount = 0x24, kSlotChunks = 0x28, kMoving = 0x158;
constexpr std::uintptr_t kRenderMode = 0x3949C, kCameraEye = 0x390F0;
// A piece (PositionedMeshBlock): how the game draws it (+0x08: 0 and 1 through the UAS lists, by their high mesh; 2 by the walk),
// its low, medium and high meshes (+0x10, +0x14, +0x18: the renderers pick by the eye's distance, past the piece's +0xCC, past
// +0xC8, nearer) and the world corners of its box. Its placement's far flag (+0x0A, distance.cpp's twins: 2 on a far copy) keeps
// nothing out: the game draws those in view at any distance (221 of Port Jeuno's 651, the walls far behind the camera among
// them), and while the pass left them to the game their shadows came and went with the camera's turn (2026-11-02 18:40)
constexpr std::uintptr_t kChunkSize = 0x16C, kChunkType = 0x08, kChunkMeshes = 0x10, kChunkHigh = 0x18, kChunkCorners = 0xF4;

// How many pieces a slot's walk draws for the shadows: its round's (every piece within reach) share of kRoundWalks, within kLeast
// and kMost. Each draw is kept by its piece (vanacore_gfx_seek's id: the piece's address), once - the sun's maps draw what's kept,
// every frame, and a piece drawn again only brings its own up to date (gfx_sun_cache.h piece_see) - so a piece needn't be drawn
// every frame. A round over 20 walks once left the maps to what the game drew for its camera and to the cache matching the rest up
// by mesh and place (Port Jeuno, 2026-11-02 21:29: whole shadows gone for a quarter-second), and drawn every walk the pass cost
// 4-5 ms of the CPU a frame, and the cache's matching kept a swaying tree again and again (The Sanctuary of Zi'Tah, 2026-11-03:
// 27,000-42,000 kept for some 1,700 of the pass's draws, the frame 20 ms). Kept by piece there's nothing to match: a round over 8
// walks, each piece brought up to date 7 or 8 times a second
constexpr int kRoundWalks = 8, kLeast = 48, kMost = 4000;

using ViewTest = int(__stdcall*)(const float* view, const float* corners);
// the game's thiscalls, called as fastcalls (this in ecx; edx unused; the callee pops the rest)
using RenderChunk = void(__fastcall*)(std::uintptr_t renderer, void* edx, std::uintptr_t lights, std::uintptr_t chunk, int moving);
using RenderUas = void(__fastcall*)(std::uintptr_t renderer, void* edx, std::uintptr_t lights, std::uintptr_t chunk);

// One zone slot's pieces as last seen, and where the pass is in its round of them
struct Slot {
    std::uintptr_t chunks = 0;
    int count = 0;
    int cursor = 0;           // the next piece the pass looks at
    int found = 0, round = 0; // pieces it drew this round, and the last whole round
    bool logged = false;
    std::vector<std::uint8_t> state;  // the flicker watch's (drawable_note): each piece drawable when last looked at, 1; not, 2
    std::vector<std::uint32_t> since; // ... the frame it last changed
};

struct Box {
    float lo[3], hi[3];
};

// Only touched on the game's thread.
bool g_searched = false;
ViewTest g_view_test = nullptr;   // the game's, through the detour's trampoline
RenderChunk g_render[2] = {};     // RenderChunk1, RenderChunk2
RenderUas g_render_uas = nullptr; // DrawUASSubchunk, for pieces of kind 0 and 1
std::uint8_t* g_stub = nullptr;
bool g_seeking = false;           // the pass is drawing: every piece passes the view test
float g_focus[3] = {};
bool g_focus_known = false;
bool g_faulted = false;           // the game's renderer faulted under the pass: it's off
Slot g_slots[2];
int g_calls[2] = {};              // the slot ends the pass ran at since update (a frame): more than one is said, once (seek)
bool g_calls_said = false;
std::uint32_t g_frame = 0;        // update's count: the frames, for the flicker watch's notes

template <typename T> T read(std::uintptr_t at) { return *reinterpret_cast<const T*>(at); }

Slot& slot_for(int s, std::uintptr_t chunks, int count)
{
    Slot& sl = g_slots[s];
    if (sl.chunks != chunks || sl.count != count) {
        sl = Slot{};
        sl.chunks = chunks, sl.count = count;
    }
    return sl;
}

int __stdcall view_test(const float* view, const float* corners)
{
    return g_seeking ? 0 : g_view_test(view, corners); // the pass's piece: in view
}

Box box_of(std::uintptr_t chunk)
{
    const auto* corner = reinterpret_cast<const float*>(chunk + kChunkCorners);
    Box b = {{corner[0], corner[1], corner[2]}, {corner[0], corner[1], corner[2]}};
    for (int i = 1; i < 8; ++i)
        for (int k = 0; k < 3; ++k) {
            b.lo[k] = std::min(b.lo[k], corner[i * 3 + k]);
            b.hi[k] = std::max(b.hi[k], corner[i * 3 + k]);
        }
    return b;
}

// the square of how far `at` is from the box
float box_distance_sq(const Box& b, const float* at)
{
    float sq = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = at[k] < b.lo[k] ? b.lo[k] - at[k] : at[k] > b.hi[k] ? at[k] - b.hi[k] : 0.0f;
        sq += d * d;
    }
    return sq;
}

// how far the box's top or bottom stands from `at`'s height: its shadow reaches that far past it along the ground for each unit of
// the light's stretch (the world's up is y, one way or the other: both taken)
float box_rise(const Box& b, const float* at) { return std::max(std::fabs(b.lo[1] - at[1]), std::fabs(b.hi[1] - at[1])); }

// the game draws piece i of the slot: it has a mesh (kinds 0 and 1 their high one, through the UAS lists); a far copy too
bool drawable(std::uintptr_t chunk)
{
    if (read<std::uint8_t>(chunk + kChunkType) <= 1)
        return g_render_uas != nullptr && read<std::uintptr_t>(chunk + kChunkHigh) != 0;
    const auto* meshes = reinterpret_cast<const std::uintptr_t*>(chunk + kChunkMeshes);
    return meshes[0] != 0 || meshes[1] != 0 || meshes[2] != 0;
}

// The flicker watch: a piece within 100 of `at` that the game stopped having meshes for (drawable no more: the pass can't draw it,
// and what the shadows kept of it goes with the buffers freed under it), or has them again, with how long it was the other way.
// Ru'Lude Gardens (2026-11-04): 38 of the shadow cache's draws gone 256 frames in every 432, though none of the pass's draws went
// missing or were refused and none expired. A dozen lines every 4 s at most, the rest counted
void drawable_note(Slot& sl, int i, std::uintptr_t chunk, bool now, const float* at)
{
    if (sl.state.size() != static_cast<std::size_t>(sl.count))
        sl.state.assign(static_cast<std::size_t>(sl.count), 0), sl.since.assign(static_cast<std::size_t>(sl.count), 0);
    const std::uint8_t want = now ? 1 : 2, was = sl.state[i];
    if (was == want)
        return;
    const std::uint32_t frames = g_frame - sl.since[i];
    sl.state[i] = want, sl.since[i] = g_frame;
    const Box b = box_of(chunk);
    if (was == 0 || box_distance_sq(b, at) > 100.0f * 100.0f)
        return;
    static std::uint32_t window = 0, said = 0, held = 0;
    if (g_frame - window >= 480) {
        if (held)
            log::write(L"Graphics: the caster pass: ... and %u more pieces' meshes came or went", held);
        window = g_frame, said = 0, held = 0;
    }
    if (said++ >= 12) {
        ++held;
        return;
    }
    const auto* m = reinterpret_cast<const std::uintptr_t*>(chunk + kChunkMeshes);
    log::write(L"Graphics: the caster pass: piece %08X of the zone (kind %u, at %.0f %.0f %.0f) %ls after %u frames (meshes low "
               L"%08X, medium %08X, high %08X)",
               static_cast<unsigned>(chunk), static_cast<unsigned>(read<std::uint8_t>(chunk + kChunkType)),
               0.5f * (b.lo[0] + b.hi[0]), 0.5f * (b.lo[1] + b.hi[1]), 0.5f * (b.lo[2] + b.hi[2]),
               now ? L"has its meshes again" : L"has no mesh to draw", frames, static_cast<unsigned>(m[0]),
               static_cast<unsigned>(m[1]), static_cast<unsigned>(m[2]));
}

// the game's renderer on one piece (the UAS lists' for kinds 0 and 1); false if it faulted
bool draw_piece(RenderChunk render, std::uintptr_t renderer, std::uintptr_t lights, std::uintptr_t chunk)
{
    __try {
        if (read<std::uint8_t>(chunk + kChunkType) <= 1)
            g_render_uas(renderer, nullptr, lights, chunk);
        else
            render(renderer, nullptr, lights, chunk, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The slot's pieces within `reach` of `at` (a tall one from farther: its rise times `stretch` past that), from where the round got
// to: drawn, up to the budget, each at its highest detail - the renderer picks a piece's mesh by its distance from the eye, so the
// eye goes into the piece's box (as near as the player ever gets) while it has a high mesh, else stays at `at`, the mesh that
// distance picks being all it has. A wall seen at the detail the camera's distance picked lost its railings as the camera moved off
int seek_slot(Slot& sl, std::uintptr_t renderer, std::uintptr_t lights, const float* at, float reach, float stretch)
{
    const RenderChunk render = g_render[read<int>(renderer + kRenderMode) == 2 ? 1 : 0];
    // (the first round in one walk: a zone's shadows whole from its first frame, not over half a second)
    const int budget = sl.round > 0 ? std::clamp(sl.round / kRoundWalks, kLeast, kMost) : kMost;
    auto* eye = reinterpret_cast<float*>(renderer + kCameraEye);
    float camera[3];
    std::memcpy(camera, eye, sizeof camera);
    int drawn = 0;
    g_seeking = true;
    for (int k = 0; k < sl.count && drawn < budget && !g_faulted; ++k) {
        const int i = sl.cursor;
        if (++sl.cursor == sl.count)
            sl.cursor = 0, sl.round = sl.found, sl.found = 0;
        const std::uintptr_t chunk = sl.chunks + static_cast<std::uintptr_t>(i) * kChunkSize;
        const bool can = drawable(chunk);
        drawable_note(sl, i, chunk, can, at);
        if (!can)
            continue;
        const Box box = box_of(chunk);
        const float limit = reach + box_rise(box, at) * stretch;
        if (box_distance_sq(box, at) > limit * limit)
            continue;
        ++sl.found, ++drawn;
        const bool high = read<std::uintptr_t>(chunk + kChunkHigh) != 0;
        for (int j = 0; j < 3; ++j)
            eye[j] = high ? 0.5f * (box.lo[j] + box.hi[j]) : at[j];
        vanacore_gfx_seek(static_cast<std::uint32_t>(chunk)); // (the piece's draws kept by it: its address, 32-bit)
        g_faulted = !draw_piece(render, renderer, lights, chunk);
    }
    g_seeking = false;
    vanacore_gfx_seek(0);
    std::memcpy(eye, camera, sizeof camera);
    if (g_faulted)
        log::write(L"Graphics: the game's renderer faulted drawing a piece of the zone round you for the sun's shadows; that's off "
                   L"until VanaCore starts again ([graphics.effects] sun_seek)");
    return drawn;
}

// From the stub at the end of a slot's draw: the renderer, the slot walked (its +0x38) and its lights
void __stdcall seek(std::uintptr_t renderer, std::uintptr_t walked, std::uintptr_t lights)
{
    const std::uintptr_t slot = walked - kSlotWalked;
    const int s = slot == renderer + kZoneSlots ? 0 : slot == renderer + kZoneSlots + kZoneSlotSize ? 1 : -1;
    const auto chunks = s >= 0 ? read<std::uintptr_t>(slot + kSlotChunks) : 0;
    const int count = s >= 0 ? read<int>(slot + kSlotChunkCount) : 0;
    if (chunks == 0 || count <= 0)
        return;
    Slot& sl = slot_for(s, chunks, count);
    ++g_calls[s];
    float stretch = 0.0f;
    const float reach = g_faulted ? 0.0f : vanacore_gfx_seek_reach(&stretch);
    if (reach <= 0.0f || (read<int>(slot + kSlotKind) == 3 && read<int>(renderer + kMoving) != 0))
        return;
    const float* at = g_focus_known ? g_focus : reinterpret_cast<const float*>(renderer + kCameraEye);
    const int drawn = seek_slot(sl, renderer, lights, at, reach, stretch);
    if (!sl.logged && sl.round > 0) {
        sl.logged = true;
        log::write(L"Graphics: the sun's shadows take the zone's pieces round you whatever the camera sees: %d of slot %d's %d "
                   L"within %.0f of you (the tall ones from farther), %d a frame ([graphics.effects] sun_seek)",
                   sl.round, s, count, reach, drawn);
    }
}

// The stub at the slot's end: pushad; pushfd; push [esp+40h] (lights); push [esp+40h] (the slot walked); push ebp; call seek;
// popfd; popad; the game's cmp; jmp back
bool hook_slot_end(std::uintptr_t site)
{
    const std::uintptr_t at = site + kSlotEndPatch;
    g_stub = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub == nullptr)
        return false;
    const auto stub = reinterpret_cast<std::uintptr_t>(g_stub);
    const auto rel = [](std::uintptr_t from_end, std::uintptr_t to) { return static_cast<std::int32_t>(to - from_end); };
    const std::uint8_t code[] = {0x60, 0x9C, 0x8B, 0x44, 0x24, 0x40, 0x50, 0x8B, 0x44, 0x24, 0x40, 0x50, 0x55, 0xE8, 0, 0, 0, 0,
                                 0x9D, 0x61};
    std::memcpy(g_stub, code, sizeof code);
    const std::int32_t to_seek = rel(stub + 18, reinterpret_cast<std::uintptr_t>(&seek));
    std::memcpy(g_stub + 14, &to_seek, 4);
    std::memcpy(g_stub + 20, reinterpret_cast<const void*>(at), kSlotEndSize);
    g_stub[27] = 0xE9;
    const std::int32_t to_back = rel(stub + 32, at + kSlotEndSize);
    std::memcpy(g_stub + 28, &to_back, 4);
    FlushInstructionCache(GetCurrentProcess(), g_stub, 64);
    std::uint8_t ours[kSlotEndSize] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    const std::int32_t to_stub = rel(at + 5, stub);
    std::memcpy(ours + 1, &to_stub, 4);
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), kSlotEndSize, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), ours, kSlotEndSize);
    VirtualProtect(reinterpret_cast<void*>(at), kSlotEndSize, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), kSlotEndSize);
    return true;
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

void install()
{
    g_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t test = game != nullptr ? scan::find(game, kViewTest) : 0;
    const std::uintptr_t end = game != nullptr ? scan::find(game, kSlotEnd) : 0;
    if (test == 0 || end == 0) {
        log::write(L"Graphics: couldn't find where the game draws the zone's pieces; the sun's shadows take only what the camera "
                   L"sees ([graphics.effects] sun_seek)");
        return;
    }
    g_render[0] = reinterpret_cast<RenderChunk>(call_target(end + kChunk1Call));
    g_render[1] = reinterpret_cast<RenderChunk>(call_target(end + kChunk2Call));
    const std::uintptr_t uas = scan::find(game, kUasLoop);
    g_render_uas = uas != 0 ? reinterpret_cast<RenderUas>(call_target(uas + kUasCall)) : nullptr;
    if (uas == 0)
        log::write(L"Graphics: couldn't find where the game draws the zone's UAS pieces; out of the camera's view they cast no "
                   L"shadow ([graphics.effects] sun_seek)");
    if (!imports::detour(reinterpret_cast<ViewTest>(test), kViewTestPrologue, &view_test, g_view_test)) {
        log::write(L"Graphics: couldn't hook the game's view test; the sun's shadows take only what the camera sees "
                   L"([graphics.effects] sun_seek)");
        return;
    }
    if (!hook_slot_end(end)) {
        log::write(L"Graphics: couldn't hook the end of the zone's draw; the sun's shadows take only what the camera sees "
                   L"([graphics.effects] sun_seek)");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    log::write(L"Graphics: the zone's pieces round you drawn for the sun's shadows whatever the camera sees: the view test at "
               L"FFXiMain.dll+0x%X, the slot's end at +0x%X ([graphics.effects] sun_seek)",
               static_cast<unsigned>(test - base), static_cast<unsigned>(end + kSlotEndPatch - base));
}

} // namespace

void update(float x, float y, float z, bool known)
{
    g_focus[0] = x, g_focus[1] = y, g_focus[2] = z;
    g_focus_known = known;
    ++g_frame;
    // (a slot drawn twice a frame - another view of the zone - runs the pass twice, the second round's draws through that view)
    for (int s = 0; s < 2; ++s) {
        if (g_calls[s] > 1 && !g_calls_said) {
            g_calls_said = true;
            log::write(L"Graphics: the zone's slot %d was drawn %d times in a frame: the sun's caster pass ran at each "
                       L"([graphics.effects] sun_seek)",
                       s, g_calls[s]);
        }
        g_calls[s] = 0;
    }
    if (!g_searched && vanacore_gfx_seek_reach(nullptr) > 0.0f)
        install();
}

} // namespace vanacore::casters
