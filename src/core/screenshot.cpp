#include "screenshot.hpp"

#include <windows.h>

#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chat.hpp"
#include "d3d8.hpp"
#include "game.hpp"
#include "graphics.hpp"
#include "log.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace vanacore::screenshot {
namespace {

using d3d8::call;
namespace slot = d3d8::slot;

std::wstring g_folder;          // <install>\screenshots
bool g_default_overlays = false; // [screenshots] overlays=
std::atomic<bool> g_copy_to_clipboard{true}; // [screenshots] clipboard=
std::atomic<int> g_pending{-1}; // the next frame's shot: -1 none, 0 without VanaCore's drawing, 1 with (asked on the hook thread)
std::atomic<ULONGLONG> g_last_request{0};

// Finished (or failed) shots, from the encoding threads, for the chat log.
std::mutex g_lock;
std::deque<std::string> g_results;

// A captured frame, 24-bit BGR rows (bottom-up is not used: top row first).
struct Frame {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::vector<BYTE> pixels;
};

void report(std::string message)
{
    const std::lock_guard lock(g_lock);
    g_results.push_back(std::move(message));
}

// Copies the back buffer into `frame`. Render targets can't be locked, so it goes through a system
// memory surface of the same format.
bool capture(void* device, Frame& frame, std::string& error)
{
    void* back = nullptr;
    if (FAILED(call(device, slot::GetBackBuffer, 0u, d3d8::kBackBufferMono, &back)) || back == nullptr) {
        error = "no back buffer";
        return false;
    }
    d3d8::SurfaceDesc desc{};
    call(back, slot::SurfaceGetDesc, &desc);
    void* copy = nullptr;
    HRESULT result = call(device, slot::CreateImageSurface, desc.width, desc.height, desc.format, &copy);
    if (SUCCEEDED(result))
        result = call(device, slot::CopyRects, back, static_cast<const RECT*>(nullptr), 0u, copy, static_cast<const POINT*>(nullptr));
    call<ULONG>(back, slot::Release);
    if (FAILED(result)) {
        if (copy != nullptr)
            call<ULONG>(copy, slot::Release);
        char text[64];
        sprintf_s(text, "the frame couldn't be copied (0x%08lX)", static_cast<unsigned long>(result));
        error = text;
        return false;
    }
    d3d8::LockedRect locked{};
    constexpr DWORD kReadOnly = 0x10; // D3DLOCK_READONLY
    if (FAILED(call(copy, slot::SurfaceLockRect, &locked, static_cast<const RECT*>(nullptr), kReadOnly))) {
        call<ULONG>(copy, slot::Release);
        error = "the frame couldn't be read";
        return false;
    }
    frame.width = desc.width;
    frame.height = desc.height;
    frame.stride = (desc.width * 3 + 3) & ~3u;
    frame.pixels.assign(static_cast<size_t>(frame.stride) * desc.height, 0);
    bool supported = true;
    for (UINT y = 0; y < desc.height && supported; ++y) {
        const BYTE* row = static_cast<const BYTE*>(locked.bits) + static_cast<size_t>(y) * locked.pitch;
        BYTE* out = frame.pixels.data() + static_cast<size_t>(y) * frame.stride;
        for (UINT x = 0; x < desc.width; ++x, out += 3) {
            switch (desc.format) {
            case d3d8::kFormatX8R8G8B8:
            case d3d8::kFormatA8R8G8B8:
                std::memcpy(out, row + x * 4, 3); // B, G, R
                break;
            case d3d8::kFormatR5G6B5: {
                const std::uint16_t v = reinterpret_cast<const std::uint16_t*>(row)[x];
                out[0] = static_cast<BYTE>((v & 31) * 255 / 31);
                out[1] = static_cast<BYTE>(((v >> 5) & 63) * 255 / 63);
                out[2] = static_cast<BYTE>(((v >> 11) & 31) * 255 / 31);
                break;
            }
            case d3d8::kFormatX1R5G5B5:
            case d3d8::kFormatA1R5G5B5: {
                const std::uint16_t v = reinterpret_cast<const std::uint16_t*>(row)[x];
                out[0] = static_cast<BYTE>((v & 31) * 255 / 31);
                out[1] = static_cast<BYTE>(((v >> 5) & 31) * 255 / 31);
                out[2] = static_cast<BYTE>(((v >> 10) & 31) * 255 / 31);
                break;
            }
            default:
                supported = false;
            }
        }
    }
    call(copy, slot::SurfaceUnlockRect);
    call<ULONG>(copy, slot::Release);
    if (!supported) {
        error = "the back buffer's format (" + std::to_string(desc.format) + ") isn't supported";
        return false;
    }
    return true;
}

// Writes `frame` to `path` as a PNG. Runs on its own thread.
bool encode(const Frame& frame, const std::wstring& path, std::string& error)
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* page = nullptr;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
        result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
        result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result))
        result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result))
        result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(result))
        result = encoder->CreateNewFrame(&page, nullptr);
    if (SUCCEEDED(result))
        result = page->Initialize(nullptr);
    if (SUCCEEDED(result))
        result = page->SetSize(frame.width, frame.height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(result))
        result = page->SetPixelFormat(&format);
    if (SUCCEEDED(result) && format != GUID_WICPixelFormat24bppBGR)
        result = E_FAIL; // the PNG encoder always takes 24-bit BGR
    if (SUCCEEDED(result))
        result = page->WritePixels(frame.height, frame.stride, static_cast<UINT>(frame.pixels.size()),
                                   const_cast<BYTE*>(frame.pixels.data()));
    if (SUCCEEDED(result))
        result = page->Commit();
    if (SUCCEEDED(result))
        result = encoder->Commit();
    if (page != nullptr)
        page->Release();
    if (encoder != nullptr)
        encoder->Release();
    if (stream != nullptr)
        stream->Release();
    if (factory != nullptr)
        factory->Release();
    if (com)
        CoUninitialize();
    if (FAILED(result)) {
        DeleteFileW(path.c_str());
        char text[64];
        sprintf_s(text, "the PNG couldn't be written (0x%08lX)", static_cast<unsigned long>(result));
        error = text;
        return false;
    }
    return true;
}

