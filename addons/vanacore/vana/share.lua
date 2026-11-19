--[[
vana\share.lua: a preset as one string to paste, and the maths that puts addons' windows where they belong on
another screen. No game calls in here (so it can be run and tested outside it); vana.lua does the reading and writing.
Loaded by vana.lua with `local M = ...`; the functions are M.share.

The string is Base64 of a small text, one record a line, fields separated by tabs (a tab, a line break and a % inside a
field are written %09, %0A and %25):

    VCP1                                    the format and its version
    n <TAB> name
    d <TAB> when it was saved
    r <TAB> width x height                  the game's resolution, with the windows (left out without them)
    s <TAB> section.key <TAB> value         a setting, as vanacore.ini has it (any number of these)
    w <TAB> name <TAB> x <TAB> y <TAB> w <TAB> h <TAB> scale    an addon's window (any number)
    c <TAB> checksum                        of every byte before this line: a paste that was cut or changed is caught

The settings are named (not numbered by a list of keys): a number would mean a different setting the moment a build adds
or moves one, and a string from another version would load wrong without saying.
--]]

local M = ...
local share = {}

local unpack = unpack or table.unpack
local MAGIC = 'VCP1'

-------------------------------------------------------------------------------
-- Base64
-------------------------------------------------------------------------------

local ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
local ENC, DEC = {}, {}
for i = 1, 64 do
    local c = ALPHABET:sub(i, i)
    ENC[i - 1] = c
    DEC[c:byte()] = i - 1
end
DEC[('-'):byte()] = 62 -- (the URL-safe spelling, if a chat turned it)
DEC[('_'):byte()] = 63

