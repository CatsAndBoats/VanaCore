#pragma once
// What the windows VanaCore puts at a place of its own through the game's own placing share (buffwin.cpp, castbar.cpp):
// a [client] number from the settings, and the menu manager and SetLocate found in FFXiMain.dll. (Taken out of the two
// files, where each had its own copy: lint's jscpd.)

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::menuplace {

// The manager's global, from the window that has the help line's text (FUN_101182a0), and SetLocate (FUN_10118d10:
// thiscall window, x, y, ret 8); see targetwin.cpp for both.
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8";
constexpr std::ptrdiff_t kManagerAt = 7;
constexpr char kSetLocate[] = "53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54";
constexpr std::ptrdiff_t kSetLocatePrologue = 8, kResizeAt = 0x6C;

// [client] `key` from `ini`: `fallback` when it's unset or not a number, else between `low` and `high`
inline double number(const wchar_t* key, double fallback, double low, double high, const wchar_t* ini)
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    if (text[0] == 0)
        return fallback;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text, &end);
    return end != text ? std::clamp(value, low, high) : fallback;
}

// The manager and SetLocate, looked for once (`searched`); `missing` logged when they aren't found. Whether they were
template <typename SetLocate> bool find(bool& searched, char*& manager, SetLocate& set_locate, const wchar_t* missing)
{
    if (searched)
        return set_locate != nullptr;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    searched = true;
    const std::uintptr_t help = scan::find(game, kHelp);
    const std::uintptr_t match = scan::find(game, kSetLocate);
    if (help != 0 && match != 0) {
        const std::uintptr_t locate = match - kSetLocatePrologue;
        const auto* start = reinterpret_cast<const std::uint8_t*>(locate);
        if ((start[0] == 0x83 || start[0] == 0xE9) && start[kResizeAt] == 0xE8) {
            std::uintptr_t found = 0;
            std::memcpy(&found, reinterpret_cast<const void*>(help + kManagerAt), sizeof found);
            manager = reinterpret_cast<char*>(found);
            set_locate = reinterpret_cast<SetLocate>(locate);
        }
    }
    if (set_locate == nullptr)
        log::write(L"%ls", missing);
    return set_locate != nullptr;
}

} // namespace vanacore::menuplace