// The logged-in character's name, or "screenshot" (at the title screen, say).
std::wstring shot_name()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    if (game::party(members, alliance) && members[0].active && members[0].id != 0 && !members[0].name.empty()) {
        std::wstring name;
        for (const char c : members[0].name)
            name += (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ? static_cast<wchar_t>(c) : L'_';
        return name;
    }
    return L"screenshot";
}

// screenshots\<yyyy-mm-dd>\<name>_<yyyy.mm.dd>_<hhmmss>.png, made unique if two land in a second.
std::wstring shot_path(std::wstring& relative)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t day[16], stamp[32];
    swprintf_s(day, L"%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    swprintf_s(stamp, L"%04u.%02u.%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    CreateDirectoryW(g_folder.c_str(), nullptr);
    const std::wstring folder = g_folder + L"\\" + day;
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring base = shot_name() + L"_" + stamp;
    std::wstring name = base + L".png";
    for (int n = 2; GetFileAttributesW((folder + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES; ++n)
        name = base + L"_" + std::to_wstring(n) + L".png";
    relative = std::wstring(L"screenshots\\") + day + L"\\" + name;
    return folder + L"\\" + name;
}

// Puts `frame` on the clipboard as a bottom-up 24-bit DIB (what paint programs and chat apps paste).
// Runs on the encoding thread.
bool copy_to_clipboard(const Frame& frame)
{
    const size_t bytes = static_cast<size_t>(frame.stride) * frame.height;
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (memory == nullptr)
        return false;
    BYTE* data = static_cast<BYTE*>(GlobalLock(memory));
    if (data == nullptr) {
        GlobalFree(memory);
        return false;
    }
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(frame.width);
    header.biHeight = static_cast<LONG>(frame.height);
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(bytes);
    std::memcpy(data, &header, sizeof(header));
    for (UINT y = 0; y < frame.height; ++y)
        std::memcpy(data + sizeof(header) + static_cast<size_t>(frame.height - 1 - y) * frame.stride,
                    frame.pixels.data() + static_cast<size_t>(y) * frame.stride, frame.stride);
    GlobalUnlock(memory);
    // another program may hold the clipboard for a moment
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
        opened = OpenClipboard(nullptr) != FALSE;
        if (!opened)
            Sleep(20);
    }
    if (!opened) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_DIB, memory) != nullptr;
    if (!ok)
        GlobalFree(memory); // the clipboard owns it once set
    CloseClipboard();
    return ok;
}

