#include "occlusion.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "log.hpp"

namespace vanacore::occlusion {
namespace {

// Why: The Sanctuary of Zi'Tah has no view lists, so the game draws every piece in view within the draw distance: at 8x from the
// outpost 882 to 1,758 pieces by the way the camera faces, some 6,000 draws, the frame CPU-bound in them (2026-11-11/12). Most are
// cedars behind the cedars in front: 31-64% of those in view stood behind the zone's collision alone from the camera (read live
// over vana.world.visible), more behind the canopies. A list baked offline hides only some 12% (it must hold everything seen from
// anywhere in its cell); a test from the camera each frame gets the camera's own share.
//
// The test is the renderer's (gfx_d3d12_occl.h): the boxes are sent once a zone, each frame's result comes back a frame or two
// later. A piece is left out once it has come back hidden in occl.frames tests in a row and drawn again the moment one says it's
// seen; a piece undrawn writes no depth and so hides nothing, so nothing is hidden by a piece that isn't drawn. What comes into
// sight as the camera moves shows when the test that saw it comes back: kept to pieces past occl.near, where little moves on
// the screen between frames, with occl.margin round each box for what does.

// The zone's pieces (PositionedMeshBlock, distance.cpp's): 0x16C bytes, the high mesh (none: nothing drawn), the 8 world corners
// of its box; the zone renderer's camera eye (as the game draws: x, height, y)
constexpr std::uintptr_t kChunkSize = 0x16C, kChunkMesh = 0x18, kChunkCorners = 0xF4, kCameraEye = 0x390F0;
constexpr ULONGLONG kLogEvery = 10000; // ms

Settings g_settings;
std::uintptr_t g_chunks = 0;    // the pieces whose boxes were sent
int g_count = 0;
unsigned g_gen = 0;             // ... the set's number at the renderer
std::vector<float> g_boxes;     // ... their boxes, 6 floats each (low x, height, y, then high; low x over high: none)
std::vector<std::uint8_t> g_run; // tests in a row each came back hidden
unsigned g_seq = 0;             // the last result taken up
unsigned g_tested = 0, g_hidden = 0; // ... its pieces on the screen, and of them hidden
std::uint64_t g_frames = 0, g_asked = 0, g_left = 0, g_results = 0; // since the last log line
ULONGLONG g_logged_at = 0;
std::vector<std::uint8_t> g_moves; // each piece: the times its box was found moved from the one sent (2 or more: never left out)
bool g_stale = false;           // a box was found moved: they're all read and sent again (at most once a second)
ULONGLONG g_sent_at = 0;
int g_resends = 0;              // this zone's (the first few logged)

// a piece's box from its corners as they are now (low x over high x: none)
void read_box(std::uintptr_t chunk, float* b)
{
    if (*reinterpret_cast<const std::uintptr_t*>(chunk + kChunkMesh) == 0) {
        b[0] = 1.0f, b[1] = b[2] = 0.0f, b[3] = 0.0f, b[4] = b[5] = 0.0f; // none
        return;
    }
    const auto* c = reinterpret_cast<const float*>(chunk + kChunkCorners);
    for (int k = 0; k < 3; ++k)
        b[k] = b[3 + k] = c[k];
    for (int v = 1; v < 8; ++v)
        for (int k = 0; k < 3; ++k) {
            b[k] = std::min(b[k], c[v * 3 + k]);
            b[3 + k] = std::max(b[3 + k], c[v * 3 + k]);
        }
}

// again: the same zone's boxes read afresh (some had moved since they were sent), its moving pieces kept
void send_boxes(std::uintptr_t chunks, int count, bool again = false)
{
    const std::size_t n = static_cast<std::size_t>(count);
    again = again && g_boxes.size() == n * 6 && g_moves.size() == n;
    if (!again)
        g_boxes.assign(n * 6, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        float* b = &g_boxes[i * 6];
        float now[6];
        read_box(chunks + i * kChunkSize, now);
        // (read again: each piece whose box moved since counted, twice making it one that keeps moving)
        if (again && g_moves[i] < 255 && !std::equal(now, now + 6, b, [](float x, float y) { return std::fabs(x - y) <= 0.01f; }))
            g_moves[i]++;
        std::copy(now, now + 6, b);
    }
    vanacore_gfx_occl_boxes(g_boxes.data(), static_cast<unsigned>(count));
    g_gen = vanacore_gfx_occl_gen();
    g_chunks = chunks, g_count = count;
    g_run.assign(static_cast<std::size_t>(count), 0);
    g_seq = 0;
    g_sent_at = GetTickCount64(), g_stale = false;
    if (!again) {
        g_moves.assign(static_cast<std::size_t>(count), 0);
        g_resends = 0;
        log::write(L"Graphics: occlusion test: %d pieces of this zone sent to be tested against what's drawn ([graphics] occl)",
                   count);
    } else if (++g_resends <= 5) {
        log::write(L"Graphics: occlusion test: this zone's boxes read again, some had moved since (%d pieces that keep moving "
                   L"never left out) ([graphics] occl)",
                   static_cast<int>(std::count_if(g_moves.begin(), g_moves.end(), [](std::uint8_t m) { return m >= 2; })));
    }
}

// the newest result, once: each piece's run of tests it came back hidden in
void take_result()
{
    unsigned gen = 0, seq = 0, tested = 0, hidden = 0;
    const unsigned char* r = vanacore_gfx_occl_result(&gen, &seq, &tested, &hidden);
    if (r == nullptr || seq == g_seq || gen != g_gen || g_run.size() != static_cast<std::size_t>(g_count))
        return;
    g_seq = seq;
    for (int i = 0; i < g_count; ++i) {
        std::uint8_t& run = g_run[static_cast<std::size_t>(i)];
        run = r[i] > 192 ? static_cast<std::uint8_t>(run < 255 ? run + 1 : 255) : 0;
    }
    g_tested = tested, g_hidden = hidden;
    g_results++;
}

// the box's nearest point to the eye, squared
float near_sq(const float* b, const float* eye)
{
    float sq = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = eye[k] < b[k] ? b[k] - eye[k] : eye[k] > b[3 + k] ? eye[k] - b[3 + k] : 0.0f;
        sq += d * d;
    }
    return sq;
}

void log_line()
{
    const ULONGLONG now = GetTickCount64();
    if (g_logged_at == 0) {
        g_logged_at = now;
        return;
    }
    if (now - g_logged_at < kLogEvery)
        return;
    if (g_frames > 0 && g_count > 0)
        log::write(L"Graphics: occlusion test: of %u pieces on the screen, %u hidden (the last test); %llu of %llu pieces a frame "
                   L"asked about %ls past %.0f (hidden in %d tests in a row), %llu results in %.0f s ([graphics] occl%ls)",
                   g_tested, g_hidden, g_left / g_frames, g_asked / g_frames, g_settings.mode == 1 ? L"left out" : L"would be left out",
                   g_settings.nearest, g_settings.frames, g_results, (now - g_logged_at) / 1000.0,
                   g_settings.mode == 1 ? L"" : L"=2, tested only");
    g_logged_at = now;
    g_frames = g_asked = g_left = g_results = 0;
}

} // namespace

