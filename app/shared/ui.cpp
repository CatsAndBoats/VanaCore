#include "ui.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

namespace vcapp::ui {

namespace {

// WIC's factory and its premultiplied BGRA (wincodec.h's, kept here so no GUID library is needed)
constexpr GUID kWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
constexpr GUID kWicPbgra = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};

template <class T> void release(T*& p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

struct FontSpec {
    Font font;
    float size;
    DWRITE_FONT_WEIGHT weight;
};
constexpr FontSpec kFonts[] = {
    {Font::Title, 19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {Font::Big, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
    {Font::Body, 14.5f, DWRITE_FONT_WEIGHT_NORMAL},     {Font::Small, 13.5f, DWRITE_FONT_WEIGHT_NORMAL},
    {Font::Label, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {Font::Button, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
};

} // namespace

D2D1_COLOR_F rgb(std::uint32_t c, float a)
{
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a};
}

D2D1_COLOR_F mix(std::uint32_t a, std::uint32_t b, float t, float alpha)
{
    const D2D1_COLOR_F x = rgb(a), y = rgb(b);
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, alpha};
}

Canvas::~Canvas()
{
    for (IDWriteTextFormat*& f : fonts_)
        release(f);
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
    if (com_)
        CoUninitialize();
}

bool Canvas::make_target()
{
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d_->CreateDCRenderTarget(&props, &rt_)))
        return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // see-through at its edges: no ClearType
    return SUCCEEDED(rt_->CreateSolidColorBrush(rgb(pal::text), &brush_));
}

bool Canvas::start(HMODULE module)
{
    com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); // (WIC, for the logo)
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw_))) ||
        !make_target())
        return false;
    for (const FontSpec& spec : kFonts) {
        IDWriteTextFormat*& format = fonts_[static_cast<int>(spec.font)];
        if (FAILED(dw_->CreateTextFormat(L"Segoe UI", nullptr, spec.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, spec.size,
                                         L"en-us", &format)))
            return false;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    load_logo(module);
    return true;
}

// The logo PNG from the module's resources, decoded to premultiplied BGRA (none: draw() puts VanaCore's colours there)
void Canvas::load_logo(HMODULE module)
{
    const HRSRC found = FindResourceW(module, L"LOGO", MAKEINTRESOURCEW(10)); // RT_RCDATA
    const HGLOBAL loaded = found != nullptr ? LoadResource(module, found) : nullptr;
    const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (bytes == nullptr)
        return;
    IWICImagingFactory* wic = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    const bool ok =
        SUCCEEDED(CoCreateInstance(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), SizeofResource(module, found))) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, kWicPbgra, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&logo_w_, &logo_h_)) && logo_w_ > 0 && logo_h_ > 0;
    if (ok) {
        logo_pixels_.resize(static_cast<std::size_t>(logo_w_) * logo_h_);
        if (FAILED(converter->CopyPixels(nullptr, logo_w_ * 4, static_cast<UINT>(logo_pixels_.size() * 4),
                                         reinterpret_cast<BYTE*>(logo_pixels_.data()))))
            logo_pixels_.clear();
    }
    release(converter);
    release(frame);
    release(decoder);
    release(stream);
    release(wic);
}

void Canvas::attach(HWND window, float width, float height)
{
    window_ = window;
    width_ = width, height_ = height;
    using GetDpi = UINT(WINAPI*)(HWND);
    auto dpi_of = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    scale_ = (dpi_of != nullptr ? static_cast<float>(dpi_of(window)) : 96.0f) / 96.0f;
    size_window(true);
}

void Canvas::dpi_changed(UINT dpi)
{
    scale_ = static_cast<float>(dpi) / 96.0f;
    size_window(false);
}

// The window sized for the card at this DPI (centred on its screen, or kept centred where it is) and its bitmap
void Canvas::size_window(bool centre)
{
    pixel_w_ = static_cast<int>(std::ceil((width_ + kShadow * 2) * scale_));
    pixel_h_ = static_cast<int>(std::ceil((height_ + kShadow * 2) * scale_));
    RECT now{};
    GetWindowRect(window_, &now);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int cx = centre ? (work.left + work.right) / 2 : (now.left + now.right) / 2;
    const int cy = centre ? (work.top + work.bottom) / 2 : (now.top + now.bottom) / 2;
    SetWindowPos(window_, nullptr, cx - pixel_w_ / 2, cy - pixel_h_ / 2, pixel_w_, pixel_h_, SWP_NOZORDER | SWP_NOACTIVATE);
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
    if (made == nullptr)
        return;
    HGDIOBJ old = SelectObject(memory_dc_, made);
    if (bitmap_ == nullptr)
        old_bitmap_ = old;
    else
        DeleteObject(bitmap_);
    bitmap_ = made;
}

bool Canvas::begin()
{
    if (window_ == nullptr || rt_ == nullptr || bitmap_ == nullptr)
        return false;
    const RECT bounds{0, 0, pixel_w_, pixel_h_};
    if (FAILED(rt_->BindDC(memory_dc_, &bounds)))
        return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(kShadow, kShadow) * D2D1::Matrix3x2F::Scale(scale_, scale_));
    return true;
}

void Canvas::end()
{
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) { // the device went: made again, and drawn again
        release(logo_);
        release(brush_);
        release(rt_);
        if (make_target())
            PostMessageW(window_, kRedraw, 0, 0);
        return;
    }
    RECT now{};
    GetWindowRect(window_, &now);
    POINT to{now.left, now.top}, from{0, 0};
    SIZE size{pixel_w_, pixel_h_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window_, nullptr, &to, &size, memory_dc_, &from, 0, &blend, ULW_ALPHA);
}

