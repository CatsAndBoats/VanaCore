--[[
freecam\daytime.lua: the free camera's Time, for photos. Any hour of the Vana'diel day on this screen alone, held still: the sky,
the sun and moon, the light and the zone's colours follow the game's own Vana'diel clock, and that is set here the way a cutscene
sets its hour. Nothing goes to the server, and the game's other clock (the server's time, which it keeps in step with each packet
and times the connection by) isn't touched.

How: while its game timer is on, the game works its Vana'diel date and time out of the server's time every frame. A cutscene that
wants an hour of its own turns the timer off, sets the date and time (XiDateTime::SetCurrentDateTime) and, at its end, turns the
timer back on (XiDateTime::EnableGameTimer, which works the time out again and refreshes the zone's weather). While the timer is
off the time stands still. These are those calls, found by their code (FFXiMain.dll 0x1018AF90 EnableGameTimer, 0x1018AFB0
DisableGameTimer right after it, 0x1018ACA0 SetCurrentDateTime; the time at 0x10627E7C, counted in sixtieths of a Vana'diel
second). Tried in game 2026-11-11: noon held from 03:14 for a minute, the connection fine, the zone's hour back when turned on.

Turned off (Zone's own), the free camera going off, the addon unloading and a zone change (0x00B, 0x00A) turn the game timer
back on: the zone's own hour comes back at once. A cutscene that ends turns it on itself, which ends a held hour too. One that
sets an hour of its own meanwhile (the clock no longer the held one) is left alone when Time is turned off.

Never move the server's time instead (FFXiMain.dll 0x10490CA0's clock): the game takes it for the server gone quiet and drops the
connection (FFXI-4001, tried 2026-11-11).

Loaded by freecam.lua with loadfile; returns its table.
--]]

local ffi = require('ffi')

local T = {}

local KEY = 'freecam.daytime'
local EPOCH = 1009810800 -- the server's Vana'diel epoch: its clock runs 25 times an Earth one from here
local TICKS_DAY = 5184000 -- the game's Vana'diel clock counts 60 to a Vana'diel second: 60 * 86400 a day
local TICKS_MINUTE = 3600

