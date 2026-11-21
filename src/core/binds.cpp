#include "binds.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include "addons.hpp"
#include "chat.hpp"
#include "log.hpp"
#include "window.hpp"

namespace vanacore::binds {
namespace {

enum class When { Always, ChatClosed, ChatOpen };

struct Bind {
    unsigned key = 0;       // a virtual-key code (kNumpadEnter for the numpad's Enter)
    unsigned modifiers = 0; // held with it, exactly
    bool up = false;        // on its release (else its press)
    When when = When::Always;
    std::string name;    // the key as given (for listing)
    std::string command; // what runs
    ULONGLONG fired = 0; // when it last ran (GetTickCount64), for the keybinds window
};

// Key capture, for the keybinds window's "press a key": the next key pressed (with the modifiers held) is the answer and
// goes to no one else
struct Capture {
    bool active = false;
    bool done = false;
    bool cancelled = false; // Escape alone
    ULONGLONG started = 0;  // GetTickCount64: a capture nobody ends lets go of the keys after 30 seconds
    unsigned key = 0;
    unsigned modifiers = 0;
};
Capture g_capture;

std::mutex g_lock;
std::vector<Bind> g_binds;
std::vector<Bind> g_base;           // the binds there were when scripts\keybinds.txt first replayed (boot.txt's and the addons')
bool g_base_known = false;          // apply_file ran: g_base is that snapshot
std::vector<std::string> g_pending; // commands the hooks' thread took, for the game's (take_win_key, take_wheel)
int g_wheel_turned[2] = {};         // a bound wheel's turn short of a notch (WHEEL_DELTA), up/down and left/right (take_wheel)
std::atomic<bool> g_block_during_input{true}; // keyboard.blockbindsduringinput
bool g_silent = false;            // keyboard.silentbinds

struct KeyName {
    const char* name;
    unsigned key;
};

// Ashita's names (DirectInput's, without DIK_) and Windower's, either case; the mouse's buttons by Windows' names (VK_
// without VK_) and as mouse1-5 (the side buttons as back and forward too), and its wheel
constexpr KeyName kNames[] = {
    {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
    {"insert", VK_INSERT}, {"ins", VK_INSERT}, {"delete", VK_DELETE}, {"del", VK_DELETE},
    {"home", VK_HOME}, {"end", VK_END},
    {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR}, {"prior", VK_PRIOR}, {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT}, {"next", VK_NEXT},
    {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"enter", VK_RETURN}, {"return", VK_RETURN},
    {"space", VK_SPACE}, {"tab", VK_TAB}, {"backspace", VK_BACK}, {"back", VK_BACK},
    {"capslock", VK_CAPITAL}, {"capital", VK_CAPITAL}, {"numlock", VK_NUMLOCK},
    {"scrolllock", VK_SCROLL}, {"scroll", VK_SCROLL}, {"pause", VK_PAUSE},
    {"printscreen", VK_SNAPSHOT}, {"prtsc", VK_SNAPSHOT}, {"sysrq", VK_SNAPSHOT}, {"snapshot", VK_SNAPSHOT},
    {"apps", VK_APPS}, {"lwin", VK_LWIN}, {"rwin", VK_RWIN},
    {"numpad*", VK_MULTIPLY}, {"multiply", VK_MULTIPLY}, {"numpadstar", VK_MULTIPLY},
    {"numpad+", VK_ADD}, {"add", VK_ADD}, {"numpadplus", VK_ADD},
    {"numpad-", VK_SUBTRACT}, {"subtract", VK_SUBTRACT}, {"numpadminus", VK_SUBTRACT},
    {"numpad.", VK_DECIMAL}, {"decimal", VK_DECIMAL}, {"numpadperiod", VK_DECIMAL},
    {"numpad/", VK_DIVIDE}, {"divide", VK_DIVIDE}, {"numpadslash", VK_DIVIDE},
    {"numpadenter", kNumpadEnter},
    {"`", VK_OEM_3}, {"grave", VK_OEM_3}, {"-", VK_OEM_MINUS}, {"minus", VK_OEM_MINUS},
    {"=", VK_OEM_PLUS}, {"equals", VK_OEM_PLUS}, {"[", VK_OEM_4}, {"lbracket", VK_OEM_4},
    {"]", VK_OEM_6}, {"rbracket", VK_OEM_6}, {"\\", VK_OEM_5}, {"backslash", VK_OEM_5},
    {";", VK_OEM_1}, {"semicolon", VK_OEM_1}, {"'", VK_OEM_7}, {"apostrophe", VK_OEM_7},
    {",", VK_OEM_COMMA}, {"comma", VK_OEM_COMMA}, {".", VK_OEM_PERIOD}, {"period", VK_OEM_PERIOD},
    {"/", VK_OEM_2}, {"slash", VK_OEM_2},
    {"lbutton", VK_LBUTTON}, {"mouse1", VK_LBUTTON}, {"rbutton", VK_RBUTTON}, {"mouse2", VK_RBUTTON},
    {"mbutton", VK_MBUTTON}, {"mouse3", VK_MBUTTON}, {"xbutton1", VK_XBUTTON1}, {"mouse4", VK_XBUTTON1},
    {"mouseback", VK_XBUTTON1}, {"xbutton2", VK_XBUTTON2}, {"mouse5", VK_XBUTTON2}, {"mouseforward", VK_XBUTTON2},
    {"wheelup", kWheelUp}, {"wheeldown", kWheelDown}, {"wheelleft", kWheelLeft}, {"wheelright", kWheelRight},
};

bool is_mouse_key(unsigned key)
{
    return key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2 ||
           (key >= kWheelUp && key <= kWheelRight);
}

// A mouse button's message (a press, release or double click), or a turn or tilt of the wheel: its button or the wheel's
// way as a key, 0 for any other message. `data` the message's wparam, or the mouse hook's mouseData (which X button, or the
// wheel's turn, in the high word of either).
unsigned mouse_key(UINT message, DWORD data)
{
    const short turn = GET_WHEEL_DELTA_WPARAM(data);
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return VK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return VK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return VK_MBUTTON;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: return HIWORD(data) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
    case WM_MOUSEWHEEL: return turn > 0 ? kWheelUp : turn < 0 ? kWheelDown : 0;
    case WM_MOUSEHWHEEL: return turn > 0 ? kWheelRight : turn < 0 ? kWheelLeft : 0;
    default: return 0;
    }
}

bool mouse_release(UINT message)
{
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A key's name (without modifiers): its virtual-key code, 0 if it isn't one
unsigned key_named(const std::string& text)
{
    const std::string name = lower(text);
    if (name.size() == 1 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9')))
        return static_cast<unsigned>(std::toupper(static_cast<unsigned char>(name[0])));
    if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24 && std::to_string(n) == name.substr(1))
            return VK_F1 + static_cast<unsigned>(n - 1);
    }
    for (const char* prefix : {"numpad", "num", "kp"}) {
        const size_t length = std::char_traits<char>::length(prefix);
        if (name.size() == length + 1 && name.compare(0, length, prefix) == 0 && std::isdigit(static_cast<unsigned char>(name[length])))
            return VK_NUMPAD0 + static_cast<unsigned>(name[length] - '0');
    }
    for (const KeyName& entry : kNames)
        if (name == entry.name)
            return entry.key;
    return 0;
}

// "^!f1": the modifiers in front, then the key. False if the key isn't one VanaCore knows.
bool parse_key(const std::string& text, Bind& out)
{
    size_t at = 0;
    while (at + 1 < text.size()) {
        const char c = text[at];
        if (c == '^')
            out.modifiers |= kCtrl;
        else if (c == '!')
            out.modifiers |= kAlt;
        else if (c == '@')
            out.modifiers |= kWin;
        else if (c == '#')
            out.modifiers |= kApps;
        else if (c == '+' || c == '~')
            out.modifiers |= kShift;
        else if (c == '%')
            out.when = When::ChatClosed;
        else if (c == '$')
            out.when = When::ChatOpen;
        else
            break;
        ++at;
    }
    out.key = key_named(text.substr(at));
    out.name = text;
    return out.key != 0;
}

void say(const std::string& text)
{
    chat::write(chat::kDefaultMode, chat::header() + text, "VanaCore");
}

void say_error(const std::string& text)
{
    say(chat::colour(chat::kError, text));
}

// The next word of `line` from `at` (spaces between), moving `at` past it
std::string next_word(std::string_view line, size_t& at)
{
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
        ++at;
    const size_t start = at;
    while (at < line.size() && line[at] != ' ' && line[at] != '\t')
        ++at;
    return std::string(line.substr(start, at - start));
}

std::string rest(std::string_view line, size_t at)
{
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
        ++at;
    size_t end = line.size();
    while (end > at && (line[end - 1] == ' ' || line[end - 1] == '\t'))
        --end;
    return std::string(line.substr(at, end - at));
}

bool same_key(const Bind& a, const Bind& b)
{
    return a.key == b.key && a.modifiers == b.modifiers && a.up == b.up && a.when == b.when;
}

void list()
{
    const std::lock_guard lock(g_lock);
    if (g_binds.empty()) {
        say("No keys are bound (/bind <key> <command>)");
        return;
    }
    say("Bound keys:");
    for (const Bind& bind : g_binds)
        say("  " + bind.name + (bind.up ? " (up)" : "") + ": " + bind.command);
}

void bind(std::string_view line, size_t at)
{
    Bind bind;
    std::string word = next_word(line, at);
    for (int flags = 0; flags < 2; ++flags) {
        const std::string flag = lower(word);
        if (flag == "up" || flag == "-up" || flag == "-u")
            bind.up = true;
        else if (flag != "down" && flag != "-down" && flag != "-d")
            break;
        word = next_word(line, at);
    }
    if (word.empty()) {
        list();
        return;
    }
    if (!parse_key(word, bind)) {
        say_error("/bind: \"" + word + "\" isn't a key VanaCore knows (f1, a, 1, numpad1, up, insert, mbutton, wheelup, ...)");
        return;
    }
    // Windower's "bind f9 up gs c x"
    size_t after = at;
    if (lower(next_word(line, after)) == "up") {
        bind.up = true;
        at = after;
    }
    bind.command = rest(line, at);
    if (bind.command.empty()) {
        say_error("/bind " + word + ": no command (/bind <key> <command>)");
        return;
    }
    {
        const std::lock_guard lock(g_lock);
        std::erase_if(g_binds, [&](const Bind& other) { return same_key(other, bind); });
        g_binds.push_back(bind);
    }
    log::write(L"Binds: %hs%hs runs \"%hs\"", bind.name.c_str(), bind.up ? " (up)" : "", bind.command.c_str());
    if (!g_silent)
        say("Bound " + bind.name + (bind.up ? " (up)" : "") + " to " + bind.command);
}

void unbind(std::string_view line, size_t at, bool all)
{
    std::string word = next_word(line, at);
    if (all || lower(word) == "all") {
        {
            const std::lock_guard lock(g_lock);
            g_binds.clear();
        }
        if (!g_silent)
            say("Unbound every key");
        return;
    }
    bool up = false;
    if (lower(word) == "up" || lower(word) == "-up" || lower(word) == "-u") {
        up = true;
        word = next_word(line, at);
    }
    Bind key;
    if (word.empty() || !parse_key(word, key)) {
        say_error("/unbind <key> | all");
        return;
    }
    if (lower(next_word(line, at)) == "up")
        up = true;
    key.up = up;
    size_t removed = 0;
    {
        const std::lock_guard lock(g_lock);
        removed = std::erase_if(g_binds, [&](const Bind& other) { return same_key(other, key); });
    }
    if (!g_silent)
        say(removed != 0 ? "Unbound " + word : word + " isn't bound");
}

// What a bind runs: a command as typed (/ma "Cure" <t>, /addon x), or Windower's console form (input /x, gs c y). A server
// command (!mog) is typed as it is, as Ashita does: the game says it and the server takes it.
std::string line_for(const std::string& command)
{
    if (!command.empty() && (command[0] == '/' || command[0] == '!'))
        return command;
    if (lower(command.substr(0, 6)) == "input ")
        return rest(command, 6);
    return "//" + command;
}

// A bind's commands: several split at each ; as Ashita's binds are (/lac disable; /wait 1; /equip head "x"), each one
// through line_for; a part starting with # is a comment, as in Ashita
std::vector<std::string> lines_of(const std::string& command)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= command.size()) {
        size_t end = command.find(';', start);
        if (end == std::string::npos)
            end = command.size();
        const std::string part = rest(std::string_view(command).substr(start, end - start), 0);
        if (!part.empty() && part[0] != '#')
            lines.push_back(line_for(part));
        start = end + 1;
    }
    return lines;
}

