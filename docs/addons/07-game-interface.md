# 7. The game's interface: `vana.gameui`

Addons can draw their own windows. What they couldn't do safely on their own is change the **game's own** interface:
hide the party list because they draw their own, put a button in the game's Config menu, take over a menu item, move
the Target window. VanaCore knows how to do each of those, so an addon asks VanaCore and VanaCore does it.

> **Call it and it's done.** There's no file to write and nothing for the player to allow: `vana.gameui.hide('party_list')`
> hides the party list. In return, **say in your addon's README what it changes in the game's interface**, so the
> player knows before they load it. /vana > Addons shows the README.

Two rules hold throughout:

1. **VanaCore does the change**, through the hook it already has. Two addons never patch the same window.
2. **Everything is put back when the addon goes.** Unloaded, crashed or reloaded, the game's interface is as it was.

An Ashita plugin asks for the same through VanaCore's plugin API: [plugins/README.md](../plugins/README.md).

`vana.gameui` needs **VanaCore 0.8.0.27 or later**; on an older one it's `nil`. Hiding the party list and moving the
Target window have been seen working in game (2026-10-21); the other elements are built and work the same way but
haven't each been tried in game yet.

## The calls

All one word, in pairs:

```lua
vana.gameui.hide(element)                     -- not drawn -> true
vana.gameui.show(element)                     -- drawn again
vana.gameui.move(element, x, y)               -- percents of the interface -> true | false, 'held by X'
vana.gameui.move(element)                     -- back to the player's own setting
vana.gameui.rect(element)                     -- where it is on the screen -> left, top, right, bottom | nil ('target_window')
vana.gameui.add(menu, label, fn[, colour])    -- your button, labelled `label` (1 to 10 letters); fn() when chosen
vana.gameui.remove(menu)                      -- the button taken out
vana.gameui.replace(item, fn)                 -- fn() instead of the game's action -> true | false, 'held by X'
vana.gameui.restore(item)                     -- the item given back to the game
vana.gameui.image(element, file[, options])   -- your picture in place of the game's -> true | false, 'held by X'
vana.gameui.image(element)                    -- the game's own picture again
vana.gameui.elements()                        -- what VanaCore can change: { { id, name, actions = { 'hide', ... } }, ... }
vana.gameui.hidden(element)                   -- who has it hidden: 'yunaui, other' (addons' and plugins' names) | nil
vana.gameui.stats_dump()                      -- the Equipment window's stats, each step, into the log (/vana statsdump)
vana.gameui.hide(window)                      -- any of the game's windows by its name ('configwi'): drawn into nothing
vana.gameui.window(window)                    -- the window as it stands: place, cursor, rows, text, pictures | nil
vana.gameui.windows()                         -- the windows up: { 'menuwind', 'prty1', ... }
vana.gameui.press(window, row[, event])       -- the player's choice made for them -> true | false, why
vana.gameui.watch(window, fn)                 -- fn(e) when it opens, changes, closes: e.what, e.name, e.window -> id
vana.gameui.unwatch(id)
```

What a call returns:

| Returns | Means |
|---------|-------|
| `true` | Done. |
| `false, 'held by X'` | `move` and `replace` have one holder at a time, the first addon to ask. VanaCore keeps that one's, and when it lets go, the next in line gets it. Ask once; yours takes effect then. |
| an error | An element VanaCore doesn't know, an action that element doesn't take, a label that isn't 1 to 10 letters, a colour that isn't one of the names or `#RRGGBB`. That's a bug for the author to fix, so it fails loudly. |

`hide` is shared: any number of addons can hide the same element, and it's shown again once none of them do. Hiding
doesn't remove: a hidden window isn't drawn, but the game still has it, so targeting, menus and macros that use it work
as before.

The player sees your hide in the **HUD Manager** (`/hud`) as if they had switched it on themselves: the element's Hide
switch shows on and is locked, its tooltip names your addon, and its box isn't drawn in the game. The switch goes back
when you show the element again or unload. `vana.gameui.hidden(element)` answers who has an element hidden, as one string
of names (or `nil`); fancychat counts for `chat_log` and `chat_input`. It doesn't include the player's own switch.

