#include "usercaches.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "log.hpp"

namespace vanacore::usercaches {
namespace {

constexpr std::uint32_t kMagic = 0x10780208; // the first word of every one seen (51 files, three characters)
constexpr size_t kHeader = 32;
constexpr size_t kEntry = 8;
constexpr size_t kPane = 81 * kEntry;
constexpr size_t kPanes = 2; // the first two are the bag's orders; what follows them in the file isn't entries
constexpr size_t kMinimum = kHeader + kPanes * kPane;

constexpr const wchar_t* kCaches[] = {L"is", L"bs", L"cl", L"ti", L"mb", L"sb", L"sk", L"ca", L"wr", L"b2",
                                      L"wr_2", L"wr_3", L"wr_4", L"wr_5", L"wr_6", L"wr_7", L"wr_8"};

// FFXiMain.dll's folder: the FFXI folder, with a trailing backslash. Empty if it can't be told.
std::wstring game_folder()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    wchar_t file[MAX_PATH * 2] = {};
    const DWORD length = game != nullptr ? GetModuleFileNameW(game, file, static_cast<DWORD>(std::size(file))) : 0;
    if (length == 0 || length >= std::size(file))
        return {};
    std::wstring folder(file, length);
    const size_t slash = folder.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : folder.substr(0, slash + 1);
}

bool read_file(const std::wstring& path, std::vector<std::uint8_t>& out)
{
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) && size.QuadPart > 0 && size.QuadPart <= 1024 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(handle, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(handle);
    return ok;
}

// How many slots the first two panes list more than once, in all (0: none, or the file isn't a shape that's understood)
size_t repeated_slots(const std::vector<std::uint8_t>& data)
{
    std::uint32_t magic = 0;
    if (data.size() < kMinimum || (std::memcpy(&magic, data.data(), sizeof(magic)), magic != kMagic))
        return 0;
    size_t repeated = 0;
    for (size_t pane = 0; pane < kPanes; ++pane) {
        std::unordered_set<std::uint32_t> slots;
        for (size_t at = kHeader + pane * kPane; at < kHeader + (pane + 1) * kPane; at += kEntry) {
            std::uint32_t entry = 0;
            std::memcpy(&entry, data.data() + at, sizeof(entry));
            if (entry != 0 && !slots.insert(entry >> 16).second)
                ++repeated;
        }
    }
    return repeated;
}

} // namespace

std::string duplicated(std::uint32_t character_id)
{
    const std::wstring game = game_folder();
    if (game.empty() || character_id == 0)
        return {};
    wchar_t id[16] = {};
    swprintf_s(id, L"%x", character_id);
    const std::wstring folder = game + L"USER\\" + id + L"\\";

    std::string found;
    int checked = 0;
    for (const wchar_t* cache : kCaches) {
        std::vector<std::uint8_t> data;
        if (!read_file(folder + cache + L".dat", data))
            continue;
        ++checked;
        const size_t repeated = repeated_slots(data);
        if (repeated == 0)
            continue;
        char name[32];
        sprintf_s(name, "%ls.dat", cache);
        if (!found.empty())
            found += ", ";
        found += name;
        log::write(L"Sort caches: %ls%ls.dat lists %zu slot%ls more than once", folder.c_str(), cache, repeated,
                   repeated == 1 ? L"" : L"s");
    }
    log::write(L"Sort caches: %d of character %x's checked in %ls: %hs", checked, character_id, folder.c_str(),
               found.empty() ? "none lists a slot twice" : found.c_str());
    return found;
}

} // namespace vanacore::usercaches
