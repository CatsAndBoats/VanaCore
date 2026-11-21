#include "chatpages.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::chatpages {
namespace {

// The page file's name (FFXiMain.dll VA 0x1018CC30, __cdecl: buffer, window, page), the only user of either format:
//
//   if (window == 0) return sprintf(buffer, "%d.log", page);    // mov [esp+8],fmt; jmp sprintf
//   return sprintf(buffer, "%d_%d.log", window, page);           // ...; push fmt; push eax; call sprintf
//
// The name goes under the TEMP root of the game's file system (0x101C2510: "TEMP/<name>"), not the character's folder.
// Writer 0x1018D390 (a page is full: the next of 20), reader 0x1018CC90 (scrolling up loads a page back).
constexpr char kName[] = "8B 44 24 08 85 C0 75 0D C7 44 24 08 ?? ?? ?? ?? E9 ?? ?? ?? ?? 8B 54 24 0C 52 50 8B 44 24 0C 68 ?? ?? ?? ?? 50 E8";
constexpr std::size_t kSingleFormat = 12; // mov [esp+8],imm32: "%d.log"
constexpr std::size_t kPageFormat = 32;   // push imm32: "%d_%d.log"

bool g_done = false;
char g_single[32]; // "%d.<pid>.log"
char g_page[32];   // "%d_%d.<pid>.log"

void write_code(std::uintptr_t at, const void* bytes, std::size_t size)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old))
        return;
    std::memcpy(reinterpret_cast<void*>(at), bytes, size);
    VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
}

// The string an imm32 at `at` points to is `expected`
bool points_to(std::uintptr_t at, const char* expected)
{
    std::uintptr_t text = 0;
    std::memcpy(&text, reinterpret_cast<const void*>(at), 4);
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(text), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
        return false;
    return std::strcmp(reinterpret_cast<const char*>(text), expected) == 0;
}

