/* commentwin.keep: the comment box (a checked player's bazaar message) kept up while the Check window's cursor is on an item.
 * hudwin.cpp includes this inside its namespace, after its helpers (at, named, open_window, g_manager), whose names it
 * uses. It is no file of its own. */
#pragma once

namespace {

// The Check window's cursor moved (its handler's slot 0x1C, 0x10133430, __thiscall; read 2026-11-09): on a row that's no
// item (View Wares) it closes the item's details and opens the comment box (CreateDrawMenu "menu    comment ", 1, 0); on
// an item it closes the comment box (DestroyDrawMenu, the comment's window, 1) and opens the item's details, so the
// bazaar message went whenever an item was looked at. push esi; mov esi,ecx; push edi; mov eax,[esi+8]; mov cx,[eax+4Ch];
// push ecx; mov ecx,esi; call; mov edi,eax; mov al,[esi+14Fh]; cmp al,0FFh; jne; test di,di; jge
constexpr char kCheckCursor[] = "56 8B F1 57 8B 46 08 66 8B 48 4C 51 8B CE E8 ?? ?? ?? ?? 8B F8 8A 86 4F 01 00 00 3C FF 75 ?? 66 85 FF 7D";
// the opening's call: mov byte ptr [eax+17h],73h; call
constexpr std::ptrdiff_t kCommentOpenCall = 0x5C;
constexpr std::uint8_t kCommentOpenBefore[] = {0xC6, 0x40, 0x17, 0x73};
// the closing's: push ecx; mov ecx,<the manager>; call
constexpr std::ptrdiff_t kCommentCloseCall = 0x76;
// The menu manager's update (each frame, from 0x1015EB40; read live 2026-11-09) gives each screen area (a window's +0x38,
// bits) to the first window in its list that has it, the newest first, and hides every later one sharing it (0x1015D410,
// __thiscall manager: window, 1, 1, 0, 1; ret 14h: state 0xE, not destroyed), to open it again once the area is free. The
// item details and the comment box are both area 0xC, so with its close skipped the details still hid it (seen live: the
// comment closed, not destroyed, under iteminfo; given area 0, it stayed up). mov ax,[esi+38h]; test ax,ax; jz;
// mov edx,eax; and edx,edi; test dx,dx; jz; cmp [esi+10h],ebx; jz; push 1; push 0; push 1; push 1; push esi; mov ecx,ebp;
// call
constexpr char kAreaHide[] = "66 8B 46 38 66 85 C0 74 ?? 8B D0 23 D7 66 85 D2 74 ?? 39 5E 10 74 ?? 6A 01 6A 00 6A 01 6A 01 56 8B CD E8";
constexpr std::ptrdiff_t kAreaHideCall = 0x22;
constexpr char kCommentName[] = "menu    comment ";
constexpr char kCheckName[] = "menu    inspect ";
using OpenMenu = char*(__thiscall*)(void* manager, const char* name, int a, int b);
using AreaHide = void(__thiscall*)(void* manager, void* window, int a, int b, int c, int d);
OpenMenu g_open_comment = nullptr;
DestroyMenu g_close_comment = nullptr;
AreaHide g_area_hide = nullptr;
bool g_keep_comment = false; // commentwin.keep
bool g_comment_tried = false;

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), sizeof rel);
    return call + 5 + rel;
}

// While commentwin.keep is on the comment box stays up as the cursor goes onto an item
void __fastcall close_comment_hook(void* manager, void*, void* window, int wait)
{
    if (!g_keep_comment)
        g_close_comment(manager, window, wait);
}

// ... and back on View Wares the one up is left as it is: CreateDrawMenu takes the window of its name that's there and
// sets it up anew (itemlist.cpp's keep_rows)
char* __fastcall open_comment_hook(void* manager, void*, const char* name, int a, int b)
{
    if (g_keep_comment) {
        char* open = open_window(kCommentName);
        if (open != nullptr && at<std::uint8_t>(open, kWinDestroyed) == 0)
            return open;
    }
    return g_open_comment(manager, name, a, b);
}

// ... and the item details opening in its area don't hide it, nor it them, while the Check window is up
void __fastcall area_hide_hook(void* manager, void*, char* window, int a, int b, int c, int d)
{
    if (g_keep_comment && window != nullptr && open_window(kCheckName) != nullptr &&
        (named(window, kCommentName) || (table_window(window) == window_named("itemdetails") && open_window(kCommentName) != nullptr)))
        return;
    g_area_hide(manager, window, a, b, c, d);
}

// The Check window's cursor handler's two calls, when they're as known
bool cursor_hooks(HMODULE game)
{
    const std::uintptr_t cursor = scan::find(game, kCheckCursor);
    const std::uintptr_t open = cursor + kCommentOpenCall, close = cursor + kCommentCloseCall;
    std::uintptr_t manager = 0;
    if (cursor != 0)
        std::memcpy(&manager, reinterpret_cast<const void*>(close - 4), sizeof manager);
    if (cursor == 0 || *reinterpret_cast<const std::uint8_t*>(open) != 0xE8 || *reinterpret_cast<const std::uint8_t*>(close) != 0xE8 ||
        std::memcmp(reinterpret_cast<const void*>(open - sizeof kCommentOpenBefore), kCommentOpenBefore, sizeof kCommentOpenBefore) != 0 ||
        *reinterpret_cast<const std::uint16_t*>(close - 6) != 0xB951 || manager != reinterpret_cast<std::uintptr_t>(g_manager))
        return false;
    g_open_comment = reinterpret_cast<OpenMenu>(call_target(open));
    g_close_comment = reinterpret_cast<DestroyMenu>(call_target(close));
    asm_text::branch(open, 0xE8, reinterpret_cast<std::uintptr_t>(&open_comment_hook), 0);
    asm_text::branch(close, 0xE8, reinterpret_cast<std::uintptr_t>(&close_comment_hook), 0);
    return true;
}

// From skip (the game's thread, the first window drawn): the calls redirected, when they're as known
void comment_hooks()
{
    if (g_comment_tried || g_manager == nullptr)
        return;
    g_comment_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t area = scan::find(game, kAreaHide);
    if (area == 0 || *reinterpret_cast<const std::uint8_t*>(area + kAreaHideCall) != 0xE8 || !cursor_hooks(game)) {
        log::write(L"HUD windows: the Check window's cursor handler or the menus' area hide wasn't found in FFXiMain.dll: "
                   L"[client] commentwin.keep does nothing");
        return;
    }
    g_area_hide = reinterpret_cast<AreaHide>(call_target(area + kAreaHideCall));
    asm_text::branch(area + kAreaHideCall, 0xE8, reinterpret_cast<std::uintptr_t>(&area_hide_hook), 0);
}

} // namespace
