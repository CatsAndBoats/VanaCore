#pragma once

#include <windows.h>

// The chat input line in True Font (input.font) as one picture: the text the game shows in the line laid out whole by the
// font (DirectWrite: each letter where the font puts it, kerned, between whole pixels), not a picture per letter on its own
// whole pixel at the game's place for its word, and a thin caret of VanaCore's that stays lit while you type and blinks at
// Windows' caret rate, in place of the game's '|' (a letter, blinking every 25 frames). ñ, é, ¿ and the rest of the game's own
// row for them (gametext.hpp) are the font's too. A line with an auto-translate phrase or an icon in it stays as it was: each
// letter where the game's is.
namespace vanacore::chatline {

// FFXiMain.dll's chat line draw (FsTextInputDisplay's) and its text draw hooked; called once the module is there (chatfont.cpp)
void install(HMODULE game);

} // namespace vanacore::chatline
