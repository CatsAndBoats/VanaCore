#include "zipread.hpp"

#include <windows.h>

#include <array>
#include <climits>
#include <cstring>

// third_party\stb\stb_zlib.c: stb_image's zlib decoder alone
extern "C" int stbi_zlib_decode_noheader_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

namespace vanacore::zipread {

namespace {

constexpr std::uint32_t kEndSignature = 0x06054b50;
constexpr std::uint32_t kCentralSignature = 0x02014b50;
constexpr std::uint32_t kLocalSignature = 0x04034b50;
constexpr std::size_t kEndSize = 22;
constexpr std::size_t kCentralSize = 46;
constexpr std::size_t kLocalSize = 30;

std::uint16_t u16(const std::string& d, std::size_t at)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(d[at]) | (static_cast<unsigned char>(d[at + 1]) << 8));
}

std::uint32_t u32(const std::string& d, std::size_t at)
{
    return static_cast<std::uint32_t>(u16(d, at)) | (static_cast<std::uint32_t>(u16(d, at + 2)) << 16);
}

// The end of central directory record: the last one, searched for back from the end past a comment of up to 64 KB.
bool find_end(const std::string& d, std::size_t& at)
{
    if (d.size() < kEndSize)
        return false;
    const std::size_t lowest = d.size() > kEndSize + 0xFFFF ? d.size() - kEndSize - 0xFFFF : 0;
    for (std::size_t i = d.size() - kEndSize + 1; i-- > lowest;) {
        if (u32(d, i) == kEndSignature && i + kEndSize + u16(d, i + 20) == d.size()) {
            at = i;
            return true;
        }
    }
    return false;
}

bool read_entry(const std::string& d, std::size_t& at, std::size_t end, Entry& e, std::string& error)
{
    if (at + kCentralSize > end || u32(d, at) != kCentralSignature) {
        error = "its list of files is damaged";
        return false;
    }
    e.made_by = u16(d, at + 4);
    e.flags = u16(d, at + 8);
    e.method = u16(d, at + 10);
    e.crc = u32(d, at + 16);
    e.packed = u32(d, at + 20);
    e.size = u32(d, at + 24);
    const std::size_t name_len = u16(d, at + 28);
    const std::size_t extra_len = u16(d, at + 30);
    const std::size_t comment_len = u16(d, at + 32);
    e.external = u32(d, at + 38);
    e.offset = u32(d, at + 42);
    const std::size_t next = at + kCentralSize + name_len + extra_len + comment_len;
    if (next > end) {
        error = "its list of files is damaged";
        return false;
    }
    e.name.assign(d, at + kCentralSize, name_len);
    if (e.packed == 0xFFFFFFFF || e.size == 0xFFFFFFFF || e.offset == 0xFFFFFFFF) {
        error = "it's a zip64 archive (over 4 GB), which an addon package can't be";
        return false;
    }
    at = next;
    return true;
}

} // namespace

std::uint32_t crc32(const void* data, std::size_t size)
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool parse(std::string data, std::size_t max_entries, Archive& out, std::string& error)
{
    out = {};
    out.data = std::move(data);
    const std::string& d = out.data;
    std::size_t end_at = 0;
    if (!find_end(d, end_at)) {
        error = "it isn't a zip file (or it's cut short)";
        return false;
    }
    const std::uint16_t disk = u16(d, end_at + 4);
    const std::uint16_t cd_disk = u16(d, end_at + 6);
    const std::uint16_t here = u16(d, end_at + 8);
    const std::uint16_t total = u16(d, end_at + 10);
    const std::uint32_t cd_size = u32(d, end_at + 12);
    const std::uint32_t cd_offset = u32(d, end_at + 16);
    if (disk != 0 || cd_disk != 0 || here != total) {
        error = "it's a zip split over several files";
        return false;
    }
    if (total == 0xFFFF || cd_offset == 0xFFFFFFFF || cd_size == 0xFFFFFFFF) {
        error = "it's a zip64 archive (over 4 GB), which an addon package can't be";
        return false;
    }
    if (total > max_entries) {
        error = "it holds " + std::to_string(total) + " files, over the " + std::to_string(max_entries) + " a package may";
        return false;
    }
    if (static_cast<std::size_t>(cd_offset) + cd_size > end_at) {
        error = "its list of files is damaged";
        return false;
    }
    std::size_t at = cd_offset;
    const std::size_t cd_end = static_cast<std::size_t>(cd_offset) + cd_size;
    out.entries.reserve(total);
    for (std::uint16_t i = 0; i < total; ++i) {
        Entry e;
        if (!read_entry(d, at, cd_end, e, error))
            return false;
        out.entries.push_back(std::move(e));
    }
    return true;
}

bool open(const std::wstring& path, std::size_t max_bytes, std::size_t max_entries, Archive& out, std::string& error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "the file can't be opened (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size{};
    std::string data;
    bool ok = GetFileSizeEx(file, &size) != 0;
    if (ok && static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
        error = "the file is " + std::to_string(size.QuadPart / (1024 * 1024)) + " MB, over the " +
                std::to_string(max_bytes / (1024 * 1024)) + " MB a package may be";
        CloseHandle(file);
        return false;
    }
    if (ok) {
        data.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        ok = data.empty() || (ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) != 0 && got == data.size());
    }
    CloseHandle(file);
    if (!ok) {
        error = "the file can't be read";
        return false;
    }
    return parse(std::move(data), max_entries, out, error);
}

bool extract(const Archive& archive, const Entry& entry, std::string& out, std::string& error)
{
    const std::string& d = archive.data;
    out.clear();
    if ((entry.flags & 1) != 0) {
        error = entry.name + " is encrypted";
        return false;
    }
    if (entry.method != 0 && entry.method != 8) {
        error = entry.name + " is packed a way a package can't use (method " + std::to_string(entry.method) + ")";
        return false;
    }
    if (static_cast<std::size_t>(entry.offset) + kLocalSize > d.size() || u32(d, entry.offset) != kLocalSignature) {
        error = entry.name + " is damaged";
        return false;
    }
    const std::size_t start = entry.offset + kLocalSize + u16(d, entry.offset + 26) + u16(d, entry.offset + 28);
    if (start > d.size() || entry.packed > d.size() - start || entry.size > INT_MAX || entry.packed > INT_MAX) {
        error = entry.name + " is damaged";
        return false;
    }
    if (entry.method == 0) {
        if (entry.packed != entry.size) {
            error = entry.name + " is damaged";
            return false;
        }
        out.assign(d, start, entry.size);
    } else {
        out.resize(entry.size);
        const int got = stbi_zlib_decode_noheader_buffer(out.data(), static_cast<int>(out.size()), d.data() + start,
                                                         static_cast<int>(entry.packed));
        if (got < 0 || static_cast<std::uint32_t>(got) != entry.size) {
            out.clear();
            error = entry.name + " is damaged (it doesn't unpack to its size)";
            return false;
        }
    }
    if (crc32(out.data(), out.size()) != entry.crc) {
        out.clear();
        error = entry.name + " is damaged (its checksum is wrong)";
        return false;
    }
    return true;
}

} // namespace vanacore::zipread
