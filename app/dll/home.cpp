#include "home.h"

#include <shellapi.h>
#include <windowsx.h>

#include "version.h"

namespace vcapp {

namespace {

constexpr float kW = 520, kPad = 26, kHead = 76, kLogo = 56, kFactH = 22, kLabelW = 78, kButtonH = 30;
constexpr float kH = kPad + kHead + 16 + kFactH * 3 + 26 + kButtonH + kPad;
constexpr wchar_t kClass[] = L"VanaCoreApp.Home";

HMODULE self()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &module);
    return module;
}

} // namespace

Home::Home(const VanaCoreAppHost& host) : host_(host)
{
    const std::wstring exe = std::wstring(host.dir) + L"\\VanaCore.exe";
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES)
        game_ = exe;
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

void Home::render()
{
    if (!canvas_.begin())
        return;
    hits_.clear();
    canvas_.card(kPad + kHead - 4);
    const float x = kPad, right = kW - kPad;
    // the head: the logo, the name, what it is
    canvas_.logo(x - 4, kPad + 25 - kLogo / 2, kLogo);
    const float tx = x - 4 + kLogo + 14;
    canvas_.text(L"VanaCore", ui::Font::Title, tx, kPad + 13, ui::rgb(ui::pal::white));
    canvas_.text(L"Retail", ui::Font::Small, tx, kPad + 37, ui::rgb(ui::pal::dim));
    const float cx = right - 4, cy = kPad + 12;
    canvas_.close_x(cx, cy, hover_ == kClose);
    hits_.add(kClose, D2D1::RectF(cx - 13, cy - 13, cx + 13, cy + 13));
    float y = kPad + kHead;
    canvas_.line(1, y - 4, kW - 1, y - 4, ui::rgb(ui::pal::line));
    // the facts: the game it starts, the two files' versions, and what went wrong if anything did
    y += 16;
    auto fact = [&](const wchar_t* label, const std::wstring& value, std::uint32_t colour, bool path) {
        const float my = y + kFactH / 2;
        canvas_.text(label, ui::Font::Label, x, my, ui::rgb(ui::pal::faint));
        canvas_.text(canvas_.fit(value, ui::Font::Small, right - x - kLabelW, path), ui::Font::Small, x + kLabelW, my, ui::rgb(colour));
        y += kFactH;
    };
    fact(L"GAME", game_.empty() ? L"VanaCore.exe isn't in this app's folder" : game_, game_.empty() ? ui::pal::rose : ui::pal::text2, true);
    fact(L"VERSION", std::wstring(L"App ") + L"" VCAPP_DLL_VERSION_STR + L"  \x00B7  Starter " + host_.exe_version, ui::pal::text2, false);
    if (!error_.empty())
        fact(L"", error_, ui::pal::rose, false);
    // the buttons, right to left
    const float by = kH - kPad - kButtonH;
    auto button = [&](Hit hit, const wchar_t* label, ui::Button style, float end, bool on) {
        const float w = canvas_.button_width(label);
        const D2D1_RECT_F r = D2D1::RectF(end - w, by, end, by + kButtonH);
        const float t = !on || hover_ != hit ? 0.0f : pressed_ == hit ? 1.0f : 0.6f;
        canvas_.button(r, label, style, t, on);
        if (on)
            hits_.add(hit, r);
        return r.left - 8;
    };
    button(kQuit, L"Close", ui::Button::Ghost, button(kPlay, L"Play", ui::Button::Accent, right, !game_.empty()), true);
    canvas_.end();
}

void Home::press(Hit hit)
{
    if (hit == kClose || hit == kQuit) {
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
