#include "menulog.hpp"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chat.hpp"
#include "hudwin.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "menuhover.hpp"
#include "sampler.hpp"
#include "scan.hpp"

namespace vanacore::menulog {
namespace {

// The menu manager (CTkMenuMng, VA 0x105ECB98, hudwin.cpp finds it): +0 the list of its windows, +0x54 the window the keys
// go to (m_pCurMCD, its current one; set only by SetCurMCD, VA 0x1015D580, which keeps the one before at +0x58: read live
// 2026-10-28 with the sort box up, which had the cursor and the keys, +0x58 the Items list). A list node: +0 the next,
// +0x10 its item, +0x14 set for one taken out (the game's walk, FUN_10126060, passes over it).
constexpr std::ptrdiff_t kCurrent = 0x54;
constexpr std::ptrdiff_t kNodeItem = 0x10, kNodeOut = 0x14;
// A window (CTkMenuCtrlData; PS2 TkMenuCtrlData.cpp): +4 its layout (CTkMenuData: its name, 16 characters, at +0x46; the
// cursor kept for its next opening at +0x4E, saved by CloseDrawMenu and taken by CreateDrawMenu), +0xC its owner (the
// handler, whose +8 is the window), +0x10 its state (0xE closed or closing), +0x14 its pieces (a piece's +0 its kind, 1 a
// button; a button's id at +0x3A, as the game's lookup FUN_10117DD0 reads it), +0x34 its flags, +0x38 its area, +0x3A to
// +0x40 its frame, +0x4C its cursor, +0x58 its count of buttons (the cursor's limit: the setters clamp to it), +0x64 and
// +0x66 its close timers, +0x69 its frame drawn, +0x6E destroyed, +0x70 to be deleted, +0x94 its resize countdown (-999
// when it isn't resizing).
constexpr std::ptrdiff_t kLayout = 4, kOwner = 0xC, kState = 0x10, kPieces = 0x14, kFlags = 0x34, kArea = 0x38, kFrame = 0x3A,
                         kCursor = 0x4C, kCount = 0x58, kCloseAnim = 0x64, kCloseWait = 0x66, kDrawn = 0x69, kDestroyed = 0x6E,
                         kDelete = 0x70, kResizing = 0x94;
constexpr std::ptrdiff_t kLayoutName = 0x46, kLayoutCursor = 0x4E, kOwnerWindow = 8, kPieceId = 0x3A;
constexpr std::uint32_t kClosed = 0xE;
constexpr int kSettled = -999;

constexpr int kMostWindows = 128;
constexpr int kMostLines = 4000; // a recording's
constexpr int kMostNodes = 512;

struct Win {
    const char* at = nullptr;
    const char* layout = nullptr;
    char raw[16] = {};
    short kept = 0;
    const char* owner = nullptr;
    const char* owner_window = nullptr;
    std::uint32_t state = 0, flags = 0;
    std::uint16_t area = 0;
    short frame[4] = {};
    short cursor = 0, count = 0;
    int buttons = 0;        // button pieces in its list
    bool on_button = false; // one has the cursor's id
    short close_anim = 0, close_wait = 0;
    int resizing = 0;
    std::uint8_t drawn = 0, destroyed = 0, del = 0;
    bool ok = false;
};

std::atomic<bool> g_on{false};
int g_lines = 0;
bool g_capped = false;
bool g_first = false;          // the first look of a recording: every window logged as up
std::vector<Win> g_last;
const char* g_current = nullptr;
bool g_list_bad = false;
bool g_no_manager = false;
const void* g_sel = nullptr;   // VanaCore's selection-menu window (chat.cpp), as last logged
int g_sel_rows = 0;
bool g_sel_other = false;      // it's another window's now, within the menu hooks' half second
bool g_cleared[256] = {};      // keys held that an addon's key_state let go, as last logged
struct Step {
    const void* data = nullptr;
    int cursor = 0, key = 0, result = 0;
    ULONGLONG at = 0;
} g_step;

void line(const char* text)
{
    if (g_lines >= kMostLines) {
        if (!g_capped) {
            g_capped = true;
            log::write(L"Menus: %d lines logged in this recording; the rest are left out", kMostLines);
        }
        return;
    }
    ++g_lines;
    log::write(L"Menus: %hs", text);
}

std::string format(const char* f, ...)
{
    char buffer[768];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buffer, sizeof buffer, f, args);
    va_end(args);
    return buffer;
}

unsigned hex(const void* p)
{
    return static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(p));
}

