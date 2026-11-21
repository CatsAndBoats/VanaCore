// Addon packages outside the game: .vcaddon opening with this VanaCore (the association the game makes at each start), and
// what a double-click on one does - rundll32 calls VanaCoreOpenPackage in this DLL, so no console window comes up. With a
// game running from this VanaCore the package goes to its inbox and the game comes to the front, where it asks; with none,
// VanaCore's own window asks here and installs it, to load at the game's next start.
//
// That window is the in-game prompt's card (addons\vanacore\vana\install.lua) drawn outside the game: the same palette,
// VanaCore's logo with its status chip, the name and version, the description, From / Into / Holds, the warning, Load at
// every start and the buttons. Direct2D and DirectWrite draw it into a layered window, so its corners are round and its
// shadow soft on every Windows; it turns into the installed (or wasn't installed) card in place. d2d1.dll is loaded only
// here, never by the game.

#include "addonpkg.hpp"
#include "addonpkg_link.hpp"

#include <windows.h>
#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "image.hpp"
#include "resource.hpp"

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")

namespace vanacore::addonpkg {

namespace {

constexpr wchar_t kExtension[] = L"Software\\Classes\\.vcaddon";
constexpr wchar_t kProgId[] = L"VanaCore.AddonPackage";
constexpr wchar_t kProgKey[] = L"Software\\Classes\\VanaCore.AddonPackage";
constexpr wchar_t kScheme[] = L"Software\\Classes\\vanacore"; // vanacore:// links from a browser
constexpr UINT kProgress = WM_APP + 1, kFetched = WM_APP + 2;

HMODULE self()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &module);
    return module;
}

std::wstring module_path()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(self(), path, static_cast<DWORD>(std::size(path)));
    return path;
}

std::wstring dir_of(const std::wstring& path)
{
    return path.substr(0, path.find_last_of(L'\\'));
}

std::wstring read_value(const wchar_t* key)
{
    wchar_t value[1024] = {};
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return {};
    return value;
}

void set_value(const wchar_t* key, const wchar_t* name, const std::wstring& value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

// 32-bit rundll32 for this 32-bit DLL: SysWOW64's on a 64-bit Windows
std::wstring rundll32()
{
    wchar_t dir[MAX_PATH] = {};
    if (GetSystemWow64DirectoryW(dir, MAX_PATH) == 0)
        GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\rundll32.exe";
}

std::wstring size_text(std::uint64_t bytes)
{
    wchar_t text[32];
    if (bytes >= 1024 * 1024)
        swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        swprintf_s(text, L"%llu KB", (bytes + 1023) / 1024);
    return text;
}

// Windows' common controls 6 (the Open dialog) through this DLL's own manifest, and per-monitor DPI for this thread (the
// Open dialog crisp, VanaCore's window scaled to each screen); undone by the destructor
struct DialogScope {
    HANDLE context = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie = 0;
    DialogScope()
    {
        using SetDpi = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto set = reinterpret_cast<SetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")))
            set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ACTCTXW ctx{};
        ctx.cbSize = sizeof ctx;
        ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        ctx.hModule = self();
        ctx.lpResourceName = MAKEINTRESOURCEW(IDR_ADDONPKG_MANIFEST);
        context = CreateActCtxW(&ctx);
        if (context != INVALID_HANDLE_VALUE)
            ActivateActCtx(context, &cookie);
    }
    ~DialogScope()
    {
        if (context != INVALID_HANDLE_VALUE) {
            DeactivateActCtx(0, cookie);
            ReleaseActCtx(context);
        }
    }
    DialogScope(const DialogScope&) = delete;
    DialogScope& operator=(const DialogScope&) = delete;
};

// VanaCore's own icon (VanaCore.exe's, which has every size up to 256), `pixels` across: the window's taskbar and
// Alt+Tab icons
HICON vanacore_icon(const std::wstring& install_dir, int pixels)
{
    HMODULE exe = LoadLibraryExW((install_dir + L"\\VanaCore.exe").c_str(), nullptr,
                                 LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (exe == nullptr)
        return nullptr;
    using ScaleDown = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);
    HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    auto load = comctl != nullptr ? reinterpret_cast<ScaleDown>(GetProcAddress(comctl, "LoadIconWithScaleDown")) : nullptr;
    HICON icon = nullptr;
    if (load == nullptr || FAILED(load(exe, MAKEINTRESOURCEW(1), pixels, pixels, &icon)))
        icon = static_cast<HICON>(LoadImageW(exe, MAKEINTRESOURCEW(1), IMAGE_ICON, pixels, pixels, 0));
    FreeLibrary(exe);
    return icon;
}

std::wstring display_name(const Package& p)
{
    return wide(p.title.empty() ? p.name : p.title);
}

std::wstring file_name(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// "Version 1.2  ·  by Someone", or for an update "1.1  →  1.2  ·  by Someone"
std::wstring sub_line(const Package& p, bool from_to)
{
    const std::wstring version = wide(p.version);
    std::wstring first;
    if (from_to)
        first = (p.installed_version.empty() ? L"?" : wide(p.installed_version)) + L"  \x2192  " + (version.empty() ? L"?" : version);
    else if (!version.empty())
        first = L"Version " + version;
    const std::wstring by = p.author.empty() ? L"" : L"by " + wide(p.author);
    if (first.empty())
        return by;
    return by.empty() ? first : first + L"  \x00B7  " + by;
}

//----------------------------------------------------------------------------------------------------------------------
// VanaCore's window
//----------------------------------------------------------------------------------------------------------------------

// The /vana palette (draw.lua's M.P)
namespace pal {
constexpr std::uint32_t bg = 0x161822, border = 0x2A2E3E, line = 0x242838, well = 0x0E1017, well_edge = 0x272B3C,
                        hover = 0x1F2334, text = 0xE8EAF1, white = 0xFFFFFF, text2 = 0xBDC1D0, dim = 0x7B8095,
                        faint = 0x4E536A, accent = 0x7AA2F7, accent2 = 0xA9C1FF, accent_lo = 0x3E5CB3, teal = 0x5FD3B3,
                        amber = 0xE8B45C, rose = 0xEE6F78, knob = 0xF2F3F8, off = 0x2B2F42, ink = 0x0B1020;
}

D2D1_COLOR_F rgb(std::uint32_t c, float a = 1.0f)
{
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a};
}

D2D1_COLOR_F mix(std::uint32_t a, std::uint32_t b, float t, float alpha = 1.0f)
{
    const D2D1_COLOR_F x = rgb(a), y = rgb(b);
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, alpha};
}

