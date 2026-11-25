#include "home.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>

#pragma comment(lib, "version.lib") // (VanaCore.Core.dll's version: GetFileVersionInfoW)

#include "version.h"

namespace vcapp {

namespace {

// the head at the top, the tabs and their page under it, the glass bar (kBar tall) the card's width at the bottom
constexpr float kW = 640, kH = 420, kPad = 26, kLogo = 56, kBar = 86, kPlayW = 140, kPlayH = 42;
constexpr std::uint32_t kVersionColour = 0xF3DE9C; // the head's version: a light yellow
constexpr wchar_t kClass[] = L"VanaCoreApp.Home";

HMODULE self()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &module);
    return module;
}

// `path`'s ProductVersion (its version resource's string), "" when it has none
std::wstring product_version(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
        return {};
    std::vector<BYTE> info(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, info.data()))
        return {};
    struct Translation {
        WORD language, codepage;
    }* translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) ||
        length < sizeof(Translation))
        return {};
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translations[0].language, translations[0].codepage);
    wchar_t* text = nullptr;
    if (!VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&text), &length) || length == 0)
        return {};
    return text;
}

} // namespace

Home::Home(const VanaCoreAppHost& host) : host_(host), dir_(host.dir), app_ini_(dir_ + L"\\vanacore.ini"), ini_(app_ini_)
{
    if (GetFileAttributesW((dir_ + L"\\VanaCore.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        game_ = dir_ + L"\\VanaCore.exe";
    vanacore_ = product_version(dir_ + L"\\VanaCore.Core.dll");
    load_profiles(); // (its file: ini_)
    detect();
    load_pivot();
    if (!errors().empty())
        tab_ = kErrors;
}

bool Home::can_play() const
{
    return !game_.empty() && !install_.pol.empty() &&
           (_wcsicmp(profiles_[profile_].file.c_str(), L"vanacore.ini") == 0 || GetFileAttributesW(ini_.c_str()) != INVALID_FILE_ATTRIBUTES);
}

// VanaCore.exe --retail (PlayOnline as installed), with the profile's settings file (--ini; none for vanacore.ini); Windows
// asks for administrator rights as its manifest says
bool Home::play()
{
    const std::wstring& file = profiles_[profile_].file;
    const std::wstring args = _wcsicmp(file.c_str(), L"vanacore.ini") == 0 ? L"--retail" : L"--retail --ini \"" + file + L"\"";
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOASYNC;
    info.hwnd = window_;
    info.lpFile = game_.c_str();
    info.lpParameters = args.c_str();
    info.lpDirectory = host_.dir;
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info))
        return true;
    const DWORD error = GetLastError();
    error_ = error == ERROR_CANCELLED ? L"Windows asked for administrator rights and they weren't given. VanaCore needs them to start PlayOnline: press START again and choose Yes."
                                     : L"Windows couldn't start " + game_ + L" (error " + std::to_wstring(error) + L"). Check it's there and that nothing (an antivirus) is holding it.";
    return false;
}

void Home::render()
{
    if (!canvas_.begin())
        return;
    hits_.clear();
    canvas_.card(0);
    const D2D1_RECT_F glass = D2D1::RectF(0, kH - kBar, kW, kH);
    canvas_.backdrop(&glass);
    const float x = kPad, right = kW - kPad;
    // the head: the logo, the name, and under it the installed VanaCore's version (light yellow) and who made it
    canvas_.logo(x - 4, kPad + 25 - kLogo / 2, kLogo);
    const float tx = x - 4 + kLogo + 14, sy = kPad + 37;
    canvas_.text(L"VanaCore", ui::Font::Title, tx, kPad + 13, ui::rgb(ui::pal::white));
    float bx = tx;
    if (!vanacore_.empty()) {
        const std::wstring version = L"v" + vanacore_, dash = L"  -  ";
        canvas_.text(version, ui::Font::Small, bx, sy, ui::rgb(kVersionColour));
        bx += canvas_.text_width(version, ui::Font::Small);
        canvas_.text(dash, ui::Font::Small, bx, sy, ui::rgb(ui::pal::dim));
        bx += canvas_.text_width(dash, ui::Font::Small);
    }
    canvas_.text(L"by: Vekien & Carver", ui::Font::Small, bx, sy, ui::rgb(ui::pal::text2));
    const float cx = right - 4, cy = kPad + 12;
    canvas_.close_x(cx, cy, hover_ == kClose);
    hits_.add(kClose, D2D1::RectF(cx - 13, cy - 13, cx + 13, cy + 13));
    // the tabs under the logo, their page under them
    if (tab_ == kErrors && errors().empty())
        tab_ = kFfxi;
    draw_tabs(x - 4, kPad + kLogo + 16);
    draw_page(D2D1::RectF(x - 8, kPad + kLogo + 50, right + 8, kH - kBar - 14));
    draw_footer();
    if (menu_)
        draw_menu();
    canvas_.end();
}

// The glass bar: the profile and START side by side in its middle
void Home::draw_footer()
{
    const float mid = kH - kBar / 2;
    std::wstring name = profiles_[profile_].name;
    for (wchar_t& c : name)
        c = static_cast<wchar_t>(towupper(c));
    const float nw = canvas_.text_width(name, ui::Font::Status), pw = std::max(130.0f, nw + 64), gap = 10;
    const float left = (kW - pw - gap - kPlayW) / 2;
    const D2D1_RECT_F profile = D2D1::RectF(left, mid - kPlayH / 2, left + pw, mid + kPlayH / 2);
    // the profile: a dark glass button, START's lettering, a caret; its pick-list opens above it
    const bool lit = hover_ == kProfile || menu_;
    for (int i = 5; i >= 1; --i) {
        const float g = static_cast<float>(i);
        canvas_.fill(profile.left - g, profile.top - g + 3, pw + 2 * g, kPlayH + 2 * g, 4 + g, ui::rgb(0x020818, 0.07f));
    }
    canvas_.fill(profile.left, profile.top, pw, kPlayH, 4, ui::rgb(ui::pal::ink, lit ? 0.7f : 0.82f));
    canvas_.outline(profile.left, profile.top, pw, kPlayH, 4, lit ? ui::rgb(0x5FF0FF, 0.8f) : ui::rgb(ui::pal::white, 0.22f));
    canvas_.styled(name, ui::Font::Status, profile.left + (pw - 24 - nw) / 2 + 4, mid);
    canvas_.glyph(menu_ ? ui::icon::up : ui::icon::down, ui::Font::IconBig, profile.right - 20, mid, ui::rgb(lit ? ui::pal::white : ui::pal::text2));
    hits_.add(kProfile, profile);
    const D2D1_RECT_F play = D2D1::RectF(profile.right + gap, mid - kPlayH / 2, profile.right + gap + kPlayW, mid + kPlayH / 2);
    const bool on = can_play();
    const float t = !on || hover_ != kPlay ? 0.0f : pressed_ == kPlay ? 1.0f : 0.6f;
    canvas_.button(play, L"START", ui::Button::Play, t, on, 0, ui::Font::Play);
    if (on)
        hits_.add(kPlay, play);
}

// The profile pick-list, above the footer's profile button: each profile (the one in use ticked), then Manage profiles
void Home::draw_menu()
{
    const float row = 32, mid = kH - kBar / 2;
    D2D1_RECT_F button{};
    for (const auto& [id, r] : hits_.list)
        if (id == kProfile)
            button = r;
    const int shown = std::min<int>(static_cast<int>(profiles_.size()), 6);
    const float w = std::max(button.right - button.left, 220.0f), h = row * (shown + 1) + 10;
    const D2D1_RECT_F m = D2D1::RectF(button.left, button.top - 8 - h, button.left + w, button.top - 8);
    (void)mid;
    for (int i = 6; i >= 1; --i) {
        const float g = static_cast<float>(i);
        canvas_.fill(m.left - g, m.top - g + 4, w + 2 * g, h + 2 * g, 6 + g, ui::rgb(0x000000, 0.07f));
    }
    canvas_.fill(m.left, m.top, w, h, 6, ui::rgb(0x0B0E18, 0.98f));
    canvas_.outline(m.left, m.top, w, h, 6, ui::rgb(ui::pal::white, 0.14f));
    float y = m.top + 5;
    for (int i = 0; i < shown; ++i, y += row) {
        const bool over = hover_ == kMenu + i, picked = i == profile_;
        if (over)
            canvas_.fill(m.left + 4, y, w - 8, row, 4, ui::rgb(ui::pal::white, 0.08f));
        canvas_.glyph(picked ? ui::icon::check : ui::icon::circle, ui::Font::Icon, m.left + 22, y + row / 2,
                      ui::rgb(picked ? ui::pal::teal : ui::pal::faint));
        canvas_.text(canvas_.fit(profiles_[i].name, ui::Font::Body, w - 56, false), ui::Font::Body, m.left + 40, y + row / 2 - 1,
                     ui::rgb(picked || over ? ui::pal::white : ui::pal::text2));
        hits_.add(kMenu + i, D2D1::RectF(m.left + 4, y, m.right - 4, y + row));
    }
    canvas_.line(m.left + 10, y + 0.5f, m.right - 10, y + 0.5f, ui::rgb(ui::pal::white, 0.08f));
    const bool over = hover_ == kMenuManage;
    if (over)
        canvas_.fill(m.left + 4, y + 1, w - 8, row - 2, 4, ui::rgb(ui::pal::white, 0.08f));
    canvas_.glyph(ui::icon::settings, ui::Font::Icon, m.left + 22, y + row / 2, ui::rgb(over ? ui::pal::white : ui::pal::text2));
    canvas_.text(L"Manage profiles\x2026", ui::Font::Body, m.left + 40, y + row / 2 - 1, ui::rgb(over ? ui::pal::white : ui::pal::text2));
    hits_.add(kMenuManage, D2D1::RectF(m.left + 4, y, m.right - 4, y + row));
    // (the list's rows first: Hits::at takes the first match, and the list lies over the page)
    std::stable_partition(hits_.list.begin(), hits_.list.end(), [](const auto& h) { return h.first >= kMenu && h.first <= kMenuManage; });
}

// The tab bar: each tab's name in a pill, the picked one lit with an accent edge; Errors (when something's wrong) in rose
void Home::draw_tabs(float x, float y)
{
    static const wchar_t* const kNames[kTabs] = {L"FFXI", L"GRAPHICS", L"PIVOT", L"PROFILES", L"ABOUT", L"ERRORS"};
    constexpr float kTabH = 26, kTabPad = 14;
    const std::size_t wrong = errors().size();
    for (int i = 0; i < kTabs; ++i) {
        if (i == kErrors && wrong == 0)
            continue;
        const std::wstring name = i == kErrors ? std::wstring(kNames[i]) + L"  " + std::to_wstring(wrong) : kNames[i];
        const std::uint32_t edge = i == kErrors ? ui::pal::rose : ui::pal::accent;
        const float w = canvas_.text_width(name, ui::Font::Label) + kTabPad * 2;
        const bool picked = tab_ == i, over = hover_ == kTab + i;
        if (picked) {
            canvas_.fill(x, y, w, kTabH, kTabH / 2, ui::rgb(edge, 0.22f));
            canvas_.outline(x, y, w, kTabH, kTabH / 2, ui::rgb(edge, 0.7f));
        } else if (over) {
            canvas_.fill(x, y, w, kTabH, kTabH / 2, ui::rgb(i == kErrors ? ui::pal::rose : ui::pal::white, i == kErrors ? 0.14f : 0.08f));
        } else if (i == kErrors) {
            canvas_.fill(x, y, w, kTabH, kTabH / 2, ui::rgb(ui::pal::rose, 0.1f));
        }
        const std::uint32_t colour = i == kErrors ? (picked ? ui::pal::white : ui::pal::rose) : picked ? ui::pal::white : over ? ui::pal::text : ui::pal::text2;
        canvas_.text_centred(name, ui::Font::Label, x + w / 2, y + kTabH / 2, ui::rgb(colour));
        hits_.add(kTab + i, D2D1::RectF(x, y, x + w, y + kTabH));
        x += w + 6;
    }
}

void Home::press(int hit)
{
    if (renaming_ >= 0 && hit != kProfileRow + renaming_ * 4 + 3) // (3: the name's box itself)
        end_rename(true);
    if (menu_ && hit != kProfile) { // a click anywhere with the pick-list open: it shuts (and does what it was on)
        menu_ = false;
        if (hit >= kMenu && hit < kMenuManage)
            use_profile(hit - kMenu);
        else if (hit == kMenuManage)
            tab_ = kProfiles;
        render();
        return;
    }
    if (hit == kClose) {
        DestroyWindow(window_);
    } else if (hit == kProfile) {
        menu_ = !menu_;
        render();
    } else if (hit == kPlay) {
        if (!can_play())
            return;
        error_.clear();
        if (play()) {
            DestroyWindow(window_);
            return;
        }
        tab_ = kErrors;
        render();
    } else if (hit >= kTab && hit < kTab + kTabs) {
        tab_ = static_cast<Tab>(hit - kTab);
        render();
    } else if (press_profiles(hit) || press_page(hit)) {
        render();
    } else {
        render();
    }
}

LRESULT CALLBACK Home::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* home = reinterpret_cast<Home*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (home == nullptr || home->window_ != window)
        return DefWindowProcW(window, message, wparam, lparam);
    return home->handle(message, wparam, lparam);
}

