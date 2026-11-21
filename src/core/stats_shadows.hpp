/* The shadows' record and the settings beside a stats report, what stats.cpp includes after stats_report.hpp (in its anonymous
 * namespace, after Recording and g_logs, whose names it uses):
 *   - shadows_start / shadows_stop: while a recording (Record Stats Report) runs, every frame's state of the sun's shadows into
 *     logs\stats-shadows-<date>-<time>.csv (Direct3D 12: gfx_sun_record.h), at most kShadowsMaxMb; the report takes it into
 *     its Shadows section and deletes it;
 *   - settings_text: the report's Settings section, every setting VanaCore has in force, the defaults
 *     too - each key vanacore.example.ini lists (its default where the player's file hasn't the key), then any other key the
 *     file sets - with the account's name, password and tokens left out.
 * Why: a strobe on one player's PC (2026-11-05) couldn't be found from the stats files, which hold no shadow state, nor from
 * the flicker watch's 1200 frames, nor without knowing every setting the player had. Its headers (version.hpp, ini.hpp,
 * settings.hpp, <cwctype>) are stats.cpp's, at the top: this is read inside its namespace. */

constexpr unsigned kShadowsMaxMb = 40; // some 25 minutes at 60 frames a second

// a path as fopen takes it (the ANSI code page)
std::string acp_path(const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void shadows_start()
{
    if (g_logs.empty())
        return;
    CreateDirectoryW(g_logs.c_str(), nullptr);
    const std::wstring name = L"stats-shadows-" + log::file_stamp() + L".csv";
    if (vanacore_gfx_sun_record(acp_path(g_logs + name).c_str(), kShadowsMaxMb) != 0)
        g_recording.shadows_file = name;
}

void shadows_stop()
{
    if (g_recording.shadows_file.empty())
        return;
    vanacore_gfx_sun_record(nullptr, 0);
    g_recording.shadows_done = g_recording.shadows_file; // (for the report the stop writes)
    g_recording.shadows_file.clear();
}

std::wstring lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(towlower(c));
    return s;
}

// A key whose value isn't for a report: the account's name and password, tokens, anything secret. Three ways in, any one
// enough: [server] keeps only its two known keys (the address, the login version), whatever else it holds is left out; a
// key with a secret's word anywhere in it; a key with a short one (user, pass...) as a part of its name (parts split at
// . _ -). And scrub_secrets, after, takes the values themselves out of the whole report, wherever they are.
bool secret_key(const std::wstring& section, const std::wstring& key)
{
    const std::wstring s = lower(section), k = lower(key);
    if (s == L"server" && k != L"server" && k != L"loginversion")
        return true;
    for (const wchar_t* word : {L"password", L"passwd", L"passphrase", L"token", L"secret", L"credential", L"email", L"apikey", L"api_key",
                                L"username", L"cookie", L"bearer", L"private"})
        if (k.find(word) != std::wstring::npos)
            return true;
    size_t from = 0;
    while (from <= k.size()) {
        const size_t end = std::min(k.find_first_of(L"._-", from), k.size());
        const std::wstring part = k.substr(from, end - from);
        for (const wchar_t* word : {L"user", L"pass", L"pwd", L"pw", L"account", L"otp", L"totp", L"auth", L"key", L"pin", L"mail", L"session"})
            if (part == word)
                return true;
        from = end + 1;
    }
    return false;
}

// The keys whose values are surely the login's or a token (not every key secret_key leaves out: a harmless number under
// a "key" or "pin" name, scrubbed everywhere, would take numbers out of the frames' tables)
bool login_secret(const std::wstring& section, const std::wstring& key)
{
    const std::wstring s = lower(section), k = lower(key);
    if (s == L"server")
        return k != L"server" && k != L"loginversion";
    for (const wchar_t* word : {L"password", L"passwd", L"passphrase", L"token", L"secret", L"credential", L"username", L"apikey", L"api_key"})
        if (k.find(word) != std::wstring::npos)
            return true;
    return k == L"user" || k == L"pass" || k == L"pwd";
}

// What scrub_secrets takes out: each login_secret value in VanaCore's settings files, and what the command line gave
// --user and --password (VanaCore.exe's own login), at least 4 letters (shorter would hit ordinary text)
void secret_values(std::vector<std::string>& out)
{
    const std::wstring dir = g_logs.size() > 5 ? g_logs.substr(0, g_logs.size() - 5) : g_logs;
    for (const std::wstring& path : {settings::file(), settings::copy(), dir + L"vanacore.ini"}) {
        std::vector<ini::Value> values;
        if (!path.empty() && ini::load(path, values))
            for (const ini::Value& v : values)
                if (login_secret(v.section, v.key))
                    out.push_back(narrow(v.value));
    }
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i) {
            const std::wstring a = lower(argv[i]);
            if (a == L"--user" || a == L"--username" || a == L"--password" || a == L"--pass")
                out.push_back(narrow(argv[i + 1]));
        }
        LocalFree(argv);
    }
}

