--[[
freecam: the free camera's window, and a keyframe sequencer for camera shots. Loaded from its scripts\boot.txt line, as any addon is (taken out, it stays off).

The window opens with the free camera (/freecam) and closes with it again, unless you opened it yourself. The X in its title
turns the free camera off, and the window goes with it; the minus beside it hides the window, the free camera staying on, and
Escape shows it again (not while typing or with a game menu open). The commands the window's buttons stand for, for a bind of
your own:
  /freecam window       opens or closes the window
  /freecam key          records a keyframe at the camera's pose        (R while the free camera is on)
  /freecam play         plays the sequence from the playhead, or stops it  (P while the free camera is on)
  /freecam stop         stops it
  /freecam rewind       playhead back to the start
  /freecam seq <n|name> picks a sequence
  /freecam weather <name|0-19|off>  a weather on your screen only, for photos; off puts the zone's own back
  /freecam time <hour|hh:mm|noon|dusk|...|off>  an hour of the Vana'diel day on your screen only, held still; off: the zone's

The window is drawn as /vana's windows are (it loads vana\draw.lua for the palette, fonts, icons and controls, and is sized by
[vana] scale, times [freecam] scale). It has five tabs under its title, and opens on Sequencer:

  Save / Load  the sequences (saved as they change): which one Sequencer shows and plays (picked, it's loaded), its name, New
             and Delete
  Sequencer  the toolbar (floaty, the sequence's switches; a power icon that starts the free camera, while it's off) and the sequencer: a
             timeline of keyframes (the camera's position, yaw and tilt), a playhead, and Play, which flies the camera through
             them: a smooth curve through the keys, or straight lines, easing in and out at the ends. Keys are dragged to retime
             them, clicked to select; double-click one to put the camera there. Click or drag the ruler to scrub. The playhead
             and the keys land on every 0.25 s while Snap is on (Shift held, any time).
  View & Effects  a photo mode, a list of its sections down the left and the one picked beside it: View (FOV, Weather,
             Time, Facing, Nameplates, Far scenery, Grass follows); Animation (any of your race's animations played on your character, on this screen alone: a Category,
             an Animation in it, a Clip of that; play or pause, Loop, Stop, the Frame it's at and its Speed; it ends with
             the free camera: freecam\anim.lua, its list freecam\animdata.lua);
             /vana's Depth of field (on or off, Strength, Focus distance, Sharp zone)
             and Colour grade (on or off, Saturation, Contrast, Brightness, Warmth, Hue), changed for the free camera only: the user's
             own come back when it goes off (restore_fx); Graphics: Ambient occlusion, Shadows, Sun strength, Reflections and Bloom, each
             on or off and its strength, the same way. Each slider has a reset arrow.
  Settings   the camera's Lag, the sequence's Playback speed and Auto Key Increment (how far Add key moves the playhead on).
  Help       the keys, as keycaps beside what they do: the sequence's, the window's and flying's.

The corner at the bottom right resizes the window ([freecam] width and height, at 100%, kept as it's let go): wider, the
timeline is longer; taller, the timeline's lane takes the room. It starts 640 across, and goes down to 460 (MIN_W) and to what
the tab holds.

View & Effects' View has Far scenery (Scenery detail: Farther and Draw the whole zone in view together; both go back to what they
were when the free camera is turned off) and Grass follows (/vana's Mass Grass Follow free camera, [graphics] grass.freecam: the
grass grows round the free camera, not your character), Facing (your character turned on the spot, on your screen only, turned
back when the free camera goes off) and Nameplates (the names over heads on or off, put back as they were when it goes off). Depth
of field's Focus is Player (your character, wherever it is in the view), Centre (what's nearest at the middle of the view) or
Free (a distance of your own). The space bar in the
toolbar makes Space play and stop the sequence, in place of opening the chat line, while the free camera is on; Space is the game's
again when it goes off.

Hide UI plays with the interface hidden, as F11 does (and shows it again when the sequence ends or is stopped, unless you've
shown it yourself). The window goes with the rest of the interface, and the sequence goes on playing; stop it with P, or show the
interface with F11. R and P are binds that exist only while the free camera is on and the chat line is closed; [freecam] key.add
and key.play in vanacore.ini name other keys (or off), and a key you've already bound is left alone.

Weather, on View & Effects, shows any weather (rain, snow, fog, a sand storm, gloom...) on this screen alone, for photos:
freecam\weather.lua hands the game the server's weather packet as if it had come from the server, and holds the server's own back
while it's on. Nothing goes to the server. Zone's own (or /freecam weather off), the free camera going off and a zone change end
it; the first two put the zone's weather back. The game fades into each over 2.4 seconds (the zone's own changes take 10 s or more).

Time, under Weather, is a slider over the Vana'diel day: dragged, it holds the day at that hour on this screen alone, the sky,
sun, moon and light following as it moves, as a cutscene does (freecam\daytime.lua turns the game's timer off and sets its
Vana'diel clock). The time stands still till its reset arrow (or /freecam time off), the free camera going off or a zone
change, which bring the zone's own hour back at once.

Sequences are kept in settings\addons\freecam\sequences.json (a list of { name, mode, ends, loop, spacing, keys }, each key
{ t, x, y, z, yaw, tilt } with the angles in radians), so they can be copied between installs.
--]]

addon.name    = 'freecam'
addon.author  = 'Vekien'
addon.version = '1.0'
addon.desc    = 'The free camera window, and a keyframe sequencer for camera shots.'

local imgui = vana.imgui
local camera = vana.camera
if imgui == nil or camera == nil or camera.set_pose == nil then
    print('freecam needs a newer VanaCore.Core.dll (vana.camera.set_pose)')
    return
end

-------------------------------------------------------------------------------
-- The look: /vana's own drawing layer (vana\draw.lua: its palette, fonts, icons, switches and sliders), loaded into a table of
-- this addon's (every addon has a Lua state of its own), so the window is made of the same pieces as /vana's
-------------------------------------------------------------------------------

local kit = { st = { anim = {}, dt = 0, last = 0 } }
do
    local chunk, err = loadfile(vana.paths.addons .. '\\vanacore\\vana\\draw.lua')
    if chunk == nil then
        print('freecam: ' .. tostring(err))
        return
    end
    local ok, why = pcall(chunk, kit)
    if not ok then
        print('freecam: vana\\draw.lua: ' .. tostring(why))
        return
    end
end

local S, P, fonts = kit.S, kit.P, kit.fonts
local BODY, SMALL = kit.BODY, kit.SMALL

-- Weather's packets (freecam\weather.lua: a weather on this screen alone, held till it's turned off); nil if it didn't load, the
-- row then greyed
local weather
do
    local chunk, err = loadfile(vana.paths.addons .. '\\vanacore\\freecam\\weather.lua')
    local ok, got = false, err
    if chunk ~= nil then
        ok, got = pcall(chunk)
    end
    if ok and type(got) == 'table' then
        weather = got
    else
        print('freecam: weather.lua: ' .. tostring(got))
    end
end

-- Time's clock (freecam\daytime.lua: an hour of the day on this screen alone, held till it's turned off); nil if it didn't load
local daytime
do
    local chunk, err = loadfile(vana.paths.addons .. '\\vanacore\\freecam\\daytime.lua')
    local ok, got = false, err
    if chunk ~= nil then
        ok, got = pcall(chunk)
    end
    if ok and type(got) == 'table' then
        daytime = got
    else
        print('freecam: daytime.lua: ' .. tostring(got))
    end
end

