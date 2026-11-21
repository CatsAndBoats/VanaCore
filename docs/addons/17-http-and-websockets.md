# 17. HTTP, WebSockets and promises

An addon can fetch web pages, call web APIs, download files and keep a WebSocket open, and **none of it stops the game**.
A call returns at once with a promise (or runs a callback you give it); the request itself runs on VanaCore's own worker
threads, many at a time, and the answer is handed to your code on the game's thread, in the next frame that has it. You
never wait, never lock, and never share a variable with another thread: your Lua is still one thread, and everything
`vana` offers is still safe to call from your callback.

```lua
-- 1. a callback
vana.http.get('https://example.com/api/price', function (res, err)
    if err then print('failed: ' .. err.message) return end
    print(res.status, #res.body)
end)

-- 2. a promise
vana.http.get(url):next(function (res) print(res:json().name) end):catch(function (err) print(err) end)

-- 3. straight-line code in a task: the task waits, the game goes on
vana.async(function ()
    local res = vana.http.get(url):await()
    print(res:json().name)
end)
```

Pick whichever reads best; they are the same request. This page covers `vana.http`, `vana.ws`, `vana.promise` and
`vana.async`.

## Requests: `vana.http`

```lua
vana.http.get(url[, options][, callback])        → promise
vana.http.head(url[, options][, callback])
vana.http.delete(url[, options][, callback])
vana.http.post(url, body[, options][, callback]) -- also put and patch
vana.http.request(options_or_url[, options][, callback])
vana.http.json(url[, options][, callback])       → promise of the decoded JSON
vana.http.download(url, path[, options][, callback])
```

A `body` that is a table is sent as JSON (`Content-Type: application/json`); a string is sent as it is. `request` takes
everything in one table (`{ url = ..., method = 'PUT', ... }`).

### Options

| Option | Meaning |
|---|---|
| `method` | `'GET'` unless the call says otherwise |
| `headers` | `{ ['X-Key'] = 'value' }` |
| `query` | a table (`{ q = 'a b', page = 2 }`) or text added to the URL as `?q=a%20b&page=2` |
| `body` | text to send |
| `json` | any value: sent as JSON, and `Accept: application/json` is asked for |
| `form` | a table (or text) sent as `application/x-www-form-urlencoded`; a list value repeats its key |
| `auth` | `'user:password'`, `{ 'user', 'password' }`, or a ready header value (`'Bearer abc'`): the `Authorization` header |
| `timeout` | seconds one step may take: connecting, sending, waiting for the answer, or a stall while it arrives (30) |
| `max_time` | seconds for the whole request (600) |
| `follow` | follow redirects (`true`); `max_redirects` (10) |
| `max_size` | the most bytes of answer that are read (16 MB; none with `save`); more fails with `too_large` |
| `save` | write the body to this file instead of keeping it (below) |
| `insecure` | accept any certificate (`false`): for a server on your own network with a self-signed one |
| `strict` | reject an answer that isn't 2xx (`false`; always on for `vana.http.json` and `vana.http.download`) |

`user:password@` in the URL works as `auth`. Cookies are not kept between requests (a cookie jar would be shared with
every other addon): send a `Cookie` header, and read `res.set_cookies`.

### The response

An answer is a response **whatever its status**: a 404 is still a response, with `ok = false`. The promise is rejected
only when there is no answer (see Errors), or with `strict`.

```lua
res.ok          -- true for 200 to 299
res.status      -- 200
res.reason      -- 'OK'
res.url         -- where it ended up, after redirects
res.headers     -- { ['content-type'] = 'text/plain', ... }: lower case names (any case works to look one up)
res.set_cookies -- a list of the Set-Cookie lines
res.body        -- the body as text (any bytes); nil when it was saved to a file
res.saved       -- the path given in `save`
res:json()      -- the body decoded (vana.json); raises an error with code 'json' if it isn't JSON
res:header(name) -- one header, any case
```

A header sent twice (`X-Dup`) is one value joined with `, `. gzip and deflate are undone for you.

```lua
vana.http.post('https://example.com/api/items', { name = 'Ash Log', qty = 3 }, {
    headers = { ['X-Api-Key'] = 'secret' },
}, function (res, err)
    if err then return print('could not send: ' .. err.message) end
    if not res.ok then return print('the server said ' .. res.status) end
    print('saved as ' .. res:json().id)
end)
```

### Errors

A rejected promise (or the second argument to a callback) holds an error. `print(err)` shows its message, and
`err.code` is a word to test it by:

