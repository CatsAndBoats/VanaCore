#pragma once

#include <string>
#include <vector>

struct ImGuiWindow;

// Addons' and plugins' ImGui windows moved and scaled from outside them (the vana window's Addons > Layout, vana.layout). Every
// addon and plugin draws in VanaCore's one ImGui context, and each top-level Begin and End goes through VanaCore (lua_imgui.cpp
// for addons, compat/ashita/gui.cpp for plugins), so a window's place and scale can be changed without its addon knowing.
//
// A window is known by its ImGui name ("Party Finder", "##pf_mini"). What's set for it is kept in config\layout.json:
//   - a window ImGui places (its addon at most gives a first place): put where it was set each time it appears, and where
//     it's dragged to after that is kept
//   - a window its addon places itself every frame: moved by an offset from where its addon puts it (it still follows what
//     it's anchored to). An addon that reads back where its window went and keeps that has taken the move as its own: the
//     offset goes.
//   - its scale: the window drawn bigger or smaller from its top-left corner, everything in it (text, pictures, what its
//     addon draws itself), with its child windows, its popups and a tooltip over it, and what it draws on ImGui's
//     foreground and background lists between its Begin and End. The addon lays it out at its own size; only the drawing
//     is scaled, and the mouse over it is taken back into its own pixels, so clicks and drags land where they're drawn.
// Game thread.
namespace vanacore::layout {

// Once, from VanaCoreInstall: config\layout.json read.
void install(const wchar_t* install_dir);

// Around ImGui::NewFrame (gui.cpp): the mouse at (x, y), over a scaled window as the last frame drew it, put into that
// window's own pixels for ImGui; then the window ImGui takes as under the mouse made the one drawn there.
void before_new_frame(float& x, float& y);
void after_new_frame();
// A mouse message's point (gui::on_mouse, between frames), put into a scaled window's own pixels as before_new_frame
// puts the frame's: a press there is ImGui's at the place it's drawn, not where it would be unscaled.
void mouse_at(float& x, float& y);

// Around ImGui::Begin(name) for an addon or plugin, `owner` who it is ("ashita/partyfinder", "vanacore/keybinds",
// "plugin/minimap"): its place before; after, what it draws from there on noted (for its scale). And before its End.
// before_begin returns ImGui window flags to add to its addon's for this Begin: a locked window's NoMove and NoResize, one
// with no pointer events' NoInputs (set_flags).
int before_begin(const char* name, const std::string& owner);
void after_begin();
// Around the ImGui::Begin of one of VanaCore's own windows its C++ draws ("VanaCore stats"): the display's pixels it shows
// are what ImGui clips it to, the same as for an addon's (a window drawn smaller than it is laid out would be cut off).
void clip_begin(const char* name);
void clip_end();
void before_end();
// An addon's SetWindowPos for window `name` (nullptr: the one it's in) with ImGui's `cond`: where to put it instead (moved
// by the window's offset).
void window_pos(const char* name, float& x, float& y, int cond);

// Each frame, from gui::end_frame before ImGui ends it: scaled windows' drawing scaled, what was drawn noted, a window
// dragged kept, the Layout tab's outline drawn, layout.json written a second after a change.
void end_frame();

// The rectangle (x0, y0)-(x1, y1) of `window`, in its own pixels, as it's drawn (scaled with the window it's part of).
void visual_rect(const ImGuiWindow* window, float& x0, float& y0, float& x1, float& y1);

// What `owner` ("ashita/fancychat") draws outside ImGui at (x, y), in its windows' own pixels (its D3DX sprites:
// gdifonts' text and rectangles, laid over its window): when that's inside one of its windows drawn scaled (the smallest,
// if more), it's drawn scaled with it, from `origin` by `scale`, as all the window's ImGui drawing is. False: as it is.
bool drawing_scale(const std::string& owner, float x, float y, float& origin_x, float& origin_y, float& scale);

// ---- On-screen objects ----------------------------------------------------------------------------------
// What an addon or plugin draws without an ImGui window (Ashita's fonts and primitives: equipmon's ammo text, a bar's
// rectangles) is one "window" here for each addon: "Text and shapes##objects/ashita/equipmon", its place the top-left corner
// of everything it has on the screen. Its X and Y move all of it by an offset; its scale draws all of it bigger or smaller from
// that corner. Kept in config\layout.json like the others.
struct ObjectGroup {
    std::string name;   // as above; the key in layout.json
    std::string owner;  // "ashita/equipmon", "plugin/minimap"
    float x = 0, y = 0; // the top-left corner of everything the owner has on the screen, where its objects put it
    float w = 0, h = 0; // and its size
};
struct ObjectMove {
    float dx = 0, dy = 0; // added to where the objects are drawn (after the scale)
    float scale = 1;      // from (x, y)
};
// Once a frame, with every owner's objects drawn in it: how each is to be moved and scaled (in the same order), and the
// windows() list has them as shown.
std::vector<ObjectMove> objects(const std::vector<ObjectGroup>& groups);

struct Window {
    std::string name;   // ImGui's, with any ## part
    std::string owner;  // as before_begin had it ("" if it hasn't been seen since the game started)
    float x = 0, y = 0; // its top-left corner, in VanaCore's interface pixels (where it was last, if it isn't shown)
    float w = 0, h = 0; // as it's drawn (its scale with it)
    float scale = 1;     // set here
    bool shown = false;  // drawn in the last frame
    bool pinned = false; // its addon places it every frame
    bool saved = false;  // layout.json has it
    bool locked = false;     // set_flags: it can't be dragged or resized
    bool no_pointer = false; // set_flags: the mouse goes through it
};

// Every window drawn in the last frame, then each layout.json has that isn't.
std::vector<Window> windows();
// Window `name` moved to (x, y) and/or scaled (a null pointer: that one as it is). The move happens at its next Begin.
void set(const std::string& name, const float* x, const float* y, const float* scale);
// Window `name` locked (the mouse can't drag or resize it; its addon, Addons > Layout and the HUD Manager still move it)
// and/or with no pointer events (the mouse goes through it and its child windows, to what's under it: the game, another
// window), a null pointer: that one as it is. Kept in config\layout.json; ImGui windows only (not text and shapes).
void set_flags(const std::string& name, const bool* locked, const bool* no_pointer);
// Forgets what's set for window `name`: its scale goes back to 1; a window its addon places goes back there, another
// stays where it is. Its lock and no pointer events stay.
void reset(const std::string& name);
// Window `name` outlined on the screen for this frame and the next (call it each frame).
void highlight(const std::string& name);

// Each frame, from gui::begin_frame: ImGui works in the game's pixels, and VanaCore's own windows (its addons', "vanacore/...",
// and /vana stats' and graph's) are drawn at `scale` (the game's pixels to an interface pixel) on top of what's set for them,
// so they keep their size in the interface's pixels.
void set_own_scale(float scale);

// Who begins window `name` (ImGui's, with any ## part), as before_begin had it ("ashita/fancychat"): "" for one no addon
// or plugin has begun (VanaCore's own its C++ draws, "VanaCore stats").
std::string owner_of(const std::string& name);

} // namespace vanacore::layout
