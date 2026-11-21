#include "zonebatch.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "../gfx/vanacore_gfx.h"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::zonebatch {
namespace {

// Why: an open zone at a long draw distance is thousands of draws a frame, the same few meshes again and again (The Sanctuary of
// Zi'Tah at 8x: 6,100 draws, 944 of them one cedar), and each costs the CPU the same whatever it draws. The renderer keeps the
// zone's draws while the game draws its zone and sends those of one mesh, texture and state as one draw for all their pieces
// (gfx_d3d12_batch.h); it needs to know where the zone's draws begin and end, which is this hook.
//
// The zone's draw (FUN_10179FD0: a thiscall on the zone renderer, DAT_105FD284, with no arguments; one caller, the renderer's
// frame): both zone slots' walks of their pieces, the pieces held back to draw last, VanaCore's caster pass at each slot's end
// (casters.cpp), and the UAS lists (FUN_101798A0, called from inside it).
//   sub esp,110h; mov eax,[104544FCh]; push ebx; push ebp; push esi; mov eax,[eax+0Ch]; xor ebx,ebx; push edi; mov ebp,ecx;
//   mov ecx,[eax]; push ebx; push ebx; push eax; call [ecx+0F4h]
constexpr char kZoneDraw[] = "81 EC 10 01 00 00 A1 ?? ?? ?? ?? 53 55 56 8B 40 0C 33 DB 57 8B E9 8B 08 53 53 50 FF 91 F4 00 00 00";
constexpr std::size_t kZoneDrawPrologue = 6; // sub esp,110h
// the renderer's moving zone (casters.cpp kMoving: a slot whose pieces move with a matrix of their own)
constexpr std::uintptr_t kMoving = 0x158;

// the game's thiscall, called as a fastcall (this in ecx; edx unused; nothing on the stack)
using ZoneDraw = void(__fastcall*)(std::uintptr_t renderer, void* edx);

// Only touched on the game's thread.
bool g_searched = false;
ZoneDraw g_zone_draw = nullptr; // the game's, through the detour's trampoline
bool g_moving = false;

void __fastcall zone_draw(std::uintptr_t renderer, void* edx)
{
    const bool batch = !g_moving && *reinterpret_cast<const int*>(renderer + kMoving) == 0;
    if (batch)
        vanacore_gfx_batch(1);
    g_zone_draw(renderer, edx);
    if (batch)
        vanacore_gfx_batch(0);
}

void install()
{
    g_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t draw = game != nullptr ? scan::find(game, kZoneDraw) : 0;
    if (draw == 0) {
        log::write(L"Graphics: couldn't find where the game draws its zone; its draws go one by one ([graphics.effects] batch)");
        return;
    }
    if (!imports::detour(reinterpret_cast<ZoneDraw>(draw), kZoneDrawPrologue, &zone_draw, g_zone_draw)) {
        log::write(L"Graphics: couldn't hook the game's zone draw; its draws go one by one ([graphics.effects] batch)");
        return;
    }
    log::write(L"Graphics: the zone's draws of one mesh sent together: the zone draw at FFXiMain.dll+0x%X ([graphics.effects] batch)",
               static_cast<unsigned>(draw - reinterpret_cast<std::uintptr_t>(game)));
}

} // namespace

void update(bool moving)
{
    g_moving = moving;
    if (!g_searched && vanacore_gfx_batch_wanted())
        install();
}

} // namespace vanacore::zonebatch
