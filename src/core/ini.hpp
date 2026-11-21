#pragma once

#include <string>
#include <vector>

// Writes to vanacore.ini that leave it as the player laid it out, where WritePrivateProfileString would
// move things: it takes out a key's line when the key is removed and adds a key at the end of its
// section, so settings the /vana window turns off and on again drift to the bottom.
//
// No value is ever written with a comment: "1 ; note" is written as 1. The session copy, and an account's settings file
// (bind's `plain`), are plain: key=value lines and nothing else (no comments at all, a key removed is its line taken out).
namespace vanacore::ini {

// A key of a file as Windows reads it: the first [section] of a name, the first of the key in it, the value trimmed and
// without a "; comment" after it.
struct Value {
    std::wstring section, key, value;
};

// A key to set (see set): `gone` takes it out, as a nullptr value does.
struct Change {
    std::wstring section, key, value;
    bool gone = false;
};

// Sets [section] key=value in the ini file at `path`, in place:
//   - a key the section has: its value changed on its own line (the spacing, and a "; comment" after
//     the value, kept)
//   - a key it has commented out (";key=..." as vanacore.example.ini documents them): that line, made
//     the key's
//   - else a new line after the section's last key (a new section at the end of the file)
// nullptr for `value` comments the key's line out where it is (";key=value"), so setting it again
// puts it back there. Every other line is left as it was, byte for byte, in the file's own encoding
// (UTF-16 with its byte order mark, as the launcher writes it, else the ANSI code page, as Windows'
// profile functions read it). False if the file couldn't be written.
//
// With a session copy bound (bind), a `path` that is the copy is written in two places, this one key in each: the
// copy at once (what the game reads), and the settings file 3 seconds after the copy's last change (save), so a /vana
// slider being dragged writes the file once, when it's let go. The file keeps every other edit made to it meanwhile, the
// copy takes none. Each file's read-modify-write is under a named
// mutex shared by every VanaCore process (Local\VanaCore.ini.<hash of the path>), and tried again 3 times, 20 ms apart,
// when another program has the file.
bool set(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value);

// Several keys at once, one read and one write of the file (and of each file they go on to).
bool set(const std::wstring& path, const std::vector<Change>& changes);

// The copy's changes the file is still to get, written to it (all at once): once 3 seconds have passed since the last,
// or now. Once a frame (settings::tick); drop_copy writes them first.
void save(bool now = false);

// Whether the copy has changes the file is still to get.
bool waiting();

// The session copy of the ini (settings.hpp makes it): the game reads `copy`, which is `file`'s settings as they were
// taken, and set() on `copy` writes `file` as well, `plain` (an account's file: no comments kept) or as laid out
// (vanacore.ini). No copy bound (the default): set() writes `path` and nothing else.
void bind(const std::wstring& copy, const std::wstring& file, bool plain);

// The keys of the file at `path` (see Value), in the file's order. A file that isn't there has none. False if it
// couldn't be read.
bool load(const std::wstring& path, std::vector<Value>& values);

// `values` as the whole of the file at `path`, plain: a [section] line for each section (in the order they first come)
// and its key=value lines, in UTF-16 with its byte order mark. `create`: only when there's no such file yet (false when
// there is). False if it couldn't be written.
bool store(const std::wstring& path, const std::vector<Value>& values, bool create = false);

// Deletes the copy and forgets it, after the changes still to be saved are. For the end of the game.
void drop_copy();

// `written` is called after each file these functions write (set, save, store, drop_copy), once it's in place:
// inicache.cpp reads the copy whole again then.
void on_write(void (*written)());

// The keys that live in the session copy only, never saved to the file: the debug views ([graphics.effects] debug,
// sun_gate, [graphics] grass.rings.show, grass.cover.show, collision.show), so every game starts without one. settings.cpp leaves
// them out of a new copy.
bool session_only(const std::wstring& section, const std::wstring& key);

} // namespace vanacore::ini
