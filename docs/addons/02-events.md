# 2. Events

An addon runs code when VanaCore raises an event. This page lists every event, what it hands your handler, and what
returning `true` does.

## Listening

```lua
vana.events.on(name, fn[, key])   -- fn runs each time `name` is raised; returns the key
vana.events.off(name, key)        -- removes it; true if one was removed
vana.on, vana.off                 -- the same, shorter
```

- `name` is case-insensitive.
- `key` names the handler so you can remove it, or replace it (a second `on` with the same key replaces the first).
  Without one, the function itself is the key: `vana.off('present', fn)`.
- Handlers run in the order they were added. An error in one is reported and the rest still run.
- You can listen for an event VanaCore doesn't raise. Nothing happens, and the log notes it once.

**Cost.** VanaCore calls into your addon only for events it listens for. An addon with no `mouse` handler is never
called for the mouse; one with no game-event handler never has the game's state compared frame to frame. Keep
handlers for high-rate events (`present`, `packet_in`, `mouse`, `key_state`, the `d3d_*` draws) short.

**Blocking.** For an event the game would act on (a command, a chat line, a packet, a key, a controller button, a draw
call), a handler that returns `true` takes it: the game never sees it. Where the event is a table, setting
`e.blocked = true` does the same. Once blocked, later addons' handlers still run and see `e.blocked == true`.

## The addon's own

| Event | Handler gets | Return |
|-------|--------------|--------|
| `load` | nothing | |
| `unload` | nothing | |
| `present` | nothing | |

- **`load`** runs once your file has run. Code at the top level of your file runs before it, so either place works;
  `load` keeps set-up together.
- **`unload`** runs on `/addon unload`, `/addon reload`, and as the game closes. Not on `/addon kill`, nor after a
  crash. After your handlers, VanaCore saves your settings stores and removes everything you drew.
- **`present`** runs once a frame, from the game's `Present`, after your tasks. Draw ImGui windows here, and read
  anything that changes frame to frame. Nothing is passed; `vana.tasks.frame()` counts the frames.

## Commands and chat

| Event | `e` | Return `true` |
|-------|-----|---------------|
| `command` | `command` (the line), `mode`, `injected`, `blocked` | The game doesn't run the line |
| `unhandled` | `command` (a `//` line nothing took, without the `//`) | Taken: no "Unknown command" |
| `text_in` | `mode`, `mode_modified`, `indent`, `indent_modified`, `message`, `message_modified`, `injected`, `blocked` | The line isn't shown |
| `text_out` | `mode`, `mode_modified`, `message`, `message_modified`, `injected`, `blocked` | The line doesn't go to the game |

- **`command`**: every command line the game is about to run, typed or sent by an addon. `e.mode` is the chat mode
  it was typed in (1 for the chat box); `e.injected` is true for a line VanaCore or an addon ran (`vana.chat.run`, a
  script, a bind). VanaCore's own commands (`/addon`, `/vana`, `/bind`) are taken before addons see them. Plugins with a
  negative priority see lines before addons; the rest after.
- **`unhandled`**: a `//something` line no addon or plugin took. Return `true` to claim it, else the chat log says
  `Unknown command`.
- **`text_in`**: a line coming into the chat log, after plugins. Change `e.message_modified` (and `e.mode_modified`,
  `e.indent_modified`) to change what's shown. The text is Shift-JIS with the game's own control codes in it. `e.mode`
  is the game's three mode bytes (`mode | flags << 8 | ... << 16`); the chat mode itself is `bit.band(e.mode, 0xFF)`.
  A line your own addon prints doesn't come back to your `text_in`, but does reach other addons'.
- **`text_out`**: a line the player typed that isn't a command, on its way to the game as chat.

[Chat and commands](05-chat-and-commands.md) has examples.

## The game's state

These are raised when something changed in the game since the last frame. VanaCore compares the game's state once a
frame, and only the parts some addon listens for. `e` is a table.

