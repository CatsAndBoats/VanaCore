// vanacore:// links: a package on the web, downloaded here (rundll32, outside the game) and then asked about as a
// double-clicked .vcaddon is. See addonpkg_link.hpp.

#include "addonpkg_link.hpp"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#include "addonpkg.hpp"

namespace vanacore::addonpkg {

namespace {

constexpr std::uint64_t kMaxDownload = 260ull * 1024 * 1024; // a package is at most 256 MB

bool starts_with_i(const std::wstring& text, const wchar_t* prefix)
{
    const std::size_t n = wcslen(prefix);
    return text.size() >= n && _wcsnicmp(text.c_str(), prefix, n) == 0;
}

std::wstring size_text(std::uint64_t bytes)
{
    wchar_t text[32];
    if (bytes >= 1024 * 1024)
        swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        swprintf_s(text, L"%llu KB", (bytes + 1023) / 1024);
    return text;
}

int hex_digit(wchar_t c)
{
    if (c >= L'0' && c <= L'9')
        return c - L'0';
    if (c >= L'a' && c <= L'f')
        return c - L'a' + 10;
    if (c >= L'A' && c <= L'F')
        return c - L'A' + 10;
    return -1;
}

// %XX as UTF-8 bytes; the rest as it is
std::wstring percent_decode(const std::wstring& text)
{
    std::wstring out;
    std::string bytes;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i + 2 < text.size() && text[i] == L'%' && hex_digit(text[i + 1]) >= 0 && hex_digit(text[i + 2]) >= 0) {
            bytes += static_cast<char>(hex_digit(text[i + 1]) * 16 + hex_digit(text[i + 2]));
            i += 2;
            continue;
        }
        out += wide(bytes);
        bytes.clear();
        if (i < text.size())
            out += text[i];
    }
    return out;
}

// WinHTTP, loaded here and never by the game
struct Http {
    HMODULE module = nullptr;
    decltype(&WinHttpOpen) open = nullptr;
    decltype(&WinHttpCrackUrl) crack = nullptr;
    decltype(&WinHttpConnect) connect = nullptr;
    decltype(&WinHttpOpenRequest) open_request = nullptr;
    decltype(&WinHttpSetOption) set_option = nullptr;
    decltype(&WinHttpSetTimeouts) set_timeouts = nullptr;
    decltype(&WinHttpSendRequest) send = nullptr;
    decltype(&WinHttpReceiveResponse) receive = nullptr;
    decltype(&WinHttpQueryHeaders) query = nullptr;
    decltype(&WinHttpReadData) read = nullptr;
    decltype(&WinHttpCloseHandle) close = nullptr;
    HINTERNET session = nullptr, connection = nullptr, request = nullptr;

    Http() = default;
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;
    ~Http()
    {
        for (HINTERNET h : {request, connection, session})
            if (h != nullptr)
                close(h);
        if (module != nullptr)
            FreeLibrary(module);
    }
    template <class T> bool bind(const char* name, T& f)
    {
        f = reinterpret_cast<T>(GetProcAddress(module, name));
        return f != nullptr;
    }
    bool load()
    {
        module = LoadLibraryW(L"winhttp.dll");
        return module != nullptr && bind("WinHttpOpen", open) && bind("WinHttpCrackUrl", crack) &&
               bind("WinHttpConnect", connect) && bind("WinHttpOpenRequest", open_request) &&
               bind("WinHttpSetOption", set_option) && bind("WinHttpSetTimeouts", set_timeouts) &&
               bind("WinHttpSendRequest", send) && bind("WinHttpReceiveResponse", receive) &&
               bind("WinHttpQueryHeaders", query) && bind("WinHttpReadData", read) && bind("WinHttpCloseHandle", close);
    }
    DWORD number(DWORD what)
    {
        DWORD value = 0, size = sizeof value;
        if (!query(request, what | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &value, &size, WINHTTP_NO_HEADER_INDEX))
            return 0;
        return value;
    }
};

std::wstring http_error(DWORD error)
{
    switch (error) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return L"The site's name can't be found. Is this PC online?";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return L"The site can't be reached";
    case ERROR_WINHTTP_TIMEOUT:
        return L"The site stopped answering";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return L"The site's certificate isn't one Windows trusts";
    case ERROR_WINHTTP_REDIRECT_FAILED:
        return L"The site sent it on to a page that isn't https";
    default:
        return L"It couldn't be downloaded (WinHTTP error " + std::to_wstring(error) + L")";
    }
}

