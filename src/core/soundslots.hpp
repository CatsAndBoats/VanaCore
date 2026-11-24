#pragma once

// [ffxi] sound.max_sounds above 20: the game gets that many sound slots. FFXI plays at most max sounds at once (0029, a
// cap it checks before each sound), but has only 20 slots (one per sound playing) whatever the cap says, the 20 written
// into its code: the slots' allocation and each walk over them. With more asked for (up to 100), the walks and the
// allocation are rewritten to that many, and the 20 slots the game has made are swapped for that many, just before its
// sound thread starts (FFXiMain.dll's CreateThread): its code is decrypted by then and no other thread uses the slots
// yet. 20 or less leaves the game as it is. Read when the game starts.
namespace vanacore::soundslots {

// From VanaCoreInstall, after registry::install (which knows sound.max_sounds), before FFXiMain.dll loads.
void install();

} // namespace vanacore::soundslots
