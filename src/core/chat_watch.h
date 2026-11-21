// The chat windows' place watched, for the "chat log moves" reports (2026-11-12: Fruitypebbles' log went down to the
// bottom in Sky and while crafting until the full log was opened and closed, which makes the logs' windows again; a
// friend's went up for some 15 minutes). Nothing is changed; vanacore.log is told, as "Chat watch:", when a log's window
//  - is made again (the game makes a new one at a zone-in, as the full log closes, ...),
//  - isn't taken by chat_ctrl (its +8 holds a window whose +0xc isn't the log), so chat.move can't place it,
//  - has moved (its left, bottom or width) since VanaCore last left it, by the game between two Presents,
//  - is drawn away from its rect for a second: its frame part not where its rect (+0x3a) is. place_chat looks only at
//    the rect, so it can't see that. A window's parts are a list at +0x14 (a node: +0 the next, +0x10 the part, +0x14
//    set to skip it), a part's +0 its type, 0 a frame, whose +0x1a x, +0x1c y, +0x22 width and +0x24 height its resize
//    (0x1011a1a0, b=0, as the chat's own calls pass it) sets from the window's new place (+0x8c, as x, y, w, h too).
// The first such thing since the last line is written as found, with how many more there were, at most a line every 2 s
// for each log. on_frame calls watch_logs_before at its start, before anything of VanaCore's moves a log, and
// watch_logs_after once the logs are placed. chat.cpp includes this inside its anonymous namespace, after chat_place.h;
// it is no file of its own.
#pragma once

// What a log's window is as looked at once (plain data, read under __try)
struct LogSeen {
    char* raw = nullptr;  // the log's +8, taken or not
    char* owner = nullptr; // that window's +0xc
    char* ctrl = nullptr; // chat_ctrl's: the window taken, or null
    short rect[4] = {};   // +0x3a: left, top, right, bottom
    short opened[4] = {}; // +0x42..+0x48: its place as opened (fancychat takes +0x42 for its top)
    short stored[4] = {}; // +0x8c: its place as its resize stored it: x, y, width, height
    short frame[4] = {};  // its frame part's: x, y, width, height
    bool framed = false;
    std::uint32_t state = 0; // +0x10
};

struct LogWatch {
    char* ctrl = nullptr;         // the window as VanaCore last left it
    short place[3] = {};          // its left, bottom and width then
    bool refused = false;         // the log's +8 holds a window chat_ctrl doesn't take
    bool away = false;            // its frame is away from its rect (for a second at least)
    ULONGLONG away_since = 0;     // the frame first seen away, 0 while it isn't
    unsigned moves = 0, made = 0; // since the last line
    wchar_t first[400] = {};      // the first thing seen since the last line, empty for none
    ULONGLONG said = 0;           // the last line's time
};
LogWatch g_watch[2];
constexpr ULONGLONG kWatchEvery = 2000, kAwayFor = 1000;

// The window's first frame part's place (x, y, width, height): false without one
bool frame_rect(const char* ctrl, short out[4])
{
    const char* node = *reinterpret_cast<char* const*>(ctrl + 0x14);
    for (int count = 0; node != nullptr && count < 64; node = *reinterpret_cast<char* const*>(node), ++count) {
        const char* part = *reinterpret_cast<char* const*>(node + 0x10);
        if (node[0x14] != 0 || part == nullptr || *reinterpret_cast<const short*>(part) != 0)
            continue;
        const short* at = reinterpret_cast<const short*>(part + 0x1A);
        out[0] = at[0], out[1] = at[1], out[2] = at[4], out[3] = at[5]; // +0x1a, +0x1c, +0x22, +0x24
        return true;
    }
    return false;
}

