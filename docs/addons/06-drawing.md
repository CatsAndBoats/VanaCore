# 6. Drawing

Two ways to put something on the screen:

- **`vana.ui`**: text boxes, filled rectangles and pictures that VanaCore draws for you every frame. You make one, set
  its properties when they change, and it stays until you destroy it or the addon unloads. Cheap and simple: a HUD, a
  label, a status line.
- **`vana.imgui`**: Dear ImGui, drawn from your `present` handler each frame. Windows with buttons, sliders, tables,
  text boxes: anything the player clicks on.

Both work in VanaCore's interface pixels: `vana.ui.screen()` gives the size. That's the frame's drawn size on
VanaCore's own renderers, and the game's back buffer otherwise; it isn't squeezed with the game's interface under
`[ui] aspect`, so what you draw is where the mouse finds it.

## Text boxes, rectangles and pictures: `vana.ui`

```lua
local box = vana.ui.text({ text = 'Hello', x = 200, y = 200, size = 24, bold = true })
local bar = vana.ui.rect({ x = 100, y = 100, width = 200, height = 12, color = '#30D030' })
local pic = vana.ui.image({ file = 'logo.png', x = 20, y = 20, width = 64 })
```

Each returns a handle. Read and write properties on it, or call its methods:

```lua
box.text = 'Hello again'      -- one property
box:set({ x = 10, y = 10 })   -- several at once
box:hide(); box:show()        -- the same as box.visible = false / true
box:destroy()                 -- gone; box.destroyed is then true, and any other access raises an error
print(box.width, box.height)  -- a text box's drawn size (read-only for text; set for rect and image)
```

VanaCore draws the boxes every frame in the order they were made (and by `layer`). You pay only when a property
changes: a text box is rendered again when its text or style changes, so set them only when they differ (compare
first in a `present` handler).

### Properties

| Property | text | rect | image | What |
|----------|:----:|:----:|:-----:|------|
| `x`, `y` | yes | yes | yes | The top-left corner, in pixels (top-right when `right`) |
| `right` | yes | | yes | `x` is the right edge, and lines line up on the right |
| `visible` | yes | yes | yes | Drawn or not (default true) |
| `opacity` | yes | yes | yes | 0 to 1, on top of the colours' own |
| `layer` | yes | yes | yes | -100 to 100: higher is drawn over lower; creation order within a layer |
| `block_mouse` | yes | yes | yes | While the cursor is over it, the game's mouse reads no buttons or wheel |
| `text` | yes | | | The text, UTF-8; `\n` for lines |
| `font` | yes | | | Family name (default `Segoe UI`): any font Windows has, or one in `fonts\` |
| `size` | yes | | | Pixels, 6 to 128 (default 16) |
| `bold`, `italic` | yes | | | |
| `color` | yes | yes* | yes* | The text's colour; for a rect its fill, for an image a tint (*) |
| `background` | yes | | | The box behind the text (default black at 55%) |
| `padding` | yes | | | Pixels between the text and the box's edge (default grows with the font) |
| `outline` | yes | | | An outline round the letters (1 pixel) |
| `stroke`, `stroke_color` | yes | | | The outline's width (0 to 8) and colour |
| `color_codes` | yes | | | `\cs(r,g,b)` in the text colours what follows, `\cr` goes back |
| `gradient`, `gradient_from`, `gradient_to` | yes | | | The text shaded from one colour to another by the pixel: `gradient` is `"down"` (`gradient_from` at the top of the letters, `gradient_to` at their baseline), `"across"` (from the left of the text to its right) or `"none"` (or `false`, the default) |
| `width`, `height` | read | yes | yes | A rect's size; an image's drawn size (0: its own; one of them: its shape kept) |
| `radius` | | yes | | Rounded corners, pixels (half the height: a pill) |
| `file` | | | yes | The picture: PNG, JPEG, BMP, GIF; relative to the addon's folder, or a full path |
| `repeat_x`, `repeat_y` | | | yes | Tile the picture that many times across its size |

Colours are `'#RRGGBB'`, `'#RRGGBBAA'`, or a number `0xAARRGGBB`. Reading one back gives `'#RRGGBBAA'`. A property that
isn't one of the box's raises an error naming the ones that are.

### Icons

VanaCore ships Google's Material Symbols Outlined, loaded into the game (nothing installed in Windows).

```lua
local glyph, font = vana.ui.icon('settings')   -- the character and the font to draw it with; nil if no such icon
local icon = vana.ui.text({ text = glyph, font = font, size = 20 })
```

Names are the ones on fonts.google.com/icons (`undo`, `close`, `search`, `warning`, ...). Draw them unbolded.

### Fonts

Any `.ttf`, `.otf`, `.ttc`, `.woff` or `.woff2` in `fonts\` next to `VanaCore.exe` is loaded at start and usable by
family name in `font`. The log lists them. Fonts Windows has installed work too.

### Example: a two-line HUD that follows the party

```lua
local box
vana.on('load', function ()
    box = vana.ui.text({ text = '', x = 0, y = 0, right = true, font = 'Consolas', size = 14,
                         color = '#FFFFFF', background = '#1E1E2ECC', padding = 8 })
end)

