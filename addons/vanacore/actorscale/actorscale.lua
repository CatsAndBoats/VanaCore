--[[
actorscale: makes whatever you're targeting bigger or smaller, on your screen only. For fun.

In game:
  /addon load actorscale vanacore
  /scale             shows or hides the window: the target's name, a size slider and Reset
  /scale 2          the target twice its size (0.1 to 10)
  /scale up | down   a quarter bigger or smaller each time
  /scale reset       the target back to its own size
  /scale resetall    everything back
  /scale list        what's scaled

The size is the entity's model size (+0x204, -1 = the model's own). The game can set it again (a server update, a
respawn), so a scaled entity is held at its size every frame while it's the same server id in that slot. Zoning or
unloading puts everything back.
--]]

addon.name    = 'actorscale'
addon.author  = 'Vekien'
addon.version = '1.2'
addon.desc    = 'Scales the targeted actor up or down (client side only).'

local imgui = vana.imgui
local kModelSize = 0x204
local kMin, kMax, kStep = 0.1, 10.0, 1.25
-- NoCollapse, AlwaysAutoResize, NoFocusOnAppearing, NoNav: the window never holds the keyboard, so Tab, Enter and the
-- rest stay the game's
-- (not AlwaysAutoResize: the width is the player's, with the resize grip, not the target name's)
local kWindowFlags = 32 + 4096 + 65536 + 131072

local window = { false } -- Begin's p_open

local list_address -- the game's array of entity pointers
local scaled = {}  -- [index] = { id, name, size, original }

local function entity_pointer(index)
    if list_address == nil then
        local at = vana.memory.find('FFXiMain.dll', 0, '8B 56 0C 8B 04 2A 8B 04 85', 9)
        list_address = at and vana.memory.read_uint32(at) or 0
    end
    if list_address == 0 then return nil end
    local p = vana.memory.read_uint32(list_address + index * 4)
    if p == nil or p == 0 then return nil end
    return p
end

local function say(text)
    print('[actorscale] ' .. text)
end

local function restore(index)
    local s = scaled[index]
    scaled[index] = nil
    if s == nil then return end
    local e = vana.game.entity(index)
    local p = entity_pointer(index)
    if p and e and e.id == s.id then
        vana.memory.write_float(p + kModelSize, s.original)
    end
end

local function restore_all()
    for index in pairs(scaled) do restore(index) end
end

local function current_target(quiet)
    local t = vana.game.target()
    if not (t and t.active and t.index ~= 0) then
        if not quiet then say('Nothing targeted.') end
        return nil
    end
    local e = vana.game.entity(t.index)
    local p = entity_pointer(t.index)
    if e == nil or p == nil then
        if not quiet then say('The target has no actor.') end
        return nil
    end
    return t.index, e, p
end

-- The model's own scale. The game (FFXiMain.dll+0xCC740, building the actor's scale) takes each axis from the model
-- size when that's 0 or more, else from the actor's per-axis scale (+0x760 x, +0x75C y, +0x758 z) when that's 0 or
-- more, else the actor's one scale (+0x754: 0.92 for a Galka NPC, 0.95 most others, 40 a door). So the model size
-- REPLACES the model's scale: sizes here are times the model's own, written as own * factor. nil (and why) when the
-- actor isn't there (not drawn) or its axes differ (a door: one size would squash it).
local kActor, kActorBack = 0xA0, 0x70 -- entity -> actor; actor -> entity (checked)
local function own_scale(p)
    local v = vana.memory.read_float(p + kModelSize)
    if v and v >= 0 then return v end
    local a = vana.memory.read_uint32(p + kActor)
    if a == nil or a == 0 or vana.memory.read_uint32(a + kActorBack) ~= p then return nil, 'is not drawn' end
    for _, o in ipairs({ 0x758, 0x75C, 0x760 }) do
        if (vana.memory.read_float(a + o) or -1) >= 0 then return nil, 'has a shape of its own (one size would squash it)' end
    end
    local base = vana.memory.read_float(a + 0x754)
    if base == nil or base <= 0 then return nil, 'has no size' end
    return base
end

local function apply(index, e, p, factor, quiet)
    factor = math.max(kMin, math.min(kMax, factor))
    local s = scaled[index]
    if s == nil or s.id ~= e.id then
        local own, why = own_scale(p)
        if own == nil then
            if not quiet then say(e.name .. ' ' .. why .. '.') end
            return
        end
        s = { id = e.id, name = e.name, original = vana.memory.read_float(p + kModelSize), own = own }
        scaled[index] = s
    end
    s.size = factor
    vana.memory.write_float(p + kModelSize, s.own * factor)
    s.raw = vana.memory.read_float(p + kModelSize) -- as a float, so the frame check compares like with like
    if not quiet then say(string.format('%s: %.2fx', e.name, factor)) end
end

-- The target's size now, times its own (1 while unscaled)
local function size_now(index)
    local s = scaled[index]
    return s and s.size or 1.0
end

