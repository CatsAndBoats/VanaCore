// VanaCoreApp as VanaCore's installer and uninstaller (install.h).
#include "install.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

#include "version.h"

#pragma comment(lib, "winhttp.lib")

namespace vcapp {

namespace {

constexpr float kW = 560, kH = 360, kPad = 26, kLogo = 56, kBar = 80, kButtonW = 200, kButtonH = 42;
constexpr wchar_t kClass[] = L"VanaCoreApp.Install";
constexpr wchar_t kLatest[] = L"https://api.github.com/repos/" VCAPP_GITHUB_REPO L"/releases/latest";
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VanaCore";
constexpr std::uint32_t kVersionColour = 0xF3DE9C;

HMODULE self()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &module);
    return module;
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring known_folder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
        out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring trimmed(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

std::wstring wide(const std::string& s)
{
    std::wstring w(s.size(), L'\0');
    w.resize(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()))));
    return w;
}

// The quoted string after `key` in a JSON text, from `from`; "" when there's none
std::string json_string(const std::string& text, const char* key, std::size_t& from)
{
    const std::string name = std::string("\"") + key + "\"";
    std::size_t at = text.find(name, from);
    if (at == std::string::npos)
        return from = std::string::npos, std::string();
    at = text.find('"', text.find(':', at + name.size()));
    const std::size_t end = at == std::string::npos ? at : text.find('"', at + 1);
    if (end == std::string::npos)
        return from = std::string::npos, std::string();
    from = end + 1;
    return text.substr(at + 1, end - at - 1);
}

// `url` (https, redirects followed: GitHub's downloads go to its storage) to `write`, `going(got, total)` each piece (false:
// stop); false on any failure
bool http_get(const std::wstring& url, const wchar_t* accept, const std::function<bool(const char*, std::size_t)>& write,
              const std::function<bool(std::size_t, std::size_t)>& going)
{
    const wchar_t* agent = L"VanaCoreApp/" L"" VCAPP_DLL_VERSION_STR;
    HINTERNET session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
        return false;
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
    DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000; // 1.2 and 1.3
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) {
        tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
    }
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    URL_COMPONENTS uc{sizeof uc};
    uc.lpszHostName = host, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path, uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 2048;
    bool ok = false;
    HINTERNET connect = nullptr, request = nullptr;
    if (WinHttpCrackUrl(url.c_str(), 0, 0, &uc) && uc.nScheme == INTERNET_SCHEME_HTTPS &&
        (connect = WinHttpConnect(session, host, uc.nPort, 0)) != nullptr &&
        (request = WinHttpOpenRequest(connect, L"GET", (std::wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) != nullptr) {
        const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        DWORD status = 0, size = sizeof status, total = 0;
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            size = sizeof total;
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total,
                                     &size, WINHTTP_NO_HEADER_INDEX))
                total = 0;
            std::vector<char> buffer(256 * 1024);
            std::size_t got = 0;
            while (true) {
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
                    break;
                if (read == 0) {
                    ok = total == 0 || got == total;
                    break;
                }
                got += read;
                if (!write(buffer.data(), read) || !going(got, total))
                    break;
            }
        }
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connect != nullptr)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// A program run hidden and waited for: its exit code (-1: it couldn't be started)
int run_hidden(const std::wstring& exe, std::wstring command)
{
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

bool make_link(const std::wstring& path, const std::wstring& target, const std::wstring& dir, const std::wstring& about)
{
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    bool made = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(dir.c_str());
        link->SetIconLocation(target.c_str(), 0);
        link->SetDescription(about.c_str());
        made = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(path.c_str(), TRUE));
    }
    if (file != nullptr)
        file->Release();
    if (link != nullptr)
        link->Release();
    return made;
}

// Where a shortcut points ("" when it isn't one)
std::wstring link_target(const std::wstring& path)
{
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    wchar_t target[MAX_PATH] = {};
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Load(path.c_str(), STGM_READ)))
        link->GetPath(target, MAX_PATH, nullptr, 0);
    if (file != nullptr)
        file->Release();
    if (link != nullptr)
        link->Release();
    return target;
}

