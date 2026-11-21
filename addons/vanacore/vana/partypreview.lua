--[[
vana\partypreview.lua: HUD > Party / Alliance's Preview: a full alliance of made-up members (FF8's summons, with jobs, HP,
MP and TP picked at random) in the party list and both alliance lists, so their settings can be seen while they're changed.
They're the game's own lists: the server's party packets (0x0C8 the roster, 0x0DD a member) made here and handed to the game
as if received (vana.packets.inject). The members the player really has stay in their places; the empty places are filled.
Nothing is sent to the server, and the game sends nothing about them (tried 2026-11-07: only its 0x015s went out).

It ends, the lists put back as the player's party really is, when the button is pressed again, when neither this window nor
the HUD Manager is open, at a zone change (0x00B) and when the addon unloads. A roster from the server (0x0C8: someone joined
or left) ends it too, the server's being the lists' then.

Loaded by vana.lua with `local M = ...`; the functions are M.partypreview.
--]]

local M = ...
local preview = { on = false }

local KEY = 'vana.partypreview'
local FAKE_ID = 0x7FFF0000 -- a made-up member's id: this plus its number (1-17); no character's is near it
local LEADER, ALLIANCE_LEADER = 4, 8 -- a member's flags (0x0C8, 0x0DD): bits 0-1 its party, then these
local NAMES = { 'Quezacotl', 'Shiva', 'Ifrit', 'Siren', 'Sacred', 'Minotaur', 'Diablos', 'Carbuncle', 'Leviathan',
                'Pandemona', 'Cerberus', 'Alexander', 'Doomtrain', 'Bahamut', 'Cactuar', 'Tonberry', 'Eden', 'Odin',
                'Gilgamesh', 'Phoenix', 'Moomba', 'MiniMog', 'Chicobo', 'Angelo', 'Griever' }
