# Your own equipment screen

The main menu's Equipment item opens the addon's window instead of the game's. `vana.gameui.catch` hears the choice and
keeps it from the game; the window draws what `vana.game.equipment()` and `vana.game.container()` say is worn. It shows
a trigger by item name, a window that opens from a game event, and `off` on unload (not needed: VanaCore drops an
addon's triggers when it goes, but it shows the call).

## `myequip.lua`

```lua
addon.name    = 'myequip'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'Opens its own equipment window from the main menu\'s Equipment item.'

local imgui = vana.imgui
local open = { false }
local SLOTS = { 'Main', 'Sub', 'Range', 'Ammo', 'Head', 'Body', 'Hands', 'Legs', 'Feet', 'Neck', 'Waist',
                'L.Ear', 'R.Ear', 'L.Ring', 'R.Ring', 'Back' }
local trigger

vana.on('load', function ()
    -- the player picks Equipment (menuwind's item 2): our window, not the game's
    trigger = vana.gameui.catch('main_menu', 'select', 'equipment', function (e)
        open[1] = true
    end)
end)

vana.on('unload', function ()
    vana.gameui.off(trigger)
end)

vana.on('present', function ()
    if not open[1] then return end
    imgui.SetNextWindowSize({ 260, 0 }, 4)                 -- ImGuiCond_FirstUseEver
    if imgui.Begin('My equipment', open) then
        local eq = vana.game.equipment()
        for slot = 0, 15 do
            local where = eq and eq[slot]
            local item = where and where.index ~= 0 and vana.game.container(where.container)[where.index]
            imgui.Text(('%-7s %s'):format(SLOTS[slot + 1], item and ('item %d'):format(item.id) or '-'))
        end
        if imgui.Button('Close') then
            open[1] = false
        end
    end
    imgui.End()
end)
```

Item names aren't in `vana.game`: they're in the game's DAT files ([Game data](../04-game-data.md) says where the
Ashita layer reads them). To see every menu's items as the player uses them, while you work out ids for other menus:

```lua
vana.gameui.on('*', 'select', function (e)
    print(('%s: item %d%s'):format(e.menu, e.item, e.name ~= '' and ' (' .. e.name .. ')' or ''))
end)
```

## README.md

> Opens its own equipment window in place of the game's when Equipment is chosen in the main menu. The game's own
> Equipment window isn't opened while this addon is loaded; unload it to have it back.
