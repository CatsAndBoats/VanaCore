#pragma once

// TLS over a socket, through Windows' own Schannel: VanaCore.exe's login (launcher\login.cpp) and the lobby's data link
// and view relay in the game (core\loader_tls.h). Include winsock2.h before windows.h, as both do.

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <winsock2.h>
#include <windows.h>

#include <schannel.h>
#include <security.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")

namespace vanacore::tls {

inline std::wstring status_text(SECURITY_STATUS status)
{
    wchar_t text[16];
    swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(status));
    return text;
}

// One TLS connection, as a client. The server's certificate isn't checked: LandSandBoat's is self-signed.
// One thread at a time: reads and writes of one connection aren't safe from two threads at once.
class Client {
public:
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client()
    {
        if (have_context_)
            DeleteSecurityContext(&context_);
        if (have_credentials_)
            FreeCredentialsHandle(&credentials_);
        if (socket_ != INVALID_SOCKET)
            closesocket(socket_);
    }

    // Connects and completes the handshake; false with the reason in `error`. `protocols` are the TLS versions to offer
    // (SP_PROT_*_CLIENT), 0 for Windows' default. `timeout_ms` bounds each send and receive (0: none).
    bool connect(std::uint32_t address, std::uint16_t port, const std::wstring& host, DWORD protocols,
                 DWORD timeout_ms, std::wstring& error)
    {
        if (socket_ != INVALID_SOCKET) {
            error = L"already connected";
            return false;
        }
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            error = L"couldn't make a socket (winsock error " + std::to_wstring(WSAGetLastError()) + L")";
            return false;
        }
        set_timeout(timeout_ms);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = address;
        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
            error = L"couldn't connect (winsock error " + std::to_wstring(WSAGetLastError()) + L")";
            return false;
        }
        connected_ = true;

        SCHANNEL_CRED settings{};
        settings.dwVersion = SCHANNEL_CRED_VERSION;
        settings.grbitEnabledProtocols = protocols;
        settings.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
        const SECURITY_STATUS status =
            AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
                                      &settings, nullptr, nullptr, &credentials_, nullptr);
        if (status != SEC_E_OK) {
            error = L"Windows' TLS wouldn't start (" + status_text(status) + L")";
            return false;
        }
        have_credentials_ = true;
        host_ = host;
        return handshake({}, error);
    }

    // True once the socket connected (a failed connect() got as far as TLS)
    bool connected() const { return connected_; }

    SOCKET socket_handle() const { return socket_; }

    // Each send and receive gives up after this long (0: waits for ever)
    void set_timeout(DWORD timeout_ms)
    {
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    }

    // Ends a read that waits in another thread (it then fails)
    void shut_down() const
    {
        if (socket_ != INVALID_SOCKET)
            shutdown(socket_, SD_BOTH);
    }

    // Sends `size` bytes, in as many records as they take
    bool write(const char* data, size_t size)
    {
        while (size > 0) {
            const size_t part = size < sizes_.cbMaximumMessage ? size : sizes_.cbMaximumMessage;
            std::vector<char> record(sizes_.cbHeader + part + sizes_.cbTrailer);
            std::memcpy(record.data() + sizes_.cbHeader, data, part);
            SecBuffer buffers[4] = {
                {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                {static_cast<unsigned long>(part), SECBUFFER_DATA, record.data() + sizes_.cbHeader},
                {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes_.cbHeader + part},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
            if (EncryptMessage(&context_, 0, &description, 0) != SEC_E_OK ||
                !send_all(record.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer))
                return false;
            data += part;
            size -= part;
        }
        return true;
    }

    bool write(const std::string& message) { return write(message.data(), message.size()); }

    // Appends to `text` what the server has sent: every whole record already here, else one receive's worth (which
    // can be none, a partial record or a session ticket). False once the connection ends or fails (the reason in
    // `error`, empty for a plain close).
    bool read_some(std::string& text, std::wstring& error)
    {
        const size_t had = text.size();
        if (!decrypt(text, error))
            return false;
        if (text.size() > had)
            return true;
        if (!receive())
            return false;
        return decrypt(text, error);
    }

    // Reads until `done` says the text read so far is complete; false if the connection ends or times out first, or
    // TLS fails (the reason in `error`).
    template <typename Done>
    bool read(std::string& text, Done done, std::wstring& error)
    {
        while (!done(text))
            if (!read_some(text, error))
                break;
        return done(text);
    }

private:
    bool send_all(const char* data, size_t size)
    {
        while (size > 0) {
            const int sent = ::send(socket_, data, static_cast<int>(size), 0);
            if (sent <= 0)
                return false;
            data += sent;
            size -= static_cast<size_t>(sent);
        }
        return true;
    }

    // Appends what the socket has to incoming_; false when it's closed, timed out or failed.
    bool receive()
    {
        char chunk[16384];
        const int received = recv(socket_, chunk, sizeof(chunk), 0);
        if (received <= 0)
            return false;
        incoming_.insert(incoming_.end(), chunk, chunk + received);
        return true;
    }

    // Decrypts every whole record in incoming_ into `text`; false if the server closed the session or TLS failed.
    bool decrypt(std::string& text, std::wstring& error)
    {
        while (!incoming_.empty()) {
            SecBuffer buffers[4] = {
                {static_cast<unsigned long>(incoming_.size()), SECBUFFER_DATA, incoming_.data()},
                {0, SECBUFFER_EMPTY, nullptr},
                {0, SECBUFFER_EMPTY, nullptr},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS status = DecryptMessage(&context_, &description, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE)
                return true;
            if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE && status != SEC_I_CONTEXT_EXPIRED) {
                error = L"TLS failed reading (" + status_text(status) + L")";
                return false;
            }
            std::vector<char> extra;
            for (const SecBuffer& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer > 0)
                    text.append(static_cast<const char*>(buffer.pvBuffer), buffer.cbBuffer);
                else if (buffer.BufferType == SECBUFFER_EXTRA && buffer.cbBuffer > 0)
                    extra.assign(static_cast<const char*>(buffer.pvBuffer),
                                 static_cast<const char*>(buffer.pvBuffer) + buffer.cbBuffer);
            }
            incoming_ = std::move(extra);
            if (status == SEC_I_CONTEXT_EXPIRED)
                return false; // the server closed the TLS session
            // TLS 1.3's messages after the handshake (session tickets) come as a renegotiation
            if (status == SEC_I_RENEGOTIATE) {
                std::vector<char> input = std::move(incoming_);
                incoming_.clear();
                if (!handshake(std::move(input), error))
                    return false;
            }
        }
        return true;
    }

    // Runs InitializeSecurityContext until the handshake is done, starting from `input` (tokens already read; empty to
    // start a new handshake).
    bool handshake(std::vector<char> input, std::wstring& error)
    {
        constexpr DWORD kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                 ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                                 ISC_REQ_MANUAL_CRED_VALIDATION;
        incoming_ = std::move(input);
        bool first = !have_context_;
        for (;;) {
            if (!first && incoming_.empty() && !receive()) {
                error = L"the connection closed during the TLS handshake";
                return false;
            }
            SecBuffer in[2] = {
                {static_cast<unsigned long>(incoming_.size()), SECBUFFER_TOKEN, incoming_.empty() ? nullptr : incoming_.data()},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc in_description{SECBUFFER_VERSION, 2, in};
            SecBuffer out[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc out_description{SECBUFFER_VERSION, 1, out};
            DWORD got = 0;
            const SECURITY_STATUS status = InitializeSecurityContextW(
                &credentials_, first ? nullptr : &context_, const_cast<wchar_t*>(host_.c_str()), kFlags, 0, 0,
                first ? nullptr : &in_description, 0, &context_, &out_description, &got, nullptr);
            if (first) {
                if (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK)
                    have_context_ = true;
                first = false;
            }
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!receive()) {
                    error = L"the connection closed during the TLS handshake";
                    return false;
                }
                continue;
            }
            if (out[0].cbBuffer > 0 && out[0].pvBuffer != nullptr) {
                const bool sent = send_all(static_cast<const char*>(out[0].pvBuffer), out[0].cbBuffer);
                FreeContextBuffer(out[0].pvBuffer);
                if (!sent) {
                    error = L"couldn't send the TLS handshake";
                    return false;
                }
            }
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
                error = L"the TLS handshake failed (" + status_text(status) + L")";
                return false;
            }
            // what the server sent past the handshake's last token stays for the next read
            if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0)
                incoming_.erase(incoming_.begin(), incoming_.end() - in[1].cbBuffer);
            else
                incoming_.clear();
            if (status == SEC_E_OK)
                break;
        }
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) {
            error = L"TLS gave no record sizes";
            return false;
        }
        return true;
    }

    SOCKET socket_ = INVALID_SOCKET;
    bool connected_ = false;
    CredHandle credentials_{};
    CtxtHandle context_{};
    bool have_credentials_ = false;
    bool have_context_ = false;
    std::wstring host_;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<char> incoming_; // encrypted bytes not decrypted yet
};

// Connects with Windows' default TLS versions, then once more asking for 1.2 if that handshake failed: Windows 7 offers
// only 1.0 unless asked for more (1.1 and 1.2 are off by default for clients there) and the server takes 1.2 and up,
// refusing it (0x80090302). Null, with the reason in `error`, if neither connects.
inline std::unique_ptr<Client> connect(std::uint32_t address, std::uint16_t port, const std::wstring& host,
                                       DWORD timeout_ms, std::wstring& error)
{
    auto client = std::make_unique<Client>();
    if (client->connect(address, port, host, 0, timeout_ms, error))
        return client;
    if (!client->connected())
        return nullptr;
    const std::wstring first = error;
    error.clear();
    client = std::make_unique<Client>();
    if (client->connect(address, port, host, SP_PROT_TLS1_2_CLIENT, timeout_ms, error))
        return client;
    error = first + L"; asking for TLS 1.2, " + error;
    return nullptr;
}

} // namespace vanacore::tls