// The binds' commands that fired: each as Ashita runs a bind, its waits holding up only its own commands after them
void run_binds(const std::vector<std::string>& commands)
{
    for (const std::string& command : commands)
        addons::run_commands(lines_of(command));
}

// The modifiers held: as of the message the game's thread is on (GetKeyState), or off that thread (the mouse hook's) as
// they are now
unsigned modifiers_held(bool now = false)
{
    const auto held = [now](int key) { return ((now ? GetAsyncKeyState(key) : GetKeyState(key)) & 0x8000) != 0; };
    unsigned modifiers = 0;
    if (held(VK_CONTROL))
        modifiers |= kCtrl;
    if (held(VK_MENU))
        modifiers |= kAlt;
    if (held(VK_SHIFT))
        modifiers |= kShift;
    if (held(VK_LWIN) || held(VK_RWIN))
        modifiers |= kWin;
    if (held(VK_APPS))
        modifiers |= kApps;
    return modifiers;
}

// A DirectInput key code's virtual key: the numpad's keys as numpad keys (Windows gives their scan codes the keys they
// are without Num Lock: Home, End, ...), the extended ones (0x80 and up) through their E0 scan codes
unsigned from_dik(unsigned dik)
{
    struct Numpad {
        unsigned dik, key;
    };
    static constexpr Numpad kNumpad[] = {
        {0x47, VK_NUMPAD7}, {0x48, VK_NUMPAD8}, {0x49, VK_NUMPAD9}, {0x4B, VK_NUMPAD4}, {0x4C, VK_NUMPAD5},
        {0x4D, VK_NUMPAD6}, {0x4F, VK_NUMPAD1}, {0x50, VK_NUMPAD2}, {0x51, VK_NUMPAD3}, {0x52, VK_NUMPAD0},
        {0x53, VK_DECIMAL}, {0x37, VK_MULTIPLY}, {0x4A, VK_SUBTRACT}, {0x4E, VK_ADD}, {0xB5, VK_DIVIDE},
        {0x9C, kNumpadEnter},
    };
    for (const Numpad& entry : kNumpad)
        if (entry.dik == dik)
            return entry.key;
    const UINT scan = (dik & 0x7F) | ((dik & 0x80) != 0 ? 0xE000 : 0);
    return MapVirtualKeyA(scan, MAPVK_VSC_TO_VK_EX);
}

