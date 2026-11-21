#pragma once

// The game's auto-translate list: Tab while typing opens it over the chat line (FFXiMain's FsCandidateMenu, made with the
// line at FUN_101b76xx and held next to it). The game places it over its own line, at the column the phrase starts at,
// whatever stands for the line on the screen, and at its own place even when the line's window is moved (input.move): it's
// put over the line's window where that is; and addons' drawing (fancychat's chat and input bar, its ImGui tabs) goes
// over the game's interface. With fancychat's input bar showing the line (chat::input_bar_addon), the list is placed
// over that bar instead, at the phrase's column in it, and each frame the game draws the list, its pixels as the game
// drew them are laid back over everything drawn after (raise.cpp), so the list is never under an addon. Everything here
// runs on the game's thread. While the list is open, the line's caret is drawn after the phrase it offers, not before it.
namespace vanacore::phraselist {

// Each frame at Present, before addons' frames: the first time, finds the list and hooks where it's placed; then, while
// fancychat's bar stands for the line, asks fancychat where the bar is. `width` x `height`: the back buffer's size, the
// pixels addons draw in.
void on_frame(unsigned width, unsigned height);

// chat.cpp's hook of the game's window draw, after each window is drawn: the list, drawn this frame, goes on top
// (raise::add).
void on_draw(const void* ctrl);

} // namespace vanacore::phraselist
