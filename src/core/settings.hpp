#pragma once

#include <string>

// The game's settings file: the one VanaCore.exe was given with --ini (the launcher passes each account's own,
// vanacore.<hash of the username>.ini), else vanacore.ini. Every key is optional: what the file hasn't is VanaCore's default.
//
// The game works from a session copy of it, not the file: %TEMP%\VanaCore\vanacore.<pid>.ini, plain (key=value lines, no
// comments). Every reader of the ini (the modules that poll it, GetPrivateProfile* anywhere, Ashita's and Windower's
// config calls) is given the copy's path, so it needs no change, and a change made to the file by something else while
// the game runs (the launcher, a hand edit, another client, another Claude session) is not taken up.
//
// What the game itself writes (ini::set on the copy) goes to the file as well, this key only, a few seconds after the
// game's last change (ini::save): to an account's file plain (no comments kept, a reset takes the key's line out), to
// vanacore.ini as it's laid out (a reset comments the key out where it is). [client] ini.upstream=1 takes the file's
// changes up as they happen (the old behaviour); settings::reload() takes them up once.
namespace vanacore::settings {

// Makes the session copy of `file`, binds it (ini::bind) and returns its path. Copies of processes that have ended are
// removed. Returns `file` itself when the copy can't be made (logged): the game then reads and writes it as it did.
// `shared` is vanacore.ini in VanaCore's folder, which every game on this PC sees (shared()).
std::wstring install(const std::wstring& file, const std::wstring& shared);

// Once a frame: looks at the file once a second. With [client] ini.upstream=1 a change is taken up (the copy becomes
// the file's settings); without, it's logged once and left (pending() is true). True when the copy changed (read it again).
bool tick();

// Takes the file's current contents up now, whatever ini.upstream says. False if that couldn't be done.
bool reload();

// [client] ini.upstream, as the copy has it.
bool upstream();

// The file holds a change the game hasn't taken up.
bool pending();

// The settings file's path (--ini's, else vanacore.ini; "" before install).
const std::wstring& file();

// vanacore.ini in VanaCore's folder: what every game on this PC sees, whichever settings file each plays with
// ([characters] main, clients.hpp).
const std::wstring& shared();

// The folder the session copies live in, %TEMP%\VanaCore ("" when there is no copy), and this game's own copy.
const std::wstring& folder();
const std::wstring& copy();

// Whether the process is still running (not ours to end, and it may be another user's: "running").
bool process_running(unsigned long pid);

// Makes this game's settings the same as `from`'s, another game's session copy: a key `from` has is set here, a key it
// hasn't is taken out here. Left alone: what belongs to this PC's install or this game's window ([client] pol, ffxi,
// bootloader; [server]; [mcp]; [characters]; [window] last and position*; [vana] x, y). Written the way the game writes
// (ini::set on the copy, so the file has it too). Returns the keys changed, or -1 when there is no copy or `from` isn't there.
int merge(const std::wstring& from);

// End of the game: what's still to be saved is, and the copy is deleted.
void shutdown();

} // namespace vanacore::settings
