# VanaCore

A modern launcher and client for FINAL FANTASY XI. VanaCore starts the game and runs inside it: it gives FFXI a
Direct3D 12 (or 11) renderer with DLSS, FSR and frame generation, scene effects and sun shadows, a longer draw
distance, Ashita plugins and Ashita and Windower addons, and many fixes and quality-of-life changes to the game's own
interface.

VanaCore is free software (GPL-3.0-or-later, see [Licence](#licence)). It isn't made or endorsed by Square Enix: you
need your own copy of the game.

## What it does

- **Graphics.** `renderer=d3d12` (or `d3d11`) replaces the game's Direct3D 8 with VanaCore's own, which draws through
  Direct3D 12 or 11. On it: ambient occlusion, height fog, god rays, bloom, a colour grade and sharpening; sun shadows;
  per-pixel lighting; a night and moonlight; depth of field; an optional cel-shaded look; FXAA; texture packs.
- **Upscaling and frame generation.** NVIDIA DLSS (Super Resolution, frame generation, DLSS 5) on RTX cards, and AMD
  FSR 3/4 with frame generation on any card.
- **The world.** A longer draw distance and more zone detail, grass, and faster frames (the game's
  per-frame occlusion check answered without stalling the graphics card).
- **The interface.** The game's interface keeps its shape in ultrawide and tall windows, at any resolution; True Font
  draws the game's text with real fonts; windows can be moved, resized and hidden.
- **Addons and plugins.** VanaCore's own Lua addon API, plus Ashita v4 addons and plugins (SDK 4.16 and 4.30) and
  Windower 4 addons, all run through VanaCore without Ashita or Windower installed.
- **The game itself.** Logs in to a LandSandBoat server without a bootloader (or starts xiloader, or the retail game
  through PlayOnline), a stable borderless window, controller support, a free camera, crash reports with call stacks.
- **The /vana window.** Every setting in game, applied live where it can be.
- **An MCP server** (off by default) so AI tools on the same machine can inspect and drive the running client.

## Requirements

- Windows 10 or 11. Linux and the Steam Deck under Wine or Proton (Direct3D 12 there needs Microsoft's
  `d3dcompiler_47` in the prefix: `winetricks d3dcompiler_47`).
- Direct3D 12: a graphics card with feature level 11_0 and resource binding tier 2. DLSS needs an NVIDIA RTX card;
  FSR runs on any card.
- An installed FINAL FANTASY XI client (a private server's client, or retail through PlayOnline).

## Running it

Put `VanaCore.exe` and the rest of a release next to `vanacore.ini` (copy it from `vanacore.example.ini`), point it at
the game and the server, and run `VanaCore.exe`:

```ini
[client]
pol=C:\Games\FFXI\PlayOnlineViewer
ffxi=C:\Games\FFXI\FINAL FANTASY XI

[server]
server=your.server.example
user=myname
```

```
VanaCore.exe                                   # logs in with vanacore.ini's [server] and starts the game
VanaCore.exe --server S --user U --password P  # the same, from a launcher
VanaCore.exe --retail                          # the retail game, through PlayOnline
VanaCore.exe <executable> [args...]            # any other program, with VanaCore loaded into it
```

In game, `/vana` opens VanaCore's window. Logs and crash dumps go to `logs\` next to `VanaCore.exe`.

## Building

Visual Studio 2022 or 2026 with **Desktop development with C++** (v143 toolset). Everything is 32-bit, as FFXI is:

```
msbuild VanaCore.sln /p:Configuration=Release /p:Platform=x86
```

Output goes to `build\<Configuration>\`. The build also builds LuaJIT (`third_party\build-luajit.cmd`). The C runtime is
linked statically, so players need no VC++ redistributable.

NVIDIA's DLSS SDK isn't in the repository (its licence allows it only inside an application): the build downloads it
from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) the first time and checks every file's SHA-256
(`third_party\dlss\fetch-dlss.ps1`). AMD's FSR headers are in the repository; its signed DLLs are downloaded the same
way (`third_party\fsr\fetch-fsr.ps1`).

`build-release.bat` builds Release and puts together a player's folder in `build\package\VanaCore`, with the debug
symbols apart in `build\package\symbols`.

Before sending C or C++ changes, run `lint.bat lizard filelen` (seconds) or the whole `lint.bat` (a few minutes): it
fails for a function or file grown past its allowance in `tools\lint\baseline.json`.

## Repository layout

| Path | What it is |
|------|------------|
| `src/launcher/` | `VanaCore.exe`: logs in, starts the game's process with `VanaCore.Core.dll` loaded into it |
| `src/core/` | `VanaCore.Core.dll`: everything inside the game (hooks, settings, overlay, the Lua and plugin hosts, Ashita and Windower compatibility) |
| `src/gfx/` | Direct3D 8 on Direct3D 12 and 11 (ported from xi-on-mac), and its offscreen tests |
| `src/dlss/` | `VanaCore.Dlss.exe`, the helper that runs DLSS, FSR and frame generation |
| `src/shared/` | Headers both use (the version, what the launcher hands the core) |
| `addons/vanacore/` | VanaCore's own Lua addons, the /vana window among them |
| `docs/` | [The reference](docs/reference.md) (every setting and feature), [writing addons](docs/addons/README.md), [the plugin API](docs/plugins/README.md) |
| `research/` | Notes from reverse-engineering the client and designing features |
| `tools/` | Code generators (the Ashita ABI and Lua bindings, game data, docs), lint, and test programs |
| `third_party/` | LuaJIT, Dear ImGui, AsmJit, LuaSocket, the Ashita SDK's headers, FSR's headers, the DLSS fetch script |
| `fonts/`, `assets/`, `scripts/` | Fonts and images loaded into the game, and the default `boot.txt` |

## Documentation

- [docs/reference.md](docs/reference.md): everything VanaCore does, with every `vanacore.ini` setting.
- [docs/addons/](docs/addons/README.md): writing addons, and the whole `vana` Lua API.
- [docs/plugins/](docs/plugins/README.md): what Ashita plugins can ask of VanaCore.
- [vanacore.example.ini](vanacore.example.ini): every setting with its default.

## Credits

VanaCore builds on [xi-on-mac](https://github.com/rubymatrix/xi-on-mac) (its Direct3D 12 renderer and effects),
[Windower Fenestra](https://github.com/Windower/Fenestra), the [Ashita v4 SDK](https://github.com/AshitaXI/Ashita-v4beta),
[LuaJIT](https://github.com/LuaJIT/LuaJIT), [Dear ImGui](https://github.com/ocornut/imgui),
[AsmJit](https://github.com/asmjit/asmjit) and [LuaSocket](https://github.com/lunarmodules/luasocket). Their licences
are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

Copyright (C) 2026 Vekien. VanaCore is free software under the GNU General Public License v3 or later
([LICENSE](LICENSE)), with additional permissions for linking NVIDIA's DLSS SDK and AMD's FSR SDK and for the game and
the plugins and addons VanaCore loads, and terms for its name and logo. [LICENSING.md](LICENSING.md) has those, and
which licence covers which file.

FINAL FANTASY XI is a registered trademark of Square Enix Holdings Co., Ltd.
