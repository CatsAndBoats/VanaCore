// Ashita's "/raw <command>" (chat.cpp): chat.cpp includes this inside its anonymous namespace, after g_parse and
// g_commands and before parse_input; it is no file of its own.

// Ashita's "/raw <command>": the command goes to the game as it is, past every plugin and addon (Shorthand sends
// "/raw /ma "Cure IV" <stpc>" for a macro's /ma). The game has no /raw of its own: its command lookup fails on it, so
// sub-target tags never open their cursor. Returns the command after "/raw ", or null when the line isn't one.
const char* raw_command(const char* line)
{
    if (line == nullptr)
        return nullptr;
    while (*line == ' ' || *line == '\t')
        ++line;
    constexpr char kRaw[] = "/raw";
    constexpr size_t kLength = sizeof kRaw - 1;
    if (_strnicmp(line, kRaw, kLength) != 0 || (line[kLength] != ' ' && line[kLength] != '\t'))
        return nullptr;
    line += kLength;
    while (*line == ' ' || *line == '\t')
        ++line;
    return *line != '\0' ? line : nullptr;
}

// A line the command handler took. A plugin that takes a game line only to send it on changed queues its copy as
// "/raw <line>", typed, for the next frame (Shorthand's QueueCommand(1, "/raw /ma \"Cure\" <stpc>") for a macro's /ma,
// /ws, /ja, /item, /ra, /attack, /target). For a macro's line (mode 2) or a sub-target pick (3-7) that copy is wrong
// twice over: the macro doesn't wait at its <st> line, since the cursor only opens a frame later, and a typed <stpc>
// while a cursor is up prints "You cannot perform that action on the selected sub-target." where a macro's is ignored
// (RogueBren, 2026-11-18). So the one typed /raw line queued while the handler had such a line runs now, in its place
// and mode. Anything else (no copy, more than one line queued, a typed line) stays as queued.
int handled_line(size_t queued_before, int mode)
{
    const int kind = mode & 0xFF;
    if (kind < 2 || kind > 7 || g_commands.size() != queued_before + 1 || g_commands.back().first != 1 ||
        raw_command(g_commands.back().second.c_str()) == nullptr)
        return 0;
    const std::string copy = std::move(g_commands.back().second);
    g_commands.pop_back();
    static bool logged = false;
    if (!logged) {
        logged = true;
        log::write(L"Chat: a plugin's /raw copy of a macro or sub-target line runs in its place, mode %d (logged once)", kind);
    }
    return g_parse(raw_command(copy.c_str()), mode);
}
