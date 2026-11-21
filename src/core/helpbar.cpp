#include "helpbar.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "chatfont.hpp"
#include "hudwin.hpp"
#include "log.hpp"
#include "netstat.hpp"
#include "scan.hpp"

namespace vanacore::helpbar {
namespace {

// The help bar is three windows of the menu manager, each with a layout named in the game's data (16 characters at the
// layout's +0x46, a window's +4 being its layout): "menu    helpwind" (the long bar), "menu    titlewin" (the box at
// its left) and, for network.panel, "menu    netstat " (the Network window).
constexpr char kHelpLayout[16 + 1] = "menu    helpwind";
constexpr char kTitleLayout[16 + 1] = "menu    titlewin";
constexpr char kNetLayout[16 + 1] = "menu    netstat ";
constexpr std::ptrdiff_t kWindowLayout = 4;
constexpr std::ptrdiff_t kLayoutName = 0x46;
constexpr std::ptrdiff_t kWindowOwner = 0xC; // the object the window belongs to (the help line's, whose draw has the text)
constexpr std::ptrdiff_t kOwnerDrawSlot = 4; // its vtable's draw (+0x10)

// The help line's draw (the owner's vtable slot 4, FUN_10139E90 in the PS2 client's order of things): sets the text
// renderer's colours, draws the text line at the owner's +0x20 and then, if the window is up, the "Help" caption's sprite.
//
// sub esp,8; push ebx; push esi; mov esi,ecx; mov ecx,[renderer]; call; mov ecx,[renderer]; call; mov eax,[renderer];
// mov ebx,80808080h; lea ecx,[esi+20h]; mov [eax+64h],ebx; mov [eax+48h],ebx; mov [eax+2Ch],ebx; mov [eax+10h],ebx;
// call <the text line's draw, __thiscall, no arguments>
constexpr char kDraw[] = "83 EC 08 53 56 8B F1 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? "
                         "BB 80 80 80 80 8D 4E 20 89 58 64 89 58 48 89 58 2C 89 58 10 E8";
constexpr std::ptrdiff_t kTextCall = 54;
// ...then: mov ecx,[esi+8]; call <is it up>; test al,al; je; mov ecx,[esi+8]; push edi; ... mov ecx,[esi+1Ch]; push edx;
// call <the caption's sprite draw, __thiscall (x, y, colour, 0, 0), ret 14h>
constexpr std::ptrdiff_t kLabelCall = 128;
constexpr std::uint8_t kLabelBefore[4] = {0x8B, 0x4E, 0x1C, 0x52}; // mov ecx,[esi+1Ch]; push edx

using TextDraw = void(__thiscall*)(void* line);
using LabelDraw = void(__thiscall*)(void* sprite, int x, int y, std::uint32_t colour, int a, int b);

// [client] helpbar.*
bool g_hide = false; // helpbar.hide: the help line left out of the draw (the title box's own is helptitle.hide, hudwin.cpp's)
bool g_background = true;
bool g_title = true;
bool g_text = true;
bool g_label = true;

bool g_tried = false;
std::uintptr_t g_text_call = 0;
std::uintptr_t g_label_call = 0;
TextDraw g_text_game = nullptr;
LabelDraw g_label_game = nullptr;
bool g_text_patched = false;
bool g_label_patched = false;

// The game's text line, left out with helpbar.text=0
void __fastcall text_hook(void* line, void*)
{
    if (g_text)
        g_text_game(line);
}

// The caption's sprite, left out with helpbar.label=0
void __fastcall label_hook(void* sprite, void*, int x, int y, std::uint32_t colour, int a, int b)
{
    if (g_label)
        g_label_game(sprite, x, y, colour, a, b);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

// A call's offset, written in one store (the game's thread runs this code)
bool write_offset(std::uintptr_t call, std::uintptr_t target)
{
    const std::uintptr_t at = call + 1;
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Help bar: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(at), GetLastError());
        return false;
    }
    *reinterpret_cast<volatile std::int32_t*>(at) = static_cast<std::int32_t>(target - (call + 5));
    VirtualProtect(reinterpret_cast<void*>(at), 4, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(at), 4);
    return true;
}

bool all_on() { return !g_hide && g_background && g_title && g_text && g_label; }

void install_hooks()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    const std::uintptr_t draw = game != nullptr ? scan::find(game, kDraw) : 0;
    if (draw == 0) {
        log::write(L"Help bar: the game's help line drawing wasn't found in FFXiMain.dll: helpbar.text and helpbar.label do nothing");
        return;
    }
    const std::uintptr_t text = draw + kTextCall;
    const std::uintptr_t label = draw + kLabelCall;
    const auto* base = reinterpret_cast<const std::uint8_t*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
    const auto in_game = [&](std::uintptr_t at) { return at > reinterpret_cast<std::uintptr_t>(base) && at < end; };
    if (*reinterpret_cast<const std::uint8_t*>(text) == 0xE8 && in_game(call_target(text))) {
        g_text_call = text;
        g_text_game = reinterpret_cast<TextDraw>(call_target(text));
    }
    if (*reinterpret_cast<const std::uint8_t*>(label) == 0xE8 && in_game(call_target(label)) &&
        std::memcmp(reinterpret_cast<const void*>(label - sizeof kLabelBefore), kLabelBefore, sizeof kLabelBefore) == 0) {
        g_label_call = label;
        g_label_game = reinterpret_cast<LabelDraw>(call_target(label));
    }
    if (g_text_call == 0)
        log::write(L"Help bar: the help line's text draw isn't as expected: helpbar.text does nothing");
    if (g_label_call == 0)
        log::write(L"Help bar: the \"Help\" caption's draw isn't as expected: helpbar.label does nothing");
    if (g_text_call != 0)
        g_text_patched = write_offset(g_text_call, reinterpret_cast<std::uintptr_t>(&text_hook));
    if (g_label_call != 0)
        g_label_patched = write_offset(g_label_call, reinterpret_cast<std::uintptr_t>(&label_hook));
    log::write(L"Help bar: the help line's drawing hooked at +0x%X (text %d, caption %d)",
               static_cast<unsigned>(draw - reinterpret_cast<std::uintptr_t>(game)), g_text_patched ? 1 : 0, g_label_patched ? 1 : 0);
}

