# VanaCore: how Ashita and Windower addons and plugins run on it

A technical write-up of how VanaCore runs **Ashita v4 addons and plugins** and **Windower 4 (and some
Windower 3) addons and plugins** without Ashita or Windower being present. It covers everything
we had to build, the formats and ABIs we reverse engineered, and the mistakes that cost time.

Section 10 maps each part onto **xi-on-mac** (the static recompilation of `FFXiMain.dll` to C on
arm64 macOS), for anyone porting the same capability there; sections 1–9 say what the parts are and
why they're built the way they are.

Source paths below are VanaCore's (`src/core/...`). Addresses are RVAs in **CatsEyeXI's client
build** of `FFXiMain.dll` (preferred base 0x10000000; the live client loads relocated, so always
use RVAs or patterns). Every hook point is found by a **byte pattern**, never a fixed address, and
the patterns are given so they can be matched against another build's unpacked image.

---

## 0. The short version

- **One process, one DLL.** `VanaCore.exe` starts the game and injects `VanaCore.Core.dll`
  before the game's main thread runs. Everything below lives in that DLL, on the game's thread.
- **Addons are Lua.** One LuaJIT 2.1 state per addon. VanaCore's own API is `vana.*`. Ashita's
  and Windower's APIs are **Lua compatibility layers** (`ashita.lua`, `windower.lua`) written over
  `vana.*`, loaded into the addon's state before its own file. Their libraries (Ashita's `libs\`,
  Windower's `libs\` and `res\`) run **unchanged**.
- **Plugins are native x86 DLLs.** VanaCore **is** the host. It `LoadLibrary`s the plugin, calls
  its exports, and hands it C++ objects whose **vtables match Ashita's SDK (4.16 and 4.30)** or
  **Windower's `Hook.dll` (0x04070300)**, implemented over VanaCore's own chat, packets, text
  renderer, game data and ImGui.
- **Everything hangs off six game hooks:**
  1. the game's command line;
  2. its chat log writer;
  3. its packet decrypt and encrypt functions;
  4. the D3D8 device's `Present`;
  5. DirectInput's keyboard and mouse reads;
  6. the game window's procedure.

  Game data (entities, party, player, target, inventory) is read **in place** from `FFXiMain`'s
  memory, at addresses found by patterns.

---

## 1. Architecture

### 1.1 Process and threads

`VanaCore.exe` logs in itself (a reimplementation of xiloader's login), then starts the game
suspended with `VanaCore.Core.dll` loaded, and calls its export `VanaCoreInstall` on a remote thread
before the main thread resumes. `VanaCoreInstall` installs hooks:

- **Import hooks:** it rewrites a module's IAT entry. It waits for `FFXiMain.dll` to load (the
  loader notification) and hooks its `d3d8!Direct3DCreate8` and `dinput8!DirectInput8Create`.
- **Vtable hooks:** it swaps entries of COM vtables: `IDirect3D8::CreateDevice`,
  `IDirect3DDevice8::Present`, and `IDirectInputDevice8A::GetDeviceState` and `GetDeviceData`.
- **Detours:** it overwrites a function's first bytes with a `jmp` to VanaCore, keeping a
  trampoline that runs the moved prologue and jumps back. It uses them on game functions found by
  pattern. The prologue sizes are hand-checked so they're whole instructions with no relative
  jumps.

**Everything that touches addons and plugins runs on the game's thread**: the render loop
(`Present`), the command line, the window procedure and the DirectInput reads (FFXI polls input on
that thread). Plugin and addon state isn't locked; the hosts record the game thread's id and refuse
(log once) calls from another thread.

### 1.2 The frame

Each frame is driven from VanaCore's `IDirect3DDevice8::Present` hook (`overlay.cpp`), in this order:

1. `chat::on_frame`: installs the chat hooks on the first frame, flushes chat-log lines queued
   before the chat log existed, and runs lines queued with `QueueCommand` / `vana.chat.run`.
2. `packets::on_frame`: installs the packet hooks on the first frame.
3. `gui::begin_frame`: `ImGui::NewFrame` (one ImGui context for everything).
4. `addons::on_frame`: `scripts\boot.txt` on the first frame, then each addon's tasks
   (coroutines), then each addon's `present` event (`d3d_present` for Ashita, `prerender` for
   Windower).
5. `plugins::on_present`: each Ashita plugin's `Direct3DPresent`.
6. `gui::end_frame`.
7. The drawing, with VanaCore's own render state (saved and restored around it):
   - in the game's pixels: Ashita plugins' fonts and primitives, then Windower plugins'
     `PreRender`, their text and primitives, and `PostRender`;
   - then in VanaCore's interface pixels: VanaCore's label and FPS counter, ImGui's draw data
     (ImGui works in the game's pixels, as Ashita's does; its vertices and clip rectangles are
     scaled here, and VanaCore's own ImGui windows drawn larger to keep their size), and addons'
     text boxes and the `/vana` window.
8. The real `Present`.

### 1.3 Source map

| Part | Files |
|---|---|
| Hook helpers (IAT, vtable, detour, pattern scan) | `imports.hpp`, `scan.hpp` |
| Command line and chat log | `chat.cpp` |
| Packets | `packets.cpp` |
| Game data (entities, party, player, target) | `game.cpp`, `game.hpp` |
| DAT reading (FTABLE/VTABLE, ROM paths) | `dats.cpp` |
| Addon host (LuaJIT states, commands, scripts) | `addons.cpp` |
| Native `vana.*` API | `lua_api.cpp` |
| Lua runtime (events, tasks, print) | `lua/vana.lua` (embedded as a resource) |
| Ashita addon layer | `lua/compat/ashita.lua` (embedded) |
| Windower addon layer | `lua/compat/windower.lua` (embedded) |
| ImGui for Lua (Ashita's `imgui` library) | `lua_imgui.cpp` |
| Plugin front end (folders, `/plugin`) | `plugins.cpp` |
| Ashita plugin host | `compat/ashita/*.cpp`, `abi.hpp` (generated by `tools/gen_ashita_abi.py`), `gui_v416.inc` (by `tools/gen_ashita_gui.py`) |
| Windower plugin host | `compat/windower/*.cpp`, `abi.hpp` (hand-written from RE) |
| Input (DirectInput and window hooks) | `input.cpp`, `window.cpp` |
| ImGui integration | `gui.cpp`, `third_party/imgui` (pinned version, see 7) |
| Text renderer (fonts, outlines, colour codes) | `text.cpp` |

---

## 2. The game hooks

These are what every addon and plugin feature ultimately stands on. Each is found by a byte
pattern in `FFXiMain.dll`'s code (`??` is any byte).

### 2.1 The command line (`chat.cpp`)

`int __cdecl ParseInput(const char* line, int mode)` at **+0x80BD0**: every line typed into the
chat box, **every macro line and every menu command**, goes through it. It runs `/` commands and
sends the rest as chat. It starts `sub esp, 0x404`.

```
81 EC 04 04 00 00 8B 8C 24 08 04 00 00 55 56 57 8D 84 24 10 02 00 00 68 00 02 00 00
50 33 ED 51 8B 0D ?? ?? ?? ?? 89 6C 24 18 E8                  (detour, prologue 6 bytes)
```

VanaCore's detour calls the command handler (section 3) first. If that handles the line, it
returns 0 and the game never sees it; otherwise it calls the original. `mode & 0xFF` is the
input mode (typed, macro, ...). Lines VanaCore injects are flagged `injected` so handlers can
tell them apart (Ashita's `HandleCommand(mode, line, injected)`). `/shutdown` is noticed here
too: the game's own teardown faults every time, and the crash handler must not report that.

### 2.2 The chat log writer (`chat.cpp`)

`void __cdecl WriteLine(int mode, const char* text)` at **+0x765D0**: the game's wrapper around
its chat log object's add-line method. `mode` is the chat log colour/channel (FFXI's chat modes);
text is Shift-JIS with FFXI's colour codes (0x1E xx, 0x1F xx) and auto-translate phrases (0xFD
...5 bytes).

```
8A 44 24 04 8B 54 24 08 88 44 24 04 33 C0 50 6A 01 8D 4C 24 0C 50 51 8B 0D ?? ?? ?? ??
52 88 44 24 19 88 44 24 1A E8 ?? ?? ?? ?? C3        (called, not hooked; the chat log global is at +0x19)
```

The global holding the chat log is **null until the game makes it**, so lines written before then
are queued and flushed on a later frame. Long lines are cut at a unit boundary (a colour code, a
Shift-JIS pair and an auto-translate phrase are never split; about 240 bytes per piece).

### 2.3 The chat input line (`chat.cpp`, Ashita's `ashita.offsets.ini [chat.inputtext]`)

`mov ecx,[input]; push ..; push ebx; push ebx; mov edx,[ecx]; ...` finds the global holding the
game's chat manager: `8B 0D ?? ?? ?? ?? 6A ?? 53 53 8B 11 53 6A ?? 6A ?? 53 6A` (+2). In that
object, the input line is at +0x7E30:

- the line's state is at +0xF108 (1 to 3 while closed, 4 while the chat line is open, 5 while a bazaar or search
  comment is being typed; Ashita's `IsInputOpen` returns 0x11 for 4, 0x21 for 5 and 0 otherwise, its
  `ChatInputOpenStatus`, and addons test for exactly those);
- the text is at +0x7E30+0x6C4;
- the length is at +0x7E30+0x888;
- the display copy (what the game renders, with the caret marker 7F FF and auto-translate previews) is at
  +0x7E30+0x6E04. It's written by the line window's draw (CTkMenuCtrlData::OnDraw, 0x101173d0, running the
  line's own drawing), so a window that isn't drawn has a stale copy: `[client] input.hide` moves the window off
  the screen rather than skipping its draw, and does so by itself while fancychat's input bar is on (its setting read
  from the addon's state with `addons::ask`). The same ask says whether fancychat pins the game's chat windows
  closed; then the chat window's calc (0x101611a0,
  `A1 ?? ?? ?? ?? 83 EC 24 55 56 8B F1 8A 48 08`) is run with the line timer (+0x34, lines = timer / +0x4e)
  zeroed and its `open` argument dropped, so the window never grows.
  With `[client] menus.ontop` (default 1), each game window drawn (OnDraw, its rect at +0x3A) over fancychat's
  `FancyChat_Chat*` or `FancyChat_InputBar*` ImGui windows is laid over everything after (raise.cpp, as the
  auto-translate list is), with the mouse there going to the game, whatever fancychat's settings. The ask also says
  whether fancychat's **Prevent obstructing FFXI UI** is on (`allSettings.EnabledChatMove`); with it on, the global
  `SetTargetPosX` fancychat's render loop asks where to slide its chat to (lib/parser.lua) is replaced by one that
  answers nowhere (x 0, `mvc.targetposY` past the screen), so the chat stays where it is.

It's needed for `IChatManager::GetInputText`, `SetInputText` and `IsInputOpen`, and for paste.

### 2.4 Packets (`packets.cpp`)

The same two functions Ashita hooks:

- **Decrypt**, `int __cdecl (uint8_t* out, uint32_t capacity, void* key, void* table, const uint8_t* in, uint32_t size)`
  at **+0xDD410**. It decrypts and decompresses a buffer from the server into `out` (a 0x1C-byte
  header, then packets) and returns the bytes written. The game then walks the packets.
  Pattern: `8B 44 24 18 81 EC B8 05 00 00 53 55 8B AC 24 C8 05 00 00 56 3B E8 57`, prologue 10
  bytes.
- **Encrypt**, `int __cdecl (uint8_t* out, uint32_t capacity, void* key, const uint8_t* in, uint32_t size, void* table)`
  at **+0xDD2E0**. It compresses and encrypts a buffer the game is sending (header, then packets).
  Pattern: `8B 44 24 10 83 EC 68 B9 07 00 00 00 53 55 8B 6C 24 74 56 57 8B F0 8B FD 83 C5 1C F3 A5`,
  prologue 7 bytes.

After the real decrypt returns (and before the real encrypt runs), VanaCore processes the plain
buffer (section 4) and rebuilds it only if a plugin handles packets or packets are waiting to be
added. The packet hooks are what make Ashita's `HandleIncomingPacket` / `HandleOutgoingPacket`,
Windower's `IncomingChunk` / `OutgoingChunk`, packet injection and Windower's packet-derived game
state work.

### 2.5 Game data (`game.cpp`, `compat/ashita/memory.cpp`)

Read in place, with every read a protected copy (`__try`/`memcpy`), because structures don't
exist before login and while zoning:

| What | Pattern | Read |
|---|---|---|
| Entity pointer array | `8B 56 0C 8B 04 2A 8B 04 85` | u32 at +9 → `entity_t*[0x900]` |
| Entity count | `47 81 FF ?? ?? ?? ?? 7C ?? 89` | u32 at +3 (0x900) |
| Party (18 × 0x7C) | `0F BE C3 8D 0C 52 56 57 8B F5 8D 04 48` | u32 at +23 |
| Player stats (0x438) | `6A 01 8D 44 24 2C 68 80 80 80 80 66 83 ?? ?? 50 66 83 ?? ?? 51 52 E8 ?? ?? ?? ?? A1` | u32 at +28 |
| Target | `53 56 8B F1 8B 48 04 33 DB 3B CB 75 06 5E 33 C0 5B 59 C3 8B 0D` | global at +45; the target data pointer is the global 0x2F4 after it |
| Inventory | `A1 ?? ?? ?? ?? 8D 94 88 ?? ?? ?? ?? 89 90 ?? ?? ?? ?? E9` | character global (+1) and the inventory's offset in it (+8, 0x9860 here) |
| Auto-follow | `8B CF E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B E8 85 ED 75 0C B9` | +25 |
| Cast bar | `85 F6 74 ?? A1 ?? ?? ?? ?? 85 C0 74 ?? 8B 48 08` | global at +5 |
| Party status icons | `B9 3C 00 00 00 8D 70 04 BF ?? ?? ?? ?? F3 A5` | +9 |
| Key items (4096 bits) | `8B 44 24 04 85 C0 7C ?? 3D ?? ?? ?? ?? 7D ?? 8B 4C 24 08 8B 14 85` | table after the bound check. CatsEyeXI's table is 0x80 dwords; retail's is smaller, which Ashita's own pattern expects |
| Job level / master level (functions) | `8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 10 73 ?? 25 FF 00 00 00` (0x18 for master) | `uint8_t __cdecl (int job)`, called |
| Known spells | `A1 ?? ?? ?? ?? 85 C0 75 ?? C3 8A 88 ?? ?? ?? ?? 84 C9 75 ?? 33 C0 C3 05 ?? ?? ?? ?? C3` (third match) | a function returning the bitfield, or null before it's loaded |

The entity, party member, player and target field offsets are in `game.cpp`. For plugins,
VanaCore uses the **SDK's own structures** (`Ashita::FFXI::entity_t` and the rest, from the SDK
headers), which match the game's memory. Plugins get pointers into the game's memory, so their
setters write the game's data directly, exactly as in Ashita.

### 2.6 Direct3D 8, DirectInput, the window

- **`Direct3DCreate8`** (IAT hook on `FFXiMain`) → `CreateDevice` (vtable) → `Present` (vtable).
  At device creation VanaCore does four things:
  - sets the window title;
  - subclasses the window procedure (`SetWindowLongPtr`);
  - tells plugins the device exists (`Direct3DInitialize`, Windower's `on_device`);
  - hooks `Present`.
- **DirectInput 8** (IAT hook on `DirectInput8Create`) → `IDirectInput8A::CreateDevice` → for the
  system keyboard and mouse, `GetDeviceState` (slot 9) and `GetDeviceData` (slot 10). FFXI reads
  **its keyboard and mouse through DirectInput, not window messages**. This matters for input
  capture: to keep a click or key from the game, you must blank it in DirectInput's reads, not
  just swallow the window message. See 7 and 6.2.9.
- **The window procedure** gets every mouse and key message first. It routes them in this order:
  1. ImGui (when it wants them);
  2. VanaCore's UI;
  3. plugins' mouse and keyboard callbacks;
  4. addons' `mouse` events;
  5. the game.
- **`GetCursorPos`** is IAT-hooked in the game's and plugins' modules so the cursor maps into
  the game's pixels when VanaCore draws the game at a size other than its resolution.

---

## 3. Command routing

`chat.cpp`'s detour calls `addons::on_command(line, mode, injected)`, which runs:

1. **VanaCore's own commands**: `/addon`, `/plugin`, `/exec`, `//screenshot` (and `//addon`,
   `//plugin`, `//exec`). These are handled and never reach the game.
2. **Native plugins**, `plugins::on_command`:
   - Ashita plugins' `HandleCommand(mode, line, injected)`, in **priority order** (lower priority
     value first, then load order), stopping at the first that returns true. This is Ashita's
     order.
   - Windower plugins: `//<name> args` goes to that plugin's `PluginCommand(" args")`, with
     `<name>` its DLL's name or, for Windower 3, the names its `GetInfo` lists (`st|statustimer`).
     Note the leading space: FFXIDB skips exactly one.
3. **Addons**: each addon's `command` event, with a table `{command, mode, injected, blocked}`.
   A handler blocks by setting `e.blocked = true` or returning true. Every addon still sees the
   line (with `blocked` set), as in Ashita.
4. **An unhandled `//` line** goes to each Windower plugin's `UnhandledCommand(line without "//")`
   until one returns true. If none does, VanaCore prints an error rather than letting the game
   send `//foo` as chat.
5. Anything else goes to the game's own `ParseInput`.

**Injected lines**: `IChatManager::ParseCommand` runs a line now and `QueueCommand` runs it next
frame, both through the same path with `injected = true`. The Windower Lua layer's
`windower.send_command` parses Windower's console syntax: `input /echo`, `wait 2`,
`lua load x`, other addons' commands, several separated by `;`.

**Scripts**: `scripts\boot.txt` runs on the first frame, each line as if typed. `#` starts a
comment, and `/wait <s>` (or `//wait`) delays the lines after it. `/exec <name>` runs
`scripts\<name>.txt`. That's how every addon and plugin gets loaded at start.

---

## 4. The packet pipeline

The buffer the game decrypts and encrypts is a **0x1C-byte header** then packets. Each packet
starts with a u16: the id in bits 0–8 and the size in 4-byte units in bits 9–15. Then comes a u16
sequence number, then the data.

`process(incoming, buffer, size, out, capacity)`:

1. If no plugin handles packets and none are waiting to be added, it copies straight through.
   This matters for performance: most frames have no packet plugins.
2. It keeps a copy of the original buffer. Plugins get **both** the packet as it came and a
   `modified` copy they can change, plus the whole buffer after the header (Ashita's
   `sizeChunk` / `dataChunk`: some plugins look at neighbouring packets).
3. For each packet, the plugins are called in order:
   - Ashita plugins (priority order) first, then Windower plugins, each seeing `blocked` so far.
   - Ashita: `HandleIncomingPacket(id, size, data, modified, sizeChunk, dataChunk, injected, blocked)`.
   - Windower: `IncomingChunk(original, modified, injected, blocked)`; it reads 0x200 bytes from
     both, so the buffers must be at least that large.
   - A plugin may change the size in `modified`'s header, and VanaCore honours it within reason
     (4..0x1FC).
4. Blocked packets are dropped; the rest are written to `out`.
5. Then packets **added** by plugins go in: `AddIncomingPacket` / `AddOutgoingPacket` (Ashita)
   and `InjectIncoming` / `InjectOutgoing` (Windower). Each is padded to 4 bytes and given the
   buffer's last sequence number, then **also passed through the plugins with `injected = true`**
   (they can block their own). An added packet that doesn't fit waits for the next buffer.
   - **An Ashita plugin that adds a packet doesn't see it go through** (`packets::inject`'s `from`).
     Ashita calls a plugin's handler for its own packet inside its `AddOutgoingPacket` call, and
     plugins guard against that: LegacyAC sets a flag (`this+0xDC`) around the call and its
     `HandleOutgoingPacket` returns at once while it's set. VanaCore dispatches later, with the flag
     clear, so LegacyAC took its own re-added cast packet (0x1A) for a new cast, swapped gear, added
     the packet again, and so on without end: hundreds of 0x050 equip packets a burst, until the server
     dropped the client (R0). Addons' packets still go to every plugin.
   - At most 256 packets wait each way; past that, new ones are dropped and logged once, so a loop like
     that can't flood the server.
   - `data` for an outgoing packet points into the buffer, and LegacyAC reads on past it until a size of
     0 (no check against `chunk_size`), so `original` and each added packet are followed by zeros.
   - `data` for a received packet is a copy of its own, 0x200 bytes with zeros after the packet, as Ashita's
     (Ashita.dll 0x10139247 copies each into a zeroed 0x200-byte vector), and `modified` is the same both ways.
     Stylist reads 0x00D's look at +0x48 whenever the word there isn't 0, and CatsEyeXI's 0x00D is mostly
     0x48 bytes: with the next packet (or stale stack) there it stored nonsense looks, put race 0xC4 on a
     character, and the game's XiSkeletonActor::SetUp (0x100D2CB0) crashed on the negative race.
6. Incoming buffers are also fed to Windower's **game-state block** (6.3.4), whether or not a
   Windower plugin is loaded, so the state is complete when one loads later.

Pitfall: the game's outgoing buffer is built in place, and `capacity` is small. VanaCore processes
outgoing packets in a static 0x2000-byte buffer and hands that to the real encrypt.

---

## 5. Lua addons

### 5.1 The host (`addons.cpp`)

- **Folders**: `addons\vanacore\<name>\<name>.lua`, `addons\ashita\<name>\<name>.lua` and
  `addons\windower\<name>\<name>.lua`, searched in that order unless `/addon load <name> <kind>`
  names the kind. The Windower folder is laid out **exactly like Windower's own addons folder**
  (`addons\windower\libs\`, `addons\windower\res\`), because Windower's libraries build paths
  relative to it.
- **Kind detection** decides which API an addon's file is written for. The rule is counted on
  identifiers not preceded by an alphanumeric, `_` or `.`:
  - any `vana.` means VanaCore;
  - otherwise it counts `windower.` + `_addon.` against `ashita.` + `AshitaCore` + `addon.`, and
    the larger wins;
  - if neither appears, the folder's kind.

  This lets an addon in the "wrong" folder still work.
- **One `lua_State` per addon** (LuaJIT 2.1: Lua 5.1 + `ffi`, `bit`, `jit`). Its setup:
  1. The standard libraries and the native `vana.*` API (`lua_api::open`).
  2. `vana.lua` (embedded as an RCDATA resource): events, tasks and `print`. It **returns a
     table of hooks** (`raise`, `frame`) that the C++ side keeps a registry reference to and
     calls into.
  3. The kind's compatibility script (`ashita.lua` or `windower.lua`, also embedded).
  4. The addon's file, loaded with the chunk name `@<full path>`, because some addons find their
     own folder from `debug.getinfo(1).source`.
- **`require`**: `package.path` / `cpath` are the addon's folder, then its kind's `libs\`. An Ashita
  addon's libraries are VanaCore's own copies, never the player's Ashita: `addons\vanacore\libs\ashita-4.30`
  (Ashita 4.30's; `ashita-4.16` with `[addons] ashita.libs=4.16`: the 9 files that differ between them are `imgui`,
  `fonts`, `primitives`, `chat`, `scaling`, `d3d8`, `win32types`, `ffxi\dats`, `ffxi\dats\npcs`; and 4.30's additions:
  `ffxi\account`, `targets`, `macros`, `time`, `itemdata`, `memory\*`, `reflect`, `reflex`, `inheritance`),
  then `addons\vanacore\libs\ashita` (what both share, `bitreader` with it). 4.30's are a superset of 4.16's but
  for the ImGui 1.81 names 1.92 renamed or dropped (`ImGuiCol_TabActive`, `ImDrawCornerFlags_*`, `ImGuiKeyModFlags_*`,
  ...), which `compat/ashita.lua` defines before an addon runs, as ImGui 1.92's obsolete names have them, when it
  loads 4.30's. Then Ashita's addons root,
  `addons\ashita\?.lua` and `?\init.lua` (what `addons\?.lua` is on Ashita, so
  `require('bludex\\lib\\spellbook')` finds `bludex\lib\spellbook.lua`). Never the mirrored
  `addons\ashita\libs` (the player's Ashita's), for Lua or DLLs. Ashita's libraries (`common`,
  `settings`, `imgui`, `chat`, ...) and Windower's (`tables`, `strings`, `config`, `texts`,
  `resources`, ...) are the **real, unmodified files**.
- **Errors**:
  - Everything is called through `xpcall` with a traceback. The first line goes to the chat log
    (once while it repeats) and the full traceback to the log.
  - An addon whose file or `load` event fails isn't loaded.
  - An addon that keeps erroring **for 5 s** (errors less than 1 s apart; usually one error every
    frame) is unloaded. Shorter bursts are tolerated, because some addons fail for a moment while
    starting.
- **Crashes**: calls into Lua run inside an SEH `__try`. An access violation in an addon's `ffi`
  code unloads that addon and **abandons its `lua_State`**, which is never closed because it may
  be corrupt; the game carries on. LuaJIT's own error exceptions pass through the filter
  untouched.
- **Threading**: a lock plus a check that Lua is only ever entered from the game thread.

### 5.2 VanaCore's native API (`vana.*`)

This is the base every compatibility layer is written over. It deliberately exposes primitives
rather than Ashita's or Windower's shapes:

- **Events**: `vana.events.on(name, fn, key)` and `off`. Raised: `load`, `unload`, `command`,
  `present` (each frame), `mouse` (type = window message − 0x200, x, y, wheel delta, blocked;
  return true to keep it from the game) and `key` (wparam, lparam, blocked). With a table (`e.blocked`
  to block): `packet_in`/`packet_out`, `text_in`/`text_out`, `key_state`, `key_data`,
  `xinput_button`/`dinput_button` and `plugin_event`. `vana.on` and `vana.off` are the same, shorter. The game's own
  (`login`, `zone_change`, `job_change`, `main_menu_open`, `chat_input_open`, ...): see
  [docs/addons/02-events.md](../docs/addons/02-events.md).
- **Tasks**: `vana.tasks.once`, `oncef`, `repeating`, `repeatingf`, `spawn`, `cancel`. Tasks are
  **coroutines resumed from the frame loop**, and can `coroutine.sleep(seconds)` or
  `coroutine.sleepf(frames)`. This is what makes Windower's `coroutine.sleep` inside event
  handlers and Ashita's `ashita.tasks` work.
- **Memory**: `vana.memory.find(module or address, size, pattern, offset, count)`, the `read_*` /
  `write_*` functions for every integer size plus `float`, `double`, `string` and `array`,
  `alloc` / `free`, `protect` / `unprotect`, and `module(name)`. Reads of unreadable memory return
  0. Writes to code or read-only pages make the page writable for the write.
- **Game**: `vana.game.entity(index)`, `entity_count()`, `party()`, `player()` and `target()`,
  read fresh from `game.cpp`.
- **Chat**: `vana.chat.write(text, mode)` into the chat log, and `vana.chat.run(line, mode)` next
  frame.
- **UI**: `vana.ui.text{...}`, `rect{...}`, `image{...}` and `icon(name)`: VanaCore's own text
  boxes, drawn every frame in creation order. Colour codes are `\cs(r,g,b)` / `\cr`, and a box
  can be `block_mouse`.
- **Other**: `vana.config.get/set` (vanacore.ini), `vana.fs.*`, `vana.encoding.to_shift_jis` and
  `from_shift_jis`, `vana.time.clock`, `vana.system.open_url` and `play_sound`, `vana.paths`, and
  `vana.imgui` (5.5).

### 5.3 Ashita addons (`lua/compat/ashita.lua`)

Ashita v4's addon API, put over `vana.*`. The pattern throughout is a `partial(name, table)`
metatable: a member the Lua doesn't have is the SDK method on VanaCore's C++ object behind it
(every 4.16 method is bound), and a name neither has **reads nil, as an unknown name on
Ashita's own objects does**, logged once with the line that read it (`Player.isZoning isn't in
Ashita's API or VanaCore's: it reads nil (...)`). It used to raise "not supported yet", which
broke addons that run in Ashita: they feature-test (`player.HasPetCommand and ...`, XIUI's
`target.GetIsLockedOn ~= nil`, methods neither SDK has) and read fields that were never there
(XIUI's `player.isZoning` in nine modules). Items, abilities, spells and status icons do the
same for a field their structure lacks (the FFI would raise). The GUI manager is a plain table,
as Ashita addons test for ImGui functions by `if imgui.Foo then`.

The coverage tool follows the SDK's objects through the addon's own wrappers (XIUI's
`GetPlayerSafe()` in `libs/memory.lua`, aliased as a global in `handlers/helpers.lua`;
Sidekick's `pcall(function () return AshitaCore:... end)` getters) as well as `AshitaCore`
chains and locals, and lists the names read with a dot from them that aren't methods: an
audit that only follows literal `AshitaCore:` chains misses most of a large addon.

- **Globals**: `addon` (name, author, version, desc; the host fills `addon.name` and
  `addon.path`); `ashita.events.register(name, key, fn)` and `unregister` (event names mapped:
  `d3d_present` → `present`); `ashita.memory` (vana.memory + `dealloc`, `get_base`, `get_size`,
  `read_literal`); `ashita.fs` (`get_dir`, `get_directory`, ...); `ashita.tasks`; `ashita.time`;
  `ashita.misc`; `ashita.regex`; `ashita.bits` (`unpack_be`/`pack_be`, `unpack_le`/`pack_le`);
  `ashita.addons_version`, `ashita.interface_version`: everything the 4.16 Addons plugin's `ashita` has;
  `addon.instance` (`state`, `current_frame`, `get_memory_usage()`). With interface 4.30 also 4.30's:
  `ashita.memory.assembler` (`assemble`, `cave`, `release`, `call`, `jmp`, `nop`: asm_text.cpp, AsmJit
  with AsmTK's parser, as Ashita has them), the `AddonManager` global (`Count`, `Get(0..)`, `IsLoaded`,
  `GetState`, `GetAuthor`, `GetDescription`, `GetFileName`, `GetLink`, `GetVersion`, `GetMemoryUsage`,
  over the Ashita addons) and the module tokens `':ffximain'` and `':boot'` (the exe). The 4.30 list comes
  from Ashita 4.30's `addons/libs/annotations` and its Addons.dll, not from the addons we have.
- **`AshitaCore`** and its managers, the parts addons use:
  - **ChatManager**: `QueueCommand`, `AddChatMessage`, `Write`.
  - **ConfigurationManager**: the `boot` alias is `vanacore.ini`. `ffxi.registry` answers with
    the game's settings as VanaCore hands them to the game.
  - **MemoryManager**:
    - party, entity, target and player getters (`GetMemberName`, `GetDistance`,
      `GetTargetIndex`, `GetMainJob`, `GetLoginStatus`, ...), over `vana.game`;
    - **reads cached for the rest of the frame**, because Ashita addons call one getter per field
      per party member per frame and each is a protected copy;
    - `GetLoginStatus` returns 2 once the player's party slot and entity agree, else 0.
    - `GetMemberZone(0)` is the zone the server's zone-in packet (0x00A) named, from the end of
      that packet's handlers until the game's party list has it (`packets::player_zone`). The list
      only catches up once the zone has loaded, seconds later, so Ashita reads the zone left in
      between: zonename, which reads it a second after 0x00A, titled the old zone.
  - **GuiManager**: `vana.imgui` (5.5).
  - **PluginManager**: `Get('addons')` reports interface **4.16**. Addons written for both
    Ashita versions pick their ImGui 1.81 calls from that, and VanaCore has ImGui 1.81.
- `GetPlayerEntity()` / `GetEntity(index)` return **views over the game's own `entity_t`** (the pointer
  `IEntity::GetRawEntity` gives plugins, cast through the FFI to the structure declared in `ashita.lua`,
  684 bytes): every field reads live, `Name` comes back as a Lua string, nested `Movement`, `Look` and
  `Render` are the structures themselves, a write reaches the game (ceximodels and sexchange set
  `ModelUpdateFlags` and `Look.Hair`), and a field the structure doesn't have reads nil. A table copy
  (`entity_table`) is the fallback without the game's objects.
- `LogManager`, Ashita's global for the log, is the plugins' `ILogManager` object: `Log(level, source, text)`
  writes `[source] level: text` to `logs\vanacore.log` (1 critical to 5 debug, one above the log's level
  dropped), `Logf` formats first, `GetLogLevel`/`SetLogLevel` are the object's. Thorny's gdifonts, bundled by
  PetsReborn, HXUI, tCrossBar, fancychat and AMANTracker, calls it when a font the layout wants isn't installed.
- **Versions**: by `[addons] ashita.interface` (4.16, the default, or 4.30), as that interface's Addons
  plugin has them: `ashita.addons_version` 2.1 or 4.1, `ashita.interface_version` 4.16 or 4.3 (nil is
  Ashita 4.0's: tCrossBar then wants XInput triggers and sticks as 32-37), `GetPluginManager():Get('Addons'):
  GetInterfaceVersion()` 4.16 or 4.30. Addons test these to choose between Ashita 4.16 code (ImGui 1.81's
  calls, printf-style `Text`, item icons by `ImageSize`) and 4.3's (ImGui 1.92, `ImGuiChildFlags_*`,
  `ImageWithBg`). VanaCore's ImGui is 1.92 and takes either. With 4.30, `Text` and the
  other printf-style calls show text as given, and the SDK objects get 4.30's added methods (compat/ashita.lua's
  `SDK430`, called on the objects 4.30's plugins get, by their 4.30 vtable slots). The libraries are 4.30's
  whatever the interface (`[addons] ashita.libs`, under `require` above).
- **Events** also include `xinput_state` (`e.user`, `e.state`: the `XINPUT_STATE` the game is about to
  read, which XIUI's crossbar casts and reads). `dinput_state`, which XIUI also registers, isn't an
  Ashita event either (the Addons plugin's event names: command, d3d_beginscene, d3d_dip, d3d_dipup,
  d3d_dp, d3d_dpup, d3d_endscene, d3d_present, dinput_button, key_data, key_state, mouse, packet_in,
  packet_out, plugin_event, text_in, text_out, unload, xinput_button, xinput_state). The draw calls'
  (`d3d_dp`, `d3d_dip`, `d3d_dpup`, `d3d_dipup`) come from hooks on the game device's draw slots
  (overlay.cpp), only while an addon listens, not for what's drawn in Present. `d3d_beginscene` and
  `d3d_endscene` are VanaCore's `beginscene` and `endscene` (its argument: whether the scene is on the back
  buffer), raised at the game's own BeginScene (after it) and EndScene (before it). With an addon listening,
  ImGui's frame opens at the first back-buffer BeginScene (gui::begin_frame; Present's call is then a no-op),
  and a back-buffer EndScene is set up as Present's drawing is (text::prepare: the whole back buffer in the
  game's pixels; graphics::own_drawing for [ui] aspect), the game's state restored after.
- **String tables** come in two DAT formats: `d_msg` (abilities, spells, zones, ...) and the "dialog"
  format (`monsters.abilities` 7035/7034, `action.messages` 7027/7026: a u32 header whose low 28 bits
  are the size less 4, then every byte XOR 0x80, an offset table whose first entry over 4 is the count,
  NUL-terminated strings). Ashita's map of both is `config\ashita\ashita.datmap.ini` in its
  `Ashita-cfg.zip`.
- **Tools**: `tools/ashita_coverage.py` (the SDK methods addons call, and whether Lua, C++ or neither
  has each), `tools/ashita_deps.py` (everything else they touch: `ashita.*`, events and their fields,
  requires, `ffi.C` symbols, ImGui, pointer names, string tables, config, entity fields, undefined
  globals) and `tools/ashita_probe.py` (an addon that calls every SDK method they call in the running
  game and writes each result). Run the first two after any change to the layer; the probe when the
  game is up.
- **The per-character settings library works unchanged.** It keys files by
  `config\addons\<addon>\<name>_<id>\`, from the party getters and the install path, which is
  VanaCore's folder.

Not there yet: the `d3d8` library (device access and D3DX texture loading through `ffi`, used by
12 of the 34 ImGui addons tested), font objects (`FrameAnchor`), the inventory and
resource manager from Lua. LuaSocket's C half (`socket.core`, `mime.core`, v3.1.0) is built in for
every addon, under both kinds' `socket.lua` and `mime.lua`. LuaSec isn't: its `ssl.dll` has its own Lua
built in and crashes a LuaJIT state, so VanaCore doesn't carry it and never looks in the mirrored
`addons\ashita\libs` for it. `ssl.https` / `socket.ssl.https` are VanaCore's own over WinHTTP;
`socket.ssl` / `ssl` load as a LuaSec whose `wrap`, `newcontext` and `loadcertificate` return nil and
the reason (trustme loads; its login-campaign cipher fetch, which wraps its own socket, fails cleanly).

### 5.4 Windower addons (`lua/compat/windower.lua`)

- **`_addon`** and `windower` globals. `windower.addon_path` is the addon's folder, and
  `windower.windower_path` is `addons\windower\`.
- **Path remapping** for paths Windower's code builds for its own layout:
  - `<windower>\addons\x` becomes `addons\windower\x`;
  - `addon_path .. '../libs/'`, for an addon kept outside `addons\windower`, becomes
    `addons\windower\libs\`.
- **Events**: `windower.register_event(name, ..., fn)` returns ids, and there's
  `unregister_event`. Handlers can `coroutine.sleep`.
  - `load`, `unload`, `prerender` and `postrender` are raised directly.
  - `addon command`: `//<cmd> args` for any name in `_addon.command(s)` or the addon's name, and
    `//lua command <name> args`.
  - **`login`, `logout`, `zone change`, `job change`, `target change`, `status change` and
    `hp`/`mp`/`tp`/`hpp`/`mpp change` are derived by polling.** A snapshot of the game data is
    compared **frame to frame**, and an event is raised when it differs. It runs in every Windower
    addon, so it reads a dozen numbers (`vana.game.self`, no tables) and only while one of these
    events has a handler. VanaCore calls into an addon for the mouse and keys only once it listens
    for them. Windower raises these
    from packets; polling was simpler and good enough.
  - `mouse` (type, x, y, delta, blocked), which is how `texts` and `images` drag. A plain left press
    goes only to handlers not registered from `libs\texts.lua` or `libs\images.lua` (found by the
    handler's source); with Shift it goes to all, so library boxes drag with Shift + left drag only.
    Once an addon has a handler of its own, its primitives are `block_mouse` boxes: window.cpp's
    low-level mouse hook takes a press over one (and its release) before the game can see it, so a
    drag of the addon's window never reaches the game.
  - Text objects are VanaCore text boxes on `layer` 1, primitives on 0: Windower draws every text
    object after every primitive, and addons rely on it (TrustSupport makes a button's hover and
    disabled looks after its label).
  - `keyboard` (dik, pressed, flags, blocked) for each key's press and release; a handler returning
    `true` keeps it from the game.
  - `action` (the table `windower.packets.parse_action` makes) for each incoming 0x028, and
    `action message` (actor_id, target_id, actor_index, target_index, message_id, param_1, param_2,
    param_3 = 0) for each 0x029. Both are raised from VanaCore's `packet_in`, after the addon's
    `incoming chunk` handlers, and not for a blocked packet. An effect an action lacks reads 0.
  - `gain buff` / `lose buff` (id) by comparing the player's buff list (counts per id) frame to frame,
    and `time change`, `day change`, `moon change` (phase ids) and `weather change` (new, old) from
    what `get_info` reads, compared frame to frame. Both only while something listens.
  - **Vana'diel clock**: LandSandBoat's `CVanaTime`: minutes = (unix - 1009810800) × 25 / 60 +
    886 × 518400; `time` is that mod 1440, `day` the day mod 8 (0 Firesday). The moon cycle is
    (day + 26) mod 84: 100% at 0, 0% at 42, and the 12 phases are 7 days each with Full Moon (6)
    centred on 0. The Unix time is the server's (each incoming buffer's header +8, kept by
    `compat/windower/state.cpp`, which also keeps the weather from 0x00A/0x057), through
    `vana.game.world()` (server_time, weather, chat_open), or the PC's when it's off by over a day.
  - `unhandled command` (command, args...): `addons.cpp` raises `unhandled` (e.command) for a `//`
    line no addon or Windower plugin took; a handler returning true keeps "Unknown command" away.
    `windower.lua` takes it whenever an addon listens.
  - `ipc message` (message): `send_ipc_message` goes through `vana.ipc.send` (`ipc.cpp`) to the
    addon of the same name in every other VanaCore instance in the Windows session: a ring of 128
    slots of up to 8000 bytes in shared memory (`Local\VanaCore.IPC.1` plus its mutex), read once
    a frame and raised as `ipc` (e.message). Not to the sender's own instance.
- **Tool**: `tools/windower_coverage.py <addons folders> [--definitions <Windower/Lua's
  definitions/windower.lua>]` lists the `windower.*` names, events and requires that addons (and the
  libraries they require) use and VanaCore lacks, has only as a stub, or can't load, plus the
  definitions' result fields `windower.lua` never sets. It runs `tools/windower_dump.lua` through the
  luatest runner to see what `windower.lua` makes. The last run is `docs/windower-coverage.md`.
- **`windower.ffxi`**: `get_player`, `get_party`, `get_party_info`, `get_info`,
  `get_mob_by_target` (`t`, `st`, `me`, `pet`, `p0`–`p5`, `a10`–`a25`), `get_mob_by_index`,
  `get_mob_by_id`, `get_mob_by_name` and `get_mob_array`, in Windower's table shapes.
  `get_abilities`, `get_spells`, `get_key_items`, `get_ability_recasts`, `get_spell_recasts` and
  `get_items` read the game's own data: the abilities' bits are packet 0xAC's (weapon skills, job
  abilities, pet commands and traits, 512, 512, 512 and 256 of them; Windower's ids are the bit,
  the bit less 512, and the bit less 1536 for traits).
- **Primitives** (`windower.prim.*`, which Windower's `images` library drives) are VanaCore boxes:
  a rectangle of its colour, or with a texture the picture tinted by its colour (its opacity
  drawn with it), fit to the texture or at its size, repeated across it.
- **Text objects** (`windower.text.*`, which Windower's `texts` library drives) are VanaCore
  text boxes. **Sizes are in points**, so convert to pixels.
- **Chat and commands**: `add_to_chat`, and `send_command` with Windower's console syntax.
- **Text**: `incoming text` (original, modified, original_mode, modified_mode, blocked) for every line coming into
  the chat log, and `outgoing text` (original, modified, blocked) for every line going to the game that no command
  took. A handler returning `true` blocks the line, one returning a string changes it. (Ashita addons have the same
  as `text_in`/`text_out`, with Ashita's event tables: `mode`, `indent`, `message`, and their `_modified`.)
- **Packets**: `incoming chunk` and `outgoing chunk` (id, original, modified, injected, blocked), on
  VanaCore's `packet_in`/`packet_out`, which an addon only listens for once it registers one of
  these. A handler returning `true` blocks the packet, one returning a string changes it to that
  (the handlers after see it as `modified`). `windower.packets.inject_incoming`/`inject_outgoing`
  (`vana.packets.inject`) and `last_incoming`/`last_outgoing` (`vana.packets.last`: the last packet
  of each id each way, kept whether anything listens or not).
- **`pack`** (`require('pack')`, and `string.pack`/`string.unpack` from the start): Windower's
  LPack, as its `packets`, `extdata` and `slips` libraries need it. Its own codes besides LPack's:
  `b<n>` an n-bit number, `q<n>` n one-bit booleans, `B` a boolean byte, `C` an unsigned byte,
  `S<n>` a fixed-length string, `A*`/`S*`/`H*` the rest. Bits fill each byte from its lowest bit,
  any other code starts at the next byte. `unpack(data, format, [byte], [bit])` starts at that byte
  and bit (1-based) and **returns the values alone**, unlike LPack (no position first). Packing
  makes `A<n>` and `S<n>` exactly n bytes. `tools/luatest/run.cmd` checks it byte for byte, then
  builds every packet Windower's `packets` library knows (204) with a value in each field and
  parses it back.
- **Other**: `get_windower_settings` (the back buffer's size), files, `wc_match`, Shift-JIS,
  `coroutine.schedule` and `coroutine.close`.

**LuaJIT had to be patched** (`third_party/luajit-vanacore.patch`), because Windower's Lua (a
modified 5.1) accepts syntax that standard Lua rejects:

- `'%d':format(n)`: method calls on string literals;
- `{a, b}[i]`: indexing a table constructor;
- `function() ... end()`: calling a function expression;
- unary `+`;
- unknown escapes in strings, which mean the character itself (Lua 5.1 behaviour);
- a lone `;` statement.

With the patch, all 1,054 Lua files in the Windower and CatsEyeXI Ashita addon folders parse.

Not there yet: Windower's events derived
from packets (`action`, `action message`, `incoming text`, ...), and its native Lua modules
(`_XIPivot.dll` and the like, which link against `LuaCore.dll`).

### 5.5 ImGui for Lua (`lua_imgui.cpp`)

Ashita's `libs\imgui.lua` wraps `AshitaCore:GetGuiManager()`, and VanaCore makes that
`vana.imgui`, a hand-written plus generated binding (250 functions generated from the SDK) that
follows **Ashita's Lua conventions exactly**. Getting these wrong breaks addons silently.

- **Pointers are tables.** `imgui.Checkbox('x', t)` reads and toggles `t[1]`; an array argument
  is `t[1]`..`t[N]`. A `p_open` that isn't a table means none.
- **Vectors**: `ImVec2` and `ImVec4` go **in** as `{x, y}` tables and come **out** as multiple
  numbers (`local x, y = imgui.GetWindowPos()`).
- **Live objects**: `GetIO()`, `GetStyle()`, `GetFont()` and `TableGetSortSpecs()` are live
  objects (`io.DisplaySize.x`, `style.Colors[ImGuiCol_Text + 1]`, **1-based**).
  `GetWindowDrawList()` is an object with the `Add*` methods.
- **Colours** are ImU32 numbers, **including negative ones**, which is what `bit.bor` produces.
  Textures are their pointer as a number.
- **printf-style functions** (`Text`, `TextColored`, `SetTooltip`, ...) take the format string
  alone, so `%%` prints `%`.
- **Arguments**: missing or nil ones take ImGui's defaults, and extra ones are ignored.
  Overloads are chosen by the arguments' Lua types.
- **A function ImGui doesn't have is nil**, never an error, because addons feature-test.

---

## 6. Native plugins

### 6.1 The front end (`plugins.cpp`)

- **Folders**: `plugins\ashita\<name>.dll` and `plugins\windower\<name>.dll`. Ashita's folder is
  searched first unless the kind is named. The commands are `/plugin load`, `unload`, `reload`
  and `list`.
- **Refused Ashita plugins**, each with its reason:
  - `Addons`: Ashita's own Lua host, whose job VanaCore does.
  - `Sequencer`: it finds what to patch by pattern-scanning Ashita's own packet code, which
    doesn't exist here.
  - The launcher also skips `Pivot` and `VanaView`, because VanaCore has both built in.
- A plugin's `GetInstallPath()` (Ashita) or `WindowerPath` (Windower) is VanaCore's folder. Their
  files therefore land in `config\plugins\<name>\`, `resources\<name>\` and so on, just as under
  Ashita.

### 6.2 The Ashita v4 plugin host (`compat/ashita/`)

#### 6.2.1 Loading

1. `LoadLibraryExA(path, LOAD_WITH_ALTERED_SEARCH_PATH)`, so the plugin's own dependencies
   resolve beside it.
2. Exports: `expGetInterfaceVersion` (a `double`, 4.16 or 4.30), `expCreatePlugin(const char* args)`
   and, optionally, `expDestroyPlugin`.
3. Pick the object set for that version (`Links`: one complete set of manager objects per
   interface version).
4. Call `GetName`, `GetAuthor`, `GetVersion`, `GetFlags` and `GetPriority`, then insert the plugin
   in **priority order** (lower first; the same priority stays in load order).
5. `Initialize(IAshitaCore*, ILogManager*, uint32_t id)`, with `id` = the module handle as Ashita
   passes it.
6. If the device already exists and the plugin's flags ask for Direct3D, `Direct3DInitialize`
   straight away.
7. Unload runs `Release()`, frees the plugin's fonts, primitives and input callbacks, calls
   `expDestroyPlugin` (or `delete`), then `FreeLibrary`.

#### 6.2.2 The ABI: generated from the SDK

Ashita plugins are C++ objects calling C++ objects: **every call goes through a vtable compiled
by MSVC against Ashita's SDK headers.** VanaCore's objects must therefore have the SDK's virtual
functions in the SDK's order, with the same parameter and return types as MSVC passes them:

- `__thiscall`, with `this` in ecx;
- structs returned by hidden pointer;
- `float` and `double` returned on the x87 stack;
- `bool` as 1 byte.

Hand-writing this for ~1,300 methods × 2 versions is hopeless, so `tools/gen_ashita_abi.py` reads
the SDK headers of **both** versions and writes `abi.hpp`:

- the interfaces, pure virtual as in the SDK (`IAshitaCore`, `IChatManager`, `IMemoryManager`,
  `IEntity`, `IParty`, `IPlayer`, `ITarget`, `IInventory`, `IResourceManager`, `IFontManager`,
  `IPrimitiveManager`, `IGuiManager`, `IKeyboard`, `IMouse`, ...), in `sdk::v416` and
  `sdk::v430`;
- the SDK's data structs (`IItem`, `IAbility`, `ISpell`, `IStatusIcon`, `Ashita::FFXI::*`);
- for each interface, a class template `<Name>Object<Impl>` implementing every method. A method
  calls `Impl::<Method>(context, args...)` **if `Impl` has a matching static function** (checked
  with a C++20 `requires` expression); otherwise it calls `sdk::unimplemented("IFoo::Bar(...)")`,
  which logs **once per method** which plugin called it, and returns zero, false or nullptr.

The backend `.cpp` files then only define `Impl` structs of static functions for what's
implemented (`struct ChatImpl { static void Write(void* c, ...); ... }`), and build the objects
with `make<v416::ChatManagerObject, ChatImpl>(&links416)`. The `context` is the version's
`Links`: a table of all that version's objects, so `GetChatManager()` can return the right
version's object.

This design means **every method always exists and is safe to call**, and the log tells you
exactly what a plugin needed that isn't implemented yet. That's how the plugin survey worked: load
each plugin in a test process and read which `unimplemented` lines it produced.

`gen_ashita_abi.py --test` also generates a **test plugin built against the real SDK** that calls
every one of the 1,275 (4.16) and 1,328 (4.30) methods, and checks each lands in the right `Impl`
with the right arguments and the stack intact. That test caught overload-ordering and
struct-return mistakes that would otherwise have been silent memory corruption.

The differences between 4.16 and 4.30 are real:

- 4.30 adds and removes methods in `IEntity`, `IParty`, `IPlayer` and `ITarget`;
- it changes key binds and packet queueing;
- it wraps **ImGui 1.92** instead of 1.81.

Plugins are called through 4.30's `IPluginBase` declaration, because its slots are identical in
both versions; the test checks that too.

#### 6.2.3 Calling into plugins safely

Every call into a plugin goes through `call_plugin(plugin, "what", lambda)`:

- `g_calling` is set to the plugin's name. Fonts, primitives and callbacks it creates during the
  call are **owned** by it, and freed when it unloads.
- The call runs inside SEH `__try` / `__except`:
  - **A C++ exception** (code `0xE06D7363`) that escapes the plugin has already been unwound, as
    Ashita's own catch would have done. VanaCore reports `what()` (read from the exception record
    with MSVC's throw info) and **the plugin carries on**.
  - **Any other fault** (an access violation, ...) **disables the plugin permanently**. It's never
    called again and its DLL stays loaded, since freeing a faulted module is unsafe. The game
    carries on, and the chat log says which plugin crashed, where (module + offset) and why.
- A plugin can ask to unload (itself or another) during a call. Unloads are deferred until the
  outermost call returns (`g_depth`) and processed then.

#### 6.2.4 The managers, and what each is built on

| Manager | Implementation |
|---|---|
| `IAshitaCore` | Install path (VanaCore's folder, trailing `\`), the D3D8 device, every manager, and the Windows calls Ashita forwards (mutex, window and registry helpers) |
| `IChatManager` | `Write`, `Writef` and `AddChatMessage` go to the chat log writer (2.2). `ParseCommand` runs through the command path now, `QueueCommand` next frame, and `ExecuteScript` runs `scripts\`. The input line's text, length, caret and open state come from 2.3 |
| `IConfigurationManager` | INI files under `config\` by alias. `boot` is vanacore.ini (read-only), and `ashita.language` its `[language]`. A value a plugin sets (`SetValue`) is written to the alias's file there and then, on its own line with the file's layout and comments kept (`ini::set`), as Ashita's does: Minimap sets each change that way and never calls `Save`. `Save` with no file named writes the file the alias was loaded from |
| `ILogManager` | Into VanaCore's log as `[plugin] level: message` |
| `IPointerManager` | **Ashita's own pointer table** (`config\ashita\ashita.pointers.ini` in an Ashita install: `chat.parseinputtext`, `packets.decryptbuffer`, `entitymap`, `inventory`, `player.loginstatus`, ...). These are the same patterns, modules and offsets, compiled into VanaCore and scanned on request. Plugins also add their own patterns |
| `IOffsetManager` | Ashita's offsets table, the same way |
| `IMemoryManager` | `IEntity` (all 268/270 getters and setters), `IParty` (with status icons), `IPlayer` (stats, skills, job points, buffs, key items, known spells, job levels via the game's own functions), `ITarget` (and the target window, the game's `g_pTkTarget`), `IRecast` (the abilities' 31 timer slots and the spells' recasts, in 1/60 s), `IInventory` (containers, treasure pool, equipment, check info), `IAutoFollow` and `ICastBar`. **All read and written in place** in the game's structures (2.5), through the SDK's struct layouts |
| `IResourceManager` | Items, abilities, spells, status icons and string tables from the DATs (6.2.6); entities by index and id; DAT paths by file id. `GetTexture` isn't done yet |
| `IFontManager` / `IPrimitiveManager` | Font objects are drawn by VanaCore's text renderer: position, font, height, colours, outline, background, right justification, parents and **anchors** (`anchor`: the corner of its own box the position goes to; `anchor_parent`: the corner of the parent, or the screen, it counts from). A background that keeps a size of its own (`auto_resize` off), has a texture or a border is a box of its own under the text, put where the font is each frame (its position and `HitTest` say so), and is drawn for a font with no text too when it keeps a size of its own (Points' compact bar is such a box); otherwise it's painted round the text at the text's size. D3DX reads Targa (`.tga`) images as well as what WIC does. Primitives are filled or textured rectangles (a texture stretched over it, tinted by its colour, moved by its offset), with their borders. Shift + left drag moves a font that can be focused and isn't locked, as Ashita does. Objects are owned by their plugin (6.2.3) |
| `IGuiManager` | Dear ImGui (6.2.7) |
| `IPacketManager` | `AddIncomingPacket` / `AddOutgoingPacket` go to 4's queue. `QueueOutgoingPacket` puts one in the game's own outgoing queue (Ashita's `packets.queuepacket1` for a slot, the callback fills it, `queuepacket2` seals it) |
| `IPluginManager` | Load, unload and get plugins, and `RaiseEvent` → every plugin's `HandleEvent` (plugins talk to each other this way). There's no POL plugin manager, because PlayOnline isn't running |
| `IInputManager` | `IKeyboard` and `IMouse` callbacks (6.2.9), key binds, `IController` and `IXInput` callbacks (each read, and each change of a control to change or block; `QueueButtonData`, `SetTrackDeadZone`), and the keyboard's scan code and virtual key conversions |

Plugin events VanaCore raises:

- `HandleCommand`;
- `HandleIncomingPacket` and `HandleOutgoingPacket`;
- `Direct3DInitialize`, `Direct3DBeginScene`, `Direct3DEndScene` (the game's scenes, with whether they draw on the back
  buffer; Minimap builds its map texture in the first, so without it the minimap never drew) and `Direct3DPresent`;
- `HandleEvent`.

Not raised yet:

- `HandleIncomingText` and `HandleOutgoingText` (the chat log text hooks);
- `Direct3DSetRenderState` and the draw-call callbacks.

One consequence: the VanaView plugin's fog, which relies on `Direct3DSetRenderState`, never
worked, so VanaCore reimplemented VanaView natively.

#### 6.2.5 Game structures in the SDK's layouts

The SDK declares FFXI's structures (`entity_t`, `partymember_t`, `targetwindow_t`, the inventory
item and so on). For the CatsEyeXI client they match memory, so `IEntity::GetName(index)` is
`entity_array[index]->Name`. **Validate the layouts against your build** before trusting them.
Private-server clients differ in places: CatsEyeXI's key item table is 4,096 bits where retail's
is smaller, which Ashita's own pattern assumes. Accessors are generated per field with a macro
over the member path (e.g. `Movement.LocalPosition.X`).

#### 6.2.6 Resources from the DATs (`resources.cpp`)

Ashita reads resources from the game's DATs, listed in its `ashita.datmap.ini`. VanaCore does the
same, lazily, on the first call, and caches the result for the process's life. `dats.cpp` maps a
file id to a path (`VTABLE.DAT` gives which ROM, `FTABLE.DAT` or `ROMn\FTABLEn.DAT` gives the
u16 `dir << 7 | file` → `ROMn\dir\file.DAT`).

- **Items** come from file ids 73–77, 91 and 55667–55671 (English), and 4–9 and 55547–55551
  (Japanese).
  - Records are **0xC00 bytes (legacy) or 0x1400 bytes (September 2026 retail)**. Tell them apart
    by which divides the file and by the 0xFF terminator at each record's end.
  - Each byte is **rotated right by 5**: `b = (b >> 5) | (b << 3)`.
  - Then there's a common header (id, flags, stack, type, resource id, targets), a layout per
    item kind at offsets that shift between the two formats, a string block (u32 count, then
    `{u32 offset, u32 kind}` pairs; each string starts 0x1C after its offset), and the icon at
    +0x280 (u32 size, then the bitmap).
- **String tables** (`d_msg` files: `abilities.names`, `spells.names`, `keyitems.*`, `zones.*`,
  `jobs.*`, ... as Ashita's datmap lists them):
  - The header is `d_msg`. When the u16 at +0x0A is 1, **every byte after the 0x40-byte header is
    XOR 0xFF**.
  - Entries come either at a fixed stride or through an index of `{offset, size}` pairs.
  - Each entry is a block of sub-strings. Text sub-strings (type 1) start 0x18 after their
    marker.
  - Key item tables are keyed by the first sub-string's value, which is the key item id, not by
    entry index.
- **Abilities and spells** (Ashita's `spdata`, file id **81**, `ROM\118\114.DAT`):
  - It's a **menu DAT**: chunks, each a 4-byte name then a u32 with the type in the low 7 bits
    and the size ÷ 16 in the rest, including the 16-byte chunk header.
  - `comm` holds abilities (0x30-byte records, by ability id: weapon skills 1–255, job abilities
    from 0x200) and `mgc_` holds spells (0x64-byte records, by spell index). The records are
    exactly the SDK's `IAbility` and `ISpell` up to their end-of-entry byte.
  - **Each record is scrambled with POLUtils' `DecodeDataBlockMask`.** Compute
    `n = |popcount(r[2]) − popcount(r[11]) + popcount(r[12])| % 5`, then rotate **every byte of
    the record right** by `{7, 1, 6, 2, 5}[n]`. We confirmed it against every record: the decoded
    id equals the record index.
  - Names and descriptions come from the `abilities.*` and `spells.*` string tables at the same
    index. `"."` marks an unused slot.

#### 6.2.7 ImGui (`gui.cpp`, `compat/ashita/gui.cpp`)

Interface 4.16 wraps **Dear ImGui 1.81 WIP at commit 6487860a**. The SDK's `imgui.h` is that
version's, and plugins pass ImGui structs (`ImVec2`, `ImGuiStyle`, `ImFontAtlas`, ...) by pointer
and by value.

VanaCore therefore **builds in exactly that commit** and verified the layouts: 291 sizes and
offsets compared against the SDK's copies. `IGuiManager`'s 371 methods are one-line forwarders to
ImGui's own functions, generated by `tools/gen_ashita_gui.py` (`gui_v416.inc`), which
reinterprets the SDK's types as ImGui's.

Rules:

- **One ImGui context** is shared by VanaCore, every plugin and every addon.
- A plugin can't destroy that context or swap ImGui's allocators; it's logged instead.
- Plugins built for 4.30 wrap ImGui 1.92, which VanaCore doesn't have, so their windows don't
  show. The chat log says so, once per plugin.
- **Fonts, as Ashita ships them**: Agave at 18 (the default), 24 and 32 px, each with Font
  Awesome 6's regular, brands and solid icons merged in. Fonts that plugins and addons add go into
  the same atlas, rebuilt before the next frame.
- **ImGui misuse must not crash the game.** VanaCore makes three guarantees:
  - `IM_ASSERT` is redefined to log once per assertion and continue;
  - at the frame's end, anything left open is closed (a `Begin` without its `End`, pushed styles
    never popped) and that's logged;
  - drawing functions do nothing outside a frame, such as from an addon's command handler.
- **Input capture**:
  - A press or wheel over an ImGui window is ImGui's alone. It's removed from the window messages
    **and from DirectInput's mouse reads** (all button and wheel data zeroed while it's ImGui's),
    because FFXI reads the mouse through DirectInput. Ashita addons' `mouse` events don't hear it
    either (nor the release of a press ImGui took): their handlers don't check `e.blocked`
    (Ashita's fonts and primitives libraries, equipmon, timers), so a click or wheel turn on a
    window over their boxes would reach the boxes. The addon whose window it is still hears it:
    fancychat scrolls its chat window by the `mouse` event's wheel, not ImGui's. Windower's and
    VanaCore's addons hear it as blocked.
  - While an ImGui text box has the keyboard, keys and characters go to ImGui, and the game's
    DirectInput keyboard reads **no presses**. Releases still go through, so a key held before
    typing started doesn't stick.
  - The IME window is placed at the text box.

#### 6.2.8 Resources the plugins expect on disk

Plugins expect Ashita's folders to exist:

- `config\plugins\<name>\`;
- `resources\` (for example `resources\findall\slips.xml`);
- `polplugins\DATs\<overlay>\` for cexidats.

The CatsEyeXI launcher mirrors Ashita's `plugins`, `addons`, `resources` and `config` into
VanaCore's folder.

Plugins that ship with VanaCore itself (CatsEyeXI's cexidats and cexislots) are in `plugins\vanacore\`: the build
replaces them on every update, the mirror never touches that folder, and `/plugin load <name>` finds
a plugin there before `plugins\ashita`. `release-vanacore.bat` signs them with VanaCore's own files, so Smart App
Control loads them (it refuses an unsigned DLL it hasn't seen widely: "Bad Image", 0xc0e90002). cexidats and cexislots are told the install path is the folder
whose `polplugins\DATs` is the pivot root (Ashita's, under the launcher), where they look for the
overlays themselves; cexidats 1.17 reads its DATs through the game's own path instead.

#### 6.2.9 Input callbacks (`IKeyboard::AddCallback`, `IMouse::AddCallback`)

The typedefs are **raw `__stdcall` function pointers**, not `std::function`:

```
HRESULT getdevicedatacallback_f(IDirectInputDevice8A*, DWORD cbObjectData, DIDEVICEOBJECTDATA* rgdod, DWORD* pdwInOut, DWORD capacity, DWORD flags)
HRESULT getdevicestatecallback_f(IDirectInputDevice8A*, DWORD cbData, void* lpvData)
BOOL    keyboardcallback_f(WPARAM vk, LPARAM keystrokeFlags, bool blocked)
BOOL    mousecallback_f(uint32_t msg, WPARAM wParam, LPARAM lParam, bool blocked)
```

They're registered by **alias**. Adding under an existing alias replaces the callback. Each is
stored with its owning plugin, called as that plugin (guarded), and removed when it unloads.
Semantics, as we worked them out:

- **`getdevicestatecallback_f`** is called **after** the real `GetDeviceState` on the keyboard,
  with the 256-byte key array. The plugin may set keys to 0x80 (held). Thirdparty does exactly
  this for keys that external programs send through Windower's `WindowerMMFKeyboardHandler_<pid>`
  memory-mapped file.
- **`getdevicedatacallback_f`** is called **after** the real `GetDeviceData`. `*pdwInOut` is the
  count returned and **the 5th argument is the buffer's capacity**: the original `*pdwInOut`
  before the call. We found that from Thirdparty's code: it appends events at
  `rgdod[count] .. rgdod[capacity-1]` and updates `*pdwInOut`. The 6th argument is `dwFlags`.
  Skip the call when `rgdod` is null, which is a count query.
- **`keyboardcallback_f`** gets each key's **window message**: WM_KEYDOWN, WM_KEYUP and the
  WM_SYS forms, with the virtual key and the message's `lParam`. That `lParam` has the same bit
  layout as a `WH_KEYBOARD` hook's (repeat count, scan code, extended bit, context, previous
  state, transition). Return TRUE to block. **A blocked key down must also be hidden from
  DirectInput** until it's released, otherwise the game still acts on it.
- **`mousecallback_f`** gets each mouse window message over the game window, with `lParam` =
  `MAKELPARAM(x, y)` **in the game's pixels**, which is where the plugin draws. Return TRUE to
  block (Minimap's drag and wheel zoom). Minimap imports neither `ScreenToClient` nor
  `GetCursorPos`, so the coordinates must already be right.

Order: plugins' DirectInput callbacks run first, then VanaCore's own filtering (ImGui capture,
hidden keys). Typing in an ImGui box therefore still wins.

### 6.3 The Windower plugin host (`compat/windower/`)

#### 6.3.1 The interface, reverse engineered

Windower never published its plugin interface. VanaCore's `abi.hpp` was reconstructed from
`Hook.dll`'s classes: their RTTI and vtables, what each slot reads and writes, how `LuaCore.dll`
calls them, and how real plugins (FFXIDB, StatusTimer, Timers) call them. The rules:

- **Every method is a virtual `__stdcall` member with `this` on the stack**, not `__thiscall`.
  The exceptions are the scalar deleting destructors, which are `__thiscall`.
- **Give every virtual a distinct name.** MSVC groups overloaded virtuals in its own order, which
  silently reorders slots.

Exports: `GetInterfaceVersion()` returns **0x04070300** (Windower 4), and `CreateInstance()`
returns `Plugin*`.

`Plugin` (the plugin's object; the slot number follows each entry):

- `GetAuthor` (0) and `GetName` (1).
- `Load(PluginManager*)` (2), and `Unload()` (3), which deletes itself.
- `IgnoreUnload` (4).
- `PreRender` (5) and `PostRender` (6).
- `PluginCommand(" args")` (7): note the **leading space**.
- `UnhandledCommand(line)` (8), returning bool.
- `IncomingText` (9) and `OutgoingText` (10), whose arguments aren't known yet.
- `IncomingChunk(original, modified, injected, blocked)` (11) and `OutgoingChunk` (12), returning
  bool to block. They read 0x200 bytes of each buffer.
- `Mouse(type, x, y, delta, blocked)` (13), returning bool to block. Type = message − 0x200 and
  the coordinates are screen pixels.
- `Keyboard(?, ?, blocked)` (14), returning bool to block. Its two arguments aren't known (the plugins seen
  so far ignore them): VanaCore passes the key's DirectInput code and 1 down, 0 up, as Windower addons'
  `keyboard` event has them.
- `AddItem` (15) and `RemoveItem` (16).
- `DeletingDestructor` (17).

`PluginManager` (the host's object):

- `GetSettings()` returns a `Settings` struct: profile, paths, resolution and flags. `WindowerPath`
  is at +0x2150, and it's where plugins find `plugins\settings\` and their own folders.
- `GetHWND`, `GetDirect3DDevice`, `GetConsole`, `GetTextHandler`, `GetPrimitiveHandler`,
  `GetPacketStreamHandler` and `GetFFXI`.

The rest:

- **`Console`**: VanaCore has no console, so `Write` goes to the chat log. A line's `\cs(r,g,b)`
  becomes the nearest chat log colour. `SendCommand` runs a command.
- **`TextHandler` / `TextObject`** (21 setters and getters; see `abi.hpp`):
  - text is UTF-8 with `\cs(r,g,b)` and `\cr`;
  - colours are passed as `(a, r, g, b)` bytes;
  - font sizes are in **points**;
  - `SetExtents` with negative values means fit the text;
  - `SetBgBorderSize` is padding on every side;
  - `SetRightJustified` makes x the right edge.
- **`PrimitiveHandler` / `PrimitiveObject`**: a colour, or an image file or module resource,
  tinted, fitted or tiled.
- **`PacketStreamHandler`**:
  - `InjectOutgoing` / `InjectIncoming` take whole packets, header included.
  - `GetLastOutgoing` / `GetLastIncoming(id, &time)` return the last packet of an id, which must
    stay valid (0x208 bytes).
  - `GetStreamState()` returns a table of **14 pointers into Windower's packet-derived game
    state** (6.3.4).
- **`FFXI::AddToChat(text, mode)`**.

#### 6.3.2 Frame order and device state

Each frame, **inside the frame's scene**, which Windower plugins such as FFXIDB assume when they
draw on the device:

1. VanaCore captures the device state (`CreateStateBlock(D3DSBT_ALL)`).
2. Each plugin's `PreRender` runs. FFXIDB draws its minimap on the device here.
3. The captured state is applied back, because plugins leave render states and textures changed.
4. VanaCore draws the Windower text and primitives, in creation order.
5. Each plugin's `PostRender` runs.

Exceptions: same policy as Ashita. A C++ exception is reported and the plugin carries on (FFXIDB
throws on `//ffxidb pos o 1`); a real fault disables the plugin.

#### 6.3.3 File redirection

Windower keeps plugins' files next to them in `<Windower>\plugins\`: `settings\`, `resources\`
and a folder per plugin. In VanaCore, Windower plugins live in `plugins\windower\`, but they build
paths from `WindowerPath + "plugins\"`.

VanaCore therefore **IAT-hooks the file APIs in each Windower plugin's module**, as the DLL loads
and before its code runs. Any path under `<VanaCore>\plugins\` becomes
`<VanaCore>\plugins\windower\`. The hooked functions:

- `CreateFileA` / `CreateFileW` / `CreateFile2`;
- the `GetFileAttributes(Ex)` functions;
- `FindFirstFile`;
- `CreateDirectory` and `RemoveDirectory`;
- `DeleteFile`, `MoveFileEx` and `CopyFile`;
- `SetFileAttributes`.

Windower 3 plugins use the **Visual C++ 2008 runtime**, whose `fopen`, `fopen_s` and
`ifstream`/`ofstream`/`fstream::open` (and constructors) open files through the CRT's own imports.
VanaCore hooks the plugin's imports of those CRT functions instead. The `__thiscall` members are
hooked as `__fastcall` stand-ins: `this` in ecx, edx unused.

#### 6.3.4 The game-state block (`state.cpp`)

Windower's `Hook.dll` keeps a big struct (0x1D4A8 bytes) updated from the server's packets, and
plugins read it through `GetStreamState()`'s 14 pointers:

1. zone/login;
2. server clock;
3. player (id, index, name, jobs, HP/MP, buffs);
4. merits;
5. key items;
6. items (32 bags × 81 slots × 0x28);
7. treasure pool;
8. equipment indices;
9. party;
10. job extra data;
11. equipment bags;
12. job points;
13. missions;
14. jobs.

VanaCore keeps **the same block, laid out the same way** (offsets in `state.cpp`), filled by
reimplementing Hook's handling of **22 incoming packets**:

- 0x00A (zone in), 0x00B (zone out), 0x01B (job info), 0x01C (bag sizes);
- 0x01E, 0x01F and 0x020 (item count, assign and update);
- 0x037, 0x044, 0x050, 0x055, 0x056, 0x057, 0x061, 0x062, 0x063;
- 0x08C, 0x08D, 0x0D2, 0x0D3, 0x0DF.

It runs from the first buffer, whether a plugin is loaded or not, so a plugin loaded mid-session
sees full state.

Not done: Hook's extras beyond the block. It calls plugins' `AddItem` / `RemoveItem`, times three
buffs (the words at player+0x136), and asks the server for an update after zoning.

#### 6.3.5 Windower 3 plugins (`legacy.cpp`)

Their interface is `0x0004xxxx`; Windower 4 itself refuses these. Only what StatusTimer uses is
known, read from its code:

- the plugin's 14 slots (argument counts from each slot's `ret n`);
- `GetInfo`, which fills an info block with the author at +0x100 and the command names at +0x308
  (`"st|statustimer"`);
- the host objects' slots StatusTimer calls;
- the settings block, 0x10048 bytes, with a wide `WindowerPath` at +0x4000 and an English flag at
  +0x10004.

Unknown host slots log that they were called and return. Their text objects are Windower 4 text
objects underneath.

---

## 7. Input: what "the game doesn't see it" requires

FFXI reads the keyboard and mouse through **DirectInput** (`GetDeviceState` and `GetDeviceData`),
the button states through `GetAsyncKeyState` and `GetKeyboardState`, and the cursor through
`GetCursorPos`. Window messages drive only the chat line's characters and a few other things.
Any overlay that takes input (ImGui, VanaCore's own window, plugin callbacks, dragging a font)
must therefore do three things:

1. **Swallow the window message.**
2. **Blank it in DirectInput's reads** while it's taken:
   - for the mouse, zero the buttons and wheel in the state struct and drop those events from
     the buffer, but **keep X and Y** so the game's cursor still follows the mouse;
   - for the keyboard, hide a specific key until a read shows it released, or drop presses
     while a text box has focus.
3. **Treat a press that started over the overlay as the overlay's until release**, even if the
   cursor leaves it.

A press on the window's frame (moving or sizing it) is also blanked, otherwise dragging the window
turns the camera. The low-level mouse and keyboard hooks run on **their own thread**: on the game
thread they made the whole PC lag while the game was loading.

---

## 8. Other lessons that cost time

- **Pattern-find everything, lazily, the first time it's needed.** Many globals are null until
  login. Structures are read through protected copies, never raw pointer chasing.
- **An unimplemented call must be safe and loud.** The generated `unimplemented()` fallback
  (Ashita), unknown-slot logging (Windower 3) and `partial()` metatables (Lua) turned "it crashes
  somewhere" into a list of exactly what to implement next.
- **Survey with a test process.** A small exe that loads the real `FFXiMain.dll` plus
  `VanaCore.Core.dll`, renders frames and feeds packets let us load every plugin and addon and
  read the log, without the game running. To feed received packets through the real hook: patch
  the game's decrypt function past its prologue to a plain `memcpy` stand-in, then call its entry
  with a plaintext buffer (a 0x1C header with the server time at +8, then packets).
- **Match the exact ImGui commit** the plugin SDK was built against, and verify struct layouts;
  never assume "close enough".
- **Per-plugin ownership** of fonts, primitives and callbacks, freed on unload, prevents dangling
  vtable calls into unloaded DLLs.
- **Plugins before addons** for commands and packets, in Ashita's priority order. Some plugins
  rely on seeing lines before addons do. But Ashita's addons are its Addons plugin, priority 0 and
  loaded near the top of the boot script: for command lines, only plugins with a negative priority
  go first, the rest after the addons (PacketFlow's `/pf` took Party Finder's Ctrl+G otherwise).
- **Addons commonly save settings in `unload`.** Unload every addon and plugin when the game
  exits. VanaCore doesn't do this yet and loses those saves.
- **Addons poll a lot.** Per-frame caches of game data reads (5.3) made a large difference.

---

## 9. What isn't done yet (VanaCore, September 2026)

- **Ashita plugins**:
  - the resource manager's `GetTexture`;
  - ImGui 1.92 for 4.30 plugins;
  - `GetAbilityRange`, `GetAbilityType` and `GetSpellRange`.
- **Ashita addons**:
  - the `d3d8` library;
  - font objects;
  - LuaSec's TLS over an addon's own socket (`ssl.wrap`);
  - the inventory and resources from Lua.
- **Windower plugins**: `IncomingText`/`OutgoingText`, `AddItem`/`RemoveItem`, and the three buff
  timers.
- **Windower addons**:
  - native Lua modules (`LuaCore.dll` stand-in).

---

## 10. Porting to xi-on-mac

xi-on-mac is a very different host:

- `FFXiMain.dll` is **statically recompiled to C**.
- Guest memory is a **4 GB window** (`GUEST_PTR(a) = rt_guest_base + a` on 64-bit hosts).
- The Win32, D3D8, DirectInput and window layer is **your own** (SDL3 input and a Metal back
  end).
- There's no Wine, and on arm64 no x86 execution at all.

Here's how each part maps.

### 10.1 What carries over almost directly

- **The Lua addon system (5) is the most portable part.** LuaJIT runs natively on arm64 macOS,
  so these port as they are: the addon host design, `vana.lua`'s events and tasks, the Ashita
  and Windower Lua layers, the patched LuaJIT (the patch is in the parser and lexer only,
  architecture-neutral), Ashita's and Windower's libraries, and ImGui 1.81 for Lua.
  - Replace VanaCore's Windows-only pieces:
    - WIC image loading → ImageIO or stb_image;
    - DirectWrite/GDI text rendering → CoreText, or your UI renderer;
    - `GetPrivateProfileString` → your ini code;
    - Shift-JIS conversion → iconv.
- **Game data reads (2.5)**:
  - Your guest memory holds the original image and the game's heap at their guest addresses, so
    the **same patterns** find the same globals if you scan the unpacked image's code bytes in
    guest memory. You could also record the addresses in your per-build metadata, since you
    already key everything by build hash.
  - Every read becomes `rd32(guest_address)` and so on.
  - The "protected copy" becomes a bounds and mapping check against your guest window.
- **The packet pipeline (4)** needs a hook on the two functions at the decrypt and encrypt
  addresses. In a recompile you control every call: either make the recompiler emit calls to
  those two addresses through a hookable function pointer, or wrap the translated functions by
  name. The buffer format and all the processing logic are identical.
- **The command line and chat log (2.1, 2.2)** work the same way: wrap the translated
  `ParseInput` (return 0 when handled) and call the translated chat log writer (queue lines until
  its global is non-null).
- **Input (7)** is simpler for you, because you implement DirectInput, the window messages and
  `GetCursorPos` yourselves (SDL3). Blank the DirectInput reads you hand to the game while an
  overlay has the input. The rule that "the game reads DirectInput, not messages" is still what
  matters.
- **D3D8 hooks (2.6)**: you own the D3D8 front end, so call your addon and plugin frame at the
  equivalent of `Present` and wrap state save/restore around foreign drawing.

### 10.2 What doesn't carry over, and the options

- **Code patches from Lua addons.** Addons that pattern-scan code and **write to code** (inline
  patches) do nothing against recompiled code, because the bytes in guest memory are no longer
  what runs. Addons that write **data** (globals, structures, floats such as Ashita's
  `drawdistance`) work. For each popular code-patching addon, implement the patch as a runtime
  option or a recompiler-level switch instead.
- **Addons using `ffi` on game addresses.** Ashita addons often do
  `ffi.cast('uint32_t*', ashita.memory.find(...))`. On your 64-bit host a guest address isn't a
  host pointer. Either give `ashita.memory.find` / `read_*` / `write_*` guest-address semantics
  (and have `find` return guest addresses; most addons only pass those back to `read_*`), or
  intercept `ffi.cast` of numbers. Addons that `ffi.cdef` Win32 or D3D8 functions and call them
  through `ffi.C` (the `d3d8` library, `GetModuleHandle`) need those functions provided natively
  or stubbed.
- **Native x86 plugin DLLs.** Ashita and Windower plugins are 32-bit Windows DLLs and **cannot
  run on arm64 macOS as they are**. The options, from least to most work:
  1. **Don't host DLLs.** Reimplement the plugins players need natively in C, or as Lua addons.
     Popular ones are small: FindAll, Minimap, Timers, Shorthand, cexidats. VanaCore itself took
     this route for VanaView and Pivot.
  2. **Recompile the plugins with your own recompiler.** They're plain x86 PE DLLs, so discover,
     recompile and link them like `FFXiMain`. The host side then needs the **same ABIs described
     above, in guest memory**:
     - Build guest-side vtables whose entries are addresses of your **thunks**, which marshal
       the guest call (stdcall, or thiscall with `this` in ecx, x87 returns, struct returns by
       hidden pointer) into host C functions. You already have this for Win32 imports, where
       imports are thunked to shims.
     - The Ashita generator's approach still applies: generate one thunk per SDK method from
       the headers, with an "unimplemented" default.
     - Plugins' calls to Win32, D3D8 and DirectInput go through your existing import thunks.
     - Plugins built with MSVC's C++ runtime also need its exception unwinding
       (`_CxxThrowException`, SEH frames on the guest's `fs:[0]` chain).
  3. **An interpreter for plugin code.** This is slower, but it avoids per-plugin discovery.
- **SEH-guarded plugin calls** become your runtime's guest fault traps: a guest access violation
  inside plugin code should mark the plugin crashed and unwind back to the host.

### 10.3 Order we'd suggest

1. **Addon host + `vana.*` + the command line and chat log hooks**. Try `/addon load` of
   VanaCore-style and simple Ashita addons (`aspect`, `distance`).
2. **The Ashita Lua layer**, then **ImGui 1.81 + the Lua binding** on your Metal back end. Most
   modern Ashita addons are ImGui windows.
3. **The Windower Lua layer + the LuaJIT patch**.
4. **Packets** (hooks + processing), then packet events for Lua (`packet_in`/`packet_out`,
   Windower's `incoming chunk`) using the `pack` library.
5. **Plugins**, only after deciding between the options in 10.2. Reimplementing the few that
   matter is probably the best value.

Addresses and patterns in this document are for CatsEyeXI's client build. For each of your builds,
run the patterns against the unpacked image, confirm each hit is unique, and record the addresses
in your metadata. The game's code hasn't moved much between recent builds, so the patterns have
held.
