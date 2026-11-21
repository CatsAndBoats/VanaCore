--[[
vana\copy.lua: Client > Characters' three copies, the Main's to the Alts (the other games running on this PC, clients.cpp):

  graphics  every setting of the Graphics and Experimental pages (a Graphics preset's, Mass Grass with them) and Client >
            Game's renderer, background resolution, frame rate and picture rows. Not the window: its mode, resolution, UI
            resolution and place stay the Alt's.
  options   every setting of the Game Options and HUD pages: the interface, True Font, chat, menus, the world, camera and
            damage numbers, the game's windows' places, sizes, frames and Hide switches.
  addons    the addons and plugins the Main has loaded, loaded in the Alt where they aren't (none is unloaded); each addon's
            settings for the Main's character made the Alt character's; and the addons' and plugins' windows put where
            the Main has them (Addons > Layout; in proportion on a screen of another size, share.fit).

A setting copied is the Main's exactly: one the Main leaves unset (its default) is taken out of the Alt's file too. The
loaded preset's name ([vana] preset, hud.preset) goes with its kind.

The Main's game makes what's sent, whichever game the button was pressed in: pressed in an Alt, the Main is asked first
(vana.ipc: the 'ipc' event reaches this addon in every other game). It's written to config\vanacore\copy-<alt pid>.txt in the
Main's install and the Alt is told where; the Alt takes it up, deletes it, and says what changed in its chat log (and in the
Main's).

An addon's settings are the files the Ashita settings library wrote in config\addons\<addon>\<Name>_<server id>\ (an Ashita
addon's) and the files in settings\addons\<Addon>\<Name>\ (a VanaCore addon's, vana.settings). Not the files an addon writes
itself (LuAshitacast's job files, xiui's satchel state, tCrossBar's binding folders), nor Windower addons' (one
settings.xml for every character), nor plugins' (theirs are one file for every character already). An addon whose settings
change is unloaded first (its unload handlers save what it holds, which would write over the copy), the files written, then
it's loaded again. Each file of the Alt's that's written over is kept in config\before-copy\<date-time>\, at the same path
under it.

Loaded by vana.lua with `local M = ...`; the functions are M.copy.
--]]

local M = ...
local copy = {}

local FORMAT = 'VCC1'
local ASK_WAIT = 6      -- seconds an Alt waits for the Main to answer
local UNLOAD_MIN = 1    -- seconds after the unloads before the files are written (their unload handlers have run)
local UNLOAD_WAIT = 6   -- at most, for an addon that doesn't go
local PLACE_AFTER = 3   -- seconds after the last load before the windows are placed (the addons have drawn theirs)
-- never unloaded for a copy: this window, and the addon CatsEyeXI requires (data.lua's extension_rows: fixed)
local KEEP = { vana = true, customcolors = true }

local NAMES = { graphics = 'Graphics settings', options = 'Game Options', addons = 'addons and plugins' }

local asking = nil -- { kind, at, main }: this Alt asked the Main and waits for its answer
local job = nil    -- the addons copy under way in this game (start_addons)

local function clock()
    return vana.time.clock()
end

local function say(text)
    print('VanaCore: ' .. text)
end

-------------------------------------------------------------------------------
-- Text: one record a line, its fields between tabs; a %, tab, CR or LF in a field written %25, %09, %0D, %0A
-------------------------------------------------------------------------------

local function esc(text)
    return (tostring(text):gsub('[%%\t\r\n]', function (c) return string.format('%%%02X', c:byte()) end))
end

local function unesc(text)
    return (text:gsub('%%(%x%x)', function (h) return string.char(tonumber(h, 16)) end))
end

local function split(line)
    local out = {}
    for field in (line .. '\t'):gmatch('([^\t]*)\t') do
        out[#out + 1] = unesc(field)
    end
    return out
end

local function record(...)
    local fields = { ... }
    for i = 2, #fields do
        fields[i] = esc(fields[i])
    end
    return table.concat(fields, '\t')
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

-- the folder `path` is in
local function folder_of(path)
    return path:match('^(.*)\\[^\\]+$') or path
end

-- an addon's name as its spellings compare: lower case, letters and digits (the folder goldilox-ui, the settings Goldilox-UI)
local function norm(name)
    return ((name or ''):lower():gsub('[^%w]', ''))
end

-------------------------------------------------------------------------------
-- The games
-------------------------------------------------------------------------------

-- The Main (the game vanacore.ini names, else this one, as Characters shows it), the other games, and this one
local function games()
    local list = vana.clients.list()
    local main, me, alts = nil, nil, {}
    for _, game in ipairs(list) do
        if game.main then
            main = game
        end
        if game.self then
            me = game
        end
    end
    main = main or me
    for _, game in ipairs(list) do
        if game ~= main then
            alts[#alts + 1] = game
        end
    end
    return main, alts, me
end

local function self_pid()
    local _, _, me = games()
    return me and me.pid or 0
end

-- this game's character: { name, id } (the server id as text: it can be past an integer's 31 bits), nil before it's in
local function character()
    local name, id = vana.game.character()
    if name == nil or name == '' or id == nil then
        return nil
    end
    return { name = name, id = string.format('%.0f', id) }
end

-- the folders an addon keeps a character's settings in: the Ashita settings library's and vana.settings'
local function ashita_folder(char)
    return char.name .. '_' .. char.id
end

local function vanacore_folder(char)
    local folder = char.name:gsub('[^%w_%-]', '')
    return folder ~= '' and folder or char.id
end

-- a file the Ashita settings library wrote (not one an addon wrote itself: a LuAshitacast job file, a hand-made config)
local function settings_file(text)
    return text:match("^require%('common'%);%s*local settings = T{%s*};") ~= nil
end

-------------------------------------------------------------------------------
-- The Main: what's sent
-------------------------------------------------------------------------------

local function settings_records(kind, lines)
    M.flush()
    for _, k in ipairs(M.copy_keys(kind)) do
        local value = vana.config.get(k.section, k.key)
        if value ~= nil and value ~= '' then
            lines[#lines + 1] = record('s', k.section, k.key, k.restart and '1' or '0', value)
        else
            lines[#lines + 1] = record('u', k.section, k.key, k.restart and '1' or '0')
        end
    end
end

local function addons_records(lines, me)
    lines[#lines + 1] = record('install', vana.paths.install)
    lines[#lines + 1] = record('char', me.name, me.id)
    for _, a in ipairs(vana.addons.loaded()) do
        lines[#lines + 1] = record('a', a.source, a.name)
    end
    for _, p in ipairs(vana.plugins.loaded()) do
        lines[#lines + 1] = record('p', p.source, p.name)
    end
    local screen, windows = M.layout_now()
    if screen ~= nil then
        local num = M.share.num
        lines[#lines + 1] = record('screen', num(screen[1]), num(screen[2]))
        for _, w in ipairs(windows) do
            lines[#lines + 1] = record('w', w.name, num(w.x), num(w.y), num(w.w or 0), num(w.h or 0), num(w.scale or 1))
        end
    end
end

-- `kind` made here (this game is the Main) and handed to each of `alts`: how many it went to, or nil and why
local function produce(kind, alts)
    local me = character()
    if me == nil then
        return nil, "the Main isn't in the game yet"
    end
    if #alts == 0 then
        return nil, 'no other game is running'
    end
    local lines = { FORMAT, record('kind', kind), record('from', tostring(self_pid()), me.name) }
    local ok, why
    if kind == 'addons' then
        ok, why = pcall(addons_records, lines, me)
    else
        ok, why = pcall(settings_records, kind, lines)
    end
    if not ok then
        return nil, tostring(why)
    end
    local text = table.concat(lines, '\n') .. '\n'
    local folder = vana.paths.install .. '\\config\\vanacore'
    vana.fs.create_dir(folder)
    local sent = 0
    for _, alt in ipairs(alts) do
        local path = folder .. '\\copy-' .. alt.pid .. '.txt'
        if vana.fs.write(path, text) and vana.ipc.send(record('copy.take', kind, tostring(alt.pid), path)) then
            sent = sent + 1
        end
    end
    if sent == 0 then
        return nil, "couldn't hand them over"
    end
    return sent
end

-------------------------------------------------------------------------------
-- The Alt: taking it up
-------------------------------------------------------------------------------

-- what happened, in this game's chat log and the Main's
local function report(data, text)
    say(text)
    local me = character()
    vana.ipc.send(record('copy.done', tostring(data.from_pid), (me and me.name or 'The Alt') .. ': ' .. text))
end

local function fail(data, why)
    report(data, "couldn't take " .. data.from_name .. "'s " .. NAMES[data.kind] .. ': ' .. why .. '.')
end

local function apply_settings(data)
    M.flush()
    local changed, restart = 0, false
    for _, s in ipairs(data.settings) do
        local now = vana.config.get(s.section, s.key)
        if now == '' then
            now = nil
        end
        if now ~= s.value then
            vana.config.set(s.section, s.key, s.value)
            changed = changed + 1
            restart = restart or s.restart
        end
    end
    if changed > 0 then
        vana.graphics.reload()
        M.presets_reread()
        M.rebuild()
    end
    local what = data.from_name .. "'s " .. NAMES[data.kind] .. ' copied: '
    if changed == 0 then
        report(data, what .. 'they were the same already.')
    else
        report(data, what .. changed .. (changed == 1 and ' setting' or ' settings') .. ' changed'
                     .. (restart and ' (the renderer and what else needs it at the next start).' or '.'))
    end
end

-- The addons copy, planned: the files to write, the addons to unload first, the plugins and addons to load
local function plan_addons(data, me)
    local install, mine = data.install, vana.paths.install
    local files, owners = {}, {}
    local function consider(rel, text, owner)
        local now = read_file(mine .. '\\' .. rel)
        if now ~= text then
            files[#files + 1] = { rel = rel, text = text, before = now }
            owners[owner] = true
        end
    end
    -- config\addons\<addon>\ and settings\addons\<Addon>\ in the Main's install, by the names as they compare
    local function folders(path)
        local by = {}
        for _, name in ipairs(vana.fs.list(path, '', 'folders') or {}) do
            by[norm(name)] = name
        end
        return by
    end
    local ashita, own = folders(install .. '\\config\\addons'), folders(install .. '\\settings\\addons')
    for _, a in ipairs(data.addons) do
        local n = norm(a.name)
        if not KEEP[a.name:lower()] then
            if a.source == 'ashita' and ashita[n] ~= nil then
                local base = 'config\\addons\\' .. ashita[n] .. '\\'
                local from = install .. '\\' .. base .. ashita_folder(data.char)
                for _, file in ipairs(vana.fs.list(from, '', 'files') or {}) do
                    local text = read_file(from .. '\\' .. file)
                    if text ~= nil and settings_file(text) then
                        consider(base .. ashita_folder(me) .. '\\' .. file, text, n)
                    end
                end
            elseif a.source == 'vanacore' and own[n] ~= nil then
                local base = 'settings\\addons\\' .. own[n] .. '\\'
                local from = install .. '\\' .. base .. vanacore_folder(data.char)
                for _, file in ipairs(vana.fs.list(from, '', 'files') or {}) do
                    local text = read_file(from .. '\\' .. file)
                    if text ~= nil then
                        consider(base .. vanacore_folder(me) .. '\\' .. file, text, n)
                    end
                end
            end
        end
    end
    -- loaded here now, by kind and name
    local here, plugins_here = {}, {}
    for _, a in ipairs(vana.addons.loaded()) do
        here[a.source .. '\0' .. a.name:lower()] = true
    end
    for _, p in ipairs(vana.plugins.loaded()) do
        plugins_here[p.source .. '\0' .. p.name:lower()] = true
    end
    local unload, reload, load = {}, {}, {}
    for _, a in ipairs(vana.addons.loaded()) do
        if owners[norm(a.name)] and not KEEP[a.name:lower()] and (a.source == 'ashita' or a.source == 'vanacore') then
            unload[#unload + 1] = a.name:lower()
            reload[a.source .. '\0' .. a.name:lower()] = true
        end
    end
    local plugins = {}
    for _, p in ipairs(data.plugins) do
        if not plugins_here[p.source .. '\0' .. p.name:lower()] then
            plugins[#plugins + 1] = '/plugin load ' .. p.name:lower() .. ' ' .. p.source
        end
    end
    local added = 0
    for _, a in ipairs(data.addons) do
        local key = a.source .. '\0' .. a.name:lower()
        if not here[key] or reload[key] then
            load[#load + 1] = '/addon load ' .. a.name:lower() .. ' ' .. a.source
            added = added + (here[key] and 0 or 1)
        end
    end
    return { files = files, unload = unload, plugins = plugins, load = load, added = added }
end

local function start_addons(data)
    if job ~= nil then
        fail(data, 'another addons copy is under way here')
        return
    end
    local me = character()
    if me == nil then
        fail(data, "this character isn't in the game yet")
        return
    end
    if data.char == nil or data.install == nil then
        fail(data, 'what was sent is missing its parts')
        return
    end
    local ok, plan = pcall(plan_addons, data, me)
    if not ok then
        fail(data, tostring(plan))
        return
    end
    job = { data = data, plan = plan, stage = 'unload', at = clock(), queue = {} }
end

-- The Alt's files written over by the copy, kept in config\before-copy\<date-time>\, then the Main's written: how many
-- were written, and where the old ones are (nil if none was kept)
local function write_files(files)
    local mine = vana.paths.install
    local root = 'config\\before-copy\\' .. os.date('%Y-%m-%d-%H%M%S')
    local kept, written = false, 0
    for _, f in ipairs(files) do
        if f.before ~= nil then
            local to = mine .. '\\' .. root .. '\\' .. f.rel
            vana.fs.create_dir(folder_of(to))
            if vana.fs.write(to, f.before) then
                kept = true
            end
        end
        local to = mine .. '\\' .. f.rel
        vana.fs.create_dir(folder_of(to))
        if vana.fs.write(to, f.text) then
            written = written + 1
        end
    end
    return written, kept and root or nil
end

-- The Main's windows put here: the same place on a screen of the same size, else in proportion (share.fit). An addon's
-- text and shapes not drawn here are left (they're moved by an offset from where they're drawn). How many moved.
local function place_windows(data)
    if data.screen == nil or #data.windows == 0 or vana.layout == nil then
        return 0
    end
    local sw, sh = vana.ui.screen()
    if not sw or sw <= 0 or not sh or sh <= 0 then
        return 0
    end
    local fitted = M.share.fit(data.windows, data.screen[1], data.screen[2], sw, sh)
    local here = {}
    for _, w in ipairs(vana.layout.windows()) do
        here[w.name] = w
    end
    local moved = 0
    for _, p in ipairs(fitted) do
        local w = here[p.name]
        local objects = p.name:find('##objects/', 1, true) ~= nil
        if objects and (w == nil or not w.shown) then
            -- (nothing drawn here to move them from)
        elseif w == nil or math.abs(w.x - p.x) > 1 or math.abs(w.y - p.y) > 1 or math.abs((w.scale or 1) - p.scale) > 0.005 then
            vana.layout.set(p.name, p.x, p.y, p.scale)
            moved = moved + 1
        end
    end
    return moved
end

local function loaded_now(names)
    local set = {}
    for _, a in ipairs(vana.addons.loaded()) do
        set[a.name:lower()] = true
    end
    for _, name in ipairs(names) do
        if set[name] then
            return true
        end
    end
    return false
end

-- Once a frame: the addons copy a stage on (one command a frame), and an unanswered ask given up
local function tick()
    local now = clock()
    if asking ~= nil and now - asking.at > ASK_WAIT then
        say(asking.main .. " didn't answer (the Main needs this VanaCore's vana addon too: /addon reload vana there).")
        asking = nil
    end
    if job == nil then
        return
    end
    local plan = job.plan
    if job.stage == 'unload' then
        if not job.unloading then
            for _, name in ipairs(plan.unload) do
                vana.chat.run('/addon unload ' .. name)
            end
            job.unloading, job.at = true, now
            return
        end
        if #plan.unload > 0 and (now - job.at < UNLOAD_MIN or (loaded_now(plan.unload) and now - job.at < UNLOAD_WAIT)) then
            return
        end
        job.stage = 'write'
    elseif job.stage == 'write' then
        job.written, job.kept = write_files(plan.files)
        for _, line in ipairs(plan.plugins) do
            job.queue[#job.queue + 1] = line
        end
        for _, line in ipairs(plan.load) do
            job.queue[#job.queue + 1] = line
        end
        job.stage, job.at = 'load', now
    elseif job.stage == 'load' then
        local line = table.remove(job.queue, 1)
        if line ~= nil then
            vana.chat.run(line)
            job.at = now
            return
        end
        if now - job.at >= PLACE_AFTER then
            job.stage = 'place'
        end
    elseif job.stage == 'place' then
        local data = job.data
        local ok, moved = pcall(place_windows, data)
        moved = ok and moved or 0
        local parts = {}
        local loads = plan.added + #plan.plugins
        parts[#parts + 1] = loads == 0 and 'nothing to load' or (loads .. ' loaded')
        parts[#parts + 1] = (job.written or 0) .. ' settings ' .. (job.written == 1 and 'file' or 'files') .. ' written'
                            .. (#plan.unload > 0 and ' (' .. #plan.unload .. ' reloaded)' or '')
        parts[#parts + 1] = moved .. (moved == 1 and ' window' or ' windows') .. ' moved'
        report(data, data.from_name .. "'s addons and plugins copied: " .. table.concat(parts, ', ') .. '.'
                     .. (job.kept and (' The old files are in ' .. job.kept .. '.') or ''))
        job = nil
        M.refresh_at = clock() + 0.2 -- (the Addons and Plugins pages built again)
    end
end

-- what the Main sent, read from its file (deleted then)
local function take(kind, path)
    asking = nil
    local text = read_file(path)
    vana.fs.remove(path)
    if text == nil then
        say("couldn't read what the Main sent (" .. path .. ').')
        return
    end
    local data = { kind = kind, from_pid = 0, from_name = 'The Main', settings = {}, addons = {}, plugins = {}, windows = {} }
    local first = true
    for line in text:gmatch('([^\n]*)\n') do
        local f = split(line)
        if first then
            if f[1] ~= FORMAT then
                say("what the Main sent is from a VanaCore this one can't read: update both.")
                return
            end
            first = false
        elseif f[1] == 'from' then
            data.from_pid, data.from_name = tonumber(f[2]) or 0, f[3] or data.from_name
        elseif f[1] == 's' or f[1] == 'u' then
            data.settings[#data.settings + 1] = { section = f[2], key = f[3], restart = f[4] == '1', value = f[1] == 's' and f[5] or nil }
        elseif f[1] == 'install' then
            data.install = f[2]
        elseif f[1] == 'char' then
            data.char = { name = f[2], id = f[3] }
        elseif f[1] == 'a' then
            data.addons[#data.addons + 1] = { source = f[2], name = f[3] }
        elseif f[1] == 'p' then
            data.plugins[#data.plugins + 1] = { source = f[2], name = f[3] }
        elseif f[1] == 'screen' then
            data.screen = { tonumber(f[2]) or 0, tonumber(f[3]) or 0 }
        elseif f[1] == 'w' and tonumber(f[3]) and tonumber(f[4]) then
            data.windows[#data.windows + 1] = { name = f[2], x = tonumber(f[3]), y = tonumber(f[4]), w = tonumber(f[5]) or 0,
                                                h = tonumber(f[6]) or 0, scale = tonumber(f[7]) or 1 }
        end
    end
    if NAMES[kind] == nil then
        return
    end
    if kind == 'addons' then
        start_addons(data)
    else
        apply_settings(data)
    end
end

-------------------------------------------------------------------------------
-- The buttons
-------------------------------------------------------------------------------

-- The Main's `kind` ('graphics', 'options' or 'addons') to the Alts. From the Main, made and handed over now: how many
-- games it went to; from an Alt, the Main is asked: 0. Nil and why when it can't be.
function copy.send(kind)
    if NAMES[kind] == nil then
        return nil, 'no such copy'
    end
    local main, alts = games()
    if main == nil or #alts == 0 then
        return nil, 'no other game is running'
    end
    if main.self then
        return produce(kind, alts)
    end
    if main.name == '' then
        return nil, "the Main isn't in the game yet"
    end
    if not vana.ipc.send(record('copy.ask', kind, tostring(main.pid), tostring(self_pid()))) then
        return nil, "couldn't reach the Main"
    end
    asking = { kind = kind, at = clock(), main = main.name }
    return 0
end

-- A copy under way in this game (the Alt's addons copy, or an ask the Main hasn't answered yet)
function copy.busy()
    return job ~= nil or asking ~= nil
end

copy.NAMES = NAMES

vana.events.on('ipc', function (e)
    local f = split(e.message or '')
    local verb = f[1]
    if verb == 'copy.take' then
        if tonumber(f[3]) == self_pid() then
            take(f[2], f[4] or '')
        end
    elseif verb == 'copy.ask' then
        if tonumber(f[3]) == self_pid() then
            local main, alts = games()
            local sent, why = nil, 'this game is no longer the Main'
            if main ~= nil and main.self then
                sent, why = produce(f[2], alts)
            end
            if sent == nil then
                vana.ipc.send(record('copy.fail', f[4] or '', "couldn't send its " .. (NAMES[f[2]] or f[2]) .. ': ' .. why .. '.'))
            end
        end
    elseif verb == 'copy.fail' then
        if tonumber(f[2]) == self_pid() then
            asking = nil
            say('The Main ' .. (f[3] or "couldn't send them."))
        end
    elseif verb == 'copy.done' then
        if tonumber(f[2]) == self_pid() then
            say(f[3] or '')
        end
    end
end, 'vana.copy')

vana.events.on('present', function ()
    local ok, why = pcall(tick)
    if not ok then
        job, asking = nil, nil
        say('the copy stopped: ' .. tostring(why))
    end
end, 'vana.copy')

M.copy = copy
return copy
