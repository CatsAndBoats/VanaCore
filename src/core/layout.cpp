#include "layout.hpp"

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.hpp"
#include "log.hpp"

namespace vanacore::layout {
namespace {

// What's set for a window (config\layout.json).
struct Entry {
    std::string owner;    // who drew it when it was last seen
    float x = 0, y = 0;   // where it was last seen; a placed one goes back there as it appears
    bool placed = false;  // ImGui places it: put at x, y as it appears, and kept where it's dragged to
    float dx = 0, dy = 0; // its addon places it every frame: added to that
    float scale = 1;
    bool objects = false; // its addon's fonts and primitives, not an ImGui window (x, y: where they were drawn, dx, dy with it)
    bool locked = false;     // the mouse can't drag or resize it (set_flags)
    bool no_pointer = false; // the mouse goes through it
};

// A stretch of one of ImGui's screen-wide draw lists (foreground, background) a window drew on.
struct Segment {
    ImDrawList* list;
    int from, to; // vertices
};

// What's seen of a window while the game runs.
struct Seen {
    std::string owner;
    int frame = -10;        // the last frame it was begun in
    int placed_frame = -10; // the last frame its addon placed it (SetNextWindowPos or SetWindowPos, ImGuiCond_Always)
    bool pinned = false;    // ... and the frame before too: its addon places it every frame
    ImVec2 asked{}, gave{}; // where its addon put it on its last placed frame, and where it went with the offset
    bool gave_valid = false;
    bool echo = false;      // last frame its addon asked for where it went the frame before (see addon_places)
    bool move = false;      // set() moved it: put at its entry's x, y at its next Begin
    ImVec2 pos{}, size{};   // as the last frame it was drawn in ended
    ImGuiWindow* window = nullptr; // ImGui's, once begun
    int drawn_frame = -10;         // the frame `drawn` is for
    std::vector<Segment> drawn;    // what it drew on the foreground and background lists, between its Begin and End
};

// A window between its Begin and End: where the foreground and background lists were as it began (or as a window begun
// inside it ended).
struct Open {
    ImGuiWindow* window;
    Seen* seen;
    int fg, bg;
    float scale; // what it's drawn at (widen_viewport while what's inside it is begun)
};

// A window drawn at a scale this frame: its top-left corner (where the scale is from) and its scale.
struct Scaled {
    ImGuiWindow* window = nullptr;
    ImVec2 origin;
    float scale = 1.0f;
};

std::wstring g_file;
std::map<std::string, Entry> g_entries;
std::map<std::string, Seen> g_seen;
std::vector<Window> g_shown; // the windows the last frame drew
std::vector<Window> g_objects_shown; // the owners' fonts and primitives the last frame drew (objects())
std::vector<Open> g_open;
std::vector<Scaled> g_scaled;           // as the last frame ended
ImGuiWindow* g_mouse_window = nullptr;  // the scaled window the mouse is in this frame (ImGui sees it in that window's pixels)
ImGuiWindow* g_hovered = nullptr;       // the window under the mouse as the windows are drawn
ImGuiWindow* g_held = nullptr;          // the scaled window a press began in: the mouse stays in its pixels until let go
ImVec2 g_move_shift;                    // its own pixels less the screen's at the press (mouse_in: while it's dragged)
bool g_set_hovered = false;             // after_new_frame puts g_hovered in place of ImGui's
bool g_dirty = false;
ULONGLONG g_changed = 0;
std::string g_highlight;
int g_highlight_frame = -10;
float g_own_scale = 1.0f; // VanaCore's own windows' scale, back to its interface's size (set_own_scale)

// VanaCore's own ImGui windows: its addons' ("vanacore/keybinds"), and those its C++ draws (/vana stats and graph)
bool own_owner(const std::string& owner)
{
    return owner.rfind("vanacore/", 0) == 0;
}

bool own_window(const ImGuiWindow* window)
{
    const std::string_view name = window->Name;
    return name.rfind("VanaCore ", 0) == 0 || name.find("###vana_") != std::string_view::npos;
}

// A scaled window's own pixels the display holds. ImGui keeps a window's drawing inside the display in the pixels it is
// begun in; one drawn smaller (its scale below 1: the game's pixels fewer than the interface's, or set so in the Layout
// tab) shows display / scale of its own pixels, so ImGui would cut it off short. Its Begin is made with the viewport that
// big instead (widen_clip, restore_clip after), and its place kept on the screen as ImGui would with the display it has.
ImVec2 g_clip_size, g_clip_work;
bool g_clip_widened = false;

bool own_window_name(const char* name)
{
    const std::string_view text = name;
    return text.rfind("VanaCore ", 0) == 0 || text.find("###vana_") != std::string_view::npos;
}

constexpr ULONGLONG kSaveDelayMs = 500;
constexpr ImGuiWindowFlags kNotTopLevel = ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup;

bool same(ImVec2 a, ImVec2 b)
{
    return std::fabs(a.x - b.x) < 0.5f && std::fabs(a.y - b.y) < 0.5f;
}

void touch()
{
    g_dirty = true;
    g_changed = GetTickCount64();
}

Entry* entry_of(const std::string& name)
{
    const auto found = g_entries.find(name);
    return found != g_entries.end() ? &found->second : nullptr;
}

// What a window's entry adds to its addon's flags at its Begin (set_flags): locked, NoMove and NoResize; no pointer
// events, NoInputs (no mouse, no keyboard or gamepad navigation, never focused by it)
int flags_of(const Entry* e)
{
    if (e == nullptr || e->objects)
        return 0;
    return (e->locked ? ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize : 0) | (e->no_pointer ? ImGuiWindowFlags_NoInputs : 0);
}

// A window with no pointer events: its child windows let the mouse through too (ImGui's don't take NoMouseInputs from the
// window they're in), set as this frame drew them for the next frame's search for the window under the mouse and for
// gui.cpp's publish (where the game keeps the mouse). Their Begin puts their own flags back each frame.
void no_pointer_children()
{
    for (ImGuiWindow* window : GImGui->Windows) {
        if (!window->Active || (window->Flags & ImGuiWindowFlags_ChildWindow) == 0 || window->RootWindow == nullptr)
            continue;
        if ((window->RootWindow->Flags & ImGuiWindowFlags_NoMouseInputs) == 0)
            continue;
        const Entry* e = entry_of(window->RootWindow->Name);
        if (e != nullptr && e->no_pointer)
            window->Flags |= ImGuiWindowFlags_NoInputs;
    }
}

// ---- Scale --------------------------------------------------------------------------------------

const Scaled* scaled_of(const ImGuiWindow* window)
{
    for (const Scaled& s : g_scaled)
        if (s.window == window && window != nullptr)
            return &s;
    return nullptr;
}

// The scaled window `window` is drawn as part of: itself, or the one it's a child window of; for a popup, the window it
// was opened from; for a tooltip (at the mouse), the one the mouse is in. Null: none.
const Scaled* scale_of_window(const ImGuiWindow* window)
{
    for (int depth = 0; window != nullptr && depth < 8; ++depth) {
        const ImGuiWindow* root = window->RootWindow;
        if (const Scaled* s = scaled_of(root))
            return s;
        if ((root->Flags & ImGuiWindowFlags_Tooltip) != 0)
            return scaled_of(g_mouse_window);
        if ((root->Flags & ImGuiWindowFlags_Popup) == 0)
            return nullptr;
        window = root->ParentWindow;
    }
    return nullptr;
}

// A point in a scaled window's own pixels to where it's drawn, and back.
ImVec2 drawn_at(const Scaled& s, ImVec2 p)
{
    return ImVec2(s.origin.x + (p.x - s.origin.x) * s.scale, s.origin.y + (p.y - s.origin.y) * s.scale);
}
ImVec2 own_at(const Scaled& s, ImVec2 p)
{
    return ImVec2(s.origin.x + (p.x - s.origin.x) / s.scale, s.origin.y + (p.y - s.origin.y) / s.scale);
}

// The mouse as ImGui is to see it in the scaled window. While ImGui drags that window (its title, its body) the mouse
// keeps the shift it had at the press, so the window follows it pixel for pixel: in own_at's pixels, from the corner the
// window moved to the frame before, each frame's move came back bigger (1 / scale) and the window jumped about.
ImVec2 mouse_in(const Scaled& s, ImVec2 mouse)
{
    const ImGuiWindow* moving = GImGui->MovingWindow;
    if (moving != nullptr && moving->RootWindow == s.window)
        return ImVec2(mouse.x + g_move_shift.x, mouse.y + g_move_shift.y);
    const ImVec2 at = own_at(s, mouse);
    g_move_shift = ImVec2(at.x - mouse.x, at.y - mouse.y);
    return at;
}

void scale_vertices(ImDrawList* list, int from, int to, const Scaled& s)
{
    to = std::min(to, list->VtxBuffer.Size);
    for (int i = std::max(0, from); i < to; ++i)
        list->VtxBuffer.Data[i].pos = drawn_at(s, list->VtxBuffer.Data[i].pos);
}

// A window's whole draw list, its clipping with it.
void scale_list(ImDrawList* list, const Scaled& s)
{
    scale_vertices(list, 0, list->VtxBuffer.Size, s);
    for (ImDrawCmd& command : list->CmdBuffer) {
        const ImVec2 a = drawn_at(s, ImVec2(command.ClipRect.x, command.ClipRect.y));
        const ImVec2 b = drawn_at(s, ImVec2(command.ClipRect.z, command.ClipRect.w));
        command.ClipRect = ImVec4(a.x, a.y, b.x, b.y);
    }
}

// What the open window drew on the foreground and background lists since it began (or a window inside it ended).
void close_segment(const Open& open)
{
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (fg->VtxBuffer.Size > open.fg)
        open.seen->drawn.push_back({fg, open.fg, fg->VtxBuffer.Size});
    if (bg->VtxBuffer.Size > open.bg)
        open.seen->drawn.push_back({bg, open.bg, bg->VtxBuffer.Size});
}

// ---- config\layout.json -------------------------------------------------------------------------

void load()
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_file.c_str(), L"rb") != 0 || f == nullptr)
        return;
    std::string text;
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof buffer, f)) > 0;)
        text.append(buffer, n);
    fclose(f);
    json::Value root;
    std::string error;
    if (!json::parse(text, root, error)) {
        log::write(L"Layout: config\\layout.json isn't JSON (%hs): nothing in it used", error.c_str());
        return;
    }
    const json::Value* windows = root.find("windows");
    if (windows == nullptr || !windows->is_object())
        return;
    for (const auto& [name, value] : windows->object) {
        if (!value.is_object() || name.empty())
            continue;
        Entry e;
        e.owner = value.get_string("owner");
        e.x = static_cast<float>(value.get_number("x"));
        e.y = static_cast<float>(value.get_number("y"));
        e.placed = value.get_bool("placed");
        e.dx = static_cast<float>(value.get_number("dx"));
        e.dy = static_cast<float>(value.get_number("dy"));
        e.scale = std::clamp(static_cast<float>(value.get_number("scale", 1)), 0.25f, 4.0f);
        e.objects = value.get_bool("objects");
        e.locked = value.get_bool("locked");
        e.no_pointer = value.get_bool("no_pointer");
        g_entries[name] = e;
    }
    log::write(L"Layout: %u window(s) from config\\layout.json", static_cast<unsigned>(g_entries.size()));
}

