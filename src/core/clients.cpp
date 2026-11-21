#include "clients.hpp"

#include <windows.h>

#include <algorithm>
#include <string>

#include "game.hpp"
#include "ini.hpp"
#include "log.hpp"
#include "overlay.hpp"
#include "settings.hpp"

namespace vanacore::clients {
namespace {

std::wstring g_published;     // the name client.<pid>.ini says
bool g_was_published = false; // it has been written
ULONGLONG g_next = 0;         // the next look, GetTickCount64

std::wstring numbered(const wchar_t* prefix, unsigned long pid)
{
    return settings::folder() + L"\\" + prefix + std::to_wstring(pid) + L".ini";
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// the character as the game has it (Shift-JIS), "" before it's in the world
std::wstring character()
{
    const std::string name = game::local_player_name();
    if (name.empty())
        return std::wstring();
    const int size = MultiByteToWideChar(932, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (size <= 0)
        return std::wstring();
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(932, 0, name.data(), static_cast<int>(name.size()), wide.data(), size);
    return wide;
}

// <prefix><pid>.ini files in the folder: [(pid, path)]
std::vector<std::pair<unsigned long, std::wstring>> numbered_files(const wchar_t* prefix)
{
    std::vector<std::pair<unsigned long, std::wstring>> result;
    const std::wstring folder = settings::folder();
    if (folder.empty())
        return result;
    const size_t length = wcslen(prefix);
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((folder + L"\\" + prefix + L"*.ini").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return result;
    do {
        if (found.cFileName[length] < L'0' || found.cFileName[length] > L'9')
            continue;
        wchar_t* end = nullptr;
        const unsigned long pid = wcstoul(found.cFileName + length, &end, 10);
        if (end == nullptr || _wcsicmp(end, L".ini") != 0 || pid == 0)
            continue;
        result.emplace_back(pid, folder + L"\\" + found.cFileName);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return result;
}

} // namespace

std::wstring main_name()
{
    if (settings::shared().empty())
        return std::wstring();
    wchar_t name[128] = {};
    GetPrivateProfileStringW(L"characters", L"main", L"", name, 128, settings::shared().c_str());
    return name;
}

bool set_main(const std::wstring& name)
{
    if (settings::shared().empty())
        return false;
    // vanacore.ini itself, which every game sees whatever settings file it plays with: ini::set on a path that isn't the
    // session copy writes that file and nothing else
    return ini::set(settings::shared(), L"characters", L"main", name.empty() ? nullptr : name.c_str());
}

std::vector<Client> list()
{
    std::vector<Client> result;
    if (settings::folder().empty())
        return result;
    const unsigned long self = GetCurrentProcessId();
    const std::wstring main = main_name();
    for (const auto& [pid, path] : numbered_files(L"client.")) {
        if (pid != self && !settings::process_running(pid)) {
            DeleteFileW(path.c_str());
            DeleteFileW(numbered(L"import.", pid).c_str());
            continue;
        }
        Client client;
        client.pid = pid;
        client.self = pid == self;
        if (client.self) {
            client.name = character();
        } else {
            wchar_t name[128] = {};
            GetPrivateProfileStringW(L"client", L"name", L"", name, 128, path.c_str());
            client.name = name;
        }
        result.push_back(client);
    }
    const bool listed = std::any_of(result.begin(), result.end(), [](const Client& c) { return c.self; });
    if (!listed) {
        Client client;
        client.pid = self;
        client.self = true;
        client.name = character();
        result.push_back(client);
    }
    for (Client& client : result)
        client.main = !main.empty() && !client.name.empty() && _wcsicmp(client.name.c_str(), main.c_str()) == 0;
    std::sort(result.begin(), result.end(), [](const Client& a, const Client& b) { return a.pid < b.pid; });
    return result;
}

int copy_main_to_alts(std::wstring& why)
{
    const std::vector<Client> all = list();
    const auto main = std::find_if(all.begin(), all.end(), [](const Client& c) { return c.main; });
    if (main == all.end()) {
        why = main_name().empty() ? L"no Main is set" : L"the Main isn't in the game";
        return -1;
    }
    if (all.size() < 2) {
        why = L"no other game is running";
        return -1;
    }
    // the Main's settings as that game has them: its session copy
    const std::wstring source = main->self ? settings::copy() : numbered(L"vanacore.", main->pid);
    if (source.empty() || !exists(source)) {
        why = L"the Main's settings aren't there";
        return -1;
    }
    int sent = 0;
    for (const Client& alt : all) {
        if (alt.pid == main->pid)
            continue;
        if (alt.self) {
            if (settings::merge(source) >= 0) {
                overlay::settings_changed();
                ++sent;
            }
            continue;
        }
        // written whole under another name and renamed, so the game never takes up half of it
        const std::wstring temporary = numbered(L"import.", alt.pid) + L".tmp";
        if (CopyFileW(source.c_str(), temporary.c_str(), FALSE)) {
            SetFileAttributesW(temporary.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (MoveFileExW(temporary.c_str(), numbered(L"import.", alt.pid).c_str(), MOVEFILE_REPLACE_EXISTING))
                ++sent;
        }
    }
    log::write(L"Clients: the Main's settings (%ls, pid %lu) sent to %d game(s)", main->name.c_str(), main->pid, sent);
    if (sent == 0)
        why = L"couldn't hand them over";
    return sent == 0 ? -1 : sent;
}

void tick()
{
    if (settings::folder().empty())
        return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_next)
        return;
    g_next = now + 1000;

    const unsigned long self = GetCurrentProcessId();
    const std::wstring name = character();
    if (!g_was_published || name != g_published) {
        // an ANSI file: a character's name is letters
        if (WritePrivateProfileStringW(L"client", L"name", name.c_str(), numbered(L"client.", self).c_str())) {
            g_published = name;
            g_was_published = true;
        }
    }

    // settings another game handed over
    const std::wstring import = numbered(L"import.", self);
    if (exists(import)) {
        const int changed = settings::merge(import);
        DeleteFileW(import.c_str());
        if (changed > 0)
            overlay::settings_changed(); // read again at the next frame, not the next half-second look
    }
}

void shutdown()
{
    if (!g_was_published)
        return;
    DeleteFileW(numbered(L"client.", GetCurrentProcessId()).c_str());
    g_was_published = false;
}

} // namespace vanacore::clients
