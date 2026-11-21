#include "chatfont.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "chatline.hpp"
#include "gametext.hpp"
#include "gamewin.hpp"
#include "hudwin.hpp"
#include "imports.hpp"
#include "itemtips.hpp"
#include "jplabels.hpp"
#include "log.hpp"
#include "macrotext.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "tipicons.hpp"
#include "uiscale.hpp"

namespace vanacore::chatfont {
namespace {

// The game's letter widths: FFXiMain's pen advance (0x1011D440: `cmp ax, 440h / jae / movsx eax, ax / movsx ecx, byte ptr
// [eax + table]`) and its character width (0x1011CAC0, which the line wrap adds up) read one signed byte per character code
// below 0x440 from a table in .data (0x1036A430), the chat's add-line and its drawing alike. A code is the cell of the font
// sheet (row * 64 + column): ASCII's is the byte less 0x20 (0x1011CA80).
constexpr const char* kWidthTable = "66 3D 40 04 73 17 0F BF C0 0F BE 88 ?? ?? ?? ??";
// The game's drawing of one chat row (0x10160BB0, cdecl, 22 dwords: the row's text, the row itself by value, x, y, an alpha
// and three more), which the chat windows' draw (0x10160DA0, chat.cpp's LogDraw) and the chat log maximized's (0x10136705,
// 0x10136CDA) both call: mov eax,[esp+58h]; mov ecx,[esp+54h]; mov edx,[esp+50h]; push eax; mov eax,[esp+50h]; push ecx;
// mov ecx,[esp+50h]; push edx; mov edx,[esp+50h]; push eax; movzx ax,byte ptr [esp+18h]
constexpr const char* kDrawRow = "8B 44 24 58 8B 4C 24 54 8B 54 24 50 50 8B 44 24 50 51 8B 4C 24 50 52 8B 54 24 50 50 66 0F B6 44 24 18";
struct RowArgs {
    const void* text;
    std::uint32_t row[15];
    int x, y, alpha, a, b, c;
};
using DrawRow = int(__cdecl*)(RowArgs);
DrawRow g_draw_row = nullptr; // the game's own, through the trampoline
// An item's details (CTkItemInfo::SetLines, 0x1013BCC0, __thiscall, ret 4: its count of lines, held to 3 to 12): the window
// made from the layout for that many ("menu    item<n>inf"), its box that tall. mov eax,[esp+4]; push esi; mov esi,ecx;
// push edi; mov edi,eax; cmp eax,[esi+36Ch]; je; cmp eax,3; jge; mov edi,3
constexpr const char* kSetLines = "8B 44 24 04 56 8B F1 57 8B F8 3B 86 6C 03 00 00 74 ?? 83 F8 03 7D 07 BF 03 00 00 00";
using SetLines = void(__fastcall*)(void*, void*, int);
SetLines g_set_lines = nullptr;
// The game's letter printer (0x1011B7D0, __thiscall, ret 14h; 0x1011B7A0 calls it with the colour 0x80808080 for 0x1011BB40, the
// pen's letter): one letter of the font sheet, `code` its cell (row * 64 + column, a short: 0x2115 and up are the icons', drawn
// another way), at pen `pos` (x, y, the interface's pixels), sx x sy times 16 pixels, `colour` over the quad's own; a strip of
// four XYZRHW vertices (FVF 0x144, stride 1Ch, u = column / 64, v = row / 128) by DrawPrimitiveUP. Hooked so that the graphics
// layer knows the draw to come is a letter, and which, whatever its shape (d3d8_chatfont.h's cf_printed). mov eax,[esp+8]; sub
// esp,10h; fild dword ptr [eax]; push ebx; push ebp; push esi; mov esi,ecx; fsub [half]; push edi; fst [esi+38h]; fstp [esi];
// fld [esp+30h]; fmul [sixteen]; fiadd dword ptr [eax]
constexpr const char* kLetter = "8B 44 24 08 83 EC 10 DB 00 53 55 56 8B F1 D8 25 ?? ?? ?? ?? 57 D9 56 38 D9 1E D9 44 24 30 D8 0D ?? ?? ?? ?? DA 00";
using Letter = void(__fastcall*)(void*, void*, int, const int*, std::uint32_t, float, float);
Letter g_letter = nullptr;
// The macro menus' name printer (0x101B7EC0, __thiscall, ret 14h: x, y, the name, its colour, the room it has; what it gives
// back no caller reads. Called from 0x101A23F3 ... 0x101A55F7: the books' list, the equip sets' list, the macro edit windows):
// the name a letter at a time, each a shape of the printer's own font (this +4: '1' to '9', '0', 'a' to 'z', 'A' to 'Z', anything
// else passed over), sprites of the "menu    keytop" sheet standing on y + 11, stopped before one would pass x + room. No text,
// so True Font couldn't draw it, and taken by their sprites for labels' letters, the keytop's R, N, U and M (where the menu2fon
// sheet's 6, 3, 8 and 2 are) made a book called "RUN" read "683" (reported 2026-11-06). With the menus' True Font on, the name
// is printed instead in the menu font, the game's own text (kPrintString), which True Font draws as any other menu text: every
// letter the name has, spaces too. push ebx; mov ebx,ecx; mov eax,[ebx+4]; test eax,eax; jne +7; or eax,-1; pop ebx; ret 14h
constexpr const char* kKeytopPrint = "53 8B D9 8B 43 04 85 C0 75 07 83 C8 FF 5B C2 14 00";
using KeytopPrint = int(__fastcall*)(void*, void*, int, int, const char*, std::uint32_t, int);
KeytopPrint g_keytop_print = nullptr;
constexpr int kKeytopBaseline = 11; // below the printer's y
constexpr int kMenuBaseline = 14;   // below a menu font cell's top (its capitals rows 2 to 13)
// The menu font (the "font    moji" sheet), as itemcount.cpp prints in it: the game's one routine for a whole string
// (FUN_1011ba10, __thiscall on the renderer, ret 8: the string, int pos[2]) sets the blending, binds the texture, then per
// letter takes its code, prints it and steps on by its width. sub esp,8; mov eax,[esp+10h]; push ebx; push ebp; mov ebp,
// [esp+14h]; mov edx,[eax+4]; push esi; mov esi,ecx; mov ecx,[eax]; mov [esp+0Ch],ecx; push edi; mov ecx,esi; mov [esp+14h],
// edx; mov [esp+20h],ebp; call states; mov ecx,esi; call bind
constexpr char kPrintString[] = "83 EC 08 8B 44 24 10 53 55 8B 6C 24 14 8B 50 04 56 8B F1 8B 08 89 4C 24 0C 57 8B CE 89 54 24 14 "
                                "89 6C 24 20 E8 ?? ?? ?? ?? 8B CE E8";
constexpr std::ptrdiff_t kStatesCall = 0x24, kBindCall = 0x2B, kCodeCall = 0x50, kPrintCall = 0x93, kAdvanceCall = 0x9E;
// The renderer, from the title bar's own drawing (CTkTitle::OnDrawPrimitive): mov ecx,[renderer]; call states; mov ecx,
// [renderer]; call bind; mov eax,[renderer]; lea ecx,[esi+0A4h]; mov dword ptr [eax+64h],80808080h
constexpr char kTitleText[] = "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? "
                              "8D 8E A4 00 00 00 C7 40 64 80 80 80 80";
constexpr std::ptrdiff_t kRendererAt = 2;
constexpr std::ptrdiff_t kCornerColours[] = {0x10, 0x2C, 0x48, 0x64}; // the renderer: its quad's corners' colours
constexpr std::uint32_t kWhite = 0x80808080;
using States = void(__thiscall*)(void* renderer);
using Bind = std::uint8_t(__thiscall*)(void* renderer);
using NextCode = int(__cdecl*)(const char** text);
using PrintCode = void(__thiscall*)(void* renderer, int code, int* pos, float sx, float sy);
using Advance = std::uint8_t(__cdecl*)(int code, int* pos);
struct MenuFont {
    char** renderer = nullptr;
    States states = nullptr;
    Bind bind = nullptr;
    NextCode next_code = nullptr;
    PrintCode print = nullptr;
    Advance advance = nullptr;
} g_menu_font;
// [client] truefont.debug: True Font's steps counted and reported (report)
bool g_debug = false;
// [client] macrobar.prefix=0: the macro bar's keys show their numbers alone, whichever letters (d3d8_chatfont.h cf_macro_prefix:
// the macro bar's windows are told apart and the labels' sprites known for it, with True Font off too)
bool g_macro_bare = false;
// [client] equip.font=hide: the Equipment window's slot names left out (vanacore_gfx_hide_letters; its window told apart and
// the labels' sprites known for it, with True Font off too)
bool g_equip_hide = false;
// equip.font=sharp (True Font on, its size not 0): the slot names are VanaCore's own whole text (listtext.cpp), the game's letters
// left out the same way (a texture is not replaced: the names are always the same words, so no letter of the game's is kept)
bool g_equip_whole = false;
struct Counts {
    unsigned drawings = 0, rows = 0, wraps = 0, lines = 0, areas = 0, holds = 0, letters = 0;
} g_counts;
// The help line's message draw (KaHelpMes's, 0x1021FB90, __thiscall, ret: the help bar's text, the window's handler +0x20):
// from the letter the scroll has reached (+0x48) at x less the scrolled pixels (+0x4C, sixteenths, moved on a pixel a frame by
// 0x1021FAA0, which takes the next letter once a whole one has gone), up to just past its box's right, while +0x3A0 is 1
// (about to scroll) or 2 (scrolling); 0 still. Hooked so the letters of a scrolling one are laid each where the game's is
// (vanacore_gfx_text_rolling): a word at a time from its first letter, the rest of the word jumped as each letter went off the
// left and the line looked laggy (reported 2026-11-06). sub esp,44h; push ebx; mov ebx,ecx; mov eax,[ebx+64h]; test eax,eax;
// je +1Eh; mov eax,[ebx+5Ch]; push 1; push eax; mov eax,[ebx+60h]
constexpr const char* kHelpDraw = "83 EC 44 53 8B D9 8B 43 64 85 C0 74 1E 8B 43 5C 6A 01 50 8B 43 60";
constexpr std::ptrdiff_t kHelpScroll = 0x3A0;
using HelpDraw = void(__fastcall*)(void*, void*);
HelpDraw g_help_draw = nullptr;
// The game's text draw that can centre (0x101C7F10, __cdecl, the caller cleans up: x, y, the text, its colour, sx, sy, centred):
// with its last argument's low byte set, x less half the text's width at sx. Only the status icons' timers ask that (FUN_10226740:
// 0.85, 0.85, 1); the 80 other callers go through 0x101C7EE0, which never does (read live 2026-11-07). It draws each letter there
// and then, a shape of the labels' sheet (0x101C7390's set, by the menu shape draw 0x1011F880 at sx, sy), so the timers are label
// letters, not the font sheet's. Hooked so such a text's letters are laid centred on x (vanacore_gfx_text_centred), not from the
// game's left. sub esp,8; push ebx; mov ebx,[esp+18h]; push ebp; push esi; test ebx,ebx; jne +0Ch; movsx eax,word ptr [esp+18h]
constexpr const char* kCentredText = "83 EC 08 53 8B 5C 24 18 55 56 85 DB 75 0C 0F BF 44 24 18";
using CentredText = int(__cdecl*)(int, int, const char*, std::uint32_t, float, float, int);
CentredText g_centred_text = nullptr;
// The game's right-aligned text draw (0x101C82B0, __cdecl, the caller cleans up: x, y, the text, its colour, a flag, sx, sy; x and y
// read as shorts): the text's last letter first, each a shape of 0x101C7390(0)'s set stepped left by its width (less one), so the
// text ends at x; its digits are the name plates' big letters, its brackets, signs and slashes the labels' smaller ones. 130
// callers (read from FFXiMain.dll 2026-11-08): the skill panels' levels (the Craft Skills' "(%d)" at 0x10148422, Combat and Magic
// Skills' "%d"), the Status window's "%4d/", merits' "[%d]", "%d P", "%3d%%", the party list's MP and TP (partylist.cpp points
// those calls at itself, then here), an item's count in its details (tipicons.cpp). Hooked so its letters are laid as one text in
// one font, ending where the game's end (vanacore_gfx_text_right): laid as words by the big letters' rules, the Craft Skills' "(7)"
// was three, "(" centred in its narrow sprite apart from the 7 and ")" over it, "( )7" (reported 2026-11-08). push ecx; push esi;
// mov esi,[esp+14h]; test esi,esi; jne +8; movsx eax,word ptr [esp+0Ch]; pop esi; pop ecx; ret; push ebx; push edi; push 0; call
constexpr const char* kRightText = "51 56 8B 74 24 14 85 F6 75 08 0F BF 44 24 0C 5E 59 C3 53 57 6A 00 E8";
using RightText = int(__cdecl*)(int, int, const char*, std::uint32_t, int, float, float);
RightText g_right_text = nullptr;
bool g_hooked[8] = {}; // the chat row drawer, the chat windows' line wrap, the item details' SetLines, the letter printer,
                       // the macro menus' name printer, the help line's draw, the centring text draw, the right-aligned text draw

bool g_tips_on = false;       // the tooltips' font is built
float g_tips_line = 1.0f;     // ... their line (tips.font.lineheight / 16), else 1
float g_tips_pads = 0.0f;     // ... the interface's pixels their paddings take
float g_tips_guess = 0.0f;    // ... what their parts' sizes and spaces may need more, before any tooltip's rows are measured
float g_tips_measured = -1.0f; // what the last tooltip laid onto the screen needed more (d3d8_tips.h), -1 none yet
void* g_tips_self = nullptr;  // the item details last given their lines, while they may want them again (tips_again)
int g_tips_lines = 0;         // ... the game's count of them
int g_tips_frames = 0;        // ... frames left to give them the count their rows turn out to need

// The interface's pixels more the item details' box is given than the game's `lines` rows (16 apart) take, to the pixel:
// their rows spread (the last moved down as far as the rows above it), the paddings, and what their parts' sizes and spaces
// moved them down, what the tooltip laid last needed, a guess before any was laid ([client] tips.font.lineheight,
// .padding.top, .padding.bottom, .<part>.size and .space)
int tips_px(int lines)
{
    const float measured = vanacore_gfx_tips_extra();
    if (measured >= 0.0f)
        g_tips_measured = measured;
    if (!g_tips_on || lines <= 0)
        return 0;
    const float spread = static_cast<float>(lines - 1) * 16.0f * (g_tips_line - 1.0f);
    return static_cast<int>(std::lround(spread + g_tips_pads + (g_tips_measured >= 0.0f ? g_tips_measured : g_tips_guess)));
}

// How wide the details' box at left, top, right, bottom (the interface's pixels, as the game laid it) is made: as wide as the
// tooltip laid in it needs for its parts' sizes (d3d8_tips.h), 0 (as it is) with none laid there
int tips_wide(int left, int top, int right, int bottom)
{
    const float wide = vanacore_gfx_tips_width(static_cast<float>(left), static_cast<float>(top), static_cast<float>(right),
                                               static_cast<float>(bottom));
    return wide > 0.0f ? static_cast<int>(std::ceil(wide)) : 0;
}

// The details' box for a new item, with the last tooltip's measure (its rows aren't laid yet); tips_again puts it right
// once they are. (SetLines runs only when the item changes: called at 0x1013C785 by the details' set-item, which looks the
// item up with 0x1015A710; with 0 lines at 0x1013BB8A, as they're made.)
void __fastcall set_lines_hook(void* self, void* /*edx*/, int lines)
{
    ++g_counts.lines;
    const int px = tips_px(lines);
    if (lines > 0)
        g_tips_self = self, g_tips_lines = lines, g_tips_frames = 30;
    // (past 12 rows its box made taller: [client] tips.extended)
    itemtips::set_lines(self, {lines, px, g_tips_on ? &tips_wide : nullptr, false}, g_set_lines);
}

// A letter of the game's drawn: the graphics layer told which, before and after the game's draw of it
void __fastcall letter_hook(void* quad, void* edx, int code, const int* pos, std::uint32_t colour, float sx, float sy)
{
    ++g_counts.letters;
    gamewin::letter(code, pos[0], pos[1], sx, sy, colour); // vana.gameui.window: the window drawn now's text, when it's kept
    vanacore_gfx_letter_draw(1, static_cast<short>(code), pos[0], pos[1], sx, sy);
    g_letter(quad, edx, code, pos, colour, sx, sy);
    vanacore_gfx_letter_draw(0, 0, 0, 0, 0.0f, 0.0f);
}

// A text the game draws centred on x (the status timers): its letters told to the graphics layer as one centred word
int __cdecl centred_text_hook(int x, int y, const char* text, std::uint32_t colour, float sx, float sy, int centred)
{
    const bool mark = (centred & 0xFF) != 0; // (the game reads the byte alone)
    if (mark)
        vanacore_gfx_text_centred(1, static_cast<float>(static_cast<short>(x))); // (the game reads x as a short)
    const int result = g_centred_text(x, y, text, colour, sx, sy, centred);
    if (mark)
        vanacore_gfx_text_centred(0, 0.0f);
    return result;
}

// A text the game right-aligns at x (the skill panels' levels, the Status window's numbers): its letters told to the graphics
// layer as one text, laid by the font to end where the game's do
int __cdecl right_text_hook(int x, int y, const char* text, std::uint32_t colour, int flag, float sx, float sy)
{
    vanacore_gfx_text_right(1);
    const int result = g_right_text(x, y, text, colour, flag, sx, sy);
    vanacore_gfx_text_right(0);
    return result;
}

// For a few frames after the details were given their lines: their box sized again as the rows laid since need it (their
// parts measured: a job list, larger stats, a wider row), never laid out again
void tips_again()
{
    if (g_tips_frames <= 0 || g_tips_self == nullptr || g_set_lines == nullptr)
        return;
    --g_tips_frames;
    if (vanacore_gfx_tips_extra() < 0.0f) // (no tooltip on the screen last frame: nothing measured)
        return;
    itemtips::set_lines(g_tips_self, {g_tips_lines, tips_px(g_tips_lines), g_tips_on ? &tips_wide : nullptr, true}, g_set_lines);
}

// A chat row drawn as chat.font has it: the font's widths and glyphs, chat.shadow's shadow (the log maximized's too, which
// isn't one of the chat windows chat.cpp knows)
int __cdecl draw_row_hook(RowArgs args)
{
    const Drawing drawing;
    ++g_counts.rows;
    return g_draw_row(args);
}
// A chat window taking a line of the log as its rows (0x10128BD0, __thiscall, ret 0x14; from its newer and older lines,
// 0x1012A270 and the like): the game wraps the line again for the window, after the log's add-line wrapped it, so that wrap
// takes the chat font's widths too. With the game's, a line the font's widths had wrapped once was cut again ("... an
// exploit, / but because it / conflicts with ...", 2026-11-04). mov eax,123Ch; call _chkstk; mov eax,[...]; push ebx;
// push ebp; push esi; mov esi,ecx; xor ebx,ebx; movsx ecx,word ptr [eax+32h]
constexpr const char* kWindowRows = "B8 3C 12 00 00 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 53 55 56 8B F1 33 DB 0F BF 48 32";
using WindowRows = int(__fastcall*)(void*, void*, int, int, int, int, int);
WindowRows g_window_rows = nullptr;

int __fastcall window_rows_hook(void* self, void* edx, int a, int b, int c, int d, int e)
{
    ++g_counts.wraps;
    hold(true);
    const int result = g_window_rows(self, edx, a, b, c, d, e);
    hold(false);
    return result;
}
constexpr std::size_t kWidths = 0x440;
constexpr int kAscii = 0x5F; // codes 0 to 0x5E: ' ' to '~'
// Past ASCII, cells the font's letters stand for too: the brackets round an auto-translate category (Tab's list, and the chat
// line while one is offered), Shift-JIS 81 79 / 81 7A, cells 153 and 154 by the game's conversion (0x1011CA80). Drawn as the
// game's they kept its widths, wider than the font's, and a word's closing one stood apart from it ("[Languages ]", reported
// 2026-11-06); as the font's [ and ] they're laid with the word.
struct Extra {
    int code;
    wchar_t letter;
};
constexpr Extra kExtras[] = {{153, L'['}, {154, L']'}};
constexpr int kGlyphs = kAscii + static_cast<int>(sizeof kExtras / sizeof kExtras[0]);
int code_of(int n) { return n < kAscii ? n : kExtras[n - kAscii].code; }
wchar_t letter_of(int n) { return n < kAscii ? static_cast<wchar_t>(0x20 + n) : kExtras[n - kAscii].letter; }
// The game's letters in their 16 x 16 cell (the sheet "font moji", ROM\119\51): capitals from row 2 to row 13, standing on
// row 14. The font's stand there too.
constexpr float kBaseline = 14.0f;
constexpr int kAtlasWidth = 1024; // (a long label's words at a large size fit)

// True Font's areas (the graphics layer's VANA_FONT_*), each its own font: the chat rows', laid out by the font's own widths;
// the font sheet's text in the windows (the menus and lists, the item lists, the descriptions and help), each letter centred
// in the game's width for it, so no window's layout moves; the menus' labels and the name plates' big letters, each centred
// in its sprite. Their settings' prefix ([client] <prefix>, .family, .size), their size by default (the interface's pixels:
// the chat's rows are 16 apart; the labels' capitals are 9 to the font sheet's 12) and their name in the log.
enum Which {
    kChat = VANA_FONT_CHAT,
    kMenus = VANA_FONT_MENUS,
    kButtons = VANA_FONT_BUTTONS,
    kTitles = VANA_FONT_TITLES, // the labels that aren't a button's (the windows' titles): the buttons' keys until their own are set
    kItems = VANA_FONT_ITEMS,
    kHelp = VANA_FONT_HELP,
    kStatus = VANA_FONT_STATUS,
    kClock = VANA_FONT_CLOCK, // the clock under the compass: its own keys alone (it followed the Status area's until 2026-11-06)
    kInput = VANA_FONT_INPUT, // the chat input line (its window "menu    inline"): menu.font's switch until input.font is set
    kParty = VANA_FONT_PARTY, // the party list ("menu    partywin"): its big letters and labels' letters; status.font's keys until set
    kAlliance = VANA_FONT_ALLIANCE, // the alliance lists ("menu    raid1", "raid2"): the same
    kPhrases = VANA_FONT_PHRASES, // the auto-translate list ("menu    fep"): menu.font's switch until phrases.font is set
    kTrade = VANA_FONT_TRADE, // the trade windows (a trade's, the delivery box's): every letter, amounts and labels
    kGil = VANA_FONT_GIL, // Current Gil's window ("menu    money", wherever it shows): the same, its own keys alone
    kProfile = VANA_FONT_PROFILE, // the Status screen's Profile panel ("menu    profile"): the same, its own keys alone
    kProfileTitle = VANA_FONT_PROFILE_TITLE, // its titles' letters (profilewin.cpp TitleQuad): profile.font's keys until set
    kMainMenu = VANA_FONT_MAINMENU, // the main menu's buttons' labels ("menu    menuwind", "socialme", the menus they open in their
                                    // place, at any depth: kWindows, area_of): the buttons' keys until set
    kEquip = VANA_FONT_EQUIP, // the Equipment window's slot names (its labels' letters), its own keys alone; or left out (hide)
    kEquipStats = VANA_FONT_EQUIP_STATS, // the Equipment window's stats (its numbers, their signs, the stats under it): the same
    kMacroBar = VANA_FONT_MACROBAR, // the macro bar's keys' labels ("Ctrl1", "Alt0"): its own keys alone (the names the menus')
    kBuffs = VANA_FONT_BUFFS, // the status icons' timers ("menu    buff    "): menu.font's switch until buffs.font is set
    kActionMenu = VANA_FONT_ACTIONMENU, // the Action Menu's buttons' labels ("menu    playermo", its Magic and Abilities lists:
                                        // kWindows): the buttons' keys until set, as the main menu's
    kTarget = VANA_FONT_TARGET, // the Target window's ("menu    targetwi") name, the big letters: the Status area's keys until set
    kTargetTitle = VANA_FONT_TARGET_TITLE, // its title, "Target" (its labels' letters): the titles' keys until set (key_for)
    kChatTabs = VANA_FONT_CHATTABS, // the chat log's tabs' names, drawn whole by chattabs.cpp: the titles' keys until set (key_for)
    kStatusName = VANA_FONT_STATUS_NAME, // the Status window's player name, drawn whole by statuswindow.cpp: its own keys alone
    kLists = VANA_FONT_LISTS, // Menu Lists: the font sheet text of the windows in kListWindows (the Merit Points menu's), the
                              // menus' keys until its own are set (key_for)
    kMacroNames = VANA_FONT_MACRO_NAMES, // the macro bar's macros' names, drawn whole by macrotext.cpp: the menus' keys until set (key_for)
    kTips = VANA_FONT_TIPS,
    kTipsTitle = VANA_FONT_TIPS_TITLE, // the tooltips' parts (VANA_TIP_*) in their font at their own sizes: read_part, not read_want
    kTipsType = VANA_FONT_TIPS_TYPE,
    kTipsStats = VANA_FONT_TIPS_STATS,
    kTipsJobs = VANA_FONT_TIPS_JOBS,
    kTipsSignature = VANA_FONT_TIPS_SIGNATURE,
    kTipsTimer = VANA_FONT_TIPS_TIMER,
    kPartyNumbers = VANA_FONT_PARTY_NUMBERS, // the party list's numbers at their own size: read_numbers, not read_want
    kAllianceNumbers = VANA_FONT_ALLIANCE_NUMBERS, // the alliance lists'
    kAreas = VANA_FONTS
};
struct AreaInfo {
    const wchar_t* prefix;
    float size;
    const wchar_t* name;
};
constexpr AreaInfo kArea[kAreas] = {{L"chat.font", 16.0f, L"Chat"},       {L"menu.font", 15.0f, L"Menus"},
                                    {L"buttons.font", 11.0f, L"Buttons"}, {L"titles.font", 11.0f, L"Titles"},
                                    {L"items.font", 15.0f, L"Item lists"},
                                    {L"help.font", 15.0f, L"Help"},       {L"status.font", 11.0f, L"Status"},
                                    {L"clock.font", 11.0f, L"Clock"},     {L"input.font", 15.0f, L"Chat input"},
                                    {L"party.font", 11.0f, L"Party list"}, {L"alliance.font", 11.0f, L"Alliance lists"},
                                    {L"phrases.font", 15.0f, L"Auto-translate list"},
                                    {L"trade.font", 11.0f, L"Trade windows"},
                                    {L"gil.font", 11.0f, L"Current Gil"},
                                    {L"profile.font", 11.0f, L"Profile"},
                                    {L"profile.title.font", 11.0f, L"Profile titles"},
                                    {L"mainmenu.font", 11.0f, L"Main commands"},
                                    {L"equip.font", 11.0f, L"Equipment slots"},
                                    {L"equip.stats.font", 11.0f, L"Equipment stats"},
                                    {L"macrobar.font", 11.0f, L"Macro bar keys"},
                                    {L"buffs.font", 13.0f, L"Status timers"}, // (the game draws them at 0.85: 13 to the menus' 15)
                                    {L"action.font", 11.0f, L"Action Menu"},
                                    {L"target.font", 11.0f, L"Target name"},
                                    {L"target.title.font", 11.0f, L"Target title"},
                                    {L"chattabs.font", 11.0f, L"Chat tabs"},
                                    {L"status.name.font", 16.0f, L"Status name"}, // (about the menu font's name it replaced)
                                    {L"lists.font", 15.0f, L"Menu lists"},
                                    {L"macrobar.name.font", 15.0f, L"Macro names"}, // (the menus' size, as their names were)
                                    {L"tips.font", 15.0f, L"Tooltips"},   {L"tips.font.title", 15.0f, L"Tooltip titles"},
                                    {L"tips.font.type", 15.0f, L"Tooltip types"}, {L"tips.font.stats", 15.0f, L"Tooltip stats"},
                                    {L"tips.font.jobs", 15.0f, L"Tooltip jobs"},
                                    {L"tips.font.signature", 15.0f, L"Tooltip signatures"},
                                    {L"tips.font.timer", 15.0f, L"Tooltip timers"},
                                    {L"party.font.numbers", 11.0f, L"Party list numbers"},
                                    {L"alliance.font.numbers", 11.0f, L"Alliance list numbers"}};
// The windows' areas (Area): the font sheet's text in them, each letter centred in the game's width for it (`centred`); the
// party and alliance lists' big letters and labels' letters, laid as the Status area's are
bool centred(int which)
{
    return which == kMenus || which == kLists || which == kItems || which == kHelp || which == kInput || which == kPhrases || which == kTrade ||
           which == kGil || which == kProfile || which == kProfileTitle || which == kEquip || which == kEquipStats || which == kBuffs ||
           which == kTarget ||
           (which >= kTips && which <= kTipsTimer);
}
bool windowed(int which)
{
    return (centred(which) && which < kTips) || which == kParty || which == kAlliance || which == kMainMenu || which == kMacroBar ||
           which == kEquip || which == kActionMenu || which == kTargetTitle;
}

// The windows whose font sheet text is an area's own, by their layout's name after "menu    " (its first letters): the item
// lists (Items and its bags, the Mog House's storage, Equipment's list, the bank, a trade's, a delivery's, a shop's, the
// auction house's), the help (the help bar and its title, a spell's details, a comment), the chat input line (the window
// Enter opens, its Say / Shout / Linkshell tab with it), the party list and the alliance lists (their names and numbers are the
// name plates' big letters and the labels', not the font sheet's: d3d8_chatfont.h cf_list_area), the auto-translate list
// ("fep", Tab while typing: its rows, its count's labels the game's). An item's details, "iteminfo"
// and the "item<n>..." and "itemx..." of each size, are the tooltips' (area_of). The rest are the menus'. The trade windows'
// every label and big letter is the Trade font's (d3d8_chatfont.h cf_what): a trade's, the delivery box being sent's with
// its Recipient box under it ("stringdl": "Recipient", OK, Exit, read live 2026-11-06), the one received ("post1", its
// Take / Drop / Return "post2"). Current Gil ("money") is its own, gil.font: it shows beside Equipment, Items and the Mog
// House as well, and Trade Windows' size changed it there (reported 2026-11-06). The Equipment window ("equip") is its own two:
// its squares' slot names (the labels' letters) equip.font's, its stats (its numbers, their signs, the stats under it)
// equip.stats.font's (d3d8_chatfont.h cf_equip_font); they were the titles' or the buttons', the Status area's and the item
// lists', so some of its letters were True Font and some the game's (reported 2026-11-06), then one area until they were
// asked for apart. Its list of what fits a slot is Items' own list ("inventor"), the item lists'. The macro bar ("mcr1pall",
// "mcr2pall"; "mcr1long", "mcr2long" the wider one) is macrobar.font's: its keys' labels, kKeytopDigits. Its macros' names are
// the menu font's text (keytop_print_hook), still the menus'. Both are drawn whole by macrotext.cpp now, taken before the game
// draws a letter: what's here is for a key or a name it leaves to the game. The status icons' window ("buff    ") is buffs.font's: its
// timers, the labels' sheet's letters drawn at 0.85 and centred under their icons by the game (FUN_10226740 -> 0x101C7F10, its
// last argument 1: x less half the text's width; read live 2026-11-07), every label letter in it (d3d8_chatfont.h cf_what: they
// were Status & Numbers'), laid centred on that x (centred_text_hook, d3d8_chatfont_words.h cf_centred_word).
struct Named {
    const char* name;
    int area;
};
constexpr Named kWindows[] = {{"inventor", kItems}, {"mnstorag", kItems}, {"itmstora", kItems}, {"equip", kEquip},
                              {"bank", kItems},     {"trade", kTrade},    {"delivery", kTrade}, {"money", kGil},   {"shop", kItems},
                              {"stringdl", kTrade}, {"post1", kTrade},    {"post2", kTrade},
                              {"auclist", kItems},  {"helpwind", kHelp},  {"titlewin", kHelp},  {"subwindo", kHelp},
                              {"comment", kHelp},   {"inline", kInput},   {"partywin", kParty}, {"raid1", kAlliance},
                              {"raid2", kAlliance}, {"fep ", kPhrases},   {"menuwind", kMainMenu}, {"socialme", kMainMenu},
                              {"mcr1pall", kMacroBar}, {"mcr2pall", kMacroBar}, {"mcr1long", kMacroBar}, {"mcr2long", kMacroBar},
                              {"buff    ", kBuffs},
                              // the Target window: the target's name its big letters (the game draws it as the name plates'
                              // at about half, read live 2026-11-07), "Target" its title's letters (d3d8_chatfont.h cf_what;
                              // asked for apart from Status & Numbers' and the titles' 2026-11-07)
                              {"targetwi", kTarget},
                              // the Status screen's Profile panel: its labels (picture letters) and values (font sheet text),
                              // one font (asked for 2026-11-08)
                              {"profile ", kProfile},
                              // the menus the main menu's commands open in its place, and the lists those open in theirs
                              // (hudwin_windows.hpp's kFollow windows; Linkshell's "lscswin" and "link1" ... "link13",
                              // Communication's "commenu" read from FFXiMain.dll 2026-11-07): their buttons the main menu's
                              // font too. They were the buttons', so Config's came up at Buttons' size (reported 2026-11-07)
                              {"statcom2", kMainMenu}, {"ut_menu ", kMainMenu}, {"merit", kMainMenu}, {"mgcmenu ", kMainMenu},
                              {"cmbmenu ", kMainMenu}, {"cmbhwin ", kMainMenu}, {"abimenu ", kMainMenu}, {"mp_mnabi", kMainMenu},
                              {"prty1", kMainMenu}, {"prty2", kMainMenu}, {"prty3", kMainMenu}, {"prty4", kMainMenu},
                              {"prty5", kMainMenu}, {"prty6", kMainMenu}, {"prty7", kMainMenu}, {"prty8", kMainMenu},
                              {"search", kMainMenu}, {"scindex ", kMainMenu}, {"scsibori", kMainMenu}, {"scchar  ", kMainMenu},
                              {"scname  ", kMainMenu}, {"scmlev  ", kMainMenu}, {"scoption", kMainMenu}, {"sccom", kMainMenu},
                              {"lscswin ", kMainMenu}, {"link", kMainMenu}, {"region  ", kMainMenu}, {"map0    ", kMainMenu},
                              {"mapscan ", kMainMenu}, {"mapv1   ", kMainMenu}, {"mapv3   ", kMainMenu}, {"missionm", kMainMenu},
                              {"mogcont ", kMainMenu}, {"bazaar  ", kMainMenu}, {"mcrmenu ", kMainMenu}, {"mcrselec", kMainMenu},
                              {"mcres20 ", kMainMenu}, {"mcrmogc ", kMainMenu}, {"configwi", kMainMenu}, {"commenu ", kMainMenu},
                              // the Action Menu (Monstrosity's too) and the Magic and Abilities lists it opens beside it
                              // (hudwin_windows.hpp's action, actionmagic, actionability): its own font, action.font (asked for
                              // 2026-11-07, with its own buttons' look, buttons.cpp's action.buttons.*)
                              {"playermo", kActionMenu}, {"mp_pmode", kActionMenu}, {"magselec", kActionMenu},
                              {"abiselec", kActionMenu}, {"mp_abisc", kActionMenu}};
// The windows whose font sheet text is Menu Lists' (kLists, lists.font), by the same first letters; their buttons and labels
// stay their area's above (kWindows: the Merit Points menu's the main menu's). The Merit Points menu: its Categories and Mode
// Switch ("merit1", "merit2", "merit3"), its list of merits ("meritcat") and its points box ("meritinf"), all General's text
// until they were asked for apart (2026-11-08). The HUD's Merit Points ("levmerit") stays General's.
constexpr const char* kListWindows[] = {"merit"};
bool list_window(const char* name)
{
    if (std::memcmp(name, "menu    ", 8) != 0)
        return false;
    for (const char* window : kListWindows)
        if (std::strncmp(name + 8, window, std::strlen(window)) == 0)
            return true;
    return false;
}

// The menus' labels (their buttons and titles: "Status", "Equipment") are no text: the menu resources (ROM\119\51.DAT) place
// a sprite of the "font    font" sheet (256 x 256) for each letter, drawn a texel to a pixel. The sheet holds them in ASCII
// order in rows 13 texels apart (from '!' at the top; A-D from u 77 of v 26, E-P v 39, Q-Z v 52, a-g v 65, h-s v 78, t-z
// v 91), each standing 10 below its row's top, their capitals 9 tall where the font sheet's are 12 (buttons.font's size is
// smaller than the menus' for it). A sprite's place and size (u, v, w, h) and its row: the letters' from
// configmenu.cpp's (the game's own, read off its labels), ',' '-' '/' read off the Status window's, the rest from the
// sheet's ink (CatsEyeXI's ROM\119\51).
struct LabelSprite {
    char c;
    std::uint8_t u, v, w, h, row;
};
constexpr LabelSprite kLabelSprites[] = {
    {'!', 12, 0, 6, 11, 0},    {'"', 21, 0, 7, 5, 0},     {'#', 30, 0, 10, 11, 0},   {'$', 40, 0, 10, 11, 0},
    {'%', 50, 0, 10, 11, 0},   {'&', 60, 0, 10, 11, 0},   {'\'', 73, 1, 4, 6, 0},    {'(', 81, 0, 5, 12, 0},
    {')', 94, 0, 5, 12, 0},    {'*', 100, 0, 10, 11, 0},  {'+', 111, 1, 8, 10, 0},   {',', 1, 11, 6, 13, 13},
    {'-', 11, 13, 8, 8, 13},   {'.', 23, 20, 4, 4, 13},   {'/', 30, 13, 9, 11, 13},   {'0', 40, 13, 9, 11, 13},
    {'1', 51, 13, 8, 11, 13},  {'2', 60, 13, 9, 11, 13},  {'3', 70, 13, 9, 11, 13},  {'4', 80, 13, 9, 11, 13},
    {'5', 90, 13, 9, 11, 13},  {'6', 101, 13, 8, 11, 13}, {'7', 111, 13, 9, 11, 13}, {'8', 0, 26, 9, 11, 26},
    {'9', 9, 26, 10, 11, 26},  {':', 19, 28, 4, 8, 26},   {';', 24, 28, 5, 9, 26},   {'<', 30, 28, 8, 9, 26},
    {'=', 39, 29, 8, 7, 26},   {'>', 48, 28, 8, 9, 26},   {'?', 56, 26, 10, 11, 26}, {'@', 66, 26, 9, 11, 26},
    {'A', 77, 26, 9, 11, 26},  {'B', 87, 26, 9, 11, 26},  {'C', 97, 26, 9, 12, 26},  {'D', 107, 26, 9, 11, 26},
    {'E', 0, 39, 9, 11, 39},   {'F', 10, 39, 9, 11, 39},  {'G', 20, 39, 9, 12, 39},  {'H', 31, 39, 9, 11, 39},
    {'I', 42, 39, 6, 11, 39},  {'J', 48, 39, 9, 12, 39},  {'K', 58, 39, 9, 11, 39},  {'L', 69, 39, 9, 11, 39},
    {'M', 80, 39, 9, 11, 39},  {'N', 91, 39, 9, 11, 39},  {'O', 102, 39, 9, 12, 39}, {'P', 113, 39, 9, 11, 39},
    {'Q', 0, 52, 9, 12, 52},   {'R', 11, 52, 10, 11, 52}, {'S', 22, 52, 9, 12, 52},  {'T', 31, 52, 9, 11, 52},
    {'U', 41, 52, 9, 12, 52},  {'V', 52, 52, 10, 11, 52}, {'W', 63, 52, 9, 11, 52},  {'X', 76, 52, 9, 11, 52},
    {'Y', 88, 52, 9, 11, 52},  {'Z', 98, 52, 11, 11, 52}, {'[', 114, 52, 6, 11, 52}, {']', 13, 65, 6, 11, 65},
    {'^', 21, 65, 8, 7, 65},   {'_', 31, 72, 8, 4, 65},   {'`', 42, 65, 5, 7, 65},   {'a', 49, 67, 8, 9, 65},
    {'b', 58, 65, 9, 11, 65},  {'c', 68, 67, 9, 9, 65},   {'d', 78, 65, 9, 11, 65},  {'e', 89, 67, 9, 9, 65},
    {'f', 99, 65, 7, 11, 65},  {'g', 107, 67, 9, 12, 65}, {'h', 0, 78, 9, 11, 78},   {'i', 10, 78, 5, 11, 78},
    {'j', 16, 78, 8, 14, 78},  {'k', 25, 78, 9, 11, 78},  {'l', 35, 78, 6, 11, 78},  {'m', 42, 80, 9, 9, 78},
    {'n', 56, 80, 9, 9, 78},   {'o', 66, 80, 9, 9, 78},   {'p', 76, 80, 9, 12, 78},  {'q', 87, 80, 9, 12, 78},
    {'r', 98, 80, 7, 9, 78},   {'s', 106, 80, 9, 9, 78},  {'t', 0, 92, 6, 10, 91},   {'u', 7, 93, 9, 9, 91},
    {'v', 17, 93, 9, 9, 91},   {'w', 27, 93, 9, 9, 91},   {'x', 39, 93, 9, 9, 91},   {'y', 50, 93, 9, 12, 91},
    {'z', 60, 93, 8, 9, 91},
};
// The name plates' big letters, on the same sheet below the labels' (rows 24 apart from v 131), 19 texels to a capital where
// the labels' are 9, each standing 21 below its row's top: the Status window's numbers and words, the clock, the name and
// level at the bottom right draw them at about half. Their places as the game's own letter lookup (0x10120110, on the
// "font    fontshp" sprites) gives them on CatsEyeXI's sheet; its punctuation is the labels' own small letters.
constexpr LabelSprite kBigSprites[] = {
    {'0', 16, 131, 19, 23, 131}, {'1', 34, 131, 19, 23, 131}, {'2', 54, 131, 19, 23, 131}, {'3', 75, 131, 19, 23, 131},
    {'4', 94, 131, 19, 23, 131}, {'5', 114, 131, 19, 23, 131}, {'6', 133, 131, 19, 23, 131}, {'7', 154, 131, 19, 23, 131},
    {'8', 173, 131, 19, 23, 131}, {'9', 192, 131, 19, 23, 131}, {'A', 211, 131, 19, 23, 131}, {'B', 234, 131, 19, 23, 131},
    {'C', 3, 155, 19, 23, 155}, {'D', 25, 155, 19, 23, 155}, {'E', 47, 155, 19, 23, 155}, {'F', 68, 155, 19, 23, 155},
    {'G', 88, 155, 19, 23, 155}, {'H', 111, 155, 19, 23, 155}, {'I', 132, 155, 14, 23, 155}, {'J', 146, 155, 19, 23, 155},
    {'K', 166, 155, 19, 23, 155}, {'L', 185, 155, 19, 23, 155}, {'M', 207, 155, 19, 23, 155}, {'N', 233, 155, 19, 23, 155},
    {'O', 3, 179, 19, 23, 179}, {'P', 25, 179, 19, 23, 179}, {'Q', 48, 179, 19, 23, 179}, {'R', 70, 179, 19, 23, 179},
    {'S', 93, 179, 19, 23, 179}, {'T', 114, 179, 19, 23, 179}, {'U', 134, 179, 19, 23, 179}, {'V', 157, 179, 19, 23, 179},
    {'W', 179, 179, 19, 23, 179}, {'X', 204, 179, 19, 23, 179}, {'Y', 226, 179, 19, 23, 179}, {'Z', 1, 203, 19, 23, 203},
    {'a', 21, 203, 17, 23, 203}, {'b', 39, 203, 19, 23, 203}, {'c', 60, 203, 19, 23, 203}, {'d', 81, 203, 19, 23, 203},
    {'e', 100, 203, 19, 23, 203}, {'f', 119, 203, 15, 23, 203}, {'g', 134, 203, 19, 27, 203}, {'h', 154, 203, 19, 23, 203},
    {'i', 173, 203, 11, 23, 203}, {'j', 185, 203, 16, 27, 203}, {'k', 200, 203, 19, 23, 203}, {'l', 219, 203, 12, 23, 203},
    {'m', 234, 203, 19, 23, 203}, {'n', 0, 227, 19, 23, 227}, {'o', 21, 227, 19, 23, 227}, {'p', 41, 227, 19, 27, 227},
    {'q', 62, 227, 19, 27, 227}, {'r', 83, 227, 15, 23, 227}, {'s', 97, 227, 19, 23, 227}, {'t', 118, 227, 13, 23, 227},
    {'u', 132, 231, 19, 19, 227}, {'v', 152, 227, 19, 23, 227}, {'w', 171, 231, 19, 19, 227}, {'x', 192, 231, 19, 19, 227},
    {'y', 210, 227, 19, 27, 227}, {'z', 229, 227, 19, 23, 227},
};
// The bold slanted letters in the windows' own pictures, on the "menu    menu2fon" sheet (256 x 256, in ROM\119\51.DAT with
// the menus): the chat line's Say / Shout / Linkshell tab over it (FsTextInputDisplay's mode picture, 0x101B7420), "Target"
// and "Help" captions, "Status" and "Network" over their windows. A to X in a row from v 161, Y, Z and a to z from v 173 (the
// small ones from 175), the digits from v 194; capitals 9 tall, standing on 170, 182 and 204. Read off the sheet's ink
// (CatsEyeXI's DAT, decoded 2026-11-05) and checked against the sprites the DAT's pictures place, which are within a texel of
// these (cf_label takes 2)
constexpr LabelSprite kCaptionSprites[] = {
    {'A', 3, 161, 8, 9, 160},    {'B', 13, 161, 8, 9, 160},   {'C', 24, 161, 8, 9, 160},   {'D', 34, 161, 9, 9, 160},
    {'E', 45, 161, 8, 9, 160},   {'F', 55, 161, 8, 9, 160},   {'G', 66, 161, 8, 9, 160},   {'H', 76, 161, 9, 9, 160},
    {'I', 87, 161, 5, 9, 160},   {'J', 94, 161, 8, 9, 160},   {'K', 104, 161, 9, 9, 160},  {'L', 115, 161, 7, 9, 160},
    {'M', 124, 161, 11, 9, 160}, {'N', 137, 161, 9, 9, 160},  {'O', 148, 161, 8, 9, 160},  {'P', 158, 161, 8, 9, 160},
    {'Q', 169, 161, 8, 11, 160}, {'R', 179, 161, 9, 9, 160},  {'S', 191, 161, 7, 9, 160},  {'T', 200, 161, 8, 9, 160},
    {'U', 211, 161, 8, 9, 160},  {'V', 221, 161, 9, 9, 160},  {'W', 231, 161, 11, 9, 160}, {'X', 243, 161, 10, 9, 160},
    {'Y', 4, 173, 8, 9, 172},    {'Z', 13, 173, 8, 9, 172},   {'a', 23, 175, 6, 7, 172},   {'b', 32, 173, 6, 9, 172},
    {'c', 41, 175, 6, 7, 172},   {'d', 50, 173, 6, 9, 172},   {'e', 59, 175, 6, 7, 172},   {'f', 68, 173, 6, 9, 172},
    {'g', 74, 175, 8, 9, 172},   {'h', 83, 173, 8, 9, 172},   {'i', 93, 173, 5, 9, 172},   {'j', 99, 173, 6, 11, 172},
    {'k', 107, 173, 7, 9, 172},  {'l', 116, 173, 5, 9, 172},  {'m', 123, 175, 10, 7, 172}, {'n', 136, 175, 7, 7, 172},
    {'o', 146, 175, 7, 7, 172},  {'p', 155, 175, 7, 9, 172},  {'q', 165, 175, 6, 9, 172},  {'r', 174, 175, 6, 7, 172},
    {'s', 182, 175, 5, 7, 172},  {'t', 191, 173, 4, 9, 172},  {'u', 198, 175, 6, 7, 172},  {'v', 207, 175, 8, 7, 172},
    {'w', 217, 175, 9, 7, 172},  {'x', 227, 175, 8, 7, 172},  {'y', 236, 175, 8, 9, 172},  {'z', 246, 175, 6, 7, 172},
    {'0', 128, 194, 8, 10, 194}, {'1', 139, 194, 8, 10, 194}, {'2', 150, 194, 8, 10, 194}, {'3', 161, 194, 8, 10, 194},
    {'4', 172, 194, 8, 10, 194}, {'5', 183, 194, 8, 10, 194}, {'6', 195, 194, 8, 10, 194}, {'7', 205, 194, 8, 10, 194},
    {'8', 216, 194, 8, 10, 194}, {'9', 227, 194, 8, 10, 194},
};
// The macro bar's keys: each a shape of the "menu    keytops3" set (ROM\119\51.DAT, its shapes 155 to 174: the key's square,
// then "Ctrl" or "Alt" in kCaptionSprites' letters, then the number), 40 x 32 a texel to a pixel; the number a sprite of the
// "menu    keytop" sheet, 1 to 9 from u 136 of v 96, 12 apart, 0 at 3, 107, 10 texels tall and standing on their bottom (the
// key's letters' baseline). Decoded 2026-11-06. That sheet's letters are the macros' names too (kKeytopPrint), so these are
// taken only in the macro bar's windows (sheet 2: d3d8_chatfont.h cf_label)
constexpr LabelSprite kKeytopDigits[] = {
    {'1', 136, 96, 5, 10, 96}, {'2', 148, 96, 6, 10, 96}, {'3', 160, 96, 6, 10, 96}, {'4', 172, 96, 7, 10, 96},
    {'5', 184, 96, 6, 10, 96}, {'6', 196, 96, 6, 10, 96}, {'7', 208, 96, 6, 10, 96}, {'8', 220, 96, 7, 10, 96},
    {'9', 232, 96, 7, 10, 96}, {'0', 3, 107, 7, 10, 107},
};

// The labels' letters to the graphics layer, once
void send_labels()
{
    static bool sent = false;
    if (sent)
        return;
    sent = true;
    std::vector<VanaLabelGlyph> list;
    for (const LabelSprite& s : kLabelSprites) // (each stands 10 below its row's top, capitals 9 tall)
        list.push_back({static_cast<std::uint8_t>(s.c - 0x20), s.u, s.v, s.w, s.h, static_cast<std::uint8_t>(s.row + 10 - s.v), 9, 0});
    for (const LabelSprite& s : kBigSprites) // (21 below, capitals 19 tall)
        list.push_back({static_cast<std::uint8_t>(s.c - 0x20), s.u, s.v, s.w, s.h, static_cast<std::uint8_t>(s.row + 21 - s.v), 19, 0});
    for (const LabelSprite& s : kCaptionSprites) // (the menu2fon sheet's: 10 below too, capitals 9 tall)
        list.push_back({static_cast<std::uint8_t>(s.c - 0x20), s.u, s.v, s.w, s.h, static_cast<std::uint8_t>(s.row + 10 - s.v), 9, 1});
    for (const LabelSprite& s : kKeytopDigits) // (the keytop sheet's: standing on their bottom, as tall as the key's capitals)
        list.push_back({static_cast<std::uint8_t>(s.c - 0x20), s.u, s.v, s.w, s.h, static_cast<std::uint8_t>(s.row + s.h - s.v), 9, 2});
    vanacore_gfx_set_label_glyphs(list.data(), static_cast<std::uint32_t>(list.size()));
}

struct Want {
    bool on = false;
    std::wstring family = L"Segoe UI";
    bool bold = false;
    bool italic = false;
    bool upper = false;           // its lower-case letters drawn as their capitals (<prefix>.uppercase; the tooltips' by part)
    float size = 16.0f;           // the font's size (its em) in the interface's pixels, as the chat's rows are (16 apart)
    float spacing = 0.0f;         // the interface's pixels more between letters (below 0, closer): the chat's
    float outline = 0.0f;         // a black outline round the letters this wide, the interface's pixels (0 none)
    float outline_opacity = 1.0f; // ... this opaque (0-1)
    float line = 1.0f;            // the rows' spacing to the game's: the tooltips' (tips.font.lineheight / 16)
    float pad = 0.0f;             // the rows moved down this far, the interface's pixels: the tooltips' (tips.font.padding.top)
    float pad_bottom = 0.0f;      // ... and this much more room below them (tips.font.padding.bottom)
    float widths = 1.0f;          // the game's widths its letters are centred in made this much wider: the tooltips' title's
    std::wstring japanese;        // the full-width letters' font (truefont.japanese: chatfont_wide.h); empty: the area's own
    bool none = false;            // its size set to 0 while on: its letters left out (vanacore_gfx_hide_letters 2), built at the default size
    bool operator==(const Want&) const = default;
};

Want g_want[kAreas];  // as the settings have it
Want g_built[kAreas]; // what the graphics layer's glyphs were made from (on false: none)
float g_built_sx[kAreas] = {}, g_built_sy[kAreas] = {};
float g_advance[kAreas][kAscii] = {}; // each ASCII letter's advance as built, the interface's pixels (text_width)
signed char* g_table = nullptr;
bool g_table_looked = false;
signed char g_game[kWidths] = {}; // the game's widths, as found
signed char g_ours[kWidths] = {}; // with the chat font's in place of ASCII's
bool g_ours_ready = false;
int g_held = 0;

std::uintptr_t call_target(std::uintptr_t call) { return call + 5 + *reinterpret_cast<const std::int32_t*>(call + 1); }

// The menu font's routines (g_menu_font: left out if they aren't found, and the macro menus' names stay the game's)
bool find_menu_font(HMODULE game)
{
    const std::uintptr_t print = scan::find(game, kPrintString);
    const std::uintptr_t title = scan::find(game, kTitleText);
    const auto* code = reinterpret_cast<const std::uint8_t*>(print);
    if (print == 0 || title == 0 || code[kStatesCall] != 0xE8 || code[kBindCall] != 0xE8 || code[kCodeCall] != 0xE8 ||
        code[kPrintCall] != 0xE8 || code[kAdvanceCall] != 0xE8)
        return false;
    g_menu_font.renderer = *reinterpret_cast<char** const*>(title + kRendererAt);
    g_menu_font.states = reinterpret_cast<States>(call_target(print + kStatesCall));
    g_menu_font.bind = reinterpret_cast<Bind>(call_target(print + kBindCall));
    g_menu_font.next_code = reinterpret_cast<NextCode>(call_target(print + kCodeCall));
    g_menu_font.print = reinterpret_cast<PrintCode>(call_target(print + kPrintCall));
    g_menu_font.advance = reinterpret_cast<Advance>(call_target(print + kAdvanceCall));
    return true;
}

// A macro menu's `name` in the menu font where the macro printer would put its letters (from x, standing on y + 11), in
// `colour`, each letter that fits within `room` of x as the printer's do. False if the font can't be used (not found, its
// sheet not loaded)
bool menu_name(int x, int y, const char* name, std::uint32_t colour, int room)
{
    const MenuFont& f = g_menu_font;
    void* renderer = f.renderer != nullptr ? *f.renderer : nullptr;
    if (renderer == nullptr || f.print == nullptr)
        return false;
    f.states(renderer);
    if (f.bind(renderer) == 0)
        return false;
    auto* corners = static_cast<char*>(renderer);
    for (const std::ptrdiff_t corner : kCornerColours)
        *reinterpret_cast<std::uint32_t*>(corners + corner) = colour;
    const int top = y + kKeytopBaseline - kMenuBaseline; // (the cell's top, so the letters stand where the game's did)
    // The room is measured in the letters True Font draws, its own advances, not the game's menu font's, which are wider:
    // by those a book called `TEST "" $$ %% !` (131 wide, 75 of room) stopped after `TEST ""` with half its row empty
    // (seen live 2026-11-12). Each letter is put where True Font will lay it, so none stands past the room either.
    float drawn = 0.0f;
    for (const char* at = name; *at != '\0';) {
        const char* was = at;
        const int code = static_cast<short>(f.next_code(&at));
        int step[2] = {};
        f.advance(code, step);
        const int ascii = at == was + 1 ? static_cast<unsigned char>(*was) - 0x20 : -1;
        const float own = ascii >= 0 && ascii < kAscii && g_advance[kMenus][ascii] > 0.0f ? g_advance[kMenus][ascii]
                                                                                           : static_cast<float>(step[0]);
        if (at == was || drawn + own > static_cast<float>(room))
            break;
        int pos[2] = {x + static_cast<int>(std::lround(drawn)), top};
        f.print(renderer, code, pos, 1.0f, 1.0f);
        drawn += own;
    }
    for (const std::ptrdiff_t corner : kCornerColours)
        *reinterpret_cast<std::uint32_t*>(corners + corner) = kWhite;
    return true;
}

// A macro menu's name printed (kKeytopPrint): with the menus' True Font on, in the menu font, which True Font draws whole, as
// one text from its left (vanacore_gfx_text_whole); else the game's keytop letters, none of them taken for a label's letter
// meanwhile. Laid as a window's words, a name ending in a digit was taken for a number and moved to end where the game's wider
// letters end (cf_word_moved): the equip sets' "Set002" ... "Set020" stood about 13 screen pixels in from "NINJA" and the
// other names over them, which the game starts at one place (measured live, 2026-11-09; "all the text is unaligned")
int __fastcall keytop_print_hook(void* printer, void* edx, int x, int y, const char* name, std::uint32_t colour, int room)
{
    // (a macro's on the macro bar: VanaCore's own text, drawn whole over the bar, whatever the game draws over it: macrotext.cpp)
    if (name != nullptr && macrotext::name(x, y + kKeytopBaseline, name, colour, room))
        return static_cast<int>(std::strlen(name));
    if (name != nullptr && g_built[kMenus].on) {
        vanacore_gfx_text_whole(1, 0, name);
        const bool printed = menu_name(x, y, name, colour, room);
        vanacore_gfx_text_whole(0, 0, nullptr);
        if (printed)
            return static_cast<int>(std::strlen(name));
    }
    vanacore_gfx_game_letters(1);
    const int printed = g_keytop_print(printer, edx, x, y, name, colour, room);
    vanacore_gfx_game_letters(0);
    return printed;
}

// The help line's message drawn (kHelpDraw): a scrolling one's letters each where the game's are
void __fastcall help_draw_hook(void* message, void* edx)
{
    const bool rolling = message != nullptr && *(static_cast<const std::uint8_t*>(message) + kHelpScroll) != 0;
    if (rolling)
        vanacore_gfx_text_rolling(1);
    g_help_draw(message, edx);
    if (rolling)
        vanacore_gfx_text_rolling(0);
}

// The game's width table and its chat row drawing (hooked), once FFXiMain.dll is there
void find_table()
{
    if (g_table_looked)
        return;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return; // (looked for again next frame)
    g_table_looked = true;
    const std::uintptr_t row = scan::find(game, kDrawRow);
    g_hooked[0] = row != 0 && imports::detour(reinterpret_cast<DrawRow>(row), 12, &draw_row_hook, g_draw_row);
    if (!g_hooked[0])
        log::write(L"Chat: the game's chat row drawing wasn't found in FFXiMain.dll: the chat log maximized keeps the game's "
                   L"letters, with no text shadow");
    const std::uintptr_t rows = scan::find(game, kWindowRows);
    g_hooked[1] = rows != 0 && imports::detour(reinterpret_cast<WindowRows>(rows), 5, &window_rows_hook, g_window_rows);
    if (!g_hooked[1])
        log::write(L"Chat: the chat windows' line wrap wasn't found in FFXiMain.dll: with [client] chat.font=sharp a long line "
                   L"can be wrapped twice");
    const std::uintptr_t lines = scan::find(game, kSetLines);
    g_hooked[2] = lines != 0 && imports::detour(reinterpret_cast<SetLines>(lines), 5, &set_lines_hook, g_set_lines);
    if (!g_hooked[2])
        log::write(L"True Font: the item details' SetLines wasn't found in FFXiMain.dll: [client] tips.font.lineheight spreads "
                   L"their rows in the same box");
    const std::uintptr_t letter = scan::find(game, kLetter);
    g_hooked[3] = letter != 0 && imports::detour(reinterpret_cast<Letter>(letter), 7, &letter_hook, g_letter);
    if (!g_hooked[3])
        log::write(L"True Font: the game's letter printer wasn't found in FFXiMain.dll: its letters are known by their shape alone "
                   L"(a 16 x 16 sprite of one cell of the font sheet)");
    const std::uintptr_t keytop = scan::find(game, kKeytopPrint);
    g_hooked[4] = keytop != 0 && find_menu_font(game) &&
                  imports::detour(reinterpret_cast<KeytopPrint>(keytop), 6, &keytop_print_hook, g_keytop_print);
    if (!g_hooked[4])
        log::write(L"True Font: the macro menus' name printer wasn't found in FFXiMain.dll: the books' and equip sets' names keep "
                   L"the game's letters");
    const std::uintptr_t help = scan::find(game, kHelpDraw);
    g_hooked[5] = help != 0 && imports::detour(reinterpret_cast<HelpDraw>(help), 6, &help_draw_hook, g_help_draw);
    if (!g_hooked[5])
        log::write(L"True Font: the help line's draw wasn't found in FFXiMain.dll: a scrolling help line's words are laid as a "
                   L"still one's");
    const std::uintptr_t centring = scan::find(game, kCentredText);
    g_hooked[6] = centring != 0 && imports::detour(reinterpret_cast<CentredText>(centring), 8, &centred_text_hook, g_centred_text);
    if (!g_hooked[6])
        log::write(L"True Font: the game's centring text draw wasn't found in FFXiMain.dll: the status timers are laid from the "
                   L"game's left, not centred under their icons");
    const std::uintptr_t right = scan::find(game, kRightText);
    g_hooked[7] = right != 0 && imports::detour(reinterpret_cast<RightText>(right), 6, &right_text_hook, g_right_text);
    if (!g_hooked[7])
        log::write(L"True Font: the game's right-aligned text draw wasn't found in FFXiMain.dll: the skill panels' levels and the "
                   L"Status window's numbers are laid as words, each where the game's starts or ends");
    chatline::install(game);
    macrotext::install(game); // the macro bar's names with a space or a symbol in them: drawn whole too
    const std::uintptr_t site = scan::find(game, kWidthTable);
    if (site == 0) {
        log::write(L"Chat: the game's letter widths weren't found in FFXiMain.dll: [client] chat.font and menu.font leave the "
                   L"letters the game's");
        return;
    }
    g_table = reinterpret_cast<signed char*>(static_cast<std::uintptr_t>(scan::operand(site, 12)));
    std::memcpy(g_game, g_table, kWidths);
}

// The tooltips' parts (VANA_TIP_*): their settings' names ([client] tips.font.<name>.size, .color, .space) and their look
constexpr const wchar_t* kTipParts[VANA_TIP_PARTS] = {L"title", L"type", L"stats", L"jobs", L"signature", L"timer"};
VanaTipPart g_tip_parts[VANA_TIP_PARTS] = {};

// What the tooltips' box needs for their rows as they're built (tips_px): their spacing, their paddings, and until a
// tooltip's rows are measured, one row of each part in its size and the space above each
void tips_box()
{
    const Want& tips = g_built[kTips];
    g_tips_on = tips.on;
    g_tips_line = tips.on ? tips.line : 1.0f;
    g_tips_pads = tips.on ? tips.pad + tips.pad_bottom : 0.0f;
    g_tips_guess = 0.0f;
    for (int part = 0; part < VANA_TIP_PARTS; ++part) {
        const Want& own = g_built[kTipsTitle + part];
        g_tips_guess += (own.on ? own.size - tips.size : 0.0f) + g_tip_parts[part].space;
    }
}

void drop(int which)
{
    if (g_built[which].on)
        vanacore_gfx_set_chat_font(which, nullptr);
    g_built[which] = Want{};
    tips_box();
    if (which == kChat)
        g_ours_ready = false;
}

// The font's size in the screen's pixels: its size at sy screen pixels to the interface's
int screen_size(const Want& want, float sy)
{
    const int size = static_cast<int>(std::lround(want.size * sy));
    return size < 1 ? 1 : size > 160 ? 160 : size;
}

// A rendered picture cut to its ink: premultiplied white (every channel the alpha), w x h, its place in the atlas, and its
// top-left corner from the pen (x) and the baseline (y) in the screen's pixels. w 0: no ink (a space).
struct Cut {
    std::vector<std::uint32_t> ink;
    int w = 0, h = 0, ax = 0, ay = 0;
    float x = 0.0f, y = 0.0f;
};

Cut cut(const text::Sprite& sprite, float shift)
{
    Cut out;
    const std::vector<std::uint32_t>& pixels = sprite.pixels();
    int x0 = sprite.width(), y0 = sprite.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < sprite.height(); ++y)
        for (int x = 0; x < sprite.width(); ++x)
            if ((pixels[static_cast<std::size_t>(y) * sprite.width() + x] >> 24) != 0)
                x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
    if (x1 < 0)
        return out;
    out.w = x1 - x0 + 1, out.h = y1 - y0 + 1;
    out.x = static_cast<float>(x0 - sprite.pen_left()) + shift, out.y = static_cast<float>(y0 - sprite.baseline());
    out.ink.resize(static_cast<std::size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            const std::uint32_t a = pixels[static_cast<std::size_t>(y0 + y) * sprite.width() + x0 + x] >> 24;
            out.ink[static_cast<std::size_t>(y) * out.w + x] = a << 24 | a << 16 | a << 8 | a;
        }
    return out;
}

// The atlas's next place for `c`, a shelf at a time, a texel apart (`row`: the shelf's height so far)
void place(Cut& c, int& x, int& y, int& row)
{
    if (c.w == 0)
        return;
    if (x + c.w > kAtlasWidth)
        x = 0, y += row + 1, row = 0;
    c.ax = x, c.ay = y;
    x += c.w + 1;
    row = c.h > row ? c.h : row;
}

void blit(std::vector<std::uint32_t>& atlas, const Cut& c)
{
    for (int r = 0; r < c.h; ++r)
        std::memcpy(&atlas[static_cast<std::size_t>(c.ay + r) * kAtlasWidth + c.ax], &c.ink[static_cast<std::size_t>(r) * c.w],
                    c.w * sizeof(std::uint32_t));
}

// Character `code`'s width for the game's table, from the picture of it in `sprite`: the chat font's advance in the interface's
// pixels and chat.font.spacing after it (none known: the game's width). What the whole-pixel width left off.
float widen(const text::Sprite& sprite, int code, float sx)
{
    const float font = sprite.exact_advance() / sx;
    const float advance = font > 0.0f ? font + g_want[kChat].spacing : static_cast<float>(g_game[code]);
    const long width = std::lround(advance);
    g_ours[code] = static_cast<signed char>(width < 1 ? 1 : width > 127 ? 127 : width);
    return advance - static_cast<float>(g_ours[code]);
}

text::Style style_of(const Want& want, float sy)
{
    text::Style style;
    style.family = want.family;
    style.weight = want.bold ? FW_BOLD : FW_NORMAL;
    style.italic = want.italic;
    style.color[0] = style.color[1] = style.color[2] = 255.0f, style.color[3] = 1.0f;
    style.background[3] = 0.0f;
    style.padding = 1;
    style.size = screen_size(want, sy);
    return style;
}

#include "chatfont_wide.h" // Sheet, add_glyph, publish, grow: an area's glyphs as built, and its letters past ASCII

// The glyphs for ASCII at sx, sy screen pixels to the interface's, and their outlines (the letters drawn with a stroke the
// outline's width, the interface's pixels), into an atlas for the graphics layer (add_glyph, publish). The chat's widths into
// g_ours; the windows' letters each centred in the game's width for it. In capitals (Want.upper), a lower-case letter's picture
// and advance are its capital's: the chat's lines wrap by them, a window's words are laid by them (each word from the game's
// start), while the game's width for the letter it printed stays what tells its words apart (cf_row_words). Its letters past
// ASCII are built again as they're drawn (grow).
void build(int which, float sx, float sy)
{
    const Want& want = g_want[which];
    Sheet& s = g_sheet[which];
    s = Sheet{};
    s.want = want, s.sx = sx, s.sy = sy;
    s.style = style_of(want, sy);
    s.outlined = s.style;
    s.stroke = want.outline * sy;
    s.outlined.stroke = static_cast<int>(std::ceil(s.stroke)), s.outlined.exact_stroke = s.stroke;
    s.tried.assign(kCodes, false);
    if (which == kChat)
        std::memcpy(g_ours, g_game, kWidths);
    text::Sprite sprite;
    for (int n = 0; n < kGlyphs; ++n) {
        add_glyph(which, code_of(n), std::wstring(1, letter_of(n)), sprite);
        s.tried[code_of(n)] = true;
    }
    publish(which);
    const text::Style& style = s.style;
    const float stroke = s.stroke;
    g_built[which] = want, g_built_sx[which] = sx, g_built_sy[which] = sy;
    tips_box();
    if (which == kChat)
        g_ours_ready = true;
    log::write(L"%ls: the letters from %ls (%ls%ls%ls), %.1f px (%d on the screen), spacing %.2f, outline %.2f px at %.0f%%, at %.3f x "
               L"%.3f screen pixels to the interface's; widths of 'a' 'm' 'W' %d %d %d (the game's %d %d %d)",
               kArea[which].name, want.family.c_str(), text::installed_face(style).c_str(),
               want.bold ? L", bold" : L"", want.upper ? L", capitals" : L"", want.size, style.size, which == kChat ? want.spacing : 0.0f, stroke,
               want.outline_opacity * 100.0f, sx, sy, (which == kChat ? g_ours : g_game)['a' - 0x20],
               (which == kChat ? g_ours : g_game)['m' - 0x20], (which == kChat ? g_ours : g_game)['W' - 0x20], g_game['a' - 0x20],
               g_game['m' - 0x20], g_game['W' - 0x20]);
}

float read_float(const wchar_t* ini, const std::wstring& key, const wchar_t* fallback)
{
    wchar_t text[64] = {};
    GetPrivateProfileStringW(L"client", key.c_str(), fallback, text, 64, ini);
    return std::wcstof(text, nullptr);
}

// [client] truefont.family, .outline, .outline.opacity: the default font, every area's (its outline while the area's own isn't set);
// truefont.japanese: every area's full-width letters' (kana, kanji), empty each area's own (Windows' Japanese font where it has none)
Want read_default(const wchar_t* ini)
{
    Want want;
    wchar_t text[128] = {};
    GetPrivateProfileStringW(L"client", L"truefont.family", L"Segoe UI", text, 128, ini);
    want.family = text[0] != L'\0' ? text : L"Segoe UI";
    GetPrivateProfileStringW(L"client", L"truefont.japanese", L"", text, 128, ini);
    want.japanese = text;
    const float outline = read_float(ini, L"truefont.outline", L"0");
    want.outline = !(outline > 0.0f) ? 0.0f : outline > 4.0f ? 4.0f : outline;
    const int opacity = static_cast<int>(GetPrivateProfileIntW(L"client", L"truefont.outline.opacity", 100, ini));
    want.outline_opacity = static_cast<float>(opacity < 0 ? 0 : opacity > 100 ? 100 : opacity) / 100.0f;
    return want;
}

// The key area `which` reads <prefix><suffix> from: its own, but the titles', the main menu's and the Action Menu's the buttons' and the party
// list's and the alliance lists' the Status area's while their own isn't set (the labels were one area, buttons.font, until the
// titles and the main menu were split off, and those the Status area's: what was set for both stays theirs). The clock's are
// its own alone: following Status & Numbers, a change there moved the clock too (reported in game 2026-11-06)
// The Target window's name the Status area's and its title the titles' (and so the buttons' in turn) while their own aren't
// set: they were those areas' letters until they were split off (2026-11-07; the name, big letters, was always the Status
// area's: it followed the menus' keys at first, which it had never been drawn in)
// The Profile panel's titles the panel's own (profile.font) while theirs (profile.title.font) aren't set (2026-11-08)
// The chat log's tabs the titles' while theirs (chattabs.font) aren't: their letters were the titles' until 2026-11-08
// Menu Lists the menus' while its own (lists.font) aren't: the Merit Points menu's text was General's until 2026-11-08
// The macro bar's macros' names the menus' while theirs (macrobar.name.font) aren't: they were the menus' until 2026-11-13
std::wstring key_for(const wchar_t* ini, int which, const wchar_t* suffix)
{
    std::wstring key = std::wstring(kArea[which].prefix) + suffix;
    wchar_t text[8] = {};
    const int from = which == kTitles || which == kMainMenu || which == kActionMenu ? kButtons
                     : which == kLists || which == kMacroNames                      ? kMenus
                     : which == kParty || which == kAlliance                        ? kStatus
                     : which == kTarget                                             ? kStatus
                     : which == kTargetTitle || which == kChatTabs                  ? kTitles
                     : which == kProfileTitle                                       ? kProfile
                                                                                    : which;
    // the main menu's and the Action Menu's by their Buttons tab's Enable ([client] mainmenu.buttons, action.buttons, as
    // buttons.cpp reads it): 1 their own keys alone, 0 the buttons' alone; never set, each the buttons' until its own is
    const wchar_t* enable = which == kMainMenu ? L"mainmenu.buttons" : which == kActionMenu ? L"action.buttons" : nullptr;
    if (enable != nullptr && GetPrivateProfileStringW(L"client", enable, L"", text, 8, ini) > 0)
        return std::wcstol(text, nullptr, 10) != 0 ? key : std::wstring(kArea[from].prefix) + suffix;
    if (from != which && GetPrivateProfileStringW(L"client", key.c_str(), L"", text, 8, ini) == 0)
        key = from == kTitles ? key_for(ini, from, suffix) : std::wstring(kArea[from].prefix) + suffix; // (the titles' own follow)
    return key;
}

// [client] `key` as a number; `none` while it isn't set (or isn't a number)
float read_set(const wchar_t* ini, const std::wstring& key, float none)
{
    wchar_t text[64] = {}, * end = nullptr;
    GetPrivateProfileStringW(L"client", key.c_str(), L"", text, 64, ini);
    const float value = std::wcstof(text, &end);
    return end == text || std::isnan(value) ? none : value;
}

// Area `which`'s outline from [client] <prefix>.outline (0-4) and .outline.opacity (percent): its own, the default font's
// (truefont.outline, already in `want`) while unset
void read_outline(const wchar_t* ini, int which, Want& want)
{
    const float outline = read_set(ini, key_for(ini, which, L".outline"), want.outline);
    want.outline = !(outline > 0.0f) ? 0.0f : outline > 4.0f ? 4.0f : outline;
    const float opacity = read_set(ini, key_for(ini, which, L".outline.opacity"), want.outline_opacity * 100.0f);
    want.outline_opacity = (!(opacity > 0.0f) ? 0.0f : opacity > 100.0f ? 100.0f : opacity) / 100.0f;
}

// Area `which` from [client] <prefix> (sharp or game; all but the chat's and the menus' menu.font's until they're set), <prefix>.family (none: the default's), .size, .bold, .italic, .uppercase (not the tooltips': each part's capitals are moved by the game's widths, read_part), .outline (read_outline), .spacing (the chat's), on the default font `look`; the titles' the buttons' until set (key_for)
void read_want(const wchar_t* ini, int which, const Want& look)
{
    const std::wstring prefix = kArea[which].prefix;
    Want& want = g_want[which];
    want = look;
    wchar_t menus[16] = {}, buttons[16] = {}, text[128] = {};
    GetPrivateProfileStringW(L"client", kArea[kMenus].prefix, L"game", menus, 16, ini);
    // (the main menu's and the Action Menu's own switch unset with their Buttons tab's Enable on: the buttons' switch, not the
    // menus'. Buttons' True Font on left them the game's letters while General's was off, reported in game 2026-11-10)
    GetPrivateProfileStringW(L"client", kArea[kButtons].prefix, menus, buttons, 16, ini);
    const wchar_t* unset = which == kChat ? L"game" : which == kMainMenu || which == kActionMenu ? buttons : menus;
    GetPrivateProfileStringW(L"client", key_for(ini, which, L"").c_str(), unset, text, 128, ini);
    want.on = _wcsicmp(text, L"sharp") == 0;
    GetPrivateProfileStringW(L"client", key_for(ini, which, L".family").c_str(), L"", text, 128, ini);
    if (text[0] != L'\0')
        want.family = text;
    want.bold = GetPrivateProfileIntW(L"client", key_for(ini, which, L".bold").c_str(), 0, ini) != 0;
    want.italic = GetPrivateProfileIntW(L"client", key_for(ini, which, L".italic").c_str(), 0, ini) != 0;
    want.upper = which != kTips && GetPrivateProfileIntW(L"client", key_for(ini, which, L".uppercase").c_str(), 0, ini) != 0;
    // (unset, below 0 or not a number: the default; 0: the letters left out, the font built at the default so nothing laid
    // by it moves; 1 to 48 px. Below 4 was a multiple of the game's letters in the chat's first build, never released)
    const float size = read_set(ini, key_for(ini, which, L".size"), -1.0f);
    want.none = want.on && size >= 0.0f && size < 0.5f;
    want.size = !(size >= 0.5f) ? kArea[which].size : size > 48.0f ? 48.0f : size;
    read_outline(ini, which, want);
    const float spacing = which == kChat ? read_float(ini, prefix + L".spacing", L"0") : 0.0f;
    want.spacing = !(spacing >= -4.0f) ? (spacing < -4.0f ? -4.0f : 0.0f) : spacing > 8.0f ? 8.0f : spacing;
    const float height = which == kTips ? read_float(ini, prefix + L".lineheight", L"16") : 16.0f; // (the game's rows: 16 apart)
    want.line = !(height >= 12.0f) ? 1.0f : (height > 32.0f ? 32.0f : height) / 16.0f;
    if (which != kTips)
        return;
    const float top = read_float(ini, prefix + L".padding.top", L"0"), bottom = read_float(ini, prefix + L".padding.bottom", L"0");
    want.pad = !(top > 0.0f) ? 0.0f : top > 48.0f ? 48.0f : top;
    want.pad_bottom = !(bottom > 0.0f) ? 0.0f : bottom > 48.0f ? 48.0f : bottom;
}

// A colour as the settings write it ("#RRGGBB"): 0xFFRRGGBB, 0 for none (the game's)
std::uint32_t colour_of(const wchar_t* text)
{
    while (*text == L' ' || *text == L'#')
        ++text;
    wchar_t* end = nullptr;
    const unsigned long rgb = std::wcstoul(text, &end, 16);
    return end == text + 6 ? 0xFF000000u | static_cast<std::uint32_t>(rgb & 0xFFFFFFu) : 0u;
}

// [client] tips.font.<part>.size: a tooltip part's rows (the item's name, its type, its stats, its jobs: d3d8_tips.h) in
// their font at this size, its letters centred in the game's widths made as much wider; none (or the rows' size) leaves them
// the rows'. .color: its letters' colour (#RRGGBB; none the game's); .space: the interface's pixels above it (not the title's);
// .numbers.color: its numbers' colour, signs and % with them (none the part's); .opacity: how opaque its letters are, 0 to 100
// (100), times tips.font.opacity's for every part; .bold, .italic and .outline (none the rows': tips.font's) its own font where
// they aren't the rows'; .uppercase: its letters in capitals (d3d8_tips.h cf_tip_upper; none tips.font.uppercase, every part's).
float read_opacity(const wchar_t* ini, const std::wstring& key)
{
    wchar_t text[32] = {}, * end = nullptr;
    GetPrivateProfileStringW(L"client", key.c_str(), L"", text, 32, ini);
    const float percent = std::wcstof(text, &end);
    if (end == text || !(percent < 100.0f)) // (empty, not a number, a NaN: opaque)
        return 1.0f;
    return percent > 0.0f ? percent / 100.0f : 0.0f;
}

void read_part(const wchar_t* ini, int part)
{
    const Want& tips = g_want[kTips];
    Want& own = g_want[kTipsTitle + part];
    own = tips;
    own.line = 1.0f, own.pad = own.pad_bottom = 0.0f; // (the rows' own: cf_spread reads them off the tooltips' font)
    const std::wstring key = std::wstring(L"tips.font.") + kTipParts[part];
    const float size = read_set(ini, key + L".size", -1.0f);
    const bool gone = size >= 0.0f && size < 0.5f; // (0: the part's letters not drawn, as at opacity 0)
    const bool sized = size >= 0.5f && std::fabs(size - tips.size) >= 0.5f;
    own.bold = GetPrivateProfileIntW(L"client", (key + L".bold").c_str(), tips.bold ? 1 : 0, ini) != 0;
    own.italic = GetPrivateProfileIntW(L"client", (key + L".italic").c_str(), tips.italic ? 1 : 0, ini) != 0;
    const float outline = read_set(ini, key + L".outline", tips.outline);
    own.outline = !(outline > 0.0f) ? 0.0f : outline > 4.0f ? 4.0f : outline;
    own.on = tips.on && (sized || own.bold != tips.bold || own.italic != tips.italic || std::fabs(own.outline - tips.outline) >= 0.01f);
    own.size = sized ? (size > 48.0f ? 48.0f : size) : tips.size;
    own.widths = own.size / tips.size;
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", (key + L".color").c_str(), L"", text, 32, ini);
    g_tip_parts[part].color = colour_of(text);
    GetPrivateProfileStringW(L"client", (key + L".numbers.color").c_str(), L"", text, 32, ini);
    g_tip_parts[part].numbers = colour_of(text);
    const float space = part == VANA_TIP_TITLE ? 0.0f : read_float(ini, key + L".space", L"0");
    g_tip_parts[part].space = !(space > 0.0f) ? 0.0f : space > 48.0f ? 48.0f : space;
    g_tip_parts[part].fade = gone ? 1.0f : 1.0f - read_opacity(ini, L"tips.font.opacity") * read_opacity(ini, key + L".opacity");
    const int every = static_cast<int>(GetPrivateProfileIntW(L"client", L"tips.font.uppercase", 0, ini));
    g_tip_parts[part].upper = GetPrivateProfileIntW(L"client", (key + L".uppercase").c_str(), every, ini) != 0 ? 1u : 0u;
}

// [client] party.font.numbers.size (alliance.font.numbers.size): the list's numbers (HP, MP, TP: d3d8_chatfont.h cf_big_word)
// in the list's font at this size, the names staying at the list's; none (or the names' size) leaves them the list's; 0 leaves
// them out (d3d8_chatfont_big.h cf_big_word). Read after the list's own (read_want).
void read_numbers(const wchar_t* ini, int which, int list)
{
    const Want& names = g_want[list];
    Want& own = g_want[which];
    own = names;
    const float size = read_set(ini, std::wstring(kArea[which].prefix) + L".size", -1.0f);
    const bool sized = size >= 0.5f && std::fabs(size - names.size) >= 0.5f;
    own.none = names.on && size >= 0.0f && size < 0.5f;
    own.on = names.on && sized;
    own.size = sized ? (size > 48.0f ? 48.0f : size) : names.size;
}

// The area of the window of `layout`, by its name (16 letters at +0x46: "menu    " and its own 8). One kWindows doesn't
// name is the main menu's if it's a list the main menu opens in its place, or one those open in theirs, however deep
// (hudwin's main_menu_list: laid out where the main menu is, as mainmenu.background takes them): Config's Windows and
// Log lists kept Global's buttons and the menus' font while Config's own list had the main menu's (reported 2026-11-10)
int area_of(const char* layout)
{
    const char* name = layout + 0x46;
    const char* own = name + 8;
    if (std::memcmp(name, "menu    ", 8) != 0)
        return kMenus;
    if (std::memcmp(own, "item", 4) == 0 &&
        (std::isdigit(static_cast<unsigned char>(own[4])) != 0 || own[4] == 'x' || std::memcmp(own + 4, "info", 4) == 0))
        return kTips; // an item's details, each size of them
    for (const Named& window : kWindows)
        if (std::strncmp(own, window.name, std::strlen(window.name)) == 0)
            return window.area;
    return hudwin::main_menu_list(layout) ? kMainMenu : kMenus;
}

// Each window's area in the log the first time it's drawn with a window font on (which windows are which)
void note_area(const char* name, int area)
{
    static std::unordered_set<std::uint64_t> seen; // (each name's FNV-1a: it's looked up at every window's draw)
    std::uint64_t hash = 14695981039346656037ull;
    for (int i = 0; i < 16; ++i)
        hash = (hash ^ static_cast<unsigned char>(name[i])) * 1099511628211ull;
    if (seen.size() >= 256 || !seen.insert(hash).second)
        return;
    log::write(L"True Font: the window \"%hs\" is in the %ls font", std::string(name, 16).c_str(), kArea[area].name);
}

// True Font's state in one line: the areas on and built, what was found of the game's, the hooks' counts since the last
// report, and the graphics layer's (vanacore_gfx_chat_font_report). [client] truefont.debug writes it every kReport frames;
// else once, after kWait frames with an area on and nothing built (waiting)
constexpr int kReport = 300, kWait = 1200;
void report(bool seen, float sx, float sy)
{
    std::wstring on, built;
    for (int which = kChat; which < kAreas; ++which) {
        if (g_want[which].on)
            on += (on.empty() ? L"" : L", ") + std::wstring(kArea[which].name);
        if (g_built[which].on)
            built += (built.empty() ? L"" : L", ") + std::wstring(kArea[which].name);
    }
    char gfx[1024] = {};
    vanacore_gfx_chat_font_report(gfx, sizeof gfx);
    bool known = false;
    const std::uint32_t target = uiscale::menu_target(known);
    log::write(L"True Font: on for %ls; built for %ls; the game's letter widths %ls; hooks: chat rows %d, line wrap %d, item lines %d, "
               L"letter printer %d, macro names %d, help line %d, centred text %d, right-aligned text %d; the game's menu target %ls (%08X); the "
               L"interface's scale %ls (%.3f x %.3f); since the last report: chat windows drawn %u, chat rows %u, lines wrapped %u, item lines set %u, windows' "
               L"areas %u, widths held %u, letters printed %u",
               on.empty() ? L"nothing" : on.c_str(), built.empty() ? L"nothing" : built.c_str(), g_table != nullptr ? L"found" : L"NOT found",
               g_hooked[0], g_hooked[1], g_hooked[2], g_hooked[3], g_hooked[4], g_hooked[5], g_hooked[6], g_hooked[7],
               known ? (target != 0 ? L"is a texture" : L"is none (the screen)") : L"can't be read",
               target, seen ? L"known" : L"NOT known yet", sx, sy, g_counts.drawings, g_counts.rows, g_counts.wraps, g_counts.lines,
               g_counts.areas, g_counts.holds, g_counts.letters);
    log::write(L"True Font: graphics: %hs", gfx); // (its own line: a log line holds 1023 characters)
    g_counts = Counts{};
}

void waiting(bool seen, float sx, float sy)
{
    static int frames = 0, since = 0;
    static bool said = false;
    bool any = false;
    for (int which = kChat; which < kAreas; ++which)
        any = any || g_want[which].on;
    if (g_debug && any && ++since >= kReport) {
        since = 0;
        report(seen, sx, sy);
    }
    frames = seen || !any ? 0 : frames + 1;
    if (frames != kWait || said)
        return;
    said = true;
    log::write(g_table == nullptr
                   ? L"True Font: on, but nothing drawn in %d frames: the game's letter widths weren't found in FFXiMain.dll"
                   : L"True Font: on, but nothing drawn in %d frames: the interface's size on the screen isn't known yet (no chat row "
                     L"or letter of the game's seen drawn into it, or it was never seen laid onto the screen; the graphics' lines "
                     L"\"True Font: the interface ...\" say which). [client] truefont.debug=1 reports every %d frames",
               kWait, kReport);
    if (!g_debug)
        report(seen, sx, sy);
}

} // namespace

