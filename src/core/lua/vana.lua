--[[
VanaCore's Lua runtime: runs in every addon before the addon's own file, after VanaCore.Core has
filled in the native parts of `vana` (memory, fs, chat, ui, config, time). Adds events, tasks and
print, and returns the hooks VanaCore.Core calls into (`raise`, `frame`).
--]]

local native = ...
local vana = vana

-------------------------------------------------------------------------------
-- Errors
-------------------------------------------------------------------------------

-- Kept before the addon runs: an addon's globals may replace them (SimpleLog sets `debug = false`), and then every
-- error of its would fail in the handler as "error in error handling".
local xpcall, pcall, tostring, type, unpack, maxn, traceback = xpcall, pcall, tostring, type, unpack, table.maxn, debug.traceback

-- Runs fn(...) and reports an error in it (with its traceback in the log) instead of raising it.
-- Returns fn's results, or nothing after an error.
local function protect(what, fn, ...)
    local results = { xpcall(fn, function (message)
        -- An error value whose __tostring fails would fail this handler too.
        local ok, text = pcall(tostring, message)
        text = ok and text or ('a ' .. type(message) .. ' error value')
        return { message = text, traceback = traceback(text, 2) }
    end, ...) }
    if results[1] then
        return unpack(results, 2, maxn(results))
    end
    -- When the handler above can't run (LuaJIT's "error in error handling", out of memory) xpcall gives back a
    -- plain value, not its table: indexing that string would raise anew (Ashita's sugar errors on unknown keys).
    local failure = results[2]
    if type(failure) ~= 'table' then
        failure = { message = tostring(failure) }
    end
    native.report(what, failure.message, failure.traceback)
end

-------------------------------------------------------------------------------
-- Events
-------------------------------------------------------------------------------

local handlers = {} -- event name -> list of { key, fn }, in the order they were added
local after = {}    -- event name -> VanaCore's own functions (lua/settings.lua's), run after the addon's handlers
local anonymous = 0

vana.events = {}

