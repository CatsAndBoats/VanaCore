// [client] chat.move and chat2.move applied (chat.cpp): a log put where the settings say or back at the game's place, and
// both resting while fancychat stands in for the chat (g_place_rests). chat.cpp includes this inside its anonymous
// namespace, after place_chat, chat_ctrl and the menus' patch, before chatlog_refresh; it is no file of its own.
#pragma once

// Log `index` back where the game's layout puts it, place and width (log 2 goes back on log 1 by itself)
void game_place(int index)
{
    char* window = nullptr;
    if (chat_ctrl(index, &window) != nullptr)
        g_chat_layout(window, nullptr);
}

void apply_chat_place(int index, bool move, double x, double y, double width)
{
    ChatLog& log = g_logs[index];
    const bool changed = move != log.move || x != log.x || y != log.y || width != log.width;
    log.x = x, log.y = y, log.width = width;
    if (!changed || g_chat_layout == nullptr || (index == 1 && g_set_locate == nullptr))
        return;
    const bool was = log.move;
    log.move = move;
    if (move) {
        if (!g_place_rests)
            place_chat(index);
        log::write(L"Chat: chat window %d at %.1f%%, %.1f%% (bottom), %.1f%% wide ([client] %ls.move)%ls", index + 1, x, y,
                   width, index == 0 ? L"chat" : L"chat2", g_place_rests ? L", once fancychat isn't standing in for it" : L"");
    } else if (was && !g_place_rests) {
        game_place(index);
        log::write(L"Chat: chat window %d back where the game puts it", index + 1);
    }
}

// g_place_rests as fancychat comes and goes (kFancyChatAsk's 8, twice a second): a moved log back at the game's place, or
// at chat.move's again (on_frame puts it there)
void set_place_rests(bool rest)
{
    if (rest == g_place_rests || g_chat_layout == nullptr)
        return;
    g_place_rests = rest;
    if (!g_logs[0].move && !g_logs[1].move)
        return;
    if (rest)
        for (int index = 0; index < 2; ++index)
            if (g_logs[index].move)
                game_place(index);
    log::write(rest ? L"Chat: fancychat stands in for the chat: the chat windows at the game's place while it does ([client] "
                      L"chat.move and chat2.move rest; moved, fancychat took them for open and hid its chat)"
                    : L"Chat: fancychat no longer stands in for the chat: the chat windows where chat.move and chat2.move put them");
}
