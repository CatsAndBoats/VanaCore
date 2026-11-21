#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "entityhp.hpp"

// Asking CatsEyeXI's server for the maximum HP of entities by server id (its "VanaCore modules" entity max HP, packet 0x1A7;
// the protocol is in the server modules' documentation/vanacore_modules.md). The game itself keeps a maximum for the local
// player only. The server answers for the player and their party and alliance, and for the monsters, pets and trusts the
// client has been told about; anything else is "not found". A server without the module never answers: the request ends as
// "no answer" after a few seconds, so a callback always hears back.
//
// Used by addons (vana.game.request_max_hp, lua_maxhp.cpp) and plugins (the plugin API's entity_request_max_hp). An answer
// that came is also kept for a minute as that entity's hp_max (source "server") in vana.game.entity and the plugin API's
// entity record. Game thread only.
namespace vanacore::maxhp {

// Most ids one request carries
constexpr unsigned kMaxIds = 8;

enum class Status : std::uint8_t {
    Ok = 0,       // hp and hp_max are the entity's HP now and its maximum
    NotFound = 1, // the server has nothing it may say for that id (not seen by this client, or a player outside the party)
    Disabled = 2, // the server has the feature switched off
    TooFast = 3,  // asked again before the server's minimum interval (100 ms) was over
    Hidden = 4,   // the server keeps this monster's maximum secret (a boss it has marked with the HIDE_MAX_HP mob mod)
    NoAnswer = 5, // VanaCore's own, never on the wire: no reply came (the server has no such module, or the reply was lost)
};
// "ok", "not_found", "disabled", "too_fast", "hidden" or "no_answer"
const char* status_name(Status status);

struct Answer {
    std::uint32_t id = 0;
    std::uint32_t hp_max = 0; // 0 unless status is Ok
    std::uint32_t hp = 0;     // the entity's HP at the moment the server answered; 0 unless status is Ok
    Status status = Status::NoAnswer;
};

// Runs on the game's thread once per frame (from on_frame, never inside the packet itself) with one Answer per id asked, in
// the order asked, and the token ask() gave.
using Done = std::function<void(unsigned token, const Answer* answers, unsigned count)>;

// Sends the request for up to kMaxIds ids. nullptr when it is sent, `done` will be called; otherwise why not (nothing is sent
// and `done` is never called). `owner`: a plugin's name, so release() can drop what it still waits for. `token`, if given,
// gets the request's number (1 to 65535, reused only after its answer).
const char* ask(const std::uint32_t* ids, unsigned count, Done done, const std::string& owner = {}, unsigned* token = nullptr);

// Every packet the game receives or sends, from packets.cpp: reads the server's replies (0x1A7) and forgets what it knew of
// other entities when the player zones (0x00A).
void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length);

// Once per frame, before the addons' and plugins' frames: requests that got no answer in time end, and what finished runs.
void on_frame();

// A plugin went: the requests it still waits for are dropped, its callbacks never run.
void release(const std::string& owner);

// The maximum the server last said for this entity, `Source::Server`, while it is fresh (a minute); `Source::None` if none.
entityhp::Max known(std::uint32_t id);

} // namespace vanacore::maxhp
