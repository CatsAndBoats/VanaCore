#pragma once

#include <cstdint>

// The HUD Manager's game windows (/vana > HUD Manager) that have no module of their own (the Target window, cast bar,
// map and chat have theirs), by their layouts' names:
//   party.move/.x/.y       the party list ("menu    partywin"): X its right edge, Y its bottom edge, as the game anchors it
//   alliance1.move/.x/.y   alliance list 1 ("menu    raid1   "): the same
//   alliance2.move/.x/.y   alliance list 2 ("menu    raid2   "): the same
//   action.move/.x/.y      the action menu, Attack, Magic, ... ("menu    playermo", Monstrosity's "mp_pmode"): X and Y
//                          its top left corner; off, the game stands it on the chat at its own left
//   chat.action=1          with action.move off, the action menu pinned to the chat at the chat's left (its bottom on the
//                          chat's top), wherever the chat is
//   action.background=0    the action menu drawn without its frame; its commands and cursor stay
//   auction.move/.x/.y     the auction house's menus ("menu    auc1", "auc2", "aucweapo", ... "wepsort", "armsort"):
//                          X their right edge, Y their top; never moved by menucontent (the item list still is)
//   mainmenu.move/.x/.y    the main menu ("menu    menuwind"): X its right edge, Y its top; the menus it opens in its
//                          place (Search, Map, ...) go where it is; mainmenu.hide leaves it out of the draw (the
//                          keys and the menus it opens still work)
//   mainmenu.background=0  the main menu, its second page ("socialme") and the menus it opens in its place (the ones
//                          below that go with it) drawn without their frames and titles; their commands and cursor stay.
//                          One closed (the main menu under a menu it opened) isn't drawn at all
//   statusmenu.move/.x/.y  the list Status opens in the main menu's place ("menu    statcom2": Combat Skl., Magic Skl.,
//                          Currencies, ...) and the lists Unity and Merit Points open in its place ("ut_menu",
//                          "merit1"...): X its right edge, Y its top; off, it goes with the main menu.
//                          statusmenu.background=0 / .solid=1 are its own (mainmenu.background doesn't reach it)
//   missions.move/.x/.y    the list Missions opens in the main menu's place ("menu    missionm": Nations, Zilart, ...):
//                          the same; never moved by menucontent (the mission lists still are)
//   actionmagic.move/.x/.y the list the Action menu's Magic opens to its right (Black Magic, Songs, Ninjutsu...: "menu
//                          magselec"): X and Y its top left; off, it's the Interaction Menu's like any window over the chat
//   actionability.move/.x/.y the list the Action menu's Abilities opens (Job Abil., Weapon Skl., Mount, ...: "menu
//                          abiselec", Monstrosity's "mp_abisc"): the same
//   actionlist.move/.x/.y  the lists the Action menu's Magic and Abilities open (the spells, the abilities: "menu    magic   ",
//                          "menu    ability "): X its left edge, Y its bottom; off, they're the Interaction Menu's
//   actionmount.move/.x/.y the list the Action menu's Mounts opens ("menu    mount   "): the same
//   jobchange.move/.x/.y   the Change Jobs list the Mog House's moogle opens ("menu    jobcselu"): X and Y its top left;
//                          off, it's the Interaction Menu's. Its Main Job / Support Job box ("jobchang") stays there
//   magicmenu, abilitymenu, synthmenu, partymenu .move/.x/.y: the same for the lists Magic, Abilities, Synthesis and
//                          Party open ("mgcmenu", "abimenu", "cmbmenu", "prty1"...)
//   sortmenu.move/.x/.y    the Options box beside the Magic, Abilities and Items lists ("mgcsortw", ...): the same. One
//                          left open after its list went is destroyed as the list would have (logged with the windows up)
//   sortconfirm.move/.x/.y the Yes/No an Options box's Auto asks ("menu    sortyn  ", "Commands"): X its right edge, Y
//                          its top; off, it stays where the game puts it (under the Options box's place, at the top right).
//                          sortconfirm.background=0 / .solid=1 are its own (mainmenu.background doesn't reach it)
//   searchmenu, regionmenu, housemenu, bazaarmenu, macromenu, configmenu .move/.x/.y: the same for the lists Search,
//                          Region Info, View House, Bazaar, Macros and Config open ("searchma", "region", "mogcont",
//                          "bazaar", "mcrmenu"..., "configwi")
//   macrooptions.move/.x/.y the box an equip set's or a macro book's row opens beside its list ("mcresmn", "mcrselop"):
//                          X its right edge, Y its top; off, the game's place at the row, on the list's other side when
//                          that's off the screen. macrooptions.background=0 / .solid=1 its frame
//   macroedit.move/.x/.y   the macro bar while Edit Macro edits it ("mcr1edit" with "mcr2edit" under it, the long bar's
//                          "mcr1edlo" / "mcr2edlo") and the editor a macro opens in its place ("macro"): X and Y its top
//                          left, the Alt row kept under the Ctrl row; off, menucontent moves them
//   macroeditoptions.move/.x/.y the box at the right beside it (Button Edit's Copy, Paste, Delete: "mcrbedit"; the
//                          editor's "mcrledit"): X its right edge, Y its top; off, where the game puts it
//   mapcontrols.move/.x/.y the map's controls (Markers, Wide Scan, Change Map: "map0", and "mapscan", "mapv1", "mapv3" in
//                          their place): X their right edge, Y their top; off, they go with the main menu
//   storagemenu.move/.x/.y the bags list Equipment and Items open beside them ("mnstorag", "itmstora"): X its right
//                          edge, Y its top
//   checkwin.move/.x/.y    the Check window (/check: "menu    inspect "): X and Y its top left; off, menucontent moves it
//   hpgauge.move/.x/.y     the HP, MP and TP gauge the Commands menu puts up ("menu    gaugewin"): the same; hpgauge.hide
//                          leaves it out of the draw
//   battletimer.move/.x/.y the time left in a battlefield or an instance ("cnttime", "eventtim", "holdtime"): X and Y its top left;
//                          off, menucontent moves it; battletimer.hide leaves it out
//   shopconfirm.move/.x/.y a shop's Yes/No before it sells or buys ("shopsell", "shopbuy"): X and Y its top left; off,
//                          where the game and menucontent put it. No hide
//   expwin.move/.x/.y      the Level / EXP box under the Status window ("menu    level   "): X and Y its top left; off,
//                          it sits under the Status window and goes with it; expwin.hide leaves it out
//   commentwin.move/.x/.y  the comment box at the bottom left (a checked player's bazaar message under the Check window,
//                          a search result's comment: "menu    comment "): X its left edge, Y its bottom;
//                          commentwin.hide leaves it out; commentwin.keep=1 keeps it up while the Check window's cursor
//                          is on an item (the game closes it for the item's details: hudwin_comment.h)
//   itemdetails.move/.x/.y every item description ("iteminfo", "item4inf"... "itemx12i"): X and Y its top left; off, it
//                          goes with its list as before
//   fulllog.move/.x/.y     the chat maximized ("menu    fulllog "): X and Y its top left corner; never moved by
//                          menucontent
//   fulllog.height         its height, a percent of the interface's (left out or 0: the game's), by the game's own resize
//   helptitle.move/.x/.y   the help bar's title box ("menu    titlewin"): X and Y its top left corner; helptitle.hide=1
//                          leaves it out; helptitle.background=0 draws it without its frame, the page's name staying
//   helpbar.move/.x/.y     the help line ("menu    helpwind"): X and Y its top left corner; its description's place,
//                          which the game takes from the window as it's made, is taken again after each move
//   helpbar.width          its width, a percent of the interface's (left out or 0: the game's), by the game's own resize
//   netwin.move/.x/.y      the Network window ("menu    netstat "): X its right edge, Y its top (network.x and .y are
//                          netstat.cpp's nudge of what's drawn in it)
//   spellinfo.move/.x/.y   the box beside the Magic and Abilities lists with the MP and recast ("menu    subwindo"): X its
//                          right edge, Y its top; spellinfo.hide leaves it out, spellinfo.background=0 draws it without
//                          its frame
//   conqbalance, conqpoints, conqcontrol .move/.x/.y/.hide/.background: the conquest map's Balance of Power ("conq6sta":
//                          X its right edge, Y its top), Conquest Points ("conq3pts": X its right edge, Y its bottom)
//                          and Regional Control ("conquer1": X and Y its top left; off, menucontent moves it)
//   besorders, besstanding .move/.x/.y/.hide/.background: the Besieged map's Orders ("beseige1", "beseige2": X and Y its
//                          top left; off, menucontent moves it) and Imperial Standing ("imperial": X its right edge, Y
//                          its bottom)
//   colorank, colobayld .move/.x/.y/.hide/.background: the Colonization map's Coalition Rank ("colorank", "coloresu": X and
//                          Y its top left; off, menucontent moves it) and Bayld ("colopoin": X its right edge, Y its bottom)
//   menucontent.move/.x/.y the windows the game lays out from the top left (Status, Equipment, Inventory, Quests...),
//                          opened together somewhere else: X and Y the top left corner of the first of them (the
//                          game's is 16, 48 of the 512 x 448 space they're laid out in; the others keep their places
//                          from it); in the world only, from the next one opened. The Status screen's Status window
//                          (16, 16) and the EXP window under it go 32 lower, the Status window's top at the box's.
//                          Never a window the game lays out over the chat (the Interaction Menu's, chat.cpp's menus.*)
//   inventory.move/.x/.y   the Items list from the main menu ("menu    inventor" at its own place, 16, 48) and its
//                          item descriptions, X and Y its top left; off, it goes with Menu Content. An item
//                          description goes with the list it opens under, whichever of the two that went by (the
//                          auction house's Sell list, a shop's), beside the list if there's no room under it
//   menucontent.hidegil   the gil window ("menu    money") left out of the draw while Status is open (the Status and
//                          Equipment screens); a shop's still shows
//   buffs.hide             the status icons left out of the draw (buffs.move is buffwin.cpp's)
//   macrobar.hide          the macro bar's palettes left out of the draw (macrobar.move is macrobar.cpp's); the keys still work
//   <id>.hide=1            every window of the table too (the main menu's lists: statusmenu, missions, magicmenu, ...,
//                          configmenu; the action menu's, the auction house's, fulllog, ...) and inventory: left out of
//                          the draw
//   hide.windows           any of the game's windows by its layout's name, the 8 letters after "menu    " (persona the
//                          Status window, money the gil window, profile, btlskill, ...), spaces or commas between: left
//                          out of the draw; name@other only while a window of the layout other is up too (persona@equip,
//                          money@equip: beside the Equipment window only). The HUD Manager's Equipment, Status Window
//                          and Gil switches are made of it (hudwin_hide.h)
//   <id>.background=0      action, actionmagic, actionability, actionlist, actionmount, jobchange, auction, storagemenu, checkwin, hpgauge, battletimer, shopconfirm, expwin, commentwin, itemdetails, macrobar, menucontent (every
//                          window of Menu Content's), inventory, target and spellinfo too: drawn without frames, their
//                          contents as ever; one closed (the Commands menu under the list it opened) isn't drawn at all,
//                          as the game draws only its frame going
//   menus.hide/.background the Interaction Menu (every window the layout stands over the chat: an NPC's selection menu,
//                          the lists the Commands menu opens; menus.move is chat.cpp's): left out, or drawn without frames
//   titles.hide=1          every menu's title tab ("Commands" over the main menu, "Magic", "Linkshell", ...) and the titles
//                          in windows' pictures ("Status", "Network", the party list's) left out of the draw (untitled)
// X and Y are percents of the interface. Off, the game places the window. (party.hide and the alliance lists' are
// alliancelist.cpp's.)
namespace vanacore::hudwin {

// `ini` (vanacore.ini): [client] <id>.* read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// From chat.cpp's hook of the game's window draw, before the window is drawn: one of the table's windows is put where its
// <id>.move says; true if it's to be left out (its <id>.hide).
bool skip(const void* ctrl);

// From the same hook, for a window it left out of the draw (skip's, the other files' hides, an empty item details box): one
// that's closed (state 0xE) gets what the game's draw does before drawing, its calc (its close timers, and its delete once
// they're over) and its input delay's count. Without it a closed window left out stayed in the manager's list for good
// (an item details box closed empty, then every trade cancelled by the game: reported 2026-11-06).
void left_out(const void* ctrl);

// The game's menu manager (CTkMenuMng, VA 0x105ECB98), once skip has found it (the first window drawn), else null
char* manager();

// From the same hook, right after skip (false) for `ctrl`: with its <id>.background 0, the window's frame and title (the
// layout's pieces) left out of this draw, by clearing the flag the game's draw goes by. The flag's value before (0: left
// as it was), for frame_back(ctrl, that) once the window's been drawn or left out, which sets it back.
std::uint8_t frameless(const void* ctrl);
void frame_back(const void* ctrl, std::uint8_t pieces);

// From the same hook, after skip (false) for `ctrl`: with [client] titles.hide=1, the menu's title tab ("Commands" over the
// main menu, "Magic", ...) and the title in a window's picture ("Status" over the Status window, "Network") left out of
// this draw. What was changed is kept in `back` (nothing: left empty), for title_back once the window's been drawn or
// left out.
constexpr int kTitleQuadsAtMost = 32;
struct TitleBack {
    std::uint8_t* shown = nullptr; // the title button's shown flag, and its value before
    std::uint8_t was = 0;
    int quads = 0; // the picture's title quads moved off the window, and their corners' places before
    char* quad[kTitleQuadsAtMost] = {};
    short y[kTitleQuadsAtMost][4] = {};
};
void untitled(const void* ctrl, TitleBack& back);
void title_back(TitleBack& back);

// The window `ctrl`'s frame is switched off by the game (its frame piece's fade mode 1, or its alpha 0, as it is after the
// game's fade-out of a window flagged 0x8000: the help bar's three) and it has no buttons. Whether anything else is drawn in
// it, the window's owner's own drawing, is not told by that: helpbar::idle says it for the help bar's windows.
bool faded(const void* ctrl);

// From itemlist.cpp's hook of the game's SetLocate (window, x, y): a Menu Content window the game puts at its own place in
// the top left (the Equipment screen puts Status, Inventory and an item's description so) goes where the box says instead
void locate(const void* window, int& x, int& y);

// Whether window `ctrl` is one of the table's that its X and Y place now (its <id>.move on): another module's moving of
// windows (equipstats.cpp's, those under the Equipment window made taller) leaves it at the HUD Manager's box
bool moved(const void* ctrl);

// Whether window `ctrl` is one of the table's (the HUD Manager's elements: placed by their own rules, moved or not)
bool own(const void* ctrl);

// Window `ctrl` put at (x, y) by the game's SetLocate as asked: locate leaves it there (no Menu Content, box or stats moving
// it again), for another module moving a window from where it is (itemtips.cpp's, those under taller item details)
void place(void* ctrl, int x, int y);

// Each frame (Present: outside the game's window draw, which goes by it): a window of the table's that goes on top while
// moved (the item details: itemdetails.move) put on a layer of its own over every menu, and back once its move is off
void raise_moved();

// In ImGui's frame (vana.layout.outline(id), /vana's eye on a Position row): a box where window `id` is or goes, Shift +
// drag moving it (<id>.move on, <id>.x and .y as it goes, not written); the frame it's let go, moved_x and moved_y are
// where it was left, for the caller to save. False for an id that isn't one that moves, or a place that can't be worked out.
bool outline(const char* id, double* moved_x = nullptr, double* moved_y = nullptr);

// vana.layout.up(id), the HUD Manager's Open only: 1 while the game window of box `id` is up (one of its layouts open and
// not closed; 'menus' a window over the chat, 'menucontent' one Menu Content takes, 'target', 'castbar', 'equip.stats' and
// 'equip.stats.check' theirs), 0 while
// it isn't, -1 for an id it can't tell (the chat, the compass, the clock, the maps: mapwin's)
int up(const char* id);

// Whether a window of `layout` is one of the main menu's lists: laid out in the main menu's place (the game opens every
// list the main menu's commands open there, and those lists' own: Config, then its Windows), and not a box of the HUD
// Manager's own (the auction house's). The lists mainmenu.background and .solid take; chatfont's mainmenu.font and
// buttons' mainmenu.buttons.* take them too
bool main_menu_list(const char* layout);

} // namespace vanacore::hudwin