## The elements

| `element` | The player sees | `hide` | `move` | `add` | `replace` | `image` |
|-----------|-----------------|:------:|:------:|:-----:|:---------:|:-------:|
| `chat_log` | Chat log (both windows) | yes | | | |
| `chat_input` | Chat line | yes | | | |
| `party_list` | Party list | yes | | | |
| `alliance_lists` | Alliance lists (both) | yes | | | |
| `target_window` | Target window | yes | yes | | |
| `target_arrow` | The arrow over the target | yes | | | | yes |
| `status_icons` | Status icons | yes | | | |
| `macro_bar` | Macro bar (all four palettes) | yes | | | |
| `network_window` | Network window | yes | | | |
| `nameplates` | The names over characters' heads | yes | | | |
| `config_menu` | Config menu | | | yes | |
| `party_menu.find_member` | Party > Find Member | | | | yes |

### `chat_log`

Both chat windows are kept closed, the way fancychat keeps them closed. Messages still arrive, so your addon can draw
its own log from `text_in`. Nothing flickers when a line comes in or the chat line opens.

### `chat_input`

The line you type chat into is kept off the screen, as the player's `[client] input.hide` does. Typing still works, so
your addon can draw the line itself.

### `party_list` and `alliance_lists`

Not drawn. The party itself doesn't change: targeting a member works, and `vana.game.party()` still reads the members.
Usually hidden together.

### `target_window`

- **`hide`**: not drawn; the target is unchanged.
- **`move`**: `vana.gameui.move('target_window', x, y)`. `x` and `y` are percents of the game's interface from its top
  left (`0, 0` top left, `100, 100` bottom right). `x` is where the window's **middle** goes and `y` where its **top**
  goes, as the player's own Target window settings in /vana have it: `50, 4` is the middle of the top edge. The window
  is kept on screen. Your place goes before the player's own `[client] target.move/x/y` while your addon holds it;
  `vana.gameui.move('target_window')` gives it back.
- **`rect`**: `vana.gameui.rect('target_window')` is where the window is on the screen now, in the interface's pixels
  (`vana.ui.screen`'s): its left, top, right and bottom, for drawing by it (a mark beside the target's name). `nil` while
  it isn't open (nothing targeted) or is hidden. It follows the window wherever it is (the player's move, yours, the HUD
  Manager's), so read it each frame you draw. Only `target_window` has a place to give: any other element is an error.
  Needs **VanaCore 0.8.218 or later** (`vana.gameui.rect` is `nil` before).

### `target_arrow`

The arrow the game draws over the targeted character's head, 20 x 32 interface units with its point at the target.

- **`hide`**: not drawn. Targeting is unchanged, and the sub-target cursor (the one `<st>` moves) stays the game's.
- **`image`**: your own picture in its place:

```lua
vana.gameui.image('target_arrow', 'arrow.png')                 -- a PNG in your addon's folder, its pixels as units
vana.gameui.image('target_arrow', 'arrow.png', { width = 24, height = 40, y = -4, color = '#FFD080', opacity = 0.9 })
vana.gameui.image('target_arrow')                               -- the game's arrow again
```

  `file` is a PNG (BMP and JPEG work too), relative to your addon's folder or a full path. The options, all optional:
  `width` and `height` in the interface's units (left out, the picture's pixels are its units: a 20 x 32 picture is the
  game's size); `x` and `y` move it from the game's anchor, the arrow's point, which is at the picture's bottom middle
  (`y = -4` lifts it 4 units); `color` tints it (`'#RRGGBB'`); `opacity` 0 to 1. Like `move`, one addon holds it at a
  time (`false, 'held by X'` for the next, whose picture takes over when the first lets go), and yours goes before the
  player's own `[client] target.arrow.image`. The picture is drawn in VanaCore's interface pass, over the game's
  windows, where the game had the arrow; a picture that can't be read is said in vanacore.log and nothing is drawn.
  The player's **Show the arrow over the target** off hides it, picture or not.

### `status_icons`, `macro_bar`, `network_window`

Not drawn. `move` isn't available for them yet.

### `nameplates`

