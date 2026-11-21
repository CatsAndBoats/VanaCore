#pragma once

// [client] sound.follow: the game's sound follows Windows' default output. FFXI opens DirectSound on the default output when
// it starts and never again, so after headphones are swapped for speakers (or a headset that disconnects and resets its
// driver) it is silent until the game is started again. With this on, the game's DirectSoundCreate8 is VanaCore's
// (dsound_follow.cpp): when the default output changes, or the one in use goes away or comes back, every sound buffer is
// moved to the new output and carries on where it was. On unless vanacore.ini turns it off (and on Wine, where it is
// off unless it turns it on). Read when the game starts.
namespace vanacore::sound {

// `ini` (vanacore.ini): from VanaCoreInstall, before FFXiMain.dll loads.
void install(const wchar_t* ini);

} // namespace vanacore::sound
