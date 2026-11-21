#pragma once

// [graphics] bump: the game's Bump Mapping option, changed while the game runs. The game reads its option
// (registry value 0017, [ffxi] bump_mapping) once, when its renderer starts: if it's on, the renderer
// assembles the bump shaders (a ps.1.1 DOT3 light over a normal map) and each zone load brings the zone's
// bump DAT (file id zone + 0x9B97) along. This builds the shaders when they're first wanted and keeps the
// flags the zone loads and draws look at. "Off" comes at once; "on" shows from the next zone entered, as the bump DAT
// is read only as a zone loads. (Calling the game's zone load for the zone the character is in left that zone
// unbuilt and the world undrawn: the build that follows it belongs to the zone-in's later steps. Don't.)
namespace vanacore::bumpmap {

enum class Mode {
    Game, // as the game's option (the registry's 0017) says
    On,
    Off,
};

// Each frame, on the game's thread.
void apply(Mode mode);

} // namespace vanacore::bumpmap
