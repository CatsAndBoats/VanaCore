# Hide the game's party list (and draw your own)

For addons with their own party list, like XIUI: the game's party list and the alliance lists stop being drawn, so
yours can take their place. The party itself doesn't change. The game still keeps it, targeting a member works as
before, and your addon reads the members as usual.

This example is an **Ashita** addon, because that's what XIUI is. The same calls work from VanaCore and Windower
addons.

## 1. The folder

An Ashita addon lives in `addons\ashita\`:

```
addons\ashita\mypartylist\
    mypartylist.lua
    README.md
```

## 2. `mypartylist.lua`

```lua
addon.name    = 'mypartylist'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'A party list of its own.'

require('common')

ashita.events.register('load', 'mypartylist_load', function ()
    -- VanaCore only: real Ashita has no `vana`, and there the game's lists simply stay
    if vana ~= nil and vana.gameui ~= nil then
        vana.gameui.hide('party_list')
        vana.gameui.hide('alliance_lists')
    end
end)

ashita.events.register('d3d_present', 'mypartylist_present', function ()
    local party = AshitaCore:GetMemoryManager():GetParty()
    for i = 0, 5 do
        if party:GetMemberIsActive(i) == 1 then
            -- draw member i your own way: party:GetMemberName(i), party:GetMemberHPPercent(i), ...
        end
    end
end)
```

The `if vana ~= nil and vana.gameui ~= nil` check is all it takes for one file to work in both VanaCore and Ashita.

## 3. Say what it changes

In your README: "On VanaCore, hides the game's party list and alliance lists: mypartylist draws its own in their
place."

## Good to know

- **Show the game's list again** with `vana.gameui.show('party_list')`, for example if your list has a hidden mode.
- **Two addons can hide the same list.** Hiding is shared: it stays hidden while any addon hides it, and comes back when
  the last one lets go.
- **The player's own setting** `[client] party.hide` (/hud > Party List > Hide) is separate. If it's on,
  the list stays hidden whatever your addon does.
- **The Target window** sits just above the game's party list. With the list hidden, you may want to move it too:
  see [move_window.md](move_window.md). `addons\vanacore\example-minparty` does both.
