#include "vanaui.hpp"

#include <imgui_internal.h> // GetWindowResizeCornerID: whether ImGui's resize grip is under the mouse or held
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "layout.hpp"

namespace vanacore::vanaui {

const char* const kIconStats = "\xEE\x91\xB3";  // chart-simple U+E473
const char* const kIconGraph = "\xEF\x88\x81";  // chart-line U+F201
const char* const kIconAddon = "\xEF\x84\xAE";  // puzzle-piece U+F12E
const char* const kIconClose = "\xEF\x80\x8D";  // xmark U+F00D
const char* const kIconCheck = "\xEF\x80\x8C";  // check U+F00C
const char* const kIconSearch = "\xEF\x80\x82"; // magnifying-glass U+F002
const char* const kIconLeft = "\xEF\x81\x93";   // chevron-left U+F053
const char* const kIconRight = "\xEF\x81\x94";  // chevron-right U+F054
const char* const kIconDown = "\xEF\x81\xB8";   // chevron-down U+F078
const char* const kIconSave = "\xEF\x83\x87";   // floppy-disk U+F0C7
const char* const kIconTrash = "\xEF\x87\xB8";  // trash U+F1F8
const char* const kIconPause = "\xEF\x81\x8C";  // pause U+F04C
const char* const kIconPlay = "\xEF\x81\x8B";   // play U+F04B
const char* const kIconDot = "";

float g_scale = 1.0f;
Fonts g_fonts;

namespace {

std::string narrow(const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

ImFont* add_font(const wchar_t* file)
{
    wchar_t windows[MAX_PATH] = {};
    if (GetWindowsDirectoryW(windows, MAX_PATH) == 0)
        return nullptr;
    const std::wstring path = std::wstring(windows) + L"\\Fonts\\" + file;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return nullptr;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const bool locked = atlas->Locked;
    atlas->Locked = false; // as lua_imgui.cpp's AddFontFromFileTTF, mid-frame
    ImFont* font = atlas->AddFontFromFileTTF(narrow(path).c_str(), 18.0f);
    atlas->Locked = locked;
    return font;
}

constexpr int kThemeColours = 41, kThemeVars = 14;

} // namespace

float S(float n)
{
    return std::floor(n * g_scale + 0.5f);
}

ImU32 u32(unsigned rgb, float alpha)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
}

ImVec4 vec4(unsigned rgb, float alpha)
{
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

unsigned mix(unsigned a, unsigned b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    unsigned out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float x = static_cast<float>((a >> shift) & 0xFF), y = static_cast<float>((b >> shift) & 0xFF);
        out |= static_cast<unsigned>(x + (y - x) * t + 0.5f) << shift;
    }
    return out;
}

float ease(const char* key, float target, float speed)
{
    float* value = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(key), target);
    *value += (target - *value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    if (std::fabs(*value - target) < 0.003f)
        *value = target;
    return *value;
}

void load_fonts()
{
    if (g_fonts.frame >= 0)
        return;
    g_fonts.frame = ImGui::GetFrameCount();
    g_fonts.text = add_font(L"segoeui.ttf");
    g_fonts.semi = add_font(L"seguisb.ttf");
    if (g_fonts.semi == nullptr)
        g_fonts.semi = g_fonts.text;
}

ImFont* ready(ImFont* font)
{
    return font != nullptr && font->IsLoaded() && ImGui::GetFrameCount() > g_fonts.frame ? font : nullptr;
}

void push_font(ImFont* font, float size)
{
    ImGui::PushFont(ready(font), S(size));
}

void push_icons(float size)
{
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImGui::PushFont(atlas->Fonts.Size > 0 ? atlas->Fonts[0] : nullptr, S(size));
}

float text_width(const char* text, ImFont* font, float size)
{
    push_font(font, size);
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    return w;
}

void text_mid(ImDrawList* dl, float x, float cy, const char* text, unsigned rgb, ImFont* font, float size, float alpha)
{
    push_font(font, size);
    const float h = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(std::floor(x), std::floor(cy - h * 0.5f - std::floor(h * 0.06f + 0.5f))), u32(rgb, alpha), text);
    ImGui::PopFont();
}