| Event | When | `e` |
|-------|------|-----|
| `login` | A character is in the world | `name`, `id`, `zone` |
| `logout` | Back at the title screen or character select (or another character logged in) | `name` |
| `zone_out` | The server sent the character out of its zone | `zone` (the one being left) |
| `zone_change` | The server put the character in a zone: every zone-in after login, Mog House in and out too (the zone can be the same) | `zone`, `previous` |
| `job_change` | The main job changed | `job`, `job_name`, `level`, `previous`, `previous_name`, `previous_level` |
| `subjob_change` | The support job changed (0 and `""` for none) | the same |
| `level_change` | The main job's level changed, the job the same (level sync too) | `job`, `job_name`, `level`, `previous_level` |
| `target_change` | The main target changed (index 0: none) | `index`, `id`, `previous_index`, `previous_id` |
| `main_menu_open`, `main_menu_close` | The main menu (either page) | |
| `action_menu_open`, `action_menu_close` | The action menu (Attack, Magic, ...) | |
| `chat_input_open`, `chat_input_close` | The chat line | |
| `menu_open`, `menu_close` | Any of the game's windows, by name | `name` |

- `job_name` is the three-letter name (`'WAR'`). `login` carries the zone, so the login's own zone-in isn't a
  `zone_change`.
- `menu_open` / `menu_close`: `e.name` is the window's layout name without its `menu    ` start: `menuwind` (the main
  menu), `socialme` (its second page), `playermo` (the action menu), `searchma` (Search), `partywin` (the party list),
  `logwindo` (the chat log), `targetwi` (the Target window), `buff` (status icons), and so on. To find a window's name,
  print them:

```lua
vana.on('menu_open', function (e) print('open', e.name) end)
vana.on('menu_close', function (e) print('closed', e.name) end)
```

  The game builds some windows again when you zone or the party changes, so those close and open again then.

```lua
vana.on('zone_change', function (e)
    print(('Zoned from %d to %d'):format(e.previous, e.zone))
end)

vana.on('job_change', function (e)
    print(('Now %s%d (was %s%d)'):format(e.job_name, e.level, e.previous_name, e.previous_level))
end)
```

## Packets

| Event | `e` | Return `true` |
|-------|-----|---------------|
| `packet_in` | `id`, `size`, `data`, `data_raw`, `data_modified`, `data_modified_raw`, `chunk_size`, `chunk_data`, `chunk_data_raw`, `injected`, `blocked` | The game doesn't get it |
| `packet_out` | the same | The packet isn't sent |

`data` is the packet as a string, its 4-byte header first; `data_modified` is what the game will get: assign a new
string to change it (the same length). `*_raw` are pointers for the FFI. `chunk_*` is the whole buffer the packet came
in. [Packets](10-packets.md) has the layout, examples and `vana.packets`.

## Keyboard and mouse

| Event | Handler gets | Return `true` |
|-------|--------------|---------------|
| `key` | `wparam, lparam, blocked` (not a table) | The game's window doesn't get the key |
| `mouse` | `type, x, y, delta, blocked, modifiers` (not a table) | The game doesn't get the mouse message |
| `key_state` | `e.data` (256 bytes), `e.data_raw`, `e.size` | |
| `key_data` | `e.key`, `e.down`, `e.injected`, `e.blocked` | The game doesn't get the key |

- **`key`**: the game window's key messages: `wparam` the virtual-key code, `lparam` Windows' (bit 31 set for a
  release, bit 30 for a repeat). While the chat line is open, Ashita and Windower addons don't get presses; VanaCore
  addons do.
- **`mouse`**: `type` is the Windows message less `0x200`: 0 move, 1 left down, 2 left up, 4 right down, 5 right up,
  7 middle down, 8 middle up, 10 wheel. `x, y` in VanaCore's interface pixels (`vana.ui.screen()`'s), `delta` the wheel's
  turn (120 a notch), `modifiers` the keys held: 1 Shift, 2 Ctrl, 4 Alt. A press or wheel over an ImGui window is
  ImGui's: you hear it with `blocked` true.
