#include "configmenu.hpp"

#include <windows.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "chat.hpp"
#include "gameui.hpp"
#include "log.hpp"
#include "scan.hpp"

namespace vanacore::configmenu {
namespace {

// FFXiMain's menus (2025-12-26) are layouts from the menu DAT (English: ROM\119\51.DAT, chunk type 0x30), registered with
// the menu manager (CTkMenuMng, 0x105ECB98) as the DAT loads: each chunk's payload is parsed into a layout object (0x68
// bytes: +0x46 its name, +8 its parse, whose first is a pointer to the window block in the payload, which the game goes
// on reading from) on the manager's list of layouts at +0x1C. When the DAT is let go, the layout of the chunk's name is
// looked up, taken off the list and freed. A list (the game's own): +0 its first node; a node +0 the next, +0x10 its
// item, +0x14 set to skip. The manager opens a menu by name: the layout of that name, a window made from it (on the
// manager's list of windows at +0, +4 the layout, +0x4C the item the cursor is on) and the handler the game's table of
// menus names ("menu    configwi" at 0x10372F18, +0x20 the global holding its handler). What a layout shows is sprites
// from sprite sheets (chunk type 0x31, on the list at 0x104DFA88), named in it by sheet and number.
//
// Here, as XI on Anything does it (rubymatrix/xi-on-anything, host/modern.c: Config > Advanced), with no change to the
// game's code:
//   - A sprite sheet of VanaCore's own, "menu    vanacore", registered with the game's: the item's label, a button and
//     words in the menu font, made of the game's own textures as its Config labels are.
//   - The Config list's layout rebuilt from the game's own (as it has it in memory, so a DAT the server or pivot gives is
//     the one read) with the item after its last, and registered in place of the game's.
//   - The list's handler pointed at a copy of its table of functions, whose input (select on the item: /vana) and
//     drawing (the item's help line) come here first.
// XI on Anything renames the game's layout out of the way and leaves it on the list. That layout's payload is freed when
// the DAT is let go (logging out, a reload), and the game reads every layout's window block as it takes a sheet away, so
// here the game's layout is taken off the list the way the game does it, and put back from a copy of its payload when
// the setting goes off. Nothing is taken off while a window made from it is open.

// The window that has the help line's text (FUN_101182a0, __thiscall on the window): push ebx; push ebp; push esi;
// mov ebp,ecx; push edi; mov ecx,<the menu manager>; call (the window in front); cmp eax,ebp; jne; mov ax,[ebp+4Ch];
// mov ecx,ebp; push eax; call <the window's item part>; mov esi,eax; test esi,esi; jz; mov al,[esi+4Ah]; test al,al; jz;
// mov edx,[esi+4Ch]. The part's +0x4A says it has a help line, +0x4C its text (else +0x40, the layout's).
constexpr char kHelp[] = "53 55 56 8B E9 57 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C5 0F 85 ?? ?? ?? ?? 66 8B 45 4C 8B CD 50 E8 ?? ?? ?? ?? "
                         "8B F0 85 F6 0F 84 ?? ?? ?? ?? 8A 46 4A 84 C0 0F 84 ?? ?? ?? ?? 8B 56 4C";
constexpr std::ptrdiff_t kHelpManagerAt = 7;
constexpr std::ptrdiff_t kHelpPartCall = 0x1F; // the window's part for an item (FUN_10117dd0, __thiscall, ret 4: item id)
// A layout's payload parsed and put on the manager's list (FUN_1015ccb0, __thiscall on the manager, ret 4: a pointer to a
// pointer to the payload, moved on as it's read; what the DAT's layout chunks are given as they load): push esi; push edi;
// push 68h; mov edi,ecx; call new; ...; call (the layout's parse); push esi; lea ecx,[edi+1Ch]; call (the list's add)
constexpr char kLayoutAdd[] = "56 57 6A 68 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 22 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 15 8B 44 24 0C "
                              "8B CE 50 E8 ?? ?? ?? ?? 56 8D 4F 1C E8 ?? ?? ?? ?? 5F 5E C2 04 00";
// ... and the layout of a payload's name taken off and freed (FUN_1015cd00, the same: as the DAT's chunks are let go):
// mov eax,[esp+4]; push esi; push edi; mov edi,ecx; mov ecx,[eax]; push ecx; mov ecx,edi; call <the layout of a name>;
// mov esi,eax; test esi,esi; jz; push esi; lea ecx,[edi+1Ch]; call (the list's remove); test eax,eax; jz; mov ecx,esi;
// call (its destructor); push esi; call delete
constexpr char kLayoutRemove[] = "8B 44 24 04 56 57 8B F9 8B 08 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 74 1D 56 8D 4F 1C E8 ?? ?? ?? ?? "
                                 "85 C0 74 10 8B CE E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 5F 5E C2 04 00";
constexpr std::ptrdiff_t kRemoveFindCall = 0xD;
// The layout of a name (FUN_1015cd60), which the removal goes by: the first on the list whose name matches: push ecx;
// push ebx; push esi; push edi; lea edi,[ecx+1Ch]; mov ecx,edi; call; ... at +0x2C lea ecx,[esi+46h]; push 10h; push ecx;
// push ebx; call strncmp
constexpr std::uint8_t kFindStart[] = {0x51, 0x53, 0x56, 0x57, 0x8D, 0x79, 0x1C, 0x8B, 0xCF, 0xE8};
constexpr std::ptrdiff_t kFindNameAt = 0x2C;
constexpr std::uint8_t kFindName[] = {0x8D, 0x4E, 0x46, 0x6A, 0x10, 0x51, 0x53, 0xE8};
// A sprite sheet's payload parsed and put on the sheets' list (FUN_1011fe90, as the layouts': ret 4, the pointer to a
// pointer): push esi; push edi; push 40h; mov edi,ecx; call new; ...
constexpr char kSheetAdd[] = "56 57 6A 40 8B F9 E8 ?? ?? ?? ?? 83 C4 04 85 C0 74 21 8B C8 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 44 24 0C "
                             "8B CE 50 E8 ?? ?? ?? ?? 56 8B CF E8 ?? ?? ?? ?? 5F 5E C2 04 00";
// ... and every sheet of a payload's name taken off and freed (FUN_1011fed0), each after the layouts on the manager's
// list (mov ecx,<the manager>+1Ch at +0x3F) have their window's sprites from it forgotten: sub esp,0Ch; mov eax,[esp+10h];
// push esi; push edi; mov edi,ecx; mov ecx,[eax]; mov [esp+10h],edi; push ecx; mov ecx,edi; call (the sheet of a name);
// mov esi,eax; test esi,esi; mov [esp+8],esi; jz
constexpr char kSheetRemove[] = "83 EC 0C 8B 44 24 10 56 57 8B F9 8B 08 89 7C 24 10 51 8B CF E8 ?? ?? ?? ?? 8B F0 85 F6 89 74 24 08 0F 84";
constexpr std::ptrdiff_t kSheetRemoveLayoutsAt = 0x40;
// What the DAT's sheet chunks call as they load and are let go (0x1011db80, 0x1011dba0), side by side: push ecx;
// add ecx,30h (the chunk's payload); lea eax,[esp]; mov [esp],ecx; push eax; mov ecx,<the sheets>; call <add>; pop ecx;
// ret; then the same with <remove>, then the next function (push esi; push edi; mov esi,ecx; call)
constexpr char kSheetThunks[] = "51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 "
                                "51 83 C1 30 8D 44 24 00 89 4C 24 00 50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 59 C3 90 90 90 90 90 90 90 "
                                "56 57 8B F1 E8";
constexpr std::ptrdiff_t kThunkSheetsAt = 14, kThunkAddCall = 18, kThunkNext = 32;
// The Config list's handler's input (FUN_10194ec0, its table's slot 6, __thiscall, ret 8: the event, then the item the
// cursor is on): on select (5) of item 1 to 13 it opens that item's menu by name: cmp word ptr [esp+4],5; jne; movsx
// eax,word ptr [esp+8]; dec eax; cmp eax,0Ch; ja; jmp [eax*4+table]; push 0; push 1; push <name>; mov ecx,<the manager>;
// call <open a menu>
constexpr char kConfigInput[] = "66 83 7C 24 04 05 0F 85 ?? ?? ?? ?? 0F BF 44 24 08 48 83 F8 0C 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ?? "
                                "6A 00 6A 01 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? E8";
constexpr std::ptrdiff_t kInputManagerAt = 44;
// The handler made (in FUN_101a74b0, which makes the menus' handlers): push 14h; call new; mov esi,eax; add esp,4;
// or ebp,-1; cmp esi,edi; mov bl,1; jz; lea ecx,[esi+4]; call; mov [esi+0Eh],bp; mov [esi+8],edi; mov [esi+0Ch],bl;
// mov dword ptr [esi],<its table>; mov [<the global>],esi; jmp; mov [<the global>],edi; push 44h
constexpr char kConfigMake[] = "6A 14 E8 ?? ?? ?? ?? 8B F0 83 C4 04 83 CD FF 3B F7 B3 01 74 20 8D 4E 04 E8 ?? ?? ?? ?? 66 89 6E 0E "
                               "89 7E 08 88 5E 0C C7 06 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? EB 06 89 3D ?? ?? ?? ?? 6A 44";
constexpr std::ptrdiff_t kMakeTableAt = 0x29, kMakeGlobalAt = 0x2F, kMakeGlobalAgainAt = 0x37;
// The game's table of menus: "menu    configwi", 16 zero bytes, the global holding its handler (+0x20)
constexpr char kMenuEntry[] = "6D 65 6E 75 20 20 20 20 63 6F 6E 66 69 67 77 69 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";
constexpr std::ptrdiff_t kEntryGlobal = 0x20;
// The handler's table: 17 slots (the next word isn't code); 0 its destructor, 1 opened (it may make item 13 one that can't
// be chosen: push esi; mov esi,ecx; push 0Dh; push 0Bh; mov ecx,[esi+8]; call), 4 drawn (each frame its window is: mov
// byte ptr [ecx+10h],1; ret), 6 the input above
constexpr int kSlots = 17, kOpenSlot = 1, kDrawSlot = 4, kInputSlot = 6;
constexpr std::uint8_t kOpenStart[] = {0x56, 0x8B, 0xF1, 0x6A, 0x0D, 0x6A, 0x0B, 0x8B, 0x4E, 0x08, 0xE8};
constexpr std::uint8_t kDrawCode[] = {0xC6, 0x41, 0x10, 0x01, 0xC3};

// The manager: its windows (+0) and layouts (+0x1C). A list's node. A layout: its parse, its name. A window: its layout,
// the item the cursor is on. An item's part: its help line. A sheet: its name. The handler: its
// window (0 while it has none: the game clears it as the window goes).
constexpr std::ptrdiff_t kWindows = 0, kLayouts = 0x1C;
constexpr std::ptrdiff_t kNodeNext = 0, kNodeItem = 0x10, kNodeSkip = 0x14;
constexpr std::ptrdiff_t kLayoutParse = 8, kLayoutName = 0x46;
constexpr std::ptrdiff_t kWindowLayout = 4, kWindowCursor = 0x4C;
constexpr std::ptrdiff_t kPartHasHelp = 0x4A, kPartHelp = 0x4C;
constexpr std::ptrdiff_t kSheetName = 0x24;
constexpr std::ptrdiff_t kHandlerWindow = 8;
constexpr int kMaxNodes = 65536;
constexpr int kSelect = 5; // a window's input events: 1 down, 2 up, 3 right, 4 left, 5 select

// A layout's payload: its name (16), 1, the count of blocks after the window's, to +0x20; then blocks, a multiple of 16
// bytes each: +0 their size, +2 x, +4 y, +0xA width, +0xC height; the window's +0x13 the edge it's anchored to, +0x14 its
// count of sprites, +0x15 and +0x16 its strings' lengths; an item's +0x12 its id, +0x15 the ids of the items before and
// after it, up, down, right and left (0xFF none), +0x1B its count of sprites, +0x1D and +0x1E its strings' lengths. At
// +0x20 its sprites, 20 bytes each (a slot, 16 bits; the sprite's number, 16 bits; the sheet's name), then the two
// strings (the help line's and the title's text, as numbers of the game's texts; "-1" none). The parse puts the
// language's sheet in for "menu    comwin" (the frame's), in the payload itself.
constexpr std::size_t kHeader = 0x20, kCountAt = 0x11;
constexpr std::size_t kBlockX = 2, kBlockY = 4, kBlockWidth = 0xA, kBlockHeight = 0xC;
constexpr std::size_t kWindowSprites = 0x14;
constexpr std::size_t kItemId = 0x12, kItemLinks = 0x15, kItemSprites = 0x1B, kItemStrings = 0x1D;
constexpr std::size_t kSprites = 0x20, kSpriteSize = 20, kSpriteNumber = 2, kSpriteSheet = 4;
enum Link { kBefore, kAfter, kUp, kDown, kRight, kLeft };
constexpr std::size_t kOurBlock = 0x40;
constexpr std::size_t kPayloadMax = 0x1000;

// The game's Config list (ROM\119\51.DAT): a window 112 x 218 whose frame is "menu    comwin" sprite 12 (sprite k has k + 1
// wells for buttons, up to 14's 15; 15 on are other things: 16 is the "Commands" title tab), 13 items 88 x 16, 16 apart
// down its left (ids 1 to 13, Gameplay first, the 13th, Gamepad, at the bottom), each a "menu    windowps" sprite, and its
// title tab (id 14, "Config").
constexpr int kGameItems = 13;
constexpr int kItem = kGameItems + 1; // VanaCore's, below the 13th
// After VanaCore's, ids kItem + 1 on: its other items (kBuiltins), then addons' (gameui.hpp: vana.gameui.add('config_menu',
// label, fn)). The frame, a well for each item, is VanaCore's sheet's own (frame_sprite), made as the game makes comwin's:
// comwin's sprite for 17 items was its "Commands" title tab, so with 4 items after the game's the list had no frame (its
// buttons, added onto what's under them, washed out over the scene) and "Commands" over its "Config".
constexpr int kExtraItemsMost = 4; // after VanaCore's: its own others and the addons'
constexpr int kItemWidth = 88, kItemHeight = 16;
constexpr char kConfigName[17] = "menu    configwi";
constexpr char kFrameSheet[] = "menu    comwin"; // the first 14 of its name: the parse ends it for the language

// VanaCore's sheet: its one sprite, the label, as the game's Config labels are made ("menu    windowps" 338, Gameplay):
// the button ("menu    buttonto": 48 of its left from u 16 of row 0, 40 of its right from row 16), then each letter in
// the menu font ("font    font") 2 right and down in a faint shadow, then the letters themselves.
constexpr char kSheet[17] = "menu    vanacore";
constexpr char kButtonTexture[17] = "menu    buttonto";
constexpr char kFontTexture[17] = "font    font    ";
// A part's colours, one a corner (as the game keeps them: 0x7F is the texel's own) and its blending. The button's tint is
// R, G, B, A on a grey texture: the game's blue 60 60 7F 70, its green 60 7F 60, yellow 7F 7F 60, Monstrosity's red
// 7F 38 38 (ROM\119\51.DAT's sheets); VanaCore's own item purple, as strong as that red. An addon's is its own
// (vana.gameui.add's colour, gameui.hpp's kColours), with the same alpha.
constexpr std::uint8_t kButtonColour[4] = {0x60, 0x60, 0x7F, 0x70};
constexpr std::uint8_t kVanaCoreColour[4] = {0x70, 0x38, 0x7F, 0x70};
constexpr std::uint8_t kShadowColour[4] = {0x7F, 0x7F, 0x7F, 0x20};
constexpr std::uint8_t kLetterColour[4] = {0x7F, 0x7F, 0x7F, 0x7F};
constexpr std::uint8_t kItemBlend[4] = {0x01, 0x00, 0x02, 0x01};
constexpr std::uint8_t kShadowBlend[4] = {0x01, 0x02, 0x00, 0x01};
// The frame, as comwin's sprites 0 to 14 are made: the window's background ("menu    newtex", light at the top, dim at the
// bottom), then each well's right and left halves of the button texture, dark, 16 apart from 8 down (singlemenu.cpp's)
constexpr char kBackTexture[17] = "menu    newtex  ";
constexpr std::uint8_t kBack[2][4] = {{0x7F, 0x7F, 0x7F, 0x7F}, {0x40, 0x40, 0x40, 0x40}}; // top, bottom
constexpr std::uint8_t kWellColour[4] = {0x00, 0x00, 0x00, 0x70};
constexpr std::uint8_t kFrameBlend[4] = {0x01, 0x00, 0x01, 0x01};
constexpr int kWellsTop = 8, kWellLeft = 18, kWellMiddle = 66, kWellRight = 106;
// The letters: where each starts across the label and sits below its top, and its place and size in the font's texture.
// The sizes and places are the game's own for these letters (every Config label, and its other menus' words, has them so);
// they're spaced as its labels space them, about a letter's width apart.
struct Letter {
    int x, y;
    int u, v, w, h;
};
constexpr Letter kLabel[] = {
    {5, 1, 52, 52, 10, 11},  // V
    {13, 3, 49, 67, 8, 9},   // a
    {21, 3, 56, 80, 9, 9},   // n
    {30, 3, 49, 67, 8, 9},   // a
    {38, 1, 97, 26, 9, 12},  // C
    {47, 3, 66, 80, 9, 9},   // o
    {56, 3, 98, 80, 7, 9},   // r
    {63, 3, 89, 67, 9, 9},   // e
};
constexpr std::size_t kPartSize = 61;

// An addon's label (MOCKUP), made as VanaCore's is from the menu font's letters, each where it is in the font's texture and
// how far it moves the next one on. The texture holds the letters in ASCII order, rows 13 apart (A-D from u 77 of v 26,
// E-P v 39, Q-Y v 52, a-g v 65, h-s v 78, t-z v 92), a short letter 2 lower in its row than a tall one. The sizes and the
// advances are the game's own, each the most common across every menu word in ROM\119\51.DAT (its sheets' parts on
// "font    font", drawn a texel to a pixel): a label set with them reads as the game's labels do. Z and 0-9 aren't known
// yet: a letter not here is left as a gap.
struct Glyph {
    char c;
    int y, u, v, w, h, advance;
};
constexpr Glyph kGlyphs[] = {
    {'A', 1, 77, 26, 9, 11, 9},   {'B', 1, 87, 26, 9, 11, 9},   {'C', 1, 97, 26, 9, 12, 9},   {'D', 1, 107, 26, 9, 11, 9},
    {'E', 1, 0, 39, 9, 11, 9},    {'F', 1, 10, 39, 9, 11, 8},   {'G', 1, 20, 39, 9, 12, 9},   {'H', 1, 31, 39, 9, 11, 9},
    {'I', 1, 42, 39, 6, 11, 5},   {'J', 1, 48, 39, 9, 12, 9},   {'K', 1, 58, 39, 9, 11, 9},   {'L', 1, 69, 39, 9, 11, 9},
    {'M', 1, 80, 39, 9, 11, 9},   {'N', 1, 91, 39, 9, 11, 9},   {'O', 1, 102, 39, 9, 12, 9},  {'P', 1, 113, 39, 9, 11, 8},
    {'Q', 1, 0, 52, 9, 12, 9},    {'R', 1, 11, 52, 10, 11, 9},  {'S', 1, 22, 52, 9, 12, 9},   {'T', 1, 31, 52, 9, 11, 8},
    {'U', 1, 41, 52, 9, 12, 8},   {'V', 1, 52, 52, 10, 11, 9},  {'W', 1, 63, 52, 9, 11, 9},   {'X', 1, 76, 52, 9, 11, 9},
    {'Y', 1, 88, 52, 9, 11, 9},
    {'a', 3, 49, 67, 8, 9, 8},    {'b', 1, 58, 65, 9, 11, 8},   {'c', 3, 68, 67, 9, 9, 9},    {'d', 1, 78, 65, 9, 11, 9},
    {'e', 3, 89, 67, 9, 9, 9},    {'f', 1, 99, 65, 7, 11, 6},   {'g', 3, 107, 67, 9, 12, 9},  {'h', 1, 0, 78, 9, 11, 9},
    {'i', 1, 10, 78, 5, 11, 5},   {'j', 1, 16, 78, 8, 14, 7},   {'k', 1, 25, 78, 9, 11, 9},   {'l', 1, 35, 78, 6, 11, 6},
    {'m', 3, 42, 80, 9, 9, 9},    {'n', 3, 56, 80, 9, 9, 9},    {'o', 3, 66, 80, 9, 9, 9},    {'p', 3, 76, 80, 9, 12, 9},
    {'q', 3, 87, 80, 9, 12, 9},   {'r', 3, 98, 80, 7, 9, 7},    {'s', 3, 106, 80, 9, 9, 9},   {'t', 2, 0, 92, 6, 10, 6},
    {'u', 3, 7, 93, 9, 9, 9},     {'v', 3, 17, 93, 9, 9, 8},    {'w', 3, 27, 93, 9, 9, 8},    {'x', 3, 39, 93, 9, 9, 9},
    {'y', 3, 50, 93, 9, 12, 8},   {'z', 3, 60, 93, 8, 9, 8},
};
constexpr int kLabelStart = 5, kGap = 6;

constexpr char kHelpText[] = "Open VanaCore's settings.";

// VanaCore's other items, after its own and before the addons': a label of kGlyphs (as an addon's), its help line and the
// command it runs at the next frame, as if typed
struct Builtin {
    const char* label;
    const char* help;
    const char* command;
};
constexpr Builtin kBuiltins[] = {
    // /vana hud manager is /hud's, kept when /hud is switched off for a player's own (Client > Commands)
    {"HUD", "Open or close the HUD Manager: move the game's windows.", "/vana hud manager"},
    {"FPS Graph", "FPS and frame time, the last minute.", "/vana graph"},
    {"Game Stats", "Where each frame's time goes.", "/vana stats"},
};
constexpr int kBuiltinCount = static_cast<int>(std::size(kBuiltins));

using Payload = void(__thiscall*)(void* list, const void** payload); // the adds and removes above
using WindowPart = char*(__thiscall*)(void* window, int item);
using WindowHelp = void(__thiscall*)(void* window);
using Draw = void(__thiscall*)(void* handler);
using Input = void(__thiscall*)(void* handler, int event, int item);

bool g_on = true;        // [client] configmenu
bool g_searched = false; // FFXiMain.dll looked at
bool g_found = false;
bool g_failed = false;   // something wasn't as known: the game's list is left alone from then on
ULONGLONG g_next = 0;    // the next look

char* g_manager = nullptr;
char* g_sheets = nullptr;
char** g_handler = nullptr; // the global holding the Config list's handler
void* g_table = nullptr;    // its table of functions, the game's
Payload g_layout_add = nullptr;
Payload g_layout_remove = nullptr;
Payload g_sheet_add = nullptr;
Payload g_sheet_remove = nullptr;
WindowPart g_window_part = nullptr;
WindowHelp g_window_help = nullptr;
Draw g_draw = nullptr;   // the game's slot 4
Input g_input = nullptr; // and 6

// VanaCore's, in memory that stays: the game keeps pointers into a payload while its layout or sheet is on its list, and
// writes the language's frame into a layout's as it parses it.
alignas(16) std::uint8_t g_layout[kPayloadMax];   // the Config list with the item
alignas(16) std::uint8_t g_original[kPayloadMax]; // the game's, as it was: put back from when the setting goes off
// VanaCore's sheet at its largest: its header (name, 3 images, the count of sprites), a label for VanaCore and each of
// kExtraItemsMost others (letters_of sets a letter at least 3 across from the last, so at most kItemWidth / 3 of them, each
// two parts, with the bar's two), and the frame (the background and two parts a well)
constexpr int kLettersMost = kItemWidth / 3;
constexpr std::size_t kLabelMost = 1 + (2 + 2 * kLettersMost) * kPartSize;
constexpr int kWellsMost = kItem + kExtraItemsMost;
constexpr std::size_t kFrameMost = 1 + (1 + 2 * kWellsMost) * kPartSize;
constexpr std::size_t kSheetMost = 16 + 1 + 3 * 16 + 2 + (1 + kExtraItemsMost) * kLabelMost + kFrameMost;
alignas(16) std::uint8_t g_sheet[kSheetMost]; // VanaCore's label, up to kExtraItemsMost others' and the frame
std::size_t g_sheet_size = 0;
void* g_ours[kSlots] = {}; // the handler's table, with our slots
std::vector<gameui::MenuItem> g_wanted; // the addons' items gameui had when the list was put in
std::vector<gameui::MenuItem> g_items;  // kBuiltins (no key), then the addons' that fit, in the list (ids kItem + 1 on)
std::vector<std::string> g_items_help; // their help lines, kept while they're in (the game keeps a pointer to the text)

bool g_added = false;       // our layout is on the game's list (as of the last look)
char* g_added_at = nullptr; // the layout object the game made of it
bool g_sheet_in = false;    // our sheet is on the game's list

template <typename T> T& at(void* base, std::ptrdiff_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

template <typename T> T operand(std::uintptr_t site, std::ptrdiff_t offset)
{
    return *reinterpret_cast<const T*>(site + offset);
}

std::uintptr_t call_target(std::uintptr_t call)
{
    return call + 5 + *reinterpret_cast<const std::int32_t*>(call + 1);
}

int u16(const std::uint8_t* p)
{
    return p[0] | p[1] << 8;
}

int s16(const std::uint8_t* p)
{
    return static_cast<std::int16_t>(u16(p));
}

void put16(std::uint8_t* p, int value)
{
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
}

// ---- VanaCore's sheet ----

void sheet_put(const void* bytes, std::size_t size)
{
    std::memcpy(g_sheet + g_sheet_size, bytes, size);
    g_sheet_size += size;
}

// A part of a sprite: a quad from x0, y0 to x1, y1 of the image's texels from u, v (as many as it's wide and tall), its top
// corners shade[0] and its bottom ones shade[1]
void sheet_part(int x0, int y0, int x1, int y1, int u, int v, const std::uint8_t (&shade)[2][4], const std::uint8_t (&blend)[4],
                const char* image)
{
    std::uint8_t part[kPartSize] = {};
    const int corners[12] = {x0, y0, x1, y0, x0, y1, x1, y1, x1 - x0, y1 - y0, u, v};
    for (int i = 0; i < 12; ++i)
        put16(part + 2 * i, corners[i]);
    for (int corner = 0; corner < 4; ++corner)
        std::memcpy(part + 25 + 4 * corner, shade[corner < 2 ? 0 : 1], 4);
    std::memcpy(part + 41, blend, 4);
    std::memcpy(part + 45, image, 16);
    sheet_put(part, sizeof part);
}

void sheet_part(int x0, int y0, int x1, int y1, int u, int v, const std::uint8_t (&colour)[4], const std::uint8_t (&blend)[4],
                const char* image)
{
    std::uint8_t shade[2][4];
    std::memcpy(shade[0], colour, 4);
    std::memcpy(shade[1], colour, 4);
    sheet_part(x0, y0, x1, y1, u, v, shade, blend, image);
}

// The frame's sprite: a window `width` x `height` with a well for each of `wells` items, as comwin's sprite k (k + 1 wells)
// is: the background, then each well's right half (40 of the button texture's right from row 16) and left half (48 of its
// left from u 16 of row 0)
void sheet_frame(int width, int height, int wells)
{
    const auto parts = static_cast<std::uint8_t>(1 + 2 * wells);
    sheet_put(&parts, 1);
    sheet_part(0, 0, width, height, 0, 0, kBack, kFrameBlend, kBackTexture);
    for (int i = 0; i < wells; ++i) {
        const int y = kWellsTop + kItemHeight * i;
        sheet_part(kWellMiddle, y, kWellRight, y + kItemHeight, 0, 16, kWellColour, kFrameBlend, kButtonTexture);
        sheet_part(kWellLeft, y, kWellMiddle, y + kItemHeight, 16, 0, kWellColour, kFrameBlend, kButtonTexture);
    }
}

// One label's sprite: a count of parts, the button in `colour`, then each letter 2 right and down in a faint shadow, then
// the letters
void sheet_label(const Letter* letters, std::size_t count, const std::uint8_t (&colour)[4] = kButtonColour)
{
    const auto parts = static_cast<std::uint8_t>(2 + 2 * count);
    sheet_put(&parts, 1);
    sheet_part(0, 0, 48, kItemHeight, 16, 0, colour, kItemBlend, kButtonTexture);
    sheet_part(48, 0, kItemWidth, kItemHeight, 0, 16, colour, kItemBlend, kButtonTexture);
    for (int pass = 0; pass < 2; ++pass) {
        const int shift = pass == 0 ? 2 : 0;
        for (std::size_t i = 0; i < count; ++i) {
            const Letter& l = letters[i];
            sheet_part(l.x + shift, l.y + shift, l.x + shift + l.w, l.y + shift + l.h, l.u, l.v, pass == 0 ? kShadowColour : kLetterColour,
                       pass == 0 ? kShadowBlend : kItemBlend, kFontTexture);
        }
    }
}

// A label as letters (kGlyphs), left to right from where VanaCore's starts, as the game spaces its words; one too long for
// the item a pixel closer each, then from 2 further left, then 2 closer. Past that, cut off.
std::vector<Letter> letters_of(const std::string& label)
{
    constexpr int kFits[][2] = {{kLabelStart, 0}, {kLabelStart, 1}, {kLabelStart - 2, 1}, {kLabelStart - 2, 2}}; // start, closer
    std::vector<Letter> out;
    for (const auto& fit : kFits) {
        out.clear();
        bool whole = true;
        int x = fit[0];
        for (const char c : label) {
            const Glyph* glyph = nullptr;
            for (const Glyph& g : kGlyphs)
                glyph = g.c == c ? &g : glyph;
            if (glyph == nullptr) { // a space, or a letter not known yet
                x += kGap - fit[1];
                continue;
            }
            if (x + glyph->w + 2 > kItemWidth) {
                whole = false;
                break;
            }
            out.push_back({x, glyph->y, glyph->u, glyph->v, glyph->w, glyph->h});
            x += glyph->advance - fit[1];
        }
        if (whole)
            break;
    }
    return out;
}

// The sheet's payload, as a DAT's sheet chunk has it: its name, its images (a count, then their names), its sprites (a
// count, 16 bits; each a count of parts, then the parts, 61 bytes each: its corners, size and place in the image as
// 16 bits each, a byte, the corners' colours, the blending, the image's name). Sprite 0 VanaCore's label, then the
// others' (g_items) in order, then the list's frame (frame_sprite), `width` x `height` with a well for every item.
int frame_sprite()
{
    return 1 + static_cast<int>(g_items.size());
}

void make_sheet(int width, int height)
{
    g_sheet_size = 0;
    sheet_put(kSheet, 16);
    const std::uint8_t images = 3;
    sheet_put(&images, 1);
    sheet_put(kButtonTexture, 16);
    sheet_put(kFontTexture, 16);
    sheet_put(kBackTexture, 16);
    const std::uint8_t sprites[2] = {static_cast<std::uint8_t>(frame_sprite() + 1), 0};
    sheet_put(sprites, 2);
    sheet_label(kLabel, std::size(kLabel), kVanaCoreColour);
    for (const gameui::MenuItem& item : g_items) {
        const std::vector<Letter> letters = letters_of(item.label);
        const std::uint8_t colour[4] = {item.tint[0], item.tint[1], item.tint[2], kButtonColour[3]}; // (vana.gameui.add's)
        sheet_label(letters.data(), letters.size(), colour);
    }
    sheet_frame(width, height, kItem + static_cast<int>(g_items.size()));
}

// ---- the game's lists ----

// The first item on a list of the game's (at `list` in `owner`) whose 16-character name (at `name_at` in the item) is
// `name`, as the game's own lookups go: nodes set to skip passed over
char* find(std::ptrdiff_t list, std::ptrdiff_t name_at, const char* name, char* owner)
{
    int count = 0;
    for (char* node = at<char*>(owner, list); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* item = at<std::uint8_t>(node, kNodeSkip) == 0 ? at<char*>(node, kNodeItem) : nullptr;
        if (item != nullptr && std::memcmp(item + name_at, name, 16) == 0)
            return item;
    }
    return nullptr;
}

char* find_sheet(const char* name)
{
    return find(0, kSheetName, name, g_sheets);
}

// The payload a layout was parsed from (its window block's place, less the header)
const std::uint8_t* payload_of(char* layout)
{
    char* parse = at<char*>(layout, kLayoutParse);
    const auto* block = parse != nullptr ? at<const std::uint8_t*>(parse, 0) : nullptr;
    return block != nullptr ? block - kHeader : nullptr;
}

// The Config list's layouts: the first of the name (the one the game opens, and the one taken off by name), and ours
struct Look {
    char* first = nullptr;
    char* ours = nullptr;
    int named = 0; // how many there are of the name
};

Look look()
{
    Look found;
    int count = 0;
    for (char* node = at<char*>(g_manager, kLayouts); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* layout = at<std::uint8_t>(node, kNodeSkip) == 0 ? at<char*>(node, kNodeItem) : nullptr;
        if (layout == nullptr || std::memcmp(layout + kLayoutName, kConfigName, 16) != 0)
            continue;
        if (found.first == nullptr)
            found.first = layout;
        ++found.named;
        if (payload_of(layout) == g_layout)
            found.ours = layout;
    }
    return found;
}

// A window (open, or closing) made from the layout: every node, skipped or not
bool in_use(const char* layout)
{
    int count = 0;
    for (char* node = at<char*>(g_manager, kWindows); node != nullptr && count < kMaxNodes; node = at<char*>(node, kNodeNext), ++count) {
        char* window = at<char*>(node, kNodeItem);
        if (window != nullptr && at<char*>(window, kWindowLayout) == layout)
            return true;
    }
    return false;
}

void call(Payload function, void* list, const void* payload)
{
    function(list, &payload);
}

void add_layout(const std::uint8_t* payload)
{
    call(g_layout_add, g_manager, payload);
}

// The first layout called "menu    configwi" taken off the list and freed, as the game does it
void remove_layout()
{
    call(g_layout_remove, g_manager, kConfigName);
}

bool add_sheet()
{
    if (find_sheet(kSheet) == nullptr)
        call(g_sheet_add, g_sheets, g_sheet);
    g_sheet_in = find_sheet(kSheet) != nullptr;
    return g_sheet_in;
}

void remove_sheet()
{
    call(g_sheet_remove, g_sheets, kSheet);
    g_sheet_in = false;
}

void fail(const wchar_t* what)
{
    log::write(L"Config menu: the game's Config list isn't the one VanaCore knows (%ls): it's left as it is, without VanaCore's item",
               what);
    g_failed = true;
}

// Not now, but maybe at a later look (the menus half loaded, a second list of the name while the game swaps them): said once
// until it goes through, and tried again
bool g_waiting_said = false;
void wait_for(const wchar_t* what)
{
    if (!g_waiting_said)
        log::write(L"Config menu: waiting to change the game's Config list (%ls): tried again", what);
    g_waiting_said = true;
}

// ---- the Config list ----

// The game's list read: where each block starts (0 the window's, 1 to 13 the items', 14 the title tab's), its size, and
// the 13th's place. nullptr if it's the one known, else what isn't.
struct Shape {
    std::size_t block[kGameItems + 2] = {};
    std::size_t size = 0;
    int bottom_y = 0;
};

const wchar_t* read_list(const std::uint8_t* p, std::size_t room, Shape& shape)
{
    if (room < kHeader || std::memcmp(p, kConfigName, 16) != 0 || p[0x10] != 1 || p[kCountAt] != kGameItems + 1)
        return L"its header";
    std::size_t offset = kHeader;
    for (int b = 0; b < kGameItems + 2; ++b) {
        const std::size_t size = offset + 2 <= room ? static_cast<std::size_t>(u16(p + offset)) : 0;
        if (size < 0x20 || size % 16 != 0 || offset + size > room)
            return L"its blocks";
        shape.block[b] = offset;
        offset += size;
    }
    shape.size = offset;
    if (shape.size + kOurBlock > kPayloadMax)
        return L"its size";
    // the window: its frame the frame sheet's sprite 12
    const std::uint8_t* window = p + shape.block[0];
    if (window[kWindowSprites] < 1 || u16(window + kSprites) != 0 || s16(window + kSprites + kSpriteNumber) != kGameItems - 1 ||
        std::memcmp(window + kSprites + kSpriteSheet, kFrameSheet, sizeof kFrameSheet - 1) != 0)
        return L"its frame";
    // the items in order, each 88 x 16 and under the first, the 13th at the bottom, Down from it the first and Up from the
    // first it: the list goes round
    const int x = s16(p + shape.block[1] + kBlockX);
    int bottom = 0;
    shape.bottom_y = INT_MIN;
    for (int id = 1; id <= kGameItems; ++id) {
        const std::uint8_t* item = p + shape.block[id];
        if (u16(item + kItemId) != id || s16(item + kBlockX) != x || s16(item + kBlockWidth) != kItemWidth ||
            s16(item + kBlockHeight) != kItemHeight || item[kItemSprites] < 1)
            return L"its items";
        if (s16(item + kBlockY) > shape.bottom_y)
            bottom = id, shape.bottom_y = s16(item + kBlockY);
    }
    const std::uint8_t* last = p + shape.block[kGameItems];
    const std::uint8_t* first = p + shape.block[1];
    if (bottom != kGameItems || last[kItemLinks + kAfter] != 1 || last[kItemLinks + kDown] != 1 ||
        first[kItemLinks + kBefore] != kGameItems || first[kItemLinks + kUp] != kGameItems)
        return L"its order";
    // the title tab: no way to it
    const std::uint8_t* title = p + shape.block[kGameItems + 1];
    if (u16(title + kItemId) != kGameItems + 1)
        return L"its title";
    for (int link = kBefore; link <= kLeft; ++link)
        if (title[kItemLinks + link] != 0xFF)
            return L"its title";
    return nullptr;
}

// The list with VanaCore's item after the 13th, and `extra` addons' items after it (as XI on Anything's config_layout):
// every block as it is, the window a button taller for each and its frame VanaCore's sheet's (frame_sprite, made to the
// window's size: comwin has none past 15 wells), the items linked in a row between the 13th and the first, the title tab
// renumbered after the last. Its size.
std::size_t make_list(const std::uint8_t* game, const Shape& shape, std::uint8_t* out, int extra)
{
    const int ours = 1 + extra;
    const int last_id = kItem + extra;
    std::memset(out, 0, kPayloadMax);
    std::memcpy(out, game, kHeader);
    out[kCountAt] = static_cast<std::uint8_t>(kGameItems + 1 + ours);
    const std::size_t title = shape.block[kGameItems + 1];
    std::memcpy(out + kHeader, game + kHeader, title - kHeader);
    std::uint8_t* window = out + shape.block[0];
    put16(window + kBlockHeight, s16(window + kBlockHeight) + ours * kItemHeight);
    put16(window + kSprites + kSpriteNumber, ours); // frame_sprite: after VanaCore's label and the extra items'
    std::memcpy(window + kSprites + kSpriteSheet, kSheet, 16);
    std::uint8_t* last = out + shape.block[kGameItems];
    last[kItemLinks + kAfter] = last[kItemLinks + kDown] = kItem;
    std::uint8_t* first = out + shape.block[1];
    first[kItemLinks + kBefore] = first[kItemLinks + kUp] = static_cast<std::uint8_t>(last_id);
    // the items: as the game's are, each its sprite of VanaCore's sheet (0 VanaCore's, then the addons'), no help line or
    // title from the game's texts
    for (int i = 0; i < ours; ++i) {
        const int id = kItem + i;
        std::uint8_t* item = out + title + i * kOurBlock;
        put16(item, static_cast<int>(kOurBlock));
        put16(item + kBlockX, s16(game + shape.block[kGameItems] + kBlockX));
        put16(item + kBlockY, shape.bottom_y + (i + 1) * kItemHeight);
        put16(item + kBlockWidth, kItemWidth);
        put16(item + kBlockHeight, kItemHeight);
        put16(item + kItemId, id);
        const auto before = static_cast<std::uint8_t>(i == 0 ? kGameItems : id - 1);
        const auto after = static_cast<std::uint8_t>(id == last_id ? 1 : id + 1);
        const std::uint8_t links[6] = {before, after, before, after, static_cast<std::uint8_t>(id), static_cast<std::uint8_t>(id)};
        std::memcpy(item + kItemLinks, links, sizeof links);
        item[kItemSprites] = 1;
        item[kItemStrings] = 2;
        item[kItemStrings + 1] = 2;
        put16(item + kSprites + kSpriteNumber, i);
        std::memcpy(item + kSprites + kSpriteSheet, kSheet, 16);
        std::memcpy(item + kSprites + kSpriteSize, "-1\0-1", 6);
    }
    // the title tab after them
    const std::size_t title_size = shape.size - title;
    std::memcpy(out + title + ours * kOurBlock, game + title, title_size);
    put16(out + title + ours * kOurBlock + kItemId, last_id + 1);
    return shape.size + ours * kOurBlock;
}

// The item put in: the game's list read from its layout (the first of the name), the sheet in, the game's layout taken off
// and ours put on. Not while the list is open.
void add_item(const Look& found)
{
    char* game = found.first;
    if (game == nullptr || in_use(game)) // the menus aren't loaded yet, or the list is open: at a later look
        return;
    if (found.named != 1) { // the removal goes by name: with two, the game's might not be the one taken off (at a later look)
        wait_for(L"more than one list of its name");
        return;
    }
    const std::uint8_t* from = payload_of(game);
    if (from == nullptr)
        return;
    // How long the payload is: its DAT chunk's length, from the chunk's header just before it (the game keeps the chunk
    // whole: its header at +0x20, the payload at +0x30), or the copy VanaCore put back
    std::size_t room = sizeof g_original;
    if (from != g_original) {
        const std::uint32_t info = *reinterpret_cast<const std::uint32_t*>(from - 0xC);
        const std::size_t length = ((info >> 7) & 0x7FFFF) * 16;
        if ((info & 0x7F) != 0x30 || length <= 16 || length - 16 > sizeof g_original) {
            fail(L"its DAT chunk");
            return;
        }
        room = length - 16;
    }
    Shape shape;
    if (const wchar_t* odd = read_list(from, room, shape)) {
        fail(odd);
        return;
    }
    // the game's frame's sheet listed: the textures VanaCore's frame is made of (its background's, newtex) are loaded
    char name[16];
    std::memcpy(name, from + shape.block[0] + kSprites + kSpriteSheet, 16);
    if (find_sheet(name) == nullptr) { // not yet: at a later look
        wait_for(L"its frame's sheet");
        return;
    }
    // VanaCore's other items, then the addons' (gameui): as many as kExtraItemsMost and the payload have room for, the
    // addons' left out first
    g_wanted = gameui::config_items();
    g_items.clear();
    for (const Builtin& builtin : kBuiltins)
        g_items.push_back({"", "VanaCore", builtin.label});
    g_items.insert(g_items.end(), g_wanted.begin(), g_wanted.end());
    while (!g_items.empty() &&
           (static_cast<int>(g_items.size()) > kExtraItemsMost || shape.size + (1 + g_items.size()) * kOurBlock > kPayloadMax))
        g_items.pop_back();
    const int addons_in = static_cast<int>(g_items.size()) > kBuiltinCount ? static_cast<int>(g_items.size()) - kBuiltinCount : 0;
    if (addons_in < static_cast<int>(g_wanted.size()))
        log::write(L"Config menu: %d of the addons' %d items fit the game's Config list; the rest are left out", addons_in,
                   static_cast<int>(g_wanted.size()));
    g_items_help.clear();
    for (std::size_t i = 0; i < g_items.size(); ++i)
        g_items_help.push_back(static_cast<int>(i) < kBuiltinCount ? std::string(kBuiltins[i].help)
                                                                   : "From the " + g_items[i].display + " addon.");
    if (from != g_original)
        std::memcpy(g_original, from, shape.size);
    make_list(g_original, shape, g_layout, static_cast<int>(g_items.size()));
    // made again each time: the labels and the frame change with g_items
    const std::uint8_t* window = g_layout + shape.block[0];
    make_sheet(s16(window + kBlockWidth), s16(window + kBlockHeight));
    if (!add_sheet()) {
        fail(L"VanaCore's sheet wasn't taken");
        return;
    }
    remove_layout(); // the game's: the first of the name
    Look now = look();
    if (now.first == nullptr)
        add_layout(g_layout);
    now = look();
    if (now.ours == nullptr || now.first != now.ours) {
        // not as it should be (another list of the name, or ours not taken): the game's back, from its copy
        if (look().first == nullptr)
            add_layout(g_original);
        if (look().ours == nullptr)
            remove_sheet();
        fail(L"its layout wasn't replaced");
        return;
    }
    g_added = true;
    g_added_at = now.ours;
    g_waiting_said = false;
    log::write(L"Config menu: VanaCore added to the game's Config list ([client] configmenu), with its %d others and %d addons' items",
               static_cast<int>(g_items.size()) - addons_in, addons_in);
}

// The item taken out ([client] configmenu=0, or the addons' items changed: put in again with them at the next look): ours
// off the list, the game's back from its copy, the sheet off. Not while the list is open.
void take_out(const Look& found, const wchar_t* why)
{
    if (in_use(found.ours))
        return;
    if (found.first != found.ours) { // the removal would take the other first: at a later look
        wait_for(L"another list of its name");
        return;
    }
    remove_layout();
    if (look().first == nullptr)
        add_layout(g_original);
    remove_sheet();
    g_added = false;
    g_added_at = nullptr;
    log::write(L"Config menu: VanaCore taken out of the game's Config list (%ls)", why);
}

// The handler on our table or the game's
void set_table(bool ours)
{
    char* handler = *g_handler;
    if (handler == nullptr)
        return;
    void*& table = at<void*>(handler, 0);
    if (ours && table == g_table)
        table = g_ours;
    else if (!ours && table == static_cast<void*>(g_ours))
        table = g_table;
}

// ---- the handler's slots ----

// Drawn, each frame its window is: as the game draws it, then the item's help line, which the game shows as the cursor
// comes to an item (the window's help: the part's own text, when it has one). Set while the cursor is on it already (the
// list just opened there), the line is made again now.
// The handler's window is one made from VanaCore's list (not the game's, open as the two are swapped: item 14 there is its
// title tab)
bool ours_open(char* window)
{
    char* layout = window != nullptr ? at<char*>(window, kWindowLayout) : nullptr;
    return layout != nullptr && payload_of(layout) == g_layout;
}

void __fastcall config_draw(char* handler, void*)
{
    g_draw(handler);
    char* window = at<char*>(handler, kHandlerWindow);
    if (!ours_open(window))
        return;
    // VanaCore's item's line, and each addon's item's (which addon it's from: the player sees whose button it is)
    for (int i = 0; i <= static_cast<int>(g_items.size()); ++i) {
        const char* text = i == 0 ? kHelpText : g_items_help[i - 1].c_str();
        char* part = g_window_part(window, kItem + i);
        if (part == nullptr || (at<std::uint8_t>(part, kPartHasHelp) == 1 && at<const char*>(part, kPartHelp) == text))
            continue;
        at<std::uint8_t>(part, kPartHasHelp) = 1;
        at<const char*>(part, kPartHelp) = text;
        if (at<short>(window, kWindowCursor) == kItem + i)
            g_window_help(window);
    }
}

// An event on the list (the window's, after its own: the cursor moved, the select sound played): select on VanaCore's item
// opens the /vana window (/vana open: opened, never closed as a bare /vana toggles it), on the next frame as if typed; the
// rest as the game does it.
void __fastcall config_input(char* handler, void*, int event, int item)
{
    const short chosen = static_cast<short>(item);
    if (static_cast<short>(event) == kSelect && chosen >= kItem && ours_open(at<char*>(handler, kHandlerWindow))) {
        if (chosen == kItem) {
            log::write(L"Config menu: VanaCore chosen: /vana open");
            chat::run(1, "/vana open");
            return;
        }
        // VanaCore's others (kBuiltins): their command at the next frame; an addon's (gameui): its function runs then
        const int index = chosen - kItem - 1;
        if (index < kBuiltinCount && index < static_cast<int>(g_items.size())) {
            log::write(L"Config menu: %hs chosen: %hs", kBuiltins[index].label, kBuiltins[index].command);
            chat::run(1, kBuiltins[index].command);
            return;
        }
        if (index < static_cast<int>(g_items.size())) {
            gameui::config_chosen(g_items[index].key);
            return;
        }
    }
    g_input(handler, event, item);
}

// ---- finding it all ----

bool in_image(std::uintptr_t address, std::uintptr_t base, std::size_t size)
{
    return address >= base && address < base + size;
}

bool found()
{
    if (g_searched)
        return g_found;
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return false;
    g_searched = true;
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t help = scan::find(game, kHelp);
    const std::uintptr_t layout_add = scan::find(game, kLayoutAdd);
    const std::uintptr_t layout_remove = scan::find(game, kLayoutRemove);
    const std::uintptr_t sheet_add = scan::find(game, kSheetAdd);
    const std::uintptr_t sheet_remove = scan::find(game, kSheetRemove);
    const std::uintptr_t thunks = scan::find(game, kSheetThunks);
    const std::uintptr_t input = scan::find(game, kConfigInput);
    const std::uintptr_t make = scan::find(game, kConfigMake);
    const std::uintptr_t entry = scan::find(base, size, kMenuEntry);
    const wchar_t* missing = help == 0            ? L"the help line"
                             : layout_add == 0    ? L"the layouts' add"
                             : layout_remove == 0 ? L"the layouts' removal"
                             : sheet_add == 0     ? L"the sheets' add"
                             : sheet_remove == 0  ? L"the sheets' removal"
                             : thunks == 0        ? L"the sheets' list"
                             : input == 0         ? L"the Config list's input"
                             : make == 0          ? L"the Config list's handler"
                             : entry == 0         ? L"the Config list's menu"
                                                  : nullptr;
    // the operands: one menu manager, its layouts, one sheets' list, one handler
    if (missing == nullptr) {
        const auto manager = operand<std::uintptr_t>(help, kHelpManagerAt);
        const auto sheets = operand<std::uintptr_t>(thunks, kThunkSheetsAt);
        const auto global = operand<std::uintptr_t>(make, kMakeGlobalAt);
        const auto table = operand<std::uintptr_t>(make, kMakeTableAt);
        const std::uintptr_t layout_find = call_target(layout_remove + kRemoveFindCall);
        if (!in_image(manager, base, size) || operand<std::uintptr_t>(input, kInputManagerAt) != manager ||
            operand<std::uintptr_t>(sheet_remove, kSheetRemoveLayoutsAt) != manager + static_cast<std::uintptr_t>(kLayouts))
            missing = L"the menu manager";
        else if (!in_image(layout_find, base, size) ||
                 std::memcmp(reinterpret_cast<const void*>(layout_find), kFindStart, sizeof kFindStart) != 0 ||
                 std::memcmp(reinterpret_cast<const void*>(layout_find + kFindNameAt), kFindName, sizeof kFindName) != 0)
            missing = L"the layout of a name";
        else if (!in_image(sheets, base, size) || operand<std::uintptr_t>(thunks, kThunkNext + kThunkSheetsAt) != sheets ||
                 call_target(thunks + kThunkAddCall) != sheet_add || call_target(thunks + kThunkNext + kThunkAddCall) != sheet_remove)
            missing = L"the sheets' list";
        else if (!in_image(global, base, size) || operand<std::uintptr_t>(make, kMakeGlobalAgainAt) != global ||
                 operand<std::uintptr_t>(entry, kEntryGlobal) != global || !in_image(table, base, size))
            missing = L"the Config list's handler";
        if (missing == nullptr) {
            void* const* slots = reinterpret_cast<void* const*>(table);
            const auto open = reinterpret_cast<std::uintptr_t>(slots[kOpenSlot]);
            const auto draw = reinterpret_cast<std::uintptr_t>(slots[kDrawSlot]);
            if (reinterpret_cast<std::uintptr_t>(slots[kInputSlot]) != input || !in_image(open, base, size) ||
                !in_image(draw, base, size) || std::memcmp(reinterpret_cast<const void*>(open), kOpenStart, sizeof kOpenStart) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(draw), kDrawCode, sizeof kDrawCode) != 0)
                missing = L"the Config list's handler's table";
        }
        if (missing == nullptr) {
            g_manager = reinterpret_cast<char*>(manager);
            g_sheets = reinterpret_cast<char*>(sheets);
            g_handler = reinterpret_cast<char**>(global);
            g_table = reinterpret_cast<void*>(table);
        }
    }
    if (missing != nullptr) {
        log::write(L"Config menu: %ls isn't as VanaCore knows it in FFXiMain.dll: the game's Config list is left as it is", missing);
        return false;
    }
    g_layout_add = reinterpret_cast<Payload>(layout_add);
    g_layout_remove = reinterpret_cast<Payload>(layout_remove);
    g_sheet_add = reinterpret_cast<Payload>(sheet_add);
    g_sheet_remove = reinterpret_cast<Payload>(sheet_remove);
    g_window_part = reinterpret_cast<WindowPart>(call_target(help + kHelpPartCall));
    g_window_help = reinterpret_cast<WindowHelp>(help);
    void* const* slots = static_cast<void* const*>(g_table);
    g_draw = reinterpret_cast<Draw>(slots[kDrawSlot]);
    g_input = reinterpret_cast<Input>(slots[kInputSlot]);
    for (int slot = 0; slot < kSlots; ++slot)
        g_ours[slot] = slots[slot];
    g_ours[kDrawSlot] = reinterpret_cast<void*>(&config_draw);
    g_ours[kInputSlot] = reinterpret_cast<void*>(&config_input);
    g_found = true;
    log::write(L"Config menu: the game's menus found in FFXiMain.dll (manager +0x%X, Config list's handler +0x%X)",
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_manager) - base),
               static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(g_handler) - base));
    return true;
}