void text_right_mid(ImDrawList* dl, float right, float cy, const char* text, unsigned rgb, ImFont* font, float size)
{
    text_mid(dl, right - text_width(text, font, size), cy, text, rgb, font, size);
}

void icon(ImDrawList* dl, float cx, float cy, const char* glyph, unsigned rgb, float size, float alpha)
{
    push_icons(size);
    const ImVec2 sz = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(std::floor(cx - sz.x * 0.5f), std::floor(cy - sz.y * 0.5f)), u32(rgb, alpha), glyph);
    ImGui::PopFont();
}

void push_theme()
{
    g_scale = ImGui::GetFontSize() / kBase;
    load_fonts();
    const auto colour = [](ImGuiCol which, unsigned rgb, float alpha = 1.0f) { ImGui::PushStyleColor(which, vec4(rgb, alpha)); };
    colour(ImGuiCol_Text, P::text);
    colour(ImGuiCol_TextDisabled, P::dim);
    colour(ImGuiCol_WindowBg, P::bg);
    colour(ImGuiCol_ChildBg, P::bg, 0.0f);
    colour(ImGuiCol_PopupBg, P::popup);
    colour(ImGuiCol_Border, P::border);
    colour(ImGuiCol_BorderShadow, P::bg, 0.0f);
    colour(ImGuiCol_FrameBg, P::well);
    colour(ImGuiCol_FrameBgHovered, P::hover);
    colour(ImGuiCol_FrameBgActive, P::hover);
    colour(ImGuiCol_TitleBg, P::side);
    colour(ImGuiCol_TitleBgActive, P::side);
    colour(ImGuiCol_ScrollbarBg, P::bg, 0.0f);
    colour(ImGuiCol_ScrollbarGrab, P::grab);
    colour(ImGuiCol_ScrollbarGrabHovered, P::grab_hi);
    colour(ImGuiCol_ScrollbarGrabActive, P::accent_lo);
    colour(ImGuiCol_CheckMark, P::accent);
    colour(ImGuiCol_SliderGrab, P::accent);
    colour(ImGuiCol_SliderGrabActive, P::accent2);
    colour(ImGuiCol_Button, P::seg);
    colour(ImGuiCol_ButtonHovered, 0x3D4463);
    colour(ImGuiCol_ButtonActive, P::accent_lo);
    colour(ImGuiCol_Header, P::accent_bg);
    colour(ImGuiCol_HeaderHovered, P::hover);
    colour(ImGuiCol_HeaderActive, P::accent_lo);
    colour(ImGuiCol_Separator, P::line);
    colour(ImGuiCol_ResizeGrip, P::bg, 0.0f); // ImGui's grip unseen: end_window draws the corner's lines over it
    colour(ImGuiCol_ResizeGripHovered, P::bg, 0.0f);
    colour(ImGuiCol_ResizeGripActive, P::bg, 0.0f);
    colour(ImGuiCol_Tab, P::bg, 0.0f);
    colour(ImGuiCol_TabHovered, P::hover);
    colour(ImGuiCol_TabSelected, P::accent_bg);
    colour(ImGuiCol_TabSelectedOverline, P::accent);
    colour(ImGuiCol_PlotLines, P::accent);
    colour(ImGuiCol_PlotHistogram, P::accent);
    colour(ImGuiCol_TableHeaderBg, P::side);
    colour(ImGuiCol_TableBorderStrong, P::border);
    colour(ImGuiCol_TableBorderLight, P::line);
    colour(ImGuiCol_TableRowBg, P::bg, 0.0f);
    colour(ImGuiCol_TableRowBgAlt, P::white, 0.025f);
    colour(ImGuiCol_TextSelectedBg, P::accent, 0.35f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(8))); // the tooltips'; a window here sets its own
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(5));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(6), S(3)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, S(5));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, S(14));
}

void pop_theme()
{
    ImGui::PopStyleVar(kThemeVars);
    ImGui::PopStyleColor(kThemeColours);
}

