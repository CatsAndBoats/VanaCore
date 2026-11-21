#include "maxhp_ask.hpp"

#include <windows.h>

#include <cstring>
#include <deque>
#include <iterator>
#include <unordered_map>
#include <utility>

#include "packets.hpp"

namespace vanacore::maxhp {
namespace {

constexpr std::uint16_t kPacket = 0x1A7;
constexpr std::uint16_t kZoneIn = 0x00A;
constexpr std::uint8_t kRequest = 1; // C2S action
constexpr std::uint8_t kReply = 1;   // S2C action
constexpr std::size_t kRequestBytes = 0x28;
constexpr std::size_t kFirstEntry = 0x08;
constexpr std::size_t kEntryBytes = 16; // id, HP now, max HP (u32 each), status (u8) and 3 reserved
constexpr std::uint64_t kTimeoutMs = 3000; // no reply by then: the server has no such module, or the reply was lost
constexpr std::uint64_t kKeepMs = 60000;   // how long an answer stands as an entity's maximum
constexpr std::size_t kMaxWaiting = 64;

struct Pending {
    std::uint32_t ids[kMaxIds] = {};
    unsigned count = 0;
    std::uint64_t deadline = 0;
    Done done;
    std::string owner;
};

struct Finished {
    unsigned token = 0;
    Answer answers[kMaxIds];
    unsigned count = 0;
    Done done;
    std::string owner;
};

struct Known {
    std::uint32_t hp_max = 0;
    std::uint64_t at = 0;
};

std::unordered_map<unsigned, Pending> g_pending;
std::deque<Finished> g_finished;
std::unordered_map<std::uint32_t, Known> g_known;
unsigned g_last_token = 0;

std::uint16_t word_at(const std::uint8_t* p)
{
    std::uint16_t out;
    std::memcpy(&out, p, sizeof out);
    return out;
}

std::uint32_t dword_at(const std::uint8_t* p)
{
    std::uint32_t out;
    std::memcpy(&out, p, sizeof out);
    return out;
}

// The next token not waiting for an answer (1 to 65535), or 0 when all are
unsigned next_token()
{
    for (unsigned tries = 0; tries < 0xFFFF; ++tries) {
        g_last_token = g_last_token % 0xFFFF + 1;
        if (g_pending.find(g_last_token) == g_pending.end())
            return g_last_token;
    }
    return 0;
}

// A request ends: its answers wait for the next on_frame
void finish(unsigned token, Pending& pending, const Answer* answers)
{
    Finished done;
    done.token = token;
    done.count = pending.count;
    for (unsigned i = 0; i < pending.count; ++i)
        done.answers[i] = answers[i];
    done.done = std::move(pending.done);
    done.owner = std::move(pending.owner);
    g_finished.push_back(std::move(done));
}

} // namespace

const char* status_name(Status status)
{
    switch (status) {
    case Status::Ok:
        return "ok";
    case Status::NotFound:
        return "not_found";
    case Status::Disabled:
        return "disabled";
    case Status::TooFast:
        return "too_fast";
    case Status::Hidden:
        return "hidden";
    default:
        return "no_answer";
    }
}

const char* ask(const std::uint32_t* ids, unsigned count, Done done, const std::string& owner, unsigned* token)
{
    if (ids == nullptr || count == 0)
        return "no ids to ask about";
    if (count > kMaxIds)
        return "at most 8 ids in one request";
    if (!done)
        return "no callback";
    if (!packets::ready())
        return "not in the game yet";
    if (g_pending.size() >= kMaxWaiting)
        return "too many requests are waiting";
    const unsigned number = next_token();
    if (number == 0)
        return "too many requests are waiting";

    std::uint8_t packet[kRequestBytes] = {};
    packet[4] = kRequest;
    packet[5] = static_cast<std::uint8_t>(count);
    const auto wire = static_cast<std::uint16_t>(number);
    std::memcpy(packet + 6, &wire, sizeof wire);
    Pending pending;
    pending.count = count;
    pending.deadline = GetTickCount64() + kTimeoutMs;
    pending.done = std::move(done);
    pending.owner = owner;
    for (unsigned i = 0; i < count; ++i) {
        pending.ids[i] = ids[i];
        std::memcpy(packet + kFirstEntry + 4 * i, &ids[i], 4);
    }
    g_pending.emplace(number, std::move(pending));
    packets::inject(false, kPacket, sizeof packet, packet);
    if (token != nullptr)
        *token = number;
    return nullptr;
}

void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length)
{
    if (!incoming || length < 4)
        return;
    const unsigned id = word_at(packet) & 0x1FF;
    if (id == kZoneIn) {
        g_known.clear(); // other entities now; a spawn in the new zone may reuse an id
        return;
    }
    if (id != kPacket || length < kFirstEntry || packet[4] != kReply)
        return;

    const auto found = g_pending.find(word_at(packet + 6));
    if (found == g_pending.end())
        return; // a reply to something else (an addon talking the protocol itself), or one that came too late
    Pending& pending = found->second;

    std::size_t entries = packet[5];
    if (entries > kMaxIds)
        entries = kMaxIds;
    if (kFirstEntry + entries * kEntryBytes > length)
        entries = (length - kFirstEntry) / kEntryBytes;

    Answer answers[kMaxIds];
    const std::uint64_t now = GetTickCount64();
    for (unsigned i = 0; i < pending.count; ++i) {
        answers[i].id = pending.ids[i]; // (NoAnswer until a reply entry is found for it)
        for (std::size_t e = 0; e < entries; ++e) {
            const std::uint8_t* entry = packet + kFirstEntry + e * kEntryBytes;
            if (dword_at(entry) != pending.ids[i])
                continue;
            // A status this VanaCore doesn't know (a newer server) is no answer, never a number a caller can't name.
            answers[i].status = entry[12] <= static_cast<std::uint8_t>(Status::Hidden) ? static_cast<Status>(entry[12]) : Status::NoAnswer;
            if (answers[i].status == Status::Ok) {
                answers[i].hp = dword_at(entry + 4); // (a snapshot: not kept, it is out of date within a frame)
                answers[i].hp_max = dword_at(entry + 8);
                g_known[pending.ids[i]] = {answers[i].hp_max, now};
            }
            break;
        }
    }
    finish(found->first, pending, answers);
    g_pending.erase(found);
}

