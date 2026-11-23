--[[
vana\draw.lua: the drawing layer of the /vana window, on ImGui's draw lists. Loaded by vana.lua with the
shared table M (`local M = ...`): it adds the look (M.P, M.FA), the scale (M.S), the fonts, the text and shape helpers,
the mouse helpers (M.hit, M.over, M.tooltip), the eased animations (M.ease) and the controls (M.switch, M.slider,
M.segments, M.dropdown, M.swatch, M.button, M.icon_button, M.meter). Nothing here reads or writes a setting: a control
takes a value and returns the new one and whether it changed.
--]]

local M = ...
local imgui = vana.imgui
M.imgui = imgui

-------------------------------------------------------------------------------
-- ImGui's numbers (1.92)
-------------------------------------------------------------------------------

M.COL = {
    Text = 0, TextDisabled = 1, WindowBg = 2, ChildBg = 3, PopupBg = 4, Border = 5, FrameBg = 7, FrameBgHovered = 8,
    FrameBgActive = 9, ScrollbarBg = 14, ScrollbarGrab = 15, ScrollbarGrabHovered = 16, ScrollbarGrabActive = 17,
    Header = 24, HeaderHovered = 25, HeaderActive = 26, Separator = 27, ModalWindowDimBg = 59,
}
M.VAR = {
    WindowPadding = 2, WindowRounding = 3, WindowBorderSize = 4, ChildRounding = 7, ChildBorderSize = 8, PopupRounding = 9,
    PopupBorderSize = 10, FramePadding = 11, FrameRounding = 12, FrameBorderSize = 13, ItemSpacing = 14, ScrollbarSize = 18,
    ScrollbarRounding = 19,
}
-- NoNavInputs: Tab is the game's (target cycling), not ImGui's tabbing to the search box; on every window and child
M.NO_NAV = 65536
-- NoTitleBar, NoResize, NoScrollbar, NoScrollWithMouse, NoCollapse, NoBackground: the frame is drawn here
M.WINDOW_FLAGS = 1 + 2 + 8 + 16 + 32 + 128 + M.NO_NAV
-- NoTitleBar, NoResize, NoMove, NoCollapse, AlwaysAutoResize, NoSavedSettings: the colour picker and the number box
M.POPUP_WINDOW = 1 + 2 + 4 + 32 + 64 + 256 + M.NO_NAV
M.FIRST_USE_EVER = 4
-- ImDrawFlags: which corners of a rectangle round
M.CORNERS = { all = 0, top = 48, bottom = 192, left = 80, right = 160, none = 256 }
local COL, VAR, CORNERS = M.COL, M.VAR, M.CORNERS

-------------------------------------------------------------------------------
-- The look
-------------------------------------------------------------------------------

M.P = {
    bg        = '#161822', -- the page
    side      = '#10121A', -- the list of pages (at its right edge)
    side_l    = '#0A0B11', -- the list of pages at its left edge: it shades to side across
    border    = '#2A2E3E',
    line      = '#242838', -- a hairline in the page
    well      = '#0E1017', -- a sunken box: a drop-down, a stepper, a set of segments, the search
    well_edge = '#272B3C',
    search    = '#080A10', -- the search box: darker than the list of pages behind it, with a lighter edge, so it stands out
    search_edge = '#3E4460',
    hover     = '#1F2334',
    text      = '#E8EAF1',
    white     = '#FFFFFF',
    text2     = '#BDC1D0',
    dim       = '#7B8095',
    faint     = '#4E536A',
    accent    = '#7AA2F7',
    accent2   = '#A9C1FF',
    accent_lo = '#3E5CB3',
    accent_bg = '#1B2340',
    teal      = '#5FD3B3',
    cyan      = '#4FD6FF', -- the sub-tab shown in the list of pages
    amber     = '#E8B45C',
    rose      = '#EE6F78',
    rose_bg   = '#3A2026',
    green     = '#9ECE6A', -- a layer that's on (Client > Layers; off is dim)
    knob      = '#F2F3F8',
    off       = '#2B2F42',
    seg       = '#313752',
    ink       = '#0B1020', -- text on the accent
    mine      = '#F7E9A6', -- this game's character in the Characters cards
    hud       = '#F3E38A', -- the HUD Manager page, light yellow
    experimental = '#C9AEF7', -- the Experimental page, light purple
    server    = '#E5C07B', -- the server's own overlay in Pivot
    -- a 'banner' button (M.button)
    banner_button = '#2E3250', banner_button_hi = '#3A3F63',
}
local P = M.P

-- '#RRGGBB' as ImGui's { r, g, b, a }
function M.rgba(hex, alpha)
    return {
        tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255, alpha or 1,
    }
end
local rgba = M.rgba

-- an ImU32 (0xAABBGGRR) for a draw list
function M.u32(hex, alpha)
    local a = math.floor(math.max(0, math.min(1, alpha or 1)) * 255 + 0.5)
    return a * 16777216 + tonumber(hex:sub(6, 7), 16) * 65536 + tonumber(hex:sub(4, 5), 16) * 256 + tonumber(hex:sub(2, 3), 16)
end
local u32 = M.u32

-- the colour `t` of the way from `a` to `b`
function M.mix(a, b, t)
    t = math.max(0, math.min(1, t))
    local function part(i)
        local x, y = tonumber(a:sub(i, i + 1), 16), tonumber(b:sub(i, i + 1), 16)
        return math.floor(x + (y - x) * t + 0.5)
    end
    return string.format('#%02X%02X%02X', part(2), part(4), part(6))
end
local mix = M.mix