-- XiDateTime::EnableGameTimer (the flag set, then the time worked out and the weather refreshed) and DisableGameTimer 32 bytes on
T.TIMER = 'C6 05 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9 74 05 E9 ?? ?? ?? ?? C3 90 90 90 90 C6 05 ?? ?? ?? ?? 00 C3'
-- XiDateTime::SetCurrentDateTime(int): the clock (B9's operand) set by SetTickFrame; the next function's start after it
T.SET = '8B 44 24 04 B9 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? C3 8D 4C 24 04 E8'

-- words typed for some times (T.find): the minute of the day
local WORDS = {
    midnight = 0, night = 0, dawn = 360, sunrise = 360, morning = 540, noon = 720, midday = 720, day = 720, afternoon = 900,
    dusk = 1080, sunset = 1080, evening = 1260,
}

local fn -- the game's calls and its clock, once found; false: not in this client
local st = {
    held = nil, -- the minute of the day held (0 to 1439), else nil: the zone's own
    ticks = nil, -- the clock as set, to tell whether something else (a cutscene) set it since
}

local function calls()
    if fn == nil then
        fn = false
        local timer = vana.memory.find('FFXiMain.dll', 0, T.TIMER)
        local set = vana.memory.find('FFXiMain.dll', 0, T.SET)
        if timer ~= 0 and set ~= 0 then
            local flag, clock = vana.memory.read_uint32(timer + 2), vana.memory.read_uint32(set + 5)
            if flag ~= 0 and clock ~= 0 then
                fn = {
                    enable = ffi.cast('void (__cdecl *)(void)', timer),
                    disable = ffi.cast('void (__cdecl *)(void)', timer + 32),
                    set = ffi.cast('void (__cdecl *)(int32_t)', set),
                    flag = ffi.cast('volatile uint8_t *', flag),
                    clock = ffi.cast('volatile int32_t *', clock),
                }
            end
        end
    end
    return fn or nil
end

-- a minute of the day (0 to 1439) as the clock shows it
function T.name(minute)
    if minute == nil then
        return '--:--'
    end
    minute = math.floor(minute) % 1440
    return string.format('%02d:%02d', math.floor(minute / 60), minute % 60)
end

-- a held hour ends by itself when the game turns its timer back on (a cutscene's end)
local function check()
    if st.held ~= nil and fn and fn.flag[0] ~= 0 then
        st.held, st.ticks = nil, nil
        vana.events.off('packet_in', KEY)
    end
end

function T.held()
    check()
    return st.held
end

-- the minute of the day the game draws now (its Vana'diel clock), or nil
function T.now()
    local f = calls()
    if f == nil then
        return nil
    end
    return math.floor((f.clock[0] % TICKS_DAY) / TICKS_MINUTE)
end

-- the zone's own minute of the day: the server's time, as the game works it out
function T.zone()
    local world = vana.game.world()
    local server = world ~= nil and world.server_time or 0
    if server == 0 then
        return nil
    end
    return math.floor((server - EPOCH) * 25 / 60) % 1440
end

-- a time from what's typed: an hour (0 to 23), hh:mm, or a word (noon, dusk, night...). Returns the minute of the day, or nil and
-- why.
function T.find(text)
    local word = tostring(text or ''):lower():gsub('%s', '')
    if word == '' then
        return nil, 'no time named'
    end
    if WORDS[word] ~= nil then
        return WORDS[word]
    end
    local h, m = word:match('^(%d%d?)[:%.h]?(%d?%d?)$')
    h, m = tonumber(h), tonumber(m ~= '' and m or '0')
    if h ~= nil and m ~= nil and h <= 24 and m <= 59 then
        return (h * 60 + m) % 1440
    end
    return nil, 'no time "' .. text .. '"'
end

function T.words()
    return 'an hour (0 to 23 or hh:mm), midnight, dawn, morning, noon, afternoon, dusk or evening'
end

local function on_packet(e)
    if not e.injected and (e.id == 0x00B or e.id == 0x00A) then
        T.off(true) -- a new zone has its own hour (the timer has to go back on: nothing else turns it on)
    end
end

-- whether a time can be held now: the game's calls found, and a character in a zone
function T.ready()
    if calls() == nil then
        return false, "this client's Vana'diel clock wasn't found"
    end
    local world = vana.game.world()
    if world == nil or (world.server_time or 0) == 0 or (vana.game.zoning ~= nil and vana.game.zoning()) then
        return false, 'there is no character in a zone'
    end
    return true
end

-- holds the game's clock at `minute` of today (0 to 1439) till T.off. Returns true, or false and why.
function T.set(minute)
    minute = tonumber(minute)
    if minute == nil or minute < 0 or minute >= 1440 then
        return false, 'no time ' .. tostring(minute)
    end
    local ok, why = T.ready()
    if not ok then
        return false, why
    end
    check()
    local f = fn
    local now = f.clock[0]
    local ticks = now - now % TICKS_DAY + math.floor(minute) * TICKS_MINUTE
    if st.held == nil then
        vana.events.on('packet_in', on_packet, KEY)
    end
    f.disable()
    f.set(ticks)
    st.held, st.ticks = math.floor(minute), f.clock[0]
    return true
end

-- ends the held time: the game timer on again, the zone's own hour back at once. `force`: even if something else (a cutscene) has
-- set the clock since (a zone change). Returns whether one was held.
function T.off(force)
    check()
    if st.held == nil then
        return false
    end
    local mine = fn.clock[0] == st.ticks
    st.held, st.ticks = nil, nil
    vana.events.off('packet_in', KEY)
    if fn.flag[0] == 0 and (mine or force) then
        fn.enable()
    end
    return true
end

return T
