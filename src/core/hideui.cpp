#include "hideui.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "binds.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::hideui {
namespace {

// FFXiMain.dll's menu manager (CTkMenuMng, g_TkMenuMng in the PS2 client; a global at VA 0x105ECB98) draws the game's
// windows, the chat log and the bars each frame unless its hide mode is 1. Its frame (OnPhaseCtrl, FUN_1015dc90) turns
// the mode over on the game's own key for it (key 0x56 of group 0x3F) in the world (phases 0x60 and 0xD0), back to 0
// on Escape (key 0x0F) while hidden, and to 0 in a cutscene or on going into another phase. SetHideMode (FUN_1015f440)
// takes a mode unless one of its two locks (+0xB2, +0xB3) is on.
//
// The manager's frame called: mov ecx,ebp; call ...; mov ecx,<the manager>; call OnPhaseCtrl; mov ecx,ebp; call ...;
// test eax,eax; mov [esp+10h],eax; jz
constexpr char kPhaseCall[] = "8B CD E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B CD E8 ?? ?? ?? ?? 85 C0 89 44 24 10 74";
constexpr std::ptrdiff_t kManagerAt = 8; // the mov ecx's operand
// SetHideMode(mode), __thiscall: mov al,[ecx+0B2h]; test al,al; jnz; mov al,[ecx+0B3h]; test al,al; jnz; mov al,[esp+4];
// cmp al,2; jge; mov [ecx+0B4h],al; ret 4
constexpr char kSetHideMode[] =
    "8A 81 B2 00 00 00 84 C0 75 22 8A 81 B3 00 00 00 84 C0 75 18 8A 44 24 04 3C 02 7D 09 88 81 B4 00 00 00 C2 04 00";
constexpr std::ptrdiff_t kPhase = 0x64;    // the manager: the game's phase as of its last frame
constexpr std::ptrdiff_t kHideMode = 0xB4; // the manager: 1 while the interface is hidden
constexpr std::ptrdiff_t kCurrent = 0x54;  // the manager: the window with the cursor (m_pCurMCD), 0 with no menu up
constexpr std::ptrdiff_t kCtrlId = 0x10;   // a window's control id, 0xE for none that's drawn
constexpr std::ptrdiff_t kWishDelete = 0x70;
constexpr std::int32_t kNoWindowId = 0xE;
constexpr std::int32_t kWorldPhases[] = {0x60, 0xD0}; // where the game's own key works

// The mouse's frame (FUN_10124EA0, on the game's mouse object at [0x104DFBDC]) starts by asking the manager for its hide
// mode, and while it's 1 it hides the cursor, puts the mouse back to idle (+0x4D = 0) and returns: a drag never becomes
// the camera's turn (+0x4D = 5) or the walk (4), so the mouse can't turn the camera. With [ui] hide.camera VanaCore makes
// that branch hide the cursor and go on: the cursor stays hidden (the frame's targeting and window drags want it shown),
// the drags work as with the interface up.
//
// sub esp,1Ch; push ebx; push esi; mov esi,ecx; push edi; mov ecx,<the manager>; call GetHideMode; test al,al;
// mov ecx,esi; jz; push 0; mov byte ptr [esi+4Dh],0; call ShowCursor; pop edi; pop esi; pop ebx; add esp,1Ch; ret
constexpr char kMouseFrame[] =
    "83 EC 1C 53 56 8B F1 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 8B CE 74 12 6A 00 C6 46 4D 00 E8 ?? ?? ?? ?? 5F 5E 5B 83 C4 1C C3";
constexpr std::ptrdiff_t kMouseManagerAt = 9; // the mov ecx's operand
constexpr std::ptrdiff_t kIdleAt = 0x1A;      // mov byte ptr [esi+4Dh],0
constexpr std::ptrdiff_t kReturnAt = 0x23;    // pop edi; pop esi; pop ebx; add esp,1Ch
constexpr std::uint8_t kIdle[4] = {0xC6, 0x46, 0x4D, 0x00};
constexpr std::uint8_t kReturn[4] = {0x5F, 0x5E, 0x5B, 0x83};
constexpr std::uint8_t kKeepState[4] = {0x90, 0x90, 0x90, 0x90};
constexpr std::uint8_t kGoOn[4] = {0x8B, 0xCE, 0xEB, 0x03}; // mov ecx,esi; jmp to the frame's next check (+0x2A)

// The right button's drag is the mouse's state 5 (FUN_10124CB0 takes it there once the cursor is 8 pixels from the
// press, or the button is held a moment); while it lasts the camera's turn axes (3 and 4, FUN_10122A30) are the cursor's
// distance from the press. The game's cursor is a Windows one, an arrow for the way it turns: it sets it with SetCursor
// (its window class has none). With [camera] mouse.hidecursor VanaCore takes FFXiMain's SetCursor import, keeps what the
// game last set, and sets none while the drag lasts; the game's comes back when it ends.
//
// The manager's input frame: mov ecx,[the mouse]; call <the mouse's frame>; mov ecx,<the manager>; call ...; test eax,eax;
// mov [esp+10h],eax
constexpr char kMouseCall[] = "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 10";
constexpr std::ptrdiff_t kMouseAt = 2;          // the mov ecx's operand: where the game keeps the mouse object's pointer
constexpr std::ptrdiff_t kMouseFrameCall = 6;   // call <the mouse's frame> (E8 rel32)
constexpr std::ptrdiff_t kCallManagerAt = 12;   // the mov ecx's operand
constexpr std::ptrdiff_t kMouseState = 0x4D;    // the mouse object: what it's doing
constexpr std::ptrdiff_t kMouseButtons = 0x58;  // the mouse object: the buttons held (bit 1 left, 2 right, 4 middle)
constexpr std::ptrdiff_t kMousePos = 4;         // the mouse object: x, y (int32) in the windows' own space
constexpr std::ptrdiff_t kWindowRect = 0x3A;    // a window: left, top, right, bottom (int16), the game's hit test 0x10118810
constexpr std::uint8_t kCameraDrag = 5;

using SetHideModeFn = void(__thiscall*)(void* manager, char mode);

unsigned g_key = VK_F11; // [ui] hide.key, 0 off
std::wstring g_key_name;  // as vanacore.ini has it, for the log
bool g_camera = true;     // [ui] hide.camera
bool g_camera_applied = false;
bool g_searched = false;
std::uintptr_t g_manager = 0;
SetHideModeFn g_set_hide_mode = nullptr;
std::uintptr_t g_mouse_frame = 0; // 0: not found, hide.camera does nothing
std::uintptr_t g_mouse = 0;       // where the mouse object's pointer is; 0: not found, mouse.hidecursor does nothing

bool g_hide_cursor = false;       // [camera] mouse.hidecursor
bool g_cursor_hooked = false;     // FFXiMain's SetCursor import is set_cursor (tried once)
bool g_cursor_hidden = false;     // none set while the camera drag lasts
HCURSOR g_game_cursor = nullptr;  // what the game last set
decltype(&SetCursor) g_set_cursor = nullptr;

// The manager and SetHideMode, the first time they're wanted once FFXiMain.dll is there
bool found()
{
    if (g_searched)
        return g_manager != 0;
    const HMODULE module = GetModuleHandleW(L"FFXiMain.dll");
    if (module == nullptr)
        return false;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(module)->e_lfanew);
    const std::uintptr_t call = scan::find(module, kPhaseCall);
    const std::uintptr_t set = scan::find(module, kSetHideMode);
    const std::uintptr_t manager = call != 0 ? *reinterpret_cast<const std::uintptr_t*>(call + kManagerAt) : 0;
    if (set == 0 || manager < base || manager >= base + nt->OptionalHeader.SizeOfImage) {
        log::write(L"Hide UI: couldn't find the game's menu manager; [ui] hide.key does nothing");
        return false;
    }
    g_manager = manager;
    g_set_hide_mode = reinterpret_cast<SetHideModeFn>(set);
    log::write(L"Hide UI: the game's menu manager found (+0x%X, SetHideMode +0x%X)", static_cast<unsigned>(manager - base),
               static_cast<unsigned>(set - base));
    const std::uintptr_t frame = scan::find(module, kMouseFrame);
    if (frame != 0 && *reinterpret_cast<const std::uintptr_t*>(frame + kMouseManagerAt) == manager)
        g_mouse_frame = frame;
    else
        log::write(L"Hide UI: the game's mouse frame wasn't found; [ui] hide.camera does nothing");
    const std::uintptr_t input = scan::find(module, kMouseCall);
    if (input != 0 && g_mouse_frame != 0) {
        std::int32_t rel = 0;
        std::memcpy(&rel, reinterpret_cast<const void*>(input + kMouseFrameCall + 1), 4);
        const std::uintptr_t mouse = *reinterpret_cast<const std::uintptr_t*>(input + kMouseAt);
        if (input + kMouseFrameCall + 5 + rel == g_mouse_frame &&
            *reinterpret_cast<const std::uintptr_t*>(input + kCallManagerAt) == manager && mouse > base &&
            mouse < base + nt->OptionalHeader.SizeOfImage)
            g_mouse = mouse;
    }
    if (g_mouse == 0)
        log::write(L"Hide UI: the game's mouse object wasn't found; [camera] mouse.hidecursor does nothing");
    return true;
}

