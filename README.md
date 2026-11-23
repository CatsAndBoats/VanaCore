<p align="center">
  <img src="assets/banner.png" alt="VanaCore" width="640">
</p>

<p align="center">
  <b>A modern launcher and client for FINAL FANTASY XI.</b>
</p>

<p align="center">
  <a href="https://github.com/CatsAndBoats/VanaCore/releases/latest"><img src="https://img.shields.io/github/v/release/CatsAndBoats/VanaCore?include_prereleases&label=version&color=2454eb" alt="Version"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/licence-GPL--3.0--or--later-2454eb" alt="Licence: GPL-3.0-or-later"></a>
  <a href="https://github.com/CatsAndBoats/VanaCore/releases"><img src="https://img.shields.io/github/downloads/CatsAndBoats/VanaCore/total?color=2454eb" alt="Downloads"></a>
</p>

<p align="center">
  <a href="https://github.com/CatsAndBoats/VanaCore/releases/latest"><img src="assets/download.png" alt="Download VanaCore" width="280"></a>
</p>

## About

VanaCore makes FINAL FANTASY XI look and run like a modern game: a new renderer, upscaling and frame generation, an
interface you can lay out your way, fixes to the base game, and a powerful addon API. Everything is set in game from
the **/vana** window.

VanaCore is fully open source under the [GPL v3](LICENSE): free to use, study, change and share, as long as what you
share stays open source too.

## Core Features

- **Modern graphics:** Direct3D 12 or 11 in place of the game's Direct3D 8, with sun shadows, ambient occlusion,
  per-pixel lighting, reflections, fog, god rays, bloom and depth of field.
