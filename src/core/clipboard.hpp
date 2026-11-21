#pragma once

#include <windows.h>

#include <string>

// Copy and paste for FFXI's chat line (window.hpp calls these for Ctrl+C and Ctrl+V).
namespace vanacore::clipboard {

// Puts the clipboard's text (its first line in Shift-JIS, up to the chat line's 120 bytes) into the chat
// line at its caret, if it's open. True if the line was open (the key is the paste's).
bool paste(HWND window);

// Copies the chat line's text to the clipboard, if it's open and has any. True if it did.
bool copy(HWND window);

// The clipboard's text, and text put on it (`window` its owner), in the game's Shift-JIS: vana.system.clipboard and
// set_clipboard (Windower addons' get_from_clipboard and copy_to_clipboard).
std::string get_text(HWND window);
bool set_text(HWND window, const std::string& text);

} // namespace vanacore::clipboard
