#include "scan.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "sigs.hpp"

namespace vanacore::scan {
namespace {

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// The pattern's bytes, -1 for "??"; empty if it isn't a pattern.
std::vector<int> parse(const char* pattern)
{
    std::vector<int> bytes;
    for (const char* at = pattern; *at != '\0';) {
        if (*at == ' ') {
            ++at;
        } else if (*at == '?') {
            bytes.push_back(-1);
            at += at[1] == '?' ? 2 : 1;
        } else {
            const int high = hex_digit(at[0]);
            const int low = high < 0 ? -1 : hex_digit(at[1]);
            if (low < 0)
                return {};
            bytes.push_back(high << 4 | low);
            at += 2;
        }
    }
    return bytes;
}

// Matches from `begin` to `end` (exclusive of the last partial match); the `count`th, counting
// down `count` as matches go by.
const BYTE* search(const BYTE* begin, const BYTE* end, const std::vector<int>& bytes, unsigned& count)
{
    if (static_cast<size_t>(end - begin) < bytes.size())
        return nullptr;
    const BYTE* last = end - bytes.size();
    // Candidates come from memchr on the first fixed byte, which is much faster than trying each.
    size_t anchor = 0;
    while (anchor < bytes.size() && bytes[anchor] < 0)
        ++anchor;
    for (const BYTE* at = begin; at <= last; ++at) {
        if (anchor < bytes.size()) {
            const auto* next = static_cast<const BYTE*>(std::memchr(at + anchor, bytes[anchor], static_cast<size_t>(last - at) + 1));
            if (next == nullptr)
                return nullptr;
            at = next - anchor;
        }
        size_t matched = 0;
        while (matched < bytes.size() && (bytes[matched] < 0 || at[matched] == bytes[matched]))
            ++matched;
        if (matched == bytes.size()) {
            if (count == 0)
                return at;
            --count;
        }
    }
    return nullptr;
}

bool readable(const MEMORY_BASIC_INFORMATION& info)
{
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return info.State == MEM_COMMIT && (info.Protect & kReadable) != 0 && (info.Protect & PAGE_GUARD) == 0;
}

// The first match of one alternative in `module`'s executable sections, or 0
std::uintptr_t find_in(HMODULE module, const std::vector<int>& bytes)
{
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        const BYTE* begin = base + section->VirtualAddress;
        unsigned count = 0;
        if (const BYTE* found = search(begin, begin + section->Misc.VirtualSize, bytes, count))
            return reinterpret_cast<std::uintptr_t>(found);
    }
    return 0;
}

// The `count`th match of one alternative in the readable memory from `begin` for `size` bytes, or 0
std::uintptr_t find_range(std::uintptr_t begin, std::size_t size, const std::vector<int>& bytes, unsigned count)
{
    // Runs of readable pages, each searched whole (a match can span pages, not unreadable gaps).
    const std::uintptr_t end = begin + size < begin ? UINTPTR_MAX : begin + size;
    std::uintptr_t at = begin;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0)
            break;
        const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (!readable(info)) {
            at = region_end;
            continue;
        }
        std::uintptr_t run_end = region_end;
        while (run_end < end) {
            MEMORY_BASIC_INFORMATION next{};
            if (VirtualQuery(reinterpret_cast<const void*>(run_end), &next, sizeof(next)) == 0 || !readable(next))
                break;
            run_end = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
        }
        if (run_end > end)
            run_end = end;
        if (const BYTE* found = search(reinterpret_cast<const BYTE*>(at), reinterpret_cast<const BYTE*>(run_end), bytes, count))
            return reinterpret_cast<std::uintptr_t>(found);
        at = run_end;
    }
    return 0;
}

// A pattern's alternatives ("A | B": the same place compiled two ways), each parsed; none at all if any isn't a pattern
std::vector<std::vector<int>> alternatives(const char* pattern)
{
    std::vector<std::vector<int>> out;
    const std::string text(pattern);
    for (std::size_t at = 0;;) {
        const std::size_t bar = text.find('|', at);
        std::vector<int> bytes = parse(text.substr(at, bar == std::string::npos ? bar : bar - at).c_str());
        if (bytes.empty())
            return {};
        out.push_back(std::move(bytes));
        if (bar == std::string::npos)
            return out;
        at = bar + 1;
    }
}

} // namespace

std::uintptr_t find(HMODULE module, const char* pattern)
{
    if (module == nullptr)
        return 0;
    std::uintptr_t match = 0;
    for (const std::vector<int>& bytes : alternatives(pattern)) {
        if ((match = find_in(module, bytes)) != 0)
            break;
    }
    sigs::note_scan(module, pattern, match);
    return match;
}

std::uint32_t operand(std::uintptr_t site, std::ptrdiff_t at)
{
    std::uint32_t value = 0;
    if (site != 0)
        std::memcpy(&value, reinterpret_cast<const void*>(site + at), sizeof value);
    return value;
}

std::uintptr_t find(std::uintptr_t begin, std::size_t size, const char* pattern, unsigned count)
{
    if (size == 0)
        return 0;
    for (const std::vector<int>& bytes : alternatives(pattern)) {
        if (const std::uintptr_t match = find_range(begin, size, bytes, count))
            return match;
    }
    return 0;
}

} // namespace vanacore::scan
