#pragma once

#include <string>

namespace vanacore::playonline {

enum class Region { JP, US, EU };

// A PlayOnline install as its setup recorded it in the registry.
struct Install {
    Region region = Region::US;
    std::wstring pol;  // PlayOnline's folder (the one with pol.exe), no trailing backslash
    std::wstring ffxi; // FINAL FANTASY XI's folder, no trailing backslash
};

// "JP", "US" or "EU"
const wchar_t* name(Region region);

// `region`'s install: HKLM\SOFTWARE\PlayOnline[US|EU]\InstallFolder in the 32-bit view (JP's key has no letters), value 1000
// PlayOnline's folder and 0001 FFXI's (the way Windower finds them). False if the registry hasn't both, or pol.exe isn't in
// the first.
bool installed(Region region, Install& out);

// The install `choice` names (jp, us or eu), or with it empty or "auto" the first the registry has, US then EU then JP.
// False, with `error` saying what's missing, if there isn't one.
bool find(const std::wstring& choice, Install& out, std::wstring& error);

} // namespace vanacore::playonline
