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

LuaJIT never frees a ctype, and each time it parses a function type or an anonymous struct, union
or enum it makes new ones (ffi.cast('int(__stdcall*)(uint32_t, uint32_t)', f) takes 4). A state
has room for 65536; after that every new ctype fails with "table overflow". Code here is run again
and again, often in loops, so this state's ffi.typeof, cast, new, sizeof, alignof, offsetof and
istype (also through require('ffi')) keep the ctype for each declaration string they're given and
reuse it: the same string is the same ctype. ffi.metatype still makes a new one from a string.
--]]

addon.name    = 'mcp'
addon.author  = 'VanaCore'
addon.version = '1.1'
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
