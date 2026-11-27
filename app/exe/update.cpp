#include "update.h"

#include <windows.h>

#include <cstdint>
#include <vector>

#include "app_api.h"
#include "json.h"
#include "net.h"
#include "version.h"

#pragma comment(lib, "version.lib")

namespace vcapp {

namespace {

using net::Http;
using net::sha256_hex;
using net::signature_valid;
using net::signer_of;
using net::wide;
using net::write_file;

constexpr wchar_t kReleases[] = L"https://api.github.com/repos/" VCAPP_GITHUB_REPO L"/releases?per_page=50";
constexpr std::size_t kMostJson = 8u << 20, kMostDll = 64u << 20;

// ---- Versions

struct Version {
    unsigned part[4] = {};
};

bool parse_version(const std::string& text, Version& v)
{
    v = Version{};
    std::size_t at = 0;
    for (int i = 0; i < 4 && at < text.size(); ++i) {
        std::size_t used = 0;
        try {
            v.part[i] = static_cast<unsigned>(std::stoul(text.substr(at), &used));
        } catch (...) {
            return false;
        }
        at += used;
        if (at < text.size() && text[at] != '.')
            return false;
        ++at;
    }
    return true;
}

int compare(const Version& a, const Version& b)
{
    for (int i = 0; i < 4; ++i)
        if (a.part[i] != b.part[i])
            return a.part[i] < b.part[i] ? -1 : 1;
    return 0;
}

std::wstring text_of(const Version& v)
{
    std::wstring s = std::to_wstring(v.part[0]) + L"." + std::to_wstring(v.part[1]) + L"." + std::to_wstring(v.part[2]);
    return v.part[3] != 0 ? s + L"." + std::to_wstring(v.part[3]) : s;
}

// A file's version resource (FILEVERSION); false when it has none (or isn't there)
bool file_version(const std::wstring& path, Version& v)
{
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size == 0)
        return false;
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || info == nullptr)
        return false;
    v.part[0] = HIWORD(info->dwFileVersionMS), v.part[1] = LOWORD(info->dwFileVersionMS);
    v.part[2] = HIWORD(info->dwFileVersionLS), v.part[3] = LOWORD(info->dwFileVersionLS);
    return true;
}

// ---- Checks

std::wstring self_path()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return path;
}

// The newest published App release (not a draft or pre-release, tagged app-v<version>) and its version
const json::Value* newest_app(const json::Value& releases, Version& newest)
{
    const json::Value* best = nullptr;
    for (const json::Value& r : releases.items) {
        const std::string& tag = r["tag_name"].str();
        Version v;
        if (r["draft"].is_true() || r["prerelease"].is_true() || tag.rfind("app-v", 0) != 0 || !parse_version(tag.substr(5), v))
            continue;
        if (best == nullptr || compare(v, newest) > 0)
            best = &r, newest = v;
    }
    return best;
}

std::wstring asset_url(const json::Value& release, const char* name)
{
    for (const json::Value& a : release["assets"].items)
        if (a["name"].str() == name)
            return wide(a["browser_download_url"].str());
    return {};
}

// The new DLL put in place, the old one kept beside it as VanaCoreApp.dll.old: false (and nothing changed) when it can't be
bool swap_in(const std::wstring& dll, const std::wstring& fresh)
{
    const std::wstring old = dll + L".old";
    const bool had = GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (had && !MoveFileExW(dll.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    if (MoveFileExW(fresh.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    if (had)
        MoveFileExW(old.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return false;
}

} // namespace

std::wstring update_dll(const std::wstring& dir, const UpdateUi& ui)
{
    ui.status(L"Checking for updates\x2026", -1);
    const std::wstring dll = dir + L"\\VanaCoreApp.dll";
    Version mine;
    const bool have = file_version(dll, mine); // none (or no DLL at all): any release is newer, and puts one there
    Http http(L"VanaCoreApp/" L"" VCAPP_EXE_VERSION_STR);
    const auto going = [&](std::size_t, std::size_t) { return !ui.cancelled(); };
    // (VANACOREAPP_RELEASES: another releases list, for testing against a fork; what it gives must still pass every check)
    wchar_t other[1024] = {};
    const std::wstring list = GetEnvironmentVariableW(L"VANACOREAPP_RELEASES", other, 1024) > 0 ? other : kReleases;
    std::string body;
    json::Value releases;
    if (!http.get(list, L"application/vnd.github+json", kMostJson, body, going) || !json::parse(body, releases) ||
        releases.type != json::Value::Type::Array)
        return ui.cancelled() ? L"" : L"Couldn't check for updates";
    Version newest;
    const json::Value* release = newest_app(releases, newest);
    if (release == nullptr || (have && compare(newest, mine) <= 0))
        return {}; // up to date
    const std::wstring was = have ? text_of(mine) : L"none";
    const std::wstring failed = L"Couldn't update to " + text_of(newest) + L": carrying on with " + was;
    // app.json first: the DLL's API (a newer one needs a newer exe: the player's download) and its hash
    const std::wstring info_url = asset_url(*release, "app.json"), dll_url = asset_url(*release, "VanaCoreApp.dll");
    json::Value info;
    if (info_url.empty() || dll_url.empty() || !http.get(info_url, L"application/octet-stream", 64 * 1024, body, going) ||
        !json::parse(body, info))
        return ui.cancelled() ? L"" : failed;
    if (static_cast<int>(info["api"].number) != VANACOREAPP_API)
        return L"VanaCore App " + text_of(newest) + L" is out: download it to update";
    const std::string want = info["dll_sha256"].str();
    const std::wstring progress_text = L"Updating VanaCore App to " + text_of(newest) + L"\x2026";
    ui.status(progress_text, 0);
    const auto downloading = [&](std::size_t got, std::size_t total) {
        if (total > 0)
            ui.status(progress_text, static_cast<float>(got) / static_cast<float>(total));
        return !ui.cancelled();
    };
    std::string data;
    if (!http.get(dll_url, L"application/octet-stream", kMostDll, data, downloading))
        return ui.cancelled() ? L"" : failed;
    if (want.size() != 64 || sha256_hex(data) != want)
        return failed;
    // written beside it, checked as a file (signed, by whoever signed this exe; its version the release's), then swapped in
    const std::wstring fresh = dll + L".new";
    if (!write_file(fresh, data))
        return L"Couldn't update VanaCore App: its folder can't be written to";
    const std::wstring mine_signer = signer_of(self_path());
    Version got;
    const bool good = signature_valid(fresh) && (mine_signer.empty() || signer_of(fresh) == mine_signer) && file_version(fresh, got) &&
                      compare(got, newest) == 0;
    if (!good || !swap_in(dll, fresh)) {
        DeleteFileW(fresh.c_str());
        return failed;
    }
    ui.status(L"Updated to " + text_of(newest), 1);
    return {};
}

bool restore_dll(const std::wstring& dir)
{
    const std::wstring dll = dir + L"\\VanaCoreApp.dll", old = dll + L".old";
    return GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES &&
           MoveFileExW(old.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace vcapp
