#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A diagnostic trace of chosen addons: what VanaCore sees them do, in logs\vanacore.log as "Trace <addon>: ...". For an addon that
// "doesn't work" on someone's machine with nothing in the log: which commands reached it, which events it ran for, what its
// ImGui windows were (open, shown, collapsed, where, how big, whether they drew anything), the packets it sent and blocked, the
// chat lines and commands it queued, and a line every 10 seconds saying what happened since.
//
// Turned on with [logging] trace.addons=partyfinder,xiui in vanacore.ini, or /vana trace partyfinder (also written to the ini);
// /vana trace off, /vana trace list. Needs logging on ([logging] enabled=1, the default). Nothing here costs anything while no
// addon is traced. Names are the addon's folder, any case. Any thread, except where noted.
//
// /vana trace <addon> <seconds> (1 to 600; "30" or "30s") is a timed trace, for "is this addon making the game lag?": the
// trace as above for that long, not written to the ini, and besides: every call into the addon's Lua timed by event, with its
// Lua memory before and after (the garbage it makes, the calls its collector ran in), a sampling profile of its Lua
// (addons::profile_start), the game's frames (frametime::watch: each frame's time and parts, and the addon's time in it), every
// other addon's time for comparison, and the hardware's state (hwmon). At the end it writes logs\trace-<addon>-<date>-<time>.txt
// whether or not logging is on (addon_trace_report.hpp) and says the gist in the chat; /vana trace off ends it early.
namespace vanacore::addon_trace {

// Once, from addons::install: [logging] trace.addons read; VanaCore's folder (the timed trace's report goes in its logs).
void install(const wchar_t* install_dir, const wchar_t* settings_ini);

// /vana trace <addon...|off|list> or <addon> <seconds> (the words after "trace"). Game thread.
void command(const std::vector<std::string>& words);

// Whether `addon` ("partyfinder", or "addon partyfinder" as packets name the one adding a packet) is traced.
bool on(std::string_view addon);
// Whether any addon is.
bool any();
// Whether a timed trace runs: then every addon's calls are timed and given to event(), for the timed one's share.
bool timing();

// One line, printf-style, "Trace <addon>: <text>".
void say(std::string_view addon, const char* format, ...);

// An addon loaded (listens: what it registered for) or went.
void loaded(std::string_view addon, const std::string& listens);
void unloaded(std::string_view addon, const std::string& reason);

// A call into the addon's Lua for `event` ("present", "command", "packet_in", ... "frame" for its tasks) took `ms`. For a
// traced addon, its Lua memory (KB) before and after the call too; less after, its garbage collector ran in the call.
void event(std::string_view addon, std::string_view event, double ms, double kb_before = 0, double kb_after = 0);

// A command line the game ran: what handled it (empty: it went to the addons and nothing took it), and the addon that took it.
// `after_addons`: the addons had it first, and none took it.
void command_line(const std::string& line, int mode, bool injected, std::string_view handled_by, bool after_addons = false);
// The line reached the addon: whether it took it (blocked) and its error, if it had one.
void command_reached(std::string_view addon, const std::string& line, bool took_it, const std::string& error);

// What an addon's top-level ImGui window was when it called Begin: its p_open before and after, what Begin returned.
struct Window {
    bool has_open = false; // it gave a p_open
    bool open_before = false;
    bool open_after = false;
    bool shown = false;     // Begin's result (false: collapsed or clipped, nothing to draw)
    bool collapsed = false;
    bool appearing = false;
    float x = 0, y = 0, w = 0, h = 0; // where it is and its size, in ImGui's pixels
    float screen_w = 0, screen_h = 0;
    int flags = 0;
};
void window_begin(std::string_view addon, const char* name, const Window& state);
// Its End: how much it drew (vertices and indices) and how big its content was.
void window_end(std::string_view addon, const char* name, int vertices, int indices, float content_w, float content_h);

// A packet reached an addon's packet_in/packet_out event; `blocked` if the addon blocked it.
void packet_delivered(std::string_view addon, bool incoming, std::uint16_t id, std::uint32_t size, bool blocked);
// An addon added a packet (AddOutgoingPacket / AddIncomingPacket), and what became of it: "queued", "sent" or why not.
void packet_added(std::string_view from, bool incoming, std::uint16_t id, const std::uint8_t* data, std::uint32_t size);
void packet_result(std::string_view from, bool incoming, std::uint16_t id, const char* what);

// A chat line an addon wrote, and a command it ran or queued.
void chat_line(std::string_view addon, int mode, std::string_view text);
void queued_command(std::string_view addon, int mode, std::string_view command, bool queued);

// Once per frame (addons::on_frame): the heartbeat, and a note for each traced addon that isn't loaded. `loaded` lists the
// addons that are, lower case. Game thread.
void tick(const std::vector<std::string>& loaded);

} // namespace vanacore::addon_trace
