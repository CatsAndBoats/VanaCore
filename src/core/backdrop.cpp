#include "backdrop.hpp"

#include <windows.h>

#include <lua.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "graphics.hpp"
#include "layout.hpp"

namespace vanacore::backdrop {
namespace {

constexpr size_t kMax = 8; // as the renderer's BD_MAX

struct Ask {
    float x, y, w, h, radius; // ImGui's pixels (the game's back buffer size), as drawn
    float strength;           // 0 to 1: how blurred (0.5 the default)
};

std::vector<Ask> g_asks;
std::vector<const ImDrawList*> g_lists; // the draw lists of the windows that asked this frame (compared, never read)
int g_frame = -1;                       // the ImGui frame the rectangles were asked in
bool g_drawn = false;                   // the renderer drew the last flush's rectangles
bool g_splitting = false;               // this frame's are to be drawn (flush's answer), and the interface not split yet

// vana.ui.backdrop(x, y, w, h [, radius [, strength]])
int ui_backdrop(lua_State* L)
{
    const float x = static_cast<float>(luaL_checknumber(L, 1)), y = static_cast<float>(luaL_checknumber(L, 2));
    const float w = static_cast<float>(luaL_checknumber(L, 3)), h = static_cast<float>(luaL_checknumber(L, 4));
    lua_pushboolean(L,
        ask(x, y, w, h, static_cast<float>(luaL_optnumber(L, 5, 0)), static_cast<float>(luaL_optnumber(L, 6, kDefaultStrength)))
            ? 1
            : 0);
    return 1;
}

} // namespace

bool ask(float x, float y, float w, float h, float radius, float strength)
{
    // inside a window only: asked from anywhere else (between frames, the MCP's lua) there is no window to follow (and
    // ImGui::GetCurrentWindow() writes to the current one, which then isn't there)
    const ImGuiContext* context = ImGui::GetCurrentContext();
    const ImGuiWindow* window = context != nullptr && context->WithinFrameScope ? context->CurrentWindow : nullptr;
    if (window != nullptr && ImGui::GetFrameCount() != g_frame) {
        g_asks.clear(); // (a frame the overlay didn't flush: those were another frame's)
        g_lists.clear();
        g_frame = ImGui::GetFrameCount();
    }
    if (window != nullptr && g_lists.size() < kMax && std::find(g_lists.begin(), g_lists.end(), window->DrawList) == g_lists.end())
        g_lists.push_back(window->DrawList);
    if (window != nullptr && g_asks.size() < kMax && w > 0 && h > 0) {
        // a window drawn scaled (Addons > Layout) is drawn from its corner by its scale: the rectangle is where that puts it
        float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        layout::visual_rect(window, x0, y0, x1, y1);
        const float scale = w > 0 ? (x1 - x0) / w : 1.0f;
        float level = kDefaultStrength; // (NaN stays that: both comparisons are false)
        if (strength >= 0.0f)
            level = std::min(strength, 1.0f);
        else if (strength < 0.0f)
            level = 0.0f;
        g_asks.push_back({x0, y0, x1 - x0, y1 - y0, std::max(0.0f, radius) * scale, level});
    }
    return g_drawn;
}

void add(lua_State* L)
{
    lua_pushcfunction(L, &ui_backdrop);
    lua_setfield(L, -2, "backdrop");
}

void flush(bool shown)
{
    const auto drop = [] {
        g_asks.clear();
        g_lists.clear();
        g_drawn = g_splitting = false;
    };
    if (g_asks.empty() || !shown) {
        drop();
        return;
    }
    unsigned layer_w = 0, layer_h = 0;
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr || !graphics::ui_size(layer_w, layer_h) || layer_w == 0 || layer_h == 0) {
        drop();
        return;
    }
    // ImGui's pixels are the game's back buffer's; the layer (and the window) its own size: as gui.cpp's render() scales
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!(display.x > 0 && display.y > 0)) {
        drop();
        return;
    }
    const float sx = static_cast<float>(layer_w) / display.x, sy = static_cast<float>(layer_h) / display.y;
    std::vector<float> rects;
    rects.reserve(g_asks.size() * 6);
    for (const Ask& a : g_asks) {
        rects.push_back(a.x * sx);
        rects.push_back(a.y * sy);
        rects.push_back(a.w * sx);
        rects.push_back(a.h * sy);
        rects.push_back(a.radius * sx);
        rects.push_back(a.strength);
    }
    g_drawn = g_splitting = vanacore_gfx_set_backdrops(rects.data(), static_cast<int>(g_asks.size())) != 0;
    g_asks.clear(); // (the lists stay for before_list, the draw that follows)
}

void before_list(const ImDrawList* list)
{
    if (!g_splitting || std::find(g_lists.begin(), g_lists.end(), list) == g_lists.end())
        return;
    g_splitting = false; // the lowest window that asked: what is drawn before it is blurred with the picture
    vanacore_gfx_ui_split();
}

} // namespace vanacore::backdrop
