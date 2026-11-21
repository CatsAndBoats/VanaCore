#include "sound.hpp"

#include "dsound_follow.hpp"
#include "imports.hpp"
#include "log.hpp"

namespace vanacore::sound {
namespace {

dsfollow::CreateFunction g_create = nullptr; // DirectSoundCreate8 as FFXiMain.dll's import had it

void log_line(const wchar_t* text)
{
    log::write(L"%ls", text);
}

void on_game_loaded(HMODULE module)
{
    // FFXiMain.dll imports DirectSoundCreate8 by its ordinal in DSOUND.dll, 11
    if (!imports::hook(module, "DSOUND.dll", "#11", &dsfollow::create, g_create)) {
        log::write(L"Sound: FFXiMain.dll's DirectSoundCreate8 isn't there to follow the output with");
        return;
    }
    dsfollow::set_functions(g_create, &log_line);
}

} // namespace

void install(const wchar_t* ini)
{
    if (ini == nullptr || ini[0] == L'\0')
        return;
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (GetPrivateProfileIntW(L"client", L"sound.follow", wine ? 0 : 1, ini) == 0) {
        log::write(L"Sound: the game keeps the output it starts on ([client] sound.follow=0)");
        return;
    }
    if (!imports::when_loaded(L"FFXiMain.dll", &on_game_loaded))
        log::write(L"Sound: FFXiMain.dll can't be watched for, so the output isn't followed");
}

} // namespace vanacore::sound