void on_frame()
{
    const std::uint64_t now = GetTickCount64();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now < it->second.deadline) {
            ++it;
            continue;
        }
        Answer answers[kMaxIds];
        for (unsigned i = 0; i < it->second.count; ++i)
            answers[i].id = it->second.ids[i]; // (NoAnswer)
        finish(it->first, it->second, answers);
        it = g_pending.erase(it);
    }
    for (auto it = g_known.begin(); it != g_known.end();)
        it = now - it->second.at > kKeepMs ? g_known.erase(it) : std::next(it);

    // What finished runs from a list taken first: a callback may ask again
    std::deque<Finished> done;
    done.swap(g_finished);
    for (Finished& one : done)
        one.done(one.token, one.answers, one.count);
}

void release(const std::string& owner)
{
    for (auto it = g_pending.begin(); it != g_pending.end();)
        it = it->second.owner == owner ? g_pending.erase(it) : std::next(it);
    for (auto it = g_finished.begin(); it != g_finished.end();)
        it = it->owner == owner ? g_finished.erase(it) : std::next(it);
}

entityhp::Max known(std::uint32_t id)
{
    entityhp::Max out;
    const auto found = g_known.find(id);
    if (found == g_known.end() || GetTickCount64() - found->second.at > kKeepMs)
        return out;
    out.source = entityhp::Source::Server;
    out.value = out.low = out.high = found->second.hp_max;
    return out;
}

} // namespace vanacore::maxhp
