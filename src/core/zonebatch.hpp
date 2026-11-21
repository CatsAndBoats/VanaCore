#pragma once

// The zone's draws of one mesh sent together ([graphics.effects] batch: gfx/gfx_d3d12_batch.h): FFXiMain's zone draw is hooked so
// the renderer knows when the game begins drawing its zone and when it's done (vanacore_gfx_batch).

namespace vanacore::zonebatch {

// On the game's thread, each frame: hooks the game's zone draw the first time the renderer wants it (vanacore_gfx_batch_wanted);
// it stays, doing nothing while batch is off. `moving`: the whole zone moves (a ferry): left alone, its pieces' motion is the
// upscaler's to see draw by draw.
void update(bool moving);

} // namespace vanacore::zonebatch
