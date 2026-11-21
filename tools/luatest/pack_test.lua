-- Windower's pack, as src\core\lua\compat\windower.lua has it: byte-for-byte checks of each format code, then (given
-- Windower's libraries) every packet its packets library knows, made with a value in each field, built and parsed
-- back the same. run.cmd builds luarun.exe and runs this:
--   luarun pack_test.lua <windower.lua> [Windower libs folder]
local source_path, libs = arg[1], arg[2]

local source = io.open(source_path, 'rb'):read('*a')
local section = source:match('%-%- >>> pack[^\n]*\n(.-)%-%- <<< pack')
assert(section, 'no pack section in ' .. source_path)
assert(loadstring(section, '@pack'))()

local failures, checks = 0, 0
local function check(ok, what)
    checks = checks + 1
    if not ok then
        failures = failures + 1
        print('FAIL ' .. what)
    end
end
local function hex(s)
    return (s:gsub('.', function (c) return ('%02X '):format(c:byte()) end))
end
local function same(got, want, what)
    check(got == want, ('%s: %s, want %s'):format(what, type(got) == 'string' and hex(got) or tostring(got),
        type(want) == 'string' and hex(want) or tostring(want)))
end
local function values(...)
    return { n = select('#', ...), ... }
end

-- the packet header: 9 bits of id, 7 of size, the sequence
same(('b9b7H'):pack(0x00A, 3, 0x1234), '\10\6\52\18', 'b9b7H pack')
local v = values(('\10\6\52\18'):unpack('b9b7H'))
same(v.n, 3, 'b9b7H unpack count (values only, no position)')
same(v[1], 0x00A, 'b9b7H id')
same(v[2], 3, 'b9b7H size')
same(v[3], 0x1234, 'b9b7H sequence')

