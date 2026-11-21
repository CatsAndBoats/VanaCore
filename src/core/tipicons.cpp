#include "tipicons.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "asm_text.hpp"
#include "itemtips.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::tipicons {
namespace {

// An item's details' draw (CTkItemInfo, 0x1013CA00, __thiscall; seen live 2026-11-05): sub esp,0A4h; push esi; mov esi,ecx;
// mov ecx,[esi+1Ch]; test ecx,ecx; jz. Its calls (E8 rel32) redirected here, each known by the bytes before it:
constexpr char kDetailsDraw[] = "81 EC A4 00 00 00 56 8B F1 8B 4E 1C 85 C9 0F 84";
// DrawIcon (0x1015A2D0, __thiscall on the item: x, y, scale, colour, 0, 0; ret 18h; itemrarity.cpp's hook) for its
// picture, 32 x 32 at x, y (the equipment grid's at a scale of 0.5): push edx; push 1.0f; push ebp (y); push ebx (x); call
constexpr std::ptrdiff_t kIconCall = 0x6D;
constexpr std::uint8_t kIconBefore[] = {0x52, 0x68, 0x00, 0x00, 0x80, 0x3F, 0x55, 0x53};
// the stack's count over it (0x101C82B0, __cdecl: x, y, text, colour, 1, scale x, scale y) 22h right and 16h down of its
// corner: add ebx,22h; push ebp; push ebx; call
constexpr std::ptrdiff_t kCountCall = 0xD1;
constexpr std::uint8_t kCountBefore[] = {0x83, 0xC3, 0x22, 0x55, 0x53};
constexpr int kCountX = 0x22, kCountY = 0x16;
// its badges (0x1013C790, __thiscall on the details: x, y; ret 8): mov ecx,esi; push edx (y); push eax (x); call
constexpr std::ptrdiff_t kBadgesCall = 0x1A0;
constexpr std::uint8_t kBadgesBefore[] = {0x8B, 0xCE, 0x52, 0x50};
// ... which draws each by the item's flags (Rare, Ex and two more, 16 x 16, left to right from x less 7 for each: x their
// row's middle) with YmMenuShape::Draw (0x1011F760, __thiscall shape: x, y, colour, blending, quads to skip; ret 14h) here
constexpr std::ptrdiff_t kBadgeCalls[] = {0x16D, 0x1A7, 0x1E2, 0x221};
// YmMenuShape::Draw with a scale (0x1011F880, __thiscall shape: x, y, scale x, scale y, colour, centred, blending, quads to
// skip; ret 20h: buttons.cpp's signature, its first 5 bytes left out as buttons.cpp and statuswindow.cpp hook them)
constexpr char kScaledDraw[] = "?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8";
constexpr float kIcon = 32.0f, kBadge = 16.0f;
// The type row under the name (seen live 2026-11-05): a weapon's skill ("(Sword)", 0x10159800) or a piece of armour's slot
// ("[Body]", 0x10159A20), then its races ("All Races ", 0x10159B30), each from a __cdecl getter of an id that gives a
// pointer into the game's own strings, or none; the draw prints each (0x10129A40, __cdecl: x, y, text, 0, colour) and moves
// on by its width, and skips one that's none. The getters' calls redirected here for [client] tips.type, and the prints'
// so that True Font knows the row's letters whatever they say (vanacore_gfx_tip_type). A getter's call: mov dx,[eax]; push
// edx; call (the skill's, the slot's) or mov ax,[eax]; push eax; call (the races'); a print's: push ebp (the text); push;
// push; call
enum class Getter { skill, slot, races };
struct TypeCall {
    std::ptrdiff_t get, print;
    Getter getter;
};
constexpr TypeCall kTypeCalls[] = {{0x1D8, 0x1F6, Getter::skill}, {0x217, 0x235, Getter::races},
                                   {0x262, 0x280, Getter::slot}, {0x2A1, 0x2BF, Getter::races}};
constexpr std::uint8_t kIdDx[] = {0x66, 0x8B, 0x10, 0x52}, kIdAx[] = {0x66, 0x8B, 0x00, 0x50};
constexpr std::uint8_t kPrintEax[] = {0x55, 0x50, 0x51}, kPrintEcx[] = {0x55, 0x51, 0x52};

using DrawIcon = void(__fastcall*)(void* item, void* edx, int x, int y, float scale, std::uint32_t colour, int a, int b);
using DrawCount = void(__cdecl*)(int x, int y, const char* text, std::uint32_t colour, int a, float sx, float sy);
using DrawBadges = void(__fastcall*)(void* details, void* edx, int x, int y);
using PlainDraw = void(__fastcall*)(void* shape, void* edx, int x, int y, std::uint32_t colour, int blending, int skip);
using ScaledDraw = void(__fastcall*)(void* shape, void* edx, int x, int y, float sx, float sy, std::uint32_t colour,
                                     int centred, int blending, int skip);
using TypeText = const char*(__cdecl*)(int id);
using Print = void(__cdecl*)(int x, int y, const char* text, int flag, std::uint32_t colour);

// Where a picture goes: this many of the interface's pixels across and down, this much larger round its middle
struct Place {
    int x = 0, y = 0;
    float size = 1.0f;
};
Place g_icon, g_badges; // [client] tips.icon.*, tips.badges.*

bool g_tried = false;
DrawIcon g_draw_icon = nullptr;
DrawCount g_draw_count = nullptr;
DrawBadges g_draw_badges = nullptr;
PlainDraw g_plain = nullptr;
ScaledDraw g_scaled = nullptr;
int g_row_x = 0, g_row_y = 0; // the badges' row being drawn: its middle across and its top

// [client] tips.type: the type row as the game writes it ("[Body]All Races"), simple ("Body - All Races") or the type alone
// ("Body")
enum class TypeLine { game, simple, type };
TypeLine g_type_line = TypeLine::game;
TypeText g_skill = nullptr, g_slot = nullptr, g_races = nullptr;
Print g_print = nullptr;
bool g_named = false;       // the type row being drawn has its skill or slot before its races (from the badges, drawn first)
char g_name[64] = {};       // ... that skill or slot without its brackets
char g_with_races[96] = {}; // ... its races after " - "

int round_int(float v) { return static_cast<int>(std::lround(v)); }

// The game's x or y argument: a short in an int (its high half what the register had)
int coord(int v) { return static_cast<short>(v); }

void __fastcall icon_hook(void* item, void* edx, int x, int y, float scale, std::uint32_t colour, int a, int b)
{
    const float grow = kIcon * scale * (g_icon.size - 1.0f) * 0.5f;
    g_draw_icon(item, edx, round_int(static_cast<float>(coord(x) + g_icon.x) - grow),
                round_int(static_cast<float>(coord(y) + g_icon.y) - grow), scale * g_icon.size, colour, a, b);
}

// The stack's count as far from the picture's corner, made larger, as the picture is
void __cdecl count_hook(int x, int y, const char* text, std::uint32_t colour, int a, float sx, float sy)
{
    const float s = g_icon.size, grow = kIcon * (s - 1.0f) * 0.5f;
    const float left = static_cast<float>(coord(x) - kCountX + g_icon.x) - grow;
    const float top = static_cast<float>(coord(y) - kCountY + g_icon.y) - grow;
    g_draw_count(round_int(left + kCountX * s), round_int(top + kCountY * s), text, colour, a, sx * s, sy * s);
}

void __fastcall badges_hook(void* details, void* edx, int x, int y)
{
    g_row_x = coord(x) + g_badges.x + itemtips::widened(details), g_row_y = coord(y) + g_badges.y;
    g_named = false; // (the type row comes next)
    g_draw_badges(details, edx, g_row_x, g_row_y);
}

// A skill's or a slot's text ("(Sword)", "[Body]"): without its brackets unless tips.type is the game's
const char* named(const char* text)
{
    g_named = text != nullptr;
    const std::size_t n = text != nullptr ? std::strlen(text) : 0;
    if (g_type_line == TypeLine::game || n < 3 || n > sizeof g_name)
        return text;
    if (!((text[0] == '[' && text[n - 1] == ']') || (text[0] == '(' && text[n - 1] == ')')))
        return text;
    std::memcpy(g_name, text + 1, n - 2);
    g_name[n - 2] = '\0';
    return g_name;
}

const char* __cdecl skill_hook(int id) { return named(g_skill(id)); }
const char* __cdecl slot_hook(int id) { return named(g_slot(id)); }

// The races' text ("All Races "): after " - " when simple and a skill or slot is before it, none for the type alone
const char* __cdecl races_hook(int id)
{
    const bool after = g_named;
    g_named = false;
    if (g_type_line == TypeLine::type)
        return nullptr;
    const char* text = g_races(id);
    const std::size_t n = text != nullptr ? std::strlen(text) : 0;
    if (g_type_line != TypeLine::simple || !after || n == 0 || n + 4 > sizeof g_with_races)
        return text;
    std::memcpy(g_with_races, " - ", 3);
    std::memcpy(g_with_races + 3, text, n + 1);
    return g_with_races;
}

void __cdecl type_print(int x, int y, const char* text, int flag, std::uint32_t colour)
{
    vanacore_gfx_tip_type(1);
    g_print(x, y, text, flag, colour);
    vanacore_gfx_tip_type(0);
}

// A badge: from the row's middle and the badges' (half a badge below its top) as much further as they're larger
void __fastcall badge_hook(void* shape, void* edx, int x, int y, std::uint32_t colour, int blending, int skip)
{
    const float s = g_badges.size, mid = static_cast<float>(g_row_y) + kBadge * 0.5f;
    if (s == 1.0f || g_scaled == nullptr) {
        g_plain(shape, edx, x, y, colour, blending, skip);
        return;
    }
    g_scaled(shape, edx, round_int(static_cast<float>(g_row_x) + static_cast<float>(x - g_row_x) * s),
             round_int(mid + (static_cast<float>(y) - mid) * s), s, s, colour, 0, blending, skip);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), sizeof rel);
    return call + 5 + rel;
}

