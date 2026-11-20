local B = 'http://127.0.0.1:18080'
local W = 'ws://127.0.0.1:18081'
local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then passed = passed + 1; print('  ok   ' .. name)
    else failed = failed + 1; print('  FAIL ' .. name .. (detail and (' -> ' .. tostring(detail)) or '')) end
end

vana.async(function ()
    print('== many requests')
    local t0 = vana.time.clock()
    local good = 0
    for batch = 1, 12 do
        local list = {}
        for i = 1, 50 do list[i] = vana.http.get(B .. '/hello') end
        local all = vana.promise.allSettled(list):await()
        for _, r in ipairs(all) do if r.status == 'fulfilled' and r.value.body == 'hello world' then good = good + 1 end end
    end
    check('600 requests all fine', good == 600, good)
    print(('     %.2f s, workers %d'):format(vana.time.clock() - t0, vana.http.info().workers))
    check('pool stayed at 6 or fewer', vana.http.info().workers <= 6)

    print('== caps')
    local list = {}
    for i = 1, 300 do list[i] = vana.http.get(B .. '/slow?ms=200') end
    local all = vana.promise.allSettled(list):await()
    local refused, done = 0, 0
    for _, r in ipairs(all) do
        if r.status == 'rejected' and tostring(r.reason):find('too many') then refused = refused + 1
        elseif r.status == 'fulfilled' then done = done + 1 end
    end
    check('256 accepted, rest refused', done == 256 and refused == 44, done .. '/' .. refused)

    local socks = {}
    for i = 1, 17 do socks[i] = vana.ws.connect(W .. '/') end
    local errs = 0
    for i = 1, 17 do
        local ok = pcall(function () socks[i]:opened():await() end)
        if not ok then errs = errs + 1 end
    end
    check('16 sockets open, 17th refused', errs == 1, errs)
    for i = 1, 17 do socks[i]:close() end
    for i = 1, 17 do socks[i]:closed():await() end
    coroutine.sleep(0.2)
    check('sockets all gone', vana.http.info().sockets == 0, vana.http.info().sockets)

    print('== socket churn')
    for i = 1, 40 do
        local s = vana.ws.connect(W .. '/')
        s:opened():await()
        s:send('hi')
        s:close()
        s:closed():await()
    end
    check('40 open/close cycles', vana.http.info().sockets == 0, vana.http.info().sockets)

    print('== idle socket (40 s)')
    local s = vana.ws.connect(W .. '/')
    s:opened():await()
    local welcome = s:receive():await()
    local lost
    s:on('close', function (code) lost = code end)
    coroutine.sleep(40)
    check('still open after 40 s idle', s.state == 'open' and lost == nil, lost)
    s:send('after idle')
    check('echo after idle', s:receive():await() == 'after idle')
    s:close()
end):next(function ()
    print(('\n%d passed, %d failed'):format(passed, failed))
    finished = true
end, function (e) print('CRASHED ' .. tostring(e)); finished = true end)
