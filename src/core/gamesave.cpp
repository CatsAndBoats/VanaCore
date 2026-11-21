#include "gamesave.hpp"

#include <windows.h>

#include <cstdint>

#include "addons.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::gamesave {
namespace {

// The save (cdecl, no arguments; VA 0x101A9F30, one match in the running game 2026-11-12): three of the game's managers,
// each asked to save if it's there, then a jump to an empty function. The first is the user files' (its save-all,
// 0x101B8750, writes every file registered with it whole, with its MD5: mcr.sys is the 4 bytes at the macro
// controller's +0x1DBC, book * 20 + set * 2 + palette), and every write is done before it returns.
//   mov ecx,[user files]; test ecx,ecx; je +5; call <save all>; the same for two more managers; jmp <ret>
constexpr char kSaveAll[] = "8B 0D ?? ?? ?? ?? 85 C9 74 05 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E8 ?? ?? ?? ?? "
                            "8B 0D ?? ?? ?? ?? 85 C9 74 05 E8 ?? ?? ?? ?? E9";

using SaveAll = void(__cdecl*)();

// A fault in the game's code doesn't stop the game being ended
bool guarded(SaveAll save)
{
    __try {
        save();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool run()
{
    if (!addons::is_game_thread()) {
        log::write(L"Game save: not on the game's thread: the game's own files (the macro book and set in use) aren't saved");
        return false;
    }
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t save = game != nullptr ? scan::find(game, kSaveAll) : 0;
    if (save == 0) {
        log::write(L"Game save: the game's save of its user files wasn't found in FFXiMain.dll: the macro book and set in "
                   L"use aren't saved");
        return false;
    }
    const bool done = guarded(reinterpret_cast<SaveAll>(save));
    log::write(done ? L"Game save: the game's own files saved (the macro book and set in use, mcr.sys), as its logout does"
                    : L"Game save: the game's save of its user files faulted: the macro book and set in use may not be saved");
    return done;
}

} // namespace vanacore::gamesave
