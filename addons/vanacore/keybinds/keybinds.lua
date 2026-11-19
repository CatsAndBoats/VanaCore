--[[
keybinds: VanaCore's key binds window. Loaded from its scripts\boot.txt line, as any addon is (taken out, it stays off).

In game:
  /keybinds           opens or closes the window
  /keybinds new       opens it on a new bind
  /keybinds close     closes it

Make a bind by clicking its key box and pressing the keys (Ctrl, Alt, Shift and the Windows key too), or by picking them,
then typing the command. The mouse works there too: the middle and side (back, forward) buttons and the wheel's tilt as
they are, left, right and the wheel's turns with a modifier held (a plain click or turn still works the window). A
gaming mouse's further buttons reach the game as the keys its own software gives them (F13-F24 clash with no keyboard),
and are pressed here as keys. Search, filter and sort the list; click a row to edit it, Copy to make another like it. A
bind that runs lights its row.

Changes apply at once and are saved to scripts\keybinds.txt: its /bind and /unbind lines run after boot.txt at every
start, so a bind boot.txt makes can be changed here too. Remove takes a bind's line out of the file and writes nothing
else; a bind boot.txt or an addon makes comes back at the next start, and its Unbind at start button (in the editor) saves
an /unbind line to keep it away. (/bind and /unbind in the chat work as ever; the window shows them, but only what it
changes is saved.)
--]]

addon.name    = 'keybinds'
addon.author  = 'Vekien'
addon.version = '1.0'
addon.desc    = 'A window for making, editing and removing key binds.'

local imgui = vana.imgui
local binds = vana.binds
if binds == nil or imgui == nil then
    print('keybinds needs a newer VanaCore.Core.dll (vana.binds)')
    return
end

-------------------------------------------------------------------------------
-- ImGui's numbers (1.92), and the look
-------------------------------------------------------------------------------

local COL = {
    Text = 0, TextDisabled = 1, WindowBg = 2, ChildBg = 3, PopupBg = 4, Border = 5, FrameBg = 7, FrameBgHovered = 8,
    FrameBgActive = 9, TitleBg = 10, TitleBgActive = 11, TitleBgCollapsed = 12, ScrollbarBg = 14, ScrollbarGrab = 15,
    ScrollbarGrabHovered = 16, ScrollbarGrabActive = 17, CheckMark = 18, Button = 21, ButtonHovered = 22, ButtonActive = 23,
    Header = 24, HeaderHovered = 25, HeaderActive = 26, Separator = 27, ResizeGrip = 30, ResizeGripHovered = 31,
    ResizeGripActive = 32,
}
local VAR = {
    WindowPadding = 2, WindowRounding = 3, WindowBorderSize = 4, ChildRounding = 7, PopupRounding = 9, FramePadding = 11,
    FrameRounding = 12, FrameBorderSize = 13, ItemSpacing = 14, ScrollbarSize = 18, ScrollbarRounding = 19, GrabRounding = 22,
}
-- NoScrollbar, NoScrollWithMouse, NoCollapse: the window lays itself out to fit, so it never scrolls
local WINDOW_FLAGS = 8 + 16 + 32
local FIRST_USE_EVER = 4

local P = {
    bg = '#12141A', panel = '#191C25', raised = '#242938', line = '#2E3445', text = '#E8EAF0', dim = '#8D93A6',
    accent = '#6D8DFF', accent_hi = '#8AA4FF', accent_lo = '#33449A', danger = '#F0707A', warn = '#EDB451', ok = '#62CE95',
    chip = '#2C3244', chip_text = '#D3D7E3', key_bg = '#3B4C95', key_text = '#FFFFFF', row = '#1E2230', hover = '#283047',
    well = '#0F1117',
}

local function rgba(hex, alpha)
    return {
        tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255, alpha or 1,
    }
end

-- an ImU32 (0xAABBGGRR) for the draw list
local function u32(hex, alpha)
    local function byte(v) return math.floor(math.max(0, math.min(1, v)) * 255 + 0.5) end
    local c = rgba(hex, alpha)
    return byte(c[4]) * 16777216 + byte(c[3]) * 65536 + byte(c[2]) * 256 + byte(c[1])
end

local THEME = {
    { COL.Text, P.text }, { COL.TextDisabled, P.dim }, { COL.WindowBg, P.bg }, { COL.ChildBg, P.panel },
    { COL.PopupBg, P.panel }, { COL.Border, P.line }, { COL.FrameBg, P.well }, { COL.FrameBgHovered, '#171A24' },
    { COL.FrameBgActive, '#1B1F2B' }, { COL.TitleBg, P.bg }, { COL.TitleBgActive, P.bg }, { COL.TitleBgCollapsed, P.bg },
    { COL.ScrollbarBg, P.panel }, { COL.ScrollbarGrab, '#3A4157' }, { COL.ScrollbarGrabHovered, '#4B5572' },
    { COL.ScrollbarGrabActive, P.accent }, { COL.CheckMark, P.accent }, { COL.Button, P.raised },
    { COL.ButtonHovered, '#2F3649' }, { COL.ButtonActive, '#39425B' }, { COL.Header, '#2A3350' },
    { COL.HeaderHovered, '#33406A' }, { COL.HeaderActive, '#3C4B80' }, { COL.Separator, P.line },
    { COL.ResizeGrip, P.line }, { COL.ResizeGripHovered, P.accent }, { COL.ResizeGripActive, P.accent_hi },
}

-- sizes at 1080 lines; the window follows the screen's height and [vana] scale, as the /vana window does
local scale = 1

local function S(n)
    return n * scale
end

-- ImGui's font is Agave at 18 px as in Ashita, whatever the screen: the sizes here are for it, and [keybinds] scale (a
-- percent, default 100) is for a screen it's too small on
local scaled_at = -1

