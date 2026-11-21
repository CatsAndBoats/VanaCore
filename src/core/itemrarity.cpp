#include "itemrarity.hpp"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "d3d8.hpp"
#include "imports.hpp"
#include "itemslot.hpp"
#include "listtext.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "sigs.hpp"
#include "text.hpp"

namespace vanacore::itemrarity {
namespace {

// An item's icon drawn (CTkItemDataMenber::DrawIcon in the PS2 client's names, FUN_1015a2d0: __thiscall on the item's
// data, ret 18h: x, y, scale, colour, count, flags), from every window that shows an item (the item details, lists,
// the equipment slots, trade...). The game makes the icon's texture from the item's bitmap the first time it's drawn.
// sub esp,8; push ebx; push esi; push edi; mov esi,ecx; call is-loaded; test al,al; jz; mov ecx,[esi+0D0Ch]. The
// field (0x150C in retail's build) is left open: see sigs::item_data_shift
constexpr char kDrawIcon[] = "83 EC 08 53 56 57 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8B 8E ?? ?? ?? ??";
constexpr std::size_t kDrawIconPrologue = 5; // sub esp,8; push ebx; push esi
// A string in the menu font in a colour (FUN_10129a40, __cdecl: x, y, text, 0, colour), which the item details window
// prints the item's name with, and many lists. mov eax,[esp+14h]; mov ecx,[esp+10h]; mov edx,[esp+0Ch]; push 1.0;
// push 1.0; push 44h; push eax
constexpr char kDrawText[] = "8B 44 24 14 8B 4C 24 10 8B 54 24 0C 68 00 00 80 3F 68 00 00 80 3F 6A 44 50";
constexpr std::size_t kDrawTextPrologue = 8; // the two movs
// The item details window's name (CTkItemInfo::OnDrawPrimitive, FUN_1013ca00), printed from a copy of the name (its
// first letter made a capital) right after the icon: push 0; push edx; push eax; push ecx; call; mov dx,[edi+0Ah]; ...
constexpr char kInfoName[] = "6A 00 52 50 51 E8 ?? ?? ?? ?? 66 8B 57 0A 66 8B 47 08 66 03 96 50 03 00 00";
constexpr std::ptrdiff_t kInfoNameReturn = 10; // where the call returns to
// A list's cell drawn (FUN_101f3310, __thiscall on the row, ret 18h: its window, cell, x, y, ..., colour), by its kind through
// a jump table that follows the function: the item's name (kind 0x19: the name its data holds) printed in the row's colour,
// which the list's controller (its vtable +0x30) makes the game's green for an equipped item. sub esp,154h; push ebx..edi;
// mov edi,[esp+16Ch]; mov ecx..; ...; jmp [eax*4+table]
constexpr char kCells[] = "81 EC 54 01 00 00 53 55 56 57 8B BC 24 6C 01 00 00 8B F1 8B 5C BE 28 85 DB 0F 84 ?? ?? ?? ?? "
                          "0F BE 04 37 48 83 F8 ?? 0F 87 ?? ?? ?? ?? FF 24 85";
constexpr std::ptrdiff_t kCellsTable = 0x30; // the jump table's address, the function's end
// An equipped item's name as the game prints it (the lists' and the details': 0x101EEEA0, 0x1013CB63), 0x80 full, and
// an item's name otherwise
constexpr std::uint32_t kEquipped = 0x509050;
constexpr std::uint32_t kPlain = 0x808080;
// The item details window (a window's +0xC: the object it belongs to, CTkItemInfo) holds the item data it shows at +0x1C:
// set by its set-item from the game's item list (0x1013C70C: mov [esi+1Ch],eax after 0x1015A710), handed to DrawIcon by its
// draw (0x1013CA6D)
constexpr std::ptrdiff_t kDetailsWindow = 0xC;
constexpr std::ptrdiff_t kDetailsItem = 0x1C;

// An item's data (CTkItemDataMenber), one of a few the game keeps loaded. These offsets are CatsEyeXI's build's: retail's
// has 0x800 more before them, so g_shift (read from the game's code when the hooks go in) is added to each
constexpr std::ptrdiff_t kItemId = 0xCB8;   // -> the item's id
constexpr std::ptrdiff_t kName = 0xCC8;     // -> its name
constexpr std::ptrdiff_t kBitmap = 0xCE8;   // -> its icon: a mark byte, a 16-character tag, then a 32x32 256-colour DIB
constexpr std::ptrdiff_t kBitmapSize = 0xCEC; // -> the icon's size
constexpr std::ptrdiff_t kIcon = 0xD0C;     // its icon (YmIcon), made from the bitmap the first time it's drawn
std::ptrdiff_t g_shift = 0;
constexpr std::ptrdiff_t kIconTexture = 8;  // YmIcon: the game's texture
constexpr std::ptrdiff_t kD3d = 0x40;       // the game's texture: its Direct3D texture (+0x44 when +0x40 is null)
constexpr std::ptrdiff_t kD3dElse = 0x44;
constexpr std::uint32_t kBitmapBytes = 0x839; // 1 + 16 + 40 + 256 * 4 + 32 * 32
constexpr std::uint8_t kBitmapMark = 0x91;
constexpr DWORD kFormatDxt3 = 0x33545844; // 'DXT3': the game's icons, alpha 0x80 (nibble 8) opaque
constexpr DWORD kLockReadOnly = 0x10;
constexpr int kOpaqueNibble = 8;
constexpr int kBand = 4; // the box: this many pixels in from the edge (5 in all)
constexpr int kTiers = 4;

using DrawIcon = void(__thiscall*)(void* item, int x, int y, float scale, std::uint32_t colour, int count, int flags);
using DrawText = void(__cdecl*)(int x, int y, const char* text, int flag, std::uint32_t colour);

struct Rgb {
    float r, g, b;
};

// The tiers' boxes as CatsEyeXI's icons draw them (0-255): the frame, then the two bevel lines in from it
constexpr std::uint8_t kShades[kTiers][3][3] = {
    {{184, 184, 184}, {160, 160, 160}, {136, 136, 136}}, // +1 grey (some 192)
    {{56, 128, 168}, {64, 112, 144}, {72, 96, 120}},     // +2 blue
    {{168, 144, 56}, {136, 120, 72}, {112, 104, 80}},    // +3 gold
    {{48, 168, 112}, {72, 144, 64}, {104, 120, 72}},     // +4 green
};
constexpr std::uint32_t kGameColours[kTiers] = {0xB8B8B8, 0x3880A8, 0xA89038, 0x30A870};

enum class Box { Game, Hide, Style };
enum class Slot { Game, Tint, Solid, Hide };

// [client] items.rarity*, and the item square's (items.slot*) that a styled box follows
Box g_box = Box::Game;
bool g_names = false;
bool g_tip_name = false, g_tip_gradient = false; // [client] tips.rarity.name, tips.rarity.gradient
int g_details_tier = -1; // the tier of the item the details window being drawn shows (Details), -1 outside its draw
bool g_details_said = false;
Rgb g_colours[kTiers];
// [client] items.equipped: an equipped item's name in a list and its details as the game has it (green), plain or in
// items.equipped.color (the game's 0x80 full); the lists' cell drawing it's printed from: [start, end)
bool g_equipped = false;
bool g_equipped_off = false; // items.equipped=off: an equipped item's name plain, and in its rarity's colour like any other
std::uint32_t g_equipped_rgb = kEquipped;
std::uintptr_t g_cells = 0, g_cells_end = 0;
Slot g_slot = Slot::Game;
float g_brightness = 1.0f, g_contrast = 1.0f, g_opacity = 1.0f;
unsigned g_version = 1; // the settings' generation: a texture written for an older one is written again

bool g_icon_tried = false, g_text_tried = false;
DrawIcon g_draw_icon = nullptr;
DrawText g_draw_text = nullptr;
std::uintptr_t g_info_return = 0;
unsigned g_frame = 0;

// An item's tier (0: no box) and its box's pixels, from its bitmap, kept per item data while it holds the same item
struct Item {
    const char* data = nullptr;
    std::uint32_t id = 0;
    const void* icon = nullptr;
    int tier = 0;
    std::array<std::uint8_t, 32 * 32> mask{};   // 1: the box's
    std::array<std::uint8_t, 32 * 32> opaque{}; // 1: drawn (the bitmap's alpha isn't 0)
    std::array<Rgb, 32 * 32> colour{};          // the bitmap's colours, top row first
};
constexpr std::size_t kItems = 64;
std::array<Item, kItems> g_items;
std::size_t g_next_item = 0;
const char* g_last_item = nullptr; // the last item whose icon was drawn
// The names (lower case) of the boxed items whose icons have been drawn, and their tiers: lists print a copy of the name
// kept with the list's row, so names are matched by their text
std::unordered_map<std::string, int> g_tier_names;
constexpr std::size_t kTierNames = 4096;

// The icons' textures VanaCore drew a box into: a hash of what it wrote (0: the game's own), the settings' generation it
// was written for, and the frames it was last read back and drawn
struct Texture {
    std::uint64_t written = 0;
    unsigned version = 0, checked = ~0u, seen = 0;
};
std::unordered_map<void*, Texture> g_textures;

std::uint64_t hash(const std::vector<std::uint8_t>& bytes)
{
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    for (const std::uint8_t b : bytes)
        h = (h ^ b) * 1099511628211ull;
    return h | 1; // never 0
}

// `text` in lower case (ASCII letters only; item names are ASCII), at most 64 characters
std::string lower(const char* text)
{
    std::string out;
    for (; *text != '\0' && out.size() < 64; ++text)
        out += (*text >= 'A' && *text <= 'Z') ? static_cast<char>(*text - 'A' + 'a') : *text;
    return out;
}

template <typename T>
T read(const char* at, std::ptrdiff_t offset)
{
    T value{};
    std::memcpy(&value, at + offset, sizeof value);
    return value;
}

Rgb rgb(std::uint32_t c)
{
    return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
}

float luma(Rgb c)
{
    return c.r * 0.299f + c.g * 0.587f + c.b * 0.114f;
}

int distance(const std::uint8_t* bgr, const std::uint8_t (&shade)[3])
{
    return std::abs(bgr[2] - shade[0]) + std::abs(bgr[1] - shade[1]) + std::abs(bgr[0] - shade[2]);
}

// `item`'s tier and box, from its bitmap: the outermost ring of pixels mostly a tier's frame colour, then the pixels
// of the 5-pixel band that are that tier's shades (the item's own pixels there kept out)
void classify(Item& item)
{
    item.tier = 0;
    const char* bitmap = *reinterpret_cast<const char* const*>(item.data + kBitmap + g_shift);
    const auto* size = *reinterpret_cast<const std::uint32_t* const*>(item.data + kBitmapSize + g_shift);
    if (bitmap == nullptr || size == nullptr || *size != kBitmapBytes || static_cast<std::uint8_t>(bitmap[0]) != kBitmapMark)
        return;
    const char* dib = bitmap + 17;
    if (read<std::int32_t>(dib, 4) != 32 || read<std::int32_t>(dib, 8) != 32 || read<std::uint16_t>(dib, 14) != 8)
        return;
    const auto* palette = reinterpret_cast<const std::uint8_t*>(dib + 40); // B, G, R, A (0x80 opaque)
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(dib + 40 + 256 * 4); // bottom row first
    const auto at = [&](int x, int y) { return palette + pixels[(31 - y) * 32 + x] * 4; };

    int votes[kTiers] = {}, opaque = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const std::uint8_t* p = at(x, y);
            item.colour[y * 32 + x] = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f};
            item.opaque[y * 32 + x] = p[3] != 0 ? 1 : 0;
            if (std::min({x, y, 31 - x, 31 - y}) != 0 || p[3] == 0)
                continue;
            ++opaque;
            for (int t = 0; t < kTiers; ++t)
                if (distance(p, kShades[t][0]) <= 24 || (t == 0 && p[0] == p[1] && p[1] == p[2] && p[0] >= 176 && p[0] <= 208))
                    ++votes[t];
        }
    const int best = static_cast<int>(std::max_element(votes, votes + kTiers) - votes);
    if (opaque < 100 || votes[best] < 80)
        return;
    item.tier = best + 1;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const std::uint8_t* p = at(x, y);
            bool box = false;
            if (std::min({x, y, 31 - x, 31 - y}) <= kBand && p[3] != 0) {
                if (best == 0)
                    box = std::abs(p[0] - p[1]) <= 8 && std::abs(p[1] - p[2]) <= 8 && p[1] >= 128 && p[1] <= 208;
                else
                    for (const auto& shade : kShades[best])
                        box = box || distance(p, shade) <= 40;
            }
            item.mask[y * 32 + x] = box ? 1 : 0;
        }
}

