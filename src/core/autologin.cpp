#include "autologin.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "game.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::autologin {
namespace {

// The title screen is the lobby's thread (the PC client's LobbyMainThread; XIClient's LobbyMainTask.cpp has it with
// names): each step opens a window, waits for a flag in the window's owner to be set, then reads the answer beside it.
// The owners are made once at the lobby's start, each in a global of FFXiMain.dll; a window's key handler (vtable slot
// 6, __thiscall (event, id), ret 8: event 5 is Enter on button `id` (from 1), 9 the cursor put on it) sets the flag.
// VanaCore calls that handler, so the game's own checks (an empty slot, a button that's off) and sounds are kept.
//
//   terms        "menu    ptc8lice"  CIwLicenceMenu: flag +0x1C, answer +0x18 (0 Accept, 1 Decline)
//   lobby menu   "menu    loby2win"  CIwLobbyMenu: flag +0x1C, button +0x18 (0 Select Character), +0x20 on per button
//   characters   "menu    dbnamese"  CIwSelectMenu: flag +0x1C, a character from +0x34 every 0xB0 (+0x34 enabled);
//                                    the one picked is a global its key handler sets
//   Yes / No     "menu    ptc6yesn"  CIwYesNoMenu: flag +0x30, message +0x18 (0xD: play this character?), answer +0x14
//
// An owner's window is open while its +8 (the menu control it's linked to) isn't 0.

// DoLicenseWindow (0x1023C7B0): CreateDrawMenu(terms); task_wait(&terms+0x1C); DestroyDrawMenu; return terms+0x18
constexpr char kTerms[] = "6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C0 1C 50 E8 ?? ?? ?? ?? "
                          "83 C4 04 B9 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8B 41 18 C3";
constexpr std::ptrdiff_t kTermsAt = 0x14;
// LobbyMenuProc (0x1023EC60): the help line's +0x14 = 2; ...; lobby+0x1C = 0; the "back from the list" flag tested
constexpr char kLobby[] = "A1 ?? ?? ?? ?? 53 56 C7 40 14 02 00 00 00 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                          "8B 0D ?? ?? ?? ?? 33 DB 89 59 1C A1 ?? ?? ?? ?? 83 F8 01";
constexpr std::ptrdiff_t kLobbyAt = 0x10;
// SelectMenuProc (0x10241B80), as it opens the list: list+0x1C = 0, the character picked = 0
constexpr char kList[] = "A1 ?? ?? ?? ?? 83 C4 04 3B C6 75 0B 8B 15 ?? ?? ?? ?? 89 5A 1C EB 0E A1 ?? ?? ?? ?? 89 1D ?? ?? ?? ?? 89 58 1C";
constexpr std::ptrdiff_t kListAt = 0xE;
// IwYesNoMenuConst2 (0x1023C900): the Yes / No owner's fields set up before it opens
constexpr char kYesNo[] = "A1 ?? ?? ?? ?? 8B 4C 24 04 53 33 DB 89 58 30 88 98 E1 A5 00 00 89 58 18 88 58 20 88 98 E0 A5 00 00 C7 40 14 01 00 00 00";
constexpr std::ptrdiff_t kYesNoAt = 1;

// Each key handler begins by reading its flag (mov eax,[ecx+flag], or the list's push ebx; push esi; mov esi,ecx):
// slot 6 checked against it before it's called
constexpr std::uint8_t kTermsKey[] = {0x8B, 0x41, 0x1C, 0xBA, 0x01, 0x00, 0x00, 0x00};
constexpr std::uint8_t kLobbyKey[] = {0x8B, 0x41, 0x1C, 0xBA, 0x01, 0x00, 0x00, 0x00};
constexpr std::uint8_t kListKey[] = {0x53, 0x56, 0x8B, 0xF1, 0xBB, 0x01, 0x00, 0x00, 0x00};
constexpr std::uint8_t kYesNoKey[] = {0x8B, 0x41, 0x30, 0xBA, 0x01, 0x00, 0x00, 0x00};

constexpr std::ptrdiff_t kControl = 8, kKeySlot = 6;
constexpr std::ptrdiff_t kTermsFlag = 0x1C;
constexpr std::ptrdiff_t kLobbyFlag = 0x1C, kLobbyOn = 0x20;
constexpr std::ptrdiff_t kListFlag = 0x1C, kListFirst = 0x34, kListStride = 0xB0, kListEnabled = 0x34;
constexpr std::ptrdiff_t kYesNoFlag = 0x30, kYesNoMessage = 0x18;
constexpr int kPlayMessage = 0xD;
constexpr int kMostCharacters = 16;
constexpr int kEnter = 5, kPoint = 9;

// How long a window has been open before it's answered: past its fade-in, and the lobby's thread waiting on it
constexpr ULONGLONG kSettle = 400;
// The Yes / No answered only this soon after VanaCore picked the character
constexpr ULONGLONG kYesNoWithin = 30000;

using KeyDown = void(__thiscall*)(void* owner, int event, int id);

enum Step { kTermsStep, kLobbyStep, kListStep, kYesNoStep, kSteps };
constexpr const wchar_t* kStepNames[kSteps] = {L"the terms accepted", L"Select Character picked", L"a character picked",
                                               L"Yes to playing it"};

bool g_on = false;       // [client] autologin
int g_character = 1;     // [client] autologin.character, 1 the first in the list
bool g_searched = false; // FFXiMain.dll was looked through (g_found: all four there)
bool g_found = false;
bool g_done = false;     // nothing more this game start
std::uintptr_t g_base = 0, g_end = 0;
char** g_owners[kSteps] = {};
ULONGLONG g_open_since[kSteps] = {};
bool g_answered[kSteps] = {};
ULONGLONG g_listed_at = 0; // when VanaCore picked the character
ULONGLONG g_next_world_check = 0;

// A protected copy: the lobby's owners go when the title screen does (0 read then)
bool peek(const void* at, void* out, std::size_t size)
{
    __try {
        std::memcpy(out, at, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
T read(const void* at)
{
    T value{};
    if (!peek(at, &value, sizeof(value)))
        return T{};
    return value;
}

std::uintptr_t operand(std::uintptr_t at)
{
    std::uintptr_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof(value));
    return value;
}

bool in_game(std::uintptr_t address) { return address > g_base && address < g_end; }

void stop(const wchar_t* why)
{
    g_done = true;
    log::write(L"Auto login: stopped (%ls)", why);
}

void find()
{
    if (g_searched)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_searched = true;
    g_base = reinterpret_cast<std::uintptr_t>(game);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    g_end = g_base + nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t terms = scan::find(game, kTerms);
    const std::uintptr_t lobby = scan::find(game, kLobby);
    const std::uintptr_t list = scan::find(game, kList);
    const std::uintptr_t yesno = scan::find(game, kYesNo);
    if (terms == 0 || lobby == 0 || list == 0 || yesno == 0) {
        log::write(L"Auto login: the title screen's windows weren't found in FFXiMain.dll (terms %d, menu %d, list %d, "
                   L"yes/no %d): [client] autologin does nothing",
                   terms != 0, lobby != 0, list != 0, yesno != 0);
        return;
    }
    g_owners[kTermsStep] = reinterpret_cast<char**>(operand(terms + kTermsAt));
    g_owners[kLobbyStep] = reinterpret_cast<char**>(operand(lobby + kLobbyAt));
    g_owners[kListStep] = reinterpret_cast<char**>(operand(list + kListAt));
    g_owners[kYesNoStep] = reinterpret_cast<char**>(operand(yesno + kYesNoAt));
    for (char** owner : g_owners)
        if (!in_game(reinterpret_cast<std::uintptr_t>(owner))) {
            log::write(L"Auto login: the title screen's windows aren't as expected: [client] autologin does nothing");
            return;
        }
    g_found = true;
    log::write(L"Auto login: the title screen's windows found (terms +0x%X, menu +0x%X, list +0x%X, yes/no +0x%X)",
               static_cast<unsigned>(terms - g_base), static_cast<unsigned>(lobby - g_base),
               static_cast<unsigned>(list - g_base), static_cast<unsigned>(yesno - g_base));
}

// The step's owner while its window is open, else nullptr
char* open_owner(Step step)
{
    char* owner = read<char*>(g_owners[step]);
    if (owner == nullptr || read<std::uintptr_t>(owner + kControl) == 0)
        return nullptr;
    return owner;
}

// The owner's key handler, if slot 6 is the one expected
KeyDown key_handler(char* owner, const std::uint8_t* start, std::size_t size)
{
    const auto vtable = read<std::uintptr_t>(owner);
    if (!in_game(vtable))
        return nullptr;
    const auto handler = read<std::uintptr_t>(reinterpret_cast<const char*>(vtable) + kKeySlot * sizeof(std::uintptr_t));
    std::uint8_t code[16] = {};
    if (!in_game(handler) || size > sizeof code || !peek(reinterpret_cast<const void*>(handler), code, size) ||
        std::memcmp(code, start, size) != 0)
        return nullptr;
    return reinterpret_cast<KeyDown>(handler);
}

// True when the step's window has been open kSettle, still waiting for its answer (its flag 0)
bool ready(Step step, char* owner, std::ptrdiff_t flag, ULONGLONG now)
{
    if (owner == nullptr) {
        g_open_since[step] = 0;
        return false;
    }
    if (g_answered[step] || read<int>(owner + flag) != 0)
        return false;
    if (g_open_since[step] == 0)
        g_open_since[step] = now;
    return now - g_open_since[step] >= kSettle;
}

void answered(Step step, ULONGLONG now)
{
    g_answered[step] = true;
    if (step == kListStep) {
        g_listed_at = now;
        log::write(L"Auto login: character %d picked", g_character);
    } else {
        log::write(L"Auto login: %ls", kStepNames[step]);
    }
    if (step == kYesNoStep)
        g_done = true;
}

bool shift_held()
{
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"autologin", 0, ini) != 0;
    const int character = static_cast<int>(GetPrivateProfileIntW(L"client", L"autologin.character", 1, ini));
    g_character = character < 1 ? 1 : character > kMostCharacters ? kMostCharacters : character;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (!g_on || g_done)
        return;
    find();
    if (!g_found)
        return;
    const ULONGLONG now = GetTickCount64();
    // a character in the world (logged in by hand, or by this): a logout back to the title screen is the player's
    if (now >= g_next_world_check) {
        g_next_world_check = now + 1000;
        if (!game::local_player_name().empty()) {
            g_done = true;
            return;
        }
    }
    bool any_open = false;
    for (int step = 0; step < kSteps; ++step)
        any_open = any_open || open_owner(static_cast<Step>(step)) != nullptr;
    if (any_open && shift_held()) {
        stop(L"Shift held at the title screen");
        return;
    }

    if (char* owner = open_owner(kTermsStep); ready(kTermsStep, owner, kTermsFlag, now)) {
        if (const KeyDown key = key_handler(owner, kTermsKey, sizeof kTermsKey)) {
            key(owner, kEnter, 1); // Accept
            answered(kTermsStep, now);
        } else {
            stop(L"the terms window's key handler isn't as expected");
        }
        return;
    }
    if (char* owner = open_owner(kLobbyStep); ready(kLobbyStep, owner, kLobbyFlag, now)) {
        if (read<int>(owner + kLobbyOn) == 0) {
            stop(L"Select Character is off: no character on this account?");
            return;
        }
        if (const KeyDown key = key_handler(owner, kLobbyKey, sizeof kLobbyKey)) {
            key(owner, kPoint, 1); // the cursor on Select Character, then Enter
            key(owner, kEnter, 1);
            answered(kLobbyStep, now);
        } else {
            stop(L"the title menu's key handler isn't as expected");
        }
        return;
    }
    if (char* owner = open_owner(kListStep); ready(kListStep, owner, kListFlag, now)) {
        const char* character = owner + kListFirst + static_cast<std::ptrdiff_t>(g_character - 1) * kListStride;
        if (read<int>(character + kListEnabled) == 0) {
            log::write(L"Auto login: stopped (character %d isn't in the list, or can't be played)", g_character);
            g_done = true;
            return;
        }
        if (const KeyDown key = key_handler(owner, kListKey, sizeof kListKey)) {
            key(owner, kPoint, g_character); // the cursor on it (the camera goes to it), then Enter
            key(owner, kEnter, g_character);
            answered(kListStep, now);
        } else {
            stop(L"the character list's key handler isn't as expected");
        }
        return;
    }
    if (char* owner = open_owner(kYesNoStep); ready(kYesNoStep, owner, kYesNoFlag, now)) {
        if (read<int>(owner + kYesNoMessage) != kPlayMessage)
            return; // another question (the game's own): the player's
        if (!g_answered[kListStep] || now - g_listed_at > kYesNoWithin) {
            stop(L"a character was picked by hand");
            return;
        }
        if (const KeyDown key = key_handler(owner, kYesNoKey, sizeof kYesNoKey)) {
            key(owner, kEnter, 1); // Yes
            answered(kYesNoStep, now);
        } else {
            stop(L"the Yes / No window's key handler isn't as expected");
        }
    }
}

} // namespace vanacore::autologin