local function compute_scale(now)
    if now - scaled_at < 1 then
        return
    end
    scaled_at = now
    scale = math.max(0.5, math.min(3, (tonumber(vana.config.get('keybinds', 'scale')) or 100) / 100))
end

-------------------------------------------------------------------------------
-- Keys and their names
-------------------------------------------------------------------------------

local MODS = {
    { 'ctrl', 'Ctrl', '^' }, { 'alt', 'Alt', '!' }, { 'shift', 'Shift', '+' }, { 'win', 'Win', '@' },
    { 'apps', 'Apps', '#' },
}

local PRETTY = {
    up = 'Up', down = 'Down', left = 'Left', right = 'Right', insert = 'Insert', delete = 'Delete', home = 'Home',
    ['end'] = 'End', pageup = 'Page Up', pagedown = 'Page Down', escape = 'Esc', enter = 'Enter', space = 'Space',
    tab = 'Tab', backspace = 'Backspace', capslock = 'Caps Lock', numlock = 'Num Lock', scrolllock = 'Scroll Lock',
    pause = 'Pause', apps = 'Menu', lwin = 'Left Win', rwin = 'Right Win', grave = '`', minus = '-', equals = '=',
    lbracket = '[', rbracket = ']', backslash = '\\', semicolon = ';', apostrophe = "'", comma = ',', period = '.',
    slash = '/', numpadenter = 'Num Enter', lbutton = 'Mouse Left', rbutton = 'Mouse Right', mbutton = 'Mouse Middle',
    xbutton1 = 'Mouse 4 (Back)', xbutton2 = 'Mouse 5 (Forward)', wheelup = 'Wheel Up', wheeldown = 'Wheel Down',
    wheelleft = 'Wheel Left', wheelright = 'Wheel Right',
}

local function pretty(key)
    local lower = key:lower()
    if PRETTY[lower] ~= nil then
        return PRETTY[lower]
    end
    local digit = lower:match('^numpad(%d)$')
    if digit ~= nil then
        return 'Num ' .. digit
    end
    if lower:match('^numpad.') then
        return 'Num ' .. lower:sub(7)
    end
    return key
end

local KEY_NAMES = binds.keys()
local KEY_ITEMS, KEY_POS = {}, {}
for i, name in ipairs(KEY_NAMES) do
    KEY_ITEMS[i] = pretty(name)
    KEY_POS[name:lower()] = i
end
local KEY_COMBO = '(choose a key)\0' .. table.concat(KEY_ITEMS, '\0') .. '\0'

