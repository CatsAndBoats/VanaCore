// The --gui start screen: the .vcaddon card's look (src\core\addonpkg_win.cpp: the /vana palette, VanaCore's logo, round
// corners and a soft shadow, Direct2D and DirectWrite into a layered window) with Retail / Private server, the server and the
// settings file, and Start Game. A layered window shows no child windows, so its two text boxes are its own: a caret,
// typing, Backspace / Delete, the arrows, Home / End, Ctrl+V. d2d1.dll and dwrite.dll are loaded only here, so VanaCore.exe
// still starts on a Windows without them (a message box asks instead).

#include <windows.h>

#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#include "gui.hpp"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

namespace vanacore {
namespace {

// The /vana palette (draw.lua's M.P), as the .vcaddon card has it
namespace pal {
constexpr std::uint32_t bg = 0x161822, border = 0x2A2E3E, line = 0x242838, well = 0x0E1017, well_edge = 0x272B3C,
                        hover = 0x1F2334, text = 0xE8EAF1, white = 0xFFFFFF, text2 = 0xBDC1D0, dim = 0x7B8095,
                        faint = 0x4E536A, accent = 0x7AA2F7, accent2 = 0xA9C1FF, accent_lo = 0x3E5CB3, rose = 0xEE6F78,
                        ink = 0x0B1020;
}

// WIC's factory and its premultiplied BGRA (wincodec.h's, kept here: no windowscodecs.lib)
constexpr GUID kWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
constexpr GUID kWicPbgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

D2D1_COLOR_F rgb(std::uint32_t c, float a = 1.0f)
{
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a};
}

D2D1_COLOR_F mix(std::uint32_t a, std::uint32_t b, float t, float alpha = 1.0f)
{
    const D2D1_COLOR_F x = rgb(a), y = rgb(b);
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, alpha};
}

// The card's measures, in DIPs, and the room round it for its shadow
constexpr float kW = 470, kPad = 26, kHead = 76, kLogo = 56, kRadius = 10, kShadow = 16;
constexpr float kSegH = 34, kFieldH = 32, kRowGap = 10, kLabelW = 78, kBrowseW = 84, kButtonH = 30;
constexpr float kTitle = 19.0f, kBody = 14.5f, kSmall = 14.0f, kLabel = 11.5f, kButton = 14.5f;
constexpr float kHeight = kPad + kHead + 14 + kSegH + 20 + kFieldH + kRowGap + kFieldH + 28 + kButtonH + kPad;
constexpr UINT_PTR kBlink = 1;
constexpr int kServerField = 0, kIniField = 1;

enum Hit { kNone, kClose, kRetail, kPrivate, kServer, kIni, kBrowse, kStart, kCancel };

template <class T> void release(T*& p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

std::wstring exe_folder()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\'));
}

std::wstring trimmed(const std::wstring& s)
{
    const std::size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// VanaCore's logo (addons\vanacore\vana\logo.png, as /vana shows it), premultiplied BGRA
bool load_logo(const std::wstring& path, std::vector<std::uint32_t>& pixels, UINT& w, UINT& h)
{
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                         reinterpret_cast<void**>(&wic))) &&
              SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, kWicPbgra, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0;
    if (ok) {
        pixels.resize(static_cast<std::size_t>(w) * h);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
    }
    release(converter);
    release(frame);
    release(decoder);
    release(wic);
    return ok;
}

class Screen {
public:
    explicit Screen(GuiChoice& choice) : choice_(choice), retail_(choice.retail)
    {
        field_[kServerField] = choice.server;
        field_[kIniField] = choice.ini;
        caret_[kServerField] = field_[kServerField].size();
        caret_[kIniField] = field_[kIniField].size();
    }
    ~Screen();
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    // Shows the screen and waits until it's shut: true on Start Game (the choice then holds what was picked)
    bool run();

private:
    bool start_graphics();
    bool make_target();
    void place();
    void render();
    void draw_card();
    void draw_head(float x, float y, float right);
    void draw_close(float right, float top);
    void draw_switch(float x, float y, float right);
    void draw_field(int i, Hit hit, float x, float y, float w);
    float draw_button(Hit hit, const std::wstring& label, int style, float right, float cy);
    Hit hit_at(float px, float py) const;
    void press(Hit hit);
    void browse();
    void focus(int field);
    void caret_to(int field, float x);
    void type(wchar_t c);
    void key(WPARAM vk);
    bool field_on(int i) const { return i == kIniField || !retail_; }

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    bool mouse(UINT message, LPARAM lparam);