// A key's name without modifiers, as /bind takes it
std::string key_label(unsigned key)
{
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24)
        return "F" + std::to_string(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return "numpad" + std::to_string(key - VK_NUMPAD0);
    for (const KeyName& entry : kNames)
        if (entry.key == key)
            return entry.name;
    return "key " + std::to_string(key);
}

// Modifiers as /bind writes them in front of a key: ^ Ctrl, ! Alt, + Shift, @ Win, # Apps
std::string modifier_prefix(unsigned modifiers)
{
    std::string name;
    if (modifiers & kCtrl)
        name += '^';
    if (modifiers & kAlt)
        name += '!';
    if (modifiers & kShift)
        name += '+';
    if (modifiers & kWin)
        name += '@';
    if (modifiers & kApps)
        name += '#';
    return name;
}

// A bind's name for /bind's list: its modifiers (^ ! + @ #), then the key's name
std::string name_of(unsigned key, unsigned modifiers)
{
    return modifier_prefix(modifiers) + key_label(key);
}

} // namespace

unsigned message_key(unsigned vk, unsigned scan, bool extended)
{
    if (vk == VK_RETURN)
        return extended ? kNumpadEnter : vk;
    if (extended)
        return vk;
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_CLEAR: {
        const unsigned numpad = from_dik(scan & 0x7F);
        return (numpad >= VK_NUMPAD0 && numpad <= VK_NUMPAD9) || numpad == VK_DECIMAL ? numpad : vk;
    }
    default:
        return vk;
    }
}

