--[[
vana\hud.lua: the HUD Manager (/hud, or Open Manager under the /vana window's HUD page), a small window over the game while
every game window's box is up in it (vana.lua's hud.boxes: Shift + drag moves one, a click picks it here). Loaded by
vana.lua with the shared table M; drawn each frame it's open, the /vana window hidden till it's closed (hud.open_manager).

  title line   its name, the HUD presets (the one picked is loaded) and their edit (the /vana window at HUD > Presets),
               Addon View, Open only and the HUD page's Grid, Snap and Names switches as icons, and close
  element      the game windows (the HUD page's element sections, data.lua) in a drop-down, < and > stepping through
               them (with Open only, only those open in the game now); the one picked has its box picked in the game
               too, for the arrow keys. Its reset after it
  settings     the element's rows, a line each, narrower than /vana's: a place's X and Y on lines of their own under it

Addon View (/hud addons, [vana] hud.addons): yellow boxes over the addons' and plugins' windows and their text and shapes
in place of the game windows' blue ones (vana.lua's hud.addon_boxes); the drop-down is the addons, and the settings the
picked one's windows, each's X, Y and Scale as Addons > Layout has them (hud.addon_section).

What the mouse does to the boxes is the HUD page's Controls, at the top of its Settings.
--]]

local M = ...
local imgui = vana.imgui
local S, P, fonts = M.S, M.P, M.fonts
local BODY, SMALL = M.BODY, M.SMALL
local st = M.st

-- pick: the element shown (its section's title), nil for none; back: the /vana window's page, hidden while this is open;
-- edit: the presets' edit clicked, the /vana window opened the next frame (vana.lua's present)
-- addon: the Addon View's addon shown (its owner, 'ashita/xiui'), nil for none; view: the view drawn last frame
M.hudman = { open = false, pick = nil, back = nil, edit = false, addon = nil, view = nil }
local hm = M.hudman

local WIN_W = 384
local PAD = 14
local TITLE_H = 38
local PICK_H = 50     -- the element's drop-down
local PICK = { size = BODY + 1, h = 32, list_w = 340, list_h = 640, search = true } -- ... and how it looks (draw.lua's M.dropdown `look`)
local ROW_H = 27
local SUB_H = 32      -- a sub-panel's heading, with room above it
local BODY_MAX = 460  -- the settings' height before they scroll
local NAME = BODY - 1 -- the rows' names
local INDENT = 12     -- a place's X and Y, under its name
local TOGGLE_W = 30   -- a title line switch's icon, centre to centre (and the last's to close)
local EDIT_GAP = 13   -- the presets' edit, its centre from their drop-down's right edge
-- NoBringToFrontOnFocus, while the /vana window is up: a click here leaves this under it
local UNDER_VANA = 8192

-- the HUD page's own switches, icons on the title line (data.lua's Settings rows, read and written the same way); Addon View
-- and Open only are this window's alone (vana.lua's hud.addon_view and hud.open_only: a VanaCore.Core.dll with
-- vana.layout.box and vana.layout.up)
local SWITCHES = {
    { kind = 'toggle', label = 'Addon View', icon = 'puzzle', section = 'vana', key = 'hud.addons', default = false,
      tip = "The addons' windows and text, in yellow boxes, in place of the game's windows",
      on_color = { P.hud, '#FFF6C4' }, -- (in the boxes' yellow while on)
      usable = function () return vana.layout.box ~= nil end },
    { kind = 'toggle', label = 'Open only', icon = 'filter', section = 'vana', key = 'hud.open_only', default = false,
      tip = 'Only the windows open in the game now (in the Addon View, the addons drawing now): their boxes, and them in the list',
      usable = function () return vana.layout.up ~= nil end },
    { kind = 'toggle', label = 'Grid', icon = 'grid', section = 'vana', key = 'hud.grid', default = true, rebuild = true },
    { kind = 'toggle', label = 'Snap to grid', icon = 'magnet', section = 'vana', key = 'hud.snap', default = true },
    { kind = 'toggle', label = 'Element names', icon = 'tag', section = 'vana', key = 'hud.names', default = true },
}

local function clock()
    return vana.time.clock()
end

-- a picker (st.picker: a colour's, a slider's typed number) opened by a row of this window, by its id; beside this window,
-- not /vana's (vana.lua's draw_window leaves it)
function M.hud_owns(picker)
    return picker ~= nil and type(picker.id) == 'string' and picker.id:sub(1, 7) == '##hudm.'
end

