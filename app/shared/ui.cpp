// Windows 10 1703's DirectWrite (IDWriteFactory5, for the icons' font from memory) declared, though the app is built for 7: it's
// asked for at run time (QueryInterface), and older Windows just draws no icons
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000003

#include "ui.h"

#include <dwrite_3.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <tuple>

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
    {Font::Play, 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
};
constexpr FontSpec kSerifPlay = {Font::Play, 24.0f, DWRITE_FONT_WEIGHT_BOLD}; // (PT Serif Bold, res\ptserif_bold.ttf)
constexpr FontSpec kIconFonts[] ={{Font::Icon, 18.0f, DWRITE_FONT_WEIGHT_NORMAL}, {Font::IconBig, 24.0f, DWRITE_FONT_WEIGHT_NORMAL}};

// `in` (opaque, premultiplied BGRA) a `down`th the size, each pixel its block's average, then blurred (three box blurs of
// `radius` each way, near a Gaussian): the glass's frosted picture, drawn stretched back over the card
std::vector<std::uint32_t> frost(const std::vector<std::uint32_t>& in, UINT iw, UINT ih, int down, int radius, UINT& w, UINT& h)
{
    w = iw / down, h = ih / down;
    if (w == 0 || h == 0)
        return {};
    std::vector<float> c[4];
    for (auto& plane : c)
        plane.assign(static_cast<std::size_t>(w) * h, 0.0f);
    for (UINT y = 0; y < h * down; ++y)
        for (UINT x = 0; x < w * down; ++x) {
            const std::uint32_t p = in[static_cast<std::size_t>(y) * iw + x];
            const std::size_t at = static_cast<std::size_t>(y / down) * w + x / down;
            for (int k = 0; k < 4; ++k)
                c[k][at] += static_cast<float>((p >> (k * 8)) & 255);
        }
    const float block = static_cast<float>(down * down);
    for (auto& plane : c)
        for (float& v : plane)
            v /= block;
    std::vector<float> t(static_cast<std::size_t>(w) * h);
    auto box = [&](std::vector<float>& a, bool across) {
        const int n = across ? static_cast<int>(w) : static_cast<int>(h), lines = across ? static_cast<int>(h) : static_cast<int>(w);
        for (int line = 0; line < lines; ++line) {
            auto at = [&](int i) -> float& {
                const int clamped = i < 0 ? 0 : i >= n ? n - 1 : i;
                return across ? a[static_cast<std::size_t>(line) * w + clamped] : a[static_cast<std::size_t>(clamped) * w + line];
            };
            float sum = 0;
            for (int i = -radius; i <= radius; ++i)
                sum += at(i);
            for (int i = 0; i < n; ++i) {
                (across ? t[static_cast<std::size_t>(line) * w + i] : t[static_cast<std::size_t>(i) * w + line]) = sum / (2 * radius + 1);
                sum += at(i + radius + 1) - at(i - radius);
            }
        }
        a.swap(t);
    };
    for (auto& plane : c)
        for (int pass = 0; pass < 3; ++pass)
            box(plane, true), box(plane, false);
    std::vector<std::uint32_t> out(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < out.size(); ++i)
        for (int k = 0; k < 4; ++k)
            out[i] |= static_cast<std::uint32_t>(std::lround(std::clamp(c[k][i], 0.0f, 255.0f))) << (k * 8);
    return out;
}

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
    release(logo_img_.bitmap);
    release(backdrop_img_.bitmap);
    release(frosted_img_.bitmap);
    for (auto& [name, image] : pictures_)
        release(image.bitmap), release(image.sized);
    release(brush_);
    release(rt_);
    if (icon_loader_ != nullptr) {
        IDWriteFactory5* f5 = nullptr;
        if (SUCCEEDED(dw_->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(&f5)))) {
            f5->UnregisterFontFileLoader(icon_loader_);
            f5->Release();
        }
        release(icon_loader_);
    }
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
    module_ = module;
    load_icons(module);
    load_images(module);
    return true;
}

