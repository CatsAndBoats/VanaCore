--[[
vana\gamepadtest.lua: the Gamepad Test window (Client > Gamepads > Gamepad Test, or /gamepad test): what each controller
Windows has is doing right now. Loaded by vana.lua with the shared table M; drawn each frame it's open, with the /vana window
open or not, in the same look (draw.lua).

It reads the controllers from Windows itself, not through the game, so it shows the same whether the game uses XInput or
DirectInput, and with the game in the background:

  XInput controllers   slots 1 to 4 (Xbox pads and what looks like one): every button, both sticks and both triggers drawn on
                       the pad, with its type, battery, wireless and force feedback, the reports a second and the packet count
  Joysticks            what Windows' joystick driver lists (DualSense, DS4, Switch pads, wheels, and an Xbox pad again):
                       every button, the axes, the hat, and the buttons and axes it says it has
  Held now, last presses, peaks   per controller, kept while the window is open; Reset clears them

While it's open the game ignores the controller (vana.gamepad.nav), so testing a button doesn't walk the character off. The
switch under the picker lets the game have it again.
--]]

local M = ...
local imgui = vana.imgui
local S, P, fonts = M.S, M.P, M.fonts
local BODY, SMALL = M.BODY, M.SMALL
local st = M.st
local bit = require('bit')
local band = bit.band

M.gptest = { open = false, hold = true, pick = nil, error = nil, recording = false }
local gt = M.gptest
local rec = { on = false } -- the recording (below): what's logged while Record is on
gt.rec = rec -- (for a test harness to drive it)
M.FA.record, M.FA.stop = M.FA.record or 0xF111, M.FA.stop or 0xF04D -- the Record button's icons

local WIN_W = 620
local PAD = 18
local TITLE_H = 40
local ROW_H = 38     -- the picker
local HOLD_H = 30    -- the switch under it
local HEAD_H = 26    -- a section's title
local HEADER_H = 56  -- the controller's name and what it is
local NOTE_H = 20
local STAT_H = 21    -- a line of the stats
local DIAGRAM_W, DIAGRAM_H = 296, 232
local FOOT_H = HEAD_H + NOTE_H * 5 + 8
local DEAD_L, DEAD_R, TRIGGER_ON = 7849, 8689, 30 -- XInput's: the sticks' dead zones and a trigger's threshold

-------------------------------------------------------------------------------
-- Windows' controller functions, through FFI (declared once, loaded when the window first opens)
-------------------------------------------------------------------------------

local ffi_ok, ffi = pcall(require, 'ffi')
local api = nil

local CDEF = [[
    typedef struct { unsigned short wButtons; unsigned char bLeftTrigger, bRightTrigger; short sThumbLX, sThumbLY, sThumbRX, sThumbRY; } GPT_XGAMEPAD;
    typedef struct { unsigned long dwPacketNumber; GPT_XGAMEPAD Gamepad; } GPT_XSTATE;
    typedef struct { unsigned char Type, SubType; unsigned short Flags; GPT_XGAMEPAD Gamepad; unsigned short LeftMotor, RightMotor; } GPT_XCAPS;
    typedef struct { unsigned char BatteryType, BatteryLevel; } GPT_XBATTERY;
    typedef struct { unsigned long dwSize, dwFlags, dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos, dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2; } GPT_JOYINFOEX;
    typedef struct { unsigned short wMid, wPid; unsigned short szPname[32]; unsigned int wXmin, wXmax, wYmin, wYmax, wZmin, wZmax, wNumButtons, wPeriodMin, wPeriodMax, wRmin, wRmax, wUmin, wUmax, wVmin, wVmax, wCaps, wMaxAxes, wNumAxes, wMaxButtons; unsigned short szRegKey[32]; unsigned short szOEMVxD[260]; } GPT_JOYCAPSW;
    unsigned long __stdcall XInputGetState(unsigned long user, GPT_XSTATE* state);
    unsigned long __stdcall XInputGetCapabilities(unsigned long user, unsigned long flags, GPT_XCAPS* caps);
    unsigned long __stdcall XInputGetBatteryInformation(unsigned long user, unsigned char devtype, GPT_XBATTERY* info);
    unsigned int __stdcall joyGetNumDevs(void);
    unsigned int __stdcall joyGetPosEx(unsigned int id, GPT_JOYINFOEX* info);
    unsigned int __stdcall joyGetDevCapsW(unsigned int id, GPT_JOYCAPSW* caps, unsigned int size);
    long __stdcall GetMessageTime(void);
    unsigned long __stdcall GetTickCount(void);
    long __stdcall RegGetValueW(void* hkey, const unsigned short* sub, const unsigned short* value, unsigned long flags, unsigned long* type, void* data, unsigned long* size);
    int __stdcall MultiByteToWideChar(unsigned int cp, unsigned long flags, const char* text, int n, unsigned short* wide, int wn);
    int __stdcall WideCharToMultiByte(unsigned int cp, unsigned long flags, const unsigned short* wide, int wn, char* text, int tn, const char* fallback, int* used);
]]

local function load_api()
    if api ~= nil then
        return api
    end
    api = { ok = false, why = 'FFI is missing' }
    if not ffi_ok then
        return api
    end
    local ok, err = pcall(ffi.cdef, CDEF)
    if not ok and not tostring(err):find('redefine', 1, true) then
        api.why = tostring(err)
        return api
    end
    local loaded, wm = pcall(ffi.load, 'winmm')
    if not loaded then
        api.why = "winmm.dll didn't load"
        return api
    end
    api.wm = wm
    api.k32 = ffi.load('kernel32')
    api.adv = ffi.load('advapi32')
    api.u32 = ffi.load('user32')
    for _, name in ipairs({ 'xinput1_4', 'xinput1_3', 'xinput9_1_0' }) do
        local found, lib = pcall(ffi.load, name)
        if found then
            api.xi, api.xi_name = lib, name
            break
        end
    end
    api.xstate, api.xcaps, api.xbat = ffi.new('GPT_XSTATE'), ffi.new('GPT_XCAPS'), ffi.new('GPT_XBATTERY')
    api.jinfo, api.jcaps = ffi.new('GPT_JOYINFOEX'), ffi.new('GPT_JOYCAPSW')
    api.jinfo_size, api.jcaps_size = ffi.sizeof('GPT_JOYINFOEX'), ffi.sizeof('GPT_JOYCAPSW')
    api.wkey, api.wval, api.wdata = ffi.new('unsigned short[260]'), ffi.new('unsigned short[32]'), ffi.new('unsigned short[128]')
    api.size, api.cbuf = ffi.new('unsigned long[1]'), ffi.new('char[520]')
    api.hkcu = ffi.cast('void*', 0x80000001)
    api.ok = true
    return api
end

local function trim(text)
    return (text:gsub('^%s+', ''):gsub('%s+$', ''))
end

-- the name Windows keeps for a joystick (the winmm driver only says "Microsoft PC-joystick driver"), by its USB ids
local function registry_name(vid, pid)
    local key = string.format('System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_%04X&PID_%04X', vid, pid)
    api.k32.MultiByteToWideChar(65001, 0, key, -1, api.wkey, 260)
    api.k32.MultiByteToWideChar(65001, 0, 'OEMName', -1, api.wval, 32)
    api.size[0] = 256
    if api.adv.RegGetValueW(api.hkcu, api.wkey, api.wval, 2, nil, api.wdata, api.size) ~= 0 then
        return nil
    end
    local n = api.k32.WideCharToMultiByte(65001, 0, api.wdata, -1, api.cbuf, 520, nil, nil)
    if n <= 1 then
        return nil
    end
    local name = trim(ffi.string(api.cbuf, n - 1))
    return name ~= '' and name or nil
end

-------------------------------------------------------------------------------
-- What the controllers are doing
-------------------------------------------------------------------------------

-- XInput's wButtons: mask and name
local X_BUTTONS = {
    { 0x1000, 'A' }, { 0x2000, 'B' }, { 0x4000, 'X' }, { 0x8000, 'Y' }, { 0x0100, 'LB' }, { 0x0200, 'RB' },
    { 0x0020, 'Back' }, { 0x0010, 'Start' }, { 0x0040, 'L3' }, { 0x0080, 'R3' },
    { 0x0001, 'D-pad up' }, { 0x0002, 'D-pad down' }, { 0x0004, 'D-pad left' }, { 0x0008, 'D-pad right' },
}
local X_SUBTYPES = {
    [1] = 'Gamepad', [2] = 'Wheel', [3] = 'Arcade stick', [4] = 'Flight stick', [5] = 'Dance pad', [6] = 'Guitar',
    [7] = 'Guitar (alternate)', [8] = 'Drum kit', [11] = 'Guitar (bass)', [19] = 'Arcade pad',
}
local BATTERY_TYPES = { [0] = 'none', [1] = 'wired', [2] = 'alkaline', [3] = 'NiMH' }
local BATTERY_LEVELS = { [0] = 'Empty', [1] = 'Low', [2] = 'Medium', [3] = 'Full' }
local HATS = { 'up', 'up-right', 'right', 'down-right', 'down', 'down-left', 'left', 'up-left' }
local AXES = { { 'X', 0 }, { 'Y', 0 }, { 'Z', 1 }, { 'R', 2 }, { 'U', 4 }, { 'V', 8 } } -- name, its JOYCAPS_HAS* bit (X and Y are always there)

-- every controller seen, by key ('x0'..'x3' XInput slots, 'j0'.. winmm joysticks), what's there now in `list`
local sources = { list = {}, by_key = {}, scanned = -100 }

local function source(key, kind)
    local s = sources.by_key[key]
    if s == nil then
        s = { key = key, kind = kind, buttons = 0, pov = 65535, packet = 0, rate = 0, rate_n = 0, rate_at = 0,
              lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0, axes = { 0, 0, 0, 0, 0, 0 }, base = nil,
              held = {}, down = {}, count = {}, history = {}, peak = {}, axmin = {}, axmax = {}, stick_on = {}, axis_on = {} }
        sources.by_key[key] = s
    end
    return s
end

