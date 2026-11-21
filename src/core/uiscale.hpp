#pragma once

#include <cstdint>

// [ffxi] resolution.ui while the game runs: the menus' size (the interface's resolution, 0037 x 0038) changed at once,
// as the game would have made it at its start. The game draws its interface into a render target of that size (none
// when it's the back buffer's) and lays its windows out in an area of it; both are made again at the new size between
// frames, and the open windows kept where their layouts anchor them. From XI on Anything's Config > Display
// (rubymatrix/xi-on-anything, host/modern.c menu_apply), on the same FFXiMain build.
namespace vanacore::uiscale {

// `ini` (the session's vanacore.ini): [ffxi] resolution.ui as the game started with it (registry.cpp gave it to the game).
void install(const wchar_t* ini);

// vanacore.ini changed: a resolution.ui other than the last one is applied after the next Present.
void reload(const wchar_t* ini);

// The game's window on another monitor: resolution.ui applied again after the next Present (auto and auto-<percent> are
// the monitor the window is on).
void refresh();

// On the game's thread, right after the frame went to the screen: a changed resolution.ui applied. With resolution.ui=window
// or window-<percent> (registry::window_share), the menus follow the game's window: made again when the size it comes to
// changes (the window made at another size, [ui] aspect changed) and has held for a second.
void after_present();

// The menus' size resolution.ui=window-<share> comes to now: that share of the game's resolution, fitted to [ui] aspect's
// shape while that squeezes the interface (so the aspect takes effect in any window). False before the game has a
// resolution.
bool fit_size(int share, long& width, long& height);

// The game's menu target as its display holds it (the render-target texture it draws its interface into, the COM pointer),
// 0 for none (the interface drawn straight onto the back buffer). `known` false while the display can't be read (FFXiMain.dll
// not there, its code not found, or the display not made yet).
std::uint32_t menu_target(bool& known);

} // namespace vanacore::uiscale
