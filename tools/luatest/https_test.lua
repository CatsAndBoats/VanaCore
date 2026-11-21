-- The https adapter in src\core\lua\compat\ashita.lua (socket.ssl.https over WinHTTP): drives the connection object the
-- way socket.http does (send the request, read the status line, the headers, the body) against a real https server, and
-- checks a failed connection says so. Needs a network. run.cmd builds luarun.exe and runs this:
--   luarun https_test.lua <ashita.lua>
local source_path = arg[1]

local source = io.open(source_path, 'rb'):read('*a')
local section = source:match('%-%- >>> https[^\n]*\n(.-)%-%- <<< https')
assert(section, 'no https section in ' .. source_path)
assert(loadstring(section, '@https'))()

local failures, checks = 0, 0
local function check(ok, what)
    checks = checks + 1
    if not ok then
        failures = failures + 1
        print('FAIL ' .. what)
    end
end

local https = require('socket.ssl.https')
check(type(https.tcp) == 'function' and type(https.request) == 'function', 'the module has tcp and request')
check(require('ssl.https') == https, 'ssl.https is the same module')

-- LuaSec itself is VanaCore's stand-in: its ssl.dll must never load (trustme requires 'socket.ssl' as it loads)
local ssl = require('socket.ssl')
check(require('ssl') == ssl, 'ssl is the same module as socket.ssl')
local wrapped, reason = ssl.wrap({}, {})
check(wrapped == nil and type(reason) == 'string', 'ssl.wrap returns nil and the reason')
check(not pcall(require, 'ssl.core'), "ssl.core (ssl.dll's) doesn't load")

-- what socket.http does, step by step
local function get(host, path)
    local c = https.tcp({})()
    c:settimeout(15)
    check(c:connect(host, 443) == 1, 'connect')
    c:send('GET ' .. path .. ' HTTP/1.1\r\nhost: ' .. host .. '\r\nuser-agent: test\r\nconnection: close\r\n\r\n')
    local status = c:receive(5)
    check(status == 'HTTP/', 'first five bytes are HTTP/: ' .. tostring(status))
    local line = c:receive('*l', status)
    local code = tonumber(line:match('^HTTP/%d%.%d (%d+)'))
    local headers = {}
    while true do
        local header = c:receive('*l')
        if header == nil or header == '' then break end
        local name, value = header:match('^([^:]+):%s*(.*)$')
        headers[name:lower()] = value
    end
    local body = c:receive('*a')
    return code, headers, body, c
end

local code, headers, body, c = get('example.com', '/')
check(code == 200, 'example.com answers 200: ' .. tostring(code))
check(tonumber(headers['content-length']) == #body, 'Content-Length is the body\'s: ' .. tostring(headers['content-length']) .. ' vs ' .. #body)
check(headers['transfer-encoding'] == nil, 'no Transfer-Encoding left')
check(body:find('Example Domain', 1, true) ~= nil, 'the page is there')
local more, why = c:receive('*a')
check(more == nil and why == 'closed', 'a read past the end says closed')

-- a redirect isn't followed (socket.http does that): the status and Location come back
local code2, headers2 = get('www.github.com', '/')
check(code2 == 301 or code2 == 302 or code2 == 200, 'github answers: ' .. tostring(code2))

-- a host that isn't there
local bad = https.tcp({})()
bad:connect('this-host-does-not-exist.invalid', 443)
bad:send('GET / HTTP/1.1\r\nhost: this-host-does-not-exist.invalid\r\n\r\n')
local got, err = bad:receive(5)
check(got == nil and type(err) == 'string', 'an unknown host fails with a message: ' .. tostring(err))

-- a request nobody can parse
local junk = https.tcp({})()
junk:connect('example.com', 443)
junk:send('not a request')
local got2, err2 = junk:receive('*l')
check(got2 == nil and err2 == 'malformed request', 'junk is refused: ' .. tostring(err2))

print(('%d checks, %d failed'):format(checks, failures))
os.exit(failures == 0 and 0 or 1)
