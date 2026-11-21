// Included by equipstats.cpp inside its namespace (its helpers and data): the stats of the gear of someone you /check,
// in a box of their own under the game's Check window ([client] equip.stats.check), or moved anywhere (equip.stats.check.move,
// .x, .y: the HUD Manager's box 'equip.stats.check'). The same reading as your own gear's
// (item_mods: the server's data, or what an item's description says where CatsEyeXI has changed it since; augments the
// server's way; the sets' bonuses), without latent effects (theirs hang on that player's HP, TP, weather...) or the server's totals
// (/checkparam answers for yourself and your pet alone).

// The Check window ("menu    inspect ", CTkEquip's subclass, vtable 0x103317E0): its draw (FUN_10133a30, __thiscall on its
// handler, no arguments, read in the client dump 2026-11-06): each slot's item icon, then its owner's name and the rest.
// Its control at +8 as the Equipment window's; the checked items' ids at +78h, a short a slot (the slot the button's
// FUN_101311c0 gives). Its start: sub esp,8Ch; push ebx; mov ebx,ecx; push ebp; push esi; mov ebp,[ebx+8]; xor eax,eax;
// mov esi,1; four mov [esp+..],ax; cmp [ebp+58h],si; push edi; jl
constexpr char kCheckDraw[] = "81 EC 8C 00 00 00 53 8B D9 55 56 8B 6B 08 33 C0 BE 01 00 00 00 66 89 44 24 0C 66 89 44 24 0E "
                              "66 89 44 24 10 66 89 44 24 12 66 39 75 58 57 7C";
constexpr std::size_t kCheckPrologue = 6; // sub esp,8Ch
constexpr std::ptrdiff_t kCheckItems = 0x78;
constexpr int kSlots = 16;
constexpr std::size_t kExtra = 24;

bool g_check_on = true; // [client] equip.stats.check
EquipDraw g_check_draw = nullptr;

// .check.move: the stats in a box of their own, its left edge at .check.x and its top at .check.y (percents of the
// interface), drawn while the Check window is open; off, under the Check window (asked for 2026-11-08: detach and move it)
bool g_check_move = false;
constexpr double kCheckX = 11.0, kCheckY = 3.0; // (beside the Check window where the game opens it)
double g_check_x = kCheckX, g_check_y = kCheckY;
// Where the stats were last drawn, for the HUD Manager's box while the Check window is shut: their size, and their place
// under the Check window (attached)
int g_check_w = 182, g_check_h = 300;
outline::Box g_check_attached;
bool g_check_seen = false;

void check_reload(const wchar_t* ini)
{
    g_check_on = GetPrivateProfileIntW(L"client", L"equip.stats.check", 1, ini) != 0;
    g_check_move = GetPrivateProfileIntW(L"client", L"equip.stats.check.move", 0, ini) != 0;
    g_check_x = percent(L"equip.stats.check.x", kCheckX, ini);
    g_check_y = percent(L"equip.stats.check.y", kCheckY, ini);
}

// A box of the HUD Manager's (vana.layout.outline `id`): at `attached` (where the stats were last drawn on their window;
// null for none) while `move` is off, else at `x` and `y` as `w` by `h` (their own box's last size); Shift + drag moving it
// into a box of their own (`move` on, `x` and `y` as it goes, not written); the frame it's let go, moved_x and moved_y are
// where it was left, for the caller to save. False when it can't be placed.
struct Placing {
    bool& move;
    double& x;
    double& y;
    int w, h;
    const outline::Box* attached;
};
bool outline_box(const char* id, const Placing& p, double* moved_x, double* moved_y)
{
    outline::Box box;
    if (!p.move && p.attached != nullptr) {
        box = *p.attached;
    } else if (int left = 0, top = 0; box_place(p.x, p.y, p.w, p.h, left, top)) {
        box = {left, top, left + p.w, top + p.h};
    } else {
        return false;
    }
    outline::Drag drag;
    if (!outline::draw(id, box, 0, drag))
        return false;
    int area_left = 0, area_top = 0, area_right = 0, area_bottom = 0;
    if (drag.held && uispace::area(area_left, area_top, area_right, area_bottom) && area_right > area_left && area_bottom > area_top) {
        p.move = true;
        p.x = std::clamp(std::round((drag.left - area_left) * 10000.0 / (area_right - area_left)) / 100.0, 0.0, 100.0);
        p.y = std::clamp(std::round((drag.top - area_top) * 10000.0 / (area_bottom - area_top)) / 100.0, 0.0, 100.0);
    }
    if (drag.released && moved_x != nullptr && moved_y != nullptr) {
        *moved_x = p.x;
        *moved_y = p.y;
    }
    return true;
}

// The last /check's gear: the server's 0x0C9 in mode 3 (the target's id at +4, the mode at +0Ah, how many at +0Bh, then
// 28 bytes an item from +0Ch: its id, its slot, a byte, its extdata's 24 bytes; 8 at most a packet, so two for a full
// set), for the augments (the window has the items alone). Another target, or a /check a second or more later, starts it
// again. Written on the network's thread, read on the game's.
constexpr std::uint8_t kInspectGear = 3;
constexpr std::size_t kInspectItems = 0x0C, kInspectItem = 28;
constexpr ULONGLONG kInspectBatch = 1000;
struct Inspected {
    std::uint32_t target = 0;
    ULONGLONG at = 0;
    std::uint16_t item[kSlots] = {};
    std::uint8_t extra[kSlots][kExtra] = {};
};
std::mutex g_inspect_lock;
Inspected g_inspected;

