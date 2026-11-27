#pragma once

// The folders a player copies addons and plugins into (addons\ashita, addons\windower, plugins\ashita,
// plugins\windower), made at start if they aren't there, each with a README.txt saying what goes in it (written
// when the folder has none), so a new install shows where things go.
namespace vanacore::folders {

void install(const wchar_t* install_dir);

} // namespace vanacore::folders
