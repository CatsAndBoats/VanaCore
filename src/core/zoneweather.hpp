#pragma once

#include <cstdint>

namespace vanacore::zoneweather {

// The weathers zone `zone`'s model DAT has a sky and light for (the weather folders of its environment tree, "weat"): a bit
// each, bit n for 0x057's weather n (0 clear to 19 darkness). The DAT is the one the game loads, through pivot's overlays.
// False (`mask` 0) when it can't be read or has none. Read once a zone (its section headers only), then kept. Any thread.
bool weathers(int zone, std::uint32_t& mask);

// The weathers whose folder in zone `zone`'s model DAT carries falling rain of its own (a texture named rain or rai1 in the
// folder itself, the drops the game draws round the camera), as weathers' bits. Most indoor zones' rain and squall folders
// are a sky and light alone - Alzadaal Undersea Ruins, the Temple of Uggalepih, Crawlers' Nest, the Boyahda Tree...: the game
// draws no rain there. False (`mask` 0) when it can't be read or has none.
bool rains(int zone, std::uint32_t& mask);

} // namespace vanacore::zoneweather
