#pragma once

#include <cstdint>

// The Config windows' labels as VanaCore's own text: a window's title ("Misc. 2"), its settings' names ("Clipping Plane",
// "Min", "Max") and its options' labels ("ON", "Compact 1"), each one whole text in True Font, drawn in VanaCore's interface
// pass where the game puts its own, as the macro bar's names and keys' labels are (macrotext.cpp).
// Before, the game's letters were kept as it drew them (a sprite of the "font    font" sheet each) and True Font's laid in their
// place, a word at a time: names came apart before a letter that hangs below the line ("St yle Lock", "Footste p Effects",
// "Cli ppin g Plane"), more of them with the cursor near (reported 2026-11-12, the cause not looked for: "can we update
// these to be proper text printed and not texture replacement?").
//   A label is read off the shape the game is about to draw (buttons.cpp's shape hooks): the window's frame holds its title
//   and its settings' names among its background, bullets and wells, an option's button its label twice (a shadow, taken away
//   from what's under it, then the letters). The letters on one line, each beside the last, are a label; a gap of 3 units or
//   more is a space.
//   Its letters are left out of the game's draw one by one (itemslot.cpp's picture hook), and the text kept with the draw's
//   place and colour (the cursor's option gold, buttons.text's colours): in the buttons' font (buttons.font) for a shape with
//   a button's bars in it, the frame's wells too, else the titles' (titles.font), as the letters were before. A font with no
//   outline gets the game's shadow under it, a black copy as far down and right.
// A label stays the game's letters (as before) while its area's True Font is off, where one of its sprites isn't a label's
// letter or isn't drawn a texel to a unit, in a shape drawn at a scale (a picked option's flash), where it doesn't stand
// whole inside its window (the game cuts what's past it), and in a window an addon hides (vana.gameui.hide). The Config
// list itself and the lists it opens (Windows, Log) are the main menu's: not taken.
namespace vanacore::labeltext {

// Around the game's drawing of a window (chat.cpp's window draw), its layout (the window's +4; null none): a Config window
// ("menu    conf...", "cfilter", "fxfilter", "textcol...") told apart, whose labels are taken. Its texts of a draw before
// are let go, and those of another window that stand under it.
struct Window {
    Window(const void* window, const char* layout);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

private:
    const void* was_;
    short was_rect_[4];
};

// Around the game's draw of `shape` (buttons.cpp's shape hooks): in a Config window, its labels read
struct Shape {
    explicit Shape(const void* shape);
    ~Shape();
    Shape(const Shape&) = delete;
    Shape& operator=(const Shape&) = delete;

private:
    bool pushed_;
};

// The game is about to draw `quad` of the shape being drawn, the shape at x, y (the interface's units) at a scale in `colour`
// (itemslot.cpp's picture hook, after buttons.cpp has set a button's label's colour and place): true for a letter of a
// label taken, which is then not drawn (its label kept to be drawn whole, at its first letter)
bool letter(const char* quad, int x, int y, float sx, float sy, std::uint32_t colour);

// The picture hook is wanted: the menus' labels' True Font is on
bool wants_quads();

// In VanaCore's interface pass (`width` x `height` the drawn pixels): the labels the game drew this frame.
void draw(void* device, unsigned width, unsigned height);

} // namespace vanacore::labeltext