**`hide`**: no name is drawn over any character's head (players', NPCs', monsters', trusts', pets', your own), nor the
line under it (a job master's stars). Targeting, the target window and the target arrow are unchanged. Draw your own
instead with [widgets](18-widgets.md); they are placed where the name would be even while it's hidden. The player's
own settings can leave out a kind of name by itself (`[graphics] nameplates.hideown`, `.hideparty`, `.hidealliance`,
`.hideplayers`, `.hidenpcs`, `.hidemonsters`, `.hidetrusts`, `.hidepets`). An addon that draws the party's and alliance's
names in its own bars can ask its players to turn on `.hideparty` and `.hidealliance` (/vana > Game Options >
Nameplates), so those names aren't shown twice.

### `config_menu`

The game's Config menu (Main Menu > Config). VanaCore's own buttons are already below the game's 13: VanaCore (purple),
then HUD, FPS Graph and Game Stats. `vana.gameui.add('config_menu', label, fn[, colour])` puts yours after them;
`fn()` runs, on the next frame, when the player chooses it.

- **Label:** 1 to 10 characters, drawn letter by letter in the game's own menu font. A to Y, a to z and spaces work;
  **Z and the digits aren't drawn yet** and show as gaps. A label too wide is set a little tighter.
- **Colour**, left out for the game's blue: `'blue'`, `'green'`, `'yellow'`, `'red'` (Monstrosity's), `'purple'`
  (VanaCore's), `'grey'` (untinted), or `'#RRGGBB'` (tints a grey button, so bright colours read best). Anything else
  raises an error.
- **Room:** 4 buttons fit after VanaCore's, and its own three take three, so one addon's button fits, the first to ask.
  The rest aren't shown (VanaCore's log says so).
- **When it appears:** the list is rebuilt with your button the next time it's closed and opened.
- **Help line:** the game's help line for your button names your addon, so the player knows whose it is.

### `party_menu.find_member`

`vana.gameui.replace('party_menu.find_member', fn)`: choosing Party > Find Member runs `fn()` (next frame) instead of
the game's search. One holder at a time; `vana.gameui.restore('party_menu.find_member')` gives the item back.

## Any window: `hide`, `window`, `windows` and `press`

For an addon that draws the game's menus its own way while the game keeps running them: hide the game's window, read what
it shows, draw that, and send the player's choice back. **Untested in game** (built 5 November 2026).

```lua
vana.gameui.hide('configwi')            -- the Config menu drawn into nothing; show('configwi') gives it back
local shown = nil                       -- what to draw: the window as the game last drew it
vana.gameui.watch('configwi', function (e)
    shown = e.window                    -- nil once it closes
end)
-- in your drawing: for _, row in ipairs(shown.rows) do ... row.text 'Gameplay', row.cursor, row.x/y/w/h ... end
vana.gameui.press('configwi', 3)              -- cursor on row 3, then select
vana.gameui.press('configwi', nil, 'cancel')  -- back out, as the player's cancel key
```

### `hide(window)`

`window` is a window's name as `menu_open` gives it (`'menuwind'`, `'configwi'`, `'prty1'`, `'searchma'`) or as
`vana.gameui.windows()` lists them: any of the game's windows, not only the elements above. Shared, put back when the addon
goes and listed by `hidden(window)`, as an element's hide is. Hidden this way:

- **Nothing of it is drawn**, its cursor (the hand) and the gold highlight on the cursor's row included: the game draws
  those as part of the window. Draw your own from `window().cursor`.
- **Its draw still runs** (into nothing), so `window()` still reads its text and pictures, and the game's own work in it
  goes on.
- **The keys and the controller still work it**, as before: the cursor moves, items are chosen, `on` and `catch` hear it.
- **The mouse doesn't**: no row of it is under the pointer, so a click over where it was does nothing. Clicks on your own
  drawing go to the game with `press`.
- Only that window: its help line at the bottom (`'helpwind'`) and the windows it opens are their own windows.

### `window(window)`

The first window of that name that's up, as it stands, or `nil`:

| Field | What |
|-------|------|
| `name` | Its name (`'configwi'`) |
| `x`, `y`, `w`, `h` | Its place on the screen, in the pixels addons draw in (`vana.ui.screen()`) |
| `scale` | Those pixels to one of the game's interface units: for sizing your drawing as the game's |
| `cursor` | The id of the row the cursor is on |
| `count` | How many rows the cursor goes over |
| `focused` | It's the window the keys go to |
| `closing` | It's closing (its last frames) |
| `hidden` | An addon hides it (`hide(window)`) |
| `drawn` | `lines` and `pictures` are from a draw of it: `false` the first time you ask for a window (see below) |
| `rows` | Its buttons, a list in order of id: `{ id, text, x, y, w, h, cursor, disabled, drawn, state }` each (`rows[1]` is the lowest id, not id 1: find a row by `row.id`) |
| `lines` | The text its last draw made, in runs: `{ text, x, y, w, h, color }` each |
| `pictures` | The pictures its last draw made that aren't letters or button bars: `{ name, x, y, w, h, color }` each |

- **`rows`** are the window's buttons: the main menu's items, a Config page's, the Party menu's. `id` is the game's
  number for the row, the one `cursor`, `press` and `on`'s `e.item` use. `text` is the button's label as the game's menu
  data gives it (`'Status'`, `'Region Info'`), read whether the window is drawn or not; a button with no label (an item
  list's rows, an arrow) has `''`. `disabled` is greyed out; `drawn` false is a row the window doesn't draw itself (the
  main menu's Map, which the menu draws on its own) or doesn't show. `state` is the game's own number for the row (5
  greyed out, 1 just chosen).
- **`lines`** are everything the window's last draw wrote, its labels included: the item names and counts of a list, a
  Config page's settings, an NPC's choices. Letters drawn next to each other in the same colour make one run; a gap of a
  few pixels is a space. The text is the game's own, Shift-JIS (`vana.encoding` converts it); ASCII is as it is. `color`
  is the colour the game draws it in, `'#RRGGBB'`.
- **`pictures`** are the rest of what it drew, by the game's name for the picture (`'menu    keytop'`): icons, on/off
  boxes, arrows, its frame's pieces. What a picture means depends on the window: look at a window's once with
  `vana.gameui.window` and you'll see which names to look for.
- **Reading starts the keeping.** `lines` and `pictures` come from the window's draws, which VanaCore keeps only for the
  windows addons watch (`watch`, below) or have asked about in the last 3 seconds. So the first `window()` of a window
  nobody watches has `drawn = false` and no lines. Use `watch` instead: its `e.window` is always whole.
- Places are the screen's pixels. A window that isn't drawn still has its place, where the game would draw it.

### `watch(window, fn)` and `unwatch(id)`

`fn(e)` runs when the window has something new to show, with the window already read:

| `e.what` | When |
|----------|------|
| `'open'` | Its first draw since it opened (or since your watch began, when it was already up) |
| `'change'` | A draw that shows anything else: it moved, the cursor moved, a row was greyed out or chosen, its text or pictures changed |
| `'close'` | Once, when it starts to close or is gone |

`e.name` is the window's name, `e.window` the same table `window(e.name)` gives, whole (`drawn` true: the draw it's
from is the one that just happened), `nil` at `'close'`. `window` is a name, `'main_menu'` (both pages), `'party_menu'`,
`'config_menu'`, `'*'` for every window, or a list of those. Returns an id for `unwatch(id)`; a watch ends when your
addon unloads too.

- **No frame late.** The game draws its windows, then VanaCore runs the addons' frame, so `fn` runs in the frame the game
  drew what it's given: draw from `e.window` in your own drawing that frame.
- **Only on change.** A window that sits still calls `fn` once, at `'open'`. Keep the last `e.window` and draw from it
  every frame.
- **Kept while watched.** A watched window's draws are kept for as long as you watch it, hidden or not. Watching `'*'`
  keeps every window's (the chat log's too, which changes often): watch the ones you draw.

```lua
local menus = {}   -- name -> the window as last drawn
vana.gameui.watch({ 'main_menu', 'config_menu', 'prty1' }, function (e)
    menus[e.name] = e.window
end)
```

### `windows()`

The names of the windows up, in the game's order: `{ 'helpwind', 'partywin', 'menuwind', ... }`.

