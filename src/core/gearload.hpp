#pragma once

// The game's gear parts that don't load: a PC-look character's slot (usually the body) the game gives up on, so it's drawn
// without it (a floating head) until its look is set up again. Each is logged in logs\vanacore.log as "Gear: ..." with
// why, the DAT and the game's resource memory at that moment. Logging only; nothing in the game changes.
namespace vanacore::gearload {

// Hooks the game's gear part loads, the first frame FFXiMain.dll is there (once).
void on_frame();

} // namespace vanacore::gearload
