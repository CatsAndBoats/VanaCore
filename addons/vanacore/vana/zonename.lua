--[[
vana\zonename.lua: the zone's name, and the region's above it, in the middle of the screen when you zone in, animating in and out
(Game Options > Zones > Zone Name and Region Name; [client] zonename*).

The zone's name is from vana.gamedata.zones (a zone it has no name for shows no name); the region's is from REGIONS below (a zone
in no region shows none). Each line has its own switch ([client] zonename, zonename.region) and its own look (font, size,
colours, place: zonename.* and zonename.region.*); the animation, the seconds and the login switch are the two's together.
It comes from the game's own events: `zone_change` (every zone-in after login, leaving the Mog House too; going into it shows
nothing) and `login` (the first zone-in, with [client] zonename.login). The zone-in packet comes before the zone has loaded, so
the name waits:

  1. the party list has the new zone for the player (it lags the zone-in by the seconds the zone takes to load), and the game
     is no longer zoning (vana.game.zoning), at least MIN_WAIT after the packet; if the list never says so (an instance, a zone
     whose id the party list spells another way), GIVE_UP seconds after it anyway;
  2. then SETTLE more, for the game's own fade up from black.

Then it animates in, holds ([client] zonename.seconds), and animates out ([client] zonename.animation):
  fade   its strength up, then down
  scale  grows from half its size to full (the box's size is 6 to 128 pixels, so it can't grow past what's set), shrinks away
  slide  comes in from a side ([client] zonename.slide: left, right, top, bottom) and leaves by another ([client] zonename.slide.to:
         those, or opposite, the default)
  dissolve  the picture is eaten away in a noise, a burning edge (orange to white-hot, [client] zonename.dissolve.color, with
         zonename.dissolve.burn) along the front: the box's dissolve and dissolve_edge, VanaCore.Core.dll's since 2026-11-13
         (a DLL without them fades)
  wave   a ripple runs through the whole name (one string, its picture's columns moved up and down by the renderer: the box's
         wave, wave_length and wave_phase, VanaCore.Core.dll's since 2026-11-13), strong as it comes and goes, and gentle while it
         stays (wave: continuous) or still once it's in (wave_io: waves in and out only). Without those in the DLL it fades
A zone-out ends it at once, and a zone-in while it waits starts it again for the new zone. A zone's look (font, size, colours,
place, animation) is read when its name starts: a change applies from the next zone.

The panel has two buttons: Show (M.zonename_show) holds the name on screen, as it'll look, for setting its place, size, font and
colours: it follows the settings as they change, and goes when Show is pressed again. Preview (M.zonename_preview) plays it as it
runs when you zone. Both show the lines that are switched on (the zone's name alone when neither is).

A gradient runs the way its Direction ([client] <prefix>.gradient.direction) says. Left to right (horizontal, the default) is one
text box with colour codes (color_codes): each letter its own colour from the first colour to the second. Top to bottom
(vertical) is the box's own gradient (box.gradient = 'down', in a DLL built after 13 November 2026): the first colour at the top
of the letters, the second at their baseline, shaded by the pixel; a DLL without it draws left to right.

Loaded by vana.lua with `local M = ...`.
--]]

local M = ...

-- each animation's seconds in and out
local ANIMATIONS = { fade = { 0.6, 1.6 }, scale = { 0.5, 0.9 }, slide = { 0.6, 0.9 }, wave = { 0.7, 1.1 }, wave_io = { 0.7, 1.1 }, dissolve = { 1.0, 1.4 } }
local SLIDES = { left = { -1, 0 }, right = { 1, 0 }, top = { 0, -1 }, bottom = { 0, 1 } } -- the side it comes in from
local MIN_WAIT = 1.0               -- seconds after the zone-in packet before the zone can count as loaded
local GIVE_UP = 12                 -- seconds after it that the zone counts as loaded whatever the party list says
local SETTLE = 0.8                 -- seconds from loaded to the name, for the game's fade up from black
local DROP = 60                    -- seconds after which a zone-in that never loaded is forgotten

-- the two lines, drawn in this order (the region's above the zone's), each with where its settings are ([client] <prefix>.fontsize
-- and so on; the switch is the prefix itself) and what they are without one: data.lua's panel has the same
local LINE_KEYS = { 'region', 'zone' }
local LINES = {
    zone = { prefix = 'zonename', size = 60, y = 40, bold = 1, color = '#FFFFFF', color2 = '#FFC850', on = 1 },
    region = { prefix = 'zonename.region', size = 32, y = 31, bold = 0, color = '#FFD98A', color2 = '#FFFFFF', on = 0 },
}

-- the region each zone is in, as the game's own region names have it (the Windower and Ashita zonename addons' lists): each row
-- is the region's name, then the ids of its zones. A zone in none has no region to show.
local REGIONS = {
    { "Kingdom of San d'Oria", 230, 231, 232, 233 },
    { "Republic of Bastok", 234, 235, 236, 237 },
    { "Federation of Windurst", 238, 239, 240, 241, 242 },
    { "Grand Duchy of Jeuno", 243, 244, 245, 246 },
    { "Ronfaure", 100, 101, 139, 140, 141, 142, 167, 190 },
    { "Zulkheim", 102, 103, 108, 193, 196, 248 },
    { "Norvallen", 1, 2, 104, 105, 149, 150, 195 },
    { "Gustaberg", 106, 107, 143, 144, 172, 173, 191 },
    { "Derfland", 109, 110, 147, 148, 197 },
    { "Sarutabaruta", 115, 116, 145, 146, 169, 170, 192, 194 },
    { "Kolshushu", 3, 4, 117, 118, 198, 213, 249 },
    { "Aragoneu", 7, 8, 119, 120, 151, 152, 200 },
    { "Fauregandi", 9, 10, 111, 166, 203, 204, 206 },
    { "Valdeaunia", 5, 6, 112, 161, 162, 165 },
    { "Qufim", 126, 127, 157, 158, 179, 184 },
    { "Li'Telor", 121, 122, 153, 154, 202, 251 },
    { "Kuzotz", 114, 125, 168, 208, 209, 247 },
    { "Vollbow", 113, 128, 174, 201, 212 },
    { "Elshimo Lowlands", 123, 176, 250, 252 },
    { "Elshimo Uplands", 124, 159, 160, 163, 205, 207, 211 },
    { "Tu'Lia", 130, 177, 178, 180, 181 },
    { "Dynamis", 39, 40, 41, 42, 134, 135, 185, 186, 187, 188, 294, 295, 296, 297 },
    { "Movalpolos", 11, 12, 13 },
    { "Tavnazian Marquisate", 26 },
    { "Tavnazian Archipelago", 24, 25, 27, 28, 29, 30, 31, 32 },
    { "Promyvion", 14, 16, 17, 18, 19, 20, 21, 22, 23 },
    { "Lumoria", 33, 34, 35, 36 },
    { "Limbus", 37, 38 },
    { "Western Aradjiah", 58, 59 },
    { "West Aht Urhgan", 48, 50, 52, 71 },
    { "Mamool Ja Savagelands", 51, 65, 66, 67, 68 },
    { "Halvung Territory", 61, 62, 63, 64 },
    { "Arrapago Islands", 53, 54, 55, 56, 57, 60, 69, 78, 79 },
    { "Ruins of Alzadaal", 72, 73, 74, 75, 76, 77 },
    { "Ronfaure Front", 80, 81, 86 },
    { "Norvallen Front", 82, 84, 175 },
    { "Gustaberg Front", 87, 88, 89, 93 },
    { "Derfland Front", 83, 90, 91, 171 },
    { "Sarutabaruta Front", 94, 95, 96, 129 },
    { "Aragoneu Front", 97, 98, 164 },
    { "Fauregandi Front", 136 },
    { "Valdeaunia Front", 137 },
    { "Orcish Hosts", 85 },
    { "Quadav Shieldwarriors", 92 },
    { "Yagudo Theomilitary", 99 },
    { "Dark Kindred", 138, 155, 156 },
    { "Abyssea", 15, 45, 132, 215, 216, 217, 218, 253, 254, 255 },
    { "The Threshold", 182, 222, 298 },
    { "Abdhaljs", 43, 44, 183 },
    { "Sacred City of Adoulin", 256, 257, 258, 259, 283, 284 },
    { "Adoulin Isles", 280, 285 },
    { "East Ulbuka Territory", 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 281, 282 },
    { "Ra'Kaznar", 274, 275, 276, 277 },
    { "Escha", 288, 289 },
    { "The Far East", 291, 292, 293 },
    { "Gugru Blue", 46, 47 },
    { "Bastore Sea", 220, 221, 227, 228 },
    { "Quon", 223, 224, 225, 226 },
}

