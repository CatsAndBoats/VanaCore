#pragma once

#include <cstddef>
#include <cstdint>

struct lua_State;

// [client] equip.stats: the Equipment window made taller, its new rows the character's stats beyond the game's Attack,
// Defense and elements. First the server's own totals (what /checkparam answers: accuracy and attack for the main and
// off hand, ranged accuracy and attack, evasion), asked for quietly while the window is open and again when the gear or
// the stats change (equip.stats.checkparam), its chat lines kept out of the log; then what the gear adds (haste, store
// TP, double attack, magic attack...: each equipped item's modifiers and its augments' from CatsEyeXI's server data,
// addons\vanacore\libs\gamedata\equip_mods.json, tools/gen_gamedata.py's; for an item whose description has changed
// since that data was made, what its description in the DATs says over it: equipdesc.hpp). Set bonuses aren't in it.
// The windows under the Equipment window (an item's description) are moved down by as much.
//
// equip.stats.move=1: the stats in a box of their own, the game's window frame (FFXiMain's own drawing of every window's
// background and border) drawn with the Equipment window, its left edge at equip.stats.x and its top at equip.stats.y
// (percents of the interface), as wide as the window or wider for the longest row. The Equipment window keeps the game's
// height then (its Attack and Defense row still taken out: the box has them). The HUD Manager's box 'equip.stats' moves it.
// The stats under the Check window (equip.stats.check) do the same with equip.stats.check.move, .x and .y ('equip.stats.check').
namespace vanacore::equipstats {

// [client] equip.stats* read again from `ini` (the settings file) whenever it changes.
void reload(const wchar_t* ini);

// In ImGui's frame (vana.layout.outline('equip.stats'), the HUD Manager): a box where the stats are (under the Equipment
// window as last drawn there, or their own box's place at its last size), Shift + drag moving them into a box of their own
// (equip.stats.move on, .x and .y as it goes, not written); the frame it's let go, moved_x and moved_y are where it was left,
// for the caller to save. False while equip.stats is off or the place can't be worked out.
bool outline(double* moved_x = nullptr, double* moved_y = nullptr);

// The same for the stats under the Check window (vana.layout.outline('equip.stats.check')): Shift + drag moves them into a
// box of their own (equip.stats.check.move, .x, .y). False while equip.stats or equip.stats.check is off.
bool check_outline(double* moved_x = nullptr, double* moved_y = nullptr);

// Once a frame on the game's thread: the first time, the settings read from `ini`; then hooks the game's drawing of the
// Equipment window the first time it's on (once FFXiMain.dll is unpacked).
void on_frame(const wchar_t* ini);

// For hudwin.cpp's hook of the game's SetLocate: how far the window has been moved down (one of the windows under the
// Equipment window, still where VanaCore put it), else 0.
int lowered(const void* window);

// For hudwin.cpp's placing of an item's details, `left` to `right` across with its top at `y`: below the Equipment window
// (as tall as equip.stats made it) when it would start over it, else y. The game puts the details under the item list
// beside it, which is shorter than the window with its stats, so they lay under the stats (asked for 2026-11-06); and
// lower_under's resize didn't hold: the details' own place (its position, which its calc lays the rect from each draw and
// itemlist.cpp's placing keeps to) stayed at the game's
int below(int left, int right, int y);

// For packets.cpp, each packet the server sends (its 4-byte header first): true for one of the /checkparam lines this
// asked for, which the game and the addons are then not given. wants_packets() says whether any is awaited.
bool swallow(const std::uint8_t* packet, std::size_t length);
bool wants_packets();

// For packets.cpp, each 0x0C9 the server sends (a /check's answer, header first): the checked gear's extdata, for the
// augments in the stats under the Check window ([client] equip.stats.check, equipstats_check.h). Any thread.
void inspected(const std::uint8_t* packet, std::size_t length);

// /vana statsdump (vana.gameui.stats_dump): the stats worked out again into the log, each equipped item's modifiers,
// latent effects, augments and sets and each stat's sum, in the server's units and as shown (equipstats_dump.h). The
// items dumped; -1 with equip_mods.json missing, -2 while it's still being read. Game thread.
int dump();

// vana.gamedata.item_stats(id[, extra]) (vana.game.native_item_stats, which lua/vana.lua hands on): one item's stats as
// the Equipment window counts them, its latent effects, its sets, its augments given its extdata, each stat's label and
// unit; nil and why while equip_mods.json isn't read (equipstats_lua.h). Game thread.
int lua_item_stats(lua_State* L);

} // namespace vanacore::equipstats
