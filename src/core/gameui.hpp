#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// MOCKUP: addons changing the game's own interface (docs/addons/07-game-interface.md).
//
// An addon hides, moves, adds to or takes over part of the game's interface through vana.gameui, with nothing to declare
// first and nothing for the player to allow: an addon tells the player what it changes in its README. Every element is
// one VanaCore already knows how to change (the hooks behind [client] party.hide, target.move, configmenu,
// partyfinder.findmember); the addon asks, VanaCore does it, and undoes it when the addon goes (unloaded, crashed).
//
// Not a sandbox: an addon has vana.memory, io and LuaJIT's ffi, so it can change the game any way it likes. What this
// gives is one place that changes it (two addons don't patch the same window), and everything put back when the addon
// goes.
namespace vanacore::gameui {

// What can be done to an element. One element takes some of them (Element::actions).
enum Action : unsigned {
    kHide = 1,      // not drawn while the addon holds it (the window is still there: the game goes on using it)
    kMove = 2,      // put where the addon says, in percents of the interface, over the player's own setting
    kAdd = 4,       // a button of the addon's in one of the game's menus, with the label it gives
    kReplace = 8,   // one of the game's menu items does the addon's thing in place of the game's
    kImage = 16,    // one of the game's pictures drawn as the addon's own picture (a PNG), in its place
};

// One of the game's elements, as an addon names it ("party_list") and as the player sees it ("Party list").
struct Element {
    const char* id;
    const char* name;
    unsigned actions;
};
const std::vector<Element>& elements();
const Element* element(const std::string& id);

// kHide -> "hide"
const char* action_word(unsigned action);

// An addon by its folder and name ("ashita/xiui"): an Ashita xiui and a VanaCore one are two addons.
std::string key(const std::string& source, const std::string& name);

// addons.cpp, as an addon loads (before its file runs): it may change the game's interface from now on. `display` is its
// name as its folder spells it.
void attach(const std::string& key, const std::string& display);

// addons.cpp, as an addon goes (unloaded, crashed, reloading): all it did undone (windows shown, put back where the
// player's settings have them, its buttons out of the menus, the game's items the game's again).
void release(const std::string& key);

// ---- what an addon asks for (lua_api.cpp's vana.gameui) ----

// What an addon's call came to. Done: done (or undone). Held: another addon holds it (a move or replace: one at a time;
// `holder` names it); this one's is kept, and takes effect when the other lets go. Unknown: an element VanaCore doesn't
// know, or an action it can't do to it. Label: add's label isn't 1 to 10 letters. Colour: add's colour isn't one of
// kColours' names or '#RRGGBB'. Unloaded: not an addon that's loaded. lua_api.cpp raises an error for the last four: the
// author's mistake.
enum class Result { Done, Held, Unknown, Label, Colour, Unloaded };

// A Config list button's tint, as the game's menu sprites colour theirs: R, G, B over a grey texture, 0x7F its own
// (configmenu.cpp). The game's blue unless an addon gives another.
using Tint = std::array<std::uint8_t, 3>;
constexpr Tint kBlue = {0x60, 0x60, 0x7F};
// vana.gameui.add's colours by name: the game's own buttons' (ROM\119\51.DAT; red is Monstrosity's) and VanaCore's purple
constexpr std::pair<const char*, Tint> kColours[] = {
    {"blue", kBlue}, {"green", {0x60, 0x7F, 0x60}}, {"yellow", {0x7F, 0x7F, 0x60}}, {"red", {0x7F, 0x38, 0x38}},
    {"purple", {0x70, 0x38, 0x7F}}, {"grey", {0x60, 0x60, 0x60}},
};

Result hide(const std::string& key, const std::string& element, bool hidden);
// x and y in percents of the interface (the element's own anchor: the Target window's middle and top, the macro bar's
// top left, as its [client] <element>.x/.y have it); unmove gives the element back to the player's setting
Result move(const std::string& key, const std::string& element, double x, double y, std::string& holder);
Result unmove(const std::string& key, const std::string& element);
// The button's text is `label`, its colour `colour` (a kColours name or '#RRGGBB'; empty: the game's blue); a choice of it
// reaches the addon as 'gameui_button' (e.element)
Result add(const std::string& key, const std::string& menu, const std::string& label, const std::string& colour, bool on);
// A choice of the item reaches the addon as 'gameui_replaced' (e.element) in place of the game's action
Result replace(const std::string& key, const std::string& item, bool take, std::string& holder);

// An addon's picture in place of one of the game's (the arrow over the target): its file (a full path: PNG, BMP, JPEG,
// as image.hpp decodes them), its size in the interface's units (0: the picture's own pixels as units), how far it's
// moved from the game's own anchor (the arrow's point) in units, and a tint (0xAARRGGBB, white for none)
struct Picture {
    std::wstring file;
    float width = 0, height = 0;
    float x = 0, y = 0;
    std::uint32_t argb = 0xFFFFFFFF;
};
// One holder at a time, as move; unimage gives the game its own picture back
Result image(const std::string& key, const std::string& element, const Picture& picture, std::string& holder);
Result unimage(const std::string& key, const std::string& element);

// ---- what the game's hooks ask (game thread) ----

// chat.cpp's hook of the game's window draw: true for a window of the menu manager (a ctrl) that an addon has hidden.
// The windows go by their layouts' names ("menu    partywin" the party list, ...), as alliancelist.cpp's party.hide does.
bool skip(const void* ctrl);

// A window an addon hides by its layout's name (hide('configwi'): any of the game's windows, as menu_open names them, which
// isn't one of the elements): gamewin.cpp's draw of it drawn into nothing (its draw still made, so that what it draws is read
// and the game's own work in it done), menuhover.cpp's mouse pick finds no row of it. The keys still go to it. Asked at every
// window's draw and the mouse's pick: cheap while no addon has one (veiling false).
bool veiled(const void* ctrl);
bool veiling();

// chat.cpp: true while an addon has `element` hidden, for the elements a module hides its own way rather than by leaving
// a window's draw out: "chat_log" (the chat windows kept closed, as fancychat keeps them) and "chat_input" (the chat line
// kept off the screen, as [client] input.hide does). Asked at the draw and the calc themselves, every frame (a list kept
// up to date, so it's cheap): a hide takes effect at the window's next draw, never a frame late, as the chat line is made
// again at its place and drawn in the frame it opens in.
bool hidden(const char* element);

// lua_api.cpp's vana.gameui.hidden, for the HUD Manager's Hide switches: the loaded addons that have `element` hidden, by
// name as their folders spell them, "a, b" ("" while none does). Not the player's own [client] <element>.hide.
std::string hidden_by(const char* element);

// targetwin.cpp, macrobar.cpp, buffwin.cpp, where they read [client] <element>.move/.x/.y: true with the place an
// addon has put `element` in, which then goes before the player's own.
bool placed(const char* element, double& x, double& y);

// targetwin.cpp, where it draws the arrow over the target: true with the picture an addon has put in the game's place
// (the holder's), which then goes before the player's own [client] target.arrow.image.
bool pictured(const char* element, Picture& out);

// configmenu.cpp: the addons' items for the Config list after VanaCore's own, in the order they were added (each the
// addon's key, to tell gameui which was chosen; its name; its label; its button's tint). config_chosen is the list's
// select on one.
struct MenuItem {
    std::string key;
    std::string display; // the addon's name, for the item's help line: the player sees whose button it is
    std::string label;
    Tint tint = kBlue;
};
std::vector<MenuItem> config_items();
void config_chosen(const std::string& key);

// partymenu.cpp: true while an addon has taken over `item` ("party_menu.find_member"); and the item chosen then, in place
// of the game's action: the addon hears 'gameui_replaced' at the next frame.
bool replaced(const char* item);
void replaced_chosen(const char* item);

// ---- triggers: vana.gameui.on, catch and off ----
// What the player does in any of the game's menus, as the window hands it to the menu's handler (the window input's
// events: 1 down, 2 up, 3 right, 4 left, 5 select, 6 cancel, 0x1C the menu key while a menu is up): an addon with a
// trigger for it hears 'gameui_trigger' at the next frame, and a catch keeps the event from the handler, so the game's
// own action doesn't happen (its window has already moved its cursor, marked the item and played the sound).
constexpr int kAnyEvent = 0;
// A trigger of the loaded addon `key`: its id (1 up), or 0 when the addon isn't loaded. `menus`: layouts' names without
// their "menu    " ("menuwind", as menu_open gives them), an alias ("main_menu": both pages; "party_menu"; "config_menu")
// or "*" for every menu; `event` one of the above or kAnyEvent; `items` the item ids it's for, empty for every one;
// `swallow` for a catch. The hook goes in at the first trigger.
int trigger(const std::string& key, const std::vector<std::string>& menus, int event, const std::vector<int>& items, bool swallow);
void untrigger(const std::string& key, int id);
// The name VanaCore knows an item of `menu` by ("equipment" for menuwind's 2), "" for one it doesn't; and an item's id by
// its name (`menu` may be an alias), 0 for none.
std::string item_name(const std::string& menu, int item);
int item_id(const std::string& menu, const std::string& name);
// The hook's question (game thread): whether (event, item) for `window` (one of the menu manager's) stays from its
// handler. Queues 'gameui_trigger' to each addon whose trigger matches.
bool dispatch(const void* window, int event, int item);

} // namespace vanacore::gameui