// The kept record of `data`'s item, read again when it holds another item or made its icon again
Item* item_for(const char* data)
{
    const auto* id = *reinterpret_cast<const std::uint32_t* const*>(data + kItemId + g_shift);
    const void* icon = *reinterpret_cast<const void* const*>(data + kIcon + g_shift);
    if (id == nullptr || icon == nullptr)
        return nullptr; // not loaded yet
    Item* item = nullptr;
    for (Item& kept : g_items)
        if (kept.data == data)
            item = &kept;
    if (item == nullptr) {
        item = &g_items[g_next_item];
        g_next_item = (g_next_item + 1) % kItems;
        item->data = data;
        item->icon = nullptr;
    }
    if (item->id != (*id & 0xFFFF) || item->icon != icon) {
        item->id = *id & 0xFFFF;
        item->icon = icon;
        classify(*item);
        const char* name = *reinterpret_cast<const char* const*>(data + kName + g_shift);
        if (item->tier != 0 && name != nullptr && g_tier_names.size() < kTierNames)
            g_tier_names[lower(name)] = item->tier;
    }
    return item;
}

// The item data the details window `ctrl` shows (kDetailsItem), or null: none, one not loaded yet (no id, no bitmap), one
// whose bitmap isn't an icon's, or (`odd` set) memory that can't be read (another window's object)
const char* details_item(const void* ctrl, bool& odd)
{
    odd = false;
    __try {
        const char* window = *reinterpret_cast<const char* const*>(static_cast<const char*>(ctrl) + kDetailsWindow);
        const char* data = window != nullptr ? *reinterpret_cast<const char* const*>(window + kDetailsItem) : nullptr;
        if (data == nullptr)
            return nullptr;
        const auto* id = *reinterpret_cast<const std::uint32_t* const*>(data + kItemId + g_shift);
        const char* bitmap = *reinterpret_cast<const char* const*>(data + kBitmap + g_shift);
        const auto* size = *reinterpret_cast<const std::uint32_t* const*>(data + kBitmapSize + g_shift);
        if (id == nullptr || (*id & 0xFFFF) == 0 || bitmap == nullptr || size == nullptr || *size != kBitmapBytes ||
            static_cast<std::uint8_t>(bitmap[0]) != kBitmapMark)
            return nullptr; // (classify finds no tier in another bitmap either)
        static_cast<void>(*reinterpret_cast<const volatile char*>(bitmap + kBitmapBytes - 1)); // (its last byte: all of it there)
        return data;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        odd = true;
        return nullptr;
    }
}