    // drawing, in DIPs from the card's top left
    float text_width(const std::wstring& text, IDWriteTextFormat* format) const;
    void text_mid(const std::wstring& text, IDWriteTextFormat* format, float x, float cy, D2D1_COLOR_F colour);
    void fill(float x, float y, float w, float h, float r, D2D1_COLOR_F colour);
    void outline(float x, float y, float w, float h, float r, D2D1_COLOR_F colour);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F colour, float width);
    void disc(float cx, float cy, float r, D2D1_COLOR_F colour);

    GuiChoice& choice_;
    bool retail_;
    std::wstring field_[2];
    std::size_t caret_[2] = {};
    D2D1_RECT_F field_rect_[2] = {};
    int focus_ = -1;
    bool caret_on_ = true;
    bool started_ = false, done_ = false;

    std::vector<std::pair<Hit, D2D1_RECT_F>> hits_;
    Hit hover_ = kNone, pressed_ = kNone;
    bool tracking_ = false;

    HWND window_ = nullptr;
    float scale_ = 1.0f;
    int pixel_w_ = 0, pixel_h_ = 0;
    HMODULE d2d_dll_ = nullptr, dw_dll_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1Bitmap* logo_ = nullptr;
    std::vector<std::uint32_t> logo_pixels_;
    UINT logo_w_ = 0, logo_h_ = 0;
    IDWriteTextFormat* title_font_ = nullptr;
    IDWriteTextFormat* body_font_ = nullptr;
    IDWriteTextFormat* small_font_ = nullptr;
    IDWriteTextFormat* label_font_ = nullptr;
    IDWriteTextFormat* button_font_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
};

Screen::~Screen()
{
    release(title_font_);
    release(body_font_);
    release(small_font_);
    release(label_font_);
    release(button_font_);
    release(logo_);
    release(brush_);
    release(rt_);
    release(dw_);
    release(d2d_);
    if (memory_dc_ != nullptr) {
        if (bitmap_ != nullptr)
            SelectObject(memory_dc_, old_bitmap_);
        DeleteDC(memory_dc_);
    }
    if (bitmap_ != nullptr)
        DeleteObject(bitmap_);
}

bool Screen::make_target()
{
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d_->CreateDCRenderTarget(&props, &rt_)))
        return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // see-through at its edges: no ClearType
    return SUCCEEDED(rt_->CreateSolidColorBrush(rgb(pal::text), &brush_));
}

bool Screen::start_graphics()
{
    using CreateD2D = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    using CreateDw = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
    d2d_dll_ = LoadLibraryW(L"d2d1.dll");
    dw_dll_ = LoadLibraryW(L"dwrite.dll");
    auto create = d2d_dll_ != nullptr ? reinterpret_cast<CreateD2D>(GetProcAddress(d2d_dll_, "D2D1CreateFactory")) : nullptr;
    auto create_dw = dw_dll_ != nullptr ? reinterpret_cast<CreateDw>(GetProcAddress(dw_dll_, "DWriteCreateFactory")) : nullptr;
    if (create == nullptr || create_dw == nullptr ||
        FAILED(create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(&d2d_))) ||
        FAILED(create_dw(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw_))) || !make_target())
        return false;
    auto font = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        if (FAILED(dw_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                         L"en-us", out)))
            return false;
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };
    if (!font(kTitle, DWRITE_FONT_WEIGHT_SEMI_BOLD, &title_font_) || !font(kBody, DWRITE_FONT_WEIGHT_NORMAL, &body_font_) ||
        !font(kSmall, DWRITE_FONT_WEIGHT_NORMAL, &small_font_) || !font(kLabel, DWRITE_FONT_WEIGHT_SEMI_BOLD, &label_font_) ||
        !font(kButton, DWRITE_FONT_WEIGHT_SEMI_BOLD, &button_font_))
        return false;
    load_logo(exe_folder() + L"\\addons\\vanacore\\vana\\logo.png", logo_pixels_, logo_w_, logo_h_);
    return true;
}