void set_string(HKEY key, const wchar_t* name, const std::wstring& value)
{
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void set_dword(HKEY key, const wchar_t* name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
}

// The folder's size in KB, everything under it
unsigned long long folder_kb(const std::wstring& dir)
{
    unsigned long long bytes = 0;
    std::vector<std::wstring> folders = {dir};
    for (std::size_t i = 0; i < folders.size(); ++i) {
        WIN32_FIND_DATAW found{};
        const HANDLE find = FindFirstFileW((folders[i] + L"\\*").c_str(), &found);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..")
                continue;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                folders.push_back(folders[i] + L"\\" + name);
            else
                bytes += (static_cast<unsigned long long>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return bytes / 1024;
}

std::wstring mb(std::size_t bytes)
{
    return std::to_wstring((bytes + (1 << 19)) >> 20);
}

} // namespace

bool has_vanacore(const std::wstring& dir)
{
    return exists(dir + L"\\VanaCore.exe");
}

Installer::Installer(const VanaCoreAppHost& host) : host_(host)
{
    const std::wstring programs = known_folder(FOLDERID_UserProgramFiles); // %LOCALAPPDATA%\Programs: the player's, no admin
    target_ = (programs.empty() ? known_folder(FOLDERID_LocalAppData) + L"\\Programs" : programs) + L"\\VanaCore";
    wchar_t to[MAX_PATH] = {}; // (VANACORE_INSTALL_TO: another first choice, for testing)
    if (GetEnvironmentVariableW(L"VANACORE_INSTALL_TO", to, MAX_PATH) > 0)
        target_ = trimmed(to);
}

Installer::~Installer()
{
    stopping_ = true;
    if (worker_.joinable())
        worker_.join();
}

void Installer::say(Stage stage, const std::wstring& text, float progress)
{
    {
        std::lock_guard<std::mutex> hold(lock_);
        stage_ = stage, text_ = text, progress_ = progress;
    }
    if (window_ != nullptr)
        PostMessageW(window_, ui::kRedraw, 0, 0);
}

void Installer::start()
{
    if (worker_.joinable())
        worker_.join();
    stopping_ = false;
    say(kFinding, L"Looking for the newest VanaCore\x2026");
    worker_ = std::thread([this] { work(); });
}

// The worker: the newest release's zip (VANACORE_INSTALL_ZIP: a zip of one's own, for testing), unpacked into the folder, then
// finish()
void Installer::work()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring zip, version, why;
    bool downloaded = false;
    wchar_t own[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"VANACORE_INSTALL_ZIP", own, MAX_PATH) > 0) {
        zip = own;
        const std::wstring name = zip.substr(zip.find_last_of(L"\\/") + 1);
        version = name.rfind(L"VanaCore-", 0) == 0 ? name.substr(9, name.size() - 9 - 4) : L"(local)";
    } else {
        std::string json;
        const bool listed = http_get(kLatest, L"application/vnd.github+json",
                                     [&](const char* data, std::size_t n) { json.append(data, n); return json.size() < (4u << 20); },
                                     [&](std::size_t, std::size_t) { return !stopping_; });
        std::size_t at = 0;
        version = wide(json_string(json, "tag_name", at));
        if (!version.empty() && version[0] == L'v')
            version.erase(0, 1);
        std::wstring url;
        for (at = 0; at != std::string::npos && url.empty();) {
            const std::wstring u = wide(json_string(json, "browser_download_url", at));
            const std::size_t slash = u.find_last_of(L'/');
            if (slash != std::wstring::npos && u.compare(slash + 1, 9, L"VanaCore-") == 0 && u.size() > 4 && u.substr(u.size() - 4) == L".zip")
                url = u;
        }
        if (stopping_)
            return;
        if (!listed || url.empty()) {
            say(kFailed, L"Couldn't reach GitHub for VanaCore's newest release. Check you're online, then try again.");
            return;
        }
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        zip = std::wstring(temp) + L"VanaCore-" + version + L".zip";
        const std::wstring part = zip + L".part";
        const HANDLE file = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            say(kFailed, L"Couldn't write to the temporary folder (" + std::wstring(temp) + L").");
            return;
        }
        const std::wstring head = L"Downloading VanaCore " + version + L"\x2026  ";
        say(kDownloading, head, 0);
        DWORD last = 0;
        const bool got = http_get(
            url, L"application/octet-stream",
            [&](const char* data, std::size_t n) {
                DWORD written = 0;
                return WriteFile(file, data, static_cast<DWORD>(n), &written, nullptr) && written == n;
            },
            [&](std::size_t done, std::size_t total) {
                const DWORD now = GetTickCount();
                if (total > 0 && now - last > 100) {
                    last = now;
                    say(kDownloading, head + mb(done) + L" of " + mb(total) + L" MB", static_cast<float>(done) / static_cast<float>(total));
                }
                return !stopping_;
            });
        CloseHandle(file);
        if (!got || stopping_ || !MoveFileExW(part.c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(part.c_str());
            if (!stopping_)
                say(kFailed, L"The download didn't finish. Check you're online, then try again.");
            return;
        }
        downloaded = true;
    }
    // unpacked by Windows' own tar (Windows 10 1803 on), the zip's VanaCore\ folder's insides into the folder; a scripts\boot.txt
    // already there (the player's own) is kept
    say(kUnpacking, L"Installing VanaCore " + version + L"\x2026");
    SHCreateDirectoryExW(nullptr, target_.c_str(), nullptr);
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring tar = std::wstring(system) + L"\\tar.exe";
    std::wstring command = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + target_ + L"\" --strip-components=1";
    if (exists(target_ + L"\\scripts\\boot.txt"))
        command += L" --exclude VanaCore/scripts/boot.txt";
    const int code = exists(tar) ? run_hidden(tar, command) : -1;
    if (downloaded)
        DeleteFileW(zip.c_str());
    if (code != 0 || !has_vanacore(target_)) {
        say(kFailed, code == -1 && !exists(tar) ? L"This Windows can't unpack the download (no tar.exe: Windows 10 1803 or later has it)."
                                                : L"VanaCore couldn't be unpacked into " + target_ + L" (tar " + std::to_wstring(code) + L").");
        return;
    }
    say(kFinishing, L"Making the shortcuts\x2026");
    if (!finish(version, why)) {
        say(kFailed, why);
        return;
    }
    say(kDone, L"VanaCore " + version + L" is installed in " + target_, 1);
    if (SUCCEEDED(com))
        CoUninitialize();
}

