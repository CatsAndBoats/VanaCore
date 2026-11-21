-- vana.settings and vana.json (src\core\lua\settings.lua), run with src\core\lua\vana.lua as an addon gets them, over a
-- stand-in for the native vana (files in a scratch folder, a character the test logs in and out). run.cmd builds
-- luarun.exe and runs this:
--   luarun settings_test.lua <vana.lua> <settings.lua> <scratch folder>
local vana_path, settings_path, scratch = arg[1], arg[2], arg[3] or 'settings_test_scratch'

local say = print -- vana.lua makes print the chat log's
local failures, checks = 0, 0
local function check(ok, what, detail)
    checks = checks + 1
    if not ok then
        failures = failures + 1
        say('FAIL ' .. what .. (detail ~= nil and (': ' .. tostring(detail)) or ''))
    end
end

-------------------------------------------------------------------------------
-- The native side, stood in for: VanaCore's paths use \, which off Windows are made /
-------------------------------------------------------------------------------

local windows = package.config:sub(1, 1) == '\\'
local function real(path)
    return windows and path or (path:gsub('\\', '/'))
end
local open, remove, rename = io.open, os.remove, os.rename
io.open = function (path, ...) return open(real(path), ...) end
os.remove = function (path) return remove(real(path)) end
os.rename = function (from, to) return rename(real(from), real(to)) end

local function shell_quote(path)
    return '"' .. real(path) .. '"'
end
if windows then
    os.execute('rmdir /s /q ' .. shell_quote(scratch) .. ' 2>nul')
    os.execute('mkdir ' .. shell_quote(scratch))
else
    os.execute('rm -rf ' .. shell_quote(scratch) .. ' && mkdir -p ' .. shell_quote(scratch))
end

local dirs = {}
local character_name, character_id = nil, 0
local character_asks = 0
local refuse_writes = nil -- a pattern: vana.fs.write fails for paths it matches
local clock = 0
local chat, logged = {}, {}

local function read(path)
    local f = io.open(path, 'rb')
    if f == nil then
        return nil
    end
    local text = f:read('*a')
    f:close()
    return text
end

local function put(path, text)
    local f = assert(io.open(path, 'wb'))
    f:write(text)
    f:close()
end

