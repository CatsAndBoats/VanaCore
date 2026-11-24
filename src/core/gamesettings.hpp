#pragma once

// [client] gamesettings.extended: the game's own Config > Gameplay window ("menu    conf2win", Game Settings) with rows of
// VanaCore's after its Music Volume: World Sounds Volume and Interface Sounds Volume (soundmix.hpp), sliders as the game's
// are, drawn and run by the game's menu code (its window, cursor, sounds, help line and slider fill). The rows under them
// (Gamma Adjustment, Inventory) move down and the window grows. Off (the default), the window is the game's.
//
// None of the game's code is changed, as configmenu.cpp does it for the Config list (XI on Anything's way): the window's
// panel is a sprite of VanaCore's own sheet, "menu    vcgamset", made of the game's own panel's parts (read from the menu
// DAT the game loaded, English only: the new rows' captions are the English menu font's letters, menufont.hpp) with the
// rows under Music Volume moved down and the new rows' captions, tracks, Min and Max in the gap; the window's layout is
// rebuilt from the game's with the new sliders after its eleven items and registered in place of the game's; and the
// window's handler gets a copy of its table of functions whose drawing (the new sliders' fill and help lines) and input
// (select on a new slider moves it with Left and Right until select or cancel, as the game's own) come here first. The
// game's eleven items keep its own code. More rows are more entries in kRows.
namespace vanacore::gamesettings {

// `ini` (the session's vanacore.ini): [client] gamesettings.extended (0 the default, 1 the extended window),
// sound.world and sound.interface (0 to 100, 100 the default: as the game plays them). Read now and by reload whenever
// vanacore.ini changes; on_frame applies them.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Each frame, from Present, on the game's thread: the handler's table set at once; twice a second, the window put in once
// the game's menus are loaded (and again after it loads them again), or taken out when the setting is off, either waiting
// while the window is open.
void on_frame();

} // namespace vanacore::gamesettings