// VanaCoreApp beside VanaCore (this exe and DLL, unless the release had its own), the shortcuts, and Apps & features' entry
bool Installer::finish(const std::wstring& version, std::wstring& why)
{
    const std::wstring from(host_.dir);
    for (const wchar_t* name : {L"VanaCoreApp.exe", L"VanaCoreApp.dll"})
        if (!exists(target_ + L"\\" + name) && !CopyFileW((from + L"\\" + name).c_str(), (target_ + L"\\" + name).c_str(), TRUE)) {
            why = L"VanaCoreApp couldn't be copied into " + target_ + L".";
            return false;
        }
    const std::wstring app = target_ + L"\\VanaCoreApp.exe";
    if (desktop_)
        make_link(known_folder(FOLDERID_Desktop) + L"\\VanaCore.lnk", app, target_, L"Start FINAL FANTASY XI with VanaCore");
    if (start_menu_)
        make_link(known_folder(FOLDERID_Programs) + L"\\VanaCore.lnk", app, target_, L"Start FINAL FANTASY XI with VanaCore");
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        set_string(key, L"DisplayName", L"VanaCore");
        set_string(key, L"DisplayVersion", version);
        set_string(key, L"Publisher", L"VanaCore");
        set_string(key, L"InstallLocation", target_);
        set_string(key, L"DisplayIcon", app + L",0");
        set_string(key, L"UninstallString", L"\"" + app + L"\" --uninstall");
        set_string(key, L"URLInfoAbout", L"https://github.com/" VCAPP_GITHUB_REPO);
        set_dword(key, L"NoModify", 1);
        set_dword(key, L"NoRepair", 1);
        set_dword(key, L"EstimatedSize", static_cast<DWORD>(std::min<unsigned long long>(folder_kb(target_), 0xFFFFFFFF)));
        RegCloseKey(key);
    }
    return true;
}

// --- the card --------------------------------------------------------------------------------------------------------------

