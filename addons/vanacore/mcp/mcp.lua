--[[
mcp: where the MCP server's lua tool runs ([mcp] enabled=1 in vanacore.ini). VanaCore loads it the
first time an AI connected to the game runs Lua, so the server's code has a state of its own, apart
from every other addon's. Globals set by that code stay here between calls.

Besides VanaCore's API (vana.*), it has:
  ffi, bit             LuaJIT's (ffi parses each C declaration string once, see below)
  hex(n)               "0x0A1B2C3D"
  call(address, signature, ...)
                       calls a function in the game, e.g.
                       call(0x10123456, 'int(__thiscall*)(void*, int)', this, 5)
  peek(address, n)     n bytes at address as hex (nil if they can't be read)
  sym(name)            where VanaCore.Core.dll's symbol `name` is now, from the .map beside the
                       loaded DLL: '_grass_floor_near' (C: the leading _ may be left off) or a C++
                       name's start, '?g_target@'. Fails unless the .map's timestamp is the loaded
                       DLL's, so an address from another build can't be used
  vcall(name, signature, ...)
                       call() at sym(name)

LuaJIT never frees a ctype, and each time it parses a function type or an anonymous struct, union
or enum it makes new ones (ffi.cast('int(__stdcall*)(uint32_t, uint32_t)', f) takes 4). A state
has room for 65536; after that every new ctype fails with "table overflow". Code here is run again
and again, often in loops, so this state's ffi.typeof, cast, new, sizeof, alignof, offsetof and
istype (also through require('ffi')) keep the ctype for each declaration string they're given and
reuse it: the same string is the same ctype. ffi.metatype still makes a new one from a string.
--]]

addon.name    = 'mcp'
addon.author  = 'VanaCore'
addon.version = '1.2'
addon.desc    = "The MCP server's Lua state"

local raw = require('ffi')
local ctypes = {} -- declaration string -> its ctype

local function ctype(declaration)
    if type(declaration) ~= 'string' then
        return declaration
    end
    local ct = ctypes[declaration]
    if ct == nil then
        ct = raw.typeof(declaration)
        ctypes[declaration] = ct
    end
    return ct
end

ffi = setmetatable({
    typeof = function (declaration, ...)
        if select('#', ...) > 0 then
            return raw.typeof(declaration, ...) -- with $ parameters: not one string's type
        end
        return ctype(declaration)
    end,
    cast = function (declaration, value)
        return raw.cast(ctype(declaration), value)
    end,
    new = function (declaration, ...)
        return raw.new(ctype(declaration), ...)
    end,
    sizeof = function (declaration, ...)
        return raw.sizeof(ctype(declaration), ...)
    end,
    alignof = function (declaration)
        return raw.alignof(ctype(declaration))
    end,
    offsetof = function (declaration, field)
        return raw.offsetof(ctype(declaration), field)
    end,
    istype = function (declaration, value)
        return raw.istype(ctype(declaration), value)
    end,
}, { __index = raw })
package.loaded.ffi = ffi
bit = require('bit')

function hex(n)
    return string.format('0x%08X', n)
end

function call(address, signature, ...)
    return ffi.cast(signature, address)(...)
end

function peek(address, n)
    local bytes = vana.memory.read_array(address, n or 16)
    if bytes == nil then
        return nil
    end
    local out = {}
    for i = 1, #bytes do
        out[i] = string.format('%02X', bytes[i])
    end
    return table.concat(out, ' ')
end

-- VanaCore.Core.dll's symbols, by its .map. Every rebuild moves them, and an address carried over
-- from another build called into the middle of some other function and took the game down (twice,
-- 2026-11-27), so nothing is kept across builds: the loaded DLL's link timestamp is read at each
-- call, the .map has to say the same one, and what was found is forgotten when it changes.
pcall(raw.cdef, [[
void* GetModuleHandleA(const char* name);
uint32_t GetModuleFileNameA(void* module, char* path, uint32_t size);
]]) -- (pcall: declared already when the state had them before)
local maps = { stamp = nil, base = 0, path = nil, found = {} }

local function loaded_core()
    local module = raw.C.GetModuleHandleA('VanaCore.Core.dll')
    if module == nil then
        error('sym: VanaCore.Core.dll is not loaded', 3)
    end
    local base = tonumber(raw.cast('uintptr_t', module))
    local pe = raw.cast('uint32_t*', base + 0x3C)[0]
    local stamp = raw.cast('uint32_t*', base + pe + 8)[0]
    if stamp ~= maps.stamp or base ~= maps.base then
        local path = raw.new('char[520]')
        local n = raw.C.GetModuleFileNameA(module, path, 520)
        maps.stamp, maps.base, maps.found = stamp, base, {}
        maps.path = raw.string(path, n):gsub('%.[^.\\/]*$', '') .. '.map'
    end
    return maps
end

-- the .map's header: its link timestamp and the base its addresses are for
local function map_header(file)
    local stamp, preferred
    for _ = 1, 40 do
        local line = file:read('*l')
        if line == nil then
            break
        end
        stamp = stamp or line:match('Timestamp is (%x+)')
        preferred = preferred or line:match('Preferred load address is (%x+)')
        if stamp and preferred then
            break
        end
    end
    return stamp and tonumber(stamp, 16), preferred and tonumber(preferred, 16)
end

function sym(name)
    local m = loaded_core()
    local address = m.found[name]
    if address then
        return address
    end
    local file = io.open(m.path, 'r')
    if file == nil then
        error('sym: no ' .. m.path, 2)
    end
    local stamp, preferred = map_header(file)
    if stamp ~= m.stamp or preferred == nil then
        file:close()
        error(string.format('sym: %s is for build %s, the loaded DLL is %08X: not used', m.path,
            stamp and string.format('%08X', stamp) or '?', m.stamp), 2)
    end
    -- ' 0001:004c48a0       _grass_floor_near          104c58a0 f   VanaCore.Gfx:grass.obj'
    local plain = name:sub(1, 1) ~= '_' and name:sub(1, 1) ~= '?' and '_' .. name or nil
    local hits = {}
    for line in file:lines() do
        local symbol, va = line:match('^%s*%x+:%x+%s+(%S+)%s+(%x+)')
        if symbol and (symbol == name or symbol == plain or (name:sub(1, 1) == '?' and symbol:sub(1, #name) == name)) then
            hits[#hits + 1] = { symbol, tonumber(va, 16) }
        end
    end
    file:close()
    if #hits == 0 then
        error('sym: ' .. name .. ' is not in ' .. m.path, 2)
    end
    if #hits > 1 then
        local names = {}
        for i = 1, math.min(#hits, 8) do
            names[i] = hits[i][1]
        end
        error('sym: ' .. name .. ' is ' .. #hits .. ' symbols: ' .. table.concat(names, ' '), 2)
    end
    address = m.base + hits[1][2] - preferred
    m.found[name] = address
    return address
end

function vcall(name, signature, ...)
    return ffi.cast(signature, sym(name))(...)
end
