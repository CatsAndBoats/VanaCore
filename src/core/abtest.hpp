#pragma once

#include <string>
#include <vector>

// /abtest: two values of a setting compared in one session, standing in one place, as rdbell's Wine/Metal tuning for
// xi-on-anything measured (separate launches vary by about 5%, too much to trust): the setting is flipped between its
// two values every few seconds in A B B A order (so a drift lands on both alike), the game's frames are timed in each
// once it has settled (a second or two after the switch, and while DLSS or frame generation are starting, nothing),
// then the setting goes back to what it was and the result is echoed to the player's own chat log (chat::echo: only the
// player sees it) and written to logs\abtest-<date>-<time>.log, which is kept whether or not logging is on: what was
// compared, the settings it was compared under, the result, and each period's frames.
//
//   /abtest <setting>=<a>,<b> [seconds per period, 8] [rounds of A B B A, 2]
//   /abtest dlss=off,on          /abtest dlss.framegen=off,2x          /abtest dlss.quality=35,100
//   /abtest graphics.effects/sun=0,1   (a setting outside [graphics]: section/key)
//   /abtest stop                 (back to how it was, no result)
namespace vanacore::abtest {

// From VanaCoreInstall: vanacore.ini, where the settings are flipped, and VanaCore's folder (the report goes in its
// logs folder).
void install(const wchar_t* ini, const wchar_t* install_dir);

// "/abtest ..." (its words): true, it's handled.
bool command(const std::vector<std::string>& parts);

// Each of the game's frames (the Present hook, on the render thread).
void on_frame();

} // namespace vanacore::abtest
