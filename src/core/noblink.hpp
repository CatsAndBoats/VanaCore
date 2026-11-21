#pragma once

// [client] blink.disable: your own character stays as it is when its gear changes. The game rebuilds a character's
// whole model on every gear change (it deletes the actor and sets a new one up, so the character vanishes for a moment
// while the parts load: the blink, however often GearSwap changes gear). With this on, a gear change is left out of that
// rebuild for the local character: the look in the game's data changes, but the model on screen stays until the option
// is turned off (the model is then rebuilt once), the character zones, or its hair, face or race changes. Other
// characters are untouched. A change of the main or sub weapon still blinks it, as the game does, unless [client]
// blink.weapons is on as well.
namespace vanacore::noblink {

// `ini` (vanacore.ini): [client] blink.disable and blink.weapons read now, and again by reload whenever vanacore.ini changes.
void install(const wchar_t* ini);
void reload(const wchar_t* ini);

// Once a frame on the game's thread: hooks the game's model rebuild step the first time it's wanted (once FFXiMain.dll
// is unpacked).
void on_frame();

} // namespace vanacore::noblink
