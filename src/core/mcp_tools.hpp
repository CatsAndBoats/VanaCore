#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "json.hpp"

// Between the MCP server (mcp.cpp: the connection, the protocol) and its tools (mcp_tools.cpp).
namespace vanacore::mcp {

// Runs `work` on the game's thread at its next frame and waits for it. False if no frame ran it within
// `timeout_ms` (the game isn't drawing: a loading screen, minimised with frames stopped); it then never
// runs, so what it captured by reference stays safe.
bool on_game_thread(const std::function<void()>& work, DWORD timeout_ms = 5000);

// What [mcp] says, for the tools.
struct Settings {
    std::wstring install_dir;
    unsigned port = 47800;
};
const Settings& settings();

// The hello tool: names the session the request being served belongs to (what the chat is about), for the player's
// chat log and the /vana window. Returns what to tell the chat.
std::string name_session(const std::string& name);

namespace tools {

// The tools/list result's "tools".
json::Value list();

// Runs a tool: the tools/call result ({content, isError}).
json::Value call(const std::string& name, const json::Value& arguments);

// Called from mcp.cpp's on_frame, on_packet and on_chat while the server runs.
void on_frame();
void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length);
void on_chat(std::string_view line, int mode);

} // namespace tools
} // namespace vanacore::mcp