void reload(const wchar_t* ini)
{
    const Want look = read_default(ini);
    for (int which = kChat; which <= kTips; ++which)
        read_want(ini, which, look);
    for (int part = 0; part < VANA_TIP_PARTS; ++part)
        read_part(ini, part);
    read_numbers(ini, kPartyNumbers, kParty);
    read_numbers(ini, kAllianceNumbers, kAlliance);
    // [client] truefont.disable=1: every area's True Font off at once (their size 0's letters left out with it), each area's
    // settings kept for when it's taken out. Turning Menus & Lists off kept on those following it, the buttons' and the
    // Equipment window's among them, whose switches are on Interface (reported in game 2026-11-08)
    if (GetPrivateProfileIntW(L"client", L"truefont.disable", 0, ini) != 0)
        for (Want& want : g_want)
            want.on = false, want.none = false;
    // [client] truefont.gdi=1: every True Font text drawn with GDI, none with DirectWrite (text.cpp)
    text::set_gdi_only(GetPrivateProfileIntW(L"client", L"truefont.gdi", 0, ini) != 0);
    vanacore_gfx_set_tip_parts(g_tip_parts);
    tips_box(); // (the spaces' guess)
    const bool debug = GetPrivateProfileIntW(L"client", L"truefont.debug", 0, ini) != 0;
    if (debug != g_debug)
        log::write(L"True Font: [client] truefont.debug=%d: its steps %ls", debug ? 1 : 0,
                   debug ? L"reported every 300 frames, the first draws of each kind described" : L"no longer reported");
    g_debug = debug;
    vanacore_gfx_chat_font_debug(debug ? 1 : 0);
    // the macro bar's keys: their True Font letters' colour (macrobar.font.color, #RRGGBB; none the game's), and "Ctrl" / "Alt"
    // left out (macrobar.prefix=0)
    wchar_t colour[32] = {};
    GetPrivateProfileStringW(L"client", L"macrobar.font.color", L"", colour, 32, ini);
    g_macro_bare = GetPrivateProfileIntW(L"client", L"macrobar.prefix", 1, ini) == 0;
    vanacore_gfx_set_macrobar(colour_of(colour), g_macro_bare ? 0 : 1);
    macrotext::look(colour_of(colour), g_macro_bare); // (the labels drawn whole: macrotext.cpp)
    // a main menu label the game squeezes to fit its button ("Communication"): at the font's full size (mainmenu.font.fixed,
    // on unset: "still small" at 85% wide, 2026-11-09), else (0) its letters narrowed as the game's are, to 85% at most
    vanacore_gfx_label_fixed(GetPrivateProfileIntW(L"client", L"mainmenu.font.fixed", 1, ini) != 0 ? 1 : 0);
    wchar_t slots[16] = {}; // the Equipment window's slot names: game, sharp (True Font) or hide (left out)
    GetPrivateProfileStringW(L"client", kArea[kEquip].prefix, L"", slots, 16, ini);
    g_equip_hide = _wcsicmp(slots, L"hide") == 0;
    g_equip_whole = g_want[kEquip].on && !g_want[kEquip].none;
    // an area's letters left out: every one while its True Font size is 0 (Want.none: 2), the slot names' (1: left out, or drawn
    // whole by listtext.cpp)
    for (int which = kChat; which < kAreas; ++which)
        vanacore_gfx_hide_letters(which, g_want[which].none ? 2 : which == kEquip && (g_equip_hide || g_equip_whole) ? 1 : 0);
}

