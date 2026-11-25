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

## Layout

| Folder | What's in it |
|---|---|
| `exe/` | VanaCoreApp.exe: `main.cpp` (start, update check, loading the DLL), `splash.cpp` |
| `dll/` | VanaCoreApp.dll: `exports.cpp` (`VanaCoreAppApi`, `VanaCoreAppRun`), `home.cpp` (the first view) |
| `shared/` | `app_api.h` (exe and DLL), `ui.cpp` (the .vcaddon card's look, compiled into both), `version.h` |
| `res/` | VanaCore's icon and logo |
