#pragma once

#include <string>
#include <string_view>
#include <vector>

// Messages between the VanaCore instances on this PC (Windower's IPC: send_ipc_message and 'ipc message', which
// multibox addons such as FastFollow, send and MyCap talk through). A message goes from an addon to the addon of the same
// name in every other instance running in this Windows session. They pass through a ring of slots in shared memory
// (Local\VanaCore.IPC.1, guarded by a mutex): each instance reads what's new once a frame. A message is at most
// kMaxMessage bytes; one that's been overwritten before an instance read it (more than kSlots behind) is lost to it.
namespace vanacore::ipc {

constexpr size_t kMaxMessage = 8000;
constexpr size_t kSlots = 128;

struct Message {
    std::string addon; // the sender's addon, in lower case
    std::string text;
};

// Sends `text` from addon `addon` to the other instances. False if it's too long or the shared memory couldn't be made.
bool send(std::string_view addon, std::string_view text);

// The messages other instances sent since the last call, oldest first (none the first time: it starts from now).
std::vector<Message> receive();

} // namespace vanacore::ipc