// The tier of the item in `data` (details_item's): its kept record's, else read from its bitmap now (its icon isn't made
// before its first draw, the details' first frame)
int tier_of(const char* data)
{
    if (const Item* item = item_for(data); item != nullptr)
        return item->tier;
    static Item fresh;
    fresh.data = data;
    classify(fresh);
    return fresh.tier;
}

// `tier`'s colour (items.rarity.color1-4), 0xRRGGBB
std::uint32_t tier_rgb(int tier)
{
    const Rgb c = g_colours[tier - 1];
    const auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return channel(c.r) << 16 | channel(c.g) << 8 | channel(c.b);
}

// A DXT colour block's four colours (always four in DXT3): its two ends, then 1/3 and 2/3 of the way
void block_palette(std::uint16_t c0, std::uint16_t c1, Rgb (&out)[4])
{
    const auto decode = [](std::uint16_t c) {
        return Rgb{((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f};
    };
    const Rgb a = decode(c0), b = decode(c1);
    out[0] = a;
    out[1] = b;
    out[2] = {(2 * a.r + b.r) / 3, (2 * a.g + b.g) / 3, (2 * a.b + b.b) / 3};
    out[3] = {(a.r + 2 * b.r) / 3, (a.g + 2 * b.g) / 3, (a.b + 2 * b.b) / 3};
}

// 16 colours into a DXT colour block: of every pair of the drawn texels' colours as its ends, the one whose four
// colours come nearest the drawn texels; each texel the nearest of the four (texels not drawn count for nothing)
void encode_block(const Rgb (&px)[16], const bool (&drawn)[16], std::uint8_t* block)
{
    const auto dist = [](Rgb a, Rgb b) { return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b); };
    const auto q = [](float v, int top) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * top)); };
    const auto encode = [&](Rgb c) { return static_cast<std::uint16_t>(q(c.r, 31) << 11 | q(c.g, 63) << 5 | q(c.b, 31)); };
    const auto nearest = [&](Rgb c, const Rgb (&palette)[4], float& error) {
        int best = 0;
        error = dist(c, palette[0]);
        for (int k = 1; k < 4; ++k)
            if (const float e = dist(c, palette[k]); e < error) {
                error = e;
                best = k;
            }
        return best;
    };
    std::uint16_t ends[16];
    int count = 0;
    for (int i = 0; i < 16; ++i)
        if (drawn[i] && std::find(ends, ends + count, encode(px[i])) == ends + count)
            ends[count++] = encode(px[i]);
    if (count == 0)
        ends[count++] = encode(px[0]);
    std::uint16_t c0 = ends[0], c1 = ends[0];
    float best = -1;
    for (int i = 0; i < count; ++i)
        for (int j = i; j < count; ++j) {
            Rgb palette[4];
            block_palette(std::max(ends[i], ends[j]), std::min(ends[i], ends[j]), palette);
            float total = 0, error = 0;
            for (int t = 0; t < 16; ++t)
                if (drawn[t]) {
                    nearest(px[t], palette, error);
                    total += error;
                }
            if (best < 0 || total < best) {
                best = total;
                c0 = std::max(ends[i], ends[j]);
                c1 = std::min(ends[i], ends[j]);
            }
        }
    Rgb palette[4];
    block_palette(c0, c1, palette);
    std::uint32_t bits = 0;
    float error = 0;
    for (int i = 0; i < 16; ++i)
        bits |= static_cast<std::uint32_t>(nearest(px[i], palette, error)) << (i * 2);
    std::memcpy(block, &c0, 2);
    std::memcpy(block + 2, &c1, 2);
    std::memcpy(block + 4, &bits, 4);
}

