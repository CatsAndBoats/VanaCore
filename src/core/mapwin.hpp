#pragma once

// [client] map.*: the in-game map (Main menu > Map). map.fullscreen=0 shows it in a square box of its own, map.scale percent
// of the screen's height, its left and top edges map.x and map.y percent across and down (Shift + drag on it moves it).
// The map's zoom, scrolling, markers and cursor stay the game's, inside the box. map.opacity makes its picture see-through,
// map.border puts a black border with a drop shadow round the box, and map.wheel (on by default) zooms it with the mouse
// wheel, full screen or not. [client] regionmap.*: the same for the region maps (Region Info > Conquest, Besieged, Campaign,
// Colonization), but for map.track and map.pointzoom.
namespace vanacore::mapwin {

// `ini` (vanacore.ini): [client] map.* and regionmap.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the maps' limits and drawing the first time one of their settings wants them
// (once FFXiMain.dll is there).
void on_frame();

// In ImGui's frame: Shift + drag on a map's box, and a left drag scrolling it.
void gui_frame();

// In ImGui's frame (vana.layout.outline('map') and ('regionmap'), /vana's HUD Manager): map `which`'s box (0 the map, 1 the
// region maps) on ImGui's foreground, open or not, Shift + drag moving it; false while it's full screen. The frame a drag is
// let go, its .x and .y to save.
bool outline(int which, double* moved_x = nullptr, double* moved_y = nullptr);

// vana.layout.up('map') and ('regionmap'), the HUD Manager's Open only: map `which` drawn in its box of late (open, not
// full screen).
bool up(int which);

// The mouse hook's thread: true while the wheel at (x, y), VanaCore's interface's pixels (ui::screen), is a map's.
bool wheel_over(int x, int y);
// The game's thread, for a wheel message: true if it was a map's (it zooms at the map's next draw).
bool on_wheel(int x, int y, int delta);

// The mouse hook's thread: true while a left press at (x, y), VanaCore's interface's pixels, is a map's in a box (it
// drags the map; the game, which would pick the zone there or buzz, never sees it).
bool takes_press(int x, int y);

} // namespace vanacore::mapwin
