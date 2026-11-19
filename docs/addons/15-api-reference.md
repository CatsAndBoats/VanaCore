# 15. API reference

Every function and field in `vana`, one line each, grouped by table. The page each belongs to has the details. A
`→` gives what a function returns; `[x]` marks an optional argument.

## `vana`

| | |
|---|---|
| `vana.version` | VanaCore's version as text (`'0.8.47'`) |
| `vana.log(...)` | A line in `logs\vanacore.log` as `<addon>: ...` |
| `print(...)` | A line in the chat log (and the log); global |

## `vana.events` ([Events](02-events.md))

| | |
|---|---|
| `vana.events.on(name, fn[, key]) → key` | Run `fn` each time `name` is raised; `key` names the handler (default `fn`) |
| `vana.events.off(name, key) → bool` | Remove it |
| `vana.on`, `vana.off` | The same |

Events: `load`, `unload`, `present`, `command`, `unhandled`, `text_in`, `text_out`, `packet_in`, `packet_out`, `key`,
`mouse`, `key_state`, `key_data`, `xinput_button`, `dinput_button`, `xinput_state`, `beginscene`, `endscene`, `d3d_dp`,
`d3d_dip`, `d3d_dpup`, `d3d_dipup`, `plugin_event`, `ipc`, `gameui_button`, `gameui_replaced`, `gameui_trigger`,
`gameui_window`; game events `login`, `logout`, `zone_out`, `zone_change`, `job_change`, `subjob_change`,
`level_change`, `target_change`, `menu_open`, `menu_close`, `main_menu_open`, `main_menu_close`, `action_menu_open`,
`action_menu_close`, `chat_input_open`, `chat_input_close`.

## `vana.tasks` and time ([Tasks and timing](03-tasks-and-timing.md))

| | |
|---|---|
| `vana.tasks.once(delay, fn, ...) → task` | `fn(...)` once, `delay` seconds from now |
| `vana.tasks.oncef(delay, fn, ...) → task` | `delay` frames from now |
| `vana.tasks.repeating(delay, repeats, every, fn, ...) → task` | `repeats` times, `every` seconds apart |
| `vana.tasks.repeatingf(delay, repeats, every, fn, ...) → task` | The same in frames |
| `vana.tasks.spawn(fn, ...) → finished, ...` | Runs `fn` now as a task; its results if it didn't wait |
| `vana.tasks.cancel(task)` | Stops it before its next run |
| `vana.tasks.frame() → n` | Frames this addon has seen |
| `coroutine.sleep(seconds)`, `coroutine.sleepf(frames)` | Inside a task: wait |
| `vana.time.clock() → seconds` | High-resolution clock |

## `vana.game` ([Game data](04-game-data.md))

| | |
|---|---|
| `vana.game.character() → name, id` | The logged in character; `nil` before login |
| `vana.game.self() → id, zone, job, level, subjob, sublevel, status, target_index, hp, mp, tp, hpp, mpp` | The player in numbers; `nil` before login and while the player isn't in the world |
| `vana.game.player() → table` | `hp_max`, `mp_max`, `main_job`, `main_job_level`, `sub_job`, `sub_job_level`, `exp`, `exp_needed`, `stats`, `stat_modifiers`, `attack`, `defense`, `resists`, `title`, `nation`, `rank`, `rank_points`, `homepoint`, `residence`, `gil`, `limit_points`, `merit_points`, `merit_points_max`, `limit_breaker`, `exp_locked`, `limit_mode`, `assimilation_points`, `su_level`, `item_level`, `unity`, `mastery`, `mount_recast`, `buffs[32]`, `status_icons[32]`, `status_timers[32]`, `homepoints`, `data_loaded`, `sub_map` |
| `vana.game.skills() → table` | `[skill id] = { name, skill, capped }` |
| `vana.game.crafts() → table` | `[craft id] = { name, skill, rank, capped }` |
| `vana.game.job_points() → table` | `[job id] = { capacity, points, spent }` |
| `vana.game.jobs() → table` | `[job id] = { level, master_level }` |
| `vana.game.pet() → table` | `index`, `id`, `name`, `hpp`, `mpp`, `tp`, `target_id`, `owner_index`, `owner_id`; `nil` without a pet |
| `vana.game.missions() → table` | `nation`, `nation_mission`, `zilart`, `promathia`, `promathia_progress`, `crystalline_prophecy`, `moogle_kupo_detat`, `shantotto_ascension`, `adoulin`, `rhapsodies`, `voracious`, `declined` |
| `vana.game.linkshell([slot]) → table` | Slot 1 (the default) or 2: `slot`, `name`, `message`, `poster`, `time`, `read_level`, `write_level`, `public_edit_level` (once its message has come), `rank`, `color`, `id`, `item`, `item_id` (while its pearl is equipped); `nil` for none |
| `vana.game.merits() → table` | `[merit id] = { count, next }` |

