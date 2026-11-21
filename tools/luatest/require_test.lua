-- How an addon's require finds modules, as src\core\lua_api.cpp sets package.path: the C++ lines that build it are read
-- from the source and run here (so a change to them is tested), against a made-up VanaCore folder. An Ashita addon
-- gets Ashita's addons root too, addons\ashita\?.lua: bludex's require('bludex\\lib\\spellbook') is
-- addons\bludex\lib\spellbook.lua on Ashita. run.cmd builds luarun.exe and runs this:
--   luarun require_test.lua <lua_api.cpp> [ashita.lua]
local source_path, ashita_path = arg[1], arg[2]

local failures, checks = 0, 0
local function check(ok, what)
    checks = checks + 1
    if not ok then
        failures = failures + 1
        print('FAIL ' .. what)
    end
end

-- The package.path open() builds for an addon of this flavour ("VanaCore", "Ashita" or "Windower")
local source = io.open(source_path, 'rb'):read('*a')
local libs_line = 'const std::string libs = context.install + "addons\\\\" + kind + "\\\\libs\\\\";'
assert(source:find(libs_line, 1, true), 'lua_api.cpp: the line that makes libs has changed: ' .. libs_line)
local section = source:match('std::string path = (.-)set_string%(L, "path", path%);')
assert(section, 'no package.path section in ' .. source_path)

