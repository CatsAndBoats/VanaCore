#pragma once

#include <cstdint>

// What nameplates.cpp keeps for finding out why a name came out wrong: how the game draws its letters, and the sharp names'
// sizes and places frame by frame.
namespace vanacore::nameplates::debug {

// What the game draws the letters with, logged the first few times it's seen different: if a name comes out wrong, what its
// draw was. `colour` and `depth` are the letter's vertices'.
void note_letter_state(void* device, std::uint32_t colour, float depth, const char* text);

// [graphics] nameplates.trace: the sharp names' sizes and places, every frame for a while, kept in memory and written at the
// end (a file write a name a frame would cost frames their time). trace_frame is called each frame with the setting, which
// starts a trace when it changes to some seconds and writes it when they're up.
void trace_frame(float asked);

// The name being drawn sharp, while tracing: where the game puts it and where and how big VanaCore draws it, in the screen's
// pixels
struct Row {
    const void* actor;
    const char* text;
    float size, exact, picture_px, scale;
    float at_x, at_y, left, top, width, height;
};
void trace_row(const Row& row);

} // namespace vanacore::nameplates::debug