// A call (E8 rel32) at `at`, after `before`
template <std::size_t N>
bool call_after(std::uintptr_t at, const std::uint8_t (&before)[N])
{
    return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8 &&
           std::memcmp(reinterpret_cast<const void*>(at - N), before, N) == 0;
}

void redirect(std::uintptr_t call, const void* to) { asm_text::branch(call, 0xE8, reinterpret_cast<std::uintptr_t>(to), 0); }

// The type row's getters' and prints' calls (kTypeCalls) in the details' draw at `draw` redirected, when each is as known
bool install_type(std::uintptr_t draw)
{
    const std::uintptr_t print = call_target(draw + kTypeCalls[0].print);
    std::uintptr_t found[3] = {0, 0, 0}; // (by Getter; addresses: cppcheck took an array of TypeText for uninitialised)
    for (const TypeCall& c : kTypeCalls) {
        const bool races = c.getter == Getter::races;
        const bool known = (races ? call_after(draw + c.get, kIdAx) : call_after(draw + c.get, kIdDx)) &&
                           (races ? call_after(draw + c.print, kPrintEcx) : call_after(draw + c.print, kPrintEax)) &&
                           call_target(draw + c.print) == print;
        std::uintptr_t& getter = found[static_cast<int>(c.getter)];
        const std::uintptr_t target = known ? call_target(draw + c.get) : 0;
        if (target == 0 || (getter != 0 && getter != target))
            return false;
        getter = target;
    }
    g_skill = reinterpret_cast<TypeText>(found[0]), g_slot = reinterpret_cast<TypeText>(found[1]);
    g_races = reinterpret_cast<TypeText>(found[2]), g_print = reinterpret_cast<Print>(print);
    const void* hooks[3] = {reinterpret_cast<const void*>(&skill_hook), reinterpret_cast<const void*>(&slot_hook),
                            reinterpret_cast<const void*>(&races_hook)};
    for (const TypeCall& c : kTypeCalls) {
        redirect(draw + c.get, hooks[static_cast<int>(c.getter)]);
        redirect(draw + c.print, reinterpret_cast<const void*>(&type_print));
    }
    return true;
}

