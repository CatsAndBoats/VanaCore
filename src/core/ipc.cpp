#include "ipc.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "log.hpp"

namespace vanacore::ipc {
namespace {

constexpr std::uint32_t kMagic = 0x43504956; // "VIPC"
constexpr size_t kNameSize = 64;

struct Slot {
    std::uint64_t sequence; // the message's number (its slot is sequence % kSlots); 0: never written
    std::uint32_t process;  // the sender's process id
    std::uint32_t length;
    char addon[kNameSize];  // lower case, zero-terminated
    char text[kMaxMessage];
};

struct Ring {
    std::uint32_t magic;
    std::uint32_t slot_size;
    std::uint64_t written; // the last message's sequence (0: none yet)
    Slot slots[kSlots];
};

HANDLE g_mapping = nullptr;
HANDLE g_mutex = nullptr;
Ring* g_ring = nullptr;
bool g_failed = false;     // couldn't be made: not tried again
std::uint64_t g_read = 0;  // the last sequence this instance has seen
bool g_started = false;    // receive() has run once (g_read is set)

// The shared ring, made by the first instance and opened by the rest. Null if it couldn't be.
Ring* ring()
{
    if (g_ring != nullptr || g_failed)
        return g_ring;
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\VanaCore.IPC.1.lock");
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Ring), L"Local\\VanaCore.IPC.1");
    if (g_mutex == nullptr || g_mapping == nullptr) {
        log::write(L"IPC: the shared memory couldn't be made (error %lu): Windower addons' IPC messages won't go between "
                   L"instances", GetLastError());
        g_failed = true;
        return nullptr;
    }
    g_ring = static_cast<Ring*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    if (g_ring == nullptr) {
        log::write(L"IPC: the shared memory couldn't be mapped (error %lu)", GetLastError());
        g_failed = true;
        return nullptr;
    }
    return g_ring;
}

// The ring's mutex for as long as this lives (an instance that died holding it leaves it abandoned: still ours).
class Lock {
public:
    Lock() : held_(WaitForSingleObject(g_mutex, 20) != WAIT_TIMEOUT) {}
    ~Lock()
    {
        if (held_)
            ReleaseMutex(g_mutex);
    }
    bool held() const { return held_; }

private:
    bool held_;
};

// A new ring (all zeros) gets its header; one from another build with other slot sizes isn't used.
bool usable(Ring& r)
{
    if (r.magic == 0) {
        r.magic = kMagic;
        r.slot_size = sizeof(Slot);
    }
    return r.magic == kMagic && r.slot_size == sizeof(Slot);
}

} // namespace

bool send(std::string_view addon, std::string_view text)
{
    if (text.size() > kMaxMessage) {
        log::write(L"IPC: %hs's message (%zu bytes) is over %zu bytes: not sent", std::string(addon).c_str(), text.size(),
                   kMaxMessage);
        return false;
    }
    Ring* r = ring();
    if (r == nullptr)
        return false;
    const Lock lock;
    if (!lock.held() || !usable(*r))
        return false;
    const std::uint64_t sequence = r->written + 1;
    Slot& slot = r->slots[sequence % kSlots];
    slot.process = GetCurrentProcessId();
    slot.length = static_cast<std::uint32_t>(text.size());
    std::memset(slot.addon, 0, kNameSize);
    for (size_t i = 0; i < addon.size() && i + 1 < kNameSize; ++i)
        slot.addon[i] = addon[i] >= 'A' && addon[i] <= 'Z' ? static_cast<char>(addon[i] - 'A' + 'a') : addon[i];
    std::memcpy(slot.text, text.data(), text.size());
    slot.sequence = sequence;
    r->written = sequence;
    return true;
}

std::vector<Message> receive()
{
    std::vector<Message> messages;
    Ring* r = ring();
    if (r == nullptr)
        return messages;
    // A plain read first: most frames nothing's new, and the mutex isn't needed to see that.
    const std::uint64_t written = *static_cast<volatile std::uint64_t*>(&r->written);
    if (!g_started || written < g_read) { // the first call: what was sent before isn't this instance's
        g_started = true;
        g_read = written;
        return messages;
    }
    if (written == g_read)
        return messages;
    const Lock lock;
    if (!lock.held() || !usable(*r))
        return messages;
    std::uint64_t from = g_read + 1;
    if (r->written >= kSlots && from < r->written - kSlots + 1)
        from = r->written - kSlots + 1; // the ones before were overwritten
    const DWORD self = GetCurrentProcessId();
    for (std::uint64_t sequence = from; sequence <= r->written; ++sequence) {
        const Slot& slot = r->slots[sequence % kSlots];
        if (slot.sequence != sequence || slot.process == self || slot.length > kMaxMessage)
            continue;
        messages.push_back({std::string(slot.addon, strnlen(slot.addon, kNameSize)), std::string(slot.text, slot.length)});
    }
    g_read = r->written;
    return messages;
}

} // namespace vanacore::ipc
