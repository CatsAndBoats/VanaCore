#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Game events for addons (vana.on(name, fn), the same as vana.events.on): what changed in the game since the last frame,
// found by watching its state once per frame while an addon listens for one of them. addons.cpp raises each with a table:
//   login, logout              e.name (and for login e.id, e.zone): a character is in the world / back at the title
//   zone_out                   e.zone: the server's zone-out came (the zone being left)
//   zone_change                e.zone, e.previous: the server's zone-in named another zone (not at login: login has e.zone)
//   job_change                 e.job, e.job_name, e.level, e.previous, e.previous_name, e.previous_level: the main job
//   subjob_change              the same for the support job (0 and "" for none)
//   level_change               e.job, e.job_name, e.level, e.previous_level: the main job's level (level sync too)
//   target_change              e.index, e.id, e.previous_index, e.previous_id: the main target (index 0: none)
//   menu_open, menu_close      e.name: one of the game's windows, by its layout's name after "menu    " ("menuwind", ...)
//   main_menu_open/_close      the main menu (either of its pages)
//   action_menu_open/_close    the action menu (Attack, Magic, ...)
//   chat_input_open/_close     the chat line
namespace vanacore::gameevents {

struct Field {
    const char* key;
    enum Kind { Number, Text } kind = Number;
    double number = 0;
    std::string text;
};

struct Event {
    const char* name;
    std::vector<Field> fields;
};

// Which of the game events `event` (lower case) is, or -1 when it isn't one: addons.cpp keeps the ones an addon listens
// for as bits (1 << index).
int index(std::string_view event);

// What changed since the last call, into `out`, for the events in `wanted` (their bits). A part not wanted on the last
// call (or the first call) only notes how things stand. Once per frame, on the game's thread.
void poll(std::uint32_t wanted, std::vector<Event>& out);

} // namespace vanacore::gameevents