vana.events.on('command', function (e)
    local args = e.command:match('^/scale%s*(.*)$')
    if args == nil then return end
    args = args:lower():gsub('^%s+', ''):gsub('%s+$', '')

    if args == '' then
        window[1] = not window[1]
    elseif args == 'resetall' then
        restore_all()
        say('Everything back to its own size.')
    elseif args == 'list' then
        local any = false
        for index, s in pairs(scaled) do
            say(string.format('%s (index %d): %.2fx', s.name, index, s.size))
            any = true
        end
        if not any then say('Nothing scaled.') end
    elseif args == 'reset' then
        local index, ent = current_target()
        if index then
            restore(index)
            say(ent.name .. ': its own size.')
        end
    elseif args == 'up' or args == 'down' or tonumber(args) then
        local index, ent, p = current_target()
        if index then
            local size
            if args == 'up' then size = size_now(index) * kStep
            elseif args == 'down' then size = size_now(index) / kStep
            else size = tonumber(args) end
            apply(index, ent, p, size)
        end
    else
        say('/scale [<0.1-10> | up | down | reset | resetall | list]')
    end
    return true
end)

-- A slider drawn as one: a thin track filled up to a round knob, logarithmic (1x in the middle, as much room to shrink
-- as to grow), snapping to 1x near it. Returns changed, value.
local function size_slider(id, value)
    local fs = imgui.GetFontSize()
    -- as wide as the window leaves beside the value
    local avail = imgui.GetContentRegionAvail()
    local value_w = imgui.CalcTextSize('10.00x')
    local w, h = math.max(fs * 4, avail - value_w - imgui.GetStyle().ItemSpacing.x), fs * 1.4
    local x, y = imgui.GetCursorScreenPos()
    imgui.InvisibleButton(id, { w, h })
    local active, hovered = imgui.IsItemActive(), imgui.IsItemHovered()

    local r = h * 0.32
    local x0, x1, cy = x + r, x + w - r, y + h / 2
    local lo, hi = math.log(kMin), math.log(kMax)
    local changed = false
    if active then
        local mx = imgui.GetMousePos()
        local t = math.max(0, math.min(1, (mx - x0) / (x1 - x0)))
        local v = math.exp(lo + t * (hi - lo))
        if math.abs(v - 1) < 0.04 then v = 1 end
        if math.abs(v - value) > 1e-4 then value, changed = v, true end
    end

    local kx = x0 + (math.log(value) - lo) / (hi - lo) * (x1 - x0)
    local t = math.max(1, fs * 0.15) -- the track's half thickness
    local dl = imgui.GetWindowDrawList()
    dl:AddRectFilled({ x0, cy - t }, { x1, cy + t }, imgui.GetColorU32(7), t)   -- FrameBg
    dl:AddRectFilled({ x0, cy - t }, { kx, cy + t }, imgui.GetColorU32(19), t)  -- SliderGrab
    local mid = (x0 + x1) / 2                                                    -- the 1x mark
    dl:AddRectFilled({ mid - 1, cy - r * 0.6 }, { mid + 1, cy + r * 0.6 }, imgui.GetColorU32(1))
    local knob = active and 20 or (hovered and 22 or 19)                         -- SliderGrabActive, ButtonHovered
    dl:AddCircleFilled({ kx, cy }, r, imgui.GetColorU32(knob))
    dl:AddCircle({ kx, cy }, r, imgui.GetColorU32(0), 0, 1)                      -- Text: an outline
    return changed, value
end

local function draw_window()
    -- Any width (the grip), the height always the contents': title, name, slider, buttons
    local fs, style = imgui.GetFontSize(), imgui.GetStyle()
    local height = imgui.GetFrameHeight() * 2 + style.WindowPadding.y * 2 + fs + math.max(fs * 1.4, fs)
        + style.ItemSpacing.y * 2
    imgui.SetNextWindowSize({ fs * 18, height }, 4) -- ImGuiCond_FirstUseEver
    imgui.SetNextWindowSizeConstraints({ fs * 10, height }, { fs * 80, height })
    if imgui.Begin('Actor Scale', window, kWindowFlags) then
        local index, e, p = current_target(true)
        if index == nil then
            imgui.TextDisabled('Nothing targeted')
        else
            imgui.Text(e.name)
            local s = scaled[index]
            local why = (s == nil or s.id ~= e.id) and select(2, own_scale(p)) or nil
            if why then
                imgui.TextDisabled('Can\'t scale: ' .. why)
            else
                local changed, size = size_slider('##size', size_now(index))
                if changed then
                    apply(index, e, p, size, true)
                end
                imgui.SameLine()
                imgui.Text(string.format('%.2fx', size))
            end
            if imgui.Button('Reset') then
                restore(index)
            end
            imgui.SameLine()
        end
        if imgui.Button('Reset all') then
            restore_all()
        end
    end
    imgui.End()
end

-- Hold scaled entities at their size; drop ones that left (another entity can take the slot).
vana.events.on('present', function ()
    if window[1] then draw_window() end
    if next(scaled) == nil then return end
    for index, s in pairs(scaled) do
        local e = vana.game.entity(index)
        local p = entity_pointer(index)
        if e == nil or p == nil or e.id ~= s.id then
            scaled[index] = nil
        elseif vana.memory.read_float(p + kModelSize) ~= s.raw then
            vana.memory.write_float(p + kModelSize, s.raw)
        end
    end
end)

vana.events.on('zone_out', function () scaled = {} end)
vana.events.on('unload', restore_all)
