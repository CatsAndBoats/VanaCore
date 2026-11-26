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

// Segoe UI's, then the icons' (Material Symbols Outlined, res\symbols.ttf; none on Windows before 10 1703: no icons drawn)
enum class Font { Title, Big, Body, Small, Label, Button, Play, Status, Icon, IconBig, Count }; // (Status: START's font, smaller)

// Material Symbols' codepoints, the icons in res\symbols.ttf (res\make_symbols.py's ICONS: add one there, then here)
namespace icon {
constexpr wchar_t play = 0xE037, undo = 0xE166, download = 0xF090, check = 0xF0BE, error = 0xF8B6, info = 0xE88E, sync = 0xE627,
                  close = 0xE5CD, settings = 0xE8B8, folder = 0xE2C8, up = 0xE316, down = 0xE313, gamepad = 0xEA28,
                  open = 0xE89E, cancel = 0xE888, edit = 0xF097, add = 0xE145, trash = 0xE92E, circle = 0xE836, shortcut = 0xF57A;
}
enum class Button { Accent, Secondary, Ghost, Play }; // (Play: the home's big one, a blue gradient in a cyan border)

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
    // The background picture (the RCDATA resource BACKDROP; none: nothing) covering the card, under a dark tint, deeper at the
    // top and the bottom; and `glass` (none: no glass) frosted, the picture blurred under it. After card().
    void backdrop(const D2D1_RECT_F* glass = nullptr);
    // An icon (ui::icon's), its middle at (cx, cy)
    void glyph(wchar_t code, Font font, float cx, float cy, D2D1_COLOR_F colour);
    // A progress bar `w` wide from (x, y), `t` 0..1 of it filled
    void bar(float x, float y, float w, float t);
    // A picture from the module's RCDATA resource `name` (PNG, JPEG; loaded the first time) in `r`; `t` 0 at rest, 0.6 under
    // the mouse (lighter), 1 pressed (darker, a little smaller); `on` false: dimmed. False: no such picture.
    bool picture(const wchar_t* name, const D2D1_RECT_F& r, float t = 0, bool on = true);
    // Its size in pixels (0 x 0: none)
    D2D1_SIZE_U picture_size(const wchar_t* name);
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
    // A line of text from `x`, its middle at `cy`, `top` at its top fading to `bottom` at its foot
    void text_shaded(const std::wstring& text, Font font, float x, float cy, D2D1_COLOR_F top, D2D1_COLOR_F bottom);
    // `text` cut to `room` with an ellipsis: at its end, or at its start (a path: its file's name shows)
    std::wstring fit(const std::wstring& text, Font font, float room, bool from_start) const;
    void logo(float x, float y, float size);
    // The close X, rose under the mouse; its hit area is 13 round its middle
    void close_x(float cx, float cy, bool hover);
    // A button in `r`; `t` 0 at rest, 0.6 under the mouse, 1 pressed; `on` false: dimmed; `icon` (0: none) before its label,
    // in `font` (Button: Icon's size, Play: IconBig's)
    void button(const D2D1_RECT_F& r, const std::wstring& label, Button style, float t, bool on = true, wchar_t icon = 0,
                Font font = Font::Button);
    float button_width(const std::wstring& label) const;
    // Text from (x, y) wrapped to `width`: the height it took
    float paragraph(const std::wstring& text, Font font, float x, float y, float width, D2D1_COLOR_F colour);
    // START's lettering: a line of text from `x`, its middle at `cy`, white fading to a cool grey over a soft dark shadow
    void styled(const std::wstring& text, Font font, float x, float cy, float alpha = 1.0f);
    // A soft shadow inside the rounded rectangle `r` (corners `radius`), `depth` in from its edges all round
    void inset_shadow(const D2D1_RECT_F& r, float radius, float depth);
    // A switch 30 x 16 from (x, its middle at cy): green and its knob right when on; lighter under the mouse
    void toggle(float x, float cy, bool on, bool hover);

private:
    // A picture from the module's resources: its pixels, and its bitmap once drawn
    struct Image {
        std::vector<std::uint32_t> pixels;
        UINT w = 0, h = 0;
        ID2D1Bitmap* bitmap = nullptr;
        ID2D1Bitmap* sized = nullptr; // picture()'s: scaled (WIC's Fant) to the pixels it's drawn at, sized_w x sized_h
        UINT sized_w = 0, sized_h = 0;
    };

    bool make_target();
    void load_icons(HMODULE module);
    void load_images(HMODULE module);
    static void load_image(HMODULE module, const wchar_t* name, Image& out);
    ID2D1Bitmap* bitmap_of(Image& image);
    ID2D1Bitmap* sized_of(Image& image, UINT w, UINT h);
    Image* named(const wchar_t* name);
    void shade(float y0, float y1, float a0, float a1);
    void gradient(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c0, D2D1_COLOR_F c1, bool across);
    void play_face(const D2D1_RECT_F& r, float t, float alpha);
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
    Image logo_img_, backdrop_img_, frosted_img_; // (frosted: the backdrop small and blurred, under the glass)
    std::vector<std::pair<std::wstring, Image>> pictures_; // picture()'s, by resource name
    HMODULE module_ = nullptr;
    IDWriteFontFileLoader* icon_loader_ = nullptr; // the icons' in-memory font loader, registered while the canvas lives
    IDWriteTextFormat* fonts_[static_cast<int>(Font::Count)] = {};
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
};

} // namespace vcapp::ui
