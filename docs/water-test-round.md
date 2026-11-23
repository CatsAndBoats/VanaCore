# The water's test round

VanaCore's water (`[graphics] water`, the water pass, the mirror, the sea, the rain on it) changes how a few places in the game
look in ways no build or lint catches. These are the places that have broken before. Go round them after any change to the
water or to what it reads (the scene's copies, the sun's casters, the zone's floors), on Direct3D 12 and Direct3D 11 both
(`[graphics] renderer`), and say in the pull request which you saw and how they looked.

Each place lists where to stand, a camera pose, what passing looks like, and what the water log says there. No GM commands are
needed: walk or teleport there as any player does.

## Before you start

- **The water log.** In `vanacore.ini`, `[graphics] water.log=1` (with `[logging] enabled=1`, the default). Its lines are in
  `logs\vanacore.log` and start `Graphics: water:`. Turn it on before you zone in: much of what it says is said once, at the
  zone's first frames.
- **Noon.** `/freecam time noon` holds the Vana'diel day at noon on your screen only (nothing goes to the server);
  `/freecam time off` puts it back. A zone change puts it back too, so set it again after one.
- **The same view as the last person.** Open the free camera with `/freecam`, then put it at the pose given below:

  ```
  /vana lua vana vana.camera.set_pose(x, y, z, yaw, tilt)
  ```

  `y` is the world's height and points down (a smaller number is higher up); `yaw` and `tilt` are radians.
  `/vana lua vana vana.camera.pose()` says where the camera is now, to give a pose of your own in a report. Go to the place
  first; the poses are where the last person looked from.
- **What VanaCore took for water.** /vana debug > Debug views > 11 colours it over the picture: magenta the game's water models
  made VanaCore's water, orange the ones left as the game draws them, green tiles found by their shape, cyan the ground's water
  (the zone's own sheets and floods). Back to 0 when done.
- **Weather.** `/freecam weather rain` shows rain on your screen only, for the rings on the water (`[graphics] rain` and
  `rain.water` on); `/freecam weather off` puts the zone's back.

## Port Jeuno: the airship berths

The sea here is one water model the game draws twice a frame, at 15.0 and again 5 yalms under it, at 19.9 (an under-sheet),
with a wider sheet at 15.0 over it. When VanaCore took the under-sheet for the sea's level, the berths were pale patches like
paving with blue-green splotches (the void under the docks read as a shallow bed) beside a dark slab with straight edges.

- **Where:** the Kazham airship's dock, at about x -24, z 112. An airship docks now and then and hides the berth's water; the
  last three poses look over the open water by the Bastok airship's berth and still show it.
- **Poses:** `-24, 8, 110, 0, -0.6`; `-14, 4, 104, 0, -0.7`; `-34, 6, 100, 0.6, -0.4`; `-50, 8, -106, 0, -0.55`;
  `-50, 8, -106, 3.14, -0.55`; `-50, 2, -100, 0.4, -0.35`.
- **Passes:** one even sea everywhere, in the berths and out past the walls, with the awnings and airships mirrored in it and
  foam along the berths' walls. No pale splotched patches, no dark slab with straight edges between two kinds of water.
- **Log:** `the sea at height 15.0 (the tiles' or chunks' level at the zone's rim) drawn on to the horizon`, and
  `the sea's surface at 15.04, over its under-sheet at 19.94 (one sheet drawn twice)`, with the fix in pull request #7. A sea
  at 19.9 is the old fault.

## The Boyahda Tree: the pond and the stream

Its water is the game's own, and VanaCore leaves it so. It's here because changes to the water have broken this zone more than
once: the check is that nothing changes.

- **Where:** the pond with the Robber Crabs, at about x 165, z -56; the stream at about x 59, z 97.
- **Poses:** the pond `162, 5, -56, 3.14, -0.3`; the stream `56, 5, 97, 0, -0.3`.
- **Passes:** the same picture with `[graphics] water` on and off: the game's dark pond and stream, no sheet of VanaCore's
  water, nothing missing or flickering. Debug view 11 colours neither.

## Bastok Markets: the fountain

The basin is a draw the game makes after the scene, with its vertices in the game's own memory; it takes the water pass's look
over copies of the scene, and reads them by the screen's pixels.

- **Where:** the fountain in the square, at about x -259, z -69.
- **Pose:** `-258.5, -14, -69, -1.75, -0.3`.
- **Passes:** clear water over the basin's mosaic, a thin foam rim along the stone, froth where the jets land, the sky and the
  pillars mirrored. Turn the camera slowly both ways: no level bands slide across the water near the screen's right edge. Check
  that once with `[graphics] background` set to a size other than the window's (`3072x3072` on a wider screen), where the
  scene's copies and the screen differ in size.
- **Log:** `a late water draw drawn with the pass's look over the pass's copies`, the two frame numbers in it the same. It's
  said for the session's first eight such draws only, so log in close by to see it.

## Bastok Markets: the harbour

The zone's sea, 8 yalms down in a pit with the city above it.

- **Where:** on the pier at about x -330, z -22.
- **Pose:** `-330, -6, -22, 1.57, -0.3`.
- **Passes:** the walls, towers and piers mirrored in the water, broken up by the waves; soft foam at the piers' feet and a
  line along the far wall; no glowing ring round the piers' feet.
- **Log:** `the sea at height 8.0 (the tiles' or chunks' level at the zone's rim) drawn on to the horizon`, and no
  `under-sheet` line.

## Jugner Forest: Lake Mechieume

A lake wider than one flood reaches: its only sheet of water is at the river's mouth, and VanaCore floods the lake from there,
carrying on where each flood stops. The flood starts only once the river's mouth has been in view since you zoned in, so turn
toward the mouth (about x 20, z 300) once first.

- **Where:** on the lake's bank at about x 69, z 375.
- **Poses:** at the bank `74, 2.5, 372, 3.14, -0.25`; from above `60, -110, 380, 0, -1.5`.
- **Passes:** VanaCore's water from shore to shore, the trees and banks mirrored in it. No saw-toothed line across the open lake
  with the game's flat water past it, no square holes over the deepest parts (from above, debug view 11 colours the whole lake
  and the river). Turn the camera all the way round at the bank: the mirror stays the whole time. In the rain, rings spread on
  the water.
- **Log:** `the ... flooded N cells of its pond`, more than once as the flood carries on; and while turning,
  `the mirror about the level 6.62 (the camera at ...): drawn` at every heading, the same level each time.

## Lower Jeuno: the harbour

The game lays this harbour in two or three sheets a little apart, which VanaCore takes as one sea, and draws its bed's meshes by
the camera's distance.

- **Where:** above the harbour at about x 43, z 85.
- **Pose:** `42.8, 1, 84.5, 0, -0.3`.
- **Passes:** one sea with no steps or seams where the sheets meet, and no darker rectangle in it where the game's tiles end;
  the water the same colour as you zoom the camera in and out (not bright teal close and dark far); no thin stray triangles of
  water on the terrace by the statue.
- **Log:** `the sea at height 17.6 (the tiles' or chunks' level at the zone's rim) drawn on to the horizon`, and no
  `under-sheet` line.
