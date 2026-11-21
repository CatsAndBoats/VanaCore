# 8. Settings and files

How an addon keeps its settings and other files: `vana.settings` for settings tables (per character or shared),
`vana.json` for JSON, `vana.fs` and `vana.paths` for files and folders, and `vana.config` for VanaCore's own settings
file, `vanacore.ini`.

## Where files go

Everything lives under `settings\` in VanaCore's folder (`vana.paths.install`), keyed on the addon's name as its
folder spells it (`Sidekick`, not `sidekick`):

```
settings\addons\<Addon>\settings.json               scope 'global': the same for every character
settings\addons\<Addon>\<Character>\settings.json  scope 'character': the character logged in
settings\addons\<Addon>\<any other file>            vana.settings.write, global
settings\addons\<Addon>\<Character>\<any file>      vana.settings.write, per character
```

A character's folder is the character's name (letters, digits, `_` and `-` kept; the server id if nothing is left).
The launcher treats `settings\` as the player's: a verify never replaces anything in it, and VanaCore backups include
it.

Ashita addons that use Ashita's own `settings` library keep writing to `config\addons\<addon>\<name>_<id>\`. That is
unchanged. Any addon can call `vana.settings`, whatever its kind.

## Quick start

```lua
addon.name    = 'Sidekick'
addon.author  = 'Seekey'
addon.version = '3.0'

-- Shared by every character: settings\addons\Sidekick\settings.json
local config = vana.settings.register({
    defaults = { scale = 1, window = { x = 100, y = 200 } },
})

-- This character's: settings\addons\Sidekick\<Character>\favorites.json
local favorites = vana.settings.register({
    name     = 'favorites',
    scope    = 'character',
    defaults = { zones = {} },
    changed  = function (data) print('favourites for ' .. (vana.settings.character() or 'nobody')) end,
})

vana.on('command', function (e)
    local scale = e.command:match('^/sidekick scale (%d+%.?%d*)$')
    if scale then
        config.data.scale = tonumber(scale)
        config:save()
        return true
    end
end)

-- Favourites keyed by zone id: whole-number keys come back as numbers
favorites.data.zones[230] = 'Ru\'Lude Gardens'
```

`store.data` is a plain table: read it and change it directly. `save()` writes it now. Changes are also saved when the
addon unloads or the game closes, and before a character switch, so a forgotten `save()` doesn't lose them.

## Stores

### `vana.settings.register(options)` → store

| Option | Default | Meaning |
|--------|---------|---------|
| `name` | `'settings'` | The file: `<name>.json`. A plain name: no folders, no `.json` |
| `scope` | `'global'` | `'global'` or `'character'` |
| `defaults` | `{}` | What a new file starts with, and what an existing file lacks is filled in from. Must be something JSON can hold |
| `changed` | none | `function (data, store)`, called when a per-character store's data is swapped: a character logs in, another takes over, or a logout puts the defaults back |

An option that's wrong raises an error at the call: a name with a folder or `.json`, an unknown scope, defaults with a
function in them, or the same name and scope registered twice (keep the store it returned).

### The store

| | |
|---|---|
| `store.data` | The settings. The same table for the store's whole life: after a reload or a character switch its contents change, not the table, so `local config = store.data` stays good. Tables inside it are replaced, though, so don't hold on to those across a switch |
| `store:save()` | Writes the file now. `true`, or `nil` and why: no character in yet (per-character), the file can't be written, or it's being held (below) |
| `store:reload()` | Reads the file again and returns `store.data`. Use it after editing the file by hand while the game runs. What wasn't saved is lost |
| `store:reset()` | The defaults again, saved. `true`, or `nil` and why |
| `store:path()` | The file it reads and writes now, or `nil` for a per-character store with no character in |
| `store.ready` | `true` when the store has a file: always for a global store, while a character is in for a per-character one |
| `store.name`, `store.scope`, `store.defaults` | As registered (`defaults` is a copy) |

## What happens when

**Registering.** The store's file is read now. If it isn't there it's written from the defaults, so the player can find
and edit it. If it lacks some of the defaults (a setting added in a new version), they're filled in: tables key by key,
but a list from the file is taken as it is, not merged with the default list. A file that then differs from what
VanaCore would write is written again, so the file on disk always matches `store.data` after loading.

**Before a character is in** (the title screen, the character select): a per-character store holds its defaults,
`store.ready` is false, `store:path()` is nil, and `store:save()` returns `nil, 'no character is logged in'`. Global
stores work as normal.

**A character logs in.** Once a per-character store exists, the character is looked at twice a second
(`vana.game.character()`). When one comes in, each per-character store reads that character's file and `changed` is
called.

**Another character, or a logout.** First each per-character store saves what changed to the character it was read
for. Then it reads the new character's file, or goes back to its defaults at a logout, and `changed` is called.

**The addon unloads** (`/addon unload`, `/addon reload`, the game closing). After the addon's own `unload` handlers,
each store that changed since it was last read or written is saved. That includes changes the `unload` handlers made.

**Nothing changed.** A store whose data is what was last read or written isn't written at unload or a switch. So if the
player edits the file by hand while the game runs, and the addon doesn't touch that store, the edit survives. To take
it up without a restart, call `store:reload()`.

**A file that isn't JSON** (a bad hand edit). A copy is kept beside it as `<name>.json.broken`, a line in the chat says
so, the defaults are used, and the file is written again from them. If even the copy can't be written, the file is left
exactly as it is and the store refuses to save over it (`save()` returns `nil` and why) until a `store:reload()` that
can keep the copy.

**Writing.** Every write goes through `vana.fs.write`: `<file>.tmp` beside the file, swapped in, so a write cut short
(the game closing, a full disk) leaves the old file whole. If another program holds the file open, or the folder is
read-only, the write fails and says so (`can't write <path> (Windows error 32)`).