void add(unsigned dik, unsigned modifiers, bool up, const std::string& command)
{
    Bind bind;
    bind.key = from_dik(dik);
    bind.modifiers = modifiers;
    bind.up = up;
    bind.name = name_of(bind.key, modifiers);
    bind.command = command;
    if (bind.key == 0 || command.empty())
        return;
    const std::lock_guard lock(g_lock);
    std::erase_if(g_binds, [&](const Bind& other) { return same_key(other, bind); });
    g_binds.push_back(bind);
}

bool remove(unsigned dik, unsigned modifiers, bool up)
{
    Bind key;
    key.key = from_dik(dik);
    key.modifiers = modifiers;
    key.up = up;
    const std::lock_guard lock(g_lock);
    return std::erase_if(g_binds, [&](const Bind& other) { return same_key(other, key); }) != 0;
}

void clear()
{
    const std::lock_guard lock(g_lock);
    g_binds.clear();
}

bool bound(unsigned dik, unsigned modifiers, bool up)
{
    Bind key;
    key.key = from_dik(dik);
    key.modifiers = modifiers;
    key.up = up;
    const std::lock_guard lock(g_lock);
    for (const Bind& bind : g_binds)
        if (same_key(bind, key))
            return true;
    return false;
}

namespace {

Entry entry_of(const Bind& bind)
{
    Entry entry;
    entry.key = key_label(bind.key);
    entry.vk = bind.key;
    entry.modifiers = bind.modifiers;
    entry.up = bind.up;
    entry.when = bind.when == When::ChatClosed ? 'c' : bind.when == When::ChatOpen ? 'o' : 'a';
    entry.spec = modifier_prefix(bind.modifiers) + (entry.when == 'c' ? "%" : entry.when == 'o' ? "$" : "") + entry.key;
    entry.command = bind.command;
    entry.fired_ago = bind.fired == 0 ? -1.0 : static_cast<double>(GetTickCount64() - bind.fired) / 1000.0;
    return entry;
}

} // namespace