-- numbers
same(table.concat({ ('I'):pack(0x01020304):unpack('CCCC') }, ','), '4,3,2,1', 'I then CCCC')
same(('h'):pack(-2), '\254\255', 'h -2')
same(('\254\255'):unpack('h'), -2, 'unpack h')
same(('\254\255'):unpack('H'), 65534, 'unpack H')
same(('i'):pack(-1):unpack('I'), 4294967295, 'i -1 as I')
same(('c'):pack(-1):unpack('c'), -1, 'c -1')
same(('C'):pack(255):unpack('C'), 255, 'C 255')
same(('l'):pack(-5):unpack('l'), -5, 'l -5')
same(('>H'):pack(0x1234), '\18\52', '>H')
same(('>I'):pack(1), '\0\0\0\1', '>I')
same(('>I'):pack(1):unpack('>I'), 1, '>I back')
same(('<H'):pack(0x1234), '\52\18', '<H')
same(('f'):pack(1.5):unpack('f'), 1.5, 'f 1.5')
same(('d'):pack(0.1):unpack('d'), 0.1, 'd 0.1')
same(('n'):pack(-123.25):unpack('n'), -123.25, 'n')
same(#('f'):pack(1), 4, 'f size')
same(#('d'):pack(1), 8, 'd size')
same(('>f'):pack(1.5):unpack('>f'), 1.5, '>f')
same(table.concat({ ('H*'):pack(1, 2, 3):unpack('H*') }, ','), '1,2,3', 'H* both ways')
same(table.concat({ ('H3'):pack(4, 5, 6):unpack('H3') }, ','), '4,5,6', 'H3 both ways')
same(('HH'):pack(1, 2):unpack('H', 3), 2, 'unpack from byte 3')
v = values(('H'):pack(1):unpack('HH'))
same(v.n, 1, 'stops where the data does')

-- booleans and bits
same(('BB'):pack(true, false), '\1\0', 'BB')
v = values(('\1\0'):unpack('BB'))
check(v[1] == true and v[2] == false, 'unpack BB')
same(('q3'):pack(true, false, true), '\5', 'q3')
v = values(('\5'):unpack('q3'))
check(v.n == 3 and v[1] == true and v[2] == false and v[3] == true, 'unpack q3')
v = values(('\255\3'):unpack('<q10'))
check(v.n == 10 and v[1] and v[10] and v[9], '<q10 gives ten booleans')
same(('b4C'):pack(0xF, 0xAB), '\15\171', 'bits then a byte starts the next byte')
same(('\0\240'):unpack('b4', 2, 5), 15, 'unpack from byte 2, bit 5')
same(('\0\240'):unpack('b4', 2), 0, 'unpack from byte 2, bit 1')
same(('b3b5'):pack(9, 1), '\9', 'b3 wraps (9 is 1 in 3 bits)')
same(('b1b1b1b1b1b1b1b1b1'):pack(1, 0, 0, 0, 0, 0, 0, 0, 1), '\1\1', 'nine single bits')
same(('b16'):pack(0xBEEF):unpack('H'), 0xBEEF, 'b16 is a little-endian word')
same(('\239\190\173\222'):unpack('b32'), 0xDEADBEEF, 'b32')
same(('b12'):pack(0xABC):unpack('b12'), 0xABC, 'b12 both ways')

-- strings
local name = ('S16'):pack('Vekien')
same(#name, 16, 'S16 is 16 bytes')
same(name:unpack('S16'), 'Vekien', 'S16 back to the name')
same(('S4'):pack('abcdef'), 'abcd', 'S4 cuts')
same(('A4'):pack('ab'), 'ab\0\0', 'A4 pads')
same(('ab\0\0'):unpack('A4'), 'ab\0\0', 'A4 as it is')
same(('xyz'):unpack('A*'), 'xyz', 'A* the rest')
same(('ab\0cd'):unpack('S*'), 'ab', 'S* up to a zero')
same(('z'):pack('hi'), 'hi\0', 'z')
v = values(('zH'):pack('hi', 5):unpack('zH'))
check(v[1] == 'hi' and v[2] == 5, 'zH both ways')
same(('p'):pack('abc'), '\3abc', 'p')
same(('\3abc'):unpack('p'), 'abc', 'unpack p')
same(('P'):pack('ab'):unpack('P'), 'ab', 'P both ways')
same(('a'):pack('ab'):unpack('a'), 'ab', 'a both ways')
same(('C C'):pack(1, 2), '\1\2', 'spaces are ignored')
check(pcall(string.pack, 'Y', 1) == false, 'an unknown code is an error')
check(require('pack') ~= nil, "require('pack')")

-- Windower's packets library: every packet it knows, made with its defaults and built, parses back the same
if libs then
    package.path = libs .. '\\?.lua;' .. package.path
    windower = { ffxi = { get_info = function () return { language = 'English' } end },
                 windower_path = libs .. '\\..\\', addon_path = libs .. '\\..\\' }
    _addon = { name = 'test' }
    -- the game's resources (names for labels) aren't needed to build and parse
    local empty = setmetatable({}, { __index = function () return nil end })
    package.loaded['resources'] = setmetatable({}, { __index = function () return empty end })
    local ok, packets = pcall(require, 'packets')
    if not ok then
        print('packets library did not load: ' .. tostring(packets))
        failures = failures + 1
    else
        local built, compared, skipped = 0, 0, 0
        for _, dir in ipairs({ 'incoming', 'outgoing' }) do
            for id = 0, 0x1FF do
                local fields = packets.raw_fields[dir][id]
                if type(fields) == 'table' then
                    local made_ok, packet = pcall(packets.new, dir, id)
                    if made_ok and not packet._error then
                        -- a value of its type in each field, so the bytes say something
                        local fields_now = packets.fields(packet)
                        local k = 0
                        for field in fields_now:it() do
                            k = k + 1
                            local ctype = field.ctype
                            if not field.const then
                                if ctype == 'bool' or ctype == 'boolbit' then
                                    packet[field.label] = k % 2 == 0
                                elseif ctype:match('^bit') then
                                    local width = tonumber(ctype:match('%[(%d+)%]') or 1)
                                    packet[field.label] = k % (2 ^ width)
                                elseif ctype:match('^char%[') then
                                    packet[field.label] = ('n%d'):format(k)
                                elseif ctype:match('^data') or ctype == 'char*' then
                                    -- left as made
                                elseif ctype == 'float' or ctype == 'double' then
                                    packet[field.label] = k + 0.5
                                elseif ctype:match('^signed') or ctype == 'int' or ctype == 'short' or ctype == 'char' then
                                    packet[field.label] = -k
                                else
                                    packet[field.label] = k % 200
                                end
                            end
                        end
                        local build_ok, raw = pcall(packets.build, packet)
                        if build_ok and raw then
                            built = built + 1
                            local parse_ok, parsed = pcall(packets.parse, dir, raw)
                            if not parse_ok then
                                check(false, ('%s 0x%03X parse: %s'):format(dir, id, tostring(parsed)))
                            else
                                same(parsed._id, id, ('%s 0x%03X id'):format(dir, id))
                                for field in fields_now:it() do
                                    local want, got = packet[field.label], parsed[field.label]
                                    local ctype = field.ctype
                                    if ctype:match('^signed') and type(want) == 'number' and ctype:match('char') then
                                        -- fine as it is
                                    end
                                    if type(want) == 'number' and type(got) == 'number' and ctype == 'float' then
                                        check(math.abs(want - got) < 1e-4, ('%s 0x%03X %s float'):format(dir, id, field.label))
                                    elseif ctype:match('^data') and (field.enc or type(want) == 'number') then
                                        -- encoded (Windower's own 6-bit text), or a number packed as raw bytes ("0",
                                        -- as Windower's own pack makes it): not a byte-for-byte comparison
                                    elseif ctype:match('^data%[') and want == '' then
                                        local size = tonumber(ctype:match('%[(.-)%]'))
                                        same(got, ('\0'):rep(size or 0), ('%s 0x%03X %s (%s) zeros'):format(dir, id, field.label, ctype))
                                    elseif not field.enc and want ~= nil and type(want) ~= 'table' then
                                        same(got, want, ('%s 0x%03X %s (%s)'):format(dir, id, field.label, ctype))
                                    end
                                    compared = compared + 1
                                end
                            end
                        else
                            skipped = skipped + 1
                        end
                    else
                        skipped = skipped + 1
                    end
                end
            end
        end
        print(('packets: %d built and parsed back, %d fields compared, %d skipped (made from a function or unbuildable)')
            :format(built, compared, skipped))
    end
end

print(('%d checks, %d failed'):format(checks, failures))
os.exit(failures == 0 and 0 or 1)
