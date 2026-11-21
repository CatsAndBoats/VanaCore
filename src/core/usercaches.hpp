#pragma once

#include <cstdint>
#include <string>

// The game's sort-order caches for a character's bags: USER\<id>\is.dat (inventory), sb.dat (Mog Satchel), wr.dat and
// wr_2.dat to wr_8.dat (wardrobes) and the others. Each is a 32-byte header, then panes of 81 eight-byte entries (the
// item's slot in the top 16 bits and its id in the bottom 16, then a second word): the order the bag's rows are shown in.
// A pane that lists a slot twice shows that item twice, in the Items menu and the rest (the slot holds one item: the
// bag, and the server, still have one), and a cache that does is corrupt: the game wrote the item into it again when
// it read it, at a login, and again at the next. CatsEyeXI's custom item ids (30720 and up) did that when the plugins
// that make them known were loaded after the login.
namespace vanacore::usercaches {

// The names (as "is.dat, sb.dat") of the caches of the character with id `character_id` whose first two panes list
// a slot more than once, or "" when none does (or the folder isn't there, or a file isn't the shape it's known to be:
// those are left alone, so as not to say a cache is corrupt when it's only one that isn't understood).
std::string duplicated(std::uint32_t character_id);

} // namespace vanacore::usercaches
