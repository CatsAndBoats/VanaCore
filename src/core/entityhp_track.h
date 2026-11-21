#pragma once

#include <algorithm>
#include <cstdint>

// entityhp.cpp's arithmetic, apart from the game so it can be tried on its own (no includes of VanaCore's).
//
// The game keeps a maximum HP (or MP) for the local player only. For a party or alliance member (a trust too) the server
// sends the current value and its percent (0x0DD), and for any other entity only the percent. A maximum for the rest is
// worked out from those two: the percent says the current value is `percent` of it, give or take the percent's own rounding
// (the server rounds it down, or up, to a whole number; which isn't known here, so the bounds hold for both and for rounding
// to the nearest). Each sample leaves a band the maximum is in; the bands of successive samples, taken while the value moves,
// cross and narrow it. A sample at 100% says the value is the maximum (to within a percent): that is the "anchor" the
// maximum is then reported as, until a sample says otherwise. A sample the band left by the last ones doesn't cross is a
// maximum that changed (a curse, a weakness ending, a food, a buff): the track starts again from that sample.
namespace vanacore::entityhp {

constexpr double kNoLimit = 4294967295.0;

struct Track {
    double lo = 0.0, hi = 0.0;  // the maximum is within these (both 0: nothing seen yet)
    std::uint32_t anchor = 0;   // the value last seen at 100%, while the samples since agree with it (0: none)
};

// One sample: the current `value` and its `percent` (0-100). A value of 0 (dead, or not sent yet) and a percent of 0 (below
// 1%, which the percent can't tell from nothing) say nothing about the maximum.
inline void feed(Track& track, std::uint32_t value, unsigned percent)
{
    if (value == 0 || percent == 0)
        return;
    const double v = static_cast<double>(value);
    if (percent >= 100) { // full: the value is the maximum, to a percent
        track = Track{v, std::min(v * 100.0 / 99.0, kNoLimit), value};
        return;
    }
    const double low = std::max(v, v * 100.0 / (percent + 1)); // the maximum is never below the value
    const double high = percent > 1 ? std::min(v * 100.0 / (percent - 1), kNoLimit) : kNoLimit;
    if (track.hi <= 0.0 || low > track.hi || high < track.lo) { // the first sample, or a maximum that has changed
        track = Track{low, high, 0};
        return;
    }
    track.lo = std::max(track.lo, low);
    track.hi = std::min(track.hi, high);
    if (track.anchor != 0 && (track.anchor < track.lo || track.anchor > track.hi))
        track.anchor = 0;
}

// The maximum the samples point to for the last one (`value`, `percent`): the anchor while there is one, else the value
// the percent gives (the percent's own edge of the band, which both roundings share) put inside the bounds. 0 for none.
inline std::uint32_t estimate(const Track& track, std::uint32_t value, unsigned percent)
{
    if (track.hi <= 0.0)
        return 0;
    if (track.anchor != 0)
        return track.anchor;
    const double guess = percent >= 100 ? static_cast<double>(value) : percent > 0 ? value * 100.0 / percent : track.lo;
    return static_cast<std::uint32_t>(std::clamp(guess, track.lo, track.hi) + 0.5);
}

// The bounds as whole numbers (the high one kNoLimit's worth when the samples set none)
inline std::uint32_t bound_low(const Track& track)
{
    return static_cast<std::uint32_t>(std::min(track.lo + 0.999, kNoLimit));
}
inline std::uint32_t bound_high(const Track& track)
{
    return static_cast<std::uint32_t>(std::min(track.hi, kNoLimit));
}

} // namespace vanacore::entityhp