local function scan(now)
    local found = {}
    if api.xi ~= nil then
        for slot = 0, 3 do
            if api.xi.XInputGetState(slot, api.xstate) == 0 then
                local s = source('x' .. slot, 'x')
                s.slot, s.gone = slot, false
                s.name = 'XInput controller ' .. (slot + 1)
                if api.xi.XInputGetCapabilities(slot, 0, api.xcaps) == 0 then
                    s.type, s.subtype, s.flags = api.xcaps.Type, api.xcaps.SubType, api.xcaps.Flags
                end
                s.battery = nil
                local ok, result = pcall(function () return api.xi.XInputGetBatteryInformation(slot, 0, api.xbat) end)
                if ok and result == 0 then
                    s.battery = { api.xbat.BatteryType, api.xbat.BatteryLevel }
                end
                found[#found + 1] = s
            end
        end
    end
    local count = math.min(16, tonumber(api.wm.joyGetNumDevs()) or 0)
    for id = 0, count - 1 do
        api.jinfo.dwSize, api.jinfo.dwFlags = api.jinfo_size, 0xFF
        if api.wm.joyGetPosEx(id, api.jinfo) == 0 then
            local s = source('j' .. id, 'j')
            s.id, s.gone = id, false
            if api.wm.joyGetDevCapsW(id, api.jcaps, api.jcaps_size) == 0 then
                local c = api.jcaps
                s.vid, s.pid, s.buttons_n, s.axes_n, s.caps = c.wMid, c.wPid, c.wNumButtons, c.wNumAxes, c.wCaps
                s.range = { { c.wXmin, c.wXmax }, { c.wYmin, c.wYmax }, { c.wZmin, c.wZmax }, { c.wRmin, c.wRmax },
                            { c.wUmin, c.wUmax }, { c.wVmin, c.wVmax } }
            end
            if s.name == nil then
                s.name = (s.vid ~= nil and registry_name(s.vid, s.pid)) or ('Joystick ' .. (id + 1))
            end
            found[#found + 1] = s
        end
    end
    if rec.on then -- a controller plugged in or pulled out while recording
        local before, after = {}, {}
        for _, s in ipairs(sources.list) do before[s.key] = s end
        for _, s in ipairs(found) do after[s.key] = s end
        for key, s in pairs(after) do
            if before[key] == nil then rec.log(now, 'pad.poll', 'controller connected: ' .. s.name) end
        end
        for key, s in pairs(before) do
            if after[key] == nil then rec.log(now, 'pad.poll', 'controller gone: ' .. s.name) end
        end
    end
    sources.list = found
end

-- a button or direction going down is a press (counted, put first in the history), coming up ends it
local function track(s, label, down, now)
    if (s.down[label] or false) == down then
        return
    end
    s.down[label] = down
    if rec.on then
        local ok, err = pcall(rec.pad_event, s, label, down, now)
        if not ok then
            rec.error = tostring(err)
        end
    end
    if down then
        s.count[label] = (s.count[label] or 0) + 1
        table.insert(s.history, 1, { label = label, at = now })
        if #s.history > 8 then
            s.history[#s.history] = nil
        end
    else
        for _, entry in ipairs(s.history) do
            if entry.label == label and entry.ended == nil then
                entry.ended = now
                break
            end
        end
    end
end

local function clear(list)
    for i = #list, 1, -1 do
        list[i] = nil
    end
end

local function stick_magnitude(x, y)
    return math.min(1, math.sqrt(x * x + y * y) / 32767)
end

-- a hat's reading (hundredths of a degree, or -1 / 65535 centred) as 'Hat up-right', nil when centred
local function hat_name(pov)
    if pov == nil or pov < 0 or pov > 35999 then
        return nil
    end
    return 'Hat ' .. HATS[math.floor((pov / 100 + 22.5) / 45) % 8 + 1]
end

local function rate_tick(s, now)
    if now - s.rate_at >= 1 then
        s.rate = s.rate_n / math.max(0.001, now - s.rate_at)
        s.rate_n, s.rate_at = 0, now
    end
end

-- false when the controller has gone since the last search
local function poll_x(s, now)
    local state = api.xstate
    if api.xi.XInputGetState(s.slot, state) ~= 0 then
        s.gone = true
        return false
    end
    local g = state.Gamepad
    if state.dwPacketNumber ~= s.packet then
        s.packet_step = state.dwPacketNumber - s.packet -- the reports since the last frame (more than 1: some came between frames)
        s.packet = state.dwPacketNumber
        s.rate_n = s.rate_n + 1
    end
    s.buttons, s.lt, s.rt = g.wButtons, g.bLeftTrigger, g.bRightTrigger
    s.lx, s.ly, s.rx, s.ry = g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY
    local held = s.held
    clear(held)
    for _, b in ipairs(X_BUTTONS) do
        local down = band(s.buttons, b[1]) ~= 0
        track(s, b[2], down, now)
        if down then
            held[#held + 1] = b[2]
        end
    end
    track(s, 'LT', s.lt > TRIGGER_ON, now)
    track(s, 'RT', s.rt > TRIGGER_ON, now)
    if s.lt > TRIGGER_ON then held[#held + 1] = 'LT' end
    if s.rt > TRIGGER_ON then held[#held + 1] = 'RT' end
    local left, right = stick_magnitude(s.lx, s.ly), stick_magnitude(s.rx, s.ry)
    if rec.on then -- a stick leaving its dead zone or going back to it
        for i = 1, 2 do
            local magnitude, dead = i == 1 and left or right, i == 1 and DEAD_L or DEAD_R
            local active = magnitude * 32767 > dead
            if active ~= (s.stick_on[i] or false) then
                s.stick_on[i] = active
                rec.log(now, 'pad.poll', string.format('XInput %d  %s stick %s (%d, %d)', s.slot + 1, i == 1 and 'left' or 'right',
                        active and 'moved out of its dead zone' or 'back inside its dead zone', i == 1 and s.lx or s.rx, i == 1 and s.ly or s.ry))
            end
        end
    end
    local peak = s.peak
    peak.l, peak.r = math.max(peak.l or 0, left), math.max(peak.r or 0, right)
    peak.lt, peak.rt = math.max(peak.lt or 0, s.lt), math.max(peak.rt or 0, s.rt)
    if #held > 0 or left * 32767 > DEAD_L or right * 32767 > DEAD_R then
        s.last_input = now
    end
    rate_tick(s, now)
    return true
end

local function poll_j(s, now)
    local info = api.jinfo
    info.dwSize, info.dwFlags = api.jinfo_size, 0xFF
    if api.wm.joyGetPosEx(s.id, info) ~= 0 then
        s.gone = true
        return false
    end
    local a = s.axes
    local v1, v2, v3, v4, v5, v6 = info.dwXpos, info.dwYpos, info.dwZpos, info.dwRpos, info.dwUpos, info.dwVpos
    local buttons, pov = info.dwButtons, info.dwPOV
    if buttons ~= s.buttons or pov ~= s.pov or v1 ~= a[1] or v2 ~= a[2] or v3 ~= a[3] or v4 ~= a[4] or v5 ~= a[5] or v6 ~= a[6] then
        s.rate_n = s.rate_n + 1
    end
    a[1], a[2], a[3], a[4], a[5], a[6] = v1, v2, v3, v4, v5, v6
    s.buttons, s.pov = buttons, pov
    if s.base == nil then
        s.base = { v1, v2, v3, v4, v5, v6 } -- where the axes first read: the controller at rest
    end
    local active = buttons ~= 0
    for i = 1, 6 do
        s.axmin[i] = math.min(s.axmin[i] or a[i], a[i])
        s.axmax[i] = math.max(s.axmax[i] or a[i], a[i])
        local range = s.range ~= nil and s.range[i] or { 0, 65535 }
        if math.abs(a[i] - s.base[i]) > (range[2] - range[1]) * 0.08 then
            active = true
        end
        if rec.on and (i <= (s.axes_n or 0) or i <= 2) then -- an axis moving 25% of its range from where it rested, and back
            local moved = math.abs(a[i] - s.base[i]) > (range[2] - range[1]) * 0.25
            if moved ~= (s.axis_on[i] or false) then
                s.axis_on[i] = moved
                rec.log(now, 'pad.poll', string.format('joy %d  axis %s %s (%d)', s.id + 1, AXES[i][1], moved and 'moved' or 'back to rest', a[i]))
            end
        end
    end
    local held = s.held
    clear(held)
    for n = 0, math.min(32, s.buttons_n or 0) - 1 do
        local label = 'Button ' .. (n + 1)
        local down = band(buttons, bit.lshift(1, n)) ~= 0
        track(s, label, down, now)
        if down then
            held[#held + 1] = label
        end
    end
    local hat = hat_name(pov)
    if hat ~= nil then
        active = true
    end
    for _, name in ipairs(HATS) do
        track(s, 'Hat ' .. name, hat == 'Hat ' .. name, now)
    end
    if hat ~= nil then
        held[#held + 1] = hat
    end
    if active then
        s.last_input = now
    end
    rate_tick(s, now)
    return true
end

-- the controllers searched for once a second, each read every frame
local function refresh(now)
    local a = load_api()
    if not a.ok or sources.polled_at == now then
        return -- (the recording's tick and the window's draw each ask once a frame)
    end
    sources.polled_at = now
    local timed = rec.on -- (while recording: what this costs a frame, so the log can show it isn't the lag)
    local t0 = timed and vana.time.clock() or 0
    if now - sources.scanned >= 1 then
        sources.scanned = now
        local ok, err = pcall(scan, now)
        gt.error = (not ok) and tostring(err) or nil
        if timed then
            rec.cost.scan = math.max(rec.cost.scan, vana.time.clock() - t0)
        end
    end
    local t1 = timed and vana.time.clock() or 0
    for _, s in ipairs(sources.list) do
        local ok, alive = pcall(s.kind == 'x' and poll_x or poll_j, s, now)
        if not ok then
            gt.error = tostring(alive)
        elseif alive == false then
            sources.scanned = -100 -- one went: look again next frame
        end
    end
    if timed then
        rec.cost.poll = math.max(rec.cost.poll, vana.time.clock() - t1)
    end
end

-------------------------------------------------------------------------------
-- Recording: what the controllers, the keyboard and the game did, and when, in a file in logs\
--
-- A press goes through three stages, each timed so a delay can be put on one of them:
--   1. Windows to the game: pad.poll (the controller read straight from Windows once a frame, at the frame's end) against
--      game.pad (the game's own read of it: xinput_button / dinput_button); for a key, key.msg (the key message reaching the
--      game, with how long it sat in the queue) against game.key (the game's DirectInput read of it)
--   2. inside the game: the first packet it sends after the input (game.out), or a window opening (game.state)
--   3. the server: gaps in what comes back (game.in)
-- Frame hitches and the game's input read gaps are logged too: the usual causes of a press that arrives late.
-------------------------------------------------------------------------------

local x_type, battery_text -- (below, in Words)
local REC_KEY = 'gamepadtest.rec'
local REC_EVENTS = { 'beginscene', 'xinput_button', 'dinput_button', 'xinput_state', 'key_state', 'key', 'packet_out', 'packet_in',
                     'menu_open', 'menu_close', 'main_menu_open', 'main_menu_close', 'action_menu_open', 'action_menu_close',
                     'chat_input_open', 'chat_input_close', 'target_change' }
local REC_MAX_SECONDS, REC_MAX_LINES = 600, 200000
local HITCH, READ_GAP, IN_GAP, ANSWER = 0.030, 0.040, 0.5, 1.5 -- seconds: a slow frame, a gap in the game's input reads or in its packets, how long after an input a reply counts

-- Ashita's numbering of an XInput control (the xinput_button event's), as the Gamepad Test names it
local X_NUMBERS = {
    [0] = 'D-pad up', 'D-pad down', 'D-pad left', 'D-pad right', 'Start', 'Back', 'L3', 'R3', 'LB', 'RB',
    [12] = 'A', 'B', 'X', 'Y', 'LT', 'RT', 'Left stick X', 'Left stick Y', 'Right stick X', 'Right stick Y',
}
local DI_XBOX = { 'A', 'B', 'X', 'Y', 'LB', 'RB', 'Back', 'Start', 'L3', 'R3' } -- a DirectInput Xbox pad's buttons 1 to 10
local HAT_OF_DPAD = { ['D-pad up'] = 'Hat up', ['D-pad down'] = 'Hat down', ['D-pad left'] = 'Hat left', ['D-pad right'] = 'Hat right' }
local PACKETS = { [0x015] = 'position', [0x01A] = 'action: target, attack, magic, ability', [0x037] = 'use item' }

local VK_NAMES = {
    [0x08] = 'Backspace', [0x09] = 'Tab', [0x0D] = 'Enter', [0x10] = 'Shift', [0x11] = 'Ctrl', [0x12] = 'Alt', [0x1B] = 'Esc',
    [0x20] = 'Space', [0x21] = 'PageUp', [0x22] = 'PageDown', [0x23] = 'End', [0x24] = 'Home', [0x25] = 'Left', [0x26] = 'Up',
    [0x27] = 'Right', [0x28] = 'Down', [0x2C] = 'PrintScreen', [0x2D] = 'Insert', [0x2E] = 'Delete', [0x5B] = 'LWin',
    [0x5C] = 'RWin', [0xA0] = 'LShift', [0xA1] = 'RShift', [0xA2] = 'LCtrl', [0xA3] = 'RCtrl', [0xA4] = 'LAlt', [0xA5] = 'RAlt',
    [0xBA] = ';', [0xBB] = '=', [0xBC] = ',', [0xBD] = '-', [0xBE] = '.', [0xBF] = '/', [0xC0] = '`', [0xDB] = '[',
    [0xDC] = '\\', [0xDD] = ']', [0xDE] = "'",
}
local function vk_name(vk)
    if VK_NAMES[vk] ~= nil then
        return VK_NAMES[vk]
    elseif (vk >= 0x30 and vk <= 0x39) or (vk >= 0x41 and vk <= 0x5A) then
        return string.char(vk)
    elseif vk >= 0x70 and vk <= 0x87 then
        return 'F' .. (vk - 0x6F)
    elseif vk >= 0x60 and vk <= 0x69 then
        return 'Num' .. (vk - 0x60)
    end
    return string.format('VK 0x%02X', vk)
end

-- DirectInput's keyboard codes (the game's key_state), by name
local DIK_NAMES = {
    [0x01] = 'Esc', [0x0C] = '-', [0x0D] = '=', [0x0E] = 'Backspace', [0x0F] = 'Tab', [0x1A] = '[', [0x1B] = ']', [0x1C] = 'Enter',
    [0x1D] = 'LCtrl', [0x27] = ';', [0x28] = "'", [0x29] = '`', [0x2A] = 'LShift', [0x2B] = '\\', [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x36] = 'RShift', [0x37] = 'Num*', [0x38] = 'LAlt', [0x39] = 'Space', [0x3A] = 'CapsLock', [0x57] = 'F11',
    [0x58] = 'F12', [0x9C] = 'NumEnter', [0x9D] = 'RCtrl', [0xB5] = 'Num/', [0xB8] = 'RAlt', [0xC7] = 'Home', [0xC8] = 'Up',
    [0xC9] = 'PageUp', [0xCB] = 'Left', [0xCD] = 'Right', [0xCF] = 'End', [0xD0] = 'Down', [0xD1] = 'PageDown',
    [0xD2] = 'Insert', [0xD3] = 'Delete', [0xDB] = 'LWin', [0xDC] = 'RWin',
}
for i, digit in ipairs({ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' }) do DIK_NAMES[0x01 + i] = digit end
for i = 1, 10 do DIK_NAMES[0x3A + i] = 'F' .. i end
for start, letters in pairs({ [0x10] = 'QWERTYUIOP', [0x1E] = 'ASDFGHJKL', [0x2C] = 'ZXCVBNM' }) do
    for i = 1, #letters do DIK_NAMES[start + i - 1] = letters:sub(i, i) end
end
for i, name in ipairs({ 'Num7', 'Num8', 'Num9', 'Num-', 'Num4', 'Num5', 'Num6', 'Num+', 'Num1', 'Num2', 'Num3', 'Num0', 'Num.' }) do
    DIK_NAMES[0x46 + i] = name
end
local function dik_name(code)
    return DIK_NAMES[code] or string.format('DIK 0x%02X', code)
end

-- one name for the same physical control as the pad polls and the game's reads call it (a DirectInput Xbox pad's Button 1 is
-- the XInput A, a hat's direction the D-pad's)
local function canon(label)
    local n = label:match('^Button (%d+)$')
    if n ~= nil then
        return DI_XBOX[tonumber(n)] or label
    end
    return HAT_OF_DPAD[label] or label
end

local function add(list, value)
    if #list < 5000 then
        list[#list + 1] = value
    end
end

local function describe(list)
    local n = #list
    if n == 0 then
        return 'none'
    end
    local sorted = {}
    for i = 1, n do sorted[i] = list[i] end
    table.sort(sorted)
    return string.format('%d    min %.1f   median %.1f   95%% %.1f   max %.1f ms', n, sorted[1], sorted[math.ceil(n / 2)],
                         sorted[math.max(1, math.ceil(n * 0.95))], sorted[n])
end

local function biggest(list)
    local m = 0
    for _, v in ipairs(list) do
        if v > m then m = v end
    end
    return m
end

function rec.log(now, tag, text)
    local buf = rec.buf
    buf[#buf + 1] = string.format('%9.1f ms  %-9s %s', (now - rec.t0) * 1000, tag, text)
    rec.lines = rec.lines + 1
end

local function rec_flush()
    if rec.file ~= nil and #rec.buf > 0 then
        rec.file:write(table.concat(rec.buf, '\n'), '\n')
        rec.file:flush()
        rec.buf = {}
    end
end

-- a pad press in the controller read straight from Windows (track() calls this for each change, from the poll)
-- Each physical control has one state, however many ways it's read (an XInput pad and its joystick twin, the game's
-- DirectInput read): rec.ctl[name] = { poll = { [where] = true }, n = how many polls hold it, poll_t, game_t, game_down }.
-- A diagonal hat is its two directions, as the XInput D-pad has them (rec.hat_dirs, in the game's reads).
function rec.pad_event(s, label, down, now)
    local where = s.kind == 'x' and ('XInput ' .. (s.slot + 1)) or ('joy ' .. ((s.id or 0) + 1))
    local frame = string.format('[last frame %.1f ms%s]', rec.dt * 1000,
                                s.kind == 'x' and (', reports +' .. (s.packet_step or 0)) or '')
    if label == 'LT' or label == 'RT' or label:sub(1, 4) == 'Hat ' then
        -- (a trigger is an axis in DirectInput, and a joystick's eight-way hat can't be matched direction by direction)
        rec.log(now, 'pad.poll', string.format('%s  %s %s  %s', where, label, down and 'down' or 'up', down and frame or ''))
        return
    end
    local c = canon(label)
    local st = rec.ctl[c]
    if down then
        if st == nil then
            st = { poll = {}, n = 0 }
            rec.ctl[c] = st
        end
        if st.poll[where] then
            return
        end
        st.poll[where], st.n = true, st.n + 1
        local note = ''
        if st.poll_t == nil then
            st.poll_t = now
            if st.game_t ~= nil then
                note = string.format('  (the game read it %.1f ms before this poll)', (now - st.game_t) * 1000)
            end
        end
        rec.log(now, 'pad.poll', string.format('%s  %s down%s  %s', where, label, note, frame))
    else
        if st == nil or not st.poll[where] then
            rec.log(now, 'pad.poll', string.format('%s  %s up', where, label))
            return
        end
        st.poll[where], st.n = nil, st.n - 1
        local note = ''
        if st.n == 0 and st.game_t == nil and not st.lost then -- over before the game read it at all
            rec.stat.short = rec.stat.short + 1
            note = string.format('    released %.1f ms after the press, before the game read it  %s', (now - st.poll_t) * 1000, frame)
        end
        rec.log(now, 'pad.poll', string.format('%s  %s up%s', where, label, note))
        if st.n == 0 and not st.game_down then
            rec.ctl[c] = nil
        end
    end
end

-- the game's own read of a controller press (it can come twice for one press: the second is left out and counted)
function rec.game_pad(now, how, label, down, flags)
    local c = canon(label)
    local st = rec.ctl[c]
    if not down then
        if st == nil or not st.game_down then
            rec.stat.dups = rec.stat.dups + 1
            return
        end
        st.game_down = false
        if st.n == 0 then
            rec.ctl[c] = nil
        end
        rec.log(now, 'game.pad', string.format('%s  %s up%s', how, label, flags))
        return
    end
    if st ~= nil and st.game_down then
        rec.stat.dups = rec.stat.dups + 1
        return
    end
    if st == nil then
        st = { poll = {}, n = 0 }
        rec.ctl[c] = st
    end
    st.game_down = true
    local note = '(no pad.poll of it first)'
    if st.poll_t ~= nil and st.game_t == nil then
        local delay = (now - st.poll_t) * 1000
        add(rec.d.pad_game, delay)
        note = string.format('(+%.1f ms after pad.poll)', delay)
    elseif st.poll_t ~= nil then
        note = ''
    end
    st.game_t = st.game_t or now
    rec.last_input = { t = now, what = how .. ' ' .. label }
    local phase = rec.frame_at ~= nil and (now - rec.frame_at) * 1000 or 0
    add(rec.d.phase, phase)
    rec.log(now, 'game.pad', string.format('%s  %s down  %s%s  [%.1f ms after the last present, last frame %.1f ms]', how, label, note, flags, phase, rec.dt * 1000))
end

-- a key message reaching the game's window (and how long it sat in the queue first)
function rec.key_msg(now, wparam, lparam)
    local release = bit.band(lparam, 0x80000000) ~= 0
    if not release and bit.band(lparam, 0x40000000) ~= 0 then
        return -- a repeat
    end
    local scan = bit.band(bit.rshift(lparam, 16), 0xFF)
    local dik = scan + (bit.band(lparam, 0x1000000) ~= 0 and 0x80 or 0)
    if release then
        rec.log(now, 'key.msg', vk_name(wparam) .. ' up')
        return
    end
    local age = (api.k32.GetTickCount() - api.u32.GetMessageTime()) % 4294967296
    local text = ''
    if age < 10000 then
        add(rec.d.key_age, age)
        text = string.format('  queue age %d ms', age)
    end
    rec.key_pending[dik] = now
    rec.log(now, 'key.msg', string.format('%s down%s  [last frame %.1f ms]', vk_name(wparam), text, rec.dt * 1000))
end

-- the game's DirectInput keyboard read changed a key
function rec.game_key(now, dik, down)
    local name = dik_name(dik)
    if not down then
        rec.log(now, 'game.key', name .. ' up')
        return
    end
    local sent = rec.key_pending[dik]
    local note = '(no key message first)'
    if sent ~= nil then
        local delay = (now - sent) * 1000
        rec.key_pending[dik] = nil
        add(rec.d.key_game, delay)
        note = string.format('(+%.1f ms after key.msg)', delay)
    end
    rec.last_input = { t = now, what = 'key ' .. name }
    local phase = rec.frame_at ~= nil and (now - rec.frame_at) * 1000 or 0
    add(rec.d.phase, phase)
    rec.log(now, 'game.key', string.format('%s down  %s  [%.1f ms after the last present, last frame %.1f ms]', name, note, phase, rec.dt * 1000))
end

-- what the game did after an input: a packet it sent, or a window or the target changing
function rec.reply(now, tag, text, packet_id)
    local li = rec.last_input
    local note = ''
    if li ~= nil and now - li.t < ANSWER and packet_id ~= 0x015 then
        local delay = (now - li.t) * 1000
        note = string.format('  (+%.1f ms after %s)', delay, li.what)
        if not li.answered then
            li.answered = true
            add(rec.d.game_out, delay)
        end
    end
    rec.log(now, tag, text .. note)
end

-- how long a gap or a frame has to be to count as a hitch: 1.6 times the frames lately (the game can run at 28 FPS: a 36 ms
-- frame is its normal), and never under the floor
local function slow_after(floor)
    return math.max(floor, (rec.avg or 0) * 1.6)
end

function rec.read_gap(now, what, gap)
    add(rec.d.read_gap, gap * 1000)
    rec.log(now, 'game.read', string.format('the game read %s %.0f ms after its last read%s', what, gap * 1000,
            gap > 0.2 and ' (a hitch)' or ''))
end

local function install()
    local clock = vana.time.clock
    local U8P = ffi.typeof('uint8_t*')
    local function on(name, fn)
        vana.events.on(name, fn, REC_KEY)
    end
    -- the controllers read again as the frame starts to draw: the game reads its input late in the frame, so a press this
    -- sees is one the game has not read yet, and the gap to its read is the press waiting
    on('beginscene', function ()
        if rec.poll_frame ~= rec.stat.frames then
            rec.poll_frame = rec.stat.frames
            refresh(clock())
        end
    end)
    on('xinput_button', function (e)
        local now = clock()
        local control, state = e.button, e.state
        local label = X_NUMBERS[control] or ('control ' .. control)
        local flags = (e.injected and '  [injected]' or '') .. (e.blocked and '  [blocked]' or '')
        if control < 16 then
            rec.game_pad(now, 'XInput', label, state ~= 0, flags)
        elseif control < 18 then
            local down = state > TRIGGER_ON
            if down ~= (rec.trigger_on[control] or false) then
                rec.trigger_on[control] = down
                rec.game_pad(now, 'XInput', label, down, flags .. '  (value ' .. state .. ')')
            end
        else
            local active = math.abs(state) > 8192
            if active ~= (rec.stick_on[control] or false) then
                rec.stick_on[control] = active
                rec.log(now, 'game.pad', string.format('XInput  %s %s (%d)%s', label, active and 'moved' or 'back to the centre', state, flags))
            end
        end
    end)
    on('dinput_button', function (e)
        local now = clock()
        local offset, state = e.button, e.state
        local flags = (e.injected and '  [injected]' or '') .. (e.blocked and '  [blocked]' or '')
        if offset >= 48 then
            rec.game_pad(now, 'DirectInput', 'Button ' .. (offset - 47), bit.band(state, 0x80) ~= 0, flags)
        elseif offset >= 32 then
            -- a hat is up, right, down and left, two at once on a diagonal: each direction's press and release on its own,
            -- as the XInput D-pad has them
            local angle = (state >= 0 and state <= 35999) and state / 100 or nil
            local now_dirs = {
                up = angle ~= nil and (angle <= 45 or angle >= 315),
                right = angle ~= nil and angle >= 45 and angle <= 135,
                down = angle ~= nil and angle >= 135 and angle <= 225,
                left = angle ~= nil and angle >= 225 and angle <= 315,
            }
            local was = rec.hat_dirs[offset] or {}
            for _, dir in ipairs({ 'up', 'right', 'down', 'left' }) do
                if now_dirs[dir] ~= (was[dir] or false) then
                    rec.game_pad(now, 'DirectInput', 'Hat ' .. dir, now_dirs[dir], flags)
                end
            end
            rec.hat_dirs[offset] = now_dirs
        else
            rec.axis_base[offset] = rec.axis_base[offset] or state
            local active = math.abs(state - rec.axis_base[offset]) > 16384
            if active ~= (rec.axis_on[offset] or false) then
                rec.axis_on[offset] = active
                rec.log(now, 'game.pad', string.format('DirectInput  axis %d %s (%d)%s', offset / 4 + 1, active and 'moved' or 'back to rest', state, flags))
            end
        end
    end)
    on('xinput_state', function (e)
        local now = clock()
        rec.sec.xreads = rec.sec.xreads + 1
        if e.user == 0 then
            local last = rec.xread_at
            if last ~= nil and now - last > slow_after(READ_GAP) then rec.read_gap(now, 'XInput', now - last) end
            rec.xread_at = now
        end
    end)
    on('key_state', function (e)
        local now = clock()
        rec.sec.kreads = rec.sec.kreads + 1
        local last = rec.kread_at
        if last ~= nil and now - last > slow_after(READ_GAP) then rec.read_gap(now, 'the keyboard and pad', now - last) end
        rec.kread_at = now
        local data = ffi.string(ffi.cast(U8P, e.data_raw), 256)
        if data ~= rec.keys then
            local before = rec.keys
            rec.keys = data
            if before ~= nil then
                for i = 1, 256 do
                    local a, b = data:byte(i), before:byte(i)
                    if a ~= b then rec.game_key(now, i - 1, bit.band(a, 0x80) ~= 0) end
                end
            end
        end
    end)
    on('key', function (wparam, lparam)
        rec.key_msg(clock(), wparam, lparam)
    end)
    on('packet_out', function (e)
        local now = clock()
        rec.sec.pout = rec.sec.pout + 1
        local kind = PACKETS[e.id]
        rec.reply(now, 'game.out', string.format('packet 0x%03X  %d bytes%s', e.id, e.size or 0, kind and ('  (' .. kind .. ')') or ''), e.id)
    end)
    on('packet_in', function (e)
        local now = clock()
        rec.sec.pin = rec.sec.pin + 1
        local last = rec.pin_at
        if last ~= nil and now - last > IN_GAP then
            add(rec.d.pin_gap, (now - last) * 1000)
            rec.log(now, 'game.in', string.format('no packet from the server for %.0f ms (this one is 0x%03X)', (now - last) * 1000, e.id or 0))
        end
        rec.pin_at = now
    end)
    for _, name in ipairs({ 'menu_open', 'menu_close' }) do
        on(name, function (e) rec.reply(clock(), 'game.state', name .. ' ' .. tostring(e.name)) end)
    end
    for _, name in ipairs({ 'main_menu_open', 'main_menu_close', 'action_menu_open', 'action_menu_close', 'chat_input_open',
                            'chat_input_close' }) do
        on(name, function () rec.reply(clock(), 'game.state', name) end)
    end
    on('target_change', function (e) rec.reply(clock(), 'game.state', 'target_change to ' .. tostring(e.index)) end)
end

-- what the settings and the game were set to: the usual reasons for input lag
local function header(now)
    local out = {}
    local function line(text) out[#out + 1] = text end
    line('VanaCore gamepad recording  ' .. os.date('%Y-%m-%d %H:%M:%S'))
    line('')
    line('The first column is milliseconds since the recording started (VanaCore\'s clock). Tags:')
    line('  pad.poll   the controller read straight from Windows, once a frame (at its end)')
    line('  game.pad   the game\'s own read of a controller button (XInput or DirectInput)')
    line('  key.msg    a key message reaching the game\'s window, with how long it sat in the queue')
    line('  game.key   the game\'s own DirectInput read of a key')
    line('  game.read  a long gap between the game\'s input reads (a slow frame, or the game polling rarely)')
    line('  game.out   a packet the game sent (position packets are noise; the first other one after an input is its answer)')
    line('  game.state a window, the chat line or the target changing')
    line('  game.in    no packet from the server for a while (network or server lag)')
    line('  frame      a slow frame (over 30 ms), and "second" a summary of each second')
    line('Delays are shown as "+N ms after ..." on the later line. The summary and what it means are at the end.')
    line('')
    for _, pair in ipairs({ { 'client', 'fps.max' }, { 'client', 'fps.background' }, { 'client', 'framepacing' }, { 'client', 'packetflow' },
                            { 'graphics', 'vsync' }, { 'input', 'gamepad.searchinterval' }, { 'input', 'gamepad.disable' },
                            { 'input', 'gamepad.disableenumeration' }, { 'ffxi', 'gamepad.mode' }, { 'ffxi', 'gamepad.guid' } }) do
        local ok, value = pcall(vana.config.get, pair[1], pair[2])
        line(string.format('setting [%s] %s = %s', pair[1], pair[2], ok and tostring(value) or '?'))
    end
    local ok, info = pcall(vana.graphics.info)
    if ok and type(info) == 'table' then
        local parts = {}
        for key, value in pairs(info) do
            local kind = type(value)
            if kind == 'string' or kind == 'number' or kind == 'boolean' then
                parts[#parts + 1] = key .. '=' .. tostring(value)
            end
        end
        table.sort(parts)
        line('graphics ' .. table.concat(parts, ' '):sub(1, 600))
    end
    local ok2, pads, xinput = pcall(vana.gamepad.list, false)
    if ok2 then
        for _, pad in ipairs(pads) do
            line(string.format('game controller  %s  %s  %s', pad.name, pad.guid, pad.opened and '(the game has it open)' or '(not open in the game)'))
        end
    end
    for _, s in ipairs(sources.list) do
        if s.kind == 'x' then
            line(string.format('polled  XInput %d  %s, %s, battery %s', s.slot + 1, x_type(s), (s.flags ~= nil and band(s.flags, 2) ~= 0) and 'wireless' or 'wired', battery_text(s)))
        else
            line(string.format('polled  joy %d  %s  VID %04X PID %04X  %d buttons, %d axes', s.id + 1, s.name, s.vid or 0, s.pid or 0, s.buttons_n or 0, s.axes_n or 0))
        end
    end
    line('')
    return out
end

local function summary(now, why)
    local d, stat = rec.d, rec.stat
    local seconds = now - rec.t0
    local out = {}
    local function line(text) out[#out + 1] = text end
    line('')
    line(string.rep('-', 78))
    line(string.format('SUMMARY  %.1f s (%s)', seconds, why))
    line(string.rep('-', 78))
    line('1. Windows to the game')
    line('   controller read (pad.poll) to the game\'s read of it : ' .. describe(d.pad_game))
    line('   key message queue age (OS to the game\'s window)     : ' .. describe(d.key_age))
    line('   key message to the game\'s DirectInput key read       : ' .. describe(d.key_game))
    line('   controller presses over before the game read them    : ' .. stat.short)
    line('   controller presses the game never read (2 s)         : ' .. stat.lost)
    line('   the game\'s reads raised twice (left out of the log)  : ' .. stat.dups)
    line('   the game reads input, after the last present         : ' .. describe(d.phase))
    line(string.format('   game input read gaps over %3d ms (1.6 x the frames)   : %d, longest %.0f ms', slow_after(READ_GAP) * 1000, #d.read_gap, biggest(d.read_gap)))
    line('2. Inside the game')
    line('   input to its first answer (a packet but not position, or a window or the target changing): ' .. describe(d.game_out))
    line('3. The server')
    line(string.format('   gaps over %d ms with no packet in                   : %d, longest %.0f ms', IN_GAP * 1000, #d.pin_gap, biggest(d.pin_gap)))
    line('Frames')
    line(string.format('   %d frames, average %.1f ms (%.0f FPS), slowest %.1f ms, %d slow (over 1.6 x the frames lately)', stat.frames,
            stat.frames > 0 and stat.frame_sum / stat.frames * 1000 or 0, stat.frame_sum > 0 and stat.frames / stat.frame_sum or 0,
            stat.frame_max * 1000, stat.hitches))
    line('   Input is read once a frame, so a press waits up to a frame for the game to see it (half of one, on average).')
    line(string.format('   This recording\'s own cost: looking for controllers up to %.1f ms once a second, reading them up to %.1f ms a frame.',
            rec.cost.scan * 1000, rec.cost.poll * 1000))
    line('')
    line('What it says')
    local frame_ms = (rec.avg or 0.016) * 1000
    local late = biggest(d.pad_game)
    if #d.pad_game == 0 then
        line('   No controller press was matched with the game\'s read of it: the game did not read the controller (is it the one in use? the')
        line('   game was in the background?), or no button was pressed.')
    elseif late > math.max(50, frame_ms * 2) then
        line(string.format('   Some presses reached the game late: up to %.0f ms between Windows having the press and the game reading it (a frame is %.0f ms).', late, frame_ms))
        line('   That is lag getting the input to the game (see the slow frames and the game.read gaps near those lines).')
    else
        line(string.format('   Presses were read within %.0f ms of Windows having them (a frame is %.0f ms): the wait is the frame itself, not a delay.', late, frame_ms))
    end
    if #d.phase > 0 then
        local sorted = {}
        for i, v in ipairs(d.phase) do sorted[i] = v end
        table.sort(sorted)
        line(string.format('   The game read its input %.0f ms after the last present in a %.0f ms frame. What a press does is drawn in the next frame,', sorted[math.ceil(#sorted / 2)], frame_ms))
        line('   so press to picture is at best the wait for the read, a frame, and the frame generation of the graphics line above.')
    end
    if biggest(d.key_age) > 50 then
        line(string.format('   Key messages sat up to %.0f ms in Windows\' queue before the game\'s window got them.', biggest(d.key_age)))
    end
    if biggest(d.key_game) > 50 then
        line(string.format('   The game read a key up to %.0f ms after its message arrived: lag inside the game\'s input handling.', biggest(d.key_game)))
    end
    local slow = biggest(d.game_out)
    if #d.game_out == 0 then
        line('   No packet followed an input: nothing was sent to the server for the presses made (a menu press sends none).')
    elseif slow > 150 then
        line(string.format('   The game took up to %.0f ms after an input to send its answer: lag inside the game (or a press that sent nothing, answered by the next packet).', slow))
    else
        line(string.format('   The game answered an input within %.0f ms: processing inside the game was quick.', slow))
    end
    if #d.pin_gap > 0 then
        line(string.format('   The server went quiet %d time(s), up to %.0f ms: that is the network or the server, not the input.', #d.pin_gap, biggest(d.pin_gap)))
    end
    return out
end

local function rec_write(lines)
    if rec.file ~= nil then
        rec.file:write(table.concat(lines, '\n'), '\n')
    end
end

function rec.start(now)
    local a = load_api()
    if not a.ok then
        print('VanaCore: the gamepad recording needs Windows\' controller functions: ' .. tostring(a.why))
        return
    end
    local dir = vana.paths.install .. '\\logs'
    vana.fs.create_dir(dir)
    local path = dir .. '\\gamepad-' .. os.date('%Y%m%d-%H%M%S') .. '.log'
    local file, why = io.open(path, 'w')
    if file == nil then
        print('VanaCore: the gamepad recording could not open ' .. path .. ': ' .. tostring(why))
        return
    end
    rec.file, rec.path = file, path
    rec.t0, rec.buf, rec.lines, rec.dt, rec.error = now, {}, 0, 0.016, nil
    rec.ctl, rec.key_pending, rec.last_input = {}, {}, nil
    rec.trigger_on, rec.stick_on, rec.axis_on, rec.axis_base, rec.hat_dirs = {}, {}, {}, {}, {}
    rec.cost, rec.focused, rec.poll_frame = { scan = 0, poll = 0 }, nil, -1
    rec.keys, rec.kread_at, rec.xread_at, rec.pin_at, rec.frame_at, rec.avg = nil, nil, nil, nil, nil, nil
    rec.d = { pad_game = {}, game_out = {}, key_age = {}, key_game = {}, read_gap = {}, pin_gap = {}, phase = {} }
    rec.stat = { frames = 0, frame_sum = 0, frame_max = 0, hitches = 0, short = 0, lost = 0, dups = 0 }
    rec.sec = { at = now, frames = 0, sum = 0, max = 0, kreads = 0, xreads = 0, pin = 0, pout = 0 }
    rec.held_before = gt.hold
    gt.hold = false -- the game has to see the controller to be timed
    sources.scanned = -100
    sources.polled_at = nil
    refresh(now)
    rec_write(header(now))
    install()
    rec.on, gt.recording = true, true
    rec.saved = nil
    print('VanaCore: recording the gamepad test to ' .. path .. ' (Stop in the Gamepad Test window, or /gamepad record)')
end

function rec.stop(now, why)
    if not rec.on then
        return
    end
    for _, name in ipairs(REC_EVENTS) do
        pcall(vana.events.off, name, REC_KEY)
    end
    rec.on, gt.recording = false, false
    for c, st in pairs(rec.ctl) do
        if st.poll_t ~= nil and st.game_t == nil and not st.lost then
            rec.log(now, 'pad.poll', string.format('%s was pressed %.1f ms before the end and the game had not read it', c, (now - st.poll_t) * 1000))
        end
    end
    rec_flush()
    rec_write(summary(now, why))
    rec.file:close()
    rec.file = nil
    gt.hold = rec.held_before
    rec.saved, rec.saved_at = rec.path, now
    print('VanaCore: gamepad recording saved to ' .. rec.path)
end

function M.gamepadtest_record_toggle()
    local now = vana.time.clock()
    if rec.on then
        rec.stop(now, 'stopped')
    else
        rec.start(now)
    end
end

-- once a frame while recording: the frame's time, a summary of each second, what was never read, the file kept up to date
function M.gamepadtest_tick(now)
    if not rec.on then
        return
    end
    refresh(now)
    local stat, sec = rec.stat, rec.sec
    if rec.frame_at ~= nil then -- (the first frame has no time yet)
        local dt = now - rec.frame_at
        rec.dt = dt
        stat.frames, stat.frame_sum, stat.frame_max = stat.frames + 1, stat.frame_sum + dt, math.max(stat.frame_max, dt)
        sec.frames, sec.sum, sec.max = sec.frames + 1, sec.sum + dt, math.max(sec.max, dt)
        local usual = rec.avg or dt
        if dt > slow_after(HITCH) then
            stat.hitches = stat.hitches + 1
            rec.log(now, 'frame', string.format('slow frame: %.1f ms (the frames lately: %.1f ms)', dt * 1000, usual * 1000))
            rec.avg = usual + (dt - usual) * 0.01 -- (a game that slowed for good is the new usual, slowly)
        else
            rec.avg = usual + (dt - usual) * 0.05
        end
    end
    rec.frame_at = now
    for c, st in pairs(rec.ctl) do
        if st.poll_t ~= nil and st.game_t == nil and not st.lost and now - st.poll_t > 2 then
            st.lost = true
            stat.lost = stat.lost + 1
            rec.log(now, 'pad.poll', string.format('%s still held 2 s after the press and the game never read it', c))
        end
    end
    local ok, focused = pcall(vana.system.focused)
    if ok and focused ~= rec.focused then
        rec.focused = focused
        rec.log(now, 'game.focus', focused and 'the game window is in front' or 'the game window is in the background (it runs slower there, and reads its controller less)')
    end
    if now - sec.at >= 1 then
        rec.log(now, 'second', string.format('%d frames (average %.1f ms, slowest %.1f)   game input reads %d (keyboard) %d (XInput)   packets in %d, out %d   %s',
                sec.frames, sec.frames > 0 and sec.sum / sec.frames * 1000 or 0, sec.max * 1000, sec.kreads, sec.xreads, sec.pin, sec.pout,
                rec.focused and 'in front' or 'in the background'))
        for _, s in ipairs(sources.list) do
            if s.rate > 0 then
                rec.log(now, 'second', string.format('%s polled: %d changes/s', s.name, math.floor(s.rate + 0.5)))
            end
        end
        rec.sec = { at = now, frames = 0, sum = 0, max = 0, kreads = 0, xreads = 0, pin = 0, pout = 0 }
        rec_flush()
    end
    if now - rec.t0 > REC_MAX_SECONDS or rec.lines > REC_MAX_LINES then
        rec.stop(now, 'the recording\'s limit: ' .. REC_MAX_SECONDS .. ' s or ' .. REC_MAX_LINES .. ' lines')
    end
end

vana.events.on('unload', function ()
    if rec.on then
        rec.stop(vana.time.clock(), 'the window was reloaded')
    end
end, 'gamepadtest.unload')

-- the one shown: the picked, else an XInput pad, else a joystick that has buttons, else the first
local function picked()
    local list = sources.list
    for _, s in ipairs(list) do
        if s.key == gt.pick then
            return s
        end
    end
    for _, s in ipairs(list) do
        if s.kind == 'x' then
            gt.pick = s.key
            return s
        end
    end
    for _, s in ipairs(list) do
        if (s.buttons_n or 0) > 0 then
            gt.pick = s.key
            return s
        end
    end
    gt.pick = list[1] and list[1].key or nil
    return list[1]
end

local function reset(s)
    s.count, s.history, s.peak, s.axmin, s.axmax = {}, {}, {}, {}, {}
    s.last_input, s.base = nil, nil
    for label in pairs(s.down) do
        s.down[label] = nil
    end
end

-------------------------------------------------------------------------------
-- Words
-------------------------------------------------------------------------------

local function ago(now, at)
    if at == nil then
        return 'none yet'
    end
    local seconds = now - at
    if seconds < 1 then
        return 'now'
    elseif seconds < 60 then
        return string.format('%d s ago', math.floor(seconds))
    end
    return string.format('%d min ago', math.floor(seconds / 60))
end

local function percent(fraction)
    return string.format('%d%%', math.floor(fraction * 100 + 0.5))
end

function x_type(s)
    if s.subtype == nil then
        return 'XInput controller'
    end
    return X_SUBTYPES[s.subtype] or ('Controller (type ' .. s.subtype .. ')')
end

function battery_text(s)
    if s.battery == nil then
        return 'unknown'
    end
    local kind, level = s.battery[1], s.battery[2]
    if kind == 1 then
        return 'wired'
    elseif kind == 0 or kind == 255 then
        return kind == 0 and 'none' or 'unknown'
    end
    return (BATTERY_LEVELS[level] or '?') .. ' (' .. (BATTERY_TYPES[kind] or '?') .. ')'
end

local function stick_text(x, y, dead)
    local m = stick_magnitude(x, y)
    return string.format('%d, %d  (%s)%s', x, y, percent(m), m * 32767 <= dead and '  dead zone' or '')
end

local function axes_of(s)
    local list = {}
    if s.axes_n == 0 then
        return list -- it says it has none (a keyboard's dongle that Windows lists as a controller)
    end
    for i, axis in ipairs(AXES) do
        if i <= 2 or (s.caps ~= nil and band(s.caps, axis[2]) ~= 0) then -- (the first two have no bit)
            list[#list + 1] = i
        end
    end
    return list
end

local function x_stats(s, now)
    local rows = {}
    local function add(label, text, colour)
        rows[#rows + 1] = { label, text, colour }
    end
    add('Type', x_type(s))
    add('Connection', (s.flags ~= nil and band(s.flags, 2) ~= 0) and 'Wireless' or 'Wired')
    add('Battery', battery_text(s))
    add('Force feedback', (s.flags ~= nil and band(s.flags, 1) ~= 0) and 'Yes' or 'No')
    add('Reports', string.format('%d / s', math.floor(s.rate + 0.5)))
    add('Packet', string.format('%d', s.packet))
    add('Left stick', stick_text(s.lx, s.ly, DEAD_L))
    add('Right stick', stick_text(s.rx, s.ry, DEAD_R))
    add('Triggers', string.format('LT %d    RT %d', s.lt, s.rt))
    add('Peaks', string.format('L %s  R %s  LT %d  RT %d', percent(s.peak.l or 0), percent(s.peak.r or 0),
                               s.peak.lt or 0, s.peak.rt or 0))
    add('Last input', ago(now, s.last_input))
    return rows
end

local function j_stats(s, now)
    local rows = {}
    local function add(label, text)
        rows[#rows + 1] = { label, text }
    end
    add('Joystick', string.format('%d of Windows\' list', (s.id or 0) + 1))
    add('USB id', s.vid ~= nil and string.format('VID %04X   PID %04X', s.vid, s.pid) or 'unknown')
    add('Buttons', string.format('%d reported', s.buttons_n or 0))
    add('Axes', string.format('%d reported%s', s.axes_n or 0, (s.caps ~= nil and band(s.caps, 16) ~= 0) and ', a hat' or ''))
    add('Reports', string.format('%d changes / s', math.floor(s.rate + 0.5)))
    add('Last input', ago(now, s.last_input))
    return rows
end

-------------------------------------------------------------------------------
-- Drawing
-------------------------------------------------------------------------------

local function chip(dl, x, y, w, h, label, on, colour, size)
    if on then
        M.fill(dl, x, y, w, h, colour or P.accent, S(4))
        M.text_centred(dl, x, y, w, h, label, P.ink, fonts.semi, size or SMALL)
    else
        M.fill(dl, x, y, w, h, P.well, S(4))
        M.outline(dl, x, y, w, h, P.well_edge, S(4))
        M.text_centred(dl, x, y, w, h, label, P.dim, fonts.text, size or SMALL)
    end
end

local function disc(dl, cx, cy, r, label, on, colour)
    if on then
        M.circle(dl, cx, cy, r, colour)
        M.text_centred(dl, cx - r, cy - r, r * 2, r * 2, label, P.ink, fonts.semi, BODY)
    else
        M.circle(dl, cx, cy, r, P.well)
        dl:AddCircle({ cx, cy }, r, M.u32(M.mix(P.well_edge, colour, 0.55)), 0, 1.5)
        M.text_centred(dl, cx - r, cy - r, r * 2, r * 2, label, M.mix(P.dim, colour, 0.5), fonts.semi, BODY)
    end
end

-- a stick: its ring, the dead zone inside it and the thumb where the stick is (x and y -32768..32767)
local function stick_draw(dl, cx, cy, r, x, y, dead, pressed)
    local nx, ny = x / 32767, y / 32767
    local m = math.sqrt(nx * nx + ny * ny)
    if m > 1 then
        nx, ny, m = nx / m, ny / m, 1
    end
    dl:AddCircleFilled({ cx, cy }, r, M.u32(pressed and M.mix(P.well, P.accent, 0.35) or P.well), 0)
    dl:AddCircle({ cx, cy }, r, M.u32(pressed and P.accent or P.well_edge), 0, 1.5)
    dl:AddCircle({ cx, cy }, r * dead, M.u32(P.faint, 0.6), 0, 1)
    local reach = r - S(9)
    local px, py = cx + nx * reach, cy - ny * reach
    local live = m > dead
    if live then
        dl:AddLine({ cx, cy }, { px, py }, M.u32(P.accent_lo), 1.5)
    end
    M.circle(dl, px, py, S(8), live and P.accent or P.dim)
end

local function draw_diagram(dl, x, y, s)
    local function at(ux, uy)
        return x + S(ux), y + S(uy)
    end
    local buttons = s.buttons
    local function down(mask)
        return band(buttons, mask) ~= 0
    end
    local bx, by = at(0, 44)
    M.fill(dl, bx, by, S(DIAGRAM_W), S(DIAGRAM_H - 44), P.well, S(46))
    M.outline(dl, bx, by, S(DIAGRAM_W), S(DIAGRAM_H - 44), P.well_edge, S(46), 0, 1.5)
    -- triggers: a meter each, with the bumper under it
    for side = 0, 1 do
        local ux = side == 0 and 22 or 190
        local value = side == 0 and s.lt or s.rt
        local tx, ty = at(ux, 0)
        M.fill(dl, tx, ty, S(84), S(16), P.off, S(5))
        if value > 0 then
            M.fill(dl, tx, ty, math.max(S(10), S(84) * value / 255), S(16), P.accent, S(5))
        end
        M.text_centred(dl, tx, ty, S(84), S(16), (side == 0 and 'LT ' or 'RT ') .. value, value > 0 and P.white or P.dim, fonts.text, SMALL - 1)
        local kx, ky = at(ux, 22)
        local mask = side == 0 and 0x0100 or 0x0200
        chip(dl, kx, ky, S(84), S(20), side == 0 and 'LB' or 'RB', down(mask), P.accent)
    end
    -- the sticks (pressed in lights the ring)
    local lx, ly = at(74, 100)
    stick_draw(dl, lx, ly, S(30), s.lx, s.ly, DEAD_L / 32767, down(0x0040))
    local rx, ry = at(186, 170)
    stick_draw(dl, rx, ry, S(30), s.rx, s.ry, DEAD_R / 32767, down(0x0080))
    -- the D-pad
    local dx, dy = at(112, 170)
    local arm_w, arm_l = S(18), S(20)
    M.fill(dl, dx - arm_w / 2, dy - arm_w / 2, arm_w, arm_w, P.well_edge, S(3))
    local function arm(ax, ay, aw, ah, mask)
        M.fill(dl, ax, ay, aw, ah, down(mask) and P.accent or P.seg, S(3))
    end
    arm(dx - arm_w / 2, dy - arm_w / 2 - arm_l, arm_w, arm_l, 0x0001)
    arm(dx - arm_w / 2, dy + arm_w / 2, arm_w, arm_l, 0x0002)
    arm(dx - arm_w / 2 - arm_l, dy - arm_w / 2, arm_l, arm_w, 0x0004)
    arm(dx + arm_w / 2, dy - arm_w / 2, arm_l, arm_w, 0x0008)
    -- Back and Start
    local kx, ky = at(108, 70)
    chip(dl, kx, ky, S(34), S(16), 'Back', down(0x0020), P.accent, SMALL - 1)
    kx, ky = at(150, 70)
    chip(dl, kx, ky, S(34), S(16), 'Start', down(0x0010), P.accent, SMALL - 1)
    -- the face buttons, in Xbox's colours
    local fx, fy = at(222, 98)
    local r = S(11)
    disc(dl, fx, fy - S(24), r, 'Y', down(0x8000), P.amber)
    disc(dl, fx, fy + S(24), r, 'A', down(0x1000), P.green)
    disc(dl, fx - S(24), fy, r, 'X', down(0x4000), P.accent)
    disc(dl, fx + S(24), fy, r, 'B', down(0x2000), P.rose)
end

local function draw_stats(dl, x, y, w, rows)
    for i, row in ipairs(rows) do
        local cy = y + S(STAT_H) * (i - 0.5)
        M.text_mid(dl, x, cy, row[1], P.dim, fonts.text, SMALL + 0.5)
        local room = w - M.measure(row[1], fonts.text, SMALL + 0.5) - S(14)
        M.text_right_mid(dl, x + w, cy, M.fit(row[2], room, fonts.text, SMALL + 0.5), row[3] or P.text, fonts.text, SMALL + 0.5)
        if i < #rows then
            M.hline(dl, x, x + w, y + S(STAT_H) * i, P.line, 0.6)
        end
    end
end

local function section_title(dl, x, y, label)
    M.text_mid(dl, x, y + S(HEAD_H) / 2 + S(2), label, P.faint, fonts.semi, SMALL - 1.5)
end

local function x_body_h()
    return DIAGRAM_H + 10
end

local function draw_x_body(dl, x, y, w, s, now)
    draw_diagram(dl, x, y, s)
    local sx = x + S(DIAGRAM_W) + S(24)
    draw_stats(dl, sx, y, x + w - sx, x_stats(s, now))
end

local function j_body_h(s)
    local n = math.min(32, s.buttons_n or 0)
    local grid = math.max(1, math.ceil(n / 8))
    local left = HEAD_H + grid * 30 + 10 + HEAD_H + 80
    local right = HEAD_H + math.max(#axes_of(s), s.axes_n == 0 and 1 or 0) * 28 + 8 + HEAD_H + 6 * STAT_H
    return math.max(left, right, 200)
end

local function draw_hat(dl, cx, cy, r, s)
    local pov = s.pov
    local centred = pov == 65535 or pov > 35999
    dl:AddCircle({ cx, cy }, r, M.u32(P.well_edge), 0, 1.5)
    local lit = (not centred) and (math.floor((pov / 100 + 22.5) / 45) % 8) or nil
    for i = 0, 7 do
        local angle = math.rad(i * 45)
        local px, py = cx + math.sin(angle) * (r - S(7)), cy - math.cos(angle) * (r - S(7))
        M.circle(dl, px, py, S(4), lit == i and P.accent or P.seg)
    end
    M.circle(dl, cx, cy, S(5), centred and P.dim or P.accent)
end

local function draw_j_body(dl, x, y, w, s, now)
    local left_w = S(300)
    -- the buttons
    section_title(dl, x, y, 'BUTTONS')
    local n = math.min(32, s.buttons_n or 0)
    local gy = y + S(HEAD_H)
    if n == 0 then
        M.text_mid(dl, x, gy + S(14), 'It reports no buttons', P.dim, fonts.text, SMALL + 0.5)
        gy = gy + S(30)
    else
        for i = 0, n - 1 do
            local cx = x + (i % 8) * S(38)
            local cy = gy + math.floor(i / 8) * S(30)
            chip(dl, cx, cy, S(34), S(26), tostring(i + 1), band(s.buttons, bit.lshift(1, i)) ~= 0, P.accent, SMALL)
        end
        gy = gy + math.ceil(n / 8) * S(30)
    end
    -- the hat
    gy = gy + S(10)
    section_title(dl, x, gy - S(2), 'HAT')
    local hy = gy + S(HEAD_H) + S(30)
    if s.caps ~= nil and band(s.caps, 16) ~= 0 then
        draw_hat(dl, x + S(30), hy, S(28), s)
        local centred = s.pov == 65535 or s.pov > 35999
        local words = centred and 'centred' or string.format('%s  (%d°)', HATS[math.floor((s.pov / 100 + 22.5) / 45) % 8 + 1], math.floor(s.pov / 100 + 0.5))
        M.text_mid(dl, x + S(74), hy, words, centred and P.dim or P.text, fonts.text, BODY - 0.5)
    else
        M.text_mid(dl, x, hy, 'It has no hat', P.dim, fonts.text, SMALL + 0.5)
    end
    -- the axes
    local rx = x + left_w + S(24)
    local rw = x + w - rx
    section_title(dl, rx, y, 'AXES')
    local ay = y + S(HEAD_H)
    if s.axes_n == 0 then
        M.text_mid(dl, rx, ay + S(14), 'It reports no axes', P.dim, fonts.text, SMALL + 0.5)
        ay = ay + S(28)
    end
    for _, i in ipairs(axes_of(s)) do
        local cy = ay + S(14)
        local value = s.axes[i]
        local range = s.range ~= nil and s.range[i] or { 0, 65535 }
        local span = math.max(1, range[2] - range[1])
        local frac = math.max(0, math.min(1, (value - range[1]) / span))
        local base = s.base ~= nil and ((s.base[i] - range[1]) / span) or 0
        local track_x, track_w = rx + S(22), rw - S(22) - S(52)
        M.text_mid(dl, rx, cy, AXES[i][1], P.dim, fonts.semi, SMALL)
        M.fill(dl, track_x, cy - S(3), track_w, S(6), P.off, S(3))
        if base > 0.35 and base < 0.65 then -- a stick: filled out from the middle
            local from, to = math.min(frac, 0.5), math.max(frac, 0.5)
            M.fill(dl, track_x + track_w * from, cy - S(3), math.max(S(2), track_w * (to - from)), S(6), P.accent, S(3))
        else -- a trigger or a slider: filled from its end
            M.fill(dl, track_x, cy - S(3), math.max(S(6), track_w * frac), S(6), P.accent, S(3))
        end
        M.circle(dl, track_x + track_w * frac, cy, S(5), P.knob)
        M.text_right_mid(dl, rx + rw, cy, string.format('%d', value), P.text, fonts.text, SMALL)
        ay = ay + S(28)
    end
    ay = ay + S(8)
    section_title(dl, rx, ay - S(2), 'STATS')
    draw_stats(dl, rx, ay + S(HEAD_H) - S(4), rw, j_stats(s, now))
end

-- what's held, and the last presses
local function draw_foot(dl, x, y, w, s, now)
    local half = w / 2 - S(12)
    section_title(dl, x, y, 'HELD NOW')
    local cx, cy = x, y + S(HEAD_H)
    local rows = 1
    if #s.held == 0 then
        M.text_mid(dl, x, cy + S(11), 'Nothing', P.faint, fonts.text, SMALL + 0.5)
    end
    for _, label in ipairs(s.held) do
        local cw = M.measure(label, fonts.semi, SMALL) + S(16)
        if cx + cw > x + half then
            cx, cy, rows = x, cy + S(26), rows + 1
        end
        if rows > 3 then
            break
        end
        chip(dl, cx, cy, cw, S(22), label, true, P.accent, SMALL)
        cx = cx + cw + S(6)
    end
    local hx = x + w / 2 + S(12)
    section_title(dl, hx, y, 'LAST PRESSES')
    for i, entry in ipairs(s.history) do
        if i > 5 then
            break
        end
        local ly = y + S(HEAD_H) + (i - 0.5) * S(NOTE_H)
        local holding = entry.ended == nil
        local count = s.count[entry.label] or 1
        M.text_mid(dl, hx, ly, entry.label, P.text, fonts.text, SMALL + 0.5)
        M.text_mid(dl, hx + S(96), ly, 'x' .. count, P.faint, fonts.text, SMALL)
        local when = holding and 'holding' or string.format('%.1f s', entry.ended - entry.at)
        M.text_right_mid(dl, x + w, ly, when, holding and P.teal or P.dim, fonts.text, SMALL)
    end
    if #s.history == 0 then
        M.text_mid(dl, hx, y + S(HEAD_H) + S(NOTE_H) / 2, 'Press a button', P.faint, fonts.text, SMALL + 0.5)
    end
end

local function header_words(s)
    if s.kind == 'x' then
        local words = x_type(s)
        if s.flags ~= nil then
            words = words .. ' · ' .. (band(s.flags, 2) ~= 0 and 'wireless' or 'wired')
        end
        return words .. ' · ' .. 'XInput slot ' .. (s.slot + 1)
    end
    local words = s.vid ~= nil and string.format('VID %04X · PID %04X', s.vid, s.pid) or 'Windows joystick'
    return words .. string.format(' · %d buttons · %d axes', s.buttons_n or 0, s.axes_n or 0)
end

local function window_height(s)
    local ph = TITLE_H + ROW_H + HOLD_H
    if s == nil then
        return ph + 120 + PAD
    end
    ph = ph + HEADER_H + (s.kind == 'x' and x_body_h() or j_body_h(s)) + FOOT_H + PAD
    return ph
end

function M.gamepadtest_toggle()
    gt.open = not gt.open
    if gt.open then
        sources.scanned = -100
    end
end

function M.draw_gamepadtest(now)
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
    -- the game keeps the controller at rest while this window has it
    if gt.hold and vana.gamepad ~= nil and vana.gamepad.nav ~= nil then
        pcall(vana.gamepad.nav, true)
    end
    refresh(now)
    local s = picked()
    local a = load_api()

    local pw = S(WIN_W)
    local ph = S(window_height(s))
    local dw, _, k = M.display(screen_w, screen_h) -- ImGui's pixels, where it's drawn k times its size
    imgui.SetNextWindowPos({ S(60) * k, S(80) * k }, M.FIRST_USE_EVER)
    imgui.SetNextWindowSize({ pw, ph }, 0)
    local VAR = M.VAR
    imgui.PushStyleVar(VAR.WindowPadding, { 0, 0 })
    imgui.PushStyleVar(VAR.WindowBorderSize, 0)
    imgui.PushStyleVar(VAR.ItemSpacing, { 0, 0 })
    if imgui.Begin('Gamepad Test###vana_gamepadtest', nil, M.WINDOW_FLAGS) then
        local x, y = imgui.GetWindowPos()
        local dl = imgui.GetWindowDrawList()
        local radius = S(6)
        M.fill(dl, x - 1, y, pw + 2, ph + 2, '#000000', radius + 1, 0, 0.35)
        M.fill(dl, x, y, pw, ph, P.bg, radius)
        M.outline(dl, x, y, pw, ph, P.border, radius)

        local lx, w = x + S(PAD), pw - S(PAD) * 2
        local cy = y + S(TITLE_H) / 2
        M.icon_centred(dl, lx + S(5), cy, 'gamepad', P.accent, 11)
        M.text_mid(dl, lx + S(18), cy, 'Gamepad Test', P.white, fonts.semi, BODY + 2)
        local right_text = x + pw - S(PAD) - S(22)
        if rec.on then
            local text = string.format('Recording %d:%02d  ·  %d lines', math.floor((now - rec.t0) / 60), math.floor(now - rec.t0) % 60, rec.lines)
            M.text_right_mid(dl, right_text, cy + S(1), text, P.rose, fonts.text, SMALL)
            M.circle(dl, right_text - M.measure(text, fonts.text, SMALL) - S(9), cy + S(1), S(3), P.rose, 0.5 + 0.5 * math.abs(math.sin(now * 3)))
        elseif rec.saved ~= nil then
            local name = rec.saved:match('[^\\]+$') or rec.saved
            M.text_right_mid(dl, right_text, cy + S(1), M.fit('Saved logs\\' .. name, S(WIN_W) - S(240), fonts.text, SMALL), P.teal, fonts.text, SMALL)
        else
            M.text_right_mid(dl, right_text, cy + S(1), '/gamepad test', P.faint, fonts.text, SMALL)
        end
        if M.icon_button('##gpt.close', x + pw - S(PAD), cy, 'close', 11, P.rose) then
            gt.open = false
        end
        M.hline(dl, x, x + pw, y + S(TITLE_H), P.line)

        -- the picker, Reset and Rescan at the right
        local py = y + S(TITLE_H) + S(ROW_H) / 2
        local rescan_w, reset_w = M.button_width('Rescan', 'reload'), M.button_width('Reset', 'reset')
        local record_label, record_icon = rec.on and 'Stop' or 'Record', rec.on and 'stop' or 'record'
        local record_w = M.button_width(record_label, record_icon)
        local right = lx + w
        if M.button('##gpt.rescan', right - rescan_w, py, 'Rescan', 'reload', 'ghost') then
            sources.scanned = -100
        end
        if imgui.IsItemHovered() then
            M.tooltip('Looks for controllers again, as plugging one in does')
        end
        right = right - rescan_w - S(8)
        if M.button('##gpt.reset', right - reset_w, py, 'Reset', 'reset', 'ghost', s ~= nil) then
            reset(s)
        end
        if imgui.IsItemHovered() then
            M.tooltip("Clears this controller's presses, counts and peaks")
        end
        right = right - reset_w - S(8)
        if M.button('##gpt.record', right - record_w, py, record_label, record_icon, rec.on and 'danger' or 'ghost') then
            M.gamepadtest_record_toggle()
        end
        if imgui.IsItemHovered() then
            M.tooltip(rec.on and 'Stops, and saves what it recorded with a summary at the end of the file'
                      or 'Writes the buttons, sticks and keys pressed, and when the game read and answered each, to logs\\gamepad-<time>.log: for finding out where input lag is (getting to the game, inside it, or the server).\nThe game gets the controller while it records. Keys the game gets are in the log.')
        end
        right = right - record_w - S(12)
        local options = {}
        for _, src in ipairs(sources.list) do
            options[#options + 1] = { src.key, (src.kind == 'x' and 'XInput ' .. (src.slot + 1) .. ': ' .. x_type(src) or src.name) }
        end
        if s ~= nil then
            local value, changed = M.dropdown('##gpt.pick', lx, py, math.min(right - lx, S(400)), options, s.key, true, 1, true, true)
            if changed then
                gt.pick = value
            end
        else
            M.text_mid(dl, lx, py, 'No controller', P.dim, fonts.text, BODY)
        end

        -- the game's hold
        local hy = y + S(TITLE_H) + S(ROW_H) + S(HOLD_H) / 2 - S(2)
        local on, flipped = M.switch('##gpt.hold', lx, hy, gt.hold, not rec.on)
        if flipped then
            gt.hold = on
        end
        M.text_mid(dl, lx + S(M.SW_W) + S(10), hy, rec.on and 'The game gets the controller while it records' or gt.hold
                   and 'The game ignores the controller while this is open' or 'The game still sees the controller',
                   (gt.hold and not rec.on) and P.text2 or P.amber, fonts.text, SMALL + 0.5)

        local sy = y + S(TITLE_H) + S(ROW_H) + S(HOLD_H)
        M.hline(dl, lx, lx + w, sy, P.line)
        if not a.ok or gt.error ~= nil then
            M.text_mid(dl, lx, sy + S(24), M.fit("Windows' controller functions failed: " .. tostring(a.ok and gt.error or a.why), w, fonts.text, SMALL + 0.5),
                       P.rose, fonts.text, SMALL + 0.5)
            if not a.ok then
                imgui.End()
                imgui.PopStyleVar(3)
                return
            end
        end
        if s == nil then
            M.icon_centred(dl, x + pw / 2, sy + S(40), 'gamepad', P.faint, 22)
            M.text_centred(dl, x, sy + S(62), pw, S(20), 'No controller found', P.text2, fonts.semi, BODY)
            M.text_centred(dl, x, sy + S(84), pw, S(20), 'Plug one in or turn it on: it shows up here at once.', P.dim, fonts.text, SMALL + 0.5)
        else
            -- who it is, and whether it's being used
            local hcy = sy + S(HEADER_H) / 2
            local active = s.last_input ~= nil and now - s.last_input < 1
            M.fill(dl, lx, hcy - S(15), S(30), S(30), active and P.accent_bg or P.well, S(7))
            M.outline(dl, lx, hcy - S(15), S(30), S(30), active and P.accent_lo or P.well_edge, S(7))
            M.icon_centred(dl, lx + S(15) + S(1.2), hcy + S(1.2), 'gamepad', active and P.accent or P.faint, 12) -- (nudged: the glyph sits left and high)
            local state_text = active and 'Active' or (s.last_input ~= nil and ('Idle · ' .. ago(now, s.last_input)) or 'Waiting for input')
            local state_w = M.measure(state_text, fonts.text, SMALL + 0.5)
            M.text_right_mid(dl, lx + w, hcy, state_text, active and P.teal or P.dim, fonts.text, SMALL + 0.5)
            M.circle(dl, lx + w - state_w - S(9), hcy, S(3), active and P.teal or P.faint)
            local room = w - S(44) - state_w - S(24)
            M.text_mid(dl, lx + S(42), hcy - S(9), M.fit(s.name, room, fonts.semi, BODY + 1), P.text, fonts.semi, BODY + 1)
            M.text_mid(dl, lx + S(42), hcy + S(10), M.fit(header_words(s), room, fonts.text, SMALL), P.dim, fonts.text, SMALL)

            local by = sy + S(HEADER_H)
            local body_h
            if s.kind == 'x' then
                body_h = x_body_h()
                draw_x_body(dl, lx, by, w, s, now)
            else
                body_h = j_body_h(s)
                draw_j_body(dl, lx, by, w, s, now)
            end
            local fy = by + S(body_h)
            M.hline(dl, lx, lx + w, fy, P.line)
            draw_foot(dl, lx, fy + S(4), w, s, now)
        end
    end
    imgui.End()
    imgui.PopStyleVar(3)
    -- a switch's or slider's writes: the /vana window's frame makes them when it's open
    if not st.open and next(M.pending) ~= nil then
        M.flush()
    end
end