std::vector<Entry> entries()
{
    std::vector<Entry> out;
    const std::lock_guard lock(g_lock);
    out.reserve(g_binds.size());
    for (const Bind& bind : g_binds)
        out.push_back(entry_of(bind));
    return out;
}

void note_base()
{
    const std::lock_guard lock(g_lock);
    if (g_base_known)
        return;
    g_base = g_binds;
    g_base_known = true;
}

bool base_entries(std::vector<Entry>& out)
{
    out.clear();
    const std::lock_guard lock(g_lock);
    if (!g_base_known)
        return false;
    out.reserve(g_base.size());
    for (const Bind& bind : g_base)
        out.push_back(entry_of(bind));
    return true;
}

bool set(const std::string& spec, bool up, const std::string& command, std::string& error)
{
    Bind bind;
    if (!parse_key(spec, bind)) {
        error = "\"" + spec + "\" isn't a key VanaCore knows (f1, a, 1, numpad1, up, insert, mbutton, wheelup, ...)";
        return false;
    }
    bind.up = up;
    bind.command = rest(command, 0);
    if (bind.command.empty()) {
        error = "no command";
        return false;
    }
    {
        const std::lock_guard lock(g_lock);
        std::erase_if(g_binds, [&](const Bind& other) { return same_key(other, bind); });
        g_binds.push_back(bind);
    }
    log::write(L"Binds: %hs%hs runs \"%hs\"", bind.name.c_str(), bind.up ? " (up)" : "", bind.command.c_str());
    return true;
}

bool unset(unsigned vk, unsigned modifiers, bool up, char when)
{
    Bind key;
    key.key = vk;
    key.modifiers = modifiers;
    key.up = up;
    key.when = when == 'c' ? When::ChatClosed : when == 'o' ? When::ChatOpen : When::Always;
    const std::lock_guard lock(g_lock);
    return std::erase_if(g_binds, [&](const Bind& other) { return same_key(other, key); }) != 0;
}

bool unset(const std::string& spec, bool up)
{
    Bind key;
    if (!parse_key(spec, key))
        return false;
    return unset(key.key, key.modifiers, up, key.when == When::ChatClosed ? 'c' : key.when == When::ChatOpen ? 'o' : 'a');
}

std::string key_name_of(unsigned key)
{
    return key_label(key);
}

std::vector<std::string> key_list()
{
    std::vector<std::string> out;
    std::vector<unsigned> seen;
    const auto add_key = [&](unsigned key) {
        if (std::find(seen.begin(), seen.end(), key) != seen.end())
            return;
        seen.push_back(key);
        out.push_back(key_label(key));
    };
    for (unsigned key = 'A'; key <= 'Z'; ++key)
        add_key(key);
    for (unsigned key = '0'; key <= '9'; ++key)
        add_key(key);
    for (unsigned key = VK_F1; key <= VK_F24; ++key)
        add_key(key);
    for (unsigned key = VK_NUMPAD0; key <= VK_NUMPAD9; ++key)
        add_key(key);
    for (const KeyName& entry : kNames)
        add_key(entry.key);
    return out;
}