// One window read, false where it faults (a window freed while its node is still linked)
bool read_window(const char* w, Win& o)
{
    o.ok = false;
    __try {
        o.at = w;
        o.layout = *reinterpret_cast<const char* const*>(w + kLayout);
        if (o.layout != nullptr) {
            std::memcpy(o.raw, o.layout + kLayoutName, sizeof o.raw);
            o.kept = *reinterpret_cast<const short*>(o.layout + kLayoutCursor);
        }
        o.owner = *reinterpret_cast<const char* const*>(w + kOwner);
        o.owner_window = o.owner != nullptr ? *reinterpret_cast<const char* const*>(o.owner + kOwnerWindow) : nullptr;
        o.state = *reinterpret_cast<const std::uint32_t*>(w + kState);
        o.flags = *reinterpret_cast<const std::uint32_t*>(w + kFlags);
        o.area = *reinterpret_cast<const std::uint16_t*>(w + kArea);
        std::memcpy(o.frame, w + kFrame, sizeof o.frame);
        o.cursor = *reinterpret_cast<const short*>(w + kCursor);
        o.count = *reinterpret_cast<const short*>(w + kCount);
        o.close_anim = *reinterpret_cast<const short*>(w + kCloseAnim);
        o.close_wait = *reinterpret_cast<const short*>(w + kCloseWait);
        o.drawn = *reinterpret_cast<const std::uint8_t*>(w + kDrawn);
        o.destroyed = *reinterpret_cast<const std::uint8_t*>(w + kDestroyed);
        o.del = *reinterpret_cast<const std::uint8_t*>(w + kDelete);
        o.resizing = *reinterpret_cast<const int*>(w + kResizing);
        o.buttons = 0;
        o.on_button = false;
        int nodes = 0;
        for (const char* node = *reinterpret_cast<const char* const*>(w + kPieces); node != nullptr && nodes < kMostNodes;
             node = *reinterpret_cast<const char* const*>(node), ++nodes) {
            if (*reinterpret_cast<const std::uint8_t*>(node + kNodeOut) != 0)
                continue;
            const char* piece = *reinterpret_cast<const char* const*>(node + kNodeItem);
            if (piece == nullptr || *reinterpret_cast<const short*>(piece) != 1)
                continue;
            ++o.buttons;
            if (*reinterpret_cast<const short*>(piece + kPieceId) == o.cursor)
                o.on_button = true;
        }
        o.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o.ok = false;
    }
    return o.ok;
}