void install_hooks()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_tried = true;
    const std::uintptr_t draw = scan::find(game, kDetailsDraw), base = reinterpret_cast<std::uintptr_t>(game);
    if (draw != 0 && !install_type(draw))
        log::write(L"Item details' type row: its draw's calls weren't as known: [client] tips.type does nothing");
    if (draw == 0 || !call_after(draw + kIconCall, kIconBefore) || !call_after(draw + kCountCall, kCountBefore) ||
        !call_after(draw + kBadgesCall, kBadgesBefore)) {
        log::write(L"Item details' pictures: their draw wasn't found in FFXiMain.dll: [client] tips.icon.* and tips.badges.* do "
                   L"nothing");
        return;
    }
    const std::uintptr_t badges = call_target(draw + kBadgesCall), plain = call_target(badges + kBadgeCalls[0]);
    const bool each = std::all_of(std::begin(kBadgeCalls), std::end(kBadgeCalls), [&](std::ptrdiff_t at) {
        return *reinterpret_cast<const std::uint8_t*>(badges + at) == 0xE8 && call_target(badges + at) == plain;
    });
    g_draw_icon = reinterpret_cast<DrawIcon>(call_target(draw + kIconCall));
    g_draw_count = reinterpret_cast<DrawCount>(call_target(draw + kCountCall));
    g_draw_badges = reinterpret_cast<DrawBadges>(badges);
    redirect(draw + kIconCall, reinterpret_cast<const void*>(&icon_hook));
    redirect(draw + kCountCall, reinterpret_cast<const void*>(&count_hook));
    redirect(draw + kBadgesCall, reinterpret_cast<const void*>(&badges_hook));
    if (each) {
        g_plain = reinterpret_cast<PlainDraw>(plain);
        g_scaled = reinterpret_cast<ScaledDraw>(scan::find(game, kScaledDraw));
        for (const std::ptrdiff_t at : kBadgeCalls)
            redirect(badges + at, reinterpret_cast<const void*>(&badge_hook));
    }
    log::write(L"Item details' pictures: their draw's calls redirected at +0x%X%ls", static_cast<unsigned>(draw - base),
               each && g_scaled != nullptr ? L"" : L"; the badges' draws weren't as known: tips.badges.size does nothing");
}

Place read_place(const wchar_t* ini, const wchar_t* key)
{
    const std::wstring prefix = key;
    Place place;
    place.x = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", (prefix + L".x").c_str(), 0, ini)), -128, 128);
    place.y = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"client", (prefix + L".y").c_str(), 0, ini)), -128, 128);
    const int size = static_cast<int>(GetPrivateProfileIntW(L"client", (prefix + L".size").c_str(), 100, ini));
    place.size = static_cast<float>(size <= 0 ? 100 : std::clamp(size, 25, 400)) / 100.0f; // (0 or unset: as the game's)
    return place;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_icon = read_place(ini, L"tips.icon");
    g_badges = read_place(ini, L"tips.badges");
    wchar_t type[16] = {};
    GetPrivateProfileStringW(L"client", L"tips.type", L"game", type, static_cast<DWORD>(std::size(type)), ini);
    g_type_line = _wcsicmp(type, L"simple") == 0 ? TypeLine::simple : _wcsicmp(type, L"type") == 0 ? TypeLine::type : TypeLine::game;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (!g_tried)
        install_hooks();
}

} // namespace vanacore::tipicons
