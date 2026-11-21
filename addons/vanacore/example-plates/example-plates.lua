--[[
example-plates: an example of vana.world widgets (docs/addons/18-widgets.md). Over the name of each party member and of the target it
stacks a tag (name and HP), an HP bar, and for the target a ">" marker beside the name. They are drawn in the game's 3D space,
so walls hide them as they hide a name, except the target's, which can stay visible through walls.

Every box is placed once, when its character first needs a plate. After that each frame only changes what changed:
box:set{...} for the text and the bar's width and colour, vana.world.update(box, {...}) for where it sits and whether walls hide
it. Nothing is placed twice.

In game:
  /addon load example-plates
  /plates names     hides or shows the game's own names (vana.gameui.hide('nameplates'))
  /plates walls     the target's plate through walls, or hidden by them
  /addon unload example-plates
--]]

addon.name    = 'example-plates'
addon.author  = 'Vekien'
addon.version = '1.0'
addon.desc    = 'Example: a tag, an HP bar and a target marker over names with vana.world, updated every frame.'

local BAR_W, BAR_H = 80, 8      -- the HP bar's size in pixels
local TAG_SIZE = 14             -- the tag's text size
local GAP = 3                   -- pixels between the name, the bar and the tag

local plates = {}               -- by target index: { back, fill, tag }
local marker = nil              -- the target's ">" (one box, moved from name to name with update)
local options = { hide_names = false, target_through_walls = true }

local function hp_colour(hpp)
    if hpp > 50 then return '#40C060' end
    if hpp > 25 then return '#E0C040' end
    return '#E04040'
end

-- A plate for the character at `index`: three boxes attached above its name, the bar's frame first (drawn first), then its
-- fill, then the tag over them. `y` lifts a box from the name's top: the bar sits just above the name, the tag above the bar.
local function make_plate(index)
    local plate = {
        back = vana.ui.rect({ width = BAR_W + 2, height = BAR_H + 2, color = '#000000C0', radius = 2 }),
        fill = vana.ui.rect({ width = BAR_W, height = BAR_H, color = hp_colour(100), radius = 2 }),
        tag  = vana.ui.text({ text = '', size = TAG_SIZE, color = '#FFFFFF', background = '#00000070', padding = 2 }),
    }
    vana.world.attach(plate.back, index, { side = 'above', y = -GAP })
    vana.world.attach(plate.fill, index, { side = 'above', y = -GAP - 1 })
    vana.world.attach(plate.tag,  index, { side = 'above', y = -GAP - BAR_H - 2 - GAP })
    return plate
end

local function destroy_plate(plate)
    plate.back:destroy()
    plate.fill:destroy()
    plate.tag:destroy()
end

-- What this frame changes on one plate
local function update_plate(plate, entity, through_walls)
    local hpp = entity.hpp
    local width = math.max(1, math.floor(BAR_W * hpp / 100))
    -- the bar's fill is as wide as the HP and starts at the frame's left edge: the boxes are centred on the name, so it is
    -- moved left by half of what it has lost
    plate.fill:set({ width = width, color = hp_colour(hpp) })
    vana.world.update(plate.fill, { x = -(BAR_W - width) / 2, walls = not through_walls })
    vana.world.update(plate.back, { walls = not through_walls })
    plate.tag:set({ text = ('%s  %d%%'):format(entity.name, hpp) })
    vana.world.update(plate.tag, { walls = not through_walls })
end

-- The characters that get a plate: the party and the target
local function wanted()
    local list = {}
    local party = vana.game.party()
    if party ~= nil then
        for slot = 1, 18 do
            local member = party.members[slot]
            if member.active and member.target_index ~= 0 then list[member.target_index] = true end
        end
    end
    local target = vana.game.target()
    local target_index = target ~= nil and target.active and target.index ~= 0 and target.index or nil
    if target_index ~= nil then list[target_index] = true end
    return list, target_index
end

vana.on('present', function ()
    local list, target_index = wanted()
    for index in pairs(list) do
        local entity = vana.game.entity(index)
        if entity == nil then
            list[index] = nil
        else
            plates[index] = plates[index] or make_plate(index)
            update_plate(plates[index], entity, options.target_through_walls and index == target_index)
        end
    end
    for index, plate in pairs(plates) do
        if list[index] == nil then
            destroy_plate(plate)
            plates[index] = nil
        end
    end
    -- the marker: made at the first target, then update moves it to the next one's name
    if target_index ~= nil and list[target_index] then
        if marker == nil then
            marker = vana.ui.text({ text = '>', size = TAG_SIZE + 4, color = '#FFE060', background = '#00000000' })
            vana.world.attach(marker, target_index, { side = 'left', x = -4 })
        end
        marker:show()
        vana.world.update(marker, { index = target_index, walls = not options.target_through_walls })
    elseif marker ~= nil then
        marker:hide()
    end
end)

vana.on('command', function (e)
    local word = e.command:match('^/plates%s*(%S*)')
    if word == nil then return end
    if word == 'names' then
        options.hide_names = not options.hide_names
        if options.hide_names then vana.gameui.hide('nameplates') else vana.gameui.show('nameplates') end
        print('plates: the game\'s names are ' .. (options.hide_names and 'hidden' or 'shown'))
    elseif word == 'walls' then
        options.target_through_walls = not options.target_through_walls
        print('plates: the target\'s plate is ' .. (options.target_through_walls and 'shown through walls' or 'hidden by walls'))
    else
        print('plates: /plates names | walls')
    end
    return true
end)