-- The package.path and package.cpath open() builds for an addon of this flavour, with these Ashita libraries
-- ([addons] ashita.libs: 4.30 if none)
local function path_for(flavour, folder, install, libs)
    libs = libs or 4.30
    local kind = flavour == 'Windower' and 'windower' or flavour == 'Ashita' and 'ashita' or 'vanacore'
    local vars = { ['context.folder'] = folder, ['context.install'] = install,
                   libs = install .. 'addons\\' .. kind .. '\\libs\\' }
    local function unquote(literal) return (literal:gsub('\\(.)', '%1')) end
    local function value(expr)
        local out = {}
        for term in expr:gmatch('[^+]+') do
            term = term:match('^%s*(.-)%s*$')
            local literal = term:match('^"(.*)"$')
            local above, yes, no = term:match('^%(context%.ashita_libs > ([%d.]+) %? "(.-)" : "(.-)"%)$')
            if literal then
                out[#out + 1] = unquote(literal)
            elseif above then
                out[#out + 1] = unquote(libs > tonumber(above) and yes or no)
            else
                assert(vars[term], 'lua_api.cpp: a term this test does not know in the package.path code: ' .. term)
                out[#out + 1] = vars[term]
            end
        end
        return table.concat(out)
    end
    local built, active, single, matched = {}, true, nil, nil
    for line in ('std::string path = ' .. section):gmatch('[^\r\n]+') do
        line = line:gsub('//.*$', '')
        local name, expr = line:match('^%s*std::string (%w+) = (.*);%s*$')
        local local_name, local_expr = line:match('^%s*const std::string (%w+) = (.*);%s*$')
        local add_name, add_expr = line:match('^%s*(%w+) %+= (.*);%s*$')
        if line:match('^%s*$') then
            -- a comment or nothing
        elseif name == 'path' or name == 'cpath' then
            built[name] = value(expr)
        elseif local_name then
            if active then vars[local_name] = value(local_expr) end
        elseif add_name == 'path' or add_name == 'cpath' then
            if active and single ~= false then built[add_name] = built[add_name] .. value(add_expr) end
            single = nil
        elseif line:match('^%s*if %(context%.flavour == Flavour::%w+%) {%s*$') then
            matched = line:match('Flavour::(%w+)') == flavour
            active = matched
        elseif line:match('^%s*if %(context%.flavour == Flavour::%w+%)%s*$') then -- no braces: the next statement only
            single = line:match('Flavour::(%w+)') == flavour
        elseif line:match('^%s*} else {%s*$') then
            active = not matched
        elseif line:match('^%s*}%s*$') then
            active = true
        elseif line:match('^%s*set_string%(L, "path", path%);%s*$') then
            -- the end of the section's text
        else
            error('lua_api.cpp: a line this test does not know in the package.path code: ' .. line)
        end
    end
    assert(built.path, 'lua_api.cpp: no package.path found')
    return built.path, built.cpath
end

-- A made-up VanaCore folder
local root = (os.getenv('TEMP') or '.') .. '\\luatest_require_' .. os.time()
local install = root .. '\\VanaCore\\'
local function write(relative, text)
    local full = install .. relative
    os.execute('mkdir "' .. full:match('^(.*)\\') .. '" >nul 2>nul')
    local file = assert(io.open(full, 'wb'))
    file:write(text)
    file:close()
end
write('addons\\ashita\\bludex\\bludex.lua', '')
write('addons\\ashita\\bludex\\lib\\spellbook.lua', "return { from = 'bludex lib' }")
write('addons\\ashita\\bludex\\helper.lua', "return { from = 'bludex folder' }")
write('addons\\ashita\\tools\\init.lua', "return { from = 'tools init' }")
-- the mirrored Ashita's own libraries: never found
write('addons\\ashita\\libs\\common.lua', "return { from = 'ashita libs' }")
write('addons\\ashita\\libs\\only.lua', "return { from = 'ashita libs' }")
-- VanaCore's: each interface's, then what both share
write('addons\\vanacore\\libs\\ashita\\common.lua', "return { from = 'vanacore shared' }")
write('addons\\vanacore\\libs\\ashita\\bitreader.lua', "return { from = 'vanacore shared' }")
write('addons\\vanacore\\libs\\ashita\\imgui.lua', "return { from = 'vanacore shared' }")
write('addons\\vanacore\\libs\\ashita-4.16\\imgui.lua', "return { from = 'vanacore 4.16' }")
write('addons\\vanacore\\libs\\ashita-4.30\\imgui.lua', "return { from = 'vanacore 4.30' }")
write('addons\\vanacore\\libs\\ashita-4.30\\ffxi\\targets.lua', "return { from = 'vanacore 4.30' }")
write('addons\\vanacore\\libs\\ashita\\dup.lua', "return { from = 'vanacore shared' }")
write('addons\\ashita\\dup.lua', "return { from = 'ashita root' }")

local function load_as(flavour, name, extra, interface)
    local folder = install .. 'addons\\ashita\\bludex\\'
    package.path = path_for(flavour, folder, install, interface) .. (extra or '')
    package.cpath = ''
    package.loaded[name] = nil
    local ok, module = pcall(require, name)
    return ok and module or nil, module
end

local function from(flavour, name, extra, libs)
    local module = load_as(flavour, name, extra, libs)
    return type(module) == 'table' and module.from or nil
end

-- the folder's own, then VanaCore's libraries (4.30's unless [addons] ashita.libs says 4.16), then the shared ones,
-- then Ashita's addons root
check(from('Ashita', 'helper') == 'bludex folder', "the addon's own folder")
check(from('Ashita', 'common') == 'vanacore shared', "VanaCore's shared libraries, not the mirrored Ashita's")
check(from('Ashita', 'only') == nil, "the mirrored Ashita's addons\\ashita\\libs is never searched")
check(from('Ashita', 'bitreader', nil, 4.16) == 'vanacore shared', 'bitreader with ashita.libs=4.16')
check(from('Ashita', 'imgui') == 'vanacore 4.30', "4.30's imgui by default")
check(from('Ashita', 'imgui', nil, 4.16) == 'vanacore 4.16', "4.16's imgui with ashita.libs=4.16")
check(from('Ashita', 'imgui', nil, 4.30) == 'vanacore 4.30', "4.30's imgui with ashita.libs=4.30")
check(from('Ashita', 'ffxi.targets') == 'vanacore 4.30', "4.30's additions by default")
check(from('Ashita', 'ffxi.targets', nil, 4.16) == nil, "not with ashita.libs=4.16")
check(from('Ashita', 'dup') == 'vanacore shared', "VanaCore's libraries come before the addons root")

-- bludex: require('bludex\\lib\\spellbook'), with a backslash, from the addons root
check(from('Ashita', 'bludex\\lib\\spellbook') == 'bludex lib', "require('bludex\\\\lib\\\\spellbook')")
check(from('Ashita', 'bludex.lib.spellbook') == 'bludex lib', "require('bludex.lib.spellbook')")
check(from('Ashita', 'tools') == 'tools init', "addons\\ashita\\tools\\init.lua")

-- an addon that adds the addons root itself, as <GetInstallPath()>addons\?.lua: with the trailing backslash Ashita's
-- has (the folder that holds no bludex here, since Ashita's addons are in addons\ashita), and without it, as VanaCore's
-- once returned it (VanaCoreaddons\?.lua); the root VanaCore adds finds it either way
check(from('Ashita', 'bludex\\lib\\spellbook', ';' .. install .. 'addons\\?.lua') == 'bludex lib',
    "an addon adds <install>addons\\?.lua")
check(from('Ashita', 'bludex\\lib\\spellbook', ';' .. root .. '\\VanaCoreaddons\\?.lua') == 'bludex lib',
    "an addon adds <install without a backslash>addons\\?.lua")

-- only Ashita's addons have that root
check(from('VanaCore', 'bludex\\lib\\spellbook') == nil, "a VanaCore addon doesn't see addons\\ashita")
check(from('Windower', 'bludex\\lib\\spellbook') == nil, "a Windower addon doesn't see addons\\ashita")

-- the Ashita path: the addon folder, then the libraries; DLLs from the addon's folder only
local ashita, ashita_c = path_for('Ashita', 'F\\', 'I\\')
check(ashita:find('F\\?.lua;I\\addons\\vanacore\\libs\\ashita-4.30\\?.lua;', 1, true) == 1,
    'the path starts with the addon folder and the libraries')
check(not ashita:find('addons\\ashita\\libs', 1, true), "the path has no addons\\ashita\\libs")
check(ashita_c == 'F\\?.dll', "an Ashita addon's DLLs come from its own folder only")
local _, windower_c = path_for('Windower', 'F\\', 'I\\')
check(windower_c == 'F\\?.dll;I\\addons\\windower\\libs\\?.dll', "a Windower addon's DLLs: its folder, then its libraries")

-- GetInstallPath keeps its trailing backslash (addons write GetInstallPath() .. 'addons\\?.lua')
if ashita_path then
    local text = io.open(ashita_path, 'rb'):read('*a')
    check(text:find("GetInstallPath = function %(%)%s*return vana%.paths%.install %.%. '\\\\'"),
        "AshitaCore:GetInstallPath() ends with a backslash")
end

os.execute('rmdir /s /q "' .. root .. '" >nul 2>nul')

print(('%d checks, %d failed'):format(checks, failures))
os.exit(failures == 0 and 0 or 1)
