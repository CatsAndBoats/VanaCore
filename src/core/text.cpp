#include "text.hpp"

#include <dwrite.h>
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <wincodec.h>

#include <thread>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <mutex>
#include <unordered_map>

#include "d3d8.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")

namespace vanacore::text {
namespace {

using d3d8::call;
namespace slot = d3d8::slot;

std::atomic<bool> g_gdi_only{false}; // [client] truefont.gdi: GDI draws every text (set_gdi_only)

HFONT create_font(const Style& style)
{
    // Grayscale antialiasing: ClearType would put colour fringes into the coverage.
    return CreateFontW(-style.size, 0, 0, 0, style.weight, style.italic ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, style.family.c_str());
}

// A stretch of one line in one colour: `colour` indexes Layout::colours, -1 is the style's colour.
struct Run {
    std::wstring text;
    int colour = -1;
};

struct Layout {
    std::vector<std::vector<Run>> lines;
    std::vector<std::array<float, 3>> colours;
};

// The text's lines (split at \n), each in runs split at colour codes when the style has them on.
Layout parse(const std::wstring& text, bool codes)
{
    Layout layout;
    layout.lines.emplace_back();
    int colour = -1;
    std::wstring pending;
    const auto flush = [&] {
        if (!pending.empty())
            layout.lines.back().push_back({pending, colour});
        pending.clear();
    };
    for (size_t at = 0; at < text.size();) {
        const wchar_t c = text[at];
        if (c == L'\n') {
            flush();
            layout.lines.emplace_back();
            ++at;
        } else if (c == L'\r') {
            ++at;
        } else if (c == L'\t') {
            pending += L"    ";
            ++at;
        } else if (codes && text.compare(at, 4, L"\\cs(") == 0 && text.find(L')', at) != std::wstring::npos) {
            const size_t close = text.find(L')', at);
            int red = 0, green = 0, blue = 0;
            if (swscanf_s(text.substr(at + 4, close - at - 4).c_str(), L"%d,%d,%d", &red, &green, &blue) == 3) {
                flush();
                const auto channel = [](int value) { return static_cast<float>(value < 0 ? 0 : value > 255 ? 255 : value); };
                layout.colours.push_back({channel(red), channel(green), channel(blue)});
                colour = static_cast<int>(layout.colours.size()) - 1;
                at = close + 1;
            } else {
                pending += c;
                ++at;
            }
        } else if (codes && text.compare(at, 3, L"\\cr") == 0) {
            flush();
            colour = -1;
            at += 3;
        } else if (codes && text.compare(at, 2, L"|c") == 0 && at + 11 <= text.size() && text[at + 10] == L'|' &&
                   text.find_first_not_of(L"0123456789abcdefABCDEF", at + 2) >= at + 10) {
            // Ashita's |cAARRGGBB| (the alpha isn't used here) ...
            flush();
            const unsigned long argb = std::wcstoul(text.substr(at + 2, 8).c_str(), nullptr, 16);
            layout.colours.push_back({static_cast<float>((argb >> 16) & 0xFF), static_cast<float>((argb >> 8) & 0xFF),
                                      static_cast<float>(argb & 0xFF)});
            colour = static_cast<int>(layout.colours.size()) - 1;
            at += 11;
        } else if (codes && text.compare(at, 2, L"|r") == 0) {
            // ... and its |r, back to the style's colour.
            flush();
            colour = -1;
            at += 2;
        } else {
            pending += c;
            ++at;
        }
    }
    flush();
    return layout;
}

// ---- Coverage: how much of each pixel the text covers --------------------------------------------
// DirectWrite lays the text out at the font's own widths and draws its letters where they fall,
// fractions of a pixel included, antialiased in grey (natural symmetric rendering). GDI, the fallback,
// snaps every letter's advance and outline to whole pixels, so letters sit unevenly apart at the
// sizes the interface uses ("Supersam pling").

// Where a line's runs go: each run's left edge and width, in pixels from the box's left edge.
struct Placed {
    float x = 0.0f;
    float width = 0.0f;
};

// The text measured and drawn: the box's size, the line height, each run's place (lines, then runs,
// in order) and a coverage byte per pixel of the box.
struct Coverage {
    int width = 0, height = 0, line_height = 0;
    int pad_y = 0; // the first line's top
    int baseline = 0; // a line's baseline below its top
    float middle = 0.0f; // a line's middle above its baseline, unrounded
    std::vector<std::vector<Placed>> placed;
    std::vector<std::uint8_t> values;
};

// The box around text `text_width` wide on `lines` lines of `line_height`, with the style's padding.
void box_size(const Style& style, float text_width, size_t lines, int line_height, int& pad_x, int& pad_y, int& width,
              int& height)
{
    // Without a set padding, the text's distance from the edge of its box grows with the font (and
    // leaves room for a thin stroke); a set padding gets the stroke's room added.
    const int stroke = style.stroke < 0 ? 0 : style.stroke > 8 ? 8 : style.stroke;
    const int room = style.padding >= 0 ? stroke : 0;
    pad_x = (style.padding >= 0 ? style.padding : (std::max)(style.size / 2, 4)) + room;
    pad_y = (style.padding >= 0 ? style.padding : (std::max)(style.size / 4, 2)) + room;
    // Italic glyphs lean past their advance on the right.
    const int lean = style.italic ? (style.size + 3) / 4 : 0;
    width = static_cast<int>(std::ceil(text_width)) + lean + 2 * pad_x;
    height = static_cast<int>(lines) * line_height + 2 * pad_y;
}

#include "text_fit.h" // ink_margin, fit_to_ink: the box grown to hold the ink that leaves it

// Each run's left edge on its line (right-aligned lines against the widest), from the runs' widths.
void place_runs(const Style& style, int pad_x, float text_width, std::vector<std::vector<Placed>>& placed)
{
    for (auto& line : placed) {
        float line_width = 0.0f;
        for (const Placed& run : line)
            line_width += run.width;
        float x = pad_x + (style.align_right ? text_width - line_width : 0.0f);
        for (Placed& run : line) {
            run.x = x;
            x += run.width;
        }
    }
}

IDWriteFactory* dwrite()
{
    static IDWriteFactory* const factory = [] {
        IUnknown* made = nullptr;
        return SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &made))
                   ? static_cast<IDWriteFactory*>(made)
                   : nullptr;
    }();
    return factory;
}

// Grey antialiasing (no ClearType: its colour fringes would be taken as coverage), the font's own
// letter shapes and places (natural symmetric), and the gamma and contrast Windows draws light text
// on dark with.
IDWriteRenderingParams* grey_rendering()
{
    static IDWriteRenderingParams* const params = [] {
        IDWriteRenderingParams* made = nullptr;
        if (IDWriteFactory* factory = dwrite())
            factory->CreateCustomRenderingParams(1.8f, 0.5f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                                 DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &made);
        return made;
    }();
    return params;
}

// As grey_rendering, the letters not fitted to whole pixels (Style::scalable); null without DirectWrite 2 (Windows 8.1)
IDWriteRenderingParams* scalable_rendering()
{
    static IDWriteRenderingParams* const params = [] {
        IDWriteFactory2* factory = nullptr;
        IDWriteRenderingParams2* made = nullptr;
        if (IDWriteFactory* base = dwrite();
            base != nullptr && SUCCEEDED(base->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void**>(&factory)))) {
            factory->CreateCustomRenderingParams(1.8f, 0.5f, 0.5f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                                 DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_GRID_FIT_MODE_DISABLED,
                                                 &made);
            factory->Release();
        }
        return static_cast<IDWriteRenderingParams*>(made);
    }();
    return params;
}

// As grey_rendering, the letters fitted to whole pixels across as GDI fits them (Style::crisp: laid out GDI-compatible)
IDWriteRenderingParams* crisp_rendering()
{
    static IDWriteRenderingParams* const params = [] {
        IDWriteRenderingParams* made = nullptr;
        if (IDWriteFactory* factory = dwrite())
            factory->CreateCustomRenderingParams(1.8f, 0.5f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_RENDERING_MODE_GDI_CLASSIC,
                                                 &made);
        return made;
    }();
    return params;
}

