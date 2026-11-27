#pragma once
// HTTPS downloads and file checks: VanaCoreApp.exe's update of the DLL (exe\update.cpp) and the DLL's update of VanaCore
// (dll\home_update.cpp). WinHTTP and WinTrust: Windows' own, nothing to ship.

#include <windows.h>

#include <winhttp.h>

#include <cstddef>
#include <functional>
#include <string>

namespace vcapp::net {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle()
    {
        if (h != nullptr)
            WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

class Http {
public:
    explicit Http(const wchar_t* agent)
    {
        // the system's proxy settings (Windows 8.1 on), else WinHTTP's own
        session_.h = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)
            session_.h = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)
            return;
        WinHttpSetTimeouts(session_.h, 5000, 5000, 8000, 15000); // resolve, connect, send, each read
        DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000; // 1.2 and 1.3 (Windows 7 has 1.2 only off by default)
        if (!WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) {
            tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session_.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
        }
    }

    // `url` (https only) into `body`, following redirects (GitHub's downloads go to its storage); false on any failure,
    // past `most` bytes, or when `going` says stop. `going(got, total)`: total 0 when it isn't known.
    bool get(const std::wstring& url, const wchar_t* accept, std::size_t most, std::string& body,
             const std::function<bool(std::size_t, std::size_t)>& going)
    {
        body.clear();
        if (!session_)
            return false;
        wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
        URL_COMPONENTS uc{sizeof uc};
        uc.lpszHostName = host, uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path, uc.dwUrlPathLength = 2048;
        uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
            return false;
        Handle connect(WinHttpConnect(session_.h, host, uc.nPort, 0));
        const std::wstring target = std::wstring(path) + extra;
        Handle request(connect ? WinHttpOpenRequest(connect.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                               : nullptr);
        if (!request)
            return false;
        const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.h, nullptr))
            return false;
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200)
            return false;
        DWORD total = 0;
        size = sizeof total;
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total,
                                 &size, WINHTTP_NO_HEADER_INDEX))
            total = 0;
        if (total > most)
            return false;
        char buffer[64 * 1024];
        while (true) {
            DWORD read = 0;
            if (!WinHttpReadData(request.h, buffer, sizeof buffer, &read))
                return false;
            if (read == 0)
                return total == 0 || body.size() == total;
            body.append(buffer, read);
            if (body.size() > most || (going && !going(body.size(), total)))
                return false;
        }
    }

private:
    Handle session_;
};

// SHA-256 of `data` as lowercase hex ("" when Windows can't)
std::string sha256_hex(const std::string& data);
// The file's Authenticode signature holds (no revocation check: it'd need the network, and a long wait when it's down)
bool signature_valid(const std::wstring& path);
// Who signed the file (its certificate's name), "" when it isn't signed
std::wstring signer_of(const std::wstring& path);
// `data` written to `path` (made or replaced) and flushed: false when it couldn't be
bool write_file(const std::wstring& path, const std::string& data);
// UTF-8 to UTF-16
std::wstring wide(const std::string& s);

} // namespace vcapp::net
