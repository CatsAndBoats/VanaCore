#pragma once

#include <imgui.h>

// The /vana window's look for VanaCore's own ImGui windows (/vana stats, /vana graph, the addon detail): vana\draw.lua's
// colours (M.P), Segoe UI, its switches, buttons, meters and search box, and its colours on ImGui's own widgets. Sizes are
// draw.lua's, taken as at ImGui's font size (18 px, Agave): S(16) is 18.
//
// A window here: push_theme(), begin_window(name), title_bar(icon, title, command, open), what's in it, end_window(shown),
// pop_theme().
namespace vanacore::vanaui {

namespace P {
constexpr unsigned bg = 0x161822, side = 0x10121A, border = 0x2A2E3E, line = 0x242838, well = 0x0E1017, well_edge = 0x272B3C,
                   search = 0x080A10, search_edge = 0x3E4460, hover = 0x1F2334, text = 0xE8EAF1, white = 0xFFFFFF,
                   text2 = 0xBDC1D0, dim = 0x7B8095, faint = 0x4E536A, accent = 0x7AA2F7, accent2 = 0xA9C1FF,
                   accent_lo = 0x3E5CB3, accent_bg = 0x1B2340, teal = 0x5FD3B3, cyan = 0x4FD6FF, amber = 0xE8B45C,
                   rose = 0xEE6F78, green = 0x9ECE6A, knob = 0xF2F3F8, off = 0x2B2F42, seg = 0x313752, ink = 0x0B1020,
                   purple = 0xC9AEF7, yellow = 0xF3E38A, popup = 0x0F1118, grab = 0x3A4158, grab_hi = 0x4A5270;
} // namespace P

constexpr float kBase = 16.0f; // the size S() takes as ImGui's font size
constexpr float kBody = 19.0f, kSmall = 16.0f, kText = 18.0f, kBig = 30.0f; // the text sizes, a little over draw.lua's
constexpr float kPad = 18.0f, kTitleH = 46.0f;                                // the window's margin and its top line

// Font Awesome's, in ImGui's own font (gui.cpp)
extern const char* const kIconStats;  // chart-simple, the /vana window's Stats
extern const char* const kIconGraph;  // chart-line, its Graphs
extern const char* const kIconAddon;  // puzzle-piece
extern const char* const kIconClose;  // xmark
extern const char* const kIconCheck;  // check
extern const char* const kIconSearch; // magnifying-glass
extern const char* const kIconLeft;   // chevron-left
extern const char* const kIconRight;  // chevron-right
extern const char* const kIconDown;   // chevron-down
extern const char* const kIconSave;   // floppy-disk
extern const char* const kIconTrash;  // trash
extern const char* const kIconPause;  // pause
extern const char* const kIconPlay;   // play
extern const char* const kIconDot;    // not a glyph: button() draws a dot (Font Awesome's circle is a ring)

extern float g_scale; // S(16) is ImGui's font size: worked out as each window begins (push_theme)

float S(float n);
ImU32 u32(unsigned rgb, float alpha = 1.0f);
ImVec4 vec4(unsigned rgb, float alpha = 1.0f);
// the colour `t` of the way from `a` to `b`
unsigned mix(unsigned a, unsigned b, float t);
// A value eased toward `target` a frame at a time, kept in the window by `key` (draw.lua's M.ease)
float ease(const char* key, float target, float speed = 18.0f);

// Segoe UI and its semibold, as the /vana window's: added the first time a window here draws, used from the next frame
// (ImGui takes a new font up between frames). Without them (Wine has no Segoe UI) ImGui's own font draws the text.
struct Fonts {
    int frame = -1; // the frame they were added in
    ImFont* text = nullptr;
    ImFont* semi = nullptr;
};
extern Fonts g_fonts;

void load_fonts();
ImFont* ready(ImFont* font);
// `font` (nullptr: the one in use) at draw.lua's `size`
void push_font(ImFont* font, float size);
// ImGui's own font, with Font Awesome merged in: the icons
void push_icons(float size);
float text_width(const char* text, ImFont* font, float size);
// Text with its line centred on cy, lifted a little as draw.lua's (Segoe UI's line has a tall ascent)
void text_mid(ImDrawList* dl, float x, float cy, const char* text, unsigned rgb, ImFont* font, float size, float alpha = 1.0f);
void text_right_mid(ImDrawList* dl, float right, float cy, const char* text, unsigned rgb, ImFont* font, float size);
void icon(ImDrawList* dl, float cx, float cy, const char* glyph, unsigned rgb, float size, float alpha = 1.0f);

// The /vana window's colours and shapes on ImGui's own widgets (tables, trees, tabs, scroll bars, tooltips), pushed
// before a window here begins and popped after it ends
void push_theme();
void pop_theme();

// A window here: no title bar of ImGui's (title_bar draws it), the page's margin round what's in it
bool begin_window(const char* name, ImGuiWindowFlags flags = 0);
// The window's top, as the /vana Debug window's: its icon and name, the command at the right and the close button, a
// hairline under it. Dragged by it (by any place in the window with nothing on it). What's under it is in a child, so it
// scrolls under the top line; end_window ends both.
void title_bar(const char* glyph, const char* title, const char* command, bool& open);
void end_window(bool shown);

// draw.lua's M.switch with its label after it; returns whether it was flipped
float toggle_width(const char* label);
bool toggle(const char* id, const char* label, bool& on, float height = 30.0f);

// draw.lua's M.button: a border (ghost), filled (accent), or a wash of rose or green with a border of it
enum class Kind { ghost, accent, danger, success };

float button_width(const char* label, const char* glyph, float height = 26.0f);
// `glyph_rgb` (0: the label's) and `glyph_alpha` colour the icon on its own (Record Stats Report's dot); `min_width` keeps
// a button as wide while its label changes
bool button(const char* id, const char* label, const char* glyph, Kind kind, float height = 26.0f, unsigned glyph_rgb = 0,
            float glyph_alpha = 1.0f, float min_width = 0.0f);

// A table's header row: semibold, small, dim, as the /vana window's column heads
void headers_row();
// A thin bar across its table cell, `fraction` of it, on the line's middle (draw.lua's M.meter)
void meter(double fraction, ImU32 fill);

// A reading's column, as the Graphics page's graphics card row: the name small and dim at the left, the value at the
// right, a 4 px bar under them, `fraction` of its width (amber near the limit, rose at it or when `hot`). `cy` is the
// middle of a 40 px row.
void reading_column(ImDrawList* dl, float x, float cy, float width, const char* name, const char* value, double fraction, bool hot);

} // namespace vanacore::vanaui
