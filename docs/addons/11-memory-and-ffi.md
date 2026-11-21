# 11. Memory and FFI

For what `vana.game` doesn't hand you: reading and writing the game's memory directly, finding code and data by
pattern, patching, and calling the game's own functions with LuaJIT's FFI. Everything here can crash the game if used
wrongly; the sections at the end say what VanaCore does about that.

Before reaching for this, check [Game data](04-game-data.md) and [Packets](10-packets.md): most of what addons want is
there already, kept right as the game changes.

## Reading and writing: `vana.memory`

Addresses are plain Lua numbers.

```lua
vana.memory.read_uint8(address)    -- also int8, int16, uint16, int32, uint32, int64, uint64, float, double
vana.memory.read_string(address[, size])   -- up to `size` bytes (1024), stopping at a NUL
vana.memory.read_bytes(address, size)      -- `size` bytes as a string, NULs and all; nil if unreadable
vana.memory.read_array(address, size)      -- a table of `size` byte values (1-based); nil if unreadable

vana.memory.write_uint8(address, value)    -- the same set of types; true if written
vana.memory.write_string(address, text)    -- the text and a NUL after it
vana.memory.write_array(address, bytes)    -- a table of byte values
```

A read of memory that isn't readable returns `0` (or `nil` for `read_bytes` and `read_array`), never a crash. A write
to a page that isn't writable (code, constants) makes it writable for the write and puts the protection back, and
flushes the instruction cache, so patching code is one call.

## Finding things

```lua
local base, size = vana.memory.module('FFXiMain.dll')   -- a loaded module's base address and size; nil if not loaded
local base, size = vana.memory.module()                 -- the game's own exe

vana.memory.find(module, size, pattern[, offset[, count]])
```

`find` scans for a byte pattern: hex bytes, two digits each, spaces optional, `??` (or `?`) for any byte:
`'8B 44 24 04 8B 0D ?? ?? ?? ??'`. The first argument is a module name (then `size` 0 means the whole module) or a start
address with `size` bytes to scan. It returns the address of the `count`th match (0 the first) plus `offset`, or `0`.

```lua
local at = vana.memory.find('FFXiMain.dll', 0, '8B 0D ?? ?? ?? ?? 85 C9 74', 2)
if at ~= 0 then
    local pointer = vana.memory.read_uint32(at)   -- the 4 bytes after 8B 0D: the address the instruction reads
    local value = vana.memory.read_uint32(pointer)
end
```

**FFXiMain.dll moves.** Its base address differs from run to run, so an address from a disassembly (Ghidra's base is
`0x10000000`) is an offset: `base + (ghidra_address - 0x10000000)`. A pattern is sturdier than an offset: it survives a
client update that shifts code, and that's how VanaCore finds everything it uses.

The game's structures VanaCore has already found are reachable without a pattern: `vana.game` reads them, and the MCP
server's `game_state` tool reports their addresses.

## Allocating and protecting

```lua
local p = vana.memory.alloc(size)          -- zeroed, readable and writable memory, or 0
vana.memory.free(p)                        -- true if freed (freeing twice is harmless)
vana.memory.protect(address, size, protection)   -- VirtualProtect: the old protection, or nil
vana.memory.unprotect(address, size)       -- readable, writable and executable; true if it worked
```

## Patching code: `vana.memory.assembler`

x86 assembly text, as Ashita 4.30's `ashita.memory.assembler` reads it:

```lua
local code, err = vana.memory.assembler.assemble(text[, base])   -- the bytes as a string, encoded to run at `base` (0); nil and the error
local cave, err = vana.memory.assembler.cave(text)               -- executable memory holding the assembled text: its address; nil and the error
vana.memory.assembler.release(cave)                              -- frees a cave; false if it isn't one
vana.memory.assembler.call(address, target, nops)                -- a call to `target` written at `address`, then `nops` NOPs
vana.memory.assembler.jmp(address, target, nops)                 -- a jump
vana.memory.assembler.nop(address, count)                        -- `count` NOPs at `address`
```

A hook is a cave with your code (ending in a jump back) and a `jmp` written over the game's instruction. Keep the
original bytes (`read_bytes`) and write them back at `unload`: **VanaCore doesn't undo memory patches for you.**

