#pragma once

// The tabs over the chat log while it's open to scroll (Window, Say, Tell, Party, Linkshell, Linkshell 2, Assist J and E,
// Unity, Shout): with their True Font on (Chat tabs, [client] chattabs.font and its .family, .size, .bold, .italic,
// .uppercase, .outline: the titles' keys until set, as chatfont.cpp key_for reads them) each tab's name is VanaCore's text,
// the whole word in that font, laid in its tab and cut at the next tab's edge, in place of the game's letters (each a sprite
// of the tab's picture, which True Font laid as one row of words: they ran off their tabs, reported 2026-11-08). In a game in
// Japanese the tabs' letters stay the game's. [client]:
//   chat.tabs.names=full|short      "Linkshell 2" or "LS2", "Party" or "PT"...
//   chat.tabs.align=left|centre     a name from its tab's left (the game's) or in the middle of what shows of its tab
//   chat.tabs.x, chat.tabs.y        the names moved this far, the interface's units (-16 to 16)
//   chat.tabs.color, chat.tabs.selected.color   the names' colour ("#RRGGBB"), the shown tab's; white both
//   chat.tabs.spacing               how far apart the tabs not shown are, the interface's units (16 to 64, the game's 30: each
//                                   covers the last's end); the shown one is its whole picture wide. With the game's letters too
//   chat.tabs.hide=1                no tabs at all (their names nor their pictures), with the game's letters too
// and with the names VanaCore's:
//   chat.tabs.width=game|fit        the game's widths (and chat.tabs.spacing), or each tab as wide as its name and
//                                   chat.tabs.padding on both sides, chat.tabs.gap (0 to 16, 1) apart
//   chat.tabs.padding               a name that far in from its tab's left (0 to 16, 4)
//   chat.tabs.height                the tabs' height (8 to 32, the game's 12), taller upward
//   chat.tabs.background=game|flat|none   the game's picture (stretched to the width and height), a flat box of VanaCore's,
//                                   or nothing behind the names
//   chat.tabs.tint, .selected.tint  "#RRGGBB": the game's picture's tint, the shown tab's (white: its own colours)
//   chat.tabs.background.color, .background.selected.color   "#RRGGBB": a flat box's colours (#2A2E45, the shown tab's #C87A2A)
//   chat.tabs.background.opacity    0 to 100 (100)
namespace vanacore::chattabs {

// `ini` (the settings file): [client] chat.tabs.* read now, and again by reload whenever it changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: the game's drawing of the tabs hooked the first time it's wanted.
void on_frame();

// In VanaCore's interface pass (`width` x `height` the drawn pixels): the tabs' names, while the game drew the tabs this frame.
void draw(void* device, unsigned width, unsigned height);

} // namespace vanacore::chattabs