// The manager's windows, as many as fit; `ok` false where the walk faulted (it stops there)
int read_list(const char* manager, Win* out, int most, const char*& current, bool& ok)
{
    int n = 0;
    ok = false;
    current = nullptr;
    __try {
        current = *reinterpret_cast<const char* const*>(manager + kCurrent);
        int nodes = 0;
        for (const char* node = *reinterpret_cast<const char* const*>(manager); node != nullptr && n < most && nodes < kMostNodes;
             node = *reinterpret_cast<const char* const*>(node), ++nodes) {
            if (*reinterpret_cast<const std::uint8_t*>(node + kNodeOut) != 0)
                continue;
            const char* w = *reinterpret_cast<const char* const*>(node + kNodeItem);
            if (w == nullptr)
                continue;
            out[n] = Win{};
            read_window(w, out[n]);
            ++n;
        }
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return n;
}

// Its layout's name without the game's "menu    " ("query", "menuwind")
std::string name_of(const Win& w)
{
    if (w.layout == nullptr)
        return "(no layout)";
    std::string n(w.raw, sizeof w.raw);
    for (char& c : n)
        if (c == '\0')
            c = ' ';
    if (n.compare(0, 8, "menu    ") == 0)
        n.erase(0, 8);
    while (!n.empty() && n.back() == ' ')
        n.pop_back();
    return n.empty() ? "?" : n;
}

// A selection menu's window ([client] menu.rows, chat.cpp): an NPC's event choices ("query") or the client's own custom menu
// ("gmtell": a menu the server sends as text, through the game's second query control)
bool selection(const Win& w)
{
    const std::string name = name_of(w);
    return name == "query" || name == "gmtell";
}

std::string who(const Win& w)
{
    return format("%s @%08X", name_of(w).c_str(), hex(w.at));
}

std::string state_of(const Win& w)
{
    if (w.state != kClosed)
        return format("open (state 0x%X)", w.state);
    std::string s = "closed";
    if (w.close_wait > 0 || w.close_anim > 0)
        s += format(" (closing: %d, %d)", w.close_wait, w.close_anim);
    if (w.destroyed != 0)
        s += ", destroyed";
    if (w.del != 0)
        s += ", to be deleted";
    return s;
}

// What can't be right about it, each "; ! ..."
std::string problems(const Win& w)
{
    if (!w.ok)
        return "; ! unreadable (freed while still in the list?)";
    std::string p;
    if (w.layout == nullptr)
        p += "; ! no layout";
    if (w.owner != nullptr && w.owner_window != w.at)
        p += format("; ! its owner's window is @%08X, not this one", hex(w.owner_window));
    if (w.state != kClosed && w.destroyed == 0 && w.count > 0) {
        if (w.cursor < 1 || w.cursor > w.count)
            p += format("; ! cursor %d outside 1-%d", w.cursor, w.count);
        else if (!w.on_button)
            p += format("; ! no button %d (the game finds none for the cursor%s)", w.cursor,
                        selection(w) && w.cursor > 3 ? ": VanaCore's menu.rows answers it with row 3's" : "");
    }
    return p;
}

std::string describe(const Win& w)
{
    if (!w.ok)
        return "unreadable";
    return format("%s, cursor %d of %d (%d button pieces), kept cursor %d, flags %08X, area %02X, at %d,%d-%d,%d, owner @%08X, frame %s",
                  state_of(w).c_str(), w.cursor, w.count, w.buttons, w.kept, w.flags, w.area, w.frame[0], w.frame[1], w.frame[2],
                  w.frame[3], hex(w.owner), w.drawn != 0 ? "drawn" : "not drawn");
}

// What changed from `a` to `b` (the same window), "" for nothing
std::string changes(const Win& a, const Win& b)
{
    std::string c;
    auto add = [&](const std::string& part) {
        if (!c.empty())
            c += ", ";
        c += part;
    };
    if (a.ok != b.ok)
        add(b.ok ? "readable again" : "unreadable");
    if (!b.ok)
        return c;
    if (a.state != b.state || a.destroyed != b.destroyed || a.del != b.del)
        add(state_of(b));
    if (a.cursor != b.cursor)
        add(format("cursor %d -> %d", a.cursor, b.cursor));
    if (a.count != b.count)
        add(format("count of buttons %d -> %d", a.count, b.count));
    if (a.buttons != b.buttons)
        add(format("button pieces %d -> %d", a.buttons, b.buttons));
    if (a.kept != b.kept)
        add(format("kept cursor %d -> %d", a.kept, b.kept));
    if (a.flags != b.flags)
        add(format("flags %08X -> %08X", a.flags, b.flags));
    if (a.owner != b.owner || a.owner_window != b.owner_window)
        add(format("owner @%08X -> @%08X", hex(a.owner), hex(b.owner)));
    if (a.drawn != b.drawn)
        add(b.drawn != 0 ? "frame drawn" : "frame not drawn");
    // its size once it's settled (not while it opens or closes, when it changes each frame)
    const int aw = a.frame[2] - a.frame[0], ah = a.frame[3] - a.frame[1], bw = b.frame[2] - b.frame[0], bh = b.frame[3] - b.frame[1];
    if (a.resizing == kSettled && b.resizing == kSettled && b.state != kClosed && (aw != bw || ah != bh))
        add(format("size %dx%d -> %dx%d (at %d,%d)", aw, ah, bw, bh, b.frame[0], b.frame[1]));
    if (!c.empty())
        c += problems(b);
    return c;
}

const Win* find(const std::vector<Win>& list, const void* at)
{
    for (const Win& w : list)
        if (w.at == at)
            return &w;
    return nullptr;
}

std::string current_text(const char* current, const std::vector<Win>& list)
{
    if (current == nullptr)
        return "no window";
    const Win* w = find(list, current);
    if (w == nullptr)
        return format("@%08X ! not in the window list", hex(current));
    return who(*w) + " (" + state_of(*w) + ")";
}

// VanaCore's selection menu ([client] menu.rows, chat.cpp): the window it keeps outlives the menu
void look_at_selection(const std::vector<Win>& now)
{
    const void* window = nullptr;
    int rows = 0, setting = 0;
    unsigned long long since = 0;
    chat::menu_state(window, rows, setting, since);
    const Win* w = window != nullptr ? find(now, window) : nullptr;
    if (window != g_sel || rows != g_sel_rows) {
        g_sel = window;
        g_sel_rows = rows;
        if (window != nullptr) {
            const std::string where = w != nullptr ? who(*w) + " in the list" : std::string("not in the list");
            line(format("VanaCore's selection menu (menu.rows %d): window @%08X, given %d rows; %s", setting, hex(window), rows,
                        where.c_str())
                     .c_str());
        }
    }
    const bool other = window != nullptr && since < 500 && w != nullptr && !selection(*w);
    if (other != g_sel_other) {
        g_sel_other = other;
        if (other)
            line(format("! VanaCore's selection-menu window @%08X is %s's now, %llu ms after the menu was drawn (its hooks check "
                        "the window is a selection menu's)",
                        hex(window), name_of(*w).c_str(), since)
                     .c_str());
    }
}

bool menu_key(std::uint32_t code)
{
    switch (code) {
    case 0x01: case 0x0C: case 0x0F: case 0x1C: case 0x9C: case 0xC8: case 0xD0: case 0xCB: case 0xCD: case 0xC9: case 0xD1:
    case 0xC7: case 0xCF: case 0xD2: case 0xD3: case 0x37: case 0xB5:
        return true;
    default:
        return (code >= 0x47 && code <= 0x53) || (code >= 0x3B && code <= 0x44) || code == 0x57 || code == 0x58;
    }
}

std::string key_name(std::uint32_t code)
{
    switch (code) {
    case 0x01: return "Escape";
    case 0x0C: return "Minus";
    case 0x0F: return "Tab";
    case 0x1C: return "Enter";
    case 0x9C: return "Numpad Enter";
    case 0xC8: return "Up";
    case 0xD0: return "Down";
    case 0xCB: return "Left";
    case 0xCD: return "Right";
    case 0xC9: return "Page Up";
    case 0xD1: return "Page Down";
    case 0xC7: return "Home";
    case 0xCF: return "End";
    case 0xD2: return "Insert";
    case 0xD3: return "Delete";
    case 0x37: return "Numpad *";
    case 0xB5: return "Numpad /";
    case 0x53: return "Numpad .";
    case 0x57: return "F11";
    case 0x58: return "F12";
    default: break;
    }
    if (code >= 0x3B && code <= 0x44)
        return format("F%u", code - 0x3B + 1);
    static const char kPad[] = "789-456+1230";
    if (code >= 0x47 && code <= 0x52)
        return format("Numpad %c", kPad[code - 0x47]);
    return format("key 0x%02X", code);
}

// SetCurMCD (CTkMenuMng, VA 0x1015D580; thiscall, the window on the stack, ret 4): the game's one place that makes a window the
// one the keys go to. Hooked for good, recording or not: an Options box (the Items list's "itmsort2", the Magic list's
// "mgcsortw"...) made the current window is logged with the code that asked and the changes before it, so an Items list that
// comes up with its sort box holding the keys (reported 2026-10-28, only after the game was started) shows who gave them
constexpr char kSetCur[] = "56 8B F1 57 8B 7C 24 0C 8B 4E 54 3B CF 74 36 85 C9 74 11 6A 00 E8 ?? ?? ?? ?? 8B 46 54 C6 80 98 00 00 00 00 8B 4E 54 89 7E 54";
constexpr std::size_t kSetCurPrologue = 8;
constexpr int kRecentChanges = 8;
constexpr int kMostCallers = 4;
constexpr int kMostSortLogged = 100;

using SetCur = void(__fastcall*)(char* manager, void* edx, char* window);
SetCur g_set_cur = nullptr;
bool g_set_cur_searched = false;
std::uintptr_t g_game_begin = 0, g_game_end = 0;

struct Change {
    char name[17] = {};
    ULONGLONG at = 0;
    std::uintptr_t from = 0;
};
Change g_changes[kRecentChanges];
int g_changes_next = 0;
int g_sort_logged = 0;
ULONGLONG g_focus_changed = 0; // the last change of the window the keys go to

// A window's layout name without the game's "menu    " ("inventor", "itmsort2"); "none" for no window, "?" where it can't be read
void window_name(const char* window, char (&out)[17])
{
    std::memcpy(out, window == nullptr ? "none" : "?", window == nullptr ? 5 : 2);
    if (window == nullptr)
        return;
    __try {
        const char* layout = *reinterpret_cast<const char* const*>(window + kLayout);
        if (layout == nullptr)
            return;
        char raw[17] = {};
        std::memcpy(raw, layout + kLayoutName, 16);
        const char* start = std::memcmp(raw, "menu    ", 8) == 0 ? raw + 8 : raw;
        std::size_t n = std::strlen(start);
        while (n > 0 && start[n - 1] == ' ')
            --n;
        std::memcpy(out, start, n);
        out[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memcpy(out, "?", 2);
    }
}

bool sort_box(const char* name)
{
    const std::size_t n = std::strlen(name);
    return std::strcmp(name, "itmsort2") == 0 || (n >= 5 && std::strcmp(name + n - 5, "sortw") == 0);
}

// An address of code as "module+0xOFFSET" (FFXiMain.dll's with the VA Ghidra shows)
std::string code_at(std::uintptr_t address)
{
    if (address >= g_game_begin && address < g_game_end)
        return format("FFXiMain.dll+0x%X (VA 0x%X)", static_cast<unsigned>(address - g_game_begin),
                      static_cast<unsigned>(address - g_game_begin + 0x10000000));
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH) != 0) {
        const wchar_t* name = wcsrchr(path, L'\\');
        char narrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, name != nullptr ? name + 1 : path, -1, narrow, sizeof narrow - 1, nullptr, nullptr);
        return format("%s+0x%X", narrow, static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(module)));
    }
    return format("0x%08X", static_cast<unsigned>(address));
}

