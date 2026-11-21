# API Explorer

A window for addon authors with two tabs: **Reference**, every table and function of VanaCore's addon API with what it
takes, what it returns and what it does; and **Snippets**, calls to run in the game, read what they return and copy into
your own addon.

- `/api` (or `/apiexplorer`) shows or hides it, `/api show` and `/api hide` only show or only hide it. It isn't loaded
  at start: `/api` loads it when it isn't (the /vana addon does that), as `/addon load apiexplorer` does.
- Each tab has a list down the left: drag its right edge to widen it. The ≡ at the left of the top line hides the list
  (the page takes its room) and shows it again, as /vana's does. The window opens on the tab it was last on.

## Reference

- The list down the left is the tables: `vana` itself, `vana.events` and the events, `vana.tasks`, `vana.game`,
  `vana.gamedata`, `vana.chat`, `vana.ui`, `vana.imgui`, `vana.gameui`, `vana.world`, `vana.settings`, `vana.http`,
  `vana.ws`, `vana.promise`, `vana.fs`, `vana.config`, `vana.binds`, `vana.memory`, `vana.system`, ... and the `addon`
  table. Click a table to open it and show its page; click it again to close it. Its entries are listed under it: click
  one to bring it into view.
- A table's page has the docs page it's from, its notes (the properties `vana.ui`'s boxes take, the elements
  `vana.gameui` knows, ...) and each entry: what it takes and returns in blue (`vana.game.entity(index) -> table`, `[x]`
  an optional argument), what it does under it, and for a value its value now (`vana.version`, `vana.paths.*`).
- **Events** lists every event `vana.on` takes: what the handler gets, what returning `true` does and when it's raised.
- The filter box finds an entry by any word of its name or description: the page then shows every entry that matches,
  under its table.
- **Snippets:** under an entry are the snippets that use it; click one to open it in the Snippets tab.
- **Try it** puts a call to the function in the Snippets tab's code box, with its signature and description above it:
  fill in the arguments, then Run. Nothing runs until you press Run.
- **Listen** (an event's entry) puts code there that listens for the event: Run it, and the output shows how many came
  and what the last one handed its handler. **Stop listening** (or another Run) ends it. The `d3d_*` draw events, `load`
  and `unload` have no Listen.
- What's listed is read from the game's own `vana` table each time the addon loads, so it's always this VanaCore's:
  - an entry in red is one the docs name that this VanaCore hasn't (an older VanaCore), and a table it hasn't at all
    says so at the top of its page;
  - an entry in purple is one this VanaCore has that the docs don't describe yet: it's listed with its name only;
  - `vana.imgui` lists every ImGui function VanaCore has, by ImGui's own names.
- The descriptions come from the addon docs (`docs/addons/15-api-reference.md`, `02-events.md` and the topic pages) through
  `reference.lua`, which `tools/gen_apiexplorer_reference.py` makes: when the docs change, run it again
  (`python tools\gen_apiexplorer_reference.py`) rather than editing `reference.lua`.

## Snippets

Pick a call from the list (the player's stats, skills, merits, missions, linkshells, the pet, the party, the target,
entities nearby, the inventory, spells and abilities, recasts, the zone), press **Run**, and read what it returns. Every
entry is a complete snippet to copy into your own addon; the code box is editable, so change an index or a slot and run
it again. **Run every second** watches a value change.

- The calls are in groups: click a group to open or close it, type in the filter box to find a call by name (every group
  with a match opens).
- **Uses:** under the call's name are the reference entries its code uses; click one to read it in the Reference tab.
- **Copy code** and **Copy output** put the text on the clipboard; **Clear** empties the output.
- The output shows the last run's result, with the time the call took; each Run replaces it.
- **Game data** reads the static tables (`vana.gamedata`): a spell by name, an item by id, your equipment and buffs by
  name, the spells your job can cast.
- **Web requests** run `vana.http` and `vana.ws` for real: the server address from the launcher's `launcher.json`, the
  response's status and headers, a promise chain, `await` and a callback, three requests at once, a 404 and a failed host,
  cancelling, a download, a WebSocket that can't connect. A call that returns a promise is waited for (the game goes on):
  the output says so, then shows what the promise holds, or why it was rejected. Needs a VanaCore with `vana.http`.
- **Widgets (vana.world)** put `vana.ui` boxes in the game's 3D space (docs/addons/18-widgets.md): a box at the target's
  nameplate (or yours), the game's names hidden and shown again, a box in the world five yalms in front of you (under the
  interface, hidden by walls) and one over the interface, a line of dots out in front of you that follows you, a tag on
  every monster nearby, where a point is on the screen, and a Remove that destroys the boxes. The world calls need the
  d3d11 or d3d12 renderer. The boxes are the addon's: they go when it unloads.
- **Game interface (vana.gameui)** finds the names `vana.gameui` takes: the elements VanaCore can hide, move or change
  (and which of those each takes), the game's windows up now, and **Find a window's name**, which lists each window as
  you open it in the game (its name, place and first row) until **Stop finding window names**. Any window can be hidden
  by that name. **Read a window** shows everything `vana.gameui.window` gives for the main menu, **Who hides what** the
  addons and plugins hiding each element, and **Hide the main menu for 5 seconds** a window hidden by its name.
- **Menu triggers** are one-shot tests of `vana.gameui.on` / `catch`: Run sets the trigger and says what to do in the
  game (open the main menu and pick Equipment, say); when it fires, the output reports what was caught and the trigger
  removes itself. One test waits at a time; **Cancel a waiting test** drops it.

The calls' fields are described in `docs/addons/04-game-data.md`; the full list is `docs/addons/15-api-reference.md`.
Changes nothing in the game, except what you Run: the Widgets entries draw and hide until you remove them, the main menu
is hidden for 5 seconds, and a Try it runs whatever call you fill in.
