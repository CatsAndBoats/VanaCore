#include "gmicons.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "client.hpp"
#include "d3d8.hpp"
#include "image.hpp"
#include "log.hpp"
#include "nameplates.hpp"
#include "text.hpp"
#include "uispace.hpp"

namespace vanacore::gmicons {
namespace {

// The game's GM icon before an entity's name, as its status icon picker (FFXiMain.dll+0x97310) has it: the GM level
// (+0x128 >> 11 & 7; none while +0x12C has 0x40000) and that level's glyph (the table at FFXiMain.dll+0x35962C in
// CatsEyeXI's client: 1-2 0x93, 3-7 0x95 to 0x99; each a 15x15 cell of the icons' sheet, as the linkshell's pearl is).
// 0: not in GM mode. The glyph alone doesn't say: 0x93 is a mentor's icon too (+0x130 has 0x4000, GM level 0).
constexpr std::ptrdiff_t kEntityGmLevel = 0x128, kEntityGmOff = 0x12C;
constexpr std::uint32_t kGmOff = 0x40000;
constexpr unsigned kGmGlyphs[8] = {0, 0x93, 0x93, 0x95, 0x96, 0x97, 0x98, 0x99};
unsigned gm_glyph(const void* entity)
{
    if (entity == nullptr)
        return 0;
    std::uint32_t level = 0, off = 0;
    std::memcpy(&level, static_cast<const char*>(entity) + kEntityGmLevel, sizeof level);
    std::memcpy(&off, static_cast<const char*>(entity) + kEntityGmOff, sizeof off);
    return (off & kGmOff) != 0 ? 0 : kGmGlyphs[level >> 11 & 7];
}

enum Icon { kDev, kAdmin, kSgm, kGm, kIcons };
struct Kind {
    const char* word;     // its PNG's name
    const char* resource; // its PNG's, in VanaCore.Core.rc
};
constexpr Kind kKinds[kIcons] = {{"dev", "GM_DEV"}, {"admin", "GM_ADMIN"}, {"sgm", "GM_SGM"}, {"gm", "GM_GM"}};

#include "gmicons_staff.h" // GM_LIST: each server's staff
// Each picture at its own size and halved, and halved again: the one drawn is the smallest at least as high as the icon on
// the screen, so it's never shrunk by more than half (smoothed only by its four nearest texels, more would shimmer)
constexpr int kSizes = 4;
constexpr std::size_t kLongestName = 32;

struct Picture {
    text::Sprite sizes[kSizes];
    int count = 0;
};
Picture g_pictures[kIcons];
std::unordered_map<std::string, int> g_staff; // lower-case names: their Icon
bool g_loaded = false;
bool g_listed = false;    // the server is in GM_LIST
bool g_enabled = true;    // nameplates.gm
int g_pick = -1;          // the Icon of the name being drawn (-1: the game's)
unsigned g_glyph = 0;     // ... its GM icon's glyph
unsigned g_code = 0;      // the glyph the next quad is of
void* g_device = nullptr; // the device the pictures' textures are on
std::unordered_set<std::string> g_said; // GM names said in the log
constexpr std::size_t kSayNames = 64;

// The game's letter vertices (FVF 0x144: position, rhw, colour, one set of texture coordinates)
struct Vertex {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
};

HMODULE self()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &module);
    return module;
}

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// This server's staff (client::server()) from GM_LIST into g_staff; false when it isn't listed
bool read_staff()
{
    const std::string& host = client::server();
    const auto server =
        std::find_if(std::begin(kGmList), std::end(kGmList), [&](const ServerStaff& s) { return lower(s.host) == host; });
    if (host.empty() || server == std::end(kGmList))
        return false;
    for (std::size_t i = 0; i < server->count; ++i)
        g_staff[lower(server->staff[i].name)] = server->staff[i].icon;
    return true;
}

// `name`'s GM icon said in the log, the first time it's drawn (kSayNames names at most)
void say(const std::string& name, unsigned glyph, int icon)
{
    if (g_said.size() >= kSayNames || !g_said.insert(name).second)
        return;
    if (icon >= 0)
        log::write(L"Graphics: %hs's GM icon (0x%02X) is %hs.png ([graphics] nameplates.gm)", name.c_str(), glyph,
                   kKinds[icon].word);
    else
        log::write(L"Graphics: %hs's GM icon (0x%02X) stays the game's ([graphics] nameplates.gm)", name.c_str(), glyph);
}

