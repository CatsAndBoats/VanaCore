# 10. Packets

Everything the server tells the game, and everything the game tells the server, goes through packets. An addon can
read each one as it passes, change it, block it, add its own, and ask for the last one of an id.

## The layout

The game sends and receives buffers, each a 28-byte header followed by packets. Every packet starts with a 4-byte
header:

| Bytes | What |
|-------|------|
| 0-1 | A 16-bit word: the id in its low 9 bits, the size in 4-byte units in its high 7 |
| 2-3 | The sequence number |
| 4 on | The payload |

Incoming and outgoing ids are separate numberings (incoming `0x00A` is the zone-in; outgoing `0x00A` is the login).
Some you'll meet:

| Incoming | | Outgoing | |
|----------|-|----------|-|
| `0x00A` | Zone in (the character's details, the zone) | `0x015` | Player position, every ~400 ms |
| `0x00B` | Zone out | `0x01A` | An action (attack, cast, ability, item) |
| `0x00D` | A player's update | `0x05B` | An event's option (menus, home points) |
| `0x00E` | An NPC's update | `0x0B5` | A chat line |
| `0x017` | A chat message | | |
| `0x01B` | Job info | | |
| `0x028` | An action (the battle log's source) | | |
| `0x029` | An action message | | |
| `0x037` | The player's update: buffs, status | | |
| `0x0DD` | A party member's update | | |
| `0x0DF` | The character's HP, MP, TP | | |

The community's packet documentation (Windower's `packets` library, Ashita's `packets` docs) describes the fields of
each. Windower's library is in `addons\windower\libs\packets` when Windower addons are installed.

## Reading packets: `packet_in` and `packet_out`

```lua
vana.on('packet_in', function (e)
    if e.id == 0x00A then
        local zone = e.data:byte(0x31) + e.data:byte(0x32) * 256   -- bytes 0x30-0x31, 1-based for string.byte
        print('Zoning into', zone)
    end
end)
```

| `e` field | What |
|-----------|------|
| `id` | The packet id |
| `size` | Its length in bytes |
| `data` | The packet as a string, header first, as the game received (or built) it |
| `data_raw` | The same bytes, a pointer for the FFI |
| `data_modified` | What the game will get (or send): assign a new string to change it |
| `data_modified_raw` | A pointer to those bytes: write through it with the FFI |
| `chunk_size`, `chunk_data`, `chunk_data_raw` | The whole buffer the packet came in |
| `injected` | VanaCore or an addon added it |
| `blocked` | An earlier handler blocked it |

Return `true`, or set `e.blocked = true`, to block the packet: the game doesn't get it (incoming) or it isn't sent
(outgoing). Later addons still see it, with `blocked` true.

To change a packet, assign a string of the same length to `e.data_modified`, or write through `data_modified_raw`.
What's copied back is at most the packet's size. Changes apply whether or not you block.

Reading fields with the FFI is faster than `string.byte` for anything you do often:

```lua
local ffi = require('ffi')
vana.on('packet_in', function (e)
    if e.id == 0x0DF then
        local p = ffi.cast('uint8_t*', e.data_raw)
        local hp = ffi.cast('uint32_t*', p + 0x08)[0]
        local mp = ffi.cast('uint32_t*', p + 0x0C)[0]
    end
end)
```

Listening for packets costs every packet; keep the handler short and check `e.id` first. An addon that doesn't listen
isn't called.

## The last packet of an id

```lua
local packet, at = vana.packets.last(incoming, id)   -- the packet as a string, header first (as packet_in's data), and when it came (seconds); nil if none yet
```

`incoming` true for a packet the game received, false for one it sent. `at` is on `vana.time.clock()`'s scale, each
later packet later. So the login's zone-in is always to hand, without listening for it:

```lua
local zone_in = vana.packets.last(true, 0x00A)
```

## Sending a packet

```lua
vana.packets.inject(incoming, id, data)
```

Adds `data` (the whole packet: VanaCore fills in the header's id, size and sequence) to the next buffer the game
receives (`incoming` true) or sends. Any thread. An injected packet goes through the other addons' `packet_in` or
`packet_out` with `e.injected` true; an Ashita addon's own injected packet isn't shown back to it.

```lua
-- an outgoing event option (0x05B), 32 bytes
local p = string.rep('\0', 32)
vana.packets.inject(false, 0x05B, p)
```

The game's own outgoing queue, as Ashita's `QueuePacket` has it, for a packet the game should sequence itself:

```lua
vana.packets.queue(id, length, align, p1, p2, function (pointer)
    local p = ffi.cast('uint8_t*', pointer)   -- the header's id is set already; fill the rest
    p[4] = 1
end)   -- false when the game holds that id back
```

Mind what you send. The server treats an injected packet as the player's. VanaCore doesn't check it, and CatsEyeXI's
rules on automation apply.

## Zoning

Two helpers for the moments around a zone change, when the game's own structures are half there:

```lua
vana.game.zoning()               -- true between the zone-out (0x00B) and the player being back in the world
vana.game.player_zone(listed)    -- the zone the last zone-in named, until the party list (`listed`) has caught up
```

`vana.game.world().server_time` is the server's time from its last packets. The `zone_out` and `zone_change` events
([Events](02-events.md)) are raised from the same packets.

## Which addons see what

Plugins see each packet first (Ashita's `HandleIncomingPacket`), then addons in load order. An Ashita addon's own
injected packet is skipped for that addon, as Ashita does. The MCP server's `packets` tool shows the last 4,000 each
way for debugging ([Developing and debugging](14-dev-workflow.md)).
