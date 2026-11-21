#pragma once

#include <string>
#include <string_view>

// Ashita's aliases (/alias): a word that stands for a command. A line whose first word is an alias's trigger runs the
// alias's command instead, with the rest of the line after it: with /alias /rdm /lac fwd, "/rdm dt" runs "/lac fwd dt".
// LuAshitacast profiles make their toggles this way (/alias /dt /lac fwd dt). Typed lines, macros, binds, scripts and
// addons' commands are all expanded, before VanaCore, plugins, addons or the game see them.
namespace vanacore::aliases {

// [input] aliases.silent (1 by default, as Ashita's [ashita.misc] aliases.silent): /alias doesn't say what it did in the
// chat log (/alias list always lists; mistakes are always said).
void install(const wchar_t* ini);

// /alias and //alias: true if `line` is one of them (done).
//   /alias add <trigger> <command>   adds an alias (one with the same trigger is replaced)
//   /alias <trigger> <command>       the same
//   /alias (del | delete) <trigger>  takes an alias off (remove and rem too)
//   /alias clear                     takes every alias off
//   /alias list                      lists them
bool command(std::string_view line);

// `line` with its first word put in place of an alias's command (an alias's command that starts with another alias is
// expanded too, but no alias twice): true if it was one, `out` the line to run instead. Triggers match whole words, in
// any case.
bool expand(std::string_view line, std::string& out);

// IChatManager's GetSilentAliases / SetSilentAliases
bool silent();
void set_silent(bool silent);

} // namespace vanacore::aliases
