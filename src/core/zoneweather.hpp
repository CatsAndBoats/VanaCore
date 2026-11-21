#pragma once

#include <cstdint>

namespace vanacore::zoneweather {

// The weathers zone `zone`'s model DAT has a sky and light for (the weather folders of its environment tree, "weat"): a bit
// each, bit n for 0x057's weather n (0 clear to 19 darkness). The DAT is the one the game loads, through pivot's overlays.
// False (`mask` 0) when it can't be read or has none. Read once a zone (its section headers only), then kept. Any thread.
bool weathers(int zone, std::uint32_t& mask);

} // namespace vanacore::zoneweather
