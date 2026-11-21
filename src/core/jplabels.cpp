#include "jplabels.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "jplabels_data.h"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::jplabels {
namespace {

// The game's language, in its settings (statuswindow.cpp's, found there only for its own hooks): the getter of the language's
// own menu sheets (FUN_10259b10): mov ecx,[settings]; mov dl,[ecx+18h]; mov eax,[ecx+8]; test dl,dl; jns; cmp eax,2; jz;
// cmp eax,3; jnz; mov eax,1; push eax; call. +8 in the settings: 0 Japanese, 1 English, 2 French, 3 German.
constexpr char kLanguage[] = "8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0F 83 F8 02 74 05 83 F8 03 75 05 "
                             "B8 01 00 00 00 50 E8";
constexpr std::ptrdiff_t kSettingsAt = 2, kLanguageAt = 8;

// A shape (buttons.cpp's): its quads' list (+4: the first node, a node's next at +0, its quad at +10h) and their count (+14h).
// A quad (XIClient's UIShapeQuad): its picture's width and height (+8, +0Ah) and place (+0Ch, +0Eh) on its texture, texels,
// as the menu resource gives them; its blending's A (+2Eh: 2, taken away, the label's shadow copy); its texture's name (+32h,
// 16 letters); its corners in the shape's units, x at +C0h and y at +C8h (top left, top right, bottom left, bottom right).
constexpr std::ptrdiff_t kShapeList = 4, kShapeCount = 0x14, kNodeQuad = 0x10;
constexpr std::ptrdiff_t kSize = 0x08, kPlace = 0x0C, kBlendA = 0x2E, kName = 0x32, kOwnX = 0xC0, kOwnY = 0xC8;
constexpr int kTakenAway = 2;
constexpr int kQuadsMost = 64;  // a shape's quads looked at (buttons.cpp's)
constexpr int kDepthMost = 4;   // shapes drawn inside another's draw kept apart
constexpr int kGap = 4;         // a label's next part no more than this far right of the last (tools\jplabels's GAP)
constexpr int kGame = 1, kFirst = 2, kLater = 3; // vanacore_gfx_label_word's

struct Entry {
    const char* quad;
    int part;       // its picture in data::kParts, -1 none
    short x0, x1, y0, y1;
    bool shadow;
    int what;       // kGame, kFirst, kLater
    std::uint32_t code;
    float wide;
};
struct Frame {
    const void* shape = nullptr;
    Entry entries[kQuadsMost] = {};
    int count = 0;
};
Frame g_frames[kDepthMost];
int g_depth = 0;
bool g_on = false;
char* const* g_settings = nullptr;
bool g_looked = false;
std::unordered_map<std::uint64_t, int> g_parts; // a picture (sheet, u, v, w, h) to its data::kParts index
std::unordered_map<std::string, int> g_labels;  // a label's parts (their indices, 2 bytes each) to its data::kLabels index

template <typename T> T at(const void* base, std::ptrdiff_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
    return value;
}

std::uint64_t picture(int sheet, int u, int v, int w, int h)
{
    return static_cast<std::uint64_t>(sheet) << 48 | static_cast<std::uint64_t>(u & 0xFFF) << 36 |
           static_cast<std::uint64_t>(v & 0xFFF) << 24 | static_cast<std::uint64_t>(w & 0xFFF) << 12 | static_cast<std::uint64_t>(h & 0xFFF);
}

void add_parts(std::string& key, int part) { key.append(reinterpret_cast<const char*>(&part), 2); }

void build_tables()
{
    for (int i = 0; i < static_cast<int>(std::size(data::kParts)); ++i) {
        const data::Part& p = data::kParts[i];
        g_parts.emplace(picture(p.sheet, p.u, p.v, p.w, p.h), i);
    }
    for (int i = 0; i < static_cast<int>(std::size(data::kLabels)); ++i) {
        const data::Label& l = data::kLabels[i];
        std::string key;
        for (int k = 0; k < l.count; ++k)
            add_parts(key, data::kLabelParts[l.first + k]);
        g_labels.emplace(std::move(key), i);
    }
}

// The settings' global, once FFXiMain.dll is there; false while it isn't (looked for again)
bool find_settings()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_looked = true;
    const std::uintptr_t found = scan::find(game, kLanguage);
    if (found == 0) {
        log::write(L"True Font: the game's language wasn't found in FFXiMain.dll: its menus' labels in Japanese stay the game's");
        return true;
    }
    g_settings = at<char* const*>(reinterpret_cast<const void*>(found), kSettingsAt);
    build_tables();
    log::write(L"True Font: the Japanese menus' labels: %d of them known, of %d pictures", static_cast<int>(g_labels.size()),
               static_cast<int>(g_parts.size()));
    return true;
}

bool japanese()
{
    const char* settings = g_settings != nullptr ? *g_settings : nullptr;
    return settings != nullptr && at<int>(settings, kLanguageAt) == 0;
}

// The Japanese label sheet `quad` draws from (data::kSheets), -1 another
int sheet_of(const char* quad)
{
    const char* name = quad + kName;
    if (std::memcmp(name, "menu    m", 9) != 0)
        return -1;
    for (int s = 0; s < static_cast<int>(std::size(data::kSheets)); ++s)
        if (std::memcmp(name, data::kSheets[s], 16) == 0)
            return s;
    return -1;
}