local last = ''
vana.on('present', function ()
    local party = vana.game.party()
    local me = party and party.members[1]
    local text = me and me.active and ('HP %d/%d  TP %d'):format(me.hp, vana.game.player().hp_max, me.tp) or ''
    if text ~= last then
        last = text
        box.text = text
        local w, h = vana.ui.screen()
        box.x, box.y = w - 16, h - 120
    end
end)
```

## ImGui windows: `vana.imgui`

VanaCore has Dear ImGui 1.92 built in, bound for Lua the way Ashita binds it, so Ashita's addons and their `imgui`
library work unchanged and VanaCore addons use the same calls. Draw from `present`:

```lua
local imgui = vana.imgui
local open = { true }               -- Begin's p_open is a one-item table
local scale = { 1.0 }

vana.on('present', function ()
    if not open[1] then return end
    imgui.SetNextWindowSize({ 300, 0 }, 4)                 -- 4: ImGuiCond_FirstUseEver
    if imgui.Begin('My window', open) then
        imgui.Text('Hello from ' .. addon.name)
        if imgui.Button('Say hi') then
            vana.chat.run('/p Hi!')
        end
        if imgui.SliderFloat('Scale', scale, 0.5, 2.0) then
            -- scale[1] changed
        end
    end
    imgui.End()                                            -- always, whether or not Begin returned true
end)
```

### The conventions

- **Dot calls.** `imgui.Begin(...)`, not `imgui:Begin`. A function ImGui hasn't got is `nil`.
- **Pointers are tables.** Where C++ takes `bool*`, `int*`, `float*` or `char*`, pass a table: `[1]` is read and
  written back (`[1]..[N]` for an array: `imgui.SliderFloat3('xyz', t, 0, 1)` uses `t[1]`, `t[2]`, `t[3]`). A
  `p_open` that isn't a table is no pointer.
- **Vectors.** `ImVec2` and `ImVec4` go in as tables (`{ x, y }`, a missing part 0) and come out as numbers:
  `local x, y = imgui.GetWindowPos()`.
- **Live objects.** `GetIO()`, `GetStyle()`, `GetFont()`, `GetWindowDrawList()` and the like return objects whose fields
  read and write the structure (`io.DisplaySize.x`, `style.Colors[ImGuiCol_Text + 1]`, 1-based arrays); methods take
  `:` (`draw_list:AddLine(...)`).
- **Colours** are `ImU32` numbers (`0xFF0000FF`; negative ones as `bit.bor` makes them work too). Textures are their
  pointer as a number.
- **Text is literal.** `Text`, `TextColored`, `SetTooltip` and the other printf-style functions take the text alone, no
  format arguments: use `string.format` first; `%%` shows `%`.
- **Defaults.** A missing or `nil` argument takes ImGui's default; extra arguments are ignored.
- **Numbers are ImGui 1.92's** (flags, `ImGuiCol_*`, `ImGuiCond_*`). The handful most used:

| Constant | Value | | Constant | Value |
|----------|-------|-|----------|-------|
| `ImGuiCond_Always` | 1 | | `ImGuiWindowFlags_NoTitleBar` | 1 |
| `ImGuiCond_Once` | 2 | | `ImGuiWindowFlags_NoResize` | 2 |
| `ImGuiCond_FirstUseEver` | 4 | | `ImGuiWindowFlags_NoMove` | 4 |
| `ImGuiCond_Appearing` | 8 | | `ImGuiWindowFlags_NoScrollbar` | 8 |
| `ImGuiCol_Text` | 0 | | `ImGuiWindowFlags_NoScrollWithMouse` | 16 |
| `ImGuiCol_WindowBg` | 2 | | `ImGuiWindowFlags_NoCollapse` | 32 |
| `ImGuiCol_Border` | 5 | | `ImGuiWindowFlags_AlwaysAutoResize` | 64 |
| `ImGuiCol_FrameBg` | 7 | | `ImGuiWindowFlags_NoBackground` | 128 |
| `ImGuiCol_TitleBg` | 10 | | `ImGuiWindowFlags_NoSavedSettings` | 256 |
| `ImGuiCol_Button` | 21 | | `ImGuiWindowFlags_NoMouseInputs` | 512 |
| `ImGuiCol_ButtonHovered` | 22 | | `ImGuiWindowFlags_MenuBar` | 1024 |
| `ImGuiCol_ButtonActive` | 23 | | `ImGuiStyleVar_WindowPadding` | 2 |
| `ImGuiCol_Header` | 24 | | `ImGuiStyleVar_WindowRounding` | 3 |
| `ImGuiCol_Separator` | 27 | | `ImGuiStyleVar_FramePadding` | 11 |
| | | | `ImGuiStyleVar_FrameRounding` | 12 |
| | | | `ImGuiStyleVar_ItemSpacing` | 14 |

  Flags add: `8 + 16 + 32` is a window with no scroll bar, no scrolling and no collapse. The full set is `imgui.h` in
  `third_party/imgui`; `addons\vanacore\libs\ashita-4.30\imgui.lua` defines every name for Ashita addons and is a handy
  list.

- **Draw only in a frame.** Functions that draw do nothing outside `present` (in a command handler, say). Set a flag
  there and draw next frame.
- **Mistakes don't end the game.** A `Begin` without its `End`, or a style pushed and never popped, is closed at the
  frame's end and logged once (`ImGui: fixed at the frame's end ...`).