// Draws a layout's glyph runs, white, into a DirectWrite bitmap (the coverage).
class CoverageRenderer final : public IDWriteTextRenderer {
public:
    CoverageRenderer(IDWriteBitmapRenderTarget* target, IDWriteRenderingParams* params) : target_(target), params_(params) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; } // on the stack, for one render
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override
    {
        *disabled = FALSE; // each run starts on a whole pixel; the letters in it fall where they fall
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    {
        *transform = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override
    {
        *pixels = 1.0f; // sizes are in pixels already
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override
    {
        return target_->DrawGlyphRun(x, y, mode, run, params_, RGB(255, 255, 255), nullptr);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override
    {
        return S_OK;
    }

private:
    IDWriteBitmapRenderTarget* target_;
    IDWriteRenderingParams* params_;
};

template <typename T>
void release(T*& object)
{
    if (object != nullptr)
        object->Release();
    object = nullptr;
}

// True when DirectWrite has the font family: one installed in Windows. The fonts VanaCore loads from
// its fonts\ folder (the icons' Material Symbols) are GDI's alone (AddFontMemResourceEx), so GDI draws
// those. Asked once a family.
bool dwrite_has(IDWriteFactory* factory, const std::wstring& family)
{
    static std::mutex lock;
    static std::unordered_map<std::wstring, bool> known;
    const std::lock_guard guard(lock);
    if (const auto found = known.find(family); found != known.end())
        return found->second;
    bool exists = false;
    IDWriteFontCollection* fonts = nullptr;
    if (SUCCEEDED(factory->GetSystemFontCollection(&fonts, FALSE))) {
        UINT32 index = 0;
        BOOL found = FALSE;
        exists = SUCCEEDED(fonts->FindFamilyName(family.c_str(), &index, &found)) && found;
        fonts->Release();
    }
    known.emplace(family, exists);
    return exists;
}

// DirectWrite's objects for drawing text, made once and kept (a new format and bitmap for every text cost a frame
// its time when a window made a hundred labels at once). Only the game's render thread draws text; the lock is for safety.
std::mutex g_dwrite_lock;

// The font DirectWrite draws a style in: its family and the face in it
struct Face {
    std::wstring family;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
};

// A family DirectWrite hasn't by that name, named as GDI names its faces ("Segoe UI Semibold" is DirectWrite's "Segoe
// UI" at weight 600): the face GDI would pick for the style, as DirectWrite has it. False if it hasn't it.
bool gdi_face(IDWriteFactory* factory, const Style& style, Face& face)
{
    static std::mutex lock;
    static std::unordered_map<std::wstring, std::pair<bool, Face>> known;
    const std::wstring key = style.family + L'|' + std::to_wstring(style.weight) + (style.italic ? L"|i" : L"|n");
    const std::lock_guard guard(lock);
    if (const auto found = known.find(key); found != known.end()) {
        face = found->second.second;
        return found->second.first;
    }
    bool ok = false;
    IDWriteGdiInterop* gdi = nullptr;
    IDWriteFont* font = nullptr;
    IDWriteFontFamily* family = nullptr;
    IDWriteLocalizedStrings* names = nullptr;
    LOGFONTW logfont{};
    logfont.lfWeight = style.weight;
    logfont.lfItalic = style.italic ? TRUE : FALSE;
    logfont.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(logfont.lfFaceName, style.family.c_str(), _TRUNCATE);
    if (SUCCEEDED(factory->GetGdiInterop(&gdi)) && SUCCEEDED(gdi->CreateFontFromLOGFONT(&logfont, &font)) &&
        SUCCEEDED(font->GetFontFamily(&family)) && SUCCEEDED(family->GetFamilyNames(&names)) && names->GetCount() > 0) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists)
            index = 0;
        UINT32 length = 0;
        if (SUCCEEDED(names->GetStringLength(index, &length))) {
            std::wstring name(length + 1, L'\0');
            if (SUCCEEDED(names->GetString(index, name.data(), length + 1))) {
                name.resize(length);
                face = {name, font->GetWeight(), font->GetStyle(), font->GetStretch()};
                ok = true;
            }
        }
    }
    for (IUnknown* object : {static_cast<IUnknown*>(names), static_cast<IUnknown*>(family), static_cast<IUnknown*>(font),
                             static_cast<IUnknown*>(gdi)})
        if (object != nullptr)
            object->Release();
    known.emplace(key, std::make_pair(ok, face));
    return ok;
}

// A font's text format (no wrapping) and its line: the height (ascent, descent and gap) and baseline, whole pixels,
// and its middle above its baseline, unrounded.
struct FontFormat {
    IDWriteTextFormat* format = nullptr;
    int line_height = 0;
    int baseline = 0;
    float middle = 0.0f;
};

// The format for the style's size in `face`, made the first time it's asked for. Null if DirectWrite can't make it.
const FontFormat* font_format(IDWriteFactory* factory, const Style& style, const Face& face)
{
    static std::unordered_map<std::wstring, FontFormat> formats;
    const float size = style.exact_size > 0.0f ? style.exact_size : static_cast<float>(style.size);
    const std::wstring key = face.family + L'|' + std::to_wstring(size) + L'|' + std::to_wstring(face.weight) + L'|' +
                             std::to_wstring(face.style) + L'|' + std::to_wstring(face.stretch);
    if (const auto found = formats.find(key); found != formats.end())
        return &found->second;
    FontFormat made;
    if (FAILED(factory->CreateTextFormat(face.family.c_str(), nullptr, face.weight, face.style, face.stretch, size, L"",
                                         &made.format)))
        return nullptr;
    made.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteTextLayout* probe = nullptr;
    DWRITE_LINE_METRICS line{};
    UINT32 count = 0;
    const bool ok = SUCCEEDED(factory->CreateTextLayout(L" ", 1, made.format, 100000.0f, 100000.0f, &probe)) &&
                    SUCCEEDED(probe->GetLineMetrics(&line, 1, &count)) && count == 1;
    release(probe);
    if (!ok) {
        release(made.format);
        return nullptr;
    }
    made.line_height = static_cast<int>(std::ceil(line.height));
    made.baseline = static_cast<int>(std::lround(line.baseline));
    made.middle = line.baseline - line.height * 0.5f;
    if (formats.size() >= 256) { // a script making fonts of every size: start over rather than keep them all
        for (auto& [name, kept] : formats)
            release(kept.format);
        formats.clear();
    }
    return &formats.emplace(key, made).first->second;
}

// The bitmap the text is drawn into, at least `width` x `height`: one kept and grown (a text clears and reads only its
// own box of it). Past kCanvasMost a side, one of the text's own size, released after it (`own` true).
constexpr int kCanvasMost = 2048;

IDWriteBitmapRenderTarget* canvas(IDWriteFactory* factory, int width, int height, bool& own)
{
    static IDWriteGdiInterop* gdi = nullptr;
    static IDWriteBitmapRenderTarget* kept = nullptr;
    static int kept_width = 0, kept_height = 0;
    own = false;
    if (gdi == nullptr && FAILED(factory->GetGdiInterop(&gdi)))
        return nullptr;
    IDWriteBitmapRenderTarget* target = nullptr;
    if (width > kCanvasMost || height > kCanvasMost) {
        own = true;
        return SUCCEEDED(gdi->CreateBitmapRenderTarget(nullptr, static_cast<UINT32>(width), static_cast<UINT32>(height), &target))
                   ? target
                   : nullptr;
    }
    if (kept != nullptr && width <= kept_width && height <= kept_height)
        return kept;
    // grown a step past what this text needs, so the next a little bigger doesn't grow it again
    const int grown_width = (std::min)(kCanvasMost, ((std::max)(width, kept_width) + 255) / 256 * 256);
    const int grown_height = (std::min)(kCanvasMost, ((std::max)(height, kept_height) + 63) / 64 * 64);
    if (kept != nullptr && SUCCEEDED(kept->Resize(static_cast<UINT32>(grown_width), static_cast<UINT32>(grown_height)))) {
        kept_width = grown_width, kept_height = grown_height;
        return kept;
    }
    release(kept);
    kept_width = kept_height = 0;
    if (FAILED(gdi->CreateBitmapRenderTarget(nullptr, static_cast<UINT32>(grown_width), static_cast<UINT32>(grown_height), &kept)))
        return nullptr;
    kept->SetPixelsPerDip(1.0f);
    kept_width = grown_width, kept_height = grown_height;
    return kept;
}

// The lines laid out and drawn with DirectWrite. False if it can't (then GDI draws them).
bool cover_dwrite(const Layout& layout, const Style& style, Coverage& out)
{
    IDWriteFactory* factory = dwrite();
    IDWriteRenderingParams* params = style.scalable && scalable_rendering() != nullptr ? scalable_rendering()
                                     : style.crisp && crisp_rendering() != nullptr   ? crisp_rendering()
                                                                                     : grey_rendering();
    if (factory == nullptr || params == nullptr)
        return false;
    Face face{style.family, static_cast<DWRITE_FONT_WEIGHT>(style.weight),
              style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL};
    // a face's GDI name only for scaled text, which GDI's whole-pixel letters don't scale smoothly: the rest as it was
    if (!dwrite_has(factory, style.family) && !(style.scalable && gdi_face(factory, style, face)))
        return false;
    const std::lock_guard guard(g_dwrite_lock);
    const FontFormat* font = font_format(factory, style, face);
    if (font == nullptr)
        return false;
    IDWriteTextFormat* format = font->format;
    bool spaced_runs = false; // the style's spacing applied (DirectWrite 1.1 there)
    const auto lay_out = [&](const std::wstring& run, IDWriteTextLayout*& made) {
        const UINT32 length = static_cast<UINT32>(run.size());
        const HRESULT laid = style.crisp // (whole-pixel widths, GDI's: Style::crisp)
                                 ? factory->CreateGdiCompatibleTextLayout(run.c_str(), length, format, 100000.0f, 100000.0f, 1.0f,
                                                                          nullptr, FALSE, &made)
                                 : factory->CreateTextLayout(run.c_str(), length, format, 100000.0f, 100000.0f, &made);
        if (FAILED(laid))
            return false;
        // the style's spacing after each letter (DirectWrite 1.1, Windows 8 on)
        if (style.spacing != 0.0f && !run.empty()) {
            IDWriteTextLayout1* spaced = nullptr;
            if (SUCCEEDED(made->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&spaced)))) {
                spaced_runs = SUCCEEDED(
                    spaced->SetCharacterSpacing(0.0f, style.spacing, 0.0f, {0, static_cast<UINT32>(run.size())}));
                spaced->Release();
            }
        }
        return true;
    };
    bool ok = true;
    out.line_height = font->line_height;
    out.baseline = font->baseline;
    out.middle = font->middle;

    // the runs, measured
    std::vector<std::vector<IDWriteTextLayout*>> runs(layout.lines.size());
    out.placed.assign(layout.lines.size(), {});
    float text_width = 0.0f;
    for (size_t l = 0; ok && l < layout.lines.size(); ++l) {
        float line_width = 0.0f;
        for (const Run& run : layout.lines[l]) {
            IDWriteTextLayout* made = nullptr;
            DWRITE_TEXT_METRICS metrics{};
            ok = lay_out(run.text, made) && SUCCEEDED(made->GetMetrics(&metrics));
            runs[l].push_back(made);
            if (!ok)
                break;
            // less the spacing after its last letter
            const float trailing = spaced_runs && !run.text.empty() ? style.spacing : 0.0f;
            const float width = (std::max)(0.0f, metrics.widthIncludingTrailingWhitespace - trailing);
            out.placed[l].push_back({0.0f, width});
            line_width += width;
        }
        text_width = (std::max)(text_width, line_width);
    }
    if (ok && text_width == 0.0f && layout.lines.size() == 1) { // no text: a box one space wide
        IDWriteTextLayout* space = nullptr;
        DWRITE_TEXT_METRICS metrics{};
        if (lay_out(L" ", space) && SUCCEEDED(space->GetMetrics(&metrics)))
            text_width = metrics.widthIncludingTrailingWhitespace;
        release(space);
    }

    // drawn white on black into the box's corner of the canvas, `margin` in from its edges (the ink past the box is kept:
    // fit_to_ink): the green channel is the coverage
    IDWriteBitmapRenderTarget* target = nullptr;
    bool own_target = false;
    int pad_x = 0, pad_y = 0;
    const int margin = ink_margin(style);
    if (ok) {
        box_size(style, text_width, layout.lines.size(), out.line_height, pad_x, pad_y, out.width, out.height);
        out.pad_y = pad_y;
        place_runs(style, pad_x, text_width, out.placed);
        target = canvas(factory, out.width + 2 * margin, out.height + 2 * margin, own_target);
        ok = target != nullptr;
    }
    if (ok) {
        const int canvas_width = out.width + 2 * margin, canvas_height = out.height + 2 * margin;
        target->SetPixelsPerDip(1.0f);
        HDC dc = target->GetMemoryDC();
        PatBlt(dc, 0, 0, canvas_width, canvas_height, BLACKNESS);
        CoverageRenderer renderer(target, params);
        for (size_t l = 0; ok && l < runs.size(); ++l)
            for (size_t r = 0; ok && r < runs[l].size(); ++r)
                ok = SUCCEEDED(runs[l][r]->Draw(nullptr, &renderer, out.placed[l][r].x + static_cast<float>(margin),
                                                static_cast<float>(margin + pad_y + static_cast<int>(l) * out.line_height)));
        DIBSECTION dib{};
        ok = ok && GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof dib, &dib) == sizeof dib && dib.dsBm.bmBits != nullptr &&
             dib.dsBm.bmBitsPixel == 32;
        if (ok) {
            // top-down (GetObject reports its height positive all the same)
            std::vector<std::uint8_t> drawn(static_cast<size_t>(canvas_width) * canvas_height);
            for (int y = 0; y < canvas_height; ++y) {
                const auto* row = static_cast<const std::uint8_t*>(dib.dsBm.bmBits) + static_cast<size_t>(y) * dib.dsBm.bmWidthBytes;
                for (int x = 0; x < canvas_width; ++x)
                    drawn[static_cast<size_t>(y) * canvas_width + x] = row[x * 4 + 1];
            }
            fit_to_ink(out, drawn, margin, style.stroke);
        }
    }
    if (own_target)
        release(target);
    for (auto& line_runs : runs)
        for (IDWriteTextLayout*& made : line_runs)
            release(made);
    return ok;
}

