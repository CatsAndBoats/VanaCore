local B = 'http://127.0.0.1:18080'
local W = 'ws://127.0.0.1:18081'
local passed, failed = 0, 0

local function check(name, cond, detail)
    if cond then
        passed = passed + 1
        print('  ok   ' .. name)
    else
        failed = failed + 1
        print('  FAIL ' .. name .. (detail and (' -> ' .. tostring(detail)) or ''))
    end
end

local function failure_of(p)
    local ok, err = pcall(function () return p:await() end)
    return not ok and err or nil
end

local TMP = os.getenv('TEMP') .. '\\vanacore-net-test'
os.execute('mkdir "' .. TMP .. '" 2>nul')

vana.async(function ()
    print('== basics')
    local res = vana.http.get(B .. '/hello'):await()
    check('get ok', res.ok and res.status == 200 and res.body == 'hello world', res.body)
    check('reason', res.reason == 'OK', res.reason)
    check('headers any case', res.headers['Content-Type'] == 'text/plain' and res:header('CONTENT-TYPE') == 'text/plain')
    check('url', res.url == B .. '/hello', res.url)

    local j = vana.http.get(B .. '/json'):await():json()
    check('json()', j.a == 1 and j.b[3] == 3 and j.s == 'x')
    local j2 = vana.http.json(B .. '/json'):await()
    check('http.json', j2.a == 1)

    -- callback style
    local got
    vana.http.get(B .. '/hello', function (r, e) got = { r, e } end)
    local t0 = vana.time.clock()
    while not got and vana.time.clock() - t0 < 5 do coroutine.sleep(0.05) end
    check('callback style', got and got[1] and got[1].body == 'hello world' and got[2] == nil)

    local cbe
    vana.http.get('http://127.0.0.1:1/', { timeout = 3 }, function (r, e) cbe = { r, e } end)
    t0 = vana.time.clock()
    while not cbe and vana.time.clock() - t0 < 6 do coroutine.sleep(0.05) end
    check('callback error style', cbe and cbe[1] == nil and cbe[2] and cbe[2].code == 'connect', cbe and cbe[2] and cbe[2].code)

    -- next chain
    local chained
    vana.http.get(B .. '/json'):next(function (r) return r:json().a end):next(function (a) chained = a + 1 end)
    t0 = vana.time.clock()
    while not chained and vana.time.clock() - t0 < 5 do coroutine.sleep(0.05) end
    check('next chain', chained == 2, chained)

    print('== post / echo')
    local e = vana.http.post(B .. '/echo', { name = 'x', n = 5 }):await():json()
    check('post json method', e.method == 'POST')
    check('post json body', vana.json.decode(e.body).name == 'x')
    check('content-type json', e.headers['content-type'] == 'application/json', e.headers['content-type'])
    check('user-agent', e.headers['user-agent'] == 'VanaCore' or e.headers['user-agent'] ~= nil, e.headers['user-agent'])
    e = vana.http.post(B .. '/echo', 'plain text', { headers = { ['X-Foo'] = 'bar' } }):await():json()
    check('post string + header', e.body == 'plain text' and e.headers['x-foo'] == 'bar')
    e = vana.http.request({ url = B .. '/echo', method = 'PUT', form = { a = 1, b = 'x y', c = { 1, 2 } }, query = { q = 'a&b' } }):await():json()
    check('form body', e.body == 'a=1&b=x%20y&c=1&c=2', e.body)
    check('form content-type', e.headers['content-type'] == 'application/x-www-form-urlencoded')
    check('query', e.query == 'q=a%26b', e.query)
    check('put method', e.method == 'PUT')
    e = vana.http.delete(B .. '/echo'):await():json()
    check('delete', e.method == 'DELETE')
    e = vana.http.patch(B .. '/echo', 'x'):await():json()
    check('patch', e.method == 'PATCH' and e.body == 'x')
    e = vana.http.get(B .. '/echo', { auth = { 'user', 'pass' } }):await():json()
    check('basic auth', e.headers.authorization == 'Basic dXNlcjpwYXNz', e.headers.authorization)
    e = vana.http.get('http://user:p%40ss@127.0.0.1:18080/echo'):await():json()
    check('userinfo auth', e.headers.authorization == 'Basic dXNlcjpwQHNz', e.headers.authorization)
    local h = vana.http.head(B .. '/head'):await()
    check('head', h.status == 200 and h.body == '' and h.headers['x-thing'] == 'yes', h.body)

    print('== status, redirects, encodings')
    local nf = vana.http.get(B .. '/404'):await()
    check('404 resolves not ok', not nf.ok and nf.status == 404 and nf.body:find('not found'))
    local err = failure_of(vana.http.get(B .. '/404', { strict = true }))
    check('strict rejects', err and err.code == 'http' and err.status == 404 and err.response.status == 404, err)
    local r = vana.http.get(B .. '/redirect'):await()
    check('redirect followed', r.status == 200 and r.body == 'hello world' and r.url == B .. '/hello', r.url)
    r = vana.http.get(B .. '/redirect', { follow = false }):await()
    check('redirect not followed', r.status == 302 and r.headers.location == '/hello', r.status)
    r = vana.http.get(B .. '/gzip'):await()
    check('gzip decoded', r.body == ('compressed hello '):rep(100), #r.body)
    r = vana.http.get(B .. '/cookies'):await()
    check('set-cookie list', #r.set_cookies == 2 and r.set_cookies[1] == 'a=1; Path=/', #r.set_cookies)
    check('dup header joined', r.headers['x-dup'] == 'one, two', r.headers['x-dup'])

    print('== sizes')
    r = vana.http.get(B .. '/big?n=5000000'):await()
    check('5 MB body', #r.body == 5000000, #r.body)
    err = failure_of(vana.http.get(B .. '/big?n=3000000', { max_size = 1000000 }))
    check('max_size', err and err.code == 'too_large', err)

    print('== failures')
    err = failure_of(vana.http.get(B .. '/hang', { timeout = 1 }))
    check('timeout', err and err.code == 'timeout', err)
    err = failure_of(vana.http.get('http://nonexistent-host.invalid/'))
    check('dns', err and (err.code == 'dns' or err.code == 'connect'), err and err.code .. ' ' .. err.message)
    err = failure_of(vana.http.get('http://127.0.0.1:1/', { timeout = 3 }))
    check('refused', err and err.code == 'connect', err)
    err = failure_of(vana.http.get('ftp://x/'))
    check('bad scheme', err and err.code == 'invalid_request' and tostring(err):find('aren.t supported'), err)
    err = failure_of(vana.http.get('http://a b/'))
    check('space in url', err and tostring(err):find('space'), err)
    err = failure_of(vana.http.get(B .. '/hello', { headers = { ['X-A'] = 'x\r\nInjected: 1' } }))
    check('header injection refused', err and tostring(err):find('line break'), err)
    err = failure_of(vana.http.get(B .. '/hello', { json = function () end }))
    check('bad json option', err and err.code == 'invalid_request', err)
    err = failure_of(vana.http.get(nil))
    check('nil url', err and err.code == 'invalid_request', err)

    print('== cancel')
    local p = vana.http.get(B .. '/hang', { timeout = 20 })
    coroutine.sleep(0.3)
    local before = vana.time.clock()
    check('cancel returns true', p:cancel() == true)
    err = failure_of(p)
    check('cancelled rejection', err and err.code == 'cancelled', err)
    check('cancel is quick', vana.time.clock() - before < 1)
    local chain = vana.http.get(B .. '/hang', { timeout = 20 }):next(function () end)
    coroutine.sleep(0.2)
    check('cancel through next', chain:cancel() == true)
    check('pending after cancels', vana.http.pending() == 0, vana.http.pending())

    print('== concurrency')
    local started = vana.time.clock()
    local list = {}
    for i = 1, 12 do list[i] = vana.http.get(B .. '/slow?ms=500') end
    local all = vana.promise.all(list):await()
    local took = vana.time.clock() - started
    check('12 slow requests in parallel', #all == 12 and took < 2.6, took)
    print(('     took %.2f s, workers %d'):format(took, vana.http.info().workers))

    print('== promises')
    local order = {}
    vana.promise.resolve(1):next(function (v) order[#order + 1] = v; return v + 1 end)
        :next(function (v) order[#order + 1] = v end)
        :finally(function () order[#order + 1] = 'fin' end)
    coroutine.sleep(0.1)
    check('resolve/next/finally order', table.concat(order, ',') == '1,2,fin', table.concat(order, ','))
    local v = vana.promise.new(function (resolve) resolve(vana.promise.delay(0.2, 'late')) end):await()
    check('resolve with promise follows', v == 'late')
    local race = vana.promise.race({ vana.promise.delay(0.5, 'slow'), vana.promise.delay(0.1, 'fast') }):await()
    check('race', race == 'fast')
    local settled = vana.promise.allSettled({ vana.promise.resolve(1), vana.promise.reject('no') }):await()
    check('allSettled', settled[1].status == 'fulfilled' and settled[2].status == 'rejected' and settled[2].reason == 'no')
    local any = vana.promise.any({ vana.promise.reject('a'), vana.promise.delay(0.1, 'ok') }):await()
    check('any', any == 'ok')
    err = failure_of(vana.promise.all({ vana.promise.resolve(1), vana.promise.reject('boom') }))
    check('all rejects', err == 'boom', err)
    local caught
    vana.promise.reject('x'):catch(function (e) caught = e end)
    coroutine.sleep(0.1)
    check('catch', caught == 'x')
    local thrown
    vana.promise.resolve(1):next(function () error('oops', 0) end):catch(function (e) thrown = e end)
    coroutine.sleep(0.1)
    check('error in handler is caught', thrown == 'oops', thrown)
    local aw = vana.async(function () return vana.promise.delay(0.1, 7):await() * 2 end):await()
    check('nested async/await', aw == 14, aw)
    local ok, why = pcall(function () vana.promise.resolve(1):await() end)
    -- inside a task this is fine; outside it errors (checked below at top level)
    check('await in task', ok, why)

    print('== downloads')
    local file = TMP .. '\\dl.bin'
    r = vana.http.download(B .. '/big?n=300000', file):await()
    check('download saved', r.saved == file and r.body == nil)
    local f = io.open(file, 'rb'); local size = f and f:seek('end'); if f then f:close() end
    check('download size', size == 300000, size)
    err = failure_of(vana.http.download(B .. '/404', file))
    check('download 404 rejected', err and err.code == 'http', err)
    f = io.open(file, 'rb'); size = f and f:seek('end'); if f then f:close() end
    check('404 left the file alone', size == 300000, size)
    check('no .part left', io.open(file .. '.part', 'rb') == nil)
    err = failure_of(vana.http.download(B .. '/hello', TMP .. '\\no\\such\\dir\\x.bin'))
    check('download to bad path', err and err.code == 'file', err)

    if os.getenv('NETTEST_TLS') == '1' then
        print('== https (self-signed certificate on 18443)')
        err = failure_of(vana.http.get('https://127.0.0.1:18443/hello'))
        check('self-signed refused', err and err.code == 'tls', err and (err.code .. ' ' .. err.message))
        r = vana.http.get('https://127.0.0.1:18443/hello', { insecure = true }):await()
        check('insecure https', r.body == 'hello world', r.body)
    end
    if os.getenv('NETTEST_INTERNET') == '1' then
        local okr, errr = pcall(function () return vana.http.get('https://example.com/', { timeout = 10 }):await() end)
        check('real https (example.com)', okr and errr.status == 200, okr and errr.status or tostring(errr))
    end

    print('== websocket')
    local events = {}
    local s = vana.ws.connect(W .. '/x', { headers = { ['X-Test'] = 'hi' }, auth = nil })
    s:on('message', function (d, bin) events[#events + 1] = { d, bin } end)
    check('starts connecting', s.state == 'connecting')
    s:send('queued before open')
    s:opened():await()
    check('open', s.state == 'open')
    t0 = vana.time.clock()
    while #events < 2 and vana.time.clock() - t0 < 3 do coroutine.sleep(0.02) end
    check('welcome message', events[1] and events[1][1]:find('hello from server') and events[1][1]:find('x=hi'), events[1] and events[1][1])
    check('queued send went after open', events[2] and events[2][1] == 'queued before open', events[2] and events[2][1])
    s:send('ping text')
    local echo = s:receive():await()
    check('receive() promise', echo == 'ping text', echo)
    s:send_binary('\0\1\2\255')
    t0 = vana.time.clock()
    local bin
    s:on('message', function (d, b) if b then bin = d end end)
    while not bin and vana.time.clock() - t0 < 3 do coroutine.sleep(0.02) end
    check('binary echo', bin == '\0\1\2\255', bin and #bin)
    s:send({ k = 'v' })
    local jsonecho
    jsonecho = s:receive():await()
    check('table sent as json', jsonecho == '{"k":"v"}', jsonecho)

    local count, last, ordered = 0, -1, true
    s:on('message', function (d)
        local n = d:match('^burst (%d+)$')
        if n then
            count = count + 1
            if tonumber(n) ~= last + 1 then ordered = false end
            last = tonumber(n)
        end
    end)
    s:send('burst')
    t0 = vana.time.clock()
    while count < 500 and vana.time.clock() - t0 < 5 do coroutine.sleep(0.02) end
    check('burst of 500 in order', count == 500 and ordered, count)

    local bigmsg
    s:on('message', function (d) if #d == 300000 then bigmsg = d end end)
    s:send('big')
    t0 = vana.time.clock()
    while not bigmsg and vana.time.clock() - t0 < 5 do coroutine.sleep(0.02) end
    check('300 KB message', bigmsg and bigmsg == ('B'):rep(300000))

    local closed
    s:on('close', function (code, reason, clean) closed = { code, reason, clean } end)
    s:close(4000, 'done')
    check('state closing', s.state == 'closing')
    local info = s:closed():await()
    check('client close', info.code == 4000 or info.code == 1005 or info.code == 4000, info.code)
    check('close event', closed ~= nil and closed[3] == true, closed and closed[3])
    check('state closed', s.state == 'closed')
    check('send after close fails', s:send('x') == false)

    -- the server closes
    local s2 = vana.ws.connect(W .. '/', { protocols = { 'chat', 'other' } })
    s2:opened():await()
    check('protocol chosen', s2.protocol == 'chat', s2.protocol)
    s2:send('close')
    local ci = s2:closed():await()
    check('server close code', ci.code == 4001 and ci.reason == 'bye' and ci.clean, ci.code .. ' ' .. tostring(ci.reason))

    -- dropped connection
    local s3 = vana.ws.connect(W .. '/')
    local errseen
    s3:on('error', function (e) errseen = e end)
    s3:opened():await()
    s3:send('drop')
    ci = s3:closed():await()
    check('drop is unclean 1006', ci.code == 1006 and not ci.clean, ci.code)
    check('drop raised error', errseen ~= nil, errseen)

    -- handshake refused
    local s4 = vana.ws.connect(W .. '/forbidden')
    local e4
    s4:on('error', function (e) e4 = e end)
    err = failure_of(s4:opened())
    check('handshake refused', err and err.code == 'handshake' and err.status == 403, err)
    ci = s4:closed():await()
    check('refused then closed', ci.code == 1006)

    -- unreachable
    local s5 = vana.ws.connect('ws://127.0.0.1:1/', { timeout = 3 })
    err = failure_of(s5:opened())
    check('connect refused', err and err.code == 'connect', err)
    local s6 = vana.ws.connect('http://bad url/')
    err = failure_of(s6:opened())
    check('bad ws url', err ~= nil, err)

    -- close while connecting
    local s7 = vana.ws.connect('ws://10.255.255.1:81/', { timeout = 20 })
    coroutine.sleep(0.2)
    s7:close()
    ci = s7:closed():await()
    check('close while connecting', ci.code == 1006 or ci.code == 1000, ci.code)

    -- receive rejected on close
    local s8 = vana.ws.connect(W .. '/')
    s8:opened():await()
    local pending_receive = s8:receive():next(function () end) -- consumes welcome? (inbox starts after first receive)
    s8:send('close')
    coroutine.sleep(0.5)
    err = failure_of(s8:receive())
    check('receive after close rejected', err and err.code == 'closed', err)

    print('== unload cleanup')
    local dangling = vana.http.get(B .. '/hang', { timeout = 20 })
    local sock = vana.ws.connect(W .. '/')
    sock:opened():await()
    check('engine counts', vana.http.info().requests >= 1 and vana.http.info().sockets >= 1, vana.http.info().sockets)
end):next(function ()
    print(('\n%d passed, %d failed'):format(passed, failed))
    finished = true
end, function (e)
    print('TEST CRASHED: ' .. tostring(e))
    failed = failed + 1
    finished = true
end)

-- await outside a task
local ok, err2 = pcall(function () vana.promise.resolve(1):await() end)
check('await outside a task errors', not ok and tostring(err2):find('inside a task') ~= nil, err2)

-- an error thrown in a handler nobody catches is reported with its traceback
vana.promise.resolve(1):next(function () local x = nil; return x.field end)
-- a rejected network promise nobody looks at is logged, not reported
vana.http.get('http://127.0.0.1:1/', { timeout = 2 })