// A window to a line, so it reads (and diffs) as a list.
void save()
{
    g_dirty = false;
    if (g_file.empty())
        return;
    std::string text = "{\n  \"windows\": {";
    bool first = true;
    for (const auto& [name, e] : g_entries) {
        json::Value v = json::Value::make_object();
        v["owner"] = e.owner;
        v["x"] = std::round(e.x);
        v["y"] = std::round(e.y);
        v["placed"] = e.placed;
        v["dx"] = std::round(e.dx);
        v["dy"] = std::round(e.dy);
        v["scale"] = std::round(e.scale * 1000) / 1000;
        if (e.objects)
            v["objects"] = true;
        if (e.locked)
            v["locked"] = true;
        if (e.no_pointer)
            v["no_pointer"] = true;
        text += first ? "\n    " : ",\n    ";
        text += json::dump(json::Value(name)) + ": " + json::dump(v);
        first = false;
    }
    text += first ? "}\n}\n" : "\n  }\n}\n";
    const std::wstring temporary = g_file + L".writing";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temporary.c_str(), L"wb") != 0 || f == nullptr) {
        log::write(L"Layout: config\\layout.json couldn't be written");
        return;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!ok || !MoveFileExW(temporary.c_str(), g_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        log::write(L"Layout: config\\layout.json couldn't be written");
    }
}

