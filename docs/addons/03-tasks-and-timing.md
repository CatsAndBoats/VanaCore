# 3. Tasks and timing

Lua has no threads here and nothing may block the game, so "do this in two seconds" and "do this every minute" are
tasks: functions run as coroutines from the frame loop, which can wait with `coroutine.sleep`.

## The clock

```lua
vana.time.clock()   -- seconds since an arbitrary start, to the microsecond (QueryPerformanceCounter)
vana.tasks.frame()  -- the frames this addon has seen since it loaded
```

`vana.time.clock()` is for measuring and timing. For the time of day use `os.time()` and `os.date()` as usual; for the
server's time, `vana.game.world().server_time` ([Game data](04-game-data.md)).

## Running something later

```lua
vana.tasks.once(delay, fn, ...)    -- fn(...) once, `delay` seconds from now
vana.tasks.oncef(delay, fn, ...)   -- fn(...) once, `delay` frames from now
vana.tasks.cancel(task)            -- stops a task before its next run
```

Both return a handle for `cancel`. A delay of 0 runs on the next frame.

```lua
vana.on('zone_change', function (e)
    vana.tasks.once(2, function ()
        print('Zone ' .. e.zone .. ' has loaded, more or less')
    end)
end)
```

## Running something repeatedly

```lua
vana.tasks.repeating(delay, repeats, every, fn, ...)   -- fn(...) `repeats` times: after `delay` s, then every `every` s
vana.tasks.repeatingf(delay, repeats, every, fn, ...)  -- the same, counting frames
```

For "forever", use a task of your own with a loop (below): it's clearer, and you can stop it.

## Waiting inside a task

Inside a task (one started with `once`, `oncef`, `repeating`, `repeatingf` or `spawn`), two functions pause it and let
the game go on:

```lua
coroutine.sleep(seconds)   -- carries on after `seconds`
coroutine.sleepf(frames)   -- carries on after `frames` frames
```

A plain `coroutine.yield()` carries on next frame. So a sequence that waits reads as straight code:

```lua
local function announce(text)
    vana.chat.run('/p ' .. text)
    coroutine.sleep(1)
    vana.chat.run('/p 3')
    coroutine.sleep(1)
    vana.chat.run('/p 2')
    coroutine.sleep(1)
    vana.chat.run('/p 1')
end

vana.on('command', function (e)
    if e.command:match('^/countdown') then
        vana.tasks.once(0, announce, 'Pulling in')
        return true
    end
end)
```

```lua
-- a loop that runs until the addon unloads, or `running` is cleared
local running = true
vana.tasks.once(0, function ()
    while running do
        local party = vana.game.party()
        if party then
            -- look at the party
        end
        coroutine.sleep(5)
    end
end)
vana.on('unload', function () running = false end)
```

## Running now, waiting later

```lua
vana.tasks.spawn(fn, ...)
```

Runs `fn(...)` straight away as a task. If it waits (sleeps), the rest runs later from the frame loop; if it finishes
without waiting, `spawn` returns `true` and `fn`'s results. Use it when the first part should happen now (answer a
command) and the rest later.

## Waiting for the network

A web request never blocks the game: `vana.http.get(url)` returns a promise at once, and inside a task `:await()` waits for
it the way `coroutine.sleep` waits for time. See [HTTP, WebSockets and promises](17-http-and-websockets.md).

```lua
vana.async(function ()
    local res = vana.http.get('https://example.com/status'):await()
    print(res.status)
end)
```

## Rules

- **Tasks run on the game's thread,** from the frame loop before `present`. Everything in `vana` is safe to call from
  one. A task that takes long holds up the frame, like a handler does.
- **An error in a task** is reported like a handler's, with its traceback, and ends that task. Other tasks go on.
- **Timing is per frame.** `sleep(0.5)` wakes on the first frame at or after half a second; at 60 fps that is within
  17 ms. Frame counts are exact.
- **New tasks join the queue as they go,** so a task scheduled while the queue runs starts on a later frame, never
  this one.
- **Tasks end with the addon.** Nothing runs after `unload`.

## Doing work in slices

A long job (walking the inventory, parsing a file, scanning memory) stutters the game if done at once. Do a slice a
frame:

```lua
vana.tasks.once(0, function ()
    for index = 0, 80 do
        local bag = vana.game.container(0)
        if bag and bag[index] and bag[index].id ~= 0 then
            -- look at one item
        end
        if index % 10 == 9 then
            coroutine.yield() -- next frame
        end
    end
end)
```
