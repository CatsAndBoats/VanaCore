#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// An MCP server inside the game (Model Context Protocol, the Streamable HTTP transport): AI tools such
// as Claude Code connect to it on this machine and see the running client - its memory, code, entities,
// packets, chat and screen - and act in it (Lua, commands, memory writes, hardware breakpoints).
// [mcp] enabled=1 in vanacore.ini starts it, on 127.0.0.1 only (port= 47800 by default); off by default.
// Connect with: claude mcp add --transport http vanacore http://127.0.0.1:47800/mcp
// The tools are in mcp_tools.cpp.
namespace vanacore::mcp {

// Reads [mcp] from `ini` and, when enabled, starts the server's thread.
void install(const wchar_t* install_dir, const wchar_t* ini);

// Once per frame, from Present, on the game's thread: runs what the tools asked of the game's thread.
void on_frame();

// Each packet the game received or sent (its 4-byte header first), as it got or sent it (packets.cpp),
// for the packets tool. Cheap when the server is off.
void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length);

// Each line reaching the chat log, as the game has it (Shift-JIS, colour codes, auto-translate phrases)
// (chat.cpp), for the chat_log tool. Game thread.
void on_chat(std::string_view line, int mode);

// True while the server is listening.
bool running();

// For the /vana window (vana.mcp.info): whether it's listening and where, whether it couldn't start, the session
// that used it last (as the player sees it: #3 claude-code "alliance list RE", how long since its last request), and
// every session since the game started (each initialize is one; a chat names its own with the hello tool).
struct Status {
    struct Session {
        unsigned number = 0;
        std::string client;      // what the client called itself (claude-code, probe)
        std::string name;        // what the chat is about ("" until it says)
        double idle_seconds = 0; // since its last request
        unsigned calls = 0;      // tool calls so far
    };
    bool running = false;
    bool failed = false;     // [mcp] enabled, but no port could be had
    unsigned port = 0;
    std::string client;      // empty until one connects
    double idle_seconds = -1; // since the last request (-1: none yet)
    std::vector<Session> sessions; // oldest first
};
Status status();

} // namespace vanacore::mcp
