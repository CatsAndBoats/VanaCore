#include "equipfirst.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::equipfirst {
namespace {

// The equip sets' options box (FFXiMain 2025-12-26, read live 2026-11-09): the menu DAT's layout "menu    mcresmn", made
// by 0x101A5A40 beside the sets' list at the chosen row. Its payload (singlemenu.cpp's notes on a layout) is the window's
// block, items 1 to 8, each 88 x 16 at x 16 and y 6 to 118, sixteen apart in id order (1 Edit, 2 Change Name, 3 Copy,
// 4 Paste, 5 Delete, 6 Use Current, 7 Equip, 8 Style Lock), each linked up and down to the next round the box, then 9 the
// title tab (no links). What a button does goes by its id and the cursor by the links, so Equip is put first by the
// buttons' places and links alone: the ids and the blocks' order stay (the game takes an item by its block's place,
// singlemenu.cpp). The game's own payload is changed, and a box takes it up as it opens: patched while one was open, that
// one stayed as it was until opened again. Tried by hand in the game first (Equip at the top, the cursor going down
// Edit ... Style Lock and round, each button doing what it did; SetCursol on 7 put the cursor on Equip, where Edit is the
// game's first). "can we make it so it's the default selected? atm edit is the default" (2026-11-09)

// The window that has the help line's text (FUN_101182a0): the menu manager at +7 (singlemenu.cpp's)
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8";
constexpr std::ptrdiff_t kHelpManagerAt = 7;
// SetCursol (0x101189B0, thiscall: the button, 0; ret 8): gamewin.cpp's
constexpr char kSetCursor[] = "83 EC 08 56 57 8B 7C 24 14 8B F1 66 85 FF 7F 05 BF 01 00 00 00 66 8B 46 58 66 3B F8 7E 02 8B F8 66 8B 46 4C";
using SetCursor = void(__fastcall*)(void* window, void* edx, int item, int flag);
// The box opened (0x101A5A40, thiscall on its handler, whose +8 is the box's window; no arguments, ret): run as the game
// makes the box, before its first draw, it puts the box beside the chosen row (0x10118D10) and greys out Paste (4) with
// nothing copied and Style Lock (8) where it can't be used, and leaves the cursor where the window starts it, on button 1
// (disassembled live 2026-11-09). The cursor was put on Equip at the frame's end at first, and Edit showed for that one
// frame ("when I first open the sub menu it's on edit for like 1 frame"): it's put there as the box opens now. mov
// eax,[<the sets' list>]; sub esp,10h; push ebx; xor ebx,ebx; push edi; cmp eax,ebx; mov edi,ecx; je; mov ecx,[edi+8]; push
// ebp; lea eax,[esp+0Ch]; push esi; push eax. Its first instruction (5 bytes, an absolute address) runs the same anywhere.
constexpr char kOpened[] = "A1 ?? ?? ?? ?? 83 EC 10 53 33 DB 57 3B C3 8B F9 0F 84 ?? ?? ?? ?? 8B 4F 08 55 8D 44 24 0C 56 50";
constexpr std::size_t kOpenedPrologue = 5;
constexpr std::ptrdiff_t kHandlerWindow = 8;
using Opened = void(__fastcall*)(void* handler, void* edx);

// The manager's lists, a node, a layout, a window (configmenu.cpp's; a window's state hudwin.cpp's)
constexpr std::ptrdiff_t kWindows = 0, kLayouts = 0x1C;
constexpr std::ptrdiff_t kNodeNext = 0, kNodeItem = 0x10, kNodeSkip = 0x14;
constexpr std::ptrdiff_t kLayoutParse = 8, kLayoutName = 0x46;
constexpr std::ptrdiff_t kWindowLayout = 4, kWindowState = 0x10;
constexpr std::uint32_t kClosing = 0xE;
constexpr int kMaxNodes = 65536;

// A layout's payload: the header (its name, 1, the count of blocks after the window's), then the blocks
constexpr std::size_t kHeader = 0x20, kCountAt = 0x11;
constexpr std::size_t kBlockX = 2, kBlockY = 4, kBlockWidth = 0xA, kBlockHeight = 0xC, kItemId = 0x12, kItemLinks = 0x15;
constexpr int kLinks = 6;
constexpr char kName[17] = "menu    mcresmn ";
constexpr int kItems = 8, kBlocks = 9, kTitle = 9, kEquip = 7;
constexpr int kItemX = 16, kItemWidth = 88, kItemHeight = 16, kTop = 6;
constexpr int kGameOrder[kItems] = {1, 2, 3, 4, 5, 6, 7, 8};
constexpr int kEquipOrder[kItems] = {7, 1, 2, 3, 4, 5, 6, 8};

bool g_on = false;       // [client] macrooptions.equipfirst
bool g_laid = false;     // the layout has Equip first
bool g_searched = false; // FFXiMain.dll looked at
bool g_found = false;
bool g_failed = false; // the box isn't the one known: left alone from then on
ULONGLONG g_next = 0;
char* g_manager = nullptr;
SetCursor g_set_cursor = nullptr;
Opened g_opened = nullptr; // the game's, through the hook (nullptr: not hooked, the cursor put at the frame's end)
const char* g_layout = nullptr; // the box's layout, as found last
const char* g_seen = nullptr;   // the box open last frame (its cursor put on Equip as it opened)

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

// A block's 16-bit field (its size, place, size on screen, id: every one this box has is under 0x8000), and one set
int field(const std::uint8_t* p)
{
    std::int16_t value = 0;
    std::memcpy(&value, p, sizeof value);
    return value;
}

void set_field(std::uint8_t* p, int value)
{
    const auto narrow = static_cast<std::int16_t>(value);
    std::memcpy(p, &narrow, sizeof narrow);
}

// How long a layout's payload is: its DAT chunk's length, from the chunk's header just before it (configmenu.cpp's). 0 if
// it isn't a layout chunk.
std::size_t room_of(const std::uint8_t* payload)
{
    const std::uint32_t info = *reinterpret_cast<const std::uint32_t*>(payload - 0xC);
    const std::size_t length = ((info >> 7) & 0x7FFFF) * 16;
    return (info & 0x7F) != 0x30 || length <= 16 ? 0 : length - 16;
}

// The first layout of the box's name on the manager's list (the one the game opens), or nullptr
char* box_layout()
{
    int count = 0;
    for (char* node = at<char*>(g_manager, kLayouts); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* layout = at<std::uint8_t>(node, kNodeSkip) == 0 ? at<char*>(node, kNodeItem) : nullptr;
        if (layout != nullptr && std::memcmp(layout + kLayoutName, kName, 16) == 0)
            return layout;
    }
    return nullptr;
}

// The open (not closing) window made from `layout`, or nullptr
char* box_open(const char* layout)
{
    int count = 0;
    for (char* node = at<char*>(g_manager, kWindows); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* window = at<char*>(node, kNodeItem);
        if (window != nullptr && at<char*>(window, kWindowLayout) == layout && at<std::uint32_t>(window, kWindowState) != kClosing)
            return window;
    }
    return nullptr;
}

// The payload's blocks (block[id] the offset of item id's; 0 the window's) if it's the box known: its header, nine blocks
// after the window's with ids 1 to 9 in order, the buttons 88 x 16 down one left edge on the eight rows 6 to 118 in any
// order, the title tab with no links. Else what isn't, for the log.
const wchar_t* read_box(const std::uint8_t* p, std::size_t (&block)[kBlocks + 1])
{
    const std::size_t room = room_of(p);
    if (room < kHeader || std::memcmp(p, kName, 16) != 0 || p[0x10] != 1 || p[kCountAt] != kBlocks)
        return L"its header";
    std::size_t offset = kHeader;
    for (int b = 0; b <= kBlocks; ++b) {
        const std::size_t size = offset + 2 <= room ? static_cast<std::size_t>(field(p + offset)) : 0;
        if (size < 0x20 || size % 16 != 0 || offset + size > room || (b != 0 && field(p + offset + kItemId) != b))
            return L"its blocks";
        block[b] = offset;
        offset += size;
    }
    unsigned rows = 0;
    for (int id = 1; id <= kItems; ++id) {
        const std::uint8_t* item = p + block[id];
        const int row = (field(item + kBlockY) - kTop) / kItemHeight;
        if (field(item + kBlockX) != kItemX || field(item + kBlockWidth) != kItemWidth || field(item + kBlockHeight) != kItemHeight ||
            field(item + kBlockY) != kTop + row * kItemHeight || row < 0 || row >= kItems)
            return L"its buttons";
        rows |= 1u << row;
    }
    for (int link = 0; link < kLinks; ++link)
        if (p[block[kTitle] + kItemLinks + link] != 0xFF)
            return L"its title";
    return rows == (1u << kItems) - 1 ? nullptr : L"its rows";
}

// The box's buttons laid top to bottom in `order`: each on its row, linked up and down to the ones beside it round the box
// (before and after the same), Right and Left to itself, as the game's are
void lay(std::uint8_t* p, const std::size_t (&block)[kBlocks + 1], const int (&order)[kItems])
{
    for (int i = 0; i < kItems; ++i) {
        std::uint8_t* item = p + block[order[i]];
        const auto before = static_cast<std::uint8_t>(order[(i + kItems - 1) % kItems]);
        const auto after = static_cast<std::uint8_t>(order[(i + 1) % kItems]);
        const auto self = static_cast<std::uint8_t>(order[i]);
        const std::uint8_t links[kLinks] = {before, after, before, after, self, self};
        set_field(item + kBlockY, kTop + i * kItemHeight);
        std::memcpy(item + kItemLinks, links, sizeof links);
    }
}

// The box's layout laid as the setting wants, while no box is open: the game loaded it with its own order, and loads it
// again after letting its menus go (logging out), so this is done whenever the order isn't the one wanted
void apply()
{
    char* layout = box_layout();
    g_layout = layout;
    if (layout == nullptr || box_open(layout) != nullptr)
        return;
    char* parse = at<char*>(layout, kLayoutParse);
    auto* first = parse != nullptr ? at<std::uint8_t*>(parse, 0) : nullptr;
    if (first == nullptr)
        return;
    std::uint8_t* p = first - kHeader;
    std::size_t block[kBlocks + 1] = {};
    if (const wchar_t* what = read_box(p, block)) {
        log::write(L"Equip first: the equip sets' options box isn't the one VanaCore knows (%ls): it's left as it is", what);
        g_failed = true;
        return;
    }
    lay(p, block, g_on ? kEquipOrder : kGameOrder);
    g_laid = g_on;
}

// The box opened (kOpened): the game's, then with Equip first the cursor on Equip, before the box is first drawn
void __fastcall opened_hook(void* handler, void* edx)
{
    g_opened(handler, edx);
    char* window = handler != nullptr ? at<char*>(handler, kHandlerWindow) : nullptr;
    if (g_laid && window != nullptr && at<char*>(window, kWindowLayout) == g_layout) {
        g_set_cursor(window, nullptr, kEquip, 0);
        g_seen = window;
    }
}

// Without the hook: a box just opened with Equip first has its cursor put on Equip at the frame's end (the game starts it
// on the first button, Edit, which shows for that frame)
void put_cursor()
{
    char* window = g_laid && g_layout != nullptr ? box_open(g_layout) : nullptr;
    if (window != nullptr && window != g_seen)
        g_set_cursor(window, nullptr, kEquip, 0);
    g_seen = window;
}

bool found()
{
    if (g_searched)
        return g_found;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::uintptr_t help = scan::find(game, kHelp);
    const std::uintptr_t set_cursor = scan::find(game, kSetCursor);
    const std::uintptr_t manager = help != 0 ? scan::operand(help, kHelpManagerAt) : 0;
    if (set_cursor == 0 || manager < base || manager >= base + nt->OptionalHeader.SizeOfImage) {
        log::write(L"Equip first: %ls isn't as VanaCore knows it in FFXiMain.dll: the equip sets' options box keeps the game's order",
                   set_cursor == 0 ? L"SetCursol" : L"the menu manager");
        return false;
    }
    g_manager = reinterpret_cast<char*>(manager);
    g_set_cursor = reinterpret_cast<SetCursor>(set_cursor);
    const std::uintptr_t opened = scan::find(game, kOpened);
    if (opened == 0 || !imports::detour(reinterpret_cast<Opened>(opened), kOpenedPrologue, &opened_hook, g_opened))
        log::write(L"Equip first: the box's opening wasn't found in FFXiMain.dll: its cursor is put on Equip a frame after it opens");
    g_found = true;
    return true;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"macrooptions.equipfirst", 0, ini) != 0; // taken up by on_frame
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_failed || (!g_on && !g_laid))
        return;
    if (g_found && g_opened == nullptr)
        put_cursor();
    const ULONGLONG now = GetTickCount64();
    if (now < g_next)
        return;
    g_next = now + 500;
    if (!found()) {
        g_failed = g_searched;
        return;
    }
    apply();
}

} // namespace vanacore::equipfirst