// The card's measures, in DIPs (install.lua's), and the room round it for its shadow
constexpr float kW = 540, kPad = 26, kHead = 76, kLogo = 56, kRadius = 10, kFactH = 22, kLine = 1.42f, kShadow = 16;
constexpr float kBody = 15.5f, kSmall = 14.0f, kTitle = 19.0f, kLabel = 11.5f, kButton = 14.5f;
constexpr int kMaxDescLines = 7;
constexpr wchar_t kWarning[] = L"An addon is a program: it runs inside the game and can do anything you can on this PC. "
                               L"Install it only if you trust where it came from.";

enum Hit { kNone, kClose, kPrimary, kSecondary, kSwitch, kLink };
enum class Badge { Down, Up, Check, Bang };
enum class Style { Accent, Secondary, Ghost };

struct Fact {
    std::wstring label, value;
    bool cut_start = false; // a path: cut from its start, so its end shows
    bool link = false;
};

struct ButtonSpec {
    Hit hit{};
    std::wstring label;
    Style style{};
    bool arrow = false; // the download arrow before it
};

template <class T> void release(T*& p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

class Card {
public:
    explicit Card(std::wstring install_dir) : dir_(std::move(install_dir)) {}
    ~Card();
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;

    // The question for a package that can be installed here
    void ask(const Package& p);
    // A package that can't be installed (`why`), or one whose install failed (`tried`)
    void failed(const Package& p, const std::wstring& why, bool tried);
    // A file that isn't a package at all
    void unreadable(const std::wstring& path, const std::wstring& why);
    // Something to say about the package and nothing to choose: OK
    void note(const Package& p, Badge badge, std::uint32_t colour, const std::wstring& title, const std::wstring& sub);
    // A link that couldn't be followed: `title`, where it led (`from`) and why not
    void missed(const std::wstring& title, const std::wstring& from, const std::wstring& why);
    // A vanacore:// link's package: downloaded from `url` into `path` while the card says so (Cancel stops it), then
    // `then(why)` (why empty: it's there) sets the card for what came, or says false when there's nothing more to show
    void fetch(const std::wstring& url, const std::wstring& path, std::function<bool(const std::wstring&)> then);

    // Shows the window and waits until it's shut (without Direct2D, a plain message box asks instead)
    void show();

private:
    bool start_graphics();
    bool make_target();
    void layout();
    void place(bool first);
    void render();
    Hit hit_at(float x, float y) const;
    void press(Hit hit);
    void run_install();
    void start_fetch();
    bool fetched();
    void fallback();
    void common_facts(const Package& p, bool keeps);

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

    // drawing, in DIPs from the card's top left
    IDWriteTextLayout* make_layout(const std::wstring& text, IDWriteTextFormat* format, float width, float height = 4096) const;
    float text_width(const std::wstring& text, IDWriteTextFormat* format) const;
    std::wstring fit(const std::wstring& text, IDWriteTextFormat* format, float room, bool from_start) const;
    void text_mid(const std::wstring& text, IDWriteTextFormat* format, float x, float cy, D2D1_COLOR_F colour);
    void fill(float x, float y, float w, float h, float r, D2D1_COLOR_F colour);
    void outline(float x, float y, float w, float h, float r, D2D1_COLOR_F colour);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F colour, float width);
    void disc(float cx, float cy, float r, D2D1_COLOR_F colour);
    void draw_head(float x, float y, float right);
    void draw_chip(float cx, float cy);
    void draw_facts(float x, float y, float right);
    float draw_box(float x, float y, float right);
    void draw_buttons(float x, float right, float cy);
    void draw_arrow(float cx, float cy, float s, D2D1_COLOR_F colour);
    void draw_warning(float cx, float cy, float s, D2D1_COLOR_F colour);

    std::wstring dir_;
    const Package* package_ = nullptr;

    // what it says
    Badge badge_ = Badge::Down;
    std::uint32_t badge_colour_ = pal::accent;
    std::wstring title_, sub_;
    std::uint32_t sub_colour_ = pal::dim;
    std::wstring desc_;
    std::vector<Fact> facts_;
    std::wstring box_;
    std::uint32_t box_colour_ = pal::amber;
    std::vector<ButtonSpec> buttons_;
    bool question_ = false; // Install / Not now, with Load at every start
    bool boot_ = true;
    bool working_ = false;
    std::wstring homepage_;

    // a link's download (fetch): the thread, what it gets and where, why it failed (read once it's joined)
    std::thread fetcher_;
    std::atomic<bool> cancel_{false};
    std::wstring fetch_url_, fetch_path_, fetch_error_;
    std::function<bool(const std::wstring&)> then_;

    // laid out, and what's under the mouse
    float height_ = 0;
    int desc_lines_ = 0, box_lines_ = 0;
    std::vector<std::pair<Hit, D2D1_RECT_F>> hits_;
    Hit hover_ = kNone, pressed_ = kNone;
    bool tracking_ = false;

    // the window and its drawing
    HWND window_ = nullptr;
    float scale_ = 1.0f;
    int pixel_w_ = 0, pixel_h_ = 0;
    bool done_ = false;
    HICON icon_big_ = nullptr, icon_small_ = nullptr;
    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1DCRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1Bitmap* logo_ = nullptr;
    image::Pixels logo_pixels_;
    IDWriteTextFormat* title_font_ = nullptr;
    IDWriteTextFormat* body_font_ = nullptr;
    IDWriteTextFormat* small_font_ = nullptr;
    IDWriteTextFormat* label_font_ = nullptr;
    IDWriteTextFormat* button_font_ = nullptr;
    IDWriteInlineObject* ellipsis_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
};

