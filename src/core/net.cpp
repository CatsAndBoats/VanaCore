#include "net.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// WinHTTP does the TLS, the proxy and the redirects; it is loaded by name so VanaCore still starts on a Windows without
// the WebSocket calls (Windows 7: requests work, sockets say so).
namespace vanacore::net {
namespace {

constexpr int kMaxWorkers = 6;                 // requests running at once; the rest wait their turn
constexpr int kMaxRequestsPerOwner = 256;      // queued or running, per addon
constexpr int kMaxSocketsPerOwner = 16;
constexpr int kMaxSockets = 64;                // in the whole process
constexpr size_t kMailboxLimit = 32u * 1024 * 1024; // a socket waits to deliver while an addon has this much unread
constexpr size_t kMaxQueuedSend = 16u * 1024 * 1024; // what an addon may have waiting to be sent on one socket
constexpr std::uint64_t kMaxBody = 256u * 1024 * 1024; // the most of a request's own body
constexpr auto kCloseWait = std::chrono::seconds(5);   // how long a socket waits for the server to answer a close

////////////////////////////////////////////////////////////////////////////////////////////////////
// Text

std::wstring widen(const std::string& text, UINT codepage = CP_UTF8)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(codepage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    if (length > 0)
        MultiByteToWideChar(codepage, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string narrow(const wchar_t* text, size_t size, UINT codepage = CP_UTF8)
{
    if (size == 0)
        return {};
    const int length = WideCharToMultiByte(codepage, 0, text, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    if (length > 0)
        WideCharToMultiByte(codepage, 0, text, static_cast<int>(size), out.data(), length, nullptr, nullptr);
    return out;
}

std::string lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool token_char(unsigned char c)
{
    if (c <= 32 || c >= 127)
        return false;
    return std::strchr("()<>@,;:\\\"/[]?={}", c) == nullptr;
}

bool token(const std::string& text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return token_char(static_cast<unsigned char>(c)); });
}

std::string base64(const std::string& data)
{
    static const char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(data[i]);
        const unsigned b = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
        const unsigned c = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
        out += kDigits[a >> 2];
        out += kDigits[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < data.size() ? kDigits[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < data.size() ? kDigits[c & 63] : '=';
    }
    return out;
}

std::string percent_decode(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// WinHTTP, by name

struct Api {
    HMODULE module = nullptr;
    bool ok = false;        // the calls requests need
    bool websocket = false; // and the ones sockets need (Windows 8)
    HINTERNET(WINAPI* open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) = nullptr;
    HINTERNET(WINAPI* connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD) = nullptr;
    HINTERNET(WINAPI* open_request)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD) = nullptr;
    BOOL(WINAPI* set_option)(HINTERNET, DWORD, LPVOID, DWORD) = nullptr;
    BOOL(WINAPI* set_timeouts)(HINTERNET, int, int, int, int) = nullptr;
    BOOL(WINAPI* send_request)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) = nullptr;
    BOOL(WINAPI* receive_response)(HINTERNET, LPVOID) = nullptr;
    BOOL(WINAPI* query_headers)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD) = nullptr;
    BOOL(WINAPI* query_option)(HINTERNET, DWORD, LPVOID, LPDWORD) = nullptr;
    BOOL(WINAPI* read_data)(HINTERNET, LPVOID, DWORD, LPDWORD) = nullptr;
    BOOL(WINAPI* close_handle)(HINTERNET) = nullptr;
    HINTERNET(WINAPI* socket_upgrade)(HINTERNET, DWORD_PTR) = nullptr;
    DWORD(WINAPI* socket_send)(HINTERNET, WINHTTP_WEB_SOCKET_BUFFER_TYPE, PVOID, DWORD) = nullptr;
    DWORD(WINAPI* socket_receive)(HINTERNET, PVOID, DWORD, DWORD*, WINHTTP_WEB_SOCKET_BUFFER_TYPE*) = nullptr;
    DWORD(WINAPI* socket_shutdown)(HINTERNET, USHORT, PVOID, DWORD) = nullptr;
    DWORD(WINAPI* socket_close)(HINTERNET, USHORT, PVOID, DWORD) = nullptr;
    DWORD(WINAPI* socket_close_status)(HINTERNET, USHORT*, PVOID, DWORD, DWORD*) = nullptr;
};

template <typename T>
bool bind(HMODULE module, const char* name, T& function)
{
    function = reinterpret_cast<T>(GetProcAddress(module, name));
    return function != nullptr;
}

const Api& api()
{
    static const Api& instance = *[] {
        auto* a = new Api; // lives as long as the process: worker threads may still be using it when it ends
        wchar_t folder[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(folder, MAX_PATH);
        a->module = length > 0 && length < MAX_PATH ? LoadLibraryW((std::wstring(folder) + L"\\winhttp.dll").c_str()) : nullptr;
        if (a->module == nullptr)
            a->module = LoadLibraryW(L"winhttp.dll");
        if (a->module == nullptr)
            return a;
        HMODULE m = a->module;
        a->ok = bind(m, "WinHttpOpen", a->open) && bind(m, "WinHttpConnect", a->connect) &&
                bind(m, "WinHttpOpenRequest", a->open_request) && bind(m, "WinHttpSetOption", a->set_option) &&
                bind(m, "WinHttpSetTimeouts", a->set_timeouts) && bind(m, "WinHttpSendRequest", a->send_request) &&
                bind(m, "WinHttpReceiveResponse", a->receive_response) && bind(m, "WinHttpQueryHeaders", a->query_headers) &&
                bind(m, "WinHttpQueryOption", a->query_option) && bind(m, "WinHttpReadData", a->read_data) &&
                bind(m, "WinHttpCloseHandle", a->close_handle);
        a->websocket = a->ok && bind(m, "WinHttpWebSocketCompleteUpgrade", a->socket_upgrade) &&
                       bind(m, "WinHttpWebSocketSend", a->socket_send) && bind(m, "WinHttpWebSocketReceive", a->socket_receive) &&
                       bind(m, "WinHttpWebSocketShutdown", a->socket_shutdown) && bind(m, "WinHttpWebSocketClose", a->socket_close) &&
                       bind(m, "WinHttpWebSocketQueryCloseStatus", a->socket_close_status);
        return a;
    }();
    return instance;
}

// "The server name or address could not be resolved (WinHTTP error 12007)".
std::string describe(DWORD error)
{
    wchar_t text[256] = {};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        api().module, error, 0, text, 256, nullptr);
    std::string message = narrow(text, length);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ' || message.back() == '.'))
        message.pop_back();
    if (message.empty())
        message = "request failed";
    return message + " (error " + std::to_string(error) + ")";
}

// The word an addon can test an error by.
const char* code_of(DWORD error)
{
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:
        return "timeout";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "dns";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "connect";
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return "tls";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "cancelled";
    case ERROR_WINHTTP_REDIRECT_FAILED:
        return "redirect";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return "invalid_url";
    default:
        return "failed";
    }
}

std::wstring g_agent = L"VanaCore";
std::mutex g_agent_lock;

// The one session every request goes through (its connections are kept alive between requests), made when the first is.
HINTERNET session()
{
    static std::mutex lock;
    static HINTERNET handle = nullptr;
    static bool tried = false;
    std::lock_guard<std::mutex> guard(lock);
    if (tried)
        return handle;
    tried = true;
    const Api& a = api();
    if (!a.ok)
        return nullptr;
    std::wstring agent;
    {
        std::lock_guard<std::mutex> agent_guard(g_agent_lock);
        agent = g_agent;
    }
    handle = a.open(agent.c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY: the system's, as the browser finds it (Windows 8.1) */, nullptr,
                    nullptr, 0);
    if (handle == nullptr)
        handle = a.open(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (handle == nullptr)
        return nullptr;
    // TLS 1.2 (and 1.3 where Windows knows it): an older Windows offers only TLS 1.0 unless it is asked
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!a.set_option(handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        a.set_option(handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL; // gzip and deflate, handled for the addon (Windows 8.1)
    a.set_option(handle, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH; // never send the player's Windows login to a server on its own
    a.set_option(handle, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
    return handle;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// URLs

struct ParsedUrl {
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path = L"/";
    std::string authorization; // "Basic ..." from user:password@ in the URL (empty: none)
};

bool parse_url(const std::string& url, bool socket, ParsedUrl& out, std::string& error)
{
    if (std::any_of(url.begin(), url.end(), [](char c) { return static_cast<unsigned char>(c) <= 32 || c == 127; })) {
        error = "the URL has a space or control character in it (encode it, e.g. %20)";
        return false;
    }
    const size_t colon = url.find("://");
    if (colon == std::string::npos) {
        error = socket ? "the URL must start with ws:// or wss://" : "the URL must start with http:// or https://";
        return false;
    }
    const std::string scheme = lower(url.substr(0, colon));
    if (scheme == "https" || scheme == "wss")
        out.secure = true;
    else if (scheme != "http" && scheme != "ws") {
        error = scheme + ":// URLs aren't supported (use http:// or https://" + std::string(socket ? ", ws:// or wss://" : "") + ")";
        return false;
    }
    if (!socket && (scheme == "ws" || scheme == "wss")) {
        error = "a ws:// URL is for vana.ws.connect; vana.http fetches http:// and https://";
        return false;
    }
    std::string rest = url.substr(colon + 3);
    const size_t fragment = rest.find('#');
    if (fragment != std::string::npos)
        rest.resize(fragment);
    const size_t end = rest.find_first_of("/?");
    std::string authority = end == std::string::npos ? rest : rest.substr(0, end);
    std::string path = end == std::string::npos ? "/" : rest.substr(end);
    if (path[0] == '?')
        path = "/" + path;
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        const std::string info = authority.substr(0, at);
        authority = authority.substr(at + 1);
        const size_t split = info.find(':');
        const std::string user = percent_decode(split == std::string::npos ? info : info.substr(0, split));
        const std::string password = split == std::string::npos ? std::string() : percent_decode(info.substr(split + 1));
        out.authorization = "Basic " + base64(user + ":" + password);
    }
    std::string host = authority;
    std::string port;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) {
            error = "the URL's address has a '[' with no ']'";
            return false;
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                error = "the URL's address isn't valid";
                return false;
            }
            port = authority.substr(close + 2);
        }
    } else {
        const size_t split = authority.rfind(':');
        if (split != std::string::npos) {
            host = authority.substr(0, split);
            port = authority.substr(split + 1);
        }
    }
    if (host.empty()) {
        error = "the URL has no host";
        return false;
    }
    unsigned number = out.secure ? 443 : 80;
    if (!port.empty()) {
        number = 0;
        for (const char c : port) {
            if (c < '0' || c > '9' || number > 65535) {
                number = 0;
                break;
            }
            number = number * 10 + static_cast<unsigned>(c - '0');
        }
        if (number == 0 || number > 65535) {
            error = "the URL's port isn't valid";
            return false;
        }
    }
    out.port = static_cast<INTERNET_PORT>(number);
    out.host = widen(host);
    out.path = widen(path);
    return true;
}

// "Name: value\r\n" for each, after checking none can break the request into two.
bool header_text(const Headers& headers, std::string& out, std::string& error)
{
    for (const auto& [name, value] : headers) {
        if (!token(name)) {
            error = "'" + name + "' isn't a valid header name";
            return false;
        }
        if (value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos) {
            error = "the " + name + " header has a line break in it";
            return false;
        }
        const std::string key = lower(name);
        if (key == "content-length" || key == "transfer-encoding" || key == "connection")
            continue; // WinHTTP sets these itself
        out += name + ": " + value + "\r\n";
    }
    return true;
}

bool has_header(const Headers& headers, const char* wanted)
{
    for (const auto& header : headers)
        if (lower(header.first) == wanted)
            return true;
    return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Owners, jobs and mailboxes

struct OwnerState;

struct Job {
    Id id = 0;
    bool socket = false;
    std::shared_ptr<OwnerState> owner;
    std::atomic<bool> cancelled{false};
    // The WinHTTP handles it holds: whoever takes one out of its slot closes it, so cancelling from another thread (which
    // is how a blocked call is made to return) and the job's own cleanup never close one twice.
    enum Slot { kConnection, kRequest, kSocket, kSlots };
    std::atomic<HINTERNET> slots[kSlots];

    Job()
    {
        for (auto& slot : slots)
            slot.store(nullptr);
    }
    virtual ~Job() { release_all(); }

    // Keeps a handle for the job; false (and the handle closed) when it was cancelled meanwhile.
    bool hold(int slot, HINTERNET handle)
    {
        slots[slot].store(handle);
        if (cancelled.load()) {
            release(slot);
            return false;
        }
        return true;
    }
    void release(int slot)
    {
        if (HINTERNET handle = slots[slot].exchange(nullptr))
            api().close_handle(handle);
    }
    void release_all()
    {
        for (int slot = kSlots; slot-- > 0;)
            release(slot);
    }
    // Ends the job from any thread: the call it is blocked in fails and it notices `cancelled`.
    virtual void stop()
    {
        cancelled.store(true);
        release_all();
    }
};

struct OwnerState {
    std::mutex lock;
    std::condition_variable room; // a poll emptied the mailbox some
    std::deque<Event> events;
    size_t bytes = 0;
    bool dead = false;
    Id next_id = 1;
    std::unordered_map<Id, std::shared_ptr<Job>> jobs;
    int requests = 0;
    int sockets = 0;
};

struct HttpJob : Job {
    HttpRequest request;
    ParsedUrl url;
    std::string headers;
};

struct Outgoing {
    bool close = false;
    bool binary = false;
    int code = 1000;
    std::string data; // the message; a close's reason
};

struct SocketJob : Job {
    WebSocketRequest request;
    ParsedUrl url;
    std::string headers;
    std::mutex m;
    std::condition_variable cv;
    std::deque<Outgoing> out;
    size_t queued = 0;
    bool opened = false;
    bool closing = false;  // a close is on its way (ours or the server's): nothing more is sent
    bool finished = false; // the receiving thread is done
    bool user_closed = false;
    bool have_deadline = false;
    std::chrono::steady_clock::time_point deadline;

    void stop() override
    {
        {
            std::lock_guard<std::mutex> guard(m);
            cancelled.store(true);
        }
        release_all();
        cv.notify_all();
    }
};

struct State {
    std::mutex lock;
    std::unordered_map<Owner, std::shared_ptr<OwnerState>> owners;
    Owner next = 1;

    std::mutex pool_lock;
    std::condition_variable pool_cv;
    std::deque<std::shared_ptr<HttpJob>> queue;
    int threads = 0;
    int idle = 0;

    std::atomic<int> sockets{0};
};

State& state()
{
    static State* instance = new State; // never destroyed: threads may still run when the process ends
    return *instance;
}

std::shared_ptr<OwnerState> find(Owner owner)
{
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    const auto found = s.owners.find(owner);
    return found == s.owners.end() ? nullptr : found->second;
}

void post(OwnerState& owner, Event event)
{
    std::lock_guard<std::mutex> guard(owner.lock);
    if (owner.dead)
        return;
    owner.bytes += event.data.size();
    owner.events.push_back(std::move(event));
}

// A socket's thread waits here while the addon has not read what it was sent (the server's sending then backs up
// instead of memory filling); false when the job or the addon is gone.
bool wait_room(OwnerState& owner, Job& job)
{
    std::unique_lock<std::mutex> lock(owner.lock);
    while (!owner.dead && !job.cancelled.load() && owner.bytes >= kMailboxLimit)
        owner.room.wait_for(lock, std::chrono::milliseconds(200));
    return !owner.dead && !job.cancelled.load();
}

// The last thing a job does: forgets it.
void finish(Job& job)
{
    {
        std::lock_guard<std::mutex> guard(job.owner->lock);
        job.owner->jobs.erase(job.id);
        if (job.socket)
            --job.owner->sockets;
        else
            --job.owner->requests;
    }
    if (job.socket)
        --state().sockets;
}

Event error_event(Event::Kind kind, const char* code, std::string text)
{
    Event event;
    event.kind = kind;
    event.code = code;
    event.text = std::move(text);
    return event;
}

Event failure(Event::Kind kind, DWORD error, const Job& job)
{
    if (job.cancelled.load())
        return error_event(kind, "cancelled", "cancelled");
    return error_event(kind, code_of(error), describe(error));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// HTTP

int milliseconds(double seconds)
{
    return static_cast<int>(std::clamp(seconds * 1000.0, 1.0, 2000000000.0));
}

std::wstring query_string(const Api& a, HINTERNET request, DWORD what)
{
    DWORD size = 0;
    a.query_headers(request, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
    if (!a.query_headers(request, what, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    text.resize(size / sizeof(wchar_t));
    return text;
}

struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring part;
    std::wstring final_path;
    ~File()
    {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            DeleteFileW(part.c_str()); // never finished
        }
    }
};

void fetch(HttpJob& job, Event& out)
{
    const HttpRequest& r = job.request;
    const Api& a = api();
    const auto cancelled = [&] { out = error_event(Event::Kind::HttpError, "cancelled", "cancelled"); };
    const auto failed = [&](DWORD error) { out = failure(Event::Kind::HttpError, error, job); };
    if (job.cancelled.load())
        return cancelled();
    HINTERNET ses = session();
    if (ses == nullptr) {
        out = error_event(Event::Kind::HttpError, "unavailable", "WinHTTP isn't available on this PC");
        return;
    }

    const ULONGLONG started = GetTickCount64();
    const ULONGLONG limit = started + static_cast<ULONGLONG>(std::clamp(r.max_time, 1.0, 86400.0) * 1000.0);
    const int step = static_cast<int>(std::min<ULONGLONG>(milliseconds(r.timeout), limit - started));

    HINTERNET connection = a.connect(ses, job.url.host.c_str(), job.url.port, 0);
    if (connection == nullptr)
        return failed(GetLastError());
    if (!job.hold(Job::kConnection, connection))
        return cancelled();
    const std::wstring method = widen(r.method);
    HINTERNET request = a.open_request(connection, method.c_str(), job.url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, job.url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
        return failed(GetLastError());
    if (!job.hold(Job::kRequest, request))
        return cancelled();

    DWORD policy = r.follow ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    a.set_option(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    DWORD redirects = static_cast<DWORD>(std::clamp(r.max_redirects, 0, 50));
    a.set_option(request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects));
    // No cookie jar shared between addons and no Windows sign-in: an addon sends the Cookie and Authorization it means to
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    a.set_option(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    if (r.insecure) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        a.set_option(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }
    a.set_timeouts(request, step, step, step, step);

    std::wstring headers = widen(job.headers);
    DWORD body_size = static_cast<DWORD>(r.body.size());
    if (!a.send_request(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                        headers.empty() ? 0 : static_cast<DWORD>(-1), body_size == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(r.body.data()),
                        body_size, body_size, 0))
        return failed(GetLastError());
    if (!a.receive_response(request, nullptr))
        return failed(GetLastError());

    DWORD status = 0;
    DWORD size = sizeof(status);
    a.query_headers(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                    WINHTTP_NO_HEADER_INDEX);
    Event event;
    event.kind = Event::Kind::HttpResponse;
    event.status = static_cast<int>(status);
    {
        const std::wstring reason = query_string(a, request, WINHTTP_QUERY_STATUS_TEXT);
        event.text = narrow(reason.c_str(), reason.size());
        const std::wstring raw = query_string(a, request, WINHTTP_QUERY_RAW_HEADERS_CRLF);
        event.headers = narrow(raw.c_str(), raw.size());
        DWORD url_size = 0;
        a.query_option(request, WINHTTP_OPTION_URL, nullptr, &url_size);
        if (url_size > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring final_url(url_size / sizeof(wchar_t) + 1, L'\0');
            if (a.query_option(request, WINHTTP_OPTION_URL, final_url.data(), &url_size))
                event.url = narrow(final_url.c_str(), url_size / sizeof(wchar_t));
        }
        if (event.url.empty())
            event.url = r.url;
    }

    DWORD announced = 0;
    size = sizeof(announced);
    const bool has_length = a.query_headers(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                            &announced, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
    if (has_length && announced > r.max_size) {
        out = error_event(Event::Kind::HttpError, "too_large",
                          "the answer is " + std::to_string(announced) + " bytes, more than max_size (" + std::to_string(r.max_size) + ")");
        return;
    }

    // Only an answer that worked replaces the file: an error page is handed back to the addon (its first 64 KB) instead
    const bool to_file = !r.save.empty() && status >= 200 && status < 300;
    File file;
    if (to_file) {
        file.final_path = widen(r.save, CP_ACP);
        file.part = file.final_path + L".part";
        file.handle = CreateFileW(file.part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.handle == INVALID_HANDLE_VALUE) {
            out = error_event(Event::Kind::HttpError, "file", "can't write " + r.save + " (Windows error " + std::to_string(GetLastError()) + ")");
            return;
        }
    } else if (has_length && r.save.empty()) {
        event.data.reserve(static_cast<size_t>(std::min<std::uint64_t>(announced, 64u * 1024 * 1024)));
    }

    std::string chunk(32768, '\0');
    std::uint64_t total = 0;
    for (;;) {
        if (job.cancelled.load())
            return cancelled();
        if (GetTickCount64() > limit) {
            out = error_event(Event::Kind::HttpError, "timeout", "took longer than max_time (" + std::to_string(static_cast<int>(r.max_time)) + " s)");
            return;
        }
        DWORD got = 0;
        if (!a.read_data(request, chunk.data(), static_cast<DWORD>(chunk.size()), &got))
            return failed(GetLastError());
        if (got == 0)
            break;
        total += got;
        if (total > r.max_size) {
            out = error_event(Event::Kind::HttpError, "too_large", "the answer is more than max_size (" + std::to_string(r.max_size) + " bytes)");
            return;
        }
        if (file.handle != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!WriteFile(file.handle, chunk.data(), got, &written, nullptr) || written != got) {
                out = error_event(Event::Kind::HttpError, "file", "can't write " + r.save + " (Windows error " + std::to_string(GetLastError()) + ")");
                return;
            }
        } else {
            event.data.append(chunk.data(), got);
            if (!r.save.empty() && event.data.size() >= 64 * 1024)
                break;
        }
    }
    if (file.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file.handle);
        file.handle = INVALID_HANDLE_VALUE; // from here its destructor leaves the file alone
        if (!MoveFileExW(file.part.c_str(), file.final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(file.part.c_str());
            out = error_event(Event::Kind::HttpError, "file", "can't write " + r.save + " (Windows error " + std::to_string(error) + ")");
            return;
        }
        event.saved = r.save;
    }
    out = std::move(event);
}

void run_http(const std::shared_ptr<HttpJob>& job)
{
    Event result;
    try {
        fetch(*job, result);
    } catch (const std::exception& e) {
        result = error_event(Event::Kind::HttpError, "failed", std::string("internal error: ") + e.what());
    } catch (...) {
        result = error_event(Event::Kind::HttpError, "failed", "internal error");
    }
    job->release_all();
    result.id = job->id;
    post(*job->owner, std::move(result));
    finish(*job);
}

void worker()
{
    State& s = state();
    std::unique_lock<std::mutex> lock(s.pool_lock);
    for (;;) {
        ++s.idle;
        const bool work = s.pool_cv.wait_for(lock, std::chrono::seconds(30), [&] { return !s.queue.empty(); });
        --s.idle;
        if (!work) {
            --s.threads; // idle for a while: this thread goes
            return;
        }
        std::shared_ptr<HttpJob> job = std::move(s.queue.front());
        s.queue.pop_front();
        lock.unlock();
        run_http(job);
        job.reset();
        lock.lock();
    }
}

void submit(const std::shared_ptr<HttpJob>& job)
{
    State& s = state();
    std::lock_guard<std::mutex> guard(s.pool_lock);
    s.queue.push_back(job);
    if (s.queue.size() > static_cast<size_t>(s.idle) && s.threads < kMaxWorkers) {
        try {
            std::thread(&worker).detach();
            ++s.threads;
        } catch (const std::exception&) {
            // no thread to be had now: an idle one (or none yet, then the next request) takes it
        }
    }
    s.pool_cv.notify_one();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// WebSocket

int valid_close_code(int code)
{
    return code == 1000 || (code >= 3000 && code <= 4999) ? code : 1000;
}

std::string clip_reason(std::string reason)
{
    if (reason.size() > 123) {
        reason.resize(123);
        while (!reason.empty() && (static_cast<unsigned char>(reason.back()) & 0xC0) == 0x80)
            reason.pop_back(); // not in the middle of a character
        if (!reason.empty() && static_cast<unsigned char>(reason.back()) >= 0xC0)
            reason.pop_back();
    }
    return reason;
}

// Sends what the addon queued, one message at a time in order; the close handshake when it asks for one.
void socket_writer(std::shared_ptr<SocketJob> job)
{
    const Api& a = api();
    std::unique_lock<std::mutex> lock(job->m);
    for (;;) {
        if (job->finished || job->cancelled.load())
            return;
        if (job->out.empty()) {
            if (job->have_deadline) {
                if (job->cv.wait_until(lock, job->deadline) == std::cv_status::timeout) {
                    lock.unlock();
                    job->stop(); // the server never answered the close
                    return;
                }
            } else {
                job->cv.wait(lock);
            }
            continue;
        }
        Outgoing item = std::move(job->out.front());
        job->out.pop_front();
        job->queued -= item.data.size();
        lock.unlock();

        HINTERNET socket = job->slots[Job::kSocket].load();
        if (socket == nullptr)
            return;
        DWORD error = 0;
        if (item.close) {
            error = a.socket_shutdown(socket, static_cast<USHORT>(item.code), item.data.empty() ? nullptr : item.data.data(),
                                      static_cast<DWORD>(item.data.size()));
            lock.lock();
            job->have_deadline = true;
            job->deadline = std::chrono::steady_clock::now() + kCloseWait;
        } else {
            error = a.socket_send(socket, item.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                  item.data.empty() ? nullptr : item.data.data(), static_cast<DWORD>(item.data.size()));
            lock.lock();
        }
        if (error != 0 && !job->cancelled.load()) {
            Event event = error_event(Event::Kind::WsError, code_of(error), "sending failed: " + describe(error));
            event.id = job->id;
            post(*job->owner, std::move(event));
            lock.unlock();
            job->stop();
            return;
        }
    }
}

struct Closed {
    int code = 1006;
    std::string reason;
    bool clean = false;
};

// Connects, completes the handshake and receives until the socket ends. Posts the open, message and error events.
void socket_run(const std::shared_ptr<SocketJob>& job, Closed& closed)
{
    const WebSocketRequest& r = job->request;
    const Api& a = api();
    const auto report = [&](const char* code, std::string text, int status = 0) {
        if (job->cancelled.load())
            return;
        Event event = error_event(Event::Kind::WsError, code, std::move(text));
        event.id = job->id;
        event.status = status;
        post(*job->owner, std::move(event));
    };
    const auto report_last = [&](DWORD error) { report(code_of(error), describe(error)); };
    if (job->cancelled.load())
        return;
    HINTERNET ses = session();
    if (ses == nullptr)
        return report("unavailable", "WinHTTP isn't available on this PC");

    const int step = milliseconds(r.timeout);
    HINTERNET connection = a.connect(ses, job->url.host.c_str(), job->url.port, 0);
    if (connection == nullptr)
        return report_last(GetLastError());
    if (!job->hold(Job::kConnection, connection))
        return;
    HINTERNET request = a.open_request(connection, L"GET", job->url.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       job->url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr)
        return report_last(GetLastError());
    if (!job->hold(Job::kRequest, request))
        return;
    if (!a.set_option(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        return report_last(GetLastError());
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    a.set_option(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    if (r.insecure) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        a.set_option(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }
    a.set_timeouts(request, step, step, step, step);

    const std::wstring headers = widen(job->headers);
    if (!a.send_request(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1),
                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return report_last(GetLastError());
    if (!a.receive_response(request, nullptr))
        return report_last(GetLastError());
    DWORD status = 0;
    DWORD size = sizeof(status);
    a.query_headers(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                    WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        const std::wstring text = query_string(a, request, WINHTTP_QUERY_STATUS_TEXT);
        return report("handshake", "the server answered " + std::to_string(status) + " " + narrow(text.c_str(), text.size()) + " instead of upgrading",
                      static_cast<int>(status));
    }
    std::string protocol;
    {
        DWORD length = 0;
        a.query_headers(request, WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol", WINHTTP_NO_OUTPUT_BUFFER, &length, WINHTTP_NO_HEADER_INDEX);
        if (length > 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            std::wstring text(length / sizeof(wchar_t) + 1, L'\0');
            if (a.query_headers(request, WINHTTP_QUERY_CUSTOM, L"Sec-WebSocket-Protocol", text.data(), &length, WINHTTP_NO_HEADER_INDEX))
                protocol = narrow(text.c_str(), length / sizeof(wchar_t));
        }
    }
    HINTERNET socket = a.socket_upgrade(request, 0);
    if (socket == nullptr)
        return report_last(GetLastError());
    job->release(Job::kRequest);
    if (!job->hold(Job::kSocket, socket))
        return;
    if (r.keepalive > 0) {
        DWORD interval = static_cast<DWORD>(std::clamp(r.keepalive * 1000.0, 1000.0, 3600000.0));
        a.set_option(socket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &interval, sizeof(interval));
    }
    a.set_timeouts(socket, 0, 0, 0, 0); // a quiet socket is not an error

    {
        std::lock_guard<std::mutex> guard(job->m);
        job->opened = true;
        if (job->user_closed) {
            // close() came while it was connecting: it ends here
            return;
        }
    }
    {
        Event event;
        event.kind = Event::Kind::WsOpen;
        event.id = job->id;
        event.url = r.url;
        event.text = protocol;
        post(*job->owner, std::move(event));
    }
    try {
        std::thread(&socket_writer, job).detach();
    } catch (const std::exception&) {
        return report("failed", "couldn't start the socket's sending thread");
    }

    std::string message;
    bool binary = false;
    std::string chunk(16384, '\0');
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        const DWORD error = a.socket_receive(socket, chunk.data(), static_cast<DWORD>(chunk.size()), &read, &type);
        if (job->cancelled.load())
            return;
        if (error != 0) {
            if (error == ERROR_WINHTTP_TIMEOUT)
                continue;
            report_last(error);
            return;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT code = 1005;
            char reason[123 + 1] = {};
            DWORD reason_size = 0;
            if (a.socket_close_status(socket, &code, reason, sizeof(reason) - 1, &reason_size) == 0) {
                closed.code = code;
                closed.reason.assign(reason, reason_size);
            } else {
                closed.code = 1005;
            }
            closed.clean = true;
            bool answered;
            {
                std::lock_guard<std::mutex> guard(job->m);
                answered = job->closing; // our close went first: this is the server's answer
                job->closing = true;
            }
            if (!answered)
                a.socket_close(socket, code == 1005 ? static_cast<USHORT>(1000) : code, nullptr, 0); // answer the server's
            return;
        }
        const bool fragment = type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
        binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
        if (message.size() + read > r.max_size) {
            report("too_large", "a message is more than max_size (" + std::to_string(r.max_size) + " bytes)");
            const char reason[] = "message too big";
            a.socket_close(socket, 1009, const_cast<char*>(reason), sizeof(reason) - 1);
            closed.code = 1009;
            closed.reason = reason;
            return;
        }
        message.append(chunk.data(), read);
        if (fragment)
            continue;
        Event event;
        event.kind = Event::Kind::WsMessage;
        event.id = job->id;
        event.data = std::move(message);
        event.flag = binary;
        message.clear();
        if (!wait_room(*job->owner, *job))
            return;
        post(*job->owner, std::move(event));
    }
}

void socket_main(std::shared_ptr<SocketJob> job)
{
    Closed closed;
    try {
        socket_run(job, closed);
    } catch (const std::exception& e) {
        Event event = error_event(Event::Kind::WsError, "failed", std::string("internal error: ") + e.what());
        event.id = job->id;
        post(*job->owner, std::move(event));
    } catch (...) {
    }
    {
        std::lock_guard<std::mutex> guard(job->m);
        job->finished = true;
    }
    job->cv.notify_all();
    job->release_all();
    Event event;
    event.kind = Event::Kind::WsClose;
    event.id = job->id;
    event.status = closed.code;
    event.text = closed.reason;
    event.flag = closed.clean;
    post(*job->owner, std::move(event));
    finish(*job);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////
// The calls

Owner create()
{
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    const Owner owner = s.next++;
    s.owners[owner] = std::make_shared<OwnerState>();
    return owner;
}

void destroy(Owner owner)
{
    State& s = state();
    std::shared_ptr<OwnerState> found;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        const auto it = s.owners.find(owner);
        if (it == s.owners.end())
            return;
        found = it->second;
        s.owners.erase(it);
    }
    std::vector<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> guard(found->lock);
        found->dead = true;
        found->events.clear();
        found->bytes = 0;
        for (const auto& entry : found->jobs)
            jobs.push_back(entry.second);
    }
    found->room.notify_all();
    for (const auto& job : jobs)
        job->stop();
}

bool http(Owner owner, HttpRequest request, Id& id, std::string& error)
{
    const auto state_of = find(owner);
    if (!state_of) {
        error = "the addon is gone";
        return false;
    }
    if (!api().ok || session() == nullptr) {
        error = "WinHTTP isn't available on this PC";
        return false;
    }
    request.method = request.method.empty() ? "GET" : request.method;
    for (char& c : request.method)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    if (!token(request.method)) {
        error = "'" + request.method + "' isn't a valid method";
        return false;
    }
    if (request.body.size() > kMaxBody) {
        error = "the body is more than 256 MB";
        return false;
    }
    auto job = std::make_shared<HttpJob>();
    if (!parse_url(request.url, false, job->url, error))
        return false;
    if (!job->url.authorization.empty() && !has_header(request.headers, "authorization"))
        request.headers.emplace_back("Authorization", job->url.authorization);
    if (!has_header(request.headers, "accept"))
        request.headers.emplace_back("Accept", "*/*");
    if (!header_text(request.headers, job->headers, error))
        return false;
    job->request = std::move(request);
    job->owner = state_of;
    {
        std::lock_guard<std::mutex> guard(state_of->lock);
        if (state_of->dead) {
            error = "the addon is gone";
            return false;
        }
        if (state_of->requests >= kMaxRequestsPerOwner) {
            error = "too many requests at once (" + std::to_string(kMaxRequestsPerOwner) + ")";
            return false;
        }
        job->id = state_of->next_id++;
        ++state_of->requests;
        state_of->jobs[job->id] = job;
    }
    id = job->id;
    submit(job);
    return true;
}

bool websocket(Owner owner, WebSocketRequest request, Id& id, std::string& error)
{
    const auto state_of = find(owner);
    if (!state_of) {
        error = "the addon is gone";
        return false;
    }
    if (!api().websocket) {
        error = "WebSockets need Windows 8 or later";
        return false;
    }
    if (session() == nullptr) {
        error = "WinHTTP isn't available on this PC";
        return false;
    }
    auto job = std::make_shared<SocketJob>();
    job->socket = true;
    if (!parse_url(request.url, true, job->url, error))
        return false;
    if (!job->url.authorization.empty() && !has_header(request.headers, "authorization"))
        request.headers.emplace_back("Authorization", job->url.authorization);
    if (!request.protocols.empty()) {
        std::string list;
        for (const std::string& protocol : request.protocols) {
            if (!token(protocol)) {
                error = "'" + protocol + "' isn't a valid protocol name";
                return false;
            }
            list += (list.empty() ? "" : ", ") + protocol;
        }
        request.headers.emplace_back("Sec-WebSocket-Protocol", list);
    }
    if (!header_text(request.headers, job->headers, error))
        return false;
    job->request = std::move(request);
    job->owner = state_of;
    {
        std::lock_guard<std::mutex> guard(state_of->lock);
        if (state_of->dead) {
            error = "the addon is gone";
            return false;
        }
        if (state_of->sockets >= kMaxSocketsPerOwner || state().sockets.load() >= kMaxSockets) {
            error = "too many open WebSockets (" + std::to_string(kMaxSocketsPerOwner) + " an addon)";
            return false;
        }
        job->id = state_of->next_id++;
        ++state_of->sockets;
        ++state().sockets;
        state_of->jobs[job->id] = job;
    }
    id = job->id;
    try {
        std::thread(&socket_main, job).detach();
    } catch (const std::exception&) {
        Event event = error_event(Event::Kind::WsError, "failed", "couldn't start the socket's thread");
        event.id = id;
        post(*state_of, std::move(event));
        Event closed;
        closed.kind = Event::Kind::WsClose;
        closed.id = id;
        closed.status = 1006;
        post(*state_of, std::move(closed));
        finish(*job);
    }
    return true;
}

void cancel(Owner owner, Id id)
{
    const auto state_of = find(owner);
    if (!state_of)
        return;
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> guard(state_of->lock);
        const auto found = state_of->jobs.find(id);
        if (found != state_of->jobs.end())
            job = found->second;
    }
    if (job)
        job->stop();
}

bool send(Owner owner, Id id, std::string data, bool binary, std::string& error)
{
    const auto state_of = find(owner);
    std::shared_ptr<Job> found;
    if (state_of) {
        std::lock_guard<std::mutex> guard(state_of->lock);
        const auto it = state_of->jobs.find(id);
        if (it != state_of->jobs.end() && it->second->socket)
            found = it->second;
    }
    if (!found) {
        error = "the socket is closed";
        return false;
    }
    auto* job = static_cast<SocketJob*>(found.get());
    {
        std::lock_guard<std::mutex> guard(job->m);
        if (job->finished || job->closing || job->cancelled.load()) {
            error = "the socket is closed";
            return false;
        }
        if (!job->opened) {
            error = "the socket isn't open yet (wait for its open event)";
            return false;
        }
        if (job->queued + data.size() > kMaxQueuedSend) {
            error = "too much waiting to be sent on this socket";
            return false;
        }
        job->queued += data.size();
        Outgoing item;
        item.binary = binary;
        item.data = std::move(data);
        job->out.push_back(std::move(item));
    }
    job->cv.notify_all();
    return true;
}

void close(Owner owner, Id id, int code, const std::string& reason)
{
    const auto state_of = find(owner);
    std::shared_ptr<Job> found;
    if (state_of) {
        std::lock_guard<std::mutex> guard(state_of->lock);
        const auto it = state_of->jobs.find(id);
        if (it != state_of->jobs.end() && it->second->socket)
            found = it->second;
    }
    if (!found)
        return;
    auto* job = static_cast<SocketJob*>(found.get());
    bool abort_now = false;
    {
        std::lock_guard<std::mutex> guard(job->m);
        if (job->closing || job->finished)
            return;
        job->closing = true;
        job->user_closed = true;
        if (!job->opened) {
            abort_now = true; // still connecting
        } else {
            Outgoing item;
            item.close = true;
            item.code = valid_close_code(code);
            item.data = clip_reason(reason);
            job->queued += item.data.size();
            job->out.push_back(std::move(item));
        }
    }
    if (abort_now)
        job->stop();
    else
        job->cv.notify_all();
}

void poll(Owner owner, std::vector<Event>& out, size_t max_events, size_t max_bytes)
{
    const auto state_of = find(owner);
    if (!state_of)
        return;
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> guard(state_of->lock);
        while (!state_of->events.empty() && out.size() < max_events && (bytes == 0 || bytes < max_bytes)) {
            Event& front = state_of->events.front();
            bytes += front.data.size();
            state_of->bytes -= front.data.size();
            out.push_back(std::move(front));
            state_of->events.pop_front();
        }
    }
    if (bytes > 0)
        state_of->room.notify_all();
}

Stats stats(Owner owner)
{
    Stats out;
    if (const auto state_of = find(owner)) {
        std::lock_guard<std::mutex> guard(state_of->lock);
        out.requests = state_of->requests;
        out.sockets = state_of->sockets;
        out.waiting = static_cast<int>(state_of->events.size());
    }
    return out;
}

int workers()
{
    State& s = state();
    std::lock_guard<std::mutex> guard(s.pool_lock);
    return s.threads;
}

void set_user_agent(const std::wstring& agent)
{
    std::lock_guard<std::mutex> guard(g_agent_lock);
    g_agent = agent;
}

} // namespace vanacore::net
