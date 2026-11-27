/* [client] <id>.scale (the HUD Manager's Scale): a table window drawn bigger or smaller, about its anchor point (its top right
 * corner for the main menu, its bottom right for the party list), so its X and Y still say where that point is. The game
 * lays the window out as ever; the renderer maps its draw (vanacore_gfx_window_scale: its vertices, its clip and True Font's
 * letters) and what the game asks of the mouse about it is asked back at the place the game laid it out: IsOnWindow (the
 * pointer over a window: its keys, the lists' mouse blocks) and IsOnButton (chat.cpp's on_button_hook: the row or button
 * under the pointer). The main menu's lists that go with it (kFollow, their Position off) and the windows laid out in its
 * place that aren't the table's (its second page, Linkshell's...) take mainmenu.scale. hudwin.cpp includes this inside its
 * namespace after main_menu_list (hudwin_up.h), whose name it uses. It is no file of its own. */
#pragma once

namespace {

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
            return &w;
        }
    if (main_menu_list(layout)) {
        follows = true;
        return window_named("mainmenu");
    }
    return nullptr;
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
        bool follows = false;
        const Window* w = scale_window(layout, follows);
        if (w == nullptr || w->scale == 1.0)
            return false;
        const short* frame = reinterpret_cast<const short*>(window + kWinFrame);
        if (frame[2] <= frame[0] || frame[3] <= frame[1])
            return false;
        s = w->scale;
        ox = frame[0] + w->anchor_x * (frame[2] - frame[0]); // (one that goes with the main menu: its anchor, the top right)
        oy = frame[1] + w->anchor_y * (frame[3] - frame[1]);
        return true;
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
