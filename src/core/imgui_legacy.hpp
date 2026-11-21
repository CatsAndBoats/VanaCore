#pragma once

#include <windows.h>

#include <imgui.h>

// ImGui 1.81's numbers (Ashita 4.16's: its plugins, and addons that load its libs\imgui.lua) as 1.92's, VanaCore's:
// flags, colours, style variables and the rest, matched by name (imgui_legacy.inc, tools/gen_ashita_gui.py), and keys.
namespace vanacore::imgui_legacy {

#include "imgui_legacy.inc"

// A Win32 key code as ImGui's key (imgui_impl_win32's mapping), or ImGuiKey_None.
inline ImGuiKey vk_key(int vk)
{
    if (vk >= '0' && vk <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F24)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_CAPITAL: return ImGuiKey_CapsLock;
    case VK_SCROLL: return ImGuiKey_ScrollLock;
    case VK_NUMLOCK: return ImGuiKey_NumLock;
    case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
    case VK_PAUSE: return ImGuiKey_Pause;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_MENU: case VK_LMENU: return ImGuiKey_LeftAlt;
    case VK_LWIN: return ImGuiKey_LeftSuper;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_RMENU: return ImGuiKey_RightAlt;
    case VK_RWIN: return ImGuiKey_RightSuper;
    case VK_APPS: return ImGuiKey_Menu;
    case VK_BROWSER_BACK: return ImGuiKey_AppBack;
    case VK_BROWSER_FORWARD: return ImGuiKey_AppForward;
    default: return ImGuiKey_None;
    }
}

// A key as the Win32 key code vk_key gives it for (Shift, Ctrl and Alt as either side's), or 0.
inline int key_vk(ImGuiKey key)
{
    switch (key) {
    case ImGuiKey_LeftShift: case ImGuiKey_RightShift: return VK_SHIFT;
    case ImGuiKey_LeftCtrl: case ImGuiKey_RightCtrl: return VK_CONTROL;
    case ImGuiKey_LeftAlt: case ImGuiKey_RightAlt: return VK_MENU;
    case ImGuiKey_KeypadEnter: return VK_RETURN;
    default: break;
    }
    for (int vk = 1; vk < 256; ++vk)
        if (vk_key(vk) == key)
            return vk;
    return 0;
}

// A key argument: ImGui 1.92's key (ImGuiKey_NamedKey_BEGIN and up), or, below that, a Win32 key code, what 1.81's
// IsKeyDown and the like took (Ashita's io.KeyMap is Win32's key codes).
inline ImGuiKey key_in(int v)
{
    return v > 0 && v < ImGuiKey_NamedKey_BEGIN ? vk_key(v) : static_cast<ImGuiKey>(v);
}

// Whether a key is one ImGui::GetKeyData accepts: a named key or a modifier. An addon or plugin can ask about a
// Win32 code vk_key has no key for (mouse buttons, IME keys), or a raw number, which key_in leaves as ImGuiKey_None
// or an out-of-range value; passing that to ImGui's IsKeyDown and the like reads out of bounds (GetKeyData only
// asserts). The key-query wrappers below check first and read as "not pressed", as ImGui 1.81 did.
inline bool key_ok(ImGuiKey key)
{
    return (key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END) || (static_cast<int>(key) & ImGuiMod_Mask_);
}
inline bool is_key_down(ImGuiKey key) { return key_ok(key) && ImGui::IsKeyDown(key); }
inline bool is_key_pressed(ImGuiKey key, bool repeat) { return key_ok(key) && ImGui::IsKeyPressed(key, repeat); }
inline bool is_key_released(ImGuiKey key) { return key_ok(key) && ImGui::IsKeyReleased(key); }
inline int key_pressed_amount(ImGuiKey key, float repeat_delay, float rate)
{
    return key_ok(key) ? ImGui::GetKeyPressedAmount(key, repeat_delay, rate) : 0;
}

// 1.81's ImGuiKey_ (Tab 0 ... Z 21: GetKeyIndex, io.KeyMap) as the Win32 key code Ashita 4.16 mapped it to.
inline int legacy_key_vk(int key)
{
    static constexpr int kVk[] = {VK_TAB, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT, VK_HOME, VK_END,
                                  VK_INSERT, VK_DELETE, VK_BACK, VK_SPACE, VK_RETURN, VK_ESCAPE, VK_RETURN,
                                  'A', 'C', 'V', 'X', 'Y', 'Z'};
    return key >= 0 && key < static_cast<int>(sizeof kVk / sizeof kVk[0]) ? kVk[key] : -1;
}
inline constexpr int kLegacyKeyCount = 22; // 1.81's ImGuiKey_COUNT

// A key as 1.81's ImGuiKey_ (an InputText callback's EventKey), or -1 for one 1.81 had no name for.
inline int legacy_key_of(ImGuiKey key)
{
    if (key == ImGuiKey_KeypadEnter)
        return 15;
    for (int i = 0; i < kLegacyKeyCount; ++i)
        if (vk_key(legacy_key_vk(i)) == key)
            return i;
    return -1;
}

// 1.81's ImDrawCornerFlags_All, the default corners of AddRect and the like.
inline constexpr int kLegacyCornersAll = 0xF;

} // namespace vanacore::imgui_legacy