std::string narrow(const std::wstring& text)
{
    std::string out;
    for (const wchar_t c : text)
        out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}

// VanaCore's own drawing, which renderer=d3d12 keeps in a layer of the window's size over the frame:
// laid over the frame at the frame's size (each pixel the average of the layer's it covers).
void add_ui_layer(Frame& frame)
{
    unsigned lw = 0, lh = 0;
    std::vector<unsigned char> layer;
    if (!graphics::ui_layer(lw, lh, layer))
        return;
    for (UINT y = 0; y < frame.height; ++y) {
        const unsigned y0 = y * lh / frame.height, y1 = std::max(y0 + 1, (y + 1) * lh / frame.height);
        BYTE* out = frame.pixels.data() + static_cast<size_t>(y) * frame.stride;
        for (UINT x = 0; x < frame.width; ++x, out += 3) {
            const unsigned x0 = x * lw / frame.width, x1 = std::max(x0 + 1, (x + 1) * lw / frame.width);
            unsigned sum[4] = {0, 0, 0, 0}, n = 0;
            for (unsigned ly = y0; ly < y1 && ly < lh; ++ly)
                for (unsigned lx = x0; lx < x1 && lx < lw; ++lx, ++n) {
                    const unsigned char* p = layer.data() + (static_cast<size_t>(ly) * lw + lx) * 4;
                    sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], sum[3] += p[3];
                }
            if (n == 0 || sum[3] == 0)
                continue;
            const unsigned a = sum[3] / n; // premultiplied: out = layer + frame * (1 - alpha)
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<BYTE>(std::min(255u, sum[c] / n + out[c] * (255 - a) / 255));
        }
    }
}

void take(void* device, bool with_ui)
{
    Frame frame;
    std::string error;
    if (!capture(device, frame, error)) {
        log::write(L"Screenshot: %hs", error.c_str());
        report("Screenshot failed: " + error);
        return;
    }
    if (with_ui)
        add_ui_layer(frame);
    std::wstring relative;
    const std::wstring path = shot_path(relative);
    // Encoding a PNG takes a while: off the game's thread.
    const bool clipboard = g_copy_to_clipboard;
    std::thread([frame = std::move(frame), path, relative, clipboard] {
        std::string problem;
        if (encode(frame, path, problem)) {
            log::write(L"Screenshot: %ls (%ux%u)", path.c_str(), frame.width, frame.height);
            std::string message = "Screenshot saved: " + narrow(relative);
            if (clipboard) {
                if (copy_to_clipboard(frame)) {
                    message += " (copied to the clipboard)";
                } else {
                    log::write(L"Screenshot: the clipboard couldn't be set");
                    message += " (couldn't copy it to the clipboard)";
                }
            }
            report(message);
        } else {
            log::write(L"Screenshot: %ls: %hs", path.c_str(), problem.c_str());
            report("Screenshot failed: " + problem);
        }
    }).detach();
}

// ---- settings --------------------------------------------------------------------------------

