# Timestamps and a filter on the chat log

An addon that puts the time in front of each chat line and drops lines that match words you give it. It shows the
`text_in` event: reading a line's mode, changing what's shown, and blocking a line.

## `chatstamp.lua`

```lua
addon.name    = 'chatstamp'
addon.author  = 'You'
addon.version = '1.0'
addon.desc    = 'A timestamp on every chat line, and a word filter.'

local config = vana.settings.register({
    defaults = { stamp = true, filter = {} },   -- filter: a list of words; a line with one of them isn't shown
})

local function filtered(message)
    local lower = message:lower()
    for _, word in ipairs(config.data.filter) do
        if lower:find(word, 1, true) then return true end
    end
    return false
end

vana.on('text_in', function (e)
    if e.injected then return end                    -- lines VanaCore and addons write: leave them
    local mode = bit.band(e.mode, 0xFF)              -- the chat mode itself; the high bytes are flags
    if #config.data.filter > 0 and filtered(e.message) then
        return true                                  -- blocked: not shown
    end
    if config.data.stamp and not e.indent then       -- not on a continuation line
        e.message_modified = os.date('[%H:%M] ') .. e.message_modified
    end
end)

vana.on('command', function (e)
    local what, rest = e.command:lower():match('^/cs%s+(%S+)%s*(.*)$')
    if what == nil then
        if e.command:lower():match('^/cs') then print('/cs stamp on|off, /cs add <word>, /cs remove <word>, /cs list') return true end
        return
    end
    if what == 'stamp' then
        config.data.stamp = rest == 'on'
    elseif what == 'add' and rest ~= '' then
        table.insert(config.data.filter, rest)
    elseif what == 'remove' then
        for i, word in ipairs(config.data.filter) do
            if word == rest then table.remove(config.data.filter, i) break end
        end
    elseif what == 'list' then
        print('Filtered: ' .. (#config.data.filter > 0 and table.concat(config.data.filter, ', ') or 'nothing'))
    end
    config:save()
    return true
end)
```

## What to notice

- **`e.message_modified` is what's shown.** Assigning to it changes the line; `e.message` stays the original. Other
  addons after this one see the change.
- **Returning `true` blocks the line.** `e.blocked = true` does the same. The game keeps the line in its log either way;
  it just isn't shown.
- **`e.injected`** marks lines VanaCore or an addon wrote (`print`, `vana.chat.write`). Leaving those alone keeps your
  own output and other addons' readable.
- **`e.indent`** is set on the continuation lines of a long message, so the stamp goes on the first line only.
- **The text is Shift-JIS with the game's codes in it.** `find` with plain matching on ASCII words works; for the line
  as the player sees it, `vana.ashita.parse_auto_translate(e.message, true)` spells out auto-translate phrases, and
  colour codes are `[\30\31].` for `gsub`.
- **The chat mode** is in the low byte of `e.mode`. The numbers are the game's: say, shout, party, linkshell, tell,
  battle messages and so on, each a different colour in the player's settings.

## `README.md`

```markdown
# chatstamp

Puts the time in front of each chat line, and hides lines containing words you add with /cs add <word>.
Changes nothing in the game's interface.
```
