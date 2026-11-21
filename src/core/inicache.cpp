#include "inicache.hpp"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "imports.hpp"
#include "ini.hpp"
#include "log.hpp"

namespace vanacore::inicache {
namespace {

using GetString = DWORD(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, LPCWSTR);
using GetInt = UINT(WINAPI*)(LPCWSTR, LPCWSTR, INT, LPCWSTR);
using GetSection = DWORD(WINAPI*)(LPCWSTR, LPWSTR, DWORD, LPCWSTR);
using WriteString = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
// ntdll's, as GetPrivateProfileIntW reads a value: base 0 (0x, 0o, 0b), a sign, leading blanks, up to the first other letter
using ToInteger = LONG(NTAPI*)(const UNICODE_STRING*, ULONG, ULONG*);

GetString g_get_string = nullptr; // Windows' (VanaCore.Core.dll's imports before they were pointed here)
GetInt g_get_int = nullptr;
GetSection g_get_section = nullptr;
WriteString g_write_string = nullptr;
ToInteger g_to_integer = nullptr;

constexpr ULONGLONG kLookEvery = 500; // ms between looks at the copy's time and size on the disk

// A section as Windows reads it
struct Section {
    std::vector<wchar_t> raw; // GetPrivateProfileSectionW's: its key=value lines, each ended by a null
    std::unordered_map<std::wstring, std::wstring> values; // by lower-case key: the first of each, as GetPrivateProfileString gives it
};

std::mutex g_mutex;
std::wstring g_copy;                                    // the session copy (set once, before the imports are pointed here)
std::unordered_map<std::wstring, Section> g_sections; // by lower-case name: the first of each
std::atomic<unsigned> g_changes{1};                     // changed()'s count; the sections are the copy's at g_read
unsigned g_read = 0;
WIN32_FILE_ATTRIBUTE_DATA g_stamp{}; // the copy's on the disk as it was read
ULONGLONG g_looked = 0;

std::wstring lower(std::wstring text)
{
    for (wchar_t& c : text)
        c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

bool ours(LPCWSTR file)
{
    return file != nullptr && !g_copy.empty() && _wcsicmp(file, g_copy.c_str()) == 0;
}

// What GetPrivateProfileSectionW(`name`) gives, into `out`: false if the copy can't be read
bool read_section(LPCWSTR name, std::vector<wchar_t>& out)
{
    out.assign(16384, L'\0');
    for (;;) {
        const DWORD got = g_get_section(name, out.data(), static_cast<DWORD>(out.size()), g_copy.c_str());
        if (got + 2 < out.size()) {
            out.resize(got);
            return true;
        }
        if (out.size() >= (1u << 22))
            return false;
        out.assign(out.size() * 2, L'\0');
    }
}

// A section's lines as GetPrivateProfileString reads their keys: the key and value trimmed of blanks, a value's quotes off
void index(Section& section)
{
    const auto trim = [](const wchar_t* from, const wchar_t* to) {
        while (from < to && (*from == L' ' || *from == L'\t'))
            ++from;
        while (to > from && (to[-1] == L' ' || to[-1] == L'\t'))
            --to;
        return std::wstring(from, to);
    };
    const std::vector<wchar_t>& raw = section.raw;
    for (std::size_t at = 0; at < raw.size() && raw[at] != L'\0';) {
        const wchar_t* const line = &raw[at];
        const std::size_t length = std::wcslen(line);
        at += length + 1;
        const wchar_t* const equals = std::wcschr(line, L'=');
        if (equals == nullptr)
            continue;
        std::wstring value = trim(equals + 1, line + length);
        if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        section.values.emplace(lower(trim(line, equals)), std::move(value));
    }
}

// The copy read whole (under g_mutex): its section names, then each section by Windows' own reading
bool read_all(unsigned changes)
{
    g_sections.clear();
    std::vector<wchar_t> names(4096);
    DWORD got = 0;
    for (;;) {
        got = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), g_copy.c_str());
        if (got + 2 < names.size())
            break;
        if (names.size() >= (1u << 20))
            return false;
        names.assign(names.size() * 2, L'\0');
    }
    for (std::size_t at = 0; at < got && names[at] != L'\0';) {
        const wchar_t* const name = &names[at];
        at += std::wcslen(name) + 1;
        const auto [it, added] = g_sections.try_emplace(lower(name));
        if (!added)
            continue;
        if (!read_section(name, it->second.raw))
            return false;
        index(it->second);
    }
    g_read = changes;
    return true;
}

bool same(const WIN32_FILE_ATTRIBUTE_DATA& a, const WIN32_FILE_ATTRIBUTE_DATA& b)
{
    return CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0 && a.nFileSizeLow == b.nFileSizeLow &&
           a.nFileSizeHigh == b.nFileSizeHigh;
}

// The sections are the copy's as it is (under g_mutex): read again after a change, or one seen on the disk. False: the
// copy can't be read (Windows answers then).
bool fresh()
{
    const unsigned changes = g_changes.load();
    const ULONGLONG now = GetTickCount64();
    if (g_read == changes && now - g_looked < kLookEvery)
        return true;
    WIN32_FILE_ATTRIBUTE_DATA stamp{};
    if (!GetFileAttributesExW(g_copy.c_str(), GetFileExInfoStandard, &stamp)) {
        g_read = 0;
        return false;
    }
    g_looked = now;
    if (g_read == changes && same(stamp, g_stamp))
        return true;
    g_stamp = stamp; // (taken first: a write while it's read is a change at the next look)
    if (read_all(changes))
        return true;
    g_read = 0;
    return false;
}

// [section] key of the copy: `value` and `found` (false: not there), or false if the copy can't be read
bool lookup(LPCWSTR section, LPCWSTR key, std::wstring& value, bool& found)
{
    const std::lock_guard lock(g_mutex);
    if (!fresh())
        return false;
    found = false;
    const auto in = g_sections.find(lower(section));
    if (in == g_sections.end())
        return true;
    const auto it = in->second.values.find(lower(key));
    if (it == in->second.values.end())
        return true;
    value = it->second;
    found = true;
    return true;
}

DWORD WINAPI get_string(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR out, DWORD size, LPCWSTR file)
{
    std::wstring value;
    bool found = false;
    if (section == nullptr || key == nullptr || out == nullptr || size < 2 || !ours(file) || !lookup(section, key, value, found))
        return g_get_string(section, key, fallback, out, size, file);
    if (!found) {
        value = fallback != nullptr ? fallback : L""; // (with its trailing spaces taken off, as Windows does)
        while (!value.empty() && value.back() == L' ')
            value.pop_back();
    }
    const DWORD length = std::min(static_cast<DWORD>(value.size()), size - 1); // (cut short: size - 1, as Windows)
    std::wmemcpy(out, value.data(), length);
    out[length] = L'\0';
    return length;
}

UINT WINAPI get_int(LPCWSTR section, LPCWSTR key, INT fallback, LPCWSTR file)
{
    std::wstring value;
    bool found = false;
    if (section == nullptr || key == nullptr || g_to_integer == nullptr || !ours(file) || !lookup(section, key, value, found))
        return g_get_int(section, key, fallback, file);
    if (!found || value.empty()) // (an empty value is the default, as Windows has it)
        return static_cast<UINT>(fallback);
    value.resize(std::min<std::size_t>(value.size(), 0x7FFF));
    const UNICODE_STRING text{static_cast<USHORT>(value.size() * 2), static_cast<USHORT>(value.size() * 2), value.data()};
    ULONG number = 0;
    g_to_integer(&text, 0, &number);
    return number;
}

DWORD WINAPI get_section(LPCWSTR section, LPWSTR out, DWORD size, LPCWSTR file)
{
    if (section != nullptr && out != nullptr && size >= 2 && ours(file)) {
        const std::lock_guard lock(g_mutex);
        if (fresh()) {
            const auto in = g_sections.find(lower(section));
            const DWORD got = in == g_sections.end() ? 0 : static_cast<DWORD>(in->second.raw.size());
            // cut short as Windows does: size - 2, and two nulls
            const DWORD length = std::min(got, size - 2);
            if (length > 0)
                std::wmemcpy(out, in->second.raw.data(), length);
            out[length] = out[length + 1] = L'\0';
            return length;
        }
    }
    return g_get_section(section, out, size, file);
}

BOOL WINAPI write_string(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file)
{
    const BOOL ok = g_write_string(section, key, value, file);
    if (ours(file))
        changed();
    return ok;
}

} // namespace