void capture_begin()
{
    const std::lock_guard lock(g_lock);
    g_capture = Capture{};
    g_capture.active = true;
    g_capture.started = GetTickCount64();
}

void capture_end()
{
    const std::lock_guard lock(g_lock);
    g_capture = Capture{};
}

CaptureState capture_state()
{
    CaptureState state;
    const std::lock_guard lock(g_lock);
    state.active = g_capture.active;
    state.done = g_capture.done;
    state.cancelled = g_capture.cancelled;
    state.key = g_capture.key;
    state.modifiers = g_capture.done ? g_capture.modifiers : modifiers_held();
    return state;
}

int apply_file(const std::string& text)
{
    // The saved binds replay without chat output: /bind and /unbind lines, anything else ignored
    const bool was_silent = g_silent;
    g_silent = true;
    int applied = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string_view line = std::string_view(text).substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        size_t at = 0;
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
            ++at;
        if (at >= line.size() || line[at] == '#')
            continue;
        if (command(line.substr(at)))
            ++applied;
    }
    g_silent = was_silent;
    return applied;
}

void install(const wchar_t* ini)
{
    g_block_during_input = GetPrivateProfileIntW(L"input", L"keyboard.blockbindsduringinput", 1, ini) != 0;
    g_silent = GetPrivateProfileIntW(L"input", L"keyboard.silentbinds", 0, ini) != 0;
    log::write(L"Binds: keyboard.blockbindsduringinput=%d keyboard.silentbinds=%d", g_block_during_input ? 1 : 0,
               g_silent ? 1 : 0);
    // Print Screen takes a screenshot, as Ashita's /bind SYSRQ /screenshot does: an ordinary bind, so /unbind printscreen
    // (or the keybinds window) turns it off and /bind printscreen <command> replaces it
    Bind snapshot;
    snapshot.key = VK_SNAPSHOT;
    snapshot.name = "printscreen";
    snapshot.command = "/screenshot";
    const std::lock_guard lock(g_lock);
    std::erase_if(g_binds, [&](const Bind& other) { return same_key(other, snapshot); });
    g_binds.push_back(snapshot);
}

void reload(const wchar_t* ini)
{
    const bool block = GetPrivateProfileIntW(L"input", L"keyboard.blockbindsduringinput", 1, ini) != 0;
    if (g_block_during_input.exchange(block) != block)
        log::write(L"Binds: keyboard.blockbindsduringinput=%d", block ? 1 : 0);
    const bool silent = GetPrivateProfileIntW(L"input", L"keyboard.silentbinds", 0, ini) != 0;
    if (g_silent != silent) {
        g_silent = silent;
        log::write(L"Binds: keyboard.silentbinds=%d", silent ? 1 : 0);
    }
}

bool block_during_input()
{
    return g_block_during_input;
}

void set_block_during_input(bool block)
{
    if (g_block_during_input.exchange(block) != block)
        log::write(L"Binds: keyboard.blockbindsduringinput=%d (set by an addon or plugin)", block ? 1 : 0);
}

bool typing_blocks()
{
    if (!g_block_during_input)
        return false;
    bool open = false;
    std::string typed;
    chat::input_line(open, typed);
    return open;
}

bool command(std::string_view line)
{
    size_t at = 0;
    const std::string name = lower(next_word(line, at));
    if (name == "/bind" || name == "//bind") {
        bind(line, at);
        return true;
    }
    if (name == "/unbind" || name == "//unbind") {
        unbind(line, at, false);
        return true;
    }
    if (name == "/unbindall" || name == "//unbindall") {
        unbind(line, at, true);
        return true;
    }
    return false;
}

unsigned key_code(std::string_view name)
{
    const unsigned key = key_named(std::string(name));
    return is_mouse_key(key) ? 0 : key;
}

