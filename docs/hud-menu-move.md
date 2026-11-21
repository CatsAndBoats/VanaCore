# Making a game menu movable in the HUD Manager

How the Auction menu was given its own HUD Manager box (2026-10-22, works in game). The same steps work for any menu
the game opens through its menu manager (CTkMenuMng): find its windows' layout names, add one entry to `g_windows` in
`src/core/hudwin.cpp`, and add the rows in the Lua.

## 1. Find the menu's windows (live, VanaCore MCP)

Every window is in the menu manager's list (FFXiMain VA 0x105ECB98: `+0` the first node, node `+0` the next, node
`+0x10` the window). A window: `+4` its layout (`+0x46` its 16-character name, e.g. `"menu    aucammo "`), `+0x10` its
state (dword, 0xE = closed), `+0x38` its area (word; 0x81 for the main menu and the menus that take its place), `+0x3A`
its rect (four shorts: left, top, right, bottom, in interface units).

FFXiMain's base changes with every game start, so get the manager's address first with `read_memory`
`ghidra(0x105ECB98)` (it prints the loaded address), then open the menu in game yourself (see below) and list what's up:

```lua
local ffi = require('ffi')
local rd = function(a) return ffi.cast('uint32_t*', a)[0] end
mng = 0x07B3CB98 -- the loaded address of VA 0x105ECB98 this game
local out, node, n = {}, rd(mng), 0
while node ~= 0 and n < 200 do
  local win = rd(node + 0x10)
  if win ~= 0 then
    local lay = rd(win + 4)
    local r = ffi.cast('int16_t*', win + 0x3A)
    out[#out+1] = string.format('%08X [%s] area=%04X st=%X rect=%d,%d,%d,%d', win, ffi.string(ffi.cast('char*', lay + 0x46), 16),
                                ffi.cast('uint16_t*', win + 0x38)[0], rd(win + 0x10), r[0], r[1], r[2], r[3])
  end
  node = rd(node); n = n + 1
end
return table.concat(out, '\n')
```

A menu is often several windows (the Auction menu is 15, one per page, all opened in the same place). To get every
name, not only the ones you clicked through:

- The manager's layouts (`+0x1C`, same node shape, node `+0x10` the layout): filter the names by a prefix (`auc`). A
  layout's `+0x10..+0x16` is its rect in the 512 x 448 design space; the same rect for every page means one slot.
- `search_memory` text `menu    auc` in FFXiMain.dll, then `find_references` on a name: the routine that closes the
  whole menu (a DestroyDrawMenu 0x1015D640 per name) lists every window it owns. The Auction one, ~0x101D8B40, also
  closed `wepsort` and `armsort`, which the prefix missed.

Keep a window out of the set if it isn't part of the "panel" (the Auction item list `auclist` / `auchisto` sits beside
the menu and stays with Menu Content).

## 2. Work out where the game puts it and what moves it today

- **Its anchor**: the rect tells you. The Auction layouts say 384, 48, but the windows open at 1792, 48 on a 1920 x
  1200 interface: the game added the interface's width - 512, so it's anchored to the right edge
  (`ffxi-window-placement-re`: FUN_10118FF0, anchor byte 1 = right, 2 = bottom, 3 = both). Pick X/Y as that corner.
