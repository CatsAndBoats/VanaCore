// loader.cpp's include unit (inside its namespace): the lobby's connections to the server, plain or over TLS.
//
// xiloader 3.0 (LandSandBoat from 2026-11-05) has the lobby's data and view connections over TLS. The data link is
// VanaCore's own, so it's TLS end to end. The view connection is the game's: connect() sends it to a relay on this
// machine instead, which carries it to the server's view port over TLS, as xiloader's does. A server before 3.0 has
// both plain: the data link is a plain socket and the game dials the view port itself.

// This thread is one of VanaCore's own lobby connections: its traffic is the server's TLS records (not packets to read)
// and its connect() goes where it's sent
thread_local bool t_lobby_own = false;

constexpr DWORD kLobbyTimeoutMs = 15000; // a TLS handshake's sends and receives

std::unique_ptr<tls::Client> g_data_tls; // the data link over TLS (else g_data)
SOCKET g_view_relay = INVALID_SOCKET;    // the view relay's listener
std::atomic<std::uint16_t> g_view_relay_port{0};
std::atomic<bool> g_lobby_closing{false};
std::wstring g_server_host; // the server's name, for TLS (its address if none)

bool send_all(SOCKET socket, const char* data, int size)
{
    while (size > 0) {
        const int sent = ::send(socket, data, size, 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= sent;
    }
    return true;
}

// --- The data link -----------------------------------------------------------------------------

// Connects the data link to the server, over TLS when the login was 3.0's; false (reported) if it can't
bool data_connect(const in_addr& server)
{
    if (g_login.lobby_tls != 0) {
        t_lobby_own = true; // this thread runs the game afterwards: just for the handshake
        std::wstring error;
        g_data_tls = tls::connect(server.s_addr, g_login.data_port, g_server_host, kLobbyTimeoutMs, error);
        t_lobby_own = false;
        if (g_data_tls == nullptr) {
            report(L"couldn't connect to the lobby at %hs:%u over TLS: %ls", g_server, g_login.data_port, error.c_str());
            return false;
        }
        g_data_tls->set_timeout(0); // the server asks when the player picks a character: no time limit
        return true;
    }
    g_data = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(g_login.data_port);
    to.sin_addr = server;
    if (g_data == INVALID_SOCKET || connect(g_data, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
        report(L"couldn't connect to the lobby at %hs:%u (winsock error %d)", g_server, g_login.data_port,
               WSAGetLastError());
        return false;
    }
    return true;
}

bool data_send(const char* data, int size)
{
    if (g_data_tls != nullptr)
        return g_data_tls->write(data, static_cast<size_t>(size));
    return send_all(g_data, data, size);
}

// Appends what the server sent to `in`; false once the link is closed
bool data_receive(std::string& in)
{
    if (g_data_tls != nullptr) {
        std::wstring error;
        if (g_data_tls->read_some(in, error))
            return true;
        if (!error.empty() && !g_lobby_closing)
            log::write(L"Lobby: the data link's TLS failed: %ls", error.c_str());
        return false;
    }
    char chunk[4096];
    const int got = recv(g_data, chunk, sizeof(chunk), 0);
    if (got <= 0)
        return false;
    in.append(chunk, static_cast<size_t>(got));
    return true;
}

// The length of the server's packet at the start of `in` (it sends none): a request (who's this, the key) is 5 bytes
// and the character list 0x148, as LandSandBoat's data_session writes them. 0 while it hasn't all arrived; all of it
// for a packet VanaCore doesn't know.
size_t data_packet_size(const std::string& in)
{
    size_t size = in.size();
    switch (static_cast<unsigned char>(in[0])) {
    case 0x01:
    case 0x02:
    case 0x15:
        size = 5;
        break;
    case 0x03:
        size = 0x148;
        break;
    default:
        break;
    }
    return in.size() >= size ? size : 0;
}

// Ends the data link's thread (its receive fails)
void data_close()
{
    g_lobby_closing = true;
    if (g_data_tls != nullptr)
        g_data_tls->shut_down();
    if (g_data != INVALID_SOCKET)
        closesocket(g_data);
}

// --- The view relay ----------------------------------------------------------------------------

// The game's view connection to the server, sent to the relay on this machine instead: true, with `relay` its address
bool view_redirect(const sockaddr* to, sockaddr_in& relay)
{
    const std::uint16_t port = g_view_relay_port.load();
    if (port == 0 || t_lobby_own || to == nullptr || to->sa_family != AF_INET)
        return false;
    const auto& at = *reinterpret_cast<const sockaddr_in*>(to);
    if (at.sin_addr.s_addr != g_login.server_address || ntohs(at.sin_port) != g_login.view_port)
        return false;
    relay = at;
    relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(port);
    return true;
}

// True for the game's end of a relayed view connection (its peer is the relay)
bool view_relayed(const sockaddr_in& peer)
{
    const std::uint16_t port = g_view_relay_port.load();
    return port != 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(peer.sin_port) == port;
}

// Carries the game's view connection to the server and back until either closes
void view_pump(SOCKET game, tls::Client& server)
{
    char chunk[16384];
    std::string text;
    while (!g_lobby_closing) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(game, &readable);
        FD_SET(server.socket_handle(), &readable);
        timeval wait{0, 250000};
        const int ready = select(0, &readable, nullptr, nullptr, &wait);
        if (ready == SOCKET_ERROR)
            return;
        if (FD_ISSET(game, &readable)) {
            const int got = recv(game, chunk, sizeof(chunk), 0);
            if (got <= 0 || !server.write(chunk, static_cast<size_t>(got)))
                return;
        }
        if (FD_ISSET(server.socket_handle(), &readable)) {
            text.clear();
            std::wstring error;
            if (!server.read_some(text, error)) {
                if (!error.empty())
                    log::write(L"Lobby: the view connection's TLS failed: %ls", error.c_str());
                return;
            }
            if (!text.empty() && !send_all(game, text.data(), static_cast<int>(text.size())))
                return;
        }
    }
}

// cppcheck-suppress constParameterCallback ; CreateThread's start routine takes a void*
DWORD WINAPI view_relay_client(void* parameter)
{
    t_lobby_own = true;
    const SOCKET game = reinterpret_cast<SOCKET>(parameter);
    std::wstring error;
    const std::unique_ptr<tls::Client> server =
        tls::connect(g_login.server_address, g_login.view_port, g_server_host, kLobbyTimeoutMs, error);
    if (server == nullptr) {
        report(L"couldn't reach the lobby's view port %hs:%u over TLS: %ls", g_server, g_login.view_port, error.c_str());
    } else {
        server->set_timeout(0);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            log::write(L"Lobby: the game's view connection goes to the server over TLS");
        view_pump(game, *server);
    }
    ::shutdown(game, SD_SEND);
    closesocket(game);
    return 0;
}

DWORD WINAPI view_relay(void*)
{
    for (;;) {
        const SOCKET game = accept(g_view_relay, nullptr, nullptr);
        if (game == INVALID_SOCKET)
            break; // closed: the game is closing
        const HANDLE thread = CreateThread(nullptr, 0, &view_relay_client, reinterpret_cast<void*>(game), 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
        else
            closesocket(game);
    }
    return 0;
}

std::uint16_t listen_locally(SOCKET& listener);

// Starts the view relay when the lobby is TLS; false (reported) if it can't listen. Null `thread` for a plain lobby.
bool view_relay_start(HANDLE& thread)
{
    thread = nullptr;
    if (g_login.lobby_tls == 0)
        return true;
    const std::uint16_t port = listen_locally(g_view_relay);
    if (port == 0) {
        report(L"couldn't open the lobby's view relay on this machine (winsock error %d)", WSAGetLastError());
        return false;
    }
    thread = CreateThread(nullptr, 0, &view_relay, nullptr, 0, nullptr);
    if (thread == nullptr) {
        report(L"couldn't start the lobby's view relay (error %lu)", GetLastError());
        return false;
    }
    g_view_relay_port = port;
    log::write(L"Game start: the lobby is TLS (xiloader 3.0's); the view relay is on port %u", port);
    return true;
}

void view_relay_close()
{
    g_lobby_closing = true;
    g_view_relay_port = 0;
    if (g_view_relay != INVALID_SOCKET)
        closesocket(g_view_relay);
}
