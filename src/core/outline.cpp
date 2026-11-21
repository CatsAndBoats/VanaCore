#include "outline.hpp"

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gui.hpp"
#include "uispace.hpp"

namespace vanacore::outline {
namespace {

bool g_space = false; // uispace has the interface's area
std::string g_held; // the id of the box held with Shift, or empty
int g_held_frame = 0; // ImGui's frame the held box was last drawn in
ImVec2 g_grab;      // where in it it was taken (pixels from its top-left corner)
// where the mouse was pressed on it, and whether it's gone ImGui's drag threshold from there since. Till it has, the box
// isn't held as far as its owner knows: a Shift + click that doesn't drag switched the window's place on where it stood,
// so a window pinned where the game puts it (an Interaction Menu on the chat) came loose without being moved
ImVec2 g_press;
bool g_dragged = false;
int g_snap = 0;     // the grid a held box snaps to (pixels; 0 none), set by snap before each draw
bool g_snap_right = false; // its right edge to a line, not its left (snap's `right`)
std::string g_label; // the name in the next box's corner (label), or empty
std::string g_clicked; // the box last left-clicked without Shift (clicked), or empty
int g_clicked_frame = -1; // ImGui's frame it was clicked in
float g_clicked_area = 0; // its size: of boxes over each other, the smallest is the one clicked
std::unordered_map<std::string, float> g_glow; // each box lit by the mouse over it, 0..1, eased in and out (its fill)
std::unordered_set<std::string> g_selected; // the boxes picked with Shift + right click: dragging one moves them all
std::string g_pick;       // the box Shift + right-clicked this frame (the smallest, of boxes over each other), or empty
float g_pick_area = 0;    // its size
bool g_right = false;     // Shift + right click this frame (taken at its first box: the first box over it takes the click)
// Shift + press this frame (taken at its first box, as g_right): each box under it bids, the smallest wins, and it's held
// from the next frame's first box. The first box drawn under it took it before, and a docked sub-menu's box lying on the
// Interaction Menu's gave the drag to the Interaction Menu: dragging the sub-menu off moved the Interaction Menu instead
// (reported 2026-11-17)
bool g_press_now = false;
std::string g_grab_id;    // the smallest box under this frame's Shift + press, or empty
float g_grab_area = 0;    // its size
ImVec2 g_grab_corner;     // its top-left corner then (pixels)
ImVec2 g_grab_mouse;      // the mouse then
// docked: the box the next drawn box goes with, and its name for the tooltip, or empty
std::string g_docked, g_docked_name;
int g_frame = -1;         // ImGui's frame a box was last drawn in
std::unordered_map<std::string, ImVec2> g_corner; // each box's top-left corner as last drawn (pixels)
std::unordered_map<std::string, ImVec2> g_start;  // while a picked box is held: the picked boxes' corners as it was taken
ImVec2 g_shift;           // how far the held box has gone from its corner then (pixels)
// The arrow keys: this frame's steps for the picked boxes (-1, 0 or 1 each way), with Shift (a big step), the boxes
// stepped since the keys were pressed and where each has been taken (its corner, the interface's units), and the frame
// they're let go (each of those boxes released there, its owner saving it)
ImVec2 g_nudge;
bool g_nudge_big = false;
std::unordered_map<std::string, ImVec2> g_nudged;
bool g_nudge_release = false;
// for the game's keyboard (takes_arrow, from its reads): some boxes picked, and when a box was last drawn
std::atomic<bool> g_any_picked{false};
std::atomic<unsigned long long> g_drawn_at{0};

// How a box is drawn: its fill's colour (its alpha added: `alpha`, and `cell` for the lines between its cells), its edge
// and how wide, and its name's tab and letters. Blue, and the picked one green with a thicker edge
struct Look {
    ImU32 fill, alpha, cell, edge;
    float width;
    ImU32 tab, text;
};
constexpr Look kPlain{IM_COL32(122, 162, 247, 0), 40, 110, IM_COL32(160, 198, 255, 255), 1.0f, IM_COL32(16, 20, 36, 210),
                      IM_COL32(255, 255, 255, 235)};
constexpr Look kPicked{IM_COL32(64, 214, 112, 0), 95, 140, IM_COL32(96, 245, 140, 255), 3.0f, IM_COL32(96, 245, 140, 255),
                       IM_COL32(6, 30, 14, 255)};

// the grid's lines (grid, snap): `step` apart, one through the screen's centre (a whole pixel): the nearest to `at`
float grid_line(float at, float centre, float step)
{
    return centre + std::round((at - centre) / step) * step;
}

// A box's corner `x0`, `y0` (pixels), `bw` wide, on the grid snap set: its top-left corner on a line's crossing, or its
// top-right with the Anchor right (snap's `right`). Only those: the top or the bottom, whichever was nearer, made a box
// whose height isn't the grid's hop a few pixels between the two as it was dragged (seen 2026-10-22)
void snap_corner(float& x0, float& y0, float bw, const ImVec2& display)
{
    const auto step = static_cast<float>(g_snap);
    const float cx = std::floor(display.x / 2), cy = std::floor(display.y / 2);
    x0 = g_snap_right ? grid_line(x0 + bw, cx, step) - bw : grid_line(x0, cx, step);
    y0 = grid_line(y0, cy, step);
}

bool arrow_down()
{
    return ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
           ImGui::IsKeyDown(ImGuiKey_DownArrow);
}

// At the frame's first box: last frame's Shift + press given to the smallest box under it, held from now as if pressed
// this frame (where in it it was taken, the click, and every picked box going with it, from where each was last drawn)
void take_grab(int frame)
{
    if (g_grab_id.empty())
        return;
    g_held = g_grab_id;
    g_held_frame = frame;
    g_grab = ImVec2(g_grab_mouse.x - g_grab_corner.x, g_grab_mouse.y - g_grab_corner.y);
    g_press = g_grab_mouse;
    g_dragged = false;
    // taken to be moved is clicked too (/vana shows its window's section, as for a click)
    g_clicked = g_grab_id, g_clicked_frame = frame, g_clicked_area = g_grab_area;
    g_start.clear();
    g_shift = ImVec2(0, 0);
    if (g_selected.count(g_grab_id) != 0) {
        for (const auto& other : g_selected)
            if (const auto corner = g_corner.find(other); corner != g_corner.end())
                g_start[other] = corner->second;
        g_start[g_grab_id] = g_grab_corner;
    }
    g_grab_id.clear();
}

// A docked box's mark (docked): a chain link on a dark tab in its top-right corner, amber while the box it goes with is
// dragged (its edge too: they move as one), and over it a tooltip saying so
constexpr ImU32 kLinked = IM_COL32(255, 196, 92, 255);
// `corner`: the box's top-right inside its edge; `wide` the box's width; `hover_name`: the mouse is over it (the name of
// the box it goes with), else null
void draw_link(ImDrawList* list, ImVec2 corner, float wide, ImU32 tab, ImU32 ink, const char* hover_name)
{
    if (wide < 48.0f)
        return; // (no room beside its name)
    const float gx = corner.x - 28.0f, gy = corner.y;
    list->AddRectFilled(ImVec2(gx, gy), ImVec2(corner.x, gy + 14.0f), tab);
    list->AddRect(ImVec2(gx + 4, gy + 4), ImVec2(gx + 16, gy + 10), ink, 3.0f, 0, 1.5f);
    list->AddRect(ImVec2(gx + 12, gy + 4), ImVec2(gx + 24, gy + 10), ink, 3.0f, 0, 1.5f);
    if (hover_name != nullptr)
        ImGui::SetTooltip("Docked to %s: it moves with it.\nShift + drag this box to undock it.", hover_name);
}

} // namespace

void snap(int pixels, bool right)
{
    g_snap = pixels > 0 ? pixels : 0;
    g_snap_right = right;
}

std::string clicked()
{
    std::string id;
    id.swap(g_clicked);
    return id;
}

void select(const std::string& id)
{
    g_selected.clear();
    g_pick.clear();
    if (!id.empty())
        g_selected.insert(id);
    g_any_picked = !g_selected.empty();
    // (picked with no boxes up, its own box shown from the next frame: not taken for boxes long gone and dropped then)
    if (ImGui::GetCurrentContext() != nullptr)
        g_frame = ImGui::GetFrameCount();
}

std::vector<std::string> selected()
{
    return {g_selected.begin(), g_selected.end()};
}

bool takes_arrow()
{
    return g_any_picked && GetTickCount64() - g_drawn_at < 250;
}

bool showing()
{
    const unsigned long long at = g_drawn_at;
    return at != 0 && GetTickCount64() - at < 250;
}

void label(const char* text)
{
    g_label = text != nullptr ? text : "";
}

void docked(const char* to, const char* name)
{
    g_docked = to != nullptr ? to : "";
    g_docked_name = name != nullptr ? name : g_docked;
}

void grid(int pixels, unsigned color, unsigned centre, unsigned fill)
{
    if (ImGui::GetCurrentContext() == nullptr || pixels < 4)
        return;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    if ((fill >> 24) != 0)
        list->AddRectFilled(ImVec2(0, 0), size, fill);
    // the lines go out from the screen's centre, so the centre is on a line and the grid is the same either side of it
    // (whole pixels: an even width's centre falls between two pixels, and the 3-wide line on it covers the pixel either side)
    const auto step = static_cast<float>(pixels);
    const float cx = std::floor(size.x / 2), cy = std::floor(size.y / 2);
    for (float x = grid_line(0, cx, step); x < size.x; x += step)
        list->AddLine(ImVec2(x, 0), ImVec2(x, size.y), color);
    for (float y = grid_line(0, cy, step); y < size.y; y += step)
        list->AddLine(ImVec2(0, y), ImVec2(size.x, y), color);
    // the dead centre, 3 pixels thick each way, so it's found at a glance
    list->AddLine(ImVec2(cx, 0), ImVec2(cx, size.y), centre, 3.0f);
    list->AddLine(ImVec2(0, cy), ImVec2(size.x, cy), centre, 3.0f);
}

bool draw(const char* id, const Box& box, int cells, Drag& drag)
{
    drag = {};
    const std::string name = std::move(g_label); // (this box's only)
    g_label.clear();
    const std::string to = std::move(g_docked), to_name = std::move(g_docked_name);
    g_docked.clear(), g_docked_name.clear();
    if (ImGui::GetCurrentContext() == nullptr)
        return false;
    if (!g_space)
        g_space = uispace::find(GetModuleHandleW(L"FFXiMain.dll"));
    const ImGuiIO& io = ImGui::GetIO();
    const auto w = static_cast<unsigned>(io.DisplaySize.x), h = static_cast<unsigned>(io.DisplaySize.y);
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, scale = 0;
    if (!uispace::to_pixels(static_cast<float>(box.left), static_cast<float>(box.top), w, h, x0, y0, scale) ||
        !uispace::to_pixels(static_cast<float>(box.right), static_cast<float>(box.bottom), w, h, x1, y1, scale))
        return false;
    // a window put off the screen (an addon's hide leaves the Target window at -4000, -4000, and the chat line at -4000
    // across): its box at the top left, as big as the window, so there is one to take. Dragged from there, the window's
    // place is set from where the box is let go as for any other
    if (x1 <= 0.0f || y1 <= 0.0f || x0 >= io.DisplaySize.x || y0 >= io.DisplaySize.y) {
        x1 -= x0, y1 -= y0;
        x0 = 0.0f, y0 = 0.0f;
    }
    const ImVec2 mouse = io.MousePos;
    // (its box stopped being drawn while it was held: its eye turned off, or /vana shut. Not just another box drawn first
    // the frame it's let go, with more than one eye on: the held one still gets its release, and its place is saved)
    const int frame = ImGui::GetFrameCount();
    if (!g_held.empty() && g_held_frame < frame - 1 && !io.MouseDown[0])
        g_held.clear();
    if (frame != g_frame) { // the frame's first box
        if (g_frame < frame - 10)
            g_selected.clear(); // (no boxes for a while: the HUD Manager left, /vana shut)
        if (!g_pick.empty()) { // last frame's Shift + right click: its box picked, or no longer
            if (g_selected.erase(g_pick) == 0)
                g_selected.insert(g_pick);
            g_pick.clear();
        }
        // a click without Shift in the game (on a box or not; not on an ImGui window), or Escape: none picked
        if ((io.MouseClicked[0] && !io.KeyShift && GImGui->HoveredWindow == nullptr) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_selected.clear();
        g_right = io.KeyShift && io.MouseClicked[1];
        take_grab(frame);
        g_press_now = io.KeyShift && io.MouseClicked[0] && g_held.empty();
        g_frame = frame;
        // the arrow keys (as pressed and as they repeat), alone or with Shift: the picked boxes go a step, not while one's
        // held with the mouse or text is being typed; the keys let go, the ones that went are released
        g_nudge = ImVec2(0, 0);
        g_nudge_big = io.KeyShift;
        if (!g_selected.empty() && g_held.empty() && !io.KeyCtrl && !io.KeyAlt && !io.WantTextInput) {
            g_nudge.x = (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1.0f : 0.0f) - (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? 1.0f : 0.0f);
            g_nudge.y = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1.0f : 0.0f) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1.0f : 0.0f);
        }
        if (g_nudge_release)
            g_nudged.clear(); // (let go last frame: a box not drawn then isn't released later)
        g_nudge_release = !g_nudged.empty() && !arrow_down();
        g_any_picked = !g_selected.empty();
        g_drawn_at = GetTickCount64();
    }
    // a picked box going with the held one (the held one snaps; these go as far as it does)
    bool moving = false;
    if (const auto start = g_start.find(id); start != g_start.end() && g_held != id) {
        if (g_dragged) {
            const float bw = x1 - x0, bh = y1 - y0;
            x0 = start->second.x + g_shift.x, y0 = start->second.y + g_shift.y;
            x1 = x0 + bw, y1 = y0 + bh;
            float ux = 0, uy = 0;
            if (uispace::to_units(x0, y0, w, h, ux, uy)) {
                drag.held = true;
                drag.left = static_cast<int>(std::lround(ux));
                drag.top = static_cast<int>(std::lround(uy));
            }
            moving = true;
        }
        if (g_held.empty()) { // let go (the held box was, this frame or the last): its owner saves where it's left
            g_start.erase(start);
            moving = false;
            drag.released = drag.held;
        }
    }
    bool held = g_held == id;
    if (held) {
        g_held_frame = frame;
        if (!g_dragged) {
            const float dx = mouse.x - g_press.x, dy = mouse.y - g_press.y;
            g_dragged = dx * dx + dy * dy >= io.MouseDragThreshold * io.MouseDragThreshold;
        }
        // the box goes with the mouse at once (the window follows a frame later, as its owner puts it)
        if (g_dragged) {
            const float bw = x1 - x0, bh = y1 - y0;
            x0 = mouse.x - g_grab.x, y0 = mouse.y - g_grab.y;
            if (g_snap > 0)
                snap_corner(x0, y0, bw, io.DisplaySize);
            x1 = x0 + bw, y1 = y0 + bh;
            if (const auto start = g_start.find(id); start != g_start.end())
                g_shift = ImVec2(x0 - start->second.x, y0 - start->second.y);
            float ux = 0, uy = 0;
            if (uispace::to_units(x0, y0, w, h, ux, uy)) {
                drag.held = true;
                drag.left = static_cast<int>(std::lround(ux));
                drag.top = static_cast<int>(std::lround(uy));
            }
        }
        if (!io.MouseDown[0]) {
            g_start.erase(id);
            g_held.clear();
            held = false;
            drag.released = drag.held;
        }
    }
    // the arrow keys on a picked box: it goes a step as if dragged there, its owner moving the window: a unit of the
    // interface (the finest a window goes: a pixel, at the game's own UI size), not snapped; with Shift a grid square while
    // it snaps, its corner on a line, else 10 units. Held there every frame till the keys are let go, as a box held with
    // the mouse is: its owner reads its place from the settings again whenever they're saved (any vana.config.set, the HUD
    // Manager's autosave), and between steps that put it back where it was saved, which the release then saved (seen
    // 2026-10-22: a nudge now and then flicked back). The keys let go, it's released where it was taken
    const bool nudge_step = !held && !moving && (g_nudge.x != 0 || g_nudge.y != 0) && g_selected.count(id) != 0;
    if (held || moving)
        g_nudged.erase(id); // (taken with the mouse: the mouse's place, not the keys')
    else if (nudge_step || g_nudged.count(id) != 0) {
        const float bw = x1 - x0, bh = y1 - y0;
        float ux = 0, uy = 0, px = 0, py = 0;
        ImVec2 at;
        const auto nudged = g_nudged.find(id);
        bool known = nudged != g_nudged.end() || uispace::to_units(x0, y0, w, h, ux, uy);
        if (nudged != g_nudged.end())
            at = nudged->second;
        else if (known)
            at = ImVec2(std::round(ux), std::round(uy));
        if (known && nudge_step && g_nudge_big && g_snap > 0) { // a grid square, its corner on a line
            known = uispace::to_pixels(at.x, at.y, w, h, px, py, scale);
            if (known) {
                px += g_nudge.x * g_snap, py += g_nudge.y * g_snap;
                snap_corner(px, py, bw, io.DisplaySize);
                known = uispace::to_units(px, py, w, h, ux, uy);
                at = ImVec2(std::round(ux), std::round(uy));
            }
        } else if (known && nudge_step) {
            const float units = g_nudge_big ? 10.0f : 1.0f;
            at.x += g_nudge.x * units, at.y += g_nudge.y * units;
        }
        if (known && nudge_step)
            g_nudged[id] = at;
        // drawn where it's been taken (its window follows a frame later, as its owner puts it)
        if (known && uispace::to_pixels(at.x, at.y, w, h, x0, y0, scale)) {
            x1 = x0 + bw, y1 = y0 + bh;
            drag.held = true;
            drag.left = static_cast<int>(at.x);
            drag.top = static_cast<int>(at.y);
            if (!nudge_step && g_nudge_release) {
                drag.released = true;
                g_nudged.erase(id);
            }
        }
    }
    const bool over = mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y1;
    // the boxes are drawn under the ImGui windows (the /vana window, the HUD Manager): where one is over a box, the mouse
    // is the window's
    const bool on_window = GImGui->HoveredWindow != nullptr;
    // Shift + press: of boxes over each other, the smallest is held, from the next frame (take_grab)
    if (over && !on_window && g_press_now) {
        const float area = (x1 - x0) * (y1 - y0);
        if (g_grab_id.empty() || area < g_grab_area)
            g_grab_id = id, g_grab_area = area, g_grab_corner = ImVec2(x0, y0), g_grab_mouse = mouse;
        // the press is the boxes': ImGui doesn't see it (it starts a window's move at the frame's end, from this)
        ImGui::GetIO().MouseClicked[0] = false;
    }
    // a click without Shift: the box's (/vana shows its window's section)
    if (over && !on_window && !io.KeyShift && io.MouseClicked[0] && g_held.empty()) {
        const float area = (x1 - x0) * (y1 - y0);
        if (g_clicked_frame != frame || area < g_clicked_area)
            g_clicked = id, g_clicked_frame = frame, g_clicked_area = area;
    }
    // Shift + right click: the box picked, or no longer, the next frame (of boxes over each other, the smallest)
    if (over && !on_window && g_right && g_held.empty()) {
        const float area = (x1 - x0) * (y1 - y0);
        if (g_pick.empty() || area < g_pick_area)
            g_pick = id, g_pick_area = area;
        ImGui::GetIO().MouseClicked[1] = false;
    }
    const bool picked = g_selected.count(id) != 0;
    if (held || (over && !on_window))
        gui::take_mouse(x0, y0, x1, y1); // the press is the box's, not the game's
    // the mouse over it (or holding it): its fill a touch stronger, eased in and out over about a tenth of a second
    float& lit = g_glow[id];
    const bool following = !to.empty() && g_held == to && g_dragged; // (docked: the box it goes with is being dragged)
    const float target = held || moving || following || (over && !on_window) ? 1.0f : 0.0f;
    const float step = std::min(1.0f, io.DeltaTime * 10.0f);
    lit += (target - lit) * step;
    if (std::fabs(target - lit) < 0.01f)
        lit = target;
    // On whole pixels, its edge inside its rect: the outer pixel is the window's own edge, the one Snap puts on a grid
    // line. A bright 1-pixel line; picked (the HUD Manager's list, Shift + right click), 3 pixels, inward, and the box
    // green where the others are blue (asked for, 2026-11-12: the one being edited told at a glance). No glow round
    // it (it made the edge soft and stood past the grid line: 2026-10-22). Under every ImGui window, over the grid
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    const float l = std::round(x0), t = std::round(y0), r = std::round(x1), b = std::round(y1);
    const Look& look = picked ? kPicked : kPlain;
    const auto alpha = static_cast<ImU32>(look.alpha + std::lround(30.0f * lit));
    list->AddRectFilled(ImVec2(l, t), ImVec2(r, b), look.fill | (alpha << IM_COL32_A_SHIFT));
    if (cells > 1) {
        const float cell = (r - l) / cells;
        for (int i = 1; i < cells; ++i) {
            const float at = std::round(l + cell * i);
            list->AddRectFilled(ImVec2(at, t), ImVec2(at + 1, b), look.fill | (look.cell << IM_COL32_A_SHIFT));
        }
    }
    const float edge = look.width;
    const ImU32 ink = following ? kLinked : look.edge;
    list->AddRectFilled(ImVec2(l, t), ImVec2(r, t + edge), ink);
    list->AddRectFilled(ImVec2(l, b - edge), ImVec2(r, b), ink);
    list->AddRectFilled(ImVec2(l, t + edge), ImVec2(l + edge, b - edge), ink);
    list->AddRectFilled(ImVec2(r - edge, t + edge), ImVec2(r, b - edge), ink);
    if (!to.empty())
        draw_link(list, ImVec2(r - edge, t + edge), r - l, look.tab, ink, over && !on_window && !held ? to_name.c_str() : nullptr);
    if (!name.empty()) { // its name in its top left corner, on a dark tab inside the edge (picked: dark on the edge's green)
        const ImVec2 text = ImGui::CalcTextSize(name.c_str());
        const float nx = l + edge, ny = t + edge;
        list->AddRectFilled(ImVec2(nx, ny), ImVec2(std::round(nx + text.x + 8), std::round(ny + text.y + 4)), look.tab);
        list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(nx + 4, ny + 2), look.text, name.c_str());
    }
    // (how to use the boxes is in the HUD Manager window's foot, vana\hud.lua: no hint over them but a docked one's)
    g_corner[id] = ImVec2(x0, y0);
    return true;
}

} // namespace vanacore::outline
