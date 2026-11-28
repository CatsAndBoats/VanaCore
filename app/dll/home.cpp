#include "home.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>

#pragma comment(lib, "version.lib") // (VanaCore.Core.dll's version: GetFileVersionInfoW)

#include "version.h"
#include "../../src/core/folders_make.h" // (VanaCore's own: the same folders and READMEs it makes at start)

namespace vcapp {

namespace {

// the head at the top, the tabs and their page under it, the glass bar (kBar tall) the card's width at the bottom
constexpr float kW = 640, kH = 480, kPad = 26, kLogo = 56, kBar = 86, kPlayW = 140, kPlayH = 42;
constexpr std::uint32_t kVersionColour = 0xF3DE9C; // the head's version: a light yellow
constexpr wchar_t kClass[] = L"VanaCoreApp.Home";
constexpr UINT_PTR kTipTimer = 1;
constexpr UINT kTipDelay = 450; // ms the mouse rests on a control before its tooltip shows

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
    reload_version();
    if (!game_.empty()) // VanaCore's addon and plugin folders, each with its README, where they aren't there
        vanacore::folders::make(dir_, [](const std::wstring&) {});
    load_profiles(); // (its file: ini_)
    detect();
    load_pivot();
    if (!errors().empty())
        tab_ = kErrors;
}

void Home::reload_version()
{
    vanacore_ = product_version(dir_ + L"\\VanaCore.Core.dll");
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
    tips_.clear();
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
        if (up_to_date()) { // (home_update.cpp found nothing newer, or just updated it): a green pill beside the version
            const std::wstring pill = L"UP TO DATE";
            const float pw = canvas_.text_width(pill, ui::Font::Label) + 16, ph = 18, px = bx + 8;
            canvas_.fill(px, sy - ph / 2, pw, ph, ph / 2, ui::rgb(ui::pal::teal, 0.18f));
            canvas_.outline(px, sy - ph / 2, pw, ph, ph / 2, ui::rgb(ui::pal::teal, 0.6f));
            canvas_.text_centred(pill, ui::Font::Label, px + pw / 2, sy, ui::rgb(ui::pal::teal));
            bx = px + pw - 4;
        }
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
    if (drop_ >= 0 && tab_ == kGraphics)
        draw_drop();
    else
        drop_ = -1;
    draw_tip();
    canvas_.end();
}

// The glass bar: the profile and START side by side in its middle
void Home::draw_footer()
{
    const float mid = kH - kBar / 2;
    if (draw_update(kW, kH, mid)) // (VanaCore being updated: its line and bar in their place, home_update.cpp)
        return;
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
    tip(kProfile, L"Pick a profile");
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

// The hovered control's tooltip, once the mouse has rested on it: under it (above when there's no room), its middle on the
// control's, kept on the card
void Home::draw_tip()
{
    if (!tip_up_ || pressed_ != kNone || hover_ == kNone)
        return;
    const std::wstring* text = nullptr;
    for (const auto& [id, t] : tips_)
        if (id == hover_)
            text = &t;
    D2D1_RECT_F at{};
    bool found = false;
    for (const auto& [id, r] : hits_.list)
        if (id == hover_ && !found)
            at = r, found = true;
    if (text == nullptr || !found)
        return;
    const float w = canvas_.text_width(*text, ui::Font::Small) + 20, h = 24;
    float x = (at.left + at.right) / 2 - w / 2, y = at.bottom + 6;
    if (y + h > kH - 6)
        y = at.top - 6 - h;
    x = std::max(8.0f, std::min(x, kW - 8 - w));
    for (int i = 4; i >= 1; --i) {
        const float g = static_cast<float>(i);
        canvas_.fill(x - g, y - g + 3, w + 2 * g, h + 2 * g, 5 + g, ui::rgb(0x000000, 0.07f));
    }
    canvas_.fill(x, y, w, h, 5, ui::rgb(0x0B0E18, 0.97f));
    canvas_.outline(x, y, w, h, 5, ui::rgb(ui::pal::white, 0.16f));
    canvas_.text(*text, ui::Font::Small, x + 10, y + h / 2, ui::rgb(ui::pal::text));
}

// The tab bar: each tab's name in a pill, the picked one lit with an accent edge; Errors (when something's wrong) in rose
void Home::draw_tabs(float x, float y)
{
    static const wchar_t* const kNames[kTabs] = {L"FFXI", L"GRAPHICS", L"PROFILES", L"PIVOT", L"ADDONS", L"LOGS", L"ABOUT", L"ERRORS"};
    constexpr float kTabH = 26, kGap = 6;
    const std::size_t wrong = errors().size();
    auto name_of = [&](int i) { return i == kErrors ? std::wstring(kNames[i]) + L"  " + std::to_wstring(wrong) : std::wstring(kNames[i]); };
    // each tab's name 14 in from its edges, less when they'd run past the card's edge (Errors showing)
    float names = 0;
    int shown = 0;
    for (int i = 0; i < kTabs; ++i)
        if (i != kErrors || wrong != 0)
            names += canvas_.text_width(name_of(i), ui::Font::Label), ++shown;
    const float pad = std::max(6.0f, std::min(14.0f, (kW - kPad + 4 - x - names - kGap * (shown - 1)) / (2.0f * shown)));
    for (int i = 0; i < kTabs; ++i) {
        if (i == kErrors && wrong == 0)
            continue;
        const std::wstring name = name_of(i);
        const std::uint32_t edge = i == kErrors ? ui::pal::rose : ui::pal::accent;
        const float w = canvas_.text_width(name, ui::Font::Label) + pad * 2;
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
        x += w + kGap;
    }
}

void Home::press(int hit)
{
    if (renaming_ >= 0 && hit != kProfileRow + renaming_ * 8 + 3) // (3: the name's box itself)
        end_rename(true);
    if (drop_ >= 0) { // a click anywhere with a drop-down open: it shuts (a pick in its list kept)
        if (hit >= kDropItem)
            pick(hit - kDropItem);
        drop_ = -1;
        render();
        return;
    }
    if (hit >= kDrop && hit < kDrop + kDrops) { // a drop-down opens, the list scrolled to what it has
        drop_ = hit - kDrop;
        const std::vector<Choice> list = choices(drop_);
        const std::wstring now = drop_value(drop_);
        drop_scroll_ = 0;
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (!list[i].sep && _wcsicmp(list[i].value.c_str(), now.c_str()) == 0)
                drop_scroll_ = std::max(0, i - 3);
        menu_ = false;
        render();
        return;
    }
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
        if (migration_->running) // (not while addons are being copied: the copy would stop half done)
            return;
        DestroyWindow(window_);
    } else if (hit == kProfile) {
        menu_ = !menu_;
        render();
    } else if (hit == kPlay) {
        if (!can_play() || updating() || migration_->running)
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
        if (tab_ == kLogs)
            load_logs();
        if (tab_ == kAddons)
            count_addons();
        render();
    } else if (press_addons(hit) || press_profiles(hit) || press_page(hit)) {
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
        tip_up_ = false; // (its tooltip after the mouse rests on it)
        SetTimer(window_, kTipTimer, kTipDelay, nullptr);
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
        if (at == kNone && (menu_ || drop_ >= 0 || renaming_ >= 0)) { // a click on nothing: the lists shut, a name being typed is kept
            menu_ = false;
            drop_ = -1;
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
        return menu_ || drop_ >= 0 || renaming_ >= 0 || hits_.at(canvas_.to_card(p.x, p.y), kNone) != kNone ? HTCLIENT : HTCAPTION;
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
    case WM_MOUSEWHEEL: // an open drop-down's list, the Pivot and Profiles lists scroll (clamped when drawn)
        if (drop_ >= 0) {
            drop_scroll_ = std::max(0, drop_scroll_ + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -2 : 2));
            render();
        } else if (tab_ == kPivot || tab_ == kProfiles || tab_ == kLogs) {
            int& scroll = tab_ == kPivot ? pivot_scroll_ : tab_ == kLogs ? logs_scroll_ : profile_scroll_;
            const int before = scroll,
                      most = static_cast<int>(tab_ == kPivot ? overlays_.size() : tab_ == kLogs ? logs_.size() : profiles_.size());
            const int step = tab_ == kLogs ? 3 : 1;
            scroll = std::max(0, std::min(scroll + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -step : step), most));
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
        } else if ((menu_ || drop_ >= 0) && wparam == VK_ESCAPE) {
            menu_ = false;
            drop_ = -1;
            render();
        } else if (wparam == VK_RETURN) {
            press(kPlay);
        } else if (wparam == VK_ESCAPE) {
            press(kClose);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kTipTimer) {
            KillTimer(window_, kTipTimer);
            tip_up_ = hover_ != kNone;
            if (tip_up_)
                render();
            return 0;
        }
        break;
    case ui::kRedraw:
        updated(); // (VanaCore just updated: the head's version)
        render();
        return 0;
    case WM_DPICHANGED:
        canvas_.dpi_changed(HIWORD(wparam));
        render();
        return 0;
    case WM_CLOSE:
        if (!migration_->running)
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
    check_update(); // (before the first frame: a VanaCore being installed here isn't shown as missing first)
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