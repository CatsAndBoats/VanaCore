#include "trusts.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "game.hpp"
#include "log.hpp"

namespace vanacore::trusts {
namespace {

constexpr std::size_t kHeader = 0x1C; // the buffer's header

// The packets: a zone entered, an update about an entity, an action, a party member (its id at 0x04).
constexpr std::uint16_t kZoneIn = 0x00A, kEntity = 0x00E, kAction = 0x028, kPartyMember = 0x0DD;
constexpr std::size_t kMemberId = 0x04;

// 0x00E: the entity's id (0x04) and index (0x08), what the update holds (0x0A: 0x20 the entity gone) and
// the first byte of its fourth set of flags (0x28: 0x01 a trust, 0x04 PetNewFlag, its arrival played).
constexpr std::size_t kId = 0x04, kIndex = 0x08, kSent = 0x0A, kFlags = 0x28;
constexpr std::uint8_t kGone = 0x20, kTrust = 0x01, kArrival = 0x04;

// 0x028, bit-packed from its 5th byte, lowest bit first: the actor (32 bits), the targets (6), results (4),
// the category (4) and its argument (32), then the recast (32) and the first target (32), its results (4),
// the first result's resolution (3), kind (2), animation (12), info (5), distortion (2), knockback (3) and
// param (17).
constexpr std::size_t kActor = 40, kCategory = 82, kArgument = 86, kParam = 213, kParamEnd = 230;
// A spell cast (category 4, the argument the spell), or begun (8: the argument "cafa" for a trust, "spfa"
// as one is interrupted; the first result's param the spell).
constexpr std::uint32_t kCast = 4, kBegin = 8, kTrustBegun = 0x61666163, kTrustInterrupted = 0x61667073;
// Trusts' spells
constexpr std::uint32_t kFirstTrust = 896, kLastTrust = 1023;
// How long after a trust's summoning begins it may arrive (the casting, then the server's next update);
// and after the cast ends.
constexpr ULONGLONG kCasting = 12000, kCastEnded = 3000;
// How long the flag stays on for a trust seen summoned: the game makes it from the first update or so.
constexpr ULONGLONG kShown = 2000;

std::atomic<int> g_spawn{static_cast<int>(Spawn::Cast)};

// By entity index, each trust seen: its id (0x04), and until when the server's flag is left on (its arrival
// played: only as it's summoned; the game plays it whenever it makes the trust anew, coming back into view).
struct Seen {
    std::uint32_t id;
    ULONGLONG shown_until;
};
Seen g_seen[0x1000] = {};

// Trusts being summoned, a cast each: each good for one trust's arrival, until `until`.
struct Summoning {
    std::uint32_t caster;
    ULONGLONG until;
};
std::vector<Summoning> g_summoning;

std::uint32_t bits(const std::uint8_t* packet, std::size_t at, unsigned count)
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= static_cast<std::uint32_t>((packet[(at + i) / 8] >> ((at + i) % 8)) & 1) << i;
    return value;
}

Summoning* summoning(std::uint32_t caster)
{
    for (Summoning& entry : g_summoning)
        if (entry.caster == caster)
            return &entry;
    return nullptr;
}

void on_action(const std::uint8_t* packet, std::size_t length, ULONGLONG now)
{
    if (length * 8 < kParamEnd)
        return;
    const std::uint32_t caster = bits(packet, kActor, 32), category = bits(packet, kCategory, 4),
                        argument = bits(packet, kArgument, 32);
    Summoning* entry = summoning(caster);
    if (category == kBegin && argument == kTrustBegun) {
        if (entry != nullptr)
            entry->until = now + kCasting;
        else
            g_summoning.push_back({caster, now + kCasting});
    } else if (category == kBegin && argument == kTrustInterrupted) {
        if (entry != nullptr)
            entry->until = 0;
    } else if (category == kCast && argument >= kFirstTrust && argument <= kLastTrust) {
        if (entry != nullptr)
            entry->until = now + kCastEnded;
        else
            g_summoning.push_back({caster, now + kCastEnded});
    }
}

// A trust summoned just now: its cast, taken.
bool take_summoning(ULONGLONG now)
{
    bool found = false;
    for (auto at = g_summoning.begin(); at != g_summoning.end();) {
        if (at->until <= now) {
            at = g_summoning.erase(at);
        } else if (!found) {
            found = true;
            at = g_summoning.erase(at);
        } else {
            ++at;
        }
    }
    return found;
}

// Whether `id` is in the party or alliance: the game's list, or a member update in this buffer the game hasn't read yet.
// The flag is what lets the game release a trust (the party menu's Release, /refa), so a party's trusts keep it.
bool in_party(std::uint32_t id, const std::vector<std::uint32_t>& members)
{
    for (const std::uint32_t member : members)
        if (member == id)
            return true;
    game::PartyMember party[18];
    game::Alliance alliance;
    if (!game::party(party, alliance))
        return false;
    for (const game::PartyMember& member : party)
        if (member.active && member.id == id)
            return true;
    return false;
}

void on_entity(std::uint8_t* packet, std::size_t length, Spawn spawn, ULONGLONG now,
               const std::vector<std::uint32_t>& members)
{
    if (length <= kFlags)
        return;
    std::uint32_t id;
    std::uint16_t index;
    std::memcpy(&id, packet + kId, sizeof(id));
    std::memcpy(&index, packet + kIndex, sizeof(index));
    Seen& seen = g_seen[index & 0xFFF];
    // not a trust (now: another at an index a trust had)
    if ((packet[kFlags] & kTrust) == 0) {
        seen = {};
        return;
    }
    // Gone (dismissed, or out of view; the update as it was sent): the next to come at this index is new
    // (its id is the index's own)
    if ((packet[kSent] & kGone) != 0) {
        seen = {};
        return;
    }
    if (seen.id != id) {
        const bool summoned = spawn == Spawn::Cast && take_summoning(now);
        seen = {id, summoned ? now + kShown : 0};
        if (spawn != Spawn::Always)
            log::write(summoned ? L"Trusts: one summoned (index %u): its arrival shown ([graphics] trusts.spawn)"
                                : L"Trusts: one came into view (index %u), not seen summoned: its arrival left out "
                                  L"([graphics] trusts.spawn)",
                       index);
    }
    if (spawn != Spawn::Always && now >= seen.shown_until && !in_party(id, members))
        packet[kFlags] &= static_cast<std::uint8_t>(~kArrival);
}

} // namespace

