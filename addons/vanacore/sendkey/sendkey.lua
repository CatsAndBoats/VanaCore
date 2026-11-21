--[[
sendkey: binds a controller button or a key to a key that Windows sends to everyone, not just the game.

For programs that listen to the keyboard on their own (Discord's push to talk, OBS, a macro tool) but can't see the
controller. The game sends the key with SendInput as the button goes down, and lets it up as the button comes up, so
holding the button holds the key. The key never reaches the game itself.

  /sendkey bind <from> <to> [block]   /sendkey bind pad:rb f13       R1 (RB) holds F13
                                      /sendkey bind f9 ctrl+shift+m   F9 sends Ctrl+Shift+M
  /sendkey learn <to> [block]         then press the button or key to bind
  /sendkey unbind <from>              takes one off
  /sendkey list                       shows the binds
  /sendkey clear                      takes all off
  /sendkey test <to>                  sends a key for a moment, to try what listens for it
  /sendkey names                      lists the button and key names

<from>   pad:<button>   a controller's XInput button: a b x y lb rb lt rt start back ls rs up down left right
                        (PlayStation names work too: cross circle square triangle l1 r1 l2 r2 l3 r3 options share)
         joy:<n>        a DirectInput controller's button n (0 is the first), for pads that aren't XInput ones
         <key>          a keyboard key, with ctrl+ shift+ alt+ held: f9, ctrl+f9, numpad5
<to>     a key, with modifiers: f13, ctrl+shift+m, volumemute
block    the game doesn't see <from> either (without it, the button or key still does its usual job in the game)

Choose a <to> the game doesn't use: f13 to f24 are good, as most keyboards have none. Binds are saved in
config\addons\sendkey\binds.txt and come back with the addon, so put /addon load sendkey in scripts\boot.txt.
--]]

addon.name    = 'sendkey'
addon.author  = 'Vekien'
addon.version = '1.0'
addon.desc    = 'Binds controller buttons and keys to keys sent to other programs (Discord push to talk).'

local ffi = require('ffi')
local bit = require('bit')

pcall(ffi.cdef, [[
typedef struct {
    uint32_t type;
    union {
        struct { int32_t dx, dy; uint32_t mouseData, dwFlags, time; uintptr_t dwExtraInfo; } mi;
        struct { uint16_t wVk, wScan; uint32_t dwFlags, time; uintptr_t dwExtraInfo; } ki;
    };
} SENDKEY_INPUT;
unsigned int SendInput(unsigned int count, SENDKEY_INPUT* inputs, int size);
unsigned int MapVirtualKeyA(unsigned int code, unsigned int map_type);
short GetAsyncKeyState(int vk);
]])

local INPUT_KEYBOARD = 1
local KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP = 0x1, 0x2
local SENT_BY_US = 0x53454E44 -- dwExtraInfo of what this sends
local TRIGGER_DOWN = 30      -- XInput's trigger threshold: a trigger this far in is a press
local OURS_FOR = 0.75        -- seconds a key it sent is still taken as its own when it comes back as a window message

local FILE_DIR = vana.paths.install .. '\\config\\addons\\sendkey'
local FILE = FILE_DIR .. '\\binds.txt'

-------------------------------------------------------------------------------
-- Names
-------------------------------------------------------------------------------

local MODIFIERS = { ctrl = 0x11, control = 0x11, shift = 0x10, alt = 0x12, win = 0x5B }
local MODIFIER_ORDER = { 'ctrl', 'shift', 'alt', 'win' }
local MODIFIER_VK = { ctrl = 0x11, shift = 0x10, alt = 0x12, win = 0x5B }

local KEYS = {
    space = 0x20, enter = 0x0D, ['return'] = 0x0D, tab = 0x09, esc = 0x1B, escape = 0x1B, backspace = 0x08,
    insert = 0x2D, delete = 0x2E, home = 0x24, ['end'] = 0x23, pageup = 0x21, pagedown = 0x22,
    up = 0x26, down = 0x28, left = 0x25, right = 0x27,
    capslock = 0x14, scrolllock = 0x91, numlock = 0x90, pause = 0x13, printscreen = 0x2C, apps = 0x5D,
    numpadmultiply = 0x6A, numpadplus = 0x6B, numpadminus = 0x6D, numpaddecimal = 0x6E, numpaddivide = 0x6F,
    minus = 0xBD, equals = 0xBB, comma = 0xBC, period = 0xBE, slash = 0xBF, semicolon = 0xBA, quote = 0xDE,
    backtick = 0xC0, grave = 0xC0, lbracket = 0xDB, rbracket = 0xDD, backslash = 0xDC,
    volumeup = 0xAF, volumedown = 0xAE, volumemute = 0xAD, mute = 0xAD,
    playpause = 0xB3, nexttrack = 0xB0, prevtrack = 0xB1, stopmedia = 0xB2,
}
for i = 0, 9 do
    KEYS[tostring(i)] = 0x30 + i
    KEYS['numpad' .. i] = 0x60 + i
end
for i = 0, 25 do
    KEYS[string.char(97 + i)] = 0x41 + i
end
for i = 1, 24 do
    KEYS['f' .. i] = 0x6F + i
end

local KEY_NAMES = {} -- a virtual-key code's name (the first name wins in the order below)
do
    local names = {}
    for name in pairs(KEYS) do
        names[#names + 1] = name
    end
    table.sort(names, function (a, b) return #a < #b or (#a == #b and a < b) end)
    for _, name in ipairs(names) do
        if KEY_NAMES[KEYS[name]] == nil then
            KEY_NAMES[KEYS[name]] = name
        end
    end
end

-- Ashita's numbers for XInput's controls, by name (the first name of a control is the one it's shown by)
local PAD = {
    { 0, 'up' }, { 1, 'down' }, { 2, 'left' }, { 3, 'right' }, { 4, 'start', 'options' }, { 5, 'back', 'share', 'select' },
    { 6, 'ls', 'l3' }, { 7, 'rs', 'r3' }, { 8, 'lb', 'l1' }, { 9, 'rb', 'r1' },
    { 12, 'a', 'cross' }, { 13, 'b', 'circle' }, { 14, 'x', 'square' }, { 15, 'y', 'triangle' },
    { 16, 'lt', 'l2' }, { 17, 'rt', 'r2' },
}
local PAD_CODE, PAD_NAME = {}, {}
for _, row in ipairs(PAD) do
    PAD_NAME[row[1]] = row[2]
    for i = 2, #row do
        PAD_CODE[row[i]] = row[1]
    end
end

local DIJOFS_BUTTON0 = 48

-------------------------------------------------------------------------------
-- Specs
-------------------------------------------------------------------------------

local function trim(text)
    return (text:gsub('^%s+', ''):gsub('%s+$', ''))
end

-- 'ctrl+shift+f13' -> { mods = { 'ctrl', 'shift' }, vk = 0x7C, text = 'ctrl+shift+f13' }, or nil and why
local function parse_key(spec)
    local mods, seen, vk = {}, {}, nil
    local parts = {}
    for part in (spec:lower() .. '+'):gmatch('([^+]*)%+') do
        parts[#parts + 1] = trim(part)
    end
    for i, part in ipairs(parts) do
        if part == '' then
            return nil, "'" .. spec .. "' has an empty part"
        elseif i < #parts then
            local mod = MODIFIERS[part]
            if mod == nil then
                return nil, "'" .. part .. "' isn't a modifier (ctrl, shift, alt, win)"
            end
            local name = part == 'control' and 'ctrl' or part
            if not seen[name] then
                seen[name] = true
            end
        else
            vk = KEYS[part]
            if vk == nil then
                return nil, "'" .. part .. "' isn't a key name (/sendkey names lists them)"
            end
        end
    end
    for _, name in ipairs(MODIFIER_ORDER) do
        if seen[name] then
            mods[#mods + 1] = name
        end
    end
    local text = #mods > 0 and (table.concat(mods, '+') .. '+' .. KEY_NAMES[vk]) or KEY_NAMES[vk]
    return { mods = mods, vk = vk, text = text }
end

-- 'pad:rb', 'joy:3' or a key -> { kind = 'pad' | 'joy' | 'key', code =, key =, id = 'pad:rb' }, or nil and why
local function parse_source(spec)
    spec = trim(spec):lower()
    local pad = spec:match('^pad:(%w+)$')
    if pad ~= nil then
        local code = PAD_CODE[pad]
        if code == nil then
            return nil, "'" .. pad .. "' isn't a controller button (/sendkey names lists them)"
        end
        return { kind = 'pad', code = code, id = 'pad:' .. PAD_NAME[code] }
    end
    local joy = spec:match('^joy:(%d+)$')
    if joy ~= nil then
        local n = tonumber(joy)
        if n > 127 then
            return nil, 'a DirectInput controller has buttons 0 to 127'
        end
        return { kind = 'joy', code = DIJOFS_BUTTON0 + n, id = 'joy:' .. n }
    end
    if spec:find('^pad:') or spec:find('^joy:') then
        return nil, "'" .. spec .. "' isn't a button (pad:rb, joy:3)"
    end
    local key, why = parse_key(spec)
    if key == nil then
        return nil, why
    end
    return { kind = 'key', key = key, id = key.text }
end

-------------------------------------------------------------------------------
-- Sending
-------------------------------------------------------------------------------

local EXTENDED = { [0x21] = true, [0x22] = true, [0x23] = true, [0x24] = true, [0x25] = true, [0x26] = true,
                   [0x27] = true, [0x28] = true, [0x2C] = true, [0x2D] = true, [0x2E] = true, [0x5B] = true,
                   [0x5D] = true, [0x6F] = true, [0x90] = true }

local ours = {} -- vk -> clock until which a window message for it is this addon's own

local function send(vk, down)
    local input = ffi.new('SENDKEY_INPUT[1]')
    input[0].type = INPUT_KEYBOARD
    input[0].ki.wVk = vk
    input[0].ki.wScan = ffi.C.MapVirtualKeyA(vk, 0)
    input[0].ki.dwFlags = (EXTENDED[vk] and KEYEVENTF_EXTENDEDKEY or 0) + (down and 0 or KEYEVENTF_KEYUP)
    input[0].ki.dwExtraInfo = SENT_BY_US
    ours[vk] = vana.time.clock()
    return ffi.C.SendInput(1, input, ffi.sizeof('SENDKEY_INPUT')) == 1
end

-- The target's keys in the order they go down: modifiers, then the key; up in the reverse
local function press(target, down)
    local ok = true
    if down then
        for _, name in ipairs(target.mods) do
            ok = send(MODIFIER_VK[name], true) and ok
        end
        ok = send(target.vk, true) and ok
    else
        ok = send(target.vk, false) and ok
        for i = #target.mods, 1, -1 do
            ok = send(MODIFIER_VK[target.mods[i]], false) and ok
        end
    end
    return ok
end

-- True for a key this addon just sent: it comes back as the game's window message and isn't anyone's to act on
local function is_ours(vk)
    local until_ = ours[vk]
    return until_ ~= nil and vana.time.clock() - until_ < OURS_FOR
end

-------------------------------------------------------------------------------
-- Binds
-------------------------------------------------------------------------------

local binds = {}  -- id -> { source =, target =, block =, held = }
local order = {}  -- ids in the order they were made
local learning    -- { target =, block = } while /sendkey learn waits for a press

local function release_all()
    for _, bind in pairs(binds) do
        if bind.held then
            bind.held = false
            press(bind.target, false)
        end
    end
end

local function describe(bind)
    return bind.source.id .. ' -> ' .. bind.target.text .. (bind.block and ' (block)' or '')
end

local function save()
    pcall(vana.fs.create_dir, vana.paths.install .. '\\config')
    pcall(vana.fs.create_dir, vana.paths.install .. '\\config\\addons')
    pcall(vana.fs.create_dir, FILE_DIR)
    local file = io.open(FILE, 'wb')
    if file == nil then
        return false
    end
    file:write('# sendkey: <from> <to> [block], one bind a line (/sendkey bind ...)\r\n')
    for _, id in ipairs(order) do
        local bind = binds[id]
        file:write(bind.source.id, ' ', bind.target.text, bind.block and ' block' or '', '\r\n')
    end
    file:close()
    return true
end

local function add(source, target, block)
    local old = binds[source.id]
    if old ~= nil and old.held then
        press(old.target, false)
    end
    if old == nil then
        order[#order + 1] = source.id
    end
    binds[source.id] = { source = source, target = target, block = block, held = false }
    return binds[source.id], old
end

local function remove(id)
    local bind = binds[id]
    if bind == nil then
        return false
    end
    if bind.held then
        press(bind.target, false)
    end
    binds[id] = nil
    for i, other in ipairs(order) do
        if other == id then
            table.remove(order, i)
            break
        end
    end
    return true
end

local function load()
    local file = io.open(FILE, 'rb')
    if file == nil then
        return
    end
    local text = (file:read('*a') or ''):gsub('^\239\187\191', '')
    file:close()
    for raw in text:gmatch('[^\r\n]+') do
        local line = trim(raw)
        if line ~= '' and line:sub(1, 1) ~= '#' then
            local from, to, flag = line:match('^(%S+)%s+(%S+)%s*(%S*)$')
            local source = from and parse_source(from)
            local target = to and parse_key(to)
            if source ~= nil and target ~= nil then
                add(source, target, flag == 'block')
            else
                vana.log('sendkey: skipped a line in binds.txt: ' .. line)
            end
        end
    end
end

local function say(text)
    print('sendkey: ' .. text)
end

-- Make a bind from the words after /sendkey bind and say so
local function bind_from(from, to, flag)
    local source, why = parse_source(from)
    if source == nil then
        return say(why)
    end
    local target
    target, why = parse_key(to)
    if target == nil then
        return say(why)
    end
    if flag ~= nil and flag ~= '' and flag ~= 'block' then
        return say("'" .. flag .. "' isn't an option (block)")
    end
    local _, old = add(source, target, flag == 'block')
    local saved = save()
    say((old ~= nil and 'Changed ' or 'Bound ') .. describe(binds[source.id]) .. (saved and '' or " (couldn't be saved)"))
end

local function list_binds()
    if #order == 0 then
        return say('No binds. /sendkey bind pad:rb f13 holds F13 while R1 is held.')
    end
    say(#order .. (#order == 1 and ' bind:' or ' binds:'))
    for _, id in ipairs(order) do
        print('  ' .. describe(binds[id]))
    end
end

local function list_names()
    local pads = {}
    for _, row in ipairs(PAD) do
        pads[#pads + 1] = 'pad:' .. table.concat(row, '/', 2)
    end
    say('Controller buttons: ' .. table.concat(pads, ', ') .. '; joy:<n> for a DirectInput button (0 is the first)')
    local keys = {}
    for name in pairs(KEYS) do
        if not name:find('^f%d+$') and not name:find('^numpad%d$') and #name > 1 then
            keys[#keys + 1] = name
        end
    end
    table.sort(keys)
    say('Keys: a-z, 0-9, f1-f24, numpad0-numpad9, ' .. table.concat(keys, ', '))
    say('Modifiers (ctrl+ shift+ alt+ win+) go before a key: ctrl+shift+m')
end

local function command(line)
    local words = {}
    for word in line:gmatch('%S+') do
        words[#words + 1] = word
    end
    local what = (words[1] or ''):lower()
    if what == 'bind' and words[2] ~= nil and words[3] ~= nil then
        bind_from(words[2], words[3], words[4])
    elseif what == 'learn' and words[2] ~= nil then
        local target, why = parse_key(words[2])
        if target == nil then
            return say(why)
        end
        if words[3] ~= nil and words[3]:lower() ~= 'block' then
            return say("'" .. words[3] .. "' isn't an option (block)")
        end
        learning = { target = target, block = words[3] ~= nil }
        say('Press the controller button or key to send ' .. target.text .. ' (Escape cancels)')
    elseif what == 'unbind' and words[2] ~= nil then
        local source, why = parse_source(words[2])
        if source == nil then
            return say(why)
        end
        if remove(source.id) then
            say('Unbound ' .. source.id .. (save() and '' or " (couldn't be saved)"))
        else
            say(source.id .. ' wasn\'t bound')
        end
    elseif what == 'list' then
        list_binds()
    elseif what == 'clear' then
        release_all()
        binds, order = {}, {}
        say('All binds removed' .. (save() and '' or " (couldn't be saved)"))
    elseif what == 'test' and words[2] ~= nil then
        local target, why = parse_key(words[2])
        if target == nil then
            return say(why)
        end
        press(target, true)
        vana.tasks.once(0.5, function () press(target, false) end)
        say('Sent ' .. target.text .. ' for half a second')
    elseif what == 'names' then
        list_names()
    else
        say('/sendkey bind <from> <to> [block] | learn <to> [block] | unbind <from> | list | clear | test <to> | names')
        say('  e.g. /sendkey bind pad:rb f13   (R1 holds F13, for Discord push to talk)')
    end
end

-------------------------------------------------------------------------------
-- Events
-------------------------------------------------------------------------------

local function chat_open()
    local ok, world = pcall(vana.game.world)
    return ok and type(world) == 'table' and world.chat_open == true
end

local function held_modifiers()
    local held = {}
    for _, name in ipairs(MODIFIER_ORDER) do
        if bit.band(ffi.C.GetAsyncKeyState(MODIFIER_VK[name]), 0x8000) ~= 0 then
            held[#held + 1] = name
        end
    end
    return table.concat(held, '+')
end

local MODIFIER_KEYS = { [0x10] = true, [0x11] = true, [0x12] = true, [0x5B] = true, [0x5C] = true }

-- A learned or bound pad press; `down` is the control now
local function pad_event(kind, code, down)
    local id = kind == 'pad' and ('pad:' .. (PAD_NAME[code] or '')) or ('joy:' .. (code - DIJOFS_BUTTON0))
    if learning ~= nil then
        if not down then
            return false
        end
        local pending = learning
        learning = nil
        local source = parse_source(id)
        add(source, pending.target, pending.block)
        say('Bound ' .. describe(binds[source.id]) .. (save() and '' or " (couldn't be saved)"))
        return true -- the press that was learned isn't the game's
    end
    local bind = binds[id]
    if bind == nil then
        return false
    end
    if down ~= bind.held then
        bind.held = down
        press(bind.target, down)
    end
    return bind.block
end

vana.events.on('xinput_button', function (e)
    local code, value = e.button, e.state
    if e.injected or PAD_NAME[code] == nil then
        return false
    end
    local down
    if code >= 16 then
        down = value > TRIGGER_DOWN
    else
        down = value ~= 0
    end
    return pad_event('pad', code, down)
end)

vana.events.on('dinput_button', function (e)
    local code = e.button
    if e.injected or code < DIJOFS_BUTTON0 or code >= DIJOFS_BUTTON0 + 128 then
        return false
    end
    return pad_event('joy', code, bit.band(e.state, 0x80) ~= 0)
end)

vana.events.on('key', function (wparam, lparam, blocked)
    local up = bit.band(lparam, 0x80000000) ~= 0
    local repeated = bit.band(lparam, 0x40000000) ~= 0 and not up
    if is_ours(wparam) then
        return true -- what this sent comes back to the game's window: the game doesn't take it
    end
    if learning ~= nil then
        if up or MODIFIER_KEYS[wparam] then
            return true -- the keys of the press before, and modifiers on their own, wait
        end
        if wparam == 0x1B then
            learning = nil
            say('Cancelled')
            return true
        end
        local text = held_modifiers()
        local source = parse_source((text ~= '' and (text .. '+') or '') .. (KEY_NAMES[wparam] or ''))
        if source == nil then
            return true
        end
        local pending = learning
        learning = nil
        add(source, pending.target, pending.block)
        say('Bound ' .. describe(binds[source.id]) .. (save() and '' or " (couldn't be saved)"))
        return true
    end
    if #order == 0 or chat_open() then
        return false
    end
    local name = KEY_NAMES[wparam]
    if name == nil then
        return false
    end
    local taken = false
    for _, id in ipairs(order) do
        local bind = binds[id]
        if bind.source.kind == 'key' and bind.source.key.vk == wparam then
            if up then
                if bind.held then
                    bind.held = false
                    press(bind.target, false)
                    taken = taken or bind.block
                end
            elseif not repeated and not bind.held and held_modifiers() == table.concat(bind.source.key.mods, '+') then
                bind.held = true
                press(bind.target, true)
                taken = taken or bind.block
            elseif bind.held then
                taken = taken or bind.block -- its repeats
            end
        end
    end
    return taken
end)

vana.events.on('command', function (e)
    local rest = e.command:match('^/sendkey%s*(.*)$')
    if rest ~= nil then
        command(rest)
        return true -- handled: the game doesn't see it
    end
end)

vana.events.on('load', function ()
    load()
    say(#order == 0 and 'Loaded. /sendkey bind pad:rb f13 holds F13 while R1 is held; /sendkey for the rest.'
                    or ('Loaded ' .. #order .. (#order == 1 and ' bind' or ' binds') .. '. /sendkey list shows them.'))
end)

vana.events.on('unload', function ()
    release_all()
end)