// The last check on the whole report before it's written: every secret value found anywhere in it (a key the rules above
// missed, a path, a line an addon logged) replaced, and the Windows user's folder (its name) in a path made %USERPROFILE%
std::string scrub_secrets(std::string text)
{
    std::vector<std::string> secrets;
    secret_values(secrets);
    std::sort(secrets.begin(), secrets.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const std::string& s : secrets) {
        if (s.size() < 4 || (s.size() < 8 && s.find_first_not_of("0123456789.") == std::string::npos))
            continue; // (too short, or a number the tables could hold by chance; the settings section has it left out by its key)
        for (size_t at = text.find(s); at != std::string::npos; at = text.find(s, at + 10))
            text.replace(at, s.size(), "(left out)");
    }
    wchar_t profile[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n > 3 && n < MAX_PATH) {
        const std::string p = narrow(profile);
        std::string lowered = text;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string lp = p;
        std::transform(lp.begin(), lp.end(), lp.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (size_t at = lowered.rfind(lp); at != std::string::npos; at = at == 0 ? std::string::npos : lowered.rfind(lp, at - 1)) {
            text.replace(at, p.size(), "%USERPROFILE%");
            if (at == 0)
                break;
        }
    }
    return text;
}

struct Known {
    std::wstring section, key, def;
};

// vanacore.example.ini's keys and defaults: "[section]" lines, and ";key=value" (the default, commented out) or "key=value"
// lines; a prose comment ("; text") is neither
std::vector<Known> example_keys(const std::wstring& path)
{
    std::vector<Known> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return out;
    std::string section;
    char line[4096];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' '))
            l.pop_back();
        if (l.rfind("\xEF\xBB\xBF", 0) == 0)
            l.erase(0, 3);
        if (l.size() > 2 && l[0] == '[' && l.back() == ']') {
            section = l.substr(1, l.size() - 2);
            continue;
        }
        const size_t from = !l.empty() && l[0] == ';' ? 1 : 0, eq = l.find('=');
        if (eq == std::string::npos || eq <= from || l[from] == ' ' || section.empty())
            continue;
        const std::string key = l.substr(from, eq - from);
        if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos)
            continue;
        const auto wide = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
        out.push_back({wide(section), wide(key), wide(l.substr(eq + 1))});
    }
    std::fclose(f);
    return out;
}

std::string settings_line(const std::wstring& section, const std::wstring& key, const std::wstring& value, const wchar_t* note)
{
    const std::wstring shown = secret_key(section, key) ? L"(left out)" : value;
    return narrow(key + L"=" + shown) + (note != nullptr ? "    ; " + narrow(note) : std::string());
}

std::string settings_text()
{
    std::vector<ini::Value> set;
    ini::load(settings::copy().empty() ? settings::file() : settings::copy(), set);
    const std::wstring dir = g_logs.size() > 5 ? g_logs.substr(0, g_logs.size() - 5) : g_logs; // (g_logs ends in "logs\")
    const std::vector<Known> known = example_keys(dir + L"vanacore.example.ini");
    std::string text = "; VanaCore " + narrow(vanacore::kVersion) + ": every setting in force when this report was saved, from " +
                       narrow(settings::file()) + " (the game's copy: " + narrow(settings::copy()) + "). Each key vanacore.example.ini " +
                       "lists, set (the file's value) or its default, then the keys the file sets that it doesn't list. The account's " +
                       "name, password and tokens are left out.\n";
    if (known.empty())
        text += "; vanacore.example.ini isn't beside VanaCore.exe: only the keys the file sets are here\n";
    std::vector<bool> used(set.size(), false);
    std::wstring at;
    const auto section = [&](const std::wstring& s) {
        if (lower(s) != lower(at))
            text += "\n[" + narrow(s) + "]\n", at = s;
    };
    for (const Known& k : known) {
        section(k.section);
        const wchar_t* note = L"default";
        std::wstring value = k.def;
        for (size_t i = 0; i < set.size(); ++i)
            if (!used[i] && lower(set[i].section) == lower(k.section) && lower(set[i].key) == lower(k.key))
                used[i] = true, value = set[i].value, note = set[i].value == k.def ? L"set, the default" : L"set";
        text += settings_line(k.section, k.key, value, note) + (note[0] == L's' && value != k.def ? " (default " + narrow(k.def) + ")" : "") + "\n";
    }
    for (size_t i = 0; i < set.size(); ++i)
        if (!used[i])
            section(set[i].section), text += settings_line(set[i].section, set[i].key, set[i].value, L"set, not in the example") + "\n";
    return text;
}
