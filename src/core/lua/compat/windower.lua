--[[
Compatibility layer: lets addons written for Windower 4's Lua API run on VanaCore, by putting that
API (windower.*, _addon, coroutine.schedule, ...) over vana.*. Runs after vana.lua in every addon
VanaCore takes for a Windower addon.

It covers what the addons tried so far use; calls to parts that aren't here yet fail with a clear
error, and events that aren't raised yet are noted in the log.
--]]

local vana = vana
local game = vana.game
local getinfo = debug.getinfo -- kept: an addon may replace `debug` (SimpleLog sets it to false)

-- Tables whose missing entries raise "not supported yet" errors naming the call; but the names in `absent` read nil, as
-- they do on Windower's own tables until something sets them (addons test for them: GearSwap's
-- `if windower.text.saved_texts`, which the texts library sets).
local function partial(name, t, absent)
    return setmetatable(t or {}, {
        absent = absent,
        __index = function (_, key)
            if absent ~= nil and absent[key] then
                return nil
            end
            error(('%s.%s is not supported by VanaCore yet'):format(name, tostring(key)), 2)
        end,
    })
end

_addon = {}
windower = {}
windower.addon_path = vana.paths.addon .. '\\'
-- VanaCore's addons\windower is laid out as Windower's addons folder: its addons, libs\ and res\.
windower.windower_path = vana.paths.addons .. '\\windower\\'
-- The game's folder (FINAL FANTASY XI) and PlayOnline's beside it (PlayOnlineViewer), each with its backslash, once the
-- game is loaded
if vana.paths.game ~= nil and vana.paths.game ~= '' then
    windower.ffxi_path = vana.paths.game .. '\\'
    windower.pol_path = vana.paths.game:gsub('[^\\]+$', '') .. 'PlayOnlineViewer\\'
end

