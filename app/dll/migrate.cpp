// Ashita's or Windower's addons, plugins, scripts and boot script brought over to VanaCore (migrate.h says what goes where)
#include "migrate.h"

#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <set>

namespace vcapp::migrate {

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

bool same(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::wstring no_slash(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

std::string utf8(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// A file's bytes (a UTF-8 BOM taken off); false when it can't be read
bool read_file(const std::wstring& path, std::string& out)
{
    out.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    if (ok && size.QuadPart > 0) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file);
    if (out.rfind("\xEF\xBB\xBF", 0) == 0)
        out.erase(0, 3);
    return ok;
}

// The names in `folder`: its folders (`folders`) or its files ending in `ext` (without it)
std::vector<std::wstring> names_in(const std::wstring& folder, bool folders, const wchar_t* ext = L"")
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return out;
    const std::size_t el = wcslen(ext);
    do {
        const std::wstring name = found.cFileName;
        const bool folder_here = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (name == L"." || name == L".." || folder_here != folders || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            continue;
        if (folders)
            out.push_back(name);
        else if (name.size() > el && same(name.substr(name.size() - el), ext))
            out.push_back(name.substr(0, name.size() - el));
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

// Every file under `folder` (its folders' too), relative to it
void files_under(const std::wstring& folder, const std::wstring& rel, std::vector<std::wstring>& out)
{
    const std::wstring here = rel.empty() ? folder : folder + L"\\" + rel;
    for (const std::wstring& f : names_in(here, false))
        out.push_back(rel.empty() ? f : rel + L"\\" + f);
    for (const std::wstring& d : names_in(here, true))
        files_under(folder, rel.empty() ? d : rel + L"\\" + d, out);
}

bool listed(const std::vector<std::wstring>& names, const std::string& name)
{
    const std::wstring w = wide(name);
    return std::any_of(names.begin(), names.end(), [&](const std::wstring& n) { return same(n, w); });
}

std::vector<std::string> words(const std::string& line)
{
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = line.find_first_not_of(" \t", at)) != std::string::npos) {
        const std::size_t end = line.find_first_of(" \t", at);
        out.push_back(line.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end;
    }
    return out;
}

// What follows the first `n` words of `line`, trimmed
std::string after_words(const std::string& line, int n)
{
    std::size_t at = 0;
    for (int i = 0; i < n; ++i) {
        at = line.find_first_not_of(" \t", at);
        if (at == std::string::npos)
            return {};
        at = line.find_first_of(" \t", at);
        if (at == std::string::npos)
            return {};
    }
    at = line.find_first_not_of(" \t", at);
    return at == std::string::npos ? std::string() : line.substr(at);
}

// A line that loads an addon or plugin: "a:<name>" or "p:<name>" (lower case), else ""
std::string load_key(const std::string& line)
{
    const std::vector<std::string> parts = words(line);
    if (parts.size() < 3 || lower(parts[1]) != "load")
        return {};
    const std::string command = lower(parts[0]);
    if (command == "/addon" || command == "//addon")
        return "a:" + lower(parts[2]);
    if (command == "/plugin" || command == "//plugin")
        return "p:" + lower(parts[2]);
    return {};
}

// The script's lines, trimmed, without blanks and comments (# and, in Windower's, --)
std::vector<std::string> script_lines(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        const std::size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#' || line.compare(a, 2, "--") == 0)
            continue;
        out.push_back(line.substr(a, line.find_last_not_of(" \t\r") - a + 1));
    }
    return out;
}

// An Ashita boot script's line as boot.txt's: Ashita's /load <plugin> is VanaCore's /plugin load; an addon or plugin that's
// being copied is loaded from its kind's folder (ashita) so a VanaCore or Windower one of the same name isn't
void ashita_line(const std::string& line, const Found& f, Found& out)
{
    const std::vector<std::string> parts = words(line);
    const std::string command = lower(parts[0]);
    if (command == "/load" && parts.size() >= 2) {
        const std::string name = lower(parts[1]);
        if (name == "addons")
            out.lines.push_back("# " + line + "   (Ashita's addon host: VanaCore runs Ashita addons itself)");
        else if (name == "sequencer")
            out.lines.push_back("# " + line + "   (Sequencer patches Ashita's own code, which VanaCore doesn't have)");
        else
            out.lines.push_back("/plugin load " + parts[1] + (listed(f.plugins, parts[1]) ? " ashita" : "")), ++out.loads;
        return;
    }
    if (command == "/unload" || ((command == "/addon" || command == "/plugin") && parts.size() >= 2 && lower(parts[1]) == "unload")) {
        out.lines.push_back("# " + line);
        return;
    }
    if ((command == "/addon" || command == "//addon") && parts.size() >= 3 && lower(parts[1]) == "load") {
        const std::string rest = after_words(line, 3);
        out.lines.push_back("/addon load " + parts[2] + (!rest.empty() ? " " + rest : listed(f.addons, parts[2]) ? " ashita" : ""));
        ++out.loads;
        return;
    }
    if ((command == "/plugin" || command == "//plugin") && parts.size() >= 3 && lower(parts[1]) == "load") {
        const std::string rest = after_words(line, 3);
        out.lines.push_back("/plugin load " + parts[2] + (!rest.empty() ? " " + rest : listed(f.plugins, parts[2]) ? " ashita" : ""));
        ++out.loads;
        return;
    }
    // Ashita's /input <command> (the command straight to the game) as the command itself, alone or as a bind's: VanaCore
    // has no /input
    if (command == "/input" && parts.size() >= 2) {
        out.lines.push_back(after_words(line, 1));
        return;
    }
    if (command == "/bind" && parts.size() >= 4 && lower(parts[2]) == "/input") {
        out.lines.push_back(parts[0] + " " + parts[1] + " " + after_words(line, 3));
        return;
    }
    out.lines.push_back(line); // (/bind, /alias, /wait, /exec, /include, a plugin's or the game's command: as they are)
}

// A Windower console command (init.txt's, a //line's) as VanaCore's command; "" when it's Windower's own
std::string windower_command(const std::string& command)
{
    const std::vector<std::string> parts = words(command);
    if (parts.empty())
        return {};
    const std::string word = lower(parts[0]);
    if (word == "input")
        return after_words(command, 1);
    if (command[0] == '/')
        return command;
    return "/" + command; // (an addon's command: one slash in VanaCore)
}

// Windower's settings, which only Windower has
bool windower_own(const std::string& word)
{
    static const char* const kPrefixes[] = {"console_", "game_", "keyboard_", "mouse_", "window_", "chatlog_"};
    for (const char* p : kPrefixes)
        if (word.rfind(p, 0) == 0)
            return true;
    return word == "setkey" || word == "showfps" || word == "unload" || word == "u";
}

void windower_line(std::string line, const Found& f, Found& out)
{
    if (line.rfind("//", 0) == 0) {
        const std::size_t at = line.find_first_not_of('/');
        if (at == std::string::npos)
            return;
        line = line.substr(at, line.find_last_not_of(" 	") - at + 1);
        line.erase(0, line.find_first_not_of(" 	"));
        if (line.empty())
            return;
    } else if (line[0] == '/') {
        out.lines.push_back(line); // (a game command: as it is)
        return;
    }
    const std::vector<std::string> parts = words(line);
    if (parts.empty())
        return;
    const std::string word = lower(parts[0]);
    const std::string sub = parts.size() > 1 ? lower(parts[1]) : "";
    if (word == "lua") {
        if ((sub == "load" || sub == "l") && parts.size() >= 3) {
            out.lines.push_back("/addon load " + parts[2] + (listed(f.addons, parts[2]) ? " windower" : ""));
            ++out.loads;
        } else {
            out.lines.push_back("# " + line);
        }
        return;
    }
    if ((word == "load" || word == "l") && parts.size() >= 2) {
        out.lines.push_back("/plugin load " + parts[1] + (listed(f.plugins, parts[1]) ? " windower" : ""));
        ++out.loads;
        return;
    }
    if (word == "wait") {
        out.lines.push_back("/wait " + after_words(line, 1));
        return;
    }
    if (word == "bind" || word == "unbind" || word == "exec") {
        out.lines.push_back("/" + line); // (a bind's command may stay Windower's: input /x, gs c y)
        return;
    }
    if (word == "alias" && parts.size() >= 3) { // alias <name> <command>: typed as /<name>
        out.lines.push_back("/alias /" + parts[1] + " " + windower_command(after_words(line, 2)));
        return;
    }
    if (windower_own(word)) {
        out.lines.push_back("# " + line + "   (Windower's own)");
        return;
    }
    out.lines.push_back(windower_command(line));
}

// The plugins Windower's launcher loads at start: settings.xml's <plugin> names
std::vector<std::string> windower_autoload(const std::wstring& root)
{
    std::vector<std::string> out;
    std::string xml;
    if (!read_file(root + L"\\settings.xml", xml))
        return out;
    for (std::size_t at = 0; (at = xml.find("<plugin>", at)) != std::string::npos;) {
        at += 8;
        const std::size_t end = xml.find("</plugin>", at);
        if (end == std::string::npos)
            break;
        std::string name = xml.substr(at, end - at);
        const std::size_t a = name.find_first_not_of(" \t\r\n"), b = name.find_last_not_of(" \t\r\n");
        if (a != std::string::npos)
            out.push_back(name.substr(a, b - a + 1));
        at = end;
    }
    return out;
}

bool is_ashita(const std::wstring& p)
{
    return exists(p + L"\\Ashita.dll") || exists(p + L"\\Ashita-cli.exe") || exists(p + L"\\Ashita.exe") ||
           (is_folder(p + L"\\config\\boot") && is_folder(p + L"\\addons"));
}

bool is_windower(const std::wstring& p)
{
    return exists(p + L"\\Windower.exe") || (exists(p + L"\\settings.xml") && is_folder(p + L"\\addons") && is_folder(p + L"\\plugins"));
}

// The install of `kind` at `at` or in a folder of its name under it
std::wstring locate(Kind kind, const std::wstring& at)
{
    static const wchar_t* const kAshita[] = {L"", L"\\Ashita", L"\\Ashita4", L"\\Ashita v4", L"\\Ashita3"};
    static const wchar_t* const kWindower[] = {L"", L"\\Windower4", L"\\Windower", L"\\Windower 4"};
    if (kind == Kind::Ashita) {
        for (const wchar_t* sub : kAshita)
            if (is_ashita(at + sub))
                return at + sub;
    } else {
        for (const wchar_t* sub : kWindower)
            if (is_windower(at + sub))
                return at + sub;
    }
    return {};
}

std::wstring today()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    static const wchar_t* const kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    return std::to_wstring(t.wDay) + L" " + kMonths[(t.wMonth + 11) % 12] + L" " + std::to_wstring(t.wYear);
}

// `from`'s files copied to `to` (made where it isn't there), its folders' too; a file `to` has already is left as it is
int copy_tree(const std::wstring& from, const std::wstring& to, Result& r)
{
    SHCreateDirectoryExW(nullptr, to.c_str(), nullptr);
    int copied = 0;
    for (const std::wstring& f : names_in(from, false)) {
        if (CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), TRUE))
            ++copied, ++r.files;
        else if (GetLastError() == ERROR_FILE_EXISTS)
            ++r.kept;
        else
            ++r.failed;
    }
    for (const std::wstring& d : names_in(from, true))
        copied += copy_tree(from + L"\\" + d, to + L"\\" + d, r);
    return copied;
}

