#pragma once

#include <cstdint>
#include <string>

// Addon packages: a .vcaddon file is a zip holding addon.json (the manifest) and one addon's folder, <name>\, with
// <name>\<name>.lua in it. A player opens one (a double-click, /vana > Addons > Install from file, or a file dropped in
// <VanaCore>\addons\install\) and is asked, with what the manifest says and where the file came from, whether to install
// it; only then is anything written, into <VanaCore>\addons\vanacore\<name>\ and nowhere else.
//
// What a package may do: install an addon whose folder isn't there, or update one a package installed (the folder's
// .vcaddon.json says so), the old folder kept in <VanaCore>\backups\addons\. Never VanaCore's own addons, CatsEyeXI's
// required one, or a folder put there another way (the launcher's addons, which it would put back, or one copied in by
// hand). Every entry is checked before any is written (zip-slip, absolute paths, Windows' reserved names, symlinks, the
// file types addons ship, 32 MB a file, 256 MB in all, 5000 files). The format is in tools\vcaddon\README.md.
namespace vanacore::addonpkg {

// A package as read, before anything is written
struct Package {
    std::wstring path;              // the .vcaddon
    std::string name;               // its folder, addons\vanacore\<name>
    std::string title, version, author, description, homepage, min_vanacore;
    std::string from;               // where it came from: the URL it was downloaded from, else the file's path
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;        // unpacked
    std::string installed_version;  // the version there now ("?" when unknown), empty when it isn't installed
    std::string problem;            // why it can't be installed; empty when it can
};

// Reads and checks the package at `path` for the VanaCore in `install_dir` (with or without a trailing backslash).
// False only when it can't be read as a package at all (its `problem` says why); a package that reads but can't be
// installed here (an addon of that name already there, a newer VanaCore needed) is true with `problem` set.
bool inspect(const std::wstring& install_dir, const std::wstring& path, Package& out);

// Installs a package inspect() found nothing wrong with (it's read and checked again): unpacked beside the addon's
// folder, then put in its place, the old one moved to backups\addons\<name>-<version>-<when>\ (the last three kept).
// The addon must not be loaded meanwhile. False with `error`.
bool install(const std::wstring& install_dir, const Package& package, std::string& error);

// scripts\boot.txt loads `name` at each start: a "//addon load <name> vanacore" line added unless one loads it already.
bool load_at_start(const std::wstring& install_dir, const std::string& name);

// <VanaCore>\addons\install\, where packages wait to be offered in game
std::wstring inbox(const std::wstring& install_dir);

// Copies `path` into the inbox (a .from file beside it says where it came from), for the game to offer. False with
// `error`.
bool queue(const std::wstring& install_dir, const std::wstring& path, std::string& error);

// The game started from `install_dir` (VanaCoreInstall): .vcaddon opens with this VanaCore (the one started last), and
// a double-click knows a game is running from it.
void start(const wchar_t* install_dir);

// The VanaCore folder start() was given (no trailing backslash); empty outside the game.
const std::wstring& home();

// True while a game runs from `install_dir`.
bool game_running(const std::wstring& install_dir);

// The next package waiting in the inbox (its size steady for a second: not still being copied in), claimed by this
// game so another started from the same folder doesn't offer it too; empty when none is. Game thread.
std::wstring next_waiting();
// A package offered is done with: moved from the inbox to its done\ folder if it waited there (its .from deleted), and
// its claim let go.
void finish(const std::wstring& path);
// A package's claim let go without an answer (the prompt's addon unloaded): it's offered again.
void release(const std::wstring& path);

// Text between UTF-8 and UTF-16
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);

} // namespace vanacore::addonpkg
