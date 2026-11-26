#include "ini.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vanacore::ini {
namespace {

std::mutex g_mutex; // one write at a time (the game's thread, and the /vana window's)

// The session copy (bind): what the game reads, and the settings file it was taken from (`g_plain`: written plain)
std::wstring g_copy, g_file;
bool g_plain = false;

// Set when the last file call failed because another program (or the other client) had the file: set() tries again
thread_local bool t_busy = false;

void note_error()
{
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED)
        t_busy = true;
}

std::wstring lowered(std::wstring path)
{
    for (wchar_t& c : path) {
        if (c == L'/')
            c = L'\\';
        else if (c >= L'A' && c <= L'Z')
            c = c - L'A' + L'a';
    }
    return path;
}

// One named mutex per file, shared by every VanaCore process: Local\VanaCore.ini.<fnv1a of the lowercase path>. Held
// across a read-modify-write so two clients writing the same file can't lose each other's key.
class FileLock {
public:
    explicit FileLock(const std::wstring& path)
    {
        uint32_t hash = 2166136261u;
        for (const wchar_t c : lowered(path)) {
            hash = (hash ^ (static_cast<uint32_t>(c) & 0xFF)) * 16777619u;
            hash = (hash ^ (static_cast<uint32_t>(c) >> 8)) * 16777619u;
        }
        wchar_t name[64];
        swprintf_s(name, L"Local\\VanaCore.ini.%08X", hash);
        mutex_ = CreateMutexW(nullptr, FALSE, name);
        if (mutex_ != nullptr) {
            const DWORD waited = WaitForSingleObject(mutex_, 2000);
            held_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
        }
    }
    ~FileLock()
    {
        if (mutex_ != nullptr) {
            if (held_)
                ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool held() const { return held_; }

private:
    HANDLE mutex_ = nullptr;
    bool held_ = false;
};

template <class Char>
using Text = std::basic_string<Char>;

// A line of the file: its text, and what ended it ("\r\n", "\n", or nothing for a last line without)
template <class Char>
struct Line {
    Text<Char> text;
    Text<Char> end;
};

template <class Char>
std::vector<Line<Char>> split(const Text<Char>& all)
{
    std::vector<Line<Char>> lines;
    for (size_t at = 0; at < all.size();) {
        Line<Char> line;
        const size_t newline = all.find(Char('\n'), at);
        if (newline == Text<Char>::npos) {
            line.text = all.substr(at);
            at = all.size();
        } else {
            size_t stop = newline;
            if (stop > at && all[stop - 1] == Char('\r'))
                --stop;
            line.text = all.substr(at, stop - at);
            line.end = all.substr(stop, newline + 1 - stop);
            at = newline + 1;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

template <class Char>
bool blank(Char c)
{
    return c == Char(' ') || c == Char('\t');
}

// Section and key names are compared as Windows' profile functions compare them: without case (they're
// ASCII in vanacore.ini)
template <class Char>
bool same(const Text<Char>& text, size_t from, size_t to, const std::wstring& name)
{
    if (to - from != name.size())
        return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t a = static_cast<wchar_t>(static_cast<std::make_unsigned_t<Char>>(text[from + i]));
        const wchar_t b = name[i];
        if ((a >= L'A' && a <= L'Z' ? a - L'A' + L'a' : a) != (b >= L'A' && b <= L'Z' ? b - L'A' + L'a' : b))
            return false;
    }
    return true;
}

// "[name]" (spaces allowed around): true, and where the name is
template <class Char>
bool header(const Text<Char>& text, size_t& from, size_t& to)
{
    size_t at = 0;
    while (at < text.size() && blank(text[at]))
        ++at;
    if (at >= text.size() || text[at] != Char('['))
        return false;
    const size_t close = text.find(Char(']'), at + 1);
    if (close == Text<Char>::npos)
        return false;
    from = at + 1, to = close;
    while (from < to && blank(text[from]))
        ++from;
    while (to > from && blank(text[to - 1]))
        --to;
    return true;
}

// A "key=value" line, or a commented one (";key=value"): where the key is, and where its value starts
// (after the = and the spaces after it) and ends (before a "; comment" after it, and the spaces before
// that). `semicolon` is where a commented line's ; is.
template <class Char>
struct Entry {
    bool commented = false;
    size_t semicolon = 0, key_from = 0, key_to = 0, value_from = 0, value_to = 0;
};

template <class Char>
bool entry(const Text<Char>& text, Entry<Char>& out)
{
    size_t at = 0;
    while (at < text.size() && blank(text[at]))
        ++at;
    out.commented = at < text.size() && text[at] == Char(';');
    if (out.commented) {
        out.semicolon = at++;
        while (at < text.size() && blank(text[at]))
            ++at;
    }
    const size_t equals = text.find(Char('='), at);
    if (at >= text.size() || text[at] == Char('[') || text[at] == Char(';') || equals == Text<Char>::npos)
        return false;
    out.key_from = at, out.key_to = equals;
    while (out.key_to > out.key_from && blank(text[out.key_to - 1]))
        --out.key_to;
    if (out.key_to == out.key_from)
        return false;
    out.value_from = equals + 1;
    while (out.value_from < text.size() && blank(text[out.value_from]))
        ++out.value_from;
    out.value_to = text.size();
    for (size_t i = out.value_from; i < text.size(); ++i) {
        if (text[i] == Char(';') && i > out.value_from && blank(text[i - 1])) {
            out.value_to = i;
            break;
        }
    }
    while (out.value_to > out.value_from && blank(text[out.value_to - 1]))
        --out.value_to;
    return true;
}

template <class Char>
Text<Char> encode(const std::wstring& text);

template <>
Text<wchar_t> encode<wchar_t>(const std::wstring& text)
{
    return text;
}

// as WritePrivateProfileStringW writes a file without a byte order mark: the ANSI code page
template <>
Text<char> encode<char>(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(length > 0 ? length : 0, '\0');
    if (length > 0)
        WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

// The file's text with the key set (see set); false when there's nothing to change. `plain` (VanaCore's own files): no
// comments at all (every comment line, and a "; comment" after a value, goes as the file is written), the key's line is
// key=value, nullptr takes its line out.
template <class Char>
bool edit(Text<Char>& all, const std::wstring& section, const std::wstring& key, const wchar_t* value, bool plain)
{
    std::vector<Line<Char>> lines = split(all);
    Text<Char> newline;
    for (const Line<Char>& line : lines)
        if (!line.end.empty()) {
            newline = line.end;
            break;
        }
    if (newline.empty())
        newline = encode<Char>(L"\r\n");
    const auto key_line = [&] { return encode<Char>(key + L"=" + value); };

    bool cleaned = false; // a plain file had comments
    if (plain) {
        std::vector<Line<Char>> kept;
        for (Line<Char>& line : lines) {
            size_t at = 0;
            while (at < line.text.size() && blank(line.text[at]))
                ++at;
            if (at < line.text.size() && line.text[at] == Char(';')) {
                cleaned = true;
                continue;
            }
            size_t from = 0, to = 0;
            Entry<Char> e{};
            if (!header(line.text, from, to) && entry(line.text, e)) {
                const Text<Char> bare = line.text.substr(e.key_from, e.key_to - e.key_from) + Char('=') +
                                        line.text.substr(e.value_from, e.value_to - e.value_from);
                if (bare != line.text) {
                    line.text = bare;
                    cleaned = true;
                }
            }
            kept.push_back(std::move(line));
        }
        lines = std::move(kept);
    }

    // the key set in `lines`; false when it's as asked already
    const auto set_key = [&]() -> bool {
        // the section (the first of that name, as Windows reads it) and what it has of the key
        size_t start = lines.size(), end = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            size_t from = 0, to = 0;
            if (!header(lines[i].text, from, to))
                continue;
            if (start == lines.size()) {
                if (same(lines[i].text, from, to, section))
                    start = i;
            } else {
                end = i;
                break;
            }
        }
        if (start == lines.size()) { // no such section: a new one at the end
            if (value == nullptr)
                return false;
            if (!lines.empty() && lines.back().end.empty())
                lines.back().end = newline;
            if (!lines.empty() && !lines.back().text.empty())
                lines.push_back({Text<Char>(), newline});
            lines.push_back({encode<Char>(L"[" + section + L"]"), newline});
            lines.push_back({key_line(), newline});
            return true;
        }
        size_t active = lines.size(), commented = lines.size(), last_key = start;
        Entry<Char> active_entry{}, commented_entry{};
        for (size_t i = start + 1; i < end; ++i) {
            Entry<Char> e{};
            if (!entry(lines[i].text, e))
                continue;
            if (!e.commented)
                last_key = i;
            if (!same(lines[i].text, e.key_from, e.key_to, key))
                continue;
            if (!e.commented && active == lines.size())
                active = i, active_entry = e;
            else if (e.commented && commented == lines.size())
                commented = i, commented_entry = e;
        }
        if (value == nullptr && plain) { // its lines taken out (every one: Windows would read the next)
            bool removed = false;
            for (size_t i = end; i-- > start + 1;) {
                Entry<Char> e{};
                if (entry(lines[i].text, e) && !e.commented && same(lines[i].text, e.key_from, e.key_to, key)) {
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
                    removed = true;
                }
            }
            return removed;
        }
        if (value == nullptr) { // commented out where it is
            if (active == lines.size())
                return false;
            lines[active].text.insert(active_entry.key_from, 1, Char(';'));
            return true;
        }
        if (active != lines.size()) {
            Text<Char>& text = lines[active].text;
            const Text<Char> now = text.substr(active_entry.value_from, active_entry.value_to - active_entry.value_from);
            const Text<Char> wanted = encode<Char>(value);
            if (now == wanted)
                return false;
            text = text.substr(0, active_entry.value_from) + wanted + text.substr(active_entry.value_to);
            return true;
        }
        if (commented != lines.size()) { // the commented line made the key's, its comment after the value kept
            Text<Char>& text = lines[commented].text;
            text = text.substr(0, commented_entry.semicolon) +
                   text.substr(commented_entry.key_from, commented_entry.value_from - commented_entry.key_from) + encode<Char>(value) +
                   text.substr(commented_entry.value_to);
            return true;
        }
        // after the section's last key
        if (lines[last_key].end.empty())
            lines[last_key].end = newline;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(last_key) + 1, Line<Char>{key_line(), newline});
        return true;
    };
    if (!set_key() && !cleaned)
        return false;
    all.clear();
    for (const Line<Char>& line : lines)
        all += line.text + line.end;
    return true;
}

bool read_file(const std::wstring& path, std::string& bytes)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return true; // a new file
        note_error();
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size());
    }
    CloseHandle(file);
    return ok;
}