-- Calls fn whenever VanaCore raises the event `name`. `key` names the handler so it can be removed
-- (a second handler with the same key replaces the first); without one, fn itself is the key.
function vana.events.on(name, fn, key)
    assert(type(name) == 'string', 'vana.events.on: the event name must be a string')
    assert(type(fn) == 'function', 'vana.events.on: the handler must be a function')
    name = name:lower()
    if key == nil then
        key = fn
    end
    local list = handlers[name]
    if list == nil then
        list = {}
        handlers[name] = list
        native.subscribe(name)
    end
    for _, handler in ipairs(list) do
        if handler.key == key then
            handler.fn = fn
            return key
        end
    end
    list[#list + 1] = { key = key, fn = fn }
    return key
end

-- Removes the handler added with `key` (or the function itself) for the event `name`.
function vana.events.off(name, key)
    local list = handlers[name:lower()]
    if list == nil then
        return false
    end
    for i, handler in ipairs(list) do
        if handler.key == key or handler.fn == key then
            table.remove(list, i)
            return true
        end
    end
    return false
end

-- vana.on(name, fn, key) and vana.off(name, key): the same, shorter. Besides VanaCore's own events, the game's
-- (gameevents.hpp): 'login', 'logout', 'zone_out', 'zone_change', 'job_change', 'subjob_change', 'level_change',
-- 'target_change', 'menu_open', 'menu_close', 'main_menu_open', 'main_menu_close', 'action_menu_open',
-- 'action_menu_close', 'chat_input_open' and 'chat_input_close', each with a table (e.zone, e.job, e.name, ...).
vana.on = vana.events.on
vana.off = vana.events.off

-- Runs each handler for `name` with `...`, then what's to run after them (whether the addon has any or not). An error
-- in one is reported and the rest still run. Returns true if a handler returned true (for 'command': the command is
-- handled).
local function raise(name, ...)
    local handled = false
    local list = handlers[name]
    if list ~= nil and #list > 0 then
        for _, handler in ipairs({ unpack(list) }) do
            if protect(name, handler.fn, ...) == true then
                handled = true
            end
        end
    end
    local last = after[name]
    if last ~= nil then
        for _, fn in ipairs(last) do
            protect(name, fn, ...)
        end
    end
    return handled
end

-- Runs fn after the addon's own handlers each time the event `name` is raised: lua/settings.lua saves at 'unload' after
-- what the addon's handlers changed.
local function run_after(name, fn)
    local list = after[name]
    if list == nil then
        list = {}
        after[name] = list
    end
    list[#list + 1] = fn
end

-------------------------------------------------------------------------------
-- Tasks: functions run as coroutines from the frame loop, which can wait with
-- coroutine.sleep(seconds) and coroutine.sleepf(frames). New tasks (and ones a wait puts back)
-- join the queue as they go, so one scheduled while the queue runs runs on a later frame.
-------------------------------------------------------------------------------

local tasks = {}
local frame_count = 0
local SLEEP = {}

-- A task waiting to run (again): at `at` seconds, or at frame `frame`.
local function schedule(fn, args, seconds, frames)
    local task = {
        co = coroutine.create(function () return fn(unpack(args, 1, args.n)) end),
        at = seconds and (vana.time.clock() + seconds) or nil,
        frame = frames and (frame_count + frames) or nil,
    }
    tasks[#tasks + 1] = task
    return task
end

-- Handles what a task's coroutine did when resumed: an error is reported, a wait puts it back in
-- the queue. Returns true with the task's results if it finished.
local function after_resume(task, ok, ...)
    if not ok then
        local message = tostring((...))
        native.report('task', message, traceback(task.co, message))
        return false
    end
    if coroutine.status(task.co) == 'dead' then
        return true, ...
    end
    local marker, seconds, frames = ...
    if marker == SLEEP then
        task.at = seconds and (vana.time.clock() + seconds) or nil
        task.frame = frames and (frame_count + frames) or nil
    else
        task.at, task.frame = nil, frame_count + 1 -- a plain yield: next frame
    end
    if not task.queued then
        task.queued = true
        tasks[#tasks + 1] = task
    end
    return false
end

vana.tasks = {}

-- Runs fn(...) once, `delay` seconds from now. Returns a handle for cancel().
function vana.tasks.once(delay, fn, ...)
    local task = schedule(fn, { n = select('#', ...), ... }, tonumber(delay) or 0, nil)
    task.queued = true
    return task
end

-- Runs fn(...) once, `delay` frames from now. Returns a handle for cancel().
function vana.tasks.oncef(delay, fn, ...)
    local task = schedule(fn, { n = select('#', ...), ... }, nil, tonumber(delay) or 0)
    task.queued = true
    return task
end

-- Runs fn(...) `repeats` times: first `delay` seconds from now, then every `every` seconds.
function vana.tasks.repeating(delay, repeats, every, fn, ...)
    local args = { n = select('#', ...), ... }
    return vana.tasks.once(delay, function ()
        for i = 1, repeats do
            fn(unpack(args, 1, args.n))
            if i < repeats then
                coroutine.sleep(every)
            end
        end
    end)
end

-- As repeating, counting frames instead of seconds.
function vana.tasks.repeatingf(delay, repeats, every, fn, ...)
    local args = { n = select('#', ...), ... }
    return vana.tasks.oncef(delay, function ()
        for i = 1, repeats do
            fn(unpack(args, 1, args.n))
            if i < repeats then
                coroutine.sleepf(every)
            end
        end
    end)
end

-- Runs fn(...) straight away as a task: if it waits, the rest of it runs later. Returns true and
-- fn's results if it finished without waiting (false if it's waiting or failed).
function vana.tasks.spawn(fn, ...)
    local task = { co = coroutine.create(fn) }
    return after_resume(task, coroutine.resume(task.co, ...))
end

-- The frames this addon has seen since it loaded (what oncef and sleepf count).
function vana.tasks.frame()
    return frame_count
end

-- Stops a task from once/oncef/repeating/repeatingf before its next run.
function vana.tasks.cancel(task)
    if type(task) == 'table' then
        task.cancelled = true
    end
end

-- Inside a task: waits `seconds`, then carries on.
function coroutine.sleep(seconds)
    coroutine.yield(SLEEP, tonumber(seconds) or 0, nil)
end

-- Inside a task: waits `frames` frames, then carries on.
function coroutine.sleepf(frames)
    coroutine.yield(SLEEP, nil, tonumber(frames) or 0)
end

local function run_tasks()
    if #tasks == 0 then
        return
    end
    local now = vana.time.clock()
    local due = tasks
    tasks = {}
    for _, task in ipairs(due) do
        if task.cancelled then
            task.queued = false
        elseif (task.at ~= nil and now < task.at) or (task.frame ~= nil and frame_count < task.frame) then
            tasks[#tasks + 1] = task
        else
            task.queued = false
            after_resume(task, coroutine.resume(task.co))
        end
    end
end

-- Functions that run first in every frame (lua/net.lua's: what the network threads finished). They must not raise.
local pumps = {}

-- Once per frame, from the game's Present: the pumps, so a task waiting on a promise carries on this frame, then the
-- tasks, then the 'present' event.
local function frame()
    frame_count = frame_count + 1
    for i = 1, #pumps do
        pumps[i]()
    end
    run_tasks()
    raise('present')
end

-------------------------------------------------------------------------------
-- vana.gameui (MOCKUP, docs/addons/07-game-interface.md): the game's own interface, changed by an addon. The native half
-- (lua_api.cpp) does hide, move and elements; here, the functions a button or a taken-over item runs, called on the
-- events VanaCore raises when the game's menu item is chosen. Each returns what the native call did: true, or false and
-- 'held by X' (another addon has the one move or replace there is; this one's takes effect when it lets go).
-------------------------------------------------------------------------------

if vana.gameui ~= nil then
    local gameui = vana.gameui
    local native_add, native_replace = gameui.native_add, gameui.native_replace
    gameui.native_add, gameui.native_replace = nil, nil -- what addons see is the pairs below
    local buttons = {}  -- menu -> the function its button runs
    local replaced = {} -- item -> the function run in place of the game's

    -- gameui.add(menu, label, fn[, colour]): this addon's button in one of the game's menus ('config_menu'), `label` its
    -- text (1 to 10 letters), `colour` its button's ('blue' the game's, as left out; 'green', 'yellow', 'red', 'purple',
    -- 'grey', or '#RRGGBB'); fn() when the player chooses it. gameui.remove(menu) takes it out.
    function gameui.add(menu, label, fn, colour)
        assert(type(label) == 'string', "vana.gameui.add: the second argument must be the button's label")
        assert(type(fn) == 'function', 'vana.gameui.add: the third argument must be a function')
        assert(colour == nil or type(colour) == 'string', "vana.gameui.add: the fourth argument, the button's colour, must be a string")
        local ok, why = native_add(menu, true, label, colour)
        buttons[menu] = fn
        return ok, why
    end

    function gameui.remove(menu)
        buttons[menu] = nil
        return native_add(menu, false)
    end

    -- gameui.replace(item, fn): fn() in place of the game's action when the player chooses `item`
    -- ('party_menu.find_member').
    function gameui.replace(item, fn)
        assert(type(fn) == 'function', 'vana.gameui.replace: the second argument must be a function')
        replaced[item] = fn
        return native_replace(item, true)
    end

    function gameui.restore(item)
        replaced[item] = nil
        return native_replace(item, false)
    end

    -- gameui.show(element): drawn again after gameui.hide
    function gameui.show(element)
        return gameui.hide(element, false)
    end

    -- gameui.image(element, file[, options]): this addon's picture in place of one of the game's ('target_arrow': the
    -- arrow over the target). `file` is a PNG (or BMP, JPEG) relative to the addon's folder, or a full path. options:
    -- width and height in the interface's units (left out: the picture's pixels as units; the game's arrow is 20 x 32),
    -- x and y to move it from the game's anchor (the arrow's point, at the picture's bottom middle) in units, color
    -- '#RRGGBB' to tint it, opacity 0 to 1. gameui.image(element) gives the game its own picture back.
    local native_image = gameui.native_image
    gameui.native_image = nil
    function gameui.image(element, file, options)
        if file == nil then
            return native_image(element)
        end
        assert(type(file) == 'string', "vana.gameui.image: the second argument must be the picture's file name")
        assert(options == nil or type(options) == 'table', 'vana.gameui.image: the third argument must be a table of options')
        options = options or {}
        return native_image(element, file, options.width, options.height, options.x, options.y, options.color or options.colour, options.opacity)
    end

    -- Triggers on the game's menus. gameui.on(menu, event[, items], fn) runs fn(e) at the next frame when the player does
    -- `event` in `menu` (the game acts as well); gameui.catch(menu, event[, items], fn) does the same and keeps the event
    -- from the game, so its own action doesn't happen. `menu` is a window's name as menu_open gives it ('menuwind'), an
    -- alias ('main_menu', 'party_menu', 'config_menu'), '*' for every menu, or a list of those; `event` 'select',
    -- 'cancel', 'up', 'down', 'left', 'right', 'menu' (the menu key while a menu is up), '*' for every one, or the
    -- game's number; `items` an item id, a name VanaCore knows ('equipment'), or a list, for those items only; the
    -- function is always last. Each returns an id for gameui.off(id). e: menu, event, key (the number), item, name
    -- ('' when unknown), caught.
    local native_on, native_off = gameui.native_on, gameui.native_off
    gameui.native_on, gameui.native_off = nil, nil
    local triggers = {} -- id -> fn
    local EVENTS = { up = 1, down = 2, right = 3, left = 4, select = 5, cancel = 6, menu = 0x1C }
    local EVENT_NAMES = {}
    for name, code in pairs(EVENTS) do
        EVENT_NAMES[code] = name
    end
    local function register(what, menu, event, items, fn)
        if fn == nil and type(items) == 'function' then -- (menu, event, fn): no item filter
            items, fn = nil, items
        end
        assert(type(menu) == 'string' or type(menu) == 'table', 'vana.gameui.' .. what .. ": the first argument must name a menu ('menuwind', 'main_menu', '*') or list them")
        local code = (event == nil or event == '*') and 0 or EVENTS[event] or tonumber(event)
        assert(code ~= nil, 'vana.gameui.' .. what .. ": the second argument must be 'select', 'cancel', 'up', 'down', 'left', 'right', 'menu', '*' or an event number")
        assert(type(fn) == 'function', 'vana.gameui.' .. what .. ': the last argument must be the function to run')
        if items ~= nil and type(items) ~= 'table' then
            items = { items }
        end
        local id = native_on(menu, code, items, what == 'catch')
        triggers[id] = fn
        return id
    end

    function gameui.on(menu, event, items, fn)
        return register('on', menu, event, items, fn)
    end

    function gameui.catch(menu, event, items, fn)
        return register('catch', menu, event, items, fn)
    end

    function gameui.off(id)
        triggers[id] = nil
        return native_off(id)
    end

    -- gameui.press(menu, row[, event]): the player's choice made for them in one of the game's windows ('menuwind', as
    -- menu_open and gameui.windows() name them): its cursor put on row `row` (a row's id as gameui.window gives it; nil
    -- leaves the cursor where it is) and `event` ('select' when left out; 'cancel', 'up', 'down', 'left', 'right', 'menu'
    -- or the game's number) given to it as the keys give it. -> true, or false and why ('menuwind' isn't up, has no row 40)
    local native_press = gameui.native_press
    gameui.native_press = nil
    function gameui.press(menu, row, event)
        assert(type(menu) == 'string', "vana.gameui.press: the first argument must name a window ('menuwind')")
        assert(row == nil or type(row) == 'number', "vana.gameui.press: the second argument must be a row's id or nil")
        local code = event == nil and EVENTS.select or EVENTS[event] or tonumber(event)
        assert(code ~= nil, "vana.gameui.press: the third argument must be 'select', 'cancel', 'up', 'down', 'left', 'right', 'menu' or an event number")
        return native_press(menu, row or 0, code)
    end

    -- gameui.watch(menu, fn): fn(e) in the same frame as a draw of the window (`menu` a window's name, 'main_menu',
    -- 'party_menu', 'config_menu', '*' for every window, or a list of those): e.what 'open' at its first draw (since it opened,
    -- or since the watch began), 'change' at a draw that shows anything else (its place, cursor, rows, text, pictures),
    -- 'close' once when it closes; e.name the window's name; e.window gameui.window(e.name), whole (drawn), nil at 'close'.
    -- Returns an id for gameui.unwatch(id). A watched window's draws are kept for as long as it's watched.
    local native_watch = gameui.native_watch
    gameui.native_watch = nil
    local ALIASES = { main_menu = { 'menuwind', 'socialme' }, party_menu = { 'prty1' }, config_menu = { 'configwi' } }
    local watches, watch_ids = {}, 0 -- id -> { menus = { name = true }, fn }
    local function rewatch()
        local names, seen = {}, {}
        for _, w in pairs(watches) do
            for name in pairs(w.menus) do
                if not seen[name] then
                    seen[name] = true
                    names[#names + 1] = name
                end
            end
        end
        native_watch(names)
    end
    function gameui.watch(menu, fn)
        assert(type(menu) == 'string' or type(menu) == 'table', "vana.gameui.watch: the first argument must name a window ('configwi', 'main_menu', '*') or list them")
        assert(type(fn) == 'function', 'vana.gameui.watch: the second argument must be the function to run')
        local menus = {}
        for _, name in ipairs(type(menu) == 'table' and menu or { menu }) do
            name = tostring(name):gsub('^menu    ', ''):gsub('%s+$', '')
            for _, each in ipairs(ALIASES[name] or { name }) do
                menus[each] = true
            end
        end
        watch_ids = watch_ids + 1
        watches[watch_ids] = { menus = menus, fn = fn }
        rewatch()
        return watch_ids
    end
    function gameui.unwatch(id)
        if watches[id] ~= nil then
            watches[id] = nil
            rewatch()
        end
    end
    vana.events.on('gameui_window', function (e)
        local w = nil
        if e.what ~= 'close' then
            w = gameui.window(e.element)
        end
        local fns = {} -- (taken first: a function may watch or unwatch)
        for _, watch in pairs(watches) do
            if watch.menus['*'] or watch.menus[e.element] then
                fns[#fns + 1] = watch.fn
            end
        end
        for _, fn in ipairs(fns) do
            fn({ what = e.what, name = e.element, window = w })
        end
    end, '__gameui_window')

    vana.events.on('gameui_trigger', function (e)
        local fn = triggers[e.trigger]
        if fn ~= nil then
            e.event = EVENT_NAMES[e.key] or e.key
            fn(e)
        end
    end, '__gameui_trigger')

    vana.events.on('gameui_button', function (e)
        local fn = buttons[e.element]
        if fn ~= nil then
            fn()
        end
    end, '__gameui_button')
    vana.events.on('gameui_replaced', function (e)
        local fn = replaced[e.element]
        if fn ~= nil then
            fn()
        end
    end, '__gameui_replaced')
end

-------------------------------------------------------------------------------
-- vana.gamedata: the game's static tables (spells, abilities, items, zones, status effects, ...) as Lua, generated
-- from the game's DATs and CatsEyeXI's server by CatsEyeXI's dats repository (tools/gen_gamedata.py) into addons\vanacore\libs\gamedata.
-- vana.gamedata.spells[id] loads spells.lua the first time it's asked for; a table in parts (items) loads the part
-- an id is in. vana.gamedata.find(table, name) gives an id by name, vana.gamedata.all(table) the whole table (every
-- part loaded, so pairs() walks it all), vana.gamedata.tables() what's there. docs/addons/04-game-data.md.
-------------------------------------------------------------------------------

do
    local folder = vana.paths.addons .. '\\vanacore\\libs\\gamedata\\'
    local manifest

    local function load_file(name)
        local chunk, err = loadfile(folder .. name .. '.lua')
        if chunk == nil then
            return nil, err
        end
        local ok, data = pcall(chunk)
        if not ok then
            return nil, data
        end
        return data
    end

    local function entry(name)
        if manifest == nil then
            manifest = load_file('manifest') or { tables = {} }
        end
        return manifest.tables[name]
    end

    local parted = {} -- name -> { parts = { [part] = true | false }, meta }
    local gamedata = setmetatable({}, {
        __index = function (t, name)
            if type(name) ~= 'string' then
                return nil
            end
            local meta = entry(name)
            if meta == nil then
                return nil
            end
            local data
            if meta.part_size ~= nil then
                local state = { parts = {}, meta = meta }
                parted[name] = state
                data = setmetatable({}, {
                    __index = function (rows, id)
                        if type(id) ~= 'number' then
                            return nil
                        end
                        local part = math.floor(id / meta.part_size)
                        if state.parts[part] == nil then
                            state.parts[part] = false
                            if meta.parts[part] ~= nil then
                                local loaded, err = load_file(name .. '_' .. part)
                                if loaded ~= nil then
                                    for k, v in pairs(loaded) do
                                        rawset(rows, k, v)
                                    end
                                    state.parts[part] = true
                                elseif err ~= nil then
                                    vana.log(('vana.gamedata.%s: part %d failed to load: %s'):format(name, part, tostring(err)))
                                end
                            end
                        end
                        return rawget(rows, id)
                    end,
                })
            else
                local err
                data, err = load_file(name)
                if data == nil then
                    if err ~= nil then
                        vana.log(('vana.gamedata.%s failed to load: %s'):format(name, tostring(err)))
                    end
                    return nil
                end
            end
            rawset(t, name, data)
            return data
        end,
    })

    -- gamedata.all(name): the table with every part loaded, so pairs() walks all of it (items: 25,000 rows, ~1 s the
    -- first time); nil for a table that isn't there.
    function gamedata.all(name)
        local data = gamedata[name]
        local state = parted[name]
        if data ~= nil and state ~= nil then
            for part in pairs(state.meta.parts) do
                if state.parts[part] == nil then
                    local _ = data[part * state.meta.part_size] -- loads the part
                end
            end
        end
        return data
    end

    -- gamedata.tables(): { name = { rows, generated, part_size?, parts? } }
    function gamedata.tables()
        entry('')
        return manifest.tables
    end

    -- gamedata.find(table, text): the id of the row named `text` (case-insensitive; items by their log name too), or
    -- nil. The first call on a table builds its name index (every part loaded).
    local indexes = {}
    function gamedata.find(name, text)
        if type(text) ~= 'string' then
            return nil
        end
        local index = indexes[name]
        if index == nil then
            local data = gamedata.all(name)
            if data == nil then
                return nil
            end
            index = {}
            for id, row in pairs(data) do
                if type(row) == 'table' then
                    for _, key in ipairs({ 'name', 'log_name', 'server_name', 'display' }) do
                        local value = row[key]
                        if type(value) == 'string' and value ~= '' then
                            local lower = value:lower()
                            if index[lower] == nil then
                                index[lower] = id
                            end
                        end
                    end
                end
            end
            indexes[name] = index
        end
        return index[text:lower()]
    end

    -- gamedata.item_stats(id[, extra]) or (slot): an item's stats as the Equipment window counts them (equipstats_lua.h);
    -- a container's slot ({ id, extra }) gives its augments too. nil, why while the data isn't read.
    local native_item_stats = vana.game.native_item_stats
    vana.game.native_item_stats = nil
    function gamedata.item_stats(item, extra)
        if type(item) == 'table' then
            return native_item_stats(item.id, item.extra)
        end
        return native_item_stats(item, extra)
    end

    vana.gamedata = gamedata
end

-------------------------------------------------------------------------------
-- vana.game.request_max_hp(ids, callback): asks CatsEyeXI's server for the maximum HP of entities by server id (maxhp_ask.hpp,
-- docs/addons/04-game-data.md "Maximum HP and MP"). The native half (lua_maxhp.cpp) sends the request and keeps the answers;
-- here, the callback: called once, in a following frame, as callback(answers) with { id, hp, hp_max, status } for each id asked.
-------------------------------------------------------------------------------

if vana.game ~= nil and vana.game.native_request_max_hp ~= nil then
    local vg = vana.game
    local native_request, native_answers = vg.native_request_max_hp, vg.native_max_hp_answers
    vg.native_request_max_hp, vg.native_max_hp_answers = nil, nil
    local waiting = {} -- token -> the callback

    -- request_max_hp(ids, callback): `ids` a server id or a list of up to 8; true when asked, or false and why not.
    function vg.request_max_hp(ids, callback)
        assert(type(callback) == 'function', 'vana.game.request_max_hp: the second argument must be a function')
        local token, why = native_request(ids)
        if token == nil then
            return false, why
        end
        waiting[token] = callback
        return true
    end

    pumps[#pumps + 1] = function ()
        if next(waiting) == nil then
            return
        end
        local ready = {}
        for token in pairs(waiting) do
            local answers = native_answers(token)
            if answers ~= nil then
                ready[#ready + 1] = { waiting[token], answers }
                waiting[token] = nil
            end
        end
        for i = 1, #ready do
            protect('request_max_hp', ready[i][1], ready[i][2])
        end
    end
end

-------------------------------------------------------------------------------
-- print: to the game's chat log (and VanaCore's log).
-------------------------------------------------------------------------------

function print(...)
    local parts = {}
    for i = 1, select('#', ...) do
        parts[#parts + 1] = tostring((select(i, ...)))
    end
    vana.chat.write(table.concat(parts, ' '))
end

return {
    raise = raise,
    frame = frame,
    protect = protect,
    after = run_after,
    report = native.report,
    pump = function (fn) pumps[#pumps + 1] = fn end,
}
