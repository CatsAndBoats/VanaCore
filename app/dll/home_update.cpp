// VanaCore kept up to date where VanaCoreInstaller.exe put it (its unins000.dat beside the app; a zip or a developer's build
// is left alone), as the app opens, nothing to press: the glass bar says "Checking for updates" in place of the profile and
// START; a newer published release than the installed VanaCore.Core.dll has its VanaCoreInstaller.exe downloaded (a bar),
// checked (signed, by whoever signed this app) and run silently over this folder (the player's settings, profiles and
// boot.txt kept, as any install over an install), then the profile and START come back with the new version in the head.
//
// The installer replaces VanaCoreApp.exe and .dll too, which this app has open: they're moved aside first (VanaCoreApp.exe.old,
// VanaCoreApp.dll.old: Windows lets a running file be renamed, not written), so it never has to close the app; the new ones
// run from the next start. Nothing here stops the player: anything that fails gives the profile and START back, with a line.

#include "home.h"

#include <shellapi.h>

#include <cstdint>
#include <memory>
#include <thread>

#include "net.h"
#include "version.h"

namespace vcapp {

namespace {

constexpr wchar_t kLatest[] = L"https://api.github.com/repos/" VCAPP_GITHUB_REPO L"/releases/latest";
constexpr char kSetup[] = "VanaCoreInstaller.exe";
constexpr std::size_t kMostJson = 4u << 20, kMostSetup = 512u << 20;
constexpr std::uint32_t kUpdateColour = 0xF3DE9C; // the bar's line: the head's light yellow
constexpr DWORD kMostInstall = 5 * 60 * 1000;     // ms the installer has

const wchar_t* agent()
{
    return L"VanaCoreApp/" L"" VCAPP_DLL_VERSION_STR;
}

// "0.9.11" (a leading v allowed) into its three numbers; false when it isn't one
bool parse(const std::wstring& text, unsigned (&v)[3])
{
    std::size_t at = !text.empty() && (text[0] == L'v' || text[0] == L'V') ? 1 : 0;
    for (int i = 0; i < 3; ++i) {
        if (at >= text.size() || !iswdigit(text[at]))
            return false;
        v[i] = 0;
        while (at < text.size() && iswdigit(text[at]))
            v[i] = v[i] * 10 + static_cast<unsigned>(text[at++] - L'0');
        if (i < 2 && (at >= text.size() || text[at++] != L'.'))
            return false;
    }
    return true;
}

bool newer(const std::wstring& release, const std::wstring& installed)
{
    unsigned a[3] = {}, b[3] = {};
    if (!parse(release, a) || !parse(installed, b))
        return false;
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] > b[i];
    return false;
}

// A JSON string's value after `"key":` from `from` on, "" when there's none; `end` where it stopped
std::string json_string(const std::string& body, const char* key, std::size_t from, std::size_t* end = nullptr)
{
    const std::string quoted = std::string("\"") + key + "\"";
    std::size_t at = body.find(quoted, from);
    if (at == std::string::npos)
        return {};
    at = body.find_first_not_of(" \t\r\n:", at + quoted.size());
    if (at == std::string::npos || body[at] != '"')
        return {};
    std::string value;
    for (++at; at < body.size() && body[at] != '"'; ++at) {
        if (body[at] == '\\' && at + 1 < body.size())
            ++at;
        value += body[at];
    }
    if (end != nullptr)
        *end = at;
    return value;
}

// This folder's VanaCore in use (its VanaCore.exe running, or VanaCore.Core.dll in a game): the files the update replaces.
// Another install's game (a developer's build) doesn't count.
bool in_use(const std::wstring& dir)
{
    for (const wchar_t* f : {L"\\VanaCore.exe", L"\\VanaCore.Core.dll"}) {
        const HANDLE h = CreateFileW((dir + f).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        else if (GetLastError() == ERROR_SHARING_VIOLATION)
            return true;
    }
    return false;
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// `setup` run silently over `dir` and waited for: its exit code (0: installed), -1 when it couldn't be run
long run_setup(const std::wstring& setup, const std::wstring& dir)
{
    const std::wstring args = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS \"/DIR=" + dir + L"\"";
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpFile = setup.c_str();
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || info.hProcess == nullptr)
        return -1;
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(info.hProcess, kMostInstall) != WAIT_OBJECT_0 || !GetExitCodeProcess(info.hProcess, &code))
        code = static_cast<DWORD>(-1);
    CloseHandle(info.hProcess);
    return static_cast<long>(code);
}

} // namespace

