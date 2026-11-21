#include "others.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <vector>

#include "game.hpp"
#include "log.hpp"
#include "packets.hpp"

namespace vanacore::others {
namespace {

constexpr std::size_t kHeader = 0x1C; // the buffer's header

// The packets: a zone entered, an update about a player, one about another entity, a trade asked for, the party and
// alliance list, one member of it. Sent: a player's full update asked for.
constexpr std::uint16_t kZoneIn = 0x00A, kPlayer = 0x00D, kEntity = 0x00E, kTrade = 0x021, kGroup = 0x0C8, kMember = 0x0DD;
constexpr std::uint16_t kAsk = 0x016, kAction = 0x028;

// 0x028, bit-packed from its 5th byte: the actor (bit 40, 32 bits), the category (82, 4) and its argument (86, 32). A
// trust's summoning begun (category 8, "cafa") or cast (4, spells 896-1023).
constexpr std::size_t kActor = 40, kCategory = 82, kArgument = 86, kArgumentEnd = 118;
constexpr std::uint32_t kBegin = 8, kCast = 4, kTrustBegun = 0x61666163, kFirstTrust = 896, kLastTrust = 1023;
// How long a trust that comes is taken as one of yours (kept in while its party list is on its way): from your trust's
// summoning begun (its casting, then the server's next update), its cast ended, and a zone entered (yours come with you)
constexpr std::uint64_t kSummoning = 12000, kSummoned = 3000, kZoning = 10000;

// 0x00A, 0x00D, 0x00E and 0x021: the entity's id (0x04) and target index (0x08). 0x00D and 0x00E: what the update holds
// (0x0A: 0x20 the entity gone; else 0x01 the position, 0x02 the claim, 0x04 HP and flags, 0x08 the name, 0x10 the
// model, 0x40 a dynamic entity's name and model together).
constexpr std::size_t kId = 0x04, kIndex = 0x08, kSent = 0x0A;
constexpr std::uint8_t kGone = 0x20, kParts = 0x5F, kName = 0x08;
// 0x00D: the player's pet's index (in every update but the one saying it's gone)
constexpr std::size_t kPet = 0x3C;
// 0x00E: the first byte of its fourth set of flags (0x01 a trust, in every update about one)
constexpr std::size_t kFlags = 0x28;
constexpr std::uint8_t kTrust = 0x01;
// 0x0C8: the party and alliance, 20 members from 0x08, 12 bytes each, the id first. 0x0DD: one member, its id at 0x04.
constexpr std::size_t kGroupAt = 0x08, kGroupEach = 12, kGroupMembers = 20;

// Target indexes: players are 0x400 to 0x6FF, the server's dynamic entities (pets, trusts, ...) 0x700 to 0x8FF.
// (A monster a player charms keeps its own index, under 0x400: it's never left out.)
constexpr std::uint16_t kFirstPlayer = 0x400, kFirstDynamic = 0x700, kEnd = 0x900;

// The kinds of update kept for each, past which the oldest goes (the server sends a few)
constexpr std::size_t kMaxKinds = 16;
// What a buffer may have added at the least, however full it is (what doesn't fit goes in the next)
constexpr std::size_t kMinRoom = 0x200;

struct Update {
    std::uint8_t sent = 0; // what it holds (0x0A)
    std::vector<std::uint8_t> data;
};

// A player or dynamic entity in view, by its index
struct Seen {
    std::uint32_t id = 0;    // none while 0
    std::uint32_t owner = 0; // a pet's: its player's id, from the player's updates (which can come before the pet's)
    std::uint16_t pet = 0;   // a player's: its pet's index
    bool trust = false;
    bool party = false;      // a trust that's been in the party (some party lists the server sends leave trusts out)
    std::uint64_t maybe_mine = 0; // a trust that came as yours might: kept in till then, as the party list may come after it
    bool trades = false;     // a player who asked to trade: kept from then on
    bool out = false;        // left out: its updates taken out
    bool in_game = false;    // the game has it: an update about it went through, and none saying it's gone since
    std::vector<Update> updates; // the last of each kind since it came, oldest first
};
Seen g_seen[kEnd - kFirstPlayer];

// What may be added to this buffer: `left` bytes, or one lot of any size if nothing has been yet
struct Room {
    std::size_t left = 0;
    bool used = false;
};

std::atomic<unsigned> g_hide{0};
std::uint32_t g_me = 0;              // the player's id and index (0x00A)
std::uint16_t g_my_index = 0;
std::vector<std::uint32_t> g_group;  // the party and alliance's ids (0x0C8, then each 0x0DD)
std::vector<std::uint16_t> g_traded; // players left out who asked to trade in this buffer: back first
std::vector<std::vector<std::uint8_t>> g_after; // their trade requests, to go after the updates bringing them back
std::uint64_t g_now = 0;             // this buffer's time
std::uint64_t g_mine_until = 0;      // a trust coming till then may be yours (kSummoning, kSummoned, kZoning)
bool g_logged = false;

std::uint16_t word(const std::uint8_t* at)
{
    std::uint16_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::uint32_t dword(const std::uint8_t* at)
{
    std::uint32_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

Seen* seen_at(std::uint16_t index)
{
    return index >= kFirstPlayer && index < kEnd ? &g_seen[index - kFirstPlayer] : nullptr;
}

bool is_player(std::uint16_t index)
{
    return index < kFirstDynamic;
}

std::uint16_t packet_id(std::uint16_t index)
{
    return is_player(index) ? kPlayer : kEntity;
}

bool me(std::uint32_t id, std::uint16_t index)
{
    if (g_me == 0)
        g_me = game::local_player_id(); // (no 0x00A seen)
    return id == g_me || index == g_my_index;
}

bool in_group(std::uint32_t id)
{
    return id != 0 && std::find(g_group.begin(), g_group.end(), id) != g_group.end();
}

// The trusts in the party now are the party's until they go
void mark_party()
{
    for (std::uint16_t index = kFirstDynamic; index < kEnd; ++index) {
        Seen& seen = *seen_at(index);
        if (seen.trust && in_group(seen.id))
            seen.party = true;
    }
}

bool left_out(const Seen& seen, std::uint16_t index, unsigned hide)
{
    if (seen.id == 0)
        return false;
    if (is_player(index))
        return (hide & kPlayers) != 0 && !seen.trades && !in_group(seen.id);
    // (yours are never left out: the game can't release one whose first update came after the party list naming it)
    if (seen.trust)
        return (hide & kTrusts) != 0 && !seen.party && !in_group(seen.id) && g_now >= seen.maybe_mine;
    return (hide & kPets) != 0 && seen.owner != 0 && seen.owner != g_me && !in_group(seen.owner);
}

bool spend(Room& room, std::size_t bytes)
{
    if (bytes == 0)
        return true;
    if (bytes <= room.left) {
        room.left -= bytes;
    } else if (!room.used) {
        room.left = 0;
    } else {
        return false;
    }
    room.used = true;
    return true;
}

int parts(std::uint8_t sent)
{
    return std::popcount(static_cast<unsigned>(sent & kParts));
}

void keep(Seen& seen, const std::uint8_t* packet, std::size_t length)
{
    Update update;
    const auto same = std::find_if(seen.updates.begin(), seen.updates.end(),
                                   [&](const Update& kept) { return kept.sent == packet[kSent]; });
    if (same != seen.updates.end()) {
        update = std::move(*same);
        seen.updates.erase(same);
    } else if (seen.updates.size() >= kMaxKinds) {
        // the oldest, but never the fullest (what the game makes it from again)
        int most = 0;
        for (const Update& kept : seen.updates)
            most = std::max(most, parts(kept.sent));
        const auto oldest = std::find_if(seen.updates.begin(), seen.updates.end(),
                                         [&](const Update& kept) { return parts(kept.sent) < most; });
        seen.updates.erase(oldest != seen.updates.end() ? oldest : seen.updates.begin());
    }
    update.sent = packet[kSent];
    update.data.assign(packet, packet + length);
    seen.updates.push_back(std::move(update));
}

// A new entity at its index (a pet's player, if their update came first, stays)
void start(Seen& seen, std::uint32_t id)
{
    const std::uint32_t owner = seen.id == 0 ? seen.owner : 0;
    seen = {};
    seen.id = id;
    seen.owner = owner;
}

// The update saying it's gone: its last, as the server's own keeps the rest (a player's mount)
std::size_t gone_size(const Seen& seen)
{
    return seen.in_game && !seen.updates.empty() ? seen.updates.back().data.size() : 0;
}

void take_out(Seen& seen, std::uint16_t index)
{
    if (seen.in_game && !seen.updates.empty()) {
        std::vector<std::uint8_t> gone = seen.updates.back().data;
        gone[kSent] = kGone;
        packets::inject(true, packet_id(index), static_cast<std::uint32_t>(gone.size()), gone.data());
    }
    seen.in_game = false;
    seen.out = true;
    if (!g_logged) {
        g_logged = true;
        log::write(L"Others: other players' updates left out from here on ([graphics] hide.players, hide.pets, hide.trusts)");
    }
}

// The updates that make it again: the fullest first (the game makes it from that), then each as they came, so each part
// ends up as the last update holding it had it
std::vector<const Update*> replay(const Seen& seen)
{
    std::vector<const Update*> updates;
    const Update* fullest = nullptr;
    int most = -1;
    for (const Update& update : seen.updates) {
        if (parts(update.sent) >= most)
            most = parts(update.sent), fullest = &update;
    }
    if (fullest != nullptr && fullest != &seen.updates.front())
        updates.push_back(fullest);
    for (const Update& update : seen.updates)
        updates.push_back(&update);
    return updates;
}

std::size_t back_size(const Seen& seen)
{
    std::size_t size = 0;
    for (const Update* update : replay(seen))
        size += update->data.size();
    return size;
}

void bring_back(Seen& seen, std::uint16_t index)
{
    bool named = false;
    for (const Update* update : replay(seen)) {
        packets::inject(true, packet_id(index), static_cast<std::uint32_t>(update->data.size()), update->data.data());
        named = named || (update->sent & kName) != 0;
    }
    // A player whose full update wasn't seen: the server sends it again when asked
    if (is_player(index) && !named) {
        std::uint8_t ask[8] = {};
        std::memcpy(ask + 4, &index, sizeof(index));
        packets::inject(false, kAsk, sizeof(ask), ask);
    }
    seen.out = false;
    seen.in_game = true;
}

// An update about one in view, kept: whether it's taken out. One to be left out now is taken out of the game (if the
// buffer has room for that); one to come back stays out until the end of the buffer (settle), so the updates it has had
// meanwhile go after those that make it again.
bool decide(Seen& seen, std::uint16_t index, unsigned hide, Room& room)
{
    if (seen.out)
        return true;
    if (left_out(seen, index, hide) && spend(room, gone_size(seen))) {
        take_out(seen, index);
        return true;
    }
    seen.in_game = true;
    return false;
}

void on_zone_in(const std::uint8_t* packet, std::size_t length)
{
    for (Seen& seen : g_seen)
        seen = {};
    g_traded.clear();
    g_after.clear();
    g_mine_until = g_now + kZoning;
    if (length >= kIndex + 2) {
        g_me = dword(packet + kId);
        g_my_index = word(packet + kIndex);
    }
}

bool on_player(const std::uint8_t* packet, std::size_t length, unsigned hide, Room& room)
{
    if (length <= kSent)
        return false;
    const std::uint32_t id = dword(packet + kId);
    const std::uint16_t index = word(packet + kIndex);
    Seen* seen = seen_at(index);
    if (seen == nullptr || !is_player(index) || id == 0 || me(id, index))
        return false;
    Seen* pet = seen->id == id && seen->pet >= kFirstDynamic ? seen_at(seen->pet) : nullptr;
    if ((packet[kSent] & kGone) != 0) {
        // (a pet in view keeps its player, so stays out if they did)
        if (pet != nullptr && pet->id == 0 && pet->owner == id)
            pet->owner = 0;
        const bool out = seen->id == id && seen->out;
        if (seen->id == id)
            *seen = {};
        return out;
    }
    if (seen->id != id)
        start(*seen, id);
    keep(*seen, packet, length);
    if (length >= kPet + 2) {
        const std::uint16_t now = word(packet + kPet);
        if (now != seen->pet) {
            if (pet != nullptr && pet->id == 0 && pet->owner == id)
                pet->owner = 0; // one that never came
            seen->pet = now;
        }
        if (Seen* its = now >= kFirstDynamic ? seen_at(now) : nullptr; its != nullptr)
            its->owner = id;
    }
    return decide(*seen, index, hide, room);
}

bool on_entity(const std::uint8_t* packet, std::size_t length, unsigned hide, Room& room)
{
    if (length <= kSent)
        return false;
    const std::uint32_t id = dword(packet + kId);
    const std::uint16_t index = word(packet + kIndex);
    Seen* seen = index >= kFirstDynamic ? seen_at(index) : nullptr;
    if (seen == nullptr || id == 0)
        return false;
    if ((packet[kSent] & kGone) != 0) {
        const bool out = seen->id == id && seen->out;
        if (seen->id == id)
            *seen = {};
        return out;
    }
    if (length <= kFlags)
        return false;
    if (seen->id != id)
        start(*seen, id);
    if ((packet[kFlags] & kTrust) != 0) {
        if (!seen->trust && g_now < g_mine_until)
            seen->maybe_mine = g_mine_until;
        seen->trust = true;
        seen->party = seen->party || in_group(id);
    }
    keep(*seen, packet, length);
    return decide(*seen, index, hide, room);
}

std::uint32_t bits(const std::uint8_t* packet, std::size_t at, unsigned count)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= static_cast<std::uint32_t>((packet[(at + i) / 8] >> ((at + i) % 8)) & 1) << i;
    return value;
}

// Your trust being summoned: a trust that comes meanwhile may be it
void on_action(const std::uint8_t* packet, std::size_t length)
{
    if (length * 8 < kArgumentEnd || bits(packet, kActor, 32) != g_me || g_me == 0)
        return;
    const std::uint32_t category = bits(packet, kCategory, 4), argument = bits(packet, kArgument, 32);
    if (category == kBegin && argument == kTrustBegun)
        g_mine_until = std::max(g_mine_until, g_now + kSummoning);
    else if (category == kCast && argument >= kFirstTrust && argument <= kLastTrust)
        g_mine_until = std::max(g_mine_until, g_now + kSummoned);
}

// Someone asking to trade is kept from then on: one left out comes back before their request
bool on_trade(const std::uint8_t* packet, std::size_t length)
{
    if (length < kIndex + 2)
        return false;
    const std::uint32_t id = dword(packet + kId);
    const std::uint16_t index = word(packet + kIndex);
    Seen* seen = is_player(index) ? seen_at(index) : nullptr;
    if (seen == nullptr || seen->id != id)
        return false;
    seen->trades = true;
    if (!seen->out)
        return false;
    g_traded.push_back(index);
    g_after.emplace_back(packet, packet + length);
    return true;
}

void on_group(const std::uint8_t* packet, std::size_t length)
{
    g_group.clear();
    for (std::size_t i = 0; i < kGroupMembers && kGroupAt + i * kGroupEach + 4 <= length; ++i)
        if (const std::uint32_t id = dword(packet + kGroupAt + i * kGroupEach); id != 0)
            g_group.push_back(id);
    mark_party();
}

void on_member(const std::uint8_t* packet, std::size_t length)
{
    if (length < kId + 4)
        return;
    const std::uint32_t id = dword(packet + kId);
    if (id == 0 || in_group(id))
        return;
    g_group.push_back(id);
    mark_party();
}

// After the buffer's own packets: those to be left out now are taken out of the game and those to come back brought
// back (who asked to trade first, then their requests), as far as the room goes; the rest in the next buffers.
void settle(unsigned hide, Room& room)
{
    for (const std::uint16_t index : g_traded)
        if (Seen* seen = seen_at(index); seen != nullptr && seen->out && !left_out(*seen, index, hide))
            bring_back(*seen, index);
    g_traded.clear();
    for (const std::vector<std::uint8_t>& packet : g_after)
        packets::inject(true, word(packet.data()) & 0x1FF, static_cast<std::uint32_t>(packet.size()), packet.data());
    g_after.clear();
    for (std::uint16_t index = kFirstPlayer; index < kEnd; ++index) {
        Seen& seen = *seen_at(index);
        if (seen.id == 0)
            continue;
        const bool out = left_out(seen, index, hide);
        if (out && !seen.out && spend(room, gone_size(seen)))
            take_out(seen, index);
        else if (!out && seen.out && spend(room, back_size(seen)))
            bring_back(seen, index);
    }
}

} // namespace

void set(unsigned hide)
{
    g_hide = hide & (kPlayers | kPets | kTrusts);
}

std::size_t on_received(std::uint8_t* buffer, std::size_t size, std::size_t room)
{
    if (buffer == nullptr || size <= kHeader)
        return size;
    const unsigned hide = g_hide.load();
    g_now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (g_me == 0)
        g_me = game::local_player_id();
    Room left{std::max(room / 2, kMinRoom)}; // the other half for the plugins'
    std::size_t at = kHeader, written = kHeader;
    while (at + 4 <= size) {
        std::uint8_t* packet = buffer + at;
        const std::uint16_t head = word(packet);
        const std::size_t length = static_cast<std::size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size)
            break;
        bool drop = false;
        switch (head & 0x1FF) {
        case kZoneIn:
            on_zone_in(packet, length);
            break;
        case kPlayer:
            drop = on_player(packet, length, hide, left);
            break;
        case kEntity:
            drop = on_entity(packet, length, hide, left);
            break;
        case kTrade:
            drop = on_trade(packet, length);
            break;
        case kAction:
            on_action(packet, length);
            break;
        case kGroup:
            on_group(packet, length);
            break;
        case kMember:
            on_member(packet, length);
            break;
        }
        if (!drop) {
            if (written != at)
                std::memmove(buffer + written, packet, length);
            written += length;
        }
        at += length;
    }
    // Not a packet: the rest as it is, for the game to judge
    if (at < size) {
        if (written != at)
            std::memmove(buffer + written, buffer + at, size - at);
        written += size - at;
    }
    settle(hide, left);
    return written;
}

} // namespace vanacore::others