// The module's own fonts from memory, one font collection of them (IDWriteFactory5: Windows 10 1703 on): the icons (the RCDATA
// resource SYMBOLS; without them Font::Icon's formats stay null and glyph() draws nothing) and PT Serif Bold (SERIF: Font::Play's;
// without it Play stays Segoe UI's)
void Canvas::load_icons(HMODULE module)
{
    IDWriteFactory5* f5 = nullptr;
    if (FAILED(dw_->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(&f5))))
        return;
    IDWriteInMemoryFontFileLoader* loader = nullptr;
    IDWriteFontSetBuilder1* builder = nullptr;
    IDWriteFontSet* set = nullptr;
    IDWriteFontCollection1* collection = nullptr;
    if (SUCCEEDED(f5->CreateInMemoryFontFileLoader(&loader)) && SUCCEEDED(f5->RegisterFontFileLoader(loader)) &&
        SUCCEEDED(f5->CreateFontSetBuilder(&builder))) {
        icon_loader_ = loader, loader->AddRef();
        for (const wchar_t* name : {L"SYMBOLS", L"SERIF"}) {
            const HRSRC found = FindResourceW(module, name, MAKEINTRESOURCEW(10)); // RT_RCDATA
            const HGLOBAL loaded = found != nullptr ? LoadResource(module, found) : nullptr;
            const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
            IDWriteFontFile* file = nullptr;
            if (bytes != nullptr &&
                SUCCEEDED(loader->CreateInMemoryFontFileReference(f5, bytes, SizeofResource(module, found), nullptr, &file)))
                builder->AddFontFile(file);
            release(file);
        }
        if (SUCCEEDED(builder->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &collection))) {
            auto make = [&](const wchar_t* family, const FontSpec& spec) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (FAILED(collection->FindFamilyName(family, &index, &exists)) || !exists)
                    return;
                IDWriteTextFormat* format = nullptr;
                if (FAILED(dw_->CreateTextFormat(family, collection, spec.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 spec.size, L"en-us", &format)))
                    return;
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                IDWriteTextFormat*& slot = fonts_[static_cast<int>(spec.font)];
                release(slot);
                slot = format;
            };
            for (const FontSpec& spec : kIconFonts)
                make(L"Material Symbols Outlined", spec);
            make(L"PT Serif", kSerifPlay);
        }
    }
    release(collection);
    release(set);
    release(builder);
    release(loader);
    release(f5);
}

// The logo PNG and the background picture (none: logo() puts VanaCore's colours there, backdrop() draws nothing)
void Canvas::load_images(HMODULE module)
{
    load_image(module, L"LOGO", logo_img_);
    load_image(module, L"BACKDROP", backdrop_img_);
    if (!backdrop_img_.pixels.empty())
        frosted_img_.pixels = frost(backdrop_img_.pixels, backdrop_img_.w, backdrop_img_.h, 4, 4, frosted_img_.w, frosted_img_.h);
}

// An image (PNG, JPEG) from the module's RCDATA resource `name`, decoded to premultiplied BGRA
void Canvas::load_image(HMODULE module, const wchar_t* name, Image& out)
{
    const HRSRC found = FindResourceW(module, name, MAKEINTRESOURCEW(10)); // RT_RCDATA
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
        SUCCEEDED(converter->GetSize(&out.w, &out.h)) && out.w > 0 && out.h > 0;
    if (ok) {
        out.pixels.resize(static_cast<std::size_t>(out.w) * out.h);
        if (FAILED(converter->CopyPixels(nullptr, out.w * 4, static_cast<UINT>(out.pixels.size() * 4),
                                         reinterpret_cast<BYTE*>(out.pixels.data()))))
            out.pixels.clear();
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
        release(logo_img_.bitmap);
        release(backdrop_img_.bitmap);
        release(frosted_img_.bitmap);
        for (auto& [name, image] : pictures_)
            release(image.bitmap), release(image.sized);
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
    IDWriteTextLayout1* spaced = nullptr;
    if (made != nullptr && font == Font::Play &&
        SUCCEEDED(made->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&spaced)))) {
        // Play's letters a pixel apart, half each side (still centred)
        spaced->SetCharacterSpacing(0.5f, 0.5f, 0, {0, static_cast<UINT32>(text.size())});
        spaced->Release();
    }
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

// A line of text from `x`, its middle at `cy`, `top` at its top fading to `bottom` at its foot
void Canvas::text_shaded(const std::wstring& text, Font font, float x, float cy, D2D1_COLOR_F top, D2D1_COLOR_F bottom)
{
    IDWriteTextLayout* l = layout(text, font);
    if (l == nullptr)
        return;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    const float y = cy - m.height / 2;
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, top}, {1.0f, bottom}};
    ID2D1GradientStopCollection* collection = nullptr;
    ID2D1LinearGradientBrush* brush = nullptr;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &collection)) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({x, y}, {x, y + m.height}), collection, &brush)))
        rt_->DrawTextLayout({x, y}, l, brush);
    release(brush);
    release(collection);
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

