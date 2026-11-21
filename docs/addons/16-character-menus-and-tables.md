# 16. The character, the menus and the game's tables: how they work

Three parts of the API that go together, each with its reference elsewhere in the series and its mechanics here:

| Part | What an addon gets | Reference |
|------|--------------------|-----------|
| The character | Everything the game keeps about you: stats, skills, merits, missions, linkshells, the pet | [4. Game data](04-game-data.md) |
| Menu triggers | A function run when the player does something in any of the game's menus, with the game's own action kept or dropped | [7. The game's interface](07-game-interface.md#triggers-on-catch-and-off) |
| Game tables | The names, descriptions, costs and numbers behind every id, as Lua tables | [4. Game data](04-game-data.md#static-game-data-vanagamedata) |

The **API Explorer** addon describes every call on this page and runs them, showing what comes back:
`/api` (it loads the addon when it isn't loaded). The last section says how to use it.

## 1. The character

```lua
local p = vana.game.player()                     -- the game's status block: stats, exp, rank, gil, merits, item levels ...
local skills, crafts = vana.game.skills(), vana.game.crafts()
local jobs = vana.game.jobs()                    -- every job's level and master level
local points = vana.game.job_points()
local pet = vana.game.pet()                      -- nil without one
local missions = vana.game.missions()
local ls = vana.game.linkshell(1)                -- or 2
local merits = vana.game.merits()                -- { [merit id] = { count, next } }
```

Every call reads the game afresh and returns a new table, or `nil` when the data isn't there yet (before login, between
zones). None of it is cached for you: a `present` handler that needs the player's stats every frame reads them once per
frame and keeps the table.

### Where it comes from

**The status block.** The game keeps one 0x438-byte block for the player: the server's status packet (0x061: HP and MP
maximums, jobs, stats, resistances, title, rank, home point, nation) is copied in whole, and the other packets fill the
rest as they come: experience points, skills (0x062), merit and limit points, job points, the status timers (0x063),
the home points known. `vana.game.player()` copies the block and hands you its fields by name; `skills()`, `crafts()` and
`job_points()` are views of the same block (a skill is the level in its low 15 bits with a capped bit on top; a craft's
rank sits in its low 5 bits and its level above). A read is one memory copy, cheap enough for every frame.

**Job levels.** The game has a function that answers "what level is job n" (the one Ashita's `GetJobLevel` calls) and
another for the master level. `vana.game.jobs()` calls both for every job. A job not unlocked is 0.

**The pet.** The game keeps its last pet status packet (0x068) as 24 bytes: the pet's index, HP%, MP%, TP and target.
`vana.game.pet()` reads them, and adds the pet's entity name and id when the entity is in range. A released pet leaves an
index of 0 behind, which reads as no pet.

**Missions.** The server's mission log (0x056, its "current" log) is copied by the game into one block: the nation and
its current mission, Zilart, Promathia and its chapter progress, the three add-ons, Adoulin, Rhapsodies, and which
expansions the player declined. `vana.game.missions()` reads it; it's `nil` until the log has come (at login). Quests and
completed missions are in other logs the game keeps as bitmaps and aren't exposed.

**Linkshells and merits: VanaCore keeps the packets.** The game shows a linkshell's name in its windows and the merit
list in its menu, but keeps neither anywhere an addon can read. So VanaCore keeps them itself from the packets as they
arrive:

- Each linkshell message packet (0x0CC) carries the slot it's for, the message, who set it and when, the permissions,
  and the linkshell's name packed six bits a letter. VanaCore decodes the name and keeps one record per slot until
  logout. The equipped pearl (a linkshell, pearlsack or linkpearl marked equipped in the inventory) carries the same
  packed name in its extra data, plus the linkshell's id and colour, so `vana.game.linkshell(slot)` finds the pearl by
  name and adds its `rank`, `color` and `id`. A slot with no message yet but a pearl still answers, from the pearl.
- The merit list (0x08C) comes at login in several packets, each a run of `{ id, next cost, count }`, and again for
  the merits that change. VanaCore keeps every entry by id; `vana.game.merits()` is that list. The points to spend and
  the cap are in `player()` (`merit_points`, `merit_points_max`).

Both stores are cleared at logout, so they're right for the next character.

### Ids

Everything is numbers: a skill 9, a buff 33, a zone 245, a merit 128. The game tables (section 3) turn them into names
and the rest: `vana.gamedata.skills[9].name`, `vana.gamedata.status_effects[33].name`, `vana.gamedata.zones[245].name`.

## 2. Menu triggers

```lua
local id = vana.gameui.on(menu, event[, items], fn)     -- hear it; the game acts as well
local id = vana.gameui.catch(menu, event[, items], fn)  -- hear it; the game's own action doesn't happen
vana.gameui.off(id)
```

`menu` is a window's name as `menu_open` gives it (`'menuwind'`, `'prty1'`, `'inventor'`), an alias (`'main_menu'` for
both pages of the main menu, `menuwind` and `socialme`; `'party_menu'`, `prty1`; `'config_menu'`, `configwi`), `'*'` for
every menu, or a list of those. `event` is
`'select'`, `'cancel'`, `'up'`, `'down'`, `'left'`, `'right'`, `'menu'` (the menu key pressed while a menu is up), or `'*'`.
`items` narrows the trigger to item ids, or names VanaCore knows (the main menu's two pages and the Party list are named
in [7. The game's interface](07-game-interface.md#triggers-on-catch-and-off)). The function is always last and gets
`e`: `menu` (the window's own name, never the alias), `event` (the event's name, or the game's number for one without a
name), `key` (the event's number), `item`, `name` (`''` when VanaCore has no name for it), `caught`.

```lua
-- the player picks Equipment in the main menu: our window, not the game's
vana.gameui.catch('main_menu', 'select', 'equipment', function (e)
    open_my_equipment()
end)

-- every choice in every menu, to find the ids of a menu nobody has named yet
vana.gameui.on('*', 'select', function (e)
    print(('%s: item %d%s'):format(e.menu, e.item, e.name ~= '' and ' (' .. e.name .. ')' or ''))
end)
```

### How it works

Every one of the game's menus is a window made from a layout, with a handler object behind it that does what its items
do. When the player presses a key, clicks an item or moves the stick, the window acts first (moves its cursor, marks
the item, plays the sound, closes itself on cancel) and then hands the event and the cursor's item to its handler,
through one function that every window shares. That hand-over is the whole of what a menu does when chosen, so it's
where VanaCore sits:

1. **The hook goes in at the first trigger.** VanaCore finds the hand-over in the game's code by its bytes and replaces
   the call into the handler with a call to its own stub. Nothing is patched until an addon sets a trigger, and the
   patch is five bytes.
2. **Each hand-over asks the registry.** The stub gives VanaCore the window, the event and the item. VanaCore reads the
   window's layout name (`menuwind`, `prty1`), then goes through every loaded addon's triggers: those whose menus, event
   and items match are hit.
3. **Observers are queued, catches decide now.** Each hit addon gets a `gameui_trigger` event at the next frame, which
   `vana.gameui` turns into your function's call with `e` filled in. If any hit is a catch, the stub returns to the game
   as the handler would have, without calling it; otherwise it jumps into the handler as the game would have.
4. **The window has already done its part.** A caught select looks like the game's own: the cursor moved, the item
   lit, the sound played. Only the action behind it (opening the Equipment window, say) is left out. A caught cancel
   still closes the window, since the window closes itself before the hand-over.

Keyboard, controller and mouse all come through the same hand-over (the manager's key dispatch and the mouse frame
both call it), so a trigger sees every way of choosing. Two addons can hear the same event, and one catch among them
is enough for the game's action to be left out; every listener's `e.caught` says so.

A trigger lasts until `off(id)` or the addon goes. `/addon reload` is a go, so an addon sets its triggers in its `load`
handler, not at the top of the file. Your function runs at the next frame, like every event, so it can't veto a choice
after the fact: a catch is declared, not decided in the handler.

**Finding a menu's items.** `vana.gameui.on('*', 'select', ...)` with a `print` names the window and item id of
whatever you pick, and the `menu_open` event names each window as it opens. With the player's **Single main menu** on,
page 2's items are `menuwind`'s 16 to 27 and keep page 2's names, so `'main_menu'` with an item name works either way.

**Not covered.** The windows VanaCore draws itself (ImGui), the chat line, and anything the game does without a menu
(a macro, a typed command). An addon that wants those listens for `command` or `key`.

## 3. The game's tables

```lua
local id = vana.gamedata.find('spells', 'Cure IV')        -- 4
local s = vana.gamedata.spells[id]                        -- name, mp, cast_time, recast, jobs = { [3] = 41, ... }, ...
local item = vana.gamedata.items[26963]                   -- Onca Suit: level, item_level, slot_names, mods = { STR = 93 }
vana.gamedata.status_effects[33].name                     -- Haste
for id, zone in pairs(vana.gamedata.zones) do ... end
vana.gamedata.tables()                                    -- what's shipped, with row counts and dates
```

Twenty tables: `spells`, `abilities`, `weapon_skills`, `status_effects`, `zones`, `items`, `key_items`, `titles`, `jobs`,
`skills`, `elements`, `merits`, `job_points`, `traits`, `mounts`, `monster_families`, `augments`, `slots`, `skill_caps`,
`skill_ranks`. [4. Game data](04-game-data.md#static-game-data-vanagamedata) lists each one's fields.

### Where they come from

Nothing in these is read from the running game. They're generated, by `conflux/tools/gen_gamedata.py`, from the two
places the facts live:

- **The game's DATs**, for what the client shows: names, descriptions, help texts, the item records (level, slots,
  races, jobs, damage, delay, flags, the description), the spell and command records (icons, menu order). The script
  reads them through xi-tools' library, the same decoders its model viewer uses.
- **CatsEyeXI's server**, for what the server decides: a spell's MP, cast time, recast and the level each job learns
  it at; an ability's recast and its timer id; a weapon skill's skillchain properties; an item's mods, auction house
  category and base price; a zone's type and music; a status effect's flags. The script parses the server's `.sql`
  dumps directly (no database running) and takes the names for the numbers from the server's own headers (modifier
  names, target and status flags, skillchains, zone types), so a renamed enum shows up on the next run.

Where the two name the same thing the client's name is used (it's what the player reads) and the server's kept beside
it (`server_name`). Ids are the ones the game uses on the wire, which are also Windower's and Ashita's: spell 4 is
Cure IV everywhere, ability 16 Mighty Strikes, status 33 Haste. The game's own command records sit at `id + 0x200` for
job abilities and pet commands; the script does that arithmetic so you don't.

### How they load

The tables are Lua files under `addons\vanacore\libs\gamedata\`, one `return { [id] = { ... }, ... }` each. `vana.gamedata`
is empty until an addon touches a table; the first `vana.gamedata.spells` runs the file and keeps the result. Spells
take a few milliseconds. Items would be 18 MB in one file, so they're written in parts of 4,096 ids: `items_6.lua` holds
ids 24576 to 28671, and `vana.gamedata.items[26963]` loads that part alone, in about 50 ms. `pairs()` on `items` sees the
parts loaded so far; `vana.gamedata.all('items')` loads every part first (about a second) and `find()` does the same the
first time it's asked, to build its name index. Every addon's Lua state has its own copy, as each has its own copy of
anything it loads.

Text is UTF-8 as the client shows it, with the element icons as `{Fire}`, `{Ice}` and so on; to print a description to
the chat, pass it through `vana.encoding.to_shift_jis` first. Times are seconds where the server keeps milliseconds
(`cast_time`, `recast`); an item's own `cast_time` and delays are the client's numbers as they are.

To regenerate after a DAT or server change, with xi-tools' Python (its `.env` names the game install):

```
D:\xi-tools\.venv\Scripts\python.exe conflux\tools\gen_gamedata.py [--only spells,items]
```

## 4. The API Explorer

`/api` (or `/apiexplorer`) shows or hides it; `/api show` and `/api hide` only show or only hide it. The addon isn't
loaded at start: `/api` loads it and shows it then. The window has two tabs, each with a list down the left (drag its right edge to widen it), and opens on the
tab it was last on.

### Reference

Every table and function of the API, with what it takes, what it returns and what it does:

- The list is the tables (`vana`, `vana.events` and the events, `vana.tasks`, `vana.game`, ..., and the `addon` table).
  Click one to open its page; its entries are listed under it.
- A table's page has the docs page it's from, its notes (the properties `vana.ui`'s boxes take, the elements
  `vana.gameui` knows, ...) and each entry: its signature, what it does, and for a value its value now (`vana.version`,
  `vana.paths.*`). **Events** lists every event `vana.on` takes, with what the handler gets.
- The filter box finds an entry by any word of its name or description.
- **Try it** puts a call to the function in the Snippets tab's code box, to fill in and Run. **Listen** (an event's
  entry) puts code there that counts the events and shows what the last one handed its handler; **Stop listening** (or
  another Run) ends it. The `d3d_*` draw events, `load` and `unload` have no Listen.
- The descriptions are this series' ([15. API reference](15-api-reference.md), [2. Events](02-events.md)), made into
  the addon's `reference.lua` by `conflux\tools\gen_apiexplorer_reference.py`. What's listed is read from the running
  VanaCore's own `vana` table: an entry in red is one the docs name that this VanaCore hasn't (an older one); an entry in
  purple is one this VanaCore has that the docs don't describe yet. `vana.imgui` lists every ImGui function by ImGui's
  own names ([6. Drawing](06-drawing.md#whats-there)).

### Snippets

A window in the spirit of Postman: pick a call, **Run** it and read what it returns, with the time it took. Each run
replaces the last.

- The calls are in groups: Character, Party and targets, Items, Abilities and spells, World, Widgets (`vana.world`), Game
  data (`vana.gamedata`), Web requests (`vana.http`, `vana.ws`), Game interface (`vana.gameui`) and Menu triggers. The
  filter box finds a call by name. **Uses** under a call's name are the reference entries its code uses.
- The code box is editable: change an index or a slot and run again. **Copy code** and **Copy output** put the text on
  the clipboard; **Clear** empties the output. **Run every second** watches a value change.
- A call that returns a promise (Web requests) is waited for while the game goes on: the output says so, then shows what
  the promise holds, or why it was rejected. A new Run cancels one still waiting.
- **Widgets** draw boxes in the game's 3D space ([18. Widgets](18-widgets.md)) until **Remove** (or the addon unloads);
  the world calls need the d3d11 or d3d12 renderer.
- **Game interface** finds the names `vana.gameui` takes: the elements, the game's windows up now, and **Find a window's
  name**, which lists each window as you open it until **Stop finding window names**.
- **Menu triggers** entries are one-shot tests: Run sets the trigger and the output says what to do in the game
  (open the main menu and pick Equipment, say); when it fires, the output reports what was caught and the trigger
  removes itself. One test waits at a time; **Cancel a waiting test** drops it.

Every call is a complete snippet to paste into an addon. Lines calling an `apiexplorer_` function are the explorer's own
reporting; drop them.