float Screen::text_width(const std::wstring& text, IDWriteTextFormat* format) const
{
    IDWriteTextLayout* layout = nullptr;
    if (text.empty() || FAILED(dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 8192, 4096, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

void Screen::text_mid(const std::wstring& text, IDWriteTextFormat* format, float x, float cy, D2D1_COLOR_F colour)
{
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 8192, 4096, &layout)))
        return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    brush_->SetColor(colour);
    rt_->DrawTextLayout({x, cy - m.height / 2}, layout, brush_);
    layout->Release();
}

void Screen::fill(float x, float y, float w, float h, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush_);
}

// A hairline just inside the rectangle
void Screen::outline(float x, float y, float w, float h, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.5f, y + 0.5f, x + w - 0.5f, y + h - 0.5f), r, r), brush_, 1.0f);
}

void Screen::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F colour, float width)
{
    brush_->SetColor(colour);
    rt_->DrawLine({x1, y1}, {x2, y2}, brush_, width);
}

void Screen::disc(float cx, float cy, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->FillEllipse(D2D1::Ellipse({cx, cy}, r, r), brush_);
}

// The window sized for the card at this DPI, centred on the mouse's screen, and its bitmap
void Screen::place()
{
    pixel_w_ = static_cast<int>(std::ceil((kW + kShadow * 2) * scale_));
    pixel_h_ = static_cast<int>(std::ceil((kHeight + kShadow * 2) * scale_));
    RECT now{};
    GetWindowRect(window_, &now);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    SetWindowPos(window_, nullptr, (work.left + work.right - pixel_w_) / 2, (work.top + work.bottom - pixel_h_) / 2, pixel_w_, pixel_h_,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    if (memory_dc_ == nullptr)
        memory_dc_ = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = pixel_w_;
    bi.bmiHeader.biHeight = -pixel_h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP made = CreateDIBSection(memory_dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (made != nullptr) {
        HGDIOBJ old = SelectObject(memory_dc_, made);
        if (bitmap_ == nullptr)
            old_bitmap_ = old;
        else
            DeleteObject(bitmap_);
        bitmap_ = made;
    }
}

void Screen::draw_head(float x, float y, float right)
{
    const float ix = x - 4, iy = y + 25 - kLogo / 2;
    if (logo_ == nullptr && !logo_pixels_.empty())
        rt_->CreateBitmap(D2D1::SizeU(logo_w_, logo_h_), logo_pixels_.data(), logo_w_ * 4,
                          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &logo_);
    if (logo_ != nullptr) {
        rt_->DrawBitmap(logo_, D2D1::RectF(ix, iy, ix + kLogo, iy + kLogo), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else { // no logo.png: VanaCore's colours where it would be
        fill(ix + 4, iy + 4, kLogo - 8, kLogo - 8, 10, rgb(pal::accent_lo));
        text_mid(L"V", title_font_, ix + kLogo / 2 - text_width(L"V", title_font_) / 2, iy + kLogo / 2, rgb(pal::white));
    }
    const float tx = ix + kLogo + 14;
    text_mid(L"VanaCore", title_font_, tx, y + 13, rgb(pal::white));
    text_mid(L"Start the game", small_font_, tx, y + 37, rgb(pal::dim));
    static_cast<void>(right);
}

// The close X, rose under the mouse
void Screen::draw_close(float right, float top)
{
    const float cx = right - 4, cy = top + 12, s = 5.0f;
    const bool over = hover_ == kClose;
    if (over)
        disc(cx, cy, 13, rgb(pal::rose, 0.14f));
    const D2D1_COLOR_F c = over ? rgb(pal::rose) : rgb(pal::dim);
    line(cx - s, cy - s, cx + s, cy + s, c, 1.6f);
    line(cx - s, cy + s, cx + s, cy - s, c, 1.6f);
    hits_.push_back({kClose, D2D1::RectF(cx - 13, cy - 13, cx + 13, cy + 13)});
}

// Retail / Private server, as two halves of one well
void Screen::draw_switch(float x, float y, float right)
{
    const float w = right - x, half = (w - 6) / 2;
    fill(x, y, w, kSegH, 6, rgb(pal::well));
    outline(x, y, w, kSegH, 6, rgb(pal::well_edge));
    const std::pair<Hit, const wchar_t*> sides[2] = {{kRetail, L"Retail"}, {kPrivate, L"Private server"}};
    for (int i = 0; i < 2; ++i) {
        const auto& [hit, label] = sides[i];
        const bool on = (hit == kRetail) == retail_, over = hover_ == hit;
        const float bx = x + 3 + i * half, by = y + 3, bh = kSegH - 6;
        if (on)
            fill(bx, by, half, bh, 4, rgb(pal::accent));
        else if (over)
            fill(bx, by, half, bh, 4, rgb(pal::hover));
        const float tw = text_width(label, button_font_);
        text_mid(label, button_font_, bx + (half - tw) / 2, y + kSegH / 2,
                 on ? rgb(pal::ink) : rgb(over ? pal::text : pal::text2));
        hits_.push_back({hit, D2D1::RectF(bx, by, bx + half, by + bh)});
    }
}

// A text box: its well, its text (kept in view round the caret while it has the keys) or its placeholder, the caret
void Screen::draw_field(int i, Hit hit, float x, float y, float w)
{
    const bool on = field_on(i), focused = focus_ == i && on, over = on && hover_ == hit;
    fill(x, y, w, kFieldH, 5, rgb(pal::well, on ? 1.0f : 0.45f));
    outline(x, y, w, kFieldH, 5, focused ? rgb(pal::accent) : over ? rgb(pal::accent_lo) : rgb(pal::well_edge, on ? 1.0f : 0.5f));
    field_rect_[i] = D2D1::RectF(x, y, x + w, y + kFieldH);
    if (on)
        hits_.push_back({hit, field_rect_[i]});
    const float tx = x + 11, room = w - 22, cy = y + kFieldH / 2;
    const std::wstring& text = field_[i];
    const float whole = text_width(text, body_font_), caret_x = text_width(text.substr(0, caret_[i]), body_font_);
    // focused: the caret in view; not: the server from its start, a settings path from its end (its file's name)
    float shift = 0;
    if (focused)
        shift = std::max(0.0f, caret_x - room);
    else if (i == kIniField)
        shift = std::max(0.0f, whole - room);
    rt_->PushAxisAlignedClip(D2D1::RectF(x + 2, y, x + w - 2, y + kFieldH), D2D1_ANTIALIAS_MODE_ALIASED);
    if (text.empty())
        text_mid(i == kServerField ? L"Server address" : L"vanacore.ini", body_font_, tx, cy, rgb(pal::faint, on ? 1.0f : 0.6f));
    else
        text_mid(text, body_font_, tx - shift, cy, on ? rgb(pal::text) : rgb(pal::dim, 0.7f));
    if (focused && caret_on_)
        line(tx + caret_x - shift, cy - 9, tx + caret_x - shift, cy + 9, rgb(pal::accent2), 1.3f);
    rt_->PopAxisAlignedClip();
}

// A button ending at `right`: 0 the accent (Start Game), 1 a well (Browse), 2 a ghost (Cancel). Where the next one ends.
float Screen::draw_button(Hit hit, const std::wstring& label, int style, float right, float cy)
{
    const float tw = text_width(label, button_font_), w = std::max(84.0f, tw + 32);
    const float bx = right - w, by = cy - kButtonH / 2;
    const bool over = hover_ == hit;
    const float t = over && pressed_ == hit ? 1.0f : over ? 0.6f : 0.0f;
    D2D1_COLOR_F fg;
    if (style == 0) {
        fill(bx, by, w, kButtonH, 4, mix(pal::accent, pal::accent2, t * 0.6f));
        fg = rgb(pal::ink);
    } else if (style == 1) {
        fill(bx, by, w, kButtonH, 4, mix(pal::well, pal::hover, t));
        outline(bx, by, w, kButtonH, 4, mix(pal::well_edge, pal::accent_lo, t));
        fg = rgb(pal::text);
    } else {
        if (t > 0)
            fill(bx, by, w, kButtonH, 4, rgb(pal::hover, t));
        fg = mix(pal::text2, pal::text, t);
    }
    text_mid(label, button_font_, bx + (w - tw) / 2, cy, fg);
    hits_.push_back({hit, D2D1::RectF(bx, by, bx + w, by + kButtonH)});
    return bx - 8;
}

// The card itself: its shadow, page, head, the switch, the two rows and the buttons
void Screen::draw_card()
{
    const float w = kW, h = kHeight;
    for (int i = 6; i >= 1; --i)
        fill(-i * 2.0f, -i * 2.0f + 4, w + i * 4.0f, h + i * 4.0f, kRadius + i * 2.0f, rgb(0, 0.045f));
    fill(0, 0, w, h, kRadius, rgb(pal::bg));
    ID2D1RoundedRectangleGeometry* card = nullptr;
    ID2D1Layer* layer = nullptr;
    if (SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), kRadius, kRadius), &card)) &&
        SUCCEEDED(rt_->CreateLayer(&layer))) {
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), card), layer); // the head's tint, round at the top
        brush_->SetColor(rgb(pal::white, 0.025f));
        rt_->FillRectangle(D2D1::RectF(0, 0, w, kPad + kHead - 4), brush_);
        rt_->PopLayer();
    }
    release(layer);
    release(card);
    outline(0, 0, w, h, kRadius, rgb(pal::border));

    const float x = kPad, right = w - kPad;
    draw_head(x, kPad, right);
    draw_close(right, kPad);
    float y = kPad + kHead;
    line(1, y - 4, w - 1, y - 4, rgb(pal::line), 1.0f);
    y += 14;
    draw_switch(x, y, right);
    y += kSegH + 20;
    text_mid(L"SERVER", label_font_, x, y + kFieldH / 2, rgb(pal::faint, retail_ ? 0.5f : 1.0f));
    draw_field(kServerField, kServer, x + kLabelW, y, right - x - kLabelW);
    y += kFieldH + kRowGap;
    text_mid(L"SETTINGS", label_font_, x, y + kFieldH / 2, rgb(pal::faint));
    draw_field(kIniField, kIni, x + kLabelW, y, right - x - kLabelW - kBrowseW - 8);
    draw_button(kBrowse, L"Browse\x2026", 1, right, y + kFieldH / 2);
    const float cy = h - kPad - 15;
    draw_button(kCancel, L"Cancel", 2, draw_button(kStart, L"Start Game", 0, right, cy), cy);
}