// A process that's still running (one that can't be opened for want of rights counts as running)
bool running(DWORD id)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (process == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

// The process id in a page name of ours ("1_0.<pid>.log", "0.<pid>.log"), else 0
DWORD page_owner(const wchar_t* name)
{
    const wchar_t* p = name;
    if (!iswdigit(*p))
        return 0;
    while (iswdigit(*p))
        ++p;
    if (*p == L'_') {
        ++p;
        if (!iswdigit(*p))
            return 0;
        while (iswdigit(*p))
            ++p;
    }
    if (*p++ != L'.' || !iswdigit(*p))
        return 0;
    DWORD id = 0;
    while (iswdigit(*p))
        id = id * 10 + static_cast<DWORD>(*p++ - L'0');
    return _wcsicmp(p, L".log") == 0 ? id : 0;
}

// Deletes the pages of games that have ended (the game never deletes its pages: each game's would pile up)
void sweep(HMODULE game)
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(game, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    std::wstring folder(path, length);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    folder += L"TEMP\\";
    WIN32_FIND_DATAW found;
    const HANDLE find = FindFirstFileW((folder + L"*.log").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    const DWORD self = GetCurrentProcessId();
    unsigned deleted = 0;
    do {
        const DWORD owner = page_owner(found.cFileName);
        if (owner == 0 || owner == self || running(owner))
            continue;
        if (DeleteFileW((folder + found.cFileName).c_str()))
            ++deleted;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (deleted != 0)
        log::write(L"Chat pages: %u page files of games that have ended deleted from %ls", deleted, folder.c_str());
}

// [client] chat.scrollback past 1,000 lines. A log window's saved lines (the pager the scroller makes, 0x64 bytes): +4 20
// slots (a page read back from its file, or 0), +0x54 byte the pages written (at most 20), +0x58 the file of slot 0 (slot n
// is file (+0x58 + n) % 20), +0x5C the window, +0x60 byte history mode. A page holds 50 lines, so a window kept 1,000:
// writing a 21st page dropped the oldest, and the write (0x1018D390) returned 50, the lines the scroller's numbers move
// down by. Only the scroller calls the pager, and only the eight functions here touch the slots, so VanaCore stands in for
// them with up to 250 slots of its own a pager (the scroller reads the count as a byte) and files numbered round 250. The
// game's waits for a window's pending writes (a page's teardown, 0x1018CBA0, and its write, 0x1018CDD0) name files 0-19,
// all the game ever wrote; they now name the window's 20 last written.
constexpr int kLinesPerPage = 50, kGamePages = 20, kMostPages = 250;
constexpr unsigned kFiles = 250;
constexpr std::size_t kWritten = 0x54, kBase = 0x58, kWindow = 0x5C, kMode = 0x60, kGameSlots = 4, kPageSize = 0xE0;

using PageName = int(__cdecl*)(char* buffer, int window, int file);
using Alloc = void*(__cdecl*)(std::size_t size);
using Free = void(__cdecl*)(void* block);
using PageNew = void*(__fastcall*)(void* page, void* edx, int window);
using PageClose = void(__fastcall*)(void* page, void* edx);
using PageFile = int(__fastcall*)(void* page, void* edx, const char* name);
using PageLine = void*(__fastcall*)(void* page, void* edx, int line);

PageName g_page_name = nullptr;
Alloc g_alloc = nullptr;
Free g_free = nullptr;
PageNew g_page_new = nullptr;
PageClose g_page_close = nullptr;
PageFile g_page_read = nullptr, g_page_write = nullptr;
PageLine g_page_line = nullptr;
void* g_pager_vtable = nullptr;
int g_lines = 1000; // [client] chat.scrollback (chat.cpp hands it over)

struct Pager {
    char* game = nullptr;    // the game's pager
    unsigned files = kFiles; // its files go round this many
    void* slots[kMostPages] = {};
};
std::vector<std::unique_ptr<Pager>> g_pagers;

struct LastFile {
    int file = -1; // the file a window wrote last, -1 before its first
    unsigned files = kFiles;
};
LastFile g_last[8]; // by window

int window_of(const char* pager)
{
    return *reinterpret_cast<const int*>(pager + kWindow);
}

unsigned base_of(const char* pager)
{
    return *reinterpret_cast<const unsigned*>(pager + kBase);
}

// VanaCore's slots for a pager; the first time, it takes over what the game's slots hold. A pager the game had already
// gone round its 20 files with keeps to 20 (its files are numbered round 20).
Pager& pager_of(char* game)
{
    for (const auto& pager : g_pagers)
        if (pager->game == game)
            return *pager;
    auto pager = std::make_unique<Pager>();
    pager->game = game;
    pager->files = base_of(game) == 0 ? kFiles : kGamePages;
    void** slots = reinterpret_cast<void**>(game + kGameSlots);
    for (int slot = 0; slot < kGamePages; ++slot) {
        pager->slots[slot] = slots[slot];
        slots[slot] = nullptr;
    }
    g_pagers.push_back(std::move(pager));
    return *g_pagers.back();
}

// How many pages a pager keeps: chat.scrollback's lines, never fewer than the game's 20
int pages_kept(const Pager& pager)
{
    if (pager.files == kGamePages || g_lines <= kGamePages * kLinesPerPage)
        return kGamePages;
    const int pages = (g_lines + kLinesPerPage - 1) / kLinesPerPage;
    return pages > kMostPages ? kMostPages : pages;
}

void free_page(void*& page)
{
    if (page == nullptr)
        return;
    g_page_close(page, nullptr);
    g_free(page);
    page = nullptr;
}

void* slot_at(char* game, int slot)
{
    return slot >= 0 && slot < kMostPages ? pager_of(game).slots[slot] : nullptr;
}

// 0x1018D220: history mode on (the game's dropped its slots unfreed; with the mode off none are loaded)
int __fastcall pager_enter(char* game)
{
    for (void*& page : pager_of(game).slots)
        free_page(page);
    game[kMode] = 1;
    return 1;
}

// 0x1018D240: history mode off, the pages read back let go
int __fastcall pager_leave(char* game)
{
    for (void*& page : pager_of(game).slots)
        free_page(page);
    game[kMode] = 0;
    return 1;
}

// 0x1018D200: the pager's teardown
int __fastcall pager_destroy(char* game)
{
    *reinterpret_cast<void**>(game) = g_pager_vtable;
    pager_leave(game);
    for (auto it = g_pagers.begin(); it != g_pagers.end(); ++it)
        if ((*it)->game == game) {
            g_pagers.erase(it);
            break;
        }
    game[kWritten] = 0;
    return 1;
}

// 0x1018D290 (slot, line): a line of a page read back, or 0
void* __fastcall pager_line(char* game, void*, int slot, int line)
{
    void* page = slot_at(game, slot);
    return page != nullptr ? g_page_line(page, nullptr, line) : nullptr;
}

// 0x1018D2B0 (slot): whether that page is read back
bool __fastcall page_loaded(char* game, int slot)
{
    return slot_at(game, slot) != nullptr;
}

// The game's 0x1018D2B0 leaves ecx (the pager) as it was, and its callers count on it: the scroller's line up and down
// (0x1018D710, 0x1018D7A0) call the unload next, and the write's caller (0x1018D840) the load, with ecx not loaded again.
// A C++ stand-in returns ecx as whatever it last held (pager_of's cookie check leaves the /GS cookie), so the unload then
// read a pager at a wild address: the crash of 10 November (VanaCore.Core.dll+0x441AD, pager_of from pager_unload) when the
// chat, scrolled back, reached a page 3 or more from the newest. This keeps ecx for them.
__declspec(naked) void pager_loaded()
{
    __asm {
        push ecx              ; the pager, kept for the caller
        mov edx, [esp + 8]    ; the slot
        call page_loaded      ; __fastcall: ecx the pager, edx the slot
        pop ecx
        ret 4
    }
}

// 0x1018D2C0 (slot): reads the page back from its file: 1, or -1 if the file couldn't be read (the page stays, empty)
int __fastcall pager_load(char* game, void*, int slot)
{
    if (slot < 0 || slot >= kMostPages)
        return -1;
    Pager& pager = pager_of(game);
    if (pager.slots[slot] != nullptr)
        return 1;
    void* page = g_alloc(kPageSize);
    if (page == nullptr)
        return -1;
    char name[0x100];
    g_page_name(name, window_of(game), static_cast<int>((base_of(game) + static_cast<unsigned>(slot)) % pager.files));
    g_page_new(page, nullptr, window_of(game));
    pager.slots[slot] = page;
    return g_page_read(page, nullptr, name) >= 0 ? 1 : -1;
}

// 0x1018D350 (slot): lets a page read back go
int __fastcall pager_unload(char* game, void*, int slot)
{
    if (slot >= 0 && slot < kMostPages)
        free_page(pager_of(game).slots[slot]);
    return 1;
}

// 0x1018D390 (page): the scroller's full page written to the next file. 0, or 50 when the oldest page was dropped for it
// (with chat.scrollback lowered below what's written, a page is dropped for each new one).
int __fastcall pager_write(char* game, void*, void* page)
{
    Pager& pager = pager_of(game);
    auto& written = *reinterpret_cast<std::uint8_t*>(game + kWritten);
    int dropped = 0;
    if (written >= pages_kept(pager) && written > 0) {
        free_page(pager.slots[0]);
        std::memmove(&pager.slots[0], &pager.slots[1], (written - 1u) * sizeof(void*));
        pager.slots[written - 1] = nullptr;
        ++*reinterpret_cast<unsigned*>(game + kBase);
        dropped = kLinesPerPage;
    }
    const unsigned slot = dropped != 0 ? written - 1u : written;
    const int file = static_cast<int>((base_of(game) + slot) % pager.files);
    char name[0x100];
    const int window = window_of(game);
    g_page_name(name, window, file);
    g_page_write(page, nullptr, name);
    if (dropped == 0)
        ++written;
    if (window >= 0 && window < 8)
        g_last[window] = {file, pager.files};
    return dropped;
}

// The name of the game's waits for a window's pending writes (index 0-19): the index-th last file the window wrote
int __cdecl wait_name(char* buffer, int window, int index)
{
    int file = index;
    if (window >= 0 && window < 8 && g_last[window].file >= 0) {
        const int files = static_cast<int>(g_last[window].files);
        file = ((g_last[window].file - index) % files + files) % files;
    }
    return g_page_name(buffer, window, file);
}

// The target of the call rel32 at `at`, or 0 if it isn't one
std::uintptr_t called(std::uintptr_t at)
{
    if (*reinterpret_cast<const std::uint8_t*>(at) != 0xE8)
        return 0;
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(at + 1), 4);
    return at + 5 + static_cast<std::uintptr_t>(rel);
}

// The pager's functions: the write (sub esp,100h; ...; cmp byte [esi+54h],14h) and those round it
constexpr char kPagerWrite[] = "81 EC 00 01 00 00 56 8B F1 8A 4E 54 80 F9 14 73 4B 8B 46 58 81 E1 FF 00 00 00 03 C1 33 D2 B9 14 00 00 00 F7 F1";
constexpr char kPagerDestroy[] = "56 8B F1 C7 06 ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 46 54 00 5E C3";
constexpr char kPagerEnter[] = "8B D1 57 B9 14 00 00 00 33 C0 8D 7A 04 F3 AB B8 01 00 00 00 5F 88 42 60 C3";
constexpr char kPagerLeave[] = "53 55 8B E9 33 DB 8A 45 54 84 C0 76 30 56 57 8D 7D 04 8B 37 85 F6 74 16 8B CE E8";
constexpr char kPagerLine[] = "8B 44 24 04 8B 4C 81 04 85 C9 74 0D 8B 54 24 08 52 E8 ?? ?? ?? ?? C2 08 00 33 C0 C2 08 00";
constexpr char kPagerLoaded[] = "8B 44 24 04 8B 54 81 04 85 D2 0F 95 C0 C2 04 00";
constexpr char kPagerLoad[] = "81 EC 00 01 00 00 56 57 8B BC 24 0C 01 00 00 8B F1 8B 44 BE 04 85 C0 75 65 8B 46 58 33 D2 03 C7 B9 14 00 00 00 F7 F1";
constexpr char kPagerUnload[] = "53 8B 5C 24 08 56 57 8B F9 8B 74 9F 04 85 F6 74 18 8B CE E8";

void install_pager(HMODULE game)
{
    const std::uintptr_t write = scan::find(game, kPagerWrite);
    const auto around = [write](const char* pattern) { return write != 0 ? scan::find(write - 0x400, 0x800, pattern) : 0; };
    const std::uintptr_t destroy = around(kPagerDestroy), enter = around(kPagerEnter), leave = around(kPagerLeave);
    const std::uintptr_t line = around(kPagerLine), loaded = around(kPagerLoaded), load = around(kPagerLoad);
    const std::uintptr_t unload = around(kPagerUnload);
    if (write == 0 || destroy == 0 || enter == 0 || leave == 0 || line == 0 || loaded == 0 || load == 0 || unload == 0) {
        log::write(L"Chat pages: the log windows' saved lines weren't found in FFXiMain.dll: they keep the game's 1,000 lines");
        return;
    }
    g_page_name = reinterpret_cast<PageName>(called(load + 0x31));
    g_alloc = reinterpret_cast<Alloc>(called(load + 0x3B));
    g_page_new = reinterpret_cast<PageNew>(called(load + 0x4D));
    g_page_read = reinterpret_cast<PageFile>(called(load + 0x61));
    g_page_close = reinterpret_cast<PageClose>(called(leave + 0x1A));
    g_free = reinterpret_cast<Free>(called(leave + 0x20));
    g_page_line = reinterpret_cast<PageLine>(called(line + 0x11));
    g_page_write = reinterpret_cast<PageFile>(called(write + 0x43));
    const auto name = reinterpret_cast<std::uintptr_t>(g_page_name);
    const auto close = reinterpret_cast<std::uintptr_t>(g_page_close);
    const auto page_write = reinterpret_cast<std::uintptr_t>(g_page_write);
    // the waits: the teardown's call of the name 3Ah in, the write's 5Fh in
    const std::uintptr_t close_wait = close != 0 ? close + 0x3A : 0, write_wait = page_write != 0 ? page_write + 0x5F : 0;
    if (name == 0 || g_alloc == nullptr || g_page_new == nullptr || g_page_read == nullptr || close == 0 || g_free == nullptr ||
        g_page_line == nullptr || page_write == 0 || called(write + 0x2F) != name || called(destroy + 9) != leave ||
        called(unload + 0x13) != close || called(unload + 0x19) != reinterpret_cast<std::uintptr_t>(g_free) ||
        called(close_wait) != name || called(write_wait) != name) {
        log::write(L"Chat pages: the log windows' saved lines aren't as expected in FFXiMain.dll: they keep the game's 1,000 lines");
        return;
    }
    g_pager_vtable = *reinterpret_cast<void**>(destroy + 5);
    for (const std::uintptr_t at : {close_wait, write_wait}) {
        const auto to = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&wait_name) - (at + 5));
        write_code(at + 1, &to, 4);
    }
    const std::pair<std::uintptr_t, void*> stands_in[] = {
        {destroy, reinterpret_cast<void*>(&pager_destroy)}, {enter, reinterpret_cast<void*>(&pager_enter)},
        {leave, reinterpret_cast<void*>(&pager_leave)},     {line, reinterpret_cast<void*>(&pager_line)},
        {loaded, reinterpret_cast<void*>(&pager_loaded)},   {load, reinterpret_cast<void*>(&pager_load)},
        {unload, reinterpret_cast<void*>(&pager_unload)},   {write, reinterpret_cast<void*>(&pager_write)}};
    int jumped = 0;
    for (const auto& [function, replacement] : stands_in)
        jumped += imports::jump(reinterpret_cast<void*>(function), replacement) ? 1 : 0;
    if (jumped != static_cast<int>(std::size(stands_in)))
        log::write(L"Chat pages: only %d of the saved lines' %u functions could be replaced: scrolling the log back may fail",
                   jumped, static_cast<unsigned>(std::size(stands_in)));
    else
        log::write(L"Chat pages: the log windows keep up to %d pages of %d lines (chat.scrollback) at +0x%X", kMostPages,
                   kLinesPerPage, static_cast<unsigned>(write - reinterpret_cast<std::uintptr_t>(game)));
}

