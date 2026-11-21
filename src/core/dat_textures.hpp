#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The textures in a DAT's 0x20 sections: DXT1/DXT3 (0xA1), paletted (0x01, 0x05, 0x81, 0x91, 0xB1; 32- or 16-bit
// palette entries) and 32-bit, decoded to 0xAARRGGBB rows top first. Mass Grass's catalogue builds its sheets from the
// player's own game files with these (a recipe names the DAT and the texture), so no art ships with VanaCore.
namespace vanacore::dat_textures {

struct Header {
    std::string name;       // as stored, 16 characters (e.g. "model   e_w02c  ")
    unsigned width = 0;
    unsigned height = 0;
    std::size_t offset = 0; // the section's data in the DAT
};

// The texture sections VanaCore can decode, in file order.
std::vector<Header> list(const std::vector<std::uint8_t>& dat);
// One of them; false if its data runs past the file.
bool decode(const std::vector<std::uint8_t>& dat, const Header& header, std::vector<std::uint32_t>& argb);
// `name` compared without trailing spaces. The first texture with it.
bool find(const std::vector<std::uint8_t>& dat, const std::string& name, Header& header);
// `name` without its trailing spaces and NULs.
std::string trimmed(const std::string& name);

} // namespace vanacore::dat_textures