| `err.code` | When |
|---|---|
| `timeout` | a step took longer than `timeout`, or the whole request longer than `max_time` |
| `dns` | the name could not be found |
| `connect` | the server could not be reached, or the connection dropped |
| `tls` | the certificate or the secure connection failed |
| `cancelled` | you called `promise:cancel()` |
| `too_large` | the answer is more than `max_size` |
| `redirect` | too many redirects, or a redirect that can't be followed |
| `file` | `save` could not be written |
| `http` | `strict` and the answer wasn't 2xx (`err.status`, `err.response`) |
| `json` | `res:json()` or `vana.http.json` and the body isn't JSON |
| `invalid_request` | nothing was sent: a bad URL, a header with a line break, an option that can't be sent |
| `busy` | too many requests at once (below) |
| `unavailable` | WinHTTP isn't there (a very old Windows) |
| `invalid_url` | WinHTTP refused the address or its scheme (a redirect's, say) |
| `failed` | any other network error |

### Downloads

```lua
vana.http.download('https://example.com/pack.zip', vana.paths.addon .. '\\pack.zip'):next(function (res)
    print('saved ' .. res.saved)
end)
```

The body is written to `pack.zip.part` as it arrives and moved over `pack.zip` only when it is all there, so a failed
or cancelled download never leaves half a file, and nothing of it sits in memory. A server answer that isn't 2xx
doesn't touch the file: `download` rejects with code `http` (`err.status`; `err.response.body` is the answer's first
64 KB). With the `save` option on another call (and `strict` off), that response is the value instead.

### Stopping a request

```lua
local request = vana.http.get(url)
request:cancel()    -- true; the promise is rejected with code 'cancelled'
```

`cancel` works on the promise a call returned and on one made from it with `:next(...)`. When your addon unloads or
reloads, everything it had going is stopped.

## Promises: `vana.promise`

A promise stands for a value that arrives later. Everything below works on any promise, yours or a request's.

```lua
local p = vana.promise.new(function (resolve, reject)
    vana.tasks.once(2, resolve, 'done')      -- resolve(value) or reject(reason), once
end)

p:next(on_fulfilled, on_rejected)   -- → a new promise for what the function returns (`then` is a Lua keyword: p['then'] is the same)
p:catch(on_rejected)                -- p:next(nil, on_rejected)
p:finally(fn)                       -- fn() either way; the outcome passes on
p:await()                           -- inside a task: the value, or raises the reason
p:cancel()                          -- stops the request behind it, if there is one
p.state                             -- 'pending', 'fulfilled' or 'rejected'; p.value or p.reason when it's settled
```

```lua
vana.promise.resolve(value)         -- already fulfilled
vana.promise.reject(reason)         -- already rejected
vana.promise.all(list)              -- → a promise of the list of every value; rejected with the first reason
vana.promise.allSettled(list)       -- → { { status = 'fulfilled', value = v } | { status = 'rejected', reason = r }, ... }
vana.promise.race(list)             -- the first to settle
vana.promise.any(list)              -- the first to be fulfilled (code 'all_failed' if none)
vana.promise.delay(seconds, value)  -- fulfilled after `seconds`
```

Things to know:

- A handler runs later, never inside the call that attached it, and in the order promises settled.
- What a handler returns settles the promise `next` gave back; a promise returned from a handler is followed. An error
  raised in a handler rejects that promise, so `:catch` after a chain catches anything above it.
- **An error raised in your handler** and not caught is reported like any addon error (with its traceback, in
  `logs\vanacore.log` and the Addons page). A **network error** nobody looked at is logged as
  `Promise: unhandled rejection: ...` and nothing more, so a request you fire and forget doesn't count against your addon.
  Attach a `:catch` when you care.
- Work on a list in parallel with `all`:

```lua
local pages = {}
for i = 1, 5 do pages[i] = vana.http.get(base .. '/page/' .. i) end
vana.promise.all(pages):next(function (all)
    for i, res in ipairs(all) do print(i, res.status) end
end)
```

## Waiting in a task: `vana.async` and `await`

```lua
vana.async(fn, ...)    -- runs fn(...) now as a task → a promise of what it returns (or rejected with what it raises)
promise:await()        -- inside a task: waits for the promise without stopping the game
```

`await` must run inside a task: `vana.async`, `vana.tasks.once` and the others ([Tasks and timing](03-tasks-and-timing.md))
all make one. Elsewhere (in an event handler, in a `:next` function) it raises `promise:await() waits inside a task`; wrap
the code in `vana.async` there. A task that awaits carries on in the same frame the answer arrives, before the other tasks.
Errors work as in any Lua: `pcall` around an `await` catches the rejection.

```lua
vana.on('command', function (e)
    local name = e.command:match('^/lookup%s+(%S+)')
    if not name then return end
    vana.async(function ()
        local ok, res = pcall(function () return vana.http.json('https://example.com/api/item/' .. name):await() end)
        print(ok and (name .. ': ' .. res.price) or ('lookup failed: ' .. tostring(res)))
        coroutine.sleep(1)       -- other waits work in the same task
    end)
    return true
end)
```

