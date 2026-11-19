# Plates: widgets over names, updated every frame

A tag, an HP bar and a target marker over the names of the party and the target (`addons\vanacore\example-plates\example-plates.lua`, which
you can load as it is: `/addon load example-plates`). It is the shape of a nameplate addon: several boxes stacked at a name, changed every
frame without being placed again, one of them kept visible through walls.

**Placing a stack once.** Each box is attached above the name with its own `y`, which lifts it from the name's top; the boxes are
drawn in the order they were placed, so the bar's frame is placed before its fill:

```lua
vana.world.attach(plate.back, index, { side = 'above', y = -3 })
vana.world.attach(plate.fill, index, { side = 'above', y = -4 })
vana.world.attach(plate.tag,  index, { side = 'above', y = -3 - 8 - 2 - 3 })   -- above the bar
```

**Changing it every frame.** `box:set{...}` changes what the box shows (its text, a rectangle's width and colour); the picture is
only remade when that changed. `vana.world.update(box, {...})` changes how it is placed, and only the options you give:

```lua
local width = math.max(1, math.floor(80 * hpp / 100))
plate.fill:set({ width = width, color = hp_colour(hpp) })
vana.world.update(plate.fill, { x = -(80 - width) / 2 })     -- the box is centred on the name: keep the bar's left edge
```

**Walls per box, at runtime.** `walls = false` draws a box over the world instead of testing it against the world's depth, so
the target's plate stays visible behind a wall while the rest are hidden. Change it with `update` whenever the target changes:

```lua
vana.world.update(plate.tag, { walls = not (index == target_index) })
```

**One marker for the target.** One box follows the target from name to name with the `index` option, instead of a box per
character:

```lua
vana.world.attach(marker, target_index, { side = 'left', x = -4 })     -- once
vana.world.update(marker, { index = target_index })                    -- each frame after
```

`vana.world.placement(box)` reads back how a box is placed. A plate is drawn only for characters the game gives a name; to
replace the game's names with these, `vana.gameui.hide('nameplates')` leaves the name's place in the world for the boxes
(`/plates names`). See [Widgets](../18-widgets.md) for every option.