// The lines laid out and drawn with GDI (each letter on a whole pixel).
void cover_gdi(const Layout& layout, const Style& style, Coverage& out)
{
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr)
        return;
    HFONT font = create_font(style);
    const HGDIOBJ old_font = SelectObject(dc, font);
    SetTextCharacterExtra(dc, static_cast<int>(std::lround(style.spacing))); // the style's spacing, whole pixels
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    out.line_height = metrics.tmHeight;
    out.baseline = metrics.tmAscent;
    out.middle = static_cast<float>(metrics.tmAscent) - static_cast<float>(metrics.tmHeight) * 0.5f;

    // DrawText lays the runs out (as the single-colour renderer before it did, to the pixel).
    constexpr UINT kFormat = DT_SINGLELINE | DT_NOPREFIX | DT_LEFT | DT_TOP | DT_NOCLIP;
    const auto measure = [&](const std::wstring& run) {
        RECT bounds{};
        DrawTextW(dc, run.c_str(), static_cast<int>(run.size()), &bounds, DT_CALCRECT | kFormat);
        return static_cast<float>(bounds.right);
    };
    out.placed.assign(layout.lines.size(), {});
    float text_width = 0.0f;
    for (size_t l = 0; l < layout.lines.size(); ++l) {
        float line_width = 0.0f;
        for (const Run& run : layout.lines[l]) {
            out.placed[l].push_back({0.0f, measure(run.text)});
            line_width += out.placed[l].back().width;
        }
        text_width = (std::max)(text_width, line_width);
    }
    if (text_width == 0.0f && layout.lines.size() == 1) // no text: a box one space wide
        text_width = measure(L" ");
    int pad_x = 0, pad_y = 0;
    box_size(style, text_width, layout.lines.size(), out.line_height, pad_x, pad_y, out.width, out.height);
    out.pad_y = pad_y;
    place_runs(style, pad_x, text_width, out.placed);

    // the box `margin` in from the edges of a bitmap that much bigger (the ink past the box is kept: fit_to_ink)
    const int margin = ink_margin(style);
    const int canvas_width = out.width + 2 * margin, canvas_height = out.height + 2 * margin;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), canvas_width, -canvas_height, 1, 32, BI_RGB}; // top-down
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
        const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        const size_t count = static_cast<size_t>(canvas_width) * canvas_height;
        std::memset(bits, 0, count * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        const int lean = style.italic ? (style.size + 3) / 4 : 0;
        for (size_t l = 0; l < layout.lines.size(); ++l) {
            const int y = margin + pad_y + static_cast<int>(l) * out.line_height;
            for (size_t r = 0; r < layout.lines[l].size(); ++r) {
                const Placed& at = out.placed[l][r];
                const int x = margin + static_cast<int>(at.x);
                RECT area{x, y, x + static_cast<int>(at.width) + lean, y + out.line_height};
                const std::wstring& run = layout.lines[l][r].text;
                DrawTextW(dc, run.c_str(), static_cast<int>(run.size()), &area, kFormat);
            }
        }
        GdiFlush();
        // GDI leaves alpha alone, so the grey level is the text's coverage.
        const auto* source = static_cast<const std::uint32_t*>(bits);
        std::vector<std::uint8_t> drawn(count);
        for (size_t i = 0; i < count; ++i)
            drawn[i] = static_cast<std::uint8_t>((source[i] >> 8) & 0xFF);
        fit_to_ink(out, drawn, margin, style.stroke);
        SelectObject(dc, old_bitmap);
    }
    if (bitmap != nullptr)
        DeleteObject(bitmap);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
}