void inspect_packet(const std::uint8_t* packet, std::size_t length)
{
    if (length < kInspectItems || packet[0x0A] != kInspectGear)
        return;
    const auto target = value_at<std::uint32_t>(packet + 4);
    const ULONGLONG now = GetTickCount64();
    const std::lock_guard lock(g_inspect_lock);
    if (target != g_inspected.target || now - g_inspected.at > kInspectBatch) {
        g_inspected = Inspected{};
        g_inspected.target = target;
    }
    g_inspected.at = now;
    const std::size_t count = std::min<std::size_t>(packet[0x0B], 8);
    for (std::size_t k = 0; k < count && kInspectItems + (k + 1) * kInspectItem <= length; ++k) {
        const std::uint8_t* item = packet + kInspectItems + k * kInspectItem;
        if (item[2] >= kSlots)
            continue;
        g_inspected.item[item[2]] = value_at<std::uint16_t>(item);
        std::memcpy(g_inspected.extra[item[2]], item + 4, kExtra);
    }
}

// The extdata the last /check gave item `id` in `slot` (or another slot with that item), none if it gave none
const std::uint8_t* inspected_extra(const Inspected& seen, int slot, std::uint16_t id)
{
    static const std::uint8_t kNone[kExtra] = {};
    if (seen.item[slot] == id)
        return seen.extra[slot];
    for (int s = 0; s < kSlots; ++s)
        if (seen.item[s] == id)
            return seen.extra[s];
    return kNone;
}

// What the checked gear adds to each stat, its sets' bonuses with it, as rows
void checked_cells(char* self, std::vector<Cell>& rows)
{
    Inspected seen;
    {
        const std::lock_guard lock(g_inspect_lock);
        seen = g_inspected;
    }
    std::vector<int> sums(g_stats.size(), 0);
    std::uint16_t worn[kSlots] = {};
    for (int slot = 0; slot < kSlots; ++slot) {
        const auto id = field<std::uint16_t>(self, kCheckItems + slot * 2);
        if (id == 0)
            continue;
        for (const Mod& mod : item_mods(id))
            add_mod(sums, mod.stat, mod.value);
        augment_sums(sums, id, inspected_extra(seen, slot, id));
        worn[slot] = id;
    }
    set_sums(sums, worn, kSlots);
    for (std::size_t i = 0; i < sums.size(); ++i)
        if (sums[i] != 0 && g_stats[i].shown)
            add(rows, g_stats[i].label.c_str(), sums[i], g_stats[i], true);
}

// The box's top-left corner: at .check.x and .check.y (equip.stats.check.move), else under the Check window `r` (over it
// when there's no room under it); false while the interface's area isn't known for a moved box
bool check_place(const Rect& r, int box_w, int box_h, int& left, int& top)
{
    if (g_check_move)
        return box_place(g_check_x, g_check_y, box_w, box_h, left, top);
    left = r.left, top = r.bottom + kUnderGap;
    int area_left = 0, area_top = 0, area_right = 0, area_bottom = 0;
    if (uispace::area(area_left, area_top, area_right, area_bottom) && top + box_h > area_bottom)
        top = std::max(area_top, r.top - kUnderGap - box_h);
    g_check_attached = {left, top, left + box_w, top + box_h};
    g_check_seen = true;
    return true;
}

// The rows in the game's window frame, under the Check window or in a box of their own (check_place), as wide as the
// window or wider for the longest row; nothing without the frame (bare text over the world can't be read)
void draw_check(char* ctrl, const std::vector<Cell>& rows)
{
    if (rows.empty() || g_frame == nullptr)
        return;
    const std::vector<Cell> none;
    const Rect r = rect_of(ctrl);
    // (in the Equipment window's stats' True Font while it's on, one font for both boxes; else the Check window's, the menus'.
    // Measured in the font its letters are drawn in: it was measured in Status & Numbers' and drawn in the menus')
    const bool own = chatfont::area_on(VANA_FONT_EQUIP_STATS);
    const int area = own ? vanacore_gfx_text_area(VANA_FONT_EQUIP_STATS) : -1;
    const int font = std::exchange(g_font, own ? VANA_FONT_EQUIP_STATS : VANA_FONT_MENUS);
    const int box_w = stats_width(none, rows, r.right - r.left);
    const int box_h = stats_height(none, rows);
    g_check_w = box_w, g_check_h = box_h;
    if (int left = 0, top = 0; check_place(r, box_w, box_h, left, top)) {
        vanacore_gfx_window_drawing(1);
        const Frame frame{box_w, box_h, 1};
        g_frame(&frame, left, top, 1.0f, 1.0f, kWhite, 0, 0);
        draw_block(rows, left, left + box_w, top, kMaxGearRows, false);
        vanacore_gfx_window_drawing(0);
    }
    g_font = font;
    if (area >= 0)
        vanacore_gfx_text_area(area);
}

void __fastcall check_hook(char* self, void*)
{
    g_check_draw(self);
    char* ctrl = self != nullptr ? field<char*>(self, kCtrlAt) : nullptr;
    if (!g_on || !g_check_on || ctrl == nullptr)
        return;
    int expected = kUnread;
    if (g_data.compare_exchange_strong(expected, kReading))
        std::thread(read_data).detach();
    if (g_data.load() != kRead)
        return;
    std::vector<Cell> rows;
    checked_cells(self, rows);
    draw_check(ctrl, rows);
}

void install_check_hook(HMODULE game)
{
    const std::uintptr_t at = scan::find(game, kCheckDraw);
    if (at == 0 || !imports::detour(reinterpret_cast<EquipDraw>(at), kCheckPrologue, reinterpret_cast<EquipDraw>(&check_hook), g_check_draw)) {
        log::write(L"Equipment stats: the Check window's drawing wasn't found in FFXiMain.dll or couldn't be hooked: no stats on /check");
        return;
    }
    log::write(L"Equipment stats: the Check window's drawing hooked at +0x%X", static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}
