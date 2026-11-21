# Fetch from the web without stopping the game

An addon with three commands: `/fetch <url>` shows what an address answers, `/watch` checks a status page once a minute and
says when it changes, and `/live` keeps a WebSocket open and prints what it sends (reconnecting when it drops). It shows
a callback, `await` in a task, and a socket. None of it holds up a frame. The calls are in
[HTTP, WebSockets and promises](../17-http-and-websockets.md).

## `webrequests.lua`

```lua
addon.name    = 'webrequests'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'Fetches addresses, watches a status page and listens to a WebSocket.'

local STATUS_URL = 'https://example.com/status.json'   -- change these to something real
local LIVE_URL   = 'wss://example.com/live'

-- /fetch <url>: a callback. res.ok is false for a 404 (the server did answer); err is set when there was no answer.
local function fetch(url)
    vana.http.get(url, { timeout = 10 }, function (res, err)
        if err then
            return print(('%s: %s (%s)'):format(url, err.message, err.code))
        end
        print(('%s: %d %s, %d bytes, %s'):format(url, res.status, res.reason, #res.body, res.headers['content-type'] or '?'))
    end)
end

-- /watch: a task that waits. await raises the error when a request fails, so pcall keeps the loop alive.
local watching = false
local function watch()
    watching = true
    vana.async(function ()
        local last
        while watching do
            local ok, res = pcall(function () return vana.http.json(STATUS_URL, { timeout = 10 }):await() end)
            if ok then
                local state = res.state or '?'
                if state ~= last then
                    print('status: ' .. state)
                    last = state
                end
            end
            coroutine.sleep(60)
        end
    end)
end

-- /live: a WebSocket that comes back by itself
local socket
local live = false
local function connect()
    socket = vana.ws.connect(LIVE_URL, {
        on_open    = function (s) s:send({ subscribe = 'events' }) end,    -- a table goes as JSON
        on_message = function (data)
            local event = vana.json.decode(data)
            print('live: ' .. (event and event.text or data))
        end,
        on_close   = function ()
            if live then vana.tasks.once(5, connect) end                   -- try again in 5 seconds
        end,
    })
end

vana.on('command', function (e)
    local word, rest = e.command:match('^/(%a+)%s*(.*)$')
    word = word and word:lower()
    if word == 'fetch' and rest ~= '' then
        fetch(rest)
        return true
    elseif word == 'watch' then
        if watching then watching = false else watch() end
        print('watch ' .. (watching and 'on' or 'off'))
        return true
    elseif word == 'live' then
        live = not live
        if live then connect() elseif socket then socket:close() end
        print('live ' .. (live and 'on' or 'off'))
        return true
    end
end)

vana.on('unload', function () watching, live = false, false end)
```

## What it shows

- **`fetch`** gives `get` a callback last: `function (res, err)`. Either style works; the callback is the shortest.
- **`watch`** is a task (`vana.async`) that waits with `:await()` and `coroutine.sleep`. A failed request raises where
  `await` is, so it is wrapped in `pcall`, and the loop goes on.
- **`connect`** keeps one socket and starts another from `on_close`. The `send` in `on_open` is the first message, and
  messages the server sends are in `on_message` as they arrive.
- Nothing is stopped when the addon unloads: requests and sockets end with it.
