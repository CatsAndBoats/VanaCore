#pragma once

// [client] target.*: the Target window (the box with the target's name and HP bar, at the screen's bottom right in the
// game):
//   target.hide=1        the window off the screen, name, bar and frame (hide, from chat.cpp's window draw hook);
//                        the arrow over the target is still drawn, and targeting is unchanged
//   target.move=1        placed by target.x (its middle) and target.y (its top edge), percents of the interface; the
//                        default is the middle of the top edge. Off, it goes back where the game has it
//   target.width=<px>    that wide in the interface's units (80 up, the game has 112); the bar grows with it
//   target.background=0  the window's background and frame gone (the name and bar stay): the graphics layer draws them
//                        with no alpha (vanacore_gfx_set_hidden_windows)
//   target.label=0       the small "Target" caption on its top edge gone; target.label.x, .y move it (it stays in the
//                        middle as the window widens)
//   target.bar.x, .y     the HP bar moved in the window, in units (left/right, up/down)
//   target.bar.mask=0    the HP bar's white outline gone; with another width the game's outline is always left out and
//                        target.bar.outline (default 1) draws a thin white one of VanaCore's
//   target.lock=game|glow|none   what shows on a locked-on target: the game's picture, a glowing border in
//                        target.lock.color (pulsing with target.lock.pulse), or nothing
//   target.arrow=0       the arrow over the target not drawn (the sub-target cursor stays; targeting is unchanged)
//   target.arrow.image=<file>   a picture (PNG, BMP, JPEG) in the arrow's place, its bottom middle at the arrow's
//                        point, its pixels as the interface's units (the game's arrow is 20 x 32); under VanaCore's
//                        folder unless a full path. An addon's vana.gameui.image('target_arrow') goes before it
namespace vanacore::targetwin {

// `ini` (vanacore.ini): [client] target.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// [client] target.hide (or an addon's vana.gameui.hide('target_window')): for a window of the menu manager (a ctrl) that
// is the Target window, put at -4000, -4000 before the game draws it (the frame it's made again in is covered too), so
// the game's own draw of it shows nothing but the arrow over the target. Called for every window drawn, before the draw,
// on the game's thread; true if it is the Target window and hidden (gameui's skip must leave it to be drawn). on_frame keeps
// it there, and puts it back when the hide is off. While an event script has given the window a name and HP bar of its own
// (fishing: the hooked fish's "???" and HP, which nothing else shows), it's drawn where the game has it whatever hides it,
// still true (gameui's skip leaves it alone).
bool hide(const void* ctrl);

// For a window of the menu manager (a ctrl) that is the Target window and not hidden, before the game draws it: placed,
// sized and dressed as on_frame does after Present. The game makes the window at its own place as something is targeted
// and draws it in that frame; placed only after Present, it showed there for one frame (reported 2026-10-30). Called for
// every window drawn, on the game's thread.
void before_draw(const void* ctrl);

// Once a frame on the game's thread, after Present: puts the window where it's wanted and as it's wanted (a window the
// game made again for a new target is found again).
void on_frame();

// In VanaCore's interface pass (`width` x `height` the drawn pixels): the glowing border of target.lock=glow, and the
// picture in the arrow's place (target.arrow.image, an addon's vana.gameui.image('target_arrow')).
void draw(void* device, unsigned width, unsigned height);

// In ImGui's frame (vana.layout.outline('target')): the window's place drawn as a box (outline.hpp), where target.x and
// target.y put it while nothing's targeted. Shift + drag moves it (target.move on, target.x and .y as it goes, not
// written): the frame it's let go, moved_x and moved_y are where it was left, for the caller to save.
bool outline(double* moved_x = nullptr, double* moved_y = nullptr);

// For vana.gameui.rect('target_window'): where the window is on the screen, in the interface's pixels (`width` x `height`,
// vana.ui.screen's): its left, top, right and bottom. False while it isn't open (nothing targeted), is hidden, or the game's
// window placing isn't known.
bool rect(unsigned width, unsigned height, float& left, float& top, float& right, float& bottom);

} // namespace vanacore::targetwin
