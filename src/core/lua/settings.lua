--[[
VanaCore's addon settings: vana.settings and vana.json, in every addon (run after lua/vana.lua, before its kind's
compatibility script). An addon's files live under VanaCore's folder, keyed on its name as its folder spells it:

    settings\addons\<Addon>\<file>                 global: the same for every character
    settings\addons\<Addon>\<Character>\<file>     per character: the character logged in

settings\plugins\<Plugin>\ is laid out the same for plugins; nothing writes there yet.

A store is one JSON file of settings with defaults, registered once:

    local store = vana.settings.register({ scope = 'character', defaults = { scale = 1, favorites = {} } })
    store.data.scale = 2
    store:save()

A per-character store follows the character: before one is in (and after a logout) it holds its defaults and isn't
saved; when one comes in, that character's file is read (or made from the defaults). What changed in a store is saved
before it's swapped for another character's and after the addon's own unload handlers, so a setting changed without a
save isn't lost; a store that didn't change isn't written, so a file edited by hand while the game runs is left as it is.

`...`: lua/vana.lua's hooks (after: runs a function after the addon's own handlers for an event; protect) and the
addon's name as its folder spells it (Sidekick, not sidekick).
--]]

local hooks, addon_name = ...
local vana = vana

-------------------------------------------------------------------------------
-- JSON: written with sorted keys and two-space indents, so a file reads (and diffs) the same from save to save
-------------------------------------------------------------------------------

local json = {}

local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }

local function quote(text)
    return '"' .. text:gsub('[%c"\\]', function (c) return escapes[c] or ('\\u%04x'):format(c:byte()) end) .. '"'
end