// The text's straight-alpha ARGB image on its box: `width` x `height`, into `pixels`; its first line's baseline in the
// image's rows into `baseline`, where that line's pen starts in its columns into `pen_left` and how far it moves into
// `advance` (Sprite::pen_left, advance), and those unrounded into `exact` (Sprite::middle, exact_advance).
struct Exact {
    float middle = 0.0f;
    float advance = 0.0f;
};

// Whether the coverage's row `y` has a drawn pixel
bool row_drawn(const Coverage& cover, int y)
{
    const std::uint8_t* row = &cover.values[static_cast<size_t>(y) * cover.width];
    return std::any_of(row, row + cover.width, [](std::uint8_t v) { return v != 0; });
}

// Whether the coverage's column `x` has a drawn pixel
bool column_drawn(const Coverage& cover, int x)
{
    for (int y = 0; y < cover.height; ++y)
        if (cover.values[static_cast<size_t>(y) * cover.width + x] != 0)
            return true;
    return false;
}

// The fill's colour on each of the image's rows (red, green, blue, three floats a row) for Style::gradient: from the highest
// drawn pixel to the first line's `baseline` (its lowest drawn pixel when the baseline isn't below it); with
// Style::gradient_across on each of its columns, from the leftmost drawn pixel to the rightmost. Empty without one.
std::vector<float> gradient_rows(const Style& style, const Coverage& cover, int baseline)
{
    const int stops = style.gradient == 2 || style.gradient == 3 ? style.gradient : 0;
    if (stops == 0 || cover.height <= 0 || cover.width <= 0)
        return {};
    const bool across = style.gradient_across;
    const int count = across ? cover.width : cover.height;
    const auto drawn = [&](int at) { return across ? column_drawn(cover, at) : row_drawn(cover, at); };
    int top = 0;
    while (top < count && !drawn(top))
        ++top;
    int bottom = across ? -1 : baseline;
    if (bottom <= top + 1) {
        bottom = count - 1;
        while (bottom > top && !drawn(bottom))
            --bottom;
    }
    const float span = static_cast<float>((std::max)(1, bottom - top));
    std::vector<float> rows(static_cast<size_t>(count) * 3);
    for (int y = 0; y < count; ++y) {
        float t = (std::clamp)((static_cast<float>(y - top) + 0.5f) / span, 0.0f, 1.0f);
        int from = 0;
        if (stops == 3) { // two halves: the first colour to the second, the second to the third
            from = t < 0.5f ? 0 : 1;
            t = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
        }
        for (int c = 0; c < 3; ++c) {
            const float a = style.gradient_colors[from][c], b = style.gradient_colors[from + 1][c];
            rows[static_cast<size_t>(y) * 3 + c] = a + (b - a) * t;
        }
    }
    return rows;
}