// The mouse over the card: hover, press, and a click where it was pressed
void Home::mouse(UINT message, LPARAM lparam)
{
    const int at = hits_.at(canvas_.to_card(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), kNone);
    if (message == WM_MOUSEMOVE) {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window_, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        if (at == hover_)
            return;
        hover_ = at;
    } else if (message == WM_LBUTTONDOWN) {
        pressed_ = at;
        SetCapture(window_);
    } else {
        ReleaseCapture();
        const int was = pressed_;
        pressed_ = kNone;
        if (at != kNone && at == was) {
            press(at);
            return;
        }
        if (at == kNone && (menu_ || renaming_ >= 0)) { // a click on nothing: the pick-list shuts, a name being typed is kept
            menu_ = false;
            if (renaming_ >= 0)
                end_rename(true);
        }
    }
    render();
}

LRESULT Home::handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_NCHITTEST: { // the card drags the window anywhere but on its controls
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window_, &p);
        // (with the pick-list open or a name being typed, a click anywhere is the card's: it shuts them)
        return menu_ || renaming_ >= 0 || hits_.at(canvas_.to_card(p.x, p.y), kNone) != kNone ? HTCLIENT : HTCAPTION;
    }
    case WM_CHAR:
        if (rename_key(message, wparam))
            render();
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        mouse(message, lparam);
        return 0;
    case WM_MOUSEWHEEL: // the Pivot and Profiles lists scroll (clamped when drawn)
        if (tab_ == kPivot || tab_ == kProfiles) {
            int& scroll = tab_ == kPivot ? pivot_scroll_ : profile_scroll_;
            const int before = scroll, most = static_cast<int>(tab_ == kPivot ? overlays_.size() : profiles_.size());
            scroll = std::max(0, std::min(scroll + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1), most));
            if (scroll != before)
                render();
        }
        return 0;
    case WM_MOUSELEAVE:
    case WM_NCMOUSEMOVE:
        tracking_ = message == WM_MOUSELEAVE ? false : tracking_;
        if (hover_ != kNone)
            hover_ = kNone, render();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && hover_ != kNone) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (rename_key(message, wparam)) { // (a name being typed takes Enter and Esc)
            render();
        } else if (menu_ && wparam == VK_ESCAPE) {
            menu_ = false;
            render();
        } else if (wparam == VK_RETURN) {
            press(kPlay);
        } else if (wparam == VK_ESCAPE) {
            press(kClose);
        }
        return 0;
    case ui::kRedraw:
        render();
        return 0;
    case WM_DPICHANGED:
        canvas_.dpi_changed(HIWORD(wparam));
        render();
        return 0;
    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

