# Greet each zone, and remember which ones

An addon that says something when you enter a zone, counts how often you've been to each (per character), and lets you
turn it off. It shows game events, a task that waits, a per-character settings store, and a command with arguments.

## `zonegreeter.lua`

```lua
addon.name    = 'zonegreeter'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'Greets you in each zone and counts your visits.'

-- This character's visits: settings\addons\zonegreeter\<Character>\settings.json
local store = vana.settings.register({
    scope    = 'character',
    defaults = { enabled = true, visits = {} },
})

local function greet(zone)
    local visits = store.data.visits
    visits[zone] = (visits[zone] or 0) + 1
    local n = visits[zone]
    print(('Zone %d: visit %d%s'):format(zone, n, n == 1 and ' (first time!)' or ''))
end

-- The zone-in comes before the zone has loaded; wait a moment so the line lands in the new zone's chat
vana.on('zone_change', function (e)
    if not store.data.enabled then return end
    vana.tasks.once(3, greet, e.zone)
end)

-- The login's own zone-in isn't a zone_change: greet it too
vana.on('login', function (e)
    if store.data.enabled then
        vana.tasks.once(3, greet, e.zone)
    end
end)

vana.on('command', function (e)
    local word = e.command:lower():match('^/zg%s*(%S*)')
    if word == nil then return end
    if word == 'on' or word == 'off' then
        store.data.enabled = word == 'on'
        store:save()
        print('zonegreeter ' .. word)
    elseif word == 'top' then
        local rows = {}
        for zone, n in pairs(store.data.visits) do rows[#rows + 1] = { zone = zone, n = n } end
        table.sort(rows, function (a, b) return a.n > b.n end)
        for i = 1, math.min(5, #rows) do
            print(('  zone %d: %d visits'):format(rows[i].zone, rows[i].n))
        end
    else
        print('/zg on|off|top')
    end
    return true
end)
```

## What to notice

- **`vana.settings.register` with `scope = 'character'`.** Before login the store holds the defaults; once a character is
  in, it reads that character's file. `store.data.visits[zone] = ...` changes the table in place, and the change is
  saved at unload, at a character switch, or when `store:save()` is called. Zone ids are whole-number keys, so they
  round-trip through JSON as numbers.
- **`vana.tasks.once(3, greet, e.zone)`.** The arguments after the function are passed to it. The task runs 3 seconds
  later from the frame loop, so `greet` is an ordinary function.
- **`login` carries the zone** and isn't followed by a `zone_change`, so both are handled.
- **The command returns `true`** so the game doesn't see `/zg`.

## `README.md`

```markdown
# zonegreeter

Greets you in each zone and counts your visits per character. /zg on, /zg off, /zg top.
Changes nothing in the game's interface.
```