// ---- Placing ------------------------------------------------------------------------------------

// Its addon places window `name` this frame, its top-left at `top_left` (`raw` what it gave ImGui, a pivot not taken
// off): how far to move it from there.
ImVec2 addon_places(const std::string& name, Seen& s, Entry* e, ImVec2 top_left, ImVec2 raw)
{
    const int frame = GImGui->FrameCount;
    if (s.placed_frame < frame - 1)
        s.gave_valid = s.echo = false;
    s.pinned = s.placed_frame >= frame - 1;
    s.placed_frame = frame;
    if (e == nullptr)
        return ImVec2(0, 0);
    if (e->placed && s.pinned) {
        // it places it every frame: where it was set is kept as an offset from where its addon puts it
        e->placed = false;
        e->dx = e->x - top_left.x;
        e->dy = e->y - top_left.y;
        s.gave_valid = s.echo = false;
        touch();
    }
    if (e->placed || (e->dx == 0 && e->dy == 0)) {
        s.gave_valid = false;
        return ImVec2(0, 0);
    }
    // it asks for where it went last frame: it may have read that back to keep as its own place. Left there for a frame;
    // asked for again the next, it has (the offset goes), else it was only moving (what it's anchored to moved by as much)
    if (s.echo) {
        s.echo = false;
        if (same(raw, s.asked)) {
            log::write(L"Layout: %hs keeps \"%hs\" where it was moved to: its offset is taken off", s.owner.c_str(), name.c_str());
            e->dx = e->dy = 0;
            s.gave_valid = false;
            touch();
            return ImVec2(0, 0);
        }
    } else if (s.gave_valid && same(raw, s.gave) && !same(raw, s.asked)) {
        s.echo = true;
        s.asked = s.gave = raw;
        return ImVec2(0, 0);
    }
    s.asked = raw;
    s.gave = ImVec2(raw.x + e->dx, raw.y + e->dy);
    s.gave_valid = true;
    return ImVec2(e->dx, e->dy);
}