int Home::run()
{
    if (!canvas_.start(self())) { // no Direct2D: asked plainly
        host_.splash_close();
        if (game_.empty()) {
            MessageBoxW(nullptr, L"VanaCore.exe isn't in this app's folder.", L"VanaCore", MB_OK | MB_ICONERROR);
            return 1;
        }
        return MessageBoxW(nullptr, L"Play FINAL FANTASY XI with VanaCore?", L"VanaCore", MB_OKCANCEL | MB_ICONQUESTION) == IDOK && play() ? 0 : 1;
    }
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &Home::proc;
    wc.hInstance = self();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = host_.icon_big;
    wc.hIconSm = host_.icon_small;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    POINT cursor{};
    GetCursorPos(&cursor); // made on the mouse's screen, so it has that screen's DPI from the start
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClass, L"VanaCore", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, cursor.x,
                                  cursor.y, 1, 1, nullptr, nullptr, self(), this);
    if (window == nullptr) {
        host_.splash_close();
        return 1;
    }
    window_ = window;
    canvas_.attach(window, kW, kH);
    render();
    ShowWindow(window, SW_SHOW);
    // over every other window (where the splash was), as the .vcaddon card comes up, without staying on top of them after:
    // Windows may refuse the foreground to a window that's a different thread's than the splash (seen 2026-11-25)
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(window);
    host_.splash_close(); // the app's up: the splash goes
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

} // namespace vcapp