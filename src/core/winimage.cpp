#include "winimage.hpp"

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "image.hpp"
#include "log.hpp"

namespace vanacore::winimage {
namespace {

constexpr int kLargest = 4096; // a picture larger than this either way is halved until it isn't (the windows never need more)

std::wstring g_install; // VanaCore's folder, ending in a backslash
std::wstring g_path;    // the picture shown now (its whole path), empty for none
int g_look[3] = {-1, -1, -1}; // fit, chat, input as last handed to the drawing

std::wstring read_string(const wchar_t* ini, const wchar_t* key, const wchar_t* fallback)
{
    wchar_t text[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"client", key, fallback, text, static_cast<DWORD>(std::size(text)), ini);
    return text;
}

// window.image's value as a whole path: from VanaCore's folder unless it's a whole one already (C:\..., \\server\...)
std::wstring whole_path(const std::wstring& value)
{
    if (value.empty())
        return value;
    const bool whole = (value.size() > 1 && value[1] == L':') || value[0] == L'\\' || value[0] == L'/';
    return whole ? value : g_install + value;
}

// Halved (each pixel the average of the 2x2 it covers) until it's kLargest or smaller both ways
void shrink(image::Pixels& p)
{
    while (p.width > kLargest || p.height > kLargest) {
        const int w = p.width > 1 ? p.width / 2 : 1, h = p.height > 1 ? p.height / 2 : 1;
        std::vector<std::uint32_t> out(static_cast<std::size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int x1 = 2 * x + 1 < p.width ? 2 * x + 1 : 2 * x, y1 = 2 * y + 1 < p.height ? 2 * y + 1 : 2 * y;
                const std::uint32_t q[4] = {p.argb[2 * y * p.width + 2 * x], p.argb[2 * y * p.width + x1],
                                            p.argb[y1 * p.width + 2 * x], p.argb[y1 * p.width + x1]};
                std::uint32_t v = 0;
                for (int c = 0; c < 32; c += 8)
                    v |= ((((q[0] >> c) & 255) + ((q[1] >> c) & 255) + ((q[2] >> c) & 255) + ((q[3] >> c) & 255) + 2) / 4) << c;
                out[static_cast<std::size_t>(y) * w + x] = v;
            }
        p.argb = std::move(out), p.width = w, p.height = h;
    }
}

// The picture at `path` (whole) handed to the drawing, or none (empty, or a file that doesn't decode)
void show(const std::wstring& path)
{
    g_path = path;
    image::Pixels pixels;
    if (path.empty() || !image::decode_file(path, pixels)) {
        if (!path.empty())
            log::write(L"Window image: %ls couldn't be read as a picture: the game's window backgrounds", path.c_str());
        vanacore_gfx_set_window_image(nullptr, 0, 0);
        return;
    }
    const int width = pixels.width, height = pixels.height;
    shrink(pixels);
    vanacore_gfx_set_window_image(pixels.argb.data(), static_cast<unsigned>(pixels.width), static_cast<unsigned>(pixels.height));
    log::write(L"Window image: %ls (%dx%d%ls) in place of the window backgrounds", path.c_str(), width, height,
               pixels.width != width ? L", drawn halved" : L"");
}

int fit_of(const std::wstring& value)
{
    const wchar_t* names[] = {L"fill", L"stretch", L"tile", L"screen"};
    for (int i = 0; i < 4; ++i)
        if (_wcsicmp(value.c_str(), names[i]) == 0)
            return i;
    return 0;
}

} // namespace

void reload(const wchar_t* ini)
{
    if (ini == nullptr)
        return;
    const std::wstring path = whole_path(read_string(ini, L"window.image", L""));
    if (_wcsicmp(path.c_str(), g_path.c_str()) != 0)
        show(path);
    // the chat windows take every window's look with chat.match=1; the chat line with input.match=windows, or chat (the chat's)
    const int chat = GetPrivateProfileIntW(L"client", L"chat.match", 0, ini) != 0 ? 1 : 0;
    const std::wstring input_match = read_string(ini, L"input.match", L"windows");
    const int input = _wcsicmp(input_match.c_str(), L"windows") == 0 || (_wcsicmp(input_match.c_str(), L"chat") == 0 && chat) ? 1 : 0;
    const int look[3] = {fit_of(read_string(ini, L"window.image.fit", L"fill")), chat, input};
    if (look[0] != g_look[0] || look[1] != g_look[1] || look[2] != g_look[2]) {
        g_look[0] = look[0], g_look[1] = look[1], g_look[2] = look[2];
        vanacore_gfx_set_window_image_look(look[0], look[1], look[2]);
    }
}

void install(const wchar_t* ini, const wchar_t* install_dir)
{
    g_install = install_dir != nullptr ? install_dir : L"";
    if (!g_install.empty() && g_install.back() != L'\\' && g_install.back() != L'/')
        g_install += L'\\';
    reload(ini);
}

bool add(const std::wstring& path, std::wstring& kept, std::wstring& why)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    image::Pixels pixels;
    if (g_install.empty() || name.empty()) {
        why = L"VanaCore's folder isn't known";
        return false;
    }
    if (!image::decode_file(path, pixels)) {
        why = L"it isn't a picture Windows can read (.png, .jpg, .bmp, .gif)";
        return false;
    }
    const std::wstring folder = g_install + L"images\\", whole = folder + name;
    CreateDirectoryW(folder.c_str(), nullptr);
    // (one of the folder's own picked: it's there already)
    if (_wcsicmp(whole.c_str(), path.c_str()) != 0 && !CopyFileW(path.c_str(), whole.c_str(), FALSE)) {
        why = L"it couldn't be copied into " + folder;
        return false;
    }
    kept = L"images\\" + name;
    if (_wcsicmp(whole.c_str(), g_path.c_str()) == 0) // set already: the setting won't change, so it's read again here
        show(whole);
    return true;
}

} // namespace vanacore::winimage
