#include "fishing.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "log.hpp"
#include "scan.hpp"

namespace vanacore::fishing {
namespace {

// The fish fight (FFXiMain.dll, the 2025-12-26 client: FUN_10089660, each frame while a fish is on the line) reads the
// player's direction once a frame through FUN_10089C50(0x3F, 4), which hands it to FUN_10089C90: that asks the game's
// input actions (FUN_10123570, __cdecl (context, action), the answer in ST0: -1 left, 1 right, 0 neither) and keeps a
// press latch, so a held direction counts once. Action 4 is the movement's left and right (the pad's movement axis, the
// mouse at the screen's edge, numpad 4/6, key binds 0xA2/0xA3). The D-pad is in the menu slots of the game's button map:
// action 9 (FUN_10120920, the pad's function bits at [pad]+0x220: 0x10 right 1, 0x20 left -1). Nothing else calls
// FUN_10089C90, so its call to FUN_10123570 goes to direction() instead, which asks the menus' left and right when the
// movement's is 0 and action 4 isn't held back (the game's context mask, the chat line open). The latch after it is the
// game's, so a D-pad press counts once like the stick's.
//
// FUN_10089C50: mov ecx,[esp+8]; xor eax,eax; cmp ecx,4; jnz; mov eax,<latch>; push eax; mov eax,[esp+8]; push ecx;
// push eax; call FUN_10089C90; add esp,0Ch; ret; cmp ecx,5; jnz; mov eax,<latch 2>; push eax; mov eax,[esp+8]; push ecx;
// push eax; call
constexpr char kFight[] = "8B 4C 24 08 33 C0 83 F9 04 75 ?? B8 ?? ?? ?? ?? 50 8B 44 24 08 51 50 E8 ?? ?? ?? ?? 83 C4 0C C3 "
                          "83 F9 05 75 ?? B8 ?? ?? ?? ?? 50 8B 44 24 08 51 50 E8";
constexpr std::ptrdiff_t kFightCall = 0x17; // its call to FUN_10089C90

// FUN_10089C90: mov eax,[esp+8]; mov ecx,[esp+4]; push eax; push ecx; call <actions>; mov edx,[esp+14h]; add esp,8;
// mov cl,[edx] (the latch)
constexpr std::uint8_t kLatchHead[] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04, 0x50, 0x51, 0xE8};
constexpr std::uint8_t kLatchTail[] = {0x8B, 0x54, 0x24, 0x14, 0x83, 0xC4, 0x08, 0x8A, 0x0A};
constexpr std::ptrdiff_t kActionsCall = 0x0A;

// FUN_10123570's start: push ecx; mov eax,[esp+0Ch]; mov edx,[context]; xor ecx,ecx; push esi; mov cx,[eax*2+masks];
// push edi; test ecx,edx; jnz; fld [zero]; pop edi; pop esi; pop ecx; ret; mov ecx,[chat]; test ecx,ecx; jz;
// mov dl,[ecx+<typing>]; test dl,dl
constexpr char kActions[] = "51 8B 44 24 0C 8B 15 ?? ?? ?? ?? 33 C9 56 66 8B 0C 45 ?? ?? ?? ?? 57 85 D1 75 0A D9 05 ?? ?? ?? ?? "
                            "5F 5E 59 C3 8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8A 91 ?? ?? ?? ?? 84 D2";
constexpr std::size_t kActionsSize = 0x37;
constexpr std::ptrdiff_t kContextAt = 0x07, kMasksAt = 0x12, kChatAt = 0x27, kTypingAt = 0x31;

// Action 9's pad source: sub esp,8; mov eax,[pad]; xor edx,edx; mov ecx,[eax+220h]; mov eax,[eax+224h];
// mov [esp+4],eax; mov eax,ecx; and eax,10h; or eax,edx; jz; fld [1.0]; add esp,8; ret; and ecx,20h; xor eax,eax;
// or ecx,eax; jz
constexpr char kMenuX[] = "83 EC 08 A1 ?? ?? ?? ?? 33 D2 8B 88 20 02 00 00 8B 80 24 02 00 00 89 44 24 04 8B C1 83 E0 10 0B C2 "
                          "74 ?? D9 05 ?? ?? ?? ?? 83 C4 08 C3 83 E1 20 33 C0 0B C8 74";

constexpr int kMovementX = 4; // the action the fight asks for

using Actions = float(__cdecl*)(int context, int action);
using MenuX = float(__cdecl*)();

bool g_on = true;        // [input] fishing.dpad
bool g_searched = false; // FFXiMain.dll was looked through
Actions g_actions = nullptr;
MenuX g_menu_x = nullptr;
const std::uint32_t* g_context = nullptr; // the actions live now (the game's context mask)
const std::uint16_t* g_masks = nullptr;   // each action's context bits
std::uint8_t* const* g_chat = nullptr;    // the chat line's object, and its typing byte at g_typing
std::uint32_t g_typing = 0;
unsigned g_used = 0;

// Action 4 as the game would let it through, held back or not (FUN_10123570's own first checks)
bool movement_live()
{
    if ((g_masks[kMovementX] & *g_context) == 0)
        return false;
    const std::uint8_t* chat = *g_chat;
    return chat == nullptr || chat[g_typing] == 0;
}

float __cdecl direction(int context, int action)
{
    const float movement = g_actions(context, action);
    if (movement != 0.0f || !g_on || action != kMovementX || !movement_live())
        return movement;
    const float menu = g_menu_x();
    if (menu != 0.0f && ++g_used == 1)
        log::write(L"Fishing: the D-pad (the menus' left and right) fights the fish ([input] fishing.dpad=1)");
    return menu;
}

std::uintptr_t call_target(std::uintptr_t at)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), sizeof rel);
    return at + 5 + rel;
}