void Installer::render()
{
    if (!canvas_.begin())
        return;
    hits_.clear();
    Stage stage;
    {
        std::lock_guard<std::mutex> hold(lock_);
        stage = stage_;
    }
    const bool busy = stage != kReady && stage != kDone && stage != kFailed;
    canvas_.card(0);
    const D2D1_RECT_F glass = D2D1::RectF(0, kH - kBar, kW, kH);
    canvas_.backdrop(&glass);
    const float x = kPad, right = kW - kPad;
    canvas_.logo(x - 4, kPad + 25 - kLogo / 2, kLogo);
    const float tx = x - 4 + kLogo + 14;
    canvas_.text(L"Install VanaCore", ui::Font::Title, tx, kPad + 13, ui::rgb(ui::pal::white));
    canvas_.text(L"by: Vekien & Carver", ui::Font::Small, tx, kPad + 37, ui::rgb(ui::pal::text2));
    const float cx = right - 4, cy = kPad + 12;
    canvas_.close_x(cx, cy, hover_ == kClose);
    hits_.add(kClose, D2D1::RectF(cx - 13, cy - 13, cx + 13, cy + 13));
    // the panel: where it goes, the shortcuts, what's happening
    const D2D1_RECT_F p = D2D1::RectF(x - 8, kPad + kLogo + 14, right + 8, kH - kBar - 14);
    canvas_.fill(p.left, p.top, p.right - p.left, p.bottom - p.top, 8, ui::rgb(ui::pal::ink, 0.92f));
    canvas_.inset_shadow(p, 8, 14);
    canvas_.outline(p.left, p.top, p.right - p.left, p.bottom - p.top, 8, ui::rgb(ui::pal::white, 0.08f));
    const float l = p.left + 16, r = p.right - 16;
    float y = p.top + 20;
    canvas_.text(L"INSTALL TO", ui::Font::Label, l, y, ui::rgb(ui::pal::dim));
    if (stage == kReady && has_vanacore(target_))
        canvas_.text(L"VanaCore's there already: it's updated, your settings stay", ui::Font::Small, l + 82, y, ui::rgb(kVersionColour));
    y += 26;
    const float cw = canvas_.text_width(L"Change\x2026", ui::Font::Button) + 24 + 28;
    const D2D1_RECT_F field = D2D1::RectF(l, y - 15, r - cw - 8, y + 15), change = D2D1::RectF(r - cw, y - 15, r, y + 15);
    canvas_.fill(field.left, field.top, field.right - field.left, 30, 4, ui::rgb(ui::pal::well, 0.95f));
    canvas_.outline(field.left, field.top, field.right - field.left, 30, 4, ui::rgb(ui::pal::well_edge));
    canvas_.text(canvas_.fit(target_, ui::Font::Body, field.right - field.left - 20, true), ui::Font::Body, field.left + 10, y - 1,
                 ui::rgb(busy ? ui::pal::dim : ui::pal::text));
    const float t_change = hover_ != kChange ? 0.0f : pressed_ == kChange ? 1.0f : 0.6f;
    canvas_.button(change, L"Change\x2026", ui::Button::Secondary, t_change, !busy && stage != kDone, ui::icon::folder);
    if (!busy && stage != kDone)
        hits_.add(kChange, change);
    y += 36;
    if (stage == kReady) { // the shortcuts, before it starts
        for (int i = 0; i < 2; ++i, y += 28) {
            const int id = i == 0 ? kDesktop : kStartMenu;
            const bool on = i == 0 ? desktop_ : start_menu_;
            const wchar_t* label = i == 0 ? L"Desktop shortcut" : L"Start menu shortcut";
            canvas_.toggle(l, y, on, hover_ == id);
            canvas_.text(label, ui::Font::Body, l + 42, y - 1, ui::rgb(ui::pal::text));
            hits_.add(id, D2D1::RectF(l, y - 11, l + 42 + canvas_.text_width(label, ui::Font::Body), y + 11));
        }
    } else {
        draw_progress(D2D1::RectF(l, y - 6, r, p.bottom - 10));
    }
    // the footer: Install, then Open (the installed App), or Try again
    const float mid = kH - kBar / 2;
    const D2D1_RECT_F b = D2D1::RectF((kW - kButtonW) / 2, mid - kButtonH / 2, (kW + kButtonW) / 2, mid + kButtonH / 2);
    const wchar_t* label = stage == kDone ? L"OPEN" : stage == kFailed ? L"TRY AGAIN" : busy ? L"INSTALLING" : L"INSTALL";
    const float t = busy || hover_ != kInstall ? 0.0f : pressed_ == kInstall ? 1.0f : 0.6f;
    canvas_.button(b, label, ui::Button::Play, t, !busy, 0, ui::Font::Play);
    if (!busy)
        hits_.add(kInstall, b);
    canvas_.end();
}

