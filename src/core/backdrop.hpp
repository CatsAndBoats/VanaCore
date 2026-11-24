#pragma once

struct lua_State;
struct ImDrawList;

// Frosted glass behind VanaCore's own ImGui windows: the picture behind a rectangle shown blurred, under the window's
// translucent fill. The renderer does the blur at Present (Direct3D 12's: gfx_d3d12_backdrop.h); this is the ask.
//
//   vana.ui.backdrop(x, y, w, h [, radius [, strength]]) -> boolean
//
// Called each frame, inside the window's ImGui::Begin, with the rectangle in the window's own drawing pixels (the ones its
// draw list takes; a window drawn scaled by Addons > Layout is followed). Up to 8 at a time. True when the renderer draws it
// (the one before this frame's answer: false in the first frame, and always with a renderer that can't), so the window can
// choose a fill that lets the blur show, or a plainer one. The strength is 0 to 1 (beyond that is held to it): 0 hardly blurred,
// 0.5 the default, 1 about twice that.
namespace vanacore::backdrop {

constexpr float kDefaultStrength = 0.5f;

// Puts `backdrop` into the table on the top of the stack (vana.ui's, lua_api.cpp's open_ui).
void add(lua_State* L);

// The same from C++ (the plugin API's backdrop): the rectangle (x, y, w, h, corner radius, ImGui's pixels as the window being
// drawn takes them) asked for this frame, from inside its ImGui window, and how blurred (0 to 1). True when the renderer drew
// the last frame's.
bool ask(float x, float y, float w, float h, float radius, float strength = kDefaultStrength);

// Once a frame, from the overlay's draw just before the interface is drawn: the rectangles asked for this frame go to the
// renderer, in the window's pixels (the layer's), and are forgotten; with `shown` false (the interface is hidden) they are
// only forgotten.
void flush(bool shown);

// From the interface's draw (gui.cpp's render), before each ImGui draw list: at the first one of a window that asked, the
// interface is split (vanacore_gfx_ui_split), so that the windows drawn before it are blurred with the picture behind the glass.
void before_list(const ImDrawList* list);

} // namespace vanacore::backdrop