void install(const wchar_t* ini)
{
    reload(ini);
    itemtips::install(ini); // [client] tips.extended: the item details' pages as one, through this SetLines hook
    tipicons::install(ini); // [client] tips.icon.*, tips.badges.*: the item details' picture and badges
}

void on_frame()
{
    if (g_held != 0) // (never: the chat's hooks are done before the frame's end)
        return;
    find_table(); // (and the row hook, for chat.shadow too, whatever draws the letters)
    if (g_macro_bare || g_equip_hide || g_equip_whole)
        send_labels(); // ("Ctrl" / "Alt", the slot names, known by their sprites, the game's letters too)
    tips_again();
    {
        bool known = false; // the game's own word on its interface's target, for the graphics layer (d3d8_chatfont.h cf_target)
        const std::uint32_t target = uiscale::menu_target(known);
        vanacore_gfx_set_interface_target(target, known ? 1 : 0);
    }
    float sx = 0.0f, sy = 0.0f;
    const bool seen = g_table != nullptr && vanacore_gfx_chat_scale(&sx, &sy); // (not until the chat is laid onto the screen)
    waiting(seen, sx, sy);
    for (int which = kChat; which < kAreas; ++which) {
        const Want& want = g_want[which];
        if (!want.on) {
            if (g_built[which].on)
                drop(which);
        } else if (seen && !(g_built[which] == want && std::fabs(sx - g_built_sx[which]) < 0.002f &&
                             std::fabs(sy - g_built_sy[which]) < 0.002f)) {
            if (which == kButtons || which == kTitles || which == kStatus || which == kClock || which == kParty || which == kAlliance ||
                which == kTrade || which == kGil || which == kProfile || which == kProfileTitle || which == kMainMenu || which == kEquip ||
                which == kEquipStats ||
                which == kMacroBar || which == kBuffs || which == kActionMenu || which == kTarget || which == kTargetTitle) // (the timers are the labels' sheet's letters)
                send_labels();
            build(which, sx, sy);
        }
    }
    jplabels::on_frame(); // (a game in Japanese: its labels' words)
    grow();               // (the letters past ASCII and the Japanese labels' words drawn with none yet)
}