// GET `url` over https (TLS 1.2 and up; redirects followed, never from https to http); the request ready to read, or why
// not
std::wstring http_get(Http& h, const std::wstring& url)
{
    if (!h.load())
        return L"Windows' WinHTTP can't be loaded";
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    u.dwHostNameLength = u.dwUrlPathLength = u.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!h.crack(url.c_str(), static_cast<DWORD>(url.size()), 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS)
        return L"The link's address isn't an https one";
    const std::wstring host(u.lpszHostName, u.dwHostNameLength);
    const std::wstring path = std::wstring(u.lpszUrlPath, u.dwUrlPathLength) + std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
    h.session = h.open(L"VanaCore", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (Windows 8.1) */, nullptr, nullptr, 0);
    if (h.session == nullptr)
        h.session = h.open(L"VanaCore", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (h.session == nullptr)
        return http_error(GetLastError());
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!h.set_option(h.session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        h.set_option(h.session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH; // never the player's Windows login
    h.set_option(h.session, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
    h.set_timeouts(h.session, 15000, 15000, 30000, 30000);
    h.connection = h.connect(h.session, host.c_str(), u.nPort, 0);
    if (h.connection != nullptr)
        h.request = h.open_request(h.connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (h.request == nullptr || !h.send(h.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !h.receive(h.request, nullptr))
        return http_error(GetLastError());
    const DWORD status = h.number(WINHTTP_QUERY_STATUS_CODE);
    if (status == 404 || status == 410)
        return L"There's no file at that address any more (" + std::to_wstring(status) + L")";
    if (status != 200)
        return L"The site answered " + std::to_wstring(status) + L" instead of the file";
    return {};
}

} // namespace

bool is_link(const std::wstring& text)
{
    return starts_with_i(text, L"vanacore:");
}

// The package a link names: vanacore://install/<https URL>, the URL as it is or percent-encoded, or
// vanacore://install?url=<percent-encoded URL>. Empty when it names none (only https, so where it came from is sure).
std::wstring link_target(const std::wstring& link)
{
    std::wstring rest = link.substr(std::wstring(L"vanacore:").size());
    while (!rest.empty() && rest.front() == L'/')
        rest.erase(0, 1);
    if (!starts_with_i(rest, L"install"))
        return {};
    rest.erase(0, 7);
    std::wstring url;
    if (starts_with_i(rest, L"?url="))
        url = percent_decode(rest.substr(5));
    else if (!rest.empty() && rest.front() == L'/')
        url = rest.substr(1);
    if (starts_with_i(url, L"https%3a"))
        url = percent_decode(url);
    if (starts_with_i(url, L"https:/") && !starts_with_i(url, L"https://"))
        url.insert(7, L"/"); // a browser that folded the two slashes into one
    if (!starts_with_i(url, L"https://") || url.size() <= 8 || url.size() > 2048)
        return {};
    std::wstring out;
    for (wchar_t c : url) {
        if (c == L' ')
            out += L"%20"; // decoded from the link: encoded again for the request
        else if (c < L' ' || c == L'"')
            return {};
        else
            out += c;
    }
    return out;
}

// The URL's file name, kept to what's safe in a file name (package.vcaddon when it has none)
std::wstring download_name(const std::wstring& url)
{
    std::wstring path = url.substr(0, url.find_first_of(L"?#"));
    path = percent_decode(path.substr(path.find_last_of(L'/') + 1));
    std::wstring name;
    for (wchar_t c : path)
        if (iswalnum(c) || c == L'.' || c == L'-' || c == L'_')
            name += c;
    while (!name.empty() && name.front() == L'.')
        name.erase(0, 1);
    if (name.size() > 80)
        name.erase(0, name.size() - 80);
    return name.empty() ? L"package.vcaddon" : name;
}

// Downloads `url` to `path`, at most kMaxDownload, `progress(got, total)` as it comes (total 0 when the site doesn't
// say), and marks it as downloaded from `url` (Windows' Zone.Identifier: what the card's From shows). Empty when it's all
// there, else why not (and no file).
std::wstring download(const std::wstring& url, const std::wstring& path, const std::atomic<bool>& cancel,
                      const std::function<void(std::uint64_t, std::uint64_t)>& progress)
{
    Http h;
    std::wstring why = http_get(h, url);
    if (!why.empty())
        return why;
    const std::uint64_t total = h.number(WINHTTP_QUERY_CONTENT_LENGTH);
    if (total > kMaxDownload)
        return L"It's " + size_text(total) + L", more than a package can be";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return L"It can't be saved there (error " + std::to_wstring(GetLastError()) + L")";
    std::vector<char> buffer(64 * 1024);
    std::uint64_t got = 0;
    for (;;) {
        DWORD n = 0, written = 0;
        if (cancel) {
            why = L"Cancelled";
            break;
        }
        if (!h.read(h.request, buffer.data(), static_cast<DWORD>(buffer.size()), &n)) {
            why = http_error(GetLastError());
            break;
        }
        if (n == 0)
            break;
        got += n;
        if (got > kMaxDownload) {
            why = L"It's more than a package can be";
            break;
        }
        if (!WriteFile(file, buffer.data(), n, &written, nullptr) || written != n) {
            why = L"It can't be saved (error " + std::to_wstring(GetLastError()) + L")";
            break;
        }
        progress(got, total);
    }
    CloseHandle(file);
    if (why.empty() && total > 0 && got != total)
        why = L"The download stopped part way";
    if (!why.empty()) {
        DeleteFileW(path.c_str());
        return why;
    }
    const std::string zone = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=" + utf8(url) + "\r\n";
    HANDLE mark = CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (mark != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(mark, zone.data(), static_cast<DWORD>(zone.size()), &written, nullptr);
        CloseHandle(mark);
    }
    return {};
}

// %TEMP%\VanaCore\<this process>\, made: where a link's package waits while it's asked about
std::wstring download_dir()
{
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    std::wstring dir = std::wstring(temp) + L"VanaCore";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

} // namespace vanacore::addonpkg
