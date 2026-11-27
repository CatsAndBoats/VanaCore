--[[
vana\weatherpreview.lua: /vana > Weather Effects' Weather preview ([graphics] weather.preview, this session only) shown with the
game's own sky, fog and light as well as VanaCore's particles: the picked weather handed to the game as the server's weather packet,
held till it's set back to Off, by freecam\weather.lua (the free camera's Weather: nothing goes to the server). The zone's files hold
the looks of the weathers it really gets; one it never gets may look like clear skies there, or like another (VanaCore's particles
show whatever the zone). A zone change ends the held weather; it's picked up again once the new zone is in.

Randomise on zone in ([graphics] weather.random, weather.random.chance): at each zone-in, outdoors (a zone whose files have three
weathers or more), a roll of weather.random.chance percent picks a weather (sunshine to darkness), how much of it (a half to
twice the particles), and its wind (a heading, still air to a gale); else the zone's own. The roll is kept in session-only keys
(weather.roll.id, .amount, .wind, .dir: never saved) that graphics_weather.h reads as it reads a preview's; a Weather preview
picked wins over it. Shown through the game the same way, so its sky, fog and light come too.

Loaded by vana.lua (load_module).
--]]

local M = ...

local W
do
    local chunk = loadfile(vana.paths.addons .. '\vanacore\freecam\weather.lua')
    if chunk ~= nil then
        local ok, got = pcall(chunk)
        if ok and type(got) == 'table' then
            W = got
        end
    end
end
if W == nil then
    return
end

local checked = 0
local tried = nil -- the weather last set (or tried): not tried again every half second while it can't be

local function number(key)
    return tonumber(vana.config.get('graphics', key) or '')
end

local function id_of(v)
    if v == nil or v < 0 or v > 19 then
        return nil
    end
    return math.floor(v)
end

-- the weather to hold: the preview's, else the zone-in's roll
local function want()
    return id_of(number('weather.preview')) or id_of(number('weather.roll.id'))
end

math.randomseed(os.time())
local rolled_zone = nil -- the zone the last roll was for
local random_was = false

local function set_roll(id, amount, wind, dir)
    vana.config.set('graphics', 'weather.roll.id', tostring(id))
    vana.config.set('graphics', 'weather.roll.amount', string.format('%.2f', amount))
    vana.config.set('graphics', 'weather.roll.wind', string.format('%.2f', wind))
    vana.config.set('graphics', 'weather.roll.dir', tostring(dir))
end

-- a zone-in's roll: outdoors only (three weathers or more in its files; nil, not known, counts as outdoors), weather.random.chance
-- percent of the time a weather from sunshine (1) to darkness (19), else none
local function roll(zone)
    rolled_zone = zone
    local set = W.in_zone(zone)
    local n = 0
    for _ in pairs(set or {}) do
        n = n + 1
    end
    local chance = number('weather.random.chance') or 50
    if (set ~= nil and n < 3) or math.random() * 100 >= chance then
        set_roll(-1, 1, 1, 0)
        return
    end
    set_roll(math.random(1, 19), 0.5 + math.random() * 1.5, 0.2 + math.random() * 2.3, math.random(0, 359))
end

-- Randomise on zone in, each half second: a roll at each new zone (and as it's turned on), the roll cleared as it's turned off
local function randomise()
    local on = (number('weather.random') or 0) ~= 0
    if not on then
        if random_was then
            random_was = false
            rolled_zone = nil
            set_roll(-1, 1, 1, 0)
        end
        return
    end
    random_was = true
    local zone = W.zone_id()
    if zone == nil or zone == rolled_zone or (vana.game.zoning ~= nil and vana.game.zoning()) then
        return
    end
    roll(zone)
end

vana.events.on('present', function ()
    local now = os.clock()
    if now - checked < 0.5 then
        return
    end
    checked = now
    randomise()
    local id = want()
    if id == nil then
        tried = nil
        if W.held() ~= nil then
            W.off()
        end
        return
    end
    if W.held() == id then
        return
    end
    if W.held() == nil and tried == id and not W.ready() then
        return
    end
    if W.ready() then
        W.set(id)
        tried = id
    end
end, 'weather_preview')

vana.events.on('unload', function ()
    W.off()
end, 'weather_preview')

M.weather_preview = W
