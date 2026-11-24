# FFXiMain.dll: every hook, patch and address VanaCore uses

Every place VanaCore changes or reads the game's own code in `FFXiMain.dll`, and how each one is found. The short
version: **almost nothing is at a fixed `FFXiMain.dll+0x...` address.** Each place is found when the game runs, by a
byte pattern (a signature) searched for in the loaded DLL. That is why the same `VanaCore.Core.dll` works on CatsEyeXI's
client and on retail's (`[client] retail=1`), two different builds. The one exception is the gamepad code
([Fixed addresses](#fixed-addresses-the-one-exception)).

Written 2026-11-24 from the source, offline: nothing here was run against a game. Companion to
[`retail-signatures.md`](retail-signatures.md), the audit of these patterns against retail's build. When you add, change or
remove a pattern, change its row here in the same commit.

- [How it works](#how-it-works): signature to hook, in five steps
- [How it survives retail](#how-it-survives-retail): what keeps a pattern working across builds
- [Watch list](#watch-list): what the audit can't check, and what could break on the next patch
- [Adding a signature](#adding-a-signature)
- [The catalogue](#the-catalogue): every feature, its patterns and what it does with them (132 sections, 457
  pattern rows: about 390 different patterns, some used by more than one feature or by Ashita's table too)

## How it works

### 1. The game's code is unpacked first

FFXiMain.dll ships packed: its code is LZSS-compressed in a `POL1` section and unpacked as the game loads. Until then a
scan finds nothing, so the features don't scan at DLL load. They scan on the first frame, the title screen or their first
use, and many retry each frame until the code is there.

### 2. `scan::find`: pattern to address

`src/core/scan.hpp`, `scan.cpp`:

```cpp
const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
const std::uintptr_t site = scan::find(game, "A1 ?? ?? ?? ?? 05 54 01 00 00 C3");
```

- A pattern is hex bytes. `??` (or `?`) matches any byte.
- `"A | B"` gives alternatives: the same code as two builds compiled it. The first alternative found anywhere wins.
- Only the module's **executable sections** are searched, and the **first match** is returned. Nothing checks that a
  pattern is unique, so a pattern that also matches something earlier in the code finds the wrong place. Keep patterns
  long enough to be unique.
- `scan::find(begin, size, pattern, count)` searches a range of readable memory instead, and can take the `count`th match.
  Several features use it to search inside a function they found already (`camera.cpp`'s key table, Ashita's spell list
  getter).
- The first scan of FFXiMain.dll logs its build, and every pattern that finds nothing is logged once (`sigs::note_scan`):

```
Signatures: FFXiMain.dll is retail's client, built 2026-09-03 (header 6A995428, image C15000)
Signatures: nothing in FFXiMain.dll matches "<the first 64 characters of the pattern>..."
```

So a miss is never silent, even where the feature logs nothing of its own. On a build VanaCore hasn't met, `vanacore.log`
lists every feature that lost its place.

### 3. Reading back what the pattern left open

A pattern leaves `??` over what a build moves: a global's address, a structure field's displacement, a call's target, a
loop's limit. The code reads the value back from the bytes it found, rather than having the number written into the source:

- `scan::operand(site, at)`: the 32 bits at `site + at`. This gives a global's address (`mov ecx,[global]` is `8B 0D` +
  address, so `operand(site, 2)`), a field's displacement, or an immediate.
- A call's target is `site + at + 5 + rel32`, worked out from the `E8` at `site + at`. Most code checks for the `E8`
  first.
- `game.cpp`'s `operand()` and Ashita compat's `code_dword()` are the same idea with a log line.

### 4. Changing the game

What a feature does with the address, from `src/core/imports.hpp` and the features themselves:

| Kind | How | Undone? |
|---|---|---|
| **Detour** | `imports::detour(fn, prologue, hook, original)`: the first `prologue` bytes (whole instructions, at least 5) become a `jmp` to VanaCore's hook; `original` is a trampoline that runs them and jumps back. The bytes are checked first. | Usually never: the hook passes straight to the game's function while its setting is off |
| **Jump** | `imports::jump(fn, replacement)`: 5 bytes, VanaCore's function stands in completely | Never |
| **Call rewrite** | An `E8 rel32` at a call site re-aimed at VanaCore's function (which often calls the old target) | Most are put back when the setting goes off |
| **Byte patch** | `VirtualProtect` and a write: a `jz` made `jmp` (`74`→`EB`), a call made `xor eax,eax` + `nop`s, a constant's operand pointed at VanaCore's own float, a limit changed | Most keep the original bytes and put them back |
| **Data write** | A table or global in the game's data (the font width table, the key table, a menu's handler pointer) | Varies; see the feature |
| **Read** | A global or structure read each frame (entities, party, target, chat, inventory) | n/a |
| **Import / vtable hook** | `imports::hook` (an import of FFXiMain.dll's, e.g. `GetSystemMetrics`), `imports::hook_vtable` (the game's D3D8 or DirectInput objects) | Found by name or slot, never by pattern |

### 5. The log names the address

Each feature that finds its place logs it as `FFXiMain.dll+0x...` (e.g. `Camera: the camera follow at FFXiMain.dll+0x... goes with
your character at once`). That is the address on the build running now. The `CatsEyeXI address` lines below
are only what the source's comments recorded for the 2025-12-26 CatsEyeXI build.

To get from one form to another:

- **Ghidra:** the image base is `0x10000000`, so `FUN_1015a2d0` is `FFXiMain.dll+0x15A2D0`.
- **In game:** the MCP tools (`read_memory`, `disassemble`, `find_pattern`) take expressions like
  `[[FFXiMain.dll+0x4A1234]+0x10]+4`.
- **Lua:** `vana.memory.find('FFXiMain.dll', 0, pattern[, offset[, count]])` searches the module's whole image with the
  same pattern syntax, from a script ([`addons/11-memory-and-ffi.md`](addons/11-memory-and-ffi.md)).

## How it survives retail

Retail's FFXiMain.dll (built 2026-09-03) is newer than CatsEyeXI's (2025-12-26). The code is mostly the same byte for
byte. What moved is where fields sit in the game's big objects, and which register the compiler picked in places.
[`retail-signatures.md`](retail-signatures.md) has the numbers. Five things keep VanaCore working on both:

1. **Wildcards over what moves.** Every address, call target and most displacements are `??`. A pattern names an
   instruction sequence, not a place.
2. **Read back, don't write down.** The value under a `??` is read from the code by `scan::operand` (above). Retail's
   network window moved its field from `+0x4630` to `+0x4638`, and `netstat.cpp` was already reading it from the
   instruction, so it kept working.
3. **Alternatives (`|`) where the compiler differs.** `camera.cpp`'s `kKeyTable` is `66 8B 04 5D` in one build and
   `66 8B 04 55` in the other (`ebx` vs `edx`), with `test ax,ax` moved. Both layouts are listed.
4. **Shifts read from the game's own code.** `sigs::item_data_shift()` (`src/core/sigs.cpp`) finds the item icon
   drawing, reads the icon field's displacement (`0xD0C` on CatsEyeXI, `0x150C` on retail) and returns the difference
   (`+0x800`). `itemrarity.cpp` and `itemlist.cpp` add it to every item data field they use. `resources.cpp` knows both
   item record sizes (`0xC00`/`0x1400`).
5. **Knowing the build.** `sigs::build(game)` reads FFXiMain.dll's header (`TimeDateStamp`, `SizeOfImage`) and names the
   two builds the audit checked. Nothing switches on it: it's for the log.

The audit (`tools\sigaudit.py`, 2026-10-31) unpacked both builds and searched every pattern in the source. 264 were found
in both. Four were in CatsEyeXI's only, and all four were fixed: `kKeyTable` (camera), Ashita's `packets.queuepacket1`
(the packet id limit is `??` now), `kDrawIcon` (item rarity, now from `item_data_shift`) and `kNetDraw` (network window).
The rest weren't FFXiMain code. So, unless a section below says otherwise, **a pattern that was in the source on
2026-10-31 matched retail's build.**

Two limits on that:

- **Patterns added since haven't been audited.** The history before 0.9.0 (2026-11-21) is squashed, so git can't date
  each pattern. These are certainly newer: `soundmix.cpp` (`kUiPlay`, `kMenuSound`, `kWorldKept`, `kWorldEvent`,
  `kWorldVolume`), `soundslots.cpp` (`kSetup`, `kClear`, `kThread`, `kCap`) and `gamesettings.cpp`'s `kEntry`. The
  catalogue notes others whose comments are dated after the audit (`gamesave.cpp`, `vanaclock.cpp`, `equipfirst.cpp`).
  Run the audit again before saying they work in retail.
- **`tools\sigaudit.py` isn't in the repository.** It is referenced from `sigs.hpp` and `retail-signatures.md` but was
  never committed, so only the person who has it can run the audit. It belongs under `tools\`.

## Watch list

These are what a pattern match doesn't prove. The audit only checks that each pattern is still found. The function
comparison catches some of the rest, but only if someone reads the changed functions. Each item is also flagged in its
feature's section below.

### Fixed addresses: the one exception

`src/core/input.cpp` and `input_background.hpp` (the gamepad) use **fixed RVAs** in FFXiMain.dll, not signatures:

| Address | What | Guard |
|---|---|---|
| `+0x8990` | IsXInputDevice (the slow WMI query), detoured so it runs once per product | its first 17 bytes, and the calls at `+0x580D`, `+0x587A` must be `E8` to it |
| `+0x55A0` | set the pad's GUID, called directly | its first 12 bytes and `C2 14 00` at `+0x4E` |
| `+0x5490` | pad mode setter (its `A0` operand also gives the gamepad and XInput switch bytes) | head `83 EC 20 A0`, `C2 0C 00` at `+0x106` |
| `+0x5130`, `+0x5320` | pad button setters | head `8B 54 24 04`, `C2 08 00` at `+0x1ED` / `+0x162` |
| `+0x5600` | the "works while the game is inactive" switch's code; seven operands read from it | head `8A 44 24 04 33 D2 3A C2 A0`, the operands must agree and be inside the image |

Each is checked before use, and on another layout the log says so (`this client's pad code isn't the one it was written
for`) and the gamepad settings do nothing. None of them is in the audit's pattern count. Whether they hold in retail is
**not known**. They're early in `.text`, where little changed, but that has not been checked. Turning them into
patterns would let the audit check them.

### Values read from past a pattern's end

The pattern matches, but the operand it reads is somewhere the pattern doesn't cover. If the code between moves, the
value is wrong and nothing notices:

- `game.cpp` (the core data everything uses): `kPartyOffset` 23 (the pattern is 13 bytes), `kTargetOffset` 45 (21
  bytes), `kPetOffset` 61 (21 bytes).
- `compat/ashita/memory.cpp`: key items +22, auto-follow +25, pet +61, ability recasts +25, spell recasts end +0x24.
- `chatfont.cpp` `kPrintString`: calls at +0x50, +0x93, +0x9E. `itemcount.cpp`: +0xCF, +0x93, +0x9E. `itemslot.cpp`: +0x3D.
  `helpbar.cpp` `kLabelCall`: 128. These at least check for an `E8` before using it.

### Fixed distances between places

- `game.cpp` `kTargetPointer = 0x2F4`: the target's pointer is 0x2F4 bytes after the global the pattern finds. No code
  names the second global, so the audit couldn't check it. **Not verified for retail**: check in game that the target
  reads.
- `chat.cpp`: the cancel key check must be exactly `0x521` after the chat calc. The chat history hooks sit at -0x70,
  +0x240, +0x110 and +0x90 from one match.
- `camera.cpp` `kSites`: 30-odd sites from -0x740 to +0xCA6 around the zoom code. All of them are checked (opcode bytes,
  `E8`, constants) before anything is written.

### Structure fields written into a pattern

The pattern only matches while the field stays where it is. If it moves, the feature stops, with a log line:

- `chat.cpp` chat input line `89 9E 04 F1 00 00` (`+0xF104`); `autologin.cpp` `kYesNo` (`+0xA5E1`, `+0xA5E0`);
  `packetflow.cpp` `kSendCheck` (`+0x417C`).
- `hudwin.cpp` `kTitleOpened`, `kGaugeOpened`, `kLinkOpened`, `kCheckCursor`; `gamewin.cpp` `kSetCursor`, `kKeyInput`;
  `gameui.cpp`'s dispatch; `tipicons.cpp` `kDetailsDraw`; every `profilewin.cpp` site.
- The renderer's `0x39xxx` displacements in `bumpmap.cpp`, `casters.cpp` (`kSlotEnd`) and `distance.cpp` (`kViewList`).
- `chattabs.cpp` `kMade` has `3B C3` / `3B CB` (`cmp eax,ebx` / `cmp ecx,ebx`), which depends on the compiler's choice of
  register.

### Structure layouts used as they are

Fields VanaCore reads or writes at fixed offsets, not read from code:

- The entity (`+0x120`, `+0x124`, `+0x128`, `+0x12C`, `+0x130`, `+0x1D4` ...).
- Party members (stride `0x7C`).
- The chat manager (`+0xF108`, `+0x7E30` + `0x6C4` / `0x888`).
- The game's options (`game_config.cpp`: table at `+4`, entries `0x2C`, `0x2394` long).
- `_canUseMacro`'s object (`macrobar.cpp`, ~15 fields).

The audit found entity fields `0xD8` and `0x120` unchanged in retail. The others weren't checked, and anything in the
shifted ranges in `retail-signatures.md` (`+0x8` from `0x4630`, `+0x70` from `0x2D60C`, `+0x78` from `0x4423C` in the
game's big object) is suspect.

### Changes made whatever the settings

Not a retail risk, but worth knowing when something odd turns up in game:

- `camera.cpp`: the mouse wheel's two menu checks are patched always and never undone. `home.reset` defaults to 0, so
  the Home entry in the game's key table is cleared by default. `mouse.noleftdrag` defaults to 1.
- `framerate.cpp`: the game's frame wait patch and the frame rate setter detour are never undone.
- `chatpages.cpp`: the 8 pager functions are replaced always, whatever `chat.scrollback` is.
- `menulog.cpp`: its SetCurMCD detour goes in always, recording or not.
- `hudwin.cpp`: most hooks go in at the first window drawn.

## Adding a signature

1. Find the code in Ghidra (the unpacked client, base `0x10000000`) and take enough bytes to be unique. `find_pattern`
   in game tells you how many matches it has.
2. `??` over every address, call target and displacement; over a register's ModRM byte if the compiler might pick
   another; over limits and sizes the game might raise.
3. Read what you need back with `scan::operand` (or the call's target), never a number from Ghidra. If you must reach
   past the pattern's end, check the opcode there first (`E8`, `8B 0D`...).
4. If not found: log one line saying which setting does nothing (`Chat: FFXI's ... wasn't found in FFXiMain.dll:
   [client] ... does nothing`), and leave the game alone.
5. Log the address found, as `FFXiMain.dll+0x%X`.
6. Add it to this document, under its feature, and run the audit against both builds if you have it.

## The catalogue

One section per feature, by area. In each section:

- **Setting** is what turns the feature on.
- **What it does** is the change made to the game.
- The table lists each pattern: its name and line in the source, what it finds, the pattern itself, and what's done with
  the match.
- **CatsEyeXI address** gives the addresses the source records for the 2025-12-26 build. Usually there are none: the
  log has them at run time.
- **Retail** says how the pattern copes with other builds, and what's fragile.

A pattern used by several files (`kHelp`, `kSetLocate`, `kLanguage`, `kShapeScaled`, `kDrawCall`, `kResize`) has a row
under each.

**[Graphics and camera](#graphics-and-camera)**

| Feature | Patterns |
|---|---|
| [True Zoom: the camera's zoom (`src/core/camera.cpp`)](#true-zoom-the-cameras-zoom-srccorecameracpp) | 1 |
| [Mouse wheel works with a menu up (`src/core/camera.cpp`)](#mouse-wheel-works-with-a-menu-up-srccorecameracpp) | 2 |
| [Home key and Change View button (`src/core/camera.cpp`)](#home-key-and-change-view-button-srccorecameracpp) | 2 |
| [Camera offset, walls and free camera (`src/core/camera.cpp`, `src/core/camera_walls.h`)](#camera-offset-walls-and-free-camera-srccorecameracpp-srccorecamera_wallsh) | 4 |
| [The game's own camera wall checks follow the offset (`src/core/camera_walls.h`)](#the-games-own-camera-wall-checks-follow-the-offset-srccorecamera_wallsh) | 7 |
| [Camera lock and swing behind (`src/core/camera.cpp`)](#camera-lock-and-swing-behind-srccorecameracpp) | 2 |
| [Mouse edge turn and left-drag turn (`src/core/camera.cpp`)](#mouse-edge-turn-and-left-drag-turn-srccorecameracpp) | 2 |
| [Free look (`/freelook`) (`src/core/camera.cpp`)](#free-look-freelook-srccorecameracpp) | 1 |
| [Draw distance (`src/core/distance.cpp`)](#draw-distance-srccoredistancecpp) | 1 |
| [Fog (`src/core/distance.cpp`)](#fog-srccoredistancecpp) | 1 |
| [Target check for far characters (`src/core/distance.cpp`)](#target-check-for-far-characters-srccoredistancecpp) | 2 |
| [Zone view lists, quadtree and far copies (`src/core/distance.cpp`)](#zone-view-lists-quadtree-and-far-copies-srccoredistancecpp) | 3 |
| [Zone object detail (`src/core/lod.cpp`)](#zone-object-detail-srccorelodcpp) | 2 |
| [Zone draws sent as batches (`src/core/zonebatch.cpp`)](#zone-draws-sent-as-batches-srccorezonebatchcpp) | 1 |
| [Water models known as they load (`src/core/water.cpp`)](#water-models-known-as-they-load-srccorewatercpp) | 1 |
| [Bump mapping on/off (`src/core/bumpmap.cpp`)](#bump-mapping-onoff-srccorebumpmapcpp) | 4 |
| [Sun shadow caster pass (`src/core/casters.cpp`)](#sun-shadow-caster-pass-srccorecasterscpp) | 3 |
| [3D aspect ratio (`src/core/graphics.cpp`)](#3d-aspect-ratio-srccoregraphicscpp) | 1 |
| [No-blink gear changes (`src/core/noblink.cpp`)](#no-blink-gear-changes-srccorenoblinkcpp) | 2 |
| [Changes to game memory and calls not found by a pattern, and hard-coded structure offsets](#changes-to-game-memory-and-calls-not-found-by-a-pattern-and-hard-coded-structure-offsets) | none |

**[Names and damage numbers](#names-and-damage-numbers)**

| Feature | Patterns |
|---|---|
| [Name size and letter spacing: the factors (`src/core/nameplates.cpp`)](#name-size-and-letter-spacing-the-factors-srccorenameplatescpp) | 1 |
| [Letter hook (`nameplates.font=sharp`, `spaced`, `centre`, pearl/bazaar moves) (`src/core/nameplates.cpp`)](#letter-hook-nameplatesfontsharp-spaced-centre-pearlbazaar-moves-srccorenameplatescpp) | 1 |
| [Sharp (True Font) names: the letters' quad draw (`src/core/nameplates.cpp`)](#sharp-true-font-names-the-letters-quad-draw-srccorenameplatescpp) | 4 |
| [Name call, line under the name, own entity (`src/core/nameplates.cpp`)](#name-call-line-under-the-name-own-entity-srccorenameplatescpp) | 3 |
| [Smooth name size (`src/core/nameplates.cpp`)](#smooth-name-size-srccorenameplatescpp) | 2 |
| [Names drawn far to near (`src/core/nameplates.cpp`)](#names-drawn-far-to-near-srccorenameplatescpp) | 1 |
| [Blink off for the target's name (`src/core/nameplates.cpp`)](#blink-off-for-the-targets-name-srccorenameplatescpp) | 1 |
| [Hard-coded game structure offsets and non-pattern code (`src/core/nameplates.cpp`, `gmicons.cpp`)](#hard-coded-game-structure-offsets-and-non-pattern-code-srccorenameplatescpp-gmiconscpp) | none |
| [Damage numbers: layout, glyphs and draw (`src/core/damagetext.cpp`)](#damage-numbers-layout-glyphs-and-draw-srccoredamagetextcpp) | 5 |
| [Damage number lifetime (`src/core/damagetext_life.h`, included by `damagetext.cpp`)](#damage-number-lifetime-srccoredamagetext_lifeh-included-by-damagetextcpp) | 2 |
| [Damage numbers: other hard-coded game offsets and non-pattern game access (`src/core/damagetext.cpp`, `damagetext_look.h`)](#damage-numbers-other-hard-coded-game-offsets-and-non-pattern-game-access-srccoredamagetextcpp-damagetext_lookh) | none |

**[Frame rate, sound and game data](#frame-rate-sound-and-game-data)**

| Feature | Patterns |
|---|---|
| [Frame rate cap (`src/core/framerate.cpp`)](#frame-rate-cap-srccoreframeratecpp) | 2 |
| [Frame pacing (`src/core/framepacing.cpp`)](#frame-pacing-srccoreframepacingcpp) | 10 |
| [Frame timing log and recording (`src/core/frametime.cpp`)](#frame-timing-log-and-recording-srccoreframetimecpp) | none |
| [World and interface sound volume (`src/core/soundmix.cpp`)](#world-and-interface-sound-volume-srccoresoundmixcpp) | 5 |
| [Sound slots (`src/core/soundslots.cpp`)](#sound-slots-srccoresoundslotscpp) | 6 |
| [Vana'diel clock hold (`src/core/vanaclock.cpp`)](#vanadiel-clock-hold-srccorevanaclockcpp) | 2 |
| [Game data: entities, party, player, target, pet, missions, job levels (`src/core/game.cpp`)](#game-data-entities-party-player-target-pet-missions-job-levels-srccoregamecpp) | 9 |
| [Game config watch and shadow setter (`src/core/game_config.cpp`)](#game-config-watch-and-shadow-setter-srccoregame_configcpp) | 2 |
| [Game save (`src/core/gamesave.cpp`)](#game-save-srccoregamesavecpp) | 1 |
| [Game events: menu windows (`src/core/gameevents.cpp`)](#game-events-menu-windows-srccoregameeventscpp) | 1 |
| [Config > Gameplay window extension (`src/core/gamesettings.cpp`)](#config--gameplay-window-extension-srccoregamesettingscpp) | 1 |

**[Chat](#chat)**

| Feature | Patterns |
|---|---|
| [Command line, chat output and chat line (`src/core/chat.cpp`)](#command-line-chat-output-and-chat-line-srccorechatcpp) | 4 |
| [Row spacing (`src/core/chat.cpp`)](#row-spacing-srccorechatcpp) | 5 |
| [Timestamp colour (`src/core/chat.cpp`)](#timestamp-colour-srccorechatcpp) | 1 |
| [No hanging indent (`src/core/chat.cpp`)](#no-hanging-indent-srccorechatcpp) | 1 |
| [Chat window opacity tracking (`src/core/chat.cpp`)](#chat-window-opacity-tracking-srccorechatcpp) | 1 |
| [Chat window position and width (`src/core/chat.cpp`)](#chat-window-position-and-width-srccorechatcpp) | 3 |
| [Second chat window follows the first (`src/core/chat.cpp`)](#second-chat-window-follows-the-first-srccorechatcpp) | 1 |
| [Compass position (`src/core/chat.cpp`)](#compass-position-srccorechatcpp) | 2 |
| [Compass hide (`src/core/chat.cpp`)](#compass-hide-srccorechatcpp) | 1 |
| [Clock position and hiding (`src/core/chat.cpp`)](#clock-position-and-hiding-srccorechatcpp) | 1 |
| [Clock kept while a menu is up (`src/core/chat_clock.h`)](#clock-kept-while-a-menu-is-up-srccorechat_clockh) | 1 |
| [Hide the chat line's mode tab (`src/core/chat.cpp`)](#hide-the-chat-lines-mode-tab-srccorechatcpp) | 1 |
| [Menus over the chat (chat.menus, menus.move) (`src/core/chat.cpp`)](#menus-over-the-chat-chatmenus-menusmove-srccorechatcpp) | 2 |
| [Chat line position and hide (`src/core/chat.cpp`)](#chat-line-position-and-hide-srccorechatcpp) | 2 |
| [Chat window hide and Escape pin (`src/core/chat.cpp`)](#chat-window-hide-and-escape-pin-srccorechatcpp) | 2 |
| [Chat line history (`src/core/chat.cpp`)](#chat-line-history-srccorechatcpp) | 1 |
| [Chat window scrollback (`src/core/chat.cpp`)](#chat-window-scrollback-srccorechatcpp) | 4 |
| [Selection menu rows (`src/core/chat.cpp`)](#selection-menu-rows-srccorechatcpp) | 16 |

**[Fonts and text](#fonts-and-text)**

| Feature | Patterns |
|---|---|
| [True Font: widths, chat rows and character reading (`src/core/chatfont.cpp`, `src/core/chatfont_wide.h`)](#true-font-widths-chat-rows-and-character-reading-srccorechatfontcpp-srccorechatfont_wideh) | 4 |
| [True Font: menu, labels, help line and tooltip hooks (`src/core/chatfont.cpp`)](#true-font-menu-labels-help-line-and-tooltip-hooks-srccorechatfontcpp) | 8 |
| [Chat line draw (`src/core/chatline.cpp`)](#chat-line-draw-srccorechatlinecpp) | 3 |
| [Chat history pages (`src/core/chatpages.cpp`)](#chat-history-pages-srccorechatpagescpp) | 10 |
| [Chat log tabs (`src/core/chattabs.cpp`)](#chat-log-tabs-srccorechattabscpp) | 3 |
| [Craft log (`src/core/craftlog.cpp`)](#craft-log-srccorecraftlogcpp) | 1 |
| [Auto-translate phrase list (`src/core/phraselist.cpp`)](#auto-translate-phrase-list-srccorephraselistcpp) | 1 |
| [Macro bar names (`src/core/macrotext.cpp`)](#macro-bar-names-srccoremacrotextcpp) | 1 |
| [Extended macro book and equip set names (`src/core/namechars.cpp`)](#extended-macro-book-and-equip-set-names-srccorenamecharscpp) | 3 |
| [Japanese menu labels (`src/core/jplabels.cpp`)](#japanese-menu-labels-srccorejplabelscpp) | 1 |
| [Help bar (`src/core/helpbar.cpp`)](#help-bar-srccorehelpbarcpp) | 1 |
| [Changes to game code not from a pattern](#changes-to-game-code-not-from-a-pattern) | none |

**[Items](#items)**

| Feature | Patterns |
|---|---|
| [More rows in the item and chat lists (`src/core/itemlist.cpp`)](#more-rows-in-the-item-and-chat-lists-srccoreitemlistcpp) | 16 |
| [Item sort (`src/core/itemlist.cpp`)](#item-sort-srccoreitemlistcpp) | 3 |
| [Shop purchase total in chat (`src/core/itemlist.cpp`)](#shop-purchase-total-in-chat-srccoreitemlistcpp) | 2 |
| [Item rarity: icon boxes, names, equipped colour (`src/core/itemrarity.cpp`)](#item-rarity-icon-boxes-names-equipped-colour-srccoreitemraritycpp) | 4 |
| [Bag count on the title bar (`src/core/itemcount.cpp`)](#bag-count-on-the-title-bar-srccoreitemcountcpp) | 3 |
| [Item slot squares and Equipment wardrobe numbers (`src/core/itemslot.cpp`)](#item-slot-squares-and-equipment-wardrobe-numbers-srccoreitemslotcpp) | 2 |
| [Item details on one page (`src/core/itemtips.cpp`)](#item-details-on-one-page-srccoreitemtipscpp) | 5 |
| [Game code changes not from a pattern, and where `item_data_shift` is used](#game-code-changes-not-from-a-pattern-and-where-item_data_shift-is-used) | none |

**[Equipment and HUD windows](#equipment-and-hud-windows)**

| Feature | Patterns |
|---|---|
| [Equip sets: Equip first (`src/core/equipfirst.cpp`)](#equip-sets-equip-first-srccoreequipfirstcpp) | 3 |
| [Equipment window stats (`src/core/equipstats.cpp`)](#equipment-window-stats-srccoreequipstatscpp) | 7 |
| [Check window stats (`src/core/equipstats_check.h`, included by `equipstats.cpp`)](#check-window-stats-srccoreequipstats_checkh-included-by-equipstatscpp) | 1 |
| [Gear parts that fail to load, logged (`src/core/gearload.cpp`)](#gear-parts-that-fail-to-load-logged-srccoregearloadcpp) | 2 |
| [HUD window placing and Menu Content (`src/core/hudwin.cpp`)](#hud-window-placing-and-menu-content-srccorehudwincpp) | 4 |
| [HUD window draw, destroy and background removal (`src/core/hudwin.cpp`)](#hud-window-draw-destroy-and-background-removal-srccorehudwincpp) | 2 |
| [Quests window button layout (`src/core/hudwin.cpp`)](#quests-window-button-layout-srccorehudwincpp) | 5 |
| [Handlers that cache a window's rect (`src/core/hudwin.cpp`)](#handlers-that-cache-a-windows-rect-srccorehudwincpp) | 5 |
| [Check window comment box kept up (`src/core/hudwin_comment.h`, included by `hudwin.cpp`)](#check-window-comment-box-kept-up-srccorehudwin_commenth-included-by-hudwincpp) | 2 |
| [Changes to game code and data not from a pattern (this set of files)](#changes-to-game-code-and-data-not-from-a-pattern-this-set-of-files) | none |

**[Macros, map and menus](#macros-map-and-menus)**

| Feature | Patterns |
|---|---|
| [Macro bar key modes (`src/core/macrobar.cpp`)](#macro-bar-key-modes-srccoremacrobarcpp) | 7 |
| [Macro bar position (`src/core/macrobar.cpp`)](#macro-bar-position-srccoremacrobarcpp) | 3 |
| [Macro bar "why isn't it up" log (`src/core/macrobar.cpp`)](#macro-bar-why-isnt-it-up-log-srccoremacrobarcpp) | 2 |
| [Map window (`src/core/mapwin.cpp`)](#map-window-srccoremapwincpp) | 10 |
| [Single main menu (`src/core/singlemenu.cpp`)](#single-main-menu-srccoresinglemenucpp) | 13 |
| [Menu hover (`src/core/menuhover.cpp`)](#menu-hover-srccoremenuhovercpp) | 3 |
| [Menu placing helper (`src/core/menuplace.h`)](#menu-placing-helper-srccoremenuplaceh) | 2 |
| [Menu focus log (`src/core/menulog.cpp`)](#menu-focus-log-srccoremenulogcpp) | 1 |

**[Interface windows](#interface-windows)**

| Feature | Patterns |
|---|---|
| [Config menu item (`src/core/configmenu.cpp`)](#config-menu-item-srccoreconfigmenucpp) | 9 |
| [Status window layout (`src/core/statuswindow.cpp`)](#status-window-layout-srccorestatuswindowcpp) | 3 |
| [Status window menu font (`src/core/statuswindow.cpp`)](#status-window-menu-font-srccorestatuswindowcpp) | 2 |
| [Status window taller (`src/core/statuswindow.cpp`)](#status-window-taller-srccorestatuswindowcpp) | 2 |
| [Level / EXP window (`src/core/statuswindow.cpp`)](#level--exp-window-srccorestatuswindowcpp) | 1 |
| [Target window (`src/core/targetwin.cpp`)](#target-window-srccoretargetwincpp) | 3 |
| [Interface size (`src/core/uiscale.cpp`)](#interface-size-srccoreuiscalecpp) | 9 |
| [Interface area (`src/core/uispace.cpp`)](#interface-area-srccoreuispacecpp) | 1 |
| [Hide UI (`src/core/hideui.cpp`)](#hide-ui-srccorehideuicpp) | 2 |
| [Camera with the interface hidden, and hidden cursor (`src/core/hideui.cpp`)](#camera-with-the-interface-hidden-and-hidden-cursor-srccorehideuicpp) | 2 |
| [Profile window clean layout (`src/core/profilewin.cpp`)](#profile-window-clean-layout-srccoreprofilewincpp) | 12 |
| [Craft Skills brackets (`src/core/profilewin.cpp`)](#craft-skills-brackets-srccoreprofilewincpp) | 1 |
| [Item details pictures and type row (`src/core/tipicons.cpp`)](#item-details-pictures-and-type-row-srccoretipiconscpp) | 2 |
| [Game window reading (`src/core/gamewin.cpp`)](#game-window-reading-srccoregamewincpp) | 4 |
| [Game UI input dispatch hook (`src/core/gameui.cpp`)](#game-ui-input-dispatch-hook-srccoregameuicpp) | 1 |

**[Party, buttons, network, login and compat](#party-buttons-network-login-and-compat)**

| Feature | Patterns |
|---|---|
| [Buff icons: scale, place, columns (`src/core/buffwin.cpp`)](#buff-icons-scale-place-columns-srccorebuffwincpp) | 8 |
| [Button style, selected-button nudge and flash (`src/core/buttons.cpp`)](#button-style-selected-button-nudge-and-flash-srccorebuttonscpp) | 6 |
| [Button spacing (`src/core/buttonspace.cpp`)](#button-spacing-srccorebuttonspacecpp) | 1 |
| [Alliance lists and the macro bar; party list hide and background (`src/core/alliancelist.cpp`)](#alliance-lists-and-the-macro-bar-party-list-hide-and-background-srccorealliancelistcpp) | 1 |
| [Party list names and numbers: colours and upper case (`src/core/partylist.cpp`)](#party-list-names-and-numbers-colours-and-upper-case-srccorepartylistcpp) | 13 |
| [Party member TP bar (`src/core/tpbar.cpp`)](#party-member-tp-bar-srccoretpbarcpp) | 5 |
| [Party menu: Find Member opens Party Finder (`src/core/partymenu.cpp`)](#party-menu-find-member-opens-party-finder-srccorepartymenucpp) | 1 |
| [Magic list A-Z order (`src/core/spelllist.cpp`)](#magic-list-a-z-order-srccorespelllistcpp) | 3 |
| [Fishing: D-pad fights the fish (`src/core/fishing.cpp`)](#fishing-d-pad-fights-the-fish-srccorefishingcpp) | 3 |
| [Network window text and layout (`src/core/netstat.cpp`)](#network-window-text-and-layout-srccorenetstatcpp) | 3 |
| [Auto login (`src/core/autologin.cpp`)](#auto-login-srccoreautologincpp) | 4 |
| [Packet hooks for plugins (`src/core/packets.cpp`)](#packet-hooks-for-plugins-srccorepacketscpp) | 2 |
| [Packet flow: shorter send wait (`src/core/packetflow.cpp`)](#packet-flow-shorter-send-wait-srccorepacketflowcpp) | 1 |
| [Game start without PlayOnline: polcore patches (`src/core/loader.cpp`)](#game-start-without-playonline-polcore-patches-srccoreloadercpp) | 4 |
| [Ashita compat: the pointer table (`src/core/compat/ashita/core.cpp`)](#ashita-compat-the-pointer-table-srccorecompatashitacorecpp) | 36 |
| [Ashita compat: game data objects (`src/core/compat/ashita/memory.cpp`)](#ashita-compat-game-data-objects-srccorecompatashitamemorycpp) | 15 |
| [Gamepad: fixed addresses in FFXiMain (`src/core/input.cpp`, with `input_background.hpp`)](#gamepad-fixed-addresses-in-ffximain-srccoreinputcpp-with-input_backgroundhpp) | 6 |
| [Window frame and mouse (`src/core/window.cpp`)](#window-frame-and-mouse-srccorewindowcpp) | none |
| [Lua `vana.memory` (`src/core/lua_api.cpp`)](#lua-vanamemory-srccorelua_apicpp) | none |

### Graphics and camera

#### True Zoom: the camera's zoom (`src/core/camera.cpp`)

**Setting:** `[camera] zoom=distance` (also `distance`, `distance.min`, `distance.max`, `turn`, `turn.vertical`, `battle.distance`, `battle.range`, `fov`, `zoom.speed`). With `zoom=game` (default) none of this is applied, except that `find()` still runs once at the title screen to read the game's turning speeds.
**What it does:** Finds the game's camera function (FUN_1001ec70) by the zoom code in it, then, only while the setting is on and a character is in the world, writes a `jmp` over the zoom's first 5 bytes to a stub (`zoom_free`), a `call` over the locked-on focal-length call (`kLockedOn` = `+0xC57`) to `locked_scrz`, and rewrites the operand of 16 `fmul/fld/fcomp/fsub/fdiv [float]` instructions (the `kSites` table) to point at VanaCore's own floats (the game's 6.0, 3.0, 100, 0.012, -0.125, turning speeds, 4.0 lock-on range). The probe push (`68 66 66 40 40`, 3.1f, at `+0x27F`) is also rewritten. All of it is put back (saved bytes) when turned off. `[camera] zoom.speed` separately rewrites the operand at four `fmul [6.0]` sites (`kRateSites`) while it is not 1.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kZoom` (`camera.cpp:50`) | the zoom block in the camera function: `mov al,[resetting]; test al,al; jz +67h; call scrz; fsubr [350]; fmul [0.25]; fstp [esp+10h]; fld [-1]` | `A0 ?? ?? ?? ?? 84 C0 74 67 E8 ?? ?? ?? ?? D8 2D ?? ?? ?? ?? D8 0D ?? ?? ?? ?? D9 5C 24 10 D9 05` | `zoom` = match. Hard-coded offsets from it, all checked by `find()` (0xE8 / opcode bytes / constant): calls at `+0x9` (scrz getter; its bytes `A1 .. D9 80 F4 02 00 00 C3` are compared, `ymdb` = operand at getter+1), `+0x78` (key), `+0xC1` (ticks), `+0x139` (clear wheel), `+0x148` (mouse), `+0x155` (wheel), `+0x1E1` (set scrz), `+0x1F2` (set config), `+0xC57` (locked-on call); `kAfterZoom` = `+0x1FA` (stub returns here); `+0x27F` probe push; `+0x660` `89 1D` (operand = the `controlled` global); `kRateSites` = `+0xC8, +0x118, +0x16E, +0x1B6`; `kSites` operands at `-0x740, -0x6DC, -0x66D, -0x574, +0x257, +0x597, +0x5B4, +0x5C6, +0x5D9, +0x5E0, +0x61D, +0x62A, +0x639, +0x698, +0x6AF, +0xCA6`, each with its two opcode bytes checked; 100, 0.012 and -0.125 compared with the float they point at |

**CatsEyeXI address:** the camera function is FUN_1001ec70 = `FFXiMain.dll+0x1EC70` (source comment, `camera.cpp:32`); the zoom block's own address is not recorded.
**Retail:** pattern has `??` only over the operands; nothing in `docs/retail-signatures.md` lists it as moved, so it matches retail. Fragile: the 30-odd offsets from the match (up to `-0x740` / `+0xCA6` away) assume the camera function's layout is identical; they are all verified at runtime (opcode bytes, `E8`, constants), and any failure makes `find()` return false (log: "Camera: couldn't find the game's zoom in FFXiMain.dll"), so zoom=distance does nothing rather than corrupting. `kGame` constants (6, 3, 100, 0.012, -0.125, 0.1067, 0.03, 4) are the CatsEyeXI values; only Far/Pull/Push are checked against the game's floats. Second `zoom.speed` log: "the game's zoom speed wasn't found".

#### Mouse wheel works with a menu up (`src/core/camera.cpp`)

**Setting:** always (runs once from `camera::on_frame`, not tied to any setting).
**What it does:** The game's two wheel readers (DirectInput one and window-message one) only count a wheel notch when no menu is current. The `call IsMenuOpen` after the DirectInput one is overwritten with `31 C0 90 90 90` (`xor eax,eax; nop x3`); the one in the message reader is redirected to `wheel_menu_open` (true only when a menu is open and the pointer is over a VanaCore/game menu window). Not undone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kWheelCalls[0]` (`camera.cpp:685`) | the DirectInput wheel reader's `mov ecx,[menus]; call IsMenuOpen` | `B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 44 24 0C 8B 4C 24 08` | call at `match+5` replaced by `31 C0 90 90 90`; its target is checked to start `8B 41 54 85 C0` (`kMenuOpenCode`) |
| `kWheelCalls[1]` (`camera.cpp:686`) | the window-message wheel reader's same call | `B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F BF 4C 24 0C 84 C0 74` | call at `match+5` rewritten (`E8` rel32) to `wheel_menu_open`; both calls must resolve to the same `IsMenuOpen` |

**CatsEyeXI address:** the log prints the menu check's `FFXiMain.dll+0x...` at run time; not recorded in source.
**Retail:** `??` over the global and call offsets only; not in docs' list of changes. Either pattern missing: nothing patched, log "the game's wheel code wasn't found; the wheel still doesn't zoom while engaged".

#### Home key and Change View button (`src/core/camera.cpp`)

**Setting:** `[camera] home.reset` (default 0), `[camera] view.reset` (default 0).
**What it does:** The game's key function (found through the camera's "behind the character" call) reads a key table, three keys to a game key; entry for game key 0x51 is Home (`0x80C7`, DIK_HOME). `home.reset=0` (the default) writes 0 over that table entry, so Home no longer turns the camera behind the character; `home.reset=1` writes `0x80C7` back. `view.reset=1` (and a character in the world) rewrites the `call key` at `kBehind+8` to `behind_key`, which also reports key 0x51 held while the controller's Change View button turns the camera; undone (original call) when off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kBehind` (`camera.cpp:123`) | the camera code's `push -1; push 4; push 51h; push 3Fh; call key; ...; mov [mode],8` | `6A FF 6A 04 6A 51 6A 3F E8 ?? ?? ?? ?? 83 C4 10 84 C0 0F 84 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 08 00 00 00` | call at `match+8` = `g_behind_site` (rewritten for view.reset); its target is the game's key function, in which `kKeyTable` is searched (`scan::find(key, 0x200, ...)`, a range scan, not whole module) |
| `kKeyTable` (`camera.cpp:126`) | in the key function: `mov ax,[reg*2+table]; test ax,ax; lea esi,...` | `66 8B 04 ?? ?? ?? ?? ?? 66 85 C0 8D 34 ?? \| 66 8B 04 ?? ?? ?? ?? ?? 8D 34 ?? ?? ?? ?? ??` | table address = operand at `match+4`; entry = `table + 0x51*3` words, must hold `0x80C7` else "Home key wasn't found; home.reset does nothing"; entry rewritten per `home.reset` |

**CatsEyeXI address:** not recorded.
**Retail:** `kKeyTable` is the pattern docs say was in CatsEyeXI only: retail's compiler used `edx` (`66 8B 04 55`) and moved `test ax,ax`, so it now has two alternatives (`|`). Index byte is `??`. Hard-coded: key 0x51, table stride 3 words, Home `0x80C7`, key function scanned for only 0x200 bytes.

#### Camera offset, walls and free camera (`src/core/camera.cpp`, `src/core/camera_walls.h`)

**Setting:** `[camera] offset.x`, `offset.y`, `walls` (default 1); also the `/freecam` command (free camera). Applies only with a character in the world, and only while an offset is non-zero or easing, walls is on, or the free camera is on.
**What it does:** The `call` that copies the freshly built view matrix into the camera (`kView + 23`) is rewritten to a stub that calls `shift_view` first (moves the view along its own right/up by the offset, casts the game's own collision ray from the character's head to the drawn camera and pulls the view in, or substitutes the free camera's pose). Undone (call put back) when none of those need it. `kWallRay` supplies the game's ray function and the global it is called on. `kProjectionGetter` supplies the scene pointer (projection at `+0x154`). `kCameraTaken` supplies the game's first-person and player-camera flags (read, never written by VanaCore).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kView` (`camera.cpp:152`) | end of the view builder: `call valid; add esp,4; test al,al; je; lea ecx,[esp+8Ch]; push ecx; lea ecx,[esi+4]; call copy; lea ecx,[esp+4Ch]; call ...; lea eax,[esi+4]; pop edi; pop esi; pop ebx; add esp,0C0h; ret` | `E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 10 8D 8C 24 8C 00 00 00 51 8D 4E 04 E8 ?? ?? ?? ?? 8D 4C 24 4C E8 ?? ?? ?? ?? 8D 46 04 5F 5E 5B 81 C4 C0 00 00 00 C3` | call at `match+23` (`kViewCopy`) rewritten to a 32-byte stub (`push ecx; push ecx; push [esp+0Ch]; call shift_view; add esp,8; pop ecx; jmp copy`); original call target kept |
| `kProjectionGetter` (`camera.cpp:156`) | the getter `mov eax,[scene]; add eax,154h; ret` | `A1 ?? ?? ?? ?? 05 54 01 00 00 C3` | `g_scene` = operand at `match+1` (the pointer to the 3D scene); projection read at scene+`0x154` |
| `kCameraTaken` (`camera.cpp:161`) | the function that takes the camera for an event: clears the first-person and player-camera flags | `B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 1A A0 ?? ?? ?? ?? 84 C0 74 11 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 A0 B2 00 00 00 FB C6 05 ?? ?? ?? ?? 00 C3` | first-person flag = operand at `match+15`; player-camera flag = operand at `match+42` |
| `kWallRay` (`camera.cpp:174`) | the camera code's `lea ecx,[esp+5Ch]; push edx; push eax; push ecx; mov ecx,[collision]; call ray; test al,al; je +9; mov byte [esp+17h],0` | `8D 4C 24 5C 52 50 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 09 C6 44 24 17 00` | collision global = operand at `match+9`; ray function = target of the call at `match+13`; then `find_walls` is run (next section) |

**CatsEyeXI address:** `kView` is in FUN_10021A20 (`FFXiMain.dll+0x21A20`, source comment `camera.cpp:144`); the rest not recorded. The log prints the view call's address at run time.
**Retail:** all `??` over operands and call targets; none listed as moved in docs. Missing `kView` or `kProjectionGetter`: "offset.x, offset.y and walls do nothing". Missing `kCameraTaken`: offset stays in events and first person. Missing `kWallRay`: "walls does nothing". Hard-coded game structure offsets: camera object at scene+`0x50` (`kCameraField`), eye `+0x44`, look-at `+0x50`, view matrix at camera+`4`, "view changed" byte at camera+`0xF4` (written to 1), scene+`0x2F8` (focal length target, read), camera view `_31/_33` at `+0x24/+0x2C` (free look), collision-ray struct `hit[8]`.

#### The game's own camera wall checks follow the offset (`src/core/camera_walls.h`)

**Setting:** `[camera] walls` / offset (same trigger as the previous section; `patch_walls(on)` runs inside `patch_offset`).
**What it does:** Seven calls inside the camera function (FUN_1001EC70) that test the camera against the zone are each rewritten (`E8` rel32) to hooks that make the same test on the camera as drawn (offset applied): `camera_ray` (three ray calls), `floor_drawn`, `ceiling_drawn`, `slide_drawn` (two sites). Put back when the offset/walls patch is off. Each call's target is checked: ray sites must call the same function as `kWallRay`'s, the two slide sites the same `set eye` function; sites that fail are skipped and counted in the log ("N of the game's 7 checks on its camera weren't found").

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kWallSites[0]` (`camera_walls.h:141`) | ray from the look-at point to the eye, after the follow (put 0.5 short of a wall) | `8B 0D ?? ?? ?? ?? 8D 94 24 8C 00 00 00 8D 6F 44 52 8D 44 24 60 55 50 E8 ?? ?? ?? ?? 84 C0 75` | call at `match+23` -> `camera_ray` |
| `kWallSites[1]` (`camera_walls.h:144`) | ray from the character's body (1 over feet), put 0.2 short | `8D 4C 24 38 8D 7B 44 50 57 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84` | call at `match+16` -> `camera_ray` |
| `kWallSites[2]` (`camera_walls.h:147`) | floor under the camera (ray 10 down, kept 0.17 over it; push `10.0` = `68 00 00 20 41`) | `8B 4F 4C 8B 57 48 8D 44 24 14 50 8B 06 68 00 00 20 41 51 8B 0D ?? ?? ?? ?? 52 50 E8 ?? ?? ?? ?? 3C 01` | call at `match+27` -> `floor_drawn`; target saved as `g_floor` |
| `kWallSites[3]` (`camera_walls.h:149`) | ceiling over the camera (ray 10 up) | `8B 57 4C 8B 47 48 8D 4C 24 14 51 8B 0E 68 00 00 20 41 52 50 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 3C 01` | call at `match+27` -> `ceiling_drawn`; target saved as `g_ceiling` |
| `kWallSites[4]` (`camera_walls.h:153`) | near-plane side check: `lea ecx,[esp+48h]; push ecx; mov ecx,edi; call set; cmp bl,1; jne` | `8D 4C 24 48 51 8B CF E8 ?? ?? ?? ?? 80 FB 01 75` | call at `match+7` -> `slide_drawn`; target saved as `g_set_eye` |
| `kWallSites[5]` (`camera_walls.h:155`) | the other near-plane site: `...; call set; mov ecx,edi; call view` | `8D 4C 24 48 51 8B CF E8 ?? ?? ?? ?? 8B CF E8` | call at `match+7` -> `slide_drawn` (must match the same `set eye` target) |
| `kWallSites[6]` (`camera_walls.h:158`) | last ray from the body again, while `[10454C0Ch]` counts down | `8B 0D ?? ?? ?? ?? 8D 94 24 2C 01 00 00 52 8D 84 24 E4 00 00 00 56 50 E8 ?? ?? ?? ?? 84 C0 0F 84` | call at `match+23` -> `camera_ray` |

**CatsEyeXI address:** all in FUN_1001EC70 (`FFXiMain.dll+0x1EC70`, `camera_walls.h:127`); individual addresses not recorded. Source mentions the global `[10454C0Ch]` (`FFXiMain.dll+0x454C0C`) only in a comment.
**Retail:** `??` over the collision global and call operands only; none listed as missing from retail. Fragile: the patterns contain hard-coded stack offsets (`esp+8Ch`, `esp+12Ch`, `esp+0E4h`) and `edi+44h` / `ebx+44h` (camera eye field `+0x44`) from this one function, so a different compiler layout drops that site (counted in the log, that check then runs on the unmoved camera). The camera's eye `+0x44` and look-at `+0x50` offsets, `ebx/edi+0x48/0x4C` etc. are hard-coded game structure offsets.

#### Camera lock and swing behind (`src/core/camera.cpp`)

**Setting:** `[camera] lock` (default 0), `[camera] behind` (default 0), `behind.speed`, `behind.delay`; patched while either `lock` or `behind` is on and a character is in the world.
**What it does:** Rewrites the `call` that copies the camera's look-at point (before the game eases it toward the character) to a stub calling `lock_follow(float* at)`, which moves the look-at point and the eye by the character's step since last frame (so the game's leash never pulls), eases the camera back out after a wall push, and (behind) swings the camera round behind the character. Undone (original call) when both are off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kLock` (`camera.cpp:190`) | `call camera; add eax,50h; lea ecx,[esp+5Ch]; push eax; push ecx; call copy; add esp,8; cmp bl,1; jne` | `E8 ?? ?? ?? ?? 83 C0 50 8D 4C 24 5C 50 51 E8 ?? ?? ?? ?? 83 C4 08 80 FB 01 75` | call at `match+14` (`kLockCopy`) rewritten to a stub (`push [esp+8]; call lock_follow; add esp,4; jmp copy`); original target kept |
| `kLockActor` (`camera.cpp:194`) | where the camera code reads the character's position: `mov edx,[esi]; mov ecx,esi; call [edx+1BCh]; push eax; push offset; call copy; mov eax,[esi]; add esp,8` | `8B 16 8B CE FF 92 BC 01 00 00 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 06 83 C4 08` | `g_lock_actor` = the `push offset` operand at `match+12` (the character's x,y,z floats, read each frame) |

**CatsEyeXI address:** not recorded.
**Retail:** `??` over operands; not listed as moved. Both must be found or "the game's camera follow wasn't found; lock does nothing". Hard-coded: vtable slot `+0x1BC` (inside the pattern), camera look-at `+0x50` / eye `+0x44` (`at - 3` floats), the 20-unit "jump" guard.

#### Mouse edge turn and left-drag turn (`src/core/camera.cpp`)

**Setting:** `[camera] mouse.edgeturn` (default 1 = game's), `[camera] mouse.noleftdrag` (default 1 = patched).
**What it does:** One conditional-jump byte each. `mouse.edgeturn=0` changes the `75` (jnz) of `call; test al,al; jnz +54h` in the mouse frame (FUN_10124700) to `EB` (jmp), so the cursor at the window's edge never turns the camera. `mouse.noleftdrag=1` (default) changes `74 1F` (je +1Fh) to `EB 1F` in the camera drag function (FUN_10124CB0), so the left button never turns the camera. Both are re-applied each frame and restored to the game's bytes when the setting is changed back.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kEdge` (`camera.cpp:220`) | `push 8Fh; call key; add esp,4; test eax,eax; jnz; call mode; test al,al; jnz +54h; movsx eax,word [left]` | `68 8F 00 00 00 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 54 0F BF 05` | byte at `match+0x1C` toggled `75` <-> `EB` |
| `kLeftDrag` (`camera.cpp:228`) | `mov bl,al; call mode; test al,al; je +1Fh; test bl,bl; jnz +15h; push -1; push 4; push 8Bh; push 3Eh` | `8A D8 E8 ?? ?? ?? ?? 84 C0 74 1F 84 DB 75 15 6A FF 6A 04 68 8B 00 00 00 6A 3E` | 2 bytes at `match+9` toggled `74 1F` <-> `EB 1F` |

**CatsEyeXI address:** mouse frame FUN_10124700 (`+0x124700`), drag FUN_10124CB0 (`+0x124CB0`), mode check FUN_1025C600 (`+0x25C600`); `[0x104DFBDC]` mouse object, `[0x105ECC14]` window edges, `[0x1062DACC]+0x44` mode (all source comments); patched byte addresses not recorded.
**Retail:** `??` over call targets and rel32s only; not listed as changed. Fragile: the jump distances `54` and `1F`/`15` and key ids `8F`, `8B`, `3E` are literal in the patterns, and the toggled byte is a fixed distance (`+0x1C`, `+9`) into the match. If not found, logs and the setting does nothing.

#### Free look (`/freelook`) (`src/core/camera.cpp`)

**Setting:** `[camera] freelook` (default 0) and the `/freelook` command.
**What it does:** The game's walk (FUN_100A77A0) and turn-on-the-spot (FUN_100A6040) each call the camera getter twice (`call camera; mov edi,eax; call camera; mov ecx,[edi+24h]; fld [eax+2Ch]`). While free look is on, those four calls are rewritten to `free_camera` (a stub `push ecx; push edx; call free_camera; pop edx; pop ecx; ret`) which returns a camera of ours holding the angle the camera had. Undone (calls back to the game's getter) when off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kFreeLook` (`camera.cpp:240`) | the walk's/turn's pair of camera-getter calls | `E8 ?? ?? ?? ?? 8B F8 E8 ?? ?? ?? ?? 8B 4F 24 D9 40 2C` | found twice (second by `scan::find(first + 1, 0x4000, ...)`); calls at `site+0` and `site+7` of both are rewritten; the getter (target of the first call) must equal all four and start/end with `A1 .. 8B 40 50 C3` (`kCameraGetter`) |

**CatsEyeXI address:** FUN_100A77A0 (`+0xA77A0`) and FUN_100A6040 (`+0xA6040`) (source comment); the two sites are "0x9CE apart" (comment, `kFreeSites`).
**Retail:** `??` over call targets; not listed as moved. Fragile: the second site must lie within `0x4000` bytes after the first; hard-coded camera view fields `+0x24` and `+0x2C` (`_31`, `_33`) written into our camera copy. Missing: "the game's walking by the camera wasn't found; /freelook does nothing".

#### Draw distance (`src/core/distance.cpp`)

**Setting:** `[graphics] drawdistance` (zone multiplier), `drawdistance.mobs` (characters), also `fog` and others below; searched the first time any of these differs from the game's.
**What it does:** Finds the two floats the game multiplies the area's draw distance by (zone = kind 0, characters = kind 1) and writes VanaCore's value into them each frame (the game's own value is learned and put back when the setting is the game's). `drawdistance.large_off` temporarily writes 0 (the game's own) into the zone's float while a very large zone with no view lists (Al'Taieu, zone 33) is held. Also reads the config global (`mov ecx,[config]` at `site+0x13`) for the target check.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kMultipliers` (`distance.cpp:29`) | the draw-distance function (`mov eax,ecx; dec eax; je +8; fmul [scenery]; jmp +6; fmul [characters]`) | `8B C1 48 74 08 D8 0D ?? ?? ?? ?? EB 06 D8 0D` | scenery float address = operand at `match+7`; characters float = operand at `match+0xF`; config global = operand at `match+0x13+2` if bytes there are `8B 0D` (outside the pattern); area field at config+`0x1C` read later |

**CatsEyeXI address:** the draw distance is FUN_10187d70 = `FFXiMain.dll+0x187D70` (comment); the log prints the match address.
**Retail:** `??` over the two float addresses, read back by fixed offsets (`kSceneryAt=7`, `kMobsAt=0xF`, `kConfigAt=0x13`), not hard-coded; not listed as changed. Fragile: `kConfigAt` and `kConfigArea` (`config+0x1C`) are offsets read past the pattern's end / into the config object. Missing: "couldn't find the game's draw distance; it stays the game's own".

#### Fog (`src/core/distance.cpp`)

**Setting:** `[graphics] fog=game|distance|custom` (custom with a start/end); also needed while `drawdistance.large_off` holds a zone.
**What it does:** Detours the game's fog function (5-byte prologue `push ebp; push esi; mov esi,ecx; push edi`, `kFogPrologue`) with `imports::detour` to `fog()`, which temporarily puts the game's own draw-distance multipliers back (fog=game), or the requested zone distance (large_off), or overrides start/end (custom). The detour is installed the first time it is wanted and stays; with `fog=distance` it passes straight through.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kFog` (`distance.cpp:37`) | the fog function (thiscall: color*, end*, start*, kind) | `55 56 8B F1 57 8B 46 E4 83 F8 01 8B 44 24 10 74 ?? 8B 4C 24 14 8B 54 24 18 C7 00 FF FF FF FF` | detour at match start, 5-byte prologue |

**CatsEyeXI address:** FUN_10187be0 = `FFXiMain.dll+0x187BE0` (comment).
**Retail:** only the `je` distance is `??`; not listed as changed. Fragile: hard-coded `esi-1Ch` (the kind field before the object, `mov eax,[esi-1Ch]`). Missing: "couldn't find the game's fog".

#### Target check for far characters (`src/core/distance.cpp`)

**Setting:** `[graphics] drawdistance.target` (game vs drawn), only relevant with `drawdistance.mobs` above the game's.
**What it does:** Detours the game's "can this actor be targeted" check (cdecl: actor, player, types, refused, self; 5-byte prologue `sub esp,8; push ebx; push ebp`) so a character past the game's own (1x) draw distance is refused. Uses the game's character-distance function (thiscall on the config) to get the game's own distance, calling it with the multiplier temporarily put back. Installed first time wanted; stays and passes through when not needed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kTargetable` (`distance.cpp:47`) | the target check: `sub esp,8; push ebx; push ebp; push esi; push edi; mov ecx,<target list>; call; ...; sete [esp+1Ch]` | `83 EC 08 53 55 56 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 7C 24 1C 8B E8 8B CF 89 6C 24 14 E8 ?? ?? ?? ?? 3C 01 0F 94 44 24 1C` | detour at match, 5-byte prologue |
| `kCharDistance` (`distance.cpp:54`) | the characters' draw distance (thiscall on the config, kind 1) | `51 8B 44 24 08 56 50 8B F1 C7 44 24 08 00 00 C8 42 E8` | match address = function, called as `fastcall(config, nullptr, 1)` |

**CatsEyeXI address:** `kCharDistance` is FUN_1018A3B0 = `+0x18A3B0` (comment); the check's address is given in the comment as FUN_10083240 (`+0x83240`; the comment wording could mean the check or a caller). The log prints the check's address.
**Retail:** `??` over the target-list address and call targets; the `100.0f` (`00 00 C8 42`) is literal; not listed as changed. Fragile: actor depth read at actor+`0x8C` and config area at config+`0x1C` (hard-coded); the game's `actor+0xB2` draw flag mentioned in a comment. Missing: "couldn't find the game's target check; characters can be targeted as far as they're drawn".

#### Zone view lists, quadtree and far copies (`src/core/distance.cpp`)

**Setting:** `[graphics] drawdistance` above 1, `drawdistance.full`, `drawdistance.fill`, `drawdistance.large_off`, `occl`; each hook only goes in while wanted.
**What it does:** (1) `drawdistance.full` overwrites the 6-byte `je <quadtree>` (`0F 84 rel32`) by a `jmp` (`E9 rel32+1; nop`) so the zone is always drawn from its quadtree, not its view lists; restored when off (a `jmp` already there is recognised as ours). (2) For any draw distance above 1x, the 6-byte `cmp byte [eax+8],1; jbe next` in the quadtree's chunk loop is replaced by a `jmp` to a stub that calls `twin_hidden(renderer, chunk)` (hides far/near duplicate copies, portal caps, `back_*` backdrops, far pieces, and asks `occlusion::hidden`), then continues; restored when not past 1x. (3) `drawdistance.fill` rewrites the 5-byte `jmp <end>` ending the list walk to a stub that continues into the quadtree walk; restored when off. All three are found the first time they are wanted.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kViewList` (`distance.cpp:65`) | the zone draw's jump to the quadtree walk: `mov eax,[esp+20h]; cmp eax,ebx; je <quadtree>; cmp [esi-4],ebx; je; mov edx,[ebp+3944Ch]; lea esi,[eax+4]` | `8B 44 24 20 3B C3 ?? ?? ?? ?? ?? ?? 39 5E FC 0F 84 ?? ?? ?? ?? 8B 95 ?? ?? ?? ?? 8D 70 04` | 6 bytes at `match+6` (`kViewListJump`) must be `0F 84` (game's) or `E9 .. .. .. .. 90` (ours already); patched to `E9 rel32+1 90` |
| `kNodeChunks` (`distance.cpp:78`) | the quadtree's chunk test in FUN_1017B370: `mov edx,[ebx+60h]; mov eax,[edx+eax*4]; cmp byte [eax+8],1; jbe next; mov cl,[eax+0Fh]; test cl,cl` | `8B 53 60 8B 04 82 80 78 08 01 76 ?? 8A 48 0F 84 C9` | 6 bytes at `match+6` replaced by `jmp` to a 36-byte stub; the `jbe` rel8 at byte 5 is read to find "next" |
| `kListEnd` (`distance.cpp:96`) | the list walk's end: `mov [esp+20h],ebx; jmp <end>; cmp [esi],ebx (quadtree start); je; mov eax,[ebp+3944Ch]; mov [esp+14h],4` | `89 5C 24 20 E9 ?? ?? ?? ?? 39 1E 74 ?? 8B 85 ?? ?? ?? ?? C7 44 24 14 04 00 00 00` | 5 bytes at `match+4` (`E9 rel32`) replaced by `E9` to an 18-byte stub (`mov esi,[esp+18h]; cmp dword [esi],0; je end; jmp quadtree`); quadtree start = `match+9`; end = the original jmp's target |

**CatsEyeXI address:** the zone draw's jump is at 0x1017AD68 (`+0x17AD68`) and the chunk test in FUN_1017B370 (`+0x17B370`) (comments); lists made by FUN_1016AC10 (`+0x16AC10`); the log prints each match address.
**Retail:** `??` over rel32s and displacements (`3944Ch`, `[ebp+..]`); not listed as changed. The renderer displacement `3944Ch` is wildcarded in both `kViewList` and `kListEnd`. Fragile: the whole feature depends on hard-coded renderer structure offsets (see the last section): slots at `+0x60` of `0x78` bytes, chunk size `0x16C`, camera list `+0x3912C`, eye `+0x390F0`, draw distance squared `+0x39424`, slot chunk/list fields `+0x24/0x28/0x2C/0x30/0x34`, chunk fields `+0x0A/0x18/0xF4`, mesh name `+8`. The hard-coded stack slot `[esp+18h]` in the `kListEnd` stub and `esi`/`eax` register assumptions are code-shape dependent; anything not as expected is logged and left alone.

#### Zone object detail (`src/core/lod.cpp`)

**Setting:** `[graphics] lod=high|range|game` (default game), `[graphics] lod.range` (default 2, 0.25 to 10).
**What it does:** The zone renderer picks each object's mesh from three (placement `+0x10` low, `+0x14` medium, `+0x18` high) by distance, in two renderers. `lod=high` rewrites the displacement byte of the low and medium loads (`10` and `14`) to `18` at four places (two per site), so the high mesh is always used; `lod=range` replaces four 10-byte compares (`fld [esp+d]; fcomp [reg+0CCh|0C8h]`) with `jmp` to stubs that divide the distance by `lod.range` squared first. Both restored to the game's bytes when off. A site whose bytes are neither the game's nor ours is left alone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kLodSites[0]` (`lod.cpp:29`) | first zone renderer's mesh pick (`esi/edi` form: `[edi+0CCh]`, `mov edx,[edi+10h]`) | `D9 44 24 ?? D8 9F CC 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 57 10 89 54 24 ?? EB ?? D9 44 24 ?? D8 9F C8 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 47 14 89 44 24 ?? EB ?? 8B 4F 18` | displacement bytes at `match+0x15` (low `10`) and `match+0x31` (medium `14`) set to `18`; compares at `match+0x00` (mid) and `match+0x1C` (near) replaced for `lod=range` |
| `kLodSites[1]` (`lod.cpp:32`) | second renderer's pick (`ecx/ebx` form: `[ecx+0CCh]`, `mov ebx,[ecx+10h]`) | `D9 44 24 ?? D8 99 CC 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 59 10 EB ?? D9 44 24 ?? D8 99 C8 00 00 00 DF E0 25 00 41 00 00 75 ?? 8B 59 14 EB ?? 8B 59 18` | bytes at `match+0x15` and `match+0x2D` set to `18`; compares at `match+0x00` and `match+0x18` for `lod=range` |

**CatsEyeXI address:** not recorded (log prints both match addresses).
**Retail:** `??` only over stack displacements and jump sizes; mesh slots (`+0xCC`, `+0xC8`, `+0x10`, `+0x14`, `+0x18`) are literal inside the pattern. Not listed as changed in docs. Either site may be missing (the other still works); both missing: "couldn't find where the game picks its zone objects' detail".

#### Zone draws sent as batches (`src/core/zonebatch.cpp`)

**Setting:** `[graphics.effects] batch` (read by the renderer; hook goes in when `vanacore_gfx_batch_wanted()` is true).
**What it does:** Detours the zone renderer's draw function (thiscall, no arguments; 6-byte prologue `sub esp,110h`) so the renderer's batcher is told when the zone's draws begin and end (`vanacore_gfx_batch(1)` / `(0)`), except in a moving zone (a ferry: `renderer+0x158` non-zero) or when `update(true)` says so. The detour is installed once and stays.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kZoneDraw` (`zonebatch.cpp:26`) | the zone's draw: `sub esp,110h; mov eax,[104544FCh]; push ebx; push ebp; push esi; mov eax,[eax+0Ch]; xor ebx,ebx; push edi; mov ebp,ecx; mov ecx,[eax]; push ebx; push ebx; push eax; call [ecx+0F4h]` | `81 EC 10 01 00 00 A1 ?? ?? ?? ?? 53 55 56 8B 40 0C 33 DB 57 8B E9 8B 08 53 53 50 FF 91 F4 00 00 00` | `imports::detour` at match, 6-byte prologue |

**CatsEyeXI address:** FUN_10179FD0 = `FFXiMain.dll+0x179FD0`, on the zone renderer `DAT_105FD284` (`+0x5FD284`); the `[104544FCh]` global (`+0x4544FC`) is in a comment (source `zonebatch.cpp:21-24`); the log prints the match address.
**Retail:** `??` over the global; the `[eax+0Ch]`, `call [ecx+0F4h]` (vtable slot) are literal; not listed as changed. Missing: "couldn't find where the game draws its zone; its draws go one by one". Hard-coded: renderer `+0x158` (moving zone).

#### Water models known as they load (`src/core/water.cpp`)

**Setting:** `[graphics] water.detect`, the water debug view and `water.log` (any of them sets `set_wanted`); hook goes in the first frame it is wanted.
**What it does:** Detours the game's map-model loader (FUN_10175990; thiscall on the resource, 7-byte prologue `sub esp,10h; push ebx; mov ebx,[ecx+28h]`) so models under a water directory (`miz`, `umi`, `sea`, `taki`, `kawa`, `wat`) are loaded with VanaCore's Direct3D 8 told (`vanacore_gfx_water_loading`), flagging their vertex buffers as water. Installed once, not undone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kModelLoad` (`water.cpp:36`) | the map-model (DAT chunk type 0x2E) handler, start of function | `83 EC 10 53 8B 59 28 55 8B 2D ?? ?? ?? ?? C7 44 24 08 00 00 00 00 C7 44 24 0C 00 00 00 00 C7 44 24 10 00 00 00 00 C7 44 24 14 00 00 00 00 8B 45 3C 56 8B 71 20 57` | detour at match, 7-byte prologue; the byte at `match+0x7D` must be `E8` (the model maker call) or install is refused |

**CatsEyeXI address:** FUN_10175990 = `FFXiMain.dll+0x175990` (build 2025-12-26, vtable slot 8 at `0x10334550` = `+0x334550`, model maker FUN_10177db0 = `+0x177DB0`). The same comment quotes XI on Anything's 2026-09-03 hooks at `0x10176ed0` (`+0x176ED0`) and `0x10176f52` (`+0x176F52`), which are 0x82 bytes apart, consistent with `kMakeCall` `0x7D`+5; their date matches retail's build, so these are probably retail's addresses (not verified here).
**Retail:** `??` over the models global only; not in docs' list. Fragile: resource fields read by hard-coded offset (`+0x20` name, `+0x28` parent handle; read under `__try`), the `0x7D` call offset. Missing: log "the game's map model loading wasn't found in FFXiMain.dll: its water models aren't known".

#### Bump mapping on/off (`src/core/bumpmap.cpp`)

**Setting:** `[graphics] bump=game|1|0` (default game).
**What it does:** Not a code patch: it finds the renderer's startup routine, the bump-mapping option global and the renderer pointer, then writes the game's own flags directly in the renderer object (`kLoaded`, `kDraw`, `kKept`) and the option global, and calls the game's setup routine (`g_setup(renderer, nullptr)`) to build the bump shaders when they were never made. `bump=0` clears them again (option 0, draw 0, kept 0; loaded 0 once no zone holds its bump DAT). Keeps bump off in zones 0xB6, 0x2D3, 0x2E6 as the game does. Applied each frame.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSetup` (`bumpmap.cpp:31`) | the renderer's startup routine (thiscall, no args) | `83 EC 4C 56 8B F1 8D 4C 24 2C C7 44 24 34 00 00 00 20 C7 44 24 38 00 00 02 40` | match = function, called by us |
| `kOption` (`bumpmap.cpp:35`) | the step that clears the load/draw flags, comparing the option global | `33 C0 89 86 98 94 03 00 89 86 9C 94 03 00 39 05 ?? ?? ?? ??` | option global = operand at `match+16` |
| `kRenderer` (`bumpmap.cpp:40`) | where the zone's chunks are built, naming the renderer global | `8B 0D ?? ?? ?? ?? 89 44 24 78 83 C4 08 39 A9 98 94 03 00` | renderer global = operand at `match+2` |
| `kZone` (`bumpmap.cpp:46`) | the per-zone-load copy of kKept to kDraw, and the object holding the zone number | `8B 86 A0 94 03 00 89 86 9C 94 03 00 8B 0D ?? ?? ?? ?? 3B CB 74 ?? 8B 89 B4 01 00 00 81 E9 B6 00 00 00` | zone-object global = operand at `match+14`; zone id read at object+`0x1B4` |

**CatsEyeXI address:** not recorded in source ("RE 2026-10-11" only; the log prints `setup` address).
**Retail:** `??` over the globals only; the renderer field displacements (`39498h`, `3949Ch`, `394A0h`) and `1B4h` are literal inside the patterns. None of these four patterns are in docs' list of four missing from retail, so they match retail; the renderer is a different object from the "big object" whose fields moved. All four are required: otherwise log "couldn't find the game's bump mapping (setup/option/renderer/zone found or missing)" and bump stays the game's. Hard-coded renderer offsets (see the last section).

#### Sun shadow caster pass (`src/core/casters.cpp`)

**Setting:** `[graphics.effects] sun_seek` (the renderer's reach; `casters::update` installs once `vanacore_gfx_seek_reach(nullptr) > 0`).
**What it does:** Detours the game's piece-in-view test (`FUN_10180670`, 5-byte prologue) so it reports "in view" while the pass draws, and overwrites 7 bytes (`cmp dword [ebp+3949Ch],1`) at the end of each zone slot's draw with a `jmp` to a stub that calls `seek(renderer, slot walked, lights)`. `seek` draws every zone piece (and UAS-list piece) within the shadows' reach of the player, through the game's own piece renderers (called as fastcalls), with the renderer's eye (`+0x390F0`) moved into each piece while it draws. Installed once and not undone; passes straight through when the reach is 0. A fault in the game's renderer turns it off for the session.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kViewTest` (`casters.cpp:40`) | the piece-against-view test (thiscall that ignores ecx, pops 8) | `83 EC 60 53 56 8B 74 24 6C 57 8B 7C 24 74 33 C9 D9 06 D8 0F D9 46 20 D8 4F 08 DE C1 D9 47 04 D8 4E 10 DE C1 D8 46 30 D9 5C 24 0C D9 47 04 D8 4E 14 D9 46 04 D8 0F DE C1 D9 46 24 D8 4F 08 DE C1 D8 46 34 D9 5C 24 10` | detour at match, 5-byte prologue |
| `kSlotEnd` (`casters.cpp:52`) | the end of a zone slot's draw: held-back pieces drawn through RenderChunk2 or RenderChunk1 | `8B 85 9C 94 03 00 33 FF 83 F8 02 8B 85 40 8D 03 00 8D B5 40 75 03 00 75 26 3B C3 7E 46 8B 56 08 8B 06 8B 4E 04 52 50 51 8B CD E8 ?? ?? ?? ?? 8B 85 40 8D 03 00 47 83 C6 0C 3B F8 7C E0 EB 24 3B C3 7E 20 8B 56 08 8B 06 8B 4E 04 52 50 51 8B CD E8 ?? ?? ?? ?? 8B 85 40 8D 03 00 47 83 C6 0C 3B F8 7C E0 83 BD 9C 94 03 00 01` | 7 bytes at `match+0x63` (`cmp dword [ebp+3949Ch],1`) replaced by `E9 rel32 90 90` to a stub (`pushad; pushfd; push [esp+40h]; push [esp+40h]; push ebp; call seek; popfd; popad; <the game's cmp>; jmp back`); RenderChunk2 = call target at `match+0x2A`, RenderChunk1 = call target at `match+0x50` |
| `kUasLoop` (`casters.cpp:65`) | the UAS list loop drawing DrawUASSubchunk | `8B 45 40 8B 5D 3C 3B C7 7E ?? 81 C3 A9 01 00 00 8A 03 33 F6 84 C0 76 ?? 8D 7B EF 8B 17 8B 45 64 8B 4C 24 10 52 50 E8` | DrawUASSubchunk = call target at `match+0x26`; if missing only the UAS pieces are skipped |

**CatsEyeXI address:** view test FUN_10180670 (`+0x180670`; FUN_10180B30 `+0x180B30` starts the same, not hooked); zone draw FUN_10179FD0 (`+0x179FD0`); RenderChunk2 `0x1017C8E0` (`+0x17C8E0`), RenderChunk1 `0x1017B480` (`+0x17B480`), DrawUASSubchunk `0x10182860` (`+0x182860`), UAS lists FUN_101798A0 (`+0x1798A0`); renderer global DAT_105FD284 (`+0x5FD284`) (all comments); the log prints `test` and `end+0x63` addresses.
**Retail:** only the call targets and the UAS loop's jump size are `??`. `kSlotEnd` is a very long pattern with literal renderer offsets (`3949Ch`, `38D40h`, `37540h`) and structure sizes (`0Ch` per piece), the most fragile pattern in these files; none of the three is on docs' list of patterns missing from retail, so they match retail. Missing `kViewTest` or `kSlotEnd`: "the sun's shadows take only what the camera sees". Hard-coded structure offsets: see the last section.

#### 3D aspect ratio (`src/core/graphics.cpp`)

**Setting:** `[graphics] aspect=auto|off|<w:h>` (default auto; not applied with `off`).
**What it does:** Finds the global that holds the game's camera object (through the code that sets the projection's shape, `mov eax,[global]; test eax,eax; je; fld [esp+4]; fmul [0.25]; fmul [3.0]`), then each frame writes `4/3 / ratio` into the camera's float at `+0x2F0` (the window's shape for auto). Only that float is written; nothing is hooked or restored when set to `off` (it just stops writing).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kAspectSetter` (`graphics.cpp:75`) | the code setting the 3D projection's shape | `A1 ?? ?? ?? ?? 85 C0 74 ?? D9 44 24 04 D8 0D ?? ?? ?? ?? D8 0D` | camera-object global = operand at `match+1`; float at object+`0x2F0` (`kAspectField`) written every frame |

**CatsEyeXI address:** not recorded (the log prints the global's `FFXiMain.dll+0x...`).
**Retail:** `??` over the global, jump size and constant addresses; not in docs' list. Hard-coded: the `+0x2F0` field (the same object as camera.cpp's focal length at `+0x2F4` / `+0x2F8`; not named as moved in docs). Missing: "aspect ratio: couldn't find the game's camera; the scene keeps its shape".

#### No-blink gear changes (`src/core/noblink.cpp`)

**Setting:** `[client] blink.disable` (default 0), `[client] blink.weapons` (default 0); hook goes in the first frame `blink.disable` is on.
**What it does:** Detours the entity "model rebuild" step (cdecl, the entity; 5-byte prologue `push esi; mov esi,[esp+8]`). For the local player's entity with an actor, with only equipment-slot bits set in the entity's update flags (`+0xF4`; hair/face bit 1 and race change excluded; main/sub bits 0x40/0x80 excluded unless `blink.weapons` is on), the step is skipped, leaving the flags set so the model rebuilds the moment the option is turned off. Installed once and stays (it passes straight through when off).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kStep` (`noblink.cpp:32`) | the step: `push esi; mov esi,[esp+8]; mov ecx,esi; call <allowed>; test al,al; jz; cmp word [esi+0F4h],0; jnz; test byte [esi+120h],1; jz` | `56 8B 74 24 08 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 66 83 BE F4 00 00 00 00 75 ?? F6 86 20 01 00 00 01 74 ??` | detour at match, 5-byte prologue; the call at `match+7` must land on `56 8B F1 8A 86 EE 00 00 00` (`kAllowedStart`) or "isn't as expected" and the feature is off |
| `kPlayer` (`noblink.cpp:40`) | the local player's entity getter: `movzx ax,[player index]; test ax,ax; jz; movsx eax,ax; mov eax,[eax*4+entities]; test eax,eax; jnz; xor eax,eax; ret` | `66 A1 ?? ?? ?? ?? 66 85 C0 74 0E 0F BF C0 8B 04 85 ?? ?? ?? ?? 85 C0 75 02 33 C0 C3` | match = function, called as `cdecl()` |

**CatsEyeXI address:** the step at `0x10095BE0` = `FFXiMain.dll+0x95BE0`, the player getter FUN `0x101CAC50` = `+0x1CAC50` (comments); the log prints both.
**Retail:** `??` over globals and jump sizes. `kStep` contains the entity flag displacements `F4h` and `120h` literally; docs' audit says entity fields `0xD8` and `0x120` did not move and `kStep` is not among the four missing from retail. Hard-coded entity offsets: actor `+0xA0`, update flags `+0xF4`, flags `+0x120` (bit 0 race change), and `+0xEE` inside the allowed function's checked start. Missing either: "the game's model rebuild step wasn't found in FFXiMain.dll: blink.disable does nothing".

#### Changes to game memory and calls not found by a pattern, and hard-coded structure offsets

- `graphics.cpp:1431`: IAT hook of `GetCursorPos` (USER32) in every loaded game/plugin module (`imports::for_each_module` + `imports::hook`), for `[ui] aspect` cursor mapping; always when the renderer is d3d11/d3d12.
- `graphics.cpp:1557-1561`: passes FFXiMain.dll's image range (base to base + `SizeOfImage`) to the renderer (`vanacore_gfx_set_image`); no code change.
- `graphics.cpp:1544` `direct3d_create()`: the game gets VanaCore's Direct3D 8 (d3d9/d3d11/d3d12) instead of its own; the interposition itself is not in these files.
- `camera.cpp` runtime stubs (`VirtualAlloc`'d, RWX) for zoom, locked focal length, view copy, lock copy, free look; `camera.cpp:1763` writes the game's shared floats 6.0 and 3.0 back when another addon (xicamera) changed them.
- `camera.cpp` hard-coded game structures: scene->camera at `+0x50` (`kCameraField`); camera eye `+0x44`, look-at `+0x50`, view `+4` (matrix), view-changed flag `+0xF4`, `_31/_33` at `+0x24/+0x2C`; scene projection `+0x154` (`_11`, `_22` at `[0]`, `[5]`), focal length `+0x2F4` (read via the getter's bytes) and target `+0x2F8`; keyboard table entry for game key 0x51 at `table + 0x51*3` words.
- `distance.cpp` renderer/zone structure offsets: zone slots `renderer+0x60` (two, `0x78` bytes each), slot `+0x24` chunk count, `+0x28` chunks, `+0x2C` list count, `+0x30` lists, `+0x34` has lists; chunk size `0x16C`, chunk `+0x0A` far flag, `+0x18` high mesh, `+0xF4` box corners; mesh `+8` name; renderer `+0x3912C` camera list, `+0x390F0` camera eye, `+0x39424` draw distance squared; config `+0x1C` area; actor `+0x8C` depth.
- `casters.cpp` hard-coded: slot kind `+0x00`, `+0x38` walked, `+0x24/+0x28` chunk count/chunks; renderer `+0x3949C` render mode, `+0x390F0` eye, `+0x158` moving; chunk size `0x16C`, `+0x08` type, `+0x10/+0x14/+0x18` low/medium/high meshes, `+0xF4` corners. `zonebatch.cpp`: renderer `+0x158`. `bumpmap.cpp`: renderer `+0x60` slots (`0x78` each, `+0x58` bump resource), `+0x3946C` first vertex shader, `+0x39494` pixel shader, `+0x39498` loaded, `+0x3949C` draw, `+0x394A0` kept; zone object `+0x1B4` id; no-bump zones 0xB6, 0x2D3, 0x2E6.
- `docs/retail-signatures.md` says the "+0x70 / +0x78" shifts are in the game's big object (`0x2D60C-0x44B38` old offsets); the renderer offsets above (`0x3912C` to `0x394A0`) fall in that numeric range, but they are in a separate renderer object (DAT_105FD284) and the patterns embedding them (`kSetup`... `kSlotEnd`) are among the 264 that match in retail, so they did not move. Not independently verified.
- `lod.cpp` writes single bytes; `distance.cpp`, `bumpmap.cpp`, `graphics.cpp` (aspect) write game floats/flags directly in game memory (no code change).

### Names and damage numbers

#### Name size and letter spacing: the factors (`src/core/nameplates.cpp`)

**Setting:** `[graphics] nameplates` (`stretched`/`fix`/`spaced`), `nameplates.font=sharp`, `nameplates.centre`, the pearl/bazaar/plate/name/stars offsets, `nameplates.gm`, or an addon's widgets at the names (any of these makes `fix`/`pearl`/`sharp` true in `apply`).
**What it does:** the game lays a name out with an across and a down factor (one shared float constant) that it multiplies each letter's size by. The two `fmul [c]` operands are rewritten to point at VanaCore's own floats `g_across`/`g_down` (the game's constant x 4:3 / window shape, divided by 4 for quarter-unit spacing). Searched when first wanted, and again after a plugin loads or unloads; restored to the game's constant when the setting goes back to `stretched` with no fine spacing. If the two operands aren't the same constant or VanaCore's, something else (Ashita's Nameplate plugin) has the code and VanaCore leaves it alone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kFactors` (`nameplates.cpp:42`) | the routine that works out the across/down factors for a name's letters | `E8 ?? ?? ?? ?? 25 FF FF 00 00 89 44 24 10 DB 44 24 10 D8 0D ?? ?? ?? ?? D8 8C 24 E0 06 00 00 D9 5C 24 4C E8 ?? ?? ?? ?? 8B BC 24 DC 06 00 00 25 FF FF 00 00 89 44 24 10 83 C9 FF DB 44 24 10 33 C0 F2 AE D8 0D ?? ?? ?? ?? D8 8C 24 E0 06 00 00` | `site+0x14` (`kAcross`) and `site+0x45` (`kDown`) are the two `fmul [c]` operands (read, then rewritten); `site+0` and `site+0x23` (`kWidthCall`, `kHeightCall`) are the calls to the game's scene width and height getters, whose targets are read from their rel32 (`install_sharp`); `g_site` is also the start of the 0x800-byte window the sharp-font patterns are searched in |

**CatsEyeXI address:** not recorded for `kFactors` (it is the routine "g_site"; only the log line prints it at run time).
**Retail:** `??` over the three constant operands and the two call targets; the `[esp+6E0h]`/`[esp+6DCh]` stack offsets are literal. docs/retail-signatures.md does not list it among the four that failed, so it matched in both builds. Not found: feature off, one log line. Fragile: `+0x14` and `+0x45` are hard-coded distances into the match.

#### Letter hook (`nameplates.font=sharp`, `spaced`, `centre`, pearl/bazaar moves) (`src/core/nameplates.cpp`)

**Setting:** `[graphics] nameplates=spaced`, `nameplates.font=sharp`, `nameplates.centre`, pearl/bazaar offsets or shape, `nameplates.gm`.
**What it does:** the `call` that gets each letter's sprite from the font (corners' texture coordinates, size, x and y offsets) has its rel32 rewritten to VanaCore's `letter`, which adjusts the written size/offsets and passes on. Installed once, when first needed; never undone, but it passes the game's values through when the settings are off. If the call already points at VanaCore's (re-found after a plugin change) it is kept.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kLetterCall` (`nameplates.cpp:50`) | the `__thiscall` on the font sheet that gets a letter's sprite | `52 50 56 51 53 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 FF 0F 84 ?? ?? ?? ?? 80 FB 8E` | `site+10` (`kLetterCallAt`) is the `E8`; its rel32 is replaced with a call to `letter` and the old target kept as `g_letter` |

**CatsEyeXI address:** not recorded.
**Retail:** `??` over the font pointer, call target and jump size. In both builds per the audit. Not found: spacing/centre/sharp off, log line.

#### Sharp (True Font) names: the letters' quad draw (`src/core/nameplates.cpp`)

**Setting:** `[graphics] nameplates.font=sharp`, and also set up when a pearl/bazaar shape or move is wanted (`pearl`), since those move parts at the same draw.
**What it does:** inside the routine found by `kFactors` it finds, in order, the corner call, the material call, the renderer load and the quad's draw call, then redirects the draw call and the material call to VanaCore (`draw_letter`, `material`). VanaCore draws the whole name as one text quad where the game draws its first letter and nothing for the rest. The renderer global and device (`renderer+0xC`) are read from the code. A second match of `kDrawCall` after the first (within 0x800) is the draw for the job-master stars line, redirected to `draw_stars`. Installed once, left in place.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kCornerCall` (`nameplates.cpp:132`) | where a letter's corners are worked out, in the name routine | `D9 5C 24 ?? D9 44 24 ?? D8 0F D9 5C 24 ?? E8 ?? ?? ?? ?? 8B 44 24 ?? 8B 4D FC 8B 55 00 89 06 C7 46 04 00 00 80 3F` | searched within 0x800 bytes of `g_site` (the `kFactors` match); an anchor only, nothing patched |
| `kMaterialCall` (`nameplates.cpp:135`) | the call that sets a letter's texture and colour | `52 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84` | searched within 0x100 after the corner call; `site+7` (`kMaterialCallAt`) call redirected to `material` |
| `kRenderer` (`nameplates.cpp:138`) | the load of the renderer object | `8B 15 ?? ?? ?? ?? 6A 00 8B 42 0C` | within 0x100 after the material call; `site+2` is the renderer global (read as `g_renderer_slot`); the device is read from `renderer+0xC` each use (`kRendererDevice`) |
| `kDrawCall` (`nameplates.cpp:142`) | the letter quad's `DrawPrimitiveUP` call (strip, 2 triangles, stride 0x1C) | `8B 0D ?? ?? ?? ?? 8D 94 24 ?? ?? ?? ?? 6A 1C 52 6A 02 6A 05 E8` | within 0x100 after the renderer; `site+20` (`kDrawCallAt`) call redirected to `draw_letter`; the next match (`draw_site+1`, within 0x800) is the stars' draw, redirected to `draw_stars` |

**CatsEyeXI address:** not recorded (the log prints the draw call's offset at run time).
**Retail:** `??` over the stack frame offset of the vertices, the renderer global, the call targets. The chain is relative (each searched near the last), so it needs the routine's layout to stay in the same order. In both builds per the audit. Not found: sharp names left to the game with a log line; stars not found: logged separately and stars stay put. The same `kDrawCall` bytes are also used in `damagetext.cpp`. Fragile: the 0x100/0x800 search windows.

#### Name call, line under the name, own entity (`src/core/nameplates.cpp`)

**Setting:** any of `nameplates.hide*` (own/party/alliance/players/npcs/monsters/trusts/pets), `.bazaar`, `.linkshell`, `.mastery`, `.size`, `.distance`, `.color.*`, `.smooth`, `.font=sharp`, any move, `.framegen`, or `vana.gameui.hide('nameplates')`; also found for `nameplates.order` (`kNameCall` only).
**What it does:** the `call` in each character's name routine that draws the name (a `__thiscall`, five args, `ret 14h`) has its rel32 redirected to `draw_name`, which can skip it, scale its size argument, recolour it, drop icons from the text, and track the plate. The call right after it that draws the line under the name (job master stars) is redirected to `draw_line`. The local player's entity getter is called, not patched, to find your own entity. Installed once the first time a setting needs it and left in place (VanaCore passes through when settings are default).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kNameCall` (`nameplates.cpp:71`) | the name drawing call in the actor's name routine (the routine is FFXiMain.dll+0xD0260 in CatsEyeXI, per the comment) | `51 52 8B CE E8 ?? ?? ?? ?? 8B 06 8B CE FF 90 D0 00 00 00` | `site+4` (`kNameCallAt`) call redirected to `draw_name`; also `g_name_site` for `nameplates.order`, to tell actors whose draw routine contains it (within `kNameRoutineSpan` = 0x400 of the vtable entry) |
| `kLineCall` (`nameplates.cpp:77`) | the call that draws the line under the name (stars) | `52 50 51 8B CE E8 ?? ?? ?? ?? 5F 5E 81 C4 18 01 00 00 C3` | within 0x80 after the name call; `site+5` (`kLineCallAt`) call redirected to `draw_line` |
| `kPlayerEntity` (`nameplates.cpp:108`) | the game's "local player's entity" getter (a `__cdecl` returning the entity or 0) | `66 A1 ?? ?? ?? ?? 66 85 C0 74 0E 0F BF C0 8B 04 85 ?? ?? ?? ?? 85 C0 75 02 33 C0 C3` | the match address itself is called as `PlayerFn` (not patched) |

**CatsEyeXI address:** the name routine's `FFXiMain.dll+0xD0260`; name builder `FUN_10097650` (`FFXiMain.dll+0x97650`) is mentioned in a comment; the call's own offset is logged at run time. Not recorded for the three signatures.
**Retail:** `??` over call targets, index global and entity-list address. The literal `FF 90 D0 00 00 00` is virtual slot `+0xD0` of the actor. In both builds per the audit. Not found: names stay as drawn, log line; player getter not found: own name not hidden, log line; line call not found: stars stay, log line.

#### Smooth name size (`src/core/nameplates.cpp`)

**Setting:** `[graphics] nameplates.smooth`.
**What it does:** the `_ftol` call that cuts the depth to a whole number (FFXiMain.dll+0x82EA8 in CatsEyeXI) is redirected to `depth_cut`, which keeps the unrounded depth so the name can be sized smoothly. The far and near depth globals (code near FFXiMain.dll+0x82F00) are read back from the code. Needs the name call hooked first. Done once, not undone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDepthCut` (`nameplates.cpp:99`) | the `_ftol` of the name's depth (`fld [esp+0Ch]; fmul [scale]; ... call _ftol`) | `D9 44 24 0C D8 0D ?? ?? ?? ?? 89 46 04 E8 ?? ?? ?? ?? 8D 4C 24 14 89 46 08 C7 46 0C 00 00 00 00` | `site+13` (`kDepthCutAt`) call redirected to `depth_cut`; the original `_ftol` target saved as `g_ftol` |
| `kDepthRange` (`nameplates.cpp:103`) | the far/near depth compare that sizes a name | `8B 4B 08 A1 ?? ?? ?? ?? 3B C8 76 0C 5F 5E 5D 32 C0 5B 83 C4 08 C2 10 00 8B 15 ?? ?? ?? ?? 33 DB 3B CA 73 08` | `operand(site+4)` (`kFarAt`) = far global address, `site+0x1A` (`kNearAt`) = near global address, dereferenced each frame; not patched |

**CatsEyeXI address:** `FFXiMain.dll+0x82EA8` (the `_ftol` call, `kDepthCut`+13), `FFXiMain.dll+0x82F00` (`kDepthRange`), per comments.
**Retail:** `??` over the scale constant, `_ftol` target and the two depth globals; read back with direct operand reads, not hard-coded. In both builds per the audit. The comment says far - near is 80 (a game constant, not verified). Not found (either one): names size in the game's steps, log line.

#### Names drawn far to near (`src/core/nameplates.cpp`)

**Setting:** `[graphics] nameplates.order` (default 1); also set up when an addon has `vana.world.place` widgets (`widgets::any_world`).
**What it does:** the first instruction of the game's actor-list walk (a 6-byte `mov esi,[list]`) is overwritten with `E9 rel32` + `90` to jump to VanaCore's `walk_stub`, which walks the same list itself, far to near by the entity's squared distance, and calls each actor's draw virtual (`+0x284`); it then jumps back to after the game's loop (`walk+48`). Installed once, never undone (with `order` off the stub just walks in the game's order). Uses `kNameCall` too (above).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kActorWalk` (`nameplates.cpp:114`) | the loop that calls each actor's overlay draw (`call [edx+284h]`) down its `+54h` list | `8B 35 ?? ?? ?? ?? 85 F6 74 26 D9 86 8C 00 00 00 D8 1D ?? ?? ?? ?? DF E0 25 00 01 00 00 75 0A 8B 16 8B CE FF 92 84 02 00 00 8B 76 54 85 F6 75 DA` | 6 bytes at the match overwritten with `E9`+rel32+`90`; `operand(site+2)` (`kWalkList`) = the actor list head global, `operand(site+18)` (`kWalkLimit`) = the float limit global, `site+48` (`kWalkAfter`) = where VanaCore jumps back to |

**CatsEyeXI address:** not recorded (the log prints it at run time).
**Retail:** `??` over the list and limit globals. The loop's offsets `+8Ch`, `+54h`, `+284h` are literal in the pattern and also hard-coded in the code (next section). In both builds per the audit. Not found: game order kept, log line. Fragile: `+48` and `+18` are hard-coded distances into the match; the patch relies on the first instruction being exactly 6 bytes.

#### Blink off for the target's name (`src/core/nameplates.cpp`)

**Setting:** `[graphics] nameplates.blink` (0 stops the pulse).
**What it does:** the game tests the actor's `[+88h] & 4` (it is the target) and jumps past the pulse (alpha times sin(16 degrees a frame) * 32 + 96) when clear. With `blink=0` that `74` (je) is changed to `EB` (jmp); turned back to `74` when blink is on again. Looked for the first time `blink` is off; restored while the game runs.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kBlink` (`nameplates.cpp:1500`) | the test of the actor's target flag in the name colour code | `F6 87 88 00 00 00 04 74 ?? 33 C9 8A 4E 03` | `site+7` (`kBlinkJump`) byte `74`<->`EB` |

**CatsEyeXI address:** `FFXiMain.dll+0x82F97` (comment).
**Retail:** `??` over the jump's size only. In both builds per the audit. Not found: blink stays, log line. Fragile: `site+7` hard-coded; `+88h` flag in the pattern.

#### Hard-coded game structure offsets and non-pattern code (`src/core/nameplates.cpp`, `gmicons.cpp`)

- Actor (the thing the walk visits): `+0x54` next, `+0x8C` float "drawn" limit, `+0x70` its entity (`kActorEntity`), `+0x88 & 4` target flag (in `kBlink`), vtable slot `0x284/4` (the overlay draw) and `+0xD0` (in `kNameCall`).
- Entity: `+0xD8` squared distance (`kEntityDistance`), `+0x74` target index, `+0x78` server id, `+0x124` flags (`0x8000000` in a linkshell), `+0x1D4` linkshell colour (`nameplates.cpp:82`); `+0x7C` name (`gmicons.cpp:285`); `+0x128` (`>>11 & 7` GM level), `+0x12C` (`0x40000` = GM mode off), `+0x130 & 0x4000` mentor flag (`gmicons.cpp:32`). docs/retail-signatures.md says the entity fields `0xD8` and `0x120` did not move; `0x124`, `0x1D4`, `0x128`, `0x12C`, `0x130` are not named as checked.
- Entities `0x400`-`0x700` are players (`kPlayersFrom`/`kPlayersTo`); pets come from `game::Entity::pet_index`.
- Glyph codes: bazaar `0x9C`, pearl `0x92`, stars `0xD2` (copied as `0xD3`, `0xD4`, `0xD5`; the code at FFXiMain.dll+0x868E9), GM glyphs `0x93`, `0x95`-`0x99`; the icon picker is FFXiMain.dll+0x97310 and its glyph table FFXiMain.dll+0x35962C in CatsEyeXI (comments only; nothing reads them). The name-plate colours table values (`0x808080`, etc.) are CatsEyeXI's game colours.
- Name routine layout: the line-under-name routine is at FFXiMain.dll+0x86C37 in CatsEyeXI (comment).
- The names' font sheet is read from `ROM\119\51.DAT` ("font    font    "), not from code.
- Party list for `hideparty`/`hidealliance` comes from `game::party()` (game.cpp's globals, not this file's).
- `gmicons.cpp`, `gmicons_staff.h`, `nameplates_debug.cpp`, `nameplates_party.h`, `nameplates_stars.h`: no patterns and no patching; they use the entity offsets above and the hooks in `nameplates.cpp`. Debug/trace and staff list are not game-code changes.

#### Damage numbers: layout, glyphs and draw (`src/core/damagetext.cpp`)

**Setting:** `[graphics] damage.*` (hooked while any `damage.*` setting wants it: `settings.wants_hook()`, e.g. `damage.font=sharp`, `damage.ontop`, colours, offsets, spawn, crit colour, label, multi, preview).
**What it does:** the call in the popup's draw to the text layout routine (`__cdecl`) is redirected to VanaCore's `layout`, which can recolour, move the point, and re-draw as VanaCore text. Inside that routine (searched within 0x400 bytes of its start) the glyph sprite call and the quad's `DrawPrimitiveUP` call are redirected too (`glyph`, `draw_glyph`). The renderer global and the scene global are read back from the code. Installed once; with settings back to the game's the hooks pass through, they are not removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kPopupCall` (`damagetext.cpp:36`) | the popup's call of the text layout routine (`push 3DCCCCCDh; push eax; add edi,198h; push esi; lea ecx,[esp+20h]; push edi; push ecx; call; add esp,14h`) | `68 ?? ?? ?? ?? 50 81 C7 98 01 00 00 56 8D 4C 24 20 57 51 E8 ?? ?? ?? ?? 83 C4 14` | `site+19` (`kPopupCallAt`) call redirected to `layout`; the call's target is the layout routine, the start for the next two searches |
| `kGlyphCall` (`damagetext.cpp:60`) | the layout's per-glyph sprite call on the `dmgnum` font (`__thiscall`, ret 14h) | `6A 00 6A 00 56 55 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F BF 06 03 D8` | within 0x400 of the routine; `site+12` (`kGlyphCallAt`) call redirected to `glyph` |
| `kDrawCall` (`damagetext.cpp:64`) | each glyph quad's `DrawPrimitiveUP` call (same bytes as `nameplates.cpp:142`) | `8B 0D ?? ?? ?? ?? 8D 94 24 ?? ?? ?? ?? 6A 1C 52 6A 02 6A 05 E8` | within 0x400 of the routine; `site+20` (`kDrawCallAt`) call redirected to `draw_glyph`; `operand(site+2)` = the renderer global (`g_renderer_at`) |
| `kSceneGetter` (`damagetext.cpp:49`) | the camera/scene getter (`mov eax,[scene]; add eax,154h; ret`) | `A1 ?? ?? ?? ?? 05 54 01 00 00 C3` | `operand(site+1)` = the scene global (`g_scene_at`), read each frame for the scene projection (`+0x154`, its `_11`/`_22`); not patched |
| `kLaidCall` (`damagetext.cpp:73`) | the call that lays the scene onto the frame (after `SetRenderState(ZWRITEENABLE,1)`: `push 1; push 0Eh; mov eax,[edx+0Ch]; push eax; mov ecx,[eax]; call [ecx+0C8h]`; then `mov ecx,<renderer>; push ebx; push ebx; call`) | `6A 01 6A 0E 8B 42 0C 50 8B 08 FF 91 C8 00 00 00 8B 0D ?? ?? ?? ?? 53 53 E8` | only when `damage.ontop` and the glyph hooks worked; `site+24` (`kLaidCallAt`) call redirected to `laid`, which draws the kept quads first, then calls the game's. Dropped back to drawing at once (`g_keep` off) if kept quads go unlaid for 3 frames |

**CatsEyeXI address:** not recorded for any of these (the log prints `call - game` for the layout call and the laid call at run time). `kLifeCompare`'s function is `FFXiMain.dll+0x41E30` (below).
**Retail:** `??` over the push of the 0.1 constant, call targets, renderer and scene globals, vertex stack offset. `kDrawCall` is the same pattern as nameplates'. Literals that matter: `add edi,198h` (popup text), `[esp+20h]`, vtable `+0xC8` (SetRenderState). In both builds per the audit (none is among the four it lists as CatsEyeXI-only). `kPopupCall` not found: whole feature off, log line. Glyph/draw not found: `sharp`/`ontop` off, log line. Scene/renderer not found: popups keep the game's projection, log line. `kLaidCall` not found: ontop numbers draw under the names, log line. Fragile: 0x400 search window; the layout must be a plain `E8` call target.

#### Damage number lifetime (`src/core/damagetext_life.h`, included by `damagetext.cpp`)

**Setting:** `[graphics] damage.duration` (seconds, default 2.5 = the game's 150 frames); hooked only when it is not the game's value.
**What it does:** in the update of a number's maker object (FFXiMain.dll+0x41E30 in CatsEyeXI) three float operands are rewritten, once, to point at VanaCore's `g_life` (replaces the `150.0` the age is compared with) and `g_life_share` (replaces the `1/150` the rise is scaled by, in two places). It first reads what the operands point at and refuses unless the compare holds exactly `150.0` and the share times 150 is within 1e-4 of 1. The popup's own countdown (`+0x110` left of `+0x114`) is stretched at its first draw. Not restored; setting the game's value again just makes `g_life` 150 again.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kLifeCompare` (`damagetext_life.h:13`) | the maker's age update and its compare with 150.0 (`fst [esi+0ACh]; je; mov ecx,[esi+0B4h]; test ecx,ecx; je; fcomp [150.0]; fnstsw ax; and eax,100h`) | `D9 96 AC 00 00 00 0F 84 ?? ?? ?? ?? 8B 8E B4 00 00 00 85 C9 0F 84 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? DF E0 25 00 01 00 00` | `site+28` (`kLifeCompareAt`) is the `fcomp` operand, a pointer rewritten to `&g_life` |
| `kLifeShare` (`damagetext_life.h:16`) | the two `fld [esi+0ACh]; fmul [1/150]` in the update | `D9 86 AC 00 00 00 D8 0D ?? ?? ?? ??` | the 1st and 2nd match (`nth` 0 and 1) within 0x100 after the compare; `site+8` (`kLifeShareAt`) operand of each `fmul` rewritten to `&g_life_share`; both must hold the same pointer |

**CatsEyeXI address:** `FFXiMain.dll+0x41E30` (the update function; comment). Operand addresses are logged at run time.
**Retail:** `??` over the jump sizes and the three float addresses; `+0xAC` (age) and `+0xB4` in the pattern are literal. In both builds per the audit. The value check (150.0, 1/150) guards against a build where the constants differ. Not found or values differ: numbers keep the game's life, log line. Fragile: `+28`, `+8` hard-coded; `+0xAC`/`+0xB4` maker fields; popup `+0x110`/`+0x114`.

#### Damage numbers: other hard-coded game offsets and non-pattern game access (`src/core/damagetext.cpp`, `damagetext_look.h`)

- Popup object: text at `+0x198` (`kPopupText`; the layout is handed `popup+0x198`, the popup is recovered by subtracting it); target `+0xFC` is a pointer to a pointer to the target's handle, whose server id is `+0x58` and target index `+0x54`; own life `+0x110`/`+0x114`.
- Renderer object: `+0x9BC` holds the 16-float projection VanaCore reads and temporarily swaps `_11`/`_22` in to agree with the scene's (`kRendererProjection`); `+0xC` is the device (`kRendererDevice`); scene projection at scene global `+0x154` (also in `kSceneGetter`).
- Digit height 18 and "Miss!" height 15 units, glyph alpha 136: the game's data, not read from code. The `menu    dmgnum` sprite set is read from `ROM\119\51.DAT`.
- Server action packet `0x028` (received buffer header `0x1C`) is parsed in `on_received`; not game code. Critical message ids 67/353, effect `0x22`, are from the server protocol.
- `damagetext_look.h`: look settings only, no patterns or game memory.

### Frame rate, sound and game data

#### Frame rate cap (`src/core/framerate.cpp`)

**Setting:** `[client] fps.max`, `fps.cutscene`, `fps.background`, `timer.resolution`; `[graphics] vsync`, `vsync.cutscene`. The two patches themselves are made at the first Present whatever the settings are.
**What it does:** Replaces the game's "FPS limit" setter (`FPS 制限[%d]`, `void __cdecl(int)`) with a jump to VanaCore's `set_divisor` (`imports::jump`), so the divisor in the game's global and in its config object follows fps.max. It also rewrites the `74 0F` (`je +0F`) at the start of the game's own frame wait to `EB xx` (a short `jmp` to where both wait loops end), so VanaCore's own limiter holds the rate. Neither is put back once made; there is no undo.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSetDivisor` (`framerate.cpp:34-35`) | the game's FPS limit setter | `8B 44 24 04 8B 0D ?? ?? ?? ?? 81 EC 00 01 00 00 3B C1 74 ?? 8B 0D ?? ?? ?? ?? 50 8D 54 24 04 68 ?? ?? ?? ?? A3 ?? ?? ?? ?? 52 89 41 ??` | jump-patched to `set_divisor`; `operand` at +6 (`kLastAt`) = the "last divisor" global; +22 (`kConfigAt`) = the global holding the config object; byte at +44 (`kFieldAt`) = the divisor's field offset in the config object (the `??` in `89 41 ??`) |
| `kWait` (`framerate.cpp:47`) | the game's frame-cap wait loops (fcompp/fnstsw, `Sleep(1)` loop, then `Sleep(0)` loop) | `DE D9 DF E0 25 00 01 00 00 74 0F 6A 01 C6 44 24 1B 01 DD D8 FF D5 F3 90 EB AC 80 7C 24 17 01 74 57` | bytes at `site+9` (`kWaitJump`, `74 0F`) replaced with `EB rel8`, the target worked out from the second `je` at `site+31` (`kWaitSecond`: `74 57` -> its destination). Skipped if the rel8 doesn't fit |

**CatsEyeXI address:** the setter and wait addresses are only logged at run time (`FPS: took over ... FFXiMain.dll+0x%X`); not recorded in source.
**Retail:** both patterns are fully literal except the three global/field slots, so any compiler change in these two functions breaks them. Not found: the setter case logs `FPS: couldn't find the game's frame rate setting` and the game keeps its own 30 FPS cap; the wait case logs `couldn't find the game's frame rate wait` and the game keeps its own wait. Fragile: `kLastAt`/`kConfigAt`/`kFieldAt` and the `kWaitJump`/`kWaitSecond` offsets are hard-coded distances into the match; the divisor field offset is read from the code (`kFieldAt`), not assumed. The game's normal divisor 2 (30 FPS) and `kGameRate` 60 are assumed.

#### Frame pacing (`src/core/framepacing.cpp`)

**Setting:** `[client] framepacing`, `framepacing.ui`, `framepacing.camera`, `autorun.walk` (default off). Each group of patches goes in while its setting is on and is put back (`take_out`) when it is turned off. Also holds the FrameFix plugin off while any of motion/ui/camera is on.
**What it does:** Finds the game's tick function (`GameManager::CheckTick`) and the call sites that read it, then rewrites those calls to VanaCore's own (`E8 rel32` to ours plus NOPs) so that at 60 FPS motion, the frame step, the interface, the chat, and the camera's loops count by the frame's real time instead of the game's cut tick. Walk-or-run gets its own tick for the player on auto-run. All the patches are reversible: `Patch` keeps the game's original bytes.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kTick` (`framepacing.cpp:22`) | the tick function itself (33 bytes, `kTickSize`) | `8B 0D ?? ?? ?? ?? D9 41 28 D8 1D ?? ?? ?? ?? DF E0 F6 C4 05 7A 07 D9 05 ?? ?? ?? ?? C3 D9 41 28 C3` | not hunted for directly: `is_tick(call_target(...))` checks the target of the calls below is this function; `tick+2` read back as the address of the game manager global (`g_game_at`) |
| `kMotionFrame` (`:31`) | `AnimationTrack::UpdateFrame`'s call to the tick | `E8 ?? ?? ?? ?? D8 4C 24 10 D8 46 24 D9 5E 24` | call at `site` rewritten to `motion_tick` (5 bytes); its target is taken as the tick |
| `kMotionBlend` (`:32`) | `AnimationTrack::UpdateBlendState`'s call to the tick | `51 56 8B F1 E8 ?? ?? ?? ?? D9 5C 24 04 33 C0 B9 00 00 80 3F 8A 46 08` | call at `site+4` (`kMotionBlendCall`) rewritten to `motion_tick`; its target must equal the first's |
| `kStep` (`:39`) | render manager frame timing's call to the tick (stores the step at `[esi+0EB0h]`) | `E8 ?? ?? ?? ?? D9 9E B0 0E 00 00 5F 5E 83 C4 08 C3` | call at `site` rewritten to `motion_tick`; target must equal the tick. All three (motion frame, blend, step) go in together or none |
| `kUiRoot` (`:46`) | the interface root window's update, matched from its 6th byte on | `56 57 8B E9 E8 ?? ?? ?? ?? 66 8B 45 56 33 FF 66 3B C7 7E 05 48 66 89 45 56` | call at `site+4` (`kUiRootCall`) rewritten to `ui_update`, which runs the original again `g_extra` times |
| `kChatRoot` (`:48`) | the chat's update call in the main interface pass | `8B 0D ?? ?? ?? ?? 3B CB 74 05 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 38 58 08` | call at `site+10` (`kChatRootCall`) rewritten to `chat_update` |
| `kChatWindows` (`:52`) | the two chat log window globals (as FancyChat finds them) | `A1 ?? ?? ?? ?? C6 40 59 01 8B 0D ?? ?? ?? ?? C6 41 59 01 C2 08 00` | `site+1` and `site+11` (`kChatWindowAt`) read as the addresses of the two window pointers; each window's countdown at `window+0x3C` is reduced by the extra 60ths. Only read, not patched. Optional: if missing, only the chat scroll sped-up is lost |
| `kCamera` (`:62`) | `YmCameraManager::Follow` (FUN_1001ec70 = `FFXiMain.dll+0x1EC70`, CatsEyeXI) | `81 EC E0 01 00 00 A0 ?? ?? ?? ?? 56 57 8B F9 84 C0 89 7C 24 10` | three loops patched at `site+0xBD8`, `+0x101C`, `+0x12E8` (`kCameraLoops`): 10 bytes `call tick; call ftol` -> `call camera_steps` + 5 NOPs. Two push-out sites at `site+0xFD8`, `+0xFE8` (`kCameraSideways`): the 4-byte address operand of `fmul [0.125]` is replaced with the address of a float 1.0. Before patching it checks `code[loop]==E8`, `code[loop+5]==E8`, that the call goes to the tick, and `code[push-2..push-1]==D8 0D` |
| `kRatio` (`:75`) | `SkeletalMeshActor::GetMoveVectorRatio` (FUN_100c8680 = `FFXiMain.dll+0xC8680`, CatsEyeXI) | `83 EC 08 56 8B F1 C7 44 24 04 00 00 00 00 8B 06 FF 90 04 03 00 00 84 C0 0F 84 ?? ?? ?? ?? A0 ?? ?? ?? ?? 84 C0 0F 85` | `site+0x1F` (`kRatioAutorun`) read as the address of the auto-run flag; also anchors `kRatioTicks` (must equal `site+0x188`) |
| `kRatioTicks` (`:78-79`) | the two calls to the tick inside that function (run speed, riding speed) | `FF 92 A8 01 00 00 D8 0D ?? ?? ?? ?? D9 5C 24 08 E8 ?? ?? ?? ?? D8 4C 24 08 EB ?? 8B 06 8B CE FF 90 A8 01 00 00 D8 0D ?? ?? ?? ?? D9 5C 24 08 E8 ?? ?? ?? ?? D8 4C 24 08` | calls at `site+0x10` and `site+0x2F` (`kRatioTickCalls`) rewritten to the naked `ratio_tick` (passes `esi`) only while `autorun.walk` is on |

**CatsEyeXI address:** only the two FUN_ names in comments (Follow `+0x1EC70`, GetMoveVectorRatio `+0xC8680`); the rest not recorded in source (the log line `Frame pacing: the game's timing found (motion +0x..., ...)` prints them at run time).
**Retail:** all ten patterns have `??` only over call/global operands; none uses `|`. Every one is a call site rewritten by a fixed offset from the match. The docs audit predates the November additions here, so retail status is not recorded; check the offsets added to the matches (`kCameraLoops`, `kCameraSideways`, `kRatioAutorun`, `kRatioTicksAt`) in any build where Follow or GetMoveVectorRatio is listed as changed. Not found: each group logs `Frame pacing: couldn't find ...` and keeps the game's timing; the motion group is all-or-nothing and the camera needs the tick from another group. The patterns assume the game manager fields `[game+0x28]` (the tick) and `[game+0x2C]` (the uncut 60ths, `kUncut`) and the actor's vtable slot `+0x304` (`kIsControlled`) and `+0x1A8` (in the pattern).

#### Frame timing log and recording (`src/core/frametime.cpp`)

**Setting:** `[logging] frametimes`, or on demand from `/vana debug` recording / `/vana stats` / `/vana trace` (watchers). Off by default; when switched off the timing stops but the hooks are not removed.
**What it does:** No signatures. It hooks `Sleep` and `SleepEx` in the import table of every non-Windows module that is not VanaCore (`imports::hook`, from `KERNEL32.dll`, `api-ms-win-core-synch-l1-2-0.dll`, `api-ms-win-core-synch-l1-1-0.dll`), and replaces every slot from 3 up in the game's Direct3D 8 device vtable (except `Present`) with a generated stub (`push index; jmp timed_common`), so each call's time on the render thread is measured. It also times the vtable slots of throwaway vertex buffer, index buffer and texture objects (the classes the game makes).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| none | | | |

**Game changes not from a pattern:** (1) IAT hooks of `Sleep`/`SleepEx` in all loaded and later-loaded game modules (`frametime.cpp:104-107`); (2) the game's D3D8 device vtable slots 3..end rewritten to stubs in a `VirtualAlloc`'d block (`:418-425`), never restored; (3) vtable slot 11/12 (Lock/Unlock) of `IDirect3DVertexBuffer8` and `IDirect3DIndexBuffer8`, `d3d8::slot::TextureLockRect`/`TextureUnlockRect` of textures (`:436-451`); (4) calls the device by hard-coded slot indices 23 (`CreateVertexBuffer`), 24 (`CreateIndexBuffer`) and `slot::CreateTexture`, with D3D8 constants (usage `0x208`, FVF `0x42`, format `101`). These are standard D3D8 numbers, not FFXiMain.dll's.
**CatsEyeXI address / Retail:** not applicable (no FFXiMain.dll code touched).

#### World and interface sound volume (`src/core/soundmix.cpp`)

**Setting:** `[client] sound.world`, `sound.interface` (set from the Config > Gameplay window rows, see "Gameplay window" below, via `soundmix::set`). Hooks are made the first time either share is under 100; they stay hooked afterwards (the shares go back to 100, which passes volumes through).
**What it does:** Detours three of the game's sound functions (`imports::detour`: the menus' sound call, the sounds' request, a playing sound's volume) and scales the volume argument by the share when the call came from a known site (return address matched against sites found by pattern). Menu sounds scale by the interface share; world and event sounds by the world share.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kUiPlay` (`soundmix.cpp:23-24`) | the menus' sound call (FUN_10036d10 = `+0x36D10`), `__thiscall`, `ret 8` | `83 EC 10 0F BF 44 24 14 8B 54 24 18 89 44 24 14 DB 44 24 14 8D 44 24 00 52 50 C7 44 24 10 00 00 00 00 D9 5C 24 08 C7 44 24 0C 00 00 00 00 C7 44 24 14 00 00 00 00 E8 ?? ?? ?? ?? 83 C4 10 C2 08 00` | detoured (first 8 bytes, `kUiPrologue`); call at `site+0x36` (`kUiRequestCall`) read as the request function (FUN_10036d60 = `+0x36D60`) |
| `kMenuSound` (`:37-39`) | each of the fourteen menu sounds' callers (0x60 apart), found by repeated scan | `51 A1 ?? ?? ?? ?? 56 85 C0 75 2B A1 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8D 4C 24 08 6A ?? 8B 80 08 00 04 00 51 8B 00 8B C8 E8 ?? ?? ?? ?? 8B 00 85 C0 A3 ?? ?? ?? ?? 74 21 8B 30 85 F6 74 1B D9 05 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 6A ?? 8B CE E8` | the final `E8` at `site+0x52` (`kMenuSoundCall`) must call the UI function; its return address (`+0x57`) is stored. Exactly 14 must be found (`kMenuSounds`) or the whole feature stays off |
| `kWorldKept` (`:46`) | the world sound request whose sound is kept at `+1E0h` of its owner | `8B 09 85 C9 74 0D 50 52 E8 ?? ?? ?? ?? 89 86 E0 01 00 00 C7 86 E4 01 00 00` | call at `site+8` must go to the request; return address kept |
| `kWorldEvent` (`:48`) | the event sound request | `8D 4C 24 04 50 51 8B CE E8 ?? ?? ?? ?? 5E 83 C4 10 C2 18 00` | call at `site+8` must go to the request; return address kept |
| `kWorldVolume` (`:50`) | the volume set for the kept sound (calls FUN_10038280 = `+0x38280`) | `8B 96 E0 01 00 00 6A 00 50 52 E8 ?? ?? ?? ?? 83 C4 0C 32 C0 5E 83 C4 20 C3` | call at `site+10` (`kWorldVolumeCall`) read as the volume function, detoured (first 6 bytes); return address kept |

Not scanned, only byte-compared to confirm a target: `kRequestStart` = `A0 ?? ?? ?? ?? 56 84 C0 8B F1` (first byte `A0`, bytes 1-4 skipped; detour of its first 5 bytes) and `kVolumeStart` = `8B 54 24 04 85 D2 7C` (`soundmix.cpp:29,32`).
**CatsEyeXI address:** menus' call `+0x36D10`, request `+0x36D60`, volume function `+0x38280` (from the FUN_ names in comments; also logged at run time).
**Retail:** only `??` over calls and globals. Each stage fails to a distinct log line (`Sound: <menus' sound call / sounds' request / world's sound requests / world's sound volume / fourteen menu sounds> isn't as VanaCore knows it...`) and the feature does nothing. Fragile: the `+0x1E0`/`+0x1E4` owner fields inside `kWorldKept`/`kWorldVolume`, the sound-count of exactly 14, the `0x60` spacing (comment only), and fixed offsets `0x36`, `0x52`, `8`, `10`. `kMenuSound` has a literal `2B` and `21`/`1B` short-jump distances and `08 00 04 00` field offset in it.

#### Sound slots (`src/core/soundslots.cpp`)

**Setting:** always; `[ffxi] sound.max_sounds` (registry value `0029`, 12-100; `registry.cpp:83`) is the cap, applied live by `reload()`.
**What it does:** When the game starts its sound thread (seen as a `CreateThread` call whose start matches `kThread`, via an IAT hook of FFXiMain.dll's `CreateThread`), rewrites the 20-slot sound setup to 100 slots: the allocation size, slot counts, the clear, and 26 loop ends `0x1310`. Then swaps the 20-slot block the game made for a bigger one from the game's own `new`. The game's "max sounds" global is set to `sound.max_sounds` (up to 100) and the cap's forced `14h` to that, never under 20, at the start and on each settings reload. Also detours a world sound's position (`kPosition`, FUN_10038250) and stop (`kStop`, FUN_100369F0) to send a handle below 0 (a sound that couldn't start, which the sound thread takes as every sound) as 0. Not undone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSetup` (`soundslots.cpp:27`; `kSetupSize=1`, `kSetupNew=27`, `kSetupCount=44`, `kSetupLoop=49`, `kSetupSlots=77`) | the sound setup that makes the slots (FUN_10249F40 = `+0x249F40`) | `68 14 13 00 00 89 2D ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF 89 2D ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 3B C5 74 ?? 8D 78 04 C7 00 14 00 00 00 BE 14 00 00 00 8D 97 88 00 00 00 8B CA E8 ?? ?? ?? ?? 81 C2 F4 00 00 00 4E 75 F0 89 3D` | immediates written: `+1` (alloc `4+20*0xF4`), `+44` (count stored), `+49` (loop count); `+27` call read as the game's `new` and called; `+77` operand = the global holding the slots pointer, read and replaced by the new block |
| `kClear` (`:32`) | the clear of the slots just after the setup (searched within `0x400` bytes of `kSetup`) | `B9 C4 04 00 00 33 C0 F3 AB 8B 3D` | dword at `+1` (`4C4h`) rewritten to `slots*0xF4/4` |
| `kThread` (`:35`) | the sound thread function (FUN_102441D0 = `+0x2441D0`), tested at the start address passed to `CreateThread` | `56 8B 74 24 08 8B 46 08 85 C0 74 ?? 57 8B 3D ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 6A 10 FF D7` | match must start exactly at the thread's start; triggers `make_slots` once |
| `kCap` (`:38-39`; `kCapSetting=2`, `kCapForced=25`) | the "max sounds" check (FUN_10038140 = `+0x38140`) | `8B 3D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? A0 ?? ?? ?? ?? 84 C0 74 05 BF 14 00 00 00 8B 0D ?? ?? ?? ?? 6A FF 6A FF 6A 05 E8` | `+25` (`mov edi,14h`) immediate rewritten to max(cap, 20); `operand(+2)` = the max-sounds global, written with the cap |
| `kPosition` | a world sound's position, command 3 (FUN_10038250 = `+0x38250`) | `56 8B 35 ?? ?? ?? ?? 85 F6 74 1D 8B 4C 24 0C 8B 44 24 10 50 D9 01 E8` | detoured (first 7 bytes); a handle below 0 sent as 0 |
| `kStop` | a world sound's stop, command 5 (FUN_100369F0 = `+0x369F0`) | `A0 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 84 C0 74 0F 8B 0D ?? ?? ?? ?? 85 C9 74 05 E9 ?? ?? ?? ?? C2 08 00` | detoured (first 5 bytes); a handle below 0 sent as 0 |

Not a pattern: `find_walks` scans every byte from `setup - 0x6000` to `setup + 0x1000` (`kWalksBefore/After`) for the dword `0x1310` (`kGameEnd = 20 * 0xF4`) in `lea r,[r+1310h]`, `add r,1310h` or `add eax,1310h` (`walk_end`, opcode and modrm checks). It needs exactly 26 walks (`kWalks`) plus exactly 1 debug list (`add ecx,1310h; add esi,74h`, left at 20), else the feature is skipped. Each is rewritten to `slots*0xF4`.
**CatsEyeXI address:** setup `+0x249F40`, thread `+0x2441D0`, cap `+0x38140` (from FUN_ comments); setup also logged at run time.
**Retail:** the `20`/`0x14`, `0xF4` (slot size) and `0x1310` constants are in the patterns and the walk scan, so a changed slot count or struct size means no match; the count of 26 walks is a strong check on any change. The whole feature logs `Sound: <part> isn't as VanaCore knows it in FFXiMain.dll: the game keeps its 20 sound slots` when unfound. Fragile: the `0x6000`/`0x1000` window around the setup, the part-at-`+0x88` loop, and the `-4` count field before the block (`read32(old - 4) == 20`).

#### Vana'diel clock hold (`src/core/vanaclock.cpp`)

**Setting:** always (read each frame by `graphics.cpp:2252`, for the sun's hour while the game's timer is off, cutscenes and the free camera's Time).
**What it does:** Reads, only, the game's Vana'diel date/time global and its "game timer on" flag. When the flag is 0 (a cutscene or the free camera has stopped the clock) it returns the game's held minute so VanaCore's sun uses it instead of the server's hour. Nothing is written or hooked.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kTimer` (`vanaclock.cpp:22-23`) | `EnableGameTimer` then `DisableGameTimer` (`mov byte [flag],1/0`) | `C6 05 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E9 ?? ?? ?? ?? C3 90 90 90 90 C6 05 ?? ?? ?? ?? 00 C3` | `operand(site, 2)` = the game timer flag's address (byte) |
| `kSet` (`:25`) | `SetCurrentDateTime(int)` | `8B 44 24 04 B9 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? C3 8D 4C 24 04 E8` | `operand(site, 5)` = the Vana'diel time global (int32, sixtieths of a Vana'diel second, `kTicksDay = 5184000` to a day) |

**CatsEyeXI address:** from the comment (VAs): time global `FFXiMain.dll+0x627E7C`, flag `+0x375B74`, SysMove `+0x18AD70`, DisableGameTimer `+0x18AFB0`, EnableGameTimer `+0x18AF90`, SetCurrentDateTime `+0x18ACA0`, XiDateTime code from `+0x18A600`.
**Retail:** both globals are read back with `operand()`, nothing hard-coded. Patterns added 2026-11, after the audit, so retail status is not recorded. If either is not found: logs `Vana'diel clock: the game's own wasn't found...` and the sun keeps the server's hour in cutscenes. `kTimer` includes `01` and `00` literal flag values and the `90 90 90 90` padding between two functions, which can change with the build.

#### Game data: entities, party, player, target, pet, missions, job levels (`src/core/game.cpp`)

**Setting:** always. Every `game::` read and the Lua/Ashita compat (`GetEntity`, `GetParty`, `GetPlayer`, `GetTarget`, the pet, missions, job-level getters) hang from `resolve()` here, which runs on first use. All reads are `copy()` (SEH-guarded memcpy); nothing in the game is patched. The job getters are called directly.
**What it does:** Finds each structure by one piece of the game's own code that uses it, reads the address of the structure out of that code's operand (`operand(game, pattern, offset, what)`), then reads fields by hard-coded offsets. Logs `Game data: entities at FFXiMain.dll+0x... (count), party +0x..., player +0x..., target +0x...` once.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kEntitiesPattern` (`game.cpp:21`) | the code `mov edx,[esi+0Ch]; mov eax,[edx+ebp]; mov eax,[eax*4+entities]` | `8B 56 0C 8B 04 2A 8B 04 85` | operand at `+9` (`kEntitiesOffset`) = the array of entity pointers |
| `kEntityCountPattern` (`:24`) | `inc edi; cmp edi,900h; jl` | `47 81 FF ?? ?? ?? ?? 7C ?? 89` | operand at `+3` = number of slots (read from the code; falls back to 0x900 if 0 or over 0x4000) |
| `kPartyPattern` (`:27`) | `lea eax,[ecx*4+members]` | `0F BE C3 8D 0C 52 56 57 8B F5 8D 04 48` | operand at `+23` (`kPartyOffset`) = the first of 18 party members (0x7C bytes each) |
| `kPlayerPattern` (`:30`) | `call ...; mov eax,[player]` | `6A 01 8D 44 24 2C 68 80 80 80 80 66 83 ?? ?? 50 66 83 ?? ?? 51 52 E8 ?? ?? ?? ?? A1` | operand at `+28` (`kPlayerOffset`) = the player's stats (HPMax first) |
| `kTargetPattern` (`:34`) | `mov ecx,[global]` in the target code | `53 56 8B F1 8B 48 04 33 DB 3B CB 75 06 5E 33 C0 5B 59 C3 8B 0D` | operand at `+45` (`kTargetOffset`) = a global; the pointer to the target data (0xD4 bytes, game-allocated) is read from `global + 0x2F4` (`kTargetPointer`) |
| `kPetPattern` (`:40`) | the pet status packet (0x068) handler's copy of the packet | `8B 44 24 0C 56 57 66 8B 48 04 8D 70 04 8B C1 83 E0 3F 83 E8 02` | operand at `+61` (`kPetOffset`) = the game's pet copy (24 bytes) |
| `kJobLevelPattern` (`:44`) | the job level getter, `uint8_t __cdecl(int job)` | `8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 10 73 ?? 25 FF 00 00 00` | the match itself is the function; called directly for each job 0-23 |
| `kJobMasterLevelPattern` (`:45`) | the master level getter | `8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 18 73 ?? 25 FF 00 00 00` | the match itself is the function; called directly |
| `kMissionsPattern` (`:48`) | the mission log packet (0x056) handler | `8B 54 24 0C 56 57 66 8B 42 24 66 3D FF FF 0F 85 ?? ?? ?? ?? 8A 42 04 A2` | operand at `+24` (`kMissionsOffset`) = the nation byte; the missions block starts 6 bytes before it (`kMissionsNation`), 0x28 bytes read |

**CatsEyeXI address:** not recorded (logged at run time only).
**Retail:** every address comes from `operand()`, but four offsets reach well past the end of their pattern, so the operand lies in code the pattern does not check: `kPartyOffset` 23 (pattern is 13 bytes), `kTargetOffset` 45 (pattern is 21), `kPetOffset` 61 (pattern is 21), `kPlayerOffset` 28 (the byte right after the pattern's `A1`, fine). If the instruction stream between differs (as with `kKeyTable`'s register change) the wrong dword is read silently; a missing pattern logs `Game data: <what> not found in FFXiMain.dll`. `docs/retail-signatures.md` says `kTargetPointer = 0x2F4` (the distance between two globals) was **not verified for retail**: check the target reads in game. Hard-coded structure offsets, all assumed identical in retail (the docs say the entity fields `0xD8` and `0x120` were checked, the rest are not recorded):
- Entity: record read `kEntitySize = 0x20C`, the whole record `0x2AC`, tail at `0x2A0` (`kTail`); index `0x74`, server id `0x78`, name `0x7C` (24), x/z/y `0x04/0x08/0x0C`, speed `0x98`, anim speed `0x9C`, distance `0xD8`, heading `0xE4`, hpp `0xEC`, type `0xEE`, race `0xEF`, models `0xFC`, render flags `0x120`, name flags `0x124`/`0x128`, status server `0x16C`, status `0x170`, claim `0x188`, zone `0x18C`, spawn flags `0x1D0`, targeted index `0x1F8`, pet index `0x1FA`, model size `0x204`, hitbox `0x208`, plus the minor fields at `game.cpp:71-74`. Entity slot logic assumes 0x900 slots with player characters from 0x400 (`entity_places`, `gm_places`).
- Party member: 0x7C bytes x 18, with name `0x06`, id `0x18`, target index `0x1C`, hp/mp/tp `0x24/0x28/0x2C`, hpp/mpp `0x30/0x31`, zone `0x32`, flags `0x38`, jobs `0x6D-0x70`, active `0x7A`; alliance record pointer at member `+0` (0x18 bytes, party counts at `0x13`).
- Player: `kPlayerSize = 0x438`, fields listed at `game.cpp:90-97` (e.g. job `0x08`, `kStatusIcons 0x2CC`, `kBuffs 0x3F8`). "read live against the game's on 2026-10-25."
- Target: `kTargetSize = 0x78`, two 0x28 entries, `kIsSubTarget 0x52`, `kLocked 0x5C`, `kMenuOpen 0x74`.
- Pet `0x18` bytes; missions `0x28` bytes (fields at `game.cpp:53-54`).
- Packet-derived (not game memory): linkshell (0x0CC), merits (0x08C), and the jobs fallback from packets 0x00A/0x00B/0x01B.

#### Game config watch and shadow setter (`src/core/game_config.cpp`)

**Setting:** always for the watch (logs changes); the shadows part is on only while VanaCore's sun shadows are on and `[graphics] gameshadows` is not `keep`. Nothing is hooked or patched; the game's `changeConfig` wrapper is called from VanaCore.
**What it does:** Finds the global pointing at the game's config object (`FsConfig`), polls its option table four times a second (`+4` of the object, `kOptions = 0x2394 / 0x2c` entries of `0x2C` bytes: `+4` number, `+8` value, `+0xC` type, type 4 = saved to `cnf.dat`) and logs changes. When the game's own Shadows option (58) isn't Off while VanaCore's sun shadows are on, it calls the game's option setter with `(58, 1)` to turn it off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kGetConfig` (`game_config.cpp:21`) | `FsConfig::getConfig` | `8B 0D ?? ?? ?? ?? 85 C9 74 0B 8B 44 24 04 50 E8 ?? ?? ?? ?? C3 83 C8 FF C3` | `+2` read directly (memcpy, not `operand`) = the global holding the config object pointer |
| `kSetConfig` (`:30`) | `FsConfig::changeConfig`'s wrapper, `int __cdecl(option, value)` | `8B 0D ?? ?? ?? ?? 85 C9 74 10 8B 44 24 08 8B 54 24 04 50 52 E8 ?? ?? ?? ?? C3 83 C8 FF C3` | the match is called as the setter (`g_set`) |

**CatsEyeXI address:** not recorded (logs `the game's options hang from FFXiMain.dll+0x%X` at run time).
**Retail:** `??` only over the global and call operand; the object layout (`+4` table, `0x2C` entry, `0x2394` table length) and option number 58 are hard-coded and not checked against retail. Not found: `Game config: couldn't find the game's options in FFXiMain.dll; changes aren't logged` (the setter is then null and shadows aren't turned off). `kSetConfig` not found sets `g_set` null silently.

#### Game save (`src/core/gamesave.cpp`)

**Setting:** always; called at shutdown (`shutdown.cpp:82`) and by `addons.cpp:1982` (a Lua/addon save). Nothing is patched.
**What it does:** Finds the game's save function (three managers each asked to save if present: the user files', then two more) and calls it on the game's thread inside an SEH guard, so the macro book and set in use (`mcr.sys`) are written as the game's logout would.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSaveAll` (`gamesave.cpp:19`) | the save-all (cdecl, no args) | `8B 0D ?? ?? ?? ?? 85 C9 74 05 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E8 ?? ?? ?? ?? E9` | the match is called directly |

**CatsEyeXI address:** `FFXiMain.dll+0x1A9F30` (VA `0x101A9F30`, "one match in the running game 2026-11-12"); the user files' save-all is `+0x1B8750`; the macro controller's `+0x1DBC` holds the mcr.sys byte.
**Retail:** only globals and calls are `??`. A uniquely matching, very generic shape (three `mov ecx,[g]; test; je +5; call`), so a build with more or fewer managers would not match; added 2026-11-12, after the audit, so retail status is not recorded. Not found: logs `Game save: the game's save of its user files wasn't found...` and returns false.

#### Game events: menu windows (`src/core/gameevents.cpp`)

**Setting:** an addon listening for `menu_open`, `menu_close`, `main_menu_open/close` or `action_menu_open/close` (read only while one does). The other events (login, logout, zone, job, level, target, chat input) come from `game::`, `packets::` and `chat::input_state()` and add no signature of their own.
**What it does:** Reads the game's menu manager's list of open windows (a linked list), following each node's window to its layout name, and compares it between polls. Nothing is written.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kWindows` (`gameevents.cpp:58`) | the code that uses the menu manager (CTkMenuMng), the same place `itemlist.cpp` finds it | `32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8` | dword at `+3` (`kWindowsAt`, `memcpy`) = the address of the manager's first-node pointer (comment: VA `0x105ECB98`) |

**CatsEyeXI address:** `FFXiMain.dll+0x5ECB98` (the manager, VA in comment); the pattern site itself is not recorded.
**Retail:** `??` over the manager's address and calls. Hard-coded structure offsets: node `+0` next, `+0x10` window; window `+4` layout; layout name 16 characters at `+0x46` ("menu    " prefix stripped); at most 128 windows. Not found: `Game events: the game's window list wasn't found in FFXiMain.dll: the menu events never fire`. The layout-name `+0x46` and window `+4` are shared with `gamesettings.cpp` (`kLayoutName`, `kWindowLayout`).

#### Config > Gameplay window extension (`src/core/gamesettings.cpp`)

**Setting:** `[client] gamesettings.extended` (default 0), `sound.world`, `sound.interface` (0-100 each; they feed `soundmix::set` whether or not the window could be put in). Put in only while the setting is on; taken out (layout and sheet removed, game's layout re-added) when it goes off or the menus are let go.
**What it does:** Adds two slider rows (World Sounds Volume, Interface Sounds Volume) to the game's `menu    conf2win` window (366x296, ROM\119\51.DAT; "FFXiMain 2025-12-26, read live 2026-11-23"): builds a new layout payload and a new sprite sheet (`menu    vcgamset`) from the game's own DAT, swaps them in through `configmenu::menus()`'s list functions, and swaps two slots in the window handler's vtable-like table (slot 4 draw, slot 6 input) for VanaCore's `page_draw` and `page_input`. The table pointer at `handler+0` is pointed at a copy (`g_ours`) and pointed back when off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kEntry` (`gamesettings.cpp:63`) | the menus' table entry for `menu    conf2win`, found as data (the name, then 16 zero bytes) | `6D 65 6E 75 20 20 20 20 63 6F 6E 66 32 77 69 6E 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` | `operand(entry, 0x20)` (`kEntryGlobal`) = the global holding the handler object; `handler+0` is its table (17 slots then a null) |

Not scanned, byte-compared once the table is read: `kDrawStart` = `56 8B F1 6A 02 6A 01 8B 46 2C 50 E8` (slot 4's code, FUN_10195ac0 = `+0x195AC0`), `kInputStart` = `53 56 8B F1 57 8A 46 20 84 C0 0F 85` (slot 6's, FUN_10195bc0 = `+0x195BC0`), `kStep` = `8B 7E 08 8B CF E8` (at input `+0x22A`), `kFillStart` = `8A 44 24 08 84 C0 75` (the slider fill, FUN_101952d0 = `+0x1952D0`). From these: fill = call at draw `+0x27` (`kDrawFillCall`); cursor-redraw function = call at step `+5` (`kStepCursorCall`); `step + 0xC` call must equal `configmenu`'s `window_help`.
**CatsEyeXI address:** the handler's table `+0x334980` (set by `+0x195A50`), draw `+0x195AC0`, input `+0x195BC0`, fill `+0x1952D0` (from comment VAs). Not recorded for `kEntry`'s match (it is data).
**Retail:** pattern is data (the menu name), so it survives code changes; the handler's table and the four code starts are checked by `memcmp` and the whole feature stays off with `Game settings: the game's Gameplay window isn't the one VanaCore knows (<what>)` if any check fails. Fragile: hard-coded `kSlots = 17`, slots 4 and 6, `kDrawFillCall 0x27`, `kInputStep 0x22A`, `kStepCursorCall 5`, `kStepHelpCall 0xC`, the window/layout offsets (`kWindowLayout 4`, `kWindowCursor 0x4C`, `kWindowMoves 0x62`, `kWindowKeys 0x85`, `kLayoutParse 8`, `kLayoutName 0x46`, node `+0x10` item and `+0x14` skip, `kHandlerWindow 8`, `kSheetName 0x24`, `kPartHasHelp 0x4A`, `kPartHelp 0x4C`, the layouts list at manager `+0x1C`), and the layout's byte format (header `0x20`, count at `0x11`, 11 items). Also needs the English `ROM\119\51.DAT` to match byte-for-byte, so another language or a different DAT leaves the window alone. Added 2026-11-23, after the audit; retail status not recorded.

### Chat

#### Command line, chat output and chat line (`src/core/chat.cpp`)

**Setting:** always (installed on the first frame by `install()`, `chat.cpp:2523`); also Lua API `vana.chat`, Ashita compat (`/addon` commands, text events, copy and paste, auto-translate).
**What it does:** Detours the game's command-line function (`chat.cpp:2533`) so typed, macro and menu lines pass through VanaCore first (`/addon`, `/raw`). Finds the game's chat-log writer and calls it to print addon lines, then follows its call (writer+0x26) into the chat log's add-line method and detours that for incoming text events. Reads the chat manager global to get the typed line's state, text and length. Hooks stay on for the session; nothing is restored.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kParsePattern` (`chat.cpp:69`) | the game's command-line function (starts `sub esp,404h`) | `81 EC 04 04 00 00 8B 8C 24 08 04 00 00 55 56 57 8D 84 24 10 02 00 00 68 00 02 00 00 50 33 ED 51 8B 0D ?? ?? ?? ?? 89 6C 24 18 E8` | detour, 6-byte prologue (`kParsePrologue`) |
| `kWritePattern` (`chat.cpp:77`) | the chat-log line writer (`void __cdecl(mode, text)`) | `8A 44 24 04 8B 54 24 08 88 44 24 04 33 C0 50 6A 01 8D 4C 24 0C 50 51 8B 0D ?? ?? ?? ?? 52 88 44 24 19 88 44 24 1A E8 ?? ?? ?? ?? C3` | called directly; `site+0x19` = the chat log global's address (`kWriteLogOffset`); call at `site+0x26` (`kWriteCall`) followed, its target must start `81 EC`, then detoured (6 bytes, `kAddLinePrologue`) |
| `kInputPattern` (`chat.cpp:93`) | `mov ecx,[chat manager]` before the `call [edx+28h]` | `8B 0D ?? ?? ?? ?? 6A ?? 53 53 8B 11 53 6A ?? 6A ?? 53 6A` | `site+2` = the chat manager global (`kInputOffset`) |
| auto-translate (`chat.cpp:3204`, inline literal in `parse_auto_translate`) | a caller of the chat manager's auto-translate parser (Ashita's `chat.parseautotranslate.this`) | `8B ?? 83 C9 FF 33 C0 6A 00 F2 AE F7 D1 49 6A 01 51 8B 0D` | needs `site+0x22` to be `E8`; the call's target (`site+0x27+rel`) is called as thiscall on the manager. Scanned over `SizeOfImage` |

**CatsEyeXI address:** the `0x10...` addresses in comments are CatsEyeXI's: command line start not recorded as such; chat manager's input line object `+0x7E30`; typing check `FUN_101b43f0` (FFXiMain.dll+0x1B43F0); add-line method 0x10127740 (FFXiMain.dll+0x127740, `chat.cpp:395`).
**Retail:** all four patterns have `??` only over globals. Not found: the log line says what stops working ("/addon commands won't work", "addon messages go to the log only", "no copy and paste", "phrases stay as they are"). Fragile:
- `site+0x19`, `+0x26`, `+0x22`, `+0x23`, `+0x27` offsets into matches.
- Hard-coded chat manager fields: input state `+0xF108`, text `+0x7E30+0x6C4`, length `+0x7E30+0x888` (`chat.cpp:95-98`). docs say `0x4630-0x2D608` shifted `+0x8` in retail, so these are suspect; not verified in retail.

#### Row spacing (`src/core/chat.cpp`)

**Setting:** `[client] chat.lineheight`
**What it does:** Patches the immediate (`site+2`) in four instructions of the chat window's draw (`add edi,-10h` etc.) to the chosen row height. Also rewrites the window sizing code (`sub edi,8`/`shl edi,4` becomes `imul edi,edi,H`/`add edi,-128`, 3 bytes each at `site` and `site+7`) so a full window is as tall as its lines. Applied when the setting changes; put back to the game's bytes at the default 16.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `g_line_sites[0]` (`chat.cpp:344`) | `add edi,-10h` in chat draw | `83 C7 F0 8B 4C 24 10 8D 54 24 14 8D 44 24 34 52 8B 49 18 50 E8` | `site+2` = `-height` |
| `g_line_sites[1]` (`chat.cpp:345`) | `sub edi,10h` | `83 EF 10 3B EB 7C ?? EB 08 8B 44 24 28 89 44 24 34 E8` | `site+2` = height |
| `g_line_sites[2]` (`chat.cpp:346`) | `add eax,-10h` | `83 C0 F0 89 44 24 2C 8B 44 24 10 8D 4C 24 30 8D 54 24 34 51 8B 48 18 52` | `site+2` = `-height` |
| `g_line_sites[3]` (`chat.cpp:347`) | `sub edx,10h` | `83 EA 10 3B C1 89 44 24 14 89 54 24 2C 0F 8C ?? ?? ?? ?? E8` | `site+2` = height |
| height site (`chat.cpp:2558`) | the window sizing code (`sub edi,8 ... shl edi,4`) | `83 EF 08 8D 4C 24 28 C1 E7 04 66 2B 7E 26` | `site..+2` and `site+7..+9` rewritten (see above) |

**CatsEyeXI address:** chat draw 0x10160da0 (FFXiMain.dll+0x160DA0), sizing 0x10161490 (FFXiMain.dll+0x161490).
**Retail:** no `??` except the jump sizes; all four sites must be found or the feature is off (log line). Fragile: `site+2`, `site+7` offsets; the height patch's bytes are written blind (no check of the original bytes).

#### Timestamp colour (`src/core/chat.cpp`)

**Setting:** `[client] chat.timestamps.color` (and `chat.timestamps*`)
**What it does:** Detours the game's text colour lookup (colour code to ABGR, `__cdecl(id)`) to answer a private colour id with the stamp's colour. Always hooked once found.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| colours (`chat.cpp:2546`) | the text colour lookup | `A0 ?? ?? ?? ?? 33 C9 84 C0 74 ?? 0F BF 54 24 04 25 FF 00 00 00 3B C2` | detour, 9 bytes |

**CatsEyeXI address:** not recorded.
**Retail:** `??` over the global and jump size. Not found: log line, colour setting does nothing.

#### No hanging indent (`src/core/chat.cpp`)

**Setting:** `[client] chat.noindent`
**What it does:** Detours the game's "does this mode indent" lookup (`__cdecl(mode)`); while VanaCore's add-line hook is running it answers 0. Hook is always in place once found; the answer only changes with the setting on.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| indent (`chat.cpp:2550`) | the mode-indent lookup | `A1 ?? ?? ?? ?? 33 C9 66 85 C0 74 ?? 8B 54 24 04 81 E2 FF 00 00 00 0F BF C0 3B C2` | detour, 7 bytes |

**CatsEyeXI address:** add-line 0x10127740, wrapper 0x10129f60, mode table 0x1036c890, indent string 0x1036d128 (comments only); the lookup itself not recorded.
**Retail:** `??` over the table global. Not found: log line.

#### Chat window opacity tracking (`src/core/chat.cpp`)

**Setting:** `[client] chat.opacity` (and the other opacity, colour, border and font settings that need the chat windows' rectangles)
**What it does:** Detours the chat window's draw (`void __fastcall(window, ...6 ints)`) to remember the two window objects and to run `chatfont` drawing around it. Rectangles are read from the object's `+8` window each frame.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| draw_chat (`chat.cpp:2554`) | the chat window's draw | `83 EC 0C A1 ?? ?? ?? ?? 89 4C 24 00 53 55 8A 48 08 56 84 C9 57` | detour, 12 bytes |

**CatsEyeXI address:** FFXiMain.dll+0x160DA0 (0x10160da0, `chat.cpp:2156`).
**Retail:** `??` over a global. Not found: logs "chat window tracking ... not found".

#### Chat window position and width (`src/core/chat.cpp`)

**Setting:** `[client] chat.move`, `chat.x`, `chat.y`, `chat.width`, `chat2.move`, `chat2.*`, also `chat.menus`/`menus.move` read the same globals
**What it does:** Detours the chat layout function (so the place is set again after the game's layout) and calls the game's resize to place the window. Takes the two logs' globals and the UI area's globals out of the calls that use them. Off, the game's layout is called to put the window back.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| layout (`chat.cpp:2564`) | the chat window layout (`mov eax,[UI end]; push esi; push edi; mov edi,[UI origin] ...`) | `A1 ?? ?? ?? ?? 56 57 8B 3D ?? ?? ?? ?? 8B F1 2B C7 05 60 FF FF FF 66 8B 4E 52 66 89 46 32` | `site+1` = UI right/bottom global, `site+9` = UI left/top global; `site+0x5A` must be `E8` (the resize call, its target taken as `g_chat_resize`); detour 7 bytes |
| log1 (`chat.cpp:2565`) | Config log settings' call of the layout after `mov ecx,[log 1]` | `8B 0D ?? ?? ?? ?? 8B 54 24 0C 52 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8` | `site+2` = log 1's global; the call at `site+0x16` must target the layout |
| log2 (`chat.cpp:2566`) | the same for log 2 | `8B 0D ?? ?? ?? ?? 56 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8` | `site+2` = log 2's global; the call at `site+0x12` must target the layout |

**CatsEyeXI address:** layout FFXiMain.dll+0x161850 (0x10161850), UI origin global 0x105ecc14, UI end 0x105ecc18, log 1 global 0x105ecc9c, log 2 0x105ecca0, resize 0x1011a1a0, SetLocate 0x10118d10, Config value `FUN_10192310` (FFXiMain.dll+0x192310).
**Retail:** `??` over the globals; the displacements `60 FF FF FF` and the `0x5A`/`0x16`/`0x12` distances are fixed. Not found: log line, `chat.move` does nothing.
Hard-coded structure offsets: chat window object `+8` = window, window `+0xC` = owner, rect `+0x3A..+0x40`, text width `+0x32`, max width `+0x52`, opened place `+0x42..+0x48`, state `+0x10` (closed 0xE), stored place `+0x8C`. These are not in the docs' shifted ranges as far as listed, none verified in retail. `chat_place.h` only calls the layout/`game_place` and `chat_watch.h` only reads these fields (frame part list `+0x14`, node `+0x10`/`+0x14`, part `+0x1A..+0x24`), no scans.

#### Second chat window follows the first (`src/core/chat.cpp`)

**Setting:** `[client] chat2.move`
**What it does:** Redirects (`redirect_call`) the call of `SetLocate` that puts log 2 on log 1 every frame, to `log2_follow`, which skips it while log 2 is moved. Config option 0xB4 is asked with the game's Config function.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| follow (`chat.cpp:2579`) | `push 0B4h; call Config` and log 2's `SetLocate` call | `68 B4 00 00 00 E8 ?? ?? ?? ?? 83 C4 04 48 74 ?? 48 75 ?? 0F BF 44 24 1A 0F BF 54 24 12 0F BF 4C 24 18 2B C2 0F BF 54 24 0E EB ?? 0F BF 44 24 0E 0F BF 54 24 12 0F BF 4C 24 14 2B C2 0F BF 54 24 16 03 C2 50 51 8B 4E 08 E8` | `call_target(site+5)` = Config function; `call_target(site+0x48)` = SetLocate, call rewritten to ours (permanent) |

**CatsEyeXI address:** 0x101607b8 (the call, FFXiMain.dll+0x1607B8), SetLocate FFXiMain.dll+0x118D10.
**Retail:** `??` over jump sizes and one call. Not found: log line. Fragile: `+5`, `+0x48`. Needs log 2's global from the layout section.

#### Compass position (`src/core/chat.cpp`)

**Setting:** `[client] chat.compass`, `compass.move`, `compass.x`, `compass.y`
**What it does:** Detours the compass's two small setters (left `+0x28`, bottom `+0x2A`), so the game's values are known and ours go in at once. The compass global comes from the pattern.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| compass (`chat.cpp:2591`) | setter of the compass's left (`mov [eax+28h],cx`) | `A1 ?? ?? ?? ?? 85 C0 74 09 66 8B 4C 24 04 66 89 48 28 C3` | `site+1` = the compass global; detour 5 bytes |
| compass_bottom (`chat.cpp:2592`) | setter of its bottom (`mov [eax+2Ah],cx`) | `A1 ?? ?? ?? ?? 85 C0 74 09 66 8B 4C 24 04 66 89 48 2A C3` | `site+1` must equal the same global; detour 5 bytes |

**CatsEyeXI address:** compass (KaCompass) global 0x106321e4; layout 0x1015e730, via 0x1021d4e0; left setter 0x1021d4c0 (FFXiMain.dll+0x21D4C0); chat window layout stand-over 0x1015e6b0.
**Retail:** `??` over the global. Not found: log line. Hard-coded: compass `+0x28` (middle/left), `+0x2A` (bottom), `+0x2C` (height), `+0x2E` (clock flag), `+0xD` (fade state).

#### Compass hide (`src/core/chat.cpp`)

**Setting:** `[client] compass.hide`
**What it does:** Writes a byte 0x24 past a match: 1 as the game has it, 0 to stop the compass drawing. Only written while it holds what VanaCore left (1 or its own 0); put back to 1 when the setting goes off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| compass_switch (`chat.cpp:2601`) | `xor eax,eax; mov ax,[ecx+..]; dec eax; cmp eax,0E3h` | `33 C0 66 8B 81 ?? ?? ?? ?? 48 3D E3 00 00 00` | the byte at `site+0x24` must read 1; patched 1<->0 |

**CatsEyeXI address:** not recorded.
**Retail:** `??` over the field's displacement; the `+0x24` is a hard-coded distance (not verified in retail; the check that the byte is 1 stops a wrong site mostly). Not found: log line.

#### Clock position and hiding (`src/core/chat.cpp`)

**Setting:** `[client] clock.hide`, `clock.move`, `clock.x`, `clock.y`, `clock.align`
**What it does:** Detours the compass's clock draw (`KaCompass::DrawTime`, thiscall `x, y`) and rewrites the call to `YkDrawString` inside it to ours (`clock_text`), so the clock line can be hidden, moved or aligned. Hooks stay on.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| clock (`chat.cpp:2611`) | DrawTime's start (`mov al,[ecx+2Eh]; sub esp,40h; test al,al ...`) | `8A 41 2E 83 EC 40 84 C0 53 56 57 0F 84 ?? ?? ?? ?? E8` | `site+0x50` must hold `68 80 80 80 80 8D 57 1A 51 8D 43 CE 52 50 E8`; detour 6 bytes; `call_target(site+0x5E)` = `YkDrawString`, that call rewritten to ours |

**CatsEyeXI address:** DrawTime 0x1021e8b0 (FFXiMain.dll+0x21E8B0), YkDrawString 0x101c7ee0 (FFXiMain.dll+0x1C7EE0), compass draw 0x1021ea70.
**Retail:** `??` over the `je` size. Hard-coded: `site+0x50` / `+0x5E` and the clock's layout constants (`kClockIcon` 0x44, `kClockText` 0x32, `kClockDown` 0x1A) are CatsEyeXI's. Not found: log line.

#### Clock kept while a menu is up (`src/core/chat_clock.h`)

**Setting:** `[client] clock.always`
**What it does:** Redirects the compass's fade-in and fade-out jumps (`E9`, rewritten as calls) and the compass draw's "is the compass drawn" call. While the setting is on the fade out is left undone so the clock stays; undone fade restored on the next frame if the setting goes off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `fades_pattern` (`chat_clock.h:68-75`, built with the compass global's bytes) | the two tiny compass fade functions, one after the other | `8B 0D <compass global> 85 C9 74 05 E9 ?? ?? ?? ?? C3 8B 0D <compass global> 85 C9 74 05 E9` | `call_target(site+0xA)` = fade in (must start `C6 41 0D 01`), `call_target(site+0x1A)` = fade out (`C6 41 0D 04`); both jumps rewritten with `redirect_call` |
| draw (`chat_clock.h:86`) | the compass's draw, just before its clock call | `0F BF 73 2A 0F BF 7B 28 2B F0 89 7C 24 44 89 74 24 28 E8 ?? ?? ?? ?? 84 C0 75 14 56 57 8B CB E8` | `call_target(site+0x1F)` must be the clock draw found above; `call_target(site+0x12)` = "compass shown" function, its call rewritten to ours |

**CatsEyeXI address:** fade in 0x1021d480 (FFXiMain.dll+0x21D480), fade out 0x1021d490 (FFXiMain.dll+0x21D490), compass draw 0x1021ea70, shown test 0x100ad5c0, HUD hide 0x1015ca10, menu manager's calls 0x1015da40 at 0x1015eee7 and 0x1015f982.
**Retail:** `??` over jump sizes; the compass global is read back from the earlier pattern (fades pattern depends on it being found). Fragile: `+0xA`, `+0x1A`, `+0x1F`, `+0x12`; hard-coded fade state `+0xD` (shown 3, in 1, out 4). Not found: log line, feature off.

#### Hide the chat line's mode tab (`src/core/chat.cpp`)

**Setting:** `[client] input.hidetab`
**What it does:** Turns the `je` (6 bytes, `0F 84 rel32`) at `site+10` that skips the mode tab in the chat line's draw into a `jmp` (`E9 rel32+1; 90`). Restored when the setting goes off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| tab (`chat.cpp:2625`) | `mov [esi+2Ch],edi; mov eax,[esi+30h]; xor edi,edi; cmp eax,edi; je` in `FsTextInputDisplay::OnDrawPrimitive` | `89 7E 2C 8B 46 30 33 FF 3B C7 0F 84 ?? ?? ?? ?? 8D 4C 24 14 66 89 7C 24 14 51 8B 4E 08` | `site+10` patched (`0F 84` -> `E9`), original 6 bytes saved |

**CatsEyeXI address:** OnDrawPrimitive 0x101B6730 (FFXiMain.dll+0x1B6730), site not recorded.
**Retail:** `??` over the jump. Fragile: `+10`, and the field `+0x30` (the shape). Not found: log line.

#### Menus over the chat (chat.menus, menus.move) (`src/core/chat.cpp`)

**Setting:** `[client] chat.menus`, `menus.move`, `menus.x`, `menus.y`
**What it does:** Rewrites two places in the layout code that stands windows over the chat: `movsx ebp,[esp+10h]` (5 bytes at `site`) becomes `call menu_left`, and the `cmp edi,10h; jge; mov edi,10h` (10 bytes at `site+0x63`) becomes `call menu_place` plus NOPs. Written while either setting is on, put back to the game's bytes when both are off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| menu_left (`chat.cpp:2631`) | layout 0x1015e4c0's left read | `0F BF 6C 24 ?? 89 4C 24 24 8B 4F 08 89 44 24 20 E8` | `site` must read `0F BF 6C 24 10` (`kMenuLeftGame`, checked at the call) and `site+0x63` must read `83 FF 10 7D 05 BF 10 00 00 00` (`kMenuPlaceGame`) or the site is dropped; both patched |
| layout_walk (`chat.cpp:2638`) | `mov ecx, manager` before the walk of the layout's windows | `B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 10 74 ?? 8D 54 24 10 B9 ?? ?? ?? ?? 52 E8 ?? ?? ?? ?? 8B F0 85 F6 74 ?? 8B 04 BD` | `site+1` = the layout manager; `manager+0x8C` must hold log 1's global; then `manager+0xA0+slot*2` shorts written (-32768) to force a re-place |

**CatsEyeXI address:** layout 0x1015e4c0 (FFXiMain.dll+0x15E4C0), layout manager global 0x105ecb98.
**Retail:** `??` over the stack offset in the first; the `+0x63` distance and the bytes there are checked but fixed. Not found: log lines ("menu placing ... not found", "window layout ... not found").
Hard-coded: manager `+0x8C` (anchors), `+0xA0` (anchor tops), window `+0x88` marker, `[esp+10h]`/`[esp+18h]` stack layout of the layout function.

#### Chat line position and hide (`src/core/chat.cpp`)

**Setting:** `[client] input.move`, `input.x`, `input.y`, `input.hide`
**What it does:** Reads the global that holds the chat line object and places or moves its window off-screen (via the game's resize). Detours the game's window draw (`CTkMenuCtrlData::OnDraw`) so the line's window is moved before its first draw.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| chat_line (`chat.cpp:2648`) | `mov eax,[line]; cmp eax,ebx; je; lea ebx,[eax+18h]; pop edi; mov [esi+0F104h],ebx` | `A1 ?? ?? ?? ?? 3B C3 74 03 8D 58 18 5F 89 9E 04 F1 00 00` | `site+1` = the chat line global |
| ctrl_draw (`chat.cpp:2654`) | the game's window draw | `83 EC 24 53 55 56 57 8B E9 E8 ?? ?? ?? ?? 66 8B 45 56 33 FF 66 3B C7 7E 05 48 66 89 45 56` | detour, 5 bytes |

**CatsEyeXI address:** chat line vtable 0x103358f8, held at 0x1062e0f8; OnDraw 0x101173d0 (FFXiMain.dll+0x1173D0).
**Retail:** `chat_line` embeds `04 F1 00 00` (`+0xF104`) with no `??`: a chat manager field written into the pattern. The 2026-10-31 audit found it in retail's build (it isn't one of the four that missed), but a build that moves that field loses `[client] input.move` (log line). Window `+0x3A`, `+0x42`, `+0x48`, `+8`, `+0xC` hard-coded.

#### Chat window hide and Escape pin (`src/core/chat.cpp`)

**Setting:** `[client] chat.hide`, `chat2.hide`; fancychat's Lua state (`kFancyChatAsk`, `chat_legacy.h`); `vana.gameui.hide('chat_log')`
**What it does:** Detours the chat window's calc so a pinned log has its line timer (`+0x34`) and `+0x59` zeroed after the game's frame, and redirects the calc's cancel-key check call (`call_target(site+0x1D)`) to `chat_cancel_check`, which says "not pressed" while pinned. Hook stays on; behaviour only changes while pinned. `chat_legacy.h` and `chat_raw.h` contain no scans (Lua strings, `/raw` text parsing).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| chat_calc (`chat.cpp:2658`) | `CTkDrawMessageWindow` calc | `A1 ?? ?? ?? ?? 83 EC 24 55 56 8B F1 8A 48 08 84 C9 0F 84 ?? ?? ?? ?? 8B 16 8B CE FF 52 4C` | detour, 5 bytes |
| cancel (`chat.cpp:2663`) | the calc's cancel-key check (`push -1; push 1; push 95h; push 3Bh; call`) | `B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 6A FF 6A 01 68 95 00 00 00 6A 3B E8 ?? ?? ?? ?? 83 C4 10 84 C0 E9` | requires `cancel - chat_calc == 0x521`; `site+0x1D` call rewritten |

**CatsEyeXI address:** calc 0x101611a0 (FFXiMain.dll+0x1611A0), key check 0x10123670 (FFXiMain.dll+0x123670); the code comment says the check is `0x53e` into the calc (`chat.cpp:1283`) while the code requires the pattern to be `0x521` past the calc start (the call is `+0x1D` further: 0x521+0x1D = 0x53E, consistent).
**Retail:** `??` over the global and jumps. The `0x521` distance is fragile: if retail changed the calc's size before that point the cancel hook is off (log line, calc hook stays). Hard-coded: window `+0x34`, `+0x50`, `+0x59`, `+0x4E`.

#### Chat line history (`src/core/chat.cpp`)

**Setting:** `[client] chat.history`
**What it does:** Detours five small functions of the game's chat line history (`FsHistory`: constructor, add, set, get, reset) and keeps lines in its own deque (up to 100 instead of 9). Only hooks all five, or none.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| history add (`chat.cpp:2359`) | `FsHistory::add` | `53 8B D9 55 57 8D 7B 04 68 4F 05 00 00 8D 83 9B 00 00 00 57 50 E8 ?? ?? ?? ?? 8D AB EC 05 00 00 6A 24 8D 8B F0 05 00 00 55 51 E8` | the other four at fixed offsets from it: ctor `-0x70`, reset `+0x240`, set `+0x110`, get `+0x90`; each's first bytes compared with a copy before any hook; detours of 8/13/10/9/8 bytes |

**CatsEyeXI address:** not recorded.
**Retail:** `??` only over a call. The pattern has `4F 05 00 00`, `9B 00 00 00`, `EC 05 00 00`, `F0 05 00 00` fixed. The four fixed distances and the byte checks protect against a different layout (then "history wasn't found", feature off). Hard-coded: history `+0x614` (count), `+0x618` (selected), `+0x5EC` (lengths), `+0x618` in `get` bytes `18 06 00 00`.

#### Chat window scrollback (`src/core/chat.cpp`)

**Setting:** `[client] chat.scrollback`
**What it does:** Detours the four functions of the saved lines: fetch-older, fetch-newer, history-mode enter and leave, so scrolling up past the 100 rows puts the saved lines' history mode on first and keeps it on. Needs all four; hooks stay on.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| older (`chat.cpp:2505`) | fetch of the line before the top row | `83 EC 44 53 55 8B E9 56 57 C6 44 24 13 00 8B 45 00 85 C0 7F 0C 32 C0` | detour 9 bytes |
| newer (`chat.cpp:2506`) | fetch of the line after the bottom row | `83 EC 44 53 55 8B E9 56 57 C6 44 24 13 00 8B 4D 10 8B 45 04 49 3B C1 7C` | detour 9 bytes |
| enter (`chat.cpp:2507`) | history mode enter | `8B C1 8B 48 0C 8A 51 60 84 D2 74 06 B8 01 00 00 00 C3 8B 50 08 85 D2 7E` | detour 10 bytes |
| leave (`chat.cpp:2508`) | history mode leave | `8B 49 0C 8A 41 60 84 C0 75 06 B8 01 00 00 00 C3 E9 ?? ?? ?? ??` | detour 8 bytes |

**CatsEyeXI address:** chat object global 0x10575FE8; older 0x1012A190 (FFXiMain.dll+0x12A190), newer 0x1012A270 (FFXiMain.dll+0x12A270), enter 0x1018D680 (FFXiMain.dll+0x18D680), leave 0x1018D6B0 (FFXiMain.dll+0x18D6B0), rows rebuild 0x101606E0.
**Retail:** no `??` except leave's jump. Fixed field offsets in patterns (`+0x0C`, `+0x60`, `+0x10`). Hard-coded: ring `+0x64044` (scroller), `+0` top, `+4` bottom, `+0x10` total; scroller `+0xC` pager, pager `+0x60` mode. `+0x64044` is past the docs' shifted ranges (unknown). Not found: log line, off.

#### Selection menu rows (`src/core/chat.cpp`)

**Setting:** `[client] menu.rows`
**What it does:** Patches eight immediates of 3 in the NPC selection menu code (from seven match sites; some sites give two) to the row count, detours a locate, a button lookup, a jump-tag function, the mouse's button pick and the cursor placer, and redirects the menu's open, draw-locate calls to ours. Immediates restored to 3 when the setting is 3. All needed or the feature is off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `g_menu_sites[0]` (`chat.cpp:427`) | immediate in `add edi,3` (open/draw/keys) | `32 0F BF FF 83 C7 03 3B FE 7C 07` | `site+6` = count |
| `g_menu_sites[1]` (`chat.cpp:428`) | `add esi,-3` | `3B FE 7C 07 83 C6 FD 66 89 70 32 66 83 78 32 00` | `site+6` = -count |
| `g_menu_sites[2]` (`chat.cpp:429`) | `cmp edi,3` and a second | `BF FA 2B FE 83 FF 03 7C 07 8D 72 FE` | `site+6` = count; `site+11` = -(count-1) |
| `g_menu_sites[3]` (`chat.cpp:430`) | `cmp ebx,3` | `24 10 74 67 83 FB 03 7D 62 8D 44 24 10 8B CD 50 E8` | `site+6` = count |
| `g_menu_sites[4]` (`chat.cpp:431`) | `lea eax,[edi+3]` | `FF FF EB 56 8D 47 03 6A 01 50 8B CE E8 ?? ?? ?? ??` | `site+6` = count |
| `g_menu_sites[5]` (`chat.cpp:432`) | `lea ecx,[edi-3]` | `FF FF EB 47 8D 4F FD 6A 01 51 8B CE E8 ?? ?? ?? ??` | `site+6` = -count |
| `g_menu_sites[6]` (`chat.cpp:433`) | `cmp eax,3` | `BF C0 2B C2 83 F8 03 7C 3A 41 66 89 4E 32 EB 33 66` | `site+6` = count |
| open_at (`chat.cpp:2680`) | the menu's open call | `8B CE E8 ?? ?? ?? ?? 8B 4E 08 6A FF 53 E8` | `call_target(site+2)` = `Fit`; call rewritten (`menu_opened`) |
| draw_at (`chat.cpp:2681`) | the menu's locate call in its draw | `51 8B 4E 08 52 8B F8 E8 ?? ?? ?? ?? 8B 44 24 16 8B 4C 24 14` | `call_target(site+7)` = Locate (starts `8B 44 24 08 56 50 8B F1`, 8 bytes, detoured); call rewritten (`draw_locate`) |
| resize_at (`chat.cpp:2682`) | the resize call | `66 8B 86 40 05 00 00 51 8B 4E 08 50 E8 ?? ?? ?? ??` | `call_target(site+12)` = Resize |
| rect_at (`chat.cpp:2683`) | the get-rect call | `8B 4E 08 8D 86 40 05 00 00 50 E8 ?? ?? ?? ??` | `call_target(site+10)` = GetRect |
| find button (`chat.cpp:2694`) | window button lookup by id | `51 56 8D 71 14 57 8B CE E8 ?? ?? ?? ?? 85 C0 89 44 24 08 74 25 66 8B 7C 24 10 8D 44 24 08 8B CE 50 E8 ?? ?? ?? ?? 66 83 38 01 75 ?? 66 39 78 3A 74` | detour 8 bytes |
| jump (`chat.cpp:2697`) | jump-to-tag (cursor Down/Up) | `66 8B 54 24 04 56 0F BF 71 18 0F BF C2 57 8D 78 FF 3B F7 5F 5E 7E ?? 66 85 D2 74 ?? 8B 49 04 8D 04 80 8D 44 81 EC 8B 00 66 39 50 12 75 ?? 0F BF 4C 24 08 49 83 F9 03 77` | detour 6 bytes |
| on_button (`chat.cpp:2703`) | `IsOnButton` | `83 EC 08 53 55 56 8B F1 8B 0D ?? ?? ?? ?? 57 89 74 24 10 D9 41 48 D8 1D ?? ?? ?? ?? DF E0 25 00 41 00 00` | detour 8 bytes; own log line if missing |
| place_at (`chat.cpp:2707`) | cursor placement | `8B 44 24 04 85 C0 74 33 8B 51 14 85 D2 74 2C 56 8B 70 0C 8B 36 66 8B 76 06` | detour 6 bytes |
| cursor_at (`chat.cpp:2710`) | cursor re-placing function (`SetCursol` caller) | `83 EC 08 56 57 8B 7C 24 14 8B F1 66 85 FF 7F 05 BF 01 00 00 00 66 8B 46 58` | `site+0x76` must be `E8`; `call_target(site+0x76)` = Reposition |

**CatsEyeXI address:** event choice window open 0x1014cb60 (FFXiMain.dll+0x14CB60), draw 0x1014cf70 (FFXiMain.dll+0x14CF70), cursor 0x1014ce40, keys 0x1014d1a0 and 0x1014d2a0, position routine 0x10118500, IsOnButton 0x101188b0, button cursor 0x101189b0 (clamp), cursor arrow 0x10119a30, re-place 0x10117d70, resource 0x1011ae20, CreateDrawMenu 0x1015d080, query control 0x1015d7f0, from 0x100a3e60.
**Retail:** most patterns have no `??` over the immediate that is patched (the 3 is in the pattern); the menu object's `+0x540` and window `+0x4C`, `+0x58`, `+0x88`, `+0x5E`, `+0x63`, layout `+0x46`, `+0x12`, `+0x16`, menu `+0x24` are hard-coded. `kMenuRowsDefault` 3 is checked implicitly by the patterns. If any pattern or the locate prologue is missing the whole feature is off ("selection menu wasn't found"). Fragile: `site+6`, `site+11`, `+12`, `+10`, `+0x76`; `menu.rows` wide offsets in retail's menu object not verified.

### Fonts and text

#### True Font: widths, chat rows and character reading (`src/core/chatfont.cpp`, `src/core/chatfont_wide.h`)

**Setting:** `[client] chat.font`, `menu.font` and the other `*.font` areas (True Font); the hooks themselves are put in once, whatever the settings (`find_table()` runs every frame until FFXiMain.dll is there).
**What it does:** Finds the game's per-character width table (signed bytes, one per code below 0x440, in `.data`) and keeps a copy; while a chat row is drawn or wrapped (`Drawing`, `hold()`) it writes True Font's own widths over the game's table, then puts the game's back (a data write, not code). Detours the chat row draw (to know a row is being drawn) and the chat window's re-wrap of a log line (so the wrap uses the font's widths too). The game's character reader is only *called* (to learn which Shift-JIS pair is which font-sheet cell), not hooked. Hooks pass straight through when the setting is off; none are removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kWidthTable` (`chatfont.cpp:39`) | the pen advance function's `cmp ax,440h` / `movsx` read of the width table | `66 3D 40 04 73 17 0F BF C0 0F BE 88 ?? ?? ?? ??` | `operand(site, 12)` = the table's address (copied into `g_game`, 0x440 bytes; overwritten and restored by `hold()`) |
| `kDrawRow` (`chatfont.cpp:44`) | the game's one-chat-row draw (cdecl, 22 dwords) | `8B 44 24 58 8B 4C 24 54 8B 54 24 50 50 8B 44 24 50 51 8B 4C 24 50 52 8B 54 24 50 50 66 0F B6 44 24 18` | `imports::detour(row, 12, draw_row_hook)`; the hook wraps the call in `Drawing` |
| `kWindowRows` (`chatfont.cpp:261`) | a chat window taking a log line as its rows (`__thiscall`, ret 0x14) | `B8 3C 12 00 00 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 53 55 56 8B F1 33 DB 0F BF 48 32` | `imports::detour(rows, 5, window_rows_hook)`; width table held while it runs |
| `kReadCode` (`chatfont_wide.h:15`) | the game's one-character text reader (cdecl: `char**`, returns the font-sheet cell) | `56 8B 74 24 08 57 8B 3E 57 E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 0F 8B 16 33 C0 42 89 16 8A 07 5F 83 E8 20 5E C3` | not hooked: called directly for every Shift-JIS lead/trail pair (0x81-0xFC, 0x40-0xFC) to build a cell-to-character map (`find_cells()`, once) |

**CatsEyeXI address:** `FFXiMain.dll+0x11D440` (pen advance, near kWidthTable's site; the table is at `+0x36A430`; the width function `+0x11CAC0` reads it too), `+0x160BB0` (kDrawRow), `+0x128BD0` (kWindowRows), `+0x11CA80` (kReadCode). kWidthTable's exact site: not recorded.
**Retail:** no `??` over fixed data: the table address is read back with `operand(site, 12)` (good). kWindowRows has `??` over `__chkstk` and the global. `kWindowRows`' `0F BF 48 32` and `B8 3C 12 00 00` (stack size 0x123C) are literal. Not found: a log line each (`Chat:` ...), feature degrades (log maximized keeps game letters, no shadow; long lines may wrap twice; widths stay the game's and True Font for chat/menu does nothing). kWidthTable missing also means `g_table == nullptr` so nothing is built.
Fragile: `0x440` table length hard-coded (`kWidths`), `kFullWidth = 14` (the game's advance for cells past the table, from `0x11D440`); ASCII cell = byte - 0x20, cells 153/154 for `[`/`]`, all derived from the game's reader at runtime except those two cell numbers.

#### True Font: menu, labels, help line and tooltip hooks (`src/core/chatfont.cpp`)

**Setting:** `[client] menu.font`, `help.font`, `buttons.font`, `titles.font`, `status.font`, `buffs.font`, `macrobar.name.font`, `tips.font.*`, `tips.font.lineheight` etc. (True Font areas); `tips.extended`; `equip.font=hide`, `macrobar.prefix=0`. Detours are put in once at start whatever the settings and are pass-through when the area is off; none are removed.
**What it does:** Detours the game's letter printer, the macro menus' name printer, the help line's message draw, the two label-sheet text draws (centred and right-aligned) and the item details' `SetLines`, so the graphics layer knows which letter or text is being drawn (and, for `SetLines`, so the details box can be made taller/wider). For the macro menus' names it also *calls* the game's menu-font string printer's routines, found by reading call targets out of `kPrintString`.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSetLines` (`chatfont.cpp:55`) | `CTkItemInfo::SetLines` (`__thiscall`, ret 4), item details' line count | `8B 44 24 04 56 8B F1 57 8B F8 3B 86 6C 03 00 00 74 ?? 83 F8 03 7D 07 BF 03 00 00 00` | `detour(lines, 5, set_lines_hook)`; also driven by `itemtips::set_lines` |
| `kLetter` (`chatfont.cpp:65`) | the game's letter printer (`__thiscall`, ret 0x14; one font-sheet cell at a pen position) | `8B 44 24 08 83 EC 10 DB 00 53 55 56 8B F1 D8 25 ?? ?? ?? ?? 57 D9 56 38 D9 1E D9 44 24 30 D8 0D ?? ?? ?? ?? DA 00` | `detour(letter, 7, letter_hook)` |
| `kKeytopPrint` (`chatfont.cpp:76`) | the macro menus' name printer (keytop sprites, `__thiscall`, ret 0x14) | `53 8B D9 8B 43 04 85 C0 75 07 83 C8 FF 5B C2 14 00` | `detour(keytop, 6, keytop_print_hook)`; needs `find_menu_font()` to succeed or the hook is not put in |
| `kPrintString` (`chatfont.cpp:86`) | the menu font's whole-string print routine (renderer `__thiscall`, ret 8) | `83 EC 08 8B 44 24 10 53 55 8B 6C 24 14 8B 50 04 56 8B F1 8B 08 89 4C 24 0C 57 8B CE 89 54 24 14 89 6C 24 20 E8 ?? ?? ?? ?? 8B CE E8` | not hooked: the E8 calls at `site+0x24` (states), `+0x2B` (bind), `+0x50` (next code), `+0x93` (print letter), `+0x9E` (advance) are each checked `== E8` and their targets called by VanaCore (`call_target`) |
| `kTitleText` (`chatfont.cpp:91`) | `CTkTitle::OnDrawPrimitive`'s use of the text renderer global | `8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8D 8E A4 00 00 00 C7 40 64 80 80 80 80` | `*(title + 2)` = address of the renderer pointer; quad corner colours written at renderer `+0x10/+0x2C/+0x48/+0x64` (set to the name's colour, put back to 0x80808080) |
| `kHelpDraw` (`chatfont.cpp:130`) | `KaHelpMes`'s message draw (`__thiscall`) | `83 EC 44 53 8B D9 8B 43 64 85 C0 74 1E 8B 43 5C 6A 01 50 8B 43 60` | `detour(help, 6, help_draw_hook)`; reads the byte at `this+0x3A0` (0 still, 1/2 scrolling) |
| `kCentredText` (`chatfont.cpp:140`) | the text draw that can centre (cdecl, 7 args) | `83 EC 08 53 8B 5C 24 18 55 56 85 DB 75 0C 0F BF 44 24 18` | `detour(centring, 8, centred_text_hook)` |
| `kRightText` (`chatfont.cpp:152`) | the right-aligned text draw (cdecl, 7 args) | `51 56 8B 74 24 14 85 F6 75 08 0F BF 44 24 0C 5E 59 C3 53 57 6A 00 E8` | `detour(right, 6, right_text_hook)` |

**CatsEyeXI address:** `FFXiMain.dll+0x13BCC0` (kSetLines), `+0x11B7D0` (kLetter; `+0x11B7A0` and `+0x11BB40` named), `+0x1B7EC0` (kKeytopPrint), `+0x11BA10` (kPrintString, `FUN_1011ba10`), `+0x21FB90` (kHelpDraw), `+0x1C7F10` (kCentredText), `+0x1C82B0` (kRightText); kTitleText: not recorded (CTkTitle::OnDrawPrimitive).
**Retail:** only `??` over the two item-data-independent globals in kLetter and kTitleText; no alternatives; nothing read with `operand()` except `*(title + 2)`. Not found: each logs a `True Font: ...` line and that one feature is lost (letters known by shape alone; macro names keep game letters; scrolling help laid as still; timers not centred; skill numbers laid as words). Docs/retail-signatures.md lists `uiscale.cpp:27` as the one UI function with extra instructions in retail; none of these is named there.
Fragile: `kSetLines` literal `[esi+36Ch]`; the five fixed call offsets from `kPrintString` (`0x24 0x2B 0x50 0x93 0x9E`, the last three past the pattern's end, only checked for `E8`); `kHelpScroll = 0x3A0` field; `kCornerColours` renderer offsets; `kTitleText` literal `0A4h` field and `+0x64`; window layout name at `layout + 0x46` (`Area`, `area_of`, 16 letters like `menu    inventor`); the `kWindows[]` table of layout names (`inventor`, `mnstorag`, `itmstora`, `equip`, `merit` ...) which are data names, not code.

#### Chat line draw (`src/core/chatline.cpp`)

**Setting:** `[client] input.font` (True Font for the chat input line), also the macro editor and text boxes. Hooks put in once at start, whatever the setting; pass-through unless the input area's True Font is on.
**What it does:** Detours the chat line's draw (`FsTextInputDisplay`), the text boxes' twin draw, and the game's plain text draw; checks that each line draw's call to the text draw (at a fixed offset) is really that call. While a line is being drawn in True Font's way, the text draw's pieces are recorded and then drawn as one picture.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDisplay` (`chatline.cpp:31`) | `FsTextInputDisplay`'s line draw (`__thiscall`, ret 0x18) | `B8 44 1C 00 00 E8 ?? ?? ?? ?? 8B 84 24 4C 1C 00 00 53 55 8B D9 56 3D 68 01 00 00 57` | `detour(display, 5, display_hook)`; byte at `site+0x129` must be `E8` calling `kText` (`kCaretCall`), whose return address (`site+0x129+5`) marks the pre-caret piece |
| `kBox` (`chatline.cpp:41`) | the text boxes' line draw (`0x101C4840`, macro editor and text boxes) | `81 EC 5C 0C 00 00 53 55 56 57 8B BC 24 74 0C 00 00 8B D9 81 FF 1A 04 00 00` | `detour(box, 6, box_hook)`; byte at `site+0x10E` must be `E8` calling `kText` (`kBoxCaretCall`) |
| `kText` (`chatline.cpp:47`) | the game's text draw (cdecl: x, y, text, colour) | `8B 44 24 10 8B 4C 24 0C 8B 54 24 08 68 00 00 80 3F 68 00 00 80 3F 6A 44 50` | `detour(draw, 8, text_hook)` |

**CatsEyeXI address:** `FFXiMain.dll+0x1B6DC0` (kDisplay), `+0x1C4840` (kBox), `+0x18F2A0` (kText).
**Retail:** no `??` except the `__chkstk` call; stack sizes `1C44h`/`0C5Ch` and compare limits `168h`/`41Ah` are literals. The two hard offsets `0x129` and `0x10E` are validated by `calls()` (E8 and target == kText). If kDisplay/kText fail, nothing is hooked and `True Font: the chat line's draw wasn't found` is logged; if only kBox fails, the macro editor line is laid as window letters (separate log line). If kDisplay's check fails the box hook is not tried either (returns early).

#### Chat history pages (`src/core/chatpages.cpp`)

**Setting:** always (names the page files after the process id and sweeps dead games' files); `[client] chat.scrollback` (more than 1,000 lines; `chatpages::set_lines`, handed over by `chat.cpp`) drives the pager and the tab-scroll jump. The pager replacements and the page-name patch are made once at the first frame FFXiMain.dll exists, whatever the setting, and not undone.
**What it does:** Replaces eight of the game's chat pager functions (page write, destroy, enter/leave history mode, line, loaded, load, unload) with jumps to VanaCore's own versions holding up to 250 slots; rewrites two call sites (the teardown's and the write's wait-for-file) to call `wait_name`; patches the imm32 format strings in the page-file-name function to point at per-process buffers (`%d.<pid>.log`, `%d_%d.<pid>.log`). With `chat.scrollback` on, flips a `jz` to `jmp` (`74`->`EB`) in the log window's scroll up so every tab (not just Window tabs) scrolls back; restored (`EB`->`74`) when it is off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kName` (`chatpages.cpp:29`) | the page file name function (`sprintf` of `%d.log` / `%d_%d.log`) | `8B 44 24 08 85 C0 75 0D C7 44 24 08 ?? ?? ?? ?? E9 ?? ?? ?? ?? 8B 54 24 0C 52 50 8B 44 24 0C 68 ?? ?? ?? ?? 50 E8` | `site+12` and `site+32` imm32s must point to strings `"%d.log"` / `"%d_%d.log"` then are overwritten (`write_code`) with our buffers |
| `kPagerWrite` (`chatpages.cpp:352`) | the pager's write of a full page | `81 EC 00 01 00 00 56 8B F1 8A 4E 54 80 F9 14 73 4B 8B 46 58 81 E1 FF 00 00 00 03 C1 33 D2 B9 14 00 00 00 F7 F1` | anchor for the others (they are searched `write-0x400` .. `+0x800`); function replaced by `imports::jump`; calls read at `write+0x2F` (page name) and `write+0x43` (file write, which has `page_write+0x5F` a name call rewritten to `wait_name`) |
| `kPagerDestroy` (`chatpages.cpp:353`) | the pager's teardown | `56 8B F1 C7 06 ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 46 54 00 5E C3` | `*(destroy + 5)` = the pager's vtable (`g_pager_vtable`); `called(destroy + 9)` must equal `leave`; replaced by jump |
| `kPagerEnter` (`chatpages.cpp:354`) | history mode on | `8B D1 57 B9 14 00 00 00 33 C0 8D 7A 04 F3 AB B8 01 00 00 00 5F 88 42 60 C3` | replaced by jump |
| `kPagerLeave` (`chatpages.cpp:355`) | history mode off, pages let go | `53 55 8B E9 33 DB 8A 45 54 84 C0 76 30 56 57 8D 7D 04 8B 37 85 F6 74 16 8B CE E8` | calls read at `leave+0x1A` (page close) and `leave+0x20` (free); replaced by jump |
| `kPagerLine` (`chatpages.cpp:356`) | a page's line fetch | `8B 44 24 04 8B 4C 81 04 85 C9 74 0D 8B 54 24 08 52 E8 ?? ?? ?? ?? C2 08 00 33 C0 C2 08 00` | call read at `line+0x11` (page line); replaced by jump |
| `kPagerLoaded` (`chatpages.cpp:357`) | whether a page is read back | `8B 44 24 04 8B 54 81 04 85 D2 0F 95 C0 C2 04 00` | replaced by jump (naked stub keeps `ecx`) |
| `kPagerLoad` (`chatpages.cpp:358`) | reads a page back from its file | `81 EC 00 01 00 00 56 57 8B BC 24 0C 01 00 00 8B F1 8B 44 BE 04 85 C0 75 65 8B 46 58 33 D2 03 C7 B9 14 00 00 00 F7 F1` | calls read at `load+0x31` (page name), `+0x3B` (alloc), `+0x4D` (page new), `+0x61` (page read); replaced by jump |
| `kPagerUnload` (`chatpages.cpp:359`) | lets a read-back page go | `53 8B 5C 24 08 56 57 8B F9 8B 74 9F 04 85 F6 74 18 8B CE E8` | `called(unload+0x13)` must equal page close, `+0x19` must equal free; replaced by jump |
| `kTabScroll` (`chatpages.cpp:417`) | the log window's scroll up test for tab kinds 0 and 8 | `8B 46 54 85 C0 74 09 83 F8 08 74 04 32 DB EB 3C` | byte at `site+5` (`74`) is written `EB` while `chat.scrollback` > 0, `74` when 0 (`apply_tab_scroll`) |

**CatsEyeXI address:** `FFXiMain.dll+0x18CC30` (kName), `+0x18D390` (kPagerWrite), `+0x18D200` (Destroy), `+0x18D220` (Enter), `+0x18D240` (Leave), `+0x18D290` (Line), `+0x18D2B0` (Loaded), `+0x18D2C0` (Load), `+0x18D350` (Unload); kTabScroll is inside the scroll up at `+0x137C20` (its own site: not recorded). Also named: file system TEMP root `+0x1C2510`, reader `+0x18CC90`, teardown `+0x18CBA0`, write wait `+0x18CDD0`.
**Retail:** no `|` alternatives; `??` only over format-string and vtable pointers. The whole pager is *replaced*, so all eight must match and every `called()` cross-check must hold or none is replaced (log `Chat pages: ... weren't found` or `aren't as expected`, game keeps 1,000 lines). kTabScroll and kName failing are separate (only Window tabs scroll back; games share page files).
Fragile: eight hard offsets from matches (`load+0x31/0x3B/0x4D/0x61`, `leave+0x1A/0x20`, `line+0x11`, `write+0x2F/0x43`, `destroy+5/+9`, `unload+0x13/0x19`, `close+0x3A`, `page_write+0x5F`, `at+12`, `at+32`); the pager's layout is hard-coded: 0x64 bytes, slots `+4` (20 of them), written count `+0x54`, base file `+0x58`, window `+0x5C`, mode byte `+0x60`, page object 0xE0 bytes, 50 lines a page, 20 file numbers; the chat object's top line `+0` and total `+0x10` read in `rows_line()`. The chat object is in the big object whose offsets docs/retail-signatures.md says moved (+0x70 for chat `0x40D0C`) but these are relative to the ring/pager, not the big object. Not verified for retail.

#### Chat log tabs (`src/core/chattabs.cpp`)

**Setting:** `[client] chat.tabs.*` (`hide`, `spacing`, `names`, `align`, `width`, `background`, ...) and `chattabs.font` (True Font area `VANA_FONT_CHATTABS`). The hook is put in only once one of those is on (`chat.tabs.hide`, `chat.tabs.spacing` != 30, or the area on) and is not removed.
**What it does:** Detours the function that draws the chat log's row of tabs so VanaCore can lay the tabs itself (hide them, change their spacing, draw flat backgrounds and True Font names), calling the game's own window-rectangle and shape-draw routines from the same function. Reads the game's language setting for the tab names.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kStrip` (`chattabs.cpp:36`) | `FUN_101376C0`, the log's tab-strip draw (`__thiscall`, ret 4); begins with 7 `??` so the start must be checked | `?? ?? ?? ?? ?? ?? ?? 55 56 57 8B F9 50 89 7C 24 10 E8 ?? ?? ?? ?? 8D 4C 24 14 89 44 24 10 33 C0 51 8B 4F 08 66 89 44 24 18 66 89 44 24 1A 66 89 44 24 1C 66 89 44 24 1E E8 ?? ?? ?? ?? 8B 54 24 16 8A 4F 69` | first 7 bytes must equal `83 EC 10 8B 44 24 14` (`kStripStart`), else not found; `detour(strip, 7, strip_hook)`; `site+0x38` (`E8`) = window rect fn (`g_rect`); `site+0xB6` (`E8`) = the plain shape draw (`g_shape`) |
| `kScaledDraw` (`chattabs.cpp:43`) | `FUN_1011F880`, the scaled shape draw (`__thiscall`, ret 0x20); first 5 bytes `??` because `buttons.cpp` hooks it | `?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8` | called directly (`g_scaled`); none found = tabs' pictures drawn at their own size |
| `kLanguage` (`chattabs.cpp:70`) | the getter of the language-dependent menu sheets (`FUN_10259b10`); same pattern as `jplabels.cpp:23` (and statuswindow's) | `8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0F 83 F8 02 74 05 83 F8 03 75 05 B8 01 00 00 00 50 E8` | `operand(site, 2)` = address of the settings pointer (`g_settings`); settings `+8` read (0 = Japanese) |

**CatsEyeXI address:** `FFXiMain.dll+0x1376C0` (kStrip), `+0x11F880` (kScaledDraw), `+0x259B10` (kLanguage, inside it); the log's vtable is `0x10331B68`.
**Retail:** the `?? x7` and `?? x5` prefixes cope with a hooked first instruction or an already-patched start (kStrip is checked against `83 EC 10 8B 44 24 14`, so it fails rather than hooks wrongly). No alternatives. Not found: `Chat tabs: ... weren't found` (settings do nothing); `kScaledDraw` / `kLanguage` quietly optional.
Fragile: two fixed call offsets `0x38` and `0xB6` from kStrip (checked `E8`, not their targets); the chat log object's tab list at `+0x68` (pairs of bytes, end marker 0x0B), pictures at `+0x80`, window at `+8`, tab kind byte `+0x69`; shape layout `+4` list, `+0x14` count, `+0x10` node quad, `+0x20..+0x26` extent, quad name `+0x32` (16 letters); settings `+0x18` and `+8`. Not in docs/retail-signatures.md's list of fixed patterns.

#### Craft log (`src/core/craftlog.cpp`)

**Setting:** always (a diagnostic log of crystal, cluster and synthesis activity in `vanacore.log`, lines `Craft:`); the game's test is found lazily, once, when something first asks (`find_gate()`).
**What it does:** Reads (no hook, no patch) the globals inside the game's "no item can be used now" test and polls them every 250 ms for 15 minutes after a crystal, cluster or synthesis, logging when the test flips. Packets are decoded separately.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kUseGate` (`craftlog.cpp:29`) | `FUN_10097b60`, the item-use gate (`mov eax,[player]; test eax,eax; ...; cmp byte ptr [mode],17h; setz al; ret`) | `A1 ?? ?? ?? ?? 85 C0 74 0D 66 83 B8 ?? ?? ?? ?? 00 74 03 B0 01 C3 80 3D ?? ?? ?? ?? ?? 0F 94 C0 C3` | `u32(site+1)` = player-data pointer global, `u32(site+12)` = flag word offset in it, `u32(site+24)` = mode byte global, `site+28` = the mode value compared with (0x17 on CatsEyeXI); all read with `__try` |

**CatsEyeXI address:** `FFXiMain.dll+0x97B60` (FUN_10097b60, "in the Ghidra export of the 2025-12-26 FFXiMain.dll").
**Retail:** every moving part is `??` and read back (global addresses, flag offset, mode value); the offsets `+1/+12/+24/+28` are fixed from the match start but fall inside the pattern's `??` runs (checked). Not found: `Craft: the game's item-use test wasn't found ...: it isn't watched`. Flag: the character data's flag offset is read from the code, not hard-coded, so a retail shift is covered; the 0x1C packet header and packet ids are in the file but are not game code.

#### Auto-translate phrase list (`src/core/phraselist.cpp`)

**Setting:** always (no key; part of `phrases.font` and fancychat/plugin chat-line placement). Found at the first frame, hooked once the chat line and list exist (first login). Not undone.
**What it does:** Finds the globals holding the chat line object and the auto-translate list (`FsCandidateMenu`), then hooks two slots of the line's *context* vtable (left stays the game's; top slot 1 and caret-offset slot 2 replaced) so the list is placed by VanaCore (at the line, a fancychat bar, or a plugin's line), and slot 4 of the chat manager's vtable (`GetDisplayText`) to change the first `7F FF` before an offered phrase into `7F FE`, so the caret is drawn after the phrase.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kMade` (`phraselist.cpp:28`) | inside `FUN_101b7640`, the chat line's making: `new` of 0x2224 bytes, `FsCandidateMenu` constructor, store to the list global | `68 24 22 00 00 89 46 1C E8 ?? ?? ?? ?? 83 C4 04 3B C3 74 ?? 8B 0D ?? ?? ?? ?? 3B CB 74 ?? 8D 51 ?? EB ?? 33 D2 8B 0D ?? ?? ?? ?? 52 51 8B C8 E8 ?? ?? ?? ?? A3` | `*(made+22)` = global holding the chat line; `*(uint8*)(made+32)` = the context's offset in the line (`lea edx,[ecx+14h]`); `*(made+53)` = global holding the list (the `A3` store's operand, just past the pattern). Also needs `uispace::find(game)` (another file) to succeed |

**CatsEyeXI address:** `FFXiMain.dll+0x1B7640` (the line's making, kMade lies inside it; exact site not recorded). Context vtable `+0x3358E8`; chat manager vtable `+0x335950` (slot 4 `FUN_101b3bd0` = `+0x1B3BD0`); `FUN_1018de60` calc `+0x18DE60`, `FUN_1018dad0` update `+0x18DAD0`; `FUN_101b6c30/6c40/6cd0` the context's left/top/offset (`+0x1B6C30` / `+0x1B6C40` / `+0x1B6CD0`).
**Retail:** `??` over the `new` call, the two globals, `je`/`jmp` distances and the context offset; but `3B C3` / `3B CB` compare against `ebx`, i.e. the register allocation must match (a register change would break it, as `kKeyTable` did elsewhere). Not found: `Phrase list: ... it stays where the game puts it`. Hook failure (the list not seen to use the line's context) logs and stops trying.
Fragile (hard-coded structure fields): list object `+8` (window ctrl), contexts at `+0x2218` / `+0x221C`, height short at `+0x2222`, ctrl `+0xC` back pointer and its rect at `+0x3A`; context vtable slot numbers 0, 1, 2; chat manager `+0x8C81` (list open), `+0xEC34` (kept display text), `+0x7E30 + 0x6C4` (raw text), `+0x7E30 + 0xA58` (caret) (Ashita's `inputtext`); `GetDisplayText` buffer 0x41A. The chat manager's address comes from `chat::input_object()` (chat.cpp, not mine).

#### Macro bar names (`src/core/macrotext.cpp`)

**Setting:** `[client] macrobar.name.font` / `macrobar.font` (True Font areas `VANA_FONT_MACRO_NAMES`, `VANA_FONT_MACROBAR`), `macrobar.prefix`. Hook put in once at start (`macrotext::install`, called from chatfont's `find_table()`), whatever the settings; pass-through when no palette is being drawn or the area is off. Also temporarily changes a button shape's quad count while the key is drawn (restored in the destructor).
**What it does:** Detours the game's fitted text draw (macro names containing a space or symbol) so, while a macro palette window is being drawn, the name is kept as VanaCore's own text instead; and, when a macro key button is drawn, cuts the shape's quad count (`+0x14`) to leave out the game's "Ctrl"/"Alt" letters and digit so VanaCore draws them.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kFitted` (`macrotext.cpp:32`) | `FUN_1018F160`, the fitted text draw (cdecl: x, y, text, colour, room, plain) | `81 EC 1C 04 00 00 83 C9 FF 33 C0 53 55 8B AC 24 30 04 00 00 56 57 8B FD F2 AE` | the first 6 bytes are checked against `81 EC 1C 04 00 00`; `detour(fitted, 6, fitted_hook)` |

**CatsEyeXI address:** `FFXiMain.dll+0x18F160` (kFitted). Related: palette draw `FUN_101A2280` (`+0x1A2280`), keytop printer `FUN_101B7EC0`, window draw `FUN_101173D0` (`+0x1173D0`), `FUN_101B7FA0`.
**Retail:** no `??` at all (the function's whole prologue is literal, including stack size `41Ch` and argument offset `430h`); no alternatives. Not found: `Macro bar: the game's fitted text draw wasn't found ...` (True Font letters laid over the game's).
Fragile: shape layout (`+4` list, `+0x10` node quad, `+0x14` count, `+0x32` quad name, `+0x64/+0x68` texture place, `+0xC0/+0xC8` own corners), picture sheet names (`menu    keytops3`, `menu    menu2fon`, `menu    keytop  `, `menu    gauge`), shape numbers 120-139 and 474-493, the digit sprite's texture coordinates (136, 96, 12 apart; 0 at 3, 107), window layout name at `layout + 0x46`, `kFittedMargin = 14`.

#### Extended macro book and equip set names (`src/core/namechars.cpp`)

**Setting:** `[client] macronames.extended`
**What it does:** Patches one operand byte at each of three sites in the game's code while the setting is on, and writes the game's original value back when it is off (`apply()`, polled every frame): the name text boxes' character set `3` -> `4` (a single byte from space up instead of `isalnum`), and the room passed to the book list's and equip set list's name print `0x4B` -> `0x60`. A site is only touched if the byte currently holds either the game's value or ours.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `g_sites[0]` "the name boxes' character set" (`namechars.cpp:31`) | mode 4's case of the text-input switch, the keyboard set-up call's `push 3` | `8B 86 ?? ?? 00 00 6A 08 50 8B 86 ?? ?? 00 00 50 8D 46 04 50 8B 86 ?? ?? 00 00 50 6A ?? E9` | byte at `site+0x1C` (`??`, the char-set operand) `3` <-> `4` |
| `g_sites[1]` "the book list's room for a name" (`namechars.cpp:36`) | the books' list row: `push 4Bh; push 80808080h; push ebx; call <book name>` | `6A ?? 68 80 80 80 80 53 E8 ?? ?? ?? ?? 0F BF 4C 24 1C 50 83 C1 08 0F BF 44 24 22 83 C0 02 50 51 8B 4E 18 E8` | byte at `site+1` `0x4B` <-> `0x60` |
| `g_sites[2]` "the equip set list's room for a name" (`namechars.cpp:38`) | the equip sets' list row (same, with `edi`) | `6A ?? 68 80 80 80 80 57 E8 ?? ?? ?? ?? 0F BF 4C 24 1A 0F BF 54 24 18 83 C1 02 50 83 C2 08 51 8B 4E 18 52 E8` | byte at `site+1` `0x4B` <-> `0x60` |

**CatsEyeXI address:** the switch is `FFXiMain.dll+0x1C5120` (the case at `+0x1C522D`); the book row at `+0x1A4FDA`, the equip set row at `+0x1A55D4`. Each pattern had one match in the running game, 2026-11-12.
**Retail:** the operand itself is `??` so each site is found whatever it holds; the first pattern has `??` over three field offsets (`[esi+...]`, high bytes literal `00 00`) and the case's character-set byte, the two list patterns have literal stack offsets (`1C`/`22`, `1A`/`18`) and `8B 4E 18`. Not found, or the byte is neither the game's nor ours: logs `Extended names: ... left as the game has it` and that site is skipped. Fully undone when the setting is turned off. Fragile: `site+0x1C` and `site+1` are fixed offsets into the matches (both land on a `??`).

#### Japanese menu labels (`src/core/jplabels.cpp`)

**Setting:** True Font menu-label areas on (`chatfont::labels_on()`: `buttons.font`, `titles.font`, `mainmenu.font`, `action.font`) and the game running in Japanese (settings `+8` == 0). The pattern is scanned once, the first frame the labels are wanted.
**What it does:** Finds the game's settings pointer so it can tell the game's language; no hook or patch. While on, each menu shape's label parts are put together into whole Japanese words for True Font to draw (using the shape and quad layout below).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kLanguage` (`jplabels.cpp:23`) | `FUN_10259b10`, the getter of the language's own menu sheets; same pattern as `chattabs.cpp:70` | `8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0F 83 F8 02 74 05 83 F8 03 75 05 B8 01 00 00 00 50 E8` | `operand(site, 2)` = address of the settings pointer; `settings + 8` read as int each frame (0 Japanese, 1 English, 2 French, 3 German) |

**CatsEyeXI address:** `FFXiMain.dll+0x259B10` (inside it).
**Retail:** `??` over the settings global only; literal `18h` and `08h` field offsets (settings `+0x18` = a flag byte, `+8` = the language) and the language values 0-3. Not found: `True Font: the game's language wasn't found ...: its menus' labels in Japanese stay the game's`. Not listed in docs/retail-signatures.md as changed.
Fragile (shape/quad layout, no pattern): shape `+4` list, `+0x14` count, `+0x10` node quad; quad `+8/+0x0A` size, `+0x0C/+0x0E` place, `+0x2E` blending A (2 = taken away), `+0x32` texture name (16 letters), `+0xC0` own x, `+0xC8` own y; label data tables (`jplabels_data.h`).

#### Help bar (`src/core/helpbar.cpp`)

**Setting:** `[client] helpbar.text=0` and `helpbar.label=0` (also `helpbar.hide`, `helpbar.background`, `helpbar.title`, `network.background`, `network.panel`, which act on the window draw instead and need no pattern).
**What it does:** When text or label is turned off at the first frame, rewrites the 32-bit offset of two `call` instructions inside the help line's draw so they call VanaCore's functions instead: the help line's text line draw and the "Help" caption sprite draw. The replacements call the game's originals while `helpbar.text` / `helpbar.label` is on, so the patch stays in whatever the setting later becomes (pass-through, not restored). Installed once.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDraw` (`helpbar.cpp:35`) | the help line's draw (owner's vtable slot 4; `sub esp,8; push ebx; push esi; mov esi,ecx; ... mov [eax+10h],ebx; call`) | `83 EC 08 53 56 8B F1 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? BB 80 80 80 80 8D 4E 20 89 58 64 89 58 48 89 58 2C 89 58 10 E8` | call at `site+54` (`kTextCall`, the text line draw; byte must be `E8` and target inside the module) rel32 rewritten to `text_hook`; call at `site+128` (`kLabelCall`, the caption sprite draw, past the pattern's end; `E8`, target inside the module and the 4 bytes before it `8B 4E 1C 52` = `mov ecx,[esi+1Ch]; push edx`) rewritten to `label_hook` |

**CatsEyeXI address:** not recorded (the source calls it `FUN_10139E90 in the PS2 client's order of things`, so that is not this build's address).
**Retail:** `??` over the renderer global and the two call distances; the 4-byte check before `site+128` guards the fixed offset. Not found: `Help bar: the game's help line drawing wasn't found ...: helpbar.text and helpbar.label do nothing`; each call logs separately if it isn't `E8`/in-module/matching.
Fragile: `kTextCall = 54` (end of the pattern, so it is the pattern's own last byte) and `kLabelCall = 128` (74 bytes past the pattern's end, guarded only by the 4-byte prefix); window layout at `window + 4`, name at `layout + 0x46` (`menu    helpwind`, `menu    titlewin`, `menu    netstat `), owner at `window + 0xC`, owner vtable slot 4 called from `draw_owner`.

#### Changes to game code not from a pattern

- `chatfont.cpp`: writes True Font's widths over the game's width-table copy at `kWidthTable`'s operand (data write, restored by `hold(false)`); writes the text renderer's four corner colours (`renderer + 0x10/0x2C/0x48/0x64`) and restores `0x80808080`; temporarily the macro key shapes' count (`macrotext.cpp`, `shape + 0x14`).
- `chatpages.cpp`: `imports::jump` over eight pager functions found by pattern; two `call` rel32s rewritten (`close+0x3A`, `page_write+0x5F`) to `wait_name`; two imm32 format-string pointers rewritten; one byte `74`<->`EB` at `kTabScroll+5`.
- `phraselist.cpp`: `imports::hook_vtable` on the chat line context's slots 1 and 2 (vtable shared by every instance) and on the chat manager's slot 4, both obtained from objects found through `chat::input_object()` and the `kMade` globals, not by pattern.
- `helpbar.cpp` and `namechars.cpp`: patches are described in their sections.
- Hard-coded structure offsets shared by these files: window layout name at `layout + 0x46` (chatfont, macrotext, helpbar), a window's layout at `+4` and owner at `+0xC` (helpbar), shape list/count `+4/+0x14` (chattabs, macrotext, jplabels).

### Items

#### More rows in the item and chat lists (`src/core/itemlist.cpp`)

**Setting:** `[client] items.rows`, `auction.rows`, `shop.rows`, `equip.rows`, `lists.rows`, `currencies.rows` (each defaults to the game's own count). `install_hook()` runs once, the first frame any of these is above the game's count, or `items.autosort` / `items.sort=az` / `shop.total` is on (those share the same hook function, see the next two sections).
**What it does:** The Items, Mog House storage, auction, shop and Equipment lists have 10 rows and the lists over the chat (Magic, Abilities, ...) 12. VanaCore detours the list's init, `SetLine` and the item details window's `RePosition`, makes extra button elements by calling the game's own button-making functions (found by following `call`s out of `kMakeButton`), makes the window taller with the game's resize, and gives the list the new rows a page. Hooks stay installed once made; with the settings back at the game's counts they pass straight through. One byte of code is patched for the storage list (below) and rewritten whenever the setting changes (20 = the game's, so it is undone by setting the rows back).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kListInit` (`itemlist.cpp:34`) | `YkWndList::OnInitialUpdatePrimitive` (FUN_101f4fd0), run once by every list window | `56 8B F1 66 8B 46 18 66 8B 4E 20 50 51 8B 4E 08 E8 ?? ?? ?? ?? 8B 56 08 32 C0 88 82 86 00 00 00 8B 4E 08 88 81 85 00 00 00` | detoured, 7 bytes prologue (`list_init_hook`) |
| `kSetLine` (`itemlist.cpp:41`) | `YkWndList::SetLine` (FUN_101f4e20, rows a page) | `66 8B 54 24 04 66 8B 41 20 66 2B C2 66 89 51 18 66 89 41 22 79 06 66 C7 41 22 00 00 C2 04 00` | detoured, 5 bytes; its trampoline also called by VanaCore (`g_set_line`) |
| `kClipCursor` (`itemlist.cpp:45`) | `YkWndList::ClipCursor` (FUN_101f4dd0) | `66 8B 41 1E 66 8B 51 18 56 57 66 8B 79 20 8D 34 02 66 3B F7 66 89 71 1C 7E` | not hooked; called directly (`g_clip`) |
| `kMakeButton` (`itemlist.cpp:51`) | `CTkMenuCtrlData::SetResourceData`'s making of a button (FUN_10117b40 + 0xD2) | `6A 60 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 84 ?? ?? ?? ?? 56 8B C8 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 66 FF 46 58 53 8B CF E8 ?? ?? ?? ?? 0F BF 4E 54 8A 86 99 00 00 00 0F BF 56 52 50 51 52 8B CF E8 ?? ?? ?? ?? 8A 86 99 00 00 00 8B CF 50 E8 ?? ?? ?? ?? 57 8D 4E 14 E8` | not hooked; the targets of the calls at `site+2` (new), `+0x15` (ctor), `+0x2B` (SetResourceData), `+0x43` (LocateCalc), `+0x51` (ShapeCalc), `+0x5A` (AddTail) are called directly; each must be `E8` |
| `kResize` (`itemlist.cpp:60`) | the game's window resize (FUN_1011a1a0, ret 0x1C) | `83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84` | called directly (`g_resize`) |
| `kReposition` (`itemlist.cpp:65`) | `CTkMenuCtrlData::RePosition` (FUN_10118d10), places the item details window | `83 EC 0C 66 8B 44 24 14 53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | detoured, 8 bytes (`reposition_hook`) |
| `kOpen` (`itemlist.cpp:71`) | `YkWndItem::Open` (FUN_101f0b00) | `8B 44 24 04 56 85 C0 8B F1 75 05 A1 ?? ?? ?? ?? 8B C8 89 46 5C 8B 11 FF 52 54 89 46 64 48 83 F8 10` | `operand(site, 0xC)` = `g_pItemCtrlList`; also used as the identity every "Open" call in the other rows is checked against (`call_target(...) == open`) |
| `kItemInfo` (`itemlist.cpp:76`) | the item details window global (`g_pTkItemInfo`), inside Open | `A1 ?? ?? ?? ?? 85 C0 74 3E 6A 00 E8 ?? ?? ?? ?? 83 C4 04 84 C0 75 30 8B 4E 5C 6A 00 6A 00 6A 01 8B 11 FF 12 8B 0D` | `operand(site, 1)` = the global |
| `kViewHouse` (`itemlist.cpp:83`) | the Mog House's opening of a storage (FUN_101d9570) | `8B 0D ?? ?? ?? ?? 6A 00 8B 11 FF 52 58 A1 ?? ?? ?? ?? 6A 00 C6 40 70 01 88 58 71 8B 44 24 14 8B 0D ?? ?? ?? ?? 50 E8` | `operand(site, 2)` = `g_pItemCtrlMove`; `operand(site, 0xE)` = `g_pYkWndBank` (also used by the sort section) |
| `kItemsUse` (`itemlist.cpp:93`) | the action menu's Items (FUN_1012b3c0) | `C2 04 00 8B 0D ?? ?? ?? ?? 6A 01 6A 00 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 83 C4 40 C2 04 00` | `operand(site, 19)` = `g_pItemCtrlUse`; the call at `site+30` must be `call Open` |
| `kAuctionSell` (`itemlist.cpp:98`) | the auction house's Sell opening the inventory list (at `0x101D8DC8` per the comment, i.e. `+0x1D8DC8`) | `83 C4 08 50 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? C2 08 00 6A 00 6A 01 68` | `operand(site, 10)` = the Sell controller; call at `site+0x15` must be `Open`. Optional: not found logs a line and that list keeps 10 rows |
| `kEquipList` (`itemlist.cpp:104`) | the Equipment menu's list of what fits a slot (at `0x10132DCA`, i.e. `+0x132DCA`) | `8B 0D ?? ?? ?? ?? 6A 00 8B 11 FF 52 58 A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8` | `operand(site, 14)` = `g_pTkEquipCtrl`; call at `site+25` must be `Open`. Optional, as above. Short and unanchored; the `Open` check is what ties it down |
| `kWindows` (`itemlist.cpp:128`) | the menu manager's list of windows (same one `statuswindow.cpp` finds) | `32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8` | `operand(site, 3)` = the list head; walked (node +0 next, +0x10 window) |
| `kBankColours` (`itemlist.cpp:153`) | `YkWndBank::MakeList` (FUN_101da9d0) row colour window | `0F BF 44 24 14 33 ED BB 40 40 40 80 8D 48 F6 3B 4C 24 10 7F 0B 8B 4C 24 10 83 C0 14 3B C8 7E 0D 66 3B 54 24 14 7C 2D 66 83 FA 0A 7D 27` | the imm8 at `site+0x1B` (`0x14`, the game's margin) is patched to rows+10 (at most 127); only done if it reads `0x14` now. Optional: not found logs a line, rows past the 20th draw grey |
| `kChatListSetup` (`itemlist.cpp:166`) | the "Ka" menus' list setup (FUN_1021ffe0, ret 0x14; Magic, Abilities, Currencies...) | `53 55 56 8B F1 33 DB 57 8B 46 20 3B C3 74 0F 83 C0 FC 50 E8 ?? ?? ?? ?? 83 C4 04 89 5E 20 8B 7C 24 1C 33 C0 3B FB 89 5E 44` | detoured, 5 bytes (`chat_setup_hook`) |
| `kChatListMade` (`itemlist.cpp:172`) | the same lists' window made (FUN_102205b0) | `56 8B F1 8B 4E 08 85 C9 74 13 6A 00 6A 00 E8 ?? ?? ?? ?? 8B 46 08 C6 80 85 00 00 00 00 8B 16 8B CE 5E FF 62 54` | detoured, 6 bytes (`chat_made_hook`) |

**CatsEyeXI address:** only what comments or `FUN_` names give: `kListInit` +0x1F4FD0, `kSetLine` +0x1F4E20, `kClipCursor` +0x1F4DD0, `kMakeButton` inside +0x117B40 (site at +0xD2), `kResize` +0x11A1A0, `kReposition` +0x118D10, `kOpen` +0x1F0B00, `kViewHouse` in +0x1D9570, `kItemsUse` in +0x12B3C0, `kAuctionSell` near +0x1D8DC8, `kEquipList` near +0x132DCA, `kBankColours` in +0x1DA9D0, `kChatListSetup` +0x21FFE0, `kChatListMade` +0x2205B0. Other rows: not recorded. The Currencies list's vtable `0x1033A2D0` and setup `0x10228A20` (comment, line 185) are described but not used.
**Retail:** per `docs/retail-signatures.md` all of these are in both builds (none of the four that were CatsEyeXI-only). Few `??`: operands that are globals are read back with `operand()`, not hard-coded. The log line is `Item list: the game's list windows weren't found in FFXiMain.dll (...)` and `items.rows` does nothing. The function returns at that point, so the sort and shop-total steps that follow it in `install_hook()` are skipped too when this core set is missing (or `Open` isn't the target of the `kItemsUse` call), and again if the three detours (`kListInit`, `kSetLine`, `kReposition`) fail. Fragile: the offsets from `kMakeButton` to its six calls (`0x15, 0x2B, 0x43, 0x51, 0x5A`; each only checked to be an `E8`); `kBankColoursAt = 0x1B` (checked to hold 0x14); `kItemCtrlListAt` 0xC, `kItemCtrlMoveAt` 2, `kBankWindowAt` 0xE, `kItemCtrlUseAt` 19, `kItemCtrlSellAt` 10, `kItemCtrlEquipAt` 14, `kItemWindowAt` 11, all offsets into the matches that land on `??` operands (safe only while the layout of the match is unchanged); `kSetLinePrologue 5`, `kListInitPrologue 7`, `kRepositionPrologue 8`, `kChatListSetupPrologue 5`, `kChatListMadePrologue 6` bytes must fall on instruction boundaries (they do in the quoted code). Hard-coded structure offsets, none shifted by the retail `+0x8/+0x70` big-object shifts as far as the doc says (UI objects, not the big object): list `+8` window, `+0x18` rows, `+0x20`, `+0x22`, `+0x26` pitch, `+0x5C` controller, `+0x60`/`+0x5C` item store, `+0x64` container; window `+4` menu data (name at data `+0x46`), `+0xC` primitive, `+0x14` elements, `+0x34` flags, `+0x3A` rect, `+0x52/+0x54` position, `+0x58` buttons, `+0x99` zoom, `+0xBE` area top, `+0x88` scroll bar; button `+0x32..+0x3A`, `+0xC` record, `+0x60` size; chat list `+0x38/+0x3C/+0x40/+0x48`, scroll bar `+0xC/+0x10/+0x14`; item details `+0x34E` text box. None was checked against retail by the audit beyond the functions listed as unchanged.

#### Item sort (`src/core/itemlist.cpp`)

**Setting:** `[client] items.autosort` (default on), `items.sort` (`game` or `az`). Installed by the same `install_hook()` as above, which runs for these too.
**What it does:** `items.autosort` calls the game's `KmItem::Sort` on a bag's store as its list opens (called, not hooked). `items.sort=az` rewrites the 4-byte comparison function pointer pushed to `qsort` inside the sort to VanaCore's name comparison; going back to `game` writes the game's pointer back, so it is undone on reload.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSort` (`itemlist.cpp:137`) | `KmItem::Sort` (FUN_1021aea0, ret 0x10) | `A1 ?? ?? ?? ?? 83 EC 08 85 C0 53 55 56 57 8B D1 0F 84 ?? ?? ?? ?? 8B 44 24 1C 8B 5C 24 20 89 42 10 8A 44 24 28 84 C0 89 5A 0C` | called as `g_sort(store, filter, key=1, arg=0x12, 0)` |
| `kSortCompare` (`itemlist.cpp:143`), searched only in the 0x300 bytes after `kSort` (`scan::find(sort, 0x300, ...)`) | the `push <compare>` before `qsort` in the sort | `25 FF 00 00 00 68 ?? ?? ?? ?? 48 6A 08 50` | `operand(site, 6)` = the game's comparison, read then overwritten with `az_compare` / restored |
| `kItemWindow` (`itemlist.cpp:88`) | the Mog House routine's opening of the inventory list (`g_pYkWndItem`) | `E8 ?? ?? ?? ?? 84 C0 75 1E 8B 0D ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 6A 06 E8` | `operand(site, 11)` = `g_pYkWndItem`; call at `site+0x10` must be `Open`. Also needs `kViewHouse`'s `g_pYkWndBank` |

**CatsEyeXI address:** `kSort` = `+0x21AEA0` (FUN_1021aea0); others not recorded.
**Retail:** both patterns are in the audited set. Sort and `kItemWindow` missing logs `the game's sorting of a bag wasn't found ... items.autosort does nothing`; missing compare logs a separate line and only `items.sort` is lost. Fragile: the sort's `qsort` push must sit within 0x300 bytes of its start; list-store offsets (`+0x60`, `+0x5C`), the table entries (8 bytes: `+0` kind, `+4` id, `+6` slot) and sort key `1` are hard-coded. A retail server sorts on its own (comment, line 130), so this matters mainly for CatsEyeXI.

#### Shop purchase total in chat (`src/core/itemlist.cpp`)

**Setting:** `[client] shop.total` (default on).
**What it does:** detours the shop's quantity-picked callback (FUN_1020e4d0) and, before it opens the Yes/No, prints `Purchase: N Name for X Gil?` to the chat using the shop's list, the Buy controller's selected row, and the item's name in its data. Passes through when off. Hook is not removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kShopBuy` (`itemlist.cpp:111`) | the shop OnOK's quantity callback (FUN_1020e4d0, `__cdecl`) | `8B 44 24 04 85 C0 7E 1C 8B 0D ?? ?? ?? ?? 6A 00 6A 01 6A 00 6A 00 6A 00 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? C3 A1 ?? ?? ?? ?? C6 40 10 00 C3` | detoured, 6 bytes; `operand(site, 0x25)` = `g_pShopBuyCtrl` |
| `kShopItem` (`itemlist.cpp:121`) | the same OnOK's lookup of the item (FUN_1020e3a0 + 0x3A) | `8D 54 24 10 53 52 E8 ?? ?? ?? ?? 83 C4 08 84 C0 74 ?? 8B 6C 24 10 8B 44 24 24 39 45 00 77 ?? 83 C5 0C 85 ED 74 ?? 33 C9 66 8B 4D 00 51 8B 0D ?? ?? ?? ?? E8` | call at `site+6` = `gcShopItemList` (called); `operand(site, 0x2F)` = `g_pTkItemData`; call at `site+0x33` = `CTkItemData::GetItemDataMenber` (called) |

**CatsEyeXI address:** `kShopBuy` = `+0x20E4D0`, `kShopItem` = `+0x20E3A0` + 0x3A (from `FUN_` names).
**Retail:** both patterns are in the audited set. Missing logs `a shop's buying wasn't found ... shop.total does nothing`. **`sigs::item_data_shift()` is applied here, `itemlist.cpp:881`:** the item's name pointer is read at `kItemNameAt (0xCC8) + sigs::item_data_shift()`. Fragile: shop list entry layout (`+0` price, `+0xC` item id, entries 0xC bytes), Buy controller `+0x10` and selected row at `+0x11`, item data name at `0xCC8`.

#### Item rarity: icon boxes, names, equipped colour (`src/core/itemrarity.cpp`)

**Setting:** `[client] items.rarity` (`game`/`hide`/`style`), `items.rarity.names`, `items.rarity.color1..4`, `items.equipped` (`game`/`off`/`color`), `items.equipped.color`, `items.slot*` (a styled box follows these), `tips.rarity.name`, `tips.rarity.gradient` (+ `.strength`, `.height`). The icon hook goes in when a box style, names, `items.equip` (via `itemslot::wants_icons`) or listtext's equip option wants it; the text hook when names, tooltip rarity, `items.equipped` or listtext wants it. Each is installed once and left in place.
**What it does:** detours the game's item icon drawing and rewrites the 32x32 DXT3 icon texture (via the D3D8 texture's LockRect/UnlockRect, game memory) so the rarity box is hidden or restyled; detours the coloured-text drawing to recolour item names (by return address: the details window's name, or the lists' cell function range) and equipped items' green. Textures VanaCore wrote are put back when `items.rarity` goes to `game` (the box redrawn from the bitmap).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDrawIcon` (`itemrarity.cpp:36`) | `CTkItemDataMenber::DrawIcon` (FUN_1015a2d0, ret 0x18) | `83 EC 08 53 56 57 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8B 8E ?? ?? ?? ??` | detoured, 5 bytes (`draw_icon_hook`); the icon field operand is `??` on purpose (CatsEyeXI `0xD0C`, retail `0x150C`) |
| `kDrawText` (`itemrarity.cpp:41`) | the menu-font coloured string draw (FUN_10129a40, `__cdecl`) | `8B 44 24 14 8B 4C 24 10 8B 54 24 0C 68 00 00 80 3F 68 00 00 80 3F 6A 44 50` | detoured, 8 bytes (`draw_text_hook`, which reads its `_ReturnAddress()`) |
| `kInfoName` (`itemrarity.cpp:45`) | the details window's name print inside `CTkItemInfo::OnDrawPrimitive` (FUN_1013ca00) | `6A 00 52 50 51 E8 ?? ?? ?? ?? 66 8B 57 0A 66 8B 47 08 66 03 96 50 03 00 00` | `site+10` (the call's return address) is remembered as "the details' name" |
| `kCells` (`itemrarity.cpp:51`) | a list's cell draw (FUN_101f3310, ret 0x18) | `81 EC 54 01 00 00 53 55 56 57 8B BC 24 6C 01 00 00 8B F1 8B 5C BE 28 85 DB 0F 84 ?? ?? ?? ?? 0F BE 04 37 48 83 F8 ?? 0F 87 ?? ?? ?? ?? FF 24 85` | the 4 bytes at `site+0x30` (its jump table's address) are read as the function's end; `[site, table)` is the "list cell" return range (rejected if empty or > 0x4000) |

**CatsEyeXI address:** `kDrawIcon` = `+0x15A2D0`, `kDrawText` = `+0x129A40`, `kInfoName` inside `+0x13CA00` (call near `+0x13CB63`), `kCells` = `+0x1F3310`. Equipped-name print sites `0x101EEEA0`, `0x1013CB63`, details window set-item `0x1013C70C`, its draw `0x1013CA6D` (comments).
**Retail:** `kDrawIcon` was the one pattern here that was CatsEyeXI-only (the `0xD0C` field); it now has `??`. **`sigs::item_data_shift()` is applied here:** `g_shift` is set from it in `install_icon_hook` (`itemrarity.cpp:573`) and `install_text_hook` (`:587`) and added to every item-data read: id `0xCB8` (`:243`, `:278`), name `0xCC8` (`:261`), bitmap `0xCE8` (`:197`, `:279`), bitmap size `0xCEC` (`:198`, `:280`), icon `0xD0C` (`:244`, `:467`). Not found logs `the game's item icon drawing wasn't found or couldn't be hooked` (resp. `coloured text drawing`); missing `kInfoName` / `kCells` just drops the details' name / the equipped colour in lists. Fragile: the details object `+0xC` window, `+0x1C` item data pointer (also read in `itemtips.cpp`); `kCells`'s jump table address at `+0x30` and the assumption the table follows the function; `kInfoName` encodes `0x350` (CTkItemInfo field); icon object `+8` texture, texture `+0x40`/`+0x44` D3D texture; the 0x839-byte bitmap size and mark byte `0x91`; the `0x509050` equipped colour and `0x808080` plain. If retail shifts the item data differently the check `abs(field - 0xD0C) <= kMostShift` (in `sigs.cpp`, not here) gives up and assumes 0.

#### Bag count on the title bar (`src/core/itemcount.cpp`)

**Setting:** `[client] items.count` (on/off) and `items.count.*` (size, x, y, color, separator, hidetotal, autototal, font.*). The hook is installed at the first frame regardless of the setting.
**What it does:** detours `YkDrawItemStack(count, size)` (FUN_101c9cf0). When `items.count` is on, VanaCore draws the count and total itself (True Font) and the game's is skipped; when off, the game's function is called with True Font's game-letters flag set. Also resolves the menu font's routines (found from the matches below) for the old `menu` face.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kItemStack` (`itemcount.cpp:29`) | `YkDrawItemStack` (FUN_101c9cf0, `__cdecl`) | `A1 ?? ?? ?? ?? 83 EC 2C 53 33 DB 56 3B C3 57 0F 84 ?? ?? ?? ?? 39 58 08 0F 84 ?? ?? ?? ?? 8D 4C 24 0C 66 89 5C 24 0C 66 89 5C 24 0E 66 89 5C 24 10 66 89 5C 24 12 51 8B 48 08 E8` | detoured, 5 bytes; `operand(site, 1)` = `g_pTkTitle`; call at `site+0x3A` = `GetWindowLocate` (called); call at `site+0xCF` must be `E8` = `YkDrawStringReverse` (called); `E8` at `reverse+0x16` = the font getter |
| `kPrintString` (`itemcount.cpp:46`) | the menu font's whole-string routine (FUN_1011ba10, `__thiscall`, ret 8) | `83 EC 08 8B 44 24 10 53 55 8B 6C 24 14 8B 50 04 56 8B F1 8B 08 89 4C 24 0C 57 8B CE 89 54 24 14 89 6C 24 20 E8 ?? ?? ?? ?? 8B CE E8` | not hooked; calls at `site+0x24` (states), `+0x2B` (bind), `+0x50` (next code), `+0x93` (print code), `+0x9E` (advance) are called directly; last three only checked to be `E8` |
| `kTitleText` (`itemcount.cpp:56`) | the title bar's draw (`CTkTitle::OnDrawPrimitive`, FUN_1014f890), which sets up the text renderer | `8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8D 8E A4 00 00 00 C7 40 64 80 80 80 80` | `operand(site, 2)` = the renderer global; its quad corner colours at `+0x10, +0x2C, +0x48, +0x64` set temporarily while drawing |

**CatsEyeXI address:** `kItemStack` = `+0x1C9CF0`, `kPrintString` = `+0x11BA10`, `kTitleText` inside `+0x14F890`. Others not recorded.
**Retail:** all three in the audited set. Not found: `Item count: the game's bag count drawing wasn't found ... items.count does nothing`; the menu font missing only loses the menu face. Fragile: the six far call offsets above (`0xCF`, `0x93`, `0x9E`, `0x50`, ...) lie past the end of the patterns so only "is an E8" is verified; title `+8` window; renderer corner offsets. No item-data offsets, so no `item_data_shift`.

#### Item slot squares and Equipment wardrobe numbers (`src/core/itemslot.cpp`)

**Setting:** `[client] items.slot` (`game`/`tint`/`solid`/`hide`) with `.color/.brightness/.contrast/.opacity`, `items.equip` (+ `.background`, `.border`, `.border.size`), `items.equip.wardrobe` (default on); the quad hook is also wanted by `buttons.*`, `sliders.*`, `<id>.solid`, `gamewin`, `jplabels`, `labeltext`, `listtext`, `profilewin` (they all draw through it).
**What it does:** detours the game's menu-picture quad draw (every window's frames, slots and buttons) and, for the "menu    itemslot" texture, draws VanaCore's own square or a recoloured copy of the DXT1 texture. Separately, with `items.equip.wardrobe=0` only, rewrites two `call` instructions in the Equipment window's draw to VanaCore stubs that skip the wardrobe number box and digit. The call rewrite is not restored; the stubs just call through when `items.equip.wardrobe` is on (it is only installed when it is off at the time).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDrawQuad` (`itemslot.cpp:36`) | the menu quad draw (FUN_1011e580, `__thiscall`, ret 0x18) | `83 EC 44 56 57 8B F9 8B 47 4C 85 C0 75 ?? 8D 77 32 56 E8` | detoured, 5 bytes (`draw_hook`) |
| `kBadgeDraw` (`itemslot.cpp:115`) | the Equipment window's draw (FUN_10132aa0) around its wardrobe number | `53 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 ?? 0F BF 4C 24 18 0F BF 54 24 1A 6A 00 6A 00 68 80 80 80 80 83 C1 10 52 51 8B C8 E8` | `call` at `site+0x27` (the box, FUN_1011f760) and at `site+0x3D` (the digit, FUN_101329a0) redirected with `asm_text::branch` to `badge_hook` / `digit_hook`; the call at `+0x3D` lies past the pattern, only checked to be `E8`. Related `FUN_101328f0` picks the picture |

**CatsEyeXI address:** `kDrawQuad` = `+0x11E580`; `kBadgeDraw` inside `+0x132AA0`, box call target `+0x11F760`, digit `+0x1329A0`. Other addresses in comments (Check window vtable `0x103317E0`, its draw `0x10133A30`, window draw FUN_101173d0, owner draw `0x10117904`) are described, not used.
**Retail:** both patterns are in the audited set. Not found: `Item slot: the game's menu picture drawing wasn't found ... items.slot does nothing` (or the wardrobe line for the badge). Fragile: quad field offsets `+0x32` name, `+0x4C` texture, `+0x50` corners, `0x1C` stride, `+0x10` colour, `+0x13` alpha, `+8` size; texture `+0x40/+0x44`; window `+4` layout name at `+0x46` (window names compared as 16-char strings); `kBadgeCall 0x27` and `kDigitCall 0x3D`. No item-data offsets, so no `item_data_shift`.

#### Item details on one page (`src/core/itemtips.cpp`)

**Setting:** `[client] tips.extended` (default on), `tips.empty.hide` (default on; only reads game objects, no pattern).
**What it does:** detours the item details window's page builder and page turn so the three pages of 12 rows merge into one table (written into the object's row table at `+0x383`), and patches three `12` immediates (the draw's and the count's loop limits) to `36`, while `tips.extended` is on; written back to `12` when it is turned off. Hooks stay installed and pass through when off. The game's resize (`kResize`) is used to make the box taller (also by chatfont.cpp's `SetLines` hook through `set_lines`) and to put it back.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kRebuild` (`itemtips.cpp:31`) | the details' page builder (FUN_1013C610) | `56 8B F1 8B 46 1C 85 C0 74 4F 8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0A 83 F8 02 74 09 83 F8 03 74 04 85 C0 74 09 8B CE E8` | detoured, 6 bytes (`rebuild_hook`) |
| `kFlip` (`itemtips.cpp:37`) | the page turn (FUN_1013BD60) | `56 8B F1 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 8A 96 80 03 00 00 FE C2` | detoured, 9 bytes (`flip_hook`) |
| `kDraw` (`itemtips.cpp:42`) | the draw of the page's rows (FUN_1013D170) | `51 56 8B F1 33 C0 8A 86 80 03 00 00 8D 04 40 80 BC 86 83 03 00 00 FF 75` | byte at `site+0x117` (the loop's `cmp edi,0Ch` immediate) patched `0C` -> `24`; accepted only if it currently reads `0x0C` |
| `kCount` (`itemtips.cpp:46`) | the count of the page's rows (FUN_1013C670) | `33 C0 33 D2 8A 91 80 03 00 00 8D 14 52 8D 8C 91 83 03 00 00 80 39 FF 74 0C 40 41 83 F8 0C 7C F4 B8 0C 00 00 00 C3` | bytes at `site+0x1D` (`cmp eax,0Ch`) and `site+0x21` (`mov eax,0Ch`) patched `0C` -> `24`; all three limits must read `0x0C` or none are touched |
| `kResize` (`itemtips.cpp:50`) | the window resize (FUN_1011A1A0), same as `itemlist.cpp:60` | `83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84` | called directly (`g_resize`); optional: without it or the limits "a page past 12 rows stays the game's" |

**CatsEyeXI address:** `kRebuild` = `+0x13C610` (set-item `+0x13C6E0`, `SetLines` `+0x13BCC0`), `kFlip` = `+0x13BD60`, `kDraw` = `+0x13D170`, `kCount` = `+0x13C670`, `kResize` = `+0x11A1A0`. Also named in comments: `0x1011A4F0` (layout compare), `0x101F10D0` (side-by-side turn), `0x1062E7E4` (comment box global), `0x101DC410` (its draw), `0x1013347A` (View Wares); not used by a pattern.
**Retail:** all five are in the audited set. `kRebuild` or `kFlip` missing logs `their page building wasn't found ... tips.extended does nothing`. Fragile: the `+0x117`, `+0x1D`, `+0x21` patch offsets (guarded by reading back `0x0C`); the details object's `+0x1C` item pointer, `+0x36C` laid-out rows (written), `+0x380` page, `+0x383` table (3x12), window `+8`, window rect `+0x3A`, plus `+0x10` window state, `+0x52/+0x54` place (`SetLocate` is hooked elsewhere, `hudwin.cpp`). The details object is a UI object, not the item data, so `item_data_shift` is not used. Minor comment slip: the `kCount` comment says `+0x1B` / `+0x20` but the constants (`0x1D`, `0x21`) are the immediates.

#### Game code changes not from a pattern, and where `item_data_shift` is used

- `sigs::item_data_shift()` is applied at `itemlist.cpp:881` (shop item name) and in `itemrarity.cpp` (`g_shift`, set at `:573` and `:587`, added at `:197, :198, :243, :244, :261, :278, :279, :280, :467`). `itemcount.cpp`, `itemslot.cpp` and `itemtips.cpp` do not use it.
- Itemlist: operand-read globals (`g_pItemCtrlList`, `g_pItemCtrlMove`, `g_pItemCtrlUse`, `g_pItemCtrlSell`, `g_pTkEquipCtrl`, `g_pTkItemInfo`, `g_pYkWndBank`, `g_pYkWndItem`, `g_pShopBuyCtrl`, `g_pTkItemData`) are dereferenced each use. Window names matched as 16-char strings (`menu    inventor`, `menu    bank    `, `menu    auclist `, `menu    shop    `, `menu    money   `, `menu    itemctrl`, `menu    playermo`, `menu    evitem  `).
- Itemrarity: writes the game's Direct3D icon textures through the D3D8 vtable (`d3d8::slot::TextureLockRect`/`UnlockRect`/`GetLevelDesc`), reads icon bitmaps from item data.
- Itemslot: reads and temporarily rewrites the quad's corner colours and alpha around each `g_draw`; recognises the `menu    itemslot` texture and the `menu    equip   ` / `menu    inspect ` / `menu    mcresed ` windows by name.
- Itemtips: moves other windows (by `+0x52/+0x54`, `lowered_by`, `lower()` each frame) when the details box is taller; `empty()` only reads the details/comment objects (`hudwin`/`chat.cpp` act on it); writes `+0x36C` on the details object and resizes via the game's resize.

### Equipment and HUD windows

#### Equip sets: Equip first (`src/core/equipfirst.cpp`)

**Setting:** `[client] macrooptions.equipfirst` (taken up by `on_frame`, polled every 500 ms; re-read on reload).
**What it does:** Rewrites the game's own in-memory payload of the equip sets' options box (menu layout `"menu    mcresmn "`) so Equip is the first button: it changes the buttons' Y shorts and up/down link bytes, leaving ids and block order alone. It also detours the box's "opened" handler so the cursor is put on Equip (by calling the game's SetCursol on button 7) before the first draw. The layout is rewritten only while no box is open, and put back to the game's order (`{1..8}`) when the setting is turned off (as long as it was laid). The detour stays installed once made.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHelp` (`equipfirst.cpp:28`) | the window that has the help line's text (FUN_101182a0); used only to reach the menu manager global | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `operand(help, 7)` (`kHelpManagerAt`) = the menu manager's address; rejected if outside FFXiMain.dll's image |
| `kSetCursor` (`equipfirst.cpp:31`) | SetCursol (0x101189B0, thiscall: window, item, flag; ret 8) | `83 EC 08 56 57 8B 7C 24 14 8B F1 66 85 FF 7F 05 BF 01 00 00 00 66 8B 46 58 66 3B F8 7E 02 8B F8 66 8B 46 4C` | called as a function at the match (not hooked) |
| `kOpened` (`equipfirst.cpp:40`) | the box's opened handler (0x101A5A40, thiscall on its handler; no args) | `A1 ?? ?? ?? ?? 83 EC 10 53 33 DB 57 3B C3 8B F9 0F 84 ?? ?? ?? ?? 8B 4F 08 55 8D 44 24 0C 56 50` | `imports::detour(opened, 5, ...)` (first instruction `mov eax,[abs]`, 5 bytes); our hook calls the game's then `SetCursol(window, 0, 7, 0)`; handler `+8` = the box's window |

**CatsEyeXI address:** `FFXiMain.dll+0x1182A0` (kHelp), `+0x1189B0` (kSetCursor), `+0x1A5A40` (kOpened). Comment says FFXiMain 2025-12-26, read live 2026-11-09.
**Retail:** The manager is read back with `operand()`, not hard-coded. `kOpened` leaves its global `??`. If `kSetCursor` or the manager isn't found it logs "Equip first: ... isn't as VanaCore knows it" and the feature stays off; if only `kOpened` is missing it logs and puts the cursor on Equip one frame after the box opens (`put_cursor`). Written on 2026-11-09, after the retail audit (2026-10-31), so these three may never have been audited against retail. `kHelp` is the same pattern the HUD windows use (below), so it is not new exposure. Fragile: hard-coded layout and window structure offsets (see below) with a strict `read_box` check that leaves the box alone (`g_failed`, logged once) if the layout doesn't match exactly.

**Hard-coded game structure offsets (no pattern):**
- Menu manager: windows list `+0`, layouts list `+0x1C`. List node: next `+0`, item `+0x10`, "skip" byte `+0x14`.
- Layout: parse pointer `+8`, name `+0x46`. Window: layout `+4`, state `+0x10` (`0xE` = closing). Handler: window `+8`.
- Layout payload: header `0x20`, block count at `+0x11`; block fields X `+2`, Y `+4`, width `+0xA`, height `+0xC`, item id `+0x12`, links `+0x15` (6 bytes). DAT chunk header read at `payload - 0xC` (type `0x30`, length `((info>>7)&0x7FFFF)*16`).
- Button geometry assumed: 8 buttons 88 x 16 at x 16, y 6 to 118.

#### Equipment window stats (`src/core/equipstats.cpp`)

**Setting:** `[client] equip.stats` (default off), with `equip.stats.checkparam`, `equip.stats.move`, `equip.stats.x`, `equip.stats.y`. The hooks are installed once, on the first frame after the setting is seen on.
**What it does:** Detours the Equipment window's draw (`CTkEquip::OnDrawPrimitive`) and draws the gear's stats under (or beside) it using the game's own text, quad and window-frame functions. While on, it also resizes the Equipment window and the windows under it (`g_resize`), moves the elements' number rows and picture quads up, and blanks the Attack/Defense digits during the game's draw. With the setting off the hook is still in place but passes through, putting the window's size back (`fit(ctrl, 0)`); the detour is never removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kEquipDraw` (`equipstats.cpp:51`) | the Equipment window's draw, `CTkEquip::OnDrawPrimitive` (FUN_10132aa0, thiscall on its handler; global handler at 0x10576038) | `83 EC 10 55 56 8B F1 57 33 C0 BD 01 00 00 00 8B 7E 08 66 89 44 24 14 66 89 44 24 16 66 89 44 24 18 66 89 44 24 1A 66 39 6F 58` | `imports::detour(at, 5, draw_hook)`; handler `+8` = control |
| `kDrawString` (`equipstats.cpp:58`) | YkDrawString (FUN_101c7ee0, cdecl: x, y, text, colour) | `8B 44 24 10 8B 4C 24 0C 8B 54 24 08 6A 00 68 00 00 80 3F 68 00 00 80 3F 50 8B 44 24 14 51 52 50 E8 ?? ?? ?? ?? 83 C4 1C C3` | called as a function (measures and draws text) |
| `kFonts` (`equipstats.cpp:62`) | the fonts table getter (FUN_101c7390, cdecl: index; 18 fonts) | `8B 44 24 04 85 C0 7C 0D 83 F8 12 7D 08 8B 04 85 ?? ?? ?? ?? C3 33 C0 C3` | called as a function; font 0's `'-'` glyph quad is copied to draw lines |
| `kShapeDraw` (`equipstats.cpp:66`) | YmMenuShape::Draw (FUN_1011f760); the pattern starts 7 bytes in (first 7 bytes left out because `buttons.cpp` hooks them) | `?? ?? ?? ?? ?? ?? ?? 8D 59 04 57 8B CB E8 ?? ?? ?? ?? 89 44 24 10 8B 44 24 28 33 FF 85 C0 7E 13` | `site + 0x6D` (`kQuadCall`) must be `E8`; its target is the quad drawing (FUN_1011e580, thiscall, ret 18h), called as a function. Missing: no lines between blocks (logged) |
| `kResize` (`equipstats.cpp:70`) | the game's control resize (FUN_1011a1a0, thiscall, ret 1Ch: x, y, w, h, 1, 0, 0) | `83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84` | called as a function |
| `kWindows` (`equipstats.cpp:71`) | the menu manager's draw, where it loads the windows' list head | `32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8` | `operand` at `site + 3` (`kWindowsAt`, read with `value_at`) = the address of the windows' list head (`g_windows`, a `char* const*`) |
| `kFrameDraw` (`equipstats.cpp:77`) | the window frame draw (FUN_10233040, thiscall on a `{width, height, kind}` frame, ret 1Ch) | `83 EC 08 53 55 56 8B F1 DB 44 24 18 8B 06 33 DB D1 E8 89 44 24 0C 89 5C 24 10 DF 6C 24 0C D9 44 24 20 D8 25 ?? ?? ?? ?? 57 DE C9 DE E9 E8` | called as a function to draw the box's background/border. Also needs `uispace::find(game)` (patterns in `uispace.cpp`, not this file); either missing turns `equip.stats.move` off (logged) |

**CatsEyeXI address:** `FFXiMain.dll+0x132AA0` (kEquipDraw), `+0x1C7EE0` (kDrawString), `+0x1C7390` (kFonts), `+0x11F760` (YmMenuShape::Draw, kShapeDraw's function; the match is at +0x11F767), `+0x11E580` (the quad drawing it calls), `+0x11A1A0` (kResize), `+0x233040` (kFrameDraw). `kWindows` is not recorded. The Equipment handler global `0x10576038` is only mentioned in a comment, not used. The draw's log line prints the hook offset at run time (`Equipment stats: the Equipment window's drawing hooked at +0x...`).
**Retail:** `kEquipDraw`, `kDrawString`, `kFrameDraw` apart from its one `??` have no wildcards; only `kFonts`, `kShapeDraw`, `kResize`, `kWindows`, `kFrameDraw` wildcard globals or call targets. The global is read back with an operand, not hard-coded. Any of the five required patterns missing means `equip.stats` does nothing (logged once). `kQuadCall = 0x6D` is a hard-coded distance from `kShapeDraw`, checked for `E8` only. Not individually recorded in `docs/retail-signatures.md`; its audit found none of these CatsEyeXI-only, but the audit predates parts of this feature.

**Hard-coded game structure offsets (no pattern):**
- Equipment handler: control `+8`; the ten numbers (FFwNumLabel) start at `+0x88`, `0x20` each (elements 0-7, Attack 8, Defense 9); within a number: Y `+6`, row `+0xA`, digits byte `+0x1C`.
- Control: layout `+4` (layout rect `+0x10`), window `+0xC` (window rect `+0x14`), rect `+0x3A` (4 shorts), pictures list `+0x14`, state `+0x10` (`0xE` closed). List node: next `+0`, item `+0x10`.
- Picture: type short `+0`, shape `+8`; shape: quads list `+4`, count `+0x14`. Quad: texture name `+0x32` (compared to `"menu    colorbal"` and `"font    font    "`), corners from `+0x50`, `0x1C` apart (colour `+0x10`, u `+0x14`, v `+0x18`), local x shorts `+0xC0`, y shorts `+0xC8`, quad size `0xD0`.
- Font table: the font 0 pointer's glyph array indexed by `'-' - 0x20`, node `+4`, quad `+0x10`.

#### Check window stats (`src/core/equipstats_check.h`, included by `equipstats.cpp`)

**Setting:** `[client] equip.stats.check` (default on, but only used while `equip.stats` is on), `equip.stats.check.move`, `.x`, `.y`. Installed from `install_hook()` after the Equipment detour succeeds.
**What it does:** Detours the Check window's draw (FUN_10133a30) and, after the game draws, draws the stats of the checked player's gear in a box under it (or a box of its own). The checked item ids are read straight from the window's handler. The detour is never removed; the hook passes through when the setting is off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kCheckDraw` (`equipstats_check.h:13`) | the Check window's draw (`menu    inspect `, CTkEquip's subclass, FUN_10133a30, vtable 0x103317E0) | `81 EC 8C 00 00 00 53 8B D9 55 56 8B 6B 08 33 C0 BE 01 00 00 00 66 89 44 24 0C 66 89 44 24 0E 66 89 44 24 10 66 89 44 24 12 66 39 75 58 57 7C` | `imports::detour(at, 6, check_hook)` |

**CatsEyeXI address:** `FFXiMain.dll+0x133A30` (FUN_10133a30); the vtable is 0x103317E0 (comment only).
**Retail:** No wildcards at all, so it is as exact as `kEquipDraw`. Not found: logs "the Check window's drawing wasn't found ... no stats on /check".
**Hard-coded game structure offsets:** Check handler control `+8`, the checked items' ids as shorts from `+0x78` (16 slots), then the same control/rect offsets as above.

#### Gear parts that fail to load, logged (`src/core/gearload.cpp`)

**Setting:** always (installed on the first frame, from `overlay.cpp`'s `gearload::on_frame`; no setting). Diagnostic only.
**What it does:** Detours the game's "gear part DAT has loaded" callback (FUN_100d0bd0, cdecl: handle, request). Before the game's own code runs (and frees the request) it reads the request and the entity/actor, applies the game's own tests, and logs a line when the game would give up on a part. It also reads the resource heap's first/last block globals to log free memory. Nothing is changed in the game's behaviour; the hook is never removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kPartLoaded` (`gearload.cpp:33`) | the part-loaded callback (FUN_100d0bd0) | `53 8B 5C 24 0C 56 8B 43 10 85 C0 75 2D 8D 4B 04 E8 ?? ?? ?? ?? 8B F0 85 F6 0F 84 ?? ?? ?? ?? 8D 4C 24 0C E8 ?? ?? ?? ?? 8B 4B 18 8B 54 24 0C 50` | `imports::detour(at, 5, ...)`. `site + 0x10` (`kActorCall`) is a call, target = the weak link's actor getter (FUN_100814d0); `site + 0x23` (`kLoadedCall`) is a call, target = the loaded test (FUN_10070e70). Byte checks on both targets, below |
| `kHeapLargest` (`gearload.cpp:49`) | the heap's "largest free block" (FUN_10028f60, cdecl: heap) | `8B 44 24 04 83 F8 06 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 51 E8` | `site + 7 + 2` reads the `ja rel32` (`kDefaultJump = 7`); at its target the bytes must be `A1 <last> 8B 0D <first>`: `read32(fallback + 1)` = global of the heap's last block, `read32(fallback + 7)` = global of the first block |

Values read back from code after `kPartLoaded`: from the actor getter at `+9` (`kEntitiesAt`) the entity pointer table; from the loaded test at `+5` (`kHandlesAt`) the resource handles global; calls at its `+0x0C` (`kValidCall`) and `+0x2D` (`kPendingCall`) are called as functions. Sanity checks before use: `actor_getter[kEntitiesAt - 3] == 0x8B`, `loaded[kHandlesAt - 2] == 0x8B`, `loaded[0x0C] == 0xE8`, `loaded[0x2D] == 0xE8`; otherwise logged and left off.

**CatsEyeXI address:** `FFXiMain.dll+0xD0BD0` (kPartLoaded's function), `+0x814D0` (weak actor), `+0x70E70` (loaded test), `+0x28F60` (kHeapLargest's function). The hook's offset is also printed in the log at run time.
**Retail:** Globals come from operands (entities, handles, heap), not fixed addresses. Several hard-coded distances from the match (`0x10`, `0x23`, `0x0C`, `0x2D`, `9`, `5`) are guarded only by the byte checks above. Missing `kPartLoaded` logs and turns the feature off; missing `kHeapLargest` just drops the memory figure. Not individually recorded in the retail docs.

**Hard-coded game structure offsets (no pattern):**
- Entity (by index into the entity table, index `< 0x4000`): server id `+0x78`, name `+0x7C` (24 bytes, CP932), actor `+0xA0`, flags `+0x120` (`0x200` = shown). `0x120` is one docs name as unchanged in retail.
- Actor: setup stamp `+0x9EC`.
- Request (0x20 bytes): `+0` stamp, `+4` weak link (`+8` target index, `+0xC` server id), `+0x10` sub-actor, `+0x18` model (`slot << 12 | id`), `+0x1C` DAT file id.
- Resource handle: first dword is the resource; heap block: `+4` in use, `+8` next block, `+0x10` list link, `0x20` header.

#### HUD window placing and Menu Content (`src/core/hudwin.cpp`)

**Setting:** `[client] <id>.move`, `.x`, `.y`, `.width`, `.height` for the windows in `hudwin_windows.hpp`; `menucontent.move`; `mainmenu.move`; `party.move`, `alliance1.move`, `alliance2.move`; `inventory.move`; and so on. The hooks (SetResource, PlaceFrame, Quests) are installed unconditionally on the first window drawn (`skip()` calls `found()`), whatever the settings.
**What it does:** Finds the menu manager and SetLocate/Resize, then places each window by its layout's 16-character name just before it's drawn (`SetLocate` or the game's resize called directly). Detours `SetResourceData` and the frame-part placer so a window made in the top-left 512 x 448 gets its place and its layout's place shifted by as much as Menu Content's box. All hooks pass through when the matching settings are off; windows already moved are put back to the game's place once when a setting is turned off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHelp` (`hudwin.cpp:40`) | the window that has the help line's text (FUN_101182a0). Same pattern as `equipfirst.cpp`'s | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `memcpy` at `site + 7` (`kManagerAt`) = the menu manager global |
| `kSetLocate` (`hudwin.cpp:42`) | SetLocate (FUN_10118d10, thiscall: window, x, y; ret 8). The pattern starts 8 bytes in | `53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | function = `match - 8` (`kSetLocatePrologue`); accepted only if its first byte is `83` (or `E9`, someone else's hook) and byte `+0x6C` (`kResizeAt`) is `E8`; that call's target = the game's resize (0x1011A1A0). Called as functions, not hooked |
| `kSetResource` (`hudwin.cpp:95`) | SetResourceData (FUN_10117b40, thiscall: window, layout; ret 4) | `53 55 56 8B F1 8B 4C 24 10 57 89 4E 04 E8` | `imports::detour(..., 5, set_resource_hook)`; tracks the window being made, then calls `buttonspace::made` |
| `kPlaceFrame` (`hudwin.cpp:98`) | the frame part placer (FUN_10118FF0, thiscall: part, x, y, scaled; ret 0Ch) | `83 EC 14 56 8B F1 57 8B 46 30 85 C0 0F 84` | `imports::detour(..., 6, place_frame_hook)`; after the game's call, adds the Menu Content shift to the part's four place shorts |

**CatsEyeXI address:** `FFXiMain.dll+0x1182A0` (kHelp), `+0x118D10` (SetLocate, the match is at +0x118D18), `+0x11A1A0` (resize, the call at SetLocate+0x6C), `+0x117B40` (kSetResource), `+0x118FF0` (kPlaceFrame).
**Retail:** The manager global is read from the operand, the resize from the call, not hard-coded. `kSetLocate` depends on 8 unmatched bytes before it (first byte `83`/`E9`) and a fixed `0x6C` distance to the resize call. `kSetResource`/`kPlaceFrame` have no wildcards. If SetLocate isn't found `party.move`/`alliance*.move` do nothing; if the manager, SetResource or PlaceFrame is missing `menucontent.move` does nothing (each logged "HUD windows: ... wasn't found in FFXiMain.dll").

**Hard-coded game structure offsets (no pattern):**
- Manager: windows list `+0` (node next `+0`, window `+0x10`), layouts' name at layout `+0x46`, area (4 shorts) `+0x7C`, phase `+0x64` (world phases `0x60`, `0xD0`).
- Window: layout `+4`, owner/handler `+0xC` (handler's window `+8`, vtable slots 1 and 5 compared against found functions), state `+0x10` (`0xE` closed), flags `+0x34` (`0x1000` over-anchor; `0x200000` titled), area `+0x38`, frame rect `+0x3A` (4 shorts), count `+0x58`, close wait `+0x66`, pieces flag `+0x69`, scaled `+0x99`, destroyed `+0x6E`, input delay `+0x56`, pieces list `+0x14`. Piece node: next `+0`, piece `+0x10`, taken-out `+0x14`; piece: type `+0`, alpha `+0xC`, picture `+8`, status `+0x10`, id `+0x3A`, shown `+0x3C`, unseen `+0x44`.
- Frame part: place `+0x1A`/`+0x1C` (and layout place `+0x1E`/`+0x20`), width/height `+0x22`/`+0x24`.
- Layout rect `+0x10` (left, top, right, bottom shorts). Picture shape: quads `+4`, count `+0x14`; quad: name `+0x32`, y shorts `+0xC8`.
- Optional "Options box" owner lookups by layout names `menu    mgcsortw`, `blusortw`, `abisortw`, `emosortw`; the list's own pointer at `+0xC0` is only in a comment.

#### HUD window draw, destroy and background removal (`src/core/hudwin.cpp`)

**Setting:** `[client] <id>.background=0`, `mainmenu.background`, `titles.hide`, `menucontent.hidegil`, `<id>.hide`. Always found with the above; acts only while a setting asks.
**What it does:** Calls two game functions directly (no detour from this file): the window calc and DestroyDrawMenu. `<id>.background=0` clears a window's pieces byte for one draw and restores it (`frameless`/`frame_back`, driven from `chat.cpp`'s draw hook). A closed window (`0xE`) gets its calc and input-delay count run by hand (`left_out`) instead of the game's draw, and an orphaned Options box is closed with DestroyDrawMenu after three draws.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kWinDraw` (`hudwin.cpp:64`) | the window draw FUN_101173D0 matched past the 5 bytes `chat.cpp`'s detour overwrites | `56 57 8B E9 E8 ?? ?? ?? ?? 66 8B 45 56 33 FF 66 3B C7 7E 05 48 66 89 45 56` | `site + 4` (`kWinCalcCall`) is a call; its target is the window calc (FUN_10117120, thiscall), called as a function |
| `kDestroyMenu` (`hudwin.cpp:71`) | the manager's DestroyDrawMenu (FUN_1015D4C0, thiscall: manager, window, wait) | `56 8B 74 24 08 57 8B F9 85 F6 74 ?? 8A 46 6E 84 C0 75 ?? 8B CE C6 46 6E 01 E8` | called as a function (`g_destroy_menu(g_manager, window, 1)`) |

**CatsEyeXI address:** `FFXiMain.dll+0x1173D0` (the window draw; the match is 5 bytes past), `+0x117120` (calc), `+0x15D4C0` (DestroyDrawMenu).
**Retail:** `kWinDraw` depends on `chat.cpp`'s detour having taken exactly 5 bytes (it is matched past them); the calc is read from a call operand. Missing: logged, `<id>.background=0` leaves closed menus' commands up / orphaned Options boxes stay.

#### Quests window button layout (`src/core/hudwin.cpp`)

**Setting:** `[client] menucontent.move` (hooks installed always, inert while it is off).
**What it does:** Detours five functions of the Quests window so that, while one of its pages lays out its buttons (or draws "Records of Eminence" text), button rects asked for are given as at the game's place and rects set are shifted back by as far as the window went. The rect get/set detours pass everything through except those moments. All-or-nothing: one missing pattern means none of the five is hooked.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kQuestPage` (`hudwin.cpp:130`) | a Quests page's layout function (FUN_1022dd30 or FUN_1022e0d0; which is which not recorded) | `A0 ?? ?? ?? ?? 83 EC 08 53 33 DB 56 3A C3 8B F1 75 1F A1 ?? ?? ?? ?? 6A 01 8D 48 20 E8` | `imports::detour(..., 5, ...)` |
| `kQuestPage2` (`hudwin.cpp:135`) | the other Quests page's layout function | `83 EC 08 56 57 8B F1 E8 ?? ?? ?? ?? 33 FF 6A 02 57 57 57 8B CE E8 ?? ?? ?? ?? 8B 4E 08 6A 03 6A 10 E8 ?? ?? ?? ?? 8B 4E 08 6A 04 6A 10 E8 ?? ?? ?? ?? 57 6A 02 8B CE E8 ?? ?? ?? ?? 8B 4E 08 6A 01 C7 46 1C` | `imports::detour(..., 7, ...)` |
| `kQuestText` (`hudwin.cpp:141`) | the "Records of Eminence" text draw (FUN_10231b00) | `81 EC 88 00 00 00 55 56 8B F1 33 ED 8B 46 08 3B C5 0F 84 ?? ?? ?? ?? 57 68 80 80 80 80 6A 6C 6A 0C` | `imports::detour(..., 6, ...)` |
| `kButtonRectGet` (`hudwin.cpp:144`) | the button rect getter (FUN_10116c20, thiscall on a button handle) | `8B 44 24 04 85 C0 74 21 8B 11 83 EA 00 74 35 4A 74 1A 66 C7 00 00 00` | `imports::detour(..., 6, ...)` |
| `kButtonRectSet` (`hudwin.cpp:145`) | the button rect setter (FUN_10116c70) | `8B 44 24 04 85 C0 74 21 8B 11 83 EA 00 74 31 4A 74 1A 66 C7 00 00 00` | `imports::detour(..., 6, ...)` |

**CatsEyeXI address:** `FFXiMain.dll+0x116C20` (get), `+0x116C70` (set), `+0x22DD30` and `+0x22E0D0` (the two pages), `+0x231B00` (text); which page pattern is which is not recorded. The Quests third page (FUN_1022df60) is deliberately not hooked.
**Retail:** `kButtonRectGet` and `kButtonRectSet` differ by one byte (`35` vs `31`, a jump size) and have no wildcards, so a changed jump distance in retail would lose both. `kQuestPage2` is the longest pattern and has four wildcarded calls. Missing: logged "the Quests window's button layout wasn't found", and the Quests buttons stay at the game's place.
**Hard-coded structure offsets:** handle kind `+0` (1 = button), handle's button `+4`, button's window `+0x5C`, list's window `+8`.

#### Handlers that cache a window's rect (`src/core/hudwin.cpp`)

**Setting:** `[client] helptitle.*`, `helpbar.*`, `hpgauge.*` (`.move`), `mainmenu.move` (Linkshell Unequip), the chat log maximized (`fulllog.height`), and the Level / EXP window under a moved Status window. The patterns are always scanned (`found()`); the writes happen only when one of those windows is placed.
**What it does:** After moving or resizing one of these windows, VanaCore re-takes the numbers the window's handler worked out once as it opened (text positions, gauge rect, Unequip sprite place, chat log rows), either by calling the game's own rect getter or button placer on the handler's fields, or by writing fields directly (`title_moved`, `level_moved`, `gauge_moved`, `link_moved`, `help_moved`, `log_resized`). No game code is changed or detoured; each pattern is used to identify a handler by its vtable slot and (for some) to find a game function by a call operand.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kTitleOpened` (`hudwin.cpp:161`) | the help bar title box handler's slot 1 (0x1014F9A0) | `56 8B F1 8B 4E 08 85 C9 74 42 57 8D 7E 14 57 E8 ?? ?? ?? ?? 0F BF 07 0F BF 4E 16 83 C0 04 83 C1 08 89 86 9C 00 00 00 89 8E A0 00 00 00 89 86 FC 00 00 00 89 8E 00 01 00 00` | `site` kept to recognise the handler (vtable slot 1 equals it); call at `site + 0xF` (`kTitleRectCall`): its target = the window rect getter (0x10118B70), called as a function. Handler fields written: rect `+0x14`, text `+0x9C/+0xA0` and `+0xFC/+0x100`, room `+0x104` |
| `kHelpUpdated` (`hudwin.cpp:173`) | CTkHelp's update, vtable slot 5 (0x10139F20, `OnUpdatePrimitive`) | `83 EC 08 33 C0 56 8B F1 8B 4E 08 C7 46 14 06 00 00 00 3B C8 C7 46 18 08 00 00 00 74 ?? 66 89 44 24 04 66 89 44 24 06 66 89 44 24 08 66 89 44 24 0A 8D 44 24 04 50 E8 ?? ?? ?? ?? 0F BF 44 24 04 0F BF 54 24 06 8D 48 06 83 C2 08 89 4E 14 89 56 18 89 4E 78 89 56 7C` | the function is called again (`g_help_updated(handler)`) after each move/resize, once `handler[0][5]` equals it |
| `kGaugeOpened` (`hudwin.cpp:190`) | the HP gauge handler's slot 1 (0x10138E90) | `56 6A 04 8B F1 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 08 8B 00 8B 48 20 89 4E 14 6A 01 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 0B 8B 10 8B 82 84 01 00 00 89 46 18 8D 4E 1C 51 8B 4E 08 E8` | call at `site + 0x39` (`kGaugeRectCall`): its target = the window rect getter, called to refill the handler's rect at `+0x1C` |
| `kLinkOpened` (`hudwin.cpp:203`) | the Linkshell menu handler's slot 1 (0x101E46F0; vtable 0x10337258) | `83 EC 0C 53 56 6A 01 8B F1 E8 ?? ?? ?? ?? 8B 08 83 C4 04 33 DB 8B 91 E8 01 00 00 6A 01 89 56 1C 8B 00 0F BF 56 14 8B 88 04 0B 00 00 6A 01 8D 44 24 14 89 4E 20 8B 4E 08 52 50` | call at `site + 0x4E` (`kLinkPlaceCall`): its target = the button place (0x10118500, thiscall: window, short[4]*, button, 1, 1), called to refill Unequip's place at handler `+0x18/+0x1A` |
| `kLogOpened` (`hudwin.cpp:208`) | the chat log windows' handler slot 1 (0x101371E0; vtable 0x10331B68) | `83 EC 08 53 56 57 8B F1 E8 ?? ?? ?? ?? 66 8B 46 58 33 DB C6 46 60 01 89 5E 64 89 5E 20` | `site` kept to recognise the handler; fields written: rows `+0x58` and `+0x1C`, height `+0x5C` |

**CatsEyeXI address:** `FFXiMain.dll+0x14F9A0` (title), `+0x139F20` (help update), `+0x138E90` (gauge), `+0x1E46F0` (link), `+0x1371E0` (log), `+0x118B70` (window rect getter), `+0x118500` (button place).
**Retail:** The two getter functions come from call operands (`kTitleRectCall`, `kGaugeRectCall`, `kLinkPlaceCall`), not addresses. These patterns embed handler field displacements (`9C 00 00 00`, `FC 00 00 00`, `84 01 00 00`, `04 0B 00 00`, `E8 01 00 00`) with no `??`, so a handler whose layout changed in retail would stop matching (each logs "HUD windows: ... wasn't found", the window moves but leaves its text/bars behind). Not individually recorded in the retail docs.
**Hard-coded structure offsets:** the field offsets above, plus the vtable slot indices 1 and 5, window owner `+0xC`, owner's window `+8`.

#### Check window comment box kept up (`src/core/hudwin_comment.h`, included by `hudwin.cpp`)

**Setting:** `[client] commentwin.keep` (default off).
**What it does:** Rewrites two call instructions inside the Check window's cursor handler (FUN_10133430, vtable slot 0x1C) and one inside the menu manager's per-frame update, using `asm_text::branch(addr, 0xE8, our_function, 0)`, so the bazaar message ("menu    comment ") stays up while the cursor is on an item. The hooks are installed once, from the first window drawn (`comment_hooks`, from `skip()`), and stay in place; they pass through to the game's functions while `commentwin.keep` is off, so nothing is undone as such.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kCheckCursor` (`hudwin_comment.h:13`) | the Check window's cursor-moved handler (0x10133430, thiscall) | `56 8B F1 57 8B 46 08 66 8B 48 4C 51 8B CE E8 ?? ?? ?? ?? 8B F8 8A 86 4F 01 00 00 3C FF 75 ?? 66 85 FF 7D` | `site + 0x5C` (`kCommentOpenCall`) must be `E8` and preceded by `C6 40 17 73`; call rewritten to `open_comment_hook`. `site + 0x76` (`kCommentCloseCall`) must be `E8`, with `51 B9` (`push ecx; mov ecx,imm32`) at `close - 6` and the 4-byte operand at `close - 4` equal to the manager found by `kHelp`; call rewritten to `close_comment_hook`. Targets are called as the game's OpenMenu (CreateDrawMenu) and DestroyMenu |
| `kAreaHide` (`hudwin_comment.h:26`) | the menu manager's per-frame update (from 0x1015EB40) where it hides later windows sharing an area | `66 8B 46 38 66 85 C0 74 ?? 8B D0 23 D7 66 85 D2 74 ?? 39 5E 10 74 ?? 6A 01 6A 00 6A 01 6A 01 56 8B CD E8` | `site + 0x22` (`kAreaHideCall`) must be `E8`; that call (to 0x1015D410, `__thiscall manager: window, 1, 1, 0, 1`) is rewritten to `area_hide_hook`, which skips the hide for the comment box and for the item details while the Check window is up |

**CatsEyeXI address:** `FFXiMain.dll+0x133430` (kCheckCursor), `+0x15D410` (the hide function that `kAreaHide`'s call targets), `+0x15EB40` (the update that contains `kAreaHide`). Read live 2026-11-09 (after the 2026-10-31 retail audit).
**Retail:** Heavily distance-dependent: two hard-coded offsets from `kCheckCursor` (`0x5C`, `0x76`) with byte checks, one from `kAreaHide` (`0x22`), and a `4F 01 00 00` handler field displacement (`+0x14F`) inside the pattern. A mismatch anywhere logs "HUD windows: the Check window's cursor handler or the menus' area hide wasn't found in FFXiMain.dll: [client] commentwin.keep does nothing" and nothing is rewritten (the rewrite of the area hide only happens once both are validated). Possibly not covered by the retail audit (written after it).
**Hard-coded structure offsets:** window `+0x38` (area bits), `+0x10` (state), `+0x6E` (destroyed), handler `+0x14F` (a byte, inside the pattern), handler `+8` (control), control `+0x4C` (a short, inside the pattern).

#### Changes to game code and data not from a pattern (this set of files)

- `equipfirst.cpp`: writes bytes into the game's loaded menu-layout payload (X/Y shorts and the 6 link bytes of eight blocks) rather than into code; restored when the setting goes off. Layout name `menu    mcresmn ` is a literal (also named in `hudwin_place.h` comments, 0x101A5A40).
- `equipstats.cpp`: writes game data in the Equipment handler and its picture quads during and after the game's draw (number Y shorts and digit bytes are changed around the game's call and restored; the balls' and letters' corner Y shorts are moved and restored); resizes and moves other windows with the game's resize (`g_resize`) and writes their `rect +0x14` fields directly (`shift`).
- `hudwin.cpp`: clears and restores a window's pieces byte (`+0x69`), a button's shown byte (`+0x3C`), and quads' Y shorts (`+0xC8`) around one window draw; writes the handler fields listed above.
- `gearload.cpp`: no writes to the game; reads only.
- No IAT or vtable hooks and no fixed-address patches in these files. All fixed addresses mentioned are in comments (CatsEyeXI build). `hudwin_hide.h`, `hudwin_place.h`, `hudwin_under.h`, `hudwin_windows.hpp`, `equipstats_sets.h`, `equipstats_dump.h` and `equipstats_lua.h` contain no patterns, hooks or scans (only comment addresses such as 0x10370288 (the menu table), 0x101A5190, 0x101A3630, 0x1015E3F0).

### Macros, map and menus

#### Macro bar key modes (`src/core/macrobar.cpp`)

**Setting:** `[client] macrobar.mode` (`game`/`hold`/`toggle`), `[client] macrobar.nodelay` (also the pre-0.7.14 `macrobar.instant`).
**What it does:** Patches the macro controller's key handling (`FsMacroController::onKeyDown`, `FFXiMain.dll+0x1A15E0` per the comment) in 14 places: the two tap `je`s made `jmp`, the two hold branches' `mov ecx,esi; call _canUseMacro` (7 bytes) made `jmp` into the book (Ctrl/Alt) paths, the book paths' two waits (`jb`) made NOPs, and, for `toggle`, the hold/book calls of `_canUseMacro` rewritten to VanaCore's `press()`, the hold wait `jb` -> `jmp`, and the book paths' "palette up already" `0F 84` -> `90 E9`. Each site's original bytes are checked first (`site()` refuses and logs if they differ, e.g. macrofix addon already patched them), saved, and written back whenever the mode changes (apply() restores all, then writes the mode's sites). `nodelay` alone patches only the two waits. Only while the setting asks for it; restored when set back to `game`. The macrofix addon is held off (`addons::hold`).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kTapOne` (`macrobar.cpp:58`) | first palette key's tap branch (toggle by tap), with the menu manager's global, both palette names, and the manager's destroy/create calls | `83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 8A 46 0C B9 ?? ?? ?? ?? 3A C3 74 ?? 68 ?? ?? ?? ?? 88 5E 18 E8 ?? ?? ?? ?? 6A 00 53 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 88 5E 0C` | `+5` je patched `74`->`EB`; `+9` call target = `_canUseMacro`; imm32 at `+22` = manager, `+31` other palette name, `+47` this palette name; `+38` call = `_destroy`, `+56` call = `_create` (also checks `imm32(+52)` = manager) |
| `kTapTwo` (`:62`) | second key's tap branch | `83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 80 7E 0C 02` | `+5` je `74`->`EB`; `+9` call must equal kTapOne's `_canUseMacro` |
| `kHold` (`:68`) | the two hold branches (match 0 = first key, match 1 = second; `scan::find(base, image, kHold, n)`) | `83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 8A 46 0C 84 C0 75 ?? 8B 46 14 85 C0 75 ?? E8 ?? ?? ?? ?? 89 46 14 EB ?? E8 ?? ?? ?? ?? 2B 46 14 3B C3 72 ?? 88 5E 18` | `+7` 7 bytes `8B CE E8 rel32` -> `E9 rel32 CC CC` (jmp to the book path's same instruction; hold mode), `+9` call -> `press<N,0x14>` (toggle), `+52` jb `72`->`EB` (toggle) |
| `kBookOne` (`:74`) | Ctrl (key 0x76) book path | `6A FF 6A 04 6A 76 6A 3E E8 ?? ?? ?? ?? 83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 38 5E 0C 0F 84` | `+20` (`mov ecx,esi; call`) is where hold branch jumps to; `+22` call -> `press<1,0x10>` (toggle); `+38` `0F 84` -> `90 E9` (toggle) |
| `kBookTwo` (`:76`) | Alt (key 0x77) book path | `6A FF 6A 04 6A 77 6A 3E E8 ?? ?? ?? ?? 83 C4 10 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 80 7E 0C 02 0F 84` | as kBookOne, palette test at `+39`, call -> `press<2,0x10>` |
| `kWaitLong` (`:81`) | the book path's wait, near-`jb` form | `2B 46 10 3B C3 0F 82 ?? ?? ?? ?? 68 ?? ?? ?? ?? B9` | `+5` `0F 82` -> NOPs (6 bytes); in hold, toggle and nodelay |
| `kWaitShort` (`:82`) | the book path's wait, short-`jb` form | `2B 46 10 3B C3 72 ?? 68 ?? ?? ?? ?? B9` | `+5` `72 xx` -> `90 90`; same modes |

**CatsEyeXI address:** `FFXiMain.dll+0x1A15E0` (the controller's key handler), `FFXiMain.dll+0x1A1B40` (`_canUseMacro`), `GetKey` `FFXiMain.dll+0x123670`, the second counter `FFXiMain.dll+0x1035E0` are stated in comments; the sites themselves not recorded (the log prints `tap_one - base` at runtime).
**Retail:** all eight patterns have almost no `??` beyond call/jump operands, so any recompile that reorders the code loses them; docs/retail-signatures.md says these were found in retail (264 of 278 matched). Not found / bytes differ: logs "Macro bar: ... wasn't found" and `macrobar.mode` does nothing. Fragile: every site offset is hard-coded from the match (`kTapManager=22, kTapOtherName=31, kTapDestroy=38, kTapName=47, kTapCreate=56, kTapJump=5, kTapCall=9, kHoldCall=7, kHoldWait=52, kBookCall=20, kBookStateOne=38, kBookStateTwo=39, kWaitJump=5`), and the patched bytes are overwritten with fixed opcodes. The controller's fields are hard-coded: +0x0C palette up, +0x0D book, +0x10 book-held counter, +0x14 hold counter, +0x18 pad open.

#### Macro bar position (`src/core/macrobar.cpp`)

**Setting:** `[client] macrobar.move` (with `macrobar.x`, `macrobar.y`).
**What it does:** Finds the menu manager's global and `SetLocate` (`FUN_10118d10`), and while the setting is on detours the manager's "open a menu by name" (`_create`, found as the call in the tap branch) so a palette window is put at (x, y) with `SetLocate` as it opens, before the game draws it. Also puts a palette that is already up each frame. The detour is installed the first frame the setting is on (`imports::detour_auto`); moved palettes are put back where they opened when it goes off (the detour itself stays).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kOpenAt` (`macrobar.cpp:326`) | `kTapOne`'s code with the je byte left open (`?? ??`, because macrobar.mode or macrofix makes it a jmp) | `83 C4 10 84 C0 ?? ?? 8B CE E8 ?? ?? ?? ?? 84 C0 74 ?? 8A 46 0C B9 ?? ?? ?? ?? 3A C3 74 ?? 68 ?? ?? ?? ?? 88 5E 18 E8 ?? ?? ?? ?? 6A 00 53 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 88 5E 0C` | `+56` must be `E8`, call target = the manager's open, detoured; `imm32(+52)` must equal the manager from kHelp; also read `+5` (`74` vs `EB`) in the log line |
| `kHelp` (`:329`) | the window that has the help line's text (`FUN_101182a0`), to get the menu manager | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `imm32(site + 7)` = the menu manager's global |
| `kSetLocate` (`:332`) | `SetLocate` (`FUN_10118d10`, thiscall window, x, y), matched 8 bytes in | `53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | function start = `match - 8` (accepted if its first byte is `83` or `E9`, i.e. not detoured away); called to place a window |

**CatsEyeXI address:** `SetLocate` = `FFXiMain.dll+0x118D10` (comment); help-line window `FUN_101182a0`; manager `VA 0x105ECB98` (menulog.cpp comment). The rest not recorded.
**Retail:** `kHelp` and `kSetLocate` have no `??` other than the manager operand; `kSetLocate` start-byte test tolerates an already-detoured function (`E9`). Not found: logs and `macrobar.move` does nothing; kOpenAt not found: palette opens at the top left for a frame. Fragile: hard-coded manager fields (windows list +0, node +0x10 window/+0x14 skip, interface area shorts at +0x7C), window fields (+4 layout, +0x3A frame, +0x42 opened place, +0x70 deleting), layout name at +0x46. `menuplace.h` has the same two patterns (below) with an extra `+0x6C == E8` check that macrobar's copy lacks.

#### Macro bar "why isn't it up" log (`src/core/macrobar.cpp`)

**Setting:** always (a few log lines a session, "Macro bar: ...", at most 60).
**What it does:** Reads `_canUseMacro`'s code to find the globals and helper functions it consults (mode, handler table, game state, menu manager, objects, four allowed windows), and detours the controller's key handler (`FsMacroController::onKeyDown`) to learn the controller and that it runs. Then every 250 ms, on the game's thread, calls the game's own ask-functions and logs which condition says no. Hook installed once at startup (`imports::detour_auto`), never removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kCanUse` (`macrobar.cpp:522`) | `_canUseMacro` (`FFXiMain.dll+0x1A1B40` by comment): first 0x60 bytes | `66 A1 ?? ?? ?? ?? 56 33 F6 66 85 C0 74 0A 0F BF C0 8B 34 85 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 83 39 60 0F 85 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8A 48 59 84 C9 0F 85 ?? ?? ?? ?? 8B 88 C4 00 00 00 85 C9 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 85 C0 74 0B` | operands read: `+2` mode, `+0x14` handler table, `+0x1A` state, `+0x28` manager, `+0x3A` object, `+0x58` busy, `+0xB1/+0xC9/+0xE1/+0xF8` four allowed windows; calls read at `+0x2C` (hidden, `FUN_1015CA10`), `+0x6D` (event, `FUN_10086E80`), `+0x9A` (has-window, `FUN_1015DAE0`), `+0xBE` (is-window, `FUN_1015DAB0`); opcodes checked at `+0x6D,+0x9A,+0xBE,+0x2C`=`E8`, `+0xB0,+0xF7`=`A1`, `+0xC7,+0xDF`=`8B` |
| `kKeyHandler` (`:526`) | the controller's key handler (`FFXiMain.dll+0x1A15E0` by comment) | `53 56 8B F1 8B 4E 04 E8 ?? ?? ?? ?? BB 01 00 00 00 6A FF 53 68 89 00 00 00 6A 3E E8` | detoured at the function start (`imports::detour_auto`) to note the controller and that it ran |

**CatsEyeXI address:** `_canUseMacro` `FFXiMain.dll+0x1A1B40`, key handler `FFXiMain.dll+0x1A15E0`, `FUN_1015CA10`, `FUN_1015DAE0`, `FUN_1015DAB0`, `FUN_10086E80` (all from comments).
**Retail:** pattern is mostly literal; many fixed offsets after the match (list above); fails safe (logs and the diagnosis is off). Hard-coded game structure values: game state value `0x60` (playing), object `+0x59`, `+0xC4`, `+0x68` (short), busy `+0x1C` (short), handler `+0x170`, handler table index `mode*4`, controller `+0x0C/+0x0D/+0x10/+0x14/+0x18`. The object, `+0xC4` and the `0x1C` busy word sit in "the game's big object" ranges that docs/retail-signatures.md says shift in retail (+0x8 from 0x4630); not verified.

#### Map window (`src/core/mapwin.cpp`)

**Setting:** `[client] map.*` (fullscreen, scale, x, y, opacity, border, background, background.color, wheel, center, drag, instantclose, walk) and `[client] regionmap.*` (same, plus `map.track`, `map.pointzoom` for the map only).
**What it does:** The first frame any of these settings wants it (`wanted()`), `install_hooks()` detours the map's limits calculation (`CalcLimit`, detour with 10-byte prologue), rewrites the call to the picture draw and the jmp to the extra-markers draw inside the map's draw function, rewrites the vtable slots of the region map window (slot 4 draw, slot 10 calc), rewrites the two shape-draw calls in YkWndImage's draw (opacity), detours the map's close, and replaces the map's two scrolling-key reads. Never undone: the hooks stay and pass through when the settings are off. Failures are logged per feature ("Map: ... wasn't found ... map.xxx does nothing").

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kCalcLimit` (`mapwin.cpp:45`) | CalcLimit (`FUN_101EC7A0`): pan and cursor limits | `83 EC 08 53 8B 99 8C 00 00 00 85 DB 0F 84` | detour, prologue 10 bytes (`imports::detour`) |
| `kMapDraw` (`:50`) | the map's draw (`FUN_101F90C0`, vtable slot 4) | `56 8B F1 E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 8B CE 5E E9` | `+3` call target = YkWndImage draw (`FUN_101ED330`) and the call's rel32 rewritten to our `image_hook`; `+0x12` the final `jmp` target = extra markers, rel32 rewritten to `marks_hook` |
| `kImageDraw` (`:57`) | YkWndImage's draw (`FUN_101ED330`), checked at the address found via kMapDraw (`scan::find(image, 16, ...) == image`) | `51 A1 ?? ?? ?? ?? 56 57 6A 02 8B 40 0C 8B F1 6A` | `imm32(image + 2)` = the renderer's global (device at `[renderer]+0xC`); calls at `image+0x134` and `image+0x215` (shape draw `FUN_1011F880`, must be the same target) rewritten to `shape_hook` |
| `kRegionCtor` (`:65`) | the region map's constructor (`FUN_101E5710`) | `E8 ?? ?? ?? ?? C7 06 ?? ?? ?? ?? 89 3D ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 89 3D ?? ?? ?? ?? 89 BE 58 01 00 00` | `imm32(site + 7)` = the region map's vtable; accepted only if its slot 4 == YkWndImage's draw; slot 4 and slot 10 overwritten (`write_slot`) with `region_hook` / `region_calc_hook` |
| `kZoom` (`:77`) | the zoom (`FUN_101EC590`, thiscall, ret 4) | `51 D9 44 24 08 56 8B F1 D8 46 58 8B 46 58 89 44 24 04 D9 56 58 D8 5E 60` | function pointer called by VanaCore for `map.wheel`; not hooked |
| `kSetPosAuto` (`:83`) | SetPosAuto (`FUN_101EE0F0`, x, y, ret 8) | `51 56 8B F1 0F BF 46 7C 0F BF 4E 7E 89 44 24 04 DB 44 24 04` | function pointer called for `map.center` / `map.track`; not hooked |
| `kClose` (`:101`) | the map's close (`FUN_101F8B40`) | `8A 44 24 04 56 84 C0 8B F1 74 ?? 8B 46 08 66 C7 86 04 01 00 00 08 00` | detour, prologue 7 bytes (`map.instantclose`, zoom saved in the box) |
| `kPanKeys` (`:112`) | the map calc's two reads of the scrolling keys (`GetAnalogKey` types 0x79 and 0x7A, level 0x26) | `6A 79 6A 26 E8 ?? ?? ?? ?? D9 5C 24 ?? 6A 7A 6A 26 E8` | calls at `+4` and `+0x11` (both must go to the same function) rel32 rewritten to `pan_key_hook` (`map.walk`) |
| `kAnalogKey` (`:114`) | GetAnalogKey (`FUN_10123570`), checked at the address the calls go to | `51 8B 44 24 0C 8B 15` | `imm32(analog + 7)` = the key-level global (read, and written with 0x18 / 0xFFFF) |
| `kSetPlayer` (`:120`) | SetPlayer (`FUN_101F75C0`, flag, ret 4) | `51 53 8A 5C 24 0C 56 8B F1 84 DB C6 44 24 0B 01 0F 84 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 66 C7 86 74 06 00 00 9F 00` | function pointer called for `map.track`; not hooked |

**CatsEyeXI address:** all from comments: limits `FFXiMain.dll+0x1EC7A0`, map draw `+0x1F90C0`, YkWndImage draw `+0x1ED330`, region ctor `+0x1E5710`, region open `+0x1E60E0`, region calc `+0x1E7120`, zoom `+0x1EC590`, SetPosAuto `+0x1EE0F0`, close `+0x1F8B40`, GetAnalogKey `+0x123570`, SetKeyLevel `+0x1232E0`, SetPlayer `+0x1F75C0`, shape draw `+0x11F880`, YkWndImage calc `+0x1ED800`. The log prints the runtime offsets.
**Retail:** all patterns are literal with few `??`; docs/retail-signatures.md lists them as matching in retail. Fragile: fixed offsets from matches (`kImageCall=3, kMarksJump=0x12, kRendererAt=2, kRendererDevice=0xC, kBackingCall=0x134, kPictureCall=0x215, kRegionVtableAt=7, kPanKeyX=4, kPanKeyY=0x11, kKeyLevelAt=7, kVtableDraw=4, kVtableCalc=10`); `uispace::find(game)` (another file) is called first. YkWndImage field offsets hard-coded: cursor +0x1C, pan +0x24, pan limits +0x34, cursor limits +0x44, offset +0x54, scale +0x58, scale-to +0x5C, range +0x60/+0x64, open scale +0x68, margins +0x7C/+0x7E/+0x80/+0x82, pictures +0x8C (bounds +0x20/+0x22/+0x24/+0x26), status flags +0x14 (0x100 mouse cursor, 4 magnet, 8 gliding, 0x40 closing, 0x38 animating), player +0x20/+0x22, glide target +0x2C, glide offset +0x6C, ctrl +8 (cursor shown at +0x62), alpha +0xA6, blend +0xAC, region map kind +0x160, you-marks +0x98, you-marker +0x674 (place +0x67A/+0x67C, facing +0x688). Absolute VAs in comments (e.g. `0x101F7DEC`, `0x101EDB43`, `0x101EE09C`, `0x101ECC5E`, `0x101F85BE`, `0x101E71C9`) are only notes, not used.

#### Single main menu (`src/core/singlemenu.cpp`)

**Setting:** `[client] singlemenu` (default 0).
**What it does:** Finds the game's menu manager, layout/sheet lists and the two main-menu pages' handlers by pattern, then twice a second (while the main menu isn't open) registers a rebuilt `menu    menuwind` layout (socialme's 12 items appended) and a sprite sheet of VanaCore's own on the game's lists. No game code is patched: menuwind's handler object's vtable pointer (`*handler`) is swapped for a copy (`g_ours`, 17 slots) whose slot 1 (opened) and slot 6 (input) are VanaCore's. Reversed (original layout back, table pointer back) when the setting is off; if any check fails once it logs "Single main menu: ... isn't as VanaCore knows it" and leaves the game alone for the session.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHelp` (`singlemenu.cpp:46`) | the window with the help line's text (`FUN_101182a0`) | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `operand(site, 7)` = menu manager; must lie in the image and match the managers read from the other patterns |
| `kLayoutAdd` (`:50`) | layout payload add (`FUN_1015ccb0`) | `56 57 6A 68 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 22 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 15 8B 44 24 0C 8B CE 50 E8 ?? ?? ?? ?? 56 8D 4F 1C E8 ?? ?? ?? ?? 5F 5E C2 04 00` | called as a function (`Payload`) |
| `kLayoutRemove` (`:52`) | layout payload removal (`FUN_1015cd00`) | `8B 44 24 04 56 57 8B F9 8B 08 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 74 1D 56 8D 4F 1C E8 ?? ?? ?? ?? 85 C0 74 10 8B CE E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 5F 5E C2 04 00` | called; call at `+0xD` = find-layout-by-name (`FUN_1015cd60`), verified by comparing its first 10 bytes `51 53 56 57 8D 79 1C 8B CF E8` and the 8 bytes at `+0x2C` `8D 4E 46 6A 10 51 53 E8` |
| `kSheetAdd` (`:60`) | sprite sheet add (`FUN_1011fe90`) | `56 57 6A 40 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 21 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 44 24 0C 8B CE 50 E8 ?? ?? ?? ?? 56 8B CF E8 ?? ?? ?? ?? 5F 5E C2 04 00` | called; must equal the call at `kSheetThunks+18` |
| `kSheetRemove` (`:62`) | sprite sheet removal by name (`FUN_1011fed0`) | `83 EC 0C 8B 44 24 10 56 57 8B F9 8B 08 89 7C 24 10 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 89 74 24 08 0F 84` | called; `operand(site, 0x40)` must equal manager + 0x1C (the layouts list); must equal call at `kSheetThunks+32+18` |
| `kSheetThunks` (`:64`) | the two thunks side by side that give the sheets' list | `51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 56 57 8B F1 E8` | `operand(site, 14)` = sheets' list; `operand(site, 32+14)` must equal it; calls at `+18` and `+32+18` checked against sheet add/remove |
| `kMenuEntry` (`:70`) | the game's table-of-menus entry for `menu    menuwind` (a data string, scanned across the whole image, `scan::find(base, size, ...)`) | `6D 65 6E 75 20 20 20 20 6D 65 6E 75 77 69 6E 64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` | `operand(site, 0x20)` = global holding menuwind's handler |
| `kSocialEntry` (`:71`) | same for `menu    socialme` (data) | `6D 65 6E 75 20 20 20 20 73 6F 63 69 61 6C 6D 65 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` | `operand(site, 0x20)` = global holding socialme's handler |
| `kMenuMake` (`:75`) | menuwind's handler constructor (`0x1013F720` in comment) | `56 8B F1 8D 4E 04 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? 66 C7 46 0E FF FF C7 46 08 00 00 00 00 C6 46 0C 01 C7 06 ?? ?? ?? ?? C7 46 14 00 00 00 00 E8` | `operand(site, 0x28)` = the handler's table of functions (slot 1 must equal menuwind-opened, slot 6 menuwind-input) |
| `kMenuInput` (`:81`) | menuwind's input function (`0x1013FF40`) | `0F BF 44 24 04 83 C0 FD 56 83 F8 19 8B F1 0F 87 ?? ?? ?? ?? 33 C9 8A 88 ?? ?? ?? ?? FF 24 8D ?? ?? ?? ?? 57 6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8` | called (`g_menu_input`); `operand(+0x29)` = other page's name (must read `menu    socialme`), `operand(+0x2E)` = manager |
| `kSocialInput` (`:84`) | socialme's input function (`0x1014D500`) | `0F BF 44 24 04 83 C0 FD 56 83 F8 19 8B F1 0F 87 ?? ?? ?? ?? 33 C9 8A 88 ?? ?? ?? ?? FF 24 8D ?? ?? ?? ?? 6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8` | called (`g_social_input`); `operand(+0x28)` = name (`menu    menuwind`), `operand(+0x2D)` = manager |
| `kMenuOpened` (`:88`) | menuwind opened (`0x1013FBD0`) | `66 A1 ?? ?? ?? ?? 56 57 33 FF 66 85 C0 8B F1 74 0A 0F BF C0 8B 3C 85 ?? ?? ?? ?? 6A 00 E8` | called; must be handler table slot 1 |
| `kSocialOpened` (`:92`) | socialme opened (`0x1014D470`) | `56 8B F1 6A 0F 6A 10 8B 4E 08 E8 ?? ?? ?? ?? 8B 4E 08 6A 0E 6A 0F E8 ?? ?? ?? ?? 8B 4E 08 6A 0D 6A 0F E8 ?? ?? ?? ?? 8B 46 08 C6 80 86 00 00 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 0C 8B 4E 08 6A 05 6A 05 E8` | call at `+0xA` = SetButtonStatus (`0x10118760`), call at `+0x36` = the Monstrosity check (`0x10097B60`), both called by VanaCore |

**CatsEyeXI address:** given in comments only for the 2025-12-26 build: menuwind handler constructor `FFXiMain.dll+0x13F720`, input `+0x13FF40`, opened `+0x13FBD0`; socialme input `+0x14D500`, opened `+0x14D470`; `FUN_101182a0`, `FUN_1015ccb0`, `FUN_1015cd00`, `FUN_1015cd60`, `FUN_1011fe90`, `FUN_1011fed0`, SetButtonStatus `+0x118760`, Monstrosity check `+0x97B60`. The log prints the two handlers' globals at runtime.
**Retail:** only the manager, sheets, globals, table, and names are read back with `operand()`; the patterns are literal otherwise. Strong cross-checks protect against drift (managers must agree, names must match, table slots must equal the found functions, call targets must lie in the image). Fragile: a hard-coded layout payload format (header 0x20, count at +0x11, block fields +2/+4/+0xA/+0xC, item id +0x12, links +0x15, sprites +0x1B / +0x14, sprite size 20), list node (+0 next, +0x10 item, +0x14 skip), layout (+8 parse, +0x46 name), window +4 layout, sheet name +0x24, handler +8 window, manager lists +0 windows / +0x1C layouts, the handler's 17-slot table (slots 1 and 6), item/event ids. The handler/table are written live (`*handler` swapped), not code.

#### Menu hover (`src/core/menuhover.cpp`)

**Setting:** `[client] menu.hovermove` (default 1), `[client] menu.nomouse` (default 0).
**What it does:** Hooks the game's mouse frame so a mouse at rest doesn't pick a menu row or give a window the keys. Three groups of `call rel32` instructions are rewritten (5 bytes, rel32 only) to VanaCore's functions: `IsOnButton` and `SetCursol` calls in the mouse frame, the Items/storage/Equipment lists' `IsOnWindow` calls (up to 8 sites), and the macro edit rows' two `IsOnWindow` calls for the Button Edit box. Installed the first frame either setting (or the interface veil) wants it; not undone (hooks pass through when off), except the first pick call is put back if the select call can't be written.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHover` (`menuhover.cpp:24`) | the hover in the game's mouse frame (`FUN_10124EA0`; site `0x101258CE`) | `8B CD E8 ?? ?? ?? ?? 66 85 C0 66 89 46 52 7E 0F 66 3B C7 74 0A 6A 00 50 8B CD E8 ?? ?? ?? ??` | `+2` call (IsOnButton) and `+26` call (SetCursol) rel32 rewritten to `pick_hook` / `select_hook`; targets must be inside the image; the mouse object is read from the hook's `point` argument (`point - 4`) |
| `kWindowHover` (`:35`) | the lists' `IsOnWindow` calls before `call [eax+18h]` (0x101F105B, 0x101F1080, 0x101F10A5) | `8D 54 24 0C 52 E8 ?? ?? ?? ?? 84 C0 74 0A 8B 07 56 6A ?? 8B CF FF 50 18` | `scan::find(base, size, ..., i)` for i = 0..7; `+5` call rel32 rewritten to `on_window_hook` (all must share one target) |
| `kBoxHover` (`:50`) | the macro edit rows' `IsOnWindow` for the Button Edit box (0x101A3407, 0x101A3AEA) | `8D 54 24 0C 52 E8 ?? ?? ?? ?? 84 C0 74 0E 8B 06 66 0F BE CB 51 6A 08 8B CE FF 50 18` | must match exactly 2 times (a third match aborts); `+5` call rel32 rewritten to `box_hook` |

**CatsEyeXI address:** `FUN_10124EA0` (`FFXiMain.dll+0x124EA0`), hover at `+0x1258CE`; list sites `+0x1F105B`, `+0x1F1080`, `+0x1F10A5`; edit row sites `+0x1A3407`, `+0x1A3AEA`; the Button Edit box's handler global `0x1062DB18` is only a note (all from comments).
**Retail:** all three patterns are literal except the key id (`6A ??`) in kWindowHover; sites are counted, not fixed. `kBoxSites = 2` and `kMostWindowSites = 8` are assumptions about how many copies the compiler makes. Mouse object fields hard-coded: +0x58 held, +0x60 pressed (bits 1/2/4), point at +4. IsOnButton/SetCursol/IsOnWindow are `thiscall` with the stated stack arguments.

#### Menu placing helper (`src/core/menuplace.h`)

**Setting:** always (shared header; used by buffwin.cpp, castbar.cpp, partyrows.cpp; their own settings turn the windows on).
**What it does:** Finds the menu manager's global and `SetLocate` so those windows can be placed with the game's own placing; no patch. The same two patterns as macrobar's and singlemenu's.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHelp` (`menuplace.h:21`) | the window with the help line's text (`FUN_101182a0`) | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `memcpy` the 4 bytes at `site + 7` = menu manager |
| `kSetLocate` (`menuplace.h:23`) | `SetLocate` (`FUN_10118d10`) | `53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | function = `match - 8`; accepted if first byte is `83` or `E9` and the byte at `+0x6C` is `E8` |

**CatsEyeXI address:** `SetLocate` `FFXiMain.dll+0x118D10`, help line `+0x1182A0` (comments in macrobar.cpp / menuplace.h).
**Retail:** no `??` except the manager operand; offsets `kManagerAt=7`, `kSetLocatePrologue=8`, `kResizeAt=0x6C` hard-coded. Not found: the caller's `missing` message is logged.

#### Menu focus log (`src/core/menulog.cpp`)

**Setting:** always (the hook), the rest of the log only while Record Stats Report runs.
**What it does:** Detours `SetCurMCD` (the game's one place that sets the window the keys go to) so a change to an Options box (Items list's `itmsort2`, Magic's `mgcsortw`, ...) is logged with the caller and the previous changes. The detour is installed first frame FFXiMain.dll is there, never removed. The rest of menulog reads the menu manager from `hudwin::manager()` (found in hudwin.cpp, not here).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSetCur` (`menulog.cpp:394`) | SetCurMCD (CTkMenuMng, thiscall, window on stack, ret 4) | `56 8B F1 57 8B 7C 24 0C 8B 4E 54 3B CF 74 36 85 C9 74 11 6A 00 E8 ?? ?? ?? ?? 8B 46 54 C6 80 98 00 00 00 00 8B 4E 54 89 7E 54` | detour, prologue 8 bytes (`imports::detour`) |

**CatsEyeXI address:** `FFXiMain.dll+0x15D580` (SetCurMCD, comment); menu manager VA `0x105ECB98` (`FFXiMain.dll+0x5ECB98`, comment, found by hudwin.cpp).
**Retail:** one literal pattern with `??` only over a call operand; not found / detour fails: logs and the focus log is off. Hard-coded structure offsets in menulog.cpp (manager +0x54 current, +0x58 previous, list node +0x10 item, +0x14 out; window +4 layout, +0xC owner, +0x10 state, +0x14 pieces, +0x34 flags, +0x38 area, +0x3A..0x40 frame, +0x4C cursor, +0x58 button count, +0x64/+0x66 close timers, +0x69 frame drawn, +0x6E destroyed, +0x70 to be deleted, +0x94 resize countdown; layout name +0x46, kept cursor +0x4E; a piece's +0 kind, button id +0x3A). The manager's `+0x54`/`+0x58` and the `0x15D580`-area shifts are not in docs/retail-signatures.md's list.

### Interface windows

#### Config menu item (`src/core/configmenu.cpp`)

**Setting:** `[client] configmenu` (default 1).
**What it does:** Adds a "VanaCore" item (and its extra items) to the game's Config list without patching code. It finds the menu manager, the layouts' and sprite sheets' lists and the Config list's handler, registers a sheet of its own, rebuilds the Config layout with the extra items (via the game's own layout add/remove functions), and swaps the handler object's table pointer (`handler+0`) for a copy whose slot 4 (draw) and slot 6 (input) are ours. Done only while the setting is on; turning it off takes the layout out, puts the game's back from a copy of its payload and restores the table pointer. No bytes of game code are written. If any signature or cross-check fails, one log line (`Config menu: ... isn't as VanaCore knows it`) and the game's list is left alone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHelp` (`configmenu.cpp:49`) | the window's help-line function (FUN_101182a0) | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8 ?? ?? ?? ?? 8B F0 85 F6 0F 84 ?? ?? ?? ?? 8A 46 4A 84 C0 0F 84 ?? ?? ?? ?? 8B 56 4C` | `operand(site, 7)` = the menu manager's address; call at `site+0x1F` = the window's "part for an item" function; the function itself called as `g_window_help` |
| `kLayoutAdd` (`configmenu.cpp:56`) | layout payload parse-and-add (FUN_1015ccb0) | `56 57 6A 68 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 22 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 15 8B 44 24 0C 8B CE 50 E8 ?? ?? ?? ?? 56 8D 4F 1C E8 ?? ?? ?? ?? 5F 5E C2 04 00` | called to put VanaCore's rebuilt layout on the manager's list |
| `kLayoutRemove` (`configmenu.cpp:62`) | layout take-off-and-free (FUN_1015cd00) | `8B 44 24 04 56 57 8B F9 8B 08 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 74 1D 56 8D 4F 1C E8 ?? ?? ?? ?? 85 C0 74 10 8B CE E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 5F 5E C2 04 00` | called to remove layouts; call at `site+0xD` is the "layout of a name" function (FUN_1015cd60), cross-checked by bytes `51 53 56 57 8D 79 1C 8B CF E8` at its start and `8D 4E 46 6A 10 51 53 E8` at `+0x2C` |
| `kSheetAdd` (`configmenu.cpp:73`) | sprite-sheet payload add (FUN_1011fe90) | `56 57 6A 40 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 21 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 44 24 0C 8B CE 50 E8 ?? ?? ?? ?? 56 8B CF E8 ?? ?? ?? ?? 5F 5E C2 04 00` | called to register `menu    vanacore`; checked equal to the add call in the thunks |
| `kSheetRemove` (`configmenu.cpp:79`) | sprite-sheet remove (FUN_1011fed0) | `83 EC 0C 8B 44 24 10 56 57 8B F9 8B 08 89 7C 24 10 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 89 74 24 08 0F 84` | called to remove the sheet; `operand(site, 0x40)` must equal manager+0x1C (cross-check that the manager is the same) |
| `kSheetThunks` (`configmenu.cpp:84`) | the DAT sheet chunks' load/unload thunks side by side (0x1011db80, 0x1011dba0) | `51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 56 57 8B F1 E8` | `operand(site, 14)` = the sheets' list; calls at `site+18` and `site+32+18` must be the sheet add and remove; second `mov ecx` at `site+32+14` must hold the same list |
| `kConfigInput` (`configmenu.cpp:92`) | the Config list handler's input function (FUN_10194ec0, table slot 6) | `66 83 7C 24 04 05 0F 85 ?? ?? ?? ?? 0F BF 44 24 08 48 83 F8 0C 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ?? 6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8` | `operand(site, 44)` must equal the manager; must equal slot 6 of the handler's table; called by our input for non-VanaCore items |
| `kConfigMake` (`configmenu.cpp:98`) | where the Config handler is created (inside FUN_101a74b0) | `6A 14 E8 ?? ?? ?? ?? 8B F0 83 C4 04 83 CD FF 3B F7 B3 01 74 20 8D 4E 04 E8 ?? ?? ?? ?? 66 89 6E 0E 89 7E 08 88 5E 0C C7 06 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? EB 06 89 3D ?? ?? ?? ?? 6A 44` | `operand(site, 0x29)` = the handler's table; `operand(site, 0x2F)` (and again at `0x37`) = the global holding the handler pointer |
| `kMenuEntry` (`configmenu.cpp:102`) | the game's table of menus, found as data (scanned over the whole image with `scan::find(base, size, ...)`): `"menu    configwi"` then 16 zero bytes | `6D 65 6E 75 20 20 20 20 63 6F 6E 66 69 67 77 69 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00` | `operand(site, 0x20)` must equal the handler global |

Extra checks that depend on game code shape (no pattern): the handler table has 17 slots; slot 1 must start `56 8B F1 6A 0D 6A 0B 8B 4E 08 E8`, slot 4 must be `C6 41 10 01 C3`.

**CatsEyeXI address:** `FFXiMain.dll+0x182A0` (kHelp), `+0x15CCB0` (kLayoutAdd), `+0x15CD00` (kLayoutRemove), `+0x15CD60` (layout-of-a-name), `+0x11FE90` (kSheetAdd), `+0x11FED0` (kSheetRemove), `+0x11DB80`/`+0x11DBA0` (thunks), `+0x194EC0` (kConfigInput), inside `+0x1A74B0` (kConfigMake), sheet list in the comment at `0x104DFA88` (`+0x4DFA88`). The log line prints the manager and handler offsets at run time.
**Retail:** every pattern has `??` over call and global operands, so it survives moved globals. Several hard-coded structure offsets are not read back: manager windows `+0`/layouts `+0x1C`, layout name `+0x46`, window layout `+4` / cursor `+0x4C`, part help flag `+0x4A` / text `+0x4C`, sheet name `+0x24`, handler window `+8`, list node `+0x10`/`+0x14`, layout payload block layout (`0x20` header, 20-byte sprites). `kConfigMake` bakes `6A 14` (object size 0x14) and `83 CD FF`; `kSheetThunks` bakes the `0x30` payload offset. Fragile: the whole feature is all-or-nothing with nine patterns, so any one miss turns it off (logged with the first missing part's name).

#### Status window layout (`src/core/statuswindow.cpp`)

**Setting:** `[client] status` (on switch) and `status.jobs`, `status.itemlevel`, `status.colors`, `status.separator`, `status.name.*`, `status.hp/mp/tp.color`. The hook is also installed with status off when `status.exp.level` is off, `status.exp.shortnext` is on, or the game is in Japanese.
**What it does:** Detours the Status window's draw function (CTkPersona::OnDrawPrimitive, 5 bytes of prologue) and redraws the window's text itself, using game functions and globals read back from the calls and operands inside it. Two calls to the jobs' picture draw are redirected (call rewrite) to ours. The detour stays installed once made; behaviour follows the settings per frame (the game's own drawing is called when off). Not undone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kPersonaDraw` (`statuswindow.cpp:39`) | the Status window's draw (FUN_10146ac0) | `83 EC 24 53 55 56 6A 01 8B F1 E8 ?? ?? ?? ?? 6A 00 8B E8 E8 ?? ?? ?? ?? 8B D8 83 C4 08 85 DB 0F 84 ?? ?? ?? ?? 66 8B 56 16 66 8B 46 14 8D 4B 02 68 80 80 80 80` | `imports::detour` with 5-byte prologue; many fixed offsets from the match, each validated by `is_call` or a byte check: calls at `+0x0A` (menu resource), `+0x13` (player data), `+0x4A` (YkDrawStringLimited), `+0x6D`, `+0x92`, `+0xBD` (YkDrawString), `+0xEF` and `+0x16A` (job picture shape draws, redirected to `job_shape`), `+0x16F`, `+0x176`, `+0x1F7`, `+0x239`; globals read with `operand()` at `+0x50` (flags), `+0x60` (jobs), `+0x7B` (sync colour immediate), `+0x2AA` (max HP), `+0x353` (max MP), `+0x412` (stats), `+0x441` (bonuses); each preceded by an opcode byte check (`A0`, `BF`, `A1`, `6D`) and the picture calls by bytes `8B 8C 8A 50 03 00 00 50` |
| `kShapeScaled` (`statuswindow.cpp:114`) | YmMenuShape::Draw with a scale (FUN_1011f880), first 5 bytes `??` because buttons.cpp may already have hooked them | `?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8` | called to draw narrowed job pictures; if not found, long job names aren't narrowed (logged) |
| `kLanguage` (`statuswindow.cpp:153`) | the language getter (FUN_10259b10) | `8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0F 83 F8 02 74 05 83 F8 03 75 05 B8 01 00 00 00 50 E8` | `operand(site, 2)` = the settings global; language read at `settings+8` each use (0 Japanese, 1 English, 2 French, 3 German) |

Also derived without scanning: YkDrawString's scaled call at `string+0x20`, the font getter at `+0x69` of that, the quad draw at `shape+0x6D` (FUN_1011e580).
**CatsEyeXI address:** `FFXiMain.dll+0x146AC0` (kPersonaDraw), `+0x11F880` (kShapeScaled), `+0x259B10` (kLanguage), `+0x11E580` (quad draw). The log prints the hook address at run time.
**Retail:** call/global operands are `??`, but every offset added to the match (listed above) is hard-coded and only validated by opcode bytes. `docs/retail-signatures.md` says this is the one UI function with extra instructions in retail (it names `uiscale.cpp:27`, not this), so the `+0x...` offsets in the Status draw are worth rechecking with the audit's changed-function list. Player-data offsets `+2/+0x20/+0x24/+0x28` and menu-resource `+0x350` are hard-coded field offsets. Not-found cases log and turn off `[client] status`.

#### Status window menu font (`src/core/statuswindow.cpp`)

**Setting:** `[client] status.name.font` (True Font name) / `status` on.
**What it does:** Finds the game's menu-font string printer and its helper calls so the name can be drawn in the titles' font. Reads only (calls the game's functions); no code is changed. If missing, the name is drawn small (logged).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kPrintString` (`statuswindow.cpp:133`) | the whole string printer (FUN_1011ba10) | `83 EC 08 8B 44 24 10 53 55 8B 6C 24 14 8B 50 04 56 8B F1 8B 08 89 4C 24 0C 57 8B CE 89 54 24 14 89 6C 24 20 E8 ?? ?? ?? ?? 8B CE E8` | calls at `site+0x24`, `+0x2B`, `+0x50`, `+0x93`, `+0x9E` (states, bind, next code, print, advance) taken as the helpers (each checked `E8`) |
| `kTitleText` (`statuswindow.cpp:140`) | the code drawing a window's title, which names the renderer global | `8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8D 8E A4 00 00 00 C7 40 64 80 80 80 80` | `operand(site, 2)` = the renderer global |

**CatsEyeXI address:** `FFXiMain.dll+0x11BA10` (kPrintString); kTitleText not recorded.
**Retail:** `??` over globals and call targets. Fixed: call offsets from `kPrintString` and the `A4 00 00 00` / `C7 40 64` immediates inside kTitleText. The renderer's corner colours at `+0x10/+0x2C/+0x48/+0x64` are hard-coded field offsets.

#### Status window taller (`src/core/statuswindow.cpp`)

**Setting:** `[client] status.taller` (default 1).
**What it does:** Calls the game's window resize function on the Status window and walks the menu manager's windows list (a global read from the pattern) to find it. No code changed; if either is missing it logs and `status.taller` does nothing.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kResize` (`statuswindow.cpp:97`) | the control resize (FUN_1011a1a0, `ret 1Ch`) | `83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84` | the function called as `g_resize` |
| `kWindows` (`statuswindow.cpp:100`) | the menu manager's drawing loop over its windows list (same pattern as `gamewin.cpp` `kManager`) | `32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8` | `operand(site, 3)` = the windows list global |

**CatsEyeXI address:** `FFXiMain.dll+0x11A1A0` (kResize); kWindows not recorded.
**Retail:** `??` over operands; hard-coded structure offsets: control rect `+0x3A`, window `+0xC`, layout `+4`, layout name `+0x46`, picture list `+0x14`, window rect `+0x14`, mark `+0x24`, quad fields `+0x32/+0x50/+0x1C/+0xC0/+0xC8`.

#### Level / EXP window (`src/core/statuswindow.cpp`)

**Setting:** `[client] status.exp.level` (default 1), `status.exp.shortnext` (default 0).
**What it does:** Detours the Level / EXP window's draw (CTkPersonaLevel::OnDrawPrimitive, 5-byte prologue) and rewrites two calls inside it ("Level 75" string draw and the "NEXTLevel" text lookup) to ours. Installed when the hook is wanted (see Status window); stays installed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kLevelDraw` (`statuswindow.cpp:122`) | the Level / EXP window's draw (FUN_101470a0) | `83 EC 28 55 56 57 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 12 F6 40 03 40 74 0C BF 40 40 40 80` | detour (5 bytes); `site+0x43` operand = master flags global (preceded by `A0`); calls at `+0x1ED` and `+0x263` redirected (the second preceded by `6A 58`) |

**CatsEyeXI address:** `FFXiMain.dll+0x1470A0`.
**Retail:** `??` over calls only; offsets `+0x43/+0x1ED/+0x263` hard-coded and checked by opcode bytes; the window's master flag `+0x1C` and text-table ids `0Ch:58h` are hard-coded.

#### Target window (`src/core/targetwin.cpp`)

**Setting:** `[client] target.*` (hide, move, x, y, width, bar.*, lock*, arrow*, name.*, label*, padding.*, spacing, background, title.font.*), and `[graphics] nameplates.gm` for the GM mark.
**What it does:** Finds the menu manager and the window-place function, then rewrites individual `call rel32` sites inside the Target window's draw (FUN_1014EEF0) to VanaCore's functions (one 4-byte offset write each, via `VirtualProtect`): HP bar (two sites), lock picture, arrow, target name (two sites), and the status mark. Installed only when a setting needs it (`hooks_wanted`); not undone once written, the hook calls through to the game's function when the setting is default.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHelp` (`targetwin.cpp:39`) | the help-line window function (FUN_101182a0), as in uiscale | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `operand(site, 7)` = menu manager |
| `kSetLocate` (`targetwin.cpp:46`) | SetLocate (FUN_10118d10), matched from the `push ebx` 8 bytes after its start (itemlist.cpp's detour prologue may precede) | `53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | function start = match`-8` (byte must be `83` or `E9`, and `start[0x6C]` must be `E8`); call at `+0x6C` = the resize function |
| `kDraw` (`targetwin.cpp:62`) | the Target window's draw (FUN_1014EEF0, vtable slot 4) | `83 EC 08 56 8B F1 57 C6 44 24 0C 01 8A 46 65 84 C0 74 63 66 8B 56 4E 8A 4E 66` | call sites rewritten (each checked `E8`): `+0x2D` and `+0xE1` HP bar (must share a target), `+0x2DC` lock draw, `+0x3B5` arrow (bytes before: `68 00 00 80 3F 68 00 00 80 3F 52 50`), `+0x2B2` and `+0x68` name (same target; `+0x34` inside it is the text-fit call), `+0x146` status mark (bytes before: `8B 4C 8A 80`; `+0x115` is the resources getter call); `+0x2BA` checked for `8B 15` and `F6 42 5C 03`, `operand(draw+0x2BA, 2)` = the target struct's global |

Further hard-coded offsets reached from the name function: `+0x54` (`push offset ":"`, verified by string compare), `+0x8A` (line draw call). HP bar function: `+2`, `+0x2B`, wrapper `+0x32`; resource shapes `+0x98`, `+0x184`.
**CatsEyeXI address:** `FFXiMain.dll+0x14EEF0` (kDraw), `+0x118D10` (SetLocate), `+0x182A0` (kHelp), HP bar function FUN_101C97F0 (`+0x1C97F0`), name draw FUN_101C8700 (`+0x1C8700`), CTkTarget owner global `0x105762F8` (`+0x5762F8`), fixed `FUN_10097310` for the mark code.
**Retail:** `??` only over the manager operand. Every call offset into `kDraw` is hard-coded and only opcode-checked (docs say a function's offsets are checked only by the changed-function list). Hard-coded structure fields: owner `+0x65` (scripted name) / `+0xBA..+0xC2` (arrow), target struct `+0x5C` (lock flag), manager `+0x7C` area, window `+4/+0xC/+0x14/+0x3A/+0x42/+0x70`, layout name `+0x46`, quad `+0x32/+0x50/+0x1C/+0x13/+0xC0/+0xC8`. A failed check logs which part is not as expected and leaves it the game's.

#### Interface size (`src/core/uiscale.cpp`)

**Setting:** `[ffxi] resolution.ui` (and the `window-<share>` form).
**What it does:** Remakes the game's menu render target and depth surface at another size by calling the game's own create functions, then writes the size into the game's copies (0037/0038 globals, the scene object's menu size, the menu manager's area, chat logs' viewport copy) and re-places open windows with SetLocate/resize. No code bytes are patched. If the core set is not found it logs and the setting applies from the next start.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDisplay` (`uiscale.cpp:27`) | the display setup (FUN_10010c00), scene object taking the back buffer's and menus' size | `66 8B 82 5C 01 00 00 66 89 46 10 8B 0D ?? ?? ?? ?? 66 8B 91 60 01 00 00 66 89 56 12 66 A1 ?? ?? ?? ?? 66 89 46 14 66 8B 0D ?? ?? ?? ??` | `operand(site, 0xD)` = graphics object global (must equal kTarget's); `operand(site, 0x1E)` = 0037 (menu width) global; `operand(site, 0x29)` = 0038 (menu height) global |
| `kTarget` (`uiscale.cpp:34`) | the menu render-target creation | `53 8D 54 24 38 6A 01 52 51 8B 0D ?? ?? ?? ?? 50 89 5C 24 48 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 89 81 E0 01 00 00 8B 15 ?? ?? ?? ?? 8B 44 24 34 89 82 E4 01 00 00 66 8B 46 16 66 8B 4E 14 50 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 89 81 E8 01 00 00` | `operand(site, 0xB)` = gfx global; call at `+0x14` = create-target function; call at `+0x45` (must be `E8`) = create-depth function |
| `kScene` (`uiscale.cpp:39`) | the scene object's global (as Ashita's libs\scaling finds it) | `A1 ?? ?? ?? ?? 85 C0 5E 74 ?? ?? 80` | `operand(site, 1)` = scene pointer |
| `kHelp` (`uiscale.cpp:42`) | FUN_101182a0 | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `operand(site, 7)` = menu manager; at `+0x63`, if `8B 0D`, `operand` at `+0x65` = the help-line global |
| `kSetLocate` (`uiscale.cpp:48`) | SetLocate (FUN_10118d10), as in targetwin | `53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | start = match`-8`; `+0x6C` call = resize |
| `kLog1` (`uiscale.cpp:53`) | Config's chat-width callback for log 1 | `8B 0D ?? ?? ?? ?? 8B 54 24 0C 52 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8` | `operand(site, 2)` = log 1 global; call at `+0x16` = log layout function (FUN_10161850) |
| `kLog2` (`uiscale.cpp:54`) | the same for log 2 | `8B 0D ?? ?? ?? ?? 56 8B 01 FF 50 5C 8B 0D ?? ?? ?? ?? E8` | `operand(site, 2)` = log 2 global; call at `+0x12` must be the same layout function |
| `kConfig` (`uiscale.cpp:57`) | the Config setting getter (push 0x9C, 0xC5) | `68 9C 00 00 00 E8 ?? ?? ?? ?? 50 68 C5 00 00 00 E8` | call at `+5` = `g_config_get` (called with 0x9C, 0xC5 for the chat widths) |
| `kViewport` (`uiscale.cpp:63`) | the chat log clip code (FUN_10160ad0), a `rep movsd` copy of the viewport | `B9 06 00 00 00 8B F0 BF ?? ?? ?? ?? F3 A5 B9 06 00 00 00 BE ?? ?? ?? ?? BF ?? ?? ?? ?? F3 A5` | `operand(site, 8)` = the viewport copy; `[2]` and `[3]` written with the new width and height |

**CatsEyeXI address:** `FFXiMain.dll+0x10C00` region (kDisplay), `+0x182A0` (kHelp), `+0x118D10` (SetLocate), `+0x161850` (log layout), `+0x160AD0` (kViewport region), `+0x9E00` per-frame surface function named in a comment; menu manager global `0x105ECB98` (`+0x5ECB98`). Others not recorded.
**Retail:** `docs/retail-signatures.md` says `kDisplay` is the one UI function with extra instructions in retail, though its pattern still matches. Hard-coded field offsets not read back: graphics object `+0x15C/+0x160` (inside the pattern), `+0x1E0` target / `+0x1E8` depth, scene `+0x10`/`+0x14`, manager area `+0x7C/+0x80`, anchor tops `+0xA0`, window `+4/+0x3A/+0x42/+0x52/+0x54/+0x70/+0x99`, log object `+8` and `+0xC`. `kLog1`/`kLog2` hard-code vtable slot `+0x5C`; Config setting ids `0x9C`/`0xC5`. Failure of kDisplay/kTarget/kScene/kHelp turns the feature off (logged); the rest degrade (open windows may sit away from their edges).

#### Interface area (`src/core/uispace.cpp`)

**Setting:** always (reads only; used by items.count.font=sharp, network.icon=dot, target, etc.).
**What it does:** Finds the menus' interface area (left/top/right/bottom shorts) the game sets once, and reads them each use to convert between interface units and screen pixels. No game code changed. If it can't be found it logs and the text drawn over the game's windows isn't drawn.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSetup` (`uispace.cpp:16`) | the menus' setup (FUN_10163b30) storing the area | `33 DB 68 20 06 00 00 66 89 1D ?? ?? ?? ?? 66 89 1D ?? ?? ?? ?? 66 A3 ?? ?? ?? ?? 66 89 35 ?? ?? ?? ?? E8` | `operand(site, 0xA)` left, `0x11` top, `0x17` right, `0x1E` bottom (shorts' addresses); top must equal left+1 short and bottom right+1 short |

**CatsEyeXI address:** `FFXiMain.dll+0x163B30` (FUN_10163b30).
**Retail:** addresses are `??` and cross-checked for adjacency; the immediate `20 06 00 00` (0x620) is fixed, a constant of the setup function.

#### Hide UI (`src/core/hideui.cpp`)

**Setting:** `[ui] hide.key` (default F11); also used by Lua/other features through `hidden()`/`set_hidden()`.
**What it does:** Finds the menu manager and SetHideMode (FUN_1015f440) and calls it with mode 1/0 to hide and show the interface; reads the manager's hide mode (`+0xB4`) and phase (`+0x64`). No code changed here.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kPhaseCall` (`hideui.cpp:26`) | the manager's frame calling OnPhaseCtrl (FUN_1015dc90) | `8B CD E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B CD E8 ?? ?? ?? ?? 85 C0 89 44 24 10 74` | `operand(site, 8)` = the menu manager (checked to be inside the image) |
| `kSetHideMode` (`hideui.cpp:30`) | SetHideMode (FUN_1015f440) | `8A 81 B2 00 00 00 84 C0 75 22 8A 81 B3 00 00 00 84 C0 75 18 8A 44 24 04 3C 02 7D 09 88 81 B4 00 00 00 C2 04 00` | called as `__thiscall` on the manager |

**CatsEyeXI address:** `FFXiMain.dll+0x15F440` (SetHideMode); manager global `0x105ECB98` (`+0x5ECB98`, comment); OnPhaseCtrl `+0x15DC90`.
**Retail:** the patterns fix the field offsets `+0xB2/+0xB3/+0xB4` (the lock bytes and the mode) inside SetHideMode. Hard-coded manager fields: `+0x54` (current window), `+0x64` (phase), `+0xB4`; window `+0x10` (control id, `0xE` none), `+0x70`, `+0x3A` rect; world phases `0x60`, `0xD0`. Not found: logs, `hide.key` does nothing.

#### Camera with the interface hidden, and hidden cursor (`src/core/hideui.cpp`)

**Setting:** `[ui] hide.camera` (default 1), `[camera] mouse.hidecursor` (default 0).
**What it does:** `hide.camera` patches the mouse frame (FUN_10124EA0), which returns early with the interface hidden, so that it carries on: `site+0x1A` `C6 46 4D 00` -> `90 90 90 90`, and `site+0x23` `5F 5E 5B 83` -> `8B CE EB 03`. Written by `apply_camera` between frames when the setting is on, and the original bytes are written back when it is turned off. `mouse.hidecursor` hooks FFXiMain's USER32 `SetCursor` import (IAT, not a pattern) and uses the mouse object pointer read from the second pattern.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kMouseFrame` (`hideui.cpp:48`) | the mouse's frame (FUN_10124EA0) | `83 EC 1C 53 56 8B F1 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 8B CE 74 12 6A 00 C6 46 4D 00 E8 ?? ?? ?? ?? 5F 5E 5B 83 C4 1C C3` | `operand(site, 9)` must equal the manager; 4 bytes patched at `+0x1A` and `+0x23` (above) |
| `kMouseCall` (`hideui.cpp:66`) | the manager's input frame calling the mouse frame | `8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 10` | `operand(site, 2)` = the mouse object pointer global (`0x104DFBDC` in the comment, `+0x4DFBDC`); call at `+6` must resolve to kMouseFrame; `operand(site, 12)` must equal the manager |

Hard-coded mouse object fields: `+0x4D` state (5 camera drag, 3/6 scroll bars), `+4` position, `+0x58/+0x5C/+0x60` buttons held/repeat/pressed (the last three are written to zero by `clear_game_mouse_buttons`).
**CatsEyeXI address:** `FFXiMain.dll+0x124EA0` (kMouseFrame); mouse global `+0x4DFBDC`.
**Retail:** `??` over globals and call target; the patched offsets `+0x1A`/`+0x23` and the replacement `EB 03` jump distance are hard-coded to this function's layout. Original bytes are not read before patching (written back from constants `C6 46 4D 00` / `5F 5E 5B 83`), so a changed layout would be corrupted; the pattern containing exactly those bytes guards it. Not found: logs, the feature does nothing.

#### Profile window clean layout (`src/core/profilewin.cpp`)

**Setting:** `[client] profile.clean` (default 0), also `profile.title.color`, `profile.title.opacity`.
**What it does:** Finds the Profile panel's draw (CTkProfile::OnDrawPrimitive, FUN_101474b0) and, inside its first 0x3EA bytes, eleven `add` immediates that set each value's x and y. With `profile.clean` on it overwrites those immediates (1, 2 or 4 bytes each) with a one-column layout and re-points the rank's right-aligned draw call (`site+11`) at the left-aligned draw (the home point's call target). The game's original immediates and call offset are saved and written back when it is turned off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDraw` (`profilewin.cpp:76`) | the panel's draw (FUN_101474b0) | `81 EC DC 00 00 00 56 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8A 46 1C 84 C0 0F 85` | search range for the sites below (`kDrawSize` = 0x3EA); the whole block `VirtualProtect`ed and written |
| site: nation picture (`profilewin.cpp:85`) | the `add` before the nation's picture | `83 C1 12 25 FF 00 00 00 68 80 80 80 80 83 C2 37` | x at `+15` (1 byte) y at `+2` (1 byte) |
| site: race (`profilewin.cpp:86`) | race | `66 83 C1 24 50 66 83 C2 32` | x `+8` (1), y `+3` (1) |
| site: race, other way (`profilewin.cpp:87`) | race (other layout) | `66 05 24 00 66 83 C1 32` | x `+7` (1), y `+2` (2) |
| site: home point (`profilewin.cpp:88`) | home point; call at `+11` is the left-aligned draw | `66 83 C1 34 66 83 C2 5E 50 51 52 E8` | x `+7` (1), y `+3` (1) |
| site: rank (`profilewin.cpp:89`) | rank; call at `+11` rewritten to the home point's draw | `66 83 C2 44 51 66 05 40 00 52 50 E8` | x `+7` (2), y `+3` (1) |
| site: points' gauge (`profilewin.cpp:90`) | rank points' gauge | `66 83 C1 54 D9 1C 24 52 66 8B 56 14 50 66 83 C2 5C` | x `+16` (1), y `+3` (1) |
| site: mastery rank (`profilewin.cpp:91`) | mastery rank | `66 83 C1 6A 50 66 83 C2 6E` | x `+8` (1), y `+3` (1) |
| site: mentor's flag (`profilewin.cpp:92`) | mentor flag | `83 C1 68 25 FF 00 00 00 68 80 80 80 80 81 C2 DA 00 00 00` | x `+15` (4), y `+2` (1) |
| site: no flag text (`profilewin.cpp:93`) | mentor text | `66 83 C2 68 51 66 05 DA 00` | x `+7` (2), y `+3` (1) |
| site: gladiator rank (`profilewin.cpp:94`) | gladiator rank | `66 05 91 00 52 66 81 C1 8C 00` | x `+8` (2), y `+2` (2) |
| site: prestige (`profilewin.cpp:95`) | prestige | `66 81 C2 A1 00 51 66 05 5A 00` | x `+8` (2), y `+3` (2) |

(The columns after the pattern in each site row are in the source's field order: x offset, x size, y offset, y size.)
**CatsEyeXI address:** `FFXiMain.dll+0x1474B0` (FUN_101474b0), read live 2026-11-08.
**Retail:** no `??` in any site pattern: each includes its own immediates (e.g. `0x12`, `0x37`), so a changed coordinate in retail would make that site not match and turn `profile.clean` off after logging "a value's place (N) wasn't found" (labels still laid out). The immediates are also what is patched, which is fragile when retail moves layout. The site search is bounded to the function's first 0x3EA bytes, a fixed size. Layout name `menu    profile ` and `Word`/row numbers are for the CatsEyeXI panel picture (252 x 178).

#### Craft Skills brackets (`src/core/profilewin.cpp`)

**Setting:** `[client] craft.brackets` (default 1).
**What it does:** Finds the craft panel's `push "(%d)"` and rewrites its 4-byte operand to point at VanaCore's `"%d"` when brackets are turned off, and back to the game's address when on. Checked that the pushed string really is `(%d)` before it is taken.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kCraftFormat` (`profilewin.cpp:103`) | the craft panel's sprintf of the level (draw at 0x10148290, vtable slot 4) | `C1 EB 05 81 E3 FF 03 00 00 8D 54 24 38 53 68 ?? ?? ?? ?? 52 E8` | 4-byte operand at `site+15` read as the format string pointer, compared to `"(%d)"`, and rewritten as above |

**CatsEyeXI address:** panel draw `FFXiMain.dll+0x148290`; the `"(%d)"` string `+0x36F898` (as stated in the comment); the push itself not recorded.
**Retail:** `??` over the format pointer; the shift and mask `C1 EB 05 / 81 E3 FF 03 00 00` and stack offset `38` are fixed. Not found or string mismatch: logs, does nothing.

#### Item details pictures and type row (`src/core/tipicons.cpp`)

**Setting:** `[client] tips.icon.x/.y/.size`, `tips.badges.x/.y/.size`, `tips.type` (game, simple, type). Hooks are installed on the first frame whatever the settings.
**What it does:** Finds the item details window's draw (CTkItemInfo, 0x1013CA00) and rewrites a series of `call rel32` sites inside it to ours with `asm_text::branch`: the item picture, stack count, badges draw, each of the four badge calls inside the badges function (+0x16D/+0x1A7/+0x1E2/+0x221), and the four type-row getter and print call pairs. Each site is only taken when the bytes just before it match. The redirects are not undone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDetailsDraw` (`tipicons.cpp:23`) | the item details draw (0x1013CA00) | `81 EC A4 00 00 00 56 8B F1 8B 4E 1C 85 C9 0F 84` | calls at `+0x6D` (DrawIcon 0x1015A2D0, before: `52 68 00 00 80 3F 55 53`), `+0xD1` (count, `83 C3 22 55 53`), `+0x1A0` (badges, `8B CE 52 50`), type row getters/prints at `+0x1D8/+0x1F6`, `+0x217/+0x235`, `+0x262/+0x280`, `+0x2A1/+0x2BF` (preceded by `66 8B 10 52` or `66 8B 00 50`; prints by `55 50 51` or `55 51 52`), badges' draws at `badges+0x16D/+0x1A7/+0x1E2/+0x221` |
| `kScaledDraw` (`tipicons.cpp:41`) | YmMenuShape::Draw with a scale (0x1011F880), first 5 bytes `??` | `?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8` | called to draw scaled badges; without it `tips.badges.size` does nothing |

**CatsEyeXI address:** `FFXiMain.dll+0x13CA00` (kDetailsDraw), `+0x15A2D0` (DrawIcon), `+0x11F880` (scaled draw), `+0x13C790` (badges), `+0x11F760` (plain shape draw), the count draw `0x101C82B0` (`+0x1C82B0`), type getters `+0x159800`, `+0x159A20`, `+0x159B30`, print `+0x129A40`.
**Retail:** `kDetailsDraw` has no `??` in its first 16 bytes: the `A4 00 00 00` stack size and `[esi+1Ch]` would change with the function. Every call offset inside is hard-coded (seen live 2026-11-05) and guarded only by the preceding bytes (some contain `0x22` for the count position, constants `kCountX/Y`). The icon is 32 x 32 and badges 16 x 16, assumed.

#### Game window reading (`src/core/gamewin.cpp`)

**Setting:** Lua API `vana.gameui.window`, `vana.gameui.watch`, `vana.gameui.press`.
**What it does:** Finds the menu manager and three game functions it calls from VanaCore to read a window's text and press keys / move the cursor in a window. Nothing in game code is changed here (quad/letter capture hooks come from itemslot.cpp and chatfont.cpp, not scanned here). It logs `found`/`NOT found` for each once.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kManager` (`gamewin.cpp:27`) | the menu manager (same pattern as statuswindow's `kWindows`) | `32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8` | `operand(site, 3)` = manager |
| `kToCell` (`gamewin.cpp:59`) | the text-to-font-sheet-cell conversion (0x1011CA80, `__cdecl`) | `56 8B 74 24 08 57 8B 3E 57 E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 0F 8B 16 33 C0 42 89 16 8A 07 5F 83 E8 20 5E C3` | called to build the cell-to-Shift-JIS table |
| `kSetCursor` (`gamewin.cpp:60`) | SetCursol (0x101189B0, `__thiscall` row, 0; `ret 8`) | `83 EC 08 56 57 8B 7C 24 14 8B F1 66 85 FF 7F 05 BF 01 00 00 00 66 8B 46 58 66 3B F8 7E 02 8B F8 66 8B 46 4C` | called as `g_set_cursor(window, row, 0)` |
| `kKeyInput` (`gamewin.cpp:61`) | a window's key input (0x10117E60, `__thiscall` key; `ret 4`) | `51 55 56 8B F1 57 83 7E 10 0E 75 09 5F 5E 33 C0 5D 59 C2 04 00` | called as `g_key_input(window, key)` |

**CatsEyeXI address:** `FFXiMain.dll+0x11CA80` (kToCell), `+0x1189B0` (kSetCursor), `+0x117E60` (kKeyInput); kManager not recorded. (Comment says "each one place in the 2025-12-26 FFXiMain.dll".)
**Retail:** no `??` in the last three: their stack sizes and field offsets (`[esi+10h]`, `[ebp+58h]`, `[esi+4Ch]`) are baked in and would not match if those moved. Hard-coded window fields: layout `+4`, state `+0x10`, pieces `+0x14`, rect `+0x3A`, cursor `+0x4C`, count `+0x58`, down `+0xB8`, wait `+0x56`, takes-keys `+0x5C`; layout name `+0x46`; piece shape `+4`, state `+0x10`, place `+0x2A`, id `+0x3A`, shown `+0x3C`; quad name `+0x32`, blend `+0x2E`, corners `+0x50` (stride `0x1C`), alpha `+0x13`, own x/y `+0xC0/+0xC8`; the font sheet's cell count `0x2200`.

#### Game UI input dispatch hook (`src/core/gameui.cpp`)

**Setting:** Lua API `vana.gameui.on` and `catch` (installed when an addon adds a trigger).
**What it does:** Rewrites 5 bytes at the tail of the window input function (FUN_10117e60, around `0x101180C6` on 2026-10-25) where the window hands an event and its cursor item to its handler's slot 6: `mov eax,[ecx]; call [eax+18h]` becomes `call dispatch_stub`. The stub asks `dispatch()` first (addons may swallow the event) and otherwise jumps to the handler's slot 6. Installed once on the first trigger; not undone. A miss is retried at each trigger; a `VirtualProtect` failure is logged.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDispatchPattern` (`gameui.cpp:634`) | the tail of the window input where events go to the handler | `8B 4E 0C 66 8B 56 4C 52 55 8B 01 FF 50 18 8B 44 24 0C 5F 5E 5D` | 5 bytes at `site = match+9` (`8B 01 FF 50 18`) become `E8 rel32` to `dispatch_stub`; stub's convention assumes `ecx` handler, `esi` window, event and item on the stack |

**CatsEyeXI address:** inside `FFXiMain.dll+0x117E60` (the comment gives `0x101180C6` for the tail on 2026-10-25); the log prints `site - base` at run time.
**Retail:** no `??`: `[esi+0Ch]` (handler), `[esi+4Ch]` (cursor item), vtable slot `0x18` (slot 6) and `[esp+0Ch]` are baked into the pattern, and the stub depends on `esi` holding the window and `ecx` the handler at that point. The original bytes are not saved or checked beyond the pattern. Not found: no log line of gameui's own (it may simply not be unpacked yet), but `scan::find` still logs the miss once (`Signatures: nothing in FFXiMain.dll matches ...`).

### Party, buttons, network, login and compat

#### Buff icons: scale, place, columns (`src/core/buffwin.cpp`)

**Setting:** `[client] buffs.scale`, `buffs.move`/`buffs.x`/`buffs.y`, `buffs.columns`.
**What it does:** Finds the status icons' window in the menu manager's list (layout name `menu    buff    ` at layout+0x46) and moves it with the game's SetLocate (`buffs.move`) and rewrites its buttons' rects (`buffs.columns`) once a frame. For `buffs.scale` it rewrites four `call` offsets (E8 rel32) inside the game's icon, timer and window draws to its own hooks, once the scale is first not 100. The call rewrites are never put back: with scale 1 the hooks pass the game's values straight through. The manager and SetLocate come from `menuplace.h` (below).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kIconCallback` (`buffwin.cpp:59`) | the status list's icon callback (FUN_10226630) | `55 56 57 E8 ?? ?? ?? ?? 8B 6C 24 10 8D 04 40 8B CD 8D 34 80 8B 44 24 20 50 C1 E6 02 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 53 68 C3 00 00 00 BB 80 80 80 80` | scope for `kIconCallBefore` (next 0x180 bytes) |
| `kIconCallBefore` (`buffwin.cpp:61`) | the bytes just before the icon's shape-draw call (FUN_100268E0) | `6A 00 53 50 8B 44 24 28 41 40 51 50 8B CA` | call at match + 14 (`kIconCallBeforeSize`) must be E8; its offset rewritten to `shape_hook`; target kept as the game's draw |
| `kTimerCallback` (`buffwin.cpp:62`) | the timer callback (FUN_10226740) | `83 EC 10 53 56 57 68 C3 00 00 00 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 7C 24 30 8B 5C 24 20 8D 04 40 57 8B CB 8D 34 80` | scope for `kTimerCallBefore` (0x180 bytes) |
| `kTimerCallBefore` (`buffwin.cpp:64`) | the bytes before the timer's text-draw call (FUN_101C7F10) | `83 C1 14 83 C2 0E 6A 01 68 9A 99 59 3F 68 9A 99 59 3F 68 80 80 80 80 50 51 52` | call at match + 26 rewritten to `text_hook` |
| `kButtonShape` (`buffwin.cpp:78`) | the window draw's (FUN_101173D0) button call to the scaled shape draw (FUN_1011F880) | `6A 00 6A 00 6A 00 53 50 51 0F BF 44 24 38 0F BF D7 52 50 8B 4C 24 3C E8` | call at match + 23 (`kButtonShapeCall`) rewritten to `menu_shape_hook` |
| `kCursorShape` (`buffwin.cpp:79`) | the same draw's cursor call | `6A 00 6A 00 6A 01 0F BF CF 50 68 00 00 80 3F 0F BF D3 68 00 00 80 3F 51 52 8B CE E8` | call at match + 27 (`kCursorShapeCall`) rewritten; both calls must go to the same function or neither is hooked |
| `kHelp` (`menuplace.h:21`, used by buffwin.cpp) | the help line window's code (FUN_101182A0) | `53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8` | `kManagerAt` = 7: the 32-bit `mov ecx,<imm>` is the CTkMenuMng global's address (read directly, not via `scan::operand`) |
| `kSetLocate` (`menuplace.h:23`, used by buffwin.cpp) | window SetLocate (FUN_10118D10, `__thiscall` window, x, y, ret 8) | `53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | function = match - 8; checked `start[0]` is `83` or `E9` (a detour already there) and `start[0x6C] == E8` |

(The two `menuplace.h` rows are not counted in the totals below; that header is shared with `castbar.cpp` and `partyrows.cpp`.)

**CatsEyeXI address:** `FFXiMain.dll+0x226630` (icon callback), `+0x226740` (timer callback), `+0x1173D0` (window draw holding the two shape calls), `+0x11F880` (shape draw), `+0x1182A0` (help line), `+0x118D10` (SetLocate). Call sites inside them: not recorded.
**Retail:** all six patterns are plain bytes, no `??` over fields except call/jump displacements. The two `*CallBefore` ones are found inside the callback by `scan::find(begin, size, pattern)`. Not found: that part is skipped and logged ("Buff icons: ... wasn't found"), the setting does nothing. Fragile: fixed distances from a match (`+14`, `+26`, `+23`, `+27`, SetLocate `-8`, `+0x6C`); hard-coded structure offsets: manager list +0, area +0x7C; window +4 layout, +0x14 parts, +0x3A frame, +0x42 opened, +0x70 deleting, layout name +0x46; button rect +0x32, id +0x3A, kind +0; shape box +0x20..+0x26. Shape fields are not in docs/retail-signatures.md's shifted ranges.

#### Button style, selected-button nudge and flash (`src/core/buttons.cpp`)

**Setting:** `[client] buttons.*`, `mainmenu.buttons.*`, `action.buttons.*` (`.style`, `.selected.nudge`, `.selected.flash`, colours, `.text.*`), plus True Font's `*.font`, `chatfont` labels and `sliders` (they turn the shape hooks on).
**What it does:** Two detours (via `imports::detour`) of the game's shape drawing (the scaled one FUN_1011F880, 5 bytes, and the plain one FUN_1011F760, 7 bytes) let VanaCore recolour, restyle or redraw menu buttons. Separately it patches single bytes in the window draw and the button-place function: one-byte `dec`/`inc` instructions become `90`, the two `add reg,2` immediates become `0`, and the flash's `0F 8E` (jle) becomes `90 E9` (nop; jmp). The patches are written when `selected.nudge=0` / `selected.flash=0` and written back to the game's own bytes when it is on again (the main menu's own nudge/flash is written at its draw and back after it each frame). The detours are not removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kShapeDraw` (`buttons.cpp:43`) | YmMenuShape::Draw with scale (FUN_1011F880) | `?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8` | first 5 bytes left out of the pattern (statuswindow.cpp finds the same function) and checked as `83 EC 0C 53 55`, then 5-byte detour |
| `kPlainDraw` (`buttons.cpp:50`) | YmMenuShape::Draw plain (FUN_1011F760) | `?? ?? ?? ?? ?? ?? ?? 8D 59 04 57 8B CB E8 ?? ?? ?? ?? 89 44 24 10 8B 44 24 28 33 FF 85 C0 7E 13` | first 7 bytes checked as `51 53 55 56 8B 71 14`, then 7-byte detour (equipstats.cpp finds the same function) |
| `kSelected` (`buttons.cpp:77`) | the window draw's loop drawing the cursor's button (inside FUN_101173D0) | `66 8B BD B8 00 00 00 8B 56 04 66 8B 46 2A 66 03 7E 2C 89 75 08 8B 4E 1C 89 54 24 1C 89 4C 24 14 8B 56 20 48 89 54 24 18 89 44 24 20 8B 46 18 4F 83 F8 0F BB 20 50 80 80 75 05 C6 44 24 13 01 66 8B 45 5E 66 85 C0 0F 8E ?? ?? ?? ?? 8B 4C 24 20 83 C7 02 50 8B 44 24 20 83 C1 02` | bytes at +0x23 and +0x2F (`dec eax`, `dec edi`) -> `90`; +0x52, +0x5A (`add edi,2`, `add ecx,2` immediates) -> `00`; +0x46,+0x47 (`0F 8E`) -> `90 E9` for no flash |
| `kRaised` (`buttons.cpp:83`) | the raised-piece draw of the cursor's button | `66 8B BD B8 00 00 00 66 8B 46 2A 66 03 7E 2C 8B 56 04 40 BB 20 50 80 80 89 75 08 89 44 24 20 47` | +0x12, +0x1F (`inc eax`, `inc edi`) -> `90` |
| `kGreyed` (`buttons.cpp:85`) | the greyed cursor's button draw | `66 8B BD B8 00 00 00 8B 4E 04 66 8B 46 2A 66 03 7E 2C 89 75 08 8B 56 1C 89 4C 24 1C 89 54 24 14 8B 4E 20 40 BB 10 28 40 40 89 4C 24 18 89 44 24 20 47` | +0x23, +0x31 (`inc`) -> `90` |
| `kPlace` (`buttons.cpp:97`) | GetButtonLocate (FUN_10118500), matched past its first 0x11 bytes (chat.cpp detours its start) | `0F BF 48 2A 0F BF 50 2C 53 57 66 8B 78 3A 66 3B 7E 4C 75 30 8A 5E 63 84 DB 74 29 8A 5C 24 18 84 DB 74 21 83 78 10 05 74 19 8A 5C 24 1C 49 4A 84 DB 74 11 66 83 7E 5E 00 7E 0A 83 C1 02 83 C2 02 EB 02 41 42` | bytes +0x2D, +0x2E (`dec ecx`, `dec edx`), +0x42, +0x43 (`inc ecx`, `inc edx`) -> `90`; +0x3C, +0x3F (`add ecx,2`, `add edx,2` immediates) -> `00` |

**CatsEyeXI address:** `FFXiMain.dll+0x11F880` (kShapeDraw), `+0x11F760` (kPlainDraw), `+0x118500` (kPlace, function start; match is past +0x11), window draw `+0x1173D0` (kSelected/kRaised/kGreyed sit inside it; individual sites not recorded). The log line "the selected button's drawing found at +0x%X" prints the match, not recorded in source.
**Retail:** Only the two `kShapeDraw`/`kPlainDraw` patterns use `??` (for the unmatched prologue); all the nudge patterns are full byte sequences with one `??` run (a jump displacement in `kSelected`). Missing: `find_patches` logs and leaves nudge/flash alone; missing detours log "buttons.style ... do nothing". Fragile: nine fixed offsets from `kSelected`/`kPlace` and the three other patterns (all listed above), which only the full function comparison would catch; structure offsets: window layout +4, name +0x46, shape list +4, count +0x14, node quad +0x10, quad name +0x32, texture +0x4C, corners +0x50 (0x1C apart), own x/y +0xC0/+0xC8, blend +0x2E (quad size 0xD0); window +0x5E (flash), +0xB8.

#### Button spacing (`src/core/buttonspace.cpp`)

**Setting:** `[client] buttons.spacing` (and the main menu and action menu sets, `*.spacing`; only with the flat style).
**What it does:** Calls the game's SetLocate to re-place a window after moving its parts' rects down and making its frame taller (by writing shorts into the game's window objects). Nothing in the game's code is patched; nothing needs undoing beyond the rects, which are restored from the copy it keeps when spacing returns to 0.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSetLocate` (`buttonspace.cpp:51`) | SetLocate (FUN_10118D10), same as `menuplace.h` | `53 55 8B 6C 24 18 56 8B F1 57 33 FF 8D 5E 14 66 89 6E 52 8B CB 66 89 46 54` | function = match - 8 (`kSetLocatePrologue`), called as `__thiscall(window, x, y)` |

**CatsEyeXI address:** `FFXiMain.dll+0x118D10` (SetLocate); `FUN_10117B40` (SetResourceData, +0x117B40), `FUN_101196D0` and `FUN_1011A1A0` are named in comments only.
**Retail:** exact bytes, no `??`. Not found: logs and the setting does nothing. This file does not check the `83`/`E9` start byte or `+0x6C` that `menuplace.h` does. Hard-coded structure offsets: window layout +4, pieces +0x14, flags +0x34, frame +0x3A, layout name +0x46; part kind +0, shape +4, rect +0x32 (top at +0x34, bottom at +0x38), height +0x30, frame height +0x24.

#### Alliance lists and the macro bar; party list hide and background (`src/core/alliancelist.cpp`)

**Setting:** `[client] alliance.macrobar` (default on), `party.hide`, `alliance1.hide`, `alliance2.hide`, `party.background`, `alliance1.background`, `alliance2.background`.
**What it does:** In the party list's frame update, the first of four "is a window in the way" checks is a `call IsAreaWindow`; its call offset is rewritten to `area_window`, which zeroes the macro palettes' area words while the game's walk runs, and the two `75 xx` jumps after the palette checks are overwritten with `90 90`. Written when `alliance.macrobar=1`, written back (call offset and two jumps' original two bytes) when 0. The hide/background keys write no code: they skip the window draw and clear `window+0x69` for the named layouts.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kChecks` (`alliancelist.cpp:27`) | the party list's checks before it keeps the alliance lists (YkWndParty update) | `6A 00 6A 00 68 81 00 00 00 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 6A 01 E8` | call at +14 (IsAreaWindow) rewritten; palette pointers' globals read at +40 and +55; the `75` jumps at +51 and +66 nopped (checked to be `75` and all addresses inside the image) |

**CatsEyeXI address:** `FFXiMain.dll+0x20250A` (party list frame update), `+0x15DA40` (IsAreaWindow), `+0x62F708` (CTkParty global, per the comment), `+0x2028F0` (FUN_102028F0, party list draw, gauge mode at owner+0x40); window draw `+0x1173D0`.
**Retail:** `??` over the manager and palette globals and the rel32s and the short jump sizes; the `81h` area mask, `6A 01` and the `84 C0 75` shapes are fixed. A shape mismatch or a non-`75` jump logs "isn't as expected" and the feature stays off. Fragile: the offsets 14/40/51/55/66 into the match; hard-coded structure fields: window object ctrl +8, ctrl area word +0x38, window layout +4 / name +0x46, owner +0xC, owner gauge byte +0x40, window pieces flag +0x69.

#### Party list names and numbers: colours and upper case (`src/core/partylist.cpp`)

**Setting:** `[client] party.names.upper`, `party.hp.color`, `party.mp.color`, `party.tp.color` and the same four with `alliance.` for the alliance lists.
**What it does:** In the party and alliance rows' draw code, the call to the HP colour function, the name draw (in zone and away), the TP number draw and the pet row's name and TP number calls are rewritten to VanaCore's (E8 offset rewritten); the MP number's `push imm32` colour is overwritten with the setting's colour. Each is written only while its key is set and restored to the game's own value when it is unset (settings change on `on_frame`).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kHpColour[0]` (`partylist.cpp:39`) | the party row's HP colour call (FUN_10200AD0) | `66 83 7E 34 00 0F 85 ?? ?? ?? ?? 56 8B CD E8 ?? ?? ?? ?? 8B 5C 24 3C 8B 7C 24 38` | call at +14 rewritten to `party_hp` |
| `kHpColour[1]` (`partylist.cpp:40`) | alliance row's | `66 83 7E 34 00 0F 85 ?? ?? ?? ?? 56 8B CD E8 ?? ?? ?? ?? 8B 7C 24 40 8B 5C 24 3C` | call at +14 rewritten; both lists' calls must go to the same function |
| `kName[0]` (`partylist.cpp:42`) | party name draw (FUN_10200AA0) for a member in this zone | `8D 56 06 68 ?? ?? ?? ?? 52 53 8B CD 57 E8` | call at +13 rewritten to `party_name` |
| `kName[1]` (`partylist.cpp:42`) | alliance name draw | `8D 46 06 8B CD 68 ?? ?? ?? ?? 50 57 53 E8` | call at +13 |
| `kAway[0]` (`partylist.cpp:44`) | party name draw, member in another zone (grey) | `8D 4E 06 68 ?? ?? ?? ?? 51 53 57 8B CD E8` | call at +13 |
| `kAway[1]` (`partylist.cpp:44`) | alliance, other zone | `8D 46 06 68 ?? ?? ?? ?? 50 57 53 8B CD E8` | call at +13 |
| `kMpColour[0]` (`partylist.cpp:46`) | party MP number's colour push | `8D 43 0A 68 ?? ?? ?? ?? 52 50 8D 47 62 50 E8` | `68` at +3, its imm32 overwritten with the colour (original kept) |
| `kMpColour[1]` (`partylist.cpp:46`) | alliance MP colour push | `68 ?? ?? ?? ?? 8D 4F 08 50 8D 53 68 51 52 E8` | `68` at +0 |
| `kTp[0]` (`partylist.cpp:48`) | party TP number call (FUN_10201020) | `8D 57 30 50 52 8B CD E8` | call at +7 rewritten to `party_tp` |
| `kTp[1]` (`partylist.cpp:48`) | alliance TP number call | `8D 53 30 51 52 8B CD E8` | call at +7 |
| `kPetTp` (`partylist.cpp:50`) | the pet row's TP number call | `8D 4B 30 50 51 8B CF E8` | call at +7; must go to the same function as the rows' TP |
| `kPetName` (`partylist.cpp:51`) | the pet row's name call (FUN_101C84B0) | `6A 66 E8 ?? ?? ?? ?? 83 C4 14 B0 01 5B 5F 5E C2 08 00` | call at +2 rewritten to `pet_name` |
| `kTpNumber` (`partylist.cpp:54`) | the TP number function itself (FUN_10201020) | `8B 44 24 0C 83 EC 20 3D E8 03 00 00 56 BE ?? ?? ?? ?? 7C 05 BE ?? ?? ?? ?? 50` | must be the address the TP calls go to; `operand(at, 0x0E)` = the grey colour, `operand(at, 0x15)` = the blue; string-draw call at +0x45 gives the game's string draw. A mismatch drops all TP hooks |

**CatsEyeXI address:** party row draw `FFXiMain.dll+0x200C50`, alliance row `+0x200E40`, HP colour `+0x200AD0`, name `+0x200AA0`, MP string draw `+0x1C82B0`, TP number `+0x201020`, pet name `+0x1C84B0`. Individual call sites: not recorded.
**Retail:** all calls found by short unique byte runs, with `??` only where a colour or displacement is. Nothing found: that call is skipped; a log line gives how many of the 4+1 name, 2 HP, 2 MP, 2+1 TP sites were found. The 16 call/push offsets are fixed from each match; the colours (`0x80606060`, `0x802060D0`) are read back with `operand()`. Hard-coded: member HP percent at member+0x30, member zone word at +0x34 (in the pattern), member name at +6, in the 0x7C-byte member record.

#### Party member TP bar (`src/core/tpbar.cpp`)

**Setting:** `[client] party.tpbar`, `party.tp` (also calls `partylist::install` for `party.names.upper` and colours).
**What it does:** The party row's call to the MP bar draw (YkDrawMPBar2) is rewritten to `mp_bar`, which for members with no spells draws the game's own bar with the TP percent and blue corners; with `party.tp` it also draws a second bar and the row's MP number call is rewritten to `draw_number` (swapping TP into the member's MP field for the call). `party.tp` additionally patches the `je` displacement in the TP number option check to 0 (so the number always shows). Written while either setting is on, put back to the game's offsets/displacement when off.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kRow` (`tpbar.cpp:35`) | the party row's call to the MP bar | `8A 56 31 8D 4E 79 51 8D 43 11 52 8D 4F 31 50 51 E8 ?? ?? ?? ?? 83 C4 20 66 83 7E 50 00` | call at +16 rewritten (`kCall`); target must start `6A 01 E8` (`kBarStart`) and its +3 call gives the menu resource getter |
| `kNumber` (`tpbar.cpp:43`) | the row's MP number formatting and string draw, searched within 0x100 bytes of the row | `8B 46 28 8D 4C 24 ?? 50 68 ?? ?? ?? ?? 51 E8 ?? ?? ?? ?? 68 00 00 80 3F 68 00 00 80 3F 6A 00 8D 54 24 ?? 8D 43 0A 68 ?? ?? ?? ?? 52 50 8D 47 62 50 E8 ?? ?? ?? ??` | call at +49 rewritten to `draw_number` |
| `kTpNumber` (`tpbar.cpp:53`) | the "show TP number" option check (option 0xAE) within 0x100 bytes | `6A 00 68 AE 00 00 00 E8 ?? ?? ?? ?? 83 C4 30 84 C0 74 ?? 56 E8 ?? ?? ?? ?? 83 C4 04 84 C0 75 ?? 8B 4E 2C` | `74` at +17 (`kOptionJump`) checked, its displacement byte (+18) written 0 for `party.tp` |
| `kMpBar` (`tpbar.cpp:65`) | the MP bar function body, matched at the MP bar's start | `6A 01 E8 ?? ?? ?? ?? 8B 00 8B 4C 24 14 8B 54 24 10 51 8B 88 9C 00 00 00 52 8B 90 88 01 00 00 8B 44 24 14 51 8B 4C 24 14 52 50 51 E8` | call at +0x2B = the bar body; table offsets 0x9C (frame) and 0x188 (fill) are in the pattern |
| `kBody` (`tpbar.cpp:70`) | the bar body, matched at its start | `8B 44 24 18 8B 4C 24 14 8B 54 24 10 68 00 00 80 3F 68 00 00 80 3F 68 00 00 80 3F 68 00 00 80 3F 50 8B 44 24 20 51 8B 4C 24 20 52 8B 54 24 20 50 51 52 E8` | call at +0x32 = the bar draw with scales (`g_draw`, called directly, not hooked) |

**CatsEyeXI address:** party row `FFXiMain.dll+0x200C50`, YkDrawMPBar2 `+0x1C9910`, bar under it `+0x1C9660`. Call sites: not recorded (log prints "MP bar call found at +0x%X").
**Retail:** `kRow`, `kMpBar`, `kBody` are fixed bytes except the call; `kNumber` has `??` over stack slots and the format string address. Not found: the whole feature or just the number is dropped (distinct log lines). Fragile: offsets +16, +49, +17/+18, +0x2B, +0x32; structure offsets: member +0x28 MP, +0x2C TP, +0x31 and +0x79 bar state, +0x6D main job, +0x6F sub job, +0x70 sub level, member size 0x7C; shape list +4, node value +0x10, shape box +0x20/+0x22, quad name +0x32, corners +0x50 (0x1C apart), colour +0x10; table +0x188 fill, +0x9C frame.

#### Party menu: Find Member opens Party Finder (`src/core/partymenu.cpp`)

**Setting:** `[client] partyfinder.findmember` (default off), or any addon calling `vana.gameui.replace('party_menu.find_member', fn)`.
**What it does:** Finds the Party list handler's "item chosen" function (slot 17 of its vtable-like table), then finds that table in the image's data by scanning for the function's address as little-endian bytes, checks slot 6 is the expected input function by its bytes, and overwrites slot 17 with `party_chosen`. Hooked once when first wanted; never restored (`party_chosen` falls through to the game's function when off).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kChosen` (`partymenu.cpp:26`) | the Party list handler's item-chosen function (FUN_102040C0, `__thiscall` ret 4) | `0F BF 44 24 04 48 83 F8 06 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ?? 8A 51 2C 84 D2 0F 94 C0 50 E8` | jump table address read at +18 (`kJumpTableAt`); case 2 (`jumps[1]`) must start `83 79 18 01 75 08 E8`; then a runtime-built pattern of the function's address (4 bytes, up to 16 matches, `scan::find(base, size, ..., n)`) finds the table slot; slot - 17 slots = table, `table[6]` must start with the `kInput` 21-byte sequence; slot 17 overwritten with `PAGE_READWRITE` store |

Fixed byte checks beyond the pattern: `kFindMemberCase` = `83 79 18 01 75 08 E8`; `kInput` = `66 83 7C 24 04 05 75 0A 8B 54 24 08 8B 01 52 FF 50 44 C2 08 00`.

**CatsEyeXI address:** handler global `FFXiMain.dll+0x62F718` (2025-12-26 per the comment); slot 6 input `FUN_10203F80` = `+0x203F80`; slot 17 `FUN_102040C0` = `+0x2040C0`. The table's own address: not recorded.
**Retail:** `??` over the jump-table address and case displacement only; slot indices 6 and 17 and element id 2 are fixed. Any check failing logs and the setting does nothing. Fragile: `+18`; slot numbers 6 and 17; `[ecx+0x2C]`, `[ecx+0x18]` fields inside the checked case code.

#### Magic list A-Z order (`src/core/spelllist.cpp`)

**Setting:** `[client] spells.autosort` (default off).
**What it does:** KaMenuMagicSel::Create calls the "add newly learned spells to the order" function; that call's offset is rewritten to `merge_hook`, which runs the game's function and then sorts the spell ids in place by the game's spell names. Hooked once on first need; never restored (the hook does nothing when the setting is off).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kCreate` (`spelllist.cpp:25`) | KaMenuMagicSel::Create (FUN_1022AA60) | `83 EC 28 53 57 8B F9 89 7C 24 1C C7 47 6C 00 00 00 00 8B 1D ?? ?? ?? ?? 85 DB 89 5C 24 24 0F 84 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 55 56 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 6A 0C 6A 00 8B CF 8B A8 ?? ?? ?? ?? 55 68` | call at +0x2C (`kMergeCall`) is the adding (FUN_1022A6F0) and is rewritten; the row draw's address is the imm32 at +0x44 (`kDrawRowAt`); `kIndexAt` 0x26 is declared but unused |
| `kMerge` (`spelllist.cpp:31`) | the adding function, checked at its start | `56 8B F1 E8 ?? ?? ?? ?? 85 C0 74 ?? 8B 8E ?? ?? ?? ?? 6A 00 68 ?? ?? ?? ?? 8D 96 ?? ?? ?? ?? 51 52 56 50 E8` | must match exactly at the call's target (0x40 bytes); read again each time a list is made: `count_at` = operand at +0xE, `most` at +0x15, `ids_at` at +0x1B (CatsEyeXI's cexislots.dll rewrites them) |
| `kName` (`spelllist.cpp:38`) | the row drawing's lookup of the spell name (FUN_1022B520) | `56 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 8B 5C 24 24 8B 74 24 28 50 56 8D 53 12 52` | searched 0x100 bytes into the row draw; call +1 = the names table getter, call +8 = the name function |

**CatsEyeXI address:** `FFXiMain.dll+0x22AA60` (Create), `+0x22A6F0` (adding), `+0x22B520` (row draw). Hooked call site: logged at runtime ("hooked at +0x%X"), not recorded in source. KaMagicIndex layout in CatsEyeXI's enlarged form: ids +0x200, count +0x2200 (game's own: +0x80, +0x880), per the comment.
**Retail:** displacements and counts are `??` and read back from the code each time (the index layout is not hard-coded). Not found: logs the three booleans and the setting does nothing. Fragile: +0x2C, +0x44, +1, +8.

#### Fishing: D-pad fights the fish (`src/core/fishing.cpp`)

**Setting:** `[input] fishing.dpad` (default on).
**What it does:** The fish fight reads the left/right direction through a function that calls the game's input-actions function; that one call is rewritten to `direction()`, which asks the menus' left/right (the D-pad) when the movement axis reads 0. Patched once, on the first frame the setting is on; not restored (the hook returns the game's answer when off).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kFight` (`fishing.cpp:28`) | FUN_10089C50(0x3F, 4), the fight's direction read | `8B 4C 24 08 33 C0 83 F9 04 75 ?? B8 ?? ?? ?? ?? 50 8B 44 24 08 51 50 E8 ?? ?? ?? ?? 83 C4 0C C3 83 F9 05 75 ?? B8 ?? ?? ?? ?? 50 8B 44 24 08 51 50 E8` | call at +0x17 goes to FUN_10089C90 (the latch function); that function must start `8B 44 24 08 8B 4C 24 04 50 51 E8` and, 5 bytes after its call at +0x0A, `8B 54 24 14 83 C4 08 8A 0A`; its call at +0x0A (to the actions function) is the one rewritten |
| `kActions` (`fishing.cpp:41`) | the game's input actions function (FUN_10123570) | `51 8B 44 24 0C 8B 15 ?? ?? ?? ?? 33 C9 56 66 8B 0C 45 ?? ?? ?? ?? 57 85 D1 75 0A D9 05 ?? ?? ?? ?? 5F 5E 59 C3 8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8A 91 ?? ?? ?? ?? 84 D2` | must match at the call target (0x37 bytes); operands read back: context mask global `operand(at, 0x07)`, masks table `+0x12`, chat object global `+0x27`, chat typing byte offset `+0x31` |
| `kMenuX` (`fishing.cpp:49`) | action 9's D-pad source (FUN_10120920) | `83 EC 08 A1 ?? ?? ?? ?? 33 D2 8B 88 20 02 00 00 8B 80 24 02 00 00 89 44 24 04 8B C1 83 E0 10 0B C2 74 ?? D9 05 ?? ?? ?? ?? 83 C4 08 C3 83 E1 20 33 C0 0B C8 74` | called directly as `float __cdecl()`; the pad's function bits at `[pad]+0x220` are inside the pattern |

**CatsEyeXI address:** `FUN_10089660` (fight, `+0x89660`), `FUN_10089C50` (`+0x89C50`), `FUN_10089C90` (`+0x89C90`), `FUN_10123570` (`+0x123570`), `FUN_10120920` (`+0x120920`). The patched call's address: logged at runtime, not recorded in source.
**Retail:** `??` over globals and tables, read back where used; fixed: the action index 4/5, pad bits `+0x220/+0x224` (in `kMenuX`), `0x10`/`0x20` masks. Any failure: logs "fish fight's direction read wasn't found" and the feature does nothing. Fragile: `+0x17`, `+0x0A`, `+0x37` size, the four operand offsets; the pad structure fields.

#### Network window text and layout (`src/core/netstat.cpp`)

**Setting:** `[client] network.*` (icon, layout, percent, sent, received, simple, hide, panel, background, text, x, y, dot.size); hooked once any of them is set to a custom value.
**What it does:** Detours (via `imports::detour`, 8 bytes) the game's Network window contents draw (YkNetData::Draw) with `draw_hook`, which draws the numbers or a line itself, and calls the game's draw through the trampoline when the layout is the game's. It also reads the network object's stats and, for `network.simple`, its silence timer. Hook is not removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kNetDraw` (`netstat.cpp:32`) | YkNetData::Draw (FUN_101FFA60, `__thiscall` ret 8) | `83 EC 10 A1 ?? ?? ?? ?? 56 85 C0 57 89 4C 24 08 0F 84 ?? ?? ?? ?? A1 ?? ?? ?? ?? 53 05 ?? ?? 00 00 55 50 E8` | detour of the first 8 bytes (`kPrologue`); game object global at +4, network stats global at +0x17, its `add eax,imm` offset at +0x1D copied (4630h CatsEyeXI, 4638h retail); calls at +0x52 (label), +0x5D (YkDrawString), +0xFC (arrow frame), +0x11D (shape draw), +0x14D (font getter) checked as `E8` and their targets used |
| `kWindowDraw` (`netstat.cpp:49`) | YkWndNetStat::OnDrawPrimitive (FUN_101FFCF0) | `83 EC 08 56 8B F1 57 8D 44 24 08 8B 4E 08 33 FF 50 66 89 7C 24 0C 66 89 7C 24 0E 66 89 7C 24 10 66 89 7C 24 12 E8 ?? ?? ?? ?? 8B 46 14 2B C7 74 1B 48 75 2A 8B 4C 24 0A 8B 54 24 08 51 52 8D 4E 44 E8` | its call at +0x41 must go to `kNetDraw`'s match; its call at +0x25 is GetWindowLocate; the YkNetData is at window+0x44, control at +8 |
| `kSilence` (`netstat.cpp:62`) | the network frame's last-packet time store (FUN_100F9D80) | `89 8E ?? ?? ?? ?? B8 1F 85 EB 51 8B 0D ?? ?? ?? ?? F7 A6` | `operand` at +2 = the silence field's offset in the network object (4190h on CatsEyeXI per the comment) |

Also uses `uispace::find(game)` (another file's pattern).

**CatsEyeXI address:** `FFXiMain.dll+0x1FFA60` (draw), `+0x1FFCF0` (window draw), `+0x0F9D80` (network frame); the hook site is logged at runtime.
**Retail:** `kNetDraw` already has `??` over the +4630h/+4638h (the one docs/retail-signatures.md says moved) and the offset is read back at +0x1D. `kSilence`'s displacement is read back. Fixed: network state byte at net+0xC (== 4 means lost), `kLost` 4, 2000 ms failing threshold, stats fields received/sent/quality by structure (not given as offsets here). Fragile: the six call offsets (+0x52, +0x5D, +0xFC, +0x11D, +0x14D, string +0x20) and window offsets +0x25/+0x41/+0x44. Not found: logs and `network.*` does nothing; `kSilence` or `kWindowDraw` missing degrade the line layout / simple status only.

#### Auto login (`src/core/autologin.cpp`)

**Setting:** `[client] autologin`, `autologin.character`.
**What it does:** Finds four owner objects (the title-screen windows' globals) by pattern and reads their addresses from the code. Each frame it checks which window is open and calls that window's key handler (vtable slot 6, `__thiscall(event, id)`) to accept the terms, press Select Character, pick a character and answer Yes. No code is patched. Handlers' first bytes are checked before each call; any mismatch stops auto login for this start.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kTerms` (`autologin.cpp:31`) | DoLicenseWindow (0x1023C7B0) | `6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C0 1C 50 E8 ?? ?? ?? ?? 83 C4 04 B9 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8B 41 18 C3` | terms owner global = imm32 at +0x14; handler checked `8B 41 1C BA 01 00 00 00` |
| `kLobby` (`autologin.cpp:35`) | LobbyMenuProc (0x1023EC60) | `A1 ?? ?? ?? ?? 53 56 C7 40 14 02 00 00 00 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 33 DB 89 59 1C A1 ?? ?? ?? ?? 83 F8 01` | lobby owner global = imm32 at +0x10; handler checked `8B 41 1C BA 01 00 00 00` |
| `kList` (`autologin.cpp:39`) | SelectMenuProc (0x10241B80) at the list's open | `A1 ?? ?? ?? ?? 83 C4 04 3B C6 75 0B 8B 15 ?? ?? ?? ?? 89 5A 1C EB 0E A1 ?? ?? ?? ?? 89 1D ?? ?? ?? ?? 89 58 1C` | list owner global = imm32 at +0xE; handler checked `53 56 8B F1 BB 01 00 00 00` |
| `kYesNo` (`autologin.cpp:42`) | IwYesNoMenuConst2 (0x1023C900) | `A1 ?? ?? ?? ?? 8B 4C 24 04 53 33 DB 89 58 30 88 98 E1 A5 00 00 89 58 18 88 58 20 88 98 E0 A5 00 00 C7 40 14 01 00 00 00` | Yes/No owner global = imm32 at +1; handler checked `8B 41 30 BA 01 00 00 00` |

**CatsEyeXI address:** DoLicenseWindow `FFXiMain.dll+0x23C7B0`, LobbyMenuProc `+0x23EC60`, SelectMenuProc `+0x241B80`, IwYesNoMenuConst2 `+0x23C900`. Owner globals: logged at runtime, not recorded.
**Retail:** `??` over the owner globals and call displacements, but **`kYesNo` has `E1 A5 00 00` and `E0 A5 00 00` fixed (offsets 0xA5E1 and 0xA5E0 into the Yes/No owner)**. The retail doc lists the big object shifts at +8 only for `0x4630-0x2D608`; these are owner fields, not that object, but they are exactly the kind of offset that moves; if they do, the pattern fails, "yes/no 0" is logged and auto login is off. Other fixed values: structure offsets flag +0x1C/+0x30, answer +0x18, lobby button on +0x20, character list first +0x34 stride 0xB0 enabled +0x34, message +0x18 == 0xD, vtable slot 6, owner +8 control.

#### Packet hooks for plugins (`src/core/packets.cpp`)

**Setting:** always installed once, on the first frame after FFXiMain.dll is loaded; `[client] packets.keepactions` only changes what is done with packets.
**What it does:** Detours the game's packet decrypt/decompress and compress/encrypt functions (10 and 7 prologue bytes, `imports::detour`) so plugins and addons see and edit each buffer the game receives and sends. Not removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kDecryptPattern` (`packets.cpp:42`) | the decrypt function (`__cdecl` out, capacity, key, table, in, size) | `8B 44 24 18 81 EC B8 05 00 00 53 55 8B AC 24 C8 05 00 00 56 3B E8 57` | detour of the first 10 bytes (`mov eax,[esp+18h]; sub esp,5B8h`) |
| `kEncryptPattern` (`packets.cpp:50`) | the encrypt function | `8B 44 24 10 83 EC 68 B9 07 00 00 00 53 55 8B 6C 24 74 56 57 8B F0 8B FD 83 C5 1C F3 A5` | detour of the first 7 bytes (`mov eax,[esp+10h]; sub esp,68h`) |

**CatsEyeXI address:** not recorded (log line "hooked (received at ..., sent at ...)" prints them).
**Retail:** exact bytes, no `??`; stack frame sizes (`5B8h`, `68h`) and the 28-byte header (0x1C) are fixed. Not found: logs "plugins see no packets" and nothing else changes. The Ashita table has the same functions with different, shorter signatures (see the Ashita section).

#### Packet flow: shorter send wait (`src/core/packetflow.cpp`)

**Setting:** `[client] packetflow` (default on).
**What it does:** The game's call to its "may I send now" check before sending a buffer is rewritten (5 bytes: `E8` and a new rel32) to `send_check`, which passes a 250 ms wait instead of the game's once the server has answered (and always after a zone-out packet 0x00D). Written when on and `packets::ready()`, the game's 5 bytes written back when off. The Ashita PacketFlow plugin is held (not loaded) while this is on.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kSendCheck` (`packetflow.cpp:20`) | the game's pre-send check call, sharing bytes with the PacketFlow plugin | `8B 86 7C 41 00 00 50 6A 00 E8 ?? ?? ?? ?? 83 C4 0C 84 C0 74` | call at +9 (`kSendCheckCall`) rewritten; target is the game's check |

**CatsEyeXI address:** not recorded (logged as "+0x%X").
**Retail:** `7C 41 00 00` (the network object's +0x417C wait) is fixed in the pattern. It is below the `+0x8` shift range (`0x4630`...) in docs/retail-signatures.md, so should match, but the doc doesn't list this pattern as checked; flag if the object grew earlier. Not found: logs "keeps its own timing".

#### Game start without PlayOnline: polcore patches (`src/core/loader.cpp`)

**Setting:** always, when VanaCore.exe starts the game itself (the loader path; not `[client] retail=1`).
**What it does:** FFXiMain.dll is not scanned. The loader creates polcore and FFXiEntry through COM, scans **polcore.dll / polcoreeu.dll** for four code sites, calls one function found there, overwrites two 16-bit words with the local profile server's port, and swaps two entries (`kShowError` 0x402, `kSessionValue` 0x3EB) in polcore's function table. It also detours ws2_32's send, gethostbyname, connect, recv (generic IAT/prolog detours). None is undone.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| (literal, `loader.cpp:601`) | polcore's connection-lock setup call | `8B 56 2C 8B 46 28 8B 4E 24 52 50 51` | module: polcore; the rel32 at match - 4 is a function that is then called |
| (literal, `loader.cpp:603`) | polcore's connection object | `81 C6 38 03 00 00 83 C4 04 81 FE` | module: polcore; the pointer at match - 10 is the connection, zeroed 0x68 bytes and its +0x48 set to a buffer |
| (literal, `loader.cpp:605`) | the profile port in the connect code | `66 C7 46 26 14 C8 88 46 09 8D 46 24` | module: polcore; word at match + 4 (`14 C8` = 51220) overwritten with the local port |
| (literal, `loader.cpp:606`) | the second profile port place | `66 C7 05 BA 4A ?? ?? 14 C8` | module: polcore; word at match + 7 overwritten |

Also `table[kCharacterSlots]` (entry 0xD3): reads the 32-bit address at +31 of that function's code as the lobby's character slots.

**CatsEyeXI address:** not recorded; these are polcore, not FFXiMain. `FFXiMain.dll` hard-codes only the table entries it calls (the numbers above).
**Retail:** retail is started by pol.exe (`[client] retail=1`) and does not go through this path. If a pattern isn't found the loader reports "this PlayOnline (polcore) isn't one VanaCore knows (not found: ...)" and stops. All four patterns are exact bytes. Fragile: `-4`, `-10`, `+4`, `+7`, `+31`, 0x68-byte connection object and +0x48 buffer pointer, function table numbers (0x007D, 0x016F, 0x01A4, 0x032F, 0x03C5, 0x0516, 0x0402, 0x03EB, 0x00D3), `kCharacterSlotSize` 0x68.

#### Ashita compat: the pointer table (`src/core/compat/ashita/core.cpp`)

**Setting:** Ashita compat (`IPointerManager::Get` for a name plugins ask for that was not `Add`ed).
**What it does:** `kKnownPointers` (the content of Ashita's `ashita.pointers.ini`) is scanned lazily by name when a plugin calls `GetPointer("name")`. The match + `offset` is returned and cached; no game code is changed. `find_pattern` scans each module of the list (`polcore.dll,polcoreeu.dll` for one entry) and returns the first. The patterns are written without spaces; `queue_outgoing` (`core.cpp:986-987`) also scans `packets.queuepacket1` and `queuepacket2` with the same literal strings itself, to call the game's outgoing queue.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `chat.addchatmessage` (`core.cpp:830`) | AddChatMessage | `81EC????????535556578BE96A086A0089` | offset 0 |
| `chat.parseautotranslate.this` (`:831`) | the `this` global of parseautotranslate | `8B??83C9FF33C06A00F2AEF7D1496A01518B0D` | offset +19 |
| `chat.parseautotranslate` (`:832`) | the function | `5153558B6C24145685ED578BF1C64424????C64424????C64424` | 0 |
| `chat.parseinputtext.this` (`:833`) | `this` for ParseInputText | `81EC????????8B8C24????????5556578D` | +34 |
| `chat.parseinputtext` (`:834`) | the function | `81EC????????8B8C24????????5556578D` | 0 |
| `chat.inputtext.this` (`:835`) | the chat input object | `8B0D????????6A??53538B11536A??6A??536A` | +2 |
| `chat.logfile` (`:836`) | the chat log file pointer | `50E8????????84C075388B0D` | -4 |
| `packets.decryptbuffer` (`:837`) | decrypt | `53558BAC24C8050000563BE857` | -10 |
| `packets.encryptbuffer` (`:838`) | encrypt | `53558B6C247456578BF08BFD83C51CF3A5` | -12 |
| `packets.queuepacket1` (`:839`, also `core.cpp:986`) | the outgoing queue's slot function | `A1????????5685C0570F??????????8B74240C81FE????00000F8DCD0000008B` | 0; the `cmp esi,<limit>` imm16 is `??` (docs say 11Eh to 120h in retail) |
| `packets.queuepacket2` (`:840`, also `:987`) | the seal function | `8B44240C8B4C240803C18BC881E103000080` | 0 |
| `autofollow` (`:841`) | auto-follow structure | `8BCFE8????????8B0D????????E8????????8BE885ED750CB9` | +25 |
| `castbar` (`:842`) | cast bar global | `85F674??A1????????85C074??8B4808` | +5 |
| `entitymap` (`:843`) | entity array | `8B560C8B042A8B0485` | +9 |
| `entitymap.size` (`:844`) | entity count | `4781FF????????7C??89` | +3 |
| `inventory` (`:845`) | inventory | `A1????????8D9488????????8990????????E9` | +1 |
| `inventory.selecteditem` (`:846`) | selected item | `8B0D????????85C9742B` | +2 |
| `menu` (`:847`) | menu | `8B480C85C974??8B510885D274??3B05` | +16 |
| `party` (`:848`) | party | `0FBEC38D0C5256578BF58D0448` | +23 |
| `party.statusicons` (`:849`) | party status icons | `B93C0000008D7004BF????????F3A5` | +9 |
| `player` (`:850`) | player | `6A018D44242C68808080806683????506683????5152E8????????A1` | +28 |
| `player.hasability` (`:851`) | has-ability function | `A1????????85C074??8A88????????84C974??8A88????????84C974??05????????C333C0C3` | 0 |
| `player.haskeyitem` (`:852`) | key item check | `8B44240485C07C1683F8707D118B4C2408` | +20; the limit `70` is fixed (memory.cpp's own pattern wildcards it) |
| `player.hasspell` (`:853`) | has-spell function | `A1????????85C075??C38A88????????84C975??33C0C305????????C3` | 0, count 2 (third match) |
| `player.joblevels` (`:854`) | job level function | `8B0D????????85C974??8B4424043C1073??25FF000000` | 0 |
| `player.jobmasterlevels` (`:855`) | master level function | `8B0D????????85C974??8B4424043C1873??25FF000000` | 0 |
| `player.loginstatus` (`:856`) | login status | `6605????6685C074??8B0D????????25????000050` | +11 |
| `player.petmp` (`:857`) | pet MP | `84C0750333C0C333C0A0????????C3` | +10 |
| `player.pettp` (`:858`) | pet TP | `8B44240C5657668B48048D70048BC183E03F83E802` | +61 |
| `recast.abilities` (`:859`) | ability recasts | `894124E9????????8B46??6A006A00508BCEE8` | +25 |
| `recast.spells` (`:860`) | spell recasts | `56BE????????66833E00??????????????662906` | +2 |
| `target.target` (`:861`) | target | `53568BF18B480433DB3BCB75065E33C05B59C38B0D` | +45 |
| `target.settarget` (`:862`, also in the second list below) | set target function | `8B0D????????83C40885C974??85C074??6A006A0150E8` | 0 |
| `clientversion` (`:863`) | client version string | `81EC240200008B0D????????C6442400BFC644240100C644240200` | +12 |
| `config.autodisconnect` (`:864`) | polcore's auto-disconnect setting, **module `polcore.dll,polcoreeu.dll`** | `8B44240883C40485C075??A3????????A3` | +12 |
| `config.languagefilter` (`:865`) | language filter | `8B0D????????85C975??83C8??C38B44240450E8????????C3` | +2 |

(Line numbers are the table's lines; 36 rows. Every row's module is `FFXiMain.dll` unless noted.)

**CatsEyeXI address:** not recorded (Ashita's file, generic).
**Retail:** The only wildcard coverage is what Ashita's file has; the packet limit in `queuepacket1` is `????`. Notes: `entitymap`, `party`, `player`, `target.*`, `inventory` here duplicate what `game.cpp` and `memory.cpp` find with their own patterns (not the same strings: e.g. memory.cpp wildcards the key item limit). Fixed: the key item limit `70` in `player.haskeyitem`, the 0x30/`3F`/`E8` immediates in `player.pettp`. Fixed offsets from a match: every non-zero `offset` above (+19, +34, +2, -4, -10, -12, +25, +5, +9, +3, +1, +2, +16, +23, +9, +28, +20, +11, +10, +61, +25, +2, +45, +12, +12, +2); a miss returns 0 to the plugin.

#### Ashita compat: game data objects (`src/core/compat/ashita/memory.cpp`)

**Setting:** Ashita compat (`IMemoryManager` and its entity, party, player, target, inventory, auto-follow, cast bar and recast objects).
**What it does:** Resolves the structures the SDK exposes, lazily on first use, each from a piece of the game's code (`places()`), then reads (and for entities and some fields, writes) the game's own structures in place. No game code is patched. `game::addresses()` (game.cpp, not this file) supplies entities, party, player, target pointer; the rest are scanned here. It calls three of the found functions directly (`job_level`, `job_master_level`, abilities, spells as `__cdecl`).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| `kInventoryPattern` (`memory.cpp:30`) | character object global and the inventory's offset in it | `A1 ?? ?? ?? ?? 8D 94 88 ?? ?? ?? ?? 89 90 ?? ?? ?? ?? E9` | `code_dword(+1)` = character global, `+8` = inventory offset |
| `kAutoFollowPattern` (`:32`) | auto-follow structure | `8B CF E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B E8 85 ED 75 0C B9` | dword at +25 (just after the pattern's `B9`) |
| `kCastBarPattern` (`:34`) | cast bar global | `85 F6 74 ?? A1 ?? ?? ?? ?? 85 C0 74 ?? 8B 48 08` | dword at +5 |
| `kStatusIconsPattern` (`:36`) | party status icons | `B9 3C 00 00 00 8D 70 04 BF ?? ?? ?? ?? F3 A5` | dword at +9 |
| `kKeyItemsPattern` (`:38`) | key item table | `8B 44 24 04 85 C0 7C ?? 3D ?? ?? ?? ?? 7D ?? 8B 4C 24 08 8B 14 85` | dword at +22, just past the pattern; limit `0x80` words assumed (`id >= 0x80 * 32` in `HasKeyItem`) |
| `kJobLevelPattern` (`:40`) | job level function | `8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 10 73 ?? 25 FF 00 00 00` | called as `uint8_t __cdecl(job)` |
| `kJobMasterLevelPattern` (`:41`) | master level function | `8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 18 73 ?? 25 FF 00 00 00` | called the same way |
| `kAbilitiesPattern` (`:44`) | the abilities' bits getter (packet 0xAC, 224 bytes) | `A1 ?? ?? ?? ?? 85 C0 74 ?? 8A 88 ?? ?? ?? ?? 84 C9 74 ?? 8A 88 ?? ?? ?? ?? 84 C9 74 ?? 05 ?? ?? ?? ?? C3 33 C0 C3` | called as `const uint8_t* __cdecl()`; 0xE0 bytes assumed |
| `kSpellsPattern` (`:47`) | the known-spells bits getter | `A1 ?? ?? ?? ?? 85 C0 75 ?? C3 8A 88 ?? ?? ?? ?? 84 C9 75 ?? 33 C0 C3 05 ?? ?? ?? ?? C3` | third match (`count` 2); called as `__cdecl()`; 0x400 spells assumed |
| `kPetMpPattern` (`:49`) | pet MP percent | `84 C0 75 03 33 C0 C3 33 C0 A0 ?? ?? ?? ??` | dword at +10 |
| `kPetPattern` (`:51`) | pet info copied from its packet | `8B 44 24 0C 56 57 66 8B 48 04 8D 70 04 8B C1 83 E0 3F 83 E8 02` | dword at +61 (well past the 21-byte pattern); pet TP read at that address + 0x0C |
| `kAbilityRecastPattern` (`:54`) | the ability recast block | `89 41 24 E9 ?? ?? ?? ?? 8B 46 ?? 6A 00 6A 00 50 8B CE E8` | dword at +25 (past the pattern); 31 slots, timers at +0xF8 |
| `kSpellRecastPattern` (`:59`) | the spell recast loop | `56 BE ?? ?? ?? ?? 66 83 3E 00 ?? ?? ?? ?? ?? ?? ?? 66 29 06` | `code_dword(+2)` = array start; end operand at +0x24 (`kSpellRecastEnd`) after `83 C6 02 81 FE` is checked, count = (end - begin)/2 else 0x401 (cexislots makes it 0x1001) |
| `kTargetWindowPattern` (`:62`) | the CTkTarget global | `8B 0D ?? ?? ?? ?? 8B 5C 24 10 8B F8 8A 41 65 84 C0` | dword at +2 |
| `set_target` literal (`memory.cpp:1172`) | the target manager global and its set-target call (from XI on Anything) | `8B 0D ?? ?? ?? ?? 83 C4 08 85 C9 74 ?? 85 C0 74 ?? 6A 00 6A 01 50 E8` | `site+2` = the manager global; the call's target from the rel32 at `site+0x17` (`site+0x1B` + it); called as `__thiscall(*manager, actor, 1, 0)` for `ITarget::SetTarget` |

**CatsEyeXI address:** not recorded.
**Retail:** `??` over every global and displacement; reads each operand back. Fragile: **+22, +25 (autofollow), +61, +25 (recasts), +0x24, +0x0C, +0xF8** are fixed distances past the patterns' ends and would need the function compare; hard-coded sizes (0x80 key item words, 0xE0 ability bytes, 0x400 spell bits, 31 ability recasts, 0x401 default spell recasts). Hard-coded structure fields come from the SDK's `abi.hpp` layouts (entity, party member 0x7C stride is `FFXI::partymember_t`, not stated here); docs/retail-signatures.md says the entity fields checked did not move. Not found: the structure reads null / returns 0.

#### Gamepad: fixed addresses in FFXiMain (`src/core/input.cpp`, with `input_background.hpp`)

**Setting:** `[input] gamepad.*` (search interval, mode, buttons, GUID selection, `gamepad.disable`, `gamepad.disableenumeration`), and always for the XInput check hook.
**What it does:** Uses **fixed RVAs** in the unpacked CatsEyeXI client, each guarded by a head-byte check (and a RET check) and skipped when the check fails. It detours the XInput-device check (0x8990, 6 bytes) so the slow WMI query runs once per product; it calls the game's pad setters directly (live gamepad settings and pad choice); and it reads two switch bytes to know the game's "works while inactive" setting. The detour is installed from the first controller search and not removed.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| none: `kIsXInputAt` 0x8990 (`input.cpp:155`) | FUN_10008990 (IsXInputDevice, `__cdecl` GUID) | head bytes `81 EC 84 00 00 00 53 55 56 57 33 DB B9 13 00 00 00`; callers at 0x580D and 0x587A must be `E8` to it | detour of 6 bytes (`sub esp,0x84`), answers cached per GUID |
| none: `0x55A0` (`input.cpp:1676`) | FUN_100055A0 (set pad GUID, `__stdcall` ret 0x14) | `56 57 B9 04 00 00 00 8D 7C 24 0C BE`, and `code[0x4E..0x50] = C2 14 00` | called directly |
| none: `kModeSetter` 0x5490 (`input.cpp:1602`) | pad mode setter | head `83 EC 20 A0`, RET at +0x106 (`C2 0C 00`) | called directly; its `A0 <addr>` at +4 gives the gamepad switch byte (+4 the XInput one), read as `at` and `at+4` (FUN_10005490's 0x10453AC4/8) |
| none: `kButtonsSetter` 0x5130 | pad buttons setter | head `8B 54 24 04`, RET at +0x1ED (`C2 08 00`) | called directly |
| none: `kButtonsExSetter` 0x5320 | second pad buttons setter | head `8B 54 24 04`, RET at +0x162 (`C2 08 00`) | called directly |
| none: `kSwitchCodeAt` 0x5600 (`input_background.hpp:14`) | FUN_10005600 (puts the inactive switch aside) | head `8A 44 24 04 33 D2 3A C2 A0`, RET at +0x51 (`C2 04 00`) | operands read at +0x08 (`A0`, aside), +0x13 (`A0`, now), +0x1E (`A2`, wanted), +0x4C (`A2`, pad changed), and again at +0x30 (`A0`), +0x35 (`88 15`), +0x3B (`A2`) which must agree; all seven addresses must be inside the image |

**CatsEyeXI address:** `FFXiMain.dll+0x8990` (XInput check), `+0x580D`, `+0x587A` (its callers), `+0x55A0`, `+0x5490`, `+0x5130`, `+0x5320`, `+0x5600`, `FUN_100057D0` (search callback, comment only), `FUN_10002840` (settings loader, comment only), `FUN_10005A40`, `FUN_101B1150` (comment only), `+0x1100` (the game's Windows-key hook, comment only), `0x10453AC4/8` (the two switch bytes).
**Retail:** **none of these uses `scan::find`**: they are fixed offsets, not in docs/retail-signatures.md's pattern count and not covered by `tools\sigaudit.py`. They only work on a build where those functions sit at those RVAs with those bytes; on any other build the log says "this client's XInput check isn't the one VanaCore knows" / "pad code isn't the one it was written for" and the gamepad settings do nothing. Other game-side hooks (not patterns): IAT hooks of `DINPUT8.dll!DirectInput8Create` (every module), `KERNEL32!GetProcAddress` and `USER32!PeekMessageA` in FFXiMain.dll only, and vtable hooks of the game's `IDirectInput8A` (`EnumDevices` slot 4, `CreateDevice` slot 3) and its devices' `GetDeviceState`/`GetDeviceData`.

#### Window frame and mouse (`src/core/window.cpp`)

**Setting:** `[window]` keys (`resizable`, `close.shutdown`, borderless modes, `keepawake`, ...); the metrics hook is always installed.
**What it does:** Patches FFXiMain.dll's import table: `USER32.dll!GetSystemMetrics` is replaced (via `imports::hook` on the module whose file name is `FFXiMain.dll`) with `system_metrics`, which gives the game the real frame and title bar sizes of its window so its mouse correction is right. No patterns and no fixed addresses are used in code. The game's window menu: when `close.shutdown` is on, Close is added back with `AppendMenuW`.

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| none | FFXiMain.dll's `GetSystemMetrics` import | IAT entry | replaced with `system_metrics` for `SM_CXSIZEFRAME`, `SM_CYSIZEFRAME`, `SM_CYCAPTION` |

**CatsEyeXI address:** only in comments: `FFXiMain+0x12452F` (button held toggle), `0x10001C00` (frame tick), `0x10016A93 -> 0x10001B50`, `0x1001656E` (RemoveMenu SC_CLOSE). None is used in code.
**Retail:** import-based, so unaffected by layout changes. The game's `WM_USER+1` tick (`kGameTick`) and the "toggles held state on each press" behaviour are assumed from the CatsEyeXI client's code (comments only).

#### Lua `vana.memory` (`src/core/lua_api.cpp`)

**Setting:** Lua API `vana.memory.*` (`find`, `module`, read and write functions).
**What it does:** Generic access for scripts: `vana.memory.find(module name or address, size, pattern[, offset[, count]])` runs `scan::find` over a named module (or an address range); `read`/`write` copy to and from any address. Nothing in lua_api.cpp scans or patches FFXiMain.dll by itself; it also exposes `vana.paths.game` from `dats::ffxi_folder()` and `vana.game` (game.hpp's data).

| Signature | Finds | Pattern | Then |
|---|---|---|---|
| (caller's, `lua_api.cpp:325`) | whatever a script asks for | script-supplied | `scan::find(begin, size, pattern, count)` + offset, returned as a number or 0 |

**CatsEyeXI address:** not applicable.
**Retail:** none.