// ---- The mouse ----------------------------------------------------------------------------------

// A mouse button is down, as ImGui last took the buttons
bool mouse_held()
{
    bool held = false;
    for (const bool down : GImGui->IO.MouseDown)
        held = held || down;
    return held;
}

// The window under the mouse as the last frame drew them, front first: ImGui's own search (FindHoveredWindow), each window
// looked at in the pixels of the scaled window it's part of (`in`, null if none).
ImGuiWindow* window_under(ImVec2 mouse, const Scaled*& in)
{
    ImGuiContext& g = *GImGui;
    const ImVec2 padding = g.Style.TouchExtraPadding;
    const ImVec2 padding_edges = g.IO.ConfigWindowsResizeFromEdges ? ImMax(padding, ImVec2(4.0f, 4.0f)) : padding;
    in = nullptr;
    for (int i = g.Windows.Size - 1; i >= 0; --i) {
        ImGuiWindow* window = g.Windows[i];
        if (!window->Active || window->Hidden || (window->Flags & ImGuiWindowFlags_NoMouseInputs) != 0)
            continue;
        const Scaled* s = scale_of_window(window);
        ImRect box(window->OuterRectClipped);
        const bool edges = (window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) == 0;
        box.Expand(edges ? padding_edges : padding);
        if (box.Contains(s != nullptr ? own_at(*s, mouse) : mouse)) {
            in = s;
            return window;
        }
    }
    return nullptr;
}

} // namespace

void install(const wchar_t* install_dir)
{
    std::wstring dir = install_dir != nullptr ? install_dir : L"";
    if (dir.empty())
        return;
    if (dir.back() != L'\\')
        dir += L'\\';
    CreateDirectoryW((dir + L"config").c_str(), nullptr);
    g_file = dir + L"config\\layout.json";
    load();
}

void before_new_frame(float& x, float& y)
{
    g_mouse_window = g_hovered = nullptr;
    g_set_hovered = false;
    if (GImGui == nullptr || g_scaled.empty()) {
        g_held = nullptr;
        return;
    }
    const bool held = mouse_held();
    const ImVec2 mouse(x, y);
    const Scaled* in = nullptr;
    g_hovered = window_under(mouse, in);
    // a press stays in the window it began in (a drag, a slider) until it's let go
    if (held)
        in = scaled_of(g_held);
    else
        g_held = in != nullptr ? in->window : nullptr;
    if (in != nullptr) {
        const ImVec2 at = mouse_in(*in, mouse);
        x = at.x;
        y = at.y;
        g_mouse_window = in->window;
    }
    g_set_hovered = !held;
}

void mouse_at(float& x, float& y)
{
    if (GImGui == nullptr || g_scaled.empty())
        return;
    const Scaled* in = nullptr;
    if (mouse_held())
        in = scaled_of(g_held); // as before_new_frame: a press stays in the window it began in
    else
        window_under(ImVec2(x, y), in);
    if (in == nullptr)
        return;
    const ImVec2 at = mouse_in(*in, ImVec2(x, y));
    x = at.x;
    y = at.y;
}

void after_new_frame()
{
    if (!g_set_hovered || GImGui == nullptr)
        return;
    // ImGui looked for the window under the mouse with it in one window's pixels: the one drawn there is the one. Not when
    // ImGui found none (a modal popup, a press that began outside its windows), or a window is being moved.
    ImGuiContext& g = *GImGui;
    if (g.MovingWindow != nullptr || g.HoveredWindow == nullptr)
        return;
    g.HoveredWindow = g.HoveredWindowUnderMovingWindow = g_hovered;
}

