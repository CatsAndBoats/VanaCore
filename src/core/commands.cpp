#include "commands.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "log.hpp"

namespace vanacore::commands {
namespace {

// What VanaCore needs to be run at all: loading addons and plugins (the /vana window is an addon), scripts and their waits,
// binds and aliases. A [commands] key for one of these is ignored.
constexpr const char* kKept[] = {"addon", "plugin", "exec", "vana", "wait", "bind", "unbind", "unbindall", "alias", "keybinds"};

std::mutex g_lock;
std::vector<std::string> g_off; // lower case, without the slashes

std::string bare(std::string_view word)
{
    while (!word.empty() && word.front() == '/')
        word.remove_prefix(1);
    std::string out(word);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool kept(const std::string& name)
{
    return std::find_if(std::begin(kKept), std::end(kKept), [&](const char* each) { return name == each; }) != std::end(kKept);
}

// [commands] as "key=value\0key=value\0\0": each key whose value is 0 (or off, false) is a command switched off
void read(const wchar_t* ini)
{
    std::vector<wchar_t> section(8192, L'\0');
    const DWORD length = GetPrivateProfileSectionW(L"commands", section.data(), static_cast<DWORD>(section.size()), ini);
    std::vector<std::string> off;
    for (const wchar_t* at = section.data(); length != 0 && *at != L'\0'; at += std::wcslen(at) + 1) {
        const wchar_t* equals = std::wcschr(at, L'=');
        if (equals == nullptr)
            continue;
        std::wstring value(equals + 1);
        std::erase_if(value, [](wchar_t c) { return c == L' ' || c == L'\t'; });
        if (value != L"0" && _wcsicmp(value.c_str(), L"off") != 0 && _wcsicmp(value.c_str(), L"false") != 0)
            continue;
        std::string name;
        for (const wchar_t* c = at; c != equals; ++c)
            if (*c != L' ' && *c != L'\t' && *c < 0x80)
                name += static_cast<char>(*c);
        name = bare(name);
        if (!name.empty() && !kept(name))
            off.push_back(name);
    }
    std::sort(off.begin(), off.end());
    const std::lock_guard lock(g_lock);
    if (off == g_off)
        return;
    g_off = std::move(off);
    std::string list;
    for (const std::string& name : g_off)
        list += (list.empty() ? "/" : ", /") + name;
    log::write(L"Commands: switched off: %hs", list.empty() ? "none" : list.c_str());
}

} // namespace

void install(const wchar_t* ini)
{
    read(ini);
}

void reload(const wchar_t* ini)
{
    read(ini);
}

bool on(std::string_view word)
{
    const std::string name = bare(word);
    const std::lock_guard lock(g_lock);
    return g_off.empty() || !std::binary_search(g_off.begin(), g_off.end(), name);
}

} // namespace vanacore::commands