local region_of = {} -- zone id -> its region's name (a zone listed twice keeps its first)
for _, row in ipairs(REGIONS) do
    for i = 2, #row do
        if region_of[row[i]] == nil then
            region_of[row[i]] = row[1]
        end
    end
end

local pending = nil -- a zone-in waiting for its zone to load: { zone, at, ready }
local show = nil    -- the name on screen: { zone, names, start, held (Show's: it stays), look }
local boxes = {}    -- each line's text box, made the first time and kept
local drawn = { zone = {}, region = {} } -- what each box was last given: stamp, x, y, opacity, the wave's swing
local wave_ok = nil -- whether VanaCore.Core.dll's text boxes have a wave (found out the first time)
local dissolve_ok = nil -- and a dissolve
local gradient_ok = nil -- and a gradient of its own (top to bottom)
local failed = false -- an error in drawing is logged once

local function number(key, default, low, high)
    local value = tonumber(vana.config.get('client', key) or '')
    if value == nil then
        return default
    end
    return math.max(low, math.min(high, value))
end

local function text_of(key, default)
    local value = vana.config.get('client', key) or ''
    return value ~= '' and value or default
end

-- a colour setting: '#RRGGBB' (anything else is the default)
local function colour_of(key, default)
    local value = text_of(key, default)
    return value:match('^#%x%x%x%x%x%x') and value or default
end

-- whether a line is switched on
local function enabled(key)
    local line = LINES[key]
    return number(line.prefix, line.on, 0, 1) ~= 0
end

-- one line's look, as its settings are now
local function style_of(key)
    local line = LINES[key]
    local p = line.prefix .. '.'
    -- (the zone's Y was once zonename.height)
    local y = key == 'zone' and number('zonename.height', line.y, 0, 100) or line.y
    return {
        size = math.floor(number(p .. 'fontsize', line.size, 6, 128) + 0.5), -- pixels, in the interface's size
        x = number(p .. 'x', 50, 0, 100),
        y = number(p .. 'y', y, 0, 100),
        family = text_of(p .. 'family', 'Georgia'),
        bold = number(p .. 'bold', line.bold, 0, 1) ~= 0,
        italic = number(p .. 'italic', 0, 0, 1) ~= 0,
        upper = number(p .. 'uppercase', 0, 0, 1) ~= 0,
        outline = number(p .. 'outline', 1, 0, 3),
        outline_color = colour_of(p .. 'outline.color', '#000000'),
        color = colour_of(p .. 'color', line.color),
        gradient = number(p .. 'gradient', 0, 0, 1) ~= 0,
        -- which way the gradient runs: 'horizontal' (left to right, each letter its own colour) or 'vertical' (top to bottom)
        direction = text_of(p .. 'gradient.direction', 'horizontal') == 'vertical' and 'vertical' or 'horizontal',
        color2 = colour_of(p .. 'color2', line.color2),
    }
end

-- everything that shapes the name, as the settings are now: the animation's (the two lines'), then each line's own
local function look()
    local animation = text_of('zonename.animation', 'fade')
    local slide = text_of('zonename.slide', 'left')
    local slide_to = text_of('zonename.slide.to', 'opposite')
    return {
        seconds = number('zonename.seconds', 3, 0.5, 15),
        animation = ANIMATIONS[animation] ~= nil and animation or 'fade',
        slide = SLIDES[slide] ~= nil and slide or 'left',
        wave_speed = number('zonename.wavespeed', 1, 0.1, 5),
        burn = number('zonename.dissolve.burn', 1, 0, 1) ~= 0,
        burn_color = colour_of('zonename.dissolve.color', '#FF7A1A'),
        slide_to = SLIDES[slide_to] ~= nil and slide_to or 'opposite',
        zone = style_of('zone'),
        region = style_of('region'),
    }
end

-- the zone's name, or nil (none is known, or the zone is 0)
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

-- what to show for a zone: { zone = its name, region = its region's } for the lines that are switched on and have something to
-- say, or nil for nothing. `force` (Show and Preview): the zone's name alone when neither line is switched on
local function names_of(zone, force)
    local names = {
        zone = enabled('zone') and name_of(zone) or nil,
        region = enabled('region') and zone ~= nil and region_of[zone] or nil,
    }
    if force and not enabled('zone') and not enabled('region') then
        names.zone = name_of(zone)
    end
    if names.zone == nil and names.region == nil then
        return nil
    end
    return names
end

local function clamp01(x)
    return x < 0 and 0 or (x > 1 and 1 or x)
end
local function smooth(p) return p * p * (3 - 2 * p) end
local function ease_out(p) return 1 - (1 - p) ^ 3 end
local function ease_in(p) return p ^ 3 end

local function rgb(hex)
    return tonumber(hex:sub(2, 3), 16), tonumber(hex:sub(4, 5), 16), tonumber(hex:sub(6, 7), 16)
end

-- the colour a fraction `t` of the way from `from` to `to`, as three numbers
local function mix(from, to, t)
    local r1, g1, b1 = rgb(from)
    local r2, g2, b2 = rgb(to)
    return math.floor(r1 + (r2 - r1) * t + 0.5), math.floor(g1 + (g2 - g1) * t + 0.5), math.floor(b1 + (b2 - b1) * t + 0.5)
end

local function letters_of(name)
    local list = {}
    for letter in name:gmatch('[%z\1-\127\194-\244][\128-\191]*') do
        list[#list + 1] = letter
    end
    return list
end

-- the name with each letter's colour code, left to right (the box's color_codes); `s`: the line's look
local function gradient_text(list, s)
    local parts = {}
    for i, letter in ipairs(list) do
        local r, g, b = mix(s.color, s.color2, #list > 1 and (i - 1) / (#list - 1) or 0)
        parts[#parts + 1] = string.format('\\cs(%d,%d,%d)%s', r, g, b, letter)
    end
    return table.concat(parts)
end

local function hide_line(key)
    local box = boxes[key]
    if box ~= nil and not box.destroyed then
        box:hide()
    end
    drawn[key] = {}
end

local function stop()
    show = nil
    for _, key in ipairs(LINE_KEYS) do
        hide_line(key)
    end
end

-- puts the name on screen from `now`: `zone` its zone's id, `names` its lines' text (nil with `held`, Show's: it stays, and
-- follows the settings as they change)
local function begin(zone, now, held, names)
    show = { zone = zone, names = names, start = now, held = held, look = look() }
end

-- whether the box can wave (a DLL without it raises an error at the property)
local function can_wave(box)
    if wave_ok == nil then
        wave_ok = pcall(function () box.wave = 0 end)
    end
    return wave_ok
end

-- whether the box can dissolve (a DLL without it raises an error at the property)
local function can_dissolve(box)
    if dissolve_ok == nil then
        dissolve_ok = pcall(function () box.dissolve = 0 end)
    end
    return dissolve_ok
end

-- whether the box can shade its letters itself, from the top of them to their baseline (a DLL without it raises an error at the
-- property; the gradient then runs left to right, as the letters' colour codes have it)
local function can_gradient(box)
    if gradient_ok == nil then
        gradient_ok = pcall(function () box.gradient = 'none' end)
    end
    return gradient_ok
end

-- one line: its box is made the first time it shows, and its text, style and place set only when they change. `l`: the
-- look, `pin` and `pout`: how far in and out the name is, 0 to 1 each
local function draw_line(key, text, l, pin, pout, now)
    local s = l[key]
    local name = s.upper and text:upper() or text
    local list = letters_of(name)
    local sw, sh = vana.ui.screen()
    local d = drawn[key]
    local alpha, size, dx, dy = smooth(pin) * (1 - smooth(pout)), s.size, 0, 0
    if l.animation == 'scale' then
        size = math.max(6, math.floor(s.size * (0.5 + 0.5 * ease_out(pin)) * (1 - 0.5 * ease_in(pout)) + 0.5))
    elseif l.animation == 'slide' then
        -- in from one side (its offset runs from the full distance to none) and out toward another (from none to the full distance)
        local from = SLIDES[l.slide]
        local to = l.slide_to == 'opposite' and { -from[1], -from[2] } or SLIDES[l.slide_to]
        local coming = (from[1] ~= 0 and sw or sh) * 0.12 * (1 - ease_out(pin))
        local going = (to[1] ~= 0 and sw or sh) * 0.12 * ease_in(pout)
        dx, dy = from[1] * coming + to[1] * going, from[2] * coming + to[2] * going
    end
    local stroke = math.max(0, math.min(8, math.floor(size / 14 * s.outline + 0.5)))
    local box = boxes[key]
    if box == nil or box.destroyed then
        box = vana.ui.text({ text = ' ', background = '#00000000', padding = 0, visible = false, layer = 40 })
        boxes[key] = box
        d = {}
        drawn[key] = d
    end
    -- top to bottom: the box shades the whole name itself; left to right: each letter its own colour (the colour codes)
    local down = s.gradient and s.direction == 'vertical' and can_gradient(box)
    local stamp = table.concat({ name, size, s.family, tostring(s.bold), tostring(s.italic), stroke, s.outline_color,
                                 s.color, tostring(s.gradient), s.color2, tostring(down) }, '|')
    if d.stamp ~= stamp then
        local by_letter = s.gradient and not down
        box:set({ text = by_letter and gradient_text(list, s) or name, color_codes = by_letter, font = s.family, size = size,
                  bold = s.bold, italic = s.italic, color = s.color, stroke = stroke, stroke_color = s.outline_color })
        if gradient_ok then
            box:set({ gradient = down and 'down' or 'none', gradient_from = s.color, gradient_to = s.color2 })
        end
        d.stamp = stamp
    end
    local x = math.floor(sw * s.x / 100 - box.width / 2 + dx)
    local y = math.floor(sh * s.y / 100 - box.height / 2 + dy)
    if d.x ~= x or d.y ~= y then
        box:set({ x = x, y = y })
        d.x, d.y = x, y
    end
    -- the dissolve: the picture eaten away in a noise, a burning edge along the front; its own strength stays full, it's the
    -- noise that takes it (a DLL without it fades)
    local dissolve = 0
    if l.animation == 'dissolve' and can_dissolve(box) then
        alpha = 1
        dissolve = math.max(1 - smooth(pin), smooth(pout))
    end
    if dissolve_ok and (dissolve ~= 0 or d.dissolve ~= 0) then
        box:set({ dissolve = dissolve, dissolve_edge = l.burn and l.burn_color or '#00000000' })
    end
    d.dissolve = dissolve
    if d.opacity ~= alpha then
        box.opacity = alpha
        d.opacity = alpha
    end
    -- the wave: a ripple along the whole name, travelling right, strong as it comes and goes and gentle while it stays (flat
    -- while Show holds it); one wavelength is four of the letters' size
    local swing = 0
    if (l.animation == 'wave' or l.animation == 'wave_io') and can_wave(box) and not show.held then
        -- stronger as it comes and goes; wave keeps a gentle one while it stays, wave_io none
        swing = size * 0.1 * ((l.animation == 'wave' and 1 or 0) + 3.5 * (1 - ease_out(pin)) + 3.5 * ease_in(pout))
    end
    if wave_ok and (swing ~= 0 or d.swing ~= 0) then
        box:set({ wave = swing, wave_length = size * 4, wave_phase = -(now - show.start) * 6 * l.wave_speed })
    end
    d.swing = swing
    box:show()
end

-- both lines, drawn together so they animate as one
local function draw(now)
    local l, names = show.look, show.names
    if show.held then
        l = look() -- (it follows the settings as they change)
        show.look = l
        names = names_of(show.zone, true)
        show.names = names
    end
    local tin, tout = ANIMATIONS[l.animation][1], ANIMATIONS[l.animation][2]
    local t = now - show.start
    local out_at = tin + l.seconds
    if not show.held and t >= out_at + tout then
        stop()
        return
    end
    -- how far in and out the name is, 0 to 1 each
    local pin = show.held and 1 or clamp01(t / tin)
    local pout = show.held and 0 or clamp01((t - out_at) / tout)
    for _, key in ipairs(LINE_KEYS) do
        local text = names ~= nil and names[key] or nil
        if text ~= nil then
            draw_line(key, text, l, pin, pout, now)
        else
            hide_line(key)
        end
    end
end

-- the zone's loaded for the player: the game isn't zoning, a player is there, and the party list has the new zone
local function loaded(zone, waited)
    if vana.game.zoning() or vana.game.self() == nil then
        return false
    end
    local party = vana.game.party()
    local me = party ~= nil and party.members[1] or nil
    return waited >= GIVE_UP or (me ~= nil and me.active and me.zone == zone)
end

local function frame()
    if pending == nil and show == nil then
        return
    end
    local now = vana.time.clock()
    if pending ~= nil then
        local waited = now - pending.at
        if waited >= DROP then
            pending = nil
        elseif pending.ready == nil then
            if waited >= MIN_WAIT and loaded(pending.zone, waited) then
                pending.ready = now
            end
        elseif now - pending.ready >= SETTLE then
            local zone = pending.zone
            local names = names_of(zone)
            pending = nil
            if names ~= nil then
                begin(zone, now, false, names)
            end
        end
    end
    if show ~= nil then
        local ok, err = pcall(draw, now)
        if not ok then
            stop()
            if not failed then
                failed = true
                vana.log('zonename: ' .. tostring(err))
            end
        end
    end
end

-- a zone-in: the zone's name and region's come once it has loaded
local function arrive(zone)
    stop()
    pending = nil
    if names_of(zone) ~= nil then
        pending = { zone = zone, at = vana.time.clock() }
    end
end

-- whether the last zone-in put the player in their Mog House (its packet's byte 0x80: 1 in it, 2 elsewhere)
local function in_mog_house()
    local data = vana.packets ~= nil and vana.packets.last ~= nil and vana.packets.last(true, 0x00A) or nil
    return type(data) == 'string' and data:byte(0x81) == 1
end

-- every zone-in but one into the Mog House (leaving it shows the city's name, as the zone id is the city's both ways)
vana.events.on('zone_change', function (e)
    if in_mog_house() then
        stop()
        pending = nil
    else
        arrive(e.zone)
    end
end)

vana.events.on('login', function (e)
    if number('zonename.login', 1, 0, 1) ~= 0 and not in_mog_house() then
        arrive(e.zone)
    else
        stop()
        pending = nil
    end
end)

vana.events.on('zone_out', function ()
    stop()
    pending = nil
end)

vana.events.on('logout', function ()
    stop()
    pending = nil
end)

vana.events.on('present', frame)

-- the zone the player is in (a sample, Lower Jeuno, before login)
local function current_zone()
    local _, zone = vana.game.self()
    return name_of(zone) ~= nil and zone or 245
end

-- the panel's Show button: the name held on screen to set it up by, until it's pressed again (or a zone change)
function M.zonename_show()
    pending = nil
    if show ~= nil and show.held then
        stop()
    else
        begin(current_zone(), vana.time.clock(), true)
    end
end

-- the panel's Preview button: the name as it runs when you zone (in, hold, out), whether it's switched on or not
function M.zonename_preview()
    pending = nil
    local zone = current_zone()
    begin(zone, vana.time.clock(), false, names_of(zone, true))
end
