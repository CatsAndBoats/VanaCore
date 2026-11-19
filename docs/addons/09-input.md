# 9. Input

Key binds, key and mouse events, the game's DirectInput keyboard, and controllers.

## Key binds: `vana.binds`

A bind runs a command when a key is pressed, and the game doesn't see the key. The player makes them with `/bind` (or
the keybinds window, `/keybinds`); an addon can too:

```lua
vana.binds.add(spec, command[, up])   -- /bind without a word in the chat log; true, or nil and why
vana.binds.remove(spec[, up])         -- /unbind; true if one was bound
vana.binds.remove(bind)               -- with an entry from list()
vana.binds.list()                     -- every bind, in the order made
vana.binds.base()                     -- the binds boot.txt and the addons made, as list() gives them; nil until they have
vana.binds.keys()                     -- the key names /bind takes
```

`spec` is the key as `/bind` takes it: modifiers, then the key's name.

| Prefix | Modifier |
|--------|----------|
| `^` | Ctrl |
| `!` | Alt |
| `@` | Windows key |
| `#` | Apps (menu) key |
| `+` or `~` | Shift |
| `%` | Only while the chat line is closed |
| `$` | Only while the chat line is open |

Key names: `a` to `z`, `0` to `9`, `f1` to `f24`, `numpad0` to `numpad9`, `up`, `down`, `left`, `right`, `insert`,
`delete`, `home`, `end`, `pageup`, `pagedown`, `return`, `space`, `escape`, `tab`, `backspace`, and the rest
`vana.binds.keys()` lists. `up` true binds the key's release.

The mouse's buttons bind the same way: `lbutton`, `rbutton`, `mbutton`, `xbutton1` and `xbutton2` (the side buttons:
`mouseback` and `mouseforward` too), or `mouse1` to `mouse5`. So does its wheel: `wheelup`, `wheeldown`, and `wheelleft`
and `wheelright` for a wheel that tilts. A bound button's press and release don't reach the game, nor a bound wheel's
turn. A wheel bind runs once a notch (a high-resolution wheel's smaller turns add up), as a press and a release, so a
bind made with `up` runs too; a tilt held runs it again each time the mouse repeats it. A click or wheel turn on
VanaCore's windows (or one a plugin or addon takes) is theirs, not a bind's. `^mbutton` is Ctrl + the middle button; a
plain `lbutton` or `rbutton` bind takes every click of that button over the game, and a plain `wheelup` or `wheeldown`
the camera's zoom.

Windows knows five mouse buttons. A gaming mouse's further buttons (a side panel's, the G keys) reach the game as the
keys the mouse's own software gives them (Logitech G Hub, Razer Synapse, ...): give them keys no keyboard has, `f13` to
`f24`, and bind those, so the keyboard's own keys stay free (a Naga's side buttons are the number row until changed).

```lua
vana.binds.add('^!f1', '/greeter toggle')      -- Ctrl+Alt+F1 runs the addon's command
vana.binds.add('$return', '/greeter send', false)
```

Each entry of `list()`: `{ key = 'F6', vk, spec = '^f6', command, up, when = 'always' | 'closed' | 'open', ctrl, alt,
shift, win, apps, fired_ago }` (`fired_ago` seconds since it ran; -1 never).

Binds the player makes are saved by the keybinds window to `scripts\keybinds.txt`. Binds your addon adds aren't saved
anywhere: add them at `load`, and remove them at `unload` if the addon shouldn't leave them behind.

### "Press a key"

For a settings window that asks the player to press the key they want:

```lua
vana.binds.capture_begin()     -- the next key pressed is captured, not acted on
local c = vana.binds.capture() -- { active, done, cancelled, key = 'F6', vk, ctrl, alt, shift, win, apps }
vana.binds.capture_end()
```

Poll `capture()` each frame: `done` with `key` set once a key was pressed, `cancelled` on Escape; the modifier fields
follow what's held until then. A mouse button pressed, or the wheel turned or tilted, over the game is captured too
(`key = 'mbutton'`, `'xbutton1'`, `'wheelleft'`, ...), over your window as anywhere; left, right and the wheel's turns
up and down only with a modifier held, so a plain click or scroll still works your window.

While the chat line is open, binds wait (`[input] keyboard.blockbindsduringinput`, on by default), unless made with
`$`.

## Key presses: the `key` event

The game window's key messages, as they arrive. Positional arguments, not a table:

```lua
vana.on('key', function (wparam, lparam, blocked)
    local up = bit.band(lparam, 0x80000000) ~= 0
    local repeated = bit.band(lparam, 0x40000000) ~= 0 and not up
    if wparam == 0x79 and not up and not repeated then   -- F10 pressed
        toggle()
        return true   -- the game doesn't get it
    end
end)
```

`wparam` is the Windows virtual-key code (`0x41` A, `0x70` F1, `0x1B` Escape, ...), `lparam` Windows': bit 31 set for
a release, bit 30 for a repeat. `blocked` is true when a plugin or an earlier addon took it; return `true` to take it.

While the player is typing in the chat line, Ashita and Windower addons get no presses (releases still). VanaCore
addons do, so check `vana.game.world().chat_open` before acting on a plain letter.

Held modifiers aren't in the event: ask Windows through the FFI (`GetAsyncKeyState`), as the sendkey addon does, or
use a bind.

## The game's keyboard: `key_state` and `key_data`

The game reads its keyboard through DirectInput, once a frame. Two events sit on that read:

```lua
vana.on('key_state', function (e)
    -- e.data: 256 bytes, the state of every key by DirectInput key code (0x80 set while down)
    -- e.data_raw: where they are, a pointer: write through it to change what the game reads
    -- e.size: 256
    if hold_escape then
        ffi.cast('uint8_t*', e.data_raw)[0x01] = 0x80   -- DIK_ESCAPE held
    end
end)

vana.on('key_data', function (e)
    -- each buffered key event: e.key (the DIK code), e.down, e.injected, e.blocked
    return e.key == 0x3B   -- true: the game doesn't get F1
end)
```

Use these to make the game see a key it didn't get, or to
hide one from the game at the DirectInput level. For ordinary hotkeys, the `key` event or a bind is simpler.

DirectInput key codes (DIK): `0x01` Escape, `0x02` to `0x0B` the digits 1 to 0, `0x0F` Tab, `0x1C` Return, `0x1D` left
Ctrl, `0x2A` left Shift, `0x38` left Alt, `0x39` Space, `0x3B` to `0x44` F1 to F10, `0x57` F11, `0x58` F12, `0xC8`
Up, `0xD0` Down, `0xCB` Left, `0xCD` Right.

## The mouse: the `mouse` event

Positional arguments:

```lua
vana.on('mouse', function (type, x, y, delta, blocked, modifiers)
    if type == 1 and not blocked and inside(x, y) then   -- left button down on my thing
        return true   -- the game doesn't get it
    end
end)
```

| Argument | Meaning |
|----------|---------|
| `type` | The Windows message less `0x200`: 0 move, 1 left down, 2 left up, 4 right down, 5 right up, 7 middle down, 8 middle up, 10 wheel |
| `x`, `y` | In VanaCore's interface pixels (`vana.ui.screen()`'s). Ashita addons get the game's pixels instead |
| `delta` | The wheel's turn, 120 a notch (0 otherwise) |
| `blocked` | A plugin, an earlier addon, or an ImGui window took it |
| `modifiers` | Keys held: 1 Shift, 2 Ctrl, 4 Alt |

A press or wheel over an ImGui window is ImGui's alone: you hear it with `blocked` true (an Ashita addon only hears it
for its own window). What an addon takes, the game doesn't get through its messages or DirectInput either, so a click
taken by an addon doesn't turn the camera.

To block the game's mouse while over your own drawing without handling the event, set `block_mouse = true` on a
`vana.ui` box. To block every button for a moment (a modal of your own):

```lua
vana.window.block_mouse(true)          -- no mouse button reaches the game while on, and half a second after
vana.window.block_mouse(false)
local blocked = vana.window.block_mouse()   -- true while it's blocked
```

## Controllers

### Events

```lua
vana.on('xinput_button', function (e)
    -- e.button: the control (table below); e.state: 0 or 1 for a button, 0-255 for a trigger
    -- e.injected: VanaCore or an addon pressed it; e.blocked
    if e.button == 9 and e.state ~= 0 then   -- RB pressed
        return true   -- the game doesn't see it
    end
end)

vana.on('dinput_button', function (e)
    -- e.button: the DirectInput data offset; buttons are 48 + n (n from 0); e.state: 0x80 while down
end)

vana.on('xinput_state', function (e)
    -- each XInputGetState read: e.user (0-3), e.state (a pointer to the XINPUT_STATE the game is about to read);
    -- e.state_modified is the same pointer (the name Ashita 4.30 addons write their changes to)
    local state = ffi.cast('XINPUT_STATE*', e.state)   -- with your own cdef; change it in place
end)
```

A handler may change `e.button` and `e.state` in place, to remap a button. XInput controls use Ashita's numbering:

| Number | Control | Number | Control |
|--------|---------|--------|---------|
| 0 | D-pad up | 8 | LB (L1) |
| 1 | D-pad down | 9 | RB (R1) |
| 2 | D-pad left | 12 | A (Cross) |
| 3 | D-pad right | 13 | B (Circle) |
| 4 | Start (Options) | 14 | X (Square) |
| 5 | Back (Share) | 15 | Y (Triangle) |
| 6 | Left stick click (L3) | 16 | LT (L2), state 0-255 |
| 7 | Right stick click (R3) | 17 | RT (R2), state 0-255 |

The sendkey addon (`addons\vanacore\sendkey`) is a worked example: it binds a controller button to a key sent to
Windows for Discord's push to talk.

### `vana.gamepad`

```lua
local controllers, xinput, xinput_reading = vana.gamepad.list([refresh])
-- controllers: { { guid, name, product, type, opened, reading, xinput }, ... } DirectInput's (guid: what the game saves
--   as padguid000; opened: the game made a device for it this session; reading: the game reads it now, through
--   DirectInput; xinput: an XInput controller)
-- xinput: { connected0, connected1, connected2, connected3 }; xinput_reading: the slots the game reads a pad from now
vana.gamepad.game()              -- { known, on, xinput }: the game's gamepad switch and XInput switch as they are now
vana.gamepad.select(guid)        -- the game uses that controller now; true, or false and why
vana.gamepad.apply({ mode = , buttons = , buttons_ex = , guid = })   -- the pad's settings as vanacore.ini's [ffxi] has them; fields left out stay
vana.gamepad.bindings()          -- { xinput, confirm, cancel, menu, options }: the game's own buttons (-1 unset)
vana.gamepad.state()             -- { present, idle }: a controller read lately, and seconds since it was used
vana.gamepad.disable(off)        -- no controller works while `off` (not saved)
vana.gamepad.disabled()          -- true while none works
vana.gamepad.nav(on)             -- your ImGui window has the controller for the next frame (say so each frame)
```

`vana.gamepad.nav(true)` each frame while your window is open lets ImGui navigate it with the pad, and the game reads
the controller at rest meanwhile.

## Is the game in front?

```lua
vana.system.focused()   -- true while the game isn't in the background
```