void apply(const Settings& settings)
{
    const bool was = g_settings.mode != 0, on = settings.mode != 0;
    g_settings = settings;
    g_settings.frames = std::clamp(g_settings.frames, 1, 60);
    vanacore_gfx_occl_set(on ? 1 : 0, g_settings.margin);
    if (on != was) {
        std::fill(g_run.begin(), g_run.end(), std::uint8_t{0});
        g_logged_at = 0, g_frames = g_asked = g_left = g_results = 0;
        log::write(on ? L"Graphics: occlusion test on: the zone's pieces far off and hidden behind what's drawn %ls ([graphics] occl)"
                      : L"Graphics: occlusion test off ([graphics] occl)",
                   settings.mode == 1 ? L"are left undrawn" : L"are counted, not left out");
    }
    if (on) {
        g_frames++;
        log_line();
    }
}

bool hidden(std::uintptr_t renderer, std::uintptr_t chunks, int count, std::size_t i)
{
    if (g_settings.mode == 0 || count <= 0)
        return false;
    if (chunks != g_chunks || count != g_count || vanacore_gfx_occl_gen() != g_gen)
        send_boxes(chunks, count);
    else if (g_stale && GetTickCount64() - g_sent_at >= 1000)
        send_boxes(chunks, count, true);
    take_result();
    g_asked++;
    if (i >= g_run.size() || g_run[i] < g_settings.frames)
        return false;
    // Why: the boxes are read once, as the zone's pieces come in, and the game was still filling their corners then: in Lower
    // Jeuno (2026-11-17) piece 20's box was a sliver at x 98-101 where its corners ran 25-101, others had their heights the
    // wrong way up. A box behind its own piece comes back hidden by the piece's own depth: left out, then seen with it gone,
    // drawn, hidden again - the stairs and the tunnel past the arch flickering in and out with the camera still. So a piece
    // is left out only while its box is still its corners'. One that has moved has them all read and sent again, at most once a
    // second (the tests start over); one found moved twice keeps moving (a door, a ship) and is never left out this zone.
    if (i < g_moves.size() && g_moves[i] >= 2)
        return false;
    float now[6];
    read_box(chunks + i * kChunkSize, now);
    const float* was = &g_boxes[i * 6];
    for (int k = 0; k < 6; ++k)
        if (std::fabs(now[k] - was[k]) > 0.01f) {
            g_stale = true;
            return false;
        }
    const auto* eye = reinterpret_cast<const float*>(renderer + kCameraEye);
    if (near_sq(&g_boxes[i * 6], eye) < g_settings.nearest * g_settings.nearest)
        return false;
    g_left++;
    return g_settings.mode == 1;
}

} // namespace vanacore::occlusion