bool begin_window(const char* name, ImGuiWindowFlags flags)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(kPad), S(14)));
    layout::clip_begin(name);
    const bool shown = ImGui::Begin(name, nullptr, flags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                                       ImGuiWindowFlags_NoNavInputs);
    layout::clip_end();
    ImGui::PopStyleVar();
    if (shown)
        push_font(g_fonts.text, kText);
    return shown;
}

void title_bar(const char* glyph, const char* title, const char* command, bool& open)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), pad = S(kPad), cy = pos.y + S(kTitleH) * 0.5f;
    const float close_x = pos.x + w - pad;
    icon(dl, pos.x + pad + S(6), cy, glyph, P::accent, 13.0f);
    text_mid(dl, pos.x + pad + S(20), cy, title, P::white, g_fonts.semi, kBody + 2.0f);
    if (command != nullptr && close_x - S(22) - text_width(command, g_fonts.text, kSmall) > pos.x + pad + S(20) + text_width(title, g_fonts.semi, kBody + 2.0f) + S(16))
        text_right_mid(dl, close_x - S(22), cy + S(1), command, P::faint, g_fonts.text, kSmall);
    ImGui::SetCursorScreenPos(ImVec2(close_x - S(11), cy - S(11)));
    if (ImGui::InvisibleButton("##close", ImVec2(S(22), S(22))))
        open = false;
    const float lit = ease("##close.lit", ImGui::IsItemHovered() ? 1.0f : 0.0f, 22.0f);
    icon(dl, close_x, cy, kIconClose, mix(P::dim, P::rose, lit), 13.0f);
    dl->AddLine(ImVec2(pos.x, pos.y + S(kTitleH)), ImVec2(pos.x + w, pos.y + S(kTitleH)), u32(P::line));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + S(kTitleH) + S(12)));
    ImGui::BeginChild("##body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
}

// The resize corner, bottom right, as API Explorer's and the /vana window's: three short lines across it, lit while ImGui's
// own grip there (unseen: push_theme's colours) is under the mouse or held. In the window's margin, clear of its body.
static void resize_corner()
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if ((window->Flags & ImGuiWindowFlags_NoResize) != 0)
        return;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiID id = ImGui::GetWindowResizeCornerID(window, 0);
    const bool held = g.ActiveId == id, lit = held || g.HoveredId == id;
    ImDrawList* dl = window->DrawList;
    const ImVec2 pos = window->Pos, end(pos.x + window->Size.x, pos.y + window->Size.y);
    const float right = end.x - S(5), bottom = end.y - S(5);
    dl->PushClipRect(pos, end, false);
    for (const float k : {4.0f, 8.0f, 12.0f})
        dl->AddLine(ImVec2(right - S(k), bottom), ImVec2(right, bottom - S(k)), u32(lit ? P::accent : P::dim), std::max(1.0f, S(1.25f)));
    dl->PopClipRect();
    if (lit && !held)
        ImGui::SetTooltip("Drag to resize");
}

void end_window(bool shown)
{
    if (shown) {
        ImGui::EndChild();
        resize_corner();
        ImGui::PopFont();
    }
    ImGui::End();
}

float toggle_width(const char* label)
{
    return S(32) + S(8) + text_width(label, g_fonts.text, kSmall + 0.5f);
}

bool toggle(const char* id, const char* label, bool& on, float height)
{
    ImGui::PushID(id);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = S(32), h = S(17), row = S(height);
    const bool clicked = ImGui::InvisibleButton("##toggle", ImVec2(toggle_width(label), row));
    const bool hovered = ImGui::IsItemHovered();
    if (clicked)
        on = !on;
    const float t = ease("##toggle.on", on ? 1.0f : 0.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = at.y + row * 0.5f, y = std::floor(cy - h * 0.5f);
    dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + h), u32(mix(P::off, P::accent, t)), h * 0.5f);
    if (hovered)
        dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + h), u32(P::white, 0.08f), h * 0.5f);
    const float r = (h - S(4)) * 0.5f;
    dl->AddCircleFilled(ImVec2(at.x + S(2) + r + (w - S(4) - r * 2.0f) * t, y + h * 0.5f), r, u32(P::knob));
    text_mid(dl, at.x + w + S(8), cy, label, hovered ? P::text : P::text2, g_fonts.text, kSmall + 0.5f);
    ImGui::PopID();
    return clicked;
}

