--[[
VanaCore's network layer: vana.promise, vana.http, vana.ws and vana.async, in every addon (run after lua/settings.lua, so
vana.json is there). The requests themselves run on VanaCore's own threads (net.cpp) and never touch this Lua state;
what they finish is taken from the engine once a frame, before the tasks run, and turned into promises and callbacks
here, on the game's thread. docs/addons/17-http-and-websockets.md says how to use it.
--]]

local hooks, native = ...
local vana = vana

-- Kept before the addon runs: an addon's globals may replace them.
local pcall, xpcall, type, tostring, tonumber, setmetatable, getmetatable, select, pairs, ipairs, next, error, rawget, unpack, traceback =
    pcall, xpcall, type, tostring, tonumber, setmetatable, getmetatable, select, pairs, ipairs, next, error, rawget, unpack, debug.traceback
local sort, concat, floor, format = table.sort, table.concat, math.floor, string.format
local running, yield = coroutine.running, coroutine.yield
local protect = hooks.protect

-------------------------------------------------------------------------------
-- Errors: what a rejected promise carries when the network (not the addon's code) failed. print() and error() show its
-- message; `code` is a word to test it by.
-------------------------------------------------------------------------------

local Error = {}
Error.__index = Error
Error.__tostring = function (e) return e.message end

local function make_error(code, message, extra)
    local e = setmetatable({ code = code, message = tostring(message) }, Error)
    if extra ~= nil then
        for k, v in pairs(extra) do
            e[k] = v
        end
    end
    return e
end

-- The message handler for code that runs for the addon: keeps the error value and where it came from.
local function on_error(e)
    local ok, text = pcall(tostring, e)
    return { value = e, trace = traceback(ok and text or ('a ' .. type(e) .. ' error value'), 2) }
end

-------------------------------------------------------------------------------
-- Promises
-------------------------------------------------------------------------------

local Promise = {}
Promise.__index = Promise

local micro = {}                 -- callbacks waiting to run: { fn, a, b }, in order
local micro_head, micro_tail = 1, 0
local unhandled = {}             -- rejected promises nobody has looked at (yet)
local ensure_pump                -- below

local function enqueue(fn, a, b)
    micro_tail = micro_tail + 1
    micro[micro_tail] = { fn, a, b }
    ensure_pump()
end

local function new_promise()
    return setmetatable({ state = 'pending', reactions = {} }, Promise)
end

-- Runs on_ok(value) or on_err(reason) when the promise settles (at once, in order, if it has). Counts as looking at it.
local function subscribe(p, on_ok, on_err)
    p.handled = true
    local state = p.state
    if state == 'pending' then
        local reactions = p.reactions
        reactions[#reactions + 1] = { on_ok, on_err }
    elseif state == 'fulfilled' then
        enqueue(on_ok, p.value)
    else
        enqueue(on_err, p.reason)
    end
end

local function settled(p)
    local reactions = p.reactions
    p.reactions = nil
    local fulfilled = p.state == 'fulfilled'
    for i = 1, #reactions do
        enqueue(reactions[i][fulfilled and 1 or 2], fulfilled and p.value or p.reason)
    end
end

local function fulfill(p, value)
    if p.state ~= 'pending' then
        return
    end
    p.state, p.value = 'fulfilled', value
    settled(p)
end

local function reject(p, reason, bug, trace)
    if p.state ~= 'pending' then
        return
    end
    p.state, p.reason, p.bug, p.trace = 'rejected', reason, bug, trace
    settled(p)
    if not p.handled then
        unhandled[#unhandled + 1] = p
        ensure_pump()
    end
end

-- Settles `p` with x: a promise is followed, anything else fulfils it.
local function resolve(p, x)
    if p.state ~= 'pending' or p.locked then
        return
    end
    if x == p then
        return reject(p, make_error('cycle', 'a promise was resolved with itself'))
    end
    if getmetatable(x) == Promise then
        p.locked = true
        subscribe(x, function (value)
            p.locked = false
            fulfill(p, value)
        end, function (reason)
            p.locked = false
            reject(p, reason, x.bug, x.trace)
        end)
        return
    end
    fulfill(p, x)
end

-- Calls fn(arg) for the promise `child`: what it returns settles `child`, what it raises rejects it.
local function run_handler(child, fn, arg)
    local ok, result = xpcall(fn, on_error, arg)
    if ok then
        resolve(child, result)
    else
        -- an Error is the network's (or a deliberate error()); anything else is a mistake in the addon's code
        reject(child, result.value, getmetatable(result.value) ~= Error, result.trace)
    end
end

-- Promise.new(function (resolve, reject) ... end): the function runs now; calling resolve(value) fulfils the promise
-- (resolve(another_promise) follows it), reject(reason) rejects it, and an error in the function rejects it too.
function Promise.new(executor)
    local p = new_promise()
    local ok, result = xpcall(executor, on_error, function (value) resolve(p, value) end, function (reason) reject(p, reason) end)
    if not ok then
        reject(p, result.value, getmetatable(result.value) ~= Error, result.trace)
    end
    return p
end

setmetatable(Promise, { __call = function (_, executor) return Promise.new(executor) end })

function Promise.is(x)
    return getmetatable(x) == Promise
end

function Promise.resolve(value)
    if getmetatable(value) == Promise then
        return value
    end
    local p = new_promise()
    fulfill(p, value)
    return p
end

function Promise.reject(reason)
    local p = new_promise()
    p.handled = false
    reject(p, reason)
    return p
end

-- promise:next(on_fulfilled, on_rejected) -> a new promise for what the handler returns. Either handler may be left out
-- (nil): the value or the reason then passes on. (`then` is a Lua keyword: p['then'] is the same call.)
function Promise:next(on_ok, on_err)
    local child = new_promise()
    child.parent = self
    subscribe(self, function (value)
        if type(on_ok) == 'function' then
            run_handler(child, on_ok, value)
        else
            resolve(child, value)
        end
    end, function (reason)
        if type(on_err) == 'function' then
            run_handler(child, on_err, reason)
        else
            reject(child, reason, self.bug, self.trace)
        end
    end)
    return child
end
Promise['then'] = Promise.next

function Promise:catch(on_err)
    return self:next(nil, on_err)
end

-- promise:finally(fn): fn() runs either way, and the promise's own outcome passes on (unless fn raises).
function Promise:finally(fn)
    local child = new_promise()
    child.parent = self
    local function run(pass)
        local ok, result = xpcall(fn, on_error)
        if not ok then
            return reject(child, result.value, getmetatable(result.value) ~= Error, result.trace)
        end
        pass(result)
    end
    subscribe(self, function (value)
        run(function () resolve(child, value) end)
    end, function (reason)
        run(function () reject(child, reason, self.bug, self.trace) end)
    end)
    return child
end

-- promise:cancel(): stops the request a promise (or a promise made from it with next) stands for, and rejects it with
-- code 'cancelled'. True when there was one to stop.
function Promise:cancel()
    if self.stop ~= nil then
        return self.stop()
    end
    if self.parent ~= nil then
        return self.parent:cancel()
    end
    return false
end

-- promise:await() -> its value, or raises its reason. Inside a task (vana.async, vana.tasks.once, ...) only: the task
-- waits and the game goes on.
function Promise:await()
    if running() == nil then
        error('promise:await() waits inside a task: wrap the code in vana.async(function () ... end)', 2)
    end
    self.handled = true
    while self.state == 'pending' do
        yield()
    end
    if self.state == 'rejected' then
        error(self.reason, 0)
    end
    return self.value
end

-- A promise per item of `list` (values that aren't promises count as done), for all / race / any / allSettled.
local function each(list, fn)
    local count = list.n or #list
    for i = 1, count do
        local item = list[i]
        fn(i, getmetatable(item) == Promise and item or Promise.resolve(item))
    end
    return count
end

-- Promise.all(list) -> a promise of the list of every value, or the first reason.
function Promise.all(list)
    local p = new_promise()
    local results = {}
    local remaining
    remaining = each(list, function (i, item)
        subscribe(item, function (value)
            results[i] = value
            remaining = remaining - 1
            if remaining == 0 then
                fulfill(p, results)
            end
        end, function (reason)
            reject(p, reason, item.bug, item.trace)
        end)
    end)
    if remaining == 0 then
        fulfill(p, results)
    end
    return p
end

-- Promise.allSettled(list) -> a promise of { { status = 'fulfilled', value = v } | { status = 'rejected', reason = r }, ... }
function Promise.allSettled(list)
    local p = new_promise()
    local results = {}
    local remaining
    local function done()
        remaining = remaining - 1
        if remaining == 0 then
            fulfill(p, results)
        end
    end
    remaining = each(list, function (i, item)
        subscribe(item, function (value)
            results[i] = { status = 'fulfilled', value = value }
            done()
        end, function (reason)
            results[i] = { status = 'rejected', reason = reason }
            done()
        end)
    end)
    if remaining == 0 then
        fulfill(p, results)
    end
    return p
end

-- Promise.race(list) -> the first to settle, as it settles.
function Promise.race(list)
    local p = new_promise()
    each(list, function (_, item)
        subscribe(item, function (value) fulfill(p, value) end, function (reason) reject(p, reason, item.bug, item.trace) end)
    end)
    return p
end

-- Promise.any(list) -> the first to be fulfilled; rejected (code 'all_failed', `reasons`) when every one is rejected.
function Promise.any(list)
    local p = new_promise()
    local reasons = {}
    local remaining
    remaining = each(list, function (i, item)
        subscribe(item, function (value) fulfill(p, value) end, function (reason)
            reasons[i] = reason
            remaining = remaining - 1
            if remaining == 0 then
                reject(p, make_error('all_failed', 'every promise was rejected', { reasons = reasons }))
            end
        end)
    end)
    if remaining == 0 then
        reject(p, make_error('all_failed', 'there was no promise to wait for', { reasons = reasons }))
    end
    return p
end

-- Promise.delay(seconds, value) -> a promise of `value`, fulfilled after `seconds`.
function Promise.delay(seconds, value)
    local p = new_promise()
    local task = vana.tasks.once(seconds, function () fulfill(p, value) end)
    p.stop = function ()
        if p.state ~= 'pending' then
            return false
        end
        vana.tasks.cancel(task)
        p.handled = true
        reject(p, make_error('cancelled', 'cancelled'))
        return true
    end
    return p
end

vana.promise = Promise

-- vana.async(fn, ...) -> a promise of what fn returns (or rejected with what it raises). fn runs now as a task, so inside
-- it promise:await() and coroutine.sleep() wait without stopping the game.
function vana.async(fn, ...)
    assert(type(fn) == 'function', 'vana.async: the first argument must be a function')
    local p = new_promise()
    local count = select('#', ...)
    local args = { ... }
    vana.tasks.spawn(function ()
        local ok, result = xpcall(fn, on_error, unpack(args, 1, count))
        if ok then
            resolve(p, result)
        else
            reject(p, result.value, getmetatable(result.value) ~= Error, result.trace)
        end
    end)
    return p
end

-------------------------------------------------------------------------------
-- The frame: what the engine finished, then the callbacks it makes ready
-------------------------------------------------------------------------------

local active_http = {} -- request id -> { promise, strict }
local active_ws = {}   -- socket id -> socket

local function report_unhandled()
    local list = unhandled
    unhandled = {}
    for i = 1, #list do
        local p = list[i]
        if not p.handled then
            p.handled = true
            local reason = p.reason
            local text = tostring(reason)
            if p.bug then
                hooks.report('promise', 'unhandled rejection: ' .. text, p.trace or text)
            elseif not (type(reason) == 'table' and reason.code == 'cancelled') then
                vana.log('Promise: unhandled rejection: ' .. text)
            end
        end
    end
end

-- Runs the callbacks that are ready, and those they make ready (up to 10000 a frame, so a promise that keeps making
-- itself another cannot hold the game up).
local function drain()
    local steps = 0
    while micro_head <= micro_tail and steps < 10000 do
        local job = micro[micro_head]
        micro[micro_head] = nil
        micro_head = micro_head + 1
        steps = steps + 1
        job[1](job[2], job[3])
    end
    if micro_head > micro_tail then
        micro_head, micro_tail = 1, 0
        if #unhandled > 0 then
            report_unhandled()
        end
    end
end

local dispatch -- below

local function pump()
    if next(active_http) ~= nil or next(active_ws) ~= nil then
        local events = native.poll()
        if events ~= nil then
            for i = 1, #events do
                dispatch(events[i])
            end
        end
    end
    drain()
end

local pump_registered = false
ensure_pump = function ()
    if not pump_registered then
        pump_registered = true
        hooks.pump(function ()
            if micro_head > micro_tail and #unhandled == 0 and next(active_http) == nil and next(active_ws) == nil then
                return -- nothing going on: the common frame costs this
            end
            protect('network', pump)
        end)
    end
end

-------------------------------------------------------------------------------
-- vana.http
-------------------------------------------------------------------------------

local http = {}
vana.http = http

local function urlencode(text)
    return (tostring(text):gsub('[^%w%-_%.~]', function (c) return format('%%%02X', c:byte()) end))
end

local function sorted_keys(t)
    local keys = {}
    for k in pairs(t) do
        keys[#keys + 1] = k
    end
    sort(keys, function (a, b) return tostring(a) < tostring(b) end)
    return keys
end

local function form_value(value)
    if type(value) == 'number' then
        return format('%.14g', value)
    end
    return tostring(value)
end

-- http.urlencode(text) -> text with everything but letters, digits and -_.~ as %XX
http.urlencode = urlencode

-- http.encode({ a = 1, b = 'x y', c = { 1, 2 } }) -> 'a=1&b=x%20y&c=1&c=2' (keys in order; a list repeats its key)
function http.encode(fields)
    local parts = {}
    for _, key in ipairs(sorted_keys(fields)) do
        local value = fields[key]
        if type(value) == 'table' then
            for _, each_value in ipairs(value) do
                parts[#parts + 1] = urlencode(key) .. '=' .. urlencode(form_value(each_value))
            end
        elseif value ~= nil then
            parts[#parts + 1] = urlencode(key) .. '=' .. urlencode(form_value(value))
        end
    end
    return concat(parts, '&')
end

local b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

local function base64(data)
    local out = {}
    for i = 1, #data, 3 do
        local a, b, c = data:byte(i, i + 2)
        local n = a * 65536 + (b or 0) * 256 + (c or 0)
        local c1, c2, c3, c4 = floor(n / 262144) % 64, floor(n / 4096) % 64, floor(n / 64) % 64, n % 64
        out[#out + 1] = b64:sub(c1 + 1, c1 + 1) .. b64:sub(c2 + 1, c2 + 1) ..
            (b and b64:sub(c3 + 1, c3 + 1) or '=') .. (c and b64:sub(c4 + 1, c4 + 1) or '=')
    end
    return concat(out)
end

-- http.basic_auth(user, password) -> 'Basic dXNlcjpwYXNz', for an Authorization header
function http.basic_auth(user, password)
    return 'Basic ' .. base64(tostring(user) .. ':' .. tostring(password or ''))
end

local headers_meta = {
    __index = function (t, name)
        if type(name) == 'string' then
            return rawget(t, name:lower())
        end
    end,
}

local Response = {}
Response.__index = Response

-- res:json() -> the value the body holds; raises an error (code 'json') when it isn't JSON
function Response:json()
    local value, why = vana.json.decode(self.body or '')
    if why ~= nil then
        error(make_error('json', 'the answer is not valid JSON: ' .. tostring(why), { response = self }), 0)
    end
    return value
end

function Response:text()
    return self.body
end

-- res:header(name) -> a header's value (any case), or nil
function Response:header(name)
    return self.headers[name]
end

local function make_response(e)
    local status = e.status
    local cookies = e.headers.set_cookies
    e.headers.set_cookies = nil
    return setmetatable({
        ok = status >= 200 and status < 300,
        status = status,
        reason = e.reason,
        url = e.url,
        headers = setmetatable(e.headers, headers_meta),
        set_cookies = cookies or {},
        body = e.body,
        saved = e.saved,
    }, Response)
end

-- Turns the options of a call into what the engine takes, or nil and why.
local function build(opts)
    local url = opts.url
    if type(url) ~= 'string' or url == '' then
        return nil, 'the URL must be a string'
    end
    if opts.query ~= nil then
        local query = type(opts.query) == 'table' and http.encode(opts.query) or tostring(opts.query)
        if query ~= '' then
            local separator = '?'
            if url:find('?', 1, true) then
                separator = (url:sub(-1) == '?' or url:sub(-1) == '&') and '' or '&'
            end
            url = url .. separator .. query
        end
    end

    local headers, names = {}, {}
    local function set_header(name, value, keep)
        local key = name:lower()
        if names[key] ~= nil then
            if keep then
                return
            end
            headers[names[key]] = nil
        end
        names[key] = name
        headers[name] = value
    end
    if type(opts.headers) == 'table' then
        for name, value in pairs(opts.headers) do
            if type(name) == 'string' and value ~= nil then
                set_header(name, tostring(value))
            end
        end
    end

    local body = opts.body
    if opts.json ~= nil then
        local text, why = vana.json.encode(opts.json, true)
        if text == nil then
            return nil, 'the json option can\'t be written as JSON: ' .. tostring(why)
        end
        body = text
        set_header('Content-Type', 'application/json', true)
        set_header('Accept', 'application/json', true)
    elseif opts.form ~= nil then
        body = type(opts.form) == 'table' and http.encode(opts.form) or tostring(opts.form)
        set_header('Content-Type', 'application/x-www-form-urlencoded', true)
    end
    if body ~= nil and type(body) ~= 'string' then
        return nil, 'the body must be a string (use the json option for a table)'
    end
    if opts.auth ~= nil then
        local auth = opts.auth
        if type(auth) == 'table' then
            set_header('Authorization', http.basic_auth(auth[1] or auth.user, auth[2] or auth.password), true)
        elseif type(auth) == 'string' then
            set_header('Authorization', auth:find(':', 1, true) and not auth:find('^%a+ ') and ('Basic ' .. base64(auth)) or auth, true)
        end
    end

    return {
        url = url,
        method = opts.method or 'GET',
        headers = headers,
        body = body,
        timeout = opts.timeout,
        max_time = opts.max_time,
        follow = opts.follow,
        max_redirects = opts.max_redirects,
        max_size = opts.max_size,
        save = opts.save,
        insecure = opts.insecure,
    }
end

-- The code of an error the engine gave on the spot (nothing was started): too much going on, no WinHTTP, or a request
-- that can't be made (a bad URL, a header with a line break).
local function refusal(message)
    if message:find('^too many') then
        return 'busy'
    elseif message:find('WinHTTP', 1, true) or message:find('Windows 8', 1, true) then
        return 'unavailable'
    end
    return 'invalid_request'
end

-- Settles the promise of a finished request.
local function finish_request(record, e)
    local p = record.promise
    if e.kind == 'failed' then
        return reject(p, make_error(e.code, e.message))
    end
    local response = make_response(e)
    if record.strict and not response.ok then
        return reject(p, make_error('http', 'HTTP ' .. response.status .. ' ' .. tostring(response.reason), { status = response.status, response = response }))
    end
    fulfill(p, response)
end

local function start(opts)
    local p = new_promise()
    local spec, why = build(opts)
    if spec == nil then
        reject(p, make_error('invalid_request', why))
        return p
    end
    local id, err = native.http(spec)
    if id == nil then
        reject(p, make_error(refusal(err), err))
        return p
    end
    active_http[id] = { promise = p, strict = opts.strict }
    p.stop = function ()
        if p.state ~= 'pending' then
            return false
        end
        active_http[id] = nil
        native.cancel(id)
        p.handled = true
        reject(p, make_error('cancelled', 'cancelled'))
        return true
    end
    ensure_pump()
    return p
end

-- A callback in place of (or beside) the promise: cb(response) when it works, cb(nil, error) when it doesn't.
local function with_callback(p, callback)
    if type(callback) == 'function' then
        p:next(function (response) callback(response) end, function (err) callback(nil, err) end)
    end
    return p
end

-- (opts, callback) as calls give them: either may be left out
local function options(a, b)
    if type(a) == 'function' then
        return {}, a
    end
    return a or {}, b
end

local function with(opts, method, url)
    local copy = {}
    for k, v in pairs(opts) do
        copy[k] = v
    end
    copy.method = method or copy.method
    copy.url = url or copy.url
    return copy
end

-- http.request(options_or_url, options_or_callback, callback) -> a promise of the response.
-- options: url, method, headers = { name = value }, query = table or text, body = text, json = value (sent as JSON),
-- form = table (sent urlencoded), auth = 'user:password' or { user, password } or a ready header value,
-- timeout = seconds a step may take (30), max_time = seconds in all (600), follow = true, max_redirects = 10,
-- max_size = bytes of answer (16 MB), save = a file to write the body to, insecure = false, strict = false (reject
-- an answer that isn't 2xx).
-- The response: ok, status, reason, url (after redirects), headers (lower case names; any case works), set_cookies,
-- body, saved, and res:json(), res:text(), res:header(name). An answer of any status is a response; the promise is
-- rejected (code 'timeout', 'dns', 'connect', 'tls', 'cancelled', 'too_large', ...) only when there is none.
function http.request(target, a, b)
    if type(target) == 'string' then
        local opts, callback = options(a, b)
        return with_callback(start(with(opts, nil, target)), callback)
    end
    assert(type(target) == 'table', 'vana.http.request: the first argument must be a URL or a table of options')
    local callback = a
    return with_callback(start(target), callback)
end

function http.get(url, a, b)
    local opts, callback = options(a, b)
    return with_callback(start(with(opts, 'GET', url)), callback)
end

function http.head(url, a, b)
    local opts, callback = options(a, b)
    return with_callback(start(with(opts, 'HEAD', url)), callback)
end

function http.delete(url, a, b)
    local opts, callback = options(a, b)
    return with_callback(start(with(opts, 'DELETE', url)), callback)
end

-- post, put, patch (url, body, options, callback): a table body is sent as JSON, a string as it is
local function sender(method)
    return function (url, body, a, b)
        local opts, callback = options(a, b)
        opts = with(opts, method, url)
        if type(body) == 'table' then
            opts.json = body
        elseif body ~= nil then
            opts.body = body
        end
        return with_callback(start(opts), callback)
    end
end
http.post = sender('POST')
http.put = sender('PUT')
http.patch = sender('PATCH')

-- http.json(url, options, callback) -> a promise of the JSON an address answers with (rejected if it answers anything but
-- 2xx, or isn't JSON)
function http.json(url, a, b)
    local opts, callback = options(a, b)
    opts = with(opts, nil, url)
    opts.strict = true
    local p = start(opts):next(function (response)
        return response:json()
    end)
    return with_callback(p, callback)
end

-- http.download(url, path, options, callback) -> a promise of the response, its body written to `path` (response.saved)
-- as it arrives: nothing of it is held in memory, so there is no size limit
function http.download(url, path, a, b)
    local opts, callback = options(a, b)
    opts = with(opts, nil, url)
    opts.save = path
    opts.strict = true
    return with_callback(start(opts), callback)
end

-- http.pending() -> how many requests are going now
function http.pending()
    local count = 0
    for _ in pairs(active_http) do
        count = count + 1
    end
    return count
end

-- http.info() -> { requests, sockets, waiting, workers }: what the engine has for this addon, and the threads it runs on
function http.info()
    return native.info()
end

-------------------------------------------------------------------------------
-- vana.ws
-------------------------------------------------------------------------------

local ws = {}
vana.ws = ws

local Socket = {}
Socket.__index = Socket

function Socket:_emit(name, ...)
    local list = self.handlers[name]
    if list == nil then
        return false
    end
    local called = false
    for _, fn in ipairs({ unpack(list) }) do
        called = true
        protect('ws ' .. name, fn, ...)
    end
    return called
end

-- socket:on(name, fn) -> the socket. Names and what the function gets:
--   'open'(socket)  'message'(data, is_binary)  'close'(code, reason, clean)  'error'(err)  (err.code, err.message)
function Socket:on(name, fn)
    assert(type(name) == 'string', 'socket:on: the event name must be a string')
    assert(type(fn) == 'function', 'socket:on: the handler must be a function')
    local list = self.handlers[name]
    if list == nil then
        list = {}
        self.handlers[name] = list
    end
    list[#list + 1] = fn
    return self
end

function Socket:off(name, fn)
    local list = self.handlers[name]
    if list ~= nil then
        for i = #list, 1, -1 do
            if list[i] == fn then
                table.remove(list, i)
            end
        end
    end
    return self
end

local function transmit(self, data, binary)
    if self.state == 'connecting' then
        -- sent, in order, as soon as it opens
        local pending = self._pending
        if pending == nil then
            pending = {}
            self._pending = pending
        end
        if #pending >= 1000 then
            return false, 'too many messages waiting for the socket to open'
        end
        pending[#pending + 1] = { tostring(data), binary }
        return true
    end
    if self.state ~= 'open' then
        return false, 'the socket is ' .. self.state
    end
    local ok, why = native.send(self.id, tostring(data), binary)
    return ok == true, why
end

-- socket:send(text) -> true, or false and why. Text goes as a text message (UTF-8; a table as JSON), send_binary as a
-- binary one. Sent while the socket is still connecting, it waits and goes when the socket opens.
function Socket:send(data)
    if type(data) == 'table' then
        return self:send_json(data)
    end
    return transmit(self, data, false)
end

function Socket:send_binary(data)
    return transmit(self, data, true)
end

function Socket:send_json(value)
    local text, why = vana.json.encode(value, true)
    if text == nil then
        return false, 'can\'t write that as JSON: ' .. tostring(why)
    end
    return self:send(text)
end

-- socket:close(code, reason): starts the closing handshake (1000, "normal", unless given). The 'close' event follows.
function Socket:close(code, reason)
    if self.state == 'connecting' or self.state == 'open' then
        self.state = 'closing'
        native.close(self.id, code or 1000, reason or '')
    end
end

function Socket:is_open()
    return self.state == 'open'
end

-- socket:opened() -> a promise of the socket once it is open (rejected if it never is)
function Socket:opened()
    if self._opened == nil then
        local p = new_promise()
        self._opened = p
        if self.state == 'open' or self.state == 'closing' and self.was_open then
            fulfill(p, self)
        elseif self.state == 'closed' then
            reject(p, self.last_error or make_error('closed', 'the socket closed'))
        end
    end
    return self._opened
end

-- socket:closed() -> a promise of { code, reason, clean } once it is closed
function Socket:closed()
    if self._closed == nil then
        local p = new_promise()
        self._closed = p
        if self.state == 'closed' then
            fulfill(p, { code = self.close_code, reason = self.close_reason, clean = self.close_clean })
        end
    end
    return self._closed
end

-- socket:receive() -> a promise of the next message's data; rejected if the socket closes first. A socket with no
-- 'message' handler keeps what it receives (up to 1000 messages) for receive() to hand out in order, from the moment it
-- connects, so a message the server sends on open is not missed.
function Socket:receive()
    local p = new_promise()
    if #self._inbox > 0 then
        fulfill(p, table.remove(self._inbox, 1))
    elseif self.state == 'closed' then
        reject(p, make_error('closed', 'the socket is closed'))
    else
        self._waiting[#self._waiting + 1] = p
    end
    return p
end

function Socket:_event(e)
    local kind = e.kind
    if kind == 'open' then
        self.state = 'open'
        self.was_open = true
        self.protocol = e.protocol
        local pending = self._pending
        self._pending = nil
        if pending ~= nil then
            for i = 1, #pending do
                native.send(self.id, pending[i][1], pending[i][2])
            end
        end
        if self._opened ~= nil then
            fulfill(self._opened, self)
        end
        self:_emit('open', self)
    elseif kind == 'message' then
        local waiting = self._waiting
        if #waiting > 0 then
            fulfill(table.remove(waiting, 1), e.data) -- asked for with receive()
        elseif not self.handlers.message or #self.handlers.message == 0 then
            local inbox = self._inbox
            inbox[#inbox + 1] = e.data
            if #inbox > 1000 then
                table.remove(inbox, 1)
            end
        end
        self:_emit('message', e.data, e.binary)
    elseif kind == 'error' then
        local err = make_error(e.code, e.message, { status = e.status })
        self.last_error = err
        if self._opened ~= nil and self._opened.state == 'pending' then
            reject(self._opened, err)
        end
        if not self:_emit('error', err) then
            vana.log(format('WebSocket %s: %s', self.url, err.message))
        end
    elseif kind == 'close' then
        self.state = 'closed'
        self.close_code, self.close_reason, self.close_clean = e.code, e.reason, e.clean
        if self.id ~= nil then
            active_ws[self.id] = nil
        end
        self._pending = nil
        if self._opened ~= nil and self._opened.state == 'pending' then
            reject(self._opened, self.last_error or make_error('closed', 'the socket closed before it opened'))
        end
        for _, p in ipairs(self._waiting) do
            reject(p, make_error('closed', 'the socket closed'))
        end
        self._waiting = {}
        if self._closed ~= nil then
            fulfill(self._closed, { code = e.code, reason = e.reason, clean = e.clean })
        end
        self:_emit('close', e.code, e.reason, e.clean)
    end
end

-- ws.connect(url, options) -> a socket, at once (it is 'connecting'; 'open' fires when it is ready).
-- options: headers = { name = value }, protocols = { 'name', ... }, timeout = seconds to connect (15), keepalive =
-- seconds between pings (30), max_size = bytes of a message (16 MB), insecure = false, and on_open, on_message,
-- on_close, on_error (the same as socket:on).
-- socket: state ('connecting', 'open', 'closing', 'closed'), url, protocol (the one the server chose), id.
function ws.connect(url, opts)
    opts = opts or {}
    local socket = setmetatable({ url = url, state = 'connecting', protocol = '', handlers = {}, _inbox = {}, _waiting = {} }, Socket)
    for _, name in ipairs({ 'open', 'message', 'close', 'error' }) do
        if type(opts['on_' .. name]) == 'function' then
            socket:on(name, opts['on_' .. name])
        end
    end
    local headers = {}
    if type(opts.headers) == 'table' then
        for name, value in pairs(opts.headers) do
            headers[name] = tostring(value)
        end
    end
    local id, err = native.websocket({
        url = url,
        headers = headers,
        protocols = opts.protocols,
        timeout = opts.timeout,
        keepalive = opts.keepalive,
        max_size = opts.max_size,
        insecure = opts.insecure,
    })
    if id == nil then
        -- the same events as any other failure, a frame later: the caller has attached its handlers by then
        socket.state = 'closed'
        enqueue(function ()
            socket:_event({ kind = 'error', code = refusal(tostring(err)), message = tostring(err) })
            socket:_event({ kind = 'close', code = 1006, reason = '', clean = false })
        end)
        return socket
    end
    socket.id = id
    active_ws[id] = socket
    ensure_pump()
    return socket
end

-- ws.count() -> how many sockets are connecting or open
function ws.count()
    local count = 0
    for _ in pairs(active_ws) do
        count = count + 1
    end
    return count
end

dispatch = function (e)
    local kind = e.kind
    if kind == 'response' or kind == 'failed' then
        local record = active_http[e.id]
        if record ~= nil then
            active_http[e.id] = nil
            finish_request(record, e)
        end
    else
        local socket = active_ws[e.id]
        if socket ~= nil then
            socket:_event(e)
        end
    end
end
