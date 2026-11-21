#pragma once

#include <cstdint>

// The GM icon before a staff member's name over their head, while they're in GM mode: CatsEyeXI's own pictures
// (assets\gm\*.png, built into VanaCore.Core.dll) in place of the game's, by whose name it is (gmicons_staff.h's GM_LIST,
// by the server's host: dev, admin, sgm; anyone else gm). On a server GM_LIST hasn't, and with [graphics]
// nameplates.gm=0, the game's. Drawn where the game draws its icon, through nameplates.cpp's hooks on the names' letters;
// all on the game's thread.
namespace vanacore::gmicons {

// The game's call drawing a letter's quad (its renderer's DrawPrimitiveUP, __thiscall)
using DrawFn = std::uint32_t(__fastcall*)(void* renderer, void* edx, unsigned prim, unsigned count, const void* vertices,
                                          unsigned stride);

// While a player's name is drawn by the game: `text` as the game has it (its icons, a space, its letters) for `entity`,
// whose GM level says whether it's in GM mode (not its icon's glyph: a mentor's is the same as GM levels 1-2) and whose
// letters choose the picture. nullptr (not a player's, or nameplates.gm off): the game's icons.
struct Name {
    Name(const char* text, const void* entity);
    ~Name();
    Name(const Name&) = delete;
    Name& operator=(const Name&) = delete;
};

// The glyph the game is setting the next letter's texture for
void note(unsigned code);

// How it's drawn (nameplates.gm.size, .x, .y): times the game's icon's height, and moved by x and y of the game's pixels
struct Look {
    float size = 1.0f;
    float x = 0.0f, y = 0.0f;
};

// The quad of an icon before the name (the game's letter vertices): when it's the GM icon of a name with a pick, the
// picture drawn in its place by `draw`, square on the screen, its right side at the game's icon's, and true (the game's
// isn't drawn)
bool draw(void* renderer, DrawFn draw, unsigned prim, unsigned count, const void* vertices, unsigned stride, const Look& look);

// [graphics] nameplates.gm as the settings have it (graphics.cpp): the Target window's mark goes with it
void enable(bool on);
bool enabled();

// The Target window's mark of its target's status, about to be drawn by the game at (x, y) in the interface's units for
// `entity`: `shape` one of the 16 x 16 shapes of the status icons (`shapes`, by their code less 0x20). True when it's the
// entity's GM mark (in GM mode, on a server GM_LIST has) and their staff picture is to be drawn in its place by draw_mark:
// the game's is left out.
bool target_mark(const void* entity, const void* shape, const void* const* shapes, int x, int y);
// In VanaCore's interface pass (the window `width` x `height`): the picture target_mark took this frame, in the mark's cell
void draw_mark(void* device, unsigned width, unsigned height);

} // namespace vanacore::gmicons
