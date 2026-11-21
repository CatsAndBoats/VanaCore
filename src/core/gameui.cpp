#include "gameui.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "addons.hpp"
#include "gamewin.hpp"
#include "log.hpp"
#include "scan.hpp"

// MOCKUP (docs/addons/07-game-interface.md): not built or tested in game.

namespace vanacore::gameui {
namespace {

// The elements VanaCore can change for an addon, and the windows each is in the game's menu manager (for hide: their
// layouts' names, 16 characters at a layout's +0x46, as alliancelist.cpp and targetwin.cpp find them; none for one its
// own module hides its own way and asks hidden() for: the chat log's windows kept closed and the chat line kept off the
// screen, chat.cpp's). One goes in this list once the hook that changes it is VanaCore's: hide comes with a layout's name
// (chat.cpp's window draw asks skip());
// move with the module that places the window asking placed() (targetwin.cpp does; buffwin.cpp and macrobar.cpp would
// in the same three lines, and their elements take kMove then); add and replace with the menu's handler hooked
// (configmenu.cpp, partymenu.cpp).
struct Known {
    Element element;
    const char* layouts[4];
};
constexpr Known kKnown[] = {
    {{"chat_log", "Chat log", kHide}, {}},
    {{"chat_input", "Chat line", kHide}, {}},
    {{"party_list", "Party list", kHide}, {"menu    partywin"}},
    {{"alliance_lists", "Alliance lists", kHide}, {"menu    raid1   ", "menu    raid2   "}},
    {{"target_window", "Target window", kHide | kMove}, {"menu    targetwi"}},
    {{"target_arrow", "Target arrow", kHide | kImage}, {}}, // the arrow over the target: targetwin.cpp asks hidden() and pictured()
    {{"status_icons", "Status icons", kHide}, {"menu    buff    "}},
    {{"macro_bar", "Macro bar", kHide}, {"menu    mcr1pall", "menu    mcr2pall", "menu    mcr1long", "menu    mcr2long"}},
    {{"network_window", "Network window", kHide}, {"menu    netstat "}},
    {{"nameplates", "Nameplates", kHide}, {}}, // every name over a character's head: nameplates.cpp asks hidden()
    {{"config_menu", "Config menu", kAdd}, {}},
    {{"party_menu.find_member", "Party > Find Member", kReplace}, {}},
};

constexpr std::pair<unsigned, const char*> kActions[] = {
    {kHide, "hide"}, {kMove, "move"}, {kAdd, "add"}, {kReplace, "replace"}, {kImage, "image"}};

// The Config list's items are 88 x 16, drawn in the menu font from letters VanaCore knows the places of
// (configmenu.cpp's kLabel): a label of up to 10 letters and spaces
constexpr std::size_t kLabelMost = 10;

constexpr std::ptrdiff_t kWindowLayout = 4; // a window's layout
constexpr std::ptrdiff_t kLayoutName = 0x46;

// Something an addon has asked for through vana.gameui, kept while it's loaded. `since` orders those that can have one
// holder (a move, a replace): the first to ask holds it, the next in line when it lets go.
struct Ask {
    unsigned action = 0;
    std::string element;
    double x = 0, y = 0; // kMove's place
    Picture picture;     // kImage's
    std::string label;   // kAdd's, and its button's tint
    Tint tint = kBlue;
    std::uint64_t since = 0;
};

// A trigger an addon has set (vana.gameui.on / catch): which menus (layouts' names without "menu    ", or "*"), which
// window event (kAnyEvent: every one), which items (empty: every one), and whether the event is kept from the handler
struct Trigger {
    int id = 0;
    std::vector<std::string> menus;
    int event = kAnyEvent;
    std::vector<int> items;
    bool swallow = false;
};

// An addon loaded since the game started: while it's loaded, what it asks for now
struct Addon {
    std::string key;
    std::string display;
    bool loaded = false;
    std::vector<Ask> asks;
    std::vector<Trigger> triggers;
};

std::vector<Addon> g_addons; // in the order they were first loaded
std::uint64_t g_sequence = 0;
int g_trigger_ids = 0;
bool g_any_triggers = false; // a loaded addon has a trigger: the hook asks dispatch() only then

void refresh_triggers()
{
    g_any_triggers = false;
    for (const Addon& addon : g_addons)
        if (addon.loaded && !addon.triggers.empty())
            g_any_triggers = true;
}

// The layouts' names skip() leaves out: every hide of a loaded addon (made again by apply())
std::vector<std::array<char, 16>> g_hidden;
// The layouts hidden by their own names (an addon's hide('configwi')), which veiled() has drawn into nothing
std::vector<std::array<char, 16>> g_veiled;
constexpr std::size_t kLayoutWord = 8; // a layout's name after "menu    "
// The elements those hides are of, for hidden() (chat.cpp asks it from the game's window draw and the chat windows' calc,
// every frame: a list made at each change, not a search of the addons' asks)
std::vector<std::string> g_hidden_elements;

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

const Known* known(const std::string& id)
{
    for (const Known& k : kKnown)
        if (id == k.element.id)
            return &k;
    return nullptr;
}

// Whether VanaCore can do `action` to `element`
bool can(const std::string& element, unsigned action)
{
    const Known* k = known(element);
    return k != nullptr && (k->element.actions & action) != 0;
}

// `element` names one of the game's windows by its layout, as menu_open gives it ("configwi", "prty1"): 1 to 8 letters and
// digits, and none of the elements'
bool layout_word(const std::string& element)
{
    return !element.empty() && element.size() <= kLayoutWord && known(element) == nullptr &&
           std::all_of(element.begin(), element.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

// A layout word's 16 characters as the layout has its name: "menu    configwi", "menu    prty1   "
std::array<char, 16> layout_of_word(const std::string& word)
{
    std::array<char, 16> name;
    name.fill(' ');
    std::memcpy(name.data(), "menu    ", 8);
    std::memcpy(name.data() + 8, word.data(), std::min(word.size(), kLayoutWord));
    return name;
}

Addon* find(const std::string& key)
{
    for (Addon& addon : g_addons)
        if (addon.key == key)
            return &addon;
    return nullptr;
}

// The loaded addon of `key`, else nullptr
Addon* loaded(const std::string& key)
{
    Addon* addon = find(key);
    return addon != nullptr && addon->loaded ? addon : nullptr;
}

Ask* ask_of(Addon& addon, unsigned action, const std::string& element)
{
    for (Ask& ask : addon.asks)
        if (ask.action == action && ask.element == element)
            return &ask;
    return nullptr;
}

Ask& keep_ask(Addon& addon, unsigned action, const std::string& element)
{
    if (Ask* ask = ask_of(addon, action, element))
        return *ask;
    addon.asks.push_back({action, element, 0, 0, {}, {}, kBlue, ++g_sequence});
    return addon.asks.back();
}

void drop_ask(Addon& addon, unsigned action, const std::string& element)
{
    addon.asks.erase(std::remove_if(addon.asks.begin(), addon.asks.end(),
                                    [&](const Ask& ask) { return ask.action == action && ask.element == element; }),
                     addon.asks.end());
}

// The addon that holds a move or replace of `element`: of the loaded ones asking for it, the first to ask
Addon* holder_of(const std::string& element, unsigned action, const Ask** held = nullptr)
{
    Addon* first = nullptr;
    const Ask* first_ask = nullptr;
    for (Addon& addon : g_addons) {
        if (!addon.loaded)
            continue;
        const Ask* ask = ask_of(addon, action, element);
        if (ask != nullptr && (first_ask == nullptr || ask->since < first_ask->since))
            first = &addon, first_ask = ask;
    }
    if (held != nullptr)
        *held = first_ask;
    return first;
}

// What's in effect made again from every loaded addon's asks: the windows skip() leaves out. The rest (placed,
// config_items, replaced) is worked out as it's asked.
void apply()
{
    g_hidden.clear();
    g_hidden_elements.clear();
    g_veiled.clear();
    for (const Addon& addon : g_addons) {
        if (!addon.loaded)
            continue;
        for (const Ask& ask : addon.asks) {
            if (ask.action == kHide && layout_word(ask.element)) {
                const std::array<char, 16> name = layout_of_word(ask.element);
                if (std::find(g_veiled.begin(), g_veiled.end(), name) == g_veiled.end())
                    g_veiled.push_back(name);
                continue;
            }
            const Known* k = ask.action == kHide ? known(ask.element) : nullptr;
            if (k == nullptr)
                continue;
            if (std::find(g_hidden_elements.begin(), g_hidden_elements.end(), ask.element) == g_hidden_elements.end())
                g_hidden_elements.push_back(ask.element);
            for (const char* layout : k->layouts) {
                if (layout == nullptr)
                    break;
                std::array<char, 16> name;
                std::memcpy(name.data(), layout, 16);
                if (std::find(g_hidden.begin(), g_hidden.end(), name) == g_hidden.end())
                    g_hidden.push_back(name);
            }
        }
    }
}

} // namespace

const std::vector<Element>& elements()
{
    static const std::vector<Element> list = [] {
        std::vector<Element> out;
        for (const Known& k : kKnown)
            out.push_back(k.element);
        return out;
    }();
    return list;
}

const Element* element(const std::string& id)
{
    const Known* k = known(id);
    return k != nullptr ? &k->element : nullptr;
}

const char* action_word(unsigned action)
{
    for (const auto& [a, name] : kActions)
        if (a == action)
            return name;
    return "";
}

std::string key(const std::string& source, const std::string& name)
{
    return lower(source) + "/" + lower(name);
}

void attach(const std::string& key, const std::string& display)
{
    Addon* addon = find(key);
    if (addon == nullptr) {
        g_addons.push_back({key, display});
        addon = &g_addons.back();
    }
    addon->display = display;
    addon->loaded = true;
    addon->asks.clear();
    addon->triggers.clear();
    refresh_triggers();
    gamewin::release(key); // (a reload: its watches went with its state)
}

void release(const std::string& key)
{
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return;
    const bool had = !addon->asks.empty() || !addon->triggers.empty();
    addon->loaded = false;
    addon->asks.clear();
    addon->triggers.clear();
    refresh_triggers();
    gamewin::release(key); // its vana.gameui.watch
    apply();
    if (had)
        log::write(L"Game UI: %hs went: what it changed in the game's interface is put back", addon->display.c_str());
}

// ---- what an addon asks for ----

Result hide(const std::string& key, const std::string& element, bool hidden)
{
    if (!can(element, kHide) && !layout_word(element))
        return Result::Unknown;
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return Result::Unloaded;
    if (hidden)
        keep_ask(*addon, kHide, element);
    else
        drop_ask(*addon, kHide, element);
    apply();
    return Result::Done;
}

Result move(const std::string& key, const std::string& element, double x, double y, std::string& holder)
{
    if (!can(element, kMove))
        return Result::Unknown;
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return Result::Unloaded;
    Ask& ask = keep_ask(*addon, kMove, element);
    ask.x = std::clamp(x, 0.0, 100.0);
    ask.y = std::clamp(y, 0.0, 100.0);
    const Addon* first = holder_of(element, kMove);
    if (first != nullptr && first != addon) {
        holder = first->display;
        return Result::Held;
    }
    return Result::Done;
}

Result unmove(const std::string& key, const std::string& element)
{
    if (!can(element, kMove))
        return Result::Unknown;
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return Result::Unloaded;
    drop_ask(*addon, kMove, element);
    return Result::Done;
}

Result image(const std::string& key, const std::string& element, const Picture& picture, std::string& holder)
{
    if (!can(element, kImage))
        return Result::Unknown;
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return Result::Unloaded;
    Ask& ask = keep_ask(*addon, kImage, element);
    ask.picture = picture;
    const Addon* first = holder_of(element, kImage);
    if (first != nullptr && first != addon) {
        holder = first->display;
        return Result::Held;
    }
    return Result::Done;
}

Result unimage(const std::string& key, const std::string& element)
{
    if (!can(element, kImage))
        return Result::Unknown;
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return Result::Unloaded;
    drop_ask(*addon, kImage, element);
    return Result::Done;
}

namespace {

// A button's colour as its tint: a kColours name (any case), or '#RRGGBB' with each channel halved (FF the texel's own,
// 7F); empty, the game's blue. False for anything else.
bool tint_of(const std::string& colour, Tint& tint)
{
    tint = kBlue;
    if (colour.empty())
        return true;
    std::string name = colour;
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& [known, its] : kColours)
        if (name == known) {
            tint = its;
            return true;
        }
    if (name.size() != 7 || name[0] != '#' || !std::all_of(name.begin() + 1, name.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
        return false;
    for (int i = 0; i < 3; ++i)
        tint[i] = static_cast<std::uint8_t>((std::stoi(name.substr(1 + 2 * i, 2), nullptr, 16) * 0x7F + 127) / 255);
    return true;
}

} // namespace

Result add(const std::string& key, const std::string& menu, const std::string& label, const std::string& colour, bool on)
{
    if (!can(menu, kAdd))
        return Result::Unknown;
    if (on && (label.empty() || label.size() > kLabelMost))
        return Result::Label;
    Tint tint = kBlue;
    if (on && !tint_of(colour, tint))
        return Result::Colour;
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return Result::Unloaded;
    if (on) {
        Ask& ask = keep_ask(*addon, kAdd, menu);
        ask.label = label;
        ask.tint = tint;
    } else
        drop_ask(*addon, kAdd, menu);
    return Result::Done;
}

Result replace(const std::string& key, const std::string& item, bool take, std::string& holder)
{
    if (!can(item, kReplace))
        return Result::Unknown;
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return Result::Unloaded;
    if (!take) {
        drop_ask(*addon, kReplace, item);
        return Result::Done;
    }
    keep_ask(*addon, kReplace, item);
    const Addon* first = holder_of(item, kReplace);
    if (first != nullptr && first != addon) {
        holder = first->display;
        return Result::Held;
    }
    return Result::Done;
}

// ---- what the game's hooks ask ----

namespace {

// The 16 characters of the name of the layout a window was made from; false if they can't be read
bool layout_name(const void* ctrl, char (&name)[16])
{
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kWindowLayout);
        if (layout == nullptr)
            return false;
        std::memcpy(name, layout + kLayoutName, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool skip(const void* ctrl)
{
    if (g_hidden.empty() || ctrl == nullptr) // every window drawn comes here: nothing more while no addon hides one
        return false;
    char name[16];
    if (!layout_name(ctrl, name))
        return false;
    for (const std::array<char, 16>& hidden : g_hidden)
        if (std::memcmp(hidden.data(), name, 16) == 0)
            return true;
    return false;
}

bool veiled(const void* ctrl)
{
    if (g_veiled.empty() || ctrl == nullptr) // every window drawn and the mouse's pick come here: nothing more while none is
        return false;
    char name[16];
    if (!layout_name(ctrl, name))
        return false;
    for (const std::array<char, 16>& veiled : g_veiled)
        if (std::memcmp(veiled.data(), name, 16) == 0)
            return true;
    return false;
}

bool veiling()
{
    return !g_veiled.empty();
}

bool hidden(const char* element)
{
    if (g_hidden_elements.empty()) // every frame, from the window draw and the chat windows' calc: nothing more while no addon hides
        return false;
    for (const std::string& hidden : g_hidden_elements)
        if (hidden == element)
            return true;
    return false;
}

std::string hidden_by(const char* element)
{
    std::string out;
    for (const Addon& addon : g_addons) {
        if (!addon.loaded)
            continue;
        for (const Ask& ask : addon.asks) {
            if (ask.action != kHide || ask.element != element)
                continue;
            if (!out.empty())
                out += ", ";
            out += addon.display;
            break;
        }
    }
    return out;
}

bool placed(const char* element, double& x, double& y)
{
    const Ask* ask = nullptr;
    if (holder_of(element, kMove, &ask) == nullptr || ask == nullptr)
        return false;
    x = ask->x;
    y = ask->y;
    return true;
}

bool pictured(const char* element, Picture& out)
{
    const Ask* ask = nullptr;
    if (holder_of(element, kImage, &ask) == nullptr || ask == nullptr)
        return false;
    out = ask->picture;
    return true;
}

std::vector<MenuItem> config_items()
{
    std::vector<std::pair<std::uint64_t, MenuItem>> found;
    for (Addon& addon : g_addons) {
        const Ask* ask = addon.loaded ? ask_of(addon, kAdd, "config_menu") : nullptr;
        if (ask != nullptr)
            found.push_back({ask->since, {addon.key, addon.display, ask->label, ask->tint}});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<MenuItem> out;
    for (auto& [since, item] : found)
        out.push_back(std::move(item));
    return out;
}

void config_chosen(const std::string& key)
{
    log::write(L"Game UI: %hs's item chosen in the Config list", key.c_str());
    addons::queue_event(key, "gameui_button", "config_menu");
}

bool replaced(const char* item)
{
    return holder_of(item, kReplace) != nullptr;
}

void replaced_chosen(const char* item)
{
    const Addon* holder = holder_of(item, kReplace);
    if (holder == nullptr)
        return;
    log::write(L"Game UI: %hs chosen: %hs's in place of the game's", item, holder->display.c_str());
    addons::queue_event(holder->key, "gameui_replaced", item);
}

// ---- triggers ----

namespace {

// The menus an addon may name as one word, and the layouts they are
constexpr std::pair<const char*, const char*> kMenuAliases[] = {
    {"main_menu", "menuwind"}, {"main_menu", "socialme"}, {"party_menu", "prty1"}, {"config_menu", "configwi"},
};

// The items VanaCore knows by name (research/mainmenu-re.md's tables; partymenu.cpp's list): the main menu's two pages
// and the Party list. On the single main menu (singlemenu.cpp) page 2's items are menuwind's 16 to 27.
struct ItemName {
    const char* menu;
    int item;
    const char* name;
};
constexpr ItemName kItemNames[] = {
    {"menuwind", 1, "status"},       {"menuwind", 2, "equipment"},  {"menuwind", 3, "magic"},       {"menuwind", 4, "items"},
    {"menuwind", 5, "abilities"},    {"menuwind", 6, "party"},      {"menuwind", 7, "trade"},       {"menuwind", 8, "search"},
    {"menuwind", 9, "linkshell"},    {"menuwind", 10, "synthesis"}, {"menuwind", 11, "region_info"}, {"menuwind", 12, "map"},
    {"socialme", 1, "missions"},     {"socialme", 2, "quests"},     {"socialme", 3, "key_items"},   {"socialme", 4, "view_house"},
    {"socialme", 5, "set_bazaar"},   {"socialme", 6, "macro_palette"}, {"socialme", 7, "config"},   {"socialme", 8, "help_desk"},
    {"socialme", 9, "communication"}, {"socialme", 10, "current_time"}, {"socialme", 11, "shut_down"}, {"socialme", 12, "log_out"},
    {"prty1", 1, "seek_party"},      {"prty1", 2, "find_member"},   {"prty1", 3, "request"},        {"prty1", 4, "view_request"},
    {"prty1", 5, "autogroup"},       {"prty1", 6, "languages"},     {"prty1", 7, "alarm_type"},
};

// A layout's 16-character name as an addon names the menu: "menu    menuwind" -> "menuwind" (trailing spaces off)
std::string menu_of(const char (&name)[16])
{
    const bool prefixed = std::memcmp(name, "menu    ", 8) == 0;
    std::string menu(name + (prefixed ? 8 : 0), prefixed ? 8 : 16);
    while (!menu.empty() && (menu.back() == ' ' || menu.back() == '\0'))
        menu.pop_back();
    return menu;
}

// The layouts `menu` names: an alias's, else itself
std::vector<std::string> layouts_of(const std::string& menu)
{
    std::vector<std::string> out;
    for (const auto& [alias, layout] : kMenuAliases)
        if (menu == alias)
            out.push_back(layout);
    if (out.empty())
        out.push_back(menu);
    return out;
}

// The hook: the window input's tail (FUN_10117e60's, at 0x101180C6 on 2026-10-25), where a window hands the event and
// its cursor item to its handler's slot 6: mov ecx,[esi+0Ch] (the handler) / mov dx,[esi+4Ch] (the item) / push edx /
// push ebp (the event) / mov eax,[ecx] / call [eax+18h] / mov eax,[esp+0Ch] / pop edi / pop esi / pop ebp. The 5 bytes
// from mov eax,[ecx] become a call to dispatch_stub, which asks dispatch() first. Every way in comes through here: the
// keyboard and the controller (the manager's key dispatch), the mouse (a click on an item, the wheel).
constexpr char kDispatchPattern[] = "8B 4E 0C 66 8B 56 4C 52 55 8B 01 FF 50 18 8B 44 24 0C 5F 5E 5D";
constexpr std::ptrdiff_t kDispatchCall = 9;
bool g_hooked = false;
bool g_hook_failed = false;

// The stub's question. The event and the item are shorts to the game.
int __cdecl decide(const void* window, int event, int item)
{
    return dispatch(window, static_cast<short>(event), static_cast<short>(item)) ? 1 : 0;
}

// In place of the game's mov eax,[ecx] / call [eax+18h]: ecx is the handler, esi the window, the event and the item are
// pushed, and the return into the game's tail is above them. Either the handler gets the call as the game made it (a
// jump: the handler's own ret 8 returns into the tail), or the stub returns as the handler would have, without it.
__declspec(naked) void dispatch_stub()
{
    __asm {
        push ecx                        ; the handler, kept across decide
        push dword ptr [esp + 12]       ; the item
        push dword ptr [esp + 12]       ; the event
        push esi                        ; the window
        call decide
        add esp, 12
        pop ecx
        test eax, eax
        jnz caught
        mov eax, dword ptr [ecx]
        jmp dword ptr [eax + 18h]       ; the handler's slot 6, returning into the game's tail
    caught:
        ret 8
    }
}

void install_hook()
{
    if (g_hooked || g_hook_failed)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    const std::uintptr_t match = scan::find(game, kDispatchPattern);
    if (match == 0)
        return; // not decrypted yet, or another client: tried again at the next trigger
    const std::uintptr_t site = match + kDispatchCall;
    std::uint8_t code[5] = {0xE8};
    const std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&dispatch_stub) - (site + 5));
    std::memcpy(code + 1, &rel, 4);
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof code, PAGE_EXECUTE_READWRITE, &protect)) {
        g_hook_failed = true;
        log::write(L"Game UI: the menus' input dispatch couldn't be hooked (error %lu): vana.gameui.on and catch do nothing", GetLastError());
        return;
    }
    std::memcpy(reinterpret_cast<void*>(site), code, sizeof code);
    VirtualProtect(reinterpret_cast<void*>(site), sizeof code, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof code);
    g_hooked = true;
    log::write(L"Game UI: the menus' input dispatch hooked (FFXiMain.dll+0x%X) for vana.gameui.on and catch",
               static_cast<unsigned>(site - reinterpret_cast<std::uintptr_t>(game)));
}

} // namespace

int trigger(const std::string& key, const std::vector<std::string>& menus, int event, const std::vector<int>& items, bool swallow)
{
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return 0;
    Trigger t;
    t.id = ++g_trigger_ids;
    for (const std::string& menu : menus)
        for (const std::string& layout : layouts_of(menu))
            t.menus.push_back(layout);
    t.event = event;
    t.items = items;
    t.swallow = swallow;
    addon->triggers.push_back(std::move(t));
    refresh_triggers();
    install_hook();
    return addon->triggers.back().id;
}

void untrigger(const std::string& key, int id)
{
    Addon* addon = loaded(key);
    if (addon == nullptr)
        return;
    addon->triggers.erase(std::remove_if(addon->triggers.begin(), addon->triggers.end(), [id](const Trigger& t) { return t.id == id; }),
                          addon->triggers.end());
    refresh_triggers();
}

std::string item_name(const std::string& menu, int item)
{
    // the single main menu's page 2: menuwind's 16 to 27 are socialme's 1 to 12
    if (menu == "menuwind" && item >= 16 && item <= 27)
        return item_name("socialme", item - 15);
    for (const ItemName& known : kItemNames)
        if (known.item == item && menu == known.menu)
            return known.name;
    return "";
}

int item_id(const std::string& menu, const std::string& name)
{
    for (const std::string& layout : layouts_of(menu))
        for (const ItemName& known : kItemNames)
            if (layout == known.menu && name == known.name)
                return known.item;
    return 0;
}

bool dispatch(const void* window, int event, int item)
{
    if (!g_any_triggers || window == nullptr)
        return false;
    char name[16];
    if (!layout_name(window, name))
        return false;
    const std::string menu = menu_of(name);
    struct Hit {
        const Addon* addon;
        int id;
    };
    std::vector<Hit> hits;
    bool swallow = false;
    for (const Addon& addon : g_addons) {
        if (!addon.loaded)
            continue;
        for (const Trigger& t : addon.triggers) {
            if (t.event != kAnyEvent && t.event != event)
                continue;
            if (!t.items.empty() && std::find(t.items.begin(), t.items.end(), item) == t.items.end())
                continue;
            if (std::find(t.menus.begin(), t.menus.end(), "*") == t.menus.end() &&
                std::find(t.menus.begin(), t.menus.end(), menu) == t.menus.end())
                continue;
            hits.push_back({&addon, t.id});
            swallow = swallow || t.swallow;
        }
    }
    if (hits.empty())
        return false;
    const std::string named = item_name(menu, item);
    for (const Hit& hit : hits)
        addons::queue_trigger(hit.addon->key, hit.id, menu, event, item, named, swallow);
    if (swallow)
        log::write(L"Game UI: event %d on %hs's item %d (%hs) kept from the game for %hs", event, menu.c_str(), item,
                   named.c_str(), hits.front().addon->display.c_str());
    return swallow;
}

} // namespace vanacore::gameui
