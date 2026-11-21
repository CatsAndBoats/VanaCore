#include "uispace.hpp"

#include <cstddef>
#include <cstdint>

#include "graphics.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::uispace {
namespace {

// The menus' setup (FUN_10163b30) sets the area, a short each: xor ebx,ebx; push 620h; mov [left],bx; mov [top],bx;
// mov [right],ax; mov [bottom],si; call operator new. Nothing hooks it, unlike the chat's layout (FUN_10161850, which
// chat.move hooks), whose mov eax,[right] at its start the old lookup read.
constexpr char kSetup[] = "33 DB 68 20 06 00 00 66 89 1D ?? ?? ?? ?? 66 89 1D ?? ?? ?? ?? 66 A3 ?? ?? ?? ?? "
                          "66 89 35 ?? ?? ?? ?? E8";
constexpr std::ptrdiff_t kLeftAt = 0xA, kTopAt = 0x11, kRightAt = 0x17, kBottomAt = 0x1E;

bool g_tried = false;
const short* g_origin = nullptr; // left, top
const short* g_end = nullptr;    // right, bottom

const short* operand(std::uintptr_t at) { return *reinterpret_cast<const short* const*>(at); }

} // namespace

bool find(HMODULE game)
{
    if (g_tried || game == nullptr)
        return g_end != nullptr;
    g_tried = true;
    const std::uintptr_t at = scan::find(game, kSetup);
    // each pair a left-and-top (right-and-bottom) the code reads as one
    if (at == 0 || operand(at + kTopAt) != operand(at + kLeftAt) + 1 ||
        operand(at + kBottomAt) != operand(at + kRightAt) + 1) {
        log::write(L"Interface: the game's interface area wasn't found in FFXiMain.dll: VanaCore's text over its windows "
                   L"(items.count.font=sharp, network.icon=dot) isn't drawn");
        return false;
    }
    g_origin = operand(at + kLeftAt);
    g_end = operand(at + kRightAt);
    return true;
}

// [ui] aspect squeezes the game's interface toward the middle only when its own target has the aspect's shape
// (graphics::ui_squeezed, given the interface's ui_width x ui_height): with a shape the interface doesn't have, the
// renderer leaves it filling the window, and a point mapped as if it were squeezed lands away from the window it
// belongs to (the Network window's dot 240 pixels under "Connected" with 21:9 and a 1920x1200 resolution.ui)
namespace {
void squeeze(float w, float h, int ui_width, int ui_height, float& squeeze_x, float& squeeze_y)
{
    squeeze_x = 1.0f, squeeze_y = 1.0f;
    if (const float aspect = graphics::ui_aspect();
        aspect > 0 && graphics::ui_squeezed(static_cast<unsigned>(ui_width), static_cast<unsigned>(ui_height))) {
        squeeze_x = aspect * h / w < 1.0f ? aspect * h / w : 1.0f;
        squeeze_y = w / (aspect * h) < 1.0f ? w / (aspect * h) : 1.0f;
    }
}
} // namespace

bool to_pixels(float x, float y, unsigned width, unsigned height, float& px, float& py, float& scale)
{
    if (g_end == nullptr)
        return false;
    const int ui_width = g_end[0] - g_origin[0], ui_height = g_end[1] - g_origin[1];
    if (ui_width <= 0 || ui_height <= 0 || width == 0 || height == 0)
        return false;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    float squeeze_x, squeeze_y;
    squeeze(w, h, ui_width, ui_height, squeeze_x, squeeze_y);
    scale = h / ui_height * squeeze_y;
    px = w * 0.5f + ((x - g_origin[0]) * w / ui_width - w * 0.5f) * squeeze_x;
    py = h * 0.5f + ((y - g_origin[1]) * h / ui_height - h * 0.5f) * squeeze_y;
    return true;
}

bool to_pixels_plain(float x, float y, unsigned width, unsigned height, float& px, float& py, float& scale)
{
    if (g_end == nullptr)
        return false;
    const int ui_width = g_end[0] - g_origin[0], ui_height = g_end[1] - g_origin[1];
    if (ui_width <= 0 || ui_height <= 0 || width == 0 || height == 0)
        return false;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    scale = h / ui_height;
    px = (x - g_origin[0]) * w / ui_width;
    py = (y - g_origin[1]) * h / ui_height;
    return true;
}

bool to_units(float px, float py, unsigned width, unsigned height, float& x, float& y)
{
    if (g_end == nullptr)
        return false;
    const int ui_width = g_end[0] - g_origin[0], ui_height = g_end[1] - g_origin[1];
    if (ui_width <= 0 || ui_height <= 0 || width == 0 || height == 0)
        return false;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    float squeeze_x, squeeze_y;
    squeeze(w, h, ui_width, ui_height, squeeze_x, squeeze_y);
    x = g_origin[0] + ((px - w * 0.5f) / squeeze_x + w * 0.5f) * ui_width / w;
    y = g_origin[1] + ((py - h * 0.5f) / squeeze_y + h * 0.5f) * ui_height / h;
    return true;
}

bool area(int& left, int& top, int& right, int& bottom)
{
    if (g_end == nullptr || g_end[0] <= g_origin[0] || g_end[1] <= g_origin[1])
        return false;
    left = g_origin[0], top = g_origin[1], right = g_end[0], bottom = g_end[1];
    return true;
}

} // namespace vanacore::uispace
