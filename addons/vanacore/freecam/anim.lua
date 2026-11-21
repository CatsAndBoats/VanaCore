--[[
freecam\anim.lua: a character's animations for the free camera's photo mode: any clip of the race's played on your own
character, paused and moved frame by frame, on this screen alone (nothing goes to the server; others see you standing).

How (FFXiMain.dll, found 2026-11-12 from the PS2 client's names and tried in game; addresses are the file's):
- An entity's actor is [entity + 0xA0]; its first model part [actor + 0x674 + 0x44] holds seven motion layers (part + 0x24,
  0x14 each, the first track at +4). A character's clip is three parts on layers 0, 1 and 2 (lower body, upper body, waist),
  named by three letters and the layer's digit: idl0, idl1, idl2. Layer 3 is the eyes, 4 the mouth.
- A track (the PS2 client's KzMotionQue): +4 the next, +0x24 its frame, +0x28 its speed, +0x2C loops left (0: for ever),
  +0x30 the clip's name, +0x34 a handle to the clip. A clip (resource type 0x2B): +0x20 its name, +0x24 its type in the low 7
  bits (1 a folder, 0 a folder's end), +8 the next resource's handle, +0x28 its folder's handle, +0x34 frames, +0x36 its rate.
  A sixtieth of a second adds speed * rate / 2 to the frame. So a pause is speed 0, and a frame is written.
- XiSkeletonActor::SetSystemMotion (0x100CD670) queues a clip by its three letters on every layer it has a part for, from the
  actor's files: its race's own, and the list at actor + 0x7E4 (a node: +4 next, +0xC kind, +0x10 layer, +0x14 file id,
  +0x1C the file's handle). The game adds an emote's or a weapon skill's file to that list when it needs it; here the same
  calls do: the file by its id (0x10072EF0), AppendNewResp (0x100D4160), and once it has loaded (0x10070E70) AddMotRes
  (0x100D11A0). DeleteResp (0x100D41C0) takes it off.
- The game's own choice of motion (DefaultMove, 0x100C8110) queues again only when the motion it wants changes (standing to
  running) or the queue has ended, so a looping or paused clip holds while the character stands. actor + 0x7D8 is the motion
  it last queued: 0 there and it queues the idle again the next frame, which is Stop.

The functions are found by their code, which holds the offsets above: a client that has moved them finds nothing, and the
section is greyed. A track is checked against its clip (same name, type 0x2B) before anything is written.

Loaded by freecam.lua with loadfile; returns its table. animdata.lua (generated: tools\gen_freecam_anims.py) is the list.
--]]

local ffi = require('ffi')
local mem = vana.memory

local A = {}

local GAME = 'FFXiMain.dll'
local SIG = {
    entities = { '8B 56 0C 8B 04 2A 8B 04 85', 9 }, -- the entity list's address is its operand
    play = '83 EC 1C 53 55 56 8B F1 33 DB 57 8D 8E 74 06 00 00 89 5C 24 18 89 1D ?? ?? ?? ?? 89 5C 24 14 89 4C 24 1C E8',
    file = 'A1 ?? ?? ?? ?? 85 C0 74 04 8B 10 EB 02 33 D2 57 8B 7C 24 0C 85 D2 74 21 8B CA E8 ?? ?? ?? ?? 3B C2 75 05 39 7A 34 74',
    append = '56 57 6A 24 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 0D 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 75 07 5F 33 C0 5E C2 10 00',
    ready = '56 8B F1 8B 0D ?? ?? ?? ?? 8B 06 50 E8 ?? ?? ?? ?? 84 C0 75 02 5E C3 8B 06 85 C0 75 04 32 C0 5E C3 8B 00 85 C0 75 04 32 C0 5E C3',
    add = '8B 44 24 08 56 8B F1 3B 86 EC 09 00 00 75 ?? 8B 44 24 08 8B 08 8B 51 24 83 E2 7F 80 FA 01 75 08 50 8B CE E8 ?? ?? ?? ?? 8B B6 E4 07 00 00',
    drop = '53 8D 99 E4 07 00 00 56 8B 33 85 F6 74 ?? 55 8B 6C 24 10 57 8B 46 1C 8B 7E 04 3B E8 75',
}
local ACTOR, PART, LAYERS, LAYER_SIZE = 0xA0, 0x674 + 0x44, 0x24, 0x14
local WANTED, WAIST, FILES, STAMP = 0x7D8, 0x881, 0x7E4, 0x9EC
local KIND = 2 -- the list's kind for a file of ours: the game's own for a cutscene's motions
local CLIP = 0x2B

A.NAMES = { -- what some clips are (XI Model Viewer's names), by their three letters
    idl = 'Idle', btl = 'Battle stance', ded = 'Death', cor = 'Knocked out', std = 'Raise', wlk = 'Walk', run = 'Run',
    mvb = 'Move back', mvl = 'Move left', mvr = 'Move right', jmp = 'Jump', bow = 'Bow', poi = 'Point', lau = 'Laugh',
    wee = 'Weep', kne = 'Kneel',
}

local fn -- the game's calls once found; false: not in this client
local st = {
    actor = 0,    -- the actor the files below were added to
    files = {},   -- { id, handle (cdata uint32_t[1]), main (a file with the clips' lower and upper parts), added }
    entry = nil,  -- the list entry they are of
    retired = {}, -- the files of an entry no longer picked: taken off once no track plays from them (sweep)
    stem = nil,   -- the clip playing, its three letters
    paused = false, speed = 1, loop = true,
}

local function u32(at)
    return (at ~= nil and at ~= 0) and (mem.read_uint32(at) or 0) or 0
end

local function name_of(value)
    local out = {}
    for i = 0, 3 do
        local c = bit.band(bit.rshift(value, i * 8), 0xFF)
        out[#out + 1] = (c >= 32 and c < 127) and string.char(c) or '?'
    end
    return table.concat(out)
end

local function id_of(stem)
    local a, b, c = stem:byte(1, 3)
    return a + b * 0x100 + c * 0x10000 + 0x20000000
end

local function calls()
    if fn == nil then
        fn = false
        local at = {}
        for key, sig in pairs(SIG) do
            local pattern, offset = sig, 0
            if type(sig) == 'table' then
                pattern, offset = sig[1], sig[2]
            end
            at[key] = mem.find(GAME, 0, pattern, offset) or 0
            if at[key] == 0 then
                vana.log('freecam: animations: ' .. key .. " isn't in this client; the Animation section is off")
                return nil
            end
        end
        local manager = u32(at.ready + 5)
        local entities = u32(at.entities)
        if manager == 0 or entities == 0 then
            return nil
        end
        fn = {
            entities = entities, manager = manager,
            play = ffi.cast('char (__thiscall *)(void *, uint32_t, float, float, int, float, float, int, int, float, float, void *)', at.play),
            file = ffi.cast('uint32_t *(__thiscall *)(void *, uint32_t *, int)', at.file),
            append = ffi.cast('void *(__thiscall *)(void *, uint32_t, int, int, int)', at.append),
            ready = ffi.cast('char (__thiscall *)(uint32_t *)', at.ready),
            add = ffi.cast('void (__thiscall *)(void *, uint32_t, int)', at.add),
            drop = ffi.cast('void (__thiscall *)(void *, uint32_t)', at.drop),
        }
    end
    return fn or nil
end

-- your character's place in the entity list (the party list's first member's), asked for once a second
local index_at, index_was = -10, nil
local function my_index()
    local now = vana.time.clock()
    if now - index_at > 1 or now < index_at then
        index_at = now
        local party = vana.game.party()
        local me = type(party) == 'table' and type(party.members) == 'table' and party.members[1] or nil
        local index = type(me) == 'table' and me.active and tonumber(me.target_index) or nil
        index_was = (index ~= nil and index > 0) and index or nil
    end
    return index_was
end

-- your character's actor and its model part, or nil while there is none (a zone change, not drawn yet)
local function mine()
    local f = calls()
    local index = f ~= nil and my_index() or nil
    if index == nil then
        return nil
    end
    local entity = u32(f.entities + index * 4)
    local actor = u32(entity ~= 0 and entity + ACTOR or 0)
    local part = u32(actor ~= 0 and actor + PART or 0)
    if part == 0 then
        return nil
    end
    return actor, part
end

-- the tracks playing on the body's layers (0 to 2): { at, layer, stem, clip }; only those whose clip is what the track says
local function tracks(part)
    local out = {}
    for layer = 0, 2 do
        local at, guard = u32(part + LAYERS + layer * LAYER_SIZE + 4), 0
        while at ~= 0 and guard < 16 do
            local clip = u32(u32(at + 0x34))
            if clip ~= 0 and bit.band(u32(clip + 0x24), 0x7F) == CLIP and u32(clip + 0x20) == u32(at + 0x30) then
                out[#out + 1] = { at = at, layer = layer, stem = name_of(u32(at + 0x30)):sub(1, 3), clip = clip }
            end
            at, guard = u32(at + 4), guard + 1
        end
    end
    return out
end

-- the outermost folder a resource is in: its file
local function root_of(resource)
    for _ = 1, 16 do
        local parent = u32(u32(resource + 0x28))
        if parent == 0 or parent == resource then
            break
        end
        resource = parent
    end
    return resource
end

-- the clips in a file: [three letters] = { frames, rate }, the longest part's
local function clips_in(root, into)
    local at, depth, guard = u32(u32(root + 8)), 0, 0
    while at ~= 0 and guard < 20000 do
        guard = guard + 1
        local kind = bit.band(u32(at + 0x24), 0x7F)
        if kind == 1 then
            depth = depth + 1
        elseif kind == 0 then
            depth = depth - 1
            if depth < 0 then
                break
            end
        elseif kind == CLIP then
            local name = name_of(u32(at + 0x20))
            local stem, frames = name:sub(1, 3), mem.read_uint16(at + 0x34) or 0
            if name:match('^[%w_@]+[0-2]$') and (into[stem] == nil or frames > into[stem].frames) then
                into[stem] = { frames = frames, rate = mem.read_float(at + 0x36) or 0 }
            end
        end
        at = u32(u32(at + 8))
    end
    return into
end

local function ours(root)
    for _, list in ipairs({ st.files, st.retired }) do
        for _, f in ipairs(list) do
            if u32(f.handle[0]) == root then
                return true
            end
        end
    end
    return false
end

-- the files of ours still in the actor's list (the game takes a cutscene's off at its end, and all with a new look)
local function still_listed(actor, handle)
    local node, guard = u32(actor + FILES), 0
    while node ~= 0 and guard < 64 do
        if u32(node + 0x1C) == handle then
            return true
        end
        node, guard = u32(node + 4), guard + 1
    end
    return false
end

local function forget()
    st.files, st.retired, st.entry, st.stem, st.paused, st.actor = {}, {}, nil, nil, false, 0
end

-- the picked entry's files are no longer wanted
local function retire()
    for _, file in ipairs(st.files) do
        st.retired[#st.retired + 1] = file
    end
    st.files, st.entry = {}, nil
end

-- takes off the actor's list the retired files no track plays from (one that still is, a clip fading out, waits a frame)
local function sweep(actor, part)
    local f = calls()
    if f == nil or #st.retired == 0 or actor ~= st.actor then
        return
    end
    local used = {}
    for _, t in ipairs(tracks(part)) do
        used[root_of(t.clip)] = true
    end
    local keep = {}
    for _, file in ipairs(st.retired) do
        if used[u32(file.handle[0])] then
            keep[#keep + 1] = file
        elseif still_listed(actor, file.handle[0]) then
            f.drop(ffi.cast('void *', actor), file.handle[0])
        end
    end
    st.retired = keep
end

-- Your character's facing (radians, the game's: what vana.game.entity(...).heading says), read, or set with a number: the
-- game keeps it in five places (the entity's +0x18, +0x38 and +0xE4, the actor's +0x48 and +0xE8) and the model turns to it
-- over a few hundred milliseconds, while the character stands (the free camera holds it still). Nothing goes to the server.
local FACING = { { 'entity', 0x18 }, { 'entity', 0x38 }, { 'entity', 0xE4 }, { 'actor', 0x48 }, { 'actor', 0xE8 } }
function A.facing(value)
    local f = calls()
    local index = f ~= nil and my_index() or nil
    local entity = index ~= nil and u32(f.entities + index * 4) or 0
    local actor = u32(entity ~= 0 and entity + ACTOR or 0)
    if entity == 0 or actor == 0 then
        return nil
    end
    if value == nil then
        return mem.read_float(entity + 0xE4)
    end
    if value ~= value or value == math.huge or value == -math.huge then
        return nil
    end
    value = (value + math.pi) % (2 * math.pi) - math.pi
    for _, place in ipairs(FACING) do
        mem.write_float((place[1] == 'entity' and entity or actor) + place[2], value)
    end
    return value
end

-- the game's calls are in this client, and your character is there to pose
function A.ready()
    return calls() ~= nil and mine() ~= nil
end

-- the race's list (animdata.lua's R[race]()), given the table animdata.lua returned; nil for a race it hasn't
function A.list(data)
    local index = my_index()
    local entity = index ~= nil and vana.game.entity(index) or nil
    local race = type(entity) == 'table' and tonumber(entity.race) or nil
    if data == nil or race == nil or data[race] == nil then
        return nil
    end
    A.lists = A.lists or {}
    A.lists[race] = A.lists[race] or data[race]()
    return A.lists[race]
end

-- An entry of the list ({ name, file, waist1, waist2... }) loaded onto your character, in place of the last; nil: the race's
-- own motions (nothing to load). Its clips are there once A.loaded() says so.
function A.pick(entry)
    local f = calls()
    local actor, part = mine()
    if f == nil or actor == nil then
        return false
    end
    if st.actor ~= actor then
        forget()
    end
    if st.stem ~= nil then
        A.stop()
    end
    retire()
    st.actor, st.entry = actor, entry
    local waist = mem.read_uint8(actor + WAIST) or 0
    local have = {}
    for i = 2, entry ~= nil and #entry or 0, 3 do
        local wanted = { { entry[i], 1, true } }
        local companion = waist == 1 and entry[i + 1] or waist == 2 and entry[i + 2] or 0
        if companion ~= nil and companion ~= 0 then
            wanted[2] = { companion, 2, false }
        end
        for _, want in ipairs(wanted) do
            -- one it still has from the last pick is kept (taking a file off takes every listing of it)
            local kept
            for at, file in ipairs(st.retired) do
                if file.id == want[1] then
                    kept = table.remove(st.retired, at)
                    break
                end
            end
            if kept ~= nil then
                kept.main = want[3]
                st.files[#st.files + 1] = kept
                have[want[1]] = true
            elseif not have[want[1]] then
                have[want[1]] = true
                local handle = ffi.new('uint32_t[1]')
                f.file(ffi.cast('void *', f.manager), handle, want[1])
                if handle[0] ~= 0 then
                    f.append(ffi.cast('void *', actor), handle[0], want[2], want[1], KIND)
                    st.files[#st.files + 1] = { id = want[1], handle = handle, main = want[3], added = false }
                end
            end
        end
    end
    sweep(actor, part)
    return true
end

-- once a frame: a file that has finished loading is counted in; your character gone or made anew forgets what was added;
-- the last pick's files come off once nothing plays from them. Returns whether every file of this pick is in.
function A.loaded()
    local f = calls()
    local actor, part = mine()
    if f == nil or actor == nil then
        return false
    end
    if st.actor ~= 0 and st.actor ~= actor then
        forget()
    end
    local all = true
    for _, file in ipairs(st.files) do
        if not still_listed(actor, file.handle[0]) then
            forget()
            return false
        end
        if not file.added then
            if f.ready(file.handle) ~= 0 then
                f.add(ffi.cast('void *', actor), file.handle[0], mem.read_int32(actor + STAMP) or 0)
                file.added = true
            else
                all = false
            end
        end
    end
    sweep(actor, part)
    return all
end

-- the clips to pick from: { { stem, name, frames } }, A to Z; the picked entry's, or (none picked) the race's own
function A.clips()
    local actor, part = mine()
    if actor == nil then
        return {}
    end
    local found = {}
    if st.entry ~= nil then
        for _, file in ipairs(st.files) do
            local root = u32(file.handle[0])
            if file.main and file.added and root ~= 0 then
                clips_in(root, found)
            end
        end
    else
        local seen = {}
        local function take(root)
            if root ~= 0 and not seen[root] and not ours(root) then
                seen[root] = true
                clips_in(root, found)
            end
        end
        for _, t in ipairs(tracks(part)) do
            take(root_of(t.clip))
        end
        local node, guard = u32(actor + FILES), 0
        while node ~= 0 and guard < 64 do
            take(u32(u32(node + 0x1C)))
            node, guard = u32(node + 4), guard + 1
        end
    end
    local out = {}
    for stem, clip in pairs(found) do
        out[#out + 1] = { stem, A.NAMES[stem] ~= nil and (A.NAMES[stem] .. ' (' .. stem .. ')') or stem, clip.frames }
    end
    table.sort(out, function (a, b) return a[1] < b[1] end)
    return out
end

-- the playing clip's tracks: ours by name, the last queued on each layer
local function playing(part)
    local out = {}
    if st.stem ~= nil then
        for _, t in ipairs(tracks(part)) do
            if t.stem == st.stem then
                out[t.layer] = t
            end
        end
    end
    return out
end

-- plays a clip by its three letters from its start, looping or once (the game's idle comes back by itself after once)
function A.play(stem, loop, speed)
    local f = calls()
    local actor = mine()
    if f == nil or actor == nil or type(stem) ~= 'string' or #stem ~= 3 then
        return false
    end
    st.speed, st.loop = speed or st.speed, loop ~= false
    local queued = f.play(ffi.cast('void *', actor), id_of(stem), 1.0, st.speed, st.loop and 0 or 1, 8.0, 1.0, 0, 0, 0.0, 0.0, nil) == 0
    if queued then
        st.actor, st.stem, st.paused = actor, stem, false
    end
    return queued
end

-- { stem, frame, frames, rate, paused, speed, loop } of the clip playing (a frame is a key of the clip's; `rate` of them go
-- by in a thirtieth of a second at speed 1), or nil once it has ended or the game has moved on
function A.now()
    local actor, part = mine()
    if actor == nil or st.stem == nil then
        return nil
    end
    local t = playing(part)
    local lead = t[0] or t[1] or t[2]
    if lead == nil then
        st.stem, st.paused = nil, false
        return nil
    end
    return {
        stem = st.stem, frame = mem.read_float(lead.at + 0x24) or 0, frames = mem.read_uint16(lead.clip + 0x34) or 0,
        rate = mem.read_float(lead.clip + 0x36) or 0, paused = st.paused, speed = st.speed, loop = st.loop,
    }
end

local function write(offset, value)
    local actor, part = mine()
    if actor == nil then
        return
    end
    for _, t in pairs(playing(part)) do
        mem.write_float(t.at + offset, value)
    end
end

function A.pause(on)
    st.paused = on == true
    write(0x28, st.paused and 0 or st.speed)
end

-- for ever, or to the clip's end and back to the game's own motion
function A.loop(on)
    st.loop = on ~= false
    local actor, part = mine()
    if actor ~= nil then
        for _, t in pairs(playing(part)) do
            mem.write_int32(t.at + 0x2C, st.loop and 0 or 1)
        end
    end
end

-- a clip of ours is playing or held
function A.playing()
    return st.stem ~= nil
end

-- something of ours is on your character: a clip, or files added (A.loaded() is to be called each frame then)
function A.active()
    return st.stem ~= nil or #st.files > 0 or #st.retired > 0
end

function A.speed(value)
    st.speed = value
    if not st.paused then
        write(0x28, value)
    end
end

-- the frame every part is at (they are as long as each other); a frame set pauses
function A.frame(value)
    st.paused = true
    write(0x28, 0)
    write(0x24, value)
end

-- back to the game's own motion: it queues its idle again the next frame. The picked entry's files stay, to play from again.
function A.stop()
    local actor = mine()
    if actor ~= nil and actor == st.actor and st.stem ~= nil then
        write(0x28, 1)
        mem.write_uint32(actor + WANTED, 0)
    end
    st.stem, st.paused = nil, false
end

-- the free camera going off, the addon unloading: the game's own motion back, and the files off: at once those nothing
-- plays from, the rest as A.loaded() is called on (a clip fades out over a few frames). Left on a character's list, a file
-- only stays loaded until its look is next set up.
function A.off()
    local actor, part = mine()
    if actor == nil or actor ~= st.actor then
        forget()
        return
    end
    A.stop()
    retire()
    sweep(actor, part)
    if #st.retired == 0 then
        forget()
    end
end

return A
