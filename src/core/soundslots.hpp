#pragma once

// 100 sound slots, and [ffxi] sound.max_sounds up to 100, changed as the game runs. FFXI plays at most max sounds at
// once (0029, a cap it checks before each sound), but has only 20 slots (one per sound playing) whatever the cap says,
// the 20 written into its code: the slots' allocation and each walk over them. The walks and the allocation are
// rewritten to 100, and the 20 slots the game has made are swapped for 100, just before its sound thread starts
// (FFXiMain.dll's CreateThread): its code is decrypted by then and no other thread uses the slots yet. Max sounds stays
// what's asked for (the game's 12 or 20 as before), so the slots past it go unused; reload() changes it while the game
// runs. The cap's own forced 20 (for a while, whatever the setting) follows it, never under 20.
//
// Whatever max sounds is: a sound that couldn't start gets the handle -1, which the sound thread takes as every sound;
// a world sound's position and stop send their kept handle as it is, so one that never started moved or faded out every
// sound playing (an AoE weaponskill on a pack). They send 0, no sound, for it instead.
namespace vanacore::soundslots {

// From VanaCoreInstall, after registry::install (which knows sound.max_sounds), before FFXiMain.dll loads.
void install();

// [ffxi] sound.max_sounds (or [ffxi.registry] 0029) as `ini` has it now, given to the game as its max sounds once the
// slots are made, up to 100; logged when it changes.
void reload(const wchar_t* ini);

} // namespace vanacore::soundslots
