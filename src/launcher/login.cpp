#include "login.hpp"

#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <schannel.h>
#include <security.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")

namespace vanacore::login {
namespace {

constexpr int kLoginCommand = 0x10;
constexpr DWORD kTimeoutMs = 15000;

// The server's result codes (a login's).
enum : int {
    kResultFail = 0x00, // no such account, or it's banned
    kResultSuccess = 0x01,
    kResultWrongPassword = 0x02,
    kResultAlreadyLoggedIn = 0x0A,
    kResultVersionUnsupported = 0x0B,
};

std::string utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::wstring hex(unsigned long value)
{
    wchar_t text[16];
    swprintf_s(text, L"0x%08lX", value);
    return text;
}

// --- JSON: just what the login's request and reply need ------------------------------------------

std::string json_string(const std::string& text)
{
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                sprintf_s(escaped, "\\u%04x", static_cast<unsigned char>(c));
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value* member(std::string_view name) const
    {
        for (const auto& [key, value] : members)
            if (key == name)
                return &value;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parse(Value& out)
    {
        if (!value(out, 0))
            return false;
        space();
        return at_ == text_.size();
    }

private:
    void space()
    {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r'))
            ++at_;
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(at_, word.size()) != word)
            return false;
        at_ += word.size();
        return true;
    }

    static void append_utf8(std::string& out, unsigned long code)
    {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(unsigned long& code)
    {
        if (at_ + 4 > text_.size())
            return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            code <<= 4;
            if (c >= '0' && c <= '9')
                code |= static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f')
                code |= static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                code |= static_cast<unsigned long>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool string(std::string& out)
    {
        if (at_ >= text_.size() || text_[at_] != '"')
            return false;
        ++at_;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size())
                return false;
            const char escaped = text_[at_++];
            switch (escaped) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned long code = 0;
                if (!hex4(code))
                    return false;
                // a surrogate pair's second half
                if (code >= 0xD800 && code <= 0xDBFF && literal("\\u")) {
                    unsigned long low = 0;
                    if (!hex4(low))
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool value(Value& out, int depth)
    {
        if (depth > 32)
            return false;
        space();
        if (at_ >= text_.size())
            return false;
        const char c = text_[at_];
        if (c == '{') {
            ++at_;
            out.type = Value::Type::Object;
            space();
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            for (;;) {
                space();
                std::string key;
                if (!string(key))
                    return false;
                space();
                if (at_ >= text_.size() || text_[at_++] != ':')
                    return false;
                Value member;
                if (!value(member, depth + 1))
                    return false;
                out.members.emplace_back(std::move(key), std::move(member));
                space();
                if (at_ >= text_.size())
                    return false;
                if (text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (text_[at_++] != '}')
                    return false;
                return true;
            }
        }
        if (c == '[') {
            ++at_;
            out.type = Value::Type::Array;
            space();
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            for (;;) {
                Value item;
                if (!value(item, depth + 1))
                    return false;
                out.items.push_back(std::move(item));
                space();
                if (at_ >= text_.size())
                    return false;
                if (text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (text_[at_++] != ']')
                    return false;
                return true;
            }
        }
        if (c == '"') {
            out.type = Value::Type::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.type = Value::Type::Bool;
            out.number = 1;
            return true;
        }
        if (literal("false")) {
            out.type = Value::Type::Bool;
            return true;
        }
        if (literal("null")) {
            out.type = Value::Type::Null;
            return true;
        }
        // a number
        const size_t start = at_;
        while (at_ < text_.size() && std::strchr("+-0123456789.eE", text_[at_]) != nullptr)
            ++at_;
        if (at_ == start)
            return false;
        out.type = Value::Type::Number;
        out.number = std::strtod(std::string(text_.substr(start, at_ - start)).c_str(), nullptr);
        return true;
    }

    std::string_view text_;
    size_t at_ = 0;
};

// True once `text` holds a whole JSON object: its braces close, outside strings.
bool whole_object(std::string_view text)
{
    int depth = 0;
    bool in_string = false, escaped = false, opened = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '{')
            ++depth, opened = true;
        else if (c == '}' && --depth == 0 && opened)
            return true;
    }
    return false;
}

// --- TLS over a socket, through Windows' own Schannel --------------------------------------------

class Tls {
public:
    ~Tls()
    {
        if (have_context_)
            DeleteSecurityContext(&context_);
        if (have_credentials_)
            FreeCredentialsHandle(&credentials_);
        if (socket_ != INVALID_SOCKET)
            closesocket(socket_);
    }

    // Connects and completes the handshake; false with the reason in `error`. `protocols` are the TLS
    // versions to offer (SP_PROT_*_CLIENT), 0 for Windows' default.
    bool connect(std::uint32_t address, std::uint16_t port, const std::wstring& host, DWORD protocols,
                 std::wstring& error)
    {
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            error = L"couldn't make a socket (winsock error " + std::to_wstring(WSAGetLastError()) + L")";
            return false;
        }
        const DWORD timeout = kTimeoutMs;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = address;
        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
            error = L"couldn't connect (winsock error " + std::to_wstring(WSAGetLastError()) + L")";
            return false;
        }
        connected_ = true;

        // The server's certificate is self-signed: it isn't checked, as xiloader doesn't.
        SCHANNEL_CRED settings{};
        settings.dwVersion = SCHANNEL_CRED_VERSION;
        settings.grbitEnabledProtocols = protocols;
        settings.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
        SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                                                           SECPKG_CRED_OUTBOUND, nullptr, &settings, nullptr, nullptr,
                                                           &credentials_, nullptr);
        if (status != SEC_E_OK) {
            error = L"Windows' TLS wouldn't start (" + hex(static_cast<unsigned long>(status)) + L")";
            return false;
        }
        have_credentials_ = true;
        host_ = host;
        return handshake({}, error);
    }

    // True once the socket connected (a failed connect() got as far as TLS)
    bool connected() const { return connected_; }

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

    bool write(const std::string& message)
    {
        if (message.size() > sizes_.cbMaximumMessage)
            return false;
        std::vector<char> record(sizes_.cbHeader + message.size() + sizes_.cbTrailer);
        std::memcpy(record.data() + sizes_.cbHeader, message.data(), message.size());
        SecBuffer buffers[4] = {
            {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
            {static_cast<unsigned long>(message.size()), SECBUFFER_DATA, record.data() + sizes_.cbHeader},
            {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes_.cbHeader + message.size()},
            {0, SECBUFFER_EMPTY, nullptr},
        };
        SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
        if (EncryptMessage(&context_, 0, &description, 0) != SEC_E_OK)
            return false;
        return send_all(record.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
    }

    // Reads until `done` says the text read so far is complete; false if the connection ends or
    // times out first, or TLS fails (the reason in `error`).
    template <typename Done>
    bool read(std::string& text, Done done, std::wstring& error)
    {
        while (!done(text)) {
            if (!incoming_.empty()) {
                SecBuffer buffers[4] = {
                    {static_cast<unsigned long>(incoming_.size()), SECBUFFER_DATA, incoming_.data()},
                    {0, SECBUFFER_EMPTY, nullptr},
                    {0, SECBUFFER_EMPTY, nullptr},
                    {0, SECBUFFER_EMPTY, nullptr},
                };
                SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
                const SECURITY_STATUS status = DecryptMessage(&context_, &description, 0, nullptr);
                if (status == SEC_E_OK || status == SEC_I_RENEGOTIATE || status == SEC_I_CONTEXT_EXPIRED) {
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
                        break; // the server closed the TLS session
                    // TLS 1.3's messages after the handshake (session tickets) come as a renegotiation
                    if (status == SEC_I_RENEGOTIATE) {
                        std::vector<char> input = std::move(incoming_);
                        incoming_.clear();
                        if (!handshake(std::move(input), error))
                            return false;
                    }
                    continue;
                }
                if (status != SEC_E_INCOMPLETE_MESSAGE) {
                    error = L"TLS failed reading the reply (" + hex(static_cast<unsigned long>(status)) + L")";
                    return false;
                }
            }
            if (!receive())
                break;
        }
        return done(text);
    }

private:
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

    // Runs InitializeSecurityContext until the handshake is done, starting from `input` (tokens
    // already read; empty to start a new handshake).
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
            SECURITY_STATUS status = InitializeSecurityContextW(
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
                error = L"the TLS handshake failed (" + hex(static_cast<unsigned long>(status)) + L")";
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

struct Winsock {
    bool ready = false;
    Winsock()
    {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Winsock()
    {
        if (ready)
            WSACleanup();
    }
};

} // namespace

bool resolve(const std::wstring& server, std::uint32_t& address, std::wstring& message)
{
    Winsock winsock;
    if (!winsock.ready) {
        message = L"Windows' networking (winsock) wouldn't start.";
        return false;
    }
    addrinfoW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* found = nullptr;
    const int error = GetAddrInfoW(server.c_str(), nullptr, &hints, &found);
    if (error != 0 || found == nullptr) {
        message = L"Couldn't find the server " + server + L" (winsock error " + std::to_wstring(error) +
                  L"). Check your internet connection.";
        return false;
    }
    address = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
    FreeAddrInfoW(found);
    return true;
}

bool parse_version(const std::wstring& text, Version& version)
{
    Version parsed = {0, 0, 0};
    size_t part = 0, at = 0;
    for (;;) {
        const size_t dot = text.find(L'.', at);
        const std::wstring number = text.substr(at, dot == std::wstring::npos ? std::wstring::npos : dot - at);
        if (part == parsed.size() || number.empty() || number.size() > 3 ||
            number.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;
        parsed[part] = std::stoi(number);
        if (parsed[part++] > 255)
            return false;
        if (dot == std::wstring::npos)
            break;
        at = dot + 1;
    }
    if (part < 2)
        return false;
    version = parsed;
    return true;
}

Result log_in(const std::wstring& server, std::uint32_t address, std::uint16_t port, const std::wstring& user,
              const std::wstring& password, const Version& version)
{
    Result result;
    Winsock winsock;
    if (!winsock.ready) {
        result.message = L"Windows' networking (winsock) wouldn't start.";
        return result;
    }
    const std::wstring where = server + L":" + std::to_wstring(port);

    // Windows 7's TLS offers only 1.0 unless asked for more (1.1 and 1.2 are off by default for
    // clients there) and the server takes 1.2 and up, refusing it (0x80090302): a handshake that
    // fails is tried again on a new connection, asking for 1.2
    std::optional<Tls> tls;
    tls.emplace();
    std::wstring error;
    bool connected = tls->connect(address, port, server, 0, error);
    if (!connected && tls->connected()) {
        std::wstring first = std::move(error);
        tls.emplace();
        error.clear();
        connected = tls->connect(address, port, server, SP_PROT_TLS1_2_CLIENT, error);
        if (!connected)
            error = first + L"; asking for TLS 1.2, " + error;
    }
    if (!connected) {
        result.message = L"Couldn't reach the login server at " + where + L": " + error + L".";
        return result;
    }

    std::string request = "{\"command\":" + std::to_string(kLoginCommand) + ",\"username\":" + json_string(utf8(user)) +
                          ",\"password\":" + json_string(utf8(password)) + ",\"otp\":\"\",\"new_password\":\"\"" +
                          // 2.1's trusted computer (skipping one-time passwords, which VanaCore doesn't use)
                          ",\"trust_token\":\"\",\"trust_this_computer\":false" +
                          ",\"version\":[" + std::to_string(version[0]) + "," + std::to_string(version[1]) + "," +
                          std::to_string(version[2]) + "]}";
    const bool sent = tls->write(request);
    SecureZeroMemory(request.data(), request.size());
    if (!sent) {
        result.message = L"Couldn't send the login to " + where + L".";
        return result;
    }

    std::string reply;
    if (!tls->read(reply, [](const std::string& text) { return whole_object(text); }, error)) {
        result.message = reply.empty() ? L"The login server at " + where + L" didn't reply" +
                                             (error.empty() ? L"" : L" (" + error + L")") + L"."
                                       : L"The login server's reply made no sense.";
        return result;
    }
    Value json;
    if (!Parser(reply).parse(json) || json.type != Value::Type::Object) {
        result.message = L"The login server's reply made no sense.";
        return result;
    }

    // A message of the server's own (an unsupported version, maintenance, ...)
    if (const Value* message = json.member("error_message");
        message != nullptr && message->type == Value::Type::String && !message->text.empty()) {
        result.outcome = Outcome::Refused;
        result.message = wide(message->text);
        return result;
    }
    const Value* code = json.member("result");
    if (code == nullptr || code->type != Value::Type::Number) {
        result.message = L"The login server's reply had no result.";
        return result;
    }
    switch (static_cast<int>(code->number)) {
    case kResultSuccess: {
        const Value* id = json.member("account_id");
        const Value* hash = json.member("session_hash");
        if (id == nullptr || id->type != Value::Type::Number || hash == nullptr || hash->type != Value::Type::Array ||
            hash->items.size() != sizeof(result.session_hash)) {
            result.message = L"The login server accepted the login but sent no account or session.";
            return result;
        }
        result.account_id = static_cast<std::uint32_t>(id->number);
        for (size_t i = 0; i < hash->items.size(); ++i)
            result.session_hash[i] = static_cast<std::uint8_t>(static_cast<int>(hash->items[i].number) & 0xFF);
        result.outcome = Outcome::LoggedIn;
        return result;
    }
    case kResultWrongPassword:
        result.outcome = Outcome::Refused;
        result.message = L"Wrong username or password.";
        return result;
    case kResultFail:
        result.outcome = Outcome::Refused;
        result.message = L"This account can't log in (it doesn't exist, or it's suspended).";
        return result;
    case kResultAlreadyLoggedIn:
        result.outcome = Outcome::Refused;
        result.message = L"This account is already logged in.";
        return result;
    case kResultVersionUnsupported:
        result.outcome = Outcome::Refused;
        result.message = L"The server doesn't support this VanaCore's login. Update VanaCore.";
        return result;
    default:
        result.outcome = Outcome::Refused;
        result.message = L"The login server refused the login (code " + std::to_wstring(static_cast<int>(code->number)) +
                         L").";
        return result;
    }
}

} // namespace vanacore::login
