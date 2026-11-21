# 13. Ashita and Windower addons

VanaCore runs three kinds of Lua addon, all with the same commands: its own, Ashita v4's, and Windower 4's. An addon
written for Ashita or Windower runs through a compatibility layer that puts that API's names over `vana.*`. This page
says how that works, what it means for an addon you write, and how to port one.

## The three kinds

| Folder | For | Libraries (`require`) |
|--------|-----|-----------------------|
| `addons\vanacore\` | VanaCore's own addons | `addons\vanacore\libs` |
| `addons\ashita\` | Ashita addons | VanaCore's copies of Ashita's (`common`, `sugar`, `switch`, `chat`, `settings`, `imgui`, `d3d8`, ...): `addons\vanacore\libs\ashita-4.30`, then `addons\vanacore\libs\ashita`, shared by both versions; then `addons\ashita\` itself (Ashita's addons root) |
| `addons\windower\` | Windower addons, laid out as Windower's own addons folder | `addons\windower\libs` (Windower's libraries), `addons\windower\res` (its resources) |

`/addon load <name>` looks in the folders in that order, so when two have an addon of the same name, VanaCore's comes
first, then Ashita's, then Windower's; `/addon load <name> windower` names the kind. The chat log says which loaded:
`(Ashita Addon) Loaded: aspect 1.0 by atom0s`.

### Which kind an addon is

VanaCore reads the addon's main file:

1. If it uses `vana.` anywhere, it's a VanaCore addon.
2. Else it counts `windower.` and `_addon.` against `ashita.`, `AshitaCore` and `addon.`: whichever it uses more.
3. If it uses none, it's its folder's kind.

So an Ashita addon that also calls `vana.gameui` once stays an Ashita addon only if it's in `addons\ashita\` and uses
`ashita.` more than... no: rule 1 wins, so it becomes a VanaCore addon and loses Ashita's API. **An Ashita or Windower
addon that wants `vana` must reach it without the word `vana.` in its main file**, or move the `vana` calls into a
second file it requires:

```lua
-- in main.lua (Ashita addon): no 'vana.' here
local extras = require('vanacore_extras')
```

```lua
-- vanacore_extras.lua
if vana ~= nil and vana.gameui ~= nil then
    vana.gameui.hide('party_list')
end
```

(The detection counts the word with a dot after it, so `vana ~= nil` alone in the main file doesn't tip it.)

### What every kind has

Each addon runs in its own LuaJIT state, with its kind's API, its libraries, and **`vana`**: the whole of VanaCore's
API is a global in every addon, whatever it's written for. An Ashita addon can keep its settings with
`vana.settings`, hide the party list with `vana.gameui`, or read `vana.game.party()`, and still run in real Ashita with
an `if vana ~= nil` guard (mind the detection above).

## Ashita addons

The layer is `src/core/lua/compat/ashita.lua` (built into `VanaCore.Core.dll`), run after `vana.lua` in every Ashita
addon. It provides `AshitaCore` and its managers (chat, memory objects for the party, player, entities, target,
inventory, the resource manager, fonts and primitives, the GUI manager, the plugin and addon managers, the pointer
manager), `ashita.events.register`, `ashita.memory`, `ashita.regex`, `ashita.fs`, `ashita.misc`, the `addon` and
`addon.instance` tables, and Ashita's Lua 5.2 extras (`table.pack`, `table.unpack`, `package.searchers`).

**Events**: `ashita.events.register(name, alias, fn)` for Ashita's names (`load`, `unload`, `command`, `d3d_present`,
`d3d_beginscene`, `d3d_endscene`, `packet_in`, `packet_out`, `text_in`, `text_out`, `key`, `mouse`, `key_data`,
`key_state`, `xinput_button`, `dinput_button`, `xinput_state`, `plugin_event`), each with the table Ashita's addons
have. `d3d_present` is VanaCore's `present`.

**ImGui**: Ashita's `imgui` library (`require('imgui')`) finds ImGui as `AshitaCore:GetGuiManager()`, which is
`vana.imgui`. ImGui itself is 1.92; an addon that loaded Ashita 4.16's `imgui.lua` passes 1.81's numbers and VanaCore
converts them.

**Interface version**: Ashita addons see interface 4.16 (CatsEyeXI's Ashita) unless `[addons] ashita.interface=4.30`
in `vanacore.ini`, which gives them what Ashita 4.30's Addons plugin does (`ashita.interface_version` 4.3, the 4.30 SDK
methods, `ashita.memory.assembler`, `AddonManager`). The libraries are 4.30's either way (they have everything 4.16's
had), unless `[addons] ashita.libs=4.16`.

**Files**: `addon.path` ends in a backslash as Ashita's does. `GetInstallPath()` is VanaCore's folder, so an addon's
`config\addons\<name>\` and `resources\` are where Ashita keeps them.

**The login packet**: with `[addons] delay.login=1` an Ashita addon that `boot.txt` loads after the login gets the
login's zone-in packet (`0x00A`) once as it loads, as it would have on Ashita.

The layer covers what the addons tried so far use; a call to a part that isn't there fails with a clear error naming
it (`... is not supported by VanaCore yet`) rather than misbehaving. Report those: an Ashita addon that works in Ashita
and not here is a VanaCore bug, fixed in VanaCore, never in the addon.

## Windower addons

The layer is `src/core/lua/compat/windower.lua`. It provides `windower.*` (`add_to_chat`, `send_command`,
`register_event`, `ffxi.get_player`, `get_party`, `get_mob_by_index`, `get_mob_by_target`, `get_info`, `get_items`,
`packets.inject`, `send_ipc_message`, `prim.*` and `text.*` for primitives and text objects, and more), `_addon`,
`coroutine.schedule`, `coroutine.sleep`, Windower's `string.pack` and `unpack`, and the paths (`windower.addon_path`,
`windower.windower_path`, `windower.ffxi_path`, `windower.pol_path`).

**Events** supported: `load`, `unload`, `prerender`, `postrender`, `addon command`, `unhandled command`, `login`,
`logout`, `job change`, `zone change`, `target change`, `status change`, `hp change`, `mp change`, `tp change`,
`hpp change`, `mpp change`, `mouse`, `keyboard`, `incoming chunk`, `outgoing chunk`, `incoming text`, `outgoing text`,
`action`, `action message`, `gain buff`, `lose buff`, `time change`, `day change`, `moon change`, `weather change`,
`ipc message`. The derived ones (`hp change`, `zone change`, ...) are worked out each frame from `vana.game.self()`.

**Commands**: `//name args` (or `//lua command name args`) runs the addon's `addon command` event. Windower's
libraries and resources are the real ones, in `addons\windower\libs` and `res`, laid out as Windower's addons folder so
their relative paths work.