float button_width(const char* label, const char* glyph, float height)
{
    return text_width(label, g_fonts.text, kSmall + 0.5f) + (glyph != nullptr ? S(16) : 0.0f) + S(20) + (height > 26.0f ? S(8) : 0.0f);
}

bool button(const char* id, const char* label, const char* glyph, Kind kind, float height, unsigned glyph_rgb, float glyph_alpha,
            float min_width)
{
    ImGui::PushID(id);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = std::max(button_width(label, glyph, height), min_width), h = S(height), radius = S(4);
    const bool clicked = ImGui::InvisibleButton("##button", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const float t = ease("##button.lit", hovered ? 1.0f : 0.0f, 22.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + w, at.y + h);
    unsigned fg = mix(P::text2, P::text, t);
    if (kind == Kind::danger || kind == Kind::success) {
        const unsigned tint = kind == Kind::danger ? P::rose : P::green;
        dl->AddRectFilled(at, end, u32(mix(P::well, tint, 0.10f + 0.10f * t)), radius);
        dl->AddRect(at, end, u32(tint, 0.45f + 0.25f * t), radius);
        fg = mix(tint, P::white, t * 0.35f);
    } else if (kind == Kind::accent) {
        dl->AddRectFilled(at, end, u32(mix(P::accent, P::accent2, t * 0.6f)), radius);
        dl->AddRect(at, end, u32(P::white, 0.22f), radius);
        fg = P::ink;
    } else {
        dl->AddRectFilled(at, end, u32(mix(P::well, P::hover, t)), radius);
        dl->AddRect(at, end, u32(mix(P::well_edge, P::accent_lo, t)), radius);
    }
    if (active)
        dl->AddRectFilled(at, end, u32(0x000000, 0.15f), radius);
    const float content = text_width(label, g_fonts.text, kSmall + 0.5f) + (glyph != nullptr ? S(16) : 0.0f);
    float x = at.x + std::floor((w - content) * 0.5f);
    const float cy = at.y + h * 0.5f;
    if (glyph != nullptr && glyph[0] == '\0') // kIconDot
        dl->AddCircleFilled(ImVec2(x + S(5), cy), S(4.5f), u32(glyph_rgb != 0 ? glyph_rgb : fg, glyph_alpha));
    else if (glyph != nullptr)
        icon(dl, x + S(5), cy + S(1), glyph, glyph_rgb != 0 ? glyph_rgb : fg, 12.0f, glyph_alpha);
    if (glyph != nullptr)
        x += S(16);
    text_mid(dl, x, cy, label, fg, g_fonts.text, kSmall + 0.5f);
    ImGui::PopID();
    return clicked;
}

void headers_row()
{
    push_font(g_fonts.semi, kSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(P::dim));
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void meter(double fraction, ImU32 fill)
{
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, line = ImGui::GetTextLineHeight(), h = S(5);
    const float y = std::floor(at.y + (line - h) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + h), u32(P::off), h * 0.5f);
    const float f = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    if (f > 0.0f)
        dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + std::max(h, w * f), y + h), fill, h * 0.5f);
    ImGui::Dummy(ImVec2(w, line));
}

void reading_column(ImDrawList* dl, float x, float cy, float width, const char* name, const char* value, double fraction, bool hot)
{
    const float ty = cy - S(8);
    text_mid(dl, x, ty, name, P::dim, g_fonts.text, kSmall);
    text_right_mid(dl, x + width, ty, value, P::text, g_fonts.semi, kSmall);
    const float by = cy + S(5), bh = S(4);
    const float f = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
    const unsigned colour = (hot || f >= 0.95f) ? P::rose : f >= 0.85f ? P::amber : P::accent;
    dl->AddRectFilled(ImVec2(x, by), ImVec2(x + width, by + bh), u32(P::off), bh * 0.5f);
    if (f > 0.0f)
        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + std::max(bh, width * f), by + bh), u32(colour), bh * 0.5f);
}

} // namespace vanacore::vanaui