// The image's bitmap on this render target, made the first time it's drawn (and again after the target is)
ID2D1Bitmap* Canvas::bitmap_of(Image& image)
{
    if (image.bitmap == nullptr && !image.pixels.empty())
        rt_->CreateBitmap(D2D1::SizeU(image.w, image.h), image.pixels.data(), image.w * 4,
                          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                          &image.bitmap);
    return image.bitmap;
}

// A dark shade over the card from `y0` to `y1`, `a0` opaque at y0 and `a1` at y1
void Canvas::shade(float y0, float y1, float a0, float a1)
{
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, rgb(pal::ink, a0)}, {1.0f, rgb(pal::ink, a1)}};
    ID2D1GradientStopCollection* collection = nullptr;
    ID2D1LinearGradientBrush* gradient = nullptr;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &collection)) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, y0}, {0, y1}), collection, &gradient)))
        rt_->FillRectangle(D2D1::RectF(0, y0, width_, y1), gradient);
    release(gradient);
    release(collection);
}

void Canvas::backdrop(const D2D1_RECT_F* glass)
{
    ID2D1Bitmap* bitmap = bitmap_of(backdrop_img_);
    ID2D1Bitmap* frosted = glass != nullptr ? bitmap_of(frosted_img_) : nullptr;
    if (bitmap == nullptr)
        return;
    const float w = width_, h = height_;
    ID2D1RoundedRectangleGeometry* shape = nullptr;
    ID2D1Layer* layer = nullptr;
    if (SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), kRadius, kRadius), &shape)) &&
        SUCCEEDED(rt_->CreateLayer(&layer))) {
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape), layer); // inside the card's round corners
        // the picture covering the card, its middle in the card's middle
        const float iw = static_cast<float>(backdrop_img_.w), ih = static_cast<float>(backdrop_img_.h);
        const float k = std::max(w / iw, h / ih), dw = iw * k, dh = ih * k;
        const D2D1_RECT_F cover = D2D1::RectF((w - dw) / 2, (h - dh) / 2, (w + dw) / 2, (h + dh) / 2);
        rt_->DrawBitmap(bitmap, cover, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        // a dark tint over it all, deeper under the head and down to the bottom's
        brush_->SetColor(rgb(pal::ink, 0.35f));
        rt_->FillRectangle(D2D1::RectF(0, 0, w, h), brush_);
        shade(0, h * 0.38f, 0.7f, 0.0f);
        shade(h * 0.36f, h, 0.0f, 0.75f);
        if (glass != nullptr) {
            // the glass: the same picture blurred where it is (the frosted one, stretched the same), darkened, a hairline on top
            rt_->PushAxisAlignedClip(*glass, D2D1_ANTIALIAS_MODE_ALIASED);
            if (frosted != nullptr)
                rt_->DrawBitmap(frosted, cover, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            brush_->SetColor(rgb(pal::ink, 0.5f));
            rt_->FillRectangle(*glass, brush_);
            brush_->SetColor(rgb(pal::white, 0.035f));
            rt_->FillRectangle(*glass, brush_);
            rt_->PopAxisAlignedClip();
            line(glass->left, glass->top + 0.5f, glass->right, glass->top + 0.5f, rgb(pal::white, 0.12f));
        }
        rt_->PopLayer();
    }
    release(layer);
    release(shape);
    outline(0, 0, w, h, kRadius, rgb(pal::border));
}

Canvas::Image* Canvas::named(const wchar_t* name)
{
    for (auto& [have, image] : pictures_)
        if (have == name)
            return image.pixels.empty() ? nullptr : &image;
    pictures_.push_back({name, Image{}});
    Image& image = pictures_.back().second;
    load_image(module_, name, image);
    return image.pixels.empty() ? nullptr : &image;
}

D2D1_SIZE_U Canvas::picture_size(const wchar_t* name)
{
    const Image* image = named(name);
    return image != nullptr ? D2D1::SizeU(image->w, image->h) : D2D1::SizeU(0, 0);
}

