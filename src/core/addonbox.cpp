#include "addonbox.hpp"

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gui.hpp"
#include "layout.hpp"

namespace vanacore::addonbox {
namespace {

// A place on the screen, in ImGui's pixels
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(ImVec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    float area() const { return w * h; }
};

// A box asked for this frame (box), drawn at its end (end_frame)
struct Asked {
    std::string label;
    ImFont* font = nullptr; // the font ImGui had as it was asked for (the HUD Manager's), for its label
    float font_size = 0;
    bool picked = false;
    bool moved = false; // held or stepped: drawn where it's been taken to (`at`), not where its window is yet
    Rect at;
};

int g_frame = -1;                     // ImGui's frame box was last called in
std::map<std::string, Rect> g_rects;  // the windows and text and shapes, as the last frame drew them (box's first call)
std::map<std::string, Asked> g_asked; // this frame's boxes
std::map<std::string, Rect> g_last;   // the boxes the last frame drew, where (after_new_frame)
int g_drawn_frame = -10;              // the frame they were drawn in
ImVec2 g_mouse(-FLT_MAX, -FLT_MAX);   // on the screen this frame (after_new_frame)
std::string g_top;                    // the box under the mouse this frame (after_new_frame), or empty
std::string g_clicked;                // clicked's
// Shift + drag: the box held, where in it it was taken, where the press was and whether it's gone ImGui's drag threshold
// from there since, where the box was (from) and where it's been put (to)
std::string g_held;
ImVec2 g_grab, g_press, g_from, g_to;
bool g_dragged = false;
// The arrow keys: this frame's step for the picked boxes (-1, 0 or 1 each way), with Shift (a big step); the boxes stepped
// since the keys were pressed, each's place before and where it's been put; and the frame they're let go
ImVec2 g_nudge;
bool g_nudge_big = false;
std::map<std::string, std::pair<ImVec2, ImVec2>> g_nudged;
bool g_nudge_release = false;
std::unordered_map<std::string, float> g_glow; // each box lit by the mouse over it (or holding it), 0..1, eased
// for the game's keyboard (takes_arrow, from its reads): picked boxes up, and when boxes were last drawn
std::atomic<bool> g_any_picked{false};
std::atomic<unsigned long long> g_drawn_at{0};

constexpr ImU32 kFill = IM_COL32(243, 227, 138, 0); // the HUD Manager's light yellow (vana\draw.lua's P.hud), its alpha added
constexpr ImU32 kEdge = IM_COL32(250, 214, 92, 255);
constexpr ImU32 kLabelText = IM_COL32(34, 28, 6, 255);
// the picked addon's boxes: green, as outline.cpp's picked box is (the one being edited told at a glance)
constexpr ImU32 kPickedFill = IM_COL32(64, 214, 112, 0);
constexpr ImU32 kPickedEdge = IM_COL32(96, 245, 140, 255);
constexpr ImU32 kPickedLabelText = IM_COL32(6, 30, 14, 255);

bool same(ImVec2 a, ImVec2 b)
{
    return std::fabs(a.x - b.x) < 0.5f && std::fabs(a.y - b.y) < 0.5f;
}

// `at` on the HUD Manager's grid: lines `pixels` apart, one through the screen's centre (a whole pixel), as outline::grid
// draws them: the nearest crossing
ImVec2 on_grid(ImVec2 at, int pixels, ImVec2 display)
{
    const auto step = static_cast<float>(pixels);
    const float cx = std::floor(display.x / 2), cy = std::floor(display.y / 2);
    return ImVec2(cx + std::round((at.x - cx) / step) * step, cy + std::round((at.y - cy) / step) * step);
}

bool arrow_down()
{
    return ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
           ImGui::IsKeyDown(ImGuiKey_DownArrow);
}

float key_step(ImGuiKey plus, ImGuiKey minus)
{
    return (ImGui::IsKeyPressed(plus) ? 1.0f : 0.0f) - (ImGui::IsKeyPressed(minus) ? 1.0f : 0.0f);
}

// The windows' and text and shapes' places as the last frame drew them (layout::windows: those on the screen)
std::map<std::string, Rect> places()
{
    std::map<std::string, Rect> out;
    for (const layout::Window& w : layout::windows())
        if (w.shown && w.w > 0 && w.h > 0)
            out[w.name] = {w.x, w.y, w.w, w.h};
    return out;
}

// The frame's first box: its places, and the arrow keys (as pressed and as they repeat, alone or with Shift; not while a
// box is held with the mouse or text is typed). The keys let go, the boxes they moved are released.
void frame_start(int frame)
{
    g_frame = frame;
    g_asked.clear();
    g_rects = places();
    const ImGuiIO& io = ImGui::GetIO();
    g_nudge = ImVec2(0, 0);
    g_nudge_big = io.KeyShift;
    if (g_held.empty() && !io.KeyCtrl && !io.KeyAlt && !io.WantTextInput)
        g_nudge = ImVec2(key_step(ImGuiKey_RightArrow, ImGuiKey_LeftArrow), key_step(ImGuiKey_DownArrow, ImGuiKey_UpArrow));
    if (g_nudge_release)
        g_nudged.clear(); // (let go last frame: a box not asked for then isn't released later)
    g_nudge_release = !g_nudged.empty() && !arrow_down();
}

// A press on the box under the mouse: clicked's, and with Shift, held to be dragged. Not the addon window's under it, nor
// ImGui's (a press on no window takes the focus from them all at the frame's end)
void take_press(const std::string& name, const Rect& at)
{
    ImGuiIO& io = ImGui::GetIO();
    if (g_top != name || !g_held.empty() || !io.MouseClicked[0])
        return;
    g_clicked = name;
    if (io.KeyShift) {
        g_held = name;
        g_grab = ImVec2(g_mouse.x - at.x, g_mouse.y - at.y);
        g_press = g_mouse;
        g_dragged = false;
        g_from = g_to = ImVec2(at.x, at.y);
        g_nudged.erase(name); // (taken with the mouse: the mouse's place, not the keys')
    }
    io.MouseClicked[0] = false;
}

// The held box: with the mouse once it's gone the drag threshold, its corner on the grid, the window moved there (it
// follows a frame later, as its addon begins it); let go, released
void drag(const std::string& name, int snap, Asked& asked, Result& result)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (!g_dragged) {
        const float dx = g_mouse.x - g_press.x, dy = g_mouse.y - g_press.y;
        g_dragged = dx * dx + dy * dy >= io.MouseDragThreshold * io.MouseDragThreshold;
    }
    if (g_dragged) {
        ImVec2 to(g_mouse.x - g_grab.x, g_mouse.y - g_grab.y);
        if (snap > 0)
            to = on_grid(to, snap, io.DisplaySize);
        to = ImVec2(std::round(to.x), std::round(to.y));
        if (!same(to, g_to)) {
            g_to = to;
            layout::set(name, &to.x, &to.y, nullptr);
        }
        asked.at.x = to.x, asked.at.y = to.y;
        asked.moved = true;
    }
    if (!io.MouseDown[0]) {
        result.released = g_dragged && !same(g_to, g_from);
        result.x = g_to.x, result.y = g_to.y, result.from_x = g_from.x, result.from_y = g_from.y;
        g_held.clear();
    }
}

// The arrow keys on a picked box: a pixel a step, or with Shift a grid square (its corner on a line) while it snaps, else
// 10; held there till the keys are let go, then released
void nudge(const std::string& name, int snap, bool picked, Asked& asked, Result& result)
{
    const bool step = picked && (g_nudge.x != 0 || g_nudge.y != 0);
    auto found = g_nudged.find(name);
    if (found == g_nudged.end()) {
        if (!step)
            return;
        const ImVec2 at(std::round(asked.at.x), std::round(asked.at.y));
        found = g_nudged.emplace(name, std::make_pair(at, at)).first;
    }
    ImVec2& to = found->second.second;
    if (step) {
        if (g_nudge_big && snap > 0) {
            const auto square = static_cast<float>(snap);
            to = on_grid(ImVec2(to.x + g_nudge.x * square, to.y + g_nudge.y * square), snap, ImGui::GetIO().DisplaySize);
        } else {
            const float pixels = g_nudge_big ? 10.0f : 1.0f;
            to = ImVec2(to.x + g_nudge.x * pixels, to.y + g_nudge.y * pixels);
        }
        layout::set(name, &to.x, &to.y, nullptr);
    }
    asked.at.x = to.x, asked.at.y = to.y;
    asked.moved = true;
    if (!step && g_nudge_release) {
        const ImVec2 from = found->second.first;
        result.released = !same(to, from);
        result.x = to.x, result.y = to.y, result.from_x = from.x, result.from_y = from.y;
        g_nudged.erase(found);
    }
}

// What a window shows last: its draw list, or its last child window's (drawn after it, over it), and so on down
ImDrawList* top_list(const ImGuiWindow* window)
{
    for (int depth = 0; depth < 16; ++depth) {
        const ImGuiWindow* last = nullptr;
        for (const ImGuiWindow* child : window->DC.ChildWindows)
            if (child->Active && !child->Hidden)
                last = child;
        if (last == nullptr)
            break;
        window = last;
    }
    return window->DrawList;
}

// A box on `list`: a yellow fill, a touch stronger with the mouse over it (eased in and out over about a tenth of a
// second), its edge inside its rect (3 pixels picked, else 2), and its label on a yellow tab above its top left corner (or
// inside it, at the screen's top). Picked (the HUD Manager's addon), all of it green
void draw_box(ImDrawList* list, const std::string& name, const Asked& asked, const Rect& r, float dt)
{
    float& lit = g_glow[name];
    const float target = g_top == name || g_held == name ? 1.0f : 0.0f;
    lit += (target - lit) * std::min(1.0f, dt * 10.0f);
    if (std::fabs(target - lit) < 0.01f)
        lit = target;
    list->PushClipRectFullScreen();
    list->PushTexture(ImGui::GetIO().Fonts->TexRef);
    const float l = std::round(r.x), t = std::round(r.y), rr = std::round(r.x + r.w), b = std::round(r.y + r.h);
    const auto alpha = static_cast<ImU32>((asked.picked ? 95 : 32) + std::lround(35.0f * lit));
    list->AddRectFilled(ImVec2(l, t), ImVec2(rr, b), (asked.picked ? kPickedFill : kFill) | (alpha << IM_COL32_A_SHIFT));
    const ImU32 line = asked.picked ? kPickedEdge : kEdge;
    const float edge = asked.picked ? 3.0f : 2.0f;
    list->AddRectFilled(ImVec2(l, t), ImVec2(rr, t + edge), line);
    list->AddRectFilled(ImVec2(l, b - edge), ImVec2(rr, b), line);
    list->AddRectFilled(ImVec2(l, t + edge), ImVec2(l + edge, b - edge), line);
    list->AddRectFilled(ImVec2(rr - edge, t + edge), ImVec2(rr, b - edge), line);
    if (!asked.label.empty() && asked.font != nullptr && asked.font_size > 0) {
        const ImVec2 text = asked.font->CalcTextSizeA(asked.font_size, FLT_MAX, 0.0f, asked.label.c_str());
        const float th = std::round(text.y + 4);
        const float ty = t - th >= 0 ? t - th : t + edge;
        list->AddRectFilled(ImVec2(l, ty), ImVec2(std::round(l + text.x + 10), ty + th), line, 3.0f,
                            t - th >= 0 ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersBottomRight);
        list->AddText(asked.font, asked.font_size, ImVec2(l + 5, ty + 2), asked.picked ? kPickedLabelText : kLabelText,
                      asked.label.c_str());
    }
    list->PopTexture();
    list->PopClipRect();
}

} // namespace

