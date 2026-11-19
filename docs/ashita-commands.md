# Ashita commands vs VanaCore

Where each list comes from:

- **Ashita column:** the strings in Ashita's `ChatManager::CommandHandler` block in `Ashita.dll`
  ("Ashita Beta Compiled On: Dec 14 2023"), and the slash strings in each
  `plugins\*.dll`. What each command does is Ashita's documented behaviour, from memory: check it against a real Ashita
  before relying on a detail.
- **VanaCore column:** read from `src/core/addons.cpp` (`vanacore_command`, `addon_command`), `binds.cpp`,
  `aliases.cpp`, `plugins.cpp` and `chat.cpp`, as of the 2026-10-09 commit. Not run in the game.

Status: **yes** = works as in Ashita, **partial** = exists but differs, **MISSING** = VanaCore doesn't know it.

## Ashita's built-in commands (Ashita.dll)

| Command | What Ashita does | VanaCore |
|---|---|---|
| `/raw <command>` | Runs `<command>` as it is, past every plugin and addon. **Shorthand sends every `/ma`, `/ws`, `/ja`, `/item`, `/ra`, `/attack`, `/target` this way.** | **built** (`chat.cpp raw_command`) |
| `/alias` (`add`, `del`, `clear`, `list`) | Command aliases | yes (`aliases.cpp`, 2026-10-09) |
| `/bind`, `/unbind` | Key binds; the command is a script string: split at `;`, `/wait` between parts delays the rest (Ashita runs it on its command thread pool) | yes (`binds.cpp`; also `/unbindall`; `;` and `/wait` as Ashita, 2026-10-11) |
| `/exec <name> [args...]` | Runs `scripts\<name>.txt`, `%1%` style arguments substituted | **built**: `%1%` style arguments (`/exec name a b`) |
| `/include <name>` | Script directive: pulls in another script | **built** (scripts only, up to 8 deep) |
| `/execstr <string> [args...]` | Runs a string as if it were a script, its commands split at `;` | **built** (split at `;`; quote it to give arguments: `/execstr "/echo %1%" hi`) |
| `/wait [seconds]`, `/sleep [seconds]`, `/pause [seconds]` | One handler: `Sleep(seconds * 1000)` on the running thread, 1 second with no number | **built**: delay a script's or bind's later lines, 1 second with no number; typed alone they do nothing |
| `/load <plugin>`, `/unload <plugin>`, `/unloadall`, `/plugins`, `/list` | Plugin management | **partial**: VanaCore has its own `/plugin load|unload|list`; Ashita's names are MISSING |
| `/about`, `/ashita`, `/aver`, `/aversion`, `/ashitaversion` | Version and credits | **MISSING** |
| `/copy`, `/paste` | Chat line to and from the clipboard | **built**: `/copy` puts the typed line on the clipboard, `/paste` inserts it at the caret (line must be open) |
| `/exit`, `/terminate` | Close the game | **built**: `/exit` runs the game's `/shutdown`, `/terminate` ends the process at once |
| `/controller`, `/gamepad`, `/keyboard`, `/mouse` | Input device options | **MISSING** |
| `/ambient` | The ambient light (`IProperties::SetD3DAmbientEnabled/Color`) | **MISSING** as a command; the effect is `[graphics.effects] ambient_force` (/vana > Graphics > Lighting > Force ambient light, 2026-11-07) |
| `/fillmode` | The Direct3D fill mode (`IProperties::SetD3DFillMode`: a D3DFILLMODE, 1 point, 2 wireframe, 3 solid) | **built** (2026-10-23): `/fillmode solid`, `wireframe` or `point` (or 3, 2, 1) on the world's draws; the interface and VanaCore's own windows stay solid; alone, says which is on. `GetD3DFillMode` and `SetD3DFillMode` read and set the same. Needs VanaCore's renderer. What Ashita's takes as arguments isn't known: see 3 below |

## Ashita's Addons plugin (Addons.dll)

VanaCore emulates it (`/addon`):

| Command | VanaCore |
|---|---|
| `/addon load <name>`, `/addon unload <name>`, `/addon reload <name>`, `/addon list` | yes |
| `/addon unloadall` | **MISSING** |

## Bundled plugins' commands

These are real Ashita plugin DLLs that VanaCore loads, so their commands reach them through the plugin command event.
The ones that matter most depend on `/raw` working, which is why Shorthand's output broke.

| Plugin | Commands |
|---|---|
| Shorthand | `/shorthand` `/shh`, and it rewrites `/ma` `/magic` `/ws` `/weaponskill` `/ja` `/jobability` `/item` `/pet` `/ra` `/range` `/shoot` `/throw` `/attack` `/ta` `/target` into `/raw ...` |
| Multisend | `/multisend` `/ms` `/msa` `/mso` `/msp` `/mss` `/mst` `/msg` |
| FindAll | `/findall` `/fa` |
| LegacyAC | `/legacyac` `/la` `/sl` `/lockstyle` |
| Minimap | `/minimap` `/mm` |
| Uberwarp | `/uberwarp` `/uw` |
| Thirdparty | `/con` `/console` `/release` `/releasekeys` `/sendkey` |
| cexislots | `/cexislots` `/extra` `/recast` (also `/ma` `/ja` forms) |
| cexidats | `/cexidats` |
| Nameplate | `/nameplate` |
| toon | `/toon` |
| Screenshot | `/screenshot` (VanaCore also has its own `//screenshot`; its `/ss` went on 2026-11-04) |
| Sequencer | (LuAshitacast's) |

## VanaCore's own commands (not Ashita's)

`/vana ...` (window, stats, graph, logs, lua, telemetry, abtest, debug), `/plugin`, `/keybinds apply`, `/abtest`,
`//screenshot`, `/freelook [on|off]` (the camera turns without turning the character as it walks),
`/freecam [on|off]` (the camera detached from the character and flown with W A S D and the mouse while it stands still),
`/freecam [window|key|play|stop|rewind|seq]` (the free camera's window and keyframe sequencer, which open with it; the freecam addon),
`/cancel <status name or id>` (the status cancel packet 0x0F1, as the debuff addon's /cancel sends it; VanaCore's runs
before the addon's, which does the same; alone, lists what's on you).

## Decided not to do

`/load` `/unload` `/unloadall` `/plugins` `/list` (VanaCore has `/plugin`), `/about` family, `/addon unloadall`, `/ambient`,
`/controller`, `/gamepad`, `/keyboard`, `/mouse`: not planned (2026-10-09). (`/fillmode` was on this list until
2026-10-23, and it's built.)

## What to do about it

1. Add the **MISSING** and **partial** rows above. `/raw`, `/exec` arguments, `/include`, `/execstr`, `/sleep`, `/pause`,
   `/wait` outside scripts, `/load` `/unload` `/unloadall` `/plugins` `/list` and `/addon unloadall` are the ones addons
   and scripts actually use.
2. Check the unknown rows against a real Ashita (`/controller`, `/gamepad`, `/keyboard`, `/mouse`, `/ambient`,
   `/fillmode`, `/copy`, `/paste`): what they take as arguments isn't in the binary's strings.
