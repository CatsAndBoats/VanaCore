# 14. Developing and debugging

## The loop

1. Edit `addons\vanacore\<name>\<name>.lua`.
2. In the game: `/addon reload <name>`.
3. Look at the chat log and `logs\vanacore.log`.

No rebuild, no restart. `reload` runs your `unload` handlers, closes the Lua state, and loads the file again, so every
`local` starts fresh. Settings stores are saved before and read after.

Bind the reload to a key while you work:

```
/bind ^f12 /addon reload myaddon
```

## Running Lua in a loaded addon

Two ways to poke at a running addon's state without editing the file:

```
/addon exec <name> <lua>       runs the Lua inside the addon; what it prints goes to the chat log
/vana lua <name> <code>        the same, and says what the code returns (tried as an expression first)
```

```
/vana lua myaddon #rows
/vana lua myaddon config.data.scale
/addon exec myaddon print(vana.game.self())
```

`/vana lua` without an addon name lists the loaded ones. Globals the code sets stay in the addon's state, so you can
set a flag and watch what changes.

## The MCP server

With `[mcp] enabled=1` in `vanacore.ini` (the /vana window's **Client > AI MCP**), VanaCore runs an MCP server inside
the game at `http://127.0.0.1:47800/mcp`, so an AI tool on this machine (Claude Code: `claude mcp add --transport http
vanacore http://127.0.0.1:47800/mcp`) works in the live game: it reads the character, party and entities, dumps
packets and the chat log, runs commands and Lua, reads and writes memory, disassembles, scans for patterns and values,
sets hardware watchpoints, and takes screenshots.

Its `lua` tool runs in a state of its own (the `mcp` addon), with the whole of `vana` and the FFI, so you can try an API
call or a memory read before writing it into your addon. To run code inside *your* addon from there, use its
`run_command` tool with `/addon exec <name> <lua>`. The VanaCore README's MCP section lists every tool.

## Logs

`logs\vanacore.log` next to `VanaCore.exe` has, for addons:

- `Addons: loading ...`, `loaded ...`, `unloaded ...`, each with the kind.
- Every error, with its traceback: `Addons: <name>: <event>: <message>` and the stack.
- `Addons: <name> listens for the '<event>' event, which VanaCore doesn't raise yet`: a misspelled event name.
- What you write with `vana.log(...)`, as `<name>: ...`.
- `ImGui: check failed ...` and `ImGui: fixed at the frame's end ...`: ImGui misuse, logged once each.

Logging is on by default (`[logging] enabled`). `print` goes to the chat log and the log both.

## Errors

An error in a handler or task is reported once in the chat log (red) and in the log with its traceback, and the addon
keeps running. The same error again isn't repeated in the chat. The rest of the addon's handlers for that event still
run, and so do other addons'.

**Five seconds of errors unload the addon.** An error that repeats on every frame (a `nil` in a `present` handler) for
5 seconds in a row means the addon is broken, and VanaCore unloads it with the reason in the chat
(`5 seconds of errors, the last: ...`). Bursts shorter than that are left alone: some addons fail for a moment as they
start.

**A crash freezes the addon.** A hardware exception in your code (an access violation from a bad FFI pointer) is caught
at the call's boundary and reported (`crashed: access violation reading 0x00000010 at FFXiMain.dll+0x1234 from
VanaCore.Core.dll+0x5678 (myaddon.lua:42 in update)`: the faulting code, what called it, and your Lua line). The
addon's Lua state is never entered again, its `unload` handlers don't run, and its memory stays allocated until the
game ends. What it made through `vana` is removed as usual. The game goes on; fix the code and load it again.

## The addon window: `/vana stats`

`/vana stats` opens VanaCore's statistics window. Its addons section lists each loaded addon with the time its events
take per frame, its Lua memory, how many errors it has reported, and which kinds of event it listens for (packets,
chat text, mouse, keys). Pick one for its pane:

- **Errors**: the distinct errors it has reported (the last 20 kinds), each with when, which event, the message and
  traceback, and how often.
- **Start profiling** / **Stop and show**: a sampling profile of the addon's Lua (LuaJIT's `jit.profile`, every
  millisecond): the top lines and functions by sample, to find what's slow. One addon at a time.

## Tracing an addon

```
/vana trace <addon>        logs what the addon does: the events it gets, the commands and packets it sees and blocks,
                           the chat lines and commands it queues, and a line every 10 seconds summing up
/vana trace <addon> 30     the same for 30 seconds (1 to 600), then a report of what it costs: see below
/vana trace off            (ends a timed trace early, its report written with what it has)
/vana trace list
```

`[logging] trace.addons=partyfinder,xiui` in `vanacore.ini` does the same from the start (the command writes it).
Nothing costs anything while no addon is traced. It's the tool for "my handler never runs": the trace shows whether the
event reached the addon, and whether another addon took it first.

### Is it making the game lag? A timed trace

`/vana trace <addon> <seconds>` traces one addon for that long (not written to `vanacore.ini`) and, at the end, writes
`logs\trace-<addon>-<date>-<time>.txt`, whether or not logging is on, and says the gist in the chat: its Lua's time a
frame and share of the frame, the game's frame rate and hitches, how many of those it caused, and where the report is.
The report has:

- **What it found**: how heavy it is (light, moderate, heavy: about how many fps it costs), whether it caused the
  hitches, which event's handlers take most of its time, and the likely why from its profile and memory: Lua the JIT
  couldn't compile, garbage made on every call, time in C (the API it calls), memory that keeps growing, ImGui windows
  drawing tens of thousands of vertices.
- **Its calls by event**: calls, calls a second, total, a frame, average and slowest, and how many took over 1 and 5 ms.
- **Its slowest calls**: when, in which event, and its Lua memory over each (a drop: its garbage collector ran).
- **The game's frames**: the frame rate, the average, median, 95% and 99% (the 1% low) frame times, the average frame's
  parts (the game's Direct3D, its sleeps, VanaCore, the frame rate limit, Present, the rest), and the 15 slowest frames,
  each with the addon's time in it and its slowest call.
- **Every addon's Lua**, for comparison: calls, time a frame and share, slowest call, most in one frame.
- **Its Lua profile** (LuaJIT's `jit.profile`, as /vana stats' Start profiling): the share compiled, interpreted, in C,
  in garbage collection, and the top lines and functions. One addon is profiled at a time: if /vana stats is profiling
  another, the report says so and has no profile.
- **What it did** (packets given, blocked and added, chat lines, commands, its ImGui windows) and **the hardware** at the
  end (CPU and GPU busy, clocks, memory, a GPU held down by power or heat).

While it runs, every addon's calls and the frames' parts are timed, which costs a little.

## Checking the game's state

- `/vana debug` opens the Debug window (frame recording, debug views).
- `/gamepad test` opens the Gamepad Test window (Client > Gamepads > Gamepad Test): the buttons pressed on each controller, its
  sticks, triggers and stats, read from Windows, so it shows a controller the game isn't using as well. Its Record button
  (`/gamepad record`) writes `logs\gamepad-<time>.log`: each button, stick and key press with the time Windows had it, the time
  the game read it, the first packet the game sent after it, slow frames, gaps in the game's input reads and in the server's
  packets, and a summary that says which stage the lag is in. The keys the game gets are in it.
- The MCP server's `game_state`, `entities`, `packets` and `chat_log` tools show what your addon reads.
- `vana.log(vana.json.encode(t))` dumps a table to the log; `/vana lua <addon> <expression>` shows one in the chat.

## Testing outside the game

`tools\luatest\run.cmd` in the VanaCore repository runs Lua tests with the same LuaJIT VanaCore builds in, on the real
`vana.lua` and `settings.lua` with stand-ins for the native functions. `settings_test.lua` is the model: pure Lua that
doesn't touch the game can be tested that way. Anything that reads the game runs in the game.

## Writing for others

- **`README.md` in the addon's folder.** What it does, its commands, and what it changes in the game's interface.
  `/vana help <addon>` and the Addons page show it.
- **`addon.desc`, `addon.version`, `addon.author`, `addon.link`.** Shown in `/addon info` and the Addons list; the
  link opens with `/addon link <name>`.
- **Check for what you need.** `vana.gameui ~= nil`, `vana.binds ~= nil`: an addon that says "needs a newer VanaCore"
  beats one that errors. `vana.version` is text; compare parts, not strings.
- **Don't leave things behind.** Binds you added, memory you patched, tasks that loop: undo them in `unload`. Boxes,
  ImGui windows, game interface changes and settings are VanaCore's to clean up.
- **Don't act on the player's behalf without being asked.** Sending commands or packets automatically is the server's
  business to allow. A command that does one thing the player asked for is an addon; one that plays is a bot.
- **Keep per-frame work small.** `present` runs 60 times a second. Read `vana.game.party()` when something changed,
  not every frame, or every few frames; compare before setting a text box's text; don't walk 2,304 entities a frame.

## For VanaCore developers

The native API is `src/core/lua_api.cpp` (every `vana.*` function the DLL provides), `src/core/lua/vana.lua` (events,
tasks, `print`, the `vana.gameui` wrappers) and `src/core/lua/settings.lua` (`vana.settings`, `vana.json`), both Lua
files built into `VanaCore.Core.dll` as resources and run in each addon before its file. `src/core/addons.cpp` hosts
the addons: loading, the events, errors and crashes, `boot.txt`. `src/core/gameevents.cpp` works out the game events;
`src/core/lua_imgui.cpp` binds ImGui. The compatibility layers are `src/core/lua/compat/ashita.lua` and `windower.lua`.

A new `vana.*` function goes in `lua_api.cpp` (or `vana.lua` when it's pure Lua), with a line in the
[API reference](15-api-reference.md) and the page it belongs to. A new event goes in `addons.cpp`'s `kEvents` (so a
listener isn't logged as unraised) and in [Events](02-events.md). Lua in `src/core/lua` takes effect after a rebuild;
addons under `addons/vanacore` are loose files.