void draw_image(const std::wstring& text, const Style& style, int& width, int& height, std::vector<std::uint32_t>& pixels,
                int& baseline, int& pen_left, int& advance, Exact& exact)
{
    width = height = baseline = pen_left = advance = 0;
    exact = Exact{};
    pixels.clear();
    const Layout layout = parse(text, style.color_codes);
    Coverage cover;
    if (g_gdi_only.load(std::memory_order_relaxed) || !cover_dwrite(layout, style, cover)) {
        cover = Coverage{};
        cover_gdi(layout, style, cover);
    }
    if (cover.values.empty() || cover.width <= 0 || cover.height <= 0)
        return;
    const int stroke = style.stroke < 0 ? 0 : style.stroke > 8 ? 8 : style.stroke;
    if (style.trim) {
        // the box cut to the drawn pixels (grown by the outline), `padding` round them
        int x0 = cover.width, y0 = cover.height, x1 = -1, y1 = -1;
        for (int y = 0; y < cover.height; ++y)
            for (int x = 0; x < cover.width; ++x)
                if (cover.values[static_cast<size_t>(y) * cover.width + x] != 0)
                    x0 = (std::min)(x0, x), x1 = (std::max)(x1, x), y0 = (std::min)(y0, y), y1 = (std::max)(y1, y);
        if (x1 >= 0) {
            const int pad = (style.padding > 0 ? style.padding : 0) + stroke;
            const int w = x1 - x0 + 1 + 2 * pad, h = y1 - y0 + 1 + 2 * pad;
            std::vector<std::uint8_t> cut(static_cast<size_t>(w) * h, 0);
            for (int y = y0; y <= y1; ++y)
                std::memcpy(&cut[static_cast<size_t>(y - y0 + pad) * w + pad], &cover.values[static_cast<size_t>(y) * cover.width + x0],
                            static_cast<size_t>(x1 - x0 + 1));
            cover.values = std::move(cut);
            cover.width = w, cover.height = h;
            cover.pad_y += pad - y0;
            for (auto& line : cover.placed)
                for (Placed& run : line)
                    run.x += static_cast<float>(pad - x0);
        }
    }
    width = cover.width;
    height = cover.height;
    baseline = cover.pad_y + cover.baseline;
    exact.middle = static_cast<float>(baseline) - cover.middle;
    if (!cover.placed.empty() && !cover.placed[0].empty()) {
        pen_left = static_cast<int>(std::lround(cover.placed[0][0].x));
        float line_width = 0.0f;
        for (const Placed& run : cover.placed[0])
            line_width += run.width;
        advance = static_cast<int>(std::lround(line_width));
        exact.advance = line_width;
    }
    const int lean = style.italic ? (style.size + 3) / 4 : 0;
    const size_t count = static_cast<size_t>(width) * height;

    // Which run's colour each pixel takes (-1: the style's), by the runs' boxes.
    std::vector<short> owner(layout.colours.empty() ? 0 : count, -1);
    if (!owner.empty()) {
        for (size_t l = 0; l < layout.lines.size(); ++l) {
            const int y = cover.pad_y + static_cast<int>(l) * cover.line_height;
            for (size_t r = 0; r < layout.lines[l].size(); ++r) {
                const int colour = layout.lines[l][r].colour;
                if (colour < 0)
                    continue;
                const Placed& at = cover.placed[l][r];
                const int left = (std::max)(0, static_cast<int>(std::floor(at.x)) - stroke);
                const int right = (std::min)(width, static_cast<int>(std::ceil(at.x + at.width)) + lean);
                for (int row = (std::max)(0, y - stroke); row < (std::min)(height, y + cover.line_height + stroke); ++row)
                    for (int column = left; column < right; ++column)
                        owner[static_cast<size_t>(row) * width + column] = static_cast<short>(colour);
            }
        }
    }

    const auto coverage_at = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return 0.0f;
        return static_cast<float>(cover.values[static_cast<size_t>(y) * width + x]) / 255.0f;
    };
    // Layers, bottom up: the box, the stroke (the text's coverage grown by `stroke` pixels in
    // every direction, so antialiased edges stay smooth), the text. Composited premultiplied,
    // then stored as straight alpha for SRCALPHA/INVSRCALPHA blending.
    const float* box = style.background;
    const float* line = style.stroke_color;
    // An outline between whole pixels wide: the outlines a whole pixel narrower and wider, mixed
    const float wide = style.exact_stroke > 0.0f ? (std::min)(style.exact_stroke, static_cast<float>(stroke))
                                                 : static_cast<float>(stroke);
    const int narrow_by = static_cast<int>(wide);
    const float mix = wide - static_cast<float>(narrow_by);
    // The outline is the coverage's most within a disc (dx*dx + dy*dy <= r*r + r) round each pixel, taken a row at a
    // time: across[k] is the coverage's most within k pixels left and right, so a pixel's is the most of across[k] in
    // the rows dy above and below it, k the disc's half-width at dy (2r+1 reads a pixel, not (2r+1)^2: a big number's
    // outline took tens of ms)
    std::vector<std::vector<std::uint8_t>> across(stroke > 0 ? stroke + 1 : 0);
    int outer[17] = {}, inner[17] = {}; // the discs' half-widths by dy + stroke (-1: outside it)
    if (stroke > 0) {
        across[0] = cover.values;
        for (int k = 1; k <= stroke; ++k) {
            across[k].resize(count);
            for (int y = 0; y < height; ++y) {
                const std::uint8_t* row = &cover.values[static_cast<size_t>(y) * width];
                const std::uint8_t* before = &across[k - 1][static_cast<size_t>(y) * width];
                std::uint8_t* out = &across[k][static_cast<size_t>(y) * width];
                std::copy(before, before + width, out);
                for (int x = k; x < width; ++x)
                    out[x] = (std::max)(out[x], row[x - k]);
                for (int x = 0; x + k < width; ++x)
                    out[x] = (std::max)(out[x], row[x + k]);
            }
        }
        const auto half_width = [](int radius, int dy) {
            const int room = radius * radius + radius - dy * dy;
            if (room < 0)
                return -1;
            int half = 0;
            while ((half + 1) * (half + 1) <= room)
                ++half;
            return half;
        };
        for (int dy = -stroke; dy <= stroke; ++dy) {
            outer[dy + stroke] = half_width(stroke, dy);
            inner[dy + stroke] = half_width(narrow_by, dy);
        }
    }
    const std::vector<float> row_ink = gradient_rows(style, cover, baseline);
    const bool graded = !row_ink.empty();
    pixels.resize(count);
    std::vector<std::uint8_t> row_outline(stroke > 0 ? width : 0), row_narrow(stroke > 0 ? width : 0);
    for (int y = 0; y < height; ++y) {
        if (stroke > 0) { // the row's outlines, a whole row at a time
            std::fill(row_outline.begin(), row_outline.end(), std::uint8_t{0});
            std::fill(row_narrow.begin(), row_narrow.end(), std::uint8_t{0});
            for (int dy = -stroke; dy <= stroke; ++dy) {
                const int row = y + dy;
                if (row < 0 || row >= height)
                    continue;
                const std::uint8_t* wide_row = &across[outer[dy + stroke]][static_cast<size_t>(row) * width];
                for (int x = 0; x < width; ++x)
                    row_outline[x] = (std::max)(row_outline[x], wide_row[x]);
                if (inner[dy + stroke] < 0)
                    continue;
                const std::uint8_t* narrow_row = &across[inner[dy + stroke]][static_cast<size_t>(row) * width];
                for (int x = 0; x < width; ++x)
                    row_narrow[x] = (std::max)(row_narrow[x], narrow_row[x]);
            }
        }
        for (int x = 0; x < width; ++x) {
            const size_t at = static_cast<size_t>(y) * width + x;
            const float* ink = graded ? &row_ink[static_cast<size_t>(style.gradient_across ? x : y) * 3] : style.color;
            float run_colour[3];
            if (!owner.empty() && owner[at] >= 0) {
                const auto& chosen = layout.colours[static_cast<size_t>(owner[at])];
                run_colour[0] = chosen[0];
                run_colour[1] = chosen[1];
                run_colour[2] = chosen[2];
                ink = run_colour;
            }
            const float glyph = coverage_at(x, y) * style.color[3];
            float outline = 0.0f, narrow = 0.0f;
            if (stroke > 0) {
                outline = static_cast<float>(row_outline[x]) / 255.0f;
                narrow = static_cast<float>(row_narrow[x]) / 255.0f;
            }
            if (mix > 0.0f)
                outline = narrow + (outline - narrow) * mix;
            else if (narrow_by < stroke)
                outline = narrow;
            float red = box[0] * box[3], green = box[1] * box[3], blue = box[2] * box[3], alpha = box[3];
            const auto over = [&](float r, float g, float b, float a) {
                red = r * a + red * (1.0f - a);
                green = g * a + green * (1.0f - a);
                blue = b * a + blue * (1.0f - a);
                alpha = a + alpha * (1.0f - a);
            };
            if (stroke > 0)
                over(line[0], line[1], line[2], outline * line[3]);
            over(ink[0], ink[1], ink[2], glyph);
            std::uint32_t& pixel = pixels[at];
            if (alpha <= 0.0f) {
                pixel = 0;
                continue;
            }
            const auto channel = [&](float premultiplied) {
                return static_cast<std::uint32_t>((std::min)(premultiplied / alpha, 255.0f) + 0.5f);
            };
            pixel = static_cast<std::uint32_t>(alpha * 255.0f + 0.5f) << 24 | channel(red) << 16 | channel(green) << 8 |
                    channel(blue);
        }
    }
}

// ---- Images drawn lately, kept: a window built again makes the same labels in new boxes ---------------------------
// Each is its text and whole style's image; the least lately used go first past kKeptBytes of pixels.
constexpr size_t kKeptBytes = 16u << 20;
constexpr size_t kKeptMostOne = 1u << 20; // a bigger image isn't kept

struct Kept {
    std::wstring text;
    Style style;
    size_t hash = 0;
    int width = 0, height = 0, baseline = 0, pen_left = 0, advance = 0;
    Exact exact;
    std::vector<std::uint32_t> pixels;
};

std::mutex g_kept_lock;
std::list<Kept> g_kept; // the latest used first
std::unordered_multimap<size_t, std::list<Kept>::iterator> g_kept_by_hash;
size_t g_kept_bytes = 0;

size_t style_hash(const std::wstring& text, const Style& style)
{
    std::uint64_t hash = 14695981039346656037ull; // FNV-1a
    const auto mix = [&hash](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    mix(text.data(), text.size() * sizeof(wchar_t));
    mix(style.family.data(), style.family.size() * sizeof(wchar_t));
    const int ints[] = {style.size, style.weight, style.stroke, style.padding,
                        (style.italic ? 1 : 0) | (style.align_right ? 2 : 0) | (style.color_codes ? 4 : 0) | (style.trim ? 8 : 0) |
                            (style.scalable ? 16 : 0)};
    mix(ints, sizeof ints);
    mix(&style.spacing, sizeof style.spacing);
    mix(&style.exact_size, sizeof style.exact_size);
    mix(&style.exact_stroke, sizeof style.exact_stroke);
    mix(style.color, sizeof style.color);
    mix(&style.gradient, sizeof style.gradient);
    mix(&style.gradient_across, sizeof style.gradient_across);
    mix(style.gradient_colors, sizeof style.gradient_colors);
    mix(style.stroke_color, sizeof style.stroke_color);
    mix(style.background, sizeof style.background);
    return static_cast<size_t>(hash ^ (hash >> 32));
}

void forget(std::list<Kept>::iterator entry)
{
    const auto [first, last] = g_kept_by_hash.equal_range(entry->hash);
    for (auto at = first; at != last; ++at)
        if (at->second == entry) {
            g_kept_by_hash.erase(at);
            break;
        }
    g_kept_bytes -= entry->pixels.size() * sizeof(std::uint32_t);
    g_kept.erase(entry);
}

// As draw_image, from the kept images when the text was drawn in this style lately.
void render(const std::wstring& text, const Style& style, int& width, int& height, std::vector<std::uint32_t>& pixels,
            int& baseline, int& pen_left, int& advance, Exact& exact)
{
    const size_t hash = style_hash(text, style);
    {
        const std::lock_guard guard(g_kept_lock);
        const auto [first, last] = g_kept_by_hash.equal_range(hash);
        for (auto at = first; at != last; ++at) {
            const auto entry = at->second;
            if (entry->text == text && entry->style == style) {
                g_kept.splice(g_kept.begin(), g_kept, entry);
                width = entry->width, height = entry->height, baseline = entry->baseline;
                pen_left = entry->pen_left, advance = entry->advance;
                exact = entry->exact;
                pixels = entry->pixels;
                return;
            }
        }
    }
    draw_image(text, style, width, height, pixels, baseline, pen_left, advance, exact);
    const size_t bytes = pixels.size() * sizeof(std::uint32_t);
    if (bytes > kKeptMostOne)
        return;
    const std::lock_guard guard(g_kept_lock);
    g_kept.push_front(Kept{text, style, hash, width, height, baseline, pen_left, advance, exact, pixels});
    g_kept_by_hash.emplace(hash, g_kept.begin());
    g_kept_bytes += bytes;
    while (g_kept_bytes > kKeptBytes && g_kept.size() > 1)
        forget(std::prev(g_kept.end()));
}

} // namespace