namespace {
void restore_clip()
{
    if (!g_clip_widened || GImGui == nullptr)
        return;
    g_clip_widened = false;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    viewport->Size = g_clip_size;
    viewport->WorkSize = g_clip_work;
}

// The viewport display / `scale` big (a scale below 1), else as it is
void widen_viewport(float scale)
{
    restore_clip();
    if (GImGui == nullptr || !(scale > 0.0f && scale < 1.0f))
        return;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    g_clip_size = viewport->Size;
    g_clip_work = viewport->WorkSize;
    const ImVec2 grown(viewport->Size.x / scale, viewport->Size.y / scale);
    viewport->WorkSize.x += grown.x - viewport->Size.x;
    viewport->WorkSize.y += grown.y - viewport->Size.y;
    viewport->Size = grown;
    g_clip_widened = true;
}

// An addon's or plugin's window is kept on the screen by ImGui's own margin, not VanaCore's (gui.cpp begin_frame: a
// 20th of the screen's height, for its own windows): FancyChat's input bar sat under the chat, nearer the screen's
// bottom than that (reported 2026-11-18, Crescelius). Swapped for its Begin, put back after it.
ImVec2 g_own_padding;
bool g_padding_swapped = false;

void addon_padding(bool own)
{
    if (own || g_padding_swapped)
        return;
    ImGuiStyle& style = GImGui->Style;
    g_own_padding = style.DisplayWindowPadding;
    style.DisplayWindowPadding = ImGuiStyle().DisplayWindowPadding;
    g_padding_swapped = true;
}

void own_padding()
{
    if (g_padding_swapped && GImGui != nullptr)
        GImGui->Style.DisplayWindowPadding = g_own_padding;
    g_padding_swapped = false;
}

// Window `name`'s Begin at `scale`: kept on the screen, then the viewport widened
void widen_clip(const char* name, float scale)
{
    if (GImGui == nullptr || name == nullptr || !(scale > 0.0f && scale < 1.0f))
        return;
    ImGuiContext& g = *GImGui;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    // not placed by its addon this frame: kept on the screen (what ImGui does with a window as it is begun, which
    // would take the widened viewport for the screen)
    if (window != nullptr && window->WasActive && (g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasPos) == 0) {
        const ImVec2 pad = ImMax(g.Style.DisplayWindowPadding, g.Style.DisplaySafeAreaPadding);
        const ImVec2 size(window->Size.x * scale, window->Size.y * scale);
        const ImVec2 low(viewport->WorkPos.x + pad.x - size.x, viewport->WorkPos.y + pad.y - size.y);
        const ImVec2 high(viewport->WorkPos.x + viewport->WorkSize.x - pad.x, viewport->WorkPos.y + viewport->WorkSize.y - pad.y);
        const ImVec2 kept(ImClamp(window->Pos.x, low.x, std::max(low.x, high.x)), ImClamp(window->Pos.y, low.y, std::max(low.y, high.y)));
        if (!same(kept, window->Pos))
            ImGui::SetNextWindowPos(kept, ImGuiCond_Always);
    }
    widen_viewport(scale);
}
} // namespace

int before_begin(const char* name, const std::string& owner)
{
    if (name == nullptr || GImGui == nullptr)
        return 0;
    restore_clip(); // (an End-less Begin before)
    own_padding();
    ImGuiContext& g = *GImGui;
    const int frame = g.FrameCount;
    Seen& s = g_seen[name];
    if (!owner.empty())
        s.owner = owner;
    addon_padding(own_owner(s.owner) || own_window_name(name));
    if (s.frame == frame)
        return flags_of(entry_of(name)); // begun again this frame: what it draws is added to the window as it is
    const bool appearing = s.frame != frame - 1;
    s.frame = frame;
    ImGuiNextWindowData& next = g.NextWindowData;
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    Entry* e = entry_of(name);
    const bool move = std::exchange(s.move, false);

    // its place: what its addon gives moved by the offset; a placed one put back where it was set as it appears (or at
    // once, moved from the Layout tab), unless its addon places it every frame
    const bool addon_placed = (next.HasFlags & ImGuiNextWindowDataFlags_HasPos) != 0 && (next.PosCond & ImGuiCond_Always) != 0;
    if (addon_placed) {
        const ImVec2 size = window != nullptr ? window->Size : ImVec2(0, 0);
        const ImVec2 top_left(next.PosVal.x - size.x * next.PosPivotVal.x, next.PosVal.y - size.y * next.PosPivotVal.y);
        const ImVec2 d = addon_places(name, s, e, top_left, next.PosVal);
        next.PosVal.x += d.x;
        next.PosVal.y += d.y;
    } else if (s.placed_frame < frame - 1) {
        s.gave_valid = false;
        s.pinned = false;
        if (e != nullptr && !e->placed && !appearing && window != nullptr) {
            // its addon stopped placing it: ImGui does now, from where it is
            e->placed = true;
            e->x = window->Pos.x;
            e->y = window->Pos.y;
            e->dx = e->dy = 0;
            touch();
        }
    }
    if (e != nullptr && e->placed && (appearing || move) && !(addon_placed && s.pinned))
        ImGui::SetNextWindowPos(ImVec2(e->x, e->y), ImGuiCond_Always);
    widen_clip(name, (e != nullptr ? e->scale : 1.0f) * (own_owner(s.owner) ? g_own_scale : 1.0f));
    return flags_of(e);
}

