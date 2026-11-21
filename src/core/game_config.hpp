#pragma once

// The game's own configuration options (its in-game Config menu: FsConfig, as the PS2 client names
// it): a table in FFXiMain.dll of 207 options, each a number. The ones of type 4 are saved per
// character to USER\<id>\cnf.dat at logout (a 5, then their values in option order); the others go
// with PlayOnline's settings.
namespace vanacore::game_config {

// Once per frame on the game's thread: a few times a second, the options are compared with the last
// look and every change is logged ("Game config: option 58 = 0 (was 1; cnf.dat value 46 at +0xBC)"),
// so changing a setting in game names its option. Many at once (a character's config loaded at
// login) are logged as one line.
//
// The game's own shadows (option 58: 0 Normal, 1 Off, 2 High) are turned off, through the game's own
// setter as its Config menu does, while VanaCore's sun shadows are on (graphics::sun_shadows). They
// stay off when VanaCore's are turned off: the Config menu puts them back.
void on_frame();

// `ini` (vanacore.ini): [graphics] gameshadows.own, which older VanaCores kept the game's setting in to
// put back, taken out.
void install(const wchar_t* ini);

} // namespace vanacore::game_config