- **`key_state`**: each read of the game's DirectInput keyboard: `e.data` the 256 key states (DirectInput key codes,
  `0x80` set while down). Write through `e.data_raw` with the FFI to make the game see a key.
- **`key_data`**: each buffered DirectInput key event: `e.key` the DIK code.

[Input](09-input.md) has all of it, with `vana.binds` for key binds.

## Controllers

| Event | `e` | Return `true` |
|-------|-----|---------------|
| `xinput_button` | `button`, `state`, `injected`, `blocked` | The game doesn't see the press |
| `dinput_button` | the same | |
| `xinput_state` | `user`, `state` and `state_modified` (pointers to the same XINPUT_STATE) | |

A handler may change `e.button` and `e.state` in place. [Input](09-input.md) has the numbering.
`xinput_state` is the whole state the game is about to read: a handler changes it through either pointer (Ashita addons
write `state_modified`, as in Ashita 4.30).

## Drawing and the scene

| Event | Handler gets | Return `true` |
|-------|--------------|---------------|
| `beginscene`, `endscene` | `back_buffer` (not a table) | |
| `d3d_dp` | `e.primitive_type`, `e.start_vertex`, `e.primitive_count` | The draw is skipped |
| `d3d_dip` | `e.primitive_type`, `e.min_index`, `e.num_vertices`, `e.start_index`, `e.primitive_count` | |
| `d3d_dpup` | `e.primitive_type`, `e.primitive_count`, `e.vertex_stream_zero_data`, `e.vertex_stream_zero_stride` | |
| `d3d_dipup` | `e.primitive_type`, `e.min_vertex_index`, `e.num_vertex_indices`, `e.primitive_count`, `e.index_data`, `e.index_data_format`, `e.vertex_stream_zero_data`, `e.vertex_stream_zero_stride` | |

The game's own scenes and draw calls, as Ashita's addons have them. `beginscene` comes after the game's `BeginScene`
and `endscene` before its `EndScene`; `back_buffer` says whether the scene draws on the back buffer (not a shadow map
or the like). The `d3d_*` events fire for every draw call the game makes: thousands a frame. Pointers are light
userdata for the FFI. Listening for any of them costs every frame; don't unless you need them.

## Between addons and clients

| Event | `e` | When |
|-------|-----|------|
| `plugin_event` | `name`, `data`, `data_raw`, `size` | A plugin or addon raised an event (`vana.plugins.raise`), at the next frame |
| `ipc` | `message` | The addon of your name in another VanaCore instance on this PC sent `vana.ipc.send(text)` |
| `gameui_button` | `element` | The player chose your button in a game menu (you use `vana.gameui.add`'s `fn` instead) |
| `gameui_replaced` | `element` | The player chose a menu item you took over (you use `vana.gameui.replace`'s `fn`) |
| `gameui_trigger` | `trigger`, `menu`, `key`, `item`, `name`, `caught` | The player did something in a game menu you set a trigger on (you use `vana.gameui.on` / `catch`'s `fn` instead) |
| `gameui_window` | `element`, `what` | A game window you watch opened, changed or closed (`what`: `'open'`, `'change'`, `'close'`; you use `vana.gameui.watch`'s `fn` instead) |

[System](12-system.md) covers `vana.plugins` and `vana.ipc`; [The game's interface](07-game-interface.md) covers
`vana.gameui`, which handles the `gameui_*` events for you.

## Order of a frame

Each frame, on the game's thread, VanaCore:

1. Runs any queued `plugin_event`, `gameui_*` and `ipc` events.
2. Compares the game's state and raises the game events (`zone_change`, `menu_open`, ...).
3. For each addon in load order: runs its due tasks, then its `present` handlers.

`command`, `text_*`, `packet_*`, `key`, `mouse` and the rest come as they happen, between frames, on the same thread.
Functions that draw (`vana.imgui.*`) do nothing outside a frame, so draw from `present`, and from a command handler set
a flag that `present` reads.