vana = {
    paths = { install = scratch },
    time = { clock = function () return clock end },
    chat = { write = function (text) chat[#chat + 1] = text end },
    log = function (text) logged[#logged + 1] = text end,
    game = {
        character = function ()
            character_asks = character_asks + 1
            if character_name == nil then
                return nil
            end
            return character_name, character_id
        end,
    },
    fs = {
        create_dir = function (path)
            os.execute((windows and 'mkdir ' or 'mkdir -p ') .. shell_quote(path) .. (windows and ' 2>nul' or ''))
            dirs[real(path)] = true
            return true
        end,
        is_dir = function (path) return dirs[real(path)] == true end,
        exists = function (path) return read(path) ~= nil or dirs[real(path)] == true end,
        remove = function (path) return os.remove(path) ~= nil end,
        write = function (path, text)
            if refuse_writes and path:find(refuse_writes) then
                return nil, 'can\'t write ' .. path .. ' (Windows error 32)'
            end
            put(path .. '.tmp', text)
            os.remove(path)
            return os.rename(path .. '.tmp', path) ~= nil
        end,
        list = function (folder, _, which)
            assert(which == 'files', 'settings.lua lists files')
            local names = {}
            local pipe = io.popen(windows and ('dir /b /a-d ' .. shell_quote(folder) .. ' 2>nul')
                or ('find ' .. shell_quote(folder) .. ' -maxdepth 1 -type f -printf "%f\\n"'))
            for line in pipe:lines() do
                names[#names + 1] = line
            end
            pipe:close()
            table.sort(names)
            return names
        end,
    },
}

local reports = {}
local native = {
    report = function (what, message) reports[#reports + 1] = what .. ': ' .. tostring(message) end,
    subscribe = function () end,
}

local hooks = assert(loadfile(vana_path))(native)
assert(loadfile(settings_path))(hooks, 'Sidekick')

local ROOT = scratch .. '\\settings\\addons\\Sidekick'
local function frame(seconds)
    clock = clock + (seconds or 1)
    hooks.frame()
end

-------------------------------------------------------------------------------
-- JSON
-------------------------------------------------------------------------------

local json = vana.json
check(json ~= nil, 'vana.json is there')

local text = json.encode({ b = 1, a = { 3, 2, 1 }, c = { x = true, y = false }, e = {}, s = 'q"\\\n\1' })
check(text == '{\n  "a": [\n    3,\n    2,\n    1\n  ],\n  "b": 1,\n  "c": {\n    "x": true,\n    "y": false\n  },\n'
    .. '  "e": {},\n  "s": "q\\"\\\\\\n\\u0001"\n}\n', 'encode: sorted keys, two-space indents, escapes', text)

local value = json.decode(text)
check(value and value.b == 1 and value.a[1] == 3 and value.a[3] == 1 and value.c.x == true and value.c.y == false
    and type(value.e) == 'table' and value.s == 'q"\\\n\1', 'decode reads back what encode wrote')

value = json.decode(json.encode({ [230] = 'Ru\'Lude', [12] = 'Gusgen', name = 'x' }))
check(value and value[230] == 'Ru\'Lude' and value[12] == 'Gusgen' and value.name == 'x',
    'whole-number keys come back as numbers (favourites by zone id)')
value = json.decode('{"07": 1, "1.5": 2, "-3": 3, "0": 4}')
check(value and value['07'] == 1 and value['1.5'] == 2 and value[-3] == 3 and value[0] == 4,
    'only keys written as whole numbers become numbers')

check(json.encode(4294967295) == '4294967295\n', 'a server id is written exactly', json.encode(4294967295))
check(json.decode(json.encode(0.1)) == 0.1 and json.decode(json.encode(1 / 3)) == 1 / 3, 'fractions read back exactly')
check(json.encode(-2.5) == '-2.5\n', 'a short fraction stays short', json.encode(-2.5))

check(json.decode('"\\u00e9\\ud83d\\ude00"') == '\195\169\240\159\152\128', '\\u escapes, a surrogate pair, to UTF-8')
check(json.decode('\239\187\191{"a": 1}').a == 1, 'a byte order mark is skipped')
check(select(2, json.decode('[1, 2,\n  3,\n  x]')) == 'line 3: unexpected x', 'a bad file says which line',
    select(2, json.decode('[1, 2,\n  3,\n  x]')))
check(select(2, json.decode('{"a": 1} 2')) ~= nil, 'trailing text is an error')
check(select(2, json.decode('{"a": "b')) ~= nil, 'an unclosed string is an error')
check(json.decode('[1, null, 3]')[3] == 3, 'null in a list leaves a hole')

local _, why = json.encode({ f = print })
check(why and why:find('value.f', 1, true) and why:find('function', 1, true), 'a function says where it is', why)
local loop = {}
loop.self = loop
_, why = json.encode(loop)
check(why and why:find('holds itself', 1, true), 'a table inside itself is refused', why)
_, why = json.encode({ 0 / 0 })
check(why ~= nil, 'NaN is refused')
_, why = json.encode({ [true] = 1 })
check(why ~= nil, 'a boolean key is refused')

-------------------------------------------------------------------------------
-- A global store
-------------------------------------------------------------------------------

local defaults = { scale = 1, favorites = { 'Jeuno' }, window = { x = 10, y = 20 } }
local global = vana.settings.register({ defaults = defaults })
local file = ROOT .. '\\settings.json'
check(global:path() == file, 'a global store is settings\\addons\\<Addon>\\settings.json', global:path())
check(global.ready, 'a global store is ready at once')
check(global.data.scale == 1 and global.data.window.x == 10 and global.data.favorites[1] == 'Jeuno', 'it starts with its defaults')
check(read(file) == json.encode(defaults), 'its file is made from the defaults', read(file))
check(global.data.window ~= defaults.window, 'the defaults are copied, not shared')

global.data.scale = 2
check(global:save() == true and json.decode(read(file)).scale == 2, 'save writes it')

check(not pcall(vana.settings.register, { defaults = {} }), 'registering the same name twice is refused')
check(not pcall(vana.settings.register, { name = 'a/b' }), 'a store name with a folder is refused')
check(not pcall(vana.settings.register, { name = 'x.json' }), 'a store name with .json is refused')
check(not pcall(vana.settings.register, { scope = 'account' }), 'an unknown scope is refused')
check(not pcall(vana.settings.register, { name = 'bad', defaults = { f = print } }), 'defaults JSON can\'t hold are refused')

-- A file edited by hand while the game runs: reload takes it up, and missing defaults are filled in (lists aren't merged)
put(file, '{ "scale": 3, "favorites": [ "Bastok" ], "window": { "x": 5 }, "extra": "kept" }')
local data = global.data
global:reload()
check(global.data == data, 'reload keeps the same table')
check(data.scale == 3 and data.window.x == 5 and data.window.y == 20 and data.extra == 'kept',
    'reload reads the file and fills in missing defaults', json.encode(data))
check(#data.favorites == 1 and data.favorites[1] == 'Bastok', 'a list is the file\'s, not merged with the default list')
check(json.decode(read(file)).window.y == 20, 'the file gets the default it lacked')

-- Unload: after the addon's own handlers, what changed is saved; what didn't change isn't written
vana.events.on('unload', function () global.data.closed_at = 42 end)
hooks.raise('unload')
check(json.decode(read(file)).closed_at == 42, 'unload saves what the addon\'s unload handler changed')
put(file, '{ "scale": 7 }')
hooks.raise('unload')
check(json.decode(read(file)).scale == 7 and json.decode(read(file)).closed_at == nil,
    'unload with nothing changed leaves a hand edit alone', read(file))
global:reload()
check(global.data.scale == 7 and global.data.window.x == 10, 'reload after a hand edit')

check(global:reset() == true and json.decode(read(file)).scale == 1 and global.data.scale == 1, 'reset puts the defaults back')

local asked = character_asks
frame(1)
frame(1)
check(character_asks == asked, 'with only global stores, frames don\'t look for the character')

-------------------------------------------------------------------------------
-- A per-character store
-------------------------------------------------------------------------------

local swaps = {}
local per = vana.settings.register({
    scope = 'character', defaults = { favorites = {}, opacity = 0.8 },
    changed = function (d, store) swaps[#swaps + 1] = (vana.settings.character() or '-') .. ':' .. tostring(d.opacity) end,
})
local mine = per.data
check(not per.ready and per:path() == nil, 'before a character is in, a per-character store isn\'t ready')
check(per.data.opacity == 0.8, 'it holds its defaults meanwhile')
local ok
ok, why = per:save()
check(ok == nil and why == 'no character is logged in', 'and saving it says why', why)
check(vana.settings.character() == nil, 'no character yet')

frame(1) -- a look with no character in: the next is half a second on
character_name, character_id = 'Yuna', 12345
frame(0.1)
check(not per.ready, 'the character is looked for twice a second, not every frame')
frame(1)
local yuna = ROOT .. '\\Yuna\\settings.json'
check(per.ready and per:path() == yuna, 'a character logging in moves it to settings\\addons\\<Addon>\\<Character>\\', per:path())
check(read(yuna) ~= nil, 'the character\'s file is made from the defaults')
check(swaps[1] == 'Yuna:0.8', 'changed is called with the character\'s settings', swaps[1])
check(per.data == mine, 'the data table is the same one the addon holds')

per.data.favorites[1] = 230
per.data.opacity = 0.5
character_name, character_id = 'Seekey', 777
frame(1)
check(json.decode(read(yuna)).opacity == 0.5 and json.decode(read(yuna)).favorites[1] == 230,
    'switching characters saves what changed to the first character\'s file', read(yuna))
check(per:path() == ROOT .. '\\Seekey\\settings.json' and per.data.opacity == 0.8 and #per.data.favorites == 0,
    'the second character gets its own settings')
check(swaps[2] == 'Seekey:0.8', 'changed again for the second', swaps[2])

per.data.opacity = 0.3
character_name = nil
frame(1)
check(json.decode(read(ROOT .. '\\Seekey\\settings.json')).opacity == 0.3, 'a logout saves the character\'s changes')
check(not per.ready and per.data.opacity == 0.8, 'after a logout it holds its defaults')

character_name, character_id = 'Yuna', 12345
frame(1)
check(per.data.opacity == 0.5 and per.data.favorites[1] == 230, 'logging back in reads the character\'s file again')

-- A broken file is kept aside and the defaults used
character_name, character_id = 'Broken', 1
vana.fs.create_dir(ROOT .. '\\Broken')
put(ROOT .. '\\Broken\\settings.json', '{ "opacity": ')
frame(1)
check(per.data.opacity == 0.8, 'a file that isn\'t JSON gives the defaults')
check(read(ROOT .. '\\Broken\\settings.json.broken') == '{ "opacity": ', 'and is kept as settings.json.broken')
check(chat[#chat] and chat[#chat]:find('settings.json.broken', 1, true), 'and the player is told', chat[#chat])
check(json.decode(read(ROOT .. '\\Broken\\settings.json')).opacity == 0.8, 'a new file is made from the defaults')

-- A broken file whose copy can't be written (the folder read-only, say) is left alone, not saved over
character_name, character_id = 'Held', 2
vana.fs.create_dir(ROOT .. '\\Held')
put(ROOT .. '\\Held\\settings.json', '{ "opacity": 0.1, ')
refuse_writes = '%.broken$'
frame(1)
check(per.data.opacity == 0.8 and read(ROOT .. '\\Held\\settings.json') == '{ "opacity": 0.1, ',
    'when the copy can\'t be kept, the defaults are used and the file is left as it is')
ok, why = per:save()
check(ok == nil and why and why:find('isn\'t saved over', 1, true), 'and save won\'t write over it', why)
per.data.opacity = 0.2
hooks.raise('unload')
check(read(ROOT .. '\\Held\\settings.json') == '{ "opacity": 0.1, ', 'nor will unload')
refuse_writes = nil
per:reload()
check(read(ROOT .. '\\Held\\settings.json.broken') == '{ "opacity": 0.1, ' and per:save() == true,
    'a reload that can keep the copy writes again')

-------------------------------------------------------------------------------
-- Other files
-------------------------------------------------------------------------------

character_name, character_id = 'Yuna', 12345
check(vana.settings.write('layouts/war.xml', '<war/>') == true, 'write makes folders under the addon\'s folder')
check(read(ROOT .. '\\layouts\\war.xml') == '<war/>', 'write puts the file there')
check(vana.settings.read('layouts\\war.xml') == '<war/>', 'read gets it back (either slash)')
check(vana.settings.exists('layouts/war.xml') and not vana.settings.exists('layouts/nin.xml'), 'exists')
check(vana.settings.write('notes.txt', 'hi', 'character') == true and read(ROOT .. '\\Yuna\\notes.txt') == 'hi',
    'character scope writes in the character\'s folder')
check(vana.settings.read('notes.txt', 'character') == 'hi' and vana.settings.read('notes.txt') == nil,
    'global and character files are apart')
check(vana.settings.path('a.txt', 'character') == ROOT .. '\\Yuna\\a.txt', 'path', vana.settings.path('a.txt', 'character'))
check(vana.settings.folder() == ROOT and vana.settings.folder('character') == ROOT .. '\\Yuna', 'folder')
local names = vana.settings.list()
check(type(names) == 'table' and #names == 1 and names[1] == 'settings.json', 'list: the files, not the characters\' folders',
    table.concat(names or {}, ','))
check(vana.settings.remove('notes.txt', 'character') == true and read(ROOT .. '\\Yuna\\notes.txt') == nil, 'remove')
check(vana.settings.remove('notes.txt', 'character') == nil, 'removing what isn\'t there says so')

for _, bad in ipairs({ '../escape.txt', '..\\escape.txt', 'C:\\x.txt', '\\x.txt', '/x.txt', 'con.txt', 'COM1',
                       'a?b', 'a*b', 'a<b', 'trailing.', 'trailing ', '', 'a/../b' }) do
    local written, reason = vana.settings.write(bad, 'x')
    check(written == nil and type(reason) == 'string', 'refused: ' .. bad, reason)
end
check(not pcall(vana.settings.folder, 'account'), 'an unknown scope is an error')

character_name = nil
ok, why = vana.settings.write('notes.txt', 'x', 'character')
check(ok == nil and why == 'no character is logged in', 'character scope with no character says why', why)
check(vana.settings.folder('character') == nil, 'no character folder with no character')

-------------------------------------------------------------------------------
-- vana.lua: what runs after the handlers
-------------------------------------------------------------------------------

local order = {}
hooks.after('custom', function () order[#order + 1] = 'after' end)
check(hooks.raise('custom') == false and order[1] == 'after', 'after runs with no handlers, and nothing is handled')
vana.events.on('custom', function () order[#order + 1] = 'handler'; return true end)
check(hooks.raise('custom') == true and order[2] == 'handler' and order[3] == 'after',
    'after runs after the handlers, and a handled event stays handled')
hooks.after('custom', function () error('boom') end)
hooks.after('custom', function () order[#order + 1] = 'still' end)
hooks.raise('custom')
check(order[#order] == 'still' and reports[#reports]:find('boom', 1, true), 'an error in one is reported and the rest run')

say(('%d checks, %d failed'):format(checks, failures))
os.exit(failures == 0 and 0 or 1)
