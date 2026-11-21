#pragma once

// Mass Grass's floors ([graphics] grass.walkable): the zone's collision, read from its model DAT on a thread
// (zonefloor::read_floors) and handed to the grass, which then grows tufts only on ground the player could stand on.
namespace vanacore::grass_floors {

// Each frame, on the game's thread: the zone the player is in (0 none) and whether the grass wants floors (on, with
// grass.walkable). A new zone's are read on a thread and handed over once read; not wanted, they're let go.
void update(int zone, bool wanted);

// ... and the zone's whole collision, its walls too (zonefloor::read_floors' walls), while /vana debug's collision view
// shows it ([graphics] collision.show): read on a thread of its own and handed over (vanacore_gfx_set_collision) the same way.
void update_collision(int zone, bool wanted);

// The graphics device lost and made again (graphics::close_if_lost): the floors handed over went with it (the collision's
// too). The zone's are read and handed over again at the next update, as for a zone not seen yet.
void forget();

} // namespace vanacore::grass_floors