-- Animation's engine (freecam\anim.lua: a clip of the race's played on your character, paused and moved by the frame) and its
-- list (freecam\animdata.lua: every race's animations by category); nil if either didn't load, the section then greyed
local anim, animdata
do
    local loaded = {}
    for _, name in ipairs({ 'anim', 'animdata' }) do
        local chunk, err = loadfile(vana.paths.addons .. '\\vanacore\\freecam\\' .. name .. '.lua')
        local ok, got = false, err
        if chunk ~= nil then
            ok, got = pcall(chunk)
        end
        if ok and type(got) == 'table' then
            loaded[name] = got
        else
            print('freecam: ' .. name .. '.lua: ' .. tostring(got))
        end
    end
    if loaded.anim ~= nil and loaded.animdata ~= nil then
        anim, animdata = loaded.anim, loaded.animdata
    end
end

-- icons this window uses that /vana's list (kit.FA) hasn't: Font Awesome's code points, by name
for name, code in pairs({
    wind = 0xF72E, loop = 0xF363, play = 0xF04B, stop = 0xF04D, rewind = 0xF048, pin = 0xF3C5, curve = 0xF55B, route = 0xF4D7,
    clock = 0xF017, hourglass = 0xF252, locate = 0xF601, video = 0xF03D, film = 0xF008, mouse = 0xF8CC, ease = 0xF1FE,
    cloud_sun = 0xF6C4, bullseye = 0xF140, crosshairs = 0xF05B, arrows_h = 0xF07E, droplet = 0xF043, fire = 0xF06D,
    water = 0xF773, lightbulb = 0xF0EB, pause = 0xF04C, person = 0xF183, list = 0xF03A,
}) do
    kit.FA[name] = code
end

-- Segoe UI as /vana has it, the two cuts this window uses (the icons are ImGui's own font). Added in the first frame the window
-- draws, used from the next: ImGui takes a new font up between frames.
function kit.load_fonts()
    local dir = ((os and os.getenv and os.getenv('WINDIR')) or 'C:\\Windows') .. '\\Fonts\\'
    fonts.icons = imgui.GetFont()
    fonts.text = imgui.AddFontFromFileTTF(dir .. 'segoeui.ttf', 18) or fonts.icons
    fonts.semi = imgui.AddFontFromFileTTF(dir .. 'seguisb.ttf', 18) or fonts.text
end

-- as big as /vana's window ([vana] scale, a percent), times [freecam] scale (a percent too, default 100)
local function user_percent()
    local vana_scale = tonumber(vana.config.get('vana', 'scale')) or 100
    local own = tonumber(vana.config.get('freecam', 'scale')) or 100
    return vana_scale * own / 100
end

-------------------------------------------------------------------------------
-- State
-------------------------------------------------------------------------------

local DEG = 180 / math.pi
local TWO_PI = 2 * math.pi
local MIN_GAP = 0.01 -- seconds between two keys, at the least

local state = {
    open = false,
    reported = {},
    head = 0,                -- the playhead, seconds
    playing = false,
    pending_play = false,    -- play asked for with the free camera off: it starts once the camera has its pose
    rate = 1.0,              -- playback speed
    last = 0,                -- the clock at the last playing frame
    scrub = false,           -- the playhead is being dragged: the camera follows it
    driving = false,         -- the camera is held and set from the sequence this frame
    selected = 1,
    drag = nil,              -- { kind = 'head' | 'key', index, span, offset } while the mouse drags in the timeline
    status = '', status_until = 0,
    lag_value = nil, fov_value = nil, -- the sliders' values while they're held
    key_time = { 0 },
    name_buf = { '' }, name_for = nil,
    confirm_delete = 0,      -- clock until which the Delete button asks "Sure?"
    typing = false,          -- an ImGui text box has the keyboard (a key bind's /freecam key then waits)
    bound = {},              -- the binds this addon made, for the free camera's time
    binds_on = false,
    auto = false,            -- the window was opened by the free camera turning on (/freecam), so it closes when it goes off
    quiet_on = false,        -- the camera is being turned on by Play: the window stays as it is
    we_hid = false,          -- Play hid the interface (Hide UI): it's shown again when the sequence stops
    scenery = nil,           -- { lod, full }: [graphics] lod and drawdistance.full as they were before the tree was pressed
    hidden = false,          -- the window hidden by its minus, the free camera on: still open, not drawn; Escape shows it again
    hide_told = false,       -- the chat log has said so, once a game
    shown = false,           -- the window was open at the last frame (opened, it's on Sequencer and not hidden)
    size = nil,              -- { w, h }: the window's size dragged at its resize corner, at 100% ([freecam] width, height); h 0: fit
    grab = nil,              -- { x, y }: the resize corner held, the mouse's distance from the window's corner
    view_section = 'view',   -- View & Effects' section shown: 'view', 'anim', 'dof', 'grade' or 'graphics'
    -- Animation: the category picked (0: Basic, the race's own motions), the animation in it (0 in Basic), the clip (its
    -- three letters; '': the game's own), Loop, Speed, and whether the animation's files have loaded (asked each frame)
    anim_cat = 0, anim_entry = 0, anim_stem = '', anim_loop = true, anim_speed = 1, anim_loaded = true,
    tab = 'sequencer',       -- the tab shown: 'files' (Save / Load), 'sequencer', 'view', 'settings' or 'help'; Sequencer each time it opens
}

local function fail(where, err)
    local message = where .. ': ' .. tostring(err)
    if not state.reported[message] then
        state.reported[message] = true
        vana.log(message)
    end
end

-- a line in the window, or in the chat log if the window is shut or hidden
local function say(text)
    state.status, state.status_until = text, vana.time.clock() + 6
    if not state.open or state.hidden then
        print('Free camera: ' .. text)
    end
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- an angle in [-pi, pi)
local function wrap(angle)
    return angle - TWO_PI * math.floor((angle + math.pi) / TWO_PI)
end

-------------------------------------------------------------------------------
-- The sequences
-------------------------------------------------------------------------------

local store = vana.settings.register({
    name = 'sequences',
    defaults = { current = 1, sequences = {}, hide_ui = false, snap = true, help = false, space_play = false },
})
local data = store.data

-- a time put on the timeline's grid (every 0.25 s) while Snap is on and `free` (Shift held) isn't, else to the hundredth
local SNAP = 0.25

local function snap_time(t, free)
    if data.snap ~= false and not free then
        return math.floor(t / SNAP + 0.5) * SNAP
    end
    return math.floor(t * 100 + 0.5) / 100
end

local version = 0 -- bumped by every change to a sequence's keys or options: the curve is built again

local function touch()
    version = version + 1
end

local function save()
    local ok, why = store:save()
    if not ok then
        fail('saving sequences.json', why)
    end
end

local function new_sequence(name)
    return { name = name, mode = 'spline', ends = true, loop = false, spacing = 2.0, keys = {} }
end

-- what a file can lack (an older one, a hand edit)
local function fix(seq)
    if type(seq) ~= 'table' then
        return new_sequence('Sequence')
    end
    if type(seq.keys) ~= 'table' then seq.keys = {} end
    if type(seq.name) ~= 'string' then seq.name = 'Sequence' end
    if seq.mode ~= 'linear' then seq.mode = 'spline' end
    if type(seq.ends) ~= 'boolean' then seq.ends = true end
    if type(seq.loop) ~= 'boolean' then seq.loop = false end
    if type(seq.spacing) ~= 'number' or seq.spacing ~= seq.spacing then seq.spacing = 2.0 end
    for i = #seq.keys, 1, -1 do
        local k = seq.keys[i]
        if type(k) ~= 'table' or type(k.t) ~= 'number' or type(k.x) ~= 'number' or type(k.y) ~= 'number'
            or type(k.z) ~= 'number' or type(k.yaw) ~= 'number' or type(k.tilt) ~= 'number' then
            table.remove(seq.keys, i)
        end
    end
    return seq
end

local function sequences()
    if type(data.sequences) ~= 'table' then
        data.sequences = {}
    end
    if #data.sequences == 0 then
        data.sequences[1] = new_sequence('Sequence 1')
    end
    return data.sequences
end

local fixed = {} -- sequences already through fix(), by identity

local function current()
    local list = sequences()
    local n = math.floor(tonumber(data.current) or 1)
    if n < 1 or n > #list then
        n = 1
    end
    data.current = n
    if not fixed[list[n]] then
        list[n] = fix(list[n])
        fixed[list[n]] = true
    end
    return list[n], n
end

local function sort_keys(seq)
    local keys = seq.keys
    table.sort(keys, function (a, b) return a.t < b.t end)
    for i = 2, #keys do
        if keys[i].t < keys[i - 1].t + MIN_GAP then
            keys[i].t = keys[i - 1].t + MIN_GAP
        end
    end
end

local function index_of(seq, key)
    for i, k in ipairs(seq.keys) do
        if k == key then
            return i
        end
    end
    return 0
end

-------------------------------------------------------------------------------
-- The curve: x, y, z, yaw (unwrapped, so it turns the short way round) and tilt, each through the keys by time
-------------------------------------------------------------------------------

local curve = { seq = nil, version = -1, n = 0, t = {}, v = {}, m = {} }
local out = { 0, 0, 0, 0, 0 }

local function build(seq)
    local list = {}
    for i, k in ipairs(seq.keys) do
        list[i] = k
    end
    table.sort(list, function (a, b) return a.t < b.t end)
    local n = #list
    local t, v, m = {}, { {}, {}, {}, {}, {} }, { {}, {}, {}, {}, {} }
    local yaw_before
    for i, k in ipairs(list) do
        local at = k.t
        if i > 1 and at < t[i - 1] + MIN_GAP then
            at = t[i - 1] + MIN_GAP
        end
        t[i] = at
        v[1][i], v[2][i], v[3][i], v[5][i] = k.x, k.y, k.z, k.tilt
        local yaw = k.yaw
        if yaw_before ~= nil then
            yaw = yaw_before + wrap(yaw - yaw_before)
        end
        v[4][i] = yaw
        yaw_before = yaw
    end
    -- a Catmull-Rom spline through the keys at their times (each key's slope the one between its neighbours'); the ends
    -- rest, or run straight on, as the sequence says. Straight lines need no slopes.
    local spline = seq.mode ~= 'linear' and n >= 2
    for c = 1, 5 do
        local vc, mc = v[c], m[c]
        for i = 1, n do
            if not spline then
                mc[i] = 0
            elseif i == 1 then
                mc[i] = seq.ends and 0 or (vc[2] - vc[1]) / (t[2] - t[1])
            elseif i == n then
                mc[i] = seq.ends and 0 or (vc[n] - vc[n - 1]) / (t[n] - t[n - 1])
            else
                mc[i] = (vc[i + 1] - vc[i - 1]) / (t[i + 1] - t[i - 1])
            end
        end
    end
    curve.seq, curve.version, curve.n, curve.t, curve.v, curve.m = seq, version, n, t, v, m
    curve.spline = spline
end

local function ready(seq)
    if curve.seq ~= seq or curve.version ~= version then
        build(seq)
    end
end

local function duration(seq)
    ready(seq)
    return curve.n > 0 and curve.t[curve.n] or 0
end

-- the camera's x, y, z, yaw, tilt at `at` seconds; nil without keys. Before the first key and after the last it stays on them.
local function sample(seq, at)
    ready(seq)
    local n = curve.n
    if n == 0 then
        return nil
    end
    local t, v, m = curve.t, curve.v, curve.m
    if n == 1 or at <= t[1] then
        return v[1][1], v[2][1], v[3][1], v[4][1], v[5][1]
    end
    if at >= t[n] then
        return v[1][n], v[2][n], v[3][n], v[4][n], v[5][n]
    end
    local i = 1
    while i < n - 1 and at >= t[i + 1] do
        i = i + 1
    end
    local dt = t[i + 1] - t[i]
    local s = (at - t[i]) / dt
    if curve.spline then
        local s2 = s * s
        local s3 = s2 * s
        local h00, h10, h01, h11 = 2 * s3 - 3 * s2 + 1, s3 - 2 * s2 + s, -2 * s3 + 3 * s2, s3 - s2
        for c = 1, 5 do
            out[c] = h00 * v[c][i] + h10 * dt * m[c][i] + h01 * v[c][i + 1] + h11 * dt * m[c][i + 1]
        end
    else
        for c = 1, 5 do
            out[c] = v[c][i] + (v[c][i + 1] - v[c][i]) * s
        end
    end
    return out[1], out[2], out[3], out[4], out[5]
end

-------------------------------------------------------------------------------
-- Recording and playing
-------------------------------------------------------------------------------

local function apply_head(seq)
    local x, y, z, yaw, tilt = sample(seq, state.head)
    if x ~= nil then
        camera.set_pose(x, y, z, yaw, tilt)
    end
end

local function go_to(key)
    return camera.set_pose(key.x, key.y, key.z, key.yaw, key.tilt)
end

local function add_key()
    local x, y, z, yaw, tilt = camera.pose()
    if x == nil then
        say('Turn the free camera on first: a key records where the camera is.')
        return false
    end
    local seq = current()
    local at = snap_time(math.max(0, state.head), false)
    local key
    for _, k in ipairs(seq.keys) do
        if math.abs(k.t - at) < 0.02 then
            key = k
            break
        end
    end
    local added = key == nil
    if added then
        key = { t = at }
        seq.keys[#seq.keys + 1] = key
    end
    key.x, key.y, key.z, key.yaw, key.tilt = x, y, z, yaw, tilt
    if added then
        sort_keys(seq)
        state.head = key.t + seq.spacing -- the next key goes after this one
    end
    state.selected = index_of(seq, key)
    touch()
    save()
    say(string.format(added and 'Key %d recorded at %.2f s.' or 'Key %d recorded again at %.2f s.', state.selected, key.t))
    return true
end

-- Hide UI: Play hides the interface as F11 does (the window goes with it), and shows it again once the sequence is over, unless
-- the player has shown it already
local function hide_interface()
    if data.hide_ui and not state.we_hid and vana.ui.interface_hidden ~= nil and not vana.ui.interface_hidden() then
        state.we_hid = vana.ui.set_interface_hidden(true)
    end
end

local function show_interface()
    if state.we_hid then
        state.we_hid = false
        if vana.ui.interface_hidden() then
            vana.ui.set_interface_hidden(false)
        end
    end
end

local function begin_play()
    local seq = current()
    if state.head >= duration(seq) - 0.001 then
        state.head = 0
    end
    state.playing = true
    state.pending_play = false
    state.last = vana.time.clock()
    hide_interface()
end

local function stop_play()
    state.playing = false
    state.pending_play = false
    show_interface()
end

local function toggle_play()
    if state.playing or state.pending_play then
        stop_play()
        say('Stopped.')
        return
    end
    local seq = current()
    if #seq.keys < 2 then
        say('Record at least two keys first.')
        return
    end
    local camera_state = camera.state()
    if not camera_state.on then
        state.quiet_on = true
        local ok, why = camera.free(true)
        if not ok then
            state.quiet_on = false
            say("The free camera couldn't turn on: " .. tostring(why))
            return
        end
        state.pending_play = true -- once the camera has taken its pose from the game's
        return
    end
    begin_play()
end

local function rewind()
    stop_play()
    state.head = 0
    if camera.state().have then
        apply_head(current())
    end
end

-- Once a frame, window or not: playback, the camera held while the sequence sets it, the keys
local function update(now)
    local cs = camera.state()
    local seq = current()
    if state.pending_play then
        if not cs.on then
            state.pending_play = false
        elseif cs.have then
            begin_play()
        end
    end
    local drive = false
    if state.playing then
        if not cs.have then
            stop_play() -- the free camera went off (a zone)
        else
            state.head = state.head + (now - state.last) * state.rate
            state.last = now
            local length = duration(seq)
            if state.head >= length then
                if seq.loop and length > 0 then
                    state.head = state.head % length
                else
                    state.head = length
                    stop_play()
                end
            end
            drive = true -- (the last frame too)
        end
    end
    if state.scrub and cs.have then
        drive = true
    end
    if drive then
        if not cs.hold then
            camera.hold(true)
        end
        apply_head(seq)
    elseif state.driving then
        camera.hold(false)
    end
    state.driving = drive
end

-------------------------------------------------------------------------------
-- The keys R and P: binds that exist while the free camera is on
-------------------------------------------------------------------------------

local BIND_ACTIONS = {
    { 'key.add', 'r', '/freecam key' },
    { 'key.play', 'p', '/freecam play' },
}

local function key_setting(name, default)
    local value = tostring(vana.config.get('freecam', name, default) or default):lower():gsub('%s', '')
    if value == '' or value == 'off' or value == 'none' or value == '0' then
        return nil
    end
    return value
end

-- a bind of the player's already on this key, bare (or only while the chat line is closed)
local function bound_already(key)
    for _, b in ipairs(vana.binds.list()) do
        if b.key:lower() == key and not (b.ctrl or b.alt or b.shift or b.win or b.apps) and not b.up and b.when ~= 'open' then
            return true
        end
    end
    return false
end

-- Space plays and stops the sequence (the toolbar's switch), for the free camera's time as R and P are. A bind, so the game doesn't
-- see the key, and the chat line it opens stays shut; typing in the chat line is left alone (the % bind waits while it's open).
-- It's one of state.bound, so it goes with the rest when the camera goes off: Space is the game's again. False when a bind of the
-- player's own already has Space (it's left alone).
local SPACE = '%space'

local function set_space_bind(want)
    local at
    for i, spec in ipairs(state.bound) do
        if spec == SPACE then
            at = i
        end
    end
    if want and at == nil then
        if bound_already('space') then
            return false
        end
        if vana.binds.add(SPACE, '/freecam play') then
            state.bound[#state.bound + 1] = SPACE
        end
    elseif not want and at ~= nil then
        vana.binds.remove(SPACE)
        table.remove(state.bound, at)
    end
    return true
end

local function set_binds(on)
    state.binds_on = on
    if on then
        for _, action in ipairs(BIND_ACTIONS) do
            local key = key_setting(action[1], action[2])
            if key ~= nil and not bound_already(key) then
                local spec = '%' .. key
                if vana.binds.add(spec, action[3]) then
                    state.bound[#state.bound + 1] = spec
                end
            end
        end
        set_space_bind(data.space_play == true)
    else
        for _, spec in ipairs(state.bound) do
            vana.binds.remove(spec)
        end
        state.bound = {}
    end
end

-------------------------------------------------------------------------------
-- Drawing: on the window's draw list, as /vana's windows are. The window's height is needed before it's drawn, so each part
-- says how tall it is (a *_height function beside its draw one); a part that ends elsewhere than it said is logged once.
-------------------------------------------------------------------------------

local WIN_W, PAD, TITLE_H, HEAD_H, ROW_H = 640, 18, 40, 30, 30
local MIN_W = 460 -- the narrowest the resize corner takes it: the toolbar's icons still fit; the title's numbers, the key's
                  -- buttons, Weather's tags and Save / Load's list make room
local NOTE_LINE, NOTE_INSET = 18, 22  -- a line of wrapped text, and the icon's column before it
local TL_RULER, TL_LANE = 22, 58      -- the timeline's ruler and the lane under it
local COL_GAP = 24                    -- between the two halves of a row
local NOTE_SIZE = BODY - 1
local TABS_H = 40                     -- the tabs under the title: Save / Load, Sequencer (the window opens on it), View & Effects, Settings, Help
local TAB_IDS = { 'files', 'sequencer', 'view', 'settings', 'help' }
local TAB_LABELS = { 'Save / Load', 'Sequencer', 'View & Effects', 'Settings', 'Help' }
local LABEL_GAP = 28                  -- after the longest name on a tab, before the controls (label_col)
local TAG_W = 84                      -- right of Settings' sliders: Time's ZONE while an hour is held
local VALUE_W = 52                    -- a Settings slider's value
local GRIP = 18                       -- the resize corner, bottom right (in the window's padding)

local TIPS = {
    exit = 'Exit the free camera: turns it off, puts the game\'s camera back and closes this window.',
    close = 'Closes this window.',
    hide = 'Hide this window: the free camera stays on. Press the ESCAPE key to restore it.',
    start = 'Detaches the camera from your character and flies it about (/freecam).',
    float = 'Floaty: the camera glides to a stop when you let go of the keys, and turns with some lag, as though on a spring. Off, it moves and turns at once.',
    lag = 'How long the glide takes. Floaty only.',
    fov = 'Field of view: how wide the view is, in degrees; higher shows more.',
    fov_off = 'Field of view needs Use true zoom (/vana, Options, Camera): without it the game\'s zoom is the field of view, and the free camera\'s wheel sets its speed instead.',
    hide_ui = 'Hide UI while playing: hides the interface when Play starts, as F11 does, and shows it again when the sequence ends or is stopped. P stops it; F11 shows the interface.',
    smooth = 'Smooth curve: a curve through the keys, turning the short way round. Off, straight lines from one key to the next.',
    ease = 'Ease in and out: rests at the first and last key, and speeds up and slows down between. Off, the path runs straight on past them. Smooth curve only.',
    loop = 'Loop: starts again from the first key when it gets to the last.',
    snap = 'Snap to 0.25 s: the playhead and the keys land on every 0.25 s as they are dragged (Add key too). Hold Shift to place one at any time.',
    scenery = 'Far scenery: turns on Scenery detail: Farther (buildings and trees keep their detailed meshes farther out) and Draw the whole zone in view (/vana, Graphics). Both go back to what they were when the free camera is turned off. The whole zone needs Extended draw distance on and a Scenery distance above 1x.',
    grass = 'Grass follows the free camera: while it\'s on, Mass Grass grows round the camera instead of your character (/vana, Experimental, Mass Grass: Follow free camera). Off, the grass stays round your character.',
    rate = 'Playback: how fast the sequence plays, 0.25 to 4 times (1 is as recorded).',
    space = 'Space plays the sequence and stops it, while the free camera is on, instead of opening the chat line (typing in the chat line is not touched). Space is the game\'s again when the free camera is turned off.',
    spacing = 'Auto Key Increment: how far the playhead moves on after Add key (or R), so the next key lands later. This sequence\'s own.',
    resize = 'Drag to resize',
    load = 'The sequence the Sequencer tab shows and plays: pick another to load it (/freecam seq <number or name>).',
    name = 'This sequence\'s name, saved as you leave the box.',
    new = 'Starts a new, empty sequence and loads it.',
    seq_label = 'The sequence shown. Click to open Save / Load and pick another.',
    dof = 'Depth of field: blurs what is nearer or farther than the focus. For the free camera only: yours comes back when it goes off.',
    grade = 'Colour grade: Saturation, Contrast, Brightness, Warmth and Hue. For the free camera only: yours comes back when it goes off.',
    fx_off = "Needs VanaCore's effects running: /vana, Graphics > Effects (Direct3D 11 or 12).",
    facing = 'Facing: turns your character on the spot, for a photo. On your screen only: it turns back when the free camera goes off.',
    nameplates = 'Nameplates: the names over heads, on or off. Put back as they were when the free camera goes off.',
    focus = 'Focus: what stays sharp.',
    focus1 = 'Player: your character stays sharp, wherever it is in the view.',
    focus2 = 'Centre: whatever is nearest at the middle of the view stays sharp. Point the camera at it.',
    focus3 = 'Free: a distance of your own from the camera, set on Focus distance.',
    focus_distance = 'Focus distance: how far from the camera stays sharp. Free focus only.',
    anim = 'Animation: plays any of your race\'s animations on your character, to pose it for a photo. On your screen only: other players see you standing. Moving your character ends it.',
    anim_off = "Animations can't be played in this client, or your character isn't there yet (vanacore.log says which).",
    anim_cat = 'The kind of animation: Basic is what your character has loaded (standing, walking, sitting...), the rest are loaded as you pick them.',
    anim_entry = 'The animation: an emote, a weapon skill, a battle stance... Its clips are listed under it once it has loaded.',
    anim_clip = "The clip to play. Game's own puts your character back as the game has it.",
    anim_frame = 'The frame shown, in thirtieths of a second. Dragging it pauses the clip there.',
    anim_speed = 'How fast the clip plays (1 is the game\'s own speed).',
    anim_play = 'Play or pause the clip.',
    anim_loop = 'Loop: the clip plays over and over. Off, it plays once and your character goes back to standing.',
    anim_stop = "Stop: your character back as the game has it.",
    dx12_only = 'Needs the Direct3D 12 renderer.',
    weather = "Weather: any weather on your screen only, for photos. Nothing goes to the server: other players see the zone's own, "
        .. "and the zone's changes wait till you pick Zone's own, turn the free camera off or change zones. The list has the "
        .. "weathers this zone's files have a look for; /freecam weather rain (a name, or 0 to 19) shows any other, which may "
        .. "look like clear skies here, or like another. The game fades into it over 2.4 seconds. "
        .. "/freecam weather off puts the zone's own back.",
    weather_now = "Now: the weather the game has. Zone: the zone's own while another is shown, put back when it ends.",
    daytime = "Time: drag to any hour of the Vana'diel day. The sky, the sun and moon and the light follow at once and stay there, "
        .. "on your screen only, as in a cutscene; nothing goes to the server. The reset arrow, turning the free camera off or "
        .. "changing zones brings the zone's own hour back at once. /freecam time 18 (an hour, hh:mm, noon, dusk...) and "
        .. '/freecam time off type it.',
    daytime_now = "Zone: the zone's own hour, while another is held.",
}

-- a note's words wrapped to a width: by size, width and text (the window measures the same ones every frame)
local wrapped, wrapped_n, wrapped_scale = {}, 0, nil

local function wrap_text(str, room, size)
    if wrapped_scale ~= kit.scale or wrapped_n > 300 then
        wrapped, wrapped_n, wrapped_scale = {}, 0, kit.scale
    end
    local key = size .. '|' .. math.floor(room) .. '|' .. str
    local lines = wrapped[key]
    if lines ~= nil then
        return lines
    end
    lines = {}
    local line = ''
    local function put(word)
        local try = line == '' and word or line .. ' ' .. word
        if line ~= '' and kit.measure(try, fonts.text, size) > room then
            lines[#lines + 1] = line
            line = word
        else
            line = try
        end
    end
    for word in str:gmatch('%S+') do
        if kit.measure(word, fonts.text, size) > room then
            -- a word too long for a line alone is broken where it runs out of room
            local piece = ''
            for letter in word:gmatch('[%z\1-\127\194-\244][\128-\191]*') do
                if piece ~= '' and kit.measure(piece .. letter, fonts.text, size) > room then
                    put(piece)
                    piece = letter
                else
                    piece = piece .. letter
                end
            end
            word = piece
        end
        put(word)
    end
    if line ~= '' then
        lines[#lines + 1] = line
    end
    if #lines == 0 then
        lines[1] = ''
    end
    wrapped[key] = lines
    wrapped_n = wrapped_n + 1
    return lines
end

local function note_height(w, str)
    return #wrap_text(str, w - S(NOTE_INSET), NOTE_SIZE) * S(NOTE_LINE) + S(10)
end

-- an icon, then the words wrapped over as many lines as it takes; returns the height
local function draw_note(dl, x, y, w, icon, str, icon_colour, text_colour)
    local lines = wrap_text(str, w - S(NOTE_INSET), NOTE_SIZE)
    local ly = y + S(5) + S(NOTE_LINE) / 2
    kit.icon_centred(dl, x + S(8), ly, icon, icon_colour or P.dim, 10)
    for _, line in ipairs(lines) do
        kit.text_mid(dl, x + S(NOTE_INSET), ly, line, text_colour or P.text2, fonts.text, NOTE_SIZE)
        ly = ly + S(NOTE_LINE)
    end
    return #lines * S(NOTE_LINE) + S(10)
end

-- A small label beside its value (the title's X Y Z YAW TILT SPEED, Weather's NOW and ZONE): its top, so its letters stand on
-- the value's baseline when the value is drawn centred on `cy` (kit.text_mid). Centred on its own, the smaller label sat on a
-- line of its own, a pixel or two off its value's. Each font's baseline is its H's bottom in its line (ImGui's baked glyph);
-- the line's lift is draw.lua's text_mid's.
local baselines, baselines_scale = {}, nil
local function baseline(font, size)
    if baselines_scale ~= kit.scale then
        baselines, baselines_scale = {}, kit.scale
    end
    local by_font = baselines[font]
    if by_font == nil then
        by_font = {}
        baselines[font] = by_font
    end
    local b = by_font[size]
    if b == nil then
        kit.push(font, size)
        local ok, y1 = pcall(function () return imgui.GetFontBaked():FindGlyph(72).Y1 end)
        kit.pop()
        b = (ok and type(y1) == 'number') and y1 or false
        by_font[size] = b
    end
    return b or nil
end

local function label_top(cy, label, label_font, label_size, value, value_font, value_size)
    local _, vh = kit.measure(value, value_font, value_size)
    local value_top = cy - vh / 2 - math.floor(vh * 0.06 + 0.5)
    local vb, lb = baseline(value_font, value_size), baseline(label_font, label_size)
    if vb == nil or lb == nil then -- (no glyphs to measure: centred, as before)
        local _, lh = kit.measure(label, label_font, label_size)
        return cy - lh / 2 - math.floor(lh * 0.06 + 0.5)
    end
    return math.floor(value_top + vb - lb + 0.5)
end

local function shift_down()
    local got, io = pcall(imgui.GetIO)
    return got and io ~= nil and io.KeyShift == true
end

-------------------------------------------------------------------------------
-- Controls
-------------------------------------------------------------------------------

local TOOL_W, TOOL_H = 30, 26 -- a toolbar icon
local BTN_H = 24              -- every button's height, as the drop-down's, the steppers' and the boxes' (kit's are 24)

-- an icon alone, no box, that toggles: the accent's blue while it's on, grey while it isn't (lighter under the mouse). Returns
-- whether it was pressed.
local function icon_toggle(id, x, cy, icon, on, enabled, tip)
    local w, h = S(TOOL_W), S(TOOL_H)
    local y = cy - h / 2
    local hovered, clicked = kit.hit(id, x, y, w, h)
    local dl = imgui.GetWindowDrawList()
    local alpha = enabled and 1 or 0.4
    local lit = kit.ease(id, on and 1 or 0)
    local over = kit.ease(id .. '.over', (hovered and enabled) and 1 or 0, 22)
    local grey = kit.mix(P.faint, P.text2, over)
    kit.icon_centred(dl, x + w / 2, cy, icon, kit.mix(grey, kit.mix(P.accent, P.accent2, over), lit), 14, alpha)
    if hovered and tip ~= nil then
        kit.tooltip(tip)
    end
    return clicked and enabled
end

-- an icon alone that acts when it's pressed, `colour` (`hover` under the mouse): exit and delete. Returns whether it was pressed.
local function icon_press(id, x, cy, icon, colour, hover, tip)
    local w, h = S(TOOL_W), S(TOOL_H)
    local hovered, clicked = kit.hit(id, x, cy - h / 2, w, h)
    local over = kit.ease(id .. '.over', hovered and 1 or 0, 22)
    kit.icon_centred(imgui.GetWindowDrawList(), x + w / 2, cy, icon, kit.mix(colour, hover, over), 14)
    if hovered and tip ~= nil then
        kit.tooltip(tip)
    end
    return clicked
end

-- a text or number box as /vana's search is: a well drawn here, ImGui's field in it see-through. `draw_input` makes the field
-- and returns whether it changed. Returns that, and whether it was let go after an edit.
local function field(x, cy, w, draw_input)
    local h = S(24)
    local y = cy - h / 2
    local dl = imgui.GetWindowDrawList()
    kit.fill(dl, x, y, w, h, P.well, S(4))
    kit.outline(dl, x, y, w, h, P.well_edge, S(4))
    local text_h = S(SMALL + 1)
    imgui.SetCursorScreenPos({ x + S(2), y + 1 })
    imgui.SetNextItemWidth(w - S(4))
    imgui.PushStyleColor(kit.COL.FrameBg, kit.rgba(P.well, 0))
    imgui.PushStyleColor(kit.COL.FrameBgHovered, kit.rgba(P.well, 0))
    imgui.PushStyleColor(kit.COL.FrameBgActive, kit.rgba(P.well, 0))
    imgui.PushStyleVar(kit.VAR.FramePadding, { S(8), math.max(0, (h - 2 - text_h) / 2) })
    imgui.PushStyleVar(kit.VAR.FrameRounding, S(4))
    imgui.PushStyleVar(kit.VAR.FrameBorderSize, 0)
    kit.push(fonts.text, SMALL + 1)
    local changed = draw_input()
    local active = imgui.IsItemActive()
    local done = imgui.IsItemDeactivatedAfterEdit()
    kit.pop()
    imgui.PopStyleVar(3)
    imgui.PopStyleColor(3)
    if active then
        kit.outline(dl, x, y, w, h, P.accent_lo, S(4))
    end
    return changed, done
end

-- The names' column of the tab being drawn (pixels): its icon, its longest name and LABEL_GAP, set by names_column as a tab starts
local label_col = 0
-- ... and the room right of its sliders (pixels): Time's ZONE, on View & Effects' View; none elsewhere
local tag_room = 0
local function names_column(names)
    local widest = 0
    for _, name in ipairs(names) do
        widest = math.max(widest, kit.measure(name, fonts.text, BODY - 0.5))
    end
    label_col = S(24) + widest + S(LABEL_GAP)
end

-- a Settings row's name: its icon (the accent's when `lit`) and its words, in the names' column (label_col), `tip` over them
local function setting_name(dl, x, cy, icon, label, lit, enabled, tip)
    local alpha = enabled and 1 or 0.45
    kit.icon_centred(dl, x + S(8), cy, icon, lit and P.accent or P.faint, 11, alpha)
    kit.text_mid(dl, x + S(24), cy, label, P.text, fonts.text, BODY - 0.5, alpha)
    if tip ~= nil and kit.over(x, cy - S(14), label_col, S(28)) then
        kit.tooltip(tip)
    end
end

-- a Settings slider: from the names' column to tag_room short of the right, its reset arrow first (lit when `opts.can_reset`),
-- its value VALUE_W wide, so every slider's track starts and ends in the same place. Returns kit.slider's.
local function setting_slider(id, x, cy, w, value, min, max, step, enabled, opts)
    opts.width, opts.value_w, opts.reset = w - label_col - tag_room, S(VALUE_W), true
    return kit.slider(id, x + label_col, cy, value, min, max, step, enabled, opts)
end

-------------------------------------------------------------------------------
-- The toolbar (the switches, as icons, on Sequencer), and Settings: Lag, FOV, Playback, Auto Key Increment, Weather and Time
-------------------------------------------------------------------------------

local TOOLBAR_H = 40

-- The tree: Scenery detail: Farther ([graphics] lod=range) and Draw the whole zone in view ([graphics] drawdistance.full), the two
-- a far view needs. What they were is kept in state.scenery the first time the tree is pressed, and put back by restore_scenery:
-- when the free camera goes off, when the window is shut with it off, and when the addon unloads.
local function config_on(value)
    value = tostring(value or ''):lower():gsub('%s', '')
    return value == '1' or value == 'on' or value == 'true' or value == 'yes'
end

local function scenery_now()
    return {
        lod = tostring(vana.config.get('graphics', 'lod') or 'game'):lower(),
        full = tostring(vana.config.get('graphics', 'drawdistance.full') or '0'),
    }
end

local function scenery_is_on(v)
    return v.lod == 'range' and config_on(v.full)
end

local function set_scenery(v)
    vana.config.set('graphics', 'lod', v.lod)
    vana.config.set('graphics', 'drawdistance.full', config_on(v.full) and '1' or '0')
end

local function toggle_scenery()
    local now = scenery_now()
    if state.scenery == nil then
        state.scenery = now
    end
    if not scenery_is_on(now) then
        set_scenery({ lod = 'range', full = '1' })
    elseif scenery_is_on(state.scenery) then
        set_scenery({ lod = 'game', full = '0' }) -- it was on before the tree was pressed: off is the game's
    else
        set_scenery(state.scenery)
    end
end

local function restore_scenery()
    local before = state.scenery
    if before ~= nil then
        state.scenery = nil
        set_scenery(before)
    end
end

-- View & Effects' depth of field and colour: /vana's own [graphics.effects] keys, changed for the free camera only. What each
-- was (its text, or false: not set) is kept the first time it's changed, in the sequences file (fx_restore: a game that ends
-- with the free camera on gets them back at the next start), and put back by restore_fx: when the free camera goes off, when
-- the window is shut with it off, and when the addon unloads. The renderer takes them up once a frame (fx_dirty), as /vana's
-- sliders are.
local FX = 'graphics.effects'

local function fx_text(value, step)
    if step >= 1 then
        return tostring(math.floor(value + 0.5))
    end
    return (string.format('%.4f', value):gsub('0+$', ''):gsub('%.$', ''))
end

-- a key's section and name: 'np:<key>' is [graphics] <key> (the nameplates' hide settings, View's Nameplates), the rest
-- [graphics.effects]; all are kept and put back the same way
local function fx_section(key)
    if key:sub(1, 3) == 'np:' then
        return 'graphics', key:sub(4)
    end
    return FX, key
end

local function fx_set(key, text)
    if type(data.fx_restore) ~= 'table' then
        data.fx_restore = {}
    end
    local section, name = fx_section(key)
    if data.fx_restore[key] == nil then
        local was = vana.config.get(section, name)
        if was == nil or was == '' then
            was = false -- (not set: taken out again)
        end
        data.fx_restore[key] = was
        save()
    end
    vana.config.set(section, name, text)
    state.fx_dirty = true
end

-- the user's own (/vana's) text for `key`, while the free camera has it changed; nil when it hasn't
local function fx_saved(key)
    local saved = nil
    if type(data.fx_restore) == 'table' then
        saved = data.fx_restore[key] -- (false: it wasn't set; `and ... or` would lose that)
    end
    if saved == nil then
        return nil
    end
    if saved == false then
        return ''
    end
    return saved
end

local function restore_fx()
    local saved = data.fx_restore
    if type(saved) ~= 'table' or next(saved) == nil then
        return
    end
    for key, was in pairs(saved) do
        local section, name = fx_section(key)
        vana.config.set(section, name, was ~= false and was or nil)
    end
    data.fx_restore = nil
    save()
    state.fx_dirty = false
    pcall(vana.graphics.reload)
end

-- Weather: shows `id` (0 to 19) on this screen alone, or (nil or -1) puts the zone's own back; says what it did. Returns whether
-- it did it.
-- `quiet` (the picker): nothing said when it works, Changing: 3, 2, 1 in its row instead (state.weather_changing)
local function set_weather(id, quiet)
    if weather == nil then
        say("Weather couldn't load (vanacore.log says why).")
        return false
    end
    if id == nil or id < 0 then
        if weather.off(true) then
            if quiet then
                state.weather_changing = vana.time.clock() + 3
            else
                say("Weather: the zone's own again, fading back.")
            end
        end
        return true
    end
    local ok, why = weather.set(id)
    if ok and quiet then
        state.weather_changing = vana.time.clock() + 3
    elseif ok then
        say('Weather: ' .. weather.name(id) .. ", on your screen only (it fades in over 2.4 seconds). /freecam weather off "
            .. "puts the zone's own back.")
    else
        say("The weather can't be shown: " .. tostring(why) .. '.')
    end
    return ok
end

-- on the right of a row, small and from `rx` leftwards: ZONE `zone` (when given), then NOW `now` (when given), with `tip` over them;
-- one that would run past `min_x` (the row's control, in a narrow window) is left out, and those after it
local function now_tags(dl, rx, cy, now, zone, tip, min_x)
    local over_from = rx
    local full = false
    local function tag(label, value)
        local vw, lw = kit.measure(value, fonts.text, SMALL), kit.measure(label, fonts.semi, SMALL - 1.5)
        if full or rx - vw - S(4) - lw < min_x then
            full = true
            return
        end
        kit.text_right_mid(dl, rx, cy, value, P.text2, fonts.text, SMALL)
        rx = rx - vw - S(4)
        kit.text(dl, rx - lw, label_top(cy, label, fonts.semi, SMALL - 1.5, value, fonts.text, SMALL), label, P.faint, fonts.semi,
                 SMALL - 1.5)
        rx = rx - lw - S(12)
    end
    if zone ~= nil then
        tag('ZONE', zone)
    end
    if now ~= nil then
        tag('NOW', now)
    end
    if rx < over_from and kit.over(rx, cy - S(12), over_from - rx, S(24)) then
        kit.tooltip(tip)
    end
end

-- the Weather row: its icon and name, the picker (Zone's own, then the weathers under their elements), and on the right the
-- weather the game has now (vana.game.world().weather) and, while another is shown, the zone's own
local function draw_weather(dl, x, cy, w)
    local held = weather ~= nil and weather.held() or nil
    local enabled = weather ~= nil and weather.ready() == true
    setting_name(dl, x, cy, 'cloud_sun', 'Weather', held ~= nil, enabled, nil)
    -- the list: Zone's own, then under their elements the weathers this zone's files have a look for (every one when that
    -- can't be told), and the one shown if it isn't among them (typed); made again when the zone or that changes
    local zone = weather ~= nil and weather.zone_id() or nil
    local key = tostring(zone) .. ':' .. tostring(held)
    if weather ~= nil and state.weather_for ~= key then
        local set = weather.in_zone(zone)
        local options = { { -1, "Zone's own" } }
        for _, group in ipairs(weather.GROUPS) do
            local header = false
            for i = 2, #group do
                local id = group[i][1]
                if set == nil or set[id] or id == held then
                    if not header then
                        options[#options + 1] = { header = group[1] }
                        header = true
                    end
                    options[#options + 1] = { id, group[i][2] }
                end
            end
        end
        state.weather_options, state.weather_for = options, key
    end
    local dx = x + label_col
    local picked, moved, right = kit.dropdown('##fc.weather', dx, cy, S(170), state.weather_options or { { -1, "Zone's own" } },
                                               held or -1, enabled, nil, true, true)
    if kit.over(x, cy - S(14), right - x, S(28)) then
        kit.tooltip(TIPS.weather)
    end
    if moved then
        set_weather(picked, true)
    end
    -- while the game fades into it (2.4 s): Changing: 3, 2, 1 after the picker, small, in place of a message
    local tags_from = right + S(12)
    local wait = (state.weather_changing or 0) - vana.time.clock()
    if wait > 0 then
        local text = 'Changing: ' .. math.ceil(wait)
        kit.text_mid(dl, right + S(12), cy, text, P.amber, fonts.text, SMALL)
        tags_from = right + S(12) + kit.measure(text, fonts.text, SMALL) + S(12)
    end
    if weather == nil then
        return
    end
    local now = weather.now()
    now_tags(dl, x + w, cy, now ~= nil and weather.name(now) or nil, held ~= nil and weather.name(weather.zone()) or nil,
             TIPS.weather_now, tags_from)
end

-- Time: holds the day at `minute` (0 to 1439) on this screen alone, or (nil or -1) brings the zone's own hour back; says what it
-- did. Returns whether it did it.
local function set_daytime(minute)
    if daytime == nil then
        say("Time couldn't load (vanacore.log says why).")
        return false
    end
    if minute == nil or minute < 0 then
        if daytime.off() then
            say("Time: the zone's own again.")
        end
        return true
    end
    local ok, why = daytime.set(minute)
    if ok then
        say('Time: ' .. daytime.name(minute) .. ', held on your screen only. /freecam time off brings the zone\'s own back.')
    else
        say("The time can't be held: " .. tostring(why) .. '.')
    end
    return ok
end

-- the Time row: its icon and name, a slider over the day (00:00 to 23:50 in 10 minutes, ticks at 06:00, noon and 18:00) with its
-- reset icon before it, and on the right, while an hour is held, the zone's own. The knob is at the hour the game draws: the
-- zone's own till it's dragged, which holds the day there, the sky following at once as it moves. The reset icon (lit while one
-- is held) brings the zone's own back.
local DAY_MARKS = { { 360, '' }, { 720, '' }, { 1080, '' } }
local function draw_daytime(dl, x, cy, w)
    local held = daytime ~= nil and daytime.held() or nil
    local enabled = daytime ~= nil and daytime.ready() == true
    setting_name(dl, x, cy, 'clock', 'Time', held ~= nil, enabled, TIPS.daytime)
    local shown = held or (daytime ~= nil and daytime.now()) or 720
    local value, changed, released, _, reset = setting_slider('##fc.daytime', x, cy, w, shown, 0, 1430, 10, enabled, {
        text = daytime ~= nil and daytime.name(shown) or '--:--', can_reset = held ~= nil, marks = DAY_MARKS,
    })
    if changed then
        -- (held at once, every step of the drag; said only if it can't be)
        local ok, why = daytime.set(value)
        if not ok then
            state.daytime_why = why
        end
    end
    if released and state.daytime_why ~= nil then
        say("The time can't be held: " .. tostring(state.daytime_why) .. '.')
        state.daytime_why = nil
    end
    if reset and held ~= nil then
        set_daytime(nil)
    end
    if held ~= nil then
        now_tags(dl, x + w, cy, nil, daytime.name(daytime.zone()), TIPS.daytime_now, x + w - tag_room + S(4))
    end
end

local function toolbar_height(ctx, w)
    return S(TOOLBAR_H)
end

local function draw_toolbar(dl, x, y, w, ctx)
    local cs, seq = ctx.cs, ctx.seq
    local cy = y + S(TOOLBAR_H) / 2
    local right = x + w
    local gap = S(6)

    -- the left: Floaty, a line, then the sequence's: Hide UI, Smooth curve, Ease in and out, Loop and Snap
    local bx = x
    if icon_toggle('##fc.t.float', bx, cy, 'wind', cs.float, true, TIPS.float) then
        camera.float(not cs.float)
    end
    bx = bx + S(TOOL_W) + gap
    kit.vline(dl, bx, cy - S(9), cy + S(9), P.line)
    bx = bx + gap + S(1)
    local smooth = seq.mode ~= 'linear'
    if icon_toggle('##fc.t.hideui', bx, cy, 'eye_off', data.hide_ui == true, true, TIPS.hide_ui) then
        data.hide_ui = not data.hide_ui
        save()
    end
    bx = bx + S(TOOL_W) + gap
    if icon_toggle('##fc.t.smooth', bx, cy, 'curve', smooth, true, TIPS.smooth) then
        seq.mode = smooth and 'linear' or 'spline'
        touch()
        save()
    end
    bx = bx + S(TOOL_W) + gap
    if icon_toggle('##fc.t.ease', bx, cy, 'ease', seq.ends, smooth, TIPS.ease) then
        seq.ends = not seq.ends
        touch()
        save()
    end
    bx = bx + S(TOOL_W) + gap
    if icon_toggle('##fc.t.loop', bx, cy, 'loop', seq.loop, true, TIPS.loop) then
        seq.loop = not seq.loop
        touch()
        save()
    end
    bx = bx + S(TOOL_W) + gap
    if icon_toggle('##fc.t.snap', bx, cy, 'magnet', data.snap ~= false, true, TIPS.snap) then
        data.snap = data.snap == false
        save()
    end
    bx = bx + S(TOOL_W) + gap
    if icon_toggle('##fc.t.space', bx, cy, 'space', data.space_play == true, true, TIPS.space) then
        data.space_play = data.space_play ~= true
        save()
        if state.binds_on and not set_space_bind(data.space_play) then
            say('Space is already one of your own binds, so it is left as it is.')
        end
    end
    -- (Far scenery and Grass follows are on View & Effects' View now: asked for)

    -- the right: into the free camera, only while it's off (the window opened by /freecam window). On, the title's X is the
    -- way out of it
    if not cs.on and icon_press('##fc.start', right - S(TOOL_W), cy, 'power', P.accent, P.accent2, TIPS.start) then
        local ok, why = camera.free(true)
        if not ok then
            say("The free camera couldn't turn on: " .. tostring(why))
        end
    end
    return y + S(TOOLBAR_H)
end

-- Lag and FOV are the camera's own value until the slider is held, then the slider's (set as it moves, saved when it's let go);
-- their reset arrows put back VanaCore's defaults (camera.cpp's: 0.35 s, and the game's 57.5 degrees). (How fast the camera
-- flies is a number in the title: the wheel sets it.)
local LAG_DEFAULT, FOV_DEFAULT = 0.35, 57.5

local function draw_lag(dl, x, cy, w, cs)
    local lag = kit.st.sliding == '##fc.lag' and state.lag_value or cs.lag
    setting_name(dl, x, cy, 'hourglass', 'Lag', cs.float, cs.float, TIPS.lag)
    local v, changed, released, _, reset = setting_slider('##fc.lag', x, cy, w, lag, 0.05, 2.0, nil, cs.float, {
        text = string.format('%.2f s', lag), can_reset = math.abs(lag - LAG_DEFAULT) > 0.004,
    })
    if reset then
        state.lag_value = nil
        camera.preview('freecam.lag', LAG_DEFAULT)
        vana.config.set('camera', 'freecam.lag', string.format('%.2f', LAG_DEFAULT))
        return
    end
    if changed then
        state.lag_value = v
        camera.preview('freecam.lag', v)
    end
    if released and state.lag_value ~= nil then
        vana.config.set('camera', 'freecam.lag', string.format('%.2f', state.lag_value))
    end
end

-- FOV: [camera] fov, which VanaCore keeps only with True Zoom (otherwise the game's zoom is the field of view); greyed without
local function draw_fov(dl, x, cy, w)
    local zoom_text = (vana.config.get('camera', 'zoom') or ''):lower()
    local true_zoom = zoom_text == '1' or zoom_text == 'on' or zoom_text == 'distance'
    local fov = kit.st.sliding == '##fc.fov' and state.fov_value or tonumber(vana.config.get('camera', 'fov')) or FOV_DEFAULT
    setting_name(dl, x, cy, 'eye', 'FOV', true_zoom, true_zoom, true_zoom and TIPS.fov or TIPS.fov_off)
    local v, moved, released, _, reset = setting_slider('##fc.fov', x, cy, w, clamp(fov, 10, 120), 10, 120, nil, true_zoom, {
        text = string.format('%.1f\194\176', fov), can_reset = math.abs(fov - FOV_DEFAULT) > 0.01,
    })
    if reset then
        state.fov_value = nil
        camera.preview('fov', FOV_DEFAULT)
        vana.config.set('camera', 'fov', string.format('%.1f', FOV_DEFAULT))
        return
    end
    if moved then
        state.fov_value = math.floor(v * 2 + 0.5) / 2
        camera.preview('fov', state.fov_value)
    end
    if released and state.fov_value ~= nil then
        vana.config.set('camera', 'fov', string.format('%.1f', state.fov_value))
    end
end

-- Playback: how fast a sequence plays, a log scale, 0.25 to 4, with 1 (real time) caught as it passes; reset, 1
local function draw_rate(dl, x, cy, w)
    setting_name(dl, x, cy, 'film', 'Playback', true, true, TIPS.rate)
    local log_rate, moved, _, _, reset = setting_slider('##fc.rate', x, cy, w, math.log(clamp(state.rate, 0.25, 4)),
                                                        math.log(0.25), math.log(4), nil, true, {
        text = string.format('%g', state.rate) .. '\195\151', can_reset = state.rate ~= 1,
    })
    if reset then
        state.rate = 1
    elseif moved then
        local rate = math.exp(log_rate)
        state.rate = math.abs(rate - 1) < 0.04 and 1 or clamp(math.floor(rate * 20 + 0.5) / 20, 0.25, 4)
    end
end

-- Auto Key Increment: how far Add key moves the playhead on (the sequence's spacing), in Snap's steps
local function draw_spacing(dl, x, cy, w, seq)
    setting_name(dl, x, cy, 'pin', 'Auto Key Increment', true, true, TIPS.spacing)
    local by = kit.arrows('##fc.spacing', x + label_col, cy, string.format('%g s', seq.spacing), true)
    if by ~= 0 then
        seq.spacing = clamp(math.floor((seq.spacing + by * SNAP) / SNAP + 0.5) * SNAP, SNAP, 60)
        save()
    end
end

-- A group's small heading, a hairline before it unless it's the first (S(8) more above it then); returns where its rows start
local function group_heading(dl, x, y, w, title, first)
    if not first then
        y = y + S(8)
        kit.hline(dl, x, x + w, y + S(4), P.line)
    end
    kit.text_mid(dl, x, y + S(HEAD_H) / 2 + S(2), title, P.faint, fonts.semi, SMALL - 1.5)
    return y + S(HEAD_H)
end

local function groups_height(rows_in_each)
    local h = 0
    for i, rows in ipairs(rows_in_each) do
        h = h + (i > 1 and S(8) or 0) + S(HEAD_H) + S(ROW_H) * rows
    end
    return h
end

-- Settings: the camera's (Lag) and the sequence's (Playback, Auto Key Increment), each under a small heading; each row a name
-- in the names' column and its control after it
local function settings_height(ctx, w)
    return groups_height({ 1, 2 }) + S(4)
end

local function draw_settings(dl, x, y, w, ctx)
    names_column({ 'Lag', 'Playback', 'Auto Key Increment' })
    local function row(draw_row, arg)
        draw_row(dl, x, y + S(ROW_H) / 2, w, arg)
        y = y + S(ROW_H)
    end
    y = group_heading(dl, x, y, w, 'CAMERA', true)
    row(draw_lag, ctx.cs)
    y = group_heading(dl, x, y, w, 'SEQUENCE')
    row(draw_rate)
    row(draw_spacing, ctx.seq)
    return y + S(4)
end

-------------------------------------------------------------------------------
-- View & Effects: a photo mode. The view (FOV, Weather, Time), and /vana's depth of field and colour grade, changed for the
-- free camera only (fx_set; restore_fx puts the user's back when it goes off). A group's switch is in its heading's right; a
-- slider moved while its group is off turns it on. Each reset arrow puts back the user's own value, lit while it's changed.
-------------------------------------------------------------------------------

-- each: the key, its name, its icon, /vana's default, the slider's ends and step, how its value reads, its tooltip
local DOF_ROWS = {
    { key = 'dof', label = 'Strength', icon = 'bullseye', default = 0, min = 1, max = 40, step = 1,
      show = function (v) return string.format('%d px', v) end,
      tip = 'Strength: how blurred the farthest things get, in pixels.' },
    -- (what stays sharp: Player, Centre or Free, and Free's distance; drawn by ext.focus_row, below)
    { focus = 'mode', label = 'Focus', icon = 'crosshairs' },
    { focus = 'distance', label = 'Focus distance', icon = 'locate' },
    { key = 'dof_range', label = 'Sharp zone', icon = 'arrows_h', default = 0.5, min = 0, max = 3, step = 0.05,
      show = function (v) return string.format('%.2f', v) end,
      tip = 'Sharp zone: how deep the sharp part round the focus is (times the focus distance) before the blur starts.' },
}
local GRADE_ROWS = {
    { key = 'sat', label = 'Saturation', icon = 'droplet', default = 1.12, min = 0, max = 2, step = 0.02,
      show = function (v) return string.format('%.2f', v) end, tip = 'Saturation: 0 is black and white, 1 the colours as they are.' },
    { key = 'contrast', label = 'Contrast', icon = 'half', default = 0.2, min = 0, max = 1, step = 0.05,
      show = function (v) return string.format('%.2f', v) end, tip = 'Contrast: darker darks and brighter lights.' },
    { key = 'brightness', label = 'Brightness', icon = 'sun', default = 0, min = -1, max = 1, step = 0.05,
      show = function (v) return string.format('%+.2f', v) end, tip = 'Brightness: the midtones lifted or lowered (black and white stay).' },
    { key = 'warmth', label = 'Warmth', icon = 'fire', default = 0, min = -1, max = 1, step = 0.05,
      show = function (v) return string.format('%+.2f', v) end, tip = 'Warmth: bluer (cool) to more orange (warm).' },
    { key = 'hue', label = 'Hue', icon = 'palette', default = 0, min = -180, max = 180, step = 1,
      show = function (v) return string.format('%+d\194\176', math.floor(v + 0.5)) end,
      tip = 'Hue: every colour turned round the colour wheel; greys stay as they are.' },
}
-- the groups' switches: /vana's effect switch (its strength above 0, <key>.enabled not 0), and the strength it's turned on at
local DOF_SWITCH = { key = 'dof', on = 12, default = 0, label = 'Depth of field' }
local GRADE_SWITCH = { key = 'grade', on = 1, default = 1, label = 'Colour grade' }
-- Graphics: /vana's effects that most change a picture, each its own switch (its key, /vana's default, the strength it's
-- turned on at) and its strength
local GFX_ROWS = {
    { key = 'ao', label = 'Ambient occlusion', icon = 'cube', default = 1.0, on = 1.0, min = 0.05, max = 2, step = 0.05,
      tip = 'Ambient occlusion: how dark corners and creases get. Adds depth.' },
    { key = 'sun', label = 'Shadows', icon = 'half', default = 0.6, on = 0.6, min = 0.05, max = 1, step = 0.05,
      tip = "Shadows: how dark VanaCore's sun shadows are. The game's own stay off while these are on." },
    -- (/vana's Lighting > Sun strength, asked for: off by default, 1 when it's turned on, up to 3; its slider goes
    -- down to 0, which is off, as /vana's does: the others stop at 0.05)
    { key = 'day_bright', label = 'Sun strength', icon = 'sun', default = 0, on = 1, min = 0, max = 3, step = 0.05,
      tip = 'Sun strength: a stronger sun outdoors by day. What faces it is lit brighter; the shade stays as it is.' },
    { key = 'refl', label = 'Reflections', icon = 'water', default = 0, on = 1, min = 0.05, max = 2, step = 0.05, dx12 = true,
      tip = 'Reflections: how strong they are on floors and flat ground. Direct3D 12.' },
    { key = 'bloom', label = 'Bloom', icon = 'lightbulb', default = 1.5, on = 1.5, min = 0.05, max = 5, step = 0.05,
      tip = 'Bloom: how strongly bright lights glow.' },
}

local function fx_number(key, default)
    return tonumber(vana.config.get(FX, key) or '') or default
end

local function fx_switched(sw)
    return tonumber(vana.config.get(FX, sw.key .. '.enabled') or '') ~= 0 and fx_number(sw.key, sw.default) > 0
end

local function fx_turn(sw, on)
    if not on then
        fx_set(sw.key .. '.enabled', '0')
        return
    end
    if vana.config.get(FX, sw.key .. '.enabled') ~= nil then
        fx_set(sw.key .. '.enabled', nil)
    end
    if fx_number(sw.key, sw.default) <= 0 then
        fx_set(sw.key, tostring(sw.on))
    end
    if sw.turned_on ~= nil then
        sw.turned_on()
    end
end

local ext = {} -- (the helpers below: the main chunk can't hold more than 200 locals)
-- Depth of field's Focus (asked for: "Focus Player" and "Free Focus"). /vana's 0 ("You") is the nearest thing just below
-- the screen's middle, where the game's own camera keeps the character: under the free camera that isn't the character at all.
--   Player  your character, wherever it is in the view ([graphics.effects] dof_player 1: the renderer measures how far into the
--           view the character stands; a VanaCore.Core.dll from before it focuses on the middle)
--   Centre  the nearest thing at the view's middle (dof_player 0, written, to tell it from never chosen)
--   Free    a distance of your own from the camera (dof_focus), on a slider whose every step is the same share nearer or
--           farther (0.5 to 100), so a close-up can be set as finely as a landscape
-- Turned on here with no Focus chosen yet, depth of field is on Player. Free starts where Player had it (near enough: the
-- ground's plane alone), else at the last distance, else 10.
ext.FOCUS_MODES = { 'Player', 'Centre', 'Free' }
ext.FOCUS_NEAR, ext.FOCUS_FAR = 0.5, 100

function ext.focus_mode()
    if fx_number('dof_focus', 0) > 0 then
        return 3
    end
    return fx_number('dof_player', 0) ~= 0 and 1 or 2
end

function ext.player_depth()
    local party = vana.game.party()
    local me = type(party) == 'table' and type(party.members) == 'table' and party.members[1] or nil
    local entity = type(me) == 'table' and me.target_index ~= nil and vana.game.entity(me.target_index) or nil
    local x, y, z, yaw, tilt = camera.pose()
    if type(entity) ~= 'table' or entity.x == nil or entity.y == nil or entity.z == nil or x == nil or z == nil or yaw == nil then
        return nil
    end
    -- how far along the camera's forward (the world's x, up and z; the entity's z is its height, its y the world's z), to the
    -- middle of the character (a yalm up)
    tilt = tilt or 0
    local depth = (entity.x - x) * math.cos(tilt) * math.cos(yaw) + (entity.z + 1 - y) * math.sin(tilt) +
                  (entity.y - z) * math.cos(tilt) * math.sin(yaw)
    return depth > ext.FOCUS_NEAR and math.min(depth, ext.FOCUS_FAR) or nil
end

function ext.set_focus_mode(mode)
    local was, at = ext.focus_mode(), fx_number('dof_focus', 0)
    if at > 0 then
        state.dof_free = at
    end
    if mode == 3 then
        local start = (was == 1 and ext.player_depth()) or state.dof_free or ext.player_depth() or 10
        fx_set('dof_focus', fx_text(clamp(start, ext.FOCUS_NEAR, ext.FOCUS_FAR), 0.01))
        return
    end
    if vana.config.get(FX, 'dof_focus') ~= nil then
        fx_set('dof_focus', nil)
    end
    fx_set('dof_player', mode == 1 and '1' or '0')
end

function DOF_SWITCH.turned_on()
    if vana.config.get(FX, 'dof_player') == nil and fx_number('dof_focus', 0) <= 0 then
        fx_set('dof_player', '1')
    end
end

function ext.focus_row(dl, x, cy, w, row, sw, enabled, on)
    local mode = ext.focus_mode()
    local from = x + label_col + S(kit.RESET_W) -- (where the sliders' tracks start, after their reset arrows)
    if row.focus == 'mode' then
        setting_name(dl, x, cy, row.icon, row.label, enabled and on, enabled, enabled and TIPS.focus or TIPS.fx_off)
        local picked, changed, over = kit.segments('##fc.focus', from, cy, ext.FOCUS_MODES, mode, enabled, nil, on and 1 or 0.55)
        if over ~= nil then
            kit.tooltip(enabled and TIPS['focus' .. over] or TIPS.fx_off)
        end
        if changed then
            ext.set_focus_mode(picked)
            if not on then
                fx_turn(sw, true)
            end
        end
        return
    end
    local free = mode == 3
    setting_name(dl, x, cy, row.icon, row.label, enabled and on and free, enabled,
                 enabled and TIPS.focus_distance or TIPS.fx_off)
    local span = math.log(ext.FOCUS_FAR / ext.FOCUS_NEAR)
    local at = free and clamp(fx_number('dof_focus', 10), ext.FOCUS_NEAR, ext.FOCUS_FAR) or nil
    local v, changed = kit.slider('##fc.fx.dof_focus', from, cy, at ~= nil and math.log(at / ext.FOCUS_NEAR) / span or 0, 0, 1,
                                  0.005, enabled and free, {
        text = at ~= nil and string.format(at < 10 and '%.1f' or '%.0f', at) or 'Auto',
        width = w - label_col - tag_room - S(kit.RESET_W), value_w = S(VALUE_W), alpha = (on and free) and 1 or 0.55,
    })
    if changed then
        fx_set('dof_focus', fx_text(ext.FOCUS_NEAR * math.exp(v * span), 0.01))
        if not on then
            fx_turn(sw, true)
        end
    end
end

-- a group's heading with its switch on the right (`first`: no hairline above it); returns where its rows start, and whether
-- the group is on
local function fx_heading(dl, x, y, w, title, sw, enabled, tip, first)
    y = group_heading(dl, x, y, w, title, first)
    local on = fx_switched(sw)
    local sx, cy = x + w - S(kit.SW_W), y - S(HEAD_H) / 2 + S(2)
    local turned, changed = kit.switch('##fc.sw.' .. sw.key, sx, cy, on, enabled)
    if kit.over(sx - S(4), cy - S(12), S(kit.SW_W) + S(8), S(24)) then
        kit.tooltip(tip)
    end
    if changed then
        fx_turn(sw, turned)
    end
    return y, on
end

local function fx_row(dl, x, cy, w, row, sw, enabled, on)
    if row.focus ~= nil then
        return ext.focus_row(dl, x, cy, w, row, sw, enabled, on)
    end
    setting_name(dl, x, cy, row.icon, row.label, enabled and on, enabled, enabled and row.tip or TIPS.fx_off)
    local value = fx_number(row.key, row.default)
    local saved = fx_saved(row.key)
    local mine = saved ~= nil and (tonumber(saved) or row.default) or nil
    local v, changed, _, _, reset = setting_slider('##fc.fx.' .. row.key, x, cy, w, clamp(value, row.min, row.max), row.min,
                                                   row.max, row.step, enabled, {
        text = row.show(value), can_reset = mine ~= nil and math.abs(mine - value) > row.step / 4, alpha = on and 1 or 0.55,
    })
    if reset and saved ~= nil then
        fx_set(row.key, saved ~= '' and saved or nil)
    elseif changed then
        fx_set(row.key, fx_text(v, row.step))
        if not on then
            fx_turn(sw, true)
        end
    end
end

-- Animation (asked for: the race's animations by category as XI Model Viewer lists them, played, paused and moved by the
-- frame, to set up a photo). Category, then the Animation in it (an emote's, a weapon skill's or a stance's files, loaded onto
-- your character as it's picked: freecam\anim.lua), then the Clip to play from those; Frame scrubs it, in thirtieths of a
-- second as the model viewer counts them, and pauses it; Speed. In the heading: play or pause, Loop and Stop. Basic is the
-- race's own motions, which need no loading. It's all on this screen alone, and goes with the free camera (anim_off).
local ANIM_NAMES = { 'Category', 'Animation', 'Clip', 'Frame', 'Speed' }

local function anim_off()
    if anim ~= nil then
        if state.facing_was ~= nil then
            pcall(anim.facing, state.facing_was) -- (View's Facing: how it faced put back)
            state.facing_was = nil
        end
        local ok, err = pcall(anim.off)
        if not ok then
            fail('animation off', err)
        end
    end
    state.anim_cat, state.anim_entry, state.anim_stem, state.anim_clips, state.anim_clips_for = 0, 0, '', nil, nil
end

-- picks a category's animation (entry 0 in category 0: the race's own): its files loaded in place of the last one's
local function anim_pick(list, cat, entry)
    state.anim_cat, state.anim_entry, state.anim_stem, state.anim_clips_for = cat, entry, '', nil
    state.anim_loaded = false -- (asked again next frame: its clips are listed from then, however fast its files came)
    anim.pick(cat > 0 and list[cat][entry + 1] or nil)
end

-- the Clip list's options: Game's own, then the picked animation's clips; made again as its files load and every 2 s (Basic's
-- are what the character has loaded, which the game changes: a weapon drawn, an emote)
local function anim_clip_options(now)
    local key = state.anim_cat .. ':' .. state.anim_entry .. ':' .. tostring(state.anim_loaded) .. ':' .. math.floor(now / 2)
    if state.anim_clips_for ~= key or state.anim_clips == nil then
        local options = { { '', state.anim_loaded and "Game's own" or 'Loading...' } }
        local found = false
        if state.anim_loaded then
            for _, clip in ipairs(anim.clips()) do
                options[#options + 1] = { clip[1], clip[2] }
                found = found or clip[1] == state.anim_stem
            end
        end
        -- (the one playing stays in the list while it plays, whatever the list is now)
        if state.anim_stem ~= '' and not found then
            if anim.playing() then
                options[#options + 1] = { state.anim_stem, state.anim_stem }
            else
                state.anim_stem = ''
            end
        end
        state.anim_clips, state.anim_clips_for = options, key
    end
    return state.anim_clips
end

local function draw_anim(dl, x, y, w, title, now_clock)
    names_column(ANIM_NAMES)
    tag_room = 0
    local list = anim ~= nil and anim.list(animdata) or nil
    local usable = list ~= nil and anim.ready()
    local now = usable and anim.now() or nil
    y = group_heading(dl, x, y, w, title, true)

    -- the heading's right: Stop, Loop, and play or pause
    local hy = y - S(HEAD_H) / 2 + S(2)
    local bx = x + w - S(TOOL_W)
    if icon_press('##fc.an.stop', bx, hy, 'stop', usable and P.text2 or P.faint, usable and P.accent2 or P.faint, TIPS.anim_stop)
       and usable then
        anim.stop()
    end
    bx = bx - S(TOOL_W)
    if icon_toggle('##fc.an.loop', bx, hy, 'loop', state.anim_loop, usable, TIPS.anim_loop) then
        state.anim_loop = not state.anim_loop
        anim.loop(state.anim_loop)
    end
    bx = bx - S(TOOL_W)
    local running = now ~= nil and not now.paused
    if icon_toggle('##fc.an.play', bx, hy, running and 'pause' or 'play', running, usable and state.anim_stem ~= '',
                   TIPS.anim_play) then
        if now == nil then
            anim.play(state.anim_stem, state.anim_loop, state.anim_speed)
        else
            anim.pause(not now.paused)
        end
    end

    local drop_w = math.min(S(280), w - label_col)
    local dx = x + label_col
    local function row_tip(cy, right, tip)
        if kit.over(x, cy - S(14), right - x, S(28)) then
            kit.tooltip(usable and tip or TIPS.anim_off)
        end
    end

    -- Category: Basic, then the list's
    local cy = y + S(ROW_H) / 2
    setting_name(dl, x, cy, 'layers', 'Category', false, usable, nil)
    if state.anim_cats_for ~= list or state.anim_cats == nil then
        local options = { { 0, 'Basic' } }
        for i, category in ipairs(list or {}) do
            options[#options + 1] = { i, category[1] }
        end
        state.anim_cats, state.anim_cats_for, state.anim_entries_for = options, list, nil
    end
    if usable and state.anim_cat > #list then
        anim_pick(list, 0, 0)
    end
    local picked, moved, right = kit.dropdown('##fc.an.cat', dx, cy, drop_w, state.anim_cats, state.anim_cat, usable, nil,
                                               true, true)
    row_tip(cy, right, TIPS.anim_cat)
    if moved and usable then
        anim_pick(list, picked, picked > 0 and 1 or 0)
    end

    -- Animation: the category's
    cy = cy + S(ROW_H)
    setting_name(dl, x, cy, 'list', 'Animation', false, usable, nil)
    if state.anim_entries_for ~= state.anim_cat or state.anim_entries == nil then
        local options = {}
        if state.anim_cat == 0 or list == nil then
            options[1] = { 0, 'Standard' }
        else
            local category = list[state.anim_cat]
            for i = 2, #category do
                options[#options + 1] = { i - 1, category[i][1] }
            end
        end
        state.anim_entries, state.anim_entries_for = options, state.anim_cat
    end
    picked, moved, right = kit.dropdown('##fc.an.entry', dx, cy, drop_w, state.anim_entries, state.anim_entry,
                                        usable and state.anim_cat > 0, nil, true, true)
    row_tip(cy, right, TIPS.anim_entry)
    if moved and usable then
        anim_pick(list, state.anim_cat, picked)
    end

    -- Clip: picked, it plays
    cy = cy + S(ROW_H)
    setting_name(dl, x, cy, 'person', 'Clip', now ~= nil, usable, nil)
    local clips = usable and anim_clip_options(now_clock) or { { '', "Game's own" } }
    picked, moved, right = kit.dropdown('##fc.an.clip', dx, cy, drop_w, clips, now ~= nil and now.stem or state.anim_stem,
                                        usable and state.anim_loaded, nil, true, true)
    row_tip(cy, right, TIPS.anim_clip)
    if moved and usable then
        state.anim_stem, state.anim_clips_for = picked, nil
        if picked == '' then
            anim.stop()
        else
            anim.play(picked, state.anim_loop, state.anim_speed)
        end
    end

    -- Frame: thirtieths of a second into the clip (a clip's own frames are its keys, `rate` of them a thirtieth)
    cy = cy + S(ROW_H)
    local last, at = 1, 0
    if now ~= nil and now.rate > 0 then
        last = math.max(1, math.ceil(now.frames / now.rate - 0.001) - 1)
        at = math.max(0, math.min(last, math.floor(now.frame / now.rate + 0.001)))
    end
    setting_name(dl, x, cy, 'film', 'Frame', now ~= nil and now.paused, usable, usable and TIPS.anim_frame or TIPS.anim_off)
    local value, changed, _, _, reset = setting_slider('##fc.an.frame', x, cy, w, at, 0, last, 1, now ~= nil, {
        text = now ~= nil and (at .. '/' .. last) or '-', can_reset = now ~= nil and at > 0,
    })
    if now ~= nil and (changed or reset) then
        anim.frame((reset and 0 or value) * now.rate)
    end

    -- Speed
    cy = cy + S(ROW_H)
    setting_name(dl, x, cy, 'gauge', 'Speed', false, usable, usable and TIPS.anim_speed or TIPS.anim_off)
    value, changed, _, _, reset = setting_slider('##fc.an.speed', x, cy, w, state.anim_speed, 0.1, 2, 0.05, usable, {
        text = string.format('%.2fx', state.anim_speed), can_reset = math.abs(state.anim_speed - 1) > 0.01,
    })
    if reset then
        value, changed = 1, true
    end
    if changed and usable then
        state.anim_speed = value
        anim.speed(value)
    end
end

-- View & Effects is a page: a list of its sections down the left (asked for), the one picked in the panel beside it, as
-- /vana's pages are. Each section: its name and icon in the list (a dot while its effect is on), and in the panel its heading
-- (with the effect's switch) and its rows. The page is as tall as its tallest section, so picking another doesn't move the window.
local VIEW_NAMES = { 'FOV', 'Weather', 'Time', 'Facing', 'Nameplates', 'Far scenery', 'Grass follows' }

-- the nameplates' hide settings ([graphics] nameplates.<name>): hiding every name is all eight on; kept and put back as the
-- effects are (fx_set's 'np:' keys)
ext.NAMEPLATE_HIDES = { 'hideown', 'hideparty', 'hidealliance', 'hideplayers', 'hidenpcs', 'hidemonsters', 'hidetrusts', 'hidepets' }

function ext.nameplates_shown()
    for _, name in ipairs(ext.NAMEPLATE_HIDES) do
        if not config_on(vana.config.get('graphics', 'nameplates.' .. name)) then
            return true
        end
    end
    return false
end

function ext.show_nameplates(on)
    for _, name in ipairs(ext.NAMEPLATE_HIDES) do
        local key = 'np:nameplates.' .. name
        if on then
            local saved = fx_saved(key) -- (as they were: some of them may have been hidden already)
            if saved ~= nil then
                fx_set(key, saved ~= '' and saved or nil)
            end
        else
            fx_set(key, '1')
        end
    end
end

-- a switch row: its name and icon, then the switch in the column the sliders start in
function ext.switch_row(dl, x, cy, id, icon, label, on, enabled, tip)
    setting_name(dl, x, cy, icon, label, on and enabled, enabled, tip)
    local turned, changed = kit.switch(id, x + label_col, cy, on, enabled)
    return turned, changed
end

-- Facing: your character turned on the spot (a photo's pose), in degrees; the reset arrow puts back how it faced. On this
-- screen alone: it's turned back when the free camera goes off.
function ext.draw_facing(dl, x, cy, w)
    local enabled = anim ~= nil and anim.ready()
    local now = enabled and anim.facing() or nil
    local degrees = now ~= nil and (math.deg(now) % 360) or 0
    setting_name(dl, x, cy, 'person', 'Facing', state.facing_was ~= nil, enabled, enabled and TIPS.facing or TIPS.anim_off)
    local v, changed, _, _, reset = setting_slider('##fc.facing', x, cy, w, degrees, 0, 360, 1, enabled and now ~= nil, {
        text = string.format('%d\194\176', math.floor(degrees + 0.5) % 360), can_reset = state.facing_was ~= nil,
    })
    if reset and state.facing_was ~= nil then
        anim.facing(state.facing_was)
        state.facing_was = nil
    elseif changed and now ~= nil then
        if state.facing_was == nil then
            state.facing_was = now
        end
        anim.facing(math.rad(v))
    end
end

function ext.draw_nameplates(dl, x, cy)
    local turned, changed = ext.switch_row(dl, x, cy, '##fc.v.nameplates', 'tag', 'Nameplates', ext.nameplates_shown(), true, TIPS.nameplates)
    if changed then
        ext.show_nameplates(turned)
    end
end

function ext.draw_scenery(dl, x, cy)
    local turned, changed = ext.switch_row(dl, x, cy, '##fc.v.scenery', 'tree', 'Far scenery', scenery_is_on(scenery_now()), true,
                                       TIPS.scenery)
    if changed then
        toggle_scenery()
    end
end

-- the grass grows round the free camera ([graphics] grass.freecam, on unless set), as /vana's Follow free camera
function ext.draw_grass(dl, x, cy)
    local follow = vana.config.get('graphics', 'grass.freecam')
    local follows = follow == nil or follow == '' or config_on(follow)
    local _, changed = ext.switch_row(dl, x, cy, '##fc.v.grass', 'seedling', 'Grass follows', follows, true, TIPS.grass)
    if changed then
        vana.config.set('graphics', 'grass.freecam', follows and '0' or '1')
        vana.graphics.reload() -- taken up at once, as /vana's graphics rows are
    end
end
local VIEW_SECTIONS = {
    { id = 'view', label = 'View', icon = 'eye', title = 'VIEW', rows = #VIEW_NAMES },
    { id = 'anim', label = 'Animation', icon = 'person', title = 'ANIMATION', rows = #ANIM_NAMES,
      lit = function () return anim ~= nil and anim.playing() end },
    { id = 'dof', label = 'Depth of field', icon = 'crosshairs', title = 'DEPTH OF FIELD', rows = #DOF_ROWS, list = DOF_ROWS,
      switch = DOF_SWITCH, tip = 'dof' },
    { id = 'grade', label = 'Colour', icon = 'palette', title = 'COLOUR', rows = #GRADE_ROWS, list = GRADE_ROWS,
      switch = GRADE_SWITCH, tip = 'grade' },
    { id = 'graphics', label = 'Graphics', icon = 'sliders', title = 'GRAPHICS', rows = #GFX_ROWS, list = GFX_ROWS },
}
local NAV_ITEM_H, NAV_PAD, NAV_GAP = 34, 6, 16 -- a section in the list, the list's padding, the gap before the panel

-- a Graphics row: its name, its switch, then its strength (the reset arrow puts back the user's own); a strength moved while
-- it's off turns it on. Greyed while VanaCore's effects aren't running, or (Reflections) the renderer isn't Direct3D 12.
local function gfx_row(dl, x, cy, w, row, enabled, d3d12)
    local usable = enabled and (not row.dx12 or d3d12)
    local on = fx_switched(row)
    local tip = usable and row.tip or (enabled and TIPS.dx12_only or TIPS.fx_off)
    setting_name(dl, x, cy, row.icon, row.label, usable and on, usable, tip)
    local sx = x + label_col
    local turned, switched = kit.switch('##fc.gsw.' .. row.key, sx, cy, on, usable)
    if switched then
        fx_turn(row, turned)
    end
    local value = fx_number(row.key, row.default)
    local saved = fx_saved(row.key)
    local mine = saved ~= nil and (tonumber(saved) or row.default) or nil
    local slider_x = sx + S(kit.SW_W) + S(12)
    local v, changed, _, _, reset = kit.slider('##fc.fx.' .. row.key, slider_x, cy, clamp(value, row.min, row.max), row.min,
                                               row.max, row.step, usable, {
        text = string.format('%.2f', value), width = math.max(S(80), x + w - slider_x - tag_room), value_w = S(VALUE_W),
        reset = true, can_reset = mine ~= nil and math.abs(mine - value) > row.step / 4, alpha = on and 1 or 0.55,
    })
    if reset and saved ~= nil then
        fx_set(row.key, saved ~= '' and saved or nil)
    elseif changed then
        fx_set(row.key, fx_text(v, row.step))
        -- (a strength dragged to 0 is off: turning it on there would put it at the strength it's turned on at)
        if not on and v > 0 then
            fx_turn(row, true)
        end
    end
end

local function view_height(ctx, w)
    local h = S(NAV_PAD) * 2 + S(NAV_ITEM_H) * #VIEW_SECTIONS
    for _, section in ipairs(VIEW_SECTIONS) do
        h = math.max(h, S(HEAD_H) + S(ROW_H) * section.rows + S(4))
    end
    return h
end

-- the list's width: its longest name, its icon, and room for the dot
local function nav_width()
    local widest = 0
    for _, section in ipairs(VIEW_SECTIONS) do
        widest = math.max(widest, kit.measure(section.label, fonts.text, BODY - 0.5))
    end
    return S(12) + S(24) + widest + S(26)
end

local function draw_view_nav(dl, x, y, nav_w, h, enabled)
    kit.fill(dl, x, y, nav_w, h, P.side, S(6))
    kit.outline(dl, x, y, nav_w, h, P.line, S(6))
    local iy = y + S(NAV_PAD)
    for _, section in ipairs(VIEW_SECTIONS) do
        local ix, iw, ih = x + S(NAV_PAD), nav_w - S(NAV_PAD) * 2, S(NAV_ITEM_H)
        local hovered, clicked = kit.hit('##fc.nav.' .. section.id, ix, iy, iw, ih)
        if clicked then
            state.view_section = section.id
        end
        local picked = state.view_section == section.id
        local t = kit.ease('##fc.nav.' .. section.id, picked and 1 or 0)
        if t > 0.01 then
            kit.fill(dl, ix, iy, iw, ih, P.accent_bg, S(4), 0, t)
            kit.fill(dl, ix, iy + S(9), S(3), ih - S(18), P.accent, S(1.5), 0, t)
        elseif hovered then
            kit.fill(dl, ix, iy, iw, ih, P.hover, S(4))
        end
        local cy = iy + ih / 2
        kit.icon_centred(dl, ix + S(14), cy, section.icon, kit.mix(P.dim, P.accent, t), 11)
        kit.text_mid(dl, ix + S(30), cy, section.label, kit.mix(hovered and P.text or P.text2, P.white, t), fonts.text, BODY - 0.5)
        -- a dot while its effect is on (Animation: while a clip of its is playing or held)
        if (section.switch ~= nil and enabled and fx_switched(section.switch)) or (section.lit ~= nil and section.lit()) then
            kit.circle(dl, ix + iw - S(10), cy, S(3), P.accent)
        end
        iy = iy + ih
    end
end

local function draw_view(dl, x, y, w, ctx)
    local h = view_height(ctx, w) + (ctx.view_extra or 0) -- (the list to the window's bottom, when it's dragged taller)
    -- depth of field and the colour grade: VanaCore's effects, greyed while they aren't running (/vana's Effects off, or the
    -- game's own renderer)
    local ok, info = pcall(vana.graphics.info)
    local enabled = ok and type(info) == 'table' and info.effects == true
    local nav_w = nav_width()
    draw_view_nav(dl, x, y, nav_w, h, enabled)

    local section = VIEW_SECTIONS[1]
    for _, s in ipairs(VIEW_SECTIONS) do
        if s.id == state.view_section then
            section = s
        end
    end
    local px, pw = x + nav_w + S(NAV_GAP), w - nav_w - S(NAV_GAP)
    local py = y
    if section.id == 'view' then
        names_column(VIEW_NAMES)
        -- Time's ZONE beside its slider while there's room for both (its slider kept at 220 or more)
        tag_room = math.min(S(TAG_W), math.max(0, pw - label_col - S(220)))
        py = group_heading(dl, px, py, pw, section.title, true)
        for _, draw_row in ipairs({ draw_fov, draw_weather, draw_daytime, ext.draw_facing, ext.draw_nameplates, ext.draw_scenery, ext.draw_grass }) do
            draw_row(dl, px, py + S(ROW_H) / 2, pw)
            py = py + S(ROW_H)
        end
    elseif section.id == 'anim' then
        draw_anim(dl, px, py, pw, section.title, ctx.now)
    elseif section.id == 'graphics' then
        local names = {}
        for i, r in ipairs(section.list) do
            names[i] = r.label
        end
        names_column(names)
        tag_room = 0
        py = group_heading(dl, px, py, pw, section.title, true)
        local d3d12 = ok and type(info) == 'table' and info.running == 'd3d12'
        for _, r in ipairs(section.list) do
            gfx_row(dl, px, py + S(ROW_H) / 2, pw, r, enabled, d3d12)
            py = py + S(ROW_H)
        end
    else
        local names = {}
        for i, r in ipairs(section.list) do
            names[i] = r.label
        end
        names_column(names)
        tag_room = 0
        local on
        py, on = fx_heading(dl, px, py, pw, section.title, section.switch, enabled,
                            enabled and TIPS[section.tip] or TIPS.fx_off, true)
        for _, r in ipairs(section.list) do
            fx_row(dl, px, py + S(ROW_H) / 2, pw, r, section.switch, enabled, on)
            py = py + S(ROW_H)
        end
    end
    tag_room = 0
    return y + h
end

-------------------------------------------------------------------------------
-- The sequence: its picker, transport, the timeline and the selected key
-------------------------------------------------------------------------------

local function view_span(seq)
    local length = duration(seq)
    return math.max(8, length + math.max(2, length * 0.2), state.head + 2)
end

local function finish_drag(seq)
    local drag = state.drag
    state.drag = nil
    state.scrub = false
    if drag ~= nil and drag.kind == 'key' then
        local key = seq.keys[drag.index]
        sort_keys(seq)
        state.selected = index_of(seq, key)
        touch()
        save()
    end
end

local function fmt_time(t, step)
    if t >= 60 then
        return string.format('%d:%02d', math.floor(t / 60), math.floor(t % 60))
    end
    return string.format(step < 1 and '%.1fs' or '%gs', t)
end

local function draw_timeline(dl, x0, y0, width, seq, lane_extra)
    local ruler, lane = S(TL_RULER), S(TL_LANE) + (lane_extra or 0)
    local height = ruler + lane
    imgui.SetCursorScreenPos({ x0, y0 })
    imgui.InvisibleButton('##fc.timeline', { width, height })
    local hovered = imgui.IsItemHovered()
    local clicked = imgui.IsItemClicked(0)
    local mouse_x, mouse_y = imgui.GetMousePos()
    local free = shift_down()

    local drag = state.drag
    local span = drag ~= nil and drag.span or view_span(seq)
    local left, right = x0 + S(16), x0 + width - S(16)
    local function px(t) return left + t / span * (right - left) end
    local function tm(x) return (x - left) / (right - left) * span end
    local keys = seq.keys
    local lane_top = y0 + ruler
    local cy = lane_top + lane / 2

    -- the key under the mouse
    local over = 0
    if hovered and drag == nil then
        local best = S(10)
        for i, k in ipairs(keys) do
            local d = math.abs(mouse_x - px(k.t))
            if d <= best and mouse_y >= lane_top - S(6) then
                best, over = d, i
            end
        end
    end

    -- a press: a key (selected, and dragged; twice, the camera goes there), or the playhead (the ruler, or the lane between keys)
    if clicked then
        if over > 0 then
            state.selected = over
            state.drag = { kind = 'key', index = over, span = span, offset = px(keys[over].t) - mouse_x }
            drag = state.drag
            if imgui.IsMouseDoubleClicked(0) then
                go_to(keys[over])
            end
        else
            stop_play()
            state.head = snap_time(clamp(tm(mouse_x), 0, span), free)
            state.scrub = true
            state.drag = { kind = 'head', span = span, offset = 0 }
            drag = state.drag
        end
    end
    if drag ~= nil then
        if imgui.IsMouseDown(0) then
            local t = snap_time(clamp(tm(mouse_x + drag.offset), 0, drag.span), free)
            if drag.kind == 'head' then
                state.head = t
            elseif keys[drag.index] ~= nil then
                keys[drag.index].t = t
                touch()
            end
        else
            finish_drag(seq)
            drag = nil
        end
    end

    -- the ruler and the lane
    local u32 = kit.u32
    kit.fill(dl, x0, y0, width, height, P.well, S(6))
    kit.fill(dl, x0, y0, width, ruler, P.side, S(6), kit.CORNERS.top)
    kit.outline(dl, x0, y0, width, height, P.well_edge, S(6))
    dl:PushClipRect({ x0, y0 }, { x0 + width, y0 + height }, true)

    -- the grid: a tick at every 0.25 s while they're far enough apart, taller at each whole second
    if (right - left) * SNAP / span >= S(6) then
        for i = 0, math.floor(span / SNAP) do
            local x = px(i * SNAP)
            local whole = i % 4 == 0
            dl:AddLine({ x, lane_top - S(whole and 6 or 3) }, { x, lane_top }, u32(P.faint, whole and 0.9 or 0.6), 1)
        end
    end
    local steps = { 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300 }
    local step = steps[#steps]
    for _, candidate in ipairs(steps) do
        if candidate / span * (right - left) >= S(64) then
            step = candidate
            break
        end
    end
    for i = 0, math.floor(span / step) do
        local t = i * step
        local x = px(t)
        dl:AddLine({ x, y0 + S(8) }, { x, lane_top }, u32(P.dim, 0.7), 1)
        dl:AddLine({ x, lane_top }, { x, y0 + height }, u32(P.line, 0.8), 1)
        kit.text(dl, x + S(4), y0 + S(3), fmt_time(t, step), P.dim, fonts.text, SMALL - 1.5)
    end
    dl:AddLine({ left, cy }, { right, cy }, u32(P.line), 1)

    -- where a press would put the playhead
    if hovered and drag == nil and over == 0 then
        local gx = px(snap_time(clamp(tm(mouse_x), 0, span), free))
        dl:AddLine({ gx, lane_top }, { gx, y0 + height }, u32(P.accent, 0.35), 1)
    end

    -- the path between keys, then the keys
    for i = 1, #keys - 1 do
        dl:AddLine({ px(keys[i].t), cy }, { px(keys[i + 1].t), cy }, u32(P.accent_lo), S(3))
    end
    for i, k in ipairs(keys) do
        local x = px(k.t)
        local r = S(i == state.selected and 9 or 7)
        local colour = i == state.selected and P.amber or (i == over and P.accent2 or P.experimental)
        local a, b, c, d = { x, cy - r }, { x + r, cy }, { x, cy + r }, { x - r, cy }
        dl:AddQuadFilled(a, b, c, d, u32(colour))
        dl:AddQuad(a, b, c, d, u32(P.bg), 1.5)
        local label = tostring(i)
        local w = kit.measure(label, fonts.text, SMALL - 1)
        kit.text(dl, x - w / 2, cy + r + S(1), label, P.dim, fonts.text, SMALL - 1)
    end

    -- the playhead, and its time while it (or a key) is dragged
    local hx = px(state.head)
    local colour = u32(state.playing and P.green or P.amber)
    dl:AddLine({ hx, y0 }, { hx, y0 + height }, colour, 2)
    dl:AddTriangleFilled({ hx - S(6), y0 }, { hx + S(6), y0 }, { hx, y0 + S(10) }, colour)
    if state.drag ~= nil then
        local t = state.drag.kind == 'head' and state.head or (keys[state.drag.index] and keys[state.drag.index].t)
        if t ~= nil then
            local label = string.format('%.2f s', t)
            local lw = kit.measure(label, fonts.semi, SMALL - 1) + S(12)
            local bx = clamp(px(t) - lw / 2, x0 + S(2), x0 + width - lw - S(2))
            kit.fill(dl, bx, y0 + S(3), lw, S(15), P.amber, S(4))
            kit.text_centred(dl, bx, y0 + S(3), lw, S(15), label, P.ink, fonts.semi, SMALL - 1)
        end
    end
    dl:PopClipRect()

    if over > 0 then
        local k = keys[over]
        kit.tooltip(string.format('Key %d at %.2f s\nx %.1f  y %.1f  z %.1f\nyaw %.1f  tilt %.1f deg\nDrag to retime, double-click to go there.',
            over, k.t, k.x, k.y, k.z, k.yaw * DEG, k.tilt * DEG))
    end
end

-- the selected key's row: nothing while the sequence has no keys (by the keys it had at the start of the frame: a key made or
-- deleted in it shows in the next)
local function key_height(ctx, w)
    if ctx.n == 0 then
        return 0
    end
    return S(ROW_H)
end

local function draw_keys(dl, x, y, w, ctx)
    local seq, cs = ctx.seq, ctx.cs
    local n = #seq.keys
    if ctx.n == 0 or n == 0 then
        return
    end
    local cy = y + S(ROW_H) / 2
    state.selected = clamp(state.selected, 1, n)
    local key = seq.keys[state.selected]
    kit.text_mid(dl, x + S(2), cy, 'KEY', P.faint, fonts.semi, SMALL - 1.5)
    local step = kit.arrows('##fc.key', x + S(30), cy, string.format('%d / %d', state.selected, n), true)
    if step ~= 0 then
        state.selected = step < 0 and (state.selected > 1 and state.selected - 1 or n) or (state.selected < n and state.selected + 1 or 1)
        key = seq.keys[state.selected]
    end
    local fx = x + S(30) + S(kit.ARROWS_W) + S(10)
    kit.text_mid(dl, fx, cy, 'at', P.dim, fonts.text, SMALL + 0.5)
    fx = fx + kit.measure('at', fonts.text, SMALL + 0.5) + S(8)
    state.key_time[1] = key.t
    local changed, done = field(fx, cy, S(84), function ()
        return imgui.DragFloat('##fc.keytime', state.key_time, 0.01, 0, 3600, '%.2f s')
    end)
    if changed then
        key.t = math.max(0, state.key_time[1])
        touch()
    end
    if done then
        sort_keys(seq)
        state.selected = index_of(seq, key)
        touch()
        save()
    end

    -- Go to, Set to camera and Delete, from the right
    local right = x + w
    local trash = S(TOOL_W)
    if icon_press('##fc.delkey', right - trash, cy, 'trash', P.rose, '#FFA7AE', 'Delete this key') then
        table.remove(seq.keys, state.selected)
        state.selected = clamp(state.selected, 1, #seq.keys)
        touch()
        save()
        return
    end
    -- (in a narrow window, icons alone, their names in their tooltips)
    local set_w, go_w = kit.button_width('Set to camera', 'camera'), kit.button_width('Go to', 'locate')
    local words = fx + S(84) + S(12) + go_w + S(8) + set_w + S(8) + trash <= right
    local set_pressed, go_pressed
    local function icon_only(id, at, icon, tip)
        if cs.have ~= true then
            kit.icon_centred(dl, at + S(TOOL_W) / 2, cy, icon, P.faint, 14, 0.5)
            return false
        end
        return icon_press(id, at, cy, icon, P.text2, P.accent2, tip)
    end
    local set_x
    if words then
        set_x = right - trash - S(8) - set_w
        set_pressed = kit.button('##fc.setkey', set_x, cy, 'Set to camera', 'camera', 'ghost', cs.have == true, nil, BTN_H)
        go_pressed = kit.button('##fc.gokey', set_x - S(8) - go_w, cy, 'Go to', 'locate', 'ghost', cs.have == true, nil, BTN_H)
    else
        set_x = right - trash - S(4) - S(TOOL_W)
        set_pressed = icon_only('##fc.setkey', set_x, 'camera', 'Set to camera: this key to where the camera is')
        go_pressed = icon_only('##fc.gokey', set_x - S(4) - S(TOOL_W), 'locate', 'Go to: the camera to this key')
    end
    if set_pressed then
        local px_, py_, pz_, yaw, tilt = camera.pose()
        if px_ ~= nil then
            key.x, key.y, key.z, key.yaw, key.tilt = px_, py_, pz_, yaw, tilt
            touch()
            save()
            say(string.format('Key %d set to where the camera is.', state.selected))
        end
    end
    if go_pressed then
        go_to(key)
    end
end

-- (the timeline's lane is taller by ctx.lane_extra: the window dragged taller than the tab holds)
local function sequence_height(ctx, w)
    return S(ROW_H) + S(4) + S(TL_RULER + TL_LANE) + ctx.lane_extra + S(4) + key_height(ctx, w)
end

local function seq_name(s, i)
    return (type(s.name) == 'string' and s.name ~= '') and s.name or ('Sequence ' .. i)
end

local function draw_sequence(dl, x, y, w, ctx)
    local seq = ctx.seq
    local right = x + w
    local cy = y + S(ROW_H) / 2

    -- the transport: Play (Stop while it plays), Rewind, Add key, the sequence's name (Save / Load picks another), and where
    -- the playhead is
    local playing = state.playing or state.pending_play
    local go, pw
    local play_w = math.max(kit.button_width('Play', 'play'), kit.button_width('Stop', 'stop')) -- (so what's after it stays put)
    if playing then
        go, pw = kit.button('##fc.play', x, cy, 'Stop', 'stop', 'danger', true, nil, BTN_H, play_w)
    else
        go, pw = kit.button('##fc.play', x, cy, 'Play', 'play', 'accent', true, nil, BTN_H, play_w)
    end
    if go then
        toggle_play()
    end
    local bx = x + pw + S(8)
    local rewound, rw = kit.button('##fc.rewind', bx, cy, 'Rewind', 'rewind', 'ghost', true, nil, BTN_H)
    if rewound then
        rewind()
    end
    bx = bx + rw + S(8)
    local added, aw = kit.button('##fc.addkey', bx, cy, 'Add key', 'pin', 'ghost', true, nil, BTN_H)
    if added then
        add_key()
    end
    bx = bx + aw + S(14)
    local total = string.format(' / %.2f s', duration(seq))
    local total_w = kit.measure(total, fonts.text, BODY - 0.5)
    local head = string.format('%.2f', state.head)
    kit.text_right_mid(dl, right, cy, total, P.dim, fonts.text, BODY - 0.5)
    kit.text_right_mid(dl, right - total_w, cy, head, state.playing and P.green or P.white, fonts.semi, BODY + 1)
    -- the sequence's name, small, when it fits between them; a click opens Save / Load
    local name = seq_name(seq, ctx.index)
    local name_w = kit.measure(name, fonts.text, SMALL)
    if bx + name_w <= right - total_w - kit.measure(head, fonts.semi, BODY + 1) - S(14) then
        local over, clicked = kit.hit('##fc.seqlabel', bx, cy - S(10), name_w, S(20))
        kit.text_mid(dl, bx, cy, name, over and P.text or P.dim, fonts.text, SMALL)
        if over then
            kit.tooltip(TIPS.seq_label)
        end
        if clicked then
            state.tab = 'files'
        end
    end
    y = y + S(ROW_H) + S(4)

    draw_timeline(dl, x, y, w, seq, ctx.lane_extra)
    y = y + S(TL_RULER + TL_LANE) + ctx.lane_extra + S(4)

    draw_keys(dl, x, y, w, ctx)
    return y + ctx.h.keys
end

-------------------------------------------------------------------------------
-- Save / Load: the sequences, saved to sequences.json as they change (there's no Save to press). Which one the Sequencer tab
-- shows and plays (picked: loaded), with its keys and length; its name; New, and Delete (pressed twice); a note of where they're
-- kept.
-------------------------------------------------------------------------------

local SAVED_NOTE = 'Sequences are saved as you change them, to settings\\addons\\freecam\\sequences.json in the VanaCore '
    .. 'folder. Copy that file to another install to take them with you.'

local function saveload_height(ctx, w)
    return S(HEAD_H) + S(ROW_H) * 3 + S(6) + note_height(w, SAVED_NOTE) + S(4)
end

local function draw_saveload(dl, x, y, w, ctx)
    names_column({ 'Sequence', 'Name' })
    local seq, now = ctx.seq, ctx.now
    local right = x + w
    local cx = x + label_col
    kit.text_mid(dl, x, y + S(HEAD_H) / 2 + S(2), 'SEQUENCES', P.faint, fonts.semi, SMALL - 1.5)
    y = y + S(HEAD_H)

    -- the one shown: picked, it's loaded into the Sequencer tab; its keys and length on the right
    local cy = y + S(ROW_H) / 2
    setting_name(dl, x, cy, 'folder', 'Sequence', true, true, TIPS.load)
    local list = sequences()
    local options = {}
    for i, s in ipairs(list) do
        options[i] = { i, seq_name(s, i) }
    end
    -- (narrower to leave its keys and length room, down to 140; past that they're left out)
    local n = #seq.keys
    local info = string.format('%d key%s, %.2f s', n, n == 1 and '' or 's', duration(seq))
    local info_w = kit.measure(info, fonts.text, SMALL)
    local list_w = math.max(S(140), math.min(S(220), right - cx - info_w - S(12)))
    local picked, moved, list_right = kit.dropdown('##fc.pick', cx, cy, list_w, options, ctx.index, true, nil, true, true)
    if moved then
        data.current = picked
        stop_play()
        state.head, state.selected = 0, 1
        save()
    end
    if right - info_w >= list_right + S(12) then
        kit.text_right_mid(dl, right, cy, info, P.text2, fonts.text, SMALL)
    end
    y = y + S(ROW_H)

    -- its name, saved as the box is left
    cy = y + S(ROW_H) / 2
    setting_name(dl, x, cy, 'pen', 'Name', true, true, TIPS.name)
    if state.name_for ~= seq then
        state.name_buf[1], state.name_for = seq.name, seq
    end
    local renamed, named = field(cx, cy, right - cx, function ()
        return imgui.InputTextWithHint('##fc.seqname', 'Name', state.name_buf, 64)
    end)
    if renamed then
        seq.name = state.name_buf[1]
    end
    if named then
        save()
    end
    y = y + S(ROW_H)

    -- New (an empty one, loaded) and Delete (asked twice)
    cy = y + S(ROW_H) / 2
    local new, nw = kit.button('##fc.new', cx, cy, 'New', 'plus', 'ghost', true, nil, BTN_H)
    if kit.over(cx, cy - S(11), nw, S(22)) then
        kit.tooltip(TIPS.new)
    end
    if new then
        list[#list + 1] = new_sequence('Sequence ' .. (#list + 1))
        data.current = #list
        stop_play()
        state.head, state.selected = 0, 1
        touch()
        save()
    end
    local bx = cx + nw + S(8)
    local sure = now < state.confirm_delete
    local del, dw = kit.button('##fc.delseq', bx, cy, sure and 'Sure?' or 'Delete', 'trash', sure and 'danger' or 'ghost', true, nil, BTN_H)
    if kit.over(bx, cy - S(11), dw, S(22)) and not sure then
        kit.tooltip('Deletes this sequence and its keys. Press twice.')
    end
    if del then
        if sure then
            table.remove(list, ctx.index)
            data.current = clamp(ctx.index, 1, math.max(1, #list))
            stop_play()
            state.head, state.selected, state.confirm_delete = 0, 1, 0
            touch()
            save()
        else
            state.confirm_delete = now + 3
        end
    end
    y = y + S(ROW_H) + S(6)
    return y + draw_note(dl, x, y, w, 'info', SAVED_NOTE) + S(4)
end

-------------------------------------------------------------------------------
-- Help: the keys, as keycaps with what they do, wrapped to their column. The Help tab.
-------------------------------------------------------------------------------

-- the window's own keys
local WINDOW_KEYS = {
    { caps = { 'Esc' }, text = 'Restores this window once its minus has hidden it' },
    { caps = { 'F11' }, text = 'Hides the interface, this window too; the sequence plays on' },
}
local WINDOW_NOTE = 'The X at the top right exits the free camera and closes this window. The minus beside it only hides '
    .. 'the window: the free camera stays on. Press the ESCAPE key to restore it.'

local FLY = {
    { caps = { 'W', 'A', 'S', 'D' }, text = 'Fly forward, left, back and right' },
    { caps = { 'E', 'Space' }, text = 'Up' },
    { caps = { 'Q', 'C' }, text = 'Down' },
    { caps = { 'Shift' }, text = 'Four times as fast' },
    { caps = { 'Right-drag' }, text = 'Look; the wheel sets the speed' },
    { caps = { 'V' }, text = 'First person, the game\'s own view: it hides your character' },
}

local shortcut_cache = { at = -10, left = {}, note = '' } -- left: the left column's groups, each { title, items }

-- the keys for the sequence, from the settings once a second
local function shortcuts()
    local now = vana.time.clock()
    if now - shortcut_cache.at >= 1 then
        shortcut_cache.at = now
        local add, play = key_setting('key.add', 'r'), key_setting('key.play', 'p')
        local keys = {}
        if add ~= nil then
            keys[#keys + 1] = { caps = { add:upper() }, text = 'Records a key where the camera is' }
        end
        if play ~= nil then
            keys[#keys + 1] = { caps = { play:upper() }, text = 'Plays the sequence, or stops it' }
        end
        if data.space_play == true then
            keys[#keys + 1] = { caps = { 'Space' }, text = 'Plays the sequence, or stops it (the switch in the toolbar)' }
        end
        -- (the sequence's has no keys while both are off and Space isn't the toolbar's)
        local left = { { 'WINDOW', WINDOW_KEYS } }
        if #keys > 0 then
            left[#left + 1] = { 'SEQUENCE', keys }
        end
        shortcut_cache.left = left
        shortcut_cache.note = (add ~= nil or play ~= nil)
            and 'Record and play are binds that exist only while the free camera is on and the chat line is shut. [freecam] key.add and key.play in vanacore.ini change them.'
            or 'The buttons in the sequence do the same.'
    end
    return shortcut_cache
end

local function cap_width(label)
    return kit.measure(label, fonts.semi, SMALL - 0.5) + S(10)
end

local function caps_width(item)
    local total = 0
    for i, label in ipairs(item.caps) do
        total = total + cap_width(label) + (i > 1 and S(3) or 0)
    end
    return total
end

local function item_lines(item, column_w)
    return wrap_text(item.text, column_w - caps_width(item) - S(10), NOTE_SIZE)
end

local function item_height(item, column_w)
    return math.max(S(24), #item_lines(item, column_w) * S(NOTE_LINE) + S(6))
end

local SUB_H = 22 -- a column's small title

local function column_height(items, column_w)
    local h = S(SUB_H)
    for _, item in ipairs(items) do
        h = h + item_height(item, column_w)
    end
    return h
end

local GROUP_GAP = 8 -- between a column's groups

local function key_groups_height(groups, column_w)
    local h = 0
    for i, group in ipairs(groups) do
        h = h + (i > 1 and S(GROUP_GAP) or 0) + column_height(group[2], column_w)
    end
    return h
end

local function help_height(ctx, w)
    local column_w = (w - S(COL_GAP)) / 2
    local cache = shortcuts()
    return math.max(key_groups_height(cache.left, column_w), column_height(FLY, column_w)) + S(4)
        + note_height(w, WINDOW_NOTE) + note_height(w, cache.note)
end

local function draw_column(dl, x, y, column_w, title, items)
    kit.text_mid(dl, x, y + S(SUB_H) / 2, title, P.dim, fonts.semi, SMALL - 1.5)
    y = y + S(SUB_H)
    for _, item in ipairs(items) do
        local ih = item_height(item, column_w)
        local cy = y + S(12)
        local cx = x
        for i, label in ipairs(item.caps) do
            if i > 1 then
                cx = cx + S(3)
            end
            cx = cx + kit.keycap(dl, cx, cy, label)
        end
        local ty = cy
        for _, line in ipairs(item_lines(item, column_w)) do
            kit.text_mid(dl, x + caps_width(item) + S(10), ty, line, P.text2, fonts.text, NOTE_SIZE)
            ty = ty + S(NOTE_LINE)
        end
        y = y + ih
    end
end

local function draw_help(dl, x, y, w, ctx)
    local column_w = (w - S(COL_GAP)) / 2
    local cache = shortcuts()
    local gy = y
    for i, group in ipairs(cache.left) do
        gy = gy + (i > 1 and S(GROUP_GAP) or 0)
        draw_column(dl, x, gy, column_w, group[1], group[2])
        gy = gy + column_height(group[2], column_w)
    end
    draw_column(dl, x + column_w + S(COL_GAP), y, column_w, 'FLYING', FLY)
    y = y + math.max(key_groups_height(cache.left, column_w), column_height(FLY, column_w)) + S(4)
    y = y + draw_note(dl, x, y, w, 'info', WINDOW_NOTE)
    return y + draw_note(dl, x, y, w, 'info', cache.note)
end

-------------------------------------------------------------------------------
-- The window
-------------------------------------------------------------------------------

local function status_height(ctx, w)
    if ctx.now >= state.status_until or state.status == '' then
        return 0
    end
    return #wrap_text(state.status, w - S(NOTE_INSET) - S(16), NOTE_SIZE) * S(NOTE_LINE) + S(16)
end

local function draw_status(dl, x, y, w, h)
    local colour = P.amber
    kit.fill(dl, x, y, w, h, colour, S(6), 0, 0.09)
    kit.outline(dl, x, y, w, h, colour, S(6), 0, 1, 0.28)
    kit.fill(dl, x, y, S(3), h, colour, S(6), kit.CORNERS.left)
    local lines = wrap_text(state.status, w - S(NOTE_INSET) - S(16), NOTE_SIZE)
    local ly = y + S(8) + S(NOTE_LINE) / 2
    kit.icon_centred(dl, x + S(16), ly, 'info', colour, 10)
    for _, line in ipairs(lines) do
        kit.text_mid(dl, x + S(NOTE_INSET) + S(8), ly, line, P.text, fonts.text, NOTE_SIZE)
        ly = ly + S(NOTE_LINE)
    end
end

-- each part's height, once a frame (a press in the frame that changes what a part holds is for the next one to show)
local function measure_parts(ctx, w)
    ctx.h = {
        toolbar = toolbar_height(ctx, w), sequence = sequence_height(ctx, w), help = help_height(ctx, w),
        settings = settings_height(ctx, w), files = saveload_height(ctx, w), view = view_height(ctx, w),
        status = status_height(ctx, w), keys = key_height(ctx, w),
    }
end

-- as tall as the tab shown holds: Sequencer the toolbar, a hairline and the sequence; the others their rows. The status line
-- under any.
local SEQ_GAP = 12 -- between the toolbar and the sequence, the hairline in it

local function window_height(ctx)
    local h = ctx.h
    local total = S(TITLE_H) + S(TABS_H)
    if ctx.tab == 'settings' then
        total = total + h.settings
    elseif ctx.tab == 'files' then
        total = total + h.files
    elseif ctx.tab == 'view' then
        total = total + h.view
    elseif ctx.tab == 'help' then
        total = total + h.help
    else
        total = total + h.toolbar + S(SEQ_GAP) + h.sequence
    end
    if h.status > 0 then
        total = total + S(8) + h.status
    end
    return total + S(PAD)
end

-- where the camera is, small, in the title's right: X Y Z, where it looks, and how fast it flies (a number: the wheel sets it)
local function draw_pose(dl, right, cy, pose, speed, left)
    local names = { 'X', 'Y', 'Z', 'YAW', 'TILT', 'SPEED' }
    local values = {
        string.format('%.1f', pose[1]), string.format('%.1f', pose[2]), string.format('%.1f', pose[3]),
        string.format('%.1f\194\176', pose[4] * DEG), string.format('%.1f\194\176', pose[5] * DEG), string.format('%.1f', speed),
    }
    local name_size, value_size = SMALL - 1.5, SMALL
    local gap, inner = S(12), S(4)
    -- the gaps close up (to 4 pixels) before the numbers run into the title; then the last ones are left out (SPEED, TILT,
    -- YAW...), in a narrow window
    local total
    while true do
        total = 0
        for i = 1, #names do
            total = total + kit.measure(names[i], fonts.semi, name_size) + inner + kit.measure(values[i], fonts.text, value_size)
        end
        if #names == 0 or total + S(4) * (#names - 1) <= right - left then
            break
        end
        names[#names], values[#values] = nil, nil
    end
    if #names == 0 then
        return
    end
    gap = #names > 1 and math.max(S(4), math.min(gap, (right - left - total) / (#names - 1))) or 0
    total = total + gap * (#names - 1)
    local x = right - total
    for i = 1, #names do
        kit.text(dl, x, label_top(cy, names[i], fonts.semi, name_size, values[i], fonts.text, value_size), names[i], P.faint,
                 fonts.semi, name_size)
        x = x + kit.measure(names[i], fonts.semi, name_size) + inner
        kit.text_mid(dl, x, cy, values[i], P.text2, fonts.text, value_size)
        x = x + kit.measure(values[i], fonts.text, value_size) + gap
    end
end

-- the title's X: out of the free camera (the game's camera back), the window shut with it. With the camera off (the window
-- opened by /freecam window), the window alone
local function exit_freecam(ctx)
    state.open, state.auto, state.hidden = false, false, false
    if state.drag ~= nil then
        finish_drag(ctx.seq)
    end
    if ctx.cs.on then
        camera.free(false)
    end
end

-- the title's minus, there while the free camera is on: the window is left open and not drawn till Escape (the key event
-- below), /freecam window or the free camera going off. The chat log says how to get it back, once a game
local function hide_window(ctx)
    state.hidden = true
    if state.drag ~= nil then
        finish_drag(ctx.seq)
    end
    if not state.hide_told then
        state.hide_told = true
        print('Press ESCAPE key to restore the freecam window')
    end
end

local function draw_title(dl, x, y, pw, ctx)
    local cy = y + S(TITLE_H) / 2
    local lx = x + S(PAD)
    kit.icon_centred(dl, lx + S(5), cy, 'video', P.accent, 11)
    local title = 'Free camera'
    kit.text_mid(dl, lx + S(18), cy, title, P.white, fonts.semi, BODY + 2)
    local on = ctx.cs.on
    if ctx.pose[1] ~= nil then
        local left = lx + S(18) + kit.measure(title, fonts.semi, BODY + 2) + S(16)
        draw_pose(dl, x + pw - S(PAD) - S(on and 46 or 24), cy + S(1), ctx.pose, ctx.cs.speed, left)
    end
    -- the minus hides the window (Escape shows it again: the free camera's own key, so only while it's on); the X exits
    local hide, over_hide = false, false
    if on then
        hide, over_hide = kit.icon_button('##fc.hide', x + pw - S(PAD) - S(22), cy, 'minus', 11, P.accent)
    end
    local exit, over_exit = kit.icon_button('##fc.close', x + pw - S(PAD), cy, 'close', 11, P.rose)
    if over_hide then
        kit.tooltip(TIPS.hide)
    elseif over_exit then
        kit.tooltip(on and TIPS.exit or TIPS.close)
    end
    if exit then
        exit_freecam(ctx)
    elseif hide then
        hide_window(ctx)
    end
    kit.hline(dl, x, x + pw, y + S(TITLE_H), P.line)
end

local function draw_contents(dl, x, y, pw, ctx)
    local lx, w = x + S(PAD), pw - S(PAD) * 2
    draw_title(dl, x, y, pw, ctx)
    local sy = y + S(TITLE_H)

    -- each part returns where it ended: a part that draws more or less than it said it would is logged (once)
    local function expect(name, ended, from, height)
        if ended ~= nil and math.abs(ended - (from + height)) > 1 then
            fail('layout', string.format('%s ended %.1f pixels from where it said', name, ended - (from + height)))
        end
    end

    -- the tabs: the one picked shows from the next frame (each part's height was measured for this one)
    local index = 2
    for i, id in ipairs(TAB_IDS) do
        if id == ctx.tab then
            index = i
        end
    end
    local picked, moved = kit.segments('##fc.tabs', lx, sy + S(TABS_H) / 2, TAB_LABELS, index, true)
    if moved and picked ~= index then
        state.tab = TAB_IDS[picked]
    end
    sy = sy + S(TABS_H)

    local h = ctx.h
    if ctx.tab == 'settings' then
        expect('settings', draw_settings(dl, lx, sy, w, ctx), sy, h.settings)
        sy = sy + h.settings
    elseif ctx.tab == 'files' then
        expect('save / load', draw_saveload(dl, lx, sy, w, ctx), sy, h.files)
        sy = sy + h.files
    elseif ctx.tab == 'view' then
        expect('view & effects', draw_view(dl, lx, sy, w, ctx), sy, h.view)
        sy = sy + h.view
    elseif ctx.tab == 'help' then
        expect('help', draw_help(dl, lx, sy, w, ctx), sy, h.help)
        sy = sy + h.help
    else
        expect('toolbar', draw_toolbar(dl, lx, sy, w, ctx), sy, h.toolbar)
        sy = sy + h.toolbar
        kit.hline(dl, lx, lx + w, sy + S(4), P.line)
        sy = sy + S(SEQ_GAP)
        expect('sequence', draw_sequence(dl, lx, sy, w, ctx), sy, h.sequence)
        sy = sy + h.sequence
    end

    if h.status > 0 then
        draw_status(dl, lx, sy + S(8), w, h.status)
    end
end

-- the window's size at 100%: [freecam] width and height (no height, or one under what the tab holds: as tall as that), read once
local function saved_size()
    if state.size == nil then
        local w = tonumber(vana.config.get('freecam', 'width') or '') or WIN_W
        local h = tonumber(vana.config.get('freecam', 'height') or '') or 0
        state.size = { math.max(MIN_W, w), math.max(0, h) }
    end
    return state.size
end

-- The resize corner, bottom right in the window's padding, as /vana's and API Explorer's: three short lines across it, lit under
-- the mouse; dragged, the window's corner follows the mouse (taken at the next frame: no narrower than MIN_W, no shorter than
-- the tab holds), kept as it's let go
local function resize_corner(dl, x, y, pw, ph, screen_w, screen_h)
    local g = S(GRIP)
    local hovered, _, active = kit.hit('##fc.resize', x + pw - g, y + ph - g, g, g)
    if active then
        local mx, my = imgui.GetMousePos()
        state.grab = state.grab or { x + pw - mx, y + ph - my }
        local w = (mx + state.grab[1] - x) / kit.scale
        local h = (my + state.grab[2] - y) / kit.scale
        state.size = { math.floor(clamp(w, MIN_W, math.max(MIN_W, screen_w / kit.scale)) + 0.5),
                       math.floor(clamp(h, 0, screen_h / kit.scale) + 0.5) }
    elseif state.grab ~= nil then
        state.grab = nil
        vana.config.set('freecam', 'width', tostring(state.size[1]))
        vana.config.set('freecam', 'height', tostring(state.size[2]))
    end
    if hovered or active then
        imgui.SetMouseCursor(6) -- ImGuiMouseCursor_ResizeNWSE
    end
    local colour = kit.u32((hovered or active) and P.accent or P.dim)
    local right, bottom = x + pw - S(5), y + ph - S(5)
    for _, k in ipairs({ 4, 8, 12 }) do
        dl:AddLine({ right - S(k), bottom }, { right, bottom - S(k) }, colour, math.max(1, S(1.25)))
    end
    if hovered and not active then
        kit.tooltip(TIPS.resize)
    end
end

local function draw(now)
    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
    -- the animations' clock and the size, kept here (the /vana window isn't what runs them)
    local st = kit.st
    st.dt = math.max(0, math.min(0.1, now - st.last))
    st.last = now
    kit.compute_scale(now, screen_h, user_percent())
    if not fonts.ready then
        fonts.ready = true
        pcall(kit.load_fonts)
        return
    end
    local seq, index = current()
    local ctx = { now = now, cs = camera.state(), seq = seq, index = index, n = #seq.keys, pose = { camera.pose() },
                  tab = state.tab, lane_extra = 0, view_extra = 0 }
    local size = saved_size()
    local pw = S(size[1])
    measure_parts(ctx, pw - S(PAD) * 2)
    local fit = window_height(ctx)
    local want = S(size[2])
    local ph = math.max(fit, want)
    -- dragged taller than the tab holds: Sequencer's timeline lane takes the room, and View & Effects' list of sections reaches
    -- down to the bottom (a message under either still adds its own, so they don't shrink while one shows)
    local status = ctx.h.status > 0 and S(8) + ctx.h.status or 0
    local extra = math.max(0, want - (fit - status))
    if ctx.tab == 'sequencer' then
        ctx.lane_extra = extra
        ctx.h.sequence = ctx.h.sequence + extra
        ph = fit + extra
    elseif ctx.tab == 'view' then
        ctx.view_extra = extra
        ctx.h.view = ctx.h.view + extra
        ph = fit + extra
    end

    local dw, dh, k = kit.display(screen_w, screen_h) -- ImGui's pixels, where it's drawn k times its size
    imgui.SetNextWindowPos({ dw - (pw + S(40)) * k, S(120) * k }, kit.FIRST_USE_EVER)
    imgui.SetNextWindowSize({ pw, ph }, 0)
    imgui.PushStyleVar(kit.VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(kit.VAR.WindowRounding, 0)
    imgui.PushStyleVar(kit.VAR.WindowBorderSize, 0)
    imgui.PushStyleVar(kit.VAR.ItemSpacing, { 0, 0 })
    if imgui.Begin('Free camera###vana_freecam', nil, kit.WINDOW_FLAGS) then
        local x, y = imgui.GetWindowPos()
        -- kept on the screen, as the /vana window is
        local cx = math.max(0, math.min(x, dw - pw * k))
        local cy = math.max(0, math.min(y, dh - ph * k))
        local clipped = false
        if cx ~= x or cy ~= y then
            imgui.SetWindowPos({ cx, cy }, 0)
            x, y = imgui.GetWindowPos()
            imgui.PushClipRect({ 0, 0 }, { dw, dh }, false)
            clipped = true
        end
        local dl = imgui.GetWindowDrawList()
        local radius = S(6)
        kit.fill(dl, x - 1, y, pw + 2, ph + 2, '#000000', radius + 1, 0, 0.35)
        kit.fill(dl, x, y, pw, ph, P.bg, radius)
        kit.outline(dl, x, y, pw, ph, P.border, radius)
        local ok, err = pcall(draw_contents, dl, x, y, pw, ctx)
        if not ok then
            fail('drawing the window', err)
        end
        ok, err = pcall(resize_corner, dl, x, y, pw, ph, screen_w, screen_h)
        if not ok then
            fail('the resize corner', err)
        end
        if clipped then
            imgui.PopClipRect()
        end
    end
    imgui.End()
    imgui.PopStyleVar(4)
end

-------------------------------------------------------------------------------
-- Events and /freecam's words for the window and the sequencer
-------------------------------------------------------------------------------

vana.events.on('present', function ()
    local now = vana.time.clock()
    local ok, err = pcall(function ()
        local on = camera.state().on
        if on ~= state.binds_on then
            set_binds(on)
            -- /freecam shows the window with the camera, and takes it away with it, unless Play turned the camera on
            if on and not state.open and not state.quiet_on then
                state.open, state.auto = true, true
            elseif not on and state.auto then
                state.open, state.auto = false, false
            end
            state.quiet_on = false
            if not on then
                state.hidden = false -- (Escape shows a hidden window only with the camera on: one of the player's own comes back)
                restore_scenery() -- the tree's far scenery goes with the free camera
                restore_fx()      -- and View & Effects' depth of field and colour, the user's own put back
                anim_off()        -- and its Animation: the game's own motion back, the files it added taken off
                if weather ~= nil and weather.held() ~= nil then
                    set_weather(nil) -- and so does Weather's, the zone's own put back
                end
                if daytime ~= nil and daytime.held() ~= nil then
                    set_daytime(nil) -- and Time's, the zone's own hour back
                end
            end
        end
        -- the window opens shown, not hidden; shut with the camera off, it takes the tree's far scenery and View & Effects'
        -- changes with it (and at the start, those a game that ended with the free camera on left in vanacore.ini)
        if state.open and not state.shown then
            state.hidden = false
            state.tab = 'sequencer' -- (it opens on Sequencer, whichever tab it was shut on)
        elseif not state.open and not on then
            restore_scenery()
            restore_fx()
            if anim ~= nil and (anim.active() or state.facing_was ~= nil) then
                anim_off()
            end
        end
        state.shown = state.open
        -- Animation's files as they finish loading, and off again after Stop (each frame while anything of its is on the
        -- character, the window shown or not)
        state.anim_loaded = anim == nil or not anim.active() or anim.loaded()
        update(now)
    end)
    if not ok then
        fail('present', err)
    end
    if state.open and not state.hidden then
        ok, err = pcall(draw, now)
        if not ok then
            fail('drawing', err)
        end
    end
    -- View & Effects' changes this frame, taken up by the renderer at once (as /vana's sliders are, once a frame)
    if state.fx_dirty then
        state.fx_dirty = false
        pcall(vana.graphics.reload)
    end
    local got, io = pcall(imgui.GetIO)
    state.typing = got and io ~= nil and io.WantTextInput == true
end)

-- Escape with the free camera on and its window hidden (its minus) or shut (/freecam close, or Play having turned the camera
-- on): the window again (one that was shut goes with the camera then, as /freecam's). Not while typing (the chat line, an
-- ImGui box) or with a game menu open: Escape is the game's then, to shut it. Taken, so the game doesn't also have it.
vana.events.on('key', function (wparam, lparam, blocked)
    if wparam ~= 0x1B or blocked or bit.band(lparam, 0xC0000000) ~= 0 then -- (a release, or a held key's repeat)
        return false
    end
    if (state.open and not state.hidden) or state.typing or not camera.state().on then
        return false
    end
    local world, target = vana.game.world(), vana.game.target()
    if (world ~= nil and world.chat_open) or (target ~= nil and target.menu_open) then
        return false
    end
    if not state.open then
        state.open, state.auto = true, true
    end
    state.hidden = false
    return true
end)

vana.events.on('command', function (e)
    local words = {}
    for word in e.command:gmatch('%S+') do
        words[#words + 1] = word
    end
    local command = words[1] and words[1]:lower() or nil
    -- (VanaCore's own /freecam, on or off, comes first and leaves these words to this addon)
    if command ~= '//freecam' and command ~= '/freecam' then
        return false
    end
    local what = words[2] and words[2]:lower() or nil
    if what == nil then
        return false
    end
    if what == 'window' or what == 'open' or what == 'close' then
        state.auto = false -- the player's own: the free camera going off doesn't take it away
    end
    if what == 'window' then
        if state.open and state.hidden then
            state.hidden = false -- hidden, the window shows rather than shuts
        else
            state.open = not state.open
        end
    elseif what == 'close' then
        state.open = false
    elseif what == 'open' then
        state.open = true
        state.hidden = false
    elseif what == 'key' or what == 'add' then
        if not (e.injected and state.typing) then
            add_key()
        end
    elseif what == 'play' then
        if not (e.injected and state.typing) then
            toggle_play()
        end
    elseif what == 'stop' then
        stop_play()
    elseif what == 'rewind' then
        rewind()
    elseif what == 'seq' then
        local list = sequences()
        local wanted = table.concat(words, ' ', 3)
        local index = tonumber(wanted)
        if index == nil then
            for i, s in ipairs(list) do
                if type(s.name) == 'string' and s.name:lower() == wanted:lower() then
                    index = i
                end
            end
        end
        if index ~= nil and list[math.floor(index)] ~= nil then
            data.current = math.floor(index)
            stop_play()
            state.head, state.selected = 0, 1
            save()
        else
            print('Free camera: no sequence "' .. wanted .. '" (1 to ' .. #list .. ')')
        end
    elseif what == 'weather' then
        -- /freecam weather <name|0-19|off>: a weather on this screen alone; alone, which it is and the names
        local wanted = table.concat(words, ' ', 3)
        local word = wanted:lower()
        if weather == nil then
            print("Free camera: Weather couldn't load (vanacore.log says why).")
        elseif wanted == '' then
            local held, now = weather.held(), weather.now()
            print('Free camera: the weather is ' .. (now ~= nil and weather.name(now) or 'not known yet')
                  .. (held ~= nil and (" on your screen (the zone's own is " .. weather.name(weather.zone()) .. ')') or " (the zone's own)")
                  .. '. /freecam weather <name or 0 to 19> shows another on your screen only, /freecam weather off puts the '
                  .. "zone's own back. Weathers: " .. weather.list() .. '.')
        elseif word == 'off' or word == 'zone' or word == 'real' or word == 'reset' then
            if weather.held() == nil then
                print("Free camera: the weather is already the zone's own.")
            end
            set_weather(nil)
        else
            local id, why = weather.find(wanted)
            if id == nil then
                print('Free camera: ' .. why .. '. Weathers: ' .. weather.list() .. ' (or 0 to 19), or off.')
            elseif set_weather(id) then
                local set = weather.in_zone(weather.zone_id())
                if set ~= nil and not set[id] then
                    print("Free camera: this zone's files have no look for " .. weather.name(id) .. ': it may look like clear '
                          .. 'skies here, or like another weather.')
                end
            end
        end
    elseif what == 'time' then
        -- /freecam time <hour|hh:mm|word|off>: an hour of the day on this screen alone, held; alone, which it is
        local wanted = table.concat(words, ' ', 3)
        local word = wanted:lower()
        if daytime == nil then
            print("Free camera: Time couldn't load (vanacore.log says why).")
        elseif wanted == '' then
            local held, now = daytime.held(), daytime.now()
            print('Free camera: the time is ' .. daytime.name(now)
                  .. (held ~= nil and (", held on your screen (the zone's own is " .. daytime.name(daytime.zone()) .. ')') or " (the zone's own)")
                  .. '. /freecam time <' .. daytime.words() .. '> holds another on your screen only, /freecam time off brings '
                  .. "the zone's own back.")
        elseif word == 'off' or word == 'zone' or word == 'real' or word == 'reset' then
            if daytime.held() == nil then
                print("Free camera: the time is already the zone's own.")
            end
            set_daytime(nil)
        else
            local minute, why = daytime.find(wanted)
            if minute == nil then
                print('Free camera: ' .. why .. '. Times: ' .. daytime.words() .. ', or off.')
            else
                set_daytime(minute)
            end
        end
    else
        return false
    end
    return true
end)

vana.events.on('unload', function ()
    stop_play()
    restore_scenery()
    restore_fx()
    anim_off()
    if weather ~= nil then
        weather.off(true)
    end
    if daytime ~= nil then
        daytime.off()
    end
    if state.driving then
        camera.hold(false)
        state.driving = false
    end
    set_binds(false)
end)