Card::~Card()
{
    cancel_ = true; // a download still going (the card closed): stopped at its next read
    if (fetcher_.joinable())
        fetcher_.join();
    release(ellipsis_);
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
    if (icon_big_ != nullptr)
        DestroyIcon(icon_big_);
    if (icon_small_ != nullptr)
        DestroyIcon(icon_small_);
}

void Card::common_facts(const Package& p, bool keeps)
{
    facts_.clear();
    facts_.push_back({L"From", wide(p.from), true});
    facts_.push_back({L"Into", dir_ + L"\\addons\\vanacore\\" + wide(p.name), true});
    facts_.push_back({L"Holds", std::to_wstring(p.files) + (p.files == 1 ? L" file, " : L" files, ") + size_text(p.bytes)});
    if (keeps)
        facts_.push_back({L"Keeps", (p.installed_version.empty() ? L"?" : wide(p.installed_version)) +
                                        L" as a backup, in backups\\addons"});
    homepage_ = wide(p.homepage);
    if (!homepage_.empty()) {
        Fact site{L"Site", homepage_};
        site.link = true;
        facts_.push_back(site);
    }
}

void Card::ask(const Package& p)
{
    package_ = &p;
    const bool update = !p.installed_version.empty();
    badge_ = update ? Badge::Up : Badge::Down;
    badge_colour_ = pal::accent;
    title_ = (update ? L"Update " : L"Install ") + display_name(p) + L"?";
    sub_ = sub_line(p, update);
    sub_colour_ = pal::dim;
    desc_ = wide(p.description);
    common_facts(p, update);
    box_ = kWarning;
    box_colour_ = pal::amber;
    question_ = true;
    buttons_ = {{kPrimary, update ? L"Update" : L"Install", Style::Accent, true}, {kSecondary, L"Not now", Style::Ghost}};
}

void Card::failed(const Package& p, const std::wstring& why, bool tried)
{
    package_ = &p;
    badge_ = Badge::Bang;
    badge_colour_ = pal::rose;
    title_ = display_name(p) + (tried ? L" wasn't installed" : L" can't be installed");
    sub_ = sub_line(p, false);
    sub_colour_ = pal::dim;
    desc_ = tried ? L"" : wide(p.description);
    common_facts(p, false);
    box_ = why.empty() ? L"It couldn't be installed" : why;
    if (box_.back() != L'.' && box_.back() != L'!' && box_.back() != L'?')
        box_ += L'.'; // the installer's reasons are sentences without a stop
    box_colour_ = pal::rose;
    question_ = false;
    buttons_ = {{kPrimary, L"Close", Style::Secondary}};
}

void Card::unreadable(const std::wstring& path, const std::wstring& why)
{
    package_ = nullptr;
    badge_ = Badge::Bang;
    badge_colour_ = pal::rose;
    title_ = L"This isn't an addon package";
    sub_ = file_name(path);
    sub_colour_ = pal::dim;
    desc_.clear();
    facts_ = {{L"From", path, true}};
    homepage_.clear();
    box_ = why.empty() ? L"It can't be read." : why + L".";
    box_colour_ = pal::rose;
    question_ = false;
    buttons_ = {{kPrimary, L"Close", Style::Secondary}};
}

void Card::note(const Package& p, Badge badge, std::uint32_t colour, const std::wstring& title, const std::wstring& sub)
{
    package_ = &p;
    badge_ = badge;
    badge_colour_ = colour;
    title_ = title;
    sub_ = sub;
    sub_colour_ = colour;
    desc_.clear();
    common_facts(p, false);
    box_.clear();
    question_ = false;
    buttons_ = {{kPrimary, L"OK", Style::Accent}};
}

void Card::missed(const std::wstring& title, const std::wstring& from, const std::wstring& why)
{
    unreadable(from, why);
    title_ = title;
    sub_ = L"";
}

void Card::fetch(const std::wstring& url, const std::wstring& path, std::function<bool(const std::wstring&)> then)
{
    package_ = nullptr;
    badge_ = Badge::Down;
    badge_colour_ = pal::accent;
    title_ = L"Getting an addon package";
    sub_ = L"Downloading\x2026";
    sub_colour_ = pal::dim;
    desc_.clear();
    facts_ = {{L"From", url, true}};
    homepage_.clear();
    box_.clear();
    question_ = false;
    buttons_ = {{kSecondary, L"Cancel", Style::Ghost}};
    fetch_url_ = url;
    fetch_path_ = path;
    then_ = std::move(then);
}

// The download on its own thread, telling the window how far it's got (at most ten times a second) and when it's done
void Card::start_fetch()
{
    HWND to = window_;
    fetcher_ = std::thread([this, to] {
        ULONGLONG last = 0;
        fetch_error_ = download(fetch_url_, fetch_path_, cancel_, [&](std::uint64_t got, std::uint64_t total) {
            const ULONGLONG now = GetTickCount64();
            if (to == nullptr || now - last < 100)
                return;
            last = now;
            PostMessageW(to, kProgress, static_cast<WPARAM>(got / 1024), static_cast<LPARAM>(total / 1024));
        });
        if (to != nullptr)
            PostMessageW(to, kFetched, 0, 0);
    });
}

// The download's done: the card turns into what came (the question, or why not), or goes (false)
bool Card::fetched()
{
    if (fetcher_.joinable())
        fetcher_.join();
    fetch_url_.clear();
    hover_ = pressed_ = kNone;
    if (!then_(fetch_error_)) {
        if (window_ != nullptr)
            DestroyWindow(window_);
        return false;
    }
    if (window_ != nullptr) {
        layout();
        place(false);
        render();
    }
    return true;
}

