#include "gearload.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

#include "dats.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::gearload {
namespace {

// A gear part's DAT has loaded: FUN_100d0bd0, __cdecl (handle, request), the callback XiSkeletonActor::SetUp (FUN_100d2cb0)
// gives the game's loader for each slot of a PC-look actor that isn't loaded yet (the PS2 client's _SetupPartsCallBack).
// The request (0x20 bytes, freed by the callback): +0 the actor's setup stamp (actor+0x9EC when it was asked for), +4 a
// weak link to the entity (+8 its target index, +0xC its server id), +0x10 a sub-actor (0: the entity's own actor),
// +0x18 the slot's model (slot << 12 | id), +0x1C the DAT's file id.
//
// The game gives up on the part, with no retry, when:
// - the weak link finds no actor: the entity's flag 0x200 is clear (or the entity is gone): the part is dropped;
// - the load isn't complete (FUN_10070e70): the part goes to the actor (FUN_100d08a0) as not loaded, which marks the slot
//   done (actor+0x8B0) with nothing in it.
// Either way the slot stays empty until the actor's look is set up again (a gear change, out of range and back, a zone).
//
// push ebx; mov ebx,[esp+0Ch]; push esi; mov eax,[ebx+10h]; test eax,eax; jnz; lea ecx,[ebx+4]; call actor; mov esi,eax;
// test esi,esi; jz; lea ecx,[esp+0Ch]; call loaded; mov ecx,[ebx+18h]; mov edx,[esp+0Ch]; push eax
constexpr char kPartLoaded[] = "53 8B 5C 24 0C 56 8B 43 10 85 C0 75 2D 8D 4B 04 E8 ?? ?? ?? ?? 8B F0 85 F6 0F 84 ?? ?? ?? ?? "
                               "8D 4C 24 0C E8 ?? ?? ?? ?? 8B 4B 18 8B 54 24 0C 50";
constexpr std::size_t kPrologue = 5;         // push ebx; mov ebx,[esp+0Ch]
constexpr std::ptrdiff_t kActorCall = 0x10;  // __thiscall (weak link): the entity's actor, or 0
constexpr std::ptrdiff_t kLoadedCall = 0x23; // __fastcall (handle*): loaded, with nothing of it pending
// The weak link's actor (FUN_100814d0): mov eax,[ecx+4]; sub esp,0Ch; mov eax,[eax*4+entities]
constexpr std::ptrdiff_t kEntitiesAt = 9;
// The loaded test (FUN_10070e70): push esi; mov esi,ecx; mov ecx,[handles]; mov eax,[esi]; push eax; call valid; ...
// mov ecx,eax; call pending
constexpr std::ptrdiff_t kHandlesAt = 5;
constexpr std::ptrdiff_t kValidCall = 0x0C;   // __thiscall (handles, handle): a live handle
constexpr std::ptrdiff_t kPendingCall = 0x2D; // __thiscall (resource): how many of its parts are still pending (textures)

// The largest free block of one of the game's heaps (FUN_10028f60, __cdecl (heap)): mov eax,[esp+4]; cmp eax,6; ja default;
// jmp [eax*4+cases]; ... The default case (heap 4, the resources' one: what's loaded from DATs is copied there) is
// mov eax,[last]; mov ecx,[first]; push eax; push ecx; call largest
constexpr char kHeapLargest[] = "8B 44 24 04 83 F8 06 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 51 E8";
constexpr std::ptrdiff_t kDefaultJump = 7; // ja rel32

// Entity fields
constexpr std::uintptr_t kServerId = 0x78, kName = 0x7C, kActor = 0xA0, kFlags = 0x120;
constexpr std::uint32_t kShown = 0x200;
constexpr std::uintptr_t kStamp = 0x9EC; // the actor's setup stamp

constexpr unsigned kLogLines = 100; // then one line every 100

using PartLoaded = void(__cdecl*)(std::uint32_t handle, std::uint32_t* request);
using WeakActor = std::uintptr_t(__thiscall*)(void* link);
using Loaded = std::uint8_t(__fastcall*)(std::uint32_t* handle);
using Valid = std::uint8_t(__thiscall*)(std::uintptr_t handles, std::uint32_t handle);
using Pending = int(__thiscall*)(std::uintptr_t resource);

bool g_tried = false;
PartLoaded g_part_loaded = nullptr; // the game's, through the trampoline
WeakActor g_weak_actor = nullptr;
Loaded g_loaded = nullptr;
Valid g_valid = nullptr;
Pending g_pending = nullptr;
std::uintptr_t g_entities = 0;     // the entity pointers
std::uintptr_t g_handles_at = 0;   // the global pointing to the resource handles
std::uintptr_t g_heap_first = 0;   // the globals holding the resource heap's first and last blocks
std::uintptr_t g_heap_last = 0;
unsigned g_failed = 0;

enum class Why { Fine, Hidden, Released, NoData, Pending };

struct Probe {
    Why why = Why::Fine;
    std::uint32_t model = 0, file = 0, index = 0, flags = 0;
    std::uintptr_t sub = 0;
    int pending = 0;
    char name[25] = {};
};

struct Heap {
    bool read = false;
    std::uint32_t size = 0, free = 0, largest = 0, blocks = 0;
};

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

std::uint32_t read32(std::uintptr_t at)
{
    return *reinterpret_cast<const std::uint32_t*>(at);
}

// What the game will do with this part, by its own tests, before the callback frees the request. Why::Fine for a part it
// takes, or drops for a reason that leaves nothing missing (an entity that's gone, a look set up again since).
void probe_request(std::uint32_t handle, const std::uint32_t* request, Probe& out)
{
    out.model = request[6];
    out.file = request[7];
    out.index = request[2];
    out.sub = request[4];
    const std::uintptr_t entity = g_entities != 0 && out.index < 0x4000 ? read32(g_entities + out.index * 4) : 0;
    if (entity == 0 || read32(entity + kServerId) != request[3])
        return; // gone: its actor was dropped too
    std::memcpy(out.name, reinterpret_cast<const void*>(entity + kName), 24);
    out.flags = read32(entity + kFlags);
    const std::uintptr_t actor = read32(entity + kActor);
    if (actor == 0)
        return; // no actor to draw it on
    if (out.sub == 0 && read32(actor + kStamp) != request[0])
        return; // its look was set up again since: that asked for its own parts
    if (g_weak_actor(const_cast<std::uint32_t*>(request) + 1) == 0) {
        out.why = Why::Hidden;
        return;
    }
    std::uint32_t local = handle;
    if (g_loaded(&local) != 0)
        return;
    const std::uintptr_t handles = read32(g_handles_at);
    if (handle == 0 || g_valid(handles, handle) == 0)
        out.why = Why::Released;
    else if (read32(handle) == 0)
        out.why = Why::NoData;
    else {
        out.why = Why::Pending;
        out.pending = g_pending(read32(handle));
    }
}

bool probe(std::uint32_t handle, const std::uint32_t* request, Probe& out)
{
    __try {
        probe_request(handle, request, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The resource heap: its free blocks (a list through +0x10, +4 set while one is in use, +8 the block after it).
void walk_heap(Heap& out)
{
    const std::uintptr_t first = read32(g_heap_first), last = read32(g_heap_last);
    if (first == 0 || last <= first)
        return;
    out.size = static_cast<std::uint32_t>(last - first);
    std::uintptr_t block = first;
    for (unsigned i = 0; i < 200000; ++i) {
        const std::uintptr_t next = read32(block + 8);
        if (read32(block + 4) == 0 && next > block + 0x20) {
            const auto size = static_cast<std::uint32_t>(next - block - 0x20);
            out.free += size;
            out.largest = size > out.largest ? size : out.largest;
            ++out.blocks;
        }
        block = read32(block + 0x10);
        if (block == last || block == 0) {
            out.read = true;
            return;
        }
    }
}

void heap(Heap& out)
{
    if (g_heap_first == 0)
        return;
    __try {
        walk_heap(out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.read = false;
    }
}

const wchar_t* slot_name(std::uint32_t model)
{
    static const wchar_t* const names[] = {L"face", L"head", L"body", L"hands", L"legs",
                                           L"feet", L"main", L"sub",  L"ranged", L"slot 9"};
    const std::uint32_t slot = model >> 12 & 0xF;
    return slot < std::size(names) ? names[slot] : L"slot ?";
}

std::wstring why_text(const Probe& p)
{
    wchar_t text[160];
    switch (p.why) {
    case Why::Hidden:
        swprintf_s(text, L"the character was hidden as it arrived (flags 0x%08X): the part was dropped", p.flags);
        break;
    case Why::Released:
        swprintf_s(text, L"its handle was already released: the slot was left empty");
        break;
    case Why::NoData:
        swprintf_s(text, L"no data (the DAT wasn't read, or the game's memory had no room): the slot was left empty");
        break;
    default:
        swprintf_s(text, L"%d of its parts (textures) were still pending: the slot was left empty", p.pending);
        break;
    }
    return text;
}

std::wstring dat_text(std::uint32_t file)
{
    const std::wstring rom = dats::rom_path(file);
    const std::wstring path = dats::path(file);
    wchar_t text[96];
    if (path.empty()) {
        swprintf_s(text, L"DAT %u (in no table)", file);
        return text;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const bool found = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != 0;
    swprintf_s(text, found ? L"DAT %u, %ls, %u KB" : L"DAT %u, %ls, not there", file, rom.c_str(),
               found ? (data.nFileSizeLow + 1023) / 1024 : 0u);
    std::wstring out = text;
    const std::wstring folder = dats::ffxi_folder();
    if (path.size() < folder.size() || _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) != 0)
        out += L" from " + path; // an overlay's
    return out;
}

void report(const Probe& p)
{
    ++g_failed;
    if (g_failed > kLogLines && g_failed % kLogLines != 0)
        return;
    wchar_t name[32] = {};
    MultiByteToWideChar(932, 0, p.name, -1, name, static_cast<int>(std::size(name)));
    Heap h;
    heap(h);
    wchar_t memory[128] = L"";
    if (h.read)
        swprintf_s(memory, L"; resource memory: %u KB free of %u KB in %u blocks, the largest %u KB", h.free / 1024,
                   h.size / 1024, h.blocks, h.largest / 1024);
    log::write(L"Gear: %ls (index %u)%ls's %ls (model 0x%04X, %ls) didn't load: %ls%ls%ls", name, p.index,
               p.sub != 0 ? L", a sub-actor" : L"", slot_name(p.model), p.model, dat_text(p.file).c_str(),
               why_text(p).c_str(), memory, g_failed > kLogLines ? L" (only every 100th is logged now)" : L"");
}

void __cdecl part_loaded_hook(std::uint32_t handle, std::uint32_t* request)
{
    Probe p;
    const bool probed = request != nullptr && probe(handle, request, p);
    g_part_loaded(handle, request); // frees the request
    if (probed && p.why != Why::Fine)
        report(p);
}

void find_heap(HMODULE game)
{
    const std::uintptr_t at = scan::find(game, kHeapLargest);
    if (at == 0)
        return;
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + kDefaultJump + 2), 4);
    const auto* fallback = reinterpret_cast<const std::uint8_t*>(at + kDefaultJump + 6 + rel);
    if (fallback[0] != 0xA1 || fallback[5] != 0x8B || fallback[6] != 0x0D)
        return;
    g_heap_last = read32(reinterpret_cast<std::uintptr_t>(fallback) + 1);
    g_heap_first = read32(reinterpret_cast<std::uintptr_t>(fallback) + 7);
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kPartLoaded) : 0;
    if (at == 0) {
        log::write(L"Gear: the game's gear part loading wasn't found in FFXiMain.dll: parts that don't load aren't logged");
        return;
    }
    const std::uintptr_t weak = call_target(at + kActorCall);
    const std::uintptr_t loaded = call_target(at + kLoadedCall);
    const auto* test = reinterpret_cast<const std::uint8_t*>(loaded);
    if (reinterpret_cast<const std::uint8_t*>(weak)[kEntitiesAt - 3] != 0x8B || test[kHandlesAt - 2] != 0x8B ||
        test[kValidCall] != 0xE8 || test[kPendingCall] != 0xE8) {
        log::write(L"Gear: the game's gear part loading isn't as expected: parts that don't load aren't logged");
        return;
    }
    g_weak_actor = reinterpret_cast<WeakActor>(weak);
    g_entities = read32(weak + kEntitiesAt);
    g_loaded = reinterpret_cast<Loaded>(loaded);
    g_handles_at = read32(loaded + kHandlesAt);
    g_valid = reinterpret_cast<Valid>(call_target(loaded + kValidCall));
    g_pending = reinterpret_cast<Pending>(call_target(loaded + kPendingCall));
    find_heap(game);
    if (!imports::detour(reinterpret_cast<PartLoaded>(at), kPrologue, &part_loaded_hook, g_part_loaded)) {
        log::write(L"Gear: the game's gear part loading couldn't be hooked: parts that don't load aren't logged");
        return;
    }
    log::write(L"Gear: gear parts that don't load are logged (hooked at +0x%X%ls)",
               static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)),
               g_heap_first != 0 ? L"" : L"; the resource memory wasn't found");
}

} // namespace

void on_frame()
{
    if (!g_tried)
        install_hook();
}

} // namespace vanacore::gearload
