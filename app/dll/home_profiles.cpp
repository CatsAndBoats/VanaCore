// The home's profiles (a named settings file each, VanaCore.exe --ini; the first is vanacore.ini itself, listed in
// vanacore.ini's [app.profiles] as <n>=<name>|<file>, the one in use [app] profile), the Profiles tab, and the Errors tab.
#include "home.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace vcapp {

namespace {

constexpr std::size_t kNameMost = 24; // a profile's name, at most

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool same(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring trimmed(const std::wstring& s)
{
    const std::size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

// The keys of a section of `ini`
std::vector<std::wstring> keys_of(const std::wstring& ini, const wchar_t* section)
{
    std::vector<wchar_t> keys(16384);
    const DWORD n = GetPrivateProfileStringW(section, nullptr, L"", keys.data(), static_cast<DWORD>(keys.size()), ini.c_str());
    std::vector<std::wstring> out;
    for (const wchar_t* key = keys.data(); key < keys.data() + n && *key != 0; key += wcslen(key) + 1)
        out.push_back(key);
    return out;
}

} // namespace

// --- the profiles ------------------------------------------------------------------------------------------------------

void Home::load_profiles()
{
    profiles_.clear();
    std::vector<std::pair<int, Profile>> listed;
    for (const std::wstring& key : keys_of(app_ini_, L"app.profiles")) {
        const std::wstring value = ini_get(L"app.profiles", key.c_str());
        const std::size_t bar = value.rfind(L'|');
        if (bar == std::wstring::npos || !std::iswdigit(key[0]))
            continue;
        const Profile p{trimmed(value.substr(0, bar)), trimmed(value.substr(bar + 1))};
        if (!p.name.empty() && !p.file.empty() && p.file.find_first_of(L"\\/:") == std::wstring::npos)
            listed.push_back({_wtoi(key.c_str()), p});
    }
    std::stable_sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [index, p] : listed)
        profiles_.push_back(p);
    if (std::none_of(profiles_.begin(), profiles_.end(), [](const Profile& p) { return same(p.file, L"vanacore.ini"); }))
        profiles_.insert(profiles_.begin(), Profile{L"Default", L"vanacore.ini"}); // (vanacore.ini is always one)
    profile_ = 0;
    const std::wstring current = ini_get(L"app", L"profile");
    for (std::size_t i = 0; i < profiles_.size(); ++i)
        if (same(profiles_[i].name, current))
            profile_ = static_cast<int>(i);
    ini_ = dir_ + L"\\" + profiles_[profile_].file;
}

// [app.profiles] as the list has it (its keys taken out one by one: the section's notes stay), [app] profile the one in use
void Home::save_profiles()
{
    for (const std::wstring& key : keys_of(app_ini_, L"app.profiles"))
        ini_set(L"app.profiles", key.c_str(), nullptr);
    for (std::size_t i = 0; i < profiles_.size(); ++i) {
        const std::wstring value = profiles_[i].name + L"|" + profiles_[i].file;
        ini_set(L"app.profiles", std::to_wstring(i).c_str(), &value);
    }
    ini_set(L"app", L"profile", &profiles_[profile_].name);
}

// The profile in use: the tabs show its settings, START starts the game with them
void Home::use_profile(int index)
{
    if (index < 0 || index >= static_cast<int>(profiles_.size()))
        return;
    profile_ = index;
    ini_ = dir_ + L"\\" + profiles_[profile_].file;
    ini_set(L"app", L"profile", &profiles_[profile_].name);
    pivot_scroll_ = 0;
    detect();
    load_pivot();
}

std::wstring Home::unique_name(const std::wstring& name, int except) const
{
    std::wstring candidate = name;
    for (int n = 2;; ++n) {
        bool taken = false;
        for (int i = 0; i < static_cast<int>(profiles_.size()); ++i)
            taken = taken || (i != except && same(profiles_[i].name, candidate));
        if (!taken)
            return candidate;
        candidate = name + L" " + std::to_wstring(n);
    }
}

// A new profile: a copy of the one in use (vanacore.profile-<n>.ini), named "Profile <n>" and its name ready to type over
void Home::new_profile()
{
    int n = 1;
    auto file_of = [](int k) { return L"vanacore.profile-" + std::to_wstring(k) + L".ini"; };
    while (exists(dir_ + L"\\" + file_of(n)) ||
           std::any_of(profiles_.begin(), profiles_.end(), [&](const Profile& p) { return same(p.file, file_of(n)); }))
        ++n;
    const std::wstring file = file_of(n), path = dir_ + L"\\" + file;
    if (!CopyFileW(ini_.c_str(), path.c_str(), TRUE)) { // (no file to copy: an empty one, UTF-16 as VanaCore's)
        const HANDLE made = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (made == INVALID_HANDLE_VALUE)
            return;
        const BYTE bom[] = {0xFF, 0xFE};
        DWORD written = 0;
        WriteFile(made, bom, sizeof bom, &written, nullptr);
        CloseHandle(made);
    }
    profiles_.push_back({unique_name(L"Profile " + std::to_wstring(profiles_.size() + 1), -1), file});
    use_profile(static_cast<int>(profiles_.size()) - 1);
    save_profiles();
    start_rename(profile_);
}

// Asked first; its settings file goes to the Recycle Bin. vanacore.ini's profile isn't deleted.
void Home::delete_profile(int index)
{
    if (index < 0 || index >= static_cast<int>(profiles_.size()) || same(profiles_[index].file, L"vanacore.ini"))
        return;
    const Profile p = profiles_[index];
    const std::wstring ask = L"Delete the profile \x201C" + p.name + L"\x201D?\n\nIts settings (" + p.file + L") go to the Recycle Bin.";
    if (MessageBoxW(window_, ask.c_str(), L"VanaCore", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
        return;
    std::wstring from = dir_ + L"\\" + p.file;
    from.push_back(L'\0'); // (SHFileOperation's list: double-null ended)
    SHFILEOPSTRUCTW op{};
    op.hwnd = window_;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (exists(dir_ + L"\\" + p.file) && SHFileOperationW(&op) != 0)
        return;
    profiles_.erase(profiles_.begin() + index);
    const int now = profile_ == index ? 0 : profile_ > index ? profile_ - 1 : profile_;
    use_profile(now);
    save_profiles();
}

// A shortcut on the desktop, "FFXI <name>", that starts the game with the profile (VanaCore.exe --retail --ini <its file>;
// vanacore.ini's: --retail alone), in VanaCore's folder, with VanaCore.exe's icon. One already there is made again.
void Home::make_shortcut(int index)
{
    if (game_.empty() || index < 0 || index >= static_cast<int>(profiles_.size()))
        return;
    const Profile& p = profiles_[index];
    std::wstring name = L"FFXI " + p.name;
    for (wchar_t& c : name)
        if (wcschr(L"\\/:*?\"<>|", c) != nullptr)
            c = L'_';
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        return;
    const std::wstring path = std::wstring(desktop) + L"\\" + name + L".lnk";
    CoTaskMemFree(desktop);
    const std::wstring args = same(p.file, L"vanacore.ini") ? L"--retail" : L"--retail --ini \"" + p.file + L"\"";
    IShellLinkW* link = nullptr;
    IPersistFile* file = nullptr;
    bool made = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(game_.c_str());
        link->SetArguments(args.c_str());
        link->SetWorkingDirectory(dir_.c_str());
        link->SetIconLocation(game_.c_str(), 0);
        link->SetDescription((L"FINAL FANTASY XI with VanaCore: the " + p.name + L" profile").c_str());
        made = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(path.c_str(), TRUE));
    }
    if (file != nullptr)
        file->Release();
    if (link != nullptr)
        link->Release();
    note_ = made ? L"\x201C" + name + L"\x201D is on your desktop" : L"The shortcut couldn't be made";
}

void Home::start_rename(int index)
{
    renaming_ = index;
    typed_ = profiles_[index].name;
    tab_ = kProfiles;
}

void Home::end_rename(bool keep)
{
    const int i = renaming_;
    renaming_ = -1;
    if (!keep || i < 0 || i >= static_cast<int>(profiles_.size()))
        return;
    const std::wstring name = trimmed(typed_);
    if (name.empty() || name == profiles_[i].name)
        return;
    profiles_[i].name = unique_name(name, i);
    save_profiles();
}

// A key while a name's being typed (WM_CHAR: the letters, Backspace; WM_KEYDOWN: Enter keeps it, Esc puts it back): true when
// it was the name's
bool Home::rename_key(UINT message, WPARAM wparam)
{
    if (renaming_ < 0)
        return false;
    if (message == WM_KEYDOWN) {
        if (wparam == VK_RETURN || wparam == VK_ESCAPE)
            end_rename(wparam == VK_RETURN);
        return true;
    }
    const wchar_t c = static_cast<wchar_t>(wparam);
    if (c == L'\b') {
        if (!typed_.empty())
            typed_.pop_back();
    } else if (c >= 32 && c != L'|' && c != 127 && typed_.size() < kNameMost) {
        typed_.push_back(c);
    }
    return true;
}

// --- what's wrong ------------------------------------------------------------------------------------------------------

std::vector<Home::Error> Home::errors() const
{
    std::vector<Error> out;
    if (game_.empty() && !updating()) // (while it's being installed here: the glass bar says so)
        out.push_back({L"VanaCore isn't beside the App",
                       L"The App starts VanaCore from its own folder, " + dir_ +
                           L", and VanaCore.exe isn't there. The App installs the latest VanaCore there each time it opens "
                           L"until it can (it needs GitHub); or keep VanaCoreApp.exe in VanaCore's folder (the one with "
                           L"VanaCore.exe, VanaCore.Core.dll and addons)."});
    if (install_.ffxi.empty())
        out.push_back({L"FINAL FANTASY XI wasn't found",
                       L"It isn't in the registry (PlayOnline's InstallFolder, US, EU or JP) and no folder's been picked. If it's "
                       L"installed, show VanaCore where: its FINAL FANTASY XI folder, PlayOnlineViewer, or the one they're in.",
                       kLocate, L"Locate\x2026"});
    else if (install_.pol.empty())
        out.push_back({L"PlayOnline wasn't found",
                       L"FINAL FANTASY XI is in " + install_.ffxi +
                           L", but there's no PlayOnline (pol.exe) for it. Show VanaCore its PlayOnlineViewer folder.",
                       kLocate, L"Locate\x2026"});
    if (!exists(ini_) && !same(profiles_[profile_].file, L"vanacore.ini")) // (no vanacore.ini: VanaCore's defaults, fine)
        out.push_back({L"\x201C" + profiles_[profile_].name + L"\x201D's settings are missing",
                       profiles_[profile_].file + L" isn't in VanaCore's folder any more, and VanaCore won't start without it. Pick "
                                                 L"another profile, or delete this one and make a new one.",
                       kTab + kProfiles, L"Profiles"});
    if (!error_.empty())
        out.push_back({L"VanaCore didn't start", error_, kPlay, L"Try again"});
    return out;
}

// --- the pages ---------------------------------------------------------------------------------------------------------

void Home::page_profiles(const D2D1_RECT_F& r)
{
    float y = r.top + 8;
    canvas_.text(L"PROFILES", ui::Font::Label, r.left, y, ui::rgb(ui::pal::dim));
    canvas_.text(note_.empty() ? L"Each has its own settings: pick one beside START" : note_, ui::Font::Small, r.left + 80, y,
                 ui::rgb(note_.empty() ? ui::pal::faint : ui::pal::teal));
    const std::wstring add = L"New Profile";
    const float aw = canvas_.text_width(add, ui::Font::Button) + 24 + 28;
    const D2D1_RECT_F a = D2D1::RectF(r.right - aw, y - 14, r.right, y + 14);
    canvas_.button(a, add, ui::Button::Accent, t_of(kNewProfile), true, ui::icon::add);
    hits_.add(kNewProfile, a);
    y += 24;
    canvas_.line(r.left, y, r.right, y, ui::rgb(ui::pal::white, 0.08f));
    const float row = 30, top = y + 4;
    const int rows = static_cast<int>((r.bottom - top) / row), count = static_cast<int>(profiles_.size());
    profile_scroll_ = std::max(0, std::min(profile_scroll_, count - rows));
    if (renaming_ >= 0 && (renaming_ < profile_scroll_ || renaming_ >= profile_scroll_ + rows))
        profile_scroll_ = std::max(0, std::min(renaming_, count - rows)); // (the one being named in view)
    for (int k = 0; k < rows && profile_scroll_ + k < count; ++k) {
        const int i = profile_scroll_ + k, id = kProfileRow + i * 8;
        const Profile& p = profiles_[i];
        const float cy = top + k * row + row / 2;
        const bool picked = i == profile_, over = hover_ >= id && hover_ < id + 8;
        if (over || picked)
            canvas_.fill(r.left - 6, cy - row / 2 + 1, r.right - r.left + 12, row - 2, 4, ui::rgb(picked ? ui::pal::accent : ui::pal::white, picked ? 0.1f : 0.04f));
        canvas_.glyph(picked ? ui::icon::check : ui::icon::circle, ui::Font::IconBig, r.left + 10, cy, ui::rgb(picked ? ui::pal::teal : ui::pal::faint));
        const float nx = r.left + 32, buttons = r.right - 90;
        if (renaming_ == i) { // the name's box: what's typed, a caret after it
            const D2D1_RECT_F box = D2D1::RectF(nx - 6, cy - 12, nx + 220, cy + 12);
            canvas_.fill(box.left, box.top, box.right - box.left, box.bottom - box.top, 4, ui::rgb(ui::pal::well, 0.95f));
            canvas_.outline(box.left, box.top, box.right - box.left, box.bottom - box.top, 4, ui::rgb(ui::pal::accent, 0.8f));
            canvas_.text(typed_, ui::Font::Body, nx, cy - 1, ui::rgb(ui::pal::white));
            const float cx = nx + canvas_.text_width(typed_, ui::Font::Body) + 1;
            canvas_.line(cx, cy - 8, cx, cy + 8, ui::rgb(ui::pal::accent2), 1.5f);
            hits_.add(id + 3, box);
        } else {
            canvas_.text(canvas_.fit(p.name, ui::Font::Body, 220, false), ui::Font::Body, nx, cy - 1, ui::rgb(picked || over ? ui::pal::white : ui::pal::text2));
        }
        canvas_.text(canvas_.fit(p.file, ui::Font::Small, buttons - 12 - (nx + 232), true), ui::Font::Small, nx + 232, cy, ui::rgb(ui::pal::faint));
        hits_.add(id, D2D1::RectF(r.left - 6, cy - row / 2, buttons - 4, cy + row / 2));
        // rename, and delete (not vanacore.ini's)
        const bool can_delete = !same(p.file, L"vanacore.ini");
        // a desktop shortcut that starts the game with it, rename, and delete (not vanacore.ini's)
        static const int kParts[] = {4, 1, 2};
        for (int n = 0; n < 3; ++n) {
            const int part = kParts[n];
            if (part == 2 && !can_delete)
                continue;
            const D2D1_RECT_F b = D2D1::RectF(buttons + n * 30, cy - 12, buttons + n * 30 + 26, cy + 12);
            const bool on = hover_ == id + part;
            if (on)
                canvas_.fill(b.left, b.top, 26, 24, 4, ui::rgb(part == 2 ? ui::pal::rose : ui::pal::white, pressed_ == id + part ? 0.22f : 0.12f));
            canvas_.glyph(part == 4 ? ui::icon::shortcut : part == 1 ? ui::icon::edit : ui::icon::trash, ui::Font::Icon, b.left + 13, cy,
                          ui::rgb(on ? (part == 2 ? ui::pal::rose : ui::pal::white) : ui::pal::dim));
            hits_.add(id + part, b);
            tip(id + part, part == 4 ? L"Desktop shortcut: starts FFXI with " + p.name : part == 1 ? L"Rename" : L"Delete " + p.name);
        }
    }
    // (the buttons before the row under them: Hits::at takes the first match)
    std::stable_partition(hits_.list.begin(), hits_.list.end(), [](const auto& h) { return h.first < kProfileRow || h.first >= kDrop || (h.first - kProfileRow) % 8 != 0; });
}

void Home::page_errors(const D2D1_RECT_F& r)
{
    float y = r.top + 2;
    for (const Error& e : errors()) {
        const float bw = e.fix == kNone ? 0 : canvas_.text_width(e.fix_label, ui::Font::Button) + 32;
        const float room = r.right - r.left - 36 - (bw > 0 ? bw + 16 : 0);
        canvas_.glyph(ui::icon::error, ui::Font::IconBig, r.left + 12, y + 11, ui::rgb(ui::pal::rose));
        canvas_.text(e.title, ui::Font::Body, r.left + 36, y + 10, ui::rgb(ui::pal::white));
        const float h = canvas_.paragraph(e.detail, ui::Font::Small, r.left + 36, y + 22, room, ui::rgb(ui::pal::text2));
        if (e.fix != kNone) {
            const D2D1_RECT_F b = D2D1::RectF(r.right - bw, y + 2, r.right, y + 32);
            canvas_.button(b, e.fix_label, ui::Button::Accent, t_of(e.fix));
            hits_.add(e.fix, b);
        }
        y += 22 + h + 14;
        if (y > r.bottom - 20)
            break;
    }
}

// --- logs ----------------------------------------------------------------------------------------------------------------

// Every .txt and .csv under logs\ and chatlogs\ (their folders too), newest first
void Home::load_logs()
{
    logs_.clear();
    std::vector<std::wstring> folders = {L"logs", L"chatlogs"};
    for (std::size_t f = 0; f < folders.size(); ++f) {
        WIN32_FIND_DATAW found{};
        const HANDLE find = FindFirstFileW((dir_ + L"\\" + folders[f] + L"\\*").c_str(), &found);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do {
            const std::wstring name = found.cFileName, path = folders[f] + L"\\" + name;
            if (name == L"." || name == L"..")
                continue;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                folders.push_back(path);
                continue;
            }
            const std::size_t dot = name.rfind(L'.');
            const std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
            if (same(ext, L".txt") || same(ext, L".csv"))
                logs_.push_back({path, found.ftLastWriteTime, (static_cast<unsigned long long>(found.nFileSizeHigh) << 32) | found.nFileSizeLow});
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    std::sort(logs_.begin(), logs_.end(), [](const Log& a, const Log& b) { return CompareFileTime(&a.written, &b.written) > 0; });
}

void Home::page_logs(const D2D1_RECT_F& r)
{
    float y = r.top + 8;
    canvas_.text(L"LOGS", ui::Font::Label, r.left, y, ui::rgb(ui::pal::dim));
    canvas_.text(std::to_wstring(logs_.size()) + L" files, newest first: click one to open it", ui::Font::Small, r.left + 50, y, ui::rgb(ui::pal::faint));
    const std::wstring folder = L"Open Folder";
    const float fw = canvas_.text_width(folder, ui::Font::Button) + 24 + 28;
    const D2D1_RECT_F b = D2D1::RectF(r.right - fw, y - 14, r.right, y + 14);
    canvas_.button(b, folder, ui::Button::Secondary, t_of(kLogsFolder), true, ui::icon::folder);
    hits_.add(kLogsFolder, b);
    y += 24;
    canvas_.line(r.left, y, r.right, y, ui::rgb(ui::pal::white, 0.08f));
    const float row = 24, top = y + 3;
    const int rows = static_cast<int>((r.bottom - top) / row), count = static_cast<int>(logs_.size());
    if (count == 0) {
        canvas_.text(L"No logs yet: VanaCore writes them in logs\\ as the game runs", ui::Font::Small, r.left, top + row / 2, ui::rgb(ui::pal::dim));
        return;
    }
    logs_scroll_ = std::max(0, std::min(logs_scroll_, count - rows));
    static const wchar_t* const kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    for (int k = 0; k < rows && logs_scroll_ + k < count; ++k) {
        const int i = logs_scroll_ + k;
        const Log& log = logs_[i];
        const float cy = top + k * row + row / 2;
        const bool over = hover_ == kLogRow + i;
        if (over)
            canvas_.fill(r.left - 6, cy - row / 2 + 1, r.right - r.left + 12, row - 2, 4, ui::rgb(ui::pal::white, 0.06f));
        // when (local time), the size, then the file
        SYSTEMTIME utc{}, t{};
        FileTimeToSystemTime(&log.written, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &t);
        wchar_t when[32];
        swprintf_s(when, L"%d %s %02d:%02d", t.wDay, kMonths[(t.wMonth + 11) % 12], t.wHour, t.wMinute);
        const unsigned long long kb = (log.bytes + 1023) / 1024;
        const std::wstring size = kb >= 1024 ? std::to_wstring(kb / 1024) + L"." + std::to_wstring(kb % 1024 * 10 / 1024) + L" MB" : std::to_wstring(kb) + L" KB";
        canvas_.text(when, ui::Font::Small, r.left, cy, ui::rgb(ui::pal::dim));
        canvas_.text(size, ui::Font::Small, r.right - canvas_.text_width(size, ui::Font::Small), cy, ui::rgb(ui::pal::faint));
        canvas_.text(canvas_.fit(log.path, ui::Font::Small, r.right - r.left - 104 - 70, true), ui::Font::Small, r.left + 104, cy,
                     ui::rgb(over ? ui::pal::white : ui::pal::text2));
        hits_.add(kLogRow + i, D2D1::RectF(r.left - 6, cy - row / 2, r.right + 6, cy + row / 2));
        tip(kLogRow + i, L"Open in Notepad");
    }
    if (count > rows) { // where in the list
        const float h = r.bottom - top;
        canvas_.fill(r.right + 9, top + h * logs_scroll_ / count, 2, std::max(12.0f, h * rows / count), 1, ui::rgb(ui::pal::white, 0.25f));
    }
}

bool Home::press_profiles(int hit)
{
    if (hit == kLogsFolder) {
        ShellExecuteW(window_, nullptr, (dir_ + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return false;
    }
    if (hit >= kLogRow) { // in Notepad
        const int i = hit - kLogRow;
        if (i < static_cast<int>(logs_.size()))
            ShellExecuteW(window_, nullptr, L"notepad.exe", (L"\"" + dir_ + L"\\" + logs_[i].path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return false;
    }
    if (hit >= kDrop)
        return false;
    if (hit == kNewProfile) {
        new_profile();
        return true;
    }
    if (hit < kProfileRow)
        return false;
    const int i = (hit - kProfileRow) / 8, part = (hit - kProfileRow) % 8;
    if (part == 0)
        use_profile(i);
    else if (part == 1)
        start_rename(i);
    else if (part == 2)
        delete_profile(i);
    else if (part == 4)
        make_shortcut(i);
    return true;
}

} // namespace vcapp