bool Card::make_target()
{
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d_->CreateDCRenderTarget(&props, &rt_)))
        return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // the window is see-through at its edges: no ClearType
    return SUCCEEDED(rt_->CreateSolidColorBrush(rgb(pal::text), &brush_));
}

bool Card::start_graphics()
{
    using CreateD2D = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    HMODULE d2d_dll = LoadLibraryW(L"d2d1.dll"); // stays loaded: rundll32 ends straight after
    auto create = d2d_dll != nullptr ? reinterpret_cast<CreateD2D>(GetProcAddress(d2d_dll, "D2D1CreateFactory")) : nullptr;
    if (create == nullptr ||
        FAILED(create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(&d2d_))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw_))))
        return false;
    if (!make_target())
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
    dw_->CreateEllipsisTrimmingSign(body_font_, &ellipsis_);

    // VanaCore's logo, as /vana and the in-game prompt show it (premultiplied for Direct2D)
    if (image::decode_file(dir_ + L"\\addons\\vanacore\\vana\\logo.png", logo_pixels_))
        for (std::uint32_t& px : logo_pixels_.argb) {
            const std::uint32_t a = px >> 24;
            auto pm = [a](std::uint32_t c) { return (c * a + 127) / 255; };
            px = (a << 24) | (pm((px >> 16) & 255) << 16) | (pm((px >> 8) & 255) << 8) | pm(px & 255);
        }
    return true;
}

IDWriteTextLayout* Card::make_layout(const std::wstring& text, IDWriteTextFormat* format, float width, float height) const
{
    IDWriteTextLayout* layout = nullptr;
    dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, std::max(1.0f, width), height, &layout);
    return layout;
}

float Card::text_width(const std::wstring& text, IDWriteTextFormat* format) const
{
    IDWriteTextLayout* layout = make_layout(text, format, 8192);
    if (layout == nullptr)
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

// `text` cut to `room` with an ellipsis: at its end, or at its start (a path: its file's name shows)
std::wstring Card::fit(const std::wstring& text, IDWriteTextFormat* format, float room, bool from_start) const
{
    if (text_width(text, format) <= room)
        return text;
    auto cut = [&](std::size_t keep) {
        return from_start ? L"\x2026" + text.substr(text.size() - keep) : text.substr(0, keep) + L"\x2026";
    };
    std::size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const std::size_t keep = (lo + hi + 1) / 2;
        if (text_width(cut(keep), format) <= room)
            lo = keep;
        else
            hi = keep - 1;
    }
    return cut(lo);
}

void Card::text_mid(const std::wstring& text, IDWriteTextFormat* format, float x, float cy, D2D1_COLOR_F colour)
{
    IDWriteTextLayout* layout = make_layout(text, format, 8192);
    if (layout == nullptr)
        return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    brush_->SetColor(colour);
    rt_->DrawTextLayout({x, cy - m.height / 2}, layout, brush_);
    layout->Release();
}

void Card::fill(float x, float y, float w, float h, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush_);
}

// A hairline just inside the rectangle
void Card::outline(float x, float y, float w, float h, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.5f, y + 0.5f, x + w - 0.5f, y + h - 0.5f), r, r), brush_, 1.0f);
}

void Card::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F colour, float width)
{
    brush_->SetColor(colour);
    rt_->DrawLine({x1, y1}, {x2, y2}, brush_, width);
}

void Card::disc(float cx, float cy, float r, D2D1_COLOR_F colour)
{
    brush_->SetColor(colour);
    rt_->FillEllipse(D2D1::Ellipse({cx, cy}, r, r), brush_);
}

// The card's height for its width (kW), and how many lines the description and the box take
void Card::layout()
{
    const float room = kW - kPad * 2;
    const float body_line = std::floor(kBody * kLine), small_line = std::floor(kSmall * kLine);
    auto lines_of = [&](const std::wstring& text, IDWriteTextFormat* format, float width) {
        int lines = 0;
        if (IDWriteTextLayout* l = make_layout(text, format, width)) {
            l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            DWRITE_TEXT_METRICS m{};
            l->GetMetrics(&m);
            lines = static_cast<int>(m.lineCount);
            l->Release();
        }
        return lines;
    };
    float h = kPad + kHead;
    desc_lines_ = desc_.empty() ? 0 : std::min(lines_of(desc_, body_font_, room), kMaxDescLines);
    if (desc_lines_ > 0)
        h += 14 + desc_lines_ * body_line;
    h += 16 + facts_.size() * kFactH;
    box_lines_ = box_.empty() ? 0 : lines_of(box_, small_font_, room - 46);
    if (!box_.empty())
        h += 16 + box_lines_ * small_line + 22;
    height_ = h + 22 + 32 + kPad;
}