- **What else moves it**: in the menu's case it was Menu Content. `hudwin::locate` (from itemlist.cpp's SetLocate
  hook) and `place_frame_hook` shift any window that isn't in `g_windows` and lies in the top-left 512 x 448, which
  sometimes took the Auction menu into Menu Content's box. Being in the table is what stops that
  (`content_window`), as does the window's over-anchor flag (+0x34 bit 0x1000, `over_anchor`: one the layout stands
  over the chat, which chat.cpp's `menu_place` puts at the Interaction Menu box; Menu Content moved those a second
  time whenever that box lay in the top-left 512 x 448, the list mid-screen and its selection at the box, 2026-10-23).
  Other movers to look for: `follow_menu` (area-0x81 windows at the main menu's slot), chat.cpp's menus over the chat,
  and modules with their own placing (`kOwnPlaced`: the Target window, the cast bar).
- **A menu the main menu opens in its place** (Missions' `missionm`, Status' `statcom2`; made with CreateDrawMenu
  straight from the main menu's handler, e.g. 0x1014D5C1): the game opens it at the main menu's current place. With
  `mainmenu.move` putting the main menu in the top left, that place is inside Menu Content's 512 x 448, so Menu
  Content shifted it again (Missions opened at 131, 230 for a main menu at 12, 139 and a Menu Content shift of 119, 91).
  Give it `kMove | kFollow`, X its right edge (anchor 1, 0, defaults 99.4, 3.0): off, it goes with the main menu every
  frame. Any other main-menu sub-menu not in the table has the same double shift.

## 3. The C++: one table entry (`src/core/hudwin.cpp`)

```cpp
{"auction", L"Auction menu", kMove,
 {"menu    auc1    ", "menu    auc2    ", /* ... up to 16 names, padded to 16 characters */},
 1, 0,        // which point X and Y are: 0 left/top, 1 right/bottom (the game's anchor)
 99.2, 4.0,   // X and Y's defaults: the game's place in percents of the interface (1904/1920, 48/1200)
 112, 154},   // the box's size before the window has been seen
```

That's all the code. `skip()` (called from chat.cpp's hook of the window draw, before each window is drawn) finds the
window by layout name and `place()` calls SetLocate (0x10118D10) on it every frame it isn't where `<id>.x/.y` say,
so a page the game opens again, or moves itself, is put back before it's ever drawn. Off, the window stays where the
game made it, and the box shows that place.

The flags: `kMove` (`<id>.move/.x/.y`), `kHide` (`<id>.hide`: left out of the draw), `kWidth` (`<id>.width`, the
game's resize), `kFollow` (off, it goes with the main menu: the Status sub-menu), `kStatus` (off, it sits under the Status
window as it is now, wherever Menu Content put it and however tall status.taller made it: the Level / EXP box `expwin`,
"menu    level   ", whose handler (0x10147010) SetLocates it back where it was made every frame, so `locate` redirects
that SetLocate to the element's place as well as `place` putting it there before each draw; statuswindow.cpp leaves it
out of the windows status.taller moves down).

Things already handled in `place()`:
- **Closed pages**: a menu's earlier pages stay in the manager's list in state 0xE while the next is open, and the draw
  hook is still called for them. `place()` skips state-0xE windows; without that the box's size flipped between pages
  every frame.
- **More than 16 windows**: raise `char layouts[16][16 + 1]` in `struct Window`.

Watch for (seen with other windows):
- A window whose owner caches its place when it opens (the help bar's title box: `title_moved` works the text's place
  out again after each move; the Target window's owner rect is only refreshed by SetLocate, not Resize).
- A list window whose item description follows it (`g_shifts`): move those as they're made (`content_shift`, as
  Inventory does), not per frame, so the description knows how far its list went.
- A window that lays its own buttons out at fixed screen places after it's made (the Quests window: SetButtonRect
  0x10118580 with tops of 62, 94, 158... and the left kept from the button, so the buttons' left went with the window and
  their tops didn't). `hudwin.cpp`'s `quest_page` hands such a page its buttons as at the game's place and adds the
  window's `g_shifts` back when it sets them. The same window's text draw also took a button's screen left as a place
  inside the window (greyed Vana'Bout over "Plaudits :"), so its draw is wrapped too. Three other menus call SetButtonRect (0x101378F1, 0x1022905E, 0x1023E522
  in the 2025-12-26 client); look there first if another menu's buttons stay behind.

## 4. The Lua (`addons/vanacore/vana/`)

- `data.lua`, HUD page: a section, in alphabetical order with the others, its heading an `element` (the HUD Manager
  window's list, hud.lua; a plain `header` would be a /vana sub-tab instead):
    ```lua
    element('Auction Menu'),
    subheader('Position'),
    place('Position', 'auction', 'X: its right edge. Y: its top edge.', 99.2, 4, 'auction'),
    anchor('auction', true), -- true: its right edge snaps to the grid
    ```
- `vana.lua`: `hud.NAMES` (the box's name in the game; every id in it gets a box while the HUD Manager is open), `hud.NO_HIDE` (no Hide switch), `hud.RIGHT` (right-anchored),
  `hud.SECTIONS` only if a click on the box should open another section than its name (a main-menu sub-menu's rows
  sit under Main Menu as a subheader, so `missions = 'Main Menu'`).
- `data.lua` has CRLF line endings (since 2026-10-22); a scripted edit has to match `\r\n`.

Done so far: `expwin` (the Level / EXP box under the Status window, `level`; `kStatus`), `commentwin` (the comment box at the bottom left, `comment`: a checked player's bazaar message under the Check window and a search result's comment, one layout made from several places, 0x1013347A, 0x1020BFE6...; anchored to the bottom edge, so X its left and Y its bottom, anchor 0, 1; never Menu Content's), `auction` (Auction menu, own section), `missions` (Mission menu, under Main Menu), `statusmenu` (with
Unity's `ut_menu` and Merit Points' `merit1/2/3`), `magicmenu` (`mgcmenu`), `abilitymenu` (`abimenu`, `mp_mnabi`),
`synthmenu` (`cmbmenu`, `cmbhwin`), `partymenu` (`prty1`...`prty8`, `prty5us/fr/de`; not the invite's `prtyjoin`),
`sortmenu` (the lists' Options boxes, `mgcsortw`, `abisortw`, `itmsortw`, ...), `sortconfirm` (their Auto's Yes/No, `sortyn`, laid out at 384, 124 on the right; not a follower), `fulllog` (the chat maximized; `kHeight`),
`searchmenu` (`searchma` and the `search*`/`sc*` lists in its slot), `regionmenu` (`region`), `housemenu` (View House's
`mogcont`), `bazaarmenu` (`bazaar`), `macromenu` (`mcrmenu`, `mcrselec`, `mcres20`, `mcrmogc`; the books' Options popup
`mcrselop` and the equip sets' `mcresmn` are laid out at 0, 0 and put at the chosen row by the list, once: they're
`macrooptions`, off at the game's place), the macro bar while it's edited (`macroedit`: `mcr1edit`/`mcr1edlo` with the Alt
row `mcr2edit`/`mcr2edlo` under it by `kUnder`, its handler having put it 42 lower once as it opened, 0x101A3630; and the
editor `macro` in their place; `kContent`) and the box beside it (`macroeditoptions`: Button Edit's `mcrbedit` at 384, 134
on the right, the editor's `mcrledit` at 384, 48; area 0, not followers; the open/close routines 0x101A2C30 - 0x101A2E10),
any window the game puts beside another and partly off the screen (`beside`
in `locate`: mirrored to the other window's other side), `storagemenu` (Equipment's `mnstorag`, Items' `itmstora`; not a
follower), the conquest map's `conqbalance` (`conq6sta`), `conqpoints` (`conq3pts`) and `conqcontrol` (`conquer1`, laid
out at 16, 48 and moved by Menu Content: `kContent`), the Besieged map's `besorders` (`beseige1`, and `beseige2` in its
place while a stronghold is under the cursor; `kContent`) and `besstanding` (`imperial`), the Colonization map's
`colorank` (`colorank`, and `coloresu` in its place for a zone other than Adoulin; `kContent`) and `colobayld`
(`colopoin`), and the zone map's `mapcontrols` (`map0`, with `mapscan`, `mapv1`, `mapv3` laid out in its slot; `kFollow`;
Wide Scan's list `mapv2` and the markers list `maplist` are placed elsewhere and left to the game), and the Mog House's Change Jobs list `jobchange` (`jobcselu`, laid out over the chat like the Action menu's lists, so the Interaction Menu's until it was in the table; its Main Job / Support Job box `jobchang` stays the Interaction Menu's). Campaign's boxes (`campresu`,
`campsan`, `camparea`, `allied`) are listed in the region maps' close, 0x101E6650; each map kind's calc (0x101E6A80
Conquest, 0x101E6CD0 Besieged, 0x101E6E10, 0x101E7010) shows which box replaces which.

Testing a move by calling SetLocate (0x10118D10) from Lua: a place in the top-left 512 x 448 gets Menu Content's shift
added by hudwin's SetLocate hook, so to put a Menu Content window back, give it the game's place (16, 48), not where it was.

The second page of the main menu (Missions ... Log Out, "socialme") opens its lists from its own handler around
0x1014D5C1: `missionm`, `bazaar`, `mcrmenu`, `configwi`, `mogcont`, `commenu`, and `/quest`, `/keyitem`, `/clock` as
commands. A list of every layout the game has loaded is the manager's `+0x1C` list (the step 1 Lua with `+0x1C`): the
main-menu slot's are the ones at 384, 48.

Where to find a main-menu sub-menu's name without opening it: the main menu's select handler is a jump table at
0x10140254 (15 items, cases from 0x1013FFD3), each a CreateDrawMenu (0x1015D080) with its name pushed. The Status
sub-menu's handler (0x101486A0) opens Unity's at 0x10148771 and the pages in the table at 0x1036F6C0 (32 bytes each:
profile, joblevel, btlskill, mgcskill, trdskill, evitem, merit1, merit2ca).

`vana.layout.outline(id)` (lua_api.cpp) falls through to `hudwin::outline`, so the box, Shift + drag and saving work
without more code.

## 5. Docs, build, deploy

- `src/core/hudwin.hpp`: a line in the list of keys. `vanacore.example.ini`: a commented block. `CHANGELOG.md`: a bullet.
- Build, then copy `VanaCore.Core.dll` (+ .pdb, .map) and the two Lua files into the game's VanaCore folder while the
  game isn't running.

## 6. Testing

Open the menu yourself: don't press keys in the game from a script (key_state Enter) to talk to an NPC or open a
menu. On 2026-10-22 one such Enter landed in an open auction house. With it open, the list in step 1 shows the rect;
`vana.config.set('client', '<id>.move', 1)` and `<id>.x/.y` move it live.
