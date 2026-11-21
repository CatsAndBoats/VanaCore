#include "playonline.hpp"

#include <windows.h>

#include <initializer_list>
#include <utility>

namespace vanacore::playonline {
namespace {

const wchar_t* key_of(Region region)
{
    switch (region) {
    case Region::JP:
        return L"SOFTWARE\\PlayOnline\\InstallFolder";
    case Region::EU:
        return L"SOFTWARE\\PlayOnlineEU\\InstallFolder";
    default:
        return L"SOFTWARE\\PlayOnlineUS\\InstallFolder";
    }
}

// A string value of an install key (32-bit view: PlayOnline is a 32-bit program), no trailing backslash; empty if it
// isn't there
std::wstring read(Region region, const wchar_t* value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_of(region), 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS)
        return {};
    wchar_t text[MAX_PATH] = {};
    DWORD size = sizeof(text) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(text), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    std::wstring path(text);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

bool is_file(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

} // namespace

const wchar_t* name(Region region)
{
    return region == Region::JP ? L"JP" : region == Region::EU ? L"EU" : L"US";
}

bool installed(Region region, Install& out)
{
    std::wstring pol = read(region, L"1000");
    std::wstring ffxi = read(region, L"0001");
    if (pol.empty() || ffxi.empty() || !is_file(pol + L"\\pol.exe"))
        return false;
    out.region = region;
    out.pol = std::move(pol);
    out.ffxi = std::move(ffxi);
    return true;
}

bool find(const std::wstring& choice, Install& out, std::wstring& error)
{
    for (const Region region : {Region::JP, Region::US, Region::EU}) {
        if (_wcsicmp(choice.c_str(), name(region)) != 0)
            continue;
        if (installed(region, out))
            return true;
        error = std::wstring(L"PlayOnline ") + name(region) + L" isn't installed (HKLM\\" + key_of(region) +
                L", values 1000 and 0001, and pol.exe in the first)";
        return false;
    }
    if (!choice.empty() && _wcsicmp(choice.c_str(), L"auto") != 0) {
        error = L"[client] retail.region=" + choice + L" isn't jp, us, eu or auto";
        return false;
    }
    for (const Region region : {Region::US, Region::EU, Region::JP}) {
        if (installed(region, out))
            return true;
    }
    error = L"no PlayOnline install is recorded in the registry (HKLM\\SOFTWARE\\PlayOnline[US|EU]\\InstallFolder, values 1000 and "
            L"0001). Install FINAL FANTASY XI, or set [client] retail.pol= to PlayOnline's folder.";
    return false;
}

} // namespace vanacore::playonline