bool same_name(const char* name, const char (&layout)[17])
{
    return std::memcmp(name, layout, 16) == 0;
}

// The window's owner's own drawing (its contents), which the game's window draw calls from inside it; in the window's True
// Font area, as chat.cpp's window draw has it after this left it out: the help line's text was the menus' font (General's
// switch) with helpbar.background=0, whatever help.font's (reported in game 2026-11-10)
void draw_owner(const char* window)
{
    void* owner = *reinterpret_cast<void* const*>(window + kWindowOwner);
    if (owner == nullptr)
        return;
    const chatfont::Area area(*reinterpret_cast<const char* const*>(window + kWindowLayout));
    void* const* vtable = *reinterpret_cast<void* const* const*>(owner);
    reinterpret_cast<void(__thiscall*)(void*)>(vtable[kOwnerDrawSlot])(owner);
}

// network.background=0: the Network window's frame is left out. Whether its numbers are drawn from inside the frame's draw
// (then they go with it, as the help line does, and are drawn from here) or by the game apart from it isn't known
// from the code, so it's watched: a frame with the numbers drawn between two of the frame's skipped draws says they're
// the game's own; none says they went with it.
enum class Contents { Unknown, Apart, Inside };
Contents g_contents = Contents::Unknown;
bool g_probing = false;
unsigned g_seen = 0;

void network_without_background(const char* window)
{
    if (g_contents == Contents::Unknown) {
        const unsigned drawn = netstat::contents_draws();
        if (g_probing) {
            g_contents = drawn != g_seen ? Contents::Apart : Contents::Inside;
            log::write(L"Network window: its numbers are drawn %ls the frame's draw ([client] network.background=0)",
                       g_contents == Contents::Apart ? L"apart from" : L"inside");
        }
        g_seen = drawn;
        g_probing = true;
        return;
    }
    if (g_contents == Contents::Inside)
        draw_owner(window);
}

} // namespace

void reload(const wchar_t* ini)
{
    g_hide = GetPrivateProfileIntW(L"client", L"helpbar.hide", 0, ini) != 0;
    g_background = GetPrivateProfileIntW(L"client", L"helpbar.background", 1, ini) != 0;
    g_title = GetPrivateProfileIntW(L"client", L"helpbar.title", 1, ini) != 0;
    g_text = GetPrivateProfileIntW(L"client", L"helpbar.text", 1, ini) != 0;
    g_label = GetPrivateProfileIntW(L"client", L"helpbar.label", 1, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (!g_tried && (!g_text || !g_label))
        install_hooks();
}

bool skip(void* ctrl)
{
    if (all_on() && !netstat::panel_hidden() && !netstat::background_hidden())
        return false;
    __try {
        const auto* window = static_cast<const char*>(ctrl);
        const char* layout = *reinterpret_cast<const char* const*>(window + kWindowLayout);
        if (layout == nullptr)
            return false;
        const char* name = layout + kLayoutName;
        if (same_name(name, kHelpLayout)) {
            if (g_hide)
                return true;
            if (g_background)
                return false;
            // the frame left out; the owner's own drawing (the text line and the caption) goes on without it
            if (g_text || g_label)
                draw_owner(window);
            return true;
        }
        if (same_name(name, kTitleLayout))
            return !g_title; // (helpbar.title=0, as the title box was hidden before helptitle.hide; hudwin.cpp has that one)
        if (same_name(name, kNetLayout)) {
            if (netstat::panel_hidden())
                return true;
            if (!netstat::background_hidden())
                return false;
            network_without_background(window);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool idle(const void* ctrl)
{
    if (ctrl == nullptr)
        return false;
    __try {
        const auto* window = static_cast<const char*>(ctrl);
        const char* layout = *reinterpret_cast<const char* const*>(window + kWindowLayout);
        if (layout == nullptr)
            return false;
        const char* name = layout + kLayoutName;
        return (same_name(name, kHelpLayout) || same_name(name, kTitleLayout)) && hudwin::faded(ctrl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

} // namespace vanacore::helpbar