-- the element titled `title` shown (nil: none), and a box of it picked in the game for the arrow keys: `box` (the one
-- clicked), else its first
function M.hud_pick(title, box)
    if title ~= hm.pick and M.hud_owns(st.picker) then
        st.picker = nil
    end
    hm.pick = title
    if box == nil and title ~= nil then
        for _, sec in ipairs(M.hud.elements()) do
            if sec.title == title then
                for _, row in ipairs(sec.rows) do
                    box = box or row.outline
                end
            end
        end
    end
    if vana.layout.select ~= nil then
        vana.layout.select(box or '')
    end
end

function M.hud_open()
    hm.open = true
    if hm.pick ~= nil and not M.hud.addon_view() then
        M.hud_pick(hm.pick) -- (its box picked again)
    end
end

-- the Addon View's addon `owner` shown ('ashita/xiui'; nil: none): its boxes picked in the game, for the arrow keys
function M.hud_pick_addon(owner)
    if owner ~= hm.addon and M.hud_owns(st.picker) then
        st.picker = nil
    end
    hm.addon = owner
end

function M.hud_close()
    hm.open = false
    if M.hud_owns(st.picker) then
        st.picker = nil
    end
    if vana.layout.select ~= nil then
        vana.layout.select('')
    end
    M.flush()
end

-------------------------------------------------------------------------------
-- The element's rows
-------------------------------------------------------------------------------

-- a tooltip once the mouse has rested on `key`'s name, as /vana's rows have theirs
local rest = { key = nil, since = 0, seen = false }

local function rest_tip(key, text)
    if text == nil then
        return
    end
    local now = clock()
    if rest.key ~= key then
        rest.key, rest.since = key, now
    elseif now - rest.since > 0.4 then
        M.tooltip(text)
    end
    rest.seen = true
end

-- a row's name from x, cut to `room` (the whole in its tooltip)
local function name(dl, id, text, tip, x, cy, room, color, alpha, size)
    size = size or NAME
    local shown = M.fit(text, room, fonts.text, size)
    M.text_mid(dl, x, cy, shown, color, fonts.text, size, alpha)
    if M.over(x, cy - S(ROW_H) / 2, room, S(ROW_H)) then
        rest_tip(id, shown ~= text and (tip ~= nil and text .. ': ' .. tip or text) or tip)
    end
end