// The icon's DXT3 blocks with its box drawn as the settings say: the game's (from the item's bitmap), taken out or
// restyled. Only the box's pixels are drawn, from the bitmap, so drawing it again over itself changes nothing.
std::vector<std::uint8_t> draw_box(const std::vector<std::uint8_t>& current, const Item& item)
{
    std::vector<std::uint8_t> blocks = current;
    const Rgb colour = g_colours[item.tier - 1];
    const auto& frame = kShades[item.tier - 1][0];
    const float base = luma({frame[0] / 255.0f, frame[1] / 255.0f, frame[2] / 255.0f});
    const bool solid = g_slot == Slot::Solid, follows = g_slot == Slot::Tint || g_slot == Slot::Solid;
    const float brightness = follows ? g_brightness : 1.0f, contrast = follows ? g_contrast : 1.0f;
    const float opacity = follows ? g_opacity : 1.0f;
    // the box's shades as fractions of its frame's, which the contrast spreads from their mean
    float mean = 0;
    int count = 0;
    for (std::size_t i = 0; i < item.mask.size(); ++i)
        if (item.mask[i] != 0) {
            mean += luma(item.colour[i]) / base;
            ++count;
        }
    mean = count > 0 ? mean / count : 1.0f;
    for (int by = 0; by < 8; ++by)
        for (int bx = 0; bx < 8; ++bx) {
            std::uint8_t* block = blocks.data() + (by * 8 + bx) * 16;
            bool any = false;
            for (int i = 0; i < 16 && !any; ++i)
                any = item.mask[(by * 4 + i / 4) * 32 + bx * 4 + i % 4] != 0;
            if (!any)
                continue;
            // the whole block from the bitmap (the item's own texels too, not decoded from what's there: compressing
            // what was compressed before would drift further from the item each time)
            std::uint64_t alpha;
            std::memcpy(&alpha, block, 8);
            Rgb px[16];
            bool drawn[16];
            for (int i = 0; i < 16; ++i) {
                const int pixel = (by * 4 + i / 4) * 32 + bx * 4 + i % 4;
                px[i] = item.colour[pixel];
                if (item.mask[pixel] == 0) {
                    drawn[i] = (alpha >> (i * 4) & 0xF) != 0; // the item's: the game's alpha kept
                    continue;
                }
                int nibble = kOpaqueNibble;
                if (g_box == Box::Hide) {
                    nibble = 0;
                } else if (g_box == Box::Style) {
                    float level = solid ? 1.0f : ((luma(item.colour[pixel]) / base - mean) * contrast + mean);
                    level *= brightness;
                    px[i] = {colour.r * level, colour.g * level, colour.b * level};
                    nibble = std::min(kOpaqueNibble, static_cast<int>(std::lround(kOpaqueNibble * opacity)));
                }
                drawn[i] = nibble != 0;
                alpha = (alpha & ~(std::uint64_t{0xF} << (i * 4))) | std::uint64_t(nibble) << (i * 4);
            }
            std::memcpy(block, &alpha, 8);
            encode_block(px, drawn, block + 8);
        }
    return blocks;
}