// Half the picture's size, each pixel the four under it averaged by their alpha (so see-through texels' colours don't
// darken the edges)
image::Pixels halved(const image::Pixels& in)
{
    image::Pixels out;
    out.width = std::max(1, in.width / 2);
    out.height = std::max(1, in.height / 2);
    out.argb.resize(static_cast<std::size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            unsigned sum[4] = {};
            for (int k = 0; k < 4; ++k) {
                const int sx = std::min(in.width - 1, x * 2 + (k & 1)), sy = std::min(in.height - 1, y * 2 + k / 2);
                const std::uint32_t p = in.argb[static_cast<std::size_t>(sy) * in.width + sx];
                const unsigned a = p >> 24;
                sum[0] += a;
                for (int c = 1; c < 4; ++c)
                    sum[c] += (p >> (24 - 8 * c) & 0xFF) * a;
            }
            std::uint32_t pixel = (sum[0] + 2) / 4 << 24;
            for (int c = 1; c < 4 && sum[0] > 0; ++c)
                pixel |= std::min(255u, (sum[c] + sum[0] / 2) / sum[0]) << (24 - 8 * c);
            out.argb[static_cast<std::size_t>(y) * out.width + x] = pixel;
        }
    return out;
}

// This server's staff and the pictures, from VanaCore.Core.dll, the first time a GM's name is drawn (none for a server
// GM_LIST hasn't). Decoded on a thread of its own, so the game thread's COM stays as the game set it.
void load()
{
    if (g_loaded)
        return;
    g_loaded = true;
    g_listed = read_staff();
    if (!g_listed) {
        log::write(L"Graphics: GM icons stay the game's: the server (%hs) has no staff list in VanaCore (gmicons_staff.h) "
                   L"([graphics] nameplates.gm)",
                   client::server().empty() ? "none given" : client::server().c_str());
        return;
    }
    const HMODULE module = self();
    image::Pixels pixels[kIcons];
    std::thread([&] {
        for (int i = 0; i < kIcons; ++i)
            image::decode_resource(module, kKinds[i].resource, pixels[i]);
    }).join();
    int loaded = 0;
    for (int i = 0; i < kIcons; ++i) {
        image::Pixels size = std::move(pixels[i]);
        Picture& picture = g_pictures[i];
        while (picture.count < kSizes && !size.argb.empty()) {
            image::Pixels next = size.width > 1 && size.height > 1 ? halved(size) : image::Pixels{};
            picture.sizes[picture.count++].set_image(std::move(size.argb), size.width, size.height);
            size = std::move(next);
        }
        if (picture.count > 0)
            ++loaded;
        else
            log::write(L"Graphics: couldn't read %hs.png from VanaCore.Core.dll; that GM icon stays the game's "
                       L"([graphics] nameplates.gm)",
                       kKinds[i].word);
    }
    log::write(L"Graphics: GM icons before staff names: %d of %d pictures, %u names listed for %hs ([graphics] nameplates.gm)",
               loaded, static_cast<int>(kIcons), static_cast<unsigned>(g_staff.size()), client::server().c_str());
}

// The picture of `icon` for an icon `pixels` high on the screen, its texture on `device`; nullptr if there's none
text::Sprite* sized(int icon, float pixels, void* device)
{
    if (device != g_device) {
        for (Picture& picture : g_pictures)
            for (text::Sprite& sprite : picture.sizes)
                sprite.release();
        g_device = device;
    }
    Picture& picture = g_pictures[icon];
    int index = 0;
    while (index + 1 < picture.count && static_cast<float>(picture.sizes[index + 1].height()) >= pixels)
        ++index;
    text::Sprite* const sprite = picture.count > 0 ? &picture.sizes[index] : nullptr;
    if (sprite == nullptr || !sprite->upload(device) || sprite->texture() == nullptr || sprite->texture_width() == 0 ||
        sprite->texture_height() == 0)
        return nullptr;
    return sprite;
}

// The vertices' colour: white, so the picture comes out as it is, and as opaque as the game would show a solid texel of
// its own on this quad. Its alpha op multiplies the vertex's alpha up (MODULATE4X for names: an icon's quad has 0x80, the
// letters' 0xC0, both shown whole), so the picture fades only where the game's icon would.
DWORD shown(void* device, DWORD letter)
{
    DWORD op = 0;
    d3d8::call(device, d3d8::slot::GetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, &op);
    if (op == d3d8::kTopSelectArg1 || op == d3d8::kTopDisable) // the texture's alpha alone, or none
        return 0xFFFFFFFFu;
    const float times = op == d3d8::kTopModulate4x ? 4.0f : op == d3d8::kTopModulate2x ? 2.0f : 1.0f;
    const float alpha = std::min(255.0f, static_cast<float>(letter >> 24) * times);
    return static_cast<DWORD>(std::lround(alpha)) << 24 | 0xFFFFFFu;
}

