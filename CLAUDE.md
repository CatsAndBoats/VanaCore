# VanaCore: build and sync rules

For anyone (or any coding assistant) working on VanaCore. Many developers play from `build\Debug`: it's their live
VanaCore install. `build\Release` is where Release builds land. Keep them in step:

## 1. Whenever you build Release, also update Debug

Don't run a real Debug solution build. It fails to link VanaCore.Dlss (MDd against the release `nvsdk_ngx_d.lib`) and
deletes `build\Debug\dlss\VanaCore.Dlss.exe`. Build Release, then copy the results into `build\Debug`:

```
msbuild VanaCore.sln -p:Configuration=Release -p:Platform=x86
```

Then copy from `build\Release` to `build\Debug`: `VanaCore.Core.dll` (+ `.pdb`, `.map`), `VanaCore.exe`, `dlss\*`, and the
`addons\vanacore\` tree (including `vana.lua`).

- Check `tasklist` first. If `VanaCore.exe` (or the game) is running, the DLL is locked: say so and don't force it.
  Copy the Lua anyway if it isn't locked.
- Never overwrite the player's own files in `build\Debug`: `vanacore.ini`, the accounts' `vanacore.<hash>.ini`,
  `scripts\boot.txt`, `config\`, `plugins\ashita\`, logs. Copy only the build outputs listed above.
- Keep the old DLL as a backup beside it (`VanaCore.Core.dll.before-<what>`).

## 2. Whenever you edit the Vana Lua, also copy it to Debug

- `addons\vanacore\**` (`vana\vana.lua`, `keybinds`, `mcp`, `libs`, ...): copy the edited file(s) to the same
  relative path under `build\Debug\addons\vanacore\` (and `build\Release\addons\vanacore\`) right after the edit. A
  `.lua` in an addon needs no rebuild.
- `src\core\lua\*.lua` (`vana.lua` runtime, `compat\ashita.lua`, `compat\windower.lua`) is part of
  `VanaCore.Core.dll`, not loose. It takes effect only after a rebuild, so follow rule 1.

Before overwriting a Debug copy, diff it against the repo. If Debug has changes the repo doesn't, say so first. Don't
discard them silently.

## 3. The game works from a copy of its settings file (dev loop)

The settings file is the one VanaCore.exe is given with `--ini`, else `vanacore.ini`. The launcher gives each account its
own, `build\Debug\vanacore.<hash>.ini` (`VanaCore.AccountIniName`: 12 hex digits of the SHA-256 of the lowercased
username), made at Start Game from the account's profile if it isn't there; a `--ini` file that isn't there stops
VanaCore.exe. **A setting edited in `vanacore.ini` does nothing for a game started with `--ini`: edit the account's file**
(`vana.config.info().file` names it), or use `vana.config.set`. Every key is optional: what the file hasn't is VanaCore's
default. `vanacore.ini` stays what every game shares (`[characters] main`).

The running game reads `%TEMP%\VanaCore\vanacore.<pid>.ini`, a session copy of the file made at start
(`src\core\settings.cpp`, `ini::bind`), plain (no comments). What the game itself writes (the /vana window,
`vana.config.set`) goes to the copy at once and, 3 seconds later, that one key to the file (an account's file plain, no
comments kept; `vanacore.ini` as laid out). A change made to the **file** meanwhile (your edit, another tool's, the
launcher's) is **not applied** to a running game unless one of:

- `[client] ini.upstream=1` is in the file the game started from: it's taken up within a second.
- `vana.config.reload()` (MCP `lua` tool, or /vana > Client > Developer > Reload settings): taken up once.
- Restart the game.

`vana.config.info()` says `{upstream, changed, file}`; `changed` is true while the file holds something the game
hasn't taken up. Test and tool code that reads the settings of a running game should read the copy (the path
`settings_ini` holds), not the file. Never edit the copy by hand: it's deleted when the game ends.

## Docs

`README.md` is the short public front page. The full manual, every setting and feature, is `docs\reference.md`:
document a new setting or feature there, not in the README.

## 4. Run the lint before committing C/C++

Lint C++ runs on pull requests that touch `src\` (`.github\workflows\lint.yaml`); nothing checks a direct push. It fails
for a function or file grown past its allowance over `tools\lint\baseline.json`, or a new finding. Before committing a
change to `src\`, run `lint.bat lizard filelen` (seconds), and the whole `lint.bat` (about 4 minutes, cppcheck most of
it) when the change is more than a small one. Something worse than the baseline: split the function or move code out to
an include unit (`grass_keep.h`, `d3d8_casters.h`) in the same commit, not a "lint ratchet paid" commit after. Don't run
`--update-baseline --accept-growth` to make a failure pass without a maintainer agreeing.

## Never edit addons

Never edit an addon's files (an Ashita install's `addons\*`, any `addons\ashita\*` copy). We don't control them and they
work in real Ashita, so every addon bug is a VanaCore bug: fix it in VanaCore (`src\core\compat\...`), never in the
addon.