bool copy_file(const std::wstring& from, const std::wstring& to, Result& r)
{
    const std::size_t slash = to.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        SHCreateDirectoryExW(nullptr, to.substr(0, slash).c_str(), nullptr);
    if (CopyFileW(from.c_str(), to.c_str(), TRUE)) {
        ++r.files;
        return true;
    }
    if (GetLastError() == ERROR_FILE_EXISTS)
        ++r.kept;
    else
        ++r.failed;
    return false;
}

bool write_file(const std::wstring& path, const std::string& text)
{
    const std::wstring temp = path + L".tmp";
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = text.empty() || (WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size());
    CloseHandle(file);
    if (ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(temp.c_str());
    return false;
}

bool is_wait(const std::string& line)
{
    const std::vector<std::string> parts = words(line);
    const std::string c = parts.empty() ? "" : lower(parts[0]);
    return c == "/wait" || c == "/sleep" || c == "/pause" || c == "//wait";
}

// found's lines added to scripts\boot.txt (those it doesn't have: a load of the same addon or plugin, the same line)
void add_to_boot(const Found& found, const std::wstring& vanacore, Result& r)
{
    const std::wstring scripts = vanacore + L"\\scripts", boot = scripts + L"\\boot.txt";
    std::string text;
    const bool had = read_file(boot, text);
    std::set<std::string> keys, seen;
    for (const std::string& line : script_lines(text)) {
        const std::string key = load_key(line);
        if (!key.empty())
            keys.insert(key);
        seen.insert(lower(line));
    }
    std::vector<std::string> add;
    int real = 0;
    for (const std::string& line : found.lines) {
        if (line[0] == '#' || is_wait(line)) {
            add.push_back(line);
            continue;
        }
        const std::string key = load_key(line);
        if ((!key.empty() && keys.count(key) != 0) || seen.count(lower(line)) != 0)
            continue;
        if (!key.empty())
            keys.insert(key), ++r.loads;
        seen.insert(lower(line));
        add.push_back(line);
        ++real;
    }
    while (!add.empty() && is_wait(add.back())) // (a wait at the end holds up nothing)
        add.pop_back();
    if (real == 0)
        return;
    if (had)
        CopyFileW(boot.c_str(), (boot + L".before-migrate").c_str(), TRUE); // (the first time only)
    else
        SHCreateDirectoryExW(nullptr, scripts.c_str(), nullptr);
    if (!text.empty() && text.back() != '\n')
        text += "\r\n";
    text += "\r\n# From " + utf8(name_of(found.kind)) + " (" + utf8(found.script.empty() ? found.root : found.script) + "), " + utf8(today()) + "\r\n";
    for (const std::string& line : add)
        text += line + "\r\n";
    if (!write_file(boot, text)) {
        r.boot_failed = true;
        r.loads = 0;
        return;
    }
    r.lines = static_cast<int>(add.size());
}

} // namespace