// `quad` drawn by the game's `draw` with `sprite`'s texture times its vertices' colour (texture stage ops of its own: the
// game's for its letters multiply colour and alpha up for its font), smoothly scaled; the device's state put back after
void draw_picture(void* renderer, DrawFn draw, void* device, const text::Sprite& sprite, const Vertex (&quad)[4])
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    // the game's alpha test (0x60 for some names) would cut the picture's smoothed edges: only see-through texels left out
    DWORD alpha_ref = 0;
    call(device, slot::GetRenderState, d3d8::rs::AlphaRef, &alpha_ref);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, DWORD{1});
    namespace tss = d3d8::tss;
    struct Saved {
        DWORD stage, type, wanted, value;
    };
    Saved saved[] = {{0, tss::ColorOp, d3d8::kTopModulate, 0},   {0, tss::ColorArg1, d3d8::kTaTexture, 0},
                     {0, tss::ColorArg2, d3d8::kTaDiffuse, 0},   {0, tss::AlphaOp, d3d8::kTopModulate, 0},
                     {0, tss::AlphaArg1, d3d8::kTaTexture, 0},   {0, tss::AlphaArg2, d3d8::kTaDiffuse, 0},
                     {0, tss::MagFilter, d3d8::kFilterLinear, 0}, {0, tss::MinFilter, d3d8::kFilterLinear, 0},
                     {0, tss::MipFilter, d3d8::kFilterNone, 0},  {0, tss::AddressU, d3d8::kAddressClamp, 0},
                     {0, tss::AddressV, d3d8::kAddressClamp, 0}, {1, tss::ColorOp, d3d8::kTopDisable, 0},
                     {1, tss::AlphaOp, d3d8::kTopDisable, 0}};
    for (Saved& s : saved) {
        call(device, slot::GetTextureStageState, s.stage, s.type, &s.value);
        call(device, slot::SetTextureStageState, s.stage, s.type, s.wanted);
    }
    void* texture = nullptr;
    call(device, slot::GetTexture, DWORD{0}, &texture);
    call(device, slot::SetTexture, DWORD{0}, sprite.texture());
    draw(renderer, nullptr, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad), static_cast<unsigned>(sizeof(Vertex)));
    call(device, slot::SetTexture, DWORD{0}, texture);
    if (texture != nullptr)
        call<ULONG>(texture, slot::Release);
    for (const Saved& s : saved)
        call(device, slot::SetTextureStageState, s.stage, s.type, s.value);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, alpha_ref);
}

// A name's letters alone (`text` past its icons), kLongestName at most
std::string name_of(const char* text)
{
    std::string name;
    for (std::size_t at = 0; text[at] != '\0' && name.size() < kLongestName; ++at)
        name += text[at];
    return name;
}

// The Icon for a GM named `name`, this server's staff and the pictures loaded first: GM_LIST's, else gm; -1 with no
// picture, or on a server GM_LIST hasn't
int pick(const std::string& name)
{
    load();
    if (!g_listed)
        return -1;
    const auto found = g_staff.find(lower(name));
    const int icon = found != g_staff.end() ? found->second : kGm;
    return g_pictures[icon].count > 0 ? icon : g_pictures[kGm].count > 0 ? kGm : -1;
}

// The Target window's mark: the status codes' shapes (the table starts at code 0x20), the entity's name (+0x7C, 16
// characters at most), and the mark's cell where the game draws it (x 0..16, y -2..14 of its place, in the interface's units)
constexpr unsigned kFirstShape = 0x20;
constexpr std::ptrdiff_t kEntityName = 0x7C;
constexpr std::size_t kEntityNameLength = 16;
constexpr float kMarkSize = 16.0f, kMarkTop = -2.0f;
struct Mark {
    bool set = false;
    int icon = -1;
    float x = 0.0f, y = 0.0f; // its cell's top left
};
Mark g_mark;

} // namespace

Name::Name(const char* text, const void* entity)
{
    g_pick = -1;
    g_glyph = 0;
    g_code = 0;
    const unsigned glyph = text != nullptr ? gm_glyph(entity) : 0;
    if (glyph == 0)
        return;
    // its GM icon among the icons before it (another status of the game's can take the icon's place)
    std::size_t at = 0;
    bool shown = false;
    for (; static_cast<unsigned char>(text[at]) >= 0x80; ++at)
        shown = shown || static_cast<unsigned char>(text[at]) == glyph;
    const std::string name = name_of(text + at + (text[at] == ' ' ? 1 : 0));
    if (!shown) {
        if (g_said.size() < kSayNames && g_said.insert(name).second)
            log::write(L"Graphics: %hs is in GM mode (its icon 0x%02X) but the game shows icon 0x%02X before the name: "
                       L"left as it is ([graphics] nameplates.gm)",
                       name.c_str(), glyph, static_cast<unsigned char>(text[0]));
        return;
    }
    g_pick = pick(name);
    g_glyph = glyph;
    if (g_listed)
        say(name, glyph, g_pick);
}