void clip_begin(const char* name)
{
    if (name != nullptr && own_window_name(name))
        widen_clip(name, g_own_scale);
}

void clip_end()
{
    restore_clip();
}

void after_begin()
{
    restore_clip();
    own_padding();
    if (GImGui == nullptr)
        return;
    ImGuiWindow* window = GImGui->CurrentWindow;
    const auto found = window != nullptr && (window->Flags & kNotTopLevel) == 0 ? g_seen.find(window->Name) : g_seen.end();
    if (found == g_seen.end()) {
        if (!g_open.empty())
            widen_viewport(g_open.back().scale); // inside the window it was begun in, as that one is
        return;
    }
    Seen& s = found->second;
    s.window = window;
    if (s.drawn_frame != GImGui->FrameCount) {
        s.drawn_frame = GImGui->FrameCount;
        s.drawn.clear();
    }
    // begun inside another window: what that one drew is up to here
    if (!g_open.empty())
        close_segment(g_open.back());
    const Entry* e = entry_of(found->first);
    const float scale = (e != nullptr ? e->scale : 1.0f) * (own_owner(s.owner) ? g_own_scale : 1.0f);
    g_open.push_back({window, &s, ImGui::GetForegroundDrawList()->VtxBuffer.Size, ImGui::GetBackgroundDrawList()->VtxBuffer.Size, scale});
    // what's begun inside it (its drop-down lists, popups and tooltips) clipped as it is
    widen_viewport(scale);
}

void before_end()
{
    if (GImGui == nullptr || g_open.empty() || g_open.back().window != GImGui->CurrentWindow)
        return;
    restore_clip();
    close_segment(g_open.back());
    g_open.pop_back();
    if (!g_open.empty()) { // back in the window it was begun inside
        g_open.back().fg = ImGui::GetForegroundDrawList()->VtxBuffer.Size;
        g_open.back().bg = ImGui::GetBackgroundDrawList()->VtxBuffer.Size;
        widen_viewport(g_open.back().scale);
    }
}

void window_pos(const char* name, float& x, float& y, int cond)
{
    if (GImGui == nullptr || (cond != 0 && (cond & ImGuiCond_Always) == 0))
        return;
    ImGuiWindow* window = name != nullptr ? ImGui::FindWindowByName(name) : GImGui->CurrentWindow;
    if (window == nullptr || (window->Flags & kNotTopLevel) != 0)
        return;
    const auto found = g_seen.find(window->Name);
    if (found == g_seen.end())
        return;
    const ImVec2 d = addon_places(found->first, found->second, entry_of(found->first), ImVec2(x, y), ImVec2(x, y));
    x += d.x;
    y += d.y;
}

void end_frame()
{
    if (GImGui == nullptr)
        return;
    restore_clip();
    ImGuiContext& g = *GImGui;
    const int frame = g.FrameCount;
    // a window its addon didn't end (ImGui ends it at the frame's end): what it drew up to now
    while (!g_open.empty()) {
        close_segment(g_open.back());
        g_open.pop_back();
    }
    g_scaled.clear();
    g_shown.clear();
    for (auto& [name, s] : g_seen) {
        if (s.frame != frame || s.window == nullptr || (s.window->Flags & kNotTopLevel) != 0)
            continue;
        s.pos = s.window->Pos;
        s.size = s.window->Size;
        Entry* e = entry_of(name);
        if (e != nullptr && !s.move) { // (a move set after its Begin this frame happens at the next)
            if (!same(ImVec2(e->x, e->y), s.pos)) {
                e->x = s.pos.x;
                e->y = s.pos.y;
                // a placed one dragged (not where its addon put it this frame): kept there
                if (e->placed && s.placed_frame != frame)
                    touch();
            }
            if (e->owner != s.owner && !s.owner.empty()) {
                e->owner = s.owner;
                touch();
            }
        }
        const float set_scale = e != nullptr ? e->scale : 1.0f;
        const float scale = set_scale * (own_owner(s.owner) ? g_own_scale : 1.0f);
        if (scale != 1)
            g_scaled.push_back({s.window, s.pos, scale});
        Window w;
        w.name = name;
        w.owner = s.owner;
        w.x = s.pos.x;
        w.y = s.pos.y;
        w.w = s.size.x * scale;
        w.h = s.size.y * scale;
        w.scale = set_scale;
        w.shown = true;
        w.pinned = s.pinned;
        w.saved = e != nullptr;
        w.locked = e != nullptr && e->locked;
        w.no_pointer = e != nullptr && e->no_pointer;
        g_shown.push_back(std::move(w));
    }
    no_pointer_children();
    // VanaCore's own windows its C++ draws (not begun through before_begin): at its interface's size too
    if (g_own_scale != 1)
        for (ImGuiWindow* window : g.Windows)
            if (window->Active && (window->Flags & kNotTopLevel) == 0 && own_window(window) &&
                g_seen.find(window->Name) == g_seen.end())
                g_scaled.push_back({window, window->Pos, g_own_scale});
    // drawn at their scale: each window that's part of a scaled one (itself, its child windows, its popups and a tooltip
    // at the mouse over it), from that one's top-left corner, and what each scaled one drew on the screen-wide lists
    if (!g_scaled.empty()) {
        for (ImGuiWindow* window : g.Windows)
            if (window->Active)
                if (const Scaled* s = scale_of_window(window))
                    scale_list(window->DrawList, *s);
        for (const auto& [name, s] : g_seen)
            if (s.frame == frame && s.drawn_frame == frame)
                if (const Scaled* scaled = scaled_of(s.window))
                    for (const Segment& segment : s.drawn)
                        scale_vertices(segment.list, segment.from, segment.to, *scaled);
    }
    // the Layout tab's outline, round the window as it's drawn
    if (g_highlight_frame >= frame - 1) {
        for (const std::vector<Window>* shown : {&g_shown, &g_objects_shown}) {
            for (const Window& w : *shown) {
                if (w.name != g_highlight)
                    continue;
                ImDrawList* draw = ImGui::GetForegroundDrawList();
                const ImVec2 a(w.x - 2, w.y - 2), b(w.x + w.w + 2, w.y + w.h + 2);
                draw->AddRectFilled(a, b, IM_COL32(122, 162, 247, 40));
                draw->AddRect(a, b, IM_COL32(122, 162, 247, 255), 0, 0, 3.0f);
            }
        }
    }
    if (g_dirty && GetTickCount64() - g_changed >= kSaveDelayMs)
        save();
}