D2D1_POINT_2F Canvas::to_card(int x, int y) const
{
    return {static_cast<float>(x) / scale_ - kShadow, static_cast<float>(y) / scale_ - kShadow};
}

void Canvas::card(float head)
{
    const float w = width_, h = height_;
    for (int i = 6; i >= 1; --i) // a soft shadow in layers
        fill(-i * 2.0f, -i * 2.0f + 4, w + i * 4.0f, h + i * 4.0f, kRadius + i * 2.0f, rgb(0, 0.045f));
    fill(0, 0, w, h, kRadius, rgb(pal::bg));
    ID2D1RoundedRectangleGeometry* shape = nullptr;
    ID2D1Layer* layer = nullptr;
    if (head > 0 && SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), kRadius, kRadius), &shape)) &&
        SUCCEEDED(rt_->CreateLayer(&layer))) {
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape), layer); // the head's tint, round at the top
        brush_->SetColor(rgb(pal::white, 0.025f));
        rt_->FillRectangle(D2D1::RectF(0, 0, w, head), brush_);
        rt_->PopLayer();
    }
    release(layer);
    release(shape);
    outline(0, 0, w, h, kRadius, rgb(pal::border));
}

void Canvas::fill(float x, float y, float w, float h, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush_);
}

// A hairline just inside the rectangle
void Canvas::outline(float x, float y, float w, float h, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.5f, y + 0.5f, x + w - 0.5f, y + h - 0.5f), r, r), brush_, 1.0f);
}

void Canvas::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F colour, float width)
{
    brush_->SetColor(colour);
    rt_->DrawLine({x1, y1}, {x2, y2}, brush_, width);
}

void Canvas::disc(float cx, float cy, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->FillEllipse(D2D1::Ellipse({cx, cy}, r, r), brush_);
}

void Canvas::clip(const D2D1_RECT_F& r)
{
    rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
}

void Canvas::unclip()
{
    rt_->PopAxisAlignedClip();
}

IDWriteTextLayout* Canvas::layout(const std::wstring& text, Font font) const
{
    IDWriteTextLayout* made = nullptr;
    dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fonts_[static_cast<int>(font)], 8192, 4096, &made);
    return made;
}

float Canvas::text_width(const std::wstring& text, Font font) const
{
    IDWriteTextLayout* l = text.empty() ? nullptr : layout(text, font);
    if (l == nullptr)
        return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

void Canvas::text(const std::wstring& text, Font font, float x, float cy, D2D1_COLOR_F colour)
{
    IDWriteTextLayout* l = layout(text, font);
    if (l == nullptr)
        return;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    brush_->SetColor(colour);
    rt_->DrawTextLayout({x, cy - m.height / 2}, l, brush_);
    l->Release();
}

void Canvas::text_centred(const std::wstring& text, Font font, float cx, float cy, D2D1_COLOR_F colour)
{
    this->text(text, font, cx - text_width(text, font) / 2, cy, colour);
}

std::wstring Canvas::fit(const std::wstring& text, Font font, float room, bool from_start) const
{
    if (text_width(text, font) <= room)
        return text;
    auto cut = [&](std::size_t keep) { return from_start ? L"\x2026" + text.substr(text.size() - keep) : text.substr(0, keep) + L"\x2026"; };
    std::size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const std::size_t keep = (lo + hi + 1) / 2;
        if (text_width(cut(keep), font) <= room)
            lo = keep;
        else
            hi = keep - 1;
    }
    return cut(lo);
}

void Canvas::logo(float x, float y, float size)
{
    if (logo_ == nullptr && !logo_pixels_.empty())
        rt_->CreateBitmap(D2D1::SizeU(logo_w_, logo_h_), logo_pixels_.data(), logo_w_ * 4,
                          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &logo_);
    if (logo_ != nullptr) {
        rt_->DrawBitmap(logo_, D2D1::RectF(x, y, x + size, y + size), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    fill(x + 4, y + 4, size - 8, size - 8, 10, rgb(pal::accent_lo)); // no logo: VanaCore's colours where it would be
    text_centred(L"V", Font::Title, x + size / 2, y + size / 2, rgb(pal::white));
}

void Canvas::close_x(float cx, float cy, bool hover)
{
    const float s = 5.0f;
    if (hover)
        disc(cx, cy, 13, rgb(pal::rose, 0.14f));
    const D2D1_COLOR_F c = hover ? rgb(pal::rose) : rgb(pal::dim);
    line(cx - s, cy - s, cx + s, cy + s, c, 1.6f);
    line(cx - s, cy + s, cx + s, cy - s, c, 1.6f);
}

float Canvas::button_width(const std::wstring& label) const
{
    return std::max(84.0f, text_width(label, Font::Button) + 32);
}

void Canvas::button(const D2D1_RECT_F& r, const std::wstring& label, Button style, float t, bool on)
{
    const float w = r.right - r.left, h = r.bottom - r.top, alpha = on ? 1.0f : 0.45f;
    if (!on)
        t = 0;
    D2D1_COLOR_F fg;
    if (style == Button::Accent) {
        fill(r.left, r.top, w, h, 4, mix(pal::accent, pal::accent2, t * 0.6f, alpha));
        fg = rgb(pal::ink, on ? 1.0f : 0.7f);
    } else if (style == Button::Secondary) {
        fill(r.left, r.top, w, h, 4, mix(pal::well, pal::hover, t));
        outline(r.left, r.top, w, h, 4, mix(pal::well_edge, pal::accent_lo, t));
        fg = rgb(pal::text, alpha);
    } else {
        if (t > 0)
            fill(r.left, r.top, w, h, 4, rgb(pal::hover, t));
        fg = mix(pal::text2, pal::text, t, alpha);
    }
    text_centred(label, Font::Button, r.left + w / 2, r.top + h / 2, fg);
}

} // namespace vcapp::ui