void write(std::uintptr_t at, const std::uint8_t (&bytes)[4])
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_EXECUTE_READWRITE, &protect)) {
        log::write(L"Hide UI: the game's code at %p couldn't be written (error %lu)", reinterpret_cast<void*>(at), GetLastError());
        return;
    }
    std::memcpy(reinterpret_cast<void*>(at), bytes, 4);
    VirtualProtect(reinterpret_cast<void*>(at), 4, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(at), 4);
}

// The mouse's frame as hide.camera wants it. Game thread, between frames: the game isn't in that code meanwhile.
void apply_camera()
{
    if (g_camera == g_camera_applied || !found() || g_mouse_frame == 0)
        return;
    write(g_mouse_frame + kIdleAt, g_camera ? kKeepState : kIdle);
    write(g_mouse_frame + kReturnAt, g_camera ? kGoOn : kReturn);
    g_camera_applied = g_camera;
    log::write(g_camera ? L"Hide UI: the mouse turns the camera with the interface hidden ([ui] hide.camera=1)"
                        : L"Hide UI: the mouse does nothing with the interface hidden, as in the game ([ui] hide.camera=0)");
}

// The game's SetCursor, from FFXiMain: kept, and set unless the camera drag has the cursor hidden
HCURSOR WINAPI set_cursor(HCURSOR cursor)
{
    const HCURSOR before = g_game_cursor;
    g_game_cursor = cursor;
    return g_cursor_hidden ? before : g_set_cursor(cursor);
}

