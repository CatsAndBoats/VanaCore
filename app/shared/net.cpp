#include "net.h"

#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")

namespace vcapp::net {

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

std::wstring wide(const std::string& s)
{
    std::wstring w(s.size(), L'\0');
    w.resize(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()))));
    return w;
}

} // namespace vcapp::net
