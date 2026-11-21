#pragma once

#include <string>
#include <string_view>
#include <vector>

// Fonts of VanaCore's own, in <VanaCore>\fonts\: loaded into the game process only (nothing is
// installed in Windows), so any GDI text in it can use them by family name: VanaCore's overlay, its
// addons' vana.ui text, and plugins' font objects. .ttf, .otf and .ttc load as they are; .woff and
// .woff2 are unpacked with DirectWrite first. VanaCore ships Material Symbols Outlined (Google's icon
// font, Apache 2.0): an icon is a character of it, found by its name (icon()).
namespace vanacore::fonts {

// Loads <install_dir>\fonts\*. Once, at start, before anything draws.
void install(const wchar_t* install_dir);

// An icon font's character for `name` ("undo", "close", "settings": the names on
// fonts.google.com/icons), from the fonts' own ligatures (the name typed in the font turns into the
// icon); 0 when no loaded font has it. `family` gets the font to draw it with ("Material Symbols
// Outlined").
char32_t icon(std::string_view name, std::wstring* family = nullptr);

// A font file picked from anywhere (/vana's Browse): copied into <VanaCore>\fonts\ (a file of that name there is kept as it
// is), so it loads at every start, and loaded now. Its family into `family`; false, and why, when it can't be.
bool add(const std::wstring& path, std::wstring& family, std::wstring& why);

// The families of the fonts in <VanaCore>\fonts\ that are letters (not icon fonts), in the folder's order.
std::vector<std::wstring> families();

// A font added to <VanaCore>\fonts\ taken out (/vana's delete icon): unloaded now, its file moved to the Recycle Bin so it
// doesn't load at the next start. Icon fonts stay. False, and why, when it can't be.
bool remove(const std::wstring& family, std::wstring& why);

} // namespace vanacore::fonts