// The whole update, in the background, when this folder is the installer's (kNone at once otherwise: the footer as it was)
void Home::check_update()
{
    // last update's moved-aside exe (the DLL's .old stays: VanaCoreApp.exe goes back to it when a new DLL won't load)
    DeleteFileW((dir_ + L"\\VanaCoreApp.exe.old").c_str());
    if (vanacore_.empty() || !exists(dir_ + L"\\unins000.dat"))
        return;
    update_->stage = Update::kChecking;
    auto state = update_;
    const std::wstring installed = vanacore_, dir = dir_;
    const HWND window = window_;
    std::thread([state, installed, dir, window] {
        const auto set = [&](Update::Stage stage, const std::wstring& note = {}, const std::wstring& version = {}) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stage = stage;
                state->note = note;
                if (!version.empty())
                    state->version = version;
            }
            PostMessageW(window, ui::kRedraw, 0, 0);
        };
        net::Http http(agent());
        std::string body;
        if (!http.get(kLatest, L"application/vnd.github+json", kMostJson, body, nullptr))
            return set(Update::kNone); // (offline, GitHub down: nothing said)
        const std::wstring tag = net::wide(json_string(body, "tag_name", 0));
        if (!newer(tag, installed))
            return set(Update::kNone);
        std::wstring url;
        for (std::size_t at = 0;;) {
            const std::string u = json_string(body, "browser_download_url", at, &at);
            if (u.empty())
                break;
            if (u.size() > sizeof kSetup && u.compare(u.size() - (sizeof kSetup - 1), std::string::npos, kSetup) == 0 &&
                u[u.size() - sizeof kSetup] == '/')
                url = net::wide(u);
        }
        if (url.empty())
            return set(Update::kNone); // (a release without its installer yet: next start)
        const std::wstring version = tag.substr(tag[0] == L'v' ? 1 : 0);
        if (in_use(dir))
            return set(Update::kFailed, L"VanaCore " + version + L" is out: close the game and open this again to update");
        set(Update::kDownloading, {}, version);
        std::string data;
        int shown = -1;
        const bool got = http.get(url, L"application/octet-stream", kMostSetup, data, [&](std::size_t done, std::size_t total) {
            const int percent = total > 0 ? static_cast<int>(done * 100 / total) : 0;
            if (percent != shown) {
                shown = percent;
                state->progress = static_cast<float>(percent) / 100.0f;
                PostMessageW(window, ui::kRedraw, 0, 0);
            }
            return IsWindow(window) != FALSE; // (the app closed: stop)
        });
        if (!got)
            return set(Update::kFailed, L"Couldn't download VanaCore " + version);
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring folder = std::wstring(temp) + L"VanaCore", setup = folder + L"\\VanaCoreInstaller-" + version + L".exe";
        CreateDirectoryW(folder.c_str(), nullptr);
        if (!net::write_file(setup, data))
            return set(Update::kFailed, L"Couldn't save VanaCore " + version + L"'s installer");
        data.clear();
        data.shrink_to_fit();
        // signed, and by whoever signed this app (an unsigned app, a developer's: any valid signature)
        const std::wstring mine = net::signer_of(dir + L"\\VanaCoreApp.exe");
        if (!net::signature_valid(setup) || (!mine.empty() && net::signer_of(setup) != mine)) {
            DeleteFileW(setup.c_str());
            return set(Update::kFailed, L"VanaCore " + version + L"'s installer isn't signed as it should be");
        }
        set(Update::kInstalling);
        // this app's own files moved aside for the installer's (back again when it fails and left none)
        static const wchar_t* const kOwn[] = {L"\\VanaCoreApp.exe", L"\\VanaCoreApp.dll"};
        for (const wchar_t* f : kOwn)
            MoveFileExW((dir + f).c_str(), (dir + f + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
        const long code = run_setup(setup, dir);
        for (const wchar_t* f : kOwn)
            if (!exists(dir + f))
                MoveFileExW((dir + f + L".old").c_str(), (dir + f).c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(setup.c_str());
        if (code != 0)
            return set(Update::kFailed, L"Couldn't install VanaCore " + version + L" (" + std::to_wstring(code) + L")");
        set(Update::kDone);
    }).detach();
}

// The update's line and bar in the glass bar while it runs (true: the footer's profile and START hidden); after it, a line
// under them when there's something to say
bool Home::draw_update(float w, float h, float mid)
{
    std::wstring line;
    Update::Stage stage;
    {
        std::lock_guard<std::mutex> lock(update_->mutex);
        stage = update_->stage;
        line = stage == Update::kChecking      ? L"Checking for updates\x2026"
               : stage == Update::kDownloading ? L"Downloading VanaCore " + update_->version + L"\x2026"
               : stage == Update::kInstalling  ? L"Installing VanaCore " + update_->version + L"\x2026"
               : stage == Update::kDone        ? L"Updated to VanaCore " + update_->version
                                               : update_->note;
    }
    const bool busy = stage == Update::kChecking || stage == Update::kDownloading || stage == Update::kInstalling;
    if (busy) {
        constexpr float bw = 300;
        const bool bar = stage != Update::kChecking; // (the download's share; full while it installs)
        canvas_.text_centred(line, ui::Font::Body, w / 2, bar ? mid - 10 : mid, ui::rgb(ui::pal::white));
        if (bar)
            canvas_.bar(w / 2 - bw / 2, mid + 10, bw, stage == Update::kDownloading ? update_->progress.load() : 1.0f);
        return true;
    }
    if (!line.empty())
        canvas_.text_centred(line, ui::Font::Small, w / 2, h - 12,
                             ui::rgb(stage == Update::kFailed ? ui::pal::rose : kUpdateColour));
    return false;
}

bool Home::updating() const
{
    std::lock_guard<std::mutex> lock(update_->mutex);
    return update_->stage == Update::kChecking || update_->stage == Update::kDownloading || update_->stage == Update::kInstalling;
}

// After an update: the head's version read again from the new VanaCore.Core.dll
void Home::updated()
{
    {
        std::lock_guard<std::mutex> lock(update_->mutex);
        if (update_->stage != Update::kDone || update_->applied)
            return;
        update_->applied = true;
    }
    reload_version();
}

} // namespace vcapp