-- Paths Windower's code builds for Windower's own layout, pointed at VanaCore's: <windower>\addons\x
-- (Windower's addons folder) is addons\windower\x, and for an addon kept outside addons\windower,
-- the libraries beside it (addon_path .. '../libs/') are addons\windower\libs.
-- Windower's own libraries and resources come with VanaCore, in addons\vanacore\libs\windower (laid out as
-- addons\windower: libs\ and res\): a file or folder read from addons\windower\libs or res that isn't there is read from
-- VanaCore's copy, so an addon copied in alone (GearSwap) has the `files`, `resources`, ... it needs. What's written
-- there stays in addons\windower.
local remap, remap_read
do
    local function normal(path)
        return (path:gsub('/', '\\'):lower())
    end
    -- The path with its .\ and x\..\ taken out, as Windows takes them out when it opens it
    local function resolved(path)
        path = path:gsub('/', '\\')
        local count
        repeat
            path, count = path:gsub('\\%.\\', '\\', 1)
        until count == 0
        repeat
            path, count = path:gsub('\\[^\\]+\\%.%.\\', '\\', 1)
        until count == 0
        return path
    end
    local own = normal(windower.windower_path)
    local carried = vana.paths.addons .. '\\vanacore\\libs\\windower\\'
    local function there(path)
        return vana.fs.exists((path:gsub('\\+$', '')))
    end
    local function carried_copy(path)
        local full = resolved(path)
        if normal(full):sub(1, #own) ~= own then
            return path
        end
        local rest = full:sub(#own + 1)
        local top = normal(rest):match('^([^\\]+)')
        if (top ~= 'libs' and top ~= 'res') or there(full) or not there(carried .. rest) then
            return path
        end
        return carried .. rest
    end
    local rules = { { from = normal(windower.windower_path .. 'addons\\'), to = windower.windower_path } }
    if normal(windower.addon_path):sub(1, #rules[1].to) ~= normal(windower.windower_path) then
        rules[#rules + 1] = { from = normal(windower.addon_path .. '..\\libs\\'), to = windower.windower_path .. 'libs\\' }
    end
    remap = function (path)
        if type(path) ~= 'string' then
            return path
        end
        local n = normal(path)
        for _, rule in ipairs(rules) do
            if n:sub(1, #rule.from) == rule.from then
                return rule.to .. path:sub(#rule.from + 1)
            end
        end
        return path
    end
    remap_read = function (path)
        if type(path) ~= 'string' then
            return path
        end
        return carried_copy(remap(path))
    end
    local open, lines, run, load = io.open, io.lines, dofile, loadfile
    io.open = function (path, mode, ...)
        local reads = mode == nil or not tostring(mode):find('[wa+]')
        return open((reads and remap_read or remap)(path), mode, ...)
    end
    io.lines = function (path, ...) return lines(remap_read(path), ...) end
    dofile = function (path) return run(remap_read(path)) end
    loadfile = function (path, ...) return load(remap_read(path), ...) end
end

-- Windower's print goes to its console; VanaCore's log is the nearest thing.
function print(...)
    vana.log(...)
end

-------------------------------------------------------------------------------
-- Game data, as Windower shapes it
-------------------------------------------------------------------------------

local job_names = {
    [0] = 'NON', 'WAR', 'MNK', 'WHM', 'BLM', 'RDM', 'THF', 'PLD', 'DRK', 'BST', 'BRD', 'RNG', 'SAM', 'NIN', 'DRG', 'SMN',
    'BLU', 'COR', 'PUP', 'DNC', 'SCH', 'GEO', 'RUN',
}
local party_keys = { 'p0', 'p1', 'p2', 'p3', 'p4', 'p5', 'a10', 'a11', 'a12', 'a13', 'a14', 'a15', 'a20', 'a21', 'a22',
                     'a23', 'a24', 'a25' }

-- The party slots (1-18) and the local player's entity index, or nil when not logged in.
local function party_and_me()
    local party = game.party()
    if party == nil then
        return nil
    end
    local me = party.members[1]
    if not me.active or me.id == 0 or me.target_index == 0 or game.entity(me.target_index) == nil then
        return party, nil
    end
    return party, me.target_index
end

local last_jobs = nil -- the jobs last read, for the moments the game reads none (zoning)

-- Logged in: the player is there and has a job (the game reads job 0 for a moment at login and while zoning: a job last
-- read stands for it, and without one the player isn't there yet, as Windower has it)
local function logged_in()
    local party, me = party_and_me()
    if me == nil then
        return false
    end
    local stats = game.player()
    local job = stats ~= nil and stats.main_job or party.members[1].main_job
    return (job or 0) ~= 0 or last_jobs ~= nil
end

-- Windower's mob table for an entity.
local function mob(entity, party)
    if entity == nil then
        return nil
    end
    local in_party, in_alliance = false, false
    if party ~= nil then
        for slot, member in ipairs(party.members) do
            if member.active and member.id == entity.id then
                in_alliance = true
                in_party = in_party or slot <= 6
            end
        end
    end
    return {
        id = entity.id,
        index = entity.index,
        name = entity.name,
        x = entity.x,
        y = entity.y,
        z = entity.z,
        facing = entity.heading,
        heading = entity.heading,
        distance = entity.distance, -- squared, as Windower's is
        hpp = entity.hpp,
        status = entity.status,
        race = entity.race,
        model_size = entity.hitbox_size,
        model_scale = entity.model_size,
        movement_speed = entity.speed,
        spawn_type = entity.spawn_flags,
        entity_type = entity.type,
        is_npc = bit.band(entity.spawn_flags, 0x01) == 0,
        target_index = entity.target_index ~= 0 and entity.target_index or nil,
        pet_index = entity.pet_index ~= 0 and entity.pet_index or nil,
        claim_id = entity.claim,
        valid_target = entity.valid_target,
        in_party = in_party,
        in_alliance = in_alliance,
        charmed = false,
        models = entity.models,
    }
end

windower.ffxi = partial('windower.ffxi')

function windower.ffxi.get_mob_by_index(index)
    local party = game.party()
    return mob(game.entity(tonumber(index) or -1), party)
end

function windower.ffxi.get_mob_by_id(id)
    id = tonumber(id)
    if id == nil then
        return nil
    end
    -- NPCs and monsters keep their index in the id's low 12 bits; players need a search.
    local party = game.party()
    local entity = game.entity(bit.band(id, 0xFFF))
    if entity ~= nil and entity.id == id then
        return mob(entity, party)
    end
    for index = 0, game.entity_count() - 1 do
        entity = game.entity(index)
        if entity ~= nil and entity.id == id then
            return mob(entity, party)
        end
    end
    return nil
end

function windower.ffxi.get_mob_by_name(name)
    name = tostring(name):lower()
    local party = game.party()
    for index = 0, game.entity_count() - 1 do
        local entity = game.entity(index)
        if entity ~= nil and entity.name:lower() == name then
            return mob(entity, party)
        end
    end
    return nil
end

function windower.ffxi.get_mob_array()
    local party = game.party()
    local mobs = {}
    for index = 0, game.entity_count() - 1 do
        local entity = game.entity(index)
        if entity ~= nil then
            mobs[index] = mob(entity, party)
        end
    end
    return mobs
end

-- 't', 'st', 'me', 'pet', 'p0'-'p5', 'a10'-'a25' (with or without <>).
function windower.ffxi.get_mob_by_target(which)
    which = tostring(which or 't'):lower():gsub('[<>]', '')
    local party, me = party_and_me()
    if me == nil then
        return nil
    end
    if which == 'me' then
        return mob(game.entity(me), party)
    end
    if which == 'pet' then
        local player = game.entity(me)
        return player and player.pet_index ~= 0 and mob(game.entity(player.pet_index), party) or nil
    end
    if which == 't' or which == 'st' then
        local target = game.target()
        if target == nil then
            return nil
        end
        local index = which == 't' and target.index or target.sub_index
        if index == 0 or (which == 't' and not target.active) or (which == 'st' and not target.is_sub_target) then
            return nil
        end
        return mob(game.entity(index), party)
    end
    for slot, key in ipairs(party_keys) do
        if key == which then
            local member = party.members[slot]
            return member.active and member.target_index ~= 0 and mob(game.entity(member.target_index), party) or nil
        end
    end
    return nil
end

-- The level of each job (by its short name: WAR, MNK ...), from the game's own function for it (uint8_t __cdecl (int job),
-- the one Ashita's IPlayer.GetJobLevel calls), read again at most once a second: addons ask for the player many times a
-- frame. Empty while the game's function isn't found.
local job_levels
do
    local ffi = require('ffi')
    local getter, cached, cached_at = nil, {}, -1
    job_levels = function ()
        local now = vana.time.clock()
        if now - cached_at < 1 then
            return cached
        end
        cached_at = now
        -- looked for again each second until it's found (FFXiMain.dll may not be decrypted yet the first time)
        if getter == nil then
            local address = vana.memory.find('FFXiMain.dll', 0, '8B 0D ?? ?? ?? ?? 85 C9 74 ?? 8B 44 24 04 3C 10 73 ?? 25 FF 00 00 00')
            if address ~= nil and address ~= 0 then
                getter = ffi.cast('uint8_t (__cdecl *)(int)', address)
            end
        end
        if getter ~= nil then
            local levels = { NON = 0 }
            for id = 1, #job_names do
                local ok, level = pcall(getter, id)
                levels[job_names[id]] = ok and tonumber(level) or 0
            end
            cached = levels
        end
        return cached
    end
end

-- The player's skills (combat, magic and crafts, by Windower's names), job points (by job, { jp_spent, jp, cp }), merits
-- (by Windower's names, from its res/merit_points.lua: the resource's name in lower case with underscores) and linkshells,
-- read again at most once a second: they change rarely and get_player is asked for many times a frame.
local player_extras
do
    local cached, cached_at, merit_names = nil, -1, nil
    player_extras = function ()
        local now = vana.time.clock()
        if cached ~= nil and now - cached_at < 1 then
            return cached
        end
        cached_at = now
        if merit_names == nil then
            merit_names = {}
            local ok, res = pcall(dofile, windower.windower_path .. 'res\\merit_points.lua')
            if ok and type(res) == 'table' then
                for id, entry in pairs(res) do
                    if type(entry) == 'table' and type(entry.en) == 'string' then
                        merit_names[id] = (entry.en:lower():gsub('[^%w]+', '_'))
                    end
                end
            end
        end
        local skills, job_points, merits = {}, {}, {}
        for _, skill in pairs(game.skills() or {}) do
            skills[skill.name] = skill.skill
        end
        for _, craft in pairs(game.crafts() or {}) do
            skills[craft.name] = craft.skill
        end
        for id, points in pairs(game.job_points() or {}) do
            if job_names[id] ~= nil then
                job_points[job_names[id]:lower()] = { jp_spent = points.spent, jp = points.points, cp = points.capacity }
            end
        end
        for id, merit in pairs(game.merits()) do
            if merit_names[id] ~= nil then
                merits[merit_names[id]] = merit.count
            end
        end
        cached = { skills = skills, job_points = job_points, merits = merits, linkshell = game.linkshell(1),
                   linkshell2 = game.linkshell(2) }
        return cached
    end
end

function windower.ffxi.get_player()
    local party, me = party_and_me()
    if me == nil then
        return nil
    end
    local entity = game.entity(me)
    local member = party.members[1]
    local stats = game.player() or { main_job = member.main_job, main_job_level = member.main_job_level,
                                     sub_job = member.sub_job, sub_job_level = member.sub_job_level, buffs = {},
                                     hp_max = 0, mp_max = 0 }
    -- Windower's player has a job while the game reads none (zoning), which addons key their settings by: the party's
    -- member 1 knows the job (it's what login is raised from) before the player structure does
    if (stats.main_job or 0) == 0 and (member.main_job or 0) ~= 0 then
        stats = setmetatable({ main_job = member.main_job, main_job_level = member.main_job_level,
                               sub_job = member.sub_job or 0, sub_job_level = member.sub_job_level or 0 },
                             { __index = stats })
    end
    if (stats.main_job or 0) == 0 then
        if last_jobs == nil then
            return nil
        end
        stats = setmetatable({ main_job = last_jobs.main_job, main_job_level = last_jobs.main_job_level,
                               sub_job = last_jobs.sub_job, sub_job_level = last_jobs.sub_job_level }, { __index = stats })
    else
        last_jobs = { main_job = stats.main_job, main_job_level = stats.main_job_level, sub_job = stats.sub_job,
                      sub_job_level = stats.sub_job_level }
    end
    -- player.jobs always has the main and sub job (their levels are known here even while the game's own getter isn't
    -- found or reads 0), as addons index it by the job's short name unguarded (GearSwap's check_wearable)
    local jobs = { NON = 0 }
    for name, level in pairs(job_levels()) do
        jobs[name] = level
    end
    if job_names[stats.main_job] ~= nil and (jobs[job_names[stats.main_job]] or 0) == 0 then
        jobs[job_names[stats.main_job]] = stats.main_job_level
    end
    if stats.sub_job ~= 0 and job_names[stats.sub_job] ~= nil and (jobs[job_names[stats.sub_job]] or 0) == 0 then
        jobs[job_names[stats.sub_job]] = stats.sub_job_level
    end
    local target = game.target()
    local buffs = {}
    for _, id in ipairs(stats.buffs) do
        if id >= 0 and id ~= 255 then
            buffs[#buffs + 1] = id
        end
    end
    local extras = player_extras()
    return {
        skills = extras.skills,
        job_points = extras.job_points,
        merits = extras.merits,
        item_level = stats.main_job_level + (stats.item_level ~= nil and stats.item_level.average or 0),
        superior_level = stats.su_level or 0,
        master_level = stats.mastery ~= nil and stats.mastery.level or 0,
        linkshell = extras.linkshell and extras.linkshell.name or nil,
        linkshell_rank = extras.linkshell and extras.linkshell.rank or nil,
        linkshell_slot = extras.linkshell and extras.linkshell.item or nil,
        linkshell2 = extras.linkshell2 and extras.linkshell2.name or nil,
        linkshell2_rank = extras.linkshell2 and extras.linkshell2.rank or nil,
        linkshell2_slot = extras.linkshell2 and extras.linkshell2.item or nil,
        name = entity.name,
        id = entity.id,
        index = entity.index,
        main_job = job_names[stats.main_job],
        main_job_id = stats.main_job,
        main_job_level = stats.main_job_level,
        sub_job = stats.sub_job ~= 0 and job_names[stats.sub_job] or nil,
        sub_job_id = stats.sub_job ~= 0 and stats.sub_job or nil,
        sub_job_level = stats.sub_job ~= 0 and stats.sub_job_level or nil,
        status = entity.status,
        in_combat = entity.status == 1,
        target_index = target ~= nil and target.active and target.index ~= 0 and target.index or nil,
        vitals = {
            hp = member.hp, mp = member.mp, tp = member.tp, hpp = member.hpp, mpp = member.mpp,
            max_hp = stats.hp_max, max_mp = stats.mp_max,
        },
        buffs = buffs,
        nation = stats.nation,
        title = stats.title,
        jobs = setmetatable(jobs, { __index = function () return 0 end }), -- a job it doesn't know is level 0, not nil
    }
end

local function party_counts(party)
    local counts = { party.party_counts[1], party.party_counts[2], party.party_counts[3] }
    for p = 1, 3 do
        if counts[p] == nil or counts[p] <= 0 then -- not filled in (solo): count the active slots
            counts[p] = 0
            for slot = (p - 1) * 6 + 1, p * 6 do
                if party.members[slot].active then
                    counts[p] = counts[p] + 1
                end
            end
        end
    end
    return counts
end

function windower.ffxi.get_party_info()
    local party = game.party()
    if party == nil then
        return nil
    end
    local counts = party_counts(party)
    return {
        party1_count = counts[1], party2_count = counts[2], party3_count = counts[3],
        party1_leader = party.party_leaders[1] ~= 0 and party.party_leaders[1] or nil,
        party2_leader = party.party_leaders[2] ~= 0 and party.party_leaders[2] or nil,
        party3_leader = party.party_leaders[3] ~= 0 and party.party_leaders[3] or nil,
        alliance_leader = party.leader ~= 0 and party.leader or nil,
    }
end

function windower.ffxi.get_party()
    local party = game.party()
    if party == nil then
        return nil
    end
    local result = windower.ffxi.get_party_info()
    local zone = party.members[1].zone
    for slot, key in ipairs(party_keys) do
        local member = party.members[slot]
        if member.active and member.id ~= 0 then
            result[key] = {
                name = member.name,
                id = member.id,
                zone = member.zone,
                hp = member.hp, mp = member.mp, tp = member.tp, hpp = member.hpp, mpp = member.mpp,
                mob = member.zone == zone and member.target_index ~= 0 and mob(game.entity(member.target_index), party) or nil,
            }
        end
    end
    return result
end

-- Vana'diel's clock as the server keeps it (LandSandBoat's CVanaTime): 25 Vana'diel minutes to an Earth minute from
-- Unix 1009810800, the first minute of the year 886. The Earth time is the server's (the game's packets carry it), the
-- PC's until one comes or if the two are more than a day apart. Returns the minute of the day (0-1439), the day of the
-- week (Windower's day ids, 0 Firesday) and the moon: its percent and phase (Windower's moon_phases ids, 0 New Moon,
-- 6 Full Moon: 84 days a cycle, a phase every 7, Full Moon around day 0 of the cycle), and the world's weather and chat
-- line.
local function world()
    local w = game.world()
    local now = os.time()
    if w.server_time ~= 0 and math.abs(w.server_time - now) < 86400 then
        now = w.server_time
    end
    local minutes = math.floor((now - 1009810800) * 25 / 60) + 886 * 518400
    local days = math.floor(minutes / 1440)
    local cycle = (days + 26) % 84
    local moon = cycle >= 42 and math.floor(100 * (cycle - 42) / 42 + 0.5) or math.floor(100 * (1 - cycle / 42) + 0.5)
    local phase = (math.floor(((cycle + 3) % 84) / 7) + 6) % 12
    return minutes % 1440, days % 8, moon, phase, w.weather, w.chat_open
end

function windower.ffxi.get_info()
    local party = game.party()
    local target = game.target()
    local language = vana.config.get('language', 'playonline') == '1' and 'Japanese' or 'English'
    local time, day, moon, moon_phase, weather, chat_open = world()
    return {
        logged_in = logged_in(),
        zone = party and party.members[1].zone or 0,
        language = language,
        server = 12, -- the server's id isn't known to VanaCore: one that the resources name
        mog_house = false,
        menu_open = target ~= nil and target.menu_open or false,
        chat_open = chat_open,
        time = time,
        day = day,
        moon = moon,
        moon_phase = moon_phase,
        weather = weather,
    }
end

-- Bit `n` (from 0) of a string of bytes, each byte's from its lowest
local function has_bit(bits, n)
    local b = bits:byte(math.floor(n / 8) + 1)
    return b ~= nil and bit.band(b, bit.lshift(1, n % 8)) ~= 0
end

-- What the player has, from the game's bits (packet 0xAC's: weapon skills, job abilities, pet commands and traits,
-- 512, 512, 512 and 256 of them), by the ids Windower's resources have: weapon skills by their bit, job abilities and
-- pet commands (blood pacts and the like, from 512) by theirs less 512, traits by theirs less 1536. Empty lists
-- before they've come.
function windower.ffxi.get_abilities()
    local result = { job_abilities = {}, weapon_skills = {}, job_traits = {}, pet_commands = {} }
    local bits = game.abilities()
    if bits == nil then
        return result
    end
    for n = 0, #bits * 8 - 1 do
        if has_bit(bits, n) then
            local list, id
            if n < 512 then
                list, id = result.weapon_skills, n
            elseif n < 1024 then
                list, id = result.job_abilities, n - 512
            elseif n < 1536 then
                list, id = result.pet_commands, n - 512
            else
                list, id = result.job_traits, n - 1536
            end
            list[#list + 1] = id
        end
    end
    return result
end

-- [spell id] = whether the player knows it (0-1023); empty before the game has them.
function windower.ffxi.get_spells()
    local result = {}
    local bits = game.spells()
    if bits == nil then
        return result
    end
    for id = 0, 1023 do
        result[id] = has_bit(bits, id)
    end
    return result
end

-- The key items the player has: their ids.
function windower.ffxi.get_key_items()
    local result = {}
    local bits = game.key_items()
    if bits ~= nil then
        for id = 0, #bits * 8 - 1 do
            if has_bit(bits, id) then
                result[#result + 1] = id
            end
        end
    end
    return result
end

-- [recast id] = seconds left (0-255, 0 when ready), as the game counts them down.
function windower.ffxi.get_ability_recasts()
    local result = {}
    for id = 0, 255 do
        result[id] = 0
    end
    local recasts = game.recasts()
    if recasts ~= nil then
        for id, ticks in pairs(recasts.abilities) do
            result[id] = ticks / 60
        end
    end
    return result
end

-- [spell id] = 1/60 seconds left (0 when ready).
function windower.ffxi.get_spell_recasts()
    local result = {}
    for id = 0, 1024 do
        result[id] = 0
    end
    local recasts = game.recasts()
    if recasts ~= nil then
        for id, ticks in pairs(recasts.spells) do
            result[id] = ticks
        end
    end
    return result
end

-- The bags by id, by the names Windower's API has (res/bags.lua's api)
local BAGS = { [0] = 'inventory', 'safe', 'storage', 'temporary', 'locker', 'satchel', 'sack', 'case', 'wardrobe',
               'safe2', 'wardrobe2', 'wardrobe3', 'wardrobe4', 'wardrobe5', 'wardrobe6', 'wardrobe7', 'wardrobe8',
               'recycle' }
local BAG_IDS = {}
for id = 0, #BAGS do
    BAG_IDS[BAGS[id]] = id
end
-- The equipment slots by the game's ids
local SLOTS = { [0] = 'main', 'sub', 'range', 'ammo', 'head', 'body', 'hands', 'legs', 'feet', 'neck', 'waist',
                'left_ear', 'right_ear', 'left_ring', 'right_ring', 'back' }

local function bag_id(bag)
    if type(bag) == 'string' then
        return BAG_IDS[bag:lower()]
    end
    return tonumber(bag)
end

local function item_entry(item, slot)
    return {
        id = item and item.id or 0,
        count = item and item.count or 0,
        status = item and bit.band(item.flags, 0xFF) or 0,
        slot = slot,
        bazaar = item and item.price or 0,
        extdata = item and item.extra:sub(1, 24) or string.rep('\0', 24),
    }
end

-- A bag as Windower has it: its items by slot (1 to its size, empty ones with id 0), max, count and enabled.
local function bag_items(id)
    local container = game.container(id)
    local result = { max = 0, count = 0, enabled = false }
    if container == nil then
        return result
    end
    local max = container.capacity > 0 and container.capacity - 1 or 0 -- slot 0 isn't one (the inventory's is gil)
    result.max, result.enabled = max, container.capacity > 0
    for slot = 1, max do
        local entry = item_entry(container[slot], slot)
        result[slot] = entry
        if entry.id ~= 0 then
            result.count = result.count + 1
        end
    end
    return result
end

local function gil_count()
    local inventory = game.container(0)
    return inventory and inventory[0] and inventory[0].count or 0
end

local function equipment_slots()
    local equipment = {}
    local equipped = game.equipment() or {}
    for id = 0, #SLOTS do
        local entry = equipped[id]
        equipment[SLOTS[id]] = entry and entry.index or 0
        equipment[SLOTS[id] .. '_bag'] = entry and entry.container or 0
    end
    return equipment
end

-- get_items(): every bag by name, with gil and the equipment (slot = its index in its bag, slot_bag = the bag);
-- get_items(bag): that bag (its id or name); get_items(bag, slot): that item; get_items(key), any other key of the
-- whole table, as Windower takes it: get_items('gil') the gil (giltracker), get_items('equipment') (equipviewer,
-- Skillchains), get_items('max_inventory').
function windower.ffxi.get_items(bag, slot)
    if bag ~= nil then
        local id = bag_id(bag)
        if id == nil and type(bag) == 'string' then
            local key = bag:lower()
            if key == 'gil' then
                return gil_count()
            elseif key == 'equipment' then
                return equipment_slots()
            end
            return windower.ffxi.get_items()[key]
        end
        if id == nil or BAGS[id] == nil then
            return nil
        end
        if slot ~= nil then
            local container = game.container(id)
            return item_entry(container and container[tonumber(slot)], tonumber(slot))
        end
        return bag_items(id)
    end
    local result = {}
    for id = 0, #BAGS do
        local items = bag_items(id)
        result[BAGS[id]] = items
        result['max_' .. BAGS[id]] = items.max
        result['count_' .. BAGS[id]] = items.count
        result['enabled_' .. BAGS[id]] = items.enabled
    end
    result.gil = gil_count()
    result.equipment = equipment_slots()
    return result
end

-- get_bag_info(): every bag by name; get_bag_info(bag): that bag (its id or name). Each is { access, capacity, count,
-- enabled }: capacity is the bag's size as get_items' max has it, access is 'Everywhere' for an enabled bag.
local function bag_info(id)
    local items = bag_items(id)
    return { access = items.enabled and 'Everywhere' or 'Unavailable', capacity = items.max, count = items.count,
             enabled = items.enabled }
end

function windower.ffxi.get_bag_info(bag)
    if bag ~= nil then
        local id = bag_id(bag)
        if id == nil or BAGS[id] == nil then
            return nil
        end
        return bag_info(id)
    end
    local result = {}
    for id = 0, #BAGS do
        result[BAGS[id]] = bag_info(id)
    end
    return result
end

-- The name of the entity with that server id, nil when there isn't one.
function windower.ffxi.get_mob_name(id)
    local found = windower.ffxi.get_mob_by_id(id)
    return found ~= nil and found.name or nil
end

-- get_mjob_data() / get_sjob_data(): what the game keeps for the job's own system (blue magic's set spells, the
-- automaton, the monster's species). VanaCore doesn't read those yet: the lists are empty and the species is nil, so
-- addons that ask (GearSwap's blue magic check, hasteinfo, Skillchains) run, seeing none set.
local function job_data()
    return { spells = {} }
end
windower.ffxi.get_mjob_data = job_data
windower.ffxi.get_sjob_data = job_data

-------------------------------------------------------------------------------
-- Moving, equipping and renaming: the game's own memory and packets
-------------------------------------------------------------------------------

-- Where the game keeps its entities (the array of their pointers) and its auto-follow structure (what Ashita's
-- IAutoFollow reads): found once by pattern in FFXiMain.dll, the address read out of the instruction.
local places = {}
local function game_place(key, pattern, offset)
    local address = places[key]
    if address == nil then
        local found = vana.memory.find('FFXiMain.dll', 0, pattern, offset)
        address = found ~= 0 and vana.memory.read_uint32(found) or 0
        places[key] = address
    end
    return address
end

local function entity_pointer(index)
    local entities = game_place('entities', '8B 56 0C 8B 04 2A 8B 04 85', 9)
    if entities == 0 or index == nil or index < 0 or index >= 0x900 then
        return nil
    end
    local pointer = vana.memory.read_uint32(entities + index * 4)
    return pointer ~= 0 and pointer or nil
end

-- The auto-follow structure: the run direction (x, height, y, w) from 12, the followed entity's index and id at 32 and
-- 36, and the byte at 41 that turns auto-running on.
local function auto_follow()
    local address = game_place('autofollow', '8B CF E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B E8 85 ED 75 0C B9', 25)
    return address ~= 0 and address or nil
end

local function stop_running(address)
    local memory = vana.memory
    memory.write_uint8(address + 41, 0)
    memory.write_uint32(address + 32, 0)
    memory.write_uint32(address + 36, 0)
    memory.write_float(address + 12, 0)
    memory.write_float(address + 16, 0)
    memory.write_float(address + 20, 0)
end

-- run(x, y): runs along that direction (any length) until told otherwise; run(true) runs the way the player faces;
-- run(false) or run() stops.
function windower.ffxi.run(x, y)
    local address = auto_follow()
    if address == nil then
        return
    end
    if x == true then
        local _, me = party_and_me()
        local entity = me ~= nil and game.entity(me) or nil
        if entity == nil then
            return
        end
        x, y = math.cos(entity.heading), -math.sin(entity.heading)
    end
    x, y = tonumber(x), tonumber(y)
    local length = x ~= nil and y ~= nil and math.sqrt(x * x + y * y) or 0
    if length == 0 then
        stop_running(address)
        return
    end
    local memory = vana.memory
    memory.write_uint32(address + 32, 0)
    memory.write_uint32(address + 36, 0)
    memory.write_float(address + 12, x / length)
    memory.write_float(address + 16, 0)
    memory.write_float(address + 20, y / length)
    memory.write_float(address + 24, 1)
    memory.write_uint8(address + 41, 1)
end

-- follow(index): auto-follows that entity; follow() stops.
function windower.ffxi.follow(index)
    local address = auto_follow()
    if address == nil then
        return
    end
    local entity = index ~= nil and game.entity(index) or nil
    if entity == nil then
        stop_running(address)
        return
    end
    vana.memory.write_uint32(address + 32, entity.index)
    vana.memory.write_uint32(address + 36, entity.id)
    vana.memory.write_uint8(address + 41, 1)
end

-- turn(angle): the player faces that heading (radians).
function windower.ffxi.turn(angle)
    local _, me = party_and_me()
    local pointer = me ~= nil and entity_pointer(me) or nil
    if pointer ~= nil then
        vana.memory.write_float(pointer + 0xE4, tonumber(angle) or 0)
    end
end

-- A packet the game sends, `size` bytes long as the server expects it: its 4-byte header (VanaCore fills it), then
-- these bytes, then zeros.
local function send(id, size, body)
    vana.packets.inject(false, id, ('\0\0\0\0' .. body .. string.rep('\0', size)):sub(1, size))
end

-- set_equip(index, slot, bag): the item at that index of the bag (the inventory by default) into the equipment slot.
function windower.ffxi.set_equip(index, slot, bag)
    send(0x50, 16, string.char(tonumber(index) % 256, tonumber(slot) % 256, (bag_id(bag or 0) or 0) % 256, 0))
end

-- cancel_buff(id): takes that buff off the player, as clicking it off does (the status cancel packet, 0x0F1: the id at
-- +4). The server refuses one that can't be cancelled.
function windower.ffxi.cancel_buff(id)
    id = math.floor(tonumber(id) or -1)
    if id < 0 or id > 0xFFFF then
        return
    end
    send(0xF1, 8, string.char(id % 256, math.floor(id / 256)))
end

-- get_item(bag, slot, count): the item at that slot of the bag into the inventory (count of it, one by default).
function windower.ffxi.get_item(bag, slot, count)
    local from = bag_id(bag)
    if from == nil or BAGS[from] == nil then
        return
    end
    count = math.floor(tonumber(count) or 1)
    send(0x29, 24, string.char(count % 256, math.floor(count / 256) % 256, math.floor(count / 65536) % 256, 0,
                           from % 256, 0, tonumber(slot) % 256, 0x52))
end

-- set_mob_name(id, name): the entity's name (by its server id or its index) as the game has it, for as long as it's
-- written; renamer writes it every frame.
function windower.set_mob_name(id, name)
    id = tonumber(id)
    local pointer = id ~= nil and entity_pointer(id < 0x900 and id or id % 0x1000) or nil
    if pointer == nil then
        return
    end
    local text = vana.encoding.to_shift_jis(tostring(name)):sub(1, 23)
    if vana.memory.read_string(pointer + 0x7C, 24) ~= text then
        vana.memory.write_string(pointer + 0x7C, text)
    end
end

-------------------------------------------------------------------------------
-- Events
-------------------------------------------------------------------------------

local RAISED = {
    ['load'] = true, ['unload'] = true, ['prerender'] = true, ['postrender'] = true, ['addon command'] = true,
    ['login'] = true, ['logout'] = true, ['job change'] = true, ['zone change'] = true, ['target change'] = true,
    ['status change'] = true, ['hp change'] = true, ['mp change'] = true, ['tp change'] = true,
    ['hpp change'] = true, ['mpp change'] = true, ['mouse'] = true, ['incoming chunk'] = true, ['outgoing chunk'] = true,
    ['incoming text'] = true, ['outgoing text'] = true, ['keyboard'] = true, ['action'] = true, ['action message'] = true,
    ['gain buff'] = true, ['lose buff'] = true, ['time change'] = true, ['day change'] = true, ['moon change'] = true,
    ['weather change'] = true, ['unhandled command'] = true, ['ipc message'] = true, ['add item'] = true,
    ['remove item'] = true,
}
local handlers = {} -- event name -> list of { id, fn }
local handler_names = {} -- id -> event name
local next_id = 0
local noted = {}

-- Events from VanaCore's that can change or block what they're about, each from the event table VanaCore raises: a
-- handler returning true blocks it, one returning a string changes it to that (the handlers after see it so).
--   'incoming chunk', 'outgoing chunk' (id, original, modified, injected, blocked): each packet the game gets or sends,
--     its 4-byte header first, as it came and as it is now
--   'incoming text' (original, modified, original_mode, modified_mode, blocked): each line coming into the chat log
--   'outgoing text' (original, modified, blocked): each line going to the game that no command took
-- VanaCore's event is listened for once an addon listens for one of these (until then they go by without calling in).
local CHANGING = {
    ['incoming chunk'] = { event = 'packet_in', field = 'data_modified',
                           args = function (e) return e.id, e.data, e.data_modified, e.injected, e.blocked end },
    ['outgoing chunk'] = { event = 'packet_out', field = 'data_modified',
                           args = function (e) return e.id, e.data, e.data_modified, e.injected, e.blocked end },
    ['incoming text'] = { event = 'text_in', field = 'message_modified',
                          args = function (e)
                              -- Windower's modes are the chat mode alone; e.mode (Ashita's) has the line's flags above it
                              return e.message, e.message_modified, bit.band(e.mode, 0xFF), bit.band(e.mode_modified, 0xFF),
                                     e.blocked
                          end },
    ['outgoing text'] = { event = 'text_out', field = 'message_modified',
                          args = function (e) return e.message, e.message_modified, e.blocked end },
}
local heard = {}

local function raise_changing(name, e)
    local list, how = handlers[name], CHANGING[name]
    if list == nil then
        return false
    end
    for _, handler in ipairs({ unpack(list) }) do
        local finished, result = vana.tasks.spawn(handler.fn, how.args(e))
        if finished then
            if result == true then
                e.blocked = true
            elseif type(result) == 'string' then
                e[how.field] = result
            end
        end
    end
    return e.blocked == true
end

-- 'keyboard' (dik, pressed, flags, blocked): each key's press and release, its DirectInput code; a handler returning
-- true keeps it from the game. From VanaCore's 'key' event (the window message), listened for once an addon listens.
local function hear_keyboard()
    vana.events.on('key', function (wparam, lparam, blocked)
        local list = handlers['keyboard']
        if list == nil then
            return false
        end
        local dik = bit.band(bit.rshift(lparam, 16), 0x7F) + (bit.band(lparam, 0x1000000) ~= 0 and 0x80 or 0)
        local pressed = bit.band(lparam, 0x80000000) == 0
        local taken = false
        for _, handler in ipairs({ unpack(list) }) do
            local finished, result = vana.tasks.spawn(handler.fn, dik, pressed, 0, blocked or taken)
            if finished and result == true then
                taken = true
            end
        end
        return taken
    end, 'windower keyboard')
end

local hear_mouse -- below, where raise is: listens for VanaCore's 'mouse'
local hear_actions -- and the incoming packets 'action' and 'action message' come from
local hear_items -- and the item packets 'add item' and 'remove item' come from

local function hear(name)
    if name == 'keyboard' and not heard[name] then
        heard[name] = true
        hear_keyboard()
        return
    end
    if (name == 'action' or name == 'action message') and not heard.actions then
        heard.actions = true
        hear_actions()
        return
    end
    if (name == 'add item' or name == 'remove item') and not heard.items then
        heard.items = true
        hear_items()
        return
    end
    if name == 'mouse' and not heard[name] then
        heard[name] = true
        hear_mouse()
        return
    end
    local how = CHANGING[name]
    if how ~= nil and not heard[name] then
        heard[name] = true
        vana.events.on(how.event, function (e) return raise_changing(name, e) end, 'windower ' .. name)
    end
end

-- True once the addon has a mouse handler of its own (not the texts and images libraries' dragging): its windows take
-- clicks, so its primitives keep a press from the game (block_mouse, below with them).
local function own_mouse()
    for _, handler in ipairs(handlers['mouse'] or {}) do
        if not handler.drags then
            return true
        end
    end
    return false
end
local refresh_prims -- below: sets each primitive's block_mouse again

-- windower.register_event(name, ..., fn): fn for each named event. Returns the ids.
function windower.register_event(...)
    local count = select('#', ...)
    local fn = select(count, ...)
    assert(type(fn) == 'function', 'windower.register_event: the last argument must be a function')
    local ids = {}
    for i = 1, count - 1 do
        local name = tostring((select(i, ...))):lower()
        next_id = next_id + 1
        handlers[name] = handlers[name] or {}
        -- the texts and images libraries' own mouse handler (it drags their boxes): a plain left press doesn't reach it
        local source = name == 'mouse' and getinfo(fn, 'S').source:lower() or ''
        local drags = source:find('[\\/]libs[\\/]texts%.lua$') ~= nil or source:find('[\\/]libs[\\/]images%.lua$') ~= nil
        table.insert(handlers[name], { id = next_id, fn = fn, drags = drags })
        if name == 'mouse' and not drags and refresh_prims ~= nil then
            refresh_prims()
        end
        handler_names[next_id] = name
        ids[#ids + 1] = next_id
        hear(name)
        if not RAISED[name] and not noted[name] then
            noted[name] = true
            vana.log(("listens for Windower's '%s' event, which VanaCore doesn't raise yet"):format(name))
        end
    end
    return unpack(ids)
end

-- Windower's raw_register_event is register_event without the addon's wrapper around it.
windower.raw_register_event = windower.register_event

function windower.unregister_event(...)
    for i = 1, select('#', ...) do
        local id = select(i, ...)
        local name = handler_names[id]
        if name ~= nil then
            for position, handler in ipairs(handlers[name]) do
                if handler.id == id then
                    table.remove(handlers[name], position)
                    break
                end
            end
            handler_names[id] = nil
        end
    end
end

-- Runs each handler for `name`; handlers can coroutine.sleep, as in Windower. Returns true if one
-- returned true (for commands: handled).
local function raise(name, ...)
    local list = handlers[name]
    if list == nil then
        return false
    end
    local handled = false
    for _, handler in ipairs({ unpack(list) }) do
        local finished, result = vana.tasks.spawn(handler.fn, ...)
        if finished and result == true then
            handled = true
        end
    end
    return handled
end

-- The derived events, and what they compare frame to frame: the player's numbers from vana.game.self (no tables,
-- as this runs every frame in every Windower addon), in `was`. Compared only while a handler listens for one of
-- them; until then (and on the frame one starts listening) they're only read.
local DERIVED = { 'login', 'logout', 'zone change', 'job change', 'target change', 'status change', 'hp change',
                  'mp change', 'tp change', 'hpp change', 'mpp change' }
local was = { known = false }

local function derived_listened()
    for i = 1, #DERIVED do
        local list = handlers[DERIVED[i]]
        if list ~= nil and #list > 0 then
            return true
        end
    end
    return false
end

-- The player's name (for login and logout), read only when that changes
local function my_name()
    local party, me = party_and_me()
    return me ~= nil and party.members[1].name or nil
end

-- Raises login/logout, zone, job, target, status and vitals changes Windower would.
local function poll()
    if not derived_listened() then
        was.known = false
        return
    end
    local id, zone, main_job, main_job_level, sub_job, sub_job_level, status, target, hp, mp, tp, hpp, mpp = game.self()
    -- logged in once the job is read; through the zoning moments it reads none
    local logged_in = id ~= nil and (main_job ~= 0 or was.logged_in == true)
    if not was.known then
        was.known, was.logged_in, was.id = true, logged_in, id
        was.name = logged_in and my_name() or nil
    elseif logged_in ~= was.logged_in or (logged_in and id ~= was.id) then
        local before = was.name
        was.logged_in, was.id = logged_in, id
        was.name = logged_in and my_name() or nil
        if logged_in then
            raise('login', was.name)
        else
            raise('logout', before)
        end
    elseif logged_in then
        if zone ~= was.zone then
            raise('zone change', zone, was.zone)
        end
        if main_job ~= was.main_job or sub_job ~= was.sub_job or main_job_level ~= was.main_job_level
            or sub_job_level ~= was.sub_job_level then
            raise('job change', main_job, main_job_level, sub_job, sub_job_level)
        end
        if target ~= was.target then
            raise('target change', target)
        end
        if status ~= was.status then
            raise('status change', status, was.status)
        end
        if hp ~= was.hp then raise('hp change', hp, was.hp) end
        if mp ~= was.mp then raise('mp change', mp, was.mp) end
        if tp ~= was.tp then raise('tp change', tp, was.tp) end
        if hpp ~= was.hpp then raise('hpp change', hpp, was.hpp) end
        if mpp ~= was.mpp then raise('mpp change', mpp, was.mpp) end
    end
    if logged_in then
        was.zone, was.main_job, was.main_job_level, was.sub_job, was.sub_job_level = zone, main_job, main_job_level, sub_job,
            sub_job_level
        was.status, was.target, was.hp, was.mp, was.tp, was.hpp, was.mpp = status, target, hp, mp, tp, hpp, mpp
    end
end

vana.events.on('load', function ()
    raise('load')
end)

vana.events.on('unload', function ()
    raise('unload') -- VanaCore removes the addon's text objects after this
end)

-- The mouse over the game window: (type, x, y, delta, blocked), type the window message - 0x200
-- (0 move, 1 left down, 2 left up, 10 wheel, ...). A handler returning true keeps it from the game
-- (texts and images drag this way). A left press (or double click) reaches Windower addons only with
-- Shift held, as it reaches Windower plugins: their boxes move with Shift + left drag, and a plain
-- click stays the game's. The wheel in notches, as Windower hands it them.
-- Listened for once an addon registers 'mouse' (VanaCore calls into an addon for the mouse only while it listens).
hear_mouse = function ()
    vana.events.on('mouse', function (type, x, y, delta, blocked, modifiers)
        -- a plain left press (no Shift) goes to the addon's own handlers (its buttons and windows: TrustSupport), not
        -- to the texts and images libraries' dragging, so a click on a text box stays the game's
        if (type == 1 or type == 3) and bit.band(modifiers or 0, 1) == 0 then
            local taken = false
            for _, handler in ipairs({ unpack(handlers['mouse'] or {}) }) do
                if not handler.drags then
                    local finished, result = vana.tasks.spawn(handler.fn, type, x, y, 0, blocked)
                    taken = taken or (finished and result == true)
                end
            end
            return taken
        end
        local notches = delta >= 0 and math.floor(delta / 120) or -math.floor(-delta / 120)
        return raise('mouse', type, x, y, notches, blocked)
    end, 'windower mouse')
end

-- 'action' (the table windower.packets.parse_action makes) from each incoming 0x28, and 'action message' (actor_id,
-- target_id, actor_index, target_index, message_id, param_1, param_2, param_3) from each 0x29: the packet as the game
-- gets it, after the addon's 'incoming chunk' handlers, and not when one blocked it. Neither can change or block it.
-- Listened for once an addon registers one of them.
hear_actions = function ()
    local byte = string.byte
    local function u16(data, at)
        local a, b = byte(data, at + 1, at + 2)
        return (a or 0) + (b or 0) * 0x100
    end
    local function u32(data, at)
        return u16(data, at) + u16(data, at + 2) * 0x10000
    end
    vana.events.on('packet_in', function (e)
        if e.blocked or (e.id ~= 0x028 and e.id ~= 0x029) then
            return false
        end
        local data = e.data_modified or e.data
        if e.id == 0x028 then
            local list = handlers['action']
            if list ~= nil and #list > 0 then
                local act = windower.packets.parse_action(data)
                if act ~= nil then
                    raise('action', act)
                end
            end
        elseif #data >= 0x1C then
            raise('action message', u32(data, 0x04), u32(data, 0x08), u16(data, 0x14), u16(data, 0x16), u16(data, 0x18),
                  u32(data, 0x0C), u32(data, 0x10), 0)
        end
        return false
    end, 'windower actions')
end

-- 'add item' and 'remove item' (bag, index, id, count): an item put in an empty slot of one of the player's bags, and one
-- gone from its slot, from the server's item packets as the game gets them (0x01F and 0x020 set a slot, 0x01E its count;
-- 0 empties it): the slot as the game has it, before the packet, against the packet. An item replaced by another in its
-- slot is a remove and an add; a count changing on the same item is neither. The gil (the inventory's slot 0) isn't an
-- item. A slot set twice before the game has taken in the first (one buffer of packets) is compared with the first.
-- Listened for once an addon registers one of them.
hear_items = function ()
    local byte = string.byte
    local function u16(data, at)
        local a, b = byte(data, at + 1, at + 2)
        return (a or 0) + (b or 0) * 0x100
    end
    local function u32(data, at)
        return u16(data, at) + u16(data, at + 2) * 0x10000
    end
    local pending = {} -- bag * 256 + index -> { id, count } a packet set this frame
    vana.events.on('present', function ()
        if next(pending) ~= nil then
            pending = {}
        end
    end, 'windower items')
    vana.events.on('packet_in', function (e)
        local id = e.id
        if e.blocked or (id ~= 0x01E and id ~= 0x01F and id ~= 0x020) then
            return false
        end
        local data = e.data_modified or e.data
        local count, item, bag, index
        if id == 0x01E and #data >= 0x0A then
            count, bag, index = u32(data, 0x04), byte(data, 0x09), byte(data, 0x0A)
        elseif id == 0x01F and #data >= 0x0C then
            count, item, bag, index = u32(data, 0x04), u16(data, 0x08), byte(data, 0x0B), byte(data, 0x0C)
        elseif id == 0x020 and #data >= 0x10 then
            count, item, bag, index = u32(data, 0x04), u16(data, 0x0C), byte(data, 0x0F), byte(data, 0x10)
        else
            return false
        end
        if index == 0 or BAGS[bag] == nil then
            return false
        end
        local key = bag * 256 + index
        local was = pending[key]
        if was == nil then
            local container = game.container(bag)
            local entry = container and container[index]
            was = { entry and entry.id or 0, entry and entry.count or 0 }
        end
        if item == nil then
            item = was[1] -- 0x01E keeps the slot's item
        end
        if count == 0 then
            item = 0
        end
        pending[key] = { item, count }
        if was[1] ~= 0 and was[1] ~= item then
            raise('remove item', bag, index, was[1], was[2])
        end
        if item ~= 0 and item ~= was[1] then
            raise('add item', bag, index, item, count)
        end
        return false
    end, 'windower items')
end

-- 'gain buff' and 'lose buff' (buff id), each time one more or one fewer of a buff is on the player (the game's list, as
-- get_player's buffs); 'time change', 'day change', 'moon change' (phase) and 'weather change' (new, old) from the clock
-- and weather get_info has. Compared frame to frame only while one is listened for; nothing is raised on the frame it
-- starts, or across a login.
local world_was = {}

local function listened(name)
    local list = handlers[name]
    return list ~= nil and #list > 0
end

local function poll_world()
    if listened('gain buff') or listened('lose buff') then
        local player = logged_in() and game.player() or nil
        if player == nil then
            world_was.buffs = nil
        else
            local now = {}
            for _, id in ipairs(player.buffs or {}) do
                if id >= 0 and id ~= 255 then
                    now[id] = (now[id] or 0) + 1
                end
            end
            local before = world_was.buffs
            if before ~= nil then
                for id, count in pairs(now) do
                    for _ = (before[id] or 0) + 1, count do raise('gain buff', id) end
                end
                for id, count in pairs(before) do
                    for _ = (now[id] or 0) + 1, count do raise('lose buff', id) end
                end
            end
            world_was.buffs = now
        end
    else
        world_was.buffs = nil
    end
    if listened('time change') or listened('day change') or listened('moon change') or listened('weather change') then
        local time, day, _, phase, weather = world()
        if world_was.time ~= nil then
            if time ~= world_was.time then raise('time change', time, world_was.time) end
            if day ~= world_was.day then raise('day change', day, world_was.day) end
            if phase ~= world_was.phase then raise('moon change', phase, world_was.phase) end
            if weather ~= world_was.weather then raise('weather change', weather, world_was.weather) end
        end
        world_was.time, world_was.day, world_was.phase, world_was.weather = time, day, phase, weather
    else
        world_was.time = nil
    end
end

vana.events.on('present', function ()
    poll()
    poll_world()
    raise('prerender')
    raise('postrender')
end)

-- Splits a console command into arguments: spaces separate them, quotes keep them together.
local function arguments(text)
    local args = {}
    local at = 1
    while at <= #text do
        local c = text:sub(at, at)
        if c:match('%s') then
            at = at + 1
        elseif c == '"' or c == "'" then
            local close = text:find(c, at + 1, true) or (#text + 1)
            args[#args + 1] = text:sub(at + 1, close - 1)
            at = close + 1
        else
            local stop = text:find('%s', at) or (#text + 1)
            args[#args + 1] = text:sub(at, stop - 1)
            at = stop
        end
    end
    return args
end

-- The names an addon answers to: _addon.command, _addon.commands and its name.
local function answers_to(command)
    command = command:lower()
    if _addon.command and tostring(_addon.command):lower() == command then
        return true
    end
    if type(_addon.commands) == 'table' then
        for _, name in ipairs(_addon.commands) do
            if tostring(name):lower() == command then
                return true
            end
        end
    end
    return _addon.name ~= nil and tostring(_addon.name):lower() == command
end

-- "//dp gun" (or "//lua command dp gun") runs the addon's 'addon command' event with "gun".
vana.events.on('command', function (e)
    local text = e.command:match('^%s*//(.*)$')
    if text == nil then
        return
    end
    local args = arguments(text)
    if #args == 0 then
        return
    end
    if (args[1]:lower() == 'lua' and (args[2] == 'c' or args[2] == 'command') and args[3]) then
        table.remove(args, 1)
        table.remove(args, 1)
    end
    -- "//lua invoke <addon> <function> [args]" (lua i): calls the addon's global function with the arguments as strings, as
    -- Windower does (GearSwap's login runs "lua i gearswap refresh_user_env"); a name that isn't a function is a command
    local verb = args[2] ~= nil and args[2]:lower() or ''
    if args[1]:lower() == 'lua' and (verb == 'i' or verb == 'invoke') and args[3] and answers_to(args[3]) then
        table.remove(args, 1)
        table.remove(args, 1)
        table.remove(args, 1)
        local fn = args[1] ~= nil and _G[args[1]] or nil
        if type(fn) == 'function' then
            table.remove(args, 1)
            vana.tasks.spawn(fn, unpack(args))
        elseif args[1] ~= nil then
            raise('addon command', unpack(args))
        end
        e.blocked = true
        return
    end
    if answers_to(args[1]) then
        table.remove(args, 1)
        raise('addon command', unpack(args))
        e.blocked = true
    end
end)

-- A // command no addon or plugin took: 'unhandled command' (command, args...), and with a handler for it it's taken (no
-- "Unknown command": shortcuts makes //cure3 <t> work this way).
vana.events.on('unhandled', function (e)
    if not listened('unhandled command') then
        return false
    end
    local args = arguments(e.command)
    if #args == 0 then
        return false
    end
    raise('unhandled command', unpack(args))
    return true
end)

-------------------------------------------------------------------------------
-- Chat and commands
-------------------------------------------------------------------------------

function windower.add_to_chat(mode, text)
    vana.chat.write(tostring(text), tonumber(mode) or 1)
end

-- A Windower console command: "input /echo hi", "wait 2", "lua load x", or another addon's
-- command. Several can be separated by semicolons.
function windower.send_command(command)
    local delay = 0
    for part in (tostring(command) .. ';'):gmatch('(.-);') do
        part = part:match('^%s*@?%s*(.-)%s*$')
        local wait = part:match('^wait%s+([%d.]+)$')
        if wait ~= nil then
            delay = delay + tonumber(wait)
        elseif part ~= '' then
            local line
            local input = part:match('^input%s+(.+)$')
            if input ~= nil then
                line = input
            else
                local action, name = part:match('^lua%s+(%a+)%s+(%S+)$')
                local invoked, rest = part:match('^lua%s+i%a*%s+(%S+)%s*(.*)$')
                if invoked ~= nil then
                    line = '//lua invoke ' .. invoked .. (rest ~= '' and ' ' .. rest or '') -- lua invoke <addon> <function>
                elseif action == 'load' or action == 'l' then
                    line = '/addon load ' .. name
                elseif action == 'unload' or action == 'u' then
                    line = '/addon unload ' .. name
                elseif action == 'reload' or action == 'r' then
                    line = '/addon reload ' .. name
                else
                    line = '//' .. part
                end
            end
            if delay > 0 then
                vana.tasks.once(delay, vana.chat.run, line)
            else
                vana.chat.run(line)
            end
        end
    end
end

-- The chat line's own text isn't reachable from Lua yet: it reads as empty and closed, and what an addon puts in it is
-- dropped.
windower.chat = partial('windower.chat', {
    input = function (text)
        vana.chat.run(tostring(text))
    end,
    is_open = function () return game.world().chat_open end,
    get_input = function () return '' end,
    set_input = function () end,
    add_to_input = function () end,
    paste = function () end,
})

-- IPC: a message for this addon in the other VanaCore instances on the PC (multiboxing), where it comes as 'ipc message'
-- (message). Not to this instance itself, as in Windower. Up to 8000 bytes.
function windower.send_ipc_message(message)
    vana.ipc.send(tostring(message))
end

vana.events.on('ipc', function (e)
    raise('ipc message', e.message)
end)

-- Windower's debug output (shown only when its debug mode is on): nothing here.
function windower.debug()
end

-- Windower's console window: its text goes to VanaCore's log, and it is never shown.
windower.console = partial('windower.console', {
    write = function (text) vana.log(tostring(text)) end,
    visible = function () return false end,
    clear = function () end,
    open = function () end,
    close = function () end,
    set_position = function () end,
})

-- Windower's regex (PCRE) as VanaCore's (std::regex, ECMAScript): replace(text, pattern, with) and split(text, pattern).
windower.regex = partial('windower.regex', {
    match = vana.ashita.regex.search,
    replace = vana.ashita.regex.replace,
    split = vana.ashita.regex.split,
})

-- Opens a file as Windows does (a program with its arguments, a document in its editor).
function windower.execute(file, arguments)
    return vana.ashita.execute(tostring(file), arguments and tostring(arguments) or '')
end

-- Windower's %variables% in a command aren't expanded: the text as it is.
function windower.format_variables(text)
    return text
end

-- The chat filters aren't reachable from Lua yet (none filtered), nor is taking the focus (the game keeps it).
function windower.get_chat_filters()
    return {}
end

function windower.take_focus()
end

-- The clipboard (its text in the game's Shift-JIS, nil when it has none) and the game window's focus.
function windower.get_from_clipboard()
    local text = vana.system.clipboard()
    return text ~= '' and text or nil
end

function windower.copy_to_clipboard(text)
    vana.system.set_clipboard(tostring(text))
end

windower.has_focus = vana.system.focused

-------------------------------------------------------------------------------
-- Settings, files, text and misc
-------------------------------------------------------------------------------

function windower.get_windower_settings()
    local width, height = vana.ui.screen()
    if width == 0 then
        width = tonumber(vana.config.game('0001')) or 1920
        height = tonumber(vana.config.game('0002')) or 1080
    end
    return {
        x_res = width, y_res = height, ui_x_res = width, ui_y_res = height,
        window_x_pos = 0, window_y_pos = 0, launcher_version = 'VanaCore ' .. vana.version,
    }
end

function windower.file_exists(path)
    path = remap_read(path)
    return vana.fs.exists(path) and not vana.fs.is_dir(path)
end

function windower.dir_exists(path)
    return vana.fs.is_dir(remap_read(path))
end

function windower.create_dir(path)
    return vana.fs.create_dir(remap(path))
end

function windower.get_dir(path)
    return vana.fs.list(remap_read(path)) or {}
end

-- Wildcard match: * any run of characters, ? any one.
function windower.wc_match(text, pattern)
    local lua_pattern = '^' .. tostring(pattern):gsub('[%^%$%(%)%%%.%[%]%+%-]', '%%%0'):gsub('%*', '.*'):gsub('%?', '.') .. '$'
    return tostring(text):match(lua_pattern) ~= nil
end

windower.to_shift_jis = vana.encoding.to_shift_jis
windower.from_shift_jis = vana.encoding.from_shift_jis

function windower.convert_auto_trans(text)
    return text
end

windower.play_sound = function (path) return vana.system.play_sound(remap(path)) end -- PlaySound opens it in winmm
windower.open_url = vana.system.open_url

function windower.get_mouse_position()
    return 0, 0
end

-- coroutine.schedule(fn, seconds): runs fn later; coroutine.close(handle) cancels it.
function coroutine.schedule(fn, delay)
    return vana.tasks.once(tonumber(delay) or 0, fn)
end

local close = coroutine.close
function coroutine.close(handle)
    if type(handle) == 'table' then
        vana.tasks.cancel(handle)
    elseif close ~= nil then
        return close(handle)
    end
end

-- Text objects, by name, drawn as VanaCore text boxes. Sizes are in points, as Windower's are.
local function points(size)
    return math.max(6, math.floor((tonumber(size) or 12) * 4 / 3 + 0.5))
end

local function argb(alpha, red, green, blue)
    local function byte(value) return math.max(0, math.min(255, math.floor(tonumber(value) or 0))) end
    return byte(alpha) * 0x1000000 + byte(red) * 0x10000 + byte(green) * 0x100 + byte(blue)
end

windower.text = partial('windower.text', nil, { saved_texts = true })
local boxes = {}

-- The box for text `name` (made hidden, as Windower makes them).
local function box(name)
    local found = boxes[name]
    if found == nil then
        error(('windower.text: no text object named "%s"'):format(tostring(name)), 3)
    end
    return found
end

-- A setting written to the box only when it differs from what the box has: addons set the same values every frame, and
-- each write reaches VanaCore (a text's changed is drawn again).
local function put(found, key, value)
    if found.shadow[key] ~= value then
        found.shadow[key] = value
        found.box[key] = value
    end
end

function windower.text.create(name)
    if boxes[name] ~= nil then
        boxes[name].box:destroy()
    end
    local initial = {
        text = '', x = 0, y = 0, font = 'Arial', size = points(12), color = 0xFFFFFFFF, background = argb(255, 0, 0, 0),
        padding = 0, visible = false,
    }
    local created = {
        -- layer 1: over every primitive, as Windower draws its text objects after its primitives (TrustSupport makes a
        -- button's other look after its label, and shows it under the label)
        box = vana.ui.text({
            text = '', x = 0, y = 0, font = 'Arial', size = points(12), color = 0xFFFFFFFF, background = 0,
            padding = 0, color_codes = true, visible = false, layer = 1,
        }),
        background = argb(255, 0, 0, 0),
        background_visible = true,
        shadow = initial,
    }
    created.box.background = created.background
    boxes[name] = created
end

function windower.text.delete(name)
    local found = boxes[name]
    if found ~= nil then
        found.box:destroy()
        boxes[name] = nil
    end
end

function windower.text.set_text(name, text)
    put(box(name), 'text', tostring(text or ''))
end

function windower.text.set_location(name, x, y)
    local found = box(name)
    put(found, 'x', math.floor(tonumber(x) or 0))
    put(found, 'y', math.floor(tonumber(y) or 0))
end

function windower.text.get_location(name)
    local b = box(name).box
    return b.x, b.y
end

function windower.text.get_extents(name)
    local b = box(name).box
    return b.width, b.height
end

function windower.text.set_visibility(name, visible)
    put(box(name), 'visible', visible and true or false)
end

function windower.text.set_font(name, font)
    put(box(name), 'font', tostring(font))
end

function windower.text.set_font_size(name, size)
    put(box(name), 'size', points(size))
end

function windower.text.set_color(name, alpha, red, green, blue)
    put(box(name), 'color', argb(alpha, red, green, blue))
end

local function update_background(found)
    put(found, 'background', found.background_visible and found.background or 0)
end

function windower.text.set_bg_color(name, alpha, red, green, blue)
    local found = box(name)
    found.background = argb(alpha, red, green, blue)
    update_background(found)
end

function windower.text.set_bg_visibility(name, visible)
    local found = box(name)
    found.background_visible = visible and true or false
    update_background(found)
end

function windower.text.set_bg_border_size(name, size)
    put(box(name), 'padding', math.max(0, math.floor(tonumber(size) or 0)))
end

function windower.text.set_bold(name, bold)
    put(box(name), 'bold', bold and true or false)
end

function windower.text.set_italic(name, italic)
    put(box(name), 'italic', italic and true or false)
end

function windower.text.set_right_justified(name, right)
    put(box(name), 'right', right and true or false)
end

function windower.text.set_bottom_justified()
end

function windower.text.set_stroke_width(name, width)
    put(box(name), 'stroke', math.max(0, math.min(8, math.floor(tonumber(width) or 0))))
end

function windower.text.set_stroke_color(name, alpha, red, green, blue)
    put(box(name), 'stroke_color', argb(alpha, red, green, blue))
end

-- windower.prim: primitives (what Windower's images library draws), as VanaCore boxes drawn with the text ones: a
-- rectangle of its colour, or with a texture (an image file) that picture tinted by its colour, at its size or the
-- texture's own (fit to texture), repeated across it as set. Made hidden, as Windower makes them.
windower.prim = partial('windower.prim')
local prims = {}

local function prim(name)
    local found = prims[name]
    if found == nil then
        error(('windower.prim: no primitive named "%s"'):format(tostring(name)), 3)
    end
    return found
end

-- Its box made (again, when it changes between a rectangle and a picture) and set as it is
local function show_prim(p)
    local image = p.texture ~= nil
    if p.box == nil or p.image ~= image then
        if p.box ~= nil then
            p.box:destroy()
        end
        p.box = image and vana.ui.image({ visible = false }) or vana.ui.rect({ visible = false })
        p.image = image
        p.applied = {}
    end
    local fit = image and p.fit
    local properties = {
        x = math.floor(p.x), y = math.floor(p.y),
        width = fit and 0 or math.max(0, math.floor(p.width)), height = fit and 0 or math.max(0, math.floor(p.height)),
        color = p.color, visible = p.visible,
        -- an addon whose windows take clicks (a mouse handler of its own): a press over its primitives is taken before the
        -- game sees it (VanaCore's mouse hook), so dragging its window doesn't drag in the game too
        block_mouse = own_mouse(),
    }
    if image then
        properties.file = p.texture
        properties.repeat_x, properties.repeat_y = p.repeat_x, p.repeat_y
    end
    -- only what differs from what the box has was written: addons set a primitive's size and place every frame
    local applied, changed, any = p.applied, {}, false
    for key, value in pairs(properties) do
        if applied[key] ~= value then
            applied[key] = value
            changed[key] = value
            any = true
        end
    end
    if any then
        p.box:set(changed)
    end
end

refresh_prims = function ()
    for _, p in pairs(prims) do
        show_prim(p)
    end
end

function windower.prim.create(name)
    if prims[name] ~= nil and prims[name].box ~= nil then
        prims[name].box:destroy()
    end
    prims[name] = { x = 0, y = 0, width = 0, height = 0, color = argb(255, 255, 255, 255), visible = false, fit = true,
                    repeat_x = 1, repeat_y = 1 }
    show_prim(prims[name])
end

function windower.prim.delete(name)
    local found = prims[name]
    if found ~= nil then
        if found.box ~= nil then
            found.box:destroy()
        end
        prims[name] = nil
    end
end

function windower.prim.set_color(name, alpha, red, green, blue)
    local p = prim(name)
    p.color = argb(alpha, red, green, blue)
    show_prim(p)
end

function windower.prim.set_position(name, x, y)
    local p = prim(name)
    p.x, p.y = tonumber(x) or 0, tonumber(y) or 0
    show_prim(p)
end

function windower.prim.set_size(name, width, height)
    local p = prim(name)
    p.width, p.height = tonumber(width) or 0, tonumber(height) or 0
    show_prim(p)
end

function windower.prim.set_visibility(name, visible)
    local p = prim(name)
    p.visible = visible and true or false
    show_prim(p)
end

-- An image file (a full path, or from the addon's folder); nil or '' takes it off.
function windower.prim.set_texture(name, path)
    local p = prim(name)
    p.texture = path ~= nil and tostring(path) ~= '' and remap(tostring(path)) or nil
    show_prim(p)
end

function windower.prim.set_fit_to_texture(name, fit)
    local p = prim(name)
    p.fit = fit and true or false
    show_prim(p)
end

function windower.prim.set_repeat(name, x, y)
    local p = prim(name)
    p.repeat_x = math.max(1, math.floor(tonumber(x) or 1))
    p.repeat_y = math.max(1, math.floor(tonumber(y) or 1))
    show_prim(p)
end

-------------------------------------------------------------------------------
-- Packets
-------------------------------------------------------------------------------

-- windower.packets: packets into the next buffer the game receives or sends (`data` the whole packet, its 4-byte
-- header first: VanaCore sets its id, size and sequence), and the last of an id the game received or sent.
windower.packets = partial('windower.packets', {
    inject_incoming = function (id, data) vana.packets.inject(true, id, data) end,
    inject_outgoing = function (id, data) vana.packets.inject(false, id, data) end,
    last_incoming = function (id) return vana.packets.last(true, id) end,
    last_outgoing = function (id) return vana.packets.last(false, id) end,
})

-- parse_action(data): an action packet (0x28, its header too) as Windower's has it: actor_id, category, param, recast,
-- target_count and targets, each { id, action_count, actions }, each action's reaction, animation, effect, stagger,
-- knockback, param, message, unknown, and its added and spike effects (has_*, then *_animation, *_effect, *_param,
-- *_message). The fields run on as bits, each byte's lowest first, from the packet's start.
function windower.packets.parse_action(data)
    if type(data) ~= 'string' or #data < 14 then
        return nil
    end
    local byte, floor = string.byte, math.floor
    local limit = #data * 8
    local pos = 0
    local function bits(count)
        local value, got = 0, 0
        while got < count do
            local at = pos + got
            if at >= limit then
                break
            end
            local take = math.min(8 - at % 8, count - got)
            local part = floor(byte(data, floor(at / 8) + 1) / 2 ^ (at % 8)) % 2 ^ take
            value = value + part * 2 ^ got
            got = got + take
        end
        pos = pos + count
        return value
    end

    pos = 40
    local act = { actor_id = bits(32), target_count = bits(10), category = bits(4), param = bits(16) }
    act.unknown = bits(16)
    act.recast = bits(32)
    act.targets = {}
    for t = 1, act.target_count do
        if pos >= limit then
            break
        end
        local target = { id = bits(32), action_count = bits(4), actions = {} }
        for a = 1, target.action_count do
            local action = {
                reaction = bits(5), animation = bits(12), effect = bits(4), stagger = bits(3), knockback = bits(3),
                param = bits(17), message = bits(10), unknown = bits(31),
            }
            -- an effect the action hasn't is zeros, as Windower's (battlemod reads add_effect_message unguarded)
            action.has_add_effect = bits(1) ~= 0
            local has = action.has_add_effect
            action.add_effect_animation = has and bits(6) or 0
            action.add_effect_effect = has and bits(4) or 0
            action.add_effect_param = has and bits(17) or 0
            action.add_effect_message = has and bits(10) or 0
            action.has_spike_effect = bits(1) ~= 0
            has = action.has_spike_effect
            action.spike_effect_animation = has and bits(6) or 0
            action.spike_effect_effect = has and bits(4) or 0
            action.spike_effect_param = has and bits(14) or 0
            action.spike_effect_message = has and bits(10) or 0
            target.actions[a] = action
        end
        act.targets[t] = target
    end
    return act
end

-- >>> pack (the tests load this part alone)
-- Windower's string.pack and string.unpack (its LPack, changed), which its packets, extdata and slips libraries and
-- many addons read and write packets with; require('pack') has them. A format is codes, each followed by a count:
--   b<n>  an unsigned number in n bits (1 without a count)      q<n>  n booleans, a bit each
--   B     a boolean in a byte          c C  a signed and an unsigned byte    h H  2 bytes    i I, l L  4 bytes
--   f d n  a float, a double, a Lua number (a double)
--   A<n>  n bytes as they are (A* the rest)        S<n>  n bytes, the string up to their first zero (S* the rest)
--   z     a string ended by a zero                 p P a  a string after its length in 1, 2 and 4 bytes
--   < > =  little endian (as it starts), big, native (little)
-- Bits fill each byte from its lowest; any other code starts at the next whole byte. A count after a number code
-- repeats it (H4: four), * as many as there are. string.unpack(data, format, [byte = 1], [bit = 1]) starts at that
-- byte (and bit of it) and returns the values alone, as Windower's does (no position after them); it stops where the
-- data does. Packing, A<n> and S<n> are made exactly n bytes long (cut, or padded with zeros).
do
    local ffi = require('ffi')
    local byte, char, sub, rep, concat, floor = string.byte, string.char, string.sub, string.rep, table.concat, math.floor

    local SIZES = { B = 1, c = 1, C = 1, h = 2, H = 2, i = 4, I = 4, l = 4, L = 4, f = 4, d = 8, n = 8 }
    local SIGNED = { c = true, h = true, i = true, l = true }
    local REAL = { f = true, d = true, n = true }
    local LENGTHS = { p = 1, P = 2, a = 4 }
    local float, double = ffi.new('float[1]'), ffi.new('double[1]')

    local function encode_int(value, size, big)
        value = floor(tonumber(value) or 0) % 2 ^ (8 * size) -- negatives as two's complement
        local bytes = {}
        for k = 1, size do
            bytes[big and size - k + 1 or k] = value % 256
            value = floor(value / 256)
        end
        return char(unpack(bytes, 1, size))
    end

    local function decode_int(data, at, size, signed, big)
        local value, scale = 0, 1
        for k = 0, size - 1 do
            value = value + byte(data, big and at + size - 1 - k or at + k) * scale
            scale = scale * 256
        end
        if signed and value >= scale / 2 then
            value = value - scale
        end
        return value
    end

    local function encode_real(value, size, big)
        local buffer = size == 4 and float or double
        buffer[0] = tonumber(value) or 0
        local bytes = ffi.string(buffer, size)
        return big and bytes:reverse() or bytes
    end

    local function decode_real(data, at, size, big)
        local bytes = sub(data, at, at + size - 1)
        if big then
            bytes = bytes:reverse()
        end
        local buffer = size == 4 and float or double
        ffi.copy(buffer, bytes, size)
        return buffer[0]
    end

    local function truth(value)
        return value ~= nil and value ~= false and value ~= 0
    end

    -- The code at `i` and its count (a number, '*' or nil), and where the next code starts.
    local function code_at(format, i)
        local code = sub(format, i, i)
        local digits = format:match('^%d+', i + 1)
        if digits then
            return code, tonumber(digits), i + 1 + #digits
        elseif sub(format, i + 1, i + 1) == '*' then
            return code, '*', i + 2
        end
        return code, nil, i + 1
    end

    function string.pack(format, ...)
        local args, nargs, arg = { ... }, select('#', ...), 1
        local out = {}
        local big = false
        local pending, pending_bits = 0, 0 -- the byte bits are going into
        local function put_bits(value, width)
            for k = 0, width - 1 do
                pending = pending + floor(value / 2 ^ k) % 2 * 2 ^ pending_bits
                pending_bits = pending_bits + 1
                if pending_bits == 8 then
                    out[#out + 1] = char(pending)
                    pending, pending_bits = 0, 0
                end
            end
        end
        local function next_arg()
            local value = args[arg]
            arg = arg + 1
            return value
        end
        local i = 1
        while i <= #format do
            local code, count
            code, count, i = code_at(format, i)
            if code == '<' or code == '=' then
                big = false
            elseif code == '>' then
                big = true
            elseif code == ' ' or code == ',' then -- luacheck: ignore (separators)
            elseif code == 'b' then
                local width = type(count) == 'number' and count or 1
                local value = next_arg()
                value = value == true and 1 or floor(tonumber(value) or 0)
                put_bits(value % 2 ^ width, width)
            elseif code == 'q' then
                local times = count == '*' and nargs - arg + 1 or count or 1
                for _ = 1, times do
                    put_bits(truth(next_arg()) and 1 or 0, 1)
                end
            else
                if pending_bits > 0 then -- the rest of the byte bits went into
                    out[#out + 1] = char(pending)
                    pending, pending_bits = 0, 0
                end
                if code == 'A' or code == 'S' then
                    local text = tostring(next_arg() or '')
                    if type(count) == 'number' then
                        text = sub(text, 1, count) .. rep('\0', count - #text)
                    end
                    out[#out + 1] = text
                elseif code == 'z' then
                    out[#out + 1] = tostring(next_arg() or '') .. '\0'
                elseif LENGTHS[code] then
                    local text = tostring(next_arg() or '')
                    out[#out + 1] = encode_int(#text, LENGTHS[code], big) .. text
                elseif SIZES[code] then
                    local size = SIZES[code]
                    local times = count == '*' and nargs - arg + 1 or count or 1
                    for _ = 1, times do
                        local value = next_arg()
                        if code == 'B' then
                            out[#out + 1] = char(truth(value) and 1 or 0)
                        elseif REAL[code] then
                            out[#out + 1] = encode_real(value, size, big)
                        else
                            out[#out + 1] = encode_int(value == true and 1 or value, size, big)
                        end
                    end
                else
                    error(("string.pack: '%s' isn't a format code"):format(code), 2)
                end
            end
        end
        if pending_bits > 0 then
            out[#out + 1] = char(pending)
        end
        return concat(out)
    end

    function string.unpack(data, format, init, bit_init)
        local results, n = {}, 0
        local length = #data
        local at = floor(tonumber(init) or 1) -- the byte next read
        local bits = floor(tonumber(bit_init) or 1) - 1 -- of it, the bits read already
        local big = false
        local function get_bits(width)
            if at + floor((bits + width - 1) / 8) > length then
                return nil
            end
            local value = 0
            for k = 0, width - 1 do
                value = value + floor(byte(data, at) / 2 ^ bits) % 2 * 2 ^ k
                bits = bits + 1
                if bits == 8 then
                    at, bits = at + 1, 0
                end
            end
            return value
        end
        local function add(value)
            n = n + 1
            results[n] = value
        end
        local i = 1
        while i <= #format do
            local code, count
            code, count, i = code_at(format, i)
            if code == '<' or code == '=' then
                big = false
            elseif code == '>' then
                big = true
            elseif code == ' ' or code == ',' then -- luacheck: ignore (separators)
            elseif code == 'b' then
                local value = get_bits(type(count) == 'number' and count or 1)
                if value == nil then
                    break
                end
                add(value)
            elseif code == 'q' then
                local times = count == '*' and math.huge or count or 1
                local short = false
                for _ = 1, times do
                    local value = get_bits(1)
                    if value == nil then
                        short = count ~= '*'
                        break
                    end
                    add(value == 1)
                end
                if short then
                    break
                end
            else
                if bits > 0 then -- the rest of the byte bits came from
                    at, bits = at + 1, 0
                end
                if code == 'A' or code == 'S' then
                    local size = count == '*' and length - at + 1 or count or 1
                    if at + size - 1 > length then
                        break
                    end
                    local text = sub(data, at, at + size - 1)
                    at = at + size
                    if code == 'S' then
                        local zero = text:find('\0', 1, true)
                        if zero then
                            text = sub(text, 1, zero - 1)
                        end
                    end
                    add(text)
                elseif code == 'z' then
                    if at > length then
                        break
                    end
                    local zero = data:find('\0', at, true) or length + 1
                    add(sub(data, at, zero - 1))
                    at = zero + 1
                elseif LENGTHS[code] then
                    local size = LENGTHS[code]
                    if at + size - 1 > length then
                        break
                    end
                    local text_length = decode_int(data, at, size, false, big)
                    if at + size + text_length - 1 > length then
                        break
                    end
                    add(sub(data, at + size, at + size + text_length - 1))
                    at = at + size + text_length
                elseif SIZES[code] then
                    local size = SIZES[code]
                    local times = count == '*' and math.huge or count or 1
                    local short = false
                    for _ = 1, times do
                        if at + size - 1 > length then
                            short = count ~= '*'
                            break
                        end
                        if code == 'B' then
                            add(byte(data, at) ~= 0)
                        elseif REAL[code] then
                            add(decode_real(data, at, size, big))
                        else
                            add(decode_int(data, at, size, SIGNED[code], big))
                        end
                        at = at + size
                    end
                    if short then
                        break
                    end
                else
                    error(("string.unpack: '%s' isn't a format code"):format(code), 2)
                end
            end
        end
        return unpack(results, 1, n)
    end

    package.preload['pack'] = function ()
        return { pack = string.pack, unpack = string.unpack }
    end
end
-- <<< pack

-- require 'sqlite3': Windower's Lua host has LuaSQLite3 built in; VanaCore doesn't carry SQLite yet. The module is
-- there, so an addon that only requires it loads (GearSwap's dev branch requires it and never uses it), and each call
-- into it fails, naming it. lsqlite3 sets the global sqlite3 too.
package.preload['sqlite3'] = function ()
    sqlite3 = partial('sqlite3')
    return sqlite3
end

-------------------------------------------------------------------------------
-- Checked when the addon loads: what it calls that VanaCore doesn't have
-------------------------------------------------------------------------------

-- The addon's own files, read for windower.<name> calls and the events it listens for, before it runs: one line in the
-- chat and the log names each call VanaCore lacks, so a missing one isn't found a call at a time as it fails. A call the
-- addon defines or replaces itself isn't one; nor is a name behind another (user_windower.x, a.windower.x).
do
    local KNOWN_NAMESPACES = { ffxi = true, text = true, prim = true, packets = true, chat = true, console = true, regex = true }
    local lacking, listens = {}, {}

    local function exists(path)
        local node = windower
        for part in path:gmatch('[^.]+') do
            if type(node) ~= 'table' then
                return false
            end
            local value = rawget(node, part)
            if value == nil then
                -- a name partial() reads as nil (one Windower doesn't have either until it's set) isn't one VanaCore lacks
                local meta = getmetatable(node)
                return meta ~= nil and type(meta.absent) == 'table' and meta.absent[part] == true
            end
            node = value
        end
        return true
    end

    local function scan(text)
        local defined = {}
        for name in text:gmatch('function%s+windower%.([%w_%.]+)') do defined[name] = true end
        for name in text:gmatch('windower%.([%w_%.]+)%s*=[^=]') do defined[name] = true end
        local at = 1
        while true do
            local from, to, name = text:find('windower%.([%a_][%w_]*%.?[%a_]?[%w_]*)', at)
            if from == nil then
                break
            end
            at = to + 1
            local before = from > 1 and text:sub(from - 1, from - 1) or ''
            if not before:match('[%w_.]') then
                -- one level or two: windower.add_to_chat, windower.ffxi.get_player
                local first, second = name:match('^([%a_][%w_]*)%.([%a_][%w_]*)$')
                local path = name
                if first ~= nil and KNOWN_NAMESPACES[first] then
                    path = first .. '.' .. second
                else
                    path = name:match('^([%a_][%w_]*)')
                end
                if path ~= nil and not defined[path] and not exists(path) then
                    lacking[path] = true
                end
            end
        end
        for event in text:gmatch('register_event%s*%(%s*[\'"]([^\'"]+)[\'"]') do
            listens[event:lower()] = true
        end
    end

    local function walk(folder, depth)
        for _, file in ipairs(vana.fs.list(folder, '.+\\.lua', 'files') or {}) do
            local handle = io.open(folder .. '\\' .. file, 'rb')
            if handle ~= nil then
                local text = handle:read('*a') or ''
                handle:close()
                if #text < 2000000 then
                    scan(text)
                end
            end
        end
        if depth < 3 then
            for _, sub_folder in ipairs(vana.fs.list(folder, '', 'folders') or {}) do
                walk(folder .. '\\' .. sub_folder, depth + 1)
            end
        end
    end

    local ok = pcall(walk, (windower.addon_path:gsub('[\\/]+$', '')), 1)
    if ok then
        local calls = {}
        for path in pairs(lacking) do
            -- the names it only mentions in passing (a field it sets on windower: _addon, addon_path) aren't calls
            if not path:match('^_') and path ~= 'addon_path' and path ~= 'windower_path' then
                calls[#calls + 1] = 'windower.' .. path
            end
        end
        table.sort(calls)
        local events = {}
        for event in pairs(listens) do
            if not RAISED[event] then
                events[#events + 1] = event
            end
        end
        table.sort(events)
        if #calls > 0 or #events > 0 then
            local name = windower.addon_path:match('([^\\/]+)[\\/]*$') or 'This addon'
            local parts = {}
            if #calls > 0 then
                parts[#parts + 1] = #calls .. ' calls (' .. table.concat(calls, ', ', 1, math.min(#calls, 8)) ..
                                    (#calls > 8 and ', ...' or '') .. ')'
            end
            if #events > 0 then
                parts[#parts + 1] = #events .. ' events (' .. table.concat(events, ', ', 1, math.min(#events, 8)) ..
                                    (#events > 8 and ', ...' or '') .. ')'
            end
            vana.log(name .. ' uses Windower ' .. table.concat(calls, ', ') .. (#events > 0 and ' | events: ' .. table.concat(events, ', ') or '') ..
                     " that VanaCore doesn't have yet")
            vana.chat.write('(Windower Addon) ' .. name .. " uses what VanaCore doesn't have yet: " .. table.concat(parts, ', '), 1)
        end
    end
end
