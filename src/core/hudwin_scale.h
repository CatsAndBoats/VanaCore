/* [client] <id>.scale (the HUD Manager's Scale): a table window drawn bigger or smaller, about its anchor point (its top right
 * corner for the main menu, its bottom right for the party list), so its X and Y still say where that point is. The game
 * lays the window out as ever; the renderer maps its draw (vanacore_gfx_window_scale: its vertices, its clip and True Font's
 * letters) and what the game asks of the mouse about it is asked back at the place the game laid it out: IsOnWindow (the
 * pointer over a window: its keys, the lists' mouse blocks) and IsOnButton (chat.cpp's on_button_hook: the row or button
 * under the pointer). The main menu's lists that go with it (kFollow, their Position off) and the windows laid out in its
 * place that aren't the table's (its second page, Linkshell's...) take mainmenu.scale. Every table window with a Position
 * (kMove) has one: the Status Icons have their own Size (buffs.scale, buffwin.cpp) and the macro bar no place of its own.
 * Menu Content's windows (content_member, and a kContent one with its Position off) take menucontent.scale about its box's
 * corner, so they stay laid out as they were together; the Items list with inventory.move on, inventory.scale about its top
 * left. Two windows another module places are here too (g_extras: the cast bar, the target window), their boxes scaled
 * there (castbar.cpp, targetwin.cpp: scale). hudwin.cpp includes this inside its namespace after main_menu_list
 * (hudwin_up.h), whose name it uses. It is no file of its own. */
#pragma once

