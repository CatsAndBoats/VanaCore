#include "statuswindow.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "gametext.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "uispace.hpp"

namespace vanacore::statuswindow {
namespace {

// CTkPersona::OnDrawPrimitive in the PS2 client's names (FUN_10146ac0), __thiscall on the window, no arguments: the
// Status window's text each frame it's shown, its rect at +14h (left, top, right, bottom shorts). The player's data
// (FUN_100e8790(0): the name at +2, HP, MP, TP at +20h, +24h, +28h) or nothing; the name with YkDrawStringLimited at
// +4,+7; then, with the jobs known (bit 0 of a flags byte), "Lv75" and the job's picture at +4,+13h and +4,+1Fh, "ILv"
// at +2Bh, HP, MP and TP's numbers at +3Ah, +46h, +52h and the seven stats from +60h, 0Dh apart, each with its bonus in
// green or its loss in red. The labels (HP, STR...) are letters in the window's picture, drawn before this.
//
// sub esp,24h; push ebx; push ebp; push esi; push 1; mov esi,ecx; call (menu resource 1); push 0; mov ebp,eax; call
// (the player's data); mov ebx,eax; add esp,8; test ebx,ebx; jz; mov dx,[esi+16h]; mov ax,[esi+14h]; lea ecx,[ebx+2];
// push 80808080h
constexpr char kPersonaDraw[] = "83 EC 24 53 55 56 6A 01 8B F1 E8 ?? ?? ?? ?? 6A 00 8B E8 E8 ?? ?? ?? ?? 8B D8 83 C4 08 "
                                "85 DB 0F 84 ?? ?? ?? ?? 66 8B 56 16 66 8B 46 14 8D 4B 02 68 80 80 80 80";
constexpr std::size_t kPrologue = 5;           // sub esp,24h; push ebx; push ebp
constexpr std::ptrdiff_t kResourceCall = 0x0A; // __cdecl (1): the menu resource the jobs' pictures are in
constexpr std::ptrdiff_t kPlayerCall = 0x13;   // __cdecl (0): the player's data
constexpr std::ptrdiff_t kLimitedCall = 0x4A;  // YkDrawStringLimited (width, x, y, text, colour)
constexpr std::ptrdiff_t kFlagsAt = 0x50;      // mov al,[flags]: bit 0 once the jobs are known
constexpr std::ptrdiff_t kJobsAt = 0x60;       // mov al,[jobs]: main job, its level, sub job, its level (bytes)
constexpr std::ptrdiff_t kSyncCall = 0x6D;     // __cdecl: an object whose byte +3 has 40h under level sync
constexpr std::ptrdiff_t kSyncColourAt = 0x7B; // mov edi,802060A0h: the level's colour then
constexpr std::ptrdiff_t kMessageCall = 0x92;  // __cdecl (table, id): the game's text ("Lv%d" 0Ah,59h)
constexpr std::ptrdiff_t kStringCall = 0xBD;   // YkDrawString (x, y, text, colour): the right edge
constexpr std::ptrdiff_t kShapeCall = 0xEF;    // YmMenuShape::Draw, __thiscall (x, y, colour, 0, 0): the main job's picture
constexpr std::ptrdiff_t kSubShapeCall = 0x16A; // and the sub job's
// Before each job picture's call: mov ecx,[edx+ecx*4+350h] (the picture); push eax (x)
constexpr std::uint8_t kPictureLoad[] = {0x8B, 0x8C, 0x8A, 0x50, 0x03, 0x00, 0x00, 0x50};
constexpr std::ptrdiff_t kItemLevelCall = 0x16F; // __cdecl: the item level (0: none)
constexpr std::ptrdiff_t kSuperiorCall = 0x176;  // __cdecl: the superior level
constexpr std::ptrdiff_t kReverseCall = 0x1F7;   // YkDrawStringReverse (right, top, text, colour, 1, 1.0, 1.0)
constexpr std::ptrdiff_t kWarningCall = 0x239;   // __thiscall (which, ret 4): white, or red for a lowered value
constexpr std::ptrdiff_t kMaxHpAt = 0x2AA;       // mov eax,[max HP]
constexpr std::ptrdiff_t kMaxMpAt = 0x353;       // mov eax,[max MP]
constexpr std::ptrdiff_t kStatsAt = 0x412;       // mov cx,[ebp*2+stats]: the seven stats (shorts)
constexpr std::ptrdiff_t kBonusesAt = 0x441;     // mov ax,[ebp*2+bonuses]: what gear and effects add (signed shorts)
// YmMenuShape::Draw calls the quad drawing (FUN_1011e580, __thiscall on the quad, ret 18h: x, y, scale x, scale y,
// colour, blending) for each of its quads, here
constexpr std::ptrdiff_t kQuadCall = 0x6D;
// YkDrawString's font: its first call, the fonts' getter (font 0)
constexpr std::ptrdiff_t kFontCall = 0x69;
constexpr std::ptrdiff_t kScaledCall = 0x20; // YkDrawString passes its arguments on with a scale: the call

// The window: its rect, its control (the picture's at its +14h list) and the mark behind a lowered value
constexpr std::ptrdiff_t kRectAt = 0x14;
constexpr std::ptrdiff_t kCtrlAt = 8;
constexpr std::ptrdiff_t kMarkAt = 0x24;
constexpr int kHpWarning = 0x17, kMpWarning = 0x18, kStatWarning = 0x10; // the warning routine's arguments
// The player's data
constexpr std::ptrdiff_t kNameAt = 2, kHpAt = 0x20, kMpAt = 0x24, kTpAt = 0x28;
constexpr std::ptrdiff_t kJobPictures = 0x350; // in the menu resource's table: a job's picture, 4 bytes each

// A menu picture's quads (YmMenuShape, as itemslot.cpp's): the list at +4, its count at +14h; a list node's next at +0
// and its item at +10h. A quad: its texture's name at +32h, its corners from +50h, 1Ch apart (x, y, z, rhw, colour,
// u, v; the drawn positions, worked out each draw from the shorts at +C0h (x) and +C8h (y), the corners in the order
// top left, top right, bottom left, bottom right)
constexpr std::ptrdiff_t kParts = 4, kPartCount = 0x14, kNodeItem = 0x10;
constexpr std::ptrdiff_t kQuadName = 0x32, kCorners = 0x50, kCorner = 0x1C, kColour = 0x10, kAlpha = 0x13, kU = 0x14,
                         kV = 0x18, kLocalX = 0xC0, kLocalY = 0xC8;
constexpr std::size_t kQuadSize = 0xD0;
constexpr char kLetterTexture[16] = {'f', 'o', 'n', 't', ' ', ' ', ' ', ' ', 'f', 'o', 'n', 't', ' ', ' ', ' ', ' '};
constexpr std::ptrdiff_t kElements = 0x14; // the control's pictures (each's type at +0, 0 a picture, its shape at +8)
// Font 0's glyphs: the font (from the getter) points at a table of shapes, by the letter less 20h. The dash's quad is
// 8 texels square; its stroke's white row is 3.5, 6.5 texels into it (the rows above and below are its dark outline).
constexpr float kWhiteTexel[2] = {3.5f, 6.5f};
constexpr int kDashTexels = 8;

// The game's resize of a window's control (FUN_1011a1a0, __thiscall, ret 1Ch: x, y, width, height, then 1, 0, 0 to
// keep the size in its frame), its pictures moved with it:
// sub esp,8; push esi; mov esi,ecx; lea ecx,[esi+14h]; call (the list's first); test eax,eax; mov [esp+8],eax; ...
constexpr char kResize[] = "83 EC 08 56 8B F1 8D 4E 14 E8 ?? ?? ?? ?? 85 C0 89 44 24 08 C7 44 24 04 00 00 00 00 0F 84";
// The windows' list, from the menu manager's drawing of them (each layer in turn): xor bl,bl; mov ecx,list; call (its
// first); test eax,eax; mov [esp+0Ch],eax; jz; lea eax,[esp+0Ch]; mov ecx,list; push eax; call
constexpr char kWindows[] = "32 DB B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 89 44 24 0C 74 3A 8D 44 24 0C B9 ?? ?? ?? ?? 50 E8";
constexpr std::ptrdiff_t kWindowsAt = 3;
// A control: its window (with the window's own rect at +14h, which its text is drawn from) at +0Ch, its rect (left,
// top, right, bottom shorts) at +3Ah, set when the game makes it from the menu's resource and by every resize. (The x,
// y, width, height at +8Ch are written by the resize only, so in a window the game has never resized, like the Gil
// window under the Status window in Equipment, they're whatever the control's memory held before: not read.)
constexpr std::ptrdiff_t kWindowAt = 0xC, kCtrlRectAt = 0x3A;
// A control's layout (+4) and its 16-character name (+0x46, hudwin.cpp's); the Level / EXP window's
constexpr std::ptrdiff_t kLayoutAt = 4, kLayoutNameAt = 0x46;
constexpr char kLevelName[] = "menu    level   ";
// YmMenuShape::Draw with a scale (FUN_1011f880, __thiscall, ret 20h: x, y, scale x, scale y, colour, centred, blending,
// quads to skip), for a job's picture made narrower to fit: sub esp,0Ch; push ebx; push ebp; push esi; mov esi,ecx;
// push edi; mov ebp,[esi+14h]; lea ecx,[esi+4]; mov [esp+18h],ebp; mov [esp+14h],ecx; call. Its first 5 bytes left out:
// buttons.cpp hooks it there ([client] buttons.style), maybe before this looks for it
constexpr char kShapeScaled[] = "?? ?? ?? ?? ?? 56 8B F1 57 8B 6E 14 8D 4E 04 89 6C 24 18 89 4C 24 14 E8";

// CTkPersonaLevel::OnDrawPrimitive (FUN_101470a0), __thiscall, no arguments: the Level / EXP window under the Status
// window in the main menu: "Level 75" at +8,+0Ah, "EXP" and its number, "NEXTLevel" and its number below, 0Ch apart,
// and the bar at +48h, all from its rect's top (+16h); its labels are the game's text (table 0Ch: 56h, 57h, 58h). With
// the job mastered (bit 0 of a flags byte, +1Ch clear) it shows the master level's instead ("M. Level"...), untouched.
// sub esp,28h; push ebp; push esi; push edi; mov esi,ecx; call (shown?); test al,al; jz; call (level sync); test eax,eax;
// jz; test byte ptr [eax+3],40h; jz; mov edi,80404040h
constexpr char kLevelDraw[] = "83 EC 28 55 56 57 8B F1 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 12 "
                              "F6 40 03 40 74 0C BF 40 40 40 80";
constexpr std::size_t kLevelPrologue = 5;        // sub esp,28h; push ebp; push esi
constexpr std::ptrdiff_t kMasterFlagsAt = 0x43;  // mov al,[flags]
constexpr std::ptrdiff_t kMasterAt = 0x1C;       // in the window: the master level's shown only while it's clear
constexpr std::ptrdiff_t kLevelLineCall = 0x1ED; // YkDrawString of "Level 75"
constexpr std::ptrdiff_t kNextLabelCall = 0x263; // the game's text 0Ch,58h: "NEXTLevel"
constexpr int kLevelRow = 12;                     // "Level 75" to "EXP"

// The menu font (the "font    moji" texture, 16x16 cells), the titles' ("Equipment"), as in itemcount.cpp: the whole
// string printer (FUN_1011ba10, __thiscall on the renderer, ret 8: text, int pos[2]) and the routines it calls
constexpr char kPrintString[] = "83 EC 08 8B 44 24 10 53 55 8B 6C 24 14 8B 50 04 56 8B F1 8B 08 89 4C 24 0C 57 8B CE 89 54 24 14 "
                                "89 6C 24 20 E8 ?? ?? ?? ?? 8B CE E8";
constexpr std::ptrdiff_t kStatesCall = 0x24;
constexpr std::ptrdiff_t kBindCall = 0x2B;
constexpr std::ptrdiff_t kCodeCall = 0x50;
constexpr std::ptrdiff_t kPrintCall = 0x93;
constexpr std::ptrdiff_t kAdvanceCall = 0x9E;
constexpr char kTitleText[] = "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? "
                              "8D 8E A4 00 00 00 C7 40 64 80 80 80 80";
constexpr std::ptrdiff_t kRendererAt = 2;
constexpr std::ptrdiff_t kCornerColours[] = {0x10, 0x2C, 0x48, 0x64};
constexpr int kMenuCell = 16;
// A job's name in Japanese in the menu font (font 0 has no kana or kanji): its cells 12 tall, about font 0's letters,
// from a little above the row
constexpr float kJobScale = 0.75f;
constexpr int kJobRise = 1;

// The game's language, in its settings: from the getter of the language's own menu sheets (FUN_10259b10): mov ecx,
// [settings]; mov dl,[ecx+18h]; mov eax,[ecx+8]; test dl,dl; jns; cmp eax,2; jz; cmp eax,3; jnz; mov eax,1; push eax;
// call
constexpr char kLanguage[] = "8B 0D ?? ?? ?? ?? 8A 51 18 8B 41 08 84 D2 79 0F 83 F8 02 74 05 83 F8 03 75 05 "
                             "B8 01 00 00 00 50 E8";
constexpr std::ptrdiff_t kSettingsAt = 2; // the global holding the settings' address
constexpr std::ptrdiff_t kLanguageAt = 8; // in them: 0 Japanese, 1 English, 2 French, 3 German

constexpr std::uint32_t kWhite = 0x80808080; // the game's colours: 80h is full (drawn doubled)
constexpr std::uint32_t kBonus = 0x80408040, kLoss = 0x80804040;
constexpr int kAway = -2000; // text measured off the screen

// The layout, in the interface's units from the window's left and top. The game's window is 112 x 190, its rows 12
// apart from +7 and its stats 13 apart from +60h, with no room left below them. The columns are the game's moved right
// by kShift (its text was 4 from the left edge).
constexpr int kInset = 7;
constexpr int kShift = kInset - 4;
constexpr int kGlyph = 11; // font 0's letters' height
struct Rows {
    int name;       // the big name's cell (the small one is at +7)
    int jobs;       // the first job row
    int item_level; // 0: not shown
    int gauges;     // HP, MP, TP
    int stats;
    int step;       // between rows
    int height;     // the window's, for status.taller (0: the game's)
};
// Taller, the rows 13 apart and the window as tall as they need (at least the game's); else in the game's 190, 12
// apart, the stats at its bottom
Rows layout(bool taller, int job_rows, bool item_level)
{
    Rows rows{};
    const int above = job_rows + (item_level ? 1 : 0); // rows between the name and HP
    rows.step = taller ? 13 : 12;
    rows.name = taller ? 5 : 4;
    rows.jobs = rows.name + (taller ? 20 : 18);
    rows.item_level = item_level ? rows.jobs + job_rows * rows.step : 0;
    rows.gauges = rows.jobs + above * rows.step + (taller ? 3 : 2);
    const int after_tp = rows.gauges + 3 * rows.step + (taller ? 10 : 6);
    if (taller) {
        rows.stats = after_tp;
        rows.height = rows.stats + 6 * rows.step + kGlyph + 9;
    } else {
        const int bottom = 190 - 5 - kGlyph - 6 * rows.step;
        rows.stats = after_tp > bottom ? after_tp : bottom;
    }
    return rows;
}
constexpr int kSlashRight = 0x46 + kShift; // HP/MP's "1302/" (the game's); TP's number lines up with the 1302
constexpr int kMaxX = 0x48 + kShift;
constexpr int kItemLevelRight = 0x46 + kShift, kSuperiorX = 0x4A + kShift;
constexpr int kStatX = 0x32 + kShift;
constexpr int kMarkX = 0x17 + kShift, kStatMarkX = 0x1F + kShift; // the game's marks behind a lowered value

constexpr const char* kJobNames[] = {"",    "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                     "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN", "MON"};
constexpr int kJobs = static_cast<int>(std::size(kJobNames));
constexpr int kPictured = 23; // the jobs with a picture of their name in the game's menus: up to RUN
// The jobs' names for a game in Japanese, in Shift-JIS as the game's text is: the name, and the short one for "75 SAM /
// 37 WHM" on one row
struct JapaneseJob {
    const char* name;
    const char* abbr;
};
constexpr JapaneseJob kJapaneseJobs[] = {
    {"", ""},
    {"\x90\xED\x8E\x6D", "\x90\xED"},                                                 // WAR: Senshi
    {"\x83\x82\x83\x93\x83\x4E", "\x83\x82"},                                         // MNK: Monku
    {"\x94\x92\x96\x82\x93\xB9\x8E\x6D", "\x94\x92"},                                 // WHM: Shiro Madoushi
    {"\x8D\x95\x96\x82\x93\xB9\x8E\x6D", "\x8D\x95"},                                 // BLM: Kuro Madoushi
    {"\x90\xD4\x96\x82\x93\xB9\x8E\x6D", "\x90\xD4"},                                 // RDM: Aka Madoushi
    {"\x83\x56\x81\x5B\x83\x74", "\x83\x56"},                                         // THF: Shiifu
    {"\x83\x69\x83\x43\x83\x67", "\x83\x69"},                                         // PLD: Naito
    {"\x88\xC3\x8D\x95\x8B\x52\x8E\x6D", "\x88\xC3"},                                 // DRK: Ankoku Kishi
    {"\x8F\x62\x8E\x67\x82\xA2", "\x8F\x62"},                                         // BST: Kemono Tsukai
    {"\x8B\xE1\x97\x56\x8E\x8D\x90\x6C", "\x8E\x8D"},                                 // BRD: Gin'yuu Shijin
    {"\x8E\xEB\x90\x6C", "\x8E\xEB"},                                                 // RNG: Karyuudo
    {"\x8E\x98", "\x8E\x98"},                                                         // SAM: Samurai
    {"\x94\x45\x8E\xD2", "\x94\x45"},                                                 // NIN: Ninja
    {"\x97\xB3\x8B\x52\x8E\x6D", "\x97\xB3"},                                         // DRG: Ryuu Kishi
    {"\x8F\xA2\x8A\xAB\x8E\x6D", "\x8F\xA2"},                                         // SMN: Shoukanshi
    {"\x90\xC2\x96\x82\x93\xB9\x8E\x6D", "\x90\xC2"},                                 // BLU: Ao Madoushi
    {"\x83\x52\x83\x8B\x83\x5A\x83\x41", "\x83\x52"},                                 // COR: Korusea
    {"\x82\xA9\x82\xE7\x82\xAD\x82\xE8\x8E\x6D", "\x82\xA9"},                         // PUP: Karakurishi
    {"\x97\x78\x82\xE8\x8E\x71", "\x97\x78"},                                         // DNC: Odoriko
    {"\x8A\x77\x8E\xD2", "\x8A\x77"},                                                 // SCH: Gakusha
    {"\x95\x97\x90\x85\x8E\x6D", "\x95\x97"},                                         // GEO: Fuusuishi
    {"\x83\x8B\x81\x5B\x83\x93\x83\x74\x83\x46\x83\x93\x83\x54\x81\x5B", "\x83\x8B"}, // RUN: Ruun Fensaa
    {"\x83\x82\x83\x93\x83\x58\x83\x5E\x81\x5B", "\x83\x82\x83\x93"},                 // MON: Monsutaa
};
static_assert(std::size(kJapaneseJobs) == std::size(kJobNames));
constexpr const char* kStatNames[] = {"STR", "DEX", "VIT", "AGI", "INT", "MND", "CHR"};

using PersonaDraw = void(__thiscall*)(void* self);
using Resource = char**(__cdecl*)(int index);
using Player = char*(__cdecl*)(int index);
using DrawLimited = int(__cdecl*)(int width, int x, int y, const char* text, std::uint32_t colour);
using Sync = const std::uint8_t*(__cdecl*)();
using Message = const char*(__cdecl*)(int table, int id);
using DrawString = int(__cdecl*)(int x, int y, const char* text, std::uint32_t colour);
using DrawScaled = int(__cdecl*)(int x, int y, const char* text, std::uint32_t colour, float sx, float sy, int right);
using DrawReverse = int(__cdecl*)(int right, int top, const char* text, std::uint32_t colour, int flag, float sx, float sy);
using Shape = void(__thiscall*)(void* shape, int x, int y, std::uint32_t colour, int, int);
using ShapeScaled = void(__thiscall*)(void* shape, int x, int y, float sx, float sy, std::uint32_t colour, int centred,
                                      int blending, int skip);
using DrawQuad = void(__thiscall*)(void* quad, int x, int y, float sx, float sy, std::uint32_t colour, int blending);
using Font = char**(__cdecl*)(int index);
using Number = int(__cdecl*)();
using Warning = std::uint32_t(__thiscall*)(void* self, int which);
using Resize = void(__thiscall*)(void* ctrl, int x, int y, int width, int height, int keep, int, int);
using States = void(__thiscall*)(void* renderer);
using Bind = std::uint8_t(__thiscall*)(void* renderer);
using NextCode = int(__cdecl*)(const char** text);
using PrintCode = void(__thiscall*)(void* renderer, int code, int* pos, float sx, float sy);
using Advance = std::uint8_t(__cdecl*)(int code, int* pos);

// status.jobs: "Level 75 SAM", "75 SAM / 37 WHM" on one row, "Lv 75 SAM", "Lv 75 Samurai" (the game's pictures of the
// jobs' names) or the game's own "Lv75 Samurai"
enum class Jobs { Full, Simple, Short, Long, Game };

// [client] status*
bool g_on = false;
Jobs g_jobs_style = Jobs::Full;
bool g_item_level = true; // .itemlevel: the ILv / Su row
bool g_taller = true;     // .taller
bool g_colours = true;    // .colors
std::uint32_t g_name_colour = kWhite, g_hp = kWhite, g_mp = kWhite, g_tp = kWhite; // the game's vertex colours
bool g_separator = true;
std::uint32_t g_separator_colour = kWhite;
bool g_exp_level = true;       // .exp.level: the Level / EXP window's "Level 75" row
bool g_exp_short_next = false; // .exp.shortnext: "NEXT" for "NEXTLevel"

bool g_tried = false;
PersonaDraw g_draw = nullptr;       // the game's, through the trampoline
PersonaDraw g_level_draw = nullptr; // the Level / EXP window's, through its trampoline
const std::uint8_t* g_master_flags = nullptr;
DrawString g_level_string = nullptr; // what the Level / EXP window's "Level 75" and "NEXTLevel" calls went to
Message g_level_message = nullptr;
Resource g_resource = nullptr;
Player g_player = nullptr;
DrawLimited g_limited = nullptr;
const std::uint8_t* g_flags = nullptr;
const std::uint8_t* g_jobs = nullptr;
Sync g_sync = nullptr;
std::uint32_t g_sync_colour = 0x802060A0;
Message g_message = nullptr;
DrawString g_string = nullptr;
DrawScaled g_scaled = nullptr;
DrawReverse g_reverse = nullptr;
Shape g_shape = nullptr;
ShapeScaled g_shape_scaled = nullptr; // null: a job's picture too wide for the window is drawn as it is
DrawQuad g_quad = nullptr;
Font g_font = nullptr;
Number g_item_level_of = nullptr, g_superior = nullptr;
Warning g_warning = nullptr;
const std::uint32_t* g_max_hp = nullptr;
const std::uint32_t* g_max_mp = nullptr;
const std::uint16_t* g_stats = nullptr;
const std::int16_t* g_bonuses = nullptr;
Resize g_resize = nullptr;     // null: the window keeps the game's size
char* const* g_windows = nullptr; // the windows' list's first node
char** g_renderer = nullptr; // the menu font's (null: the big name is drawn small)
States g_states = nullptr;
Bind g_bind = nullptr;
NextCode g_next_code = nullptr;
PrintCode g_print = nullptr;
Advance g_advance = nullptr;
bool g_language_searched = false;
char* const* g_settings = nullptr; // the game's settings' global (null: not found)
char* g_persona = nullptr;         // the window the game is drawing itself (status off), while it does

// The label letters VanaCore moved out of the window, with their corners' places in it (the shorts at +C8h), to give back
struct Hidden {
    char* quad;
    short y[4];
};
std::vector<Hidden> g_hidden;
// How far down they go: out of the window and off the screen. (See-through, as they were, True Font still drew their outline
// and the game's shadow copy under them: dark letters over the window, at the Titles font's size.)
constexpr int kAwayDown = 0x1000;

// The window made taller: its control and the game's height for it; and each window under it moved down, where it was
// put (a window the game has put back is moved again)
struct Moved {
    char* ctrl;
    short y; // where VanaCore put it
};
char* g_grown = nullptr;
short g_grown_from = 0;
int g_extra = 0; // how much taller VanaCore has made it
std::vector<Moved> g_moved;
// The Level / EXP window made shorter by its "Level 75" row: its control and the game's height for it
char* g_exp_ctrl = nullptr;
short g_exp_from = 0;
bool g_exp_shorter = false;

// The separator's quad: font 0's dash, copied each time it's drawn
alignas(16) char g_line[kQuadSize];

std::uintptr_t call_target(std::uintptr_t call)
{
    std::int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + rel;
}

bool is_call(std::uintptr_t at) { return *reinterpret_cast<const std::uint8_t*>(at) == 0xE8; }

template <typename T> T operand(std::uintptr_t at)
{
    T value{};
    std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof value);
    return value;
}

template <typename T> T& field(char* base, std::ptrdiff_t offset) { return *reinterpret_cast<T*>(base + offset); }

// 0xRRGGBB as the game's vertex colour: each channel halved (drawn doubled, so 0x80 is full), alpha 0x80 unless given
std::uint32_t game_colour(std::uint32_t rgb, std::uint32_t alpha = 0x80)
{
    const auto channel = [](unsigned c) { return static_cast<std::uint32_t>((c * 128 + 127) / 255); };
    return alpha << 24 | channel((rgb >> 16) & 0xFF) << 16 | channel((rgb >> 8) & 0xFF) << 8 | channel(rgb & 0xFF);
}

int text(int x, int y, const char* line, std::uint32_t colour) { return static_cast<short>(g_string(x, y, line, colour)); }

void text_right(int right, int y, const char* line, std::uint32_t colour) { g_reverse(right, y, line, colour, 1, 1.0f, 1.0f); }

int width(const char* line) { return text(kAway, kAway, line, kWhite) - kAway; }

// Every label letter in the window's picture, `fn(quad)` on each
template <typename Fn> void each_letter(char* self, Fn fn)
{
    char* ctrl = field<char*>(self, kCtrlAt);
    if (ctrl == nullptr)
        return;
    int elements = 0;
    for (char* node = field<char*>(ctrl, kElements); node != nullptr && elements < 16; node = field<char*>(node, 0), ++elements) {
        char* element = field<char*>(node, kNodeItem);
        if (element == nullptr || field<short>(element, 0) != 0)
            continue;
        char* shape = field<char*>(element, 8);
        if (shape == nullptr)
            continue;
        const int count = field<int>(shape, kPartCount);
        int parts = 0;
        for (char* part = field<char*>(shape, kParts); part != nullptr && parts < count && parts < 128;
             part = field<char*>(part, 0), ++parts) {
            char* quad = field<char*>(part, kNodeItem);
            if (quad != nullptr && std::memcmp(quad + kQuadName, kLetterTexture, sizeof kLetterTexture) == 0)
                fn(quad);
        }
    }
}

short& corner_y(char* quad, int corner) { return field<short>(quad, kLocalY + corner * 2); }

// The entry of a letter VanaCore has moved away and that's still where it put it (null: the game's place, or one the game
// has made anew)
Hidden* hidden_entry(char* quad)
{
    for (Hidden& hidden : g_hidden)
        if (hidden.quad == quad && corner_y(quad, 0) == hidden.y[0] + kAwayDown)
            return &hidden;
    return nullptr;
}

// The window's own labels moved out of it (their places kept, for when status is turned off); again if the game has made
// them anew
void hide_labels(char* self)
{
    each_letter(self, [](char* quad) {
        if (hidden_entry(quad) != nullptr)
            return;
        Hidden hidden{quad, {}};
        for (int i = 0; i < 4; ++i) {
            hidden.y[i] = corner_y(quad, i);
            corner_y(quad, i) = static_cast<short>(hidden.y[i] + kAwayDown);
        }
        for (Hidden& old : g_hidden) {
            if (old.quad == quad) {
                old = hidden;
                return;
            }
        }
        g_hidden.push_back(hidden);
    });
}

void show_labels(char* self)
{
    if (g_hidden.empty())
        return;
    each_letter(self, [](char* quad) {
        if (const Hidden* hidden = hidden_entry(quad); hidden != nullptr)
            for (int i = 0; i < 4; ++i)
                corner_y(quad, i) = hidden->y[i];
    });
    g_hidden.clear();
}

// Where a control is and how big, from its rect
struct Place {
    short x, y, w, h;
};
Place place_of(const char* ctrl)
{
    const auto* rect = reinterpret_cast<const short*>(ctrl + kCtrlRectAt);
    return {rect[0], rect[1], static_cast<short>(rect[2] - rect[0]), static_cast<short>(rect[3] - rect[1])};
}

// A control moved down by `dy` (its pictures with it) and its window's own rect with it
void move_window(char* ctrl, int dy)
{
    const Place place = place_of(ctrl);
    g_resize(ctrl, place.x, place.y + dy, place.w, place.h, 1, 0, 0);
    if (char* window = field<char*>(ctrl, kWindowAt); window != nullptr && field<short>(window, kRectAt + 2) == place.y) {
        field<short>(window, kRectAt + 2) = static_cast<short>(place.y + dy);
        field<short>(window, kRectAt + 6) = static_cast<short>(field<short>(window, kRectAt + 6) + dy);
    }
}

// The Level / EXP window ("menu    level   "): hudwin.cpp keeps it under the Status window itself, wherever and however
// tall that is, or where its element's X and Y say (expwin.*), so it's never moved here (looked for under the game's
// bottom only, it went down twice, or not at all when the game made it before the Status window)
bool level_window(char* ctrl)
{
    const char* layout = field<char*>(ctrl, kLayoutAt);
    return layout != nullptr && std::memcmp(layout + kLayoutNameAt, kLevelName, 16) == 0;
}

// The windows under the Status window, in its column, one after the other (the Gil window...; not the Level/EXP window,
// hudwin.cpp's): `fn(ctrl)` on each whose top is just under `bottom`, the next looked for under it
template <typename Fn> void each_window_under(char* ctrl, int bottom, Fn fn)
{
    if (g_windows == nullptr)
        return;
    const int x = place_of(ctrl).x;
    for (int column = 0; column < 4; ++column) {
        char* found = nullptr;
        int nodes = 0;
        for (char* node = *g_windows; node != nullptr && nodes < 256; node = field<char*>(node, 0), ++nodes) {
            char* other = field<char*>(node, kNodeItem);
            if (other == nullptr || other == ctrl || level_window(other))
                continue;
            const Place place = place_of(other);
            if (std::abs(place.x - x) <= 4 && place.y >= bottom - 4 && place.y < bottom + 8 && place.h > 0) {
                found = other;
                break;
            }
        }
        if (found == nullptr)
            return;
        bottom = place_of(found).y + place_of(found).h;
        fn(found);
    }
}

bool was_moved(char* ctrl)
{
    for (const Moved& moved : g_moved)
        if (moved.ctrl == ctrl && place_of(ctrl).y == moved.y)
            return true;
    return false;
}

// status.taller: the window `height` tall if that's taller than the game's, the windows under it moved down by as
// much; 0 puts them back
void fit_window(char* self, int height)
{
    char* ctrl = field<char*>(self, kCtrlAt);
    if (g_resize == nullptr || ctrl == nullptr)
        return;
    Place place = place_of(ctrl);
    if (g_grown != ctrl) { // first, or the game's made the window anew
        if (height == 0)
            return;
        g_grown = ctrl;
        g_grown_from = place.h;
        g_extra = 0;
        g_moved.clear();
    } else if (place.h != g_grown_from + g_extra) { // the game's sized it again
        g_grown_from = place.h;
        g_extra = 0;
    }
    const int extra = height > g_grown_from ? height - g_grown_from : 0;
    if (g_extra != extra) {
        if (g_extra != 0) { // the windows VanaCore moved (and the game hasn't) back up first
            each_window_under(ctrl, place.y + g_grown_from + g_extra, [](char* other) {
                if (was_moved(other))
                    move_window(other, -g_extra);
            });
            g_moved.clear();
        }
        g_resize(ctrl, place.x, place.y, place.w, g_grown_from + extra, 1, 0, 0);
        place = place_of(ctrl);
        field<short>(self, kRectAt + 6) = static_cast<short>(place.y + place.h);
        g_extra = extra;
    }
    if (g_extra == 0)
        return;
    // the windows under where its bottom was, each time: the game may put one back
    each_window_under(ctrl, place.y + g_grown_from, [](char* other) {
        if (was_moved(other))
            return;
        move_window(other, g_extra);
        for (Moved& moved : g_moved) {
            if (moved.ctrl == other) {
                moved.y = place_of(other).y;
                return;
            }
        }
        g_moved.push_back({other, place_of(other).y});
    });
}

// A flat line `length` long from `x`, `y`, its ends fading out: font 0's dash's quad, its every corner on the white
// middle of its stroke, drawn as two halves
void draw_line(int x, int y, int length, std::uint32_t colour)
{
    char** font = g_font != nullptr ? g_font(0) : nullptr;
    char** glyphs = font != nullptr ? reinterpret_cast<char**>(*font) : nullptr;
    char* dash = glyphs != nullptr ? glyphs['-' - 0x20] : nullptr;
    char* node = dash != nullptr ? field<char*>(dash, kParts) : nullptr;
    char* source = node != nullptr ? field<char*>(node, kNodeItem) : nullptr;
    if (source == nullptr || g_quad == nullptr)
        return;
    std::memcpy(g_line, source, kQuadSize);
    const float u0 = field<float>(g_line, kCorners + kU), u1 = field<float>(g_line, kCorners + kCorner + kU);
    const float v0 = field<float>(g_line, kCorners + kV), v1 = field<float>(g_line, kCorners + 2 * kCorner + kV);
    if (u1 <= u0 || v1 <= v0)
        return;
    const float u = u0 + (u1 - u0) * kWhiteTexel[0] / kDashTexels;
    const float v = v0 + (v1 - v0) * kWhiteTexel[1] / kDashTexels;
    const int half = length / 2;
    for (int part = 0; part < 2; ++part) {
        const short xs[4] = {0, static_cast<short>(half), 0, static_cast<short>(half)};
        const short ys[4] = {0, 0, 1, 1};
        for (int i = 0; i < 4; ++i) {
            field<short>(g_line, kLocalX + i * 2) = xs[i];
            field<short>(g_line, kLocalY + i * 2) = ys[i];
            char* corner = g_line + kCorners + i * kCorner;
            field<float>(corner, kU) = u;
            field<float>(corner, kV) = v;
            const bool right = (i & 1) != 0;
            const bool end = part == 0 ? !right : right; // the line's ends see-through
            field<std::uint32_t>(corner, kColour) = end ? colour & 0x00FFFFFF : colour;
        }
        g_quad(g_line, x + part * half, y, 1.0f, 1.0f, kWhite, 0);
    }
}

bool menu_font() { return g_renderer != nullptr && *g_renderer != nullptr && g_print != nullptr; }

// A line in the menu font (Shift-JIS, its kana and kanji too): its letters' codes and its width at full size
struct MenuLine {
    int codes[32];
    int count;
    int width;
};
MenuLine menu_line(const char* line)
{
    MenuLine out{};
    for (const char* at = line; *at != '\0' && out.count < 32;) {
        const int code = static_cast<short>(g_next_code(&at));
        int step[2] = {};
        g_advance(code, step);
        out.codes[out.count++] = code;
        out.width += step[0];
    }
    return out;
}

// `line` from `x`, `y` (its cells' top), `sx` wide and `sy` tall, in `colour`. False if the font couldn't be bound.
bool print_line(const MenuLine& line, int x, int y, float sx, float sy, std::uint32_t colour)
{
    void* renderer = *g_renderer;
    g_states(renderer);
    if (g_bind(renderer) == 0)
        return false;
    auto* corners = static_cast<char*>(renderer);
    for (const std::ptrdiff_t corner : kCornerColours)
        field<std::uint32_t>(corners, corner) = colour;
    float left = static_cast<float>(x);
    for (int i = 0; i < line.count; ++i) {
        int pos[2] = {static_cast<int>(std::lround(left)), y};
        g_print(renderer, line.codes[i], pos, sx, sy);
        int step[2] = {};
        g_advance(line.codes[i], step);
        left += step[0] * sx;
    }
    for (const std::ptrdiff_t corner : kCornerColours)
        field<std::uint32_t>(corners, corner) = kWhite;
    return true;
}

// The name in the menu font, made smaller to fit the window if it must. False if it can't be (the font isn't there or
// the name would be too small).
bool draw_big_name(const char* name, int x, int y, int room)
{
    if (!menu_font())
        return false;
    const MenuLine line = menu_line(name);
    if (line.count == 0)
        return true;
    const float scale = line.width > room ? static_cast<float>(room) / line.width : 1.0f;
    if (scale < 0.6f)
        return false;
    // a smaller name sits lower, its letters' bottoms where the full size's are
    y += static_cast<int>(std::lround(kMenuCell * (1.0f - scale) * 0.75f));
    return print_line(line, x, y, scale, scale, g_name_colour);
}

#include "statuswindow_name.h" // status.name.font: the name whole in its own True Font

// The game's text in Japanese (its own setting, whichever way it was set: the launcher's [language] playonline=1 or
// PlayOnline's)
bool japanese()
{
    const char* settings = g_settings != nullptr ? *g_settings : nullptr;
    return settings != nullptr && *reinterpret_cast<const int*>(settings + kLanguageAt) == 0;
}

// A job's name in Japanese from `x` on the font 0 row at `y`, made narrower if it would pass `right`. False if it isn't
// drawn (the game's in English, or the menu font isn't there): the caller draws its own.
bool draw_japanese_job(int job, int x, int y, int right, std::uint32_t colour)
{
    if (job <= 0 || job >= kJobs || !japanese() || !menu_font())
        return false;
    const MenuLine line = menu_line(kJapaneseJobs[job].name);
    float sx = kJobScale;
    if (line.width > 0 && line.width * kJobScale > right - x) {
        sx = static_cast<float>(right - x) / line.width;
        sx = sx > kJobScale * 0.6f ? sx : kJobScale * 0.6f;
    }
    return print_line(line, x, y - kJobRise, sx, kJobScale, colour);
}

const char* job_name(int job) { return job > 0 && job < kJobs ? kJobNames[job] : "?"; }

// The game's picture of a job's name ("Samurai", menu resource 1), or null
void* job_picture(int job)
{
    if (g_resource == nullptr || job <= 0 || job >= kPictured)
        return nullptr;
    char** resource = g_resource(1);
    char* table = resource != nullptr ? *resource : nullptr;
    return table != nullptr ? field<void*>(table, job * 4 + kJobPictures) : nullptr;
}

// The job whose picture `picture` is (0: none)
int job_of(const void* picture)
{
    for (int job = 1; job < kPictured; ++job)
        if (job_picture(job) == picture)
            return job;
    return 0;
}

// A job's picture at `x`, `y`, made narrower if it would pass `right` (its left and right edges at +20h, +22h)
void draw_picture(void* picture, int x, int y, int right, std::uint32_t colour)
{
    const auto* edges = reinterpret_cast<const short*>(static_cast<char*>(picture) + 0x20);
    const int wide = edges[1] - edges[0];
    if (g_shape_scaled != nullptr && wide > 0 && x + wide > right) {
        const float sx = static_cast<float>(right - x) / wide;
        g_shape_scaled(picture, x, y, sx > 0.6f ? sx : 0.6f, 1.0f, colour, 0, 0, 0);
        return;
    }
    g_shape(picture, x, y, colour, 0, 0);
}

// "75 SAM / 37 WHM" on one row (the short Japanese names in a game in Japanese), made narrower if it would pass `right`
void draw_jobs_simple(int x, int y, int right, std::uint32_t main_colour)
{
    const bool in_japanese = japanese() && menu_font();
    char parts[5][16] = {};
    std::uint32_t colours[5] = {main_colour, kWhite, kWhite, kWhite, kWhite};
    MenuLine names[5] = {}; // a part that's a job's Japanese name, in the menu font
    int count = 0;
    for (int i = 0; i < 2; ++i) {
        const int job = g_jobs[i * 2], level = g_jobs[i * 2 + 1];
        if (job == 0)
            continue;
        if (count != 0)
            std::snprintf(parts[count++], sizeof parts[0], " / ");
        colours[count] = i == 0 ? main_colour : kWhite;
        std::snprintf(parts[count++], sizeof parts[0], "%d ", level);
        if (in_japanese && job < kJobs)
            names[count] = menu_line(kJapaneseJobs[job].abbr);
        std::snprintf(parts[count++], sizeof parts[0], "%s", job_name(job));
        if (count > 3)
            break;
    }
    float widths[5] = {};
    float full = 0.0f;
    for (int i = 0; i < count; ++i) {
        widths[i] = names[i].count != 0 ? names[i].width * kJobScale : static_cast<float>(width(parts[i]));
        full += widths[i];
    }
    const float scale = full > right - x && full > 0.0f ? (right - x) / full : 1.0f;
    for (int i = 0; i < count; ++i) {
        if (names[i].count != 0) {
            print_line(names[i], x, y - kJobRise, kJobScale * scale, kJobScale, colours[i]);
            x += static_cast<int>(std::lround(widths[i] * scale));
        } else {
            x = static_cast<short>(g_scaled(x, y, parts[i], colours[i], scale, 1.0f, 0));
        }
    }
}

// The jobs' rows from `y`, `step` apart (one row for Simple), the main job's level in `main_colour` (the game's level
// sync blue): "Level 75 SAM", "Lv 75 SAM", "Lv 75" and the game's picture of the job's name, or the game's own "Lv75"
// and picture; in a game in Japanese, the job's Japanese name in place of the picture or "SAM" (the game's own window
// doesn't shorten them). The levels are right-aligned so one and two digits line up.
void draw_jobs(int left, int right, int y, int step, std::uint32_t main_colour)
{
    if (g_jobs_style == Jobs::Simple) {
        draw_jobs_simple(left + kInset, y, right - 4, main_colour);
        return;
    }
    const bool game = g_jobs_style == Jobs::Game && g_message != nullptr;
    const char* label = g_jobs_style == Jobs::Full ? "Level" : "Lv";
    const int level_right = kInset + width(label) + 4 + width("99");
    char number[16];
    for (int i = 0; i < 2; ++i, y += step) {
        const int job = g_jobs[i * 2], level = g_jobs[i * 2 + 1];
        if (job == 0)
            continue;
        const std::uint32_t colour = i == 0 ? main_colour : kWhite;
        int x = left + level_right + 4;
        if (game) {
            std::snprintf(number, sizeof number, g_message(0xA, 0x59), level);
            x = text(left + kInset, y, number, colour) + 2;
        } else {
            text(left + kInset, y, label, kWhite);
            std::snprintf(number, sizeof number, "%d", level);
            text_right(left + level_right, y, number, colour);
        }
        if (draw_japanese_job(job, x, y, right - 4, game ? colour : kWhite))
            continue;
        const bool named = game || g_jobs_style == Jobs::Long; // the name, not "SAM"
        void* picture = named ? job_picture(job) : nullptr;
        if (picture != nullptr)
            draw_picture(picture, x, y, right - 4, game ? colour : kWhite);
        else
            text(x, y, job_name(job), kWhite);
    }
}

// HP and MP: the label, "1302/" and the maximum (red with the game's mark when it's lowered)
void draw_gauge(char* self, int left, int y, const char* label, int value, int maximum, int warning, std::uint32_t colour)
{
    text(left + kInset, y, label, colour);
    const std::uint32_t max_colour = g_warning(self, warning);
    if (max_colour != kWhite)
        g_shape(field<void*>(self, kMarkAt), left + kMarkX, y + 1, kWhite, 0, 0);
    char number[24];
    std::snprintf(number, sizeof number, "%4d", value);
    const int slash = kSlashRight - width("/");
    text_right(left + slash, y, number, colour);
    text(left + slash, y, "/", kWhite);
    std::snprintf(number, sizeof number, "%d", maximum);
    text(left + kMaxX, y, number, max_colour);
}

// The Level / EXP window kLevelRow shorter (`shorter`), or the game's height again
void fit_level(char* self, bool shorter)
{
    char* ctrl = field<char*>(self, kCtrlAt);
    if (g_resize == nullptr || ctrl == nullptr)
        return;
    Place place = place_of(ctrl);
    if (g_exp_ctrl != ctrl) { // first, or the game's made the window anew
        if (!shorter)
            return;
        g_exp_ctrl = ctrl;
        g_exp_from = place.h;
        g_exp_shorter = false;
    } else if (place.h != g_exp_from - (g_exp_shorter ? kLevelRow : 0)) { // the game's sized it again
        g_exp_from = place.h;
        g_exp_shorter = false;
    }
    if (shorter == g_exp_shorter)
        return;
    g_resize(ctrl, place.x, place.y, place.w, g_exp_from - (shorter ? kLevelRow : 0), 1, 0, 0);
    place = place_of(ctrl);
    field<short>(self, kRectAt + 6) = static_cast<short>(place.y + place.h);
    g_exp_shorter = shorter;
}

// status.exp.level=0: everything the window draws a row higher, the "Level 75" row not drawn (level_line), the window
// a row shorter
void __fastcall level_hook(char* self, void*)
{
    const bool master = static_cast<std::uint8_t>(self[kMasterAt]) == 0 && (*g_master_flags & 1) != 0;
    const bool hide = !g_exp_level && !master;
    fit_level(self, hide);
    if (!hide) {
        g_level_draw(self);
        return;
    }
    short& top = field<short>(self, kRectAt + 2);
    top = static_cast<short>(top - kLevelRow);
    g_level_draw(self);
    top = static_cast<short>(top + kLevelRow);
}

int __cdecl level_line(int x, int y, const char* line, std::uint32_t colour)
{
    return g_exp_level ? g_level_string(x, y, line, colour) : x;
}

// "NEXTLevel" less its "Level" (status.exp.shortnext)
const char* __cdecl next_label(int table, int id)
{
    const char* label = g_level_message(table, id);
    if (!g_exp_short_next || label == nullptr)
        return label;
    static char shorter[64];
    const std::size_t length = std::strlen(label);
    if (length < 6 || length >= sizeof shorter || std::strcmp(label + length - 5, "Level") != 0)
        return label;
    std::size_t end = length - 5;
    while (end > 0 && label[end - 1] == ' ')
        --end;
    std::memcpy(shorter, label, end);
    shorter[end] = '\0';
    return shorter;
}

// A call instruction's target changed to `to`
bool redirect_call(std::uintptr_t call, const void* to)
{
    DWORD protect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(call), 5, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(to) - (call + 5));
    std::memcpy(reinterpret_cast<void*>(call + 1), &rel, 4);
    VirtualProtect(reinterpret_cast<void*>(call), 5, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    return true;
}

// The game's own drawing of a job's picture in its Status window (status off; both calls redirected here, __thiscall):
// in a game in Japanese, the job's Japanese name in its place
void __fastcall job_shape(void* picture, void*, int x, int y, std::uint32_t colour, int centred, int blending)
{
    const int job = japanese() ? job_of(picture) : 0;
    const int right = g_persona != nullptr ? field<short>(g_persona, kRectAt + 4) - 4 : x + 0x60;
    if (job == 0 || !draw_japanese_job(job, x, y, right, colour))
        g_shape(picture, x, y, colour, centred, blending);
}

void __fastcall draw_hook(char* self, void*)
{
    if (!g_on) {
        show_labels(self);
        fit_window(self, 0);
        g_persona = self;
        g_draw(self);
        g_persona = nullptr;
        return;
    }
    char* player = g_player(0);
    if (player == nullptr)
        return;
    hide_labels(self);
    const Rows rows = layout(g_taller && g_resize != nullptr, g_jobs_style == Jobs::Simple ? 1 : 2, g_item_level);
    fit_window(self, rows.height);
    const auto* rect = reinterpret_cast<const short*>(self + kRectAt);
    const int left = rect[0], top = rect[1], right = rect[2];
    // the name in its own True Font (drawn in VanaCore's pass), else the menu font's, else the game's small one
    const char* name = player + kNameAt;
    const int room = right - left - kInset * 2;
    if (!keep_name(name, left + kInset, top + rows.name, room) && !draw_big_name(name, left + kInset, top + rows.name, room))
        g_limited(right - left - 6, left + kInset, top + 7, name, g_name_colour);
    if ((*g_flags & 1) == 0)
        return;

    // the jobs, the main job's level in the game's level sync colour under it
    const std::uint8_t* sync = g_sync();
    const std::uint32_t main_colour = sync != nullptr && (sync[3] & 0x40) != 0 ? g_sync_colour : kWhite;
    draw_jobs(left, right, top + rows.jobs, rows.step, main_colour);

    char number[24];
    int y = top + rows.item_level;
    if (rows.item_level != 0) {
        text(left + kInset, y, "ILv", kWhite);
        const int item_level = g_item_level_of();
        if (item_level == 0)
            std::snprintf(number, sizeof number, "---/");
        else
            std::snprintf(number, sizeof number, "%3d/", item_level);
        text_right(left + kItemLevelRight, y, number, kWhite);
        std::snprintf(number, sizeof number, "Su%2d", g_superior());
        text(left + kSuperiorX, y, number, kWhite);
    }

    y = top + rows.gauges;
    draw_gauge(self, left, y, "HP", field<int>(player, kHpAt), static_cast<int>(*g_max_hp), kHpWarning, g_colours ? g_hp : kWhite);
    y += rows.step;
    draw_gauge(self, left, y, "MP", field<int>(player, kMpAt), static_cast<int>(*g_max_mp), kMpWarning, g_colours ? g_mp : kWhite);
    y += rows.step;
    const std::uint32_t tp = g_colours ? g_tp : kWhite;
    text(left + kInset, y, "TP", tp);
    std::snprintf(number, sizeof number, "%4d", field<int>(player, kTpAt));
    text_right(left + kSlashRight - width("/"), y, number, tp);

    // the line, halfway between TP and STR
    if (g_separator)
        draw_line(left + kInset, top + (y - top + kGlyph + rows.stats) / 2, right - left - kInset * 2, g_separator_colour);

    for (int i = 0; i < 7; ++i) {
        y = top + rows.stats + i * rows.step;
        const std::uint32_t colour = g_warning(self, kStatWarning + i);
        if (colour != kWhite)
            g_shape(field<void*>(self, kMarkAt), left + kStatMarkX, y + 1, kWhite, 0, 0);
        text(left + kInset, y, kStatNames[i], kWhite);
        std::snprintf(number, sizeof number, "%4d", g_stats[i]);
        const int end = text(left + kStatX, y, number, colour);
        const int bonus = g_bonuses[i];
        if (bonus > 0) {
            std::snprintf(number, sizeof number, " +%d", bonus);
            text(end, y, number, kBonus);
        } else if (bonus < 0) {
            std::snprintf(number, sizeof number, " %d", bonus);
            text(end, y, number, kLoss);
        }
    }
}

// The menu font's routines (left out if they aren't found: the name is drawn small)
void find_menu_font(HMODULE game)
{
    const std::uintptr_t print = scan::find(game, kPrintString);
    const std::uintptr_t title = scan::find(game, kTitleText);
    if (print == 0 || title == 0 || !is_call(print + kStatesCall) || !is_call(print + kBindCall) ||
        !is_call(print + kCodeCall) || !is_call(print + kPrintCall) || !is_call(print + kAdvanceCall)) {
        log::write(L"Status window: the game's menu font wasn't found in FFXiMain.dll: the name is drawn small while its True Font is off");
        return;
    }
    g_renderer = operand<char**>(title + kRendererAt);
    g_states = reinterpret_cast<States>(call_target(print + kStatesCall));
    g_bind = reinterpret_cast<Bind>(call_target(print + kBindCall));
    g_next_code = reinterpret_cast<NextCode>(call_target(print + kCodeCall));
    g_advance = reinterpret_cast<Advance>(call_target(print + kAdvanceCall));
    g_print = reinterpret_cast<PrintCode>(call_target(print + kPrintCall));
}

// The windows' list and resize, for status.taller (left out if they aren't found: the window keeps the game's size)
void find_windows(HMODULE game)
{
    const std::uintptr_t resize = scan::find(game, kResize);
    const std::uintptr_t windows = scan::find(game, kWindows);
    if (resize == 0 || windows == 0) {
        log::write(L"Status window: the game's window list or resize wasn't found in FFXiMain.dll: status.taller does nothing");
        return;
    }
    g_resize = reinterpret_cast<Resize>(resize);
    g_windows = operand<char* const*>(windows + kWindowsAt);
}

// The Level / EXP window's drawing hooked, its "Level 75" row's and "NEXTLevel"'s calls redirected
void install_level(HMODULE game)
{
    const std::uintptr_t at = scan::find(game, kLevelDraw);
    const auto* code = reinterpret_cast<const std::uint8_t*>(at);
    if (at == 0 || code[kMasterFlagsAt - 1] != 0xA0 || !is_call(at + kLevelLineCall) || !is_call(at + kNextLabelCall) ||
        code[kNextLabelCall - 4] != 0x6A || code[kNextLabelCall - 3] != 0x58) {
        log::write(L"Status window: the game's drawing of the Level / EXP window wasn't found in FFXiMain.dll: status.exp.* do nothing");
        return;
    }
    g_master_flags = operand<const std::uint8_t*>(at + kMasterFlagsAt);
    g_level_string = reinterpret_cast<DrawString>(call_target(at + kLevelLineCall));
    g_level_message = reinterpret_cast<Message>(call_target(at + kNextLabelCall));
    if (!imports::detour(reinterpret_cast<PersonaDraw>(at), kLevelPrologue, reinterpret_cast<PersonaDraw>(&level_hook),
                         g_level_draw) ||
        !redirect_call(at + kLevelLineCall, reinterpret_cast<const void*>(&level_line)) ||
        !redirect_call(at + kNextLabelCall, reinterpret_cast<const void*>(&next_label))) {
        log::write(L"Status window: the game's drawing of the Level / EXP window couldn't be hooked: status.exp.* do nothing");
        return;
    }
    log::write(L"Status window: the Level / EXP window's drawing hooked at +0x%X",
               static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
}

void install_persona(HMODULE game)
{
    const std::uintptr_t at = scan::find(game, kPersonaDraw);
    const auto* code = reinterpret_cast<const std::uint8_t*>(at);
    if (at == 0 || !is_call(at + kResourceCall) || !is_call(at + kPlayerCall) || !is_call(at + kLimitedCall) ||
        code[kFlagsAt - 1] != 0xA0 || code[kJobsAt - 1] != 0xA0 || !is_call(at + kSyncCall) ||
        code[kSyncColourAt - 1] != 0xBF || !is_call(at + kMessageCall) || !is_call(at + kStringCall) ||
        !is_call(at + kShapeCall) || !is_call(at + kItemLevelCall) || !is_call(at + kSuperiorCall) ||
        !is_call(at + kReverseCall) || !is_call(at + kWarningCall) || code[kMaxHpAt - 1] != 0xA1 ||
        code[kMaxMpAt - 1] != 0xA1 || code[kStatsAt - 1] != 0x6D || code[kBonusesAt - 1] != 0x6D) {
        log::write(L"Status window: the game's drawing of it wasn't found in FFXiMain.dll: [client] status does nothing");
        return;
    }
    const std::uintptr_t string = call_target(at + kStringCall);
    const std::uintptr_t shape = call_target(at + kShapeCall);
    if (!is_call(string + kScaledCall) || !is_call(call_target(string + kScaledCall) + kFontCall)) {
        log::write(L"Status window: the game's text drawing wasn't found in FFXiMain.dll: [client] status does nothing");
        return;
    }
    g_resource = reinterpret_cast<Resource>(call_target(at + kResourceCall));
    g_player = reinterpret_cast<Player>(call_target(at + kPlayerCall));
    g_limited = reinterpret_cast<DrawLimited>(call_target(at + kLimitedCall));
    g_flags = operand<const std::uint8_t*>(at + kFlagsAt);
    g_jobs = operand<const std::uint8_t*>(at + kJobsAt);
    g_sync = reinterpret_cast<Sync>(call_target(at + kSyncCall));
    g_sync_colour = operand<std::uint32_t>(at + kSyncColourAt);
    g_message = reinterpret_cast<Message>(call_target(at + kMessageCall));
    g_string = reinterpret_cast<DrawString>(string);
    g_scaled = reinterpret_cast<DrawScaled>(call_target(string + kScaledCall));
    g_font = reinterpret_cast<Font>(call_target(call_target(string + kScaledCall) + kFontCall));
    g_shape = reinterpret_cast<Shape>(shape);
    if (is_call(shape + kQuadCall))
        g_quad = reinterpret_cast<DrawQuad>(call_target(shape + kQuadCall));
    else
        log::write(L"Status window: the game's quad drawing wasn't found in FFXiMain.dll: status.separator draws no line");
    g_item_level_of = reinterpret_cast<Number>(call_target(at + kItemLevelCall));
    g_superior = reinterpret_cast<Number>(call_target(at + kSuperiorCall));
    g_reverse = reinterpret_cast<DrawReverse>(call_target(at + kReverseCall));
    g_warning = reinterpret_cast<Warning>(call_target(at + kWarningCall));
    g_max_hp = operand<const std::uint32_t*>(at + kMaxHpAt);
    g_max_mp = operand<const std::uint32_t*>(at + kMaxMpAt);
    g_stats = operand<const std::uint16_t*>(at + kStatsAt);
    g_bonuses = operand<const std::int16_t*>(at + kBonusesAt);
    find_menu_font(game);
    if (const std::uintptr_t scaled = scan::find(game, kShapeScaled); scaled != 0)
        g_shape_scaled = reinterpret_cast<ShapeScaled>(scaled);
    else
        log::write(L"Status window: the game's scaled picture drawing wasn't found in FFXiMain.dll: long job names aren't narrowed");
    if (!imports::detour(reinterpret_cast<PersonaDraw>(at), kPrologue, reinterpret_cast<PersonaDraw>(&draw_hook), g_draw)) {
        log::write(L"Status window: the game's drawing of it couldn't be hooked: [client] status does nothing");
        return;
    }
    log::write(L"Status window: its drawing hooked at +0x%X", static_cast<unsigned>(at - reinterpret_cast<std::uintptr_t>(game)));
    // the game's own pictures of the jobs' names, for a game in Japanese with status off
    const auto picture_call = [&](std::ptrdiff_t call) {
        return is_call(at + call) && call_target(at + call) == shape &&
               std::memcmp(code + call - sizeof kPictureLoad, kPictureLoad, sizeof kPictureLoad) == 0;
    };
    if (!picture_call(kShapeCall) || !picture_call(kSubShapeCall) ||
        !redirect_call(at + kShapeCall, reinterpret_cast<const void*>(&job_shape)) ||
        !redirect_call(at + kSubShapeCall, reinterpret_cast<const void*>(&job_shape)))
        log::write(L"Status window: the game's drawing of the jobs' names wasn't as known: in Japanese, its own window keeps them");
}

// The game's language, looked for once FFXiMain.dll is there
void find_language()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    g_language_searched = true;
    const std::uintptr_t at = scan::find(game, kLanguage);
    if (at == 0) {
        log::write(L"Status window: the game's language wasn't found in FFXiMain.dll: the jobs' names stay in English");
        return;
    }
    g_settings = operand<char* const*>(at + kSettingsAt);
}

void install_hook()
{
    g_tried = true;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return;
    find_windows(game);
    install_persona(game);
    install_level(game);
}

std::uint32_t read_colour(const wchar_t* ini, const wchar_t* key, std::uint32_t fallback, std::uint32_t alpha = 0x80)
{
    wchar_t value[64] = {};
    GetPrivateProfileStringW(L"client", key, L"", value, 64, ini);
    float colour[4] = {static_cast<float>((fallback >> 16) & 0xFF), static_cast<float>((fallback >> 8) & 0xFF),
                       static_cast<float>(fallback & 0xFF), 1.0f};
    text::parse_colour(value, colour);
    return game_colour(static_cast<std::uint32_t>(colour[0]) << 16 | static_cast<std::uint32_t>(colour[1]) << 8 |
                           static_cast<std::uint32_t>(colour[2]),
                       alpha);
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"status", 0, ini) != 0;
    wchar_t value[64] = {};
    GetPrivateProfileStringW(L"client", L"status.jobs", L"full", value, 64, ini);
    g_jobs_style = _wcsicmp(value, L"simple") == 0 ? Jobs::Simple
                   : _wcsicmp(value, L"short") == 0  ? Jobs::Short
                   : _wcsicmp(value, L"long") == 0   ? Jobs::Long
                   : _wcsicmp(value, L"game") == 0   ? Jobs::Game
                                                     : Jobs::Full;
    g_item_level = GetPrivateProfileIntW(L"client", L"status.itemlevel", 1, ini) != 0;
    g_taller = GetPrivateProfileIntW(L"client", L"status.taller", 1, ini) != 0;
    g_colours = GetPrivateProfileIntW(L"client", L"status.colors", 1, ini) != 0;
    g_name_colour = read_colour(ini, L"status.name.color", 0xFFFFFF);
    // XIUI's bars' light ends
    g_hp = read_colour(ini, L"status.hp.color", 0xFA9C9C);
    g_mp = read_colour(ini, L"status.mp.color", 0xBFE07D);
    g_tp = read_colour(ini, L"status.tp.color", 0x78C4EE);
    g_separator = GetPrivateProfileIntW(L"client", L"status.separator", 1, ini) != 0;
    g_separator_colour = read_colour(ini, L"status.separator.color", 0xC8D8F8, 0x70);
    g_exp_level = GetPrivateProfileIntW(L"client", L"status.exp.level", 1, ini) != 0;
    g_exp_short_next = GetPrivateProfileIntW(L"client", L"status.exp.shortnext", 0, ini) != 0;
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_tried)
        return;
    if (!g_language_searched)
        find_language();
    // a game in Japanese: hooked for its jobs' names, status on or off
    if (g_on || !g_exp_level || g_exp_short_next || japanese())
        install_hook();
}

void draw(void* device, unsigned width, unsigned height) { draw_name(device, width, height); }

int lowered(const void* window)
{
    for (const Moved& moved : g_moved)
        if (moved.ctrl == window && place_of(moved.ctrl).y == moved.y)
            return g_extra;
    return 0;
}

} // namespace vanacore::statuswindow