// `image` scaled to w x h pixels by WIC's Fant (an average over each pixel's whole footprint: a picture shrunk 3 or 4 times
// keeps its thin lines, where Direct2D's linear sampling here skips them), made again only when the size changes
ID2D1Bitmap* Canvas::sized_of(Image& image, UINT w, UINT h)
{
    if (image.sized != nullptr && image.sized_w == w && image.sized_h == h)
        return image.sized;
    release(image.sized);
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* source = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * h);
    const bool ok =
        SUCCEEDED(CoCreateInstance(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromMemory(image.w, image.h, kWicPbgra, image.w * 4, static_cast<UINT>(image.pixels.size() * 4),
                                              reinterpret_cast<BYTE*>(image.pixels.data()), &source)) &&
        SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(source, w, h, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(scaler->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
    release(scaler);
    release(source);
    release(wic);
    if (ok && SUCCEEDED(rt_->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4,
                                          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                                          &image.sized)))
        image.sized_w = w, image.sized_h = h;
    return image.sized;
}

bool Canvas::picture(const wchar_t* name, const D2D1_RECT_F& r, float t, bool on)
{
    Image* image = named(name);
    if (image == nullptr || bitmap_of(*image) == nullptr)
        return false;
    if (!on)
        t = 0;
    // its pixels: `r` at this DPI, its top left on a whole pixel, so the scaled bitmap is drawn one to one
    const auto snap = [&](float v) { return std::round((v + kShadow) * scale_) / scale_ - kShadow; };
    const UINT pw = static_cast<UINT>(std::lround((r.right - r.left) * scale_)), ph = static_cast<UINT>(std::lround((r.bottom - r.top) * scale_));
    const float left = snap(r.left), top = snap(r.top);
    D2D1_RECT_F at = D2D1::RectF(left, top, left + pw / scale_, top + ph / scale_);
    ID2D1Bitmap* bitmap = pw > 0 && ph > 0 ? sized_of(*image, pw, ph) : nullptr;
    if (bitmap == nullptr)
        bitmap = image->bitmap;
    // pressed: a little smaller, round its middle
    if (t > 0.8f) {
        const float dx = (at.right - at.left) * 0.01f, dy = (at.bottom - at.top) * 0.01f;
        at = D2D1::RectF(at.left + dx, at.top + dy, at.right - dx, at.bottom - dy);
    }
    rt_->DrawBitmap(bitmap, at, on ? 1.0f : 0.45f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (t > 0) {
        // lighter under the mouse, darker pressed: a wash in the picture's own shape (its alpha the mask)
        brush_->SetColor(t > 0.8f ? rgb(pal::ink, 0.22f) : rgb(pal::white, 0.14f));
        const D2D1_ANTIALIAS_MODE was = rt_->GetAntialiasMode();
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED); // (FillOpacityMask's need)
        rt_->FillOpacityMask(bitmap, brush_, D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &at, nullptr);
        rt_->SetAntialiasMode(was);
    }
    return true;
}

void Canvas::logo(float x, float y, float size)
{
    if (ID2D1Bitmap* bitmap = bitmap_of(logo_img_)) {
        rt_->DrawBitmap(bitmap, D2D1::RectF(x, y, x + size, y + size), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
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

void Canvas::glyph(wchar_t code, Font font, float cx, float cy, D2D1_COLOR_F colour)
{
    if (fonts_[static_cast<int>(font)] != nullptr)
        text_centred(std::wstring(1, code), font, cx, cy, colour);
}

void Canvas::bar(float x, float y, float w, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    fill(x, y, w, 4, 2, rgb(pal::white, 0.1f));
    if (t > 0)
        fill(x, y, std::max(4.0f, w * t), 4, 2, rgb(pal::accent));
}

// A rounded rectangle filled from `c0` to `c1`: left to right (`across`), or top to bottom
void Canvas::gradient(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c0, D2D1_COLOR_F c1, bool across)
{
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, c0}, {1.0f, c1}};
    ID2D1GradientStopCollection* collection = nullptr;
    ID2D1LinearGradientBrush* brush = nullptr;
    const D2D1_POINT_2F to = across ? D2D1::Point2F(r.right, r.top) : D2D1::Point2F(r.left, r.bottom);
    if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &collection)) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({r.left, r.top}, to), collection, &brush)))
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    release(brush);
    release(collection);
}