-- Font Awesome 6 (merged into ImGui's font), by name
M.FA = {
    gear = 0xF013, display = 0xF26C, sun = 0xF185, moon = 0xF186, image = 0xF03E, palette = 0xF53F, window = 0xF2D0,
    puzzle = 0xF12E, plug = 0xF1E6, info = 0xF05A, wand = 0xE2CA, seedling = 0xF4D8, comment = 0xF075, keyboard = 0xF11C,
    down = 0xF078, up = 0xF077, right = 0xF054, close = 0xF00D, check = 0xF00C, reset = 0xF0E2, reload = 0xF021,
    folder = 0xF07C, bolt = 0xF0E7, gauge = 0xF625, chip = 0xF2DB, camera = 0xF030, half = 0xF042, question = 0xF059,
    bug = 0xF188, globe = 0xF0AC, discord = 0xF392, github = 0xF09B, minus = 0xF068, plus = 0xF067, map = 0xF279,
    layers = 0xF5FD, search = 0xF002, cat = 0xF6BE, heart = 0xF004, save = 0xF0C7, trash = 0xF1F8, download = 0xF019,
    gamepad = 0xF11B, crown = 0xF521, sliders = 0xF1DE, lock = 0xF023, warning = 0xF071, chart = 0xF201, tree = 0xF1BB, arrow = 0xF061,
    arrow_down = 0xF063, -- Client > Characters: the Main's down to the copies, down to the Alt's
    robot = 0xF544, flask = 0xF0C3, star = 0xF005, power = 0xF011, upload = 0xF093, export = 0xF56E, left = 0xF053,
    copy = 0xF0C5, paste = 0xF0EA, bars = 0xF0C9, stats = 0xE473, eye = 0xF06E, eye_off = 0xF070, cube = 0xF1B2,
    hud = 0xF247, -- object-group: the HUD Manager's windows
    open = 0xF35D, -- up-right-from-square: the HUD's Open Manager (a window of its own)
    grid = 0xF00A, magnet = 0xF076, tag = 0xF02B, -- the HUD Manager's Grid, Snap and Names switches
    filter = 0xF0B0, -- the HUD Manager's Open only switch
    bookmark = 0xF02E, -- the Presets sub-tabs (Graphics', the HUD Manager's)
    share = 0xE09A, -- arrow-up-from-bracket: a preset's Export
    pen = 0xF304, -- the HUD Manager's edit, after its presets
    eject = 0xF052, -- an addon's Unload, on its page's heading
    font = 0xF031, -- the True Font page
    play = 0xF04B, -- a Preview button's (Zone Name's, Load Screen's)
    lock_open = 0xF3C1, pointer = 0xF245, -- an addon window's Lock and No Pointer Events (pointer_off: DRAWN)
}

-- a code point as UTF-8 (Font Awesome's live at U+E000..U+F8FF: three bytes)
local function utf8(cp)
    return string.char(0xE0 + math.floor(cp / 4096), 0x80 + math.floor(cp / 64) % 64, 0x80 + cp % 64)
end

function M.fa(name)
    return utf8(M.FA[name] or M.FA.question)
end
local fa = M.fa

-------------------------------------------------------------------------------
-- Scale and fonts
-------------------------------------------------------------------------------

-- the screen's height decides the size, as the old window's does, times the user's percent
M.scale = 1
local scaled_at = -1

function M.S(n)
    return math.floor(n * M.scale + 0.5)
end
local S = M.S

-- where ImGui places windows: the game's pixels (ImGui's DisplaySize), not the interface's (vana.ui.screen, the size
-- this window is laid out at). Returns their width and height, and the scale VanaCore's own windows are drawn at
-- there (layout.cpp's own scale): a window ww wide in the interface's pixels is ww * k wide on ImGui's screen.
function M.display(screen_w, screen_h)
    local ok, io = pcall(imgui.GetIO)
    local w, h = 0, 0
    if ok and io ~= nil then
        w, h = io.DisplaySize.x, io.DisplaySize.y
    end
    if not w or w <= 0 or not h or h <= 0 then
        return screen_w, screen_h, 1
    end
    return w, h, math.sqrt((w / screen_w) * (h / screen_h))
end

-- `percent`: the user's scale ([vana] scale); `force` recomputes now (the scale slider let go)
function M.compute_scale(now, screen_h, percent, force)
    if not force and now - scaled_at < 1 then
        return
    end
    scaled_at = now
    local user = math.max(0.5, math.min(3, (percent or 100) / 100))
    M.scale = math.max(0.75, math.min(1.6, screen_h / 1440)) * 1.1 * user
end

-- Segoe UI for the text (every Windows has it), ImGui's own (Agave with Font Awesome merged in) for the icons.
-- Added in the first frame the window draws, used from the next: ImGui takes a new font up between frames.
M.fonts = { ready = false, text = nil, semi = nil, bold = nil, bold_italic = nil, semi_italic = nil, italic = nil, icons = nil }
local fonts = M.fonts

local function windows_fonts()
    return ((os and os.getenv and os.getenv('WINDIR')) or 'C:\\Windows') .. '\\Fonts\\'
end

function M.load_fonts()
    local dir = windows_fonts()
    fonts.icons = imgui.GetFont()
    fonts.text = imgui.AddFontFromFileTTF(dir .. 'segoeui.ttf', 18)
    fonts.semi = imgui.AddFontFromFileTTF(dir .. 'seguisb.ttf', 18)
    -- a section's heading (bold italic), a sub-panel's (semibold italic), an addon's name in the list of pages (italic)
    -- and Client > VanaCore's version (bold); a Windows without them gets the semibold (the regular for the italic)
    local function optional(file, fallback)
        local found = io and io.open and io.open(dir .. file, 'rb')
        if not found then
            return fallback or fonts.semi
        end
        found:close()
        return imgui.AddFontFromFileTTF(dir .. file, 18) or fallback or fonts.semi
    end
    fonts.bold_italic = optional('segoeuiz.ttf')
    fonts.semi_italic = optional('seguisbi.ttf')
    fonts.italic = optional('segoeuii.ttf', fonts.text)
    fonts.bold = optional('segoeuib.ttf')
end

-- A font list's pick in its own font (data.lua font_list's `face`, a file in Windows' Fonts): nil until it's there, then
-- the font. Added as a list shows it, two a frame (a list opening doesn't stall), and used from the frame after (ImGui
-- takes a new font up between frames). Kept for the session: ImGui has no removing a font here.
local faces = { added = {}, frame = -1, n = 0 } -- added[file]: { font, frame } (false: the file didn't load)
function M.face(file)
    if file == nil or not fonts.ready then
        return nil
    end
    local frame = imgui.GetFrameCount()
    local face = faces.added[file]
    if face == nil then
        -- Segoe UI and its Semibold are the window's own
        local own = (file == 'segoeui.ttf' and fonts.text) or (file == 'seguisb.ttf' and fonts.semi) or nil
        if own ~= nil then
            face = { own, -1 }
        else
            if faces.frame ~= frame then
                faces.frame, faces.n = frame, 0
            end
            if faces.n >= 2 then
                return nil
            end
            faces.n = faces.n + 1
            local ok, font = pcall(imgui.AddFontFromFileTTF, windows_fonts() .. file, 18)
            face = ok and font ~= nil and { font, frame } or false
        end
        faces.added[file] = face
    end
    if face == false or face[2] >= frame then
        return nil
    end
    return face[1]
end

M.BODY, M.SMALL = 16, 13.5 -- the text sizes
local BODY, SMALL = M.BODY, M.SMALL

-------------------------------------------------------------------------------
-- The logo (addons\vanacore\vana\logo.png) and Client > VanaCore's banner (banner.png: the README's, 1024 x 256) as
-- textures for ImGui, made through VanaCore's own D3DX (what Ashita addons call) on the game's device. nil when they
-- can't be: the tile shows a V and the banner the name instead.
-------------------------------------------------------------------------------

M.logo = { tried = false, texture = nil, holder = nil, banner = nil, banner_holder = nil }

function M.load_logo()
    local logo = M.logo
    logo.tried = true
    local ok, ffi = pcall(require, 'ffi')
    if not ok or ffi == nil or vana.ashita == nil or vana.ashita.object == nil or vana.paths == nil then
        return
    end
    pcall(ffi.cdef, 'long __stdcall D3DXCreateTextureFromFileA(void* device, const char* file, void** texture);')
    local device = vana.ashita.object('device')
    if device == nil then
        return
    end
    local function load(name)
        local path = vana.paths.addons .. '\\vanacore\\vana\\' .. name
        local holder = ffi.new('void*[1]')
        local made, hr = pcall(function () return ffi.C.D3DXCreateTextureFromFileA(ffi.cast('void*', device), path, holder) end)
        if made and hr == 0 and holder[0] ~= nil then
            return holder, holder[0]
        end
    end
    logo.holder, logo.texture = load('logo.png')
    logo.banner_holder, logo.banner = load('banner.png')
end

function M.release_logo()
    local logo = M.logo
    local ok, ffi = pcall(require, 'ffi')
    for _, texture in ipairs({ logo.texture or false, logo.banner or false }) do
        if texture and ok then
            -- IUnknown::Release, the vtable's third slot
            pcall(function ()
                local vtable = ffi.cast('void**', texture)[0]
                ffi.cast('long(__stdcall**)(void*)', vtable)[2](texture)
            end)
        end
    end
    logo.texture, logo.holder, logo.banner, logo.banner_holder = nil, nil, nil, nil
end

-------------------------------------------------------------------------------
-- Text and shapes
-------------------------------------------------------------------------------

function M.push(font, size)
    imgui.PushFont(font, S(size))
end
local push = M.push

function M.pop()
    imgui.PopFont()
end
local pop = M.pop

-- a string's size by font, size and string: the window measures the same labels every frame. Dropped when the scale
-- changes (the sizes are scaled) and when it grows large (a live value's text changes).
local measured, measured_scale, measured_n = {}, nil, 0

function M.measure(str, font, size)
    if font == nil or size == nil or str == nil then
        push(font, size)
        local w, h = imgui.CalcTextSize(str)
        pop()
        return w, h
    end
    if measured_scale ~= M.scale or measured_n > 4000 then
        measured, measured_scale, measured_n = {}, M.scale, 0
    end
    local by_font = measured[font]
    if by_font == nil then
        by_font = {}
        measured[font] = by_font
    end
    local by_size = by_font[size]
    if by_size == nil then
        by_size = {}
        by_font[size] = by_size
    end
    local entry = by_size[str]
    if entry == nil then
        push(font, size)
        local w, h = imgui.CalcTextSize(str)
        pop()
        entry = { w, h }
        by_size[str] = entry
        measured_n = measured_n + 1
    end
    return entry[1], entry[2]
end
local measure = M.measure

function M.text(dl, x, y, str, color, font, size, alpha)
    push(font, size)
    dl:AddText({ x, y }, u32(color, alpha), str)
    pop()
end
local text = M.text

-- How far text is lifted when centred: its box is the font's line (a tall ascent, a descent), so centring the box
-- leaves the letters low; they're lifted by a share of the line's height.
local function lift(h)
    return math.floor(h * 0.06 + 0.5)
end

-- How far down `font`'s letters go to sit where Segoe UI's (fonts.text) do at `size`, whole pixels: lift() is Segoe UI's,
-- whose line is tall over its capitals, and a font with a shorter line has its capitals higher in it (Calibri ~2 px at
-- 18, Palatino ~3). Measured from each font's own H (its glyph's top and bottom from the line's top), so any font a
-- list shows lines up. 0 for the window's own fonts. Kept by font and size, dropped when the scale changes.
local cap_shifts, cap_scale = {}, nil
local function cap_middle(font, size)
    push(font, size)
    local ok, middle = pcall(function ()
        local glyph = imgui.GetFontBaked():FindGlyph(72) -- (H)
        return (glyph.Y0 + glyph.Y1) / 2
    end)
    pop()
    return ok and type(middle) == 'number' and middle or nil
end
function M.cap_shift(font, size)
    if font == nil or size == nil or fonts.text == nil then
        return 0
    end
    if cap_scale ~= M.scale then
        cap_shifts, cap_scale = {}, M.scale
    end
    local by_size = cap_shifts[font]
    if by_size == nil then
        by_size = {}
        for _, own in pairs(fonts) do
            if own == font then
                by_size.own = true
            end
        end
        cap_shifts[font] = by_size
    end
    if by_size.own then
        return 0
    end
    local shift = by_size[size]
    if shift == nil then
        local mine, segoe = cap_middle(font, size), cap_middle(fonts.text, size)
        if mine == nil or segoe == nil then
            return 0 -- (not kept: measured again next time)
        end
        shift = math.floor(segoe - mine + 0.5)
        by_size[size] = shift
    end
    return shift
end
local cap_shift = M.cap_shift

-- text with its line centred on cy
function M.text_mid(dl, x, cy, str, color, font, size, alpha)
    local _, h = measure(str, font, size)
    text(dl, x, cy - h / 2 - lift(h) + cap_shift(font, size), str, color, font, size, alpha)
end
local text_mid = M.text_mid

-- `str` a letter at a time, `spacing` apart beyond the font's own (ImGui has no letter spacing), its middle at `cy`; nil `dl`
-- only measures. Its width.
function M.text_spaced(dl, x, cy, str, color, font, size, alpha, spacing)
    local start = x
    for letter in str:gmatch('[%z\1-\127\194-\244][\128-\191]*') do -- a UTF-8 character at a time
        if dl ~= nil then
            text_mid(dl, x, cy, letter, color, font, size, alpha)
        end
        x = x + measure(letter, font, size) + spacing
    end
    return x - start - spacing
end

function M.text_centred(dl, x, y, w, h, str, color, font, size, alpha)
    local tw, th = measure(str, font, size)
    text(dl, x + math.floor((w - tw) / 2), y + math.floor((h - th) / 2) - lift(th), str, color, font, size, alpha)
end
local text_centred = M.text_centred

function M.text_right_mid(dl, right, cy, str, color, font, size, alpha)
    local tw, th = measure(str, font, size)
    text(dl, right - tw, cy - th / 2 - lift(th), str, color, font, size, alpha)
end
local text_right_mid = M.text_right_mid

-- `str` cut to fit `room` pixels, with an ellipsis
function M.fit(str, room, font, size)
    if measure(str, font, size) <= room then
        return str
    end
    local n = #str
    while n > 1 and measure(str:sub(1, n) .. '…', font, size) > room do
        n = n - 1
    end
    return str:sub(1, n) .. '…'
end

-- text shading from colour `left` to colour `right`, a letter at a time (each placed by the width of the text before
-- it, so the letters sit as they would in one piece); returns its width
function M.gradient_text(dl, x, cy, str, left, right, font, size)
    local n = #str
    local width = measure(str, font, size)
    for i = 1, n do
        local before = i > 1 and measure(str:sub(1, i - 1), font, size) or 0
        text_mid(dl, x + before, cy, str:sub(i, i), mix(left, right, (i - 0.5) / n), font, size)
    end
    return width
end

-- icons drawn with lines, not in Font Awesome, by name; each fn(dl, cx, cy, size, colour as u32), about as big as a
-- glyph of `size`
local DRAWN = {}

-- Material Symbols' "downloading" (Import): an arrow down in a ring, whole round the left, in dashes down the right
function DRAWN.downloading(dl, cx, cy, size, c)
    local r = size * M.scale * 0.5
    local th = math.max(1, size * M.scale * 0.12)
    dl:PathArcTo({ cx, cy }, r, math.pi * 0.5, math.pi * 1.5, 0)
    dl:PathStroke(c, false, th)
    for _, dash in ipairs({ { -0.38, -0.2 }, { -0.08, 0.08 }, { 0.2, 0.38 } }) do
        dl:PathArcTo({ cx, cy }, r, dash[1] * math.pi, dash[2] * math.pi, 0)
        dl:PathStroke(c, false, th)
    end
    local s = r * 0.5
    dl:AddLine({ cx, cy - s }, { cx, cy + s * 0.8 }, c, th)
    dl:PathLineTo({ cx - s * 0.7, cy + s * 0.1 })
    dl:PathLineTo({ cx, cy + s * 0.8 })
    dl:PathLineTo({ cx + s * 0.7, cy + s * 0.1 })
    dl:PathStroke(c, false, th)
end

-- Outline (a True Font line's, before its value): a hollow A, its edge in `c` a pixel all round and the drop-down's well
-- inside it, as an outlined letter is drawn
function DRAWN.outline(dl, cx, cy, size, c)
    local face = fonts.bold or fonts.semi or fonts.text
    local w, h = measure('A', face, size)
    local x, y = cx - w / 2, cy - h / 2 - lift(h)
    local o = math.max(1, math.floor(M.scale + 0.5))
    push(face, size)
    for _, d in ipairs({ { -o, 0 }, { o, 0 }, { 0, -o }, { 0, o }, { -o, -o }, { o, -o }, { -o, o }, { o, o } }) do
        dl:AddText({ x + d[1], y + d[2] }, c, 'A')
    end
    dl:AddText({ x, y }, u32(P.well), 'A')
    pop()
end

-- a space bar's key symbol: a bracket open at the top
function DRAWN.space(dl, cx, cy, size, c)
    local w = size * M.scale * 0.6
    local h = size * M.scale * 0.22
    dl:PathLineTo({ cx - w, cy - h })
    dl:PathLineTo({ cx - w, cy + h })
    dl:PathLineTo({ cx + w, cy + h })
    dl:PathLineTo({ cx + w, cy - h })
    dl:PathStroke(c, false, math.max(1, size * M.scale * 0.12))
end

-- No Pointer Events (an addon's window the mouse goes through): Font Awesome's arrow pointer struck through, bottom-left
-- to top-right (across the arrow, which points up-left)
function DRAWN.pointer_off(dl, cx, cy, size, c)
    local str = fa('pointer')
    local w, h = measure(str, fonts.icons, size)
    push(fonts.icons, size)
    dl:AddText({ cx - w / 2, cy - h / 2 }, c, str)
    pop()
    local r = size * M.scale * 0.6
    dl:AddLine({ cx - r, cy + r }, { cx + r, cy - r }, c, math.max(1, size * M.scale * 0.14))
end

function M.icon_centred(dl, cx, cy, name, color, size, alpha)
    if DRAWN[name] ~= nil then
        DRAWN[name](dl, cx, cy, size, u32(color, alpha))
        return
    end
    local str = fa(name)
    local w, h = measure(str, fonts.icons, size)
    text(dl, cx - w / 2, cy - h / 2, str, color, fonts.icons, size, alpha)
end
local icon_centred = M.icon_centred

function M.fill(dl, x, y, w, h, color, radius, corners, alpha)
    dl:AddRectFilled({ x, y }, { x + w, y + h }, u32(color, alpha), radius or 0, corners or 0)
end
local fill = M.fill

function M.outline(dl, x, y, w, h, color, radius, corners, thickness, alpha)
    dl:AddRect({ x, y }, { x + w, y + h }, u32(color, alpha), radius or 0, corners or 0, thickness or 1)
end
local outline = M.outline

function M.hline(dl, x1, x2, y, color, alpha)
    dl:AddLine({ x1, y }, { x2, y }, u32(color, alpha), 1)
end

function M.vline(dl, x, y1, y2, color, alpha)
    dl:AddLine({ x, y1 }, { x, y2 }, u32(color, alpha), 1)
end

-- a bent arrow (down, then right) drawn with lines so no font has to carry the glyph: from (x, top) down to
-- cy, then right to x + w; width and height in unscaled pixels
function M.sub_arrow(dl, x, top, cy, w, color, alpha)
    local c = u32(color, alpha)
    local right = x + S(w)
    dl:AddLine({ x, top }, { x, cy }, c, 1)
    dl:AddLine({ x, cy }, { right, cy }, c, 1)
    dl:AddLine({ right, cy }, { right - S(3), cy - S(3) }, c, 1)
    dl:AddLine({ right, cy }, { right - S(3), cy + S(3) }, c, 1)
end

-- a < or > ('left' / 'right') drawn with lines, its ink centred on (cx, cy): the icon font's chevrons sit left and low
function M.chevron(dl, cx, cy, name, color, alpha)
    local c = u32(color, alpha)
    local hw, hh = S(1.75), S(3.25)
    local tip = name == 'left' and cx - hw or cx + hw
    local back = name == 'left' and cx + hw or cx - hw
    dl:AddLine({ back, cy - hh }, { tip, cy }, c, S(1.5))
    dl:AddLine({ tip, cy }, { back, cy + hh }, c, S(1.5))
end
local chevron = M.chevron

function M.circle(dl, cx, cy, r, color, alpha)
    dl:AddCircleFilled({ cx, cy }, r, u32(color, alpha), 0)
end
local circle = M.circle

-- a rounded rectangle shading from colour `left` to colour `right`: strips between its two rounded ends
function M.hgradient(dl, x, y, w, h, left, right, radius)
    fill(dl, x, y, radius * 2, h, left, radius)
    fill(dl, x + w - radius * 2, y, radius * 2, h, right, radius)
    local steps = 32
    local x0, span = x + radius, w - radius * 2
    for k = 0, steps - 1 do
        local x1, x2 = x0 + math.floor(span * k / steps), x0 + math.floor(span * (k + 1) / steps)
        fill(dl, x1, y, math.max(1, x2 - x1), h, mix(left, right, (k + 0.5) / steps))
    end
end

-------------------------------------------------------------------------------
-- The mouse
-------------------------------------------------------------------------------

-- an item the mouse can use at (x, y): hovered, clicked, held, right-clicked
function M.hit(id, x, y, w, h)
    imgui.SetCursorScreenPos({ x, y })
    imgui.InvisibleButton(id, { math.max(1, w), math.max(1, h) })
    return imgui.IsItemHovered(), imgui.IsItemClicked(0), imgui.IsItemActive(), imgui.IsItemClicked(1)
end
local hit = M.hit

-- the mouse over a box of this window, with no item made for it
function M.over(x, y, w, h)
    return imgui.IsWindowHovered() and imgui.IsMouseHoveringRect({ x, y }, { x + w, y + h })
end

-- a tooltip by the mouse (the window's own padding is 0, so it brings its own), its text wrapped past ~400 px
function M.tooltip(str)
    imgui.PushStyleVar(VAR.WindowPadding, { S(9), S(6) })
    imgui.PushStyleVar(VAR.WindowRounding, S(4))
    imgui.PushStyleVar(VAR.WindowBorderSize, 1)
    push(fonts.text, SMALL + 0.5)
    if imgui.BeginTooltip() then
        imgui.PushTextWrapPos(S(300))
        imgui.TextUnformatted(str)
        imgui.PopTextWrapPos()
        imgui.EndTooltip()
    end
    pop()
    imgui.PopStyleVar(3)
end
local tooltip = M.tooltip

-- `id` eased towards `target` a bit each frame (M.st.anim, M.st.dt: the window's state)
function M.ease(id, target, speed)
    local st = M.st
    local v = st.anim[id]
    if v == nil then
        st.anim[id] = target
        return target
    end
    v = v + (target - v) * math.min(1, st.dt * (speed or 18))
    if math.abs(v - target) < 0.003 then
        v = target
    end
    st.anim[id] = v
    return v
end
local ease = M.ease

-- the style of ImGui's own popups (a drop-down's list, the pickers) pushed, and popped: the window pushes zero padding
function M.push_popup_style()
    imgui.PushStyleColor(COL.PopupBg, rgba('#0F1118'))
    imgui.PushStyleColor(COL.WindowBg, rgba('#0F1118'))
    imgui.PushStyleColor(COL.Border, rgba(P.border))
    imgui.PushStyleColor(COL.Header, rgba(P.accent_bg))
    imgui.PushStyleColor(COL.HeaderHovered, rgba(P.hover))
    imgui.PushStyleColor(COL.HeaderActive, rgba(P.accent_lo))
    imgui.PushStyleColor(COL.Separator, rgba(P.line))
    imgui.PushStyleColor(COL.Text, rgba(P.text))
    imgui.PushStyleColor(COL.FrameBg, rgba(P.well))
    imgui.PushStyleColor(COL.FrameBgHovered, rgba(P.well))
    imgui.PushStyleColor(COL.FrameBgActive, rgba(P.well))
    imgui.PushStyleVar(VAR.WindowPadding, { S(10), S(8) })
    imgui.PushStyleVar(VAR.WindowRounding, S(5))
    imgui.PushStyleVar(VAR.WindowBorderSize, 1)
    imgui.PushStyleVar(VAR.PopupRounding, S(5))
    imgui.PushStyleVar(VAR.PopupBorderSize, 1)
    imgui.PushStyleVar(VAR.FramePadding, { S(8), S(4) })
    imgui.PushStyleVar(VAR.FrameRounding, S(4))
    imgui.PushStyleVar(VAR.ItemSpacing, { S(8), S(8) })
end

function M.pop_popup_style()
    imgui.PopStyleVar(8)
    imgui.PopStyleColor(11)
end

-------------------------------------------------------------------------------
-- Controls (each draws itself, centred on cy, and returns its value and whether it changed)
-------------------------------------------------------------------------------

M.SW_W, M.SW_H = 32, 17

-- `colour`: the track's when on (row.colour: the Buttons tabs' Enable, light green), the accent's if none
function M.switch(id, x, cy, on, enabled, alpha_scale, colour)
    local w, h = S(M.SW_W), S(M.SW_H)
    local y = cy - h / 2
    local hovered, clicked = hit(id, x, y, w, h)
    local changed = false
    if clicked and enabled then
        on = not on
        changed = true
    end
    local t = ease(id, on and 1 or 0)
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    fill(dl, x, y, w, h, mix(P.off, colour or P.accent, t), h / 2, 0, alpha)
    if hovered and enabled then
        fill(dl, x, y, w, h, '#FFFFFF', h / 2, 0, 0.08)
    end
    local r = (h - S(4)) / 2
    local kx = x + S(2) + r + (w - S(4) - r * 2) * t
    circle(dl, kx, y + h / 2, r, P.knob, alpha)
    return on, changed
end

M.SLIDER_W = 200 -- the track and the value after it
M.RESET_W = 20   -- a reset icon and the gap after it, before its control

-- A setting's reset icon from x (M.RESET_W across, its control after it): lit when `can_reset` (the setting isn't its
-- default), brighter under the mouse. Every setting's, a slider's and the rest (vana.lua M.row_reset). Returns clicked.
-- (2026-11-23: not drawn any more, a setting's nor a sub-panel's: only each panel heading's Reset button stays. The room
-- it took is kept so no control moves; set true to bring them back.)
M.ROW_RESETS = false
function M.reset_icon(id, x, cy, can_reset, enabled, alpha)
    if not M.ROW_RESETS then
        return false
    end
    local hov, clicked = hit(id, x - S(2), cy - S(10), S(18), S(20))
    local lit = ease(id .. 'lit', hov and enabled and 1 or 0, 22)
    icon_centred(imgui.GetWindowDrawList(), x + S(7), cy, 'reset', mix(can_reset and P.dim or P.faint, P.text, lit), 9,
                 (enabled and 1 or 0.4) * (alpha or 1))
    if hov and enabled then
        tooltip('Back to the default')
    end
    return clicked and enabled
end

-- A slider. `opts`: text (the value as shown), reset (a reset icon before the track, lit when `can_reset`), marks
-- ({ { value, name }, ... } under the track), alpha, width (the whole, SLIDER_W unless given) and value_w (the value's
-- column, 48 unless given). Returns value, changed (while dragged), released (the drag ended), value_clicked (the
-- value's text pressed: a typed number), reset_clicked.
function M.slider(id, x, cy, value, min, max, step, enabled, opts)
    opts = opts or {}
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (opts.alpha or 1)
    local changed, released, value_clicked, reset_clicked = false, false, false, false
    if opts.reset then
        reset_clicked = M.reset_icon(id .. '.reset', x, cy, opts.can_reset, enabled, opts.alpha)
        x = x + S(M.RESET_W)
    end
    local value_w = opts.value_w or S(48)
    local track_w = (opts.width or S(M.SLIDER_W)) - value_w - S(8) - (opts.reset and S(20) or 0)
    local h = S(18)
    -- the press area runs past the track's ends up to the reset icon and the value, so the knob at either end (drawn half
    -- past it) is the slider's and not the window's background (a press there dragged the window)
    local hovered, _, active = hit(id, x - S(4), cy - h / 2, track_w + S(4) + S(8), h)
    local was_active = M.st.sliding == id
    if active and enabled then
        local mx = imgui.GetMousePos()
        local f = math.max(0, math.min(1, (mx - x) / track_w))
        local v = min + (max - min) * f
        if step and step > 0 then
            v = math.floor((v - min) / step + 0.5) * step + min
        end
        v = math.max(min, math.min(max, v))
        if v ~= value then
            value = v
            changed = true
        end
        M.st.sliding = id
    elseif was_active then
        M.st.sliding = nil
        released = true
    end
    local f = (value - min) / (max - min)
    f = math.max(0, math.min(1, f))
    local th = S(4)
    fill(dl, x, cy - th / 2, track_w, th, P.off, th / 2, 0, alpha)
    if f > 0 then
        fill(dl, x, cy - th / 2, math.max(th, track_w * f), th, P.accent, th / 2, 0, alpha)
    end
    if opts.marks ~= nil then
        -- a tick at each mark; its name under it when it doesn't run into the one before
        local last_right = -1
        for _, mark in ipairs(opts.marks) do
            local mf = math.max(0, math.min(1, (mark[1] - min) / (max - min)))
            local mx = x + track_w * mf
            M.vline(dl, mx, cy + S(4), cy + S(7), P.faint, alpha)
            local tw = measure(mark[2], fonts.text, SMALL - 2.5)
            local tx = math.max(x, math.min(x + track_w - tw, mx - tw / 2))
            if tx > last_right + S(4) then
                text(dl, tx, cy + S(7), mark[2], P.faint, fonts.text, SMALL - 2.5, alpha)
                last_right = tx + tw
            end
        end
    end
    local kr = S(5.5)
    local kx = x + track_w * f
    local lit = ease(id .. '.lit', (hovered or active) and enabled and 1 or 0)
    circle(dl, kx, cy, kr + S(1) * lit, active and P.accent2 or mix(P.knob, P.accent2, lit * 0.5), alpha)
    -- the value: a press on it opens a typed number
    local vx = x + track_w + S(8)
    local vhov, vclick = hit(id .. '.value', vx, cy - S(10), value_w, S(20))
    if vhov and enabled then
        fill(dl, vx, cy - S(10), value_w, S(20), P.hover, S(3))
    end
    value_clicked = vclick and enabled
    text_right_mid(dl, vx + value_w, cy, opts.text or tostring(value), P.text2, fonts.text, SMALL + 0.5, alpha)
    return value, changed, released, value_clicked, reset_clicked
end

M.SEG_SIZE = BODY - 1 -- the segments' text
M.SEG_PAD = 10        -- either side of each word

function M.segments_width(labels, size)
    size = size or M.SEG_SIZE
    local widths, total = {}, S(4)
    for i, label in ipairs(labels) do
        widths[i] = measure(label, fonts.text, size) + S(M.SEG_PAD) * 2
        total = total + widths[i] + (i > 1 and S(1) or 0)
    end
    return total, widths
end

-- `labels`: the words; `index`: the one chosen (0: none). Returns index, changed and the one the mouse is on (nil: none).
function M.segments(id, x, cy, labels, index, enabled, size, alpha_scale)
    size = size or M.SEG_SIZE
    local h = S(size + 7)
    local y = cy - h / 2
    local total, widths = M.segments_width(labels, size)
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    local changed = false
    local over = nil
    fill(dl, x, y, total, h, P.well, S(4), 0, alpha)
    outline(dl, x, y, total, h, P.well_edge, S(4), 0, 1, alpha)
    local at = x + S(2)
    for i, label in ipairs(labels) do
        local hovered, clicked = hit(id .. i, at, y + S(2), widths[i], h - S(4))
        if hovered then
            over = i
        end
        if clicked and enabled and index ~= i then
            index = i
            changed = true
        end
        local t = ease(id .. i, index == i and 1 or 0)
        if t > 0.01 then
            fill(dl, at, y + S(2), widths[i], h - S(4), P.seg, S(3), 0, t * alpha)
        end
        if hovered and index ~= i and enabled then
            fill(dl, at, y + S(2), widths[i], h - S(4), '#FFFFFF', S(3), 0, 0.05)
        end
        text_centred(dl, at, y + S(2), widths[i], h - S(4), label, mix(P.dim, P.text, t), fonts.text, size, alpha)
        at = at + widths[i] + S(1)
    end
    return index, changed, over
end

-- Tabs across a section (vana.lua's kind 'tabs': Interface > Buttons' Global, Main Menu, Action Menu), from `x` to `right`,
-- `y` their top, M.TABS_H tall: each word a tab, the one picked (`index`) white over a line in the accent, the rest dim
-- (lighter under the mouse), a hairline under them all. Returns index and whether it changed.
M.TABS_H = 30
M.TAB_PAD = 14 -- either side of each word
function M.tabs(id, x, y, right, labels, index, alpha_scale)
    local dl = imgui.GetWindowDrawList()
    local alpha = alpha_scale or 1
    local h = S(M.TABS_H)
    local base = y + h - S(1)
    M.hline(dl, x, right, base, P.line, alpha)
    local changed = false
    local at = x
    for i, label in ipairs(labels) do
        local w = measure(label, fonts.semi, BODY) + S(M.TAB_PAD) * 2
        local hovered, clicked = hit(id .. i, at, y, w, h)
        if clicked and index ~= i then
            index, changed = i, true
        end
        local t = ease(id .. i, index == i and 1 or 0)
        local colour = mix(hovered and P.text or P.dim, P.white, t)
        text_centred(dl, at, y, w, h - S(3), label, colour, fonts.semi, BODY, alpha)
        if t > 0.01 then
            fill(dl, at + S(6), base - S(2), w - S(12), S(3), P.accent, S(1.5), 0, t * alpha)
        end
        at = at + w
    end
    return index, changed
end

-- Switches joined as one control (a True Font line's Bold, Italic and Uppercase, 2026-11-08): one well and outline round
-- them all, a hairline between each, each a button of its own lit while on (as M.chip's). `chips`: { { letter, font, on,
-- enabled, id = its own id }, ... }, from x; the width it takes M.chip_bar_width's (which sets each one's w).
-- Returns the one pressed (its index; nil: none) and the one the mouse is on.
function M.chip_bar_width(chips)
    local total = S(4)
    for i, c in ipairs(chips) do
        c.w = math.max(S(24), measure(c[1], c[2], M.SEG_SIZE) + S(12))
        total = total + c.w + (i > 1 and S(1) or 0)
    end
    return total
end
function M.chip_bar(x, cy, chips, alpha_scale)
    local h = S(M.SEG_SIZE + 7)
    local y = cy - h / 2
    local total = M.chip_bar_width(chips)
    local dl = imgui.GetWindowDrawList()
    local any = false
    for _, c in ipairs(chips) do
        any = any or c[4]
    end
    local frame = (any and 1 or 0.4) * (alpha_scale or 1)
    fill(dl, x, y, total, h, P.well, S(4), 0, frame)
    outline(dl, x, y, total, h, P.well_edge, S(4), 0, 1, frame)
    local at, pressed, over = x + S(2), nil, nil
    for i, c in ipairs(chips) do
        if i > 1 then -- (in the gap before it, under no button's light)
            M.vline(dl, at - S(1), y + S(5), y + h - S(5), P.well_edge, frame)
        end
        local enabled = c[4]
        local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
        local hovered, clicked = hit(c.id, at, y + S(2), c.w, h - S(4))
        if hovered then
            over = i
        end
        if clicked and enabled then
            pressed = i
        end
        local t = ease(c.id, (c[3] ~= (pressed == i)) and 1 or 0)
        if t > 0.01 then
            fill(dl, at, y + S(2), c.w, h - S(4), P.seg, S(3), 0, t * alpha)
        end
        if hovered and enabled and not c[3] then
            fill(dl, at, y + S(2), c.w, h - S(4), '#FFFFFF', S(3), 0, 0.05)
        end
        text_centred(dl, at, y, c.w, h, c[1], mix(P.dim, P.text, t), c[2], M.SEG_SIZE, alpha)
        at = at + c.w + S(1)
    end
    return pressed, over
end

-- A small square button lit while `on` (a True Font line's Bold and Italic), `w` across, its letter in `font` in the middle (a
-- letter, not the icon font's glyph: those sit a pixel or two left of their box's middle). Returns on and whether it was
-- flipped.
function M.chip(id, x, cy, w, letter, font, on, enabled, alpha_scale)
    local h = S(M.SEG_SIZE + 7)
    local y = cy - h / 2
    local hovered, clicked = hit(id, x, y, w, h)
    local changed = false
    if clicked and enabled then
        on, changed = not on, true
    end
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    local t = ease(id, on and 1 or 0)
    fill(dl, x, y, w, h, P.well, S(4), 0, alpha)
    outline(dl, x, y, w, h, P.well_edge, S(4), 0, 1, alpha)
    if t > 0.01 then
        fill(dl, x + S(2), y + S(2), w - S(4), h - S(4), P.seg, S(3), 0, t * alpha)
    end
    if hovered and enabled and not on then
        fill(dl, x + S(2), y + S(2), w - S(4), h - S(4), '#FFFFFF', S(3), 0, 0.05)
    end
    text_centred(dl, x, y, w, h, letter, mix(P.dim, P.text, t), font, M.SEG_SIZE, alpha)
    return on, changed
end

-- A checkbox from x with `label` after it (a damage number's Gradient), the box and its label one click; the width it takes
-- is M.checkbox_width's. Returns on and whether it was flipped.
function M.checkbox_width(label)
    return S(16) + S(7) + measure(label, fonts.text, SMALL + 0.5)
end
function M.checkbox(id, x, cy, label, on, enabled, alpha_scale)
    local box = S(16)
    local hovered, clicked = hit(id, x, cy - S(10), M.checkbox_width(label), S(20))
    local changed = false
    if clicked and enabled then
        on, changed = not on, true
    end
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    local t = ease(id, on and 1 or 0)
    local y = cy - box / 2
    fill(dl, x, y, box, box, mix(P.well, P.accent, t), S(3), 0, alpha)
    outline(dl, x, y, box, box, hovered and enabled and P.accent or P.well_edge, S(3), 0, 1, alpha)
    if t > 0.01 then
        icon_centred(dl, x + box / 2, cy, 'check', '#FFFFFF', 9, t * alpha)
    end
    text_mid(dl, x + box + S(7), cy, label, P.text2, fonts.text, SMALL + 0.5, alpha)
    return on, changed
end

M.DROP_W = 190
M.DROP_ICON_W = 12 -- a drop-down's look.icon, before its pick
-- a drop-down's padding each side, room for its chevron and the gap after its icon or word: as ever, and look.compact's
M.DROP_FIT = { pad = 6, chevron = 14, gap = 5 }
M.DROP_COMPACT = { pad = 4, chevron = 11, gap = 3 }
M.DROP_SEARCH = 20 -- a list with this many picks or more has a search box at its top (as has one whose `look` asks)
-- a drop-down list's category names' font (spaced capitals)
local function header_font() return fonts.semi_italic or fonts.semi end

-- each searchable list's search: { text = { typed } (InputText's buffer), focus = the field gets the keyboard }, made
-- anew as its list opens
local drop_search = {}

-- the options with every one of `words` (lower case) in their text, in any order: a category's name kept before the first
-- one under it, the lines dropped
local function drop_filtered(options, words)
    if #words == 0 then
        return options
    end
    local list, header = {}, nil
    for _, option in ipairs(options) do
        if option.header then
            header = option
        elseif not option.sep then
            local text, all = tostring(option[2]):lower(), true
            for _, word in ipairs(words) do
                if not text:find(word, 1, true) then
                    all = false
                    break
                end
            end
            if all then
                if header ~= nil then
                    list[#list + 1] = header
                    header = nil
                end
                list[#list + 1] = option
            end
        end
    end
    return list
end

-- a list's search box, all of its width at its top, as the addon list's: a well with the magnifier, ImGui's field in it
-- see-through, the × that empties it. It has the keyboard as the list opens. Returns the words typed, lower case.
local function drop_search_box(id, size)
    local search = drop_search[id]
    if search == nil or imgui.IsWindowAppearing() then
        search = { text = { '' }, focus = true }
        drop_search[id] = search
    end
    local dl = imgui.GetWindowDrawList()
    local x, y = imgui.GetCursorScreenPos()
    local w = (imgui.GetContentRegionAvail())
    local h = S(size + 9)
    local cy = y + h / 2
    fill(dl, x, y, w, h, P.well, S(4))
    outline(dl, x, y, w, h, P.well_edge, S(4), 0, 1)
    icon_centred(dl, x + S(13), cy, 'search', P.faint, 10)
    local searching = search.text[1] ~= ''
    local text_h = S(size)
    imgui.SetCursorScreenPos({ x + S(22), y - math.floor(text_h * 0.06 + 0.5) })
    imgui.SetNextItemWidth(w - S(22) - (searching and S(26) or S(4)))
    imgui.PushStyleColor(COL.FrameBg, rgba(P.well, 0))
    imgui.PushStyleColor(COL.FrameBgHovered, rgba(P.well, 0))
    imgui.PushStyleColor(COL.FrameBgActive, rgba(P.well, 0))
    imgui.PushStyleVar(VAR.FramePadding, { S(6), math.max(0, (h - text_h) / 2) })
    imgui.PushStyleVar(VAR.FrameBorderSize, 0)
    if search.focus then
        imgui.SetKeyboardFocusHere()
        search.focus = false
    end
    imgui.InputTextWithHint(id .. '.search', 'Search', search.text, 64)
    imgui.PopStyleVar(2)
    imgui.PopStyleColor(3)
    if searching and M.icon_button(id .. '.search.clear', x + w - S(13), cy, 'close', 10, P.rose, P.faint) then
        search.text[1] = ''
        search.focus = true
    end
    imgui.SetCursorScreenPos({ x, y + h + S(6) })
    local words = {}
    for word in search.text[1]:lower():gmatch('%S+') do
        words[#words + 1] = word
    end
    return words
end

-- A drop-down's list's width, set as it opens and kept until it closes (ImGui fitting it to its rows each frame had it
-- change as a font list scrolled: a font's name is in its own font only once its row is in sight, Segoe UI before).
-- It fits the longest pick (a font's name in its font if that's loaded, else a quarter wider than in Segoe UI) and the
-- longest category's name, between `w` (look.list_w) and 360 px.
local drop_w = {} -- id -> px, while its list is open
local function drop_list_width(options, headers, size, w, look)
    local frame = imgui.GetFrameCount()
    local widest, header_w = 0, 0
    for _, option in ipairs(options) do
        if option.header then
            if headers > 0 then
                header_w = math.max(header_w, M.text_spaced(nil, 0, 0, option.header:upper(), nil, header_font(), look and SMALL or SMALL - 1, 1, S(1.2)))
            end
        elseif option[1] ~= nil and option[2] ~= nil then
            local face = option.face ~= nil and faces.added[option.face] or nil
            if face and face[2] < frame then
                widest = math.max(widest, measure(option[2], face[1], size))
            else
                widest = math.max(widest, measure(option[2], fonts.text, size) * (option.face ~= nil and 1.25 or 1))
            end
        end
    end
    -- wide enough for its longest category's name and a bit of its line after it ("SUPER ULTRAWIDE" was cut at the box's
    -- width): the window's padding both sides, the gap, the line at least, as drawn in M.dropdown
    header_w = header_w > 0 and header_w + S(50) or 0
    local lo = math.max(w, S(look and look.list_w or 0), header_w)
    local hi = math.max(w, S(look and look.list_w or 360), header_w)
    -- the window's padding both sides (push_popup_style), the scrollbar and a little slack
    return math.max(lo, math.min(hi, widest + S(10) * 2 + S(6) + S(6)))
end

-- A drop-down with a real list: `options` are { { value, label } } (a { sep = true } draws a line, a { header = 'Name' } a
-- category's name, neither picked), `value` the one chosen. `look`: a bigger one ({ size: its text, h: its box's height,
-- list_w: its list's width at least, list_h: its height at most, search: a search box over its list, however short (false:
-- none, however long) }, the HUD Manager's element list); prefix: a word dim in the box before the pick, or icon: an icon
-- there, M.DROP_ICON_W wide (a True Font line's outline: DRAWN.outline); to_pick: the list opens scrolled to the pick.
-- The box ends at x + w and fits its text (the same padding each side, the chevron after it); the list under it is
-- `w` wide at least. A click opens ImGui's popup; a pick closes it. A list of M.DROP_SEARCH picks or more starts with a
-- search box, which has the keyboard: what's typed keeps the options with every word of it in their name, and Enter
-- picks the first of them (outlined). `full`: the box is all of `w`, whatever its text.
-- Returns value, changed, and the box's right edge.
function M.dropdown(id, x, cy, w, options, value, enabled, alpha_scale, from_left, full, look)
    local h = S(look and look.h or 24)
    local size = look and look.size or M.SEG_SIZE
    local y = cy - h / 2
    -- (look.compact: M.DROP_COMPACT's padding, chevron room and gap after its icon or word, a True Font line's size and outline)
    local fit = look ~= nil and look.compact and M.DROP_COMPACT or M.DROP_FIT
    local pad, chevron = S(fit.pad), S(fit.chevron)
    local shown, shown_face = tostring(value), nil
    local picks, headers = 0, 0
    for _, option in ipairs(options) do
        if option.header then
            headers = headers + 1
        elseif option[1] ~= nil then
            picks = picks + 1
            if option[1] == value then
                shown, shown_face = option[2], option.face
            end
        end
    end
    -- (a font list's pick in its own font)
    local shown_font = M.face(shown_face) or fonts.text
    local searchable = (look ~= nil and look.search) or (picks >= M.DROP_SEARCH and not (look ~= nil and look.search == false))
    local prefix, icon = look ~= nil and look.prefix or nil, look ~= nil and look.icon or nil
    local prefix_w = icon ~= nil and S(M.DROP_ICON_W) + S(fit.gap) or prefix ~= nil and measure(prefix, fonts.text, size) + S(fit.gap) or 0
    local text_w = measure(shown, shown_font, size) + prefix_w
    -- the text (with a little slack, so measuring it a hair wide doesn't cut it to "O..."), a gap, the chevron (8 wide)
    local bw = full and w or math.min(w, math.max(S(64), text_w + pad * 2 + S(6) + S(8) + S(10)))
    local bx = from_left and x or x + w - bw
    local hovered, clicked = hit(id, bx, y, bw, h)
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    local open = imgui.IsPopupOpen ~= nil and imgui.IsPopupOpen(id .. '.list') or false
    fill(dl, bx, y, bw, h, P.well, S(4), 0, alpha)
    outline(dl, bx, y, bw, h, (hovered or open) and enabled and P.accent_lo or P.well_edge, S(4), 0, 1, alpha)
    -- the text a touch above the middle (its box holds a descent), the chevron level with it and `pad` from the edge
    -- (the icon font sets its glyphs a pixel low and wide)
    if icon ~= nil then
        icon_centred(dl, bx + pad + S(M.DROP_ICON_W) / 2, cy - S(0.5), icon, P.text2, size, alpha)
    elseif prefix ~= nil then
        text_mid(dl, bx + pad, cy - S(0.5), prefix, P.dim, fonts.text, size, alpha)
    end
    text_mid(dl, bx + pad + prefix_w, cy - S(0.5), M.fit(shown, bw - pad * 2 - chevron - prefix_w, shown_font, size), P.text2, shown_font, size, alpha)
    icon_centred(dl, bx + bw - pad - S(4) - 1, cy - S(1), open and 'up' or 'down', P.dim, look and look.h and 9 or 7.5, alpha)
    local changed = false
    if clicked and enabled then
        imgui.OpenPopup(id .. '.list')
    end
    -- the list under the box, as tall as the screen holds under it (it scrolls); over the box instead when there's more room
    -- there (a box near the screen's bottom)
    local top = y + h + S(2)
    -- (a category's name takes a header's room on top, so a list with them shows as many picks before it scrolls)
    local max_h = S(look and look.list_h or 420 + 30 * headers)
    local up = false
    local screen_ok, screen_w, screen_h = pcall(vana.ui.screen)
    if screen_ok then
        screen_w, screen_h = (screen_w and screen_w > 0) and screen_w or 1920, (screen_h and screen_h > 0) and screen_h or 1080
        local _, display_h = M.display(screen_w, screen_h)
        local below, above = display_h - top - S(10), y - S(2) - S(10)
        up = below < math.min(max_h, S(300)) and above > below
        max_h = math.max(S(120), math.min(max_h, up and above or below))
    end
    local list_size = look and look.size or BODY - 0.5
    imgui.SetNextWindowPos({ from_left and bx or x, up and y - S(2) or top }, 0, { 0, up and 1 or 0 })
    if open or (clicked and enabled) then
        -- one width while it's open (drop_list_width)
        local list_w = drop_w[id]
        if list_w == nil then
            list_w = drop_list_width(options, headers, list_size, w, look)
            drop_w[id] = list_w
        end
        imgui.SetNextWindowSizeConstraints({ list_w, 0 }, { list_w, max_h })
    else
        drop_w[id] = nil
    end
    M.push_popup_style()
    if imgui.BeginPopup(id .. '.list') then
        push(fonts.text, list_size)
        local list, first = options, nil -- (first: what Enter picks)
        if searchable then
            local words = drop_search_box(id, list_size)
            list = drop_filtered(options, words)
            if #words > 0 then
                for _, option in ipairs(list) do
                    if first == nil and not option.header then
                        first = option
                    end
                end
                if first == nil then
                    imgui.TextDisabled('Nothing matches')
                elseif imgui.IsKeyPressed(0x0D, false) then -- (VK_RETURN)
                    changed = first[1] ~= value
                    value = first[1]
                    imgui.CloseCurrentPopup()
                end
            end
        end
        for i, option in ipairs(list) do
            if option.sep then
                imgui.Separator()
            elseif option.header then
                -- a category: its name in small spaced capitals, in grey, and a line after it, close over its first pick
                local hx, hy = imgui.GetCursorScreenPos()
                local hw = (imgui.GetContentRegionAvail())
                local hh = S(i > 1 and 22 or 16)
                imgui.Dummy({ hw, hh })
                local hdl = imgui.GetWindowDrawList()
                local hcy = hy + hh - S(9)
                -- (2 in from where a pick's highlight starts, half the item spacing left of its text)
                local tw = M.text_spaced(hdl, hx - S(2), hcy, option.header:upper(), P.dim, header_font(), look and SMALL or SMALL - 1, 1, S(1.2))
                M.hline(hdl, hx - S(2) + tw + S(8), hx + hw - S(4), hcy, P.faint, 0.6)
            else
                local selected = option[1] == value
                -- a font's name in that font, once its row is in sight (a font is loaded only as it's shown)
                local face = option.face ~= nil and imgui.IsRectVisible({ 1, S(list_size) }) and M.face(option.face) or nil
                local shift = face ~= nil and cap_shift(face, list_size) or 0
                local picked
                if shift ~= 0 then
                    -- (its name drawn here instead, moved down to sit level with the Segoe UI rows': cap_shift)
                    local tx, ty = imgui.GetCursorScreenPos()
                    push(face, list_size)
                    picked = imgui.Selectable('##' .. i, selected)
                    pop()
                    text(imgui.GetWindowDrawList(), tx, ty + shift, option[2], P.text, face, list_size)
                else
                    if face ~= nil then
                        push(face, list_size)
                    end
                    picked = imgui.Selectable(option[2] .. '##' .. i, selected)
                    if face ~= nil then
                        pop()
                    end
                end
                if selected and look ~= nil and look.to_pick and imgui.IsWindowAppearing() then
                    imgui.SetScrollHereY(0.5)
                end
                if picked then
                    if not selected then
                        value = option[1]
                        changed = true
                    end
                    imgui.CloseCurrentPopup()
                end
                if option == first then
                    local x0, y0 = imgui.GetItemRectMin()
                    local x1, y1 = imgui.GetItemRectMax()
                    outline(imgui.GetWindowDrawList(), x0, y0, x1 - x0, y1 - y0, P.accent_lo, 0, 0, 1)
                end
            end
        end
        pop()
        imgui.EndPopup()
    end
    M.pop_popup_style()
    return value, changed, bx + bw
end

M.STEP_W = 22 -- dropdown_arrows' < and > buttons
M.STEP_GAP = 3

-- the width dropdown_arrows takes for `labels`: the box fits the longest, so the arrows stay put while flicking
function M.dropdown_arrows_width(labels)
    local longest = 0
    for _, label in ipairs(labels) do
        longest = math.max(longest, measure(label, fonts.text, M.SEG_SIZE))
    end
    return longest + S(6) * 2 + S(14) + (S(M.STEP_W) + S(M.STEP_GAP)) * 2
end

-- A drop-down between < and > buttons that step to the option before / after it (stopping at the ends), `w` wide from x.
-- Returns value, changed.
function M.dropdown_arrows(id, x, cy, w, options, value, enabled, alpha_scale)
    local h, bw, gap = S(24), S(M.STEP_W), S(M.STEP_GAP)
    local y = cy - h / 2
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    local values, at = {}, 0
    for _, option in ipairs(options) do
        if not option.sep and not option.header then
            values[#values + 1] = option[1]
            if option[1] == value then
                at = #values
            end
        end
    end
    local changed = false
    local function side(sx, name, by)
        -- (a value not in the list: < goes to the last, > to the first)
        local to = at == 0 and (by > 0 and 1 or #values) or at + by
        local can = enabled and to >= 1 and to <= #values
        local hovered, clicked = hit(id .. '.' .. name, sx, y, bw, h)
        fill(dl, sx, y, bw, h, P.well, S(4), 0, alpha)
        outline(dl, sx, y, bw, h, hovered and can and P.accent_lo or P.well_edge, S(4), 0, 1, alpha)
        chevron(dl, sx + bw / 2, cy, name, hovered and can and P.text or P.dim, alpha * (can and 1 or 0.4))
        if clicked and can then
            value, changed = values[to], true
        end
    end
    side(x, 'left', -1)
    side(x + w - bw, 'right', 1)
    local picked, moved = M.dropdown(id, x + bw + gap, cy, w - (bw + gap) * 2, options, value, enabled, alpha_scale, true, true)
    if moved then
        value, changed = picked, true
    end
    return value, changed
end

-- a colour swatch and its text, the pair ending at `right`; `open`: its picker is open. Returns clicked.
function M.swatch(id, right, cy, hex, label, enabled, open, alpha_scale)
    local h = S(20)
    local y = cy - h / 2
    local tw = measure(label, fonts.text, SMALL + 0.5)
    local sw_ = S(16)
    local w = sw_ + S(7) + tw
    local x = right - w
    local hovered, clicked = hit(id, x, y, w, h)
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    if open then
        fill(dl, x - S(4), y, w + S(8), h, P.hover, S(4))
    end
    fill(dl, x, y + S(2), sw_, h - S(4), hex, S(3), 0, alpha)
    outline(dl, x, y + S(2), sw_, h - S(4), (hovered or open) and enabled and P.accent or '#FFFFFF', S(3), 0, 1, ((hovered or open) and 0.8 or 0.15) * alpha)
    text_right_mid(dl, right, cy, label, P.text2, fonts.text, SMALL + 0.5, alpha)
    return clicked and enabled
end

M.ARROWS_W = 110

-- a left / right selector: chevrons either side of the text; returns -1, 1 (an end pressed) or 0
function M.arrows(id, x, cy, text_, enabled, alpha_scale)
    local w, h = S(M.ARROWS_W), S(24)
    local y = cy - h / 2
    local bw = S(24)
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled and 1 or 0.4) * (alpha_scale or 1)
    local step = 0
    fill(dl, x, y, w, h, P.well, S(4), 0, alpha)
    outline(dl, x, y, w, h, P.well_edge, S(4), 0, 1, alpha)
    local function side(sx, name, corners, by)
        local hovered, clicked = hit(id .. name, sx, y, bw, h)
        if clicked and enabled then
            step = by
        end
        if hovered and enabled then
            fill(dl, sx + 1, y + 1, bw - 2, h - 2, '#FFFFFF', S(3), corners, 0.07)
        end
        chevron(dl, sx + bw / 2, cy, name, hovered and enabled and P.text or P.dim, alpha)
    end
    side(x, 'left', CORNERS.left, -1)
    side(x + w - bw, 'right', CORNERS.right, 1)
    text_centred(dl, x + bw, y, w - bw * 2, h, text_, P.text2, fonts.text, M.SEG_SIZE, alpha)
    return step
end

-- the width a button of `label` takes
function M.button_width(label, icon_name)
    return measure(label, fonts.text, SMALL + 0.5) + (icon_name and S(14) or 0) + S(18)
end

-- a small button: 'ghost' (a border), 'accent' (filled), 'secondary' (filled, quiet), 'soft' / 'neutral' / 'danger' (tinted,
-- bordered, the same shape) or 'banner'; `height`
-- when not the usual 22; `font` and `size` when not the usual text (with a `width` that fits them: M.button_width measures
-- the usual). Returns clicked, the width it took
function M.button(id, x, cy, label, icon_name, kind, enabled, alpha_scale, height, width, font, size)
    local h = S(height or 22)
    local y = cy - h / 2
    -- wider when asked: the content stays centred; an icon only, as wide as asked (a square), no room for a label
    local w = (label == '' and icon_name) and (width or S(26)) or math.max(M.button_width(label, icon_name), width or 0)
    local hovered, clicked, active = hit(id, x, y, w, h)
    local dl = imgui.GetWindowDrawList()
    local alpha = (enabled == false and 0.4 or 1) * (alpha_scale or 1)
    local t = ease(id, hovered and 1 or 0, 22)
    local tint = (kind == 'soft' and P.accent) or (kind == 'danger' and P.rose) or (kind == 'neutral' and P.text2)
        or (kind == 'success' and P.green) or nil
    if tint then
        -- soft / neutral / danger / success: one construction (a faint wash of the colour, a border of it, its text), so a row of them matches
        fill(dl, x, y, w, h, mix(P.well, tint, 0.10 + 0.10 * t), S(4), 0, alpha)
        outline(dl, x, y, w, h, tint, S(4), 0, 1, (0.45 + 0.25 * t) * alpha)
    elseif kind == 'accent' then
        fill(dl, x, y, w, h, mix(P.accent, P.accent2, t * 0.6), S(4), 0, alpha)
        outline(dl, x, y, w, h, '#FFFFFF', S(4), 0, 1, 0.22 * alpha)
    elseif kind == 'banner' then
        fill(dl, x, y, w, h, mix(P.banner_button, P.banner_button_hi, t), S(5), 0, alpha)
    elseif kind == 'secondary' then
        fill(dl, x, y, w, h, mix(P.seg, '#3D4463', t), S(4), 0, alpha)
        outline(dl, x, y, w, h, '#FFFFFF', S(4), 0, 1, 0.12 * alpha)
    else
        fill(dl, x, y, w, h, mix(P.well, P.hover, t), S(4), 0, alpha)
        outline(dl, x, y, w, h, mix(P.well_edge, P.accent_lo, t), S(4), 0, 1, alpha)
    end
    if active then
        fill(dl, x, y, w, h, '#000000', S(4), 0, 0.15)
    end
    local fg = kind == 'accent' and P.ink or tint and mix(tint, '#FFFFFF', t * 0.35) or (kind == 'banner' or kind == 'secondary') and P.text or mix(P.text2, P.text, t)
    font, size = font or fonts.text, size or SMALL + 0.5
    local content = measure(label, font, size) + (icon_name and S(14) or 0)
    local at = x + math.floor((w - content) / 2)
    if icon_name and label == '' then -- an icon only: in the middle
        icon_centred(dl, x + w / 2, cy, icon_name, fg, 10, alpha)
        return clicked and enabled ~= false, w
    end
    if icon_name then
        -- a glyph a pixel lower: centred on its line it sits above the label's letters (a drawn icon is centred on cy)
        icon_centred(dl, at + S(4), DRAWN[icon_name] and cy or cy + S(1), icon_name, fg, 10, alpha)
        at = at + S(14)
    end
    text_mid(dl, at, cy, label, fg, font, size, alpha)
    return clicked and enabled ~= false, w
end

-- a small icon that acts as a button; returns clicked, hovered
function M.icon_button(id, cx, cy, icon_name, size, hover_color, color, enabled)
    local r = S(10)
    local hovered, clicked = hit(id, cx - r, cy - r, r * 2, r * 2)
    local dl = imgui.GetWindowDrawList()
    local t = ease(id, hovered and enabled ~= false and 1 or 0, 22)
    icon_centred(dl, cx, cy, icon_name, mix(color or P.dim, hover_color or P.text, t), size or 10, enabled == false and 0.4 or 1)
    return clicked and enabled ~= false, hovered
end

-- Icon switches along a row, right to left from `right` (the last's right edge), each { key, label, icon, icon_on, tip,
-- tip_on, get, set }: its icon_on in the accent's blue while on, its icon in grey while off, a click flipping it, its
-- label and what it does now in its tooltip (an addon window's Lock and No Pointer Events: data.lua layout.flags_of).
-- Returns the room they take.
M.ICON_SWITCH_W = 22
function M.icon_switches(list, id, right, cy, enabled)
    local cx = right - S(M.ICON_SWITCH_W) / 2
    for i = #list, 1, -1 do
        local item = list[i]
        local ok, on = pcall(item.get)
        on = ok and on == true
        local clicked, hovered = M.icon_button(id .. '.' .. item.key, cx, cy, on and item.icon_on or item.icon, 11,
                                               on and P.accent2 or P.text2, on and P.accent or P.faint, enabled)
        if hovered then
            tooltip(item.label .. (on and ': on\n' .. item.tip_on or ': off\n' .. item.tip))
        end
        if clicked then
            pcall(item.set, not on)
        end
        cx = cx - S(M.ICON_SWITCH_W)
    end
    return S(M.ICON_SWITCH_W) * #list
end

M.METER_W = 200

function M.meter(dl, x, cy, fraction, label, color)
    local value_w = S(76)
    local w, h = S(M.METER_W) - value_w - S(8), S(4)
    fill(dl, x, cy - h / 2, w, h, P.off, h / 2)
    fill(dl, x, cy - h / 2, math.max(h, w * fraction), h, color or P.accent, h / 2)
    text_right_mid(dl, x + w + S(8) + value_w, cy, label, P.dim, fonts.text, SMALL)
end

-- a keycap: "Ctrl", "F10"
function M.keycap(dl, x, cy, label)
    local kw = measure(label, fonts.semi, SMALL - 0.5) + S(10)
    fill(dl, x, cy - S(9), kw, S(18), P.seg, S(3))
    text_centred(dl, x, cy - S(9), kw, S(18), label, P.text, fonts.semi, SMALL - 0.5)
    return kw
end

-- a pill: a short label in a rounded box after a name ("DX12": what the setting needs); returns its width and height
function M.pill(dl, x, cy, label, alpha)
    local pw, ph = measure(label, fonts.semi, SMALL - 2) + S(10), S(15)
    fill(dl, x, cy - ph / 2, pw, ph, P.accent_bg, ph / 2, 0, alpha)
    outline(dl, x, cy - ph / 2, pw, ph, P.accent_lo, ph / 2, 0, 1, alpha)
    text_centred(dl, x, cy - ph / 2, pw, ph, label, P.accent2, fonts.semi, SMALL - 2, alpha)
    return pw, ph
end