void enable(bool on)
{
    g_enabled = on;
}

bool enabled()
{
    return g_enabled;
}

bool target_mark(const void* entity, const void* shape, const void* const* shapes, int x, int y)
{
    const unsigned glyph = g_enabled ? gm_glyph(entity) : 0;
    if (glyph == 0 || shape == nullptr || shapes == nullptr || shapes[glyph - kFirstShape] != shape)
        return false;
    char name[kEntityNameLength + 1] = {};
    std::memcpy(name, static_cast<const char*>(entity) + kEntityName, kEntityNameLength);
    const int icon = pick(name_of(name));
    if (icon < 0)
        return false;
    g_mark = {true, icon, static_cast<float>(x), static_cast<float>(y) + kMarkTop};
    return true;
}

void draw_mark(void* device, unsigned width, unsigned height)
{
    const Mark mark = g_mark;
    g_mark.set = false;
    float left = 0, top = 0, right = 0, bottom = 0, scale = 0;
    if (!mark.set || !uispace::to_pixels(mark.x, mark.y, width, height, left, top, scale) ||
        !uispace::to_pixels(mark.x + kMarkSize, mark.y + kMarkSize, width, height, right, bottom, scale))
        return;
    // square, as high as the cell, in its middle across (the cell is squeezed with [ui] aspect)
    const float tall = bottom - top;
    const text::Sprite* const sprite = tall > 1.0f ? sized(mark.icon, tall, device) : nullptr;
    if (sprite == nullptr)
        return;
    const float times = tall / static_cast<float>(sprite->height());
    const float wide = static_cast<float>(sprite->width()) * times;
    sprite->draw(device, static_cast<int>(std::lround((left + right - wide) * 0.5f)), static_cast<int>(std::lround(top)), 1.0f,
                 times);
}

Name::~Name()
{
    g_pick = -1;
    g_glyph = 0;
    g_code = 0;
}

void note(unsigned code)
{
    g_code = code & 0xFF;
}

bool draw(void* renderer, DrawFn draw, unsigned prim, unsigned count, const void* vertices, unsigned stride, const Look& look)
{
    if (g_pick < 0 || g_code != g_glyph || draw == nullptr || prim != d3d8::kTriangleStrip || count != 2 ||
        stride != sizeof(Vertex) || vertices == nullptr)
        return false;
    nameplates::Pass pass;
    if (!nameplates::pass(pass))
        return false;
    const Vertex* const letter = static_cast<const Vertex*>(vertices);
    float left = letter[0].x, top = letter[0].y;
    float right = left, bottom = top;
    for (int i = 1; i < 4; ++i) {
        left = std::min(left, letter[i].x), right = std::max(right, letter[i].x);
        top = std::min(top, letter[i].y), bottom = std::max(bottom, letter[i].y);
    }
    const float height = (bottom - top) * look.size;
    if (!(height > 0.5f))
        return false;
    const text::Sprite* const sprite = sized(g_pick, height * pass.ky, pass.device);
    if (sprite == nullptr)
        return false;
    // as wide on the screen as it is high (the game's icons are narrowed for a 16:9 stretch; these pictures are square),
    // its right side where the game's is, by the space before the name, so it grows away from the name; then moved
    const float width = height * pass.ky / pass.kx, middle = (top + bottom) * 0.5f + look.y;
    const float x1 = right + look.x, x0 = x1 - width;
    const float y0 = middle - height * 0.5f, y1 = middle + height * 0.5f;
    const float u = static_cast<float>(sprite->width()) / static_cast<float>(sprite->texture_width());
    const float v = static_cast<float>(sprite->height()) / static_cast<float>(sprite->texture_height());
    const DWORD colour = shown(pass.device, letter[0].colour);
    const Vertex quad[4] = {{x0, y0, letter[0].z, letter[0].rhw, colour, 0.0f, 0.0f},
                            {x1, y0, letter[1].z, letter[1].rhw, colour, u, 0.0f},
                            {x0, y1, letter[2].z, letter[2].rhw, colour, 0.0f, v},
                            {x1, y1, letter[3].z, letter[3].rhw, colour, u, v}};
    draw_picture(renderer, draw, pass.device, *sprite, quad);
    return true;
}

} // namespace vanacore::gmicons
