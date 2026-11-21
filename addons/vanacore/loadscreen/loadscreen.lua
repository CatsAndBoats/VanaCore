--[[
loadscreen: while the game changes zone (and at the first zone after picking a character), where you're going, what
the game is doing and a bar for how far along it is, in the middle of the screen. Text and a bar, over the game's own
picture or over one of your screenshots.

From vana.game.loading() (zoneload.cpp), which follows each load:
  server  waiting for the server to say which zone (no longer than 12 s on screen: a logout looks like this too). Its
          steps: leaving the zone, connecting to the zone server, waiting for its answer; the bar moves by the time the
          last waits took, and the server's answer time (a round trip) is shown once the zone loads
  zone    the game reads the zone's data files: the bar counts them toward the total the zone needed last time
          (cache\zoneload.txt), or eases toward full the first time
  people  the zone is in: the text fades, and (People loading, off by default) a line at the top centre counts the
          people's models still loading

A logout or /shutdown (the server's zone-out of type 1, out_type) shows none of it: the game fades to black, no
screenshot, no text. The black lifts as the title screen's windows open (as autologin.cpp names them), or after
LOGOUT_LONGEST should they not be seen; a /shutdown's game is gone before then.

What shows, its scale and position, its colours, its panel and a picture behind it are /vana > Options > Loading Screen
([loadscreen] in the settings file, read each frame it shows). The picture: one of your screenshots (VanaCore's
screenshots folder, every day's), a new one picked at random for each load, darkened toward its edges (edges.png).

In game:
  /loadscreen            /vana at its Loading Screen settings
  /loadscreen preview    shows it for a few seconds as a load would
  /loadscreen preview logout   the logout's fade to black, lifting after a moment as the title screen would
  /addon unload loadscreen   switches it off (its scripts\boot.txt line taken out, as for any addon)
--]]

addon.name    = 'loadscreen'
addon.author  = 'Vekien'
addon.version = '0.2'
addon.desc    = 'A loading screen while the game changes zone: where to, what it is doing and how far along.'

local LAYER   = 90      -- over the HUD and the zone name
local FADE_IN = 0.15    -- s
local FADE_OUT = 0.45   -- s
local BAR_W, BAR_H = 420, 6

local ui = {}           -- the boxes, made at load
local opts = {}         -- the settings, read each frame it shows (read_opts)
local shots = {}        -- the screenshots' full paths, listed at most once a minute
local shots_at = -100
local picked = nil      -- the screenshot this load shows
local picture_file = nil -- the one ui.picture has
local shown = 0         -- the screen's opacity, 0 to 1
local pic = 0           -- the screenshot's opacity, 0 to 1 (its own fade in and out)
local bar = 0           -- the bar's drawn fill, 0 to 1 (eased toward the target)
local last_clock = nil
local corner_alpha = 0
local preview_until = nil
local SERVER_LONGEST = 12 -- s of "waiting for the server" shown at most
local logged_out = false  -- the logout event came: nothing shown until the next load starts

-- A logout's black (its zone-out out_type 1): fast in, as a /shutdown's game ends about 0.6 s after its zone-out
local LOGOUT_IN, LOGOUT_OUT = 0.4, 0.6 -- s
local LOGOUT_LONGEST = 10                -- s of black at most, should the title screen's windows not be seen
local TITLE_MENUS = { ptc8lice = true, loby2win = true, dbnamese = true, ptc6yesn = true } -- the title screen's windows
local black = 0                          -- the black's opacity, 0 to 1
local at_title = false                   -- a title screen window opened since the logout: the black lifts
local preview_logout = false             -- /loadscreen preview logout: the preview is the logout's black

local function now() return vana.time.clock() end

-- A colour setting as '#RRGGBBAA': a '#RRGGBB' given to a vana.ui rect is drawn at 55% (its default alpha), so the
-- panel at Panel opacity 100% stayed see-through; `default` when it's unset or not a colour
local function solid(colour, default)
    colour = (colour ~= nil and colour:match('^#%x+$')) and colour or default
    return #colour == 7 and colour .. 'FF' or colour
end

-- [loadscreen]: what /vana > Options > Loading Screen sets (its defaults are these)
local function read_opts()
    local function on(key, default)
        local v = vana.config.get('loadscreen', key)
        if v == nil or v == '' then return default end
        return v ~= '0'
    end
    local function num(key, default, lo, hi)
        local v = tonumber(vana.config.get('loadscreen', key) or '')
        return v and math.max(lo, math.min(hi, v)) or default
    end
    opts.from, opts.bar, opts.status = on('from', true), on('bar', true), on('status', true)
    opts.file, opts.people, opts.image = on('file', true), on('people', false), on('image', false)
    opts.oneline = on('oneline', false)
    opts.scale = num('scale', 100, 50, 200) / 100
    opts.x, opts.y = num('x', 50, 0, 100) / 100, num('y', 50, 0, 100) / 100
    opts.edges = num('image.edges', 80, 0, 100) / 100
    opts.panel = num('panel.opacity', 70, 0, 100) / 100
    opts.panel_color = solid(vana.config.get('loadscreen', 'panel.color'), '#0A0D14')
    opts.pic_in = num('image.fadein', 0.4, 0, 3)   -- s: the screenshot's own fades, apart from the text's
    opts.pic_out = num('image.fadeout', 0.8, 0, 3)
    opts.bar_color = solid(vana.config.get('loadscreen', 'bar.color'), '#7AA2F7')
end

-- a screenshot at random (nil when there are none), for a load starting
local function pick_screenshot()
    if now() - shots_at > 60 then
        shots_at = now()
        shots = {}
        local folder = vana.paths.install .. '\\screenshots'
        for _, name in ipairs(vana.fs.list(folder, '.*\\.(png|jpe?g|bmp)', { which = 'files', deep = true }) or {}) do
            shots[#shots + 1] = folder .. '\\' .. name
        end
    end
    if #shots == 0 then
        return nil
    end
    return shots[math.random(#shots)]
end

-- the zone's name, or nil
local function name_of(zone)
    if zone == nil or zone == 0 or vana.gamedata == nil then
        return nil
    end
    local ok, row = pcall(function () return vana.gamedata.zones[zone] end)
    local name = ok and type(row) == 'table' and row.name or nil
    if type(name) ~= 'string' or name == '' or name == 'unknown' then
        return nil
    end
    return name
end

local function make()
    ui.picture = vana.ui.image({ file = addon.path .. '\\edges.png', layer = LAYER - 2, visible = false })
    ui.edges = vana.ui.image({ file = addon.path .. '\\edges.png', layer = LAYER - 1, visible = false })
    ui.title = vana.ui.text({ text = ' ', size = 34, bold = true, color = '#F2EBDD', background = '#00000000', stroke = 2, stroke_color = '#000000C0',
        padding = 0, layer = LAYER + 1, visible = false })
    ui.from = vana.ui.text({ text = ' ', size = 15, color = '#B8C0CE', background = '#00000000', padding = 0, stroke = 2,
        stroke_color = '#000000C0', layer = LAYER + 1, visible = false })
    ui.track = vana.ui.rect({ width = BAR_W, height = BAR_H, radius = BAR_H / 2, color = '#FFFFFF26',
        layer = LAYER + 1, visible = false })
    ui.fill = vana.ui.rect({ width = 0, height = BAR_H, radius = BAR_H / 2, color = '#7AA2F7', layer = LAYER + 2,
        visible = false })
    ui.status = vana.ui.text({ text = ' ', size = 15, color = '#C9CFDA', background = '#00000000', padding = 0, stroke = 2,
        stroke_color = '#000000C0', layer = LAYER + 1, visible = false })
    ui.file = vana.ui.text({ text = ' ', size = 12, font = 'Consolas', color = '#9AA3B2', background = '#00000000', stroke = 2, stroke_color = '#000000C0',
        padding = 0, layer = LAYER + 1, visible = false })
    ui.corner = vana.ui.text({ text = ' ', size = 13, color = '#C9CFDA', background = '#0A0D14B0', padding = 8,
        layer = LAYER, visible = false })
    -- behind the text and bar while a screenshot is behind it all, so they read over a bright picture
    -- (Panel background colour and Panel opacity)
    ui.panel = vana.ui.rect({ width = 0, height = 0, radius = 10, color = '#0A0D14', layer = LAYER, visible = false })
    -- a logout's black, over it all
    ui.black = vana.ui.rect({ width = 0, height = 0, color = '#000000FF', layer = LAYER + 3, visible = false })
end

local function set_text(box, text)
    if box.text ~= text then
        box.text = text
    end
end

-- a text box centred on x
local function centre(box, cx, y)
    box:set({ x = math.floor(cx - box.width / 2), y = math.floor(y) })
end

-- a text box's size by the Size setting, set only when it differs
local function sized(box, size)
    size = math.max(6, math.floor(size * opts.scale + 0.5))
    if box.size ~= size then
        box.size = size
    end
end

-- where the bar should be: the files read against the zone's last total, else easing toward full
local function target_of(state)
    if state.phase == 'server' then
        local wait = state.wait or 0
        return wait > 0 and 0.02 + 0.08 * math.min(1, state.elapsed / wait) or 0.08
    end
    if state.ready then
        return 1
    end
    if state.zone_expected > 0 then
        return math.min(0.97, 0.1 + 0.87 * state.zone_files / state.zone_expected)
    end
    return 0.1 + 0.85 * (1 - math.exp(-state.zone_files / 12))
end

-- the wait for the server, by its step (state.step: nil at a login, or with a DLL from before steps)
local function server_status(state, from)
    local wait = state.wait or 0
    if wait > 0 and state.elapsed > wait * 2 + 1 then
        return string.format('The server is slow to answer…  %.1f s', state.elapsed)
    elseif state.step == 'leaving' then
        return from and ('Leaving ' .. from .. '…') or 'Leaving the zone…'
    elseif state.step == 'connecting' then
        return 'Connecting to the zone server…'
    end
    return 'Waiting for the server…'
end

local function status_of(state, from)
    if state.phase == 'server' then
        return server_status(state, from)
    end
    if state.ready then
        return 'Ready'
    end
    local answer = state.answer and string.format('  ·  server %d ms', math.floor(state.answer + 0.5)) or ''
    if state.zone_expected > 0 then
        return string.format('Loading the zone  ·  %d / %d', math.min(state.zone_files, state.zone_expected),
            state.zone_expected) .. answer
    end
    return string.format('Loading the zone  ·  %d files', state.zone_files) .. answer
end

-- a made-up load for /loadscreen preview: 0.9 s of server (as measured), 1.6 s of zone, then the people
local function preview_state()
    local t = 3.7 - (preview_until - now())
    if t < 0.9 then
        local step = t < 0.15 and 'leaving' or t < 0.75 and 'connecting' or 'waiting'
        return { phase = 'server', step = step, elapsed = t, wait = 0.9, zone = 0, previous = 245, zone_files = 0,
            zone_expected = 29, people_files = 0, people_expected = 114, ready = false, file = '' }
    end
    local files = math.min(29, math.floor((t - 0.9) / 1.6 * 29))
    return { phase = t < 2.5 and 'zone' or 'people', elapsed = t, wait = 0.9, answer = 134, zone = 246,
        previous = 245, zone_files = files, zone_expected = 29, people_files = math.floor(math.max(0, t - 1.3) * 30),
        people_expected = 114, ready = t >= 2.5, file = string.format('ROM/1/%d.DAT', 20 + files) }
end

local function draw_corner(state, dt)
    local want = state ~= nil and state.ready and state.people_expected ~= 0 and 1 or 0
    if state ~= nil and state.ready and state.people_expected == 0 and state.people_files > 0 then
        want = 1
    end
    corner_alpha = want > corner_alpha and math.min(1, corner_alpha + dt / 0.2) or math.max(0, corner_alpha - dt / 0.6)
    if corner_alpha <= 0 or not opts.people then
        ui.corner.visible = false
        return
    end
    if state ~= nil then
        local text = state.people_expected > 0
            and string.format('Loading people  %d / %d', math.min(state.people_files, state.people_expected),
                state.people_expected)
            or string.format('Loading people  %d', state.people_files)
        set_text(ui.corner, text)
    end
    local sw = vana.ui.screen()
    centre(ui.corner, sw / 2, 24) -- top centre, clear of the party list
    ui.corner:set({ opacity = corner_alpha, visible = true })
end

-- The screenshot and its edges over the screen, at its own fade (pic)
local function draw_picture(covering, dt)
    local fade = covering and opts.pic_in or opts.pic_out
    local step = fade > 0 and dt / fade or 1
    pic = covering and math.min(1, pic + step) or math.max(0, pic - step)
    local with_picture = opts.image and picked ~= nil and pic > 0
    if not with_picture then
        if ui.picture.visible then ui.picture.visible = false end
        if ui.edges.visible then ui.edges.visible = false end
        return
    end
    if picture_file ~= picked then
        picture_file = picked -- (kept here: a picture is read from disk again each time its file is set)
        ui.picture.file = picked
    end
    local sw, sh = vana.ui.screen()
    ui.picture:set({ x = 0, y = 0, width = sw, height = sh, opacity = pic, visible = true })
    ui.edges:set({ x = 0, y = 0, width = sw, height = sh, opacity = pic * opts.edges, visible = opts.edges > 0 })
end

-- A logout's black over the whole screen, fading in while `want`, out after
local function draw_black(want, dt)
    black = want and math.min(1, black + dt / LOGOUT_IN) or math.max(0, black - dt / LOGOUT_OUT)
    if black <= 0 then
        if ui.black.visible then ui.black.visible = false end
        return
    end
    local sw, sh = vana.ui.screen()
    ui.black:set({ x = 0, y = 0, width = sw, height = sh, opacity = black, visible = true })
end

-- The parts switched on, stacked round the Position setting, a panel behind them (Panel opacity, 0 for none)
local function place(sw, sh)
    local s = opts.scale
    sized(ui.title, 34); sized(ui.from, 15); sized(ui.status, 15); sized(ui.file, 12)
    local bar_w, bar_h = math.floor(BAR_W * s), math.max(2, math.floor(BAR_H * s))
    -- each part shown: its box (or the bar), its height, the gap after it
    local parts = { { ui.title, ui.title.height, 4 * s } }
    if opts.from and not opts.oneline then parts[#parts + 1] = { ui.from, ui.from.height, 14 * s } end
    if opts.oneline then parts[1][3] = 14 * s end
    if opts.bar then parts[#parts + 1] = { 'bar', bar_h, 14 * s } end
    if opts.status then parts[#parts + 1] = { ui.status, ui.status.height, 8 * s } end
    if opts.file then parts[#parts + 1] = { ui.file, ui.file.height, 0 } end
    local total = 0
    for i, part in ipairs(parts) do
        total = total + part[2] + (i < #parts and part[3] or 0)
    end
    local cx, y = sw * opts.x, sh * opts.y - total / 2
    if opts.panel > 0 then
        local widest = 0
        for _, part in ipairs(parts) do
            widest = math.max(widest, part[1] == 'bar' and bar_w or part[1].width)
        end
        local pad_x, pad_y = math.floor(28 * s), math.floor(18 * s)
        local w, h = math.floor(widest) + pad_x * 2, math.floor(total) + pad_y * 2
        ui.panel:set({ x = math.floor(cx - w / 2), y = math.floor(y) - pad_y, width = w, height = h,
            radius = math.floor(10 * s), color = opts.panel_color, opacity = shown * opts.panel, visible = true })
    elseif ui.panel.visible then
        ui.panel.visible = false
    end
    for _, box in ipairs({ ui.from, ui.status, ui.file, ui.track, ui.fill }) do
        if box.visible then
            box.visible = false
        end
    end
    for _, part in ipairs(parts) do
        if part[1] == 'bar' then
            local x = math.floor(cx - bar_w / 2)
            ui.track:set({ x = x, y = math.floor(y), width = bar_w, height = bar_h, radius = bar_h / 2, opacity = shown,
                visible = true })
            ui.fill:set({ x = x, y = math.floor(y), width = math.max(bar_h, bar_w * bar), height = bar_h,
                radius = bar_h / 2, color = opts.bar_color, opacity = shown, visible = true })
        else
            centre(part[1], cx, y)
            part[1]:set({ opacity = shown, visible = true })
        end
        y = y + part[2] + part[3]
    end
end

local function frame()
    if ui.title == nil then
        return
    end
    local clock = now()
    local dt = last_clock and math.min(0.1, clock - last_clock) or 0
    last_clock = clock

    local state = nil
    local logout = false -- a logout's black wanted
    if preview_until ~= nil then
        if clock < preview_until then
            if preview_logout then
                logout = true
            else
                state = preview_state()
            end
        else
            preview_until, preview_logout = nil, false
        end
    elseif vana.game.loading ~= nil then
        state = vana.game.loading()
        if state == nil or state.out_type ~= 1 then
            at_title = false -- a title screen window counts only while a logout's load is the one going
        end
        if state == nil then
            logged_out = false -- the next load shows again
        elseif state.out_type == 1 then
            -- a logout or /shutdown: black, no screenshot or text, until the title screen
            logout = not at_title and state.elapsed < LOGOUT_LONGEST
            state = nil
        elseif logged_out or (state.phase == 'server' and state.elapsed > SERVER_LONGEST) then
            state = nil
        end
    end
    draw_black(logout, dt)

    if state ~= nil or shown > 0 or corner_alpha > 0 or pic > 0 then
        read_opts()
    end
    local covering = state ~= nil and not state.ready
    if covering and shown <= 0 and pic <= 0 then
        picked = opts.image and pick_screenshot() or nil -- a new picture for each load (not one still fading out)
    end
    shown = covering and math.min(1, shown + dt / FADE_IN) or math.max(0, shown - dt / FADE_OUT)
    draw_corner(state, dt)
    draw_picture(covering, dt)
    if shown <= 0 then
        bar = 0
        for name, box in pairs(ui) do
            if name ~= 'corner' and name ~= 'picture' and name ~= 'edges' and name ~= 'black' and box.visible then
                box.visible = false
            end
        end
        return
    end

    if state ~= nil then
        local target = target_of(state)
        bar = bar + (target - bar) * math.min(1, dt * 8)
        local where = name_of(state.zone)
        local fallback = state.login and 'Logging in…' or (state.phase == 'server' and 'Travelling…' or 'Loading…')
        local from = name_of(state.previous)
        -- Single line zone info: From → To on the title's line (To alone when where from isn't known or is switched off)
        local title = where or fallback
        if opts.oneline and opts.from and from ~= nil then
            title = from .. '  →  ' .. title
        end
        set_text(ui.title, title)
        set_text(ui.from, from and ('from ' .. from) or (state.login and where and 'Logging in' or ' '))
        set_text(ui.status, status_of(state, from))
        set_text(ui.file, state.file ~= '' and state.file or ' ')
    end

    local sw, sh = vana.ui.screen()
    place(sw, sh)
end

vana.events.on('load', function ()
    math.randomseed(os.time())
    read_opts()
    make()
    if vana.game.loading == nil then
        print('loadscreen: this VanaCore.Core.dll has no vana.game.loading; the screen shows only with /loadscreen preview')
    end
end)

vana.events.on('unload', function ()
    for _, box in pairs(ui) do
        box:destroy()
    end
    ui = {}
end)

-- A mistake here mustn't cost a frame's error every frame: the first one hides it all and stops it until a reload
local failed = false
vana.events.on('present', function ()
    if failed then
        return
    end
    local ok, err = pcall(frame)
    if not ok then
        failed = true
        for _, box in pairs(ui) do
            pcall(function () box.visible = false end)
        end
        print('loadscreen: stopped after an error (/addon reload loadscreen starts it again): ' .. tostring(err))
    end
end)

-- After a logout (a zone-out with no zone-in) nothing more of that load shows
vana.events.on('logout', function ()
    logged_out = true
end)
local function shown_again()
    logged_out = false -- a zone-in or a login: whatever load follows is shown
end
vana.events.on('zone_change', shown_again)
vana.events.on('login', shown_again)

-- The title screen's windows: a logout's black lifts (frame)
vana.events.on('menu_open', function (e)
    if TITLE_MENUS[e.name] then
        at_title = true
    end
end)

vana.events.on('command', function (e)
    local words = {}
    for word in e.command:gmatch('%S+') do
        words[#words + 1] = word:lower()
    end
    if words[1] ~= '/loadscreen' and words[1] ~= '//loadscreen' then
        return false
    end
    if words[2] == 'preview' then
        preview_logout = words[3] == 'logout' -- 2 s of black, then it lifts
        preview_until = now() + (preview_logout and 2 or 3.7)
        bar = 0
        shown, pic = 0, 0 -- a new picture, as a load picks one
    elseif words[2] == nil then
        return false -- /vana opens at its Loading Screen settings (vana.lua)
    else
        print('loadscreen: /loadscreen opens its settings; /loadscreen preview shows it as a load would '
            .. '(/loadscreen preview logout: a logout\'s fade to black)')
    end
    return true
end)
