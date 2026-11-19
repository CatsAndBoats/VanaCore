--[[
apiexplorer: VanaCore's addon API, to read and to run.

Reference: every table and function in `vana` (and the events), each with its arguments, what it returns and what it does.
What's listed is read from the game's own `vana` table as it is now, so a function this VanaCore has and the docs don't
yet is there too, and one the docs name that this VanaCore hasn't is marked; the descriptions are the docs'
(reference.lua, made from docs/addons by conflux/tools/gen_apiexplorer_reference.py). Each entry links to the snippets
that use it, and Try it puts a call to it in the code box (Listen, for an event, shows each one as it comes).

Snippets: a call to pick, run, read what it returns and copy, in the spirit of Postman. Every one is a complete snippet
you can paste into your own addon; the code box is editable, so change a slot or an index and run it again.

In game:
  /addon load apiexplorer    opens the window
  /api                       shows or hides it (/apiexplorer too)
  /addon unload apiexplorer
--]]

addon.name    = 'apiexplorer'
addon.author  = 'Vekien'
addon.version = '1.4'
addon.desc    = 'The addon API: every vana table and function with what it takes and returns, and snippets to run and copy.'

local imgui = vana.imgui
local config = vana.settings.register({ defaults = { shown = true, call = 'The player\'s stats and buffs', tab = 'reference' } })

