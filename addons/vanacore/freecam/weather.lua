--[[
freecam\weather.lua: the free camera's Weather, for photos. Any weather, drawn on this screen alone: the server's weather packet
(0x057: when the weather started, which it is, and how long it fades in, in Vana'diel minutes) made here and handed to the game as
if the server had sent it (vana.packets.inject). Nothing goes to the server, which carries on with the zone's own weather; other
players see theirs.

While a weather is held, the server's own 0x057s are kept from the game (the last one is remembered), so the zone's changes don't
take it away. Turned off, the zone's weather is put back: the server's last, as a fresh 0x057 (the one the zone had when the
weather was picked, if none came since). That happens when it's turned off, when the free camera goes off (freecam.lua) and when
the addon unloads. A zone change (0x00B, 0x00A) just ends it: the new zone's weather comes with its zone-in.

The game fades from one weather to the next over the packet's last field, Vana'diel minutes (x 3600 ticks, 2.4 real seconds each;
0 swaps at once): FFXiMain's 0x057 handler (VA 0x1009F1C0) keeps it and its weather task (0x1009F0F0) multiplies it by 3600 for
the zone's SetWeather, as the PS2 client's RecWeatherTask does. The server sends 4 to 28 (10 s to over a minute); ours send FADE.
The start time doesn't change it: one put an hour back still faded (tried in game 2026-11-10).

A zone's files hold the looks of the weathers it really gets: one it never gets may look like clear skies there, or like another.

Loaded by freecam.lua with loadfile; returns its table.
--]]

local W = {}

local KEY = 'freecam.weather'
local EPOCH = 1009810800 -- 0x057's start time counts Earth seconds from here (the server's Vana'diel epoch, its "vana time")
local FADE = 1 -- our 0x057s' fade, in Vana'diel minutes: 2.4 seconds (the server's take 10 s or more)

-- the weathers in the server's order (0x057's ids), under their elements (the picker's headers): { id, name, other words }
W.GROUPS = {
    { 'Calm', { 0, 'Clear' }, { 1, 'Sunny', 'sun' }, { 2, 'Cloudy', 'clouds', 'cloud' }, { 3, 'Fog', 'mist' } },
    { 'Fire', { 4, 'Hot spell', 'hot' }, { 5, 'Heat wave', 'heat' } },
    { 'Water', { 6, 'Rain' }, { 7, 'Squall', 'squalls' } },
    { 'Earth', { 8, 'Dust storm', 'dust' }, { 9, 'Sand storm', 'sand' } },
    { 'Wind', { 10, 'Wind' }, { 11, 'Gales', 'gale' } },
    { 'Ice', { 12, 'Snow' }, { 13, 'Blizzards', 'blizzard' } },
    { 'Thunder', { 14, 'Thunder' }, { 15, 'Thunderstorms', 'thunderstorm', 'storm', 'storms' } },
    { 'Light', { 16, 'Auroras', 'aurora' }, { 17, 'Stellar glare', 'stellar', 'glare' } },
    { 'Dark', { 18, 'Gloom' }, { 19, 'Darkness', 'dark' } },
}

local NAMES = {} -- id -> name
local WORDS = {} -- a word typed for one (lower case, no spaces) -> id
for _, group in ipairs(W.GROUPS) do
    for i = 2, #group do
        local w = group[i]
        NAMES[w[1]] = w[2]
        WORDS[w[2]:lower():gsub('[%s_%-]', '')] = w[1]
        for j = 3, #w do
            WORDS[w[j]] = w[1]
        end
    end
end

local st = {
    held = nil, -- the weather shown while one is held (an id), else nil: the zone's
    real = nil, -- while one is held, the zone's weather from the server: { id }
}

function W.name(id)
    return NAMES[id] or ('weather ' .. tostring(id))
end

function W.held()
    return st.held
end

-- the weather the game has now (vana.game.world().weather: the last 0x057 or zone-in it was given, the held one's too)
function W.now()
    local world = vana.game.world()
    return world ~= nil and world.weather or nil
end

-- the zone's own weather: while one is held, the server's last; else what the game has
function W.zone()
    if st.held ~= nil and st.real ~= nil then
        return st.real.id
    end
    return W.now()
end

-- a weather from what's typed: its id (0 to 19), its name (any case, with or without spaces), a word of its, or the start of
-- just one name. Returns the id, or nil and why.
function W.find(text)
    local word = tostring(text or ''):lower():gsub('[%s_%-]', '')
    if word == '' then
        return nil, 'no weather named'
    end
    local n = tonumber(word)
    if n ~= nil then
        if NAMES[n] ~= nil then
            return n
        end
        return nil, 'no weather ' .. word .. ' (0 to 19)'
    end
    if WORDS[word] ~= nil then
        return WORDS[word]
    end
    local found
    for id = 0, 19 do
        if NAMES[id]:lower():gsub('%s', ''):sub(1, #word) == word then
            if found ~= nil then
                return nil, '"' .. text .. '" could be ' .. NAMES[found] .. ' or ' .. NAMES[id]
            end
            found = id
        end
    end
    if found ~= nil then
        return found
    end
    return nil, 'no weather "' .. text .. '"'
end

-- the names, for a line in the chat
function W.list()
    local names = {}
    for id = 0, 19 do
        names[#names + 1] = NAMES[id]:lower()
    end
    return table.concat(names, ', ')
end

-- the zone the character is in (vana.game.self's), or nil
function W.zone_id()
    if vana.game.self == nil then
        return nil
    end
    local _, zone = vana.game.self()
    return zone
end

-- the weathers zone `zone`'s files have a sky and light for, as a set (id -> true): its model DAT's environments, as the game
-- loads it (vana.game.zone_weathers). nil when that can't be told (a VanaCore.Core.dll without it, a DAT not read): any then.
local zone_sets = {}
function W.in_zone(zone)
    if zone == nil or vana.game.zone_weathers == nil then
        return nil
    end
    local set = zone_sets[zone]
    if set == nil then
        local list = vana.game.zone_weathers(zone)
        if type(list) ~= 'table' or #list == 0 then
            return nil
        end
        set = {}
        for _, id in ipairs(list) do
            set[id] = true
        end
        zone_sets[zone] = set
    end
    return set
end

local function u16(v)
    return string.char(v % 256, math.floor(v / 256) % 256)
end

local function u32(v)
    return u16(v % 65536) .. u16(math.floor(v / 65536) % 65536)
end

local function get16(s, at) -- (`at` from 0, as the packet's offsets are)
    return s:byte(at + 1) + s:byte(at + 2) * 256
end

-- 0x057 as the server makes it when the weather changes: the 4-byte header (VanaCore fills it in), the start time (now), the
-- weather, the fade (FADE, shorter than the server's)
local function packet(id)
    local world = vana.game.world()
    local now = (world ~= nil and world.server_time or 0) - EPOCH
    return '\0\0\0\0' .. u32(math.max(0, now)) .. u16(id) .. u16(FADE)
end

-- the zone's weather as the game has it now, from the server: its last 0x057, or the zone-in's (0x00A) if that came later
local function from_server()
    local w, at = vana.packets.last(true, 0x057)
    local z, zat = vana.packets.last(true, 0x00A)
    if w ~= nil and #w >= 12 and (z == nil or (at or 0) > (zat or 0)) then
        return { id = get16(w, 8) }
    end
    if z ~= nil and #z >= 0x76 then
        return { id = get16(z, 0x68) }
    end
    local world = vana.game.world()
    return { id = world ~= nil and world.weather or 0 }
end

local function on_packet(e)
    if e.injected then
        return
    end
    if e.id == 0x057 then
        -- the zone's weather changed: kept for when the held one ends, and kept from the game
        if e.size >= 12 then
            st.real = { id = get16(e.data, 8) }
        end
        return true
    elseif e.id == 0x00B or e.id == 0x00A then
        W.off(false) -- (the new zone's weather comes with its zone-in)
    end
end

-- whether a weather can be set now: a character in a zone, and the packet calls there
function W.ready()
    if vana.packets == nil or vana.packets.inject == nil or vana.packets.last == nil then
        return false, 'it needs a newer VanaCore.Core.dll (vana.packets.inject)'
    end
    local world = vana.game.world()
    if world == nil or (world.server_time or 0) == 0 or (vana.game.zoning ~= nil and vana.game.zoning()) then
        return false, 'there is no character in a zone'
    end
    return true
end

-- shows weather `id` (0 to 19) on this screen, held till W.off. Returns true, or false and why.
function W.set(id)
    if NAMES[id] == nil then
        return false, 'no weather ' .. tostring(id)
    end
    local ok, why = W.ready()
    if not ok then
        return false, why
    end
    if st.held == nil then
        st.real = from_server()
        vana.events.on('packet_in', on_packet, KEY)
    end
    st.held = id
    vana.packets.inject(true, 0x057, packet(id))
    return true
end

-- ends the held weather, putting the zone's back (`put_back` false: not, for a zone change). Returns whether one was held.
function W.off(put_back)
    if st.held == nil then
        return false
    end
    st.held = nil
    vana.events.off('packet_in', KEY)
    local real = st.real
    st.real = nil
    if put_back ~= false and real ~= nil and W.ready() then
        vana.packets.inject(true, 0x057, packet(real.id))
    end
    return true
end

return W
