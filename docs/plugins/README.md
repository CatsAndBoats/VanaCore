# VanaCore for Ashita plugins

What VanaCore does to the game's interface for an addon (`vana.gameui`, `vana.ui.game_on_top`: see
[07-game-interface.md](../addons/07-game-interface.md)) an Ashita plugin can ask for too, through VanaCore's plugin API.
**Built 6 November 2026, not yet tried in game.**

## Getting it

Copy one header, [`src/shared/vanacore/plugin_api.h`](../../src/shared/vanacore/plugin_api.h), into your plugin. Nothing
to link: the plugin finds VanaCore when it runs, so the same DLL works in Ashita, where it just gets `NULL`.

```cpp
#include "plugin_api.h"

const VanaCorePluginApi* vana = nullptr;

bool MyPlugin::Initialize(IAshitaCore* core, ILogManager* log, uint32_t id)
{
    vana = vanacore_plugin_api(); // NULL in Ashita, or in a VanaCore older than this header
    if (vana != nullptr) {
        vana->gameui_hide("chat_input", 1); // the game's chat line off the screen: I draw my own
        vana->gameui_hide("chat_log", 1);   // the game's chat windows kept closed: I draw my own
        vana->game_on_top(1, VANACORE_ON_TOP_NOT_CHAT_LOG); // the game's menus over my ImGui windows
    }
    return true;
}
```

Two rules, as for addons:

1. **Call it from your plugin's own callbacks, on the game's thread** (`Initialize`, `Direct3DPresent`,
   `HandleCommand`, ...). That's how VanaCore knows which plugin is asking. From another thread a call does nothing and
   returns `VANACORE_NOT_A_PLUGIN`.
2. **Everything is put back when the plugin goes**: unloaded, reloaded or crashed, whether it undid it or not.

Coordinates are the game's pixels (the back buffer's): the ones ImGui, Ashita's fonts and primitives and the mouse
callbacks already use. Strings are UTF-8.

## The calls

| Call | What it does |
|------|--------------|
| `gameui_hide(element, 1 \| 0)` | `vana.gameui.hide` / `show`: an element, or any of the game's windows by its name (`"configwi"`) |
| `gameui_move(element, x, y, holder, size)` | `vana.gameui.move`: percents of the interface. `VANACORE_HELD` while another holds it |
| `gameui_unmove(element)` | back to the player's own setting |
| `gameui_image(element, &image, holder, size)` | `vana.gameui.image` (`"target_arrow"`): a `VanaCoreImage` (full path, size, offset, tint) |
| `gameui_unimage(element)` | the game's own picture again |
| `gameui_hidden_by(element, out, size)` | who has it hidden (`"FancyChat, yunaui"`); returns the length |
| `game_on_top(1 \| 0, flags)` | `vana.ui.game_on_top`: the game's windows drawn over your ImGui windows where they meet |
| `game_on_top_areas(areas, count, flags)` | the same over what you draw without ImGui (your own Direct3D, fonts, primitives): up to 8 rectangles, kept until the next call |
| `game_on_top_active()` | 1 while the game's windows are being laid over (the player's `[client] menus.ontop` on, and working) |
| `game_window_at(x, y)` | 1 when a game window laid over is at that point: let your mouse callback pass it to the game |
| `chat_input_caret(1 \| 0, x, top, bottom)` | where your chat line's caret is, so the game's auto-translate list (Tab) opens there |
| `gameui_window(name)` | `vana.gameui.window`: one of the game's windows as it stands this frame (place, cursor, rows, text, pictures), or `NULL`. [Below](#drawing-a-game-window-your-own-way) |
| `gameui_windows(out, size)` | `vana.gameui.windows`: the names of the windows up, comma separated (`"helpwind,menuwind"`) |
| `gameui_press(name, row, event)` | `vana.gameui.press`: the cursor put on a row, then a key event given to the window, as the player's keys give it |
| `entity(index, &out)` | `vana.game.entity`: the entity at a target index, all the game's record of it, in a `VanaCoreEntity` (set its `size` first). `VANACORE_UNKNOWN` when there is none. [Below](#entities-and-maximum-hp) |
| `entity_by_id(id, &out)` | the same for a server id (every slot is looked at: keep the index and call `entity` by it each frame) |
| `entity_vitals(id, &out)` | `vana.game.vitals`: a party or alliance member's HP, MP and TP with their **maximums**, also while they are in another zone, in a `VanaCoreVitals`. `VANACORE_UNKNOWN` for anyone else |
| `entity_request_max_hp(ids, count, callback, user)` | asks CatsEyeXI's server for the maximum HP of up to 8 entities by server id, a monster's too; `callback(user, answers, count)` runs on the game's thread, once, in a following frame. [Below](#asking-the-server-for-a-maximum-hp) |