// The addons' items gameui has now aren't the ones the list was put in with (MOCKUP: an addon's button added or taken
// out, the addon gone)
bool items_changed()
{
    const std::vector<gameui::MenuItem> now = gameui::config_items();
    if (now.size() != g_wanted.size())
        return true;
    for (std::size_t i = 0; i < now.size(); ++i)
        if (now[i].key != g_wanted[i].key || now[i].label != g_wanted[i].label || now[i].tint != g_wanted[i].tint)
            return true;
    return false;
}

} // namespace

void reload(const wchar_t* ini)
{
    g_on = GetPrivateProfileIntW(L"client", L"configmenu", 1, ini) != 0; // taken up by on_frame
}

void install(const wchar_t* ini)
{
    reload(ini);
}

void on_frame()
{
    if (g_failed || (!g_on && !g_added && !g_sheet_in))
        return;
    // every frame, one pointer: a handler the game made again (logging in) has VanaCore's slots at once, not half a second on
    if (g_found)
        set_table(g_on && g_added);
    const ULONGLONG now = GetTickCount64();
    if (now < g_next)
        return;
    g_next = now + 500;
    if (!found()) {
        g_failed = g_searched;
        return;
    }
    const Look lists = look();
    if (g_added && lists.ours == nullptr) {
        // the game let its menus go (logging out, a reload), ours with them: put in again when they're back
        g_added = false;
        log::write(L"Config menu: the game's menus were let go; VanaCore's item goes back in when they're loaded");
    }
    // the sheet goes with the layout (its sprites keep the game's textures, which go with the menus); not while a window
    // made from our layout is still up
    if (lists.ours == nullptr && g_sheet_in && (g_added_at == nullptr || !in_use(g_added_at))) {
        remove_sheet();
        g_added_at = nullptr;
    }
    if (g_on && lists.ours == nullptr && !g_sheet_in)
        add_item(lists);
    else if (!g_on && lists.ours != nullptr)
        take_out(lists, L"[client] configmenu=0");
    else if (g_on && lists.ours != nullptr && items_changed())
        take_out(lists, L"the addons' items changed: put in again with them"); // waits while the list is open
    set_table(!g_failed && g_on && g_added);
}

} // namespace vanacore::configmenu