bool write_all(const std::wstring& path, const std::string& bytes, DWORD disposition, bool flush = true)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        note_error();
        return false;
    }
    DWORD written = 0;
    const bool ok = (bytes.empty() || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) &&
                    written == bytes.size() && (!flush || FlushFileBuffers(file));
    CloseHandle(file);
    return ok;
}

// The file's contents replaced where it is (another program has it open, so no new file can be put in its place): under
// an exclusive lock of the whole file, taken before anything is written. Windows' profile functions lock the file while
// they read it, and the open (CREATE_ALWAYS) emptied the file at once, so a write their lock then refused
// (ERROR_LOCK_VIOLATION) left it empty: the session copy, read by another thread as Vanilla UI wrote its keys, lost every
// setting (2026-11-10). Refused now, the file is as it was and set() tries again.
bool overwrite(const std::wstring& path, const std::string& bytes, bool flush)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        note_error();
        return false;
    }
    // a profile function's read holds its lock for well under a millisecond: waited for, a little
    OVERLAPPED whole{};
    bool locked = false;
    for (int tries = 0; !locked && tries < 100; ++tries) {
        locked = LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &whole) != 0;
        if (!locked && GetLastError() != ERROR_LOCK_VIOLATION)
            break;
        if (!locked)
            Sleep(tries < 20 ? 0 : 1);
    }
    if (!locked) {
        note_error();
        CloseHandle(file);
        return false;
    }
    DWORD written = 0;
    const bool ok = (bytes.empty() || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) &&
                    written == bytes.size() && SetEndOfFile(file) && (!flush || FlushFileBuffers(file));
    if (!ok)
        note_error();
    UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &whole);
    CloseHandle(file);
    return ok;
}