void Screen::render()
{
    if (window_ == nullptr || rt_ == nullptr || bitmap_ == nullptr)
        return;
    hits_.clear();
    const RECT bounds{0, 0, pixel_w_, pixel_h_};
    if (FAILED(rt_->BindDC(memory_dc_, &bounds)))
        return;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(kShadow, kShadow) * D2D1::Matrix3x2F::Scale(scale_, scale_));
    draw_card();
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) { // the device went: made again, and drawn again
        release(logo_);
        release(brush_);
        release(rt_);
        if (make_target())
            PostMessageW(window_, WM_APP, 0, 0);
        return;
    }
    RECT now{};
    GetWindowRect(window_, &now);
    POINT to{now.left, now.top}, from{0, 0};
    SIZE size{pixel_w_, pixel_h_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, nullptr, &to, &size, memory_dc_, &from, 0, &blend, ULW_ALPHA);
}

Hit Screen::hit_at(float px, float py) const
{
    const float x = px / scale_ - kShadow, y = py / scale_ - kShadow;
    for (const auto& [hit, r] : hits_)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return hit;
    return kNone;
}

void Screen::focus(int field)
{
    focus_ = field >= 0 && field_on(field) ? field : -1;
    caret_on_ = true;
    if (focus_ >= 0)
        SetTimer(window_, kBlink, GetCaretBlinkTime(), nullptr);
    else
        KillTimer(window_, kBlink);
}