// The window sized for the card at this DPI: centred on the mouse's screen the first time, then kept centred where it is
void Card::place(bool first)
{
    pixel_w_ = static_cast<int>(std::ceil((kW + kShadow * 2) * scale_));
    pixel_h_ = static_cast<int>(std::ceil((height_ + kShadow * 2) * scale_));
    RECT now{};
    GetWindowRect(window_, &now);
    POINT at{(now.left + now.right) / 2, (now.top + now.bottom) / 2};
    if (first)
        GetCursorPos(&at);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    int x = first ? (work.left + work.right - pixel_w_) / 2 : at.x - pixel_w_ / 2;
    int y = first ? (work.top + work.bottom - pixel_h_) / 2 : at.y - pixel_h_ / 2;
    x = std::max<int>(work.left, std::min<int>(x, work.right - pixel_w_));
    y = std::max<int>(work.top, std::min<int>(y, work.bottom - pixel_h_));
    SetWindowPos(window_, nullptr, x, y, pixel_w_, pixel_h_, SWP_NOZORDER | SWP_NOACTIVATE);

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

// The status chip on the logo's corner (install.lua's draw_chip): an arrow down (install), up (update), a tick
// (installed), an exclamation (can't)
void Card::draw_chip(float cx, float cy)
{
    const float r = 9;
    disc(cx, cy, r + 2.5f, rgb(pal::bg)); // a ring of the card's colour, cutting it out of the logo
    disc(cx, cy, r, rgb(badge_colour_));
    const D2D1_COLOR_F ink = rgb(pal::ink);
    const float th = 1.9f;
    auto seg = [&](float x1, float y1, float x2, float y2) { line(cx + x1 * r, cy + y1 * r, cx + x2 * r, cy + y2 * r, ink, th); };
    if (badge_ == Badge::Check) {
        seg(-0.45f, 0.02f, -0.12f, 0.36f);
        seg(-0.12f, 0.36f, 0.48f, -0.32f);
    } else if (badge_ == Badge::Bang) {
        seg(0, -0.5f, 0, 0.12f);
        disc(cx, cy + 0.46f * r, th * 0.75f, ink);
    } else {
        const float d = badge_ == Badge::Up ? -1.0f : 1.0f;
        seg(0, -0.48f * d, 0, 0.44f * d);
        seg(-0.4f, 0.06f * d, 0, 0.46f * d);
        seg(0.4f, 0.06f * d, 0, 0.46f * d);
    }
}

void Card::draw_head(float x, float y, float right)
{
    const float size = kLogo;
    const float ix = x - 4, iy = y + 25 - size / 2; // centred on the title and its sub line
    if (logo_ == nullptr && !logo_pixels_.argb.empty())
        rt_->CreateBitmap(D2D1::SizeU(logo_pixels_.width, logo_pixels_.height), logo_pixels_.argb.data(), logo_pixels_.width * 4,
                          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                          &logo_);
    if (logo_ != nullptr) {
        rt_->DrawBitmap(logo_, D2D1::RectF(ix, iy, ix + size, iy + size), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else { // no logo.png: VanaCore's colours where it would be
        fill(ix + 4, iy + 4, size - 8, size - 8, 10, rgb(pal::accent_lo));
        text_mid(L"V", title_font_, ix + size / 2 - text_width(L"V", title_font_) / 2, iy + size / 2, rgb(pal::white));
    }
    draw_chip(ix + size * 0.84f, iy + size * 0.84f);
    const float tx = ix + size + 14;
    text_mid(fit(title_, title_font_, right - tx - 30, false), title_font_, tx, y + 13, rgb(pal::white));
    if (!sub_.empty())
        text_mid(fit(sub_, small_font_, right - tx, false), small_font_, tx, y + 37, rgb(sub_colour_));
}

void Card::draw_facts(float x, float y, float right)
{
    const float label_w = 58;
    for (std::size_t i = 0; i < facts_.size(); ++i) {
        const Fact& f = facts_[i];
        const float cy = y + (i + 0.5f) * kFactH;
        std::wstring label = f.label;
        for (wchar_t& c : label)
            c = static_cast<wchar_t>(std::towupper(c));
        text_mid(label, label_font_, x, cy, rgb(pal::faint));
        const float vx = x + label_w, room = right - vx;
        const std::wstring value = fit(f.value, small_font_, room, f.cut_start);
        if (f.link) {
            const float w = text_width(value, small_font_);
            const bool over = hover_ == kLink;
            text_mid(value, small_font_, vx, cy, rgb(over ? pal::accent2 : pal::accent));
            if (over)
                line(vx, cy + 8.5f, vx + w, cy + 8.5f, rgb(pal::accent2), 0.8f);
            hits_.push_back({kLink, D2D1::RectF(vx, cy - 9, vx + w, cy + 9)});
        } else {
            text_mid(value, small_font_, vx, cy, rgb(pal::text2));
        }
    }
}

void Card::draw_warning(float cx, float cy, float s, D2D1_COLOR_F colour)
{
    ID2D1PathGeometry* path = nullptr;
    if (SUCCEEDED(d2d_->CreatePathGeometry(&path))) {
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure({cx, cy - s}, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine({cx + s * 1.1f, cy + s * 0.85f});
            sink->AddLine({cx - s * 1.1f, cy + s * 0.85f});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            ID2D1StrokeStyle* round = nullptr;
            d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &round);
            brush_->SetColor(colour);
            rt_->DrawGeometry(path, brush_, 1.6f, round);
            release(round);
        }
        path->Release();
    }
    line(cx, cy - s * 0.35f, cx, cy + s * 0.2f, colour, 1.6f);
    disc(cx, cy + s * 0.52f, 1.0f, colour);
}

// The warning (or the problem) in a tinted box with its icon
float Card::draw_box(float x, float y, float right)
{
    const float small_line = std::floor(kSmall * kLine);
    const float h = box_lines_ * small_line + 22;
    fill(x, y, right - x, h, 6, mix(pal::bg, box_colour_, 0.09f));
    outline(x, y, right - x, h, 6, rgb(box_colour_, 0.35f));
    draw_warning(x + 20, y + 11 + small_line / 2, 7, rgb(box_colour_));
    if (IDWriteTextLayout* l = make_layout(box_, small_font_, right - x - 46)) {
        l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, small_line, small_line * 0.78f);
        brush_->SetColor(rgb(pal::text2));
        rt_->DrawTextLayout({x + 38, y + 11}, l, brush_);
        l->Release();
    }
    return y + h;
}

// The download arrow over its tray
void Card::draw_arrow(float cx, float cy, float s, D2D1_COLOR_F colour)
{
    line(cx, cy - s, cx, cy + s * 0.45f, colour, 1.7f);
    line(cx - s * 0.6f, cy - s * 0.1f, cx, cy + s * 0.5f, colour, 1.7f);
    line(cx + s * 0.6f, cy - s * 0.1f, cx, cy + s * 0.5f, colour, 1.7f);
    line(cx - s, cy + s, cx + s, cy + s, colour, 1.7f);
}

// The buttons along the bottom, right to left, and Load at every start on the left
void Card::draw_buttons(float x, float right, float cy)
{
    const float h = 30, gap = 8;
    float at = right;
    for (const ButtonSpec& b : buttons_) {
        const float icon = b.arrow ? 18.0f : 0.0f;
        const std::wstring label = working_ && b.hit == kPrimary ? L"Installing\x2026" : b.label;
        const float tw = text_width(label, button_font_);
        const float w = std::max(84.0f, tw + icon + 32);
        const float bx = at - w, by = cy - h / 2;
        const bool over = !working_ && hover_ == b.hit;
        const float t = over && pressed_ == b.hit ? 1.0f : over ? 0.6f : 0.0f;
        D2D1_COLOR_F fg;
        if (b.style == Style::Accent) {
            fill(bx, by, w, h, 4, mix(pal::accent, pal::accent2, t * 0.6f, working_ ? 0.6f : 1.0f));
            fg = rgb(pal::ink);
        } else if (b.style == Style::Secondary) {
            fill(bx, by, w, h, 4, mix(pal::well, pal::hover, t));
            outline(bx, by, w, h, 4, mix(pal::well_edge, pal::accent_lo, t));
            fg = rgb(pal::text);
        } else {
            if (t > 0)
                fill(bx, by, w, h, 4, rgb(pal::hover, t));
            fg = mix(pal::text2, pal::text, t, working_ ? 0.4f : 1.0f);
        }
        const float lx = bx + (w - tw - icon) / 2 + icon;
        if (b.arrow)
            draw_arrow(lx - 11, cy, 5, fg);
        text_mid(label, button_font_, lx, cy, fg);
        hits_.push_back({b.hit, D2D1::RectF(bx, by, bx + w, by + h)});
        at = bx - gap;
    }
    if (!question_)
        return;
    // a switch as /vana's (32 x 17)
    const float sw = 32, sh = 17, sy = cy - sh / 2;
    const bool over = !working_ && hover_ == kSwitch;
    const float alpha = working_ ? 0.4f : 1.0f;
    fill(x, sy, sw, sh, sh / 2, boot_ ? rgb(pal::accent, alpha) : rgb(pal::off, alpha));
    if (over)
        fill(x, sy, sw, sh, sh / 2, rgb(pal::white, 0.08f));
    const float r = (sh - 4) / 2;
    disc(boot_ ? x + sw - 2 - r : x + 2 + r, cy, r, rgb(pal::knob, alpha));
    const std::wstring label = L"Load at every start";
    text_mid(label, small_font_, x + sw + 10, cy, working_ ? rgb(pal::dim) : rgb(over ? pal::white : pal::text));
    hits_.push_back({kSwitch, D2D1::RectF(x, cy - 11, x + sw + 10 + text_width(label, small_font_), cy + 11)});
}

void Card::render()
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

    const float w = kW, h = height_;
    // a soft shadow in layers, the page colour, the title block a shade lighter, and a hairline round it
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

    const float lx = kPad, right = w - kPad, top = kPad;
    draw_head(lx, top, right);
    if (!working_) { // the close X, rose under the mouse
        const float cx = right - 4, cy = top + 12, s = 5.0f;
        const bool over = hover_ == kClose;
        if (over)
            disc(cx, cy, 13, rgb(pal::rose, 0.14f));
        const D2D1_COLOR_F c = over ? rgb(pal::rose) : rgb(pal::dim);
        line(cx - s, cy - s, cx + s, cy + s, c, 1.6f);
        line(cx - s, cy + s, cx + s, cy - s, c, 1.6f);
        hits_.push_back({kClose, D2D1::RectF(cx - 13, cy - 13, cx + 13, cy + 13)});
    }
    float cy = top + kHead;
    line(1, cy - 4, w - 1, cy - 4, rgb(pal::line), 1.0f);
    if (desc_lines_ > 0) {
        cy += 14;
        const float body_line = std::floor(kBody * kLine);
        if (IDWriteTextLayout* l = make_layout(desc_, body_font_, right - lx, desc_lines_ * body_line + 1)) {
            l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, body_line, body_line * 0.78f);
            const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0};
            l->SetTrimming(&trim, ellipsis_); // past seven lines: cut with an ellipsis
            brush_->SetColor(rgb(pal::text));
            rt_->DrawTextLayout({lx, cy}, l, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            l->Release();
        }
        cy += desc_lines_ * body_line;
    }
    draw_facts(lx, cy + 16, right);
    cy += 16 + facts_.size() * kFactH;
    if (!box_.empty())
        draw_box(lx, cy + 16, right);
    draw_buttons(lx, right, h - kPad - 15);

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

