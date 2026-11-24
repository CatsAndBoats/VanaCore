--[[
vana\install.lua: the addon package prompt. Loaded by vana.lua with the shared table M.

A package (.vcaddon: VanaCore's addonpkg.cpp reads and installs it) comes from a double-click on one while the game runs
(it waits in addons\install\ and the game comes to the front), a file dropped in addons\install\, or Install from file on
the Addons page (/vana install). This shows what's in it before anything is written - its name, version, author and
description, where it came from and where it goes, the version it replaces - with the warning that an addon is a program,
and asks: Install and load, Install only, or Not now. Load at every start adds its scripts\boot.txt line.

It draws with the /vana window shut too, over the game with the screen dimmed, one package at a time.
--]]

local M = ...
local imgui = vana.imgui
local S, P, fonts = M.S, M.P, M.fonts
local BODY, SMALL = M.BODY, M.SMALL
local st = M.st

local W = 540          -- the dialog's width
local PAD = 26         -- its margins
local HEAD = 64        -- the title block
local LOGO = 64        -- VanaCore's logo in it
local RADIUS = 10      -- the card's corners
local FACT_H = 22      -- a line of the From / Into / Size table
local LINE = 1.42      -- a wrapped line's height, in its text's size
local MAX_DESC_LINES = 7
local WARNING = 'An addon is a program: it runs inside the game and can do anything you can on this PC. '
    .. 'Install it only if you trust where it came from.'

local api = vana.addons
local install = { prompt = nil, next_look = 0, boot = true }
M.install = install

local function clock()
    return vana.time.clock()
end

local function available()
    return api ~= nil and api.package_info ~= nil
end

local function block_mouse(ms)
    if vana.window ~= nil and vana.window.block_mouse ~= nil then
        vana.window.block_mouse(true, ms or 400)
    end
end

local function size_text(bytes)
    if bytes >= 1024 * 1024 then
        return string.format('%.1f MB', bytes / (1024 * 1024))
    end
    return string.format('%d KB', math.max(1, math.floor((bytes + 1023) / 1024)))
end

-- `text` wrapped to `room` wide: its lines. A word too long for a line (a path) is broken where it must be.
local function wrap(text, room, font, size)
    local lines = {}
    for para in (text .. '\n'):gmatch('(.-)\n') do
        local line = ''
        for word in para:gmatch('%S+') do
            local try = line == '' and word or line .. ' ' .. word
            if M.measure(try, font, size) <= room then
                line = try
            else
                if line ~= '' then
                    lines[#lines + 1] = line
                end
                while M.measure(word, font, size) > room and #word > 1 do
                    local n = #word
                    while n > 1 and M.measure(word:sub(1, n), font, size) > room do
                        n = n - 1
                    end
                    lines[#lines + 1] = word:sub(1, n)
                    word = word:sub(n + 1)
                end
                line = word
            end
        end
        lines[#lines + 1] = line
    end
    while #lines > 0 and lines[#lines] == '' do
        lines[#lines] = nil
    end
    return lines
end

-- a path cut from its start to fit, so its end (the file's name) still shows
local function fit_start(str, room, font, size)
    if M.measure(str, font, size) <= room then
        return str
    end
    local n = 1
    while n < #str and M.measure('…' .. str:sub(n), font, size) > room do
        n = n + 1
    end
    return '…' .. str:sub(n)
end

local function display_name(info)
    return (info.title ~= nil and info.title ~= '') and info.title or info.name
end

-------------------------------------------------------------------------------
-- Opening one
-------------------------------------------------------------------------------

-- The prompt for the package at `path` (from the inbox when `waiting`)
function install.open(path, waiting)
    if not available() then
        print('VanaCore: this VanaCore can\'t install addon packages: update it.')
        return
    end
    local info, why = api.package_info(path)
    local old = install.prompt
    if old ~= nil and old.path ~= path and old.state ~= 'done' and api.package_release ~= nil then
        pcall(api.package_release, old.path) -- (a prompt put aside keeps no claim on its file: it would never be offered again)
    end
    install.prompt = {
        path = path, waiting = waiting, info = info, why = why, state = 'ask', opens = true, at = clock(),
        boot = install.boot,
    }
end

-- Install from file (the Addons page, /vana install): Windows' Open dialog, then the prompt
function install.pick()
    if not available() then
        print('VanaCore: this VanaCore can\'t install addon packages: update it.')
        return
    end
    block_mouse(600000) -- the dialog holds the game's thread: no frame renews it
    local path = api.package_pick()
    block_mouse(400)
    if path ~= nil then
        install.open(path, false)
    end
end

-- /vana install [path]
function install.command(rest)
    rest = rest ~= nil and rest:gsub('^%s+', ''):gsub('%s+$', ''):gsub('^"(.*)"$', '%1') or ''
    if rest == '' then
        install.pick()
    else
        install.open(rest, false)
    end
end

local function close()
    local p = install.prompt
    if p ~= nil then
        install.boot = p.boot
        if api.package_done ~= nil then
            pcall(api.package_done, p.path)
        end
    end
    install.prompt = nil
end

-------------------------------------------------------------------------------
-- Installing
-------------------------------------------------------------------------------

local function is_loaded(name)
    for _, entry in ipairs(api.loaded()) do
        if entry.source == 'vanacore' and entry.name:lower() == name:lower() then
            return true
        end
    end
    return false
end

-- the choice made: unload the old one if it's loaded, then install (next frame on: the unload is a command)
local function choose(p, load)
    p.load = load
    p.reload = p.info.loaded
    p.state = 'working'
    p.since = clock()
    if p.info.loaded then
        vana.chat.run('/addon unload ' .. p.info.name)
    end
end

local function step(p)
    if p.state ~= 'working' then
        return
    end
    local info = p.info
    if is_loaded(info.name) then
        if clock() - p.since > 4 then
            p.state, p.error = 'failed', info.name .. ' didn\'t unload, so it wasn\'t replaced'
        end
        return
    end
    local ok, why = api.package_install(p.path)
    if not ok then
        p.state, p.error = 'failed', why or 'it couldn\'t be installed'
        if p.reload then
            vana.chat.run('/addon load ' .. info.name .. ' vanacore') -- the old one back as it was
        end
        return
    end
    if p.load then
        vana.chat.run('/addon load ' .. info.name .. ' vanacore')
    end
    p.at_start = p.boot and api.load_at_start(info.name) or false
    p.state, p.done_at = 'done', clock()
    M.refresh_at = clock() + 0.6 -- the Addons page lists it
end

-------------------------------------------------------------------------------
-- Drawing
-------------------------------------------------------------------------------

-- What the dialog says, laid out for its width: { badge, badge_colour, title, sub, sub_colour, desc, box, box_colour,
-- box_icon, facts }
local function content(p, room)
    local info = p.info
    local c = { facts = {} }
    if info == nil then
        c.badge, c.badge_colour = 'bang', P.rose
        c.title = 'This isn\'t an addon package'
        c.sub, c.sub_colour = p.path:match('[^\\/]+$') or p.path, P.dim
        c.box, c.box_colour, c.box_icon = p.why or 'It can\'t be read', P.rose, 'warning'
        c.facts[1] = { 'From', p.path, true }
        return c
    end
    local name = display_name(info)
    local by = info.author ~= '' and ('by ' .. info.author) or nil
    local version = info.version ~= '' and info.version or nil
    local update = info.installed_version ~= nil
    if p.state == 'done' then
        c.badge, c.badge_colour = 'check', P.teal
        c.title = name .. (version and (' ' .. version) or '') .. ' is installed'
        c.sub = p.load and (p.at_start and 'Loaded now, and at every start' or 'Loaded now')
                or (p.at_start and 'It loads at the next start' or 'Switch it on in Addons when you want it')
        c.sub_colour = P.teal
    elseif p.state == 'failed' or info.problem ~= nil then
        c.badge, c.badge_colour = 'bang', P.rose
        c.title = name .. (p.state == 'failed' and ' wasn\'t installed' or ' can\'t be installed')
        c.sub = table.concat({ version and ('Version ' .. version) or nil, by }, '  ·  ')
        c.sub_colour = P.dim
        c.box, c.box_colour, c.box_icon = p.error or info.problem, P.rose, 'warning'
    else
        c.badge, c.badge_colour = update and 'up' or 'down', P.accent
        c.title = (update and 'Update ' or 'Install ') .. name .. '?'
        local from_to = update and ((info.installed_version or '?') .. '  →  ' .. (version or '?'))
                        or (version and ('Version ' .. version) or nil)
        c.sub = table.concat({ from_to, by }, '  ·  ')
        c.sub_colour = P.dim
        c.box, c.box_colour, c.box_icon = WARNING, P.amber, 'warning'
    end
    if info.description ~= '' and p.state ~= 'done' then
        c.desc = wrap(info.description, room, fonts.text, BODY - 0.5)
        if #c.desc > MAX_DESC_LINES then
            for i = #c.desc, MAX_DESC_LINES + 1, -1 do
                c.desc[i] = nil
            end
            c.desc[MAX_DESC_LINES] = M.fit(c.desc[MAX_DESC_LINES] .. ' …', room, fonts.text, BODY - 0.5)
        end
    end
    c.facts[#c.facts + 1] = { 'From', info.from, true }
    c.facts[#c.facts + 1] = { 'Into', 'addons\\vanacore\\' .. info.name, false }
    c.facts[#c.facts + 1] = { 'Holds', info.files .. (info.files == 1 and ' file, ' or ' files, ') .. size_text(info.bytes), false }
    if update and p.state ~= 'done' and info.problem == nil then
        c.facts[#c.facts + 1] = { 'Keeps', (info.installed_version or '?') .. ' as a backup, in backups\\addons', false }
    end
    if info.homepage ~= '' then
        c.facts[#c.facts + 1] = { 'Site', info.homepage, false, link = true }
    end
    return c
end

local function box_lines(c, room)
    if c.box == nil then
        return nil
    end
    local text = c.box:find('[%.!?]$') and c.box or c.box .. '.' -- the installer's reasons are sentences without a stop
    return wrap(text, room - S(40), fonts.text, SMALL + 0.5)
end

local function height(c, room)
    local h = S(PAD) + S(HEAD)
    if c.desc ~= nil then
        h = h + S(14) + #c.desc * math.floor(S(BODY - 0.5) * LINE)
    end
    h = h + S(16) + #c.facts * S(FACT_H)
    local lines = box_lines(c, room)
    if lines ~= nil then
        h = h + S(16) + #lines * math.floor(S(SMALL + 0.5) * LINE) + S(22)
    end
    return h + S(22) + S(32) + S(PAD)
end

-- The status chip on the logo's corner: a disc of the status colour with its mark drawn in lines (exactly centred, sharp
-- at any scale): an arrow down (install), up (update), a tick (installed), an exclamation (can't)
local function draw_chip(dl, cx, cy, kind, colour)
    local r = S(11)
    M.circle(dl, cx, cy, r + S(2.5), P.bg) -- a ring of the card's colour, cutting it out of the logo
    M.circle(dl, cx, cy, r, colour)
    local ink, th = M.u32(P.ink), math.max(1.5, S(2.2))
    local function line(x1, y1, x2, y2)
        dl:AddLine({ cx + x1 * r, cy + y1 * r }, { cx + x2 * r, cy + y2 * r }, ink, th)
    end
    if kind == 'check' then
        line(-0.45, 0.02, -0.12, 0.36)
        line(-0.12, 0.36, 0.48, -0.32)
    elseif kind == 'bang' then
        line(0, -0.5, 0, 0.12)
        M.circle(dl, cx, cy + 0.46 * r, th * 0.75, P.ink)
    else
        local d = kind == 'up' and -1 or 1
        line(0, -0.48 * d, 0, 0.44 * d)
        line(-0.4, 0.06 * d, 0, 0.46 * d)
        line(0.4, 0.06 * d, 0, 0.46 * d)
    end
end

local function draw_head(dl, x, y, right, c)
    local size = S(LOGO)
    local ix, iy = x - S(6), y + S(24) - size / 2
    if not M.logo.tried then
        pcall(M.load_logo)
    end
    if M.logo.texture ~= nil then
        dl:AddImage(M.logo.texture, { ix, iy }, { ix + size, iy + size })
    else -- VanaCore's cat where the logo couldn't be loaded, as the About banner has it
        M.fill(dl, ix + S(4), iy + S(4), size - S(8), size - S(8), P.accent_lo, S(10))
        M.icon_centred(dl, ix + size / 2, iy + size / 2, 'cat', P.white, 20)
    end
    draw_chip(dl, ix + size * 0.84, iy + size * 0.84, c.badge, c.badge_colour)
    local tx = x + S(LOGO + 8)
    M.text_mid(dl, tx, y + S(13), M.fit(c.title, right - tx - S(30), fonts.semi, BODY + 3), P.white, fonts.semi, BODY + 3)
    if c.sub ~= nil and c.sub ~= '' then
        M.text_mid(dl, tx, y + S(37), M.fit(c.sub, right - tx, fonts.text, SMALL + 0.5), c.sub_colour, fonts.text, SMALL + 0.5)
    end
end

local function draw_facts(dl, x, y, right, c)
    local label_w = S(58)
    for i, fact in ipairs(c.facts) do
        local cy = y + (i - 0.5) * S(FACT_H)
        M.text_mid(dl, x, cy, fact[1]:upper(), P.faint, fonts.semi, SMALL - 1.5)
        local room = right - x - label_w
        local value = fact[3] and fit_start(fact[2], room, fonts.text, SMALL + 0.5) or M.fit(fact[2], room, fonts.text, SMALL + 0.5)
        local vx = x + label_w
        if fact.link then
            local w = M.measure(value, fonts.text, SMALL + 0.5)
            local over = M.over(vx, cy - S(9), w, S(18))
            M.text_mid(dl, vx, cy, value, over and P.accent2 or P.accent, fonts.text, SMALL + 0.5)
            if over then
                M.hline(dl, vx, vx + w, cy + S(8), P.accent2, 0.7)
                M.tooltip('Opens ' .. fact[2] .. ' in your browser')
                if imgui.IsMouseClicked(0) then
                    vana.system.open_url(fact[2])
                end
            end
        else
            M.text_mid(dl, vx, cy, value, P.text2, fonts.text, SMALL + 0.5)
            if value ~= fact[2] and M.over(vx, cy - S(9), room, S(18)) then
                M.tooltip(fact[2])
            end
        end
    end
    return y + #c.facts * S(FACT_H)
end

-- the warning (or the problem) in a tinted box with its icon
local function draw_box(dl, x, y, right, c)
    local lines = box_lines(c, right - x)
    if lines == nil then
        return y
    end
    local lh = math.floor(S(SMALL + 0.5) * LINE)
    local h = #lines * lh + S(22)
    M.fill(dl, x, y, right - x, h, M.mix(P.bg, c.box_colour, 0.09), S(6))
    M.outline(dl, x, y, right - x, h, c.box_colour, S(6), 0, 1, 0.35)
    M.icon_centred(dl, x + S(20), y + S(11) + lh / 2, c.box_icon, c.box_colour, 11)
    for i, line in ipairs(lines) do
        M.text_mid(dl, x + S(38), y + S(11) + (i - 0.5) * lh, line, P.text2, fonts.text, SMALL + 0.5)
    end
    return y + h
end

-- the buttons along the bottom, right to left; returns what was pressed
local function draw_buttons(p, x, right, cy)
    local info = p.info
    local working = p.state == 'working'
    local function button(id, label, icon, kind, at, enabled)
        local w = M.button_width(label, icon) + S(12)
        local clicked = M.button('##vcaddon.' .. id, at - w, cy, label, icon, kind, enabled ~= false and not working, nil, 30, w)
        return clicked, at - w - S(8)
    end
    if info == nil or p.state == 'done' or p.state == 'failed' or info.problem ~= nil then
        local done = button('close', p.state == 'done' and 'Done' or 'Close', nil, p.state == 'done' and 'accent' or 'ghost', right)
        return done and 'close' or nil
    end
    local update = info.installed_version ~= nil
    local go, at = button('load', working and 'Installing…' or (update and 'Update and load' or 'Install and load'), 'download', 'accent', right)
    local only, at2 = button('only', update and 'Update only' or 'Install only', nil, 'secondary', at)
    local later = button('later', 'Not now', nil, 'ghost', at2)
    -- Load at every start, on the left
    local on, switched = M.switch('##vcaddon.boot', x, cy, p.boot, not working)
    if switched then
        p.boot = on
    end
    M.text_mid(imgui.GetWindowDrawList(), x + S(M.SW_W) + S(10), cy, 'Load at every start', working and P.dim or P.text, fonts.text, SMALL + 0.5)
    if M.over(x, cy - S(10), S(M.SW_W) + S(10) + M.measure('Load at every start', fonts.text, SMALL + 0.5), S(20)) then
        M.tooltip('Adds it to scripts\\boot.txt, as its switch on the Addons page does')
    end
    return (go and 'load') or (only and 'only') or (later and 'close') or nil
end

local function draw_prompt(p, now)
    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
    -- the /vana window's frame keeps the scale and the animations' clock, but it isn't drawn while a prompt is up (vana.lua's
    -- present): they're kept here
    st.dt = math.max(0, math.min(0.1, now - st.last))
    st.last = now
    M.compute_scale(now, screen_h, M.user_scale() * 100)
    local pw = S(W)
    local room = pw - S(PAD) * 2
    local c = content(p, room)
    local ph = height(c, room)
    local dw, dh = M.display(screen_w, screen_h)
    if p.opens then
        p.opens = false
        imgui.OpenPopup('##vcaddon')
    end
    imgui.SetNextWindowPos({ dw / 2, dh / 2 }, 8, { 0.5, 0.5 }) -- (Appearing)
    imgui.SetNextWindowSize({ pw, ph }, 0)
    M.push_popup_style()
    imgui.PushStyleVar(M.VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleColor(M.COL.PopupBg, M.rgba(P.bg, 0))
    imgui.PushStyleColor(M.COL.Border, M.rgba(P.bg, 0))
    imgui.PushStyleColor(M.COL.ModalWindowDimBg, { 0.02, 0.03, 0.06, 0.55 }) -- the game dimmed behind it
    local shown = imgui.BeginPopupModal('##vcaddon', nil, 1 + 2 + 4 + 8 + 16 + 32 + 256 + M.NO_NAV)
    imgui.PopStyleColor(3)
    imgui.PopStyleVar(1)
    local pressed
    if shown then
        block_mouse()
        local x, y = imgui.GetWindowPos()
        local dl = imgui.GetWindowDrawList()
        -- the card, drawn past the window's own edges (its shadow would be cut square there): a soft shadow in layers,
        -- the page colour, the title block a shade lighter, and a hairline round it
        imgui.PushClipRect({ 0, 0 }, { dw, dh }, false)
        local r = S(RADIUS)
        for i = 4, 1, -1 do
            M.fill(dl, x - S(i * 2), y - S(i * 2) + S(4), pw + S(i * 4), ph + S(i * 4), '#000000', r + S(i * 2), 0, 0.07)
        end
        M.fill(dl, x, y, pw, ph, P.bg, r)
        M.fill(dl, x, y, pw, S(PAD) + S(HEAD) - S(4), '#FFFFFF', r, M.CORNERS.top, 0.025)
        M.outline(dl, x, y, pw, ph, P.border, r)
        local lx, right = x + S(PAD), x + pw - S(PAD)
        local top = y + S(PAD)
        draw_head(dl, lx, top, right, c)
        if p.state ~= 'working' and M.icon_button('##vcaddon.x', right - S(4), top + S(12), 'close', 11, P.rose) then
            pressed = 'close'
        end
        local cy = top + S(HEAD)
        M.hline(dl, x + S(1), x + pw - S(1), cy - S(4), P.line)
        if c.desc ~= nil then
            cy = cy + S(14)
            local lh = math.floor(S(BODY - 0.5) * LINE)
            for i, line in ipairs(c.desc) do
                M.text_mid(dl, lx, cy + (i - 0.5) * lh, line, P.text, fonts.text, BODY - 0.5)
            end
            cy = cy + #c.desc * lh
        end
        cy = draw_facts(dl, lx, cy + S(16), right, c)
        if c.box ~= nil then
            cy = draw_box(dl, lx, cy + S(16), right, c)
        end
        pressed = draw_buttons(p, lx, right, y + ph - S(PAD) - S(15)) or pressed
        imgui.PopClipRect()
        if pressed == 'close' then
            imgui.CloseCurrentPopup()
        end
        imgui.EndPopup()
    end
    M.pop_popup_style()
    return pressed
end

-- Each frame (vana.lua's present, the /vana window open or not): a waiting package taken up, the prompt drawn, the
-- install stepped along
function install.frame(now)
    if not available() then
        return
    end
    local p = install.prompt
    if p == nil then
        if now < install.next_look then
            return
        end
        install.next_look = now + 1
        local path = api.package_waiting()
        if path ~= nil then
            install.open(path, true)
        end
        return
    end
    if not fonts.ready then
        fonts.ready = true
        pcall(M.load_fonts)
        return
    end
    step(p)
    local pressed = draw_prompt(p, now)
    if pressed == 'close' then
        close()
    elseif pressed == 'load' or pressed == 'only' then
        choose(p, pressed == 'load')
    end
end

-- vana unloading (a reload) with a prompt up: the package let go unanswered, so the next vana offers it again
vana.events.on('unload', function ()
    local p = install.prompt
    if p ~= nil and p.state ~= 'done' and api ~= nil and api.package_release ~= nil then
        pcall(api.package_release, p.path)
    end
end)