// The new contents whole: a file beside it, put in its place (so a read meanwhile sees the old or the
// new, not half of each); written over the old if that can't be done (another program has it open).
// durable: on the disk before it's put in place, and put there as ReplaceFile does (the old one's attributes kept).
// The session copy isn't (a file of the session's own, gone when the game ends): the flush and ReplaceFile were some
// 32 of the 41 ms each change took, and a /vana slider dragged changes it every frame.
bool replace_file(const std::wstring& path, const std::string& bytes, bool durable)
{
    const std::wstring temporary = path + L".writing";
    if (write_all(temporary, bytes, CREATE_ALWAYS, durable)) {
        if ((durable && ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) ||
            MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)))
            return true;
        DeleteFileW(temporary.c_str());
    }
    return overwrite(path, bytes, durable);
}

std::atomic<void (*)()> g_written{nullptr}; // on_write's

void written()
{
    if (const auto call = g_written.load())
        call();
}

bool write_file(const std::wstring& path, const std::string& bytes, bool durable = true)
{
    const bool ok = replace_file(path, bytes, durable);
    written();
    return ok;
}

} // namespace

namespace {

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// A value as it's written: without a "; comment" after it (a ; with a space or tab before it, as entry() reads one),
// and the spaces before that
std::wstring bare(const std::wstring& value)
{
    std::wstring text = value;
    for (size_t i = 1; i < text.size(); ++i) {
        if (text[i] == L';' && (text[i - 1] == L' ' || text[i - 1] == L'\t')) {
            text.resize(i);
            break;
        }
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
        text.pop_back();
    return text;
}

// The keys set in one file, as the file's own encoding has it, in one read and one write (no lock of ours, no retries);
// durable as write_file
bool edit_file(const std::wstring& path, const std::vector<Change>& changes, bool plain, bool durable)
{
    std::string bytes;
    if (!read_file(path, bytes))
        return false;
    const auto apply = [&changes, plain](auto& text) {
        bool changed = false;
        for (const Change& change : changes)
            changed = edit(text, change.section, change.key, change.gone ? nullptr : change.value.c_str(), plain) || changed;
        return changed;
    };
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        // UTF-16 LE with its byte order mark
        std::wstring text((bytes.size() - 2) / 2, L'\0');
        memcpy(text.data(), bytes.data() + 2, text.size() * 2);
        if (!apply(text))
            return true;
        std::string out("\xFF\xFE", 2);
        out.append(reinterpret_cast<const char*>(text.data()), text.size() * 2);
        return write_file(path, out, durable);
    }
    // bytes: a UTF-8 byte order mark kept out of the way of the first line
    const bool bom = bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0;
    std::string text = bom ? bytes.substr(3) : bytes;
    if (!apply(text))
        return true;
    return write_file(path, bom ? "\xEF\xBB\xBF" + text : text, durable);
}

// edit_file under the file's cross-process lock, and tried again (3 x 20 ms) while another program has the file
bool set_keys(const std::wstring& path, const std::vector<Change>& changes, bool plain, bool durable = true)
{
    for (int attempt = 0;; ++attempt) {
        t_busy = false;
        bool ok = false;
        {
            const FileLock lock(path);
            ok = lock.held() && edit_file(path, changes, plain, durable);
        }
        if (ok || !t_busy || attempt >= 3)
            return ok;
        Sleep(20);
    }
}

// The copy's changes the file is still to get (the latest of each key, in the order they were first made), written once
// none has come for kSaveAfter: a /vana slider being dragged writes its key every frame
constexpr ULONGLONG kSaveAfter = 3000;
std::vector<Change> g_pending;
ULONGLONG g_save_at = 0;

void queue(Change change)
{
    for (Change& pending : g_pending)
        if (lowered(pending.section) == lowered(change.section) && lowered(pending.key) == lowered(change.key)) {
            pending = std::move(change);
            return;
        }
    g_pending.push_back(std::move(change));
}

// The pending changes written to the file (g_mutex held): once their time has come, or `now`. Kept to try again later
// if the file can't be written; a file that isn't there is left that way.
void save_pending(bool now)
{
    if (g_pending.empty() || (!now && GetTickCount64() < g_save_at))
        return;
    if (!exists(g_file)) {
        g_pending.clear();
        return;
    }
    if (set_keys(g_file, g_pending, g_plain))
        g_pending.clear();
    else
        g_save_at = GetTickCount64() + kSaveAfter;
}

// The text of a file's bytes: UTF-16 with its byte order mark, UTF-8 with one, else the ANSI code page (as Windows'
// profile functions read them)
std::wstring decode(const std::string& bytes)
{
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring text((bytes.size() - 2) / 2, L'\0');
        memcpy(text.data(), bytes.data() + 2, text.size() * 2);
        return text;
    }
    const bool utf8 = bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0;
    const char* from = bytes.data() + (utf8 ? 3 : 0);
    const int size = static_cast<int>(bytes.size() - (utf8 ? 3 : 0));
    const UINT page = utf8 ? CP_UTF8 : CP_ACP;
    const int length = size > 0 ? MultiByteToWideChar(page, 0, from, size, nullptr, 0) : 0;
    std::wstring text(length > 0 ? length : 0, L'\0');
    if (length > 0)
        MultiByteToWideChar(page, 0, from, size, text.data(), length);
    return text;
}

} // namespace

