#pragma once
// The look both VanaCoreApp.exe (its splash) and VanaCoreApp.dll (its views) draw with: the .vcaddon card's
// (src\core\addonpkg_win.cpp), the /vana palette on a card with round corners and a soft shadow, Direct2D and DirectWrite
// into a layered window. Compiled into each of them (no DLL between them to share it from).

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vcapp::ui {

// The /vana palette (draw.lua's M.P)
namespace pal {
constexpr std::uint32_t bg = 0x161822, border = 0x2A2E3E, line = 0x242838, well = 0x0E1017, well_edge = 0x272B3C,
                        hover = 0x1F2334, text = 0xE8EAF1, white = 0xFFFFFF, text2 = 0xBDC1D0, dim = 0x7B8095,
                        faint = 0x4E536A, accent = 0x7AA2F7, accent2 = 0xA9C1FF, accent_lo = 0x3E5CB3, teal = 0x5FD3B3,
                        amber = 0xE8B45C, rose = 0xEE6F78, ink = 0x0B1020;
}

D2D1_COLOR_F rgb(std::uint32_t c, float a = 1.0f);
D2D1_COLOR_F mix(std::uint32_t a, std::uint32_t b, float t, float alpha = 1.0f);

enum class Font { Title, Big, Body, Small, Label, Button, Count };
enum class Button { Accent, Secondary, Ghost };

constexpr float kShadow = 16, kRadius = 10; // the room round the card for its shadow, its corners (DIPs)
constexpr UINT kRedraw = WM_APP + 0x40;     // posted to the window when the drawing had to be made again (draw once more)

// What's under the mouse: each frame's controls and where they are, in the card's DIPs
template <class Id> struct Hits {
    std::vector<std::pair<Id, D2D1_RECT_F>> list;
    void clear() { list.clear(); }
    void add(Id id, const D2D1_RECT_F& r) { list.push_back({id, r}); }
    Id at(D2D1_POINT_2F p, Id none) const
    {
        for (const auto& [id, r] : list)
            if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom)
                return id;
        return none;
    }
};

class Canvas {
public:
    Canvas() = default;
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // Direct2D, DirectWrite and the logo (the RCDATA resource LOGO in `module`). False: there's no Direct2D to draw with.
    bool start(HMODULE module);
    // The layered window it draws into and the card's size (DIPs): the window sized for its DPI, centred on its screen
    void attach(HWND window, float width, float height);
    // WM_DPICHANGED: sized again for it, kept centred where it is
    void dpi_changed(UINT dpi);

    // A frame: begin, draw in DIPs from the card's top left, end (it's shown). False from begin: nothing to draw into.
    bool begin();
    void end();
    // A point in the window's client pixels, in the card's DIPs
    D2D1_POINT_2F to_card(int x, int y) const;
    float width() const { return width_; }
    float height() const { return height_; }

    // The card: its shadow, its page, the head's tint `head` tall (0: none) and the hairline round it
    void card(float head);
    void fill(float x, float y, float w, float h, float r, D2D1_COLOR_F colour);
    void outline(float x, float y, float w, float h, float r, D2D1_COLOR_F colour);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F colour, float width = 1.0f);
    void disc(float cx, float cy, float r, D2D1_COLOR_F colour);
    void clip(const D2D1_RECT_F& r);
    void unclip();
    float text_width(const std::wstring& text, Font font) const;
    // A line of text from `x`, its middle at `cy`; and one centred on `cx`
    void text(const std::wstring& text, Font font, float x, float cy, D2D1_COLOR_F colour);
    void text_centred(const std::wstring& text, Font font, float cx, float cy, D2D1_COLOR_F colour);
    // `text` cut to `room` with an ellipsis: at its end, or at its start (a path: its file's name shows)
    std::wstring fit(const std::wstring& text, Font font, float room, bool from_start) const;
    void logo(float x, float y, float size);
    // The close X, rose under the mouse; its hit area is 13 round its middle
    void close_x(float cx, float cy, bool hover);
    // A button in `r`; `t` 0 at rest, 0.6 under the mouse, 1 pressed; `on` false: dimmed
    void button(const D2D1_RECT_F& r, const std::wstring& label, Button style, float t, bool on = true);
    float button_width(const std::wstring& label) const;

private:
    bool make_target();
    void load_logo(HMODULE module);
    void size_window(bool centre);
    IDWriteTextLayout* layout(const std::wstring& text, Font font) const;

    bool com_ = false;
    HWND window_ = nullptr;
    float width_ = 0, height_ = 0, scale_ = 1.0f;
    int pixel_w_ = 0, pixel_h_ = 0;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1Bitmap* logo_ = nullptr;
    std::vector<std::uint32_t> logo_pixels_;
    UINT logo_w_ = 0, logo_h_ = 0;
    IDWriteTextFormat* fonts_[static_cast<int>(Font::Count)] = {};
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
};

} // namespace vcapp::ui
