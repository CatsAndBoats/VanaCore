/* vana.layout.up (the HUD Manager's Open only): whether the game window a HUD box stands for is up now; and main_menu_list,
 * whether a layout is one of the main menu's lists. hudwin.cpp includes this inside its namespace, after its helpers
 * (open_window, window_named, table_window, over_anchor, content_member, in_menu_slot, g_looks), whose names it uses. It is
 * no file of its own. */
#pragma once

namespace {

// boxes another module draws (targetwin.cpp's, castbar.cpp's, equipstats.cpp's) for a window out of the table, by its layout
constexpr const char* kUpLayouts[][2] = {{"target", "menu    targetwi"},
                                         {"castbar", "menu    casttime"},
                                         {"equip.stats", "menu    equip   "},
                                         {"equip.stats.check", "menu    inspect "}};

// some window in the manager's list, not closed, that `pick` takes
template <class Pick> bool any_up(Pick pick)
{
    int count = 0;
    for (char* node = at<char*>(g_manager, 0); node != nullptr && count < 256; node = at<char*>(node, 0), ++count) {
        char* window = at<char*>(node, 0x10);
        if (window != nullptr && at<std::uint32_t>(window, kWinState) != kWinClosed && at<char*>(window, kWindowLayout) != nullptr &&
            pick(window))
            return true;
    }
    return false;
}

} // namespace

int up(const char* id)
{
    if (id == nullptr || !found() || g_manager == nullptr)
        return -1;
    // the Interaction Menu: a window the layout stands over the chat (an NPC's choices, the Commands menu's lists) that has
    // no box of its own in the table
    if (std::strcmp(id, "menus") == 0)
        return any_up([](char* window) { return over_anchor(window) && table_window(window) == nullptr; }) ? 1 : 0;
    // Menu Content: a window it takes (note_content)
    if (std::strcmp(id, "menucontent") == 0)
        return any_up([](char* window) { return content_member(window, at<char*>(window, kWindowLayout)); }) ? 1 : 0;
    // the main menu: its own window, or a list it opened in its place (Config's, its second page, Linkshell's, ...), which
    // closes it (state 0xE) while that's up (in_menu_slot)
    if (std::strcmp(id, "mainmenu") == 0)
        return any_up([](char* window) {
                   return named(window, "menu    menuwind") || in_menu_slot(at<char*>(window, kWindowLayout));
               })
                   ? 1
                   : 0;
    if (std::strcmp(id, "inventory") == 0)
        return open_window(kItemsName) != nullptr ? 1 : 0;
    for (const auto& other : kUpLayouts)
        if (std::strcmp(id, other[0]) == 0)
            return open_window(other[1]) != nullptr ? 1 : 0;
    const Window* w = window_named(id);
    if (w == nullptr)
        return -1;
    for (const char* name : w->layouts)
        if (name[0] != 0 && open_window(name) != nullptr)
            return 1;
    return 0;
}

bool main_menu_list(const char* layout)
{
    if (layout == nullptr)
        return false;
    __try {
        if (!in_menu_slot(layout))
            return false;
        // a box of the table's: the main menu's own, or a list it opens in its place (kFollow); one with a box of its own
        // laid out there (the auction house's, the storage list, Conquest's) isn't
        for (const auto& w : g_windows)
            for (const char* name : w.layouts)
                if (name[0] != 0 && std::memcmp(layout + kLayoutName, name, 16) == 0)
                    return (w.can & kFollow) != 0 || std::strcmp(w.id, "mainmenu") == 0;
        for (const auto& look : g_looks)
            if (look.layout[0] != 0 && std::memcmp(layout + kLayoutName, look.layout, 16) == 0)
                return false;
        return true; // any other list there, at any depth (Config's Windows "conf5m", Log "conf11m", Font Colors "conftxtc")
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