Hit Card::hit_at(float px, float py) const
{
    if (working_)
        return kNone;
    const float x = px / scale_ - kShadow, y = py / scale_ - kShadow;
    for (const auto& [hit, r] : hits_)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return hit;
    return kNone;
}

// Install chosen: "Installing…" while it's written, then the installed (or wasn't installed) card in its place
void Card::run_install()
{
    working_ = true;
    render();
    std::string error;
    const Package& p = *package_;
    const bool ok = install(dir_, p, error);
    const bool at_start = ok && boot_ && load_at_start(dir_, p.name);
    working_ = false;
    if (ok)
        note(p, Badge::Check, pal::teal, display_name(p) + (p.version.empty() ? L"" : L" " + wide(p.version)) + L" is installed",
             at_start ? L"It loads the next time you start the game" : L"Switch it on in /vana > Addons when you want it");
    else
        failed(p, wide(error), true);
    if (ok)
        buttons_ = {{kPrimary, L"Done", Style::Accent}};
    hover_ = pressed_ = kNone;
    layout();
    place(false);
    render();
}

void Card::press(Hit hit)
{
    if (working_ || (!fetch_url_.empty() && hit == kPrimary)) // downloading: only Cancel or the X
        return;
    switch (hit) {
    case kClose:
    case kSecondary:
        DestroyWindow(window_);
        break;
    case kPrimary:
        if (question_)
            run_install();
        else
            DestroyWindow(window_);
        break;
    case kSwitch:
        if (question_) {
            boot_ = !boot_;
            render();
        }
        break;
    case kLink:
        if (homepage_.rfind(L"https://", 0) == 0 || homepage_.rfind(L"http://", 0) == 0)
            ShellExecuteW(window_, L"open", homepage_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK Card::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* card = reinterpret_cast<Card*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (card == nullptr || card->window_ != window)
        return DefWindowProcW(window, message, wparam, lparam);
    return card->handle(message, wparam, lparam);
}

LRESULT Card::handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    auto mouse = [&] { return hit_at(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam))); };
    switch (message) {
    case WM_NCHITTEST: { // the card drags the window anywhere but on its controls
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window_, &p);
        return hit_at(static_cast<float>(p.x), static_cast<float>(p.y)) != kNone ? HTCLIENT : HTCAPTION;
    }
    case WM_NCMOUSEMOVE:
        if (hover_ != kNone) {
            hover_ = kNone;
            render();
        }
        break;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window_, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        const Hit now = mouse();
        if (now != hover_) {
            hover_ = now;
            render();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != kNone) {
            hover_ = kNone;
            render();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && hover_ != kNone) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        pressed_ = mouse();
        SetCapture(window_);
        render();
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        const Hit up = mouse(), was = pressed_;
        pressed_ = kNone;
        render();
        if (up != kNone && up == was)
            press(up);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_RETURN)
            press(kPrimary);
        else if (wparam == VK_ESCAPE)
            press(kClose);
        else if (wparam == VK_SPACE)
            press(kSwitch);
        return 0;
    case WM_APP: // drawn again after the device was lost
        render();
        return 0;
    case kProgress: { // the download's progress, in KB
        const std::uint64_t got = static_cast<std::uint64_t>(wparam) * 1024, total = static_cast<std::uint64_t>(lparam) * 1024;
        sub_ = L"Downloading  \x00B7  " + size_text(got) + (total > 0 ? L" of " + size_text(total) : L"");
        render();
        return 0;
    }
    case kFetched:
        static_cast<void>(fetched());
        return 0;
    case WM_DPICHANGED: // moved to a screen of another DPI: sized for it
        scale_ = HIWORD(wparam) / 96.0f;
        place(false);
        render();
        return 0;
    case WM_CLOSE: // Alt+F4, or Close on the taskbar
        if (!working_)
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

