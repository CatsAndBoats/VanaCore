#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace vanacore::login {

// A bootloader version, as the login server checks it (major.minor).
using Version = std::array<int, 3>;

// The version VanaCore reports first: xiloader 3.0.0's protocol. A LandSandBoat login server wants 2.0.x until
// 2026-02-12, 2.1.x (trust tokens) to 2026-09-26, 2.2.x (the profile server) to 2026-11-05, then 3.0.x (the lobby's
// data and view over TLS). The login only differs in 2.1's optional fields, which VanaCore always sends.
inline constexpr Version kVersion = {3, 0, 0};

// True for a version whose protocol VanaCore speaks (2.x, 3.x)
bool speaks(const Version& version);

// True if the lobby's data and view connections are TLS for this version (3.0 and up), else plain
inline bool lobby_tls(const Version& version)
{
    return version[0] >= 3;
}

// "2.0.1" (or "2.0", its patch 0) as a version; false if it isn't one (each part 0-255).
bool parse_version(const std::wstring& text, Version& version);

enum class Outcome {
    LoggedIn,
    Refused,       // the server said no: wrong username or password, banned, already logged in, ...
    ServerProblem, // it couldn't be reached, or its reply made no sense
};

struct Result {
    Outcome outcome = Outcome::ServerProblem;
    std::wstring message; // for the player when it isn't LoggedIn
    std::uint32_t account_id = 0;
    std::uint8_t session_hash[16] = {};
    Version version = {}; // the version reported (the one the server took, when LoggedIn)
};

// Resolves `server` (a host name or an IPv4 address) into `address` (network byte order); false,
// with the reason in `message`, if it can't.
bool resolve(const std::wstring& server, std::uint32_t& address, std::wstring& message);

// Logs in to the login server at `address`:`port` (TLS) as xiloader does: one JSON request, one
// JSON reply with the account's id and the session's hash. `server` is the name the certificate
// would carry (not checked: the server's certificate is self-signed). With `negotiate`, a server
// that refuses `version` for another VanaCore speaks (its message names it) is asked again with that.
Result log_in(const std::wstring& server, std::uint32_t address, std::uint16_t port, const std::wstring& user,
              const std::wstring& password, const Version& version = kVersion, bool negotiate = true);

} // namespace vanacore::login