// What's happening: a line, a bar (an endless sweep while it can't say how far), or what went wrong in rose
void Installer::draw_progress(const D2D1_RECT_F& r)
{
    Stage stage;
    std::wstring text;
    float progress;
    {
        std::lock_guard<std::mutex> hold(lock_);
        stage = stage_, text = text_, progress = progress_;
    }
    if (stage == kFailed) {
        canvas_.glyph(ui::icon::error, ui::Font::IconBig, r.left + 12, r.top + 12, ui::rgb(ui::pal::rose));
        canvas_.paragraph(text, ui::Font::Body, r.left + 34, r.top + 2, r.right - r.left - 34, ui::rgb(ui::pal::text));
        return;
    }
    if (stage == kDone) {
        canvas_.glyph(ui::icon::check, ui::Font::IconBig, r.left + 12, r.top + 12, ui::rgb(ui::pal::teal));
        canvas_.paragraph(text, ui::Font::Body, r.left + 34, r.top + 2, r.right - r.left - 34, ui::rgb(ui::pal::text));
        hits_.add(kOpenFolder, D2D1::RectF(r.left + 34, r.top, r.right, r.top + 40));
        return;
    }
    canvas_.text(canvas_.fit(text, ui::Font::Body, r.right - r.left, false), ui::Font::Body, r.left, r.top + 10, ui::rgb(ui::pal::text));
    if (progress >= 0) {
        canvas_.bar(r.left, r.top + 32, r.right - r.left, progress);
    } else { // a sweep, a third of the bar wide, crossing in a second and a half
        const float w = r.right - r.left, span = w / 3, at = static_cast<float>(GetTickCount() % 1500) / 1500.0f * (w + span) - span;
        canvas_.fill(r.left, r.top + 32, w, 4, 2, ui::rgb(ui::pal::white, 0.1f));
        canvas_.clip(D2D1::RectF(r.left, r.top + 30, r.right, r.top + 38));
        canvas_.fill(r.left + at, r.top + 32, span, 4, 2, ui::rgb(ui::pal::accent));
        canvas_.unclip();
    }
}

void Installer::press(int hit)
{
    Stage stage;
    {
        std::lock_guard<std::mutex> hold(lock_);
        stage = stage_;
    }
    if (hit == kClose) {
        DestroyWindow(window_);
    } else if (hit == kChange) {
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            FILEOPENDIALOGOPTIONS options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            dialog->SetTitle(L"Where should VanaCore go? (a VanaCore folder is made in it)");
            IShellItem* item = nullptr;
            wchar_t* path = nullptr;
            if (SUCCEEDED(dialog->Show(window_)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring picked = trimmed(path);
                // a folder named VanaCore, or one with VanaCore in it already, is the place itself; else a VanaCore folder in it
                const std::wstring last = picked.substr(picked.find_last_of(L'\\') + 1);
                target_ = has_vanacore(picked) || _wcsicmp(last.c_str(), L"VanaCore") == 0 ? picked : picked + L"\\VanaCore";
                CoTaskMemFree(path);
            }
            if (item != nullptr)
                item->Release();
            dialog->Release();
        }
        render();
    } else if (hit == kDesktop || hit == kStartMenu) {
        (hit == kDesktop ? desktop_ : start_menu_) = !(hit == kDesktop ? desktop_ : start_menu_);
        render();
    } else if (hit == kInstall) {
        if (stage == kDone) { // the installed App, and this one goes
            ShellExecuteW(nullptr, nullptr, (target_ + L"\\VanaCoreApp.exe").c_str(), nullptr, target_.c_str(), SW_SHOWNORMAL);
            DestroyWindow(window_);
            return;
        }
        start();
        render();
    } else if (hit == kOpenFolder) {
        ShellExecuteW(window_, nullptr, target_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

LRESULT CALLBACK Installer::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    auto* self = reinterpret_cast<Installer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr || self->window_ != window)
        return DefWindowProcW(window, message, wparam, lparam);
    return self->handle(message, wparam, lparam);
}

void Installer::mouse(UINT message, LPARAM lparam)
{
    const int at = hits_.at(canvas_.to_card(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), kNone);
    if (message == WM_MOUSEMOVE) {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window_, 0};
            tracking_ = TrackMouseEvent(&t) != FALSE;
        }
        if (at == hover_)
            return;
        hover_ = at;
    } else if (message == WM_LBUTTONDOWN) {
        pressed_ = at;
        SetCapture(window_);
    } else {
        ReleaseCapture();
        const int was = pressed_;
        pressed_ = kNone;
        if (at != kNone && at == was) {
            press(at);
            return;
        }
    }
    render();
}

