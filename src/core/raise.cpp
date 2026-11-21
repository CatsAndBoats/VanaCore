#include "raise.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "d3d8.hpp"
#include "graphics.hpp"
#include "gui.hpp"
#include "log.hpp"
#include "uispace.hpp"

namespace vanacore::raise {
namespace {

using d3d8::call;
namespace slot = d3d8::slot;

// The windows added this frame (interface units), and how many of them are copied: the ones after that were drawn since
// the last take (in a later scene) and wait for the next
short g_rects[kMost][4] = {};
int g_count = 0;
int g_taken = 0;

// Each one copied: by the renderer (all of them laid back at once), or, with Windows' Direct3D 8 or 9, into a render
// target of its own (IDirect3DTexture8 and its surface). Those are kept from frame to frame, grown when a bigger window
// comes, and let go before the device's Reset (release: a render target in the default pool stops it), when the device is
// lost, and once one hasn't been used for kIdleFrames
struct Copy {
    void* texture = nullptr;
    void* surface = nullptr;
    UINT width = 0, height = 0; // the texture's
    DWORD format = 0;
    unsigned idle = 0;          // frames since it was last used
    RECT rect = {};             // this frame's copy in it
    POINT at = {};              // where it came from in the back buffer
};
constexpr unsigned kIdleFrames = 600; // ten seconds at 60 FPS
Copy g_copies[kMost];
int g_copied = 0;
bool g_by_renderer = false;
bool g_copy_failed = false;

// Where this frame's are on the back buffer (its pixels, ImGui's), for the mouse at the put
std::vector<RECT> g_covered;

// This frame's copied (interface units), and those the last put laid back (laid)
short g_taken_rects[kMost * 2][4] = {};
int g_taken_count = 0;
short g_laid[kMost * 2][4] = {};
int g_laid_count = 0;

// The addons that asked for the game's windows over theirs (vana.ui.game_on_top)
std::vector<std::string> g_owners;

bool inside(const short* a, const short* b)
{
    return a[0] >= b[0] && a[1] >= b[1] && a[2] <= b[2] && a[3] <= b[3];
}

void set(short* to, const short* from)
{
    for (int i = 0; i < 4; ++i)
        to[i] = from[i];
}

void release(Copy& copy)
{
    if (copy.surface != nullptr)
        call<ULONG>(copy.surface, slot::Release);
    if (copy.texture != nullptr)
        call<ULONG>(copy.texture, slot::Release);
    copy.surface = copy.texture = nullptr;
    copy.width = copy.height = 0, copy.format = 0, copy.idle = 0;
}

// The back buffer's (left, top)-(right, bottom) into a render target of its own (the one kept, if it's big enough): false
// if it couldn't be made or copied
bool copy_out(void* device, void* back, const d3d8::SurfaceDesc& desc, const RECT& from, Copy& copy)
{
    const UINT w = static_cast<UINT>(from.right - from.left), h = static_cast<UINT>(from.bottom - from.top);
    copy.rect = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    copy.at = {from.left, from.top};
    copy.idle = 0;
    const POINT to = {0, 0};
    HRESULT result = S_OK;
    if (copy.texture == nullptr || copy.width < w || copy.height < h || copy.format != desc.format) {
        const UINT tw = w > copy.width ? w : copy.width, th = h > copy.height ? h : copy.height;
        release(copy);
        result = call(device, slot::CreateTexture, tw, th, 1u, d3d8::kUsageRenderTarget, desc.format, d3d8::kPoolDefault,
                      &copy.texture);
        if (SUCCEEDED(result))
            result = call(copy.texture, slot::TextureGetSurfaceLevel, 0u, &copy.surface);
        if (SUCCEEDED(result))
            copy.width = tw, copy.height = th, copy.format = desc.format;
    }
    if (SUCCEEDED(result))
        result = call(device, slot::CopyRects, back, &from, 1u, copy.surface, &to);
    if (SUCCEEDED(result))
        return true;
    release(copy);
    g_copy_failed = true; // not tried again
    log::write(L"Raise: a game window couldn't be copied (0x%08lX): addons can draw over the auto-translate list and the "
               L"windows over fancychat's chat",
               static_cast<unsigned long>(result));
    return false;
}

} // namespace

void game_on_top(const std::string& owner, bool on)
{
    const auto found = std::find(g_owners.begin(), g_owners.end(), owner);
    if (on && found == g_owners.end()) {
        g_owners.push_back(owner);
        log::write(L"Raise: %hs asks for the game's windows over its own (vana.ui.game_on_top)", owner.c_str());
    } else if (!on && found != g_owners.end()) {
        g_owners.erase(found);
        log::write(L"Raise: %hs no longer asks for the game's windows over its own", owner.c_str());
    }
}

const std::vector<std::string>& game_on_top_owners()
{
    return g_owners;
}

void add(const short rect[4])
{
    if (rect[2] <= rect[0] || rect[3] <= rect[1])
        return;
    // among those not copied yet (one copied holds an earlier scene's pixels)
    for (int i = g_taken; i < g_count; ++i) {
        if (inside(rect, g_rects[i]))
            return;
        if (inside(g_rects[i], rect)) {
            set(g_rects[i], rect);
            return;
        }
    }
    if (g_count < kMost) {
        set(g_rects[g_count++], rect);
        return;
    }
    // no room: the last one not copied grows to take it in
    if (g_taken < g_count) {
        short* last = g_rects[g_count - 1];
        last[0] = rect[0] < last[0] ? rect[0] : last[0];
        last[1] = rect[1] < last[1] ? rect[1] : last[1];
        last[2] = rect[2] > last[2] ? rect[2] : last[2];
        last[3] = rect[3] > last[3] ? rect[3] : last[3];
    }
}

bool pending()
{
    return g_taken < g_count;
}

void take(void* device)
{
    if (!pending())
        return;
    void* back = nullptr;
    if (FAILED(call(device, slot::GetBackBuffer, 0u, d3d8::kBackBufferMono, &back)) || back == nullptr)
        return;
    d3d8::SurfaceDesc desc{};
    call(back, slot::SurfaceGetDesc, &desc);
    for (; g_taken < g_count; ++g_taken) {
        const short* rect = g_rects[g_taken];
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0, scale = 0;
        if (!uispace::to_pixels(rect[0], rect[1], desc.width, desc.height, x0, y0, scale) ||
            !uispace::to_pixels(rect[2], rect[3], desc.width, desc.height, x1, y1, scale))
            continue;
        const LONG width = static_cast<LONG>(desc.width), height = static_cast<LONG>(desc.height);
        const LONG left = static_cast<LONG>(std::floor(x0)) > 0 ? static_cast<LONG>(std::floor(x0)) : 0;
        const LONG top = static_cast<LONG>(std::floor(y0)) > 0 ? static_cast<LONG>(std::floor(y0)) : 0;
        const LONG right = static_cast<LONG>(std::ceil(x1)) < width ? static_cast<LONG>(std::ceil(x1)) : width;
        const LONG bottom = static_cast<LONG>(std::ceil(y1)) < height ? static_cast<LONG>(std::ceil(y1)) : height;
        if (right <= left || bottom <= top)
            continue;
        const RECT place = {left, top, right, bottom};
        if (graphics::raise_take(left, top, right - left, bottom - top)) {
            g_by_renderer = true;
        } else {
            if (g_copy_failed || g_copied >= kMost || !copy_out(device, back, desc, place, g_copies[g_copied]))
                continue;
            ++g_copied;
        }
        g_covered.push_back(place);
        if (g_taken_count < kMost * 2)
            set(g_taken_rects[g_taken_count++], rect);
    }
    call<ULONG>(back, slot::Release);
}

void put(void* device)
{
    if (g_by_renderer)
        graphics::raise_put();
    bool kept = false;
    for (const Copy& copy : g_copies)
        kept = kept || copy.texture != nullptr;
    if (kept && call(device, slot::TestCooperativeLevel) != S_OK) {
        release(); // the device lost: its Reset is coming
    } else if (g_copied > 0) {
        void* back = nullptr;
        if (SUCCEEDED(call(device, slot::GetBackBuffer, 0u, d3d8::kBackBufferMono, &back)) && back != nullptr) {
            for (int i = 0; i < g_copied; ++i)
                call(device, slot::CopyRects, g_copies[i].surface, &g_copies[i].rect, 1u, back, &g_copies[i].at);
            call<ULONG>(back, slot::Release);
        }
    }
    // the copies not used this frame age, and go once they've been idle a while
    for (int i = g_copied; i < kMost; ++i)
        if (g_copies[i].texture != nullptr && ++g_copies[i].idle > kIdleFrames)
            release(g_copies[i]);
    g_copied = 0;
    g_by_renderer = false;
    g_count = g_taken = 0;
    gui::set_covered(g_covered);
    g_covered.clear();
    std::memcpy(g_laid, g_taken_rects, sizeof g_laid);
    g_laid_count = g_taken_count;
    g_taken_count = 0;
}

int laid(short (*rects)[4], int most)
{
    const int n = std::min(most, g_laid_count);
    for (int i = 0; i < n; ++i)
        set(rects[i], g_laid[i]);
    return n;
}

void release()
{
    for (Copy& copy : g_copies)
        release(copy);
    g_copied = 0;
}

} // namespace vanacore::raise