local function modifier_words(entry)
    local words = {}
    for _, mod in ipairs(MODS) do
        if entry[mod[1]] then
            words[#words + 1] = mod[2]
        end
    end
    return words
end

local function trim(text)
    return (text or ''):match('^%s*(.-)%s*$')
end

-- ImGui's text is printf's format: a % in a bind's command is doubled
local function plain(text)
    return (text:gsub('%%', '%%%%'))
end

-- what a bind runs, as the chat line it becomes (binds.cpp's line_for)
local function line_for(command)
    if command:sub(1, 1) == '/' then
        return command
    end
    if command:lower():sub(1, 6) == 'input ' then
        return trim(command:sub(7))
    end
    return '//' .. command
end

-- "^!%f1": the key as /bind takes it, from an editor's choices
local function spec_of(ed)
    local spec = ''
    for _, mod in ipairs(MODS) do
        if ed[mod[1]] then
            spec = spec .. mod[3]
        end
    end
    if ed.when == 2 then
        spec = spec .. '%'
    elseif ed.when == 3 then
        spec = spec .. '$'
    end
    return spec .. (ed.key or '')
end

-------------------------------------------------------------------------------
-- What's saved: scripts\keybinds.txt, the changes made here, replayed after boot.txt (VanaCore's //keybinds apply)
-------------------------------------------------------------------------------

local FILE_DIR = vana.paths.install .. '\\scripts'
local FILE = FILE_DIR .. '\\keybinds.txt'
local EXPORT = FILE_DIR .. '\\binds-export.txt'

local saved = {} -- { id = 'up ^f6' or nil, text = '/bind up ^f6 ...' } in order
local save_error = nil

local MOD_FLAG = { ['^'] = 'c', ['!'] = 'a', ['@'] = 'w', ['#'] = 'p', ['+'] = 's', ['~'] = 's', ['%'] = 'x', ['$'] = 'o' }

-- A key's identity: the same for ^!f6 and !^f6 (the modifiers in any order, as /bind takes them), up or down
local function id_of(spec, up)
    local text = spec:lower()
    local flags, at = {}, 1
    while at < #text and MOD_FLAG[text:sub(at, at)] ~= nil do
        flags[#flags + 1] = MOD_FLAG[text:sub(at, at)]
        at = at + 1
    end
    table.sort(flags)
    return (up and 'up ' or '') .. table.concat(flags) .. ':' .. text:sub(at)
end

local function load_saved()
    saved = {}
    local file = io.open(FILE, 'rb')
    if file == nil then
        return
    end
    local text = file:read('*a') or ''
    file:close()
    text = text:gsub('^\239\187\191', '')
    for line in text:gmatch('[^\r\n]+') do
        line = trim(line)
        local op, rest = line:match('^//?(%a+)%s+(.*)$')
        op = op and op:lower() or nil
        if op == 'bind' or op == 'unbind' then
            local up = false
            local word, tail = rest:match('^(%S+)%s*(.*)$')
            for _ = 1, 2 do
                local flag = word and word:lower() or ''
                if flag == 'up' or flag == '-up' or flag == '-u' then
                    up = true
                elseif flag ~= 'down' and flag ~= '-down' and flag ~= '-d' then
                    break
                end
                word, tail = tail:match('^(%S+)%s*(.*)$')
            end
            saved[#saved + 1] = { id = word ~= nil and id_of(word, up) or nil, text = line, op = op }
        elseif line ~= '' and line:sub(1, 1) ~= '#' then
            saved[#saved + 1] = { text = line }
        end
    end
end

-- `op` 'bind' or 'unbind' for the key: what the file says about it from now on
local function drop(id)
    for i = #saved, 1, -1 do
        if saved[i].id == id then
            table.remove(saved, i)
        end
    end
end

local function record(op, spec, up, command)
    local id = id_of(spec, up)
    drop(id)
    saved[#saved + 1] = {
        id = id,
        op = op,
        text = '/' .. op .. ' ' .. (up and 'up ' or '') .. spec .. (command ~= nil and (' ' .. command) or ''),
    }
end

-- The keys boot.txt and the addons had bound when this file replayed, by id; nil until it has (then every key counts as one)
local function base_ids()
    local list = binds.base ~= nil and binds.base() or nil -- (a VanaCore without it: every key counts)
    if list == nil then
        return nil
    end
    local ids = {}
    for _, entry in ipairs(list) do
        ids[id_of(entry.spec, entry.up)] = true
    end
    return ids
end

-- Whether boot.txt or an addon bound this key (so it comes back at every start unless the file /unbinds it)
local function is_base(spec, up)
    local base = base_ids()
    return base ~= nil and base[id_of(spec, up)] == true
end

local function write_saved()
    -- /unbind lines for keys nothing else bound do nothing (earlier versions left one behind for every key removed here)
    local base = base_ids()
    if base ~= nil then
        for i = #saved, 1, -1 do
            if saved[i].op == 'unbind' and saved[i].id ~= nil and not base[saved[i].id] then
                table.remove(saved, i)
            end
        end
    end
    if not vana.fs.exists(FILE_DIR) then
        vana.fs.create_dir(FILE_DIR)
    end
    local file, why = io.open(FILE, 'wb')
    if file == nil then
        save_error = why or "the file couldn't be opened"
        return false
    end
    file:write('# Written by the keybinds window (/keybinds). It runs after boot.txt at every start: /bind lines make or change\n')
    file:write("# binds, /unbind lines take away ones boot.txt or an addon made. Edit it freely; the window keeps what's here.\n")
    for _, item in ipairs(saved) do
        file:write(item.text, '\n')
    end
    file:close()
    save_error = nil
    return true
end

-------------------------------------------------------------------------------
-- State
-------------------------------------------------------------------------------

local state = {
    open = false,
    entries = {},
    next_refresh = 0,
    search = { '' },
    filters = { ctrl = false, alt = false, shift = false, win = false, release = false, chat = false },
    sort = { 0 },
    ed = nil,
    toast = nil,
    view = { rows = {}, shown = 0 },
    dirty = true,
    reported = {},
}

local FILTERS = {
    { 'ctrl', 'Ctrl' }, { 'alt', 'Alt' }, { 'shift', 'Shift' }, { 'win', 'Win' }, { 'release', 'Release' },
    { 'chat', 'Chat' },
}

local function fail(where, err)
    local message = where .. ': ' .. tostring(err)
    if not state.reported[message] then
        state.reported[message] = true
        vana.log(message)
    end
end

local function mask_of(entry)
    return (entry.ctrl and 1 or 0) + (entry.alt and 2 or 0) + (entry.shift and 4 or 0) + (entry.win and 8 or 0)
        + (entry.apps and 16 or 0)
end

local function group_title(entry)
    local words = modifier_words(entry)
    return #words == 0 and 'No modifier' or table.concat(words, ' + ')
end

local function refresh()
    state.entries = binds.list()
    for i, entry in ipairs(state.entries) do
        entry.index = i
        entry.mask = mask_of(entry)
        local words = modifier_words(entry)
        entry.hay = (table.concat(words, ' ') .. ' ' .. entry.key .. ' ' .. pretty(entry.key) .. ' ' .. entry.spec .. ' '
            .. entry.command .. (entry.up and ' release up' or '') .. (entry.when ~= 'always' and ' chat ' .. entry.when or '')):lower()
    end
    state.dirty = true
end

local SORTS = {
    function(a, b) -- by key: modifiers, then the key
        if a.mask ~= b.mask then return a.mask < b.mask end
        if a.vk ~= b.vk then return a.vk < b.vk end
        if a.up ~= b.up then return not a.up end
        return a.index < b.index
    end,
    function(a, b) return a.index < b.index end, -- order made
    function(a, b) -- command
        local x, y = a.command:lower(), b.command:lower()
        if x ~= y then return x < y end
        return a.index < b.index
    end,
    function(a, b) -- recently used
        local x, y = a.fired_ago >= 0 and a.fired_ago or math.huge, b.fired_ago >= 0 and b.fired_ago or math.huge
        if x ~= y then return x < y end
        return a.index < b.index
    end,
}

local function rebuild()
    state.dirty = false
    local words = {}
    for word in state.search[1]:lower():gmatch('%S+') do
        words[#words + 1] = word
    end
    local shown = {}
    for _, entry in ipairs(state.entries) do
        local keep = true
        local f = state.filters
        if (f.ctrl and not entry.ctrl) or (f.alt and not entry.alt) or (f.shift and not entry.shift) or (f.win and not entry.win)
            or (f.release and not entry.up) or (f.chat and entry.when == 'always') then
            keep = false
        end
        for _, word in ipairs(words) do
            if keep and not entry.hay:find(word, 1, true) then
                keep = false
            end
        end
        if keep then
            shown[#shown + 1] = entry
        end
    end
    local sort = state.sort[1] + 1
    table.sort(shown, SORTS[sort] or SORTS[1])
    local rows = {}
    local previous, group = nil, nil
    for _, entry in ipairs(shown) do
        if sort == 1 and entry.mask ~= previous then
            previous = entry.mask
            group = { group = group_title(entry), count = 0 }
            rows[#rows + 1] = group
        end
        if sort == 1 then
            group.count = group.count + 1
        end
        rows[#rows + 1] = { entry = entry }
    end
    state.view = { rows = rows, shown = #shown }
end

local function toast(text, kind, undo)
    state.toast = { text = text, kind = kind or 'ok', at = vana.time.clock(), undo = undo }
end

-------------------------------------------------------------------------------
-- Making, changing and removing binds
-------------------------------------------------------------------------------

local function stop_capture()
    if state.ed ~= nil and state.ed.capturing then
        binds.capture_end()
        state.ed.capturing = false
    end
end

local function close_editor()
    stop_capture()
    state.ed = nil
end

local function start_capture()
    local ed = state.ed
    if ed ~= nil then
        binds.capture_begin()
        ed.capturing = true
        ed.capture_at = vana.time.clock()
    end
end

local function new_editor(from, capture)
    close_editor()
    local when = from and (from.when == 'closed' and 2 or from.when == 'open' and 3 or 1) or 1
    state.ed = {
        orig = nil, ctrl = from ~= nil and from.ctrl or false, alt = from ~= nil and from.alt or false,
        shift = from ~= nil and from.shift or false, win = from ~= nil and from.win or false,
        apps = from ~= nil and from.apps or false, key = nil, up = from and from.up or false, when = when, command = { from and from.command or '' }, capturing = false,
        key_index = { 0 }, focus = false,
    }
    if capture then
        start_capture()
    end
end

local function edit_entry(entry)
    close_editor()
    state.ed = {
        orig = entry, ctrl = entry.ctrl, alt = entry.alt, shift = entry.shift, win = entry.win, apps = entry.apps,
        key = entry.key, up = entry.up, when = entry.when == 'closed' and 2 or entry.when == 'open' and 3 or 1,
        command = { entry.command }, capturing = false, key_index = { 0 }, focus = false,
    }
end

local function label_of(entry)
    local words = modifier_words(entry)
    words[#words + 1] = pretty(entry.key)
    return table.concat(words, ' + ')
end

-- Take the bind away and its line out of scripts\keybinds.txt; no /unbind is written. A bind boot.txt or an addon made
-- comes back at the next start (unbind_at_start is for those).
local function remove_entry(entry, quiet)
    local again = is_base(entry.spec, entry.up)
    binds.remove(entry)
    drop(id_of(entry.spec, entry.up))
    local ok = write_saved()
    refresh()
    if not quiet then
        local text = 'Removed ' .. label_of(entry) .. (again and ' (boot.txt or an addon binds it again at start)' or '')
        toast(ok and text or ("Removed, but scripts\\keybinds.txt couldn't be written"), ok and 'ok' or 'bad', function()
            binds.add(entry.spec, entry.command, entry.up)
            record('bind', entry.spec, entry.up, entry.command)
            write_saved()
            refresh()
            toast('Brought back ' .. label_of(entry))
        end)
    end
end

-- For a bind boot.txt or an addon made: take it away now and save an /unbind line, so it stays away at every start
local function unbind_at_start(entry)
    binds.remove(entry)
    record('unbind', entry.spec, entry.up)
    local ok = write_saved()
    refresh()
    toast(ok and ('Unbound ' .. label_of(entry) .. ' at every start') or ("Unbound, but scripts\\keybinds.txt couldn't be written"), ok and 'ok' or 'bad', function()
        drop(id_of(entry.spec, entry.up))
        binds.add(entry.spec, entry.command, entry.up)
        write_saved()
        refresh()
        toast('Brought back ' .. label_of(entry))
    end)
end

-- what's wrong with the editor (problem: it can't be saved) or worth a word (warning)
local function check(ed)
    if ed.key == nil then
        return 'Choose a key: click the box and press it, or pick one below.'
    end
    if trim(ed.command[1]) == '' then
        return 'Type the command it runs.'
    end
    local spec = spec_of(ed):lower()
    for _, entry in ipairs(state.entries) do
        local same = entry.spec:lower() == spec and entry.up == ed.up
        local mine = ed.orig ~= nil and entry.spec:lower() == ed.orig.spec:lower() and entry.up == ed.orig.up
        if same and not mine then
            return nil, 'Already bound to ' .. entry.command .. ': saving replaces it.'
        end
    end
    return nil, nil
end

local function save_editor()
    local ed = state.ed
    if ed == nil or check(ed) ~= nil then
        return
    end
    local spec = spec_of(ed)
    local command = trim(ed.command[1])
    local old = ed.orig
    if old ~= nil and (old.spec:lower() ~= spec:lower() or old.up ~= ed.up) then
        binds.remove(old)
        drop(id_of(old.spec, old.up))
    end
    local ok, why = binds.add(spec, command, ed.up)
    if not ok then
        toast("Couldn't bind it: " .. tostring(why), 'bad')
        return
    end
    record('bind', spec, ed.up, command)
    local written = write_saved()
    close_editor()
    refresh()
    toast(written and ('Saved ' .. spec) or ("Bound, but scripts\\keybinds.txt couldn't be written"), written and 'ok' or 'bad')
end

local function export_all()
    local file, why = io.open(EXPORT, 'wb')
    if file == nil then
        toast("Couldn't write scripts\\binds-export.txt: " .. tostring(why), 'bad')
        return
    end
    file:write('# Every key bind at ' .. os.date('%Y-%m-%d %H:%M') .. ' (/exec binds-export runs them)\n')
    local entries = binds.list()
    for _, entry in ipairs(entries) do
        file:write('/bind ', entry.up and 'up ' or '', entry.spec, ' ', entry.command, '\n')
    end
    file:close()
    toast('Wrote ' .. #entries .. ' binds to scripts\\binds-export.txt')
end

-------------------------------------------------------------------------------
-- Drawing
-------------------------------------------------------------------------------

-- A child window whose end is always matched, whatever its drawing does
local function child(name, size, border, draw)
    if imgui.BeginChild(name, size, border, 0) then
        imgui.SetWindowFontScale(scale)
        local ok, err = pcall(draw)
        if not ok then
            fail('drawing ' .. name, err)
        end
    end
    imgui.EndChild()
end

local function push_colors(button, hover, active, text)
    imgui.PushStyleColor(COL.Button, rgba(button))
    imgui.PushStyleColor(COL.ButtonHovered, rgba(hover))
    imgui.PushStyleColor(COL.ButtonActive, rgba(active))
    imgui.PushStyleColor(COL.Text, rgba(text))
end

local function pop_colors()
    imgui.PopStyleColor(4)
end

local function accent_button(label, width)
    push_colors(P.accent, P.accent_hi, '#A3B7FF', '#0B1020')
    local pressed = imgui.Button(label, { width or 0, 0 })
    pop_colors()
    return pressed
end

local function plain_button(label, width)
    return imgui.Button(label, { width or 0, 0 })
end

local function danger_button(label, width)
    push_colors('#4A2A31', '#6A343D', '#85414B', '#FFD9DC')
    local pressed = imgui.Button(label, { width or 0, 0 })
    pop_colors()
    return pressed
end

local function toggle_chip(label, on, id)
    if on then
        push_colors(P.accent_lo, '#40529F', '#4E62B8', '#FFFFFF')
    else
        push_colors(P.raised, '#2F3649', '#39425B', P.dim)
    end
    local pressed = imgui.Button(label .. '##' .. id)
    pop_colors()
    return pressed
end

local function caption(text)
    imgui.TextColored(rgba(P.dim), text)
end

local function line_height()
    return imgui.GetTextLineHeight()
end

-- a rounded label at x, y with the text inside; returns the width it took
local function chip(dl, x, y, height, text, background, foreground)
    local width = (imgui.CalcTextSize(text)) + S(14)
    dl:AddRectFilled({ x, y }, { x + width, y + height }, u32(background), S(5))
    dl:AddText({ x + S(7), y + (height - line_height()) / 2 }, u32(foreground), text)
    return width
end

-- the key combination as keycaps, from x; returns the width it took
local function draw_combo(dl, x, y, height, entry, key)
    local at = x
    for _, mod in ipairs(MODS) do
        if entry[mod[1]] then
            at = at + chip(dl, at, y, height, mod[2], P.chip, P.chip_text) + S(4)
        end
    end
    if key ~= nil then
        at = at + chip(dl, at, y, height, pretty(key), P.key_bg, P.key_text) + S(4)
    end
    return at - x
end

local function tags_of(entry)
    local tags = {}
    if entry.up then
        tags[#tags + 1] = 'on release'
    end
    if entry.when == 'closed' then
        tags[#tags + 1] = 'chat closed'
    elseif entry.when == 'open' then
        tags[#tags + 1] = 'chat open'
    end
    return tags
end

local ACTIONS = { 'Edit', 'Copy', 'Remove' }

local function draw_group(row)
    local dl = imgui.GetWindowDrawList()
    local x, y = imgui.GetCursorScreenPos()
    local width = (imgui.GetContentRegionAvail())
    local height = S(26)
    imgui.Dummy({ width, height })
    local text = row.group:upper()
    local text_y = y + height - line_height() - S(4)
    dl:AddText({ x + S(6), text_y }, u32(P.accent), text)
    local after = x + S(6) + (imgui.CalcTextSize(text)) + S(8)
    dl:AddText({ after, text_y }, u32(P.dim), tostring(row.count))
    local line_x = after + (imgui.CalcTextSize(tostring(row.count))) + S(10)
    dl:AddLine({ line_x, text_y + line_height() / 2 }, { x + width - S(6), text_y + line_height() / 2 }, u32(P.line), 1)
end

-- one bind's row; returns what was asked of it: 'edit', 'copy', 'remove' or nil
local function draw_row(entry, n)
    local dl = imgui.GetWindowDrawList()
    local x, y = imgui.GetCursorScreenPos()
    local width = (imgui.GetContentRegionAvail())
    local height = S(36)
    imgui.InvisibleButton('##row' .. n, { width, height })
    local hovered = imgui.IsItemHovered()
    local clicked = imgui.IsItemClicked(0)
    local mouse_x = imgui.GetMousePos()

    local zone_w = S(68)
    local zones_x = x + width - zone_w * #ACTIONS - S(6)
    local zone = nil
    if hovered then
        zone = math.floor((mouse_x - zones_x) / zone_w)
        if zone < 0 or zone >= #ACTIONS then
            zone = nil
        end
    end

    local being_edited = state.ed ~= nil and state.ed.orig
    local selected = being_edited and being_edited.spec == entry.spec and being_edited.up == entry.up
    dl:AddRectFilled({ x, y }, { x + width, y + height }, u32(hovered and P.hover or P.row, selected and 1 or 0.9), S(6))
    if selected then
        dl:AddRect({ x, y }, { x + width, y + height }, u32(P.accent), S(6), 15, 1.5)
    end
    local fired = entry.fired_ago
    if fired >= 0 and fired < 1.5 then
        local t = 1 - fired / 1.5
        dl:AddRectFilled({ x, y }, { x + width, y + height }, u32(P.accent, 0.38 * t), S(6))
        dl:AddRectFilled({ x, y + S(5) }, { x + S(3), y + height - S(5) }, u32(P.accent_hi, t), S(2))
    end

    local chip_h = S(22)
    local chip_y = y + (height - chip_h) / 2
    local used = draw_combo(dl, x + S(12), chip_y, chip_h, entry, entry.key)
    local tags = tags_of(entry)
    local tag_x = x + S(12) + used + S(4)
    for _, tag in ipairs(tags) do
        local tw = (imgui.CalcTextSize(tag))
        dl:AddText({ tag_x, y + (height - line_height()) / 2 }, u32(P.warn, 0.85), tag)
        tag_x = tag_x + tw + S(10)
    end

    local command_x = x + S(300)
    local command_end = hovered and zones_x - S(8) or x + width - S(10)
    dl:PushClipRect({ command_x, y }, { command_end, y + height }, true)
    dl:AddText({ command_x, y + (height - line_height()) / 2 }, u32('#C9D4FF'), entry.command)
    dl:PopClipRect()

    if hovered then
        for i, name in ipairs(ACTIONS) do
            local zx = zones_x + (i - 1) * zone_w
            local on = zone == i - 1
            local remove = i == #ACTIONS
            if on then
                dl:AddRectFilled({ zx + S(2), y + S(6) }, { zx + zone_w - S(2), y + height - S(6) },
                    u32(remove and '#6A343D' or '#3A4666'), S(5))
            end
            local tw = (imgui.CalcTextSize(name))
            dl:AddText({ zx + (zone_w - tw) / 2, y + (height - line_height()) / 2 },
                u32(on and (remove and '#FFD9DC' or '#FFFFFF') or P.dim), name)
        end
        if zone == nil and mouse_x > command_x - S(4) then
            imgui.SetTooltip(plain(entry.command) .. '\n' .. (entry.up and 'Runs when the key is released' or 'Runs when the key is pressed')
                .. (entry.when == 'closed' and '\nOnly while the chat line is closed' or entry.when == 'open' and '\nOnly while the chat line is open' or ''))
        end
    end

    if clicked then
        return zone == 1 and 'copy' or zone == 2 and 'remove' or 'edit'
    end
    return nil
end

local function draw_list()
    local view = state.view
    if #state.entries == 0 then
        imgui.Dummy({ 0, S(40) })
        imgui.TextColored(rgba(P.dim), 'No key binds yet.')
        imgui.TextColored(rgba(P.dim), 'Click New bind, or use /bind in the chat. Binds from boot.txt show here too.')
        return
    end
    if view.shown == 0 then
        imgui.Dummy({ 0, S(40) })
        imgui.TextColored(rgba(P.dim), 'No bind matches.')
        return
    end
    imgui.PushStyleVar(VAR.ItemSpacing, { S(8), S(3) })
    local action, target = nil, nil
    local n = 0
    for _, row in ipairs(view.rows) do
        if row.group ~= nil then
            draw_group(row)
        else
            n = n + 1
            local asked = draw_row(row.entry, n)
            if asked ~= nil then
                action, target = asked, row.entry
            end
        end
    end
    imgui.PopStyleVar(1)
    if action == 'edit' then
        edit_entry(target)
    elseif action == 'copy' then
        new_editor(target, true)
    elseif action == 'remove' then
        remove_entry(target)
    end
end

-- the "click, then press the keys" box; returns nothing, changes the editor
local function draw_capture(ed)
    local x, y = imgui.GetCursorScreenPos()
    local width = math.min((imgui.GetContentRegionAvail()) - S(90), S(460))
    local height = S(42)
    imgui.InvisibleButton('##capture', { width, height })
    local hovered = imgui.IsItemHovered()
    if imgui.IsItemClicked(0) then
        if ed.capturing then
            stop_capture()
        else
            start_capture()
        end
    end

    local live = {}
    if ed.capturing then
        live = binds.capture()
        if live.done or not live.active or vana.time.clock() - ed.capture_at > 20 then
            binds.capture_end()
            ed.capturing = false
            if live.done and not live.cancelled and live.key ~= nil then
                for _, mod in ipairs(MODS) do
                    ed[mod[1]] = live[mod[1]] == true
                end
                ed.key = live.key
                ed.focus = true
            end
        end
    end

    local dl = imgui.GetWindowDrawList()
    local border = ed.capturing and P.accent or (hovered and '#56608A' or P.line)
    dl:AddRectFilled({ x, y }, { x + width, y + height }, u32(P.well), S(8))
    dl:AddRect({ x, y }, { x + width, y + height }, u32(border), S(8), 15, ed.capturing and 2 or 1)
    local chip_h = S(26)
    local chip_y = y + (height - chip_h) / 2
    if ed.capturing then
        local used = draw_combo(dl, x + S(12), chip_y, chip_h, live, nil)
        local pulse = 0.55 + 0.45 * math.abs(math.sin(vana.time.clock() * 3))
        dl:AddText({ x + S(12) + used + S(4), y + (height - line_height()) / 2 }, u32(P.accent_hi, pulse),
            used > 0 and 'now press the key, button or wheel...' or 'Press the keys or a mouse button...')
        local hint = 'Esc cancels'
        local tw = (imgui.CalcTextSize(hint))
        dl:AddText({ x + width - tw - S(12), y + (height - line_height()) / 2 }, u32(P.dim), hint)
    elseif ed.key ~= nil then
        draw_combo(dl, x + S(12), chip_y, chip_h, ed, ed.key)
        local hint = 'Click to press another'
        local tw = (imgui.CalcTextSize(hint))
        dl:AddText({ x + width - tw - S(12), y + (height - line_height()) / 2 }, u32(P.dim, hovered and 1 or 0.6), hint)
    else
        dl:AddText({ x + S(14), y + (height - line_height()) / 2 }, u32(P.dim), 'Click here, then press the keys or a mouse button')
    end
    if hovered and not ed.capturing then
        imgui.SetTooltip('Ctrl, Alt, Shift and the Windows key count too.\n'
            .. 'The mouse: the middle and side (back, forward) buttons and the wheel tilted left or right as they are;\n'
            .. 'left, right and the wheel turned up or down with Ctrl, Alt or Shift held (plain, they still work this window).\n'
            .. "A gaming mouse's further buttons: give them keys in the mouse's own software (F13-F24 clash with no\n"
            .. 'keyboard), then press them here.')
    end
    imgui.SameLine()
    if plain_button('Clear', S(70)) then
        stop_capture()
        ed.key, ed.ctrl, ed.alt, ed.shift, ed.win, ed.apps = nil, false, false, false, false, false
    end
end

local EXAMPLES = {
    { 'Spell', '/ma "Cure" <t>' }, { 'Ability', '/ja "Provoke" <t>' }, { 'Weapon skill', '/ws "Savage Blade" <t>' },
    { 'Item', '/item "Hi-Potion" <me>' }, { 'Target', '/input /ta <a10>' }, { 'Equip set', '/equipset 1' },
}

local function editor_height(has_note)
    local frame = line_height() + S(12)
    local gap = S(8)
    -- title, caption, capture box, pick row, options row, caption, command box, examples row, [note], buttons
    local items = { line_height(), line_height(), S(42), frame, frame, line_height(), frame, frame, frame }
    if has_note then
        items[#items + 1] = line_height()
    end
    local total = S(28) + gap * #items
    for _, h in ipairs(items) do
        total = total + h
    end
    return total + S(10)
end

local function draw_editor()
    local ed = state.ed
    local problem, note = check(ed)
    child('##editor', { 0, editor_height(problem ~= nil or note ~= nil) }, true, function()
        imgui.TextColored(rgba(P.accent_hi), ed.orig ~= nil and 'Edit bind' or 'New bind')

        caption('KEY')
        draw_capture(ed)

        -- the same choices, picked
        for _, mod in ipairs(MODS) do
            if toggle_chip(mod[2], ed[mod[1]], 'm' .. mod[1]) then
                ed[mod[1]] = not ed[mod[1]]
            end
            imgui.SameLine()
        end
        ed.key_index[1] = KEY_POS[(ed.key or ''):lower()] or 0
        imgui.SetNextItemWidth(S(190))
        if imgui.Combo('##key', ed.key_index, KEY_COMBO, 16) then
            ed.key = ed.key_index[1] > 0 and KEY_NAMES[ed.key_index[1]] or nil
        end

        imgui.AlignTextToFramePadding()
        caption('RUNS')
        imgui.SameLine()
        if toggle_chip('When pressed', not ed.up, 'press') then ed.up = false end
        imgui.SameLine()
        if toggle_chip('When released', ed.up, 'release') then ed.up = true end
        imgui.SameLine(0, S(28))
        imgui.AlignTextToFramePadding()
        caption('WHILE')
        for i, name in ipairs({ 'Always', 'Chat closed', 'Chat open' }) do
            imgui.SameLine()
            if toggle_chip(name, ed.when == i, 'when' .. i) then ed.when = i end
        end

        caption('COMMAND')
        imgui.SetNextItemWidth(-S(2))
        if ed.focus then
            imgui.SetKeyboardFocusHere(0)
            ed.focus = false
        end
        -- EnterReturnsTrue
        if imgui.InputTextWithHint('##command', '/ma "Cure" <t>      //addon reload name      or any chat command', ed.command, 512, 64) then
            save_editor()
        end
        imgui.AlignTextToFramePadding()
        caption('TRY')
        for i, example in ipairs(EXAMPLES) do
            imgui.SameLine()
            if plain_button(example[1] .. '##ex' .. i) then
                ed.command[1] = example[2]
                ed.focus = true
            end
        end

        if problem ~= nil then
            imgui.TextColored(rgba(P.dim), plain(problem))
        elseif note ~= nil then
            imgui.TextColored(rgba(P.warn), plain(note))
        end

        local ready = problem == nil
        if ready then
            if accent_button(ed.orig ~= nil and 'Save changes' or 'Save bind', S(130)) then
                save_editor()
            end
        else
            imgui.PushStyleVar(0, 0.4)
            plain_button(ed.orig ~= nil and 'Save changes' or 'Save bind', S(130))
            imgui.PopStyleVar(1)
        end
        imgui.SameLine()
        if plain_button('Test', S(70)) and trim(ed.command[1]) ~= '' then
            vana.chat.run(line_for(trim(ed.command[1])))
        end
        if imgui.IsItemHovered() then
            imgui.SetTooltip('Runs the command now, as the key would')
        end
        imgui.SameLine()
        if plain_button('Cancel', S(80)) then
            close_editor()
        end
        if ed.orig ~= nil and state.ed ~= nil then
            if is_base(ed.orig.spec, ed.orig.up) then
                imgui.SameLine((imgui.GetWindowWidth()) - S(16) - S(112) - S(8) - S(120))
                if plain_button('Unbind at start', S(120)) then
                    local entry = ed.orig
                    close_editor()
                    unbind_at_start(entry)
                end
                if imgui.IsItemHovered() then
                    imgui.SetTooltip('boot.txt or an addon binds this key at every start: this takes it away now and saves an /unbind line so it stays away')
                end
            end
            imgui.SameLine((imgui.GetWindowWidth()) - S(16) - S(112))
            if danger_button('Remove bind', S(112)) then
                local entry = ed.orig
                close_editor()
                remove_entry(entry)
            end
        end
    end)
end

local function draw_toolbar()
    imgui.SetNextItemWidth(S(210))
    if imgui.InputTextWithHint('##search', 'Search...', state.search, 128) then
        state.dirty = true
    end
    for _, filter in ipairs(FILTERS) do
        imgui.SameLine()
        if toggle_chip(filter[2], state.filters[filter[1]], 'f' .. filter[1]) then
            state.filters[filter[1]] = not state.filters[filter[1]]
            state.dirty = true
        end
    end
    imgui.SameLine()
    imgui.SetNextItemWidth(S(150))
    if imgui.Combo('##sort', state.sort, 'By key\0Order made\0Command\0Recently used\0', 6) then
        state.dirty = true
    end
end

local function draw_footer()
    local now = vana.time.clock()
    local note = state.toast
    if note ~= nil and now - note.at > 10 then
        state.toast = nil
        note = nil
    end
    imgui.AlignTextToFramePadding()
    if note ~= nil then
        imgui.TextColored(rgba(note.kind == 'bad' and P.danger or P.ok), plain(note.text))
        if note.undo ~= nil then
            imgui.SameLine()
            if imgui.SmallButton('Undo') then
                local undo = note.undo
                state.toast = nil
                undo()
            end
        end
    elseif save_error ~= nil then
        imgui.TextColored(rgba(P.danger), plain("Couldn't write scripts\\keybinds.txt: " .. save_error))
    else
        imgui.TextColored(rgba(P.dim), 'Changes apply now and are saved to scripts\\keybinds.txt')
    end
    imgui.SameLine((imgui.GetWindowWidth()) - S(16) - S(100))
    if plain_button('Export all', S(100)) then
        export_all()
    end
    if imgui.IsItemHovered() then
        imgui.SetTooltip('Writes every bind as /bind lines to scripts\\binds-export.txt')
    end
end

local function draw_contents()
    imgui.AlignTextToFramePadding()
    imgui.TextColored(rgba(P.dim), string.format('%d bind%s%s', #state.entries, #state.entries == 1 and '' or 's',
        state.view.shown ~= #state.entries and (', ' .. state.view.shown .. ' shown') or ''))
    imgui.SameLine((imgui.GetWindowWidth()) - S(16) - S(120))
    if accent_button('+  New bind', S(120)) then
        new_editor(nil, true)
    end
    imgui.Spacing()
    draw_toolbar()
    imgui.Spacing()
    if state.ed ~= nil then
        draw_editor()
    end
    local _, room = imgui.GetContentRegionAvail()
    -- room for the footer: the gap above it, then a button's height
    local footer = S(8) + line_height() + S(12)
    child('##list', { 0, math.max(S(120), room - footer) }, false, draw_list)
    draw_footer()
end

local function draw()
    local now = vana.time.clock()
    if now >= state.next_refresh then
        state.next_refresh = now + 0.12
        refresh()
    end
    if state.dirty then
        -- a failure here keeps the last list, and doesn't skip the frame (a window drawn every other frame flickers)
        local ok, err = pcall(rebuild)
        if not ok then
            fail('building the list', err)
        end
    end

    local screen_w, screen_h = vana.ui.screen()
    screen_w, screen_h = screen_w or 1920, screen_h or 1080
    compute_scale(now)
    imgui.SetNextWindowPos({ screen_w / 2, screen_h / 2 }, FIRST_USE_EVER, { 0.5, 0.5 })
    imgui.SetNextWindowSize({ math.min(S(920), screen_w - 40), math.min(S(620), screen_h - 40) }, FIRST_USE_EVER)

    local pushed_colors = 0
    for _, entry in ipairs(THEME) do
        imgui.PushStyleColor(entry[1], rgba(entry[2]))
        pushed_colors = pushed_colors + 1
    end
    imgui.PushStyleVar(VAR.WindowPadding, { S(16), S(14) })
    imgui.PushStyleVar(VAR.WindowRounding, S(10))
    imgui.PushStyleVar(VAR.WindowBorderSize, 1)
    imgui.PushStyleVar(VAR.ChildRounding, S(8))
    imgui.PushStyleVar(VAR.PopupRounding, S(8))
    imgui.PushStyleVar(VAR.FramePadding, { S(10), S(6) })
    imgui.PushStyleVar(VAR.FrameRounding, S(6))
    imgui.PushStyleVar(VAR.FrameBorderSize, 0)
    imgui.PushStyleVar(VAR.ItemSpacing, { S(8), S(8) })
    imgui.PushStyleVar(VAR.ScrollbarSize, S(12))
    imgui.PushStyleVar(VAR.ScrollbarRounding, S(8))
    imgui.PushStyleVar(VAR.GrabRounding, S(6))

    local open = { true }
    if imgui.Begin('Key binds###vana_keybinds', open, WINDOW_FLAGS) then
        imgui.SetWindowFontScale(scale)
        local ok, err = pcall(draw_contents)
        if not ok then
            fail('drawing the window', err)
        end
    end
    imgui.End()
    imgui.PopStyleVar(12)
    imgui.PopStyleColor(pushed_colors)

    if not open[1] then
        state.open = false
        close_editor()
    end
end

-------------------------------------------------------------------------------
-- /keybinds
-------------------------------------------------------------------------------

local function open_window()
    state.open = true
    state.next_refresh = 0
end

local function close_window()
    state.open = false
    close_editor()
end

vana.events.on('load', function ()
    load_saved()
end)

vana.events.on('present', function ()
    if state.open then
        local ok, err = pcall(draw)
        if not ok then
            fail('present', err)
        end
    end
end)

vana.events.on('command', function (e)
    local words = {}
    for word in e.command:gmatch('%S+') do
        words[#words + 1] = word
    end
    local command = words[1] and words[1]:lower() or nil
    if command ~= '//keybinds' and command ~= '/keybinds' then
        return false
    end
    local what = words[2] and words[2]:lower() or nil
    if what == 'close' then
        close_window()
    elseif what == 'new' then
        open_window()
        refresh()
        new_editor(nil, false)
    elseif what == nil then
        if state.open then close_window() else open_window() end
    else
        print(command .. ' [new|close]: the key binds window')
    end
    return true
end)

vana.events.on('unload', function ()
    close_editor()
end)