// The log window's scroll up (0x10137C20) fetched older saved lines only on the Window tabs (kinds 0 and 8: test eax,eax;
// jz fetch; cmp eax,8; jz fetch; xor bl,bl): Say, Tell, Linkshell and the rest stopped at the rows they held, though the fetch
// (0x1012A190) takes the tab's kind and passes over other kinds' lines, and scrolling down fetches on every tab. With
// chat.scrollback on, the first jz is a jmp: every tab scrolls back through its own lines.
constexpr char kTabScroll[] = "8B 46 54 85 C0 74 09 83 F8 08 74 04 32 DB EB 3C";
constexpr std::size_t kTabJump = 5;
std::uintptr_t g_tab_jump = 0;

void apply_tab_scroll()
{
    if (g_tab_jump == 0)
        return;
    const std::uint8_t jump = g_lines > 0 ? 0xEB : 0x74;
    if (*reinterpret_cast<const std::uint8_t*>(g_tab_jump) != jump)
        write_code(g_tab_jump, &jump, 1);
}

// The rows' top line (the chat object's +0) or the lines' total (+0x10), or -1
int rows_line(const void* ring, std::size_t at)
{
    __try {
        return *reinterpret_cast<const int*>(static_cast<const char*>(ring) + at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The last fetch of an older line that found none: on a tab other than Window's, it looked back through every saved line
// (up to 12,500, their pages read from disk) for one of the tab's kind. The same rows looking again find none again; a new
// line, a dropped page or another tab moves their numbers.
struct OlderLook {
    const void* ring = nullptr;
    int kind = -1, top = -1, total = -1;
};
OlderLook g_no_older;

OlderLook look_of(const void* ring, int kind)
{
    return {ring, kind, rows_line(ring, 0), rows_line(ring, 0x10)};
}

void patch()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_done = true;
    install_pager(game);
    const std::uintptr_t tabs = scan::find(game, kTabScroll);
    g_tab_jump = tabs != 0 ? tabs + kTabJump : 0;
    apply_tab_scroll();
    if (tabs == 0)
        log::write(L"Chat pages: the log tabs' scroll up wasn't found in FFXiMain.dll: only the Window tabs scroll back");
    const std::uintptr_t at = scan::find(game, kName);
    if (at == 0) {
        log::write(L"Chat pages: the page name wasn't found in FFXiMain.dll: games from this install share their chat pages");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    if (!points_to(at + kSingleFormat, "%d.log") || !points_to(at + kPageFormat, "%d_%d.log")) {
        log::write(L"Chat pages: the page name at +0x%X isn't as expected: games from this install share their chat pages",
                   static_cast<unsigned>(at - base));
        return;
    }
    const DWORD self = GetCurrentProcessId();
    std::snprintf(g_single, sizeof(g_single), "%%d.%lu.log", self);
    std::snprintf(g_page, sizeof(g_page), "%%d_%%d.%lu.log", self);
    const auto single = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_single));
    const auto page = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_page));
    write_code(at + kSingleFormat, &single, 4);
    write_code(at + kPageFormat, &page, 4);
    log::write(L"Chat pages: named after this process at +0x%X (TEMP\\1_0.%lu.log...)", static_cast<unsigned>(at - base),
               self);
    sweep(game);
}

} // namespace

void on_frame()
{
    if (!g_done)
        patch();
}

void set_lines(int lines)
{
    g_lines = lines;
    apply_tab_scroll();
}

bool known_no_older(const void* ring, int kind)
{
    const OlderLook now = look_of(ring, kind);
    return now.ring == g_no_older.ring && now.kind == g_no_older.kind && now.top == g_no_older.top && now.total == g_no_older.total;
}

void note_older(const void* ring, int kind, bool found)
{
    g_no_older = found ? OlderLook{} : look_of(ring, kind);
}

} // namespace vanacore::chatpages
