--[[
Checks VanaCore's LuaJIT change for C functions declared with fewer parameters than they have (x86 Windows):
LuAshitacast's encoding.lua declares WideCharToMultiByte with 7 of its 8 parameters, so Windows reads the 8th
(lpUsedDefaultChar) from whatever is on the stack past the arguments and writes through it.

  - the interpreter: the stack past the arguments is zeroed, so the 8th reads NULL; the stack is dirtied with an
    unmapped address first (a vararg call that leaves it there), as an earlier call would
  - the JIT: such a function (it pops more than its declaration passes) is kept out of compiled code, which would
    readjust the stack by the declared amount; a correctly declared one (MultiByteToWideChar) still compiles

Run: build\obj\luatest\luarun.exe tools\luatest\ffi_stack_test.lua (build it as run.cmd does)
--]]

local ffi = require('ffi')

-- LuAshitacast's declarations, as they are
ffi.cdef[[
    int MultiByteToWideChar(uint32_t CodePage, uint32_t dwFlags, char* lpMultiByteStr, int cbMultiByte, wchar_t* lpMultiByteStr, int32_t cchWideChar);
    int WideCharToMultiByte(uint32_t CodePage, uint32_t dwFlags, wchar_t* lpWideCharString, int32_t cchWideChar, char* lpMultiByteStr, int32_t cbMultiByte, char lpDefaultChar);
    int sprintf(char* buffer, const char* format, ...);
]]

-- LuAshitacast's encoding.lua UTF8_To_ShiftJIS, as it is
local function UTF8_To_ShiftJIS(input)
    local buffer = ffi.new('char[4096]')
    ffi.copy(buffer, input)
    local wBuffer = ffi.new('wchar_t[4096]')
    ffi.C.MultiByteToWideChar(65001, 0, buffer, -1, wBuffer, 4096)
    ffi.C.WideCharToMultiByte(932, 0, wBuffer, -1, buffer, 4096, 0)
    return ffi.string(buffer)
end

-- A vararg call with many arguments: it leaves them on the stack, where the next call's unused slots are
local scratch = ffi.new('char[512]')
local bad = ffi.cast('void*', 0x65B1F900) -- unmapped: writing through it is an access violation
local function dirty()
    ffi.C.sprintf(scratch, '%p %p %p %p %p %p %p %p %p %p %p %p %p %p %p %p', bad, bad, bad, bad, bad, bad, bad, bad,
        bad, bad, bad, bad, bad, bad, bad, bad)
end

local events = { stop = 0, abort = 0 }
jit.attach(function (what) events[what] = (events[what] or 0) + 1 end, 'trace')

local utf8 = '\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88' -- テスト
local sjis = '\x83\x65\x83\x58\x83\x67'

-- 1. interpreter, stack dirtied first
jit.off()
dirty()
local out = UTF8_To_ShiftJIS(utf8)
assert(out == sjis, 'interpreter: wrong conversion')
print('interpreter: converted after a dirty stack, no crash')
jit.on()

-- 2. a hot loop, interleaved with dirtying calls: compiled where it can be
local failures = 0
for i = 1, 20000 do
    if i % 7 == 0 then
        dirty()
    end
    if UTF8_To_ShiftJIS(utf8) ~= sjis then
        failures = failures + 1
    end
end
assert(failures == 0, ('hot loop: %d wrong conversions'):format(failures))
print(('hot loop: 20000 conversions, all right (traces: %d compiled, %d aborted)'):format(events.stop, events.abort))

-- 3. a correctly declared Windows function still compiles: MultiByteToWideChar alone in a loop
events = { stop = 0, abort = 0 }
local wide = ffi.new('wchar_t[64]')
local input = ffi.new('char[16]', 'abc')
local total = 0
for i = 1, 20000 do
    total = total + ffi.C.MultiByteToWideChar(65001, 0, input, -1, wide, 64)
end
assert(total == 20000 * 4, 'MultiByteToWideChar loop: wrong results')
assert(events.stop > 0, 'MultiByteToWideChar loop: nothing compiled (it should be)')
print(('correct declaration: compiled (%d traces)'):format(events.stop))

print('ffi_stack_test: all passed')
