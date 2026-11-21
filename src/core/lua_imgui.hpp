#pragma once

struct lua_State;

// ImGui for Lua (gui.hpp's context), as Ashita's Addons.dll binds IGuiManager for its addons, so
// their imgui library (AshitaCore:GetGuiManager(), compat/ashita.lua) works unchanged:
//   - functions are called with a dot (imgui.Begin(...)), and a name ImGui doesn't have is nil;
//   - a pointer is a table: [1] is read and written back ([1]..[N] for an array); p_open that isn't
//     a table is no pointer;
//   - ImVec2 and ImVec4 go in as tables ({x, y}, a missing component 0) and come out as several
//     numbers; the fields of ImGui's structures (io.DisplaySize, style.Colors[i], 1-based) are live
//     objects with .x .y .z .w;
//   - ImU32 colours are numbers (negative ones as bit.bor makes them too), textures their pointer as
//     a number;
//   - missing or nil arguments take the C++ defaults, extra ones are ignored;
//   - printf-style text (Text, TextColored, SetTooltip, ...) is its format with no arguments: %% is %.
// Functions that draw do nothing outside a frame (an addon's command handler).
namespace vanacore::lua_imgui {

// A new table of the functions, left on the stack (vana.imgui).
void push(lua_State* L);

} // namespace vanacore::lua_imgui
