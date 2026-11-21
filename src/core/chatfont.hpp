#pragma once

// True Font: the interface's letters drawn from a TrueType font at the screen's own pixels, instead of the game's 16-pixel
// font sheet made larger with the interface. Each area has its own switch, size, font and style ([client] <area>.font=sharp,
// <area>.font.size in the interface's pixels, 0 its letters left out, <area>.font.family, empty for the default font,
// <area>.font.bold and .italic): the chat's rows (chat.font,
// and chat.font.spacing: the interface's pixels more between letters), the menus and lists (menu.font), the Merit Points
// menu's lists (lists.font, Menu Lists: each key menu.font's until it's set; their buttons the main menu's), the item lists
// (items.font), an item's details (tips.font, and tips.font.lineheight: its rows that far apart, 16 the game's, its box made
// taller to hold them), the help (help.font), the chat input line (input.font), the auto-translate list (phrases.font), the
// menus' buttons (buttons.font), the other
// labels, the windows' titles (titles.font: each of its keys buttons.font's until it's set), the main menu's buttons apart
// from every other menu's (mainmenu.font: its windows "menuwind" and "socialme"; each key buttons.font's until set), the
// Action Menu's apart too (action.font: "playermo" and its lists; the same), the Target window's name (target.font: the name
// plates' big letters, each key status.font's until set) and its "Target" title (target.title.font, each key titles.font's until set) and the Status window's numbers
// and words (status.font) and its player name (status.name.font, its own keys alone: drawn whole, statuswindow_name.h); those follow menu.font's switch until they're set. The party list's and the alliance lists' names
// and numbers (party.font, alliance.font: the name plates' big letters, each key status.font's until it's set). The default font is
// [client] truefont.family, .outline and .outline.opacity (a black outline that wide round the letters).
// VanaCore renders each area's glyphs (text::Sprite) at the size the interface is laid onto the screen, and the graphics layer
// draws them where the game draws its own (d3d8_chatfont.h). While the game wraps a line for the chat (its add-line) and
// draws a chat window, its table of letter widths holds the chat font's own, so the lines are laid out by the font's
// spacing; the other areas' letters are each centred in the game's width for it, so no window's layout moves. ASCII only:
// every other character is the game's. The chat input line is drawn whole instead (chatline.cpp).
namespace vanacore::text {
struct Style;
}

namespace vanacore::chatfont {

// `ini` (the settings file): [client] truefont.* and the areas' *.font* read now, and again by reload whenever it changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: the glyphs made again when the font or the chat's size on the screen changed.
void on_frame();

// The font's widths in the game's table from the first hold to the last release (nested: the game's add-line runs inside
// itself), the game's again after. chat.cpp holds around its add-line and draw hooks.
void hold(bool on);

// Around the game's drawing of a chat window: the widths held, and the graphics layer told it's the chat's text it draws.
struct Drawing {
    Drawing();
    ~Drawing();
    Drawing(const Drawing&) = delete;
    Drawing& operator=(const Drawing&) = delete;
};

// Around the game's drawing of a window (chat.cpp's window draw), its layout (a window's +4; null none): the font sheet's text
// in it in its area's font, by the layout's name ("menu    inventor": the item lists', "menu    iteminfo": the help's), the
// menus' for the rest
struct Area {
    explicit Area(const char* layout);
    ~Area();
    Area(const Area&) = delete;
    Area& operator=(const Area&) = delete;

private:
    int was_;
};

// The layout (a window's +4; null none) is an item's details' ("menu    item<n>inf" and the like: the tooltips' area)
bool details_window(const char* layout);

// The layout (a window's +4; null none) is the main menu's or one it opens in its place (mainmenu.font's windows:
// buttons.cpp's mainmenu.buttons.* go by them too)
bool main_menu_window(const char* layout);

// The layout (a window's +4; null none) is the Action Menu's or one of its Magic and Abilities lists (action.font's windows:
// buttons.cpp's action.buttons.* go by them too)
bool action_menu_window(const char* layout);

// True Font draws the menus' labels (buttons.font or titles.font on): buttons.cpp then tells the graphics layer when a button's
// shape is drawn, so its letters are the buttons' and the rest the titles'
bool labels_on();

// For reading the game's text (gamewin.cpp): the game's own width for a letter of its font sheet (`code` its cell), the
// interface's pixels, -1 while its table isn't found; and the character a sprite of the "font    font" sheet is (the menus'
// labels, the name plates' big letters), by the sprite's place on the sheet (u, v: its texels, from the top left), 0 for none.
int game_width(int code);
char label_char(int u, int v);

// A menu label's letter alone (not the name plates' big ones), by its sprite's place on the "font    font" sheet: the
// character, and how far below the sprite's top its baseline is (the sheet's texels: a label is drawn a texel to a unit).
// False for any other sprite. labeltext.cpp reads a window's labels by it.
bool label_letter(int u, int v, char& c, int& base);

// How wide True Font draws `text` in `area` (VANA_FONT_*): its letters' advances at its size, the interface's pixels (a
// window's words are laid by them from where the game's start); -1 while that area isn't on. equipstats.cpp fits its labels by it.
float text_width(int area, const char* text);

// Whether `area` (VANA_FONT_*) is True Font's now (on and built): profilewin.cpp switches its titles' letters to theirs only then
bool area_on(int area);

// The chat input line's True Font as built (input.font on, its letters not left out): its letters' style at the screen's size,
// the same with its outline's stroke (0: no outline), and whether its letters are capitals. False while it's off.
bool input_style(text::Style& letters, text::Style& stroked, bool& upper);

// The same for any area (VANA_FONT_*), and its outline's opacity (0 to 1): the Target window's name drawn whole (targetwin.cpp)
bool area_style(int area, text::Style& letters, text::Style& stroked, bool& upper, float& outline_opacity);

} // namespace vanacore::chatfont