-- The calls, by group. Each `code` is a whole chunk: what it returns is printed in the output box.
local GROUPS = {
    { name = 'Character', calls = {
        { name = 'Who is logged in', code = [=[
return vana.game.character()   -- name, id; nil before login]=] },
        { name = 'The player in numbers', code = [=[
-- Thirteen numbers and no table: the cheap read for code that runs every frame.
local id, zone, job, level, subjob, sublevel, status, target, hp, mp, tp, hpp, mpp = vana.game.self()
return { id = id, zone = zone, job = job, level = level, subjob = subjob, sublevel = sublevel, status = status,
         target = target, hp = hp, mp = mp, tp = tp, hpp = hpp, mpp = mpp }]=] },
        { name = 'The player\'s stats and buffs', code = [=[
-- Jobs, HP/MP max, stats, resists, exp, rank, gil, limit and merit points, item levels, unity, mastery, buffs,
-- status timers, home points. See docs/addons/04-game-data.md for every field.
return vana.game.player()]=] },
        { name = 'Skills', code = [=[
-- By skill id: { name, skill, capped }. 9 is katana, 39 ninjutsu, 33 healing magic.
return vana.game.skills()]=] },
        { name = 'Crafting skills', code = [=[
-- By craft: { name, skill, rank, capped }. 0 fishing, 1 woodworking ... 8 cooking, 9 synergy.
return vana.game.crafts()]=] },
        { name = 'Every job\'s level', code = [=[
-- By job id: { level, master_level }. 1 WAR, 2 MNK ... 13 NIN ... 22 RUN.
return vana.game.jobs()]=] },
        { name = 'Job points', code = [=[
-- By job id: { capacity, points, spent }.
return vana.game.job_points()]=] },
        { name = 'Merits', code = [=[
-- By merit id: { count, next }. Windower's res/merit_points.lua names the ids (128 is STR).
local merits = vana.game.merits()
local player = vana.game.player()
return { to_spend = player and player.merit_points, cap = player and player.merit_points_max, merits = merits }]=] },
        { name = 'Missions', code = [=[
-- The current missions as the game keeps them: nation, Zilart, Promathia, the add-ons, Adoulin, Rhapsodies.
return vana.game.missions()]=] },
        { name = 'Linkshells', code = [=[
-- Slot 1 is the linkshell, 2 is linkshell 2: name, message, poster, and the equipped pearl's rank, colour and id.
return { linkshell = vana.game.linkshell(1), linkshell2 = vana.game.linkshell(2) }]=] },
        { name = 'The pet', code = [=[
-- nil without a pet. hpp, mpp and tp are the server's last word; index finds its entity.
local pet = vana.game.pet()
if pet == nil then return 'no pet' end
pet.entity = vana.game.entity(pet.index)
return pet]=] },
    } },
    { name = 'Party and targets', calls = {
        { name = 'The party and alliance', code = [=[
-- members[1..18]: 1 to 6 your party (slot 1 is you), the rest the alliance. Check `active` first.
return vana.game.party()]=] },
        { name = 'Party members, one line each', code = [=[
local party = vana.game.party()
if party == nil then return 'not logged in' end
local lines = {}
for slot = 1, 18 do
    local m = party.members[slot]
    if m.active then
        lines[#lines + 1] = ('%2d %-16s HP %3d%% MP %3d%% TP %4d  job %2d/%2d  zone %d'):format(
            slot, m.name, m.hpp, m.mpp, m.tp, m.main_job, m.sub_job, m.zone)
    end
end
return lines]=] },
        { name = 'The target', code = [=[
return vana.game.target()   -- index, id, active, sub target, locked, menu_open]=] },
        { name = 'The target\'s entity', code = [=[
local t = vana.game.target()
if t == nil or not t.active then return 'no target' end
return vana.game.entity(t.index)   -- position, heading, hpp, status, flags, models ...]=] },
        { name = 'The target\'s HP and max HP (asks the server)', code = [=[
-- The game tells you a monster's HP only as a percent. CatsEyeXI's server will say the numbers: here, your current target's
-- HP now and its maximum. The answer comes in a following frame, so this returns a promise and the explorer shows what it
-- holds when it arrives. In your own addon, print() the line in the callback instead. status is 'ok', 'not_found',
-- 'disabled', 'too_fast', 'hidden' (a boss the server keeps secret: hp and hp_max are 0) or 'no_answer' (a server without it).
if vana.game.request_max_hp == nil then return 'this VanaCore has no request_max_hp' end
local t = vana.game.target()
if t == nil or not t.active then return 'no target' end
local e = vana.game.entity(t.index)
if e == nil then return 'no target' end
return vana.promise.new(function (resolve)
    local sent, why = vana.game.request_max_hp(t.id, function (answers)
        local a = answers[1]
        if a.status == 'ok' then
            resolve(('%s: %d / %d HP (%d%%)'):format(e.name, a.hp, a.hp_max, e.hpp))
        elseif a.status == 'hidden' then
            resolve(('%s: the server keeps its HP secret (%d%%)'):format(e.name, e.hpp))
        else
            resolve(('%s: no number (%s), HP %d%%'):format(e.name, a.status, e.hpp))
        end
    end)
    if not sent then resolve('not asked: ' .. tostring(why)) end
end)]=] },
        { name = 'My own entity', code = [=[
local party = vana.game.party()
local me = party and party.members[1]
if me == nil or not me.active or me.target_index == 0 then return 'not in the world' end
return vana.game.entity(me.target_index)]=] },
        { name = 'Monsters within 20 yalms', code = [=[
-- Every entity slot: monsters have spawn flag 0x10; distance is squared yalms.
local list = {}
for index = 0, vana.game.entity_count() - 1 do
    local e = vana.game.entity(index)
    if e and bit.band(e.spawn_flags, 0x10) ~= 0 and e.hpp > 0 and e.valid_target and e.distance < 400 then
        list[#list + 1] = ('%s (index %d) %.1f yalms, HP %d%%'):format(e.name, e.index, math.sqrt(e.distance), e.hpp)
    end
end
return list]=] },
        { name = 'Players nearby', code = [=[
-- Player characters sit in slots 0x400 and up.
local list = {}
for index = 0x400, vana.game.entity_count() - 1 do
    local e = vana.game.entity(index)
    if e and e.type == 0 and e.name ~= '' and e.distance < 2500 then
        list[#list + 1] = ('%s %.1f yalms'):format(e.name, math.sqrt(e.distance))
    end
end
return list]=] },
    } },
    { name = 'Items', calls = {
        { name = 'Inventory summary', code = [=[
-- Bag 0 is the inventory; its slot 0 holds the gil. Bags 1 to 17 are the safe, storage, satchel, wardrobes ...
local bag = vana.game.container(0)
if bag == nil then return 'no inventory yet' end
local used, items = 0, {}
for i = 1, bag.capacity - 1 do
    local item = bag[i]
    if item and item.id ~= 0 then
        used = used + 1
        if #items < 10 then items[#items + 1] = ('slot %d: item %d x%d flags 0x%X'):format(i, item.id, item.count, item.flags) end
    end
end
return { used = used, capacity = bag.capacity, gil = bag[0] and bag[0].count, first_ten = items }]=] },
        { name = 'Equipment', code = [=[
-- equipment()[slot] says which bag and index holds the item worn there (index 0: nothing).
local eq = vana.game.equipment()
if eq == nil then return 'no inventory yet' end
local names = { 'main', 'sub', 'range', 'ammo', 'head', 'body', 'hands', 'legs', 'feet', 'neck', 'waist',
                'left_ear', 'right_ear', 'left_ring', 'right_ring', 'back' }
local worn = {}
for slot = 0, 15 do
    local where = eq[slot]
    if where and where.index ~= 0 then
        local bag = vana.game.container(where.container)
        local item = bag and bag[where.index]
        worn[names[slot + 1]] = item and ('item %d (bag %d slot %d)'):format(item.id, where.container, where.index) or '?'
    end
end
return worn]=] },
    } },
    { name = 'Abilities and spells', calls = {
        { name = 'Spells known', code = [=[
-- A bit per spell id. Bit n set: the player has spell n.
local bits = vana.game.spells()
if bits == nil then return 'not sent yet' end
local known = {}
for id = 0, #bits * 8 - 1 do
    if bit.band(bits:byte(math.floor(id / 8) + 1), bit.lshift(1, id % 8)) ~= 0 then known[#known + 1] = id end
end
return { count = #known, ids = known }]=] },
        { name = 'Abilities, by kind', code = [=[
-- 224 bytes of bits: weapon skills from 0, job abilities from 0x200, pet commands from 0x400, traits from 0x600.
local function set_bits(bits, from, to)
    local ids = {}
    for id = from, to do
        local byte = bits:byte(math.floor(id / 8) + 1)
        if byte and bit.band(byte, bit.lshift(1, id % 8)) ~= 0 then ids[#ids + 1] = id - from end
    end
    return ids
end
local bits = vana.game.abilities()
if bits == nil then return 'not sent yet' end
return { weapon_skills = set_bits(bits, 0, 0x1FF), job_abilities = set_bits(bits, 0x200, 0x3FF),
         pet_commands = set_bits(bits, 0x400, 0x5FF), traits = set_bits(bits, 0x600, 0x6FF) }]=] },
        { name = 'Key items', code = [=[
local bits = vana.game.key_items()
if bits == nil then return 'not sent yet' end
local have = {}
for id = 0, #bits * 8 - 1 do
    if bit.band(bits:byte(math.floor(id / 8) + 1), bit.lshift(1, id % 8)) ~= 0 then have[#have + 1] = id end
end
return { count = #have, ids = have }]=] },
        { name = 'Recast timers', code = [=[
-- Only timers counting down appear, in 1/60ths of a second. Ability keys are recast timer ids, not ability ids.
local r = vana.game.recasts()
if r == nil then return 'not yet' end
local out = { abilities = {}, spells = {} }
for id, frames in pairs(r.abilities) do out.abilities[id] = ('%.1f s'):format(frames / 60) end
for id, frames in pairs(r.spells) do out.spells[id] = ('%.1f s'):format(frames / 60) end
return out]=] },
    } },
    { name = 'World', calls = {
        { name = 'Zone, time and weather', code = [=[
local party = vana.game.party()
local listed = party and party.members[1].zone or 0
return { world = vana.game.world(), zoning = vana.game.zoning(), zone = vana.game.player_zone(listed),
         clock = os.date('%H:%M:%S'), server_time = os.date('!%Y-%m-%d %H:%M:%S', vana.game.world().server_time) }]=] },
        { name = 'Screen size', code = [=[
local width, height = vana.ui.screen()   -- VanaCore's interface size, the pixels vana.ui and vana.imgui draw in
return { width = width, height = height }]=] },
        { name = 'The last zone-in packet', code = [=[
-- The last packet of an id the game received (true) or sent (false), header first. 0x00A is the zone-in.
local p = vana.packets.last(true, 0x00A)
if p == nil or p == '' then return 'none yet' end
local hex = {}
for i = 1, math.min(#p, 64) do hex[#hex + 1] = ('%02X'):format(p:byte(i)) end
return { bytes = #p, zone = p:byte(0x31) + p:byte(0x32) * 256, first_64 = table.concat(hex, ' ') }]=] },
        { name = 'Addons loaded', code = [=[
return { addons = vana.addons.loaded(), plugins = vana.plugins.loaded() }]=] },
    } },
    -- Boxes in the game's 3D space (docs/addons/18-widgets.md). A snippet keeps its boxes in apiexplorer_widgets, so a second
    -- Run updates the box instead of making another; "Remove the widgets" destroys them. In your addon keep them in locals.
    { name = 'Widgets (vana.world)', calls = {
        { name = 'A box at a nameplate', code = [=[
-- vana.world.attach(box, index[, options]): a vana.ui box where the name over target `index` is. It is drawn in the same pass as
-- the names, so walls hide it and the effects, DLSS and frame generation treat it as they treat a name. Run again to update it.
-- side: 'above' (default), 'below', 'left', 'right' or 'over'; x, y: pixels from there; scale; distance = true: it grows and
-- shrinks with the name; walls = false: not hidden by walls.
local t = vana.game.target()
local index = t and t.active and t.index
if not index then
    local party = vana.game.party()
    local me = party and party.members[1]
    index = me and me.active and me.target_index   -- no target: your own nameplate
end
if not index or index == 0 then return 'not in the world' end
local e = vana.game.entity(index)
apiexplorer_widgets = apiexplorer_widgets or {}
-- the look: change it and Run again (the box is made once, then box:set(look) applies it every run)
local look = { text = 'Hello ' .. (e and e.name or '?'), size = 16, color = '#FFFFFF', background = '#B01818E0', padding = 4 }
local box = apiexplorer_widgets.plate
if box == nil or box.destroyed then
    box = vana.ui.text(look)
    apiexplorer_widgets.plate = box
else
    box:set(look)
end
vana.world.attach(box, index, { side = 'above' })
return ('A box above the name of target %d. "Remove the widgets" takes it away.'):format(index)]=] },
        { name = 'Change a placed box without placing it again', code = [=[
-- vana.world.update(box, options): changes some options of a box that is already placed, and only the ones given. It is cheap
-- enough for every frame (a plate's HP bar, its offset in a stack, whether walls hide it). The box's own text, size or colour
-- change with box:set{...}. vana.world.placement(box) reads back how it is placed. Run "A box at a nameplate" first: this moves
-- it to the left of the name, 12 pixels up, and lets it show through walls; run that entry again to put it back.
local box = apiexplorer_widgets and apiexplorer_widgets.plate
if box == nil or box.destroyed then return 'run "A box at a nameplate" first' end
vana.world.update(box, { side = 'left', y = -12, walls = false })
return vana.world.placement(box)]=] },
        { name = 'Hide the game\'s names', code = [=[
-- vana.gameui.hide('nameplates'): no name over any head while this addon holds it (the line under a job master's name too).
-- A widget made with vana.world.attach is still drawn where the name would be: the name is laid out and only its letters are
-- left out. The player's own [graphics] nameplates.hide* settings leave out one kind of name at a time.
return vana.gameui.hide('nameplates')]=] },
        { name = 'Show the game\'s names again', code = [=[
return vana.gameui.show('nameplates')]=] },
        { name = 'A box in the world, in front of you', code = [=[
-- vana.world.place(box, x, y, z[, options]): a box at a point of the world. x, y and z are a position as vana.game.entity has
-- it (z is the height). Walls hide it by the world's depth (a pillar cuts off the part behind it). Options: up (yalms above
-- the point), pivot ('bottom', 'center', 'top', 'left', 'right': the part of the box on the point), x, y (pixels from there),
-- scale, distance (its own size at that many yalms, smaller farther), range (not drawn past that many yalms), walls = false
-- (shown through walls), over_ui = true (drawn over the game's windows). Needs the d3d11 or d3d12 renderer.
local party = vana.game.party()
local me = party and party.members[1]
if me == nil or not me.active or me.target_index == 0 then return 'not in the world' end
local e = vana.game.entity(me.target_index)
if e == nil then return 'not in the world' end
local ahead = 5   -- yalms in front of you; heading 0 faces east and turns clockwise, hence the minus
local x, y = e.x + math.cos(e.heading) * ahead, e.y - math.sin(e.heading) * ahead
apiexplorer_widgets = apiexplorer_widgets or {}
-- the look: change it and Run again (the box is made once, then box:set(look) applies it every run)
local look = { text = 'Here', size = 18, color = '#FFE060', background = '#000000B0', padding = 4 }
local box = apiexplorer_widgets.marker
if box == nil or box.destroyed then
    box = vana.ui.text(look)
    apiexplorer_widgets.marker = box
else
    box:set(look)
end
vana.world.place(box, x, y, e.z, { up = 1.5, pivot = 'bottom', distance = 8, over_ui = false, walls = true })
return { placed = { x = x, y = y, z = e.z }, screen = { vana.world.to_screen(x, y, e.z) }, clear_line = vana.world.visible(x, y, e.z) }]=] },
        { name = 'A box in the world, over the interface', code = [=[
-- over_ui = true draws the box with the vana.ui boxes, after the game's windows and your ImGui windows. It has no depth of its
-- own there: walls = true hides the whole box while the zone's collision (walls, floors, doors) is between the camera and the
-- point, walls = false always shows it. Try both with a wall between you and the box.
local party = vana.game.party()
local me = party and party.members[1]
if me == nil or not me.active or me.target_index == 0 then return 'not in the world' end
local e = vana.game.entity(me.target_index)
if e == nil then return 'not in the world' end
local ahead = 5
local x, y = e.x + math.cos(e.heading) * ahead, e.y - math.sin(e.heading) * ahead
apiexplorer_widgets = apiexplorer_widgets or {}
-- the look: change it and Run again (the box is made once, then box:set(look) applies it every run)
local look = { text = 'Here', size = 18, color = '#FFE060', background = '#000000B0', padding = 4 }
local box = apiexplorer_widgets.marker
if box == nil or box.destroyed then
    box = vana.ui.text(look)
    apiexplorer_widgets.marker = box
else
    box:set(look)
end
vana.world.place(box, x, y, e.z, { up = 1.5, over_ui = true, walls = true })
return 'Placed over the interface; hidden by the zone\'s collision. Change walls to false to see it through walls.']=] },
        { name = 'A line out in front of you', code = [=[
-- A shape out of widgets: twelve small round boxes (vana.ui.rect with a radius) one yalm apart, from you out along the way
-- you face, at chest height. A present handler moves them every frame, so the line follows you as you walk and turn; a
-- second Run replaces it (the handler's key is the same). distance = 6: each dot is its own size 6 yalms from the camera and
-- smaller farther away, so the line narrows into the distance. Remove the widgets takes it away.
apiexplorer_widgets = apiexplorer_widgets or {}
for _, old in ipairs(apiexplorer_widgets.line or {}) do
    if not old.destroyed then old:destroy() end
end
local dots = {}
for i = 1, 12 do
    dots[i] = vana.ui.rect({ width = 10, height = 10, radius = 5, color = i % 4 == 0 and '#FF6040' or '#40E0FF' })
end
apiexplorer_widgets.line = dots
vana.on('present', function ()
    local party = vana.game.party()
    local me = party and party.members[1]
    if me == nil or not me.active or me.target_index == 0 then return end
    local e = vana.game.entity(me.target_index)
    if e == nil then return end   -- gone for a moment while zoning
    for i, dot in ipairs(dots) do
        if not dot.destroyed then
            vana.world.place(dot, e.x + math.cos(e.heading) * i, e.y - math.sin(e.heading) * i, e.z,
                { up = 1.2, pivot = 'center', distance = 6 })
        end
    end
end, 'apiexplorer_line')
return 'A line of 12 dots, 1 to 12 yalms ahead of you. Remove the widgets takes it away.']=] },
        { name = 'A beam of light from the floor', code = [=[
-- An RPG marker: a column of light standing on a point, a glow and two rings of dots turning on the floor round it, discs of
-- light rising up it and sparks climbing it, all placed in the world, so walls hide them. The column is boxes laid in the
-- world with facing = 'upright': each stands on the point, as tall as its height in yalms, and turns round its upright line to
-- face the camera, so it keeps standing up from every side and from above is seen end on, short, over its glow. The glow and
-- the discs lie on the ground (facing = 'ground'). vana.ui boxes have no gradients, so the light is translucent rounded rects,
-- short and tall, wide and narrow: where more of them overlap (low down, in the middle) it's brighter. The dots, the sparks and
-- the label face the camera (round from every side) and keep their size in yalms with distance = D. On your target's feet (it
-- follows them), or 6 yalms ahead of you with nothing targeted. A present handler animates it. A second Run replaces it;
-- Remove the widgets takes it away.
local COLOR = '#40C8FF'   -- the light's colour
local HEIGHT = 6          -- yalms tall
local WIDTH = 1           -- yalms across
local LABEL = '!'         -- floating on top ('' for none)
local D = 12              -- the distance (yalms) the boxes facing the camera are sized for
local party = vana.game.party()
local me = party and party.members[1]
local e = me and me.active and me.target_index ~= 0 and vana.game.entity(me.target_index)
if not e then return 'not in the world' end
local t = vana.game.target()
local follow = t and t.active and t.index or nil
local on = follow and vana.game.entity(follow)
local at = on and { x = on.x, y = on.y, z = on.z } or { x = e.x + math.cos(e.heading) * 6, y = e.y - math.sin(e.heading) * 6, z = e.z }
-- How many pixels a yalm is at D, for the boxes facing the camera: the longest a yalm along x, y or up is on the screen at the
-- beam's spot (z is the game's: up is smaller), times its distance; else at your feet, else a guess.
local function measure(x, y, z)
    local sx, sy, depth = vana.world.to_screen(x, y, z)
    if not sx then return nil end
    local most = 0
    for _, p in ipairs({ { x + 1, y, z }, { x, y + 1, z }, { x, y, z - 1 } }) do
        local px, py = vana.world.to_screen(p[1], p[2], p[3])
        if px then most = math.max(most, math.sqrt((px - sx) ^ 2 + (py - sy) ^ 2)) end
    end
    return most > 0 and most * depth / D or nil
end
local _, screen_h = vana.ui.screen()
local per_yalm = measure(at.x, at.y, at.z) or measure(e.x, e.y, e.z) or screen_h * 0.9 / D
local function pixels(yalms) return math.max(2, math.floor(yalms * per_yalm + 0.5)) end
local r, g, b = tonumber(COLOR:sub(2, 3), 16), tonumber(COLOR:sub(4, 5), 16), tonumber(COLOR:sub(6, 7), 16)
local function tint(white, alpha)   -- the colour taken `white` of the way to white, at alpha 0 to 255
    local function mix(c) return math.floor(c + (255 - c) * white + 0.5) end
    return ('#%02X%02X%02X%02X'):format(mix(r), mix(g), mix(b), alpha)
end
local function dot(yalms, color)   -- a round box facing the camera, `yalms` across at D
    local size = pixels(yalms)
    return vana.ui.rect({ width = size, height = size, radius = math.floor(size / 2), color = color })
end
local function laid(w, h, color)   -- a rounded rect to lay in the world: w by h is its shape; width and height size it in yalms
    return vana.ui.rect({ width = 32, height = math.max(2, math.floor(32 * h / w + 0.5)), radius = 16, color = color })
end
apiexplorer_widgets = apiexplorer_widgets or {}
local function destroy(item)   -- the last Run's
    if type(item) == 'table' then for _, box in pairs(item) do destroy(box) end
    elseif not item.destroyed then item:destroy() end
end
destroy(apiexplorer_widgets.beam or {})
local beam = {}
apiexplorer_widgets.beam = beam
local function place(box, options)   -- drawn in the order placed: the floor first, so the light is over it
    vana.world.place(box, at.x, at.y, at.z, options)
    beam[#beam + 1] = box
    return box
end
local glow = place(laid(1, 1, tint(0.1, 0x38)), { facing = 'ground', width = WIDTH * 2.6, pivot = 'center', up = 0.05 })
if vana.world.placement(glow).facing == nil then
    destroy(beam)
    return 'needs a newer VanaCore: one whose vana.world.place takes facing'
end
-- the rings: { yalms from the middle, dots, dot size, white, alpha, turns a second }
local rings = {}
for n, ring in ipairs({ { WIDTH * 1.1, 32, 0.12, 0.2, 0xE0, 0.4 }, { WIDTH * 0.75, 16, 0.08, 0.5, 0x90, -0.7 } }) do
    rings[n] = { radius = ring[1], speed = ring[6], dots = {} }
    for i = 1, ring[2] do
        rings[n].dots[i] = place(dot(ring[3] * (i % 4 == 0 and 1.8 or 1), tint(ring[4], ring[5])), { up = 0.15, pivot = 'center', distance = D })
    end
end
local discs = {}   -- lying flat, rising up the column
for i = 1, 3 do
    discs[i] = place(laid(1, 1, tint(0.5, 0x50)), { facing = 'ground', width = WIDTH * 1.4, pivot = 'center' })
end
-- the light: { share of WIDTH, white, alpha, heights as shares of HEIGHT }, the widest first, each standing on the point
local light = {}
for _, layer in ipairs({ { 1.8, 0.0, 0x12, { 0.5, 1.0 } },                -- a haze round it
                         { 1.0, 0.15, 0x26, { 0.35, 0.6, 0.85, 1.0 } },   -- the beam
                         { 0.35, 0.7, 0x40, { 0.3, 0.6, 0.9 } } }) do     -- its white core
    local w = WIDTH * layer[1]
    for _, share in ipairs(layer[4]) do
        light[#light + 1] = place(laid(w, HEIGHT * share, tint(layer[2], layer[3])),
                                  { facing = 'upright', width = w, height = HEIGHT * share })
    end
end
local sparks = {}
for i = 1, 12 do sparks[i] = place(dot(0.07, tint(0.8, 0xFF)), { pivot = 'center', distance = D }) end
local label
if LABEL ~= '' then
    label = place(vana.ui.text({ text = LABEL, size = math.min(128, pixels(1.2)), bold = true, color = tint(0.6, 0xFF),
                                 background = '#00000000', stroke = 3, stroke_color = tint(0, 0xC0) }),
                  { up = HEIGHT + 0.3, pivot = 'bottom', distance = D })
end
local start = vana.time.clock()
vana.on('present', function ()
    local now = vana.time.clock() - start
    local f = follow and vana.game.entity(follow)
    if f then at.x, at.y, at.z = f.x, f.y, f.z end
    local pulse = 0.8 + 0.2 * math.sin(now * 3)
    for _, rect in ipairs(light) do
        if not rect.destroyed then
            rect.opacity = pulse
            if f then vana.world.update(rect, { at = at }) end
        end
    end
    if not glow.destroyed then
        glow.opacity = pulse
        if f then vana.world.update(glow, { at = at }) end
    end
    for i, disc in ipairs(discs) do
        local s = (now / 3 + i / #discs) % 1                  -- 0 on the floor to 1 at the top: 3 s a climb
        if not disc.destroyed then
            disc.opacity = math.sin(s * math.pi)              -- fades in off the floor and out at the top
            vana.world.update(disc, { at = at, up = 0.1 + s * HEIGHT, width = WIDTH * (1.4 - 0.7 * s) })
        end
    end
    for _, ring in ipairs(rings) do
        for i, d in ipairs(ring.dots) do
            local a = now * ring.speed + i / #ring.dots * 2 * math.pi
            if not d.destroyed then
                vana.world.update(d, { at = { x = at.x + math.cos(a) * ring.radius, y = at.y + math.sin(a) * ring.radius, z = at.z } })
            end
        end
    end
    for i, spark in ipairs(sparks) do
        local s = (now / 2.5 + i / #sparks) % 1               -- 0 on the floor to 1 at the top: 2.5 s a climb
        local a, out = i * 2.4 + s * 4, WIDTH * (0.12 + 0.06 * (i % 4))   -- spiralling up, each at its own way out
        if not spark.destroyed then
            spark.opacity = math.sin(s * math.pi)
            vana.world.update(spark, { at = { x = at.x + math.cos(a) * out, y = at.y + math.sin(a) * out, z = at.z }, up = s * HEIGHT })
        end
    end
    if label and not label.destroyed then
        vana.world.update(label, { at = at, up = HEIGHT + 0.3 + 0.15 * math.sin(now * 2) })
    end
end, 'apiexplorer_beam')
return { on = on and on.name or '6 yalms ahead of you', pixels_a_yalm_at_D = per_yalm }]=] },
        { name = 'A tag on every monster nearby', code = [=[
-- vana.world.attach on a lot of characters: a tag with the name and HP% over each of the 12 nearest monsters within 25 yalms,
-- drawn at the monster's nameplate, so walls hide it as they hide the name. Walking the entity table is the costly part, so a
-- scan once a second finds the monsters and a present handler every frame only changes the tags' text. A tag appears only
-- where the game draws the monster's name (its own name display setting). To have the tag and not the game's name, turn on
-- /vana > Nameplates > Hide monsters' names: the name is laid out for its place and only its letters are left out.
-- A second Run replaces it; Remove the widgets takes it away.
apiexplorer_widgets = apiexplorer_widgets or {}
for _, old in pairs(apiexplorer_widgets.monsters or {}) do
    if not old.destroyed then old:destroy() end
end
local tags = {}   -- the tags, by the monster's target index
apiexplorer_widgets.monsters = tags
local RANGE = 25 * 25   -- entity.distance is in squared yalms
local near = {}         -- the monsters in range at the last scan, nearest first
local last_scan = -1
local TINTS = { '#601818C0', '#605010C0', '#184818C0' }   -- low, middle and high HP
vana.on('present', function ()
    local now = vana.time.clock()
    if now - last_scan >= 1 then
        last_scan = now
        local found = {}
        for index = 0, vana.game.entity_count() - 1 do
            local e = vana.game.entity(index)
            if e and bit.band(e.spawn_flags, 0x10) ~= 0 and e.hpp > 0 and e.valid_target and e.distance < RANGE then
                found[#found + 1] = { index = index, distance = e.distance }
            end
        end
        table.sort(found, function (a, b) return a.distance < b.distance end)
        near = {}
        local keep = {}
        for i = 1, math.min(12, #found) do
            near[i] = found[i].index
            keep[found[i].index] = true
        end
        for index, tag in pairs(tags) do   -- the ones that left, or died
            if not keep[index] then
                tag:destroy()
                tags[index] = nil
            end
        end
    end
    for _, index in ipairs(near) do
        local e = vana.game.entity(index)
        if e ~= nil and e.hpp > 0 then
            local tag = tags[index]
            if tag == nil then
                tag = vana.ui.text({ text = '', size = 13, color = '#FFFFFF', background = TINTS[3], padding = 2 })
                tags[index] = tag
                vana.world.attach(tag, index, { side = 'above', y = -2 })
            end
            tag:set({ text = ('%s  %d%%'):format(e.name, e.hpp), background = TINTS[e.hpp > 66 and 3 or e.hpp > 33 and 2 or 1] })
        end
    end
end, 'apiexplorer_monsters')
return 'Tags on up to 12 monsters within 25 yalms of you. Remove the widgets takes them away.']=] },
        { name = 'Where a point is on the screen', code = [=[
-- vana.world.to_screen(x, y, z) -> x, y, distance: a point of the world in the interface's pixels (vana.ui's) and how far in front
-- of the camera it is; nil behind the camera or off the screen. vana.world.visible(x, y, z): whether the zone's collision leaves
-- the camera a clear line to it (nil where that is not known). Try "Run every second" and turn the camera.
local t = vana.game.target()
if t == nil or not t.active then return 'target something first' end
local e = vana.game.entity(t.index)
if e == nil then return 'no such entity' end
local sx, sy, distance = vana.world.to_screen(e.x, e.y, e.z)
return { screen_x = sx, screen_y = sy, yalms_from_camera = distance, clear_line = vana.world.visible(e.x, e.y, e.z) }]=] },
        { name = 'Waymarks on the ground', code = [=[
-- vana.world.marks(list): rings of light on the ground, drawn by the scene effects from the scene's own depth, so a slope carries
-- a ring and whatever stands in front of it hides it. The list is this addon's whole set each call (Run again replaces it;
-- vana.world.marks() takes them off). Each { x, y, z, radius, color, strength, shape ('round' or 'square'), height (a column of
-- light rising from it), glyph ('A' to 'D', '1' to '4', over the column), glyph_size, glyph_height }. Here: a waymark A at your
-- feet, a square waymark 1 four yalms ahead of you, and a red ring under your target. Needs the d3d11 or d3d12 renderer with its
-- scene effects (vana.graphics.info().active); shape, height and glyph need VanaCore 0.8.219 or later.
local party = vana.game.party()
local me = party and party.members[1]
if me == nil or not me.active or me.target_index == 0 then return 'not in the world' end
local e = vana.game.entity(me.target_index)
if e == nil then return 'not in the world' end
local ahead = 4   -- heading 0 faces east and turns clockwise, hence the minus
local list = {
    { x = e.x, y = e.y, z = e.z, radius = 1.2, color = '#FFD040', height = 6, glyph = 'A' },
    { x = e.x + math.cos(e.heading) * ahead, y = e.y - math.sin(e.heading) * ahead, z = e.z, radius = 1.2, color = '#5BE0FF',
      shape = 'square', height = 6, glyph = '1' },
}
local t = vana.game.target()
local te = t and t.active and t.index ~= me.target_index and vana.game.entity(t.index)
if te then
    list[#list + 1] = { x = te.x, y = te.y, z = te.z, radius = math.max(1, te.hitbox_size or 1), color = '#FF4F6A', strength = 1.5 }
end
vana.world.marks(list)
local info = vana.graphics.info()
return { marks = #list, drawn = info ~= nil and info.active or false, note = 'Remove the widgets takes them off.' }]=] },
        { name = 'The ground under the mouse', code = [=[
-- vana.world.pick(x, y) -> x, y, z: the point of the zone under a pixel of the interface (vana.ui's, as the mouse event gives it):
-- where the camera's line through it first meets the zone's collision, as vana.game.entity has positions; nil when nothing is met
-- within 150 yalms. The zone's collision, not what is drawn: a tree without collision isn't met, nor a monster or a player. Here
-- a mouse handler picks under the pointer as it moves and keeps a green ring there (it replaces this addon's marks) until
-- Remove the widgets. VanaCore 0.8.219 or later.
local last = 0
vana.on('mouse', function (kind, x, y)
    if kind ~= 0 then return end   -- moves only
    local gx, gy, gz = vana.world.pick(x, y)
    if gx == nil then return end
    vana.world.marks({ { x = gx, y = gy, z = gz, radius = 0.8, color = '#7CFF7C', strength = 1.5 } })
    local now = vana.time.clock()
    if now - last >= 0.25 then
        last = now
        apiexplorer_report(('The ground under the pointer: x %.1f, y %.1f, z %.1f'):format(gx, gy, gz))
    end
end, 'apiexplorer_pick')
return 'Move the mouse over the ground: a green ring follows the pointer. Remove the widgets stops it.']=] },
        { name = 'Where the target\'s name is', code = [=[
-- vana.world.name_at(index) -> x, top, bottom: where the game drew the name over target index lately, in the interface's pixels:
-- the target's place across (its name is centred on it) and the letters' top and bottom. For drawing by a name with your own
-- drawing: a mark just over top sits where the game puts the name, the right height for any model. nil when the name wasn't
-- drawn in the last moments (too far, hidden). Try "Run every second" and turn the camera. VanaCore 0.8.218 or later.
local t = vana.game.target()
if t == nil or not t.active then return 'target something first' end
local x, top, bottom = vana.world.name_at(t.index)
if x == nil then return 'its name isn\'t drawn now' end
return { x = x, top = top, bottom = bottom }]=] },
        { name = 'Remove the widgets', code = [=[
-- Destroying a box ends its placement (an addon's boxes also go when it unloads). vana.world.detach(box) instead puts a box back
-- on the screen at its x and y.
local removed = 0
local function destroy(item)
    if type(item) == 'table' then   -- a list of boxes (the line's dots, the monsters' tags)
        for _, box in pairs(item) do destroy(box) end
    elseif not item.destroyed then
        item:destroy()
        removed = removed + 1
    end
end
for _, item in pairs(apiexplorer_widgets or {}) do destroy(item) end
vana.off('present', 'apiexplorer_line')       -- the line's handler
vana.off('present', 'apiexplorer_monsters')   -- the monsters' tags'
vana.off('present', 'apiexplorer_beam')       -- the beam of light's
vana.off('mouse', 'apiexplorer_pick')         -- the ground under the mouse's
apiexplorer_widgets = nil
vana.gameui.show('nameplates')
vana.world.marks()                            -- the waymarks and rings
return ('%d widget(s) removed, the marks taken off; the game\'s names are shown again.'):format(removed)]=] },
    } },
    { name = 'Game data (static tables)', calls = {
        { name = 'A spell by name', code = [=[
-- vana.gamedata: the game's static tables (names, descriptions, costs ...), generated from the DATs and the server.
-- find() gives an id by name; the table by id has everything about it.
local id = vana.gamedata.find('spells', 'Cure IV')
return { id = id, spell = vana.gamedata.spells[id] }]=] },
        { name = 'An item by id', code = [=[
-- Items are in parts of 4,096 ids: asking for one loads its part (a fraction of a second), not all 25,000.
return vana.gamedata.items[26963]   -- Onca Suit]=] },
        { name = 'An item\'s stats (CatsEyeXI\'s data)', code = [=[
-- What the Equipment window counts for an item: the server's modifiers (names and units), or its description's where
-- CatsEyeXI has changed it since the data was made (source says which), latent effects, sets, each stat's label and unit.
-- Pass a container's slot instead of an id for its augments too: vana.gamedata.item_stats(vana.game.container(8)[1])
local s, why = vana.gamedata.item_stats(27537)   -- Brutal Earring +1
if s == nil then return why end
local shown = {}
for name, value in pairs(s.mods) do
    local stat = s.stats[name]
    shown[#shown + 1] = ('%s %+g%s'):format(stat.label, value / stat.unit, stat.percent and '%' or '')
end
return { shown = shown, stats = s }]=] },
        { name = 'What I have equipped, by name', code = [=[
local eq = vana.game.equipment()
if eq == nil then return 'no inventory yet' end
local names = { 'main', 'sub', 'range', 'ammo', 'head', 'body', 'hands', 'legs', 'feet', 'neck', 'waist',
                'left_ear', 'right_ear', 'left_ring', 'right_ring', 'back' }
local worn = {}
for slot = 0, 15 do
    local where = eq[slot]
    local item = where and where.index ~= 0 and vana.game.container(where.container)[where.index]
    local data = item and vana.gamedata.items[item.id]
    worn[names[slot + 1]] = data and ('%s (lv %d%s)'):format(data.name, data.level or 0,
        data.item_level and data.item_level > 0 and ', ilvl ' .. data.item_level or '') or '-'
end
return worn]=] },
        { name = 'My buffs, by name', code = [=[
local p = vana.game.player()
if p == nil then return 'not logged in' end
local list = {}
for _, id in ipairs(p.buffs) do
    if id >= 0 and id ~= 255 then
        local buff = vana.gamedata.status_effects[id]
        list[#list + 1] = ('%d %s'):format(id, buff and buff.name or '?')
    end
end
return #list > 0 and list or 'no buffs'
]=] },
        { name = 'The zone I am in', code = [=[
local party = vana.game.party()
local zone = vana.game.player_zone(party and party.members[1].zone or 0)
return { id = zone, zone = vana.gamedata.zones[zone] }]=] },
        { name = 'Spells my job can cast at my level', code = [=[
-- jobs = { [job id] = level the job learns it at }
local p = vana.game.player()
if p == nil then return 'not logged in' end
local list = {}
for id, spell in pairs(vana.gamedata.all('spells')) do
    local level = spell.jobs[p.main_job]
    if level ~= nil and level <= p.main_job_level then
        list[#list + 1] = ('%s (lv %d, %d MP)'):format(spell.name, level, spell.mp)
    end
end
table.sort(list)
return { count = #list, spells = list }]=] },
        { name = 'Tables available', code = [=[
return vana.gamedata.tables()   -- each with its row count and the date it was generated]=] },
    } },
    -- These return a promise: the explorer waits for it (the game goes on) and shows what it holds, or why it failed.
    -- They need a VanaCore that has vana.http (docs/addons/17-http-and-websockets.md).
    { name = 'Web requests (vana.http)', calls = {
        { name = 'launcher.json: the server address', code = [=[
-- vana.http.json(url) is a promise of the decoded JSON. The request runs on another thread: the game doesn't stop, and
-- the explorer shows the value when it arrives.
return vana.http.json('https://launcher.catseyexi.com/launcher/launcher.json'):next(function (launcher)
    return launcher.url_server_address
end)]=] },
        { name = 'launcher.json: every address in it', code = [=[
return vana.http.json('https://launcher.catseyexi.com/launcher/launcher.json'):next(function (launcher)
    local found = { launcher_version = launcher.launcher_version }
    for key, value in pairs(launcher) do
        if key:match('^url_') then found[key] = value end
    end
    return found
end)]=] },
        { name = 'launcher.json: status, headers and size', code = [=[
-- vana.http.get gives the whole response: an answer is a response whatever its status.
return vana.http.get('https://launcher.catseyexi.com/launcher/launcher.json'):next(function (res)
    return { ok = res.ok, status = res.status, reason = res.reason, url = res.url, bytes = #res.body,
             content_type = res:header('Content-Type'), headers = res.headers }
end)]=] },
        { name = 'The same with await', code = [=[
-- vana.async runs the function as a task; await waits for a promise without stopping the game, and raises the error
-- if the request failed (pcall catches it).
return vana.async(function ()
    local res = vana.http.get('https://launcher.catseyexi.com/launcher/launcher.json'):await()
    local launcher = res:json()
    return { status = res.status, server = launcher.url_server_address, test_server = launcher.url_test_address }
end)]=] },
        { name = 'The same with a callback', code = [=[
-- A callback is the other way to ask: fn(res, err), one of them nil. In your addon, print() or act in place of
-- apiexplorer_report, which only puts the text in the output box.
vana.http.get('https://launcher.catseyexi.com/launcher/launcher.json', function (res, err)
    if err then
        apiexplorer_report(('failed: %s - %s'):format(err.code, err.message))
        return
    end
    apiexplorer_report(('status %d, server address: %s'):format(res.status, res:json().url_server_address))
end)
return 'waiting for the answer']=] },
        { name = 'launcher.json, then the player count', code = [=[
-- A promise returned from a handler is followed: the second request starts when the first has answered.
return vana.http.json('https://launcher.catseyexi.com/launcher/launcher.json'):next(function (launcher)
    return vana.http.get(launcher.url_player_count)
end):next(function (res)
    return { status = res.status, body = res.body:sub(1, 200) }
end)]=] },
        { name = 'Three requests at once (promise.all)', code = [=[
local started = vana.time.clock()
local urls = {
    'https://launcher.catseyexi.com/launcher/launcher.json',
    'https://catseyexi.com/PlayerCount.txt',
    'https://catseyexi.com/launcher_discord.json',
}
local requests = {}
for i, url in ipairs(urls) do requests[i] = vana.http.get(url) end
return vana.promise.all(requests):next(function (all)
    local lines = {}
    for i, res in ipairs(all) do lines[i] = ('%d  %7d bytes  %s'):format(res.status, #res.body, urls[i]) end
    return { seconds = vana.time.clock() - started, results = lines }
end)]=] },
        { name = 'A 404 is still a response', code = [=[
-- The promise is rejected only when there is no answer at all (next test), or with the option strict = true.
return vana.http.get('https://launcher.catseyexi.com/launcher/no-such-file.json'):next(function (res)
    return { ok = res.ok, status = res.status, reason = res.reason }
end)]=] },
        { name = 'An error: no such host', code = [=[
-- err.code says why: timeout, dns, connect, tls, cancelled, too_large, redirect, file, http, json, invalid_request, busy.
return vana.http.get('https://no-such-host.invalid/'):next(nil, function (err)
    return { code = err.code, message = err.message }
end)]=] },
        { name = 'Cancel a request', code = [=[
local request = vana.http.get('https://launcher.catseyexi.com/launcher/launcher.json')
local stopped = request:cancel()   -- true: there was a request to stop
return request:next(nil, function (err) return { cancel_returned = stopped, code = err.code } end)]=] },
        { name = 'Download to a file', code = [=[
-- download() writes the body to <file>.part as it arrives and moves it into place when it is all there: nothing is held
-- in memory. This test deletes the file again.
local path = (os.getenv('TEMP') or '.') .. '\\apiexplorer-launcher.json'
return vana.http.download('https://launcher.catseyexi.com/launcher/launcher.json', path):next(function (res)
    local file = io.open(res.saved, 'rb')
    local size = file and file:seek('end')
    if file then file:close() end
    os.remove(res.saved)
    return { status = res.status, saved = res.saved, bytes_on_disk = size }
end)]=] },
        { name = 'A WebSocket that cannot connect', code = [=[
-- vana.ws.connect returns at once; opened() is a promise that is rejected if the socket never opens. A working one:
--   socket:on('message', function (data) print(data) end); socket:send('hello'); socket:close()
local socket = vana.ws.connect('wss://no-such-host.invalid/live')
return socket:opened():next(nil, function (err)
    return { code = err.code, message = err.message, state = socket.state }
end)]=] },
        { name = 'What the network layer holds', code = [=[
return { requests_going = vana.http.pending(), sockets = vana.ws.count(), info = vana.http.info() }]=] },
    } },
    -- vana.gameui (docs/addons/07-game-interface.md): what an addon can hide, move and read of the game's interface, and
    -- how to find a window's name
    { name = 'Game interface (vana.gameui)', calls = {
        { name = 'Elements to hide, move or change', code = [=[
-- What vana.gameui.hide / move / image / add / replace take by name, and which of those each one takes.
-- Any of the game's windows can be hidden too, by its own name: the next two entries find one.
local list = {}
for _, e in ipairs(vana.gameui.elements()) do
    list[#list + 1] = ('%-24s %-32s %s'):format(e.id, e.name, table.concat(e.actions, ', '))
end
return list]=] },
        { name = 'Where the Target window is', code = [=[
-- vana.gameui.rect('target_window') -> left, top, right, bottom: where the game's Target window is now, in the interface's
-- pixels (vana.ui.screen's), wherever the player or an addon moved it, for drawing by it (a mark by the target's name). nil
-- while nothing is targeted or it's hidden. Only target_window has a place to give. VanaCore 0.8.218 or later.
local left, top, right, bottom = vana.gameui.rect('target_window')
if left == nil then return 'the Target window isn\'t up: target something' end
return { left = left, top = top, right = right, bottom = bottom, width = right - left, height = bottom - top }]=] },
        { name = 'Game windows up now', code = [=[
-- The names of the game's windows that are up, in the game's order. Open a menu and Run: its name is the new one.
-- vana.gameui.hide(name) hides that window, vana.gameui.window(name) reads it.
return vana.gameui.windows()]=] },
        { name = 'Find a window\'s name (as you open it)', code = [=[
-- vana.gameui.watch('*', fn): fn(e) when any window opens, changes or closes. This lists each window as it opens,
-- until 'Stop finding window names'. Every window's draws are kept while it runs: in your addon, watch the ones you draw.
apiexplorer_find_stop()
local seen = {}
apiexplorer_find = vana.gameui.watch('*', function (e)
    local w = e.window
    if e.what ~= 'open' or w == nil then return end
    local first = w.rows[1] and w.rows[1].text or ''
    seen[#seen + 1] = ('%-10s at %d, %d  %d x %d  %d rows%s'):format(e.name, math.floor(w.x), math.floor(w.y),
        math.floor(w.w), math.floor(w.h), #w.rows, first ~= '' and ('  first: ' .. first) or '')
    apiexplorer_report('Windows opened since Run, newest last:\n' .. table.concat(seen, '\n'))
end)
return 'Open any menu or window in the game: its name shows here as it opens.']=] },
        { name = 'Stop finding window names', code = [=[
return apiexplorer_find_stop() and 'stopped' or 'nothing was running']=] },
        { name = 'Read a window (the main menu)', code = [=[
-- vana.gameui.window(name): its place, cursor, rows (each button's id and label), and the text and pictures its last
-- draw made. Open the main menu first. The first read of a window nobody watches has drawn = false and no lines:
-- Run again (or watch it) for the whole window.
return vana.gameui.window('menuwind') or 'the main menu (menuwind) isn\'t up: open it and Run again']=] },
        { name = 'Who hides what', code = [=[
-- vana.gameui.hidden(element): the addons and plugins hiding it, nil while nobody does (not the player's own settings).
local out = {}
for _, e in ipairs(vana.gameui.elements()) do
    local by = vana.gameui.hidden(e.id)
    if by ~= nil then out[e.id] = by end
end
return next(out) ~= nil and out or 'nothing is hidden by an addon or plugin']=] },
        { name = 'Hide the main menu for 5 seconds', code = [=[
-- vana.gameui.hide(window): any window by its name, drawn into nothing; the keys still work it, the mouse doesn't.
-- Open the main menu first. vana.gameui.show gives it back (VanaCore does too when your addon unloads).
vana.gameui.hide('menuwind')
vana.tasks.once(5, function () vana.gameui.show('menuwind') end)
return 'menuwind hidden: it comes back in 5 seconds.']=] },
    } },
    -- Each of these is a one-shot test: Run sets the trigger and says what to do; when it fires, the trigger reports
    -- here (apiexplorer_report, below) and removes itself. One test waits at a time: a new Run replaces it.
    { name = 'Menu triggers', calls = {
        { name = 'Catch Equipment in the main menu', code = [=[
-- vana.gameui.catch(menu, event[, items], fn): fn(e) at the next frame when the player does `event` in `menu`, and
-- the game's own action is left out: the Equipment window doesn't open. In your addon, drop the apiexplorer_ lines.
apiexplorer_test_begin()
local id
id = vana.gameui.catch('main_menu', 'select', 'equipment', function (e)
    vana.gameui.off(id)   -- a one-shot: gone once it has fired (an addon's triggers also go when it unloads)
    apiexplorer_report(("Caught %s: %s item %d, event '%s'. The game's Equipment window stayed closed.\nTrigger %d removed."):format(
        e.name, e.menu, e.item, e.event, id))
end)
apiexplorer_test = id
return 'Test by opening the main menu and picking Equipment: the game\'s window should stay closed.']=] },
        { name = 'Hear the next choice in any menu', code = [=[
-- vana.gameui.on(menu, event[, items], fn): the same, but the game acts as well. '*' is every menu.
apiexplorer_test_begin()
local id
id = vana.gameui.on('*', 'select', function (e)
    vana.gameui.off(id)
    apiexplorer_report(("Heard a choice: %s item %d%s (event '%s', caught: %s). The game acted as usual.\nTrigger %d removed."):format(
        e.menu, e.item, e.name ~= '' and ' (' .. e.name .. ')' or '', e.event, tostring(e.caught), id))
end)
apiexplorer_test = id
return 'Test by picking anything in any game menu.']=] },
        { name = 'Hear the next choice in the Party list', code = [=[
-- A menu by its alias ('party_menu' is the main menu's Party list, window prty1) with its items named: seek_party,
-- find_member, request, view_request, autogroup, languages, alarm_type.
apiexplorer_test_begin()
local id
id = vana.gameui.on('party_menu', 'select', function (e)
    vana.gameui.off(id)
    apiexplorer_report(('Heard Party > %s (item %d).\nTrigger %d removed.'):format(e.name ~= '' and e.name or '?', e.item, id))
end)
apiexplorer_test = id
return 'Test by opening the main menu, then Party, and picking an item.']=] },
        { name = 'Hear the next cursor move in the main menu', code = [=[
-- Every event ('*'): the cursor moving (up, down, left, right), select, cancel, the menu key. e.event names it.
apiexplorer_test_begin()
local id
id = vana.gameui.on('main_menu', '*', function (e)
    vana.gameui.off(id)
    apiexplorer_report(("Heard '%s' in %s on item %d%s.\nTrigger %d removed."):format(
        e.event, e.menu, e.item, e.name ~= '' and ' (' .. e.name .. ')' or '', id))
end)
apiexplorer_test = id
return 'Test by opening the main menu and moving the cursor.']=] },
        { name = 'Cancel a waiting test', code = [=[
if apiexplorer_test == nil then return 'no test is waiting' end
vana.gameui.off(apiexplorer_test)
local id = apiexplorer_test
apiexplorer_test = nil
return ('Trigger %d removed.'):format(id)]=] },
    } },
}

local CALLS, BY_NAME = {}, {}
for _, group in ipairs(GROUPS) do
    for _, call in ipairs(group.calls) do
        call.group = group.name
        CALLS[#CALLS + 1] = call
        BY_NAME[call.name] = call
    end
end

-- Printing what a call returns: tables with sorted keys, short lists on one line, strings quoted when they're text and
-- as hex when they're bytes (the game's bit strings, an item's extra data).
local function scalar(value)
    local t = type(value)
    if t == 'string' then
        if value:find('[^\32-\126\t\r\n]') == nil then
            local s = value:gsub('\\', '\\\\'):gsub("'", "\\'"):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t')
            return "'" .. s .. "'"
        end
        local hex = {}
        for i = 1, math.min(#value, 24) do hex[#hex + 1] = ('%02X'):format(value:byte(i)) end
        return ('<%d bytes: %s%s>'):format(#value, table.concat(hex, ' '), #value > 24 and ' ...' or '')
    elseif t == 'number' then
        if value == math.floor(value) and math.abs(value) < 2^53 then return ('%d'):format(value) end
        return (('%.4f'):format(value):gsub('0+$', ''):gsub('%.$', ''))
    end
    return tostring(value)
end

local function key_order(a, b)
    local ta, tb = type(a), type(b)
    if ta ~= tb then return ta == 'number' end
    if ta == 'number' or ta == 'string' then return a < b end
    return tostring(a) < tostring(b)
end

local function dump(value, indent, out, seen)
    if type(value) ~= 'table' then
        out[#out + 1] = scalar(value)
        return
    end
    if seen[value] then
        out[#out + 1] = '<the same table again>'
        return
    end
    if indent > 10 then
        out[#out + 1] = '{ ... }'
        return
    end
    local keys = {}
    for k in pairs(value) do keys[#keys + 1] = k end
    if #keys == 0 then
        out[#out + 1] = '{}'
        return
    end
    table.sort(keys, key_order)
    seen[value] = true
    -- a list of a few scalars stays on one line
    local list = #keys <= 24
    for i, k in ipairs(keys) do
        if not list then break end
        list = k == i and type(value[k]) ~= 'table'
    end
    if list then
        local parts = {}
        for _, k in ipairs(keys) do parts[#parts + 1] = scalar(value[k]) end
        local line = '{ ' .. table.concat(parts, ', ') .. ' }'
        if #line <= 110 then
            out[#out + 1] = line
            seen[value] = nil
            return
        end
    end
    local pad = ('    '):rep(indent + 1)
    out[#out + 1] = '{\n'
    for i, k in ipairs(keys) do
        if i > 300 then
            out[#out + 1] = pad .. ('... %d more\n'):format(#keys - 300)
            break
        end
        out[#out + 1] = pad
        if type(k) == 'string' and k:match('^[%a_][%w_]*$') then
            out[#out + 1] = k
        else
            out[#out + 1] = '[' .. scalar(k) .. ']'
        end
        out[#out + 1] = ' = '
        dump(value[k], indent + 1, out, seen)
        out[#out + 1] = ',\n'
    end
    out[#out + 1] = ('    '):rep(indent) .. '}'
    seen[value] = nil
end

local function describe(...)
    local count = select('#', ...)
    if count == 0 then return '(nothing returned)' end
    local out = {}
    for i = 1, count do
        if i > 1 then out[#out + 1] = '\n' end
        dump((select(i, ...)), 0, out, {})
    end
    return table.concat(out)
end

-------------------------------------------------------------------------------
-- The reference: every table and function in `vana` as the game has it now, described from the docs
-------------------------------------------------------------------------------

-- reference.lua (made by conflux/tools/gen_apiexplorer_reference.py from docs/addons/15-api-reference.md, 02-events.md and
-- the topic pages): `spaces`, the tables in the docs' order with their page and notes; `entries`, one a row of the docs:
-- the table it's filed under (ns), its name in the list (label), what it takes and returns (sig), what it does (desc), the
-- paths to look up in the game (live: `*` ends a wildcard), what a snippet's code has when it uses it (find), a call to try
-- (call) or the event to listen for (event), and an event's other columns (more).
local REFERENCE = (function ()
    local ok, data = pcall(require, 'reference')
    if ok and type(data) == 'table' then
        return data
    end
    vana.log('apiexplorer: reference.lua didn\'t load (' .. tostring(data) .. '): the Reference tab lists what the game has, undescribed')
    return { spaces = {}, entries = {} }
end)()

-- A path looked up from the globals with rawget, so nothing is loaded by asking (vana.gamedata's tables load on first use)
local function resolve(path)
    local value = _G
    for part in path:gmatch('[^.]+') do
        if type(value) ~= 'table' then return nil end
        value = rawget(value, part)
    end
    return value
end

-- A table of functions (vana.game, vana.ashita.regex), or right under vana one of plain values (vana.paths); not data (a
-- vana.gamedata table) or a struct's type
local function is_namespace(t, depth)
    local looked, plain = 0, depth == 0
    for _, v in next, t do
        if type(v) == 'function' then return true end
        if type(v) == 'table' or type(v) == 'userdata' then plain = false end
        looked = looked + 1
        if looked >= 64 then return false end
    end
    return plain and looked > 0
end

-- The live `vana`: { [table path] = { [key] = value } } for every table of functions under it, down to vana.x.y, with the
-- functions and values in each (names starting with _ are VanaCore's own)
local function walk_vana()
    local tables = {}
    local function visit(path, t, depth)
        local members = {}
        tables[path] = members
        for k, v in next, t do
            if type(k) == 'string' and k:sub(1, 1) ~= '_' then
                if type(v) == 'table' and depth < 2 and is_namespace(v, depth) then
                    visit(path .. '.' .. k, v, depth + 1)
                else
                    members[k] = v
                end
            end
        end
    end
    if type(vana) == 'table' then visit('vana', vana, 0) end
    return tables
end

-- Whether a snippet's code uses a name: a path (vana.game.entity, not vana.game.entity_count), or text with a ( in it
-- (`:next(`, `on('present'`) anywhere
local function mentions(text, find)
    local anywhere = find:find('(', 1, true) ~= nil
    local at = 1
    while true do
        local i, j = text:find(find, at, true)
        if i == nil then return false end
        if anywhere then return true end
        if not text:sub(i - 1, i - 1):match('[%w_.:]') and not text:sub(j + 1, j + 1):match('[%w_]') then return true end
        at = j + 1
    end
end

local function shown_value(v)
    if type(v) == 'table' then return '(a table)' end
    return scalar(v)
end

-- The reference as the window shows it: { spaces = { { name, title, page, notes, entries } }, entries }. Each entry gets
-- id, space, missing (the docs name it, this VanaCore hasn't it), extra (this VanaCore has it, the docs don't), now (its
-- value, for a value), examples (the snippets that use it) and search (its text, lower case, for the filter); each call
-- gets uses (the entries its code uses).
local function build_reference()
    local spaces, by_name = {}, {}
    local function space(name, from)
        local s = by_name[name]
        if s == nil then
            s = { name = name, title = from and from.title, page = from and from.page or '15-api-reference.md',
                  notes = from and from.notes or {}, entries = {} }
            by_name[name] = s
            spaces[#spaces + 1] = s
        end
        return s
    end
    for _, s in ipairs(REFERENCE.spaces or {}) do space(s.name, s) end

    local live = walk_vana()
    local entries = {}
    local function add(e)
        e.id = #entries + 1
        e.space = space(e.ns)
        e.desc = e.desc or ''
        e.examples = {}
        local text = { e.label, e.sig, e.desc }
        for _, m in ipairs(e.more or {}) do text[#text + 1] = m[2] end
        e.search = table.concat(text, '\n'):lower()
        entries[e.id] = e
        e.space.entries[#e.space.entries + 1] = e
    end

    -- the docs' entries, each looked up in the game
    local covered, wildcards, covered_functions = {}, {}, {}
    for _, e in ipairs(REFERENCE.entries or {}) do
        local found = 0
        e.now = {}
        for _, name in ipairs(e.live or {}) do
            if name:sub(-1) == '*' then   -- vana.memory.write_*, vana.gamedata.* (its tables, as they load)
                local path, prefix = name:match('^(.*)%.([^.]*)%*$')
                wildcards[#wildcards + 1] = name:sub(1, -2)
                for k in pairs(live[path] or {}) do
                    if k:sub(1, #prefix) == prefix then found = found + 1 end
                end
            else
                local v = resolve(name)
                if v ~= nil then
                    found = found + 1
                    covered[name] = true
                    if type(v) == 'function' then
                        covered_functions[v] = true
                    else
                        e.now[#e.now + 1] = name .. ' = ' .. shown_value(v)
                    end
                end
            end
        end
        e.missing = #(e.live or {}) > 0 and found == 0
        add(e)
    end

    -- what the game has that no entry names: listed too, so nothing new is left out (one that is the same function as a
    -- named one, as vana.promise['then'] is next, isn't)
    local function is_covered(path, v)
        if covered[path] or (type(v) == 'function' and covered_functions[v]) then return true end
        for _, w in ipairs(wildcards) do
            if path:sub(1, #w) == w then return true end
        end
        return false
    end
    local imgui_functions = false
    local paths = {}
    for path in pairs(live) do paths[#paths + 1] = path end
    table.sort(paths)
    for _, path in ipairs(paths) do
        local keys = {}
        for k in pairs(live[path]) do keys[#keys + 1] = k end
        table.sort(keys)
        for _, k in ipairs(keys) do
            local full, v = path .. '.' .. k, live[path][k]
            if not is_covered(full, v) then
                local fn = type(v) == 'function'
                local e = { ns = path, label = fn and k .. '()' or k, sig = fn and full .. '(...)' or full, live = { full },
                            find = { full }, page = by_name[path] and by_name[path].page or '15-api-reference.md',
                            now = fn and {} or { full .. ' = ' .. shown_value(v) } }
                if path == 'vana.imgui' and k == 'types' then
                    e.desc = 'ImGui\'s structures by name: vana.imgui.types.<Struct>.new() makes one.'
                elseif path == 'vana.imgui' then
                    e.desc = ''   -- ImGui::<name>, as the table's notes say
                    imgui_functions = true
                else
                    e.extra = true
                    e.desc = 'In this VanaCore, not in the API reference (docs/addons/15-api-reference.md) yet: what it takes and does isn\'t described.'
                    e.call = fn and full .. '()' or nil
                end
                add(e)
            end
        end
    end

    if imgui_functions then
        local s = space('vana.imgui')
        local notes = {}
        for i, note in ipairs(s.notes) do notes[i] = note end
        notes[#notes + 1] = 'Each function below is ImGui::<its name>, with the arguments Dear ImGui 1.92 gives it: '
            .. 'imgui.h or ImGui\'s own docs describe them.'
        s.notes = notes
    end

    -- the snippets that use each entry, and the entries each snippet uses
    for _, call in ipairs(CALLS) do call.uses = {} end
    for _, e in ipairs(entries) do
        for _, call in ipairs(CALLS) do
            for _, find in ipairs(e.find or {}) do
                if mentions(call.code, find) then
                    e.examples[#e.examples + 1] = call
                    call.uses[#call.uses + 1] = e
                    break
                end
            end
        end
    end

    -- a table the docs have that this VanaCore hasn't at all
    for _, s in ipairs(spaces) do
        local looked, missing = 0, 0
        for _, e in ipairs(s.entries) do
            if #(e.live or {}) > 0 then
                looked = looked + 1
                if e.missing then missing = missing + 1 end
            end
        end
        s.absent = looked > 0 and missing == looked
    end
    return { spaces = spaces, entries = entries, by_name = by_name }
end

-- The window's state
local open = { true }
local current = nil           -- the call picked
local code = { '' }           -- the code box (editable)
local output = { '' }         -- the output box (read only): the last run's result
local status = ''
local auto = { false }
local last_run = 0
local MAX_OUTPUT = 200000

local function pick(call)
    current = call
    code[1] = call.code
    config.data.call = call.name
    pcall(config.save, config)
end

local function collect(...)
    return { n = select('#', ...), ... }
end

-- A call that returns a promise (vana.http, vana.ws) is waited for: the output says so, and shows what the promise holds
-- (or why it was rejected) when it settles. A new Run drops one still waiting (and cancels its request).
local run_id = 0
local waiting = nil

local function run()
    run_id = run_id + 1
    local id = run_id
    apiexplorer_listen(nil)   -- a Run ends listening for an event (a Listen snippet starts it again)
    if waiting ~= nil then
        local old = waiting
        waiting = nil
        pcall(function () old:cancel() end)
    end
    local started = vana.time.clock()
    local name = current and current.name or 'code'
    local fn, err = loadstring(code[1], '=apiexplorer')
    local text, promise
    if fn == nil then
        text = 'syntax error: ' .. tostring(err)
    else
        local r = collect(pcall(fn))
        if not r[1] then
            text = 'error: ' .. tostring(r[2])
        elseif vana.promise ~= nil and vana.promise.is(r[2]) then
            promise = r[2]
        else
            text = describe(unpack(r, 2, r.n))   -- exactly what the call returned, no trailing nils
        end
    end

    local function show(body, what)
        local took = (vana.time.clock() - started) * 1000
        status = ('%s in %.2f ms, %d bytes'):format(what, took, #body)
        local header = ('-- %s  (%s, %s)\n'):format(name, os.date('%H:%M:%S'), status)
        output[1] = (header .. body):sub(1, MAX_OUTPUT) -- each run shows its own result alone
        last_run = vana.time.clock()
    end

    if promise == nil then
        show(text, 'ran')
        return
    end
    waiting = promise
    status = 'waiting for the answer ...'
    output[1] = ('-- %s  (%s)\n(waiting for the answer)'):format(name, os.date('%H:%M:%S'))
    promise:next(function (value)
        if id ~= run_id then return end
        waiting = nil
        show(describe(value), 'answered')
    end, function (reason)
        if id ~= run_id then return end
        waiting = nil
        show('rejected: ' .. describe(reason), 'rejected')
    end)
end

-- ImGui 1.92's numbers (docs/addons/06-drawing.md: VanaCore addons use ImGui's own values)
-- For the Menu triggers entries, which report from a trigger's callback later than their Run: apiexplorer_test_begin()
-- drops a test still waiting (one at a time), apiexplorer_test holds the waiting trigger's id, apiexplorer_report(text)
-- puts the result in the output box in place of the "test by ..." line.
apiexplorer_test = nil
function apiexplorer_test_begin()
    if apiexplorer_test ~= nil then
        vana.gameui.off(apiexplorer_test)
        apiexplorer_test = nil
    end
end
function apiexplorer_report(text)
    apiexplorer_test = nil
    output[1] = ('-- %s  (%s)\n%s'):format(current and current.name or 'code', os.date('%H:%M:%S'), text)
    status = 'fired at ' .. os.date('%H:%M:%S')
end

-- For 'Find a window's name': the watch running (its id), and stopping it (true when one was)
apiexplorer_find = nil
function apiexplorer_find_stop()
    if apiexplorer_find == nil then return false end
    vana.gameui.unwatch(apiexplorer_find)
    apiexplorer_find = nil
    return true
end

-- For the Reference's Listen (an event's entry): apiexplorer_listen(name) listens for that event and shows in the output
-- box how many came and the last one's arguments (four times a second at most); apiexplorer_listen(nil) stops. One event at
-- a time. The handler returns nothing, so the game still gets what it would.
local listening = nil         -- { name, count, text, at, dirty, started, call }
function apiexplorer_listen(name)
    if listening ~= nil then
        vana.off(listening.name, 'apiexplorer_listen')
        listening = nil
    end
    if name == nil then return false end
    local l = { name = name, count = 0, at = -1, started = os.date('%H:%M:%S'), call = current and current.name or name }
    listening = l
    vana.on(name, function (...)
        l.count = l.count + 1
        local now = vana.time.clock()
        if now - l.at >= 0.25 then
            l.at = now
            l.text = select('#', ...) == 0 and '(nothing: this event hands its handler no arguments)' or describe(...)
            l.when = os.date('%H:%M:%S')
            l.dirty = true
        end
    end, 'apiexplorer_listen')
    return ('Listening for %s since %s: nothing yet. Each one shows here; Stop listening (or another Run) ends it.'):format(name, l.started)
end

local function show_listening()
    local l = listening
    if l == nil or not l.dirty then return end
    l.dirty = false
    output[1] = ('-- %s  (since %s)\n%d %s event(s); the last, at %s, handed the handler:\n%s'):format(
        l.call, l.started, l.count, l.name, l.when, l.text):sub(1, MAX_OUTPUT)
    status = ('%d %s'):format(l.count, l.name)
end

local READ_ONLY = 512         -- ImGuiInputTextFlags_ReadOnly (1 << 9)
local ALLOW_TAB = 32          -- ImGuiInputTextFlags_AllowTabInput (1 << 5)
local FIRST_USE_EVER = 4      -- ImGuiCond_FirstUseEver

-------------------------------------------------------------------------------
-- The look: /vana's own drawing layer (vana\draw.lua: its palette, fonts, icons, buttons, segments and tooltips), loaded
-- into a table of this addon's as freecam does, so the window is made of the same pieces as /vana's and /vana stats'
-------------------------------------------------------------------------------

local kit = { st = { anim = {}, dt = 0, last = 0 } }
do
    local chunk, err = loadfile(vana.paths.addons .. '\\vanacore\\vana\\draw.lua')
    if chunk ~= nil then
        local ok, why = pcall(chunk, kit)
        err = not ok and why or nil
    end
    if err ~= nil then
        print('apiexplorer: vana\\draw.lua: ' .. tostring(err))
        return
    end
end

local S, P, fonts = kit.S, kit.P, kit.fonts
local BODY, SMALL = kit.BODY, kit.SMALL

-- icons this window uses that /vana's list (kit.FA) hasn't: Font Awesome's code points
kit.FA.code, kit.FA.play, kit.FA.stop = 0xF121, 0xF04B, 0xF04D

-- Darker than /vana's page where there's reading to do: the page, and darker still the code and output boxes
local INK = { page = '#0B0D13', box = '#07080C', box_edge = '#262A3A' }

-- '#RRGGBB' as ImGui's colour, made once
local RGBA = setmetatable({}, { __index = function (t, hex)
    local v = kit.rgba(hex)
    t[hex] = v
    return v
end })

-- Segoe UI as /vana has it, the cuts this window uses (code is in ImGui's own font, Agave, monospaced). Added in the first frame
-- the window draws, used from the next: ImGui takes a new font up between frames. A Windows (or Wine) without one gets the next.
function kit.load_fonts()
    local dir = ((os and os.getenv and os.getenv('WINDIR')) or 'C:\\Windows') .. '\\Fonts\\'
    local function add(file, fallback)
        local found = io and io.open and io.open(dir .. file, 'rb')
        if not found then
            return fallback
        end
        found:close()
        return imgui.AddFontFromFileTTF(dir .. file, 18) or fallback
    end
    fonts.icons = imgui.GetFont()
    fonts.text = add('segoeui.ttf', fonts.icons)
    fonts.semi = add('seguisb.ttf', fonts.text)
    fonts.bold_italic = add('segoeuiz.ttf', fonts.semi)
    fonts.semi_italic = add('seguisbi.ttf', fonts.semi)
end

local function clamp(v, lo, hi)
    return math.max(lo, math.min(hi, v))
end

-- This window's own scale (the top line's - 100% +), a percent of /vana's: [apiexplorer] scale, 60 to 200 in tens
local SCALE_MIN, SCALE_MAX, SCALE_STEP = 60, 200, 10
local function own_scale()
    return clamp(tonumber(config.data.scale) or 100, SCALE_MIN, SCALE_MAX)
end

-- as big as /vana's window ([vana] scale, a percent), times this window's own
local function user_percent()
    return (tonumber(vana.config.get('vana', 'scale')) or 100) * own_scale() / 100
end

-- ImGui's own widgets in the window (the filter, code and output boxes, scroll bars, a selection, tooltips) in /vana's colours,
-- as vanaui.cpp's push_theme does for /vana stats. ImGuiCol numbers past kit.COL's: ResizeGrip 30 to 32, InputTextCursor 33,
-- TextSelectedBg 53.
local THEME = {}
for _, c in ipairs({
    { kit.COL.Text, P.text }, { kit.COL.TextDisabled, P.dim }, { kit.COL.ChildBg, P.bg, 0 }, { kit.COL.PopupBg, '#0F1118' },
    { kit.COL.Border, INK.box_edge }, { kit.COL.FrameBg, INK.box }, { kit.COL.FrameBgHovered, INK.box },
    { kit.COL.FrameBgActive, INK.box }, { kit.COL.ScrollbarBg, P.bg, 0 }, { kit.COL.ScrollbarGrab, '#3A4158' },
    { kit.COL.ScrollbarGrabHovered, '#4A5270' }, { kit.COL.ScrollbarGrabActive, P.accent_lo }, { 30, P.bg, 0 },
    { 31, P.accent_lo }, { 32, P.accent }, { 33, P.accent2 }, { 53, P.accent, 0.35 },
}) do
    THEME[#THEME + 1] = { c[1], kit.rgba(c[2], c[3]) }
end

local function push_theme()
    for _, c in ipairs(THEME) do
        imgui.PushStyleColor(c[1], c[2])
    end
    local VAR = kit.VAR
    imgui.PushStyleVar(VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(VAR.WindowRounding, S(6))
    imgui.PushStyleVar(VAR.WindowBorderSize, 0)
    imgui.PushStyleVar(5, { S(600), S(360) }) -- WindowMinSize
    imgui.PushStyleVar(VAR.FramePadding, { S(10), S(8) })
    imgui.PushStyleVar(VAR.FrameRounding, S(5))
    imgui.PushStyleVar(VAR.ItemSpacing, { S(8), S(4) })
    imgui.PushStyleVar(VAR.ScrollbarSize, S(10))
    imgui.PushStyleVar(VAR.ScrollbarRounding, S(5))
end

local function pop_theme()
    imgui.PopStyleVar(9)
    imgui.PopStyleColor(#THEME)
end

-- NoTitleBar, NoResize, NoScrollbar, NoScrollWithMouse, NoCollapse, NoBackground: the frame, the top line and the resize corner
-- are drawn here (ImGui's own grip would be under the frame, unseen)
local WINDOW = 1 + 2 + 8 + 16 + 32 + 128 + kit.NO_NAV
local PADDED = 2              -- ImGuiChildFlags_AlwaysUseWindowPadding
local TITLE_H = 36            -- the top line: the icon, the name, the tabs, the scale and the close (as /vana's)
local PAD = 18
local SEARCH_H = 30
local GRIP = 18               -- the resize corner, bottom right; the page stops above it
local HEAD_ROW, ITEM_ROW = 28, 23 -- in the list: a table's (a group's) row, an entry's (a snippet's)
local TABS, TAB_IDS = { 'Reference', 'Snippets' }, { 'reference', 'snippets' }
local tab = 'reference'       -- the page up: 'reference' or 'snippets'
-- dragging: the list's edge being dragged; grab: the resize corner held (the mouse's distance from the window's corner);
-- size: the window's this frame; next_size: one to take at the next frame (resized, rescaled); rescale: the scale changed
local ui = { dragging = false, grab = nil, size = nil, next_size = nil, rescale = false }

local function set_tab(name)
    if tab ~= name then
        tab = name
        config.data.tab = name
        pcall(config.save, config)
    end
end

vana.on('load', function ()
    open[1] = config.data.shown ~= false
    config.data.groups = type(config.data.groups) == 'table' and config.data.groups or {}   -- the groups open in the list
    config.data.spaces = type(config.data.spaces) == 'table' and config.data.spaces or {}   -- the tables open in the reference
    pick(BY_NAME[config.data.call] or CALLS[1])
    config.data.groups[current.group] = true
    tab = config.data.tab == 'snippets' and 'snippets' or 'reference'
    print('API Explorer: /api shows or hides the window.')
end)

-- /api (or /apiexplorer) shows or hides the window; /api show and /api hide only show or only hide it (/vana > Developer's
-- API Explorer button shows it)
vana.on('command', function (e)
    local word = e.command:match('^/api%s*(%a*)%s*$') or e.command:match('^/apiexplorer%s*(%a*)%s*$')
    if word == nil or (word ~= '' and word ~= 'show' and word ~= 'hide') then
        return
    end
    if word == '' then
        open[1] = not open[1]
    else
        open[1] = word == 'show'
    end
    config.data.shown = open[1]
    pcall(config.save, config)
    return true
end)

local filter = { '' }

-------------------------------------------------------------------------------
-- Pieces of the page (ImGui's own layout, so text of any length wraps), in /vana's fonts and colours
-------------------------------------------------------------------------------

-- Text at the cursor, wrapped at the page's edge
local function para(str, colour, font, size)
    kit.push(font or fonts.text, size or BODY)
    imgui.PushStyleColor(kit.COL.Text, RGBA[colour or P.text])
    imgui.PushTextWrapPos(0)
    imgui.TextUnformatted(str)
    imgui.PopTextWrapPos()
    imgui.PopStyleColor(1)
    kit.pop()
end

local function gap(n)
    imgui.Dummy({ 1, S(n) })
end

-- A hairline across the page between two entries
local function rule()
    local x, y = imgui.GetCursorScreenPos()
    local w = imgui.GetContentRegionAvail()
    kit.hline(imgui.GetWindowDrawList(), x, x + w, y + S(8), P.line)
    imgui.Dummy({ math.max(1, w), S(16) })
end

-- A part's heading, as /vana's sub-panels': semibold italic, spaced, in teal (capitals unless it names code), a line after it
local function section_label(title)
    local x, y = imgui.GetCursorScreenPos()
    local w = imgui.GetContentRegionAvail()
    local h = S(30)
    local cy = y + h / 2
    local dl = imgui.GetWindowDrawList()
    local shown = title:find('[%.%(_:]') and title or title:upper()
    local tw = kit.text_spaced(dl, x, cy, shown, P.teal, fonts.semi_italic or fonts.semi, SMALL, 1, S(1.2))
    if tw + S(10) < w then
        kit.hline(dl, x + tw + S(10), x + w, cy, P.teal, 0.8)
    end
    imgui.Dummy({ math.max(1, w), h })
end

-- Items along a line at the cursor, wrapping when the next doesn't fit; flow_end puts the cursor under them. Out of the view
-- (a long table's entries far above or below) they're only measured.
local function flow_begin(row_h)
    local x, y = imgui.GetCursorScreenPos()
    local w = imgui.GetContentRegionAvail()
    local _, wy = imgui.GetWindowPos()
    local wh = imgui.GetWindowHeight()
    return { x0 = x, y0 = y, x = x, y = y, right = x + w, h = S(row_h or 26), dl = imgui.GetWindowDrawList(),
             draw = y < wy + wh and y > wy - S(300) }
end

local function flow_place(f, w)
    if f.x > f.x0 and f.x + w > f.right then
        f.x, f.y = f.x0, f.y + f.h
    end
    local x = f.x
    f.x = f.x + w + S(10)
    return x, f.y + f.h / 2
end

local function flow_end(f)
    imgui.SetCursorScreenPos({ f.x0, f.y0 })
    imgui.Dummy({ math.max(1, f.right - f.x0), f.y - f.y0 + f.h })
end

local function flow_words(f, str, colour)
    local w = kit.measure(str, fonts.text, SMALL + 0.5)
    local x, cy = flow_place(f, w)
    if f.draw then
        kit.text_mid(f.dl, x, cy, str, colour or P.dim, fonts.text, SMALL + 0.5)
    end
end

-- a link: in the accent, lighter and underlined under the mouse. Returns clicked.
local function flow_link(f, id, str, tip)
    local w = kit.measure(str, fonts.text, SMALL + 0.5)
    local x, cy = flow_place(f, w)
    if not f.draw then
        return false
    end
    local hovered, clicked = kit.hit(id, x, cy - S(10), w, S(20))
    local colour = hovered and P.accent2 or P.accent
    kit.text_mid(f.dl, x, cy, str, colour, fonts.text, SMALL + 0.5)
    if hovered then
        kit.hline(f.dl, x, x + w, cy + S(8), colour, 0.8)
        imgui.SetMouseCursor(7) -- ImGuiMouseCursor_Hand
        if tip ~= nil then
            kit.tooltip(tip)
        end
    end
    return clicked
end

-- one of /vana's buttons (kit.button: 'accent', 'soft', 'danger', or nil for a ghost). Returns clicked.
local function flow_button(f, id, label, icon, kind, tip)
    local x, cy = flow_place(f, kit.button_width(label, icon))
    if not f.draw then
        return false
    end
    local clicked = kit.button(id, x, cy, label, icon, kind)
    if tip ~= nil and imgui.IsItemHovered() then
        kit.tooltip(tip)
    end
    return clicked
end

-------------------------------------------------------------------------------
-- The list down the left: a filter box as /vana's search, then rows drawn here as /vana's pages and sub-tabs (those out of view
-- left out: the reference has hundreds)
-------------------------------------------------------------------------------

local function list_begin(id, w)
    local x, y = imgui.GetCursorScreenPos()
    local _, wy = imgui.GetWindowPos()
    return { id = id, x = x, y = y, y0 = y, w = w, top = wy, bottom = wy + imgui.GetWindowHeight(),
             dl = imgui.GetWindowDrawList() }
end

-- a table's (a group's) row: its chevron, its name and how many are under it; lit while it's the page shown. Returns clicked.
local function list_head(l, key, label, count, expanded, lit, colour)
    local h = S(HEAD_ROW)
    local y = l.y
    l.y = y + h
    if y + h < l.top or y > l.bottom then
        return false
    end
    local dl, x, cy = l.dl, l.x, y + h / 2
    local hovered, clicked = kit.hit(l.id .. key, x, y, l.w, h)
    local t = kit.ease(l.id .. key, lit and 1 or 0, 20)
    if t > 0.01 then
        kit.fill(dl, x, y, l.w, h, P.accent_bg, 0, 0, t * 0.9)
        kit.fill(dl, x, y, S(2), h, P.accent, 0, 0, t)
    end
    if hovered and not lit then
        kit.fill(dl, x, y, l.w, h, P.hover, 0, 0, 0.7)
    end
    kit.icon_centred(dl, x + S(14), cy, expanded and 'down' or 'right', hovered and P.text or P.faint, 7)
    local right = x + l.w - S(10)
    local n = tostring(count)
    local nw = kit.measure(n, fonts.text, SMALL) + S(8)
    kit.text_right_mid(dl, right, cy, n, P.faint, fonts.text, SMALL)
    local room = right - nw - (x + S(26))
    kit.text_mid(dl, x + S(26), cy, kit.fit(label, room, fonts.text, BODY + 0.5), colour or P.green, fonts.text, BODY + 0.5)
    return clicked
end

-- an entry's (a snippet's) row, on a line down from its table's chevron: the one picked cyan, with a bar on the line. Returns
-- clicked.
local function list_item(l, key, label, on, colour)
    local h = S(ITEM_ROW)
    local y = l.y
    l.y = y + h
    if y + h < l.top or y > l.bottom then
        return false
    end
    local dl, x, cy = l.dl, l.x, y + h / 2
    local gx = x + S(14)
    local hovered, clicked = kit.hit(l.id .. key, x, y, l.w, h)
    if hovered and not on then
        kit.fill(dl, x, y, l.w, h, P.hover, 0, 0, 0.7)
    end
    kit.vline(dl, gx, y, y + h, P.line)
    if on then
        kit.fill(dl, gx - S(1), y + S(4), S(3), h - S(8), P.cyan, S(1.5))
    end
    local c = on and P.cyan or (hovered and P.white) or colour or P.text2
    kit.text_mid(dl, x + S(26), cy, kit.fit(label, l.w - S(34), fonts.text, BODY - 0.5), c, fonts.text, BODY - 0.5)
    return clicked
end

local function list_end(l)
    imgui.SetCursorScreenPos({ l.x, l.y0 })
    imgui.Dummy({ math.max(1, l.w), math.max(1, l.y - l.y0 + S(8)) })
end

-- The filter at the list's top, as /vana's search: a dark well with a lighter edge, ImGui's field in it, see-through, and a red
-- x that clears it
local CLEAR = kit.rgba(P.well, 0)
local function search_box(id, buffer, hint, x, y, w)
    local dl = imgui.GetWindowDrawList()
    local h = S(SEARCH_H)
    kit.fill(dl, x + S(1), y + S(4), w, h, '#000000', S(6), 0, 0.18)
    kit.fill(dl, x + S(1), y + S(2), w, h, '#000000', S(6), 0, 0.3)
    kit.fill(dl, x, y, w, h, P.search, S(6))
    kit.outline(dl, x, y, w, h, P.search_edge, S(6))
    kit.icon_centred(dl, x + S(14), y + h / 2, 'search', P.faint, 9)
    local typed = buffer[1] ~= ''
    imgui.SetCursorScreenPos({ x + S(24), y + 1 })
    imgui.SetNextItemWidth(w - S(24) - (typed and S(28) or S(4)))
    imgui.PushStyleColor(kit.COL.FrameBg, CLEAR)
    imgui.PushStyleColor(kit.COL.FrameBgHovered, CLEAR)
    imgui.PushStyleColor(kit.COL.FrameBgActive, CLEAR)
    imgui.PushStyleVar(kit.VAR.FramePadding, { S(6), math.max(0, (h - 2 - S(SMALL + 1)) / 2) })
    imgui.PushStyleVar(kit.VAR.FrameBorderSize, 0)
    kit.push(fonts.text, SMALL + 1)
    imgui.InputTextWithHint(id, hint, buffer, 64)
    kit.pop()
    imgui.PopStyleVar(2)
    imgui.PopStyleColor(3)
    if typed and kit.icon_button(id .. '.clear', x + w - S(15), y + h / 2, 'close', 12, '#FFA7AE', P.rose) then
        buffer[1] = ''
    end
end

-- The snippets by group: click a group to open or close it. A filter shows every snippet whose name has its text, groups open.
local function draw_snippet_list(w)
    local l = list_begin('##calls.', w)
    local text = filter[1]:lower()
    local groups = config.data.groups
    for _, group in ipairs(GROUPS) do
        local count = 0
        for _, call in ipairs(group.calls) do
            if text == '' or call.name:lower():find(text, 1, true) then count = count + 1 end
        end
        if count > 0 then
            local expanded = text ~= '' or groups[group.name] == true
            local lit = current ~= nil and current.group == group.name and not expanded
            if list_head(l, 'g.' .. group.name, group.name, count, expanded, lit) and text == '' then
                groups[group.name] = not expanded or nil
                pcall(config.save, config)
            end
            if expanded then
                for _, call in ipairs(group.calls) do
                    if (text == '' or call.name:lower():find(text, 1, true))
                        and list_item(l, 'c.' .. call.name, call.name, call == current) then
                        pick(call)
                    end
                end
            end
        end
    end
    list_end(l)
end

-------------------------------------------------------------------------------
-- The Reference tab
-------------------------------------------------------------------------------

-- Events a Listen would make costly (the d3d_* draws come thousands a frame) or that never come while it waits
local NO_LISTEN = { load = true, unload = true, d3d_dp = true, d3d_dip = true, d3d_dpup = true, d3d_dipup = true }

local reference = nil         -- build_reference()'s, made the first time the window is drawn
local ref_space = nil         -- the table shown
local ref_entry = nil         -- the entry picked
local scroll_to = nil         -- an entry to bring into view on the page
local ref_filter = { '' }

local function entry_key(e)
    return e.ns .. '|' .. e.sig
end

local function get_reference()
    if reference == nil then
        reference = build_reference()
        ref_space = reference.by_name[config.data.space or ''] or reference.spaces[1]
        for _, e in ipairs(reference.entries) do
            if entry_key(e) == config.data.entry then
                ref_entry, ref_space, scroll_to = e, e.space, e
            end
        end
    end
    return reference
end

local function matches(e, text)
    return text == '' or e.search:find(text, 1, true) ~= nil
end

local function pick_entry(e)
    ref_entry, ref_space, scroll_to = e, e.space, e
    config.data.space, config.data.entry = e.space.name, entry_key(e)
    config.data.spaces[e.space.name] = true
    pcall(config.save, config)
end

-- An entry from a snippet's Uses: the Reference tab, on it
local function open_entry(e)
    if not matches(e, ref_filter[1]:lower()) then ref_filter[1] = '' end
    pick_entry(e)
    set_tab('reference')
end

-- A snippet from an entry's links: the Snippets tab, on it
local function open_call(call)
    pick(call)
    if config.data.groups[call.group] == nil and BY_NAME[call.name] == call then
        config.data.groups[call.group] = true
        pcall(config.save, config)
    end
    set_tab('snippets')
end

-- An entry's fact (its `more`) as shown: an event's `e` is the table its handler is given
local function fact(m)
    if m[1] == 'e' then
        return 'Handler gets', 'the table e, with ' .. m[2]
    end
    return m[1], m[2]
end

-- A description as a sentence: its first letter a capital when it starts with a plain word ("a //something line ..."), not a
-- name from the code ("index, id, ...", "true when ...")
local PLAIN_START = { the = true, a = true, an = true, runs = true, opens = true, each = true, every = true, ['for'] = true,
                      whether = true, that = true, repeats = true, plays = true, moves = true, frees = true, brings = true,
                      no = true, up = true }
local function sentence(text)
    local word = text:match('^(%l+)[%s,]')
    if word ~= nil and PLAIN_START[word] then
        return text:sub(1, 1):upper() .. text:sub(2)
    end
    return text
end

-- Long text as comment lines of about `width` letters
local function comment_lines(text, width, out)
    local line = '--'
    for word in text:gmatch('%S+') do
        if #line + 1 + #word > width and line ~= '--' then
            out[#out + 1] = line
            line = '--'
        end
        line = line .. ' ' .. word
    end
    if line ~= '--' then out[#out + 1] = line end
end

-- Try it: a call to the entry's function in the code box, its signature and description above it; Listen: the event
-- listened for. Nothing runs until Run.
local function try_entry(e)
    local lines = {}
    if e.event then
        comment_lines(e.sig .. (e.desc ~= '' and (': ' .. e.desc) or ''), 116, lines)
        for _, m in ipairs(e.more or {}) do
            local label, words = fact(m)
            comment_lines(label .. ': ' .. words, 116, lines)
        end
        lines[#lines + 1] = '-- Run listens for it: the output shows how many came and what the last one handed its handler, until'
        lines[#lines + 1] = '-- Stop listening or another Run. In your addon:'
        lines[#lines + 1] = ("--   vana.on('%s', function (e) ... end)"):format(e.event)
        lines[#lines + 1] = ("return apiexplorer_listen('%s')"):format(e.event)
    else
        comment_lines(e.sig, 116, lines)
        if e.desc ~= '' then comment_lines(e.desc, 116, lines) end
        if e.call:match('%((.*)%)$') ~= '' then
            lines[#lines + 1] = '-- Fill in the arguments, then Run (as they are, each is nil).'
        end
        lines[#lines + 1] = 'return ' .. e.call
    end
    auto[1] = false
    open_call({ name = e.event and ('Listen for ' .. e.event) or ('Try ' .. e.call), group = 'Reference',
                code = table.concat(lines, '\n'), uses = { e } })
end

-- A fact under an entry's description: its label in a column, dim, its words after it
local FACT_W = 118
local function draw_fact(label, words)
    local x, y = imgui.GetCursorScreenPos()
    local _, line_h = kit.measure('Ag', fonts.text, BODY)
    kit.text_mid(imgui.GetWindowDrawList(), x, y + line_h / 2, label, P.dim, fonts.semi, SMALL + 0.5)
    imgui.SetCursorScreenPos({ x + S(FACT_W), y })
    para(words, P.text, fonts.text, BODY)
end

-- An entry: what it takes and returns (the code's font: cyan when picked, rose when this VanaCore hasn't it, purple when the
-- docs haven't it), what it does, its facts, its value now, then Try it (Listen for an event) and the snippets that use it
local function draw_entry(e, x0)
    imgui.PushID('entry' .. e.id)
    if scroll_to == e then
        imgui.SetScrollHereY(0.1)
        scroll_to = nil
    end
    local _, y0 = imgui.GetCursorScreenPos()
    local picked = e == ref_entry
    para(e.sig, picked and P.cyan or e.missing and P.rose or e.extra and P.experimental or P.accent2, fonts.icons, BODY)
    if e.desc ~= '' then
        para(sentence(e.desc), P.text, fonts.text, BODY + 0.5)
    end
    for _, m in ipairs(e.more or {}) do
        draw_fact(fact(m))
    end
    if e.missing then
        para('Not in this VanaCore: the docs name it, the game\'s vana table hasn\'t it (an older VanaCore?).', P.rose,
             fonts.text, SMALL + 0.5)
    end
    for _, now in ipairs(e.now or {}) do
        para('Now: ' .. now, P.dim, fonts.icons, SMALL + 1)
    end

    local can_try = (e.call ~= nil and not e.missing) or (e.event ~= nil and not NO_LISTEN[e.event])
    if can_try or #e.examples > 0 then
        gap(2)
        local f = flow_begin(28)
        if can_try then
            local tip = e.event and 'Puts code that listens for this event in the Snippets tab: Run starts it.'
                or 'Puts a call to it in the Snippets tab\'s code box: fill in the arguments, then Run.'
            if flow_button(f, '##try' .. e.id, e.event and 'Listen' or 'Try it', e.event and 'eye' or 'play', 'soft', tip) then
                try_entry(e)
            end
        end
        if #e.examples > 0 then
            flow_words(f, 'Snippets:')
            for i, call in ipairs(e.examples) do
                if flow_link(f, '##example' .. e.id .. '.' .. i, call.name) then open_call(call) end
            end
        end
        flow_end(f)
    end
    if picked then
        local _, y1 = imgui.GetCursorScreenPos()
        kit.fill(imgui.GetWindowDrawList(), x0 - S(12), y0 + S(2), S(3), math.max(S(4), y1 - y0 - S(8)), P.cyan, S(1.5))
    end
    imgui.PopID()
end

-- The tables down the left: each one with how many entries it has (click one to show it; again to close it), its entries
-- under it. A filter (names and descriptions) shows every match, tables open.
local function draw_ref_list(w)
    local ref = get_reference()
    local l = list_begin('##ref.', w)
    local text = ref_filter[1]:lower()
    local open_spaces = config.data.spaces
    for _, s in ipairs(ref.spaces) do
        local count = 0
        for _, e in ipairs(s.entries) do
            if matches(e, text) then count = count + 1 end
        end
        if count > 0 then
            local expanded = text ~= '' or open_spaces[s.name] == true
            local lit = s == ref_space and (ref_entry == nil or ref_entry.space ~= s)
            if list_head(l, 's.' .. s.name, s.title or s.name, count, expanded, lit, s.absent and P.rose or nil) then
                if text == '' then
                    open_spaces[s.name] = not (expanded and s == ref_space) or nil
                end
                ref_space, ref_entry = s, nil
                config.data.space, config.data.entry = s.name, nil
                pcall(config.save, config)
            end
            if expanded then
                for _, e in ipairs(s.entries) do
                    local colour = e.missing and P.rose or e.extra and P.experimental or nil
                    if matches(e, text) and list_item(l, 'e.' .. e.id, e.label, e == ref_entry, colour) then
                        pick_entry(e)
                    end
                end
            end
        end
    end
    list_end(l)
end

-- The page: the table picked, its notes and every entry in it; with a filter, every entry that matches, under its table's name
local function draw_ref_page()
    local ref = get_reference()
    local text = ref_filter[1]:lower()
    local x0 = imgui.GetCursorScreenPos()
    if text ~= '' then
        local any = false
        for _, s in ipairs(ref.spaces) do
            local first = true
            for _, e in ipairs(s.entries) do
                if matches(e, text) then
                    if first then
                        if any then gap(10) end
                        section_label(s.title or s.name)
                        first, any = false, true
                    else
                        rule()
                    end
                    draw_entry(e, x0)
                end
            end
        end
        if not any then
            para('Nothing matches "' .. ref_filter[1] .. '".', P.dim)
        end
    elseif ref_space ~= nil then
        local s = ref_space
        para(s.title or s.name, P.white, fonts.bold_italic or fonts.semi, BODY + 5)
        para(('%d entries, from docs/addons/%s'):format(#s.entries, s.page), P.dim, fonts.text, SMALL + 0.5)
        if s.absent then
            para(('This VanaCore has no %s: it\'s newer than the game\'s VanaCore.'):format(s.name), P.rose)
        end
        if #s.notes > 0 then
            gap(4)
            for _, note in ipairs(s.notes) do para(note, P.text2) end
        end
        gap(4)
        section_label('Entries')
        for i, e in ipairs(s.entries) do
            if i > 1 then rule() end
            draw_entry(e, x0)
        end
        gap(12)
    else
        para('Pick a table.', P.dim)
    end
end

-------------------------------------------------------------------------------
-- The Snippets tab's page: the code, its buttons and the output
-------------------------------------------------------------------------------

-- Lua's colours in the code and output boxes (Tokyo Night's, whose blue is /vana's accent)
local SYNTAX = {
    keyword = kit.u32('#BB9AF7'), string = kit.u32(P.green), number = kit.u32('#FF9E64'), comment = kit.u32('#6B7194'),
    call = kit.u32(P.accent), builtin = kit.u32('#7DCFFF'), name = kit.u32('#C0CAF5'), punct = kit.u32('#9AA5CE'),
}
local function word_set(words)
    local t = {}
    for w in words:gmatch('%S+') do t[w] = true end
    return t
end
local KEYWORDS = word_set('and break do else elseif end for function goto if in local not or repeat return then until while')
local CONSTANTS = word_set('true false nil')
local BUILTINS = word_set('vana ffi bit string table math os io coroutine debug ipairs pairs next print tostring tonumber type '
    .. 'select unpack pcall xpcall error assert setmetatable getmetatable rawget rawset require self')
local CODE_SIZE = SMALL - 0.5   -- the boxes' text
local HIGHLIGHT_MAX = 40000     -- longer text (a big output) is shown plain: colouring it at each change would stall a frame
local CALLBACK_ALWAYS = 1048576 -- ImGuiInputTextFlags_CallbackAlways (1 << 20): the cursor's place, while it's typed in
local SCROLL_X = 2048           -- ImGuiWindowFlags_HorizontalScrollbar

-- Lua text as coloured runs, line by line: { { { text, colour }, ... }, ... }. Long strings and block comments run over lines.
local function lua_runs(text)
    local lines, line = {}, {}
    local function add(str, colour)
        local at = 1
        while true do
            local nl = str:find('\n', at, true)
            local piece = str:sub(at, nl and nl - 1 or -1)
            if piece ~= '' then line[#line + 1] = { piece, colour } end
            if nl == nil then return end
            lines[#lines + 1] = line
            line = {}
            at = nl + 1
        end
    end
    local i, n = 1, #text
    while i <= n do
        local b = text:byte(i)
        local nb = text:byte(i + 1) or 0
        local j
        if b == 45 and nb == 45 then -- a comment: to the line's end, or a --[[ ]] block
            local eq = text:match('^%[(=*)%[', i + 2)
            if eq ~= nil then
                local close = text:find(']' .. eq .. ']', i + 4 + #eq, true)
                j = close and close + #eq + 1 or n
            else
                j = (text:find('\n', i, true) or n + 1) - 1
            end
            add(text:sub(i, j), SYNTAX.comment)
        elseif b == 34 or b == 39 then -- a quoted string, to its quote or the line's end
            j = i + 1
            while j <= n do
                local d = text:byte(j)
                if d == 92 then
                    j = j + 2
                elseif d == b or d == 10 then
                    break
                else
                    j = j + 1
                end
            end
            j = math.min(j, n)
            add(text:sub(i, j), SYNTAX.string)
        elseif b == 91 and text:match('^%[=*%[', i) then -- a long string: [[ ]], [=[ ]=]
            local eq = text:match('^%[(=*)%[', i)
            local close = text:find(']' .. eq .. ']', i + 2 + #eq, true)
            j = close and close + #eq + 1 or n
            add(text:sub(i, j), SYNTAX.string)
        elseif (b >= 48 and b <= 57) or (b == 46 and nb >= 48 and nb <= 57) then -- a number
            j = (text:match('^0[xX]%x*()', i) or text:match('^%d*%.?%d*[eE][%+%-]?%d+()', i) or text:match('^%d*%.?%d*()', i)) - 1
            add(text:sub(i, j), SYNTAX.number)
        elseif (b >= 65 and b <= 90) or (b >= 97 and b <= 122) or b == 95 then -- a name: a keyword, a call, Lua's own
            j = text:match('^[%w_]*()', i + 1) - 1
            local word = text:sub(i, j)
            local before = text:byte(i - 1)
            local colour = SYNTAX.name
            if KEYWORDS[word] then
                colour = SYNTAX.keyword
            elseif CONSTANTS[word] then
                colour = SYNTAX.number
            elseif text:match('^[ \t]*[%(\'"{]', j + 1) then
                colour = SYNTAX.call
            elseif BUILTINS[word] and before ~= 46 and before ~= 58 then
                colour = SYNTAX.builtin
            end
            add(word, colour)
        elseif b == 32 or b == 9 or b == 10 or b == 13 then
            j = text:match('^%s*()', i) - 1
            add(text:sub(i, j), SYNTAX.name)
        else
            j = i
            add(text:sub(i, i), SYNTAX.punct)
        end
        i = j + 1
    end
    lines[#lines + 1] = line
    return lines
end

-- A box's text laid out for its colours (with the box's font pushed): each run's x on its line, the widest line. Made again
-- when the text or the size changes; nil lines when it's too long to colour.
local function lay_out(box, text, px)
    if box.text == text and box.px == px then
        return box
    end
    box.text, box.px, box.lines, box.w = text, px, nil, 0
    if #text > HIGHLIGHT_MAX then
        return box
    end
    local lines = lua_runs(text)
    local widest = 0
    for _, line in ipairs(lines) do
        local x = 0
        for _, run in ipairs(line) do
            run[3] = x
            run[4] = run[1]:find('%S') ~= nil -- (spaces aren't drawn)
            x = x + imgui.CalcTextSize(run[1])
        end
        widest = math.max(widest, x)
    end
    box.lines, box.w = lines, widest
    return box
end

-- A code box: the darkest well, an edge, ImGui's own font (monospaced), Lua in colour. ImGui's box is as big as its text, in a
-- child that scrolls, with its own letters unseen: the coloured ones are drawn under its cursor and selection. While it's typed
-- in, the view follows the cursor. Text too long to colour is ImGui's box as it is.
local BOXES = {} -- each box's: its text laid out, the cursor's place (caret) and the last one followed (seen)
local function code_box(id, buffer, size, w, h, flags)
    local box = BOXES[id]
    if box == nil then
        box = {}
        box.callback = function (data)
            box.caret = data.CursorPos
            return 0
        end
        BOXES[id] = box
    end
    local x, y = imgui.GetCursorScreenPos()
    local dl = imgui.GetWindowDrawList()
    kit.fill(dl, x, y, w, h, INK.box, S(5))
    kit.outline(dl, x, y, w, h, INK.box_edge, S(5))
    kit.push(fonts.icons, CODE_SIZE)
    local lay = lay_out(box, buffer[1], S(CODE_SIZE))
    if lay.lines == nil then
        imgui.PushStyleColor(kit.COL.FrameBg, CLEAR)
        imgui.InputTextMultiline(id, buffer, size, { w, h }, flags)
        imgui.PopStyleColor(1)
        kit.pop()
        return
    end
    local _, lh = imgui.CalcTextSize('X')
    local px, py = S(10), S(8) -- the theme's FramePadding
    if imgui.BeginChild(id .. '.view', { w, h }, 0, kit.NO_NAV + SCROLL_X) then
        local cw, ch = imgui.GetContentRegionAvail()
        local iw = math.floor(math.max(cw, lay.w + px * 2 + S(24)))
        local ih = math.floor(math.max(ch, #lay.lines * lh + py * 2 + lh))
        imgui.PushStyleColor(kit.COL.Text, CLEAR)
        imgui.PushStyleColor(kit.COL.FrameBg, CLEAR)
        imgui.PushStyleColor(kit.COL.FrameBgHovered, CLEAR)
        imgui.PushStyleColor(kit.COL.FrameBgActive, CLEAR)
        box.caret = nil
        imgui.InputTextMultiline(id, buffer, size, { iw, ih }, flags + CALLBACK_ALWAYS, box.callback)
        imgui.PopStyleColor(4)

        -- the lines in view, in colour (laid out again when this frame's typing changed the text)
        local ix, iy = imgui.GetItemRectMin()
        local cdl = imgui.GetWindowDrawList()
        local _, wy = imgui.GetWindowPos()
        local top = iy + py
        lay = lay_out(box, buffer[1], S(CODE_SIZE))
        local last = math.min(#(lay.lines or {}), math.floor((wy + imgui.GetWindowHeight() - top) / lh) + 1)
        for i = math.max(1, math.floor((wy - top) / lh) + 1), last do
            local ly = top + (i - 1) * lh
            for _, run in ipairs(lay.lines[i]) do
                if run[4] then
                    cdl:AddText({ ix + px + run[3], ly }, run[2], run[1])
                end
            end
        end

        -- the cursor moved while typing: its line and column brought into view
        if box.caret ~= nil and box.caret ~= box.seen then
            box.seen = box.caret
            local before = buffer[1]:sub(1, box.caret)
            local line = select(2, before:gsub('\n', '')) + 1
            local cx = px + imgui.CalcTextSize(before:match('[^\n]*$'))
            local cy = py + (line - 1) * lh
            local sx, sy = imgui.GetScrollX(), imgui.GetScrollY()
            if cy < sy then
                imgui.SetScrollY(math.max(0, cy - py))
            elseif cy + lh + py > sy + ch then
                imgui.SetScrollY(cy + lh + py - ch)
            end
            if cx < sx + px then
                imgui.SetScrollX(math.max(0, cx - px * 2))
            elseif cx + px > sx + cw then
                imgui.SetScrollX(cx + px - cw)
            end
        elseif box.caret == nil then
            box.seen = nil
        end
    end
    imgui.EndChild()
    kit.pop()
end

local function draw_work()
    if current == nil then
        para('Pick a snippet.', P.dim)
        return
    end
    section_label(current.group)
    para(current.name, P.white, fonts.bold_italic or fonts.semi, BODY + 5)
    if current.uses and #current.uses > 0 then -- the reference's entries for what the code uses
        local f = flow_begin(26)
        flow_words(f, 'Uses:')
        for i, e in ipairs(current.uses) do
            local name = e.find and e.find[1] and not e.find[1]:find('(', 1, true) and e.find[1] or e.label
            if flow_link(f, '##uses' .. i, name, e.sig) then open_entry(e) end
        end
        flow_end(f)
    end
    gap(4)
    local width, room = imgui.GetContentRegionAvail()
    code_box('##code', code, 8192, width, math.max(S(120), math.floor(room * 0.4)), ALLOW_TAB)

    local f = flow_begin(36)
    if flow_button(f, '##run', 'Run', 'play', 'accent') then
        run()
    end
    if flow_button(f, '##copy_code', 'Copy code', 'copy') then
        vana.system.set_clipboard(code[1])
        status = 'code copied'
    end
    if flow_button(f, '##copy_output', 'Copy output', 'copy') then
        vana.system.set_clipboard(output[1])
        status = 'output copied'
    end
    if flow_button(f, '##clear', 'Clear', 'trash') then
        output[1] = ''
        status = ''
    end
    local x, cy = flow_place(f, kit.checkbox_width('Run every second'))
    local on, flipped = kit.checkbox('##auto', x, cy, 'Run every second', auto[1], true)
    if flipped then
        auto[1] = on
    end
    if listening ~= nil and flow_button(f, '##stop', 'Stop listening', 'stop', 'danger') then
        status = 'stopped listening for ' .. listening.name
        apiexplorer_listen(nil)
    end
    if status ~= '' then
        flow_words(f, status)
    end
    flow_end(f)

    local w, h = imgui.GetContentRegionAvail()
    code_box('##output', output, #output[1] + 1, w, math.max(S(80), h), READ_ONLY)
end

-------------------------------------------------------------------------------
-- The window: the top line, the list on the left (its edge dragged to widen it) and the page
-------------------------------------------------------------------------------

-- the list's right edge: dragged, the list is as wide as the mouse says ([apiexplorer] list_w, unscaled)
local function divider(dl, dx, y, h, wx, ww)
    local hovered, _, active = kit.hit('##divider', dx - S(3), y, S(6), h)
    if hovered or active then
        imgui.SetMouseCursor(4) -- ImGuiMouseCursor_ResizeEW
        kit.vline(dl, dx, y, y + h, active and P.accent or P.accent_lo)
    end
    if active then
        local mx = imgui.GetMousePos()
        config.data.list_w = math.floor(clamp(mx - wx, S(160), ww - S(320)) / kit.scale + 0.5)
        ui.dragging = true
    elseif ui.dragging then
        ui.dragging = false
        pcall(config.save, config)
    end
end

-- the list: its filter box on the window, its rows in a child of their own that scrolls
local function side(id, x, y, w, h, buffer, hint, rows)
    search_box('##' .. id .. '.filter', buffer, hint, x + S(12), y + S(12), w - S(24))
    local ly = y + S(12) + S(SEARCH_H) + S(10)
    imgui.SetCursorScreenPos({ x, ly })
    if imgui.BeginChild('##' .. id .. '.list', { w - S(3), y + h - ly - S(8) }, 0, kit.NO_NAV) then
        rows((imgui.GetContentRegionAvail()))
    end
    imgui.EndChild()
end

local function page(id, x, y, w, h, draw)
    imgui.SetCursorScreenPos({ x, y })
    imgui.PushStyleVar(kit.VAR.WindowPadding, { S(12), S(8) })
    -- (it stops above the resize corner: a child over it would take the corner's mouse)
    local shown = imgui.BeginChild('##' .. id .. '.page', { w, h - S(GRIP) }, PADDED, kit.NO_NAV)
    imgui.PopStyleVar(1)
    if shown then
        draw()
    end
    imgui.EndChild()
end

-- The resize corner, bottom right: three short lines across it, lit under the mouse; dragged, the window's corner follows the
-- mouse (taken at the next frame, no smaller than the window's least size)
local function resize_corner(dl, x, y, ww, wh)
    local g = S(GRIP)
    local hovered, _, active = kit.hit('##resize', x + ww - g, y + wh - g, g, g)
    local mx, my = imgui.GetMousePos()
    if active then
        ui.grab = ui.grab or { x + ww - mx, y + wh - my }
        ui.next_size = { math.floor(math.max(S(600), mx + ui.grab[1] - x)), math.floor(math.max(S(360), my + ui.grab[2] - y)) }
    else
        ui.grab = nil
    end
    if hovered or active then
        imgui.SetMouseCursor(6) -- ImGuiMouseCursor_ResizeNWSE
    end
    local colour = kit.u32((hovered or active) and P.accent or P.dim)
    local right, bottom = x + ww - S(5), y + wh - S(5)
    for _, k in ipairs({ 4, 8, 12 }) do
        dl:AddLine({ right - S(k), bottom }, { right, bottom - S(k) }, colour, math.max(1, S(1.25)))
    end
    if hovered and not active then
        kit.tooltip('Drag to resize')
    end
end

-- This window's scale in the top line, ending at `right`: a well with - and + at its ends and the percent between (click it for
-- 100%). The new scale and the window's size go in at the next frame.
local function scale_control(dl, right, cy)
    local w, h, bw = S(96), S(22), S(24)
    local x, y = right - w, cy - h / 2
    kit.fill(dl, x, y, w, h, P.well, S(4))
    kit.outline(dl, x, y, w, h, P.well_edge, S(4))
    local now = own_scale()
    local want = now
    local function side(sx, icon, by, corners, tip)
        local hovered, clicked = kit.hit('##scale.' .. icon, sx, y, bw, h)
        local can = (by < 0 and now > SCALE_MIN) or (by > 0 and now < SCALE_MAX)
        if hovered and can then
            kit.fill(dl, sx + 1, y + 1, bw - 2, h - 2, '#FFFFFF', S(3), corners, 0.07)
        end
        kit.icon_centred(dl, sx + bw / 2, cy, icon, (hovered and can) and P.text or (can and P.dim or P.faint), 8)
        if hovered then
            kit.tooltip(tip)
        end
        if clicked and can then
            want = now + by * SCALE_STEP
        end
    end
    side(x, 'minus', -1, kit.CORNERS.left, 'Smaller')
    side(x + w - bw, 'plus', 1, kit.CORNERS.right, 'Bigger')
    local hovered, clicked = kit.hit('##scale.reset', x + bw, y, w - bw * 2, h)
    kit.text_centred(dl, x + bw, y, w - bw * 2, h, now .. '%', hovered and P.text or P.text2, fonts.text, SMALL + 0.5)
    if hovered then
        kit.tooltip('This window\'s size (/vana\'s scale times this). Click for 100%.')
    end
    if clicked then
        want = 100
    end
    if want ~= now then
        config.data.scale = want
        pcall(config.save, config)
        ui.rescale = true
    end
    return w
end

local function draw_window()
    local x, y = imgui.GetWindowPos()
    local ww, wh = imgui.GetWindowWidth(), imgui.GetWindowHeight()
    ui.size = { ww, wh }
    local dl = imgui.GetWindowDrawList()
    local r = S(6)
    kit.fill(dl, x - 1, y, ww + 2, wh + 2, '#000000', r + 1, 0, 0.35)
    kit.fill(dl, x, y, ww, wh, P.bg, r)

    -- the top line: the switch for the list, the icon and name, the tabs, the scale and the close
    local th = S(TITLE_H)
    local cy = y + th / 2
    local lx = x + S(PAD)
    -- the list down the left hidden or shown again, as /vana's list of pages is ([apiexplorer] list): the page takes its room
    local listed = config.data.list ~= false
    local switched, over_switch = kit.icon_button('##list', lx + S(3), cy, 'bars', 10.5, P.text, P.dim)
    if switched then
        listed = not listed
        config.data.list = listed
        pcall(config.save, config)
    end
    if over_switch then
        kit.tooltip(listed and 'Hide the list' or 'Show the list')
    end
    lx = lx + S(24)
    kit.icon_centred(dl, lx + S(5), cy, 'code', P.accent, 11)
    kit.text_mid(dl, lx + S(19), cy, 'API Explorer', P.white, fonts.semi, BODY + 1)
    local tx = lx + S(19) + kit.measure('API Explorer', fonts.semi, BODY + 1) + S(18)
    local picked, moved = kit.segments('##tabs', tx, cy, TABS, tab == 'snippets' and 2 or 1, true)
    if moved then
        set_tab(TAB_IDS[picked])
    end
    local close_x = x + ww - S(PAD)
    if kit.icon_button('##close', close_x, cy, 'close', 11, P.rose) then
        open[1] = false
    end
    scale_control(dl, close_x - S(20), cy)
    kit.hline(dl, x, x + ww, y + th, P.line)

    -- the list (a little lighter) and the page (darker, for reading), the window's lower corners rounded. The list slides
    -- shut (and open) with the switch; its contents and its edge's drag are there once it's fully open
    local by, bh = y + th + 1, wh - th - 1
    local full = math.floor(clamp(S(config.data.list_w or 250), S(160), ww - S(320)))
    local lw = math.floor(full * kit.ease('list', listed and 1 or 0, 14) + 0.5)
    local px = x + 1
    if lw > 0 then
        kit.fill(dl, x, by, lw, bh, P.side, r, 64) -- (the bottom left corner)
        kit.fill(dl, x + lw, by, ww - lw, bh, INK.page, r, 128)
        kit.vline(dl, x + lw, by, y + wh, P.border)
        px = x + lw + S(3)
    else
        kit.fill(dl, x, by, ww, bh, INK.page, r, kit.CORNERS.bottom)
    end
    kit.outline(dl, x, y, ww, wh, P.border, r)
    local whole = lw >= full
    if whole then
        divider(dl, x + lw, by, bh, x, ww)
    end
    if tab == 'reference' then
        if whole then side('ref', x, by, lw, bh, ref_filter, 'Filter: a name or a word', draw_ref_list) end
        page('ref', px, by, x + ww - px, bh, draw_ref_page)
    else
        if whole then side('calls', x, by, lw, bh, filter, 'Filter by name', draw_snippet_list) end
        page('calls', px, by, x + ww - px, bh, draw_work)
    end
    resize_corner(dl, x, y, ww, wh)
end

vana.on('present', function ()
    show_listening()
    if not open[1] then return end
    local now = vana.time.clock()
    local st = kit.st
    st.dt = math.max(0, math.min(0.1, now - st.last))
    st.last = now
    local _, screen_h = vana.ui.screen()
    local was = kit.scale
    kit.compute_scale(now, (screen_h and screen_h > 0) and screen_h or 1080, user_percent(), ui.rescale)
    if ui.rescale then
        -- the scale changed in the top line: the window grows or shrinks with what's in it
        ui.rescale = false
        if ui.size ~= nil and was > 0 and kit.scale ~= was then
            local k = kit.scale / was
            ui.next_size = { math.floor(ui.size[1] * k + 0.5), math.floor(ui.size[2] * k + 0.5) }
        end
    end
    if not fonts.ready then
        fonts.ready = true
        pcall(kit.load_fonts)
        return
    end
    get_reference()   -- the snippets' Uses links need it too
    if ui.next_size ~= nil then
        imgui.SetNextWindowSize(ui.next_size, 0)
        ui.next_size = nil
    else
        imgui.SetNextWindowSize({ S(900), S(620) }, FIRST_USE_EVER)
    end
    push_theme()
    if imgui.Begin('API Explorer', nil, WINDOW) then
        kit.push(fonts.text, BODY)
        draw_window()
        kit.pop()
    end
    imgui.End()
    pop_theme()
    if not open[1] then
        config.data.shown = false
        pcall(config.save, config)
    end
    if auto[1] and waiting == nil and vana.time.clock() - last_run >= 1 then
        run()
    end
end)