void set(Spawn spawn)
{
    g_spawn = static_cast<int>(spawn);
}

void on_received(std::uint8_t* buffer, std::size_t size)
{
    // (with trusts.spawn=always too: which entities are trusts, for their names' colour)
    const Spawn spawn = static_cast<Spawn>(g_spawn.load());
    if (buffer == nullptr || size <= kHeader)
        return;
    const ULONGLONG now = GetTickCount64();
    // the party members this buffer brings (a trust's first update can come before the game lists it)
    std::vector<std::uint32_t> members;
    for (std::size_t at = kHeader; at + 4 <= size;) {
        const std::uint8_t* packet = buffer + at;
        std::uint16_t head;
        std::memcpy(&head, packet, sizeof(head));
        const std::size_t length = static_cast<std::size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size)
            break;
        if ((head & 0x1FF) == kPartyMember && length >= kMemberId + 4) {
            std::uint32_t id;
            std::memcpy(&id, packet + kMemberId, sizeof(id));
            members.push_back(id);
        }
        at += length;
    }
    for (std::size_t at = kHeader; at + 4 <= size;) {
        std::uint8_t* packet = buffer + at;
        std::uint16_t head;
        std::memcpy(&head, packet, sizeof(head));
        const std::size_t length = static_cast<std::size_t>(head >> 9) * 4;
        if (length < 4 || at + length > size)
            break;
        switch (head & 0x1FF) {
        case kZoneIn:
            std::memset(g_seen, 0, sizeof(g_seen));
            g_summoning.clear();
            break;
        case kEntity:
            on_entity(packet, length, spawn, now, members);
            break;
        case kAction:
            on_action(packet, length, now);
            break;
        }
        at += length;
    }
}

bool is_trust(std::uint16_t index)
{
    return g_seen[index & 0xFFF].id != 0;
}

} // namespace vanacore::trusts
