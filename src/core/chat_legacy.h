// fancychat's "legacy chat opened" while VanaCore keeps log 1 closed (chat.cpp). chat.cpp includes this inside its
// anonymous namespace, after kFancyChatAsk and g_chat_pinned; it is no file of its own.
#pragma once

// fancychat takes log 1 for opened by the player while its top (+0x42) holds still or rises and is over the UI's bottom
// less 34, and for closed again only when the top moves down (lib/render.lua: uiw.LegacyChatOpen; its chat is hidden
// meanwhile unless Show with legacy chat). Under its own pin (the line timer zeroed after the game's frame) the log still
// grows a step at every new line and eases back down, so a wrong "opened" (a re-layout, a UI size change, a zone) is
// cleared by the next line. Under VanaCore's pin (chat_calc_hook) the log never moves at all, so it stayed: fancychat's
// chat gone until something moved the log (Show with legacy chat on and off again). Reported on Discord 2026-11. While
// VanaCore has pinned the log since the last ask (it can't have been opened) and it rests where fancychat's own test calls
// closed, the flag is put back; held high (chat.move resting failed, a stale UI size), it's left as fancychat has it
// (setting it would make fancychat dump and empty its buffer at the next frame).
constexpr char kFancyChatUnlatch[] =
    "local ok, done = pcall(function()\n"
    "    local state = package.loaded['lib.state']\n"
    "    local uiw = state and state.uiw\n"
    "    if type(uiw) ~= 'table' or uiw.LegacyChatOpen ~= true then return 0 end\n"
    "    local top, size = tonumber(uiw.MemValue), tonumber(uiw.UISizeY)\n"
    "    if top == nil or size == nil or top == 0 or top < size - 34 then return 0 end\n"
    "    uiw.LegacyChatOpen = false\n"
    "    return 1\n"
    "end)\n"
    "return ok and done or 0";
bool g_pinned_before = false; // g_chat_pinned at the ask before

// Twice a second, after the ask: fancychat's "legacy chat opened" put back while VanaCore keeps log 1 closed (above)
void unlatch_legacy()
{
    if (g_chat_pinned && g_pinned_before && addons::ask("fancychat", kFancyChatUnlatch) == 1)
        log::write(L"Chat: fancychat took the game's chat for opened while VanaCore keeps it closed: its chat shown again");
    g_pinned_before = g_chat_pinned;
}