## WebSockets: `vana.ws`

```lua
local socket = vana.ws.connect('wss://example.com/live', {
    headers = { ['X-Key'] = 'secret' },
    protocols = { 'chat' },           -- Sec-WebSocket-Protocol; socket.protocol is the one the server chose
    timeout = 15,                      -- seconds to connect
    keepalive = 30,                    -- seconds between pings
    max_size = 16 * 1024 * 1024,       -- the largest message taken in
    on_message = function (data, is_binary) print(data) end,
})
```

`connect` returns **at once**: the socket is `'connecting'`, and its `'open'` event comes when the handshake is done.
Messages sent before then wait and go in order as soon as it opens.

```lua
socket:on('open', function (s) s:send('hello') end)
socket:on('message', function (data, is_binary) ... end)
socket:on('close', function (code, reason, clean) ... end)
socket:on('error', function (err) print(err.code, err.message) end)   -- a 'close' follows an error
socket:off(name, fn)

socket:send(text)          -- true, or false and why; a table is sent as JSON
socket:send_binary(bytes)
socket:send_json(value)
socket:close([code[, reason]])   -- the closing handshake; 'close' follows (a code is 1000 or 3000 to 4999)

socket.state     -- 'connecting', 'open', 'closing' or 'closed'
socket.protocol, socket.url, socket:is_open()
```

Or with promises, in a task:

```lua
vana.async(function ()
    local socket = vana.ws.connect('wss://example.com/live')
    socket:opened():await()             -- raises the error if it never opens
    socket:send({ subscribe = 'prices' })
    for _ = 1, 10 do
        local text = socket:receive():await()   -- the next message
        print(text)
    end
    socket:close()
    print(socket:closed():await().code)         -- { code, reason, clean }
end)
```

`receive()` hands out messages in order. A socket with no `'message'` handler keeps what arrives (the last 1000) from the
moment it connects, so a message the server sends as soon as it opens is there when you ask. Once you have a `'message'`
handler, messages go to it, and to a `receive()` that is waiting. A pending `receive()` is rejected (code `closed`) if the
socket closes.

An error has a `code`: `dns`, `connect`, `tls`, `timeout`, `handshake` (the server answered something other than 101,
`err.status`), `too_large`, `invalid_request`, `busy`, `unavailable`, `failed` (any other error). `opened()` is rejected
with the socket's error, or code `closed` if it closed without one. A connection the server drops without a closing
handshake is code `1006` with `clean = false`. There is no automatic reconnect; start another from `'close'`:

```lua
local function connect()
    local socket = vana.ws.connect(url)
    socket:on('close', function () vana.tasks.once(5, connect) end)
    return socket
end
connect()
```

## Limits and rules

- **Threads.** Up to 6 requests run at once; more wait their turn. An addon may have 256 requests going (more are
  rejected with `busy`) and 16 WebSockets (64 in all). A socket uses two threads of its own while it lives.
- **Frame cost.** Your callbacks run from the frame, so a callback that takes long holds the frame up as any handler does.
  At most 256 events (8 MB) a frame are taken from the threads; a socket that is sent faster than your addon reads
  waits, and the server's sending backs up, instead of memory filling.
- **Sizes.** A body, an answer or a message may be 16 MB unless `max_size` says otherwise (a request body 256 MB; a
  download to a file has no limit).
- **Security.** Certificates are checked, with the system's proxy and TLS 1.2 (and 1.3 where Windows has it). No
  cookies, no Windows sign-in. An address on this PC or your network (`127.0.0.1`, `192.168...`) is reachable like any
  other: only load addons you trust.
- **Windows 7.** Requests work, with HTTPS needing Windows 7's TLS 1.2 update; WebSockets need Windows 8 or later and say
  `unavailable` otherwise.
- **Other addons.** Each addon has its own queue, counts and sockets; one addon's requests never reach another's code.
- **Looking at it.** `vana.http.pending()` is how many requests are going; `vana.ws.count()` how many sockets;
  `vana.http.info()` is `{ requests, sockets, waiting, workers }` (what the engine holds for the addon, and the threads
  running now).

Also: `vana.http.encode(table)` builds `a=1&b=x%20y`, `vana.http.urlencode(text)` encodes one value, and
`vana.http.basic_auth(user, password)` gives `'Basic dXNlcjpwYXNz'`. `vana.json.encode(value, true)` writes compact JSON
on one line (the default is indented, for files); `vana.json.decode(text)` reads it ([Settings and files](08-settings-and-files.md)).