void install(const std::wstring& copy)
{
    static bool done = false; // pointed here once: again would call itself
    if (done || copy.empty())
        return;
    done = true;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&install), &self))
        return;
    g_copy = copy;
    g_to_integer = reinterpret_cast<ToInteger>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlUnicodeStringToInteger"));
    ini::on_write(&changed);
    // in this order, each only once the one before is: the copy's reads (read_section) need Windows' own
    // GetPrivateProfileSectionW, and a write of it must say so before any read is answered from memory
    const bool hooked = imports::hook(self, "KERNEL32.dll", "WritePrivateProfileStringW", &write_string, g_write_string) &&
                        imports::hook(self, "KERNEL32.dll", "GetPrivateProfileSectionW", &get_section, g_get_section) &&
                        imports::hook(self, "KERNEL32.dll", "GetPrivateProfileStringW", &get_string, g_get_string) &&
                        imports::hook(self, "KERNEL32.dll", "GetPrivateProfileIntW", &get_int, g_get_int);
    log::write(hooked ? L"Settings: VanaCore reads %ls from memory, read whole again as it changes"
                      : L"Settings: VanaCore's reads of %ls couldn't be pointed at memory; Windows reads each key from the file",
               copy.c_str());
}

void changed()
{
    g_changes.fetch_add(1);
}

} // namespace vanacore::inicache
