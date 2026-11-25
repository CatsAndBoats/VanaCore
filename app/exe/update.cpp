#include "update.h"

#include <windows.h>

#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wintrust.h>

#include <cstdint>
#include <vector>

#include "app_api.h"
#include "json.h"
#include "version.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")

namespace vcapp {

namespace {

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

std::wstring wide(const std::string& s)
{
    std::wstring w(s.size(), L'\0');
    w.resize(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()))));
    return w;
}

// ---- HTTPS (WinHTTP: Windows' own, nothing to ship)

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
    Http()
    {
        const wchar_t* agent = L"VanaCoreApp/" L"" VCAPP_EXE_VERSION_STR;
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

// ---- Checks

std::string sha256_hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
        static const char kHex[] = "0123456789abcdef";
        for (unsigned char b : digest)
            hex += kHex[b >> 4], hex += kHex[b & 15];
    }
    if (hash != nullptr)
        BCryptDestroyHash(hash);
    if (alg != nullptr)
        BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// The file's Authenticode signature holds (no revocation check: it'd need the network, and a long wait when it's down)
bool signature_valid(const std::wstring& path)
{
    WINTRUST_FILE_INFO file{sizeof file};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof data};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return result == ERROR_SUCCESS;
}

// Who signed the file (its certificate's name), "" when it isn't signed
std::wstring signer_of(const std::wstring& path)
{
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0, content = 0, format = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                          &encoding, &content, &format, &store, &message, nullptr))
        return {};
    std::wstring name;
    DWORD size = 0;
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) && size > 0) {
        std::vector<BYTE> buffer(size);
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &size)) {
            const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(buffer.data());
            CERT_INFO info{};
            info.Issuer = signer->Issuer;
            info.SerialNumber = signer->SerialNumber;
            if (PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT,
                                                                 &info, nullptr)) {
                wchar_t text[512] = {};
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, text, 512);
                name = text;
                CertFreeCertificateContext(cert);
            }
        }
    }
    CryptMsgClose(message);
    CertCloseStore(store, 0);
    return name;
}

std::wstring self_path()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    return path;
}

bool write_file(const std::wstring& path, const std::string& data)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size() &&
                    FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
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
    Http http;
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