// The caret put at the letter nearest `x` (the window's pixels) in the field
void Screen::caret_to(int field, float x)
{
    const D2D1_RECT_F& r = field_rect_[field];
    const std::wstring& text = field_[field];
    const float room = r.right - r.left - 22;
    const float whole = text_width(text, body_font_);
    const float shift = field == focus_ ? std::max(0.0f, text_width(text.substr(0, caret_[field]), body_font_) - room)
                                        : field == kIniField ? std::max(0.0f, whole - room) : 0.0f;
    const float at = x / scale_ - kShadow - (r.left + 11) + shift;
    std::size_t best = 0;
    float best_gap = 1e9f;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const float gap = std::fabs(text_width(text.substr(0, i), body_font_) - at);
        if (gap < best_gap)
            best_gap = gap, best = i;
    }
    caret_[field] = best;
}

void Screen::browse()
{
    wchar_t file[MAX_PATH] = {};
    const std::wstring dir = exe_folder();
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.hwndOwner = window_;
    open.lpstrFilter = L"Settings (*.ini)\0*.ini\0All files\0*.*\0";
    open.lpstrFile = file;
    open.nMaxFile = MAX_PATH;
    open.lpstrInitialDir = dir.c_str();
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&open))
        return;
    std::wstring picked = file;
    // beside VanaCore.exe: its name alone, as --ini takes it
    if (picked.size() > dir.size() + 1 && _wcsnicmp(picked.c_str(), dir.c_str(), dir.size()) == 0 && picked[dir.size()] == L'\\' &&
        picked.find(L'\\', dir.size() + 1) == std::wstring::npos)
        picked = picked.substr(dir.size() + 1);
    field_[kIniField] = picked;
    caret_[kIniField] = picked.size();
}

