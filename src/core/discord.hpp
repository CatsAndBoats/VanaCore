#pragma once

// [discord]: Discord Rich Presence, the character, its jobs and its zone on the player's Discord profile (from XI on
// Anything, rubymatrix/xi-on-anything, host/discord.c). No SDK: the Discord app listens on \\.\pipe\discord-ipc-0 to 9
// for frames of a u32 opcode, a u32 length and JSON. Opcode 0 is the handshake ({"v":1,"client_id":...}), 1 a command
// (SET_ACTIVITY here) or Discord's answer, 2 a close. A thread of its own talks to Discord, so the game's thread never
// waits on the pipe: it looks for Discord every 15 s, and sends an activity only when it changes, at most every 5 s
// (Discord takes about 5 in 20 s).
namespace vanacore::discord {

// `ini` (vanacore.ini): [discord] enabled and name, read now, and again by reload whenever vanacore.ini changes. enabled
// is on by default; 0 clears the activity and lets go of Discord. The application is CatsEyeXI's, fixed.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: every 2 s while Discord is connected, the character, its jobs and its zone handed
// to the thread (every 15 s while it isn't, only who is logged in, for the start time). Nothing with enabled=0.
void on_frame();

} // namespace vanacore::discord
