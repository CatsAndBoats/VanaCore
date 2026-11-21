#pragma once

// The sun's shadows whatever the camera sees ([graphics.effects] sun_seek): every piece of the zone within the shadow maps' reach of
// the player (a tall one from farther, as far as its shadow reaches) drawn by the game's own renderer for those maps alone (the
// renderer's GfxDraw.sought), measured from the player, a few at a time round after round, the shadow cache casting them between.

namespace vanacore::casters {

// On the game's thread, each frame: where the player stands (the world as the game draws it: x, height, y), known false while
// there's no player. Puts the pass in the first time the renderer wants it (vanacore_gfx_seek_reach); it stays, doing nothing
// while it isn't wanted.
void update(float x, float y, float z, bool known);

} // namespace vanacore::casters
