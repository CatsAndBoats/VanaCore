// The game state Windower keeps from the server's packets, which its plugins read through the
// pointer table PacketStreamHandler slot 4 returns (FFXIDB: logged in, zone, the player's id and
// index; StatusTimer: buffs, name, the server's clock and more). Hook.dll keeps it inside its plugin
// manager and fills it in its incoming packet handler; this is that handler's behaviour, packet by
// packet, over a block laid out as Hook's (so each pointer lands where the plugins expect). What
// Hook does beyond the block (calling plugins' AddItem/RemoveItem, timing three buffs, asking the
// server for an update after zoning) isn't done.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "objects.hpp"

namespace vanacore::windower {
namespace {

// Offsets into the block, as in Hook's plugin manager.
constexpr std::size_t kSize = 0x1d4a8;
constexpr std::size_t kMerits = 0x1c;       // a byte per merit (its id >> 1)
constexpr std::size_t kKeyItems = 0x101c;   // a byte per key item
constexpr std::size_t kItems = 0x301c;      // 32 bags x 81 slots of Item
constexpr std::size_t kBagMax = 0x1c51c;    // per bag: its size - 1
constexpr std::size_t kBagCount = 0x1c53c;  // per bag: items in it
constexpr std::size_t kBagOpen = 0x1c55c;   // 11 bags: usable
constexpr std::size_t kTreasure = 0x1c57c;  // 10 x 0x30
constexpr std::size_t kEquipIndex = 0x1c75c; // 16 equipment slots: inventory index
constexpr std::size_t kEquipBag = 0x1c76c;  // ...and bag (dword)
constexpr std::size_t kParty = 0x1c7ac;
constexpr std::size_t kPlayer = 0x1cacc;    // id, index, name[16], ..., buffs at +0x1c
constexpr std::size_t kBuffs = 0x1cae8;     // 32 words, 0xFF none
constexpr std::size_t kZone = 0x1cc24;      // logged in, zone, weather, in a Mog House
constexpr std::size_t kJobExtra = 0x1cc30;  // 0x044, main job; 0x1ce38 sub job
constexpr std::size_t kServerTime = 0x1d040;
constexpr std::size_t kJobPoints = 0x1d044;
constexpr std::size_t kMissions = 0x1d3d4;
constexpr std::size_t kJobs = 0x1d3f4;      // a flag per job; levels (dwords) at 0x1d40c
constexpr std::size_t kJobLevels = 0x1d40c;

constexpr std::size_t kBags = 32;
constexpr std::size_t kSlots = 81;
constexpr std::size_t kItemSize = 0x28;
constexpr std::uint8_t kEquipped = 5;

// The table plugins get: the parts of the block, in Hook's order.
constexpr std::size_t kParts[] = {kZone,      kServerTime, kPlayer, kMerits,   kKeyItems,  kItems,    kTreasure,
                                  kEquipIndex, kParty,     kJobExtra, kEquipBag, kJobPoints, kMissions, kJobs};

std::vector<std::uint8_t> g_block(kSize, 0);
const void* g_table[std::size(kParts)] = {};

std::uint8_t* at(std::size_t offset)
{
    return g_block.data() + offset;
}

template <typename T>
T get(const std::uint8_t* from)
{
    T value;
    std::memcpy(&value, from, sizeof(value));
    return value;
}

template <typename T>
void put(std::size_t offset, T value)
{
    std::memcpy(at(offset), &value, sizeof(value));
}

// An item slot: +0 id (word), +2 its index in the bag, +4 count, +8 (bazaar price), +0xC status
// (5 equipped), +0xD 24 bytes of extra data.
std::uint8_t* item(std::size_t bag, std::size_t slot)
{
    return at(kItems + (bag * kSlots + slot) * kItemSize);
}

void reset()
{
    std::fill(g_block.begin(), g_block.end(), std::uint8_t{0});
    for (std::size_t i = 0; i < 32; ++i)
        put<std::uint16_t>(kBuffs + i * 2, 0xFF);
    for (std::size_t bag = 0; bag < kBags; ++bag) {
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            std::uint8_t* entry = item(bag, slot);
            const std::uint16_t id = slot == 0 ? 0xFFFF : 0; // slot 0 is gil's
            std::memcpy(entry, &id, sizeof(id));
            entry[2] = static_cast<std::uint8_t>(slot);
        }
    }
    for (std::size_t i = 0; i < 10; ++i)
        *at(kTreasure + i * 0x30 + 0x2C) = static_cast<std::uint8_t>(i);
    *at(0x1ce3c) = 1;
}

// An item's status changing: unequipped, it's off the equipment list.
void set_status(std::uint8_t* entry, std::uint8_t status, std::size_t bag)
{
    if (status == 0 && entry[0xC] == kEquipped) {
        for (std::size_t slot = 0; slot < 16; ++slot) {
            if (*at(kEquipIndex + slot) == entry[2] && get<std::uint32_t>(at(kEquipBag + slot * 4)) == bag) {
                *at(kEquipIndex + slot) = 0;
                put<std::uint32_t>(kEquipBag + slot * 4, 0);
            }
        }
    }
    entry[0xC] = status;
}

void remove_item(std::uint8_t* entry, std::size_t bag)
{
    if (entry[2] == 0)
        return;
    if (entry[0xC] == kEquipped)
        set_status(entry, 0, bag);
    const std::uint16_t id = get<std::uint16_t>(entry);
    const std::uint8_t index = entry[2];
    std::memset(entry, 0, kItemSize);
    entry[2] = index;
    if (id != 0)
        --*at(kBagCount + bag);
}

void on_packet(const std::uint8_t* p, std::size_t size)
{
    const std::uint16_t id = get<std::uint16_t>(p) & 0x1FF;
    // Hook reads fields well past the end of short packets (the game's buffer is there); here,
    // anything past the packet reads as zero.
    std::uint8_t padded[0x200] = {};
    std::memcpy(padded, p, std::min<std::size_t>(size, sizeof(padded)));
    p = padded;
    switch (id) {
    case 0x00A: // zone in
        *at(kZone) = 1;
        std::memset(at(kTreasure), 0, 10 * 0x30);
        put<std::uint32_t>(0x1cb60, p[0xB4]); // main job
        put<std::uint32_t>(0x1cb68, p[0xB7]); // sub job
        put<std::uint32_t>(0x1cb2c, get<std::uint32_t>(p + 0xE8)); // max HP
        put<std::uint32_t>(0x1cb38, get<std::uint32_t>(p + 0xEC)); // max MP
        std::memcpy(at(kPlayer + 8), p + 0x84, 16);  // name
        put<std::uint32_t>(kPlayer, get<std::uint32_t>(p + 4));
        put<std::uint32_t>(kPlayer + 4, get<std::uint16_t>(p + 8));
        put<std::uint16_t>(kZone + 2, get<std::uint16_t>(p + 0x30));
        put<std::uint32_t>(kZone + 4, p[0x68]); // weather
        *at(kZone + 8) = p[0x80] == 1;         // in a Mog House
        break;
    case 0x00B: // zone out (logout: everything goes)
        if (p[4] == 1)
            reset();
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            remove_item(item(3, slot), 3);   // temporary items
            remove_item(item(17, slot), 17); // recycle bin
        }
        break;
    case 0x01B: { // job info
        std::memcpy(at(0x1cb48), p + 0x48, 0x18);
        const std::uint32_t unlocked = get<std::uint32_t>(p + 0xC);
        const std::uint32_t flags = get<std::uint32_t>(p + 0x68);
        for (std::size_t job = 0; job < 24; ++job) {
            *at(0x1cc0a + job) = (unlocked >> job) & 1;
            *at(kJobs + job) = (flags >> job) & 1;
            put<std::uint32_t>(kJobLevels + job * 4, p[0x6C + job]);
        }
        break;
    }
    case 0x01C: // bag sizes
        for (std::size_t bag = 0; bag < 32; ++bag)
            *at(kBagMax + bag) = p[4 + bag] != 0 ? static_cast<std::uint8_t>(p[4 + bag] - 1) : 0;
        for (std::size_t bag = 0; bag < 11; ++bag)
            *at(kBagOpen + bag) = get<std::int16_t>(p + 0x24 + bag * 2) > 1;
        if (*at(kZone + 8) == 0) { // outside a Mog House: safe, storage, locker, safe 2 aren't open
            *at(kBagOpen + 1) = 0;
            *at(kBagOpen + 2) = 0;
            *at(kBagOpen + 4) = 0;
            *at(kBagOpen + 9) = 0;
        }
        break;
    case 0x01E: { // item count
        const std::size_t bag = p[8], slot = p[9];
        if (bag >= kBags || slot >= kSlots)
            break;
        std::uint8_t* entry = item(bag, slot);
        std::memcpy(entry + 4, p + 4, 4);
        set_status(entry, p[0xA], bag);
        if (get<std::uint32_t>(entry + 4) == 0)
            remove_item(entry, bag);
        break;
    }
    case 0x01F: { // item assign
        const std::size_t bag = p[0xA], slot = p[0xB];
        if (bag >= kBags || slot >= kSlots)
            break;
        std::uint8_t* entry = item(bag, slot);
        const std::uint16_t item_id = get<std::uint16_t>(p + 8);
        if (entry[2] != 0) {
            if (get<std::uint16_t>(entry) == 0) {
                if (item_id != 0)
                    ++*at(kBagCount + bag);
            } else if (item_id == 0) {
                remove_item(entry, bag);
            }
        }
        std::memcpy(entry + 4, p + 4, 4);
        std::memcpy(entry, &item_id, 2);
        set_status(entry, p[0xC], bag);
        break;
    }
    case 0x020: { // item update
        const std::size_t bag = p[0xE], slot = p[0xF];
        if (bag >= kBags || slot >= kSlots)
            break;
        std::uint8_t* entry = item(bag, slot);
        const std::uint16_t item_id = get<std::uint16_t>(p + 0xC);
        if (slot != 0) {
            if (get<std::uint16_t>(entry) == 0) {
                if (static_cast<std::int16_t>(item_id) > 0)
                    ++*at(kBagCount + bag);
            } else if (item_id == 0) {
                remove_item(entry, bag);
            }
        }
        std::memcpy(entry, &item_id, 2);
        std::memcpy(entry + 4, p + 4, 8);
        set_status(entry, p[0x10], bag);
        std::memcpy(entry + 0xD, p + 0x11, 0x18);
        if (item_id >= 0x201 && item_id <= 0x205) { // a linkshell
            if (p[0x10] == 0x13) {
                // Its name: 20 characters of 6 bits from byte 0x1A.
                static const char kLetters[] = "\0abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
                for (std::size_t i = 0; i < 20; ++i) {
                    const std::size_t bit = i * 6;
                    const unsigned pair = static_cast<unsigned>(p[0x1A + bit / 8]) << 8 | p[0x1A + bit / 8 + 1];
                    const unsigned code = (pair >> (10 - bit % 8)) & 0x3F;
                    *at(0x1cb6d + i) = code < sizeof(kLetters) - 1 ? static_cast<std::uint8_t>(kLetters[code]) : 0;
                }
                *at(0x1cb81) = p[0xF];
            } else if (p[0x10] == 0 && p[0xF] == *at(0x1cb81)) {
                *at(0x1cb6d) = 0;
            }
        }
        break;
    }
    case 0x037: // player update
        put<std::uint32_t>(0x1cae4, p[0x30]);
        *at(0x1c567) = p[0x5C] & 1;
        *at(0x1c568) = (p[0x5C] >> 1) & 1;
        *at(0x1c569) = (p[0x5C] >> 3) & 1;
        *at(0x1c56a) = (p[0x5C] >> 4) & 1;
        *at(0x1c56b) = (p[0x5C] >> 5) & 1;
        *at(0x1c56c) = (p[0x5C] >> 6) & 1;
        break;
    case 0x044: { // job extra data (blue magic, attachments...)
        const std::size_t base = p[5] != 0 ? 0x1ce38 : kJobExtra;
        put<std::uint32_t>(base, p[4]);
        const std::size_t length = size > 8 ? std::min<std::size_t>(size - 8, 0x200) : 0;
        std::memcpy(at(base + 5), p + 8, length);
        break;
    }
    case 0x050: { // equip
        const std::size_t slot = p[5], bag = p[6], index = p[4];
        if (slot >= 16 || bag >= kBags || index >= kSlots)
            break;
        *at(kEquipIndex + slot) = p[4];
        put<std::uint32_t>(kEquipBag + slot * 4, p[6]);
        item(bag, index)[0xC] = kEquipped;
        break;
    }
    case 0x055: { // key items
        const std::size_t type = p[0x84];
        if (type >= 16)
            break;
        for (std::size_t i = 0; i < 64; ++i)
            for (std::size_t bit = 0; bit < 8; ++bit)
                *at(kKeyItems + (type * 64 + i) * 8 + bit) = (p[4 + i] >> bit) & 1;
        break;
    }
    case 0x056: { // missions
        const std::uint16_t type = get<std::uint16_t>(p + 0x24);
        if (type == 0xFFFF) {
            std::memcpy(at(kMissions), p + 8, 12);
            *at(0x1d3f0) = p[0x18] & 0xF;
            *at(0x1d3f1) = static_cast<std::uint8_t>(static_cast<std::int8_t>(p[0x18]) >> 4);
            *at(0x1d3f2) = p[0x19] & 0xF;
            std::memcpy(at(0x1d3e8), p + 0x1C, 8);
        } else if (type == 0x80) {
            std::memcpy(at(0x1d3e0), p + 0x18, 8);
        }
        break;
    }
    case 0x057: // weather
        put<std::uint32_t>(kZone + 4, p[8]);
        break;
    case 0x061: { // character stats
        std::memcpy(at(0x1cb2c), p + 4, 4);
        std::memcpy(at(0x1cb38), p + 8, 4);
        const std::size_t main = p[0xC];
        put<std::uint32_t>(0x1cb60, p[0xC]);
        put<std::uint32_t>(0x1cb68, p[0xE]);
        *at(0x1cb64) = p[0xD];
        *at(0x1cb6c) = p[0xF];
        *at(0x1cc22) = p[0x46];
        *at(0x1cb44) = p[0x50];
        *at(0x1cc08) = p[0x52];
        *at(0x1cc09) = static_cast<std::uint8_t>(p[0x55] + p[0xD]);
        if (main < 24) {
            put<std::uint32_t>(kJobLevels + main * 4, p[0x65]);
            *at(kJobs + main) = p[0x66] & 1;
        }
        break;
    }
    case 0x062: // skills
        std::memcpy(at(0x1cb82), p + 0x80, 0x80);
        break;
    case 0x063: // player status
        if (p[4] == 5)
            std::memcpy(at(0x1d344), p + 0xC, 0x90);
        else if (p[4] == 9 && get<std::uint16_t>(p + 0xA) != 0)
            std::memcpy(at(kBuffs), p + 8, 0x40);
        break;
    case 0x08C: // merits
        for (std::size_t offset = 8; offset + 4 < size; offset += 4) {
            const std::size_t merit = get<std::uint16_t>(p + offset) >> 1;
            if (merit < 0x1000)
                *at(kMerits + merit) = p[offset + 3];
        }
        break;
    case 0x08D: // job points
        for (std::size_t offset = 4; offset < size; offset += 4) {
            const std::size_t point = get<std::uint16_t>(p + offset);
            if (point < 0x300)
                *at(kJobPoints + point) = p[offset + 3] >> 2;
        }
        break;
    case 0x0D2: { // treasure pool: item found
        const std::size_t index = p[0x14];
        if (index >= 10)
            break;
        std::uint8_t* entry = at(kTreasure + index * 0x30);
        std::memcpy(entry + 0x20, p + 0x12, 2);
        std::memcpy(entry + 0x24, p + 8, 4);
        std::memcpy(entry + 0x28, p + 0x18, 4);
        std::memcpy(entry + 2, p + 0x10, 2);
        entry[0x2C] = p[0x14];
        entry[0] = get<std::uint16_t>(p + 0x10) != 0;
        break;
    }
    case 0x0D3: { // treasure pool: lot
        const std::size_t index = p[0x14];
        if (index >= 10)
            break;
        std::uint8_t* entry = at(kTreasure + index * 0x30);
        entry[0] = p[0x15] == 0;
        if (p[0x15] == 0) {
            std::memcpy(entry + 4, p + 0xE, 2);
            std::memcpy(entry + 8, p + 4, 4);
            const std::uint32_t lot = get<std::uint16_t>(p + 0xC);
            std::memcpy(entry + 0xC, &lot, 4);
            std::memcpy(entry + 0x10, p + 0x16, 0x10);
        } else {
            std::memset(entry + 2, 0, 0x1E);
            std::memset(entry + 0x20, 0, 8);
            put<std::uint32_t>(kTreasure + index * 0x30 + 0x28, 0);
        }
        break;
    }
    case 0x0DF: { // character update
        if (get<std::uint32_t>(p + 4) != get<std::uint32_t>(at(kPlayer)))
            break;
        std::memcpy(at(0x1cb28), p + 8, 4);    // HP
        std::memcpy(at(0x1cb34), p + 0xC, 4);  // MP
        std::memcpy(at(0x1cb40), p + 0x10, 4); // TP
        *at(0x1cb30) = p[0x16];
        *at(0x1cb3c) = p[0x17];
        const std::uint32_t main = get<std::uint32_t>(at(0x1cb60));
        if (main < 24) {
            put<std::uint32_t>(kJobLevels + main * 4, p[0x24]);
            *at(kJobs + main) = p[0x25] & 1;
        }
        break;
    }
    default:
        break;
    }
}

struct Init {
    Init()
    {
        reset();
        for (std::size_t i = 0; i < std::size(kParts); ++i)
            g_table[i] = at(kParts[i]);
    }
} g_init;

} // namespace

const void* const* state_pointers()
{
    return g_table;
}

void state_on_received(const std::uint8_t* buffer, std::size_t size)
{
    constexpr std::size_t kHeader = 0x1C;
    if (buffer == nullptr || size < kHeader)
        return;
    put<std::uint32_t>(kServerTime, get<std::uint32_t>(buffer + 8));
    for (std::size_t offset = kHeader; offset + 4 <= size;) {
        const std::size_t length = static_cast<std::size_t>(get<std::uint16_t>(buffer + offset) >> 9) * 4;
        if (length < 4 || offset + length > size)
            break;
        on_packet(buffer + offset, length);
        offset += length;
    }
}

void state_world(std::uint32_t& server_time, std::uint32_t& weather)
{
    server_time = get<std::uint32_t>(at(kServerTime));
    weather = get<std::uint32_t>(at(kZone + 4));
}

} // namespace vanacore::windower
