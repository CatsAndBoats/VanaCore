# 5. Chat and commands

## Writing to the chat log

```lua
print(...)                      -- the arguments joined with spaces, as a line in the chat log (and VanaCore's log)
vana.chat.write(text[, mode])   -- a line (or lines: "\n" splits) in the chat log, in chat mode `mode`'s colour
vana.log(...)                   -- VanaCore's log only (logs\vanacore.log), as "<addon>: ..."
```

`mode` is the game's chat mode number, which picks the line's colour. VanaCore uses these for its own lines:

| mode | Colour | VanaCore uses it for |
|------|--------|----------------------|
| 1 | White | The default: what `print` uses |
| 2 | Green | Success ("Loaded: ...") |
| 68 | Red | Errors |
| 104 | Yellow | Warnings |
| 106 | Cream | Messages |

Other numbers are the game's own chat modes (say, party, linkshell, ...), each in the colour the player set for it.

```lua
vana.chat.write('Something went wrong: ' .. why, 68)
```

Long lines are split as the game needs. Text is passed to the game as bytes: a string in your UTF-8 source with
characters past ASCII should go through `vana.encoding.to_shift_jis` first (below).

## Running a command or saying something

```lua
vana.chat.run(line[, mode])   -- runs `line` as if typed, on the next frame
```

Anything the player could type: a game command (`'/ma "Cure IV" <t>'`, `'/p Pulling'`, `'/equip main "Excalibur"'`), a
VanaCore command (`'/addon reload tparty'`, `'/bind ^f1 /ja "Provoke" <t>'`), or a line of chat. It goes through every
addon's `command` handlers like a typed line, with `e.injected` true.

Plain chat (no leading `/`) goes to the current chat mode as the game would send it. Mind what you send: the server
sees it as the player's.

## Your own commands

Every line starting with `/` reaches your `command` handler before the game. Match yours, do it, return `true`:

```lua
vana.on('command', function (e)
    local args = {}
    for word in e.command:gmatch('%S+') do args[#args + 1] = word end
    if args[1]:lower() ~= '/greeter' then
        return
    end
    local what = (args[2] or ''):lower()
    if what == 'on' then
        enabled = true
    elseif what == 'off' then
        enabled = false
    else
        print('/greeter on|off')
    end
    return true
end)
```

What `e` holds: `command` (the whole line), `mode` (1 for the chat box), `injected` (true for a line an addon or
script ran), `blocked` (an earlier addon took it). Returning `true`, or setting `e.blocked = true`, keeps the line from
the game. A line nothing takes goes to the game, which says it isn't a command (for `/x`) or sends it as chat.

Lines starting with `//` are addons' own by convention: the game never sees them. One that no addon claims raises the
`unhandled` event (`e.command` without the slashes) in every addon, and if none returns `true`, the chat log says
`Unknown command`.

Quoted arguments (`/greeter say "hello there"`) are yours to parse: `e.command` is the raw line.

## Reading chat lines

Each line coming into the chat log raises `text_in`; each line the player sends raises `text_out`:

```lua
vana.on('text_in', function (e)
    local mode = bit.band(e.mode, 0xFF)
    if mode == 9 and e.message:find('Defeated', 1, true) then   -- 9 is a battle message mode
        return true -- not shown
    end
end)
```

| `e` field | `text_in` | `text_out` |
|-----------|-----------|------------|
| `message` | The line as the game has it: Shift-JIS, with the game's control codes (colours, auto-translate) | The line typed |
| `message_modified` | Assign to change what's shown | Assign to change what's sent |
| `mode` | The three mode bytes: `bit.band(e.mode, 0xFF)` is the chat mode | The mode typed in |
| `mode_modified` | Assign to change the colour | |
| `indent`, `indent_modified` | Whether the line is indented (a continuation) | |
| `injected` | VanaCore or an addon wrote it | |
| `blocked` | An earlier addon blocked it | the same |

Returning `true` or setting `e.blocked = true` drops the line. Changes to `*_modified` apply even when you don't block.

A line your own addon prints doesn't come back into its own `text_in` (other addons' do), and lines bounce between
addons at most four deep.

```lua
-- a timestamp on every line
vana.on('text_in', function (e)
    if not e.indent then
        e.message_modified = os.date('[%H:%M] ') .. e.message_modified
    end
end)
```

### Auto-translate phrases and control codes

Chat text carries the game's codes: colours (`0x1E`, `0x1F` followed by a colour byte), auto-translate phrases
(`0xFD` ... `0xFD`), and line layout bytes. To read a line as the player sees it:

```lua
local plain = vana.ashita.parse_auto_translate(e.message, true)   -- phrases spelled out; `true` puts them in brackets
```

Colour codes are two bytes; strip them with `text:gsub('[\30\31].', '')` if you don't want them.

## Text encoding

Your Lua file is UTF-8 and the game is Shift-JIS. ASCII is the same in both, so most addons never notice. For anything
else (accented letters, Japanese, the game's special glyphs):

```lua
vana.encoding.to_shift_jis(utf8)      -- for text you hand the game: chat lines, vana.chat.run, packets
vana.encoding.from_shift_jis(sjis)    -- for text from the game you show elsewhere: a file, a web request, an ImGui window
```

`vana.ui.text` and `vana.imgui` take UTF-8. Names from `vana.game` and lines from `text_in` are Shift-JIS.

## Regular expressions

Lua patterns are usually enough. For a real regular expression (ECMAScript, as C++'s `std::regex` and Ashita's have
it), compiled once and kept:

```lua
vana.ashita.regex.search(text, pattern)      -- every match: { { match, group1, ... }, ... }, or nil
vana.ashita.regex.match(text, pattern)       -- the whole text matched: { match, group1, ... }, or nil
vana.ashita.regex.replace(text, pattern, with)   -- $1 for a group
vana.ashita.regex.split(text, pattern)       -- the pieces between matches
```

A bad pattern gives `nil` (`search`, `match`), the text as it was (`replace`), or the text alone (`split`).

## The clipboard

```lua
local text = vana.system.clipboard()   -- the clipboard's text, Shift-JIS; '' when it has none
vana.system.set_clipboard(text)        -- Shift-JIS text onto the clipboard; true if it did
```

## While the player is typing

`vana.game.world().chat_open` is true while the chat line is open. Addons that act on keys should hold off then
([Input](09-input.md)); the `chat_input_open` and `chat_input_close` events mark the moments.
