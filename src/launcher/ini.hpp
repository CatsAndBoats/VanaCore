#pragma once

#include <windows.h>

#include <string>

namespace vanacore {

inline std::wstring ini_read(const std::wstring& ini, const wchar_t* section, const wchar_t* key,
                             const wchar_t* fallback = L"")
{
    std::wstring value(4096, L'\0');
    const DWORD length = GetPrivateProfileStringW(section, key, fallback, value.data(),
                                                  static_cast<DWORD>(value.size()), ini.c_str());
    value.resize(length);
    return value;
}

} // namespace vanacore
