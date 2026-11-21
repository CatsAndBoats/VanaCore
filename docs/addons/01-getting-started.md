# 1. Getting started

## The folder

An addon is a folder under `addons\vanacore\` with a Lua file of the same name:

```
addons\vanacore\greeter\
    greeter.lua      the code; VanaCore runs this file when the addon loads
    README.md        what it does, for the player (shown in /vana > Addons)
```

The folder's spelling is the addon's display name (`Greeter` if you spell it so), and it names the folder your settings
go in. Commands use it in any case: `/addon load greeter`.

Other files are yours to lay out as you like. `require('something')` looks in the addon's folder first (`something.lua`
or `something\init.lua`), then in `addons\vanacore\libs\`.

## The smallest addon

```lua
addon.name    = 'greeter'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'Says hello when you log in.'

vana.on('login', function (e)
    print('Welcome back, ' .. e.name .. '!')
end)
```

Save it as `addons\vanacore\greeter\greeter.lua`, then in the game type:

```
/addon load greeter
```

The chat log says `(VanaCore Addon) Loaded: greeter 1.0 by You`. Log in, and it greets you.

## The `addon` table

`addon` is a table VanaCore gives your file before it runs. Two fields are filled in:

| Field | Value |
|-------|-------|
| `addon.name` | The addon's name, lower case |
| `addon.path` | The addon's folder, without a trailing backslash |

Set the rest yourself. `/addon info <name>` and the /vana window's Addons page show them:

| Field | Shown as |
|-------|----------|
| `addon.author` | By whom |
| `addon.version` | Its version |
| `addon.desc` | One line on what it does |
| `addon.link` | A web address; `/addon link <name>` opens it in the browser |

Your file is run once, at load. Code at the top level runs then; everything else happens through events.

## Events, in one paragraph

An addon does things when VanaCore raises an event. `vana.on(name, fn)` (the same as `vana.events.on`) runs `fn` each
time `name` happens. The ones every addon uses:

```lua
vana.on('load', function () end)       -- after your file has run
vana.on('unload', function () end)     -- the addon is going: /addon unload, /addon reload, the game closing
vana.on('present', function () end)    -- once a frame: draw your ImGui windows here
vana.on('command', function (e) end)   -- every line typed into the chat that starts with /
```

[Events](02-events.md) lists all of them.

## Printing

`print(...)` writes a line to the game's chat log (and to VanaCore's log). `vana.log(...)` writes to VanaCore's log
only, `logs\vanacore.log`, as `<addon>: <text>`: for what the player needn't see.

```lua
print('Loaded', addon.name, addon.version)   -- "Loaded greeter 1.0" in the chat
vana.log('party has', #rows, 'rows')          -- "greeter: party has 3 rows" in the log
```

A coloured line is `vana.chat.write(text, mode)`: see [Chat and commands](05-chat-and-commands.md).

## A command of your own

Every line the player types that starts with `/` comes to your `command` handler before the game sees it. Return
`true` to say you took it, so the game doesn't try to run it (and complain):

```lua
vana.on('command', function (e)
    local text = e.command:match('^/greet%s+(.+)$')
    if text ~= nil then
        print('Hello, ' .. text)
        return true
    end
end)
```

`e.command` is the whole line as typed. Commands are matched by you, so pick a first word no other addon uses.

## Loading, unloading, reloading

| In the chat | Does |
|-------------|------|
| `/addon load <name>` | Loads it from `addons\vanacore`, else `addons\ashita`, else `addons\windower` (several names at once work) |
| `/addon load <name> vanacore` | From that folder only (`ashita`, `windower`) |
| `/addon unload <name>` | Runs its `unload` handlers and closes its Lua |
| `/addon reload <name>` | Unload, then load again from the folder it came from: your edit-test loop |
| `/addon kill <name>` | Unloads without running `unload` (for an addon stuck in a bad state) |
| `/addon list` | What's loaded |
| `/addon info <name>` | Its file, version, author, description, link, memory and state |
| `/addon exec <name> <lua>` | Runs Lua inside the addon; what it prints goes to the chat log |
| `/addon unloadall` | Unloads every Ashita and Windower addon (VanaCore's own stay) |
| `/addon silent [0\|1]` | No chat line as addons load and unload |

`//addon ...` works the same. The /vana window's **Addons** page has a switch per addon that does the same as load and
unload.

Only one addon of a name is loaded at a time. When an addon unloads, VanaCore removes everything it made: its text
boxes and pictures, its ImGui windows, what it changed in the game's interface, and saves its settings. You don't have
to clean up yourself, though an `unload` handler may.

## Loading at every start: `scripts\boot.txt`

`scripts\boot.txt` next to `VanaCore.exe` runs when the game starts. Each line is a command as you'd type it; `#` starts
a comment, and `/wait <seconds>` delays the lines after it:

```
/addon load greeter
/addon load tparty
```

A typed `/addon load` or `/addon unload`, and the /vana window's switches, keep `boot.txt` in step: loading an addon
adds a `//addon load <name> <kind>` line unless one loads it already, and unloading takes the name out. So loading an
addon once means it loads from then on.

By default (`[addons] delay.login=1` in `vanacore.ini`) `boot.txt`'s `/addon` lines don't run at the title screen but
once a character is in the world, so your `load` handler can read the character straight away. `/addon load` typed by
hand isn't held.

## What's in your Lua

- **Lua 5.1 with LuaJIT 2.1**: `string`, `table`, `math`, `os`, `io`, `coroutine`, `debug`, plus `ffi`, `bit` and
  `jit`. `io.open` works on any path (paths in the ANSI code page). `vana.paths` has the folders you'll want
  ([Settings and files](08-settings-and-files.md)).
- **`vana`**: VanaCore's API, the subject of this series.
- **`addon`**: your addon's table, above.
- **`print`**: to the chat log.
- **`coroutine.sleep(seconds)`** and **`coroutine.sleepf(frames)`**, inside a task ([Tasks and timing](03-tasks-and-timing.md)).
- **Libraries**: `require('ashita.json')`, `require('ashita.common')` and the rest of `addons\vanacore\libs\`; LuaSocket's C
  half is built in (`require('socket.core')`).
- **Text is bytes.** Your file is UTF-8; the game's text (names, chat) is Shift-JIS. `vana.encoding` converts
  ([Chat and commands](05-chat-and-commands.md)).

## When things go wrong

An error in a handler is reported in the chat log and in `logs\vanacore.log` with its traceback, and the addon keeps
running: the rest of its handlers for that event still run. An error that repeats on every frame for 5 seconds unloads
the addon, with the reason in the chat. [Developing and debugging](14-dev-workflow.md) has the rest: tracing an addon,
running Lua in it live, profiling, and what a crash (a bad FFI pointer) does.

## Next

- [Events](02-events.md): everything that can happen, and what each event hands you.
- [Examples](examples/README.md): complete addons to copy.