void hold(bool on)
{
    if (on) {
        ++g_counts.holds;
        if (g_held++ == 0 && g_ours_ready && g_table != nullptr)
            std::memcpy(g_table, g_ours, kWidths);
    } else if (g_held > 0 && --g_held == 0 && g_table != nullptr) {
        std::memcpy(g_table, g_game, kWidths);
    }
}

Drawing::Drawing()
{
    ++g_counts.drawings;
    hold(true);
    vanacore_gfx_chat_drawing(1);
}

Drawing::~Drawing()
{
    vanacore_gfx_chat_drawing(0);
    hold(false);
}

Area::Area(const char* layout) : was_(-1)
{
    // (with the labels' areas on too, not only a window's: the main menu's window told apart while only the buttons' are on, its
    // letters the game's while its own font is off; a trade's, Current Gil's and the Equipment window's, every letter of which is
    // its own area's, while only the titles' or Status & Numbers are on: their "+" and "%" were the titles' and their digits the
    // game's, the Equipment stats' "+3%" read "+ %", reported 2026-11-06)
    bool any = g_built[kTips].on || g_built[kButtons].on || g_built[kTitles].on || g_built[kStatus].on || g_built[kClock].on ||
               g_macro_bare || g_equip_hide || g_equip_whole; // (macrobar.prefix=0, equip.font=hide: their windows told apart, True Font on or off)
    for (int which = kChat; which < kTips && !any; ++which)
        any = windowed(which) && g_built[which].on;
    if (layout == nullptr || !any)
        return;
    const char* name = layout + 0x46;
    const int area = area_of(layout);
    const bool lists = list_window(name); // (its text Menu Lists', its buttons and labels the area's)
    ++g_counts.areas;
    note_area(name, lists ? kLists : area);
    was_ = vanacore_gfx_text_area(area | (lists ? VANA_TEXT_LISTS : 0));
}