bool read_overlays(const wchar_t* ini)
{
    return ini == nullptr || ini[0] == L'\0' || GetPrivateProfileIntW(L"screenshots", L"overlays", 1, ini) != 0;
}

// [screenshots] clipboard=: on unless the file says 0
bool read_clipboard(const wchar_t* ini)
{
    return ini == nullptr || ini[0] == L'\0' || GetPrivateProfileIntW(L"screenshots", L"clipboard", 1, ini) != 0;
}

} // namespace

void install(const wchar_t* install_dir, const wchar_t* ini)
{
    g_folder = std::wstring(install_dir) + L"\\screenshots";
    g_default_overlays = read_overlays(ini);
    g_copy_to_clipboard = read_clipboard(ini);
    log::write(L"Screenshots: Print Screen takes them (a key bind, /bind printscreen /screenshot); copied to the clipboard: %d",
               g_copy_to_clipboard ? 1 : 0);
}

void open_folder()
{
    if (g_folder.empty())
        return;
    CreateDirectoryW(g_folder.c_str(), nullptr); // so it opens before the first shot
    const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", g_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    log::write(L"Screenshots: open %ls: %ls", g_folder.c_str(), ok ? L"ok" : L"failed");
    if (!ok)
        report("Screenshot folder couldn't be opened: " + narrow(g_folder));
}

void reload(const wchar_t* ini)
{
    if (ini != nullptr && ini[0] != L'\0') {
        g_default_overlays = read_overlays(ini);
        g_copy_to_clipboard = read_clipboard(ini);
    }
}

void request(int overlays)
{
    // One press can arrive twice (the key's message and the key's state): one shot per press.
    const ULONGLONG now = GetTickCount64();
    if (g_pending >= 0 || now - g_last_request < 300)
        return;
    g_last_request = now;
    g_pending = overlays < 0 ? (g_default_overlays ? 1 : 0) : (overlays != 0 ? 1 : 0);
}

namespace {

// capture()'s requests, taken in before_overlay (without VanaCore's drawing) or after_overlay (with it)
struct CodeCapture {
    bool overlays = false;
    Captured done;
};
std::mutex g_captures_lock;
std::vector<CodeCapture> g_captures;
std::atomic<bool> g_capture_waiting{false};

void run_captures(void* device, bool overlays)
{
    if (!g_capture_waiting)
        return;
    std::vector<CodeCapture> due;
    {
        const std::lock_guard lock(g_captures_lock);
        for (auto it = g_captures.begin(); it != g_captures.end();) {
            if (it->overlays == overlays) {
                due.push_back(std::move(*it));
                it = g_captures.erase(it);
            } else {
                ++it;
            }
        }
        g_capture_waiting = !g_captures.empty();
    }
    if (due.empty())
        return;
    Frame frame;
    std::string error;
    const bool ok = capture(device, frame, error);
    if (ok && overlays)
        add_ui_layer(frame);
    for (CodeCapture& each : due)
        each.done(ok, frame.width, frame.height, frame.stride, frame.pixels, error);
}

} // namespace

void capture(bool overlays, Captured done)
{
    const std::lock_guard lock(g_captures_lock);
    g_captures.push_back({overlays, std::move(done)});
    g_capture_waiting = true;
}

void before_overlay(void* device)
{
    run_captures(device, false);
    if (g_pending == 0) {
        g_pending = -1;
        take(device, false);
    }
}

void after_overlay(void* device)
{
    run_captures(device, true);
    if (g_pending == 1) {
        g_pending = -1;
        take(device, true);
    }
}

void on_frame()
{
    std::deque<std::string> results;
    {
        const std::lock_guard lock(g_lock);
        results.swap(g_results);
    }
    for (const std::string& result : results)
        chat::write(chat::kDefaultMode, chat::header() + result, "VanaCore");
}

} // namespace vanacore::screenshot