## JSON: `vana.json`

```lua
local text, why = vana.json.encode(value)   -- the text, or nil and what can't be written (and where: value.window.f)
local value, why = vana.json.decode(text)   -- the value, or nil and where the text went wrong (line 3: unexpected x)
local line = vana.json.encode(value, true)  -- compact: one line, no indents (what a web request sends)
```

- **Written for people.** Keys sorted, whole-number keys first; two-space indents; a newline at the end. A file reads
  and diffs the same from save to save.
- **Lists and objects.** A table whose keys are 1 to n with none missing is a JSON list; any other table is an object.
  An empty table is `{}`.
- **Number keys.** Keys written as whole numbers (`"230"`, `"-1"`, `"0"`; not `"07"` or `"1.5"`) are read back as
  numbers, so a table keyed by zone or item id round-trips. The flip side: a text key that looks like a whole number
  comes back as the number.
- **`null`** is read as `nil`: a key with `null` isn't there, and a list with `null` in it has a hole.
- **Not allowed:** functions, userdata, threads, `true`/`false` or table keys, NaN or infinity, a table that holds
  itself. Refused with an error, not written as something else.
- **Numbers.** Whole numbers exact up to 2^53. Fractions with 14 significant digits when that reads back exactly, else
  17, so `0.1` stays `0.1`.
- A UTF-8 byte order mark at the start of a file is skipped. Text is written as UTF-8, as it was given.

## Other files in the settings folder

For files that aren't a settings table (an XML layout, a CSV export, a cache), in the same folders:

| | |
|---|---|
| `vana.settings.write(file, text[, scope])` | Writes `text`, making the folders it needs, whole or not at all. `true`, or `nil` and why |
| `vana.settings.read(file[, scope])` | The file's text, or `nil` and why (`'<file> isn't there'`, no character) |
| `vana.settings.exists(file[, scope])` | `true` or `false` |
| `vana.settings.remove(file[, scope])` | Deletes a file (or an empty folder). `true`, or `nil` and why |
| `vana.settings.list([scope])` | The names of the files in the folder (not subfolders), `{}` if it isn't there yet, or `nil` and why |
| `vana.settings.path(file[, scope])` | Where `file` is, whether or not it's there, or `nil` and why |
| `vana.settings.folder([scope])` | The folder itself, or `nil` and why |
| `vana.settings.character()` | The logged in character's folder name, or `nil` |

`scope` is `'global'` (the default) or `'character'`. With `'character'` and no character in, each returns
`nil, 'no character is logged in'`.

`file` is a path under the folder, with subfolders if you like (`'layouts/war.xml'`, `/` or `\`). Refused, with the
reason returned: a drive or a full path, `.` or `..`, Windows' reserved names (`con`, `nul`, `com1`, ...), the characters
`< > : " | ? *` and control characters, a name ending in a dot or a space.

