#pragma once

#include <cstdint>

namespace vanacore::vanaclock {

// The minute of the Vana'diel day (0.01 to 1440) the game draws while its game timer is off: an hour a cutscene has set, one the
// free camera's Time holds (addons\vanacore\freecam\daytime.lua), the title screen's. False while the timer is on (the game's
// clock is then the server's time worked out, as graphics.cpp's track_daylight works it out) or before FFXiMain.dll is there;
// `minute` is left as it was.
bool held_minute(double& minute);

// The minute of the Vana'diel day (0 to 1440) the server's clock is at now, to the millisecond: the PC's clock set on the server's
// (`server`, Unix seconds from its last packet's header; 0 not known, the PC's alone).
double server_minute(std::uint32_t server);

} // namespace vanacore::vanaclock