### What's there

Nearly all of ImGui 1.92's public API, by its own names:

| Group | Functions |
|-------|-----------|
| Windows | `Begin`, `End`, `BeginChild`, `EndChild`, `SetNextWindowPos/Size/Collapsed/Focus/BgAlpha/SizeConstraints/ContentSize/Scroll`, `SetWindowPos/Size/Collapsed/Focus/FontScale`, `GetWindowPos/Size/Width/Height`, `IsWindowAppearing/Collapsed/Focused/Hovered`, `GetWindowDpiScale`, `GetContentRegionAvail`, `GetWindowDrawList`, `GetBackgroundDrawList`, `GetForegroundDrawList`, `GetVisible`, `SetVisible` |
| Layout | `Separator`, `SeparatorText`, `SameLine`, `NewLine`, `Spacing`, `Dummy`, `Indent`, `Unindent`, `BeginGroup`, `EndGroup`, `GetCursorPos*`, `SetCursorPos*`, `GetCursorStartPos`, `GetCursorScreenPos`, `SetCursorScreenPos`, `AlignTextToFramePadding`, `GetTextLineHeight*`, `GetFrameHeight*`, `PushItemWidth`, `PopItemWidth`, `SetNextItemWidth`, `CalcItemWidth`, `PushTextWrapPos`, `PopTextWrapPos`, `CalcTextSize` |
| Text | `Text`, `TextColored`, `TextDisabled`, `TextWrapped`, `TextUnformatted`, `LabelText`, `BulletText`, `Bullet`, `TextLink`, `TextLinkOpenURL`, `Value` |
| Widgets | `Button`, `SmallButton`, `InvisibleButton`, `ArrowButton`, `Checkbox`, `CheckboxFlags`, `RadioButton`, `ProgressBar`, `Image`, `ImageWithBg`, `ImageButton`, `Selectable`, `BeginDisabled`, `EndDisabled` |
| Input | `InputText`, `InputTextWithHint`, `InputTextMultiline`, `InputInt/2/3/4`, `InputFloat/2/3/4`, `InputDouble`, `InputScalar/N`, `DragInt/2/3/4`, `DragFloat/2/3/4`, `DragIntRange2`, `DragFloatRange2`, `DragScalar/N`, `SliderInt/2/3/4`, `SliderFloat/2/3/4`, `SliderAngle`, `SliderScalar/N`, `VSliderInt`, `VSliderFloat`, `VSliderScalar` |
| Colour | `ColorEdit3/4`, `ColorPicker3/4`, `ColorButton`, `SetColorEditOptions`, `ColorConvertU32ToFloat4`, `ColorConvertFloat4ToU32`, `ColorConvertRGBtoHSV`, `ColorConvertHSVtoRGB`, `GetColorU32`, `GetStyleColorVec4`, `GetStyleColorName` |
| Lists and trees | `BeginCombo`, `EndCombo`, `Combo`, `BeginListBox`, `EndListBox`, `ListBox`, `TreeNode`, `TreeNodeEx`, `TreePush`, `TreePop`, `CollapsingHeader`, `SetNextItemOpen`, `GetTreeNodeToLabelSpacing`, `PlotLines`, `PlotHistogram` |
| Menus and popups | `BeginMenuBar`, `EndMenuBar`, `BeginMainMenuBar`, `EndMainMenuBar`, `BeginMenu`, `BeginMenuEx`, `EndMenu`, `MenuItem`, `MenuItemEx`, `OpenPopup`, `BeginPopup`, `BeginPopupModal`, `EndPopup`, `CloseCurrentPopup`, `BeginPopupContextItem/Window/Void`, `OpenPopupOnItemClick`, `IsPopupOpen`, `BeginTooltip`, `EndTooltip`, `SetTooltip`, `SetItemTooltip`, `BeginItemTooltip` |
| Tables and columns | `BeginTable`, `EndTable`, `TableNextRow`, `TableNextColumn`, `TableSetColumnIndex`, `TableSetupColumn`, `TableSetupScrollFreeze`, `TableHeader`, `TableHeadersRow`, `TableAngledHeadersRow`, `TableGetSortSpecs`, `TableGetColumn*`, `TableGetHoveredColumn`, `TableGetRowIndex`, `TableSetColumnEnabled`, `TableSetBgColor`, `Columns`, `NextColumn`, `GetColumnIndex/Width/Offset`, `SetColumnWidth/Offset`, `GetColumnsCount` |
| Tabs | `BeginTabBar`, `EndTabBar`, `BeginTabItem`, `EndTabItem`, `TabItemButton`, `SetTabItemClosed` |
| Style and fonts | `PushStyleColor`, `PopStyleColor`, `PushStyleVar`, `PushStyleVarX/Y`, `PopStyleVar`, `GetStyle`, `StyleColorsDark/Light/Classic`, `PushFont`, `PopFont`, `GetFont`, `GetFontSize`, `GetFontBaked`, `GetFontTexUvWhitePixel`, `AddFontFromFileTTF`, `PushItemFlag`, `PopItemFlag` |
| Ids and items | `PushID`, `PopID`, `GetID`, `GetItemID`, `IsItemHovered/Active/Focused/Clicked/Visible/Edited/Activated/Deactivated/DeactivatedAfterEdit/ToggledOpen/ToggledSelection`, `IsAnyItemHovered/Active/Focused`, `GetItemRectMin/Max/Size`, `SetItemDefaultFocus`, `SetKeyboardFocusHere`, `SetNextItemAllowOverlap`, `SetItemKeyOwner`, `SetNextItemSelectionUserData`, `SetNextItemStorageID`, `SetNavCursorVisible` |
| Input state | `GetIO`, `GetPlatformIO`, `IsKeyDown/Pressed/Released`, `IsKeyChordPressed`, `GetKeyName`, `GetKeyPressedAmount`, `IsMouseDown/Clicked/Released/DoubleClicked/Dragging/HoveringRect/PosValid/ReleasedWithDelay`, `IsAnyMouseDown`, `GetMouseClickedCount`, `GetMousePos`, `GetMousePosOnOpeningCurrentPopup`, `GetMouseDragDelta`, `ResetMouseDragDelta`, `GetMouseCursor`, `SetMouseCursor`, `SetNextFrameWantCaptureKeyboard/Mouse`, `Shortcut`, `SetNextItemShortcut` |
| Scrolling and clipping | `GetScrollX/Y`, `SetScrollX/Y`, `GetScrollMaxX/Y`, `SetScrollHereX/Y`, `SetScrollFromPosX/Y`, `PushClipRect`, `PopClipRect`, `IsRectVisible`, `CalcListClipping` |
| Drag and drop | `BeginDragDropSource`, `EndDragDropSource`, `BeginDragDropTarget`, `EndDragDropTarget`, `SetDragDropPayload`, `AcceptDragDropPayload`, `GetDragDropPayload` |
| Misc | `GetTime`, `GetFrameCount`, `GetVersion`, `GetMainViewport`, `GetClipboardText`, `SetClipboardText`, `ShowDemoWindow`, `ShowMetricsWindow`, `ShowStyleEditor`, `ShowAboutWindow`, `ShowUserGuide`, `ShowDebugLogWindow`, `ShowIDStackToolWindow`, `ShowFontSelector`, `ShowStyleSelector`, `DebugLog`, `DebugTextEncoding`, `DebugFlashStyleColor`, `LogToTTY/File/Clipboard`, `LogButtons`, `LogText`, `LogFinish`, `LoadIniSettingsFromDisk/Memory`, `SaveIniSettingsToDisk/Memory`, `MemAlloc`, `MemFree`, `BeginMultiSelect`, `EndMultiSelect` |
| Docking and viewports | `DockSpace`, `DockSpaceOverViewport`, `GetWindowDockID`, `IsWindowDocked`, `SetNextWindowClass/DockID/Viewport`, `GetWindowViewport`, `FindViewportByID/PlatformHandle` |
| Older ImGui's names (for addons written for them) | `BeginChildFrame`, `EndChildFrame`, `ListBoxHeader`, `ListBoxFooter`, `GetContentRegionMax`, `GetWindowContentRegionMin/Max/Width`, `PushAllowKeyboardFocus`, `PopAllowKeyboardFocus`, `PushButtonRepeat`, `PopButtonRepeat`, `CaptureKeyboardFromApp`, `CaptureMouseFromApp`, `SetItemAllowOverlap`, `GetKeyIndex` |