bool set(const std::wstring& path, const std::vector<Change>& asked)
{
    if (path.empty())
        return false;
    std::vector<Change> changes;
    for (const Change& change : asked)
        if (!change.section.empty() && !change.key.empty())
            changes.push_back({change.section, change.key, change.gone ? std::wstring() : bare(change.value), change.gone});
    if (changes.empty())
        return true;
    std::lock_guard lock(g_mutex);
    if (g_copy.empty() || lowered(path) != lowered(g_copy))
        return set_keys(path, changes, false);
    // The session copy: the copy now (what the game reads), and the file later, these keys only (it keeps whatever else was
    // changed in it, and the copy takes none of that): kSaveAfter after the last change, so a slider dragged writes it
    // once. A file that isn't there is left that way, not made out of one key.
    if (!set_keys(g_copy, changes, true, false))
        return false;
    if (exists(g_file)) {
        for (Change& change : changes)
            if (!session_only(change.section, change.key))
                queue(std::move(change));
        g_save_at = GetTickCount64() + kSaveAfter;
    }
    return true;
}

bool set(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    if (section == nullptr || key == nullptr)
        return false;
    return set(path, std::vector<Change>{{section, key, value != nullptr ? value : L"", value == nullptr}});
}

void save(bool now)
{
    std::lock_guard lock(g_mutex);
    save_pending(now);
}

