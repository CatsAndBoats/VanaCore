/* outline (vana.layout.outline, /vana's eye on a Position row): a table window's box, Shift + drag moving it. hudwin.cpp
 * includes this inside its namespace, after its helpers (window_named, found, area, wanted, g_manager), whose names it
 * uses. It is no file of its own. */
#pragma once

bool outline(const char* id, double* moved_x, double* moved_y)
{
    Window* w = id != nullptr ? window_named(id) : nullptr;
    if (w == nullptr || (w->can & kMove) == 0 || !found() || g_manager == nullptr)
        return false;
    // a list that goes with the main menu (move off) has no box of its own: it would lie on the main menu's, and Shift +
    // drag there took whichever of the six was drawn first, so the main menu stayed and a list was moved off it (seen
    // 2026-10-22). Its Position switch on, its box is where its X and Y put it
    if ((w->can & kFollow) != 0 && !w->move)
        return false;
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    if (!area(area_left, area_top, area_w, area_h))
        return false;
    // where its X and Y put it (or the game, as it last made it), at the size it last had
    const int width = w->seen_w > 0 ? w->seen_w : w->default_w, height = w->seen_h > 0 ? w->seen_h : w->default_h;
    outline::Box box;
    if (!wanted(*w, w->move, width, height, box.left, box.top))
        return false;
    box.right = box.left + width;
    box.bottom = box.top + height;
    // move off and laid out over the chat (the Action menu's Magic and Abilities sub-menus...): it goes with the Interaction
    // Menu's box ("menus", chat.cpp) until it's dragged off, which its box shows (a chain link; amber as that's dragged)
    if (!w->move && w->over_chat)
        outline::docked("menus", "the Interaction Menu");
    outline::Drag drag;
    if (!outline::draw(w->id, box, 0, drag))
        return false;
    // dragged: the window goes where the box is taken to (move on) at once
    if (drag.held) {
        w->move = true;
        w->x = std::clamp(std::round((drag.left + w->anchor_x * width - area_left) * 10000.0 / area_w) / 100.0, 0.0, 100.0);
        w->y = std::clamp(std::round((drag.top + w->anchor_y * height - area_top) * 10000.0 / area_h) / 100.0, 0.0, 100.0);
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = w->x;
        *moved_y = w->y;
    }
    return true;
}
