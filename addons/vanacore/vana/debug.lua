--[[
vana\debug.lua: the Debug window (/vana debug, or Debug under the /vana window's Tools), tools for finding out why
something goes wrong, each on a tab of its own. Loaded by vana.lua with the shared table M; drawn each frame it's open, with
the /vana window open or not, in the same look (draw.lua).

  Debug views        one effect's part of the picture alone ([graphics.effects] debug, M.DEBUG_VIEW): never saved to the
                     settings file, so every game starts with none (ini.cpp session_only). Reflections (17), Reflection
                     ground (18, the zone's floors by kind) and Rain (31) have their colours under it. Shadow Layers beside
                     it (a part of the sun's shadows off, M.SUN_GATE), and Freeze rain under them ([graphics] rain.freeze,
                     d3d8_rain.h rain_tick, never saved)
  Collision          the zone's whole collision drawn over the world, see-through ([graphics] collision.show, floors.c):
                     walls red, floors by their kind of ground in Reflection ground's colours. Never saved either
  Grass              Mass Grass's view of the tufts a floor covers ([graphics] grass.cover.show, grass.c), with its
                     colours. Never saved (its rings are on the Rings tab)
  DLSS crash tests  the DLSS helper (dlss\VanaCore.Dlss.exe) made to fail at its next frame, each a different way (an
                     access violation, one on another thread, an unhandled C++ exception, a fail-fast, a hang), to try what
                     a real crash leaves: the chat's line, its report in logs\vanacore.log, the dump. Picked from a
                     list; Trigger asks twice.
                     vana.graphics.dlss_crash_test; DirectX 12 with DLSS or FSR running.
  Shadow & AO rings  where the shadows' and the occlusion's rings fall (sun_rings, ao_rings: fewer taps past each one out
                     from the camera) and the taps each takes: Show rings tints the picture by ring (the Debug view's 15 and
                     16), with each one's switch and spacing beside it, the Graphics page's own settings; then Mass
                     Grass's rings (grass.rings.show: circles on the ground, never saved) with where they fall
  Frame recording    /vana debug <seconds> and /vana debug stop (debugrec.cpp): each frame's time and where it went, into
                     logs\frames-*.csv and logs\debug-*.log

A new tool is a tab in TABS: its word on the tab, its full name (the tab's tooltip), its height and how it draws.
--]]

local M = ...
local imgui = vana.imgui
local S, P, fonts = M.S, M.P, M.fonts
local BODY, SMALL = M.BODY, M.SMALL
local st = M.st

M.debug = { open = false, tab = 'views' } -- tab: the one shown, kept till the game closes
local dbg = M.debug

local WIN_W = 560
local PAD = 18
local TITLE_H = 40
local TABS_H = 40 -- the tabs under the title
local ROW_H = 30  -- a button and its words
local NOTE_H = 20 -- a line of small text

local function clock()
    return vana.time.clock()
end

-------------------------------------------------------------------------------
-- DLSS crash tests
-------------------------------------------------------------------------------

local TESTS = {
    { kind = 'crash', label = 'Crash', words = 'An access violation in the helper: the full report and a dump.' },
    { kind = 'thread', label = 'Other thread', words = 'The same on another of its threads, as a driver\'s would be.' },
    { kind = 'cpp', label = 'C++ exception', words = 'An unhandled C++ exception, named in the report.' },
    { kind = 'fastfail', label = 'Fail-fast', words = 'Windows ends it at once: no report, the chat still has the code.' },
    { kind = 'hang', label = 'Hang', words = 'It stops answering: DLSS turns off within 3 s, the game goes on.' },
}

-- ... as the list's { value, label }
local TEST_OPTIONS = {}
for i, t in ipairs(TESTS) do
    TEST_OPTIONS[i] = { t.kind, t.label }
end

local crash = {
    pick = 'crash',    -- the test the list has chosen
    armed = nil,       -- { kind, until }: Trigger pressed once (pressed again before then, it runs)
    said = nil,        -- { text, colour, until }: what the last one did
    info = nil, info_at = -1, -- vana.graphics.info(), read twice a second
}

-- the upscaler's state as the /vana window's DLSS line has it, and whether a test can run now
local function crash_state(now)
    if now - crash.info_at >= 0.5 then
        crash.info_at = now
        local ok, info = pcall(vana.graphics.info)
        crash.info = ok and info or nil
    end
    local info = crash.info
    if info == nil then
        return 'VanaCore\'s renderer isn\'t running', false
    end
    if info.running ~= 'd3d12' then
        return 'Needs DirectX 12 (the game is on ' .. tostring(info.running or info.renderer) .. ')', false
    end
    if info.upscaler == 'fsr' then
        return 'FSR: ' .. tostring(info.fsr), true
    elseif info.upscaler == 'dlss' then
        return 'DLSS: ' .. tostring(info.dlss), true
    end
    return 'DLSS and FSR are off: turn one on (Graphics > DLSS) to test its helper', false
end

local function crash_height()
    return S(NOTE_H) + S(ROW_H) + S(NOTE_H) + S(NOTE_H) * 2 + S(8)
end

local function crash_draw(dl, x, y, w, now)
    local state, ready = crash_state(now)
    local cy = y + S(NOTE_H) / 2
    M.circle(dl, x + S(4), cy, S(3), ready and P.green or P.faint)
    M.text_mid(dl, x + S(14), cy, M.fit(state, w - S(14), fonts.text, SMALL + 0.5), ready and P.text2 or P.dim, fonts.text, SMALL + 0.5)
    y = y + S(NOTE_H)

    -- the test picked, Trigger beside it, and what it does under them; the list as wide as its longest name (M.dropdown's
    -- padding and chevron), so it doesn't move with the pick
    local dw = 0
    for _, t in ipairs(TESTS) do
        dw = math.max(dw, M.measure(t.label, fonts.text, M.SEG_SIZE))
    end
    dw = dw + S(36)
    local ry = y + S(ROW_H) / 2
    local pick, picked = M.dropdown('##debug.crash.test', x, ry, dw, TEST_OPTIONS, crash.pick, true, nil, true, true)
    if picked then
        crash.pick, crash.armed = pick, nil
    end
    local t = TESTS[1]
    for _, test in ipairs(TESTS) do
        if test.kind == crash.pick then
            t = test
        end
    end

    local bw = math.max(M.button_width('Trigger', nil), M.button_width('Again to run', nil))
    local asking = crash.armed ~= nil and crash.armed[1] == t.kind and now < crash.armed[2]
    local clicked = M.button('##debug.crash.run', x + dw + S(8), ry, asking and 'Again to run' or 'Trigger', nil,
                             asking and 'danger' or 'ghost', ready, nil, nil, bw)
    if imgui.IsItemHovered() then
        M.tooltip(ready and 'Press twice: the helper ' .. (t.kind == 'hang' and 'hangs' or 'fails') .. ' at its next frame'
                  or 'Needs DLSS or FSR running on DirectX 12')
    end
    if clicked then
        if not asking then
            crash.armed = { t.kind, now + 5 }
        else
            crash.armed = nil
            local ok, done, why = pcall(vana.graphics.dlss_crash_test, t.kind)
            if ok and done then
                crash.said = { 'Asked: the helper ' .. (t.kind == 'hang' and 'hangs' or 'fails') .. ' at its next frame. '
                               .. 'The chat and logs\\vanacore.log say what it left.', P.teal, now + 12 }
            else
                crash.said = { 'Not run: ' .. tostring(ok and why or done), P.rose, now + 8 }
            end
        end
    end
    y = y + S(ROW_H)
    M.text_mid(dl, x, y + S(NOTE_H) / 2, M.fit(t.words, w, fonts.text, SMALL + 0.5), P.text2, fonts.text, SMALL + 0.5)
    y = y + S(NOTE_H)

    -- what the last one did, or what to expect
    y = y + S(4)
    local said = crash.said ~= nil and now < crash.said[3] and crash.said or nil
    local first = said and said[1] or 'It runs at the helper\'s next frame, so in the world, not a menu.'
    M.text_mid(dl, x, y + S(NOTE_H) / 2, M.fit(first, w, fonts.text, SMALL), said and said[2] or P.dim, fonts.text, SMALL)
    M.text_mid(dl, x, y + S(NOTE_H) * 1.5, M.fit('Switch DLSS off and on again for a new helper.', w, fonts.text, SMALL), P.dim,
               fonts.text, SMALL)
end

-------------------------------------------------------------------------------
-- Debug views ([graphics.effects] debug)
-------------------------------------------------------------------------------

-- Reflection ground's colours (the Debug view's 18), by the zone's floors' kind of ground as fx_refl's KIND_COL has them
-- (gfx_d3d12.c), each kind's number its place less one
local KINDS = {
    { '#B58CE6', 'object' }, { '#C7B58C', 'path' }, { '#6EC759', 'grass' }, { '#E6D16E', 'sand' }, { '#F2F5FF', 'snow' },
    { '#E8945C', 'stone' }, { '#E8638C', 'metal' }, { '#A86B3D', 'wood' }, { '#59C7E6', 'shallow water' }, { '#3D6EDB', 'deep water' },
}
-- ... the kinds the reflections keep to by [graphics.effects] refl_floors (the Graphics page's Ground; 0 Any: no floors read)
local GROUND_KEEPS = { [1] = { stone = true, metal = true }, [2] = { stone = true, metal = true, wood = true },
                       [3] = { stone = true, metal = true, wood = true, object = true } }

-- the lines under the view for the reflections' two and the rain's: what Reflections shows, Reflection ground's legend (two
-- rows of five), Rain's colours
local function views_extra(view)
    return (view == '17' or view == '31') and S(NOTE_H) or view == '18' and S(NOTE_H) * 3 or 0
end

local function views_height()
    return S(ROW_H) * 2 + views_extra(M.read(M.DEBUG_VIEW)) + S(4)
end

-- the rain held still: its drops, splashes, streaks, ripples and lens beads stop where they are; never saved (ini.cpp session_only)
local RAIN_FREEZE = { kind = 'toggle', section = 'graphics', key = 'rain.freeze', default = false }
-- Rain's colours (the Debug view's 31): the rain pass's red over the picture in grey (gfx_hlsl_rain.h), where it rains
local RAIN_COLOURS = { { '#FF0000', 'rain' }, { '#4080FF', 'under a roof' }, { '#9A9A9A', 'the rest' } }

-- whether VanaCore's renderer draws the game (DirectX 11 or 12): vana.graphics.info(), twice a second
local renderer = { active = false, at = -1 }
local function renderer_ready(now)
    if now - renderer.at >= 0.5 then
        renderer.at = now
        local ok, info = pcall(vana.graphics.info)
        renderer.active = ok and info ~= nil and info.active == true
    end
    return renderer.active
end

local function views_draw(dl, x, y, w, now)
    local row = M.DEBUG_VIEW
    local ry = y + S(ROW_H) / 2
    -- the view, and a part of the sun's shadows turned off (M.SUN_GATE) beside it: each half the width
    local half = math.floor((w - S(16)) / 2)
    local label_w = M.measure('View', fonts.text, BODY - 0.5) + S(10)
    M.text_mid(dl, x, ry, 'View', P.text, fonts.text, BODY - 0.5)
    if M.over(x, ry - S(12), half, S(24)) then
        M.tooltip(M.TIPS.rows['graphics.effects.debug'])
    end
    local current = M.read(row)
    local view, moved = M.dropdown('##debug.view', x + label_w, ry, half - label_w, row.options, current, M.effects_available(), nil,
                                   true, true)
    if moved then
        M.write(row, view, true)
    end
    local gx = x + half + S(16)
    local gate_label = 'Shadow Layers'
    local gate_w = M.measure(gate_label, fonts.text, BODY - 0.5) + S(10)
    M.text_mid(dl, gx, ry, gate_label, P.text, fonts.text, BODY - 0.5)
    if M.over(gx, ry - S(12), half, S(24)) then
        M.tooltip(M.TIPS.rows['graphics.effects.sun_gate'])
    end
    local gate, gate_moved = M.dropdown('##debug.sungate', gx + gate_w, ry, x + w - gx - gate_w, M.SUN_GATE.options,
                                        M.read(M.SUN_GATE), M.effects_available(), nil, true, true)
    if gate_moved then
        M.write(M.SUN_GATE, gate, true)
    end

    -- the rain held still, under them
    y = y + S(ROW_H)
    ry = y + S(ROW_H) / 2
    local ready, frozen = renderer_ready(now), M.read(RAIN_FREEZE)
    local clicked, bw = M.button('##debug.rain.freeze', x, ry, frozen and 'Let it fall' or 'Freeze rain', 'camera',
                                 frozen and 'accent' or 'ghost', ready)
    if imgui.IsItemHovered() then
        M.tooltip(ready and 'VanaCore\'s rain held still, to look at it' or 'Needs DirectX 11 or 12')
    end
    if clicked and ready then
        M.write(RAIN_FREEZE, not frozen, true)
    end
    local note = not ready and 'Needs DirectX 11 or 12 (Graphics > Renderer)'
        or frozen and 'Frozen: drops, splashes, streaks and beads stay where they are'
        or 'While it rains, or with Preview rain'
    M.text_mid(dl, x + bw + S(12), ry, M.fit(note, w - bw - S(12), fonts.text, SMALL + 0.5), ready and P.text2 or P.dim, fonts.text,
               SMALL + 0.5)

    y = y + S(ROW_H)
    if current == '31' then
        local lx, cy = x, y + S(NOTE_H) / 2
        for _, c in ipairs(RAIN_COLOURS) do
            M.circle(dl, lx + S(4), cy, S(4), c[1])
            M.text_mid(dl, lx + S(12), cy, c[2], P.text2, fonts.text, SMALL)
            lx = lx + S(12) + M.measure(c[2], fonts.text, SMALL) + S(16)
        end
    elseif current == '17' then
        M.text_mid(dl, x, y + S(NOTE_H) / 2, M.fit('Colour: the reflections at twice their strength. Grey: none.', w, fonts.text, SMALL),
                   P.dim, fonts.text, SMALL)
    elseif current == '18' then
        local ground = math.floor((tonumber(vana.config.get(M.FX, 'refl_floors') or '') or 1) + 0.5)
        local keeps = GROUND_KEEPS[ground] or {}
        for i, k in ipairs(KINDS) do
            local lx = x + ((i - 1) % 5) * math.floor(w / 5)
            local cy = y + S(NOTE_H) * (math.floor((i - 1) / 5) + 0.5)
            M.circle(dl, lx + S(4), cy, S(4), k[1], keeps[k[2]] and 1 or 0.35)
            M.text_mid(dl, lx + S(12), cy, k[2], keeps[k[2]] and P.text2 or P.dim, fonts.text, SMALL)
        end
        local note = GROUND_KEEPS[ground] and 'Solid: reflects. Striped: left out by Ground. Grey: no floor there.'
                     or 'Ground is Any: the floors aren\'t read, so no colours.'
        M.text_mid(dl, x, y + S(NOTE_H) * 2.5, M.fit(note, w, fonts.text, SMALL), P.dim, fonts.text, SMALL)
    end
end

-------------------------------------------------------------------------------
-- Shadow and AO rings (sun_rings, ao_rings)
-------------------------------------------------------------------------------

local FX = M.FX
local AO_TAPS = { 6, 10, 16, 24, 32 } -- the occlusion's taps by ao_quality (gfx_d3d12.c AO_TAPS)
local AO_KEEP = { 1, 0.75, 0.5, 0.3 } -- ... the share kept past each ring (fx_ao)
local AO_QUALITY = { 'Low', 'Medium', 'High', 'Ultra', 'Max' }
-- the view's colours, as ring_tint draws them (gfx_d3d12.c)
local RING_COLOURS = { { P.green, 'every tap' }, { '#E8D45C', 'past ring 1' }, { '#E8945C', 'past ring 2' }, { P.rose, 'past ring 3' } }

-- each one's settings as the Graphics page's rows have them (data.lua), read and written the same way, and its view (the
-- Debug view's number)
local RINGS = {
    { name = 'Shadow rings', what = 'Shadows', view = '15',
      on = { kind = 'toggle', section = FX, key = 'sun_rings', default = true },
      spacing = { kind = 'slider', section = FX, key = 'sun_ring_spacing', default = 15, min = 5, max = 60, step = 1 } },
    { name = 'AO rings', what = 'AO', view = '16',
      on = { kind = 'toggle', section = FX, key = 'ao_rings', default = true },
      spacing = { kind = 'slider', section = FX, key = 'ao_ring_spacing', default = 15, min = 5, max = 60, step = 1 } },
}

-- the taps a pixel takes in each ring, "16 / 12 / 8 / 6 taps" ("... taps (Max)" for the occlusion's, by its quality)
local function taps_text(i)
    if i == 1 then
        return '16 / 12 / 8 / 6 taps'
    end
    local q = math.max(0, math.min(4, math.floor((tonumber(vana.config.get(FX, 'ao_quality') or '') or 0) + 0.5)))
    local n, taps = AO_TAPS[q + 1], {}
    for k, keep in ipairs(AO_KEEP) do
        taps[k] = math.max(math.floor(n * keep + 0.5), math.min(n, 4))
    end
    return table.concat(taps, ' / ') .. ' taps (' .. AO_QUALITY[q + 1] .. ')'
end

-- the rain's (the Debug view's 32, gfx_hlsl_rain.h dbg 6): its 100-yalm bands, and its lines
local RAIN_RING_COLOURS = { { P.green, '0-100' }, { '#E8D45C', '-200' }, { '#E8945C', '-300' }, { P.rose, '-400' }, { '#B373E6', '-500' },
                            { '#33E6FF', 'splash range' }, { '#FFFFFF', 'rain range' } }

-- Mass Grass's rings (grass.c draw_rings): circles on the ground round the player where fewer tufts are kept, in its colours;
-- never saved (ini.cpp session_only). The Debug window's Grass tab's till 2026-11-22 ("it has its own section")
local GRASS_RINGS = { kind = 'toggle', section = 'graphics', key = 'grass.rings.show', default = false }
local GRASS_RING_COLOURS = { { '#40FF40', 'ring 1' }, { '#FFE040', 'ring 2' }, { '#FF8020', 'ring 3' }, { '#40A0FF', 'tufts shrink' },
                             { '#FF3030', 'range' } }

local function rings_height()
    return S(NOTE_H) * 2 + S(ROW_H) * 3 + S(NOTE_H) * 2 + S(ROW_H) + S(NOTE_H) + S(4)
end

local function rings_draw(dl, x, y, w, now)
    for i, r in ipairs(RINGS) do
        local on, spacing = M.read(r.on), math.floor(M.read(r.spacing) + 0.5)
        local text = on and string.format('%s: rings at %d, %d, %d; %s', r.what, spacing, spacing * 2, spacing * 3, taps_text(i))
                     or r.what .. ': rings off, every tap everywhere'
        local cy = y + S(NOTE_H) / 2
        M.circle(dl, x + S(4), cy, S(3), on and P.green or P.faint)
        M.text_mid(dl, x + S(14), cy, M.fit(text, w - S(14), fonts.text, SMALL + 0.5), on and P.text2 or P.dim, fonts.text, SMALL + 0.5)
        y = y + S(NOTE_H)
    end

    -- the views, one at a time: Debug views' view
    local view = M.read(M.DEBUG_VIEW)
    local bx, cy = x, y + S(ROW_H) / 2
    for i, r in ipairs(RINGS) do
        local showing = view == r.view
        local clicked, bw = M.button('##debug.rings.view.' .. i, bx, cy, (showing and 'Hide ' or 'Show ') .. r.name:lower(), 'eye',
                                     showing and 'accent' or 'ghost')
        if imgui.IsItemHovered() then
            M.tooltip('Tints the picture by ring, as the colours below (Debug views). DirectX 11 or 12, effects on')
        end
        if clicked then
            M.write(M.DEBUG_VIEW, showing and '0' or r.view, true)
        end
        bx = bx + bw + S(8)
    end
    local rain_showing = view == '32'
    local rclicked = M.button('##debug.rings.view.rain', bx, cy, rain_showing and 'Hide rain rings' or 'Show rain rings', 'eye',
                              rain_showing and 'accent' or 'ghost')
    if imgui.IsItemHovered() then
        M.tooltip('Tints the ground by the rain\'s 100-yalm bands, a fifth fewer drops each, as the second row of colours below. '
                  .. 'While it rains, or with Preview rain')
    end
    if rclicked then
        M.write(M.DEBUG_VIEW, rain_showing and '0' or '32', true)
    end
    y = y + S(ROW_H)

    -- each one's switch and spacing
    for i, r in ipairs(RINGS) do
        local ry = y + S(ROW_H) / 2
        M.text_mid(dl, x, ry, r.name, P.text, fonts.text, BODY - 0.5)
        local sx = x + S(110)
        local on, flipped = M.switch('##debug.rings.on.' .. i, sx, ry, M.read(r.on), true)
        if flipped then
            M.write(r.on, on, true)
        end
        local lx = sx + S(M.SW_W) + S(16)
        M.text_mid(dl, lx, ry, 'Spacing', P.dim, fonts.text, SMALL + 0.5)
        local px = lx + S(54)
        local value = M.read(r.spacing)
        local v, moved, released = M.slider('##debug.rings.spacing.' .. i, px, ry, value, r.spacing.min, r.spacing.max,
                                            r.spacing.step, on, { text = string.format('%d', math.floor(value + 0.5)),
                                                                  width = math.min(S(M.SLIDER_W), x + w - px) })
        if moved then
            M.write(r.spacing, v) -- once a frame while dragged (draw_debug's flush)
        end
        if released then
            M.write(r.spacing, v, true)
        end
        y = y + S(ROW_H)
    end

    -- the views' colours: the shadows' and occlusion's, then the rain's
    for row, colours in ipairs({ RING_COLOURS, RAIN_RING_COLOURS }) do
        local lx, cy2 = x, y + S(NOTE_H) * (row - 0.5)
        for _, c in ipairs(colours) do
            M.circle(dl, lx + S(4), cy2, S(4), c[1])
            M.text_mid(dl, lx + S(12), cy2, c[2], P.dim, fonts.text, SMALL)
            lx = lx + S(12) + M.measure(c[2], fonts.text, SMALL) + S(row == 2 and 10 or 16)
        end
    end
    y = y + S(NOTE_H) * 2

    -- Mass Grass's rings: their button, where they fall (Draw distance detail, a share of its Range), and their colours under it
    local showing, ready = M.read(GRASS_RINGS), renderer_ready(now)
    local gy = y + S(ROW_H) / 2
    local clicked, bw = M.button('##debug.rings.grass', x, gy, showing and 'Hide grass rings' or 'Show grass rings', 'eye',
                                 showing and 'accent' or 'ghost', ready)
    if imgui.IsItemHovered() then
        M.tooltip(ready and 'Circles on the ground round you where fewer grass tufts are kept, ring by ring' or 'Needs DirectX 11 or 12')
    end
    if clicked and ready then
        M.write(GRASS_RINGS, not showing, true)
    end
    local spacing = math.floor((tonumber(vana.config.get('graphics', 'grass.ring.spacing') or '') or 25) + 0.5)
    local note = not ready and 'Needs DirectX 11 or 12 (Graphics > Renderer)'
        or tonumber(vana.config.get('graphics', 'grass') or '') ~= 1 and 'Mass Grass is off (Game Options)'
        or string.format('Grass: rings at %d%%, %d%% and %d%% of its range (Mass Grass > Advanced > Draw distance detail)', spacing,
                         math.min(spacing * 2, 100), math.min(spacing * 3, 100))
    M.text_mid(dl, x + bw + S(12), gy, M.fit(note, w - bw - S(12), fonts.text, SMALL + 0.5), ready and P.text2 or P.dim, fonts.text,
               SMALL + 0.5)
    y = y + S(ROW_H)
    local lx, cy3 = x, y + S(NOTE_H) / 2
    for _, c in ipairs(GRASS_RING_COLOURS) do
        M.circle(dl, lx + S(4), cy3, S(4), c[1], showing and 1 or 0.35)
        M.text_mid(dl, lx + S(12), cy3, c[2], showing and P.text2 or P.dim, fonts.text, SMALL)
        lx = lx + S(12) + M.measure(c[2], fonts.text, SMALL) + S(16)
    end
end

-------------------------------------------------------------------------------
-- Collision ([graphics] collision.show)
-------------------------------------------------------------------------------

-- the zone's collision, read from its files when shown and as the player zones (grass_floors::update_collision), drawn over the
-- world by floors.c; never saved (ini.cpp session_only)
local COLLISION = { kind = 'toggle', section = 'graphics', key = 'collision.show', default = false }
local WALL_COLOUR = '#E8302C' -- floors.c COLL_WALL_RGB; the floors' are KINDS

local collision_ready = renderer_ready -- (the renderer draws the view)

local function collision_height()
    return S(ROW_H) + S(NOTE_H) * 3 + S(4)
end

local function collision_draw(dl, x, y, w, now)
    local ready, showing = collision_ready(now), M.read(COLLISION)
    local ry = y + S(ROW_H) / 2
    local clicked, bw = M.button('##debug.collision', x, ry, showing and 'Hide collision' or 'Show collision', 'eye',
                                 showing and 'accent' or 'ghost', ready)
    if imgui.IsItemHovered() then
        M.tooltip(ready and 'The zone\'s collision over the world, see-through: what stops you and what you stand on'
                  or 'Needs DirectX 11 or 12')
    end
    if clicked and ready then
        M.write(COLLISION, not showing, true)
    end
    local note = ready and 'Over the world, behind what\'s nearer' or 'Needs DirectX 11 or 12 (Graphics > Renderer)'
    M.text_mid(dl, x + bw + S(12), ry, M.fit(note, w - bw - S(12), fonts.text, SMALL + 0.5), ready and P.text2 or P.dim, fonts.text,
               SMALL + 0.5)
    y = y + S(ROW_H)

    -- the colours: the walls', then the floors' by their ground (two rows of five, as Reflection ground's)
    local col_w = math.floor(w / 5)
    local cy = y + S(NOTE_H) / 2
    M.circle(dl, x + S(4), cy, S(4), WALL_COLOUR)
    M.text_mid(dl, x + S(12), cy, 'wall', P.text2, fonts.text, SMALL)
    M.text_mid(dl, x + col_w, cy, M.fit('What stops you. Floors by the ground your steps sound for:', w - col_w, fonts.text, SMALL),
               P.dim, fonts.text, SMALL)
    y = y + S(NOTE_H)
    for i, k in ipairs(KINDS) do
        local lx = x + ((i - 1) % 5) * col_w
        local ky = y + S(NOTE_H) * (math.floor((i - 1) / 5) + 0.5)
        M.circle(dl, lx + S(4), ky, S(4), k[1])
        M.text_mid(dl, lx + S(12), ky, k[2], P.text2, fonts.text, SMALL)
    end
end

-------------------------------------------------------------------------------
-- Frame recording (/vana debug <seconds>)
-------------------------------------------------------------------------------

local function record_height()
    return S(ROW_H) + S(NOTE_H) + S(4)
end

local function record_draw(dl, x, y, w, now)
    local ry = y + S(ROW_H) / 2
    local clicked, bw = M.button('##debug.record', x, ry, 'Record 30 s', nil, 'ghost')
    if clicked then
        vana.chat.run('/vana debug 30')
    end
    local stop, sw = M.button('##debug.record.stop', x + bw + S(8), ry, 'Stop', nil, 'ghost')
    if stop then
        vana.chat.run('/vana debug stop')
    end
    M.text_mid(dl, x + bw + sw + S(20), ry, M.fit('Each frame\'s time and where it went', w - bw - sw - S(20), fonts.text, SMALL + 0.5),
               P.text2, fonts.text, SMALL + 0.5)
    M.text_mid(dl, x, y + S(ROW_H) + S(NOTE_H) / 2,
               M.fit('Into logs\\frames-*.csv and logs\\debug-*.log. /vana debug <seconds> records 5 to 120 s.', w, fonts.text, SMALL),
               P.dim, fonts.text, SMALL)
end

-------------------------------------------------------------------------------
-- The window
-------------------------------------------------------------------------------

-------------------------------------------------------------------------------
-- Mass Grass ([graphics] grass.rings.show, grass.cover.show)
-------------------------------------------------------------------------------

-- the grass's views, drawn by grass.c over its tufts; never saved (ini.cpp session_only). On the Mass Grass panel till 2026-11-22;
-- its rings are on the Rings tab with the others' (GRASS_RINGS)
local GRASS_VIEWS = {
    { key = 'grass.cover.show', show = 'Show covered grass', hide = 'Hide covered grass',
      tip = 'The tufts a floor over them hides (a building\'s, over the ground), drawn; left out otherwise',
      colours = { { P.rose, 'under a floor (left out)' }, { '#E8D45C', 'no floor under it to tell' } } },
}
for _, v in ipairs(GRASS_VIEWS) do
    v.row = { kind = 'toggle', section = 'graphics', key = v.key, default = false }
end

local function grass_height()
    return (S(ROW_H) + S(NOTE_H)) * #GRASS_VIEWS + S(4)
end

local function grass_draw(dl, x, y, w, now)
    local ready = renderer_ready(now)
    local on = tonumber(vana.config.get('graphics', 'grass') or '') == 1
    for i, v in ipairs(GRASS_VIEWS) do
        local showing = M.read(v.row)
        local ry = y + S(ROW_H) / 2
        local clicked, bw = M.button('##debug.grass.' .. i, x, ry, showing and v.hide or v.show, 'eye', showing and 'accent' or 'ghost',
                                     ready)
        if imgui.IsItemHovered() then
            M.tooltip(ready and v.tip or 'Needs DirectX 11 or 12')
        end
        if clicked and ready then
            M.write(v.row, not showing, true)
        end
        local note = not ready and 'Needs DirectX 11 or 12 (Graphics > Renderer)' or not on and 'Mass Grass is off (Game Options)' or nil
        if note ~= nil then
            M.text_mid(dl, x + bw + S(12), ry, M.fit(note, w - bw - S(12), fonts.text, SMALL + 0.5), P.dim, fonts.text, SMALL + 0.5)
        end
        y = y + S(ROW_H)
        local lx, cy = x, y + S(NOTE_H) / 2
        for _, c in ipairs(v.colours) do
            M.circle(dl, lx + S(4), cy, S(4), c[1], showing and 1 or 0.35)
            M.text_mid(dl, lx + S(12), cy, c[2], showing and P.text2 or P.dim, fonts.text, SMALL)
            lx = lx + S(12) + M.measure(c[2], fonts.text, SMALL) + S(16)
        end
        y = y + S(NOTE_H)
    end
end

local TABS = {
    { id = 'views', tab = 'Views', title = 'Debug views', height = views_height, draw = views_draw },
    { id = 'collision', tab = 'Collision', title = 'Collision', height = collision_height, draw = collision_draw },
    { id = 'grass', tab = 'Grass', title = 'Mass Grass', height = grass_height, draw = grass_draw },
    { id = 'crash', tab = 'DLSS crash', title = 'DLSS crash tests', height = crash_height, draw = crash_draw },
    { id = 'rings', tab = 'Rings', title = 'Shadow, AO, rain & grass rings', height = rings_height, draw = rings_draw },
    { id = 'record', tab = 'Recording', title = 'Frame recording', height = record_height, draw = record_draw },
}
local TAB_LABELS = {}
for i, t in ipairs(TABS) do
    TAB_LABELS[i] = t.tab
end

function M.debug_toggle()
    dbg.open = not dbg.open
end

function M.draw_debug(now)
    if not fonts.ready then
        fonts.ready = true
        pcall(M.load_fonts)
        return
    end
    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
    -- with the /vana window shut its frame doesn't run: the scale and the animations' clock kept here
    if not st.open then
        st.dt = math.max(0, math.min(0.1, now - st.last))
        st.last = now
        M.compute_scale(now, screen_h, M.user_scale() * 100)
    end

    -- as tall as the tab shown
    local tab, index = TABS[1], 1
    for i, t in ipairs(TABS) do
        if t.id == dbg.tab then
            tab, index = t, i
        end
    end
    local pw = S(WIN_W)
    local ph = S(TITLE_H) + S(TABS_H) + tab.height() + S(PAD)

    local dw, _, k = M.display(screen_w, screen_h) -- ImGui's pixels, where it's drawn k times its size
    imgui.SetNextWindowPos({ dw - (pw + S(40)) * k, S(120) * k }, M.FIRST_USE_EVER)
    imgui.SetNextWindowSize({ pw, ph }, 0)
    local VAR = M.VAR
    imgui.PushStyleVar(VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(VAR.WindowBorderSize, 0)
    imgui.PushStyleVar(VAR.ItemSpacing, { 0, 0 })
    if imgui.Begin('Debug###vana_debug', nil, M.WINDOW_FLAGS) then
        local x, y = imgui.GetWindowPos()
        local dl = imgui.GetWindowDrawList()
        local radius = S(6)
        M.fill(dl, x - 1, y, pw + 2, ph + 2, '#000000', radius + 1, 0, 0.35)
        M.fill(dl, x, y, pw, ph, P.bg, radius)
        M.outline(dl, x, y, pw, ph, P.border, radius)

        local lx, w = x + S(PAD), pw - S(PAD) * 2
        local cy = y + S(TITLE_H) / 2
        M.icon_centred(dl, lx + S(5), cy, 'bug', P.accent, 11)
        M.text_mid(dl, lx + S(18), cy, 'Debug', P.white, fonts.semi, BODY + 2)
        M.text_right_mid(dl, x + pw - S(PAD) - S(22), cy + S(1), '/vana debug', P.faint, fonts.text, SMALL)
        if M.icon_button('##debug.close', x + pw - S(PAD), cy, 'close', 11, P.rose) then
            dbg.open = false
        end
        M.hline(dl, x, x + pw, y + S(TITLE_H), P.line)

        -- the tabs, each one's full name its tooltip; the one picked shows from the next frame, the window sized for it
        local picked, moved, over = M.segments('##debug.tabs', lx, y + S(TITLE_H) + S(TABS_H) / 2, TAB_LABELS, index, true)
        if over ~= nil and TABS[over].title ~= TABS[over].tab then
            M.tooltip(TABS[over].title)
        end
        if moved then
            dbg.tab = TABS[picked].id
        end
        tab.draw(dl, lx, y + S(TITLE_H) + S(TABS_H), w, now)
    end
    imgui.End()
    imgui.PopStyleVar(3)
    -- a slider's writes while it's dragged: the /vana window's frame makes them when it's open (vana.lua's present)
    if not st.open and next(M.pending) ~= nil then
        M.flush()
    end
end