void Screen::press(Hit hit)
{
    switch (hit) {
    case kClose:
    case kCancel:
        DestroyWindow(window_);
        return;
    case kRetail:
    case kPrivate:
        retail_ = hit == kRetail;
        if (!field_on(focus_))
            focus(-1);
        break;
    case kBrowse:
        browse();
        break;
    case kStart:
        choice_.retail = retail_;
        choice_.server = trimmed(field_[kServerField]);
        choice_.ini = trimmed(field_[kIniField]);
        started_ = true;
        DestroyWindow(window_);
        return;
    default:
        return;
    }
    render();
}

// A typed letter, Backspace, Ctrl+Backspace (the field cleared up to the caret) or Ctrl+V into the field with the keys
void Screen::type(wchar_t c)
{
    if (focus_ < 0)
        return;
    std::wstring& text = field_[focus_];
    std::size_t& caret = caret_[focus_];
    if (c == 0x08 && caret > 0) {
        text.erase(--caret, 1);
    } else if (c == 0x7F) {
        text.erase(0, caret), caret = 0;
    } else if (c == 0x16 && OpenClipboard(window_)) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* p = static_cast<const wchar_t*>(GlobalLock(data))) {
                std::wstring pasted;
                for (; *p != 0; ++p)
                    if (*p >= 0x20)
                        pasted += *p;
                text.insert(caret, pasted), caret += pasted.size();
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    } else if (c >= 0x20) {
        text.insert(caret++, 1, c);
    }
    caret_on_ = true;
    render();
}

void Screen::key(WPARAM vk)
{
    if (vk == VK_RETURN) {
        press(kStart);
        return;
    }
    if (vk == VK_ESCAPE) {
        press(kClose);
        return;
    }
    if (vk == VK_TAB) { // through the fields that take text, Shift back
        const bool back = GetKeyState(VK_SHIFT) < 0;
        std::vector<int> fields;
        for (int i = 0; i < 2; ++i)
            if (field_on(i))
                fields.push_back(i);
        const auto at = std::find(fields.begin(), fields.end(), focus_);
        const std::size_t n = fields.size();
        const std::size_t next = at == fields.end() ? (back ? n - 1 : 0) : (static_cast<std::size_t>(at - fields.begin()) + (back ? n - 1 : 1)) % n;
        focus(fields[next]);
    } else if (focus_ >= 0) {
        std::wstring& text = field_[focus_];
        std::size_t& caret = caret_[focus_];
        if (vk == VK_LEFT && caret > 0)
            --caret;
        else if (vk == VK_RIGHT && caret < text.size())
            ++caret;
        else if (vk == VK_HOME)
            caret = 0;
        else if (vk == VK_END)
            caret = text.size();
        else if (vk == VK_DELETE && caret < text.size())
            text.erase(caret, 1);
        else
            return;
        caret_on_ = true;
    } else {
        return;
    }
    render();
}