function share.b64encode(data)
    local out = {}
    for i = 1, #data, 3 do
        local a, b, c = data:byte(i, i + 2)
        local v = a * 65536 + (b or 0) * 256 + (c or 0)
        out[#out + 1] = ENC[math.floor(v / 262144) % 64] .. ENC[math.floor(v / 4096) % 64]
                        .. (b and ENC[math.floor(v / 64) % 64] or '=') .. (c and ENC[v % 64] or '=')
    end
    return table.concat(out)
end

-- the bytes `text` stands for (spaces, breaks and the padding are ignored), or nil if it isn't Base64
function share.b64decode(text)
    text = text:gsub('%s+', ''):gsub('=+$', '')
    if text == '' or #text % 4 == 1 or text:find('[^%w%+/%-_]') then
        return nil
    end
    local out = {}
    for i = 1, #text, 4 do
        local group = text:sub(i, i + 3)
        local v, n = 0, #group
        for j = 1, 4 do
            v = v * 64 + (j <= n and DEC[group:byte(j)] or 0)
        end
        local bytes = { math.floor(v / 65536) % 256, math.floor(v / 256) % 256, v % 256 }
        out[#out + 1] = string.char(unpack(bytes, 1, n - 1))
    end
    return table.concat(out)
end

-------------------------------------------------------------------------------
-- The string
-------------------------------------------------------------------------------

local function esc(value)
    return (tostring(value):gsub('[%%\r\n\t]', function (c) return string.format('%%%02X', c:byte()) end))
end

local function unesc(value)
    return (value:gsub('%%(%x%x)', function (hex) return string.char(tonumber(hex, 16)) end))
end

-- a number as short as it goes: 100, 7.5, 0.25
local function num(value)
    local text = string.format('%.2f', tonumber(value) or 0)
    text = text:gsub('%.?0+$', '')
    if text == '' or text == '-' or text == '-0' then
        text = '0'
    end
    return text
end

share.num = num

local function checksum(text)
    local a, b = 1, 0
    for i = 1, #text do
        a = (a + text:byte(i)) % 65521
        b = (b + a) % 65521
    end
    return string.format('%04x%04x', b, a)
end

local function split(line)
    local fields = {}
    for field in (line .. '\t'):gmatch('([^\t]*)\t') do
        fields[#fields + 1] = unesc(field)
    end
    return fields
end

local function valid_key(key)
    return key:match('^[%w_%-%.]+$') ~= nil and key:find('.', 1, true) ~= nil
end

-- A preset as its string. `data`: { name, saved, values = { ['section.key'] = 'text' }, screen = { w, h } and
-- windows = { { name, x, y, w, h, scale } } (both or neither) }.
function share.pack(data)
    local lines = { MAGIC, 'n\t' .. esc(data.name or ''), 'd\t' .. esc(data.saved or '') }
    if data.windows ~= nil and #data.windows > 0 and data.screen ~= nil then
        lines[#lines + 1] = 'r\t' .. num(data.screen[1]) .. 'x' .. num(data.screen[2])
    end
    local keys = {}
    for key in pairs(data.values or {}) do
        keys[#keys + 1] = key
    end
    table.sort(keys)
    for _, key in ipairs(keys) do
        lines[#lines + 1] = 's\t' .. esc(key) .. '\t' .. esc(data.values[key])
    end
    if data.screen ~= nil then
        for _, w in ipairs(data.windows or {}) do
            lines[#lines + 1] = table.concat({ 'w', esc(w.name), num(w.x), num(w.y), num(w.w or 0), num(w.h or 0), num(w.scale or 1) }, '\t')
        end
    end
    local body = table.concat(lines, '\n') .. '\n'
    return share.b64encode(body .. 'c\t' .. checksum(body) .. '\n')
end

-- A string as the data pack() was given, or nil and what's wrong with it, in words for the person who pasted it.
function share.unpack(text)
    local raw = share.b64decode(text or '')
    if raw == nil then
        return nil, "That isn't a VanaCore preset string."
    end
    local first = raw:match('^([^\n]*)\n')
    if first == nil or not first:match('^VCP%d+$') then
        return nil, "That isn't a VanaCore preset string."
    end
    if tonumber(first:match('%d+')) > tonumber(MAGIC:match('%d+')) then
        return nil, 'That string is from a newer VanaCore: update this one to read it.'
    end
    local body, sum = raw:match('^(.*\n)c\t(%x+)\n?$')
    if body == nil then
        return nil, 'That string looks cut short: copy all of it.'
    end
    if checksum(body) ~= sum then
        return nil, 'That string was changed or cut short: copy it again.'
    end
    local data = { name = '', saved = '', values = {} }
    local windows = {}
    for line in body:gmatch('([^\n]*)\n') do
        local f = split(line)
        local kind = f[1]
        if kind == 'n' then
            data.name = f[2] or ''
        elseif kind == 'd' then
            data.saved = f[2] or ''
        elseif kind == 'r' then
            local w, h = (f[2] or ''):match('^(%d+%.?%d*)x(%d+%.?%d*)$')
            if w ~= nil then
                data.screen = { tonumber(w), tonumber(h) }
            end
        elseif kind == 's' then
            if f[2] ~= nil and valid_key(f[2]) then
                data.values[f[2]] = f[3] or ''
            end
        elseif kind == 'w' then
            if f[2] ~= nil and f[2] ~= '' and tonumber(f[3]) and tonumber(f[4]) then
                windows[#windows + 1] = { name = f[2], x = tonumber(f[3]), y = tonumber(f[4]), w = tonumber(f[5]) or 0,
                                          h = tonumber(f[6]) or 0, scale = tonumber(f[7]) or 1 }
            end
        end
    end
    if data.screen ~= nil and #windows > 0 then
        data.windows = windows
    end
    return data
end

-------------------------------------------------------------------------------
-- Another screen
-------------------------------------------------------------------------------

local function clamp(value, low, high)
    return math.max(low, math.min(high, value))
end

-- Windows placed on a `from_w` x `from_h` screen, placed for a `to_w` x `to_h` one: { { name, x, y, scale } }.
-- Each window keeps its place in proportion to the screen: its centre is the same fraction across and down (so one against
-- the right edge stays against it, on a wider screen too), and the window is as many times bigger as the screen is
-- taller (the interface is a pixel size: on a screen twice as tall a window is half the size it looked, so its scale
-- doubles), kept inside the screen and the 0.25 to 4 scale VanaCore allows. The same resolution leaves them as they are.
-- Also gives the factor the scales were multiplied by.
function share.fit(windows, from_w, from_h, to_w, to_h)
    local out = {}
    local same = from_w == to_w and from_h == to_h
    local usable = from_w > 0 and from_h > 0 and to_w > 0 and to_h > 0
    local factor = (usable and not same) and to_h / from_h or 1
    if math.abs(factor - 1) < 0.005 then
        factor = 1
    end
    for _, w in ipairs(windows) do
        local scale = w.scale or 1
        local x, y = w.x, w.y
        if usable and not same then
            local was = scale > 0 and scale or 1
            scale = clamp(was * factor, 0.25, 4)
            local grown = scale / was -- (less than the factor where the scale hit an end)
            local width, height = (w.w or 0) * grown, (w.h or 0) * grown
            x = (w.x + (w.w or 0) / 2) / from_w * to_w - width / 2
            y = (w.y + (w.h or 0) / 2) / from_h * to_h - height / 2
            x = width <= to_w and clamp(x, 0, to_w - width) or 0
            y = height <= to_h and clamp(y, 0, to_h - height) or 0
        end
        out[#out + 1] = { name = w.name, x = math.floor(x + 0.5), y = math.floor(y + 0.5), scale = math.floor(scale * 100 + 0.5) / 100 }
    end
    return out, factor
end

M.share = share
return share
