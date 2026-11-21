#include "gameevents.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>

#include "chat.hpp"
#include "game.hpp"
#include "log.hpp"
#include "packets.hpp"
#include "scan.hpp"

namespace vanacore::gameevents {
namespace {

// The events, each a bit (1 << its place here). Keep in step with gameevents.hpp.
constexpr const char* kNames[] = {"login",          "logout",          "zone_out",         "zone_change",
                                  "job_change",     "subjob_change",   "level_change",     "target_change",
                                  "menu_open",      "menu_close",      "main_menu_open",   "main_menu_close",
                                  "action_menu_open", "action_menu_close", "chat_input_open", "chat_input_close"};
static_assert(std::size(kNames) <= 32);

constexpr std::uint32_t bit(const char* name)
{
    for (size_t i = 0; i < std::size(kNames); ++i) {
        const char* a = kNames[i];
        const char* b = name;
        while (*a != 0 && *a == *b)
            ++a, ++b;
        if (*a == 0 && *b == 0)
            return 1u << i;
    }
    return 0;
}

// The parts watched, each read only while an addon listens for one of its events (the session always)
constexpr std::uint32_t kZone = bit("zone_out") | bit("zone_change");
constexpr std::uint32_t kJobs = bit("job_change") | bit("subjob_change") | bit("level_change");
constexpr std::uint32_t kTarget = bit("target_change");
constexpr std::uint32_t kMenus = bit("menu_open") | bit("menu_close") | bit("main_menu_open") | bit("main_menu_close") |
                                 bit("action_menu_open") | bit("action_menu_close");
constexpr std::uint32_t kChat = bit("chat_input_open") | bit("chat_input_close");

constexpr const char* kJobNames[] = {"",    "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                     "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN", "MON"};

const char* job_name(unsigned job)
{
    return job < std::size(kJobNames) ? kJobNames[job] : "";
}

// The menu manager's (CTkMenuMng, VA 0x105ECB98) list of the windows up, from the same place itemlist.cpp finds it: +0 its
// first node; a node +0 the next, +0x10 its window; a window +4 its layout, the layout's name 16 characters at +0x46
// ("menu    menuwind").
constexpr char kWindows[] = "32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8";
constexpr std::ptrdiff_t kWindowsAt = 3;
constexpr std::ptrdiff_t kNodeNext = 0, kNodeWindow = 0x10, kWindowLayout = 4, kLayoutName = 0x46;
constexpr int kWindowsAtMost = 128;
constexpr char kMenuPrefix[] = "menu    ";

// The windows the main menu (its two pages) and the action menu are
constexpr const char* kMainMenu[] = {"menuwind", "socialme"};
constexpr const char* kActionMenu = "playermo";

// A zone-out that doesn't end in a zone-in this long after is a logout (as Ashita's login status, packets::zoning)
constexpr ULONGLONG kLogoutAfter = 1000; // ms the character must be gone (not between zones) for a logout

bool g_searched = false;
char* const* g_windows = nullptr;

using Name = char[17];

// The layouts' names of the windows up (as many as fit in `names`), trimmed: "menuwind" for "menu    menuwind". A window
// torn down while its node is still linked (a zone change) stops the walk there. How many.
int read_windows(Name* names, int most)
{
    int count = 0;
    __try {
        int nodes = 0;
        for (char* node = *g_windows; node != nullptr && nodes < kWindowsAtMost && count < most;
             node = *reinterpret_cast<char**>(node + kNodeNext), ++nodes) {
            const char* window = *reinterpret_cast<char**>(node + kNodeWindow);
            const char* layout = window != nullptr ? *reinterpret_cast<const char* const*>(window + kWindowLayout) : nullptr;
            if (layout == nullptr)
                continue;
            const char* name = layout + kLayoutName;
            int length = 16;
            if (std::memcmp(name, kMenuPrefix, sizeof kMenuPrefix - 1) == 0)
                name += sizeof kMenuPrefix - 1, length -= static_cast<int>(sizeof kMenuPrefix - 1);
            while (length > 0 && (name[length - 1] == ' ' || name[length - 1] == 0))
                --length;
            std::memcpy(names[count], name, static_cast<size_t>(length));
            names[count][length] = 0;
            ++count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

bool windows_found()
{
    if (!g_searched) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return false;
        g_searched = true;
        const std::uintptr_t match = scan::find(game, kWindows);
        if (match != 0)
            std::memcpy(&g_windows, reinterpret_cast<const void*>(match + kWindowsAt), sizeof g_windows);
        else
            log::write(L"Game events: the game's window list wasn't found in FFXiMain.dll: the menu events never fire");
    }
    return g_windows != nullptr;
}

// How things stood at the last poll, each part's `primed` false until it's been read (or while nothing listens for it)
struct Session {
    bool primed = false;
    bool in = false;           // a character in the world, or between zones
    ULONGLONG gone_since = 0;  // when the character went (not between zones), 0 while it's there
    std::string name;
    std::uint32_t id = 0;
} g_session;

struct Zone {
    bool primed = false;
    double zone_in_at = 0, zone_out_at = 0; // the last zone-in (0x00A) and zone-out (0x00B) packets' times
    std::uint16_t zone = 0;
} g_zone;

struct Jobs {
    bool primed = false;
    std::uint8_t main = 0, main_level = 0, sub = 0, sub_level = 0;
} g_jobs;

struct Target {
    bool primed = false;
    unsigned index = 0;
    std::uint32_t id = 0;
} g_target;

struct Menus {
    bool primed = false;
    std::vector<std::string> up; // the windows' names, sorted
} g_menus;

struct Chat {
    bool primed = false;
    bool open = false;
} g_chat;

Field number(const char* key, double value)
{
    return {key, Field::Number, value, {}};
}

Field text(const char* key, std::string value)
{
    return {key, Field::Text, 0, std::move(value)};
}

void add(std::uint32_t wanted, std::vector<Event>& out, const char* name, std::vector<Field> fields = {})
{
    if ((wanted & bit(name)) != 0)
        out.push_back({name, std::move(fields)});
}

// The zone the player is in (the zone-in packet's until the party list has it)
std::uint16_t current_zone()
{
    game::PartyMember members[18];
    game::Alliance alliance;
    return packets::player_zone(game::party(members, alliance) ? members[0].zone : 0);
}

void poll_session(std::uint32_t wanted, bool in_world, std::vector<Event>& out)
{
    const bool zoning = packets::zoning(in_world); // also what clears it once the player is back
    const bool in = in_world || zoning;
    const ULONGLONG now = GetTickCount64();
    if (!g_session.primed) {
        g_session = {true, in, 0, in_world ? game::local_player_name() : std::string(), in_world ? game::local_player_id() : 0};
        return;
    }
    if (in) {
        g_session.gone_since = 0;
        // another character back within packets::zoning's two minutes, never seen out: the last one logged out
        if (g_session.in && in_world && g_session.id != 0 && game::local_player_id() != g_session.id) {
            g_session.in = false;
            add(wanted, out, "logout", {text("name", g_session.name)});
        }
        if (!g_session.in && in_world) {
            g_session.in = true;
            g_session.name = game::local_player_name();
            g_session.id = game::local_player_id();
            add(wanted, out, "login",
                {text("name", g_session.name), number("id", g_session.id), number("zone", current_zone())});
        }
        return;
    }
    if (!g_session.in)
        return;
    if (g_session.gone_since == 0)
        g_session.gone_since = now;
    if (now - g_session.gone_since < kLogoutAfter)
        return;
    g_session.in = false;
    g_session.gone_since = 0;
    add(wanted, out, "logout", {text("name", g_session.name)});
}

void poll_zone(std::uint32_t wanted, std::vector<Event>& out)
{
    double zone_in_at = 0, zone_out_at = 0;
    const std::string zone_in = packets::last(true, 0x00A, &zone_in_at);
    packets::last(true, 0x00B, &zone_out_at);
    const bool logged_in = g_session.in;
    if (!g_zone.primed) {
        g_zone = {true, zone_in_at, zone_out_at, logged_in ? current_zone() : std::uint16_t(0)};
        return;
    }
    if (zone_out_at > g_zone.zone_out_at) {
        g_zone.zone_out_at = zone_out_at;
        if (logged_in)
            add(wanted, out, "zone_out", {number("zone", g_zone.zone)});
    }
    if (zone_in_at > g_zone.zone_in_at && zone_in.size() >= 0x32) {
        g_zone.zone_in_at = zone_in_at;
        std::uint16_t zone = 0;
        std::memcpy(&zone, zone_in.data() + 0x30, sizeof zone); // ZoneNo (packets.cpp zone_in)
        const std::uint16_t previous = g_zone.zone;
        g_zone.zone = zone;
        // the first zone-in of a login is the login's (its e.zone), not a change
        if (logged_in && previous != 0)
            add(wanted, out, "zone_change", {number("zone", zone), number("previous", previous)});
    }
}

void poll_jobs(std::uint32_t wanted, bool in_world, std::vector<Event>& out)
{
    game::Player player;
    // jobs read 0 for a moment after logging in: only a character in the world with a main job counts
    if (!in_world || !game::player(player) || player.main_job == 0)
        return;
    if (!g_jobs.primed) {
        g_jobs = {true, player.main_job, player.main_job_level, player.sub_job, player.sub_job_level};
        return;
    }
    if (player.main_job != g_jobs.main) {
        add(wanted, out, "job_change",
            {number("job", player.main_job), text("job_name", job_name(player.main_job)), number("level", player.main_job_level),
             number("previous", g_jobs.main), text("previous_name", job_name(g_jobs.main)),
             number("previous_level", g_jobs.main_level)});
    } else if (player.main_job_level != g_jobs.main_level) {
        add(wanted, out, "level_change",
            {number("job", player.main_job), text("job_name", job_name(player.main_job)), number("level", player.main_job_level),
             number("previous_level", g_jobs.main_level)});
    }
    if (player.sub_job != g_jobs.sub) {
        add(wanted, out, "subjob_change",
            {number("job", player.sub_job), text("job_name", job_name(player.sub_job)), number("level", player.sub_job_level),
             number("previous", g_jobs.sub), text("previous_name", job_name(g_jobs.sub)),
             number("previous_level", g_jobs.sub_level)});
    }
    g_jobs = {true, player.main_job, player.main_job_level, player.sub_job, player.sub_job_level};
}

void poll_target(std::uint32_t wanted, bool in_world, std::vector<Event>& out)
{
    game::Target target;
    if (!in_world || !game::target(target))
        return;
    const unsigned index = target.active ? target.index : 0;
    const std::uint32_t id = target.active ? target.id : 0;
    if (g_target.primed && index != g_target.index)
        add(wanted, out, "target_change",
            {number("index", index), number("id", id), number("previous_index", g_target.index), number("previous_id", g_target.id)});
    g_target = {true, index, id};
}

bool has(const std::vector<std::string>& names, const char* name)
{
    return std::binary_search(names.begin(), names.end(), std::string(name));
}

bool main_menu(const std::vector<std::string>& names)
{
    return has(names, kMainMenu[0]) || has(names, kMainMenu[1]);
}

void poll_menus(std::uint32_t wanted, std::vector<Event>& out)
{
    if (!windows_found())
        return;
    Name names[kWindowsAtMost];
    const int count = read_windows(names, kWindowsAtMost);
    std::vector<std::string> up(names, names + count);
    std::sort(up.begin(), up.end());
    up.erase(std::unique(up.begin(), up.end()), up.end());
    if (!g_menus.primed) {
        g_menus = {true, std::move(up)};
        return;
    }
    if (up == g_menus.up)
        return;
    const std::vector<std::string>& was = g_menus.up;
    for (const std::string& name : was)
        if (!has(up, name.c_str()))
            add(wanted, out, "menu_close", {text("name", name)});
    for (const std::string& name : up)
        if (!has(was, name.c_str()))
            add(wanted, out, "menu_open", {text("name", name)});
    if (main_menu(was) != main_menu(up))
        add(wanted, out, main_menu(up) ? "main_menu_open" : "main_menu_close");
    if (has(was, kActionMenu) != has(up, kActionMenu))
        add(wanted, out, has(up, kActionMenu) ? "action_menu_open" : "action_menu_close");
    g_menus.up = std::move(up);
}

void poll_chat(std::uint32_t wanted, std::vector<Event>& out)
{
    const bool open = chat::input_state() == 4; // 4 the chat line open (5 another text field, 1-3 closed)
    if (g_chat.primed && open != g_chat.open)
        add(wanted, out, open ? "chat_input_open" : "chat_input_close");
    g_chat = {true, open};
}

} // namespace

int index(std::string_view event)
{
    for (size_t i = 0; i < std::size(kNames); ++i)
        if (event == kNames[i])
            return static_cast<int>(i);
    return -1;
}

void poll(std::uint32_t wanted, std::vector<Event>& out)
{
    // The session is read whenever anything is: the zone's events go by it, and the jobs and target are only read in the
    // world.
    const bool in_world = !game::local_player_name().empty();
    if (wanted != 0)
        poll_session(wanted, in_world, out);
    else
        g_session.primed = false;
    // out of the world: the next character's jobs and target are its own, not changes (its first zone-in is the login's:
    // poll_zone)
    if (!g_session.in)
        g_jobs.primed = g_target.primed = false;
    if ((wanted & kZone) != 0)
        poll_zone(wanted, out);
    else
        g_zone.primed = false;
    if ((wanted & kJobs) != 0)
        poll_jobs(wanted, in_world, out);
    else
        g_jobs.primed = false;
    if ((wanted & kTarget) != 0)
        poll_target(wanted, in_world, out);
    else
        g_target.primed = false;
    if ((wanted & kMenus) != 0)
        poll_menus(wanted, out);
    else
        g_menus.primed = false;
    if ((wanted & kChat) != 0)
        poll_chat(wanted, out);
    else
        g_chat.primed = false;
}

} // namespace vanacore::gameevents
