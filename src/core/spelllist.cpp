#include "spelllist.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::spelllist {
namespace {

// KaMenuMagicSel::Create (FUN_1022aa60, the PS2 client's name; __thiscall: category, rearranging, filter), which both
// Magic lists are made by: it has the game add newly learned spells to its order of the character's spells
// (KaMagicIndex, at g_pMagicIndex: a bit per spell known, then the spells' ids in the order listed, then how many),
// then lists the category's spells in that order. sub esp,28h; push ebx; push edi; mov edi,ecx; mov [esp+1Ch],edi;
// mov dword ptr [edi+6Ch],0; mov ebx,[g_pKaDataMagic]; test ebx,ebx; mov [esp+24h],ebx; jz; mov ecx,[g_pMagicIndex];
// push ebp; push esi; call (the adding); mov eax,[g_pMagicIndex]; push 0Ch; push 0; mov ecx,edi; mov ebp,[eax+(count)];
// push ebp; push (a row's drawing)
constexpr char kCreate[] = "83 EC 28 53 57 8B F9 89 7C 24 1C C7 47 6C 00 00 00 00 8B 1D ?? ?? ?? ?? 85 DB 89 5C 24 24 0F 84 "
                           "?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 55 56 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 6A 0C 6A 00 8B CF 8B A8 ?? ?? "
                           "?? ?? 55 68";
constexpr std::ptrdiff_t kIndexAt = 0x26, kMergeCall = 0x2C, kDrawRowAt = 0x44;
// The adding (FUN_1022a6f0, __thiscall on the index): push esi; mov esi,ecx; call (the spells known); test eax,eax; jz;
// mov ecx,[esi+(count)]; push 0; push (most spells); lea edx,[esi+(ids)]; push ecx; push edx; push esi; push eax; call
constexpr char kMerge[] = "56 8B F1 E8 ?? ?? ?? ?? 85 C0 74 ?? 8B 8E ?? ?? ?? ?? 6A 00 68 ?? ?? ?? ?? 8D 96 ?? ?? ?? ?? 51 52 "
                          "56 50 E8";
constexpr std::ptrdiff_t kCountAt = 0xE, kMostAt = 0x15, kIdsAt = 0x1B;
constexpr std::size_t kMergeLength = 0x40;
// A row's drawing (FUN_1022b520) takes the spell's name as it's shown: push esi (the spell); call (the spells' names'
// table, made the first time); mov ecx,eax; call (a name, __thiscall: spell); mov ebx,[esp+24h]; mov esi,[esp+28h];
// push eax; push esi; lea edx,[ebx+12h]; push edx
constexpr char kName[] = "56 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 8B 5C 24 24 8B 74 24 28 50 56 8D 53 12 52";
constexpr std::ptrdiff_t kTableCall = 1, kNameCall = 8;
constexpr std::size_t kDrawRowLength = 0x100; // how far into the row's drawing its name is looked for

using Merge = void(__fastcall*)(char* index, void* edx);
using Table = void*(__cdecl*)();
using Name = const char*(__fastcall*)(void* table, void* edx, unsigned spell);

bool g_autosort = false; // [client] spells.autosort
bool g_tried = false;
Merge g_merge = nullptr; // the game's
Table g_table = nullptr;
Name g_name = nullptr;
bool g_logged = false, g_logged_layout = false;

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

template <typename T> T operand(std::uintptr_t at)
{
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof value);
    return value;
}

// Where the index keeps its ids and count, and how many spells it holds, as the adding's code says now. Read each time
// a list is made, not once at the hook: CatsEyeXI's cexislots.dll enlarges the index (4096 spells: ids +0x200, count
// +0x2200, where the game's own 1024 have +0x80 and +0x880) by rewriting these operands in place, often after the hook
// is in; a layout read before then put the count in the middle of the ids (0), and nothing was ever sorted.
struct Layout {
    std::ptrdiff_t count_at = 0, ids_at = 0;
    int most = 0;
};

bool read_layout(Layout& layout)
{
    const auto merge = reinterpret_cast<std::uintptr_t>(g_merge);
    if (scan::find(merge, kMergeLength, kMerge) != merge)
        return false;
    layout.count_at = operand<std::int32_t>(merge + kCountAt);
    layout.ids_at = operand<std::int32_t>(merge + kIdsAt);
    layout.most = operand<std::int32_t>(merge + kMostAt);
    return layout.most > 0 && layout.ids_at >= 0 &&
           layout.ids_at + 2 * static_cast<std::ptrdiff_t>(layout.most) <= layout.count_at;
}