bool camera_drag()
{
    const auto mouse = *reinterpret_cast<const std::uintptr_t*>(g_mouse);
    return mouse != 0 && *reinterpret_cast<const std::uint8_t*>(mouse + kMouseState) == kCameraDrag;
}

// The cursor hidden while the right button turns the camera, and the game's back after. Game thread (the window's, so
// SetCursor reaches it).
void update_cursor()
{
    if (!g_cursor_hooked && g_hide_cursor && found() && g_mouse != 0) {
        g_cursor_hooked = true;
        if (const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
            game == nullptr || !imports::hook(game, "USER32.dll", "SetCursor", &set_cursor, g_set_cursor)) {
            g_set_cursor = nullptr;
            log::write(L"Hide UI: the game's SetCursor import couldn't be hooked; [camera] mouse.hidecursor does nothing");
        } else {
            g_game_cursor = GetCursor();
        }
    }
    if (g_set_cursor == nullptr)
        return;
    const bool hide = g_hide_cursor && camera_drag();
    if (hide)
        g_set_cursor(nullptr); // each frame: anything else that sets one meanwhile (an ImGui window) is undone
    else if (g_cursor_hidden)
        g_set_cursor(g_game_cursor);
    g_cursor_hidden = hide;
}

bool in_world()
{
    const std::int32_t phase = *reinterpret_cast<const std::int32_t*>(g_manager + kPhase);
    for (const std::int32_t world : kWorldPhases)
        if (phase == world)
            return true;
    return false;
}

// Hidden: shown again (anywhere, so it can't stay hidden where the game's key doesn't work). Shown: hidden, in the world.
void toggle()
{
    if (hidden())
        g_set_hide_mode(reinterpret_cast<void*>(g_manager), 0);
    else if (in_world())
        g_set_hide_mode(reinterpret_cast<void*>(g_manager), 1);
}

} // namespace