Draw lists (`GetWindowDrawList()`, `GetBackgroundDrawList()`, `GetForegroundDrawList()`) have `AddLine`, `AddRect`,
`AddRectFilled`, `AddRectFilledMultiColor`, `AddQuad(Filled)`, `AddTriangle(Filled)`, `AddCircle(Filled)`,
`AddNgon(Filled)`, `AddEllipse(Filled)`, `AddText`, `AddBezierCubic`, `AddBezierQuadratic`, `AddPolyline`,
`AddConvexPolyFilled`, `AddConcavePolyFilled`, `PushClipRect`, `PopClipRect`, `PushTexture`, `PopTexture`:

```lua
local dl = imgui.GetBackgroundDrawList()
dl:AddRectFilled({ 10, 10 }, { 210, 40 }, 0xAA000000, 4)
dl:AddText({ 16, 16 }, 0xFFFFFFFF, 'Behind every window')
```

ImGui's structures can be made too: `imgui.types.ImGuiListClipper.new()`, `imgui.types.ImGuiTextFilter.new()`, and
the rest in `imgui.types`, each with its methods.

`ShowDemoWindow()` draws ImGui's own demo, which shows every widget with its code: the quickest way to find the call
you want.

### Fonts in ImGui

The default is Agave at 18 px with Font Awesome 6's icons merged in; 24 and 32 px are loaded too. `AddFontFromFileTTF`
adds your own to the shared atlas (rebuilt before the next frame); `PushFont`/`PopFont` switch. `SetWindowFontScale`
scales a window's text.

