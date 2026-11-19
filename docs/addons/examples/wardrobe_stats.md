# The stats of the gear in your wardrobes

An addon that prints a wardrobe item's stats to the chat: `/wstats Brutal` prints those of every piece in your Mog
Wardrobes with "Brutal" in its name. The stats come from CatsEyeXI's own data (`vana.gamedata.item_stats`), the same the
Equipment window counts, with the item's augments. It shows reading containers, an item's stats and units, and a command
with arguments.

## `wstats.lua`

```lua
addon.name    = 'wstats'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'Prints the stats of the gear in your wardrobes, from CatsEyeXI\'s own data.'

-- The Mog Wardrobes' container ids: Wardrobe 1 is 8, Wardrobes 2 to 8 are 10 to 16
local WARDROBES = { 8, 10, 11, 12, 13, 14, 15, 16 }
local MOST = 5 -- pieces printed for one name

-- A modifier as the game writes it: Haste's 500 is "Haste +5%"
local function shown(s, name, value)
    local stat = s.stats[name]
    return ('%s %+g%s'):format(stat.label, value / stat.unit, stat.percent and '%' or '')
end

-- { NAME = value } written out, sorted
local function written(s, mods)
    local out = {}
    for name, value in pairs(mods) do
        out[#out + 1] = shown(s, name, value)
    end
    table.sort(out)
    return table.concat(out, ', ')
end

-- Every piece in the wardrobes: { wardrobe = 1 to 8, slot = the container's slot }
local function pieces()
    local found = {}
    for n, container in ipairs(WARDROBES) do
        local bag = vana.game.container(container)
        for i = 1, bag and bag.capacity - 1 or 0 do
            if bag[i] and bag[i].id ~= 0 then
                found[#found + 1] = { wardrobe = n, slot = bag[i] }
            end
        end
    end
    return found
end

local SOURCES = {
    server      = 'the server\'s data',
    description = 'its description (changed since the data was made)',
    unread      = 'the server\'s data (its description unread)',
}

local function print_stats(piece)
    local s, why = vana.gamedata.item_stats(piece.slot) -- a slot: its augments are counted too
    if s == nil then
        print('No stats: ' .. why)
        return
    end
    print(('%s (%d, Wardrobe %d), from %s:'):format(s.name or '?', s.id, piece.wardrobe, SOURCES[s.source]))
    if next(s.mods) then
        print('  ' .. written(s, s.mods))
    end
    if s.augments and next(s.augments) then
        print('  Augments: ' .. written(s, s.augments))
    end
    for _, latent in ipairs(s.latents or {}) do
        print(('  Latent: %s (condition %d, %d)'):format(shown(s, latent.mod, latent.value), latent.condition, latent.param))
    end
    for _, set in ipairs(s.sets or {}) do
        local bonuses = {}
        for name, values in pairs(set.bonuses) do
            for i, value in ipairs(values) do
                bonuses[#bonuses + 1] = ('%s with %d'):format(shown(s, name, value), set.least + i - 1)
            end
        end
        table.sort(bonuses)
        print(('  Set of %d items: %s'):format(#set.items, table.concat(bonuses, ', ')))
    end
end

vana.on('command', function (e)
    local word, rest = e.command:match('^(/%S+)%s*(.-)%s*$')
    if word == nil or word:lower() ~= '/wstats' then return end
    if rest == '' then
        print('/wstats <part of a name>: the stats of the pieces in your wardrobes named so')
        return true
    end
    local want, shown_count = rest:lower(), 0
    for _, piece in ipairs(pieces()) do
        local s = vana.gamedata.item_stats(piece.slot.id)
        if s and s.name and s.name:lower():find(want, 1, true) and shown_count < MOST then
            print_stats(piece)
            shown_count = shown_count + 1
        end
    end
    if shown_count == 0 then
        print('Nothing in your wardrobes named like "' .. rest .. '"')
    end
    return true
end)
```

What it prints, for a Brutal Earring +1 in Wardrobe 2:

```
Brutal Earring +1 (27537, Wardrobe 2), from the server's data:
  Double Attack +6%, Store TP +2
```

## What to notice

- **`vana.gamedata.item_stats(piece.slot)`.** Given a container's slot (`{ id, extra, ... }`), it counts the slot's
  augments too, as `augments`; given just an id, as in the name search, it has none. [Game data](../04-game-data.md#an-items-stats-vanagamedataitem_stats)
  has every field.
- **The values are the server's.** `mods` uses the server's modifier names and units: `value / stats[name].unit` is the
  number the game shows (Haste: 100 is 1%), and `stats[name].label` its name. A stat the Equipment window doesn't show
  (DEF, HP, STR...) has `shown = false` and its modifier name as its label.
- **`source` says where `mods` came from.** `'server'`: CatsEyeXI's server data, the item unchanged since it was made.
  `'description'`: CatsEyeXI has changed the item since, so the numbers in its description are taken over the server's.
- **`latents` are conditions, not totals.** `condition` and `param` are the server's (`LATENT` in `latent_effect.h`):
  whether one is on depends on the character (HP, the weather, the day...).
- **`vana.game.container(id)` is `nil`** before the inventory has come from the server, and a wardrobe you don't have
  has no slots, so `pieces()` finds none.
- **The command returns `true`** so the game doesn't see `/wstats`. (`/ws` is the game's own weapon skill command.)

## `README.md`

```markdown
# wstats

/wstats <part of a name>: prints the stats of the pieces in your Mog Wardrobes named so, from CatsEyeXI's own data,
their augments, latent effects and sets with them. Changes nothing in the game's interface.
```