// A DXT3 texture's first level: its blocks read into `blocks`, or written from them
bool texture_blocks(void* d3d, std::vector<std::uint8_t>& blocks, bool write)
{
    d3d8::SurfaceDesc desc{};
    if (FAILED(d3d8::call(d3d, d3d8::slot::TextureGetLevelDesc, 0u, &desc)) || desc.format != kFormatDxt3 ||
        desc.width != 32 || desc.height != 32)
        return false;
    constexpr std::size_t row = 8 * 16, rows = 8;
    if (write && blocks.size() != row * rows)
        return false;
    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(d3d, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<RECT*>(nullptr),
                          write ? DWORD{0} : kLockReadOnly)))
        return false;
    blocks.resize(row * rows);
    auto* bits = static_cast<std::uint8_t*>(locked.bits);
    for (std::size_t y = 0; y < rows; ++y) {
        if (write)
            std::memcpy(bits + y * locked.pitch, blocks.data() + y * row, row);
        else
            std::memcpy(blocks.data() + y * row, bits + y * locked.pitch, row);
    }
    d3d8::call(d3d, d3d8::slot::TextureUnlockRect, 0u);
    return true;
}

// The item's icon texture as the settings say. Read back once a frame: a texture the game made again (another icon,
// perhaps at the same address) is taken as the game's.
void sync_icon(const Item& item)
{
    const char* icon = *reinterpret_cast<const char* const*>(item.data + kIcon + g_shift);
    const char* texture = icon != nullptr ? *reinterpret_cast<const char* const*>(icon + kIconTexture) : nullptr;
    if (texture == nullptr)
        return;
    void* d3d = *reinterpret_cast<void* const*>(texture + kD3d);
    if (d3d == nullptr)
        d3d = *reinterpret_cast<void* const*>(texture + kD3dElse);
    if (d3d == nullptr)
        return;
    if (g_box == Box::Game && g_textures.find(d3d) == g_textures.end())
        return; // never drawn into
    Texture& state = g_textures[d3d];
    state.seen = g_frame;
    if (state.checked == g_frame && state.version == g_version)
        return;
    state.checked = g_frame;
    std::vector<std::uint8_t> now;
    if (!texture_blocks(d3d, now, false)) {
        g_textures.erase(d3d);
        return;
    }
    if (state.written != 0 && hash(now) != state.written) {
        state.written = 0; // made again: the game's own
        state.version = 0;
    }
    if (state.version == g_version)
        return;
    state.version = g_version;
    if (g_box == Box::Game && state.written == 0)
        return;
    std::vector<std::uint8_t> blocks = draw_box(now, item);
    if (!texture_blocks(d3d, blocks, true))
        return;
    state.written = g_box == Box::Game ? 0 : hash(blocks);
}

