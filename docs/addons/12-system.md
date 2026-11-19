# 12. System

The smaller tables: the game window and monitors, the clipboard, sounds and URLs, other VanaCore clients on this PC,
graphics information, plugins and other addons, and what's left.

## `vana.system`

```lua
vana.system.open_url(url)         -- opens http:// or https:// in the default browser; true if it did
vana.system.play_sound(path)      -- plays a .wav without waiting; true if it started
vana.system.clipboard()           -- the clipboard's text, Shift-JIS ('' when none)
vana.system.set_clipboard(text)   -- Shift-JIS text onto the clipboard; true if it did
vana.system.focused()             -- true while the game isn't in the background
```

```lua
vana.system.play_sound(addon.path .. '\\sounds\\ding.wav')
```

## `vana.window`

```lua
local w = vana.window.info()   -- nil before the game has a window
```

| Field | What |
|-------|------|
| `x`, `y`, `width`, `height` | The window on the screen (its frame) |
| `client_width`, `client_height` | The picture's size |
| `monitor` | Which monitor it's on, 1 the leftmost |
| `topmost`, `minimized`, `maximized`, `covering` | Its state (`covering`: full screen) |
| `character` | The character the window's saved position is for (`''` before login) |
| `monitors` | `[1..n]` left to right, each `{ x, y, width, height, main }` |

```lua
vana.window.monitor(n)                    -- moves the game to monitor n (full screen: covers it); false if it can't
vana.window.block_mouse(on[, ms])         -- no mouse button reaches the game while on, and `ms` (400) after; returns whether blocked
vana.window.block_mouse()                 -- just asks
```

For the size VanaCore's interface is drawn in, `vana.ui.screen()` ([Drawing](06-drawing.md)).

## Other game clients on this PC: `vana.clients` and `vana.ipc`

A player running several characters has several VanaCore instances. Two things connect them.

### Who's running

```lua
for _, c in ipairs(vana.clients.list()) do
    -- c.pid, c.name (the character; '' before it's in the world), c.self (this game), c.main (the Main)
end
vana.clients.set_main(name)   -- that character is the Main ([characters] main in vanacore.ini); nil or '' for none
vana.clients.copy()           -- the Main's settings to every other game running; how many, or nil and why
vana.clients.front(pid)       -- brings that game's window in front (from this game while it's in front); true if it is now
```

### Talking between them

```lua
vana.ipc.send(text)   -- to the addon of *your addon's name* in every other instance; false if too long or no shared memory

vana.on('ipc', function (e)
    -- e.message: what another instance's copy of this addon sent
end)
```

A message goes from an addon to the same-named addon in every other VanaCore running in this Windows session, through
shared memory, read once a frame. At most 8,000 bytes; an instance more than 128 messages behind loses the oldest.
This is what Windower's `send_ipc_message` and `ipc message` map onto, so multibox addons (FastFollow, send) work.

```lua
-- tell the others to follow me
vana.on('command', function (e)
    if e.command == '/followme' then
        vana.ipc.send('follow ' .. select(1, vana.game.character()))
        return true
    end
end)
vana.on('ipc', function (e)
    local who = e.message:match('^follow (%S+)$')
    if who then vana.chat.run('/follow ' .. who) end
end)
```

## Graphics: `vana.graphics`

```lua
local g = vana.graphics.info()
```

