#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Non-blocking HTTP(S) and WebSocket clients for addons (docs/addons/17-http-and-websockets.md). Nothing here touches
// Lua or waits on the game's thread: a request is queued to a small pool of worker threads (WinHTTP, one blocking call
// per worker) and a WebSocket gets two threads of its own (one receiving, one sending). What comes back is posted to
// the mailbox of the addon that asked, and the addon's frame takes it out with poll(), so its callbacks run on the
// game's thread, like every other event. lua_net.cpp is the Lua half.
namespace vanacore::net {

using Owner = std::uint64_t; // one per addon state: its mailbox and its jobs
using Id = std::uint32_t;    // a request or a socket, told apart within one owner (never 0)

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method = "GET";
    std::string url;            // http:// or https://
    Headers headers;
    std::string body;
    double timeout = 30;        // seconds: connecting, sending, waiting for the answer, and any stall while it arrives
    double max_time = 600;      // seconds for the whole request, however it goes
    bool follow = true;         // follow redirects
    int max_redirects = 10;
    std::uint64_t max_size = 16u * 1024 * 1024; // the most of a body that is read (more fails the request)
    std::string save;           // when set, the body goes to this file (ANSI, as vana.fs has paths; written whole or not at all)
    bool insecure = false;      // accept any certificate
};

struct WebSocketRequest {
    std::string url;            // ws://, wss://, http:// or https://
    Headers headers;
    std::vector<std::string> protocols; // Sec-WebSocket-Protocol, in order of preference
    double timeout = 15;        // seconds to connect and complete the handshake
    double keepalive = 30;      // seconds between pings; 0 leaves WinHTTP's own
    std::uint64_t max_size = 16u * 1024 * 1024; // the largest message taken in (more closes the socket with 1009)
    bool insecure = false;
};

// What a job tells its addon, in the order it happened.
struct Event {
    enum class Kind {
        HttpResponse, // status, text (the reason), url (the final one), headers (raw), data (the body) or saved
        HttpError,    // code, text
        WsOpen,       // url, text (the protocol the server chose, or empty)
        WsMessage,    // data, flag (binary)
        WsError,      // code, text (a close follows)
        WsClose,      // status (the close code), text (the reason), flag (it ended with a close handshake)
    };
    Kind kind = Kind::HttpError;
    Id id = 0;
    int status = 0;
    std::string text;
    std::string url;
    std::string headers;
    std::string data;
    std::string saved;
    std::string code;
    bool flag = false;
};

// A new mailbox. Call destroy() when the addon is gone: it stops every job of the owner and drops what is waiting.
Owner create();
void destroy(Owner owner);

// Queues a request / opens a socket. False with `error` when it can't be (a bad URL, too many at once, no WinHTTP).
bool http(Owner owner, HttpRequest request, Id& id, std::string& error);
bool websocket(Owner owner, WebSocketRequest request, Id& id, std::string& error);

// Stops a request or closes a socket at once (a request's event is then an error with code "cancelled"; a socket's close
// is code 1006). Nothing else.
void cancel(Owner owner, Id id);

// A message for a socket, sent in order with the others. False with `error` when the socket isn't open (yet) or its queue
// is full.
bool send(Owner owner, Id id, std::string data, bool binary, std::string& error);
// Starts the closing handshake; the socket's WsClose arrives when the server has answered (or after 5 seconds).
void close(Owner owner, Id id, int code, const std::string& reason);

// Takes up to `max_events` waiting events (and about `max_bytes` of their data) out of the owner's mailbox, oldest first.
void poll(Owner owner, std::vector<Event>& out, size_t max_events, size_t max_bytes);

struct Stats {
    int requests = 0; // queued or running
    int sockets = 0;  // connecting or open
    int waiting = 0;  // events not taken yet
};
Stats stats(Owner owner);
int workers(); // threads in the pool now

// The agent WinHTTP introduces itself with ("VanaCore/0.8.68"); before the first request.
void set_user_agent(const std::wstring& agent);

} // namespace vanacore::net
