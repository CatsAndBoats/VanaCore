# soundtest: a harness for dsound_follow.cpp

Runs VanaCore's DirectSound wrapper (`src\core\dsound_follow.cpp`, what `[client] sound.follow` hooks into the game) over real
DirectSound, calling it the way `FFXiMain.dll` does: a `DirectSoundCreate8` on the default output, `SetCooperativeLevel`
(PRIORITY), a 0x30000-byte looping buffer made with the game's description (0x24 bytes, flags 0x500c0, freed after the call),
`QueryInterface(IDirectSoundBuffer8)`, writes through `Lock`/`Unlock`, `SetVolume`/`SetPan`/`SetFrequency`, `Play(LOOPING)`.

```
build.bat            :: makes harness.exe (Visual Studio 2022, x86)
harness.exe          :: plays a quiet tone for ~2 s and moves the device to the default output
harness.exe --flip   :: also switches Windows' default output to another active device and back (changes sound settings)
```

- Without `--flip`: `rebuild_now()` moves the device by hand, and the checks are that the buffer is still playing and looping,
  its play position carried on, its volume, pan and pitch and its contents (the first tone and the quarter rewritten after
  `Play`) are as they were, and the new buffer's position moves.
- With `--flip`: the move comes by itself from Windows' notification when the default output changes, both ways. Needs a
  second active output.
- Exit code 0: everything passed.

The wrapper has not been run against a real unplugged device here (that is what `--flip` stands in for): a headset that
resets its driver may answer differently, and the game's log (`logs\vanacore.log`, "Sound:" lines) says what it returned.