namespace {

// A window another module places, with an <id>.scale of its own here: its layout and its anchor point (its X and Y's)
struct Extra {
    const char* id = nullptr;
    char layout[16 + 1] = {};
    double anchor_x = 0, anchor_y = 0;
    double scale = 1;
};
Extra g_extras[] = {
    {"castbar", "menu    casttime", 0.5, 0}, // (castbar.cpp: X its middle, Y its top)
    {"target", "menu    targetwi", 0.5, 0},  // (targetwin.cpp: X its middle, Y its top)
};

double percent_scale(const std::wstring& id, const wchar_t* ini)
{
    return number(id + L".scale", 100, 50, 200, ini) / 100.0;
}

void scale_reload(const wchar_t* ini)
{
    for (auto& w : g_windows)
        w.scale = (w.can & kMove) != 0 ? percent_scale(std::wstring(w.id, w.id + std::strlen(w.id)), ini) : 1.0;
    for (auto& e : g_extras)
        e.scale = percent_scale(std::wstring(e.id, e.id + std::strlen(e.id)), ini);
}

// IsOnWindow (FUN_10118810, __thiscall window, point; ret 4): the point (x, y, int) inside the window's frame (+0x3A..+0x40),
// read live 2026-11-27: mov edx,[esp+4]; push esi; movsx esi,[ecx+3Ch]; mov eax,[edx+4]; cmp esi,eax; jg; movsx esi,[ecx+40h]
using OnWindowTest = char(__fastcall*)(void* window, void* edx, const int* point);
constexpr char kOnWindow[] = "8B 54 24 04 56 0F BF 71 3C 8B 42 04 3B F0 7F 20 0F BF 71 40 3B F0 7C 18 8B 02 0F BF 51 3A";
constexpr size_t kOnWindowPrologue = 5; // mov edx,[esp+4]; push esi
OnWindowTest g_on_window_game = nullptr;
bool g_scale_searched = false;

// the scaled windows as drawn lately, for the mouse: the window, its scale and anchor point (the interface's units)
struct ScaledSeen {
    const void* window = nullptr;
    double s = 1, ox = 0, oy = 0;
    unsigned long long ms = 0;
};
constexpr int kScaledSeen = 16;
constexpr unsigned long long kScaledFresh = 500; // a window not drawn for this long isn't mapped (closed, or scaled back)
ScaledSeen g_scaled_seen[kScaledSeen];

char __fastcall on_window_hook(void* window, void* edx, const int* point);

void scale_hooks()
{
    if (g_scale_searched)
        return;
    g_scale_searched = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t at = game != nullptr ? scan::find(game, kOnWindow) : 0;
    if (at == 0 || !imports::detour(reinterpret_cast<OnWindowTest>(at), kOnWindowPrologue, &on_window_hook, g_on_window_game))
        log::write(L"HUD windows: FFXI's test of the pointer over a window wasn't found in FFXiMain.dll: with <id>.scale, the "
                   L"pointer over a scaled window is taken where the game laid it out");
}

// The table's window that sets the scale of a window of `layout`: its own, the main menu for one that goes with it
// (`follows`), or none
const Window* scale_window(const char* layout, bool& follows)
{
    follows = false;
    for (const auto& w : g_windows)
        for (const char* name : w.layouts) {
            if (name[0] == 0 || std::memcmp(layout + kLayoutName, name, 16) != 0)
                continue;
            if ((w.can & kFollow) != 0 && !w.move) {
                follows = true;
                return window_named("mainmenu");
            }
            if ((w.can & kContent) != 0 && !w.move)
                return nullptr; // (Menu Content's while its Position is off: content_scale)
            return &w;
        }
    if (main_menu_list(layout)) {
        follows = true;
        return window_named("mainmenu");
    }
    return nullptr;
}

// Menu Content's scale for window `window` of `layout`, one of its windows, about its box's corner (where its first window
// goes: the game's 16, 48 in the area, or Menu Content's box with its Position on)
bool content_scale(const void* window, const char* layout, double& s, double& ox, double& oy)
{
    const Window* content = window_named("menucontent");
    if (content == nullptr || content->scale == 1.0 || g_manager == nullptr || !content_member(window, layout))
        return false;
    int area_left = 0, area_top = 0, area_w = 0, area_h = 0;
    if (!area(area_left, area_top, area_w, area_h))
        return false;
    int left = area_left + kItemsX, top = area_top + kItemsY;
    if (content->move && !wanted(*content, true, kContentW, kContentH, left, top))
        return false;
    s = content->scale, ox = left, oy = top;
    return true;
}

// Window `ctrl`'s scale (other than 1) and its anchor point, the interface's units
bool scale_of(const void* ctrl, double& s, double& ox, double& oy)
{
    if (ctrl == nullptr)
        return false;
    __try {
        const char* window = static_cast<const char*>(ctrl);
        const char* layout = *reinterpret_cast<const char* const*>(window + kWindowLayout);
        if (layout == nullptr)
            return false;
        const short* frame = reinterpret_cast<const short*>(window + kWinFrame);
        if (frame[2] <= frame[0] || frame[3] <= frame[1])
            return false;
        // about its anchor point, `ax` and `ay` of the way across it
        const auto about = [&](double scale, double ax, double ay) {
            s = scale;
            ox = frame[0] + ax * (frame[2] - frame[0]);
            oy = frame[1] + ay * (frame[3] - frame[1]);
            return scale != 1.0;
        };
        bool follows = false;
        if (const Window* w = scale_window(layout, follows))
            return about(w->scale, w->anchor_x, w->anchor_y); // (one that goes with the main menu: its anchor, the top right)
        const Window* items = window_named("inventory");
        if (items != nullptr && items->move && std::memcmp(layout + kLayoutName, kItemsName, 16) == 0)
            return about(items->scale, 0, 0);
        for (const auto& e : g_extras)
            if (std::memcmp(layout + kLayoutName, e.layout, 16) == 0)
                return about(e.scale, e.anchor_x, e.anchor_y);
        return content_scale(ctrl, layout, s, ox, oy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void scaled_note(const void* window, double s, double ox, double oy)
{
    const unsigned long long now = GetTickCount64();
    ScaledSeen* slot = &g_scaled_seen[0];
    for (auto& seen : g_scaled_seen) {
        if (seen.window == window) {
            slot = &seen;
            break;
        }
        if (seen.ms < slot->ms)
            slot = &seen; // (else the one drawn longest ago)
    }
    slot->window = window, slot->s = s, slot->ox = ox, slot->oy = oy, slot->ms = now;
}

char __fastcall on_window_hook(void* window, void* edx, const int* point)
{
    int mapped[2];
    if (map_point(window, point, mapped))
        return g_on_window_game(window, edx, mapped);
    return g_on_window_game(window, edx, point);
}

} // namespace

bool map_point(const void* window, const int* point, int mapped[2])
{
    if (window == nullptr || point == nullptr)
        return false;
    const unsigned long long now = GetTickCount64();
    for (const auto& seen : g_scaled_seen) {
        if (seen.window != window || now - seen.ms > kScaledFresh || !(seen.s > 0))
            continue;
        mapped[0] = static_cast<int>(std::lround(seen.ox + (point[0] - seen.ox) / seen.s));
        mapped[1] = static_cast<int>(std::lround(seen.oy + (point[1] - seen.oy) / seen.s));
        return true;
    }
    return false;
}

void preview(const std::string& key, double value)
{
    const double s = (std::min)((std::max)(value, 50.0), 200.0) / 100.0;
    for (auto& w : g_windows)
        if ((w.can & kMove) != 0 && key == std::string(w.id) + ".scale") {
            w.scale = s;
            return;
        }
    for (auto& e : g_extras)
        if (key == std::string(e.id) + ".scale") {
            e.scale = s;
            return;
        }
}

double scale(const char* id)
{
    for (const auto& e : g_extras)
        if (id != nullptr && std::strcmp(e.id, id) == 0)
            return e.scale;
    return 1.0;
}

Scaled::Scaled(const void* ctrl)
{
    double s = 1, ox = 0, oy = 0;
    if (!scale_of(ctrl, s, ox, oy))
        return;
    scale_hooks();
    // (the game's vertices lie half a unit up and left of its units)
    vanacore_gfx_window_scale(static_cast<float>(s), static_cast<float>(ox) - 0.5f, static_cast<float>(oy) - 0.5f);
    scaled_note(ctrl, s, ox, oy);
    on = true;
}

Scaled::~Scaled()
{
    if (on)
        vanacore_gfx_window_scale(0.0f, 0.0f, 0.0f);
}

namespace {

// A table window's box (outline) at its scale: `box` as laid out taken about its anchor point
void scale_box(const Window& w, outline::Box& box)
{
    if (w.scale == 1.0)
        return;
    const double width = box.right - box.left, height = box.bottom - box.top;
    const double ox = box.left + w.anchor_x * width, oy = box.top + w.anchor_y * height;
    box.left = static_cast<int>(std::lround(ox - w.anchor_x * width * w.scale));
    box.top = static_cast<int>(std::lround(oy - w.anchor_y * height * w.scale));
    box.right = box.left + static_cast<int>(std::lround(width * w.scale));
    box.bottom = box.top + static_cast<int>(std::lround(height * w.scale));
}

} // namespace