// The return addresses in FFXiMain.dll (a call's next instruction) among the stack words above the hook's argument `arg`:
// the code that asked, the nearest first
int callers_of(const void* arg, std::uintptr_t (&out)[kMostCallers])
{
    int n = 0;
    __try {
        const auto* words = reinterpret_cast<const std::uintptr_t*>(arg);
        for (int i = 1; i < 96 && n < kMostCallers; ++i) {
            const std::uintptr_t v = words[i];
            if (v > g_game_begin + 5 && v < g_game_end && *reinterpret_cast<const std::uint8_t*>(v - 5) == 0xE8)
                out[n++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// The frame (left, top, right, bottom: the units of the pointer's place) of a window; false where it can't be read
bool frame_of(const char* window, short (&frame)[4])
{
    __try {
        std::memcpy(frame, window + kFrame, sizeof frame);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void log_sort_focus(const char* name, const char* before, std::uintptr_t from, const void* arg, const char* window)
{
    std::uintptr_t callers[kMostCallers] = {};
    const int n = callers_of(arg, callers);
    // where the pointer was, and where the box: the game hands the list's key 8 (the box) when the pointer is over the box
    std::string pointer = "the pointer unknown";
    int px = 0, py = 0;
    bool moved = false;
    short frame[4] = {};
    if (menuhover::pointer(px, py, moved) && window != nullptr && frame_of(window, frame)) {
        const bool over = px >= frame[0] && px <= frame[2] && py >= frame[1] && py <= frame[3];
        pointer = format("the pointer at %d, %d (%s), the box at %d,%d-%d,%d (the pointer %s it)", px, py,
                         moved ? "moved" : "at rest", frame[0], frame[1], frame[2], frame[3], over ? "over" : "not over");
    }
    std::string chain;
    for (int i = 0; i < n; ++i)
        chain += (i == 0 ? "" : ", ") + code_at(callers[i]);
    const ULONGLONG now = GetTickCount64();
    std::string recent;
    for (int i = 0; i < kRecentChanges; ++i) {
        const Change& c = g_changes[(g_changes_next + i) % kRecentChanges];
        if (c.at == 0)
            continue;
        recent += (recent.empty() ? "" : ", ") + format("%s from %s %llu ms ago", c.name, code_at(c.from).c_str(), now - c.at);
    }
    log::write(L"Menus: the \"%hs\" box was given the keys by %hs (further up the stack: %hs); the window that had them was \"%hs\"; "
               L"%hs; the changes before it: %hs; %hs",
               name, code_at(from).c_str(), chain.empty() ? "none found" : chain.c_str(), before, pointer.c_str(),
               recent.empty() ? "none" : recent.c_str(), windows_text().c_str());
}

// In place of the game's SetCurMCD: a window made the current one, noted, an Options box logged, then the game's own
void __fastcall set_cur_hook(char* manager, void* edx, char* window)
{
    const char* const was = *reinterpret_cast<char* const*>(manager + kCurrent);
    if (window != was) {
        const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        char name[17];
        window_name(window, name);
        if (sort_box(name) && g_sort_logged < kMostSortLogged) {
            ++g_sort_logged;
            char before[17];
            window_name(was, before);
            log_sort_focus(name, before, from, &window, window);
        }
        Change& c = g_changes[g_changes_next];
        std::memcpy(c.name, name, sizeof c.name);
        c.at = GetTickCount64();
        g_focus_changed = c.at;
        c.from = from;
        g_changes_next = (g_changes_next + 1) % kRecentChanges;
    }
    g_set_cur(manager, edx, window);
}

// The game's SetCurMCD found and hooked, the first frame FFXiMain.dll is there
void hook_focus()
{
    if (g_set_cur_searched)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_set_cur_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    g_game_begin = base;
    g_game_end = base + nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t at = scan::find(game, kSetCur);
    if (at == 0) {
        log::write(L"Menus: the game's change of the window the keys go to wasn't found in FFXiMain.dll: an Options box given the keys isn't logged");
        return;
    }
    if (!imports::detour(reinterpret_cast<SetCur>(at), kSetCurPrologue, &set_cur_hook, g_set_cur)) {
        log::write(L"Menus: the game's change of the window the keys go to couldn't be hooked: an Options box given the keys isn't logged");
        return;
    }
    log::write(L"Menus: the game's change of the window the keys go to hooked at +0x%X: an Options box given the keys is logged",
               static_cast<unsigned>(at - base));
}

} // namespace

bool on()
{
    return g_on.load(std::memory_order_relaxed);
}

unsigned long long focus_changed_at()
{
    return g_focus_changed;
}

void note(const char* text)
{
    if (on() && text != nullptr)
        line(text);
}

std::string windows_text()
{
    const char* manager = hudwin::manager();
    if (manager == nullptr)
        return "no menu manager yet";
    static Win looked[kMostWindows];
    const char* current = nullptr;
    bool ok = false;
    const int n = read_list(manager, looked, kMostWindows, current, ok);
    const std::vector<Win> now(looked, looked + n);
    std::string text = "the keys go to " + current_text(current, now) + "; " + std::to_string(n) + " windows";
    int shown = 0;
    for (const Win& w : now) {
        if (w.ok && w.state == kClosed)
            continue; // a closed one is on its way out
        if (shown == 16) {
            text += ", ...";
            break;
        }
        text += (shown++ == 0 ? ": " : ", ") + who(w) + " " + state_of(w);
    }
    if (shown == 0)
        text += ", none open";
    if (!ok)
        text += " (! the list faulted)";
    return text;
}

void on_frame()
{
    hook_focus(); // always: an Options box given the keys is logged whether or not a recording runs
    const bool running = sampler::running();
    if (running != on()) {
        g_on = running;
        if (running) {
            g_lines = 0;
            g_capped = false;
            g_first = true;
            g_last.clear();
            g_current = nullptr;
            g_list_bad = false;
            g_no_manager = false;
            g_sel = nullptr;
            g_sel_rows = 0;
            g_sel_other = false;
            std::memset(g_cleared, 0, sizeof g_cleared);
            g_step = {};
            line("a stats recording started: the game's windows, menu keys and pad buttons are logged until it ends");
        } else {
            line(format("the recording ended (%d lines)", g_lines).c_str());
            g_last.clear();
            return;
        }
    }
    if (!running)
        return;
    const char* manager = hudwin::manager();
    if (manager == nullptr) {
        if (!g_no_manager) {
            g_no_manager = true;
            line("! the game's menu manager isn't known yet (no window has been drawn): no windows logged");
        }
        return;
    }
    static Win looked[kMostWindows];
    const char* current = nullptr;
    bool ok = false;
    const int n = read_list(manager, looked, kMostWindows, current, ok);
    if (ok == g_list_bad) {
        g_list_bad = !ok;
        if (!ok)
            line(format("! the window list faulted after %d windows (a window freed while still linked?)", n).c_str());
    }
    std::vector<Win> now(looked, looked + n);
    for (const Win& w : now) {
        const Win* was = find(g_last, w.at);
        if (was == nullptr || was->layout != w.layout) {
            std::string text = "+ " + who(w) + ": " + describe(w) + problems(w);
            if (g_first)
                text += " (up as the recording started)";
            else if (was != nullptr)
                text += " (made where " + who(*was) + " was)";
            line(text.c_str());
            continue;
        }
        const std::string c = changes(*was, w);
        if (!c.empty())
            line((who(w) + ": " + c).c_str());
    }
    for (const Win& w : g_last) {
        const Win* still = find(now, w.at);
        if (still == nullptr || still->layout != w.layout)
            line(("- " + who(w) + ": out of the list" + (w.state != kClosed ? " ! while it was open" : "")).c_str());
    }
    if (g_first || current != g_current) {
        std::string text = "the keys go to " + current_text(current, now);
        if (!g_first)
            text += " (was " + current_text(g_current, g_last) + ")";
        line(text.c_str());
        g_current = current;
    }
    look_at_selection(now);
    g_last.swap(now);
    g_first = false;
}

void key_step(const void* data, int cursor, int key, int result, const char* by)
{
    if (!on())
        return;
    cursor = static_cast<short>(cursor);
    key = static_cast<short>(key);
    const ULONGLONG at = GetTickCount64();
    if (data == g_step.data && cursor == g_step.cursor && key == g_step.key && result == g_step.result && at - g_step.at < 250)
        return; // the same step asked again in the same moment
    g_step = {data, cursor, key, result, at};
    std::string where = format("layout @%08X", hex(data));
    for (const Win& w : g_last)
        if (w.layout == data && w.ok) {
            where = who(w);
            break;
        }
    line(format("key step in %s: from %d, direction %d, to %d%s (%s)", where.c_str(), cursor, key, result,
                result == cursor ? " (stays)" : "", by != nullptr ? by : "?")
             .c_str());
}

void key(std::uint32_t code, bool down, const char* kept_by)
{
    if (!on() || (kept_by == nullptr && (!down || !menu_key(code))))
        return;
    const std::string fate = kept_by != nullptr ? std::string("kept from the game by ") + kept_by : std::string("reached the game");
    line(format("%s %s: %s", key_name(code).c_str(), down ? "pressed" : "let go", fate.c_str()).c_str());
}

void key_state(const std::uint8_t* before, const std::uint8_t* after)
{
    if (!on() || before == nullptr || after == nullptr)
        return;
    for (unsigned code = 0; code < 256; ++code) {
        const bool cleared = (before[code] & 0x80) != 0 && (after[code] & 0x80) == 0;
        if (cleared == g_cleared[code] || !menu_key(code))
            continue;
        g_cleared[code] = cleared;
        line(format("%s held: %s", key_name(code).c_str(),
                    cleared ? "an addon's key_state let it go (the game reads it as up)" : "the game reads it held again")
                 .c_str());
    }
}

void pad(bool xinput, std::uint32_t button, std::int32_t value, bool injected, const char* kept_by)
{
    if (!on() || (xinput ? button >= 16 : button < 0x20))
        return;
    const std::string fate = kept_by != nullptr ? std::string("kept from the game by ") + kept_by : std::string("reached the game");
    const std::string which = xinput ? format("XInput button %u", button) : format("DirectInput offset 0x%X", button);
    line(format("pad %s = %d%s: %s", which.c_str(), value, injected ? " (sent by a plugin or addon)" : "", fate.c_str()).c_str());
}

} // namespace vanacore::menulog