Result box(const std::string& name, const std::string& label, int snap, bool picked)
{
    Result result;
    if (GImGui == nullptr || name.empty())
        return result;
    const int frame = ImGui::GetFrameCount();
    if (frame != g_frame)
        frame_start(frame);
    const auto found = g_rects.find(name);
    if (found == g_rects.end())
        return result;
    result.drawn = true;
    Asked& asked = g_asked[name];
    asked.label = label;
    asked.font = ImGui::GetFont();
    asked.font_size = ImGui::GetFontSize();
    asked.picked = picked;
    asked.moved = false;
    asked.at = found->second;
    take_press(name, found->second);
    if (g_held == name)
        drag(name, snap, asked, result);
    else
        nudge(name, snap, picked, asked, result);
    // the press is the box's, not the game's (text and shapes have no window of ImGui's to take it)
    if (g_top == name || g_held == name)
        gui::take_mouse(asked.at.x, asked.at.y, asked.at.x + asked.at.w, asked.at.y + asked.at.h);
    return result;
}

std::string clicked()
{
    std::string name;
    name.swap(g_clicked);
    return name;
}

bool takes_arrow()
{
    return g_any_picked && GetTickCount64() - g_drawn_at < 250;
}

void after_new_frame(float x, float y)
{
    g_mouse = ImVec2(x, y);
    g_top.clear();
    if (GImGui == nullptr)
        return;
    ImGuiContext& g = *GImGui;
    if (g_drawn_frame < g.FrameCount - 1) { // no boxes last frame (Addon View off, the HUD Manager shut)
        g_last.clear();
        g_held.clear();
        g_nudged.clear();
        return;
    }
    if (!g_held.empty()) { // a box held: nothing else has the mouse till it's let go
        g.HoveredWindow = g.HoveredWindowUnderMovingWindow = nullptr;
        return;
    }
    if (g.MovingWindow != nullptr) // (ImGui moving a window: the HUD Manager)
        return;
    if (g.HoveredWindow != nullptr) {
        // an addon's window with its box under the mouse: the box's (the front one's, as the windows are drawn). Another
        // window (VanaCore's own: the HUD Manager) keeps it
        const ImGuiWindow* hovered = g.HoveredWindow;
        const ImGuiWindow* root = hovered->RootWindowPopupTree != nullptr ? hovered->RootWindowPopupTree : hovered->RootWindow;
        const auto found = root != nullptr ? g_last.find(root->Name) : g_last.end();
        if (found == g_last.end() || !found->second.contains(g_mouse))
            return;
        g_top = found->first;
        g.HoveredWindow = g.HoveredWindowUnderMovingWindow = nullptr;
        return;
    }
    // over none of ImGui's windows (text and shapes, or a window that lets the mouse through): of the boxes there, the
    // smallest
    float best = FLT_MAX;
    for (const auto& [name, r] : g_last)
        if (r.contains(g_mouse) && r.area() < best) {
            best = r.area();
            g_top = name;
        }
}

void end_frame()
{
    if (GImGui == nullptr)
        return;
    const int frame = ImGui::GetFrameCount();
    if (g_frame != frame) { // no box this frame
        g_last.clear();
        g_any_picked = false;
        return;
    }
    if (!g_held.empty() && g_asked.count(g_held) == 0)
        g_held.clear(); // (its window gone while it was held)
    // where each is now: the windows as this frame drew them (their Begin after a move), text and shapes as the last
    const std::map<std::string, Rect> now = places();
    g_last.clear();
    bool any_picked = false;
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    const float dt = ImGui::GetIO().DeltaTime;
    for (const auto& [name, asked] : g_asked) {
        Rect r = asked.at;
        if (const auto found = now.find(name); !asked.moved && found != now.end())
            r = found->second;
        const ImGuiWindow* window = ImGui::FindWindowByName(name.c_str());
        const bool own = window != nullptr && window->Active && !window->Hidden;
        draw_box(own ? top_list(window) : background, name, asked, r, dt);
        g_last[name] = r;
        any_picked = any_picked || asked.picked;
    }
    g_drawn_frame = frame;
    g_any_picked = any_picked;
    g_drawn_at = GetTickCount64();
}

} // namespace vanacore::addonbox