bool waiting()
{
    std::lock_guard lock(g_mutex);
    return !g_pending.empty();
}

void bind(const std::wstring& copy, const std::wstring& file, bool plain)
{
    std::lock_guard lock(g_mutex);
    g_copy = copy, g_file = file, g_plain = plain;
    g_pending.clear();
}

bool load(const std::wstring& path, std::vector<Value>& values)
{
    values.clear();
    std::string bytes;
    if (!read_file(path, bytes))
        return false;
    const std::wstring all = decode(bytes);
    std::unordered_set<std::wstring> sections, keys; // lowered, as seen: the first of each is Windows'
    std::wstring section, lowered_section;
    bool reading = false; // in the first [section] of its name
    for (const Line<wchar_t>& line : split(all)) {
        size_t from = 0, to = 0;
        if (header(line.text, from, to)) {
            section = line.text.substr(from, to - from);
            lowered_section = lowered(section);
            reading = sections.insert(lowered_section).second;
            continue;
        }
        Entry<wchar_t> e{};
        if (!reading || !entry(line.text, e) || e.commented)
            continue;
        std::wstring key = line.text.substr(e.key_from, e.key_to - e.key_from);
        if (!keys.insert(lowered_section + L'\n' + lowered(key)).second)
            continue;
        values.push_back({section, std::move(key), line.text.substr(e.value_from, e.value_to - e.value_from)});
    }
    return true;
}

bool store(const std::wstring& path, const std::vector<Value>& values, bool create)
{
    if (path.empty())
        return false;
    // the sections in the order they first come, each with its keys (the first of each)
    std::vector<std::pair<std::wstring, std::vector<const Value*>>> sections;
    std::unordered_map<std::wstring, size_t> section_at; // lowered name -> its place in `sections`
    std::unordered_set<std::wstring> keys;
    for (const Value& value : values) {
        if (value.section.empty() || value.key.empty())
            continue;
        const std::wstring name = lowered(value.section);
        if (!keys.insert(name + L'\n' + lowered(value.key)).second)
            continue;
        const auto [at, added] = section_at.emplace(name, sections.size());
        if (added)
            sections.push_back({value.section, {}});
        sections[at->second].second.push_back(&value);
    }
    std::wstring text;
    for (const auto& [name, entries] : sections) {
        if (!text.empty())
            text += L"\r\n";
        text += L"[" + name + L"]\r\n";
        for (const Value* value : entries)
            text += value->key + L"=" + bare(value->value) + L"\r\n";
    }
    std::string bytes("\xFF\xFE", 2);
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * 2);

    std::lock_guard lock(g_mutex);
    for (int attempt = 0;; ++attempt) {
        t_busy = false;
        bool ok = false;
        {
            const FileLock file_lock(path);
            if (file_lock.held()) {
                ok = create ? write_all(path, bytes, CREATE_NEW) : write_file(path, bytes);
                if (ok && create)
                    written();
            }
        }
        if (ok || !t_busy || attempt >= 3)
            return ok;
        Sleep(20);
    }
}

void drop_copy()
{
    std::lock_guard lock(g_mutex);
    if (g_copy.empty())
        return;
    save_pending(true);
    DeleteFileW(g_copy.c_str());
    DeleteFileW((g_copy + L".writing").c_str());
    written();
    g_copy.clear(), g_file.clear();
    g_pending.clear();
}

void on_write(void (*callback)())
{
    g_written.store(callback);
}

bool session_only(const std::wstring& section, const std::wstring& key)
{
    const std::wstring s = lowered(section), k = lowered(key);
    return (s == L"graphics.effects" && (k == L"debug" || k == L"sun_gate")) ||
           (s == L"graphics" && (k == L"grass.rings.show" || k == L"grass.cover.show" || k == L"collision.show" || k == L"rain.freeze" ||
                                    k == L"weather.preview"));
}

} // namespace vanacore::ini
