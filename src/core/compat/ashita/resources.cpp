// IResourceManager: items, abilities, spells and string tables from the game's DATs (read once, on
// first use, and kept; the items are indexed then and made as they are asked for), and the entity helpers.
// Item records are laid out as in both the legacy (0xC00-byte) and the September 2026 retail (0x1400-byte)
// item DATs.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../dats.hpp"
#include "../../game.hpp"
#include "../../log.hpp"
#include "backend.hpp"

namespace vanacore::ashita {
namespace {

// The game's text language for resources: 1 Japanese, 2 English ([language] playonline=). Read once: the game's
// language is set at boot, and every GetString without a language comes here (reading the ini each time cost XIUI
// ~40 us a call, several calls a party member a frame).
uint32_t default_language()
{
    static std::atomic<uint32_t> cached{0};
    uint32_t language = cached.load(std::memory_order_relaxed);
    if (language != 0)
        return language;
    const std::wstring& ini = settings_ini();
    if (ini.empty()) // not installed yet: English, and ask again next time
        return 2;
    language = GetPrivateProfileIntW(L"language", L"playonline", 0, ini.c_str()) == 1 ? 1 : 2;
    cached.store(language, std::memory_order_relaxed);
    return language;
}

uint32_t language_or_default(uint32_t language)
{
    return language == 1 || language == 2 ? language : default_language();
}

// Strings handed out stay put for the process's life.
std::mutex g_text_lock;
std::deque<std::string> g_texts;

const char* keep(std::string text)
{
    const std::lock_guard lock(g_text_lock);
    g_texts.push_back(std::move(text));
    return g_texts.back().c_str();
}

uint32_t u32(const uint8_t* at)
{
    uint32_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

uint16_t u16(const uint8_t* at)
{
    uint16_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Items

enum class Layout { General, Usable, Puppet, Armor, Weapon, Maze, Instinct };

struct ItemDat {
    uint32_t en_id;
    uint32_t jp_id;
    Layout layout;
};
// Ashita's DAT map (config\ashita\ashita.datmap.ini): the item DATs by file id.
constexpr ItemDat kItemDats[] = {
    {73, 4, Layout::General},        // general items
    {74, 5, Layout::Usable},         // usable items
    {75, 6, Layout::Weapon},         // weapons
    {76, 7, Layout::Armor},          // armor
    {77, 8, Layout::Puppet},         // automaton attachments
    {91, 9, Layout::General},        // currency (gil)
    {55667, 55547, Layout::General}, // storage slips
    {55668, 55548, Layout::Armor},   // armor, part two
    {55669, 55549, Layout::General}, // monstrosity
    {55670, 55550, Layout::Instinct}, // instincts
    {55671, 55551, Layout::General}, // general items, part two
};

// CatsEyeXI's own items (cexidats' id blocks, the dats repo's Custom_Items) are written after the instincts in the
// instincts' DAT (55670, ROM/288/80), each block's records in its own layout. Ashita lays a record out by its id, not
// its DAT, and cexidats gives Ashita these blocks; read by the instincts' layout, a custom armour or weapon had no
// slots and no jobs, and LuAshitacast wouldn't equip it.
struct IdBlock {
    uint32_t first;
    uint32_t last;
    Layout layout;
};
constexpr IdBlock kCustomBlocks[] = {
    {30720, 34815, Layout::General},
    {34816, 38911, Layout::Usable},
    {38912, 47103, Layout::Armor},
    {47104, 55295, Layout::Weapon},
    {55296, 57343, Layout::General}, // furnishings
};

// The layout of item `id`'s record in a DAT whose own layout is `dat`.
Layout record_layout(uint32_t id, Layout dat)
{
    for (const IdBlock& block : kCustomBlocks)
        if (id >= block.first && id <= block.last)
            return block.layout;
    return dat;
}

constexpr size_t kLegacyStride = 0xC00;
constexpr size_t kRetailStride = 0x1400;
constexpr size_t kIconOffset = 0x280; // the icon: u32 size, then the image

// An item DAT is read in blocks, never whole: the biggest are 60 to 130 MB, and a block that size can't be found in a
// 32-bit game's address space once its textures are loaded (the C++ exception that ended GetItemById for equipmon and
// LuAshitacast, and the game after them). A multiple of both record sizes (0xF000 is their least common multiple), so
// no record straddles two blocks.
constexpr size_t kBlock = 0xF000 * 8;

bool read_at(HANDLE file, uint64_t offset, void* out, size_t size)
{
    OVERLAPPED where = {};
    where.Offset = static_cast<DWORD>(offset);
    where.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    return ReadFile(file, out, static_cast<DWORD>(size), &read, &where) && read == size;
}

// `each(offset, block)` for the file's blocks in order. False if one can't be read.
template <class Each>
bool each_block(HANDLE file, uint64_t size, Each&& each)
{
    std::vector<uint8_t> block(kBlock);
    for (uint64_t at = 0; at < size; at += kBlock) {
        block.resize(static_cast<size_t>(std::min<uint64_t>(kBlock, size - at)));
        if (!read_at(file, at, block.data(), block.size()))
            return false;
        each(at, block);
    }
    return true;
}

// The record size of an item DAT: whichever of the two divides its size, or the one whose
// records end in the 0xFF terminator when both do.
size_t stride_of(HANDLE file, uint64_t size)
{
    const bool legacy = size >= kLegacyStride && size % kLegacyStride == 0;
    const bool retail = size >= kRetailStride && size % kRetailStride == 0;
    if (legacy != retail)
        return legacy ? kLegacyStride : kRetailStride;
    if (!legacy)
        return 0;
    size_t hits[2] = {}; // records ending in the terminator, at the legacy stride and at the retail one
    const bool read = each_block(file, size, [&](uint64_t, const std::vector<uint8_t>& block) {
        for (size_t i = 0; i < 2; ++i)
            for (size_t at = (i ? kRetailStride : kLegacyStride) - 1; at < block.size(); at += i ? kRetailStride : kLegacyStride)
                hits[i] += block[at] == 0xFF;
    });
    if (!read)
        return 0;
    return hits[1] * kLegacyStride > hits[0] * kRetailStride ? kRetailStride : kLegacyStride;
}

// An item DAT's bytes are stored rotated.
void decode(uint8_t* bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>((bytes[i] >> 5) | (bytes[i] << 3));
}

// Where the string block starts, per layout: legacy, retail.
size_t text_offset(Layout layout, bool retail)
{
    switch (layout) {
    case Layout::Usable: return 0x1C;
    case Layout::Armor: return retail ? 0x30 : 0x2C;
    case Layout::Weapon: return retail ? 0x3C : 0x38;
    case Layout::Maze: return 0x54;
    case Layout::Instinct: return retail ? 0x2C : 0x28;
    default: return retail ? 0x1C : 0x18;
    }
}

// A string block (u32 count, then count x {u32 offset, u32 kind}) at `at`?
bool string_block(const uint8_t* record, size_t size, size_t at)
{
    if (at + 8 > size)
        return false;
    const uint32_t count = u32(record + at);
    return count >= 1 && count <= 16 && u32(record + at + 4) == 4 + count * 8 && at + u32(record + at + 4) < kIconOffset;
}

// The record's strings (empty for number slots).
std::vector<std::string> record_strings(const uint8_t* record, size_t size, size_t expected)
{
    size_t at = expected;
    if (!string_block(record, size, at)) {
        at = 0;
        for (size_t probe = 0x08; probe < 0x92; probe += 2) {
            if (string_block(record, size, probe)) {
                at = probe;
                break;
            }
        }
        if (at == 0)
            return {};
    }
    const uint32_t count = u32(record + at);
    std::vector<std::string> out(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = u32(record + at + 4 + i * 8);
        const uint32_t kind = u32(record + at + 8 + i * 8);
        const size_t text = at + offset + 0x1C; // after the sub-string's marker and 0x18 bytes
        if (kind != 0 || text >= size)
            continue;
        const auto* start = reinterpret_cast<const char*>(record + text);
        out[i].assign(start, strnlen(start, size - text));
    }
    return out;
}

// The item's numbers from its record: the header all layouts share, then its layout's own
// (offsets per record format, as xi-tools' item layouts tabulate them).
void fill_fields(sdk::IItem& item, const uint8_t* r, Layout layout, bool retail)
{
    item.Id = u32(r);
    item.Flags = u16(r + 0x04);
    item.StackSize = u16(r + (retail ? 0x08 : 0x06));
    item.Type = u16(r + (retail ? 0x0A : 0x08));
    item.ResourceId = u16(r + (retail ? 0x0C : 0x0A));
    item.Targets = u16(r + (retail ? 0x0E : 0x0C));
    const size_t shift = retail ? 2 : 0;     // after the common header
    const size_t equip_shift = retail ? 4 : 0; // after an equipment's races
    switch (layout) {
    case Layout::Usable:
        item.CastTime = u16(r + 0x0E + shift);
        break;
    case Layout::Puppet:
        item.PuppetSlotId = u16(r + 0x0E + shift);
        item.PuppetElements = u32(r + (retail ? 0x14 : 0x10));
        break;
    case Layout::Instinct:
        item.Level = u16(r + 0x0E + shift);
        item.Races = u16(r + 0x12 + shift);
        item.InstinctCost = u16(r + 0x18 + equip_shift);
        break;
    case Layout::Armor:
    case Layout::Weapon: {
        item.Level = u16(r + 0x0E + shift);
        item.Slots = u16(r + 0x10 + shift);
        item.Races = u16(r + 0x12 + shift);
        item.Jobs = u32(r + 0x14 + equip_shift);
        item.SuperiorLevel = u16(r + 0x18 + equip_shift);
        // the enchantment's numbers, after the shield size (armour) or the weapon's own: charges and cast time a byte each,
        // the cast delay 16 bits, the recast 32 (a Warp Ring's 20 hours are 72000 seconds), the base item, the item level a
        // byte. Read before as four 16-bit numbers, every one after the charges a field out (as POLUtils, Windower's
        // resources and XI on Anything's res.c have them)
        const size_t charges = (layout == Layout::Armor ? 0x1C : 0x28) + equip_shift;
        if (layout == Layout::Armor) {
            item.ShieldSize = u16(r + 0x1A + equip_shift);
        } else {
            item.Damage = u16(r + 0x1C + equip_shift);
            item.Delay = u16(r + 0x1E + equip_shift);
            item.DPS = u16(r + 0x20 + equip_shift);
            item.Skill = r[0x22 + equip_shift];
            item.JugSize = r[0x23 + equip_shift];
        }
        item.MaxCharges = r[charges];
        item.CastTime = r[charges + 1];
        item.CastDelay = u16(r + charges + 2);
        item.RecastDelay = u32(r + charges + 4);
        item.BaseItemId = u16(r + charges + 8);
        item.ItemLevel = r[charges + 10];
        break;
    }
    default:
        break;
    }
}

// The items are indexed on first use (where each id's records are, and its names) and made one at a time, when something
// asks for them: an IItem is 3 KB with its icon, and all 57,857 of them were ~190 MB a 32-bit game on Wine didn't have.
// Addons get theirs without the icon (kAddonItem), read when one asks for its Bitmap: SimpleLog asks for every item there
// is when it loads, 151 MB of the game's address space as whole items.
struct Place {
    int8_t dat = -1; // the index in kItemDats, -1 for none
    uint32_t offset = 0;
};

struct Stub {
    Place fields;             // the English record that made the item: its numbers and its icon
    Place text;               // the last English record of the id: its strings
    Place japanese;           // the Japanese record: its strings
    const char* name[2] = {}; // Japanese, English: what Name[1] and Name[2] will be
};

// An item DAT, left open for reading a record at a time.
struct DatFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    size_t stride = 0;
    bool retail = false;
};

struct Items {
    std::mutex lock;                // the index being built, and the items being made
    std::atomic<bool> ready{false}; // the index is built: the stubs and files stay as they are from now on
    ULONGLONG retry_at = 0;         // after an index that failed, when to try again
    std::vector<Stub> stubs;        // by id
    std::vector<uint16_t> order;    // the ids with an English record, in the order they were read
    std::unique_ptr<std::atomic<sdk::IItem*>[]> by_id;
    std::vector<std::unique_ptr<sdk::IItem>> owned;
    std::unique_ptr<std::atomic<sdk::IItem*>[]> addon_by_id; // the addons' items, without their icons
    std::vector<std::unique_ptr<uint8_t[]>> addon_owned;
    DatFile files[2][std::size(kItemDats)]; // English, Japanese
};
Items g_items;

// An addon's item: an IItem up to its Bitmap (ImageSize is there), 341 bytes of the 2,776. Its Bitmap is read from the
// DAT when the addon asks for it (item_icon).
constexpr size_t kAddonItem = offsetof(sdk::IItem, Bitmap);

void close_dat(DatFile& file)
{
    if (file.handle != INVALID_HANDLE_VALUE)
        CloseHandle(file.handle);
    file = DatFile{};
}

void index_dat(size_t index, bool japanese)
{
    const ItemDat& dat = kItemDats[index];
    const uint32_t id = japanese ? dat.jp_id : dat.en_id;
    DatFile& file = g_items.files[japanese][index];
    const std::wstring path = dats::path(id);
    if (!path.empty())
        file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size = {};
    if (file.handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.handle, &size)) {
        log::write(L"Resources: item DAT %u isn't there", id);
        close_dat(file);
        return;
    }
    file.stride = stride_of(file.handle, static_cast<uint64_t>(size.QuadPart));
    if (file.stride == 0) {
        log::write(L"Resources: DAT %u isn't an item DAT (%llu bytes)", id, static_cast<uint64_t>(size.QuadPart));
        close_dat(file);
        return;
    }
    file.retail = file.stride == kRetailStride;
    const size_t stride = file.stride;
    const bool read = each_block(file.handle, static_cast<uint64_t>(size.QuadPart), [&](uint64_t base, std::vector<uint8_t>& block) {
        decode(block.data(), block.size());
        for (size_t at = 0; at + stride <= block.size(); at += stride) {
            const uint8_t* record = block.data() + at;
            const uint32_t item = u32(record);
            if (item >= 65536)
                continue;
            // Blank records ("." for a name) are items too, as in Ashita: SimpleLog lists GetItemById(1, 2, ...) up to the
            // first nil and looks items up by their place in that list.
            std::vector<std::string> strings = record_strings(record, stride, text_offset(record_layout(item, dat.layout), file.retail));
            if (strings.empty() || strings[0].empty())
                strings.assign(1, ".");
            Stub& stub = g_items.stubs[item];
            const Place place{static_cast<int8_t>(index), static_cast<uint32_t>(base + at)};
            if (japanese) {
                if (strings[0] == "." || stub.fields.dat < 0) // only the English DAT makes items
                    continue;
                stub.japanese = place;
                stub.name[0] = keep(std::move(strings[0]));
            } else {
                if (stub.fields.dat < 0) {
                    stub.fields = place;
                    g_items.order.push_back(static_cast<uint16_t>(item));
                }
                stub.text = place;
                stub.name[1] = keep(std::move(strings[0]));
            }
        }
    });
    if (!read)
        log::write(L"Resources: item DAT %u couldn't be read to its end", id);
}

void close_index()
{
    for (auto& language : g_items.files)
        for (DatFile& file : language)
            close_dat(file);
    std::vector<Stub>().swap(g_items.stubs);
    std::vector<uint16_t>().swap(g_items.order);
    g_items.by_id.reset();
    g_items.addon_by_id.reset();
}

// With the lock held: is the index there? One that failed (a DAT's blocks or the stubs didn't fit in memory) is tried
// again 10 seconds on, not on every call.
bool ensure_index()
{
    if (g_items.ready.load(std::memory_order_relaxed))
        return true;
    if (GetTickCount64() < g_items.retry_at)
        return false;
    try {
        const ULONGLONG start = GetTickCount64();
        g_items.stubs.assign(65536, Stub{});
        g_items.by_id = std::make_unique<std::atomic<sdk::IItem*>[]>(65536);
        g_items.addon_by_id = std::make_unique<std::atomic<sdk::IItem*>[]>(65536);
        for (size_t i = 0; i < std::size(kItemDats); ++i)
            index_dat(i, false);
        for (size_t i = 0; i < std::size(kItemDats); ++i)
            index_dat(i, true);
        g_items.ready.store(true, std::memory_order_release);
        log::write(L"Resources: %zu items read in %llu ms", g_items.order.size(), GetTickCount64() - start);
        return true;
    } catch (const std::exception& e) {
        log::write(L"Resources: the item DATs couldn't be read (%hs); trying again in 10 s", e.what());
        close_index();
        g_items.retry_at = GetTickCount64() + 10000;
        return false;
    }
}

// A record of the item DAT `place` is in, decoded.
bool read_record(const Place& place, bool japanese, std::vector<uint8_t>& out)
{
    const DatFile& file = g_items.files[japanese][place.dat];
    if (file.handle == INVALID_HANDLE_VALUE)
        return false;
    out.resize(file.stride);
    if (!read_at(file.handle, place.offset, out.data(), out.size()))
        return false;
    decode(out.data(), out.size());
    return true;
}

// The strings of the record at `place` (none if it can't be read).
std::vector<std::string> record_strings_at(const Place& place, bool japanese, std::vector<uint8_t>& scratch)
{
    if (!read_record(place, japanese, scratch))
        return {};
    const DatFile& file = g_items.files[japanese][place.dat];
    const Layout layout = record_layout(u32(scratch.data()), kItemDats[place.dat].layout);
    return record_strings(scratch.data(), file.stride, text_offset(layout, file.retail));
}

// The size of a record's icon, 0 for none (or one that doesn't fit an IItem's Bitmap or the record).
uint32_t icon_size(const uint8_t* record, size_t stride)
{
    const uint32_t icon = u32(record + kIconOffset);
    return icon > 0 && icon <= sizeof(sdk::IItem::Bitmap) && kIconOffset + 4 + icon <= stride ? icon : 0;
}

// With the lock held: `item` (zeroed) filled in as item `id`, from its records, with its Bitmap if `icon`. False if the
// index has none or they can't be read.
bool fill_item(uint32_t id, sdk::IItem& item, bool icon)
{
    const Stub& stub = g_items.stubs[id];
    std::vector<uint8_t> record;
    if (stub.fields.dat < 0 || !read_record(stub.fields, false, record))
        return false;
    const DatFile& numbers = g_items.files[0][stub.fields.dat];
    fill_fields(item, record.data(), record_layout(id, kItemDats[stub.fields.dat].layout), numbers.retail);
    item.ImageSize = icon_size(record.data(), numbers.stride);
    if (icon)
        std::memcpy(item.Bitmap, record.data() + kIconOffset + 4, item.ImageSize);
    const std::vector<std::string> english = record_strings_at(stub.text, false, record);
    item.Name[2] = stub.name[1];
    item.LogNameSingular[2] = english.size() > 2 ? keep(english[2]) : nullptr;
    item.LogNamePlural[2] = english.size() > 3 ? keep(english[3]) : nullptr;
    item.Description[2] = english.size() > 4 ? keep(english[4]) : nullptr;
    if (stub.japanese.dat >= 0) {
        const std::vector<std::string> japanese = record_strings_at(stub.japanese, true, record);
        item.Name[1] = stub.name[0];
        item.Description[1] = japanese.size() > 1 ? keep(japanese[1]) : nullptr;
    }
    // [0] is the default language's, falling back to the other.
    const bool japanese = default_language() == 1;
    const auto pick = [&](const char* (&texts)[3]) { texts[0] = japanese && texts[1] != nullptr ? texts[1] : texts[2] != nullptr ? texts[2] : texts[1]; };
    pick(item.Name);
    pick(item.Description);
    pick(item.LogNameSingular);
    pick(item.LogNamePlural);
    return true;
}

// With the lock held: the item `id`, whole (a plugin's), or without its icon (an addon's). Null if the index has none or
// its records can't be read.
sdk::IItem* make_item(uint32_t id, bool whole)
{
    if (whole) {
        auto made = std::make_unique<sdk::IItem>();
        std::memset(made.get(), 0, sizeof(sdk::IItem));
        if (!fill_item(id, *made, true))
            return nullptr;
        sdk::IItem* item = made.get();
        g_items.owned.push_back(std::move(made));
        g_items.by_id[id].store(item, std::memory_order_release);
        return item;
    }
    sdk::IItem scratch;
    std::memset(&scratch, 0, sizeof(scratch));
    if (!fill_item(id, scratch, false))
        return nullptr;
    auto made = std::make_unique<uint8_t[]>(kAddonItem);
    std::memcpy(made.get(), &scratch, kAddonItem);
    auto* item = reinterpret_cast<sdk::IItem*>(made.get());
    g_items.addon_owned.push_back(std::move(made));
    g_items.addon_by_id[id].store(item, std::memory_order_release);
    return item;
}

// What there is of item `id` for a plugin (`whole`: by_id) or an addon (an addon's item, else a whole one), null if none.
sdk::IItem* made_item(uint32_t id, bool whole)
{
    if (!whole)
        if (sdk::IItem* item = g_items.addon_by_id[id].load(std::memory_order_acquire))
            return item;
    return g_items.by_id[id].load(std::memory_order_acquire);
}

// The item `id` (`whole` for a plugin; an addon's has no Bitmap): null for none, and for one the game's memory had no room
// to make (it's asked for again).
sdk::IItem* item_by_id(uint32_t id, bool whole = true)
{
    if (id >= 65536)
        return nullptr;
    if (g_items.ready.load(std::memory_order_acquire))
        if (sdk::IItem* item = made_item(id, whole))
            return item;
    const std::lock_guard lock(g_items.lock);
    if (!ensure_index())
        return nullptr;
    if (sdk::IItem* item = made_item(id, whole))
        return item;
    try {
        return make_item(id, whole);
    } catch (const std::exception& e) {
        static int failures = 0;
        if (++failures <= 5)
            log::write(L"Resources: item %u couldn't be made (%hs)", id, e.what());
        return nullptr;
    }
}

// Item `id`'s icon (its Bitmap's bytes, ImageSize of them), read from its record: an addon's item's Bitmap. Empty for
// none.
std::string item_icon(uint32_t id)
{
    if (id >= 65536)
        return {};
    const std::lock_guard lock(g_items.lock);
    if (!ensure_index())
        return {};
    const Stub& stub = g_items.stubs[id];
    try {
        std::vector<uint8_t> record;
        if (stub.fields.dat < 0 || !read_record(stub.fields, false, record))
            return {};
        const uint32_t size = icon_size(record.data(), g_items.files[0][stub.fields.dat].stride);
        return std::string(reinterpret_cast<const char*>(record.data() + kIconOffset + 4), size);
    } catch (const std::exception&) {
        return {};
    }
}

// The name an item has by default (Name[0]), without making the item.
const char* default_name(const Stub& stub)
{
    const bool japanese = default_language() == 1;
    return japanese && stub.name[0] != nullptr ? stub.name[0] : stub.name[1] != nullptr ? stub.name[1] : stub.name[0];
}

const char* item_name(uint32_t id)
{
    if (id >= 65536)
        return nullptr;
    if (!g_items.ready.load(std::memory_order_acquire)) {
        const std::lock_guard lock(g_items.lock);
        if (!ensure_index())
            return nullptr;
    }
    return default_name(g_items.stubs[id]);
}

// The first item (in the DATs' order) whose name in `language` (1 Japanese, 2 English) is `name`; `whole` as item_by_id.
sdk::IItem* item_by_name(const char* name, uint32_t language, bool whole = true)
{
    if (!g_items.ready.load(std::memory_order_acquire)) {
        const std::lock_guard lock(g_items.lock);
        if (!ensure_index())
            return nullptr;
    }
    for (const uint16_t id : g_items.order) {
        const char* text = g_items.stubs[id].name[language == 1 ? 0 : 1];
        if (text != nullptr && _stricmp(text, name) == 0)
            return item_by_id(id, whole);
    }
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// String tables (d_msg DATs)

struct StringDat {
    const char* name;
    uint32_t en_id;
    uint32_t jp_id; // 0: none
    uint32_t en_part;
    uint32_t jp_part;
    bool dialog = false; // a "dialog" DAT (Ashita's type 1: XOR 0x80, an offset table) rather than a d_msg one (type 3)
};
// Ashita's DAT map (config\ashita\ashita.datmap.ini): each table, its DATs, and which of an
// entry's strings it is.
constexpr StringDat kStringDats[] = {
    {"action.messages", 7027, 7026, 0, 0, true},
    {"monsters.abilities", 7035, 7034, 0, 0, true},
    {"abilities.names", 55701, 55581, 0, 0},
    {"abilities.descriptions", 55733, 55613, 0, 0},
    {"spells.names", 55702, 55582, 0, 0},
    {"spells.descriptions", 55734, 55614, 0, 0},
    {"augments", 55692, 55572, 0, 0},
    {"buffs.names", 55732, 55605, 0, 0},
    {"buffs.names_log", 55732, 0, 1, 0},
    {"commands.help", 55687, 55567, 0, 0},
    {"days", 55658, 55538, 0, 0},
    {"directions", 55659, 55539, 0, 0},
    {"emotes", 55675, 55555, 0, 0},
    {"equipment.slots", 55471, 0, 0, 0},
    {"equipment.slots_old", 55666, 55546, 0, 0},
    {"jobpoints", 55694, 55574, 0, 0},
    {"jobpoints.gifts", 55674, 55554, 0, 0},
    {"jobs.names", 55467, 55536, 0, 0},
    {"jobs.names_abbr", 55468, 55468, 0, 0},
    {"keyitems.names", 55697, 55577, 4, 1},
    {"keyitems.names_plural", 55697, 0, 5, 0},
    {"keyitems.descriptions", 55697, 55577, 6, 2},
    {"merits", 55686, 55566, 0, 0},
    {"monsters.groups", 55690, 55570, 0, 0},
    {"monster.groups_plural", 55690, 0, 1, 0},
    {"moonphases", 55660, 55540, 0, 0},
    {"mounts.names", 55681, 55561, 0, 0},
    {"mounts.descriptions", 55682, 55562, 0, 0},
    {"races", 55469, 55469, 0, 0},
    {"regions", 55654, 55534, 0, 0},
    {"titles", 55704, 55584, 0, 0},
    {"weather", 55657, 55537, 0, 0},
    {"weather.effects", 55657, 0, 1, 0},
    {"zones.names", 55465, 55535, 0, 0},
    {"zones.names_abbr", 55661, 0, 0, 0},
    {"zones.names_search", 55466, 55466, 0, 0},
};

// One language's table: entry index (or, for key items, key item id) to text.
struct Strings {
    std::map<uint32_t, const char*> by_index;
};

std::mutex g_strings_lock;
std::map<std::pair<std::string, uint32_t>, std::unique_ptr<Strings>> g_strings; // (table, language)

// The text of sub-string `part` of a d_msg block, and its first sub-string's marker (a key
// item's id). False if the block isn't well formed.
bool block_text(const uint8_t* block, size_t size, uint32_t part, std::string& text, uint32_t& marker)
{
    if (size < 4)
        return false;
    const uint32_t count = u32(block);
    if (count == 0 || count > 64 || 4 + static_cast<size_t>(count) * 8 > size)
        return false;
    const uint32_t first = u32(block + 4);
    marker = first + 4 <= size ? u32(block + first) : 0;
    if (part >= count)
        return false;
    const uint32_t offset = u32(block + 4 + part * 8);
    if (offset + 4 > size || u32(block + offset) != 1) // 1: a text sub-string
        return false;
    const size_t at = offset + 4 + 0x18;
    if (at >= size)
        return false;
    const auto* start = reinterpret_cast<const char*>(block + at);
    text.assign(start, strnlen(start, size - at));
    return true;
}

// A "dialog" DAT (monster abilities, action messages: ROM\27\*.DAT): a u32 header whose low 28 bits are the file's
// size less 4, then, every byte XOR 0x80, a table of u32 offsets from the table's start (the first offset over 4 is
// their count) to NUL-terminated strings. Entry i is the string at offset i (Ashita's indexes: a monster ability's
// action id less 256).
std::unique_ptr<Strings> read_dialog(uint32_t file)
{
    auto out = std::make_unique<Strings>();
    std::vector<uint8_t> raw;
    if (file == 0 || !dats::read(file, raw) || raw.size() < 8 || (u32(raw.data()) & 0x0FFFFFFF) != raw.size() - 4)
        return out;
    for (size_t i = 4; i < raw.size(); ++i)
        raw[i] ^= 0x80;
    const size_t table = 4;
    const uint32_t first = u32(raw.data() + table);
    const uint32_t count = first / 4;
    if (first % 4 != 0 || count == 0 || table + static_cast<size_t>(count) * 4 > raw.size())
        return out;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = table + u32(raw.data() + table + static_cast<size_t>(i) * 4);
        if (at >= raw.size())
            continue;
        const auto* start = reinterpret_cast<const char*>(raw.data() + at);
        out->by_index[i] = keep(std::string(start, strnlen(start, raw.size() - at)));
    }
    return out;
}

std::unique_ptr<Strings> read_strings(const StringDat& dat, uint32_t language)
{
    if (dat.dialog)
        return read_dialog(language == 1 ? dat.jp_id : dat.en_id);
    auto out = std::make_unique<Strings>();
    const uint32_t file = language == 1 ? dat.jp_id : dat.en_id;
    const uint32_t part = language == 1 ? dat.jp_part : dat.en_part;
    std::vector<uint8_t> raw;
    if (file == 0 || !dats::read(file, raw) || raw.size() < 0x40 || std::memcmp(raw.data(), "d_msg", 5) != 0)
        return out;
    const bool flip = u16(raw.data() + 0x0A) == 1;
    const uint32_t file_size = u32(raw.data() + 0x14);
    const uint32_t header = u32(raw.data() + 0x18);
    const uint32_t meta = u32(raw.data() + 0x1C);
    const uint32_t stride = u32(raw.data() + 0x20);
    const uint32_t count = u32(raw.data() + 0x28);
    if (header != 0x40 || file_size > raw.size())
        return out;
    if (flip)
        for (size_t i = 0x40; i < file_size; ++i)
            raw[i] ^= 0xFF;
    const bool key_items = std::strncmp(dat.name, "keyitems.", 9) == 0;
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = 0;
        size_t size = 0;
        if (stride > 0 && meta == 0) {
            at = 0x40 + static_cast<size_t>(i) * stride;
            size = stride;
        } else if (stride == 0 && meta == count * 8) {
            at = 0x40 + meta + u32(raw.data() + 0x40 + i * 8);
            size = u32(raw.data() + 0x40 + i * 8 + 4);
        } else {
            break;
        }
        if (at + size > file_size)
            break;
        std::string text;
        uint32_t marker = 0;
        if (block_text(raw.data() + at, size, part, text, marker))
            out->by_index[key_items ? marker : i] = keep(std::move(text));
    }
    return out;
}

const StringDat* string_dat(const char* table)
{
    if (table == nullptr)
        return nullptr;
    for (const StringDat& dat : kStringDats)
        if (same(dat.name, table))
            return &dat;
    return nullptr;
}

const Strings* strings(const char* table, uint32_t language)
{
    const StringDat* dat = string_dat(table);
    if (dat == nullptr)
        return nullptr;
    const std::lock_guard lock(g_strings_lock);
    auto& slot = g_strings[{dat->name, language}];
    if (slot == nullptr)
        slot = read_strings(*dat, language);
    return slot.get();
}

const char* string_at(const char* table, uint32_t index, uint32_t language)
{
    const Strings* found = strings(table, language_or_default(language));
    if (found == nullptr)
        return nullptr;
    const auto it = found->by_index.find(index);
    return it != found->by_index.end() ? it->second : nullptr;
}

int32_t string_index(const char* table, const char* text, uint32_t language)
{
    const Strings* found = text != nullptr ? strings(table, language_or_default(language)) : nullptr;
    if (found == nullptr)
        return -1;
    for (const auto& [index, value] : found->by_index)
        if (_stricmp(value, text) == 0)
            return static_cast<int32_t>(index);
    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Abilities and spells: Ashita's "spdata" DAT (file 81, ROM\118\114.DAT), a menu DAT whose "comm"
// chunk holds the abilities (0x30-byte records, by ability id) and "mgc_" the spells (0x64 bytes,
// by spell index), as IAbility and ISpell lay them out. Each record is scrambled: its bytes are
// rotated right by a count picked by how many bits three of them have (POLUtils'
// DecodeDataBlockMask), and those three (2, 11 and 12) aren't rotated: rotating them too made
// spells' cast times (byte 12) 4-8x too long, and their Type, MP over 255 and abilities' TPCost
// wrong. Their names and descriptions are the abilities.* and spells.* tables'.

constexpr uint32_t kSpellDat = 81;
constexpr size_t kAbilityRecord = 0x30;
constexpr size_t kSpellRecord = 0x64;
static_assert(offsetof(sdk::IAbility, Name) >= kAbilityRecord && offsetof(sdk::ISpell, Name) >= kSpellRecord);

void unscramble(uint8_t* record, size_t size)
{
    constexpr int kShift[5] = {7, 1, 6, 2, 5}; // rotated right by this, for the bit count mod 5
    const int count = std::popcount(record[2]) - std::popcount(record[11]) + std::popcount(record[12]);
    const int shift = kShift[(count < 0 ? -count : count) % 5];
    for (size_t i = 0; i < size; ++i)
        if (i != 2 && i != 11 && i != 12)
            record[i] = std::rotr(record[i], shift);
}

// A menu DAT's chunk (a 4-byte name, then u32: its type in the low 7 bits, its size over 16 above
// them, the 16-byte header counted): where its data starts, and how long it is. False if it has none.
bool menu_chunk(const std::vector<uint8_t>& raw, const char* name, size_t& at, size_t& size)
{
    for (size_t offset = 0; offset + 16 <= raw.size();) {
        const size_t length = static_cast<size_t>(u32(raw.data() + offset + 4) >> 7) * 16;
        if (length < 16 || offset + length > raw.size())
            return false;
        if (std::memcmp(raw.data() + offset, name, 4) == 0) {
            at = offset + 16;
            size = length - 16;
            return true;
        }
        offset += length;
    }
    return false;
}

// Every record is handed out by id, an unused slot too, as Ashita's are: simplelog fills its name tables by asking for
// ids 1, 2, 3, ... and stops at the first nil, so a gap blanked every weapon skill and trust past it. Lookups by name
// and by recast timer see only the named ones.
struct Actions {
    std::vector<sdk::IAbility*> abilities; // by id
    std::vector<sdk::ISpell*> spells;      // by index
    std::vector<std::unique_ptr<sdk::IAbility>> owned_abilities; // the named ones
    std::vector<std::unique_ptr<sdk::ISpell>> owned_spells;
    std::vector<std::unique_ptr<sdk::IAbility>> unused_abilities;
    std::vector<std::unique_ptr<sdk::ISpell>> unused_spells;
};
std::once_flag g_actions_once;
Actions g_actions;

// Names and descriptions from the string tables: [1] Japanese, [2] English, [0] the default language's
template <typename Entry>
bool name_entry(Entry& entry, const char* names, const char* descriptions, uint32_t index)
{
    for (uint32_t language : {1u, 2u}) {
        entry.Name[language] = string_at(names, index, language);
        entry.Description[language] = string_at(descriptions, index, language);
    }
    const bool japanese = default_language() == 1;
    const auto pick = [&](const char* (&texts)[3]) { texts[0] = japanese && texts[1] != nullptr ? texts[1] : texts[2] != nullptr ? texts[2] : texts[1]; };
    pick(entry.Name);
    pick(entry.Description);
    if (entry.Name[0] != nullptr && entry.Name[0][0] != '\0' && std::strcmp(entry.Name[0], ".") != 0) // "." is an unused slot
        return true;
    for (const char*& text : entry.Name) // an unused slot's names read "" rather than nil
        text = text != nullptr ? text : "";
    for (const char*& text : entry.Description)
        text = text != nullptr ? text : "";
    return false;
}

void load_actions()
{
    const ULONGLONG start = GetTickCount64();
    std::vector<uint8_t> raw;
    size_t at = 0, size = 0;
    if (!dats::read(kSpellDat, raw)) {
        log::write(L"Resources: the ability and spell DAT (%u) isn't there", kSpellDat);
        return;
    }
    if (menu_chunk(raw, "comm", at, size)) {
        g_actions.abilities.assign(size / kAbilityRecord, nullptr);
        for (size_t i = 0; i < size / kAbilityRecord; ++i) {
            auto ability = std::make_unique<sdk::IAbility>();
            std::memset(ability.get(), 0, sizeof(sdk::IAbility));
            std::memcpy(ability.get(), raw.data() + at + i * kAbilityRecord, kAbilityRecord);
            unscramble(reinterpret_cast<uint8_t*>(ability.get()), kAbilityRecord);
            g_actions.abilities[i] = ability.get();
            const bool named = name_entry(*ability, "abilities.names", "abilities.descriptions", static_cast<uint32_t>(i));
            (named && ability->Id == i ? g_actions.owned_abilities : g_actions.unused_abilities).push_back(std::move(ability));
        }
    }
    if (menu_chunk(raw, "mgc_", at, size)) {
        g_actions.spells.assign(size / kSpellRecord, nullptr);
        for (size_t i = 0; i < size / kSpellRecord; ++i) {
            auto spell = std::make_unique<sdk::ISpell>();
            std::memset(spell.get(), 0, sizeof(sdk::ISpell));
            std::memcpy(spell.get(), raw.data() + at + i * kSpellRecord, kSpellRecord);
            unscramble(reinterpret_cast<uint8_t*>(spell.get()), kSpellRecord);
            g_actions.spells[i] = spell.get();
            const bool named = name_entry(*spell, "spells.names", "spells.descriptions", static_cast<uint32_t>(i));
            (named && spell->Index == i ? g_actions.owned_spells : g_actions.unused_spells).push_back(std::move(spell));
        }
    }
    log::write(L"Resources: %zu abilities and %zu spells read (%zu and %zu more unused slots) in %llu ms",
               g_actions.owned_abilities.size(), g_actions.owned_spells.size(), g_actions.unused_abilities.size(),
               g_actions.unused_spells.size(), GetTickCount64() - start);
}

Actions& actions()
{
    std::call_once(g_actions_once, &load_actions);
    return g_actions;
}

template <typename Entry>
Entry* by_name(const std::vector<std::unique_ptr<Entry>>& entries, const char* name, uint32_t language)
{
    if (name == nullptr)
        return nullptr;
    const uint32_t which = language_or_default(language);
    for (const auto& entry : entries)
        if (entry->Name[which] != nullptr && _stricmp(entry->Name[which], name) == 0)
            return entry.get();
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Entities by index and id.

const FFXI::entity_t* entity_at(uint32_t index)
{
    const game::Addresses at = game::addresses();
    if (at.entities == 0 || index >= at.entity_count)
        return nullptr;
    return reinterpret_cast<FFXI::entity_t**>(at.entities)[index];
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Status icons: Ashita's status_icons DATs (file 87, ROM\119\57.DAT, in English; 12 in Japanese), a 0x1800-byte
// record per status. A record's first 0x280 bytes are rotated right by an amount of its own (1 or 7 seen: the one its
// string block reads with): u16 the status id (the record's index but for a few), a byte with 0x80 when it can be
// cancelled, a byte 0xFF when its timer is hidden, then its strings (the description). The icon follows unscrambled,
// as in the item DATs: u32 its size, then its type, its 16-byte name and the bitmap (a DIB).

constexpr uint32_t kStatusDats[2] = {87, 12}; // English, Japanese
constexpr size_t kStatusStride = 0x1800;

struct StatusIcons {
    std::vector<std::unique_ptr<sdk::IStatusIcon>> by_index; // null where a record has nothing
    std::map<uint16_t, sdk::IStatusIcon*> by_id;
};

// The icon's bitmap as Ashita hands it: a BMP file (the DIB after a file header), ImageSize the icon's size in the DAT
// plus the header's 14 bytes (what addons written for Ashita expect).
void fill_icon(sdk::IStatusIcon& icon, const uint8_t* record)
{
    const uint32_t size = u32(record + kIconOffset);
    constexpr size_t kNamed = 1 + 16; // its type and name, before the DIB
    if (size <= kNamed + sizeof(BITMAPINFOHEADER) || kIconOffset + 4 + size > kStatusStride)
        return;
    const uint8_t* image = record + kIconOffset + 4;
    const uint8_t* dib = image + kNamed;
    size_t dib_size = size - kNamed;
    BITMAPINFOHEADER h;
    std::memcpy(&h, dib, sizeof(h));
    const DWORD colours = h.biBitCount <= 8 ? (h.biClrUsed != 0 ? h.biClrUsed : 1u << h.biBitCount) : h.biClrUsed;
    const DWORD masks = h.biCompression == BI_BITFIELDS && h.biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    // The size a record gives can be short of its pixels (the HD pack's DATs say 32x32 at 8 bits a pixel of icons that are
    // 32 bits): the game goes by the header, and the rest of the icon is in the record, so the bitmap is what the header
    // says where the record and Bitmap have room for it (an icon cut at the size shows as its bottom rows only)
    if ((h.biCompression == BI_RGB || h.biCompression == BI_BITFIELDS) && h.biPlanes == 1 && h.biWidth > 0 &&
        h.biHeight != 0 && h.biBitCount != 0 && h.biSize >= sizeof(BITMAPINFOHEADER)) {
        const uint64_t row = (static_cast<uint64_t>(h.biWidth) * h.biBitCount + 31) / 32 * 4;
        const uint64_t rows = h.biHeight < 0 ? -static_cast<int64_t>(h.biHeight) : h.biHeight;
        const uint64_t need = h.biSize + masks + static_cast<uint64_t>(colours) * 4 + row * rows;
        const size_t room = (std::min)(kStatusStride - (kIconOffset + 4 + kNamed), sizeof(icon.Bitmap) - sizeof(BITMAPFILEHEADER));
        if (need > dib_size && need <= room)
            dib_size = static_cast<size_t>(need);
    }
    if (sizeof(BITMAPFILEHEADER) + dib_size > sizeof(icon.Bitmap))
        return;
    BITMAPFILEHEADER f{};
    f.bfType = 0x4D42; // "BM"
    f.bfSize = static_cast<DWORD>(sizeof(f) + dib_size);
    f.bfOffBits = static_cast<DWORD>(sizeof(f) + h.biSize + masks + colours * 4);
    icon.ImageSize = static_cast<uint32_t>(kNamed + dib_size + sizeof(f));
    icon.ImageType = image[0];
    std::memcpy(icon.ImageName, image + 1, sizeof(icon.ImageName));
    std::memcpy(icon.Bitmap, &f, sizeof(f));
    std::memcpy(icon.Bitmap + sizeof(f), dib, dib_size);
}

StatusIcons load_status_icons()
{
    StatusIcons icons;
    const ULONGLONG start = GetTickCount64();
    for (int language = 0; language < 2; ++language) {
        const bool japanese = language == 1;
        std::vector<uint8_t> raw;
        if (!dats::read(kStatusDats[language], raw)) {
            log::write(L"Resources: the status icon DAT (%u) isn't there", kStatusDats[language]);
            continue;
        }
        const size_t records = raw.size() / kStatusStride;
        if (!japanese)
            icons.by_index.resize(records);
        for (size_t index = 0; index < records && index < icons.by_index.size(); ++index) {
            uint8_t* record = raw.data() + index * kStatusStride;
            // the rotation its string block reads with
            uint8_t info[kIconOffset];
            bool found = false;
            for (int n = 0; n < 8 && !found; ++n) {
                for (size_t i = 0; i < kIconOffset; ++i)
                    info[i] = static_cast<uint8_t>((record[i] >> n) | (record[i] << (8 - n)));
                found = string_block(info, kIconOffset, 4);
            }
            if (!found)
                continue;
            const std::vector<std::string> strings = record_strings(info, kIconOffset, 4);
            sdk::IStatusIcon* icon = icons.by_index[index].get();
            if (icon == nullptr) {
                if (japanese)
                    continue;
                icons.by_index[index] = std::make_unique<sdk::IStatusIcon>();
                icon = icons.by_index[index].get();
                std::memset(icon, 0, sizeof(*icon));
                icon->Index = static_cast<uint16_t>(index);
                icon->Id = u16(info);
                icon->CanCancel = (info[2] & 0x80) != 0 ? 1 : 0;
                icon->HideTimer = info[3] != 0 ? 1 : 0;
                fill_icon(*icon, record);
                icons.by_id.emplace(icon->Id, icon);
            }
            if (!strings.empty() && !strings[0].empty())
                icon->Description[japanese ? 1 : 2] = keep(strings[0]);
        }
    }
    const bool japanese = default_language() == 1;
    size_t count = 0;
    for (auto& icon : icons.by_index) {
        if (icon == nullptr)
            continue;
        ++count;
        icon->Description[0] = japanese && icon->Description[1] != nullptr ? icon->Description[1] : icon->Description[2] != nullptr ? icon->Description[2] : icon->Description[1];
    }
    log::write(L"Resources: %zu status icons read in %llu ms", count, GetTickCount64() - start);
    return icons;
}

const StatusIcons& status_icons()
{
    static const StatusIcons icons = load_status_icons();
    return icons;
}

// An addon's GetItemById or GetItemByName goes through the resource managers' methods, as a plugin's does, so a plugin
// that hooks them answers it too: cexidats hooks both and gives CatsEyeXI's own items (30720-57343) from its cache. While
// one is asked (`addon`), VanaCore's own methods give the addon's item (no Bitmap), and say they were reached and what
// they gave, so a hook that answered can be told from one that passed it on.
struct AddonLookup {
    bool addon = false;
    bool reached = false;
    const void* made = nullptr;

    sdk::IItem* reach(sdk::IItem* item)
    {
        reached = true;
        made = item;
        return item;
    }
};
thread_local AddonLookup t_lookup;

// Both versions' objects (host.cpp's), set when they're built.
Links* g_links[2] = {};

// `call(links)` as an addon's lookup through each version's resource manager in turn, until a plugin's hook answers it;
// VanaCore's own item when none does, `direct()` when there are no objects.
template <class Call, class Direct>
const void* addon_lookup(Call&& call, Direct&& direct)
{
    const void* item = nullptr;
    bool asked = false;
    for (Links* links : g_links) {
        if (links == nullptr || links->resources == nullptr)
            continue;
        const AddonLookup outer = t_lookup;
        t_lookup = AddonLookup{true, false, nullptr};
        item = call(*links);
        const AddonLookup seen = t_lookup;
        t_lookup = outer;
        asked = true;
        if (!seen.reached || item != seen.made)
            return item;
    }
    return asked ? item : direct();
}

struct ResourcesImpl {
    static sdk::IStatusIcon* GetStatusIconByIndex(void*, uint16_t index)
    {
        const auto& icons = status_icons().by_index;
        return index < icons.size() ? icons[index].get() : nullptr;
    }
    static sdk::IStatusIcon* GetStatusIconById(void*, uint16_t id)
    {
        const auto& icons = status_icons().by_id;
        const auto found = icons.find(id);
        return found != icons.end() ? found->second : nullptr;
    }
    static sdk::IAbility* GetAbilityById(void*, uint32_t id)
    {
        const auto& abilities = actions().abilities;
        return id < abilities.size() ? abilities[id] : nullptr;
    }
    static sdk::IAbility* GetAbilityByName(void*, const char* name, uint32_t language)
    {
        return by_name(actions().owned_abilities, name, language);
    }
    // The ability with that recast timer: a job ability's (ids from 0x200) before any other's
    static sdk::IAbility* GetAbilityByTimerId(void*, uint32_t timer)
    {
        sdk::IAbility* other = nullptr;
        for (const auto& ability : actions().owned_abilities) {
            if (ability->RecastTimerId != timer)
                continue;
            if (ability->Id >= 0x200)
                return ability.get();
            other = other != nullptr ? other : ability.get();
        }
        return other;
    }
    static sdk::ISpell* GetSpellById(void*, uint32_t id)
    {
        const auto& spells = actions().spells;
        return id < spells.size() ? spells[id] : nullptr;
    }
    static sdk::ISpell* GetSpellByName(void*, const char* name, uint32_t language)
    {
        return by_name(actions().owned_spells, name, language);
    }
    // A spell's or ability's range (or its area's) in yalms: the DAT's byte is the distance itself (2-12), and 15 is
    // none (the caster alone), as Windower's resources read the same bytes (blucheck shows them)
    static uint32_t yalms(int8_t raw) { return raw < 0 || raw >= 15 ? 0 : static_cast<uint32_t>(raw); }
    static uint32_t GetSpellRange(void* c, uint32_t id, bool area)
    {
        const sdk::ISpell* spell = GetSpellById(c, id);
        return spell != nullptr ? yalms(area ? static_cast<int8_t>(spell->AreaRange) : spell->Range) : 0;
    }
    static uint32_t GetAbilityRange(void* c, uint32_t id, bool area)
    {
        const sdk::IAbility* ability = GetAbilityById(c, id);
        return ability != nullptr ? yalms(area ? static_cast<int8_t>(ability->AreaRange) : ability->Range) : 0;
    }
    static sdk::IItem* GetItemById(void*, uint32_t id)
    {
        if (!t_lookup.addon)
            return item_by_id(id);
        return t_lookup.reach(item_by_id(id, false));
    }
    static sdk::IItem* GetItemByName(void*, const char* name, uint32_t language)
    {
        if (name == nullptr)
            return t_lookup.addon ? t_lookup.reach(nullptr) : nullptr;
        if (!t_lookup.addon)
            return item_by_name(name, language_or_default(language));
        return t_lookup.reach(item_by_name(name, language_or_default(language), false));
    }
    static uint32_t GetEntityCount(void*) { return game::addresses().entity_count; }
    static uint32_t GetEntityIndexById(void*, uint32_t id)
    {
        const uint32_t count = game::addresses().entity_count;
        for (uint32_t i = 0; i < count; ++i)
            if (const FFXI::entity_t* e = entity_at(i); e != nullptr && e->ServerId == id)
                return i;
        return 0;
    }
    static uint32_t GetEntityIdByIndex(void*, uint32_t index)
    {
        const FFXI::entity_t* e = entity_at(index);
        return e != nullptr ? e->ServerId : 0;
    }
    static const char* GetEntityNameByIndex(void*, uint32_t index)
    {
        const FFXI::entity_t* e = entity_at(index);
        return e != nullptr ? reinterpret_cast<const char*>(e->Name) : nullptr;
    }
    static const char* GetEntityNameById(void* c, uint32_t id)
    {
        const uint32_t index = GetEntityIndexById(c, id);
        const FFXI::entity_t* e = entity_at(index);
        return e != nullptr && e->ServerId == id ? reinterpret_cast<const char*>(e->Name) : nullptr;
    }
    static const char* GetString(void*, const char* table, uint32_t index) { return string_at(table, index, 0); }
    static const char* GetString(void*, const char* table, uint32_t index, uint32_t language)
    {
        return string_at(table, index, language);
    }
    static int32_t GetString(void*, const char* table, const char* text) { return string_index(table, text, 0); }
    static int32_t GetString(void*, const char* table, const char* text, uint32_t language)
    {
        return string_index(table, text, language);
    }
    static int32_t GetStringLength(void*, const char* table, uint32_t index)
    {
        const char* text = string_at(table, index, 0);
        return text != nullptr ? static_cast<int32_t>(std::strlen(text)) : -1;
    }
    static int32_t GetStringLength(void*, const char* table, uint32_t index, uint32_t language)
    {
        const char* text = string_at(table, index, language);
        return text != nullptr ? static_cast<int32_t>(std::strlen(text)) : -1;
    }
    static uint32_t GetFilePath(void*, uint32_t file, char* buffer, uint32_t size)
    {
        const std::wstring path = dats::path(file);
        if (path.empty())
            return 0;
        const int length = WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (buffer != nullptr && size > 0)
            WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, buffer, static_cast<int>(size), nullptr, nullptr);
        return length > 0 ? static_cast<uint32_t>(length - 1) : 0;
    }
};

} // namespace

const char* resource_string(const char* table, uint32_t index, uint32_t language)
{
    return table != nullptr ? string_at(table, index, language) : nullptr;
}

int32_t resource_string_index(const char* table, const char* text, uint32_t language)
{
    return table != nullptr ? string_index(table, text, language) : -1;
}

const char* resource_item_name(uint32_t id)
{
    return item_name(id);
}

const char* resource_item_description(uint32_t id)
{
    const sdk::IItem* item = item_by_id(id, false);
    return item != nullptr ? item->Description[2] : nullptr;
}

const void* resource_addon_item(uint32_t id)
{
    return addon_lookup(
        [id](const Links& links) -> const void* {
            if (links.version == 416)
                return static_cast<sdk::v416::IResourceManager*>(links.resources)->GetItemById(id);
            return static_cast<sdk::v430::IResourceManager*>(links.resources)->GetItemById(id);
        },
        [id] { return item_by_id(id, false); });
}

const void* resource_addon_item_by_name(const char* name, uint32_t language)
{
    if (name == nullptr)
        return nullptr;
    return addon_lookup(
        [name, language](const Links& links) -> const void* {
            if (links.version == 416)
                return static_cast<sdk::v416::IResourceManager*>(links.resources)->GetItemByName(name, language);
            return static_cast<sdk::v430::IResourceManager*>(links.resources)->GetItemByName(name, language);
        },
        [name, language] { return item_by_name(name, language_or_default(language), false); });
}

std::string resource_item_icon(uint32_t id)
{
    return item_icon(id);
}

// Whether `item` (an address resource_addon_item gave) is VanaCore's own addon item, cut short before its Bitmap. One a
// plugin's hook gave (cexidats' cache for CatsEyeXI's own ids) is whole, its Bitmap and ImageSize its own.
bool resource_item_is_stub(uintptr_t item)
{
    if (item == 0 || !g_items.ready.load(std::memory_order_acquire))
        return false;
    const uint32_t id = static_cast<const sdk::IItem*>(reinterpret_cast<const void*>(item))->Id;
    return id < 65536 && g_items.addon_by_id[id].load(std::memory_order_acquire) == reinterpret_cast<const sdk::IItem*>(item);
}

void build_resources(Links& a, Links& b)
{
    a.resources = make<sdk::v416::ResourceManagerObject, ResourcesImpl>(&a);
    b.resources = make<sdk::v430::ResourceManagerObject, ResourcesImpl>(&b);
    g_links[0] = &a;
    g_links[1] = &b;
}

} // namespace vanacore::ashita