bool write_call(std::uintptr_t at, std::uintptr_t to)
{
    std::uint8_t call[5] = {0xE8};
    const auto rel = static_cast<std::int32_t>(to - (at + 5));
    std::memcpy(call + 1, &rel, sizeof rel);
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), sizeof call, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    std::memcpy(reinterpret_cast<void*>(at), call, sizeof call);
    VirtualProtect(reinterpret_cast<void*>(at), sizeof call, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), sizeof call);
    return true;
}

// The fight's call site and the game's two input functions, checked by their bytes; 0 if any isn't as expected
std::uintptr_t find_site(HMODULE game)
{
    const std::uintptr_t fight = scan::find(game, kFight);
    const std::uintptr_t menu_x = scan::find(game, kMenuX);
    if (fight == 0 || menu_x == 0)
        return 0;
    const std::uintptr_t latch = call_target(fight + kFightCall);
    const auto* code = reinterpret_cast<const std::uint8_t*>(latch);
    if (std::memcmp(code, kLatchHead, sizeof kLatchHead) != 0 ||
        std::memcmp(code + kActionsCall + 5, kLatchTail, sizeof kLatchTail) != 0)
        return 0;
    const std::uintptr_t site = latch + kActionsCall;
    const std::uintptr_t actions = call_target(site);
    if (scan::find(actions, kActionsSize, kActions) != actions)
        return 0;
    g_actions = reinterpret_cast<Actions>(actions);
    g_menu_x = reinterpret_cast<MenuX>(menu_x);
    g_context = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(scan::operand(actions, kContextAt)));
    g_masks = reinterpret_cast<const std::uint16_t*>(static_cast<std::uintptr_t>(scan::operand(actions, kMasksAt)));
    g_chat = reinterpret_cast<std::uint8_t* const*>(static_cast<std::uintptr_t>(scan::operand(actions, kChatAt)));
    g_typing = scan::operand(actions, kTypingAt);
    return site;
}

void patch()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const std::uintptr_t site = find_site(game);
    if (site == 0) {
        log::write(L"Fishing: the fish fight's direction read wasn't found in FFXiMain.dll: [input] fishing.dpad does nothing");
        return;
    }
    if (!write_call(site, reinterpret_cast<std::uintptr_t>(&direction))) {
        log::write(L"Fishing: the fish fight's direction read at +0x%X couldn't be patched: [input] fishing.dpad does nothing",
                   static_cast<unsigned>(site - base));
        return;
    }
    log::write(L"Fishing: the fish fight's direction read at +0x%X takes the D-pad too (actions +0x%X, menu left and right "
               L"+0x%X)",
               static_cast<unsigned>(site - base), static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_actions) - base),
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_menu_x) - base));
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"input", L"fishing.dpad", 1, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_on && !g_searched)
        patch();
}

} // namespace vanacore::fishing
