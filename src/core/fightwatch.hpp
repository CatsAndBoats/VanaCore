#pragma once

// The fight watch: lines starting "Fight watch:" in vanacore.log while an Aern (Ul'aern, Om'aern, ...: a monster whose
// name ends in 'aern) is within 50 yalms, for the reports of the whole world washing out white or pink while fighting one
// in Al'Taieu and coming back after the fight (Trence 2026-11-22, Vitali 2026-11-26). Logged:
//  - when one comes near: which, where, the renderer and upscaler as they are, and the [graphics] and
//    [graphics.effects] keys the settings set;
//  - the frame's lighting as the game's lit draws were given it (gfx d3d8_fightwatch.h): its ambient, fog and sun, the
//    brightest a draw's light added up to, and each distinct light; when it changes, else every 3 seconds;
//  - the scene's colour (the 3D scene alone, as the frame shows it) every 2 seconds: its mean and how much of it is near
//    white;
//  - the first time a fight's scene looks washed out: a screenshot (as Print Screen's, without VanaCore's drawing) and the
//    draw log of one frame (logs\fightwatch-<date>-<time>-draws.txt, each lit draw with its lights);
//  - the Aern's HP and status as they change, and when it's gone (15 s without one near).
// Nothing at all while [logging] enabled is off. At most 800 lines a session. Game thread only.
namespace vanacore::fightwatch {

// Once a frame (overlay.cpp's Present).
void on_frame();

} // namespace vanacore::fightwatch
