#pragma once

// [client] status: the Status window (the character's name, jobs, item level, HP/MP/TP and the seven stats at the top
// left while the main menu or Equipment is open) laid out by VanaCore: the name bigger, in its own True Font
// (status.name.font, drawn whole: statuswindow_name.h) or, that off, the menu titles' font, the jobs as "Level 75 SAM" (status.jobs=full), "75 SAM / 37 WHM" on
// one row (simple), "Lv 75 SAM" (short), "Lv 75 Samurai" (long) or the game's "Lv75 Samurai" (game), the ILv / Su row
// shown or not (status.itemlevel), HP, MP and TP in colour (status.colors, status.hp.color, .mp.color, .tp.color: their
// labels and current numbers), a line before STR (status.separator, status.separator.color) and the rows spaced out,
// the window as tall as they need and the windows under it moved down (status.taller). The window's own labels (its picture's letters) are hidden and written again where the layout puts
// them; the game's warnings stay: the level in blue under level sync, a lowered maximum or stat in red with its mark.
// The Level / EXP window under it, with or without status: its "Level 75" row left out and the window a row shorter
// (status.exp.level=0), "NEXT" for "NEXTLevel" (status.exp.shortnext).
namespace vanacore::statuswindow {

// `ini` (vanacore.ini): [client] status* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's drawing of the window the first time status is on (once
// FFXiMain.dll is unpacked).
void on_frame();

// In VanaCore's interface pass (overlay.cpp), the drawn pixels `width` x `height`: the name the window's draw kept this
// frame, in its True Font (status.name.font)
void draw(void* device, unsigned width, unsigned height);

// For hudwin.cpp's hook of the game's SetLocate: how far status.taller has moved `window` down (one of the windows under
// the Status window, still where VanaCore put it), else 0.
int lowered(const void* window);

} // namespace vanacore::statuswindow