```lua
local ok, why = vana.settings.write('layouts/' .. job .. '.xml', xml, 'character')
if not ok then
    print('Sidekick: layout not saved: ' .. why)
end
```

## Coming from Ashita's `settings` library

| Ashita (`require('settings')`) | VanaCore |
|---|---|
| `settings.load(defaults)` | `vana.settings.register({ scope = 'character', defaults = defaults })`, then `store.data` |
| `settings.load(defaults, 'alias')` | `register({ name = 'alias', scope = 'character', defaults = ... })` |
| `settings.save()` | `store:save()` |
| `settings.reset()` | `store:reset()` |
| `settings.register('settings', 'key', fn)` | `register({ ..., changed = fn })` |
| `config\addons\<addon>\<name>_<id>\settings.lua` | `settings\addons\<Addon>\<Character>\settings.json` |

Ashita's library has no global scope; here it's the default. Its files are Lua; these are JSON, so they can't hold
functions. Nothing moves an addon's existing `config\addons` files across: an addon that wants to keep them can read the
old file once with `io.open` and save it into its store.

## Files anywhere: `vana.fs` and `vana.paths`

Lua's `io` library works as usual. `vana.fs` adds what it lacks:

```lua
vana.fs.exists(path)              -- true if the file or folder is there
vana.fs.is_dir(path)              -- true if it's a folder
vana.fs.create_dir(path)          -- the folder and any missing parents; true if it exists afterwards
vana.fs.remove(path)              -- a file, or an empty folder; true if removed
vana.fs.write(path, data)         -- whole or not at all (<path>.tmp swapped in); true, or nil and why
vana.fs.list(folder[, filter[, which]])
```

`list` returns names (not paths), or `nil` and a message. `filter` is a regular expression for the whole name (empty
for all), matched without regard to case; `which` is `'files'` or `'folders'`, or a table
`{ which = 'files', deep = true, case = true }` (`deep`: every subfolder's entries too, as paths from `folder`; `case`:
the filter matches case).

```lua
for _, name in ipairs(vana.fs.list(addon.path .. '\\profiles', '.*\\.lua') or {}) do
    print(name)
end
```

Paths are in the ANSI code page, as Lua's `io` library uses. The folders you'll want:

| | |
|---|---|
| `vana.paths.install` | VanaCore's folder (where `VanaCore.exe` is) |
| `vana.paths.addons` | `<install>\addons` |
| `vana.paths.addon` | Your addon's folder |
| `vana.paths.game` | The game's own folder (`FFXiMain.dll`, `FTABLE.DAT` and the ROMs), `''` before the game is loaded |

None ends in a backslash.

## VanaCore's own settings: `vana.config`

`vanacore.ini` holds VanaCore's settings (renderer, effects, window, the game's own `[ffxi]` settings, and so on).
Addons can read and write it:

```lua
vana.config.get(section, key[, default])   -- the value as text (UTF-8), or `default` (nil) if the key isn't set
vana.config.set(section, key, value)       -- writes it (value as text; nil removes the key); true if written
vana.config.game(name)                     -- the game's own setting as the ini hands it to the game ('0001', 'padmode000'), or nil
vana.config.reload()                       -- takes the file up now (below); true if it was
vana.config.info()                         -- { upstream, changed, file }
```

Every key is optional: what the file hasn't is VanaCore's default, so `get` returns `nil` for a default. The `[client]`,
`[graphics]` and other sections and keys are listed in the VanaCore README.

**How a change reaches the running game.** The game works from a copy of the settings file made at start. A `set`
goes to the copy at once and to the file three seconds later, and what reads the setting sees it at the next frame.
Graphics settings apply at once after `vana.graphics.reload()`. A change made to the **file** by something else (an
editor, the launcher) isn't taken up by the running game unless `[client] ini.upstream=1` is set, `vana.config.reload()`
is called, or the game restarts; `vana.config.info().changed` is true while the file holds something the game hasn't
taken up, and `.file` names the file (the launcher gives each account its own, `vanacore.<hash>.ini`).

`vana.config` is for VanaCore's settings. Your own settings belong in `vana.settings`.
