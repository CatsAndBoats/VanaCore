#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// FFXI's chat: the command line (what's typed into the chat box, and what macros and menus run)
// and the chat log. Everything here runs on the game's thread.
namespace vanacore::chat {

// The chat mode (colour) VanaCore's own lines and addons' print() use.
inline constexpr int kDefaultMode = 1;

// FFXI's inline text colours: "\x1E" and a colour number colour the text after them.
std::string colour(int code, std::string_view text);
// "[VanaCore] " in the colours addons use for their own name.
std::string header(std::string_view name = "VanaCore");
inline constexpr int kError = 68;   // red
inline constexpr int kSuccess = 2;  // green
inline constexpr int kWarning = 104; // yellow
inline constexpr int kMessage = 106; // cream

// [client] chatlog=1 in `ini`: every line that reaches the chat log is also appended to
// <install_dir>\logs\chatlog.<character name>.<yyyy-mm-dd>.log. Call from VanaCoreInstall.
void install_log(const wchar_t* install_dir, const wchar_t* ini);

// Called with each command line before the game runs it: typed into the chat box (mode 1), run by
// a macro or a menu, or queued with run() (`injected`). Returns true when it's handled, and the
// game then never sees it.
using CommandHandler = bool (*)(const std::string& line, int mode, bool injected);
void set_command_handler(CommandHandler handler);

// A line of text: one coming into the chat log (incoming: every line the game writes there, VanaCore's own too), or one
// going to the game (outgoing: typed or run, and no command took it). A handler may change it (`modified`, as it starts
// the same as `message`; incoming, its mode and indent too) or block it. `injected`: VanaCore's own (a line it writes,
// a command it runs). Incoming, `mode` is the game's three mode bytes (mode | flags << 8 | ... << 16, as Ashita's e.mode);
// the chat mode itself is mode & 0xFF.
struct Text {
    int mode = 0;
    bool indent = false;
    std::string message;
    int mode_modified = 0;
    bool indent_modified = false;
    std::string modified;
    bool injected = false;
};
using TextHandler = bool (*)(bool incoming, Text& text); // true: blocked
void set_text_handler(TextHandler handler);

// Adds `text` to the chat log in chat mode `mode`'s colour, a line per "\n" (long lines are split).
// Lines wait until the game's chat log exists. Also written to VanaCore's log, as from `source`
// (e.g. the addon's name).
void write(int mode, std::string_view text, std::string_view source = "Chat");

// A line for the player alone: "[VanaCore] text" in the default colour, added to their own chat log (what /echo does).
// It's never sent to the server, a party or another player, and it isn't run as a command, so no addon's command event
// sees it either. What VanaCore tells the player itself (/vana debug, /vana abtest) goes through this, never /say.
void echo(std::string_view text, std::string_view source = "VanaCore");

// Runs `line` as if it were typed (mode 1), on the next frame.
void run(int mode, std::string line);

// Runs `line` now, as injected (see set_command_handler), or on the next frame when a command is
// being handled already.
void execute(int mode, const std::string& line);

// Once per frame, from Present: the first time, finds FFXI's chat functions in FFXiMain.dll (its
// code is decrypted by then) and hooks the command line; then adds waiting lines to the chat log
// and runs queued commands.
void on_frame();

// True once the command line is hooked (commands can be typed).
bool ready();

// The chat log's row spacing in the UI's units ([client] chat.lineheight; 16, the game's, until it's applied)
int line_height();

// For menulog.cpp: the selection menu's window VanaCore keeps ([client] menu.rows; null before one opens; kept after the
// menu closes), the rows it last gave a menu, the setting, and how long ago that window was last drawn as one (ms)
void menu_state(const void*& window, int& rows, int& setting, unsigned long long& since_ms);

// A chat window's place as the /vana window's slider is dragged ([client] chat.x, chat.y, chat.width and chat2.*; compass.x/y,
// clock.x/y, menus.x/y, input.x/y), put on screen at the next frame without vanacore.ini (the window writes that when it's let go)
void preview(const std::string& key, double value);

// In ImGui's frame (vana.layout.outline): window `which`'s place drawn as a box (outline.hpp), 0 the main chat window, 1
// the second, 2 the input line, 3 the compass, 4 the menus over the chat (where the next one goes, at the size of the last),
// 5 the clock under the compass;
// where it isn't open, where its settings put it. Shift + drag moves it (its move switch on,
// X and Y as it goes, not written): the frame it's let go, moved_x and moved_y are where it was left, for the caller to save.
bool outline(int which, double* moved_x = nullptr, double* moved_y = nullptr);

// The chat window the game stands the compass and the menus over the chat on (log 2 when it's shown, else log 1): its
// frame as drawn (left, top, right, bottom, the interface's units). False while neither is shown.
bool top_chat(short* rect);

// Whether window `ctrl` is one of the chat logs' (log 1 or 2, while shown)
bool log_window(const void* ctrl);

// The chat line (what's being typed): whether it's open, and its text as the game keeps it
// (Shift-JIS, with auto-translate phrases and colour codes). False if the game's input line
// wasn't found.
bool input_line(bool& open, std::string& text);

// The chat line's state as the game keeps it (the chat manager's +0xF108): 4 while the chat line is open, 5 while
// another text field is (a bazaar or search comment), 1 to 3 closed; 0 if the chat manager isn't found.
std::uint32_t input_state();

// `text` with its auto-translate phrases spelled out, by the game's own parser (the chat line's, FFXiMain.dll's
// FUN_101ab0f0); `brackets`: inside the game's green brackets (EF 27 ... EF 28). The text as it was if the parser
// can't be found.
std::string parse_auto_translate(std::string_view text, bool brackets);

// The game's chat manager object, whose chat line is at +0x7E30 (0 if it isn't found or made yet).
std::uintptr_t input_object();
// True while an addon shows the chat line its own way and the game's is kept off the screen by itself: fancychat's input
// bar (its Chat Window > Enable input chat bar), as asked twice a second. Not [client] input.hide, which has no bar to go by.
bool input_bar_addon();
// True while fancychat keeps the game's chat windows closed (its own chat is up and not shown beside the legacy one), as
// asked twice a second. Not [client] chat.hide, which is the player's own.
bool chat_addon_pinned();
// True while the game's windows are laid over fancychat's chat and the windows of addons and plugins that ask
// ([client] menus.ontop on, the window draw hooked and the interface's area known), as of the last half second.
bool raising();
// The bottom of the game's interface area (where its windows are laid), in its units; 0 while it isn't known.
int ui_bottom();

} // namespace vanacore::chat
