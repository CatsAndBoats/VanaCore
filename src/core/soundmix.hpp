#pragma once

// World and interface sounds' own volumes, under the game's Sound Effects Volume (Config > Gameplay's extended rows,
// gamesettings.cpp). The game plays every sound effect through one request (FFXiMain 2025-12-26 0x10036D60, __thiscall:
// its parameters, the volume 0 to 127) and sets a playing one's volume through another (0x10038280, __cdecl: the sound,
// the volume, the time it takes); its menus' sounds (the cursor, select, cancel, error...: fourteen functions 0x60 apart
// from 0x10037940, each a sound of its own) go through a third (0x10036D10, __thiscall: the pan, the volume) that makes
// the request. Which sound a call is comes from where it's called from, as By-T0r's VanaSound (MIT) has it:
//   - interface: the fourteen menu sounds' calls of the third;
//   - world: the two requests the game makes for the sounds of the world and of battle (the calls whose results are kept at
//     +0x1E0 of the sound's owner, and the event's sound), and the volume set on the first of them.
// A volume is scaled as VanaSound scales it: (volume x share + 50) / 100, 0 to 127. Other sounds (music, and sound effects
// played from elsewhere) are left alone. All three are found by their code and checked before anything is hooked: on
// another build nothing is.
namespace vanacore::soundmix {

// `world` and `ui`: 0 to 100, 100 as the game plays them. The hooks go in the first time either is under 100 (on the game's
// thread, FFXiMain.dll loaded); both at 100 they pass every call through.
void set(int world, int ui);

} // namespace vanacore::soundmix
