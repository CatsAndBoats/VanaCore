#pragma once
// Bringing a player's Ashita or Windower setup over to VanaCore (the Addons tab, home_addons.cpp): they pick the main folder
// (Ashita's, Windower's, or the one it's in), and what's found there is copied into VanaCore's own folders, never over a
// file VanaCore's folder already has:
//
//   Ashita (v4, v3)                          VanaCore
//   addons\<name>\ (not libs)            ->  addons\ashita\<name>\
//   plugins\<name>.dll (not sdk\)        ->  plugins\ashita\<name>.dll (not Addons or Sequencer: VanaCore refuses them)
//   config\addons\, config\plugins\, a   ->  config\... (where Ashita addons and plugins look: VanaCore's folder is their
//     copied plugin's config\<name>\           install path)
//   scripts\ (but the boot script)       ->  scripts\ (for /exec and /include)
//   scripts\default.txt (else the        ->  its lines added to scripts\boot.txt: /load <p> as /plugin load <p> ashita,
//     script config\boot\*.ini names)          /addon load <a> as /addon load <a> ashita, the rest as they are
//
//   Windower (4)                             VanaCore
//   addons\<name>\ (not libs)            ->  addons\windower\<name>\ (an addon's data\ with it: GearSwap's sets)
//   plugins\<name>.dll                   ->  plugins\windower\<name>.dll
//   scripts\ (but init.txt)              ->  scripts\
//   scripts\init.txt, settings.xml's     ->  scripts\boot.txt: lua load <a> as /addon load <a> windower, the launcher's
//     autoloaded plugins                       plugins as /plugin load <p> windower, bind/alias/wait/exec/input as
//                                              VanaCore's /bind, /alias, /wait, /exec and the command; Windower's own
//                                              settings (console_*, game_*) as comments
//
// boot.txt gets the lines at its end under a comment naming where they came from, without the loads it has already (a second
// run adds nothing), and is kept first as boot.txt.before-migrate (once).

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace vcapp::migrate {

enum class Kind { Ashita, Windower };

// What a picked folder has
struct Found {
    Kind kind = Kind::Ashita;
    std::wstring root;                         // the install's own folder (the picked one, or Ashita\ / Windower4\ in it)
    std::vector<std::wstring> addons, plugins; // folder names; .dll names without .dll
    std::vector<std::wstring> configs;         // Ashita: folders under config\ to copy
    std::vector<std::wstring> scripts;         // files under scripts\ (relative), the boot script left out
    std::wstring script;                       // the boot script's path, "" when there's none
    std::vector<std::string> lines;            // its lines as boot.txt's (UTF-8), comments for the ones that aren't VanaCore's
    int loads = 0;                             // how many of `lines` load an addon or plugin
};

// `picked`'s install of `kind` into `out`; false with `why` when there's none (or it's the other kind's, or VanaCore's own)
bool find(Kind kind, const std::wstring& picked, const std::wstring& vanacore, Found& out, std::wstring& why);

struct Result {
    int addons = 0, plugins = 0, configs = 0, scripts = 0; // copied (with at least one file new)
    int files = 0, kept = 0, failed = 0;                   // files copied; already in VanaCore's folder (left); couldn't be
    int lines = 0, loads = 0;                              // boot.txt's new lines; of them, loads
    bool boot_failed = false;                              // boot.txt couldn't be written
};

// Copies what `found` has into `vanacore`'s folders and adds its lines to scripts\boot.txt. `step` is told each part as it
// starts (its name, how many parts are done, of how many). Runs on any thread.
Result run(const Found& found, const std::wstring& vanacore, const std::function<void(const std::wstring&, int, int)>& step);

const wchar_t* name_of(Kind kind);

} // namespace vcapp::migrate