// Button::Play's face: a soft drop shadow, deep blue to bright blue left to right, a gloss on its upper half and a bright cyan
// border; lighter under the mouse, pressed: a pixel down on a shorter shadow
void Canvas::play_face(const D2D1_RECT_F& at, float t, float alpha)
{
    constexpr float kR = 4;
    const float press = t >= 1.0f ? 1.0f : 0.0f, glow = t >= 1.0f ? 0.0f : t;
    const D2D1_RECT_F r = D2D1::RectF(at.left, at.top + press, at.right, at.bottom + press);
    // the shadow: rings each a little wider and fainter, below and round it (no blur effect on a DC render target)
    const float drop = 4 - press * 2;
    for (int i = 6; i >= 1; --i) {
        const float g = static_cast<float>(i);
        fill(r.left - g, r.top - g + drop, r.right - r.left + 2 * g, r.bottom - r.top + 2 * g, kR + g,
             rgb(0x020818, 0.075f * alpha));
    }
    // the body, blue across, lighter under the mouse
    gradient(r, kR, mix(0x1438B8, 0x1E4FD8, glow, alpha), mix(0x2A8CF2, 0x45A8FF, glow, alpha), true);
    // the gloss: white fading out down the upper half
    gradient(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.top + (r.bottom - r.top) * 0.55f), kR - 1,
             rgb(pal::white, (0.22f + glow * 0.08f) * alpha), rgb(pal::white, 0), false);
    // the border: bright cyan, its glow a fainter ring outside it under the mouse
    if (glow > 0) {
        brush_->SetColor(rgb(0x5FF0FF, 0.35f * glow * alpha));
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1, r.top - 1, r.right + 1, r.bottom + 1), kR + 1, kR + 1),
                                  brush_, 2.0f);
    }
    brush_->SetColor(rgb(0x5FF0FF, alpha));
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), kR, kR),
                              brush_, 1.0f);
}

float Canvas::button_width(const std::wstring& label) const
{
    return std::max(84.0f, text_width(label, Font::Button) + 32);
}

void Canvas::button(const D2D1_RECT_F& r, const std::wstring& label, Button style, float t, bool on, wchar_t icon, Font font)
{
    const float w = r.right - r.left, h = r.bottom - r.top, alpha = on ? 1.0f : 0.45f;
    if (!on)
        t = 0;
    D2D1_COLOR_F fg;
    if (style == Button::Play) {
        play_face(r, t, alpha);
        fg = rgb(pal::white, on ? 1.0f : 0.7f);
    } else if (style == Button::Accent) {
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
    const Font icons = font == Font::Play ? Font::IconBig : Font::Icon;
    const bool play = style == Button::Play;
    const float cy = r.top + h / 2 + (play && t >= 1 ? 1.0f : 0.0f);
    const D2D1_COLOR_F under = rgb(0x06123A, 0.55f * alpha); // Play's: a dark shadow a pixel under its label
    if (icon == 0 || fonts_[static_cast<int>(icons)] == nullptr) {
        if (play) {
            // a dark shadow behind it, soft: a faint spread round a darker copy two pixels down
            const float lx = r.left + (w - text_width(label, font)) / 2;
            for (const auto& [dx, dy, a] : {std::tuple{-1.0f, 2.0f, 0.3f}, {1.0f, 2.0f, 0.3f}, {0.0f, 3.0f, 0.35f}, {0.0f, 2.0f, 0.7f}})
                text(label, font, lx + dx, cy + dy, rgb(0x020818, a * alpha));
            // white at the top to a little less white (a cool grey) at the bottom
            text_shaded(label, font, lx, cy, rgb(pal::white, fg.a), rgb(0xD6E2F2, fg.a));
            return;
        }
        text_centred(label, font, r.left + w / 2, cy, fg);
        return;
    }
    // the icon and the label side by side, the two in the middle
    const float iw = text_width(std::wstring(1, icon), icons), gap = 6, lw = text_width(label, font);
    const float x = r.left + (w - iw - gap - lw) / 2;
    if (play) {
        glyph(icon, icons, x + iw / 2, cy + 1, under);
        text(label, font, x + iw + gap, cy + 1, under);
    }
    glyph(icon, icons, x + iw / 2, cy, fg);
    text(label, font, x + iw + gap, cy, fg);
}

} // namespace vcapp::ui
