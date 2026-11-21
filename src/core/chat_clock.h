// [client] clock.always=1 (chat.cpp): the clock under the compass kept on screen while a menu is up. chat.cpp includes this
// inside its anonymous namespace, after clock_draw_hook; it is no file of its own.
//
// What took the clock away (found live 2026-11-11): the menu manager fades the compass out (0x1021d490: KaCompass's +0xd,
// its fade state, set to 4; 3 is shown, 1 fading in, 0 gone) as a window of the kind that covers it opens (Status's pages
// and the like: 0x1015da40 with 4, at 0x1015eee7 and 0x1015f982), and in again (0x1021d480: +0xd 1) once the last has
// closed. The compass's draw (0x1021ea70) draws nothing at +0xd 0 and the clock with the compass's fade, so the clock went
// with it. Each of those two is `mov ecx,[compass]; test ecx,ecx; je; jmp <the fade>`: the jmps are pointed here.
// While clock.always is on, the fade out is left undone and the compass's draw goes the way it goes with the compass off,
// the clock alone (its call of 0x100ad5c0, true while the compass is to be drawn, answered false), until the fade in,
// which is left undone too (the compass is shown already: its fade in would bring the clock in again from nothing).
// What hides the whole HUD (0x1015ca10, asked by the compass's draw: the HUD's hide key, being KO'd) still hides it.
using CompassFade = void(__fastcall*)(char* compass); // __thiscall, nothing else
using CompassShown = bool(__cdecl*)();
CompassFade g_compass_fade_in = nullptr; // the game's
CompassFade g_compass_fade_out = nullptr;
CompassShown g_compass_shown = nullptr;
bool g_clock_always = false;  // clock.always
char* g_clock_kept = nullptr; // the compass whose fade out was left undone (a menu is up), or null
constexpr int kCompassFade = 0xd;
constexpr std::uint8_t kCompassShown = 3;

void __fastcall compass_fade_out_hook(char* compass)
{
    if (g_clock_always && !g_clock_hide) {
        g_clock_kept = compass;
        return;
    }
    g_compass_fade_out(compass);
}

void __fastcall compass_fade_in_hook(char* compass)
{
    const bool kept = compass == g_clock_kept;
    g_clock_kept = nullptr;
    if (kept && static_cast<std::uint8_t>(compass[kCompassFade]) == kCompassShown)
        return;
    g_compass_fade_in(compass);
}

bool __cdecl compass_shown_hook()
{
    if (g_clock_kept != nullptr && *g_compass == g_clock_kept)
        return false; // a menu is up: the clock alone
    return g_compass_shown();
}

// Each frame: a kept compass that's gone (made again at a zone change) is forgotten, and with clock.always turned off (or
// clock.hide on) while the menu is still up, the kept one gets the game's fade out after all
void keep_clock()
{
    if (g_clock_kept == nullptr)
        return;
    char* compass = *g_compass;
    if (compass != g_clock_kept) {
        g_clock_kept = nullptr;
        return;
    }
    if (!g_clock_always || g_clock_hide) {
        g_clock_kept = nullptr;
        g_compass_fade_out(compass);
    }
}

// The compass's fade in and out (two little functions one after the other, each on the compass's global), and the
// compass's draw's call asking whether the compass is drawn, just before its call of the clock's draw (`clock`, as found)
void install_clock_always(HMODULE game, std::uintptr_t clock)
{
    const auto at = reinterpret_cast<std::uintptr_t>(g_compass);
    char fades_pattern[128] = {};
    std::snprintf(fades_pattern, sizeof fades_pattern,
                  "8B 0D %02X %02X %02X %02X 85 C9 74 05 E9 ?? ?? ?? ?? C3 8B 0D %02X %02X %02X %02X 85 C9 74 05 E9",
                  static_cast<unsigned>(at & 0xFF), static_cast<unsigned>((at >> 8) & 0xFF),
                  static_cast<unsigned>((at >> 16) & 0xFF), static_cast<unsigned>(at >> 24), static_cast<unsigned>(at & 0xFF),
                  static_cast<unsigned>((at >> 8) & 0xFF), static_cast<unsigned>((at >> 16) & 0xFF),
                  static_cast<unsigned>(at >> 24));
    const std::uintptr_t fades = g_compass != nullptr ? scan::find(game, fades_pattern) : 0;
    // the fades themselves start mov byte ptr [ecx+0Dh],1 (in) and 4 (out)
    const auto fade = [](std::uintptr_t fn, std::uint8_t state) {
        const std::uint8_t start[] = {0xC6, 0x41, static_cast<std::uint8_t>(kCompassFade), state};
        return std::memcmp(reinterpret_cast<const void*>(fn), start, sizeof start) == 0;
    };
    // the compass's draw: movsx esi,[ebx+2Ah] (its bottom), movsx edi,[ebx+28h] (its middle), less its height, the call,
    // test al,al; jne (the compass and then the clock); push esi; push edi; mov ecx,ebx; call the clock's draw
    const std::uintptr_t draw =
        scan::find(game, "0F BF 73 2A 0F BF 7B 28 2B F0 89 7C 24 44 89 74 24 28 E8 ?? ?? ?? ?? 84 C0 75 14 56 57 8B CB E8");
    if (fades == 0 || draw == 0 || clock == 0 || call_target(draw + 0x1F) != clock || !fade(call_target(fades + 0xA), 1) ||
        !fade(call_target(fades + 0x1A), 4)) {
        log::write(L"Chat: the compass's fades weren't found in FFXiMain.dll: [client] clock.always does nothing");
        return;
    }
    g_compass_fade_in = reinterpret_cast<CompassFade>(call_target(fades + 0xA));
    g_compass_fade_out = reinterpret_cast<CompassFade>(call_target(fades + 0x1A));
    g_compass_shown = reinterpret_cast<CompassShown>(call_target(draw + 0x12));
    redirect_call(fades + 0xA, &compass_fade_in_hook); // the jmps, as calls' are: E9 and its rel32
    redirect_call(fades + 0x1A, &compass_fade_out_hook);
    redirect_call(draw + 0x12, &compass_shown_hook);
}
