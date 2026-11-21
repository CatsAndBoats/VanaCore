#include "namechars.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::namechars {
namespace {

// One byte of the game's code: an operand `at` bytes into what `pattern` finds (left as ?? there, so the site is found
// whichever value it holds), the game's value and the one extended names want. Each pattern: one match in the running
// game, 2026-11-12.
struct Site {
    const wchar_t* what = L"";
    const char* pattern = "";
    std::ptrdiff_t at = 0;
    std::uint8_t game = 0, extended = 0;
    std::uint8_t* byte = nullptr; // in the game's code, once found holding one of the two
};
Site g_sites[] = {
    // Mode 4's case of the switch that makes a text input the one typed into (0x101C5120; the case at 0x101C522D): the
    // keyboard's set-up called with the box's place, its callbacks, its longest text and the character set, 3 (a typed
    // character passes if isalnum) or 4 (a single byte from a space up).
    //   mov eax,[esi+69Ch]; push 8; push eax; mov eax,[esi+698h]; push eax; lea eax,[esi+4]; push eax; mov eax,[esi+398h];
    //   push eax; push 3; jmp <the call>
    {L"the name boxes' character set",
     "8B 86 ?? ?? 00 00 6A 08 50 8B 86 ?? ?? 00 00 50 8D 46 04 50 8B 86 ?? ?? 00 00 50 6A ?? E9", 0x1C, 3, 4},
    // A row of the books' list (0x101A4FDA) and of the equip sets' (0x101A55D4): the name printed with its room, 75.
    //   push 4Bh; push 80808080h; push ebx; call <the book's name>; movsx ecx,[esp+1Ch]; push eax; add ecx,8; ...
    //   push 4Bh; push 80808080h; push edi; call <the set's name>; movsx ecx,[esp+1Ah]; movsx edx,[esp+18h]; ...
    {L"the book list's room for a name",
     "6A ?? 68 80 80 80 80 53 E8 ?? ?? ?? ?? 0F BF 4C 24 1C 50 83 C1 08 0F BF 44 24 22 83 C0 02 50 51 8B 4E 18 E8", 1, 0x4B, 0x60},
    {L"the equip set list's room for a name",
     "6A ?? 68 80 80 80 80 57 E8 ?? ?? ?? ?? 0F BF 4C 24 1A 0F BF 54 24 18 83 C1 02 50 83 C2 08 51 8B 4E 18 52 E8", 1, 0x4B,
     0x60},
};

bool g_on = false;       // [client] macronames.extended
bool g_read = false;     // read from the ini once (the first frame)
bool g_searched = false;
bool g_applied = false;  // the sites hold what g_was wants
bool g_was = false;

void find(HMODULE game)
{
    for (Site& site : g_sites) {
        const std::uintptr_t found = scan::find(game, site.pattern);
        auto* byte = found != 0 ? reinterpret_cast<std::uint8_t*>(found + site.at) : nullptr;
        if (byte != nullptr && (*byte == site.game || *byte == site.extended)) {
            site.byte = byte;
            continue;
        }
        if (byte != nullptr)
            log::write(L"Extended names: %ls isn't as VanaCore knows it in FFXiMain.dll (%02X): left as the game has it", site.what,
                       *byte);
        else
            log::write(L"Extended names: %ls wasn't found in FFXiMain.dll: left as the game has it", site.what);
    }
}

void apply()
{
    if (g_applied && g_was == g_on)
        return;
    int changed = 0;
    for (Site& site : g_sites) {
        const std::uint8_t want = g_on ? site.extended : site.game;
        if (site.byte == nullptr || *site.byte == want)
            continue;
        DWORD protect = 0;
        if (!VirtualProtect(site.byte, 1, PAGE_EXECUTE_READWRITE, &protect)) {
            log::write(L"Extended names: the game's code at %p couldn't be written (error %lu): %ls stays the game's",
                       static_cast<void*>(site.byte), GetLastError(), site.what);
            site.byte = nullptr;
            continue;
        }
        *site.byte = want;
        VirtualProtect(site.byte, 1, protect, &protect);
        FlushInstructionCache(GetCurrentProcess(), site.byte, 1);
        ++changed;
    }
    if (changed != 0)
        log::write(g_on ? L"Extended names: macro book and equip set names take spaces and symbols, and their lists give a "
                          L"name the row's width ([client] macronames.extended=1)"
                        : L"Extended names: macro book and equip set names as the game has them ([client] "
                          L"macronames.extended=0)");
    g_applied = true;
    g_was = g_on;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"macronames.extended", 0, ini) != 0; // taken up by on_frame
}

void on_frame(const wchar_t* ini)
{
    if (!g_read) {
        g_read = true;
        reload(ini);
    }
    if (!g_searched) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return; // (looked for again next frame)
        g_searched = true;
        find(game);
    }
    apply();
}

} // namespace vanacore::namechars