// No Direct2D (a Windows too old for it, or a broken one): plain message boxes ask and tell the same
void Card::fallback()
{
    std::wstring text = title_;
    if (!sub_.empty())
        text += L"\n" + sub_;
    if (!desc_.empty())
        text += L"\n\n" + desc_;
    if (!box_.empty())
        text += L"\n\n" + box_;
    if (!question_) {
        MessageBoxW(nullptr, text.c_str(), L"VanaCore", MB_OK | (badge_ == Badge::Bang ? MB_ICONERROR : MB_ICONINFORMATION));
        return;
    }
    if (MessageBoxW(nullptr, text.c_str(), L"VanaCore", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    std::string error;
    const Package& p = *package_;
    if (!install(dir_, p, error)) {
        MessageBoxW(nullptr, (display_name(p) + L" wasn't installed\n\n" + wide(error)).c_str(), L"VanaCore", MB_OK | MB_ICONERROR);
        return;
    }
    const bool at_start = load_at_start(dir_, p.name);
    const std::wstring done = display_name(p) + L" is installed\n\n" +
                              (at_start ? L"It loads the next time you start the game." : L"Switch it on in the game: /vana > Addons.");
    MessageBoxW(nullptr, done.c_str(), L"VanaCore", MB_OK | MB_ICONINFORMATION);
}

void Card::show()
{
    if (!start_graphics()) {
        if (!fetch_url_.empty()) { // no window to show it coming: downloaded first
            start_fetch();
            if (!fetched())
                return;
        }
        fallback();
        return;
    }
    static const wchar_t kClass[] = L"VanaCore.AddonPackage.Window";
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &Card::proc;
    wc.hInstance = self();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    POINT cursor{};
    GetCursorPos(&cursor); // made on the mouse's screen, so it has that screen's DPI from the start
    HWND made = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClass, L"VanaCore", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                                cursor.x, cursor.y, 1, 1, nullptr, nullptr, self(), this);
    if (made == nullptr) {
        fallback();
        return;
    }
    window_ = made;
    using GetDpi = UINT(WINAPI*)(HWND);
    auto dpi_of = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    scale_ = (dpi_of != nullptr ? static_cast<float>(dpi_of(window_)) : 96.0f) / 96.0f;
    icon_big_ = vanacore_icon(dir_, GetSystemMetrics(SM_CXICON));
    icon_small_ = vanacore_icon(dir_, GetSystemMetrics(SM_CXSMICON));
    if (icon_big_ != nullptr)
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_big_));
    if (icon_small_ != nullptr)
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small_));

    layout();
    place(true);
    render();
    ShowWindow(window_, SW_SHOW);
    // over every other window, as one the player just asked for should be (rundll32 has no window of its own to come up
    // with), without staying on top of them after
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(window_);
    if (!fetch_url_.empty())
        start_fetch();

    MSG msg;
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    window_ = nullptr;
}

//----------------------------------------------------------------------------------------------------------------------
// The game, if one is running
//----------------------------------------------------------------------------------------------------------------------

struct WindowSearch {
    const std::wstring* dll;
    HWND found;
};

// A window whose process has `dll` loaded (EnumWindows)
BOOL CALLBACK window_of(HWND window, LPARAM lparam)
{
    auto* s = reinterpret_cast<WindowSearch*>(lparam);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId()) // this rundll32 has the DLL loaded too
        return TRUE;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr)
        return TRUE;
    HMODULE modules[512];
    DWORD needed = 0;
    if (K32EnumProcessModules(process, modules, sizeof modules, &needed)) {
        const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 512);
        for (DWORD i = 0; i < count && s->found == nullptr; ++i) {
            wchar_t path[MAX_PATH * 2] = {};
            if (K32GetModuleFileNameExW(process, modules[i], path, MAX_PATH * 2) && _wcsicmp(path, s->dll->c_str()) == 0)
                s->found = window;
        }
    }
    CloseHandle(process);
    return s->found == nullptr;
}

