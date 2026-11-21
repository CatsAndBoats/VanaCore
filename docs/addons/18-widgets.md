# 18. Widgets: boxes in the game's 3D space

A `vana.ui` box (text, rectangle or picture) normally sits at a fixed place on the screen, drawn over everything. A **widget**
is the same box placed in the game's 3D space instead: at a character's nameplate, or at a point in the world. VanaCore draws
it in the pass that draws the names over heads, at a depth the world's own depth tests, so **walls hide it as they hide a
name**, and the effects, DLSS and frame generation treat it as they treat a name. That is what an addon cannot do alone: the
world's depth is in VanaCore's own renderer, out of reach of an addon's draws.

There are two levels. The simple one puts a box where a name would be (`vana.world.attach`). The advanced one puts it at any
point of the world (`vana.world.place`), over or under the interface, behind walls or not. Both go through `vana.world`.

`vana.world` also has what an addon marking things in the world needs beside its boxes: [rings of light and
waymarks](#rings-of-light-on-the-ground-vanaworldmarks) drawn on the ground by the scene effects (`vana.world.marks`), [the
ground under a pixel](#where-is-a-point-vanaworldto_screen-vanaworldvisible-and-vanaworldpick) for putting one down with the
mouse (`vana.world.pick`), and [where a name is](#where-a-name-is-vanaworldname_at) on the screen (`vana.world.name_at`).

```lua
local star = vana.ui.text({ text = '*', size = 18, color = '#FFD040', background = '#00000000' })
vana.world.attach(star, vana.game.target().index, { side = 'above' })   -- over the target's name
vana.world.detach(star)                                                  -- back to the screen at star.x, star.y
```

A placed box is not drawn at its `x` and `y` any more. `box.visible`, `box.opacity`, `box:set{...}` and `box:destroy()` work as
they always do (destroying it ends its placement). One box has one placement: placing it again changes it.

**Needs** the renderer `d3d11` or `d3d12` for the world's camera (`vana.world.place`, `to_screen`, `visible`, `pick`,
`name_at`), and its scene effects for `marks`. `attach` works with any renderer that VanaCore's name hooks find.
`name_at` and `marks` need **VanaCore 0.8.218 or later**; `pick` and the marks' `shape`, `height` and `glyph` 0.8.219 or
later (`vana.world.pick` is `nil` before, and an older VanaCore draws a waymark as a plain ring). Hide the game's own names with
[`vana.gameui.hide('nameplates')`](07-game-interface.md#nameplates) when the widgets replace them.

The API Explorer (`/addon load apiexplorer`, `/api`) has a **Widgets (vana.world)** group: run each entry to see a box at a
nameplate, in front of you, or over the interface, a beam of light standing on the floor (an RPG marker made of boxes,
its column laid in the world so it stands up from every side), waymarks on the ground, a ring following the mouse over the ground, or where the target's name is, and edit the code to try
the options.

## At a nameplate: `vana.world.attach`

```lua
vana.world.attach(box, index [, options])
```

`index` is the character's target index (`0` to `4095`, as `vana.game.entity(index)` takes). The box is drawn after that
character's name, positioned from the name's edges. It is drawn **only for characters the game gives a name** (its own name
display setting, and the distance it stops drawing names at). While the name is hidden (the player's `[graphics]
nameplates.hide*` settings, or `vana.gameui.hide('nameplates')`) the name is still laid out for its place and the widget is
drawn there; only the letters are left out.

| Option | Default | |
|--------|---------|---|
| `side` | `'above'` | Which edge of the name the box sits on: `'above'`, `'below'`, `'left'`, `'right'`, or `'over'` (centred on it) |
| `x`, `y` | 0 | Pixels from there, right and down (the scene's pixels: those of `vana.ui`) |
| `scale` | 1 | The box's size times this |
| `distance` | `false` | `true`: the box grows and shrinks with the name, as the name does with its distance |
| `walls` | `true` | `false`: not hidden by walls (drawn over the world, under the interface) |

## At a point in the world: `vana.world.place`

```lua
vana.world.place(box, x, y, z [, options])
```

`x`, `y`, `z` are a position as `vana.game.entity` has it: `x` and `y` across the ground, `z` the height, the game's way
round: **up is a smaller `z`** (2 yalms over the point is `z - 2`; the `up` option does this for you).

| Option | Default | |
|--------|---------|---|
| `up` | 0 | Yalms above the point |
| `pivot` | `'bottom'` | Which part of the box sits on the point: `'bottom'` (a marker standing on it), `'center'`, `'top'`, `'left'`, `'right'` |
| `x`, `y` | 0 | Pixels from there, right and down |
| `scale` | 1 | The box's size times this |
| `distance` | 0 | Above 0: the box is its own size at this many yalms from the camera, bigger nearer and smaller farther (a quarter to four times) |
| `range` | 0 | Above 0: not drawn farther than this many yalms from the camera |
| `over_ui` | `false` | `true`: drawn over the game's windows and your ImGui windows, not under them |
| `walls` | `true` | Whether walls hide it |
| `facing` | `'camera'` | `'camera'`: a picture facing the screen. `'upright'`, `'ground'` or `'wall'`: laid in the world ([below](#laid-in-the-world-facing)) |
| `heading` | 0 | Laid on the `'ground'` or a `'wall'`: which way it's turned, radians as `vana.game.entity`'s `heading` |
| `width`, `height` | 0 | Laid in the world: its size in yalms. One of them: the other follows the picture's shape; neither: 1 yalm across |
| `toward` | 0 | Yalms toward the camera (negative: away). Facing the camera, on the ground or a wall: it only counts as that much nearer for the walls (the same pixels). `'upright'`: it stands out from its line, level, toward the camera: a column's front, or with a negative value its back |

**Under the interface (`over_ui = false`)** the box is drawn into the world in the names' pass, just before the names (a name
goes over a box). `walls = true` hides it per pixel by the world's depth (the part of it behind a pillar is cut off, as a
name's would be); `walls = false` draws it over the world, always.

**Over the interface (`over_ui = true`)** the box is drawn after the game's windows, the ImGui windows and every `vana.ui` box
(whatever its `layer`). It has no depth
of its own there, so `walls = true` hides the whole box when the zone's collision (walls, floors, doors) is between the camera
and the point (`vana.world.visible`), and shows it where that isn't known. It follows the interface being hidden (F11).

### Laid in the world: `facing`

A box facing the camera is a picture over the point: it looks the same from every side, so a tall one (a column of light)
stands up the screen however the camera looks at it, and from above it seems to lie across the floor. With `facing`, the
box is laid in the world instead, a flat card with a size in yalms, its four corners points of the world: it has the world's
perspective, and walls hide the part of it behind them.

```lua
local beam = vana.ui.rect({ width = 32, height = 192, radius = 16, color = '#40C8FF60' })
vana.world.place(beam, e.x, e.y, e.z, { facing = 'upright', width = 1, height = 6 })        -- a column 6 yalms tall
local pool = vana.ui.rect({ width = 32, height = 32, radius = 16, color = '#40C8FF40' })
vana.world.place(pool, e.x, e.y, e.z, { facing = 'ground', width = 2.5, pivot = 'center', up = 0.05 })   -- a disc on the floor
local sign = vana.ui.text({ text = 'This way' })
vana.world.place(sign, e.x, e.y, e.z, { facing = 'wall', heading = e.heading, height = 0.5, up = 2 })   -- facing your way
```

- `'upright'`: stands on the point and turns round its upright line to face the camera, as a beam or a post would be drawn.
  From the side it stands up; from above it is seen end on, short (a glow on the ground under it reads as its foot). A
  column round someone standing in it is two cards: `toward = -r` behind them and `toward = r` in front (r its radius), so
  they are inside it, not cut by one card through their middle.
- `'ground'`: lies flat, its top toward `heading` (0: east).
- `'wall'`: stands on the point facing `heading`, like a sign; from behind it is seen mirrored, as a pane of glass is.

`pivot` is the part of the card on the point (`'bottom'`: standing on it; `'center'` for one centred on it), `up` lifts the
point, `scale` multiplies the size, and `range` and `walls` work as for any box. The size is in yalms, so `distance`, `x`
and `y` (pixels) don't apply, and a laid box is always drawn in the world, under the interface (`over_ui` doesn't apply).
The picture's pixels are only its shape and how sharp it is: a 32 by 192 rect laid 1 by 6 yalms is a column. For the
walls, a laid box counts as a little nearer the camera than it is (0.08 yalms and 1.5% of its distance; it covers the same
pixels), so the ground or wall it lies on doesn't cut through it or flicker over it as the camera moves (uneven ground, frame
generation's made frames); what stands on it still hides it. A card whose
corner is behind the camera isn't drawn. `vana.world.update` changes `facing`, `heading`, `width` and `height` like the other
options, so a card can turn or grow every frame.

## Where is a point: `vana.world.to_screen`, `vana.world.visible` and `vana.world.pick`

```lua
local sx, sy, distance = vana.world.to_screen(x, y, z)   -- the interface's pixels, or nil
local clear = vana.world.visible(x, y, z)                -- true, false, or nil
local gx, gy, gz = vana.world.pick(mouse_x, mouse_y)     -- the ground under a pixel, or nil
```

`to_screen` gives the point in the pixels `vana.ui` uses and how far in front of the camera it is; `nil` behind the camera, past
its planes, far off the screen, or before the world has a camera. `visible` asks the game's collision whether a line from the
camera to the point meets the zone; `nil` where that isn't known (no camera yet, the zone's collision isn't loaded). It is the
zone's collision, not what is drawn: a tree or a prop without collision doesn't block it. `pick` goes the other way: the point
of the zone under a pixel, where the camera's line through it first meets the zone's collision (the ground, a floor, a wall), as
`vana.game.entity` has positions (`z` the height), for putting something down where the pointer is; `nil` when nothing is met
within 150 yalms, or the collision isn't there. The same collision: a pick through a tree without it lands on the ground under
it, and a monster or a player isn't met (they aren't in it).

## Changing a widget every frame: `vana.world.update`

```lua
vana.world.update(box, { y = -20, walls = false })    -- only what is given changes
local now = vana.world.placement(box)                 -- how it is placed: the options, and `index` or `at`; nil if not
```

`placement` gives `x`, `y`, `scale`, `walls`, then for a box at a name `side`, `distance` (a boolean) and `index`, and for
a box at a point `up`, `distance`, `range`, `over_ui`, `facing`, `heading`, `width`, `height`, `toward` and `at` (`{ x, y, z }`).
`pivot` isn't read back. (A VanaCore from before `facing` gives no `facing`: an addon can tell from that.)

Placing a box (`attach`, `place`) once and then changing it is the cheap way to keep a plate in step with the game.

- **What shows** (a text box's text, a rectangle's `width`, `height` and `color`, a picture's `file`) is changed on the box with
  `box:set{...}` or `box.text = ...`. The picture is remade only when that changed, so a bar whose width follows HP costs a
  little only on the frames HP moves.
- **How it is placed** is changed with `update`: the options of the box's own kind, so a box moves without being placed
  again. A box at a name takes `x`, `y`, `scale`, `walls`, `side`, `distance` and `index` (another character's name); a
  box at a point `x`, `y`, `scale`, `walls`, `up`, `pivot`, `distance`, `range`, `over_ui`, `facing`, `heading`, `width`,
  `height`, `toward` and `at` (`{ x = ..., y = ..., z = ... }`, another point). The other kind's options are ignored (`update` doesn't turn one kind into the other), and
  updating a box that isn't placed is an error. `walls` can be changed at any time: `false` for the target's plate
  so it stays visible through walls, `true` for the rest.
- **A stack of boxes** at one name is one `attach` per box, each with its own `y` from the name's edge; boxes are drawn in the
  order they were placed, so the one placed last is on top.
- **Every placed box**, at a name or at a point, is drawn in the order it was **first** placed: placing it again keeps
  its turn, and its `layer` doesn't count. `detach` it (or make it again) to put it on top.
- Calling `attach` or `place` again on a placed box **replaces** its placement: the options you give, and every other option at
  its default. To change some options and keep the rest, use `update`.

A full example, with a bar, a tag and a marker, is [plates](examples/plates.md): `/addon load example-plates`.

## Putting a box back

```lua
vana.world.detach(box)
```

The box is drawn at its `x` and `y` again.

## Things to know

- **A frame behind, a pixel off.** The widgets are placed from the camera of the frame being drawn, so they stay on their
  point while the camera moves. With frame generation the frames between are moved as a name's are.
- **Pictures are made as the box changes.** A box whose text changes every frame is remade every frame; change it when it
  changes.
- **The Ashita `Nameplate` plugin** is held off while any widget is placed (it rewrites the same code); it loads again when
  the last one is gone.
- **Positions are the game's.** `z` is the height as the game stores it, and `up` lifts a point; put a marker over an NPC at
  `entity.x, entity.y, entity.z` with `up` of its height.

## Where a name is: `vana.world.name_at`

```lua
local x, top, bottom = vana.world.name_at(index)   -- nil when its name wasn't drawn in the last moments
```

For an addon that draws by a name with its own drawing (ImGui, `vana.ui` boxes placed by hand) rather than attaching a
box: the target's place across the screen (its name is centred on it) and the letters' top and bottom, in the interface's
pixels (`vana.ui.screen`'s), as the game drew the name over target `index` within the last 300 ms. A mark drawn just over `top` sits where the game puts the name,
which is the right height for any model; the top of a model's bounding box isn't (a dragon's raised wings are far over its
head). `nil` while the name isn't drawn: too far, hidden by the nameplates settings, or the names aren't VanaCore's to see.
`vana.game.entity(index)` has the same three as `name_x`, `name_top` and `name_bottom` (VanaCore 0.8.224 or later).

## Rings of light on the ground: `vana.world.marks`

```lua
vana.world.marks({
    { x = e.x, y = e.y, z = e.z, radius = 1.2, color = '#FFD040' },
    { x = f.x, y = f.y, z = f.z, radius = 0.8, color = '#5BE0FF', strength = 0.6 },
    { x = 10, y = -20, z = 0.5, radius = 1.5, color = '#FF4F6A', shape = 'square', height = 6, glyph = '1' },  -- a waymark
})
vana.world.marks()   -- off
```

A ring of light on the ground under something, in a colour of yours: a bright line with a glow round it and a faint pool
inside, breathing, and a ripple going out from it now and then. With a `height`, a column of light rises from the ring to
that height, in streaks drifting upward and thinning to nothing at the top: a waymark, telling people where to stand. The
list is your addon's whole set each time (so a mark that has gone is left out of the next call); `nil` or an empty list
takes them all off, and unloading does too. Each ring:

- `x`, `y`, `z`: its centre as the game has positions (`entity.x`, `entity.y`, `entity.z`; `z` the height), the ground
  under the thing's feet.
- `radius`: in world units, 0.1 to 50 (1); a monster's `hitbox_size` is a fair start.
- `color`: `'#RRGGBB'` (white).
- `strength`: how strongly it's drawn, 0 to 4 (1); 0 isn't drawn.
- `shape`: `'round'` (the default), or `'square'`, a square of that half-width.
- `height`: the column of light over it, in world units, 0 to 30 (0: none). Six to ten reads well on a character.
- `glyph`: a sign over the column's top (over the ring, with no column): `'A'`, `'B'`, `'C'`, `'D'`, `'1'`, `'2'`, `'3'` or
  `'4'` (`'a'` is `'A'`), as FFXIV's waymarks have them, drawn as strokes facing the camera in the ring's colour;
  `glyph_size` half its height in world units, 0.1 to 10 (0.8); `glyph_height` how high over the ring its centre floats,
  world units, 0 to 30 (0, the default: just over the column's top, or a little over the ring without one; FFXIV's hang
  about 5 up).

Numbers past their ends are held to them. A `shape` other than `'round'` or `'square'`, a `glyph` that isn't one of the
eight, a `color` that isn't `'#RRGGBB'` or an entry that isn't a table is an error naming the entry, and none of the list
is taken (the marks drawn before stay).

Unlike a box, a ring isn't a picture: the scene effects draw it from the scene's own depth, onto whatever ground the
scene has there. So it lies on a slope, and what stands in front of it (the monster it's under, a character between it
and the camera) hides that part of it, as a decal would. The column is light in the air, cut the same way: someone
standing in it is lit in front of it and hides what's behind them, and it shows against the sky. The glyph is cut by the
depth too: whatever stands between the camera and it hides it, where a sign drawn over the screen would sit over them.
A name or anything else over the column is yours to draw (project the top, `z - height` as the game has heights). The first 16 rings of every addon's together are drawn, those
within 70 units of the camera, fading out past 50. On the game's own Direct3D 8 (no scene effects) nothing is drawn:
`vana.graphics.info().active` is false there, and an addon can draw its own over the screen instead.
