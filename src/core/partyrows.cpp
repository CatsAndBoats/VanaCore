#include "partyrows.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "game.hpp"
#include "menuplace.h"

namespace vanacore::partyrows {
namespace {

// The lists' rows are their windows' buttons (found 2026-11-07 from the PS2 client's YkWndPartyList and YkWndPartyRaid1
// OnDrawPrimitive, and the PC's FUN_10202da3): row n is drawn at button n's place (GetButtonLocate, FUN_10118500, which
// finds the button by its id), and the cursor and the mouse go by those places too. A window's parts are a list at +0x14 (a
// node: +0 the next, +0x10 its part, +0x14 set to skip), a part's kind the short at +0 (0 the frame, 1 a button); a
// button's rect in the window at +0x32 (left, top, right, bottom) and its id at +0x3A, which the window's placing
// (ResizeWindow, FUN_1011a1a0, and SetLocate through it: FUN_101196d0 for each button) turns into its place at +0x2A, as
// buffwin.cpp lays out the status icons. The window's frame is +0x3A..+0x40 (left, top, right, bottom).
// The party list is as tall as its members: RestoreWindow (FUN_1011a4f0) resizes it to a layout for each count, leaving the
// buttons' rects as they are; the alliance lists are six rows tall whoever's in them. The frame part's +0x30 is the layout
// frame it was last made from (RestoreWindow's), whose data (its first pointer) has the game's height at +0xC: read only to
// tell the game's height from ours when the two are the same, and trusted only once it has been seen to be the game's.
constexpr std::ptrdiff_t kWinParts = 0x14, kWinFrame = 0x3A, kButtonRect = 0x32, kButtonId = 0x3A;
constexpr std::ptrdiff_t kFrameLayout = 0x30, kLayoutHeight = 0xC;
constexpr short kFrameKind = 0, kButtonKind = 1;
constexpr int kRows = 6, kPartsAtMost = 64, kMostSpacing = 40;

struct List {
    const char* id = nullptr;   // hudwin's
    char* window = nullptr;     // the window as last spaced
    short base[kRows][4] = {};  // its buttons' rects as the game made them
    short wrote[kRows][4] = {}; // as written here
    int game_h = 0;             // its height as the game made it
    int game_bottom = 0;        // its bottom where the game (or the player's Position) put it
    int set_top = 0, set_h = -1; // as made here
    bool layout_ok = false;     // the layout frame's height has been seen to be the game's
    bool spaced = false;        // made other than the game's (to go back when the settings are 0)
    // its Position as last seen, and how far it was lifted while that was off: hudwin.cpp keeps the place it last saw
    // while the Position was off (lifted then) and puts it back there when the Position is turned off again
    bool was_moved = false;
    int lift_unmoved = 0;
};
List g_lists[] = {{"party"}, {"alliance1"}, {"alliance2"}};
int g_party = 0, g_alliance = 0; // [client] party.spacing, alliance.spacing

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

// The window's frame part and its buttons 1-6 (null where there's none); false without a frame part
bool parts_of(char* window, char*& frame, char* (&buttons)[kRows])
{
    frame = nullptr;
    int count = 0;
    for (char* node = at<char*>(window, kWinParts); node != nullptr && count < kPartsAtMost; node = at<char*>(node, 0), ++count) {
        char* part = at<char*>(node, 0x10);
        if (part == nullptr || at<std::uint8_t>(node, 0x14) != 0)
            continue;
        const short kind = at<short>(part, 0);
        const int id = at<short>(part, kButtonId);
        if (kind == kFrameKind && frame == nullptr)
            frame = part;
        else if (kind == kButtonKind && id >= 1 && id <= kRows && buttons[id - 1] == nullptr)
            buttons[id - 1] = part;
    }
    return frame != nullptr;
}

// The height of the layout frame the window was last made from, 0 if there's none
int layout_height(char* frame)
{
    char* layout = at<char*>(frame, kFrameLayout);
    char* data = layout != nullptr ? at<char*>(layout, 0) : nullptr;
    return data != nullptr ? at<short>(data, kLayoutHeight) : 0;
}

// The game's height and bottom for the window as it is now (`top`, `height`): a size other than ours is the game's (a
// party list's count changed); a place other than ours is the game's or the Position's
void seen(List& list, bool fresh, int top, int height, int layout_h)
{
    if (fresh || height != list.set_h) {
        list.game_h = height;
        if (layout_h > 0)
            list.layout_ok = layout_h == height;
    } else if (list.layout_ok && layout_h > 0) {
        list.game_h = layout_h; // (made as tall as ours: its layout says what the game's is)
    }
    if (fresh || top != list.set_top || height != list.set_h)
        list.game_bottom = top + height;
}

// How far the list goes up for the lists under it: the alliance lists stand on a full party list (and the second on the
// first), each the taller for its spacing. None for one the player has put somewhere (its Position on: the HUD Manager's
// drag turns it on too). The second, with the first put elsewhere, stands on the first's empty place: up only as far as
// the party list's growth reaches past that (the first's height as the game makes it, six rows, seen live 2026-11-07 as
// the full party list's 134 units until the first has been seen)
constexpr int kSixRows = 134;
int lifted(const List& list, bool moved)
{
    if (moved || list.id == g_lists[0].id)
        return 0;
    const int under = (kRows - 1) * g_party;
    if (list.id == g_lists[1].id)
        return under;
    const List& first = g_lists[1];
    return first.was_moved ? std::max(0, under - (first.game_h > 0 ? first.game_h : kSixRows)) : under + (kRows - 1) * g_alliance;
}

// How many rows the list's frame holds: the party list's members (the game's count; it draws six at most), the alliance
// lists' six
int rows_of(const List& list)
{
    if (list.id != g_lists[0].id)
        return kRows;
    game::PartyMember members[18];
    game::Alliance alliance;
    if (!game::party(members, alliance))
        return kRows;
    return std::clamp(static_cast<int>(alliance.party_counts[0]), 1, kRows);
}

// The buttons' rects as the game made them (what's there, unless it's what was written here), then each `spacing` lower
// than the one before; true if any was written
// cppcheck-suppress constParameterReference ; its base and wrote are written, by memcpy
bool spread(List& list, char* (&buttons)[kRows], bool fresh, int spacing)
{
    bool moved = false;
    for (int i = 0; i < kRows; ++i) {
        if (buttons[i] == nullptr)
            continue;
        short* rect = &at<short>(buttons[i], kButtonRect);
        if (fresh || std::memcmp(rect, list.wrote[i], sizeof list.wrote[i]) != 0)
            std::memcpy(list.base[i], rect, sizeof list.base[i]);
        short want[4] = {list.base[i][0], static_cast<short>(list.base[i][1] + spacing * i), list.base[i][2],
                         static_cast<short>(list.base[i][3] + spacing * i)};
        std::memcpy(list.wrote[i], want, sizeof want);
        if (std::memcmp(rect, want, sizeof want) != 0) {
            std::memcpy(rect, want, sizeof want);
            moved = true;
        }
    }
    return moved;
}

void apply(List& list, char* window, Resize resize, bool moved)
{
    const int spacing = list.id == g_lists[0].id ? g_party : g_alliance;
    const int lift = lifted(list, moved);
    const bool back = list.was_moved && !moved; // its Position just turned off: put back where it was, lifted as it was then
    const int lift_then = list.lift_unmoved;
    list.was_moved = moved;
    if (!moved)
        list.lift_unmoved = lift;
    if (spacing == 0 && lift == 0 && !list.spaced)
        return; // the game's own: left alone
    char* frame = nullptr;
    char* buttons[kRows] = {};
    if (!parts_of(window, frame, buttons) || buttons[0] == nullptr)
        return;
    const short* win = &at<short>(window, kWinFrame);
    const int left = win[0], top = win[1], width = win[2] - win[0], height = win[3] - win[1];
    if (width <= 0 || height <= 0)
        return;
    const bool fresh = window != list.window || !list.spaced;
    list.window = window;
    seen(list, fresh, top, height, layout_height(frame));
    if (back)
        list.game_bottom = top + height + lift_then;
    const bool rects = spread(list, buttons, fresh, spacing);
    const int want_h = list.game_h + spacing * (rows_of(list) - 1);
    const int want_top = list.game_bottom - lift - want_h; // its bottom where it was, up by as much as it's lifted
    if (rects || want_h != height || want_top != top)
        resize(window, left, want_top, width, want_h, 1, 0, 0); // (the buttons placed from their rects again)
    list.set_top = want_top;
    list.set_h = want_h;
    list.spaced = spacing != 0 || lift != 0;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_party = static_cast<int>(menuplace::number(L"party.spacing", 0, 0, kMostSpacing, ini));
    g_alliance = static_cast<int>(menuplace::number(L"alliance.spacing", 0, 0, kMostSpacing, ini));
}

void space(const char* id, char* window, Resize resize, bool moved)
{
    if (window == nullptr || resize == nullptr)
        return;
    for (auto& list : g_lists) {
        if (std::strcmp(id, list.id) != 0)
            continue;
        __try {
            apply(list, window, resize, moved);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            list.window = nullptr;
        }
        return;
    }
}

} // namespace vanacore::partyrows
