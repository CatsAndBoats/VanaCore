#include "log.hpp"

#include <windows.h>

#include <vanacore/console.hpp>
#include <vanacore/log_file.hpp>

#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>

namespace vanacore::log {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_enabled{false}; // open() has been called: [logging] enabled=1
LogFile g_file;
std::wstring g_install_dir; // where logs\ is, kept so logging can be turned on while the game runs
bool g_console = false; // mirror to the console (attached=1)

std::wstring format_message(const wchar_t* format, va_list args)
{
    wchar_t message[1024];
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    return message;
}

void emit(const std::wstring& message)
{
    const std::wstring line = g_file.write(message);
    OutputDebugStringW(line.c_str());
    if (g_console)
        write_console_line(GetStdHandle(STD_OUTPUT_HANDLE), L"VanaCore: " + message, kConsoleGrey);
}

} // namespace

void open(const wchar_t* install_dir)
{
    std::lock_guard lock(g_mutex);
    g_install_dir = install_dir != nullptr ? install_dir : L"";
    g_file.open(g_install_dir);
    g_enabled = true;
}

void install(const wchar_t* install_dir)
{
    std::lock_guard lock(g_mutex);
    g_install_dir = install_dir != nullptr ? install_dir : L"";
}

bool set_enabled(bool on)
{
    std::lock_guard lock(g_mutex);
    if (on == g_enabled.load(std::memory_order_relaxed))
        return true;
    if (on) {
        if (g_install_dir.empty())
            return false;
        g_file.open(g_install_dir);
        g_enabled = true;
        emit(L"Logging turned on while the game runs ([logging] enabled=1): lines from here on");
    } else {
        emit(L"Logging turned off while the game runs ([logging] enabled=0)");
        g_enabled = false;
        g_file.close();
    }
    return true;
}

bool enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void mirror_to_console()
{
    std::lock_guard lock(g_mutex);
    g_console = true;
}

std::atomic<void (*)(const std::wstring&)> g_tap{nullptr};

void write(const wchar_t* format, ...)
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return; // logging is off: nothing is formatted, written, printed or sent anywhere
    va_list args;
    va_start(args, format);
    const std::wstring message = format_message(format, args);
    va_end(args);
    std::lock_guard lock(g_mutex);
    emit(message);
    if (auto* tap = g_tap.load(std::memory_order_relaxed))
        tap(message);
}

void set_tap(void (*tap)(const std::wstring& line))
{
    g_tap.store(tap, std::memory_order_relaxed);
}

void write_from_crash(const wchar_t* format, ...)
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;
    va_list args;
    va_start(args, format);
    const std::wstring message = format_message(format, args);
    va_end(args);
    // The crash may have happened inside write(), holding the lock: don't wait for it.
    const bool locked = g_mutex.try_lock();
    emit(message);
    if (locked)
        g_mutex.unlock();
}

std::wstring file_stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u-%02u-%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return stamp;
}

Report::~Report()
{
    close();
}

bool Report::open(const std::wstring& logs_dir, const std::wstring& name, const std::wstring& stamp)
{
    close();
    if (logs_dir.empty())
        return false;
    CreateDirectoryW(logs_dir.c_str(), nullptr);
    path_ = logs_dir + name + L"-" + stamp + L".log";
    const HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    WriteFile(file, "\xEF\xBB\xBF", 3, &written, nullptr); // UTF-8 BOM, for Notepad and friends
    file_ = file;
    return true;
}

void Report::write(const std::string& line)
{
    if (file_ == nullptr)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[40];
    sprintf_s(stamp, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
              t.wMilliseconds);
    const std::string text = stamp + line + "\r\n";
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(file_), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

void Report::close()
{
    if (file_ != nullptr)
        CloseHandle(static_cast<HANDLE>(file_));
    file_ = nullptr;
}

} // namespace vanacore::log
