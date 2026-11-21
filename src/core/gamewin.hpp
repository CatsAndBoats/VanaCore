#pragma once

#include <cstdint>
#include <string>
#include <vector>

// vana.gameui.window, windows and press (docs/addons/07-game-interface.md): any of the game's windows read as it stands, and a
// row of one picked as the player would pick it, for an addon that draws the game's menus its own way while the game keeps
// running them (YunaUI). With vana.gameui.hide('<layout>') the window is drawn into nothing (gameui::veiled): its draw still
// runs, so its text and pictures are read from it, and its rows still take the keys.
//
// What's read:
// - The window: the menu manager's (CTkMenuMng, VA 0x105ECB98: +0 its list of windows, +0x54 the window the keys go to). A
//   window +4 its layout (name 16 characters at +0x46, "menu    menuwind"), +0x10 its state (0xE closing), +0x14 its pieces,
//   +0x3A..+0x40 its rectangle (left, top, right, bottom, shorts, the interface's units), +0x4C the cursor's row, +0x58 the
//   rows the cursor goes over, +0xB8 how far its pieces are moved down (opening). Seen live on the main menu, 2026-11-05.
// - Its rows: its pieces of type 1 (a button; +0 the type), each +4 its shape, +0x10 its state (5 greyed out), +0x2A..+0x30
//   its place and size, +0x3A its id (the cursor's and gameui.on's item), +0x3C 1 when the window's draw draws it. The label
//   is no text: the menu resource places a sprite of the "font    font" sheet for each letter in the button's shape (+4 the
//   first of its quads, a node +0 the next, +0x10 the quad; a quad +0x32 its texture's name, +0x50 four corners 0x1C apart
//   with u, v at +0x14, +0x18; +0xC0 and +0xC8 its corners' own x and y, shorts), each letter twice (a faint shadow 2 right
//   and down first): chatfont's table of the sheet's sprites names each.
// - The text and pictures its last draw made, when an addon asked for it in the last few seconds or watches it: the game's letter
//   printer's letters (chatfont.cpp's hook: lists, values, settings) and the quads of its menu pictures (itemslot.cpp's hook:
//   the labels' letters, icons, on/off boxes), joined into runs of text. The printer's letters are the font sheet's cells,
//   given back as the game's Shift-JIS by the game's own conversion (FFXiMain's 0x1011CA80) undone.
namespace vanacore::gamewin {

// A rectangle in the interface's units (the game's own, FFXiMain's interface area): left, top, right, bottom
struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
};

// One of a window's buttons: the main menu's items, a Config page's, a list's visible lines
struct Row {
    int id = 0;            // the game's number for it: the cursor's, gameui.on's e.item, press's row
    Rect rect;
    std::string text;      // its label as the menu resource gives it ("Status"); "" for one with none (a list's rows)
    int state = 0;         // the game's state for it (+0x10)
    bool cursor = false;   // the cursor is on it
    bool disabled = false; // greyed out (state 5)
    bool drawn = true;     // the window's draw draws it (false: the menu draws it itself, or it isn't shown)
};

// A run of text the window's last draw made, in Shift-JIS as the game's text is (ASCII as it is), and its colour as the game
// gives it (0xAARRGGBB, 0x80 for full: the game draws twice as bright)
struct Line {
    std::string text;
    Rect rect;
    std::uint32_t colour = 0;
};

// A picture the window's last draw made that isn't a letter or a button's bar: its texture's name ("menu    keytop")
struct Picture {
    std::string name;
    Rect rect;
    std::uint32_t colour = 0;
};

struct Window {
    std::string name;      // the layout's name after "menu    ", trailing spaces off ("menuwind")
    Rect rect;
    int cursor = 0;        // the id of the row the cursor is on
    int count = 0;         // the rows the cursor goes over
    bool focused = false;  // the window the keys go to
    bool closing = false;
    bool hidden = false;   // an addon hides it by its layout's name: drawn into nothing
    bool drawn = false;    // lines and pictures are from a draw of it (not yet, the first time a window is asked for)
    std::vector<Row> rows;
    std::vector<Line> lines;
    std::vector<Picture> pictures;
};

// The windows up, as the menu manager lists them, by name ("menuwind")
std::vector<std::string> names();

// The first window up whose layout is `name` ("menuwind"; "menu    menuwind" too), read; false when none is up. Asking for a
// window has its draws kept for a few seconds (lines, pictures): the first read of one shows what its next draw has.
bool read(const std::string& name, Window& out);

// The player's choice made for them in the window `name`: its cursor put on row `row` (0: where it is) and `event` given to it
// as the keys give it (vana.lua's numbers: 1 up, 2 down, 3 right, 4 left, 5 select, 6 cancel; 0 none), through the game's own
// key input, so the window's sound and gameui.on's triggers are as for the keys. Busy: the window isn't taking keys now (its
// +0x5C off, or +0x56 frames to wait), and nothing was done.
enum class Press { Done, NotUp, NoRow, Busy, NotFound };
Press press(const std::string& name, int row, int event);

// vana.gameui.watch (lua/vana.lua keeps the functions): the windows the addon of `key` (gameui::key's) watches, by name
// ("configwi"; "*" every one; none: it watches none). A watched window's draws are kept for as long as it's watched, and
// after a draw of it the addon hears 'gameui_window' in the same frame (addons' frame, at Present, comes after the game's
// windows are drawn): e.element its name, e.what 'open' (its first draw since it opened, or since it was watched), 'change' (a
// draw that shows anything else: its place, cursor, rows, text or pictures) or 'close' (once, when it's closing or gone).
void watch(const std::string& key, const std::vector<std::string>& names);
void release(const std::string& key); // the addon goes (gameui::attach, release): it watches none

// Once a frame, before the addons' (overlay.cpp): watched windows that closed or went told
void on_frame();

// ---- the hooks' ----

// Around the game's draw of one window (chat.cpp's window draw): its text and pictures kept when an addon has asked for it,
// and the draw made into nothing when an addon hides it by its layout's name
class Drawing {
public:
    explicit Drawing(const void* ctrl);
    ~Drawing();
    Drawing(const Drawing&) = delete;
    Drawing& operator=(const Drawing&) = delete;

private:
    const void* ctrl_ = nullptr;
    bool kept_ = false;
    bool dropped_ = false;
};

// chatfont.cpp's hook of the game's letter printer: letter `code` (the font sheet's cell) at pen x, y, sx x sy times 16 pixels
void letter(int code, int x, int y, float sx, float sy, std::uint32_t colour);

// itemslot.cpp's hook of the game's quad draw: a quad of a menu picture drawn at x, y, its own corners times sx, sy
void quad(const char* quad, int x, int y, float sx, float sy, std::uint32_t colour);

// itemslot.cpp: the quad hook is wanted (an addon has read a window)
bool wants_quads();

} // namespace vanacore::gamewin
