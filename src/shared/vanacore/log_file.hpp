#pragma once

#include <windows.h>

#include <cstdio>
#include <string>

namespace vanacore {

// <install dir>\logs\vanacore.log, shared by VanaCore.exe and VanaCore.Core (in the game). It's
// opened for appending only and every line goes out in one unbuffered write, so lines from both
// processes land whole, in order, and are on disk even if the process dies straight after.
class LogFile {
public:
    void open(const std::wstring& install_dir)
    {
        if (file_ != INVALID_HANDLE_VALUE)
            return;
        const std::wstring dir = install_dir + L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        // Logging is on by default and the file is only ever appended to: past 8 MB, this start renames the old log
        // to vanacore-<date time>.log (nothing is overwritten or deleted) and begins a fresh vanacore.log. If the
        // rename fails, the log just keeps growing.
        WIN32_FILE_ATTRIBUTE_DATA existing{};
        if (GetFileAttributesExW((dir + L"\\vanacore.log").c_str(), GetFileExInfoStandard, &existing) &&
            ((static_cast<unsigned long long>(existing.nFileSizeHigh) << 32) | existing.nFileSizeLow) > 8ull * 1024 * 1024) {
            SYSTEMTIME now;
            GetLocalTime(&now);
            wchar_t stamp[40];
            swprintf_s(stamp, L"\\vanacore-%04u-%02u-%02u-%02u%02u%02u.log", now.wYear, now.wMonth, now.wDay, now.wHour,
                       now.wMinute, now.wSecond);
            MoveFileExW((dir + L"\\vanacore.log").c_str(), (dir + stamp).c_str(), 0);
        }
        file_ = CreateFileW((dir + L"\\vanacore.log").c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        DWORD written = 0;
        if (file_ != INVALID_HANDLE_VALUE && GetFileSizeEx(file_, &size) && size.QuadPart == 0)
            WriteFile(file_, "\xEF\xBB\xBF", 3, &written, nullptr); // UTF-8 BOM, for Notepad and friends
    }

    // Closes the file (logging turned off while the process runs); open() makes it again.
    void close()
    {
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }

    // Writes "YYYY-MM-DD hh:mm:ss.mmm message" and returns the line (with its newline).
    std::wstring write(const std::wstring& message)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[32];
        swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                   t.wSecond, t.wMilliseconds);
        const std::wstring line = stamp + message + L"\n";
        if (file_ != INVALID_HANDLE_VALUE) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0,
                                                  nullptr, nullptr);
            std::string utf8(bytes, '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr,
                                nullptr);
            DWORD written = 0;
            WriteFile(file_, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        return line;
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

// "FFXiMain.dll+0x89EF0" for a code address, or the bare address outside any module.
inline std::wstring describe_address(const void* address)
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    wchar_t text[MAX_PATH + 32];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH) != 0) {
        const wchar_t* name = wcsrchr(path, L'\\');
        swprintf_s(text, L"%ls+0x%X", name != nullptr ? name + 1 : path,
                   static_cast<unsigned>(static_cast<const BYTE*>(address) - reinterpret_cast<const BYTE*>(module)));
    } else {
        swprintf_s(text, L"0x%p", address);
    }
    return text;
}

// A readable name for the exception or exit codes a crash leaves behind.
inline const wchar_t* describe_status(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return L"access violation";
    case EXCEPTION_STACK_OVERFLOW: return L"stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return L"illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return L"privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return L"in-page error";
    case STATUS_HEAP_CORRUPTION: return L"heap corruption";
    case STATUS_STACK_BUFFER_OVERRUN: return L"stack buffer overrun (security check)";
    case 0xC0000142: return L"DLL initialisation failed";
    case 0xE06D7363: return L"unhandled C++ exception";
    default: return nullptr;
    }
}

} // namespace vanacore
