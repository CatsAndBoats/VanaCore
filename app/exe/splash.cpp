#include "splash.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace vcapp {

namespace {

constexpr float kW = 360, kH = 232;
constexpr float kLogo = 84, kTrackX = 48, kTrackY = 200, kTrackH = 4;
constexpr UINT kStatus = WM_APP + 1, kClosing = WM_APP + 2;
constexpr UINT_PTR kSweepTimer = 1;
constexpr wchar_t kClass[] = L"VanaCoreApp.Splash";

} // namespace

Splash::~Splash()
{
    close();
    if (thread_.joinable())
        thread_.join();
    if (ready_ != nullptr)
        CloseHandle(ready_);
}

void Splash::show()
{
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
    WaitForSingleObject(ready_, 5000);
}

void Splash::status(const std::wstring& text, float progress, bool note)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text_ = text;
        progress_ = progress;
        note_ = note;
    }
    if (const HWND window = window_)
        PostMessageW(window, kStatus, 0, 0);
}

void Splash::close()
{
    if (const HWND window = window_)
        PostMessageW(window, kClosing, 0, 0);
}

void Splash::run()
{
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &Splash::proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    POINT cursor{};
    GetCursorPos(&cursor); // made on the mouse's screen, so it has that screen's DPI from the start
    HWND window = canvas_.start(instance_) ? CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClass, L"VanaCore", WS_POPUP | WS_SYSMENU,
                                                             cursor.x, cursor.y, 1, 1, nullptr, nullptr, instance_, this)
                                           : nullptr;
    if (window == nullptr) {
        SetEvent(ready_);
        return;
    }
    canvas_.attach(window, kW, kH);
    window_ = window;
    render();
    ShowWindow(window, SW_SHOW);
    // over every other window without staying on top of them after (the foreground may be refused it)
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(window);
    SetTimer(window, kSweepTimer, 16, nullptr);
    SetEvent(ready_);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void Splash::render()
{
    if (!canvas_.begin())
        return;
    std::wstring text;
    float progress = 0;
    bool note = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text = text_;
        progress = progress_;
        note = note_;
    }
    hits_.clear();
    canvas_.card(0);
    canvas_.logo((kW - kLogo) / 2, 26, kLogo);
    canvas_.text_centred(L"VanaCore", ui::Font::Big, kW / 2, 136, ui::rgb(ui::pal::white));
    canvas_.text_centred(canvas_.fit(text, ui::Font::Small, kW - kTrackX * 2 + 24, false), ui::Font::Small, kW / 2, 172,
                         ui::rgb(note ? ui::pal::amber : ui::pal::dim));
    // the bar: filled to the progress, or a sweep across it while there's none to show
    const float tw = kW - kTrackX * 2;
    canvas_.fill(kTrackX, kTrackY, tw, kTrackH, kTrackH / 2, ui::rgb(ui::pal::well_edge));
    if (progress >= 0) {
        canvas_.fill(kTrackX, kTrackY, tw * std::min(progress, 1.0f), kTrackH, kTrackH / 2, ui::rgb(ui::pal::accent));
    } else {
        const float seg = tw * 0.3f, at = kTrackX - seg + (tw + seg) * sweep_;
        canvas_.clip(D2D1::RectF(kTrackX, kTrackY, kTrackX + tw, kTrackY + kTrackH));
        canvas_.fill(at, kTrackY, seg, kTrackH, kTrackH / 2, ui::rgb(ui::pal::accent));
        canvas_.unclip();
    }
    const float cx = kW - 22, cy = 22;
    canvas_.close_x(cx, cy, hover_ == kClose);
    hits_.add(kClose, D2D1::RectF(cx - 13, cy - 13, cx + 13, cy + 13));
    canvas_.end();
}

LRESULT CALLBACK Splash::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* splash = reinterpret_cast<Splash*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (splash == nullptr || splash->window_ != window)
        return DefWindowProcW(window, message, wparam, lparam);
    return splash->handle(message, wparam, lparam);
}

LRESULT Splash::handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    const HWND window = window_;
    switch (message) {
    case WM_NCHITTEST: { // dragged anywhere but its X
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &p);
        return hits_.at(canvas_.to_card(p.x, p.y), kNone) != kNone ? HTCLIENT : HTCAPTION;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        const Hit now = hits_.at(canvas_.to_card(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), kNone);
        if (now != hover_)
            hover_ = now, render();
        return 0;
    }
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
    case WM_LBUTTONUP:
        if (hits_.at(canvas_.to_card(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), kNone) == kClose) {
            cancelled_ = true;
            DestroyWindow(window);
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            cancelled_ = true;
            DestroyWindow(window);
        }
        return 0;
    case WM_TIMER:
        sweep_ = std::fmod(sweep_ + 0.012f, 1.0f);
        render();
        return 0;
    case kStatus:
    case ui::kRedraw:
        render();
        return 0;
    case WM_DPICHANGED:
        canvas_.dpi_changed(HIWORD(wparam));
        render();
        return 0;
    case kClosing:
        DestroyWindow(window);
        return 0;
    case WM_CLOSE: // Alt+F4: as its X
        cancelled_ = true;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kSweepTimer);
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace vcapp
