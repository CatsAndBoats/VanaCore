#pragma once

// Two games from one FFXI install no longer share their chat logs. The log windows keep older lines in page files in the
// install's TEMP folder (TEMP\1_0.log to 1_19.log for the first window, 2_0.log... for the second, 20 pages of 50 lines
// each), read back when the log is scrolled up. The names are the same for every copy of the game, so a second game
// writes over the first one's pages: scrolling up showed the other character's chat. VanaCore names the pages after the
// process (TEMP\1_0.<process id>.log) and deletes those of games that have ended.
//
// The game keeps 20 pages (1,000 lines) a window; VanaCore keeps as many as [client] chat.scrollback asks for, up to 250
// (12,500 lines), files 1_0 to 1_249.
namespace vanacore::chatpages {

// Once a frame on the game's thread: patches the page names the first time FFXiMain.dll is there.
void on_frame();

// [client] chat.scrollback: the lines the log windows keep (the pages for them, never fewer than the game's 20). On (above
// 0), every log tab scrolls back through its own lines, not only the Window tabs.
void set_lines(int lines);

// The log's fetch of an older line (0x1012A190, the chat object `ring`, the tab's `kind`): known_no_older says the last
// fetch from these same rows found none (so another would look back through every saved line again for nothing);
// note_older records what a fetch found.
bool known_no_older(const void* ring, int kind);
void note_older(const void* ring, int kind, bool found);

} // namespace vanacore::chatpages