Area::~Area()
{
    if (was_ >= 0)
        vanacore_gfx_text_area(was_);
}

bool details_window(const char* layout)
{
    return layout != nullptr && area_of(layout) == kTips;
}

bool main_menu_window(const char* layout)
{
    return layout != nullptr && area_of(layout) == kMainMenu;
}

bool action_menu_window(const char* layout)
{
    return layout != nullptr && area_of(layout) == kActionMenu;
}

bool labels_on()
{
    return g_want[kButtons].on || g_want[kTitles].on || g_want[kMainMenu].on || g_want[kActionMenu].on;
}

int game_width(int code)
{
    return g_table != nullptr && code >= 0 && code < static_cast<int>(kWidths) ? g_game[code] : -1;
}

bool area_on(int area) { return area >= 0 && area < kAreas && g_built[area].on; }

float text_width(int area, const char* text)
{
    if (area < 0 || area >= kAreas || !g_built[area].on || text == nullptr)
        return -1.0f;
    float width = 0.0f;
    for (const char* c = text; *c != '\0'; ++c) {
        const int code = static_cast<unsigned char>(*c) - 0x20, game = game_width(code);
        width += code >= 0 && code < kAscii ? g_advance[area][code] : static_cast<float>(game > 0 ? game : 0);
    }
    return width;
}