namespace {

// Whether `bind` is on `key` held with `modifiers` and takes it now, the chat line open as `chat_open` says: a $ bind only
// while the line is open, a % bind only while it's closed, any other while it's closed or, with
// keyboard.blockbindsduringinput off, open too. Print Screen types nothing, so it works with the line open as well.
bool bind_takes(const Bind& bind, unsigned key, unsigned modifiers, bool chat_open)
{
    if (bind.key != key || bind.modifiers != modifiers)
        return false;
    if (bind.when == When::ChatOpen)
        return chat_open;
    return !chat_open || (bind.when != When::ChatClosed && (!g_block_during_input || key == VK_SNAPSHOT));
}

// The commands `key` held with `modifiers` runs, added to `run` (on its press, or its release for an up bind): true if a
// bind has it, so the key is kept from the game. With g_lock held.
bool match(unsigned key, unsigned modifiers, bool down, bool repeat, std::vector<std::string>& run)
{
    bool chat_open = false;
    std::string typed;
    chat::input_line(chat_open, typed);
    bool taken = false;
    for (Bind& bind : g_binds) {
        if (!bind_takes(bind, key, modifiers, chat_open))
            continue;
        taken = true;
        if (bind.up != down && !repeat) {
            run.push_back(bind.command);
            bind.fired = GetTickCount64();
        }
    }
    return taken;
}

// A modifier key on its own: not a capture's answer
bool is_modifier_key(unsigned key)
{
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_LCONTROL || key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN ||
           key == VK_APPS;
}

} // namespace

bool takes_key(unsigned dik, unsigned modifiers)
{
    // the numpad's keys are numpad keys whatever Num Lock says, as message_key has them (the game's keyboard reads them so
    // too: its Tab target cycling is DirectInput's Num0, 0x52, either way)
    const unsigned key = from_dik(dik);
    if (key == 0)
        return false;
    const std::lock_guard lock(g_lock);
    // a capture takes the next key pressed, and every key after it until it's ended (capture_key)
    if (g_capture.active && (g_capture.done || GetTickCount64() - g_capture.started <= 30000))
        return !is_modifier_key(key);
    bool chat_open = false;
    bool asked = false; // the chat line's state read once, and only for a key some bind is on (W, A, S, D are held a lot)
    for (const Bind& bind : g_binds) {
        if (bind.key != key || bind.modifiers != modifiers)
            continue;
        if (!asked) {
            std::string typed;
            chat::input_line(chat_open, typed);
            asked = true;
        }
        if (bind_takes(bind, key, modifiers, chat_open))
            return true;
    }
    return false;
}

namespace {

// While a capture is on: a key pressed (not a modifier alone) is its answer. True if the key is the capture's (a press, and
// the repeats and release that follow, are kept from the game). With g_lock held.
bool capture_key(unsigned key, bool down, unsigned modifiers)
{
    if (!g_capture.active)
        return false;
    if (!g_capture.done && GetTickCount64() - g_capture.started > 30000) {
        g_capture = Capture{};
        return false;
    }
    if (is_modifier_key(key))
        return false;
    if (down && !g_capture.done) {
        g_capture.done = true;
        g_capture.key = key;
        g_capture.modifiers = modifiers;
        g_capture.cancelled = key == VK_ESCAPE && modifiers == 0;
    }
    return true;
}

} // namespace

bool capture_mouse(UINT message, DWORD data)
{
    const unsigned key = mouse_key(message, data);
    if (key == 0 || mouse_release(message))
        return false;
    const unsigned modifiers = modifiers_held(true);
    // what works the window stays its own without a modifier: a plain left or right click (the box clicked again stops
    // the capture, Clear clears it), a plain turn of the wheel (the list scrolls)
    if ((key == VK_LBUTTON || key == VK_RBUTTON || key == kWheelUp || key == kWheelDown) && modifiers == 0)
        return false;
    const std::lock_guard lock(g_lock);
    if (!g_capture.active || g_capture.done || GetTickCount64() - g_capture.started > 30000)
        return false;
    g_capture.done = true;
    g_capture.key = key;
    g_capture.modifiers = modifiers;
    return true;
}