LRESULT CALLBACK Screen::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* screen = reinterpret_cast<Screen*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (screen == nullptr || screen->window_ != window)
        return DefWindowProcW(window, message, wparam, lparam);
    return screen->handle(message, wparam, lparam);
}

// The mouse over the card: hover, press and release (a field takes the keys and its caret where it was clicked)
bool Screen::mouse(UINT message, LPARAM lparam)
{
    const float px = static_cast<float>(GET_X_LPARAM(lparam)), py = static_cast<float>(GET_Y_LPARAM(lparam));
    const Hit at = hit_at(px, py);
    if (message == WM_MOUSEMOVE) {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window_, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        if (at == hover_)
            return true;
        hover_ = at;
    } else if (message == WM_LBUTTONDOWN) {
        pressed_ = at;
        SetCapture(window_);
        if (at == kServer || at == kIni) {
            const int field = at == kServer ? kServerField : kIniField;
            caret_to(field, px);
            focus(field);
        }
    } else if (message == WM_LBUTTONUP) {
        ReleaseCapture();
        const Hit was = pressed_;
        pressed_ = kNone;
        if (at != kNone && at == was && at != kServer && at != kIni) {
            press(at);
            return true;
        }
    } else {
        return false;
    }
    render();
    return true;
}

LRESULT Screen::handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_NCHITTEST: { // the card drags the window anywhere but on its controls
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window_, &p);
        return hit_at(static_cast<float>(p.x), static_cast<float>(p.y)) != kNone ? HTCLIENT : HTCAPTION;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        mouse(message, lparam);
        return 0;
    case WM_NCMOUSEMOVE:
    case WM_MOUSELEAVE:
        tracking_ = message == WM_MOUSELEAVE ? false : tracking_;
        if (hover_ != kNone) {
            hover_ = kNone;
            render();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && hover_ != kNone) {
            SetCursor(LoadCursorW(nullptr, hover_ == kServer || hover_ == kIni ? IDC_IBEAM : IDC_HAND));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        key(wparam);
        return 0;
    case WM_CHAR:
        type(static_cast<wchar_t>(wparam));
        return 0;
    case WM_TIMER:
        caret_on_ = !caret_on_;
        render();
        return 0;
    case WM_APP: // drawn again after the device was lost
        render();
        return 0;
    case WM_DPICHANGED: // moved to a screen of another DPI: sized for it
        scale_ = HIWORD(wparam) / 96.0f;
        place();
        render();
        return 0;
    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        done_ = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

bool Screen::run()
{
    if (!start_graphics()) // no Direct2D: asked plainly, started as vanacore.ini has it
        return MessageBoxW(nullptr, L"Start the game?", L"VanaCore", MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
    static const wchar_t kClass[] = L"VanaCore.StartScreen";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &Screen::proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    POINT cursor{};
    GetCursorPos(&cursor); // made on the mouse's screen, so it has that screen's DPI from the start
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClass, L"VanaCore", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, cursor.x,
                              cursor.y, 1, 1, nullptr, nullptr, instance, this);
    if (window_ == nullptr)
        return false;
    using GetDpi = UINT(WINAPI*)(HWND);
    auto dpi_of = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    scale_ = (dpi_of != nullptr ? static_cast<float>(dpi_of(window_)) : 96.0f) / 96.0f;
    place();
    focus(retail_ ? -1 : kServerField);
    render();
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    MSG message;
    while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return started_;
}

} // namespace

bool show_start_screen(GuiChoice& choice)
{
    // per-monitor DPI for this thread (the card sharp on each screen), and COM for the logo and the Open dialog
    using SetDpi = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto set = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    const DPI_AWARENESS_CONTEXT was = set != nullptr ? set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    bool started = false;
    {
        Screen screen(choice);
        started = screen.run();
    }
    if (com)
        CoUninitialize();
    if (set != nullptr && was != nullptr)
        set(was);
    return started;
}

} // namespace vanacore