### The player can move and scale your windows

Every top-level `Begin`/`End` goes through VanaCore's layout system (`/vana layout`: the Addons page's Layout; or the HUD
Manager's Addon View, `/hud addons`, a yellow box over each window to Shift + drag). The player can move any addon's
window, and scale it, without the addon knowing: positions and scales are kept in `config\layout.json` by window name. Your
fonts and primitives move together, as one "Text and shapes" box. Two rules follow:

- **Give each window a stable name.** `Begin('Timers')` keeps its place; `Begin('Timers ' .. count)` is a new window
  every time the count changes. Use `###` for a changing title: `Begin('Timers (' .. n .. ')###timers')`.
- **Place windows once**, with `SetNextWindowPos(pos, 4)` (`FirstUseEver`), unless it must follow something. A window
  you place every frame (`ImGuiCond_Always`) is "pinned": the player's move becomes an offset from yours, so it still
  follows what it's anchored to.

```lua
vana.layout.windows()               -- every addon's and plugin's ImGui window: { name, owner, x, y, w, h, scale, shown, pinned, saved, locked, no_pointer }
vana.layout.set(name, x, y, scale)  -- move or scale one (nil leaves that part)
vana.layout.flags(name, locked, no_pointer) -- lock one (the mouse can't drag or resize it) and/or let the mouse through it (nil leaves that part)
vana.layout.reset(name)             -- forget what's set for it
vana.layout.highlight(name)         -- outline it this frame and the next
```

