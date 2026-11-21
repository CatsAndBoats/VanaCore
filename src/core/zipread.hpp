#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Reading a zip file (an addon package, addonpkg.hpp): its central directory, and an entry's bytes, stored or deflated
// (third_party\stb's zlib decoder), checked against its CRC-32. Nothing is trusted: every offset and size is checked
// against the file, an entry is inflated into a buffer of the size its directory gives and no more, and what a reader
// can't do safely (zip64, encryption, a multi-disk archive, any other method) is refused. Any thread.
namespace vanacore::zipread {

struct Entry {
    std::string name;            // as stored: '/' between folders, a directory's ending in '/'
    std::uint16_t made_by = 0;   // the high byte is the host (3: Unix, whose mode is the external attributes' high half)
    std::uint16_t flags = 0;     // bit 0 encrypted, bit 11 the name is UTF-8
    std::uint16_t method = 0;    // 0 stored, 8 deflated
    std::uint32_t crc = 0;
    std::uint32_t packed = 0;    // its size in the file
    std::uint32_t size = 0;      // its size unpacked
    std::uint32_t external = 0;  // external attributes
    std::uint32_t offset = 0;    // of its local header
    bool directory() const { return !name.empty() && name.back() == '/'; }
    bool symlink() const { return (made_by >> 8) == 3 && ((external >> 16) & 0xF000) == 0xA000; }
};

struct Archive {
    std::string data;            // the whole file
    std::vector<Entry> entries;  // in the central directory's order
};

// Reads `path` (at most `max_bytes`) and its central directory (at most `max_entries` entries). False with `error`, in
// words for a player, when it isn't a zip this reader takes.
bool open(const std::wstring& path, std::size_t max_bytes, std::size_t max_entries, Archive& out, std::string& error);

// The same, for a zip already in memory.
bool parse(std::string data, std::size_t max_entries, Archive& out, std::string& error);

// An entry's bytes, unpacked and checked against its CRC-32. False with `error`.
bool extract(const Archive& archive, const Entry& entry, std::string& out, std::string& error);

// CRC-32 (the zip's: polynomial 0xEDB88320)
std::uint32_t crc32(const void* data, std::size_t size);

} // namespace vanacore::zipread
