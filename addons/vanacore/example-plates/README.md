# example-plates

An example of `vana.world` widgets (docs/addons/18-widgets.md): over the name of each party member and of the target it stacks a
tag (name and HP) and an HP bar, and beside the target's name a ">" marker. They are drawn in the game's 3D space, so walls hide
them as they hide a name; the target's can stay visible through walls.

- `/addon load example-plates` loads it, `/addon unload example-plates` takes everything away.
- `/plates names` hides or shows the game's own names (`vana.gameui.hide('nameplates')`), so the plates are all there is.
- `/plates walls` lets the target's plate show through walls or not.

Each box is placed once. Every frame after that only `box:set{...}` (the text, the bar's width and colour) and
`vana.world.update(box, {...})` (where it sits, whether walls hide it) run: nothing is placed twice.

It changes the game's interface only while loaded: with `/plates names` it hides the game's names. Unloading puts them back.
