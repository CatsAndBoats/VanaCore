#pragma once

// [ffxi] sound.max_sounds above 20: the game gets that many sound slots. FFXI plays at most max sounds at once (0029, a
// cap it checks before each sound), but has only 20 slots (one per sound playing) whatever the cap says, the 20 written
// into its code: the slots' allocation and each walk over them. With more asked for (up to 100), the walks and the
// allocation are rewritten to that many, and the 20 slots the game has made are swapped for that many, just before its
// sound thread starts (FFXiMain.dll's CreateThread): its code is decrypted by then and no other thread uses the slots
// yet. 20 or less leaves the slots as they are. Read when the game starts.
//
// Whatever max sounds is: a sound that couldn't start gets the handle -1, which the sound thread takes as every sound;
// a world sound's position and stop send their kept handle as it is, so one that never started moved or faded out every
// sound playing (an AoE weaponskill on a pack). They send 0, no sound, for it instead.
namespace vanacore::soundslots {

// From VanaCoreInstall, after registry::install (which knows sound.max_sounds), before FFXiMain.dll loads.
void install();

} // namespace vanacore::soundslots