void set_gdi_only(bool on)
{
    if (g_gdi_only.exchange(on) == on)
        return;
    const std::lock_guard guard(g_kept_lock); // (the kept pictures are DirectWrite's, or GDI's, no more)
    g_kept.clear();
    g_kept_by_hash.clear();
    g_kept_bytes = 0;
}

bool parse_colour(std::wstring_view text, float (&colour)[4])
{
    if (!text.empty() && text[0] == L'#')
        text.remove_prefix(1);
    if ((text.size() != 6 && text.size() != 8) || text.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring_view::npos)
        return false;
    const unsigned long value = std::wcstoul(std::wstring(text).c_str(), nullptr, 16);
    const unsigned long rgb = text.size() == 8 ? value >> 8 : value;
    colour[0] = static_cast<float>((rgb >> 16) & 0xFF);
    colour[1] = static_cast<float>((rgb >> 8) & 0xFF);
    colour[2] = static_cast<float>(rgb & 0xFF);
    if (text.size() == 8)
        colour[3] = static_cast<float>(value & 0xFF) / 255.0f;
    return true;
}

void argb_colour(std::uint32_t argb, float (&colour)[4])
{
    colour[0] = static_cast<float>((argb >> 16) & 0xFF);
    colour[1] = static_cast<float>((argb >> 8) & 0xFF);
    colour[2] = static_cast<float>(argb & 0xFF);
    colour[3] = static_cast<float>(argb >> 24) / 255.0f;
}

std::wstring hex(const float (&colour)[4])
{
    wchar_t text[16];
    swprintf_s(text, L"#%02X%02X%02X%02X", static_cast<unsigned>(colour[0]), static_cast<unsigned>(colour[1]),
               static_cast<unsigned>(colour[2]), static_cast<unsigned>(colour[3] * 255.0f + 0.5f));
    return text;
}

std::wstring installed_face(const Style& style)
{
    wchar_t used[LF_FACESIZE] = {};
    if (HDC dc = CreateCompatibleDC(nullptr)) {
        HFONT font = create_font(style);
        const HGDIOBJ old = SelectObject(dc, font);
        GetTextFaceW(dc, LF_FACESIZE, used);
        SelectObject(dc, old);
        DeleteObject(font);
        DeleteDC(dc);
    }
    return used;
}

void Sprite::set(std::wstring text, const Style& style)
{
    if (rendered_ && text == text_ && style == style_)
        return;
    text_ = std::move(text);
    style_ = style;
    Exact exact;
    render(text_, style_, width_, height_, pixels_, baseline_, pen_left_, advance_, exact);
    middle_ = exact.middle;
    exact_advance_ = exact.advance;
    rendered_ = true;
    stale_ = true;
}

void Sprite::set_image(std::vector<std::uint32_t> pixels, int width, int height)
{
    text_.clear();
    pixels_ = std::move(pixels);
    width_ = width;
    height_ = height;
    baseline_ = pen_left_ = advance_ = 0;
    middle_ = exact_advance_ = 0.0f;
    rendered_ = true;
    stale_ = true;
}

namespace {

// On a thread of its own, so the game thread's COM stays as the game set it.
bool decode(const std::wstring& path, int& width, int& height, std::vector<std::uint32_t>& pixels)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        // premultiplied while it's scaled, so the see-through edges don't darken
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        if (width <= 0 && height <= 0)
            width = static_cast<int>(w), height = static_cast<int>(h);
        else if (width <= 0)
            width = static_cast<int>((static_cast<double>(w) * height / h) + 0.5);
        else if (height <= 0)
            height = static_cast<int>((static_cast<double>(h) * width / w) + 0.5);
        width = width < 1 ? 1 : width > 4096 ? 4096 : width;
        height = height < 1 ? 1 : height > 4096 ? 4096 : height;
        IWICBitmapSource* source = converter;
        if ((static_cast<UINT>(width) != w || static_cast<UINT>(height) != h) && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            (SUCCEEDED(scaler->Initialize(converter, width, height, WICBitmapInterpolationModeHighQualityCubic)) ||
             SUCCEEDED(scaler->Initialize(converter, width, height, WICBitmapInterpolationModeFant))))
            source = scaler;
        UINT sw = 0, sh = 0;
        source->GetSize(&sw, &sh);
        width = static_cast<int>(sw), height = static_cast<int>(sh);
        pixels.assign(static_cast<std::size_t>(width) * height, 0);
        ok = SUCCEEDED(source->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
        // back to straight alpha, as the sprites are
        for (std::uint32_t& p : pixels) {
            const std::uint32_t a = p >> 24;
            if (a == 0) {
                p = 0;
            } else if (a < 255) {
                const auto un = [a](std::uint32_t c) { const std::uint32_t v = (c * 255 + a / 2) / a; return v > 255 ? 255u : v; };
                p = a << 24 | un((p >> 16) & 0xFF) << 16 | un((p >> 8) & 0xFF) << 8 | un(p & 0xFF);
            }
        }
    }
    for (IUnknown* object : {static_cast<IUnknown*>(scaler), static_cast<IUnknown*>(converter), static_cast<IUnknown*>(frame),
                             static_cast<IUnknown*>(decoder), static_cast<IUnknown*>(factory)})
        if (object != nullptr)
            object->Release();
    if (SUCCEEDED(com))
        CoUninitialize();
    return ok;
}

} // namespace

bool load_image(const std::wstring& path, int& width, int& height, std::vector<std::uint32_t>& pixels)
{
    bool ok = false;
    std::thread([&] { ok = decode(path, width, height, pixels); }).join();
    return ok;
}

void Sprite::release()
{
    if (texture_ != nullptr)
        call<ULONG>(texture_, slot::Release);
    texture_ = nullptr;
    texture_width_ = texture_height_ = 0;
    stale_ = true;
}

// Copies the image into the texture, making a bigger texture first if it doesn't fit.
namespace {

// A patchy noise for dissolving a picture: three octaves of value noise (cells of 9, 4 and 1.5 pixels), then spread evenly
// over 0-255 (by rank), so the picture goes at an even rate as the amount grows. The same for the same size.
void make_noise(int width, int height, std::vector<std::uint8_t>& noise)
{
    const auto hash = [](int x, int y) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
    };
    const auto value = [&](float px, float py, float cell) {
        const float fx = px / cell, fy = py / cell;
        const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
        const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        const float top = hash(ix, iy) + (hash(ix + 1, iy) - hash(ix, iy)) * sx;
        const float bottom = hash(ix, iy + 1) + (hash(ix + 1, iy + 1) - hash(ix, iy + 1)) * sx;
        return top + (bottom - top) * sy;
    };
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> raw(count);
    std::array<std::size_t, 1024> bins{};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            const float n = value(fx, fy, 9.0f) * 0.55f + value(fx + 57.0f, fy + 31.0f, 4.0f) * 0.3f +
                            value(fx + 13.0f, fy + 91.0f, 1.5f) * 0.15f;
            raw[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = n;
            ++bins[static_cast<std::size_t>(std::clamp(n, 0.0f, 0.999f) * 1024.0f)];
        }
    }
    std::array<float, 1024> rank{}; // each bin's share of the pixels below it, in the middle of its own
    std::size_t below = 0;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        rank[i] = (static_cast<float>(below) + static_cast<float>(bins[i]) * 0.5f) / static_cast<float>(std::max<std::size_t>(count, 1));
        below += bins[i];
    }
    noise.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        noise[i] = static_cast<std::uint8_t>(
            std::lround(rank[static_cast<std::size_t>(std::clamp(raw[i], 0.0f, 0.999f) * 1024.0f)] * 255.0f));
}

