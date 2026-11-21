#pragma once

#include <string>

// [graphics] renderer=d3d9: the game's Direct3D 8 on Direct3D 9. The game, VanaCore's overlay and
// addons, screenshots and plugins all keep talking Direct3D 8; each call goes to its Direct3D 9
// counterpart (crosire's d3d8to9 does the same; this is written afresh). d3d9.dll is loaded by name,
// so one in the game's folder (ReShade's, DXVK's) comes before Windows' own. No scene effects: the
// game is drawn as it draws itself.
namespace vanacore::d3d9 {

// Direct3DCreate8 for the game: an IDirect3D8 (a COM pointer) on Direct3D 9, or nullptr when there's no
// Direct3D 9. vsync: Present waits for the display (read when the device is made, and at each Reset).
void* direct3d_create(bool vsync);

// [graphics] vsync changed while the game runs: from the next Reset (a full-screen game's return from
// Alt+Tab), else the next start.
void set_vsync(bool on);

// The graphics card Direct3D 9 runs on (UTF-8), "" before direct3d_create.
std::string adapter();

// Where d3d9.dll was loaded from, "" before direct3d_create.
std::wstring library();

} // namespace vanacore::d3d9
