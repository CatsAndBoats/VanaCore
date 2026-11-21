# Examples

Complete addons to copy, one per task. Each is a folder under `addons\vanacore\` with a `.lua` file of the same name
and a README that says what it does:

```
addons\vanacore\hidechat\
    hidechat.lua      your code
    README.md         what it does, including what it changes in the game's interface
```

Load one with `/addon load <name>` (it's then in `scripts\boot.txt` and loads every start), stop it with
`/addon unload <name>`, and after an edit `/addon reload <name>`.

## Getting around the game

| Example | Shows |
|---------|-------|
| [zone_greeter.md](zone_greeter.md) | Game events, a task that waits, a per-character settings store, a command with arguments |
| [party_window.md](party_window.md) | An ImGui window drawn every frame from `vana.game.party()`, with a slider saved to settings |
| [chat_timestamps.md](chat_timestamps.md) | Reading and changing chat lines with `text_in`: a timestamp on each, and a filter |
| [web_requests.md](web_requests.md) | Web requests that never stop the game: a callback, `await` in a task, and a WebSocket that reconnects |
| [wardrobe_stats.md](wardrobe_stats.md) | Reading the Mog Wardrobes with `vana.game.container()`, and a piece's stats, augments, latent effects and sets from CatsEyeXI's data with `vana.gamedata.item_stats` |

## Boxes in the game's 3D space (`vana.world`)

| Example | What it does |
|---------|--------------|
| [plates.md](plates.md) | A tag, an HP bar and a target marker stacked over names, changed every frame with `vana.world.update`, the target's kept visible through walls |

## Changing the game's interface (`vana.gameui`)

| Example | What it does |
|---------|--------------|
| [hide_chat.md](hide_chat.md) | Hides the game's chat log (the most requested one) |
| [add_button.md](add_button.md) | Adds a button to the game's Config menu that runs your addon's code |
| [hide_party_list.md](hide_party_list.md) | Hides the game's party list so you can draw your own (XIUI's case), and does it from an Ashita addon |
| [move_window.md](move_window.md) | Moves the game's Target window |
| [replace_menu_item.md](replace_menu_item.md) | Makes Party > Find Member run your addon's code instead of the game's |
| [custom_equipment.md](custom_equipment.md) | Catches the main menu's Equipment item (`vana.gameui.catch`) and opens your own window in place of the game's; `on('*', 'select')` to find any menu's item ids |

The calls, what they return, and every element are in [The game's interface](../07-game-interface.md). In short: all one
word, in pairs (`hide`/`show`, `move`, `add`/`remove`, `replace`/`restore`), each returns `true` when done or
`false, 'held by X'` when another addon holds a move or a replace, and raises an error for something VanaCore can't do.
You never need to undo anything: when your addon unloads, reloads or crashes, VanaCore puts the game's interface back.

## The shipped addons

`addons\vanacore\` has VanaCore's own addons, which are examples too:

| Addon | Shows |
|-------|-------|
| `apiexplorer` | **Start here.** An ImGui window with two tabs. **Reference**: every table, function and event of the API with what it takes, returns and does (this series' descriptions, checked against the running VanaCore), with Try it and Listen. **Snippets**: calls to run and copy, in groups (the character, party and targets, items, abilities and spells, the world, widgets, game data, web requests, the game's interface, menu triggers). `/addon load apiexplorer`, then `/api` shows or hides it |
| `example-plates` | `vana.world` widgets over names: a tag, an HP bar and a target marker, updated every frame (`/addon load example-plates`) |
| `example-hello` | The smallest addon: a `vana.ui.text` box and a command |
| `example-minparty` | `vana.gameui` (hide, move, add) with a party list drawn in a text box |
| `sendkey` | Controller and key events, binds saved to a file, `SendInput` through the FFI |
| `keybinds` | A full ImGui window over `vana.binds`: lists, editing, key capture |
| `freecam` | `vana.camera` (pose, hold) under an ImGui window drawn with `vana\draw.lua`, the /vana window's own palette, fonts, icons and controls (an addon has a Lua state of its own, so it loads that file into a table of its own), over a hand-drawn timeline with a 0.25 s snap: keyframes recorded from the free camera and played back as a spline, `vana.settings` for the saved sequences, binds that exist only while the camera is on |
| `vana` | The /vana window itself: ImGui draw lists, `vana.config`, every panel of VanaCore's settings |