// One pixel of a dissolving picture (straight-alpha ARGB): gone where the noise `n` (0-255) is under the front, burning in
// the 0.1 of noise above it (`edge` toward white-hot, the picture's colour left at the band's far side), going out over 0.03.
std::uint32_t dissolve_pixel(std::uint32_t pixel, std::uint8_t n, float amount, const float (&edge)[4])
{
    constexpr float kBand = 0.1f, kFade = 0.03f;
    const float front = -kBand + amount * (1.0f + kBand + kFade); // below -kBand at 0, past 1 + kFade at 1
    const float diff = static_cast<float>(n) / 255.0f - front;
    const float keep = std::clamp(diff / kFade + 1.0f, 0.0f, 1.0f);
    const float heat = edge[3] > 0.0f ? std::clamp(1.0f - diff / kBand, 0.0f, 1.0f) : 0.0f;
    float rgb[3] = {static_cast<float>((pixel >> 16) & 0xFF), static_cast<float>((pixel >> 8) & 0xFF),
                    static_cast<float>(pixel & 0xFF)};
    if (heat > 0.0f) {
        const float hot[3] = {255.0f, 240.0f, 190.0f};
        const float white = std::clamp((heat - 0.6f) / 0.4f, 0.0f, 1.0f); // the very front goes white-hot
        const float mix = std::clamp(heat * 1.6f, 0.0f, 1.0f) * edge[3];
        for (int i = 0; i < 3; ++i)
            rgb[i] += ((edge[i] + (hot[i] - edge[i]) * white) - rgb[i]) * mix;
    }
    const auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f))); };
    return channel(static_cast<float>(pixel >> 24) * keep) << 24 | channel(rgb[0]) << 16 | channel(rgb[1]) << 8 | channel(rgb[2]);
}

} // namespace

void Sprite::set_dissolve(float amount, const float (&edge)[4])
{
    amount = std::clamp(amount, 0.0f, 1.0f);
    if (amount == dissolve_ && std::memcmp(edge_, edge, sizeof edge_) == 0)
        return;
    dissolve_ = amount;
    std::memcpy(edge_, edge, sizeof edge_);
    stale_ = true;
}

bool Sprite::upload(void* device)
{
    if (!stale_)
        return texture_ != nullptr;
    if (pixels_.empty())
        return false;
    // Power-of-two sizes: the oldest D3D8 drivers need them.
    UINT width = 1, height = 1;
    while (width < static_cast<UINT>(width_))
        width <<= 1;
    while (height < static_cast<UINT>(height_))
        height <<= 1;
    if (texture_ == nullptr || width > texture_width_ || height > texture_height_) {
        release();
        if (FAILED(call(device, slot::CreateTexture, width, height, 1u, DWORD{0}, d3d8::kFormatA8R8G8B8,
                        d3d8::kPoolManaged, &texture_))) {
            texture_ = nullptr;
            return false;
        }
        texture_width_ = width;
        texture_height_ = height;
    }
    d3d8::LockedRect locked{};
    if (FAILED(call(texture_, slot::TextureLockRect, 0u, &locked, static_cast<const RECT*>(nullptr), DWORD{0})))
        return false;
    const bool dissolving = dissolve_ > 0.0f;
    if (dissolving && (noise_width_ != width_ || noise_height_ != height_)) {
        make_noise(width_, height_, noise_);
        noise_width_ = width_;
        noise_height_ = height_;
    }
    for (UINT y = 0; y < texture_height_; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<BYTE*>(locked.bits) + y * locked.pitch);
        for (UINT x = 0; x < texture_width_; ++x) {
            if (x >= static_cast<UINT>(width_) || y >= static_cast<UINT>(height_))
                row[x] = 0;
            else if (dissolving)
                row[x] = dissolve_pixel(pixels_[y * width_ + x], noise_[y * width_ + x], dissolve_, edge_);
            else
                row[x] = pixels_[y * width_ + x];
        }
    }
    call(texture_, slot::TextureUnlockRect, 0u);
    stale_ = false;
    return true;
}

#include "text_replay.h" // record, replay: what's drawn over the game's windows drawn again over those raised on top

void Sprite::draw(void* device, int x, int y, float opacity, float scale, const Wave& wave, const Crop* crop) const
{
    if (texture_ == nullptr || opacity <= 0.0f || !(scale > 0.0f))
        return;
    if (g_recording && !(wave.amplitude != 0.0f && wave.length > 0.0f))
        g_recorded.push_back({this, texture_, x, y, opacity, scale, crop != nullptr, crop != nullptr ? *crop : Crop{}});
    if (scale != 1.0f) {
        // bigger or smaller than its pixels: filtered, then point again as begin() set it
        const auto filter = [&](DWORD mode) {
            call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::MagFilter, mode);
            call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::MinFilter, mode);
        };
        filter(d3d8::kFilterLinear);
        draw_quad(device, x, y, opacity, scale, wave, crop);
        filter(d3d8::kFilterPoint);
        return;
    }
    draw_quad(device, x, y, opacity, 1.0f, wave, crop);
}

void Sprite::draw_quad(void* device, int x, int y, float opacity, float scale, const Wave& wave, const Crop* crop) const
{
    // -0.5: D3D8 pixel centres sit on whole coordinates, so this maps texels 1:1.
    float left = static_cast<float>(x) - 0.5f;
    float top = static_cast<float>(y) - 0.5f;
    float right = left + static_cast<float>(width_) * scale;
    float bottom = top + static_cast<float>(height_) * scale;
    float u0 = 0.0f, v0 = 0.0f;
    float u = static_cast<float>(width_) / static_cast<float>(texture_width_);
    float v = static_cast<float>(height_) / static_cast<float>(texture_height_);
    constexpr int kWaveColumns = 512;
    const bool waving = wave.amplitude != 0.0f && wave.length > 0.0f;
    // A crop: the quad cut to the pixels it keeps (their edges half a pixel out from their centres), its texture's coordinates
    // cut the same way
    if (crop != nullptr && !waving) {
        const float cut_left = std::max(left, static_cast<float>(crop->left) - 0.5f);
        const float cut_top = std::max(top, static_cast<float>(crop->top) - 0.5f);
        const float cut_right = std::min(right, static_cast<float>(crop->right) - 0.5f);
        const float cut_bottom = std::min(bottom, static_cast<float>(crop->bottom) - 0.5f);
        if (!(cut_right > cut_left) || !(cut_bottom > cut_top))
            return;
        const float across = u / (right - left), down = v / (bottom - top);
        u0 = (cut_left - left) * across, u = (cut_right - left) * across;
        v0 = (cut_top - top) * down, v = (cut_bottom - top) * down;
        left = cut_left, top = cut_top, right = cut_right, bottom = cut_bottom;
    }
    call(device, slot::SetTexture, DWORD{0}, texture_);
    // A wave: the quad cut into columns (2 pixels wide, 512 at most), each moved up or down, drawn as one strip; flat, it's the
    // one column and the strip is the quad.
    const int columns = waving ? std::clamp(static_cast<int>(std::ceil((right - left) / 2.0f)), 1, kWaveColumns) : 1;
    const auto swing = [&](float fraction) { // how far down the column at `fraction` of the width is moved, in pixels
        return waving ? wave.amplitude * std::sin(6.2831853f * (right - left) * fraction / wave.length + wave.phase) : 0.0f;
    };
    if (opacity >= 1.0f) {
        struct Vertex {
            float x, y, z, rhw, u, v;
        };
        Vertex strip[2 * (kWaveColumns + 1)];
        for (int c = 0; c <= columns; ++c) {
            const float fraction = static_cast<float>(c) / static_cast<float>(columns);
            const float px = left + (right - left) * fraction;
            const float dy = swing(fraction);
            strip[2 * c] = {px, top + dy, 0, 1, u0 + (u - u0) * fraction, v0};
            strip[2 * c + 1] = {px, bottom + dy, 0, 1, u0 + (u - u0) * fraction, v};
        }
        call(device, slot::DrawPrimitiveUP, d3d8::kTriangleStrip, static_cast<UINT>(2 * columns), static_cast<const void*>(strip),
             static_cast<UINT>(sizeof(Vertex)));
        return;
    }
    // Faded: the texture's alpha times the vertices' (white, `opacity` opaque), then back to what
    // begin() set.
    struct Vertex {
        float x, y, z, rhw;
        DWORD colour;
        float u, v;
    };
    const DWORD colour = static_cast<DWORD>(std::lround(opacity * 255.0f)) << 24 | 0xFFFFFF;
    Vertex strip[2 * (kWaveColumns + 1)];
    for (int c = 0; c <= columns; ++c) {
        const float fraction = static_cast<float>(c) / static_cast<float>(columns);
        const float px = left + (right - left) * fraction;
        const float dy = swing(fraction);
        strip[2 * c] = {px, top + dy, 0, 1, colour, u0 + (u - u0) * fraction, v0};
        strip[2 * c + 1] = {px, bottom + dy, 0, 1, colour, u0 + (u - u0) * fraction, v};
    }
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, d3d8::kTopModulate);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaArg2, d3d8::kTaDiffuse);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwDiffuseTex1);
    call(device, slot::DrawPrimitiveUP, d3d8::kTriangleStrip, static_cast<UINT>(2 * columns), static_cast<const void*>(strip),
         static_cast<UINT>(sizeof(Vertex)));
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, d3d8::kTopSelectArg1);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwTex1);
}