void __fastcall draw_icon_hook(char* data, void*, int x, int y, float scale, std::uint32_t colour, int count, int flags)
{
    g_last_item = data;
    itemslot::icon_drawn(x, y); // (items.equip: the Check and Equipment Set windows' slot names left out under icons)
    listtext::equip_icon(x, y); // (the Equipment window's slot names, drawn whole, left out under them too)
    if (g_box != Box::Game || !g_textures.empty()) {
        if (const Item* item = item_for(data); item != nullptr && item->tier != 0)
            sync_icon(*item);
    } else if (g_names) {
        item_for(data);
    }
    g_draw_icon(data, x, y, scale, colour, count, flags);
}

// The game's colour (0x80 full, alpha kept) for `tier`'s name
std::uint32_t name_colour(int tier, std::uint32_t colour)
{
    const Rgb c = g_colours[tier - 1];
    const auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 128)); };
    return (colour & 0xFF000000u) | channel(c.r) << 16 | channel(c.g) << 8 | channel(c.b);
}

// A string printed from `from` is an item's name: a list's cell's or the details window's
bool name_site(std::uintptr_t from) { return from == g_info_return || (from >= g_cells && from < g_cells_end); }

// `colour` for a string printed from `from`: an equipped item's name (the game's green, from a list's cell or the details
// window) in items.equipped's colour
std::uint32_t equipped_colour(std::uintptr_t from, std::uint32_t colour)
{
    return g_equipped && name_site(from) && (colour & 0xFFFFFFu) == kEquipped ? (colour & 0xFF000000u) | g_equipped_rgb : colour;
}

