#pragma once

#include <windows.h>

#include <cstdint>

// The macro bar's text as VanaCore's own: each macro's name and each key's label ("Ctrl1", "Alt 0") one whole text in True
// Font, drawn in VanaCore's interface pass where the game puts its own, as the chat log's tabs' names are (chattabs.cpp).
// Before, the game's letters were kept as it drew them and True Font's laid in their place (the keys' "Ctrl" / "Alt" and
// numbers known by their sprites, the names printed in the menu font for it): whatever the game drew over a letter after it
// gave that letter back to the game's font at the game's place, so with the mouse over the bar a name read "DL SS", two of
// its letters True Font's from its left and two the game's where the game's wider letters stood (reported 2026-11-12: "we
// should be using pure true font text box on these ... and not trying to do a texture replacement").
//   A macro's name (macrobar.name.font on: Macro Bar > Name Font, which follows General until it's set): taken where the palette asks for it to be printed,
//   the keytop printer (chatfont.cpp's hook) or, a name with a space or a symbol in it, the game's fitted text draw (hooked
//   here); the letters that fit the key, as the game's stop before one that would pass it.
//   A key's label (macrobar.font on): taken as the key's shape is drawn (buttons.cpp's shape hooks), its letters left out of
//   that draw (the shape's count cut to the quads before them, put back after), in the draw's own colour (the cursor's key
//   gold, a greyed one faint) or macrobar.font.color; with macrobar.prefix=0 its number alone, where the game has it.
// A name past ASCII, a key shape that isn't laid as the game's are (another menu DAT), or the area's True Font off: the
// game's own letters, as before.
namespace vanacore::macrotext {

// [client] macrobar.font.color (0xFFRRGGBB, 0 the game's) and macrobar.prefix=0 (`bare`), as chatfont.cpp read them
void look(std::uint32_t colour, bool bare);

// Once FFXiMain.dll is there (chatfont.cpp's find_table): the game's fitted text draw hooked
void install(HMODULE game);

// Around the game's drawing of a window (chat.cpp's window draw), its layout (a window's +4; null none): the macro bar's
// palettes ("menu    mcr1pall", "mcr2pall", "mcr1long", "mcr2long") and Macros > Edit's rows in their place ("mcr1edit",
// "mcr2edit", "mcr1edlo", "mcr2edlo") told apart, whose names and keys are taken
struct Window {
    explicit Window(const char* layout);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

private:
    bool was_;
};

// Around the game's draw of `shape` at x, y (the interface's units) at a scale in `colour` (buttons.cpp's shape hooks): a
// palette key's label left out of it and kept to be drawn whole
struct Key {
    Key(void* shape, int x, int y, float sx, float sy, std::uint32_t colour);
    ~Key();
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;

private:
    int* count_;
    int all_;
};

// A macro's name the game is about to print in a palette, from x standing on `baseline`, in `colour`, with `room` across:
// kept to be drawn whole (true: the game prints nothing). False outside a palette, with the names' True Font off, or for a
// name past ASCII: the game's own print.
bool name(int x, int baseline, const char* text, std::uint32_t colour, int room);

// In VanaCore's interface pass (`width` x `height` the drawn pixels): the names and labels the game drew this frame.
void draw(void* device, unsigned width, unsigned height);

} // namespace vanacore::macrotext
