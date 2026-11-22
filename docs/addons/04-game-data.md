# 4. Game data

`vana.game` reads FINAL FANTASY XI's own data out of the running game: the character, the party and alliance, every
entity around you, the target, your inventory and equipment, recast timers. Each call reads the game fresh and returns
a new table, or `nil` when the data isn't there (before login, at the character select, between zones). Names are text
as the game holds them, Shift-JIS ([Chat and commands](05-chat-and-commands.md) says how to convert).

To see any of it live, `/addon load apiexplorer`: the shipped API Explorer addon's Reference tab has each call on this
page, and its Snippets tab runs them and prints what comes back, with the code to copy
([The API Explorer](16-character-menus-and-tables.md#4-the-api-explorer)).

## Who's logged in

```lua
local name, id = vana.game.character()   -- 'Vekien', 12345678; nil before login
```

`nil` before a character is in the world, at the character select, and while the game has no player. This is what
`vana.settings` keys a character's folder on, and what the `login` event announces.

## Which server

```lua
local s = vana.game.server()
s.name   -- 'CatsEyeXI': the server's own name, as its lobby gives it (the world the character list shows)
s.host   -- 'server.catseyexi.com': the address VanaCore was given for it, in lower case
```

`name` is `nil` until the lobby has sent its world list, which it does before the character list shows, so it is there
by the time a character is in the world; it is what the server's own settings call it, at most 15 letters. `host` is
`nil` for PlayOnline as installed, and so is `name`. An addon made for one server checks `name`; the same server can be
reached at more than one address.

## The player, cheaply

```lua
local id, zone, job, level, subjob, sublevel, status, target, hp, mp, tp, hpp, mpp = vana.game.self()
```

Thirteen numbers, no table: for code that runs every frame (the Windower layer derives its `hp change` and friends
from this). `nil` before login, at the character select and whenever the player isn't in the world (no entity in party
slot 1). `status` is the entity status (0 idle, 1 engaged, 2 dead, 3 engaged dead, 4 in an
event, 5 on a chocobo, 33 resting, 44 crafting, 47 sitting, 85 fishing), `target` the main target's index (0 for none).

## The player's stats and buffs

```lua
local p = vana.game.player()
```

| Field | What |
|-------|------|
| `hp_max`, `mp_max` | Maximums (current HP, MP and TP are in party slot 1, below) |
| `main_job`, `main_job_level`, `sub_job`, `sub_job_level` | Job ids (table below) and levels. The server's own values, so they're right from the first frame after login |
| `exp`, `exp_needed` | Experience points into the level, and the level's total |
| `stats` | `{ str, dex, vit, agi, int, mnd, chr }`: the base stats |
| `stat_modifiers` | The same keys: what gear and buffs add (the green and red numbers) |
| `attack`, `defense` | As the status window shows them |
| `resists` | `{ fire, ice, wind, earth, lightning, water, light, dark }`: the elemental resistances |
| `title` | The title's id |
| `nation` | 0 San d'Oria, 1 Bastok, 2 Windurst |
| `rank`, `rank_points` | The nation rank, and the rank points toward the next |
| `homepoint` | The home point's zone id |
| `residence` | The residence's id |
| `gil` | What the inventory holds (its slot 0) |
| `limit_points`, `merit_points`, `merit_points_max` | Limit points toward the next merit, merits unspent, and the most the character can hold |
| `limit_breaker`, `exp_locked`, `limit_mode` | Merits unlocked; at the level cap (also set in limit mode); limit mode on |
| `assimilation_points` | Blue Mage |
| `su_level` | Superior equipment level |
| `item_level` | `{ highest, average, main, ranged }`: item levels, the average's as the part over 99 (0 below level 99) |
| `unity` | `{ faction, points, evaluation, evaluation_partial, chat_color }` |
| `mastery` | `{ job, level, flags, exp, exp_needed }`: job mastery |
| `mount_recast` | The mount's recast |
| `buffs` | `[1..32]` status effect ids; `-1` or `255` for an empty slot |
| `status_icons`, `status_timers` | `[1..32]` the status timers' icons (`255` for none) and their ends, the server's values from its status packet (0x063): Windower's addons read a timer as `timer / 60 + 1582473106` Unix seconds |
| `homepoints` | 64 bytes, a bit per home point known (`has_bit` below works on it) |
| `data_loaded`, `sub_map` | The game's flags for which of this the server has sent; the sub-map number |

Skills, job points, every job's level, the pet, missions, linkshells and merits have calls of their own, below.
[16. The character, the menus and the game's tables](16-character-menus-and-tables.md) says where each comes from.

```lua
local function has_buff(id)
    local p = vana.game.player()
    if p == nil then return false end
    for _, buff in ipairs(p.buffs) do
        if buff == id then return true end
    end
    return false
end
```

### Job ids

| id | Job | id | Job | id | Job |
|----|-----|----|-----|----|-----|
| 0 | none | 8 | DRK | 16 | BLU |
| 1 | WAR | 9 | BST | 17 | COR |
| 2 | MNK | 10 | BRD | 18 | PUP |
| 3 | WHM | 11 | RNG | 19 | DNC |
| 4 | BLM | 12 | SAM | 20 | SCH |
| 5 | RDM | 13 | NIN | 21 | GEO |
| 6 | THF | 14 | DRG | 22 | RUN |
| 7 | PLD | 15 | SMN | 23 | MON |

The `job_change` event hands you the name (`e.job_name`) as well.

## Skills, crafts and job points

```lua
local skills = vana.game.skills()        -- nil before login
skills[9]                                -- { name = 'katana', skill = 236, capped = true }
local crafts = vana.game.crafts()
crafts[6]                                -- { name = 'bonecraft', skill = 100, rank = 9, capped = true }
local points = vana.game.job_points()
points[13]                               -- { capacity = 0, points = 0, spent = 0 }: NIN's job points
```

Skills are by the game's skill id, each `{ name, skill, capped }`: 1 hand_to_hand, 2 dagger, 3 sword, 4 great_sword, 5 axe,
6 great_axe, 7 scythe, 8 polearm, 9 katana, 10 great_katana, 11 club, 12 staff, 22 automaton_melee, 23 automaton_archery,
24 automaton_magic, 25 archery, 26 marksmanship, 27 throwing, 28 guard, 29 evasion, 30 shield, 31 parrying, 32 divine_magic,
33 healing_magic, 34 enhancing_magic, 35 enfeebling_magic, 36 elemental_magic, 37 dark_magic, 38 summoning_magic, 39 ninjutsu,
40 singing, 41 stringed_instrument, 42 wind_instrument, 43 blue_magic, 44 geomancy, 45 handbell. `capped` is set while the
skill is at its cap for the level. Crafts are `{ name, skill, rank, capped }` by craft: 0 fishing, 1 woodworking, 2 smithing,
3 goldsmithing, 4 clothcraft, 5 leathercraft, 6 bonecraft, 7 alchemy, 8 cooking, 9 synergy, 10 riding, 11 digging; `rank`
is the guild rank (0 Amateur to 9 Veteran), `capped` set when the next rank is needed. Job points are by job id,
`{ capacity, points, spent }`.

```lua
local skills = vana.game.skills()
if skills then
    for _, s in pairs(skills) do
        if s.skill > 0 then print(('%-20s %3d%s'):format(s.name, s.skill, s.capped and ' (capped)' or '')) end
    end
end
```

## Every job's level

```lua
local jobs = vana.game.jobs()   -- nil while the game can't say
jobs[13].level                  -- NIN's level
jobs[13].master_level           -- and its master level
```

By job id, `{ level, master_level }`, from the game's own getters (what Ashita's `GetJobLevel` calls): 0 for a job not yet
unlocked.

## The pet

```lua
local pet = vana.game.pet()   -- nil without a pet
```

| Field | What |
|-------|------|
| `index` | The pet's entity index (`vana.game.entity(pet.index)` for its position and the rest) |
| `id`, `name` | Its entity's, when it's in range |
| `hpp`, `mpp`, `tp` | The server's last word on them (its pet status packet, 0x068) |
| `target_id` | What it's fighting (server id; 0 for nothing) |
| `owner_index`, `owner_id` | The owner, as the packet named them |

The game keeps the server's last pet status; a released pet reads as none. The entity has its HP% too (`hpp`) and the
owner's entity its `pet_index`.

## Missions

```lua
local m = vana.game.missions()   -- nil until the server has sent the current missions (at login)
```

| Field | What |
|-------|------|
| `nation`, `nation_mission` | The nation (0 San d'Oria, 1 Bastok, 2 Windurst) and its current mission's id; `65535` for none |
| `zilart`, `promathia`, `adoulin`, `rhapsodies`, `voracious` | The current mission in each of those, as the server numbers them |
| `promathia_progress` | A nibble per chapter |
| `crystalline_prophecy`, `moogle_kupo_detat`, `shantotto_ascension` | The add-ons' current missions (4 bits each) |
| `declined` | Expansions the player turned down: bit 0 Zilart, 1 ACP, 2 ASA, 3 Promathia, 5 Adoulin, 6 Rhapsodies |

These are the game's own copy of the server's mission log (packet 0x056), the values as the server sends them (Treasures
of Aht Urhgan and Wings of the Goddess aren't in that log). Quests and completed missions aren't read.

## Linkshells

```lua
local ls = vana.game.linkshell(1)   -- the linkshell; vana.game.linkshell(2) is linkshell 2. nil when there's none to say
```

| Field | What |
|-------|------|
| `slot` | 1 or 2 |
| `name` | The linkshell's name |
| `message`, `poster`, `time` | Its message (Shift-JIS), who set it and when (Unix seconds); from the server's message packet (0x0CC), at login and when it changes |
| `read_level`, `write_level`, `public_edit_level` | The message's permissions |
| `rank`, `color`, `id` | From the pearl equipped for it in the inventory: 1 linkshell, 2 pearlsack, 3 linkpearl; the colour word; the linkshell's id |
| `item`, `item_id` | The pearl's inventory index and item id (513 linkshell, 514 pearlsack, 515 linkpearl) |

The game has no readable copy of a linkshell's name, so VanaCore keeps the server's message packets per slot (they go at
logout) and reads the equipped pearls' names out of their extra data; a slot with no message yet still has its pearl's
fields. `slot` is optional (1 by default). Without a pearl equipped for the slot, `rank`, `color`, `id`, `item` and
`item_id` are left out; without a message, `message`, `poster`, `time` and the three levels are. The `linkshell change` event (Windower layer) and chat mode 6 / 213 lines come from the same linkshells.

## Merits

```lua
local merits = vana.game.merits()   -- {} before the server has listed them
merits[128]                         -- { count = 3, next = 4 }: STR merits bought, and the next one's cost
```

By merit id (Windower's `res/merit_points.lua` names them), from the server's merit list (0x08C: the whole list at login,
the changed ones as they're bought). The points to spend are `vana.game.player().merit_points`.

## The party and alliance

```lua
local party = vana.game.party()
```

| Field | What |
|-------|------|
| `members` | `[1..18]`: slots 1 to 6 your party (slot 1 is you), 7 to 12 and 13 to 18 the other two alliance parties |
| `leader` | The alliance leader's server id |
| `party_leaders` | `[1..3]`: each party's leader's id |
| `party_counts` | `[1..3]`: how many members each party has |

Each member:

| Field | What |
|-------|------|
| `active` | `true` when the slot is filled. Check it first |
| `slot` | 0 to 17 |
| `name`, `id` | Name and server id |
| `target_index` | The member's entity index: 0 while out of the zone (then `vana.game.entity` can't see them) |
| `hp`, `mp`, `tp` | Current values |
| `hpp`, `mpp` | Percents |
| `hp_max`, `hp_max_low`, `hp_max_high`, `hp_max_source`, `mp_max`, ... | The maximums the game never gives for anyone but you: [below](#maximum-hp-and-mp) |
| `zone` | The zone the member is in |
| `main_job`, `main_job_level`, `sub_job`, `sub_job_level` | |
| `flags` | The game's flags word |

```lua
local party = vana.game.party()
if party then
    for slot = 1, 6 do
        local m = party.members[slot]
        if m.active then
            print(('%-12s %3d%% HP  %4d TP  %s'):format(m.name, m.hpp, m.tp, m.zone == party.members[1].zone and '' or '(away)'))
        end
    end
end
```

A party can hold a seventh member on CatsEyeXI (a fourth trust let in while a member is out of the zone). The list
reads six, as the game draws six.

## Entities

Anything with a target index: players, NPCs, monsters, pets, doors, home points.

```lua
local e = vana.game.entity(index)    -- nil if there's nothing at that index
local n = vana.game.entity_count()   -- how many slots there are (2304)
```

| Field | What |
|-------|------|
| `index` | Target index, 0 to `entity_count() - 1`. Players and NPCs of the zone have fixed indexes; players are 0x400 and up |
| `id` | Server id |
| `name` | Shift-JIS |
| `x`, `y`, `z` | Position. `z` is height, the game's way round: up is a smaller `z`; `x`, `y` are the map plane |
| `heading` | Radians |
| `distance` | To you, **squared** yalms, as the game keeps it: `math.sqrt(e.distance)` for yalms |
| `speed` | Movement speed |
| `hpp` | HP percent |
| `type` | 0 player, 1 NPC or monster, 2 NPC with a fixed model, 3 door or similar |
| `race` | Race id |
| `status` | As `vana.game.self()`'s |
| `spawn_flags` | `0x01` player, `0x02` NPC, `0x10` monster, `0x0D` the local player |
| `model_size`, `hitbox_size` | |
| `claim` | Claim word; its low word is the claimer's id |
| `target_index` | What it's targeting (not kept for the local player) |
| `pet_index` | Its pet's index |
| `render_flags`, `name_flags` | The game's flag words; `render_flags` bit `0x200` is set while it can be targeted |
| `valid_target` | That bit as a boolean |
| `name_x`, `name_top`, `name_bottom` | Where its name over its head is on the screen, in the interface's pixels (`vana.ui.screen`'s): its place across (the name is centred on it) and the letters' top and bottom as the game drew them in the last moments, so it follows a tall hat or a big monster. Left out (`nil`) while its name isn't drawn: too far, or hidden by the nameplates settings. The same as [`vana.world.name_at`](18-widgets.md#where-a-name-is-vanaworldname_at). VanaCore 0.8.224 or later |
| `models` | `[1..9]`: hair, head, body, hands, legs, feet, main, sub, ranged |
| `distance_yalms` | The square root of `distance` |
| `is_pc`, `is_npc`, `is_monster`, `is_self` | `spawn_flags`' bits `0x01`, `0x02`, `0x10`, and `0x0D` (the local player) as booleans |
| `claim_id`, `claimed` | `claim`'s low word (the claimer's id) and its high-word flag |
| `anim_speed`, `door_id` | The animation speed; a door's id (`_6er`) |
| `render` | `[1..9]`: the render flag words 0 to 8 (`render_flags` and `name_flags` are the first two; the third is the second name flags: bazaar, GM icon) |
| `status_server`, `status_event` | The status as the server sent it; the status while it talks to an NPC or is in an event |
| `costume_id`, `mount_id`, `zone_id` | Its costume, its mount (while the status is 85) and its zone (only the local player's, under conditions) |
| `animations` | `[1..10]`: the animations it plays |
| `animation_time`, `animation_step`, `animation_play` | The current animation's time and step, and the flag that makes it play |
| `emote_id`, `emote_target_index` | The emote it performs and who it is aimed at |
| `last_action_id` | Its last action's id (talking to some NPCs sets it to `tlk0`) |
| `name_color`, `linkshell_color`, `campaign_name_flag`, `ballista_flags` | Name colour code; linkshell colour (BGR); the campaign sword; Ballista's |
| `interaction_target_index` | Who it last talked to |
| `fellow_index`, `trust_owner_index` | Its adventuring fellow; a trust's owner |
| `party_slot`, `hp`, `mp`, `tp`, `mpp` | **Only for a member of your party or alliance** (a trust too): their numbers as the game has them. `hpp` is every entity's |
| `hp_max`, `hp_max_low`, `hp_max_high`, `hp_max_source`, `mp_max`, ... | The maximums: [below](#maximum-hp-and-mp). `hp_max_source` is `'none'` and there are no numbers for an entity outside the party and alliance, until the server is [asked](#asking-the-server-monsters-too) (`'server'`) |

`vana.game.entity_by_id(id)` is the same table for the entity with that server id (nil when there is none). It looks at every
slot, so ask when you need it and keep the index.

### Maximum HP and MP

The game has a maximum HP and MP for **you** only. For a party or alliance member the server sends the current value and its
percent, and for any other entity (a monster you have targeted) only the percent: the game has no number for it to read, so
there is none to give. VanaCore does for the members what an addon would otherwise do for itself: it samples the party every
frame and works the maximum out, once, for every addon and plugin.

| Field | What |
|-------|------|
| `hp_max` | The maximum. Left out (nil) when `hp_max_source` is `'none'` |
| `hp_max_low`, `hp_max_high` | It is not below the first, nor above the second (`4294967295`: no upper limit yet) |
| `hp_max_source` | `'self'`: the game's own number (you), exact. `'full'`: the value seen while the percent read 100, kept until a later sample disagrees: within a percent of the maximum, usually exact. `'estimate'`: worked out from a value and its percent, narrowed by each sample as the value moves; trust `hp_max_low` and `hp_max_high` over `hp_max`. `'server'`: CatsEyeXI's server said it when [asked](#asking-the-server-monsters-too), exact, kept for a minute. `'none'`: not in your party or alliance, and the server not asked |
| `mp_max`, `mp_max_low`, `mp_max_high`, `mp_max_source` | The same for MP |

A member who has been seen at full HP has `'full'`; one who has not (you joined them already hurt) has an `'estimate'` that
tightens as their HP changes, and becomes `'full'` the first time they read 100%. A maximum that changes (a curse, a
weakness ending, a food or a buff) shows as a percent the last samples don't allow, and the maximum starts again from that
sample: it follows the change when the percent moves, and until then (a change too small to move it) stays as it was.
Someone who leaves the party starts again when they come back.

#### Asking the server (monsters too)

A monster's maximum HP is nowhere in the game, so VanaCore can only get it from the server. CatsEyeXI's server answers a
request for it (its "VanaCore modules": `entity max HP`, packet `0x1A7`), by server id:

`vana.game.request_max_hp(ids, callback)` — `ids` is a server id or a list of up to 8. Returns `true` when the request is
sent, or `false` and why (not in the game yet, more than 8 ids, too many requests waiting). `callback(answers)` is called
once, in a following frame, with a list in the order asked of `{ id, hp, hp_max, status }`:

| `status` | |
|----------|---|
| `'ok'` | `hp` is the entity's HP and `hp_max` its maximum, as the server has them right now (curses, Max HP boosts and Max HP down are in the maximum). `hp` is a snapshot of that moment: ask again for a newer one |
| `'not_found'` | the server has nothing to say for that id: it is not you, your party or alliance, nor a monster, pet or trust your client has been told about (so an entity out of range is not answered) |
| `'disabled'` | the server has the feature switched off |
| `'too_fast'` | you asked again within 100 ms; the server keeps that gap per character |
| `'hidden'` | the server keeps this monster's maximum secret (a boss it has marked); `hp` and `hp_max` are 0, and asking again won't change it |
| `'no_answer'` | nothing came in 3 seconds: a server without the module, or a lost reply |

Every id gets an answer, so the callback always runs. Only HP is available: monsters have no MP the player is meant to know.
An `'ok'` answer's `hp_max` is also kept for a minute as that entity's `hp_max` (`hp_max_source = 'server'`) in `vana.game.entity`
(the `hp` is not kept: it is out of date within a frame), until you zone; ask again to refresh it (a monster that despawns and spawns again may have rolled another level).

```lua
-- size a burn by the target's maximum HP
local function on_target()
    local t = vana.game.target()
    local e = t.active and vana.game.entity(t.index)
    if not e then return end
    if e.hp_max then return use(e.hp_max, e.hpp) end
    vana.game.request_max_hp(e.id, function (answers)
        local a = answers[1]
        if a.status == 'ok' then use(a.hp_max, e.hpp) end
    end)
end
```

Print the current target's HP and maximum HP (the API Explorer addon has this as "The target's HP and max HP (asks the
server)", under Party and targets):

```lua
local t = vana.game.target()
local e = t and t.active and vana.game.entity(t.index)
if e then
    vana.game.request_max_hp(t.id, function (answers)
        local a = answers[1]
        if a.status == 'ok' then
            print(('%s: %d / %d HP (%d%%)'):format(e.name, a.hp, a.hp_max, e.hpp))
        elseif a.status == 'hidden' then
            print(('%s: the server keeps its HP secret (%d%%)'):format(e.name, e.hpp))   -- a boss marked to hide it
        else
            print(('%s: no number (%s), HP %d%%'):format(e.name, a.status, e.hpp))
        end
    end)
end
```

An Ashita addon can use it the same way behind `if vana then`; it runs unchanged in real Ashita, where the call isn't there.

```lua
local party = vana.game.party()
local me = party and party.members[1]
if me and me.hp_max_source ~= 'none' then
    print(('%d of %d HP, %d missing (%s)'):format(me.hp, me.hp_max, me.hp_max - me.hp, me.hp_max_source))
end

-- a heal sized by the target's missing HP
local function missing(id)
    local v = vana.game.vitals(id)    -- by server id, also for a member in another zone
    if v and v.hp_max then return v.hp_max - v.hp, v.hp_max_source end
end
```

Walking the whole table every frame is heavy (2304 reads). Walk it when something changes, or a slice a frame
([Tasks and timing](03-tasks-and-timing.md)), and keep the indexes you care about.

```lua
-- the nearest monster within 20 yalms
local function nearest_monster()
    local best, best_d
    for index = 0, vana.game.entity_count() - 1 do
        local e = vana.game.entity(index)
        if e and bit.band(e.spawn_flags, 0x10) ~= 0 and e.hpp > 0 and e.valid_target and e.distance < 400 then
            if best_d == nil or e.distance < best_d then
                best, best_d = e, e.distance
            end
        end
    end
    return best
end
```

## The target

```lua
local t = vana.game.target()
```

| Field | What |
|-------|------|
| `index`, `id`, `active` | The main target (`index` 0 and `active` false: none) |
| `sub_index`, `sub_id`, `sub_active` | The sub target, while one is being chosen (`<st>`) |
| `is_sub_target` | A sub target is being chosen |
| `locked` | Locked on |
| `menu_open` | A game menu is open |
| `raw` | `[1..2]` `{ index, id, active }`: the game's two entries as they are |

```lua
vana.game.set_target(index)   -- targets that entity as the game's own code does; false if it has no actor (out of range)
```

The `target_change` event fires when the main target changes, with the old and new indexes.

## Zone, time and weather

```lua
local w = vana.game.world()
w.server_time   -- Unix seconds, from the server's last packets (0 before any)
w.weather       -- the zone's weather id
w.chat_open     -- the chat line is open (the player is typing)

vana.game.zoning()               -- true between zones: the zone-out came and the player isn't back yet
vana.game.loading()              -- nil, or the load under way: phase ('server', 'zone', 'people'), login, zone,
                                 -- previous, elapsed, ready, zone_files, zone_expected, people_files,
                                 -- people_expected (0 not known yet), file ('ROM/1/41.DAT'), step (while the
                                 -- server is waited for after a zone-out: 'leaving', 'connecting', 'waiting'),
                                 -- wait (s the last waits took), answer (ms the server took to answer, once it
                                 -- has: a round trip), out_type (the zone-out's type, 2 a zone change); the
                                 -- loadscreen addon's
vana.game.player_zone(listed)    -- the zone the server's zone-in named, until the party list (`listed`) has caught up
vana.game.zone_weathers(zone)    -- the weathers (0x057's ids) the zone's files have a sky and light for, or nil
```

`zone_weathers` reads the zone's model DAT, the one the game loads (an overlay's when pivot has one): the weather folders
of its environment tree (`weat\fine`, `weat\rain`...). A weather a zone has no folder for draws there with another's sky,
often clear's: most cities have no rain. Read once a zone, its section headers only.

The party list's zone for you (`party.members[1].zone`) lags a zone-in by the seconds the new zone takes to load.
`player_zone` gives the new one at once:

```lua
local zone = vana.game.player_zone(party.members[1].zone)
```

Or listen for `zone_change`, which carries it.

## Abilities, spells and key items

```lua
local abilities = vana.game.abilities()   -- 224 bytes: the game's bits for weapon skills, job abilities, pet commands, traits
local spells    = vana.game.spells()      -- 128 bytes: a bit per spell id
local key_items = vana.game.key_items()   -- 512 bytes: a bit per key item id
```

Each is a string of the game's bits, `nil` until the server has sent them. Bit `n` is set when the player has `n`:

```lua
local function has_bit(bits, n)
    local byte = bits:byte(math.floor(n / 8) + 1)
    return byte ~= nil and bit.band(byte, bit.lshift(1, n % 8)) ~= 0
end

local knows_cure = has_bit(vana.game.spells() or '', 1) -- spell 1 is Cure
```

The abilities string is the 0xAC packet's 224 bytes as the game keeps them. By kind: weapon skills from bit 0 (bit
`n` is weapon skill `n`), job abilities from bit 0x200, pet commands from 0x400, traits from 0x600 to 0x6FF: the bits
Ashita's `HasWeaponSkill`, `HasPetCommand` and `HasTrait` test.

## Recast timers

```lua
local r = vana.game.recasts()   -- nil before the game has them
r.abilities[timer_id]   -- 1/60ths of a second left, for the ability recast timers counting down
r.spells[spell_id]      -- 1/60ths of a second left, for the spells counting down
```

Only timers that are counting appear. Divide by 60 for seconds. Ability keys are the game's recast timer ids (the
ability's `recast_id`, not its ability id).

## Inventory

```lua
local bag = vana.game.container(0)   -- nil before the inventory is there
bag.capacity                          -- the game's count, slot 0 included: 81 for an 80-slot inventory
bag[index]                            -- index 0 to capacity - 1 (80 at most): nil past what's read
```

Slot 0 is reserved in every bag; in the inventory it holds the gil (`bag[0].count`, what `player().gil` reads). Items
are in 1 to `capacity - 1`. Each slot: `{ id, index, count, flags, price, extra }`. `id` is the item id (0 for an empty slot), `count` how many,
`flags` the game's (bazaar, equipped, ...), `price` the bazaar price, `extra` the 24 augment bytes as a string.

| Container id | Bag |
|--------------|-----|
| 0 | Inventory |
| 1 | Mog Safe |
| 2 | Storage |
| 3 | Temporary items |
| 4 | Mog Locker |
| 5 | Mog Satchel |
| 6 | Mog Sack |
| 7 | Mog Case |
| 8 | Mog Wardrobe |
| 9 | Mog Safe 2 |
| 10 to 15 | Mog Wardrobe 2 to 7 |
| 16 | Mog Wardrobe 8 |
| 17 | Recycle Bin |

A slot has only the item's id. Its name, level and the rest are in `vana.gamedata.items[id]`
([Static game data](#static-game-data-vanagamedata)): `vana.gamedata.items[bag[i].id].name`.

```lua
local bag = vana.game.container(0)
if bag then
    local used = 0
    for i = 1, bag.capacity - 1 do
        if bag[i] and bag[i].id ~= 0 then used = used + 1 end
    end
    print(('Inventory: %d/%d'):format(used, bag.capacity - 1))
end
```

## Equipment

```lua
local eq = vana.game.equipment()   -- nil before the inventory is there
eq[slot]                            -- { container, index }: where the item is; index 0 for nothing equipped
```

| Slot | | Slot | | Slot | | Slot | |
|------|-|------|-|------|-|------|-|
| 0 | Main | 4 | Head | 8 | Feet | 12 | Right ear |
| 1 | Sub | 5 | Body | 9 | Neck | 13 | Left ring |
| 2 | Range | 6 | Hands | 10 | Waist | 14 | Right ring |
| 3 | Ammo | 7 | Legs | 11 | Left ear | 15 | Back |

```lua
local eq = vana.game.equipment()
if eq and eq[0].index ~= 0 then
    local item = vana.game.container(eq[0].container)[eq[0].index]
    print('Main hand item id', item.id)
end
```

## Packets as a source

Some things aren't in memory in a handy form, or you want them the moment they arrive. The last packet of any id is
kept: `vana.packets.last(true, 0x00A)` is the login or zone-in packet, `vana.packets.last(true, 0x01B)` the job info.
[Packets](10-packets.md) has the layout and the `packet_in` event.

## Static game data: `vana.gamedata`

Everything above is read from the running game and is only ids: a buff is `33`, a spell `4`, an item `26963`. The
names, descriptions, costs and the rest live in the game's DAT files and in the server, so VanaCore ships them as Lua
tables, generated from both (`tools/gen_gamedata.py` in CatsEyeXI's [CatsAndBoats/dats](https://github.com/CatsAndBoats/dats) repository, from the DATs through
xi-tools and CatsEyeXI's server SQL), under `addons\vanacore\libs\gamedata\`. Nothing is read from the game for these.

```lua
local id = vana.gamedata.find('spells', 'Cure IV')    -- 4 (case-insensitive; items by their log name too)
local spell = vana.gamedata.spells[id]
print(spell.name, spell.mp, spell.cast_time, spell.recast, spell.jobs[3])   -- Cure IV  88  2.5  8  41 (WHM)
local item = vana.gamedata.items[26963]
print(item.name, item.level, item.item_level, table.concat(item.slot_names, ','), (item.mods or {}).STR)
local buff = vana.gamedata.status_effects[33]      -- { name = 'Haste', adjective = 'hastened', ... }
local zone = vana.gamedata.zones[245]              -- { name = 'Lower Jeuno', ... }
```

| Table | Keyed by | Each row |
|-------|----------|----------|
| `spells` | spell id | `name`, `description`, `type` (white, black, song, ninjutsu, summoning, blue, geomancy, trust), `skill`, `skill_name`, `element`, `mp`, `cast_time` and `recast` (seconds), `range`, `radius`, `aoe`, `targets` (bits) and `target_names`, `jobs` (`{ [job id] = level }`), `requirements`, `family`, `message`, `animation`, `icon`, `content_tag`; also `id`, `magic_burst_message`, `animation_time` (ms), `base`, `multiplier`, `ce` and `ve` (the server's enmity), `menu_index` |
| `abilities` | ability id (the server's and Windower's; the game's command record is `id + 0x200`) | `name`, `description`, `job`, `job_name`, `level`, `recast` (seconds), `recast_id` (the timer `vana.game.recasts()` keys), `targets`, `target_names`, `range`, `aoe`, `radius`, `action_type`, `add_type`, `add_type_names`, `merit_mod`, `charges`, `tp`, `icon` |
| `weapon_skills` | weapon skill id | `name`, `description`, `skill`, `skill_name`, `skill_level`, `element`, `range`, `aoe`, `skillchains` (`{ 'impaction', ... }`), `main_only`, `jobs` |
| `status_effects` | status id (`vana.game.player().buffs` values) | `name`, `adjective`, `server_name`, `flags` and `flag_names`, `type`, `negative_id`, `overwrite`, `block_id`, `remove_id`, `element`, `min_duration` |
| `zones` | zone id | `name`, `server_name`, `type` and `type_names` (city, outdoors, dungeon, ...), `music` (`day`, `night`, `battle_solo`, `battle_party`), `restriction`, `tax`, `id`, `misc` (the server's zone flags) |
| `items` | item id | `id`, `name`, `log_name`, `log_plural`, `article`, `description`, `resource_id`, `category` (general, usable, armor, weapon, puppet, maze, instinct, roe, currency), `type`, `flags` and `flag_names`, `stack`, `targets`; equipment: `level`, `item_level`, `superior_level`, `slots` and `slot_names`, `races` and `race_names`, `jobs` (bits, bit n = job n) and `job_ids`, `shield_size`, `max_charges`, `cast_time`, `use_delay`, `reuse_delay`; weapons: `damage`, `delay`, `dps`, `skill`, `skill_name`, `damage_type`, `hits`, `jug_size`, `base_item_id`, `subskill`, `ilvl_skill`, `ilvl_parry`, `ilvl_macc`, `unlock_points`; puppet parts: `puppet_slot`, `element_charge`; instincts: `instinct_cost`; from the server: `mods` (`{ STR = 5, ... }`, the server's modifier names; only on an item that has any), `server_name`, `sort_name`, `server_type`, `ah` and `ah_name` (the auction house category), `base_sell`, `model_id`, `script_type`, `rslot`, `rslot_look`; usable items: `use_targets`, `activation`, `animation`, `animation_time`, `use_max_charges`, `use_delay_server`, `reuse_delay_server`, `use_aoe` |
| `key_items` | key item id | `name`, `plural`, `description`, `category` |
| `titles` | title id | `name` |
| `jobs` | job id | `name`, `short` |
| `skills` | skill id | `name`, `display`, `kind` (combat, magic, craft) |
| `elements` | the server's element number | `name` (none, fire, ice, wind, earth, lightning, water, light, dark) |
| `merits`, `job_points`, `traits` | their server ids | `name`, `display`, `upgrades`, `jobs` / `job`; traits: `levels` (`{ job, level, rank, mod, value }` each) |
| `mounts`, `monster_families`, `augments`, `slots` | their ids | names and texts; families with their stats |
| `skill_caps`, `skill_ranks` | level / skill id | a level's cap per rank (A+ first); each job's rank in a skill |

`vana.gamedata.tables()` lists them with their row counts and the date they were generated. A table loads the first
time an addon touches it (`spells` in a few milliseconds; `items` is in parts of 4,096 ids, and the part an id is in
loads when it's first asked for, so an addon reading armour doesn't parse the 25,000 items). `pairs()` on `items` only
sees the parts loaded so far: `vana.gamedata.all('items')` loads every part first (about a second). Each addon's Lua
state has its own copy.

Text is UTF-8 as the client shows it, with `{Fire}` and the other element icons as those words; a description's line
breaks are `\n`. To print one to the chat, `vana.encoding.to_shift_jis` first. Times are seconds where the server
keeps milliseconds (`cast_time`, `recast`) and the server's own units elsewhere (`recast` of an ability is seconds,
an item's `cast_time` and delays are the client's numbers). Where the server's enums name a value the row carries both
the number and the names (`targets` / `target_names`, `flags` / `flag_names`); unlisted bits are just bits.

To regenerate after a DAT or server change, from the dats repository with xi-tools' Python (its `.env` naming the game
install):

```
<xi-tools>\.venv\Scripts\python.exe tools\gen_gamedata.py --out <VanaCore>\addons\vanacore\libs\gamedata [--only spells,items]
```

## An item's stats: `vana.gamedata.item_stats`

What an item gives, as VanaCore's Equipment window counts it (`[client] equip.stats`), from CatsEyeXI's own data:

```lua
local s, why = vana.gamedata.item_stats(27537)        -- by id; nil and why while the data isn't read
local s = vana.gamedata.item_stats(bag[i])            -- a container's slot ({ id, extra }): its augments too
local s = vana.gamedata.item_stats(id, extra)         -- an id and its 24 extdata bytes

for name, value in pairs(s.mods) do
    local stat = s.stats[name]
    print(('%s %+g%s'):format(stat.label, value / stat.unit, stat.percent and '%' or ''))
end
-- Double Attack +6%
-- Store TP +2
```

Where it comes from: `addons\vanacore\libs\gamedata\equip_mods.json`, made by `gen_gamedata.py` from CatsEyeXI's server
(its SQL with its custom items' over it, its gear sets' Lua), and the item's description in the game's DATs as the game
has them now, CatsEyeXI's own included. An item CatsEyeXI changes after the data was made has a different description:
for that one the numbers its description gives are taken over the server's (a stat it gives no number for stays the
server's). `vana.gamedata.items[id].mods` is the server's as the items tables were made; `item_stats` follows the DATs.

| Field | |
|-------|-|
| `id`, `name` | The item, its name as the DATs have it |
| `description` | Its description as VanaCore reads its numbers: ASCII, element icons as their names |
| `source` | Where `mods` came from: `'server'` (its description is the one the data was made with), `'description'` (changed since, or new: its numbers over the server's), `'unread'` (the DATs couldn't be read: the server's alone) |
| `mods` | `{ NAME = value }`: what VanaCore counts, by the server's modifier names (`modifier.h`), in the server's units |
| `server` | `{ NAME = value }`: the server's alone (`equip_mods.json`), to compare |
| `latents` | `{ { mod, value, condition, param }, ... }`: `condition` is the server's `LATENT` (`latent_effect.h`), `param` its parameter. Left out when it has none |
| `sets` | Each gear set it's in: `{ items, least, most, bonuses }`. `items` the set's item ids; `bonuses` `{ NAME = { for least pieces, least + 1, ... } }`, up to `most`. Left out when it's in none |
| `augments` | Given its extdata: `{ NAME = value }`, its augments' modifiers as the server works them out (a charged item's extdata isn't augments: `{}`) |
| `stats` | Each `NAME` above: `{ label, unit, percent, highest, shown }`. `value / unit` is the number shown (Haste: 100 = 1%); `percent` shown with %; `highest` only the highest worn counts, not the sum; `shown = false` for one the Equipment window doesn't show (DEF, HP, MP, STR to CHR, elemental magic evasion, added effects), labelled by its name |
| `data` | `{ generated, source }`: when `equip_mods.json` was made, from which server and modules commits |

The first call of the game reads `equip_mods.json` (a moment, once for every addon); each item's description is read
once. Nil and why: `'not an item id'`, `'not ready yet: ask again'` (before the game's first frame),
`'still being read: ask again'` (the Equipment window is reading it), or the file wasn't read. The server keeps its own
totals: `/checkparam` (and the Equipment window's top rows) are those.
