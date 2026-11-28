// The home's tabs: what's installed (FFXI from the registry, else where Locate found it; VanaCore beside the app), the settings
// to make before the game starts (vanacore.ini beside the app: [graphics], [pivot], [pivot.order]), and about.
#include "home.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <cwchar>

#include "version.h"

namespace vcapp {

namespace {

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_folder(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring trimmed(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

std::wstring parent(const std::wstring& path)
{
    const std::size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? std::wstring() : path.substr(0, at);
}

bool same(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// HKLM\SOFTWARE\PlayOnline<region>\InstallFolder's `value` (32-bit view), "" when it isn't there
std::wstring install_value(const wchar_t* key_name, const wchar_t* value)
{
    std::wstring path = std::wstring(L"SOFTWARE\\") + key_name + L"\\InstallFolder";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS)
        return {};
    wchar_t text[MAX_PATH] = {};
    DWORD type = 0, size = sizeof(text) - sizeof(wchar_t);
    const bool ok = RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(text), &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    return ok ? trimmed(text) : std::wstring();
}

// The renderers the Graphics tab offers: [graphics] renderer's value, its name
struct Renderer {
    const wchar_t *value, *name;
};
constexpr Renderer kRenderers[] = {{L"d3d12", L"DirectX 12"}, {L"d3d11", L"DirectX 11"}, {L"d3d9", L"DirectX 9"}, {L"d3d8", L"Original"}};

} // namespace

// --- vanacore.ini ------------------------------------------------------------------------------------------------------

// The profile's settings file, or vanacore.ini for the app's own sections ([app], [app.profiles])
std::wstring Home::ini_get(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const
{
    wchar_t text[2048] = {};
    GetPrivateProfileStringW(section, key, fallback, text, 2048, (_wcsnicmp(section, L"app", 3) == 0 ? app_ini_ : ini_).c_str());
    return text;
}

void Home::ini_set(const wchar_t* section, const wchar_t* key, const std::wstring* value)
{
    set_in(_wcsnicmp(section, L"app", 3) == 0 ? app_ini_ : ini_, section, key, value);
}

// `value` null: the key taken out. A new file is made UTF-16 (as VanaCore writes vanacore.ini), so Windows keeps it so.
void Home::set_in(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const std::wstring* value)
{
    if (value != nullptr && !exists(ini)) {
        const HANDLE file = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const BYTE bom[] = {0xFF, 0xFE};
            DWORD written = 0;
            WriteFile(file, bom, sizeof bom, &written, nullptr);
            CloseHandle(file);
        }
    }
    WritePrivateProfileStringW(section, key, value != nullptr ? value->c_str() : nullptr, ini.c_str());
}

// --- what's installed --------------------------------------------------------------------------------------------------

// FFXI: the registry's install ([client] retail.region's, else US, EU, JP as VanaCore looks), else where Locate found it
// ([client] retail.pol, [app] ffxi); its tools' gamepad config for its region; FFXiMain.dll's date
void Home::detect()
{
    install_ = {};
    struct Region {
        const wchar_t *name, *key, *tools;
    };
    static const Region kRegions[] = {{L"us", L"PlayOnlineUS", L"ToolsUS"}, {L"eu", L"PlayOnlineEU", L"ToolsEU"}, {L"jp", L"PlayOnline", L"Tools"}};
    const std::wstring wanted = ini_get(L"client", L"retail.region");
    for (int pass = 0; pass < 2 && install_.ffxi.empty(); ++pass)
        for (const Region& region : kRegions) {
            if (pass == 0 && !same(wanted, region.name))
                continue;
            const std::wstring ffxi = install_value(region.key, L"0001");
            if (ffxi.empty() || !exists(ffxi + L"\\FFXiMain.dll"))
                continue;
            install_.ffxi = ffxi;
            install_.pol = install_value(region.key, L"1000");
            install_.region = region.name;
            install_.registry = true;
            break;
        }
    const std::wstring pol = trimmed(ini_get(L"client", L"retail.pol")), ffxi = trimmed(ini_get(L"app", L"ffxi"));
    if (!pol.empty() && exists(pol + L"\\pol.exe"))
        install_.pol = pol; // (VanaCore.exe --retail starts this one: as retail.pol says)
    if (install_.ffxi.empty() && !ffxi.empty() && exists(ffxi + L"\\FFXiMain.dll"))
        install_.ffxi = ffxi;
    if (install_.ffxi.empty())
        install_.pol.clear(); // (no game to play)
    if (install_.region.empty())
        install_.region = wanted.empty() ? L"us" : wanted;
    const wchar_t* tools = same(install_.region, L"eu") ? L"ToolsEU" : same(install_.region, L"jp") ? L"Tools" : L"ToolsUS";
    for (const wchar_t* folder : {tools, L"ToolsUS", L"ToolsEU", L"Tools"})
        if (!install_.ffxi.empty() && exists(install_.ffxi + L"\\" + folder + L"\\FFXiPadConfig.exe")) {
            install_.pad = install_.ffxi + L"\\" + folder + L"\\FFXiPadConfig.exe";
            break;
        }
    ffxi_date_.clear();
    WIN32_FILE_ATTRIBUTE_DATA data{};
    SYSTEMTIME utc{}, local{};
    if (!install_.ffxi.empty() && GetFileAttributesExW((install_.ffxi + L"\\FFXiMain.dll").c_str(), GetFileExInfoStandard, &data) &&
        FileTimeToSystemTime(&data.ftLastWriteTime, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        static const wchar_t* const kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        ffxi_date_ = std::to_wstring(local.wDay) + L" " + kMonths[(local.wMonth + 11) % 12] + L" " + std::to_wstring(local.wYear);
    }
}

std::wstring Home::pick_folder(const wchar_t* title) const
{
    IFileOpenDialog* dialog = nullptr;
    std::wstring picked;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return picked;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    IShellItem* item = nullptr;
    wchar_t* path = nullptr;
    if (SUCCEEDED(dialog->Show(window_)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        picked = trimmed(path);
        CoTaskMemFree(path);
    }
    if (item != nullptr)
        item->Release();
    dialog->Release();
    return picked;
}

// Locate: a folder of the install (FINAL FANTASY XI's, PlayOnlineViewer's, SquareEnix or the one above it); the two found
// from it go to [app] ffxi and [client] retail.pol (VanaCore.exe --retail starts that PlayOnline)
void Home::locate()
{
    const std::wstring at = pick_folder(L"Where's FINAL FANTASY XI? (its folder, PlayOnline's, or the one they're in)");
    if (at.empty())
        return;
    std::wstring ffxi, pol;
    for (const std::wstring& p : {at, at + L"\\FINAL FANTASY XI", at + L"\\SquareEnix\\FINAL FANTASY XI", parent(at) + L"\\FINAL FANTASY XI"})
        if (ffxi.empty() && exists(p + L"\\FFXiMain.dll"))
            ffxi = p;
    for (const std::wstring& p : {at, at + L"\\PlayOnlineViewer", at + L"\\SquareEnix\\PlayOnlineViewer", parent(at) + L"\\PlayOnlineViewer",
                                  parent(ffxi) + L"\\PlayOnlineViewer"})
        if (pol.empty() && exists(p + L"\\pol.exe"))
            pol = p;
    if (ffxi.empty() || pol.empty()) {
        MessageBoxW(window_, ffxi.empty() ? L"There's no FINAL FANTASY XI (FFXiMain.dll) there." : L"There's no PlayOnline (pol.exe) beside FINAL FANTASY XI there.",
                    L"VanaCore", MB_OK | MB_ICONWARNING);
        return;
    }
    ini_set(L"app", L"ffxi", &ffxi);
    for (const Profile& profile : profiles_) // (every profile starts the game from there)
        set_in(dir_ + L"\\" + profile.file, L"client", L"retail.pol", &pol);
    detect();
}

void Home::open(const std::wstring& what, const wchar_t* args)
{
    ShellExecuteW(window_, nullptr, what.c_str(), args, parent(what).c_str(), SW_SHOWNORMAL);
}

// --- Pivot -------------------------------------------------------------------------------------------------------------

// [pivot] enabled and rootfolder (relative: from the app's folder), [pivot.order]'s overlays in their order (on), then the
// root's other folders (off)
void Home::load_pivot()
{
    pivot_on_ = ini_get(L"pivot", L"enabled", L"0") == L"1";
    std::wstring root = trimmed(ini_get(L"pivot", L"rootfolder", L"pivot"));
    pivot_root_ = root.size() > 1 && (root[1] == L':' || root[0] == L'\\') ? root : dir_ + L"\\" + root;
    overlays_.clear();
    std::vector<std::pair<int, std::wstring>> order;
    wchar_t keys[8192] = {};
    const DWORD n = GetPrivateProfileStringW(L"pivot.order", nullptr, L"", keys, 8192, ini_.c_str());
    for (const wchar_t* key = keys; key < keys + n && *key != 0; key += wcslen(key) + 1) {
        const std::wstring name = trimmed(ini_get(L"pivot.order", key));
        if (!name.empty() && std::iswdigit(key[0]))
            order.push_back({_wtoi(key), name});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [index, name] : order)
        overlays_.push_back({name, true});
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((pivot_root_ + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        std::vector<std::wstring> others;
        do {
            const std::wstring name = found.cFileName;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || name == L"." || name == L"..")
                continue;
            if (std::none_of(overlays_.begin(), overlays_.end(), [&](const Overlay& o) { return same(o.name, name); }))
                others.push_back(name);
        } while (FindNextFileW(find, &found));
        FindClose(find);
        std::sort(others.begin(), others.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (const std::wstring& name : others)
            overlays_.push_back({name, false});
    }
}

// [pivot.order] as the list has it: its keys each taken out (the section's notes stay), then 0= on for the ones that are on
void Home::save_pivot()
{
    wchar_t keys[8192] = {};
    const DWORD n = GetPrivateProfileStringW(L"pivot.order", nullptr, L"", keys, 8192, ini_.c_str());
    std::vector<std::wstring> old;
    for (const wchar_t* key = keys; key < keys + n && *key != 0; key += wcslen(key) + 1)
        old.push_back(key);
    for (const std::wstring& key : old)
        ini_set(L"pivot.order", key.c_str(), nullptr);
    int index = 0;
    for (const Overlay& o : overlays_)
        if (o.on)
            ini_set(L"pivot.order", std::to_wstring(index++).c_str(), &o.name);
}

// --- the pages ---------------------------------------------------------------------------------------------------------

float Home::t_of(int hit) const
{
    return hover_ != hit ? 0.0f : pressed_ == hit ? 1.0f : 0.6f;
}

// A fact: a tick (teal) or a cross (rose), a line, and a dimmer one under it
void Home::fact(float x, float cy, bool ok, const std::wstring& title, const std::wstring& detail, float room)
{
    canvas_.glyph(ok ? ui::icon::check : ui::icon::cancel, ui::Font::IconBig, x + 12, cy, ui::rgb(ok ? ui::pal::teal : ui::pal::rose));
    canvas_.text(canvas_.fit(title, ui::Font::Body, room - 36, false), ui::Font::Body, x + 36, cy - 9, ui::rgb(ui::pal::text));
    canvas_.text(canvas_.fit(detail, ui::Font::Small, room - 36, true), ui::Font::Small, x + 36, cy + 10, ui::rgb(ui::pal::dim));
}

void Home::draw_page(const D2D1_RECT_F& r)
{
    canvas_.fill(r.left, r.top, r.right - r.left, r.bottom - r.top, 8, ui::rgb(ui::pal::ink, 0.92f));
    canvas_.inset_shadow(r, 8, 14);
    canvas_.outline(r.left, r.top, r.right - r.left, r.bottom - r.top, 8, ui::rgb(ui::pal::white, 0.08f));
    const D2D1_RECT_F in = D2D1::RectF(r.left + 16, r.top + 14, r.right - 16, r.bottom - 14);
    switch (tab_) {
    case kFfxi:
        page_ffxi(in);
        break;
    case kGraphics:
        page_graphics(in);
        break;
    case kPivot:
        page_pivot(in);
        break;
    case kProfiles:
        page_profiles(in);
        break;
    case kErrors:
        page_errors(in);
        break;
    case kLogs:
        page_logs(in);
        break;
    case kAddons:
        page_addons(in);
        break;
    default:
        page_about(in);
        break;
    }
}

void Home::page_ffxi(const D2D1_RECT_F& r)
{
    const float w = r.right - r.left, row = 46;
    float cy = r.top + 18;
    // FINAL FANTASY XI: a tick and its folder, or a cross and Locate
    const bool found = !install_.ffxi.empty();
    const float bw = 112;
    std::wstring where = install_.ffxi;
    if (found && install_.registry)
        where += L"  (" + install_.region + L", the registry)";
    fact(r.left, cy, found, found ? L"FINAL FANTASY XI" : L"FINAL FANTASY XI not found",
         found ? where : L"Not in the registry: show VanaCore where it is", found ? w : w - bw - 12);
    if (!found) {
        const D2D1_RECT_F b = D2D1::RectF(r.right - bw, cy - 15, r.right, cy + 15);
        canvas_.button(b, L"Locate\x2026", ui::Button::Accent, t_of(kLocate), true, ui::icon::folder);
        hits_.add(kLocate, b);
    }
    cy += row;
    // VanaCore beside the app
    fact(r.left, cy, !game_.empty(), game_.empty() ? L"VanaCore not found" : L"VanaCore" + (vanacore_.empty() ? L"" : L" v" + vanacore_),
         game_.empty() ? L"Put VanaCoreApp in VanaCore's folder" : game_, w);
    cy += row;
    // the game's tools: its gamepad config, and its folder
    const float y = cy - 6;
    const std::wstring pad = L"Gamepad Config", folder = L"Open Folder";
    const float pw = canvas_.button_width(pad) + 24, fw = canvas_.button_width(folder) + 24;
    const D2D1_RECT_F p = D2D1::RectF(r.left, y - 15, r.left + pw, y + 15);
    canvas_.button(p, pad, ui::Button::Secondary, t_of(kPad), !install_.pad.empty(), ui::icon::gamepad);
    if (!install_.pad.empty())
        hits_.add(kPad, p);
    const D2D1_RECT_F f = D2D1::RectF(p.right + 8, y - 15, p.right + 8 + fw, y + 15);
    canvas_.button(f, folder, ui::Button::Secondary, t_of(kFfxiFolder), found, ui::icon::folder);
    if (found)
        hits_.add(kFfxiFolder, f);
}

void Home::page_graphics(const D2D1_RECT_F& r)
{
    std::wstring now = ini_get(L"graphics", L"renderer", L"d3d8");
    float y = r.top + 6;
    canvas_.text(L"RENDERER", ui::Font::Label, r.left, y, ui::rgb(ui::pal::dim));
    y += 28;
    float x = r.left;
    int picked = 3;
    for (int i = 0; i < 4; ++i)
        if (same(now, kRenderers[i].value))
            picked = i;
    for (int i = 0; i < 4; ++i) {
        const float bw = canvas_.button_width(kRenderers[i].name) + 4;
        const D2D1_RECT_F b = D2D1::RectF(x, y - 15, x + bw, y + 15);
        canvas_.button(b, kRenderers[i].name, i == picked ? ui::Button::Accent : ui::Button::Secondary, t_of(kRenderer + i));
        hits_.add(kRenderer + i, b);
        x += bw + 6;
    }
    // the window's mode, the game's resolution and the interface's: /vana's three drop-downs (Client > Display, Resolution)
    y += 38;
    static const wchar_t* const kLabels[kDrops] = {L"MODE", L"RESOLUTION", L"UI RESOLUTION"};
    const float gap = 12, w = (r.right - r.left - gap * 2) / 3;
    for (int d = 0; d < kDrops; ++d) {
        const float fx = r.left + d * (w + gap);
        canvas_.text(kLabels[d], ui::Font::Label, fx, y, ui::rgb(ui::pal::dim));
        const D2D1_RECT_F field = D2D1::RectF(fx, y + 12, fx + w, y + 42);
        const bool open = drop_ == d, over = hover_ == kDrop + d;
        canvas_.fill(field.left, field.top, w, 30, 4, ui::rgb(over || open ? ui::pal::hover : ui::pal::well, 0.95f));
        canvas_.outline(field.left, field.top, w, 30, 4, open ? ui::rgb(ui::pal::accent, 0.8f) : ui::rgb(over ? ui::pal::accent_lo : ui::pal::well_edge));
        const std::vector<Choice> list = choices(d);
        const std::wstring value = drop_value(d);
        std::wstring shown = value;
        for (const Choice& c : list)
            if (!c.sep && same(c.value, value))
                shown = c.label;
        const std::size_t dash = shown.find(L" - "); // (too long for the field: without its size)
        if (canvas_.text_width(shown, ui::Font::Body) > w - 40 && dash != std::wstring::npos)
            shown.resize(dash);
        canvas_.text(canvas_.fit(shown, ui::Font::Body, w - 40, false), ui::Font::Body, field.left + 10, field.top + 14, ui::rgb(ui::pal::text));
        canvas_.glyph(open ? ui::icon::up : ui::icon::down, ui::Font::IconBig, field.right - 16, field.top + 15, ui::rgb(over || open ? ui::pal::white : ui::pal::text2));
        hits_.add(kDrop + d, field);
        drop_at_[d] = field;
    }
}

// --- the Graphics tab's drop-downs ----------------------------------------------------------------------------------------

// What a drop-down has now: [ffxi] mode (unset: Borderless fullscreen, as VanaCore hands it to the game), resolution.game
// (unset: auto), resolution.ui (unset: Same as Resolution)
std::wstring Home::drop_value(int d) const
{
    if (d == kMode) {
        std::wstring mode = ini_get(L"ffxi", L"mode");
        mode.erase(std::remove_if(mode.begin(), mode.end(), [](wchar_t c) { return c == L' ' || c == L'_' || c == L'-'; }), mode.end());
        for (wchar_t& c : mode)
            c = static_cast<wchar_t>(std::towlower(c));
        return mode.empty() ? L"borderlessfullscreen" : mode == L"0" ? L"fullscreen" : mode == L"1" ? L"windowed" : mode == L"2" ? L"borderless" : mode == L"3" ? L"borderlessfullscreen" : mode;
    }
    std::wstring v = ini_get(L"ffxi", d == kResolution ? L"resolution.game" : L"resolution.ui");
    v.erase(std::remove(v.begin(), v.end(), L' '), v.end());
    for (wchar_t& c : v)
        c = static_cast<wchar_t>(std::towlower(c));
    return v.empty() && d == kResolution ? L"auto" : v;
}

// A drop-down's choices, as /vana lists them (data.lua's Client page): the mode's three (the old Full screen too while the
// file has it); the resolutions' shares of the screen (and of the game window for the interface's), a line, then the sizes,
// widest first, the screen's own marked (added when it isn't one of them), and a size the file has that isn't listed
std::vector<Home::Choice> Home::choices(int d) const
{
    std::vector<Choice> out;
    if (d == kMode) {
        if (drop_value(kMode) == L"fullscreen")
            out.push_back({L"fullscreen", L"Full screen"});
        out.push_back({L"windowed", L"Windowed"});
        out.push_back({L"borderless", L"Borderless windowed"});
        out.push_back({L"borderlessfullscreen", L"Borderless fullscreen"});
        return out;
    }
    // the screen the app is on, and its work area (less the taskbar), in pixels
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &mi);
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
    auto size = [](int w, int h) { return std::to_wstring(w) + L" x " + std::to_wstring(h); };
    if (d == kUiResolution) {
        out.push_back({L"", L"Same as Resolution"});
        out.push_back({L"window", L"Fit the game window"});
        out.push_back({L"window-75", L"75% of the game window"});
        out.push_back({L"window-50", L"50% of the game window"});
    }
    for (int share : {100, 75, 50}) {
        const std::wstring at = mw > 0 ? L" - " + size((mw * share + 50) / 100, (mh * share + 50) / 100) : L"";
        out.push_back({share == 100 ? L"auto" : L"auto-" + std::to_wstring(share), (share == 100 ? L"Screen (auto)" : std::to_wstring(share) + L"% of the screen") + at});
        if (share == 100 && d == kResolution)
            out.push_back({L"auto-taskbar", L"Screen without the taskbar" + (ww > 0 ? L" - " + size(ww, wh) : L"")});
    }
    out.push_back({L"", L"", true});
    static const int kSizes[][2] = {{1280, 720}, {1280, 800}, {1366, 768}, {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080},
                                    {1920, 1200}, {2400, 1350}, {2560, 1080}, {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 2160}};
    std::vector<std::pair<int, int>> sizes;
    for (const auto& s : kSizes)
        sizes.push_back({s[0], s[1]});
    if (mw > 0 && std::find(sizes.begin(), sizes.end(), std::make_pair(mw, mh)) == sizes.end())
        sizes.push_back({mw, mh});
    std::sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
    for (const auto& [w, h] : sizes)
        out.push_back({std::to_wstring(w) + L"x" + std::to_wstring(h), size(w, h) + (w == mw && h == mh ? L" (screen)" : L"")});
    const std::wstring now = drop_value(d);
    if (std::none_of(out.begin(), out.end(), [&](const Choice& c) { return !c.sep && same(c.value, now); }))
        out.push_back({now, now});
    return out;
}

// The open drop-down's list, under its field (above it when there's no room): eight rows at most, the wheel scrolls the rest
void Home::draw_drop()
{
    const std::vector<Choice> list = choices(drop_);
    const D2D1_RECT_F field = drop_at_[drop_];
    const float row = 28, sep = 9;
    const int most = 8, count = static_cast<int>(list.size());
    auto height_of = [&](const Choice& c) { return c.sep ? sep : row; };
    // the rows that show: from drop_scroll_, as many as fit in `most` rows' height
    drop_scroll_ = std::max(0, std::min(drop_scroll_, std::max(0, count - most)));
    float h = 8;
    int shown = 0;
    for (int i = drop_scroll_; i < count && h + height_of(list[i]) <= most * row + 8; ++i, ++shown)
        h += height_of(list[i]);
    const float w = std::max(field.right - field.left, 250.0f);
    const float below = field.bottom + 4, above = field.top - 4 - h;
    const float top = below + h <= canvas_.height() - 8 || above < 8 ? below : above;
    const float left = std::min(field.left, canvas_.width() - 8 - w);
    for (int i = 5; i >= 1; --i) {
        const float g = static_cast<float>(i);
        canvas_.fill(left - g, top - g + 4, w + 2 * g, h + 2 * g, 6 + g, ui::rgb(0x000000, 0.07f));
    }
    canvas_.fill(left, top, w, h, 6, ui::rgb(0x0B0E18, 0.98f));
    canvas_.outline(left, top, w, h, 6, ui::rgb(ui::pal::white, 0.14f));
    const std::wstring now = drop_value(drop_);
    float y = top + 4;
    for (int k = 0; k < shown; ++k) {
        const int i = drop_scroll_ + k;
        const Choice& c = list[i];
        if (c.sep) {
            canvas_.line(left + 10, y + sep / 2, left + w - 10, y + sep / 2, ui::rgb(ui::pal::white, 0.1f));
            y += sep;
            continue;
        }
        const bool picked = same(c.value, now), over = hover_ == kDropItem + i;
        if (over)
            canvas_.fill(left + 4, y, w - 8, row, 4, ui::rgb(ui::pal::white, 0.08f));
        if (picked)
            canvas_.glyph(ui::icon::check, ui::Font::Icon, left + 18, y + row / 2, ui::rgb(ui::pal::teal));
        canvas_.text(canvas_.fit(c.label, ui::Font::Body, w - 44, false), ui::Font::Body, left + 34, y + row / 2 - 1,
                     ui::rgb(picked || over ? ui::pal::white : ui::pal::text2));
        hits_.add(kDropItem + i, D2D1::RectF(left + 4, y, left + w - 4, y + row));
        y += row;
    }
    if (drop_scroll_ > 0 || drop_scroll_ + shown < count) // (more than show: where in the list)
        canvas_.fill(left + w - 4, top + 6 + (h - 12) * drop_scroll_ / count, 2, std::max(12.0f, (h - 12) * shown / count), 1, ui::rgb(ui::pal::white, 0.25f));
    // (the list's rows first: Hits::at takes the first match, and the list lies over the page)
    std::stable_partition(hits_.list.begin(), hits_.list.end(), [](const auto& hit) { return hit.first >= kDropItem; });
}

// A pick from the open drop-down: written to the profile's file (a resolution pick also takes out [window] lastsize, the size
// the window's edges were dragged to, as /vana does, so the window is the size picked)
void Home::pick(int index)
{
    const std::vector<Choice> list = choices(drop_);
    if (index < 0 || index >= static_cast<int>(list.size()) || list[index].sep)
        return;
    const std::wstring& value = list[index].value;
    if (drop_ == kMode) {
        ini_set(L"ffxi", L"mode", &value);
    } else {
        ini_set(L"ffxi", drop_ == kResolution ? L"resolution.game" : L"resolution.ui", value.empty() ? nullptr : &value);
        if (drop_ == kResolution)
            ini_set(L"window", L"lastsize", nullptr);
    }
}

void Home::page_pivot(const D2D1_RECT_F& r)
{
    // the switch, the root folder, Choose and Open
    float y = r.top + 8;
    canvas_.toggle(r.left, y, pivot_on_, hover_ == kPivotOn);
    canvas_.text(L"Pivot", ui::Font::Body, r.left + 42, y - 1, ui::rgb(ui::pal::text));
    hits_.add(kPivotOn, D2D1::RectF(r.left, y - 11, r.left + 42 + canvas_.text_width(L"Pivot", ui::Font::Body), y + 11));
    const float ow = 30, cw = canvas_.text_width(L"Choose\x2026", ui::Font::Button) + 24 + 28; // (the icon's 24, the padding)
    const D2D1_RECT_F open = D2D1::RectF(r.right - ow, y - 13, r.right, y + 13), choose = D2D1::RectF(open.left - 6 - cw, y - 13, open.left - 6, y + 13);
    canvas_.button(choose, L"Choose\x2026", ui::Button::Secondary, t_of(kPivotRoot), true, ui::icon::folder);
    canvas_.button(open, L"", ui::Button::Ghost, t_of(kPivotOpen), is_folder(pivot_root_), ui::icon::open);
    hits_.add(kPivotRoot, choose);
    if (is_folder(pivot_root_))
        hits_.add(kPivotOpen, open);
    tip(kPivotOpen, L"Open the overlays' folder");
    const float px = r.left + 42 + canvas_.text_width(L"Pivot", ui::Font::Body) + 14;
    canvas_.text(canvas_.fit(pivot_root_, ui::Font::Small, choose.left - 10 - px, true), ui::Font::Small, px, y, ui::rgb(ui::pal::dim));
    // the overlays: on ones in their order (looked in first at the top), then the rest
    y += 22;
    canvas_.line(r.left, y, r.right, y, ui::rgb(ui::pal::white, 0.08f));
    const float row = 27, top = y + 4;
    const int rows = static_cast<int>((r.bottom - top) / row);
    if (overlays_.empty()) {
        canvas_.text(is_folder(pivot_root_) ? L"No overlays here yet: each mod goes in a folder of its own in it" : L"There's no such folder yet: Choose one",
                     ui::Font::Small, r.left, top + row / 2, ui::rgb(ui::pal::dim));
        return;
    }
    pivot_scroll_ = std::max(0, std::min(pivot_scroll_, static_cast<int>(overlays_.size()) - rows));
    int on_count = 0;
    for (const Overlay& o : overlays_)
        on_count += o.on ? 1 : 0;
    for (int k = 0; k < rows && pivot_scroll_ + k < static_cast<int>(overlays_.size()); ++k) {
        const int i = pivot_scroll_ + k;
        const Overlay& o = overlays_[i];
        const float cy = top + k * row + row / 2;
        const int id = kRow + i * 4;
        if (hover_ >= id && hover_ < id + 4)
            canvas_.fill(r.left - 6, cy - row / 2 + 1, r.right - r.left + 12, row - 2, 4, ui::rgb(ui::pal::white, 0.04f));
        canvas_.toggle(r.left, cy, o.on, hover_ == id);
        hits_.add(id, D2D1::RectF(r.left, cy - 10, r.left + 30, cy + 10));
        tip(id, o.on ? L"Turn off" : L"Turn on");
        if (o.on)
            canvas_.text(std::to_wstring(i + 1), ui::Font::Small, r.left + 40, cy, ui::rgb(ui::pal::accent2));
        canvas_.text(canvas_.fit(o.name, ui::Font::Body, r.right - r.left - 130, true), ui::Font::Body, r.left + 58, cy - 1,
                     ui::rgb(o.on ? ui::pal::text : ui::pal::dim));
        if (!o.on)
            continue;
        const bool up = i > 0, down = i + 1 < on_count;
        const float ax = r.right - 52;
        for (int part = 1; part <= 2; ++part) {
            const bool can = part == 1 ? up : down;
            const D2D1_RECT_F b = D2D1::RectF(ax + (part - 1) * 26, cy - 11, ax + (part - 1) * 26 + 24, cy + 11);
            if (can && hover_ == id + part)
                canvas_.fill(b.left, b.top, b.right - b.left, b.bottom - b.top, 4, ui::rgb(ui::pal::white, pressed_ == id + part ? 0.16f : 0.1f));
            canvas_.glyph(part == 1 ? ui::icon::up : ui::icon::down, ui::Font::IconBig, (b.left + b.right) / 2, cy,
                          ui::rgb(can ? (hover_ == id + part ? ui::pal::white : ui::pal::text2) : ui::pal::faint));
            if (can)
                hits_.add(id + part, b), tip(id + part, part == 1 ? L"Up: looked in sooner" : L"Down: looked in later");
        }
    }
    if (static_cast<int>(overlays_.size()) > rows) // (more than fit: the wheel scrolls them)
        canvas_.text(std::to_wstring(pivot_scroll_ + 1) + L"\x2013" + std::to_wstring(std::min<int>(pivot_scroll_ + rows, static_cast<int>(overlays_.size()))) +
                         L" of " + std::to_wstring(overlays_.size()),
                     ui::Font::Small, r.right - 64, r.bottom + 3, ui::rgb(ui::pal::faint));
}

void Home::page_about(const D2D1_RECT_F& r)
{
    float y = r.top + 8;
    canvas_.text(L"VanaCore brings FINAL FANTASY XI up to date: new renderers, effects, a cleaner interface, addons", ui::Font::Small, r.left, y,
                 ui::rgb(ui::pal::text2));
    y += 18;
    canvas_.text(L"and mods, while the game stays the game. Free and open source.", ui::Font::Small, r.left, y, ui::rgb(ui::pal::text2));
    y += 30;
    const float kx = r.left, vx = r.left + 130;
    const char* const app_version = VCAPP_DLL_VERSION_STR;
    const std::wstring app = L"v" + std::wstring(app_version, app_version + std::strlen(app_version));
    const std::pair<std::wstring, std::wstring> facts[] = {
        {L"VanaCore", vanacore_.empty() ? L"not found" : L"v" + vanacore_},
        {L"VanaCore App", app},
        {L"FINAL FANTASY XI", install_.ffxi.empty() ? L"not found" : ffxi_date_.empty() ? L"installed" : L"updated " + ffxi_date_},
        {L"Region", install_.ffxi.empty() ? L"-" : install_.region == L"jp" ? L"Japan" : install_.region == L"eu" ? L"Europe" : L"North America"},
    };
    for (const auto& [key, value] : facts) {
        canvas_.text(key, ui::Font::Small, kx, y, ui::rgb(ui::pal::dim));
        canvas_.text(value, ui::Font::Small, vx, y, ui::rgb(key == L"VanaCore" && !vanacore_.empty() ? 0xF3DE9C : ui::pal::text));
        y += 20;
    }
    const std::wstring gh = L"GitHub";
    const float gw = canvas_.button_width(gh) + 16;
    const D2D1_RECT_F b = D2D1::RectF(r.right - gw, r.bottom - 30, r.right, r.bottom);
    canvas_.button(b, gh, ui::Button::Secondary, t_of(kGithub), true, ui::icon::open);
    hits_.add(kGithub, b);
}

// A click on a page's control: true when it changed something to draw
bool Home::press_page(int hit)
{
    if (hit == kLocate) {
        locate();
        return true;
    }
    if (hit == kPad) {
        open(install_.pad);
        return false;
    }
    if (hit == kFfxiFolder) {
        open(install_.ffxi);
        return false;
    }
    if (hit >= kRenderer && hit < kRenderer + 4) {
        const std::wstring value = kRenderers[hit - kRenderer].value;
        ini_set(L"graphics", L"renderer", &value);
        return true;
    }
    if (hit == kPivotOn) {
        pivot_on_ = !pivot_on_;
        const std::wstring value = pivot_on_ ? L"1" : L"0";
        ini_set(L"pivot", L"enabled", &value);
        return true;
    }
    if (hit == kPivotRoot) {
        std::wstring root = pick_folder(L"The folder holding Pivot's overlays");
        if (root.empty())
            return false;
        const std::wstring under = dir_ + L"\\";
        if (root.size() > under.size() && _wcsnicmp(root.c_str(), under.c_str(), under.size()) == 0)
            root = root.substr(under.size()); // (VanaCore's own folder: kept relative, as the default)
        ini_set(L"pivot", L"rootfolder", &root);
        load_pivot();
        return true;
    }
    if (hit == kPivotOpen) {
        open(pivot_root_);
        return false;
    }
    if (hit >= kRow) {
        const int i = (hit - kRow) / 4, part = (hit - kRow) % 4;
        if (i >= static_cast<int>(overlays_.size()))
            return false;
        if (part == 0) { // on: to the end of the on ones; off: to the top of the off ones
            Overlay o = overlays_[i];
            o.on = !o.on;
            overlays_.erase(overlays_.begin() + i);
            const auto at = std::find_if(overlays_.begin(), overlays_.end(), [](const Overlay& x) { return !x.on; });
            overlays_.insert(at, o);
        } else if (part == 1 && i > 0) {
            std::swap(overlays_[i], overlays_[i - 1]);
        } else if (part == 2 && i + 1 < static_cast<int>(overlays_.size()) && overlays_[i + 1].on) {
            std::swap(overlays_[i], overlays_[i + 1]);
        }
        save_pivot();
        return true;
    }
    return false;
}

} // namespace vcapp
