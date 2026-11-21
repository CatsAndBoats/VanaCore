#pragma once

#include <string>
#include <vector>

// The games running on this PC, for the /vana window's Client > Characters section: each game says who it is in
// %TEMP%\VanaCore\client.<pid>.ini, and one character is the Main. "Copy settings" makes every other game's
// settings (its session copy of its settings file, settings.hpp) the Main's.
//
// Another game's settings are handed over as a file, import.<its pid>.ini in the same folder, which that game takes
// up at its next look (once a second) and deletes: each game writes only its own copy.
namespace vanacore::clients {

struct Client {
    unsigned long pid = 0;
    std::wstring name; // the character; "" before it's in the world
    bool self = false; // this game
    bool main = false; // the character [characters] main names
};

// Once a frame: says who this game is when it changes, and takes up settings another game handed over.
void tick();

// The games running now (a game that ended is dropped), by process id. This game is always there.
std::vector<Client> list();

// [characters] main in vanacore.ini (settings::shared: every game sees it, whatever settings file it plays with), "" if
// none.
std::wstring main_name();

// Makes `name` the Main ("" for none). False if vanacore.ini couldn't be written.
bool set_main(const std::wstring& name);

// The Main's settings to every other game running. Returns how many games they went to, or -1 with `why`.
int copy_main_to_alts(std::wstring& why);

// End of the game: its client file is deleted.
void shutdown();

} // namespace vanacore::clients
