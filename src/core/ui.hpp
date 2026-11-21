#pragma once

#include <string>

#include "text.hpp"

// Text boxes and filled rectangles addons put on the screen, drawn by the overlay every frame in
// the order they were made. Only used from the game's thread.
namespace vanacore::ui {

struct Box {
    int id = 0;
    std::string owner; // the addon that made it
    std::wstring text;
    text::Style style;
    int x = 0; // top-left corner (top-right when `right`), in back buffer pixels
    int y = 0;
    bool right = false; // x is the box's right edge, and its lines line up on the right
    bool visible = true;
    bool placed = false; // widgets.hpp has it at a nameplate or a point in the world: not drawn at (x, y)
    float opacity = 1.0f; // 0-1: all of it drawn this opaque, on top of its colour's own (a disabled row, half there)
    // A text box drawn as a wave, as one string (vana.ui.text's wave, wave_length and wave_phase): each column of its picture
    // moved up or down by wave * sin(2 pi x / wave_length + wave_phase) pixels. 0 swing or length: flat.
    float wave = 0.0f;
    float wave_length = 0.0f;
    float wave_phase = 0.0f;
    // A text box dissolved (vana.ui.text's dissolve and dissolve_edge): 0 whole to 1 gone, its picture eaten away in a noise with
    // a burning edge along the front in dissolve_edge (r, g, b 0-255, a 0-1; no alpha, no edge).
    float dissolve = 0.0f;
    float dissolve_edge[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    text::Sprite sprite; // text and style as last drawn
    // A filled rectangle instead of text (vana.ui.rect): width x height at (x, y) in style.background.
    bool rect = false;
    int width = 0;
    int height = 0;
    int radius = 0; // a rect's corners rounded, antialiased (drawn as a picture of it; half the height: a pill)
    // A picture instead of text (vana.ui.image): `file` drawn at width x height (0 x 0 its own size;
    // one of them 0 keeps its shape), repeat_x x repeat_y times across it, tinted by style.background (its
    // colour times the picture's, its opacity the picture's).
    bool image = false;
    std::wstring file;
    int repeat_x = 1;
    int repeat_y = 1;
    std::wstring loaded; // the file and size last loaded

    // While the cursor is over it (or a press that began over it is held), the game's DirectInput
    // mouse reads no buttons and no wheel (input.cpp): the /vana window's clicks don't turn the camera.
    bool block_mouse = false;
    // Drawn over every box of a lower layer, and in creation order within its own (Windower addons' text objects are
    // layer 1, over their primitives at 0, as Windower draws them).
    int layer = 0;
};

// A new, empty box for addon `owner`, with the default style.
Box& create(const std::string& owner);
// The box with `id`, or nullptr once it's destroyed.
Box* find(int id);
void destroy(int id);
// Destroys every box addon `owner` made (it's unloading).
void destroy_owned(const std::string& owner);
// The box's text and style rendered, so its size is known.
void refresh(Box& box);
// ... and for a placed box (widgets.hpp), which is always a picture: a rectangle too, rounded or not
void refresh_placed(Box& box);

// Draws the visible boxes. Between text::begin and text::end.
void draw(void* device);
// Frees every box's texture (the device is changing).
void release();

// True when (x, y), in back buffer pixels, is over a visible box marked block_mouse, or over an
// ImGui window that takes the mouse (gui.hpp), as last drawn. Any thread.
bool blocks_mouse(int x, int y);
// ... the boxes alone (drawn over ImGui's windows, so the first to have a press there).
bool boxes_block_mouse(int x, int y);

// The size VanaCore's interface is laid out in, as of the last frame (0 before the first): the frame's
// drawn size (the window's, with renderer=d3d12 and [graphics] resolution=window), else the game's
// back buffer's. Boxes, blocks_mouse and addons' vana.ui work in these pixels.
void set_screen(unsigned width, unsigned height);
void screen(unsigned& width, unsigned& height);

// The game's back buffer size as of the last frame (0 before the first): what plugins draw in and
// the game's own pixels.
void set_game_screen(unsigned width, unsigned height);
void game_screen(unsigned& width, unsigned& height);

} // namespace vanacore::ui
