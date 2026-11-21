# A party window in ImGui

An ImGui window listing the party with HP and TP bars, drawn every frame from `vana.game.party()`, with a scale slider
kept in a shared settings store. It shows the ImGui conventions: `Begin` with a one-item table for its open flag,
vectors as tables, colours as `ImU32` numbers, and a stable window name so the player can move and scale it from
`/vana layout`.

## `partywin.lua`

```lua
addon.name    = 'partywin'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'A party window with HP and TP bars.'

local imgui = vana.imgui
local config = vana.settings.register({ defaults = { scale = 1.0, shown = true } })

local open = { true }           -- Begin's p_open: a one-item table ImGui writes
local scale = { 1.0 }           -- SliderFloat's value, the same way
local NO_SCROLL = 8 + 16        -- ImGuiWindowFlags_NoScrollbar + NoScrollWithMouse
local FIRST_USE_EVER = 4        -- ImGuiCond_FirstUseEver

local function bar(fraction, colour, width)
    imgui.PushStyleColor(45, colour)             -- ImGuiCol_PlotHistogram: ProgressBar's fill
    imgui.ProgressBar(fraction, { width, 0 }, '')
    imgui.PopStyleColor()
end

vana.on('load', function ()
    open[1] = config.data.shown
    scale[1] = config.data.scale
end)

vana.on('present', function ()
    if not open[1] then return end
    local party = vana.game.party()
    if party == nil then return end

    imgui.SetNextWindowSize({ 320, 0 }, FIRST_USE_EVER)
    if imgui.Begin('Party###partywin', open, NO_SCROLL) then   -- ###: the name stays the same whatever the title says
        imgui.SetWindowFontScale(scale[1])
        for slot = 1, 6 do
            local m = party.members[slot]
            if m.active then
                imgui.Text(('%-15s'):format(m.name))
                imgui.SameLine(130 * scale[1])
                bar(m.hpp / 100, m.hpp < 30 and 0xFF3030FF or 0xFF40C040, 90 * scale[1])   -- red low, green else (0xAABBGGRR)
                imgui.SameLine()
                bar(math.min(m.tp, 3000) / 3000, m.tp >= 1000 and 0xFF40C0FF or 0xFF808080, 90 * scale[1])
            end
        end
        imgui.Separator()
        if imgui.SliderFloat('Scale', scale, 0.6, 2.0, '%.1f') then
            config.data.scale = scale[1]       -- saved at unload, or:
            config:save()
        end
    end
    imgui.End()
end)

vana.on('command', function (e)
    if e.command:lower():match('^/partywin') then
        open[1] = not open[1]
        config.data.shown = open[1]
        return true
    end
end)
```

## What to notice

- **`imgui.Begin('Party###partywin', open, flags)`** returns whether the window is open to draw into; `imgui.End()` is
  called either way. The player closing the window with its **x** sets `open[1]` to false.
- **Colours are `ImU32`**: `0xAABBGGRR`. `0xFF40C040` is opaque, green 0xC0, red 0x40.
- **`ProgressBar` takes `{ width, height }`**; 0 for a part means ImGui's default.
- **`PushStyleColor(45, ...)`** uses ImGui 1.92's `ImGuiCol_PlotHistogram`. The numbers are listed in
  [Drawing](../06-drawing.md); `addons\vanacore\libs\ashita-4.30\imgui.lua` has every name.
- **The window name is stable** (`###partywin`), so the Layout page and `config\imgui.ini` keep its place and scale by
  that name whatever the title shows.
- **A global store** (`scope` left out): the window's scale is the same for every character.

## `README.md`

```markdown
# partywin

A party window with HP and TP bars. /partywin shows or hides it; drag its Scale slider to size it.
Changes nothing in the game's interface. Hide the game's own party list with vana.gameui if you want this in its place.
```