| Field | What |
|-------|------|
| `renderer` | What `vanacore.ini` asks for: `'d3d8'`, `'d3d9'`, `'d3d11'`, `'d3d12'` |
| `running` | The one the game draws with (`d3d8` when the one asked for didn't come up) |
| `active` | True when the game draws through VanaCore's own Direct3D 8 (on 12 or 11) |
| `effects` | True when VanaCore's scene effects can run |
| `gpu`, `driver`, `driver_version`, `driver_date`, `driver_installed`, `driver_days` | The graphics card and its driver (`''` with the game's own Direct3D 8) |
| `dlss`, `framegen`, `fsr`, `upscaler`, `dlss_card`, `dlss5`, `fsr_card`, `*_why`, `dlss_sized`, `stopped` | The upscalers' state |
| `scene_width`, `scene_height` | The 3D scene's size |
| `monitor_width`, `monitor_height`, `work_width`, `work_height` | The screen the window is on, and it less the taskbar |
| `ui_fit_width`, `ui_fit_height` | The menus' size `[ffxi] resolution.ui=window` comes to |
| `d3d12_unsupported` | The card can't do Direct3D 12 |

```lua
vana.graphics.reload()      -- applies vanacore.ini's live graphics settings now (after vana.config.set)
vana.graphics.retry()       -- DLSS or FSR that stopped tried again
vana.graphics.recompile()   -- the scene effects' shaders compiled again; false without VanaCore's renderer
vana.graphics.damage_test(kind) -- numbers VanaCore makes over the player to show how damage numbers are placed ('hit', 'round', 'barrage'); false for another kind
```

```lua
local hw = vana.graphics.hardware()  -- what the graphics card and the CPU are doing now
```

The call holds the readings for 5 s (polled once a second, through VanaCore's helper), so read it each half second while
a window shows them and they stop once it's closed. `gpu` is true when NVIDIA's NVML gave a reading (an NVIDIA card);
`gpu_why` says why not.

| Field | What |
|-------|------|
| `gpu`, `gpu_why` | A reading is there, or why not |
| `gpu_busy`, `gpu_temp`, `gpu_mhz`, `gpu_max_mhz` | % of the time something ran on the card, its temperature (C) and clock |
| `gpu_watts`, `gpu_limit_watts` | Its power and its limit |
| `vram_used_mb`, `vram_total_mb` | Its memory in use (every program's) and in all |
| `vram_game_mb`, `vram_budget_mb` | This game's own video memory and Windows' budget for it (DXGI: any card; 0 unknown) |
| `throttled`, `throttle_text` | The clocks are held down by a limit (power, heat), and the reasons in words |
| `cpu`, `cpu_mhz`, `cpu_max_mhz`, `cpu_limit_mhz` | The CPU's clock now while it runs, boost included (Windows 8 on; the busy cores count the most), its rated (base) clock, and what Windows lets it run at (below the rated clock: held back) |
| `cpu_busy`, `cpu_game` | % of every core's time something ran, and this game ran, over the last second (Task Manager's: 100 is all the cores); nil for the first second |
| `ram_used_mb`, `ram_total_mb`, `ram_game_mb` | The PC's memory in use (every program's) and in all, and this game's private memory (Task Manager's Commit size) |
| `power`, `ac`, `battery`, `power_plan`, `power_mode` | Plugged in, % charged (-1 none), Windows' power plan and mode |

The rest of the table (`dlss_indicator`, `dlss_crash_test`, `grass_*`) serves the /vana window and isn't for addons.

An addon that draws its own effects or depends on the renderer checks `info().active` and `info().running`.

## Plugins and addons

```lua
vana.plugins.loaded()          -- { { name, source = 'ashita' | 'windower', memory }, ... }
vana.plugins.raise(name, data) -- a plugin event: plugins' HandleEvent, and every addon's 'plugin_event' at the next frame

vana.addons.loaded()           -- { { name, source, version, author, desc, link, file, state, memory }, ... } in load order
```

`state` is Ashita's numbering: 0 error (it crashed), 1 running, 3 loading, 4 unloading, 5 reloading. `memory` is the
bytes its Lua state holds. `source` is the folder it came from: `'vanacore'`, `'ashita'` or `'windower'`.

A plugin's `memory` is the bytes it has allocated: its C runtime and Windows heap blocks and the address space it reserved
with VirtualAlloc, counted from as its DLL loads (not what other DLLs allocate for it, such as Direct3D's textures). It's
nil when VanaCore isn't counting it. /vana stats shows it beside each plugin, and a click on one shows all its calls.

```lua
local function loaded(name)
    for _, a in ipairs(vana.addons.loaded()) do
        if a.name:lower() == name then return true end
    end
    return false
end
```

`plugin_event` is how plugins and addons signal each other: `vana.plugins.raise('myaddon.refresh', payload)` reaches
every addon's `plugin_event` handler (`e.name`, `e.data`, `e.data_raw`, `e.size`) next frame, and every Ashita plugin's
`HandleEvent`.

## The Ashita layer's extras: `vana.ashita`

Built for the Ashita compatibility layer, usable by any addon:

```lua
vana.ashita.string(table, index[, language])   -- the resource manager's GetString: the text at that index, or nil
vana.ashita.string(table, text[, language])    -- the index of that text, or -1
vana.ashita.parse_auto_translate(text, brackets)   -- auto-translate phrases spelled out
vana.ashita.regex.search / match / replace / split  -- ECMAScript regular expressions ([Chat and commands](05-chat-and-commands.md))
vana.ashita.execute(file[, parameters])        -- opens the file as Windows would (an editor for a text file); true if it did
vana.ashita.console(show)                      -- the console window (with [client] attached=1) shown or hidden
vana.ashita.gui_visible([visible])             -- whether ImGui's windows are drawn; the same as imgui.GetVisible/SetVisible
vana.ashita.object(name[, interface])          -- an Ashita manager object for the FFI ('fonts', 'primitives', 'resources', 'inventory', 'device'); nil if none
vana.ashita.win32(name, value)                 -- SetFocus(window), SetForegroundWindow(window), GetSystemMetrics(index)
vana.ashita.interface, vana.ashita.libs        -- [addons] ashita.interface and ashita.libs, as numbers (4.16 or 4.30)
```

`language` for `string` is 0 the default, 1 Japanese, 2 English. The resource tables are the game's string DATs
(`'spells.names'`, `'items.names'`, and so on, as Ashita's resource manager names them).

## The rest

```lua
vana.version        -- VanaCore's version as text ('0.8.47')
vana.log(...)       -- a line in VanaCore's log, as "<addon>: ..."
vana.time.clock()   -- seconds, to the microsecond ([Tasks and timing](03-tasks-and-timing.md))
vana.encoding.to_shift_jis(text), vana.encoding.from_shift_jis(text)   -- ([Chat and commands](05-chat-and-commands.md))
vana.mcp.info()     -- the MCP server's state: { running, failed, port, client, idle, sessions }
```

A few functions exist for the /vana window and the HUD Manager rather than addons, and are listed in the
[API reference](15-api-reference.md) for completeness: `vana.chat.preview`, `vana.camera.preview`, `vana.layout.outline`,
`vana.layout.grid`, `vana.layout.up`, `vana.layout.clicked`, `vana.layout.select`, `vana.layout.selected`,
`vana.layout.box`, `vana.layout.box_clicked`.

## The free camera: `vana.camera`

`/freecam` detaches the camera from the character and flies it about (W A S D, the mouse). An addon can turn it on and off,
read where it is and put it somewhere, which is all a camera path needs:

```lua
vana.camera.free(true)                      -- on; true, or false and why (no character in the world)
local cs = vana.camera.state()              -- { on, have, hold, float, lag, speed }
if cs.have then                             -- the pose is taken from the game's view a frame after it comes on
    local x, y, z, yaw, tilt = vana.camera.pose()
    vana.camera.hold(true)                  -- the keys and mouse stop moving it
    vana.camera.set_pose(x + 5, y, z, yaw, tilt)   -- each frame, from a present handler
end
```

Yaw and tilt are radians, yaw about the world's up from the world's x axis toward its z, tilt up from level (limited to 89
degrees). `hold` is dropped when the free camera goes off, by itself on a zone. The `freecam` addon (opened by `/freecam`) is a window and a
keyframe sequencer over these calls, and a worked example.

Its **Weather** (`/freecam weather <name|0-19|off>`) shows any weather on this screen only, for photos, with `vana.packets`
alone (`addons\vanacore\freecam\weather.lua`). It hands the game the server's weather packet, incoming 0x057, as if received:

```lua
-- after the 4-byte header: when the weather started (Earth seconds since 1009810800), the weather, its fade (Vana'diel minutes)
local start = vana.game.world().server_time - 1009810800
vana.packets.inject(true, 0x057, '\0\0\0\0' .. u32(start) .. u16(6) .. u16(1))   -- 6: rain, 1: a 2.4 s fade
```

The weathers are the server's order: 0 clear, 1 sunny, 2 cloudy, 3 fog, then two of each element, the light one first: 4
and 5 fire (hot spell, heat wave), 6 and 7 water (rain, squall), 8 and 9 earth (dust storm, sand storm), 10 and 11 wind
(wind, gales), 12 and 13 ice (snow, blizzards), 14 and 15 thunder (thunder, thunderstorms), 16 and 17 light (auroras, stellar
glare), 18 and 19 dark (gloom, darkness). The game fades into it over the last field's Vana'diel minutes (2.4 real
seconds each, 0 at once; the server sends 4 to 28), whatever the start time; the picker sends 1. While one is held, its
`packet_in` blocks the server's own 0x057s (not `e.injected` ones), keeping the last, and the zone's weather is injected
again when it's turned off; a zone change (0x00B, 0x00A) just ends it. Nothing
goes to the server. `vana.game.world().weather` is the weather the game was given, an injected one too. A zone's files have the
look only of the weathers it really gets: the picker lists those (`vana.game.zone_weathers`, from the zone's model DAT), and the
typed command can still show any.

Its **Time** (`/freecam time <hour|hh:mm|noon|dusk|...|off>`, `addons\vanacore\freecam\daytime.lua`) holds the Vana'diel day at
any hour on this screen only, by the game's own calls, as a cutscene does. While its game timer is on, the game works its
Vana'diel date and time out of the server's time every frame; a cutscene turns the timer off, sets the time, and turns it on
again at its end, and while it's off the time stands still. The calls are found by pattern:

```lua
local ffi = require('ffi')
local timer = vana.memory.find('FFXiMain.dll', 0, 'C6 05 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E9 ?? ?? ?? ?? C3 90 90 90 90 C6 05 ?? ?? ?? ?? 00 C3')
local set = vana.memory.find('FFXiMain.dll', 0, '8B 44 24 04 B9 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? C3 8D 4C 24 04 E8')
local enable = ffi.cast('void (__cdecl *)(void)', timer)       -- EnableGameTimer: the server's hour back, the weather refreshed
local disable = ffi.cast('void (__cdecl *)(void)', timer + 32) -- DisableGameTimer
local set_time = ffi.cast('void (__cdecl *)(int32_t)', set)    -- SetCurrentDateTime
local clock = ffi.cast('volatile int32_t *', vana.memory.read_uint32(set + 5)) -- 60 to a Vana'diel second, 5,184,000 a day

disable()
set_time(clock[0] - clock[0] % 5184000 + 18 * 216000) -- today at 18:00
```

Nothing goes to the server. Never move the game's other clock instead (the server's time, which it keeps in step with each
packet's): the game takes the jump for the server gone quiet and drops the connection.

Its **Animation** (View & Effects; `addons\vanacore\freecam\anim.lua`) plays any of the race's animations on your
character, on this screen only, and pauses and moves them by the frame, with the game's own calls and memory:

- An entity's actor is `[entity + 0xA0]`; its first model part `[actor + 0x6B8]` has seven motion layers from `part + 0x24`
  (0x14 each, the first track at `+4`). A character's clip is three parts on layers 0, 1 and 2 (lower body, upper body,
  waist), named by three letters and the layer's digit (`idl0`, `idl1`, `idl2`).
- A track: `+4` the next, `+0x24` its frame (a float), `+0x28` its speed, `+0x2C` loops left (0: for ever), `+0x30` the
  clip's name, `+0x34` a handle to the clip (whose `+0x34` is its frame count, `+0x36` its rate). A pause is the speed set
  to 0; a frame is written.
- `XiSkeletonActor::SetSystemMotion` queues a clip by its three letters on every layer the actor's files have a part for.
  A file is added to the actor's list (`actor + 0x7E4`) by its file id, as the game adds an emote's or a weapon skill's.
  The calls are found by pattern (the signatures are in `anim.lua`).
- The game queues its own motion again only when the one it wants changes or the queue ends, so a looping or paused clip
  holds while the character stands; 0 written to `actor + 0x7D8` (the motion it last queued) brings its idle back.

The list of animations by race and category (`animdata.lua`) is generated by `tools\gen_freecam_anims.py` from XI Model
Viewer's list and the game's file table.
