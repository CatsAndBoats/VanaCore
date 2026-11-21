#pragma once

#include <string>
#include <vector>

// /vana debug: about 30 seconds of the game's frames recorded one by one, to see a pattern: each frame's time and
// where it went (the game's own work, its Direct3D calls and the slowest of them, its sleeps, VanaCore's work and frame
// rate limit, Present; and from Direct3D 12's profile, turned on for it: drawing the calls, waiting for the GPU, the
// swap chain, waits on the DLSS helper, GPU time, draws, uploads, pipelines made), with what the log said meanwhile.
// Then logs\frames-<date>-<time>.csv (a row a frame), and in logs\debug-<date>-<time>.log (and briefly in the player's
// own chat log, echoed: only they see it): the frame times' spread, what holds the frame rate (the game's own cap,
// fps.max, the GPU, or the CPU's work), the worst hitches with what grew in each and what the log said around them (when
// logging is on), and whether they come at a steady interval. Both files are kept whether or not logging is on.
//
//   /vana debug [seconds, 30]      /vana debug stop   (the recording so far)
namespace vanacore::debugrec {

// From VanaCoreInstall: VanaCore's folder (the CSV goes in its logs folder).
void install(const wchar_t* install_dir);

// "debug ..." (its words after /vana): true, it's handled.
bool command(const std::vector<std::string>& parts);

} // namespace vanacore::debugrec