void fill(void* device, int x, int y, int width, int height, std::uint32_t argb)
{
    if (width <= 0 || height <= 0 || (argb >> 24) == 0)
        return;
    struct Vertex {
        float x, y, z, rhw;
        DWORD colour;
    };
    const float left = static_cast<float>(x) - 0.5f;
    const float top = static_cast<float>(y) - 0.5f;
    const float right = left + static_cast<float>(width);
    const float bottom = top + static_cast<float>(height);
    const Vertex quad[4] = {{left, top, 0, 1, argb}, {right, top, 0, 1, argb}, {left, bottom, 0, 1, argb}, {right, bottom, 0, 1, argb}};
    // The vertices' colour instead of a texture, then back to what begin() set for sprites.
    call(device, slot::SetTexture, DWORD{0}, static_cast<void*>(nullptr));
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::ColorArg1, d3d8::kTaDiffuse);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaArg1, d3d8::kTaDiffuse);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwDiffuse);
    call(device, slot::DrawPrimitiveUP, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad), static_cast<UINT>(sizeof(Vertex)));
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::ColorArg1, d3d8::kTaTexture);
    call(device, slot::SetTextureStageState, DWORD{0}, d3d8::tss::AlphaArg1, d3d8::kTaTexture);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwTex1);
}

void draw_texture(void* device, void* texture, float x, float y, float width, float height, float u0, float v0, float u1,
                  float v1, std::uint32_t argb)
{
    if (texture == nullptr || width <= 0.0f || height <= 0.0f || (argb >> 24) == 0)
        return;
    struct Vertex {
        float x, y, z, rhw;
        DWORD colour;
        float u, v;
    };
    const float left = x - 0.5f;
    const float top = y - 0.5f;
    const float right = left + width;
    const float bottom = top + height;
    const Vertex quad[4] = {{left, top, 0, 1, argb, u0, v0},
                            {right, top, 0, 1, argb, u1, v0},
                            {left, bottom, 0, 1, argb, u0, v1},
                            {right, bottom, 0, 1, argb, u1, v1}};
    // The texture times the vertices' colour, filtered and repeating, then back to what begin() set for sprites.
    const auto stage_state = [&](DWORD type, DWORD value) { call(device, slot::SetTextureStageState, DWORD{0}, type, value); };
    call(device, slot::SetTexture, DWORD{0}, texture);
    stage_state(d3d8::tss::ColorOp, d3d8::kTopModulate);
    stage_state(d3d8::tss::ColorArg2, d3d8::kTaDiffuse);
    stage_state(d3d8::tss::AlphaOp, d3d8::kTopModulate);
    stage_state(d3d8::tss::AlphaArg2, d3d8::kTaDiffuse);
    stage_state(d3d8::tss::AddressU, d3d8::kAddressWrap);
    stage_state(d3d8::tss::AddressV, d3d8::kAddressWrap);
    stage_state(d3d8::tss::MagFilter, d3d8::kFilterLinear);
    stage_state(d3d8::tss::MinFilter, d3d8::kFilterLinear);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwDiffuseTex1);
    call(device, slot::DrawPrimitiveUP, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad), static_cast<UINT>(sizeof(Vertex)));
    stage_state(d3d8::tss::ColorOp, d3d8::kTopSelectArg1);
    stage_state(d3d8::tss::AlphaOp, d3d8::kTopSelectArg1);
    stage_state(d3d8::tss::AddressU, d3d8::kAddressClamp);
    stage_state(d3d8::tss::AddressV, d3d8::kAddressClamp);
    stage_state(d3d8::tss::MagFilter, d3d8::kFilterPoint);
    stage_state(d3d8::tss::MinFilter, d3d8::kFilterPoint);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwTex1);
}

bool begin(void* device, UINT width, UINT height, DWORD& saved_state)
{
    if (!prepare(device, width, height, saved_state))
        return false;
    if (FAILED(call(device, slot::BeginScene))) {
        restore(device, saved_state);
        return false;
    }
    return true;
}

bool prepare(void* device, UINT width, UINT height, DWORD& saved_state)
{
    // Everything drawing changes is put back afterwards, so the game's next frame is unaffected.
    saved_state = 0;
    if (FAILED(call(device, slot::CreateStateBlock, d3d8::kStateBlockAll, &saved_state)))
        return false;
    const d3d8::Viewport viewport{0, 0, width, height, 0.0f, 1.0f};
    call(device, slot::SetViewport, &viewport);
    const auto render_state = [&](DWORD type, DWORD value) { call(device, slot::SetRenderState, type, value); };
    render_state(d3d8::rs::ZEnable, FALSE);
    render_state(d3d8::rs::ZWriteEnable, FALSE);
    render_state(d3d8::rs::FillMode, d3d8::kFillSolid);
    render_state(d3d8::rs::CullMode, d3d8::kCullNone);
    render_state(d3d8::rs::AlphaTestEnable, FALSE);
    render_state(d3d8::rs::AlphaBlendEnable, TRUE);
    render_state(d3d8::rs::SrcBlend, d3d8::kBlendSrcAlpha);
    render_state(d3d8::rs::DestBlend, d3d8::kBlendInvSrcAlpha);
    render_state(d3d8::rs::BlendOp, d3d8::kBlendOpAdd);
    render_state(d3d8::rs::FogEnable, FALSE);
    render_state(d3d8::rs::SpecularEnable, FALSE);
    render_state(d3d8::rs::StencilEnable, FALSE);
    render_state(d3d8::rs::Lighting, FALSE);
    render_state(d3d8::rs::Clipping, TRUE);
    render_state(d3d8::rs::VertexBlend, 0);
    render_state(d3d8::rs::Wrap0, 0);
    render_state(d3d8::rs::ColorWriteEnable, 0xF);
    const auto stage_state = [&](DWORD stage, DWORD type, DWORD value) {
        call(device, slot::SetTextureStageState, stage, type, value);
    };
    stage_state(0, d3d8::tss::ColorOp, d3d8::kTopSelectArg1);
    stage_state(0, d3d8::tss::ColorArg1, d3d8::kTaTexture);
    stage_state(0, d3d8::tss::AlphaOp, d3d8::kTopSelectArg1);
    stage_state(0, d3d8::tss::AlphaArg1, d3d8::kTaTexture);
    stage_state(0, d3d8::tss::TexCoordIndex, 0);
    stage_state(0, d3d8::tss::TextureTransformFlags, 0);
    stage_state(0, d3d8::tss::AddressU, d3d8::kAddressClamp);
    stage_state(0, d3d8::tss::AddressV, d3d8::kAddressClamp);
    stage_state(0, d3d8::tss::MagFilter, d3d8::kFilterPoint);
    stage_state(0, d3d8::tss::MinFilter, d3d8::kFilterPoint);
    stage_state(0, d3d8::tss::MipFilter, d3d8::kFilterNone);
    stage_state(1, d3d8::tss::ColorOp, d3d8::kTopDisable);
    stage_state(1, d3d8::tss::AlphaOp, d3d8::kTopDisable);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwTex1);
    call(device, slot::SetPixelShader, DWORD{0});
    return true;
}

void end(void* device, DWORD saved_state)
{
    call(device, slot::EndScene);
    restore(device, saved_state);
}

void restore(void* device, DWORD saved_state)
{
    call(device, slot::ApplyStateBlock, saved_state);
    call(device, slot::DeleteStateBlock, saved_state);
}

} // namespace vanacore::text
