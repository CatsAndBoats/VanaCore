# Retail FFXiMain.dll: signatures

VanaCore finds everything in FFXiMain.dll by pattern (`scan::find`), never by address. This is what that held up against
when the CatsEyeXI client was compared with retail's, and how to keep it so. Checked 2026-10-31, offline, nothing run.

| | header | image | built |
|---|---|---|---|
| CatsEyeXI (`catseyexi-client\Game\FINAL FANTASY XI`) | `0x694E5965` | `0xBDF000` | 2025-12-26 |
| retail (`SquareEnix\FINAL FANTASY XI`) | `0x6A995428` | `0xC15000` | 2026-09-03 |

Retail's is the newer build. Both are packed the same way (code LZSS-compressed in `POL1`, unpacked as the game loads),
so `.text` can't be read off disk; `tools\sigaudit.py` unpacks with xi-tools first.

## What the audit found

- 278 hex patterns in the source. 264 are in both builds. 14 are in neither (strings VanaCore finds in data, polcore's,
  crypto constants, the 120-slot patch's own code): nothing to fix. **Four were in CatsEyeXI's only**, and each said why:
  - `kKeyTable` (camera Home key): the compiler picked another register (`66 8B 04 5D` became `66 8B 04 55`) and put
    `test ax,ax` elsewhere. Now two alternatives (`|`).
  - `packets.queuepacket1` (Ashita's table, twice): the packet id limit inside it, `cmp esi,11Eh`, is `120h` in retail.
  - `kDrawIcon` (item rarity): the item data's icon field, `0xD0C`, is `0x150C` in retail.
  - `kNetDraw` (Network window): `add eax,4630h` is `4638h`. The code already read that operand back.
- Under the 264 that match, 225 functions are instruction for instruction the same. The rest changed only in where fields
  sit, plus one UI function with extra instructions (`uiscale.cpp:27`) and some switch tables (not code).
- Over the whole binary 15,828 functions are the same, 1,275 changed, 75 added, 22 removed. What moved, by how much:

| shift | where (old offsets) | what |
|---|---|---|
| `+0x800` | `0xCAF-0xDA9` | an item's data (id, name, bitmap, icon): `sigs::item_data_shift()` reads it from the icon drawing |
| `+0x8` | `0x4630-0x2D608` | the game's big object (inventory `0x9860`, recasts `0x2CF40`, abilities `0x1A620`, the network's `0x4630`) |
| `+0x70` | `0x2D60C-0x43DC4` | the same object after a further `0x68` bytes (chat `0x40D0C`, craft log `0x40D46`) |
| `+0x78` | `0x4423C-0x44B38` | and `8` more (the network's `0x44B38`) |

Smaller shifts (`+2`, `+4`) are UI sizes and unrelated structures, not VanaCore's.

## What changed in VanaCore

- `scan::find` takes alternatives, `"A | B"`, and logs the first scan of FFXiMain.dll's build and every pattern that found
  nothing (`Signatures: ...` in `vanacore.log`), once each. `scan::operand(site, at)` reads a displacement back out of the
  code a pattern found. `sigs.hpp` knows the two builds above and holds the item data's shift.
- The four patterns above. `itemrarity.cpp` and `itemlist.cpp` add `sigs::item_data_shift()` to the item data's offsets.

## Writing a signature that survives a build

1. Leave `??` over what a build moves: a displacement, a limit, a jump's size, a register's encoding (`5D`/`55`/`7D`).
2. Read the number back from the code: `scan::operand(site, at)`. Don't write the number down.
3. Where the compiler laid the same code out two ways, give both: `"A | B"`.
4. Run the audit (below) against the build and read the top of the report.

## Running it

```
<xi-tools>\.venv\Scripts\python.exe tools\sigaudit.py --xi-tools <xi-tools> "<old>\FFXiMain.dll" "<new>\FFXiMain.dll"
```

About 8 seconds, reads two 3 MB files, writes the unpacked copies to `build\sigaudit\`. Run it again each time retail
patches, with the previous build as OLD. Read: "Found in old, not in new" (a feature loses its hook; fix the pattern),
"different function" (a pattern that now finds something else), then the changed functions (a field moved under a
pattern that still matches: check every offset the source adds to that match).

## What it can't see

- An offset the source adds to a match (`site + 0x1D`, `k...At`) is only checked by the function comparison: if that
  function is listed as changed, look at the instruction the offset lands on.
- A distance between two globals (`game.cpp` `kTargetPointer = 0x2F4`): nothing in the code names the second global, so
  this was **not verified for retail**. Check in game that the target reads.
- A hard-coded offset into a structure (`kEntitySize`, `entity_field`, party member `0x7C`): the entity fields checked
  (`0xD8`, `0x120`) are used by hundreds of unchanged instructions, so they didn't move; the item data did. Anything in
  the shifted ranges above is suspect. `kLegacyStride`/`kRetailStride` in `resources.cpp` (item records `0xC00`/`0x1400`) already know both.
- Code the game gets from a retail update later. Every retail patch can move these again; the audit is the check.

## Starting retail (`[client] retail=1`)

Done the way Windower (Fenestra, MIT) does it; its launcher and `core/src/hooks` were the model.

- **Where it is**: `HKLM\SOFTWARE\PlayOnline[US|EU]\InstallFolder` in the 32-bit view (JP's key is `PlayOnline`), value `1000`
  PlayOnline's folder, `0001` FFXI's, trailing `\`. Both must be there, with `pol.exe` in the first. No version value is
  there; it's `FFXiMain.dll`'s header (`sigs::build()`). `SquareEnix\FinalFantasyXI` under each has the settings (`0042` is
  the user's "My Games" folder).
- **How it starts** (`src/launcher/playonline.cpp`, `main.cpp` `retail_target`): `pol.exe /game eAZcFcB` (the argument turns on
  FFXI's quick start button) created suspended in its own folder, `VanaCore.Core.dll` loaded and `VanaCoreInstall` run, then
  resumed: the same injection as every other way in, no login, `[server]` or bootloader. FFXI loads in pol.exe's process.
- **Administrator rights**: pol.exe's manifest says `requireAdministrator` (checked in this machine's copy), so `CreateProcessW`
  from an ordinary VanaCore.exe fails with 740. `launch_failed` then runs VanaCore.exe again with `runas` and returns its exit
  code. Windower instead runs pol.exe as the invoking user (the `RUNASINVOKER` compatibility flag under
  `HKCU\...\AppCompatFlags\Layers`) and makes the folders writable, and answers the game's registry calls itself; not done here.
- **In the game** (`client.hpp`): `retail()` turns off `game_files` (the COM/folder redirect), registry's stand-in keys and
  defaults, and pivot's `catseyexi` overlay. `[ffxi]` and `[ffxi.registry]` still serve what they name, and the window mode is
  read from the real registry when `[ffxi] mode` isn't set.
- **Not done**: Steam launch (Windower starts the app by id), several clients (Windower appends the process id to every mutex
  name, VanaCore only makes FFXI's unnamed), blocking `hook.dll` (Windower returns null for it), `GetACP` 932.
