#include "itemtips.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "chat.hpp"
#include "hudwin.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::itemtips {
namespace {

// An item's details (CTkItemInfo) keep what each row of their box shows as a table of row ids: three pages of 12 at +0x383
// (0xFF: no row), the page shown at +0x380. A row's id says what's drawn in it, on whatever page (the draw, 0x1013D170):
// 0x01 the name (the icon, and the type in the row after it, 0x00), 0x0A-0x1D the description's lines, 0x1E-0x27 the
// augments', 0x32-0x34 the job list's, 0x3C-0x3E the notes under it (0x46-0x48 the same with the page arrow beside them),
// 0x50 the page arrow, 0x00 nothing (the type's row, or padding to the box's least rows). The pages are built again each
// time the details are set (0x1013C610, from the set-item 0x1013C6E0, each frame a list's cursor is on an item): page 1 the
// name, type, description, jobs and notes as far as 12 rows go, page 2 the name again with the rest of the description and
// the augments (always there, never on page 1), page 3 what's left. Built as the game builds them, then put together
// (merge). The game's own table is put back before it builds them again: it doesn't clear what it doesn't write, so it
// carries on from its own. push esi; mov esi,ecx; mov eax,[esi+1Ch]; test eax,eax; jz; mov ecx,[language]; mov dl,[ecx+18h]
constexpr char kRebuild[] = "56 8B F1 8B 46 1C 85 C0 74 4F 8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0A 83 F8 02 74 09 83 F8 03 74 04 "
                            "85 C0 74 09 8B CE E8";
constexpr std::size_t kRebuildPrologue = 6;
// The page turn (0x1013BD60, __thiscall; the set-item's, on the key that turns it): push esi; mov esi,ecx; mov ecx,[global];
// call; test al,al; jnz; mov dl,[esi+380h]; inc dl. Two details side by side (an item and the one it's compared with) turn
// together, as many pages as the longer has (0x101F10D0).
constexpr char kFlip[] = "56 8B F1 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 8A 96 80 03 00 00 FE C2";
constexpr std::size_t kFlipPrologue = 9; // (the mov's operand is an address, not relative: it runs the same from anywhere)
// The draw of the page's rows (0x1013D170): push ecx; push esi; mov esi,ecx; xor eax,eax; mov al,[esi+380h]; lea eax,
// [eax+eax*2]; cmp byte ptr [esi+eax*4+383h],0FFh; jne. Its loop ends inc edi; cmp edi,0Ch (+0x115); jl. Each row's drawer
// takes the row and works out its place from it (the row times 16): no store of 12.
constexpr char kDraw[] = "51 56 8B F1 33 C0 8A 86 80 03 00 00 8D 04 40 80 BC 86 83 03 00 00 FF 75";
constexpr std::ptrdiff_t kDrawMost = 0x117;
// The count of the page's rows (0x1013C670), the set-item's for SetLines: xor eax,eax; xor edx,edx; mov dl,[ecx+380h]; ...
// inc eax; inc ecx; cmp eax,0Ch (+0x1B); jl; mov eax,0Ch (+0x20); ret
constexpr char kCount[] = "33 C0 33 D2 8A 91 80 03 00 00 8D 14 52 8D 8C 91 83 03 00 00 80 39 FF 74 0C 40 41 83 F8 0C 7C F4 B8 0C 00 00 "
                          "00 C3";
constexpr std::ptrdiff_t kCountMost[] = {0x1D, 0x21};
// The game's resize of a window (0x1011A1A0: __thiscall window, x, y, w, h, 1, 0, 0; itemlist.cpp's), the frame drawn to it
constexpr char kResize[] = "83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84";

constexpr std::ptrdiff_t kWindow = 8;    // the details: their window
constexpr std::ptrdiff_t kLines = 0x36C; // ... the rows their box was last laid out for (SetLines does nothing for the same)
constexpr std::ptrdiff_t kPage = 0x380;  // ... the page shown
constexpr std::ptrdiff_t kTable = 0x383; // ... the pages' rows
constexpr std::ptrdiff_t kRect = 0x3A;   // a window: left, top, right, bottom (16 bits each, the interface's pixels)
constexpr int kPageRows = 12, kRows = 36, kRowHeight = 16, kMostRows = 64;
constexpr std::uint8_t kNone = 0xFF, kName = 0x01, kBlank = 0x00;
constexpr std::uint8_t kFirstLine = 0x0A, kLastAugment = 0x27, kFirstJobs = 0x32, kLastJobs = 0x34;
constexpr std::uint8_t kFirstNote = 0x3C, kLastNote = 0x3E, kArrowNote = 0x46, kArrowNoteLast = 0x48;
constexpr int kKept = 4; // details kept track of at once (the game has two: an item's and the one it's compared with)

using Rebuild = void(__fastcall*)(char* self, void* edx);
using Flip = void(__fastcall*)(char* self, void* edx);
using Resize = void(__thiscall*)(void* window, int x, int y, int w, int h, char a, char b, char c);

bool g_on = true;        // [client] tips.extended
bool g_hide_empty = true; // [client] tips.empty.hide
bool g_searched = false; // FFXiMain.dll was looked through
bool g_hooked = false;   // ... and the page building and turn hooked
Rebuild g_rebuild = nullptr;
Flip g_flip = nullptr;
Resize g_resize = nullptr;
std::uint8_t* g_most[3] = {}; // the draw's and the count's 12 (all found, else none)
bool g_wide = false;          // ... made 36
unsigned g_merged = 0;

// The details' table as the game built it, and as it was put together, for each of them last put together
struct Kept {
    const char* self = nullptr;
    std::uint8_t game[kRows] = {};
    std::uint8_t merged[kRows] = {};
};
Kept g_kept[kKept];
int g_kept_next = 0;

// The details' box sized again (taller than the game's 12 rows, a True Font tooltip's pixels): its window, and its height
// as the game laid it out (for 12 past 12, else its rows; it lays one out only for a count it hasn't)
struct Grown {
    const char* self = nullptr;
    const char* window = nullptr;
    int base = 0, base_w = 0;
};
Grown g_grown[kKept];
int g_grown_next = 0;

template <typename T>
T read(const char* at)
{
    T value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

template <typename T>
void write(char* at, T value)
{
    std::memcpy(at, &value, sizeof(value));
}

template <typename Entry>
Entry* find(Entry (&entries)[kKept], const char* self)
{
    for (Entry& entry : entries)
        if (entry.self == self)
            return &entry;
    return nullptr;
}

template <typename Entry>
Entry* make(Entry (&entries)[kKept], int& next, const char* self)
{
    Entry* entry = &entries[next];
    next = (next + 1) % kKept;
    *entry = Entry{};
    entry->self = self;
    return entry;
}

// The most rows one page can take: 36 with the draw's and the count's limits widened and the box able to grow, else 12
int most_rows()
{
    return g_wide && g_resize != nullptr ? kRows : kPageRows;
}

// The details' pages put together into one (`out`: 36 rows, 0xFF after the last), in order: the name (and the type's row),
// the description, the augments, the job list, the notes. False when there's one page already, or the rows won't fit.
// The box is kept no shorter than the game's first page (its padding to the least rows).
bool merge(const std::uint8_t* table, std::uint8_t* out)
{
    if (table[0] != kName || (table[kPageRows] == kNone && table[2 * kPageRows] == kNone))
        return false;
    int first = 0;
    while (first < kPageRows && table[first] != kNone)
        ++first;
    bool has[256] = {};
    for (int page = 0; page < kRows; page += kPageRows)
        for (int row = 0; row < kPageRows && table[page + row] != kNone; ++row) {
            std::uint8_t id = table[page + row];
            if (id >= kArrowNote && id <= kArrowNoteLast)
                id = static_cast<std::uint8_t>(id - (kArrowNote - kFirstNote)); // (no arrow: there's no page to turn to)
            has[id] = true;
        }
    int n = 0;
    out[n++] = kName;
    if (first > 1 && table[1] == kBlank)
        out[n++] = kBlank; // the type's row
    const auto take = [&](int from, int to) {
        for (int id = from; id <= to; ++id)
            if (has[id] && n < kRows)
                out[n++] = static_cast<std::uint8_t>(id);
    };
    take(kFirstLine, kLastAugment); // the description's lines, then the augments'
    take(kFirstJobs, kLastJobs);
    take(kFirstNote, kLastNote);
    if (n > most_rows())
        return false;
    while (n < first)
        out[n++] = kBlank;
    std::memset(out + n, kNone, kRows - n);
    return true;
}

void __fastcall rebuild_hook(char* self, void* edx)
{
    auto* table = reinterpret_cast<std::uint8_t*>(self + kTable);
    Kept* kept = find(g_kept, self);
    if (kept != nullptr && std::memcmp(table, kept->merged, kRows) == 0)
        std::memcpy(table, kept->game, kRows); // the game's own, to build on (set again for a new item: not ours then)
    g_rebuild(self, edx);
    std::uint8_t out[kRows];
    if (!g_on || !merge(table, out)) {
        if (kept != nullptr)
            kept->self = nullptr;
        return;
    }
    if (kept == nullptr)
        kept = make(g_kept, g_kept_next, self);
    std::memcpy(kept->game, table, kRows);
    std::memcpy(table, out, kRows);
    std::memcpy(kept->merged, out, kRows);
    self[kPage] = 0; // (the build turns to the augments' page when asked: they're on the first now)
    if (++g_merged == 1)
        log::write(L"Item details: an item's pages put together into one ([client] tips.extended=1)");
}

// Not turned while it's on and the details are one page (put together, or one to start with): an item and the one it's
// compared with turn together, and the shorter showed its name alone on a page it hasn't. Pages too long to put together
// turn as the game's.
void __fastcall flip_hook(char* self, void* edx)
{
    const auto* table = reinterpret_cast<const std::uint8_t*>(self + kTable);
    const Kept* kept = find(g_kept, self);
    const bool merged = kept != nullptr && std::memcmp(table, kept->merged, kRows) == 0;
    if (g_on && (merged || (table[kPageRows] == kNone && table[2 * kPageRows] == kNone)))
        return;
    g_flip(self, edx);
}

bool write_code(std::uint8_t* at, std::uint8_t value)
{
    DWORD protect = 0;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    *at = value;
    VirtualProtect(at, 1, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    return true;
}

// The rows drawn and counted on a page: 36 (the whole table, which is then one page) or the game's 12
void widen(bool wide)
{
    if (wide == g_wide || g_most[0] == nullptr)
        return;
    for (std::uint8_t* at : g_most)
        write_code(at, static_cast<std::uint8_t>(wide ? kRows : kPageRows));
    g_wide = wide;
}

void find_and_hook()
{
    if (g_searched)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const std::uintptr_t rebuild = scan::find(game, kRebuild);
    const std::uintptr_t flip = scan::find(game, kFlip);
    const std::uintptr_t draw = scan::find(game, kDraw);
    const std::uintptr_t count = scan::find(game, kCount);
    g_resize = reinterpret_cast<Resize>(scan::find(game, kResize));
    if (rebuild == 0 || flip == 0) {
        log::write(L"Item details: their page building wasn't found in FFXiMain.dll: [client] tips.extended does nothing");
        return;
    }
    if (!imports::detour(reinterpret_cast<Rebuild>(rebuild), kRebuildPrologue, &rebuild_hook, g_rebuild) ||
        !imports::detour(reinterpret_cast<Flip>(flip), kFlipPrologue, &flip_hook, g_flip)) {
        log::write(L"Item details: their page building couldn't be hooked: [client] tips.extended does nothing");
        return;
    }
    g_hooked = true;
    std::uint8_t* most[3] = {draw != 0 ? reinterpret_cast<std::uint8_t*>(draw + kDrawMost) : nullptr,
                             count != 0 ? reinterpret_cast<std::uint8_t*>(count + kCountMost[0]) : nullptr,
                             count != 0 ? reinterpret_cast<std::uint8_t*>(count + kCountMost[1]) : nullptr};
    const bool limits = std::all_of(std::begin(most), std::end(most), [](const std::uint8_t* at) { return at != nullptr && *at == kPageRows; });
    if (limits)
        std::copy(std::begin(most), std::end(most), std::begin(g_most));
    log::write(L"Item details: their pages hooked (+0x%X, turn +0x%X)%s", static_cast<unsigned>(rebuild - base),
               static_cast<unsigned>(flip - base),
               limits && g_resize != nullptr ? L"" : L"; their 12-row limit or the resize wasn't found: a page past 12 rows stays the game's");
}

} // namespace

namespace {

// The box `window` back to the size the game laid it (`grown`'s), before the game is asked to lay it out again: it skips a
// layout its window already has (0x1011A4F0 compares each element's record), so the size it left was the one made here, and
// the box grew by `extra` each time (an item at the auction house: Price history makes its details again, 4 times 36 pixels
// on a crystal's 3 rows, seen live 2026-11-06)
void restore(const Grown* grown, char* window)
{
    if (grown == nullptr || window == nullptr || grown->window != window || g_resize == nullptr || grown->base <= 0)
        return;
    const short left = read<short>(window + kRect), top = read<short>(window + kRect + 2);
    const short right = read<short>(window + kRect + 4), bottom = read<short>(window + kRect + 6);
    if (right - left != grown->base_w || bottom - top != grown->base)
        g_resize(window, left, top, grown->base_w, grown->base, 1, 0, 0);
}

// The windows the game lays out under the details' box as it makes them, the box at most 12 rows tall: the auction
// house's Price Set for a bid puts the details at 16, 48 (side 2, area 0x50; 16, 48 - 382, 248 at 12 rows) and the gil
// window ("menu    money") at 16, 240 - 128, 296 and the price ("menu    moneyctr") at 130, 240 - 496, 296, from their
// layouts, 8 into the 12-row box's frame (read live 2026-11-10). A box made taller covered them (reported 2026-11-10:
// Grape daifuku's 14 rows, 48 - 280, over the price). Each open window whose top is at the game's bottom of a box made
// taller, across it, goes down to 2 under the box (the game's own 8 into its frame put the box's last line on the gil
// window's title, 2026-11-10: "doesn't feel like it's at the bottom"), by its own SetLocate; one moved here already
// is moved by the difference, back up for none; the game putting one back is seen and it's moved again. Not the HUD
// Manager's windows (placed by their own rules), nor the details, nor the chat logs: a log whose top was near a taller
// box's game bottom (a tall log, a large interface) went down under it while the cursor was on an item in the inventory
// (Grape Daifuku's 14 rows; reported 2026-11-22, gone with tips.extended off), and chat.move put it back each frame
constexpr std::ptrdiff_t kWinState = 0x10, kWinX = 0x52, kWinY = 0x54; // a window: its state, its place (as SetLocate put it)
constexpr std::uint32_t kClosed = 0xE;
constexpr int kUnderAbove = 12, kUnderBelow = 8; // a top this far into or under the game's bottom of the box: under it
constexpr int kUnderGap = 2; // ... and put this far under the taller box (the game's details 2 under the help bar, 46 - 48)
constexpr int kLoweredMost = 8, kNodesMost = 256;
constexpr std::ptrdiff_t kNodeWindow = 0x10; // the menu manager's list (its +0): a node's next at +0, its window

struct Lowered {
    char* window = nullptr;
    short top = 0;
    int by = 0;
};
Lowered g_lowered[kLoweredMost];
int g_lowered_count = 0;

// A box made taller than the game's: across, its bottom as the game laid it, where the windows under it go
struct Box {
    int left = 0, right = 0, bottom = 0, below = 0;
};

int grown_boxes(Box (&out)[kKept])
{
    int n = 0;
    for (const Grown& grown : g_grown) {
        if (grown.self == nullptr || grown.window == nullptr || grown.base <= 0 || read<const char*>(grown.self + kWindow) != grown.window ||
            read<std::uint32_t>(grown.window + kWinState) == kClosed)
            continue;
        const short left = read<short>(grown.window + kRect), top = read<short>(grown.window + kRect + 2);
        const short right = read<short>(grown.window + kRect + 4), bottom = read<short>(grown.window + kRect + 6);
        if (bottom - top > grown.base)
            out[n++] = {left, right, top + grown.base, bottom + kUnderGap};
    }
    return n;
}

bool details_window(const char* window)
{
    for (const Grown& grown : g_grown)
        if (grown.self != nullptr && window == grown.window)
            return true;
    return false;
}

// How far `window`, its top at `top` where the game put it, goes down: under the box it's under, 0 under none
int under(const char* window, int top, const Box* boxes, int n)
{
    const short left = read<short>(window + kRect), right = read<short>(window + kRect + 4);
    int by = 0;
    for (int i = 0; i < n; ++i)
        if (right > left && left < boxes[i].right && right > boxes[i].left && top >= boxes[i].bottom - kUnderAbove &&
            top <= boxes[i].bottom + kUnderBelow)
            by = std::max(by, boxes[i].below - top);
    return by;
}

int lowered_by(const char* window)
{
    for (int i = 0; i < g_lowered_count; ++i)
        if (g_lowered[i].window == window && read<short>(window + kRect + 2) == g_lowered[i].top)
            return g_lowered[i].by;
    return 0;
}

void lower_walk(const char* manager, const Box* boxes, int n)
{
    Lowered kept[kLoweredMost];
    int count = 0, nodes = 0;
    for (const char* node = read<char*>(manager); node != nullptr && nodes < kNodesMost; node = read<char*>(node), ++nodes) {
        char* window = read<char*>(node + kNodeWindow);
        if (window == nullptr || read<std::uint32_t>(window + kWinState) == kClosed || details_window(window) || hudwin::own(window) ||
            chat::log_window(window))
            continue;
        const int was = lowered_by(window);
        const int by = under(window, read<short>(window + kRect + 2) - was, boxes, n);
        if (by != was)
            hudwin::place(window, read<short>(window + kWinX), read<short>(window + kWinY) + by - was);
        if (by != 0 && count < kLoweredMost)
            kept[count++] = {window, read<short>(window + kRect + 2), by};
    }
    std::copy(kept, kept + count, g_lowered);
    g_lowered_count = count;
}

void lower()
{
    const char* manager = hudwin::manager();
    __try {
        Box boxes[kKept];
        const int n = grown_boxes(boxes);
        if (manager != nullptr && (n != 0 || g_lowered_count != 0))
            lower_walk(manager, boxes, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lowered_count = 0; // (a window torn down while its node is still linked: the walk stops there)
    }
}

} // namespace

void set_lines(void* details, const Size& size, SetLines original)
{
    char* self = static_cast<char*>(details);
    Grown* grown = find(g_grown, self);
    const int lines = size.lines, extra = size.extra;
    const bool taller = g_on && lines > kPageRows;
    if (g_resize == nullptr || self == nullptr || lines <= 0 || (!taller && extra == 0 && size.wide == nullptr)) {
        if (size.again)
            return;
        if (grown != nullptr && self != nullptr) {
            restore(grown, read<char*>(self + kWindow));
            write<int>(self + kLines, 0); // laid out from the game's layout again (it lays one out only for a count it hasn't)
            grown->self = nullptr;
        }
        const bool whole = g_resize == nullptr && lines > 0 && extra != 0; // (no resize: the pixels as whole rows)
        original(self, nullptr, whole ? static_cast<int>(std::ceil((lines * kRowHeight + extra) / 16.0f - 0.25f)) : lines);
        return;
    }
    const int laid = taller ? kPageRows : std::clamp(lines, 3, kPageRows); // the rows the game lays the box out for
    char* window = read<char*>(self + kWindow);
    if (grown == nullptr || grown->window != window || read<int>(self + kLines) != laid) {
        if (size.again)
            return;
        restore(grown, window);
        write<int>(self + kLines, 0);
        original(self, nullptr, laid); // the game's box, sized again
        window = read<char*>(self + kWindow);
        if (window == nullptr)
            return;
        if (grown == nullptr)
            grown = make(g_grown, g_grown_next, self);
        grown->window = window;
        grown->base = read<short>(window + kRect + 6) - read<short>(window + kRect + 2);
        grown->base_w = read<short>(window + kRect + 4) - read<short>(window + kRect);
    }
    const short left = read<short>(window + kRect), top = read<short>(window + kRect + 2);
    const short right = read<short>(window + kRect + 4), bottom = read<short>(window + kRect + 6);
    const int below = taller ? (std::min(lines, kMostRows) - kPageRows) * kRowHeight : 0;
    const int high = std::max(grown->base + below + extra, kRowHeight);
    const int asked = size.wide != nullptr ? size.wide(left, top, left + grown->base_w, top + high) : 0;
    const int wide = std::max(asked, grown->base_w);
    if (bottom - top != high || right - left != wide) {
        g_resize(window, left, top, wide, high, 1, 0, 0);
        lower(); // (the windows under it lowered before they're drawn, not a frame later)
    }
}

namespace {

constexpr std::ptrdiff_t kCtrlObject = 0xC; // a window: the object it belongs to (itemrarity.cpp's)

// The details window `ctrl` shows no item (its details' item, +0x1C, none)
bool no_item(const char* ctrl)
{
    constexpr std::ptrdiff_t kItem = 0x1C;
    const char* details = read<const char*>(ctrl + kCtrlObject);
    return details != nullptr && read<const void*>(details + kItem) == nullptr;
}

// The comment box ("menu    comment ": a checked player's bazaar message under the Check window, a search result's
// comment, ...) belongs to one object (the global at 0x1062E7E4, its window at +8). Its draw (0x101DC410) draws the 3
// lines of its kind (+0x16, 0 to 3), each 41 bytes from +0x1A ((kind * 3 + line) * 41), kind 2 a picture after them, and
// the line being typed (+0x14) from its edit field (+0x378) while there's one. The Check window opens it whenever the
// cursor is on View Wares (0x1013347A), the player's message or not: with none, the lines are all spaces (seen live
// 2026-11-08, an empty box under the Check window).
bool no_comment(const char* ctrl)
{
    constexpr char kName[] = "menu    comment ";
    constexpr std::ptrdiff_t kLayout = 4, kLayoutName = 0x46, kOwnWindow = 8;
    constexpr std::ptrdiff_t kKind = 0x16, kText = 0x1A, kEdit = 0x378, kLineBytes = 41;
    constexpr int kLines = 3, kPictureKind = 2, kLastKind = 3;
    const char* layout = read<const char*>(ctrl + kLayout);
    if (layout == nullptr || std::memcmp(layout + kLayoutName, kName, sizeof(kName) - 1) != 0)
        return false;
    const char* box = read<const char*>(ctrl + kCtrlObject);
    if (box == nullptr || read<const char*>(box + kOwnWindow) != ctrl || read<const void*>(box + kEdit) != nullptr)
        return false;
    const int kind = read<std::int8_t>(box + kKind);
    if (kind < 0 || kind > kLastKind || kind == kPictureKind)
        return false;
    for (int line = 0; line < kLines; ++line) {
        const char* text = box + kText + (kind * kLines + line) * kLineBytes;
        for (int i = 0; i < kLineBytes && text[i] != 0; ++i)
            if (text[i] != ' ')
                return false;
    }
    return true;
}

} // namespace

bool empty(const void* ctrl, bool details)
{
    if (!g_hide_empty || ctrl == nullptr)
        return false;
    __try {
        const char* window = static_cast<const char*>(ctrl);
        return details ? no_item(window) : no_comment(window);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false; // (another window's object: not read)
    }
}

int widened(const void* details)
{
    const char* self = static_cast<const char*>(details);
    const Grown* grown = self != nullptr ? find(g_grown, self) : nullptr;
    const char* window = grown != nullptr ? read<const char*>(self + kWindow) : nullptr;
    if (window == nullptr || window != grown->window)
        return 0;
    return std::max(read<short>(window + kRect + 4) - read<short>(window + kRect) - grown->base_w, 0);
}

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"tips.extended", 1, ini) != 0;
    g_hide_empty = GetPrivateProfileIntW(L"client", L"tips.empty.hide", 1, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_on && !g_searched)
        find_and_hook();
    widen(g_on && g_hooked);
    lower();
}

int lowered(const void* window)
{
    __try {
        return window != nullptr ? lowered_by(static_cast<const char*>(window)) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

} // namespace vanacore::itemtips
