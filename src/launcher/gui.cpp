#include <windows.h>

#include <commdlg.h>

#include "gui.hpp"

#pragma comment(lib, "comdlg32.lib")

namespace vanacore {
namespace {

enum Id {
    kRetail = 101,
    kPrivate,
    kServerLabel,
    kServer,
    kIniLabel,
    kIni,
    kBrowse,
    kStart = IDOK,
    kCancel = IDCANCEL,
};

struct State {
    GuiChoice* choice = nullptr;
    bool started = false;
};

std::wstring text_of(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

HWND make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
          HFONT font, DWORD ex = 0)
{
    HWND control = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

// the server and settings-file rows only matter for a private server
void sync(HWND window)
{
    const bool priv = IsDlgButtonChecked(window, kPrivate) == BST_CHECKED;
    EnableWindow(GetDlgItem(window, kServer), priv);
    EnableWindow(GetDlgItem(window, kServerLabel), priv);
}

void browse(HWND window)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.hwndOwner = window;
    open.lpstrFilter = L"Settings (*.ini)\0*.ini\0All files\0*.*\0";
    open.lpstrFile = file;
    open.nMaxFile = MAX_PATH;
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&open))
        SetWindowTextW(GetDlgItem(window, kIni), file);
}

LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const GuiChoice& c = *state->choice;
        make(window, L"STATIC", L"VanaCore", SS_LEFT, 16, 12, 360, 20, 0, font);
        make(window, L"BUTTON", L"Retail (PlayOnline as installed)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 16, 40,
             360, 20, kRetail, font);
        make(window, L"BUTTON", L"Private server (VanaCore's own login)", BS_AUTORADIOBUTTON, 16, 62, 360, 20, kPrivate,
             font);
        make(window, L"STATIC", L"Server:", SS_LEFT, 32, 92, 60, 18, kServerLabel, font);
        make(window, L"EDIT", c.server.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 96, 88, 280, 22, kServer, font,
             WS_EX_CLIENTEDGE);
        make(window, L"STATIC", L"Settings:", SS_LEFT, 16, 124, 70, 18, kIniLabel, font);
        make(window, L"EDIT", c.ini.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 96, 120, 210, 22, kIni, font,
             WS_EX_CLIENTEDGE);
        make(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 312, 119, 64, 24, kBrowse, font);
        make(window, L"BUTTON", L"Start Game", BS_DEFPUSHBUTTON | WS_TABSTOP, 200, 160, 84, 28, kStart, font);
        make(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 292, 160, 84, 28, kCancel, font);
        CheckRadioButton(window, kRetail, kPrivate, c.retail ? kRetail : kPrivate);
        sync(window);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kRetail:
        case kPrivate:
            sync(window);
            return 0;
        case kBrowse:
            browse(window);
            return 0;
        case kStart:
            state->choice->retail = IsDlgButtonChecked(window, kRetail) == BST_CHECKED;
            state->choice->server = text_of(GetDlgItem(window, kServer));
            state->choice->ini = text_of(GetDlgItem(window, kIni));
            state->started = true;
            DestroyWindow(window);
            return 0;
        case kCancel:
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool show_start_screen(GuiChoice& choice)
{
    State state;
    state.choice = &choice;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = &proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.lpszClassName = L"VanaCoreStartScreen";
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassW(&cls);

    RECT rect{0, 0, 392, 204};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"VanaCore", style, x, y, w, h, nullptr, nullptr, instance, &state);
    if (window == nullptr)
        return false;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return state.started;
}

} // namespace vanacore
