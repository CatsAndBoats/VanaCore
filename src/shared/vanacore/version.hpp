#pragma once
// VanaCore's version. release.yaml sets both lines; VanaCore.Core.rc puts the second in VanaCore.Core.dll's ProductVersion
// (what VanaCoreApp shows of the installed VanaCore). The static_assert holds them the same.

#define VANACORE_VERSION_TEXT "0.9.12"

#ifndef RC_INVOKED
namespace vanacore {

inline constexpr wchar_t kVersion[] = L"0.9.12";

constexpr bool same_version(const wchar_t* a, const char* b)
{
    for (; *a == static_cast<wchar_t>(*b); ++a, ++b)
        if (*a == L'\0')
            return true;
    return false;
}
static_assert(same_version(kVersion, VANACORE_VERSION_TEXT), "version.hpp: kVersion and VANACORE_VERSION_TEXT differ");

} // namespace vanacore
#endif