```lua
local site = vana.memory.find('FFXiMain.dll', 0, 'D9 44 24 04 D8 0D ?? ?? ?? ??')
local saved = vana.memory.read_bytes(site, 5)
vana.memory.assembler.nop(site, 5)            -- skip the instruction
vana.on('unload', function ()
    vana.memory.write_array(site, { saved:byte(1, -1) })
end)
```

## Calling the game: LuaJIT's FFI

```lua
local ffi = require('ffi')
ffi.cdef [[
    typedef struct { float x, y, z; } Vec3;
]]
```

The game's functions are called by casting an address to a function pointer. FFXiMain.dll is 32-bit; mind the calling
convention (`__thiscall` for methods, `__stdcall` for Windows APIs, `__cdecl` otherwise):

```lua
local base = vana.memory.module('FFXiMain.dll')
local set_target = ffi.cast('int (__thiscall*)(void*, int)', base + 0x123456)
set_target(this, index)
```

Windows APIs are there through `ffi.C` after a `cdef`:

```lua
ffi.cdef('unsigned long GetTickCount(void);')
local now = ffi.C.GetTickCount()
```

Pointers VanaCore hands you (`e.data_raw`, `e.data_modified_raw`, `e.state`, `e.vertex_stream_zero_data`) are light
userdata: cast them, `ffi.cast('uint8_t*', e.data_raw)`.

### Two LuaJIT things to know

**Ctypes are never freed.** Each `ffi.typeof`, `ffi.cast`, `ffi.new`, `ffi.sizeof` with a *string* declaration parses
it and may make a new ctype; a state has room for 65,536, and after that every new one fails with `table overflow`.
Parse each declaration once, at the top of your file, and reuse the ctype:

```lua
local u8ptr = ffi.typeof('uint8_t*')
vana.on('packet_in', function (e)
    local p = ffi.cast(u8ptr, e.data_raw)   -- no parse per packet
end)
```

**Numbers are doubles.** An address or an `ImU32` colour is fine (53 bits), but `bit.*` works on 32 bits and a `uint64`
needs the FFI's `ffi.new('uint64_t')` or `ULL` literals.

## When it goes wrong

A hardware exception in your code (an access violation from a bad pointer, a divide by zero) is caught at the boundary
of the call into your addon. VanaCore reports it in the chat and the log, with where it happened
(`crashed: access violation reading 0x00000010 at FFXiMain.dll+0x1234 from VanaCore.Core.dll+0x5678 (myaddon.lua:42 in
update)`), and **freezes the addon**: its Lua state is never entered again (its `unload` handlers don't run, and its
memory stays allocated until the game ends), because the state's stack is unknown after the fault. Everything it made
through `vana` (boxes, ImGui windows, game interface changes) is removed as usual. Memory you patched stays patched.

`at` is the instruction that faulted; `from` is the code that called it, which is what tells when the fault is in a C
library routine (`ffi.string` of a bad pointer faults in `strlen`, called from your code). An address with no DLL name is
code made as the game runs: LuaJIT's compiled Lua (your FFI call itself) or a hook. The brackets are your Lua, innermost
first; in a loop LuaJIT has compiled they can name where the compiled stretch began (the loop, or the function that
called the one at fault) rather than the line itself. The log has the return addresses further up the stack too.

The game itself is still running, so a crash in an addon is recoverable: fix the code, `/addon load` it again.

Everything here runs on the game's thread. Don't start threads of your own with the FFI and call `vana` from them.

## Tools for the work

- **The MCP server** (`[mcp] enabled=1`; [Developing and debugging](14-dev-workflow.md)) gives an AI tool, or you
  through it, memory reads and writes, a disassembler, pattern and reference searches, a value scanner (Cheat Engine's
  first scan and next scan), hardware watchpoints that don't stop the game, and a Lua console with the FFI. It's the
  fastest way to find an address before writing the addon.
- **`/vana lua <addon> <code>`** runs Lua in a loaded addon, so you can try a read before committing it to the file.
- **Windower's and Ashita's own addons** are a library of found addresses and structure layouts for the same client.