What a call returns: `VANACORE_OK` (0), `VANACORE_HELD` (1, `holder` names who has it), `VANACORE_NOT_UP` and
`VANACORE_BUSY` (2 and 3, `gameui_press`), `VANACORE_UNKNOWN` (an element or action VanaCore doesn't know),
`VANACORE_BAD_ARGUMENT`, `VANACORE_NOT_A_PLUGIN`, `VANACORE_UNAVAILABLE` (VanaCore didn't find what it needs in the game). The elements and what each does are
[07-game-interface.md's](../addons/07-game-interface.md#the-elements). The player's HUD Manager shows your hides with your
plugin's name, as it does an addon's.

`flags`: `VANACORE_ON_TOP_NOT_CHAT_LOG` for a chat that stands for the game's own: the game's chat windows are never laid
over it (they would be while you don't hide `chat_log`, the player showing both).

Not for plugins yet: `vana.gameui.add`, `replace`, `on`/`catch` and `watch` (they call back into Lua). In place of
`watch`, read `gameui_window` every frame and look at its `changes`.

The API's version is 3 (`entity`, `entity_by_id`, `entity_vitals`, `entity_request_max_hp`; 2 since 0.8.205 had `gameui_window`, `gameui_windows`,
`gameui_press`): a plugin built with this header gets `NULL` from an older VanaCore. Players get the new one from the
launcher.

## Entities and maximum HP

Ashita's SDK gives a plugin the whole entity record (`IEntity`, `GetRawEntity`) and a party member's HP, but no maximum HP
for anyone but the player: the game has none for anyone else (a party member's HP and percent come from the server, any
other entity's percent alone). `entity_vitals` gives it, and `entity` is the whole record flat with the same health in
`vitals`:

```c
VanaCoreVitals v = {0};
v.size = sizeof(v);                          // always: it says how much of the struct your header knows
if (vana != NULL && vana->entity_vitals(member_id, &v) == VANACORE_OK && v.hp_max.source != VANACORE_MAX_NONE) {
    uint32_t missing = v.hp_max.value - v.hp;    // size the heal by it
    if (v.hp_max.source == VANACORE_MAX_ESTIMATE) { /* v.hp_max.low and .high say how far to trust it */ }
}
```

`VanaCoreMax.source`: `VANACORE_MAX_SELF` (the game's own, for the local player), `VANACORE_MAX_FULL` (the value seen while
the percent read 100, within a percent of the maximum, usually exact), `VANACORE_MAX_ESTIMATE` (worked out from a value and
its percent: the maximum is between `low` and `high`, and each sample narrows it) and `VANACORE_MAX_NONE` (not in the party
or alliance: only `hpp` is known; for a monster VanaCore has no number to give either). How it is worked out, and what a
curse or a weakness ending does to it, is in [04-game-data.md](../addons/04-game-data.md#maximum-hp-and-mp). Call the
entity functions from one of the plugin's callbacks on the game's thread, as every call; strings are UTF-8.

### Asking the server for a maximum HP

A monster's maximum HP is nowhere in the game. CatsEyeXI's server will say it when asked (its "VanaCore modules", packet
`0x1A7`), for you, your party and alliance, and the monsters, pets and trusts your client has been told about:

```c
static void __cdecl got_max_hp(void* user, const VanaCoreMaxHpAnswer* answers, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)       // one answer per id asked, in the order asked
        if (answers[i].status == VANACORE_MAXHP_OK)
            remember(answers[i].id, answers[i].hp, answers[i].hp_max);   // HP now (a snapshot) and the maximum
}

uint32_t id = target_server_id;
if (vana != NULL && vana->entity_request_max_hp != NULL)
    vana->entity_request_max_hp(&id, 1, got_max_hp, NULL);   // VANACORE_OK when it is sent
```

The callback runs once, on the game's thread in a following frame (never inside the request). `status` is
`VANACORE_MAXHP_OK`, `_NOT_FOUND` (not seen by your client, or a player outside your party), `_DISABLED` (the server has it
switched off), `_TOO_FAST` (asked again within 100 ms), `_HIDDEN` (the server keeps that monster's maximum secret) or `_NO_ANSWER` (nothing in 3 s: a server without the module).
Every id gets an answer, so the callback always runs, unless the plugin is unloaded first (then it never does). An `OK` answer is
also kept for a minute, its `hp_max` only, as the entity's `vitals.hp_max` (`VANACORE_MAX_SERVER`) in `entity` and `entity_by_id`
(`hp` is a snapshot of that moment: ask again for a newer one). Only HP: monsters have no MP the player is meant to know.

## From the FancyChat addon to a plugin

What VanaCore did for the FancyChat addon by reading its Lua, a plugin asks for itself:

| The addon | The plugin |
|-----------|------------|
| Enable input chat bar on: the game's chat line kept off the screen | `gameui_hide("chat_input", 1)` while the bar is on, `0` when it's off |
| Its chat up (not Show with legacy chat, not hidden): the game's chat windows kept closed, no bounce at the chat line's opening | `gameui_hide("chat_log", 1)` while it is, `0` otherwise |
| The game's menus drawn over its chat windows, tabs and input bar (`menus.ontop`) | `game_on_top(1, VANACORE_ON_TOP_NOT_CHAT_LOG)` for ImGui windows; `game_on_top_areas(...)` for what it draws itself |
| Prevent obstructing FFXI UI: its chat held still (VanaCore swapped its `SetTargetPosX`) | don't slide while `game_on_top_active()` is 1 |
| The auto-translate list over its input bar, at the phrase's column | `chat_input_caret(1, caret_x, bar_top, bar_bottom)` each frame the bar shows and the line is open; `0` when it's not ([example](#the-auto-translate-list-at-your-input-bar)) |
| Clicks on a game window over its chat go to the game (ImGui windows don't take them) | ImGui windows: the same. Your own mouse callback: pass it on while `game_window_at(x, y)` is 1 |

Calling them every frame is fine: a hide asked again changes nothing, and vanacore.log says only what changes.

## The auto-translate list at your input bar

The game puts its auto-translate list (Tab) over its own chat line, wherever that is, hidden or not. Tell VanaCore
where your bar is and where the phrase starts in it, each frame, and the list opens there: over the bar, or under it
when there's no room above, kept on the screen. VanaCore does the same for the FancyChat addon from its Lua
(`src/core/phraselist.cpp`'s `bar_place`).

```cpp
#include "plugin_api.h"
#include <algorithm>
#include <cstring>
#include <string>

static const VanaCorePluginApi* vana = nullptr; // vana = vanacore_plugin_api(); in Initialize (NULL in plain Ashita)

// Where the offered phrase goes: your bar's text up to the raw caret, measured as your bar draws its text. The game's
// raw text doesn't hold the phrase the list offers, so this is where the phrase starts.
static float phrase_x(IChatManager* chat, const InputBar& bar)
{
    const char* raw = chat->GetInputTextRaw();
    const uint32_t caret = std::min(chat->GetInputTextRawCaretPosition(), chat->GetInputTextRawLength());
    const std::string before(raw, strnlen(raw, caret));

    char shown[1024] = {};
    chat->ParseAutoTranslate(before.c_str(), shown, sizeof shown, true); // phrases spelled out, as the bar draws them
    return bar.text_left - bar.scroll + bar.measure(shown);              // your bar's own font, inset and scroll
}

// Every frame, in Direct3DPresent, after the bar is laid out
static void link_auto_translate(IChatManager* chat, const InputBar& bar)
{
    if (vana == nullptr)
        return;
    if (!bar.shown || chat->IsInputOpen() != 0x11) { // 0x11: the chat line is open
        vana->chat_input_caret(0, 0, 0, 0);
        return;
    }
    const float x = std::clamp(phrase_x(chat, bar), bar.left, bar.right - bar.inset);
    vana->chat_input_caret(1, x, bar.top, bar.bottom); // the game's pixels, as ImGui's
}
```

`InputBar` is whatever your plugin keeps for its bar; `measure` takes the text through the same steps your bar's own
text goes through (Shift-JIS to UTF-8, colour codes out). A bar that already draws its caret from
`GetInputTextRawCaretPosition` has `phrase_x` already: pass its caret's x. Don't take the caret from
`GetInputTextDisplay`: while the list is open VanaCore draws that caret after the offered phrase, so the list would
open past it.

### How it works inside VanaCore

**The game moves the list, not VanaCore.** There's no "set the list's position" call, in VanaCore or in your plugin.
VanaCore changes the answers the game gets when it asks where the list goes, and the game puts its own window there.

**The list is the game's own window.** FFXI's class for it is `FsCandidateMenu`. The game makes it together with the
chat line and keeps a pointer to it in a global. VanaCore finds that global by scanning the game's code
(`phraselist.cpp`'s `find()`) and keeps it as `g_list`. That's the list's window reference.

**Writing a position into the window wouldn't stay.** Every frame the list's own update places the window again. It
asks the chat line three questions, through the line's vtable (FFXiMain `0x103358E8`, the line object + `0x14`):

| Slot | Question | The game's answer |
|------|----------|-------------------|
| `[0]` | left | `15` |
| `[1]` | top | the chat log's bottom − 157, just over the game's own chat line |
| `[2]` | caret offset | the width of the typed text before the phrase |

It then puts the window at (left + caret offset, top). A position written into the window would be put back the next
frame. That's why the list always opened over the game's chat line, even with that line hidden or moved.

**VanaCore answers two of the questions.** It swaps slots `[1]` and `[2]` for its own `top_hook` and `offset_hook`
(`phraselist.cpp`'s `hook()`). Each frame:

1. Your plugin calls `chat_input_caret(1, x, top, bottom)`. VanaCore keeps the values for your plugin (`pluginapi.cpp`).
2. VanaCore's `phraselist::on_frame` turns them into the game's interface units. `place_at` reads the list's height
   through `g_list` and puts the list's bottom a pixel over your bar's top, or under the bar when there's no room
   above, kept on the screen.
3. The game's update asks top and caret offset. `top_hook` answers your bar's top; `offset_hook` answers your x minus
   the game's left of 15, so left + offset comes out at your x.
4. The game places its own window there, the way it always does.

```
your plugin, each frame                     VanaCore                                  the game, each frame
-----------------------                     --------                                  --------------------
chat_input_caret(1, x, top, bottom)  --->   on_frame: x, top -> interface units
                                            place_at: list's bottom over your bar
                                                                                      list update asks the line:
                                            top_hook     <-------------------------   [1] top?
                                            offset_hook  <-------------------------   [2] caret offset?
                                                                                      window at (15 + offset, top)
```

With nobody calling `chat_input_caret` (and no FancyChat addon bar), the hooks answer the game's chat line's own
window, so the list follows a line moved with `[client] input.move`.

**VanaCore keeps the list over your drawing.** Your plugin draws after the game's interface, so it would draw over
the list. VanaCore's `phraselist::on_draw` uses `g_list` to find the list's rectangle when the game draws it, copies
that area before plugins and addons draw, and puts it back after them. You don't need to do anything for that.

**One more change: the caret goes after the offered phrase.** While the list is open, the game's display text
(`GetInputTextDisplay`) holds the phrase at the caret, with the caret's mark before it. VanaCore moves the mark to
after the phrase (`phraselist.cpp`'s `display_hook`), so the game's own line draws its `|` after the phrase. That's
why your x has to come from the raw caret, not the display text.

| What | Where |
|------|-------|
| The list's window, the hooks, the place | `src/core/phraselist.cpp` |
| Your plugin's caret, kept per plugin, dropped when it unloads | `src/core/pluginapi.cpp`'s `chat_input_caret` and `caret()` |
| The list kept over plugins' and addons' drawing | `src/core/raise.cpp` (`raise::add`) |

## Drawing a game window your own way

The same as YunaUI does as an addon (`vana.gameui.hide`, `window`, `press`): the game keeps running its menu, you draw
it. Three steps:

1. **Hide it**: `gameui_hide("configwi", 1)`. The window is drawn into nothing: none of it shows, its cursor (the
   hand) and the gold bar on the cursor's row included. Its draw still runs, so its text is still read, and the keys
   and the controller still work it. The mouse doesn't (no row of it is under the pointer): clicks are yours.
2. **Read it every frame**: `gameui_window("configwi")` in `Direct3DPresent`, after the game has drawn its windows,
   so it's this frame's. `NULL` while it isn't up.
3. **Send the player's choice back**: `gameui_press("configwi", row.id, VANACORE_EVENT_SELECT)`. It goes through the
   game's own key input, so the window plays its sound and opens what it opens, as if the player had pressed the key.

```cpp
#include "plugin_api.h"

static const VanaCorePluginApi* vana = nullptr;
static uint32_t laid_out = 0; // the window's `changes` at your last layout

// Initialize
vana = vanacore_plugin_api();
if (vana != nullptr)
    vana->gameui_hide("configwi", 1); // the game's Config menu drawn into nothing: you draw it

// Direct3DPresent, every frame
if (vana != nullptr) {
    const VanaCoreWindow* w = vana->gameui_window("configwi");
    if (w != nullptr && !w->closing) {
        if (w->changes != laid_out) { // it moved, its cursor moved, a row greyed out, its text changed
            laid_out = w->changes;
            // redo anything you keep laid out
        }
        draw_frame(w->x, w->y, w->w, w->h); // where the game would draw it: where the player put it
        for (uint32_t i = 0; i < w->row_count; ++i) {
            const VanaCoreWindowRow& row = w->rows[i];
            if (!row.drawn)
                continue;
            draw_button(row.x, row.y, row.w, row.h, row.text, row.cursor != 0, row.disabled != 0);
            if (clicked(row.x, row.y, row.w, row.h) && !row.disabled)
                vana->gameui_press("configwi", row.id, VANACORE_EVENT_SELECT); // cursor on it, then select
        }
        for (uint32_t i = 0; i < w->line_count; ++i) // everything else it wrote: values, a list's names and counts
            draw_text(w->lines[i].x, w->lines[i].y, w->lines[i].text, w->lines[i].argb);
    }
}

// Escape in your window: back out, as the player's cancel key
vana->gameui_press("configwi", 0, VANACORE_EVENT_CANCEL);
```

`draw_frame`, `draw_button`, `draw_text` and `clicked` are yours (ImGui, your own Direct3D, Ashita's fonts).

**What a window gives** (`VanaCoreWindow`, all places in the game's pixels, as your drawing):

| Field | What |
|-------|------|
| `x`, `y`, `w`, `h` | Its place: where the game would draw it, hidden or not |
| `scale` | The game's pixels to one of its interface units, for sizing your drawing as the game's |
| `cursor`, `count` | The id of the row the cursor is on; how many rows the cursor goes over |
| `focused`, `closing`, `hidden`, `drawn` | The keys go to it; it's closing; someone hides it; `lines` and `pictures` are from a draw of it |
| `changes` | Up by one each time a read finds anything else than your last read did |
| `rows` | Its buttons: `id` (what `cursor` and `gameui_press` use), place, `text` (the label, `""` for a list's lines), `state`, `cursor`, `disabled`, `drawn` |
| `lines` | Every run of text its last draw made, labels included, UTF-8, with its place and colour |
| `pictures` | The rest it drew: icons, on/off boxes, arrows, its frame's pieces, by the game's name for each (`"menu    keytop"`) |

- **The pointer stays good until you ask for the same window again**, or your plugin unloads. Asking for another
  window doesn't touch it.
- **Ask every frame.** A window's text and pictures are kept while a plugin or addon asks for it (at least every 3
  seconds) or hides it. Only the very first ask has `drawn` 0 and no lines; the next frame's is whole, and so is the
  first read after it opens.
- **`VANACORE_BUSY`** from `gameui_press`: the window has just taken a key and won't take another this frame. Press
  again next frame. `VANACORE_NOT_UP`: it's gone or closing. `VANACORE_BAD_ARGUMENT`: a row past `count`.
- **Only that window.** Its help line at the bottom (`"helpwind"`) and the windows it opens are their own windows:
  hide and read those by their names too.
- **Other game windows over yours:** `game_on_top(1, 0)` (above) lays the game's windows over your ImGui windows where
  they meet; `game_on_top_areas` over what you draw yourself.

**Finding a window's name.** `gameui_windows` lists the windows up, so open the menu and see which name is new. Or in
game, the API Explorer addon (`/addon load apiexplorer`, `/api`) has a **Game interface** group: *Find a window's name*
lists each window as you open it, *Elements to hide, move or change* lists the named elements, *Read a window* shows
everything a window gives. Names seen so far: `menuwind` (the main menu), `configwi` (Config), `prty1` (the main
menu's Party list), `helpwind` (the help line), `partywin` (the party list).

## The player can move what you draw

Without any call: the HUD Manager's Addon View (`/hud addons`) and Addons > Layout move and scale a plugin's ImGui windows, its Ashita fonts and primitives, and what it draws on the device itself in its Direct3D callbacks (Ashita's `Direct3DBeginScene`, `Direct3DEndScene`, `Direct3DPresent`; Windower's frame, `PreRender` and `PostRender`). Those screen-space (XYZRHW) draws onto the back buffer are drawn moved, with your fonts and primitives, as one box named after your plugin; your mouse callbacks get the mouse back in your own place, so your hit tests and drags work unchanged. Keep drawing where you placed yourself: don't add the move in. Draws made with transformed vertices (a 3D marker in the world) aren't moved.