const wchar_t* name_of(Kind kind)
{
    return kind == Kind::Ashita ? L"Ashita" : L"Windower";
}

bool find(Kind kind, const std::wstring& picked, const std::wstring& vanacore, Found& out, std::wstring& why)
{
    out = {};
    out.kind = kind;
    const std::wstring at = no_slash(picked);
    const wchar_t* name = name_of(kind);
    if (same(at, no_slash(vanacore)) || exists(at + L"\\VanaCore.exe")) {
        why = std::wstring(L"That's VanaCore's own folder. Pick the ") + name + L" folder you played with.";
        return false;
    }
    out.root = locate(kind, at);
    if (out.root.empty()) {
        const Kind other = kind == Kind::Ashita ? Kind::Windower : Kind::Ashita;
        if (!locate(other, at).empty())
            why = std::wstring(other == Kind::Ashita ? L"That's an " : L"That's a ") + name_of(other) + L" folder: use From " + name_of(other) + L" for it.";
        else
            why = kind == Kind::Ashita ? L"There's no Ashita there (Ashita-cli.exe, Ashita.dll, or its config\\boot and addons folders). Pick the folder Ashita is in."
                                       : L"There's no Windower there (Windower.exe, or its settings.xml, addons and plugins folders). Pick the folder Windower is in.";
        return false;
    }
    const std::wstring& root = out.root;
    for (const std::wstring& a : names_in(root + L"\\addons", true))
        if (!same(a, L"libs") && !names_in(root + L"\\addons\\" + a, false, L".lua").empty())
            out.addons.push_back(a);
    for (const std::wstring& p : names_in(root + L"\\plugins", false, L".dll"))
        if (kind == Kind::Windower || (!same(p, L"addons") && !same(p, L"sequencer")))
            out.plugins.push_back(p);
    // the boot script: Ashita's default.txt (else the one a config\boot\*.ini names), Windower's init.txt
    if (kind == Kind::Ashita) {
        if (exists(root + L"\\scripts\\default.txt"))
            out.script = root + L"\\scripts\\default.txt";
        for (const std::wstring& ini : names_in(root + L"\\config\\boot", false, L".ini")) {
            wchar_t script[MAX_PATH] = {};
            GetPrivateProfileStringW(L"ashita.boot", L"script", L"", script, MAX_PATH, (root + L"\\config\\boot\\" + ini + L".ini").c_str());
            if (out.script.empty() && script[0] != 0 && wcschr(script, L':') == nullptr && exists(root + L"\\scripts\\" + script))
                out.script = root + L"\\scripts\\" + script;
        }
        // its addons' and plugins' settings: config\addons, config\plugins, and a copied plugin's own config\<name>
        for (const std::wstring& c : names_in(root + L"\\config", true))
            if (same(c, L"addons") || same(c, L"plugins") ||
                std::any_of(out.plugins.begin(), out.plugins.end(), [&](const std::wstring& p) { return same(p, c); }))
                out.configs.push_back(c);
    } else if (exists(root + L"\\scripts\\init.txt")) {
        out.script = root + L"\\scripts\\init.txt";
    }
    std::vector<std::wstring> scripts;
    files_under(root + L"\\scripts", L"", scripts);
    for (const std::wstring& s : scripts)
        if (out.script.empty() || !same(root + L"\\scripts\\" + s, out.script))
            out.scripts.push_back(s);
    // its lines as boot.txt's
    if (kind == Kind::Windower)
        for (const std::string& p : windower_autoload(root))
            if (listed(out.plugins, p))
                out.lines.push_back("/plugin load " + p + " windower"), ++out.loads;
    std::string text;
    if (!out.script.empty() && read_file(out.script, text))
        for (const std::string& line : script_lines(text)) {
            if (kind == Kind::Ashita)
                ashita_line(line, out, out);
            else
                windower_line(line, out, out);
        }
    if (out.addons.empty() && out.plugins.empty() && out.lines.empty()) {
        why = std::wstring(L"That ") + name + L" folder has no addons, plugins or boot script to bring over.";
        return false;
    }
    return true;
}