// Entries first to past (one pass's, left to right) as the label they are, if it's known: its first part its words, the rest
// later parts; else each the game's
void look_up(Entry* const* group, int count)
{
    std::string key;
    for (int i = 0; i < count; ++i) {
        if (group[i]->part < 0)
            return; // (each kGame already)
        add_parts(key, group[i]->part);
    }
    const auto found = g_labels.find(key);
    if (found == g_labels.end())
        return; // (each kGame already)
    const Entry& first = *group[0];
    const int width = group[count - 1]->x1 - first.x0, own = first.x1 - first.x0;
    for (int i = 0; i < count; ++i)
        group[i]->what = i == 0 ? kFirst : kLater;
    group[0]->code = static_cast<std::uint32_t>(VANA_LABEL_WORDS + found->second);
    group[0]->wide = own > 0 && width > own ? static_cast<float>(width) / static_cast<float>(own) : 1.0f;
}

// A frame's entries of one pass put together as tools\jplabels does: left to right, a part on the last's row (their heights
// overlapping) starting no further than kGap right of its end (nor left of its start) goes with it
void group_pass(Frame& f, bool shadow)
{
    Entry* order[kQuadsMost];
    int n = 0;
    for (int i = 0; i < f.count; ++i)
        if (f.entries[i].shadow == shadow)
            order[n++] = &f.entries[i];
    std::stable_sort(order, order + n, [](const Entry* a, const Entry* b) { return a->x0 != b->x0 ? a->x0 < b->x0 : a->y0 < b->y0; });
    for (int start = 0, i = 1; i <= n; ++i) {
        const Entry* last = i > start ? order[i - 1] : nullptr;
        const bool joined = i < n && last != nullptr && !(order[i]->y0 >= last->y1 || order[i]->y1 <= last->y0) &&
                            order[i]->x0 - last->x1 <= kGap && order[i]->x0 >= last->x0;
        if (joined)
            continue;
        if (i > start)
            look_up(order + start, i - start);
        start = i;
    }
}

// Frame f's entries: the label sheets' quads of `shape` (false: it couldn't be read)
bool collect(Frame& f, const void* shape)
{
    __try {
        const char* node = at<const char*>(shape, kShapeList);
        const int quads = at<int>(shape, kShapeCount);
        for (int i = 0; i < quads && i < kQuadsMost && node != nullptr; ++i, node = at<const char*>(node, 0)) {
            const char* quad = at<const char*>(node, kNodeQuad);
            const int sheet = quad != nullptr ? sheet_of(quad) : -1;
            if (sheet < 0)
                continue;
            const auto part = g_parts.find(picture(sheet, at<std::uint16_t>(quad, kPlace), at<std::uint16_t>(quad, kPlace + 2),
                                                   at<std::uint16_t>(quad, kSize), at<std::uint16_t>(quad, kSize + 2)));
            f.entries[f.count++] = {quad, part != g_parts.end() ? part->second : -1, at<short>(quad, kOwnX),
                                    at<short>(quad, kOwnX + 2), at<short>(quad, kOwnY), at<short>(quad, kOwnY + 4),
                                    at<std::uint8_t>(quad, kBlendA) == kTakenAway, kGame, 0, 1.0f};
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        f.count = 0;
        return false;
    }
}

const Entry* entry_of(const char* quad)
{
    if (g_depth == 0)
        return nullptr;
    const Frame& f = g_frames[g_depth - 1];
    for (int i = 0; i < f.count; ++i)
        if (f.entries[i].quad == quad)
            return &f.entries[i];
    return nullptr;
}

} // namespace

void on_frame()
{
    const bool wanted = chatfont::labels_on();
    if (wanted && !g_looked && !find_settings())
        return;
    g_on = wanted && japanese();
}

bool on() { return g_on; }

void shape_begin(const void* shape)
{
    if (g_depth == kDepthMost)
        return;
    Frame& f = g_frames[g_depth++];
    f.shape = shape, f.count = 0;
    if (!g_on || shape == nullptr || !collect(f, shape) || f.count == 0)
        return;
    group_pass(f, false);
    group_pass(f, true);
}

void shape_end(const void* shape)
{
    if (g_depth > 0 && g_frames[g_depth - 1].shape == shape)
        --g_depth;
}

Quad::Quad(const char* quad)
{
    if (!g_on || quad == nullptr)
        return;
    const Entry* e = entry_of(quad);
    if (e == nullptr && sheet_of(quad) < 0)
        return;
    told_ = true;
    if (e == nullptr)
        vanacore_gfx_label_word(kGame, at<std::uint8_t>(quad, kBlendA) == kTakenAway ? 1 : 0, 0, 1.0f);
    else
        vanacore_gfx_label_word(e->what, e->shadow ? 1 : 0, e->code, e->wide);
}

Quad::~Quad()
{
    if (told_)
        vanacore_gfx_label_word(0, 0, 0, 1.0f);
}

const char* words(int index)
{
    return index >= 0 && index < static_cast<int>(std::size(data::kLabels)) ? data::kLabels[index].text : nullptr;
}

} // namespace vanacore::jplabels
