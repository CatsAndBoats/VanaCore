# Writing addons for VanaCore

VanaCore runs Lua addons inside FINAL FANTASY XI. An addon is a folder with one Lua file in it. It can draw on the
screen, read the game's data (your character, the party, the target, your inventory), react to what happens in the
game (a zone change, a chat line, a packet, a key), run commands, keep settings, and change parts of the game's own
interface. This series says how.

Everything an addon can use is in one global table, `vana`. There is no `require` for it: it is there when your file
runs. The API is described page by page below, and all of it is listed once more, one line per function, in the
[API reference](15-api-reference.md).

## The series

| Page | What it covers |
|------|----------------|
| [1. Getting started](01-getting-started.md) | Your first addon: the folder, the `addon` table, loading and reloading it, `print`, commands, `boot.txt` |
| [2. Events](02-events.md) | `vana.events.on`: every event VanaCore raises, what each hands you, and how to block one |
| [3. Tasks and timing](03-tasks-and-timing.md) | Running something later or every few seconds: `vana.tasks`, `coroutine.sleep`, `vana.time.clock` |
| [4. Game data](04-game-data.md) | `vana.game`: your character, the player's stats and buffs, the party and alliance, entities, the target, inventory, equipment, recasts |
| [5. Chat and commands](05-chat-and-commands.md) | Writing to the chat log, running commands, handling `/yourcommand`, reading chat lines, Shift-JIS, auto-translate |
| [6. Drawing](06-drawing.md) | Text boxes, rectangles and pictures (`vana.ui`), ImGui windows (`vana.imgui`), icons, fonts, the layout system |
| [7. The game's interface](07-game-interface.md) | `vana.gameui`: hiding the party list or chat, moving the Target window and where it is, a button in the Config menu, taking over a menu item |
| [8. Settings and files](08-settings-and-files.md) | `vana.settings` (per character or shared), `vana.json`, `vana.fs`, `vana.paths`, and VanaCore's own `vanacore.ini` through `vana.config` |
| [9. Input](09-input.md) | Key binds (`vana.binds`), key and mouse events, the DirectInput keyboard, controllers (`vana.gamepad`) |
| [10. Packets](10-packets.md) | Reading, changing, blocking and sending the game's packets |
| [11. Memory and FFI](11-memory-and-ffi.md) | `vana.memory`: reading and writing the game's memory, finding code by pattern, calling the game's functions with LuaJIT's FFI |
| [12. System](12-system.md) | Windows, monitors, the clipboard, sounds, URLs, other game clients on this PC (`vana.clients`, `vana.ipc`), graphics info, plugins |
| [13. Ashita and Windower addons](13-ashita-and-windower.md) | How addons written for Ashita or Windower run on VanaCore, and how to port one |
| [Ashita plugins](../plugins/README.md) | VanaCore's plugin API: what `vana.gameui` and `vana.ui.game_on_top` do, for a C++ plugin |
| [14. Developing and debugging](14-dev-workflow.md) | The edit-reload loop, running Lua in a loaded addon, logs, errors and crashes, tracing, profiling, publishing |
| [15. API reference](15-api-reference.md) | Every `vana.*` function on one page |
| [16. The character, the menus and the game's tables](16-character-menus-and-tables.md) | How the character reads, menu triggers (`vana.gameui.on` / `catch`) and the game tables (`vana.gamedata`) work underneath, and the API Explorer |
| [17. HTTP, WebSockets and promises](17-http-and-websockets.md) | Fetching web pages and APIs, downloads and WebSockets without stopping the game: `vana.http`, `vana.ws`, `vana.promise`, `vana.async`, `await` |
| [18. Widgets](18-widgets.md) | Boxes in the game's 3D space: at a character's nameplate or a point in the world, hidden by walls or not; rings of light and waymarks on the ground, the ground under the pointer, where a name is (`vana.world`) |
| [Examples](examples/README.md) | Complete addons to copy, one per task |

## Three things to know first

**Your addon has its own Lua.** Each addon runs in its own LuaJIT 2.1 state (Lua 5.1, with LuaJIT's `ffi`, `bit` and
`jit` libraries). Nothing you define leaks into another addon, and another addon's crash doesn't touch yours.

**Everything happens on the game's thread, once a frame.** Events, tasks and your drawing all run from the game's
`Present`. Nothing in `vana` needs a lock, and nothing you do blocks the game unless it takes long.

**Say what your addon does.** Put a `README.md` in the addon's folder. The /vana window shows it on the Addons page,
and it is where you tell the player what the addon changes in the game's interface, if anything.

## Where things are

```
<VanaCore>\                      the folder VanaCore.exe is in
    addons\vanacore\<name>\      your addon: <name>.lua and README.md
    addons\vanacore\libs\        libraries every VanaCore addon can require
    addons\ashita\               Ashita addons, run through VanaCore's compatibility layer
    addons\windower\             Windower addons, the same
    settings\addons\<Name>\      what vana.settings saves (shared, and a folder per character)
    scripts\boot.txt             the commands run at every start: /addon load lines go here
    logs\vanacore.log            VanaCore's log: vana.log and every addon error with its traceback
    config\imgui.ini             ImGui's window positions; config\layout.json the Layout page's
    fonts\                       fonts loaded into the game for vana.ui text
```

## Versions

`vana.version` is VanaCore's version as text (`'0.8.47'`). A part of the API that isn't in an older VanaCore is simply
`nil`, so an addon that must load on old and new alike checks before using it:

```lua
if vana.gameui ~= nil then
    vana.gameui.hide('party_list')
end
```

The pages note where a feature needs a particular version.
