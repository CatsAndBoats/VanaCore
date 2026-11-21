// The zone link: the game's UDP with the map server, for the log. loader.cpp includes this inside its unnamed namespace,
// after g_login, address_text and peer_of, whose names it uses. It is no file of its own.
//
// FFXI-3001 ("connection timed out") after a character is picked is the map server not answering the game: the lobby's
// select reply (0x0B) names the server and its port, the game binds its UDP port (polcore's, 54090) and sends its first
// packet (0x00A) there. These lines say which of that didn't happen: the port taken by another program, the server's port
// refusing it (an ICMP "port unreachable" comes back to recvfrom as WSAECONNRESET), or nothing coming back at all.
#pragma once

namespace zonelink {

using SendTo = int(WINAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using RecvFrom = int(WINAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using Bind = int(WINAPI*)(SOCKET, const sockaddr*, int);
SendTo g_send_to = nullptr;
RecvFrom g_recv_from = nullptr;
Bind g_bind = nullptr;

constexpr ULONGLONG kSilentMs = 5000; // no answer this long after the first packet: said in the log
std::atomic<int> g_lines{40};         // its own budget: a game zoning all evening says little
std::atomic<int> g_binds{4};          // the UDP sockets made, the first few
std::atomic<int> g_errors{3};         // the zone socket's receive errors, the first few

bool say(std::atomic<int>& budget = g_lines)
{
    return budget.fetch_sub(1) > 0;
}

// The game's thread, mostly; an addon's socket may send from another
std::mutex g_lock;
std::uint32_t g_zone_ip[2] = {}; // the 0x0B's server address, as it came and byte-swapped (which order it's in varies)
sockaddr_in g_peer{};            // the map server the game sends to now
SOCKET g_socket = INVALID_SOCKET;
unsigned g_sent = 0;
unsigned g_received = 0;
ULONGLONG g_first_sent = 0;
bool g_silence_said = false;
std::atomic<bool> g_waiting{false}; // packets sent to g_peer, none back yet: the game's every receive looks at the time

std::uint32_t read32(const char* at)
{
    std::uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

std::string dotted(std::uint32_t in_memory)
{
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = in_memory;
    const std::string text = address_text(at);
    return text.substr(0, text.rfind(':'));
}

// The server's map servers: the address the lobby named, either way round, or the lobby's own
bool is_zone(const sockaddr_in& peer)
{
    const std::uint32_t address = peer.sin_addr.s_addr;
    return address == g_zone_ip[0] || address == g_zone_ip[1] || address == g_login.server_address;
}

std::uint16_t local_port(SOCKET socket)
{
    sockaddr_in at{};
    int length = sizeof(at);
    return getsockname(socket, reinterpret_cast<sockaddr*>(&at), &length) == 0 ? ntohs(at.sin_port) : 0;
}

bool is_udp(SOCKET socket)
{
    int type = 0;
    int length = sizeof(type);
    return getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) == 0 && type == SOCK_DGRAM;
}

// The lobby's answer to a character being picked (0x0B, lpkt_next_login): the map server at 56 and its port at 60, the
// search server at 64 and 68. Each pick starts the zone link's counts again.
void on_lobby_reply(const char* data, int size)
{
    if (size < 72 || std::memcmp(data + 4, "IXFF", 4) != 0 || static_cast<unsigned char>(data[8]) != 0x0B)
        return;
    const std::uint32_t zone = read32(data + 56);
    {
        const std::lock_guard lock(g_lock);
        g_zone_ip[0] = zone;
        g_zone_ip[1] = _byteswap_ulong(zone);
        g_peer = {};
        g_socket = INVALID_SOCKET;
        g_sent = g_received = 0;
        g_first_sent = 0;
        g_silence_said = false;
        g_waiting = false;
    }
    log::write(L"Zone link: the lobby sends the character to the map server at %hs, port %u (search %hs, port %u)",
               dotted(zone).c_str(), read32(data + 60), dotted(read32(data + 64)).c_str(), read32(data + 68));
}

// Called with g_lock held: kSilentMs since the first packet to g_peer and nothing back is said, once
void silence_check()
{
    const ULONGLONG waited = GetTickCount64() - g_first_sent;
    if (g_received != 0 || g_silence_said || waited < kSilentMs)
        return;
    g_silence_said = true;
    g_waiting = false;
    if (say())
        log::write(L"Zone link: no answer from %hs in %llu s: %u packets sent from port %u, none back. The server takes none "
                   L"of them (its log says why: a zone key that isn't the one it has?) or its answers don't get here; the "
                   L"game gives up with FFXI-3001",
                   address_text(g_peer).c_str(), waited / 1000, g_sent, local_port(g_socket));
}

void on_sent(SOCKET socket, const sockaddr_in& peer, int size, int result, DWORD error)
{
    if (!is_zone(peer))
        return;
    const std::lock_guard lock(g_lock);
    if (peer.sin_addr.s_addr != g_peer.sin_addr.s_addr || peer.sin_port != g_peer.sin_port) {
        g_peer = peer;
        g_socket = socket;
        g_sent = g_received = 0;
        g_first_sent = GetTickCount64();
        g_silence_said = false;
        g_waiting = true;
        if (say())
            log::write(L"Zone link: the game's first packet to %hs: %d bytes from its port %u%ls", address_text(peer).c_str(),
                       size, local_port(socket),
                       result == SOCKET_ERROR ? (L", which failed (winsock error " + std::to_wstring(error) + L")").c_str() : L"");
    }
    ++g_sent;
    silence_check();
}

void on_received(const sockaddr_in& peer, int size)
{
    if (!is_zone(peer))
        return;
    const std::lock_guard lock(g_lock);
    g_waiting = false;
    if (++g_received == 1 && say())
        log::write(L"Zone link: the first answer from %hs: %d bytes, %.1f s after the first packet (%u sent)%ls",
                   address_text(peer).c_str(), size, (GetTickCount64() - g_first_sent) / 1000.0, g_sent,
                   g_silence_said ? L", after the silence above" : L"");
}

// The game looking for an answer and finding none yet
void on_nothing_yet(SOCKET socket)
{
    if (!g_waiting.load(std::memory_order_relaxed))
        return;
    const std::lock_guard lock(g_lock);
    if (socket == g_socket)
        silence_check();
}

void on_receive_error(SOCKET socket, DWORD error)
{
    {
        const std::lock_guard lock(g_lock);
        if (socket != g_socket)
            return;
    }
    if (say(g_errors))
        log::write(L"Zone link: the game's port %u couldn't receive: winsock error %lu%ls", local_port(socket), error,
                   error == WSAECONNRESET ? L": the map server's port refused a packet (ICMP port unreachable: nothing "
                                            L"listening there, or a firewall)"
                                          : L"");
}

int WINAPI send_to(SOCKET socket, const char* data, int size, int flags, const sockaddr* to, int length)
{
    const int result = g_send_to(socket, data, size, flags, to, length);
    const DWORD error = GetLastError();
    if (to != nullptr && length >= static_cast<int>(sizeof(sockaddr_in)) && to->sa_family == AF_INET)
        on_sent(socket, *reinterpret_cast<const sockaddr_in*>(to), size, result, error);
    SetLastError(error);
    return result;
}

int WINAPI recv_from(SOCKET socket, char* data, int size, int flags, sockaddr* from, int* length)
{
    const int result = g_recv_from(socket, data, size, flags, from, length);
    const DWORD error = GetLastError();
    if (result != SOCKET_ERROR) {
        if (from != nullptr && length != nullptr && *length >= static_cast<int>(sizeof(sockaddr_in)) &&
            from->sa_family == AF_INET)
            on_received(*reinterpret_cast<const sockaddr_in*>(from), result);
    } else if (error == WSAEWOULDBLOCK) {
        on_nothing_yet(socket);
    } else {
        on_receive_error(socket, error);
    }
    SetLastError(error);
    return result;
}

// The game's UDP port, kept for the whole session. The game makes a new UDP socket at every zone and asks for 54090 each
// time; a second game on this PC finds it taken and goes on to 54346 (54090 + 256). When the first game logs out, the
// second's next zone got 54090: its packets then came from the port the logged-out character's session on the map
// server still had (LandSandBoat keys sessions by address and port, and drops a logged-out one only on its next packet
// or a timeout), the zone-in joined that stale session, nothing came back, and the game gave up with FFXI-3001
// (megathread: Levelliuer, Chadbrowick, dual-boxing). Now a later ask for the first port gets the one this game had:
// the server sees one port from it all session, as with a single game.
std::atomic<std::uint16_t> g_first_wanted{0}; // the first port the game asked a UDP socket for (54090)
std::atomic<std::uint16_t> g_kept{0};         // the port it got for it (54090, or 54346 for a second game)
std::atomic<int> g_kept_lines{3};

// A port the game asks for: the first, or one of its fallbacks after it (each 256 on)
bool game_port(std::uint16_t wanted)
{
    std::uint16_t first = 0;
    g_first_wanted.compare_exchange_strong(first, wanted);
    first = g_first_wanted.load();
    return wanted >= first && (wanted - first) % 256 == 0 && wanted - first <= 256 * 16;
}

// True (bound) if the socket took the kept port in place of the first one asked for again
bool bind_kept(SOCKET socket, const sockaddr_in& asked)
{
    const std::uint16_t wanted = ntohs(asked.sin_port);
    const std::uint16_t kept = g_kept.load();
    if (kept == 0 || kept == wanted || wanted != g_first_wanted.load())
        return false;
    sockaddr_in at = asked;
    at.sin_port = htons(kept);
    if (g_bind(socket, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0) {
        if (say(g_kept_lines))
            log::write(L"Zone link: the game's port %u couldn't be kept (winsock error %d): port %u as the game asks", kept,
                       WSAGetLastError(), wanted);
        return false;
    }
    if (say(g_kept_lines))
        log::write(L"Zone link: the game asked for port %u again: given %u, its port this session (another game had %u)",
                   wanted, kept, wanted);
    return true;
}

// The game's UDP port: taken (WSAEADDRINUSE) means another program, or another game, has it
int WINAPI bind_traced(SOCKET socket, const sockaddr* at, int length)
{
    const bool udp4 = at != nullptr && length >= static_cast<int>(sizeof(sockaddr_in)) && at->sa_family == AF_INET &&
                      is_udp(socket);
    const auto* asked = reinterpret_cast<const sockaddr_in*>(at);
    const bool ours = udp4 && asked->sin_port != 0 && game_port(ntohs(asked->sin_port));
    if (ours && bind_kept(socket, *asked)) {
        SetLastError(0);
        return 0;
    }
    const int result = g_bind(socket, at, length);
    const DWORD error = GetLastError();
    if (ours && result == 0) {
        std::uint16_t none = 0;
        g_kept.compare_exchange_strong(none, local_port(socket));
    }
    if (at != nullptr && length >= static_cast<int>(sizeof(sockaddr_in)) && at->sa_family == AF_INET && is_udp(socket) &&
        say(g_binds)) {
        const unsigned port = ntohs(reinterpret_cast<const sockaddr_in*>(at)->sin_port);
        if (result == 0)
            log::write(L"Zone link: a UDP socket on port %u", port != 0 ? port : local_port(socket));
        else
            log::write(L"Zone link: a UDP socket couldn't have port %u: winsock error %lu%ls", port, error,
                       error == WSAEADDRINUSE ? L" (another program has it: another game still running?)" : L"");
    }
    SetLastError(error);
    return result;
}

// sendto, recvfrom and bind, for the log and to keep the game's UDP port (bind_traced); false if they couldn't be
// patched (the lobby works without them)
bool install(HMODULE ws2)
{
    const auto send_to_function = reinterpret_cast<SendTo>(GetProcAddress(ws2, "sendto"));
    const auto recv_from_function = reinterpret_cast<RecvFrom>(GetProcAddress(ws2, "recvfrom"));
    const auto bind_function = reinterpret_cast<Bind>(GetProcAddress(ws2, "bind"));
    return send_to_function != nullptr && recv_from_function != nullptr && bind_function != nullptr &&
           imports::detour_auto(send_to_function, &send_to, g_send_to) &&
           imports::detour_auto(recv_from_function, &recv_from, g_recv_from) &&
           imports::detour_auto(bind_function, &bind_traced, g_bind);
}

} // namespace zonelink