-- HP picked from these (percent): full ones, then under 75 (the game's yellow), 50 (orange) and 25 (red)
local HPP = { 100, 100, 100, 96, 88, 79, 74, 62, 51, 43, 37, 25, 18, 9 }
-- TP: under 1000 (the game's grey), 1000 and over
local TP = { 0, 0, 120, 480, 999, 1000, 1000, 1350, 2000, 2780, 3000 }

local made = {}       -- id -> true: the made-up members shown
local flags_were = {} -- id -> flags: the player's real members' own, as they were at the start

local function u8(v) return string.char(v % 256) end
local function u16(v) return string.char(v % 256, math.floor(v / 256) % 256) end
local function u32(v) return u16(v % 65536) .. u16(math.floor(v / 65536) % 65536) end

-- 0x0C8: `kind` (0 a party, 5 an alliance), then 20 places, each { id, index, flags, zone } or empty
local function roster(kind, entries)
    local t = { '\0\0\0\0', u8(kind), '\0\0\0' }
    for i = 1, 20 do
        local e = entries[i]
        t[#t + 1] = e ~= nil and (u32(e.id) .. u16(e.index) .. u8(e.flags) .. '\0' .. u16(e.zone) .. '\0\0') or string.rep('\0', 12)
    end
    return table.concat(t)
end

-- 0x0DD: one member; `zone` 0 when it's in the player's zone (the server's way: its HP, MP and TP then)
local function member(m)
    local name = m.name:sub(1, 15)
    return '\0\0\0\0' .. u32(m.id) .. u32(m.hp) .. u32(m.mp) .. u32(m.tp) .. u32(m.flags) .. u16(m.index) .. u8(m.num)
        .. '\0\0' .. u8(m.hpp) .. u8(m.mpp) .. '\0' .. u16(m.zone) .. u8(m.main_job) .. u8(m.main_job_level)
        .. u8(m.sub_job) .. u8(m.sub_job_level) .. '\0\0' .. name .. string.rep('\0', 16 - #name) .. '\0\0\0\0'
end

-- a real member (vana.game.party()'s) as the server would send it to a player in zone `here`
local function real_member(m, flags, here)
    local away = m.zone ~= 0 and m.zone ~= here
    return member({ id = m.id, flags = flags, name = m.name, num = m.slot % 6, index = away and 0 or m.target_index,
                    zone = away and m.zone or 0, hp = away and 0 or m.hp, mp = away and 0 or m.mp, tp = away and 0 or m.tp,
                    hpp = away and 0 or m.hpp, mpp = away and 0 or m.mpp, main_job = away and 0 or m.main_job,
                    main_job_level = away and 0 or m.main_job_level, sub_job = away and 0 or m.sub_job,
                    sub_job_level = away and 0 or m.sub_job_level })
end

-- a made-up member, `num` its place in its party (0-5)
local function made_up(id, flags, num, name)
    local main = math.random(1, 22)
    local sub = (main + math.random(1, 21) - 1) % 22 + 1
    local level = math.random(60, 99)
    local hp_max, hpp = math.random(900, 3200), HPP[math.random(#HPP)]
    local mp_max = math.random(4) == 1 and 0 or math.random(150, 1400) -- a quarter with none (a warrior's 0)
    local mpp = mp_max > 0 and math.random(5, 100) or 0
    return member({ id = id, flags = flags, name = name, num = num, index = 0, zone = 0,
                    hp = math.floor(hp_max * hpp / 100), mp = math.floor(mp_max * mpp / 100), tp = TP[math.random(#TP)],
                    hpp = hpp, mpp = mpp, main_job = main, main_job_level = level, sub_job = sub,
                    sub_job_level = math.floor(level / 2) })
end

local function names()
    local list = {}
    for i, name in ipairs(NAMES) do
        list[i] = name
    end
    for i = #list, 2, -1 do
        local j = math.random(i)
        list[i], list[j] = list[j], list[i]
    end
    return list
end

local function inject(id, data)
    vana.packets.inject(true, id, data)
end

local function unhook()
    vana.events.off('packet_in', KEY)
    vana.events.off('present', KEY)
end

-- the lists as the player's party really is: its members' roster (none: solo), and each of them with its flags as they were
function preview.stop()
    if not preview.on then
        return
    end
    preview.on = false
    unhook()
    local party = vana.game.party()
    local me = party ~= nil and party.members[1] or nil
    if me == nil then
        return
    end
    local real, entries, alliance = {}, {}, false
    for _, m in ipairs(party.members) do
        if m.active and m.id ~= 0 and not made[m.id] then
            local flags = flags_were[m.id] or m.flags
            real[#real + 1] = { m = m, flags = flags }
            entries[#entries + 1] = { id = m.id, index = m.target_index, flags = flags, zone = m.zone }
            alliance = alliance or bit.band(flags, 3) ~= 0
        end
    end
    -- (alone: the server's empty roster, as it sends one when a party breaks up)
    inject(0x0C8, #real > 1 and roster(alliance and 5 or 0, entries) or roster(0, {}))
    for _, r in ipairs(real) do
        inject(0x0DD, real_member(r.m, r.flags, me.zone))
    end
    made, flags_were = {}, {}
end

local function on_packet(e)
    if e.injected then
        return
    end
    if e.id == 0x0C8 then -- the server's roster: the lists are its now
        preview.on = false
        unhook()
        made, flags_were = {}, {}
    elseif e.id == 0x00B then -- leaving the zone
        preview.stop()
    end
end

local function on_present()
    local st, hm = M.st, M.hudman
    if not (st.open or st.minimised or (hm ~= nil and hm.open)) then
        preview.stop()
    end
end

-- every empty place in the three parties filled; an alliance leader where there's none (the player's party's leader, or the
-- player alone), its flags put back at the end
function preview.start()
    local party = vana.game.party()
    local me = party ~= nil and party.members[1] or nil
    if me == nil or not me.active or me.id == 0 or vana.packets == nil or vana.packets.inject == nil then
        return
    end
    local entries, members, list, n = {}, {}, names(), 0
    local led, by = false, nil -- an alliance leader among them; the entry that's made one if not
    made, flags_were = {}, {}
    for p = 0, 2 do
        local count = 0
        for s = p * 6 + 1, p * 6 + 6 do
            local m = party.members[s]
            if m ~= nil and m.active and m.id ~= 0 then
                flags_were[m.id] = m.flags
                local e = { id = m.id, index = m.target_index, flags = m.flags, zone = m.zone, m = m }
                entries[#entries + 1] = e
                led = led or bit.band(m.flags, ALLIANCE_LEADER) ~= 0
                if p == 0 and (by == nil or bit.band(m.flags, LEADER) ~= 0 and bit.band(by.flags, LEADER) == 0) then
                    by = e
                end
                count = count + 1
            end
        end
        while count < 6 do
            n = n + 1
            local id, flags = FAKE_ID + n, p + (count == 0 and LEADER or 0)
            made[id] = true
            entries[#entries + 1] = { id = id, index = 0, flags = flags, zone = me.zone }
            members[#members + 1] = made_up(id, flags, count, list[n])
            count = count + 1
        end
    end
    if not led and by ~= nil then
        by.flags = bit.bor(by.flags, LEADER, ALLIANCE_LEADER)
        table.insert(members, 1, real_member(by.m, by.flags, me.zone))
    end
    for _, e in ipairs(entries) do
        e.m = nil
    end
    inject(0x0C8, roster(5, entries))
    for _, data in ipairs(members) do
        inject(0x0DD, data)
    end
    preview.on = true
    vana.events.on('packet_in', on_packet, KEY)
    vana.events.on('present', on_present, KEY)
end

function preview.toggle()
    if preview.on then
        preview.stop()
    else
        preview.start()
    end
end

math.randomseed(os.time())

vana.events.on('unload', function ()
    preview.stop()
end, KEY)

M.partypreview = preview
return preview