// An equipped item's name (the game's green) keeps its equipped colour, the game's or items.equipped.color, over its rarity's
// (items.rarity.names, tips.rarity.name) and, in True Font's tooltip, over the title's own colour (vanacore_gfx_tip_equipped):
// which item is worn mattered more (asked for 2026-11-06). items.equipped=off makes it plain, and its rarity's colour then
void __cdecl draw_text_hook(int x, int y, const char* text, int flag, std::uint32_t colour)
{
    const std::uintptr_t from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool equipped = !g_equipped_off && name_site(from) && (colour & 0xFFFFFFu) == kEquipped;
    colour = equipped_colour(from, colour);
    if ((g_names || g_tip_name) && text != nullptr && !equipped) {
        int tier = 0;
        if (from == g_info_return) {
            // the details window prints its long name ("Marid ring +1"): of the item it shows (Details, around its draw), else
            // the one whose icon it drew just before
            if (g_details_tier >= 0)
                tier = g_details_tier;
            else if (const Item* item = g_last_item != nullptr ? item_for(g_last_item) : nullptr; item != nullptr)
                tier = item->tier;
        } else if (const auto found = g_names && !g_tier_names.empty() ? g_tier_names.find(lower(text)) : g_tier_names.end();
                   found != g_tier_names.end()) {
            tier = found->second;
        }
        if (tier != 0)
            colour = name_colour(tier, colour);
    }
    // a list's name, its colour as set above: kept to be drawn whole in True Font (listtext.cpp), the game printing nothing
    if (from >= g_cells && from < g_cells_end && listtext::keep(x, y, text, colour))
        return;
    const bool worn = equipped && from == g_info_return;
    if (worn)
        vanacore_gfx_tip_equipped(1);
    g_draw_text(x, y, text, flag, colour);
    if (worn)
        vanacore_gfx_tip_equipped(0);
}

void install_icon_hook(HMODULE game)
{
    g_icon_tried = true;
    g_shift = sigs::item_data_shift();
    const std::uintptr_t at = scan::find(game, kDrawIcon);
    void* trampoline = at != 0 ? imports::detour(reinterpret_cast<void*>(at), kDrawIconPrologue, reinterpret_cast<void*>(&draw_icon_hook)) : nullptr;
    if (trampoline == nullptr) {
        log::write(L"Item rarity: the game's item icon drawing wasn't found or couldn't be hooked: [client] items.rarity does nothing");
        return;
    }
    g_draw_icon = reinterpret_cast<DrawIcon>(trampoline);
    log::write(L"Item rarity: the item icon drawing hooked at +0x%X", static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}

void install_text_hook(HMODULE game)
{
    g_text_tried = true;
    g_shift = sigs::item_data_shift();
    const std::uintptr_t at = scan::find(game, kDrawText);
    if (const std::uintptr_t info = scan::find(game, kInfoName); info != 0)
        g_info_return = info + kInfoNameReturn;
    else
        log::write(L"Item rarity: the item details window's name wasn't found: items.rarity.names leaves it");
    if (const std::uintptr_t cells = scan::find(game, kCells); cells != 0) {
        g_cells = cells;
        std::memcpy(&g_cells_end, reinterpret_cast<const void*>(cells + kCellsTable), sizeof g_cells_end);
        if (g_cells_end <= g_cells || g_cells_end - g_cells > 0x4000)
            g_cells = g_cells_end = 0;
    }
    if (g_cells == 0)
        log::write(L"Item rarity: the lists' cell drawing wasn't found: items.equipped leaves the lists' names");
    void* trampoline = at != 0 ? imports::detour(reinterpret_cast<void*>(at), kDrawTextPrologue, reinterpret_cast<void*>(&draw_text_hook)) : nullptr;
    if (trampoline == nullptr) {
        log::write(L"Item rarity: the game's coloured text drawing wasn't found or couldn't be hooked: items.rarity.names does nothing");
        return;
    }
    g_draw_text = reinterpret_cast<DrawText>(trampoline);
    log::write(L"Item rarity: the coloured text drawing hooked at +0x%X", static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}

// [client] items.equipped (game, off, color) and items.equipped.color (unset: the game's green)
void read_equipped(const wchar_t* ini)
{
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"client", L"items.equipped", L"game", text, 64, ini);
    const bool off = _wcsicmp(text, L"off") == 0, own = _wcsicmp(text, L"color") == 0;
    g_equipped = off || own;
    g_equipped_off = off;
    g_equipped_rgb = off ? kPlain : kEquipped;
    if (!own)
        return;
    GetPrivateProfileStringW(L"client", L"items.equipped.color", L"", text, 64, ini);
    float colour[4] = {static_cast<float>(kEquipped >> 16 & 0xFF) * 2, static_cast<float>(kEquipped >> 8 & 0xFF) * 2,
                       static_cast<float>(kEquipped & 0xFF) * 2, 1.0f};
    text::parse_colour(text, colour);
    const auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v / 255.0f, 0.0f, 1.0f) * 128)); };
    g_equipped_rgb = channel(colour[0]) << 16 | channel(colour[1]) << 8 | channel(colour[2]);
}

} // namespace

