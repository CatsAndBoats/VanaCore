#pragma once

#include <cstdint>
#include <string>
#include <vector>

// An item description's stats as the Equipment window's stats (equipstats.cpp) read them, for an item CatsEyeXI has
// changed since VanaCore's item data was made (its DATs come through Pivot without a VanaCore release): each signed number
// the description gives before its first condition ("Latent effect:", "Set:", "Pet:"...), with the words before it. Kept
// in step with tools/gen_gamedata.py's desc_ascii, desc_fingerprint and desc_parse, which made the data's "words" and
// "described" from the same reading: change one, change the other.
namespace vanacore::equipdesc {

// The description's bytes (the DAT's: cp932, element icons, auto-translate phrases) as the generator reads them (xi-tools'
// xi.mv.xi_database.decode_text, then its ASCII): an element icon as its name (Fire ... Dark), an auto-translate bracket
// and the 4 bytes after it left out, a double-byte letter left out, the rest's ASCII
std::string ascii(const char* raw);

// FNV-1a of the text's letters, digits and + - % (everything else left out): equip_mods.json's "described"
std::uint32_t fingerprint(const std::string& text);

// One stat the description gives: its label (lower case, quotes as spaces, single spaces, ending in % for a percentage)
// and its signed number
struct Said {
    std::string label;
    int value = 0;
};
std::vector<Said> parse(const std::string& text);

} // namespace vanacore::equipdesc
