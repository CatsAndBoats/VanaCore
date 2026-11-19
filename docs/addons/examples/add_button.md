# Add a button to the game's Config menu

Your addon gets its own button in the game's Config list, under VanaCore's. The game's own menu code draws it, plays
its sounds and moves the cursor to it, the same as the game's own items. When the player chooses it, your function runs.

```
Config
  Gameplay
  ...
  Gamepad
  VanaCore
  HUD
  FPS Graph
  Game Stats
  Timers        <- yours
```

## 1. `timers.lua`

```lua
addon.name    = 'timers'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'A timers window, opened from Config.'

local imgui = vana.imgui
local open = { false } -- ImGui's Begin takes the open flag as a one-item table

vana.events.on('load', function ()
    vana.gameui.add('config_menu', 'Timers', function ()
        open[1] = not open[1] -- runs on the frame after the player chooses Config > Timers
    end)
end)

vana.events.on('present', function ()
    if not open[1] then
        return
    end
    if imgui.Begin('Timers', open) then
        imgui.Text('Your timers here.')
    end
    imgui.End()
end)
```

The second argument is the button's text, 1 to 10 letters. Any other length raises an error.

A fourth argument colours the button, as the game colours some of its own (Monstrosity is red):

```lua
vana.gameui.add('config_menu', 'Timers', toggle, 'green')
```

`'blue'` (the game's, the same as leaving it out), `'green'`, `'yellow'`, `'red'`, `'purple'` (VanaCore's), `'grey'`, or
your own as `'#RRGGBB'`. It tints a grey button, so a bright colour reads best.

## 2. Say what it changes

In your README: "Adds a Timers button to the game's Config menu."

## 3. Load it

Type `/addon load timers`. Open the main menu > Config, and **Timers** is at the bottom. Its help line at the top of
the screen says **From the timers addon.**, so the player always knows whose button it is.

## Good to know

- **When it appears.** The Config list is rebuilt with your button the next time it's closed. If the player has it
  open when your addon loads, it shows up when they open it again.
- **Your function runs on the next frame**, not inside the game's menu code, so it's safe to do anything there:
  open windows, run commands, print.
- **Taking it out:** `vana.gameui.remove('config_menu')` removes the button, and unloading the addon does too.
- **Room is limited:** 4 buttons fit after VanaCore's, and its own HUD, FPS Graph and Game Stats take three, so
  one addon's button fits, the first to ask. Past that the rest are left out (VanaCore's log says so).
- **Letters:** the label is drawn letter by letter in the game's own menu font. A to Y, a to z and spaces work; Z and
  the digits 0 to 9 aren't known yet (`configmenu.cpp`, `kGlyphs`) and show as gaps.