// The game started from this DLL's folder: its window, if one is showing
HWND game_window(const std::wstring& dll)
{
    WindowSearch search{&dll, nullptr};
    EnumWindows(&window_of, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

std::wstring pick_package()
{
    wchar_t picked[MAX_PATH * 2] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.lpstrFilter = L"VanaCore addon packages (*.vcaddon)\0*.vcaddon\0Zip files (*.zip)\0*.zip\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = picked;
    dialog.nMaxFile = static_cast<DWORD>(std::size(picked));
    dialog.lpstrTitle = L"Install an addon package";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::wstring(picked) : std::wstring();
}

// The game's running: the package waits in its inbox, and the game comes to the front, where it asks (false: nothing
// to show here), or the card says why it couldn't
bool hand_to_game(Card& card, const std::wstring& install_dir, const std::wstring& path, const Package& p)
{
    std::string error;
    if (!queue(install_dir, path, error)) {
        card.failed(p, L"It couldn't be handed to the game: " + wide(error), true);
        return true;
    }
    HWND window = game_window(module_path());
    if (window == nullptr) {
        card.note(p, Badge::Down, pal::accent, display_name(p) + L" is waiting in the game",
                  L"Switch to Final Fantasy XI: VanaCore asks there");
        return true;
    }
    if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    return false;
}

// The package at `path`: the card set to ask about it or say why not (true), or it's gone to the running game (false)
bool decide(Card& card, const std::wstring& install_dir, const std::wstring& path, Package& p)
{
    if (!inspect(install_dir, path, p)) {
        card.unreadable(path, wide(p.problem));
        return true;
    }
    if (!p.problem.empty()) {
        card.failed(p, wide(p.problem), false);
        return true;
    }
    if (game_running(install_dir))
        return hand_to_game(card, install_dir, path, p);
    card.ask(p);
    return true;
}

// A vanacore:// link: its package downloaded while the card says so, then asked about as a double-click is; the
// download deleted after (the game's inbox keeps its own copy)
void open_link(const std::wstring& install_dir, const std::wstring& link)
{
    const std::wstring url = link_target(link);
    const std::wstring dir = url.empty() ? L"" : download_dir();
    const std::wstring path = dir + L"\\" + download_name(url);
    {
        // cppcheck-suppress variableScope ; the download's callback (card.fetch) fills it, and the card outlives the branch
        Package p;
        Card card(install_dir);
        if (url.empty())
            card.missed(L"This link can't be opened", link,
                        L"It doesn't name an addon package: VanaCore's links are vanacore://install/ and the package's https "
                        L"address");
        else
            card.fetch(url, path, [&](const std::wstring& why) {
                if (why.empty())
                    return decide(card, install_dir, path, p);
                if (why != L"Cancelled")
                    card.missed(L"The package couldn't be downloaded", url, why);
                return why != L"Cancelled";
            });
        card.show();
    }
    if (!url.empty()) {
        DeleteFileW(path.c_str());
        RemoveDirectoryW(dir.c_str());
    }
}

} // namespace

void register_association(const std::wstring& install_dir)
{
    const std::wstring dll = install_dir + L"\\VanaCore.Core.dll";
    const std::wstring command = L"\"" + rundll32() + L"\" \"" + dll + L"\",VanaCoreOpenPackage \"%1\"";
    const std::wstring command_key = std::wstring(kProgKey) + L"\\shell\\open\\command";
    const std::wstring scheme_key = std::wstring(kScheme) + L"\\shell\\open\\command";
    if (read_value(command_key.c_str()) == command && read_value(kExtension) == kProgId &&
        read_value(scheme_key.c_str()) == command)
        return;
    set_value(kExtension, nullptr, kProgId);
    set_value(kProgKey, nullptr, L"VanaCore addon package");
    set_value(kProgKey, L"FriendlyTypeName", L"VanaCore addon package");
    set_value((std::wstring(kProgKey) + L"\\DefaultIcon").c_str(), nullptr, L"\"" + install_dir + L"\\VanaCore.exe\",0");
    set_value(command_key.c_str(), nullptr, command);
    // vanacore://install/<https URL> from a browser: the same command, given the link
    set_value(kScheme, nullptr, L"URL:VanaCore");
    set_value(kScheme, L"URL Protocol", L"");
    set_value((std::wstring(kScheme) + L"\\DefaultIcon").c_str(), nullptr, L"\"" + install_dir + L"\\VanaCore.exe\",0");
    set_value(scheme_key.c_str(), nullptr, command);
    // what Windows and the browsers call it ("Open VanaCore?"), not rundll32's own name
    for (const std::wstring& key : {std::wstring(kProgKey), std::wstring(kScheme)})
        set_value((key + L"\\shell\\open").c_str(), L"FriendlyAppName", L"VanaCore");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

} // namespace vanacore::addonpkg

// rundll32.exe "<VanaCore>\VanaCore.Core.dll",VanaCoreOpenPackage "<file>": a .vcaddon double-clicked (no file: Windows'
// Open dialog asks for one), or "vanacore://install/...": a link to one, opened from a browser. Exported undecorated via VanaCore.Core.def.
extern "C" void CALLBACK VanaCoreOpenPackageW(HWND, HINSTANCE, LPWSTR command_line, int)
{
    using namespace vanacore::addonpkg;
    const DialogScope scope;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // WIC, for the logo
    const std::wstring install_dir = dir_of(module_path());
    std::wstring path = command_line != nullptr ? command_line : L"";
    while (!path.empty() && (path.back() == L' ' || path.back() == L'"'))
        path.pop_back();
    while (!path.empty() && (path.front() == L' ' || path.front() == L'"'))
        path.erase(0, 1);
    if (is_link(path)) {
        open_link(install_dir, path);
    } else {
        if (path.empty())
            path = pick_package();
        if (!path.empty()) {
            Package p;
            Card card(install_dir);
            if (decide(card, install_dir, path, p))
                card.show();
        }
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}
