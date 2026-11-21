#include "settings.hpp"

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "ini.hpp"
#include "log.hpp"

namespace vanacore::settings {
namespace {

std::wstring g_file;    // the settings file: --ini's, else vanacore.ini
std::wstring g_shared;  // vanacore.ini in VanaCore's folder
std::wstring g_copy;    // %TEMP%\VanaCore\vanacore.<pid>.ini: what the game reads
std::wstring g_folder;  // %TEMP%\VanaCore
ULONGLONG g_next = 0;   // the next look, GetTickCount64
bool g_told = false;    // this change has been logged
bool g_changed = false; // the file holds a change the copy doesn't, as of the last look

// A file's time and size: the file is read again only when it or the copy changed (or every 10 seconds)
struct Stamp {
    ULONGLONG time = 0, size = 0;
    bool operator==(const Stamp& other) const { return time == other.time && size == other.size; }
};
Stamp g_looked[2]; // the copy and the file, as the last look read them
ULONGLONG g_looked_at = 0;

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool named(const std::wstring& text, const wchar_t* name)
{
    return _wcsicmp(text.c_str(), name) == 0;
}

Stamp stamp(const std::wstring& path)
{
    Stamp result;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        result.time = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        result.size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    return result;
}

// vanacore.<pid>.ini of a process that is gone (it crashed, or was ended from outside)
void remove_stale(const std::wstring& folder)
{
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((folder + L"\\vanacore.*.ini").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        // vanacore.<digits>.ini
        constexpr wchar_t kPrefix[] = L"vanacore.";
        if (_wcsnicmp(found.cFileName, kPrefix, 9) != 0 || found.cFileName[9] < L'0' || found.cFileName[9] > L'9')
            continue;
        wchar_t* end = nullptr;
        const unsigned long pid = wcstoul(found.cFileName + 9, &end, 10);
        if (end == nullptr || _wcsicmp(end, L".ini") != 0 || pid == 0 || pid == GetCurrentProcessId() || process_running(pid))
            continue;
        DeleteFileW((folder + L"\\" + found.cFileName).c_str());
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

// "section\nkey" without case, as Windows compares them
std::wstring id(const ini::Value& value)
{
    std::wstring text = value.section + L'\n' + value.key;
    for (wchar_t& c : text)
        if (c >= L'A' && c <= L'Z')
            c = c - L'A' + L'a';
    return text;
}

std::map<std::wstring, const ini::Value*> index(const std::vector<ini::Value>& values)
{
    std::map<std::wstring, const ini::Value*> result;
    for (const ini::Value& value : values)
        result.emplace(id(value), &value);
    return result;
}

// `values` without the keys that live in the copy only (ini::session_only: the debug views)
std::vector<ini::Value> saved(std::vector<ini::Value> values)
{
    std::erase_if(values, [](const ini::Value& value) { return ini::session_only(value.section, value.key); });
    return values;
}

// The same keys with the same values (in any order)
bool same(const std::vector<ini::Value>& a, const std::vector<ini::Value>& b)
{
    const auto left = index(a), right = index(b);
    if (left.size() != right.size())
        return false;
    for (const auto& [key, value] : left) {
        const auto other = right.find(key);
        if (other == right.end() || other->second->value != value->value)
            return false;
    }
    return true;
}

// Whether the file holds something the copy doesn't. False while the game's own changes are still to be written to it.
bool look()
{
    if (g_copy.empty() || ini::waiting() || !exists(g_file))
        return false;
    // Reading them is a few milliseconds: not again while neither has changed. A rewrite within the same clock tick and
    // size isn't seen by the stamps, so they're read again after 10 seconds whatever they say.
    const Stamp now[2] = {stamp(g_copy), stamp(g_file)};
    const ULONGLONG at = GetTickCount64();
    if (g_looked_at != 0 && at - g_looked_at < 10000 && now[0] == g_looked[0] && now[1] == g_looked[1])
        return g_changed;
    g_looked[0] = now[0], g_looked[1] = now[1];
    g_looked_at = at;
    std::vector<ini::Value> copy, file;
    if (!ini::load(g_copy, copy) || !ini::load(g_file, file))
        return g_changed;
    return !same(saved(copy), saved(file));
}

// The file's settings over the copy's: everything changed in it since is the game's from now on
bool take_up()
{
    ini::save(true); // the game's own changes into the file first, so taking it up keeps them
    // the copy's own debug views (ini::session_only) stay as they are; the file's are never taken
    std::vector<ini::Value> file, copy;
    if (!exists(g_file) || !ini::load(g_file, file) || !ini::load(g_copy, copy))
        return false;
    file = saved(std::move(file));
    for (const ini::Value& value : copy)
        if (ini::session_only(value.section, value.key))
            file.push_back(value);
    return ini::store(g_copy, file);
}

// what merge() leaves alone: the PC's, the install's, this window's
bool kept(const ini::Value& value)
{
    const std::wstring& section = value.section;
    const std::wstring& key = value.key;
    if (named(section, L"server") || named(section, L"mcp") || named(section, L"characters") || ini::session_only(section, key))
        return true;
    if (named(section, L"client"))
        return named(key, L"pol") || named(key, L"ffxi") || named(key, L"bootloader");
    if (named(section, L"window"))
        return named(key, L"last") || named(key, L"lastsize") || _wcsnicmp(key.c_str(), L"position", 8) == 0;
    if (named(section, L"vana"))
        return named(key, L"x") || named(key, L"y");
    return false;
}

} // namespace

bool process_running(unsigned long pid)
{
    const HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return GetLastError() != ERROR_INVALID_PARAMETER; // access denied: it's there, and not ours
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

std::wstring install(const std::wstring& file, const std::wstring& shared)
{
    g_file = file;
    g_shared = shared;
    g_copy.clear(), g_folder.clear();
    if (file.empty())
        return file;
    if (!exists(file)) {
        log::write(L"Settings: %ls isn't there; the game reads and writes it as it is", file.c_str());
        return file;
    }
    wchar_t temp[MAX_PATH + 2] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length > MAX_PATH) {
        log::write(L"Settings: no temp folder for a session copy of %ls; the game reads the file itself", file.c_str());
        return file;
    }
    std::wstring folder(temp, length);
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/'))
        folder.pop_back();
    folder += L"\\VanaCore";
    CreateDirectoryW(folder.c_str(), nullptr);
    remove_stale(folder);

    const std::wstring copy = folder + L"\\vanacore." + std::to_wstring(GetCurrentProcessId()) + L".ini";
    // dllmain.cpp puts the path back in BootInfo::settings_ini, which is 260 wide. The copy is plain: what the file says,
    // without comments (a "; note" after a value isn't read as part of it), and without the debug views (ini::session_only):
    // one left in the file by an older VanaCore isn't up at the start either.
    std::vector<ini::Value> values;
    if (copy.size() >= 260 || !ini::load(file, values) || !ini::store(copy, saved(std::move(values)))) {
        log::write(L"Settings: couldn't make %ls (error %lu); the game reads %ls itself", copy.c_str(), GetLastError(),
                   file.c_str());
        return file;
    }
    // an account's file (--ini) is VanaCore's and the launcher's: written plain. vanacore.ini keeps its comments.
    std::wstring full(MAX_PATH, L'\0'), base(MAX_PATH, L'\0');
    full.resize(GetFullPathNameW(file.c_str(), MAX_PATH, full.data(), nullptr));
    base.resize(GetFullPathNameW(shared.c_str(), MAX_PATH, base.data(), nullptr));
    ini::bind(copy, file, _wcsicmp(full.c_str(), base.c_str()) != 0);
    g_copy = copy;
    g_folder = folder;
    g_next = GetTickCount64() + 1000;
    g_looked_at = 0;
    g_told = g_changed = false;
    log::write(L"Settings: working from %ls, a copy of %ls; changes to the file while the game runs are %ls", copy.c_str(),
               file.c_str(), upstream() ? L"taken up ([client] ini.upstream=1)" : L"not taken up ([client] ini.upstream=0)");
    return copy;
}

bool upstream()
{
    return !g_copy.empty() && GetPrivateProfileIntW(L"client", L"ini.upstream", 0, g_copy.c_str()) != 0;
}

bool pending()
{
    g_changed = look();
    return g_changed;
}

const std::wstring& file()
{
    return g_file;
}

const std::wstring& shared()
{
    return g_shared;
}

const std::wstring& folder()
{
    return g_folder;
}

const std::wstring& copy()
{
    return g_copy;
}

int merge(const std::wstring& from)
{
    if (g_copy.empty() || !exists(from))
        return -1;
    std::vector<ini::Value> theirs, mine;
    if (!ini::load(from, theirs) || !ini::load(g_copy, mine))
        return -1;
    const auto theirs_by_id = index(theirs), mine_by_id = index(mine);
    std::vector<ini::Change> changes;
    for (const ini::Value& value : theirs) {
        if (kept(value))
            continue;
        const auto now = mine_by_id.find(id(value));
        if (now == mine_by_id.end() || now->second->value != value.value)
            changes.push_back({value.section, value.key, value.value, false});
    }
    // what `from` doesn't have is its default: here too
    for (const ini::Value& value : mine)
        if (!kept(value) && theirs_by_id.find(id(value)) == theirs_by_id.end())
            changes.push_back({value.section, value.key, std::wstring(), true});
    if (!changes.empty() && !ini::set(g_copy, changes))
        return -1;
    log::write(L"Settings: made like %ls (%d keys changed)", from.c_str(), static_cast<int>(changes.size()));
    return static_cast<int>(changes.size());
}

bool reload()
{
    if (g_copy.empty())
        return true; // the game reads the file itself
    const bool ok = take_up();
    log::write(ok ? L"Settings: %ls taken up (reload)" : L"Settings: couldn't take %ls up (reload)", g_file.c_str());
    g_told = false;
    if (ok)
        g_changed = false;
    return ok;
}

bool tick()
{
    if (g_copy.empty())
        return false;
    ini::save(); // the game's changes into the file, once they've stopped for a few seconds
    const ULONGLONG now = GetTickCount64();
    if (now < g_next)
        return false;
    g_next = now + 1000;
    g_changed = look();
    if (!g_changed) {
        g_told = false;
        return false;
    }
    if (upstream()) {
        if (take_up()) {
            log::write(L"Settings: %ls changed on disk; taken up ([client] ini.upstream=1)", g_file.c_str());
            g_told = g_changed = false;
            return true;
        }
        if (!g_told) {
            g_told = true;
            log::write(L"Settings: %ls changed on disk; couldn't take it up (will try again)", g_file.c_str());
        }
        return false;
    }
    if (!g_told) {
        g_told = true;
        log::write(L"Settings: %ls changed on disk; not taken up ([client] ini.upstream=0)", g_file.c_str());
    }
    return false;
}

void shutdown()
{
    if (g_copy.empty())
        return;
    ini::drop_copy();
    g_copy.clear(), g_folder.clear();
}

} // namespace vanacore::settings