void visual_rect(const ImGuiWindow* window, float& x0, float& y0, float& x1, float& y1)
{
    const Scaled* s = scale_of_window(window);
    if (s == nullptr)
        return;
    const ImVec2 a = drawn_at(*s, ImVec2(x0, y0)), b = drawn_at(*s, ImVec2(x1, y1));
    x0 = a.x;
    y0 = a.y;
    x1 = b.x;
    y1 = b.y;
}

bool drawing_scale(const std::string& owner, float x, float y, float& origin_x, float& origin_y, float& scale)
{
    if (g_scaled.empty() || owner.empty())
        return false;
    const Scaled* best = nullptr;
    float best_area = 0;
    for (const auto& [name, s] : g_seen) {
        if (s.owner != owner || x < s.pos.x || y < s.pos.y || x >= s.pos.x + s.size.x || y >= s.pos.y + s.size.y)
            continue;
        const Scaled* scaled = scaled_of(s.window); // (drawn scaled in the last frame)
        const float area = s.size.x * s.size.y;
        if (scaled != nullptr && (best == nullptr || area < best_area)) {
            best = scaled;
            best_area = area;
        }
    }
    if (best == nullptr)
        return false;
    origin_x = best->origin.x;
    origin_y = best->origin.y;
    scale = best->scale;
    return true;
}

std::vector<ObjectMove> objects(const std::vector<ObjectGroup>& groups)
{
    g_objects_shown.clear();
    std::vector<ObjectMove> moves;
    moves.reserve(groups.size());
    for (const ObjectGroup& g : groups) {
        Entry* e = entry_of(g.name);
        ObjectMove move;
        if (e != nullptr) {
            e->objects = true;
            move = {e->dx, e->dy, e->scale};
            e->x = g.x + e->dx; // where it is drawn, for when it isn't
            e->y = g.y + e->dy;
            if (e->owner != g.owner) {
                e->owner = g.owner;
                touch();
            }
        }
        moves.push_back(move);
        Window w;
        w.name = g.name;
        w.owner = g.owner;
        w.x = g.x + move.dx;
        w.y = g.y + move.dy;
        w.w = g.w * move.scale;
        w.h = g.h * move.scale;
        w.scale = move.scale;
        w.shown = true;
        w.saved = e != nullptr;
        g_objects_shown.push_back(std::move(w));
    }
    return moves;
}

std::vector<Window> windows()
{
    std::vector<Window> out = g_shown;
    out.insert(out.end(), g_objects_shown.begin(), g_objects_shown.end());
    for (const auto& [name, e] : g_entries) {
        const auto named = [&](const Window& w) { return w.name == name; };
        if (std::any_of(g_shown.begin(), g_shown.end(), named) || std::any_of(g_objects_shown.begin(), g_objects_shown.end(), named))
            continue;
        Window w;
        w.name = name;
        w.owner = e.owner;
        w.x = e.x;
        w.y = e.y;
        w.scale = e.scale;
        w.pinned = !e.objects && !e.placed && (e.dx != 0 || e.dy != 0);
        w.saved = true;
        w.locked = e.locked;
        w.no_pointer = e.no_pointer;
        out.push_back(std::move(w));
    }
    return out;
}