-- a place's sliders as lines under its name: an X and the Y after it share one, as do an opacity and the colour after it
-- (the chat's Background and Border: `tint`, the colour's swatch at the line's end); the rest (a Width, a nudge's Dot) a
-- line each
local function sub_lines(row)
    local lines, i = {}, 1
    while i <= #row.sliders do
        local sub, after = row.sliders[i], row.sliders[i + 1]
        if sub.label == 'X' and after ~= nil and after.label == 'Y' then
            lines[#lines + 1] = { sub, after }
            i = i + 2
        elseif sub.kind == 'slider' and after ~= nil and after.kind == 'colour' then
            lines[#lines + 1] = { sub, after, tint = true }
            i = i + 2
        else
            lines[#lines + 1] = { sub }
            i = i + 1
        end
    end
    return lines
end

-- A place's own sub-panel (a heading, then only its Position or Sub-menu row and its Anchor: the Main Menu's sub-menus,
-- most windows' Position) read as one: its switch (or Game / Chat / Custom) on the heading's line, its Anchor beside it
-- and its X and Y under it only while it's placed. The rest of the rows as they are. (A dozen sub-menus at four lines each
-- were a wall of greyed rows: 2026-10-22)
local GENERIC = { Position = true, ['Sub-menu'] = true }

-- The element drop-down's categories, in the order they are listed (data.lua's element(text, group)); an element with none
-- is under Misc, last
local GROUP_ORDER = { 'Action Menu', 'Chat', 'Combat', 'Help Bar', 'Main Menu', 'Maps', 'Party', 'Windows' }

-- the elements as the drop-down's options: each category's name as a heading (never picked), then its elements as they come
-- (those `keep` takes)
local function grouped_options(elements, keep)
    local by, order = {}, {}
    for _, group in ipairs(GROUP_ORDER) do
        by[group] = {}
        order[#order + 1] = group
    end
    by.Misc = {}
    for _, s in ipairs(elements) do
        local group = s.head ~= nil and s.head.group or 'Misc'
        if by[group] == nil then -- (a category GROUP_ORDER doesn't know: after the others, before Misc)
            by[group] = {}
            order[#order + 1] = group
        end
        if keep(s) then
            local list = by[group]
            list[#list + 1] = { s.title, s.title }
        end
    end
    order[#order + 1] = 'Misc'
    local options = {}
    for _, group in ipairs(order) do
        if #by[group] > 0 then
            options[#options + 1] = { header = group }
            for _, option in ipairs(by[group]) do
                options[#options + 1] = option
            end
        end
    end
    return options
end

-- The Addon View's drop-down: the addons (vana.lua's hud.addons) under their kind's heading, by name; with Open only, those
-- drawing now (the one shown stays)
local KIND_ORDER = { { 'ashita', 'Ashita addons' }, { 'windower', 'Windower addons' }, { 'vanacore', 'VanaCore addons' },
                     { 'plugin', 'Plugins' } }
local function addon_options(addons)
    local open_only = M.hud.open_only()
    local by = {}
    for _, addon in ipairs(addons) do
        if addon.shown or not open_only or addon.owner == hm.addon then
            local list = by[addon.kind] or {}
            by[addon.kind] = list
            list[#list + 1] = { addon.owner, addon.name }
        end
    end
    local options = {}
    for _, kind in ipairs(KIND_ORDER) do
        if by[kind[1]] ~= nil then
            options[#options + 1] = { header = kind[2] }
            for _, option in ipairs(by[kind[1]]) do
                options[#options + 1] = option
            end
            by[kind[1]] = nil
        end
    end
    for _, list in pairs(by) do -- (a kind KIND_ORDER doesn't know)
        for _, option in ipairs(list) do
            options[#options + 1] = option
        end
    end
    return options
end

-- the element's boxes in the game (its rows' outline: the ids vana.layout.outline draws)
local function boxes_of(sec)
    local ids = {}
    for _, row in ipairs(sec.rows) do
        if row.outline ~= nil then
            ids[#ids + 1] = row.outline
        end
    end
    return ids
end

-- An element in the drop-down: any, or with Open only (vana.lua's hud.up_now) one whose window is open in the game now. The
-- one picked stays (closed while it was being set, it dropped out from under the mouse), as does one whose window can't be
-- told (no box, or one vana.layout.up can't tell: the chat's, the compass's)
local function listed(sec)
    local up = M.hud.up_now
    if up == nil or sec.title == hm.pick then
        return true
    end
    local ids = boxes_of(sec)
    for _, id in ipairs(ids) do
        if up[id] ~= false then
            return true
        end
    end
    return #ids == 0
end

-- an element with a box in the game starts with its Toggle Move Widget row (draw_row's 'widget')
local function view_of(sec)
    local view, rows, i = {}, sec.rows, 1
    local ids = boxes_of(sec)
    if #ids > 0 then
        view[1] = { kind = 'widget', label = 'Toggle Move Widget', boxes = ids }
    end
    while i <= #rows do
        local row, place = rows[i], rows[i + 1]
        local anchor = rows[i + 2] ~= nil and rows[i + 2].kind == 'choice' and rows[i + 2].label == 'Anchor' and rows[i + 2] or nil
        local after = rows[i + (anchor ~= nil and 3 or 2)]
        if row.kind == 'subheader' and place ~= nil and place.kind == 'layout' and (place.toggle ~= nil or place.modes ~= nil)
           and GENERIC[place.label] and (after == nil or after.kind == 'subheader') then
            view[#view + 1] = { kind = 'block', head = row, place = place, anchor = anchor }
            i = i + (anchor ~= nil and 3 or 2)
        else
            view[#view + 1] = row
            i = i + 1
        end
    end
    return view
end

-- a block's place set by the player: its switch on, or Custom of Game / Chat / Custom
local function placed(place)
    if place.toggle ~= nil then
        local ok, on = pcall(place.toggle.get)
        return ok and on or false
    end
    local ok, index = pcall(place.modes.get)
    return ok and index == 3
end

local function row_height(row, first)
    if row.kind == 'block' then
        return (first and S(ROW_H) or S(SUB_H)) + (placed(row.place) and #sub_lines(row.place) * S(ROW_H) or 0)
    end
    if row.show_if ~= nil then
        local ok, on = pcall(row.show_if)
        if not (ok and on) then
            return 0
        end
    end
    if row.kind == 'subheader' then
        return first and S(ROW_H) or S(SUB_H)
    elseif row.kind == 'widget' then
        return S(ROW_H)
    elseif row.kind == 'layout' then
        return S(ROW_H) * (1 + #sub_lines(row))
    elseif row.kind == 'toggle' or row.kind == 'choice' or row.kind == 'slider' or row.kind == 'colour' then
        return S(ROW_H)
    end
    return 0 -- (a note: the tooltips say it)
end

-- a choice at the row's right: segments when they fit beside its name, else a drop-down. Returns the room it takes.
local function draw_choice(row, id, x, cy, w, enabled, alpha)
    local options, labels = {}, {}
    for _, option in ipairs(row.options) do
        if not option.sep then
            options[#options + 1] = option
            labels[#labels + 1] = option[2]
        end
    end
    local value = M.read(row)
    local name_w = M.measure(row.label, fonts.text, NAME)
    local total = M.segments_width(labels, SMALL)
    if not row.dropdown and #labels <= 4 and name_w + S(12) + total <= w then
        local index = 0
        for i, option in ipairs(options) do
            if option[1] == value then
                index = i
            end
        end
        local picked, moved, over = M.segments(id, x + w - total, cy, labels, index, enabled, SMALL, alpha)
        if over ~= nil and options[over].tip ~= nil then
            M.tooltip(options[over].tip)
        end
        if moved then
            M.changed(row, options[picked][1], true)
        end
        return total
    end
    local dw = math.max(S(110), math.min(w - name_w - S(12), S(190)))
    local picked, moved = M.dropdown(id, x + w - dw, cy, dw, row.options, value, enabled, alpha)
    if moved then
        M.changed(row, picked, true)
    end
    return dw
end

-- a place's switch (a mover's Move: off, the game places it) or its modes (Game, Chat, Custom), its right edge at
-- `right`. The switch has its state before it, Pinned or Moved, so that a moved window reads as one that can be pinned
-- back (a moved Interaction Menu looked as if it couldn't be, 2026-10-23). Returns whether it's placed and the room it takes.
local function draw_place_control(row, id, right, cy, enabled, alpha)
    if row.toggle ~= nil then
        local ok, on = pcall(row.toggle.get)
        on = ok and on or false
        local new, flipped = M.switch(id .. '.move', right - S(M.SW_W), cy, on, enabled, alpha)
        if flipped then
            M.flush()
            pcall(row.toggle.set, new)
            on = new
        end
        local word = on and 'Moved' or 'Pinned'
        local word_w = M.measure(word, fonts.text, SMALL)
        local word_x = right - S(M.SW_W) - S(6) - word_w
        M.text_mid(imgui.GetWindowDrawList(), word_x, cy, word, on and P.text or P.dim, fonts.text, SMALL, alpha)
        if M.over(word_x, cy - S(10), right - word_x, S(20)) then
            M.tooltip(on and 'Moved: at its X and Y. Off pins it back where the game puts it, as Reset does.'
                          or 'Pinned where the game puts it. On, or its box dragged in the game: at its X and Y.')
        end
        return on, right - word_x
    end
    local ok, index = pcall(row.modes.get)
    index = ok and index or 1
    local w = M.segments_width(row.modes.labels, SMALL)
    local picked, moved, over = M.segments(id .. '.mode', right - w, cy, row.modes.labels, index, enabled, SMALL, alpha)
    if moved then
        M.flush()
        pcall(row.modes.set, picked)
        index = picked
    end
    if over ~= nil and row.modes.tips ~= nil and row.modes.tips[over] ~= nil then
        M.tooltip(row.modes.tips[over])
    end
    return index == 3, w
end

-- A place's sliders (or colours) on lines from `cy` (the first's middle), set in from x: X and Y side by side, an opacity
-- and its colour too (sub_lines); a line's first track starts where a lone slider's does, under the longest of their labels
local function draw_subs(dl, row, id, x, cy, w, enabled, alpha)
    local lines = sub_lines(row)
    local first_w = M.measure('X', fonts.text, SMALL) + S(10)
    for _, line in ipairs(lines) do
        if #line == 1 or line.tint then
            first_w = math.max(first_w, M.measure(line[1].label, fonts.text, SMALL) + S(10))
        end
    end
    local sx, sw = x + S(INDENT), w - S(INDENT)
    local gap = S(16)
    for i, line in ipairs(lines) do
        local scy = cy + S(ROW_H) * (i - 1)
        -- each sub's { left, width, label's width (0: none) }
        local spans
        if line.tint then -- the swatch (its box, a gap, the hex) at the end, no label: the line's name says what
            local value = M.read(line[2])
            local swatch_w = S(23) + M.measure(value ~= '' and value or "Game's", fonts.text, SMALL + 0.5)
            spans = { { sx, sw - swatch_w - gap, first_w }, { sx + sw - swatch_w, swatch_w, 0 } }
        elseif #line == 2 then
            local hw = (sw - gap) / 2
            spans = { { sx, hw, first_w }, { sx + hw + gap, hw, M.measure(line[2].label, fonts.text, SMALL) + S(10) } }
        else
            spans = { { sx, sw, first_w } }
        end
        for k, sub in ipairs(line) do
            local at, sub_w, label_w = spans[k][1], spans[k][2], spans[k][3]
            local sub_on = enabled
            if sub.lock_if ~= nil then
                local ok, locked = pcall(sub.lock_if)
                sub_on = sub_on and not (ok and locked)
            end
            local sub_id = id .. '.' .. tostring(sub.key or sub.label)
            if label_w > 0 then
                name(dl, sub_id, sub.label, M.tip_of(sub), at, scy, label_w - S(4), P.dim, alpha, SMALL)
            end
            if sub.kind == 'colour' then
                M.draw_colour(sub, sub_id, at + sub_w, scy, sub_on, alpha)
            else
                M.draw_slider(sub, sub_id, at + label_w, scy, sub_on, alpha, sub_w - label_w, S(40))
            end
        end
    end
end

-- a row's id: by its setting, else its first slider's (the Main Menu's Sub-menu rows share a name), else its name
local function id_of(sec, row)
    local own = row.key or (row.sliders ~= nil and row.sliders[1] ~= nil and row.sliders[1].key) or row.label or row.text
    -- (row.source: rows sharing a key, data.lua's hide_window rows all hide.windows, each its own: one id, they were one
    -- switch's knob, eased between them)
    return '##hudm.' .. sec.title .. '.' .. (row.section or '') .. '.' .. tostring(own) .. (row.source ~= nil and ('.' .. row.source) or '')
end

-- A place's own sub-panel as one (view_of): the heading's title and line, then its Anchor (while placed) and its switch at
-- the line's end; its X and Y under it while placed
local function draw_block(sec, block, x, y, w, h, first)
    local dl = imgui.GetWindowDrawList()
    local place = block.place
    local id = id_of(sec, place)
    local alpha, enabled = M.row_state(place, {})
    local right = x + w
    local cy = y + (first and S(ROW_H) or S(SUB_H)) - S(ROW_H) / 2
    local on, control_w = draw_place_control(place, id, right, cy, enabled, alpha)
    local line_end = right - control_w - S(10)
    if on and block.anchor ~= nil then -- Left / Right: which side snaps to the grid as it's dragged
        local anchor = block.anchor
        local labels = { anchor.options[1][2], anchor.options[2][2] }
        local aw = M.segments_width(labels, SMALL - 1)
        local index = M.read(anchor) == anchor.options[2][1] and 2 or 1
        local picked, moved = M.segments(id .. '.anchor', line_end - aw, cy, labels, index, enabled, SMALL - 1, alpha)
        if moved then
            M.changed(anchor, anchor.options[picked][1], true)
        end
        if M.over(line_end - aw, cy - S(10), aw, S(20)) then
            rest_tip(id .. '.anchor', 'Anchor: the side that snaps to the grid as it\'s dragged')
        end
        line_end = line_end - aw - S(10)
    end
    local tw = M.text_spaced(dl, x, cy, block.head.text:upper(), on and P.teal or M.mix(P.teal, P.dim, 0.5),
                             fonts.semi_italic or fonts.semi, SMALL - 1, alpha, S(1.2))
    if line_end > x + tw + S(8) then
        M.hline(dl, x + tw + S(8), line_end, cy, P.teal, (on and 0.45 or 0.2) * alpha)
    end
    if on then
        draw_subs(dl, place, id, x, cy + S(ROW_H), w, enabled, alpha)
    end
    return h
end

-- A row: its name on the left, its control on the right; a place's (a mover's, a pair's) sliders on lines under its
-- name. Returns its height.
local function draw_row(sec, row, x, y, w, first)
    local h = row_height(row, first)
    if h == 0 then
        return 0
    end
    if row.kind == 'block' then
        return draw_block(sec, row, x, y, w, h, first)
    end
    local dl = imgui.GetWindowDrawList()
    local id = id_of(sec, row)
    if row.kind == 'widget' then
        -- the element's move widget: its boxes drawn in the game or not (the element still picks; its sliders still move
        -- it), the eye open or shut by it, in light yellow
        local cy = y + S(ROW_H) / 2
        if M.over(x - S(6), y, w + S(12), h) then
            M.fill(dl, x - S(6), y, w + S(12), h, P.hover, S(3), 0, 0.8)
        end
        local shown = M.hud.widget_shown(row.boxes[1])
        local on, flipped = M.switch(id, x + w - S(M.SW_W), cy, shown, true, 1)
        if flipped then
            M.hud.show_widgets(row.boxes, on)
        end
        M.icon_centred(dl, x + S(7), cy, on and 'eye' or 'eye_off', P.hud, 11, 1)
        name(dl, id, row.label, 'Its box in the game, to drag. Off, it isn\'t drawn: pick it here and use its sliders',
             x + S(20), cy, w - S(M.SW_W) - S(30), P.hud, 1)
        return h
    end
    local alpha, enabled = M.row_state(row, {})
    local cy = y + S(ROW_H) / 2
    local right = x + w

    if row.kind == 'subheader' then
        -- a sub-panel's heading: its title in small spaced capitals, in teal, and a line after it
        cy = y + h - S(ROW_H) / 2
        local tw = M.text_spaced(dl, x, cy, row.text:upper(), P.teal, fonts.semi_italic or fonts.semi, SMALL - 1, alpha, S(1.2))
        if right > x + tw + S(8) then
            M.hline(dl, x + tw + S(8), right, cy, P.teal, 0.45 * alpha)
        end
        return h
    end
    if M.over(x - S(6), y, w + S(12), h) then
        M.fill(dl, x - S(6), y, w + S(12), h, P.hover, S(3), 0, 0.8)
    end
    local color = enabled and P.text or P.dim
    if row.kind == 'toggle' then
        local on, flipped = M.switch(id, right - S(M.SW_W), cy, M.read(row), enabled, alpha)
        if flipped then
            M.changed(row, on, true)
        end
        name(dl, id, row.label, M.tip_of(row), x, cy, w - S(M.SW_W) - S(10), color, alpha)
    elseif row.kind == 'slider' then
        local sw = math.min(S(200), math.floor(w * 0.62))
        M.draw_slider(row, id, right - sw, cy, enabled, alpha, sw, S(44))
        name(dl, id, row.label, M.tip_of(row), x, cy, w - sw - S(8), color, alpha)
    elseif row.kind == 'colour' then
        M.draw_colour(row, id, right, cy, enabled, alpha)
        name(dl, id, row.label, M.tip_of(row), x, cy, w - S(100), color, alpha)
    elseif row.kind == 'choice' then
        local room = draw_choice(row, id, x, cy, w, enabled, alpha)
        name(dl, id, row.label, M.tip_of(row), x, cy, w - room - S(10), color, alpha)
    elseif row.kind == 'layout' then
        -- its name, then its Move switch or its modes (a pair has neither: always placed); its sliders under it
        local on, control_w = true, 0
        if row.toggle ~= nil or row.modes ~= nil then
            on, control_w = draw_place_control(row, id, right, cy, enabled, alpha)
        end
        if row.flags ~= nil then -- (an addon's window: its Lock and No Pointer Events, data.lua layout.flags_of)
            control_w = control_w + M.icon_switches(row.flags, id .. '.flags', right - control_w, cy, enabled)
        end
        name(dl, id, row.label, M.tip_of(row), x, cy, w - control_w - S(10), on and color or P.dim, alpha)
        draw_subs(dl, row, id, x, cy + S(ROW_H), w, enabled, alpha)
    end
    return h
end

local function rows_height(view)
    local h = 0
    for i, row in ipairs(view) do
        h = h + row_height(row, i == 1)
    end
    return h
end

-------------------------------------------------------------------------------
-- The window
-------------------------------------------------------------------------------

-- the title line's switches, icons alone, right to left from `right` (an icon's centre) as far apart as the last is from
-- close: the accent's blue while on, grey while off. Returns the left edge of the first.
local function draw_switches(dl, right, cy)
    local cx = right
    local r = S(11)
    for i = #SWITCHES, 1, -1 do
        local row = SWITCHES[i]
        local usable = row.usable == nil or row.usable()
        local on = usable and M.read(row)
        local on_color = row.on_color or { P.accent, P.accent2 }
        local clicked, hovered = M.icon_button('##hudm.switch.' .. row.key, cx, cy, row.icon, 12.5, on and on_color[2] or P.text2,
                                               on and on_color[1] or P.faint, usable)
        if hovered and not usable then
            M.tooltip(row.label .. ': needs a newer VanaCore.Core.dll')
        elseif hovered then
            M.tooltip(row.label .. (on and ': on' or ': off') .. (M.tip_of(row) ~= nil and '\n' .. M.tip_of(row) or ''))
        end
        if clicked then
            M.changed(row, not on, true)
        end
        cx = cx - S(TOGGLE_W)
    end
    return cx + S(TOGGLE_W) - r
end

function M.draw_hud(now)
    if not fonts.ready then
        fonts.ready = true
        pcall(M.load_fonts)
        return
    end
    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
    -- with the /vana window shut its frame doesn't run: the scale and the animations' clock kept here (by the Debug
    -- window when it's open: it's drawn first)
    if not st.open and not M.debug.open then
        st.dt = math.max(0, math.min(0.1, now - st.last))
        st.last = now
        M.compute_scale(now, screen_h, M.user_scale() * 100)
    end

    -- Addon View or the game's windows: switched, the other's picked boxes let go
    local addon_view = M.hud.addon_view()
    if hm.view ~= nil and hm.view ~= addon_view then
        if M.hud_owns(st.picker) then
            st.picker = nil
        end
        if addon_view then
            if vana.layout.select ~= nil then
                vana.layout.select('') -- (the element shown stays, for when the view is switched back)
            end
        else
            M.hud_pick(hm.pick)
        end
    end
    hm.view = addon_view
    local elements, addons, addon = M.hud.elements(), nil, nil
    local sec = nil
    if addon_view then
        local by
        addons, by = M.hud.addons()
        addon = hm.addon ~= nil and by[hm.addon] or nil
        hm.addon = addon ~= nil and hm.addon or nil -- (unloaded: none)
        sec = addon ~= nil and M.hud.addon_section(addon) or nil
    else
        for _, s in ipairs(elements) do
            if s.title == hm.pick then
                sec = s
            end
        end
        if sec == nil then
            hm.pick = nil -- (its section gone: none)
        end
    end
    local pw = S(WIN_W)
    local fixed = S(TITLE_H) + S(PICK_H)
    local body_h = sec ~= nil and rows_height(view_of(sec)) + S(14) or S(48)
    body_h = math.max(S(48), math.min(body_h, S(BODY_MAX), screen_h - fixed - S(40)))
    local ph = fixed + body_h

    local dw, dh, k = M.display(screen_w, screen_h) -- ImGui's pixels, where it's drawn k times its size
    imgui.SetNextWindowPos({ dw - (pw + S(40)) * k, S(140) * k }, M.FIRST_USE_EVER)
    imgui.SetNextWindowSize({ pw, ph }, 0)
    local COL, VAR = M.COL, M.VAR
    local colours = {
        { COL.ChildBg, P.bg, 0 }, { COL.ScrollbarBg, P.bg, 0 }, { COL.ScrollbarGrab, '#3A4158', 1 },
        { COL.ScrollbarGrabHovered, '#4A5270', 1 }, { COL.ScrollbarGrabActive, P.accent_lo, 1 },
    }
    for _, c in ipairs(colours) do
        imgui.PushStyleColor(c[1], M.rgba(c[2], c[3]))
    end
    imgui.PushStyleVar(VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(VAR.WindowRounding, 0)
    imgui.PushStyleVar(VAR.WindowBorderSize, 0)
    imgui.PushStyleVar(VAR.ChildRounding, 0)
    imgui.PushStyleVar(VAR.ChildBorderSize, 0)
    imgui.PushStyleVar(VAR.ItemSpacing, { 0, 0 })
    imgui.PushStyleVar(VAR.ScrollbarSize, S(6))
    imgui.PushStyleVar(VAR.ScrollbarRounding, S(3))
    local closing, editing = false, false
    local x, y = 0, 0
    if imgui.Begin('HUD###vana_hud', nil, M.WINDOW_FLAGS + ((st.open and not st.minimised) and UNDER_VANA or 0)) then
        x, y = imgui.GetWindowPos()
        -- kept on the screen, as the /vana window is (the clip widened to the screen where it was put back)
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
        M.fill(dl, x - 1, y, pw + 2, ph + 2, '#000000', radius + 1, 0, 0.35)
        M.fill(dl, x, y, pw, ph, P.bg, radius)
        M.outline(dl, x, y, pw, ph, P.border, radius)
        local lx, w = x + S(PAD), pw - S(PAD) * 2

        -- the title line: its name in a light yellow, a touch deeper to the right; the HUD presets and their edit; Grid,
        -- Snap and Names; close
        local ty = y + S(TITLE_H) / 2
        local title_w = M.gradient_text(dl, lx, ty, 'HUD', '#FFF7D1', P.hud, fonts.semi, BODY + 1)
        local close_x = x + pw - S(PAD) - S(4)
        local close_clicked, over_close = M.icon_button('##hudm.close', close_x, ty, 'close', 12, P.rose)
        if over_close then
            M.tooltip(hm.back ~= nil and 'Close, back to the VanaCore window' or 'Close (/hud)')
        end
        closing = close_clicked
        local switches_x = draw_switches(dl, close_x - S(TOGGLE_W), ty)
        local px = lx + title_w + S(12)
        local pick_right = M.hud.preset_pick('##hudm.preset', px, ty, switches_x - S(EDIT_GAP) * 2 - px)
        local edit_clicked, over_edit = M.icon_button('##hudm.edit', pick_right + S(EDIT_GAP), ty, 'pen', 11, P.text)
        if over_edit then
            M.tooltip('Edit the HUD presets (the VanaCore window, HUD > Presets)')
        end
        editing = edit_clicked
        M.hline(dl, x, x + pw, y + S(TITLE_H), P.border)

        -- the element: the game windows by name, in categories (the Addon View: the addons, by kind); its reset after it
        local py = y + S(TITLE_H) + S(PICK_H) / 2
        local options = addon_view and addon_options(addons) or grouped_options(elements, listed)
        local current = addon_view and hm.addon or hm.pick
        local reset_w = S(28)
        local picked, moved = M.dropdown(addon_view and '##hudm.pick.addon' or '##hudm.pick', lx, py, w - reset_w, options, current or '',
                                         #options > 0, 1, true, true, PICK)
        if moved and addon_view then
            M.hud_pick_addon(picked ~= '' and picked or nil)
            local _, by = M.hud.addons()
            addon = hm.addon ~= nil and by[hm.addon] or nil
            sec = addon ~= nil and M.hud.addon_section(addon) or nil
        elseif moved then
            M.hud_pick(picked ~= '' and picked or nil)
            sec = nil
            for _, s in ipairs(elements) do
                if s.title == hm.pick then
                    sec = s
                end
            end
        end
        if current == nil then -- (the drop-down's own text is empty: the value isn't one of them)
            M.text_mid(dl, lx + S(6), py - S(0.5), addon_view and 'Select an addon' or 'Select an element', P.dim, fonts.text, PICK.size)
        end
        local reset_clicked, over_reset = M.icon_button('##hudm.reset', lx + w - S(8), py, 'reset', 10, P.text, P.faint, sec ~= nil)
        if over_reset and sec ~= nil then
            M.tooltip(addon_view and 'Reset ' .. sec.title .. "'s windows: their places and scale as its addon has them"
                                 or 'Reset ' .. sec.title .. ' to the defaults')
        end
        if reset_clicked and sec ~= nil then
            if addon_view then
                M.hud.addon_reset(addon)
            else
                M.reset_rows(sec.rows)
            end
            if M.hud_owns(st.picker) then
                st.picker = nil
            end
        end
        M.hline(dl, x, x + pw, y + S(TITLE_H) + S(PICK_H), P.line)

        -- its settings, scrolling (each element its own scroll)
        local by = y + S(TITLE_H) + S(PICK_H) + 1
        imgui.SetCursorScreenPos({ x, by })
        local body_id = addon_view and 'addon.' .. (hm.addon or '') or (hm.pick or '')
        if imgui.BeginChild('##hudm.body.' .. body_id, { pw, body_h - 1 }, 0, M.NO_NAV) then
            local bx, cy0 = imgui.GetCursorScreenPos()
            local bw = (imgui.GetContentRegionAvail())
            if sec == nil then
                local empty = not addon_view and 'Click a box in the game, or pick one above'
                              or #options == 0 and 'No addon has a window or text on the screen'
                              or 'Click a yellow box in the game, or pick one above'
                M.text_centred(imgui.GetWindowDrawList(), bx, cy0, bw, body_h - 1, empty, P.dim, fonts.text, SMALL + 0.5)
            else
                local ry = cy0 + S(7)
                for i, row in ipairs(view_of(sec)) do
                    ry = ry + draw_row(sec, row, bx + S(PAD), ry, bw - S(PAD) * 2, i == 1)
                end
                imgui.SetCursorScreenPos({ bx, ry })
                imgui.Dummy({ 1, S(7) })
            end
        end
        imgui.EndChild()
        if clipped then
            imgui.PopClipRect()
        end
    end
    imgui.End()
    imgui.PopStyleVar(8)
    imgui.PopStyleColor(#colours)
    if not rest.seen then
        rest.key = nil
    end
    rest.seen = false

    if M.hud_owns(st.picker) then -- a colour's picker or a typed number, beside this window
        M.draw_pickers(x, y, pw, ph, screen_w, screen_h)
    end
    if closing then
        M.hud.close_manager()
    elseif editing then
        -- (not now: the press brings this window to the front at the frame's end, over the /vana window opened by it)
        hm.edit = true
    end
end