### `vana.gamedata` (static tables from the DATs and the server; [Game data](04-game-data.md#static-game-data-vanagamedata))

| | |
|---|---|
| `vana.gamedata.<table>[id] → row` | `spells`, `abilities`, `weapon_skills`, `status_effects`, `zones`, `items`, `key_items`, `titles`, `jobs`, `skills`, `elements`, `merits`, `job_points`, `traits`, `mounts`, `monster_families`, `augments`, `slots`, `skill_caps`, `skill_ranks`; loaded on first use |
| `vana.gamedata.find(table, name) → id \| nil` | By name, case-insensitive (items by log name too) |
| `vana.gamedata.all(table) → table` | Every part loaded, for `pairs()` (`items` is in parts) |
| `vana.gamedata.tables() → { name = { rows, generated, part_size?, parts? } }` | What's shipped; a table kept in parts (`items`) has `part_size` and `parts` (`{ [part] = rows }`) |
| `vana.gamedata.item_stats(id[, extra] \| slot) → table \| nil, why` | An item's stats as the Equipment window counts them: `id`, `name`, `description`, `source` (`'server'`, `'description'`, `'unread'`), `mods` and `server` (`{ NAME = value }`), `latents`, `sets`, `augments` (given its extdata, or a container's slot), `stats` (each name's `label`, `unit`, `percent`, `highest`, `shown`), `data` (`generated`, `source`) |
| `vana.game.party() → table` | `members[18]`, `leader`, `party_leaders[3]`, `party_counts[3]`. A member has `hp`, `mp`, `tp`, `hpp`, `mpp` and the maximums the game never says: `hp_max`, `hp_max_low`, `hp_max_high`, `hp_max_source` (`'self'`, `'full'`, `'estimate'`, `'none'`), and the same for `mp` ([Maximum HP and MP](04-game-data.md#maximum-hp-and-mp)) |
| `vana.game.vitals(id) → table \| nil` | A party or alliance member's health by server id, also while they are in another zone: `id`, `party_slot`, `hp`, `hpp`, `mp`, `mpp`, `tp`, `hp_max`, `hp_max_low`, `hp_max_high`, `hp_max_source`, `mp_max`, `mp_max_low`, `mp_max_high`, `mp_max_source`. nil for anyone not in the party or alliance |
| `vana.game.request_max_hp(ids, callback) → true \| false, why` | Asks CatsEyeXI's server for the maximum HP of up to 8 entities by server id (a monster's too: the game has none). `callback(answers)` runs once, in a following frame, with `{ id, hp, hp_max, status }` for each id asked (`status`: `'ok'`, `'not_found'`, `'disabled'`, `'too_fast'`, `'hidden'`, `'no_answer'`). An answer's `hp_max` is also kept a minute as the entity's `hp_max` (`hp_max_source = 'server'`). Example, printing the current target's HP and maximum HP: [Asking the server](04-game-data.md#asking-the-server-monsters-too) |
| `vana.game.entity_by_id(id) → table \| nil` | The same table as `entity` for the entity with that server id (every slot is looked at: ask when it's needed, keep the index) |
| `vana.game.entity(index) → table` | `index`, `id`, `name`, `x`, `y`, `z`, `heading`, `distance` (squared), `speed`, `hpp`, `type`, `race`, `status`, `spawn_flags`, `model_size`, `hitbox_size`, `claim`, `target_index`, `pet_index`, `render_flags`, `name_flags`, `valid_target`, `name_x`, `name_top`, `name_bottom` (its name on the screen, nil while it isn't drawn), `models[9]`; also `distance_yalms`, `is_pc`, `is_npc`, `is_monster`, `is_self`, `claim_id`, `claimed`, `anim_speed`, `door_id`, `render[9]`, `status_server`, `status_event`, `costume_id`, `zone_id`, `animations[10]`, `animation_time`, `animation_step`, `animation_play`, `emote_target_index`, `emote_id`, `linkshell_color`, `name_color`, `campaign_name_flag`, `mount_id`, `last_action_id`, `interaction_target_index`, `ballista_flags`, `fellow_index`, `trust_owner_index`, and for a party or alliance member `party_slot`, `hp`, `mp`, `tp`, `mpp`, `hp_max`, `hp_max_low`, `hp_max_high`, `hp_max_source`, `mp_max`, ... (`hp_max_source` is `'none'` for anyone else) |
| `vana.game.entity_count() → n` | Entity slots (2304) |
| `vana.game.target() → table` | `index`, `id`, `active`, `sub_index`, `sub_id`, `sub_active`, `is_sub_target`, `locked`, `menu_open`, `raw[2]` |
| `vana.game.set_target(index) → bool` | Target that entity |
| `vana.game.world() → table` | `server_time`, `weather`, `chat_open` |
| `vana.game.zoning() → bool` | Between zones |
| `vana.game.loading() → table?` | A zone (or the login's first zone) loading: `phase` (`server`, `zone`, `people`), `login`, `zone`, `previous`, `elapsed`, `ready`, `zone_files`, `zone_expected`, `people_files`, `people_expected`, `file`, `step` (`leaving`, `connecting`, `waiting`), `wait`, `answer`, `out_type`; nil when none is |
| `vana.game.player_zone(listed) → zone` | The zone-in's zone until the party list has it |
| `vana.game.zone_weathers(zone) → ids \| nil` | The weathers (0x057's ids, lowest first) the zone's model DAT has a sky and light for, the DAT the game loads (pivot's overlays too); nil if it can't be read |
| `vana.game.abilities() → bits` | 224 bytes: weapon skills from bit 0, job abilities from 0x200, pet commands from 0x400, traits from 0x600; `nil` until sent |
| `vana.game.spells() → bits` | 128 bytes; `nil` until sent |
| `vana.game.key_items() → bits` | 512 bytes; `nil` until sent |
| `vana.game.recasts() → table` | `abilities[timer_id]`, `spells[spell_id]` in 1/60 s |
| `vana.game.container(id) → table` | `capacity` (slot 0 included: 81 for an 80-slot bag), `[0..n] = { id, index, count, flags, price, extra }`; slot 0 is reserved (the gil, in the inventory) |
| `vana.game.equipment() → table` | `[0..15] = { container, index }` |

## `vana.chat` and text ([Chat and commands](05-chat-and-commands.md))

| | |
|---|---|
| `vana.chat.write(text[, mode])` | A line in the chat log (mode 1 white, 2 green, 68 red, 104 yellow, 106 cream) |
| `vana.chat.run(line[, mode])` | Run as typed, next frame |
| `vana.chat.preview(key, value)` | VanaCore's own: a chat window's place previewed as /vana's slider drags |
| `vana.encoding.to_shift_jis(utf8) → sjis` | |
| `vana.encoding.from_shift_jis(sjis) → utf8` | |
| `vana.ashita.parse_auto_translate(text, brackets) → text` | Phrases spelled out |
| `vana.ashita.regex.search(text, pattern) → matches` | Every match, `{ { match, groups... }, ... }` or `nil` |
| `vana.ashita.regex.match(text, pattern) → match` | The whole text, or `nil` |
| `vana.ashita.regex.replace(text, pattern, with) → text` | `$1` for a group |
| `vana.ashita.regex.split(text, pattern) → pieces` | |

## `vana.ui` ([Drawing](06-drawing.md))

| | |
|---|---|
| `vana.ui.text([props]) → box` | A text box |
| `vana.ui.rect([props]) → box` | A filled rectangle |
| `vana.ui.image(props) → box` | A picture (`file` required) |
| `box.<property>`, `box:set(props)` | Read and write properties |
| `box:show()`, `box:hide()`, `box:destroy()` | |
| `box.destroyed` | True once destroyed |
| `vana.ui.screen() → width, height` | VanaCore's interface size (0, 0 before the first frame) |
| `vana.ui.icon(name) → glyph, font` | A Material Symbols icon by name; `nil` if none |
| `vana.ui.add_image([path]) → path` | Copies a .png, .jpg, .bmp or .gif into VanaCore's images folder (over a file of that name) once it's known to decode; its path from VanaCore's folder (`images\<name>`), as `[client] window.image` takes it. With no path, Windows' Open dialog asks for one. `nil, why` if it can't be used; `nil` if the dialog is cancelled |
| `vana.ui.add_font([path]) → family` | Copies a .ttf, .otf, .ttc, .woff or .woff2 into VanaCore's fonts folder (it loads at every start) and loads it now; with no path, Windows' Open dialog asks for one. `nil, why` if it can't be used; `nil` if the dialog is cancelled |
| `vana.ui.fonts() → { family, ... }` | The fonts in VanaCore's fonts folder (icon fonts left out) |
| `vana.ui.remove_font(family) → true` | Takes a font added to VanaCore's fonts folder out: unloaded now, its file moved to the Recycle Bin. `nil, why` for an icon font or one not added |
| `vana.ui.game_on_top([on])` | The game's windows are drawn over this addon's ImGui windows where they meet (on unless `false`; dropped on unload) |
| `vana.ui.interface_hidden() → bool` | True while the game's interface is hidden (F11, `[ui] hide.key`) |
| `vana.ui.set_interface_hidden(on) → bool` | Hides or shows the interface as F11 does (hiding works in the world only); true if it's hidden after |

Properties: `x`, `y`, `right`, `visible`, `opacity`, `layer`, `block_mouse`; text: `text`, `font`, `size`, `bold`,
`italic`, `color`, `background`, `padding`, `outline`, `stroke`, `stroke_color`, `color_codes`, `gradient`, `gradient_from`, `gradient_to`; rect: `width`, `height`,
`color`, `radius`; image: `file`, `width`, `height`, `color`, `repeat_x`, `repeat_y`.

## `vana.imgui` ([Drawing](06-drawing.md))

Dear ImGui 1.92 by its own names, with Ashita's Lua conventions (pointers as tables, vectors as `{x, y}` in and
numbers out). `vana.imgui.types.<Struct>.new()` makes ImGui structures.

## `vana.layout` ([Drawing](06-drawing.md))

| | |
|---|---|
| `vana.layout.windows() → list` | Addons' and plugins' ImGui windows: `{ name, owner, x, y, w, h, scale, shown, pinned, saved, locked, no_pointer }` |
| `vana.layout.set(name, x, y, scale)` | Move or scale one (`nil` leaves a part) |
| `vana.layout.flags(name, locked, no_pointer)` | Lock one (the mouse can't drag or resize it) and/or let the mouse through it (`nil` leaves a part) |
| `vana.layout.reset(name)` | Forget what's set for it |
| `vana.layout.highlight(name)` | Outline it this frame and the next |
| `vana.layout.outline(what[, snap[, label[, anchor]]]) → drawn, x, y` | VanaCore's own (the HUD Manager): a game window's box |
| `vana.layout.grid(pixels[, color, alpha, background, background_alpha, centre_alpha])` | VanaCore's own: a grid this frame |
| `vana.layout.up(what) → up` | VanaCore's own (the HUD Manager's Open only): that box's window is open now; `nil` if it can't tell |
| `vana.layout.clicked() → id` | VanaCore's own |
| `vana.layout.box(name[, label[, snap[, picked]]]) → drawn, x, y, from_x, from_y` | VanaCore's own (the HUD Manager's Addon View): an addon window's yellow box, Shift + drag moving it |
| `vana.layout.box_clicked() → name` | VanaCore's own |
| `vana.layout.select(what)`, `vana.layout.selected() → ids` | VanaCore's own |

## `vana.gameui` ([The game's interface](07-game-interface.md))

| | |
|---|---|
| `vana.gameui.hide(element) → true` | Not drawn |
| `vana.gameui.show(element) → true` | Drawn again |
| `vana.gameui.move(element, x, y) → true \| false, why` | Percents of the interface |
| `vana.gameui.move(element) → true` | Back to the player's setting |
| `vana.gameui.rect(element) → left, top, right, bottom \| nil` | Where it is on the screen, in the interface's pixels; `'target_window'` for now, nil while nothing is targeted |
| `vana.gameui.add(menu, label, fn[, colour]) → true` | Your button; `fn()` when chosen |
| `vana.gameui.remove(menu) → true` | |
| `vana.gameui.replace(item, fn) → true \| false, why` | `fn()` instead of the game's action |
| `vana.gameui.restore(item) → true` | |
| `vana.gameui.image(element, file[, options]) → true \| false, why` | Your picture (PNG, relative to your folder) in the game's place; options `width`, `height`, `x`, `y` (units), `color`, `opacity` |
| `vana.gameui.image(element) → true` | The game's own picture again |
| `vana.gameui.on(menu, event[, items], fn) → id` | `fn(e)` when the player does `event` in `menu` (`'select'`, `'cancel'`, `'up'`, `'down'`, `'left'`, `'right'`, `'menu'`, `'*'`); `e.menu`, `e.event`, `e.key`, `e.item`, `e.name`, `e.caught` |
| `vana.gameui.catch(menu, event[, items], fn) → id` | The same, and the game's own action is left out |
| `vana.gameui.off(id)` | The trigger taken out |
| `vana.gameui.elements() → list` | `{ { id, name, actions }, ... }` |
| `vana.gameui.hidden(element) → string \| nil` | The addons and plugins that have it hidden, `'a, b'` (the player's own switch isn't in it); the HUD Manager shows it as the element's Hide, on and locked |
| `vana.gameui.stats_dump() → n` | The Equipment window's stats worked out again into the log (`Stats dump:` lines): each equipped item's id, name and source, its modifiers, latent effects and augments, the sets, each stat's sum in the server's units and as shown. The items dumped; `-1` with `equip_mods.json` missing, `-2` while it's still being read (`/vana statsdump`) |
| `vana.gameui.hide(window) → true` | Any of the game's windows by its name (`'configwi'`): drawn into nothing, still read by `window`, still worked by the keys, not by the mouse |
| `vana.gameui.window(window) → table \| nil` | `name`, `x`, `y`, `w`, `h`, `scale`, `cursor`, `count`, `focused`, `closing`, `hidden`, `drawn`; `rows` `{ id, text, x, y, w, h, cursor, disabled, drawn, state }`; `lines` `{ text, x, y, w, h, color }`; `pictures` `{ name, x, y, w, h, color }` |
| `vana.gameui.windows() → list` | The names of the game's windows up |
| `vana.gameui.press(window, row[, event]) → true \| false, why` | Cursor on `row` (nil: where it is), then `event` (`'select'` left out) through the game's key input |
| `vana.gameui.watch(window, fn) → id` | `fn(e)` in the frame the window is drawn: `e.what` `'open'`, `'change'` (anything it shows changed), `'close'`; `e.name`; `e.window` (`window()`'s table, whole; nil at close). `window` a name, `'main_menu'`, `'party_menu'`, `'config_menu'`, `'*'` or a list |
| `vana.gameui.unwatch(id)` | The watch ended |

Elements: `chat_log`, `chat_input`, `party_list`, `alliance_lists`, `target_window`, `target_arrow` (hide, image),
`status_icons`, `macro_bar`, `network_window`, `nameplates` (hide), `config_menu`, `party_menu.find_member`.

## `vana.world` ([Widgets](18-widgets.md))

| | |
|---|---|
| `vana.world.attach(box, index[, options])` | A `vana.ui` box at the name over target `index`: `side`, `x`, `y`, `scale`, `distance`, `walls` |
| `vana.world.place(box, x, y, z[, options])` | A box at a point of the world: `up`, `pivot`, `x`, `y`, `scale`, `distance`, `range`, `over_ui`, `walls`; `facing` (`'camera'`, or laid in the world: `'upright'`, `'ground'`, `'wall'`) with `heading`, `width`, `height` (yalms); `toward` (yalms nearer the camera; an upright card stands out toward it) |
| `vana.world.update(box, options)` | Changes some options of a placed box without placing it again (`index` for another name, `at` for another point too); cheap every frame |
| `vana.world.placement(box) → table \| nil` | How the box is placed now: `x`, `y`, `scale`, `walls`; at a name `side`, `distance`, `index`; at a point `up`, `distance`, `range`, `over_ui`, `facing`, `heading`, `width`, `height`, `toward`, `at`; `nil` if not placed |
| `vana.world.detach(box)` | Back to being drawn at its `x` and `y` |
| `vana.world.to_screen(x, y, z) → x, y, distance \| nil` | A point of the world in the interface's pixels |
| `vana.world.visible(x, y, z) → bool \| nil` | Whether the zone's collision leaves the camera a clear line to the point |
| `vana.world.pick(x, y) → x, y, z \| nil` | The point of the zone under a pixel of the interface: where the camera's line through it first meets the zone's collision (the ground, a wall), as `vana.game.entity` has positions; nil when nothing is met within 150 yalms |
| `vana.world.name_at(index) → x, top, bottom \| nil` | Where the name over target `index` is, in the interface's pixels: the target's place across (its name is centred on it), the letters' top and bottom as drawn lately; nil when the name wasn't drawn in the last moments |
| `vana.world.marks(list)` | Rings of light on the ground, this addon's whole set: each `{ x, y, z, radius, color, strength, shape, height, glyph, glyph_size, glyph_height }`; `shape` `'round'` or `'square'`, `height` a column of light rising from it, `glyph` A-D or 1-4 over its top, `glyph_height` how high it floats (a waymark); nil takes them off. Laid on the ground by the scene effects, hidden by what stands in front |

## `vana.settings` and `vana.json` ([Settings and files](08-settings-and-files.md))

| | |
|---|---|
| `vana.settings.register(options) → store` | `name`, `scope` (`'global'`, `'character'`), `defaults`, `changed` |
| `store.data`, `store.ready`, `store.name`, `store.scope`, `store.defaults` | |
| `store:save() → true \| nil, why` | |
| `store:reload() → data` | |
| `store:reset() → true \| nil, why` | |
| `store:path() → path \| nil` | |
| `vana.settings.write(file, text[, scope]) → true \| nil, why` | |
| `vana.settings.read(file[, scope]) → text \| nil, why` | |
| `vana.settings.exists(file[, scope]) → bool` | |
| `vana.settings.remove(file[, scope]) → true \| nil, why` | |
| `vana.settings.list([scope]) → names \| nil, why` | |
| `vana.settings.path(file[, scope]) → path \| nil, why` | |
| `vana.settings.folder([scope]) → path \| nil, why` | |
| `vana.settings.character() → name \| nil` | |
| `vana.json.encode(value[, compact]) → text \| nil, why` | Indented for files; `compact` writes one line |
| `vana.json.decode(text) → value \| nil, why` | |

## `vana.http`, `vana.ws`, `vana.promise`, `vana.async` ([HTTP, WebSockets and promises](17-http-and-websockets.md))

Nothing here blocks: a call returns a promise (or runs a callback `fn(res, err)` given last) and the answer comes in a later
frame, on the game's thread.

| | |
|---|---|
| `vana.http.get/head/delete(url[, options][, callback]) → promise` | `options`: `method`, `headers`, `query`, `body`, `json`, `form`, `auth`, `timeout`, `max_time`, `follow`, `max_redirects`, `max_size`, `save`, `insecure`, `strict` |
| `vana.http.post/put/patch(url, body[, options][, callback]) → promise` | A table `body` is sent as JSON |
| `vana.http.request(options_or_url[, options][, callback]) → promise` | Everything in one table |
| `vana.http.json(url[, options][, callback]) → promise` | The decoded JSON; rejected unless 2xx |
| `vana.http.download(url, path[, options][, callback]) → promise` | The body written to `path` whole or not at all; rejected unless 2xx |
| `vana.http.pending() → n`, `vana.http.info() → table` | Requests going; `{ requests, sockets, waiting, workers }` |
| `vana.http.encode(table) → text`, `.urlencode(text) → text`, `.basic_auth(user, password) → text` | Form and query text, `Basic ...` |
| response | `ok`, `status`, `reason`, `url`, `headers` (lower case), `set_cookies`, `body`, `saved`, `res:json()`, `res:text()`, `res:header(name)` |
| error | `code` (`timeout`, `dns`, `connect`, `tls`, `cancelled`, `too_large`, `redirect`, `file`, `http`, `json`, `invalid_request`, `invalid_url`, `busy`, `unavailable`, `failed`; a socket's also `handshake`, `closed`), `message`; `print(err)` shows the message |
| `vana.ws.connect(url[, options]) → socket` | At once; `options`: `headers`, `protocols`, `timeout`, `keepalive`, `max_size`, `insecure`, `on_open`, `on_message`, `on_close`, `on_error` |
| `socket:on(name, fn) → socket`, `socket:off(name, fn)` | `'open'(socket)`, `'message'(data, is_binary)`, `'close'(code, reason, clean)`, `'error'(err)` |
| `socket:send(text \| table) → bool, why`, `:send_binary(bytes)`, `:send_json(value)` | Sent before it opens, they wait |
| `socket:close([code[, reason]])` | The closing handshake |
| `socket:opened()`, `:closed()`, `:receive() → promise` | The socket; `{ code, reason, clean }`; the next message's data |
| `socket.state`, `.protocol`, `.url`, `socket:is_open()`; `vana.ws.count() → n` | `'connecting'`, `'open'`, `'closing'`, `'closed'` |
| `vana.promise.new(function (resolve, reject) end) → promise` | Also `vana.promise(fn)` |
| `p:next(on_ok, on_err)`, `p:catch(fn)`, `p:finally(fn)` → promise | `p['then']` is `next` |
| `p:await() → value` | In a task: waits, or raises the reason |
| `p:cancel() → bool`, `p.state`, `p.value`, `p.reason` | Stops the request behind it |
| `vana.promise.resolve(v)`, `.reject(r)`, `.all(list)`, `.allSettled(list)`, `.race(list)`, `.any(list)`, `.delay(seconds, v)`, `.is(x)` | |
| `vana.async(fn, ...) → promise` | Runs `fn` now as a task |

## `vana.fs`, `vana.paths`, `vana.config` ([Settings and files](08-settings-and-files.md))

| | |
|---|---|
| `vana.fs.exists(path) → bool` | |
| `vana.fs.is_dir(path) → bool` | |
| `vana.fs.create_dir(path) → bool` | With parents |
| `vana.fs.remove(path) → bool` | A file or an empty folder |
| `vana.fs.write(path, data) → true \| nil, why` | Whole or not at all |
| `vana.fs.list(folder[, filter[, which]]) → names \| nil, why` | `which`: `'files'`, `'folders'`, or `{ which, deep, case }` |
| `vana.paths.install`, `.addons`, `.addon`, `.game` | Folders, no trailing backslash |
| `vana.config.get(section, key[, default]) → text` | A `vanacore.ini` value |
| `vana.config.set(section, key, value) → bool` | Writes it (`nil` removes) |
| `vana.config.game(name) → text \| nil` | A game setting as the ini hands it to the game |
| `vana.config.reload() → bool` | Take the file up now |
| `vana.config.info() → { upstream, changed, file }` | |

## `vana.binds` and input ([Input](09-input.md))

| | |
|---|---|
| `vana.binds.add(spec, command[, up]) → true \| nil, why` | `/bind` |
| `vana.binds.remove(spec[, up]) → bool`, `vana.binds.remove(bind) → bool` | `/unbind` |
| `vana.binds.list() → list` | `{ key, vk, spec, command, up, when, ctrl, alt, shift, win, apps, fired_ago }` |
| `vana.binds.base() → list \| nil` | `list()`'s entries, for the binds there were when `scripts\keybinds.txt` replayed; nil before |
| `vana.binds.keys() → names` | |
| `vana.binds.capture_begin()`, `vana.binds.capture_end()` | |
| `vana.binds.capture() → { active, done, cancelled, key, vk, ctrl, alt, shift, win, apps }` | |
| `vana.gamepad.list([refresh]) → controllers, xinput, xinput_reading` | DirectInput's controllers `{ guid, name, product, type, opened, reading, xinput }` each, which of XInput's four are connected, and which the game reads now |
| `vana.gamepad.game() → { known, on, xinput }` | The game's own gamepad and XInput switches as they are now (`known` false on a client it wasn't written for) |
| `vana.gamepad.select(guid) → true \| false, why` | |
| `vana.gamepad.apply({ mode, buttons, buttons_ex, guid }) → true \| false, why` | |
| `vana.gamepad.bindings() → { xinput, confirm, cancel, menu, options }` | |
| `vana.gamepad.state() → { present, idle }` | |
| `vana.gamepad.disable(off)`, `vana.gamepad.disabled() → bool` | |
| `vana.gamepad.nav(on)` | Your window has the controller next frame |

## `vana.packets` ([Packets](10-packets.md))

| | |
|---|---|
| `vana.packets.inject(incoming, id, data)` | Into the next buffer |
| `vana.packets.last(incoming, id) → data, at \| nil` | The last of that id |
| `vana.packets.queue(id, length, align, p1, p2, fill) → bool` | The game's own outgoing queue; `fill(pointer)` |

## `vana.memory` ([Memory and FFI](11-memory-and-ffi.md))

| | |
|---|---|
| `vana.memory.read_int8/uint8/int16/uint16/int32/uint32/int64/uint64/float/double(address) → value` | 0 if unreadable |
| `vana.memory.read_string(address[, size]) → text` | |
| `vana.memory.read_bytes(address, size) → text \| nil` | |
| `vana.memory.read_array(address, size) → bytes \| nil` | |
| `vana.memory.write_int8/uint8/int16/uint16/int32/uint32/int64/uint64/float/double(address, value) → bool` | `true` if written |
| `vana.memory.write_string(address, text) → bool` | With a NUL |
| `vana.memory.write_array(address, bytes) → bool` | |
| `vana.memory.find(module \| address, size, pattern[, offset[, count]]) → address \| 0` | The `count`th match (0: the first) of a byte pattern (`'8B 0D ?? ??'`) plus `offset`; `size` 0 is the whole module |
| `vana.memory.module([name]) → base, size \| nil` | |
| `vana.memory.alloc(size) → address \| 0`, `vana.memory.free(address) → bool` | |
| `vana.memory.protect(address, size, protection) → old \| nil` | |
| `vana.memory.unprotect(address, size) → bool` | |
| `vana.memory.assembler.assemble(text[, base]) → bytes \| nil, err` | |
| `vana.memory.assembler.cave(text) → address \| nil, err` | |
| `vana.memory.assembler.release(address) → bool` | |
| `vana.memory.assembler.call(address, target, nops)`, `.jmp(...)`, `.nop(address, count)` | |

## System ([System](12-system.md))

| | |
|---|---|
| `vana.system.open_url(url) → bool` | |
| `vana.system.play_sound(path) → bool` | |
| `vana.system.clipboard() → text`, `vana.system.set_clipboard(text) → bool` | Shift-JIS |
| `vana.system.focused() → bool` | |
| `vana.window.info() → table \| nil` | `x`, `y`, `width`, `height`, `client_width`, `client_height`, `monitor`, `topmost`, `minimized`, `maximized`, `covering`, `character`, `monitors` |
| `vana.window.monitor(n) → bool` | |
| `vana.window.block_mouse([on[, ms]]) → bool` | |
| `vana.clients.list() → list` | `{ pid, name, self, main }` |
| `vana.clients.set_main(name) → bool` | |
| `vana.clients.copy() → count \| nil, why` | |
| `vana.clients.front(pid) → bool` | |
| `vana.ipc.send(text) → bool` | To the same-named addon in other instances |
| `vana.graphics.info() → table` | `renderer`, `running`, `active`, `effects`, `gpu`, the driver, upscalers, sizes |
| `vana.graphics.hardware() → table` | `gpu`, `gpu_busy`, `gpu_temp`, `gpu_watts`, `gpu_limit_watts`, `vram_used_mb`, `vram_total_mb`, `vram_game_mb`, `vram_budget_mb`, `throttled`, the CPU (`cpu_busy`, `cpu_game`), the memory (`ram_used_mb`, `ram_total_mb`, `ram_game_mb`), power; the card's load from NVML (NVIDIA), this game's memory from DXGI (any card); held 5 s a call |
| `vana.graphics.reload()`, `.retry()`, `.recompile() → bool` | |
| `vana.graphics.damage_test(kind) → bool` | `'hit'` (one), `'round'` (three hits of a weapon skill at once) or `'barrage'` (nine hits over two seconds): numbers VanaCore draws itself over the player, placed by `[graphics] damage.spawn`, `damage.offset.*`, with the colours, multi-hit icon and skill name set; what the /vana Damage Numbers test buttons run. False for another kind |
| `vana.graphics.dlss_indicator([on]) → bool`, `.dlss_crash_test(kind)`, `.grass_assets()`, `.grass_capture([name])`, `.grass_capture_status()`, `.grass_reload()` | VanaCore's own |
| `vana.camera.preview(key, value)` | VanaCore's own |
| `vana.camera.free([on]) → on \| false, why` | The `/freecam` camera on or off (the other way with nothing). `why`: the game's camera view wasn't found, or no character is in the world |
| `vana.camera.float([on]) → on` | `/freecam float`: the floaty free camera, written to `[camera] freecam.float` |
| `vana.camera.state() → table` | `on`, `have` (on and holding the pose taken from the game's view), `hold`, `float`, `lag`, `speed` |
| `vana.camera.pose() → x, y, z, yaw, tilt \| nil` | The free camera's pose: the eye, and the way it looks as radians (yaw about the world's up from the world's x toward z; tilt up from level). `nil` unless `have` |
| `vana.camera.set_pose(x, y, z, yaw, tilt) → bool` | Puts it there; its speed and glide are dropped. `false` unless `have` |
| `vana.camera.hold([on])` | While held, the keys, mouse and wheel move nothing, so a pose set every frame stays. Off with the free camera |
| `vana.camera.speed([units]) → units` | The free camera's speed a second, set if given |
| `vana.mcp.info() → { running, failed, port, client, idle, sessions }` | |
| `vana.plugins.loaded() → list`, `vana.plugins.raise(name, data)` | `{ name, source, memory }` |
| `vana.addons.loaded() → list` | `{ name, source, version, author, desc, link, file, state, memory }` |
| `vana.ashita.object(name[, interface]) → userdata \| nil` | |
| `vana.ashita.item(id \| name[, language]) → userdata \| nil` | The resource manager's item (`GetItemById`, `GetItemByName`), for `compat\ashita.lua`'s FFI; without its icon |
| `vana.ashita.item_icon(id) → text` | The item's icon, read from its DAT; `''` for none |
| `vana.ashita.string(table, index \| text[, language]) → text \| index` | |
| `vana.ashita.win32(name, value) → number` | `SetFocus`, `SetForegroundWindow`, `GetSystemMetrics` |
| `vana.ashita.execute(file[, parameters]) → bool` | |
| `vana.ashita.console(show)` | |
| `vana.ashita.gui_visible([visible]) → bool` | |
| `vana.ashita.interface`, `vana.ashita.libs` | Numbers: 4.16 or 4.30 |

## The `addon` table

| | |
|---|---|
| `addon.name` | Lower case, set by VanaCore |
| `addon.path` | The addon's folder, set by VanaCore |
| `addon.author`, `addon.version`, `addon.desc`, `addon.link` | Yours to set |