namespace {

// The key message on_queued_key last let through, which the window procedure gets next: not asked about twice (the
// chat line the game's loop opened in between would make a $ bind run on the key that opened it). With g_lock held.
struct Queued {
    UINT message = 0;
    WPARAM wparam = 0;
    LPARAM lparam = 0;
};
Queued g_let_through;
bool g_snapshot_down = false; // Print Screen's press was seen (Windows tells only its release): take_key. With g_lock held.

// on_key and on_queued_key: the bind (or capture) that takes the key, its commands run
bool take_key(UINT message, WPARAM wparam, LPARAM lparam, bool queued)
{
    std::vector<std::string> run;
    bool taken = false;
    {
        const std::lock_guard lock(g_lock);
        if (!queued && g_let_through.message == message && g_let_through.wparam == wparam && g_let_through.lparam == lparam) {
            g_let_through = Queued{};
            return false;
        }
        g_let_through = Queued{};
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const unsigned key = message_key(static_cast<unsigned>(wparam), static_cast<unsigned>(lparam >> 16) & 0xFF,
                                         (lparam & (1 << 24)) != 0);
        if (capture_key(key, down, modifiers_held()))
            return true;
        if (!g_binds.empty()) {
            const bool repeat = down && (lparam & (1 << 30)) != 0;
            // Windows sends Print Screen's release and not its press: a release no press came before is the press too
            const bool lone_release = key == VK_SNAPSHOT && !down && !g_snapshot_down;
            if (key == VK_SNAPSHOT)
                g_snapshot_down = down;
            if (lone_release)
                taken = match(key, modifiers_held(), true, false, run);
            if (match(key, modifiers_held(), down, repeat, run))
                taken = true;
        }
        if (queued && !taken)
            g_let_through = Queued{message, wparam, lparam};
    }
    run_binds(run);
    return taken;
}

} // namespace

bool on_queued_key(UINT message, WPARAM wparam, LPARAM lparam)
{
    return take_key(message, wparam, lparam, true);
}

bool on_key(UINT message, WPARAM wparam, LPARAM lparam)
{
    return take_key(message, wparam, lparam, false);
}

bool on_mouse(UINT message, WPARAM wparam)
{
    const unsigned key = mouse_key(message, static_cast<DWORD>(wparam));
    if (key == 0)
        return false;
    std::vector<std::string> run;
    bool taken = false;
    {
        const std::lock_guard lock(g_lock);
        if (g_binds.empty())
            return false;
        taken = match(key, modifiers_held(), !mouse_release(message), false, run);
    }
    run_binds(run);
    return taken;
}

bool take_win_key(unsigned key, bool down, bool repeat, unsigned modifiers)
{
    const HWND window = window::handle();
    if (window == nullptr)
        return false;
    bool taken = false;
    bool queued = false;
    {
        const std::lock_guard lock(g_lock);
        if (capture_key(key, down, modifiers))
            return true;
        if (g_binds.empty())
            return false;
        std::vector<std::string> run;
        taken = match(key, modifiers, down, repeat, run);
        for (std::string& line : run)
            g_pending.push_back(std::move(line));
        queued = !run.empty();
    }
    if (queued)
        PostMessageW(window, kRunPending, 0, 0);
    return taken;
}

bool take_wheel(UINT message, DWORD data)
{
    const unsigned key = mouse_key(message, data);
    const HWND window = window::handle();
    if ((message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL) || key == 0 || window == nullptr)
        return false;
    const unsigned modifiers = modifiers_held(true);
    bool queued = false;
    {
        const std::lock_guard lock(g_lock);
        if (g_binds.empty())
            return false;
        std::vector<std::string> run;
        // a bind on it, run or not: every turn of it is kept from the game, a notch's worth or less
        if (!match(key, modifiers, true, true, run))
            return false;
        int& turned = g_wheel_turned[message == WM_MOUSEHWHEEL ? 1 : 0];
        const int turn = GET_WHEEL_DELTA_WPARAM(data);
        if ((turned < 0) != (turn < 0)) // turned back: a new count
            turned = 0;
        turned += turn;
        for (; turned >= WHEEL_DELTA || turned <= -WHEEL_DELTA; turned -= turn > 0 ? WHEEL_DELTA : -WHEEL_DELTA) {
            match(key, modifiers, true, false, run);
            match(key, modifiers, false, false, run);
        }
        for (std::string& line : run)
            g_pending.push_back(std::move(line));
        queued = !run.empty();
    }
    if (queued)
        PostMessageW(window, kRunPending, 0, 0);
    return true;
}

void run_pending()
{
    std::vector<std::string> run;
    {
        const std::lock_guard lock(g_lock);
        run.swap(g_pending);
    }
    run_binds(run);
}

} // namespace vanacore::binds
