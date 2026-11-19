# Move the game's Target window

The Target window is the box with your target's name and HP bar. This example puts it wherever your addon says. A
typical reason is that your addon draws something where the game puts it, or hides the party list it normally sits on.

## 1. `mytarget.lua`

```lua
addon.name    = 'mytarget'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'Puts the Target window at the top of the screen.'

vana.events.on('load', function ()
    -- x: where its middle goes, y: where its top goes, both in percents of the game's interface
    local ok, why = vana.gameui.move('target_window', 50, 4)
    if not ok then
        print('mytarget: the Target window stays where it is (' .. why .. ')')
    end
end)

-- /mytarget <x> <y> moves it again; /mytarget alone puts it back where the player has it
vana.events.on('command', function (e)
    local word, x, y = e.command:match('^(/%S+)%s*(%S*)%s*(%S*)')
    if word == nil or word:lower() ~= '/mytarget' then
        return
    end
    if x == '' then
        vana.gameui.move('target_window')
    else
        vana.gameui.move('target_window', tonumber(x) or 50, tonumber(y) or 4)
    end
    return true
end)
```

Call `move` again whenever you like. The window follows at once.

## 2. Say what it changes

In your README: "Moves the game's Target window to the top of the screen (/mytarget to move it, /mytarget alone to put
it back)."

## Where `x` and `y` put it

- Both are **percents of the game's interface**, from its top left: `0, 0` is the top left corner and `100, 100` the
  bottom right.
- For the Target window, `x` is where its **middle** goes and `y` where its **top** goes. That's the same as the
  player's own Target window settings in /vana, so `50, 4` is the middle of the top edge.
- It's always kept on screen. A position that would put part of it off the edge is pulled back in.

## Good to know

- **Your position wins over the player's own** (/hud > Target Window > Position) while your addon holds
  the window. `vana.gameui.move('target_window')` with no position, or unloading your addon, hands it back to their
  setting.
- **One addon at a time.** If another addon already moved the Target window, `move` returns `false, 'held by X'`.
  Your position is remembered, and the window comes to you when that addon lets go.
- **Other windows:** only the Target window can be moved in this mockup. The macro bar and status icons already have
  movers in VanaCore and would follow the same pattern (see `../07-game-interface.md`).