Result run(const Found& found, const std::wstring& vanacore, const std::function<void(const std::wstring&, int, int)>& step)
{
    Result r;
    const std::wstring kind = found.kind == Kind::Ashita ? L"ashita" : L"windower";
    const int total = static_cast<int>(found.addons.size() + found.plugins.size() + found.configs.size()) + 2;
    int done = 0;
    for (const std::wstring& a : found.addons) {
        step(a, done++, total);
        if (copy_tree(found.root + L"\\addons\\" + a, vanacore + L"\\addons\\" + kind + L"\\" + a, r) > 0)
            ++r.addons;
    }
    for (const std::wstring& p : found.plugins) {
        step(p + L".dll", done++, total);
        if (copy_file(found.root + L"\\plugins\\" + p + L".dll", vanacore + L"\\plugins\\" + kind + L"\\" + p + L".dll", r))
            ++r.plugins;
    }
    for (const std::wstring& c : found.configs) {
        step(L"config\\" + c, done++, total);
        if (copy_tree(found.root + L"\\config\\" + c, vanacore + L"\\config\\" + c, r) > 0)
            ++r.configs;
    }
    step(L"scripts", done++, total);
    for (const std::wstring& s : found.scripts)
        if (!same(s, L"boot.txt") && copy_file(found.root + L"\\scripts\\" + s, vanacore + L"\\scripts\\" + s, r))
            ++r.scripts;
    step(L"boot.txt", done++, total);
    add_to_boot(found, vanacore, r);
    step(L"", done, total);
    return r;
}

} // namespace vcapp::migrate