void set(const std::string& name, const float* x, const float* y, const float* scale)
{
    if (name.empty())
        return;
    // An owner's fonts and primitives: moved by an offset from where they're drawn (x, y there are where they are now)
    const auto drawn = std::find_if(g_objects_shown.begin(), g_objects_shown.end(), [&](const Window& w) { return w.name == name; });
    const bool objects = drawn != g_objects_shown.end() || (entry_of(name) != nullptr && entry_of(name)->objects);
    if (objects) {
        const bool created = g_entries.find(name) == g_entries.end();
        Entry& e = g_entries[name];
        e.objects = true;
        if (created && drawn != g_objects_shown.end()) {
            e.owner = drawn->owner;
            e.x = drawn->x;
            e.y = drawn->y;
        }
        // (e.x, e.y) is where its objects are drawn: e.x - e.dx where they'd be without the offset
        if (x != nullptr) {
            e.dx = *x - (e.x - e.dx);
            e.x = *x;
        }
        if (y != nullptr) {
            e.dy = *y - (e.y - e.dy);
            e.y = *y;
        }
        if (scale != nullptr && std::isfinite(*scale))
            e.scale = std::clamp(*scale, 0.25f, 4.0f);
        touch();
        return;
    }
    const auto seen = g_seen.find(name);
    Seen* s = seen != g_seen.end() ? &seen->second : nullptr;
    const bool shown = s != nullptr && GImGui != nullptr && s->frame >= GImGui->FrameCount - 1;
    const bool created = g_entries.find(name) == g_entries.end();
    Entry& e = g_entries[name];
    if (created && shown) {
        e.x = s->pos.x;
        e.y = s->pos.y;
        e.placed = true; // made an offset at its Begin if its addon places it every frame
    }
    if (s != nullptr && !s->owner.empty())
        e.owner = s->owner;
    if (x != nullptr || y != nullptr) {
        e.x = x != nullptr ? *x : e.x;
        e.y = y != nullptr ? *y : e.y;
        e.placed = true;
        e.dx = e.dy = 0;
        if (s != nullptr)
            s->move = true;
    }
    if (scale != nullptr && std::isfinite(*scale))
        e.scale = std::clamp(*scale, 0.25f, 4.0f);
    touch();
}

void set_flags(const std::string& name, const bool* locked, const bool* no_pointer)
{
    if (name.empty() || (locked == nullptr && no_pointer == nullptr))
        return;
    const auto objects = [&](const Window& w) { return w.name == name; };
    const Entry* had = entry_of(name);
    if ((had != nullptr && had->objects) || std::any_of(g_objects_shown.begin(), g_objects_shown.end(), objects))
        return; // (text and shapes aren't an ImGui window)
    const auto seen = g_seen.find(name);
    const Seen* s = seen != g_seen.end() ? &seen->second : nullptr;
    if (had == nullptr && s == nullptr)
        return; // not seen since the game started, nor in layout.json
    const bool shown = s != nullptr && GImGui != nullptr && s->frame >= GImGui->FrameCount - 1;
    Entry& e = g_entries[name];
    if (had == nullptr && shown) { // made as set() makes one: where it is now
        e.x = s->pos.x;
        e.y = s->pos.y;
        e.placed = true;
    }
    if (s != nullptr && !s->owner.empty())
        e.owner = s->owner;
    if (locked != nullptr)
        e.locked = *locked;
    if (no_pointer != nullptr)
        e.no_pointer = *no_pointer;
    touch();
}

void reset(const std::string& name)
{
    const auto found = g_entries.find(name);
    if (found == g_entries.end())
        return;
    const Entry& was = found->second;
    if (was.locked || was.no_pointer) {
        // its lock and no pointer events stay (not its place: the Scale slider's reset is this too). Not placed, no offset:
        // one its addon places goes back there, another stays where it is (made placed there at its next Begin)
        Entry kept;
        kept.owner = was.owner;
        kept.x = was.x;
        kept.y = was.y;
        kept.locked = was.locked;
        kept.no_pointer = was.no_pointer;
        found->second = kept;
    } else {
        g_entries.erase(found);
    }
    touch();
}

void highlight(const std::string& name)
{
    g_highlight = name;
    g_highlight_frame = GImGui != nullptr ? GImGui->FrameCount : 0;
}

void set_own_scale(float scale)
{
    g_own_scale = std::isfinite(scale) && scale > 0 ? scale : 1.0f;
}

std::string owner_of(const std::string& name)
{
    const auto seen = g_seen.find(name);
    return seen != g_seen.end() ? seen->second.owner : std::string();
}

} // namespace vanacore::layout
