#include "partymenu.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "addons.hpp"
#include "chat.hpp"
#include "gameui.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::partymenu {
namespace {

// The main menu's Party list ("menu    prty1": Seek Party, Find Member, Request, View Request, Autogroup, Languages,
// Alarm Type, ids 1 to 7) has a handler (its global FFXiMain+0x62F718, 2025-12-26) whose table of functions has the
// window's input at slot 6 (FUN_10203f80: on select, event 5, it calls slot 17 with the item) and the item chosen at
// slot 17 (FUN_102040c0, __thiscall, ret 4: the item's id, a short): movsx eax,word ptr [esp+4]; dec eax; cmp eax,6; ja;
// jmp [eax*4+table]; then item 1's case, Seek Party: mov dl,[ecx+2Ch]; test dl,dl; sete al; push eax; call
constexpr char kChosen[] = "0F BF 44 24 04 48 83 F8 06 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ?? 8A 51 2C 84 D2 0F 94 C0 50 E8";
constexpr std::ptrdiff_t kJumpTableAt = 18;
// Find Member's case (item 2): the search for characters seeking a party, or with a target seeking one, that target's:
// cmp dword ptr [ecx+18h],1; jne; call
constexpr std::uint8_t kFindMemberCase[] = {0x83, 0x79, 0x18, 0x01, 0x75, 0x08, 0xE8};
// The input (slot 6): cmp word ptr [esp+4],5; jne; mov edx,[esp+8]; mov eax,[ecx]; push edx; call [eax+44h]; ret 8
constexpr std::uint8_t kInput[] = {0x66, 0x83, 0x7C, 0x24, 0x04, 0x05, 0x75, 0x0A, 0x8B, 0x54, 0x24, 0x08,
                                   0x8B, 0x01, 0x52, 0xFF, 0x50, 0x44, 0xC2, 0x08, 0x00};
constexpr int kInputSlot = 6, kChosenSlot = 17;
constexpr short kFindMember = 2;
constexpr char kFindMemberElement[] = "party_menu.find_member"; // as gameui.cpp and addons name it

using Chosen = void(__thiscall*)(void* handler, int item);

bool g_on = false; // [client] partyfinder.findmember (off by default: not live until Party Finder's author agrees)
bool g_tried = false;
Chosen g_chosen = nullptr; // the game's slot 17

bool in_image(std::uintptr_t address, std::uintptr_t base, std::size_t size)
{
    return address >= base && address < base + size;
}

// The Party Finder addon loaded and running
bool party_finder_running()
{
    for (const addons::Loaded& addon : addons::loaded())
        if (addon.state == 1 && _stricmp(addon.name.c_str(), "partyfinder") == 0)
            return true;
    return false;
}

// Its full window shown (not the small icon it keeps up in its mini mode)
bool party_finder_shown()
{
    for (const layout::Window& window : layout::windows())
        if (window.shown && window.name == "Party Finder")
            return true;
    return false;
}

// An item chosen in the Party list: Find Member is the addon's that took it over
// (vana.gameui.replace('party_menu.find_member', fn): fn runs at the next frame), else it opens Party Finder when it runs
// and [client] partyfinder.findmember is on (/pf toggle: opened if it's closed, its full window if it's in its mini mode;
// nothing if it's up already, as /pf toggle would make it small), on the next frame as if typed; the rest as the game
// does it. The select sound was played by the window already.
// MOCKUP: with gameui, partyfinder.findmember and party_finder_running() go: Party Finder calls
// vana.gameui.replace('party_menu.find_member', fn) itself.
void __fastcall party_chosen(void* handler, void*, int item)
{
    if (static_cast<short>(item) == kFindMember && gameui::replaced(kFindMemberElement)) {
        gameui::replaced_chosen(kFindMemberElement);
        return;
    }
    if (g_on && static_cast<short>(item) == kFindMember && party_finder_running()) {
        if (!party_finder_shown()) {
            log::write(L"Party menu: Find Member chosen: /pf toggle (Party Finder)");
            chat::run(1, "/pf toggle");
        }
        return;
    }
    g_chosen(handler, item);
}

// The handler's table found by its slot 17 (a pointer to the function above, in the image's data, with slot 6 the input)
// and that slot pointed here. One store: the game's thread runs it.
void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr) {
        g_tried = false;
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t chosen = scan::find(game, kChosen);
    if (chosen == 0) {
        log::write(L"Party menu: the Party list's handler wasn't found in FFXiMain.dll: partyfinder.findmember does nothing");
        return;
    }
    const auto* jumps = *reinterpret_cast<const std::uintptr_t* const*>(chosen + kJumpTableAt);
    if (!in_image(reinterpret_cast<std::uintptr_t>(jumps), base, size) || !in_image(jumps[kFindMember - 1], base, size) ||
        std::memcmp(reinterpret_cast<const void*>(jumps[kFindMember - 1]), kFindMemberCase, sizeof kFindMemberCase) != 0) {
        log::write(L"Party menu: the Party list's Find Member isn't as expected: partyfinder.findmember does nothing");
        return;
    }
    char pattern[3 * sizeof chosen + 1] = {};
    for (std::size_t i = 0; i < sizeof chosen; ++i)
        std::snprintf(pattern + 3 * i, 4, i + 1 < sizeof chosen ? "%02X " : "%02X", static_cast<unsigned>((chosen >> (8 * i)) & 0xFF));
    void** slot = nullptr;
    for (unsigned n = 0; slot == nullptr && n < 16; ++n) {
        const std::uintptr_t match = scan::find(base, size, pattern, n);
        if (match == 0)
            break;
        if (match % 4 != 0 || match < base + kChosenSlot * 4)
            continue;
        auto* table = reinterpret_cast<void**>(match) - kChosenSlot;
        const auto input = reinterpret_cast<std::uintptr_t>(table[kInputSlot]);
        if (in_image(input, base, size) && std::memcmp(reinterpret_cast<const void*>(input), kInput, sizeof kInput) == 0)
            slot = reinterpret_cast<void**>(match);
    }
    if (slot == nullptr) {
        log::write(L"Party menu: the Party list's handler's table wasn't found: partyfinder.findmember does nothing");
        return;
    }
    DWORD protect = 0;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &protect)) {
        log::write(L"Party menu: the handler's table couldn't be written (error %lu): partyfinder.findmember does nothing", GetLastError());
        return;
    }
    g_chosen = reinterpret_cast<Chosen>(chosen);
    *reinterpret_cast<void* volatile*>(slot) = reinterpret_cast<void*>(&party_chosen);
    VirtualProtect(slot, sizeof *slot, protect, &protect);
    log::write(L"Party menu: the Party list's choice hooked (+0x%X, its table's slot at +0x%X)", static_cast<unsigned>(chosen - base),
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(slot) - base));
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"partyfinder.findmember", 0, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    // hooked once it's wanted: the setting on, or an addon holding Find Member (vana.gameui.replace)
    if (!g_tried && (g_on || gameui::replaced(kFindMemberElement)))
        install_hook();
}

} // namespace vanacore::partymenu
