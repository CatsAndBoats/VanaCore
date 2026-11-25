#include "home.h"

#include <shellapi.h>
#include <windowsx.h>

#include <cwchar>
#include <vector>

#pragma comment(lib, "version.lib") // (VanaCore.Core.dll's version: GetFileVersionInfoW)

#include "version.h"

namespace vcapp {

namespace {

// the background picture's shape (16:9): the head at the top, the glass bar (kBar tall) the card's width at the bottom
constexpr float kW = 640, kH = 360, kPad = 26, kLogo = 56, kBar = 86, kPlayW = 140, kPlayH = 42;
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

Home::Home(const VanaCoreAppHost& host) : host_(host)
{
    const std::wstring dir(host.dir);
    if (GetFileAttributesW((dir + L"\\VanaCore.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        game_ = dir + L"\\VanaCore.exe";
    vanacore_ = product_version(dir + L"\\VanaCore.Core.dll");
}

// VanaCore.exe --retail (PlayOnline as installed); Windows asks for administrator rights as its manifest says
bool Home::play()
{
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOASYNC;
    info.hwnd = window_;
    info.lpFile = game_.c_str();
    info.lpParameters = L"--retail";
    info.lpDirectory = host_.dir;
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info))
        return true;
    const DWORD error = GetLastError();
    error_ = error == ERROR_CANCELLED ? L"VanaCore wasn't started: it needs administrator rights" : L"VanaCore.exe couldn't be started (error " + std::to_wstring(error) + L")";
    return false;
}

Home::Status Home::status() const
{
    if (game_.empty())
        return {ui::icon::error, ui::pal::rose, L"VanaCore.exe not found", L"Put VanaCoreApp in VanaCore's folder"};
    if (!error_.empty())
        return {ui::icon::error, ui::pal::rose, L"Couldn't start VanaCore", error_};
    return {ui::icon::check, ui::pal::teal, L"Ready to play", game_};
}

// The status panel: its icon, from `x`, its middle at `cy`, `room` wide
void Home::draw_status(const Status& s, float x, float cy, float room)
{
    canvas_.glyph(s.icon, ui::Font::IconBig, x + 12, cy, ui::rgb(s.colour));
    const float tx = x + 36, w = room - 36;
    const bool bar = s.progress >= 0;
    // the two lines' middles 20 apart, 2 above the bar's middle: the title's line has room above its capitals the detail's
    // hasn't below it (seen 2026-11-25: centred on their middles, the space above looked bigger)
    const float top = cy - (bar ? 17.0f : 12.0f);
    canvas_.text(canvas_.fit(s.title, ui::Font::Body, w, false), ui::Font::Body, tx, top, ui::rgb(ui::pal::text));
    canvas_.text(canvas_.fit(s.detail, ui::Font::Small, w, true), ui::Font::Small, tx, top + 20, ui::rgb(ui::pal::dim));
    if (bar)
        canvas_.bar(tx, top + 34, w, s.progress);
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
    // the glass bar: the status on the left, Play on the right
    const float mid = kH - kBar / 2;
    const D2D1_RECT_F play = D2D1::RectF(right - kPlayW, mid - kPlayH / 2, right, mid + kPlayH / 2);
    draw_status(status(), x, mid, play.left - 24 - x);
    const bool on = !game_.empty();
    const float t = !on || hover_ != kPlay ? 0.0f : pressed_ == kPlay ? 1.0f : 0.6f;
    canvas_.button(play, L"START",ui::Button::Play, t, on, 0, ui::Font::Play);
    if (on)
        hits_.add(kPlay, play);
    canvas_.end();
}

void Home::press(Hit hit)
{
    if (hit == kClose) {
        DestroyWindow(window_);
    } else if (hit == kPlay && !game_.empty()) {
        if (play())
            DestroyWindow(window_);
        else
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
    const Hit at = hits_.at(canvas_.to_card(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), kNone);
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
        const Hit was = pressed_;
        pressed_ = kNone;
        if (at != kNone && at == was) {
            press(at);
            return;
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
        return hits_.at(canvas_.to_card(p.x, p.y), kNone) != kNone ? HTCLIENT : HTCAPTION;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        mouse(message, lparam);
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
        if (wparam == VK_RETURN)
            press(kPlay);
        else if (wparam == VK_ESCAPE)
            press(kClose);
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