local function number_text(n, where)
    if n ~= n or n == math.huge or n == -math.huge then
        error(('%s: %s is not a number JSON can hold'):format(where, tostring(n)), 0)
    end
    if n % 1 == 0 and n > -2^53 and n < 2^53 then
        return n == 0 and '0' or ('%.0f'):format(n) -- exact to 2^53 (a server id is past %d's 31 bits on some Luas)
    end
    local text = ('%.14g'):format(n)
    if tonumber(text) ~= n then
        text = ('%.17g'):format(n)
    end
    return text
end

-- How many items `t` holds when it's a list (keys 1 to n, none missing), else nil. An empty table isn't one.
local function list_length(t)
    local n = 0
    for k in pairs(t) do
        if type(k) ~= 'number' or k < 1 or k % 1 ~= 0 then
            return nil
        end
        n = n + 1
    end
    for i = 1, n do
        if rawget(t, i) == nil then
            return nil
        end
    end
    return n > 0 and n or nil
end

-- Whole numbers first, in order, then the rest as text in order.
local function key_order(a, b)
    local na, nb = type(a) == 'number', type(b) == 'number'
    if na ~= nb then
        return na
    end
    if na then
        return a < b
    end
    return tostring(a) < tostring(b)
end

local encode_value

local function encode_table(t, indent, seen, where, out)
    if seen[t] then
        error(where .. ': a table that holds itself', 0)
    end
    seen[t] = true
    local compact = indent == false -- json.encode(value, true): no indents or line breaks
    local inner = not compact and indent .. '  ' or false
    local n = list_length(t)
    if n ~= nil then
        out[#out + 1] = '['
        for i = 1, n do
            out[#out + 1] = compact and (i > 1 and ',' or '') or ((i > 1 and ',\n' or '\n') .. inner)
            encode_value(rawget(t, i), inner, seen, where .. '[' .. i .. ']', out)
        end
        out[#out + 1] = compact and ']' or ('\n' .. indent .. ']')
    else
        local keys = {}
        for k in pairs(t) do
            local kind = type(k)
            if kind == 'number' then
                number_text(k, where) -- a NaN or infinite key fails here
            elseif kind ~= 'string' then
                error(('%s: a %s key (only text and number keys go in JSON)'):format(where, kind), 0)
            end
            keys[#keys + 1] = k
        end
        if #keys == 0 then
            out[#out + 1] = '{}'
            seen[t] = nil
            return
        end
        table.sort(keys, key_order)
        out[#out + 1] = '{'
        for i, k in ipairs(keys) do
            local name = type(k) == 'number' and number_text(k, where) or k
            out[#out + 1] = compact and ((i > 1 and ',' or '') .. quote(name) .. ':')
                or ((i > 1 and ',\n' or '\n') .. inner .. quote(name) .. ': ')
            encode_value(rawget(t, k), inner, seen, where .. '.' .. name, out)
        end
        out[#out + 1] = compact and '}' or ('\n' .. indent .. '}')
    end
    seen[t] = nil
end

encode_value = function (value, indent, seen, where, out)
    local kind = type(value)
    if kind == 'table' then
        encode_table(value, indent, seen, where, out)
    elseif kind == 'string' then
        out[#out + 1] = quote(value)
    elseif kind == 'number' then
        out[#out + 1] = number_text(value, where)
    elseif kind == 'boolean' then
        out[#out + 1] = value and 'true' or 'false'
    elseif value == nil then
        out[#out + 1] = 'null'
    else
        error(('%s: a %s (only tables, text, numbers and true/false go in JSON)'):format(where, kind), 0)
    end
end

-- json.encode(value[, compact]): its JSON text, or nil and what in it can't be written (a function, a table inside
-- itself, ...). Indented, with sorted keys and a line break at the end, as a file reads best; compact: one line, as a
-- request sends it.
function json.encode(value, compact)
    local out = {}
    local indent = ''
    if compact then
        indent = false -- and/or can't say this: `compact and false or ''` is ''
    end
    local ok, why = pcall(encode_value, value, indent, {}, 'value', out)
    if not ok then
        return nil, why
    end
    return table.concat(out) .. (compact and '' or '\n')
end

local decode_value

local function fail(text, at, message)
    local line = 1
    for _ in text:sub(1, at - 1):gmatch('\n') do
        line = line + 1
    end
    error(('line %d: %s'):format(line, message), 0)
end

local function skip_space(text, at)
    return text:find('[^ \t\r\n]', at) or #text + 1
end

local function utf8_char(code)
    if code < 0x80 then
        return string.char(code)
    elseif code < 0x800 then
        return string.char(0xC0 + math.floor(code / 0x40), 0x80 + code % 0x40)
    elseif code < 0x10000 then
        return string.char(0xE0 + math.floor(code / 0x1000), 0x80 + math.floor(code / 0x40) % 0x40, 0x80 + code % 0x40)
    end
    return string.char(0xF0 + math.floor(code / 0x40000), 0x80 + math.floor(code / 0x1000) % 0x40,
        0x80 + math.floor(code / 0x40) % 0x40, 0x80 + code % 0x40)
end

local unescapes = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }

local function decode_string(text, at) -- at: the opening quote
    local parts = {}
    local i = at + 1
    while true do
        local stop = text:find('["\\]', i)
        if stop == nil then
            fail(text, at, 'a string with no closing quote')
        end
        parts[#parts + 1] = text:sub(i, stop - 1)
        if text:sub(stop, stop) == '"' then
            return table.concat(parts), stop + 1
        end
        local c = text:sub(stop + 1, stop + 1)
        if c == 'u' then
            local hex = text:sub(stop + 2, stop + 5)
            local code = #hex == 4 and hex:find('^%x%x%x%x$') and tonumber(hex, 16)
            if not code then
                fail(text, stop, 'a \\u escape without four hex digits')
            end
            i = stop + 6
            if code >= 0xD800 and code <= 0xDBFF and text:sub(i, i + 1) == '\\u' then
                local low = tonumber(text:sub(i + 2, i + 5), 16)
                if low and low >= 0xDC00 and low <= 0xDFFF then
                    code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)
                    i = i + 6
                end
            end
            parts[#parts + 1] = utf8_char(code)
        elseif unescapes[c] then
            parts[#parts + 1] = unescapes[c]
            i = stop + 2
        else
            fail(text, stop, 'an unknown escape \\' .. c)
        end
    end
end

-- A key that's a whole number written as one ("230", "-1": not "07" or "1.5") comes back as the number, so a table
-- keyed by ids (favourites by zone) reads back as it was saved.
local function key_value(name)
    if name:find('^%-?[1-9]%d*$') or name == '0' then
        local n = tonumber(name)
        if n and n > -2^53 and n < 2^53 then
            return n
        end
    end
    return name
end

local function decode_object(text, at)
    local result = {}
    local i = skip_space(text, at + 1)
    if text:sub(i, i) == '}' then
        return result, i + 1
    end
    while true do
        if text:sub(i, i) ~= '"' then
            fail(text, i, 'expected a "key"')
        end
        local name
        name, i = decode_string(text, i)
        i = skip_space(text, i)
        if text:sub(i, i) ~= ':' then
            fail(text, i, 'expected : after "' .. name .. '"')
        end
        local value
        value, i = decode_value(text, skip_space(text, i + 1))
        result[key_value(name)] = value
        i = skip_space(text, i)
        local c = text:sub(i, i)
        if c == '}' then
            return result, i + 1
        elseif c ~= ',' then
            fail(text, i, 'expected , or } after "' .. name .. '"')
        end
        i = skip_space(text, i + 1)
    end
end

local function decode_array(text, at)
    local result = {}
    local n = 0
    local i = skip_space(text, at + 1)
    if text:sub(i, i) == ']' then
        return result, i + 1
    end
    while true do
        local value
        value, i = decode_value(text, i)
        n = n + 1
        result[n] = value
        i = skip_space(text, i)
        local c = text:sub(i, i)
        if c == ']' then
            return result, i + 1
        elseif c ~= ',' then
            fail(text, i, 'expected , or ] in a list')
        end
        i = skip_space(text, i + 1)
    end
end

decode_value = function (text, at)
    local c = text:sub(at, at)
    if c == '{' then
        return decode_object(text, at)
    elseif c == '[' then
        return decode_array(text, at)
    elseif c == '"' then
        return decode_string(text, at)
    elseif text:sub(at, at + 3) == 'true' then
        return true, at + 4
    elseif text:sub(at, at + 4) == 'false' then
        return false, at + 5
    elseif text:sub(at, at + 3) == 'null' then
        return nil, at + 4
    end
    local number = text:match('^-?%d+%.?%d*[eE]?[-+]?%d*', at)
    local value = number and tonumber(number)
    if value == nil then
        fail(text, at, c == '' and 'the text ends too soon' or ('unexpected ' .. c))
    end
    return value, at + #number
end

-- json.decode(text): the value the JSON text holds (null as nil), or nil and where it went wrong ("line 3: ...").
function json.decode(text)
    if type(text) ~= 'string' then
        return nil, 'not text'
    end
    if text:sub(1, 3) == '\239\187\191' then -- UTF-8 byte order mark
        text = text:sub(4)
    end
    local ok, value, at = pcall(decode_value, text, skip_space(text, 1))
    if not ok then
        return nil, value
    end
    at = skip_space(text, at)
    if at <= #text then
        local _, why = pcall(fail, text, at, 'more after the value')
        return nil, why
    end
    return value
end

vana.json = json

-------------------------------------------------------------------------------
-- Folders and file names
-------------------------------------------------------------------------------

addon_name = addon_name or (type(addon) == 'table' and addon.name) or 'unnamed'
local ROOT = vana.paths.install .. '\\settings\\addons\\' .. addon_name

local RESERVED = { con = true, prn = true, aux = true, nul = true }
for i = 1, 9 do
    RESERVED['com' .. i] = true
    RESERVED['lpt' .. i] = true
end

-- `file` as a path under an addon's folder ('layouts\\war.xml', / or \), or nil and why it can't be one: no drive, no
-- leading separator, no . or .. folders, none of Windows' reserved names or characters.
local function relative(file)
    if type(file) ~= 'string' or file == '' then
        return nil, 'a file name is needed'
    end
    local path = file:gsub('/', '\\')
    if path:find('^\\') then
        return nil, file .. ' must be a name under the addon\'s folder, not a full path'
    end
    if path:find('[%c<>:"|?*]') then
        return nil, file .. ' has a character Windows doesn\'t allow in a file name'
    end
    local parts = {}
    for part in path:gmatch('[^\\]+') do
        if part == '.' or part == '..' then
            return nil, file .. ' can\'t go up out of the addon\'s folder'
        end
        if part:find('[ %.]$') then
            return nil, file .. ': a name can\'t end in a dot or a space'
        end
        if RESERVED[(part:match('^[^%.]*') or ''):lower()] then
            return nil, file .. ': ' .. part .. ' is a name Windows keeps for itself'
        end
        parts[#parts + 1] = part
    end
    if #parts == 0 then
        return nil, 'a file name is needed'
    end
    return table.concat(parts, '\\')
end

local function parent(path)
    return path:match('^(.*)\\[^\\]*$')
end

local function read_file(path)
    local file = io.open(path, 'rb')
    if file == nil then
        return nil
    end
    local text = file:read('*a')
    file:close()
    return text
end

-- Writes `text` to `path` whole or not at all (vana.fs.write), making its folders first.
local function write_file(path, text)
    vana.fs.create_dir(parent(path))
    return vana.fs.write(path, text)
end

-------------------------------------------------------------------------------
-- The character
-------------------------------------------------------------------------------

local character = false -- the logged in character's folder name, nil when none, false until first asked
local stores = {}       -- registered, in order
local switch            -- below: moves the per-character stores to another character
local watch_character   -- below: starts looking for a logout or another character (the first per-character store)

-- The logged in character's folder name (its name, as FFXI's names are: letters), or nil.
local function current_character()
    local name, id = vana.game.character()
    if name == nil then
        return nil
    end
    local folder = name:gsub('[^%w_%-]', '')
    if folder == '' then
        folder = tostring(id)
    end
    return folder
end

-- The character now, after moving the per-character stores to it if it changed.
local function refresh()
    local now = current_character()
    if character == false then
        character = now
    elseif now ~= character then
        switch(now)
    end
    return character
end

local function folder_for(scope, level)
    if scope == nil or scope == 'global' then
        return ROOT
    elseif scope == 'character' then
        local name = refresh()
        if name == nil then
            return nil, 'no character is logged in'
        end
        return ROOT .. '\\' .. name
    end
    error("vana.settings: scope must be 'global' or 'character', not " .. tostring(scope), level + 1)
end

-------------------------------------------------------------------------------
-- Stores
-------------------------------------------------------------------------------

local function copy(value)
    if type(value) ~= 'table' then
        return value
    end
    local out = {}
    for k, v in pairs(value) do
        out[k] = copy(v)
    end
    return out
end

-- What `defaults` has that `into` hasn't, copied in, into tables both have as settings; a list (favourites) is taken
-- as it is, not merged item by item.
local function merge(into, defaults)
    for k, default in pairs(defaults) do
        local value = into[k]
        if value == nil then
            into[k] = copy(default)
        elseif type(value) == 'table' and type(default) == 'table' and list_length(default) == nil then
            merge(value, default)
        end
    end
end

-- `target` holding what `source` holds: the same table, so an addon's `local config = store.data` stays good.
local function replace(target, source)
    for k in pairs(target) do
        target[k] = nil
    end
    for k, v in pairs(source) do
        target[k] = v
    end
end

local Store = {}
Store.__index = Store

local function store_file(store)
    local folder = folder_for(store.scope, 2)
    return folder and (folder .. '\\' .. store.name .. '.json') or nil
end

-- Writes the store if its data isn't what was read or written last (`force`: whatever it holds). True, or nil and why.
local function write_store(store, force)
    if store.file == nil then
        return nil, 'no character is logged in'
    end
    if store.held ~= nil then
        return nil, store.held
    end
    local text, why = json.encode(store.data)
    if text == nil then
        return nil, why
    end
    if text == store.saved and not force then
        return true
    end
    local ok, failed = write_file(store.file, text)
    if not ok then
        return nil, failed
    end
    store.saved = text
    return true
end

-- Reads the store's file for the character now (or holds its defaults when there's none), and writes the file when
-- it's missing or lacks a default, so it's there to read and edit.
local function load_store(store)
    store.file = store_file(store)
    store.ready = store.file ~= nil
    store.held = nil
    local data = {}
    local text = store.file and read_file(store.file)
    if text ~= nil then
        local decoded, why = json.decode(text)
        if type(decoded) == 'table' then
            data = decoded
        else
            -- Not settings (a bad hand edit): a copy kept beside it for the player, and the defaults used. If even the
            -- copy can't be written, the file is left alone and nothing is saved over it until it's reloaded.
            why = why or 'not a table'
            local aside = store.file .. '.broken'
            if write_file(aside, text) then
                print(('%s: %s couldn\'t be read (%s): it\'s kept as %s.broken, and the defaults are used'):format(
                    addon_name, store.file, why, store.name .. '.json'))
                text = nil
            else
                print(('%s: %s couldn\'t be read (%s): the defaults are used, and the file is left as it is'):format(
                    addon_name, store.file, why))
                store.held = store.file .. ' couldn\'t be read (' .. why .. '), and isn\'t saved over'
            end
        end
    end
    merge(data, store.defaults)
    replace(store.data, data)
    store.saved = text
    if store.ready then
        local ok, why = write_store(store)
        if not ok then
            vana.log(('settings: %s not written: %s'):format(store.file, tostring(why)))
        end
    end
end

switch = function (now)
    for _, store in ipairs(stores) do
        if store.scope == 'character' then
            write_store(store) -- what changed, to the character it was read for
        end
    end
    character = now
    for _, store in ipairs(stores) do
        if store.scope == 'character' then
            load_store(store)
            if store.changed ~= nil then
                hooks.protect('settings', store.changed, store.data, store)
            end
        end
    end
end

-- store:save(): writes the settings now. True, or nil and why (no character in yet, the file can't be written).
function Store:save()
    return write_store(self, true)
end

-- store:reload(): reads the file again (an edit made by hand while the game runs); what wasn't saved is lost.
function Store:reload()
    load_store(self)
    return self.data
end

-- store:reset(): the defaults again, saved.
function Store:reset()
    replace(self.data, copy(self.defaults))
    return write_store(self, true)
end

-- store:path(): the file it reads and writes now (nil for a per-character store with no character in).
function Store:path()
    return self.file
end

vana.settings = {}

-- vana.settings.register{ name = 'settings', scope = 'global' or 'character', defaults = {}, changed = fn(data, store) }:
-- the store for <name>.json, with the file's settings (what it lacks taken from `defaults`) in store.data. `changed` is
-- called when a per-character store's data is swapped for another character's (or the defaults, at a logout).
function vana.settings.register(options)
    options = options or {}
    assert(type(options) == 'table', 'vana.settings.register: give it a table of options')
    local name = options.name or 'settings'
    assert(type(name) == 'string' and not name:find('[\\/]') and not name:lower():find('%.json$') and relative(name .. '.json'),
        'vana.settings.register: name must be a plain file name (no folders, no .json): ' .. tostring(name))
    local scope = options.scope or 'global'
    assert(scope == 'global' or scope == 'character',
        "vana.settings.register: scope must be 'global' or 'character', not " .. tostring(scope))
    local defaults = options.defaults or {}
    assert(type(defaults) == 'table', 'vana.settings.register: defaults must be a table')
    local _, why = json.encode(defaults)
    if why ~= nil then
        error('vana.settings.register: defaults can\'t be saved as JSON: ' .. why, 2)
    end
    assert(options.changed == nil or type(options.changed) == 'function', 'vana.settings.register: changed must be a function')
    for _, other in ipairs(stores) do
        if other.scope == scope and other.name:lower() == name:lower() then
            error(('vana.settings.register: %s (%s) is registered already: use the store it gave'):format(name, scope), 2)
        end
    end
    local store = setmetatable({
        name = name, scope = scope, defaults = copy(defaults), data = {}, changed = options.changed,
    }, Store)
    refresh()
    load_store(store)
    stores[#stores + 1] = store
    if scope == 'character' then
        watch_character()
    end
    return store
end

-- vana.settings.character(): the logged in character's folder name, or nil.
function vana.settings.character()
    return refresh()
end

-- vana.settings.folder([scope]): the addon's folder, or the character's ('character'); nil and why with no character.
function vana.settings.folder(scope)
    return folder_for(scope, 2)
end

-- The full path of `file` in the scope's folder, or nil and why.
local function file_path(file, scope)
    local folder, why = folder_for(scope, 3)
    if folder == nil then
        return nil, why
    end
    local name
    name, why = relative(file)
    if name == nil then
        return nil, why
    end
    return folder .. '\\' .. name
end

-- vana.settings.path(file[, scope]): where `file` is (it needn't be there), or nil and why.
function vana.settings.path(file, scope)
    return file_path(file, scope)
end

-- vana.settings.write(file, text[, scope]): writes any file of the addon's (layouts\war.xml), whole or not at all,
-- making its folders. True, or nil and why.
function vana.settings.write(file, text, scope)
    assert(type(text) == 'string', 'vana.settings.write: the text must be a string')
    local path, why = file_path(file, scope)
    if path == nil then
        return nil, why
    end
    return write_file(path, text)
end

-- vana.settings.read(file[, scope]): the file's text, or nil and why (it isn't there, no character).
function vana.settings.read(file, scope)
    local path, why = file_path(file, scope)
    if path == nil then
        return nil, why
    end
    local text = read_file(path)
    if text == nil then
        return nil, file .. ' isn\'t there'
    end
    return text
end

-- vana.settings.exists(file[, scope])
function vana.settings.exists(file, scope)
    local path = file_path(file, scope)
    return path ~= nil and vana.fs.exists(path)
end

-- vana.settings.remove(file[, scope]): deletes a file (or an empty folder). True, or nil and why.
function vana.settings.remove(file, scope)
    local path, why = file_path(file, scope)
    if path == nil then
        return nil, why
    end
    if vana.fs.remove(path) then
        return true
    end
    return nil, file .. ' couldn\'t be removed (it isn\'t there, or it\'s a folder with something in it)'
end

-- vana.settings.list([scope]): the names of the files in the folder (not the characters' folders), or nil and why.
function vana.settings.list(scope)
    local folder, why = folder_for(scope, 2)
    if folder == nil then
        return nil, why
    end
    if not vana.fs.is_dir(folder) then
        return {}
    end
    return vana.fs.list(folder, nil, 'files')
end

-------------------------------------------------------------------------------
-- Following the character, and saving at unload
-------------------------------------------------------------------------------

local next_look = 0

-- Twice a second once a per-character store is registered (watch_character): a logout or another character moves them.
local function look()
    local now = vana.time.clock()
    if now >= next_look then
        next_look = now + 0.5
        refresh()
    end
end

watch_character = function ()
    watch_character = function () end
    hooks.after('present', look)
end

-- After the addon's own unload handlers: what changed in each store since it was read or saved.
hooks.after('unload', function ()
    for _, store in ipairs(stores) do
        local ok, why = write_store(store)
        if not ok and store.file ~= nil then
            vana.log(('settings: %s not saved at unload: %s'):format(store.file, tostring(why)))
        end
    end
end)
