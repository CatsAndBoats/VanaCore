#pragma once

#include <windows.h>

#include <cstdio>
#include <string>

namespace vanacore {

inline constexpr WORD kConsoleYellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
inline constexpr WORD kConsoleMagenta = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
inline constexpr WORD kConsoleRed = FOREGROUND_RED | FOREGROUND_INTENSITY;
inline constexpr WORD kConsoleGrey = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

// SGR colour code for a console text attribute (foreground only).
inline int vt_colour(WORD attributes)
{
    int code = 30;
    if (attributes & FOREGROUND_RED)
        code += 1;
    if (attributes & FOREGROUND_GREEN)
        code += 2;
    if (attributes & FOREGROUND_BLUE)
        code += 4;
    return (attributes & FOREGROUND_INTENSITY) != 0 ? code + 60 : code;
}

// Writes one line in xiloader's style, "[MM/DD/YY hh:mm:ss] message", the stamp yellow and the
// message in `colour`. VanaCore.exe and the game share a console, so the line goes out in a single
// write (colours as VT escapes) and can't interleave with the other's output. Consoles without VT
// support get the colours as attributes; redirected output gets plain UTF-8.
inline void write_console_line(HANDLE out, const std::wstring& message, WORD colour)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf_s(stamp, L"[%02u/%02u/%02u %02u:%02u:%02u] ", t.wMonth, t.wDay, t.wYear % 100, t.wHour, t.wMinute,
               t.wSecond);
    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(out, &mode)) {
        if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
            SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            const std::wstring line = L"\x1b[" + std::to_wstring(vt_colour(kConsoleYellow)) + L"m" + stamp + L"\x1b[" +
                                      std::to_wstring(vt_colour(colour)) + L"m" + message + L"\x1b[0m\n";
            WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
            return;
        }
        CONSOLE_SCREEN_BUFFER_INFO info{};
        GetConsoleScreenBufferInfo(out, &info);
        SetConsoleTextAttribute(out, kConsoleYellow);
        WriteConsoleW(out, stamp, static_cast<DWORD>(wcslen(stamp)), &written, nullptr);
        SetConsoleTextAttribute(out, colour);
        const std::wstring rest = message + L"\n";
        WriteConsoleW(out, rest.c_str(), static_cast<DWORD>(rest.size()), &written, nullptr);
        SetConsoleTextAttribute(out, info.wAttributes);
        return;
    }
    const std::wstring line = stamp + message + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr,
                                          nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

} // namespace vanacore