bool look(int index, LogSeen& seen)
{
    __try {
        seen = LogSeen{};
        char* window = g_logs[index].global != nullptr ? *g_logs[index].global : nullptr;
        seen.raw = window != nullptr ? *reinterpret_cast<char**>(window + 8) : nullptr;
        seen.owner = seen.raw != nullptr ? *reinterpret_cast<char**>(seen.raw + 0xC) : nullptr;
        seen.ctrl = chat_ctrl(index);
        if (seen.ctrl == nullptr)
            return true;
        std::memcpy(seen.rect, seen.ctrl + 0x3A, sizeof seen.rect);
        std::memcpy(seen.opened, seen.ctrl + 0x42, sizeof seen.opened);
        std::memcpy(seen.stored, seen.ctrl + 0x8C, sizeof seen.stored);
        seen.state = *reinterpret_cast<const std::uint32_t*>(seen.ctrl + 0x10);
        seen.framed = frame_rect(seen.ctrl, seen.frame);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A window's places as a line's words
void describe(const LogSeen& seen, wchar_t* out, size_t size)
{
    if (seen.ctrl == nullptr) {
        swprintf_s(out, size, L"no window taken (+8 %p, its +0xc %p)", static_cast<void*>(seen.raw),
                   static_cast<void*>(seen.owner));
        return;
    }
    const short *r = seen.rect, *o = seen.opened, *s = seen.stored, *f = seen.frame;
    wchar_t frame[48] = L"none";
    if (seen.framed)
        swprintf_s(frame, L"%d,%d %dx%d", f[0], f[1], f[2], f[3]);
    swprintf_s(out, size, L"rect %d,%d-%d,%d, +0x42 %d,%d,%d,%d, stored %d,%d %dx%d, frame %ls, state 0x%X, window %p", r[0],
               r[1], r[2], r[3], o[0], o[1], o[2], o[3], s[0], s[1], s[2], s[3], frame, seen.state,
               static_cast<void*>(seen.ctrl));
}

// The first thing since the last line kept as found (the rest are counted)
void watch_note(LogWatch& watch, const LogSeen& seen, const wchar_t* what)
{
    if (watch.first[0] != L'\0')
        return;
    wchar_t places[300] = {};
    describe(seen, places, _countof(places));
    swprintf_s(watch.first, L"%ls: %ls", what, places);
}

// The frame away from the rect for kAwayFor, or back on it
void watch_frame(LogWatch& watch, const LogSeen& seen, ULONGLONG now)
{
    const short* r = seen.rect;
    const short place[4] = {r[0], r[1], static_cast<short>(r[2] - r[0]), static_cast<short>(r[3] - r[1])};
    const bool off = seen.framed && std::memcmp(seen.frame, place, sizeof place) != 0;
    if (!off) {
        if (watch.away)
            watch_note(watch, seen, L"its frame is back on its rect");
        watch.away = false, watch.away_since = 0;
        return;
    }
    if (watch.away_since == 0)
        watch.away_since = now;
    if (!watch.away && now - watch.away_since >= kAwayFor) {
        watch.away = true;
        watch_note(watch, seen, L"its frame is drawn away from its rect");
    }
}

void watch_log(int index, ULONGLONG now)
{
    LogSeen seen;
    if (!look(index, seen))
        return;
    LogWatch& watch = g_watch[index];
    const bool refused = seen.raw != nullptr && seen.ctrl == nullptr;
    if (refused != watch.refused)
        watch_note(watch, seen, refused ? L"its window isn't taken, so chat.move can't place it" : L"its window is taken again");
    watch.refused = refused;
    if (seen.ctrl == nullptr) {
        watch.ctrl = nullptr, watch.away = false, watch.away_since = 0;
        return;
    }
    if (seen.ctrl != watch.ctrl) {
        ++watch.made;
        watch_note(watch, seen, watch.ctrl == nullptr ? L"its window made" : L"its window made again");
        return;
    }
    const short place[3] = {seen.rect[0], seen.rect[3], static_cast<short>(seen.rect[2] - seen.rect[0])};
    if (std::memcmp(place, watch.place, sizeof place) != 0) {
        ++watch.moves;
        wchar_t what[96] = {};
        swprintf_s(what, L"moved, from left %d, bottom %d, width %d", watch.place[0], watch.place[1], watch.place[2]);
        watch_note(watch, seen, what);
    }
    watch_frame(watch, seen, now);
}

void watch_logs_before()
{
    const ULONGLONG now = GetTickCount64();
    for (int index = 0; index < 2; ++index)
        if (g_logs[index].global != nullptr)
            watch_log(index, now);
}

// The zone, the player's status (the server's and the client's: 4 in an event) and the log's chat.move, for a line
void watch_context(int index, wchar_t* out, size_t size)
{
    game::PartyMember members[18];
    game::Alliance alliance;
    game::Entity entity;
    unsigned zone = 0, server = 0, client = 0;
    if (game::party(members, alliance) && members[0].active) {
        zone = members[0].zone;
        if (members[0].target_index != 0 && game::entity(members[0].target_index, entity))
            server = entity.status_server, client = entity.status;
    }
    swprintf_s(out, size, L"zone %u, status %u/%u, %ls.move %ls", zone, server, client, index == 0 ? L"chat" : L"chat2",
               log_moved(index) ? L"on" : L"off");
}

// The logs as VanaCore leaves them this frame, and a line for what was seen (at most every kWatchEvery)
void watch_logs_after()
{
    const ULONGLONG now = GetTickCount64();
    for (int index = 0; index < 2; ++index) {
        LogWatch& watch = g_watch[index];
        LogSeen seen;
        if (g_logs[index].global == nullptr || !look(index, seen))
            continue;
        watch.ctrl = seen.ctrl;
        watch.place[0] = seen.rect[0], watch.place[1] = seen.rect[3];
        watch.place[2] = static_cast<short>(seen.rect[2] - seen.rect[0]);
        if (watch.first[0] == L'\0' || now - watch.said < kWatchEvery)
            continue;
        wchar_t context[96] = {}, after[300] = {};
        watch_context(index, context, _countof(context));
        describe(seen, after, _countof(after));
        log::write(L"Chat watch: chat window %d %ls; %u moves and %u windows made since the last line; now %ls; %ls",
                   index + 1, watch.first, watch.moves, watch.made, after, context);
        watch.first[0] = L'\0', watch.moves = 0, watch.made = 0, watch.said = now;
    }
}