void reload(const wchar_t* ini)
{
    g_camera = GetPrivateProfileIntW(L"ui", L"hide.camera", 1, ini) != 0; // taken up by on_frame
    g_hide_cursor = GetPrivateProfileIntW(L"camera", L"mouse.hidecursor", 0, ini) != 0;
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"ui", L"hide.key", L"f11", text, 64, ini);
    if (g_key_name == text)
        return;
    g_key_name = text;
    std::string name;
    for (const wchar_t* c = text; *c != L'\0'; ++c)
        name += *c < 0x80 ? static_cast<char>(*c) : '?';
    if (name.empty() || _stricmp(name.c_str(), "off") == 0 || name == "0") {
        g_key = 0;
        log::write(L"Hide UI: off ([ui] hide.key=%ls)", text);
        return;
    }
    g_key = binds::key_code(name);
    if (g_key == 0) {
        g_key = VK_F11;
        log::write(L"Hide UI: [ui] hide.key=%ls isn't a key VanaCore knows (f11, scrolllock, numpad0, ...); F11", text);
        return;
    }
    log::write(L"Hide UI: %ls hides the game's interface and shows it again ([ui] hide.key)", text);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_camera != g_camera_applied)
        apply_camera();
    update_cursor();
}

bool on_key(UINT message, WPARAM wparam, LPARAM lparam)
{
    const unsigned key = binds::message_key(static_cast<unsigned>(wparam), static_cast<unsigned>(lparam >> 16) & 0xFF,
                                            (lparam & (1 << 24)) != 0);
    if (g_key == 0 || key != g_key || !found())
        return false;
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (down && (lparam & (1 << 30)) == 0)
        toggle();
    return true;
}

bool hidden()
{
    return found() && *reinterpret_cast<const std::uint8_t*>(g_manager + kHideMode) == 1;
}

bool set_hidden(bool hide)
{
    if (!found())
        return false;
    if (hide != hidden())
        toggle();
    return hidden();
}

bool mouse_over_window()
{
    if (!found() || g_mouse == 0)
        return false;
    const auto mouse = *reinterpret_cast<const std::uintptr_t*>(g_mouse);
    if (mouse == 0)
        return false;
    // +0x4D: 3 dragging a window's scroll bar, 6 a middle-button scroll in one. 2 is only a menu up, wherever the cursor is
    // (FUN_10124E00 asks HaveActiveWindow), so the cursor is tested against the current window's rect as the game does
    const std::uint8_t state = *reinterpret_cast<const std::uint8_t*>(mouse + kMouseState);
    if (state == 3 || state == 6)
        return true;
    if (!window_up())
        return false;
    const auto current = *reinterpret_cast<const std::uintptr_t*>(g_manager + kCurrent);
    const auto* pos = reinterpret_cast<const std::int32_t*>(mouse + kMousePos);
    const auto* rect = reinterpret_cast<const std::int16_t*>(current + kWindowRect);
    return pos[0] >= rect[0] && pos[0] <= rect[2] && pos[1] >= rect[1] && pos[1] <= rect[3];
}

bool window_up()
{
    if (!found())
        return true;
    const auto current = *reinterpret_cast<const std::uintptr_t*>(g_manager + kCurrent);
    if (current == 0)
        return false;
    // HaveActiveWindow: not the id 0xE control, nor a window about to be deleted
    return *reinterpret_cast<const std::int32_t*>(current + kCtrlId) != kNoWindowId &&
           *reinterpret_cast<const std::uint8_t*>(current + kWishDelete) == 0;
}

unsigned game_mouse_buttons()
{
    if (!found() || g_mouse == 0)
        return 0;
    const auto mouse = *reinterpret_cast<const std::uintptr_t*>(g_mouse);
    if (mouse == 0)
        return 0;
    return *reinterpret_cast<const std::uint32_t*>(mouse + kMouseButtons) & 7u;
}

void clear_game_mouse_buttons()
{
    if (!found() || g_mouse == 0)
        return;
    const auto mouse = *reinterpret_cast<const std::uintptr_t*>(g_mouse);
    if (mouse == 0)
        return;
    // +0x58 held, +0x5C repeat, +0x60 pressed (bit 1 left, 2 right, 4 middle)
    for (const std::ptrdiff_t at : {kMouseButtons, kMouseButtons + 4, kMouseButtons + 8})
        *reinterpret_cast<std::uint32_t*>(mouse + at) = 0;
}

} // namespace vanacore::hideui