### `press(window, row[, event])`

The player's choice made for them: the window's cursor put on `row` (a row's `id`; `nil` leaves it where it is), then
`event` given to the window as the keys give it: `'select'` (left out), `'cancel'`, `'up'`, `'down'`, `'left'`, `'right'`,
`'menu'`, or the game's number. It goes through the game's own key input, so the window plays its sound, marks the item,
opens what it opens, and `on` and `catch` hear it as they hear the keys.

Returns `true`, or `false` and why: `'configwi' isn't up` (or it's closing), `has no row 40` (past `count`), `isn't
taking keys now` (it has just taken one: try on the next frame). Press only what's up and has the row: a row past the ones the cursor goes over
isn't one to choose.

## Checking for it

An addon that should still load on an older VanaCore, or an Ashita or Windower addon that also runs in real Ashita or
Windower (every addon has `vana` here, whatever API it's written for):

```lua
if vana ~= nil and vana.gameui ~= nil then
    vana.gameui.hide('party_list')
end
```

## A full example

example-minparty (`addons\vanacore\example-minparty`) hides the party and alliance lists, draws a small list of its own in their
corner, moves the Target window to sit above it, and adds a Party button to the Config menu that shows or hides its
list. Its header says what it changes, as a README would.

```lua
vana.events.on('load', function ()
    vana.gameui.hide('party_list')
    vana.gameui.hide('alliance_lists')
    local moved, why = vana.gameui.move('target_window', 88, 70)   -- middle 88% across, top 70% down
    if not moved then
        vana.log('minparty: the Target window stays where it is (' .. tostring(why) .. ')')
    end
    vana.gameui.add('config_menu', 'Party', toggle)
end)
-- no unload handler needed: VanaCore puts everything back
```

One example per task is in [examples](examples/README.md): hide the chat, add a button, hide the party list (from an
Ashita addon too), move a window, replace a menu item, catch the Equipment item and open your own window.

## Triggers: `on`, `catch` and `off`

Everything the player does in any of the game's menus goes from the menu's window to its handler as an event and the
item under the cursor: select, cancel, the cursor moving, the menu key. VanaCore hooks that one hand-over (it covers
the keyboard, the controller and the mouse), so an addon can hear any of it, in any menu, and keep it from the game:

```lua
-- hear it; the game acts as well
local id = vana.gameui.on(menu, event[, items], fn)
-- hear it, and the game's own action doesn't happen
local id = vana.gameui.catch(menu, event[, items], fn)
vana.gameui.off(id)
```

| Argument | What |
|----------|------|
| `menu` | A window's name as `menu_open` gives it (`'menuwind'`, `'prty1'`, `'inventor'`), an alias (`'main_menu'`: both pages of the main menu, `menuwind` and `socialme`; `'party_menu'`: `prty1`; `'config_menu'`: `configwi`), `'*'` for every menu, or a list of those. `e.menu` is the window's own name, never the alias |
| `event` | `'select'`, `'cancel'`, `'up'`, `'down'`, `'left'`, `'right'`, `'menu'` (the menu key while a menu is up), `'*'` for every one, or the game's number |
| `items` | An item id, a name VanaCore knows, or a list of them: the trigger is for those items only. Left out: every item |
| `fn` | Always last. `fn(e)`, at the next frame: `e.menu`, `e.event` (the event's name, or the game's number for one without a name), `e.key` (the number), `e.item` (the item's id), `e.name` (its name when VanaCore knows it, else `''`), `e.caught` (the game's action was kept from it, by your catch or another addon's) |

`catch` is what a custom equipment screen wants: the player picks Equipment in the main menu, your window opens, the
game's doesn't. The game's window has already moved its cursor, marked the item and played the select sound when the
event is caught, so it looks as the game's own choice does; only the handler's action is left out.

```lua
vana.gameui.catch('main_menu', 'select', 'equipment', function (e)
    open_my_equipment()          -- e.name is 'equipment', e.item 2, e.menu 'menuwind'
end)

vana.gameui.on('*', 'select', function (e)   -- every choice in every menu, for finding items' ids
    print(('%s: item %d%s'):format(e.menu, e.item, e.name ~= '' and ' (' .. e.name .. ')' or ''))
end)
```

The items VanaCore knows by name:

| Menu | Items |
|------|-------|
| `menuwind` (main menu, page 1) | 1 `status`, 2 `equipment`, 3 `magic`, 4 `items`, 5 `abilities`, 6 `party`, 7 `trade`, 8 `search`, 9 `linkshell`, 10 `synthesis`, 11 `region_info`, 12 `map` |
| `socialme` (main menu, page 2) | 1 `missions`, 2 `quests`, 3 `key_items`, 4 `view_house`, 5 `set_bazaar`, 6 `macro_palette`, 7 `config`, 8 `help_desk`, 9 `communication`, 10 `current_time`, 11 `shut_down`, 12 `log_out` |
| `prty1` (Party list) | 1 `seek_party`, 2 `find_member`, 3 `request`, 4 `view_request`, 5 `autogroup`, 6 `languages`, 7 `alarm_type` |

With the player's **Single main menu** on, page 2's items are `menuwind`'s 16 to 27 and keep page 2's names, so
`'main_menu'` with a name catches either way. Any other menu's items go by id: `vana.gameui.on('*', 'select', ...)`
prints them as you use the menu, and `menu_open` names the windows.

A trigger lasts until `off(id)` or the addon unloads (`/addon reload` included: set them in your `load` handler).
Several addons can hear the same event; one catch is enough for the game's action to be left out, and every listener's
`e.caught` says so. Triggers don't fire for the windows VanaCore draws itself (ImGui) nor for the chat line: only for
the game's menus. How the hook works is in [16. The character, the menus and the game's tables](16-character-menus-and-tables.md#2-menu-triggers).

## The player's side

- The addon's README says what it changes. The /vana window shows it on the Addons page.
- Each addon button in the Config list has a help line saying which addon it's from.
- To undo it: unload the addon. Everything it changed is put back at once.

## What this is not

It's not a sandbox. Addons have `vana.memory.write`, Lua's `io` and LuaJIT's `ffi`, so they can patch the game any way
they like. What this gives is one place that does the changing (so two addons don't each patch the party list; `move`
and `replace` have one holder, `hide` is shared), everything put back when the addon goes, even after a crash, and an
easy way that is also the safe way.

## For VanaCore's own code

| File | What |
|------|------|
| `src/core/gameui.hpp`, `gameui.cpp` | The element list (`kKnown`), what each loaded addon has asked for, and what the hooks call (`skip`, `hidden`, `placed`, `config_items`, `replaced`) |
| `src/core/lua_api.cpp` | `vana.gameui`'s native half (`open_gameui`): `hide`, `move`, `elements`, and the `native_add` / `native_replace` that `vana.lua` wraps |
| `src/core/lua/vana.lua` | `add`, `remove`, `replace`, `restore`, `show`: keeps the addon's functions and calls them on the `gameui_button` and `gameui_replaced` events |
| `src/core/addons.cpp` | `gameui::attach` when an addon loads (before its file runs), `gameui::release` in `remove()`, the two events queued for one addon at the next frame |
| `src/core/chat.cpp`, `targetwin.cpp`, `partymenu.cpp`, `configmenu.cpp` | The hooks that ask gameui: the window draw skip, the chat log and line, the Target window's place, Find Member, the Config list's items |
| `src/core/gamewin.hpp`, `gamewin.cpp` | `window`, `windows`, `press`: the menu manager's windows read, their buttons' labels from the game's letter sprites, each window's draw kept (or drawn into nothing for `hide(window)`, `vanacore_gfx_drop_draws`), the game's SetCursol and key input |
| `src/core/lua_gamewin.cpp` | Their Lua half; `chatfont.cpp`'s letter printer hook and `itemslot.cpp`'s quad hook hand gamewin what a window draws, `menuhover.cpp` keeps the mouse off a hidden window |

Adding an element: `hide` for any game window with a layout name is one line in `kKnown`; `move` needs the module
that places the window to ask `gameui::placed()` (three lines, as `targetwin.cpp` does); `add` and `replace` need that
menu's handler hooked. Then add it to the table above.