- **DLSS and FSR:** NVIDIA DLSS (with frame generation, and [DLSS 5](#dlss-5) with NVIDIA's own DLL) on RTX cards,
  and AMD FSR 3/4 with frame generation on any card.
- **HUD Manager:** edit the game's own interface like FFXIV's HUD layouts. Move, resize or hide any window or menu.
- **Addons and plugins:** VanaCore's own addon API can place things in the 3D world, hook and add to the game's
  menus, and draw windows under the game's own.
- **Base game fixes:** smoother crowds and busy zones, fewer stutters, and a loading screen when you change zone that
  shows where you're going and how far along it is.

## Additional Features

- **Water:** VanaCore's own water shader, with shore foam, waterfalls, the sea out to the horizon, and wet ground in
  the rain.
- **Mass Grass:** thick grass across the zones' ground.
- **A bigger world:** a longer draw distance and more zone detail.
- **Photo mode:** a free camera with time of day, weather, depth of field, and your character's animations with play,
  pause and a frame slider.
- **An interface that fits:** ultrawide and tall screens without stretching, and sharp text with real fonts.
- **Ashita and Windower support:** Ashita v4 addons and plugins (plugin SDK 4.16 and 4.30), Windower 4 addons and
  plugins, and some Windower 3 plugins.
- **For addon makers:** more than 30 events, packets, memory through LuaJIT's FFI, web requests and WebSockets,
  reload without a restart, and the in-game **API Explorer** with runnable snippets.
- **Debugging:** debug views for each rendering pass, a live frame-rate graph, performance recordings, detailed logs,
  and crash reports with full crash dumps.
- **Quality of life:** a stable borderless window, controller support, and many fixes to the game's menus.

## Requirements

- Windows 10 or 11, or Linux and the Steam Deck through Wine or Proton (Direct3D 12 there needs Microsoft's
  `d3dcompiler_47` in the prefix: `winetricks d3dcompiler_47`).
- A graphics card with Direct3D 12 (feature level 11_0, resource binding tier 2). DLSS needs an NVIDIA RTX card; FSR
  runs on any card.
- An installed FINAL FANTASY XI client: a private server's client, or retail through PlayOnline.

VanaCore isn't made or endorsed by Square Enix, and doesn't own any of the game's files or data. The game's names,
text and data, including the item, spell and ability data generated from it for VanaCore's addons, belong to Square
Enix.

## Credits

- **Main developers:** [Vekien](https://github.com/vekien) and [Carver](https://github.com/CarverXI)
- **GM icons:** Ramtough and Ark
- **Special thanks:** [RubyMatrix](https://github.com/rubymatrix), whose [xi-on-anything](https://github.com/rubymatrix/xi-on-anything)
  is where VanaCore's Direct3D 12 renderer and effects began
- **Shaped and beta tested by:** the players of CatsEyeXI (see [Beta testers](#beta-testers))

---

# Developers & Private Servers

> **Running a private server?** We highly recommend forking VanaCore and making it your own: point it at your
> server, give it your name and branding, and change whatever you'd like for your players. See
> [Making it yours](#making-it-yours) below.

## Running it

A release is a folder: `VanaCore.exe`, `VanaCore.Core.dll`, `dlss\`, `addons\`, `fonts\`, `scripts\` and
`vanacore.example.ini`. Settings come from `vanacore.ini` beside `VanaCore.exe` (copy it from `vanacore.example.ini`),
or from the file `--ini` names. Every key is optional: what the file leaves out is VanaCore's default.

### From a launcher (how CatsEyeXI's launcher does it)

The launcher writes a settings file for each account, then starts VanaCore with the login on the command line, so the
password is never stored in a file:

```
VanaCore.exe --server play.example.com --user myname --password "my password" --ini vanacore.1a2b3c4d5e6f.ini
```

| Argument | What it does |
|----------|--------------|
| `--server <host>` | The login server. Overrides `[server] server=` |
| `--user <name>` | The account name (`--username` works too) |
| `--password <pass>` | The password (`--pass` works too). VanaCore never logs or prints its command line |
| `--ini <file>` | The settings file to use instead of `vanacore.ini`, relative to `VanaCore.exe`'s folder. VanaCore won't start if it's missing, so make it first |
| `--lang <JP\|US\|EU>` | PlayOnline's region (`0`-`2` also work). Default `US` |

CatsEyeXI's launcher names each account's file `vanacore.<first 12 hex digits of the SHA-256 of the lowercased
username>.ini`, so logs and screenshots never show the username, and writes at least this into it before each start:

```ini
[client]
; where the game is installed
pol=C:\Games\MyServer\Game\PlayOnlineViewer
ffxi=C:\Games\MyServer\Game\FINAL FANTASY XI

[server]
; the version VanaCore's login reports: only major.minor is checked. Match what your server's login wants
; (VanaCore's own is 2.1.2; a LandSandBoat server from before 2026-02-12 wants 2.0.x)
loginversion=2.0.1

[graphics]
renderer=d3d12

[ffxi]
; the game's own settings, so no PlayOnline registry setup is needed
mode=borderless
resolution.game=auto
resolution.ui=auto-75
```

`vanacore.example.ini` lists every setting with its default. `[ffxi]` covers the game's own options (window mode,
resolutions, textures, sound, gamepad), so a launcher can set them up without touching the registry.

### By hand

Put the login in `vanacore.ini` and run `VanaCore.exe` on its own:

```ini
[server]
server=play.example.com
user=myname
password="my password"
```

```
VanaCore.exe                         # logs in with vanacore.ini's [server] and starts the game
VanaCore.exe --retail                # the retail game, through PlayOnline
VanaCore.exe <executable> [args...]  # any other program, with VanaCore loaded into it
```

`[client] bootloader=` starts a bootloader such as xiloader instead of VanaCore's own login, passing it `[server]`'s
server, user and password.

In game, `/vana` opens VanaCore's window. Logs and crash dumps go to `logs\` next to `VanaCore.exe`.

## Making it yours

What a server's fork usually changes:

| What | Where |
|------|-------|
| The default login server | `src/launcher/settings.hpp` (`kDefaultServer`), and `[server] server=` in `vanacore.example.ini` |
| The server's name in messages ("try again from the CatsEyeXI launcher", the console's welcome) | `src/launcher/main.cpp` |
| Discord Rich Presence: your own Discord application's ID and art | `src/core/discord.cpp` (`kAppId`, `kArtText`) |
| Staff icons over GMs' names, by server address | `src/core/gmicons_staff.h` |
| Your server's own DAT overlay folder, served before any other | `src/core/pivot.cpp` (`kServer`) |
| The name and logo | `assets/`, `addons/vanacore/vana/logo.png`, `src/launcher/VanaCore.ico`. A fork may say it's based on VanaCore but must use its own name and logo ([LICENSING.md](LICENSING.md), term 4) |

The GPL asks one thing back: if you share your fork, share its source too.

## Building

### You'll need

- **Visual Studio 2022 or 2026** with the **Desktop development with C++** workload (v143 toolset).
- **Git**, and an internet connection the first time (the build downloads NVIDIA's DLSS SDK and AMD's FSR DLLs).
- **Python 3**, only for `lint.bat` and the code generators in `tools/`.

### Steps

1. Clone the repository:

   ```
   git clone https://github.com/CatsAndBoats/VanaCore.git
   cd VanaCore
   ```

2. Build it, from a **Developer Command Prompt for Visual Studio** (or open `VanaCore.sln` and build **Release | x86**):

   ```
   msbuild VanaCore.sln /p:Configuration=Release /p:Platform=x86
   ```

   Everything is 32-bit, as FFXI is, except the DLSS helper, which builds as 64-bit on its own.

3. Find it in `build\Release\`, with VanaCore's addons, scripts and fonts copied beside it. To run it from there, copy
   `vanacore.example.ini` to `build\Release\vanacore.ini` and set it up as in [Running it](#running-it).

4. For a folder to give to players, run `build-release.bat`. It builds Release and puts the player's folder together in
   `build\package\VanaCore`, with the debug symbols apart in `build\package\symbols`.

### Good to know

- The first build downloads NVIDIA's DLSS SDK from [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) and checks every
  file's SHA-256 (`third_party\dlss\fetch-dlss.ps1`): its licence allows it only inside an application, so it isn't
  in the repository. AMD's FSR headers are in the repository; its signed DLLs are downloaded the same way
  (`third_party\fsr\fetch-fsr.ps1`).
- LuaJIT builds with it (`third_party\build-luajit.cmd`).
- The C runtime is linked statically, so players need no Visual C++ redistributable.

### Before sending changes

Run `lint.bat lizard filelen` (seconds) or the whole `lint.bat` (a few minutes). It fails for a function or file
grown past its allowance in `tools\lint\baseline.json`.

## Repository layout

| Path | What it is |
|------|------------|
| `src/launcher/` | `VanaCore.exe`: logs in, starts the game's process with `VanaCore.Core.dll` loaded into it |
| `src/core/` | `VanaCore.Core.dll`: everything inside the game (hooks, settings, overlay, the Lua and plugin hosts, Ashita and Windower compatibility) |
| `src/gfx/` | Direct3D 8 on Direct3D 12 and 11 (ported from xi-on-anything), and its offscreen tests |
| `src/dlss/` | `VanaCore.Dlss.exe`, the helper that runs DLSS, FSR and frame generation |
| `src/shared/` | Headers both use (the version, what the launcher hands the core) |
| `addons/vanacore/` | VanaCore's own Lua addons, the /vana window among them |
| `docs/` | The reference, and the addon and plugin docs (see [Documentation](#documentation)) |
| `tools/` | Code generators (the Ashita ABI and Lua bindings, game data, docs), lint, and test programs |
| `third_party/` | LuaJIT, Dear ImGui, AsmJit, LuaSocket, the Ashita SDK's headers, FSR's headers, the DLSS fetch script |
| `fonts/`, `assets/`, `scripts/` | Fonts and images loaded into the game, and the default `boot.txt` |

## Documentation

- [docs/reference.md](docs/reference.md): everything VanaCore does, with every `vanacore.ini` setting.
- [docs/addons/](docs/addons/README.md): writing addons, and the whole `vana` Lua API.
- [docs/plugins/](docs/plugins/README.md): what Ashita plugins can ask of VanaCore.
- [docs/tech/d3d12-frame-order.html](docs/tech/d3d12-frame-order.html): one Direct3D 12 frame in order, and when the grass, glow, shadows and effects come in.
- [docs/water-test-round.md](docs/water-test-round.md): the places to look at in the game after a change to the water, and what each should look like.
- [vanacore.example.ini](vanacore.example.ini): every setting with its default.

## Contributing

Issues and pull requests are welcome, AI-assisted ones included. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: it
says what a pull request needs before it can be merged.

## Third-party code

VanaCore builds on:

- [xi-on-anything](https://github.com/rubymatrix/xi-on-anything): the Direct3D 12 renderer and effects
- [Ashita v4 SDK](https://github.com/AshitaXI/Ashita-v4beta)
- [LuaJIT](https://github.com/LuaJIT/LuaJIT)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [AsmJit](https://github.com/asmjit/asmjit)
- [LuaSocket](https://github.com/lunarmodules/luasocket)

Their licences are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Inspired by [Windower Fenestra](https://github.com/Windower/Fenestra).

## DLSS 5

DLSS 5 isn't bundled or provided by VanaCore, as NVIDIA hasn't released it yet. To use it, find NVIDIA's
`nvngx_dlssnr.dll` yourself and put it in VanaCore's `dlss\` folder, beside `VanaCore.Dlss.exe`. The next time the
game starts, DLSS 5's switch in the /vana window's DLSS panel unlocks (it needs an NVIDIA RTX 40 or 50 card). Once
NVIDIA officially releases DLSS 5, VanaCore will provide it.

## Licence

VanaCore is free software under the GNU General Public License v3 or later
([LICENSE](LICENSE)), with additional permissions for linking NVIDIA's DLSS SDK and AMD's FSR SDK and for the game and
the plugins and addons VanaCore loads, and terms for its name and logo. [LICENSING.md](LICENSING.md) has those, and
which licence covers which file.

FINAL FANTASY XI is a registered trademark of Square Enix Holdings Co., Ltd.

## Also check out

- **Looking for a native client for Mac, Linux, Windows and Steam Deck?** Check out
  [xi-on-anything](https://github.com/rubymatrix/xi-on-anything).

## Beta testers

Special thanks to the CatsEyeXI players who beta tested VanaCore:

Yunas, Tai (Bug Bounty), Crobat, Torkson, Carver, Araedi (Grass Detective), Stieg, Muziko, Mizer, SmartASCII, Nokk,
Fluff, Pie (GM), Mythicangel, Loudnoises, Elehex, Ramtough, Arkine, Azlora, Cadia, Crocodile, Daeloth, Guatalupe,
Kraunik, Lunexi, Nobody, Thethethethe, Vitali, Charta, Citrus, Odei, Onix, The Doctor, Hando_San, Tizer, Snuk, Arestia,
Rusbando, Benthere, Skeletonhater, MadMikeBLVD, Patronos, Caite.