Window positions ImGui itself keeps are in `config\imgui.ini`.

### The game's windows over yours: `vana.ui.game_on_top`

ImGui is drawn after the game, so your windows cover the game's own (an NPC's choices, the action menu, item details,
shops, the macro bar). An addon that sits where the game puts its windows (a chat replacement, a big HUD) can ask for
them to go over its windows:

```lua
vana.ui.game_on_top(true)       -- from now on, until false or unload; true when called with nothing
```

Each frame, a game window that meets one of your top-level ImGui windows (as the last frame drew it) is copied as the
game drew it and laid back over everything at the end of the frame. The mouse over it is the game's. It is per addon, so
the others stay as they are, and it is dropped when the addon unloads. Ashita and Windower addons have the `vana` global
too: `if vana and vana.ui then vana.ui.game_on_top(true) end`. The player's `[client] menus.ontop=0` turns it off for
every addon.

What to expect:

- The game window is pasted back as a rectangle, over *everything* in it, other addons' windows included, and its
  corners show the game's picture behind, not your window. Give it only the windows that need it. VanaCore's own panels
  (/vana, the HUD Manager) stay over the game's windows: one that meets a panel is left where it is.
- A window that covers the screen (a full-screen overlay) puts every game window over everything. Use it on the windows
  that sit where the game's go.
- Your windows' places are the last frame's, so a window being dragged is a frame behind.
- At most 8 game windows are laid back a frame. A game window with nothing in it (under 16 of the interface's units
  high or wide: the chat log with no lines showing, the status icons with none; one closed; the help bar's title box and
  help line once the game has faded their frames out, with no menu open) is left where it is, not pasted back.
- Only the game's windows count (menus, lists, the action menu, the macro bar, shops, item details, ...). Nameplates,
  damage numbers and the target arrow are not windows and stay where they are.

### Input and ImGui

A press or wheel over an ImGui window is ImGui's alone: the game doesn't see it, and `mouse` handlers hear it with
`blocked` true. While an ImGui text box has the keyboard, keys go to ImGui and the game reads no presses. A window with
`ImGuiWindowFlags_NoMouseInputs` lets the mouse through.

For a controller: call `vana.gamepad.nav(true)` each frame your window is open and wants the pad. ImGui then navigates
with it and the game reads the controller at rest.

`imgui.GetVisible()` and `imgui.SetVisible(false)` hide every ImGui window (they're still built). `//vana` has the same
switch for the player.