## Porting to a VanaCore addon

Most addons needn't be ported: they run as they are. Porting is for an addon written for VanaCore from the start, or one
that wants `vana.settings`, `vana.gameui` and the rest without the layer. The names map one to one:

### From Ashita

| Ashita | VanaCore |
|--------|----------|
| `ashita.events.register('load', 'load_cb', fn)` | `vana.on('load', fn)` |
| `ashita.events.register('d3d_present', ...)` | `vana.on('present', fn)` |
| `ashita.events.register('command', ...)`, `e.command`, `e.blocked = true` | the same event and fields |
| `ashita.events.register('packet_in', ...)` | `vana.on('packet_in', fn)`, the same `e` |
| `ashita.events.register('text_in', ...)` | `vana.on('text_in', fn)`, the same `e` |
| `ashita.events.register('key', 'mouse', 'key_data', 'key_state', 'xinput_button', ...)` | the same names |
| `AshitaCore:GetChatManager():AddChatMessage(mode, indent, text)` | `vana.chat.write(text, mode)` |
| `AshitaCore:GetChatManager():QueueCommand(mode, line)` | `vana.chat.run(line, mode)`; lines queued in mode 2 (the game's macro stack) in one frame run last first, as in Ashita |
| `GetMemoryManager():GetParty():GetMemberName(i)` and friends | `vana.game.party().members[i + 1].name` |
| a party member's or entity's **maximum** HP (Ashita has none: addons note the HP when the percent is full) | `vana.game.party().members[i + 1].hp_max`, `vana.game.entity(index).hp_max`, `vana.game.vitals(id).hp_max` ([Maximum HP and MP](04-game-data.md#maximum-hp-and-mp)); guard with `if vana then` to keep the addon running in real Ashita |
| `GetMemoryManager():GetPlayer():GetMainJob()` | `vana.game.player().main_job` |
| `GetMemoryManager():GetEntity():GetName(index)` | `vana.game.entity(index).name` |
| `GetMemoryManager():GetTarget():GetTargetIndex(0)` | `vana.game.target().index` |
| `GetMemoryManager():GetInventory():GetContainerItem(bag, i)` | `vana.game.container(bag)[i]` |
| `GetMemoryManager():GetInventory():GetEquippedItem(slot)` | `vana.game.equipment()[slot]` |
| `GetMemoryManager():GetRecast():GetSpellTimer(id)` | `vana.game.recasts().spells[id]` |
| `GetPacketManager():AddIncomingPacket(id, data)` | `vana.packets.inject(true, id, data)` |
| `GetResourceManager():GetString('spells.names', id)` | `vana.ashita.string('spells.names', id)` |
| `ashita.memory.read_uint32(address)`, `find(...)` | `vana.memory.read_uint32`, `vana.memory.find` |
| `ashita.memory.assembler` | `vana.memory.assembler` |
| `ashita.regex.search(text, pattern)` | `vana.ashita.regex.search` |
| `ashita.misc.open_url`, `play_sound` | `vana.system.open_url`, `vana.system.play_sound` |
| `ashita.fs.exists`, `create_directory`, `get_directory` | `vana.fs.exists`, `vana.fs.create_dir`, `vana.fs.list` |
| `require('settings')` | `vana.settings` ([Settings and files](08-settings-and-files.md)) |
| `require('imgui')` | `vana.imgui` (the same calls) |
| `ashita.tasks.once(delay, fn)` | `vana.tasks.once(delay, fn)` |
| `addon.path` | `addon.path` (no trailing backslash here) |

### From Windower

| Windower | VanaCore |
|----------|----------|
| `windower.register_event('load', fn)` | `vana.on('load', fn)` |
| `windower.register_event('prerender', fn)` | `vana.on('present', fn)` |
| `windower.register_event('addon command', fn)` | `vana.on('command', ...)`, matching `//name` or `/name` yourself |
| `windower.register_event('incoming chunk', fn)` | `vana.on('packet_in', fn)`: `e.id`, `e.data`, `e.data_modified`, return `true` to block |
| `windower.register_event('incoming text', fn)` | `vana.on('text_in', fn)`: `e.message`, `e.message_modified` |
| `windower.register_event('zone change' / 'job change' / 'login' / 'target change', fn)` | `vana.on('zone_change' / 'job_change' / 'login' / 'target_change', fn)` with a table `e` |
| `windower.register_event('keyboard', fn)` | `vana.on('key_data', fn)` (DIK codes) or `vana.on('key', fn)` (virtual keys) |
| `windower.add_to_chat(mode, text)` | `vana.chat.write(text, mode)` |
| `windower.send_command(line)` | `vana.chat.run(line)` |
| `windower.chat.input(text)` | `vana.chat.run(text)` |
| `windower.ffxi.get_player()` | `vana.game.player()`, `vana.game.self()`, `vana.game.character()` |
| `windower.ffxi.get_party()` | `vana.game.party()` |
| `windower.ffxi.get_mob_by_index(i)` | `vana.game.entity(i)` |
| `windower.ffxi.get_mob_by_target('t')` | `vana.game.entity(vana.game.target().index)` |
| `windower.ffxi.get_info()` | `vana.game.world()`, `vana.game.player_zone(...)` |
| `windower.ffxi.get_items(bag)` | `vana.game.container(bag)` |
| `windower.ffxi.get_abilities()`, `get_spells()`, `get_key_items()` | `vana.game.abilities()`, `spells()`, `key_items()` (bit strings) |
| `windower.packets.inject_incoming(id, data)` | `vana.packets.inject(true, id, data)` |
| `windower.send_ipc_message(text)`, `'ipc message'` | `vana.ipc.send(text)`, `vana.on('ipc', ...)` |
| `windower.text.create(...)`, `windower.prim.create(...)` | `vana.ui.text({...})`, `vana.ui.rect({...})`, `vana.ui.image({...})` |
| `coroutine.schedule(fn, delay)` | `vana.tasks.once(delay, fn)` |
| `coroutine.sleep(seconds)` | the same, inside a task |
| `windower.addon_path`, `windower.windower_path` | `vana.paths.addon`, `vana.paths.addons` |
| `windower.get_windower_settings()` | `vana.ui.screen()`, `vana.window.info()` |
| `windower.open_url(url)`, `windower.play_sound(path)` | `vana.system.open_url`, `vana.system.play_sound` |
| `config.load(defaults)` (the `config` library) | `vana.settings.register({ scope = 'character', defaults = defaults })` |

### What changes in the move

- **Events hand you a table** (`e`) for the game events, where Windower passed positional arguments. `key` and `mouse`
  are positional in VanaCore too.
- **Zero- and one-based.** `vana.game.party().members` is 1-based (slot 1 is you); container and equipment tables are
  0-based, as the game's are.
- **Names stay Shift-JIS** from `vana.game`; Windower's `res` tables gave UTF-8 for resources. `vana.encoding` converts.
- **Settings are JSON**, not Lua, and can't hold functions.
- **No `require` for the API.** `vana` is already there.

## Where the layers are documented

`docs/ashita-windower-compat.md` in the VanaCore repository describes both layers and the plugin hosts in depth:
how commands are routed, the packet pipeline, which Ashita manager is built on what, and what's not covered yet.
