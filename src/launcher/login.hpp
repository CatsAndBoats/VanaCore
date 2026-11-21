#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace vanacore::login {

// A bootloader version, as the login server checks it (major.minor).
using Version = std::array<int, 3>;

// The version VanaCore reports: xiloader 2.1.2's protocol. A LandSandBoat login server wants 2.1.x
// from 2026-02-12 (before, 2.0.x; from 2026-09-27, 2.2.x).
inline constexpr Version kVersion = {2, 1, 2};

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
};

// Resolves `server` (a host name or an IPv4 address) into `address` (network byte order); false,
// with the reason in `message`, if it can't.
bool resolve(const std::wstring& server, std::uint32_t& address, std::wstring& message);

// Logs in to the CatsEyeXI login server at `address`:`port` (TLS) as xiloader 2.1.2 does: one
// JSON request, one JSON reply with the account's id and the session's hash. `server` is the name
// the certificate would carry (not checked: the server's certificate is self-signed).
Result log_in(const std::wstring& server, std::uint32_t address, std::uint16_t port, const std::wstring& user,
              const std::wstring& password, const Version& version = kVersion);

} // namespace vanacore::login
