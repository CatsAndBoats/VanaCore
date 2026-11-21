#include "aliases.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include "chat.hpp"
#include "log.hpp"

namespace vanacore::aliases {
namespace {

struct Alias {
    std::string trigger; // as given (for listing)
    std::string key;     // lower case, for matching
    std::string command;
};

// How many aliases one line goes through at most (each alias once: one that leads back to itself stops there)
constexpr int kMaxDepth = 8;

std::mutex g_lock;
std::vector<Alias> g_aliases; // in the order they were added
bool g_silent = true;         // aliases.silent

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
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
    while (end > at && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r'))
        --end;
    return std::string(line.substr(at, end - at));
}

void list()
{
    const std::lock_guard lock(g_lock);
    if (g_aliases.empty()) {
        say("No aliases (/alias <trigger> <command>)");
        return;
    }
    say("Aliases:");
    for (const Alias& alias : g_aliases)
        say("  " + alias.trigger + ": " + alias.command);
}

void add(std::string_view line, size_t at)
{
    Alias alias;
    alias.trigger = next_word(line, at);
    alias.command = rest(line, at);
    if (alias.trigger.empty() || alias.command.empty()) {
        say_error("/alias " + (alias.trigger.empty() ? std::string("add") : alias.trigger) +
                  ": no command (/alias <trigger> <command>)");
        return;
    }
    alias.key = lower(alias.trigger);
    {
        const std::lock_guard lock(g_lock);
        std::erase_if(g_aliases, [&](const Alias& other) { return other.key == alias.key; });
        g_aliases.push_back(alias);
    }
    log::write(L"Aliases: %hs runs \"%hs\"", alias.trigger.c_str(), alias.command.c_str());
    if (!g_silent)
        say("Alias " + alias.trigger + " runs " + alias.command);
}

void remove(std::string_view line, size_t at)
{
    const std::string trigger = next_word(line, at);
    if (trigger.empty()) {
        say_error("/alias delete <trigger>");
        return;
    }
    const std::string key = lower(trigger);
    size_t removed = 0;
    {
        const std::lock_guard lock(g_lock);
        removed = std::erase_if(g_aliases, [&](const Alias& other) { return other.key == key; });
    }
    log::write(L"Aliases: %hs %hs", trigger.c_str(), removed != 0 ? "removed" : "wasn't one");
    if (!g_silent)
        say(removed != 0 ? "Removed the alias " + trigger : trigger + " isn't an alias");
}

void clear()
{
    {
        const std::lock_guard lock(g_lock);
        g_aliases.clear();
    }
    log::write(L"Aliases: cleared");
    if (!g_silent)
        say("Cleared every alias");
}

} // namespace

void install(const wchar_t* ini)
{
    g_silent = GetPrivateProfileIntW(L"input", L"aliases.silent", 1, ini) != 0;
    log::write(L"Aliases: aliases.silent=%d", g_silent ? 1 : 0);
}

bool command(std::string_view line)
{
    size_t at = 0;
    const std::string name = lower(next_word(line, at));
    if (name != "/alias" && name != "//alias")
        return false;
    size_t after = at;
    const std::string sub = lower(next_word(line, after));
    if (sub.empty())
        say_error("/alias add <trigger> <command>, /alias <trigger> <command>, /alias delete <trigger>, /alias clear, /alias list");
    else if (sub == "add")
        add(line, after);
    else if (sub == "del" || sub == "delete" || sub == "remove" || sub == "rem")
        remove(line, after);
    else if (sub == "clear")
        clear();
    else if (sub == "list")
        list();
    else
        add(line, at);
    return true;
}

bool expand(std::string_view line, std::string& out)
{
    const std::lock_guard lock(g_lock);
    if (g_aliases.empty())
        return false;
    std::string text(line);
    std::vector<std::string> used;
    for (int depth = 0; depth < kMaxDepth; ++depth) {
        size_t at = 0;
        const std::string word = lower(next_word(text, at));
        if (word.empty() || std::find(used.begin(), used.end(), word) != used.end())
            break;
        const auto alias = std::find_if(g_aliases.begin(), g_aliases.end(), [&](const Alias& each) { return each.key == word; });
        if (alias == g_aliases.end())
            break;
        used.push_back(word);
        text = alias->command + text.substr(at);
    }
    if (used.empty())
        return false;
    out = std::move(text);
    return true;
}

bool silent()
{
    return g_silent;
}

void set_silent(bool silent)
{
    g_silent = silent;
}

} // namespace vanacore::aliases
