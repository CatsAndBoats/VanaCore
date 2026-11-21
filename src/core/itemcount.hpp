#pragma once

// [client] items.count: the count on a bag window's title bar (Items, Satchel, Mog Safe, ...), which the game draws as a
// small slanted "17" over "/80", moved (items.count.x, .y), coloured (items.count.color), with the bag's size left off
// (items.count.hidetotal) or after it as 17/80, 17 (80) or 17 - 80 (items.count.separator), small or as big as the count
// (items.count.autototal), in True Font (VanaCore's own text, rendered at the window's pixels, as every True Font area's:
// items.count.font.family (unset the default font, truefont.family), .size (the interface's pixels, 0 the count left out;
// unset items.count.size's percent of 16, else 24), .bold, .italic, .outline, .outline.opacity (unset the default font's)).
// With True Font off everywhere (truefont.disable=1) it's the title's own letters, items.count.size percent bigger.
namespace vanacore::itemcount {

// `ini` (vanacore.ini): [client] items.count* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's drawing of the count at the first frame (FFXiMain.dll unpacked). With
// items.count off it's the game's own count, its letters all kept the game's (True Font took some of them, not all).
void on_frame();

// In VanaCore's own interface (between text::begin and text::end, in the window's `width` x `height` pixels): the sharp
// count, if a bag window was drawn this frame.
void draw(void* device, unsigned width, unsigned height);

} // namespace vanacore::itemcount