void reload(const wchar_t* ini)
{
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"client", L"items.rarity", L"game", text, 64, ini);
    g_box = _wcsicmp(text, L"hide") == 0 ? Box::Hide : _wcsicmp(text, L"style") == 0 ? Box::Style : Box::Game;
    g_names = GetPrivateProfileIntW(L"client", L"items.rarity.names", 0, ini) != 0;
    for (int t = 0; t < kTiers; ++t) {
        wchar_t key[32];
        swprintf_s(key, L"items.rarity.color%d", t + 1);
        GetPrivateProfileStringW(L"client", key, L"", text, 64, ini);
        float colour[4] = {static_cast<float>(kGameColours[t] >> 16 & 0xFF), static_cast<float>(kGameColours[t] >> 8 & 0xFF),
                           static_cast<float>(kGameColours[t] & 0xFF), 1.0f};
        text::parse_colour(text, colour);
        g_colours[t] = {colour[0] / 255.0f, colour[1] / 255.0f, colour[2] / 255.0f};
    }
    read_equipped(ini);
    GetPrivateProfileStringW(L"client", L"items.slot", L"game", text, 64, ini);
    g_slot = _wcsicmp(text, L"tint") == 0   ? Slot::Tint
             : _wcsicmp(text, L"solid") == 0 ? Slot::Solid
             : _wcsicmp(text, L"hide") == 0  ? Slot::Hide
                                             : Slot::Game;
    const auto percent = [ini](const wchar_t* key, int top) {
        const int value = static_cast<int>(GetPrivateProfileIntW(L"client", key, 100, ini));
        return (value < 0 ? 0 : value > top ? top : value) / 100.0f;
    };
    g_brightness = percent(L"items.slot.brightness", 200);
    g_contrast = percent(L"items.slot.contrast", 300);
    g_opacity = percent(L"items.slot.opacity", 200);
    ++g_version;
    // an item's details in its tier's colour: the name (here at the game's print, True Font's in the graphics layer) and
    // the gradient (the graphics layer, told the item's colour around the window's draw: Details)
    g_tip_name = GetPrivateProfileIntW(L"client", L"tips.rarity.name", 0, ini) != 0;
    g_tip_gradient = GetPrivateProfileIntW(L"client", L"tips.rarity.gradient", 0, ini) != 0;
    const float strength = static_cast<float>(std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", L"tips.rarity.gradient.strength", 30, ini)), 0, 100));
    const float height = static_cast<float>(std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", L"tips.rarity.gradient.height", 36, ini)), 4, 160));
    vanacore_gfx_set_tip_rarity(g_tip_name ? 1 : 0, g_tip_gradient ? strength / 100.0f : 0.0f, height);
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    ++g_frame;
    // (the text hook for the details' name, and it reads the item data's place in this build for Details too)
    const bool icons = g_box != Box::Game || g_names || itemslot::wants_icons() || listtext::equip_wanted(), names = g_names || g_tip_name || g_tip_gradient || g_equipped || listtext::wanted();
    if ((icons && !g_icon_tried) || (names && !g_text_tried)) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return;
        if (icons && !g_icon_tried)
            install_icon_hook(game);
        if (names && !g_text_tried)
            install_text_hook(game);
    }
    // textures holding the game's own box not drawn for a while are forgotten (those VanaCore drew into are kept, to be
    // put back if items.rarity goes back to game)
    if (g_frame % 600 == 0)
        for (auto it = g_textures.begin(); it != g_textures.end();)
            it = it->second.written == 0 && g_frame - it->second.seen > 600 ? g_textures.erase(it) : std::next(it);
}

Details::Details(const void* ctrl) : on_(false)
{
    if (ctrl == nullptr || !(g_tip_name || g_tip_gradient || g_names) || !g_text_tried)
        return;
    bool odd = false;
    const char* data = details_item(ctrl, odd);
    if (data == nullptr) {
        if (odd && !g_details_said)
            log::write(L"Item rarity: an item details window whose item data couldn't be read (its window at +0xC, the data at "
                       L"+0x1C): tips.rarity leaves it");
        g_details_said = g_details_said || odd;
        return;
    }
    on_ = true;
    g_details_tier = tier_of(data);
    vanacore_gfx_tip_item(1, g_details_tier != 0 ? 1 : 0, g_details_tier != 0 ? tier_rgb(g_details_tier) : 0u);
}

Details::~Details()
{
    if (!on_)
        return;
    g_details_tier = -1;
    vanacore_gfx_tip_item(0, 0, 0u);
}

} // namespace vanacore::itemrarity
