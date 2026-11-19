# Take over a game menu item: Party > Find Member

Your addon decides what one of the game's menu items does. Here, choosing **Party > Find Member** in the main menu opens
your addon's own search instead of the game's.

This is what VanaCore does for the Party Finder addon today, through a setting made just for it
(`[client] partyfinder.findmember`). With `replace`, any addon can do it.

## 1. `myfinder.lua`

```lua
addon.name    = 'myfinder'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'A party search, opened from Party > Find Member.'

local imgui = vana.imgui
local open = { false }

vana.events.on('load', function ()
    vana.gameui.replace('party_menu.find_member', function ()
        open[1] = true -- the game's own search doesn't run
    end)
end)

vana.events.on('present', function ()
    if not open[1] then
        return
    end
    if imgui.Begin('Find Member', open) then
        imgui.Text('Your search here.')
    end
    imgui.End()
end)
```

## 2. Say what it changes

In your README: "Party > Find Member opens myfinder's search instead of the game's."

## What happens when the player chooses it

1. The game plays its select sound, as for any item.
2. The game's own search **doesn't** run.
3. Your function runs on the next frame.

Everything else in the Party menu stays the game's.

## Good to know

- **Give it back** with `vana.gameui.restore('party_menu.find_member')`, for example from a setting in your addon.
  Unloading does it too.
- **One addon at a time.** A second addon that asks gets `false, 'held by X'`, and takes over when the first lets go.
  Open your window some other way too, such as a command, so the addon is still usable then.
- **Other menu items:** each needs VanaCore to hook that menu first. Find Member is the only one in this mockup;
  Config buttons are [add_button.md](add_button.md).