// The index's spells in A-Z order by their names (as the game shows them); spells with the same name by id
void sort(char* index)
{
    Layout layout;
    const bool ok = read_layout(layout);
    if (!g_logged_layout) {
        g_logged_layout = true;
        log::write(ok ? L"Spell list: the game's order of spells has its ids at +0x%X, its count at +0x%X (%d spells at most)"
                      : L"Spell list: the game's order of spells isn't as expected (+0x%X ids, +0x%X count, %d spells): not sorted",
                   static_cast<unsigned>(layout.ids_at), static_cast<unsigned>(layout.count_at), layout.most);
    }
    if (!ok)
        return;
    const int count = *reinterpret_cast<int*>(index + layout.count_at);
    if (count < 2 || count > layout.most)
        return;
    void* table = g_table();
    if (table == nullptr)
        return;
    auto* ids = reinterpret_cast<std::uint16_t*>(index + layout.ids_at);
    struct Spell {
        std::string name;
        std::uint16_t id;
    };
    std::vector<Spell> spells;
    spells.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const char* name = g_name(table, nullptr, ids[i]); // copied: a name may be made in a buffer the next one reuses
        spells.push_back({name != nullptr ? name : "", ids[i]});
    }
    std::stable_sort(spells.begin(), spells.end(), [](const Spell& a, const Spell& b) {
        const int order = _stricmp(a.name.c_str(), b.name.c_str());
        return order != 0 ? order < 0 : a.id < b.id;
    });
    for (int i = 0; i < count; ++i)
        ids[i] = spells[static_cast<std::size_t>(i)].id;
    if (!g_logged) {
        g_logged = true;
        log::write(L"Spell list: %d spells put in A-Z order ([client] spells.autosort)", count);
    }
}

// The game adding newly learned spells to its order, as a Magic list is made: then put in A-Z order
void __fastcall merge_hook(char* index, void* edx)
{
    g_merge(index, edx);
    if (g_autosort && index != nullptr)
        sort(index);
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    const std::uintptr_t create = scan::find(game, kCreate);
    const std::uintptr_t merge = create != 0 && is_call(create + kMergeCall) ? call_target(create + kMergeCall) : 0;
    const bool merge_ok = merge != 0 && scan::find(merge, kMergeLength, kMerge) == merge;
    const std::uintptr_t draw = create != 0 ? operand<std::uintptr_t>(create + kDrawRowAt) : 0;
    const std::uintptr_t name = draw != 0 ? scan::find(draw, kDrawRowLength, kName) : 0;
    if (create == 0 || !merge_ok || name == 0 || !is_call(name + kTableCall) || !is_call(name + kNameCall)) {
        log::write(L"Spell list: the game's Magic list wasn't found in FFXiMain.dll (%d%d%d): [client] spells.autosort does nothing",
                   create != 0, merge_ok, name != 0);
        return;
    }
    g_table = reinterpret_cast<Table>(call_target(name + kTableCall));
    g_name = reinterpret_cast<Name>(call_target(name + kNameCall));
    g_merge = reinterpret_cast<Merge>(merge);
    // Create's call of the adding goes to the hook instead (the adding itself is left whole)
    const std::uintptr_t call = create + kMergeCall;
    const std::int32_t rel =
        static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&merge_hook) - (call + 5));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(call + 1), 4, PAGE_EXECUTE_READWRITE, &old)) {
        log::write(L"Spell list: the game's Magic list couldn't be hooked: [client] spells.autosort does nothing");
        return;
    }
    std::memcpy(reinterpret_cast<void*>(call + 1), &rel, 4);
    VirtualProtect(reinterpret_cast<void*>(call + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    log::write(L"Spell list: the game's Magic list hooked at +0x%X",
               static_cast<unsigned>(call - reinterpret_cast<std::uintptr_t>(game)));
}

} // namespace

void reload(const wchar_t* ini) { g_autosort = GetPrivateProfileIntW(L"client", L"spells.autosort", 0, ini) != 0; }

void install(const wchar_t* ini) { reload(ini); }

void on_frame()
{
    if (g_autosort && !g_tried)
        install_hook();
}

} // namespace vanacore::spelllist
