# Hide the game's chat log

The most requested one: the game's chat windows (the main log and the second log, if it's on) stay closed. Chat still
arrives. The game keeps every line, and addons still see each one through their `text_in` event, so a chat addon can
show them its own way.

This is the same trick VanaCore already uses for fancychat: each frame, the chat windows are told they have no lines to
show, so they never open. Nothing flickers or bounces when a line comes in or the chat line opens.

## 1. Make the folder

```
addons\vanacore\hidechat\
    hidechat.lua
    README.md
```

## 2. Do it: `hidechat.lua`

```lua
addon.name    = 'hidechat'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = "Keeps the game's chat log closed."

vana.events.on('load', function ()
    vana.gameui.hide('chat_log')
end)
```

That's the whole addon. You don't need an `unload` handler: when hidechat unloads, VanaCore lets the chat log open
again.

## 3. Say what it changes: `README.md`

```markdown
# hidechat

Keeps the game's chat log closed. Chat still arrives: unload hidechat (or type /hidechat) to see it again.

Changes to the game's interface: hides the chat log.
```

The player reads it in /vana > Addons > hidechat, so say plainly what changes and how to get it back.

## 4. Load it

Type `/addon load hidechat`. The chat log closes at once. From then on hidechat loads at every start (`/addon load`
adds it to `scripts\boot.txt`).

## Add a toggle

To show the chat for a moment, for example to read a tell, add a command:

```lua
local hidden = true

vana.events.on('command', function (e)
    if e.command:lower() == '/hidechat' then
        hidden = not hidden
        if hidden then
            vana.gameui.hide('chat_log')
        else
            vana.gameui.show('chat_log')
        end
        print('hidechat: chat log ' .. (hidden and 'hidden' or 'shown'))
        return true -- handled: the game doesn't see /hidechat
    end
end)
```

## Hide the chat line too (only if you draw your own)

The chat line is where you type. It's a separate element, `chat_input`. Hide it **only** if your addon shows what's
being typed some other way, or the player will be typing blind. While it's hidden, typing still works and addons can
still read the text being typed.

```lua
vana.gameui.hide('chat_input')
```

## Good to know

- **fancychat** already keeps the game's chat closed by itself. You don't need this alongside it, though the two don't
  conflict.
- **Escape** still closes menus as usual, but won't open the chat log a step at a time while it's hidden.
