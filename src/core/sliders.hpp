#pragma once

#include <cstdint>

// [client] sliders.*: the game's sliders (the Config windows' bars: Window Settings' lines and width, the volumes, ...),
// drawn from its "menu    gauge" picture: a bar with a half dot at each end in the window's frame, and a fill stretched to
// the value over it. sliders.style=flat draws each as VanaCore's own instead: a bar sliders.height thick in
// sliders.track.color (sliders.track.opacity), rounded with sliders.round, filled to the value in sliders.fill.color
// (sliders.selected.color on the one the cursor is on), a knob at the value (sliders.knob: none, round, square, bar;
// sliders.knob.size, sliders.knob.color), and its ends (sliders.ends: none, game for the game's half dots, round, square,
// bar; sliders.ends.size, sliders.ends.color). The game still moves the value: its mouse and keys are as they were.
// /vana Game Options > Sliders.
namespace vanacore::sliders {

// `ini`: [client] sliders.* read now, and again whenever the settings change.
void reload(const wchar_t* ini);

// Whether the flat style is on: the menu picture drawing (itemslot.cpp) and the shape drawing (buttons.cpp) hooked for it.
bool active();

// Around each draw of a shape (buttons.cpp's hooks of the game's shape drawing): its sliders found from its quads (a bar
// between two end halves), so on_quad knows each of their parts.
struct Shape {
    explicit Shape(const void* shape);
    ~Shape();
    Shape(const Shape&) = delete;
    Shape& operator=(const Shape&) = delete;

private:
    bool scanned_;
};

// Around a game window's draw (itemslot.cpp's enter_window and leave_window): its sliders kept while it's drawn; at leave,
// the knob of each that had no fill drawn (a value of 0) drawn at its start.
void enter_window();
void leave_window();

// Once a frame on the game's thread: sliders kept outside any window dropped.
void on_frame();

// From itemslot.cpp's hook of the game's drawing of one menu picture: true if it was a slider's part, drawn here as the
// flat style has it (or left out), else false for the game to draw it.
bool on_quad(const char* quad, int x, int y, float sx, float sy, std::uint32_t colour);

// The Direct3D device the game's texture of `quad` is on (the texture's Direct3D texture at +40h, else +44h), not AddRef'd;
// nullptr while the quad has no texture yet. For a picture of VanaCore's own made on the same device (buttons.cpp's too).
void* device_of(const char* quad);

} // namespace vanacore::sliders