bool area_style(int area, text::Style& letters, text::Style& stroked, bool& upper, float& outline_opacity)
{
    if (area < 0 || area >= kAreas)
        return false;
    const Want& want = g_built[area];
    const float sy = g_built_sy[area];
    if (!want.on || want.none || !(sy > 0.0f))
        return false;
    letters = style_of(want, sy), stroked = letters; // (as build makes its letters and their outlines)
    const float stroke = want.outline * sy;
    stroked.stroke = static_cast<int>(std::ceil(stroke)), stroked.exact_stroke = stroke;
    upper = want.upper;
    outline_opacity = want.outline_opacity;
    return true;
}

bool input_style(text::Style& letters, text::Style& stroked, bool& upper)
{
    float opacity = 0.0f;
    return area_style(kInput, letters, stroked, upper, opacity);
}

char label_char(int u, int v)
{
    // the sprite at that place, else one a texel off it (the menu resource's coordinates are fractions of the sheet)
    for (int off = 0; off <= 1; ++off) {
        const auto at = [&](const LabelSprite& s) { return std::abs(s.u - u) <= off && std::abs(s.v - v) <= off; };
        for (const LabelSprite& s : kLabelSprites)
            if (at(s))
                return s.c;
        for (const LabelSprite& s : kBigSprites)
            if (at(s))
                return s.c;
    }
    return 0;
}

bool label_letter(int u, int v, char& c, int& base)
{
    for (int off = 0; off <= 1; ++off)
        for (const LabelSprite& s : kLabelSprites)
            if (std::abs(s.u - u) <= off && std::abs(s.v - v) <= off) {
                c = s.c, base = s.row + 10 - v;
                return true;
            }
    return false;
}

} // namespace vanacore::chatfont
