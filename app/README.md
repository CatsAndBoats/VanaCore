# VanaCore App

A Windows app for starting VanaCore, in two signed files:

- **VanaCoreApp.exe**: the splash, the update check (to come), then loads VanaCoreApp.dll. Kept small and changed rarely,
  since a new signed exe starts its certificate's reputation over.
- **VanaCoreApp.dll**: every view and all the logic. Updated freely, and in future by the exe itself.

They agree on `shared/app_api.h`: plain C, a version number (`VANACOREAPP_API`) the exe checks before calling the DLL.
Both link the CRT in (/MT), so the two files are all it needs.

## Building

Visual Studio 2022 (v143), x86:

```
msbuild app\VanaCoreApp.sln -p:Configuration=Release -p:Platform=x86
```

Both land in `app\build\Release\` (Debug in `app\build\Debug\`), each with its `.pdb`.

## Updating

At the splash, VanaCoreApp.exe asks GitHub for the newest published App release (`app-v<version>`, not a draft) and
compares it with VanaCoreApp.dll's own version. A newer one is downloaded (the release's `VanaCoreApp.dll`), checked
against `app.json`'s SHA-256, its signature and signer (the exe's), and its version, then swapped in; the old DLL stays
beside it as `VanaCoreApp.dll.old` and comes back if the new one won't load. Only the DLL: a release whose API needs a newer
exe is left for the player to download. Anything that fails (no network, GitHub down, a folder that can't be written)
leaves the DLL as it was, says so on the splash in amber, and the app starts. `--no-update` skips the check;
`VANACOREAPP_RELEASES` points it at another releases list (a fork's, for testing).

## Releasing

`.github/workflows/app.yaml`, run by hand, releases the App on its own: its own version (`VCAPP_DLL_VERSION` in
`shared/version.h`, the patch number raised by the workflow), its own GitHub releases (`app-v<version>`, never marked
latest), no VanaCore build or release.

- **dev**: builds, signs, pushes the two files to the vcdev build on R2 for testers, and drafts the App release.
- **prod**: the tested draft's files (checked against vcdev's) go into the vanacore build on R2 and the latest VanaCore
  release's zip (its notes then say "Includes VanaCore App x.y.z"), then the draft is published.

VanaCore always carries the newest published App (`.github/scripts/bundle_app.ps1`): `release.yaml` puts it in every
package, `promote.yaml` again after every promotion.

The exe stays the same signed file while `VCAPP_EXE_VERSION` is the last published App's: the workflow uses that release's
exe again and only the DLL is new. Raise `VCAPP_EXE_VERSION` when the exe has to change, and always with
`VANACOREAPP_API` (the workflow stops if the API changed and the exe's version didn't).

## Addons tab

**From Ashita** and **From Windower** bring a player's setup over: they pick the folder they played with (Ashita's or
Windower's own, or the one it's in), are told what was found and asked, then it's copied in the background with a bar.
Nothing VanaCore's folder has already is replaced, so running it again adds only what's new.

- Ashita: `addons\<name>` to `addons\ashita`, `plugins\*.dll` to `plugins\ashita` (not Addons or Sequencer), `config\addons`,
  `config\plugins` and a copied plugin's `config\<name>` to `config\`, the other scripts to `scripts\`. `scripts\default.txt`
  (else the script a `config\boot\*.ini` names) goes into `boot.txt`: `/load <p>` as `/plugin load <p> ashita`, `/addon load
  <a>` with `ashita` when it was copied, `/input <command>` (alone or a bind's) as the command, the rest as it is.
- Windower: `addons\<name>` to `addons\windower` (an addon's `data\` with it), `plugins\*.dll` to `plugins\windower`, the other
  scripts to `scripts\`. `scripts\init.txt` and the launcher's autoloaded plugins (`settings.xml`) go into `boot.txt`: `lua load
  <a>` as `/addon load <a> windower`, `load <p>` as `/plugin load <p> windower`, `bind`, `alias`, `wait`, `exec` and `input` as
  VanaCore's, an addon's command with one slash, Windower's own settings (`console_*`, `game_*`) as comments.

Both leave out `addons\libs` (VanaCore carries the libraries). `boot.txt` gets the lines at its end under a `# From ...`
comment, without the addons and plugins it loads already, and is first kept as `boot.txt.before-migrate` (once).

## Layout

| Folder | What's in it |
|---|---|
| `exe/` | VanaCoreApp.exe: `main.cpp` (start, update check, loading the DLL), `splash.cpp` |
| `dll/` | VanaCoreApp.dll: `exports.cpp` (`VanaCoreAppApi`, `VanaCoreAppRun`), `home.cpp` (the first view), `home_addons.cpp` + `migrate.cpp` (the Addons tab) |
| `shared/` | `app_api.h` (exe and DLL), `ui.cpp` (the .vcaddon card's look, compiled into both), `version.h` |
| `res/` | VanaCore's icon and logo |