LRESULT Installer::handle(UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window_, &p);
        return hits_.at(canvas_.to_card(p.x, p.y), kNone) != kNone ? HTCLIENT : HTCAPTION;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        mouse(message, lparam);
        return 0;
    case WM_MOUSELEAVE:
    case WM_NCMOUSEMOVE:
        tracking_ = message == WM_MOUSELEAVE ? false : tracking_;
        if (hover_ != kNone)
            hover_ = kNone, render();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && hover_ != kNone) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_RETURN)
            press(kInstall);
        else if (wparam == VK_ESCAPE)
            press(kClose);
        return 0;
    case WM_TIMER: // the sweep moving while it works
        render();
        return 0;
    case ui::kRedraw:
        render();
        return 0;
    case WM_DPICHANGED:
        canvas_.dpi_changed(HIWORD(wparam));
        render();
        return 0;
    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        KillTimer(window_, 1);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

int Installer::run()
{
    if (!canvas_.start(self())) {
        host_.splash_close();
        MessageBoxW(nullptr, L"VanaCore isn't in this app's folder, and this Windows can't show the installer.", L"VanaCore", MB_OK | MB_ICONERROR);
        return 1;
    }
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &Installer::proc;
    wc.hInstance = self();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = host_.icon_big;
    wc.hIconSm = host_.icon_small;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    POINT cursor{};
    GetCursorPos(&cursor);
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClass, L"Install VanaCore", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                                  cursor.x, cursor.y, 1, 1, nullptr, nullptr, self(), this);
    if (window == nullptr) {
        host_.splash_close();
        return 1;
    }
    window_ = window;
    canvas_.attach(window, kW, kH);
    render();
    ShowWindow(window, SW_SHOW);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(window);
    host_.splash_close();
    SetTimer(window, 1, 33, nullptr); // (the sweep)
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    window_ = nullptr;
    return 0;
}

// --- uninstalling ------------------------------------------------------------------------------------------------------------

int uninstall(const VanaCoreAppHost& host)
{
    host.splash_close();
    const std::wstring dir(host.dir);
    if (!has_vanacore(dir))
        return 1;
    const std::wstring ask = L"Uninstall VanaCore from " + dir +
                             L"?\n\nEverything in that folder is deleted, your settings, profiles and logs too, and the shortcuts go.";
    if (MessageBoxW(nullptr, ask.c_str(), L"Uninstall VanaCore", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDOK)
        return 0;
    // the shortcuts that lead into the folder, and the entry (only when it's this folder's)
    for (const std::wstring& link : {known_folder(FOLDERID_Desktop) + L"\\VanaCore.lnk", known_folder(FOLDERID_Programs) + L"\\VanaCore.lnk"}) {
        const std::wstring to = link_target(link);
        if (!to.empty() && _wcsnicmp(to.c_str(), (dir + L"\\").c_str(), dir.size() + 1) == 0)
            DeleteFileW(link.c_str());
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        wchar_t at[MAX_PATH] = {};
        DWORD size = sizeof at - sizeof(wchar_t), type = 0;
        const bool ours = RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<BYTE*>(at), &size) == ERROR_SUCCESS &&
                          _wcsicmp(trimmed(at).c_str(), dir.c_str()) == 0;
        RegCloseKey(key);
        if (ours)
            RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    }
    // the folder, by a hidden cmd once this process (whose exe and DLL are in it) has gone
    wchar_t system[MAX_PATH] = {}, temp[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    GetTempPathW(MAX_PATH, temp);
    const std::wstring cmd = std::wstring(system) + L"\\cmd.exe";
    std::wstring line = L"\"" + cmd + L"\" /c ping 127.0.0.1 -n 3 >nul & rd /s /q \"" + dir + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(cmd.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, temp, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0; // (no "done" box: the folder goes only once this process has)
}

} // namespace vcapp
