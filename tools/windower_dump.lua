-- What src\core\lua\compat\windower.lua puts in `windower`, for windower_coverage.py: it runs the script with a
-- stand-in `vana` (any name it reaches for is there and does nothing) and prints one line per name, tab-separated:
--   path  type  first line  last line   (the lines only for functions defined in windower.lua; type `native` is a
--   function of vana's that windower.lua hands on as it is)
-- Run with the luatest runner (build\obj\luatest\luarun.exe, made by tools\luatest\run.cmd):
--   luarun windower_dump.lua <windower.lua>
local path = arg[1]
local source = assert(io.open(path, 'rb')):read('*a')
local out = io.write

local ANYTHING = {}
local function anything()
    return setmetatable({}, ANYTHING)
end
ANYTHING.__index = function (t, k)
    local v = anything()
    rawset(t, k, v)
    return v
end
ANYTHING.__call = function () return nil end
vana = anything()
vana.paths = { addon = 'C:\\VanaCore\\addons\\windower\\Dump', addons = 'C:\\VanaCore\\addons',
               game = 'C:\\PlayOnline\\SquareEnix\\FINAL FANTASY XI' }
vana.events = { on = function () end, off = function () end }
vana.tasks = { spawn = function (fn, ...) return true, fn(...) end }
vana.log = function () end
vana.config = { get = function (_, _, default) return default end, game = function () return nil end }
vana.fs = { list = function () return {} end }
game = anything()

local ok, err = pcall(assert(loadstring(source, '@windower.lua')))
if not ok then
    io.stderr:write('windower.lua stopped while loading (the dump has what it made before): ' .. tostring(err) .. '\n')
end

local seen = {}
local function walk(t, prefix, depth)
    if seen[t] or depth > 3 then
        return
    end
    seen[t] = true
    local keys = {}
    for k in pairs(t) do
        if type(k) == 'string' then
            keys[#keys + 1] = k
        end
    end
    table.sort(keys)
    for _, k in ipairs(keys) do
        local v = rawget(t, k)
        local name = prefix .. k
        if getmetatable(v) == ANYTHING then
            out(('%s\tnative\t0\t0\n'):format(name)) -- vana's own function, taken as it is
        elseif type(v) == 'function' then
            local info = debug.getinfo(v, 'S')
            local here = info.source == '@windower.lua'
            out(('%s\tfunction\t%s\t%s\n'):format(name, here and info.linedefined or 0, here and info.lastlinedefined or 0))
        else
            out(('%s\t%s\t0\t0\n'):format(name, type(v)))
            if type(v) == 'table' then
                walk(v, name .. '.', depth + 1)
            end
        end
    end
end
walk(windower or {}, 'windower.', 1)
